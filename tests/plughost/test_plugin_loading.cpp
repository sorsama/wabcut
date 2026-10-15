// Review wave 5, A5-P2-5: plugins load on a worker outside an offline render. A slow plugin (its state takes long to
// load) never stops the thread that starts playback or the player's feeder: its processor plays the dry signal,
// latency-compensated, until the plugin is ready, then takes it over at a block boundary, fading in over one block.
// Ahead of playback a block may take HostOptions::playbackBlockTimeout (50 ms) before the instance counts as hung,
// never the plugin's own stall. An offline render (export, ScopedOfflineRender) still loads the plugin before it plays.
//
// Before the fix PluginEffects::create loaded the plugin on the calling thread (helper start, create, state: here up
// to 1.5 s): EffectChain::bind, the player's start() and its feeder waited for it (every track underran meanwhile), and
// a block ahead of playback could take blockTimeout (3 s) before the instance was marked hung.

#include "PlughostFixture.h"

#include "../playback/PlaybackFixture.h"

#include <wabcut/playback/audio/OfflineMixdown.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>

using namespace wabcut;
using namespace wabcut::playback;
using namespace wabcut::test;

namespace {

/// The CLAP test plugin's longer state blob (TestGainClap.cpp's StateBlob): clapState()'s fields, then how long loading
/// and saving the state take (a slow plugin) and whether process() prints to stdout.
std::string clapStateLong(double gainDb, double latency, double stateMs, double chatter = 0.0) {
    struct Blob {
        std::uint32_t magic;
        double gain, latency, crash, stall, stateMs, chatter;
    } blob;
    static_assert(sizeof(Blob) == 56, "TestGainClap.cpp's StateBlob");
    std::memset(&blob, 0, sizeof blob);
    blob.magic = 0x57544731;
    blob.gain = gainDb;
    blob.latency = latency;
    blob.stateMs = stateMs;
    blob.chatter = chatter;
    return plughost::base64Encode(&blob, sizeof blob);
}

SyntheticSignal tone(double hz, float amplitude) {
    SyntheticSignal s;
    s.kind = SyntheticSignal::Kind::Sine;
    s.frequency = hz;
    s.amplitude = amplitude;
    return s;
}

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

/// A played stream (interleaved stereo, left channel) in plugin blocks of kBlockFrames from frame `origin`, where the
/// source is `dry(f)` and the plugin's output `dry(f) * g`.
struct LoadAudit {
    std::uint64_t dry = 0;      ///< blocks of the dry signal throughout
    std::uint64_t wet = 0;      ///< blocks of the plugin's output throughout
    std::uint64_t fade = 0;     ///< blocks between the two (fading in)
    std::uint64_t dryLater = 0; ///< dry blocks after the plugin first sounded (it never goes back once it plays)
    std::size_t silent = 0;     ///< frames of silence where the source sounds
    std::size_t wrong = 0;      ///< frames neither dry, the plugin's, nor between the two
};

template <class Dry>
LoadAudit auditLoad(const std::vector<float>& played, std::size_t origin, Dry dry, float g) {
    LoadAudit audit;
    const std::size_t total = played.size() / 2;
    const auto block = static_cast<std::size_t>(plughost::kBlockFrames);
    bool sounded = false;
    for (std::size_t b = origin; b + block <= total; b += block) {
        bool any = false;
        bool allDry = true;
        bool allWet = true;
        for (std::size_t k = b; k < b + block; ++k) {
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
            ++audit.wet;
            sounded = true;
        } else if (allDry) {
            ++audit.dry;
            audit.dryLater += sounded ? 1 : 0;
        } else {
            ++audit.fade;
            sounded = true;
        }
    }
    return audit;
}

/// A1: a 440 Hz tone for 60 s whose clip carries `effect`.
Project toneProject(const EffectInstance& effect) {
    Project p;
    p.media('A', tone(440.0, 0.5f));
    p.addTrack({{'A', Ticks{}, ms(60000), Ticks{}}});
    Track track = p.track(0);
    track.clips.mutableAt(0).setEffects({effect});
    p.state = Project::withTrack(p.state, 0, track);
    return p;
}

/// The tests' host options, but plugins load on a worker as in the app (blocks still have 3 s: a loaded computer).
plughost::HostOptions deferredOptions() {
    plughost::HostOptions options = testHostOptions();
    options.deferInstantiation = true;
    return options;
}

} // namespace

