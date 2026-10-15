// Reference Ultra Key (UltraKey.h) on a synthetic green-screen card: a realistic chroma green (not pure 0, 1, 0) with a
// square that is mid grey on its left half and a skin tone on its right half, its edge a 6-pixel linear-light ramp.

#include "ReferenceTestSupport.h"

#include <wabcut/model/Effects.h>
#include <wabcut/render/reference/UltraKey.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

using namespace wabcut;
using namespace wabcut::render::reference;
using Catch::Approx;

namespace {

const Rgb kScreen{0.10, 0.60, 0.20}; // encoded green screen
const Rgb kGrey{0.5, 0.5, 0.5};
const Rgb kSkin{0.85, 0.65, 0.55};

/// 32 x 32: the square [8, 24) x [8, 24) mixed over the screen by c = clamp((inside + 3) / 6, 0, 1), inside = the pixel
/// centre's signed distance into the square (pixels).
Image greenScreenCard() {
    Image out(32, 32);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const double px = x + 0.5;
            const double py = y + 0.5;
            const double inside = std::min({px - 8.0, 24.0 - px, py - 8.0, 24.0 - py});
            const double c = std::clamp((inside + 3.0) / 6.0, 0.0, 1.0);
            const Rgb& fg = x < 16 ? kGrey : kSkin;
            Rgba p{0.0, 0.0, 0.0, 1.0};
            for (std::size_t i = 0; i < 3; ++i) {
                p[i] = c * toLinear(fg[i]) + (1.0 - c) * toLinear(kScreen[i]);
            }
            out.setD(x, y, p);
        }
    }
    return out;
}

UltraKeyParams cardParams() {
    UltraKeyParams p;
    p.keyColor = kScreen;
    return p;
}

/// The encoded straight colour of a pixel.
Rgb encodedAt(const Image& image, int x, int y) {
    const Rgba p = image.atD(x, y);
    return {toEncoded(p[0] / p[3]), toEncoded(p[1] / p[3]), toEncoded(p[2] / p[3])};
}

double greenExcess(const Rgb& c) { return c[1] - 0.5 * (c[0] + c[2]); }

} // namespace

TEST_CASE("Ultra Key reference: parameters follow the effect's keys and defaults", "[render_reference][key]") {
    EffectInstance e;
    e.effect = std::string(kUltraKeyEffect);
    CHECK(ultraKeyParams(e, {}) == UltraKeyParams{}); // every descriptor default is the struct's default
    const EffectDescriptor* d = EffectRegistry::builtin().find(kUltraKeyEffect);
    REQUIRE(d);
    CHECK(d->params.size() == 20); // output, keyColor.r/g/b and the 16 sliders: all in UltraKeyParams
    e.set("output", 2.0);
    e.set("keyColor.b", 0.75);
    e.set("tolerance", 62.0);
    e.set("luma", 80.0);
    const UltraKeyParams p = ultraKeyParams(e, {});
    CHECK(p.output == UltraKeyOutput::ColorChannel);
    CHECK(p.keyColor[2] == 0.75);
    CHECK(p.tolerance == 62.0);
    CHECK(p.luma == 80.0);
    CHECK(p.pedestal == 10.0);
}

TEST_CASE("Ultra Key reference: Y'CbCr and the distance to the key", "[render_reference][key]") {
    const YCbCr g = toYCbCr({0.0, 1.0, 0.0});
    CHECK(g.y == Approx(0.7152));
    CHECK(g.cb == Approx(-0.7152 / 1.8556));
    CHECK(g.cr == Approx(-0.7152 / 1.5748));
    const Rgb back = fromYCbCr(toYCbCr(kSkin));
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(back[i] == Approx(kSkin[i]).margin(1e-12));
    }
    CHECK(keyDistance(kScreen, kScreen).d == Approx(0.0).margin(1e-12));
    CHECK(keyDistance(kGrey, kScreen).d == Approx(1.0).margin(1e-12)); // neutral grey is 1 by definition
    CHECK(keyDistance(kGrey, kScreen).dl == Approx(0.5 - luma(kScreen)));
}

