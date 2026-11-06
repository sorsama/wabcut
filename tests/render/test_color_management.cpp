// Colour management on the GPU (I.9, ColorPipeline v2): the compositor's managed input paths (analytic PQ / HLG, camera
// log and ACES through baked LUTs), its output paths (the ACES output transforms, PQ, HLG, the tone-mapped SDR preview
// of HDR), the grade in ACEScct, a nest keeping HDR values, and the HDR scopes - each against the CPU plans of
// wabcut::color (ColorSpaces.h), which tests/color checks against OpenColorIO itself. The default pipeline ignores all
// of it. Tolerances: 8-bit outputs 2.5 / 255, 10-bit outputs 3 / 1023 (the 10-bit Y'CbCr input and the LUT
// interpolation are what is left; the references see the same quantised code values as the GPU).

#include "RenderTestSupport.h"

#include <wabcut/color/ColorMath.h>
#include <wabcut/color/ColorSpaces.h>
#include <wabcut/color/Grade.h>
#include <wabcut/color/Scopes.h>
#include <wabcut/render/Compositor.h>
#include <wabcut/render/GpuScopes.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <rhi/qrhi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

using namespace wabcut;
using namespace wabcut::render;
using namespace wabcut::render::test;

namespace {

constexpr int kW = 256;
constexpr int kH = 64;

/// The code values of the test picture: a grey ramp on top, colour patches below (non-linear R'G'B').
Rgb pictureAt(int x, int y) {
    const double u = (x + 0.5) / kW;
    if (y < kH / 2) {
        return {u, u, u};
    }
    const int patch = x * 8 / kW;
    static constexpr Rgb kPatches[] = {{0.62, 0.35, 0.28}, {0.30, 0.55, 0.32}, {0.25, 0.32, 0.62}, {0.70, 0.70, 0.40},
                                       {0.95, 0.90, 0.85}, {0.45, 0.45, 0.45}, {0.10, 0.12, 0.15}, {0.80, 0.20, 0.60}};
    return kPatches[patch];
}

const ColorInfo kTags{ColorMatrix::BT709, ColorRange::Full, ColorPrimaries::BT709, TransferFunction::BT709, ChromaLocation::Left};

/// What the layer shader gets for the picture at (x, y): its 10-bit Y'CbCr quantisation decoded back (and clamped).
Rgb codeAt(int x, int y, const ColorInfo& tags = kTags) {
    const auto yuv = encodeYuv(pictureAt(x, y), tags.matrix, tags.range, 10);
    const SampleToRgb s2r = sampleToRgb(tags.matrix, tags.range, SampleEncoding::Bits10Low, false);
    const auto rgb = s2r.apply({yuv[0] / 65535.0, yuv[1] / 65535.0, yuv[2] / 65535.0});
    return {std::clamp(rgb[0], 0.0, 1.0), std::clamp(rgb[1], 0.0, 1.0), std::clamp(rgb[2], 0.0, 1.0)};
}

CompositeLayer pictureLayer(std::optional<InputColorSpace> space, bool graphics = false, const ColorInfo& tags = kTags) {
    CompositeLayer layer = layerOf(makeFrame(media::PixelFormat::YUV444P10, kW, kH, tags, pictureAt));
    layer.inputSpace = space;
    layer.graphics = graphics;
    return layer;
}

ColorPipeline managed(ColorManagementMode mode, color::OutputTarget exportTarget = color::OutputTarget::Sdr) {
    ColorPipeline pipeline;
    pipeline.management.mode = mode;
    pipeline.exportTarget = exportTarget;
    return pipeline;
}

std::unique_ptr<OffscreenRenderer> makeRenderer10(OutputRole role = OutputRole::Export) {
    OffscreenOptions options;
    options.role = role;
    options.tenBit = true;
    auto renderer = OffscreenRenderer::create(options);
    if (!renderer) {
        FAIL("OffscreenRenderer::create (10-bit) failed: " << renderer.error().message);
    }
    return std::move(*renderer);
}

/// RGB of a pixel of an RGB10A2 or RGBA8 image, in [0, 1].
Rgb pixelAny(const RenderedImage& image, int x, int y) {
    if (!image.tenBit) {
        return pixelOf(image, x, y);
    }
    std::uint32_t w = 0;
    std::memcpy(&w, image.pixel(x, y), 4);
    return {(w & 1023u) / 1023.0, ((w >> 10) & 1023u) / 1023.0, ((w >> 20) & 1023u) / 1023.0};
}

/// The largest difference between the image and `expected` over a grid of sample points.
double worstOver(const RenderedImage& image, const std::function<Rgb(int, int)>& expected, std::string* where) {
    double worst = 0.0;
    for (int y = 4; y < kH; y += 8) {
        for (int x = 2; x < kW; x += 4) {
            const Rgb got = pixelAny(image, x, y);
            const Rgb want = expected(x, y);
            const double d = maxDiff(got, want);
            if (d > worst) {
                worst = d;
                *where = std::format("({}, {}): got {} want {}", x, y, describe(got), describe(want));
            }
        }
    }
    return worst;
}

color::Rgb3 toRgb3(const Rgb& c) { return {c[0], c[1], c[2]}; }
Rgb fromRgb3(const color::Rgb3& c) { return {c[0], c[1], c[2]}; }

/// The CPU chain the GPU runs: the layer's input plan, then the output plan.
Rgb planned(const ColorManagement& m, InputColorSpace space, bool limited, bool graphics, color::OutputTarget target,
            const Rgb& code) {
    const auto in = color::planInput(m, space, limited, graphics);
    const auto out = color::planOutput(m, target);
    REQUIRE(in);
    REQUIRE(out);
    return fromRgb3(color::runOutputPlan(*out, color::runInputPlan(*in, toRgb3(code))));
}

/// A working value as the layer pass leaves it: light is never negative on the canvas (layer.frag clips colours
/// outside the working gamut at 0; no gamut compression).
color::Rgb3 onCanvas(const color::Rgb3& w) { return {std::max(w[0], 0.0), std::max(w[1], 0.0), std::max(w[2], 0.0)}; }

} // namespace

