#pragma once

// The importers' intermediate representation.
//
// Every format parser produces an IrDocument: a format-neutral description of media, sequences, tracks,
// clips, transitions and markers in Ticks, plus report entries for what it could not represent. The
// TimelineBuilder turns that into a ProjectState through the model's edit operations. Keeping this
// layer between the formats and the model means a model change (transitions, retiming, sequence
// duration, MediaLocator) is adapted in one place, not in five parsers.
//
// Export (X.1) runs the other way: describeForExport() (Exporters.h) turns a sequence of a project into an
// IrDocument, and each format writer writes that. The fields only export fills say so.

#include <wabcut/interchange/ImportReport.h>
#include <wabcut/interchange/MediaRelinker.h>
#include <wabcut/model/Clip.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/Sequence.h>
#include <wabcut/model/Time.h>
#include <wabcut/model/Track.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wabcut::interchange::detail {

inline constexpr std::size_t kNoMedia = static_cast<std::size_t>(-1);
inline constexpr std::size_t kNoSequence = static_cast<std::size_t>(-1);

struct IrMarker {
    Marker marker;
    /// Non-empty when the marker was converted (e.g. a clip marker made a sequence marker): the reason.
    std::string approximation;
    SourceLocation location;
};

struct IrMedia {
    /// Document-unique key (file id, asset id, decoded path, clip name).
    std::string key;
    std::string name;
    MediaReference reference;
    bool hasVideo = false;
    bool stillImage = false;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::optional<Rate> frameRate;
    Rate sampleRate = kRate48kHz;
    /// Channel count of each audio stream the file declares (the builder adds streams clips need).
    std::vector<std::int32_t> audioChannels;
    /// Declared media duration (the builder keeps it only when every clip fits inside it).
    std::optional<Ticks> duration;
    /// Clips' source In values are absolute source timecode (EDL), not offsets from the first frame. The
    /// builder subtracts the media's start timecode when the media inspector knows it.
    bool absoluteSourceTimecode = false;
    /// The file's embedded start timecode as a position, when known (export: EDL source timecodes, FCP7 <timecode>).
    std::optional<Ticks> startTimecode;
    /// Markers of the source (X.1: Premiere's clip markers, MediaItem::markers), in media time: they move with every
    /// clip of it.
    std::vector<IrMarker> markers;
    /// The bins the source keeps it in (X.1: FCP7 bins, FCPXML events), outermost first; empty: the import bin.
    std::vector<std::string> bin;
    SourceLocation location;
};

/// A constant speed change (Premiere's Speed/Duration): the clip covers `duration` of timeline and duration x
/// speed of media from `sourceIn`. A reversed clip plays that range backwards (`sourceIn` is its lowest media
/// time); a freeze shows the frame at `sourceIn` for the whole clip.
struct IrSpeed {
    Speed ratio{1, 1};  ///< exact; {0, 1} = freeze frame
    bool reversed = false;
    /// Non-empty when the source's speed has no exact Speed (an irrational time scalar, ...): what was rounded.
    std::string approximation;
    SourceLocation location; ///< where the speed is written (filter, timeMap, effect, M2 line)
};

/// Variable speed (FCP7 `timeremap` graph, FCPXML `timeMap` ramps): a time remap curve (TimeRemap.h) from the
/// clip's curve time to source positions relative to the start of the media file, and the curve time at the clip's
/// first frame. The builder applies it with edit::setTimeRemap.
struct IrRemap {
    Curve curve;
    Ticks curveIn;
    /// Non-empty when the source's curve could not be carried exactly (smooth ramps, fractional frames).
    std::string approximation;
    SourceLocation location;
};

/// A text title (a generator with no media file): the builder makes it with edit::addTitle.
struct IrTitle {
    std::string text;       ///< UTF-8, lines separated by newlines
    std::string fontFamily;
    std::string fontStyle;
    /// "wabcut.title.style" values the source sets (size, fill, stroke, alignment, ...).
    std::vector<EffectParam> style;
    /// Non-empty when parts of the source's title could not be carried: what they were.
    std::string approximation;
};

