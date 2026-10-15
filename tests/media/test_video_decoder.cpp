#include "TestMedia.h"

#include <wabcut/media/VideoDecoder.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <optional>
#include <random>
#include <vector>

using namespace wabcut;
using namespace wabcut::media;
using namespace wabcut::media::test;

namespace {

std::unique_ptr<VideoDecoder> openOrSkip(std::string_view name, const DecoderOptions& options = {}) {
    if (!haveMedia(name)) {
        SKIP("test clip " << name << " was not generated");
    }
    auto decoder = VideoDecoder::open(mediaPath(name), -1, options);
    INFO(name << ": " << (decoder ? "" : decoder.error().message));
    REQUIRE(decoder.has_value());
    return std::move(*decoder);
}

/// Asserts that frameAt(t) returns frame `expected` (by barcode and by timestamp range).
void checkFrameAt(VideoDecoder& decoder, Ticks t, std::int64_t expected) {
    auto frame = decoder.frameAt(t);
    INFO("t = " << t.value << " ticks (" << t.toSeconds() << " s), expected frame " << expected << ": "
                << (frame ? "" : frame.error().message));
    REQUIRE(frame.has_value());
    CHECK(frameIndexOf(*frame) == expected);
    CHECK(frame->pts <= t);
}

/// Frame-exact sequential + random access over a constant-rate clip with `frames` frames at `rate`.
void checkFrameAccuracy(VideoDecoder& decoder, std::int64_t frames, Rate rate) {
    const Ticks frame = framesToTicks(1, rate);
    SECTION("sequential: every frame, at its start, middle and last tick, without seeking") {
        const auto seeksBefore = decoder.stats().seeks;
        for (std::int64_t i = 0; i < frames; ++i) {
            const Ticks start = framesToTicks(i, rate);
            auto f = decoder.frameAt(start);
            REQUIRE(f.has_value());
            CHECK(frameIndexOf(*f) == i);
            CHECK(f->pts == start);
            CHECK(f->duration == frame);
            checkFrameAt(decoder, start + Ticks{frame.value / 2}, i);
            checkFrameAt(decoder, start + frame - Ticks{1}, i);
        }
        CHECK(decoder.stats().seeks == seeksBefore);
    }
    SECTION("random access, including backwards jumps and GOP boundaries") {
        std::vector<std::int64_t> targets{frames - 1, 0,  frames / 2, 1,  frames - 2, 59,
                                          60,         61, 29,         30, 31,         0};
        std::mt19937 rng(20261011);
        std::uniform_int_distribution<std::int64_t> pick(0, frames - 1);
        for (int i = 0; i < 40; ++i) {
            targets.push_back(pick(rng));
        }
        for (const std::int64_t target : targets) {
            if (target < 0 || target >= frames) {
                continue;
            }
            const Ticks t = framesToTicks(target, rate) + Ticks{frame.value / 3};
            checkFrameAt(decoder, t, target);
        }
    }
}

} // namespace

TEST_CASE("Video: 8-bit H.264 decodes frame-exactly on the hardware path", "[media][video][hw]") {
    auto decoder = openOrSkip("h264_1080p60_aac.mp4");
    INFO("backend: " << decoder->backendName());
    if (gpuExpected()) {
        // The first backend of the policy order must be the one in use on a machine with a GPU
        // (cuda with an NVIDIA GPU, d3d11va otherwise).
        REQUIRE(decoder->backendName() == hardwareBackendOrder().front());
        CHECK(decoder->isHardwareAccelerated());
    }
    CHECK(decoder->firstFrameTime() == Ticks{0});
    auto first = decoder->frameAt(Ticks{0});
    REQUIRE(first.has_value());
    CHECK(first->width == 1920);
    CHECK(first->height == 1080);
    if (decoder->isHardwareAccelerated()) {
        CHECK(first->format == PixelFormat::NV12);
        CHECK(first->cpu()->planes[1].width == 960);
        CHECK(first->cpu()->planes[1].height == 540);
    }
    CHECK(first->color.matrix == ColorMatrix::BT709);
    CHECK(first->color.range == ColorRange::Limited);
    checkFrameAccuracy(*decoder, 600, kRate60);
}

