// Reference blend modes (Blend.h): hand-computed pixels for a few modes, every mode against goldens, the W3C identities
// every mode keeps, and Dissolve's pattern.

#include "ReferenceTestSupport.h"

#include <wabcut/render/reference/Blend.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <utility>
#include <vector>

using namespace wabcut;
using namespace wabcut::render::reference;
using Catch::Approx;
using test::premultiplied;

namespace {

Image onePixel(const Rgb& c, double a) {
    Image out(1, 1);
    out.set(0, 0, premultiplied(c[0], c[1], c[2], a));
    return out;
}

void checkPixel(const Image& image, const Rgba& want) {
    REQUIRE(image.width == 1);
    REQUIRE(image.height == 1);
    const Rgba got = image.atD(0, 0);
    for (std::size_t c = 0; c < 4; ++c) {
        INFO("channel " << c);
        CHECK(got[c] == Approx(want[c]).margin(1e-6));
    }
}

// The 8 x 2 pair of the golden test: 8 colours per row (HDR and the branch points 0, 0.25, 0.5, 1 included). The
// backdrop is opaque on row 0 and half transparent on row 1; the source is opaque in even columns and half transparent
// in odd ones, so every pair of alphas (1, 0.5) appears.
const std::vector<Rgb>& backdropColors() {
    static const std::vector<Rgb> colors{{0.2, 0.5, 0.8},   {0.9, 0.1, 0.4}, {0.0, 0.0, 0.0},  {1.0, 1.0, 1.0},
                                         {0.25, 0.75, 0.5}, {0.6, 0.6, 0.6}, {1.5, 0.3, 0.05}, {0.05, 0.95, 0.3}};
    return colors;
}

const std::vector<Rgb>& sourceColors() {
    static const std::vector<Rgb> colors{{0.6, 0.4, 0.1},   {0.3, 0.8, 0.5}, {0.5, 0.5, 0.5}, {0.2, 0.0, 1.0},
                                         {0.75, 0.25, 0.9}, {0.0, 1.0, 0.0}, {0.4, 1.2, 0.7}, {0.5, 0.5, 0.0}};
    return colors;
}

Image goldenBackdrop() {
    Image out(8, 2);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 8; ++x) {
            const Rgb& c = backdropColors()[static_cast<std::size_t>(x)];
            out.set(x, y, premultiplied(c[0], c[1], c[2], y == 0 ? 1.0 : 0.5));
        }
    }
    return out;
}

Image goldenSource() {
    Image out(8, 2);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 8; ++x) {
            const Rgb& c = sourceColors()[static_cast<std::size_t>(x)];
            out.set(x, y, premultiplied(c[0], c[1], c[2], x % 2 == 0 ? 1.0 : 0.5));
        }
    }
    return out;
}

} // namespace

TEST_CASE("Blend reference: hand-computed pixels of seven modes at alpha 1 and 0.5", "[render_reference][blend]") {
    const Rgb cb{0.2, 0.5, 0.8};
    const Rgb cs{0.6, 0.4, 0.1};
    // B(Cb, Cs) worked out by hand from the handoff's table.
    const std::vector<std::pair<BlendMode, Rgb>> cases{
        {BlendMode::Normal, {0.6, 0.4, 0.1}},
        {BlendMode::Multiply, {0.12, 0.2, 0.08}},
        {BlendMode::Screen, {0.68, 0.7, 0.82}},                // Cb + Cs - Cb Cs
        {BlendMode::Overlay, {0.24, 0.4, 0.64}},               // Cb <= 0.5: 2 Cb Cs, else Screen(Cs, 2 Cb - 1)
        {BlendMode::Difference, {0.4, 0.1, 0.7}},
        {BlendMode::ColorDodge, {0.5, 0.5 / 0.6, 0.8 / 0.9}},  // Cb / (1 - Cs)
        {BlendMode::Hue, {0.672848, 0.432848, 0.072848}},      // SetSat(Cs, 0.6) = (0.6, 0.36, 0), lifted to Lum(Cb)
    };
    for (const auto& [mode, b] : cases) {
        INFO(std::string(toString(mode)));
        const Rgb got = blendFunction(mode, cb, cs);
        for (std::size_t c = 0; c < 3; ++c) {
            CHECK(got[c] == Approx(b[c]).margin(1e-9));
        }
        // Both opaque: the blend function itself.
        checkPixel(blend(onePixel(cb, 1.0), onePixel(cs, 1.0), mode, 1.0), {b[0], b[1], b[2], 1.0});
        // Source alpha 0.5 over an opaque backdrop: 0.5 B + 0.5 Cb; opacity 0.5 is the same.
        const Rgba half{0.5 * b[0] + 0.5 * cb[0], 0.5 * b[1] + 0.5 * cb[1], 0.5 * b[2] + 0.5 * cb[2], 1.0};
        checkPixel(blend(onePixel(cb, 1.0), onePixel(cs, 0.5), mode, 1.0), half);
        checkPixel(blend(onePixel(cb, 1.0), onePixel(cs, 1.0), mode, 0.5), half);
        // Backdrop alpha 0.5 under an opaque source: Cr = 0.5 Cs + 0.5 B, opaque.
        checkPixel(blend(onePixel(cb, 0.5), onePixel(cs, 1.0), mode, 1.0),
                   {0.5 * cs[0] + 0.5 * b[0], 0.5 * cs[1] + 0.5 * b[1], 0.5 * cs[2] + 0.5 * b[2], 1.0});
        // Both 0.5: 0.5 Cr + 0.5 (0.5 Cb), alpha 0.75.
        checkPixel(blend(onePixel(cb, 0.5), onePixel(cs, 0.5), mode, 1.0),
                   {0.25 * (cs[0] + b[0] + cb[0]), 0.25 * (cs[1] + b[1] + cb[1]), 0.25 * (cs[2] + b[2] + cb[2]), 0.75});
    }
}

