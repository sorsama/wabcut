// The plugin host end to end (A.6): CLAP and VST3 test plugins in wabcut-plughost, through the effect chain as the
// engine runs them: processing and latency, sample-accurate automation, state round trips, a plugin missing on this
// computer, and a crash that disables one instance and nothing else.

#include "PlughostFixture.h"

#include <wabcut/model/Effects.h>

#include <catch2/catch_approx.hpp>

#include <chrono>
#include <cstring>
#include <thread>

using namespace wabcut;
using namespace wabcut::test;
using Catch::Approx;

namespace {

/// Frames of `out` that differ from `in` delayed by `delay` frames and scaled by `gain(frame)` (input frame index).
template <class Gain>
std::size_t mismatches(const std::vector<float>& in, const std::vector<float>& out, int delay, Gain gain, int from = 0) {
    std::size_t wrong = 0;
    const auto frames = static_cast<int>(in.size() / 2);
    for (int s = std::max(from, delay); s < frames; ++s) {
        const int f = s - delay;
        for (int c = 0; c < 2; ++c) {
            const float expected = in[static_cast<std::size_t>(f) * 2 + static_cast<std::size_t>(c)] * gain(f);
            wrong += out[static_cast<std::size_t>(s) * 2 + static_cast<std::size_t>(c)] != expected ? 1 : 0;
        }
    }
    return wrong;
}

} // namespace

TEST_CASE("CLAP and VST3 plugins process in the helper with their latency compensated by the chain", "[plughost]") {
    TestPlugins plugins;
    const std::vector<float> in = stereoTone(48000);

    SECTION("CLAP: gain from a parameter, latency from the state") {
        audio::EffectChain chain(2, 48000);
        const EffectStack stack{pluginEffect(plugins.clap(), 11, {{"p1", ParamValue(-6.0)}}, clapState(0.0, 100.0))};
        chain.bind(stack);
        REQUIRE(chain.latencyFrames() == plughost::kAheadPipelineFrames + 100);
        const auto out = runChain(chain, in);
        CHECK(mismatches(in, out, chain.latencyFrames(), [](int) { return dbGain(-6.0); }) == 0);
        // Before the latency: silence.
        CHECK(out[2 * 50] == 0.0f);
    }
    SECTION("VST3: gain normalised, latency from the state") {
        audio::EffectChain chain(2, 48000);
        const EffectStack stack{pluginEffect(plugins.vst3(), 12, {{"p0", ParamValue(0.75)}}, vst3State(60.0 / 72.0, 0.0, 37))};
        chain.bind(stack);
        REQUIRE(chain.latencyFrames() == plughost::kAheadPipelineFrames + 37);
        const auto out = runChain(chain, in);
        CHECK(mismatches(in, out, chain.latencyFrames(), [](int) { return dbGain(-6.0); }) == 0);
    }
    SECTION("the two in a row: latencies add up") {
        audio::EffectChain chain(2, 48000);
        const EffectStack stack{pluginEffect(plugins.clap(), 13, {{"p1", ParamValue(-3.0)}}, clapState(0.0, 64.0)),
                                pluginEffect(plugins.vst3(), 14, {{"p0", ParamValue(57.0 / 72.0)}})};
        chain.bind(stack);
        REQUIRE(chain.latencyFrames() == 2 * plughost::kAheadPipelineFrames + 64);
        const auto out = runChain(chain, in);
        const float g = dbGain(-3.0) * dbGain(-3.0);
        std::size_t wrong = 0;
        for (int s = chain.latencyFrames(); s < 48000; ++s) {
            const float expected = in[static_cast<std::size_t>(s - chain.latencyFrames()) * 2] * dbGain(-3.0) * dbGain(-3.0);
            wrong += std::fabs(out[static_cast<std::size_t>(s) * 2] - expected) > 1e-7f ? 1 : 0;
        }
        CHECK(wrong == 0);
        (void)g;
    }
    // One helper per plugin binary.
    CHECK(plugins.host->processCount() <= 2);
}

