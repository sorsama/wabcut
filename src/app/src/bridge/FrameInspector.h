#pragma once

// FrameInspector (M5k, QML singleton `FrameInspector`): "Explain this frame" - why the program monitor shows what it
// shows at the playhead, and why the speakers play what they play.
//
// It is built on the evaluators the preview and export already run (read only, no render path of its own):
//   * video: playback::evaluateVideoLayers() (with and without occlusion culling, the program monitor's proxy and
//     caption settings), its LayerSpec effects, transitions, redactions and grades, and resolveSource() for proxies;
//   * audio: the model's evaluation helpers the track renderer and the master bus use (Clip::sourceTimeAt,
//     parameterAt, the effect stacks, the A.3 track fader / pan and master fader, unitySpeedRanges() for the C.6 audio
//     policy) and the project's redactions, as AudioRedactions reads them;
//   * captions: playback::captionTrackFor() / evaluateCaption() with the monitor's CaptionSettings.
// explainFrame() is the pure function behind it (tests compare it with the evaluator); the bridge refreshes it while
// `active` (the panel is on screen) when the playhead, the project or the sequence changes.
//
// Preview-only overrides: "solo this layer" and "bypass this effect" change what the monitor and the speakers play
// WITHOUT changing the project. The bridge builds the overridden state in a private EditSession and points the
// program monitor's PlaybackController and TimelineAudioPlayer at it (the same scheduler and track renderer, the same
// code path); clearing the overrides points them back at ProjectController's session. The project's EditSession is
// never written: no undo step, no revision, no dirty flag (a test asserts it). The overrides follow edits (the private
// state is rebuilt from every new snapshot) and are dropped when the sequence or the project changes.
//
// Principle 9: QML sees keys ("c12", "e5", "t3", "master") and display text (timecode, "sample 96 000 at 48 kHz").

#include <wabcut/model/EditSession.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Time.h>
#include <wabcut/playback/video/Evaluate.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class PlaybackBridge;
class ProjectController;

/// What is overridden for the preview (none of it reaches the project).
struct PreviewOverrides {
    /// Clips shown alone among the layers of their kind (video clips hide the other video clips, audio clips mute the
    /// other audio clips).
    std::set<ClipId> soloClips;
    /// Effect instances bypassed: (owner, effect). Owner: a clip, a track (its track stack) or invalid = the master
    /// audio stack.
    std::set<std::pair<std::uint64_t, std::uint64_t>> bypassed;

    bool empty() const { return soloClips.empty() && bypassed.empty(); }
    bool isBypassed(std::uint64_t owner, EffectInstanceId effect) const { return bypassed.contains({owner, effect.value}); }
    bool operator==(const PreviewOverrides&) const = default;
};

/// `project` with `overrides` applied to `sequence` (shares every node it does not change).
ProjectState applyPreviewOverrides(const ProjectState& project, SequenceId sequence, const PreviewOverrides& overrides);

/// One effect of a stack as the evaluator treats it.
struct ExplainedEffect {
    enum class State { Applied, Disabled, Unsupported, PreviewBypassed };
    EffectInstanceId id;
    std::string effect;   ///< effect id, e.g. "wabcut.color.grade"
    QString name;         ///< display name
    State state = State::Applied;
    /// Masks limiting it (I.6), as names ("Face (subtract, inverted)"); the opacity mask lists its shapes too.
    QStringList masks;
    /// 0.2: what the evaluated effect does at this frame, for effects that need it explained: Border ("8 px, #FFFFFF,
    /// 100 % opacity"), Ultra Key (key colour, output, matte generation and cleanup, spill), Track Matte Key (the matte
    /// track and how it is read, or why the layer shows nothing). Empty otherwise.
    QString detail;
};

/// A colour-grade stage (color::GradeSpec, in the shader's order).
struct GradeStage {
    QString name;
    bool active = false;
};

