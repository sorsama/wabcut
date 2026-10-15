#pragma once

// Reference layout effects (C.15, docs/internal/c15-parity-handoff.md "Layouts"): Gaussian Blur, Drop Shadow, masks (rounded
// rectangles, circles), Border and the vertical blur fill. The effects that already run on the GPU mirror the
// compositor's math (shaders/fx.frag, Compositor.cpp, and its CPU twin tests/render/EffectsReference.cpp) on a layer
// that covers the canvas with an identity transform at preview divisor 1: one picture pixel = one canvas texel = one
// sequence pixel. Pictures are linear premultiplied RGBA (Image.h).

#include <wabcut/model/Effects.h>
#include <wabcut/model/Masks.h>
#include <wabcut/model/Time.h>
#include <wabcut/render/reference/Image.h>

#include <array>
#include <vector>

namespace wabcut::render::reference {

// ---- Gaussian blur (wabcut.blur.gaussian) ---------------------------------------------------------------------------

/// Gaussian blur of `radius` pixels: sigma = max(0, radius) / 3 (PreparedComposite.h EffectType::GaussianBlur,
/// Compositor.cpp applyEffect). Same as gaussianBlurSigma(picture, sigma, repeatEdges).
Image gaussianBlur(const Image& picture, double radius, bool repeatEdges);

/// The compositor's separable Gaussian (fx.frag blur(), one horizontal then one vertical pass), per pass and pixel:
///   sigma < 0.05: no change
///   taps = ceil(3 sigma),  w(i) = exp(-0.5 i^2 / sigma^2)
///   out(x) = (P(x) + sum_{i=1..taps} w(i) (P(x + i) + P(x - i))) / (1 + 2 sum_{i=1..taps} w(i))
/// P outside the picture: the edge pixel when `repeatEdges` (clamp), else transparent black (the edges darken: the
/// weights are not renormalised). The intermediate between the passes is double precision.
/// GPU difference: above sigma = 24 texels (kMaxBlurSigma) the compositor samples a coarser mip level with sigma / 2^lod
/// instead of adding taps (and caps taps at 96); the reference always runs the full-resolution kernel, which that mip
/// path approximates. At or below 24 the two are the same kernel (taps <= 72).
Image gaussianBlurSigma(const Image& picture, double sigma, bool repeatEdges);

// ---- Drop shadow (wabcut.dropShadow) --------------------------------------------------------------------------------

/// wabcut.dropShadow's parameters with their descriptor defaults (Effects.cpp kDropShadowParams).
struct DropShadowParams {
    double opacity = 0.5;
    double angle = 135.0;   ///< degrees, 0 = up, clockwise
    double distance = 10.0; ///< pixels
    double softness = 10.0; ///< pixels, like a blur radius (sigma = softness / 3)
    Rgb color{0.0, 0.0, 0.0}; ///< "red", "green", "blue": display-referred 0..1 (srgbToLinear)

