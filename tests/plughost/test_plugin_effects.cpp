// Plugin effects in the timeline (A.6): on clip, track and master stacks through the player (preview) and
// OfflineMixdown (export), sample for sample alike, and aligned to the sample by the plugin delay compensation (the
// test gives the helper its time between callbacks, so it holds on a loaded computer); a plugin that stalls on the
// master bus costs the dry signal for its late blocks, each flagged, never a wait in the audio callback; per-instance
// overhead with 10 instances.

#include "PlughostFixture.h"

#include "../playback/PlaybackFixture.h"

#include <wabcut/playback/audio/OfflineMixdown.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

using namespace wabcut;
using namespace wabcut::playback;
using namespace wabcut::test;

namespace {

SyntheticSignal tone(double hz, float amplitude) {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Sine;
    s.frequency = hz;
    s.amplitude = amplitude;
    return s;
}

std::vector<float> exported(const Project& p, std::size_t frames) {
    OfflineMixdownOptions options;
    options.renderer.edgeFade = Ticks{};
    options.project = std::make_shared<const ProjectState>(p.state);
    OfflineMixdown mix(p.state.sequences.front(), 2, 48000, Ticks{}, *p.feed, options);
    std::vector<float> out(frames * 2);
    for (std::size_t done = 0; done < frames;) {
        const std::size_t n = std::min<std::size_t>(frames - done, 300 + done % 777);
        mix.render(std::span<float>(out.data() + done * 2, n * 2));
        done += n;
    }
    return out;
}

/// The test's generous deadline: waits (at most 10 s) until the helpers have done every block submitted so far, so the
/// next callback finds the blocks it plays ready however loaded the computer is. The callback itself never waits.
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

std::int64_t firstMismatch(const std::vector<float>& a, const std::vector<float>& b, std::size_t samples) {
    for (std::size_t i = 0; i < samples; ++i) {
        if (a[i] != b[i]) {
            return static_cast<std::int64_t>(i);
        }
    }
    return -1;
}

/// A stream played through a master plugin (gain `g`, no latency of its own) that may miss its deadlines, block by
/// block. The master chain was primed with its latency (the pipeline), so output frame f is the plugin's block
/// f / kBlockFrames: the plugin's output (`dry(f) * g`), or while that block is late the dry signal (`dry(f)`), and in the
/// first block in time after a late one a fade from the dry signal to the plugin's.
struct BlockAudit {
    std::uint64_t dryBlocks = 0;  ///< blocks that played the dry signal throughout (late)
    std::uint64_t wetBlocks = 0;  ///< blocks that played the plugin's output throughout
    std::uint64_t fadeBlocks = 0; ///< blocks between the two (fading back in)
    std::size_t silent = 0;       ///< frames of silence where the tone sounds
    std::size_t wrong = 0;        ///< frames neither dry, the plugin's, nor between the two
};

template <class Dry>
BlockAudit auditBlocks(const std::vector<float>& played, Dry dry, float g) {
    BlockAudit audit;
    const std::size_t frames = played.size() / 2;
    const std::size_t blockFrames = plughost::kBlockFrames;
    for (std::size_t b = 0; b * blockFrames < frames; ++b) { // the last one played in part
        bool any = false;
        bool allDry = true;
        bool allWet = true;
        for (std::size_t k = b * blockFrames; k < std::min((b + 1) * blockFrames, frames); ++k) {
            const float want = dry(static_cast<std::int64_t>(k));
            if (std::fabs(want) < 1e-3f) {
                continue; // a zero crossing tells nothing
            }
            any = true;
            const float out = played[k * 2];
            if (out == 0.0f && played[k * 2 + 1] == 0.0f) {
                ++audit.silent;
                allDry = false;
                allWet = false;
                continue;
            }
            allDry = allDry && std::fabs(out - want) <= 1e-6f;
            allWet = allWet && std::fabs(out - want * g) <= 1e-6f;
            const float lo = std::min(want, want * g) - 1e-6f;
            const float hi = std::max(want, want * g) + 1e-6f;
            audit.wrong += out < lo || out > hi ? 1 : 0;
        }
        if (!any) {
            continue;
        }
        if (allWet) {
            ++audit.wetBlocks;
        } else if (allDry) {
            ++audit.dryBlocks;
        } else {
            ++audit.fadeBlocks;
        }
    }
    return audit;
}

/// A project with tone 'A' on A1: a clip with the CLAP gain (-6 dB, 100 frames of latency) and the VST3 gain on the
/// track (-3 dB, 37 frames), optionally the CLAP gain on the master too (-2 dB, 50 frames).
void pluginProject(Project& p, const TestPlugins& plugins, bool master) {
    p.media('A', tone(440.0, 0.5f));
    p.addTrack({{'A', ms(100), ms(800), Ticks{}}});
    Track track = p.track(0);
    track.clips.mutableAt(0).setEffects({pluginEffect(plugins.clap(), 101, {{"p1", ParamValue(-6.0)}}, clapState(0.0, 100.0))});
    track.effects = {pluginEffect(plugins.vst3(), 102, {{"p0", ParamValue(57.0 / 72.0)}}, vst3State(57.0 / 72.0, 0.0, 37))};
    p.state = Project::withTrack(p.state, 0, track);
    if (master) {
        Sequence seq = *p.state.sequences.front();
        seq.audioEffects = {pluginEffect(plugins.clap(), 103, {{"p1", ParamValue(-2.0)}}, clapState(0.0, 50.0))};
        p.state = p.state.withSequence(std::make_shared<const Sequence>(std::move(seq)));
    }
}

} // namespace

