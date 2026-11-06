// AI video effects (0.7) on decoded frames: a frame becomes the picture a model sees with ITS matrix and range (BT.601 /
// 709 / 2020, limited or full; 8 and 10 bit; 4:2:0, 4:2:2, 4:4:4; NV12 and P010; RGBA with alpha), and the processed picture
// goes back as YUV 4:4:4 at 10 bits with the same matrix and range (so the colour pipeline reads it like the original), or
// as RGBA8 for an RGB source. Real frames of the generated test media go through too.

#include "VideoTestUtil.h"

#include <wabcut/ai/video/FrameConvert.h>
#include <wabcut/ai/video/MediaFrames.h>
#include <wabcut/media/VideoDecoder.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numeric>
#include <string_view>

using namespace wabcut;
using namespace wabcut::ai::video;
using namespace wabcut::ai::video::test;
using media::PixelFormat;

namespace {

struct Owned {
    std::vector<std::vector<std::uint8_t>> planes;
};

/// A frame of `format` with every plane filled by `fill(plane, x, y)` (the sample's value in the format's own bits).
media::FrameHandle makeFrame(PixelFormat format, int w, int h, ColorInfo color, const std::function<int(int, int, int)>& fill,
                             AlphaMode alpha = AlphaMode::None) {
    media::FrameHandle f;
    f.width = w;
    f.height = h;
    f.format = format;
    f.color = color;
    f.alpha = alpha;
    f.pts = Ticks{1000};
    f.duration = Ticks{500};
    auto owned = std::make_shared<Owned>();
    media::CpuPlanes cpu;
    const int bytes = media::pixelFormatBytesPerTexel(format, 0);
    for (int p = 0; p < media::pixelFormatPlaneCount(format); ++p) {
        const bool chroma = p == 1 || p == 2;
        const bool halfW = chroma && (format == PixelFormat::NV12 || format == PixelFormat::P010 || format == PixelFormat::YUV420P ||
                                      format == PixelFormat::YUV420P10 || format == PixelFormat::YUV422P || format == PixelFormat::YUV422P10 ||
                                      format == PixelFormat::YUVA420P);
        const bool halfH = chroma && (format == PixelFormat::NV12 || format == PixelFormat::P010 || format == PixelFormat::YUV420P ||
                                      format == PixelFormat::YUV420P10 || format == PixelFormat::YUVA420P);
        const bool semi = (format == PixelFormat::NV12 || format == PixelFormat::P010) && p == 1;
        const int pw = halfW ? (w + 1) / 2 : w;
        const int ph = halfH ? (h + 1) / 2 : h;
        const int bpt = media::pixelFormatBytesPerTexel(format, p);
        std::vector<std::uint8_t> data(static_cast<std::size_t>(pw) * ph * bpt);
        for (int y = 0; y < ph; ++y) {
            for (int x = 0; x < pw; ++x) {
                const std::size_t at = (static_cast<std::size_t>(y) * pw + x) * bpt;
                if (format == PixelFormat::RGBA8) {
                    for (int c = 0; c < 4; ++c) {
                        data[at + c] = static_cast<std::uint8_t>(fill(c, x, y));
                    }
                } else if (semi) {
                    for (int c = 0; c < 2; ++c) {
                        const int v = fill(1 + c, x, y);
                        if (bytes == 1) {
                            data[at + c] = static_cast<std::uint8_t>(v);
                        } else {
                            data[at + 2 * c] = static_cast<std::uint8_t>(v & 0xFF);
                            data[at + 2 * c + 1] = static_cast<std::uint8_t>(v >> 8);
                        }
                    }
                } else if (bpt == 1) {
                    data[at] = static_cast<std::uint8_t>(fill(p, x, y));
                } else {
                    const int v = fill(p, x, y);
                    data[at] = static_cast<std::uint8_t>(v & 0xFF);
                    data[at + 1] = static_cast<std::uint8_t>(v >> 8);
                }
            }
        }
        cpu.planes[static_cast<std::size_t>(p)] = media::Plane{data.data(), pw * bpt, pw, ph};
        owned->planes.push_back(std::move(data));
    }
    cpu.owner = owned;
    f.payload = std::move(cpu);
    return f;
}

int channel(const Picture& p, int x, int y, int c) { return p.rgb[(static_cast<std::size_t>(y) * p.width + x) * 3 + c]; }

ColorInfo color(ColorMatrix m, ColorRange r) {
    ColorInfo c;
    c.matrix = m;
    c.range = r;
    c.primaries = ColorPrimaries::BT709;
    c.transfer = TransferFunction::BT709;
    return c;
}

} // namespace