TEST_CASE("The default pipeline ignores the graphics flag and the export target: today's output byte for byte",
          "[render][gpu][colormanagement]") {
    auto renderer = makeRenderer();
    auto plain = renderer->render(compositeOf({kW, kH}, {pictureLayer(std::nullopt)}));
    auto marked = renderer->render(compositeOf({kW, kH}, {pictureLayer(std::nullopt, true)}));
    REQUIRE(plain);
    REQUIRE(marked);
    CHECK(plain->rgba == marked->rgba);
    // An explicit Rec.709 reading is the same picture too (BT.709, no matrix).
    auto rec709 = renderer->render(compositeOf({kW, kH}, {pictureLayer(InputColorSpace::Rec709)}));
    REQUIRE(rec709);
    std::string where;
    CHECK(worstOver(*rec709, [&](int x, int y) { return pixelAny(*plain, x, y); }, &where) <= 1.0 / 255.0);
    // Disable Color Management ignores input colour spaces altogether: the plain picture, byte for byte.
    ColorPipeline off;
    off.management.mode = ColorManagementMode::Disabled;
    auto ignored = renderer->render(compositeOf({kW, kH}, {pictureLayer(InputColorSpace::SLog3SGamut3, true)}, off));
    REQUIRE(ignored);
    CHECK(ignored->rgba == plain->rgba);
    // The PQ export target of a display-referred pipeline is ignored too.
    ColorPipeline pq;
    pq.exportTarget = color::OutputTarget::Pq;
    auto still = renderer->render(compositeOf({kW, kH}, {pictureLayer(std::nullopt)}, pq));
    REQUIRE(still);
    CHECK(still->rgba == plain->rgba);
}

TEST_CASE("Direct Rec.709 tone-maps camera log on input; Disable Color Management shows HDR as its code values (0.2)",
          "[render][gpu][colormanagement][direct709]") {
    auto renderer = makeRenderer();
    const ColorPipeline direct; // Direct Rec.709 (SDR), the default
    for (const InputColorSpace space : {InputColorSpace::SLog3SGamut3Cine, InputColorSpace::VLogVGamut, InputColorSpace::LogC3,
                                        InputColorSpace::LogC4, InputColorSpace::CanonLog3, InputColorSpace::RedLog3G10}) {
        DYNAMIC_SECTION(toString(space)) {
            auto image = renderer->render(compositeOf({kW, kH}, {pictureLayer(space)}, direct));
            REQUIRE(image);
            savePng(*image, QStringLiteral("colormanagement/direct709_%1.png").arg(QString::fromLatin1(toString(space))));
            std::string where;
            const double worst = worstOver(*image, [&](int x, int y) {
                return planned(direct.management, space, false, false, color::OutputTarget::Sdr, codeAt(x, y));
            }, &where);
            INFO(where);
            CHECK(worst <= 2.5 / 255.0);
        }
    }
    // An HDR (PQ) override on a clip tagged BT.709: tone-mapped as an HDR capture, not read through the BT.709 curve.
    auto pq = renderer->render(compositeOf({kW, kH}, {pictureLayer(InputColorSpace::Rec2100Pq)}, direct));
    REQUIRE(pq);
    std::string where;
    double worst = worstOver(*pq, [&](int x, int y) {
        return planned(direct.management, InputColorSpace::Rec2100Pq, false, false, color::OutputTarget::Sdr, codeAt(x, y));
    }, &where);
    INFO(where);
    CHECK(worst <= 2.5 / 255.0);

    // A PQ-tagged picture: Direct Rec.709 tone-maps it by its tags (the gaming default), Disable Color Management reads
    // the code values through the BT.709 curve as they are (washed out, as Premiere shows HDR without management).
    const ColorInfo pqTags{ColorMatrix::BT709, ColorRange::Full, ColorPrimaries::BT709, TransferFunction::PQ, ChromaLocation::Left};
    auto toneMapped = renderer->render(compositeOf({kW, kH}, {pictureLayer(std::nullopt, false, pqTags)}, direct));
    ColorPipeline off;
    off.management.mode = ColorManagementMode::Disabled;
    auto raw = renderer->render(compositeOf({kW, kH}, {pictureLayer(std::nullopt, false, pqTags)}, off));
    REQUIRE(toneMapped);
    REQUIRE(raw);
    CHECK(toneMapped->rgba != raw->rgba);
    worst = worstOver(*raw, [&](int x, int y) { return codeAt(x, y, pqTags); }, &where);
    INFO("off " << where);
    CHECK(worst <= 2.5 / 255.0);
}

