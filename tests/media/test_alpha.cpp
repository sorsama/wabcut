#include "TestMedia.h"

#include <wabcut/media/Probe.h>
#include <wabcut/media/ThumbnailService.h>
#include <wabcut/media/VideoDecoder.h>

#include <catch2/catch_test_macros.hpp>
#include <cstring>

using namespace wabcut;
using namespace wabcut::media;
using namespace wabcut::media::test;

namespace {

/// Alpha of pixel (x, y) of a YUVA444P10 frame, scaled to 8 bits.
int alpha8(const FrameHandle& f, int x, int y) {
    const Plane& a = f.cpu()->planes[3];
    std::uint16_t v = 0;
    std::memcpy(&v, a.data + static_cast<std::ptrdiff_t>(y) * a.stride + 2 * x, 2);
    return v >> 2;
}

void requireMedia(std::string_view name) {
    if (!haveMedia(name)) {
        SKIP("test clip " << name << " was not generated");
    }
}

} // namespace

TEST_CASE("Alpha: probe reports alpha mode, bit depth and chroma siting", "[media][alpha]") {
    requireMedia("prores_4444_alpha.mov");
    auto info = probe(mediaPath("prores_4444_alpha.mov"));
    REQUIRE(info.has_value());
    REQUIRE(info->video.size() == 1);
    const VideoStreamInfo& v = info->video.front();
    CHECK(v.codec == "prores");
    CHECK(v.profile == "4444");
    CHECK(v.alpha == AlphaMode::Straight);
    CHECK(v.pixelFormat == "yuva444p12le");
    CHECK(v.bitDepth == 12);
    CHECK(v.chromaSubsampling == "4:4:4");

    requireMedia("h264_1080p60_aac.mp4");
    auto opaque = probe(mediaPath("h264_1080p60_aac.mp4"));
    REQUIRE(opaque.has_value());
    CHECK(opaque->video.front().alpha == AlphaMode::None);
    CHECK(opaque->video.front().color.chromaLocation == ChromaLocation::Left);
}

TEST_CASE("Alpha: ProRes 4444 keeps its alpha plane (12-bit delivered as 10-bit)", "[media][alpha]") {
    requireMedia("prores_4444_alpha.mov");
    auto decoder = VideoDecoder::open(mediaPath("prores_4444_alpha.mov"));
    REQUIRE(decoder.has_value());
    for (std::int64_t i = 0; i < 12; ++i) {
        auto f = (*decoder)->frameAt(framesToTicks(i, kRate24));
        REQUIRE(f.has_value());
        CHECK(frameIndexOf(*f) == i);
        if (i == 0) {
            CHECK(f->format == PixelFormat::YUVA444P10);
            CHECK(f->planeCount() == 4);
            CHECK(f->alpha == AlphaMode::Straight);
            REQUIRE(f->cpu() != nullptr);
            CHECK(f->cpu()->planes[3].width == 640);
            CHECK(f->cpu()->planes[3].height == 360);
            CHECK(f->byteSize() >= std::size_t{640} * 360 * 2 * 4);
            // The ramp: alpha = x * 255 / 640 (8-bit) on every row (drawbox leaves alpha alone).
            for (const int y : {16, 200}) {
                for (const int x : {8, 160, 320, 480, 632}) {
                    INFO("x = " << x << ", y = " << y);
                    CHECK(std::abs(alpha8(*f, x, y) - x * 255 / 640) <= 2);
                }
            }
        }
    }
}

TEST_CASE("Alpha: 12-bit sources are delivered as 10-bit", "[media][alpha]") {
    requireMedia("ffv1_12bit.mkv");
    auto info = probe(mediaPath("ffv1_12bit.mkv"));
    REQUIRE(info.has_value());
    CHECK(info->video.front().bitDepth == 12);
    CHECK(info->video.front().alpha == AlphaMode::None);

    auto decoder = VideoDecoder::open(mediaPath("ffv1_12bit.mkv"));
    REQUIRE(decoder.has_value());
    for (std::int64_t i = 0; i < 10; ++i) {
        auto f = (*decoder)->frameAt(framesToTicks(i, kRate25));
        REQUIRE(f.has_value());
        CHECK(f->format == PixelFormat::YUV444P10);
        CHECK(f->alpha == AlphaMode::None);
        CHECK(frameIndexOf(*f) == i);
    }
}

TEST_CASE("Alpha: thumbnails carry the alpha channel", "[media][alpha]") {
    requireMedia("prores_4444_alpha.mov");
    ThumbnailOptions options;
    options.count = 1;
    options.height = 90; // 160 x 90
    auto strip = extractThumbnails(mediaPath("prores_4444_alpha.mov"), options);
    REQUIRE(strip.has_value());
    const Thumbnail& t = strip->thumbnails.front();
    REQUIRE(t.width == 160);
    const auto alphaAt = [&](int x, int y) {
        return t.rgba[(static_cast<std::size_t>(y) * t.width + x) * 4 + 3];
    };
    CHECK(alphaAt(2, 60) < 20);   // left edge: nearly transparent
    CHECK(alphaAt(80, 60) > 110); // middle: half
    CHECK(alphaAt(80, 60) < 145);
    CHECK(alphaAt(157, 60) > 235); // right edge: nearly opaque
}
