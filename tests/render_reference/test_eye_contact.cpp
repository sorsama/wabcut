// AI Eye Contact (card 72) on the CPU reference (render/reference/LibraryDistort.cpp "EyeContact", the twin of shaders/fx_distort.frag): the
// iris moves by the baked shift inside the opening between the lids, the sclera on its sides fills what it left, and nothing outside the
// opening changes by a single bit. The GPU shader is held to this reference by the render suite's library test
// (tests/render/test_library_effects.cpp). The eye is drawn analytically (a lens-shaped opening with shaded sclera, an iris with a pupil and
// a catch-light, noisy skin around it) so the tests know where everything is, level and rolled.

#include <wabcut/render/reference/LibraryEffects.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>

using namespace wabcut::render::reference;
using E = LibraryEffect;
using Catch::Approx;

namespace {

constexpr int kW = 256;
constexpr int kH = 128;
constexpr double kPi = 3.14159265358979;

double noise(int x, int y, int salt = 0) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 73856093u ^ static_cast<std::uint32_t>(y) * 19349663u ^ static_cast<std::uint32_t>(salt) * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return static_cast<double>(h & 0xFFFFu) / 32768.0 - 1.0;
}

/// One eye: the middle of its corner line, the half width, the lids' heights above / below that line, the iris radius and its place in the eye's
/// own frame (x along the corner line, y down), the roll of the corner line in degrees.
struct EyeSpec {
    double cx = 128.0;
    double cy = 64.0;
    double a = 40.0;
    double up = 18.0;
    double lo = 16.0;
    double r = 9.0;
    double irisX = 0.0;
    double irisY = 3.0;
    double rollDegrees = 0.0;
};

struct Local {
    double x;
    double y;
};
Local localOf(const EyeSpec& e, double px, double py) {
    const double t = e.rollDegrees * kPi / 180.0;
    const double dx = px - e.cx;
    const double dy = py - e.cy;
    return {dx * std::cos(t) + dy * std::sin(t), -dx * std::sin(t) + dy * std::cos(t)};
}

bool insideOpening(const EyeSpec& e, const Local& l) {
    const double u = l.x / e.a;
    const double k = 1.0 - u * u;
    return std::abs(u) < 1.0 && l.y > -e.up * k && l.y < e.lo * k;
}

Rgb encodedColour(const EyeSpec& e, int x, int y) {
    const Local l = localOf(e, x + 0.5, y + 0.5);
    const auto jitter = [&](Rgb c, double amp) {
        return Rgb{c[0] + amp * noise(x, y, 1), c[1] + amp * noise(x, y, 2), c[2] + amp * noise(x, y, 3)};
    };
    if (!insideOpening(e, l)) {
        return jitter({0.80, 0.62, 0.52}, 0.02); // skin, with a grain (so "nothing changed" means something)
    }
    const double dIris = std::hypot(l.x - e.irisX, l.y - e.irisY);
    if (std::hypot(l.x - e.irisX + 3.0, l.y - e.irisY + 3.0) < 1.4) {
        return {0.98, 0.98, 0.98}; // the catch-light
    }
    if (dIris < 3.5) {
        return {0.04, 0.03, 0.03}; // the pupil
    }
    if (dIris < e.r) {
        return jitter({0.30, 0.18, 0.10}, 0.01);
    }
    const double u = l.x / e.a;
    const double k = 1.0 - u * u;
    const double shade = 0.78 + 0.22 * (l.y + e.up * k) / ((e.up + e.lo) * k); // darker under the upper lid
    return jitter({0.93 * shade, 0.91 * shade, 0.89 * shade}, 0.008);
}

Image pictureOf(const EyeSpec& e, double alpha = 1.0) {
    Image image(kW, kH);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const Rgb c = encodedColour(e, x, y);
            image.set(x, y, {static_cast<float>(toLinear(c[0]) * alpha), static_cast<float>(toLinear(c[1]) * alpha),
                             static_cast<float>(toLinear(c[2]) * alpha), static_cast<float>(alpha)});
        }
    }
    return image;
}

double lumaAt(const Image& image, int x, int y) {
    const Rgba c = image.atD(x, y);
    const double a = std::max(c[3], 1e-9);
    return 0.2126 * toEncoded(c[0] / a) + 0.7152 * toEncoded(c[1] / a) + 0.0722 * toEncoded(c[2] / a);
}

/// The compositor's parameters for one eye (model/EyeContact.h layout): the shift is in the eye's frame, in pixels.
LibraryParams eyeParams(const EyeSpec& e, double shiftX, double shiftY, double view = 0.0, int count = 1) {
    LibraryParams q{};
    q[5] = view;
    q[6] = count;
    q[8] = e.cx / kW;
    q[9] = e.cy / kH;
    q[10] = e.rollDegrees * kPi / 180.0;
    q[11] = e.a / kW;
    q[12] = e.up / kW;
    q[13] = e.lo / kW;
    q[14] = e.irisX / kW;
    q[15] = e.irisY / kW;
    q[16] = e.r / kW;
    q[17] = shiftX / kW;
    q[18] = shiftY / kW;
    return q;
}

