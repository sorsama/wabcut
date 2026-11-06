#pragma once

// CaptureBridge: File > Capture… (0.7, card "Capture video from tape, capture card or camera", docs/capture.md), the QML
// singleton `Capture`. Premiere's Capture panel adapted: pick a video device and one of its formats (picture size, frame
// rate, pixel layout), a sound input and how the picture is written (a preset), watch the live picture, press Record and Stop;
// the finished file is imported into the bin the user chose as one undo step "Capture".
//
//   * Devices and privacy. Listing the devices (refresh()) opens none. Picking a video device reads its formats (the system
//     opens it for a moment). The device is opened - the camera light comes on - only by startPreview() and record(), which
//     QML calls on the user's own action; the top bar shows a recording indicator while any is open (`deviceOpen`).
//     Recording is not a command: no agent, script or MCP client can start it. When the system refuses access (Windows:
//     Settings > Privacy & security > Camera / Microphone) the error says how to allow it and `accessKind` names the page
//     openPrivacySettings() opens.
//   * Off the GUI thread. Listing, reading formats, opening, starting and finishing run on a worker thread; the picture is
//     converted for display by the image provider (image://wabcut-capture/<serial>) on the loader's threads; the GUI thread
//     only polls the session's counters at about 30 Hz.
//   * Where files go: <project>.wabcut-media/Capture next to the saved project, else Documents/Wabcut/Capture. Each capture
//     is "<clip name>.mkv" (Matroska: it survives the program stopping) with a journal next to it while it is written.
//   * Stopping. Stop finishes the file and imports it (ProjectFile.importFiles into the chosen bin; one undo step). A device
//     that disappears, or a drive that is nearly full, stops the capture the same way and says why. Closing the project or
//     quitting finishes the file and keeps its journal, so the next start offers to import it (`interrupted`); a capture the
//     program could not finish (a crash, a power cut) is offered too, repaired from what reached the disk.
//   * Windows only in this version (Media Foundation); other systems show `unsupportedReason`.

#include <wabcut/capture/CaptureSession.h>

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class ProjectController;
class ProjectLifecycle;

/// The live picture, shared with the image provider (which asks from the loader's threads).
struct CaptureShared {
    std::mutex mutex;
    std::shared_ptr<capture::CaptureSession> session;
};

class CaptureBridge : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Capture)
    QML_SINGLETON

    /// False on a system without a capture source (everything but Windows in this version): the panel shows
    /// `unsupportedReason` instead of the controls.
    Q_PROPERTY(bool supported READ supported NOTIFY devicesChanged FINAL)
    Q_PROPERTY(QString unsupportedReason READ unsupportedReason NOTIFY devicesChanged FINAL)
    /// [{ id, name }] (refresh()); no device is open.
    Q_PROPERTY(QVariantList videoDevices READ videoDevices NOTIFY devicesChanged FINAL)
    /// [{ id, name, isDefault }]
    Q_PROPERTY(QVariantList audioDevices READ audioDevices NOTIFY devicesChanged FINAL)
    /// The picked video device's formats, best first: [{ index, label, width, height, fps, native, compressed, interlaced }].
    Q_PROPERTY(QVariantList formats READ formats NOTIFY formatsChanged FINAL)
    /// The device id picked ("" = none yet: nothing opens until the user picks one and starts the preview).
    Q_PROPERTY(QString videoDevice READ videoDevice WRITE setVideoDevice NOTIFY selectionChanged FINAL)
    Q_PROPERTY(int formatIndex READ formatIndex WRITE setFormatIndex NOTIFY selectionChanged FINAL)
    Q_PROPERTY(bool audioEnabled READ audioEnabled WRITE setAudioEnabled NOTIFY selectionChanged FINAL)
    /// The sound input id; "" = the system default.
    Q_PROPERTY(QString audioDevice READ audioDevice WRITE setAudioDevice NOTIFY selectionChanged FINAL)
    /// [{ id, label, description }]
    Q_PROPERTY(QVariantList presets READ presets CONSTANT FINAL)
    Q_PROPERTY(QString preset READ preset WRITE setPreset NOTIFY selectionChanged FINAL)
    /// Listing devices or reading a device's formats is running.
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged FINAL)
    /// "idle", "opening", "previewing", "starting" (the recording is being set up), "recording", "finishing".
    Q_PROPERTY(QString state READ state NOTIFY stateChanged FINAL)
    /// A device is open (preview or recording): the top bar's indicator.
    Q_PROPERTY(bool deviceOpen READ deviceOpen NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool recording READ recording NOTIFY stateChanged FINAL)
    /// While a device is open: { seconds, elapsedText ("0:01:23"), dropped, droppedText, bytes, sizeText, fps, fpsText, level
    /// (0..1), picture (a picture has arrived), freeText, estimateText, encoder, deviceName }.
    Q_PROPERTY(QVariantMap status READ status NOTIFY statusChanged FINAL)
    /// "212 GB free on Data: room for about 2 h 10 min at this setting", worked out for the format and quality picked (and,
    /// while recording, from the size the file really grows by).
    Q_PROPERTY(QString diskText READ diskText NOTIFY statusChanged FINAL)
    /// Changes with every new picture: the live view's image source is "image://wabcut-capture/" + previewSerial.
    Q_PROPERTY(int previewSerial READ previewSerial NOTIFY statusChanged FINAL)
    /// What the live view is shaped like (width / height of the picture as shown).
    Q_PROPERTY(double aspect READ aspect NOTIFY formatsChanged FINAL)
    Q_PROPERTY(QString error READ error NOTIFY messageChanged FINAL)
    /// "camera" or "microphone" when `error` is the system refusing access (openPrivacySettings() opens the page), else "".
    Q_PROPERTY(QString accessKind READ accessKind NOTIFY messageChanged FINAL)
    /// The last capture's outcome ("Added "Capture 1" to Footage"), "" after the next start.
    Q_PROPERTY(QString notice READ notice NOTIFY messageChanged FINAL)
    /// The folder the captures go to.
    Q_PROPERTY(QString captureFolder READ captureFolder NOTIFY folderChanged FINAL)
    /// Captures a crash or a quit left unimported: [{ path, name, sizeText, startedAt, finished }].
    Q_PROPERTY(QVariantList interrupted READ interrupted NOTIFY interruptedChanged FINAL)