TEST_CASE("Video: 8-bit H.264 decodes frame-exactly in software", "[media][video]") {
    DecoderOptions options;
    options.allowHardware = false;
    auto decoder = openOrSkip("h264_1080p60_aac.mp4", options);
    CHECK(decoder->backendName() == "software");
    CHECK_FALSE(decoder->isHardwareAccelerated());
    auto first = decoder->frameAt(Ticks{0});
    REQUIRE(first.has_value());
    CHECK(first->format == PixelFormat::YUV420P);
    CHECK(first->cpu()->planes[1].width == 960);
    checkFrameAccuracy(*decoder, 600, kRate60);
}

TEST_CASE("Video: preferred backend is tried first", "[media][video][hw]") {
    if (!gpuExpected()) {
        SKIP("no GPU expected on this machine");
    }
    // Prefer the backend the policy would otherwise try second.
    const std::vector<std::string> order = hardwareBackendOrder();
    REQUIRE(order.size() >= 2);
    DecoderOptions options;
    options.preferredBackend = order[1];
    auto decoder = openOrSkip("h264_1080p60_aac.mp4", options);
    CHECK(decoder->backendName() == order[1]);
    checkFrameAt(*decoder, framesToTicks(123, kRate60), 123);

    options.preferredBackend = "software";
    auto software = openOrSkip("h264_1080p60_aac.mp4", options);
    CHECK(software->backendName() == "software");

    options.preferredBackend = "no-such-backend";
    auto bad = VideoDecoder::open(mediaPath("h264_1080p60_aac.mp4"), -1, options);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("Video: backend order follows the GPU vendor policy and can be overridden", "[media][video][hw]") {
    const std::vector<std::string> vendors = gpuVendors();
    const bool nvidia = std::find(vendors.begin(), vendors.end(), "NVIDIA") != vendors.end();
    const std::vector<std::string> policy = hardwareBackendOrder();
#if defined(_WIN32)
    // Measured readback throughput: cuda beats d3d11va on NVIDIA (see hardwareBackendOrder()).
    CHECK(policy == (nvidia ? std::vector<std::string>{"cuda", "d3d11va", "qsv"}
                            : std::vector<std::string>{"d3d11va", "cuda", "qsv"}));
#else
    CHECK_FALSE(policy.empty());
#endif

    setHardwareBackendOrder({"no-such", "d3d11va", "d3d11va"}); // unknown and duplicate names are ignored
#if defined(_WIN32)
    CHECK(hardwareBackendOrder() == std::vector<std::string>{"d3d11va"});
    if (gpuExpected() && haveMedia("h264_1080p60_aac.mp4")) {
        auto decoder = VideoDecoder::open(mediaPath("h264_1080p60_aac.mp4"));
        REQUIRE(decoder.has_value());
        CHECK((*decoder)->backendName() == "d3d11va");
    }
#endif
    setHardwareBackendOrder({});
    CHECK(hardwareBackendOrder() == policy);
}

TEST_CASE("Video: every available hardware backend is frame-exact", "[media][video][hw]") {
    // The platform order only exercises the first backend that works; force each one in turn so a fallback
    // machine (no d3d11va, no NVIDIA) still gets frame-exact results.
    for (const std::string& backend : hardwareBackendOrder()) {
        DYNAMIC_SECTION("backend " << backend) {
            DecoderOptions options;
            options.preferredBackend = backend;
            auto decoder = openOrSkip("h264_1080p60_aac.mp4", options);
            if (decoder->backendName() != backend) {
                SKIP(backend << " is not available on this machine");
            }
            checkFrameAccuracy(*decoder, 600, kRate60);
        }
    }
}

TEST_CASE("Video: H.264 10-bit 4:2:2 falls back to software automatically", "[media][video][hw]") {
    auto decoder = openOrSkip("h264_422_10bit.mp4");
    // No consumer GPU before RTX 50 / Arc decodes H.264 4:2:2 10-bit; the decoder must notice and use
    // software.
    CHECK(decoder->backendName() == "software");
    CHECK_FALSE(decoder->isHardwareAccelerated());
    if (gpuExpected()) {
        CHECK(decoder->stats().hardwareFallbacks >= 1); // d3d11va (and cuda) were tried and rejected
    }
    auto first = decoder->frameAt(Ticks{0});
    REQUIRE(first.has_value());
    CHECK(first->format == PixelFormat::YUV422P10);
    CHECK(first->cpu()->planes[1].width == 640);
    CHECK(first->cpu()->planes[1].height == 720);
    checkFrameAccuracy(*decoder, 60, kRate30);
}

TEST_CASE("Video: HEVC Main 10 decodes to P010 on the GPU", "[media][video][hw]") {
    auto decoder = openOrSkip("hevc_main10_420.mp4");
    if (gpuExpected()) {
        CHECK(decoder->backendName() == hardwareBackendOrder().front());
    }
    auto first = decoder->frameAt(Ticks{0});
    REQUIRE(first.has_value());
    CHECK(first->format == (decoder->isHardwareAccelerated() ? PixelFormat::P010 : PixelFormat::YUV420P10));
    CHECK(first->color == ColorInfo{ColorMatrix::BT2020NonConstant, ColorRange::Limited,
                                    ColorPrimaries::BT2020, TransferFunction::PQ, ChromaLocation::Left});
    checkFrameAccuracy(*decoder, 60, kRate30);
}

TEST_CASE("Video: ProRes 422 is native 4:2:2 10-bit and exact at 23.976", "[media][video]") {
    auto decoder = openOrSkip("prores_422.mov");
    CHECK(decoder->backendName() == "software"); // no hardware ProRes decoding outside macOS
    auto first = decoder->frameAt(Ticks{0});
    REQUIRE(first.has_value());
    CHECK(first->format == PixelFormat::YUV422P10);
    CHECK(first->color.chromaLocation == ChromaLocation::Left); // unsignalled siting resolved for 4:2:2
    checkFrameAccuracy(*decoder, 48, kRate23_976);
}

/// frameAt(t) must return frame `n` whose [pts, pts + duration) is exactly [start, next): both ends are
/// checked.
void checkSpan(VideoDecoder& decoder, Ticks t, std::int64_t n, Ticks start, std::optional<Ticks> next) {
    auto f = decoder.frameAt(t);
    INFO("t = " << t.toSeconds() << " s, frame " << n << ": " << (f ? "" : f.error().message));
    REQUIRE(f.has_value());
    CHECK(frameIndexOf(*f) == n);
    CHECK(f->pts == start);
    CHECK(f->contains(t));
    if (next) {
        CHECK(f->duration == *next - start);
        CHECK(f->contains(*next - Ticks{1}));
        CHECK_FALSE(f->contains(*next));
    }
}

TEST_CASE("Video: VFR clip returns the frame on screen", "[media][video]") {
    // Frame n starts at k(n)/30 s with k = n for n < 30 and 30 + 2(n - 30) after (see gen_test_media.py).
    const auto startOf = [](std::int64_t n) {
        return framesToTicks(n < 30 ? n : 30 + 2 * (n - 30), kRate30);
    };
    const auto nextOf = [&](std::int64_t n) {
        return n < 59 ? std::optional<Ticks>(startOf(n + 1)) : std::nullopt;
    };
    const Ticks slot = framesToTicks(1, kRate30);
    for (const bool hardware : {true, false}) {
        DYNAMIC_SECTION((hardware ? "hardware allowed" : "software")) {
            DecoderOptions options;
            options.allowHardware = hardware;
            auto decoder = openOrSkip("vfr.mp4", options);
            CHECK(decoder->streamInfo().variableFrameRate);
            for (std::int64_t n = 0; n < 60; ++n) {
                checkSpan(*decoder, startOf(n), n, startOf(n), nextOf(n));
                if (n >= 30 && n < 59) {
                    // The second 1/30 s slot of a 15 fps frame still shows that frame.
                    checkSpan(*decoder, startOf(n) + slot + Ticks{slot.value / 2}, n, startOf(n), nextOf(n));
                }
            }
            for (const std::int64_t n : {45LL, 3LL, 58LL, 30LL, 29LL, 0LL, 31LL}) {
                checkSpan(*decoder, startOf(n) + Ticks{1}, n, startOf(n), nextOf(n));
            }
        }
    }
}

TEST_CASE("Video: VFR without a sample index publishes display durations (review repro)", "[media][video]") {
    // vfr_quadratic.mkv: frame n starts at n*n/25 s. FFV1 in Matroska carries packet durations of 1/25 s, so
    // the display duration has to come from the next timestamp.
    const auto startOf = [](std::int64_t n) { return framesToTicks(n * n, kRate25); };
    auto decoder = openOrSkip("vfr_quadratic.mkv");
    for (std::int64_t n = 0; n < 8; ++n) {
        const auto next = n < 7 ? std::optional<Ticks>(startOf(n + 1)) : std::nullopt;
        checkSpan(*decoder, startOf(n), n, startOf(n), next);
        if (next) {
            checkSpan(*decoder, *next - Ticks{1}, n, startOf(n), next); // the last tick before the next frame
        }
    }
    // The reported case: .30 s lies in frame 2 = [.16, .36).
    checkSpan(*decoder, Ticks{kTicksPerSecond * 30 / 100}, 2, startOf(2), startOf(3));
    // Random order (seeks) gives the same spans.
    for (const std::int64_t n : {6LL, 1LL, 4LL, 0LL, 7LL, 3LL}) {
        checkSpan(*decoder, startOf(n) + Ticks{1}, n, startOf(n),
                  n < 7 ? std::optional<Ticks>(startOf(n + 1)) : std::nullopt);
    }
}

TEST_CASE("Video: rotation is reported, pixels stay as stored", "[media][video]") {
    auto decoder = openOrSkip("rotated_90.mp4");
    CHECK(decoder->streamInfo().rotationDegrees == 90);
    auto f = decoder->frameAt(Ticks{0});
    REQUIRE(f.has_value());
    CHECK(f->width == 640);
    CHECK(f->height == 360);
    CHECK(frameIndexOf(*f) == 0);
}

TEST_CASE("Video: frames are cheap shared handles that outlive the decoder", "[media][video]") {
    FrameHandle kept;
    {
        auto decoder = openOrSkip("h264_1080p60_aac.mp4");
        auto f = decoder->frameAt(framesToTicks(77, kRate60));
        REQUIRE(f.has_value());
        kept = *f;
        const FrameHandle copy = kept;
        CHECK(copy.cpu()->planes[0].data == kept.cpu()->planes[0].data);
        CHECK(copy.cpu()->owner == kept.cpu()->owner);
        CHECK(kept.byteSize() >= std::size_t{1920} * 1080 * 3 / 2);
    }
    REQUIRE(kept.isValid());
    CHECK(frameIndexOf(kept) == 77); // still readable after the decoder is gone
}

TEST_CASE("Video: errors", "[media][video]") {
    auto missing = VideoDecoder::open(mediaPath("missing.mp4"));
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ErrorCode::NotFound);

    if (!haveMedia("sine_48k.wav") || !haveMedia("h264_1080p60_aac.mp4")) {
        SKIP("test clips were not generated");
    }
    auto audioOnly = VideoDecoder::open(mediaPath("sine_48k.wav"));
    REQUIRE_FALSE(audioOnly.has_value());
    CHECK(audioOnly.error().code == ErrorCode::NotFound);

    auto audioStream = VideoDecoder::open(mediaPath("h264_1080p60_aac.mp4"), 1);
    REQUIRE_FALSE(audioStream.has_value());
    CHECK(audioStream.error().code == ErrorCode::InvalidArgument);

    auto decoder = VideoDecoder::open(mediaPath("h264_1080p60_aac.mp4"));
    REQUIRE(decoder.has_value());
    auto negative = (*decoder)->frameAt(Ticks{-1});
    REQUIRE_FALSE(negative.has_value());
    CHECK(negative.error().code == ErrorCode::InvalidArgument);
    auto past = (*decoder)->frameAt(Ticks::fromSeconds(11));
    REQUIRE_FALSE(past.has_value());
    CHECK(past.error().code == ErrorCode::NotFound);
    checkFrameAt(**decoder, Ticks::fromSeconds(10) - Ticks{1}, 599); // the last tick still has a frame
    checkFrameAt(**decoder, Ticks{0}, 0);                            // and the decoder recovers after errors
}

