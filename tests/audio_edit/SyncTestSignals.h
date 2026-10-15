#pragma once

// Test signals for audio sync (A.5): "recordings" of one synthetic event, defined in continuous time so every rate
// reads the same sound, with a recording's start, gain, noise, a room (a few decaying echoes), a band limit and a
// clock skew of its own.

#include <wabcut/audio/edit/AudioSync.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>
#include <span>
#include <vector>

namespace wabcut::test {

/// The event: speech-like bursts (syllables of 60..260 ms, each a few partials with a fast attack and a decay)
/// separated by short pauses, `seconds` long. A pure function of time.
class SyntheticEvent {
public:
    SyntheticEvent(double seconds, std::uint32_t seed) {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<double> length(0.06, 0.26);
        std::uniform_real_distribution<double> pause(0.02, 0.35);
        std::uniform_real_distribution<double> pitch(110.0, 260.0);
        std::uniform_real_distribution<double> formant(400.0, 3200.0);
        std::uniform_real_distribution<double> level(0.25, 1.0);
        for (double t = 0.1; t < seconds; ) {
            Burst b;
            b.start = t;
            b.length = length(rng);
            b.level = level(rng);
            const double f0 = pitch(rng);
            for (int k = 0; k < 6; ++k) {
                b.freq[k] = k < 3 ? f0 * (k + 1) : formant(rng);
                b.amp[k] = k < 3 ? 1.0 / (k + 1) : 0.5;
            }
            m_bursts.push_back(b);
            t += b.length + pause(rng);
        }
        m_seconds = seconds;
    }

    double seconds() const { return m_seconds; }

    /// The sound at time t (seconds; silence outside the event).
    double at(double t) const {
        if (t < 0.0 || t >= m_seconds) {
            return 0.0;
        }
        auto it = std::upper_bound(m_bursts.begin(), m_bursts.end(), t, [](double v, const Burst& b) { return v < b.start; });
        if (it == m_bursts.begin()) {
            return 0.0;
        }
        const Burst& b = *(it - 1);
        const double u = t - b.start;
        if (u >= b.length) {
            return 0.0;
        }
        const double envelope = std::min(1.0, u / 0.008) * std::exp(-3.0 * u / b.length);
        double s = 0.0;
        for (int k = 0; k < 6; ++k) {
            s += b.amp[k] * std::sin(2.0 * std::numbers::pi * b.freq[k] * t);
        }
        return 0.25 * b.level * envelope * s;
    }

private:
    struct Burst {
        double start = 0.0;
        double length = 0.0;
        double level = 1.0;
        double freq[6]{};
        double amp[6]{};
    };
    std::vector<Burst> m_bursts;
    double m_seconds = 0.0;
};

/// One device's recording of the event.
struct Recording {
    double start = 0.0;      ///< event time of the recording's first sample (seconds)
    double seconds = 60.0;   ///< length
    double gain = 1.0;
    double noise = 0.0;      ///< white noise level (amplitude)
    bool room = false;       ///< echoes at 23, 41, 67 and 97 ms
    double skewPpm = 0.0;    ///< the device's clock runs fast by this much
    std::uint32_t noiseSeed = 1;
};

class RecordingSignal final : public audio::edit::SyncSignal {
public:
    RecordingSignal(const SyntheticEvent& event, Recording r) : m_event(event), m_r(r) {}

    std::int64_t frames(int rate) const override { return static_cast<std::int64_t>(m_r.seconds * rate); }

    std::expected<void, Error> read(int rate, std::int64_t first, std::span<float> out) override {
        ++reads;
        const std::int64_t length = frames(rate);
        for (std::size_t i = 0; i < out.size(); ++i) {
            const std::int64_t n = first + static_cast<std::int64_t>(i);
            if (n < 0 || n >= length) {
                out[i] = 0.0f;
                continue;
            }
            // Device time n / rate is event time start + (n / rate) / (1 + skew).
            const double t = m_r.start + (static_cast<double>(n) / rate) / (1.0 + m_r.skewPpm * 1e-6);
            double s = m_event.at(t);
            if (m_r.room) {
                s += 0.5 * m_event.at(t - 0.023) + 0.35 * m_event.at(t - 0.041) + 0.25 * m_event.at(t - 0.067) +
                     0.15 * m_event.at(t - 0.097);
            }
            s *= m_r.gain;
            if (m_r.noise > 0.0) {
                // Deterministic white noise per (seed, rate, sample).
                std::uint64_t z = static_cast<std::uint64_t>(n) * 0x9E3779B97F4A7C15ull + m_r.noiseSeed * 0xBF58476D1CE4E5B9ull +
                                  static_cast<std::uint64_t>(rate);
                z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
                z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
                z ^= z >> 31;
                s += m_r.noise * (static_cast<double>(z >> 11) / 4503599627370496.0 - 1.0);
            }
            out[i] = static_cast<float>(s);
        }
        return {};
    }

    int reads = 0;

private:
    const SyntheticEvent& m_event;
    Recording m_r;
};

} // namespace wabcut::test