TEST_CASE("Wide Gamut (No Tone Mapping): ACEScg working space, Rec.709 out, SDR media unchanged (0.2)",
          "[render][gpu][colormanagement][widegamut]") {
    auto renderer = makeRenderer();
    const ColorPipeline wide = managed(ColorManagementMode::WideGamut);
    for (const InputColorSpace space : {InputColorSpace::Rec709, InputColorSpace::SLog3SGamut3Cine, InputColorSpace::LogC4}) {
        DYNAMIC_SECTION(toString(space)) {
            auto image = renderer->render(compositeOf({kW, kH}, {pictureLayer(space)}, wide));
            REQUIRE(image);
            const auto in = color::planInput(wide.management, space, false, false);
            const auto out = color::planOutput(wide.management, color::OutputTarget::Sdr);
            REQUIRE(in);
            REQUIRE(out);
            std::string where;
            // The straight conversion to Rec.709 mixes channels: the canvas's clip of negative ACEScg light counts.
            const double worst = worstOver(*image, [&](int x, int y) {
                return fromRgb3(color::runOutputPlan(*out, onCanvas(color::runInputPlan(*in, toRgb3(codeAt(x, y))))));
            }, &where);
            INFO(where);
            CHECK(worst <= 2.5 / 255.0);
        }
    }
    // SDR media passes through the ACEScg working space unchanged (no tone mapping, no inverse output transform).
    auto image = renderer->render(compositeOf({kW, kH}, {pictureLayer(InputColorSpace::Rec709)}, wide));
    REQUIRE(image);
    std::string where;
    const double worst = worstOver(*image, [](int x, int y) { return codeAt(x, y); }, &where);
    INFO(where);
    CHECK(worst <= 2.5 / 255.0);
}

TEST_CASE("ACES 1.3: camera log, HDR and display-referred inputs through the ACES SDR output transform",
          "[render][gpu][colormanagement][aces]") {
    auto renderer = makeRenderer();
    const ColorPipeline pipeline = managed(ColorManagementMode::Aces13);
    for (const InputColorSpace space :
         {InputColorSpace::SLog3SGamut3Cine, InputColorSpace::SLog3SGamut3, InputColorSpace::VLogVGamut, InputColorSpace::LogC3,
          InputColorSpace::LogC4, InputColorSpace::CanonLog3, InputColorSpace::AppleLog, InputColorSpace::Rec2100Pq,
          InputColorSpace::Rec2100Hlg, InputColorSpace::Rec709, InputColorSpace::Srgb}) {
        DYNAMIC_SECTION(toString(space)) {
            auto composite = compositeOf({kW, kH}, {pictureLayer(space)}, pipeline);
            auto image = renderer->render(composite);
            REQUIRE(image);
            savePng(*image, QStringLiteral("colormanagement/aces_%1.png").arg(QString::fromLatin1(toString(space))));
            std::string where;
            const double worst = worstOver(*image, [&](int x, int y) {
                return planned(pipeline.management, space, false, false, color::OutputTarget::Sdr, codeAt(x, y));
            }, &where);
            INFO(where);
            CHECK(worst <= 2.5 / 255.0);
        }
    }
    // A title (graphics) in an ACES project reads through the inverse output transform: it looks as designed.
    auto titled = renderer->render(compositeOf({kW, kH}, {pictureLayer(InputColorSpace::Srgb, true)}, pipeline));
    REQUIRE(titled);
    std::string where;
    const double worst = worstOver(*titled, [](int x, int y) { return codeAt(x, y); }, &where);
    INFO(where);
    CHECK(worst <= 4.0 / 255.0); // the round trip through the SDR output transform, within its LUT's interpolation
}

