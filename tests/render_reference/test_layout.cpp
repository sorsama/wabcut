// Reference layout effects (Layout.h): Gaussian Blur, masks, Border, Drop Shadow and the vertical blur fill.

#include "ReferenceTestSupport.h"

#include <wabcut/model/Effects.h>
#include <wabcut/model/Masks.h>
#include <wabcut/render/reference/Layout.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace wabcut;
using namespace wabcut::render::reference;
using Catch::Approx;

namespace {

constexpr double kPi = 3.14159265358979323846;

Mask shapeMask(MaskShape shape, double cx, double cy, double sx, double sy, double roundness = 0.0) {
    Mask m;
    m.id = 1;
    m.shape = shape;
    m.set("center.x", cx);
    m.set("center.y", cy);
    m.set("size.x", sx);
    m.set("size.y", sy);
    if (roundness > 0.0) {
        m.set("roundness", roundness);
    }
    return m;
}

/// An opaque mid-grey 32 x 32 layer (encoded 0.5).
Image greyLayer() { return Image::filled(32, 32, test::fromEncoded({0.5, 0.5, 0.5}, 1.0)); }

/// The facecam of a layout: the grey layer cut by a rounded rectangle (16 x 16 px, corner radius 4 px) or a circle
/// (radius 8 px), centred.
Image roundedFacecam() {
    return applyMask(greyLayer(), maskCoverage(shapeMask(MaskShape::Rectangle, 0.5, 0.5, 0.5, 0.5, 0.5), {}, 32, 32));
}

Image circleFacecam() {
    return applyMask(greyLayer(), maskCoverage(shapeMask(MaskShape::Ellipse, 0.5, 0.5, 0.5, 0.5), {}, 32, 32));
}

double coverageSum(const Image& coverage) {
    double sum = 0.0;
    for (int y = 0; y < coverage.height; ++y) {
        for (int x = 0; x < coverage.width; ++x) {
            sum += static_cast<double>(coverage.alpha(x, y));
        }
    }
    return sum;
}

Rgba channelSums(const Image& image) {
    Rgba sum{};
    for (std::size_t i = 0; i < image.rgba.size(); ++i) {
        sum[i % 4] += static_cast<double>(image.rgba[i]);
    }
    return sum;
}

/// The border hugs the layer: every pixel outside the layer (alpha < 0.5) within width - 1 of a layer pixel (alpha
/// >= 0.5, centre to centre) is border (alpha > 0.5 after the effect), none beyond width + 1 is. Returns the ring's
/// pixel count.
int checkRing(const Image& layer, const Image& bordered, double width) {
    std::vector<std::pair<int, int>> inside;
    for (int y = 0; y < layer.height; ++y) {
        for (int x = 0; x < layer.width; ++x) {
            if (layer.alpha(x, y) >= 0.5f) {
                inside.emplace_back(x, y);
            }
        }
    }
    REQUIRE_FALSE(inside.empty());
    int ring = 0;
    for (int y = 0; y < layer.height; ++y) {
        for (int x = 0; x < layer.width; ++x) {
            if (layer.alpha(x, y) >= 0.5f) {
                continue;
            }
            double nearest = std::numeric_limits<double>::infinity();
            for (const auto& [qx, qy] : inside) {
                nearest = std::min(nearest, std::hypot(static_cast<double>(qx - x), static_cast<double>(qy - y)));
            }
            const bool isRing = bordered.alpha(x, y) > 0.5f;
            ring += isRing ? 1 : 0;
            INFO("pixel " << x << ", " << y << " at " << nearest << " px from the layer");
            if (nearest <= width - 1.0) {
                CHECK(isRing);
            }
            if (nearest > width + 1.0) {
                CHECK_FALSE(isRing);
            }
        }
    }
    return ring;
}

} // namespace

// ---- Gaussian blur ------------------------------------------------------------------------------------------------

