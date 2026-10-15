#include "TestMedia.h"

#include <wabcut/media/FrameCache.h>
#include <wabcut/media/VideoDecoder.h>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <random>
#include <thread>
#include <vector>

using namespace wabcut;
using namespace wabcut::media;
using namespace wabcut::media::test;

namespace {

/// An RGBA8 CPU frame backed by a plain vector; byteSize() == width * height * 4.
FrameHandle cpuFrame(int width, int height, std::uint8_t fill = 0) {
    auto pixels =
        std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(width) * height * 4, fill);
    FrameHandle f;
    f.width = width;
    f.height = height;
    f.format = PixelFormat::RGBA8;
    CpuPlanes cpu;
    cpu.planes[0] = Plane{pixels->data(), width * 4, width, height};
    cpu.owner = pixels;
    f.payload = cpu;
    return f;
}

/// A (reserved) native payload; byteSize() estimates the surface: width * height * 4 for RGBA8.
FrameHandle gpuFrame(int width, int height) {
    FrameHandle f;
    f.width = width;
    f.height = height;
    f.format = PixelFormat::RGBA8;
    D3D11Texture texture;
    texture.owner = std::make_shared<int>(0);
    texture.device = DeviceToken{7, 1};
    f.payload = texture;
    return f;
}

constexpr std::size_t kFrameBytes = 100 * 100 * 4; // 40 000

FrameKey key(std::int64_t pts, std::uint64_t fingerprint = 1) {
    return FrameKey{SourceIdentity{fingerprint, 0}, 0, pts};
}

} // namespace