struct Centroid {
    double x = 0.0;
    double y = 0.0;
    int count = 0;
};
/// The centre of the dark pixels (the iris and pupil) of the eye's window.
Centroid darkCentroid(const Image& image, const EyeSpec& e) {
    Centroid c;
    for (int y = static_cast<int>(e.cy - e.a); y < static_cast<int>(e.cy + e.a); ++y) {
        for (int x = static_cast<int>(e.cx - e.a); x < static_cast<int>(e.cx + e.a); ++x) {
            if (x < 0 || y < 0 || x >= kW || y >= kH) {
                continue;
            }
            const Local l = localOf(e, x + 0.5, y + 0.5);
            if (insideOpening(e, l) && lumaAt(image, x, y) < 0.40) {
                c.x += x + 0.5;
                c.y += y + 0.5;
                ++c.count;
            }
        }
    }
    if (c.count > 0) {
        c.x /= c.count;
        c.y /= c.count;
    }
    return c;
}

/// Pixels farther than `margin` pixels outside the opening (in the eye's own frame): they must not change.
/// The pictures side by side, enlarged three times, for people to look at: test-output/render_reference/<name>.ppm.
void savePictures(const std::string& name, const std::vector<const Image*>& pictures) {
    const std::filesystem::path dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "render_reference";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir / (name + ".ppm"), std::ios::binary);
    const int zoom = 3;
    const int cropX = 48;
    const int cropW = 160;
    const int cropY = 24;
    const int cropH = 80;
    out << "P6\n" << cropW * zoom * static_cast<int>(pictures.size()) << " " << cropH * zoom << "\n255\n";
    for (int y = 0; y < cropH * zoom; ++y) {
        for (const Image* image : pictures) {
            for (int x = 0; x < cropW * zoom; ++x) {
                const Rgba c = image->atD(cropX + x / zoom, cropY + y / zoom);
                const double a = std::max(c[3], 1e-9);
                for (int k = 0; k < 3; ++k) {
                    out.put(static_cast<char>(static_cast<unsigned char>(std::clamp(toEncoded(c[k] / a), 0.0, 1.0) * 255.0 + 0.5)));
                }
            }
        }
    }
}

bool outsideOpening(const EyeSpec& e, int x, int y, double margin) {
    const Local l = localOf(e, x + 0.5, y + 0.5);
    const double u = l.x / e.a;
    if (std::abs(u) >= 1.0 + margin / e.a) {
        return true;
    }
    const double k = std::max(0.0, 1.0 - u * u);
    return l.y < -e.up * k - margin || l.y > e.lo * k + margin;
}

} // namespace

TEST_CASE("Eye Contact with no eyes or no shift returns the picture bit for bit", "[eyecontact][reference]") {
    const EyeSpec eye;
    const Image in = pictureOf(eye);
    // No eyes baked: the evaluator hands the effect nothing to do.
    CHECK(libraryEffect(in, E::EyeContact, eyeParams(eye, 0.0, -5.0, 0.0, 0)) == in);
    // An eye with a shift of zero changes nothing either.
    const Image still = libraryEffect(in, E::EyeContact, eyeParams(eye, 0.0, 0.0));
    CHECK(still == in);
    // An eye of no width (a broken measurement) is skipped, and so is a shift that is not a number: the picture comes back as it was.
    LibraryParams q = eyeParams(eye, 0.0, -5.0);
    q[11] = 0.0;
    CHECK(libraryEffect(in, E::EyeContact, q) == in);
    q = eyeParams(eye, std::numeric_limits<double>::quiet_NaN(), -5.0);
    CHECK(libraryEffect(in, E::EyeContact, q) == in);
    // A shift under a twentieth of a pixel is nothing.
    CHECK(libraryEffect(in, E::EyeContact, eyeParams(eye, 0.0, 0.03)) == in);
}

