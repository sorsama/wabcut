#pragma once

// A sequence (timeline): settings, video tracks V1..Vn (bottom to top), audio tracks A1..An, in/out
// marks and markers. Tracks are shared immutable nodes, so a new sequence revision shares every track
// an edit did not touch with the previous revision.

#include <wabcut/model/CaptionStyle.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/Time.h>
#include <wabcut/model/Track.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut {

using TrackPtr = std::shared_ptr<const Track>;

/// A caption: text shown over [start, start + duration) on the timeline. Lines are separated by '\n'.
struct CaptionCue {
    Ticks start;
    Ticks duration;
    std::string text; ///< UTF-8; one or more lines joined by '\n' (at most kMaxCaptionTextBytes)
    SpeakerId speaker; ///< who speaks (ProjectState::speakers); invalid = nobody in particular

    Ticks end() const { return start + duration; }
    bool operator==(const CaptionCue&) const = default;
};

/// A caption track (subtitles) of a sequence: cues on the timeline, in one language, and how they look.
///
/// Captions follow edits (C.7): a caption track behaves like a sync-locked track that is never locked. Ripples
/// (insert, extract, ripple delete / trim, Q/W, transcript deletes, Insert Frame Hold Segment, moves in insert mode,
/// speed changes with ripple) move, trim or remove the cues in the same edit: material after the edit point moves
/// with it, a removed range takes the part of each cue inside it (a cue left without time goes), and an insert inside
/// a cue cuts it in two (both keep its text, like a clip cut through). Retiming an audio clip (Speed/Duration, a time
/// remap that adds or removes time inside it) also re-times the cues over the clip with its speech. Edits that do not
/// change timing (overwrite, lift, delete, move, trim, roll, slip, slide, razor) leave captions where they are.
struct CaptionTrack {
    CaptionTrackId id;
    std::string name;
    std::string language; ///< ISO 639-1, e.g. "en", "th"; empty = unknown
    bool enabled = true;
    /// Sorted by start, positive durations; never overlapping unless style.allowOverlap.
    std::vector<CaptionCue> cues;
    CaptionStyle style;

    bool operator==(const CaptionTrack&) const = default;
};

/// A beat in timeline time and its place in the bar.
struct Beat {
    Ticks time;
    std::int32_t index = 0;  ///< beats counted from the grid's downbeat at `phase` (negative before it)
    std::int32_t inBar = 1;  ///< 1..beatsPerBar; 1 = downbeat
    bool isDownbeat() const { return inBar == 1; }
    bool operator==(const Beat&) const = default;
};

/// The tempo grid of a sequence (detected from music or tapped by hand): what beat-snapping, edit.snapToBeats
/// and edit.cutToBeats use. Regular (beats at phase + k x 60/bpm) unless `beats` lists detected beats, which
/// follow music that drifts. Beats before 0 do not exist.
struct BeatGrid {
    /// Tempo in thousandths of a beat per minute (120000 = 120 BPM): exact, so beat times are deterministic.
    std::int64_t bpmMilli = 120000;
    /// Timeline time of a downbeat (beat 1 of a bar); a regular grid extends both ways from it.
    Ticks phase;
    std::int32_t beatsPerBar = 4;
    /// Detected beats in timeline time (sorted, >= 0) with `phase` among them; empty for a regular grid.
    std::vector<Ticks> beats;

    /// The beat period (nearest tick).
    Ticks period() const;
    double bpm() const { return static_cast<double>(bpmMilli) / 1000.0; }
    /// Beats in [range.start, range.end()), in time order.
    std::vector<Beat> beatsIn(TimeRange range) const;
    /// The beat nearest to `t` (the earlier on a tie), or nullopt when the grid has no beat at or after 0.
    std::optional<Beat> nearestBeat(Ticks t) const;
    /// The first beat at or after `t`.
    std::optional<Beat> beatAtOrAfter(Ticks t) const;
    bool operator==(const BeatGrid&) const = default;
};

/// What a marker is for (Premiere's marker types). Comment markers are the default; the others are metadata that
/// exports and encoders read: chapters, segmentation, and web links (a URL with an optional frame target).
enum class MarkerType : std::uint8_t { Comment, Chapter, Segmentation, WebLink };

/// A marker: on a sequence (in sequence time) or on a media item (a clip marker, in source time; Premiere shows a
/// master clip's markers on every instance). A duration of 0 is a point marker.
/// How sure a highlight finder is about a moment (C.15, MarkerHighlight).
enum class HighlightConfidence : std::uint8_t { Low, Medium, High };

