#pragma once

// Finishing operations: keyframes, effect stacks, transitions, speed and frame holds. Same contract as
// EditOps.h: pure functions from a state to a new state, failing with an Error and changing nothing; locked
// tracks are never changed; every result passes validate().
//
// Keyframe and effect edits touch exactly one clip's content block (copy-on-write: other clips that shared
// it, such as the other half of a razor cut, keep theirs), one track's stack or one of the sequence's master
// stacks (EffectTarget). Clip keyframe times are curve times (media time, see Parameters.h); commands convert
// timeline times with Clip::curveTimeAt. Track and master keyframe times are timeline times.

#include <wabcut/model/EditOps.h>
#include <wabcut/model/Effects.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace wabcut::edit {

// ---------------------------------------------------------------------------------------------------
// Parameters and keyframes

/// A parameter of a clip: an intrinsic one (ClipProperties) or a parameter of an effect in its stack. 0.6: or a
/// keyframable number of a graphic clip's layer (`layer` its id, `key` the path: GraphicOps.h layerParam), whose keys
/// are in graphic time (GraphicResolve.h graphicTimeAt; the clip's curve time unless the graphic has responsive time).
struct ParamRef {
    std::optional<ParamId> intrinsic;
    EffectInstanceId effect;
    std::string key;
    std::uint32_t layer = 0;

    static ParamRef of(ParamId id) { return {id, {}, {}, 0}; }
    static ParamRef ofEffect(EffectInstanceId effect, std::string key) { return {std::nullopt, effect, std::move(key), 0}; }
    static ParamRef ofLayer(std::uint32_t layer, std::string path) { return {std::nullopt, {}, std::move(path), layer}; }
};

/// Replaces a parameter's value (a constant or a curve). Values must lie in the parameter's range; video
/// parameters belong to video clips and audio parameters to audio clips; a non-animatable effect parameter
/// takes constants only.
EditOutcome setParameter(const ProjectState& state, SequenceId sequence, ClipId clip, const ParamRef& param,
                         ParamValue value);

/// Adds a key, or replaces the key at the same time. A constant parameter becomes a curve holding just
/// this key.
EditOutcome addKeyframe(const ProjectState& state, SequenceId sequence, ClipId clip, const ParamRef& param,
                        Keyframe key);
/// Moves key `index` to curve time `time`; fails if another key is there. Keys stay sorted, so the moved
/// key's index may change.
EditOutcome moveKeyframe(const ProjectState& state, SequenceId sequence, ClipId clip, const ParamRef& param,
                         std::size_t index, Ticks time);
/// Moves keys `indices` (each listed once) together by `delta` curve ticks. The delta is clamped so that no
/// moved key leaves the clip's media range (the curve times from its start to its end; a key already outside
/// does not move further out); EditResult::appliedDelta is the delta used. A moved key landing on a key that
/// does not move replaces it (the moved key wins). Keys stay sorted, so indices may change.
EditOutcome moveKeyframes(const ProjectState& state, SequenceId sequence, ClipId clip, const ParamRef& param,
                          std::span<const std::size_t> indices, Ticks delta);
/// Removes key `index`. Removing the last key leaves the parameter constant at that key's value.
EditOutcome removeKeyframe(const ProjectState& state, SequenceId sequence, ClipId clip, const ParamRef& param,
                           std::size_t index);

struct KeyframeChange {
    std::optional<double> value;
    std::optional<Interpolation> interpolation;
    std::optional<BezierHandle> in;
    std::optional<BezierHandle> out;
};
/// Changes key `index`'s value, interpolation or Bezier handles.
EditOutcome setKeyframe(const ProjectState& state, SequenceId sequence, ClipId clip, const ParamRef& param,
                        std::size_t index, const KeyframeChange& change);

/// Premiere's stopwatch. On: a constant becomes a curve with one key at `curveTime` holding the constant.
/// Off: a curve becomes the constant it has at `curveTime` (all keys are deleted). No-op when already so.
EditOutcome setAnimated(const ProjectState& state, SequenceId sequence, ClipId clip, const ParamRef& param,
                        bool animated, Ticks curveTime);

