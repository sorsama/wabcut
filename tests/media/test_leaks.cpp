#include "TestMedia.h"

#include <wabcut/media/AudioDecoder.h>
#include <wabcut/media/Probe.h>
#include <wabcut/media/VideoDecoder.h>

#include <algorithm>
#include <climits>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::media;
using namespace wabcut::media::test;

namespace {

/// Runs `cycle` 200 times after a warm-up and checks that what the decoders own does not grow with the cycle
/// count: live heap bytes (every host allocation of ours and FFmpeg's), GPU memory (surfaces, decoder objects,
/// CUDA allocations) and OS handles. Each is compared as the floor (lowest reading) of the first quarter of
/// the run against the floor of the last quarter: single readings jitter by a few MB (heap subsegments, frame
/// thread pools).
///
/// Process private bytes are checked differently. They also hold host-side pools GPU drivers manage on their
/// own: on NVIDIA, the CUDA path's private bytes swing by 200+ MB (e.g. 330 -> 560 -> 340 MB) with no change in
/// heap, GPU memory or handles, and grow faster while another process loads the GPU (2026-10: +265 MB within
/// one run while an e2e suite rendered). A cache levels off; a leak keeps growing. So when private bytes grew
/// by more than 128 MB, up to four more chunks of 50 cycles run, and the check passes once a chunk's floor is
/// no higher than the level before it (+16 MB): growth of more than ~0.3 MB per cycle that never stops fails.
struct GrowthReport {
    long long heapDelta = 0;
    long long gpuDelta = 0; ///< 0 when GPU memory cannot be read
    long long privateDelta = 0;
    bool privateOk = true; ///< below the bound, or levelled off within the extra chunks
    long long handlesDelta = 0;
    std::string text;
};

GrowthReport measureGrowth(const char* what, const std::function<void(int)>& cycle) {
    for (int i = 0; i < 10; ++i) { // warm-up: device creation, pools, lazily loaded DLLs
        cycle(i);
    }
    constexpr int kCycles = 200;
    constexpr int kQuarter = kCycles / 4;
    struct Floor {
        std::size_t heap = SIZE_MAX;
        std::size_t privateBytes = SIZE_MAX;
        long long gpu = LLONG_MAX;
        void take() {
            heap = std::min(heap, heapLiveBytes());
            privateBytes = std::min(privateBytes, processPrivateBytes());
            gpu = std::min(gpu, processGpuMemoryBytes());
        }
    } first, last;
    const std::size_t handlesBefore = processHandleCount();
    int cycles = 0;
    for (; cycles < kCycles; ++cycles) {
        cycle(cycles);
        if (cycles < kQuarter) {
            first.take();
        } else if (cycles >= kCycles - kQuarter) {
            last.take();
        }
    }
    const std::size_t handlesAfter = processHandleCount();
    const auto delta = [](auto a, auto b) { return static_cast<long long>(b) - static_cast<long long>(a); };
    const auto mb = [](long long bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); };

    GrowthReport r;
    r.heapDelta = delta(first.heap, last.heap);
    r.gpuDelta = first.gpu >= 0 && last.gpu >= 0 ? delta(first.gpu, last.gpu) : 0;
    r.privateDelta = delta(first.privateBytes, last.privateBytes);
    r.handlesDelta = delta(handlesBefore, handlesAfter);

    // Private bytes: plateau detection when they grew (see checkNoGrowth).
    constexpr long long kPrivateBound = 128LL * 1024 * 1024;
    constexpr long long kPlateauSlack = 16LL * 1024 * 1024;
    constexpr int kChunk = 50;
    std::string plateau;
    r.privateOk = r.privateDelta < kPrivateBound;
    long long level = static_cast<long long>(last.privateBytes);
    for (int chunk = 0; chunk < 4 && !r.privateOk; ++chunk) {
        std::size_t floor = SIZE_MAX;
        for (int i = 0; i < kChunk; ++i, ++cycles) {
            cycle(cycles);
            floor = std::min(floor, processPrivateBytes());
        }
        plateau += std::format(" {:.0f}", mb(static_cast<long long>(floor)));
        r.privateOk = static_cast<long long>(floor) <= level + kPlateauSlack;
        level = static_cast<long long>(floor);
    }
    r.text = std::format("{} over the last {} cycles: live heap floor {} -> {} ({}), GPU memory floor {:.1f} -> "
                         "{:.1f} MB, private bytes floor {:.1f} -> {:.1f} MB{}, handles {} -> {}",
                         what, kCycles - kQuarter, first.heap, last.heap, r.heapDelta, mb(first.gpu), mb(last.gpu),
                         mb(static_cast<long long>(first.privateBytes)),
                         mb(static_cast<long long>(last.privateBytes)),
                         plateau.empty() ? std::string() : ", then per 50 cycles (MB):" + plateau, handlesBefore,
                         handlesAfter);
    return r;
}

