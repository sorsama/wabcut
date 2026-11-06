// AI video effects (0.7): tiled processing and the picture operations. A picture goes through any model in overlapping
// tiles with the same result as in one go (a model of finite reach), whatever its size - smaller than a tile, not a
// multiple of it, one pixel - at any scale; cancel stops between tiles; a failing model, a wrong-sized answer and a NaN
// are handled; the picture operations (resize, halve, mix, PSNR) are exact where they can be.

#include "VideoTestUtil.h"

#include <wabcut/ai/video/Tiling.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace wabcut;
using namespace wabcut::ai::video;
using namespace wabcut::ai::video::test;

namespace {

/// Nearest-neighbour enlargement of a tile: what a perfect upscaler of a local model would be.
TileModel nearest(int scale) {
    return [scale](std::span<const float> input, int size, std::vector<float>& output) -> std::expected<void, Error> {
        const int out = size * scale;
        output.assign(static_cast<std::size_t>(out) * out * 3, 0.0f);
        const std::size_t plane = static_cast<std::size_t>(size) * size;
        const std::size_t outPlane = static_cast<std::size_t>(out) * out;
        for (int c = 0; c < 3; ++c) {
            for (int y = 0; y < out; ++y) {
                for (int x = 0; x < out; ++x) {
                    output[static_cast<std::size_t>(c) * outPlane + static_cast<std::size_t>(y) * out + x] =
                        input[static_cast<std::size_t>(c) * plane + static_cast<std::size_t>(y / scale) * size + x / scale];
                }
            }
        }
        return {};
    };
}

/// A 3 x 3 box blur of one planar picture (edges clamped): reach one pixel.
std::vector<float> blur3(std::span<const float> in, int w, int h, bool reflect) {
    std::vector<float> out(in.size());
    const std::size_t plane = static_cast<std::size_t>(w) * h;
    for (int c = 0; c < 3; ++c) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float sum = 0.0f;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int sx = reflect ? reflectIndex(x + dx, w) : std::clamp(x + dx, 0, w - 1);
                        const int sy = reflect ? reflectIndex(y + dy, h) : std::clamp(y + dy, 0, h - 1);
                        sum += in[static_cast<std::size_t>(c) * plane + static_cast<std::size_t>(sy) * w + sx];
                    }
                }
                out[static_cast<std::size_t>(c) * plane + static_cast<std::size_t>(y) * w + x] = sum / 9.0f;
            }
        }
    }
    return out;
}

TileModel boxBlur() {
    return [](std::span<const float> input, int size, std::vector<float>& output) -> std::expected<void, Error> {
        output = blur3(input, size, size, false);
        return {};
    };
}

Picture wholeBlur(const Picture& in) {
    std::vector<float> planar(in.pixels() * 3);
    const std::size_t plane = in.pixels();
    for (std::size_t i = 0; i < plane; ++i) {
        for (std::size_t c = 0; c < 3; ++c) {
            planar[c * plane + i] = static_cast<float>(in.rgb[3 * i + c]) / 65535.0f;
        }
    }
    const std::vector<float> blurred = blur3(planar, in.width, in.height, true);
    Picture out = in;
    for (std::size_t i = 0; i < plane; ++i) {
        for (std::size_t c = 0; c < 3; ++c) {
            out.rgb[3 * i + c] = static_cast<std::uint16_t>(std::clamp(blurred[c * plane + i] * 65535.0f + 0.5f, 0.0f, 65535.0f));
        }
    }
    return out;
}

int maxDifference(const Picture& a, const Picture& b) {
    int worst = 0;
    for (std::size_t i = 0; i < a.rgb.size(); ++i) {
        worst = std::max(worst, std::abs(static_cast<int>(a.rgb[i]) - static_cast<int>(b.rgb[i])));
    }
    return worst;
}

} // namespace

