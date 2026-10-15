#pragma once

// Gaming edit operations (C.16): the moves a gaming editor makes over and over, each one edit (one undo step) that leaves
// plain, editable timeline items behind.
//
//   instantReplay    the moment again right after itself in slow motion: copies of its clips with a time-remap speed
//                    ramp (100 % easing into the slow speed and back out), the REPLAY stinger template over it and
//                    dip-to-white flashes at its cuts. Premiere's instant replay is a nest plus Time Remapping by hand.
//   findDeadTime     low-activity stretches of gameplay (game-audio energy and picture motion are both low), with
//   removeTimeRanges handles kept: the preview list, then a ripple delete on every unlocked track (like a transcript
//                    silence cleanup).
//   zoomPunch        a punch-in zoom on a moment: Motion Scale keys easing in, holding and easing out.
//   shake            a camera shake: Motion Position (and optionally Rotation) keys of a seeded, decaying pattern.
//   makeVersion      a 9:16 (Shorts) or 1:1 copy of a 16:9 edit laid out with the C.15 layouts, captions moved into
//                    the vertical safe area.
//
// Times are timeline ticks unless a field says otherwise; callers snap them to frames (the commands do).

#include <wabcut/model/CaptionStyle.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Graphics.h>
#include <wabcut/model/Layouts.h>
#include <wabcut/model/Parameters.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Time.h>

#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wabcut {

// ---- Instant replay ---------------------------------------------------------------------------------------------------

/// Slow-motion speed bounds of an instant replay.
inline constexpr double kMinReplaySpeed = 0.05;
inline constexpr double kMaxReplaySpeed = 1.0;

struct ReplayOptions {
    /// The moment: this clip's span (and, with includeAudio, its linked audio clips)...
    std::optional<ClipId> clip;
    /// ...or this timeline range (frame-aligned): every clip of media on an unlocked track that overlaps it (video
    /// tracks; audio tracks too with includeAudio). Graphics, titles, adjustment layers and other generated items are
    /// left out (overlays do not replay).
    std::optional<TimeRange> range;
    /// The replay's slow-motion speed, kMinReplaySpeed..kMaxReplaySpeed (0.5 = half speed).
    double speed = 0.5;
    /// Speed ramps: the replay starts at 100 %, eases into `speed`, and eases back to 100 % at its end (time-remap
    /// Bezier keys). Off: a constant `speed`. Moments shorter than 8 frames never ramp.
    bool ramp = true;
    /// Replay the audio clips too, slowed to the replay's length (pitch drops). Off: the replay is silent on audio
    /// tracks (they get the gap an insert opens).
    bool includeAudio = false;
    /// Dip-to-white flashes at the cut into the replay and the cut out of it.
    bool flash = true;
    /// Length of a flash (centred on its cut); 0 = 0.2 s. Whole frames, at least 2.
    Ticks flashDuration{};
    /// The stinger laid over the replay on a free video track above it (a new track when none is free): null = none.
    const GraphicTemplate* label = nullptr;
    /// Field values for the stinger (template field id -> value).
    std::vector<std::pair<std::string, FieldValue>> labelValues;
    /// Sequence markers at or after the insert point move with the material (Premiere's Ripple Sequence Markers).
    bool rippleMarkers = true;
};

struct ReplayResult {
    edit::EditResult edit;
    TimeRange moment;                   ///< what is replayed
    TimeRange replay;                   ///< where the replay plays: [moment end, moment end + its length)
    std::vector<ClipId> clips;          ///< the replay's clips (copies of the moment's)
    ClipId label;                       ///< the stinger clip; invalid when none
    std::vector<TransitionId> flashes;  ///< the dip-to-white transitions added
    std::vector<std::string> notes;     ///< what was left out and why
};

