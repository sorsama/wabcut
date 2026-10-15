#pragma once

// Masks in the compositor (I.6, docs/masks.md): effect masks (an effect's result mixed into its input by coverage),
// opacity masks (the layer's alpha times coverage) and source-anchored redactions. Playback evaluates the model's
// masks at the frame (Masks.h) into a MaskSpec; the compositor renders it on the GPU (shaders/mask.frag):
//
//   1. per shape, the exact signed distance to its flattened outline (frame heights, + inside, even-odd), into a
//      texture over the shape's bounds (at least one frame), padded by kMaskSdfMargin on each side - cached by the outline, so a static
//      shape, or one only moved by its (tracked) transform, is never rasterised again;
//   2. per frame, the coverage: each shape's distance sampled through its inverse transform (bilinear: the
//      GPU-friendly approximation of the exact distance), the feather a smoothstep across it, the expansion an offset,
//      then opacity, invert and the add / subtract / intersect combination - cached while every input is unchanged;
//   3. the coverage sampled in canvas space through the layer's placement (MIX, ALPHA).
//
// maskCoverageCpu() is the double-precision reference with exact distances: the GPU result matches it within the PSNR
// the tests require (tests/render/test_masks.cpp).

#include <wabcut/model/Masks.h>
#include <wabcut/model/Time.h>
#include <wabcut/render/Geometry.h>
#include <wabcut/render/Matte.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace wabcut::render {

/// A closed outline's vertex: the point and its Bezier tangents as offsets (normalised frame coordinates).
struct OutlinePoint {
    Vec2 p;
    Vec2 in;
    Vec2 out;
};

/// Bezier segments are flattened into this many line segments (a 16-gon chord error is < 0.2 % of the radius).
inline constexpr int kMaskSegmentsPerCurve = 16;
/// Signed-distance textures pad the shape's bounds by at least this margin (normalised) on each side; more when the
/// shape's soft edge reaches further (feather / 2 + expansion), so its coverage never plateaus at the texture's edge.
inline constexpr double kMaskSdfMargin = 0.25;
/// The distance (frame heights, outside) of every point from a shape that covers nothing: fewer than three outline
/// points or a collapsed (singular) transform. Such a shape still combines like any other: inverted it covers the whole
/// frame, as an Intersect it clears it, as a Subtract it changes nothing.
inline constexpr double kMaskEmptyDistance = 1.0e4;

/// A closed cubic Bezier outline as a polygon (normalised frame coordinates), kMaskSegmentsPerCurve per curve; a
/// straight edge (both tangents zero) is one segment.
std::vector<Vec2> flattenOutline(std::span<const OutlinePoint> outline);

/// One mask of a list, at the frame.
struct MaskShapeSpec {
    /// The shape's polygon before its transform, closed (the last point connects to the first), normalised frame
    /// coordinates.
    std::vector<Vec2> polygon;
    /// The transform (the tracker's), normalised coordinates: (x', y') = (a x + b y + c, d x + e y + f).
    std::array<double, 6> affine{1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
    double feather = 0.0;   ///< soft edge width, frame heights
    double expansion = 0.0; ///< grow (+) / shrink (-), frame heights
    double opacity = 1.0;
    bool inverted = false;
    std::uint8_t mode = 0;  ///< 0 add, 1 subtract, 2 intersect
    /// AI-14 matte masks: the matte at the frame as a distance field (render/Matte.h). When set it replaces the
    /// polygon's distances (the polygon is then the matte's bounding box, unused); null for vector shapes and for a
    /// matte mask without a matte at the frame (the bounding box is drawn instead).
    std::shared_ptr<const MatteField> matte;
    /// Content hash of `polygon` (or the matte's identity) and the aspect (the distance texture's cache key).
    std::uint64_t key = 0;

    bool operator==(const MaskShapeSpec&) const = default;
};

/// A mask list at the frame: the coverage of a layer or effect.
struct MaskSpec {
    std::vector<MaskShapeSpec> shapes;
    /// Frame width / height in pixels (lengths and distances are isotropic in pixels).
    double aspect = 16.0 / 9.0;
    /// Hash of everything above (the coverage cache key).
    std::uint64_t key = 0;

    /// Computes `key` and every shape's `key`.
    void seal();
    bool operator==(const MaskSpec&) const = default;
};

/// Signed distance (frame heights, positive inside by the even-odd rule) from `p` to `polygon`, both in normalised
/// frame coordinates; `aspect` = width / height.
double maskSignedDistance(std::span<const Vec2> polygon, Vec2 p, double aspect);

/// CPU reference of the coverage over the frame ([0, 1]^2), `width` x `height` texels, top row first, exact distances.
/// A feather narrower than one texel is a one-texel antialiased edge.
std::vector<float> maskCoverageCpu(const MaskSpec& spec, int width, int height);

/// The enabled masks of a model list (model/Masks.h) at a curve time: outlines flattened, transform, edge and
/// combination at that time, sealed. Null when no mask is enabled (an effect then applies everywhere). `frameAspect` =
/// the frame's displayed width / height. Matte masks ask the installed MatteSource for the matte at `matteTime`, the
/// media time the frame shows (a remapped or frozen clip's curve time is not a media time); without it, at the curve
/// time. A field that is not usable (wrong size, non-finite distances) counts as no matte. `use` = Redaction fails
/// closed (render/Matte.h): without a usable matte, also for a matte mask without a matte reference, the bounding box
/// grown by a quarter.
std::shared_ptr<const MaskSpec> maskSpecAt(std::span<const Mask> masks, Ticks curveTime, double frameAspect,
                                           MatteUse use = MatteUse::Effect, std::optional<Ticks> matteTime = std::nullopt);

/// A redaction on a layer (CompositeLayer::redactions): applied to the layer's own pixels before any of its effects.
struct LayerRedaction {
    enum class Style : std::uint8_t { Blur, Mosaic, Solid };
    Style style = Style::Blur;
    /// Blur radius (Gaussian sigma = strength / 2) or mosaic cell, in frame heights.
    double strength = 0.05;
    /// Solid: display-referred colour 0..1.
    std::array<double, 3> color{};
    std::shared_ptr<const MaskSpec> mask;

    bool operator==(const LayerRedaction&) const = default;
};

} // namespace wabcut::render