TEST_CASE("Preview and export render plugin effects sample for sample alike, aligned by delay compensation",
          "[plughost][export]") {
    TestPlugins plugins;
    for (const bool master : {false, true}) {
        INFO((master ? "clip, track and master plugins" : "clip and track plugins"));
        Project p;
        pluginProject(p, plugins, master);
        const std::size_t frames = 48 * 1100;
        Rig rig(p);
        rig.player->play();
        // The master plugin runs on the audio callback, which never waits for it: the helper gets its time between
        // callbacks (settle), so a loaded computer cannot make it late.
        for (int block = 0; block < 110; ++block) {
            settle(plugins);
            rig.pull();
        }
        const auto offline = exported(p, frames);
        std::uint64_t missed = 0;
        for (const auto& i : plugins.host->instances()) {
            missed += i->missedBlocks();
        }
        // Only the callback that starts the master chain can still miss: it primes the chain (its latency's worth of
        // mix) and plays the first blocks at once, leaving the helper no time between. A 480-frame callback plays at
        // most 3 blocks, and they lie in the silent lead-in (the clip starts at 100 ms): a miss there plays silence
        // (flagged) where the export has silence too, so the comparison below stays sample for sample.
        CHECK(missed <= (master ? 3u : 0u));
        CHECK(firstMismatch(rig.stream, offline, frames * 2) == -1);
        // Aligned: the clip plays the tone where it lies, every plugin's latency compensated.
        const float g = dbGain(-6.0) * dbGain(-3.0) * (master ? dbGain(-2.0) : 1.0f);
        std::size_t wrong = 0;
        for (std::int64_t k = 4800 + 100; k < 4800 + 38000; ++k) {
            const float expected = p.feed->sample(p.mediaId('A'), 0, k - 4800, 0, k48k) * g;
            wrong += std::fabs(offline[static_cast<std::size_t>(k) * 2] - expected) > 1e-6f ? 1 : 0;
        }
        CHECK(wrong == 0);
        CHECK(offline[100 * 2] == 0.0f);
        CHECK(rig.player->stats().underrunFrames == 0);
    }
}

