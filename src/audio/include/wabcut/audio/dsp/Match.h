#pragma once

// Reference matching, the measuring half (0.7, EQ Match and Level Match; docs/audio.md "Match"). Pure DSP, no project
// types; what is derived from the measurements (the EQ curve, the radar) is wabcut/audio/edit/MatchCurve.h.
//
//   SpectrumAverager   the long-term average spectrum of a signal in the 30 third-octave bands of the Graphic Equalizer
//                      (31.25 Hz x 2^(k / 3)): Hann-windowed FFT frames, the power of the channels averaged (the LFE of a
//                      5.1 signal skipped), only the frames that are not pauses counted (a relative gate under the
//                      loud frames, so a long silence or a noise floor between the words does not shape the average).
//   LevelAccumulator   RMS, sample peak and crest factor (the loudness and the true peak come from LoudnessMeter).

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

namespace wabcut::audio::dsp {

inline constexpr int kSpectrumBands = 30; ///< the Graphic Equalizer (30 Bands) sliders

/// Centre of band `band` (0-based): 31.25 Hz x 2^(band / 3).
double spectrumBandHz(int band) noexcept;

/// The average spectrum of a signal: power per bin, in dB (relative to a full-scale sine at a bin centre), per band; NaN
/// where the band lies above the signal's usable range (0.45 x the sample rate).
struct SpectrumAverage {
    std::array<double, kSpectrumBands> levelDb;
    double seconds = 0.0;       ///< the signal's length
    double activeSeconds = 0.0; ///< of it, the frames that counted
    int sampleRate = 0;

    SpectrumAverage() { levelDb.fill(std::numeric_limits<double>::quiet_NaN()); }
};

class SpectrumAverager {
public:
    /// `channels` 1..8. `stride` N: only every Nth frame is analysed (long programmes: the average of a sample of the
    /// frames is the average), 1..64.
    SpectrumAverager(int channels, int sampleRate, int stride = 1);
    ~SpectrumAverager();
    SpectrumAverager(SpectrumAverager&&) noexcept;
    SpectrumAverager& operator=(SpectrumAverager&&) noexcept;

    /// Adds `frames` interleaved frames.
    void add(const float* interleaved, std::size_t frames);
    /// The average so far (can be called again after more audio).
    SpectrumAverage result() const;

    /// The FFT size used for `sampleRate` (bins at most ~6 Hz wide).
    static int fftSizeFor(int sampleRate) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// RMS (over all non-LFE channels), sample peak and crest factor of what was added.
class LevelAccumulator {
public:
    explicit LevelAccumulator(int channels);
    void add(const float* interleaved, std::size_t frames);
    double rmsDbfs() const noexcept;       ///< -inf for silence
    double samplePeakDbfs() const noexcept;
    double crestDb() const noexcept;       ///< peak - RMS; 0 for silence
    std::int64_t frames() const noexcept { return m_frames; }

private:
    int m_channels;
    std::int64_t m_frames = 0;
    double m_sumSquares = 0.0;
    std::int64_t m_samples = 0;
    double m_peak = 0.0;
};

} // namespace wabcut::audio::dsp