struct ExplainedVideoLayer {
    ClipId clip;
    TrackId track;
    QString trackName;  ///< "V2"
    QString clipName;
    QString mediaName;
    enum class Source { Original, Proxy, ProxyForOffline, Generated };
    Source source = Source::Original;
    QString sourceFile; ///< file name decoded ("" for generated layers)
    Ticks sourceTime;   ///< the frame shown (LayerSpec::sourceTime)
    Ticks curveTime;    ///< where its parameters and keyframes are evaluated
    std::int64_t sourceFrame = 0; ///< frame number in the media's rate
    QString sourceTimecode;       ///< in the media's rate
    QString mapping;              ///< "100 % forward", "Freeze frame", "Time remap: segment 2 of 3, ramp, reverse"
    bool remapped = false;
    /// Transition in progress: "Cross Dissolve", its progress (0..1) and this layer's side.
    std::optional<QString> transition;
    double transitionProgress = 0.0;
    QString transitionSide; ///< "outgoing", "incoming", "one-sided"
    std::vector<ExplainedEffect> effects;
    /// Effects the evaluator ran (LayerSpec::effects.size()) and bypassed (LayerSpec::bypassedEffects).
    int evaluatedEffects = 0;
    int bypassedEffects = 0;
    std::vector<GradeStage> grade; ///< empty without an applied grade
    int missingLuts = 0;
    /// Redactions covering its source time (names) and how many the evaluator applies (LayerSpec::redactions).
    QStringList redactions;
    int evaluatedRedactions = 0;
    double opacity = 1.0;
    /// 0.2: the clip's blend mode when it is not Normal ("Multiply"), else empty.
    QString blendMode;
    /// 0.2: set when this layer is another layer's matte or second picture, not drawn itself: "Track Matte of V2", or
    /// "Second picture of V2's Gradient Wipe".
    QString matteFor;
    render::LayerTransform transform;
    /// Fully covered by an opaque layer above (the preview does not even decode it).
    bool occluded = false;
    bool soloed = false;
};

struct ExplainedGain {
    QString stage; ///< "Clip volume", "Clip pan", "Track fader", "Track pan", "Master fader"
    QString value; ///< "-3.0 dB", "L 20", "Centre", "Muted"
};

struct ExplainedAudioLayer {
    ClipId clip;
    TrackId track;
    QString trackName; ///< "A1"
    QString clipName;
    QString mediaName;
    Ticks sourceTime;
    std::int64_t sourceSample = 0; ///< at the stream's sample rate
    int sampleRate = 0;
    QString sourceTimecode;
    /// Why nothing is heard ("" when it plays): track muted, speed policy, disabled clip, ...
    QString silentReason;
    std::vector<ExplainedGain> gain;
    std::vector<ExplainedEffect> clipEffects;
    std::vector<ExplainedEffect> trackEffects;
    std::optional<QString> transition; ///< audio transition in progress
    QStringList redactions;            ///< "Licence plate: bleep"
    bool soloed = false;
};

struct ExplainedCaption {
    bool shown = false;      ///< the monitor draws captions (the Captions panel's "show in program")
    QString trackName;
    QString text;
    QString inText;
    QString outText;
    QString timing;          ///< where the timing comes from
};

struct FrameExplanation {
    Ticks time;
    std::vector<ExplainedVideoLayer> video;  ///< top (highest track) first, as they stack on screen
    std::vector<ExplainedAudioLayer> audio;  ///< A1 first
    std::vector<ExplainedEffect> masterEffects;
    std::optional<ExplainedCaption> caption;
};

/// Explains `sequence` at `time` the way the preview evaluates it (`preview`: the monitor's proxy settings;
/// `captions`: what it burns in, or null). `overrides` only mark rows (the explanation is of the project).
FrameExplanation explainFrame(const ProjectState& project, const Sequence& sequence, Ticks time,
                              const playback::PreviewSettings& preview, const playback::CaptionSettings* captions,
                              const PreviewOverrides& overrides = {});

