// FrameImporter pool: textures are reused, retired only after the GPU is done, and nothing leaks over 1,000
// frames.

#include "RenderTestSupport.h"

#include <wabcut/render/Compositor.h>
#include <wabcut/render/FrameImporter.h>
#include <wabcut/render/OffscreenRenderer.h>

#include <catch2/catch_test_macros.hpp>

#if defined(_WIN32)
// clang-format off: psapi.h needs windows.h first
#include <windows.h>
#include <psapi.h>
// clang-format on
#endif

using namespace wabcut;
using namespace wabcut::render;
using namespace wabcut::render::test;
using media::PixelFormat;

namespace {

std::size_t privateBytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof counters;
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof counters)) {
        return counters.PrivateUsage;
    }
#endif
    return 0;
}

const ColorInfo kRec709{ColorMatrix::BT709, ColorRange::Limited, ColorPrimaries::BT709,
                        TransferFunction::BT709, ChromaLocation::Left};

} // namespace

TEST_CASE("Resident frames are not uploaded again", "[render][gpu][pool]") {
    auto renderer = makeRenderer();
    const media::VideoFrame frame =
        makeFrame(PixelFormat::NV12, 256, 128, kRec709, [](int x, int) { return Rgb{x / 256.0, 0.5, 0.25}; });
    auto composite = compositeOf({256, 128}, {layerOf(frame)});
    REQUIRE(renderer->render(composite).has_value());
    const FrameImporterStats first = renderer->compositor().importer().stats();
    CHECK(first.uploads == 1);
    REQUIRE(renderer->render(composite).has_value());
    REQUIRE(renderer->render(compositeOf({256, 128}, {layerOf(frame), layerOf(frame, {}, 0.5)})).has_value());
    const FrameImporterStats again = renderer->compositor().importer().stats();
    CHECK(again.uploads == 1); // the same frame (same owner) stays resident
    CHECK(again.residentHits == 3);
    CHECK(again.setsCreated == 1);
}

TEST_CASE("Texture pool reuses sets with no leak over 1,000 frames", "[render][gpu][pool]") {
    auto renderer = makeRenderer();
    const FrameImporter& importer = renderer->compositor().importer();
    auto alive = std::make_shared<std::atomic<int>>(0);
    constexpr int kFrames = 1000;
    constexpr int kLayers = 3;

    // Three distinct sources of different formats; every frame gets fresh owners (new identities), like a
    // decoder.
    const std::array<media::VideoFrame, kLayers> sources{
        makeFrame(PixelFormat::NV12, 1920, 1080, kRec709,
                  [](int x, int) { return Rgb{x / 1920.0, 0.5, 0.5}; }),
        makeFrame(PixelFormat::YUV420P, 1920, 1080, kRec709,
                  [](int, int y) { return Rgb{0.5, y / 1080.0, 0.5}; }),
        makeFrame(PixelFormat::P010, 1280, 720, kRec709,
                  [](int x, int y) { return Rgb{0.5, 0.5, (x + y) / 2000.0}; })};
    // A small ring of in-flight frames, like a decoder's output queue.
    std::vector<std::array<media::VideoFrame, kLayers>> ring(8);

    std::size_t bytesAt100 = 0;
    std::uint64_t setsAt100 = 0;
    for (int i = 0; i < kFrames; ++i) {
        auto& frames = ring[static_cast<std::size_t>(i) % ring.size()];
        for (std::size_t l = 0; l < kLayers; ++l) {
            frames[l] = withNewOwner(sources[l], alive); // the previous occupant is released here
        }
        std::vector<CompositeLayer> layers;
        LayerTransform small;
        small.scale = 0.5;
        layers.push_back(layerOf(frames[0]));
        layers.push_back(layerOf(frames[1], small, 0.7));
        layers.push_back(layerOf(frames[2], small, 0.5));
        auto timing = renderer->renderTimed(compositeOf({1920, 1080}, std::move(layers)));
        REQUIRE(timing.has_value());
        REQUIRE(timing->composite.layersDrawn == kLayers);
        if (i == 100) {
            bytesAt100 = privateBytes();
            setsAt100 = importer.stats().setsCreated;
        }
    }
    const FrameImporterStats stats = importer.stats();
    INFO("sets created " << stats.setsCreated << ", live " << stats.liveSets << ", retained "
                         << stats.retainedFrames << ", uploads " << stats.uploads << ", latency "
                         << importer.retireLatency());
    CHECK(stats.uploads == static_cast<std::uint64_t>(kFrames * kLayers));
    // Steady state: no new texture sets after warm-up, a bounded pool, bounded retained sources.
    CHECK(stats.setsCreated == setsAt100);
    CHECK(stats.liveSets <= static_cast<std::size_t>(kLayers * (importer.retireLatency() + 1)));
    CHECK(stats.retainedFrames <= static_cast<std::size_t>(kLayers * (importer.retireLatency() + 1)));
    CHECK(stats.poolExhausted == 0);
    // Frames the importer let go of were really freed: only the ring and the retained ones are alive.
    CHECK(alive->load() <= static_cast<int>(ring.size()) * kLayers + static_cast<int>(stats.retainedFrames));
    const std::size_t bytesAtEnd = privateBytes();
    if (bytesAt100 > 0) {
        const auto growth = static_cast<double>(bytesAtEnd) - static_cast<double>(bytesAt100);
        INFO("private bytes growth after warm-up: " << growth / (1024.0 * 1024.0) << " MiB");
        CHECK(growth <
              32.0 * 1024 * 1024); // the ring is ~8 x 3 frames; drivers may cache some staging memory
    }

    ring.clear();
    renderer.reset(); // releases the pool and the retained sources
    CHECK(alive->load() == 0);
}

TEST_CASE("Released resources bump the device generation", "[render][gpu][pool]") {
    auto renderer = makeRenderer();
    const media::VideoFrame frame =
        makeFrame(PixelFormat::YUV420P, 64, 64, kRec709, [](int, int) { return Rgb{0.5, 0.5, 0.5}; });
    REQUIRE(renderer->render(compositeOf({64, 64}, {layerOf(frame)})).has_value());
    FrameImporter& importer = renderer->compositor().importer();
    const std::uint64_t generation = importer.deviceGeneration();
    CHECK(importer.stats().liveSets == 1);
    renderer->compositor().releaseResources();
    CHECK(importer.deviceGeneration() == generation + 1);
    CHECK(importer.stats().liveSets == 0);
    // Rendering again re-imports (no stale residency).
    const std::uint64_t uploads = importer.stats().uploads;
    REQUIRE(renderer->render(compositeOf({64, 64}, {layerOf(frame)})).has_value());
    CHECK(importer.stats().uploads == uploads + 1);
}
