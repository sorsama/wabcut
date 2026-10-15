#include <wabcut/audio/Sources.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace wabcut::audio {

void SilenceSource::render(std::span<float> interleaved, int /*channels*/, Rate /*sampleRate*/,
                           Ticks /*startTime*/) noexcept {
    std::fill(interleaved.begin(), interleaved.end(), 0.0f);
}

SineSource::SineSource(double frequencyHz, float amplitude) noexcept
    : m_frequency(std::isfinite(frequencyHz) ? frequencyHz : 0.0),
      m_amplitude(std::isfinite(amplitude) && amplitude > 0.0f ? amplitude : 0.0f) {}

void SineSource::render(std::span<float> interleaved, int channels, Rate sampleRate, Ticks startTime) noexcept {
    if (channels < 1 || !sampleRate.isValid()) {
        std::fill(interleaved.begin(), interleaved.end(), 0.0f);
        return;
    }
    const auto ch = static_cast<std::size_t>(channels);
    const std::size_t frames = interleaved.size() / ch;

    // Phase (in cycles) of the first frame: frequency · t, reduced modulo 1 without ever forming the huge product
    // frequency · ticks (which would lose all fractional precision in a double after a few minutes). Split t into
    // whole seconds and a sub-second remainder: frac(f·s) + f·r/TPS.
    const std::int64_t seconds = floorDiv(startTime.value, kTicksPerSecond);
    const std::int64_t remainder = startTime.value - seconds * kTicksPerSecond; // [0, TPS)
    double phase = m_frequency * static_cast<double>(seconds);
    phase -= std::floor(phase);
    phase += m_frequency * static_cast<double>(remainder) / static_cast<double>(kTicksPerSecond);
    phase -= std::floor(phase);

    // Advance per frame in cycles; re-anchored from the exact start every call, so error never accumulates
    // across calls, and within a call the drift is ~frames · 1e-16 cycles.
    const double step = m_frequency * static_cast<double>(sampleRate.den) / static_cast<double>(sampleRate.num);
    constexpr double kTwoPi = 2.0 * std::numbers::pi;
    for (std::size_t f = 0; f < frames; ++f) {
        const auto value = static_cast<float>(static_cast<double>(m_amplitude) * std::sin(kTwoPi * phase));
        for (std::size_t c = 0; c < ch; ++c) {
            interleaved[f * ch + c] = value;
        }
        phase += step;
        phase -= std::floor(phase);
    }
    std::fill(interleaved.begin() + static_cast<std::ptrdiff_t>(frames * ch), interleaved.end(), 0.0f);
}

RingBufferSource::RingBufferSource(std::shared_ptr<SpscFrameRing> ring) noexcept : m_ring(std::move(ring)) {}

void RingBufferSource::render(std::span<float> interleaved, int channels, Rate /*sampleRate*/,
                              Ticks /*startTime*/) noexcept {
    std::size_t framesRead = 0;
    const std::size_t frames = channels > 0 ? interleaved.size() / static_cast<std::size_t>(channels) : 0;
    if (m_ring && channels == m_ring->channels()) {
        framesRead = m_ring->read(interleaved);
    }
    const std::size_t samplesRead = framesRead * static_cast<std::size_t>(std::max(channels, 1));
    std::fill(interleaved.begin() + static_cast<std::ptrdiff_t>(samplesRead), interleaved.end(), 0.0f);
    if (framesRead < frames) {
        m_underrunFrames.fetch_add(frames - framesRead, std::memory_order_relaxed);
    }
}

} // namespace wabcut::audio
