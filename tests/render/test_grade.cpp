// The colour grade on the GPU (shaders/grade.frag) against its CPU reference (wabcut::color, Grade.cpp), stage by
// stage and all together: PSNR >= 45 dB (>= 50 dB for the pure LUT paths), tetrahedral vs trilinear LUTs, the HSL
// secondary with its matte refine (blurs through the compositor's own blur, mirrored by EffectsReference), the mask
// views, the before/after split and the reference side by side; plus the grade's GPU cost at 1080p.
// Rendered pairs land in test-output/render/grade/.

#include "EffectsReference.h"
#include "RenderTestSupport.h"

#include <wabcut/color/Grade.h>
#include <wabcut/color/Looks.h>
#include <wabcut/render/Compositor.h>
#include <wabcut/render/StillImage.h>

#include <QDir>
#include <QImage>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>

using namespace wabcut;
using namespace wabcut::render;
using namespace wabcut::render::test;

namespace {

using color::operator*;
using color::operator+;

constexpr int kW = 320;
constexpr int kH = 180;

std::uint8_t byte(double v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); }

/// A picture with every hue at several saturations and lightnesses, greys and skin.
std::array<std::uint8_t, 4> palette(int x, int y) {
    const double h = static_cast<double>(x) / kW;
    const double band = static_cast<double>(y) / kH;
    if (band > 0.85) { // grey ramp
        return {byte(h), byte(h), byte(h), 255};
    }
    if (band > 0.75) { // skin tones
        return {byte(0.55 + 0.4 * h), byte(0.38 + 0.3 * h), byte(0.28 + 0.25 * h), 255};
    }
    const double s = 0.15 + 0.85 * std::fmod(band * 3.0, 1.0);
    const double v = 0.25 + 0.75 * (1.0 - band);
    const color::Rgb c = color::hueChroma(h); // chroma of the hue at value 1
    const double y0 = 0.5;
    return {byte((y0 + c[0] * s) * v), byte((y0 + c[1] * s) * v), byte((y0 + c[2] * s) * v), 255};
}

QString gradeDir() {
    const QString dir = outputDir() + QStringLiteral("/grade");
    QDir().mkpath(dir);
    return dir;
}

/// Linear straight colour of a premultiplied reference pixel, and back.
color::Rgb straightOf(const Rgba& p) { return p[3] > 0 ? color::Rgb{p[0] / p[3], p[1] / p[3], p[2] / p[3]} : color::Rgb{0, 0, 0}; }
Rgba premultiplied(const color::Rgb& c, double a) { return {c[0] * a, c[1] * a, c[2] * a, a}; }