// ---------------------------------------------------------------------------------------------------
// Effect stacks

/// Adds an effect to each clip (each gets its own instance, appended or inserted at `index`): a known,
/// non-intrinsic effect of the clip's kind (video effects on video clips). Parameters not given take the
/// descriptor's defaults. The new instance ids are in EditResult::createdEffects, in clip order.
EditOutcome addEffect(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips,
                      const std::string& effect, std::vector<EffectParam> params = {},
                      std::optional<std::size_t> index = std::nullopt);
EditOutcome removeEffect(const ProjectState& state, SequenceId sequence, ClipId clip, EffectInstanceId effect);
/// Moves an instance to position `index` in its stack (clamped to the end).
EditOutcome moveEffect(const ProjectState& state, SequenceId sequence, ClipId clip, EffectInstanceId effect,
                       std::size_t index);

struct EffectChange {
    std::optional<bool> enabled;
    /// Parameters to set (constants or curves); others keep their values.
    std::vector<EffectParam> params;
    /// Data entries to set (EffectInstance::setData: an empty value removes the key); others keep theirs. Unknown
    /// effects keep their data as they are, like their parameters. Entries must pass effectDataProblem.
    std::vector<EffectData> data;
};
/// Enables/disables an instance or sets its parameters. Unknown effects (kept from a newer or foreign
/// project) can be enabled, disabled, moved and removed, but their parameters are left as they are.
EditOutcome setEffect(const ProjectState& state, SequenceId sequence, ClipId clip, EffectInstanceId effect,
                      const EffectChange& change);

// ---------------------------------------------------------------------------------------------------
// Effect stacks of any target: a clip, a track or the sequence's master buses

/// Where an effect stack lives. A track's stack is applied to the composited (video) or mixed (audio) track;
/// the master stacks to the sequence's composited video (Sequence::effects) and its mix (Sequence::audioEffects).
/// Keyframe times on track and master effects are timeline times (clip effects use curve time, Parameters.h).
struct EffectTarget {
    /// 0.6: Media = a media item's Master Clip Effects (MediaItem::masterEffects; the sequence is not used): video and
    /// audio effects (the kinds the item has streams of), keyframe times in media time.
    enum class Kind : std::uint8_t { Clip, Track, MasterVideo, MasterAudio, Media };
    Kind kind = Kind::Clip;
    ClipId clip;   ///< Kind::Clip
    TrackId track; ///< Kind::Track
    MediaId media; ///< Kind::Media

    static EffectTarget ofClip(ClipId clip) { return {Kind::Clip, clip, {}, {}}; }
    static EffectTarget ofTrack(TrackId track) { return {Kind::Track, {}, track, {}}; }
    static EffectTarget masterVideo() { return {Kind::MasterVideo, {}, {}, {}}; }
    static EffectTarget masterAudio() { return {Kind::MasterAudio, {}, {}, {}}; }
    static EffectTarget ofMedia(MediaId media) { return {Kind::Media, {}, {}, media}; }
    bool operator==(const EffectTarget&) const = default;
};

/// The same operations on any target. A clip target behaves exactly like the clip overloads above. A track
/// target needs an unlocked track and takes effects of the track's kind; the master targets take video
/// (MasterVideo) or audio (MasterAudio) effects. Track and master parameters are effect parameters
/// (ParamRef::ofEffect), plus the faders (A.3): ParamRef::of(ParamId::VolumeDb) and ParamRef::of(ParamId::Pan) on
/// an audio track (Track::volume, Track::pan) and ParamRef::of(ParamId::VolumeDb) on MasterAudio
/// (Sequence::masterVolume), in timeline time. Other intrinsic parameters belong to clips.
EditOutcome addEffect(const ProjectState& state, SequenceId sequence, const EffectTarget& target,
                      const std::string& effect, std::vector<EffectParam> params = {},
                      std::optional<std::size_t> index = std::nullopt);
