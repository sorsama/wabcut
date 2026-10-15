#pragma once

// The 3-band parametric EQ of wabcut.audio.eq3: low shelf, peaking mid band, high shelf.

#include <wabcut/audio/dsp/Biquad.h>

#include <array>

namespace wabcut::audio::dsp {

/// Most channels a DSP block processes (the mixer's limit).
inline constexpr int kMaxDspChannels = 8;

struct Eq3Settings {
    double lowFrequency = 100.0;
    double lowGainDb = 0.0;
    double midFrequency = 1000.0;
    double midGainDb = 0.0;
    double midQ = 1.0;
    double highFrequency = 8000.0;
    double highGainDb = 0.0;

    bool operator==(const Eq3Settings&) const = default;
};

/// RT-safe after construction. With all three gains at 0 dB the EQ is bit-exact identity, and once the state left by
/// earlier settings has decayed it costs nothing.
class Eq3 {
public:
    /// `channels` is clamped to [1, kMaxDspChannels].
    Eq3(int channels, double sampleRate) noexcept;

    /// Recomputes the coefficients when the settings changed. Changing coefficients keeps the filter state, so
    /// automation sweeps are smooth.
    void setSettings(const Eq3Settings& settings) noexcept;
    const Eq3Settings& settings() const noexcept { return m_settings; }

    void process(float* interleaved, int frames) noexcept;
    void reset() noexcept;

    /// The combined magnitude response in dB.
    double magnitudeDb(double frequency) const noexcept;

private:
    int m_channels;
    double m_sampleRate;
    Eq3Settings m_settings;
    bool m_flat = true;
    bool m_quiet = true; ///< every state is exactly zero
    std::array<BiquadCoefficients, 3> m_coefficients{};
    std::array<std::array<BiquadState, 3>, kMaxDspChannels> m_state{};
};

} // namespace wabcut::audio::dsp