TEST_CASE("HDR Rec.2100 PQ: PQ, HLG, SDR and log inputs exported as PQ (10-bit), HLG, and the tone-mapped preview",
          "[render][gpu][colormanagement][hdr]") {
    auto renderer10 = makeRenderer10();
    auto display = makeRenderer(OutputRole::Display);
    const ColorPipeline pq = managed(ColorManagementMode::HdrPq, color::OutputTarget::Pq);
    const ColorPipeline hlg = managed(ColorManagementMode::HdrHlg, color::OutputTarget::Hlg);
    for (const InputColorSpace space : {InputColorSpace::Rec2100Pq, InputColorSpace::Rec2100Hlg, InputColorSpace::Rec709,
                                        InputColorSpace::Rec2020, InputColorSpace::SLog3SGamut3Cine, InputColorSpace::LogC4}) {
        DYNAMIC_SECTION(toString(space)) {
            // PQ export: 10 bits.
            auto image = renderer10->render(compositeOf({kW, kH}, {pictureLayer(space)}, pq));
            REQUIRE(image);
            REQUIRE(image->tenBit);
            std::string where;
            double worst = worstOver(*image, [&](int x, int y) {
                return planned(pq.management, space, false, false, color::OutputTarget::Pq, codeAt(x, y));
            }, &where);
            INFO("PQ " << where);
            CHECK(worst <= 3.0 / 1023.0);
            // HLG export.
            image = renderer10->render(compositeOf({kW, kH}, {pictureLayer(space)}, hlg));
            REQUIRE(image);
            worst = worstOver(*image, [&](int x, int y) {
                return planned(hlg.management, space, false, false, color::OutputTarget::Hlg, codeAt(x, y));
            }, &where);
            INFO("HLG " << where);
            CHECK(worst <= 3.0 / 1023.0);
            // The monitor: the tone-mapped SDR preview (8-bit).
            image = display->render(compositeOf({kW, kH}, {pictureLayer(space)}, pq));
            REQUIRE(image);
            savePng(*image, QStringLiteral("colormanagement/hdr_preview_%1.png").arg(QString::fromLatin1(toString(space))));
            worst = worstOver(*image, [&](int x, int y) {
                return planned(pq.management, space, false, false, color::OutputTarget::Sdr, codeAt(x, y));
            }, &where);
            INFO("preview " << where);
            CHECK(worst <= 2.5 / 255.0);
        }
    }
    // The preview keeps HDR highlights apart: the mastering peak (1000 nits) displays near SDR white, the reference
    // white (203 nits) below it with headroom, both brighter than SDR mid grey.
    const auto previewOf = [&](double nits) {
        const auto out = color::planOutput(pq.management, color::OutputTarget::Sdr);
        REQUIRE(out);
        const double v = nits / pq.management.referenceWhiteNits;
        return color::runOutputPlan(*out, {v, v, v})[1];
    };
    CHECK(previewOf(1000.0) >= 0.97);
    CHECK(previewOf(203.0) < 0.95);
    CHECK(previewOf(203.0) > 0.7);
    CHECK(previewOf(203.0) > previewOf(50.0));
}

TEST_CASE("The grade works in ACEScct in a managed project (GPU vs the CPU reference)", "[render][gpu][colormanagement][grade]") {
    auto renderer = makeRenderer();
    const ColorPipeline pipeline = managed(ColorManagementMode::Aces13);
    color::GradeSpec spec;
    spec.params.exposure = 0.6;
    spec.params.contrast = 25;
    spec.params.saturation = 130;
    spec.params.temperature = 12;
    spec.params.shadows = 40; // the tone curve works on encoded values: where the encoding shows
    spec.params.highlights = -40;
    spec.encoding = color::GradeEncoding::AcesCct; // the CPU reference's encoding; the compositor follows the pipeline
    REQUIRE(spec.active());
    CompositeLayer layer = pictureLayer(InputColorSpace::SLog3SGamut3Cine);
    LayerEffect grade;
    grade.type = EffectType::Grade;
    grade.grade = std::make_shared<const color::GradeSpec>(spec);
    layer.effects = {grade};
    auto image = renderer->render(compositeOf({kW, kH}, {layer}, pipeline));
    REQUIRE(image);
    savePng(*image, QStringLiteral("colormanagement/aces_grade.png"));
    const auto in = color::planInput(pipeline.management, InputColorSpace::SLog3SGamut3Cine, false, false);
    const auto out = color::planOutput(pipeline.management, color::OutputTarget::Sdr);
    REQUIRE(in);
    REQUIRE(out);
    std::string where;
    const double worst = worstOver(*image, [&](int x, int y) {
        const color::Rgb3 w = onCanvas(color::runInputPlan(*in, toRgb3(codeAt(x, y))));
        const color::Rgb graded = color::gradePixel(spec, {w[0], w[1], w[2]}, 0.0, 0.0, static_cast<double>(kW) / kH);
        return fromRgb3(color::runOutputPlan(*out, {graded[0], graded[1], graded[2]}));
    }, &where);
    INFO(where);
    CHECK(worst <= 3.0 / 255.0);
    // ...and not in BT.709: the same grade encoded as BT.709 lands elsewhere.
    color::GradeSpec rec709 = spec;
    rec709.encoding = color::GradeEncoding::Rec709;
    double far = 0.0;
    for (int y = 4; y < kH; y += 24) {
        for (int x = 2; x < kW; x += 16) {
            const color::Rgb3 w = onCanvas(color::runInputPlan(*in, toRgb3(codeAt(x, y))));
            const color::Rgb graded = color::gradePixel(rec709, {w[0], w[1], w[2]}, 0.0, 0.0, static_cast<double>(kW) / kH);
            far = std::max(far, maxDiff(pixelAny(*image, x, y), fromRgb3(color::runOutputPlan(*out, {graded[0], graded[1], graded[2]}))));
        }
    }
    CHECK(far > 6.0 / 255.0);
}

