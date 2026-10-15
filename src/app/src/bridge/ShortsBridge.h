#pragma once

// Auto reframe and the shorts finder in the app (AI-9, docs/ai.md "Auto reframe", "Shorts finder"):
//
//   Shorts  (QML singleton "Shorts", the Shorts browser panel): find() runs shorts.find on a worker (the transcript
//           scored locally; optionally re-ranked by the user's cloud model, with consent); candidates come back as
//           cards: score breakdown, reasons, an editable title and a strip of frames across the range. create() makes
//           the chosen shorts as a staged preview (shorts.create, dry run): apply() commits them as one undo step,
//           discard() drops them; exportAll() queues 1080x1920 exports of what was made (H.264 or HEVC).
//   Reframe (QML singleton "Reframe", the Reframe dialog from the sequence menu): preview() finds the subjects of the
//           current sequence's media (analysis.subjects, cached) and runs reframe.createSequence as a dry run; the
//           dialog draws the frame path and subjects over the source frames; apply() commits the new sequence and
//           switches to it. downloadDetectors() fetches YuNet and YOLOX (license cards first) into the model store.
//
// Both run catalog commands on a scratch copy of the project, so the live session only changes on apply(). Frames
// come from the image provider "wabcut-frames" (image://wabcut-frames/<media key>/<source ticks>).

#include <QImage>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <wabcut/model/ProjectState.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace wabcut::command {
class CommandServices;
}
namespace wabcut::ai {
class IChatModel;
}

class AiController; // global (AiController.h)

namespace wabcut::app {

class ProjectController;
class ProjectLifecycle;
class TranscriptBridge;

/// The app's services for the AI-9 commands: transcripts, audio analysis, subjects (one subject provider, one
/// wabcut-embed with the detectors, for the whole app) and, when given, the user's cloud chat model (created on the GUI
/// thread through AiPlatform, so consent, the locks and the spend cap apply to its calls).
std::shared_ptr<command::CommandServices> aiEditingServices(TranscriptBridge* transcripts, const std::filesystem::path& projectFile,
                                                            std::shared_ptr<ai::IChatModel> chat = nullptr);

/// The project the preview frames (image://wabcut-frames) are decoded from: bridges that show frames of a project set
/// it before their previews (the Shorts, Reframe, Rough cut, Takes and Auto zoom bridges).
void setPreviewFrameContext(std::shared_ptr<const ProjectState> state, std::filesystem::path projectFile);

/// Tests: the preview frames (image://wabcut-frames) come from `frames` instead of the decoder; null = decode.
void setPreviewFramesForTests(std::function<QImage(const MediaItem& media, Ticks sourceTime)> frames);

/// Subjects of the project's media for the reframe and shorts commands (the app's one subject provider).
std::shared_ptr<command::CommandServices> appReframeServices(const std::filesystem::path& projectFile);

/// C.16's gaming edits' host services: sequence.removeDeadTime's media activity (editor::activityMeasure, decoded on the
/// calling thread: run it on a worker) and sequence.makeVersion's Auto Reframe (the app's subject provider).
std::shared_ptr<command::CommandServices> appGamingServices(const std::filesystem::path& projectFile);

/// One job off the GUI thread at a time; its result is applied on the GUI thread; cancellable. A job that throws ends
/// as a failure instead of ending the app (std::terminate on the worker).
class BridgeJob {
public:
    using Finish = std::function<void()>;
    /// Runs on the worker: returns what to do on the GUI thread afterwards.
    using Work = std::function<Finish(const std::atomic<bool>& cancelled)>;
    /// Runs on the worker when the work threw (`message` is the exception's text): returns what to do on the GUI
    /// thread instead, typically showing the message.
    using Failed = std::function<Finish(const QString& message)>;