TEST_CASE("Outside an offline render a slow plugin plays dry and aligned until it is loaded then takes over at a block boundary",
          "[plughost][loading]") {
    TestPlugins plugins(deferredOptions());
    constexpr int kTotal = 48000 * 10;
    const std::vector<float> in = stereoTone(kTotal);
    std::vector<float> out = in;
    audio::EffectChain chain(2, 48000);
    // -6 dB, no latency of its own; loading its state takes 800 ms.
    const EffectStack stack{pluginEffect(plugins.clap(), 831, {}, clapStateLong(-6.0, 0.0, 800.0))};
    const auto t0 = std::chrono::steady_clock::now();
    chain.bind(stack);
    CHECK(msSince(t0) < 400.0); // before the fix: the helper started and the state loaded inside bind()
    REQUIRE(chain.latencyFrames() == plughost::kAheadPipelineFrames); // the latency it is expected to have (scanned: 0)

    int done = 0;
    const auto run = [&](int blocks, bool paced) {
        for (int i = 0; i < blocks; ++i) {
            REQUIRE(done + plughost::kBlockFrames <= kTotal);
            chain.process(out.data() + static_cast<std::size_t>(done) * 2, plughost::kBlockFrames, done, audio::CurveClock{});
            done += plughost::kBlockFrames;
            if (paced) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    };
    // Blocks in about real time until the worker has loaded the plugin and the processor gave it its first block, then
    // 64 more (each waited for: the plugin plays).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    std::shared_ptr<plughost::PluginInstance> instance;
    for (;;) {
        instance = plugins.effects->instanceFor(EffectInstanceId{831});
        if (instance && instance->shared().submitted.load(std::memory_order_acquire) > 0) {
            break;
        }
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        run(1, true);
    }
    run(64, false);
    out.resize(static_cast<std::size_t>(done) * 2);
    CHECK(instance->running());

    // The chain's latency first: silence. Then output frame s is input frame s - 256: dry while the plugin loads, then
    // one block fading in, then the plugin's -6 dB, on the plugin's block grid.
    CHECK(std::all_of(out.begin(), out.begin() + 2 * plughost::kAheadPipelineFrames, [](float v) { return v == 0.0f; }));
    const LoadAudit audit = auditLoad(
        out, static_cast<std::size_t>(plughost::kAheadPipelineFrames),
        [&](std::int64_t s) { return in[static_cast<std::size_t>(s - plughost::kAheadPipelineFrames) * 2]; }, dbGain(-6.0));
    CHECK(audit.silent == 0);
    CHECK(audit.wrong == 0);
    CHECK(audit.dry >= 10); // 800 ms of loading: many blocks
    CHECK(audit.fade == 1); // the switch: at a block boundary, over one block
    CHECK(audit.dryLater == 0);
    CHECK(audit.wet >= 60);
}

TEST_CASE("A slow plugin loading on a worker never stalls playback start or the feeder: dry and aligned until it plays",
          "[plughost][loading]") {
    TestPlugins plugins(deferredOptions());
    // -6 dB, no latency; loading its state takes 1.5 s.
    Project p = toneProject(pluginEffect(plugins.clap(), 801, {}, clapStateLong(-6.0, 0.0, 1500.0)));
    const auto t0 = std::chrono::steady_clock::now();
    auto rig = std::make_unique<Rig>(p); // start(): the player's first pass, on this thread, creates the clip's processor
    rig->player->play();
    CHECK(msSince(t0) < 1000.0); // before the fix: at least the 1.5 s of loading

    double longest = 0.0;
    const auto pull = [&] {
        const auto t = std::chrono::steady_clock::now();
        rig->pull();
        longest = std::max(longest, msSince(t));
        std::this_thread::sleep_for(std::chrono::milliseconds(10)); // about real time
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    std::shared_ptr<plughost::PluginInstance> instance;
    for (;;) {
        instance = plugins.effects->instanceFor(EffectInstanceId{801});
        if (instance && instance->shared().submitted.load(std::memory_order_acquire) > 0) {
            break; // loaded, and the clip's processor gave it its first block
        }
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        pull();
    }
    for (int i = 0; i < 60; ++i) { // 600 ms more: past the feeder's lead (250 ms)
        pull();
    }
    CHECK(longest < 1000.0); // no pull waited for the plugin to load
    CHECK(instance->running());
    CHECK(rig->player->stats().underrunFrames == 0);
    // Dry and aligned while it loaded, one block fading in at a block boundary, then the plugin's -6 dB.
    const LoadAudit audit =
        auditLoad(rig->stream, 0, [&](std::int64_t k) { return p.feed->sample(p.mediaId('A'), 0, k, 0, k48k); }, dbGain(-6.0));
    CHECK(audit.silent == 0);
    CHECK(audit.wrong == 0);
    CHECK(audit.dry >= 100);
    CHECK(audit.fade == 1);
    CHECK(audit.dryLater == 0);
    CHECK(audit.wet >= 32);
    WARN("slow plugin: " << audit.dry << " blocks dry while it loaded, then " << audit.wet << " through it; longest pull "
                         << longest << " ms");
}

TEST_CASE("Ahead of playback a block waits at most playbackBlockTimeout: a stalling plugin is bypassed and the feeder never waits for its stall",
          "[plughost][loading][deadline]") {
    TestPlugins plugins(plughost::HostOptions{}); // the defaults: loaded on a worker, 50 ms per block ahead of playback
    // -6 dB; every process call sleeps 400 ms.
    const EffectInstance effect = pluginEffect(plugins.clap(), 811, {}, clapState(-6.0, 0.0, 0.0, 400.0));
    Project p = toneProject(effect);
    Rig rig(p);
    rig.player->play();
    double longest = 0.0;
    const auto pull = [&] {
        const auto t = std::chrono::steady_clock::now();
        rig.pull();
        longest = std::max(longest, msSince(t));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    std::shared_ptr<plughost::PluginInstance> instance;
    for (;;) {
        instance = plugins.effects->instanceFor(EffectInstanceId{811});
        if (instance && instance->health() == plughost::InstanceHealth::Hung) {
            break;
        }
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        pull();
    }
    for (int i = 0; i < 30; ++i) {
        pull();
    }
    CHECK(instance->health() == plughost::InstanceHealth::Hung);
    CHECK(plugins.effects->status(effect).status == plughost::PluginEffectStatus::Hung);
    // 50 ms for the late block, then the instance is bypassed. Before the fix every block waited out the 400 ms stall
    // (3 s were allowed), and one feeder pass of the 250 ms lead took many seconds.
    CHECK(longest < 300.0);
    CHECK(rig.player->stats().underrunFrames == 0);
    // Dry and aligned throughout, never silent (its first block came too late: the plugin's -6 dB is never heard).
    const LoadAudit audit =
        auditLoad(rig.stream, 0, [&](std::int64_t k) { return p.feed->sample(p.mediaId('A'), 0, k, 0, k48k); }, dbGain(-6.0));
    CHECK(audit.silent == 0);
    CHECK(audit.wrong == 0);
    CHECK(audit.wet == 0);
    CHECK(audit.fade == 0);
    CHECK(audit.dry > 0);
    WARN("stalling plugin: longest pull " << longest << " ms");
}

TEST_CASE("An offline render loads a slow plugin before it plays: the plugin from the first sample and never dry",
          "[plughost][loading][export]") {
    TestPlugins plugins(deferredOptions());
    const float g = dbGain(-6.0);

    SECTION("OfflineMixdown (export, loudness analysis)") {
        // -6 dB and 32 frames of latency (the scanned latency is 0: compensating the guess would be 32 frames off).
        Project p = toneProject(pluginEffect(plugins.clap(), 821, {}, clapStateLong(-6.0, 32.0, 500.0)));
        OfflineMixdownOptions options;
        options.renderer.edgeFade = Ticks{};
        options.project = std::make_shared<const ProjectState>(p.state);
        OfflineMixdown mix(p.state.sequences.front(), 2, 48000, Ticks{}, *p.feed, options);
        std::vector<float> out(static_cast<std::size_t>(24000) * 2);
        mix.render(out);
        std::size_t wrong = 0;
        for (std::int64_t k = 0; k < 24000; ++k) {
            const float expected = p.feed->sample(p.mediaId('A'), 0, k, 0, k48k) * g;
            wrong += std::fabs(out[static_cast<std::size_t>(k) * 2] - expected) > 1e-6f ? 1 : 0;
        }
        CHECK(wrong == 0);
    }
    SECTION("an effect chain inside a ScopedOfflineRender") {
        const audio::ScopedOfflineRender offline;
        audio::EffectChain chain(2, 48000);
        // Alive while the chain runs: bind() keeps pointers into it.
        const EffectStack stack{pluginEffect(plugins.clap(), 822, {}, clapStateLong(-6.0, 32.0, 500.0))};
        chain.bind(stack);
        REQUIRE(chain.latencyFrames() == plughost::kAheadPipelineFrames + 32); // the loaded plugin's, not the guess
        const std::vector<float> in = stereoTone(9600);
        const auto out = runChain(chain, in);
        const int delay = chain.latencyFrames();
        std::size_t wrong = 0;
        for (int s = delay; s < 9600; ++s) {
            const float expected = in[static_cast<std::size_t>(s - delay) * 2] * g;
            wrong += std::fabs(out[static_cast<std::size_t>(s) * 2] - expected) > 1e-6f ? 1 : 0;
        }
        CHECK(wrong == 0);
    }
    SECTION("outside one: bind returns at once and the chain rebuilds around the loaded instance when its latency differs") {
        audio::EffectChain chain(2, 48000);
        const EffectStack stack{pluginEffect(plugins.clap(), 823, {}, clapStateLong(-6.0, 32.0, 1000.0))};
        const auto t0 = std::chrono::steady_clock::now();
        chain.bind(stack);
        CHECK(msSince(t0) < 500.0);
        CHECK(chain.latencyFrames() == plughost::kAheadPipelineFrames); // the scanned latency while it loads
        const std::vector<float> in = stereoTone(4800);
        const auto dry = runChain(chain, in);
        std::size_t notDry = 0;
        for (int s = plughost::kAheadPipelineFrames; s < 4800; ++s) {
            notDry += dry[static_cast<std::size_t>(s) * 2] != in[static_cast<std::size_t>(s - plughost::kAheadPipelineFrames) * 2] ? 1 : 0;
        }
        CHECK(notDry == 0);
        // Loaded, the plugin reports 32 frames: its processor is stale and the next bind rebuilds it around the instance
        // the worker loaded (not a second one).
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        bool rebuilt = false;
        while (!rebuilt) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            rebuilt = chain.bind(stack);
            if (!rebuilt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        CHECK(chain.latencyFrames() == plughost::kAheadPipelineFrames + 32);
        CHECK(plugins.host->instances().size() == 1);
        chain.reset();
        const auto wet = runChain(chain, in);
        const int delay = chain.latencyFrames();
        std::size_t wrong = 0;
        for (int s = delay; s < 4800; ++s) {
            const float expected = in[static_cast<std::size_t>(s - delay) * 2] * g;
            wrong += std::fabs(wet[static_cast<std::size_t>(s) * 2] - expected) > 1e-6f ? 1 : 0;
        }
        CHECK(wrong == 0);
    }
}
