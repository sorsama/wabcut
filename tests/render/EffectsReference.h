#pragma once

// CPU twins of shaders/fx.frag (the compositor's effect and transition passes), for golden-image tests.
//
// Images are linear Rec.709 with premultiplied alpha, one double RGBA per canvas pixel. The references assume the
// layer covers the canvas with an identity transform (frame pixel = canvas pixel = sequence pixel) and run at
// full resolution (preview divisor 1), which is how the golden tests set up their composites; the shader's maths
// is mirrored exactly (same tap counts, weights, constants and order of operations).

#include <wabcut/render/ColorPipeline.h>
#include <wabcut/render/OffscreenRenderer.h>
#include <wabcut/render/PreparedComposite.h>
#include <wabcut/render/reference/Image.h>
#include <wabcut/render/reference/Transitions.h>

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace wabcut::render::test {

using Rgba = std::array<double, 4>;

struct LinearImage {
    int width = 0;
    int height = 0;
    std::vector<Rgba> px;

    LinearImage() = default;
    LinearImage(int w, int h) : width(w), height(h), px(static_cast<std::size_t>(w) * static_cast<std::size_t>(h)) {}
    Rgba& at(int x, int y) { return px[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)]; }
    const Rgba& at(int x, int y) const {
        return px[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
    }
    /// Bilinear sample at canvas pixel coordinates (pixel centres at +0.5), clamped to the edge.
    Rgba sample(double x, double y) const;
};

/// An 8-bit straight-alpha, sRGB-encoded RGBA picture (what an untagged RGBA8 layer is) as a linear image.
LinearImage linearFromRgba8(int width, int height, const std::function<std::array<std::uint8_t, 4>(int, int)>& at);

/// One stack effect, as the compositor runs it on a full-canvas layer with an identity transform.
LinearImage referenceEffect(const LinearImage& in, const LayerEffect& effect);
/// A transition mix of A and B (null = transparent, a one-sided transition): render/reference/Transitions.h.
LinearImage referenceTransition(const LinearImage* a, const LinearImage* b, const LayerTransition& mix);

/// Pictures and transitions in the reference library's types (render/reference).
reference::Image toReference(const LinearImage& in);
LinearImage fromReference(const reference::Image& in);
reference::TransitionSpec referenceSpec(const LayerTransition& mix);

/// How a rendered picture matches its reference's encoded RGB24 (0.2 library tests): the PSNR, and the fraction of pixels
/// with a channel more than `tolerance` (of 255) off - a few pixels on either side of a hard threshold may flip between
/// the GPU's floats and the reference's doubles.
struct RenderMatch {
    double psnr = 0.0;
    double mismatched = 0.0;
};
RenderMatch compareToReference(const RenderedImage& image, const std::vector<std::uint8_t>& rgb24, int tolerance = 6);
/// `top` over `bottom` (premultiplied).
LinearImage referenceOver(const LinearImage& top, const LinearImage& bottom);

/// The encoded RGB24 output of `image` composited over black (OutputAlpha::Opaque) through `transform`.
std::vector<std::uint8_t> encodeOverBlack(const LinearImage& image, OutputTransform transform);

} // namespace wabcut::render::test
