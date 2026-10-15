#include <wabcut/audio/dsp/Compressor.h>
#include <wabcut/audio/dsp/Equalizer.h> // kMaxDspChannels

#include <algorithm>
#include <cmath>

namespace wabcut::audio::dsp {

namespace {

/// One-pole coefficient for a time constant: after `ms` a step has moved 1 - 1/e of the way.
double timeCoefficient(double ms, double sampleRate) noexcept {
    const double samples = std::max(ms, 1e-3) * 1e-3 * sampleRate;
    return std::exp(-1.0 / samples);
}

} // namespace

double compressorCurveDb(double x, const CompressorSettings& s) noexcept {
    const double t = s.thresholdDb;
    const double r = std::max(s.ratio, 1.0);
    const double w = std::max(s.kneeDb, 0.0);
    const double over = x - t;
    if (2.0 * over < -w) {
        return x;
    }
    if (w > 0.0 && 2.0 * std::abs(over) <= w) {
        const double k = over + w / 2.0;
        return x + (1.0 / r - 1.0) * k * k / (2.0 * w);
    }
    return t + over / r;
}

Compressor::Compressor(int channels, double sampleRate) noexcept
    : m_channels(std::clamp(channels, 1, kMaxDspChannels)), m_sampleRate(sampleRate > 0.0 ? sampleRate : 48000.0) {
    m_settings.ratio = 0.0; // force the first setSettings to compute
    setSettings(CompressorSettings{});
}

void Compressor::setSettings(const CompressorSettings& s) noexcept {
    if (s == m_settings) {
        return;
    }
    m_settings = s;
    m_attackCoefficient = timeCoefficient(s.attackMs, m_sampleRate);
    m_releaseCoefficient = timeCoefficient(s.releaseMs, m_sampleRate);
    m_makeup = std::pow(10.0, s.makeupDb / 20.0);
}

void Compressor::process(float* interleaved, int frames) noexcept {
    const auto ch = static_cast<std::size_t>(m_channels);
    const CompressorSettings& s = m_settings;
    const bool compresses = s.ratio > 1.0;
    for (int i = 0; i < frames; ++i) {
        float* frame = interleaved + static_cast<std::size_t>(i) * ch;
        double peak = 0.0;
        for (std::size_t c = 0; c < ch; ++c) {
            peak = std::max(peak, static_cast<double>(std::fabs(frame[c])));
        }
        double target = 0.0; // gain the static curve asks for, dB (<= 0)
        if (compresses && peak > 1e-6) {
            const double level = 20.0 * std::log10(peak);
            target = std::min(compressorCurveDb(level, s) - level, 0.0);
        }
        const double a = target < m_gainDb ? m_attackCoefficient : m_releaseCoefficient;
        m_gainDb = a * m_gainDb + (1.0 - a) * target;
        if (m_gainDb > -1e-9) {
            m_gainDb = 0.0; // settle exactly (also keeps the state out of denormals)
        }
        const double g = m_makeup * (m_gainDb == 0.0 ? 1.0 : std::pow(10.0, m_gainDb / 20.0));
        if (g != 1.0) {
            for (std::size_t c = 0; c < ch; ++c) {
                frame[c] = static_cast<float>(frame[c] * g);
            }
        }
    }
}

void Compressor::reset() noexcept { m_gainDb = 0.0; }

int Compressor::settleFrames() const noexcept {
    const double ms = std::min(5.0 * std::max(m_settings.releaseMs, m_settings.attackMs), 2000.0);
    return static_cast<int>(std::ceil(ms * 1e-3 * m_sampleRate));
}

} // namespace wabcut::audio::dsp
