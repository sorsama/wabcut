// ColorPipeline reference math (CPU): checked against published constants, not against itself.

#include <wabcut/render/ColorPipeline.h>
#include <wabcut/render/Compositor.h>
#include <wabcut/render/OffscreenRenderer.h>
#include <wabcut/render/PreparedComposite.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
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

TEST_CASE("Scale Width stretches the layer along its own axes, before the clip's rotation (card 12)", "[render][transform]") {
    const CanvasSize canvas{256, 256};
    LayerTransform t;
    t.setScaleXY(2.0, 0.5); // twice as wide, half as high
    CHECK(t.scale == 0.5);
    CHECK(t.aspectX == 4.0);
    CHECK(t.scaleX() == 2.0);
    CHECK(t.hasArea());
    // An uniform transform keeps aspectX exactly 1: nothing changes for a clip that was never stretched.
    LayerTransform plain;
    plain.setScaleXY(0.75, 0.75);
    CHECK(plain.aspectX == 1.0);
    CHECK(plain == LayerTransform{.scale = 0.75});
    // A zero axis leaves nothing to draw.
    LayerTransform flat;
    flat.setScaleXY(0.0, 1.0);
    CHECK_FALSE(flat.hasArea());
    flat.setScaleXY(1.0, 0.0);
    CHECK_FALSE(flat.hasArea());

    // 64 x 32 frame: x spans 2 * 64 = 128, y 0.5 * 32 = 16 around the centre.
    const Affine2D m = layerToCanvas(t, 64, 32, 1.0, canvas);
    CHECK(m.map({0, 0}).x == Approx(128 - 64));
    CHECK(m.map({64, 0}).x == Approx(128 + 64));
    CHECK(m.map({0, 0}).y == Approx(128 - 8));
    CHECK(m.map({0, 32}).y == Approx(128 + 8));
    // Rotation applies after the stretch: a quarter turn makes the wide frame tall.
    t.rotationDegrees = 90.0;
    const Affine2D turned = layerToCanvas(t, 64, 32, 1.0, canvas);
    CHECK(turned.map({0, 16}).y == Approx(128 - 64));
    CHECK(turned.map({64, 16}).y == Approx(128 + 64));
    CHECK(turned.map({32, 0}).x == Approx(128 + 8));
}

TEST_CASE("A stretched clip of a turned stream keeps Scale Width along the displayed width (card 12)", "[render][transform]") {
    // A phone clip stored 64 x 32, displayed turned a quarter clockwise as 32 x 64. Scale Width 2 / Scale Height 0.5 is
    // 2 wide and 0.5 high in the picture as shown: 64 x 32 on the canvas, whatever the stored axes are.
    LayerTransform t;
    t.setScaleXY(2.0, 0.5);
    applyDisplayOrientation(t, 1, false);
    CHECK(t.rotationDegrees == Approx(90.0));
    const CanvasSize canvas{256, 256};
    const Affine2D m = layerToCanvas(t, 64, 32, 1.0, canvas);
    double x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
    for (const Vec2 p : {Vec2{0, 0}, Vec2{64, 0}, Vec2{64, 32}, Vec2{0, 32}}) {
        x0 = std::min(x0, m.map(p).x);
        x1 = std::max(x1, m.map(p).x);
        y0 = std::min(y0, m.map(p).y);
        y1 = std::max(y1, m.map(p).y);
    }
    CHECK(x1 - x0 == Approx(2.0 * 32.0)); // displayed width 32 * Scale Width 2
    CHECK(y1 - y0 == Approx(0.5 * 64.0)); // displayed height 64 * Scale Height 0.5
    // Half turns and no turn do not swap.
    LayerTransform half;
    half.setScaleXY(2.0, 0.5);
    applyDisplayOrientation(half, 2, false);
    CHECK(half.scaleX() == Approx(2.0));
    CHECK(half.scale == Approx(0.5));
}