struct IrClip {
    std::string name;
    std::size_t media = kNoMedia;
    StreamRef stream;
    Ticks start;
    Ticks duration;
    /// Source time of the first frame, relative to the start of the media file.
    Ticks sourceIn;
    bool enabled = true;
    /// Clips with the same non-negative group are linked (A/V link). Document-unique.
    std::int64_t linkGroup = -1;
    /// Static motion/opacity/audio values the parser could carry.
    std::optional<ClipProperties> properties;
    /// A constant speed other than 100% forward (nullopt: normal playback).
    std::optional<IrSpeed> speed;
    /// Variable speed; takes the place of `speed`.
    std::optional<IrRemap> remap;
    /// Set for a title (then `media` is kNoMedia).
    std::optional<IrTitle> title;
    /// C.8: a nested or multicam clip (then `media` is kNoMedia): the IrDocument::sequences index of the sequence it
    /// plays, or its IrSequence::key when the parser meets the clip before that sequence. `sourceIn` is in that
    /// sequence's time; the stream is Video 0 (its picture) or Audio 0 (its mix).
    std::size_t sequence = kNoSequence;
    std::string sequenceKey;
    /// A multicam clip's active angles (indices into IrSequence::multicam angles): one for video, any for audio.
    std::vector<std::size_t> angles;
    /// A.9, audio clips: the source channels the clip plays (FCP7 channel tracks, FCPXML audio-channel-source).
    std::optional<AudioChannelMap> channels;
    /// Effects after the intrinsic ones (Motion, Opacity, Volume, Pan), in stack order. Export: the clip's stack, which
    /// the writers map to the format's effects where they can.
    std::vector<EffectInstance> effects;
    SourceLocation location;
};

enum class TransitionKind : std::uint8_t { Dissolve, DipToBlack, DipToWhite, Wipe, AudioCrossfade, Other };

/// A transition straddling an edit point (or a fade at a clip's head/tail).
struct IrTransition {
    std::string name;
    TransitionKind kind = TransitionKind::Dissolve;
    /// The model's transition effect when the source names it exactly (X.1: Wabcut's own exports); empty: from `kind`.
    std::string effect;
    /// The edit point the transition belongs to.
    Ticks cut;
    /// Extent before and after the cut (a centred 30-frame dissolve: 15 + 15 frames).
    Ticks before;
    Ticks after;
    SourceLocation location;
};

/// An explicit gap element (FCPXML <gap>, OTIO Gap); used for report locations only, gaps are implicit.
struct IrGap {
    TimeRange range;
    SourceLocation location;
};

struct IrTrack {
    TrackKind kind = TrackKind::Video;
    std::string name;
    bool enabled = true;
    bool locked = false;
    std::optional<bool> targeted;
    /// Export: an audio track's mute and role (FCPXML roles).
    bool muted = false;
    TrackRole role = TrackRole::None;
    std::vector<IrClip> clips;
    std::vector<IrTransition> transitions;
    std::vector<IrGap> gaps;
    SourceLocation location;
};

/// One angle of a multicam source (C.8): its tracks (indices into the sequence's track lists) and its sync offset.
struct IrMulticamAngle {
    std::string name;
    std::optional<std::size_t> videoTrack;
    std::vector<std::size_t> audioTracks;
    Ticks offset;
};

struct IrMulticam {
    MulticamSync sync = MulticamSync::Manual;
    std::vector<IrMulticamAngle> angles;
};

struct IrSequence {
    /// Document-unique key nested clips may name it by (IrClip::sequenceKey); empty when nothing does.
    std::string key;
    /// Set on a multicam source (FCPXML <multicam>, FCP7 <multiclip>).
    std::optional<IrMulticam> multicam;
    std::string name;
    Rate frameRate = kRate30;
    bool dropFrame = false;
    std::int32_t width = 1920;
    std::int32_t height = 1080;
    Rate audioRate = kRate48kHz;
    std::vector<IrTrack> videoTracks; // V1..Vn
    std::vector<IrTrack> audioTracks; // A1..An
    std::vector<IrMarker> markers;
    std::optional<Ticks> markIn;
    std::optional<Ticks> markOut;
    /// Explicit duration, when the format has one (it can end in a trailing gap).
    std::optional<Ticks> duration;
    /// The timecode of the first frame (X.1: Sequence::startTimecode), when the format says.
    std::optional<Ticks> startTimecode;
    /// The bins the source keeps it in (X.1), outermost first; empty: the import bin.
    std::vector<std::string> bin;
    SourceLocation location;
};

struct IrDocument {
    std::vector<IrMedia> media;
    std::vector<IrSequence> sequences;
    /// Entries the parser produced (approximations and drops it knows about).
    ImportReport report;
};

} // namespace wabcut::interchange::detail
