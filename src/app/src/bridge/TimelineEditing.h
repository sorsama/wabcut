#pragma once

// TimelineEditing: the edit side of the timeline's mouse gestures, independent of rendering.
//
// A drag is described by a GestureSpec and turned into a model edit by applyGesture(), a pure function of the
// project state. TimelineView runs it on the current snapshot for the live ghost preview (nothing is committed
// while the mouse moves), and on release ProjectController::commitGesture() runs the very same function inside
// an EditSession gesture, so the committed result is exactly the preview and one drag is one undo step.
//
// Tools (Premiere): V select/move, A / Shift+A track select forward/backward, B ripple edit, N rolling edit,
// Y slip, U slide, C razor, R rate stretch, P pen. On the selection tool an edge drag trims (Ctrl/Cmd: ripple trim), a
// body drag moves (Ctrl/Cmd: insert instead of overwrite; Alt: ignore links). 0.1.2: the rate stretch tool's edge drag
// changes the clip's speed so the same material fills the new length; the pen tool edits the volume rubber band.

#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectState.h>

#include <QString>

#include <optional>
#include <span>
#include <vector>

namespace wabcut::app {

enum class TimelineTool { Select, TrackForward, TrackBackward, Ripple, Roll, Slip, Slide, Razor, Remix, RateStretch, Pen };

/// "select", "trackForward", "trackBackward", "ripple", "roll", "slip", "slide", "razor", "remix" (M5m: Premiere's Remix
/// tool: dragging a music clip's end proposes a length, nothing changes until the remix is applied), "rateStretch",
/// "pen" (0.1.2).
QString toolKey(TimelineTool tool);
std::optional<TimelineTool> parseToolKey(const QString& key);

enum class GestureKind { None, Move, Trim, RippleTrim, Roll, Slip, Slide, RateStretch };

struct GestureSpec {
    GestureKind kind = GestureKind::None;
    /// Move and Slip: the clips to edit (linked partners follow unless `links` is IgnoreLinks). Trim, ripple
    /// trim, roll, slide and rate stretch: the grabbed clip is clips.front().
    std::vector<ClipId> clips;
    edit::Edge edge = edit::Edge::Tail;
    edit::MoveMode moveMode = edit::MoveMode::Overwrite;
    edit::LinkMode links = edit::LinkMode::FollowLinks;
    /// Timeline delta of the drag (for Slip the source delta: dragging left shows later source frames).
    Ticks delta;
    /// Move only: tracks crossed, counted outward from the video/audio divider.
    edit::TrackShift tracks;
    /// Move only: audio tracks to add below the last one first (dropping below the last track creates one).
    int newAudioTracks = 0;
};

/// The model edit for a gesture (pure; the state is not changed).
edit::EditOutcome applyGesture(const ProjectState& state, SequenceId sequence, const GestureSpec& spec);

/// Untranslated undo label of a gesture (translation context "wabcut::app::ProjectController").
const char* gestureLabel(const GestureSpec& spec);

struct SnapResult {
    Ticks delta;                   ///< the delta after snapping
    std::optional<Ticks> target;   ///< where it snapped (the indicator line), if a target was in reach
};

/// Auto-scroll while dragging near an edge of the view: the velocity (px/s, negative = towards the start) for a
/// pointer at `pointer` in a view `extent` px long. Zero outside the `zone` px next to each edge, then proportional
/// to how deep the pointer is in the zone, reaching `maxSpeed` at the edge and beyond it.
double autoScrollVelocity(double pointer, double extent, double zone, double maxSpeed);

/// Snaps a drag: of the `edges` (each moved by `delta`), the one closest to a snap target within `tolerance`
/// lands exactly on it. `sortedTargets` comes from edit::snapTargets(). Without a target in reach the delta is
/// returned unchanged.
SnapResult snapDelta(std::span<const Ticks> edges, Ticks delta, std::span<const Ticks> sortedTargets, Ticks tolerance);

} // namespace wabcut::app