    ~BridgeJob();
    bool running() const { return m_running; }
    void cancel() { m_cancelled = true; }
    /// Cancels the job and waits for its worker (its result is still applied on the GUI thread later, while the
    /// context lives). Owners call it when something the worker uses goes away: AiController::shuttingDown for jobs
    /// that use the AI platform or borrow its transport (review wave 5, A5-P2-19).
    void stop();
    /// What the jobs started without a `failed` of their own do when they throw (default: nothing, the job just ends).
    void setFailed(Failed failed) { m_failed = std::move(failed); }
    /// False when a job is running. `synchronous` runs it on the calling thread (tests).
    bool start(QObject* context, bool synchronous, Work work, Failed failed = {});

private:
    void reap();
    std::jthread m_thread;
    std::atomic<bool> m_cancelled{false};
    bool m_running = false;
    Failed m_failed;
};

class ShortsBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY jobChanged FINAL)
    Q_PROPERTY(QString phase READ phase NOTIFY jobChanged FINAL)
    /// 0..1, or -1 when unknown.
    Q_PROPERTY(double progress READ progress NOTIFY jobChanged FINAL)
    Q_PROPERTY(QVariantList candidates READ candidates NOTIFY candidatesChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY candidatesChanged FINAL)
    Q_PROPERTY(bool reranked READ reranked NOTIFY candidatesChanged FINAL)
    /// The user's cloud chat model can re-rank (a model is set up, the project is not no-cloud).
    Q_PROPERTY(bool cloudAvailable READ cloudAvailable NOTIFY statusChanged FINAL)
    /// Staged shorts waiting for apply() / discard().
    Q_PROPERTY(int pendingCount READ pendingCount NOTIFY pendingChanged FINAL)
    Q_PROPERTY(QStringList pendingNames READ pendingNames NOTIFY pendingChanged FINAL)
    /// What apply() made ("s12" keys), for exportAll().
    Q_PROPERTY(int createdCount READ createdCount NOTIFY pendingChanged FINAL)

public:
    static ShortsBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    explicit ShortsBridge(QObject* parent = nullptr);
    ~ShortsBridge() override;

    void setController(ProjectController* controller);
    void setProjectFile(ProjectLifecycle* file);
    void setTranscripts(TranscriptBridge* transcripts);
    void setAi(::AiController* ai);

    bool busy() const { return m_job.running(); }
    QString phase() const { return m_phase; }
    double progress() const { return m_progress; }
    QVariantList candidates() const { return m_candidates; }
    QString message() const { return m_message; }
    bool reranked() const { return m_reranked; }
    bool cloudAvailable() const;
    int pendingCount() const { return static_cast<int>(m_pendingNames.size()); }
    QStringList pendingNames() const { return m_pendingNames; }
    int createdCount() const { return static_cast<int>(m_created.size()); }

    /// Finds `count` shorts (about `query`, "" = anything); `rerank` asks the cloud model too.
    Q_INVOKABLE QString find(int count, const QString& query, bool rerank);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void setTitle(int index, const QString& title);
    /// Stages the shorts at these candidate indices (all when empty) as new 9:16 sequences.
    Q_INVOKABLE QString create(const QVariantList& indices, const QVariantMap& options = {});
    Q_INVOKABLE QString apply();
    Q_INVOKABLE void discard();
    /// Queues 1080x1920 exports of the shorts apply() made; `hevc` for H.265. Returns how many were queued.
    Q_INVOKABLE int exportAll(bool hevc);

    /// Tests: run jobs on the calling thread.
    void setSynchronous(bool synchronous) { m_synchronous = synchronous; }
    /// Tests: services instead of the app's (fixture transcripts, subjects, a scripted cloud model).
    void setServices(std::shared_ptr<command::CommandServices> services) { m_services = std::move(services); }
    bool waitForIdle(int milliseconds);

signals:
    void jobChanged();
    void candidatesChanged();
    void statusChanged();
    void pendingChanged();

private:
    std::shared_ptr<command::CommandServices> services();
    void setJob(QString phase, double progress);

    QPointer<ProjectController> m_controller;
    QPointer<ProjectLifecycle> m_projectFile;
    QPointer<TranscriptBridge> m_transcripts;
    QPointer<::AiController> m_ai;
    std::shared_ptr<command::CommandServices> m_services;
    BridgeJob m_job;
    bool m_synchronous = false;
    QString m_phase;
    double m_progress = 0.0;
    QVariantList m_candidates;
    QString m_message;
    bool m_reranked = false;
    SequenceId m_sequence; ///< the candidates' sequence
    std::shared_ptr<const ProjectState> m_pending;
    std::uint64_t m_pendingRevision = 0;
    QStringList m_pendingNames;
    std::vector<SequenceId> m_pendingIds;
    std::vector<SequenceId> m_created;
};

class ReframeBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY jobChanged FINAL)
    Q_PROPERTY(QString phase READ phase NOTIFY jobChanged FINAL)
    Q_PROPERTY(double progress READ progress NOTIFY jobChanged FINAL)
    /// The dry run's result: {name, width, height, clips: [{clip, name, media, guide, path: [[t,x,y,w,h]...],
    /// subjects: [...], frames: [url per path sample]}]}; empty before preview().
    Q_PROPERTY(QVariantMap preview READ preview NOTIFY previewChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY previewChanged FINAL)
    /// Faces and people are detected (helper built, YuNet and YOLOX downloaded); else salient regions only.
    Q_PROPERTY(bool detectorsAvailable READ detectorsAvailable NOTIFY statusChanged FINAL)
    Q_PROPERTY(QString detectorStatus READ detectorStatus NOTIFY statusChanged FINAL)
    Q_PROPERTY(QString detectorLicense READ detectorLicense CONSTANT FINAL)
    Q_PROPERTY(bool downloading READ downloading NOTIFY statusChanged FINAL)
    Q_PROPERTY(double downloadProgress READ downloadProgress NOTIFY statusChanged FINAL)
    Q_PROPERTY(QString sourceName READ sourceName NOTIFY statusChanged FINAL)

public:
    static ReframeBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    explicit ReframeBridge(QObject* parent = nullptr);
    ~ReframeBridge() override;

    void setController(ProjectController* controller);
    void setProjectFile(ProjectLifecycle* file);
    void setTranscripts(TranscriptBridge* transcripts);
    void setAi(::AiController* ai);

    bool busy() const { return m_job.running(); }
    QString phase() const { return m_phase; }
    double progress() const { return m_progress; }
    QVariantMap preview() const { return m_preview; }
    QString message() const { return m_message; }
    bool detectorsAvailable() const;
    QString detectorStatus() const;
    QString detectorLicense() const;
    bool downloading() const { return m_downloading; }
    double downloadProgress() const { return m_downloadProgress; }
    QString sourceName() const;

    /// options: aspect ("9:16"), mode, fit, smoothness (0..1).
    Q_INVOKABLE QString makePreview(const QVariantMap& options);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE QString apply();
    Q_INVOKABLE void discard();
    Q_INVOKABLE void downloadDetectors();
    Q_INVOKABLE void cancelDownload();

    void setSynchronous(bool synchronous) { m_synchronous = synchronous; }
    void setServices(std::shared_ptr<command::CommandServices> services) { m_services = std::move(services); }
    bool waitForIdle(int milliseconds);

signals:
    void jobChanged();
    void previewChanged();
    void statusChanged();
    void notice(const QString& text);

private:
    std::shared_ptr<command::CommandServices> services();
    void setJob(QString phase, double progress);

    QPointer<ProjectController> m_controller;
    QPointer<ProjectLifecycle> m_projectFile;
    QPointer<TranscriptBridge> m_transcripts;
    QPointer<::AiController> m_ai;
    std::shared_ptr<command::CommandServices> m_services;
    BridgeJob m_job;
    bool m_synchronous = false;
    QString m_phase;
    double m_progress = 0.0;
    QVariantMap m_preview;
    QString m_message;
    std::shared_ptr<const ProjectState> m_pending;
    std::uint64_t m_pendingRevision = 0;
    SequenceId m_pendingSequence;
    bool m_downloading = false;
    double m_downloadProgress = 0.0;
    std::jthread m_download;
};

struct ShortsBridgeForeign {
    Q_GADGET
    QML_FOREIGN(ShortsBridge)
    QML_NAMED_ELEMENT(Shorts)
    QML_SINGLETON
public:
    static ShortsBridge* create(QQmlEngine* e, QJSEngine* j) { return ShortsBridge::create(e, j); }
};

struct ReframeBridgeForeign {
    Q_GADGET
    QML_FOREIGN(ReframeBridge)
    QML_NAMED_ELEMENT(Reframe)
    QML_SINGLETON
public:
    static ReframeBridge* create(QQmlEngine* e, QJSEngine* j) { return ReframeBridge::create(e, j); }
};

} // namespace wabcut::app