TEST_CASE("Layout reference: Gaussian blur keeps a constant and its energy", "[render_reference][blur]") {
    const Image constant = Image::filled(16, 16, {0.3f, 0.2f, 0.1f, 1.0f});
    CHECK(compare(gaussianBlur(constant, 9.0, true), constant).maxAbs <= 1e-6); // edges repeated: no darkening
    const Image transparentEdges = gaussianBlur(constant, 9.0, false);
    CHECK(transparentEdges.alpha(0, 0) < transparentEdges.alpha(8, 8)); // without: the edges darken
    CHECK(transparentEdges.alpha(8, 8) < 1.0f);
    CHECK(gaussianBlur(constant, 0.1, true) == constant); // sigma < 0.05: nothing to blur

    Image blob(32, 32);
    for (int y = 14; y < 18; ++y) {
        for (int x = 14; x < 18; ++x) {
            blob.set(x, y, test::premultiplied(0.5, 0.25, 1.0, 1.0));
        }
    }
    const Rgba before = channelSums(blob);
    for (const bool repeatEdges : {false, true}) {
        const Rgba after = channelSums(gaussianBlur(blob, 6.0, repeatEdges)); // 6 taps never reach the edges
        for (std::size_t c = 0; c < 4; ++c) {
            CHECK(after[c] == Approx(before[c]).epsilon(1e-5));
        }
    }
}

TEST_CASE("Layout reference: Gaussian blur sigma is radius over 3", "[render_reference][blur]") {
    Image impulse(33, 33);
    impulse.set(16, 16, {1.0f, 1.0f, 1.0f, 1.0f});
    const Image blurred = gaussianBlur(impulse, 9.0, false); // sigma 3
    const double centre = static_cast<double>(blurred.alpha(16, 16));
    CHECK(static_cast<double>(blurred.alpha(17, 16)) / centre == Approx(std::exp(-0.5 / 9.0)).epsilon(1e-5));
    CHECK(static_cast<double>(blurred.alpha(16, 19)) / centre == Approx(std::exp(-0.5)).epsilon(1e-5));
    CHECK(static_cast<double>(blurred.alpha(26, 16)) == 0.0); // taps = ceil(3 sigma) = 9: nothing at 10 px
    CHECK(blurred.alpha(25, 16) > 0.0f);
    // The kernel's weights sum to 1: its centre is 1 / (1 + 2 sum w)^2.
    double total = 1.0;
    for (int i = 1; i <= 9; ++i) {
        total += 2.0 * std::exp(-0.5 * i * i / 9.0);
    }
    CHECK(centre == Approx(1.0 / (total * total)).epsilon(1e-6));
}

TEST_CASE("Layout reference: Gaussian blur goldens", "[render_reference][blur][golden]") {
    // Checkers of 4 px in two colours with a transparent 6 x 6 corner.
    Image pattern(24, 24);
    for (int y = 0; y < 24; ++y) {
        for (int x = 0; x < 24; ++x) {
            if (x < 6 && y < 6) {
                continue;
            }
            pattern.set(x, y, (x / 4 + y / 4) % 2 == 0 ? test::premultiplied(0.8, 0.2, 0.1, 1.0)
                                                       : test::premultiplied(0.1, 0.3, 0.9, 1.0));
        }
    }
    test::checkGolden("gaussian_blur_repeat_edges", gaussianBlur(pattern, 6.0, true));
    test::checkGolden("gaussian_blur_transparent_edges", gaussianBlur(pattern, 6.0, false));
}

// ---- Masks --------------------------------------------------------------------------------------------------------

