#pragma once

// EssentialSoundBridge (M5m, QML singleton `EssentialSound`): Premiere's Essential Sound panel and its Remix, over the
// A.7 commands (docs/ai.md "Essential Sound (A.7)" and "Music Remix (A.7)").
//
//   * The selection: the selected audio clips of the current sequence (a video clip stands for its linked audio). Its
//     type comes from the Essential Sound record at the top of the first clip's stack (wabcut.audio.essentialSound);
//     "mixed" when the clips differ, "" when they have none.
//   * setType / applyPreset / setOption run audio.essentialSound on the selection on a worker (loudness matching
//     renders the clip): one undo step each. The options sent are always the full set the panel shows, so a setting
//     that is off stays off when a preset turns it on by default. Sliders preview locally (previewOption) and apply
//     once, at the end of the drag (commitOptions).
//   * A section's switch turns the effects marked with that section on or off (they keep their settings, as
//     Premiere's checkbox); a marked effect the user disabled in Effect Controls shows its section off.
//   * Remix: preview() runs audio.remix as a dry run on a worker and draws the joins on the timeline (an external
//     preview: a marker at each join, the new length as a ghost); apply() commits the same request (one undo step),
//     renderTake() makes one rendered take instead, cancel() drops the preview. Any edit drops it too.
//
// Principle 9: QML sees keys ("c12"), option paths ("repair.noise") and display numbers.

#include "CommandJob.h"

#include <wabcut/command/Command.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/Ids.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut::command {
class CommandServices;
}

namespace wabcut::app {

class ProjectController;

class EssentialSoundBridge : public QObject {
    Q_OBJECT
    /// The selected audio clips ("c12"), in timeline order.
    Q_PROPERTY(QStringList clips READ clips NOTIFY selectionChanged FINAL)
    /// "dialogue" | "music" | "sfx" | "ambience"; "" when the clips have no type yet; "mixed" when they differ.
    Q_PROPERTY(QString type READ type NOTIFY selectionChanged FINAL)
    /// The preset recorded on the first clip ("default", "podcastVoice", ...), "" without a record.
    Q_PROPERTY(QString preset READ preset NOTIFY selectionChanged FINAL)
    /// The first selected clip's length in seconds (the Duration field's start), 0 without one.
    Q_PROPERTY(double clipSeconds READ clipSeconds NOTIFY selectionChanged FINAL)
    /// The presets of `type`: [{ id, name }], "default" first.
    Q_PROPERTY(QVariantList presets READ presets NOTIFY selectionChanged FINAL)
    /// The settings shown: { loudness, targetLufs, repair: { noise, enhance, rumble, hum, deess, reverb },
    /// clarity: { dynamics, eq, eqAmount }, creative: { reverb, amount }, ducking: { enabled, amount, attack, release,
    /// hold }, width, pan (a number, or undefined) }. Off is 0 / false / "" (eq, reverb).
    Q_PROPERTY(QVariantMap options READ options NOTIFY optionsChanged FINAL)
    /// Per section of the first clip's stack: "on", "off" (its marked effects are disabled) or "" (none built). Keys:
    /// the marks ("loudness", "repair.noise", "clarity.eq", ...) and the groups ("repair", "clarity", "creative").
    Q_PROPERTY(QVariantMap sections READ sections NOTIFY selectionChanged FINAL)
    /// The EQ and reverb presets: [{ id, name }] ("" = none first).
    Q_PROPERTY(QVariantList eqPresets READ eqPresets CONSTANT FINAL)
    Q_PROPERTY(QVariantList reverbPresets READ reverbPresets CONSTANT FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY jobChanged FINAL)
    Q_PROPERTY(QString phase READ phase NOTIFY jobChanged FINAL)
    /// 0..1, or -1 when unknown.
    Q_PROPERTY(double progress READ progress NOTIFY jobChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)
    /// The remix preview: { clip ("c12"), busy, phase, progress, targetSeconds, resultSeconds, errorSeconds, bpm,
    /// joins: [{ at (timeline ticks), seconds (from the clip's start), timecode, confidence (0..1), onBar }],
    /// introEnd, outroStart (source seconds), warnings: [text], error }; empty when none is shown.
    Q_PROPERTY(QVariantMap remix READ remix NOTIFY remixChanged FINAL)

public:
    explicit EssentialSoundBridge(QObject* parent = nullptr);
    ~EssentialSoundBridge() override;
    static EssentialSoundBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    void setController(ProjectController* controller);
    /// The audio services (tests inject fakes; by default the app's, appAudioServices()).
    void setServices(std::shared_ptr<command::CommandServices> services) { m_services = std::move(services); }
    /// Jobs on the calling thread (tests).
    void setSynchronous(bool synchronous);