namespace edit {

/// Instant replay (see the file comment): the moment's material is copied, retimed and inserted right after the moment
/// on its tracks; every unlocked sync-locked track opens the same gap, so everything after stays in sync. The playhead
/// goes to the replay's start. Errors: NotFound (clip, sequence), InvalidArgument (neither or both of clip and range,
/// an empty range, a speed out of bounds, nothing replayable in the moment, a clip in it that is time-remapped,
/// reversed, a freeze frame or a nested sequence, a locked clip, a stinger that does not fit).
std::expected<ReplayResult, Error> instantReplay(const ProjectState& state, SequenceId sequence, const ReplayOptions& options);

} // namespace edit

// ---- Dead time --------------------------------------------------------------------------------------------------------

/// How busy a media item is over its own time: one sample per `step` from source time 0, each 0..1 (0 = silent / still).
/// `audio` is game-audio loudness, `motion` the change between frames; either may be empty (no audio, no picture, or
/// not measured). Hosts make these (AI signal curves, or a cheap pass over proxies and thumbnails).
struct MediaActivity {
    static constexpr std::size_t kMaxSamples = 4'000'000; ///< ~ 46 days at 10 samples a second

    Ticks step;
    std::vector<float> audio;
    std::vector<float> motion;
    std::string source; ///< what measured it ("analysis", "proxy", ...)

    bool operator==(const MediaActivity&) const = default;
};

/// Why `activity` is not usable (a step <= 0, too many samples, values outside 0..1 or not finite), or nullopt.
std::optional<std::string> mediaActivityProblem(const MediaActivity& activity);

struct DeadTimeOptions {
    /// Only stretches at least this long count (before the handles are taken off). 0.5..3600.
    double minSeconds = 3.0;
    /// 0..1: how much counts as dead. Higher finds more (a higher activity threshold).
    double sensitivity = 0.5;
    /// Kept at each end of a stretch so cuts do not feel abrupt (like a silence cleanup's handles). 0..5 s.
    double handleSeconds = 0.3;
    /// The tracks to listen to and watch; empty = defaultActivityTracks.
    std::vector<TrackId> tracks;
    /// Only look inside this range (e.g. In to Out); unset = the whole sequence.
    std::optional<TimeRange> range;
};

/// Why options are out of range, or nullopt.
std::optional<std::string> deadTimeOptionsProblem(const DeadTimeOptions& options);

/// A sampled timeline curve: values[i] is the sample at start + i * step (each covers [t, t + step)).
struct ActivityCurve {
    Ticks start;
    Ticks step;
    std::vector<double> values;
};

struct DeadTimeRange {
    TimeRange stretch;      ///< the whole quiet stretch
    TimeRange remove;       ///< the stretch without its handles, whole frames: what removeTimeRanges takes out
    double activity = 0.0;  ///< mean combined activity over the stretch, 0..1
};

struct DeadTimeAnalysis {
    ActivityCurve audio;    ///< loudest audio track per sample, normalised (0..1)
    ActivityCurve motion;   ///< busiest video track per sample, normalised
    ActivityCurve combined; ///< max(audio, motion), smoothed: what is compared with `threshold`
    double threshold = 0.0;
    std::vector<DeadTimeRange> ranges; ///< in time order, not overlapping
    std::vector<MediaId> unmeasured;   ///< media on the listened tracks without activity (counted as quiet)
    std::vector<TrackId> tracks;       ///< the tracks used
};

/// The media activity of a media item, or null when there is none.
using ActivityLookup = std::function<const MediaActivity*(MediaId)>;

/// The tracks dead-time detection uses by default: the lowest video track holding media (the gameplay capture) and every
/// unmuted audio track (game audio and commentary: talking over a quiet stretch keeps it).
std::vector<TrackId> defaultActivityTracks(const Sequence& sequence);

/// Samples per second of the timeline curves findDeadTime builds.
inline constexpr std::int64_t kActivityRate = 10;

