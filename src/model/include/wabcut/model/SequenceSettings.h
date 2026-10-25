#pragma once

// Sequence settings, duplicates and New Sequence From Clip (P.2, E5): Premiere's Sequence > Sequence Settings on an
// existing sequence, Edit > Duplicate in the Project panel and File > New > Sequence From Clip.
//
// Changing the timebase of a sequence with clips keeps the edit where it is in real time and puts every edit point on
// the new frame grid (Premiere conforms the clips' positions): each clip's start and end move to the nearest frame of the
// new rate (the start moves later, the end earlier, where the media has no handle to reach the nearer frame), the clip's
// source follows its start (direction- and speed-aware), so the picture under the rest of the clip does not move, and
// adjacent clips stay adjacent. A clip shorter than half a frame of the new rate disappears. Transitions, markers, In and
// Out, the work area, the duration and caption cues move to the grid the same way. Locked tracks are left exactly as they
// are. Keyframes are in media time and do not move.

#include <wabcut/model/EditOps.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/ProjectState.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace wabcut::edit {

/// Sequence Settings fields to change; unset fields keep their value.
struct SequenceSettingsChange {
    std::optional<std::string> name;
    /// Timebase (an exact rate, 1 to 1000 fps). Clips are conformed to the new frame grid (see above).
    std::optional<Rate> frameRate;
    /// Frame size, 16 to 16384 pixels each way. Clips keep their Motion (positions are from the frame's centre).
    std::optional<std::int32_t> width;
    std::optional<std::int32_t> height;
    std::optional<Rate> pixelAspect;
    std::optional<FieldOrder> fields;
    /// Audio sample rate (an exact rate, 8 to 384 kHz).
    std::optional<Rate> audioRate;
    /// Drop-frame timecode display (honoured at 29.97 / 59.94; any other rate turns it off).
    std::optional<bool> dropFrame;
    /// Preview file format id ("" = default), at most 32 bytes.
    std::optional<std::string> previewFormat;
    std::optional<VideoTimeDisplay> videoDisplay;
    std::optional<AudioTimeDisplay> audioDisplay;
    /// The timecode of the first frame (0 to kMaxStartTimecode).
    std::optional<Ticks> startTimecode;
    /// 0.2: the label colour, 0..15 (Premiere's Label on a sequence).
    std::optional<std::int32_t> colorIndex;
    /// 0.4: Markers > Ripple Sequence Markers (Sequence::rippleMarkers): the sequence's markers follow ripple edits.
    std::optional<bool> rippleMarkers;
};

/// Sequence > Sequence Settings. Errors: NotFound, InvalidArgument (a value out of range). One ChangeSet.
EditOutcome setSequenceSettings(const ProjectState& state, SequenceId sequence, const SequenceSettingsChange& change);

/// The time `t` as the sequence's Video Display Format shows it (Premiere's Sequence Settings > Display Format): its
/// timecode (drop-frame where the sequence uses it), the frame count ("1234"), or feet + frames of 16 mm (40 frames a
/// foot) or 35 mm film (16 frames a foot; "0077+02"). Negative times get a leading '-'.
std::string formatSequenceTime(const Sequence& sequence, Ticks t);

/// 0.2, Premiere's Show Audio Time Units: the time `t` in the sequence's Audio Display Format - hours, minutes, seconds
/// and the audio sample within the second at its audio rate ("00:00:01:09600"), or milliseconds ("00:00:01.200").
/// Negative times get a leading '-'.
std::string formatSequenceAudioTime(const Sequence& sequence, Ticks t);

/// "<name> Copy", or "<name> Copy 2", 3, ... when that is taken among `taken` (Premiere's Duplicate naming).
std::string copyName(const std::string& name, const std::vector<std::string>& taken);

/// Edit > Duplicate on a sequence: a copy with new ids in the same bin, named copyName() unless `name` is given.
std::expected<Created<SequenceId>, Error> duplicateSequence(const ProjectState& state, SequenceId sequence,
                                                            std::string name = {});
/// Edit > Duplicate on a project item: another item for the same file (or generator) with its settings, marks, markers
/// and metadata, in the same bin, named copyName() unless `name` is given. Clips keep playing the original item.
std::expected<Created<MediaId>, Error> duplicateMedia(const ProjectState& state, MediaId media, std::string name = {});

/// File > New > Sequence From Clip: a sequence whose settings match the item's video (frame size as displayed, rate,
/// pixel aspect; 1920x1080 at 30 fps for audio-only items and the item's size at 30 fps for stills) and audio (sample
/// rate; one audio track per stream, at least three), named after the item and in its bin, with the item (between its
/// Source Monitor marks) edited in at the start. Errors: NotFound, InvalidArgument (an item a sequence cannot be made
/// from).
std::expected<Created<SequenceId>, Error> newSequenceFromClip(const ProjectState& state, MediaId media,
                                                              std::string name = {});

/// Sequence > Add Tracks... with a placement (Premiere's Before First Track / After Target Track / After Last Track; the
/// track header menu's Add Track): a track of `kind` at `index` among the tracks of that kind (0: it becomes V1 / A1;
/// the count or more: above the top video track / below the last audio track, as addTrack does). The tracks from
/// `index` on move up one number with their clips. The new track has Track's defaults and `name` (empty: "Video n" /
/// "Audio n"). Errors: NotFound.
std::expected<Created<TrackId>, Error> insertTrack(const ProjectState& state, SequenceId sequence, TrackKind kind,
                                                   std::size_t index, std::string name = {});

} // namespace wabcut::edit
