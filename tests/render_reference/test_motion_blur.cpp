// The CPU reference of Motion Blur (0.7, MotionBlur.h): a picture drawn at several placements and averaged.

#include "ReferenceTestSupport.h"

#include <wabcut/render/reference/MotionBlur.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace wabcut;
using namespace wabcut::render::reference;
using Catch::Approx;

namespace {

Placement2D at(double x, double y) {
    Placement2D p;
    p.e = x;
    p.f = y;
    return p;
}

} // namespace

TEST_CASE("Reference Motion Blur: a square drawn once covers its own pixels and the fraction of an edge pixel", "[render_reference][motionblur]") {
    const Image square = Image::filled(8, 8, {1.0f, 1.0f, 1.0f, 1.0f});
    const Image whole = drawPlaced(square, 32, 16, at(4, 4));
    CHECK(whole.at(4, 4)[3] == Approx(1.0));
    CHECK(whole.at(11, 11)[3] == Approx(1.0));
    CHECK(whole.at(3, 8)[3] == 0.0f);
    CHECK(whole.at(12, 8)[3] == 0.0f);
    // Half a pixel to the right: the left edge column is half covered, and so is the column past the right edge.
    const Image half = drawPlaced(square, 32, 16, at(4.5, 4));
    CHECK(half.at(4, 6)[3] == Approx(0.5));
    CHECK(half.at(5, 6)[3] == Approx(1.0));
    CHECK(half.at(12, 6)[3] == Approx(0.5));
    // A quarter turn about the picture's corner lands on the other side of it.
    Placement2D turned;
    turned.a = 0.0;
    turned.b = 1.0;
    turned.c = -1.0;
    turned.d = 0.0;
    turned.e = 20.0;
    turned.f = 2.0;
    const Image rotated = drawPlaced(square, 32, 16, turned);
    CHECK(rotated.at(15, 4)[3] == Approx(1.0));  // x' = 20 - y, y' = 2 + x: the square covers x in (12, 20], y in [2, 10)
    CHECK(rotated.at(21, 4)[3] == 0.0f);
    // A placement with no area, an empty picture or canvas draw nothing.
    CHECK(drawPlaced(square, 32, 16, Placement2D{0, 0, 0, 0, 5, 5}).at(5, 5)[3] == 0.0f);
    CHECK(drawPlaced(Image{}, 32, 16, at(0, 0)).at(0, 0)[3] == 0.0f);
    CHECK(drawPlaced(square, 0, 0, at(0, 0)).empty());
}

TEST_CASE("Reference Motion Blur: the average smears by the share of the instants that cover each pixel", "[render_reference][motionblur]") {
    const Image square = Image::filled(8, 8, {1.0f, 1.0f, 1.0f, 1.0f});
    // Five instants, the square moving 4 pixels each: it sits at x = 0, 4, 8, 12, 16 (width 8) on a 32 x 16 canvas.
    std::vector<Placement2D> instants;
    for (int i = 0; i < 5; ++i)
        instants.push_back(at(4.0 * i, 4));
    const Image blurred = motionBlurred(square, 32, 16, instants);
    CHECK(blurred.at(2, 8)[0] == Approx(1.0 / 5.0));   // only the first instant
    CHECK(blurred.at(10, 8)[0] == Approx(2.0 / 5.0));  // the second and the third
    CHECK(blurred.at(14, 8)[0] == Approx(2.0 / 5.0));  // the third and the fourth
    CHECK(blurred.at(22, 8)[0] == Approx(1.0 / 5.0));  // only the last
    CHECK(blurred.at(26, 8)[0] == 0.0f);               // past the smear
    CHECK(blurred.at(10, 2)[0] == 0.0f);               // above the square's rows
    // The smear is as long as the path plus the square: 16 + 8 pixels.
    int covered = 0;
    for (int x = 0; x < 32; ++x)
        covered += blurred.at(x, 8)[3] > 0.0f ? 1 : 0;
    CHECK(covered == 24);
    // Colour and alpha average the same way (premultiplied), and the sum of the picture is kept: mass is not lost.
    double mass = 0.0;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 32; ++x)
            mass += blurred.at(x, y)[3];
    }
    CHECK(mass == Approx(64.0));
    // One placement is the plain draw; none is a transparent canvas.
    CHECK(motionBlurred(square, 32, 16, std::vector<Placement2D>{at(4, 4)}) == drawPlaced(square, 32, 16, at(4, 4)));
    CHECK(motionBlurred(square, 32, 16, std::vector<Placement2D>{}).at(5, 5)[3] == 0.0f);
}
