#include <wabcut/audio/dsp/Repair.h>

#include <algorithm>
#include <cmath>

namespace wabcut::audio::dsp {

namespace {

/// One-pole coefficient for a time constant: after `ms` a step has moved 1 - 1/e of the way.
double timeCoefficient(double ms, double sampleRate) noexcept {
    const double samples = std::max(ms, 1e-3) * 1e-3 * sampleRate;
    return std::exp(-1.0 / samples);
}

double dbToLinear(double db) noexcept { return std::pow(10.0, db / 20.0); }

double linearToDb(double v) noexcept { return v > 1e-12 ? 20.0 * std::log10(v) : -240.0; }

double combinedDb(const BiquadCoefficients* c, int n, double frequency, double sampleRate) noexcept {
    double db = 0.0;
    for (int i = 0; i < n; ++i) {
        db += magnitudeDb(c[i], frequency, sampleRate);
    }
    return db;
}

} // namespace

// ---- CutFilter ---------------------------------------------------------------------------------------------------

CutFilter::CutFilter(int channels, double sampleRate, double slopeDbPerOctave) noexcept
    : m_channels(std::clamp(channels, 1, kMaxDspChannels)), m_sampleRate(sampleRate > 0.0 ? sampleRate : 48000.0),
      m_sections(std::clamp(static_cast<int>(std::lround(slopeDbPerOctave / 12.0)), 1, kMaxSections)) {
    setFrequencies(kLowCutOff, kHighCutOff);
}

void CutFilter::setFrequencies(double lowCutHz, double highCutHz) noexcept {
    if (lowCutHz == m_low && highCutHz == m_high) {
        return;
    }
    m_low = lowCutHz;
    m_high = highCutHz;
    const bool lowWasOn = m_lowOn;
    const bool highWasOn = m_highOn;
    m_lowOn = lowCutHz > kLowCutOff;
    m_highOn = highCutHz < kHighCutOff && highCutHz < 0.45 * m_sampleRate;
    for (int k = 0; k < m_sections; ++k) {
        const double q = butterworthQ(m_sections, k);
        m_lowCoefficients[static_cast<std::size_t>(k)] = highPass(m_sampleRate, std::max(lowCutHz, 1.0), q);
        m_highCoefficients[static_cast<std::size_t>(k)] = lowPass(m_sampleRate, highCutHz, q);
    }
    // A side that turns on starts from silence, not from what it held when it was last on.
    for (int c = 0; c < m_channels; ++c) {
        for (int k = 0; k < m_sections; ++k) {
            if (m_lowOn && !lowWasOn) {
                m_state[static_cast<std::size_t>(c)][static_cast<std::size_t>(k)].reset();
            }
            if (m_highOn && !highWasOn) {
                m_state[static_cast<std::size_t>(c)][static_cast<std::size_t>(kMaxSections + k)].reset();
            }
        }
    }
}

void CutFilter::process(float* io, int frames) noexcept {
    if (!m_lowOn && !m_highOn) {
        return;
    }
    const auto ch = static_cast<std::size_t>(m_channels);
    for (std::size_t c = 0; c < ch; ++c) {
        auto& st = m_state[c];
        for (int i = 0; i < frames; ++i) {
            float v = io[static_cast<std::size_t>(i) * ch + c];
            if (m_lowOn) {
                for (int k = 0; k < m_sections; ++k) {
                    v = st[static_cast<std::size_t>(k)].process(m_lowCoefficients[static_cast<std::size_t>(k)], v);
                }
            }
            if (m_highOn) {
                for (int k = 0; k < m_sections; ++k) {
                    v = st[static_cast<std::size_t>(kMaxSections + k)].process(m_highCoefficients[static_cast<std::size_t>(k)], v);
                }
            }
            io[static_cast<std::size_t>(i) * ch + c] = v;
        }
    }
}

void CutFilter::reset() noexcept {
    for (auto& channel : m_state) {
        for (BiquadState& s : channel) {
            s.reset();
        }
    }
}

double CutFilter::magnitudeDb(double frequency) const noexcept {
    double db = 0.0;
    if (m_lowOn) {
        db += combinedDb(m_lowCoefficients.data(), m_sections, frequency, m_sampleRate);
    }
    if (m_highOn) {
        db += combinedDb(m_highCoefficients.data(), m_sections, frequency, m_sampleRate);
    }
    return db;
}

// ---- DeHum -------------------------------------------------------------------------------------------------------

DeHum::DeHum(int channels, double sampleRate, double frequency, int harmonics) noexcept
    : m_channels(std::clamp(channels, 1, kMaxDspChannels)), m_sampleRate(sampleRate > 0.0 ? sampleRate : 48000.0),
      m_frequency(std::clamp(frequency, 20.0, 200.0)), m_harmonics(0) {
    const int wanted = std::clamp(harmonics, 1, kMaxHarmonics);
    while (m_harmonics < wanted && (m_harmonics + 1) * m_frequency < 0.45 * m_sampleRate) {
        ++m_harmonics;
    }
    setReduction(0.0);
}

void DeHum::setReduction(double reductionDb) noexcept {
    const double r = std::max(reductionDb, 0.0);
    if (r == m_reduction) {
        return;
    }
    m_reduction = r;
    for (int k = 0; k < m_harmonics; ++k) {
        m_coefficients[static_cast<std::size_t>(k)] = notch(m_sampleRate, m_frequency * (k + 1), kNotchQ, r);
    }
}

void DeHum::process(float* io, int frames) noexcept {
    if (m_reduction <= 0.0) {
        return; // a notch of depth 0 is the identity: skipping it is the same
    }
    const auto ch = static_cast<std::size_t>(m_channels);
    for (std::size_t c = 0; c < ch; ++c) {
        auto& st = m_state[c];
        for (int i = 0; i < frames; ++i) {
            float v = io[static_cast<std::size_t>(i) * ch + c];
            for (int k = 0; k < m_harmonics; ++k) {
                v = st[static_cast<std::size_t>(k)].process(m_coefficients[static_cast<std::size_t>(k)], v);
            }
            io[static_cast<std::size_t>(i) * ch + c] = v;
        }
    }
}

void DeHum::reset() noexcept {
    for (auto& channel : m_state) {
        for (BiquadState& s : channel) {
            s.reset();
        }
    }
}

double DeHum::magnitudeDb(double frequency) const noexcept {
    return m_reduction > 0.0 ? combinedDb(m_coefficients.data(), m_harmonics, frequency, m_sampleRate) : 0.0;
}

// ---- DeEsser -----------------------------------------------------------------------------------------------------

DeEsser::DeEsser(int channels, double sampleRate) noexcept
    : m_channels(std::clamp(channels, 1, kMaxDspChannels)), m_sampleRate(sampleRate > 0.0 ? sampleRate : 48000.0) {
    m_attack = timeCoefficient(0.5, m_sampleRate);
    m_release = timeCoefficient(60.0, m_sampleRate);
    m_settings.frequency = 0.0; // force the first setSettings to compute
    setSettings(DeEsserSettings{});
}

void DeEsser::setSettings(const DeEsserSettings& s) noexcept {
    if (s == m_settings) {
        return;
    }
    if (s.frequency != m_settings.frequency) {
        m_highPass = highPass(m_sampleRate, s.frequency);
    }
    m_settings = s;
}

void DeEsser::process(float* io, int frames) noexcept {
    const auto ch = static_cast<std::size_t>(m_channels);
    const double threshold = m_settings.thresholdDb;
    const double most = std::max(m_settings.maxReductionDb, 0.0);
    const double slope = 1.0 - 1.0 / kRatio;
    std::array<float, kMaxDspChannels> band{};
    for (int i = 0; i < frames; ++i) {
        float* frame = io + static_cast<std::size_t>(i) * ch;
        double peak = 0.0;
        for (std::size_t c = 0; c < ch; ++c) {
            band[c] = m_state[c].process(m_highPass, frame[c]);
            peak = std::max(peak, static_cast<double>(std::fabs(band[c])));
        }
        const double a = peak > m_envelope ? m_attack : m_release;
        m_envelope = a * m_envelope + (1.0 - a) * peak;
        if (m_envelope < 1e-12) {
            m_envelope = 0.0;
        }
        const double over = linearToDb(m_envelope) - threshold;
        m_reductionDb = over > 0.0 ? std::min(over * slope, most) : 0.0;
        if (m_reductionDb > 0.0) {
            const double keep = 1.0 - dbToLinear(-m_reductionDb); // how much of the band is taken out
            for (std::size_t c = 0; c < ch; ++c) {
                frame[c] = static_cast<float>(frame[c] - keep * band[c]);
            }
        }
    }
}

void DeEsser::reset() noexcept {
    for (BiquadState& s : m_state) {
        s.reset();
    }
    m_envelope = 0.0;
    m_reductionDb = 0.0;
}

int DeEsser::settleFrames() const noexcept { return static_cast<int>(std::ceil(0.3 * m_sampleRate)); } // 5 releases

// ---- Expander ----------------------------------------------------------------------------------------------------

double expanderGainDb(double levelDb, const ExpanderSettings& s) noexcept {
    const double under = s.thresholdDb - levelDb;
    if (under <= 0.0 || s.ratio <= 1.0) {
        return 0.0;
    }
    return -std::min(under * (s.ratio - 1.0), std::max(s.rangeDb, 0.0));
}

Expander::Expander(int channels, double sampleRate) noexcept
    : m_channels(std::clamp(channels, 1, kMaxDspChannels)), m_sampleRate(sampleRate > 0.0 ? sampleRate : 48000.0) {
    m_detectorRelease = timeCoefficient(50.0, m_sampleRate);
    m_settings.ratio = 0.0; // force the first setSettings to compute
    setSettings(ExpanderSettings{});
}

void Expander::setSettings(const ExpanderSettings& s) noexcept {
    if (s == m_settings) {
        return;
    }
    m_settings = s;
    m_attack = timeCoefficient(s.attackMs, m_sampleRate);
    m_release = timeCoefficient(s.releaseMs, m_sampleRate);
}

void Expander::process(float* io, int frames) noexcept {
    const auto ch = static_cast<std::size_t>(m_channels);
    const bool expands = m_settings.ratio > 1.0 && m_settings.rangeDb > 0.0;
    for (int i = 0; i < frames; ++i) {
        float* frame = io + static_cast<std::size_t>(i) * ch;
        double peak = 0.0;
        for (std::size_t c = 0; c < ch; ++c) {
            peak = std::max(peak, static_cast<double>(std::fabs(frame[c])));
        }
        m_envelope = peak > m_envelope ? peak : m_detectorRelease * m_envelope + (1.0 - m_detectorRelease) * peak;
        if (m_envelope < 1e-12) {
            m_envelope = 0.0;
        }
        const double target = expands ? expanderGainDb(linearToDb(m_envelope), m_settings) : 0.0;
        const double a = target > m_gainDb ? m_attack : m_release;
        m_gainDb = a * m_gainDb + (1.0 - a) * target;
        if (m_gainDb > -1e-9) {
            m_gainDb = 0.0; // settle exactly open
        }
        if (m_gainDb != 0.0) {
            const double g = dbToLinear(m_gainDb);
            for (std::size_t c = 0; c < ch; ++c) {
                frame[c] = static_cast<float>(frame[c] * g);
            }
        }
    }
}

void Expander::reset() noexcept {
    m_envelope = 0.0;
    m_gainDb = 0.0;
}

int Expander::settleFrames() const noexcept {
    const double ms = std::min(5.0 * std::max(m_settings.releaseMs, m_settings.attackMs), 2000.0);
    return static_cast<int>(std::ceil(ms * 1e-3 * m_sampleRate));
}

} // namespace wabcut::audio::dsp
