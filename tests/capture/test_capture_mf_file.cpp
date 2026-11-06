// The Media Foundation source (0.7, Windows): the code a camera's pictures go through - the source reader's output type, the
// copy out of the sample's buffer (strides, the padded rows of a decoder's buffer), the clock - checked against pictures with
// known content. A camera is not opened: the same source reads a generated test clip (1080p60 H.264 with its frame number as a
// barcode, build/test-output/media) through the same reader, which decodes it to NV12 as it decodes a camera's MJPEG. Skipped,
// loudly, where the clip is missing or the platform is not Windows.

#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32

#include "SystemBackends.h"

#include <wabcut/capture/CaptureTypes.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace wabcut;
using namespace wabcut::capture;
namespace fs = std::filesystem;

namespace {

/// The frame number the generated clips carry: 16 cells of 32 x 32 luma pixels, bit b at x = 32 b, white = 1.
std::int64_t clipBarcode(const CaptureFrame& frame) {
    std::int64_t value = 0;
    for (int bit = 0; bit < 16; ++bit) {
        const int x = bit * 32 + 16;
        const int y = 16;
        if (frame.plane(0)[static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.stride(0)) + static_cast<std::size_t>(x)] >= 128) {
            value |= std::int64_t{1} << bit;
        }
    }
    return value;
}

/// The first picture of `file` decoded by FFmpeg as NV12: the reference MF's copy is compared with.
struct Reference {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> y;
    std::vector<std::uint8_t> uv;
};

bool decodeFirstPicture(const fs::path& file, Reference& out) {
    AVFormatContext* raw = nullptr;
    const std::string path = file.generic_string();
    if (avformat_open_input(&raw, path.c_str(), nullptr, nullptr) < 0) {
        return false;
    }
    std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> in(raw, [](AVFormatContext* p) { avformat_close_input(&p); });
    if (avformat_find_stream_info(in.get(), nullptr) < 0) {
        return false;
    }
    const int index = av_find_best_stream(in.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (index < 0) {
        return false;
    }
    const AVCodec* codec = avcodec_find_decoder(in->streams[index]->codecpar->codec_id);
    std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)> ctx(codec ? avcodec_alloc_context3(codec) : nullptr,
                                                                    [](AVCodecContext* p) { avcodec_free_context(&p); });
    if (!ctx || avcodec_parameters_to_context(ctx.get(), in->streams[index]->codecpar) < 0 || avcodec_open2(ctx.get(), codec, nullptr) < 0) {
        return false;
    }
    std::unique_ptr<AVPacket, void (*)(AVPacket*)> packet(av_packet_alloc(), [](AVPacket* p) { av_packet_free(&p); });
    std::unique_ptr<AVFrame, void (*)(AVFrame*)> frame(av_frame_alloc(), [](AVFrame* p) { av_frame_free(&p); });
    bool got = false;
    while (!got && av_read_frame(in.get(), packet.get()) >= 0) {
        if (packet->stream_index == index && avcodec_send_packet(ctx.get(), packet.get()) >= 0 &&
            avcodec_receive_frame(ctx.get(), frame.get()) >= 0) {
            got = true;
        }
        av_packet_unref(packet.get());
    }
    if (!got) {
        return false;
    }
    std::unique_ptr<AVFrame, void (*)(AVFrame*)> nv12(av_frame_alloc(), [](AVFrame* p) { av_frame_free(&p); });
    std::unique_ptr<SwsContext, void (*)(SwsContext*)> sws(sws_alloc_context(), [](SwsContext* p) { sws_free_context(&p); });
    nv12->format = AV_PIX_FMT_NV12;
    nv12->width = frame->width;
    nv12->height = frame->height;
    nv12->colorspace = frame->colorspace; // the same matrix and range: only the layout changes (an unset one is taken as BT.601)
    nv12->color_range = frame->color_range;
    if (av_frame_get_buffer(nv12.get(), 0) < 0 || sws_scale_frame(sws.get(), nv12.get(), frame.get()) < 0) {
        return false;
    }
    out.width = frame->width;
    out.height = frame->height;
    out.y.resize(static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height));
    out.uv.resize(static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height) / 2);
    for (int row = 0; row < out.height; ++row) {
        std::copy_n(nv12->data[0] + static_cast<std::ptrdiff_t>(row) * nv12->linesize[0], out.width,
                    out.y.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(out.width));
    }
    for (int row = 0; row < out.height / 2; ++row) {
        std::copy_n(nv12->data[1] + static_cast<std::ptrdiff_t>(row) * nv12->linesize[1], out.width,
                    out.uv.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(out.width));
    }
    return true;
}

double psnr(const std::uint8_t* a, const std::uint8_t* b, std::size_t count) {
    double sum = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        sum += d * d;
    }
    if (sum == 0.0) {
        return 99.0;
    }
    return 10.0 * std::log10(255.0 * 255.0 / (sum / static_cast<double>(count)));
}

} // namespace

TEST_CASE("Media Foundation source: pictures arrive as NV12 with the right planes, the right numbers and the right times",
          "[capture][mf]") {
    const fs::path file = fs::path(WABCUT_TEST_MEDIA_DIR) / "h264_1080p60_aac.mp4";
    if (!fs::exists(file)) {
        SKIP("The generated test clip is missing: " + file.generic_string() + " (build the media fixtures)");
    }
    auto opened = openMediaFoundationFile(file);
    REQUIRE(opened);
    VideoSource& source = **opened;
    CHECK(source.format().width == 1920);
    CHECK(source.format().height == 1080);
    CHECK(source.format().pixel == PixelFormat::Nv12);
    CHECK(source.format().frameRate == Rate{60, 1});

    std::mutex mutex;
    std::vector<CaptureFrame> frames;
    REQUIRE(source.start([&](CaptureFrame&& frame) {
        std::lock_guard lock(mutex);
        if (frames.size() < 40) {
            frames.push_back(std::move(frame));
        }
    }));
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < end) {
        {
            std::lock_guard lock(mutex);
            if (frames.size() >= 40) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    source.stop();
    REQUIRE(frames.size() == 40);

    for (std::size_t i = 0; i < frames.size(); ++i) {
        INFO("picture " << i);
        REQUIRE(frames[i].valid());
        CHECK(frames[i].width == 1920);
        CHECK(frames[i].height == 1080);
        CHECK(frames[i].pixel == PixelFormat::Nv12);
        // The number the clip carries: consecutive, from the first.
        CHECK(clipBarcode(frames[i]) == static_cast<std::int64_t>(i));
        // The time: the file's own stamps, 1/60 s apart.
        const double seconds = static_cast<double>(frames[i].hostNs - frames[0].hostNs) / 1e9;
        CHECK(std::abs(seconds - static_cast<double>(i) / 60.0) < 0.002);
    }

    // The first picture against FFmpeg's decode of the same file: the luma and the interleaved chroma agree (a shifted chroma plane,
    // the padded rows of a decoder's buffer read as picture, or a wrong stride would not).
    Reference reference;
    REQUIRE(decodeFirstPicture(file, reference));
    REQUIRE(reference.width == 1920);
    REQUIRE(reference.height == 1080);
    const CaptureFrame& first = frames.front();
    CHECK(psnr(first.plane(0), reference.y.data(), reference.y.size()) > 40.0);
    CHECK(psnr(first.plane(1), reference.uv.data(), reference.uv.size()) > 38.0);
}

#else

TEST_CASE("Media Foundation source: pictures arrive as NV12 with the right planes, the right numbers and the right times",
          "[capture][mf]") {
    SKIP("Media Foundation is Windows only");
}

#endif