TEST_CASE("HDR values above the reference white survive a nest (float through the nest, PQ out)", "[render][gpu][colormanagement][nested]") {
    auto renderer10 = makeRenderer10();
    const ColorPipeline pq = managed(ColorManagementMode::HdrPq, color::OutputTarget::Pq);
    auto flat = renderer10->render(compositeOf({kW, kH}, {pictureLayer(InputColorSpace::Rec2100Pq)}, pq));
    REQUIRE(flat);
    auto inner = std::make_shared<PreparedComposite>();
    inner->canvas = {kW, kH};
    inner->layers = {pictureLayer(InputColorSpace::Rec2100Pq)};
    CompositeLayer nest;
    nest.nested = inner;
    auto nested = renderer10->render(compositeOf({kW, kH}, {nest}, pq));
    REQUIRE(nested);
    std::string where;
    const double worst = worstOver(*nested, [&](int x, int y) { return pixelAny(*flat, x, y); }, &where);
    INFO(where);
    CHECK(worst <= 2.0 / 1023.0);
    // The ramp's top end is far above the reference white (PQ code 0.58): it is not clipped there.
    CHECK(pixelAny(*nested, kW - 3, 4)[1] > 0.9);
}

TEST_CASE("HDR scopes read PQ code values (the nits scale) and the HLG signal (the % scale)", "[render][gpu][colormanagement][scopes]") {
    auto renderer = makeRenderer(OutputRole::Display);
    REQUIRE(renderer->rhi()->isFeatureSupported(QRhi::Compute));
    const auto flatLayer = [](double code, InputColorSpace space) {
        CompositeLayer layer = layerOf(makeFrame(media::PixelFormat::YUV444P10, kW, kH, kTags,
                                                 [code](int, int) { return Rgb{code, code, code}; }));
        layer.inputSpace = space;
        return layer;
    };
    const auto lumaLevel = [&](const ColorPipeline& pipeline, const CompositeLayer& layer, bool hlg) {
        auto image = renderer->render(compositeOf({kW, kH}, {layer}, pipeline));
        REQUIRE(image);
        GpuScopes scopes(renderer->rhi());
        QRhiCommandBuffer* cb = nullptr;
        REQUIRE(renderer->rhi()->beginOffscreenFrame(&cb) == QRhi::FrameOpSuccess);
        color::ScopeData data;
        bool done = false;
        REQUIRE(scopes.record(cb, renderer->compositor().canvasTexture(), QSize(kW, kH), OutputTransform::Rec709, {},
                              [&](color::ScopeData d) {
                                  data = std::move(d);
                                  done = true;
                              },
                              ScopeHdr{hlg, pipeline.management.referenceWhiteNits, pipeline.management.peakNits}));
        REQUIRE(renderer->rhi()->endOffscreenFrame() == QRhi::FrameOpSuccess);
        if (!done) {
            renderer->rhi()->finish();
        }
        REQUIRE(done);
        CHECK(data.scale == (hlg ? 2 : 1));
        // The level holding most of the luma histogram.
        const auto& h = data.histogram[3];
        return static_cast<double>(std::max_element(h.begin(), h.end()) - h.begin()) / static_cast<double>(h.size());
    };
    const ColorPipeline pq = managed(ColorManagementMode::HdrPq);
    // A PQ clip at 1000 nits reads 0.752 on the nits scale; one at 203 nits (the reference white) 0.581.
    CHECK(std::abs(lumaLevel(pq, flatLayer(color::nitsToPq(1000.0), InputColorSpace::Rec2100Pq), false) - 0.7518) < 0.01);
    CHECK(std::abs(lumaLevel(pq, flatLayer(color::nitsToPq(203.0), InputColorSpace::Rec2100Pq), false) - 0.5807) < 0.01);
    // SDR white (BT.709 code 1.0) sits at the reference white.
    CHECK(std::abs(lumaLevel(pq, flatLayer(1.0, InputColorSpace::Rec709), false) - 0.5807) < 0.01);
    // HLG: SDR white at 75 %.
    const ColorPipeline hlgPipeline = managed(ColorManagementMode::HdrHlg);
    CHECK(std::abs(lumaLevel(hlgPipeline, flatLayer(1.0, InputColorSpace::Rec709), true) - 0.75) < 0.015);
}

// ---- 0.6 (#72): HDR finishing - Minimal Tone Mapping, gamut compression, output colour spaces, SDR Conform ----------