class FrameInspector : public QObject {
    Q_OBJECT
    /// The panel is on screen: refresh on playhead / project changes (otherwise nothing is evaluated).
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged FINAL)
    Q_PROPERTY(QString timecode READ timecode NOTIFY explanationChanged FINAL)
    /// [{ key ("c12"), track, clipName, mediaName, source ("original"|"proxy"|"proxyOffline"|"generated"), sourceText,
    ///    sourceFile, sourceTimecode, sourceFrame (text), mapping, remapped, transition ("" or "Cross Dissolve 40 %,
    ///    outgoing"), effects: [{ key ("e5"), owner ("c12"), name, state ("applied"|"disabled"|"unsupported"|
    ///    "previewBypassed"), stateText, masks (text), detail (0.2) }], grade: [{ name, active }], gradeText,
    ///    redactions (text), opacityText, motionText, blendText (0.2: "" for Normal), matteText (0.2: "" unless the layer is
    ///    another layer's matte), occluded, soloed }], top first.
    Q_PROPERTY(QVariantList layers READ layers NOTIFY explanationChanged FINAL)
    /// [{ key, track, clipName, mediaName, sampleText, sourceTimecode, silentReason, gain: [{ stage, value }],
    ///    effects: [...as above, clip then track], transition, redactions (text), soloed }]
    Q_PROPERTY(QVariantList audio READ audio NOTIFY explanationChanged FINAL)
    /// The master audio stack: [{ key, owner ("master"), name, state, stateText }].
    Q_PROPERTY(QVariantList masterEffects READ masterEffects NOTIFY explanationChanged FINAL)
    /// { shown, track, text, inText, outText, timing } or an empty map.
    Q_PROPERTY(QVariantMap caption READ caption NOTIFY explanationChanged FINAL)
    /// Preview-only overrides are active (the monitor does not show the project as it is).
    Q_PROPERTY(bool previewing READ previewing NOTIFY overridesChanged FINAL)
    Q_PROPERTY(int overrideCount READ overrideCount NOTIFY overridesChanged FINAL)

public:
    explicit FrameInspector(QObject* parent = nullptr);
    ~FrameInspector() override;
    static FrameInspector* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    void setController(ProjectController* controller);
    /// The program monitor's playback (its proxy and caption settings; the overrides retarget its players).
    void setPlayback(PlaybackBridge* playback);

    bool active() const { return m_active; }
    void setActive(bool active);
    QString timecode() const { return m_timecode; }
    QVariantList layers() const { return m_layers; }
    QVariantList audio() const { return m_audio; }
    QVariantList masterEffects() const { return m_master; }
    QVariantMap caption() const { return m_caption; }
    bool previewing() const { return !m_overrides.empty(); }
    int overrideCount() const;

    /// The last explanation (C++ / tests).
    const FrameExplanation& explanation() const { return m_explanation; }
    const PreviewOverrides& overrides() const { return m_overrides; }
    /// The state the monitor plays while overrides are active (null otherwise; tests).
    ProjectSnapshot previewSnapshot() const;

    /// Explains the frame at the playhead now.
    Q_INVOKABLE void refresh();
    /// Solo / unsolo a layer ("c12") for the preview.
    Q_INVOKABLE void toggleSolo(const QString& clipKey);
    /// Bypass / restore an effect for the preview. `owner`: a clip ("c12"), a track ("t3") or "master".
    Q_INVOKABLE void toggleBypass(const QString& owner, const QString& effectKey);
    Q_INVOKABLE void clearOverrides();

signals:
    void activeChanged();
    void explanationChanged();
    void overridesChanged();

private:
    void scheduleRefresh();
    void publish();
    void applyOverrides();
    void retarget(const EditSession* session);

    QPointer<ProjectController> m_controller;
    QPointer<PlaybackBridge> m_playback;
    bool m_active = false;
    bool m_refreshQueued = false;
    SequenceId m_sequence;
    FrameExplanation m_explanation;
    QString m_timecode;
    QVariantList m_layers;
    QVariantList m_audio;
    QVariantList m_master;
    QVariantMap m_caption;
    PreviewOverrides m_overrides;
    /// The overridden state the players follow while previewing. Kept alive for the bridge's lifetime once made: the
    /// players' threads may still read it for a pass after they are pointed back at the project.
    std::unique_ptr<EditSession> m_previewSession;
    bool m_retargeted = false;
};

/// QML registration through create().
struct FrameInspectorForeign {
    Q_GADGET
    QML_FOREIGN(FrameInspector)
    QML_NAMED_ELEMENT(FrameInspector)
    QML_SINGLETON
public:
    static FrameInspector* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return FrameInspector::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