TEST_CASE("Eye Contact moves the iris up by the shift, fills the sclera it left and leaves everything outside the opening untouched",
          "[eyecontact][reference]") {
    const EyeSpec eye; // the iris 5 px below the middle of the opening (the middle is 2 px above the corner line)
    const Image in = pictureOf(eye);
    const Centroid before = darkCentroid(in, eye);
    REQUIRE(before.count > 100);
    const Image out = libraryEffect(in, E::EyeContact, eyeParams(eye, 0.0, -5.0));
    savePictures("eye_contact_up", {&in, &out});
    const Centroid after = darkCentroid(out, eye);
    REQUIRE(after.count > 100);
    CHECK(after.x == Approx(before.x).margin(0.35));
    CHECK(after.y == Approx(before.y - 5.0).margin(0.35));
    CHECK(after.count == Approx(static_cast<double>(before.count)).epsilon(0.06)); // the iris keeps its size
    // The new iris: the pupil is where the iris centre now is.
    CHECK(lumaAt(out, 128, 64 + 3 - 5) < 0.12);
    CHECK(lumaAt(in, 128, 64 + 3 - 5) > 0.12); // before, the pupil was 5 px lower: here was the iris, not the pupil
    // Where the iris was and is no more, the sclera is back: bright, like the sclera in the same row beside the iris.
    const int vacatedY = 64 + 3 + 8; // the old iris's lower rim, outside the new disc and well inside the opening
    CHECK(lumaAt(in, 128, vacatedY) < 0.45);
    CHECK(lumaAt(out, 128, vacatedY) > 0.75);
    // It looks like the sclera of that place (the picture's own shading under the upper lid, without the iris): within a tenth.
    const Local l = localOf(eye, 128.5, vacatedY + 0.5);
    const double k = 1.0 - (l.x / eye.a) * (l.x / eye.a);
    const double shade = 0.78 + 0.22 * (l.y + eye.up * k) / ((eye.up + eye.lo) * k);
    CHECK(lumaAt(out, 128, vacatedY) == Approx(0.2126 * 0.93 * shade + 0.7152 * 0.91 * shade + 0.0722 * 0.89 * shade).margin(0.1));
    // Nothing outside the opening (and a few pixels round it) changes: the lids, the skin, the grain.
    int checked = 0;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            if (outsideOpening(eye, x, y, 4.0)) {
                ++checked;
                REQUIRE(out.at(x, y) == in.at(x, y));
            }
        }
    }
    CHECK(checked > kW * kH * 8 / 10);
    // The change is inside the eye window only: the pixels that changed all lie in the opening or within its soft edge.
    int changed = 0;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            if (out.at(x, y) != in.at(x, y)) {
                ++changed;
                REQUIRE_FALSE(outsideOpening(eye, x, y, 4.0));
            }
        }
    }
    CHECK(changed > 100);
}

TEST_CASE("Eye Contact follows the eye's roll: the shift is in the eye's own frame", "[eyecontact][reference]") {
    EyeSpec eye;
    eye.rollDegrees = 10.0;
    const Image in = pictureOf(eye);
    const Centroid before = darkCentroid(in, eye);
    REQUIRE(before.count > 100);
    // "Up" in the eye's frame is (+sin, -cos) of the roll in the picture.
    const double t = 10.0 * kPi / 180.0;
    const Image out = libraryEffect(in, E::EyeContact, eyeParams(eye, 0.0, -5.0));
    const Centroid after = darkCentroid(out, eye);
    CHECK(after.x == Approx(before.x + 5.0 * std::sin(t)).margin(0.4));
    CHECK(after.y == Approx(before.y - 5.0 * std::cos(t)).margin(0.4));
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            if (outsideOpening(eye, x, y, 4.0)) {
                REQUIRE(out.at(x, y) == in.at(x, y));
            }
        }
    }
    // A sideways shift in the eye's frame is along the corner line.
    const Image sideways = libraryEffect(in, E::EyeContact, eyeParams(eye, 4.0, 0.0));
    const Centroid moved = darkCentroid(sideways, eye);
    CHECK(moved.x == Approx(before.x + 4.0 * std::cos(t)).margin(0.4));
    CHECK(moved.y == Approx(before.y + 4.0 * std::sin(t)).margin(0.4));
}

TEST_CASE("Eye Contact corrects both eyes of a face, each by its own shift", "[eyecontact][reference]") {
    EyeSpec left;
    left.cx = 70.0;
    left.irisY = 4.0;
    EyeSpec right;
    right.cx = 186.0;
    right.irisY = 6.0;
    right.rollDegrees = 0.0;
    // One picture with both eyes (the second drawn where the first is not).
    Image in(kW, kH);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const Local lr = localOf(right, x + 0.5, y + 0.5);
            const Rgb c = insideOpening(right, lr) || std::abs(x + 0.5 - right.cx) < right.a + 4 ? encodedColour(right, x, y) : encodedColour(left, x, y);
            in.set(x, y, {static_cast<float>(toLinear(c[0])), static_cast<float>(toLinear(c[1])), static_cast<float>(toLinear(c[2])), 1.0f});
        }
    }
    LibraryParams q = eyeParams(left, 0.0, -4.0, 0.0, 2);
    const LibraryParams second = eyeParams(right, 0.0, -6.0);
    for (std::size_t i = 0; i < 11; ++i) {
        q[19 + i] = second[8 + i];
    }
    const Centroid leftBefore = darkCentroid(in, left);
    const Centroid rightBefore = darkCentroid(in, right);
    REQUIRE(leftBefore.count > 100);
    REQUIRE(rightBefore.count > 100);
    const Image out = libraryEffect(in, E::EyeContact, q);
    CHECK(darkCentroid(out, left).y == Approx(leftBefore.y - 4.0).margin(0.4));
    CHECK(darkCentroid(out, right).y == Approx(rightBefore.y - 6.0).margin(0.4));
}

