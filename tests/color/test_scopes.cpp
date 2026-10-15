// Scope binning and statistics on synthetic pictures: histogram bins exact on ramps, waveform columns, vectorscope
// colour-bar targets, the step grid, verdicts.

#include <wabcut/color/Scopes.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <functional>
#include <numeric>
#include <vector>

using namespace wabcut;
using namespace wabcut::color;
using Catch::Approx;

namespace {

std::vector<float> picture(int w, int h, const std::function<Rgb(int, int)>& f) {
    std::vector<float> px(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const Rgb c = f(x, y);
            float* p = px.data() + (static_cast<std::size_t>(y) * w + x) * 4;
            p[0] = static_cast<float>(c[0]);
            p[1] = static_cast<float>(c[1]);
            p[2] = static_cast<float>(c[2]);
            p[3] = 1.0f;
        }
    }
    return px;
}

} // namespace

TEST_CASE("Scopes: a horizontal ramp fills every histogram bin exactly and climbs the waveform", "[color][scopes]") {
    // 256 columns, each one level: bin centres (i + 0.5) / 256, so every pixel lands in its own bin.
    const int w = 256, h = 16;
    const auto px = picture(w, h, [](int x, int) {
        const double v = (x + 0.5) / 256.0;
        return Rgb{v, v, v};
    });
    ScopeSettings s;
    s.step = 1;
    const ScopeData d = computeScopes(px.data(), w, h, 4, s);
    CHECK(d.pixels == static_cast<std::uint64_t>(w * h));
    for (int c = 0; c < 4; ++c) {
        for (int i = 0; i < 256; ++i) {
            INFO("channel " << c << " bin " << i);
            CHECK(d.histogram[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] == static_cast<std::uint32_t>(h));
        }
    }
    // Waveform: column x has all its pixels at level x.
    for (int x = 0; x < 256; ++x) {
        CHECK(d.waveLuma[static_cast<std::size_t>(x) * 256 + static_cast<std::size_t>(x)] == static_cast<std::uint32_t>(h));
    }
    // Greys sit in the vectorscope's centre.
    CHECK(d.vector[128 * 256 + 128] == static_cast<std::uint32_t>(w * h));
    const ScopeStats st = scopeStats(d);
    CHECK(st.lumaMin == Approx(0.0));
    CHECK(st.lumaMax == Approx(100.0));
    CHECK(st.p50 == Approx(50.0).margin(0.5));
    CHECK(st.lumaMean == Approx(50.0).margin(0.01));
    CHECK(st.cast == "neutral");
    CHECK(st.exposure == "ok");
}

TEST_CASE("Scopes: colour bars land on the vectorscope targets; parades split the channels", "[color][scopes]") {
    const std::vector<Rgb> bars{{1, 1, 1}, {1, 1, 0}, {0, 1, 1}, {0, 1, 0}, {1, 0, 1}, {1, 0, 0}, {0, 0, 1}};
    for (const double amount : {0.75, 1.0}) {
        const int w = 70, h = 10;
        const auto px = picture(w, h, [&](int x, int) { return bars[static_cast<std::size_t>(x / 10)] * amount; });
        for (const double zoom : {1.0, 2.0}) {
            ScopeSettings s;
            s.step = 1;
            s.vectorZoom = zoom;
            const ScopeData d = computeScopes(px.data(), w, h, 4, s);
            for (std::size_t b = 1; b < bars.size(); ++b) {
                const auto target = vectorscopeTarget(bars[b], amount);
                const int vx = std::clamp(static_cast<int>(std::floor(256 * (0.5 + target[0] * zoom) + 0.5)), 0, 255);
                const int vy = std::clamp(static_cast<int>(std::floor(256 * (0.5 - target[1] * zoom) + 0.5)), 0, 255);
                INFO("bar " << b << " amount " << amount << " zoom " << zoom);
                CHECK(d.vector[static_cast<std::size_t>(vy) * 256 + static_cast<std::size_t>(vx)] >= 100u);
            }
        }
        // 100 % red: Cr = +0.5 exactly (the top of the scope), Cb = -0.1146.
        const auto red = vectorscopeTarget({1, 0, 0}, 1.0);
        CHECK(red[1] == Approx(0.5).margin(1e-4));
        CHECK(red[0] == Approx(-0.1146).margin(1e-4));
    }
    // RGB parade: a red frame puts every R sample at the top, G and B at the bottom.
    const auto red = picture(64, 8, [](int, int) { return Rgb{1, 0, 0}; });
    ScopeSettings s;
    s.step = 1;
    const ScopeData d = computeScopes(red.data(), 64, 8, 4, s);
    CHECK(std::accumulate(d.waveRgb[0].begin() + 255 * 256, d.waveRgb[0].end(), 0u) == 64u * 8u);
    CHECK(std::accumulate(d.waveRgb[1].begin(), d.waveRgb[1].begin() + 256, 0u) == 64u * 8u);
    const ScopeStats st = scopeStats(d);
    CHECK(st.cast == "red");
    CHECK(st.clippedHigh == Approx(1.0));
}

