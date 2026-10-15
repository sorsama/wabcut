#include <wabcut/audio/LevelMeter.h>

#include <algorithm>
#include <cmath>

namespace wabcut::audio {

void LevelMeter::configure(int channels, int sampleRate, MeterBallistics ballistics) noexcept {
    m_channels = std::clamp(channels, 1, kMaxChannels);
    const double rate = static_cast<double>(std::max(sampleRate, 1));
    const double fallDb = std::max(0.0, static_cast<double>(ballistics.peakFallDbPerSecond));
    m_peakFallPerFrame = static_cast<float>(std::pow(10.0, -fallDb / 20.0 / rate));
    const double window = std::max(1e-6, static_cast<double>(ballistics.rmsWindowSeconds));
    m_rmsCoefficient = 1.0 - std::exp(-1.0 / (window * rate));
    m_peakState.fill(0.0f);
    m_meanSquare.fill(0.0);
    publish();
    m_clipped.store(false, std::memory_order_relaxed);
}

void LevelMeter::process(std::span<const float> interleaved) noexcept {
    const auto channels = static_cast<std::size_t>(m_channels);
    const std::size_t frames = interleaved.size() / channels;
    if (frames == 0) {
        return;
    }
    const float release = std::pow(m_peakFallPerFrame, static_cast<float>(frames));
    bool clipped = false;
    for (std::size_t c = 0; c < channels; ++c) {
        float blockPeak = 0.0f;
        double meanSquare = m_meanSquare[c];
        const double k = m_rmsCoefficient;
        for (std::size_t f = 0; f < frames; ++f) {
            const float x = interleaved[f * channels + c];
            if (!std::isfinite(x)) {
                continue;
            }
            const float a = std::fabs(x);
            blockPeak = std::max(blockPeak, a);
            const double xd = static_cast<double>(x);
            meanSquare += k * (xd * xd - meanSquare);
        }
        if (meanSquare < 1e-30) {
            meanSquare = 0.0; // keep the decay out of the denormal range
        }
        m_meanSquare[c] = meanSquare;
        m_peakState[c] = std::max(blockPeak, m_peakState[c] * release);
        clipped = clipped || blockPeak >= 1.0f;
    }
    publish();
    if (clipped) {
        m_clipped.store(true, std::memory_order_relaxed);
    }
}

void LevelMeter::processSilence(std::int64_t frames) noexcept {
    if (frames <= 0) {
        return;
    }
    const auto n = static_cast<double>(frames);
    const float release = std::pow(m_peakFallPerFrame, static_cast<float>(n));
    const double decay = std::pow(1.0 - m_rmsCoefficient, n);
    for (int c = 0; c < m_channels; ++c) {
        m_peakState[c] *= release;
        m_meanSquare[c] *= decay;
        if (m_meanSquare[c] < 1e-30) {
            m_meanSquare[c] = 0.0;
        }
    }
    publish();
}

void LevelMeter::reset() noexcept {
    m_peakState.fill(0.0f);
    m_meanSquare.fill(0.0);
    publish();
}

void LevelMeter::publish() noexcept {
    for (int c = 0; c < kMaxChannels; ++c) {
        const bool active = c < m_channels;
        m_peak[c].store(active ? m_peakState[c] : 0.0f, std::memory_order_relaxed);
        m_rms[c].store(active ? static_cast<float>(std::sqrt(m_meanSquare[c])) : 0.0f, std::memory_order_relaxed);
    }
}

float LevelMeter::peak(int channel) const noexcept {
    if (channel < 0 || channel >= kMaxChannels) {
        return 0.0f;
    }
    return m_peak[channel].load(std::memory_order_relaxed);
}

float LevelMeter::rms(int channel) const noexcept {
    if (channel < 0 || channel >= kMaxChannels) {
        return 0.0f;
    }
    return m_rms[channel].load(std::memory_order_relaxed);
}

} // namespace wabcut::audio
