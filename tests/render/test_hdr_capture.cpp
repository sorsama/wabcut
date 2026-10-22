// HDR game captures in an SDR project (I.9, gaming): OBS, Game Bar and ShadowPlay HDR recordings are BT.2020 PQ (or HLG)
// files. The layer pass tone-maps them into SDR (BT.2390 EETF from a 1000-nit master, the 203-nit reference white
// standing for SDR white) instead of reading them through the BT.709 curve (washed out, highlights clipped) - checked
// against the CPU reference color::hdrCaptureToSdr and render::referencePixel, 2.5 / 255.

#include "RenderTestSupport.h"

#include <wabcut/color/ColorSpaces.h>
#include <wabcut/render/Compositor.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <string>

using namespace wabcut;
using namespace wabcut::render;
using namespace wabcut::render::test;
using Catch::Approx;

namespace {

constexpr int kW = 256;
constexpr int kH = 64;

/// The code values of the test picture: a grey ramp on top (0 to 10000 nits as PQ), colour patches below.
Rgb pictureAt(int x, int y) {
    const double u = (x + 0.5) / kW;
    if (y < kH / 2) {
        return {u, u, u};
    }
    const int patch = x * 8 / kW;
    static constexpr Rgb kPatches[] = {{0.62, 0.35, 0.28}, {0.30, 0.55, 0.32}, {0.25, 0.32, 0.62}, {0.70, 0.70, 0.40},
                                       {0.75, 0.72, 0.70}, {0.45, 0.45, 0.45}, {0.10, 0.12, 0.15}, {0.66, 0.30, 0.60}};
    return kPatches[patch];
}

/// What the layer shader gets for the picture at (x, y): its 10-bit Y'CbCr quantisation decoded back (and clamped).
Rgb codeAt(int x, int y, const ColorInfo& tags) {
    const auto yuv = encodeYuv(pictureAt(x, y), tags.matrix, tags.range, 10);
    const SampleToRgb s2r = sampleToRgb(tags.matrix, tags.range, SampleEncoding::Bits10Low, false);
    const auto rgb = s2r.apply({yuv[0] / 65535.0, yuv[1] / 65535.0, yuv[2] / 65535.0});
    return {std::clamp(rgb[0], 0.0, 1.0), std::clamp(rgb[1], 0.0, 1.0), std::clamp(rgb[2], 0.0, 1.0)};
}

Rgb bt709(const color::Rgb3& linear) {
    return {linearToOutput(OutputTransform::Rec709, linear[0]), linearToOutput(OutputTransform::Rec709, linear[1]),
            linearToOutput(OutputTransform::Rec709, linear[2])};
}

} // namespace

TEST_CASE("HDR captures: where PQ levels land in SDR (mid-tones pass, the reference white under SDR white, the peak on it)",
          "[render][hdr][gaming]") {
    const auto grey = [](double nits) {
        const double code = color::nitsToPq(nits);
        return bt709(*color::hdrCaptureToSdr(TransferFunction::PQ, {code, code, code}))[1];
    };
    CHECK(grey(20.0) == Approx(linearToOutput(OutputTransform::Rec709, 20.0 / 203.0)).margin(1e-3));
    CHECK(grey(203.0) > 0.85);
    CHECK(grey(203.0) < 0.95);
    CHECK(grey(1000.0) == Approx(1.0).margin(1e-3));
    CHECK(grey(4000.0) == Approx(1.0).margin(1e-3));
    double previous = -1.0;
    for (double nits = 0.1; nits <= 1000.0; nits *= 1.25) {
        const double g = grey(nits);
        CHECK(g >= previous); // monotonic
        previous = g;
    }
    CHECK_FALSE(color::hdrCaptureToSdr(TransferFunction::BT709, {0.5, 0.5, 0.5}).has_value());
}

TEST_CASE("HDR captures in an SDR project are tone-mapped into SDR on the GPU (PQ, HLG)", "[render][gpu][hdr][gaming]") {
    auto renderer = makeRenderer();
    for (const TransferFunction transfer : {TransferFunction::PQ, TransferFunction::HLG}) {
        const char* name = transfer == TransferFunction::PQ ? "pq" : "hlg";
        DYNAMIC_SECTION(name) {
            const ColorInfo tags{ColorMatrix::BT2020NonConstant, ColorRange::Limited, ColorPrimaries::BT2020, transfer,
                                 ChromaLocation::Left};
            auto image = renderer->render(
                compositeOf({kW, kH}, {layerOf(makeFrame(media::PixelFormat::YUV444P10, kW, kH, tags, pictureAt))}));
            REQUIRE(image);
            savePng(*image, QStringLiteral("hdr_capture/%1_sdr.png").arg(QString::fromLatin1(name)));
            double worst = 0.0;
            std::string where;
            for (int y = 4; y < kH; y += 8) {
                for (int x = 2; x < kW; x += 4) {
                    const auto sdr = color::hdrCaptureToSdr(transfer, codeAt(x, y, tags));
                    REQUIRE(sdr);
                    const Rgb want = bt709(*sdr);
                    const Rgb got = pixelOf(*image, x, y);
                    if (maxDiff(got, want) > worst) {
                        worst = maxDiff(got, want);
                        where = std::format("({}, {}): got {} want {}", x, y, describe(got), describe(want));
                    }
                }
            }
            INFO(where);
            CHECK(worst <= 2.5 / 255.0);
            // The CPU reference of the whole default pipeline agrees.
            const ResolvedColor resolved = resolveSourceColor(tags, ColorPipeline{}, false);
            const auto yuv = encodeYuv(pictureAt(200, 10), tags.matrix, tags.range, 10);
            const auto ref = referencePixel({yuv[0] / 65535.0, yuv[1] / 65535.0, yuv[2] / 65535.0}, 1.0, resolved,
                                            SampleEncoding::Bits10Low, false, OutputTransform::Rec709);
            CHECK(maxDiff(Rgb{ref[0], ref[1], ref[2]}, pixelOf(*image, 200, 10)) <= 2.5 / 255.0);
        }
    }
}
