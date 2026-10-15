#pragma once

// AI-13 podcast and interview editing in the app (docs/ai.md "Podcast and interview editing"): two QML singletons over
// multicam.autoSwitch and the podcast workflow, built like the creator bridges (CreatorBridge.h): a job runs on a
// scratch copy of the project on a worker, and the live session changes only on apply, as one undo step guarded by the
// revision the preview was made at. Everything runs on this computer: no cloud, no cost.
//
//   AutoSwitch       the "Auto switch" dialog of a multicam clip: preset (Podcast calm, Interview, Energetic), every
//                    rule as a slider or toggle, the angle <-> speaker mapping with each camera's face as a thumbnail
//                    (the guesses, which the user may confirm or change), and preview(): the cut list (time, angle,
//                    explanation) drawn over the timeline as ghosts; apply() adds the angle cuts as one undo step.
//   PodcastWorkflow  the "Podcast workflow" sheet: the recordings (import files or pick from the project; cameras and
//                    microphones), the options, and the step checklist (sync, lay out, transcribe, Enhance Speech,
//                    loudness, clean-up, switching, captions, chapters, shorts). Each step can be unticked, previewed
//                    (staged, the timeline ghost) and applied as its own undo step, or skipped; runAll() runs the rest in
//                    turn. After each applied step the project is checkpointed in its version history (C.9) and the run
//                    journal is written next to it, so resume() continues after a crash.

#include "CreatorBridge.h"

#include <wabcut/ai/workflow/Podcast.h>

#include <QList>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace wabcut::ai::multicam {
class AngleSignals;
}

namespace wabcut::app {

/// The project's media signals for the speaker analysis (each angle's microphone level, the faces of the app's one
/// subject provider), one per project file for the session: levels are decoded once.
std::shared_ptr<ai::multicam::AngleSignals> appAngleSignals(const std::filesystem::path& projectFile);

/// What the AI-13 commands get in the app: the creator services (transcripts, audio, subjects, ...), the speaker
/// signals (each angle's microphone level, the faces of the app's one subject provider), audio sync for multicam.create,
/// Enhance Speech on this computer and local transcription.
std::shared_ptr<command::CommandServices> multicamServices(QQmlEngine* engine, TranscriptBridge* transcripts,
                                                           const std::filesystem::path& projectFile);

class AutoSwitchBridge : public CreatorBase {
    Q_OBJECT
    /// The presets: [{id, name, description, minHold, lead, overlapMin, silenceWide, varietyAfter, varietyLength,
    ///  reactions, reactionStrength, reactionHold, openWide, openLength, wideOnOverlap, wideOnLaughter, avoidMidWord,
    ///  maxSnap}] (seconds).
    Q_PROPERTY(QVariantList presets READ presets CONSTANT FINAL)
    /// After preview(): {clip, name, preset, start, duration, speakers: [{index, key, name, seconds, mic}], angles: [{angle,
    ///  name, role, speakers, confidence, confirmed, reason, frame, face: {x, y, w, h}}], cuts: [{seconds, offset, time,
    ///  angle, angleName, reason, explanation, confidence, moved}], stats, notes}.
    Q_PROPERTY(QVariantMap result READ result NOTIFY resultChanged FINAL)

public:
    static AutoSwitchBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    explicit AutoSwitchBridge(QObject* parent = nullptr);

    QVariantList presets() const;
    QVariantMap result() const { return m_result; }

    /// The rule values of a preset ("calm", "interview", "energetic"), in seconds.
    Q_INVOKABLE QVariantMap presetValues(const QString& preset) const;
    /// The first multicam clip under the playhead (else the sequence's first), as a clip key; "" without one.
    Q_INVOKABLE QString defaultClip() const;
    /// Whether `clip` ("c<id>", in the current sequence) is a multicam clip (the clip menu's Auto Switch…).
    Q_INVOKABLE bool isMulticam(const QString& clip) const;
    /// options: preset, the rules (as presetValues), target ("video" | "both"), faces (false: quicker), angles: [{angle (1-based), role
    /// ("closeUp" | "wide" | "unknown"), speaker (key or name)}].
    Q_INVOKABLE QString preview(const QString& clip, const QVariantMap& options = {});
    Q_INVOKABLE QString apply();
    void discard() override;

private:
    QVariantMap m_result;
    ClipId m_clip;
};

class PodcastBridge : public CreatorBase {
    Q_OBJECT
    /// The recordings in the run: [{key, id, name, kind ("camera" | "mic"), seconds}].
    Q_PROPERTY(QVariantList recordings READ recordings NOTIFY recordingsChanged FINAL)
    /// The project's media that can be added: [{key, id, name, hasVideo, hasAudio, seconds, added}].
    Q_PROPERTY(QVariantList library READ library NOTIFY recordingsChanged FINAL)
    /// The steps: [{step, index, title, detail, command, enabled, heavy, reason, status ("todo" | "running" | "staged" |
    ///  "done" | "skipped" | "failed"), summary}].
    Q_PROPERTY(QVariantList steps READ steps NOTIFY stepsChanged FINAL)
    Q_PROPERTY(QString cost READ cost CONSTANT FINAL)
    /// runAll() is going through the steps.
    Q_PROPERTY(bool running READ running NOTIFY stepsChanged FINAL)
    Q_PROPERTY(int doneCount READ doneCount NOTIFY stepsChanged FINAL)
    Q_PROPERTY(int enabledCount READ enabledCount NOTIFY stepsChanged FINAL)
    /// The step being previewed or run ("" none).
    Q_PROPERTY(QString current READ current NOTIFY stepsChanged FINAL)
    /// The project has an unfinished run (its journal).
    Q_PROPERTY(bool canResume READ canResume NOTIFY stepsChanged FINAL)
    /// The run's options (as setOptions takes them).
    Q_PROPERTY(QVariantMap options READ options NOTIFY stepsChanged FINAL)
    /// The YouTube chapters text, once the chapters step ran.
    Q_PROPERTY(QString youtubeChapters READ youtubeChapters NOTIFY stepsChanged FINAL)
    /// The episode sequence ("s<id>"), once laid out.
    Q_PROPERTY(QString episode READ episode NOTIFY stepsChanged FINAL)

public:
    static PodcastBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    explicit PodcastBridge(QObject* parent = nullptr);