/// The grade on a full-canvas layer with an identity transform, as the compositor runs it (CPU reference).
LinearImage referenceGrade(const LinearImage& in, const color::GradeSpec& g, int maskView = 0) {
    const color::GradeParams& p = g.params;
    const bool maskOn = maskView != 0 && p.hsl;
    const bool secondary = g.hslActive() || maskOn;
    const bool refine = secondary && (p.denoise > 0 || p.blur > 0 || p.hslSharpen > 0);
    const double aspect = static_cast<double>(in.width) / in.height;
    const auto qx = [&](int x) { return (x + 0.5) / in.width * 2.0 - 1.0; };
    const auto qy = [&](int y) { return (y + 0.5) / in.height * 2.0 - 1.0; };
    const auto finish = [&](const color::Rgb& display, double matte, const color::Rgb& blurred, int x, int y, double a) {
        color::Rgb e = display;
        if (secondary) {
            const color::Rgb graded = color::applySecondary(p, display, matte, blurred);
            if (maskOn) {
                e = maskView == 1 ? color::Rgb{matte, matte, matte}
                    : maskView == 2 ? display * matte
                                    : display * matte + color::Rgb{1 - matte, 1 - matte, 1 - matte};
            } else {
                e = graded;
            }
        }
        color::Rgb lin = color::decode709(e);
        if (g.vignetteActive()) {
            lin = color::applyVignette(p, lin, qx(x), qy(y), aspect);
        }
        return premultiplied(lin, a);
    };
    LinearImage out(in.width, in.height);
    if (!refine) {
        for (int y = 0; y < in.height; ++y) {
            for (int x = 0; x < in.width; ++x) {
                const Rgba& px = in.at(x, y);
                if (px[3] <= 0) {
                    continue;
                }
                const color::Rgb e = color::gradeMain(g, straightOf(px));
                const double m = secondary ? color::qualifierMatte(p, e) : 0.0;
                out.at(x, y) = finish(e, m, e, x, y, px[3]);
            }
        }
        return out;
    }
    // MAIN -> MATTE -> [blur, threshold] -> [blur] -> APPLY, the blurs being the compositor's Gaussian.
    LinearImage main(in.width, in.height);
    LinearImage matte(in.width, in.height);
    for (int y = 0; y < in.height; ++y) {
        for (int x = 0; x < in.width; ++x) {
            const Rgba& px = in.at(x, y);
            if (px[3] <= 0) {
                matte.at(x, y) = {0, 0, 0, 1};
                continue;
            }
            const color::Rgb e = color::gradeMain(g, straightOf(px));
            main.at(x, y) = premultiplied(color::decode709(e), px[3]);
            const double m = color::qualifierMatte(p, e);
            matte.at(x, y) = {m, m, m, 1};
        }
    }
    const auto blur = [](const LinearImage& image, double sigma) {
        LayerEffect e;
        e.type = EffectType::GaussianBlur;
        e.params = {sigma * 3.0, 1.0};
        return referenceEffect(image, e);
    };
    if (p.denoise > 0) {
        matte = blur(matte, color::denoiseSigma(p));
        for (Rgba& m : matte.px) {
            const double t = color::denoiseThreshold(m[0]);
            m = {t, t, t, 1};
        }
    }
    if (p.blur > 0) {
        matte = blur(matte, color::refineBlurSigma(p));
    }
    const LinearImage sharp = p.hslSharpen > 0 ? blur(main, color::kSecondarySharpenSigma) : main;
    for (int y = 0; y < in.height; ++y) {
        for (int x = 0; x < in.width; ++x) {
            const Rgba& px = main.at(x, y);
            if (px[3] <= 0) {
                continue;
            }
            const color::Rgb e = color::encode709(straightOf(px));
            const color::Rgb b = color::encode709(straightOf(sharp.at(x, y)));
            out.at(x, y) = finish(e, matte.at(x, y)[0], b, x, y, px[3]);
        }
    }
    return out;
}

LayerEffect gradeEffect(const color::GradeSpec& spec) {
    LayerEffect e;
    e.type = EffectType::Grade;
    e.grade = std::make_shared<const color::GradeSpec>(spec);
    return e;
}

double compare(OffscreenRenderer& renderer, const std::shared_ptr<PreparedComposite>& composite, const LinearImage& expected,
               const std::string& name) {
    auto image = renderer.render(composite);
    REQUIRE(image);
    const std::vector<std::uint8_t> reference = encodeOverBlack(expected, OutputTransform::Rec709);
    savePng(*image, QStringLiteral("grade/%1.png").arg(QString::fromStdString(name)));
    QImage(reference.data(), expected.width, expected.height, expected.width * 3, QImage::Format_RGB888)
        .save(gradeDir() + QStringLiteral("/%1.reference.png").arg(QString::fromStdString(name)));
    return psnrRgb(*image, reference);
}

/// A 1D LUT (per-channel curves) and a 3D LUT (a strong, non-linear look).
std::shared_ptr<const color::Lut> test1d() {
    color::Lut lut;
    lut.size1d = 64;
    for (int i = 0; i < 64; ++i) {
        const double x = i / 63.0;
        lut.table1d.push_back(static_cast<float>(std::pow(x, 0.8)));
        lut.table1d.push_back(static_cast<float>(x * x * (3 - 2 * x)));
        lut.table1d.push_back(static_cast<float>(0.1 + 0.8 * x));
    }
    return std::make_shared<const color::Lut>(std::move(lut));
}

std::shared_ptr<const color::Lut> test3d(int size = 17) {
    color::Lut lut;
    lut.size3d = size;
    for (int b = 0; b < size; ++b) {
        for (int g = 0; g < size; ++g) {
            for (int r = 0; r < size; ++r) {
                const double R = r / (size - 1.0), G = g / (size - 1.0), B = b / (size - 1.0);
                lut.table3d.push_back(static_cast<float>(std::pow(R, 0.7) * 0.9 + 0.1 * G));
                lut.table3d.push_back(static_cast<float>(G * G * 0.8 + 0.2 * B));
                lut.table3d.push_back(static_cast<float>(std::sin(B * 1.4) * 0.7 + 0.3 * R * G));
            }
        }
    }
    return std::make_shared<const color::Lut>(std::move(lut));
}

} // namespace