TEST_CASE("Ultra Key reference: a green-screen card keys out with soft edges", "[render_reference][key]") {
    const Image card = greenScreenCard();
    const Image keyed = ultraKey(card, cardParams());
    REQUIRE(keyed.width == 32);
    REQUIRE(keyed.height == 32);
    // The screen goes, the grey and skin-tone foreground stay.
    for (const auto& [x, y] : {std::pair{1, 1}, std::pair{30, 2}, std::pair{16, 30}, std::pair{2, 16}}) {
        CHECK(keyed.alpha(x, y) < 0.02f);
    }
    for (const auto& [x, y] : {std::pair{12, 15}, std::pair{13, 20}, std::pair{19, 15}, std::pair{20, 12}}) {
        CHECK(keyed.alpha(x, y) > 0.98f);
    }
    // The foreground's colour is untouched (no spill there, identity colour correction): premultiplied by alpha 1.
    const Pixel grey = keyed.at(12, 15);
    CHECK(grey[0] == Approx(toLinear(0.5)).margin(1e-5));
    CHECK(grey[1] == Approx(toLinear(0.5)).margin(1e-5));
    // The edge on the grey side (c = 0.25) is partly transparent: a soft edge.
    const float edge = keyed.alpha(6, 15);
    CHECK(edge > 0.02f);
    CHECK(edge < 0.98f);
    // Alpha only falls towards the screen along the row.
    for (int x = 1; x < 12; ++x) {
        CHECK(keyed.alpha(x, 15) >= keyed.alpha(x - 1, 15));
    }
    test::checkGolden("ultra_key_composite", keyed);
}

TEST_CASE("Ultra Key reference: spill is reduced on an edge pixel", "[render_reference][key]") {
    const Image card = greenScreenCard();
    // The default Range reaches the visible edge: suppression is full up to where the matte rises and fades past it.
    UltraKeyParams p = cardParams();
    p.output = UltraKeyOutput::ColorChannel;
    const Image colour = ultraKey(card, p);
    const Rgb before = encodedAt(card, 6, 15);
    const Rgb after = encodedAt(colour, 6, 15);
    INFO("edge before " << before[0] << " " << before[1] << " " << before[2] << ", after " << after[0] << " " << after[1]
                        << " " << after[2]);
    CHECK(colour.alpha(6, 15) == 1.0f); // the colour channel is opaque
    CHECK(greenExcess(before) > 0.05);
    CHECK(greenExcess(after) < 0.9 * greenExcess(before));
    // Nothing to remove on the foreground: its colour stays.
    const Rgb fg = encodedAt(colour, 12, 15);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(fg[i] == Approx(0.5).margin(1e-5));
    }
    // No spill setting: the edge keeps its colour.
    p.spill = 0.0;
    const Rgb untouched = encodedAt(ultraKey(card, p), 6, 15);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(untouched[i] == Approx(before[i]).margin(1e-5));
    }
}

TEST_CASE("Ultra Key reference: the alpha and colour channel outputs", "[render_reference][key]") {
    const Image card = greenScreenCard();
    UltraKeyParams p = cardParams();
    const Image composite = ultraKey(card, p);
    p.output = UltraKeyOutput::AlphaChannel;
    const Image matte = ultraKey(card, p);
    p.output = UltraKeyOutput::ColorChannel;
    const Image colour = ultraKey(card, p);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const Pixel m = matte.at(x, y);
            CHECK(m[3] == 1.0f);
            CHECK(m[0] == m[1]);
            CHECK(m[1] == m[2]);
            // The matte's grey is the composite's alpha in the sequence encoding.
            CHECK(static_cast<double>(m[0]) == Approx(toLinear(static_cast<double>(composite.alpha(x, y)))).margin(1e-6));
            CHECK(colour.alpha(x, y) == 1.0f);
        }
    }
    CHECK(matte.at(1, 1)[0] < 1e-6f);
    CHECK(matte.at(12, 15)[0] > 0.97f);
    // The colour channel shows the screen too (it is opaque).
    const Rgb screen = encodedAt(colour, 1, 1);
    CHECK(screen[1] > screen[0]);
    test::checkGolden("ultra_key_alpha", matte);
}

