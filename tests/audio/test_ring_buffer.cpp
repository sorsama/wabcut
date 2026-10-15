#include <wabcut/audio/SpscRingBuffer.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

using namespace wabcut::audio;

namespace {

// Frame i carries (i mod 2^22, i mod 2^22 + 0.5): exact in float, so the consumer can check every sample.
constexpr std::uint64_t kPatternMask = (1U << 22) - 1;
float patternL(std::uint64_t frame) { return static_cast<float>(frame & kPatternMask); }
float patternR(std::uint64_t frame) { return static_cast<float>(frame & kPatternMask) + 0.5f; }

void fill(std::vector<float>& buf, std::uint64_t firstFrame, std::size_t frames) {
    buf.resize(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        buf[2 * i] = patternL(firstFrame + i);
        buf[2 * i + 1] = patternR(firstFrame + i);
    }
}

} // namespace

TEST_CASE("Ring capacity rounds up to a power of two", "[audio][ring]") {
    CHECK(SpscFrameRing(1000, 2).capacityFrames() == 1024);
    CHECK(SpscFrameRing(1024, 2).capacityFrames() == 1024);
    CHECK(SpscFrameRing(0, 0).capacityFrames() == 1);
    CHECK(SpscFrameRing(0, 0).channels() == 1);
}

TEST_CASE("Ring preserves order across wrap-around and reports partial transfers", "[audio][ring]") {
    SpscFrameRing ring(8, 2);
    std::vector<float> in;
    std::vector<float> out(64, -1.0f);

    fill(in, 0, 6);
    REQUIRE(ring.write(in) == 6);
    CHECK(ring.readableFrames() == 6);
    CHECK(ring.writableFrames() == 2);

    REQUIRE(ring.read(std::span(out).first(4 * 2)) == 4);
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(out[2 * i] == patternL(i));
        CHECK(out[2 * i + 1] == patternR(i));
    }

    // 6 more frames only fit partially (2 queued, capacity 8): the write wraps and stops at 6 queued + ...
    fill(in, 6, 10);
    CHECK(ring.write(in) == 6); // 2 already queued + 6 = full
    CHECK(ring.writableFrames() == 0);
    CHECK(ring.write(in) == 0);

    REQUIRE(ring.read(out) == 8);
    for (std::size_t i = 0; i < 8; ++i) {
        CHECK(out[2 * i] == patternL(4 + i));
        CHECK(out[2 * i + 1] == patternR(4 + i));
    }
    CHECK(ring.read(out) == 0);
    CHECK(ring.totalWritten() == 12);
    CHECK(ring.totalRead() == 12);
}

TEST_CASE("Ring ignores a trailing partial frame and supports discard", "[audio][ring]") {
    SpscFrameRing ring(16, 2);
    const std::vector<float> threeSamples{1.0f, 2.0f, 3.0f};
    CHECK(ring.write(threeSamples) == 1);
    std::vector<float> out(3, -1.0f);
    CHECK(ring.read(out) == 1);
    CHECK(out[0] == 1.0f);
    CHECK(out[1] == 2.0f);
    CHECK(out[2] == -1.0f); // untouched

    std::vector<float> in;
    fill(in, 0, 10);
    REQUIRE(ring.write(in) == 10);
    CHECK(ring.discard(3) == 3);
    std::vector<float> one(2);
    REQUIRE(ring.read(one) == 1);
    CHECK(one[0] == patternL(3));
    CHECK(ring.discardAll() == 6);
    CHECK(ring.readableFrames() == 0);
    CHECK(ring.discard(5) == 0);
}

namespace {

void stress(std::size_t capacity, std::uint64_t totalFrames) {
    SpscFrameRing ring(capacity, 2);
    std::atomic<bool> failed{false};

    std::thread producer([&] {
        std::mt19937 rng(1234);
        std::uniform_int_distribution<std::size_t> chunk(1, 997);
        std::vector<float> buf;
        std::uint64_t next = 0;
        while (next < totalFrames) {
            const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(chunk(rng), totalFrames - next));
            fill(buf, next, want);
            std::size_t offset = 0;
            while (offset < want) {
                const std::size_t n = ring.write(std::span<const float>(buf).subspan(offset * 2));
                if (n == 0) {
                    std::this_thread::yield();
                }
                offset += n;
            }
            next += want;
        }
    });

    std::mt19937 rng(5678);
    std::uniform_int_distribution<std::size_t> chunk(1, 1031);
    std::vector<float> buf(1031 * 2);
    std::uint64_t expected = 0;
    while (expected < totalFrames && !failed.load()) {
        const std::size_t want = chunk(rng);
        const std::size_t n = ring.read(std::span(buf).first(want * 2));
        if (n == 0) {
            std::this_thread::yield();
            continue;
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (buf[2 * i] != patternL(expected + i) || buf[2 * i + 1] != patternR(expected + i)) {
                failed.store(true);
                break;
            }
        }
        expected += n;
    }
    producer.join();

    CHECK_FALSE(failed.load());
    CHECK(expected == totalFrames);           // nothing lost
    CHECK(ring.readableFrames() == 0);        // nothing duplicated
    CHECK(ring.totalWritten() == totalFrames);
    CHECK(ring.totalRead() == totalFrames);
}

} // namespace

TEST_CASE("Ring survives a multi-threaded stress test without loss or reordering", "[audio][ring][stress]") {
    SECTION("tiny ring: constant contention, wraps every few frames") { stress(64, 4'000'000); }
    SECTION("realistic ring") { stress(8192, 10'000'000); }
}