TEST_CASE("Eye Contact keeps the alpha of a layer that is partly transparent", "[eyecontact][reference]") {
    const EyeSpec eye;
    const Image in = pictureOf(eye, 0.6);
    const Image out = libraryEffect(in, E::EyeContact, eyeParams(eye, 0.0, -5.0));
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            REQUIRE(out.alpha(x, y) == Approx(0.6f).margin(1e-5));
        }
    }
    // The iris still moved: the picture is a mix of premultiplied pixels, so what it shows is the same as at full opacity.
    CHECK(darkCentroid(out, eye).y == Approx(darkCentroid(in, eye).y - 5.0).margin(0.4));
}

TEST_CASE("Eye Contact's Eye Map view shows the opening, the new iris and what the iris left", "[eyecontact][reference]") {
    const EyeSpec eye;
    const Image in = pictureOf(eye);
    const Image map = libraryEffect(in, E::EyeContact, eyeParams(eye, 0.0, -5.0, 1.0));
    const auto at = [&](int x, int y) { return map.atD(x, y); };
    // The middle of the new iris: iris (red) and opening (green) full, nothing vacated.
    CHECK(at(128, 64 + 3 - 5)[0] > 0.95);
    CHECK(at(128, 64 + 3 - 5)[1] > 0.95);
    CHECK(at(128, 64 + 3 - 5)[2] < 0.05);
    // The old iris's lower rim: in the opening, vacated, not iris.
    CHECK(at(128, 64 + 3 + 8)[0] < 0.05);
    CHECK(at(128, 64 + 3 + 8)[1] > 0.95);
    CHECK(at(128, 64 + 3 + 8)[2] > 0.9);
    // The sclera at the eye's end: opening only. The skin: nothing at all. Everything opaque.
    CHECK(at(128 - 30, 64)[0] < 0.05);
    CHECK(at(128 - 30, 64)[1] > 0.9);
    CHECK(at(128, 10)[0] == 0.0);
    CHECK(at(128, 10)[1] == 0.0);
    CHECK(at(128, 10)[2] == 0.0);
    CHECK(at(128, 10)[3] == 1.0);
    // No eyes: a black map.
    const Image none = libraryEffect(in, E::EyeContact, eyeParams(eye, 0.0, -5.0, 1.0, 0));
    CHECK(none.atD(128, 64)[1] == 0.0);
}

TEST_CASE("Eye Contact on an iris towards the corner keeps the lids where they are and never lets skin into the opening", "[eyecontact][reference]") {
    // An iris 17 % of the eye's width to the side (inside the effect's default Side Limit) moved back to the middle: the sclera fills from the
    // side that has any, the skin never enters the opening and the lids stay.
    EyeSpec eye;
    eye.irisX = 14.0;
    eye.irisY = -2.0;
    const Image in = pictureOf(eye);
    const Image out = libraryEffect(in, E::EyeContact, eyeParams(eye, -14.0, 0.0));
    const Centroid after = darkCentroid(out, eye);
    const Centroid before = darkCentroid(in, eye);
    REQUIRE(after.count > 80);
    CHECK(after.x == Approx(before.x - 14.0).margin(0.6));
    CHECK(after.y == Approx(before.y).margin(0.5));
    // No skin colour (bright and much redder than blue) appears inside the opening: its pixels are sclera, iris or pupil.
    int skinInside = 0;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const Local l = localOf(eye, x + 0.5, y + 0.5);
            const double u = l.x / eye.a;
            const double k = 1.0 - u * u;
            if (std::abs(u) < 0.85 && l.y > -eye.up * k + 3.0 && l.y < eye.lo * k - 3.0) {
                const Rgba c = out.atD(x, y);
                const double r = toEncoded(c[0]);
                const double b = toEncoded(c[2]);
                const double luma = 0.2126 * r + 0.7152 * toEncoded(c[1]) + 0.0722 * b;
                skinInside += luma > 0.5 && r - b > 0.18 ? 1 : 0; // skin is bright and 0.28 redder than blue; sclera is 0.04 redder; the iris is dark
            }
        }
    }
    CHECK(skinInside < 12);
    // The lids and everything outside the opening are as they were.
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            if (outsideOpening(eye, x, y, 4.0)) {
                REQUIRE(out.at(x, y) == in.at(x, y));
            }
        }
    }
}