TEST_CASE("Layout reference: mask coverage follows the model outline", "[render_reference][mask]") {
    // A rectangle on pixel edges: exactly 1 inside and 0 outside, box-filtered or with the GPU's ramp.
    const Mask box = shapeMask(MaskShape::Rectangle, 0.5, 0.5, 0.5, 0.5);
    for (const int supersample : {1, 4}) {
        const Image c = maskCoverage(box, {}, 32, 32, supersample);
        for (int y = 0; y < 32; ++y) {
            for (int x = 0; x < 32; ++x) {
                const bool in = x >= 8 && x < 24 && y >= 8 && y < 24;
                CHECK(c.alpha(x, y) == (in ? 1.0f : 0.0f));
                CHECK(c.at(x, y)[0] == c.alpha(x, y));
            }
        }
    }
    // An edge through pixel centres covers half of them.
    const Mask half = shapeMask(MaskShape::Rectangle, 0.5, 0.5, 15.0 / 32.0, 0.5); // x from 8.5 to 23.5 px
    CHECK(maskCoverage(half, {}, 32, 32, 4).alpha(8, 16) == 0.5f);
    CHECK(maskCoverage(half, {}, 32, 32, 1).alpha(8, 16) == Approx(0.5).margin(1e-9));
    // A circle of radius 8 px: area pi r^2 (16 segments per quarter lose 0.16 %).
    const Mask circle = shapeMask(MaskShape::Ellipse, 0.5, 0.5, 0.5, 0.5);
    CHECK(coverageSum(maskCoverage(circle, {}, 32, 32)) == Approx(kPi * 64.0).epsilon(0.01));
    CHECK(coverageSum(maskCoverage(circle, {}, 32, 32, 1)) == Approx(kPi * 64.0).epsilon(0.01));
    // The rectangle rotated by 45 degrees keeps its area (the 4 x 4 points' diagonals run along its edges here: 258.75,
    // the exact count of those points); rounded corners lose (4 - pi) r^2.
    Mask turned = box;
    turned.set("rotation", 45.0);
    CHECK(coverageSum(maskCoverage(turned, {}, 32, 32)) == Approx(256.0).epsilon(0.015));
    CHECK(coverageSum(maskCoverage(turned, {}, 32, 32, 8)) == Approx(256.0).epsilon(0.002));
    const Mask rounded = shapeMask(MaskShape::Rectangle, 0.5, 0.5, 0.5, 0.5, 0.5); // r = 4 px
    CHECK(coverageSum(maskCoverage(rounded, {}, 32, 32)) == Approx(256.0 - (4.0 - kPi) * 16.0).epsilon(0.01));
    // Inverted, opacity, Subtract, disabled.
    const Image plain = maskCoverage(circle, {}, 32, 32);
    Mask inverted = circle;
    inverted.inverted = true;
    Mask faint = circle;
    faint.set("opacity", 0.5);
    Mask subtract = circle;
    subtract.mode = MaskMode::Subtract;
    Mask off = circle;
    off.enabled = false;
    const Image inv = maskCoverage(inverted, {}, 32, 32);
    const Image fnt = maskCoverage(faint, {}, 32, 32);
    const Image sub = maskCoverage(subtract, {}, 32, 32);
    const Image all = maskCoverage(off, {}, 32, 32);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const double c = static_cast<double>(plain.alpha(x, y));
            CHECK(static_cast<double>(inv.alpha(x, y)) == Approx(1.0 - c).margin(1e-7));
            CHECK(static_cast<double>(fnt.alpha(x, y)) == Approx(0.5 * c).margin(1e-7));
            CHECK(static_cast<double>(sub.alpha(x, y)) == Approx(1.0 - c).margin(1e-7));
            CHECK(all.alpha(x, y) == 1.0f);
        }
    }
    // Expansion grows the shape (frame heights: 2 px); feather softens it symmetrically about the edge.
    Mask grown = box;
    grown.set("expansion", 2.0 / 32.0);
    const Image g = maskCoverage(grown, {}, 32, 32);
    CHECK(g.alpha(6, 16) == 1.0f);
    CHECK(g.alpha(5, 16) == 0.0f);
    Mask feathered = box;
    feathered.set("feather", 4.0 / 32.0);
    const Image f = maskCoverage(feathered, {}, 32, 32);
    CHECK(f.alpha(16, 16) == 1.0f);
    CHECK(f.alpha(2, 16) == 0.0f);
    CHECK(static_cast<double>(f.alpha(7, 16) + f.alpha(8, 16)) == Approx(1.0).margin(1e-6)); // symmetric about x = 8
    for (int x = 1; x < 16; ++x) {
        CHECK(f.alpha(x, 16) >= f.alpha(x - 1, 16));
    }
}

TEST_CASE("Layout reference: an opacity mask multiplies every channel", "[render_reference][mask]") {
    const Image layer = Image::filled(4, 4, {0.4f, 0.2f, 0.1f, 0.8f});
    Image coverage(4, 4);
    coverage.set(1, 2, {0.5f, 0.5f, 0.5f, 0.5f});
    const Image masked = applyMask(layer, coverage);
    CHECK(masked.at(1, 2) == Pixel{0.2f, 0.1f, 0.05f, 0.4f});
    CHECK(masked.at(0, 0) == Pixel{0.0f, 0.0f, 0.0f, 0.0f});
}

