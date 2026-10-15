#pragma once

// Lottie (Bodymovin) import (C.13, docs/graphics.md "Lottie"): an After Effects animation exported as Lottie JSON becomes
// a Wabcut graphic - layers with keyframes - for the subset the graphics model holds:
//   layers      shape, solid, text and precomp layers (a precomp is a group clipped to its size, its layers inside);
//               null and other parents through wrapper groups; in and out points, start offsets and time stretch;
//               hidden layers; blend modes; the Drop Shadow effect
//   shapes      groups, rectangles (rounded), ellipses, polygons and stars, paths (animated), fills, strokes (with
//               dashes), linear and radial gradient fills, trim paths, rounded corners on rectangles
//   transforms  anchor, position (also split), scale, rotation and opacity, with their easing as Bezier keys
//   text        the first text document (font, size, colour, alignment, tracking, leading, box) and range-selector
//               animators (opacity, position, scale, rotation)
//   masks       add, subtract and intersect; inverted; opacity; expansion; animated paths
// Everything else is in the report, like the timeline importers' (ImportReport.h): Approximated (imported, changed - the
// reason says how) or Dropped (images, effects other than drop shadows, track mattes, expressions, repeaters, merge
// paths, 3D...). Each layer imported as it was is Carried.
//
// Untrusted input: at most kMaxLottieBytes, nesting bounded, every count bounded (layers, assets, keys, vertices) and
// every number checked; the result passes graphicProblem() - an importer bug that broke that would be a ParseError whose
// message starts with "internal:", never an invalid graphic.

#include <wabcut/interchange/ImportReport.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Graphics.h>
#include <wabcut/model/Time.h>

#include <cstddef>
#include <expected>
#include <string_view>

namespace wabcut::interchange {

inline constexpr std::size_t kMaxLottieBytes = 16 * 1024 * 1024;
inline constexpr std::size_t kMaxLottieAssets = 1000;
inline constexpr std::size_t kMaxLottieLayersPerComp = 10'000;

struct LottieImport {
    /// The design canvas is the animation's w x h; graphic time 0 is its in point (keys before it are kept).
    Graphic graphic;
    /// Out point - in point.
    Ticks duration{};
    /// The animation's frame rate (as a fraction: 29.97 is 30000/1001).
    Rate frameRate;
    ImportReport report;
};

/// Imports Lottie JSON. `fileName` names the source in the report. Errors: ParseError (not JSON, not a Lottie animation,
/// too big or deep, a bad frame rate, canvas or time range).
std::expected<LottieImport, Error> importLottie(std::string_view json, std::string_view fileName = "animation.json");

} // namespace wabcut::interchange