TEST_CASE("FrameCache: least recently used frames are evicted to stay within the budget", "[media][cache]") {
    FrameCache cache(100'000); // room for two 40 kB frames
    cache.put(key(10), cpuFrame(100, 100, 1));
    cache.put(key(11), cpuFrame(100, 100, 2));
    CHECK(cache.stats().cpuBytes == 2 * kFrameBytes);
    CHECK(cache.stats().evictions == 0);

    cache.put(key(12), cpuFrame(100, 100, 3)); // evicts 10 (LRU)
    CHECK_FALSE(cache.contains(key(10)));
    CHECK(cache.contains(key(11)));
    CHECK(cache.contains(key(12)));
    CHECK(cache.stats().evictions == 1);
    CHECK(cache.stats().cpuBytes <= cache.stats().budget.cpuBytes);

    REQUIRE(cache.get(key(11)).has_value());      // 11 becomes most recently used
    cache.put(key(10, 2), cpuFrame(100, 100, 4)); // same pts, other source: evicts 12, not 11
    CHECK(cache.contains(key(11)));
    CHECK_FALSE(cache.contains(key(12)));
    auto other = cache.get(key(10, 2));
    REQUIRE(other.has_value());
    CHECK(other->cpu()->planes[0].data[0] == 4);

    const FrameCacheStats s = cache.stats();
    CHECK(s.entries == 2);
    CHECK(s.insertions == 4);
    CHECK(s.evictions == 2);
    CHECK(s.hits == 2);
    CHECK(s.misses == 0);
}

TEST_CASE("FrameCache: every key component separates entries", "[media][cache]") {
    FrameCache cache(10 * kFrameBytes);
    const FrameKey base{SourceIdentity{0xabc, 1}, 0, 512, 0, DeviceToken{}};
    cache.put(base, cpuFrame(100, 100, 1));
    FrameKey generation = base;
    generation.source.generation = 2; // relinked media
    FrameKey stream = base;
    stream.stream = 1;
    FrameKey pts = base;
    pts.pts = 768;
    FrameKey proxy = base;
    proxy.representation = 1;
    FrameKey device = base;
    device.decode = DeviceToken{7, 1}; // the same frame read back from a GPU decoder
    for (const FrameKey& k : {generation, stream, pts, proxy, device}) {
        CHECK_FALSE(cache.get(k).has_value());
    }
    CHECK(cache.get(base).has_value());

    // frameKeyOf() reads the identity straight off the frame.
    FrameHandle f = cpuFrame(10, 10);
    f.source = SourceIdentity{0xabc, 1};
    f.streamPts = 512;
    CHECK(frameKeyOf(f, 0) == base);
}

TEST_CASE("FrameCache: CPU and GPU frames have separate budgets", "[media][cache]") {
    FrameCache cache(FrameCacheBudget{2 * kFrameBytes, 3 * kFrameBytes});
    for (std::int64_t i = 0; i < 3; ++i) {
        cache.put(key(i), cpuFrame(100, 100));
        cache.put(key(100 + i), gpuFrame(100, 100));
    }
    const FrameCacheStats s = cache.stats();
    CHECK(s.cpuBytes == 2 * kFrameBytes); // one CPU frame evicted
    CHECK(s.gpuBytes == 3 * kFrameBytes); // all GPU frames fit
    CHECK_FALSE(cache.contains(key(0)));
    CHECK(cache.contains(key(100)));
    CHECK(s.evictions == 1);

    cache.put(key(103), gpuFrame(100, 100)); // GPU pressure evicts the oldest GPU frame, never a CPU one
    CHECK_FALSE(cache.contains(key(100)));
    CHECK(cache.contains(key(1)));
    CHECK(cache.contains(key(2)));

    FrameCache cpuOnly(10 * kFrameBytes); // no GPU budget: native frames are refused, not stored
    cpuOnly.put(key(1), gpuFrame(100, 100));
    CHECK(cpuOnly.stats().rejected == 1);
}

TEST_CASE("FrameCache: pinned frames are never evicted", "[media][cache]") {
    FrameCache cache(2 * kFrameBytes);
    cache.put(key(1), cpuFrame(100, 100, 1));
    cache.put(key(2), cpuFrame(100, 100, 2));
    FramePin pin = cache.pin(key(1));
    REQUIRE(pin);
    CHECK(pin.frame().cpu()->planes[0].data[0] == 1);
    CHECK(cache.stats().pinned == 1);
    REQUIRE(cache.get(key(2)).has_value()); // 1 is now the least recently used...

    cache.put(key(3), cpuFrame(100, 100, 3)); // ...but pinned, so 2 goes
    CHECK(cache.contains(key(1)));
    CHECK_FALSE(cache.contains(key(2)));

    // With 1 and 3 pinned, a newcomer is the only evictable entry.
    FramePin pin3 = cache.pin(key(3));
    cache.put(key(4), cpuFrame(100, 100, 4));
    CHECK_FALSE(cache.contains(key(4)));
    CHECK(cache.stats().cpuBytes == 2 * kFrameBytes);
    cache.erase(key(1)); // erase skips pinned entries
    CHECK(cache.contains(key(1)));

    FramePin moved = std::move(pin);
    CHECK_FALSE(pin);
    CHECK(moved);
    moved.release();
    pin3.release();
    CHECK(cache.stats().pinned == 0);
    CHECK_FALSE(cache.pin(key(99))); // miss: empty pin

    // Releasing the last pin of an over-budget cache evicts right away.
    {
        FramePin a = cache.pin(key(1));
        FramePin b = cache.pin(key(3));
        cache.setBudget(FrameCacheBudget{kFrameBytes, 0});
        CHECK(cache.stats().cpuBytes == 2 * kFrameBytes); // held over budget by the pins
    }
    CHECK(cache.stats().cpuBytes == kFrameBytes);
}

TEST_CASE("FrameCache: in-flight reservations deduplicate decodes and are never evicted", "[media][cache]") {
    FrameCache cache(2 * kFrameBytes);
    REQUIRE(cache.beginDecode(key(1), kFrameBytes));
    CHECK(cache.isInFlight(key(1)));
    CHECK_FALSE(cache.beginDecode(key(1), kFrameBytes)); // a second requester waits instead of decoding again
    CHECK_FALSE(cache.get(key(1)).has_value());          // not readable yet
    CHECK_FALSE(cache.contains(key(1)));
    CHECK(cache.stats().inFlight == 1);
    CHECK(cache.stats().cpuBytes == kFrameBytes); // reserved bytes count

    cache.put(key(2), cpuFrame(100, 100));
    cache.put(key(3), cpuFrame(100, 100)); // over budget: evicts 2, never the reservation
    CHECK(cache.isInFlight(key(1)));
    CHECK_FALSE(cache.contains(key(2)));

    cache.put(key(1), cpuFrame(100, 100, 9)); // completes the reservation
    CHECK_FALSE(cache.isInFlight(key(1)));
    CHECK(cache.get(key(1))->cpu()->planes[0].data[0] == 9);
    CHECK(cache.stats().inFlight == 0);

    REQUIRE(cache.beginDecode(key(5), kFrameBytes));
    cache.abortDecode(key(5));
    CHECK_FALSE(cache.isInFlight(key(5)));
    CHECK(cache.stats().inFlight == 0);
    CHECK_FALSE(cache.beginDecode(key(1), kFrameBytes)); // already cached
}

TEST_CASE("FrameCache: statistics and maintenance", "[media][cache]") {
    FrameCache cache(1'000'000);
    CHECK_FALSE(cache.get(key(0)).has_value());
    cache.put(key(0), cpuFrame(100, 100));
    CHECK(cache.get(key(0)).has_value());
    CHECK(cache.get(key(0)).has_value());
    const FrameCacheStats s = cache.stats();
    CHECK(s.hits == 2);
    CHECK(s.misses == 1);

    cache.put(key(0), cpuFrame(50, 50)); // replacing does not double-count
    CHECK(cache.stats().cpuBytes == std::size_t{50} * 50 * 4);
    CHECK(cache.stats().entries == 1);

    for (std::int64_t i = 1; i <= 5; ++i) {
        cache.put(key(i, 7), cpuFrame(10, 10));
    }
    cache.eraseSource(7);
    CHECK(cache.stats().entries == 1);
    cache.erase(key(0));
    CHECK(cache.stats().entries == 0);
    CHECK(cache.stats().cpuBytes == 0);

    FrameCache tiny(1000); // a frame bigger than the whole budget is refused instead of flushing everything
    tiny.put(key(0), cpuFrame(100, 100));
    CHECK(tiny.stats().rejected == 1);
    CHECK(tiny.stats().entries == 0);

    for (std::int64_t i = 0; i < 10; ++i) {
        cache.put(key(100 + i), cpuFrame(100, 100));
    }
    cache.setBudget(FrameCacheBudget{3 * kFrameBytes, 0}); // shrinking evicts immediately
    CHECK(cache.stats().entries == 3);
    CHECK(cache.contains(key(109)));
    CHECK_FALSE(cache.contains(key(106)));
    cache.resetCounters();
    CHECK(cache.stats().hits == 0);
    CHECK(cache.stats().evictions == 0);
    cache.clear();
    CHECK(cache.stats().cpuBytes == 0);

    CHECK(mediaKeyFromPath("C:/media/a.mp4") == mediaKeyFromPath("C:/media/a.mp4"));
    CHECK(mediaKeyFromPath("C:/media/a.mp4") != mediaKeyFromPath("C:/media/b.mp4"));
}

TEST_CASE("FrameCache: evicted frames stay valid while referenced", "[media][cache]") {
    FrameCache cache(kFrameBytes);
    cache.put(key(0), cpuFrame(100, 100, 42));
    auto held = cache.get(key(0));
    REQUIRE(held.has_value());
    cache.put(key(1), cpuFrame(100, 100, 7)); // evicts frame 0
    CHECK_FALSE(cache.contains(key(0)));
    CHECK(held->cpu()->planes[0].data[0] == 42);
    CHECK(held->cpu()->owner.use_count() == 1);
}

TEST_CASE("FrameCache: concurrent readers, writers and pins", "[media][cache]") {
    FrameCache cache(20 * kFrameBytes);
    std::atomic<std::uint64_t> gets{0};
    std::atomic<std::uint64_t> corrupt{0}; // Catch2 assertions are not thread-safe: count, check after join
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937 rng(static_cast<unsigned>(t));
            std::uniform_int_distribution<std::int64_t> frame(0, 63);
            for (int i = 0; i < 5000; ++i) {
                const FrameKey k = key(frame(rng), static_cast<std::uint64_t>(t % 2 + 1));
                const auto expected = static_cast<std::uint8_t>(k.pts);
                if (i % 3 == 0) {
                    cache.put(k, cpuFrame(100, 100, expected));
                } else if (i % 7 == 0) {
                    FramePin p = cache.pin(k);
                    ++gets;
                    if (p && p.frame().cpu()->planes[0].data[0] != expected) {
                        ++corrupt;
                    }
                } else {
                    auto f = cache.get(k);
                    ++gets;
                    if (f && f->cpu()->planes[0].data[0] != expected) {
                        ++corrupt;
                    }
                }
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    CHECK(corrupt.load() == 0);
    const FrameCacheStats s = cache.stats();
    CHECK(s.hits + s.misses == gets.load());
    CHECK(s.cpuBytes <= s.budget.cpuBytes);
    CHECK(s.cpuBytes == s.entries * kFrameBytes);
    CHECK(s.pinned == 0);
}

TEST_CASE("FrameCache: holds real decoded frames keyed by stream PTS", "[media][cache]") {
    if (!haveMedia("h264_1080p60_aac.mp4")) {
        SKIP("test clip was not generated");
    }
    DecoderOptions options;
    options.allowHardware = false;
    auto decoder = VideoDecoder::open(mediaPath("h264_1080p60_aac.mp4"), -1, options);
    REQUIRE(decoder.has_value());

    auto first = (*decoder)->frameAt(Ticks{0});
    REQUIRE(first.has_value());
    FrameCache cache(first->byteSize() * 4); // four 1080p frames
    for (std::int64_t i = 0; i < 10; ++i) {
        auto f = (*decoder)->frameAt(framesToTicks(i, kRate60));
        REQUIRE(f.has_value());
        cache.put(frameKeyOf(*f, 0), *f);
    }
    const FrameCacheStats s = cache.stats();
    CHECK(s.entries == 4);
    CHECK(s.evictions == 6);
    CHECK(s.cpuBytes <= s.budget.cpuBytes);

    // Another decoder (its own session, its own seeks) finds the same frames under the same keys.
    auto second = VideoDecoder::open(mediaPath("h264_1080p60_aac.mp4"), -1, options);
    REQUIRE(second.has_value());
    for (std::int64_t i = 9; i >= 6; --i) {
        auto f = (*second)->frameAt(framesToTicks(i, kRate60));
        REQUIRE(f.has_value());
        auto cached = cache.get(frameKeyOf(*f, 0));
        REQUIRE(cached.has_value());
        CHECK(frameIndexOf(*cached) == i);
    }
    auto evicted = (*second)->frameAt(framesToTicks(5, kRate60));
    REQUIRE(evicted.has_value());
    CHECK_FALSE(cache.get(frameKeyOf(*evicted, 0)).has_value());
}
