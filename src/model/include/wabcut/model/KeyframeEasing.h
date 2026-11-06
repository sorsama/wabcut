#pragma once

// Easing presets for keyframes (0.7, the Graph Editor): a library of named curves - the Penner easings (Sine, Quad, Cubic,
// Quart, Quint, Expo, Circ, Back) and the CSS ones (Ease, Ease In, Ease Out, Ease In Out) - that shape the segment between
// two keyframes as one unit-square cubic Bezier, the same four numbers every web easing table lists:
//
//     (0, 0) --- (x1, y1) --- (x2, y2) --- (1, 1)        x = time along the segment, y = share of the value change
//
// A Wabcut Bezier key stores its two sides as a handle each (Parameters.h BezierHandle: `influence` = how far along the
// segment the control point lies, `slope` = value units per second there), which is exactly such a curve: the leaving side
// of key A is (x1, y1) and the arriving side of key B is (1 - x2, 1 - y2), so a preset becomes two handles and nothing in
// the evaluator, the file format or the other views changes. Back and the like overshoot (y outside 0..1): the handle then
// points the other way, which a handle can do.
//
// Pure functions on a Curve (no project state): the Effect Controls bridge applies them to the curve it edits (one undo step)
// and the keyframes.ease command to the clip's parameter.

#include <wabcut/model/Parameters.h>

#include <cstddef>
#include <span>
#include <string_view>

namespace wabcut::edit {

struct EasingPreset {
    std::string_view id;    ///< stable: "easeInOutCubic"
    std::string_view name;  ///< English UI name (the app translates it): "Ease In Out Cubic"
    std::string_view group; ///< "Basic", "Sine", "Quad", "Cubic", "Quart", "Quint", "Expo", "Circ", "Back"
    double x1 = 1.0 / 3.0, y1 = 1.0 / 3.0, x2 = 2.0 / 3.0, y2 = 2.0 / 3.0; ///< the control points (unit square, x in 0..1)
};

/// Every preset, in menu order (Basic first).
std::span<const EasingPreset> easingPresets();
/// The preset with that id, or null.
const EasingPreset* findEasingPreset(std::string_view id);

/// The preset's value at `u` (0..1 along the segment, 0..1 of the value change; overshoots leave 0..1): the curve a segment
/// shaped by it draws, solved by bisection on x like Curve::valueAt does. Thumbnails and tests use it.
double easingValue(const EasingPreset& preset, double u);

/// Shapes the segment from key `index` to key `index + 1` like `preset`: both keys become Bezier with the preset's handles on
/// that segment. A key that was not Bezier keeps the shape of its OTHER side (a linear side stays straight, a flat ease side
/// stays flat), so only the one segment changes; a Hold key is a step that cannot be a handle, so as the arriving key of the
/// segment it stays Hold and only the leaving side of the first key is shaped. False (nothing changed) when `index` has no
/// next key or the two keys are at the same time.
bool applyEasing(Curve& curve, std::size_t index, const EasingPreset& preset);

/// The segment after every key in `indices` (the last key has none), in index order. How many segments were shaped.
std::size_t applyEasing(Curve& curve, std::span<const std::size_t> indices, const EasingPreset& preset);

} // namespace wabcut::edit