namespace {

/// Saturated synthetic colours: a grey ramp on top, the Rec.2020 primaries and secondaries (and two near-greys) below.
Rgb primariesAt(int x, int y) {
    const double u = (x + 0.5) / kW;
    if (y < kH / 2) {
        return {u, u, u};
    }
    static constexpr Rgb kPatches[] = {{0.75, 0.0, 0.0}, {0.0, 0.75, 0.0}, {0.0, 0.0, 0.75}, {0.62, 0.62, 0.0},
                                       {0.0, 0.62, 0.62}, {0.62, 0.0, 0.62}, {0.9, 0.85, 0.8}, {0.4, 0.42, 0.45}};
    return kPatches[x * 8 / kW];
}

const ColorInfo kTags2020{ColorMatrix::BT2020NonConstant, ColorRange::Full, ColorPrimaries::BT2020, TransferFunction::BT709,
                          ChromaLocation::Left};

Rgb primariesCodeAt(int x, int y) {
    const auto yuv = encodeYuv(primariesAt(x, y), kTags2020.matrix, kTags2020.range, 10);
    const SampleToRgb s2r = sampleToRgb(kTags2020.matrix, kTags2020.range, SampleEncoding::Bits10Low, false);
    const auto rgb = s2r.apply({yuv[0] / 65535.0, yuv[1] / 65535.0, yuv[2] / 65535.0});
    return {std::clamp(rgb[0], 0.0, 1.0), std::clamp(rgb[1], 0.0, 1.0), std::clamp(rgb[2], 0.0, 1.0)};
}

CompositeLayer primariesLayer(InputColorSpace space) {
    CompositeLayer layer = layerOf(makeFrame(media::PixelFormat::YUV444P10, kW, kH, kTags2020, primariesAt));
    layer.inputSpace = space;
    return layer;
}

/// The working value the layer pass leaves on the canvas: below 0 clipped unless the setup compresses gamut (0.6).
color::Rgb3 canvasOf(const ColorManagement& m, const color::Rgb3& w) { return m.compressesGamut() ? w : onCanvas(w); }

color::Rgb3 workingValueOf(const ColorManagement& m, InputColorSpace space, const Rgb& code) {
    const auto in = color::planInput(m, space, false, false);
    REQUIRE(in);
    return canvasOf(m, color::runInputPlan(*in, toRgb3(code)));
}

} // namespace

TEST_CASE("Wide Gamut Minimal Tone Mapping on the GPU: SDR media unchanged and scene-referred highlights rolled off (0.6)",
          "[render][gpu][colormanagement][widegamut]") {
    auto renderer = makeRenderer();
    const ColorPipeline minimal = managed(ColorManagementMode::WideGamutMinimal);
    REQUIRE(minimal.management.compressesGamut());
    for (const InputColorSpace space : {InputColorSpace::Rec709, InputColorSpace::SLog3SGamut3Cine, InputColorSpace::LogC4,
                                        InputColorSpace::Rec2100Pq, InputColorSpace::Rec2100Hlg}) {
        DYNAMIC_SECTION(toString(space)) {
            auto image = renderer->render(compositeOf({kW, kH}, {pictureLayer(space)}, minimal));
            REQUIRE(image);
            savePng(*image, QStringLiteral("colormanagement/minimal_%1.png").arg(QString::fromLatin1(toString(space))));
            const auto out = color::planOutput(minimal.management, color::OutputTarget::Sdr);
            REQUIRE(out);
            std::string where;
            const double worst = worstOver(*image, [&](int x, int y) {
                return fromRgb3(color::runOutputPlan(*out, workingValueOf(minimal.management, space, codeAt(x, y))));
            }, &where);
            INFO(where);
            CHECK(worst <= 2.5 / 255.0);
        }
    }
    // SDR media and graphics come out as they went in (no roll-off, no gamut compression on display-referred pictures).
    for (const bool graphics : {false, true}) {
        auto image = renderer->render(compositeOf({kW, kH}, {pictureLayer(InputColorSpace::Rec709, graphics)}, minimal));
        REQUIRE(image);
        std::string where;
        const double worst = worstOver(*image, [](int x, int y) { return codeAt(x, y); }, &where);
        INFO(where);
        CHECK(worst <= 2.5 / 255.0);
    }
}

TEST_CASE("Gamut compression on the GPU matches the CPU plans and keeps working values below 0 (0.6)",
          "[render][gpu][colormanagement][gamut]") {
    auto renderer = makeRenderer();
    auto renderer10 = makeRenderer10();
    for (const ColorManagementMode mode : {ColorManagementMode::WideGamut, ColorManagementMode::Aces13, ColorManagementMode::HdrPq}) {
        ColorPipeline pipeline = managed(mode, mode == ColorManagementMode::HdrPq ? color::OutputTarget::Pq : color::OutputTarget::Sdr);
        pipeline.management.gamutCompression = true;
        pipeline.management.gamutCompressionStrength = 0.8;
        for (const InputColorSpace space : {InputColorSpace::SLog3SGamut3, InputColorSpace::LogC4, InputColorSpace::Rec709}) {
            DYNAMIC_SECTION(toString(mode) << " " << toString(space)) {
                auto& r = mode == ColorManagementMode::HdrPq ? renderer10 : renderer;
                auto image = r->render(compositeOf({kW, kH}, {pictureLayer(space)}, pipeline));
                REQUIRE(image);
                const auto out = color::planOutput(pipeline.management, outputTargetFor(pipeline, OutputRole::Export));
                REQUIRE(out);
                std::string where;
                const double worst = worstOver(*image, [&](int x, int y) {
                    return fromRgb3(color::runOutputPlan(*out, workingValueOf(pipeline.management, space, codeAt(x, y))));
                }, &where);
                INFO(where);
                CHECK(worst <= (image->tenBit ? 3.0 / 1023.0 : 2.5 / 255.0));
            }
        }
    }
    // A saturated S-Gamut3 blue lies outside ACEScg: compressed, it comes closer to the gamut and its negative channel is
    // kept on the canvas (the Rec.709 output then differs from a clipped canvas).
    ColorPipeline wide = managed(ColorManagementMode::WideGamut);
    const auto in = color::planInput(wide.management, InputColorSpace::SLog3SGamut3, false, false);
    REQUIRE(in);
    const color::Rgb3 plain = color::runInputPlan(*in, {0.25, 0.25, 0.95});
    wide.management.gamutCompression = true;
    const auto compressedPlan = color::planInput(wide.management, InputColorSpace::SLog3SGamut3, false, false);
    REQUIRE(compressedPlan);
    const color::Rgb3 compressed = color::runInputPlan(*compressedPlan, {0.25, 0.25, 0.95});
    REQUIRE(std::min({plain[0], plain[1], plain[2]}) < 0.0);
    CHECK(std::min({compressed[0], compressed[1], compressed[2]}) > std::min({plain[0], plain[1], plain[2]}));
    CHECK(std::max({compressed[0], compressed[1], compressed[2]}) == Catch::Approx(std::max({plain[0], plain[1], plain[2]})));
}