// ---- Border -------------------------------------------------------------------------------------------------------

TEST_CASE("Layout reference: a border hugs a rounded rectangle", "[render_reference][border]") {
    const Image facecam = roundedFacecam();
    const Image bordered = border(facecam, 3.0, {1.0, 1.0, 1.0}, 1.0);
    checkRing(facecam, bordered, 3.0);
    // Along the middle row the ring is 3 px on each side (the box spans x 8..23).
    int left = 0;
    int right = 0;
    for (int x = 0; x < 32; ++x) {
        const bool ring = facecam.alpha(x, 16) < 0.5f && bordered.alpha(x, 16) > 0.5f;
        (x < 16 ? left : right) += ring ? 1 : 0;
    }
    CHECK(std::abs(left - 3) <= 1);
    CHECK(std::abs(right - 3) <= 1);
    // The border is white and opaque outside, the layer unchanged inside.
    CHECK(bordered.at(6, 16) == Pixel{1.0f, 1.0f, 1.0f, 1.0f});
    CHECK(bordered.at(16, 16) == facecam.at(16, 16));
    CHECK(bordered.at(1, 16) == Pixel{0.0f, 0.0f, 0.0f, 0.0f});
    // A rounded corner gets a rounded border: 3 px off a sharp corner, (6, 6) stays clear (3.8 px from the arc).
    CHECK(bordered.alpha(6, 6) == 0.0f);
    CHECK(border(applyMask(greyLayer(), maskCoverage(shapeMask(MaskShape::Rectangle, 0.5, 0.5, 0.5, 0.5), {}, 32, 32)),
                 3.0, {1.0, 1.0, 1.0}, 1.0)
              .alpha(6, 6) == 1.0f);
    CHECK(border(facecam, 0.0, {1.0, 1.0, 1.0}, 1.0) == facecam);
    CHECK(border(facecam, 3.0, {1.0, 1.0, 1.0}, 0.0) == facecam);
    test::checkGolden("border_rounded_rect", bordered);
}

TEST_CASE("Layout reference: a circle facecam gets a ring", "[render_reference][border]") {
    const Image facecam = circleFacecam();
    const BorderParams red{3.0, {1.0, 0.0, 0.0}, 1.0};
    const Image bordered = border(facecam, red);
    const int ring = checkRing(facecam, bordered, 3.0);
    // About pi ((8 + 3)^2 - 8^2) pixels.
    CHECK(static_cast<double>(ring) == Approx(kPi * (121.0 - 64.0)).epsilon(0.15));
    // No seam: the circle's anti-aliased edge pixels are opaque once the border is under them.
    int soft = 0;
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const float a = facecam.alpha(x, y);
            if (a > 0.0f && a < 1.0f) {
                ++soft;
                INFO("pixel " << x << ", " << y << " alpha " << a);
                CHECK(bordered.alpha(x, y) == Approx(1.0f).margin(1e-6));
            }
        }
    }
    CHECK(soft > 20);
    // The colour is display-referred (sRGB): red stays pure red.
    CHECK(bordered.at(16, 6) == Pixel{1.0f, 0.0f, 0.0f, 1.0f});
    test::checkGolden("border_circle", bordered);
}

TEST_CASE("Layout reference: border parameters follow the effect", "[render_reference][border]") {
    EffectInstance e;
    e.effect = std::string(kBorderEffect);
    CHECK(borderParams(e, {}) == BorderParams{});
    e.set("width", 6.0);
    e.set("color.g", 0.5);
    e.set("opacity", 0.25);
    const BorderParams p = borderParams(e, {});
    CHECK(p.width == 6.0);
    CHECK(p.color == Rgb{1.0, 0.5, 1.0});
    CHECK(p.opacity == 0.25);
}

// ---- Drop shadow --------------------------------------------------------------------------------------------------

