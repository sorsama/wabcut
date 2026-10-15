// ColorPipeline reference math (CPU): checked against published constants, not against itself.

#include <wabcut/render/ColorPipeline.h>
#include <wabcut/render/Compositor.h>
#include <wabcut/render/OffscreenRenderer.h>
#include <wabcut/render/PreparedComposite.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

using namespace wabcut;
using namespace wabcut::render;
using Catch::Approx;

namespace {

std::array<double, 3> decode8(ColorMatrix m, ColorRange r, int y, int cb, int cr) {
    return sampleToRgb(m, r, SampleEncoding::Bits8, false).apply({y / 255.0, cb / 255.0, cr / 255.0});
}

void checkRgb(const std::array<double, 3>& got, double r, double g, double b, double eps = 0.008) {
    INFO("got " << got[0] << ", " << got[1] << ", " << got[2]);
    CHECK(std::abs(got[0] - r) < eps);
    CHECK(std::abs(got[1] - g) < eps);
    CHECK(std::abs(got[2] - b) < eps);
}

} // namespace

TEST_CASE("Y'CbCr decoding matches SMPTE RP 219 / BT.601 75% bar code values", "[render][color]") {
    // BT.709 limited (RP 219): white, black, 75% red / green / blue.
    checkRgb(decode8(ColorMatrix::BT709, ColorRange::Limited, 235, 128, 128), 1, 1, 1);
    checkRgb(decode8(ColorMatrix::BT709, ColorRange::Limited, 16, 128, 128), 0, 0, 0);
    checkRgb(decode8(ColorMatrix::BT709, ColorRange::Limited, 51, 109, 212), 0.75, 0, 0);
    checkRgb(decode8(ColorMatrix::BT709, ColorRange::Limited, 133, 63, 52), 0, 0.75, 0);
    checkRgb(decode8(ColorMatrix::BT709, ColorRange::Limited, 28, 212, 120), 0, 0, 0.75);
    // BT.601 limited: 75% red / green / blue.
    checkRgb(decode8(ColorMatrix::BT601, ColorRange::Limited, 65, 100, 212), 0.75, 0, 0);
    checkRgb(decode8(ColorMatrix::BT601, ColorRange::Limited, 112, 72, 58), 0, 0.75, 0);
    checkRgb(decode8(ColorMatrix::BT601, ColorRange::Limited, 35, 212, 114), 0, 0, 0.75);
    // The same code values mean different colours under the other matrix.
    const auto wrong = decode8(ColorMatrix::BT601, ColorRange::Limited, 51, 109, 212);
    CHECK(std::abs(wrong[1] - 0.0) > 0.03);
    // Full range: 0..255 luma, chroma centred on 128.
    checkRgb(decode8(ColorMatrix::BT709, ColorRange::Full, 255, 128, 128), 1, 1, 1);
    checkRgb(decode8(ColorMatrix::BT709, ColorRange::Full, 0, 128, 128), 0, 0, 0);
    checkRgb(decode8(ColorMatrix::BT709, ColorRange::Full, 128, 128, 128), 128 / 255.0, 128 / 255.0,
             128 / 255.0);
}

TEST_CASE("10-bit samples decode from low-bit and P010 high-bit storage", "[render][color]") {
    const auto low = sampleToRgb(ColorMatrix::BT709, ColorRange::Limited, SampleEncoding::Bits10Low, false);
    checkRgb(low.apply({940 / 65535.0, 512 / 65535.0, 512 / 65535.0}), 1, 1, 1, 1e-6);
    checkRgb(low.apply({64 / 65535.0, 512 / 65535.0, 512 / 65535.0}), 0, 0, 0, 1e-6);
    const auto high = sampleToRgb(ColorMatrix::BT709, ColorRange::Limited, SampleEncoding::Bits10High, false);
    checkRgb(high.apply({940 * 64 / 65535.0, 512 * 64 / 65535.0, 512 * 64 / 65535.0}), 1, 1, 1, 1e-6);
    // 10-bit 75% red (BT.709): Y 204, Cb 435, Cr 848.
    checkRgb(low.apply({204 / 65535.0, 435 / 65535.0, 848 / 65535.0}), 0.75, 0, 0, 0.003);
    const auto full = sampleToRgb(ColorMatrix::BT709, ColorRange::Full, SampleEncoding::Bits10Low, false);
    checkRgb(full.apply({1023 / 65535.0, 512 / 65535.0, 512 / 65535.0}), 1, 1, 1, 1e-6);
}

