#include <wabcut/audio/edit/Remix.h>

#include "SyncFft.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
#include <utility>

namespace wabcut::audio::edit {

namespace {

constexpr int kFrame = 2048;
constexpr int kHop = 512;
constexpr int kMelBands = 40;
constexpr double kMelLow = 30.0;
constexpr double kMelHigh = 8000.0;
constexpr double kChromaLow = 55.0;
constexpr double kChromaHigh = 5000.0;

double hzToMel(double f) { return 2595.0 * std::log10(1.0 + f / 700.0); }
double melToHz(double m) { return 700.0 * (std::pow(10.0, m / 2595.0) - 1.0); }

/// Triangular mel filters over the FFT bins: per band, (first bin, weights).
struct MelBank {
    std::vector<int> first;
    std::vector<std::vector<float>> weights;
};

MelBank melBank(int sampleRate) {
    MelBank bank;
    const double high = std::min(kMelHigh, 0.5 * sampleRate);
    const double m0 = hzToMel(kMelLow);
    const double m1 = hzToMel(high);
    std::vector<double> edges(kMelBands + 2);
    for (int i = 0; i < kMelBands + 2; ++i) {
        edges[static_cast<std::size_t>(i)] = melToHz(m0 + (m1 - m0) * i / (kMelBands + 1));
    }
    const double binHz = static_cast<double>(sampleRate) / kFrame;
    for (int b = 0; b < kMelBands; ++b) {
        const double lo = edges[static_cast<std::size_t>(b)];
        const double mid = edges[static_cast<std::size_t>(b) + 1];
        const double hi = edges[static_cast<std::size_t>(b) + 2];
        const int k0 = std::max(1, static_cast<int>(std::ceil(lo / binHz)));
        const int k1 = std::min(kFrame / 2, static_cast<int>(std::floor(hi / binHz)));
        std::vector<float> w;
        for (int k = k0; k <= k1; ++k) {
            const double f = k * binHz;
            const double v = f <= mid ? (f - lo) / std::max(mid - lo, 1e-9) : (hi - f) / std::max(hi - mid, 1e-9);
            w.push_back(static_cast<float>(std::max(v, 0.0)));
        }
        if (w.empty()) { // a band narrower than a bin: the nearest bin
            bank.first.push_back(std::clamp(static_cast<int>(std::lround(mid / binHz)), 1, kFrame / 2));
            bank.weights.push_back({1.0f});
        } else {
            bank.first.push_back(k0);
            bank.weights.push_back(std::move(w));
        }
    }
    return bank;
}

} // namespace

std::vector<BeatFeatures> beatFeatures(std::span<const float> samples, int sampleRate, std::span<const Ticks> beats) {
    std::vector<BeatFeatures> out;
    if (beats.size() < 2 || sampleRate <= 0) {
        return out;
    }
    const std::size_t intervals = beats.size() - 1;
    out.resize(intervals);
    std::vector<std::array<double, 12>> chroma(intervals, std::array<double, 12>{});
    std::vector<std::array<double, 13>> mfcc(intervals, std::array<double, 13>{});
    std::vector<int> frames(intervals, 0);

    // Bin -> pitch class (0 = C), or -1 outside the chroma range.
    std::vector<int> pitchClass(kFrame / 2 + 1, -1);
    const double binHz = static_cast<double>(sampleRate) / kFrame;
    for (int k = 1; k <= kFrame / 2; ++k) {
        const double f = k * binHz;
        if (f >= kChromaLow && f <= kChromaHigh) {
            const auto midi = static_cast<long>(std::lround(69.0 + 12.0 * std::log2(f / 440.0)));
            pitchClass[static_cast<std::size_t>(k)] = static_cast<int>(((midi % 12) + 12) % 12);
        }
    }
    const MelBank bank = melBank(sampleRate);
    // DCT-II basis for c1..c13 of the 40 log-mel energies.
    std::array<std::array<double, kMelBands>, 13> dct{};
    for (int c = 0; c < 13; ++c) {
        for (int b = 0; b < kMelBands; ++b) {
            dct[static_cast<std::size_t>(c)][static_cast<std::size_t>(b)] =
                std::cos(std::numbers::pi * (c + 1) * (b + 0.5) / kMelBands);
        }
    }
    std::vector<float> window(kFrame);
    for (int i = 0; i < kFrame; ++i) {
        window[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / kFrame));
    }