TEST_CASE("Layout reference: drop shadow moves the alpha and goes under the layer", "[render_reference][shadow]") {
    Image dot(24, 24);
    dot.set(10, 10, {0.5f, 0.5f, 0.5f, 1.0f});
    DropShadowParams p;
    p.opacity = 0.5;
    p.angle = 90.0; // 0 = up, clockwise: to the right
    p.distance = 2.0;
    p.softness = 0.0;
    const Image hard = dropShadow(dot, p);
    CHECK(hard.at(10, 10) == dot.at(10, 10));
    CHECK(hard.at(12, 10)[3] == Approx(0.5f).margin(1e-6));
    CHECK(hard.at(12, 10)[0] == 0.0f); // black
    CHECK(hard.at(8, 10)[3] == 0.0f);
    CHECK(channelSums(hard)[3] == Approx(1.5).margin(1e-6));
    // Coloured (display-referred) and soft: the shadow blurred with sigma = softness / 3 (here 2: weights w(i) =
    // exp(-i^2 / 8) over 6 taps), under the opaque dot, which hides the shadow's share at its own pixel.
    p.color = {1.0, 0.0, 0.0};
    p.softness = 6.0;
    const Image soft = dropShadow(dot, p);
    double total = 1.0;
    for (int i = 1; i <= 6; ++i) {
        total += 2.0 * std::exp(-0.125 * i * i);
    }
    const double hidden = 0.5 * std::exp(-0.5) / (total * total); // the shadow at the dot: 2 px left of its centre
    CHECK(soft.at(10, 10) == dot.at(10, 10));
    CHECK(channelSums(soft)[3] == Approx(1.5 - hidden).epsilon(1e-5));
    CHECK(channelSums(soft)[0] == Approx(0.5 + 0.5 - hidden).epsilon(1e-5)); // the dot's red plus the shadow's
    CHECK(soft.at(12, 7)[3] > 0.0f);
    // Default direction (135 degrees): down and to the right.
    const Image facecam = border(roundedFacecam(), 2.0, {1.0, 1.0, 1.0}, 1.0);
    DropShadowParams layout;
    layout.opacity = 0.6;
    layout.distance = 4.0;
    layout.softness = 6.0;
    const Image shadowed = dropShadow(facecam, layout);
    CHECK(shadowed.alpha(27, 16) > 0.3f);
    CHECK(shadowed.alpha(27, 16) > shadowed.alpha(4, 16));
    CHECK(shadowed.at(16, 16) == facecam.at(16, 16)); // under the opaque layer nothing changes
    EffectInstance e;
    e.effect = "wabcut.dropShadow";
    CHECK(dropShadowParams(e, {}) == DropShadowParams{});
    e.set("angle", 45.0);
    CHECK(dropShadowParams(e, {}).angle == 45.0);
    test::checkGolden("drop_shadow_layout", shadowed);
}

// ---- Blur fill ----------------------------------------------------------------------------------------------------

TEST_CASE("Layout reference: blur fill covers a vertical canvas", "[render_reference][blurfill]") {
    // A 16:9 picture (32 x 18) with a gradient and stripes, into a 9:16 canvas (18 x 32).
    Image picture(32, 18);
    for (int y = 0; y < 18; ++y) {
        for (int x = 0; x < 32; ++x) {
            picture.set(x, y, test::premultiplied(x / 31.0, (x / 4) % 2 == 0 ? 0.8 : 0.2, y / 17.0, 1.0));
        }
    }
    const Image scaled = coverScale(picture, 18, 32);
    const double s = 32.0 / 18.0; // the height fills the canvas
    const Rgba centre = sampleBilinear(picture, (9.5 - 9.0) / s + 16.0, (16.5 - 16.0) / s + 9.0);
    const Pixel got = scaled.at(9, 16);
    for (std::size_t c = 0; c < 4; ++c) {
        CHECK(static_cast<double>(got[c]) == Approx(centre[c]).margin(1e-6));
    }
    const Image filled = blurFill(picture, 18, 32, 6.0);
    REQUIRE(filled.width == 18);
    REQUIRE(filled.height == 32);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 18; ++x) {
            CHECK(filled.alpha(x, y) == Approx(1.0f).margin(1e-6)); // every pixel covered, no dark edges
        }
    }
    test::checkGolden("blur_fill_vertical", filled);
}