TEST_CASE("Plugin parameter automation is sample-accurate on the parameter grid", "[plughost][automation]") {
    TestPlugins plugins;
    const std::vector<float> in = stereoTone(24000);
    const Curve ramp{{Keyframe{Ticks{}, 0.0}, Keyframe{Ticks{kTicksPerSecond / 2}, -24.0}}};
    for (const bool vst3 : {false, true}) {
        INFO((vst3 ? "VST3" : "CLAP"));
        audio::EffectChain chain(2, 48000);
        // CLAP: dB from 0 to -24 over 0.5 s; VST3: the same as normalised values.
        const Curve curve = vst3 ? Curve{{Keyframe{Ticks{}, 60.0 / 72.0}, Keyframe{Ticks{kTicksPerSecond / 2}, 36.0 / 72.0}}} : ramp;
        const EffectStack stack{vst3 ? pluginEffect(plugins.vst3(), 21, {{"p0", ParamValue(curve)}})
                                     : pluginEffect(plugins.clap(), 22, {{"p1", ParamValue(curve)}})};
        chain.bind(stack);
        const int delay = chain.latencyFrames();
        const auto out = runChain(chain, in, 100);
        // Each 32-frame cell plays the value at its first frame (the chain's grid), from that frame exactly.
        const auto gainAt = [&](int f) {
            const int cellStart = f / audio::kParamCellFrames * audio::kParamCellFrames;
            const double v = curve.valueAt(Ticks{static_cast<std::int64_t>(cellStart) * 5'292'000});
            return dbGain(vst3 ? -60.0 + 72.0 * v : v);
        };
        CHECK(mismatches(in, out, delay, gainAt) == 0);
    }
}

TEST_CASE("Plugin state round-trips through the instance and the project", "[plughost][state]") {
    TestPlugins plugins;
    SECTION("an instance's saved state recreates the same sound") {
        auto first = plugins.host->instantiate(plugins.clap(), 48000.0, clapState(-9.0, 12.0));
        REQUIRE(first);
        CHECK((*first)->latency() == 12);
        auto state = (*first)->saveState();
        REQUIRE(state);
        CHECK(*state == clapState(-9.0, 12.0));
        auto second = plugins.host->instantiate(plugins.clap(), 48000.0, *state);
        REQUIRE(second);
        CHECK((*second)->latency() == 12);
        auto values = (*second)->parameterValues();
        REQUIRE(values);
        bool sawGain = false;
        for (const auto& [id, value] : *values) {
            if (id == 1) {
                CHECK(value == Approx(-9.0));
                sawGain = true;
            }
        }
        CHECK(sawGain);
    }
    SECTION("a VST3 chunk (component and controller) round-trips") {
        auto first = plugins.host->instantiate(plugins.vst3(), 48000.0, vst3State(0.5, 0.0, 21));
        REQUIRE(first);
        CHECK((*first)->latency() == 21);
        auto state = (*first)->saveState();
        REQUIRE(state);
        auto second = plugins.host->instantiate(plugins.vst3(), 48000.0, *state);
        REQUIRE(second);
        CHECK((*second)->latency() == 21);
        auto values = (*second)->parameterValues();
        REQUIRE(values);
        CHECK(std::any_of(values->begin(), values->end(), [](const auto& v) { return v.first == 0 && v.second == Approx(0.5); }));
    }
    SECTION("the project keeps the chunk as base64 parts, bounded") {
        EffectInstance e = plughost::makePluginEffect(plugins.clap(), EffectInstanceId{3});
        std::string big(30000, 'A'); // 30000 base64 characters: 30 parts
        REQUIRE(plughost::setPluginState(e, big));
        CHECK(plughost::pluginState(e) == big);
        CHECK(e.data.size() == 30 + 2); // the parts, the checksum, the name
        CHECK_FALSE(effectStackProblem({e}, EffectKind::Audio, 100));
        CHECK_FALSE(plughost::setPluginState(e, std::string((plughost::kMaxStateBytes + 3) / 3 * 4 + 4, 'A')));
        REQUIRE(plughost::setPluginState(e, ""));
        CHECK(plughost::pluginState(e).empty());
        CHECK(e.data.size() == 1); // only the name is left
    }
    SECTION("a processor rebuilt for a state just captured from its instance keeps that instance") {
        audio::EffectChain chain(2, 48000);
        EffectStack stack{pluginEffect(plugins.clap(), 31, {{"p1", ParamValue(-6.0)}})};
        chain.bind(stack);
        runChain(chain, stereoTone(2048));
        const auto live = plugins.effects->instanceFor(EffectInstanceId{31});
        REQUIRE(live);
        auto captured = plugins.effects->captureState(stack[0]);
        REQUIRE(captured);
        REQUIRE(plughost::setPluginState(stack[0], *captured));
        CHECK_FALSE(chain.bind(stack)); // nothing rebuilt
        CHECK(plugins.effects->instanceFor(EffectInstanceId{31}) == live);
    }
}

