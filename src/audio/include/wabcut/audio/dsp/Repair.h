#pragma once

// The repair effects of Essential Sound (A.7): the high/low-cut filter (wabcut.audio.filter: rumble removal, the
// "Phone" and "Radio" bands), DeHum (wabcut.audio.dehum), the de-esser (wabcut.audio.deess) and the downward expander
// (wabcut.audio.expander: reverb tails and room noise between words). Own implementations on the cookbook sections of
// Biquad.h; every class is RT-safe after construction (no allocation, no locks) and processes interleaved frames.

#include <wabcut/audio/dsp/Biquad.h>
#include <wabcut/audio/dsp/Equalizer.h> // kMaxDspChannels

#include <array>

namespace wabcut::audio::dsp {

/// High-pass and low-pass Butterworth sections at the cut frequencies. Off at the ends of their ranges: a low cut at
/// or below kLowCutOff and a high cut at or above kHighCutOff (or 0.45 of the rate) pass the audio untouched.
class CutFilter {
public:
    static constexpr double kLowCutOff = 20.0;
    static constexpr double kHighCutOff = 20000.0;
    static constexpr int kMaxSections = 4; ///< per side: 48 dB per octave

    /// `slopeDbPerOctave` 12, 24, 36 or 48 (rounded to one of them): 1 to 4 sections per side.
    CutFilter(int channels, double sampleRate, double slopeDbPerOctave) noexcept;
    void setFrequencies(double lowCutHz, double highCutHz) noexcept;
    void process(float* interleaved, int frames) noexcept;
    void reset() noexcept;
    /// The combined magnitude response in dB.
    double magnitudeDb(double frequency) const noexcept;
    int sections() const noexcept { return m_sections; }

private:
    int m_channels;
    double m_sampleRate;
    int m_sections;
    double m_low = -1.0;
    double m_high = -1.0;
    bool m_lowOn = false;
    bool m_highOn = false;
    std::array<BiquadCoefficients, kMaxSections> m_lowCoefficients{};
    std::array<BiquadCoefficients, kMaxSections> m_highCoefficients{};
    std::array<std::array<BiquadState, 2 * kMaxSections>, kMaxDspChannels> m_state{};
};

/// Mains hum removal: a narrow cut of `reductionDb` at the hum frequency and its harmonics (notch() sections of q
/// kNotchQ: exactly -reductionDb at each harmonic, a bandwidth of a twelfth of it). 0 dB is bit-exact identity.
class DeHum {
public:
    static constexpr int kMaxHarmonics = 8;
    static constexpr double kNotchQ = 12.0;

    /// `frequency` 40..70 Hz (50, 60, 59.94); `harmonics` 1..kMaxHarmonics (the fundamental counts as the first).
    DeHum(int channels, double sampleRate, double frequency, int harmonics) noexcept;
    void setReduction(double reductionDb) noexcept;
    void process(float* interleaved, int frames) noexcept;
    void reset() noexcept;
    double magnitudeDb(double frequency) const noexcept;
    int harmonics() const noexcept { return m_harmonics; }

private:
    int m_channels;
    double m_sampleRate;
    double m_frequency;
    int m_harmonics; ///< sections in use (harmonics below 0.45 of the rate)
    double m_reduction = -1.0;
    std::array<BiquadCoefficients, kMaxHarmonics> m_coefficients{};
    std::array<std::array<BiquadState, kMaxHarmonics>, kMaxDspChannels> m_state{};
};

struct DeEsserSettings {
    double thresholdDb = -30.0;   ///< level of the sibilant band where reduction starts
    double maxReductionDb = 10.0; ///< most the band goes down
    double frequency = 5500.0;    ///< the sibilant band starts here (second-order high-pass)
    bool operator==(const DeEsserSettings&) const = default;
};

/// Split-band de-esser: the band above `frequency` (a high-pass of the signal; the rest is the signal minus it, so the
/// two always sum back to the input) goes down by 3/4 of its level over the threshold, at most maxReductionDb. The
/// detector follows the band's peak (stereo-linked) with a 0.5 ms attack and a 60 ms release. Below the threshold the
/// output is the input exactly.
class DeEsser {
public:
    static constexpr double kRatio = 4.0;

    DeEsser(int channels, double sampleRate) noexcept;
    void setSettings(const DeEsserSettings& settings) noexcept;
    void process(float* interleaved, int frames) noexcept;
    void reset() noexcept;
    /// Reduction of the band at the end of the last block, dB (>= 0).
    double reductionDb() const noexcept { return m_reductionDb; }
    int settleFrames() const noexcept;

private:
    int m_channels;
    double m_sampleRate;
    DeEsserSettings m_settings;
    BiquadCoefficients m_highPass{};
    std::array<BiquadState, kMaxDspChannels> m_state{};
    double m_attack = 0.0;
    double m_release = 0.0;
    double m_envelope = 0.0; ///< the band's peak level, linear
    double m_reductionDb = 0.0;
};

struct ExpanderSettings {
    double thresholdDb = -45.0;
    double ratio = 2.0;     ///< below the threshold each dB of level drop becomes `ratio` dB (1 = off)
    double rangeDb = 12.0;  ///< most reduction
    double attackMs = 2.0;  ///< opening (the gain rising back to 0 dB)
    double releaseMs = 150.0;
    bool operator==(const ExpanderSettings&) const = default;
};

/// The static curve: gain (dB, <= 0) for a steady level (dB).
double expanderGainDb(double levelDb, const ExpanderSettings& settings) noexcept;

/// Downward expander: quiet passages (a room's reverb tail after a word, noise between words) go further down;
/// speech above the threshold passes untouched. Stereo-linked peak detector (instant attack, 50 ms release) and the
/// gain smoothed in the log domain, opening with the attack and closing with the release time constant.
class Expander {
public:
    Expander(int channels, double sampleRate) noexcept;
    void setSettings(const ExpanderSettings& settings) noexcept;
    void process(float* interleaved, int frames) noexcept;
    void reset() noexcept;
    double gainDb() const noexcept { return m_gainDb; }
    int settleFrames() const noexcept;

private:
    int m_channels;
    double m_sampleRate;
    ExpanderSettings m_settings;
    double m_attack = 0.0;
    double m_release = 0.0;
    double m_detectorRelease = 0.0;
    double m_envelope = 0.0;
    double m_gainDb = 0.0;
};

} // namespace wabcut::audio::dsp
