#pragma once

// Synthetic pictures and audio for the analysis tests (C.5): deterministic scenes with motion, cuts, flashes, fades
// and dissolves; click tracks with accented downbeats; a synthesized drum loop; a WAV writer.

#include <wabcut/ai/analysis/ShotDetection.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <vector>

namespace wabcut::ai::analysis::test {

inline constexpr int kW = 160;
inline constexpr int kH = 90;
inline constexpr Rate kFps{25, 1};

/// Frame `n` of scene `scene`: a coloured background with stripes and a square that moves `speed` px a frame.
inline AnalysisFrame sceneFrame(int scene, std::int64_t n, int speed = 1) {
    AnalysisFrame f;
    f.pts = framesToTicks(n, kFps);
    f.duration = framesToTicks(1, kFps);
    f.width = kW;
    f.height = kH;
    f.rgb.resize(kW * kH * 3);
    const double hue = std::fmod(scene * 97.0, 360.0);
    const auto channel = [&](double offset) {
        return 0.5 + 0.45 * std::cos((hue + offset) * std::numbers::pi / 180.0);
    };
    const double r = channel(0.0);
    const double g = channel(120.0);
    const double b = channel(240.0);
    const int square = static_cast<int>((n * speed) % (kW - 20));
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const double stripe = ((x / (8 + scene % 5)) + (y / (6 + scene % 3))) % 2 == 0 ? 1.0 : 0.7;
            const bool inSquare = x >= square && x < square + 20 && y >= 30 && y < 50;
            std::uint8_t* px = &f.rgb[(static_cast<std::size_t>(y) * kW + x) * 3];
            px[0] = static_cast<std::uint8_t>(inSquare ? 250 : 255.0 * r * stripe);
            px[1] = static_cast<std::uint8_t>(inSquare ? 250 : 255.0 * g * stripe);
            px[2] = static_cast<std::uint8_t>(inSquare ? 20 : 255.0 * b * stripe);
        }
    }
    return f;
}

inline AnalysisFrame scaled(AnalysisFrame f, double gain) {
    for (std::uint8_t& v : f.rgb) {
        v = static_cast<std::uint8_t>(std::clamp(std::lround(v * gain), 0L, 255L));
    }
    return f;
}

inline AnalysisFrame mixed(const AnalysisFrame& a, const AnalysisFrame& b, double t) {
    AnalysisFrame f = a;
    for (std::size_t i = 0; i < f.rgb.size(); ++i) {
        f.rgb[i] = static_cast<std::uint8_t>(std::lround(a.rgb[i] * (1.0 - t) + b.rgb[i] * t));
    }
    return f;
}

inline AnalysisFrame flat(std::int64_t n, std::uint8_t value) {
    AnalysisFrame f = sceneFrame(0, n);
    std::fill(f.rgb.begin(), f.rgb.end(), value);
    return f;
}

/// Pseudo-random noise in [-1, 1] (deterministic).
struct Noise {
    std::uint32_t state = 12345;
    double next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<double>(state >> 8) / static_cast<double>(1u << 23) - 1.0;
    }
};

/// A click track: a 1 kHz click (500 Hz and twice as loud on downbeats, every `beatsPerBar` beats) at
/// phase + k x 60 / bpm, over a quiet noise floor. `clicks` receives each click's start sample.
inline std::vector<float> clickTrack(double bpm, double phaseSeconds, double seconds, int rate, int beatsPerBar,
                                     std::vector<std::int64_t>* clicks = nullptr) {
    std::vector<float> x(static_cast<std::size_t>(seconds * rate), 0.0f);
    Noise noise;
    for (float& v : x) {
        v = static_cast<float>(0.001 * noise.next());
    }
    const double period = 60.0 / bpm;
    for (int k = 0;; ++k) {
        const double t = phaseSeconds + k * period;
        const auto start = static_cast<std::int64_t>(std::llround(t * rate));
        if (start >= static_cast<std::int64_t>(x.size())) {
            break;
        }
        if (clicks) {
            clicks->push_back(start);
        }
        const bool down = k % beatsPerBar == 0;
        const double freq = down ? 500.0 : 1000.0;
        const double gain = down ? 0.8 : 0.4;
        for (std::int64_t i = 0; i < static_cast<std::int64_t>(0.03 * rate) && start + i < static_cast<std::int64_t>(x.size()); ++i) {
            const double tt = static_cast<double>(i) / rate;
            x[static_cast<std::size_t>(start + i)] +=
                static_cast<float>(gain * std::sin(2.0 * std::numbers::pi * freq * tt) * std::exp(-tt / 0.006));
        }
    }
    return x;
}

/// A drum loop: kick on beats 1 and 3, snare on 2 and 4, closed hi-hat on every eighth note. `beats` receives each
/// beat's start sample.
inline std::vector<float> drumLoop(double bpm, double phaseSeconds, double seconds, int rate,
                                   std::vector<std::int64_t>* beats = nullptr) {
    std::vector<float> x(static_cast<std::size_t>(seconds * rate), 0.0f);
    Noise noise;
    const auto add = [&](std::int64_t start, double length, auto&& sample) {
        for (std::int64_t i = 0; i < static_cast<std::int64_t>(length * rate) && start + i < static_cast<std::int64_t>(x.size()); ++i) {
            x[static_cast<std::size_t>(start + i)] += static_cast<float>(sample(static_cast<double>(i) / rate));
        }
    };
    const double eighth = 30.0 / bpm;
    for (int k = 0;; ++k) {
        const double t = phaseSeconds + k * eighth;
        const auto start = static_cast<std::int64_t>(std::llround(t * rate));
        if (start >= static_cast<std::int64_t>(x.size())) {
            break;
        }
        // Hats on every eighth.
        add(start, 0.03, [&](double tt) { return 0.12 * noise.next() * std::exp(-tt / 0.008); });
        if (k % 2 != 0) {
            continue;
        }
        if (beats) {
            beats->push_back(start);
        }
        const int beat = (k / 2) % 4;
        if (beat == 0 || beat == 2) {
            // Kick: a falling sine, 150 -> 50 Hz.
            double phase = 0.0;
            add(start, 0.18, [&](double tt) {
                const double f = 50.0 + 100.0 * std::exp(-tt / 0.03);
                phase += 2.0 * std::numbers::pi * f / rate;
                return 0.9 * std::sin(phase) * std::exp(-tt / 0.06);
            });
        } else {
            // Snare: noise with a 200 Hz body.
            add(start, 0.12, [&](double tt) {
                return (0.5 * noise.next() + 0.3 * std::sin(2.0 * std::numbers::pi * 200.0 * tt)) * std::exp(-tt / 0.03);
            });
        }
    }
    return x;
}

/// A 16-bit mono WAV.
inline void writeWav(const std::filesystem::path& file, const std::vector<float>& x, int rate) {
    std::ofstream out(file, std::ios::binary);
    const auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const auto bytes = static_cast<std::uint32_t>(x.size() * 2);
    out.write("RIFF", 4);
    u32(36 + bytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(1);
    u32(static_cast<std::uint32_t>(rate));
    u32(static_cast<std::uint32_t>(rate * 2));
    u16(2);
    u16(16);
    out.write("data", 4);
    u32(bytes);
    for (const float v : x) {
        u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f))));
    }
}

} // namespace wabcut::ai::analysis::test
