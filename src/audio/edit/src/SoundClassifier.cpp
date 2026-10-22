#include <wabcut/audio/edit/SoundClassifier.h>

#include "SyncFft.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace wabcut::audio::edit {

namespace {

using detail::Cf;

double percentile(std::vector<double> values, double p) {
    if (values.empty()) {
        return -144.0;
    }
    std::sort(values.begin(), values.end());
    const double at = std::clamp(p, 0.0, 1.0) * static_cast<double>(values.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(at));
    const std::size_t hi = std::min(lo + 1, values.size() - 1);
    return values[lo] + (values[hi] - values[lo]) * (at - static_cast<double>(lo));
}

/// A soft threshold: 0 well below `center`, 1 well above, 0.5 at it; `width` is the 27..73 % span.
double above(double x, double center, double width) { return 1.0 / (1.0 + std::exp(-(x - center) / std::max(width, 1e-9))); }
double below(double x, double center, double width) { return 1.0 - above(x, center, width); }
/// High inside [lo, hi], falling off outside.
double within(double x, double lo, double hi, double width) { return above(x, lo, width) * below(x, hi, width); }

} // namespace

SoundFeatures soundFeatures(std::span<const float> mono, int sampleRate) {
    SoundFeatures f;
    if (sampleRate < 8000 || mono.empty()) {
        return f;
    }
    // About 16 kHz: a box filter then every d-th sample (speech and music cues all lie below 6 kHz).
    const int d = std::max(1, static_cast<int>(std::lround(static_cast<double>(sampleRate) / 16000.0)));
    const double rate = static_cast<double>(sampleRate) / d;
    std::vector<double> x;
    x.reserve(mono.size() / static_cast<std::size_t>(d) + 1);
    for (std::size_t i = 0; i + static_cast<std::size_t>(d) <= mono.size(); i += static_cast<std::size_t>(d)) {
        double sum = 0.0;
        for (int k = 0; k < d; ++k) {
            sum += mono[i + static_cast<std::size_t>(k)];
        }
        x.push_back(sum / d);
    }
    f.seconds = static_cast<double>(x.size()) / rate;
    // Frames of ~32 ms every ~16 ms.
    std::size_t n = 64;
    while (static_cast<double>(n) < 0.032 * rate) {
        n <<= 1;
    }
    const std::size_t hop = n / 2;
    if (x.size() < n) {
        return f;
    }
    const std::size_t frames = (x.size() - n) / hop + 1;
    const double frameRate = rate / static_cast<double>(hop);
    detail::SyncFft fft(n);
    std::vector<Cf> spectrum(n);
    std::vector<double> window(n);
    for (std::size_t i = 0; i < n; ++i) {
        window[i] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n));
    }
    const auto minLag = static_cast<std::size_t>(std::floor(rate / 400.0));
    const auto maxLag = std::min(static_cast<std::size_t>(std::ceil(rate / 80.0)), n - 1);
    const auto loBin = static_cast<std::size_t>(std::ceil(100.0 / rate * static_cast<double>(n)));
    const auto hiBin = std::min(static_cast<std::size_t>(6000.0 / rate * static_cast<double>(n)), n / 2);

    std::vector<double> levelDb(frames);
    std::vector<double> rms(frames);
    std::vector<double> flatness(frames, 0.0);
    std::vector<double> pitch(frames, 0.0); // Hz; 0 = unvoiced
    for (std::size_t k = 0; k < frames; ++k) {
        const double* s = x.data() + k * hop;
        double energy = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            energy += s[i] * s[i];
        }
        rms[k] = std::sqrt(energy / static_cast<double>(n));
        levelDb[k] = 10.0 * std::log10(energy / static_cast<double>(n) + 1e-14);
        // Spectral flatness over 100 Hz .. 6 kHz.
        for (std::size_t i = 0; i < n; ++i) {
            spectrum[i] = Cf(static_cast<float>(s[i] * window[i]), 0.0f);
        }
        fft.forward(spectrum.data());
        double logSum = 0.0;
        double sum = 0.0;
        std::size_t bins = 0;
        for (std::size_t b = loBin; b <= hiBin; ++b) {
            const double p = static_cast<double>(std::norm(spectrum[b])) + 1e-18;
            logSum += std::log(p);
            sum += p;
            ++bins;
        }
        if (bins > 0 && sum > 0.0) {
            flatness[k] = std::exp(logSum / static_cast<double>(bins)) / (sum / static_cast<double>(bins));
        }
        // Voicing: the normalised autocorrelation's peak in 80..400 Hz, its lag refined by a parabola.
        if (energy > 1e-10) {
            double best = 0.0;
            std::size_t bestLag = 0;
            std::vector<double> r(maxLag + 2, 0.0);
            for (std::size_t lag = minLag > 0 ? minLag - 1 : 0; lag <= maxLag + 1 && lag < n; ++lag) {
                double num = 0.0;
                double a = 0.0;
                double b = 0.0;
                for (std::size_t i = 0; i + lag < n; ++i) {
                    num += s[i] * s[i + lag];
                    a += s[i] * s[i];
                    b += s[i + lag] * s[i + lag];
                }
                r[lag] = a > 0.0 && b > 0.0 ? num / std::sqrt(a * b) : 0.0;
            }
            for (std::size_t lag = std::max<std::size_t>(minLag, 1); lag <= maxLag; ++lag) {
                if (r[lag] > best && r[lag] >= r[lag - 1] && r[lag] >= r[lag + 1]) {
                    best = r[lag];
                    bestLag = lag;
                }
            }
            if (best > 0.55 && bestLag > 0) {
                const double y0 = r[bestLag - 1];
                const double y1 = r[bestLag];
                const double y2 = r[bestLag + 1];
                const double denom = y0 - 2.0 * y1 + y2;
                const double shift = std::abs(denom) > 1e-12 ? std::clamp(0.5 * (y0 - y2) / denom, -0.5, 0.5) : 0.0;
                pitch[k] = rate / (static_cast<double>(bestLag) + shift);
            }
        }
    }

    const double loud = percentile(levelDb, 0.95);
    if (loud < -70.0) {
        f.levelDb = loud;
        return f; // silent
    }
    std::vector<double> audible;
    std::size_t pauses = 0;
    for (std::size_t k = 0; k < frames; ++k) {
        if (levelDb[k] < loud - 30.0) {
            ++pauses;
            pitch[k] = 0.0;
        } else {
            audible.push_back(levelDb[k]);
        }
    }
    f.pauseShare = static_cast<double>(pauses) / static_cast<double>(frames);
    f.levelDb = percentile(audible, 0.5);
    f.crestDb = percentile(audible, 0.99) - f.levelDb;
    f.levelSpreadDb = percentile(audible, 0.75) - percentile(audible, 0.25);
    double flatSum = 0.0;
    std::size_t voiced = 0;
    std::size_t pairs = 0;
    std::size_t glides = 0;
    std::size_t steady = 0;
    for (std::size_t k = 0; k < frames; ++k) {
        if (levelDb[k] >= loud - 30.0) {
            flatSum += flatness[k];
        }
        if (pitch[k] > 0.0) {
            ++voiced;
            if (k > 0 && pitch[k - 1] > 0.0) {
                ++pairs;
                const double semitones = std::abs(12.0 * std::log2(pitch[k] / pitch[k - 1]));
                if (semitones < 0.08) {
                    ++steady;
                } else if (semitones <= 2.0) {
                    ++glides;
                }
            }
        }
    }
    f.flatness = audible.empty() ? 0.0 : flatSum / static_cast<double>(audible.size());
    f.voicedShare = audible.empty() ? 0.0 : static_cast<double>(voiced) / static_cast<double>(audible.size());
    f.glideShare = pairs > 0 ? static_cast<double>(glides) / static_cast<double>(pairs) : 0.0;
    f.steadyShare = pairs > 0 ? static_cast<double>(steady) / static_cast<double>(pairs) : 0.0;

    // Syllabic modulation: the level envelope's spectrum, 2.5..7 Hz against 0.5..16 Hz.
    double mean = 0.0;
    for (const double v : rms) {
        mean += v;
    }
    mean /= static_cast<double>(frames);
    double band = 0.0;
    double total = 0.0;
    for (double hz = 0.5; hz <= 16.0 && hz < frameRate / 2.0; hz += 0.25) {
        double re = 0.0;
        double im = 0.0;
        const double w = 2.0 * std::numbers::pi * hz / frameRate;
        for (std::size_t k = 0; k < frames; ++k) {
            const double v = rms[k] - mean;
            re += v * std::cos(w * static_cast<double>(k));
            im -= v * std::sin(w * static_cast<double>(k));
        }
        const double p = re * re + im * im;
        total += p;
        if (hz >= 2.5 && hz <= 7.0) {
            band += p;
        }
    }
    f.syllabicModulation = total > 0.0 ? band / total : 0.0;

    // Beat regularity: the onset envelope's autocorrelation peak between 0.25 and 1.5 s.
    std::vector<double> onset(frames, 0.0);
    for (std::size_t k = 1; k < frames; ++k) {
        onset[k] = std::max(0.0, std::max(levelDb[k], loud - 60.0) - std::max(levelDb[k - 1], loud - 60.0));
    }
    double onsetMean = 0.0;
    for (const double v : onset) {
        onsetMean += v;
    }
    onsetMean /= static_cast<double>(frames);
    double zero = 0.0;
    for (double& v : onset) {
        v -= onsetMean;
        zero += v * v;
    }
    const auto lagFrom = static_cast<std::size_t>(0.25 * frameRate);
    const auto lagTo = std::min(static_cast<std::size_t>(1.5 * frameRate), frames / 2);
    double regular = 0.0;
    for (std::size_t lag = std::max<std::size_t>(lagFrom, 1); lag <= lagTo; ++lag) {
        double c = 0.0;
        for (std::size_t k = 0; k + lag < frames; ++k) {
            c += onset[k] * onset[k + lag];
        }
        const double norm = zero * static_cast<double>(frames - lag) / static_cast<double>(frames);
        if (norm > 0.0) {
            regular = std::max(regular, c / norm);
        }
    }
    f.beatRegularity = std::clamp(regular, 0.0, 1.0);
    return f;
}

