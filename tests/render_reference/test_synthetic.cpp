// Reference pictures of the synthetic media (Synthetic.h) and the transfer curves they use.

#include "ReferenceTestSupport.h"

#include <wabcut/model/Synthetic.h>
#include <wabcut/render/reference/Synthetic.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::render::reference;
using Catch::Approx;

TEST_CASE("Synthetic reference: the transfer curves", "[render_reference][synthetic]") {
    // BT.709: the linear toe below 4.5 b and the power segment, inverse of each other.
    CHECK(toLinear(0.0) == 0.0);
    CHECK(toLinear(1.0) == Approx(1.0).margin(1e-12));
    CHECK(toLinear(0.04) == Approx(0.04 / 4.5).margin(1e-15));
    CHECK(toLinear(0.5) == Approx(0.2597194371).margin(1e-9));
    for (const double v : {0.0, 0.01, 0.05, 0.081, 0.2, 0.5, 0.9, 1.0}) {
        CHECK(toEncoded(toLinear(v)) == Approx(v).margin(1e-12));
    }
    CHECK(toLinear(0.5f) == static_cast<float>(toLinear(0.5)));
    CHECK(toLinear(-1.0) == 0.0); // clamped
    CHECK(toEncoded(2.0) == Approx(1.0).margin(1e-12));
    // sRGB for user colours.
    CHECK(srgbToLinear(0.5) == Approx(0.2140411405).margin(1e-9));
    CHECK(linearToSrgb(srgbToLinear(0.3)) == Approx(0.3).margin(1e-12));
    CHECK(luma({1.0, 1.0, 1.0}) == Approx(1.0).margin(1e-15));
}

TEST_CASE("Synthetic reference: Color Matte Black Video and Transparent Video are exact", "[render_reference][synthetic]") {
    const Rgb color{0.5, 0.25, 1.0};
    const Image matte = colorMatte(4, 3, color);
    REQUIRE(matte.width == 4);
    REQUIRE(matte.height == 3);
    const Pixel want{static_cast<float>(toLinear(0.5)), static_cast<float>(toLinear(0.25)), 1.0f, 1.0f};
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 4; ++x) {
            CHECK(matte.at(x, y) == want);
            CHECK(blackVideo(4, 3).at(x, y) == Pixel{0.0f, 0.0f, 0.0f, 1.0f});
            CHECK(transparentVideo(4, 3).at(x, y) == Pixel{0.0f, 0.0f, 0.0f, 0.0f});
        }
    }
    CHECK(static_cast<double>(want[0]) == Approx(0.2597194371).margin(1e-7));
    // Encoded through the sequence's BT.709 curve, the matte exports exactly its colour.
    CHECK(toEncoded(static_cast<double>(want[1])) == Approx(0.25).margin(1e-6));
    CHECK(colorMatte(0, 3, color).empty());
}

TEST_CASE("Synthetic reference: pictures of synthetic media items", "[render_reference][synthetic]") {
    const SyntheticFormat format{8, 4, kRate30};
    const auto matte = syntheticMediaItem(SyntheticKind::ColorMatte, {{"color.g", 0.25}}, format);
    REQUIRE(matte.has_value());
    // color.r and color.b take their defaults (1).
    CHECK(syntheticPicture(*matte, 8, 4) == colorMatte(8, 4, {1.0, 0.25, 1.0}));
    const auto black = syntheticMediaItem(SyntheticKind::BlackVideo, {}, format);
    REQUIRE(black.has_value());
    CHECK(syntheticPicture(*black, 8, 4) == blackVideo(8, 4));
    const auto clear = syntheticMediaItem(SyntheticKind::TransparentVideo, {}, format);
    REQUIRE(clear.has_value());
    CHECK(syntheticPicture(*clear, 8, 4) == transparentVideo(8, 4));
    const auto adjustment = adjustmentMediaItem(format);
    REQUIRE(adjustment.has_value());
    CHECK(syntheticPicture(*adjustment, 8, 4).empty()); // no picture of its own
    CHECK(syntheticPicture(MediaItem{}, 8, 4).empty());
}
