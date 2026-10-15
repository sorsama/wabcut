#pragma once

// The downward compressor of wabcut.audio.compressor: a feed-forward, stereo-linked peak compressor with a soft knee
// and attack/release smoothing of the gain in the log domain (Giannoulis, Massberg and Reiss, "Digital Dynamic Range
// Compressor Design - A Tutorial and Analysis", JAES 2012: the smooth decoupled peak detector).

namespace wabcut::audio::dsp {

struct CompressorSettings {
    double thresholdDb = -20.0;
    double ratio = 4.0;      ///< 4 = 4:1; 1 = no compression
    double attackMs = 10.0;  ///< time constant of the gain reduction rising (1 - 1/e of a step)
    double releaseMs = 100.0;
    double makeupDb = 0.0;
    double kneeDb = 6.0;     ///< soft-knee width centred on the threshold (0 = hard knee)

    bool operator==(const CompressorSettings&) const = default;
};

/// The static curve: the output level (dB, before makeup) for a steady input level (dB).
double compressorCurveDb(double inputDb, const CompressorSettings& settings) noexcept;

/// RT-safe after construction.
class Compressor {
public:
    Compressor(int channels, double sampleRate) noexcept;

    void setSettings(const CompressorSettings& settings) noexcept;
    const CompressorSettings& settings() const noexcept { return m_settings; }

    void process(float* interleaved, int frames) noexcept;
    void reset() noexcept;

    /// Gain reduction at the end of the last block, dB (>= 0).
    double gainReductionDb() const noexcept { return -m_gainDb; }
    /// Input that settles the detector after a reset: five release time constants (capped at 2 s).
    int settleFrames() const noexcept;

private:
    int m_channels;
    double m_sampleRate;
    CompressorSettings m_settings;
    double m_attackCoefficient = 0.0;
    double m_releaseCoefficient = 0.0;
    double m_makeup = 1.0;
    double m_gainDb = 0.0; ///< smoothed gain (<= 0 dB)
};

} // namespace wabcut::audio::dsp