    QStringList clips() const;
    QString type() const { return m_type; }
    QString preset() const { return m_preset; }
    double clipSeconds() const { return m_clipSeconds; }
    QVariantList presets() const;
    QVariantMap options() const { return m_options; }
    QVariantMap sections() const { return m_sections; }
    QVariantList eqPresets() const;
    QVariantList reverbPresets() const;
    bool busy() const { return m_job.running() || m_previewJob.running(); }
    QString phase() const { return m_phase; }
    double progress() const { return m_progress; }
    QString message() const { return m_message; }
    QVariantMap remix() const { return m_remix; }

    /// Tags the selection: "dialogue", "music", "sfx", "ambience" (its default preset), or "none" (clear). Returns ""
    /// when started, else why not.
    Q_INVOKABLE QString setType(const QString& type);
    /// Applies a preset of the selection's type. Returns "" or why not.
    Q_INVOKABLE QString applyPreset(const QString& preset);
    /// Sets one setting ("repair.noise", "clarity.eq", "ducking.enabled", "width", ...) and applies at once.
    Q_INVOKABLE QString setOption(const QString& path, const QVariant& value);
    /// Sets one setting for the panel only (a slider being dragged); commitOptions() applies.
    Q_INVOKABLE void previewOption(const QString& path, const QVariant& value);
    /// Applies the settings shown (the end of a slider drag). Returns "" or why not.
    Q_INVOKABLE QString commitOptions();
    /// Turns a section's marked effects on or off: a mark ("repair.noise") or a group ("repair"). One undo step.
    Q_INVOKABLE QString setSectionEnabled(const QString& section, bool enabled);
    /// Removes Essential Sound from the selection (its record and marked effects; the clips' own effects stay).
    Q_INVOKABLE QString clear();
    Q_INVOKABLE void cancelJob();

    /// Remix: previews `clipKey` remixed to `seconds` (a dry run; the timeline shows the joins). Returns "" or why not.
    Q_INVOKABLE QString preview(const QString& clipKey, double seconds);
    /// Remix: previews the clip ended with the last video clip ("fit": "video").
    Q_INVOKABLE QString previewFit(const QString& clipKey);
    /// Remix: the first selected music clip to `seconds` (the panel's Duration).
    Q_INVOKABLE QString previewSelected(double seconds);
    /// Remix: commits the previewed request (the clip becomes its pieces). One undo step.
    Q_INVOKABLE QString apply();
    /// Remix: renders the previewed request as one take (a new media item the clip plays).
    Q_INVOKABLE QString renderTake();
    /// Remix: drops the preview (and stops its analysis).
    Q_INVOKABLE void cancel();

signals:
    void selectionChanged();
    void optionsChanged();
    void jobChanged();
    void messageChanged();
    void remixChanged();

private:
    std::shared_ptr<command::CommandServices> services();
    void refresh();
    void setJob(const QString& phase, double progress);
    void setMessage(const QString& message);
    /// The selected audio clips (video clips stand for their linked audio), in timeline order.
    std::vector<ClipId> selectedAudio() const;
    /// audio.essentialSound on the selection with `params` (clips added). Returns "" when started.
    QString run(command::Json params, const char* label);
    /// The full options object the panel shows (every setting given, so presets cannot turn off-settings on).
    command::Json fullOptions() const;
    QString startPreview(command::Json params);
    void finishPreview(const command::CommandResult& result, std::uint64_t generation, const ProjectSnapshot& base);
    void setRemixProgress(const QString& phase, double progress, std::uint64_t generation);
    QString commitRemix(bool take);
    void showRemixPreview(const QVariantMap& remix);
    void clearRemixPreview();
    void onRevisionChanged();

    QPointer<ProjectController> m_controller;
    std::shared_ptr<command::CommandServices> m_services;
    std::function<std::shared_ptr<command::CommandServices>()> m_factory;
    std::shared_ptr<command::CommandServices> m_built; ///< m_factory's, for the project file m_builtFor
    std::filesystem::path m_builtFor;
    CommandJob m_job;
    BridgeJob m_previewJob;
    bool m_synchronous = false;
    std::vector<ClipId> m_clips;
    QString m_type;
    QString m_preset;
    double m_clipSeconds = 0;
    QVariantMap m_options;
    QVariantMap m_sections;
    QString m_phase;
    double m_progress = 0;
    QString m_message;

    QVariantMap m_remix;
    std::optional<command::Json> m_remixRequest; ///< the previewed audio.remix params (dryRun dropped)
    std::uint64_t m_remixGeneration = 0;
    bool m_previewShown = false; ///< this bridge put an external preview on the timeline
};

struct EssentialSoundBridgeForeign {
    Q_GADGET
    QML_FOREIGN(EssentialSoundBridge)
    QML_NAMED_ELEMENT(EssentialSound)
    QML_SINGLETON
public:
    static EssentialSoundBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return EssentialSoundBridge::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
