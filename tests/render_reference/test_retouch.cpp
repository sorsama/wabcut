// Skin Smoothing (card 100) and AI Face Retouch (card 132) on the CPU reference (render/reference/LibraryRetouch.cpp): what the
// effects change and, as much, what they leave alone. The GPU shader is held to this reference by the render suite's library
// test (tests/render/test_library_effects.cpp). Pictures are built from encoded (sequence encoding) colours; the face map of
// Face Retouch is the effect's second picture (linear red = skin, green = mouth, blue = eyes coverage).

#include <wabcut/render/reference/LibraryEffects.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>

using namespace wabcut::render::reference;
using E = LibraryEffect;

namespace {

constexpr int kW = 96;
constexpr int kH = 64;

/// A deterministic value in [-1, 1] for a pixel and a salt.
double noise(int x, int y, int salt = 0) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 73856093u ^ static_cast<std::uint32_t>(y) * 19349663u ^ static_cast<std::uint32_t>(salt) * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return static_cast<double>(h & 0xFFFFu) / 32768.0 - 1.0;
}

Pixel pixelOf(const Rgb& encoded, double alpha = 1.0) {
    return {static_cast<float>(toLinear(encoded[0]) * alpha), static_cast<float>(toLinear(encoded[1]) * alpha),
            static_cast<float>(toLinear(encoded[2]) * alpha), static_cast<float>(alpha)};
}

Rgb encodedAt(const Image& image, int x, int y) {
    const Rgba c = image.atD(x, y);
    const double a = c[3];
    return a > 0.0 ? Rgb{toEncoded(c[0] / a), toEncoded(c[1] / a), toEncoded(c[2] / a)} : Rgb{};
}

double lumaOf(const Rgb& c) { return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]; }
double satOf(const Rgb& c) {
    const double mx = std::max({c[0], c[1], c[2]});
    return mx > 0.0 ? (mx - std::min({c[0], c[1], c[2]})) / mx : 0.0;
}

using Colour = std::function<Rgb(int, int)>;

Image pictureOf(const Colour& colour, int w = kW, int h = kH) {
    Image image(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            image.set(x, y, pixelOf(colour(x, y)));
        }
    }
    return image;
}

constexpr Rgb kSkin{0.80, 0.62, 0.52};
constexpr Rgb kBlue{0.20, 0.35, 0.80};

Rgb jittered(const Rgb& base, int x, int y, double amplitude) {
    return {base[0] + amplitude * noise(x, y, 1), base[1] + amplitude * noise(x, y, 2), base[2] + amplitude * noise(x, y, 3)};
}

LibraryParams skinParams(double amount, double detail, double size, double tone = 123.0, double range = 50.0, double softness = 50.0,
                         double view = 0.0) {
    LibraryParams q{};
    q[0] = amount;
    q[1] = detail;
    q[2] = size;
    q[3] = tone;
    q[4] = range;
    q[5] = softness;
    q[6] = view;
    return q;
}

LibraryParams faceParams(double smoothing, double detail, double size, double blemish, double blemishSize, double teeth, double eyes,
                         double view = 0.0) {
    LibraryParams q{};
    q[0] = smoothing;
    q[1] = detail;
    q[2] = size;
    q[3] = blemish;
    q[4] = blemishSize;
    q[5] = teeth;
    q[6] = eyes;
    q[7] = view;
    return q;
}

/// A face map: linear coverage skin, mouth, eyes from functions of the pixel.
Image mapOf(const std::function<double(int, int)>& skin, const std::function<double(int, int)>& mouth = {},
            const std::function<double(int, int)>& eyes = {}, int w = kW, int h = kH) {
    Image map(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            map.set(x, y, {static_cast<float>(skin ? skin(x, y) : 0.0), static_cast<float>(mouth ? mouth(x, y) : 0.0),
                           static_cast<float>(eyes ? eyes(x, y) : 0.0), 1.0f});
        }
    }
    return map;
}