TEST_CASE("Solid YUV colours convert with the stream's matrix and range", "[ai][video][convert]") {
    SECTION("BT.709 limited, 8-bit 4:2:0: white, black and red") {
        const ColorInfo c = color(ColorMatrix::BT709, ColorRange::Limited);
        const auto solid = [&](int y, int u, int v) {
            auto pic = pictureOf(makeFrame(PixelFormat::YUV420P, 8, 8, c, [&](int p, int, int) { return p == 0 ? y : p == 1 ? u : v; }));
            REQUIRE(pic);
            return std::array<int, 3>{channel(*pic, 3, 3, 0), channel(*pic, 3, 3, 1), channel(*pic, 3, 3, 2)};
        };
        const auto white = solid(235, 128, 128);
        const auto black = solid(16, 128, 128);
        const auto red = solid(63, 102, 240);
        for (int i = 0; i < 3; ++i) {
            CHECK(white[i] >= 65535 - 300);
            CHECK(black[i] <= 300);
        }
        CHECK(red[0] >= 65535 - 1500);
        CHECK(red[1] <= 1500);
        CHECK(red[2] <= 1500);
    }
    SECTION("full range is not stretched twice") {
        const auto grey = [](ColorRange range) {
            auto pic = pictureOf(makeFrame(PixelFormat::YUV444P, 4, 4, color(ColorMatrix::BT601, range), [](int p, int, int) { return p == 0 ? 200 : 128; }));
            REQUIRE(pic);
            return channel(*pic, 1, 1, 0);
        };
        CHECK(std::abs(grey(ColorRange::Full) - 200 * 257) <= 300);                 // 200 / 255
        CHECK(std::abs(grey(ColorRange::Limited) - (184 * 65535) / 219) <= 300);    // (200 - 16) / 219
    }
    SECTION("10-bit planar, P010 (value in the high bits) and NV12 agree with 8-bit white and black") {
        const ColorInfo c = color(ColorMatrix::BT2020NonConstant, ColorRange::Limited);
        const auto level = [&](PixelFormat f, int y10, int c10) {
            const int shift = f == PixelFormat::P010 ? 6 : 0;
            auto pic = pictureOf(makeFrame(f, 8, 8, c, [&](int p, int, int) { return (p == 0 ? y10 : c10) << shift; }));
            REQUIRE(pic);
            return channel(*pic, 4, 4, 1);
        };
        for (const PixelFormat f : {PixelFormat::YUV420P10, PixelFormat::YUV422P10, PixelFormat::YUV444P10, PixelFormat::P010}) {
            INFO(media::pixelFormatName(f));
            CHECK(level(f, 940, 512) >= 65535 - 300);
            CHECK(level(f, 64, 512) <= 300);
        }
        const ColorInfo c709 = color(ColorMatrix::BT709, ColorRange::Limited);
        auto nv12 = pictureOf(makeFrame(PixelFormat::NV12, 8, 8, c709, [](int p, int, int) { return p == 0 ? 235 : 128; }));
        REQUIRE(nv12);
        CHECK(channel(*nv12, 2, 5, 2) >= 65535 - 300);
    }
    SECTION("an untagged stream gets the conventional matrix: 601 below 720 lines, 709 from there") {
        const auto greenOf = [](int height) {
            ColorInfo untagged; // all Unknown
            auto pic = pictureOf(makeFrame(PixelFormat::YUV444P, 4, height, untagged, [](int p, int, int) { return p == 0 ? 120 : p == 1 ? 90 : 90; }));
            REQUIRE(pic);
            return channel(*pic, 1, 1, 1);
        };
        CHECK(greenOf(576) != greenOf(1080));
    }
}

TEST_CASE("Chroma is upsampled smoothly, not in blocks", "[ai][video][convert]") {
    const ColorInfo c = color(ColorMatrix::BT709, ColorRange::Limited);
    // Cb rises one step per chroma sample across a 4:2:0 frame: blue rises smoothly across the luma columns.
    auto pic = pictureOf(makeFrame(PixelFormat::YUV420P, 16, 8, c, [](int p, int x, int) { return p == 0 ? 128 : p == 1 ? 100 + x * 8 : 128; }));
    REQUIRE(pic);
    int worstStep = 0;
    int previous = channel(*pic, 0, 4, 2);
    for (int x = 1; x < 15; ++x) {
        const int v = channel(*pic, x, 4, 2);
        CHECK(v >= previous - 2);
        worstStep = std::max(worstStep, v - previous);
        previous = v;
    }
    // Nearest upsampling would step by a whole chroma step (4 luma steps of the ramp) at every second pixel and not at all in between.
    const int total = channel(*pic, 14, 4, 2) - channel(*pic, 0, 4, 2);
    CHECK(worstStep < total / 10);
}

