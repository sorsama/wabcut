// The grade's CPU reference (Grade.h): the parameter table against the registry, curve baking (monotone, exact end
// points, periodic hue curves), and what each stage does on its own.

#include <wabcut/color/Cdl.h>
#include <wabcut/color/Grade.h>
#include <wabcut/color/Looks.h>
#include <wabcut/model/Effects.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>

using namespace wabcut;
using namespace wabcut::color;
using Catch::Approx;

namespace {

GradeSpec specOf(GradeParams p) {
    GradeSpec s;
    s.params = p;
    return s;
}

double maxDiff(const Rgb& a, const Rgb& b) {
    return std::max({std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2])});
}

} // namespace

TEST_CASE("Grade parameters: the table covers the registry, defaults agree", "[color][grade]") {
    const EffectDescriptor* d = EffectRegistry::builtin().find(kGradeEffect);
    REQUIRE(d);
    const GradeParams defaults;
    const auto all = gradeParamsToEffect(defaults, false);
    CHECK(all.size() == d->params.size());
    for (const EffectParamInfo& info : d->params) {
        INFO(info.key);
        const auto it = std::find_if(all.begin(), all.end(), [&](const EffectParam& p) { return p.key == info.key; });
        REQUIRE(it != all.end());
        CHECK(it->value.constant() == info.defaultValue);
    }
    CHECK(std::is_sorted(all.begin(), all.end(), [](const EffectParam& a, const EffectParam& b) { return a.key < b.key; }));
    CHECK(gradeParamsToEffect(defaults, true).empty());
    // Round trip through an instance, with a keyframed parameter.
    EffectInstance e;
    e.effect = std::string(kGradeEffect);
    GradeParams p;
    p.exposure = 1.25;
    p.lift = {0.01, -0.02, 0.03};
    p.hsl = false;
    p.invert = true;
    for (const EffectParam& param : gradeParamsToEffect(p, true)) {
        e.set(param.key, param.value);
    }
    e.set("basic.contrast", ParamValue(Curve{{Keyframe{Ticks{0}, 0.0}, Keyframe{Ticks{100}, 50.0}}}));
    const GradeParams back = gradeParamsAt(e, Ticks{50});
    CHECK(back.exposure == 1.25);
    CHECK(back.lift == Rgb{0.01, -0.02, 0.03});
    CHECK_FALSE(back.hsl);
    CHECK(back.invert);
    CHECK(back.contrast == Approx(25.0));
    CHECK(gradeParamsAt(e, Ticks{100}).contrast == Approx(50.0));
}

TEST_CASE("Curves: monotone cubics through the points, exact end points, no overshoot", "[color][grade][curves]") {
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    for (int trial = 0; trial < 200; ++trial) {
        // Random monotone points (some flat runs), x strictly increasing.
        std::vector<EffectPoint> pts;
        const int n = 2 + static_cast<int>(rng() % 7);
        double x = 0.0, y = u(rng) * 0.2;
        for (int i = 0; i < n; ++i) {
            pts.push_back({x, y});
            x += 0.02 + u(rng) * (1.0 - x) / (n - i);
            y += (rng() % 4 == 0) ? 0.0 : u(rng) * (1.0 - y) * 0.5;
        }
        GradeCurves curves;
        curves.master = pts;
        const auto baked = bakeCurves(curves);
        double previous = -1.0;
        for (int i = 0; i < BakedCurves::kSize; ++i) {
            const double v = baked->rgbm[static_cast<std::size_t>(i) * 4 + 3];
            CHECK(v >= previous - 1e-9);
            CHECK(v >= pts.front().y - 1e-6); // never below the first nor above the last point (float table)
            CHECK(v <= pts.back().y + 1e-6);
            previous = v;
        }
        for (const EffectPoint& p : pts) {
            CHECK(evaluateCurve(pts, p.x) == Approx(p.y).margin(1e-12)); // through every point
        }
        CHECK(evaluateCurve(pts, -0.25) == Approx(evaluateCurve(pts, 0.0) - 0.25)); // slope 1 beyond [0, 1]
        CHECK(evaluateCurve(pts, 1.5) == Approx(evaluateCurve(pts, 1.0) + 0.5));
    }
    // Identity: no points, or the two corners.
    CHECK(bakeCurves(GradeCurves{})->rgbIdentity);
    GradeCurves corners;
    corners.red = {{0, 0}, {1, 1}};
    CHECK(bakeCurves(corners)->rgbIdentity);
    corners.red = {{0, 0}, {0.5, 0.6}, {1, 1}};
    const auto lifted = bakeCurves(corners);
    CHECK_FALSE(lifted->rgbIdentity);
    CHECK(lifted->rgbm[0] == 0.0f);                                        // end points exact
    CHECK(lifted->rgbm[(BakedCurves::kSize - 1) * 4] == Approx(1.0).margin(1e-7));
    CHECK(sampleRgbCurve(*lifted, 0, 0.5) == Approx(0.6).margin(1e-6));     // the table hits the control point
}

