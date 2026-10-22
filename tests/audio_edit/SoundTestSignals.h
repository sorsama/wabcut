#pragma once

// p02 audio: synthetic sounds of the four Essential Sound types for the auto-tag classifier, deterministic (seeded),
// mono at 16 kHz: speech-like talk (a gliding glottal pulse train through moving formants, in syllables and words with
// pauses), music (held chords of harmonic tones over a steady beat), sound effects (impacts and whooshes out of
// silence) and ambience (a steady noise bed: rain, room tone).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

namespace wabcut::test::sounds {

inline constexpr int kRate = 16000;

struct Rng {
    std::uint32_t s;
    explicit Rng(std::uint32_t seed) : s(seed ? seed : 1u) {}
    double next() { // 0..1
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<double>(s) / 4294967295.0;
    }
    double noise() { return next() * 2.0 - 1.0; }
};

/// A two-pole resonator (a formant) on x, in place.
inline void resonate(std::vector<double>& x, std::size_t from, std::size_t to, double hz, double bandwidth, double gain) {
    const double r = std::exp(-std::numbers::pi * bandwidth / kRate);
    const double c = 2.0 * r * std::cos(2.0 * std::numbers::pi * hz / kRate);
    double y1 = 0.0;
    double y2 = 0.0;
    std::vector<double> out(to - from);
    for (std::size_t i = from; i < to; ++i) {
        const double y = (1.0 - r) * x[i] + c * y1 - r * r * y2;
        y2 = y1;
        y1 = y;
        out[i - from] = y * gain;
    }
    for (std::size_t i = from; i < to; ++i) {
        x[i] = out[i - from];
    }
}

/// Talk: words of 2-4 syllables (~4.5 syllables a second) separated by pauses, the pitch gliding around `f0`.
inline std::vector<float> speech(double seconds, std::uint32_t seed = 7, double f0 = 140.0) {
    Rng rng(seed);
    const auto total = static_cast<std::size_t>(seconds * kRate);
    std::vector<double> out(total, 0.0);
    std::size_t at = static_cast<std::size_t>(0.1 * kRate);
    const double vowels[][3] = {{730, 1090, 2440}, {270, 2290, 3010}, {300, 870, 2240}, {530, 1840, 2480}, {640, 1190, 2390}};
    double phase = 0.0;
    while (at < total) {
        const int syllables = 2 + static_cast<int>(rng.next() * 3.0);
        for (int s = 0; s < syllables && at < total; ++s) {
            const auto length = static_cast<std::size_t>((0.16 + 0.08 * rng.next()) * kRate);
            const std::size_t end = std::min(total, at + length);
            const double* v = vowels[static_cast<std::size_t>(rng.next() * 4.999)];
            const double start = f0 * (0.9 + 0.25 * rng.next());
            const double slope = (rng.next() - 0.5) * 0.6; // intonation: up to +-30 % over the syllable
            std::vector<double> voice(end - at, 0.0);
            for (std::size_t i = at; i < end; ++i) {
                const double u = static_cast<double>(i - at) / static_cast<double>(end - at);
                const double hz = start * (1.0 + slope * u) * (1.0 + 0.03 * std::sin(2.0 * std::numbers::pi * 5.0 * u));
                phase += hz / kRate;
                phase -= std::floor(phase);
                const double pulse = phase < 0.08 ? 1.0 - phase / 0.08 : -0.1; // a glottal-like pulse train
                const double envelope = std::sin(std::numbers::pi * u);            // the syllable's rise and fall
                voice[i - at] = pulse * envelope;
            }
            std::vector<double> formants(end - at, 0.0);
            for (int k = 0; k < 3; ++k) {
                std::vector<double> band = voice;
                resonate(band, 0, band.size(), v[k], 80.0 + 40.0 * k, 1.0 / (k + 1));
                for (std::size_t i = 0; i < band.size(); ++i) {
                    formants[i] += band[i];
                }
            }
            // A consonant: a short noise burst at the syllable's start.
            const auto consonant = std::min<std::size_t>(static_cast<std::size_t>(0.03 * kRate), formants.size());
            for (std::size_t i = 0; i < consonant; ++i) {
                formants[i] += 0.05 * rng.noise();
            }
            for (std::size_t i = at; i < end; ++i) {
                out[i] += formants[i - at];
            }
            at = end;
        }
        at += static_cast<std::size_t>((0.15 + 0.3 * rng.next()) * kRate); // the pause between words
    }
    double peak = 1e-9;
    for (const double v : out) {
        peak = std::max(peak, std::fabs(v));
    }
    std::vector<float> f(total);
    for (std::size_t i = 0; i < total; ++i) {
        f[i] = static_cast<float>(0.5 * out[i] / peak);
    }
    return f;
}

/// Music: chords of harmonic tones held for a beat or two, a kick on every beat and a hi-hat between, at `bpm`.
inline std::vector<float> music(double seconds, double bpm = 120.0, std::uint32_t seed = 3) {
    Rng rng(seed);
    const auto total = static_cast<std::size_t>(seconds * kRate);
    std::vector<double> out(total, 0.0);
    const double beat = 60.0 / bpm;
    const double roots[] = {220.0, 196.0, 174.61, 246.94};
    std::size_t chord = 0;
    for (double t = 0.0; t < seconds; t += 2.0 * beat, ++chord) {
        const double root = roots[chord % 4];
        const double notes[] = {root, root * 1.25992, root * 1.49831};
        const auto from = static_cast<std::size_t>(t * kRate);
        const std::size_t to = std::min(total, static_cast<std::size_t>((t + 2.0 * beat) * kRate));
        for (std::size_t i = from; i < to; ++i) {
            const double time = static_cast<double>(i) / kRate;
            double v = 0.0;
            for (const double hz : notes) {
                for (int h = 1; h <= 6; ++h) {
                    v += std::sin(2.0 * std::numbers::pi * hz * h * time) / (h * h);
                }
            }
            out[i] += 0.12 * v;
        }
    }
    for (double t = 0.0; t < seconds; t += beat) {
        const auto from = static_cast<std::size_t>(t * kRate);
        for (std::size_t i = from; i < std::min(total, from + static_cast<std::size_t>(0.15 * kRate)); ++i) {
            const double u = static_cast<double>(i - from) / kRate;
            out[i] += 0.8 * std::exp(-u * 30.0) * std::sin(2.0 * std::numbers::pi * (60.0 + 80.0 * std::exp(-u * 40.0)) * u);
        }
        const auto hat = static_cast<std::size_t>((t + beat / 2.0) * kRate);
        for (std::size_t i = hat; i < std::min(total, hat + static_cast<std::size_t>(0.05 * kRate)); ++i) {
            out[i] += 0.15 * std::exp(-static_cast<double>(i - hat) / kRate * 80.0) * rng.noise();
        }
    }
    double peak = 1e-9;
    for (const double v : out) {
        peak = std::max(peak, std::fabs(v));
    }
    std::vector<float> f(total);
    for (std::size_t i = 0; i < total; ++i) {
        f[i] = static_cast<float>(0.6 * out[i] / peak);
    }
    return f;
}

/// Sound effects: impacts (noise and a thump that decay fast) and whooshes (a swelling noise), out of near silence.
inline std::vector<float> effects(double seconds, std::uint32_t seed = 11) {
    Rng rng(seed);
    const auto total = static_cast<std::size_t>(seconds * kRate);
    std::vector<double> out(total, 0.0);
    double t = 0.3;
    int kind = 0;
    while (t < seconds - 0.6) {
        const auto from = static_cast<std::size_t>(t * kRate);
        if (kind % 2 == 0) {
            for (std::size_t i = from; i < std::min(total, from + static_cast<std::size_t>(0.5 * kRate)); ++i) {
                const double u = static_cast<double>(i - from) / kRate;
                out[i] += std::exp(-u * 12.0) * (0.8 * rng.noise() + 0.6 * std::sin(2.0 * std::numbers::pi * 55.0 * u));
            }
        } else {
            for (std::size_t i = from; i < std::min(total, from + static_cast<std::size_t>(0.6 * kRate)); ++i) {
                const double u = static_cast<double>(i - from) / (0.6 * kRate);
                out[i] += 0.5 * std::sin(std::numbers::pi * u) * std::sin(std::numbers::pi * u) * rng.noise();
            }
        }
        ++kind;
        t += 0.9 + 1.4 * rng.next();
    }
    for (std::size_t i = 0; i < total; ++i) {
        out[i] += 0.0005 * rng.noise(); // a faint floor
    }
    std::vector<float> f(total);
    for (std::size_t i = 0; i < total; ++i) {
        f[i] = static_cast<float>(0.7 * out[i]);
    }
    return f;
}

/// Ambience: a steady noise bed (rain on a window, a room's tone) with a slow, slight swell.
inline std::vector<float> ambience(double seconds, std::uint32_t seed = 5) {
    Rng rng(seed);
    const auto total = static_cast<std::size_t>(seconds * kRate);
    std::vector<float> f(total);
    double low = 0.0;
    for (std::size_t i = 0; i < total; ++i) {
        const double time = static_cast<double>(i) / kRate;
        const double white = rng.noise();
        low = 0.97 * low + 0.03 * white; // some warmth
        const double swell = 1.0 + 0.15 * std::sin(2.0 * std::numbers::pi * 0.2 * time);
        f[i] = static_cast<float>(0.2 * swell * (0.6 * white + 2.0 * low));
    }
    return f;
}

/// `a` with `b` added at `gain`.
inline std::vector<float> mixed(std::vector<float> a, const std::vector<float>& b, double gain) {
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
        a[i] += static_cast<float>(gain * b[i]);
    }
    return a;
}

} // namespace wabcut::test::sounds
