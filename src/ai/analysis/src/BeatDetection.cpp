#include <wabcut/ai/analysis/BeatDetection.h>

#include "Fft.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>

namespace wabcut::ai::analysis {

namespace {

constexpr int kMelBands = 40;
constexpr double kMelMinHz = 30.0;
constexpr double kMelMaxHz = 8000.0;
constexpr double kDbRange = 80.0;

double hzToMel(double hz) { return 2595.0 * std::log10(1.0 + hz / 700.0); }
double melToHz(double mel) { return 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0); }

struct Onsets {
    double frameRate = 100.0;  ///< frames per second
    int hop = 220;             ///< samples per frame
    std::vector<double> flux;  ///< raw log-mel spectral flux (>= 0)
    std::vector<double> strength; ///< flux high-passed and normalised (what the tracker uses)
};

/// Log-mel spectral flux, one value per hop (frame k is centred on sample k x hop).
Onsets onsetStrength(std::span<const float> x, int sampleRate) {
    Onsets o;
    o.hop = std::max(1, static_cast<int>(std::lround(sampleRate / 100.0)));
    o.frameRate = static_cast<double>(sampleRate) / o.hop;
    std::size_t n = 256;
    while (static_cast<double>(n) < 0.046 * sampleRate) {
        n <<= 1;
    }
    const detail::Fft fft(n);
    std::vector<double> window(n);
    for (std::size_t i = 0; i < n; ++i) {
        window[i] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n));
    }
    // Triangular mel filters over the FFT bins.
    const std::size_t bins = n / 2 + 1;
    const double maxHz = std::min(kMelMaxHz, sampleRate / 2.0);
    std::vector<double> edges(kMelBands + 2);
    for (int b = 0; b < kMelBands + 2; ++b) {
        edges[static_cast<std::size_t>(b)] =
            melToHz(hzToMel(kMelMinHz) + (hzToMel(maxHz) - hzToMel(kMelMinHz)) * b / (kMelBands + 1));
    }
    std::vector<std::vector<std::pair<std::size_t, double>>> filters(kMelBands);
    for (int b = 0; b < kMelBands; ++b) {
        const double lo = edges[static_cast<std::size_t>(b)];
        const double mid = edges[static_cast<std::size_t>(b) + 1];
        const double hi = edges[static_cast<std::size_t>(b) + 2];
        for (std::size_t k = 0; k < bins; ++k) {
            const double hz = static_cast<double>(k) * sampleRate / static_cast<double>(n);
            double w = 0.0;
            if (hz > lo && hz < mid) {
                w = (hz - lo) / (mid - lo);
            } else if (hz >= mid && hz < hi) {
                w = (hi - hz) / (hi - mid);
            }
            if (w > 0.0) {
                filters[static_cast<std::size_t>(b)].emplace_back(k, w);
            }
        }
    }
    const auto frames = static_cast<std::size_t>(x.size() / static_cast<std::size_t>(o.hop) + 1);
    std::vector<std::vector<double>> mel(frames, std::vector<double>(kMelBands));
    std::vector<std::complex<double>> buffer(n);
    double maxDb = -1e9;
    for (std::size_t f = 0; f < frames; ++f) {
        const auto centre = static_cast<std::int64_t>(f) * o.hop;
        for (std::size_t i = 0; i < n; ++i) {
            const std::int64_t s = centre - static_cast<std::int64_t>(n / 2) + static_cast<std::int64_t>(i);
            const double v = s >= 0 && s < static_cast<std::int64_t>(x.size()) ? x[static_cast<std::size_t>(s)] : 0.0;
            buffer[i] = {v * window[i], 0.0};
        }
        fft.transform(buffer);
        for (int b = 0; b < kMelBands; ++b) {
            double power = 0.0;
            for (const auto& [k, w] : filters[static_cast<std::size_t>(b)]) {
                power += w * std::norm(buffer[k]);
            }
            const double db = 10.0 * std::log10(std::max(power, 1e-10));
            mel[f][static_cast<std::size_t>(b)] = db;
            maxDb = std::max(maxDb, db);
        }
    }
    o.flux.assign(frames, 0.0);
    for (std::size_t f = 1; f < frames; ++f) {
        double total = 0.0;
        for (int b = 0; b < kMelBands; ++b) {
            const double now = std::max(mel[f][static_cast<std::size_t>(b)], maxDb - kDbRange);
            const double before = std::max(mel[f - 1][static_cast<std::size_t>(b)], maxDb - kDbRange);
            total += std::max(0.0, now - before);
        }
        o.flux[f] = total / kMelBands;
    }
    // High-pass (remove the local mean over half a second) and normalise to unit standard deviation.
    const auto half = static_cast<std::size_t>(std::lround(o.frameRate / 4.0));
    std::vector<double> prefix(frames + 1, 0.0);
    for (std::size_t f = 0; f < frames; ++f) {
        prefix[f + 1] = prefix[f] + o.flux[f];
    }
    o.strength.assign(frames, 0.0);
    for (std::size_t f = 0; f < frames; ++f) {
        const std::size_t a = f > half ? f - half : 0;
        const std::size_t b = std::min(frames, f + half + 1);
        o.strength[f] = o.flux[f] - (prefix[b] - prefix[a]) / static_cast<double>(b - a);
    }
    double var = 0.0;
    for (const double v : o.strength) {
        var += v * v;
    }
    const double sd = std::sqrt(var / std::max<std::size_t>(frames, 1));
    if (sd > 1e-12) {
        for (double& v : o.strength) {
            v /= sd;
        }
    }
    return o;
}