TEST_CASE("RGBA frames keep their colour and alpha", "[ai][video][convert]") {
    const ColorInfo rgb = color(ColorMatrix::RGB, ColorRange::Full);
    auto frame = makeFrame(PixelFormat::RGBA8, 4, 4, rgb, [](int c, int x, int y) { return c == 3 ? 255 - x * 40 : (c + 1) * 40 + y * 10; }, AlphaMode::Straight);
    auto pic = pictureOf(frame);
    REQUIRE(pic);
    CHECK(pic->hasAlpha());
    CHECK(channel(*pic, 2, 1, 0) == 50 * 257);
    CHECK(channel(*pic, 2, 1, 2) == 130 * 257);
    CHECK(pic->alpha[1 * 4 + 2] == (255 - 80) * 257);
    // Without an alpha mode the alpha channel is ignored.
    frame.alpha = AlphaMode::None;
    auto opaque = pictureOf(frame);
    REQUIRE(opaque);
    CHECK_FALSE(opaque->hasAlpha());
}

TEST_CASE("A processed picture goes back as YUV 4:4:4 10-bit in the source's own matrix and range", "[ai][video][convert]") {
    const Picture original = pattern(32, 18, 2);
    for (const ColorMatrix matrix : {ColorMatrix::BT601, ColorMatrix::BT709, ColorMatrix::BT2020NonConstant}) {
        for (const ColorRange range : {ColorRange::Limited, ColorRange::Full}) {
            const ColorInfo c = color(matrix, range);
            const media::FrameHandle like = makeFrame(PixelFormat::YUV420P, 32, 18, c, [](int, int, int) { return 100; });
            auto stored = storedFrameOf(original, like);
            REQUIRE(stored);
            CHECK(stored->format == static_cast<std::uint8_t>(PixelFormat::YUV444P10));
            CHECK(stored->planes.size() == 3);
            CHECK(stored->matrix == static_cast<std::uint8_t>(matrix));
            CHECK(stored->range == static_cast<std::uint8_t>(range));
            auto shared = std::make_shared<const StoredFrame>(*stored);
            const media::FrameHandle back = frameOf(shared, &like);
            REQUIRE(back.isValid());
            CHECK(back.format == PixelFormat::YUV444P10);
            CHECK(back.color == like.color);
            auto round = pictureOf(back);
            REQUIRE(round);
            INFO("matrix " << static_cast<int>(matrix) << " range " << static_cast<int>(range));
            CHECK(psnr(original, *round) > 52.0); // 10-bit quantisation through the matrix
        }
    }
}

TEST_CASE("Alpha rides through as a fourth 10-bit plane; RGB sources stay RGBA8", "[ai][video][convert]") {
    const ColorInfo c = color(ColorMatrix::BT709, ColorRange::Limited);
    const Picture withAlpha = pattern(16, 8, 1, true);
    media::FrameHandle yuva = makeFrame(PixelFormat::YUVA420P, 16, 8, c, [](int, int, int) { return 100; }, AlphaMode::Straight);
    auto stored = storedFrameOf(withAlpha, yuva);
    REQUIRE(stored);
    CHECK(stored->format == static_cast<std::uint8_t>(PixelFormat::YUVA444P10));
    REQUIRE(stored->planes.size() == 4);
    auto round = pictureOf(frameOf(std::make_shared<const StoredFrame>(*stored)));
    REQUIRE(round);
    REQUIRE(round->hasAlpha());
    int worst = 0;
    for (std::size_t i = 0; i < withAlpha.alpha.size(); ++i) {
        worst = std::max(worst, std::abs(static_cast<int>(withAlpha.alpha[i]) - static_cast<int>(round->alpha[i])));
    }
    CHECK(worst < 140);

    // A picture without alpha from an alpha source is stored opaque (no fourth plane); an alpha picture from an opaque source too.
    auto opaque = storedFrameOf(pattern(16, 8, 1), yuva);
    REQUIRE(opaque);
    CHECK(opaque->planes.size() == 3);
    media::FrameHandle plain = makeFrame(PixelFormat::YUV420P, 16, 8, c, [](int, int, int) { return 100; });
    auto dropped = storedFrameOf(withAlpha, plain);
    REQUIRE(dropped);
    CHECK(dropped->planes.size() == 3);

    const ColorInfo rgb = color(ColorMatrix::RGB, ColorRange::Full);
    media::FrameHandle rgba = makeFrame(PixelFormat::RGBA8, 16, 8, rgb, [](int, int, int) { return 100; }, AlphaMode::Straight);
    auto packed = storedFrameOf(withAlpha, rgba);
    REQUIRE(packed);
    CHECK(packed->format == static_cast<std::uint8_t>(PixelFormat::RGBA8));
    CHECK(packed->planes.size() == 1);
    auto packedBack = pictureOf(frameOf(std::make_shared<const StoredFrame>(*packed)));
    REQUIRE(packedBack);
    CHECK(psnr(withAlpha, *packedBack) > 48.0);
    CHECK(packedBack->hasAlpha());
}

