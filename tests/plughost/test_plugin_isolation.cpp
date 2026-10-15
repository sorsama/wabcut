// Review wave 5 (A.6 plugin host): what a plugin's processor may and may not do to the chains around it. A chain that
// adopts a shared processor never changes what the live chain sends (A5-P1-1); a stereo plugin on a 6-channel chain
// keeps the other channels intact and aligned (A5-P1-2); latency past 1 s fails an instance, from the plugin or from
// corrupt shared memory (A5-P2-4, S5-P2-7); invalid output is replaced and clamped (A5-P2-8, S5-P2-7).

#include "PlughostFixture.h"

#include <wabcut/model/Effects.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stop_token>
#include <thread>

using namespace wabcut;
using namespace wabcut::test;

namespace {

/// Runs `chain` over interleaved frames [from, to) of `io` (`channels` per frame), in pieces of 333 frames.
void runRange(audio::EffectChain& chain, std::vector<float>& io, int channels, int from, int to) {
    for (int f = from; f < to;) {
        const int n = std::min(333, to - f);
        chain.process(io.data() + static_cast<std::size_t>(f) * static_cast<std::size_t>(channels), n, f, audio::CurveClock{});
        f += n;
    }
}

} // namespace

TEST_CASE("A chain that adopts a plugin processor with new parameter keys leaves the live chain's values alone",
          "[plughost][keys]") {
    // A5-P1-1. The master bus builds its next program with bind(stack, &live), sharing the plugin's processor. Before the
    // fix that bind called bindKeys() on the shared processor, so the live chain (the audio thread plays it until it
    // picks up the next one) sent its values under the new keys: its p5 value (0) went out as p1, the gain, which became
    // 0 dB instead of the state's -6 dB, and it read past the end of its one-value cell for the second key. Now each
    // chain has its own immutable binding and the live chain's output stays at -6 dB throughout.
    TestPlugins plugins;
    const std::vector<float> in = stereoTone(48000);
    std::vector<float> out = in;
    audio::EffectChain live(2, 48000);
    const EffectStack before{pluginEffect(plugins.clap(), 601, {{"p5", ParamValue(0.0)}}, clapState(-6.0, 0.0))};
    live.bind(before);
    const int delay = live.latencyFrames();
    runRange(live, out, 2, 0, 12000);
    const auto instance = plugins.effects->instanceFor(EffectInstanceId{601});
    REQUIRE(instance);

    audio::EffectChain next(2, 48000);
    const EffectStack after{
        pluginEffect(plugins.clap(), 601, {{"p1", ParamValue(-6.0)}, {"p5", ParamValue(0.0)}}, clapState(-6.0, 0.0))};
    next.bind(after, &live);
    CHECK(plugins.effects->instanceFor(EffectInstanceId{601}) == instance); // the processor is shared, not rebuilt
    CHECK(plugins.host->instances().size() == 1);
    runRange(live, out, 2, 12000, 30000); // the live chain plays on
    runRange(next, out, 2, 30000, 48000); // then the next one carries on with the same processor

    std::size_t wrong = 0;
    for (int s = delay; s < 48000; ++s) {
        for (std::size_t c = 0; c < 2; ++c) {
            const float expected = in[static_cast<std::size_t>(s - delay) * 2 + c] * dbGain(-6.0);
            wrong += std::fabs(out[static_cast<std::size_t>(s) * 2 + c] - expected) > 1e-6f ? 1 : 0;
        }
    }
    CHECK(wrong == 0);
}