EditOutcome removeEffect(const ProjectState& state, SequenceId sequence, const EffectTarget& target,
                         EffectInstanceId effect);
EditOutcome moveEffect(const ProjectState& state, SequenceId sequence, const EffectTarget& target,
                       EffectInstanceId effect, std::size_t index);
EditOutcome setEffect(const ProjectState& state, SequenceId sequence, const EffectTarget& target,
                      EffectInstanceId effect, const EffectChange& change);
EditOutcome setParameter(const ProjectState& state, SequenceId sequence, const EffectTarget& target,
                         const ParamRef& param, ParamValue value);
EditOutcome addKeyframe(const ProjectState& state, SequenceId sequence, const EffectTarget& target,
                        const ParamRef& param, Keyframe key);
EditOutcome moveKeyframe(const ProjectState& state, SequenceId sequence, const EffectTarget& target,
                         const ParamRef& param, std::size_t index, Ticks time);
EditOutcome removeKeyframe(const ProjectState& state, SequenceId sequence, const EffectTarget& target,
                           const ParamRef& param, std::size_t index);
EditOutcome setKeyframe(const ProjectState& state, SequenceId sequence, const EffectTarget& target,
                        const ParamRef& param, std::size_t index, const KeyframeChange& change);
EditOutcome setAnimated(const ProjectState& state, SequenceId sequence, const EffectTarget& target,
                        const ParamRef& param, bool animated, Ticks curveTime);

/// The effect stack of a target in a sequence (null when the clip or track does not exist).
const EffectStack* effectStackOf(const Sequence& sequence, const EffectTarget& target);

// Master Clip Effects (0.6, card #71; MasterClipEffects.h). The target operations above work on
// EffectTarget::ofMedia (one undo step on the media item). The clip operations above that name an effect of the clip's
// media's master stack (and not of the clip's own) act on that master stack, so Effect Controls edits a master effect
// through the clip it shows it with.

/// The stack of any target in a project: a media item's master stack, else effectStackOf(sequence).
const EffectStack* effectStackOf(const ProjectState& state, SequenceId sequence, const EffectTarget& target);
/// Moves a clip's effect onto its media's Master Clip Effects (last; its keys and masks are media time, the same for
/// every instance), and back: a master effect becomes the first of the clip's own effects (it still plays before
/// them). One step each. Nested and multicam clips have no master clip effects.
EditOutcome moveEffectToMaster(const ProjectState& state, SequenceId sequence, ClipId clip, EffectInstanceId effect);
EditOutcome moveEffectToClip(const ProjectState& state, SequenceId sequence, ClipId clip, EffectInstanceId effect);
/// Turns the master clip effects off for these clips (they play their own effects only), or on again.
EditOutcome setMasterEffectsOff(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips, bool off);

// ---------------------------------------------------------------------------------------------------
// Transitions

/// Where a transition sits relative to the cut (Track.h; stored on the transition).
using wabcut::TransitionAlignment;

/// A transition on one edit point (Track.h): two-sided (`outgoing` ends where `incoming` starts) or
/// one-sided (`incoming` invalid with after == 0: at A's tail; `outgoing` invalid with before == 0: at B's
/// head).
struct TransitionSpec {
    ClipId outgoing;
    ClipId incoming;
    Ticks before;
    Ticks after;
    std::string effect = "crossDissolve";
    /// Constant parameters (e.g. a wipe's "angle").
    std::vector<EffectParam> params;
    /// Two-sided: Center / StartAtCut / EndAtCut re-split before + after that way; Custom keeps them as given;
    /// nullopt: the alignment they describe (alignmentOf). One-sided transitions are EndAtCut at A's tail and
    /// StartAtCut at B's head.
    std::optional<TransitionAlignment> alignment;
};

/// What to do when the media lacks the handles a two-sided transition needs.
enum class HandleFit {
    Exact,        ///< fail, naming the clip and how much media it has
    UseAvailable, ///< shorten each side to the media there is (Premiere's insufficient-media adjustment)
};