TEST_CASE("Blend reference: every mode keeps the source-over identities", "[render_reference][blend]") {
    const Image backdrop = goldenBackdrop();
    const Image source = goldenSource();
    const Image clear(8, 2);
    REQUIRE(blendModesInMenuOrder().size() == kBlendModeCount);
    for (const BlendMode mode : blendModesInMenuOrder()) {
        INFO(std::string(toString(mode)));
        // A transparent source (or opacity 0) leaves the backdrop as it is.
        CHECK(compare(blend(backdrop, clear, mode, 1.0), backdrop).maxAbs <= 1e-7);
        CHECK(compare(blend(backdrop, source, mode, 0.0), backdrop).maxAbs <= 1e-7);
        if (mode == BlendMode::Dissolve) {
            continue; // all or nothing: shown pixels are opaque
        }
        // Over a transparent backdrop every mode is Normal: the source times the opacity.
        const Image alone = blend(clear, source, mode, 0.5);
        Image want(8, 2);
        for (std::size_t i = 0; i < want.rgba.size(); ++i) {
            want.rgba[i] = static_cast<float>(0.5 * static_cast<double>(source.rgba[i]));
        }
        CHECK(compare(alone, want).maxAbs <= 1e-6);
        // Results stay finite with alpha in [0, 1].
        const Image both = blend(backdrop, source, mode, 1.0);
        for (std::size_t i = 0; i < both.rgba.size(); ++i) {
            CHECK(std::isfinite(both.rgba[i]));
            if (i % 4 == 3) {
                CHECK(both.rgba[i] >= 0.0f);
                CHECK(both.rgba[i] <= 1.0f);
            }
        }
    }
}

TEST_CASE("Blend reference: all 27 modes on an 8x2 pair match the goldens", "[render_reference][blend][golden]") {
    const Image backdrop = goldenBackdrop();
    const Image source = goldenSource();
    for (const double opacity : {1.0, 0.5}) {
        // One row pair per mode, in menu order.
        Image stacked(8, 2 * static_cast<int>(kBlendModeCount));
        int row = 0;
        for (const BlendMode mode : blendModesInMenuOrder()) {
            const Image result = blend(backdrop, source, mode, opacity, 7, 3);
            for (int y = 0; y < 2; ++y) {
                for (int x = 0; x < 8; ++x) {
                    stacked.set(x, row + y, result.at(x, y));
                }
            }
            row += 2;
        }
        test::checkGolden(opacity == 1.0 ? "blend_modes_opacity100" : "blend_modes_opacity50", stacked);
    }
}

TEST_CASE("Blend reference: Dissolve is deterministic and covers about the opacity", "[render_reference][blend]") {
    // The splitmix64 reference: the first outputs of the generator seeded with 0.
    CHECK(splitmix64(0, 0) == 0xE220A8397B1DCDAFull);
    CHECK(splitmix64(0, 1) == 0x6E789E6AA1B965F4ull);
    const Image backdrop = Image::filled(64, 64, {0.0f, 0.0f, 1.0f, 1.0f});
    const Image source = Image::filled(64, 64, {1.0f, 0.0f, 0.0f, 1.0f});
    const auto shown = [&](const Image& image) {
        int count = 0;
        for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
            const bool red = image.rgba[i] == 1.0f && image.rgba[i + 2] == 0.0f && image.rgba[i + 3] == 1.0f;
            const bool blue = image.rgba[i] == 0.0f && image.rgba[i + 2] == 1.0f && image.rgba[i + 3] == 1.0f;
            CHECK((red || blue)); // all or nothing
            count += red ? 1 : 0;
        }
        return static_cast<double>(count) / static_cast<double>(image.pixelCount());
    };
    const Image a = blend(backdrop, source, BlendMode::Dissolve, 0.5, 42, 0);
    CHECK(a == blend(backdrop, source, BlendMode::Dissolve, 0.5, 42, 0)); // same seed and frame: same pixels
    CHECK(shown(a) == Approx(0.5).margin(0.03));
    CHECK_FALSE(a == blend(backdrop, source, BlendMode::Dissolve, 0.5, 43, 0)); // another clip
    CHECK_FALSE(a == blend(backdrop, source, BlendMode::Dissolve, 0.5, 42, 1)); // another frame
    CHECK(shown(blend(backdrop, source, BlendMode::Dissolve, 0.25, 42, 0)) == Approx(0.25).margin(0.03));
    CHECK(shown(blend(backdrop, source, BlendMode::Dissolve, 0.0, 42, 0)) == 0.0);
    CHECK(shown(blend(backdrop, source, BlendMode::Dissolve, 1.0, 42, 0)) == 1.0);
    // The source's own alpha counts like opacity; shown pixels are opaque.
    const Image halfSource = Image::filled(64, 64, {0.5f, 0.0f, 0.0f, 0.5f});
    CHECK(shown(blend(backdrop, halfSource, BlendMode::Dissolve, 1.0, 42, 0)) == Approx(0.5).margin(0.03));
    // A lower opacity shows a subset of the pixels a higher one shows (the same threshold per pixel).
    const Image quarter = blend(backdrop, source, BlendMode::Dissolve, 0.25, 42, 0);
    for (std::size_t i = 0; i < quarter.rgba.size(); i += 4) {
        if (quarter.rgba[i] == 1.0f) {
            CHECK(a.rgba[i] == 1.0f);
        }
    }
}
