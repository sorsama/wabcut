// 0.7 (card 044): the processor of wabcut.audio.dynamicsProcessing (descriptor: src/model/src/EffectsAudioAmplitude.cpp;
// the DSP: wabcut/audio/dsp/DynamicsProcessing.h). It reads the parameter cells, hands the sample-accurate gains and the
// mix to the DSP as ramps, and notes the levels for the Effect Controls meters under its instance's id.

#include "PremiereEffects.h"

#include <wabcut/audio/dsp/DynamicsProcessing.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <span>
#include <string>

namespace wabcut::audio::premiere {

namespace {

constexpr std::string_view kDynamicsProcessing = "wabcut.audio.dynamicsProcessing";

double gainOf(double db) noexcept { return db <= -144.0 ? 0.0 : std::pow(10.0, db / 20.0); }

class DynamicsProcessingProcessor final : public EffectProcessor {
public:
    DynamicsProcessingProcessor(const EffectDescriptor& d, int channels, int sampleRate, std::span<const double> values)
        : m_dsp(channels, sampleRate, structureOf(d, values)), m_structure(structureOf(d, values)), m_rate(sampleRate),
          m_input(paramIndex(d, "inputGain")), m_output(paramIndex(d, "outputGain")), m_mix(paramIndex(d, "mix")),
          m_limiterCeiling(paramIndex(d, "limiterCeiling")), m_limiterRelease(paramIndex(d, "limiterRelease")),
          m_gateThreshold(paramIndex(d, "autoGateThreshold")), m_gateRelease(paramIndex(d, "autoGateRelease")) {
        for (std::size_t i = 0; i < m_crossover.size(); ++i) {
            m_crossover[i] = paramIndex(d, "crossover" + std::to_string(i + 1));
        }
        static constexpr std::array<std::string_view, 9> kKeys{"Threshold", "Ratio", "Range",  "Attack", "Release",
                                                              "Knee",      "Makeup", "Bypass", "Solo"};
        for (std::size_t b = 0; b < m_band.size(); ++b) {
            const std::string prefix = "band" + std::to_string(b + 1);
            for (std::size_t k = 0; k < kKeys.size(); ++k) {
                m_band[b][k] = paramIndex(d, prefix + std::string(kKeys[k]));
            }
        }
        // What the interface starts from: the release the processor was built for sizes its pre-roll.
        for (std::size_t b = 0; b < m_band.size(); ++b) {
            m_buildSettings.band[b].releaseMs = values[static_cast<std::size_t>(m_band[b][4])];
        }
        m_buildSettings.autoGateReleaseMs = values[static_cast<std::size_t>(m_gateRelease)];
        m_buildSettings.limiterReleaseMs = values[static_cast<std::size_t>(m_limiterRelease)];
    }

    int latencyFrames() const noexcept override { return m_dsp.latencyFrames(); }
    int settleFrames() const noexcept override {
        return dsp::DynamicsProcessing::settleFrames(m_buildSettings, m_structure, static_cast<double>(m_rate));
    }

    void attachInstance(std::uint64_t instance) override { m_dsp.attachMeters(dsp::dynamicsMetersFor(instance)); }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        const double* v = cell.start;
        dsp::DynamicsSettings s;
        for (std::size_t i = 0; i < m_crossover.size(); ++i) {
            s.crossoverHz[i] = v[m_crossover[i]];
        }
        const int bands = m_structure.bands;
        for (int b = 0; b < bands; ++b) {
            const auto& at = m_band[static_cast<std::size_t>(b)];
            dsp::DynamicsBand& band = s.band[static_cast<std::size_t>(b)];
            band.mode = m_structure.modes[static_cast<std::size_t>(b)];
            band.thresholdDb = v[at[0]];
            band.ratio = v[at[1]];
            band.rangeDb = v[at[2]];
            band.attackMs = v[at[3]];
            band.releaseMs = v[at[4]];
            band.kneeDb = v[at[5]];
            band.makeupDb = v[at[6]];
            band.bypass = v[at[7]] >= 0.5;
            band.solo = v[at[8]] >= 0.5;
        }
        s.autoGateThresholdDb = v[m_gateThreshold];
        s.autoGateReleaseMs = v[m_gateRelease];
        s.limiterCeilingDb = v[m_limiterCeiling];
        s.limiterReleaseMs = v[m_limiterRelease];

        // The gains and the mix are straight lines over the cell; frame k of the call plays at start + step * (offset + k).
        dsp::DynamicsRamps r;
        const double inA = gainOf(cell.start[m_input]);
        r.input = inA;
        r.inputStep = (gainOf(cell.end[m_input]) - inA) / cell.length;
        const double outA = gainOf(cell.start[m_output]);
        r.output = outA;
        r.outputStep = (gainOf(cell.end[m_output]) - outA) / cell.length;
        const double mixA = std::clamp(cell.start[m_mix] / 100.0, 0.0, 1.0);
        r.mix = mixA;
        r.mixStep = (std::clamp(cell.end[m_mix] / 100.0, 0.0, 1.0) - mixA) / cell.length;
        r.position = static_cast<double>(cell.offset);
        m_dsp.process(io, frames, s, r);
    }
    void reset() noexcept override { m_dsp.reset(); }

private:
    static dsp::DynamicsStructure structureOf(const EffectDescriptor& d, std::span<const double> values) {
        const auto at = [&](std::string_view key) {
            const auto i = static_cast<std::size_t>(paramIndex(d, key));
            return i < values.size() ? values[i] : d.params[i].defaultValue;
        };
        dsp::DynamicsStructure s;
        s.bands = std::clamp(static_cast<int>(std::lround(at("bandCount"))), 1, dsp::kDynamicsMaxBands);
        for (std::size_t b = 0; b < s.modes.size(); ++b) {
            const int mode = std::clamp(static_cast<int>(std::lround(at("band" + std::to_string(b + 1) + "Mode"))), 0, 3);
            s.modes[b] = static_cast<dsp::DynamicsMode>(mode);
        }
        s.limiter = at("limiterEnable") >= 0.5;
        s.autoGate = at("autoGate") >= 0.5;
        return s;
    }

    dsp::DynamicsProcessing m_dsp;
    dsp::DynamicsStructure m_structure;
    int m_rate;
    int m_input, m_output, m_mix, m_limiterCeiling, m_limiterRelease, m_gateThreshold, m_gateRelease;
    std::array<int, dsp::kDynamicsMaxBands - 1> m_crossover{};
    std::array<std::array<int, 9>, dsp::kDynamicsMaxBands> m_band{};
    dsp::DynamicsSettings m_buildSettings{};
};

} // namespace

bool isDynamicsProcessing(std::string_view id) noexcept {
    return id == kDynamicsProcessing;
}

std::unique_ptr<EffectProcessor> createDynamicsProcessingProcessor(const EffectDescriptor& descriptor, int channels, int sampleRate,
                                                                   std::span<const double> values) {
    if (descriptor.id != kDynamicsProcessing || values.size() < descriptor.params.size()) {
        return nullptr;
    }
    return std::make_unique<DynamicsProcessingProcessor>(descriptor, channels, sampleRate, values);
}

} // namespace wabcut::audio::premiere