TEST_CASE("Grade: every stage on the GPU matches the CPU reference", "[render][gpu][grade]") {
    auto renderer = makeRenderer(OutputRole::Export);
    const LinearImage input = linearFromRgba8(kW, kH, palette);
    struct Case {
        std::string name;
        std::function<void(color::GradeSpec&)> setup;
        double minPsnr;
    };
    color::GradeCurves rgbCurves;
    rgbCurves.master = {{0, 0.02}, {0.3, 0.25}, {0.7, 0.8}, {1, 0.97}};
    rgbCurves.red = {{0, 0}, {0.5, 0.56}, {1, 1}};
    rgbCurves.blue = {{0, 0.05}, {0.5, 0.45}, {1, 0.95}};
    color::GradeCurves hueCurves;
    hueCurves.hueSat = {{0.0, 0.5}, {0.33, 0.85}, {0.6, 0.2}, {0.85, 0.5}};
    hueCurves.hueHue = {{0.05, 0.5}, {0.4, 0.58}, {0.75, 0.45}};
    hueCurves.hueLuma = {{0.1, 0.5}, {0.6, 0.7}};
    hueCurves.lumaSat = {{0, 0.3}, {0.5, 0.5}, {1, 0.7}};
    hueCurves.satSat = {{0, 0.5}, {0.5, 0.7}, {1, 0.5}};
    const auto bakedRgb = color::bakeCurves(rgbCurves);
    const auto bakedHue = color::bakeCurves(hueCurves);
    const std::vector<Case> cases = {
        {"input_lut_1d", [&](color::GradeSpec& g) { g.inputLut = test1d(); }, 50.0},
        {"input_lut_3d", [&](color::GradeSpec& g) { g.inputLut = test3d(); }, 50.0},
        {"input_lut_slog3", [&](color::GradeSpec& g) { g.inputLut = color::builtinLut("builtin:slog3-sgamut3cine-to-rec709"); }, 50.0},
        {"look_trilinear", [&](color::GradeSpec& g) {
             g.look = test3d(9);
             g.interpolation = color::LutInterpolation::Trilinear;
         }, 50.0},
        {"white_balance", [](color::GradeSpec& g) { g.params.temperature = 45, g.params.tint = -30; }, 45.0},
        {"exposure_contrast", [](color::GradeSpec& g) { g.params.exposure = 0.7, g.params.contrast = 35; }, 45.0},
        {"contrast_flat", [](color::GradeSpec& g) { g.params.contrast = -60; }, 45.0},
        {"tone", [](color::GradeSpec& g) {
             g.params.highlights = -60, g.params.shadows = 50, g.params.whites = 40, g.params.blacks = -30;
         }, 45.0},
        {"saturation_vibrance", [](color::GradeSpec& g) { g.params.saturation = 140, g.params.vibrance = 60; }, 45.0},
        {"desaturate", [](color::GradeSpec& g) { g.params.saturation = 20, g.params.vibrance = -40; }, 45.0},
        {"look_intensity", [](color::GradeSpec& g) {
             g.look = color::builtinLut("builtin:teal-orange");
             g.params.lookIntensity = 70;
         }, 50.0},
        {"faded_tints", [](color::GradeSpec& g) {
             g.params.fadedFilm = 60, g.params.shadowTintHue = 200, g.params.shadowTintAmount = 70;
             g.params.highlightTintHue = 35, g.params.highlightTintAmount = 50, g.params.tintBalance = 20;
         }, 45.0},
        {"creative_saturation", [](color::GradeSpec& g) { g.params.creativeSaturation = 60, g.params.creativeVibrance = 40; }, 45.0},
        {"curves_rgb", [&](color::GradeSpec& g) { g.curves = bakedRgb; }, 45.0},
        {"curves_hue", [&](color::GradeSpec& g) { g.curves = bakedHue; }, 45.0},
        {"wheels_cdl", [](color::GradeSpec& g) {
             g.params.lift = {0.03, -0.01, 0.05}, g.params.gamma = {0.9, 1.05, 1.2}, g.params.gain = {1.1, 1.0, 0.92};
             g.params.offset = {0.0, 0.01, -0.02}, g.params.cdlSaturation = 1.3;
         }, 45.0},
        {"hsl_secondary", [](color::GradeSpec& g) {
             g.params.hueCenter = 120, g.params.hueWidth = 70, g.params.hueSoftness = 25, g.params.satLow = 20, g.params.satSoftness = 10;
             g.params.hslSaturation = 30, g.params.hslTemperature = -40, g.params.hslContrast = 25, g.params.hslOffset = {0.02, 0, 0.04};
         }, 45.0},
        {"hsl_inverted_luma", [](color::GradeSpec& g) {
             g.params.lumaLow = 30, g.params.lumaHigh = 70, g.params.lumaSoftness = 15, g.params.invert = true, g.params.hslTint = 40;
         }, 45.0},
        {"hsl_refine", [](color::GradeSpec& g) {
             g.params.hueCenter = 30, g.params.hueWidth = 50, g.params.hueSoftness = 15, g.params.denoise = 20, g.params.blur = 6;
             g.params.hslSharpen = 60, g.params.hslSaturation = 150;
         }, 45.0},
        {"vignette", [](color::GradeSpec& g) {
             g.params.vignetteAmount = -2.0, g.params.vignetteMidpoint = 40, g.params.vignetteRoundness = 30, g.params.vignetteFeather = 40;
         }, 45.0},
        {"full_grade", [&](color::GradeSpec& g) {
             g.inputLut = test1d();
             g.look = color::builtinLut("builtin:warm-film");
             g.curves = bakedHue;
             g.params.temperature = -20, g.params.exposure = 0.3, g.params.contrast = 15, g.params.highlights = -30;
             g.params.saturation = 110, g.params.lookIntensity = 50, g.params.fadedFilm = 20;
             g.params.lift = {0.01, 0, 0.02}, g.params.gain = {1.05, 1, 0.98};
             g.params.hueCenter = 30, g.params.hueWidth = 60, g.params.hslSaturation = 120;
             g.params.vignetteAmount = -1;
         }, 45.0},
    };
    std::ofstream summary((gradeDir() + QStringLiteral("/psnr.md")).toStdString(), std::ios::trunc);
    summary << "# Grade stages: GPU vs CPU reference (PSNR, dB)\n\n| stage | PSNR | min |\n|---|---|---|\n";
    for (const Case& c : cases) {
        DYNAMIC_SECTION(c.name) {
            color::GradeSpec spec;
            c.setup(spec);
            REQUIRE(spec.active());
            CompositeLayer layer = layerOf(makeRgbaFrame(kW, kH, palette));
            layer.effects = {gradeEffect(spec)};
            const double psnr = compare(*renderer, compositeOf({kW, kH}, {layer}), referenceGrade(input, spec), c.name);
            WARN(c.name << ": PSNR " << psnr << " dB");
            summary << "| " << c.name << " | " << std::round(psnr * 100) / 100 << " | " << c.minPsnr << " |\n";
            CHECK(psnr >= c.minPsnr);
            // The grade does something: far from the ungraded picture.
            const double untouched = compare(*renderer, compositeOf({kW, kH}, {layerOf(makeRgbaFrame(kW, kH, palette))}),
                                             referenceGrade(input, spec), c.name + "_untouched");
            CHECK(untouched < psnr - 3.0);
        }
    }
}