/// Adds a transition. Fails when the clips do not meet, a side is longer than its clip, the media lacks the
/// handles (HandleFit::Exact), it overlaps another transition or sits on an edit point that has one, the
/// track is locked, or the effect is not a known transition of the track's kind. The new id is in
/// EditResult::createdTransitions. A side shortened to the available handles makes the alignment the one the
/// new sides describe (usually Custom) unless they still fit it.
EditOutcome addTransition(const ProjectState& state, SequenceId sequence, TrackId track, const TransitionSpec& spec,
                          HandleFit fit = HandleFit::Exact);

/// Unset fields keep their value. Sides given explicitly (`before` / `after`) set the alignment to the one they
/// describe (alignmentOf), unless `alignment` is Custom. Otherwise a new `duration` and/or `alignment` re-splits
/// the transition by its (new) alignment; Custom keeps the cut where it is (`before`, at most the length).
struct TransitionChange {
    std::optional<Ticks> before;
    std::optional<Ticks> after;
    std::optional<Ticks> duration;
    std::optional<TransitionAlignment> alignment;
    std::optional<std::string> effect;
    std::optional<std::vector<EffectParam>> params;
};
EditOutcome setTransition(const ProjectState& state, SequenceId sequence, TransitionId transition,
                          const TransitionChange& change, HandleFit fit = HandleFit::Exact);
EditOutcome removeTransition(const ProjectState& state, SequenceId sequence, TransitionId transition);

struct EdgeTransition {
    TrackId track;
    TransitionSpec spec;
};
/// The transition for a clip's edge: two-sided when the neighbouring clip meets the edge, split by `alignment`
/// (Custom: half before the cut, like Center), otherwise one-sided inside the clip (EndAtCut at a tail,
/// StartAtCut at a head).
std::expected<EdgeTransition, Error> transitionAtEdge(const ProjectState& state, SequenceId sequence, ClipId clip,
                                                      Edge edge, Ticks duration, TransitionAlignment alignment,
                                                      std::string effect);

struct DefaultTransitions {
    std::string video = "crossDissolve";
    std::string audio = "constantPower";
    /// nullopt: one second (whole frames at the sequence's nominal rate).
    std::optional<Ticks> videoDuration;
    std::optional<Ticks> audioDuration;
    TransitionAlignment alignment = TransitionAlignment::Center;
};

/// Apply Video Transition (Ctrl/Cmd+D) / Apply Audio Transition (Ctrl/Cmd+Shift+D): the default transition on
/// the edit point nearest the playhead on each unlocked targeted track of that kind, shortened to the
/// available handles. Edit points that already have a transition are left alone. Fails if none could take
/// one.
EditOutcome applyDefaultTransition(const ProjectState& state, SequenceId sequence, Ticks playhead, TrackKind kind,
                                   const DefaultTransitions& defaults = {});
/// Apply Default Transitions to Selection (Shift+D): default transitions on both edges of each clip (the
/// video default on video clips, the audio default on audio clips), shortened to the available handles.
EditOutcome applyDefaultTransitions(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips,
                                   const DefaultTransitions& defaults = {});

/// 0.6: a selected edit point (Premiere's trim selection, TrimBridge) as a place for a transition.
struct EdgePoint {
    ClipId clip;
    Edge edge = Edge::Tail;
    /// nullopt: the defaults' alignment (a roll point holds both sides of the cut). A point on one side only takes
    /// EndAtCut at a tail and StartAtCut at a head.
    std::optional<TransitionAlignment> alignment;
};
/// Apply Video / Audio Transition (Ctrl/Cmd+D, Ctrl/Cmd+Shift+D) and an Effects-panel transition with edit points
/// selected: a transition on each point whose track is of `kind`, shortened to the available handles, two-sided when
/// another clip meets the edge (transitionAtEdge). `effect` ("" = the default of that kind) replaces the default. A cut
/// shared by two points gets one transition; points on locked tracks or that already have one are left alone. One
/// result; fails when none could take one.
EditOutcome applyTransitionsAtEdges(const ProjectState& state, SequenceId sequence, std::span<const EdgePoint> points,
                                    TrackKind kind, const DefaultTransitions& defaults = {},
                                    const std::string& effect = {});

