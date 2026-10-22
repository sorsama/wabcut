#pragma once

// ObjectMaskBridge: the AI Object Mask in the app (singleton `ObjectMasks`, AI-14; docs/masks.md "Object masks",
// docs/ai.md "AI Object Mask"). Owner: AI platform.
//
// It works on the clip and mask target of `Masks` (MaskBridge): Effect Controls' mask section shows its Object tool
// (qml/panels/masks/ObjectMaskControls.qml) and the program monitor its prompt overlay (ObjectPromptOverlay.qml):
//   * armed, a click on the monitor is a positive point (or negative: the toggle, or Alt) and a drag a box, on the
//     frame at the playhead. The first prompt runs mask.objectSelect there ("none": that frame only, a second or so
//     once the model is loaded), the next ones mask.refine on the object mask it made (the one selected in Masks);
//   * Track backward / forward / both ways runs mask.propagate: a job with its progress, cancel and per-frame
//     confidence; it stops at the shot's cuts unless "across shots";
//   * the matte preview over the monitor: off, colour (the object tinted) or alpha (the matte in grey), from the
//     analysis cache (image://wabcut-matte);
//   * the model: SAM 2.1 tiny (190 MB, Apache-2.0), downloaded on request (its licence card shown first), then kept
//     loaded in wabcut-embed between clicks.
// Every edit is computed on a worker thread from a snapshot and applied as one undo step if the project did not move.
//
// The Source Monitor: with "Track an object" on, a redaction drag is a box prompt instead of a fixed box: the object is
// segmented at the source position and tracked through the source's In..Out (else the whole media), and becomes a
// redaction whose region is its matte mask. Redactions fail closed (render/Matte.h): where a frame has no matte the
// region is the nearest matte grown, never nothing. The assistant cannot make redactions (redaction.* stay out of its
// reach); this is the user's tool.

#include <wabcut/ai/segment/MediaSegment.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/Masks.h>

#include <QImage>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

class QJSEngine;
class QQmlEngine;

namespace wabcut::ai::analysis {
class AnalysisProvider;
}

namespace wabcut::app {

class MaskBridge;
class ProjectController;
class SourceMonitorBridge;

class ObjectMaskBridge : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(ObjectMasks)
    QML_SINGLETON

    /// SAM 2.1 is on this computer; else downloadModel() (modelCard: its size and licence, shown before).
    Q_PROPERTY(bool available READ available NOTIFY modelChanged FINAL)
    Q_PROPERTY(QString modelCard READ modelCard CONSTANT FINAL)
    Q_PROPERTY(bool downloading READ downloading NOTIFY modelChanged FINAL)
    Q_PROPERTY(double downloadProgress READ downloadProgress NOTIFY modelChanged FINAL)
    /// The program monitor's prompt mode: clicks add points, a drag draws a box.
    Q_PROPERTY(bool armed READ armed WRITE setArmed NOTIFY stateChanged FINAL)
    /// What a click says: on the object (true) or off it (false); Alt-click says the other.
    Q_PROPERTY(bool positive READ positive WRITE setPositive NOTIFY stateChanged FINAL)
    /// Track across shot cuts (default false: stop at the cuts around the frame).
    Q_PROPERTY(bool acrossShots READ acrossShots WRITE setAcrossShots NOTIFY stateChanged FINAL)
    /// The object mask the prompts refine (Masks' key, "e12/3"), or "" (the next prompt makes a new one).
    Q_PROPERTY(QString mask READ mask NOTIFY stateChanged FINAL)
    /// Its prompt on the frame at the playhead, canvas fractions: points [{x, y, positive}], box {x0, y0, x1, y1}.
    Q_PROPERTY(QVariantList points READ points NOTIFY geometryChanged FINAL)
    Q_PROPERTY(QVariantMap box READ box NOTIFY geometryChanged FINAL)
    /// A run: busy, progress 0..1, what it does now, per-frame confidence (0..1; -1 where the object was not seen)
    /// and the frames it covers (the strip's width).
    Q_PROPERTY(bool busy READ busy NOTIFY runChanged FINAL)
    Q_PROPERTY(double progress READ progress NOTIFY runChanged FINAL)
    Q_PROPERTY(QString activity READ activity NOTIFY runChanged FINAL)
    Q_PROPERTY(QVariantList confidence READ confidence NOTIFY runChanged FINAL)
    Q_PROPERTY(int total READ total NOTIFY runChanged FINAL)
    /// What the last run did (frames, where it stopped, the device and its speed), or its problem.
    Q_PROPERTY(QString status READ status NOTIFY runChanged FINAL)
    /// The matte preview: "off", "colour" or "alpha"; its image (image://wabcut-matte/..., "" = none) and where the
    /// frame lies on the canvas: [TLx, TLy, TRx, TRy, BLx, BLy] (canvas fractions).
    Q_PROPERTY(QString preview READ preview WRITE setPreview NOTIFY previewChanged FINAL)
    Q_PROPERTY(QString previewSource READ previewSource NOTIFY previewChanged FINAL)
    Q_PROPERTY(QVariantList previewQuad READ previewQuad NOTIFY previewChanged FINAL)
    /// The Source Monitor's redaction drag tracks an object instead of marking a fixed box.
    Q_PROPERTY(bool trackRedactions READ trackRedactions WRITE setTrackRedactions NOTIFY stateChanged FINAL)

public:
    /// Not default-constructible on purpose: QML then makes the singleton with create(), which registers the matte
    /// preview's image provider (Qt prefers a default constructor over create()).
    explicit ObjectMaskBridge(QObject* parent);
    ~ObjectMaskBridge() override;
    static ObjectMaskBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    bool available() const;
    QString modelCard() const;
    bool downloading() const { return m_downloading; }
    double downloadProgress() const { return m_downloadProgress; }
    bool armed() const { return m_armed; }
    void setArmed(bool on);
    bool positive() const { return m_positive; }
    void setPositive(bool on);
    bool acrossShots() const { return m_acrossShots; }
    void setAcrossShots(bool on);
    QString mask() const { return m_mask; }
    QVariantList points() const { return m_points; }
    QVariantMap box() const { return m_box; }
    bool busy() const { return m_busy; }
    double progress() const { return m_progress; }
    QString activity() const { return m_activity; }
    QVariantList confidence() const { return m_confidence; }
    int total() const { return m_total; }
    QString status() const { return m_status; }
    QString preview() const { return m_preview; }
    void setPreview(const QString& mode);
    QString previewSource() const { return m_previewSource; }
    QVariantList previewQuad() const { return m_previewQuad; }
    bool trackRedactions() const { return m_trackRedactions; }
    void setTrackRedactions(bool on);

