#include <wabcut/playback/audio/SyntheticAudioFeed.h>

#include <wabcut/audio/TimeMath.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <numbers>
#include <thread>

namespace wabcut::playback {

namespace {

/// frac(frequency · n / rate) without forming a huge product: split n into whole seconds and a remainder.
double cycles(double frequency, std::int64_t n, Rate rate) {
    const std::int64_t perSecond = rate.num / std::max(rate.den, 1);
    const std::int64_t seconds = floorDiv(n, std::max<std::int64_t>(perSecond, 1));
    const std::int64_t rest = n - seconds * perSecond;
    double phase = frequency * static_cast<double>(seconds);
    phase -= std::floor(phase);
    phase += frequency * static_cast<double>(rest) / static_cast<double>(perSecond);
    return phase - std::floor(phase);
}

std::uint32_t hash(std::uint32_t seed, std::int64_t n) {
    // splitmix64 finalizer
    std::uint64_t z = static_cast<std::uint64_t>(n) + 0x9E3779B97F4A7C15ULL * (seed + 1);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return static_cast<std::uint32_t>((z ^ (z >> 31)) >> 32);
}

} // namespace

float SyntheticAudioFeed::evaluate(const SyntheticSignal& s, std::int64_t n, int channel, Rate rate) {
    float value = 0.0f;
    switch (s.kind) {
    case SyntheticSignal::Kind::Silence: value = 0.0f; break;
    case SyntheticSignal::Kind::Constant: value = s.amplitude; break;
    case SyntheticSignal::Kind::Sine:
        value = static_cast<float>(static_cast<double>(s.amplitude) * std::sin(2.0 * std::numbers::pi * cycles(s.frequency, n, rate)));
        break;
    case SyntheticSignal::Kind::Ramp:
        value = static_cast<float>(static_cast<double>(s.amplitude) * (2.0 * cycles(s.frequency, n, rate) - 1.0));
        break;
    case SyntheticSignal::Kind::Noise:
        value = s.amplitude * (static_cast<float>(hash(s.seed, n)) / 2147483648.0f - 1.0f);
        break;
    }
    if (!s.channelGains.empty()) {
        return channel >= 0 && static_cast<std::size_t>(channel) < s.channelGains.size() ? value * s.channelGains[static_cast<std::size_t>(channel)]
                                                                                         : 0.0f;
    }
    return channel == 0 ? value : value * s.channelScale;
}

void SyntheticAudioFeed::setSignal(MediaId media, int streamOrdinal, SyntheticSignal signal) {
    std::lock_guard lock(m_mutex);
    m_signals[{media.value, streamOrdinal}] = signal;
}

std::optional<SyntheticSignal> SyntheticAudioFeed::find(MediaId media, int streamOrdinal) const {
    std::lock_guard lock(m_mutex);
    const auto it = m_signals.find({media.value, streamOrdinal});
    if (it == m_signals.end()) {
        return std::nullopt;
    }
    return it->second;
}

float SyntheticAudioFeed::sample(MediaId media, int streamOrdinal, std::int64_t n, int channel, Rate rate) const {
    const auto signal = find(media, streamOrdinal);
    if (!signal || n < 0) {
        return 0.0f;
    }
    if (signal->duration && n >= ticksToSamples(*signal->duration, rate)) {
        return 0.0f;
    }
    return evaluate(*signal, n, channel, rate);
}

std::expected<std::size_t, Error> SyntheticAudioFeed::read(MediaId media, int streamOrdinal, Ticks sourceTime,
                                                           std::span<float> interleaved, int channels, Rate rate) {
    m_reads.fetch_add(1, std::memory_order_relaxed);
    if (const std::int64_t delay = m_delayUs.load(std::memory_order_relaxed); delay > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(delay));
    }
    const auto signal = find(media, streamOrdinal);
    if (!signal) {
        return std::unexpected(Error{ErrorCode::NotFound, "synthetic stream not defined"});
    }
    return fill(*signal, sourceTime, interleaved, channels, rate);
}

void SyntheticAudioFeed::setTake(std::string path, SyntheticSignal signal) {
    std::lock_guard lock(m_mutex);
    m_takes[std::move(path)] = signal;
}

std::expected<std::size_t, Error> SyntheticAudioFeed::readTake(const std::string& path, Ticks sourceTime,
                                                               std::span<float> interleaved, int channels, Rate rate) {
    m_takeReads.fetch_add(1, std::memory_order_relaxed);
    std::optional<SyntheticSignal> signal;
    {
        std::lock_guard lock(m_mutex);
        if (const auto it = m_takes.find(path); it != m_takes.end()) {
            signal = it->second;
        }
    }
    if (!signal) {
        return std::unexpected(Error{ErrorCode::NotFound, "synthetic take not defined"});
    }
    return fill(*signal, sourceTime, interleaved, channels, rate);
}

std::expected<std::size_t, Error> SyntheticAudioFeed::fill(const SyntheticSignal& s, Ticks sourceTime, std::span<float> interleaved,
                                                           int channels, Rate rate) {
    const std::optional<SyntheticSignal> signal = s;
    if (channels < 1 || !rate.isExact()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "bad channel count or rate"});
    }
    const auto ch = static_cast<std::size_t>(channels);
    const auto frames = static_cast<std::int64_t>(interleaved.size() / ch);
    const std::int64_t first = ticksToSamples(sourceTime, rate);
    std::int64_t available = frames;
    if (signal->duration) {
        available = std::clamp<std::int64_t>(ticksToSamples(*signal->duration, rate) - first, 0, frames);
    }
    for (std::int64_t f = 0; f < available; ++f) {
        const std::int64_t n = first + f;
        for (std::size_t c = 0; c < ch; ++c) {
            // Before the media's start (negative source time) there is nothing: silence.
            interleaved[static_cast<std::size_t>(f) * ch + c] = n < 0 ? 0.0f : evaluate(*signal, n, static_cast<int>(c), rate);
        }
    }
    return static_cast<std::size_t>(available);
}

} // namespace wabcut::playback