TEST_CASE("Video: a 240 fps source read at 60 fps times returns every fourth frame and decodes the rest only",
          "[media][video]") {
    // h264_240fps*.mp4: 480 frames at 240 fps in one GOP (gen_test_media.py), one with 2 B-frames.
    for (const char* clip : {"h264_240fps.mp4", "h264_240fps_bframes.mp4"}) {
        DYNAMIC_SECTION(clip) {
            DecoderOptions options;
            options.allowHardware = false; // software: no read-back, exact counts
            auto decoder = openOrSkip(clip, options);
            const Rate rate = decoder->streamInfo().frameRate;
            INFO("frame rate " << rate.num << "/" << rate.den);
            CHECK(std::abs(rate.toDouble() - 240.0) < 0.01);
            const Ticks sourceFrame = framesToTicks(1, Rate{240, 1});
            for (std::int64_t n = 0; n < 120; ++n) {
                const Ticks t = framesToTicks(n, kRate60); // sequence frame n of a 60 fps sequence
                auto f = decoder->frameAt(t);
                INFO("sequence frame " << n << ": " << (f ? "" : f.error().message));
                REQUIRE(f.has_value());
                CHECK(frameIndexOf(*f) == 4 * n);
                CHECK(f->pts == t);
                CHECK(f->duration == sourceFrame);
            }
            const VideoDecoderStats s = decoder->stats();
            INFO("decoded " << s.framesDecoded << ", delivered " << s.framesDelivered);
            CHECK(s.seeks == 0); // forward only
            CHECK(s.framesDelivered == 120);
            CHECK(s.framesDecoded >= 4 * 119 + 1); // every source frame up to the last one shown
            CHECK(s.framesDecoded <= 480);
            CHECK(s.framesReadBack == 0);
            CHECK(s.framesConverted == 0); // yuv420p is handed out as it is
        }
    }
}