/// The standard deviation of a region's encoded luma.
double spread(const Image& image, int x0, int y0, int x1, int y1) {
    double sum = 0.0;
    double sum2 = 0.0;
    int n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const double l = lumaOf(encodedAt(image, x, y));
            sum += l;
            sum2 += l * l;
            ++n;
        }
    }
    const double mean = sum / n;
    return std::sqrt(std::max(0.0, sum2 / n - mean * mean));
}

/// The fine texture of a region: the standard deviation of the encoded luma's difference from its 3 x 3 neighbourhood's mean (the large
/// shading does not count).
double texture(const Image& image, int x0, int y0, int x1, int y1) {
    double sum2 = 0.0;
    int n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            double around = 0.0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    around += lumaOf(encodedAt(image, x + dx, y + dy));
                }
            }
            const double d = lumaOf(encodedAt(image, x, y)) - around / 9.0;
            sum2 += d * d;
            ++n;
        }
    }
    return std::sqrt(sum2 / n);
}

double meanLuma(const Image& image, int x0, int y0, int x1, int y1) {
    double sum = 0.0;
    int n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            sum += lumaOf(encodedAt(image, x, y));
            ++n;
        }
    }
    return sum / n;
}

bool sameRegion(const Image& a, const Image& b, int x0, int y0, int x1, int y1) {
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (a.at(x, y) != b.at(x, y)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

TEST_CASE("Skin Smoothing smooths skin and leaves what is not skin exactly as it was", "[render_reference][retouch]") {
    const Image in = pictureOf([](int x, int y) { return jittered(x < kW / 2 ? kSkin : kBlue, x, y, 0.04); });
    const Image out = libraryEffect(in, E::SkinSmoothing, skinParams(100.0, 0.0, 6.0));
    // The blue half is not skin: not one pixel of it changes, not even next to the skin.
    CHECK(sameRegion(in, out, kW / 2, 0, kW, kH));
    // The skin half loses its grain but keeps its colour.
    const double before = spread(in, 12, 12, 36, 52);
    const double after = spread(out, 12, 12, 36, 52);
    CHECK(after < before * 0.4);
    CHECK(std::abs(meanLuma(out, 12, 12, 36, 52) - meanLuma(in, 12, 12, 36, 52)) < 0.01);
    const Rgb a = encodedAt(out, 24, 32);
    CHECK(std::abs(a[0] - kSkin[0]) < 0.05);
    CHECK(std::abs(a[1] - kSkin[1]) < 0.05);
    CHECK(std::abs(a[2] - kSkin[2]) < 0.05);
}

TEST_CASE("Skin Smoothing with Amount 0 or Size 0 changes nothing and Keep Detail 100 keeps every pixel", "[render_reference][retouch]") {
    const Image in = pictureOf([](int x, int y) { return jittered(kSkin, x, y, 0.08); });
    CHECK(libraryEffect(in, E::SkinSmoothing, skinParams(0.0, 30.0, 6.0)) == in);
    CHECK(libraryEffect(in, E::SkinSmoothing, skinParams(100.0, 0.0, 0.0)) == in);
    CHECK(libraryEffect(in, E::SkinSmoothing, skinParams(100.0, 100.0, 6.0)) == in); // all the detail kept: nothing is taken away
    // Above 1.0 in linear light (a super-white highlight) is not clipped by an effect that did not touch it.
    Image bright = in;
    bright.set(3, 3, {1.5f, 1.4f, 1.3f, 1.0f});
    CHECK(libraryEffect(bright, E::SkinSmoothing, skinParams(0.0, 30.0, 6.0)) == bright);
}

TEST_CASE("Skin Smoothing keeps the edges inside the skin", "[render_reference][retouch]") {
    const Rgb light{0.85, 0.68, 0.58};
    const Rgb dark{0.55, 0.40, 0.33};
    const Image in = pictureOf([&](int x, int y) { return jittered(x < kW / 2 ? light : dark, x, y, 0.04); });
    const Image out = libraryEffect(in, E::SkinSmoothing, skinParams(100.0, 0.0, 8.0));
    const double stepBefore = meanLuma(in, kW / 2 - 8, 8, kW / 2, 56) - meanLuma(in, kW / 2, 8, kW / 2 + 8, 56);
    const double stepAfter = meanLuma(out, kW / 2 - 8, 8, kW / 2, 56) - meanLuma(out, kW / 2, 8, kW / 2 + 8, 56);
    CHECK(stepAfter > stepBefore * 0.95); // the edge is as strong as before
    CHECK(spread(out, 8, 12, 32, 52) < spread(in, 8, 12, 32, 52) * 0.4); // the grain on each side still goes
    CHECK(spread(out, 64, 12, 88, 52) < spread(in, 64, 12, 88, 52) * 0.4);
}

TEST_CASE("Skin Smoothing: Skin Tone and Tone Range decide what is skin and Show displays it", "[render_reference][retouch]") {
    const Image in = pictureOf([](int x, int y) { return jittered(x < kW / 2 ? kSkin : kBlue, x, y, 0.02); });
    const Image mask = libraryEffect(in, E::SkinSmoothing, skinParams(50.0, 30.0, 6.0, 123.0, 50.0, 50.0, 1.0));
    const auto grey = [&](const Image& image, int x, int y) { return encodedAt(image, x, y)[0]; };
    CHECK(grey(mask, 20, 32) > 0.9);           // inside the skin: skin
    CHECK(grey(mask, 70, 32) < 0.05);          // the blue: not
    CHECK(mask.alpha(70, 32) == 1.0f);         // the view is opaque
    // Skin Tone moved far away with a narrow range: the same skin is no longer skin.
    const Image moved = libraryEffect(in, E::SkinSmoothing, skinParams(50.0, 30.0, 6.0, 155.0, 5.0, 10.0, 1.0));
    CHECK(grey(moved, 20, 32) < 0.1);
    // A wide range takes in colours the default does not: a greener skin tone.
    const Rgb olive{0.74, 0.64, 0.45}; // a hue about 150 degrees on the vectorscope, 28 away from the default skin line
    const Image green = pictureOf([&](int x, int y) { return jittered(olive, x, y, 0.01); });
    const double narrow = grey(libraryEffect(green, E::SkinSmoothing, skinParams(50.0, 30.0, 6.0, 123.0, 0.0, 10.0, 1.0)), 48, 32);
    const double wide = grey(libraryEffect(green, E::SkinSmoothing, skinParams(50.0, 30.0, 6.0, 123.0, 100.0, 50.0, 1.0)), 48, 32);
    CHECK(wide > narrow + 0.3);
}

TEST_CASE("Skin Smoothing does not treat greys whites or blacks as skin and keeps alpha", "[render_reference][retouch]") {
    const Image grey = pictureOf([](int x, int y) {
        const double g = 0.5 + 0.06 * noise(x, y, 7);
        return Rgb{g, g, g}; // no chroma at all: whatever the grain
    });
    CHECK(libraryEffect(grey, E::SkinSmoothing, skinParams(100.0, 0.0, 8.0)) == grey);
    const Image white = pictureOf([](int, int) { return Rgb{0.99, 0.99, 0.99}; });
    CHECK(libraryEffect(white, E::SkinSmoothing, skinParams(100.0, 0.0, 8.0)) == white);
    const Image black = pictureOf([](int, int) { return Rgb{0.02, 0.01, 0.01}; });
    CHECK(libraryEffect(black, E::SkinSmoothing, skinParams(100.0, 0.0, 8.0)) == black);
    // Alpha: premultiplied half-transparent skin is smoothed as colour and keeps its alpha exactly.
    Image half(kW, kH);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            half.set(x, y, pixelOf(jittered(kSkin, x, y, 0.08), x < 40 ? 0.5 : x < 60 ? 1.0 : 0.0));
        }
    }
    const Image out = libraryEffect(half, E::SkinSmoothing, skinParams(100.0, 0.0, 6.0));
    bool alphaKept = true;
    bool finite = true;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            alphaKept = alphaKept && out.alpha(x, y) == half.alpha(x, y);
            for (int k = 0; k < 4; ++k) {
                finite = finite && std::isfinite(out.at(x, y)[static_cast<std::size_t>(k)]);
            }
        }
    }
    CHECK(alphaKept);
    CHECK(finite);
    CHECK(sameRegion(half, out, 60, 0, kW, kH)); // transparent pixels stay as they were
}