public:
    explicit CaptureBridge(QQmlEngine* engine, QObject* parent = nullptr);
    ~CaptureBridge() override;
    static CaptureBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    bool supported() const;
    QString unsupportedReason() const;
    QVariantList videoDevices() const { return m_videoDevices; }
    QVariantList audioDevices() const { return m_audioDevices; }
    QVariantList formats() const { return m_formatList; }
    QString videoDevice() const { return m_videoDevice; }
    int formatIndex() const { return m_formatIndex; }
    bool audioEnabled() const { return m_audioEnabled; }
    QString audioDevice() const { return m_audioDevice; }
    QVariantList presets() const;
    QString preset() const { return m_preset; }
    bool loading() const { return m_loading; }
    QString state() const;
    bool deviceOpen() const { return m_state != State::Idle; }
    bool recording() const { return m_state == State::Recording || m_state == State::Finishing; }
    QVariantMap status() const { return m_status; }
    QString diskText() const { return m_diskText; }
    int previewSerial() const { return m_previewSerial; }
    double aspect() const;
    QString error() const { return m_error; }
    QString accessKind() const { return m_accessKind; }
    QString notice() const { return m_notice; }
    QString captureFolder() const;
    QVariantList interrupted() const { return m_interrupted; }

    void setVideoDevice(const QString& id);
    void setFormatIndex(int index);
    void setAudioEnabled(bool enabled);
    void setAudioDevice(const QString& id);
    void setPreset(const QString& id);

    /// Lists the video and sound devices (opens none) and looks for captures an earlier run left.
    Q_INVOKABLE void refresh();
    /// Opens the picked device (and the sound input) and shows the live picture. Only on the user's action.
    Q_INVOKABLE void startPreview();
    /// Closes the devices (finishing a recording first).
    Q_INVOKABLE void stopPreview();
    /// Starts writing to "<clipName>.mkv" in the Capture folder; `binKey` is the Library bin ("b12"; "" = the root) the
    /// finished capture is imported into. False (error says why) when it cannot start. Needs the preview.
    Q_INVOKABLE bool record(const QString& clipName, const QString& binKey);
    /// Finishes the file and imports it.
    Q_INVOKABLE void stop();
    /// The name proposed for the next capture ("Capture 1").
    Q_INVOKABLE QString suggestName() const;
    /// Opens the Windows privacy page for `accessKind` (Camera or Microphone).
    Q_INVOKABLE void openPrivacySettings();
    /// Repairs the unimported capture at `path` (one of `interrupted`) and imports it into `binKey`.
    Q_INVOKABLE void recoverInterrupted(const QString& path, const QString& binKey);
    /// Deletes an unimported capture (one of `interrupted`) and its journal.
    Q_INVOKABLE void discardInterrupted(const QString& path);
    Q_INVOKABLE void clearMessages();

    // ---- Tests ----
    /// A backend instead of the system's (before the first use).
    void setBackend(std::shared_ptr<capture::CaptureBackend> backend);
    capture::CaptureBackend* backend() const { return m_backend.get(); }
    /// A named encoder for the captures (tests use "libx264" to stay off the graphics card); "" = the preset's.
    void setEncoder(std::string encoder) { m_encoder = std::move(encoder); }
    /// Replaces opening a privacy page.
    void setUrlOpener(std::function<void(const QUrl&)> opener) { m_urlOpener = std::move(opener); }
    /// Waits (processing events) until no worker task is queued or running. False on timeout.
    bool waitIdle(int timeoutMs);
    /// The shared picture source of the image provider.
    std::shared_ptr<CaptureShared> shared() const { return m_shared; }
    /// Polls the session now (what the 30 Hz timer does).
    void pollNow() { poll(); }
    /// Rescans the Capture folder for unimported captures.
    void rescanInterrupted();