TEST_CASE("Video: on the GPU a 240 fps source read at 60 fps times reads back only the frames it returns",
          "[media][video][hw]") {
    auto decoder = openOrSkip("h264_240fps.mp4");
    INFO("backend: " << decoder->backendName());
    if (!decoder->isHardwareAccelerated()) {
        SKIP("not decoded on the GPU here");
    }
    if (decoder->backendName() == "qsv") {
        SKIP("QSV's wrapper decoder copies every frame at once (it recycles its surfaces)");
    }
    for (std::int64_t n = 0; n < 120; ++n) {
        auto f = decoder->frameAt(framesToTicks(n, kRate60));
        REQUIRE(f.has_value());
        CHECK(frameIndexOf(*f) == 4 * n);
    }
    const VideoDecoderStats s = decoder->stats();
    INFO("decoded " << s.framesDecoded << ", delivered " << s.framesDelivered << ", read back " << s.framesReadBack);
    CHECK(s.framesDelivered == 120);
    CHECK(s.framesDecoded >= 4 * 119 + 1);
    CHECK(s.framesReadBack <= s.framesDelivered); // the frames stepped over stay on the GPU
}

TEST_CASE("Video: frames that need a pixel-format conversion are converted when they are returned only",
          "[media][video]") {
    // FFV1 4:4:4 12-bit is handed out as YUV444P10; reading frame 5 first decodes frames 0-6 (one ahead).
    auto decoder = openOrSkip("ffv1_12bit.mkv");
    auto f = decoder->frameAt(framesToTicks(5, kRate25));
    REQUIRE(f.has_value());
    CHECK(frameIndexOf(*f) == 5);
    CHECK(f->format == PixelFormat::YUV444P10);
    VideoDecoderStats s = decoder->stats();
    INFO("decoded " << s.framesDecoded << ", seeks " << s.seeks);
    CHECK(s.framesDecoded >= 6);
    CHECK(s.framesConverted == 1);
    CHECK(s.framesDelivered == 1);
    // The same frame again: no second conversion.
    auto again = decoder->frameAt(framesToTicks(5, kRate25) + Ticks{1});
    REQUIRE(again.has_value());
    CHECK(frameIndexOf(*again) == 5);
    s = decoder->stats();
    CHECK(s.framesConverted == 1);
    CHECK(s.framesDelivered == 2);
}

