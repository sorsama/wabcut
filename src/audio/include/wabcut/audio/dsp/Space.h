#pragma once

// The space effects of Essential Sound (A.7): an algorithmic reverb (wabcut.audio.reverb) and stereo width
// (wabcut.audio.width). Own implementations (no third-party code):
//
//   * Reverb: a feedback delay network after Jot and Chaigne (1991), "Digital delay networks for designing artificial
//     reverberators": a predelay, four Schroeder all-pass diffusers, eight delay lines (prime lengths scaled by the
//     room size) fed back through a Householder matrix (I - 2/N 11^T, lossless), each line with a one-pole absorption
//     filter whose gain at DC and at Nyquist gives the decay time asked (RT60) and a shorter one at high frequencies
//     (damping). Left and right read the lines with orthogonal sign patterns, so the tails are decorrelated; width
//     narrows them towards mono. Deterministic, sample by sample: preview and export render the same samples.
//   * Stereo width: mid/side. The side signal is scaled by the width (0 = mono, 1 = unchanged, 2 = twice as wide);
//     below the bass-mono frequency it is removed. The mid signal is never touched, so the mono fold-down (L + R) of
//     the output is the input's, whatever the width: always mono compatible.
//
// RT-safe after construction: the constructors allocate the delay lines; process() and reset() never allocate.

#include <wabcut/audio/dsp/Biquad.h>
#include <wabcut/audio/dsp/Equalizer.h> // kMaxDspChannels

#include <array>
#include <cstdint>
#include <vector>

namespace wabcut::audio::dsp {

struct ReverbSettings {
    double decayMs = 1500.0; ///< RT60 at low frequencies
    double damping = 0.5;    ///< 0 = the same decay at all frequencies .. 1 = highs decay 10 times faster
    double mix = 0.25;       ///< 0 = dry .. 1 = only the reverb
    double width = 1.0;      ///< 0 = a mono tail .. 1 = fully decorrelated left and right
    bool operator==(const ReverbSettings&) const = default;
};

class Reverb {
public:
    static constexpr int kLines = 8;
    static constexpr int kDiffusers = 4;
    static constexpr double kMaxPredelayMs = 200.0;

    /// `size` 0..1 (a small room .. a hall: the delay lines' lengths), `predelayMs` 0..kMaxPredelayMs. Allocates.
    Reverb(int channels, double sampleRate, double size, double predelayMs);
    void setSettings(const ReverbSettings& settings) noexcept;
    const ReverbSettings& settings() const noexcept { return m_settings; }
    void process(float* interleaved, int frames) noexcept;
    void reset() noexcept;
    /// Input that brings a reset reverb to its steady tail: the decay time, at most 3 s.
    int settleFrames() const noexcept;
    /// The delay lines' lengths in frames (tests).
    std::array<int, kLines> lineLengths() const noexcept;

private:
    struct Delay {
        std::vector<double> buffer;
        int position = 0;
    };
    struct Line {
        Delay delay;
        double a = 0.0;     ///< absorption one-pole: y = b x + a y[n-1]
        double b = 1.0;
        double state = 0.0;
    };
    struct Diffuser {
        Delay delay;
        double g = 0.7;
    };

    void computeAbsorption() noexcept;

    int m_channels;
    double m_sampleRate;
    ReverbSettings m_settings;
    Delay m_predelay;
    int m_predelayFrames = 0;
    std::array<Diffuser, kDiffusers> m_diffusers;
    std::array<Line, kLines> m_lines;
    std::int64_t m_quiet = 0;  ///< consecutive frames with no input and no tail (flushes the state)
    bool m_silent = true;      ///< the state is exactly zero
};

/// Mid/side width with an optional mono bass. Channels other than the first two pass through; mono input is untouched.
class StereoWidth {
public:
    StereoWidth(int channels, double sampleRate) noexcept;
    /// `width` 0..2; `bassMonoHz` 0 = off, else the side signal below it (second-order high-pass) is removed.
    void setSettings(double width, double bassMonoHz) noexcept;
    void process(float* interleaved, int frames) noexcept;
    void reset() noexcept;

private:
    int m_channels;
    double m_sampleRate;
    double m_width = 1.0;
    double m_bassMono = 0.0;
    BiquadCoefficients m_sideHighPass{};
    BiquadState m_sideState;
};

} // namespace wabcut::audio::dsp
