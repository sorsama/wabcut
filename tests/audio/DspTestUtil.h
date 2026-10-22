#pragma once

// Signal helpers for the DSP tests: tones, noise and level measurements, and (p02) running an effect of the registry
// through an EffectChain.

#include <wabcut/audio/effects/EffectChain.h>

#include <wabcut/model/Effects.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string>
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
        const double angle = 2.0 * std::numbers::pi * frequency * static_cast<double>(i);
        const auto v = static_cast<float>(amplitude * std::sin(angle / rate + phase));
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

/// RMS of `channel` over frames [first, last).
inline double rmsOf(const std::vector<float>& x, int channels, int channel, std::size_t first, std::size_t last) {
    double sum = 0.0;
    const std::size_t frames = x.size() / static_cast<std::size_t>(channels);
    last = std::min(last, frames);
    for (std::size_t i = first; i < last; ++i) {
        const double v = x[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(channel)];
        sum += v * v;
    }
    return std::sqrt(sum / static_cast<double>(std::max<std::size_t>(last - first, 1)));
}

/// Amplitude of the `frequency` component of `channel` over frames [first, last) (one DFT bin, Hann-free: use whole
/// periods or long spans).
inline double toneAmplitude(const std::vector<float>& x, int channels, double frequency, std::size_t first, std::size_t last,
                            int channel = 0, int rate = 48000) {
    double re = 0.0;
    double im = 0.0;
    const std::size_t frames = x.size() / static_cast<std::size_t>(channels);
    last = std::min(last, frames);
    for (std::size_t i = first; i < last; ++i) {
        const double v = x[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(channel)];
        const double a = 2.0 * std::numbers::pi * frequency * static_cast<double>(i) / rate;
        re += v * std::cos(a);
        im -= v * std::sin(a);
    }
    const double n = static_cast<double>(std::max<std::size_t>(last - first, 1));
    return 2.0 * std::sqrt(re * re + im * im) / n;
}

/// A unit impulse (amplitude at frame `at`, every channel) in `frames` frames of silence.
inline std::vector<float> impulse(std::size_t frames, int channels, std::size_t at = 0, float amplitude = 1.0f) {
    std::vector<float> out(frames * static_cast<std::size_t>(channels), 0.0f);
    for (int c = 0; c < channels; ++c) {
        out[at * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] = amplitude;
    }
    return out;
}

/// An effect instance of the built-in registry (id 1, enabled) with `params` (sorted here).
inline EffectInstance effectOf(std::string id, std::vector<EffectParam> params = {}) {
    EffectInstance e;
    e.id = EffectInstanceId{1};
    e.effect = std::move(id);
    std::sort(params.begin(), params.end(), [](const EffectParam& a, const EffectParam& b) { return a.key < b.key; });
    e.params = std::move(params);
    return e;
}

/// Runs `effect` over interleaved `x` through an EffectChain (the path preview and export take), in blocks of `block`
/// frames from chain frame 0 at timeline 0. The output is as long as the input (an effect with latency delays it).
inline std::vector<float> runEffect(const EffectInstance& effect, std::vector<float> x, int channels, int rate = 48000,
                                    int block = 512) {
    const EffectStack stack{effect};
    EffectChain chain(channels, rate);
    chain.bind(stack);
    const std::size_t frames = x.size() / static_cast<std::size_t>(channels);
    const CurveClock clock{Ticks{}, Rate{rate, 1}.ticksPerUnit(), nullptr};
    for (std::size_t done = 0; done < frames;) {
        const std::size_t n = std::min(frames - done, static_cast<std::size_t>(block));
        chain.process(x.data() + done * static_cast<std::size_t>(channels), static_cast<int>(n),
                      static_cast<std::int64_t>(done), clock);
        done += n;
    }
    return x;
}

/// The latency an effect's processor reports (frames), as a bound chain sees it.
inline int effectLatency(const EffectInstance& effect, int channels, int rate = 48000) {
    const EffectStack stack{effect};
    EffectChain chain(channels, rate);
    chain.bind(stack);
    return chain.latencyFrames();
}

} // namespace wabcut::audio::test
