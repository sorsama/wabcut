#pragma once

// Auditions (0.6, card #74): a take stack at a cut - Final Cut Pro's Auditions, DaVinci Resolve's Take Selector.
//
// An audition is an ordinary clip that holds alternatives for the same place in the edit: other performances, line
// readings, music cues or graphics versions. The clip itself is the active take - its media (or nested sequence /
// multicam source), source In, speed, name, parameters and effects are the active take's - so playback, export, nested
// sequences, interchange and every edit see exactly the active take and nothing else. The other takes ride in the
// clip's content block (ClipContent::audition) with their own source, name, parameters and effects.
//
// All takes share the audition's timeline range. A take is stored as it would play over that whole range (its source
// In at the audition's start); a take that runs out of media before the range ends (or starts after it begins) is
// *short*: while it is active the clip covers only the part it has media for, and the audition remembers how much it
// left uncovered (headTrim / tailTrim), so making a longer take active again gives the cut back its full length (as far
// as the track is still free there).
//
// Edits treat an audition like any clip: moves, ripples, slips, slides and copies leave its takes where they are
// (relative to the clip); trims and razor cuts change every take with the clip (Clip::moveHead, moveTail and splitOff
// keep them in step), and a razor cut gives both pieces the whole stack. A clip's linked partners that are auditions with
// the same number of takes (the picture and sound of camera takes) form one audition: the ops below change all of them
// together, take i of one being take i of the others.

#include <wabcut/model/Clip.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/Time.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut {

struct ProjectState;
struct Track;
struct Sequence;

/// Most takes one audition holds (the validator's bound; files are untrusted).
inline constexpr std::size_t kMaxAuditionTakes = 32;
/// Longest take name kept (the clip name bound of the UI).
inline constexpr std::size_t kMaxTakeNameBytes = 256;

/// One take of an audition that is not the active one: what the clip plays when it becomes active.
struct AuditionTake {
    MediaId media;
    StreamRef stream;
    /// A nested sequence or multicam source instead of media (Clip::sequence), with its angles.
    SequenceId sequence;
    AngleSet angles;
    /// Source time at the audition's start, as Clip::sourceIn over the audition's whole range (direction-aware). It may
    /// lie outside the media: the take is short there.
    Ticks sourceIn;
    Speed speed;
    bool reversed = false;
    bool maintainPitch = false;
    /// Name, parameters, effects, time remap... of the take (never an audition of its own). Null: default content.
    std::shared_ptr<const ClipContent> content;

    const ClipContent& contentOrDefault() const;
    const std::string& name() const { return contentOrDefault().name; }
    bool isNested() const { return sequence.isValid(); }
    bool operator==(const AuditionTake& other) const;
};

/// A clip's take stack (ClipContent::audition). Immutable and shared like the rest of the content block.
struct Audition {
    /// The takes other than the active one (the clip), in take order.
    std::vector<AuditionTake> others;
    /// Where the active take (the clip) sits in take order: 0..others.size().
    std::uint32_t active = 0;
    /// The part of the audition's range the active take leaves uncovered before the clip's start and after its end
    /// (a short take); 0 when it fills the range.
    Ticks headTrim;
    Ticks tailTrim;

    std::size_t size() const { return others.size() + 1; }
    bool operator==(const Audition& other) const = default;
};

/// The clip's take stack, or null for a clip that is not an audition.
const Audition* auditionOf(const Clip& clip);
/// Number of takes (1 for a clip that is not an audition).
std::size_t takeCount(const Clip& clip);
/// The audition's whole timeline range: the clip widened by its head and tail trims (the clip's range when it is
/// not an audition).
TimeRange auditionRange(const Clip& clip);
/// Take `index` (take order, < takeCount) as a clip over the audition's whole range: the active take is the clip
/// itself widened by the trims. Same id, links and colour as the clip; its content holds no audition. It may play
/// past its media (a short take): use materializedTake for what the clip would become.
Clip auditionTakeClip(const Clip& clip, std::size_t index);
/// The name of take `index` (the clip's name for the active take).
const std::string& takeName(const Clip& clip, std::size_t index);
/// `take` as a clip over `range` (no id, links or colour): what it plays there.
Clip takeClip(const AuditionTake& take, TimeRange range);
/// A take of a clip (for edits that build or change stacks): `clip`'s source and content as an AuditionTake, the
/// content without an audition.
AuditionTake takeFromClip(const Clip& clip);

/// Calls fn(MediaId, SequenceId) for the source of every take of `clip` (the clip's own included): what deleting
/// media or a sequence, merging projects and copying clips must keep.
template <class Fn>
void forEachTakeSource(const Clip& clip, Fn&& fn) {
    fn(clip.media, clip.sequence);
    if (const Audition* a = auditionOf(clip)) {
        for (const AuditionTake& t : a->others) {
            fn(t.media, t.sequence);
        }
    }
}
/// Whether any take of `clip` plays `media` / `sequence`.
bool clipUsesMedia(const Clip& clip, MediaId media);
bool clipUsesSequence(const Clip& clip, SequenceId sequence);

/// The audition's take stack with every take's media and sequence ids passed through `map` (merging and pasting
/// between projects): `map(MediaId&, SequenceId&)` rewrites them in place. Returns the clip unchanged when it is
/// not an audition.
template <class Map>
Clip remapAuditionSources(Clip clip, Map&& map);