TEST_CASE("RGB frames only get range scaling", "[render][color]") {
    checkRgb(
        sampleToRgb(ColorMatrix::RGB, ColorRange::Full, SampleEncoding::Bits8, true).apply({0.2, 0.4, 0.6}),
        0.2, 0.4, 0.6, 1e-9);
    checkRgb(sampleToRgb(ColorMatrix::RGB, ColorRange::Limited, SampleEncoding::Bits8, true)
                 .apply({235 / 255.0, 16 / 255.0, 125.5 / 255.0}),
             1, 0, 0.5, 1e-9);
}

TEST_CASE("Transfer functions match IEC 61966-2-1 and BT.709 and round-trip", "[render][color]") {
    CHECK(transferToLinear(TransferFunction::SRGB, 0.5) == Approx(0.214041).epsilon(1e-5));
    CHECK(transferToLinear(TransferFunction::SRGB, 0.04) == Approx(0.04 / 12.92));
    CHECK(linearToOutput(OutputTransform::SRGB, 0.214041) == Approx(0.5).epsilon(1e-5));
    CHECK(linearToOutput(OutputTransform::Rec709, 0.18) == Approx(0.409).epsilon(2e-3));
    CHECK(linearToOutput(OutputTransform::Rec709, 0.01) == Approx(0.045));
    CHECK(transferToLinear(TransferFunction::Gamma22, 0.5) == Approx(std::pow(0.5, 2.2)));
    for (int i = 0; i <= 100; ++i) {
        const double v = i / 100.0;
        CHECK(linearToOutput(OutputTransform::Rec709, transferToLinear(TransferFunction::BT709, v)) ==
              Approx(v).margin(1e-9));
        CHECK(linearToOutput(OutputTransform::SRGB, transferToLinear(TransferFunction::SRGB, v)) ==
              Approx(v).margin(1e-9));
        CHECK(linearToOutput(OutputTransform::Linear, v) == Approx(v));
    }
    // Out-of-range input is clamped, never NaN.
    CHECK(transferToLinear(TransferFunction::BT709, -0.2) == 0.0);
    CHECK(linearToOutput(OutputTransform::SRGB, 3.0) == Approx(1.0));
}

TEST_CASE("Primaries convert to Rec.709 (BT.2087 matrix, white preserved)", "[render][color]") {
    const Mat3 id = primariesToRec709(ColorPrimaries::BT709);
    CHECK(id.m == Mat3::identity().m);
    // ITU-R BT.2087-0, linear BT.2020 -> BT.709.
    const Mat3 m = primariesToRec709(ColorPrimaries::BT2020);
    const double expected[9] = {1.6605, -0.5876, -0.0728, -0.1246, 1.1329, -0.0083, -0.0182, -0.1006, 1.1187};
    for (int i = 0; i < 9; ++i) {
        CHECK(m.m[static_cast<std::size_t>(i)] == Approx(expected[i]).margin(2e-3));
    }
    for (ColorPrimaries p : {ColorPrimaries::BT601_525, ColorPrimaries::BT601_625, ColorPrimaries::BT2020,
                             ColorPrimaries::DisplayP3, ColorPrimaries::DCIP3}) {
        const auto white = primariesToRec709(p) * std::array<double, 3>{1, 1, 1};
        INFO("primaries " << static_cast<int>(p));
        checkRgb(white, 1, 1, 1, 1e-6);
    }
}

