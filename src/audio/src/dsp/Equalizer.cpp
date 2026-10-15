#include <wabcut/audio/dsp/Equalizer.h>

#include <algorithm>
#include <cmath>

namespace wabcut::audio::dsp {

Eq3::Eq3(int channels, double sampleRate) noexcept
    : m_channels(std::clamp(channels, 1, kMaxDspChannels)), m_sampleRate(sampleRate > 0.0 ? sampleRate : 48000.0) {
    m_settings.lowGainDb = 1.0; // force the first setSettings to compute
    setSettings(Eq3Settings{});
}

void Eq3::setSettings(const Eq3Settings& s) noexcept {
    if (s == m_settings) {
        return;
    }
    m_settings = s;
    m_coefficients[0] = lowShelf(m_sampleRate, s.lowFrequency, s.lowGainDb);
    m_coefficients[1] = peaking(m_sampleRate, s.midFrequency, s.midGainDb, s.midQ);
    m_coefficients[2] = highShelf(m_sampleRate, s.highFrequency, s.highGainDb);
    // At 0 dB every section is exactly the identity, and a zero state stays exactly zero: skipping is the same.
    m_flat = s.lowGainDb == 0.0 && s.midGainDb == 0.0 && s.highGainDb == 0.0;
    if (!m_flat) {
        m_quiet = false;
    }
}

void Eq3::process(float* interleaved, int frames) noexcept {
    if (m_flat && m_quiet) {
        return;
    }
    const auto ch = static_cast<std::size_t>(m_channels);
    for (int i = 0; i < frames; ++i) {
        float* frame = interleaved + static_cast<std::size_t>(i) * ch;
        for (std::size_t c = 0; c < ch; ++c) {
            auto& s = m_state[c];
            float x = frame[c];
            x = s[0].process(m_coefficients[0], x);
            x = s[1].process(m_coefficients[1], x);
            frame[c] = s[2].process(m_coefficients[2], x);
        }
    }
    // Flush state far below the float floor (it would decay into denormals during long silences). Identity
    // sections still ring out what earlier settings left; once that is gone the flat EQ is skipped again.
    bool quiet = true;
    for (std::size_t c = 0; c < ch; ++c) {
        for (BiquadState& s : m_state[c]) {
            if (std::abs(s.z1) < 1e-30 && std::abs(s.z2) < 1e-30) {
                s.reset();
            } else {
                quiet = false;
            }
        }
    }
    m_quiet = quiet;
}

void Eq3::reset() noexcept {
    for (auto& channel : m_state) {
        for (BiquadState& s : channel) {
            s.reset();
        }
    }
    m_quiet = true;
}

double Eq3::magnitudeDb(double frequency) const noexcept {
    double db = 0.0;
    for (const BiquadCoefficients& c : m_coefficients) {
        db += dsp::magnitudeDb(c, frequency, m_sampleRate);
    }
    return db;
}

} // namespace wabcut::audio::dsp
