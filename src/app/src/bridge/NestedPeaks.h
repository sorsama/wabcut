#pragma once

// Waveforms of sequences (0.5, phase 1): the audio clip of a nested sequence, a multicam clip or a merged clip (Clip >
// Merge Clips) has no media of its own, so PeakSource has nothing to say about it. Its waveform is approximated from
// the peaks of the clips it plays: each column of the clip maps through Clip::sourceTimeAt into the nested sequence's
// time, and takes the loudest peak of the leaf clips there on the tracks the clip reads (Resolve.h nestedTracksRead: a
// plain nested clip the audio tracks whose output is on, a multicam clip its active angles' tracks, a merged clip every
// angle's), nested clips inside followed one level further (two levels in all). Disabled and frozen clips give nothing.
// Gains, effects and the nested master are not applied in this approximation (phase 2 below renders the real mix): it
// shows where the sound is, not how loud the mix plays it. A nested clip sounds only at 100 % (forwards or reversed; a remapped one in its
// 100 % spans), as TrackRenderer plays it; elsewhere its waveform is flat.
//
// Pure functions of the project state and the peak source: TimelineView calls them while building a track (render
// thread, GUI thread blocked) and the facts of each nested sequence (TimelineNestInfo) list the media they read, so new
// peaks of one of them rebuild the tracks that show it.
//
// Phase 2 (NestedMixPeaks.h): given a mix source, the mix the clip really plays (gains, effects, faders, the nested
// master) is rendered in the background and, once complete, drawn instead - exact. While it renders, the part rendered
// so far is exact and the rest keeps the approximation, shaded as building; when it fails, the approximation stays.

#include "PeakSource.h"

#include <wabcut/model/EditSession.h>
#include <wabcut/model/ProjectState.h>

#include <cstdint>
#include <span>
#include <vector>

namespace wabcut::app {

class NestedMixSource;

/// How deep nestedPeaks() follows nested clips: the clip's own sequence and one level inside it.
inline constexpr int kNestedPeakDepth = 2;

struct NestedPeaksResult {
    bool drawn = false;    ///< some leaf clip had peaks, or the mix is rendered: `out` holds the waveform
    bool building = false; ///< a leaf's peaks or the mix are still being made (`pending` says where)
    bool exact = false;    ///< phase 2: drawn from the complete rendered mix
};

/// Fills `out` with the waveform of nested, multicam or merged audio clip `clip` over its timeline range [ta, tb)
/// (out.size() equal columns, as PeakSource::peaks): from its rendered mix when `mixes` has it complete for `project`
/// (exact), else approximated from the leaf clips' peaks in `peaks` (the part of a mix rendered so far exact). `pending`,
/// when not null, is resized to out.size() and set to 1 for the columns that are not final yet (a leaf still building
/// past them, the mix not rendered that far).
NestedPeaksResult nestedPeaks(const Clip& clip, const ProjectState& state, const PeakSource& peaks, Ticks ta, Ticks tb,
                              std::span<float> out, std::vector<std::uint8_t>* pending = nullptr,
                              const NestedMixSource* mixes = nullptr, const ProjectSnapshot& project = {});

/// The media nestedPeaks() may read for clips of `sequence`: every clip on its audio tracks, nested clips followed to
/// kNestedPeakDepth levels; sorted, each once.
std::vector<std::uint64_t> nestedPeakMedia(const ProjectState& state, SequenceId sequence);

/// A stamp of what those waveforms depend on: `sequence` and the sequences its audio tracks nest (each edit replaces a
/// sequence's pointer). 0 when it does not exist.
std::uint64_t nestedPeakStamp(const ProjectState& state, SequenceId sequence);

} // namespace wabcut::app