    const detail::SyncFft fft(kFrame);
    std::vector<detail::Cf> buffer(kFrame);
    std::vector<double> power(kFrame / 2 + 1);
    const auto n = static_cast<std::int64_t>(samples.size());
    const auto sampleOf = [&](Ticks t) { return static_cast<std::int64_t>(std::llround(t.toSeconds() * sampleRate)); };
    for (std::size_t beat = 0; beat < intervals; ++beat) {
        // Frames that start on the beat and stay inside it (one centred on it when the beat is shorter than a frame):
        // the same music on a beat gives the same features wherever the beat is, whatever the hop's phase.
        const std::int64_t b0 = sampleOf(beats[beat]);
        const std::int64_t b1 = sampleOf(beats[beat + 1]);
        std::int64_t start = b0;
        std::int64_t last = b1 - kFrame;
        if (last < b0) {
            start = (b0 + b1) / 2 - kFrame / 2;
            last = start;
        }
        for (; start <= last; start += kHop) {
            for (int i = 0; i < kFrame; ++i) {
                const std::int64_t at = start + i;
                const float v = at >= 0 && at < n ? samples[static_cast<std::size_t>(at)] : 0.0f;
                buffer[static_cast<std::size_t>(i)] = detail::Cf(v * window[static_cast<std::size_t>(i)], 0.0f);
            }
            fft.forward(buffer.data());
            for (int k = 0; k <= kFrame / 2; ++k) {
                power[static_cast<std::size_t>(k)] = std::norm(buffer[static_cast<std::size_t>(k)]);
            }
            auto& ch = chroma[beat];
            for (int k = 1; k <= kFrame / 2; ++k) {
                if (const int pc = pitchClass[static_cast<std::size_t>(k)]; pc >= 0) {
                    ch[static_cast<std::size_t>(pc)] += std::sqrt(power[static_cast<std::size_t>(k)]);
                }
            }
            std::array<double, kMelBands> mel{};
            double total = 0.0;
            for (int b = 0; b < kMelBands; ++b) {
                double e = 0.0;
                const auto& w = bank.weights[static_cast<std::size_t>(b)];
                const int k0 = bank.first[static_cast<std::size_t>(b)];
                for (std::size_t i = 0; i < w.size(); ++i) {
                    e += w[i] * power[static_cast<std::size_t>(k0) + i];
                }
                mel[static_cast<std::size_t>(b)] = e;
                total += e;
            }
            // The log floored 80 dB under the frame's energy (and at about -100 dBFS): bands with next to nothing in
            // them must not swing the coefficients with the noise floor.
            const double floor = std::max(total * 1e-8, 1e-5);
            for (double& e : mel) {
                e = std::log10(std::max(e, floor));
            }
            auto& mf = mfcc[beat];
            for (int c = 0; c < 13; ++c) {
                double v = 0.0;
                for (int b = 0; b < kMelBands; ++b) {
                    v += dct[static_cast<std::size_t>(c)][static_cast<std::size_t>(b)] * mel[static_cast<std::size_t>(b)];
                }
                mf[static_cast<std::size_t>(c)] += v;
            }
            ++frames[beat];
        }
    }
    for (std::size_t i = 0; i < intervals; ++i) {
        const std::size_t from = i; // every beat has at least one frame
        const int count = std::max(frames[from], 1);
        double norm = 0.0;
        for (double v : chroma[from]) {
            norm += v * v;
        }
        norm = std::sqrt(norm);
        for (std::size_t k = 0; k < 12; ++k) {
            out[i].chroma[k] = norm > 0.0 ? static_cast<float>(chroma[from][k] / norm) : 0.0f;
        }
        for (std::size_t k = 0; k < 13; ++k) {
            out[i].mfcc[k] = static_cast<float>(mfcc[from][k] / count);
        }
        // RMS of the beat's samples.
        const auto s0 = static_cast<std::int64_t>(std::llround(beats[i].toSeconds() * sampleRate));
        const auto s1 = static_cast<std::int64_t>(std::llround(beats[i + 1].toSeconds() * sampleRate));
        double sum = 0.0;
        std::int64_t count2 = 0;
        for (std::int64_t s = std::max<std::int64_t>(s0, 0); s < std::min(s1, n); ++s) {
            sum += static_cast<double>(samples[static_cast<std::size_t>(s)]) * samples[static_cast<std::size_t>(s)];
            ++count2;
        }
        out[i].rms = count2 > 0 ? static_cast<float>(std::sqrt(sum / static_cast<double>(count2))) : 0.0f;
    }
    return out;
}

