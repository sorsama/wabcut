#pragma once

// AI-11 creator superpowers in the app (docs/ai.md "Creator superpowers"): three QML singletons over the catalog
// commands. Each runs its command on a scratch copy of the project on a worker; the live session changes only on
// apply(), as one undo step guarded by the revision the preview was made at.
//
//   RoughCut  the "Rough cut" sheet: preview(text, options) runs roughcut.fromScript as a dry run (paste a script, an
//             outline or bullets; scope; prefer; clean-up; B-roll; the optional cloud interpretation of loose outlines,
//             only with a set-up model and never for a no-cloud project). The report lists every line with its chosen
//             take, confidence and alternates; swapTake(beat, take) swaps in the staged cut (a staged edit, again a dry
//             run); apply() commits the cut and opens it; discard() drops it.
//   Takes     the "Takes" panel: find(options) runs takes.find on the current sequence; every group shows its takes'
//             words with the difference from the best, quality and pace; audition(group, take) plays a take in place
//             (timeline takes from the playhead in the Program Monitor, others in the Source Monitor); use(group, take)
//             and useBest() stage the edit (drawn as the timeline ghost: struck-through cuts, dashed new clips); apply()
//             commits it, discard() drops it.
//   AutoZoom  "Auto zoom…" on a clip: preview(clip, options) runs autozoom.apply as a dry run with the zoom window over
//             time and the recording's frames (image://wabcut-frames) for the dialog to draw; apply() commits it.

#include "ShortsBridge.h"

#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QQmlEngine>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <wabcut/command/Command.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/ProjectState.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wabcut::command {
class CommandServices;
}

class AiController;

namespace wabcut::app {

class ProjectController;
class ProjectLifecycle;
class TranscriptBridge;

/// What the creator commands get in the app: transcripts and audio (aiEditingServices), take audio (AudioEnergyService),
/// the Library's smart search (B-roll, the text encoder), the recordings' frames and sidecars (auto zoom), and when given
/// the user's guarded cloud chat model (outline interpretation).
std::shared_ptr<command::CommandServices> creatorServices(QQmlEngine* engine, TranscriptBridge* transcripts,
                                                          const std::filesystem::path& projectFile,
                                                          std::shared_ptr<ai::IChatModel> chat = nullptr);

/// The common part of the three bridges: the project, the services, one job at a time, a staged state.
class CreatorBase : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY jobChanged FINAL)
    Q_PROPERTY(QString phase READ phase NOTIFY jobChanged FINAL)
    /// 0..1, or -1 when unknown.
    Q_PROPERTY(double progress READ progress NOTIFY jobChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY resultChanged FINAL)
    /// A staged edit waits for apply() / discard().
    Q_PROPERTY(bool staged READ staged NOTIFY resultChanged FINAL)

public:
    explicit CreatorBase(QObject* parent = nullptr);
    ~CreatorBase() override;

    void setEngine(QQmlEngine* engine);
    bool busy() const { return m_job.running(); }
    QString phase() const { return m_phase; }
    double progress() const { return m_progress; }
    QString message() const { return m_message; }
    bool staged() const { return static_cast<bool>(m_staged); }

    Q_INVOKABLE void cancel() { m_job.cancel(); }
    Q_INVOKABLE virtual void discard();
    /// Menus call this; Main.qml opens the sheet, panel or dialog (with `clip` for Auto zoom).
    Q_INVOKABLE void open(const QString& clip = QString()) { emit openRequested(clip); }

    /// Tests: run jobs on the calling thread; services instead of the app's.
    void setSynchronous(bool synchronous) { m_synchronous = synchronous; }
    void setServices(std::shared_ptr<command::CommandServices> services) { m_services = std::move(services); }
    void setController(ProjectController* controller) { m_controller = controller; }
    bool waitForIdle(int milliseconds);
    /// The job is cancelled and joined when `ai` shuts down, before its platform and transport are destroyed: the
    /// bridges' jobs use its chat and speech models and borrow its transport for model downloads (review wave 5,
    /// A5-P2-19; QML singletons have no destruction order). Bridges made by their QML factory call it themselves.
    void stopJobsWith(::AiController* ai);
    /// Tests: runs `work` as the bridge's job.
    bool runWorkerForTests(BridgeJob::Work work) { return m_job.start(this, false, std::move(work)); }

signals:
    void jobChanged();
    void resultChanged();
    void openRequested(const QString& clip);

protected:
    struct Run {
        command::Json data;
        std::vector<std::string> warnings;
        std::shared_ptr<const ProjectState> preview;
        std::optional<command::CommandError> error;
    };
    /// Runs `command` on a scratch copy of `base` (a dry run when `dryRun`), then `finish` on the GUI thread.
    bool runJob(QString phase, ProjectSnapshot base, std::string command, command::Json params, bool dryRun,
                std::function<void(Run&&)> finish, std::shared_ptr<command::CommandServices> services = nullptr);
    std::shared_ptr<command::CommandServices> services(std::shared_ptr<ai::IChatModel> chat = nullptr);
    /// Commits the staged state as one undo step labelled `label` if the session is still at the revision it was made
    /// from. Returns an error message, or "".
    QString commitStaged(const QString& label);
    void setJob(QString phase, double progress);
    void showGhost(SequenceId sequence);
    void clearGhost();

    QPointer<QQmlEngine> m_engine;
    QPointer<ProjectController> m_controller;
    std::shared_ptr<command::CommandServices> m_services;
    BridgeJob m_job;
    QPointer<::AiController> m_stopsWith;
    bool m_synchronous = false;
    QString m_phase;
    double m_progress = 0.0;
    QString m_message;
    std::shared_ptr<const ProjectState> m_staged;
    ProjectSnapshot m_stagedBase;
    std::uint64_t m_stagedRevision = 0;
};

