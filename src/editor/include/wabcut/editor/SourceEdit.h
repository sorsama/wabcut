#pragma once

// Library -> timeline: editing a media item into a sequence where it was dropped (Premiere's drag from the
// project panel). A drop overwrites at the drop time (Ctrl = insert; the UI picks the mode). The streams of the kind of
// the track under the pointer start on that track; the other kind goes to the matching track of its own kind (a drop
// on V2 puts the audio on A2, one on A2 the picture on V2), or, when that track has something else where the clips
// land, to the nearest one that is free there (the tracks after it first), so a drop never cuts clips on tracks it did
// not land on; only when every track of that kind is taken there does it use the matching track anyway (Premiere's
// patching). Clips linked to the ones the drop overwrites on the pointer's track count as free: dropping an A/V clip
// over another one replaces both halves. Dropped outside the tracks: the targeted tracks (else V1, A1, A2, ...). The
// clips are linked; one undo step through the model's insert/overwrite edits. A.10: the audio of a multi-track
// recording (OBS, ShadowPlay, Game Bar) is routed by name (edit::audioPatchesByName from the track under the pointer,
// or the matching one): a stream joins the track named for its source wherever the drop lands, takes a free track, or
// gets a new one, which the edit adds and names. A sequence dropped the same way becomes a nested clip of it (C.8; a
// multicam clip of a multicam source): its picture and mix land like a media item's video and one audio stream.

#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>

#include <expected>
#include <optional>
#include <vector>

namespace wabcut::editor {

enum class DropMode : std::uint8_t { Insert, Overwrite };

struct MediaDrop {
    MediaId media;
    SequenceId sequence;
    /// Drop time (snapped to the sequence frame grid by the edit).
    Ticks at;
    /// Track under the pointer; invalid when dropped outside the tracks.
    TrackId track;
    DropMode mode = DropMode::Insert;
    /// Source range to use; default: the whole media (still images: the default still duration), or the whole
    /// sequence.
    std::optional<TimeRange> sourceRange;
    /// A sequence dropped instead of media (`media` is then ignored): a nested clip of it.
    SequenceId sourceSequence;
    /// 0.2 UX (a drag from the Project panel, Premiere): without a `sourceRange`, the media's Source Monitor marks
    /// (edit::storedSourceMarks) choose it - In (else the start) to Out (else the end). Off: the whole media.
    bool useSourceMarks = false;
};

/// The patches a drop of `media` on `track` uses (see the file comment). Locked tracks are skipped. `landing`: where
/// the clips will lie; without it every track counts as free.
std::vector<edit::SourcePatch> patchesForDrop(const Sequence& sequence, const MediaItem& media, TrackId track,
                                              std::optional<TimeRange> landing = std::nullopt);
/// The same for a sequence dropped as a source: its picture (Video 0) and its mix (Audio 0), as far as it has them.
std::vector<edit::SourcePatch> patchesForDrop(const Sequence& sequence, const Sequence& source, TrackId track,
                                              std::optional<TimeRange> landing = std::nullopt);

/// The edit for a drop, as one undo step ("Insert" / "Overwrite"). Fails (nothing changes) for unknown ids, media
/// without usable streams, no free destination track, or a sequence dropped into itself (or into one it plays).
edit::EditOutcome dropMedia(EditSession& session, const MediaDrop& drop);

} // namespace wabcut::editor