TEST_CASE("Ultra Key reference: matte cleanup and colour correction", "[render_reference][key]") {
    // Choke and soften are fractions of the frame width: 100 % of choke is 0.5 % of it, 0.16 px on the 32 px card.
    UltraKeyParams p = cardParams();
    p.choke = 100.0;
    CHECK(ultraKey(greenScreenCard(), p) == ultraKey(greenScreenCard(), cardParams())); // a disc under 1 px: no change
    // A 400 x 4 strip, screen left of x = 200 and grey from there: 100 % is 2 px.
    Image strip(400, 4);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 400; ++x) {
            strip.set(x, y, test::fromEncoded(x < 200 ? kScreen : kGrey, 1.0));
        }
    }
    const Image plain = ultraKey(strip, cardParams());
    CHECK(plain.alpha(199, 1) < 1e-6f);
    CHECK(plain.alpha(200, 1) == 1.0f);
    p = cardParams();
    p.choke = 100.0; // erode by 2 px
    const Image choked = ultraKey(strip, p);
    CHECK(choked.alpha(200, 1) < 1e-6f);
    CHECK(choked.alpha(201, 1) < 1e-6f);
    CHECK(choked.alpha(202, 1) == 1.0f);
    p = cardParams();
    p.soften = 100.0; // sigma 2 px
    const Image softened = ultraKey(strip, p);
    CHECK(softened.alpha(199, 1) > 0.05f);
    CHECK(softened.alpha(200, 1) < 0.95f);
    CHECK(softened.alpha(197, 1) < softened.alpha(199, 1));
    CHECK(softened.alpha(150, 1) == Approx(0.0).margin(1e-6));
    // Contrast about the mid point steepens that ramp.
    p.contrast = 100.0;
    const Image steeper = ultraKey(strip, p);
    CHECK(steeper.alpha(198, 1) <= softened.alpha(198, 1));
    CHECK(steeper.alpha(201, 1) >= softened.alpha(201, 1));
    // Contrast on the card: the soft edge moves away from 0.5.
    p = cardParams();
    p.contrast = 100.0;
    const double soft = static_cast<double>(ultraKey(greenScreenCard(), cardParams()).alpha(6, 15));
    const double hardEdge = static_cast<double>(ultraKey(greenScreenCard(), p).alpha(6, 15));
    CHECK(std::abs(hardEdge - 0.5) >= std::abs(soft - 0.5));
    // Colour correction: luminance 50 halves Y', hue 180 turns the chroma around, saturation 0 makes grey.
    p = cardParams();
    p.luminance = 50.0;
    p.hue = 180.0;
    p.saturation = 0.0;
    const Rgb c = ultraKeyColorAt(kSkin, p);
    const double y = luma(kSkin) * 0.5;
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(c[i] == Approx(y).margin(1e-9));
    }
    p.saturation = 100.0;
    const YCbCr turned = toYCbCr(ultraKeyColorAt(kSkin, p));
    const YCbCr original = toYCbCr(kSkin);
    CHECK(turned.cb == Approx(-original.cb).margin(1e-9));
    CHECK(turned.cr == Approx(-original.cr).margin(1e-9));
}

TEST_CASE("Ultra Key reference: shadows clear the background but not a dark foreground", "[render_reference][key]") {
    UltraKeyParams p;
    p.keyColor = {0.0, 1.0, 0.0};
    // Dark foregrounds against the default key stay opaque whatever the Shadow slider says.
    for (const double v : {0.5, 0.25, 0.1}) {
        INFO("grey " << v);
        p.shadow = 100.0;
        CHECK(ultraKeyMatteAt({v, v, v}, p) > 0.98);
    }
    // A shadow on the screen (darker green, still in the key) goes further with Shadow up.
    const Rgb shadowed{0.05, 0.55, 0.1};
    p.shadow = 0.0;
    const double without = ultraKeyMatteAt(shadowed, p);
    p.shadow = 100.0;
    CHECK(ultraKeyMatteAt(shadowed, p) <= without);
}
