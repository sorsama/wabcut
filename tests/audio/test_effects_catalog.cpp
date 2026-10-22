// p02 audio: every audio effect of the registry runs: a processor at mono, stereo and 5.1, silence stays silence, the
// output stays finite on loud noise, the latency it reports matches its lookahead flag, and its parameter names,
// ranges and defaults are well formed.

#include "DspTestUtil.h"

#include <wabcut/audio/effects/EffectChain.h>

#include <wabcut/model/Effects.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::audio;
using namespace wabcut::audio::test;

namespace {

std::vector<const EffectDescriptor*> audioStackEffects() {
    std::vector<const EffectDescriptor*> out;
    for (const EffectDescriptor& d : EffectRegistry::builtin().all()) {
        if (d.kind == EffectKind::Audio && !d.intrinsic && d.id != "wabcut.audio.enhanceSpeech" &&
            d.id != "wabcut.audio.essentialSound") {
            out.push_back(&d);
        }
    }
    return out;
}

} // namespace

TEST_CASE("Every audio effect in the registry has a processor", "[audio][effects][catalog]") {
    for (const EffectDescriptor* d : audioStackEffects()) {
        INFO(std::string(d->id));
        CHECK(isImplementedAudioEffect(d->id));
        CHECK(d->params.size() <= static_cast<std::size_t>(kMaxEffectParams));
        for (const EffectParamInfo& p : d->params) {
            INFO(std::string(p.key));
            CHECK(!p.key.empty());
            CHECK(!p.displayName.empty());
            CHECK(p.minValue <= p.defaultValue);
            CHECK(p.defaultValue <= p.maxValue);
        }
        for (const int channels : {1, 2, 6}) {
            const std::vector<double> defaults = [&] {
                std::vector<double> v;
                for (const EffectParamInfo& p : d->params) {
                    v.push_back(p.defaultValue);
                }
                return v;
            }();
            CHECK(createEffectProcessor(*d, channels, 48000, defaults) != nullptr);
        }
    }
}

TEST_CASE("Audio effects keep silence silent and loud noise finite", "[audio][effects][catalog]") {
    for (const EffectDescriptor* d : audioStackEffects()) {
        INFO(std::string(d->id));
        const EffectInstance e = effectOf(std::string(d->id));
        for (const int channels : {1, 2}) {
            INFO("channels " << channels);
            const std::vector<float> quiet = runEffect(e, std::vector<float>(48000 * static_cast<std::size_t>(channels), 0.0f), channels);
            CHECK(peak(quiet, channels) < 1e-6);
            const std::vector<float> loud = runEffect(e, noise(1.0, 1.0, channels), channels);
            bool finite = true;
            for (const float v : loud) {
                finite = finite && std::isfinite(v);
            }
            CHECK(finite);
            CHECK(peak(loud, channels) < 64.0);
        }
    }
}

TEST_CASE("An audio effect reports latency only when it looks ahead", "[audio][effects][catalog]") {
    for (const EffectDescriptor* d : audioStackEffects()) {
        INFO(std::string(d->id));
        const int latency = effectLatency(effectOf(std::string(d->id)), 2);
        CHECK(latency >= 0);
        if (!d->capabilities.has(EffectCapability::AudioLookahead)) {
            CHECK(latency == 0);
        }
    }
}