TEST_CASE("reflectIndex mirrors without repeating the edge, also for tiny pictures", "[ai][video][tiling]") {
    CHECK(reflectIndex(0, 5) == 0);
    CHECK(reflectIndex(4, 5) == 4);
    CHECK(reflectIndex(-1, 5) == 1);
    CHECK(reflectIndex(5, 5) == 3);
    CHECK(reflectIndex(-6, 5) == 2);
    CHECK(reflectIndex(9, 5) == 1);
    CHECK(reflectIndex(7, 1) == 0);
    CHECK(reflectIndex(-3, 2) == 1);
    CHECK(tileCount(1920, 1080, 256) == 8 * 5);
    CHECK(tileCount(256, 256, 256) == 1);
    CHECK(tileCount(257, 1, 256) == 2);
    CHECK(tileCount(0, 10, 256) == 0);
}

TEST_CASE("An identity model gives the picture back for any size, tile and pad", "[ai][video][tiling]") {
    const auto identity = nearest(1);
    for (const auto& [w, h] : {std::pair{1, 1}, {7, 5}, {33, 20}, {64, 64}, {100, 37}, {130, 3}}) {
        for (const int tile : {64, 128}) {
            for (const int pad : {0, 8, 16}) {
                const Picture in = pattern(w, h, 2);
                auto result = runTiled(in, TileSpec{tile, pad, 1}, identity);
                INFO(w << "x" << h << " tile " << tile << " pad " << pad);
                REQUIRE(result);
                CHECK(result->picture.width == w);
                CHECK(result->picture.height == h);
                CHECK(result->tiles == tileCount(w, h, tile));
                CHECK(maxDifference(in, result->picture) <= 1);
            }
        }
    }
}

TEST_CASE("A 2x and a 4x model enlarge the picture tile by tile", "[ai][video][tiling]") {
    for (const int scale : {2, 4}) {
        const Picture in = pattern(70, 45, 1);
        auto result = runTiled(in, TileSpec{64, 16, scale}, nearest(scale));
        REQUIRE(result);
        REQUIRE(result->picture.width == 70 * scale);
        REQUIRE(result->picture.height == 45 * scale);
        CHECK(result->tiles == 2); // 70 wide takes two columns of 64, 45 high one row
        for (const auto& [x, y] : {std::pair{0, 0}, {69 * scale, 44 * scale}, {33 * scale + 1, 20 * scale + 2}, {65 * scale, 3}}) {
            for (int c = 0; c < 3; ++c) {
                const std::size_t out = (static_cast<std::size_t>(y) * result->picture.width + x) * 3 + c;
                const std::size_t src = (static_cast<std::size_t>(y / scale) * in.width + x / scale) * 3 + c;
                CHECK(std::abs(static_cast<int>(result->picture.rgb[out]) - static_cast<int>(in.rgb[src])) <= 1);
            }
        }
    }
}

TEST_CASE("Tiled processing equals processing in one go when the pad covers the model's reach (no seams)", "[ai][video][tiling]") {
    for (const auto& [w, h] : {std::pair{150, 90}, {64, 64}, {65, 130}}) {
        const Picture in = pattern(w, h, 3);
        const Picture whole = wholeBlur(in);
        for (const int tile : {64, 128}) {
            auto tiled = runTiled(in, TileSpec{tile, 8, 1}, boxBlur());
            INFO(w << "x" << h << " tile " << tile);
            REQUIRE(tiled);
            CHECK(maxDifference(whole, tiled->picture) <= 2);
        }
    }
}

TEST_CASE("Without a pad the seams show (the pad is what hides them)", "[ai][video][tiling]") {
    const Picture in = pattern(150, 90, 3);
    auto tiled = runTiled(in, TileSpec{64, 0, 1}, boxBlur());
    REQUIRE(tiled);
    CHECK(maxDifference(wholeBlur(in), tiled->picture) > 100);
}

