#pragma once

// MomentsBridge: one-click "moments" for gaming clips (M5o, singleton `Moments`): speed presets on the existing time
// remapping commands and zoom presets as ordinary Motion keyframes. Every preset is one undo step; the result is
// plain remap keys and keyframes, editable afterwards in the Time Remapping line and Effect Controls.
//
//   * The target: the clip named, else the selected clip under the playhead, else the topmost video clip under the
//     playhead (an audio clip hands over to its linked video). The moment is the playhead when it is inside the clip,
//     else the clip's middle. Everything is clamped inside the clip (a frame in from each end) and scaled down when
//     the clip is short; a clip too short for it is refused.
//   * Speed (timeRemap.enable / addKey / setSpeed / addHold / setInterpolation / setKey, run on a copy of the project
//     and committed as one step; linked audio follows the picture, as the commands do, so it stays in sync; it is
//     heard only where it plays at exactly 100 %, so a slowed stretch is silent until audible slow motion lands):
//       - slowMo(percent, seconds): normal -> slow -> normal around the moment, the slow stretch `seconds` of footage
//         centred on the frame at the moment, eased in and out by three constant-speed steps of about 0.16 s each
//         side (straight segments, so the audio before and after stays audible);
//       - rampIn(percent, seconds): the `seconds` of footage before the moment fast, stepping down to 100 % at it;
//       - freezeReplay(freeze, replay, percent): the frame at the moment holds for `freeze` seconds, then the last
//         `replay` seconds of action play again at `percent`, then the clip carries on at 100 %;
//       - fastForward(percent): In to Out when both marks are inside the clip, else 3 s from the moment, at `percent`.
//     The clip grows (or shrinks) by the time the remap gains (loses), moving later clips like a ripple trim. Keys
//     end on frames (a step's speed may round a little, e.g. 43.75 % -> 40 %), so the clip's end stays on a frame.
//   * Zoom (clip.addKeyframe on motion.scale / motion.position.*, relative to the value at the moment):
//       - punchIn(percent, hold): -> `percent` in about 0.12 s, held 1 s, back in 0.2 s; with `hold` it stays in;
//       - shake(pixels): a decaying position wiggle of 8 steps (about 45 ms, a frame at least) ending where it started.
//   * 0.4 instant replay (C.16 clip.instantReplay): In to Out when both sequence marks are set and the range overlaps
//     the target clip (the command's range mode: every clip of media in it, linked audio and facecam too); else, on a
//     clip longer than 20 s, the 5 s before the playhead; else the whole clip. Speed (25 / 50 %), sound and the REPLAY
//     stinger are remembered (QSettings moments/replaySpeed, moments/replaySound, moments/replayLabel).
//   * Keymap: moments.slowMo and moments.punchIn (runCommand), no default keys.

#include <wabcut/model/Ids.h>
#include <wabcut/model/Time.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <optional>

class QJSEngine;
class QQmlEngine;

namespace wabcut::command {
class CommandRegistry;
class CommandServices;
}

namespace wabcut::app {
class BridgeJob;
}

namespace wabcut::app {

class ProjectController;

class MomentsBridge : public QObject {
    Q_OBJECT
    /// What a preset would act on now: { available, clip ("c12"), name, video (it can zoom), speed (it can be
    /// retimed), speedReason (why not), atText (the moment's timecode), reason (why nothing, when unavailable) }.
    Q_PROPERTY(QVariantMap target READ target NOTIFY targetChanged FINAL)
    /// The last refusal or error ("" after a preset applied), for a toast.
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)
    /// C.16's gaming edits the catalog has: { replay (clip.instantReplay), version (sequence.makeVersion), deadTime
    /// (sequence.removeDeadTime) }. Their entries show only when there.
    Q_PROPERTY(QVariantMap gamingEdits READ gamingEdits NOTIFY catalogChanged FINAL)
    /// Remove dead time's preview: { busy, found (ranges are waiting for Remove), count, seconds, text, error }.
    Q_PROPERTY(QVariantMap deadTime READ deadTime NOTIFY deadTimeChanged FINAL)
    /// 0.4: instant replay's options (QSettings moments/replaySpeed 0.25 | 0.5, default 0.5; moments/replaySound,
    /// default false: the replay's audio, slowed; moments/replayLabel, default true: the REPLAY stinger).
    Q_PROPERTY(double replaySpeed READ replaySpeed WRITE setReplaySpeed NOTIFY replayOptionsChanged FINAL)
    Q_PROPERTY(bool replaySound READ replaySound WRITE setReplaySound NOTIFY replayOptionsChanged FINAL)
    Q_PROPERTY(bool replayLabel READ replayLabel WRITE setReplayLabel NOTIFY replayOptionsChanged FINAL)

public:
    explicit MomentsBridge(QObject* parent = nullptr);
    ~MomentsBridge() override;
    static MomentsBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setController(ProjectController* controller);

