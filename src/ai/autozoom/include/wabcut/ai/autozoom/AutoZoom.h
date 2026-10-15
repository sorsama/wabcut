#pragma once

// Auto zoom for screen recordings (AI-11, the "Screen Studio" effect; docs/ai.md "Creator superpowers"): the clip
// zooms in where things happen (clicks, typing, where the cursor comes to rest) and back out when nothing does, as
// ordinary, editable Transform keyframes (Position X/Y and Scale) on the clip. Nothing is rendered.
//
//   planZoom()   activity (Activity.h) -> focus segments: events that follow each other within `idleSeconds` and fit
//                one window are one segment; a segment zooms so its events fill the window with a margin, at most
//                `maxZoom`, from a little before its first event until `holdSeconds` after its last. A scene change
//                zooms out. The wanted zoom and window centre over time go through AI-9's L1 camera-path optimiser
//                (reframe/CameraPath.h): it keeps still what should be still and turns steps into an editor's ramps,
//                with the window always inside the frame. The path is then reduced to few keys (Ramer-Douglas-
//                Peucker on all channels together) and the ramps get at least `transitionSeconds`.
//   applyZoom()  the keys on the clip, with the chosen easing: Scale = zoom x the clip's own scale and
//                Position = zoom x (its own position - window centre). Both channels share key times and easing, so the
//                visible window between keys is always a blend of the windows at the keys and never leaves the frame.
//                The clip's own placement (its Position and Scale at its first key, or its constants) is the zoomed-
//                out state, and every path starts there, so running auto zoom again with the same options gives the
//                same keys (idempotent) instead of zooming a zoom.
//
// Cursor smoothing and a cursor highlight (drawing over the recording) are out of scope: they need a renderer
// effect; the activity data needed for them is the same.

#include <wabcut/ai/autozoom/Activity.h>
#include <wabcut/model/ProjectState.h>

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::autozoom {

inline constexpr std::string_view kGenerator = "autozoom/1";

enum class Easing { EaseInOut, Linear, Smooth };
std::string_view toString(Easing easing);
std::optional<Easing> easingFromString(std::string_view text);

struct ZoomOptions {
    double maxZoom = 2.0;           ///< 1.1 .. 4
    double holdSeconds = 1.5;       ///< stay zoomed this long after the last event of a segment
    double idleSeconds = 2.5;       ///< events closer than this (and fitting one window) share a zoom
    double transitionSeconds = 0.6; ///< shortest zoom in / out / pan
    double leadSeconds = 0.4;       ///< the zoom is in place this long before a segment's first event
    double margin = 0.25;           ///< around the events, as a fraction of the region (each side)
    double minRegion = 0.12;        ///< a point event is treated as a region this big (fraction of the picture)
    Easing easing = Easing::EaseInOut;
    double smoothness = 0.5;        ///< the L1 optimiser's: 0 follows closely, 1 moves only when it must
    double gridFps = 30.0;          ///< path samples per second
    double tolerance = 0.003;       ///< key reduction: largest deviation, as a fraction of the frame
    bool clicks = true;             ///< which events zoom
    bool typing = true;
    bool dwell = true;
};

/// The parameters as JSON (the review report, the command's echo), and back (unknown keys ignored).
std::string optionsToJson(const ZoomOptions& options);

/// One zoom: where and when.
struct ZoomSegment {
    Ticks start;                    ///< source time the zoom is fully in
    Ticks end;                      ///< source time it starts to leave
    double zoom = 1.0;
    double x = 0.5;                 ///< focus, 0..1 of the displayed picture
    double y = 0.5;
    std::size_t events = 0;
    std::string reason;             ///< "click", "typing", "2 clicks and typing", ...
};

/// The camera over the clip's source range: zoom (>= 1) and the window centre in frame units (0 = frame centre,
/// +-0.5 = the frame's edges), per sample.
struct ZoomPath {
    std::vector<Ticks> times;       ///< source times, uniform
    std::vector<double> zoom;
    std::vector<double> u;          ///< window centre x in frame units
    std::vector<double> v;          ///< window centre y
    std::vector<ZoomSegment> segments;
    std::vector<std::size_t> keys;  ///< sample indices kept as keyframes
};

/// Where a picture point lands on the frame before any zoom, in frame units (0 = centre): what planZoom aims at.
struct Placement {
    double frameWidth = 1920.0;     ///< sequence pixels
    double frameHeight = 1080.0;
    double pictureWidth = 1920.0;   ///< displayed picture pixels (sample aspect and rotation applied)
    double pictureHeight = 1080.0;
    double positionX = 0.0;         ///< the clip's base Position (sequence px from the frame centre)
    double positionY = 0.0;
    double scale = 1.0;             ///< base Scale
    double anchorX = 0.0;           ///< Anchor (source px from the picture centre)
    double anchorY = 0.0;
    double rotation = 0.0;          ///< degrees, clockwise
    /// Frame units of a picture point (0..1 of the picture).
    std::pair<double, double> toFrame(double x, double y) const;
};

/// Plans the zoom over [source) from `activity` for a clip placed as `placement`.
ZoomPath planZoom(const ActivityTrack& activity, TimeRange source, const Placement& placement, const ZoomOptions& options = {});

struct ZoomResult {
    ProjectState state;
    std::size_t keyframes = 0;      ///< keys written (Position X + Y + Scale)
    std::size_t replacedKeys = 0;   ///< keys the clip had on those parameters before (a previous run, or hand-made)
    Placement placement;
    ZoomPath path;
    std::vector<std::string> warnings;
};

/// The clip's base placement: its Position, Scale, Anchor and Rotation when they are constants, else their values at
/// their first key (the zoomed-out start an earlier auto zoom wrote). Errors: NotFound, InvalidArgument (not a video
/// clip; a freeze frame or time-remapped clip).
std::expected<Placement, Error> placementOf(const ProjectState& state, SequenceId sequence, ClipId clip);

/// Writes `path` onto the clip as Position X/Y and Scale keyframes (replacing their keys; Anchor and Rotation kept).
std::expected<ZoomResult, Error> applyZoom(const ProjectState& state, SequenceId sequence, ClipId clip, const ZoomPath& path,
                                           const Placement& placement, const ZoomOptions& options);

/// Largest distance (frame units) the window strays outside the frame anywhere on the clip's written curves, sampled
/// at `fps`: 0 when it never shows outside the frame. For tests and the report.
double maxOutside(const Clip& clip, const Placement& placement, double fps = 60.0);

} // namespace wabcut::ai::autozoom