/// Finds dead time: each listened track's clips are sampled from their media's activity (through each clip's speed,
/// direction and remap), the loudest audio and busiest picture per sample are normalised by their 90th percentile over
/// the sequence, combined (max) and smoothed over half a second; stretches below the threshold
/// (0.04 + 0.32 x sensitivity) for at least minSeconds are dead. Gaps on every listened track are dead too. Errors:
/// NotFound (sequence, a track), InvalidArgument (options).
std::expected<DeadTimeAnalysis, Error> findDeadTime(const ProjectState& state, SequenceId sequence, const ActivityLookup& activity,
                                                    const DeadTimeOptions& options = {});

namespace edit {

/// Ripple-deletes `ranges` (any order; overlapping ones merge) on every unlocked track and the caption tracks, last
/// range first, as one edit. With `rippleMarkers`, sequence markers after a range move back by its length and markers
/// inside one move to its start. Errors: NotFound, InvalidArgument (an empty or negative range, nothing unlocked).
EditOutcome removeTimeRanges(const ProjectState& state, SequenceId sequence, std::span<const TimeRange> ranges,
                             bool rippleMarkers = true);

} // namespace edit

// ---- Zoom punch and shake -----------------------------------------------------------------------------------------------

struct ZoomPunchOptions {
    /// The zoom at the punch's peak, a factor of the clip's scale there (1.3 = 30 % closer), 1.01..10.
    double scale = 1.3;
    /// The whole punch: ease in, hold, ease out. 2 frames..30 s.
    Ticks duration = Ticks{kTicksPerSecond / 2};
    /// Share of `duration` easing in, 0.05..0.9 (the punch hits at `at`: it starts this much earlier).
    double attack = 0.2;
    /// Share easing back out, 0..0.9 (0 = cut back at the end). attack + release <= 1.
    double release = 0.4;
    /// The keys' interpolation (Linear, EaseIn, EaseOut, EaseInOut, Bezier is EaseInOut; Hold is refused).
    Interpolation easing = Interpolation::EaseInOut;
    /// The point that stays put while zooming, fractions of the frame (0.5, 0.5 = its centre); unset = the clip's
    /// anchor (Position is not touched).
    std::optional<std::array<double, 2>> focus;
};

/// Why options are out of range, or nullopt.
std::optional<std::string> zoomPunchProblem(const ZoomPunchOptions& options);

struct ShakeOptions {
    /// Largest offset, sequence pixels (0.5..1000); it decays to 0 over the duration.
    double intensity = 24.0;
    /// How long it shakes, 2 frames..30 s.
    Ticks duration = Ticks{kTicksPerSecond / 2};
    /// Direction changes per second, 1..60 (keys land on frames, at most one a frame).
    double frequency = 14.0;
    /// Largest rotation, degrees (0 = none), 0..45.
    double rotation = 0.0;
    /// The pattern's seed; 0 = derived from the clip id and the time (the same shake again for the same request).
    std::uint64_t seed = 0;
    /// Scales the clip up for the shake's length so its edges do not show (Scale keys).
    bool cover = true;
};

std::optional<std::string> shakeProblem(const ShakeOptions& options);

/// One key of a shake: `time` from its start, offsets in pixels and degrees.
struct ShakeSample {
    Ticks time;
    double dx = 0.0;
    double dy = 0.0;
    double rotation = 0.0;
};
/// The shake's pattern on the frame grid of `rate`: a zero key at 0, one key per direction change with decaying random
/// offsets (seeded, the same everywhere: SplitMix64), and a zero key at the end. Deterministic.
std::vector<ShakeSample> shakePattern(const ShakeOptions& options, std::uint64_t seed, Rate rate);

namespace edit {

/// A punch-in zoom on a video clip, hitting at timeline time `at` (see ZoomPunchOptions): keys on Scale (and on
/// Position with a focus) replace the keys already inside the punch; outside it the curves are unchanged. Errors:
/// NotFound, InvalidArgument (options, not a video clip, a locked track, the punch outside the clip).
EditOutcome zoomPunch(const ProjectState& state, SequenceId sequence, ClipId clip, Ticks at, const ZoomPunchOptions& options);

/// A camera shake on a video clip from timeline time `at`: Position (and Rotation, and Scale with `cover`) keys of
/// shakePattern added to the curves' values, replacing keys inside the shake. Errors as zoomPunch.
EditOutcome shake(const ProjectState& state, SequenceId sequence, ClipId clip, Ticks at, const ShakeOptions& options);

} // namespace edit