namespace {

/// The beat features ready to compare: chroma as given, MFCCs standardised over the piece, levels in dB.
struct Comparable {
    std::vector<std::array<float, 12>> chroma;
    std::vector<std::array<float, 13>> mfcc;
    std::vector<float> levelDb;

    explicit Comparable(const std::vector<BeatFeatures>& f) {
        const std::size_t n = f.size();
        chroma.resize(n);
        mfcc.resize(n);
        levelDb.resize(n);
        std::array<double, 13> mean{};
        std::array<double, 13> var{};
        for (const BeatFeatures& b : f) {
            for (std::size_t k = 0; k < 13; ++k) {
                mean[k] += b.mfcc[k];
            }
        }
        for (double& m : mean) {
            m /= static_cast<double>(std::max<std::size_t>(n, 1));
        }
        for (const BeatFeatures& b : f) {
            for (std::size_t k = 0; k < 13; ++k) {
                var[k] += (b.mfcc[k] - mean[k]) * (b.mfcc[k] - mean[k]);
            }
        }
        for (std::size_t i = 0; i < n; ++i) {
            chroma[i] = f[i].chroma;
            for (std::size_t k = 0; k < 13; ++k) {
                const double sd = std::sqrt(var[k] / static_cast<double>(std::max<std::size_t>(n, 1)));
                mfcc[i][k] = sd > 1e-9 ? static_cast<float>((f[i].mfcc[k] - mean[k]) / sd) : 0.0f;
            }
            levelDb[i] = static_cast<float>(20.0 * std::log10(f[i].rms + 1e-6));
        }
    }

    /// 0 for identical beats; about 1.5 for unrelated ones.
    double distance(std::size_t a, std::size_t b) const {
        double dot = 0.0;
        for (std::size_t k = 0; k < 12; ++k) {
            dot += static_cast<double>(chroma[a][k]) * chroma[b][k];
        }
        double m = 0.0;
        for (std::size_t k = 0; k < 13; ++k) {
            const double d = mfcc[a][k] - mfcc[b][k];
            m += d * d;
        }
        const double level = std::fabs(levelDb[a] - levelDb[b]) / 20.0;
        return (1.0 - std::clamp(dot, 0.0, 1.0)) + 0.5 * std::sqrt(m / 13.0) + 0.5 * std::min(level, 2.0);
    }
};

struct Hop {
    int from = 0; ///< beat jumped from
    int to = 0;   ///< beat landed on
    double cost = 0.0;   ///< normalised: 0 seamless, about 1 for unrelated music
    double raw = 0.0;    ///< without the off-bar penalty (for the confidence)
    bool onBar = true;
    std::int64_t delta = 0; ///< ticks the hop adds (from - to in source time)
};

struct State {
    int position = -1;      ///< the beat the last hop landed on (-1: the range's start)
    std::int64_t added = 0; ///< ticks added so far
    double cost = 0.0;
    int joins = 0;
    int parent = -1;        ///< index into the history
    int hop = -1;           ///< into the hop list
};

double errorPenalty(double errorBeats) {
    const double e = std::fabs(errorBeats);
    return e <= 0.5 ? 0.3 * e : 0.15 + 3.0 * (e - 0.5);
}

} // namespace