TEST_CASE("Grade: tetrahedral and trilinear LUTs both match their references and differ", "[render][gpu][grade]") {
    auto renderer = makeRenderer(OutputRole::Export);
    const LinearImage input = linearFromRgba8(kW, kH, palette);
    std::vector<std::uint8_t> pictures[2];
    for (const auto interpolation : {color::LutInterpolation::Tetrahedral, color::LutInterpolation::Trilinear}) {
        color::GradeSpec spec;
        // r*g, g*b, b*r on a 2-point lattice: exact for trilinear (bilinear products), piecewise linear for tetrahedral.
        color::Lut products;
        products.size3d = 2;
        for (int b = 0; b < 2; ++b) {
            for (int g = 0; g < 2; ++g) {
                for (int r = 0; r < 2; ++r) {
                    products.table3d.push_back(static_cast<float>(r * g));
                    products.table3d.push_back(static_cast<float>(g * b));
                    products.table3d.push_back(static_cast<float>(b * r));
                }
            }
        }
        spec.look = std::make_shared<const color::Lut>(std::move(products));
        spec.interpolation = interpolation;
        CompositeLayer layer = layerOf(makeRgbaFrame(kW, kH, palette));
        layer.effects = {gradeEffect(spec)};
        auto composite = compositeOf({kW, kH}, {layer});
        const std::string name = interpolation == color::LutInterpolation::Tetrahedral ? "lut_tetrahedral" : "lut_trilinear";
        CHECK(compare(*renderer, composite, referenceGrade(input, spec), name) >= 50.0);
        auto image = renderer->render(composite);
        REQUIRE(image);
        pictures[interpolation == color::LutInterpolation::Trilinear ? 1 : 0] = image->rgba;
    }
    int different = 0;
    for (std::size_t i = 0; i < pictures[0].size(); ++i) {
        different += std::abs(int(pictures[0][i]) - int(pictures[1][i])) > 2 ? 1 : 0;
    }
    CHECK(different > 100);
}