class RoughCutBridge : public CreatorBase {
    Q_OBJECT
    /// {title, found, beatCount, words, sources, milliseconds, beats: [{beat, text, section, found, confidence, note,
    ///  start, end, cues, takes: [{take, chosen, mediaName, text, match, score, fillers, falseStarts, complete, reasons,
    ///  thumb}]}]}; empty before preview().
    Q_PROPERTY(QVariantMap report READ report NOTIFY resultChanged FINAL)
    /// The user's cloud chat model can interpret loose outlines (a model is set up, the project is not no-cloud).
    Q_PROPERTY(bool cloudAvailable READ cloudAvailable NOTIFY resultChanged FINAL)

public:
    static RoughCutBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    explicit RoughCutBridge(QObject* parent = nullptr);

    QVariantMap report() const { return m_report; }
    bool cloudAvailable() const;

    /// options: scope ("all" | "sequence"), prefer ("best" | "cleanest" | "first" | "last"), cleanup (bool), broll (bool),
    /// interpret (bool; refused without consent, see consentPrompt), allowOnce (bool), split ("auto" | "lines" |
    /// "sentences"), name.
    Q_INVOKABLE QString preview(const QString& text, const QVariantMap& options = {});
    /// The consent prompt for placing loose bullets with the cloud chat model ("interpret"): ask before preview() with
    /// interpret on; its answer comes back as options.allowOnce (or a recorded consent). Empty without a cloud model.
    Q_INVOKABLE QVariantMap consentPrompt(int characters) const;
    /// Reads a .txt / .md file the user picked (the file dialog's URL) as the script text.
    Q_INVOKABLE QString readScript(const QUrl& file);
    /// Puts take `take` (1-based) of line `beat` (1-based) into the staged cut: a staged edit.
    Q_INVOKABLE QString swapTake(int beat, int take);
    Q_INVOKABLE QString apply();
    void discard() override;

private:
    void setReport(const command::Json& data, const ProjectState& state);
    QVariantMap m_report;
    QPointer<::AiController> m_ai;
    QPointer<TranscriptBridge> m_transcripts;
    QPointer<ProjectLifecycle> m_projectFile;
    SequenceId m_cut;
};

class TakesBridge : public CreatorBase {
    Q_OBJECT
    /// [{group, text, onTimeline, improvable, confidence, takes: [{take, best, onTimeline, source, start, end, text,
    ///  diff: [{op, text}], score, fillers, falseStarts, hesitations, complete, clipped, levelDb, noiseDb,
    ///  wordsPerSecond, reasons}]}]
    Q_PROPERTY(QVariantList groups READ groups NOTIFY resultChanged FINAL)
    /// The staged edit's review list: [{group, text, take, source, replaced, removed, reasons}].
    Q_PROPERTY(QVariantList review READ review NOTIFY resultChanged FINAL)

public:
    static TakesBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    explicit TakesBridge(QObject* parent = nullptr);

    QVariantList groups() const { return m_groups; }
    QVariantList review() const { return m_review; }

    /// options: allMedia (bool: look for alternates in every recording too), minSimilarity.
    Q_INVOKABLE QString find(const QVariantMap& options = {});
    /// Plays take `take` (1-based) of `group` (1-based) in place; returns its length in seconds (the panel stops it).
    Q_INVOKABLE double audition(int group, int take);
    Q_INVOKABLE QString use(int group, int take);
    Q_INVOKABLE QString useBest();
    Q_INVOKABLE QString apply();
    void discard() override;

private:
    QString stageUse(command::Json params, const QString& phase);
    QVariantList m_groups;
    QVariantList m_review;
    command::Json m_findParams = command::Json::object();
    SequenceId m_sequence;
};

class AutoZoomBridge : public CreatorBase {
    Q_OBJECT
    /// {clip, name, origin, segments: [{start, end, zoom, x, y, reason}], keyframes, path: [{t, zoom, x, y, w, h, frame}],
    ///  events: [{t, kind, x, y}], duration}; empty before preview().
    Q_PROPERTY(QVariantMap zoomPreview READ zoomPreview NOTIFY resultChanged FINAL)

public:
    static AutoZoomBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    explicit AutoZoomBridge(QObject* parent = nullptr);

    QVariantMap zoomPreview() const { return m_preview; }

    /// options: maxZoom, hold, idle, transition, easing ("easeInOut" | "linear" | "smooth"), clicks, typing, dwell.
    Q_INVOKABLE QString preview(const QString& clip, const QVariantMap& options = {});
    Q_INVOKABLE QString apply();
    void discard() override;

private:
    QVariantMap m_preview;
    ClipId m_clip;
};

struct RoughCutBridgeForeign {
    Q_GADGET
    QML_FOREIGN(RoughCutBridge)
    QML_NAMED_ELEMENT(RoughCut)
    QML_SINGLETON
public:
    static RoughCutBridge* create(QQmlEngine* e, QJSEngine* j) { return RoughCutBridge::create(e, j); }
};

struct TakesBridgeForeign {
    Q_GADGET
    QML_FOREIGN(TakesBridge)
    QML_NAMED_ELEMENT(Takes)
    QML_SINGLETON
public:
    static TakesBridge* create(QQmlEngine* e, QJSEngine* j) { return TakesBridge::create(e, j); }
};

struct AutoZoomBridgeForeign {
    Q_GADGET
    QML_FOREIGN(AutoZoomBridge)
    QML_NAMED_ELEMENT(AutoZoom)
    QML_SINGLETON
public:
    static AutoZoomBridge* create(QQmlEngine* e, QJSEngine* j) { return AutoZoomBridge::create(e, j); }
};

} // namespace wabcut::app