/// Runs `cycle` 200 times after a warm-up and checks that what the decoders own does not grow with the cycle
/// count: live heap bytes (every host allocation of ours and FFmpeg's), GPU memory (surfaces, decoder objects,
/// CUDA allocations) and OS handles. Each is compared as the floor (lowest reading) of the first quarter of
/// the run against the floor of the last quarter: single readings jitter by a few MB (heap subsegments, frame
/// thread pools).
///
/// Process private bytes are checked differently. They also hold host-side pools GPU drivers manage on their
/// own: on NVIDIA, the CUDA path's private bytes swing by 200+ MB (e.g. 330 -> 560 -> 340 MB) with no change in
/// heap, GPU memory or handles, and grow faster while another process loads the GPU (2026-10: +265 MB within
/// one run while an e2e suite rendered). A cache levels off; a leak keeps growing. So when private bytes grew
/// by more than 128 MB, up to four more chunks of 50 cycles run, and the check passes once a chunk's floor is
/// no higher than the level before it (+16 MB): growth of more than ~0.3 MB per cycle that never stops fails.
void checkNoGrowth(const char* what, const std::function<void(int)>& cycle) {
    const GrowthReport r = measureGrowth(what, cycle);
    INFO(r.text);
    // 8 MB over 150 cycles catches ~55 kB per cycle: a leaked codec context, frame pool or 1080p frame (3
    // MB).
    CHECK(r.heapDelta < 8LL * 1024 * 1024);
    // 64 MB over 150 cycles catches a leaked surface pool or decoder (tens of MB) in a few cycles and a leaked
    // 1080p surface (3 MB) per cycle; holding every 10th decoder grows it by ~2 GB per 100 cycles.
    CHECK(r.gpuDelta < 64LL * 1024 * 1024);
    CHECK(r.privateOk);
    CHECK(r.handlesDelta < 32);
}

} // namespace

TEST_CASE("Leaks: 200 open/decode/close cycles", "[media][leaks]") {
    if (!haveMedia("h264_1080p60_aac.mp4") || !haveMedia("vfr.mp4") || !haveMedia("ramp_48k.wav")) {
        SKIP("test clips were not generated");
    }
    if (heapLiveBytes() == 0) {
        SKIP("no heap probe on this platform");
    }
    {
        // The probe must see multi-megabyte blocks: decoded frames are that size and come from the same heap.
        const std::size_t before = heapLiveBytes();
        std::vector<std::uint8_t> block(std::size_t{8} << 20, 1);
        CHECK(heapLiveBytes() > before + (std::size_t{6} << 20));
        CHECK(block.back() == 1);
    }

    SECTION("hardware video decoder (1080p H.264)") {
        checkNoGrowth("hardware video", [](int i) {
            auto decoder = VideoDecoder::open(mediaPath("h264_1080p60_aac.mp4"));
            REQUIRE(decoder.has_value());
            if (gpuExpected()) {
                REQUIRE((*decoder)->isHardwareAccelerated());
            }
            for (std::int64_t f : {0, 1, 2}) {
                REQUIRE((*decoder)->frameAt(framesToTicks(f + i % 7 * 60, kRate60)).has_value());
            }
        });
    }
    SECTION("software video decoder with fallback probing (10-bit 4:2:2)") {
        checkNoGrowth("software video", [](int i) {
            auto decoder = VideoDecoder::open(mediaPath("h264_422_10bit.mp4"));
            REQUIRE(decoder.has_value());
            REQUIRE((*decoder)->frameAt(framesToTicks(i % 60, kRate30)).has_value());
        });
    }
    SECTION("audio decoder with resampling, and probe") {
        checkNoGrowth("audio + probe", [](int i) {
            auto decoder = AudioDecoder::open(mediaPath("h264_1080p60_aac.mp4"), -1, kRate44_1kHz, 2);
            REQUIRE(decoder.has_value());
            std::vector<float> buf(2048 * 2);
            REQUIRE((*decoder)->read(Ticks::fromSeconds(i % 9), buf).has_value());
            REQUIRE(probe(mediaPath("vfr.mp4")).has_value());
        });
    }
}

#if defined(_WIN32)
TEST_CASE("Leak check: the private-bytes plateau tells a driver cache from a leak", "[media][leaks]") {
    // Committed pages outside the C heap, like a driver's host-side pools: private bytes only.
    std::vector<void*> blocks;
    const auto commit = [&](std::size_t bytes) {
        void* p = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        REQUIRE(p != nullptr);
        std::memset(p, 1, bytes);
        blocks.push_back(p);
    };
    const auto release = [&] {
        for (void* p : blocks) {
            VirtualFree(p, 0, MEM_RELEASE);
        }
        blocks.clear();
    };
    constexpr std::size_t kMB = 1024 * 1024;

    // A cache: grows 2 MB per cycle for 80 cycles (160 MB), then stays.
    const GrowthReport cache = measureGrowth("cache", [&](int i) {
        if (i >= 10 && i < 90) {
            commit(2 * kMB);
        }
    });
    INFO(cache.text);
    CHECK(cache.privateDelta > 128LL * 1024 * 1024); // the plain bound alone would have failed
    CHECK(cache.privateOk);
    CHECK(cache.heapDelta < 8LL * 1024 * 1024);
    release();

    // A leak: 1 MB per cycle, forever.
    const GrowthReport leak = measureGrowth("leak", [&](int) { commit(kMB); });
    INFO(leak.text);
    CHECK_FALSE(leak.privateOk);
    release();
}
#endif