TEST_CASE("A display orientation places a stored frame the way the thumbnails turn it (card 154)", "[render][transform]") {
    // FrameHandle.h: rotate clockwise by rotationDegrees, then mirror the rotated picture horizontally. Where a stored
    // point (x, y) of a w x h frame lands in the displayed picture, by that definition:
    constexpr double w = 64.0;
    constexpr double h = 32.0;
    const auto displayed = [&](int rotation, bool mirrored, Vec2 p) {
        double dw = w;
        Vec2 q = p;
        switch (rotation) {
        case 90:
            q = {h - p.y, p.x};
            dw = h;
            break;
        case 180:
            q = {w - p.x, h - p.y};
            break;
        case 270:
            q = {p.y, w - p.x};
            dw = h;
            break;
        default:
            break;
        }
        if (mirrored) {
            q.x = dw - q.x;
        }
        return q;
    };
    for (const int rotation : {0, 90, 180, 270}) {
        for (const bool mirrored : {false, true}) {
            INFO("rotation " << rotation << (mirrored ? " mirrored" : ""));
            const bool sideways = rotation == 90 || rotation == 270;
            const CanvasSize canvas{sideways ? 32 : 64, sideways ? 64 : 32};
            LayerTransform t;
            applyDisplayOrientation(t, rotation / 90, mirrored);
            CHECK(t.mirrorX == mirrored);
            const Affine2D m = layerToCanvas(t, w, h, 1.0, canvas);
            for (const Vec2 p : {Vec2{0, 0}, Vec2{w, 0}, Vec2{w, h}, Vec2{0, h}, Vec2{10, 5}, Vec2{50, 30}}) {
                const Vec2 want = displayed(rotation, mirrored, p);
                CHECK(m.map(p).x == Approx(want.x).margin(1e-9));
                CHECK(m.map(p).y == Approx(want.y).margin(1e-9));
            }
            // The clip's anchor is given in displayed pixels from the displayed centre: the stored point that displays
            // there lands on the canvas centre plus the position, whatever the orientation.
            const Vec2 stored{10, 5};
            const Vec2 shown = displayed(rotation, mirrored, stored);
            LayerTransform moved;
            moved.anchor = {shown.x - canvas.width / 2.0, shown.y - canvas.height / 2.0};
            moved.position = {7, -3};
            applyDisplayOrientation(moved, rotation / 90, mirrored);
            const Affine2D anchored = layerToCanvas(moved, w, h, 1.0, canvas);
            CHECK(anchored.map(stored).x == Approx(canvas.width / 2.0 + 7.0).margin(1e-9));
            CHECK(anchored.map(stored).y == Approx(canvas.height / 2.0 - 3.0).margin(1e-9));
        }
    }
    // A flip is a reflection: the matrix's determinant is negative, and the inverse still round-trips.
    LayerTransform flip;
    flip.mirrorX = true;
    const Affine2D reflected = layerToCanvas(flip, 64, 32, 1.0, {64, 32});
    CHECK(reflected.determinant() < 0.0);
    CHECK(reflected.map({0, 10}).x == Approx(64.0));
    CHECK(reflected.inverted().map(reflected.map({12, 9})).x == Approx(12.0));
    CHECK(transformQuarterTurns(1, true) == 3);
    CHECK(transformQuarterTurns(1, false) == 1);
    CHECK(transformQuarterTurns(0, true) == 0);
}

TEST_CASE("Letterboxing keeps the canvas aspect, centred", "[render][transform]") {
    CHECK(Compositor::letterboxRect({1000, 1000}, {1920, 1080}) == QRect(0, 218, 1000, 563));
    CHECK(Compositor::letterboxRect({1920, 1080}, {1920, 1080}) == QRect(0, 0, 1920, 1080));
    CHECK(Compositor::letterboxRect({2000, 1000}, {1920, 1080}) == QRect(111, 0, 1778, 1000));
    CHECK(Compositor::letterboxRect({0, 100}, {1920, 1080}).isEmpty());
}

TEST_CASE("layerToCanvas puts a frame's pixels over the canvas pixel aspect ratio (145 156)", "[render][transform]") {
    const CanvasSize hdv{1440, 1080, 4.0 / 3.0}; // an HDV sequence: its pixels are 4:3, seen as 1920x1080
    // An HDV frame (1440x1080, 4:3 pixels) fills the HDV canvas one to one.
    const Affine2D one = layerToCanvas({}, 1440, 1080, 4.0 / 3.0, hdv);
    CHECK(one.map({0, 0}).x == Approx(0));
    CHECK(one.map({1440, 1080}).x == Approx(1440));
    CHECK(one.map({1440, 1080}).y == Approx(1080));
    // A square-pixel 1920x1080 picture is seen 1920 wide: 1440 canvas pixels.
    const Affine2D square = layerToCanvas({}, 1920, 1080, 1.0, hdv);
    CHECK(square.map({1920, 0}).x - square.map({0, 0}).x == Approx(1440));
    CHECK(square.map({0, 1080}).y - square.map({0, 0}).y == Approx(1080));
    // A square canvas (the default) is unchanged by the field.
    const CanvasSize plain{1920, 1080};
    CHECK(layerToCanvas({}, 1920, 1080, 1.0, plain).a == Approx(1.0));
    CHECK(layerToCanvas({}, 1440, 1080, 4.0 / 3.0, plain).a == Approx(4.0 / 3.0));
    // Nothing usable as a ratio: square.
    CHECK(layerToCanvas({}, 100, 100, 1.0, CanvasSize{1920, 1080, 0.0}).a == Approx(1.0));
}