TEST_CASE("AI Face Retouch smooths only the skin the face map names", "[render_reference][retouch]") {
    const Image in = pictureOf([](int x, int y) { return jittered(kSkin, x, y, 0.04); });
    const auto inside = [](int x, int y) { return std::hypot((x + 0.5 - 48.0) / 30.0, (y + 0.5 - 32.0) / 26.0) < 1.0 ? 1.0 : 0.0; };
    const Image map = mapOf(inside);
    const Image out = libraryEffect(in, E::FaceRetouch, faceParams(100.0, 0.0, 6.0, 100.0, 10.0, 0.0, 0.0), &map);
    // Outside the map not a pixel changes (the same colour of skin, blemish removal on).
    bool outsideSame = true;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            if (inside(x, y) == 0.0 && out.at(x, y) != in.at(x, y)) {
                outsideSame = false;
            }
        }
    }
    CHECK(outsideSame);
    CHECK(spread(out, 40, 24, 56, 40) < spread(in, 40, 24, 56, 40) * 0.5);
    CHECK(std::abs(meanLuma(out, 40, 24, 56, 40) - meanLuma(in, 40, 24, 56, 40)) < 0.01);
}

TEST_CASE("AI Face Retouch with every amount at 0 or without a face map leaves the picture exactly as it was", "[render_reference][retouch]") {
    const Image in = pictureOf([](int x, int y) { return jittered(kSkin, x, y, 0.08); });
    const Image map = mapOf([](int, int) { return 1.0; }, [](int x, int) { return x > 30 && x < 60 ? 1.0 : 0.0; },
                            [](int, int y) { return y > 20 && y < 30 ? 1.0 : 0.0; });
    CHECK(libraryEffect(in, E::FaceRetouch, faceParams(0.0, 30.0, 6.0, 0.0, 10.0, 0.0, 0.0), &map) == in);
    // Sizes of 0 switch smoothing and blemish removal off even with their amounts up; teeth and eyes at 0 do nothing.
    CHECK(libraryEffect(in, E::FaceRetouch, faceParams(100.0, 30.0, 0.0, 100.0, 0.0, 0.0, 0.0), &map) == in);
    // No face map (the picture is not processed yet): the picture as it is, whatever the settings.
    CHECK(libraryEffect(in, E::FaceRetouch, faceParams(100.0, 0.0, 6.0, 100.0, 10.0, 100.0, 100.0), nullptr) == in);
    // An empty map (no face in the frame): nothing changes either.
    const Image none = mapOf([](int, int) { return 0.0; });
    CHECK(libraryEffect(in, E::FaceRetouch, faceParams(100.0, 0.0, 6.0, 100.0, 10.0, 100.0, 100.0), &none) == in);
}