std::expected<RemixPlan, Error> planRemix(const MusicAnalysis& music, TimeRange range, const RemixOptions& options) {
    const std::vector<Ticks>& beats = music.beats;
    const auto n = static_cast<int>(beats.size());
    if (n < 8 || music.features.size() + 1 != beats.size()) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "the music has too few beats to remix"));
    }
    if (options.target.value <= 0) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "the target duration must be positive"));
    }
    const Ticks in = range.start;
    const Ticks out = range.end();
    // Beats inside the range.
    const int lo = static_cast<int>(std::lower_bound(beats.begin(), beats.end(), in) - beats.begin());
    const int hi = static_cast<int>(std::lower_bound(beats.begin(), beats.end(), out) - beats.begin()) - 1;
    const int bpb = std::max(music.beatsPerBar, 1);
    if (hi - lo < 4 * bpb) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "the clip has too little music to remix (fewer than four bars)"));
    }
    std::vector<std::int64_t> intervals;
    for (int i = lo; i < hi; ++i) {
        intervals.push_back(beats[static_cast<std::size_t>(i) + 1].value - beats[static_cast<std::size_t>(i)].value);
    }
    std::nth_element(intervals.begin(), intervals.begin() + static_cast<std::ptrdiff_t>(intervals.size() / 2), intervals.end());
    const std::int64_t beatTicks = std::max<std::int64_t>(intervals[intervals.size() / 2], 1);

    RemixPlan plan;
    plan.target = options.target;
    plan.beatSeconds = Ticks{beatTicks}.toSeconds();
    plan.crossfade = options.crossfade.value >= 0
                         ? options.crossfade
                         : Ticks{std::clamp<std::int64_t>(beatTicks / 8, kTicksPerSecond / 100, kTicksPerSecond / 20)};
    const std::int64_t pre = plan.crossfade.value / 2;

    // Where each beat sits in the bar.
    std::vector<int> barPosition(static_cast<std::size_t>(n));
    int firstDownbeat = 0;
    if (!music.downbeats.empty()) {
        firstDownbeat = static_cast<int>(std::lower_bound(beats.begin(), beats.end(), music.downbeats.front()) - beats.begin());
    }
    for (int i = 0; i < n; ++i) {
        barPosition[static_cast<std::size_t>(i)] = ((i - firstDownbeat) % bpb + bpb) % bpb;
    }

    const Comparable features(music.features);
    const int window = bpb;

    // Typical distance of unrelated beats: the median over a fixed spread of pairs.
    std::vector<double> sample;
    std::uint32_t seed = 0x9e3779b9u;
    const int span = hi - lo;
    for (int k = 0; k < 2048; ++k) {
        seed = seed * 1664525u + 1013904223u;
        const int a = lo + static_cast<int>(seed % static_cast<std::uint32_t>(span));
        seed = seed * 1664525u + 1013904223u;
        const int b = lo + static_cast<int>(seed % static_cast<std::uint32_t>(span));
        if (std::abs(a - b) > window) {
            sample.push_back(features.distance(static_cast<std::size_t>(a), static_cast<std::size_t>(b)));
        }
    }
    double typical = 1.0;
    if (!sample.empty()) {
        std::nth_element(sample.begin(), sample.begin() + static_cast<std::ptrdiff_t>(sample.size() / 2), sample.end());
        typical = std::max(sample[sample.size() / 2], 1e-6);
    }
    // The beats' distances inside the range, summed along each diagonal: a hop's cost is a window of one diagonal.
    const int cells = hi - lo; // beat intervals in the range
    if (cells > 4000) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "the clip is too long to remix (more than 4000 beats)"));
    }
    std::vector<double> diagonal(static_cast<std::size_t>(cells) * static_cast<std::size_t>(cells), 0.0);
    for (int x = 0; x < cells; ++x) {
        for (int y = 0; y < cells; ++y) {
            const double d = features.distance(static_cast<std::size_t>(lo + x), static_cast<std::size_t>(lo + y));
            const double before = x > 0 && y > 0 ? diagonal[static_cast<std::size_t>(x - 1) * static_cast<std::size_t>(cells) + static_cast<std::size_t>(y - 1)] : 0.0;
            diagonal[static_cast<std::size_t>(x) * static_cast<std::size_t>(cells) + static_cast<std::size_t>(y)] = before + d;
        }
    }
    const auto prefix = [&](int x, int y) {
        return x < 0 || y < 0 ? 0.0 : diagonal[static_cast<std::size_t>(x) * static_cast<std::size_t>(cells) + static_cast<std::size_t>(y)];
    };
    /// The mean distance of the bar before and the bar after beats a and j (normalised by the typical distance).
    const auto hopCost = [&](int a, int j) {
        const int x0 = a - window - lo;
        const int y0 = j - window - lo;
        const int x1 = a + window - 1 - lo;
        const int y1 = j + window - 1 - lo;
        if (x0 < 0 || y0 < 0 || x1 >= cells || y1 >= cells) {
            return std::numeric_limits<double>::infinity();
        }
        return (prefix(x1, y1) - prefix(x0 - 1, y0 - 1)) / (2.0 * window) / typical;
    };

    // Sections: where the music's average sound changes. At each bar line, the contrast of the mean timbre (MFCCs) and
    // level of the four bars before it and the four after it: inside a section the two are alike, at a section
    // boundary (instruments coming in or out) they are not. Harmony is left out here: a short intro's chords are a
    // part of a progression, and its average would move the boundary by a bar (the joins use chroma).
    std::vector<int> bars; // beat index of each downbeat in [lo, hi]
    for (int i = lo; i <= hi; ++i) {
        if (barPosition[static_cast<std::size_t>(i)] == 0) {
            bars.push_back(i);
        }
    }
    const auto barCount = static_cast<int>(bars.size());
    struct Mean {
        std::array<double, 13> mfcc{};
        double level = 0.0;
        int count = 0;
    };
    const auto meanOf = [&](int fromBar, int toBar) {
        Mean mean;
        for (int x = std::max(fromBar, 0); x < std::min(toBar, barCount); ++x) {
            for (int k = 0; k < bpb; ++k) {
                const int b = bars[static_cast<std::size_t>(x)] + k;
                if (b >= n - 1) {
                    break;
                }
                const auto bi = static_cast<std::size_t>(b);
                for (std::size_t c = 0; c < 13; ++c) {
                    mean.mfcc[c] += features.mfcc[bi][c];
                }
                mean.level += features.levelDb[bi];
                ++mean.count;
            }
        }
        if (mean.count > 0) {
            for (double& v : mean.mfcc) {
                v /= mean.count;
            }
            mean.level /= mean.count;
        }
        return mean;
    };
    const auto contrast = [bpb](const Mean& a, const Mean& b) {
        if (a.count < 2 * bpb || b.count < 2 * bpb) { // at least two bars on each side (the ends of the beats)
            return 0.0;
        }
        double timbre = 0.0;
        for (std::size_t c = 0; c < 13; ++c) {
            timbre += (a.mfcc[c] - b.mfcc[c]) * (a.mfcc[c] - b.mfcc[c]);
        }
        return std::sqrt(timbre / 13.0) + 0.5 * std::min(std::fabs(a.level - b.level) / 20.0, 2.0);
    };
    std::vector<double> novelty(static_cast<std::size_t>(std::max(barCount, 1)), 0.0);
    constexpr int kKernel = 4;
    for (int m = 1; m < barCount; ++m) {
        novelty[static_cast<std::size_t>(m)] = contrast(meanOf(m - kKernel, m), meanOf(m, m + kKernel));
    }
    double meanNovelty = 0.0;
    double sdNovelty = 0.0;
    for (double v : novelty) {
        meanNovelty += v;
    }
    meanNovelty /= static_cast<double>(novelty.size());
    for (double v : novelty) {
        sdNovelty += (v - meanNovelty) * (v - meanNovelty);
    }
    sdNovelty = std::sqrt(sdNovelty / static_cast<double>(novelty.size()));
    std::vector<int> boundaries; // bar indices
    for (int m = 1; m < barCount; ++m) {
        const double v = novelty[static_cast<std::size_t>(m)];
        const double after = m + 1 < barCount ? novelty[static_cast<std::size_t>(m) + 1] : -std::numeric_limits<double>::infinity();
        if (v > meanNovelty + 0.5 * sdNovelty && v >= novelty[static_cast<std::size_t>(m) - 1] && v >= after) {
            boundaries.push_back(m);
        }
    }
    int introBar = barCount >= 8 ? 2 : 1;
    for (const int m : boundaries) {
        if (m >= 2 && m <= barCount * 35 / 100) {
            introBar = m;
            break;
        }
    }
    int outroBar = barCount - (barCount >= 8 ? 2 : 1);
    // (A boundary may be in the last bar the beats reach: the music after the last beat, often a quiet ending the beat
    // tracker leaves out, belongs to the outro anyway.)
    for (auto it = boundaries.rbegin(); it != boundaries.rend(); ++it) {
        if (*it <= barCount - 1 && *it >= barCount * 65 / 100) {
            outroBar = *it;
            break;
        }
    }
    int first = lo + window; // hops need a bar of context on each side
    int last = hi - window;
    if (options.keepIntro && introBar < barCount) {
        first = std::max(first, bars[static_cast<std::size_t>(introBar)]);
    }
    if (options.keepOutro && outroBar > 0 && outroBar < barCount) {
        last = std::min(last, bars[static_cast<std::size_t>(outroBar)]);
    }
    plan.introEnd = options.keepIntro ? beats[static_cast<std::size_t>(std::clamp(first, lo, hi))] : in;
    plan.outroStart = options.keepOutro ? beats[static_cast<std::size_t>(std::clamp(last, lo, hi))] : out;
    if (last <= first) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "the clip is too short between its intro and its outro to remix"));
    }

    const std::int64_t original = out.value - in.value;
    const std::int64_t wanted = options.target.value - original;
    const int minPiece = options.minPieceBeats > 0 ? options.minPieceBeats : bpb;
    if (wanted < 0) {
        // The shortest remix keeps the intro and the outro and jumps once from one to the other.
        const std::int64_t shortest = (beats[static_cast<std::size_t>(first)].value - in.value) + (out.value - beats[static_cast<std::size_t>(last)].value);
        if (options.target.value < shortest - beatTicks / 2) {
            return std::unexpected(Error(ErrorCode::InvalidArgument,
                                         std::format("the target is shorter than the intro and the outro together ({:.1f} s): "
                                                     "choose a longer one, or let the remix cut into the intro or outro",
                                                     Ticks{shortest}.toSeconds())));
        }
    }

    plan.segments.push_back(RemixSegment{in, out});
    plan.duration = Ticks{original};
    if (std::llabs(wanted) * 2 < beatTicks) {
        return plan; // already within half a beat
    }

    // Candidate hops: for every distance in beats, the most seamless jumps of that length, a bar or more apart (so two
    // jumps of the same length can be taken in different places of the song).
    std::map<int, std::vector<Hop>> byDelta;
    constexpr std::size_t kCollect = 32;
    constexpr std::size_t kPerDelta = 8;
    for (int a = first; a <= last; ++a) {
        for (int j = first; j <= last; ++j) {
            if (j == a) {
                continue;
            }
            const double raw = hopCost(a, j);
            if (!std::isfinite(raw)) {
                continue;
            }
            Hop h;
            h.from = a;
            h.to = j;
            h.raw = raw;
            h.onBar = barPosition[static_cast<std::size_t>(a)] == barPosition[static_cast<std::size_t>(j)];
            h.cost = raw + (h.onBar ? 0.0 : 0.35);
            h.delta = beats[static_cast<std::size_t>(a)].value - beats[static_cast<std::size_t>(j)].value;
            auto& list = byDelta[a - j];
            if (list.size() < kCollect) {
                list.push_back(h);
                std::sort(list.begin(), list.end(), [](const Hop& x, const Hop& y) { return x.cost < y.cost; });
            } else if (h.cost < list.back().cost) {
                list.back() = h;
                std::sort(list.begin(), list.end(), [](const Hop& x, const Hop& y) { return x.cost < y.cost; });
            }
        }
    }
    std::vector<Hop> hops;
    for (auto& [delta, list] : byDelta) {
        (void)delta;
        std::vector<Hop> spread;
        for (const Hop& h : list) {
            if (std::ranges::none_of(spread, [&](const Hop& o) { return std::abs(o.from - h.from) < bpb; })) {
                spread.push_back(h);
            }
            if (spread.size() == kPerDelta) {
                break;
            }
        }
        list = std::move(spread);
        hops.insert(hops.end(), list.begin(), list.end());
    }
    if (hops.empty()) {
        return plan;
    }
    constexpr double kJoinCost = 0.05;
    const auto finalScore = [&](double cost, std::size_t joins, std::int64_t added) {
        return cost + kJoinCost * static_cast<double>(joins) + errorPenalty(static_cast<double>(wanted - added) / static_cast<double>(beatTicks));
    };
    const auto playable = [&](const std::vector<const Hop*>& sequence) {
        int position = -1;
        for (const Hop* h : sequence) {
            const int earliest = position < 0 ? first : std::max(first, position + minPiece);
            if (h->from < earliest) {
                return false;
            }
            position = h->to;
        }
        return true;
    };

    // 1. The shapes remixes take: one jump, a loop taken several times, and either with one more jump for the rest
    //    (before or after it). Exhaustive over the most seamless jumps.
    std::vector<const Hop*> bestSequence;
    double bestSequenceScore = finalScore(0.0, 0, 0);
    const auto consider = [&](const std::vector<const Hop*>& sequence) {
        double cost = 0.0;
        std::int64_t added = 0;
        for (const Hop* h : sequence) {
            cost += h->cost;
            added += h->delta;
        }
        const double s = finalScore(cost, sequence.size(), added);
        if (s < bestSequenceScore - 1e-12 && playable(sequence)) {
            bestSequenceScore = s;
            bestSequence = sequence;
        }
    };
    {
        std::vector<const Hop*> byCost;
        for (const Hop& h : hops) {
            byCost.push_back(&h);
        }
        std::sort(byCost.begin(), byCost.end(), [](const Hop* x, const Hop* y) { return x->cost < y->cost; });
        byCost.resize(std::min<std::size_t>(byCost.size(), 512));
        const int maxJoins = std::max(options.maxJoins, 1);
        for (const Hop* main : byCost) {
            const int repeats = main->delta > 0 ? maxJoins : 1; // a loop may be taken again; a skip only once
            for (int k = 1; k <= repeats; ++k) {
                std::vector<const Hop*> loop(static_cast<std::size_t>(k), main);
                consider(loop);
                const std::int64_t rest = wanted - static_cast<std::int64_t>(k) * main->delta;
                if (std::llabs(rest) * 2 >= beatTicks && k < maxJoins) {
                    const auto key = static_cast<int>(std::llround(static_cast<double>(rest) / static_cast<double>(beatTicks)));
                    for (const int d : {key - 1, key, key + 1}) {
                        const auto it = byDelta.find(d);
                        if (it == byDelta.end()) {
                            continue;
                        }
                        for (const Hop& other : it->second) {
                            std::vector<const Hop*> before = loop;
                            before.insert(before.begin(), &other);
                            consider(before);
                            std::vector<const Hop*> after = loop;
                            after.push_back(&other);
                            consider(after);
                        }
                    }
                }
                if (main->delta > 0 && static_cast<std::int64_t>(k) * main->delta > wanted + beatTicks) {
                    break; // more loops only overshoot
                }
            }
        }
    }

    // 2. A beam search in play order for anything else (several different jumps): states ranked by their cost with a
    //    mild pull towards the target (an intermediate state may be far from it: later jumps close the gap), the best
    //    finished one by the full score.
    constexpr std::size_t kBeam = 48;
    const auto score = [&](const State& s) { return finalScore(s.cost, static_cast<std::size_t>(s.joins), s.added); };
    const auto rank = [&](const State& s) {
        return s.cost + kJoinCost * s.joins + 0.05 * std::fabs(static_cast<double>(wanted - s.added)) / static_cast<double>(beatTicks);
    };
    std::vector<State> history;
    std::vector<State> beam{State{}};
    State best = beam.front();
    double bestScore = score(best);
    int stale = 0;
    for (int depth = 0; depth < std::max(options.maxJoins, 1) && !beam.empty() && stale < 4; ++depth) {
        std::vector<State> next;
        for (const State& s : beam) {
            history.push_back(s);
            const int parent = static_cast<int>(history.size()) - 1;
            const int earliest = s.position < 0 ? first : std::max(first, s.position + minPiece);
            for (std::size_t h = 0; h < hops.size(); ++h) {
                const Hop& hop = hops[h];
                if (hop.from < earliest) {
                    continue;
                }
                // Moving away from the target by more than a beat is never useful at the last step, but an
                // intermediate hop may overshoot to be corrected later: allow it within the original length.
                const std::int64_t added = s.added + hop.delta;
                if (std::llabs(wanted - added) > std::llabs(wanted) + original) {
                    continue;
                }
                State t;
                t.position = hop.to;
                t.added = added;
                t.cost = s.cost + hop.cost;
                t.joins = s.joins + 1;
                t.parent = parent;
                t.hop = static_cast<int>(h);
                next.push_back(t);
            }
        }
        // Keep the best state per (landing beat, added beats): the rest are the same remix at a higher cost.
        std::sort(next.begin(), next.end(), [&](const State& x, const State& y) { return rank(x) < rank(y); });
        std::vector<State> kept;
        std::map<std::pair<int, std::int64_t>, bool> seen;
        for (const State& t : next) {
            const auto key = std::make_pair(t.position, (t.added + beatTicks / 2) / beatTicks);
            if (seen.contains(key)) {
                continue;
            }
            seen[key] = true;
            kept.push_back(t);
            if (kept.size() == kBeam) {
                break;
            }
        }
        beam = std::move(kept);
        bool improved = false;
        for (const State& t : next) {
            if (const double v = score(t); v < bestScore - 1e-12) {
                bestScore = v;
                best = t;
                improved = true;
            }
        }
        if (improved && best.parent >= 0 && std::ranges::find_if(beam, [&](const State& t) { return t.parent == best.parent && t.hop == best.hop; }) == beam.end()) {
            beam.push_back(best); // a finished remix the ranking dropped is still a candidate to extend
        }
        stale = improved ? 0 : stale + 1;
    }
    // Rebuild the hops of the best state.
    std::vector<const Hop*> chosen;
    // Every state with a hop has a parent in `history` (it was pushed when it was expanded); the start has none.
    for (State s = best; s.hop >= 0; s = history[static_cast<std::size_t>(s.parent)]) {
        chosen.push_back(&hops[static_cast<std::size_t>(s.hop)]);
    }
    std::reverse(chosen.begin(), chosen.end());
    if (bestSequenceScore < bestScore) {
        chosen = bestSequence;
    }
    if (chosen.empty()) {
        return plan;
    }

    plan.segments.clear();
    plan.cost = 0.0;
    Ticks start = in;
    Ticks at{0};
    const auto onGrid = [](std::int64_t t) { return Ticks{(t + kRemixCutGrid / 2) / kRemixCutGrid * kRemixCutGrid}; };
    for (std::size_t k = 0; k < chosen.size(); ++k) {
        const Hop& h = *chosen[k];
        const Ticks cutFrom = onGrid(beats[static_cast<std::size_t>(h.from)].value - pre);
        const Ticks cutTo = onGrid(beats[static_cast<std::size_t>(h.to)].value - pre);
        plan.segments.push_back(RemixSegment{start, cutFrom});
        at = at + (cutFrom - start);
        RemixJoin join;
        join.index = k;
        join.at = at;
        join.fromBeat = beats[static_cast<std::size_t>(h.from)];
        join.toBeat = beats[static_cast<std::size_t>(h.to)];
        join.confidence = std::clamp(1.0 - h.raw, 0.0, 1.0);
        join.onBar = h.onBar;
        plan.joins.push_back(join);
        plan.cost += h.cost;
        start = cutTo;
    }
    plan.segments.push_back(RemixSegment{start, out});
    std::int64_t total = 0;
    for (const RemixSegment& s : plan.segments) {
        total += s.duration().value;
    }
    plan.duration = Ticks{total};
    return plan;
}