/// What is wrong with a clip's take stack on a track of `kind`, or nullopt (validate() reports it): at least two
/// takes and at most kMaxAuditionTakes, the active index in range, trims >= 0, every take's source present with a
/// stream of the track's kind, valid speed, parameters and effects, no take holding an audition.
std::optional<std::string> auditionProblem(const ProjectState& state, StreamKind kind, const Clip& clip);

/// How a take fits the audition's range: the timeline ticks it is short at the head and at the tail (0 = it has media
/// there), rounded up to frames of `frameRate`. Unbounded media (stills, generators, a freeze frame) are never short.
struct TakeFit {
    Ticks head;
    Ticks tail;
    bool fills() const { return head.value == 0 && tail.value == 0; }
};
TakeFit takeFit(const ProjectState& state, const Clip& clip, std::size_t index, Rate frameRate);

/// Where the linked partners of an audition clip that are auditions with the same number of takes sit (the picture and
/// sound of a camera take): the clip first, then the others in track order. Locked tracks are skipped.
std::vector<ClipId> auditionPartners(const Sequence& sequence, ClipId clip);

namespace edit {

/// A media item or sequence to add as a take (Add Source Monitor Clip to Audition): its In (source time at the
/// audition's start; for a sequence, in its time) and, empty = the source's name, the take's name.
struct TakeSource {
    MediaId media;
    SequenceId sequence;
    Ticks sourceIn;
    std::string name;
};

/// Clip > Auditions > New Audition (Final Cut's Create Audition): the clips (with their linked partners) become the takes
/// of one audition at the place of the first of them (the earliest; at the same time the lowest track), which is take
/// 1 and the active take; the others follow in timeline order, each aligned to the audition's start with its own In
/// point, and leave the timeline (no ripple). A take of several clips (a picture with its linked sound) keeps them in
/// sync; every take must have as many picture and sound clips as the first. A selected audition joins with all its
/// takes. One clip alone becomes an audition with a copy of itself, the copy active (Duplicate as Audition). One undo
/// step. Refuses clips on locked tracks, takes of different shapes and more than kMaxAuditionTakes takes.
EditOutcome createAudition(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips);
/// Add to Audition: the clips (with their linked partners, not the audition's own) become takes after the last one,
/// aligned to its start, and leave the timeline. The active take stays. Same shape rules as createAudition. A clip that
/// is not an audition yet becomes one. Without `alignStarts` each take keeps its timeline position instead (what it
/// plays at each moment: the interchange importers place the takes where they play).
EditOutcome addAuditionTakes(const ProjectState& state, SequenceId sequence, ClipId audition,
                             std::span<const ClipId> clips, bool alignStarts = true);
/// Add Source Monitor Clip to Audition: a media item (its streams of the audition's kinds, in order) or sequence becomes
/// a take after the last one, its In at the audition's start. Refuses a source without a picture (sound) for a picture
/// (sound) audition.
EditOutcome addAuditionSource(const ProjectState& state, SequenceId sequence, ClipId audition, const TakeSource& source);
/// Use this take: take `index` becomes the active take of the audition (and its partners). The clip keeps its place,
/// id and links; it covers the audition's range as far as the take has media there and the track is free (a short take
/// leaves the rest empty, remembered for the next switch). Transitions at its edges stay where the take still has the
/// media they need, else they go. The playhead does not move. One undo step; the active take again is a no-op.
EditOutcome setActiveTake(const ProjectState& state, SequenceId sequence, ClipId audition, std::size_t index);
/// Removes take `index`. The active take passes to the next take (the previous one for the last); an audition left
/// with one take becomes a plain clip of it.
EditOutcome removeAuditionTake(const ProjectState& state, SequenceId sequence, ClipId audition, std::size_t index);
/// Moves take `from` to place `to` in take order (the active take stays active).
EditOutcome moveAuditionTake(const ProjectState& state, SequenceId sequence, ClipId audition, std::size_t from,
                             std::size_t to);
/// Renames take `index` (on every partner; the clip's name for the active take). Refuses an empty name and names past
/// kMaxTakeNameBytes.
EditOutcome renameAuditionTake(const ProjectState& state, SequenceId sequence, ClipId audition, std::size_t index,
                               std::string name);
/// Duplicate Take: a copy of take `index` ("<name> copy", with new effect ids) right after it, and the copy becomes the
/// active take - to try other effects on it (Final Cut's Duplicate as Audition).
EditOutcome duplicateAuditionTake(const ProjectState& state, SequenceId sequence, ClipId audition, std::size_t index);
/// Flatten Audition (Final Cut's Finalize Audition): the auditions among `clips` (with their partners) keep only their
/// active take and become plain clips. Clips that are not auditions are ignored; none at all is refused.
EditOutcome flattenAuditions(const ProjectState& state, SequenceId sequence, std::span<const ClipId> clips);

} // namespace edit

// --- Template definitions ----------------------------------------------------------------------------------------

template <class Map>
Clip remapAuditionSources(Clip clip, Map&& map) {
    const Audition* a = auditionOf(clip);
    if (!a) {
        return clip;
    }
    Audition next = *a;
    for (AuditionTake& t : next.others) {
        map(t.media, t.sequence);
    }
    if (!(next == *a)) {
        ClipContent content = clip.content();
        content.audition = std::make_shared<const Audition>(std::move(next));
        clip.setContent(std::move(content));
    }
    return clip;
}

} // namespace wabcut