/// Autocorrelation of the positive onset strength, normalised per lag, for lags 0..maxLag.
std::vector<double> autocorrelation(const std::vector<double>& s, std::size_t maxLag) {
    std::vector<double> pos(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        pos[i] = std::max(0.0, s[i]);
    }
    std::vector<double> r(maxLag + 1, 0.0);
    for (std::size_t lag = 0; lag <= maxLag && lag < pos.size(); ++lag) {
        double sum = 0.0;
        for (std::size_t i = 0; i + lag < pos.size(); ++i) {
            sum += pos[i] * pos[i + lag];
        }
        r[lag] = sum / static_cast<double>(pos.size() - lag);
    }
    return r;
}

double lagAt(const std::vector<double>& r, double lag) {
    const auto i = static_cast<std::size_t>(std::floor(lag));
    if (i + 1 >= r.size()) {
        return i < r.size() ? r[i] : 0.0;
    }
    const double f = lag - static_cast<double>(i);
    return r[i] * (1.0 - f) + r[i + 1] * f;
}

struct Tempo {
    double period = 0.0;     ///< frames per beat
    double periodicity = 0.0; ///< 0..1
};

Tempo estimateTempo(const Onsets& o, const BeatOptions& opt) {
    const double fr = o.frameRate;
    const auto minLag = static_cast<std::size_t>(std::max(1.0, std::floor(60.0 * fr / opt.maxBpm)));
    const auto maxLag = static_cast<std::size_t>(std::ceil(60.0 * fr / opt.minBpm));
    const std::vector<double> r = autocorrelation(o.strength, maxLag + 2);
    std::size_t best = 0;
    double bestScore = -1.0;
    for (std::size_t lag = minLag; lag <= maxLag && lag + 1 < r.size(); ++lag) {
        const bool peak = r[lag] >= r[lag - 1] && r[lag] >= r[lag + 1];
        if (!peak) {
            continue;
        }
        const double bpm = 60.0 * fr / static_cast<double>(lag);
        const double octaves = std::log2(bpm / opt.priorBpm) / std::max(opt.priorOctaves, 0.1);
        const double score = r[lag] * std::exp(-0.5 * octaves * octaves);
        if (score > bestScore) {
            bestScore = score;
            best = lag;
        }
    }
    if (best == 0) {
        return {60.0 * fr / opt.priorBpm, 0.0};
    }
    // Sub-frame lag (parabola through the peak).
    const auto refine = [&](std::size_t lag) {
        const double a = r[lag - 1];
        const double b = r[lag];
        const double c = r[lag + 1];
        const double denom = a - 2.0 * b + c;
        return static_cast<double>(lag) + (std::abs(denom) > 1e-12 ? 0.5 * (a - c) / denom : 0.0);
    };
    double period = refine(best);
    // Twice as fast, when the onsets half way between beats are about as strong (174 BPM, not 87).
    const double faster = period / 2.0;
    if (60.0 * fr / faster <= opt.maxBpm && faster >= 1.0 && lagAt(r, faster) >= 0.6 * lagAt(r, period)) {
        const auto lag = static_cast<std::size_t>(std::lround(faster));
        std::size_t peak = lag;
        for (std::size_t k = lag > 1 ? lag - 1 : 1; k <= lag + 1 && k + 1 < r.size(); ++k) {
            if (r[k] > r[peak]) {
                peak = k;
            }
        }
        period = peak > 0 && peak + 1 < r.size() ? refine(peak) : faster;
    }
    return {period, r[0] > 0.0 ? std::clamp(lagAt(r, period) / r[0], 0.0, 1.0) : 0.0};
}