TEST_CASE("Output colour spaces Rec.2020 SDR and P3-D65 on the GPU match the CPU plans (0.6)",
          "[render][gpu][colormanagement][output]") {
    auto renderer = makeRenderer();
    for (const ColorManagementMode mode : {ColorManagementMode::DisplayRec709, ColorManagementMode::WideGamut,
                                           ColorManagementMode::HdrPq, ColorManagementMode::Aces13}) {
        for (const color::OutputTarget target : {color::OutputTarget::Rec2020Sdr, color::OutputTarget::P3D65}) {
            DYNAMIC_SECTION(toString(mode) << " " << (target == color::OutputTarget::P3D65 ? "p3" : "2020")) {
                const ColorPipeline pipeline = managed(mode, target);
                const InputColorSpace space = mode == ColorManagementMode::HdrPq ? InputColorSpace::Rec2100Pq : InputColorSpace::Rec709;
                auto image = renderer->render(compositeOf({kW, kH}, {pictureLayer(space)}, pipeline));
                REQUIRE(image);
                savePng(*image, QStringLiteral("colormanagement/output_%1_%2.png")
                                    .arg(QString::fromLatin1(toString(mode)))
                                    .arg(target == color::OutputTarget::P3D65 ? QStringLiteral("p3d65") : QStringLiteral("rec2020")));
                const auto out = color::planOutput(pipeline.management, target);
                REQUIRE(out);
                std::string where;
                const double worst = worstOver(*image, [&](int x, int y) {
                    return fromRgb3(color::runOutputPlan(*out, workingValueOf(pipeline.management, space, codeAt(x, y))));
                }, &where);
                INFO(where);
                CHECK(worst <= 2.5 / 255.0);
            }
        }
    }
    // Rec.709 white stays white in every SDR space, and a Rec.709 red is less saturated in Rec.2020 code values.
    const ColorManagement direct;
    const auto r2020 = color::planOutput(direct, color::OutputTarget::Rec2020Sdr);
    REQUIRE(r2020);
    const color::Rgb3 white = color::runOutputPlan(*r2020, {1.0, 1.0, 1.0});
    CHECK(white[0] == Catch::Approx(1.0).margin(1e-6));
    CHECK(white[2] == Catch::Approx(1.0).margin(1e-6));
    const color::Rgb3 red = color::runOutputPlan(*r2020, {1.0, 0.0, 0.0});
    CHECK(red[1] > 0.1);
    CHECK(red[0] < 1.0);
}