// ---------------------------------------------------------------------------------------------------
// Beat grid (Sequence::beatGrid): what beat-snapping, edit.snapToBeats and edit.cutToBeats use

/// Why `grid` is not a valid beat grid (tempo 1..1000 BPM, 1..32 beats per bar, downbeat >= 0, explicit beats
/// sorted, >= 0 and including the downbeat), or nullopt.
std::optional<std::string> beatGridProblem(const BeatGrid& grid);
/// Sets (or, with nullopt, removes) the sequence's beat grid. Errors: NotFound, InvalidArgument.
EditOutcome setBeatGrid(const ProjectState& state, SequenceId sequence, std::optional<BeatGrid> grid);

// ---------------------------------------------------------------------------------------------------
// Speed and frame holds

struct SpeedChange {
    Speed speed;                        ///< positive (a frame hold is frameHold())
    std::optional<bool> reversed;       ///< nullopt: unchanged
    std::optional<bool> maintainPitch;  ///< nullopt: unchanged (audio)
    /// Premiere's "Ripple Edit, Shifting Trailing Clips": later material on the clip's track and on
    /// sync-locked tracks moves by the change in length. Off: the clip may only grow into the gap after it.
    bool ripple = false;
    /// Speed and duration unlinked (the chain icon in Premiere's Speed/Duration dialog): each clip keeps its
    /// start and length and plays more or less of its media from the same source In (the source Out moves).
    /// Fails when the media ends before the new Out. `ripple` does not apply. Importers use it to retime a
    /// placed clip without moving its neighbours.
    bool keepDuration = false;
};

/// Speed/Duration (Ctrl/Cmd+R). Each clip (and, with FollowLinks, its partners) keeps its source In and plays
/// the same source material at the new speed: the new length is the source length divided by the speed, in
/// whole frames (at least one). Frame holds cannot be retimed.
EditOutcome setSpeed(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips,
                     const SpeedChange& change, LinkMode links = LinkMode::FollowLinks);

/// The speed at which `sourceDuration` of media lasts `timelineDuration` on the timeline: the largest
/// representable ratio not above the exact one (so the clip never reads past its media). nullopt for
/// non-positive durations.
std::optional<Speed> speedForDuration(Ticks sourceDuration, Ticks timelineDuration);

/// Frame hold on a video clip at timeline time `at`.
///   duration nullopt: Premiere's "Add Frame Hold": the clip is cut at `at` and the rest holds that frame.
///   duration given: "Insert Frame Hold Segment": a hold of that length is inserted at `at`, rippling the rest
///   of the track and the sync-locked tracks (their material at `at` is cut through, like an insert).
/// The hold keeps the clip's content: its parameters and effects keep animating (Clip::curveTimeAt).
EditOutcome frameHold(const ProjectState& state, SequenceId sequence, ClipId clip, Ticks at,
                      std::optional<Ticks> duration = std::nullopt);

// ---------------------------------------------------------------------------------------------------
// Time remapping (TimeRemap.h): speed ramps, frame-hold and reverse segments on one clip. Times are the clip's
// curve times (Clip::curveTimeAt; commands convert timeline times); keys outside the clip are kept (trims show
// them). Every edit keeps the clip's length and every frame it shows inside its media (InvalidArgument), and
// fails on a clip that is not remapped (except setTimeRemapEnabled).