TEST_CASE("A plugin this computer does not have is kept, bypassed and badged", "[plughost][missing]") {
    TestPlugins plugins;
    EffectInstance absent;
    absent.id = EffectInstanceId{41};
    absent.effect = "plugin.vst3.00112233445566778899AABBCCDDEEFF";
    absent.set("p7", ParamValue(0.25));
    absent.setData(EffectData{std::string(plughost::kPluginNameKey), "Elsewhere EQ (Some Vendor)"});
    REQUIRE(plughost::setPluginState(absent, clapState(0.0, 0.0)));
    // The model keeps it as an unknown effect: valid, saved unchanged, not evaluable.
    CHECK_FALSE(effectStackProblem({absent}, EffectKind::Audio, 100));
    CHECK_FALSE(isEvaluable(absent, EffectKind::Audio));
    // The engine bypasses it (no latency, the input unchanged).
    audio::EffectChain chain(2, 48000);
    chain.bind(EffectStack{absent});
    CHECK(chain.empty());
    const auto report = plugins.effects->status(absent);
    CHECK(report.status == plughost::PluginEffectStatus::Missing);
    CHECK(report.message == "Missing plugin: Elsewhere EQ (Some Vendor)");
    // A scanned plugin is known to the model through the catalog's descriptors.
    const EffectDescriptor* d = EffectRegistry::builtin().find(plugins.clap().effectId());
    REQUIRE(d != nullptr);
    CHECK(d->kind == EffectKind::Audio);
    REQUIRE(d->param("p1") != nullptr);
    CHECK(d->param("p1")->minValue == -60.0);
    CHECK(d->param("p1")->maxValue == 12.0);
    CHECK(d->param("p1")->animatable);
    CHECK_FALSE(d->param("p2")->animatable); // Latency: not automatable
}

TEST_CASE("A crash in a plugin disables that instance with a badge; the editor and other plugins carry on",
          "[plughost][crash]") {
    TestPlugins plugins;
    const std::vector<float> in = stereoTone(48000);
    audio::EffectChain chain(2, 48000);
    // The CLAP plugin crashes at 0.25 s (its "Crash" parameter); the VST3 plugin after it runs in another helper.
    const Curve crash{{Keyframe{Ticks{}, 0.0, Interpolation::Hold}, Keyframe{Ticks{kTicksPerSecond / 4}, 1.0, Interpolation::Hold}}};
    EffectStack stack{pluginEffect(plugins.clap(), 51, {{"p1", ParamValue(-6.0)}, {"p3", ParamValue(crash)}}),
                      pluginEffect(plugins.vst3(), 52, {{"p0", ParamValue(57.0 / 72.0)}})};
    chain.bind(stack);
    const int delay = chain.latencyFrames();
    const auto out = runChain(chain, in, 480);
    const auto clap = plugins.effects->instanceFor(EffectInstanceId{51});
    const auto vst3 = plugins.effects->instanceFor(EffectInstanceId{52});
    REQUIRE(clap);
    REQUIRE(vst3);
    CHECK(clap->health() == plughost::InstanceHealth::Crashed);
    CHECK(clap->problem().find("crashed") != std::string::npos);
    CHECK(vst3->running());
    const auto report = plugins.effects->status(stack[0]);
    CHECK(report.status == plughost::PluginEffectStatus::Crashed);
    CHECK(plugins.effects->status(stack[1]).status == plughost::PluginEffectStatus::Ready);
    // Before the crash: both gains. After it: the CLAP stage is bypassed (dry, still delayed), the VST3 one plays on.
    std::size_t before = 0;
    for (int s = delay; s < 9000; ++s) {
        const float expected = in[static_cast<std::size_t>(s - delay) * 2] * dbGain(-6.0) * dbGain(-3.0);
        before += std::fabs(out[static_cast<std::size_t>(s) * 2] - expected) > 1e-6f ? 1 : 0;
    }
    CHECK(before == 0);
    std::size_t after = 0;
    for (int s = 30000; s < 48000; ++s) {
        const float expected = in[static_cast<std::size_t>(s - delay) * 2] * dbGain(-3.0);
        after += std::fabs(out[static_cast<std::size_t>(s) * 2] - expected) > 1e-6f ? 1 : 0;
    }
    CHECK(after == 0);
    // A new instance of the crashed plugin gets a new helper and works.
    stack[0].set("p3", ParamValue(0.0));
    audio::EffectChain again(2, 48000);
    again.bind(EffectStack{stack[0]});
    const auto fresh = runChain(again, in);
    CHECK(mismatches(in, fresh, again.latencyFrames(), [](int) { return dbGain(-6.0); }) == 0);
}
