#pragma once

// VoiceOverBridge: voice-over recording (A.8, docs/ai.md "Voice-over recording"), the QML singleton `VoiceOver`.
// Owner: Audio engineer; the QML (record buttons, count-in overlay, takes switcher, input meter, Preferences > Audio
// input device) is the UI engineer's (docs/ai.md, "Voice-over recording: UI notes").
//
//   * The input: the capture devices (inputDevices, inputDevice: "" = the system default), openInput() for the input
//     meter (Preferences > Audio, the armed track's meter), closeInput(). `inputOpen` is true exactly while the
//     microphone is open: the UI shows the recording indicator then. The microphone opens only from these calls and
//     start(), which QML makes on the user's own action; no command, agent or MCP client reaches them.
//   * A pass: start() (record.start / record.toggle) records onto the first armed audio track (track.setRecordArm),
//     else the first targeted one, between the sequence In and Out when both are set, else from the playhead until
//     stop(): the count-in, the pre-roll, the punch-in/out and the post-roll (`options`), with input monitoring.
//     `state` and `countIn` drive the count-in overlay; stop() (record.stop, or the transport stopped: Space) ends the
//     pass and puts the take in the project as one undo step ("Voice-over Record"): a WAV (48 kHz, 24-bit) in the
//     project's Voice-over folder (takesFolder(): one per project), a media item in the Voice-over bin and a clip on
//     the track. cancel() (an explicit cancel only) drops it. Quitting, closing the window or the project, or this
//     bridge going away during a pass ends it as stop() does: the take is kept, and placed while the project exists.
//   * Recovery: each take's WAV has a journal (WavWriter) with where it was recorded. When a project file opens, the
//     takes a crash (or a quit before placing) left in its folder are listed in `recoveredTakes` for a prompt, read
//     only; placeRecoveredTake() repairs the one the user picks and puts it back where it was recorded,
//     dismissRecoveredTakes() stops offering them. Only files named as the recorder names takes, with their journal,
//     are looked at: never another WAV, a link or junction, or a file with other names (hard links).
//   * Latency: measureLatency() runs a loopback pass (the output wired, or held, to the input) and sets the
//     calibration from what came back.
//   * Takes: takesOf(clip) lists a recorded slot's takes; useTake(clip, take) is record.useTake.
//
// Keys cross into QML as opaque strings ("t12", "c7"); levels in dB; no ticks (principle 9).

#include <wabcut/model/Ids.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <filesystem>
#include <memory>
#include <optional>

class QJSEngine;
class QQmlEngine;

namespace wabcut::audio {
class AudioInput;
}
namespace wabcut::editor {
class PlaybackSession;
}
namespace wabcut::playback {
class VoiceOverRecorder;
}

namespace wabcut::app {

class PlaybackBridge;
class ProjectController;

class VoiceOverBridge : public QObject {
    Q_OBJECT
    // Registered through VoiceOverSingleton (below): a default-constructible class that is its own QML_SINGLETON is
    // built with its default constructor, and create() (which hands it the controller and the playback) never runs.
    /// [{ id, name, isDefault }]: the capture devices (refreshDevices()).
    Q_PROPERTY(QVariantList inputDevices READ inputDevices NOTIFY inputDevicesChanged FINAL)
    /// The device id to record from; "" = the system default.
    Q_PROPERTY(QString inputDevice READ inputDevice WRITE setInputDevice NOTIFY inputChanged FINAL)
    /// The microphone is open (show the recording indicator).
    Q_PROPERTY(bool inputOpen READ inputOpen NOTIFY inputChanged FINAL)
    Q_PROPERTY(QString inputName READ inputName NOTIFY inputChanged FINAL)
    /// { peakDb, rmsDb, clipped } since the previous reading (~30 Hz while the input is open).
    Q_PROPERTY(QVariantMap levels READ levels NOTIFY levelsChanged FINAL)
    /// A pass is running.
    Q_PROPERTY(bool recording READ recording NOTIFY stateChanged FINAL)
    /// "idle", "countIn", "preroll", "recording", "postroll", "finished", "failed".
    Q_PROPERTY(QString state READ state NOTIFY stateChanged FINAL)
    /// The count shown in the count-in overlay (3, 2, 1; 0 outside the count-in).
    Q_PROPERTY(int countIn READ countIn NOTIFY stateChanged FINAL)
    Q_PROPERTY(double recordedSeconds READ recordedSeconds NOTIFY stateChanged FINAL)
    /// "A2": the track the pass records onto ("" when idle).
    Q_PROPERTY(QString targetTrack READ targetTrack NOTIFY stateChanged FINAL)
    /// { inputMs, monitorMs, calibrationMs, roundTripMs }.
    Q_PROPERTY(QVariantMap latency READ latency NOTIFY latencyChanged FINAL)
    /// { countIn (0..10 s), beep, prerollSeconds, postrollSeconds, monitor, monitorGainDb, calibrationMs }. The UI
    /// persists it (Preferences > Audio > Voice-over).
    Q_PROPERTY(QVariantMap options READ options WRITE setOptions NOTIFY optionsChanged FINAL)
    /// The last failure, for a toast ("" after a good pass).
    Q_PROPERTY(QString lastError READ lastError NOTIFY errorChanged FINAL)
    /// Takes to offer back when the project opens (a prompt, or a row per take): [{ path, name, seconds, interrupted
    /// (cut off by a crash: repaired when placed), track ("A2"), startText, placeable }]. Empty when there is nothing.
    Q_PROPERTY(QVariantList recoveredTakes READ recoveredTakes NOTIFY recoveredTakesChanged FINAL)

public:
    explicit VoiceOverBridge(QObject* parent = nullptr);
    ~VoiceOverBridge() override;
    static VoiceOverBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setController(ProjectController* controller);
    void setPlayback(PlaybackBridge* playback);
    /// The microphone. With WABCUT_AUDIO_BACKEND=null or null-realtime (tests, headless machines) it is the Null input:
    /// no device, it hears what push() gives it.
    audio::AudioInput& input();
    /// Where a saved project's takes go: <dir>/<project name>.wabcut-media/Voice-over.
    static std::filesystem::path takesFolder(const std::filesystem::path& projectFile);