TEST_CASE("The frame of a stored picture is the cache's own memory, with the orientation and source of the decoded one", "[ai][video][convert]") {
    media::FrameHandle like = makeFrame(PixelFormat::YUV420P, 16, 8, color(ColorMatrix::BT709, ColorRange::Limited), [](int, int, int) { return 100; });
    like.orientation = media::Orientation{90, true};
    like.source = media::SourceIdentity{0xABCD, 3};
    like.timeBase = TimeBase{1, 90000};
    like.sampleAspect = Rate{4, 3};
    auto stored = storedFrameOf(pattern(16, 8), like);
    REQUIRE(stored);
    stored->pts = 777;
    stored->duration = 888;
    auto shared = std::make_shared<const StoredFrame>(*stored);
    const media::FrameHandle f = frameOf(shared, &like);
    REQUIRE(f.isValid());
    CHECK(f.pts == Ticks{777});
    CHECK(f.duration == Ticks{888});
    CHECK(f.orientation == like.orientation);
    CHECK(f.source == like.source);
    CHECK(f.timeBase == TimeBase{1, 90000});
    CHECK(f.sampleAspect == Rate{4, 3});
    REQUIRE(f.cpu() != nullptr);
    CHECK(f.cpu()->planes[0].data == shared->planes[0].data.data()); // no copy
    CHECK(f.cpu()->planes[0].stride == 16 * 2);
    // The frame keeps the cache's memory alive.
    const std::uint8_t first = shared->planes[0].data[0];
    shared.reset();
    CHECK(f.cpu()->planes[0].data[0] == first);
    CHECK_FALSE(frameOf(nullptr).isValid());
}

TEST_CASE("Frames the converter cannot read are refused with a reason", "[ai][video][convert]") {
    media::FrameHandle native;
    native.width = 16;
    native.height = 8;
    native.format = PixelFormat::NV12;
    media::D3D11Texture texture;
    texture.owner = std::make_shared<int>(1);
    native.payload = texture;
    auto r = pictureOf(native);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::Unsupported);
    media::FrameHandle unknown = makeFrame(PixelFormat::YUV420P, 8, 8, ColorInfo{}, [](int, int, int) { return 0; });
    unknown.format = PixelFormat::Unknown;
    CHECK_FALSE(pictureOf(unknown));
    CHECK_FALSE(storedFrameOf(Picture{}, makeFrame(PixelFormat::YUV420P, 8, 8, ColorInfo{}, [](int, int, int) { return 0; })));
}

TEST_CASE("Real decoded frames go through: 8-bit 4:2:0, 10-bit 4:2:2 and ProRes with alpha", "[ai][video][convert][media]") {
    const std::filesystem::path media = WABCUT_TEST_MEDIA_DIR;
    for (const char* name : {"h264_360p120.mp4", "h264_422_10bit.mp4", "hevc_main10_420.mp4", "prores_4444_alpha.mov"}) {
        const std::filesystem::path file = media / name;
        INFO(name);
        // tools/gen_test_media.py makes the HEVC Main 10 clip only when a 10-bit HEVC encoder is there (it is optional,
        // and CI runners have none), so that one clip is skipped when missing; the other three are always made.
        if (std::string_view(name) == "hevc_main10_420.mp4" && !std::filesystem::exists(file)) {
            WARN("hevc_main10_420.mp4 was not generated (no 10-bit HEVC encoder); skipping that clip");
            continue;
        }
        REQUIRE(std::filesystem::exists(file));
        auto decoder = wabcut::media::VideoDecoder::open(file.string());
        REQUIRE(decoder);
        auto frame = (*decoder)->frameAt(Ticks{});
        REQUIRE(frame);
        auto pic = pictureOf(*frame);
        REQUIRE(pic);
        CHECK(pic->width == frame->width);
        CHECK(pic->height == frame->height);
        CHECK(pic->valid());
        auto stored = storedFrameOf(*pic, *frame);
        REQUIRE(stored);
        auto round = pictureOf(frameOf(std::make_shared<const StoredFrame>(*stored)));
        REQUIRE(round);
        CHECK(psnr(*pic, *round) > 40.0);
        if (std::string(name).starts_with("prores_4444")) {
            CHECK(frame->alpha != AlphaMode::None);
            CHECK(round->hasAlpha());
        }
    }
}