/// Turns time remapping on or off.
///   On: a remap that reproduces the clip's timing. At 100 %: the identity over the whole media (keys at its start
///   and end; at the clip's ends when the media's length is unknown), so trims still reach all of it. Other
///   speeds and reversed: one straight segment at that speed and direction reaching as far into the media as it goes
///   (keys on the speed's exact grid), so the frames inside the clip and beyond its ends (transition handles, trims)
///   stay the same tick for tick.
///   A freeze frame: one key. The clip then has speed 100 % and its sourceIn becomes a curve time (unchanged in
///   value: keyframes of a clip that was retimed then animate at timeline rate).
///   Off: a constant speed again. If the clip shows a single straight segment, its speed (reversed for a reverse
///   segment) or, on video, a freeze frame for a hold; otherwise 100 % from the frame at the clip's start. Fails
///   if that would play past the media (trim the clip first).
EditOutcome setTimeRemapEnabled(const ProjectState& state, SequenceId sequence, ClipId clip, bool enabled);
/// Turns time remapping on with `remap` (replacing any) and `curveIn` as the curve time at the clip's start (its new
/// sourceIn, >= 0): importers, pasting a remap. Fails on a bad curve or one that shows frames outside the media.
EditOutcome setTimeRemap(const ProjectState& state, SequenceId sequence, ClipId clip, Curve remap, Ticks curveIn);
/// Adds a key at `time` with the sampled source position: Hold inside a hold, otherwise Linear. A new linear key
/// can reshape a smooth ramp; this is not shape-preserving Bezier subdivision. No-op when a key is there.
EditOutcome addTimeRemapKey(const ProjectState& state, SequenceId sequence, ClipId clip, Ticks time);
/// Moves key `index` to `time`. Without ripple it stays between its neighbours and the segments on both sides
/// change speed. With ripple the keys after it move by as much (Premiere's speed keyframe drag): only the
/// segment before it changes speed and the later material plays earlier or later.
EditOutcome moveTimeRemapKey(const ProjectState& state, SequenceId sequence, ClipId clip, std::size_t index,
                             Ticks time, bool ripple = false);
struct TimeRemapKeyChange {
    std::optional<Ticks> source;                ///< a new position (source ticks)
    std::optional<Interpolation> interpolation; ///< Hold: a step; ease / Bezier: a speed ramp around the key
};
EditOutcome setTimeRemapKey(const ProjectState& state, SequenceId sequence, ClipId clip, std::size_t index,
                            const TimeRemapKeyChange& change);
/// Removes key `index`; the only key cannot go (turn remapping off instead).
EditOutcome removeTimeRemapKey(const ProjectState& state, SequenceId sequence, ClipId clip, std::size_t index);
/// Sets the speed of segment `index` (key index to key index + 1; positive, the direction stays): key index + 1
/// and the keys after it move so the segment lasts its source length / speed (whole ticks, at least one).
EditOutcome setTimeRemapSpeed(const ProjectState& state, SequenceId sequence, ClipId clip, std::size_t index,
                              Speed speed);
/// Frame-hold segment: the frame shown at `time` holds for `duration`; the rest of the remap moves later by it.
EditOutcome addTimeRemapHold(const ProjectState& state, SequenceId sequence, ClipId clip, Ticks time,
                             Ticks duration);
/// Reverse segment (Premiere's reverse speed keyframe): from `time` the media plays backwards at 100 % for
/// `duration`, then forwards at 100 % over the same frames back to where it was; the rest of the remap moves
/// later by twice `duration`. Fails inside a reverse segment and with less than `duration` of media before.
EditOutcome addTimeRemapReverse(const ProjectState& state, SequenceId sequence, ClipId clip, Ticks time,
                                Ticks duration);

// ---------------------------------------------------------------------------------------------------
// Caption tracks

/// Adds a caption track (when `track.id` is invalid: a new id is allocated and returned in the result's
/// `createdCaptionTrack`) or replaces the track with that id. Cues must be sorted, non-overlapping and positive.
EditOutcome setCaptionTrack(const ProjectState& state, SequenceId sequence, CaptionTrack track);
EditOutcome removeCaptionTrack(const ProjectState& state, SequenceId sequence, CaptionTrackId track);

} // namespace wabcut::edit
