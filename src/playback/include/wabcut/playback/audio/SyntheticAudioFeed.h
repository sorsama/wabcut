#pragma once

// SyntheticAudioFeed: a deterministic AudioFeed for tests and demos (no files, no decoding).

#include <wabcut/playback/audio/AudioFeed.h>

#include <wabcut/audio/RealTime.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>
#include <string>
#include <utility>

namespace wabcut::playback {

/// What a synthetic media stream contains. Every sample is a pure function of its source sample index
/// n = floor(sourceTime / ticksPerSample), so tests can compute expected output exactly.
struct SyntheticSignal {
    enum class Kind {
        Silence,
        Constant, ///< amplitude
        Sine,     ///< amplitude · sin(2π · frequency · n / rate)
        Ramp,     ///< sawtooth: amplitude · (2 · frac(frequency · n / rate) − 1); unique values for alignment checks
        Noise,    ///< white noise in [−amplitude, amplitude], a hash of (seed, n)
    };
    Kind kind = Kind::Sine;
    double frequency = 440.0;
    float amplitude = 0.5f;
    std::uint32_t seed = 1;
    /// Right channel (and higher) = left · channelScale, so channel routing and balance are observable.
    float channelScale = 1.0f;
    /// A.9: when set, channel c = left · channelGains[c] instead (0 past its end): distinct multichannel sources for the
    /// channel-mapping tests.
    std::vector<float> channelGains;
    /// Length of the media. Reads past it return fewer frames. nullopt = endless.
    std::optional<Ticks> duration;
};

class SyntheticAudioFeed final : public AudioFeed {
public:
    /// Defines media `media`, audio stream `streamOrdinal`. Unknown streams fail to read (an Error).
    /// Thread-safe.
    void setSignal(MediaId media, int streamOrdinal, SyntheticSignal signal);
    /// Defines an offline take file (A.4): readTake(path) plays `signal` (sample n = floor(sourceTime / ticksPerSample),
    /// like a stream). Unknown paths fail to read. Thread-safe.
    void setTake(std::string path, SyntheticSignal signal);
    /// Makes every read() sleep this long first (a slow decoder, for underrun tests). Thread-safe.
    void setReadDelay(std::chrono::microseconds delay) { m_delayUs.store(delay.count(), std::memory_order_relaxed); }

    std::expected<std::size_t, Error> read(MediaId media, int streamOrdinal, Ticks sourceTime,
                                           std::span<float> interleaved, int channels, Rate rate) override;
    std::expected<std::size_t, Error> readTake(const std::string& path, Ticks sourceTime, std::span<float> interleaved,
                                               int channels, Rate rate) override;

    /// The value read() produces for source sample `sampleIndex` on `channel` (0 when the stream is unknown or
    /// the sample lies outside the media).
    float sample(MediaId media, int streamOrdinal, std::int64_t sampleIndex, int channel, Rate rate) const;

    std::uint64_t reads() const noexcept { return m_reads.load(std::memory_order_relaxed); }
    std::uint64_t takeReads() const noexcept { return m_takeReads.load(std::memory_order_relaxed); }

    /// The pure signal function.
    static float evaluate(const SyntheticSignal& signal, std::int64_t sampleIndex, int channel, Rate rate);

private:
    std::optional<SyntheticSignal> find(MediaId media, int streamOrdinal) const;
    static std::expected<std::size_t, Error> fill(const SyntheticSignal& signal, Ticks sourceTime, std::span<float> interleaved,
                                                  int channels, Rate rate);

    mutable audio::rt::CheckedMutex m_mutex;
    std::map<std::pair<std::uint64_t, int>, SyntheticSignal> m_signals;
    std::map<std::string, SyntheticSignal> m_takes;
    std::atomic<std::uint64_t> m_takeReads{0};
    std::atomic<std::int64_t> m_delayUs{0};
    std::atomic<std::uint64_t> m_reads{0};
};

} // namespace wabcut::playback