TEST_CASE("A stereo plugin on a 6-channel chain plays L/R and passes the other channels through, aligned",
          "[plughost][channels]") {
    // A5-P1-2. Before the fix the processor clamped itself to 2 channels and walked the 6-channel buffer as stereo
    // frames: the first third of it went through the plugin scrambled (L/R, C/LFE, Ls/Rs of one frame as three stereo
    // frames), the rest stayed dry and undelayed. Now it strides by the chain's channels: L/R through the plugin, C, LFE,
    // Ls and Rs delayed by the same latency.
    TestPlugins plugins;
    constexpr int kChannels = 6;
    constexpr int kFrames = 24000;
    std::vector<float> in(static_cast<std::size_t>(kFrames) * kChannels);
    for (int k = 0; k < kFrames; ++k) {
        for (int c = 0; c < kChannels; ++c) {
            in[static_cast<std::size_t>(k) * kChannels + static_cast<std::size_t>(c)] =
                static_cast<float>(0.1 * (c + 1) * std::sin(2.0 * 3.141592653589793 * (200.0 + 100.0 * c) * k / 48000.0));
        }
    }
    audio::EffectChain chain(kChannels, 48000);
    // Alive while the chain runs: bind() keeps pointers into it.
    const EffectStack stack{pluginEffect(plugins.clap(), 611, {{"p1", ParamValue(-6.0)}}, clapState(0.0, 40.0))};
    chain.bind(stack);
    const int delay = chain.latencyFrames();
    REQUIRE(delay == plughost::kAheadPipelineFrames + 40);
    std::vector<float> out = in;
    runRange(chain, out, kChannels, 0, kFrames);
    std::size_t wrongPlugin = 0;
    std::size_t wrongOthers = 0;
    std::size_t notSilent = 0;
    for (int s = 0; s < kFrames; ++s) {
        for (int c = 0; c < kChannels; ++c) {
            const float got = out[static_cast<std::size_t>(s) * kChannels + static_cast<std::size_t>(c)];
            if (s < delay) {
                notSilent += got != 0.0f ? 1 : 0;
                continue;
            }
            const float source = in[static_cast<std::size_t>(s - delay) * kChannels + static_cast<std::size_t>(c)];
            if (c < 2) {
                wrongPlugin += std::fabs(got - source * dbGain(-6.0)) > 1e-6f ? 1 : 0;
            } else {
                wrongOthers += got != source ? 1 : 0;
            }
        }
    }
    CHECK(notSilent == 0);
    CHECK(wrongPlugin == 0);
    CHECK(wrongOthers == 0);
}

TEST_CASE("A plugin that reports more than a second of latency fails to load", "[plughost][latency]") {
    // A5-P2-4 / S5-P2-7. 4800 frames at 4 kHz is 1.2 s. Before the fix the helper published any latency and the
    // processor sized its delay line and the engine's read-ahead by it; now the helper refuses the instance.
    TestPlugins plugins;
    const auto made = plugins.host->instantiate(plugins.clap(), 4000.0, clapState(0.0, 4800.0));
    REQUIRE_FALSE(made);
    CHECK(made.error().message.find("latency") != std::string::npos);
}

TEST_CASE("A latency past the limit in shared memory fails the instance and never sizes a processor",
          "[plughost][latency]") {
    // S5-P2-7. The helper's shared memory is untrusted. Before the fix latency() returned 0x7fffffff as it was, the
    // instance kept running and went back to the pool, and the next processor of the effect took it and computed its
    // delay line as pipeline + latency + 1 in int (overflow; a length_error or a gigabyte). Now the latency reads as at
    // most the limit, the instance fails (it is not pooled) and the next processor gets a new instance.
    TestPlugins plugins;
    const EffectInstance effect = pluginEffect(plugins.clap(), 621, {{"p1", ParamValue(-6.0)}});
    std::shared_ptr<plughost::PluginInstance> instance;
    {
        audio::EffectChain first(2, 48000);
        first.bind(EffectStack{effect});
        runChain(first, stereoTone(2048));
        instance = plugins.effects->instanceFor(EffectInstanceId{621});
        REQUIRE(instance);
    } // its processor goes away: the instance is idle in the pool
    instance->shared().latency.store(0x7fffffffu);
    CHECK(instance->latency() <= plughost::maxLatencyFrames(48000.0));
    CHECK(instance->health() == plughost::InstanceHealth::Failed);
    CHECK(instance->problem().find("latency") != std::string::npos);
    for (const auto context : {audio::ChainContext::Ahead, audio::ChainContext::Live}) {
        audio::EffectChain chain(2, 48000, context);
        // Alive while the chain runs: bind() keeps pointers into it.
        const EffectStack stack{effect};
        chain.bind(stack);
        const int pipeline = context == audio::ChainContext::Live ? plughost::kLivePipelineFrames : plughost::kAheadPipelineFrames;
        CHECK(chain.latencyFrames() == pipeline);
        const auto out = runChain(chain, stereoTone(9600));
        CHECK(std::fabs(out[out.size() - 2]) > 0.0f);
    }
}