TEST_CASE("AI Face Retouch heals a small spot and keeps a feature", "[render_reference][retouch]") {
    const Rgb spot{0.74, 0.45, 0.40};
    const Rgb nostril{0.25, 0.12, 0.10};
    const Image in = pictureOf([&](int x, int y) {
        if (x >= 30 && x < 33 && y >= 30 && y < 33) {
            return spot;
        }
        if (x >= 60 && x < 74 && y >= 28 && y < 38) {
            return nostril;
        }
        return jittered(kSkin, x, y, 0.01);
    });
    const Image map = mapOf([](int, int) { return 1.0; });
    const Image out = libraryEffect(in, E::FaceRetouch, faceParams(0.0, 30.0, 6.0, 100.0, 10.0, 0.0, 0.0), &map);
    const auto deviation = [&](const Image& image, int x0, int y0, int x1, int y1) {
        double sum = 0.0;
        int n = 0;
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const Rgb c = encodedAt(image, x, y);
                sum += std::hypot(c[0] - kSkin[0], c[1] - kSkin[1], c[2] - kSkin[2]);
                ++n;
            }
        }
        return sum / n;
    };
    CHECK(deviation(in, 30, 30, 33, 33) > 0.15);
    CHECK(deviation(out, 30, 30, 33, 33) < deviation(in, 30, 30, 33, 33) * 0.4); // the spot is gone
    CHECK(deviation(out, 60, 28, 74, 38) > deviation(in, 60, 28, 74, 38) * 0.95); // the nostril stays
    // Skin that is not a spot keeps its fine texture (the noise of +-0.01 is still there).
    CHECK(spread(out, 4, 4, 24, 24) > spread(in, 4, 4, 24, 24) * 0.8);
    // Blemish Removal at 0 does nothing even with a size.
    CHECK(libraryEffect(in, E::FaceRetouch, faceParams(0.0, 30.0, 6.0, 0.0, 10.0, 0.0, 0.0), &map) == in);
}

