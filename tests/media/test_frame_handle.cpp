#include "TestMedia.h"

#include <wabcut/media/FrameHandle.h>
#include <wabcut/media/ThumbnailService.h>
#include <wabcut/media/VideoDecoder.h>
#include <wabcut/media/VideoFrame.h>

#include <catch2/catch_test_macros.hpp>
#include <type_traits>

using namespace wabcut;
using namespace wabcut::media;
using namespace wabcut::media::test;

static_assert(std::is_same_v<VideoFrame, FrameHandle>, "the rev-1 name stays an alias for one release");

TEST_CASE("FrameHandle: pixel format table", "[media][frame]") {
    CHECK(pixelFormatPlaneCount(PixelFormat::NV12) == 2);
    CHECK(pixelFormatPlaneCount(PixelFormat::YUV422P10) == 3);
    CHECK(pixelFormatPlaneCount(PixelFormat::YUVA444P10) == 4);
    CHECK(pixelFormatPlaneCount(PixelFormat::YUVA420P) == 4);
    CHECK(pixelFormatPlaneCount(PixelFormat::RGBA8) == 1);
    CHECK(pixelFormatBitDepth(PixelFormat::YUVA444P10) == 10);
    CHECK(pixelFormatBytesPerTexel(PixelFormat::P010, 1) == 4);
    CHECK(pixelFormatBytesPerTexel(PixelFormat::YUVA444P10, 3) == 2);
    CHECK(pixelFormatHasAlpha(PixelFormat::YUVA444P10));
    CHECK(pixelFormatHasAlpha(PixelFormat::RGBA8));
    CHECK_FALSE(pixelFormatHasAlpha(PixelFormat::YUV444P10));
    CHECK(pixelFormatName(PixelFormat::YUVA444P10) == "yuva444p10");
}

TEST_CASE("FrameHandle: payload variant", "[media][frame]") {
    FrameHandle f;
    f.width = 1920;
    f.height = 1080;
    f.format = PixelFormat::NV12;
    CHECK(f.payloadKind() == PayloadKind::None);
    CHECK_FALSE(f.isValid());
    CHECK(f.cpu() == nullptr);
    CHECK(f.byteSize() == 0);

    // A native payload (declared for the GPU path, never produced in the MVP) is not CPU memory.
    D3D11Texture texture;
    int dummyTexture = 0;
    texture.texture = &dummyTexture;
    texture.arraySlice = 3;
    texture.device = DeviceToken{7, 2};
    texture.owner = std::make_shared<int>(0);
    f.payload = texture;
    CHECK(f.payloadKind() == PayloadKind::D3D11Texture);
    CHECK(f.isValid());
    CHECK(f.cpu() == nullptr);
    CHECK(f.device() == DeviceToken{7, 2});
    CHECK_FALSE(f.device().isCpu());
    CHECK(f.byteSize() == std::size_t{1920} * 1080 * 3 / 2); // GPU-budget estimate of an NV12 surface

    f.payload = DrmPrime{};
    CHECK(f.payloadKind() == PayloadKind::DrmPrime);
    f.payload = CVPixelBuffer{};
    CHECK(f.payloadKind() == PayloadKind::CVPixelBuffer);
    CHECK_FALSE(f.isValid()); // no owner
}

TEST_CASE("FrameHandle: decoded frames carry stream timing, colour and identity", "[media][frame]") {
    if (!haveMedia("h264_1080p60_aac.mp4")) {
        SKIP("test clip was not generated");
    }
    for (const bool hardware : {false, true}) {
        DYNAMIC_SECTION((hardware ? "hardware" : "software")) {
            DecoderOptions options;
            options.allowHardware = hardware;
            auto decoder = VideoDecoder::open(mediaPath("h264_1080p60_aac.mp4"), -1, options);
            REQUIRE(decoder.has_value());
            auto f = (*decoder)->frameAt(framesToTicks(42, kRate60));
            REQUIRE(f.has_value());
            CHECK(f->payloadKind() == PayloadKind::CpuPlanes);
            REQUIRE(f->cpu() != nullptr);
            // MP4 time base 1/15360 at 60 fps: 256 units per frame, original timestamps survive untouched.
            CHECK(f->timeBase.num == 1);
            CHECK(f->timeBase.den == 15360);
            CHECK(f->streamPts == 42 * 256);
            CHECK(f->pts == framesToTicks(42, kRate60));
            CHECK(f->sampleAspect == Rate{1, 1});
            CHECK(f->orientation == Orientation{});
            CHECK(f->alpha == AlphaMode::None);
            CHECK(f->color.matrix == ColorMatrix::BT709);
            CHECK(f->color.chromaLocation == ChromaLocation::Left); // H.264 default siting, resolved
            if ((*decoder)->isHardwareAccelerated()) {
                CHECK_FALSE(f->device().isCpu()); // read back from a GPU device
                CHECK(f->device().generation >= 1);
            } else {
                CHECK(f->device().isCpu());
            }
        }
    }
}

TEST_CASE("FrameHandle: orientation follows the display matrix", "[media][frame]") {
    if (!haveMedia("rotated_90.mp4") || !haveMedia("transposed.mp4")) {
        SKIP("test clips were not generated");
    }
    auto rotated = VideoDecoder::open(mediaPath("rotated_90.mp4"));
    REQUIRE(rotated.has_value());
    auto r = (*rotated)->frameAt(Ticks{0});
    REQUIRE(r.has_value());
    CHECK(r->orientation == Orientation{90, false});

    auto transposed = VideoDecoder::open(mediaPath("transposed.mp4"));
    REQUIRE(transposed.has_value());
    auto t = (*transposed)->frameAt(Ticks{0});
    REQUIRE(t.has_value());
    CHECK(t->orientation == Orientation{90, true});
    CHECK(t->width == 640); // stored pixels are never rotated by the decoder

    // Ground truth from FFmpeg's own autorotation: the clip displays as a transpose, so frame 15's barcode
    // (cells 0..3 white, top-left in the stored picture) becomes a vertical strip down the left edge, at the
    // top.
    ThumbnailOptions options;
    options.count = 1; // centre of the 1 s clip = frame 15
    options.height = 640;
    auto strip = extractThumbnails(mediaPath("transposed.mp4"), options);
    REQUIRE(strip.has_value());
    const Thumbnail& thumb = strip->thumbnails.front();
    REQUIRE(thumb.width == 360);
    REQUIRE(thumb.height == 640);
    const auto green = [&](int x, int y) {
        return thumb.rgba[(static_cast<std::size_t>(y) * thumb.width + x) * 4 + 1];
    };
    for (int cell = 0; cell < 4; ++cell) {
        CHECK(green(16, cell * 32 + 16) > 220); // white cells down the left edge
    }
    CHECK(green(16, 4 * 32 + 16) < 40);       // cell 4 is black
    CHECK(green(thumb.width - 16, 16) < 220); // nothing white in the top-right corner
}