    QVariantMap target() const { return targetFor({}); }
    QString message() const { return m_message; }
    /// `target` for a clip ("c12"; "" = the target above): what a preset from its menu would act on.
    Q_INVOKABLE QVariantMap targetFor(const QString& clip) const;

    /// Each returns "" when it applied (one undo step), else why not (also in `message`). `clip` = "" for the
    /// target above.
    Q_INVOKABLE QString slowMo(double percent = 25.0, double seconds = 1.0, const QString& clip = {});
    Q_INVOKABLE QString rampIn(double percent = 300.0, double seconds = 1.0, const QString& clip = {});
    Q_INVOKABLE QString freezeReplay(double freezeSeconds = 0.5, double replaySeconds = 2.0, double percent = 50.0,
                                     const QString& clip = {});
    Q_INVOKABLE QString fastForward(double percent = 300.0, const QString& clip = {});
    Q_INVOKABLE QString punchIn(double percent = 130.0, bool hold = false, const QString& clip = {});
    Q_INVOKABLE QString shake(double pixels = 14.0, const QString& clip = {});
    /// C.16 clip.instantReplay: copied right after the range at replaySpeed with a ramp, the REPLAY stinger
    /// (replayLabel) and a white flash at each cut (later material moves). One undo step. 0.4: the range rules in the
    /// header comment; `options` { speed, includeAudio, label } override the remembered ones (scripts, tests).
    Q_INVOKABLE QString instantReplay(const QString& clip = {}, const QVariantMap& options = {});
    double replaySpeed() const { return m_replaySpeed; }
    /// Clamped to 0.25 or 0.5 (the nearer); written to QSettings at once.
    void setReplaySpeed(double speed);
    bool replaySound() const { return m_replaySound; }
    void setReplaySound(bool on);
    bool replayLabel() const { return m_replayLabel; }
    void setReplayLabel(bool on);
    /// C.16 sequence.makeVersion of the current sequence: "shorts" (1080x1920) or "square"; the new sequence opens.
    Q_INVOKABLE QString makeVersion(const QString& format = QStringLiteral("shorts"));
    QVariantMap gamingEdits() const;
    QVariantMap deadTime() const { return m_deadTime; }
    /// C.16 sequence.removeDeadTime's preview on the current sequence, off the GUI thread (it measures the gameplay's
    /// motion and the game audio): `deadTime` lists what Remove would take out.
    Q_INVOKABLE void findDeadTime();
    /// Removes the previewed stretches (handles kept, ripple on every unlocked track): one undo step.
    Q_INVOKABLE QString removeDeadTime();
    Q_INVOKABLE void cancelDeadTime();
    /// Tests: run the preview on the calling thread.
    void setSynchronous(bool synchronous) { m_synchronous = synchronous; }
    /// Tests: the catalog the C.16 edits run on (nullptr = the app's).
    void setRegistry(const command::CommandRegistry* registry);
    /// By name, for menus: "slowMo25", "slowMo50", "rampIn", "freezeReplay", "fastForward200" / "300" / "400",
    /// "punchIn", "punchInHold", "shake", "instantReplay", "shortsVersion", "squareVersion". options.clip: the clip
    /// ("" = the target).
    Q_INVOKABLE QString apply(const QString& preset, const QVariantMap& options = {});
    /// moments.slowMo (25 %, 1 s) and moments.punchIn (130 %): true when the id is one of them.
    Q_INVOKABLE bool runCommand(const QString& commandId);

signals:
    void targetChanged();
    void messageChanged();
    void catalogChanged();
    void deadTimeChanged();
    /// 0.4: replaySpeed, replaySound or replayLabel changed.
    void replayOptionsChanged();

private:
    struct Target {
        ClipId clip;
        Ticks at;
    };
    /// The clip and moment a preset acts on, or why there is none (`speed`: it retimes, so it needs footage).
    std::optional<Target> resolve(const QString& clipKey, bool speed, QString* why) const;
    QString finish(bool ok, const QString& failure);
    void setMessage(const QString& message);

    const command::CommandRegistry& registry() const;
    /// The C.16 edits' host services: the template library and measurer, the media activity and Auto Reframe.
    std::shared_ptr<command::CommandServices> services() const;

    QPointer<ProjectController> m_controller;
    QString m_message;
    const command::CommandRegistry* m_registry = nullptr;
    std::unique_ptr<BridgeJob> m_deadTimeJob;
    QVariantMap m_deadTime;
    /// The previewed ranges as sequence.removeDeadTime takes them back ([{start, end}] in ticks), as JSON text.
    std::string m_deadTimeRanges;
    std::uint64_t m_deadTimeSequence = 0;
    bool m_synchronous = false;
    /// 0.4: instant replay's remembered options (read from QSettings in the constructor).
    double m_replaySpeed = 0.5;
    bool m_replaySound = false;
    bool m_replayLabel = true;
};

struct MomentsSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::MomentsBridge)
    QML_NAMED_ELEMENT(Moments)
    QML_SINGLETON
public:
    static MomentsBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return MomentsBridge::create(qmlEngine, jsEngine); }
};

} // namespace wabcut::app