TEST_CASE("AI Face Retouch heals and smooths by the map's skin weight: a soft tail beyond the face's edge changes nothing", "[render_reference][retouch]") {
    // Found on a real photo: the parser's skin probability fades over a few pixels past the true edge of the head; healing by the
    // face's colour there smeared skin tone onto the background. Skin on the left, a grey room on the right, the map 1 inside and a
    // tail of 2 % over 6 pixels beyond the edge.
    const Rgb room{0.45, 0.43, 0.40};
    const Image in = pictureOf([&](int x, int y) { return x < 48 ? jittered(kSkin, x, y, 0.02) : jittered(room, x, y, 0.01); });
    const Image map = mapOf([](int x, int) { return x < 48 ? 1.0 : x < 54 ? 0.02 : 0.0; });
    const Image out = libraryEffect(in, E::FaceRetouch, faceParams(100.0, 0.0, 6.0, 100.0, 12.0, 0.0, 0.0), &map);
    double largest = 0.0;
    for (int y = 4; y < kH - 4; ++y) {
        for (int x = 48; x < kW; ++x) {
            const Rgb a = encodedAt(in, x, y);
            const Rgb b = encodedAt(out, x, y);
            for (std::size_t k = 0; k < 3; ++k) {
                largest = std::max(largest, std::abs(a[k] - b[k]));
            }
        }
    }
    CHECK(largest < 0.01); // the room did not take on skin colour
    CHECK(sameRegion(in, out, 54, 0, kW, kH)); // and beyond the tail not a pixel moved
    // Inside the face the same settings still work: the skin loses its grain.
    CHECK(spread(out, 8, 12, 40, 52) < spread(in, 8, 12, 40, 52) * 0.7);
}

