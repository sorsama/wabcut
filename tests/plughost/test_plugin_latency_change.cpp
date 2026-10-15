// Review wave 5, A5-P2-4: a plugin that changes its latency while it plays (a linear-phase EQ or a lookahead limiter
// switched from its own editor) stays aligned, in export and in preview. The test plugin's "Latency" parameter (p2)
// does what such a plugin does: its sound moves by the new latency at once and it asks the host for a restart, after
// which it reports the new value.
//
// Before the fix the helper only forwarded the restart request as an event: the shared memory kept the latency the
// instance was created with (0), the clip's processor went on compensating that, and from the change on the clip played
// 40 frames late (its clicks at 24040 and 48040) until something rebuilt the processor. Now the helper restarts the
// plugin and publishes the new latency, the processor goes stale and the clip's chain rebuilds it around the same
// instance, compensating the new value: the clicks land where they lie on the timeline.

#include "PlughostFixture.h"

#include "../playback/PlaybackFixture.h"

#include <wabcut/playback/audio/OfflineMixdown.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <span>
#include <thread>

using namespace wabcut;
using namespace wabcut::playback;
using namespace wabcut::test;

namespace {

constexpr int kNewLatency = 40;

/// A sawtooth at 2 Hz, 0.5: every sample of a half second has its own value, and it jumps from +0.5 to -0.5 (a click)
/// at every half second (source frames 24000, 48000, ...).
SyntheticSignal clicks() {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Ramp;
    s.frequency = 2.0;
    s.amplitude = 0.5f;
    return s;
}

/// A1: one clip of clicks() from 0 for 1.5 s with the CLAP test gain at -6 dB (effect `id`) whose latency is 0 until
/// 200 ms and kNewLatency frames from then on.
void latencyProject(Project& p, const TestPlugins& plugins, std::uint64_t id) {
    p.media('A', clicks());
    p.addTrack({{'A', Ticks{}, ms(1500), Ticks{}}});
    const Curve latency{{Keyframe{Ticks{}, 0.0, Interpolation::Hold},
                         Keyframe{ms(200), static_cast<double>(kNewLatency), Interpolation::Hold}}};
    Track track = p.track(0);
    track.clips.mutableAt(0).setEffects(
        {pluginEffect(plugins.clap(), id, {{"p1", ParamValue(-6.0)}, {"p2", ParamValue(latency)}}, clapState(0.0, 0.0))});
    p.state = Project::withTrack(p.state, 0, track);
}

/// Waits (at most 10 s) until the helper has restarted the plugin and publishes `latency` (frames) as its latency.
bool waitForLatency(const plughost::PluginInstance& instance, int latency) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (instance.latency() != latency) {
        if (!instance.running() || std::chrono::steady_clock::now() > until) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

/// Frames in [from, to) of the interleaved stereo `out` (left channel) that are not the clip's source at that very
/// frame times `g`.
std::size_t misaligned(const Project& p, const std::vector<float>& out, std::int64_t from, std::int64_t to, float g) {
    std::size_t wrong = 0;
    for (std::int64_t k = from; k < to; ++k) {
        const float expected = p.feed->sample(p.mediaId('A'), 0, k, 0, k48k) * g;
        wrong += std::fabs(out[static_cast<std::size_t>(k) * 2] - expected) > 1e-6f ? 1 : 0;
    }
    return wrong;
}

/// The output frame in [from, to) with the largest step from the frame before it: where a click lands.
std::int64_t clickAt(const std::vector<float>& out, std::int64_t from, std::int64_t to) {
    std::int64_t best = from;
    float largest = -1.0f;
    for (std::int64_t k = std::max<std::int64_t>(from, 1); k < to; ++k) {
        const float step = std::fabs(out[static_cast<std::size_t>(k) * 2] - out[static_cast<std::size_t>(k - 1) * 2]);
        if (step > largest) {
            largest = step;
            best = k;
        }
    }
    return best;
}

} // namespace

TEST_CASE("A plugin that changes its latency while it plays stays aligned in export and preview", "[plughost][latency][export]") {
    TestPlugins plugins;
    const float g = dbGain(-6.0);

    SECTION("export (OfflineMixdown)") {
        Project p;
        latencyProject(p, plugins, 701);
        OfflineMixdownOptions options;
        options.renderer.edgeFade = Ticks{};
        options.project = std::make_shared<const ProjectState>(p.state);
        OfflineMixdown mix(p.state.sequences.front(), 2, 48000, Ticks{}, *p.feed, options);
        std::vector<float> out(static_cast<std::size_t>(48 * 1300) * 2);
        // The first 300 ms: the change (at 200 ms) has reached the plugin, which asked for its restart.
        const std::size_t first = 48 * 300;
        mix.render(std::span<float>(out.data(), first * 2));
        const auto instance = plugins.effects->instanceFor(EffectInstanceId{701});
        REQUIRE(instance);
        // The helper restarts the plugin and publishes the latency it reports now (before the fix: never; this waited the
        // whole 10 s and the latency stayed 0).
        CHECK(waitForLatency(*instance, kNewLatency));
        CHECK(instance->latencyGeneration() >= 1);
        // The rest: the clip's chain finds its processor stale at the next block and rebuilds it around the instance.
        mix.render(std::span<float>(out.data() + first * 2, out.size() - first * 2));
        CHECK(plugins.effects->instanceFor(EffectInstanceId{701}) == instance); // the same instance, not a new one
        CHECK(instance->running());
        CHECK(plugins.host->instances().size() == 1);
        // Before the change, and from the rebuild on: every frame is the source at that very frame, at -6 dB.
        CHECK(misaligned(p, out, 0, 9000, g) == 0);
        CHECK(misaligned(p, out, 48 * 400, 48 * 1300, g) == 0);
        // The clicks land where they lie on the timeline (before the fix: 40 frames late).
        CHECK(clickAt(out, 48 * 450, 48 * 550) == 24000);
        CHECK(clickAt(out, 48 * 950, 48 * 1050) == 48000);
    }

    SECTION("preview (the player)") {
        Project p;
        latencyProject(p, plugins, 702);
        Rig rig(p);
        rig.player->play();
        // 300 ms played; the feeder has rendered 250 ms further, past the change at 200 ms.
        for (int block = 0; block < 30; ++block) {
            rig.pull();
        }
        const auto instance = plugins.effects->instanceFor(EffectInstanceId{702});
        REQUIRE(instance);
        CHECK(waitForLatency(*instance, kNewLatency));
        // Up to 1.3 s: the feeder's next pass rebuilds the clip's processor (at most 550 ms into the timeline).
        for (int block = 0; block < 100; ++block) {
            rig.pull();
        }
        CHECK(instance->running());
        CHECK(plugins.effects->instanceFor(EffectInstanceId{702}) == instance);
        CHECK(misaligned(p, rig.stream, 0, 9000, g) == 0);
        CHECK(misaligned(p, rig.stream, 48 * 700, 48 * 1300, g) == 0);
        CHECK(clickAt(rig.stream, 48 * 950, 48 * 1050) == 48000);
        CHECK(rig.player->stats().underrunFrames == 0);
    }
}