TEST_CASE("Invalid plugin output is replaced by the dry signal or clamped, and counted", "[plughost][sanitize]") {
    // A5-P2-8 / S5-P2-7. Before the fix the plugin's samples were copied as they came: a burst of NaN poisoned the EQ after
    // it (its filter state stays NaN, so everything after the burst was NaN), 1e30 went straight on. Now a NaN block
    // plays the dry signal (counted; the instance fails after kMaxInvalidBlocks of them) and anything else is clamped.
    // An offline render (as export's): the plugin loads before it plays (A5-P2-5), so the burst at 0.25 s reaches it
    // however fast the chain runs.
    const audio::ScopedOfflineRender offline;
    TestPlugins plugins;
    const std::vector<float> in = stereoTone(48000);
    EffectInstance eq;
    eq.id = EffectInstanceId{632};
    eq.effect = "wabcut.audio.eq3";
    eq.version = 1;
    eq.set("lowGain", ParamValue(3.0));

    SECTION("a short burst of NaN") {
        const Curve burst{{Keyframe{Ticks{}, 0.0, Interpolation::Hold}, Keyframe{Ticks{kTicksPerSecond / 4}, 1.0, Interpolation::Hold},
                           Keyframe{Ticks{kTicksPerSecond / 4 + kTicksPerSecond / 1000}, 0.0, Interpolation::Hold}}};
        audio::EffectChain chain(2, 48000);
        // Alive while the chain runs: bind() keeps pointers into it.
        const EffectStack stack{pluginEffect(plugins.clap(), 631, {{"p1", ParamValue(-6.0)}, {"p5", ParamValue(burst)}}), eq};
        chain.bind(stack);
        const auto out = runChain(chain, in);
        std::size_t invalid = 0;
        for (const float v : out) {
            invalid += std::isfinite(v) ? 0 : 1;
        }
        CHECK(invalid == 0);
        double energy = 0.0;
        for (std::size_t i = out.size() / 2; i < out.size(); ++i) {
            energy += static_cast<double>(out[i]) * out[i];
        }
        CHECK(energy > 1.0); // after the burst: sound, not silence
        const auto instance = plugins.effects->instanceFor(EffectInstanceId{631});
        REQUIRE(instance);
        CHECK(instance->invalidBlocks() >= 1);
        CHECK(instance->running()); // one burst does not fail it
    }
    SECTION("NaN for good fails the instance; its effect is bypassed") {
        audio::EffectChain chain(2, 48000);
        // Alive while the chain runs: bind() keeps pointers into it.
        const EffectStack stack{pluginEffect(plugins.clap(), 633, {{"p5", ParamValue(1.0)}}), eq};
        chain.bind(stack);
        const auto out = runChain(chain, in);
        CHECK(std::all_of(out.begin(), out.end(), [](float v) { return std::isfinite(v); }));
        const auto instance = plugins.effects->instanceFor(EffectInstanceId{633});
        REQUIRE(instance);
        CHECK(instance->health() == plughost::InstanceHealth::Failed);
        CHECK(instance->invalidBlocks() >= plughost::kMaxInvalidBlocks);
        CHECK(plugins.effects->status(pluginEffect(plugins.clap(), 633)).status == plughost::PluginEffectStatus::Failed);
    }
    SECTION("1e30 is clamped") {
        audio::EffectChain chain(2, 48000);
        // Alive while the chain runs: bind() keeps pointers into it.
        const EffectStack stack{pluginEffect(plugins.clap(), 634, {{"p5", ParamValue(2.0)}})};
        chain.bind(stack);
        const auto out = runChain(chain, in);
        CHECK(std::all_of(out.begin(), out.end(), [](float v) { return std::isfinite(v) && std::fabs(v) <= plughost::kMaxOutputLevel; }));
        CHECK(std::fabs(out[out.size() - 2]) == plughost::kMaxOutputLevel);
    }
}

TEST_CASE("A scan stops at once when asked, even on a plugin that hangs", "[plughost][scan]") {
    // A5-P2-21 (the scan half). Before the fix scanFile() waited the whole scan timeout for a hung plugin whatever the
    // stop token said, so quitting during a scan blocked for up to 30 s. Now it checks the token every 50 ms.
    plughost::CatalogOptions options;
    options.helper = plughostPath();
    options.folders = {testPluginDir() / "blocked"}; // WabcutTestHangOnScan.clap sleeps for 10 minutes when scanned
    options.cacheFile = plughostOutput("scan-stop.json");
    options.scanTimeout = std::chrono::milliseconds(20000);
    std::filesystem::remove(options.cacheFile);
    plughost::PluginCatalog catalog(options);
    std::stop_source stop;
    std::thread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        stop.request_stop();
    });
    const auto t0 = std::chrono::steady_clock::now();
    const plughost::ScanReport report = catalog.scan(stop.get_token());
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    stopper.join();
    CHECK(report.cancelled);
    CHECK(seconds < 5.0);
    for (const auto& b : catalog.blocked()) {
        CHECK(b.path.filename() != std::filesystem::path("WabcutTestHangOnScan.clap")); // cancelled: not blamed
    }
}