TEST_CASE("AI Face Retouch whitens teeth and brightens eyes only inside their regions", "[render_reference][retouch]") {
    const Rgb teeth{0.85, 0.78, 0.55};
    const Rgb tongue{0.45, 0.12, 0.12};
    const Rgb sclera{0.85, 0.82, 0.80};
    const Rgb iris{0.25, 0.15, 0.10};
    const auto inMouth = [](int x, int y) { return x >= 20 && x < 44 && y >= 40 && y < 50; };
    const auto inEye = [](int x, int y) { return x >= 50 && x < 74 && y >= 8 && y < 16; };
    const Image in = pictureOf([&](int x, int y) {
        if (inMouth(x, y)) {
            return x < 32 ? teeth : tongue;
        }
        if (inEye(x, y)) {
            return x < 62 ? sclera : iris;
        }
        if (x >= 4 && x < 14 && y >= 4 && y < 14) {
            return teeth; // the same colour outside the mouth map
        }
        if (x >= 76 && x < 90 && y >= 40 && y < 52) {
            return sclera; // and outside the eye map
        }
        return kSkin;
    });
    const Image map = mapOf([](int, int) { return 0.0; }, [&](int x, int y) { return inMouth(x, y) ? 1.0 : 0.0; },
                            [&](int x, int y) { return inEye(x, y) ? 1.0 : 0.0; });
    const Image out = libraryEffect(in, E::FaceRetouch, faceParams(0.0, 30.0, 6.0, 0.0, 10.0, 100.0, 100.0), &map);
    const Rgb whitened = encodedAt(out, 26, 45);
    CHECK(lumaOf(whitened) > lumaOf(teeth) + 0.04);
    CHECK(satOf(whitened) < satOf(teeth) * 0.5);
    const Rgb tongueAfter = encodedAt(out, 38, 45);
    CHECK(std::abs(tongueAfter[0] - tongue[0]) < 0.01);
    CHECK(std::abs(tongueAfter[1] - tongue[1]) < 0.01);
    CHECK(std::abs(tongueAfter[2] - tongue[2]) < 0.01);
    const Rgb brighter = encodedAt(out, 55, 12);
    CHECK(lumaOf(brighter) > lumaOf(sclera) + 0.03);
    CHECK(satOf(brighter) < satOf(sclera));
    const Rgb irisAfter = encodedAt(out, 68, 12);
    CHECK(std::abs(irisAfter[0] - iris[0]) < 0.01);
    CHECK(std::abs(irisAfter[1] - iris[1]) < 0.01);
    CHECK(std::abs(irisAfter[2] - iris[2]) < 0.01);
    // Outside the regions: not a pixel moved (also not the skin around, with smoothing off).
    CHECK(sameRegion(in, out, 4, 4, 14, 14));
    CHECK(sameRegion(in, out, 76, 40, 90, 52));
    CHECK(sameRegion(in, out, 0, 20, kW, 38));
    // Teeth Whitening alone does not touch the eyes and Eye Brightening alone does not touch the teeth.
    const Image teethOnly = libraryEffect(in, E::FaceRetouch, faceParams(0.0, 30.0, 6.0, 0.0, 10.0, 100.0, 0.0), &map);
    CHECK(sameRegion(in, teethOnly, 50, 8, 74, 16));
    const Image eyesOnly = libraryEffect(in, E::FaceRetouch, faceParams(0.0, 30.0, 6.0, 0.0, 10.0, 0.0, 100.0), &map);
    CHECK(sameRegion(in, eyesOnly, 20, 40, 44, 50));
    // More Teeth Whitening whitens more.
    const double half = lumaOf(encodedAt(libraryEffect(in, E::FaceRetouch, faceParams(0.0, 30.0, 6.0, 0.0, 10.0, 50.0, 0.0), &map), 26, 45));
    CHECK(half > lumaOf(teeth));
    CHECK(half < lumaOf(whitened));
}