TEST_CASE("A plugin that stalls on the master bus misses deadlines: the dry signal for its blocks, flagged, never a wait",
          "[plughost][deadline]") {
    // A5-P2-7: a late block used to play as exact silence (hard edges on the whole mix); it now plays the latency-aligned
    // dry signal. The plugin has a gain here (-6 dB) so its output and the dry signal differ.
    TestPlugins plugins;
    Project p;
    p.media('A', tone(440.0, 0.5f));
    p.addTrack({{'A', Ticks{}, ms(2000), Ticks{}}});
    Sequence seq = *p.state.sequences.front();
    // 20 ms of sleep per 5.3 ms block: the helper falls behind at once.
    seq.audioEffects = {pluginEffect(plugins.clap(), 201, {{"p1", ParamValue(-6.0)}}, clapState(0.0, 0.0, 0.0, 20.0))};
    p.state = p.state.withSequence(std::make_shared<const Sequence>(std::move(seq)));
    Rig rig(p);
    rig.player->play();
    double longest = 0.0;
    std::vector<float> buffer(960);
    std::vector<float> played;
    for (int block = 0; block < 100; ++block) {
        const auto t0 = std::chrono::steady_clock::now();
        REQUIRE(rig.engine.pull(buffer, rig.simulatedTime()));
        longest = std::max(longest, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        played.insert(played.end(), buffer.begin(), buffer.end());
        rig.m_elapsedFrames += 480;
        rig.player->feedOnce();
    }
    const auto instance = plugins.effects->instanceFor(EffectInstanceId{201});
    REQUIRE(instance);
    const std::uint64_t missed = instance->missedBlocks();
    CHECK(missed > 0);
    CHECK(instance->running()); // late, not broken
    CHECK(longest < 5.0);       // the callback never waited for the helper
    CHECK(plughost::PluginHost::realtimeWaits() == 0);
    // Each missed block is the dry signal, flagged once (here the helper falls behind for good: few if any blocks have
    // the plugin's output); nothing is silent.
    const BlockAudit audit =
        auditBlocks(played, [&](std::int64_t k) { return p.feed->sample(p.mediaId('A'), 0, k, 0, k48k); }, dbGain(-6.0));
    CHECK(audit.dryBlocks == missed);
    CHECK(audit.silent == 0);
    CHECK(audit.wrong == 0);
    WARN("stalled master plugin: " << instance->missedBlocks() << " blocks missed in 1 s, longest callback " << longest << " ms");
}

TEST_CASE("A missed deadline plays its block dry and flags it; blocks returned in time play exactly",
          "[plughost][deadline]") {
    // A5-P2-7: before, a late block was exact silence; now it is the latency-aligned dry signal, and the next block in
    // time fades back to the plugin's output over one block.
    TestPlugins plugins;
    Project p;
    p.media('A', tone(440.0, 0.5f));
    p.addTrack({{'A', Ticks{}, ms(2000), Ticks{}}});
    Sequence seq = *p.state.sequences.front();
    // -2 dB and 6 ms of sleep per 5.3 ms block: in time when the test waits for it, late when it does not.
    seq.audioEffects = {pluginEffect(plugins.clap(), 211, {{"p1", ParamValue(-2.0)}}, clapState(0.0, 0.0, 0.0, 6.0))};
    p.state = p.state.withSequence(std::make_shared<const Sequence>(std::move(seq)));
    Rig rig(p);
    rig.player->play();
    // Stretches of 20 callbacks: the helper waited for before each (every block it plays is done), then 20 in a quick
    // burst (it falls behind: the burst's last blocks are late), and so on.
    for (int block = 0; block < 100; ++block) {
        if ((block / 20) % 2 == 0) {
            settle(plugins);
        }
        rig.pull();
    }
    const auto instance = plugins.effects->instanceFor(EffectInstanceId{211});
    REQUIRE(instance);
    CHECK(instance->running());
    const std::uint64_t missed = instance->missedBlocks();
    CHECK(missed > 0);
    const BlockAudit audit =
        auditBlocks(rig.stream, [&](std::int64_t k) { return p.feed->sample(p.mediaId('A'), 0, k, 0, k48k); }, dbGain(-2.0));
    CHECK(audit.dryBlocks == missed); // every late block was dry, and every dry one flagged
    CHECK(audit.silent == 0);
    CHECK(audit.wrong == 0);          // the rest is the plugin's output (or the fade to it), where it belongs
    // The settled stretches play (but for the first callback, which primes the chain and plays its first blocks at
    // once, and the blocks a burst left late across their edges): 3 x 20 callbacks hold 112 of the 188 blocks.
    CHECK(audit.wetBlocks + audit.fadeBlocks > 90);
    CHECK(audit.wetBlocks + audit.fadeBlocks + audit.dryBlocks == 188);
    CHECK(plughost::PluginHost::realtimeWaits() == 0);
    WARN("late master plugin: " << missed << " of 188 blocks missed, " << audit.wetBlocks << " played in time");
}

TEST_CASE("Per-instance overhead with 10 plugin instances", "[plughost][overhead]") {
    TestPlugins plugins;
    const int seconds = 10;
    const std::vector<float> in = stereoTone(48000 * seconds);
    const int blocks = 48000 * seconds / plughost::kBlockFrames;
    for (const auto context : {audio::ChainContext::Ahead, audio::ChainContext::Live}) {
        audio::EffectChain chain(2, 48000, context);
        EffectStack stack;
        for (int i = 0; i < 10; ++i) {
            stack.push_back(pluginEffect(plugins.clap(), static_cast<std::uint64_t>(300 + i + (context == audio::ChainContext::Live ? 20 : 0)),
                                         {{"p1", ParamValue(-0.5)}}));
        }
        chain.bind(stack);
        runChain(chain, stereoTone(4096), 512); // warm up
        const auto t0 = std::chrono::steady_clock::now();
        const auto out = runChain(chain, in, 512);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        const double perInstanceBlock = us / (static_cast<double>(blocks) * 10.0);
        const double realtime = us / (seconds * 1e6);
        std::printf("10 instances, %s: %.1f us per instance per 256-frame block (%.2f %% of real time for all ten)\n",
                    context == audio::ChainContext::Ahead ? "ahead of playback (each block waited for)" : "master (pipelined)",
                    perInstanceBlock, 100.0 * realtime);
        CHECK(perInstanceBlock < 1000.0); // far below the 5.3 ms a block lasts
        CHECK(std::fabs(out[out.size() - 2]) > 0.0f);
    }
    CHECK(plugins.host->processCount() == 1); // ten instances of one plugin: one helper
}