/// C.15 (gaming, G.2): what a highlight finder says about a marked moment - the AI engineer's highlight finder writes
/// these on markers, the Highlights panel reads them. Bounded (project files and commands are untrusted).
struct MarkerHighlight {
    static constexpr std::size_t kMaxReasons = 8;
    static constexpr std::size_t kMaxReasonBytes = 200;
    static constexpr std::size_t kMaxSourceBytes = 64;

    double score = 0.0;                                       ///< 0..1
    HighlightConfidence confidence = HighlightConfidence::Medium;
    std::vector<std::string> reasons;                         ///< at most 8, each 1..200 bytes ("kill streak", "mic peak")
    Ticks peak;                                               ///< the best moment, inside [marker start, marker end]
    std::string source = "highlights";                        ///< what found it, 1..64 bytes

    bool operator==(const MarkerHighlight&) const = default;
};

std::string_view toString(HighlightConfidence confidence);
std::optional<HighlightConfidence> highlightConfidenceFromString(std::string_view text);

struct Marker {
    Ticks start;
    Ticks duration;
    std::string name;
    std::string comment;
    std::int32_t colorIndex = 0;
    // M5i: Premiere's marker type and web-link fields. Saved only when set.
    MarkerType type = MarkerType::Comment;
    std::string url;    ///< web link: http or https only
    std::string target; ///< web link: the frame target
    /// C.15: a highlight finder's verdict on the moment (MarkerHighlight). Saved only when set.
    std::optional<MarkerHighlight> highlight;

    Ticks end() const { return start + duration; }
    bool operator==(const Marker&) const = default;
};

/// Bounds for markers (project files and commands are untrusted).
inline constexpr std::size_t kMaxMarkerTextBytes = 4096; ///< name and comment
inline constexpr std::size_t kMaxMarkerUrlBytes = 2048;  ///< url and target
inline constexpr std::size_t kMaxMarkers = 10000;        ///< per sequence or media item

/// Why a marker is not allowed (a negative start or duration, text over the bounds, a URL that is not http or
/// https, a highlight out of its bounds: score 0..1, at most 8 reasons of 1..200 UTF-8 bytes, a source of 1..64, a peak
/// inside the marker), or nullopt when it is fine.
std::optional<std::string> markerProblem(const Marker& marker);
/// "comment", "chapter", "segmentation", "webLink".
std::string_view markerTypeName(MarkerType type);
std::optional<MarkerType> markerTypeFromName(std::string_view name);

/// Where a derived sequence came from (AI-9: a reframed or shorts sequence, docs/ai.md "Auto reframe"): the source
/// sequence and the generator's parameters, so the derivation can be run again after the source is edited. The source
/// may have been deleted since (re-running then reports it); that is not an invalid state. `params` is a JSON object as
/// text, bounded (project files are untrusted input).
struct SequenceDerivation {
    static constexpr std::size_t kMaxGeneratorBytes = 64;
    static constexpr std::size_t kMaxParamsBytes = 16 * 1024;

    SequenceId source;
    std::string generator; ///< e.g. "reframe/1", "shorts/1"
    std::string params;    ///< a JSON object (text): what the generator was asked for

    bool operator==(const SequenceDerivation&) const = default;
};

/// How a multicam source's angles were synchronised (Premiere's Create Multi-Camera Source Sequence).
enum class MulticamSync : std::uint8_t {
    Timecode,  ///< the media's start timecodes line up
    InPoints,  ///< the media's Source Monitor In marks line up
    OutPoints, ///< their Out marks line up
    Markers,   ///< their first clip markers line up
    Audio,     ///< their audio waveforms (AudioSyncAnalyzer, Nesting.h)
    Manual,    ///< offsets set by hand (multicam.setSync)
};

/// Bound on a multicam angle's sync offset (24 hours): project files are untrusted.
inline constexpr Ticks kMaxMulticamOffset = Ticks::fromSeconds(24 * 60 * 60);

/// One camera (or recorder) of a multicam source: its tracks in the source sequence and its sync.
struct MulticamAngle {
    std::string name;
    TrackId video;              ///< its video track; invalid for an audio-only angle
    std::vector<TrackId> audio; ///< its audio tracks (none for a video-only angle)
    /// Where the angle's material starts in the source sequence's time: its clips lie this far after 0 (the sync).
    /// 0 to kMaxMulticamOffset; multicam.setSync moves the angle's clips by the difference.
    Ticks offset;

    bool operator==(const MulticamAngle&) const = default;
};

/// A sequence that is a multicam source (Nesting.h): N synchronised angles, each a video track and/or audio tracks of
/// this sequence. Multicam clips (Clip::sequence + Clip::angles) play one angle's picture and one or more angles'
/// sound; angle cuts are ordinary razor cuts of those clips.
struct MulticamInfo {
    MulticamSync sync = MulticamSync::Timecode;
    std::vector<MulticamAngle> angles; ///< at least one, at most kMaxMulticamAngles