signals:
    void devicesChanged();
    void formatsChanged();
    void selectionChanged();
    void loadingChanged();
    void stateChanged();
    void statusChanged();
    void messageChanged();
    void folderChanged();
    void interruptedChanged();
    /// A capture was added to the project (the file's path).
    void captured(const QString& path);
    /// Captures from an earlier run wait for an import (shown as a toast when a project opens).
    void interruptedFound(int count);

private:
    enum class State { Idle, Opening, Previewing, Starting, Recording, Finishing };
    struct Pending {
        std::filesystem::path file;
        QString binKey;
        QString name;
        std::string note;
    };

    void post(std::function<void()> task);
    void workerLoop(std::stop_token stop);
    void ensureBackend();
    void setLoading(bool loading);
    void setState(State state);
    void setError(const QString& message, const QString& access = {});
    void setNotice(const QString& message);
    void poll();
    void onProjectChanged();
    void onProjectReset();
    void finishForShutdown();
    void applyDevices(std::vector<capture::VideoDevice> video, std::vector<capture::AudioDevice> audio, const QString& problem);
    void applyFormats(const QString& deviceId, std::vector<capture::VideoFormat> formats, const std::optional<Error>& problem);
    void finishRecording(const std::expected<capture::RecorderResult, Error>& result, bool keepPreview);
    void importCaptured(const std::filesystem::path& file, const QString& binKey, const QString& name);
    QVariantMap interruptedEntry(const capture::InterruptedCapture& capture) const;
    capture::SessionConfig sessionConfig() const;
    std::filesystem::path folderPath() const;
    QString freeSpaceText(double bytesPerSecond) const;
    double estimatedBytesPerSecond() const;
    void updateDisk();
    void rememberFormat();
    void restartPreview();

    QQmlEngine* m_engine = nullptr;
    ProjectController* m_controller = nullptr;
    ProjectLifecycle* m_lifecycle = nullptr;
    std::shared_ptr<capture::CaptureBackend> m_backend;
    std::shared_ptr<capture::CaptureSession> m_session;
    std::shared_ptr<CaptureShared> m_shared = std::make_shared<CaptureShared>();
    std::function<void(const QUrl&)> m_urlOpener;

    QVariantList m_videoDevices;
    QVariantList m_audioDevices;
    QVariantList m_formatList;
    std::vector<capture::VideoFormat> m_formats;
    QString m_videoDevice;
    QString m_videoDeviceName;
    int m_formatIndex = -1;
    bool m_audioEnabled = true;
    QString m_audioDevice;
    QString m_preset = QStringLiteral("high");
    std::string m_encoder;
    bool m_loading = false;
    int m_loadingCount = 0;
    State m_state = State::Idle;
    QVariantMap m_status;
    QString m_diskText;
    int m_previewSerial = 0;
    float m_level = 0.0f;
    QString m_error;
    QString m_accessKind;
    QString m_notice;
    QVariantList m_interrupted;
    std::string m_scannedProject;
    Pending m_pending;
    qint64 m_lastDiskCheckMs = 0;
    qint64 m_freeBytes = -1;
    bool m_autoStopping = false;
    bool m_destroying = false;
    std::uint64_t m_resetListener = 0; ///< the session's listener for a project replaced (new, open, close)
    std::uint64_t m_epoch = 0; ///< bumped when the devices are closed or the project goes: a worker result of an older epoch is dropped

    QTimer m_timer;
    std::mutex m_taskMutex;
    std::condition_variable_any m_taskCv;
    std::deque<std::function<void()>> m_tasks;
    std::atomic<int> m_tasksOutstanding{0};
    std::jthread m_worker;
};

} // namespace wabcut::app