TEST_CASE("Hue curves are periodic and smooth across the wrap", "[color][grade][curves]") {
    const std::vector<EffectPoint> pts{{0.05, 0.8}, {0.4, 0.5}, {0.7, 0.3}, {0.95, 0.6}};
    // Continuous at 0/1 and periodic.
    CHECK(evaluateHueCurve(pts, 0.0) == Approx(evaluateHueCurve(pts, 1.0)));
    CHECK(evaluateHueCurve(pts, 0.999999) == Approx(evaluateHueCurve(pts, 0.0)).margin(1e-4));
    CHECK(evaluateHueCurve(pts, 1.25) == Approx(evaluateHueCurve(pts, 0.25)));
    CHECK(evaluateHueCurve(pts, -0.3) == Approx(evaluateHueCurve(pts, 0.7)));
    for (const EffectPoint& p : pts) {
        CHECK(evaluateHueCurve(pts, p.x) == Approx(p.y).margin(1e-12));
    }
    // No overshoot beyond the data's range.
    for (int i = 0; i < 1000; ++i) {
        const double v = evaluateHueCurve(pts, i / 1000.0);
        CHECK(v >= 0.3 - 1e-9);
        CHECK(v <= 0.8 + 1e-9);
    }
    CHECK(evaluateHueCurve({}, 0.3) == 0.5);
    CHECK(evaluateHueCurve({{0.3, 0.7}}, 0.9) == 0.7);
    GradeCurves curves;
    curves.hueSat = pts;
    const auto baked = bakeCurves(curves);
    CHECK_FALSE(baked->hueNeutral);
    CHECK(baked->satNeutral);
    CHECK(sampleRow(baked->hue, 4, 0, 0.9999, true) == Approx(sampleRow(baked->hue, 4, 0, 0.0, true)).margin(2e-3));
}