TEST_CASE("SDR Conform on the GPU matches the CPU reference and flags what it mapped (0.6)",
          "[render][gpu][colormanagement][conform]") {
    auto renderer = makeRenderer();
    struct Setting {
        ColorManagementMode mode;
        color::OutputTarget target;
        ToneMapOperator op;
        GamutMapping gamut;
        bool legal;
        double peak;
        double paper;
    };
    const Setting settings[] = {
        {ColorManagementMode::HdrPq, color::OutputTarget::Sdr, ToneMapOperator::Bt2390, GamutMapping::AcesRgc, false, 100.0, 100.0},
        {ColorManagementMode::HdrPq, color::OutputTarget::Sdr, ToneMapOperator::Aces, GamutMapping::Saturation, false, 203.0, 120.0},
        {ColorManagementMode::HdrHlg, color::OutputTarget::P3D65, ToneMapOperator::Reinhard, GamutMapping::Clip, true, 100.0, 100.0},
        {ColorManagementMode::WideGamutMinimal, color::OutputTarget::Rec2020Sdr, ToneMapOperator::Bt2390, GamutMapping::AcesRgc,
         true, 100.0, 80.0},
    };
    int index = 0;
    for (const Setting& s : settings) {
        DYNAMIC_SECTION("setting " << index++) {
            ColorPipeline pipeline = managed(s.mode, s.target);
            SdrConform& c = pipeline.management.sdrConform;
            c.enabled = true;
            c.toneMap = s.op;
            c.gamut = s.gamut;
            c.legalRange = s.legal;
            c.targetPeakNits = s.peak;
            c.paperWhiteNits = s.paper;
            pipeline.conformStats = true;
            REQUIRE(pipeline.management.conformsSdr());
            auto image = renderer->render(compositeOf({kW, kH}, {primariesLayer(InputColorSpace::Rec2100Pq)}, pipeline));
            REQUIRE(image);
            savePng(*image, QStringLiteral("colormanagement/conform_%1.png").arg(index));
            const auto plan = color::planOutput(pipeline.management, s.target, true);
            REQUIRE(plan);
            REQUIRE(plan->kind == color::OutputPlan::Kind::Conform);
            double worst = 0.0;
            std::string where;
            int samples = 0;
            int flagsMatch = 0;
            bool anyToneMapped = false;
            bool anyGamutMapped = false;
            for (int y = 4; y < kH; y += 8) {
                for (int x = 2; x < kW; x += 4) {
                    const color::ConformPixel want =
                        color::sdrConformPixel(*plan, workingValueOf(pipeline.management, InputColorSpace::Rec2100Pq, primariesCodeAt(x, y)));
                    const Rgb got = pixelOf(*image, x, y);
                    const double d = maxDiff(got, fromRgb3(want.code));
                    if (d > worst) {
                        worst = d;
                        where = std::format("({}, {}): got {} want {}", x, y, describe(got), describe(fromRgb3(want.code)));
                    }
                    const int flags = (want.toneMapped ? 1 : 0) | (want.gamutMapped ? 2 : 0);
                    flagsMatch += conformFlagsOf(alphaOf(*image, x, y)) == flags ? 1 : 0;
                    anyToneMapped = anyToneMapped || want.toneMapped;
                    anyGamutMapped = anyGamutMapped || want.gamutMapped;
                    ++samples;
                }
            }
            INFO(where);
            CHECK(worst <= 3.0 / 255.0);
            CHECK(flagsMatch >= samples * 98 / 100); // floats vs doubles may flip a pixel sitting on a threshold
            if (s.mode != ColorManagementMode::WideGamutMinimal) {
                CHECK(anyToneMapped); // PQ code values run far above the reference white (Minimal rolled them off on input)
            }
            if (s.target != color::OutputTarget::Rec2020Sdr) {
                CHECK(anyGamutMapped); // the Rec.2020 primaries lie outside Rec.709 and P3
            }
        }
    }
    // Without conformStats the export's alpha stays opaque; SDR Conform off leaves the setup's own SDR output.
    ColorPipeline plain = managed(ColorManagementMode::HdrPq);
    plain.management.sdrConform.enabled = true;
    auto image = renderer->render(compositeOf({kW, kH}, {primariesLayer(InputColorSpace::Rec2100Pq)}, plain));
    REQUIRE(image);
    CHECK(alphaOf(*image, 10, 10) == 1.0);
}

TEST_CASE("The monitor previews SDR Conform before and after on a split (0.6)", "[render][gpu][colormanagement][conform]") {
    auto display = makeRenderer(OutputRole::Display);
    ColorPipeline pipeline = managed(ColorManagementMode::HdrPq);
    pipeline.management.sdrConform.toneMap = ToneMapOperator::Reinhard; // off: the preview shows it all the same
    auto composite = compositeOf({kW, kH}, {primariesLayer(InputColorSpace::Rec2100Pq)}, pipeline);
    composite->colorView.conformSplit = 0.5;
    auto image = display->render(composite);
    REQUIRE(image);
    savePng(*image, QStringLiteral("colormanagement/conform_preview_split.png"));
    const auto before = color::planOutput(pipeline.management, color::OutputTarget::Sdr);
    const auto after = color::planOutput(pipeline.management, color::OutputTarget::Sdr, true);
    REQUIRE(before);
    REQUIRE(after);
    double worst = 0.0;
    std::string where;
    for (int y = 4; y < kH; y += 8) {
        for (int x = 2; x < kW; x += 4) {
            if (std::abs(x - kW / 2) < 3) {
                continue;
            }
            const color::Rgb3 w = workingValueOf(pipeline.management, InputColorSpace::Rec2100Pq, primariesCodeAt(x, y));
            const Rgb want = fromRgb3(x < kW / 2 ? color::runOutputPlan(*before, w) : color::sdrConformPixel(*after, w).code);
            const Rgb got = pixelOf(*image, x, y);
            if (maxDiff(got, want) > worst) {
                worst = maxDiff(got, want);
                where = std::format("({}, {}): got {} want {}", x, y, describe(got), describe(want));
            }
        }
    }
    INFO(where);
    CHECK(worst <= 3.0 / 255.0);
    // The whole picture conformed (split 0), and no preview without the view.
    composite->colorView.conformSplit = 0.0;
    auto whole = display->render(composite);
    REQUIRE(whole);
    const color::Rgb3 w = workingValueOf(pipeline.management, InputColorSpace::Rec2100Pq, primariesCodeAt(10, 50));
    CHECK(maxDiff(pixelOf(*whole, 10, 50), fromRgb3(color::sdrConformPixel(*after, w).code)) <= 3.0 / 255.0);
}