TEST_CASE("A rotated clip stays a rigid picture on a non-square sequence (145 156)", "[render][transform]") {
    const CanvasSize hdv{1440, 1080, 4.0 / 3.0}; // seen as 1920x1080
    // A phone clip stored 1920x1080 with a quarter turn, scaled to fit the seen frame: 1080x1920 seen -> s = 0.5625, so it
    // is 607.5 seen (455.6 canvas pixels) wide and 1080 high, centred: a 9:16 picture. Before the fix the canvas ratio
    // sat before the turn and the picture came out one to one (810 x 810 seen).
    LayerTransform phone;
    phone.rotationDegrees = 90.0;
    phone.scale = 0.5625;
    const Affine2D m = layerToCanvas(phone, 1920, 1080, 1.0, hdv);
    double minX = 1e9;
    double maxX = -1e9;
    double minY = 1e9;
    double maxY = -1e9;
    for (const Vec2 corner : {Vec2{0, 0}, Vec2{1920, 0}, Vec2{1920, 1080}, Vec2{0, 1080}}) {
        const Vec2 at = m.map(corner);
        minX = std::min(minX, at.x);
        maxX = std::max(maxX, at.x);
        minY = std::min(minY, at.y);
        maxY = std::max(maxY, at.y);
    }
    CHECK(maxX - minX == Approx(607.5 * 3.0 / 4.0).margin(1e-6)); // 455.625 canvas pixels
    CHECK(maxY - minY == Approx(1080.0).margin(1e-6));
    CHECK((maxX - minX) * hdv.pixelAspect / (maxY - minY) == Approx(9.0 / 16.0).margin(1e-9)); // seen shape
    CHECK((minX + maxX) / 2.0 == Approx(720.0).margin(1e-6));                                  // still centred
    CHECK((minY + maxY) / 2.0 == Approx(540.0).margin(1e-6));

    // A 30 degree Motion rotation turns the picture without shearing it: its top and left edges stay at a right angle in
    // the shape they are seen in (the canvas x stretched by its ratio). It used to meet at 104 degrees.
    LayerTransform tilted;
    tilted.rotationDegrees = 30.0;
    tilted.scale = 0.75;
    const Affine2D t = layerToCanvas(tilted, 1920, 1080, 1.0, hdv);
    const Vec2 o = t.map({0, 0});
    const Vec2 right = t.map({1920, 0});
    const Vec2 down = t.map({0, 1080});
    const double ex = (right.x - o.x) * hdv.pixelAspect;
    const double ey = right.y - o.y;
    const double fx = (down.x - o.x) * hdv.pixelAspect;
    const double fy = down.y - o.y;
    CHECK(ex * fx + ey * fy == Approx(0.0).margin(1e-6));
    // The edges keep the lengths of the frame at that scale (1920 x 1080 seen at 0.75).
    CHECK(std::hypot(ex, ey) == Approx(1920.0 * 0.75).margin(1e-6));
    CHECK(std::hypot(fx, fy) == Approx(1080.0 * 0.75).margin(1e-6));
    // The same call on a square canvas is the rigid turn it always was.
    const Affine2D plain = layerToCanvas(tilted, 1920, 1080, 1.0, CanvasSize{1920, 1080});
    const Vec2 po = plain.map({0, 0});
    const Vec2 pr = plain.map({1920, 0});
    const Vec2 pd = plain.map({0, 1080});
    CHECK((pr.x - po.x) * (pd.x - po.x) + (pr.y - po.y) * (pd.y - po.y) == Approx(0.0).margin(1e-6));
}

TEST_CASE("Letterboxing shows an anamorphic canvas in the shape the sequence pixel aspect ratio gives it (156)",
          "[render][transform]") {
    // HDV: 1440x1080 with 4:3 pixels is seen as 16:9, as the 1920x1080 above; with square pixels it would be 4:3.
    CHECK(Compositor::letterboxRect({1000, 1000}, {1440, 1080}, 4.0 / 3.0) == QRect(0, 218, 1000, 563));
    CHECK(Compositor::letterboxRect({1000, 1000}, {1440, 1080}) == QRect(0, 125, 1000, 750));
    // DV widescreen: 720x480 with 40:33 pixels.
    CHECK(Compositor::letterboxRect({1000, 1000}, {720, 480}, 40.0 / 33.0) == QRect(0, 225, 1000, 550));
    // A wide frame pillarboxes the seen shape.
    CHECK(Compositor::letterboxRect({2000, 1000}, {1440, 1080}, 4.0 / 3.0) == QRect(111, 0, 1778, 1000));
    // Nothing usable as a ratio (a damaged project): square pixels.
    CHECK(Compositor::letterboxRect({1000, 1000}, {1920, 1080}, 0.0) == QRect(0, 218, 1000, 563));
    CHECK(Compositor::letterboxRect({1000, 1000}, {1920, 1080}, -2.0) == QRect(0, 218, 1000, 563));
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