TEST_CASE("Video: decode throughput, 1080p60 H.264", "[media][video][benchmark]") {
    if (!haveMedia("h264_1080p60_aac.mp4")) {
        SKIP("test clip was not generated");
    }
    struct Run {
        std::string label;
        DecoderOptions options;
    };
    std::vector<Run> runs;
    for (const std::string& backend : hardwareBackendOrder()) {
        DecoderOptions o;
        o.preferredBackend = backend;
        runs.push_back({backend, o});
    }
    DecoderOptions software;
    software.allowHardware = false;
    runs.push_back({"software", software});

    std::string report = "1080p60 H.264 High, 600 frames, sequential frameAt() incl. GPU->CPU transfer; "
                         "CPU = process CPU time (all threads) per frame\n";
    for (const Run& run : runs) {
        auto decoder = VideoDecoder::open(mediaPath("h264_1080p60_aac.mp4"), -1, run.options);
        REQUIRE(decoder.has_value());
        if ((*decoder)->backendName() != run.label) {
            report += std::format("  {:<9} unavailable (got {})\n", run.label, (*decoder)->backendName());
            continue;
        }
        const auto start = std::chrono::steady_clock::now();
        const double cpuStart = processCpuSeconds();
        for (std::int64_t i = 0; i < 600; ++i) {
            auto f = (*decoder)->frameAt(framesToTicks(i, kRate60));
            REQUIRE(f.has_value());
        }
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const double cpu = processCpuSeconds() - cpuStart;

        // Scrubbing: random frames across the clip (GOP 60, so each access decodes ~30 frames on average).
        std::mt19937 rng(7);
        std::uniform_int_distribution<std::int64_t> pick(0, 599);
        const auto randomStart = std::chrono::steady_clock::now();
        for (int i = 0; i < 40; ++i) {
            const std::int64_t target = pick(rng);
            auto f = (*decoder)->frameAt(framesToTicks(target, kRate60));
            REQUIRE(f.has_value());
            CHECK(frameIndexOf(*f) == target);
        }
        const double randomMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - randomStart)
                .count() /
            40.0;
        report += std::format("  {:<9} {:7.1f} fps   CPU {:5.2f} ms/frame   random access {:6.1f} ms\n",
                              run.label, 600.0 / seconds, cpu * 1000.0 / 600.0, randomMs);
    }
    std::printf("%s", report.c_str());
    std::ofstream(std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "media_decode_throughput.txt") << report;
    SUCCEED(report);
}
