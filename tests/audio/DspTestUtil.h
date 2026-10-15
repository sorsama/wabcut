#pragma once

// Signal helpers for the DSP tests: tones, noise and level measurements.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

namespace wabcut::audio::test {

inline double dbToAmp(double db) { return std::pow(10.0, db / 20.0); }
inline double ampToDb(double a) { return a > 0.0 ? 20.0 * std::log10(a) : -300.0; }

/// Interleaved sine, the same on every channel.
inline std::vector<float> sine(double frequency, double amplitude, double seconds, int channels = 2, int rate = 48000,
                               double phase = 0.0) {
    const auto frames = static_cast<std::size_t>(std::llround(seconds * rate));
    std::vector<float> out(frames * static_cast<std::size_t>(channels));
    for (std::size_t i = 0; i < frames; ++i) {
        const auto v = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * frequency * i / rate + phase));
        for (int c = 0; c < channels; ++c) {
            out[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] = v;
        }
    }
    return out;
}

/// Deterministic white noise in [-amplitude, amplitude] (xorshift), different per channel.
inline std::vector<float> noise(double amplitude, double seconds, int channels = 2, int rate = 48000,
                                std::uint32_t seed = 12345) {
    const auto n = static_cast<std::size_t>(std::llround(seconds * rate)) * static_cast<std::size_t>(channels);
    std::vector<float> out(n);
    std::uint32_t s = seed;
    for (float& v : out) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        v = static_cast<float>(amplitude * (static_cast<double>(s) / 4294967295.0 * 2.0 - 1.0));
    }
    return out;
}

inline void append(std::vector<float>& a, const std::vector<float>& b) { a.insert(a.end(), b.begin(), b.end()); }

/// Peak |x| over interleaved frames [first, last).
inline double peak(const std::vector<float>& x, int channels, std::size_t first = 0, std::size_t last = SIZE_MAX) {
    const std::size_t frames = x.size() / static_cast<std::size_t>(channels);
    last = std::min(last, frames);
    double p = 0.0;
    for (std::size_t i = first * static_cast<std::size_t>(channels); i < last * static_cast<std::size_t>(channels); ++i) {
        p = std::max(p, static_cast<double>(std::fabs(x[i])));
    }
    return p;
}

/// RMS of channel 0 over frames [first, last).
inline double rms(const std::vector<float>& x, int channels, std::size_t first, std::size_t last) {
    double sum = 0.0;
    for (std::size_t i = first; i < last; ++i) {
        const double v = x[i * static_cast<std::size_t>(channels)];
        sum += v * v;
    }
    return std::sqrt(sum / static_cast<double>(std::max<std::size_t>(last - first, 1)));
}

} // namespace wabcut::audio::test