TEST_CASE("AI Face Retouch looks natural at its defaults: texture stays and nothing moves far", "[render_reference][retouch]") {
    // A gentle shading with pore-sized texture, skin everywhere.
    const Image in = pictureOf([](int x, int y) {
        const double shade = 0.04 * std::sin(x / 14.0) + 0.03 * std::cos(y / 11.0);
        return Rgb{kSkin[0] + shade + 0.03 * noise(x, y, 4), kSkin[1] + shade + 0.03 * noise(x, y, 5), kSkin[2] + shade + 0.03 * noise(x, y, 6)};
    });
    const Image map = mapOf([](int, int) { return 1.0; });
    // The defaults of the effect's descriptor: Skin Smoothing 50, Keep Detail 30, size 6, Blemish Removal 50 of size 10, Teeth 40, Eyes 30.
    const Image soft = libraryEffect(in, E::FaceRetouch, faceParams(50.0, 30.0, 6.0, 50.0, 10.0, 40.0, 30.0), &map);
    const Image full = libraryEffect(in, E::FaceRetouch, faceParams(100.0, 30.0, 6.0, 100.0, 10.0, 100.0, 100.0), &map);
    const double grain = texture(in, 20, 12, 76, 52);
    CHECK(texture(soft, 20, 12, 76, 52) > grain * 0.6);   // the defaults keep most of the texture
    CHECK(texture(soft, 20, 12, 76, 52) < grain * 0.95);  // and still smooth it
    CHECK(texture(full, 20, 12, 76, 52) > grain * 0.2);   // even at 100 % the skin is not plastic
    CHECK(texture(full, 20, 12, 76, 52) < texture(soft, 20, 12, 76, 52));
    double largest = 0.0;
    double shadeChange = 0.0;
    for (int y = 8; y < 56; ++y) {
        for (int x = 8; x < 88; ++x) {
            const Rgb a = encodedAt(in, x, y);
            const Rgb b = encodedAt(full, x, y);
            for (std::size_t k = 0; k < 3; ++k) {
                largest = std::max(largest, std::abs(a[k] - b[k]));
            }
        }
    }
    CHECK(largest < 0.2);
    // The large-scale shading (the light on the face) is not flattened: 16 x 16 block means move by less than 1 %.
    for (int by = 16; by < 48; by += 16) {
        for (int bx = 16; bx < 80; bx += 16) {
            shadeChange = std::max(shadeChange, std::abs(meanLuma(in, bx, by, bx + 16, by + 16) - meanLuma(full, bx, by, bx + 16, by + 16)));
        }
    }
    CHECK(shadeChange < 0.01);
}

TEST_CASE("AI Face Retouch views display the face map and a map of another size is stretched over the picture", "[render_reference][retouch]") {
    const Image in = pictureOf([](int x, int y) { return jittered(kSkin, x, y, 0.05); });
    const Image map = mapOf([](int x, int) { return x < kW / 2 ? 1.0 : 0.0; }, [](int, int y) { return y < kH / 2 ? 0.5 : 0.0; },
                            [](int x, int y) { return x >= kW / 2 && y >= kH / 2 ? 1.0 : 0.0; });
    const auto grey = [&](double view, int x, int y) {
        return encodedAt(libraryEffect(in, E::FaceRetouch, faceParams(50.0, 30.0, 6.0, 50.0, 10.0, 40.0, 30.0, view), &map), x, y)[0];
    };
    CHECK(grey(1.0, 10, 10) > 0.99);
    CHECK(grey(1.0, 80, 10) < 0.01);
    CHECK(std::abs(grey(2.0, 10, 10) - 0.5) < 0.01);
    CHECK(grey(2.0, 10, 50) < 0.01);
    CHECK(grey(3.0, 80, 50) > 0.99);
    CHECK(grey(3.0, 10, 10) < 0.01);
    // A face map made at a working size (half the picture's) is stretched over the picture: the left half is skin either way.
    const Image smallMap = mapOf([](int x, int) { return x < kW / 4 ? 1.0 : 0.0; }, {}, {}, kW / 2, kH / 2);
    const Image shown = libraryEffect(in, E::FaceRetouch, faceParams(50.0, 30.0, 6.0, 50.0, 10.0, 40.0, 30.0, 1.0), &smallMap);
    CHECK(encodedAt(shown, 10, 10)[0] > 0.99);
    CHECK(encodedAt(shown, 80, 10)[0] < 0.01);
}
