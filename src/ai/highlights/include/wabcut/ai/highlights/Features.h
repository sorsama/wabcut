#pragma once

// G.2 gameplay highlights, step 1 (docs/ai.md "Gameplay highlights"): what a recording sounds and looks like, every
// 100 ms, measured on this computer. Pure C++ (no decoder): wabcut_ai_highlights_media feeds these extractors with
// decoded audio and small pictures; tests feed them synthetic signals.
//
//   AudioFeatureExtractor   one audio stream (mono, 16 kHz): loudness, voicing and pitch (speech-like sound),
//                           laughter-like bursts
//   VisualFeatureExtractor  one video stream (small RGB pictures): motion, brightness flashes, picture changes
//
// Both report in media time from the stream's origin: sample i covers [i, i + 1) x 100 ms.

#include <wabcut/ai/analysis/ShotDetection.h>
#include <wabcut/model/Time.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

namespace wabcut::ai::highlights {

/// Feature samples per second (one per 100 ms).
inline constexpr int kFeatureRate = 10;
/// The audio analysis rate (mono).
inline constexpr int kAudioRate = 16000;
/// Level of silence, dBFS.
inline constexpr float kSilenceDb = -100.0f;

/// Sample index of media time `t` (floor; negative times clamp to 0).
std::size_t featureIndex(Ticks t);
/// Media time of the start of sample `i`.
Ticks featureTime(std::size_t i);

/// Per-100 ms features of one audio stream.
struct AudioFeatures {
    std::vector<float> levelDb;  ///< RMS over the last 400 ms, dBFS (kSilenceDb for digital silence)
    std::vector<float> voiced;   ///< 0..1: share of the hop's 20 ms frames that are voiced (periodic at 70-500 Hz)
    std::vector<float> pitchHz;  ///< median pitch of the voiced frames in the hop; 0 = none
    std::vector<float> burst;    ///< 0..1: laughter-like 3-7 Hz bursts of loudness over the last second

    std::size_t size() const { return levelDb.size(); }
    bool operator==(const AudioFeatures&) const = default;
};

class AudioFeatureExtractor {
public:
    /// `sampleRate`: of the samples given to add() (resampled to nothing: 16 kHz is expected, 8-48 kHz work).
    explicit AudioFeatureExtractor(int sampleRate = kAudioRate);
    /// Mono samples in order, any chunk size.
    void add(std::span<const float> mono);
    /// The features of everything added (the last partial 100 ms counts when it has at least 20 ms).
    AudioFeatures finish();

private:
    void hop();
    void frame(std::span<const float> samples);

    int m_rate;
    std::size_t m_hopSamples;
    std::size_t m_frameSamples; // 20 ms pitch frames, 40 ms analysis window
    std::vector<float> m_pending;
    std::deque<float> m_energy10ms;   // mean square per 10 ms over the last second (bursts)
    std::deque<double> m_hopEnergy;   // sum of squares and count per hop for the 400 ms level
    std::deque<std::size_t> m_hopCounts;
    std::vector<float> m_window;      // the last 40 ms (pitch analysis window)
    std::vector<float> m_decimated;   // scratch
    std::size_t m_frameFill = 0;
    double m_tenMsSum = 0.0;
    std::size_t m_tenMsCount = 0;
    double m_hopSum = 0.0;
    std::size_t m_hopCount = 0;
    int m_hopFrames = 0;
    int m_hopVoiced = 0;
    std::vector<float> m_hopPitches;
    AudioFeatures m_out;
};

/// Per-100 ms features of one video stream.
struct VisualFeatures {
    std::vector<float> motion; ///< mean absolute luma change between consecutive frames, 0..1 (mean over the hop)
    std::vector<float> luma;   ///< mean luma, 0..1
    std::vector<float> flash;  ///< 0..1: how far the hop's brightest frame is above the half-second before it
    std::vector<float> cut;    ///< 1 when the picture changed completely within the hop (a cut, a respawn, a menu)

    std::size_t size() const { return motion.size(); }
    bool operator==(const VisualFeatures&) const = default;
};

class VisualFeatureExtractor {
public:
    /// Frames in display order. Gaps (dropped frames, variable frame rate) are fine: each frame counts in the hop
    /// that holds its start time.
    void add(const analysis::AnalysisFrame& frame);
    VisualFeatures finish();

private:
    void closeHopsBefore(std::size_t index);

    std::vector<std::uint8_t> m_previous; // luma of the previous frame (downsampled to <= 64 px wide)
    int m_prevWidth = 0;
    int m_prevHeight = 0;
    std::vector<int> m_prevHistogram;
    std::size_t m_hop = 0;      // the hop being filled
    bool m_started = false;
    double m_motionSum = 0.0;
    int m_motionCount = 0;
    double m_lumaSum = 0.0;
    double m_lumaMax = 0.0;
    int m_lumaCount = 0;
    bool m_cut = false;
    std::deque<float> m_recentLuma; // the hops' mean luma over the last 500 ms
    VisualFeatures m_out;
};

} // namespace wabcut::ai::highlights
