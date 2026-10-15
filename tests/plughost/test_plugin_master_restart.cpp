// Review wave 5, A5-P2-7, the loop-wrap scenario of the review ("no zero runs at the wrap points"). HIDDEN ([.]): run it
// by hand with  wabcut_plughost_tests "[loop-wrap]".
//
// What A5-P2-7's fix covers is already tested in test_plugin_effects.cpp: a late master block plays the latency-aligned
// dry signal, flagged, and the next block in time fades back over one block ("A missed deadline plays its block dry
// and flags it; ..."), including the blocks the master's first callback (a restart: it primes the pipeline and plays
// at once) finds late ("A plugin that stalls on the master bus ...": silent == 0 from frame 0).
//
// Why hidden: a loop wrap may fail here for a reason outside that fix. TimelineAudioPlayer::renderMix reads the mix at
// `start + latency` (the master's pipeline, 4096 frames with a plugin) without the loop mapping, so in the last 85 ms
// before the loop end it asks the track rings for timeline times past the loop end. The rings hold the wrapped audio
// there (stamped from the loop start), which TrackStream::read drops as stale (skippedFrames), and after the wrap the
// restarted master finds it gone. If this test fails with skippedFrames / underrunFrames > 0 that is the cause; if
// only `silent` fails, the missed restart blocks are still silence.

#include "PlughostFixture.h"

#include "../playback/PlaybackFixture.h"

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace wabcut;
using namespace wabcut::playback;
using namespace wabcut::test;

namespace {

/// Waits (at most 10 s) until the helpers have done every block submitted so far (as test_plugin_effects.cpp): the next
/// callback finds the blocks it plays ready, so only a restart, which plays its first blocks in the callback that
/// primes it, can find them late.
void settle(const TestPlugins& plugins) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (const auto& instance : plugins.host->instances()) {
        plughost::InstanceShared& shared = instance->shared();
        const std::uint64_t submitted = shared.submitted.load(std::memory_order_acquire);
        while (instance->running() && shared.completed.load(std::memory_order_acquire) < submitted) {
            REQUIRE(std::chrono::steady_clock::now() < until);
            instance->waitDone(std::chrono::milliseconds(20));
        }
    }
}

} // namespace

TEST_CASE("A master plugin plays the dry signal and never silence where the master restarts at loop wraps",
          "[.loop-wrap][plughost][deadline]") {
    TestPlugins plugins;
    Project p;
    SyntheticSignal tone;
    tone.kind = SyntheticSignal::Kind::Sine;
    tone.frequency = 440.0;
    tone.amplitude = 0.5f;
    p.media('A', tone);
    p.addTrack({{'A', Ticks{}, ms(2000), Ticks{}}});
    Sequence seq = *p.state.sequences.front();
    seq.audioEffects = {pluginEffect(plugins.clap(), 1001, {{"p1", ParamValue(-6.0)}})};
    p.state = p.state.withSequence(std::make_shared<const Sequence>(std::move(seq)));
    Rig rig(p);
    rig.player->setLoop(TimeRange{ms(100), ms(200)}); // [100 ms, 300 ms)
    rig.player->feedOnce();
    rig.player->play();
    for (int block = 0; block < 100; ++block) { // 1 s: the start and four wraps
        settle(plugins);
        rig.pull();
    }
    const auto instance = plugins.effects->instanceFor(EffectInstanceId{1001});
    REQUIRE(instance);
    CHECK(instance->running());

    // Timeline frame of output frame k: straight to 300 ms, then the loop's 9600 frames from 100 ms over and over.
    const auto timelineAt = [](std::int64_t k) { return k < 14400 ? k : 4800 + (k - 14400) % 9600; };
    const float g = dbGain(-6.0);
    std::size_t silent = 0;
    std::size_t wrong = 0;
    std::size_t dry = 0;
    for (std::size_t k = 0; k < rig.stream.size() / 2; ++k) {
        const float want = p.feed->sample(p.mediaId('A'), 0, timelineAt(static_cast<std::int64_t>(k)), 0, k48k);
        if (std::fabs(want) < 1e-3f) {
            continue; // a zero crossing tells nothing
        }
        const float out = rig.stream[k * 2];
        if (out == 0.0f && rig.stream[k * 2 + 1] == 0.0f) {
            ++silent;
            continue;
        }
        dry += std::fabs(out - want) <= 1e-6f ? 1 : 0;
        const float lo = std::min(want, want * g) - 1e-6f;
        const float hi = std::max(want, want * g) + 1e-6f;
        wrong += out < lo || out > hi ? 1 : 0;
    }
    const TimelineAudioStats stats = rig.player->stats();
    INFO("skipped " << stats.skippedFrames << ", underruns " << stats.underrunFrames << ", resyncs " << stats.resyncs);
    CHECK(silent == 0); // before the fix: the blocks after the start and after every wrap
    CHECK(wrong == 0);  // the plugin's output, the dry signal where it was late, or the fade between them
    CHECK(stats.underrunFrames == 0);
    CHECK(stats.skippedFrames == 0);
    CHECK(plughost::PluginHost::realtimeWaits() == 0);
    WARN("master restarts: " << instance->missedBlocks() << " blocks missed (" << dry << " frames dry) in 1 s with 4 loop wraps");
}
