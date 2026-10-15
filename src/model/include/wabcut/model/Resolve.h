#pragma once

// Resolving nested and multicam clips to the media they show and play (C.8; docs/internal/c8-nested-multicam-handoff.md).
//
// Render and audio evaluate a sequence clip by clip. A clip of media is a leaf; a nested clip (Clip::sequence) stands
// for what its sequence shows or plays at the nested time its timing maps to, and a multicam clip for its active
// angles only. resolveLeaves() walks that down to leaf clips, composing the time maps exactly (each level is
// Clip::sourceTimeAt of the level above: speed, reverse, freeze and time remaps, integer ticks), and says for each leaf
// which nested clips lead to it (`path`, outermost first) so the evaluator can apply effects in order:
//   leaf clip (intrinsic + effect stack) -> its track (track effects; audio: fader, pan) -> composite / mix of the
//   nested sequence's tracks (V1 up; audio summed) -> the nested sequence's master stage (video effects; audio effects
//   and master fader; not for a multicam source) -> the nested clip (intrinsic + effects, keyframes at its own curve
//   time) -> its track -> ... -> the top sequence.
// Within one nested level the leaves come in track order (V1 first, A1 first), then time.
//
// Which tracks a level reads: a nested sequence's tracks of the clip's kind whose output is on (video: Track::enabled;
// audio: not muted, and only soloed tracks while any is soloed); a multicam clip only its angles' tracks, whatever
// their flags. Disabled clips and clips of unknown source kinds give nothing; a nested clip past its sequence's end
// gives nothing. Transitions: inside a transition region of a track both clips are active, each leaf (and each level
// of `path`) records the transition and how far it is (0..1) - the evaluator blends them as it does on the top level.
//
// Redactions (Masks.h) are anchored to media: a leaf gets every enabled redaction of its media whose range holds the
// media time it shows - its `source` for a video leaf; its `audioRanges` (or `source` when none) for an audio leaf
// with audio treatment - unless the redaction excepts the leaf clip or any clip of its path. So a redacted face or name
// stays redacted however it is nested, and excepting the nested clip excepts everything inside it.
//
// Caching: a ProjectState never changes, so a resolution keyed on (state revision pointer, sequence, clip id, time)
// stays valid; evaluators usually cache per sequence revision the list of (nested clip -> its sequence's tracks) and
// re-run resolveLeaves per frame (cost: O(depth x tracks x log clips)). ChangeSet::sequences names the sequences an
// edit changed: invalidate the cache of every sequence that contains one of them (sequenceContains, Nesting.h).

#include <wabcut/model/Blend.h>
#include <wabcut/model/ProjectState.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace wabcut {

/// Deeper nesting than this resolves to nothing (a cycle cannot exist in a valid project; this bounds broken ones).
inline constexpr std::size_t kMaxNestingDepth = 64;

/// A clip taking part in a transition at the resolved time.
struct TransitionUse {
    const Transition* transition = nullptr; ///< null when the clip is not in a transition there
    bool incoming = false;                  ///< the clip is the transition's incoming (B) side
    double progress = 0.0;                  ///< 0 at the transition's start, 1 at its end
};

/// One nested or multicam clip on the way down to a leaf.
struct NestLevel {
    SequenceId sequence; ///< the sequence that holds the nested clip
    TrackId track;
    std::size_t trackIndex = 0; ///< 0 = V1 / A1 of `sequence`
    const Clip* clip = nullptr; ///< the nested clip (alive as long as the ProjectState)
    Ticks time;                 ///< the time in `sequence`
    Ticks nestedTime;           ///< the time it shows in its own sequence (clip->sourceTimeAt(time))
    Ticks curveTime;            ///< its keyframes' time (clip->curveTimeAt(time))
    TransitionUse transition;
};

/// A media clip a resolution reached.
struct ResolvedLeaf {
    SequenceId sequence; ///< the sequence that holds the leaf
    TrackId track;
    std::size_t trackIndex = 0; ///< 0 = V1 / A1 of `sequence`
    const Clip* clip = nullptr; ///< a media clip (alive as long as the ProjectState)
    Ticks time;                 ///< the time in `sequence`
    Ticks sourceTime;           ///< the media time it shows / plays: clip->sourceTimeAt(time)
    Ticks curveTime;            ///< its keyframes' time: clip->curveTimeAt(time)
    TransitionUse transition;
    std::vector<NestLevel> path; ///< the nested clips leading to it, outermost first (empty for a top-level leaf)
    std::vector<RedactionId> redactions; ///< the redactions that apply to it at sourceTime
};

/// The leaves `clip` (a clip of `sequence`) shows or plays at time `t` of `sequence`: the clip itself when it plays
/// media, else what its nested sequence or multicam angles hold at the mapped time (see above). `t` may lie in a
/// transition handle outside the clip.
std::vector<ResolvedLeaf> resolveLeaves(const ProjectState& state, SequenceId sequence, const Clip& clip, Ticks t);

