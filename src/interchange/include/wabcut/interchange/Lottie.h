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
#include <vector>

namespace wabcut::interchange {

inline constexpr std::size_t kMaxLottieBytes = 16 * 1024 * 1024;
inline constexpr std::size_t kMaxLottieAssets = 1000;
inline constexpr std::size_t kMaxLottieLayersPerComp = 10'000;
/// A dotLottie archive (.lottie, a ZIP): the archive itself; its animation is bounded by kMaxLottieBytes.
inline constexpr std::size_t kMaxDotLottieBytes = 64u * 1024u * 1024u;

struct LottieImport {
    /// The design canvas is the animation's w x h; graphic time 0 is its in point (keys before it are kept).
    Graphic graphic;
    /// 0.7 (card 178): the template fields the animation offers - each text (a Text field, and a Color field for its fill)
    /// and each solid fill or stroke colour that does not change over the animation (a Color field), named after their
    /// layers; text first, at most kMaxTemplateFields. They bind layers of `graphic` (graphic.instance is not set).
    std::vector<TemplateField> fields;
    /// Out point - in point.
    Ticks duration{};
    /// The animation's frame rate (as a fraction: 29.97 is 30000/1001).
    Rate frameRate;
    ImportReport report;
};

/// Imports Lottie JSON. `fileName` names the source in the report. Errors: ParseError (not JSON, not a Lottie animation,
/// too big or deep, a bad frame rate, canvas or time range).
std::expected<LottieImport, Error> importLottie(std::string_view json, std::string_view fileName = "animation.json");

/// 0.7 (card 178): whether `bytes` start like a ZIP archive (a dotLottie file).
bool looksLikeZip(std::string_view bytes);

/// Imports a dotLottie archive (.lottie: a ZIP with manifest.json and the animations as animations/<id>.json - or a/<id>.json
/// in version 1 files; images, themes and state machines beside them). `animationId` chooses the animation (default: the
/// manifest's initial one, else the first). The archive is read with the bounded ZIP reader (Zip.h). What the importer
/// cannot use is in the report: the other animations, themes, state machines and images. Errors: ParseError (not a ZIP, no
/// animation, a damaged one, no such animation id), then importLottie's.
std::expected<LottieImport, Error> importDotLottie(std::string_view bytes, std::string_view fileName = "animation.lottie",
                                                   std::string_view animationId = {});

/// The ids of the animations a dotLottie archive holds (manifest order), for choosing one. Errors as importDotLottie's.
std::expected<std::vector<std::string>, Error> dotLottieAnimationIds(std::string_view bytes);

} // namespace wabcut::interchange