// ---- Vertical and square versions ---------------------------------------------------------------------------------------

enum class VersionFormat : std::uint8_t { Shorts, Square };
std::string_view toString(VersionFormat format);
std::optional<VersionFormat> versionFormatFromString(std::string_view text);

/// How a version lays out its gameplay.
enum class VersionMethod : std::uint8_t {
    Auto,     ///< a facecam: Shorts VerticalSplit / Square blur fill with a facecam box; none: Auto Reframe when the host
              ///< has it, else the blur fill
    Layout,   ///< the facecam layouts (a facecam is required)
    Reframe,  ///< Auto Reframe (the host must have it)
    BlurFill, ///< the gameplay fitted to the width over a blurred copy, facecam box when there is one
};
std::string_view toString(VersionMethod method);
std::optional<VersionMethod> versionMethodFromString(std::string_view text);

/// A host's Auto Reframe: a reframed copy of sequence `source` sized width x height, named `name`, added to the project.
struct ReframedSequence {
    ProjectState state;
    SequenceId sequence;
    std::vector<std::string> notes;
};
using ReframeFunction = std::function<std::expected<ReframedSequence, Error>(const ProjectState& state, SequenceId source,
                                                                             int width, int height, std::string name)>;

struct VersionOptions {
    VersionFormat format = VersionFormat::Shorts;
    VersionMethod method = VersionMethod::Auto;
    std::string name;                         ///< empty: "<source name> (Shorts)" / "(Square)"
    /// The facecam's track; unset: a video track above the gameplay's named like a webcam ("facecam", "webcam", "cam",
    /// "camera", "face", "กล้อง"), else one whose clips carry a C.15 layout (picture-in-picture).
    std::optional<TrackId> facecamTrack;
    /// Instead of a facecam track: where the capture shows the webcam (C.15 LayoutOptions::facecamRegion).
    std::optional<NormalizedRect> facecamRegion;
    /// Layout details (the box corner and size of a square version, the split of a Shorts one...). The facecamRegion
    /// above wins over the one inside.
    LayoutOptions layout;
    bool captions = true;                     ///< move caption tracks into the version's safe area
    ReframeFunction reframe;                  ///< the host's Auto Reframe, if it has one
};

struct VersionResult {
    ProjectState state;
    ChangeSet changes;
    SequenceId sequence;
    VersionMethod method = VersionMethod::Auto; ///< what was used in the end (never Auto)
    TrackId facecamTrack;                       ///< the facecam track used; invalid when none
    std::size_t clipsLaidOut = 0;
    std::size_t captionTracks = 0;              ///< caption tracks restyled
    std::vector<std::string> notes;
};

/// The facecam track makeVersion would use without an explicit one (see VersionOptions::facecamTrack), if any.
std::optional<TrackId> detectFacecamTrack(const Sequence& sequence);

/// The caption style of `style` moved into a version's safe area (Shorts: custom placement above the platform's bottom
/// controls, narrower lines; Square: bottom of an 8 % safe area).
CaptionStyle versionCaptionStyle(const CaptionStyle& style, VersionFormat format);

namespace edit {

/// A Shorts (1080x1920) or square (1080x1080) version of a 16:9 sequence: a copy with new ids at the version's size and
/// the source's frame rate, its gameplay laid out (see VersionMethod) and its captions restyled. The source is not
/// changed. Errors: NotFound (sequence, facecam track), InvalidArgument (a sequence that is not landscape, a Layout
/// method without a facecam, a Reframe method without the host's, layout options), and the reframer's errors.
std::expected<VersionResult, Error> makeVersion(const ProjectState& state, SequenceId sequence, const VersionOptions& options);

} // namespace edit

} // namespace wabcut