TEST_CASE("Tiling cancels between tiles, ends with a failing model and refuses a wrong answer", "[ai][video][tiling]") {
    const Picture in = pattern(200, 200, 0);
    int asked = 0;
    auto cancelled = runTiled(in, TileSpec{64, 8, 1}, nearest(1), [&] { return ++asked > 3; });
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == ErrorCode::Cancelled);
    CHECK(asked == 4); // three tiles ran, the fourth was not started

    int tiles = 0;
    const TileModel failing = [&](std::span<const float>, int, std::vector<float>&) -> std::expected<void, Error> {
        return ++tiles == 2 ? std::unexpected(Error(ErrorCode::DecoderError, "boom")) : std::expected<void, Error>{};
    };
    auto failed = runTiled(in, TileSpec{64, 8, 1}, [&](std::span<const float> i, int s, std::vector<float>& o) {
        auto r = failing(i, s, o);
        if (r) {
            o.assign(static_cast<std::size_t>(s) * s * 3, 0.0f);
        }
        return r;
    });
    REQUIRE_FALSE(failed);
    CHECK(failed.error().message == "boom");

    const TileModel wrong = [](std::span<const float>, int size, std::vector<float>& out) -> std::expected<void, Error> {
        out.assign(static_cast<std::size_t>(size) * size, 0.0f); // one plane, not three
        return {};
    };
    auto badSize = runTiled(in, TileSpec{64, 8, 1}, wrong);
    REQUIRE_FALSE(badSize);
    CHECK(badSize.error().code == ErrorCode::DecoderError);

    CHECK_FALSE(runTiled(Picture{}, TileSpec{}, nearest(1)));
    CHECK_FALSE(runTiled(in, TileSpec{64, 8, 3}, nearest(1)));
}

TEST_CASE("A model's NaN and out-of-range values become black and white, never undefined", "[ai][video][tiling]") {
    const Picture in = pattern(10, 10, 0);
    const TileModel wild = [](std::span<const float>, int size, std::vector<float>& out) -> std::expected<void, Error> {
        out.assign(static_cast<std::size_t>(size) * size * 3, std::nanf(""));
        out[0] = 5.0f;
        out[1] = -3.0f;
        return {};
    };
    auto r = runTiled(in, TileSpec{64, 8, 1}, wild);
    REQUIRE(r);
    // The tile's first values lie in its pad; every kept value is NaN -> 0.
    CHECK(std::all_of(r->picture.rgb.begin(), r->picture.rgb.end(), [](std::uint16_t v) { return v == 0; }));
}

TEST_CASE("Picture operations: resize, halve, mix, PSNR", "[ai][video][picture]") {
    const Picture p = pattern(16, 8, 1, true);
    CHECK(p.valid());
    const Picture same = resized(p, 16, 8);
    CHECK(same.rgb == p.rgb);
    CHECK(same.alpha == p.alpha);
    const Picture big = resized(p, 32, 16);
    REQUIRE(big.valid());
    CHECK(big.alpha.size() == 32u * 16u);
    CHECK_FALSE(resized(Picture{}, 4, 4).valid());
    CHECK_FALSE(resized(p, 0, 4).valid());

    Picture flat;
    flat.width = 4;
    flat.height = 2;
    flat.rgb = {0, 0, 0, 100, 100, 100, 0, 0, 0, 100, 100, 100, 200, 200, 200, 300, 300, 300, 200, 200, 200, 300, 300, 300};
    const Picture half = halved(flat);
    REQUIRE(half.width == 2);
    REQUIRE(half.height == 1);
    CHECK(half.rgb[0] == 150); // (0 + 100 + 200 + 300) / 4
    CHECK(half.rgb[3] == 150);
    CHECK(halved(pattern(1, 1)).width == 1); // too small: unchanged

    const Picture a = pattern(8, 8, 0);
    const Picture b = pattern(8, 8, 5);
    CHECK(mixed(a, b, 0.0).rgb == a.rgb);
    CHECK(mixed(a, b, 1.0).rgb == b.rgb);
    const Picture m = mixed(a, b, 0.5);
    CHECK(std::abs(static_cast<int>(m.rgb[0]) - (static_cast<int>(a.rgb[0]) + static_cast<int>(b.rgb[0])) / 2) <= 1);
    CHECK(mixed(a, pattern(4, 4), 0.5).width == 4); // a size mismatch: the "to" picture
    CHECK(std::isinf(psnr(a, a)));
    CHECK(psnr(a, b) > 20.0);
    CHECK(psnr(a, b) < 80.0);
    CHECK(psnr(a, pattern(4, 4)) == 0.0);
    CHECK(meanAbsDifference(a, a) == 0.0);
    CHECK(meanAbsDifference(a, b) > 0.0);

    const auto plane = resizedPlane(p.alpha, 16, 8, 8, 4);
    CHECK(plane.size() == 32u);
    CHECK(resizedPlane({}, 16, 8, 8, 4).empty());
}