/// Every leaf a sequence shows (`kind` Video) or plays (Audio) at time `t`: resolveLeaves over each track of that kind
/// whose output is on, with the clips active at `t` there (both clips inside a transition).
std::vector<ResolvedLeaf> resolveSequenceLeaves(const ProjectState& state, SequenceId sequence, StreamKind kind, Ticks t);

/// The tracks a nested or multicam `clip` reads from its sequence `nested` by the rules above, as indices into
/// nested.tracks() of the clip's kind in compositing / mixing order (V1 / A1 first): a multicam clip its active angles'
/// tracks whatever their flags, a nested clip the tracks whose output is on. resolveLeaves() descends through these; the
/// audio renderer mixes them (a nested level's track effects and faders run on each track's sum). Empty for a clip
/// that is not nested.
std::vector<std::size_t> nestedTracksRead(const Sequence& nested, const Clip& clip);

// ---- The composite plan (C.15: adjustment layers, blend modes, track mattes) ----------------------------------------
//
// resolveComposite() says, for one time of a sequence, what the compositor does, in order: start from a transparent
// canvas and run the steps bottom to top (V1 first; inside a track, a transition's outgoing clip before its incoming
// one). docs/internal/c15-parity-handoff.md has the pixel math.
//   Layer   a clip's picture: a media leaf, or a nested / multicam clip whose `nested` plan is rendered on its own canvas
//           first (and then its sequence's master stage). The layer gets its effects (Track Matte Key: `matte`), then
//           its Motion, and is blended over the accumulated canvas with `blend` at alpha x `opacity`.
//   Adjust  an adjustment layer clip: take the accumulated canvas, apply the clip's Motion and its effect stack (masks
//           limit each effect, keyframes at `curveTime`), then blend the adjusted canvas over the unadjusted one with
//           `blend` at `opacity` (Normal at 1 = replace). It adjusts only what is below it in its own sequence level:
//           inside a nest, the nest's lower tracks.
// Steps in a transition carry `transition`: blend layers as the top level does; an adjustment step fades its result in
// (incoming: opacity x progress) or out (outgoing: opacity x (1 - progress)) whatever the transition kind.
// Which tracks a level reads follows resolveLeaves (output on; a multicam clip its angles' tracks); a matte track is
// read whatever its output switch. Bounded like resolveLeaves (kMaxNestingDepth, a visit budget: `truncated`).

enum class CompositeStepKind : std::uint8_t { Layer, Adjust };

struct CompositePlan;

/// A Track Matte Key on a layer (the clip's first enabled "wabcut.key.trackMatte" instance).
struct MatteUse {
    const EffectInstance* effect = nullptr; ///< alive as long as the ProjectState
    int trackNumber = 0;                    ///< 1-based video track number, as the effect's "matteTrack" says
    TrackId track;                          ///< invalid: no such track (or the layer's own track) - the key shows nothing
    bool luma = false;                      ///< composite using the matte's luma (else its alpha)
    bool reverse = false;
    /// The matte: that track's own clips at this time on a transparent canvas (a plan of one track; nested and
    /// adjustment clips there resolve as anywhere). Null when `track` is invalid.
    std::shared_ptr<const CompositePlan> plan;
};

struct CompositeStep {
    CompositeStepKind kind = CompositeStepKind::Layer;
    SequenceId sequence;        ///< the level's sequence
    TrackId track;
    std::size_t trackIndex = 0; ///< 0 = V1 of `sequence`
    const Clip* clip = nullptr; ///< alive as long as the ProjectState
    Ticks time;                 ///< the time in `sequence`
    Ticks sourceTime;           ///< clip->sourceTimeAt(time) (a nested clip: the time in its sequence)
    Ticks curveTime;            ///< clip->curveTimeAt(time): its keyframes' time
    TransitionUse transition;
    double opacity = 1.0;       ///< the clip's Opacity at curveTime, 0..1
    BlendMode blend = BlendMode::Normal;
    /// Layer of a nested or multicam clip: what its sequence shows at sourceTime. Null for media.
    std::shared_ptr<const CompositePlan> nested;
    std::optional<MatteUse> matte;
    /// Layer of media: the redactions covering it (as ResolvedLeaf::redactions).
    std::vector<RedactionId> redactions;

    bool isAdjustment() const { return kind == CompositeStepKind::Adjust; }
};

struct CompositePlan {
    SequenceId sequence;
    Ticks time;
    std::vector<CompositeStep> steps; ///< bottom to top
    /// A nested level's master video stack (not for a multicam source): applied to the level's composite after the
    /// steps. Null at the top level and when empty.
    const EffectStack* masterEffects = nullptr;
    bool truncated = false; ///< the visit budget ran out: steps after it are missing

    /// Steps of every level, nested plans and mattes included (diagnostics, tests).
    std::size_t stepCount() const;
};

/// The composite plan of `sequence` at time `t` (see above). An empty plan for an unknown sequence.
CompositePlan resolveComposite(const ProjectState& state, SequenceId sequence, Ticks t);

} // namespace wabcut
