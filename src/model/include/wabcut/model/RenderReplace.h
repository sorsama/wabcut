#pragma once

// 0.7 (card 069): Render and Replace for nested sequences (Premiere's "Render and Replace", and Restore Unrendered).
//
// A nested clip (Nesting.h) plays a sequence live: every frame composites the nest's tracks again. Render and Replace
// bakes it: the nested sequence is rendered once to a media file (the export machinery, any export preset), and the
// clip plays that file from then on, keeping its place, In and Out, speed, direction, links, name, label, keyframes
// and effects. Nothing else about the clip changes, so trims, slips, speed changes and the effects on the clip keep
// working on the rendered file. Restore Unrendered puts the nested sequence back where it was.
//
// The rendered file holds the nested sequence from `origin` (a frame of the nested sequence) on, so everything keyed to
// the clip's source time (its In point and every keyframe of its parameters and effects) moves earlier by `origin` when
// the file takes over and later by `origin` when the nest returns (RenderedNest records it). The picture and the sound
// of one nest (a video clip and its audio partner) come from one file; a clip alone (picture only, sound only) from
// the same kind of file.
//
// Refused: clips that are not nested, multi-camera clips (which show one angle of the source), auditions, locked
// clips, clips with Time Remapping or an object mask (their keys are tied to the nest), and nests that play nothing.

#include <wabcut/model/EditOps.h>
#include <wabcut/model/Nesting.h>

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace wabcut::edit {

/// One file Render and Replace makes: a nested sequence's stretch and the clips of one sequence that take it over
/// (a picture clip first, then its sound; each stream appears at most once per group).
struct NestRenderUnit {
    SequenceId nested;
    std::vector<ClipId> clips;
    bool video = false; ///< the file needs the nest's picture
    bool audio = false; ///< ... and its mix
    /// What the clips play, in the nested sequence's time: [sourceIn, sourceOut) (a freeze frame: one frame).
    TimeRange used;
    /// What to render: `used` widened by the handles, kept inside the nested sequence and on its frame grid. This is
    /// the range to give the export; its start is the `origin` to give renderAndReplace().
    TimeRange range;
};

/// Works out what the selected clips (and their linked partners) of `outer` need: one unit per distinct stretch of one
/// nest. `handle` is the extra media rendered before and after what the clips play (>= 0), so a later trim still has
/// picture. Errors: NotFound (sequence, clip), InvalidArgument (nothing nested selected, or a clip Render and Replace
/// refuses: the message names it and why).
std::expected<std::vector<NestRenderUnit>, Error> planRenderAndReplace(const ProjectState& state, SequenceId outer,
                                                                       std::span<const ClipId> clips, Ticks handle);

/// Whether the nested sequence may show transparency over `range` (a gap on the lowest video track, a graphic or a
/// blend mode there): such a nest should be rendered with an alpha-capable preset.
bool nestMayBeTransparent(const ProjectState& state, SequenceId nested, TimeRange range);

/// Adds `rendered` (a media item for the file that was made; its bin and name are kept) and makes the clips play it:
/// each loses its nested sequence, plays the file's stream of its kind, keeps everything else and remembers the
/// sequence (Clip::renderedNest). `origin` is where the file starts in the nested sequence's time. One undo step.
/// Errors: NotFound, InvalidArgument (a clip that is not nested, plays another sequence than the rest, a nest the file
/// does not hold (too short, missing stream), or is refused as in planRenderAndReplace). created media id: the result's
/// createdMedia.
EditOutcome renderAndReplace(const ProjectState& state, SequenceId outer, std::span<const ClipId> clips, MediaItem rendered,
                             Ticks origin);

/// Restore Unrendered: the named clips (and their linked partners) that were rendered and replaced play their nested
/// sequence again; the rendered media stays in the project. Clips that were not rendered are left alone. One undo
/// step. Errors: NotFound, InvalidArgument (no clip was rendered; the nested sequence is gone, lost its stream or
/// would contain the sequence).
EditOutcome restoreUnrendered(const ProjectState& state, SequenceId outer, std::span<const ClipId> clips);

/// Whether the clip was rendered and replaced and can be restored.
inline bool isRenderedNest(const Clip& clip) { return clip.renderedNest() != nullptr; }

} // namespace wabcut::edit
