// 0.7 (card 047): the processor of wabcut.audio.spectralRepair (descriptor: src/model/src/EffectsAudioTime.cpp; the edit list:
// wabcut/model/SpectralRepair.h; the DSP: wabcut/audio/dsp/SpectralRepair.h). The edits come from the instance's data
// when the processor is built (the chain rebuilds it whenever they change), the FFT size and the listen mode from its
// constant parameters. The clip's time map comes with every call (ParamCell::clock): the repair is anchored to the clip's
// SOURCE time, so it stays on the sound when the clip is trimmed, moved or retimed.

#include "PremiereEffects.h"

#include <wabcut/audio/dsp/SpectralRepair.h>
#include <wabcut/model/SpectralRepair.h>
#include <wabcut/model/Time.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace wabcut::audio::premiere {

namespace {

std::int64_t floorDivide(std::int64_t a, std::int64_t b) noexcept {
    const std::int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

/// The chain's clock as the repair's time map.
class ClockMap final : public dsp::SpectralTimeMap {
public:
    ClockMap(const CurveClock& clock, int sampleRate) : m_clock(clock), m_rate(sampleRate) {}

    double sourceSeconds(std::int64_t frame) const noexcept override {
        return static_cast<double>(m_clock.curveTimeAt(frame).value) / static_cast<double>(kTicksPerSecond);
    }
    double pitchRatio(std::int64_t frame) const noexcept override {
        if (m_clock.clip == nullptr || m_clock.clip->maintainPitch) {
            return 1.0;
        }
        // A clip resampled to another speed plays the source at that many times its pitch.
        const double perFrame = static_cast<double>((m_clock.curveTimeAt(frame + 1) - m_clock.curveTimeAt(frame)).value) /
                                static_cast<double>(kTicksPerSecond);
        const double ratio = std::fabs(perFrame) * m_rate;
        return ratio > 0.01 ? ratio : 1.0;
    }

private:
    const CurveClock& m_clock;
    int m_rate;
};

class SpectralRepairProcessor final : public EffectProcessor {
public:
    SpectralRepairProcessor(const EffectDescriptor& d, int channels, int sampleRate, std::span<const double> values,
                            const EffectInstance* instance)
        : m_rate(sampleRate), m_repair(channels, sampleRate, editsOf(instance), optionsOf(d, values)) {}

    int latencyFrames() const noexcept override { return m_repair.latencyFrames(); }
    int settleFrames() const noexcept override { return m_repair.settleFrames(); }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        if (cell.clock == nullptr || !m_repair.active()) {
            return;
        }
        const ClockMap map(*cell.clock, m_rate);
        const std::int64_t grid = floorDivide(cell.clock->origin.value, std::max<std::int64_t>(cell.clock->ticksPerFrame, 1));
        m_repair.process(io, frames, cell.frame, grid, map);
    }
    void reset() noexcept override { m_repair.reset(); }

private:
    static std::vector<SpectralEdit> editsOf(const EffectInstance* instance) {
        return instance != nullptr ? unpackSpectralEdits(*instance) : std::vector<SpectralEdit>{};
    }
    static dsp::SpectralRepairOptions optionsOf(const EffectDescriptor& d, std::span<const double> values) {
        dsp::SpectralRepairOptions o;
        const auto at = [&](std::string_view key) {
            const auto i = static_cast<std::size_t>(paramIndex(d, key));
            return i < values.size() ? values[i] : d.params[i].defaultValue;
        };
        const int size = std::clamp(static_cast<int>(std::lround(at("fftSize"))), 0, 2);
        o.fftSize = size == 0 ? 1024 : size == 1 ? 2048 : 4096;
        const int listen = std::clamp(static_cast<int>(std::lround(at("listen"))), 0, 2);
        o.listen = listen == 1 ? dsp::SpectralListen::Selection : listen == 2 ? dsp::SpectralListen::Removed : dsp::SpectralListen::Off;
        return o;
    }

    int m_rate;
    dsp::SpectralRepair m_repair;
};

} // namespace

bool isSpectralRepair(std::string_view id) noexcept {
    return id == kSpectralRepairEffect;
}

std::unique_ptr<EffectProcessor> createSpectralRepairProcessor(const EffectDescriptor& descriptor, int channels, int sampleRate,
                                                               std::span<const double> values, const EffectInstance* instance) {
    if (descriptor.id != kSpectralRepairEffect || values.size() < descriptor.params.size()) {
        return nullptr;
    }
    return std::make_unique<SpectralRepairProcessor>(descriptor, channels, sampleRate, values, instance);
}

} // namespace wabcut::audio::premiere