TEST_CASE("Grade: mask views, before/after split and the reference side by side", "[render][gpu][grade]") {
    auto renderer = makeRenderer(OutputRole::Display);
    const LinearImage input = linearFromRgba8(kW, kH, palette);
    color::GradeSpec spec;
    spec.params.hueCenter = 220;
    spec.params.hueWidth = 60;
    spec.params.hueSoftness = 20;
    spec.params.hslSaturation = 0;
    spec.params.exposure = 0.5;
    for (const int view : {1, 2, 3}) {
        CompositeLayer layer = layerOf(makeRgbaFrame(kW, kH, palette));
        layer.effects = {gradeEffect(spec)};
        layer.clipId = 7;
        auto composite = compositeOf({kW, kH}, {layer});
        composite->colorView.maskView = view;
        composite->colorView.maskClip = 7;
        const LinearImage expectedMask = referenceGrade(input, spec, view);
        const double maskPsnr = compare(*renderer, composite, expectedMask, "mask_view_" + std::to_string(view));
        if (maskPsnr < 45.0) { // diagnose: the worst pixels
            auto img = renderer->render(composite);
            const auto ref = encodeOverBlack(expectedMask, OutputTransform::Rec709);
            int shown = 0;
            for (int y = 0; y < kH && shown < 6; ++y) {
                for (int x = 0; x < kW && shown < 6; ++x) {
                    const int d = std::abs(int(img->pixel(x, y)[0]) - int(ref[(y * kW + x) * 3]));
                    if (d > 40) {
                        const auto in = palette(x, y);
                        const color::Rgb e = color::gradeMain(spec, straightOf(input.at(x, y)));
                        const color::Hsv hsv = color::toHsv(e);
                        WARN("mask " << view << " at " << x << "," << y << ": gpu " << int(img->pixel(x, y)[0]) << " ref "
                                     << int(ref[(y * kW + x) * 3]) << " input " << int(in[0]) << " " << int(in[1]) << " "
                                     << int(in[2]) << " hue " << hsv.h * 360 << " s " << hsv.s);
                        ++shown;
                    }
                }
            }
        }
        CHECK(maskPsnr >= 45.0);
        // Another clip's mask is not shown.
        composite->colorView.maskClip = 8;
        CHECK(compare(*renderer, composite, referenceGrade(input, spec), "mask_other_clip") >= 45.0);
    }
    // Split at 40 %: left = before the grade, right = after.
    CompositeLayer layer = layerOf(makeRgbaFrame(kW, kH, palette));
    layer.effects = {gradeEffect(spec)};
    auto composite = compositeOf({kW, kH}, {layer});
    composite->colorView.split = 0.4;
    auto image = renderer->render(composite);
    REQUIRE(image);
    savePng(*image, QStringLiteral("grade/split.png"));
    const LinearImage graded = referenceGrade(input, spec);
    const std::vector<std::uint8_t> after = encodeOverBlack(graded, OutputTransform::Rec709);
    const std::vector<std::uint8_t> before = encodeOverBlack(input, OutputTransform::Rec709);
    double errLeft = 0, errRight = 0;
    for (int y = 0; y < kH; ++y) {
        for (const int x : {20, 60, 100}) { // left of 128
            for (int c = 0; c < 3; ++c) {
                errLeft = std::max(errLeft, std::abs(image->pixel(x, y)[c] - double(before[(y * kW + x) * 3 + c])));
            }
        }
        for (const int x : {160, 250, 310}) {
            for (int c = 0; c < 3; ++c) {
                errRight = std::max(errRight, std::abs(image->pixel(x, y)[c] - double(after[(y * kW + x) * 3 + c])));
            }
        }
    }
    CHECK(errLeft <= 2.0);
    CHECK(errRight <= 3.0);
    // Side by side with a reference still: the program in the right half, the still in the left.
    std::vector<std::uint8_t> still(kW * kH * 4);
    for (int i = 0; i < kW * kH; ++i) {
        still[i * 4] = 200, still[i * 4 + 1] = 40, still[i * 4 + 2] = 40, still[i * 4 + 3] = 255;
    }
    auto reference = std::make_shared<const StillImage>(kW, kH, std::move(still));
    CompositeLayer program = layerOf(makeRgbaFrame(kW, kH, palette));
    std::vector<CompositeLayer> layers{program};
    // The scheduler's placement (playback::placeSideBySide), inlined here to keep render tests free of playback.
    for (CompositeLayer& l : layers) {
        l.transform.position = {kW / 4.0, 0};
        l.transform.scale = 0.5;
    }
    CompositeLayer left;
    left.view = reference->view(reference);
    left.transform.position = {-kW / 4.0, 0};
    left.transform.scale = 0.5;
    layers.insert(layers.begin(), left);
    auto sideBySide = renderer->render(compositeOf({kW, kH}, layers));
    REQUIRE(sideBySide);
    savePng(*sideBySide, QStringLiteral("grade/side_by_side.png"));
    const std::uint8_t* l = sideBySide->pixel(kW / 4, kH / 2);
    CHECK(std::abs(int(l[0]) - 200) <= 2); // the still, as it was (BT.709 in, BT.709 out)
    CHECK(std::abs(int(l[1]) - 40) <= 2);
    const std::uint8_t* top = sideBySide->pixel(3 * kW / 4, 5);
    CHECK(top[0] + top[1] + top[2] == 0); // letterboxed above the half-size program
}