/// Ellis' dynamic programming: the beat sequence with the best onset strength for the period.
std::vector<std::size_t> trackBeats(const std::vector<double>& strength, double period, double tightness) {
    const std::size_t n = strength.size();
    std::vector<double> cumulative(n, 0.0);
    std::vector<std::int64_t> back(n, -1);
    const auto lo = static_cast<std::int64_t>(std::lround(period / 2.0));
    const auto hi = static_cast<std::int64_t>(std::lround(2.0 * period));
    for (std::size_t t = 0; t < n; ++t) {
        double best = -1e300;
        std::int64_t from = -1;
        for (std::int64_t d = std::max<std::int64_t>(lo, 1); d <= hi; ++d) {
            const std::int64_t tau = static_cast<std::int64_t>(t) - d;
            if (tau < 0) {
                break;
            }
            const double penalty = std::log(static_cast<double>(d) / period);
            const double score = cumulative[static_cast<std::size_t>(tau)] - tightness * penalty * penalty;
            if (score > best) {
                best = score;
                from = tau;
            }
        }
        cumulative[t] = strength[t] + (from >= 0 ? std::max(best, 0.0) : 0.0);
        back[t] = from >= 0 && best > 0.0 ? from : -1;
    }
    // The last beat: the last local maximum of the cumulative score above half the median of the maxima.
    std::vector<double> maxima;
    for (std::size_t t = 1; t + 1 < n; ++t) {
        if (cumulative[t] > cumulative[t - 1] && cumulative[t] >= cumulative[t + 1]) {
            maxima.push_back(cumulative[t]);
        }
    }
    if (maxima.empty()) {
        return {};
    }
    std::vector<double> sorted = maxima;
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
    const double threshold = 0.5 * sorted[sorted.size() / 2];
    std::int64_t last = -1;
    for (std::size_t t = n - 2; t >= 1; --t) {
        if (cumulative[t] > cumulative[t - 1] && cumulative[t] >= cumulative[t + 1] && cumulative[t] >= threshold) {
            last = static_cast<std::int64_t>(t);
            break;
        }
    }
    std::vector<std::size_t> beats;
    for (std::int64_t t = last; t >= 0; t = back[static_cast<std::size_t>(t)]) {
        beats.push_back(static_cast<std::size_t>(t));
    }
    std::reverse(beats.begin(), beats.end());
    // The ends of the path can bend to reach a stray onset (a hi-hat after the last beat): drop end beats whose
    // interval to their neighbour is off the period by more than a fifth.
    const auto offTempo = [&](std::size_t a, std::size_t b) {
        return std::abs(static_cast<double>(b - a) - period) > 0.2 * period;
    };
    while (beats.size() >= 3 && offTempo(beats[beats.size() - 2], beats.back())) {
        beats.pop_back();
    }
    while (beats.size() >= 3 && offTempo(beats[0], beats[1])) {
        beats.erase(beats.begin());
    }
    return beats;
}