    /// A click on the program monitor (canvas fractions): a point on (positive) or off the object.
    Q_INVOKABLE bool click(double x, double y, bool positive);
    /// A box dragged on the program monitor (canvas fractions).
    Q_INVOKABLE bool drawBox(double x0, double y0, double x1, double y1);
    /// Tracks the object mask through the clip from its last prompt: "backward", "forward" or "both".
    Q_INVOKABLE bool propagate(const QString& direction);
    Q_INVOKABLE void cancel();
    /// Forgets which object mask the prompts refine: the next prompt makes a new one.
    Q_INVOKABLE void newObject();
    /// The Source Monitor: a box dragged over the source picture (fractions of it) becomes an AI-tracked redaction.
    Q_INVOKABLE bool redactObject(double x0, double y0, double x1, double y1);
    /// 0.2: every face of the Source Monitor's media (between its In and Out) found, followed and covered by its own
    /// redaction (redaction.detectFaces), in the style Masks redacts with. One undo step.
    Q_INVOKABLE bool redactFaces();
    Q_INVOKABLE void downloadModel();
    Q_INVOKABLE void cancelDownload();

    // ---- C++ (tests, the assistant) ----
    /// Run on the calling thread (tests).
    void setSynchronous(bool on) { m_synchronous = on; }
    /// Replaces the media-backed services (tests: a scripted model and video; the shots of the media).
    void setServices(std::shared_ptr<ai::segment::SegmentServices> services, std::shared_ptr<ai::analysis::AnalysisProvider> shots);
    /// The object-mask services for the assistant's runs (the same model, kept loaded).
    std::shared_ptr<ai::segment::SegmentServices> servicesFor(const std::filesystem::path& projectFile);
    /// The matte preview image (the image provider reads it).
    QImage previewImage() const;

public slots:
    void refresh();

signals:
    void modelChanged();
    void stateChanged();
    void geometryChanged();
    void runChanged();
    void previewChanged();

private:
    struct Placement;
    struct Outcome;
    ProjectController* controller() const;
    MaskBridge* masks() const;
    SourceMonitorBridge* sourceMonitor() const;
    std::filesystem::path projectFile() const;
    void attach();
    std::shared_ptr<ai::analysis::AnalysisProvider> shotsFor(const std::filesystem::path& projectFile);
    std::optional<Placement> placement() const;
    std::optional<Placement> sourcePlacement() const;
    std::shared_ptr<const ai::segment::MatteTrack> trackOf(const std::string& identity);
    /// Runs `command` on a copy of the project on a worker thread; `apply` commits its result on the GUI thread.
    bool runCommand(const char* command, nlohmann::json params, QString activity, std::function<QString(const Outcome&)> apply);
    void startRun(QString activity);
    void finishRun(QString status);
    void onProgress(const ai::segment::SegmentProgress& p);
    bool prompt(std::vector<ai::segment::PromptPoint> points, std::optional<ai::segment::MatteBox> box);
    void refreshPreview();
    void setStatus(const QString& status);

    bool m_attached = false;
    bool m_armed = false;
    bool m_positive = true;
    bool m_acrossShots = false;
    bool m_trackRedactions = false;
    QString m_mask;
    QVariantList m_points;
    QVariantMap m_box;
    bool m_busy = false;
    double m_progress = 0.0;
    QString m_activity;
    QVariantList m_confidence;
    int m_total = 0;
    QString m_status;
    QString m_preview = QStringLiteral("off");
    QString m_previewSource;
    QVariantList m_previewQuad;
    int m_previewSerial = 0;
    bool m_downloading = false;
    double m_downloadProgress = 0.0;
    std::jthread m_download;

    std::shared_ptr<ai::segment::SegmentServices> m_services;
    std::shared_ptr<ai::analysis::AnalysisProvider> m_shots;
    std::filesystem::path m_servicesFile;
    bool m_injected = false;
    std::map<std::string, std::shared_ptr<const ai::segment::MatteTrack>> m_tracks;

    struct Shared {
        mutable std::mutex mutex;
        QImage preview;
    };
    std::shared_ptr<Shared> m_shared = std::make_shared<Shared>();

    bool m_synchronous = false;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    std::jthread m_worker;
};

} // namespace wabcut::app
