#pragma once

// Loudness measurement (EBU R128 / ITU-R BS.1770-4) through libebur128: integrated loudness, loudness range, true
// and sample peak, and the loudest momentary / short-term values. Not real-time: libebur128 allocates as it
// accumulates gating blocks. Used by the analysis jobs (playback/audio/Loudness.h).
// 0.6 (P1 round, #54, loudness QC): also where - the frame the loudest momentary (400 ms) and short-term (3 s) windows
// start, the 100 ms block with the highest true peak, and every 100 ms block whose true peak reaches kPeakBlockFloorDbtp
// (so true-peak overs above any ceiling from -6 dBTP up can be listed afterwards).

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace wabcut::audio::dsp {

/// 0.6 (#54): blocks at or above this true peak are kept (LoudnessResult::peakBlocks), at most kMaxPeakBlocks of them.
inline constexpr double kPeakBlockFloorDbtp = -6.0;
inline constexpr std::size_t kMaxPeakBlocks = 100000;

/// 0.6 (#54): the true peak of one 100 ms block of the programme.
struct PeakBlock {
    std::int64_t frame = 0;   ///< its first frame
    std::int64_t frames = 0;  ///< its length (100 ms; the last one may be shorter)
    double truePeakDbtp = -std::numeric_limits<double>::infinity();
};

/// What a measurement found. Levels are -infinity for digital silence (and for programmes shorter than one
/// 400 ms gating block).
struct LoudnessResult {
    double integratedLufs = -std::numeric_limits<double>::infinity();
    double rangeLu = 0.0;
    double truePeakDbtp = -std::numeric_limits<double>::infinity();
    double samplePeakDbfs = -std::numeric_limits<double>::infinity();
    double maxMomentaryLufs = -std::numeric_limits<double>::infinity();  ///< 400 ms window
    double maxShortTermLufs = -std::numeric_limits<double>::infinity();  ///< 3 s window
    std::int64_t frames = 0;
    /// 0.6 (#54): the frame where the loudest momentary (400 ms) / short-term (3 s) window starts, and the first frame
    /// of the 100 ms block holding the highest true peak; -1 when there is none (silence, too short).
    std::int64_t maxMomentaryFrame = -1;
    std::int64_t maxShortTermFrame = -1;
    std::int64_t truePeakFrame = -1;
    /// The 100 ms blocks whose true peak reached kPeakBlockFloorDbtp, in order (the first kMaxPeakBlocks).
    std::vector<PeakBlock> peakBlocks;

    /// Integrated loudness measured (not silence or too short).
    bool hasIntegrated() const noexcept { return integratedLufs > -1e300; }
};

class LoudnessMeter {
public:
    /// Channels map as L, R, C, LFE (not weighted), Ls, Rs; a single channel counts as dual mono (EBU R128 s1).
    LoudnessMeter(int channels, int sampleRate);
    ~LoudnessMeter();
    LoudnessMeter(LoudnessMeter&&) noexcept;
    LoudnessMeter& operator=(LoudnessMeter&&) noexcept;

    int channels() const noexcept { return m_channels; }
    int sampleRate() const noexcept { return m_sampleRate; }
    bool valid() const noexcept;

    /// Adds `frames` interleaved frames.
    void add(const float* interleaved, std::size_t frames);
    LoudnessResult result() const;
    void reset();

    // Live reads (A.3, the player's loudness tap): cheap, unlike result() they skip the loudness range. Levels are
    // -infinity for silence or before the first audio.
    /// Loudness of the last 400 ms (momentary) / 3 s (short-term) added; libebur128's windows, so a programme shorter
    /// than the window reads as if it were preceded by silence.
    double momentaryLufs() const;
    double shortTermLufs() const;
    /// Integrated (gated) loudness of everything added since the last reset.
    double integratedLufs() const;
    /// Highest true peak (dBTP, 4x oversampled) of everything added, over all channels.
    double truePeakDbtp() const;
    /// The loudest momentary / short-term values so far, read every 100 ms.
    double maxMomentaryLufs() const;
    double maxShortTermLufs() const;
    std::int64_t frames() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    int m_channels = 2;
    int m_sampleRate = 48000;
};

/// The gain (dB) that moves `measuredLufs` to `targetLufs`; 0 when nothing was measured.
double normalizationGainDb(const LoudnessResult& measured, double targetLufs) noexcept;

} // namespace wabcut::audio::dsp