TEST_CASE("Untagged media resolve to the defined SDR fallback; HDR is flagged", "[render][color]") {
    const ColorPipeline pipeline;
    const ResolvedColor yuv = resolveSourceColor(ColorInfo{}, pipeline, false);
    CHECK(yuv.matrix == ColorMatrix::BT709);
    CHECK(yuv.range == ColorRange::Limited);
    CHECK(yuv.primaries == ColorPrimaries::BT709);
    CHECK(yuv.transfer == TransferFunction::BT709);
    CHECK(yuv.chromaLocation == ChromaLocation::Left);
    CHECK_FALSE(yuv.hdr);
    const ResolvedColor rgb = resolveSourceColor(ColorInfo{}, pipeline, true);
    CHECK(rgb.matrix == ColorMatrix::RGB);
    CHECK(rgb.range == ColorRange::Full);
    CHECK(rgb.transfer == TransferFunction::SRGB);
    ColorInfo tagged{ColorMatrix::BT601, ColorRange::Full, ColorPrimaries::BT601_625,
                     TransferFunction::Unknown, ChromaLocation::Center};
    const ResolvedColor sd = resolveSourceColor(tagged, pipeline, false);
    CHECK(sd.matrix == ColorMatrix::BT601);
    CHECK(sd.range == ColorRange::Full);
    CHECK(sd.primaries == ColorPrimaries::BT601_625);
    CHECK(sd.transfer == TransferFunction::BT709);
    CHECK(sd.chromaLocation == ChromaLocation::Center);
    tagged.transfer = TransferFunction::PQ;
    CHECK(resolveSourceColor(tagged, pipeline, false).hdr);
    CHECK(ColorPipeline{}.isSupportedVersion());
    ColorPipeline future;
    future.version = ColorPipeline::kCurrentVersion + 1;
    CHECK_FALSE(future.isSupportedVersion());
}

TEST_CASE("Layer transform maps source pixels like Premiere's Motion effect", "[render][transform]") {
    const CanvasSize canvas{256, 256};
    // Identity: a canvas-sized frame lands pixel for pixel.
    const Affine2D id = layerToCanvas({}, 256, 256, 1.0, canvas);
    CHECK(id.map({10, 20}).x == Approx(10));
    CHECK(id.map({10, 20}).y == Approx(20));
    // 64x32 frame, scale 2, rotate 90 deg clockwise, position (+10, -20): the frame's top-left quadrant
    // centre (16, 8) is 2 * (-16, -8) from the centre; rotated clockwise (y down) that is (16, -32); placed
    // at (138, 108).
    LayerTransform t;
    t.scale = 2.0;
    t.rotationDegrees = 90.0;
    t.position = {10, -20};
    const Affine2D m = layerToCanvas(t, 64, 32, 1.0, canvas);
    CHECK(m.map({16, 8}).x == Approx(154));
    CHECK(m.map({16, 8}).y == Approx(76));
    CHECK(m.map({32, 16}).x == Approx(138)); // frame centre -> position
    CHECK(m.map({32, 16}).y == Approx(108));
    // The anchor point (offset from the frame centre, source px) is what lands on the position.
    t.anchor = {16, 8};
    const Affine2D anchored = layerToCanvas(t, 64, 32, 1.0, canvas);
    CHECK(anchored.map({48, 24}).x == Approx(138));
    CHECK(anchored.map({48, 24}).y == Approx(108));
    // Inverse round-trips.
    const Vec2 back = anchored.inverted().map(anchored.map({5, 7}));
    CHECK(back.x == Approx(5));
    CHECK(back.y == Approx(7));
    // Anamorphic pixels stretch x before scaling.
    const Affine2D par = layerToCanvas({}, 100, 100, 2.0, canvas);
    CHECK(par.map({100, 50}).x - par.map({0, 50}).x == Approx(200));
}

TEST_CASE("Letterboxing keeps the canvas aspect, centred", "[render][transform]") {
    CHECK(Compositor::letterboxRect({1000, 1000}, {1920, 1080}) == QRect(0, 218, 1000, 563));
    CHECK(Compositor::letterboxRect({1920, 1080}, {1920, 1080}) == QRect(0, 0, 1920, 1080));
    CHECK(Compositor::letterboxRect({2000, 1000}, {1920, 1080}) == QRect(111, 0, 1778, 1000));
    CHECK(Compositor::letterboxRect({0, 100}, {1920, 1080}).isEmpty());
}

TEST_CASE("Offscreen backend policy mirrors the app's RhiPolicy", "[render][rhi]") {
#if defined(_WIN32)
    CHECK(resolveRhiBackend(RhiBackendRequest::PlatformDefault, "") == RhiBackendRequest::D3D11);
    CHECK(resolveRhiBackend(RhiBackendRequest::PlatformDefault, " D3D12 ") == RhiBackendRequest::D3D12);
    CHECK(resolveRhiBackend(RhiBackendRequest::PlatformDefault, "metal") == RhiBackendRequest::D3D11);
#endif
    CHECK(resolveRhiBackend(RhiBackendRequest::PlatformDefault, "gl") == RhiBackendRequest::OpenGL);
    CHECK(resolveRhiBackend(RhiBackendRequest::Null, "d3d12") == RhiBackendRequest::Null);
}