TEST_CASE("Grade: colour views reach graded layers inside a nested composite (I.9)", "[render][gpu][grade][nested]") {
    auto renderer = makeRenderer(OutputRole::Display);
    color::GradeSpec spec;
    spec.params.hueCenter = 220;
    spec.params.hueWidth = 60;
    spec.params.hueSoftness = 20;
    spec.params.hslSaturation = 0;
    spec.params.exposure = 1.0;
    const auto nest = [&](bool graded, std::uint64_t leafClip) {
        CompositeLayer leaf = layerOf(makeRgbaFrame(kW, kH, palette));
        if (graded) {
            leaf.effects = {gradeEffect(spec)};
        }
        leaf.clipId = leafClip;
        auto inner = std::make_shared<PreparedComposite>();
        inner->canvas = {kW, kH};
        inner->layers = {leaf};
        // The nest at half size in the right half of the canvas: x 160..320 of 320, y 45..135.
        CompositeLayer group;
        group.nested = inner;
        group.transform.position = {kW / 4.0, 0.0};
        group.transform.scale = 0.5;
        group.clipId = 9;
        return compositeOf({kW, kH}, {group});
    };
    const auto rendered = [&](const std::shared_ptr<PreparedComposite>& composite, const std::string& name) {
        auto image = renderer->render(composite);
        REQUIRE(image);
        savePng(*image, QString::fromStdString("grade/" + name + ".png"));
        return std::move(*image);
    };
    const RenderedImage before = rendered(nest(false, 7), "nested_before");
    const RenderedImage after = rendered(nest(true, 7), "nested_after");
    const auto columnsMatch = [&](const RenderedImage& image, const RenderedImage& reference, int x0, int x1) {
        int worst = 0;
        for (int y = 50; y < 130; y += 3) {
            for (int x = x0; x < x1; x += 3) {
                for (int c = 0; c < 3; ++c) {
                    worst = std::max(worst, std::abs(int(image.pixel(x, y)[c]) - int(reference.pixel(x, y)[c])));
                }
            }
        }
        return worst;
    };
    CHECK(columnsMatch(before, after, 165, 315) > 20); // the grade shows in the nest

    // Before/after at 75 % of the screen (x 240): the nested layer's grade splits there, not at 75 % of its own canvas.
    auto split = nest(true, 7);
    split->colorView.split = 0.75;
    const RenderedImage splitImage = rendered(split, "nested_split");
    CHECK(columnsMatch(splitImage, before, 165, 236) <= 2);
    CHECK(columnsMatch(splitImage, after, 245, 315) <= 2);

    // The mask view of the nested clip (9) shows the matte of every graded layer in it; another clip's (8) shows none.
    auto masked = nest(true, 7);
    masked->colorView.maskView = 1;
    masked->colorView.maskClip = 9;
    const RenderedImage maskImage = rendered(masked, "nested_mask");
    CHECK(columnsMatch(maskImage, after, 165, 315) > 20);
    masked->colorView.maskClip = 7; // the layer inside, by its own clip
    CHECK(columnsMatch(rendered(masked, "nested_mask_leaf"), maskImage, 165, 315) <= 1);
    masked->colorView.maskClip = 8;
    CHECK(columnsMatch(rendered(masked, "nested_mask_other"), after, 165, 315) <= 1);
}