    bool operator==(const DropShadowParams&) const = default;
};
DropShadowParams dropShadowParams(const EffectInstance& instance, Ticks curveTime);

/// The compositor's drop shadow (fx.frag MODE_SHADOW + blur + MODE_OVER; Compositor.cpp applyEffect DropShadow):
///   (dx, dy) = max(0, distance) (sin(angle), -cos(angle))              (y down: 135 degrees = down and to the right)
///   a(x, y)  = sampleBilinear(layer, x + 0.5 - dx, y + 0.5 - dy).alpha, 0 where that point is outside [0, w] x [0, h]
///   shadow   = (srgbToLinear(color) k, k),  k = a clamp(opacity, 0, 1)
///   shadow   = gaussianBlurSigma(shadow, max(0, softness) / 3, repeatEdges = false)   when that sigma >= 0.05
///   out      = layer + shadow (1 - layer.alpha)                                         (the layer over its shadow)
/// Apply it after the masks and the border so the shadow follows the final alpha.
Image dropShadow(const Image& layer, const DropShadowParams& params);

// ---- Masks (wabcut.mask, I.6) ---------------------------------------------------------------------------------------

/// The coverage of one model mask over a width x height picture of the layer's SOURCE frame (normalised coordinates
/// (0, 0)..(1, 1), aspect = width / height, square pixels), as an Image whose every pixel is (c, c, c, c).
///   outline   maskOutlineAt(mask, curveTime, aspect): the model's own outline, its transform.* applied
///   polygon   each cubic segment flattened into 16 line segments (a straight edge, both tangents zero, into one), as
///             render::flattenOutline (kMaskSegmentsPerCurve)
///   d(p)      the signed distance (frame heights, + inside by the even-odd rule) from p to the polygon in aspect
///             space (x times aspect), as render::maskSignedDistance; fewer than 3 points: d = -1e4 (covers nothing)
///   edge      e = d + expansion, f = max(0, feather), aa = 1 / height (one pixel in frame heights):
///             f > aa:            c = smoothstep(-f/2, f/2, e) at the pixel centre (the GPU's feather)
///             supersample <= 1:  c = clamp(0.5 + e / aa, 0, 1) at the pixel centre (the GPU's one-texel ramp,
///                                exactly render::maskCoverageCpu)
///             supersample = n:   c = the share of the n x n points (x + (i + 0.5) / n, y + (j + 0.5) / n) with e > 0
///                                (the box-filtered area of the hard-edged shape)
///   then      c = inverted ? 1 - c : c;  c *= clamp(opacity, 0, 1);  a Subtract mask (it starts from full) gives 1 - c
/// A disabled mask covers everything (c = 1). A Matte-shaped mask (AI-14) has no matte here: its outline is its bounding
/// box, as the model's maskOutlineAt gives it. The GPU applies the mask transform to the sample point and scales the
/// untransformed shape's distance by sqrt|det|; the reference measures the transformed outline: the same for rotation,
/// uniform scale and offset.
Image maskCoverage(const Mask& mask, Ticks curveTime, int width, int height, int supersample = 4);

/// A model outline flattened into a closed polygon (normalised coordinates), as described for maskCoverage().
std::vector<std::array<double, 2>> flattenMaskOutline(const std::vector<MaskVertex>& outline);
/// Signed distance (frame heights, + inside by the even-odd rule) from `p` to a closed polygon, both normalised;
/// `aspect` = width / height. Fewer than 3 points: -1e9.
double polygonSignedDistance(const std::vector<std::array<double, 2>>& polygon, double px, double py, double aspect);

/// An opacity mask (EffectType::OpacityMask, mask.frag MODE_ALPHA): every channel of `layer` times the coverage's alpha
/// at the same pixel (0 outside the coverage picture).
Image applyMask(const Image& layer, const Image& coverage);

// ---- Border (wabcut.border, C.15) -----------------------------------------------------------------------------------

/// wabcut.border's parameters with their descriptor defaults (Effects.cpp kBorderParams).
struct BorderParams {
    double width = 4.0;     ///< sequence pixels
    Rgb color{1.0, 1.0, 1.0}; ///< "color.r/g/b": display-referred 0..1 (srgbToLinear, like the drop shadow's colour)
    double opacity = 1.0;

    bool operator==(const BorderParams&) const = default;
};
BorderParams borderParams(const EffectInstance& instance, Ticks curveTime);

/// A stroke of `width` pixels outside the layer's alpha edge, under the layer:
///   a      = clamp(layer.alpha, 0, 1)
///   a_d(p) = max a(q) over pixel centres q inside the picture with |q - p| <= width   (a disc dilation)
///   b      = clamp(a_d, 0, 1) clamp(opacity, 0, 1)
///   out    = layer + (srgbToLinear(color) b, b) (1 - a)            (the layer over the dilated shape in the border colour)
/// The visible stroke is b (1 - a): where the layer is absent (a = 0) it is the handoff's clamp(a_d - a, 0, 1) exactly,
/// and under an opaque layer (a = 1) nothing shows, as there. Between, the handoff's literal "layer over (clamp(a_d - a)
/// x colour)" would apply the layer's coverage twice and leave a see-through seam at the anti-aliased edge (alpha
/// a + (1 - a)^2: 0.75 at a = 0.5); drawing the whole dilated shape under the layer closes it (alpha 1 there).
/// width <= 0 or opacity <= 0: the layer unchanged. A rounded box or a circle gets a rounded border: it follows alpha.
Image border(const Image& layer, double width, const Rgb& color, double opacity);
Image border(const Image& layer, const BorderParams& params);

// ---- Blur fill (layout verticalBlurFill) ----------------------------------------------------------------------------

/// The picture scaled to cover a canvasWidth x canvasHeight canvas, centred (Motion "scale to fill"):
///   s = max(cw / pw, ch / ph);  out(x, y) = sampleBilinear(picture, (x + 0.5 - cw / 2) / s + pw / 2,
///                                                                  (y + 0.5 - ch / 2) / s + ph / 2)
/// The frame covers the canvas, so the layer pass's analytic edge coverage (layer.frag) is 1 on every canvas pixel.
Image coverScale(const Image& picture, int canvasWidth, int canvasHeight);

/// The blurred background of a vertical layout: gaussianBlur(coverScale(picture, cw, ch), blurRadius, repeatEdges =
/// true), so an opaque picture fills every canvas pixel (alpha 1) and the edges do not darken.
Image blurFill(const Image& picture, int canvasWidth, int canvasHeight, double blurRadius);

} // namespace wabcut::render::reference