TEST_CASE("Scopes: the step grid, transparent pixels, verdicts, images", "[color][scopes]") {
    const int w = 1920, h = 1080;
    ScopeSettings s;
    CHECK(scopeStep(s, w, h) == 2); // 2.07 M pixels -> every second pixel each way (518 k)
    CHECK(scopeStep(s, 640, 360) == 1);
    const auto dark = picture(w, h, [](int x, int) {
        const double v = 0.02 + 0.15 * x / 1920.0;
        return Rgb{v, v * 0.7, v * 0.5};
    });
    const ScopeData d = computeScopes(dark.data(), w, h, 4, s);
    CHECK(d.step == 2);
    CHECK(d.pixels == 960u * 540u);
    const ScopeStats st = scopeStats(d);
    CHECK(st.exposure == "underexposed");
    CHECK(st.cast != "neutral");
    // Transparent pixels are not counted.
    auto half = picture(10, 10, [](int, int) { return Rgb{0.5, 0.5, 0.5}; });
    for (int i = 0; i < 50; ++i) {
        half[static_cast<std::size_t>(i) * 4 + 3] = 0.0f;
    }
    CHECK(computeScopes(half.data(), 10, 10, 4).pixels == 50u);
    // 8-bit input agrees with float input for exact code values.
    std::vector<std::uint8_t> rgba8(16 * 4 * 4);
    std::vector<float> rgbaF(16 * 4 * 4);
    for (int i = 0; i < 64; ++i) {
        for (int c = 0; c < 3; ++c) {
            rgba8[static_cast<std::size_t>(i) * 4 + static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(i * 4 + c);
            rgbaF[static_cast<std::size_t>(i) * 4 + static_cast<std::size_t>(c)] = static_cast<float>(i * 4 + c) / 255.0f;
        }
        rgba8[static_cast<std::size_t>(i) * 4 + 3] = 255;
        rgbaF[static_cast<std::size_t>(i) * 4 + 3] = 1.0f;
    }
    ScopeSettings exact;
    exact.step = 1;
    const ScopeData a = computeScopes8(rgba8.data(), 16, 4, 16 * 4, exact);
    const ScopeData b = computeScopes(rgbaF.data(), 16, 4, 4, exact);
    CHECK(a.histogram[0] == b.histogram[0]);
    CHECK(a.waveLuma == b.waveLuma);
    // Images have the documented sizes.
    int iw = 0, ih = 0;
    const auto wave = renderWaveform(d, 1, iw, ih);
    CHECK(wave.size() == static_cast<std::size_t>(iw * ih * 4));
    CHECK(iw == 3 * 256);
    const auto vec = renderVectorscope(d, iw, ih);
    CHECK(vec.size() == static_cast<std::size_t>(256 * 256 * 4));
    const auto hist = renderHistogram(d, iw, ih);
    CHECK(hist.size() == static_cast<std::size_t>(iw * ih * 4));
}

TEST_CASE("Scope images: pooling keeps every count, a single hit stays visible", "[color][scopes]") {
    // A flat grey picture with one bright pixel: the bright one is a single hit next to thousands.
    std::vector<std::uint8_t> rgba(64 * 64 * 4, 0);
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i] = rgba[i + 1] = rgba[i + 2] = 100;
        rgba[i + 3] = 255;
    }
    rgba[0] = rgba[1] = rgba[2] = 230;
    ScopeSettings settings;
    settings.step = 1;
    const ScopeData d = computeScopes8(rgba.data(), 64, 64, 64 * 4, settings);
    int w = 0, h = 0;
    const auto wave = renderWaveform(d, 0, w, h);
    const int brightLevel = static_cast<int>(std::floor(230.0 / 255.0 * 256.0));
    const std::size_t at = (static_cast<std::size_t>(255 - brightLevel) * static_cast<std::size_t>(w)) * 4; // column 0
    CHECK(wave[at + 3] == 255);
    CHECK(wave[at] >= static_cast<std::uint8_t>(0.4 * 235)); // 40 % of full, not near black

    const ScopeData pooled = pooledScopes(d, 2, 4, 2);
    CHECK(pooled.settings.columns == 128);
    CHECK(pooled.settings.levels == 64);
    CHECK(pooled.settings.vectorSize == 128);
    CHECK(pooled.waveLuma.size() == 128u * 64u);
    CHECK(pooled.vector.size() == 128u * 128u);
    CHECK(pooled.histogram[3].size() == 64u);
    const auto sum = [](const std::vector<std::uint32_t>& v) { return std::accumulate(v.begin(), v.end(), std::uint64_t{0}); };
    CHECK(sum(pooled.waveLuma) == sum(d.waveLuma));
    CHECK(sum(pooled.waveRgb[1]) == sum(d.waveRgb[1]));
    CHECK(sum(pooled.waveCbCr[0]) == sum(d.waveCbCr[0]));
    CHECK(sum(pooled.vector) == sum(d.vector));
    CHECK(sum(pooled.histogram[0]) == sum(d.histogram[0]));
    CHECK(pooled.histogram[3][static_cast<std::size_t>(brightLevel / 4)] == 1u);
    // Factors that do not divide the size are ignored.
    const ScopeData same = pooledScopes(d, 3, 1, 5);
    CHECK(same.settings.columns == d.settings.columns);
    CHECK(same.waveLuma == d.waveLuma);
    int pw = 0, ph = 0;
    const auto pooledWave = renderWaveform(pooled, 0, pw, ph);
    CHECK(pw == 128);
    CHECK(ph == 64);
    CHECK(pooledWave.size() == static_cast<std::size_t>(pw * ph * 4));
}
