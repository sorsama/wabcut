#pragma once

// Music Remix (A.7, docs/ai.md "Music Remix"; Premiere's Remix): a music clip retimed to a target duration by playing
// its own sections in another order (repeating or skipping bars), never by stretching it.
//
//   1. Beats and bars: the beat tracker's beats and downbeats (C.5, ai/analysis BeatDetection.h).
//   2. Beat-synchronous features: per beat, a 12-bin chroma (harmony) and 13 MFCCs (timbre) averaged over the beat,
//      and its RMS level (own STFT: 2048-point Hann frames, hop 512, at the analysis rate).
//   3. Self-similarity: a jump from beat a to beat j is seamless when the music around a sounds like the music around
//      j, so its cost is the mean feature distance of the beats before and after the two (one bar each way); jumps off
//      the bar grid cost more. A join's confidence (0..1) is 1 - cost / the typical cost of two unrelated beats.
//   4. Sections: a novelty curve over the bars (at each bar line, how far the mean timbre and level of the four bars
//      before it are from those of the four after it, a block form of Foote's checkerboard kernel) finds the section
//      boundaries; the intro is the music up to the first one (or the first two bars), the outro the music from the
//      last one (or the last two bars). Both are played once, as they are: no jump starts or lands inside them.
//   5. The plan: the jump sequence (each piece at least a bar long) whose length is nearest the target (within half a
//      beat when the beats allow) at the lowest total cost: an exhaustive search of the shapes remixes take (one jump,
//      a loop taken several times, either with one more jump), and a beam search in play order for the rest.
//
// The result is a list of source pieces played back to back with a short crossfade at each join, centred just
// before the landing beat so its attack is untouched (audio.remix puts them on the timeline as clips with constant-gain
// transitions, or renders them as one take). Pure: the analysis of a file is the host's (MusicAnalyzer).

#include <wabcut/model/Error.h>
#include <wabcut/model/Time.h>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::audio::edit {

struct BeatFeatures {
    std::array<float, 12> chroma{}; ///< L2-normalised
    std::array<float, 13> mfcc{};   ///< c1..c13 of the log-mel spectrum
    float rms = 0.0f;
    bool operator==(const BeatFeatures&) const = default;
};

/// What a remix needs to know about a piece of music (its whole file, in source time).
struct MusicAnalysis {
    static constexpr std::string_view kAnalyzer = "wabcut-remix-1";
    double bpm = 0.0;
    float beatConfidence = 0.0f;
    int beatsPerBar = 4;
    std::vector<Ticks> beats;         ///< sorted
    std::vector<Ticks> downbeats;     ///< a subset of beats
    std::vector<BeatFeatures> features; ///< features[i] describes [beats[i], beats[i + 1]); beats.size() - 1 entries
    Ticks duration;
    bool operator==(const MusicAnalysis&) const = default;
};

/// Analysis rate of beatFeatures() (and of the beat tracker).
inline constexpr int kRemixAnalysisRate = 22050;
/// Cuts are on a grid of whole samples at 44.1, 48, 88.2 and 96 kHz alike (1/300 s, the rates' greatest common divisor):
/// every piece then plays its music a whole number of samples away from where the clip played it, so a remix is sample
/// exact at those rates.
inline constexpr std::int64_t kRemixCutGrid = kTicksPerSecond / 300;
static_assert(kTicksPerSecond % 300 == 0);

/// The features of the beats of mono audio (`samples` from source time 0 at `sampleRate`).
std::vector<BeatFeatures> beatFeatures(std::span<const float> samples, int sampleRate, std::span<const Ticks> beats);

struct RemixOptions {
    Ticks target;               ///< the wanted duration
    bool keepIntro = true;
    bool keepOutro = true;
    Ticks crossfade{-1};        ///< join crossfade; negative: an eighth of a beat, 10..50 ms
    int maxJoins = 48;
    int minPieceBeats = 0;      ///< shortest piece between two joins; 0: one bar
};

/// A piece of the source, played in full: [sourceStart, sourceEnd).
struct RemixSegment {
    Ticks sourceStart;
    Ticks sourceEnd;
    Ticks duration() const { return sourceEnd - sourceStart; }
    bool operator==(const RemixSegment&) const = default;
};

/// A join between piece `index` and piece `index + 1`.
struct RemixJoin {
    std::size_t index = 0;
    Ticks at;          ///< from the remix's start: where the next piece starts (the cut)
    Ticks fromBeat;    ///< source time of the beat jumped from (where the music would have gone on)
    Ticks toBeat;      ///< source time of the beat landed on
    double confidence = 0.0;
    bool onBar = true; ///< both beats in the same place in the bar
};

struct RemixPlan {
    std::vector<RemixSegment> segments; ///< back to back; the first starts at the range's start, the last ends at its end
    std::vector<RemixJoin> joins;
    Ticks duration;
    Ticks target;
    Ticks crossfade;
    Ticks introEnd;   ///< source time: the intro is [range start, introEnd)
    Ticks outroStart; ///< source time: the outro is [outroStart, range end)
    double beatSeconds = 0.0;
    double cost = 0.0;
    /// How far the plan is from the target (positive: longer).
    Ticks error() const { return duration - target; }
};

/// Plans a remix of the source range `range` (a clip's [sourceIn, sourceOut)) of `music` to `options.target`. Errors:
/// InvalidArgument (no beats, a target too short to keep the intro and the outro, a range without enough music).
std::expected<RemixPlan, Error> planRemix(const MusicAnalysis& music, TimeRange range, const RemixOptions& options);

/// Renders a plan from `source` (interleaved, `channels`, from source time 0 at `sampleRate`): the pieces back to back,
/// each join crossfaded with constant gain over the plan's crossfade centred on the cut, as the timeline plays the
/// clips audio.remix places. Returns the interleaved result (plan.duration long).
std::vector<float> renderRemix(const RemixPlan& plan, std::span<const float> source, int channels, int sampleRate);

} // namespace wabcut::audio::edit