/// The attack nearest to `centre` (a sample index): the 1 ms block with the biggest rise in energy within
/// [centre - before, centre + after], then the first sample there reaching half of the local peak.
std::int64_t attackNear(std::span<const float> x, int sampleRate, std::int64_t centre, std::int64_t before,
                        std::int64_t after) {
    const std::int64_t block = std::max(1, sampleRate / 1000);
    const std::int64_t size = static_cast<std::int64_t>(x.size());
    const std::int64_t from = std::max<std::int64_t>(0, centre - before);
    const std::int64_t to = std::min(size, centre + after);
    if (to - from < 4 * block) {
        return centre;
    }
    const auto energy = [&](std::int64_t start) {
        double e = 0.0;
        for (std::int64_t s = std::max<std::int64_t>(start, 0); s < std::min(start + block, size); ++s) {
            e += static_cast<double>(x[static_cast<std::size_t>(s)]) * x[static_cast<std::size_t>(s)];
        }
        return e;
    };
    double bestRise = 0.0;
    std::int64_t bestBlock = -1;
    for (std::int64_t start = from; start + block <= to; start += block / 2 > 0 ? block / 2 : 1) {
        double previous = 0.0;
        for (int k = 1; k <= 3; ++k) {
            previous = std::max(previous, energy(start - k * block));
        }
        const double rise = energy(start) - previous;
        if (rise > bestRise) {
            bestRise = rise;
            bestBlock = start;
        }
    }
    if (bestBlock < 0) {
        return centre;
    }
    double peak = 0.0;
    for (std::int64_t s = std::max<std::int64_t>(0, bestBlock - block); s < std::min(size, bestBlock + 3 * block); ++s) {
        peak = std::max(peak, static_cast<double>(std::abs(x[static_cast<std::size_t>(s)])));
    }
    for (std::int64_t s = std::max<std::int64_t>(0, bestBlock - block); s < std::min(size, bestBlock + 3 * block); ++s) {
        if (std::abs(x[static_cast<std::size_t>(s)]) >= 0.5 * peak) {
            return s;
        }
    }
    return bestBlock;
}

} // namespace

std::string BeatOptions::fingerprint() const {
    char buffer[160];
    std::snprintf(buffer, sizeof buffer, "%s %.3f-%.3f p%.3f/%.3f t%.3f b%d", tracker.c_str(), minBpm, maxBpm, priorBpm,
                  priorOctaves, tightness, beatsPerBar);
    return buffer;
}

