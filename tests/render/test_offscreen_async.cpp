// AsyncOffscreenRenderer: in-order delivery, bounded in-flight work (backpressure), clean stop.

#include "RenderTestSupport.h"

#include <wabcut/render/OffscreenRenderer.h>

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace wabcut;
using namespace wabcut::render;
using namespace wabcut::render::test;

TEST_CASE("Async offscreen rendering is ordered and backpressured", "[render][gpu][async]") {
    std::mutex mutex;
    std::vector<std::uint64_t> order;
    std::vector<int> firstPixel;
    AsyncOffscreenOptions options;
    options.maxInFlight = 2;
    options.renderer.alpha = OutputAlpha::Opaque;
    auto async = AsyncOffscreenRenderer::create(options, [&](const AsyncFrameResult& result) {
        // A slow consumer (an encoder): the producer must be held back.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::lock_guard lock(mutex);
        order.push_back(result.sequence);
        firstPixel.push_back(result.image ? result.image->rgba[0] : -1);
    });
    REQUIRE(async.has_value());
    INFO("backend " << (*async)->backendName());

    constexpr int kFrames = 12;
    for (int i = 0; i < kFrames; ++i) {
        const auto v = static_cast<std::uint8_t>(i * 20);
        const media::VideoFrame frame =
            makeRgbaFrame(64, 64, [v](int, int) { return std::array<std::uint8_t, 4>{v, 0, 0, 255}; });
        ColorPipeline srgb;
        srgb.exportTransform = OutputTransform::SRGB;
        auto sequence = (*async)->submit(compositeOf({64, 64}, {layerOf(frame)}, srgb));
        REQUIRE(sequence.has_value());
        CHECK(*sequence == static_cast<std::uint64_t>(i + 1));
    }
    (*async)->flush();
    const AsyncOffscreenStats stats = (*async)->stats();
    CHECK(stats.submitted == kFrames);
    CHECK(stats.delivered == kFrames);
    CHECK(stats.failed == 0);
    CHECK(stats.maxQueued <= options.maxInFlight);
    CHECK(stats.blockedSubmits > 0);
    {
        std::lock_guard lock(mutex);
        REQUIRE(order.size() == kFrames);
        for (int i = 0; i < kFrames; ++i) {
            CHECK(order[static_cast<std::size_t>(i)] == static_cast<std::uint64_t>(i + 1));
            CHECK(std::abs(firstPixel[static_cast<std::size_t>(i)] - i * 20) <= 1);
        }
    }
    // Non-blocking submit reports a full queue instead of waiting.
    (*async)->stop();
    CHECK_FALSE((*async)->trySubmit(compositeOf({64, 64}, {})).has_value());
    const auto afterStop = (*async)->submit(compositeOf({64, 64}, {}));
    REQUIRE_FALSE(afterStop.has_value());
    CHECK(afterStop.error().code == ErrorCode::Cancelled);
}