    QVariantList recordings() const;
    QVariantList library() const;
    QVariantList steps() const;
    QString cost() const { return QStringLiteral("local, free"); }
    bool running() const { return m_runAll; }
    int doneCount() const;
    int enabledCount() const;
    QString current() const { return m_current ? QString::fromLatin1(ai::workflow::toString(*m_current)) : QString(); }
    bool canResume() const;
    QVariantMap options() const;
    QString youtubeChapters() const { return QString::fromStdString(m_context.youtubeChapters); }
    QString episode() const;

    /// Imports files into the project (ProjectFile.importFiles) and adds them: with a picture a camera, else a mic.
    Q_INVOKABLE QString importFiles(const QList<QUrl>& files);
    /// Adds a project media item ("m<id>"); `kind` "camera" | "mic" | "" (by its streams).
    Q_INVOKABLE void addMedia(const QString& media, const QString& kind = QString());
    Q_INVOKABLE void removeMedia(const QString& media);
    /// name, sync, enhanceStrength, lufs, fillers, silences, switchPreset, captionPreset, maxChapters,
    /// minChapterSeconds, shorts, shortsCount.
    Q_INVOKABLE void setOptions(const QVariantMap& options);
    /// Ticks or unticks a step.
    Q_INVOKABLE void setStepEnabled(const QString& step, bool enabled);
    /// Runs one step on a scratch copy: staged (the timeline ghost), waiting for applyStep() or discard().
    Q_INVOKABLE QString previewStep(const QString& step);
    /// Applies the staged step as its own undo step, checkpoints the project's history and writes the journal.
    Q_INVOKABLE QString applyStep();
    Q_INVOKABLE QString skipStep(const QString& step);
    /// Runs every remaining enabled step in turn, each applied as its own undo step (stop with cancel()).
    Q_INVOKABLE QString runAll();
    /// Continues the project's unfinished run: its recordings, options and progress from the journal, the project from the
    /// last history checkpoint when the open project has lost the steps (one undo step).
    Q_INVOKABLE QString resume();
    /// Starts over (the journal is kept until the next applied step).
    Q_INVOKABLE void reset();
    /// Opens the episode in the timeline.
    Q_INVOKABLE void openEpisode();
    void discard() override;
    Q_INVOKABLE void cancel();

    /// Tests: the journal and history next to this file instead of the project's.
    void setProjectFileForTests(std::filesystem::path file) { m_projectFileOverride = std::move(file); }

signals:
    void recordingsChanged();
    void stepsChanged();

private:
    ai::workflow::PodcastPlan plan() const;
    /// The services the steps run with (the app's, built once per project file; or the tests').
    std::shared_ptr<command::CommandServices> stepServices() const;
    std::filesystem::path projectFile() const;
    QString startStep(ai::workflow::StepKind kind);
    void finishStep(ai::workflow::StepKind kind, std::optional<ai::workflow::StepResult> result, QString error,
                    ai::workflow::PodcastContext context);
    void record(ai::workflow::StepKind kind, const std::string& status, const std::string& revision = {});
    QString continueRun();

    ai::workflow::PodcastOptions m_options;
    ai::workflow::PodcastContext m_context;
    ai::workflow::PodcastContext m_stagedContext;
    std::map<ai::workflow::StepKind, std::string> m_status;
    std::map<ai::workflow::StepKind, QString> m_summary;
    std::optional<ai::workflow::StepKind> m_current;
    std::optional<ai::workflow::StepKind> m_stagedStep;
    ai::workflow::Journal m_journal;
    bool m_runAll = false;
    std::filesystem::path m_projectFileOverride;
    QString m_lastProject;
    mutable std::shared_ptr<command::CommandServices> m_built;
    mutable std::filesystem::path m_builtFor;
    // The last plan and what it was made from (plan()).
    mutable ai::workflow::PodcastPlan m_plan;
    mutable ProjectSnapshot m_planState;
    mutable std::shared_ptr<command::CommandServices> m_planServices;
    mutable std::string m_planKey;
    mutable std::chrono::steady_clock::time_point m_plannedAt;
};

struct AutoSwitchBridgeForeign {
    Q_GADGET
    QML_FOREIGN(AutoSwitchBridge)
    QML_NAMED_ELEMENT(AutoSwitch)
    QML_SINGLETON
public:
    static AutoSwitchBridge* create(QQmlEngine* e, QJSEngine* j) { return AutoSwitchBridge::create(e, j); }
};

struct PodcastBridgeForeign {
    Q_GADGET
    QML_FOREIGN(PodcastBridge)
    QML_NAMED_ELEMENT(PodcastWorkflow)
    QML_SINGLETON
public:
    static PodcastBridge* create(QQmlEngine* e, QJSEngine* j) { return PodcastBridge::create(e, j); }
};

} // namespace wabcut::app