std::expected<BeatAnalysis, Error> DspBeatTracker::track(const MonoAudio& audio, const BeatOptions& options,
                                                         const AnalysisProgress& progress) {
    if (audio.sampleRate < 8000 || audio.sampleRate > 384000) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "beat tracking needs 8..384 kHz audio"});
    }
    if (options.minBpm <= 0.0 || options.maxBpm <= options.minBpm) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "the tempo range is empty"});
    }
    const Rate rate{audio.sampleRate, 1};
    BeatAnalysis out;
    out.tracker = id();
    out.beatsPerBar = std::max(options.beatsPerBar, 1);
    out.duration = samplesToTicks(static_cast<std::int64_t>(audio.samples.size()), rate);
    const auto stopped = [&](double fraction) { return progress && !progress(fraction); };
    if (audio.samples.size() < static_cast<std::size_t>(audio.sampleRate)) {
        return out; // under a second: no tempo to speak of
    }
    const Onsets o = onsetStrength(audio.samples, audio.sampleRate);
    if (stopped(0.6)) {
        return std::unexpected(Error{ErrorCode::Cancelled, "beat tracking was stopped"});
    }
    const Tempo tempo = estimateTempo(o, options);
    std::vector<std::size_t> frames = trackBeats(o.strength, tempo.period, options.tightness);
    if (stopped(0.9)) {
        return std::unexpected(Error{ErrorCode::Cancelled, "beat tracking was stopped"});
    }
    // Drop weak beats at the ends (silence or intros without a pulse).
    const auto onsetAt = [&](std::size_t f) {
        double v = 0.0;
        for (std::size_t k = f > 2 ? f - 2 : 0; k <= std::min(f + 2, o.flux.size() - 1); ++k) {
            v = std::max(v, o.flux[k]);
        }
        return v;
    };
    if (!frames.empty()) {
        std::vector<double> strengths;
        for (const std::size_t f : frames) {
            strengths.push_back(onsetAt(f));
        }
        // librosa's rule: half the RMS of the beats' onset strengths.
        double squares = 0.0;
        for (const double v : strengths) {
            squares += v * v;
        }
        const double floor = 0.5 * std::sqrt(squares / static_cast<double>(strengths.size()));
        std::size_t first = 0;
        std::size_t last = frames.size();
        while (first < last && strengths[first] < floor) {
            ++first;
        }
        while (last > first && strengths[last - 1] < floor) {
            --last;
        }
        frames = std::vector<std::size_t>(frames.begin() + static_cast<std::ptrdiff_t>(first),
                                          frames.begin() + static_cast<std::ptrdiff_t>(last));
    }
    // Sample-exact attacks.
    const double periodSamples = tempo.period * o.hop;
    const auto window = static_cast<std::int64_t>(std::min(0.06 * audio.sampleRate, 0.4 * periodSamples));
    std::vector<std::int64_t> samples;
    for (const std::size_t f : frames) {
        const std::int64_t centre = static_cast<std::int64_t>(f) * o.hop;
        const std::int64_t s = attackNear(audio.samples, audio.sampleRate, centre, window, window);
        if (samples.empty() || s > samples.back()) {
            samples.push_back(s);
        }
    }
    // Tempo from the beats themselves (least squares), else from the autocorrelation.
    double period = periodSamples;
    double regularity = 0.0;
    if (samples.size() >= 4) {
        const auto n = static_cast<double>(samples.size());
        double meanI = (n - 1.0) / 2.0;
        double meanS = 0.0;
        for (const std::int64_t s : samples) {
            meanS += static_cast<double>(s);
        }
        meanS /= n;
        double sxy = 0.0;
        double sxx = 0.0;
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const double di = static_cast<double>(i) - meanI;
            sxy += di * (static_cast<double>(samples[i]) - meanS);
            sxx += di * di;
        }
        if (sxx > 0.0 && sxy > 0.0) {
            period = sxy / sxx;
            double residual = 0.0;
            for (std::size_t i = 0; i < samples.size(); ++i) {
                const double fit = meanS + (static_cast<double>(i) - meanI) * period;
                residual += (static_cast<double>(samples[i]) - fit) * (static_cast<double>(samples[i]) - fit);
            }
            const double sd = std::sqrt(residual / n);
            regularity = std::clamp(1.0 - sd / (0.1 * period), 0.0, 1.0);
        }
    }
    out.bpm = period > 0.0 && samples.size() >= 2 ? 60.0 * audio.sampleRate / period : 0.0;
    out.confidence = static_cast<float>(std::clamp(0.5 * tempo.periodicity + 0.5 * regularity, 0.0, 1.0));
    for (const std::int64_t s : samples) {
        out.beats.push_back(audio.origin + samplesToTicks(s, rate));
    }
    // Downbeats: the place in the bar whose beats are loudest just after their attack, low frequencies (kick drums)
    // counting double (a one-pole low-pass at about 150 Hz).
    const auto bpb = static_cast<std::size_t>(out.beatsPerBar);
    if (bpb > 1 && samples.size() >= 2 * bpb) {
        const double alpha = 1.0 - std::exp(-2.0 * std::numbers::pi * 150.0 / audio.sampleRate);
        const auto span = static_cast<std::int64_t>(0.05 * audio.sampleRate);
        const auto loudness = [&](std::int64_t start) {
            double low = 0.0;
            double lowEnergy = 0.0;
            double fullEnergy = 0.0;
            const std::int64_t end = std::min<std::int64_t>(start + span, static_cast<std::int64_t>(audio.samples.size()));
            for (std::int64_t s = std::max<std::int64_t>(start, 0); s < end; ++s) {
                const double v = audio.samples[static_cast<std::size_t>(s)];
                low += alpha * (v - low);
                lowEnergy += low * low;
                fullEnergy += v * v;
            }
            return 2.0 * std::sqrt(lowEnergy) + std::sqrt(fullEnergy);
        };
        std::vector<double> accent(bpb, 0.0);
        std::vector<int> count(bpb, 0);
        for (std::size_t i = 0; i < samples.size(); ++i) {
            accent[i % bpb] += loudness(samples[i]);
            ++count[i % bpb];
        }
        std::size_t best = 0;
        for (std::size_t k = 0; k < bpb; ++k) {
            accent[k] /= std::max(count[k], 1);
            if (accent[k] > accent[best]) {
                best = k;
            }
        }
        for (std::size_t i = best; i < samples.size(); i += bpb) {
            out.downbeats.push_back(out.beats[i]);
        }
    } else if (bpb == 1) {
        out.downbeats = out.beats;
    }
    if (stopped(1.0)) {
        return std::unexpected(Error{ErrorCode::Cancelled, "beat tracking was stopped"});
    }
    return out;
}

std::unique_ptr<BeatTracker> makeBeatTracker(std::string_view name) {
    if (name == "dsp" || name == "dsp-1" || name.empty()) {
        return std::make_unique<DspBeatTracker>();
    }
    return nullptr;
}

std::expected<BeatAnalysis, Error> detectBeats(const MonoAudio& audio, const BeatOptions& options,
                                               const AnalysisProgress& progress) {
    DspBeatTracker tracker;
    return tracker.track(audio, options, progress);
}

} // namespace wabcut::ai::analysis