    bool operator==(const MulticamInfo&) const = default;
};

/// The latest start timecode a sequence may have: just under 24 hours (X.1).
inline constexpr Ticks kMaxStartTimecode{24LL * 3600 * kTicksPerSecond - 1};

struct Sequence {
    SequenceId id;
    /// Bin that holds the sequence; invalid = the project's root bin.
    BinId bin;
    std::string name;
    Rate frameRate = kRate30;
    std::int32_t width = 1920;
    std::int32_t height = 1080;
    Rate audioRate = kRate48kHz;
    /// Display timecode as drop-frame (only meaningful at 29.97/59.94).
    bool dropFrame = false;
    /// X.1: the timecode of the sequence's first frame (Premiere's Start Time, often 01:00:00:00), 0 to
    /// kMaxStartTimecode. Interchange and display only: times in the model still count from 0. Saved only when set.
    Ticks startTimecode;
    /// V1..Vn, bottom to top.
    std::vector<TrackPtr> videoTracks;
    /// A1..An.
    std::vector<TrackPtr> audioTracks;
    /// Sequence In mark (inclusive tick).
    std::optional<Ticks> markIn;
    /// Sequence Out mark (exclusive tick: the end of the last marked frame).
    std::optional<Ticks> markOut;
    /// Sorted by start.
    std::vector<Marker> markers;
    /// Explicit sequence length, at least contentEnd(). Edits that extend the content grow it; ripple
    /// removals leave it alone, so a trailing gap survives until edit::setSequenceDuration or
    /// edit::fitSequenceDurationToContent shrinks it.
    Ticks duration;
    /// Premiere's work area bar (render/export range), if set. Lies within [0, duration].
    std::optional<TimeRange> workArea;
    /// Master effect stack, applied to the sequence's composited video. Placeholder: stored, validated and
    /// saved; no evaluator uses it yet.
    EffectStack effects;
    /// Master audio effect stack (audio effects only), applied to the sequence's mix on the master bus before
    /// the master fader and meters: e.g. a true-peak limiter. Keyframe times are timeline times.
    EffectStack audioEffects;
    /// A.3: the master fader (dB, 0 = unity), after audioEffects: the intrinsic volume parameter of the master bus
    /// (ParamId::VolumeDb on EffectTarget::masterAudio()), a constant or a curve in timeline time.
    ParamValue masterVolume;
    /// A.9: the master's channel format: stereo, or 5.1 (mono and stereo tracks go through their 5.1 panners). Preview
    /// on a stereo device and stereo exports downmix it (ITU-R BS.775). Saved only when not stereo.
    ChannelFormat masterFormat = ChannelFormat::Stereo;
    /// Caption tracks (subtitles), drawn above the video tracks.
    std::vector<CaptionTrack> captionTracks;
    /// The tempo grid beat-snapping uses, when the sequence has one (edit::setBeatGrid).
    std::optional<BeatGrid> beatGrid;
    /// Set on sequences an AI generator derived from another one (AI-9); absent otherwise.
    std::optional<SequenceDerivation> derivedFrom;
    /// Set on a multicam source sequence (C.8, Nesting.h); absent otherwise.
    std::optional<MulticamInfo> multicam;

    struct TrackPosition {
        TrackKind kind = TrackKind::Video;
        std::size_t index = 0; // 0 = V1 / A1
    };
    struct ClipLocation {
        const Track* track = nullptr;
        std::size_t index = 0;

        const Clip& clip() const { return track->clips[index]; }
    };

    const std::vector<TrackPtr>& tracks(TrackKind kind) const {
        return kind == TrackKind::Video ? videoTracks : audioTracks;
    }
    std::size_t trackCount() const { return videoTracks.size() + audioTracks.size(); }

    /// Calls fn(const Track&) for V1..Vn then A1..An.
    template <class Fn>
    void forEachTrack(Fn&& fn) const {
        for (const auto& t : videoTracks) {
            fn(*t);
        }
        for (const auto& t : audioTracks) {
            fn(*t);
        }
    }

    const Track* findTrack(TrackId trackId) const;
    std::optional<TrackPosition> trackPosition(TrackId trackId) const;
    /// Linear in the number of clips.
    std::optional<ClipLocation> findClip(ClipId clipId) const;
    /// End of the last clip on any track (O(tracks)).
    Ticks contentEnd() const;
    /// [markIn, markOut) when both marks are set.
    std::optional<TimeRange> markedRange() const;

    /// Deep value equality (compares track contents, not track pointers).
    bool operator==(const Sequence& other) const;
};

} // namespace wabcut
