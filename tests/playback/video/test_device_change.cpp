// Zero-copy across a device change (Codex review P2-11): the program monitor's QRhi is recreated (device reset,
// window moved to another adapter) and shares a new D3D11 device with the decoders. The scheduler must notice,
// drop what was decoded on the old device (sessions, cached surfaces, composites) and show the paused frame again,
// decoded on the new device, without anyone seeking.

#include "PlaybackTestSupport.h"

#include <wabcut/media/VideoDecoder.h>
#include <wabcut/playback/video/PlaybackController.h>
#include <wabcut/render/OffscreenRenderer.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

using namespace wabcut;
using namespace wabcut::playback;
using namespace wabcut::playback::test;
using namespace std::chrono_literals;

namespace {

constexpr const char* kClip = "h264_1080p60_aac.mp4"; // 8-bit 4:2:0: d3d11va decodes it

std::shared_ptr<const render::PreparedComposite> waitForComposite(
    render::CompositeMailbox& mailbox, const std::function<bool(const render::PreparedComposite&)>& wanted,
    std::chrono::milliseconds timeout = 5000ms) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(16ms);
        const auto sel = mailbox.acquire(steadyNowNs() + 16'666'667);
        if (sel.composite && wanted(*sel.composite)) {
            return sel.composite;
        }
    }
    return nullptr;
}

struct ResetSharedDevice {
    ~ResetSharedDevice() { (void)media::setSharedD3D11Device(nullptr, nullptr); }
};

} // namespace

TEST_CASE("Zero-copy: a new shared device re-renders the paused frame on it", "[playback][zerocopy][gpu]") {
    if (!haveMedia(kClip)) {
        SKIP("test media not generated");
    }
    ResetSharedDevice reset;
    auto first = render::OffscreenRenderer::create({.role = render::OutputRole::Display, .shareDeviceWithDecoders = true});
    REQUIRE(first);
    const media::DeviceToken deviceA = media::sharedD3D11Device();
    if (deviceA.isCpu()) {
        SKIP("no shared D3D11 device here (zero-copy is Windows + D3D11 only)");
    }

    TestProject project = makeProject({{kClip, 0, 0, 120, 0}});
    EditSession session(project.state);
    PlaybackOptions options;
    options.scheduler.zeroCopy = true;
    PlaybackController controller(options);
    controller.setSession(&session);
    controller.setSequence(project.sequence);
    controller.seek(framesToTicks(30, kRate60));

    const auto onA = waitForComposite(*controller.mailbox(), [&](const render::PreparedComposite& c) {
        return c.targetTime == framesToTicks(30, kRate60) && c.layers.size() == 1 && c.layers.front().frame.isValid();
    });
    REQUIRE(onA);
    if (onA->layers.front().frame.payloadKind() == media::PayloadKind::CpuPlanes) {
        SKIP("this clip did not decode to native surfaces here");
    }
    CHECK(onA->layers.front().frame.device() == deviceA);
    auto pictureA = (*first)->render(onA, QSize(480, 270));
    REQUIRE(pictureA);

    // A second renderer (a new QRhi, another D3D11 device) shares its device: the old surfaces are unusable there.
    auto second = render::OffscreenRenderer::create({.role = render::OutputRole::Display, .shareDeviceWithDecoders = true});
    REQUIRE(second);
    const media::DeviceToken deviceB = media::sharedD3D11Device();
    REQUIRE(deviceB != deviceA);
    auto timedOld = (*second)->renderTimed(onA, QSize(480, 270));
    REQUIRE(timedOld);
    CHECK(timedOld->composite.layersDrawn == 0); // the importer refuses another device's surface

    // Still paused, nobody seeks: the scheduler notices the device change and decodes the frame again on B.
    const auto onB = waitForComposite(*controller.mailbox(), [&](const render::PreparedComposite& c) {
        return c.targetTime == framesToTicks(30, kRate60) && c.layers.size() == 1 && c.layers.front().frame.isValid() &&
               c.layers.front().frame.device() == deviceB;
    });
    REQUIRE(onB);
    CHECK(onB->seekEpoch > onA->seekEpoch);
    auto timedNew = (*second)->renderTimed(onB, QSize(480, 270));
    REQUIRE(timedNew);
    CHECK(timedNew->composite.layersDrawn == 1);
    auto pictureB = (*second)->render(onB, QSize(480, 270));
    REQUIRE(pictureB);
    // Same picture on both devices (same frame, same compositor).
    double se = 0.0;
    for (std::size_t i = 0; i < pictureA->rgba.size(); ++i) {
        const double d = static_cast<double>(pictureA->rgba[i]) - pictureB->rgba[i];
        se += d * d;
    }
    CHECK(se / static_cast<double>(pictureA->rgba.size()) < 1.0);
    const SchedulerStats stats = controller.scheduler().stats();
    CHECK(stats.deviceChanges == 1);

    // Playing on: every frame comes from the new device.
    controller.play();
    const auto playing = waitForComposite(*controller.mailbox(), [&](const render::PreparedComposite& c) {
        return c.targetTime > framesToTicks(40, kRate60) && c.layers.size() == 1;
    });
    controller.pause();
    REQUIRE(playing);
    CHECK(playing->layers.front().frame.device() == deviceB);
}
