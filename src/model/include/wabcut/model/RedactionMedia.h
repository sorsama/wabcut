#pragma once

// Which media a source-anchored redaction covers (Masks.h; review wave 5 E5-P1-1 and A5-P1-3).
//
// A redaction marks the source material, not the Library item it was drawn on. A subclip (MediaItem::subclip) plays its
// master's file at the master's media times, and a time-preserving derived item (MediaItem::derived: separated stems,
// sample-aligned from source time 0) carries the same media times as its source: they all share one redaction root, and
// a redaction drawn on any of them covers every item with that root, at the same media times. A master or source that
// no longer exists still anchors its subclips and stems (removing it never frees what its redactions covered: fail
// closed). Every place that matches a redaction to the media a clip plays - the video evaluator and resolver, the audio
// bleep, the caption mask, thumbnails, uploads, transcription - uses this rule.

#include <wabcut/model/Ids.h>
#include <wabcut/model/Masks.h>
#include <wabcut/model/ProjectState.h>

#include <string_view>
#include <vector>

namespace wabcut {

/// Whether media derived by `operation` (DerivedProvenance::operation) keeps its source's media times sample for sample:
/// then the source's redactions cover it at the same times.
bool derivationKeepsTime(std::string_view operation);

/// The media `media`'s redactions are anchored to: through subclip masters and time-preserving derivations, to the
/// first item that has neither (or to an id that no longer exists).
MediaId redactionRoot(const ProjectState& state, MediaId media);

/// Whether `redaction` covers clips of `media` (the same redaction root). Exceptions and times are the caller's.
bool redactionCovers(const ProjectState& state, const Redaction& redaction, MediaId media);

/// The project's redactions covering `media`, in id order (pointers into `state`: valid while it is held).
std::vector<const Redaction*> redactionsCovering(const ProjectState& state, MediaId media);

} // namespace wabcut