std::vector<float> renderRemix(const RemixPlan& plan, std::span<const float> source, int channels, int sampleRate) {
    const auto ch = static_cast<std::size_t>(std::max(channels, 1));
    const Rate rate{sampleRate, 1};
    const std::int64_t total = ticksToSamples(plan.duration, rate);
    std::vector<float> out(static_cast<std::size_t>(std::max<std::int64_t>(total, 0)) * ch, 0.0f);
    const auto sourceFrames = static_cast<std::int64_t>(source.size() / ch);
    const std::int64_t half = ticksToSamples(Ticks{plan.crossfade.value / 2}, rate);
    std::int64_t position = 0; // output frame where the piece starts
    for (std::size_t k = 0; k < plan.segments.size(); ++k) {
        const RemixSegment& s = plan.segments[k];
        const std::int64_t first = ticksToSamples(s.sourceStart, rate);
        const std::int64_t length = ticksToSamples(s.sourceEnd, rate) - first;
        const bool joinBefore = k > 0;
        const bool joinAfter = k + 1 < plan.segments.size();
        const std::int64_t from = joinBefore ? -half : 0;
        const std::int64_t to = joinAfter ? length + half : length;
        for (std::int64_t i = from; i < to; ++i) {
            const std::int64_t o = position + i;
            const std::int64_t src = first + i;
            if (o < 0 || o >= total || src < 0 || src >= sourceFrames) {
                continue;
            }
            double g = 1.0;
            if (joinBefore && i < half) {
                g = (static_cast<double>(i + half) + 0.5) / static_cast<double>(2 * half); // fading in over the cut
            }
            if (joinAfter && i >= length - half) {
                g *= 1.0 - (static_cast<double>(i - (length - half)) + 0.5) / static_cast<double>(2 * half); // fading out
            }
            for (std::size_t c = 0; c < ch; ++c) {
                out[static_cast<std::size_t>(o) * ch + c] += static_cast<float>(g * source[static_cast<std::size_t>(src) * ch + c]);
            }
        }
        position += length;
    }
    return out;
}

} // namespace wabcut::audio::edit