SoundGuess classifySound(const SoundFeatures& f) {
    SoundGuess g;
    g.features = f;
    if (f.seconds <= 0.0 || f.levelDb < -70.0) {
        g.silent = true;
        return g;
    }
    // Dialogue: syllable-rate modulation, voiced and gliding pitch (intonation), short gaps, a level that rises and
    // falls with the phrases (music with a melody glides too, but keeps its level).
    const double dialogue = 0.30 * above(f.syllabicModulation, 0.30, 0.06) + 0.15 * within(f.voicedShare, 0.2, 0.9, 0.05) +
                            0.25 * above(f.glideShare, 0.30, 0.06) + 0.15 * within(f.pauseShare, 0.05, 0.6, 0.04) +
                            0.15 * above(f.levelSpreadDb, 4.0, 1.0);
    // Music: a regular beat, held notes, sound throughout.
    const double music = 0.30 * above(f.beatRegularity, 0.30, 0.06) + 0.30 * above(f.steadyShare, 0.35, 0.07) +
                         0.20 * above(f.voicedShare, 0.35, 0.08) + 0.20 * below(f.pauseShare, 0.15, 0.05);
    // Sound effects: events standing out of quiet (high crest, gaps), little pitch.
    const double sfx = 0.40 * above(f.crestDb, 14.0, 3.0) + 0.30 * above(f.pauseShare, 0.30, 0.07) +
                       0.30 * below(f.voicedShare, 0.30, 0.08);
    // Ambience: a steady, noise-like bed without gaps.
    const double ambience = 0.40 * above(f.flatness, 0.25, 0.06) + 0.30 * below(f.levelSpreadDb, 6.0, 1.5) +
                            0.30 * below(f.pauseShare, 0.05, 0.02);
    g.scores = {dialogue, music, sfx, ambience};
    std::size_t best = 0;
    for (std::size_t i = 1; i < g.scores.size(); ++i) {
        if (g.scores[i] > g.scores[best]) {
            best = i;
        }
    }
    double second = 0.0;
    for (std::size_t i = 0; i < g.scores.size(); ++i) {
        if (i != best) {
            second = std::max(second, g.scores[i]);
        }
    }
    constexpr SoundType kTypes[] = {SoundType::Dialogue, SoundType::Music, SoundType::Sfx, SoundType::Ambience};
    g.type = kTypes[best];
    g.confidence = std::clamp((g.scores[best] - second) * 2.5, 0.0, 1.0);
    return g;
}

SoundGuess classifySound(std::span<const float> mono, int sampleRate) { return classifySound(soundFeatures(mono, sampleRate)); }

} // namespace wabcut::audio::edit
