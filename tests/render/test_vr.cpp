// Card 38 (0.7): the VR viewer of the monitors on the GPU (the output pass looks around in an equirectangular canvas)
// against the CPU twin in model/Vr.h; VR Reorient is checked with the effect library's other effects
// (test_library_effects.cpp).

#include "EffectsReference.h"
#include "RenderTestSupport.h"

#include <wabcut/model/Vr.h>
#include <wabcut/render/Compositor.h>
#include <wabcut/render/OffscreenRenderer.h>

#include <QDir>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::render;
using namespace wabcut::render::test;

namespace {

constexpr int kPicW = 512;
constexpr int kPicH = 256;

std::uint8_t byte(double v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); }

/// A smooth, colourful sphere: colour varies with longitude and latitude, so a wrong direction shows at once.
std::array<std::uint8_t, 4> sphere(int x, int y) {
    const double u = (x + 0.5) / kPicW;
    const double v = (y + 0.5) / kPicH;
    const double kTwoPi = 6.283185307179586;
    return {byte(0.5 + 0.5 * std::sin(kTwoPi * u * 2.0)), byte(v), byte(0.5 + 0.5 * std::cos(kTwoPi * u * 3.0 + v * 3.0)), 255};
}

/// Two eyes in one frame: the left eye (first half) red, the right eye blue, both with the sphere's green.
std::array<std::uint8_t, 4> eyes(bool topBottom, int x, int y) {
    const bool second = topBottom ? y >= kPicH / 2 : x >= kPicW / 2;
    const double v = (y + 0.5) / kPicH;
    return {byte(second ? 0.05 : 0.9), byte(v), byte(second ? 0.9 : 0.05), 255};
}

struct ViewCase {
    const char* name;
    VrFormat format;
    float yaw;
    float pitch;
    float roll;
    float fov;
    int width;
    int height;
    int eye = 0;
};

/// The viewer's picture as the CPU computes it, through the same encoding as the export output.
std::vector<std::uint8_t> expectedView(const ViewCase& c, const std::function<std::array<std::uint8_t, 4>(int, int)>& picture) {
    const LinearImage source = linearFromRgba8(kPicW, kPicH, picture);
    LinearImage out(c.width, c.height);
    const vr::Mat3 rotation = vr::rotation(c.yaw, c.pitch, c.roll);
    for (int y = 0; y < c.height; ++y) {
        for (int x = 0; x < c.width; ++x) {
            const vr::EquirectUv uv = vr::viewUv(c.format, rotation, c.fov, static_cast<double>(c.width) / c.height,
                                                 (x + 0.5) / c.width, (y + 0.5) / c.height, c.eye);
            out.at(x, y) = uv.inside ? source.sample(uv.u * kPicW, uv.v * kPicH) : Rgba{0.0, 0.0, 0.0, 1.0};
        }
    }
    return encodeOverBlack(out, outputTransformFor(ColorPipeline{}, OutputRole::Export));
}

RenderedImage viewed(const ViewCase& c, const std::function<std::array<std::uint8_t, 4>(int, int)>& picture) {
    OffscreenOptions options;
    options.role = OutputRole::Export;
    options.vr.enabled = true;
    options.vr.format = c.format;
    options.vr.yaw = c.yaw;
    options.vr.pitch = c.pitch;
    options.vr.roll = c.roll;
    options.vr.fov = c.fov;
    options.vr.eye = c.eye;
    auto renderer = OffscreenRenderer::create(options);
    if (!renderer) {
        FAIL("OffscreenRenderer::create failed: " << renderer.error().message);
    }
    const auto composite = compositeOf({kPicW, kPicH}, {layerOf(makeRgbaFrame(kPicW, kPicH, picture))});
    auto image = (*renderer)->render(composite, QSize(c.width, c.height));
    REQUIRE(image);
    return std::move(*image);
}

} // namespace