    QVariantList inputDevices() const { return m_devices; }
    QString inputDevice() const { return m_deviceId; }
    void setInputDevice(const QString& id);
    bool inputOpen() const;
    QString inputName() const;
    QVariantMap levels() const { return m_levels; }
    bool recording() const;
    QString state() const;
    int countIn() const;
    double recordedSeconds() const;
    QString targetTrack() const { return m_trackLabel; }
    QVariantMap latency() const;
    QVariantMap options() const;
    void setOptions(const QVariantMap& options);
    QString lastError() const { return m_error; }
    QVariantList recoveredTakes() const { return m_recovered; }

    Q_INVOKABLE void refreshDevices();
    Q_INVOKABLE bool openInput();
    Q_INVOKABLE void closeInput();
    Q_INVOKABLE bool start();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void toggle();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE bool measureLatency();
    /// track.setRecordArm; "" or the error.
    Q_INVOKABLE QString setRecordArm(const QString& track, bool armed);
    /// [{ take, media ("m4"), name, startText, seconds, device, createdAt, active }] of the slot `clip` plays.
    Q_INVOKABLE QVariantList takesOf(const QString& clip);
    /// record.useTake; "" or the error.
    Q_INVOKABLE QString useTake(const QString& clip, int take);
    /// Scans the project's Voice-over folder again, read only, for takes that were never placed (their journal is
    /// there and no media item uses them) and lists them in `recoveredTakes` (also returned). Runs by itself when a
    /// project file opens.
    Q_INVOKABLE QVariantList recoverTakes();
    /// Repairs a take `recoveredTakes` lists (a crash's stale header) and puts it back where it was recorded (its
    /// sequence, track and punch-in), one undo step; "" or the error.
    Q_INVOKABLE QString placeRecoveredTake(const QString& path);
    /// Stops offering the listed takes (their journals go; the files stay in the folder).
    Q_INVOKABLE void dismissRecoveredTakes();

signals:
    void inputDevicesChanged();
    void inputChanged();
    void levelsChanged();
    void stateChanged();
    void latencyChanged();
    void optionsChanged();
    void errorChanged();
    /// A take was put in the project (its clip key), or a loopback pass measured the latency.
    void takeRecorded(const QString& clip);
    void latencyMeasured(double roundTripMs, int clips);
    /// `recoveredTakes` changed: show (or update) the recovery prompt when it is not empty.
    void recoveredTakesChanged();

private:
    /// Playback runs past the sequence end while a pass runs (PlaybackController::setStopAtEnd), restored however
    /// the pass ends.
    class FreeRun {
    public:
        explicit FreeRun(PlaybackBridge* playback);
        ~FreeRun();
        FreeRun(const FreeRun&) = delete;
        FreeRun& operator=(const FreeRun&) = delete;

    private:
        QPointer<PlaybackBridge> m_playback;
    };

    bool beginPass(bool loopback);
    void finishPass();
    /// Ends a running pass for good (the app quits, the project closes, the bridge goes): as stop(), the take kept and
    /// placed, while the pass's playback session exists; after it went, the file is closed as it is (a take for the
    /// recovery scan) and the engine is not touched.
    void endPass();
    /// Scans for takes to recover when another project file opened.
    void onMediaChanged();
    void poll();
    void setError(const QString& message);
    bool ensureInput();
    void releaseInputIfUnheld();

    QPointer<ProjectController> m_controller;
    QPointer<PlaybackBridge> m_playback;
    std::unique_ptr<audio::AudioInput> m_input;
    std::optional<FreeRun> m_freeRun; // declared before m_recorder: restored after the pass is gone
    std::unique_ptr<playback::VoiceOverRecorder> m_recorder;
    editor::PlaybackSession* m_passSession = nullptr; ///< the session the pass runs on (its engine and player)
    std::filesystem::path m_scannedProject;          ///< the project file the takes were last scanned for
    QVariantList m_recovered;
    QTimer m_timer;
    QVariantList m_devices;
    QString m_deviceId;
    QVariantMap m_levels;
    QString m_error;
    bool m_inputHeld = false; ///< openInput() asked for it (the meter), beyond a pass
    bool m_passLoopback = false;
    SequenceId m_sequence;
    TrackId m_track;
    QString m_trackLabel;
    // Options.
    int m_countIn = 3;
    bool m_beep = true;
    double m_preroll = 2.0;
    double m_postroll = 1.0;
    bool m_monitor = false;
    double m_monitorGainDb = 0.0;
    double m_calibrationMs = 0.0;
    double m_roundTripMs = 0.0;
};

struct VoiceOverSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::VoiceOverBridge)
    QML_NAMED_ELEMENT(VoiceOver)
    QML_SINGLETON
public:
    static VoiceOverBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return VoiceOverBridge::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