TEST_CASE("Grade stages on their own", "[color][grade]") {
    const Rgb grey{0.18, 0.18, 0.18};
    const Rgb colour{0.4, 0.2, 0.08};
    // Neutral: identity (and no stage is active).
    {
        const GradeSpec s = specOf({});
        CHECK_FALSE(s.active());
        CHECK(maxDiff(gradePixel(s, colour), colour) < 1e-12);
    }
    // Exposure: x 2^stops in linear light.
    {
        GradeParams p;
        p.exposure = 1.0;
        CHECK(maxDiff(gradePixel(specOf(p), colour), colour * 2.0) < 1e-12);
    }
    // Contrast pivots at 18 % grey.
    {
        GradeParams p;
        p.contrast = 60;
        CHECK(maxDiff(gradePixel(specOf(p), grey), grey) < 1e-12);
        CHECK(gradePixel(specOf(p), {0.5, 0.5, 0.5})[0] > 0.5);
        CHECK(gradePixel(specOf(p), {0.05, 0.05, 0.05})[0] < 0.05);
    }
    // White balance keeps a neutral's luminance and moves warm/cool the right way.
    {
        GradeParams p;
        p.temperature = 50;
        const Rgb warm = gradePixel(specOf(p), grey);
        CHECK(luma(warm) == Approx(0.18).margin(1e-9));
        CHECK(warm[0] > warm[2]);
        p.temperature = 0;
        p.tint = 50;
        const Rgb magenta = gradePixel(specOf(p), grey);
        CHECK(magenta[1] < magenta[0]);
        CHECK(magenta[1] < magenta[2]);
        const Mat3 m = whiteBalanceMatrix(37, -12);
        const Mat3 back = inverse(m);
        CHECK(maxDiff(mul(back, mul(m, colour)), colour) < 1e-12);
    }
    // Tone controls: a monotone curve, mid grey fixed, no clipping.
    {
        GradeParams p;
        p.highlights = -100;
        p.shadows = 100;
        p.whites = 100;
        p.blacks = -100;
        double previous = -1e9;
        for (int i = -10; i <= 110; ++i) {
            const double x = i / 100.0;
            const double y = toneCurve(p, x);
            CHECK(y > previous);
            previous = y;
        }
        CHECK(toneCurve(p, 0.5) == Approx(0.5));
        CHECK(toneCurve(p, 1.0) == Approx(1.12));
        CHECK(toneCurve(p, 0.0) == Approx(-0.12));
    }
    // Saturation keeps luma; vibrance favours dull colours and spares skin.
    {
        GradeParams p;
        p.saturation = 0;
        const Rgb mono = gradePixel(specOf(p), colour);
        CHECK(mono[0] == Approx(mono[1]).margin(1e-12));
        CHECK(luma(mono) == Approx(luma(colour)).margin(1e-12));
        p.saturation = 100;
        p.vibrance = 100;
        const Rgb dull{0.2, 0.18, 0.17};
        const Rgb vivid{0.6, 0.02, 0.02};
        const auto gain = [&](const Rgb& c) {
            const Rgb out = gradePixel(specOf(p), c);
            return (out[0] - luma(out)) / (c[0] - luma(c));
        };
        CHECK(gain(dull) > gain(vivid));
        const Rgb skin = decode709({0.85, 0.62, 0.5});
        const Rgb notSkin = decode709({0.5, 0.62, 0.85}); // the same saturation, opposite hue
        CHECK(gain(skin) < gain(notSkin) + 1e-9);
    }
    // Faded film lifts black.
    {
        GradeParams p;
        p.fadedFilm = 100;
        CHECK(encode709(gradePixel(specOf(p), {0, 0, 0}))[0] == Approx(0.2));
        CHECK(encode709(gradePixel(specOf(p), {1, 1, 1}))[0] == Approx(1.0));
    }
    // Wheels = the CDL, round trip through cdlOf/applyCdl.
    {
        GradeParams p;
        p.lift = {0.05, 0.0, -0.03};
        p.gain = {1.1, 0.95, 1.2};
        p.gamma = {0.9, 1.0, 1.2};
        p.offset = {0.01, 0.02, 0.0};
        p.cdlSaturation = 0.8;
        const Cdl cdl = cdlOf(p);
        GradeParams q;
        applyCdl(q, cdl);
        for (const Rgb& c : {colour, grey, Rgb{0.9, 0.7, 0.1}}) {
            const Rgb a = encode709(gradePixel(specOf(p), c));
            const Rgb b = encode709(gradePixel(specOf(q), c));
            const Rgb ref = applyCdlToPixel(cdl, encode709(c));
            CHECK(maxDiff(a, b) < 1e-12);
            CHECK(maxDiff(a, ref) < 1e-12);
        }
    }
    // The HSL qualifier: hue band with softness, inverse.
    {
        GradeParams p;
        p.hueCenter = 120; // green
        p.hueWidth = 40;
        p.hueSoftness = 20;
        CHECK(qualifierMatte(p, {0.1, 0.8, 0.1}) == Approx(1.0));
        CHECK(qualifierMatte(p, {0.8, 0.1, 0.1}) == Approx(0.0));
        const Rgb edge = {0.1, 0.8, 0.8 * std::tan(0.0) + 0.1};
        CHECK(qualifierMatte(p, edge) == Approx(1.0));
        p.invert = true;
        CHECK(qualifierMatte(p, {0.8, 0.1, 0.1}) == Approx(1.0));
        p.invert = false;
        p.hslSaturation = 0;
        const GradeSpec s = specOf(p);
        CHECK(s.hslActive());
        const Rgb out = encode709(gradePixel(s, decode709({0.1, 0.8, 0.1})));
        CHECK(out[0] == Approx(out[1]).margin(1e-9)); // the qualified green is desaturated
        const Rgb red = encode709(gradePixel(s, decode709({0.8, 0.1, 0.1})));
        CHECK(red[0] == Approx(0.8).margin(1e-9)); // red untouched
    }
    // Vignette darkens corners, not the centre.
    {
        GradeParams p;
        p.vignetteAmount = -2;
        CHECK(maxDiff(gradePixel(specOf(p), grey, 0, 0), grey) < 1e-12);
        CHECK(gradePixel(specOf(p), grey, 0.95, 0.95)[0] < 0.18 * 0.5);
    }
    // Stage switches turn whole stages off.
    {
        GradeParams p;
        p.exposure = 2;
        p.basic = false;
        CHECK_FALSE(specOf(p).active());
    }
}

TEST_CASE("evaluateGrade bakes curves once and resolves LUTs", "[color][grade]") {
    EffectInstance e;
    e.effect = std::string(kGradeEffect);
    e.setData(EffectData{std::string(gradekey::master), std::vector<EffectPoint>{{0, 0}, {0.5, 0.4}, {1, 1}}});
    e.setData(EffectData{std::string(gradekey::look), std::string("builtin:warm-film")});
    e.setData(EffectData{std::string(gradekey::inputLut), std::string("asset:404")});
    const LutResolver resolver = [](std::string_view ref) { return ref.starts_with("builtin:") ? builtinLut(ref) : nullptr; };
    const GradeSpec a = evaluateGrade(e, Ticks{0}, resolver);
    const GradeSpec b = evaluateGrade(e, Ticks{1000}, resolver);
    REQUIRE(a.curves);
    CHECK(a.curves == b.curves); // shared, not re-baked per frame
    CHECK(a.look);
    CHECK_FALSE(a.inputLut);
    CHECK(a.missingLuts == 1);
    CHECK(a.curvesActive());
    CHECK(a.creativeActive());
    // A look at 0 % intensity is a no-op; at 100 % it is the LUT.
    GradeSpec s = a;
    s.curves = nullptr;
    s.params.lookIntensity = 0;
    const Rgb c{0.2, 0.3, 0.4};
    CHECK(maxDiff(gradePixel(s, c), c) < 1e-9);
    s.params.lookIntensity = 100;
    CHECK(maxDiff(encode709(gradePixel(s, c)), applyLut(*a.look, encode709(c))) < 1e-9);
}