TEST_CASE("VR viewer: the output pass looks around in a 360 canvas like the CPU twin says (card 38)", "[render][gpu][vr]") {
    const VrFormat mono{VrProjection::Equirectangular360, StereoLayout::Mono};
    const ViewCase cases[] = {
        {"ahead", mono, 0, 0, 0, 90, 320, 180},
        {"right_and_up", mono, 40, 20, 0, 70, 320, 180},
        {"wide_down_left", mono, -35, -25, 0, 120, 320, 180},
        {"tele", mono, 12, 5, 0, 30, 256, 256},
        {"tilted", mono, 25, 10, 20, 80, 320, 180},
        {"portrait", mono, 60, 0, 0, 60, 180, 320},
    };
    const QString dir = outputDir() + QStringLiteral("/vr");
    QDir().mkpath(dir);
    for (const ViewCase& c : cases) {
        DYNAMIC_SECTION(c.name) {
            const RenderedImage image = viewed(c, sphere);
            REQUIRE(image.width == c.width);
            REQUIRE(image.height == c.height);
            const RenderMatch m = compareToReference(image, expectedView(c, sphere));
            WARN(c.name << ": PSNR " << m.psnr << " dB, " << m.mismatched * 100.0 << " % of pixels off");
            savePng(image, QStringLiteral("vr/viewer_%1.png").arg(QString::fromLatin1(c.name)));
            CHECK(m.psnr >= 38.0);
            CHECK(m.mismatched <= 0.01);
        }
    }
}

TEST_CASE("VR viewer: a stereoscopic canvas is seen through one eye and a 180 canvas has an edge (card 38)", "[render][gpu][vr]") {
    for (const bool topBottom : {true, false}) {
        DYNAMIC_SECTION(std::string(topBottom ? "top-bottom" : "side-by-side")) {
            const VrFormat format{VrProjection::Equirectangular360, topBottom ? StereoLayout::TopBottom : StereoLayout::SideBySide};
            const auto picture = [topBottom](int x, int y) { return eyes(topBottom, x, y); };
            for (const int eye : {0, 1}) {
                const ViewCase c{"eye", format, 20, 0, 0, 90, 160, 90, eye};
                const RenderedImage image = viewed(c, picture);
                // The left eye is red, the right blue: the view shows only the eye asked for.
                for (const std::array<int, 2>& at : {std::array<int, 2>{20, 20}, std::array<int, 2>{80, 45}, std::array<int, 2>{140, 70}}) {
                    const Rgb p = pixelOf(image, at[0], at[1]);
                    CHECK((eye == 0 ? p[0] > p[2] + 0.4 : p[2] > p[0] + 0.4));
                }
                const RenderMatch m = compareToReference(image, expectedView(c, picture));
                CHECK(m.psnr >= 38.0);
            }
        }
    }
    // A 180 canvas seen from the side: looking at its right edge leaves black where the sphere has no picture.
    const VrFormat half{VrProjection::Equirectangular180, StereoLayout::Mono};
    const ViewCase edge{"edge", half, 80, 0, 0, 90, 320, 180};
    const RenderedImage image = viewed(edge, sphere);
    const Rgb inside = pixelOf(image, 10, 90);
    const Rgb beyond = pixelOf(image, 310, 90);
    CHECK(inside[0] + inside[1] + inside[2] > 0.3);
    CHECK(beyond[0] + beyond[1] + beyond[2] < 0.02);
    const RenderMatch m = compareToReference(image, expectedView(edge, sphere));
    CHECK(m.psnr >= 38.0);
}

TEST_CASE("VR viewer: off and the output pass is the ordinary letterboxed canvas (card 38)", "[render][gpu][vr]") {
    // No viewer: the canvas fills a target of its own shape exactly as before.
    auto renderer = makeRenderer(OutputRole::Export);
    const auto composite = compositeOf({kPicW, kPicH}, {layerOf(makeRgbaFrame(kPicW, kPicH, sphere))});
    auto plain = renderer->render(composite);
    REQUIRE(plain);
    const LinearImage source = linearFromRgba8(kPicW, kPicH, sphere);
    const std::vector<std::uint8_t> expected = encodeOverBlack(source, outputTransformFor(ColorPipeline{}, OutputRole::Export));
    const RenderMatch m = compareToReference(*plain, expected);
    CHECK(m.psnr >= 40.0);
    // A viewer on a flat canvas format (none) is ignored too.
    OffscreenOptions options;
    options.vr.enabled = true;
    options.vr.format = VrFormat{};
    auto ignored = OffscreenRenderer::create(options);
    REQUIRE(ignored);
    auto same = (*ignored)->render(composite);
    REQUIRE(same);
    CHECK(compareToReference(*same, expected).psnr >= 40.0);
}