TEST_CASE("Grade: GPU cost of a full grade at 1080p", "[render][gpu][grade][perf]") {
    OffscreenOptions options;
    options.timestamps = true;
    auto created = OffscreenRenderer::create(options);
    REQUIRE(created);
    OffscreenRenderer& renderer = **created;
    const auto picture = [](int x, int y) { return std::array<std::uint8_t, 4>{byte(x / 1920.0), byte(y / 1080.0), byte(0.5), 255}; };
    const media::VideoFrame frame = makeRgbaFrame(1920, 1080, picture);
    color::GradeCurves curves;
    curves.master = {{0, 0}, {0.5, 0.55}, {1, 1}};
    curves.hueSat = {{0, 0.5}, {0.5, 0.7}};
    color::GradeSpec full;
    full.inputLut = color::builtinLut("builtin:slog3-sgamut3cine-to-rec709");
    full.look = color::builtinLut("builtin:teal-orange");
    full.curves = color::bakeCurves(curves);
    full.params.temperature = 20, full.params.exposure = 0.2, full.params.contrast = 10, full.params.highlights = -20;
    full.params.saturation = 110, full.params.fadedFilm = 10, full.params.shadowTintAmount = 20;
    full.params.lift = {0.01, 0, 0}, full.params.cdlSaturation = 1.1;
    full.params.hueCenter = 30, full.params.hueWidth = 60, full.params.hslSaturation = 120;
    full.params.vignetteAmount = -1;
    color::GradeSpec refined = full;
    refined.params.denoise = 20, refined.params.blur = 8, refined.params.hslSharpen = 30;
    const auto timeOf = [&](const std::vector<LayerEffect>& effects) {
        CompositeLayer layer = layerOf(frame);
        layer.effects = effects;
        auto composite = compositeOf({1920, 1080}, {layer});
        std::vector<double> gpu;
        for (int i = 0; i < 40; ++i) {
            auto t = renderer.renderTimed(composite);
            REQUIRE(t);
            if (i >= 5 && t->gpuMs >= 0) {
                gpu.push_back(t->gpuMs);
            }
        }
        std::sort(gpu.begin(), gpu.end());
        return gpu.empty() ? -1.0 : gpu[gpu.size() / 2];
    };
    const double plain = timeOf({});
    const double graded = timeOf({gradeEffect(full)});
    const double withRefine = timeOf({gradeEffect(refined)});
    WARN("1080p GPU frame: plain " << plain << " ms, full grade " << graded << " ms, with HSL refine " << withRefine << " ms ("
                                   << renderer.adapterName() << ")");
    std::ofstream report((gradeDir() + QStringLiteral("/perf.md")).toStdString(), std::ios::trunc);
    report << "# Grade GPU cost, 1920x1080 (" << renderer.backendName() << ", " << renderer.adapterName() << ")\n\n"
           << "Median GPU time of a whole frame (layer, effects, output), 35 frames each:\n\n"
           << "| frame | GPU ms | grade ms |\n|---|---|---|\n"
           << "| no grade | " << plain << " | - |\n"
           << "| full grade (input LUT 65^3, look 33^3, curves, basic, creative, wheels, HSL, vignette) | " << graded << " | "
           << graded - plain << " |\n"
           << "| full grade + HSL denoise, blur and sharpen | " << withRefine << " | " << withRefine - plain << " |\n";
    if (plain >= 0) {
        CHECK(graded - plain < 8.0); // well inside a 60 fps frame budget even on modest GPUs
    }
}
