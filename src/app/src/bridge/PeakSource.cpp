#include "PeakSource.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wabcut::app {

namespace {

// 40 ms source-time cells: fine enough to look like speech/music at normal zoom, coarse enough that a slice
// never needs more than a handful of samples.
constexpr std::int64_t kCellTicks = kTicksPerSecond / 25;

std::uint64_t mix(std::uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

float cellValue(std::uint64_t seed, std::int64_t cell) {
    const std::uint64_t h = mix(seed ^ (static_cast<std::uint64_t>(cell) * 0x9e3779b97f4a7c15ULL));
    return static_cast<float>(h >> 40) / static_cast<float>(1ULL << 24); // [0, 1)
}

// Smooth envelope (phrases) times per-cell detail, in [0.08, 0.95].
float amplitude(std::uint64_t seed, std::int64_t cell) {
    const double phase = static_cast<double>(seed % 997);
    const double phrase = 0.5 + 0.5 * std::sin(static_cast<double>(cell) * 0.11 + phase);
    const double detail = 0.45 + 0.55 * cellValue(seed, cell);
    return static_cast<float>(std::clamp(0.08 + 0.87 * phrase * detail, 0.0, 0.95));
}

} // namespace

bool PlaceholderPeakSource::peaks(MediaId media, StreamRef stream, Ticks sourceStart, Ticks sourceEnd,
                                  std::span<float> out) const {
    if (out.empty() || sourceEnd <= sourceStart)
        return false;
    const std::uint64_t seed = mix(media.value * 31 + static_cast<std::uint64_t>(stream.index));
    const std::int64_t span = sourceEnd.value - sourceStart.value;
    const auto slices = static_cast<std::int64_t>(out.size());
    for (std::int64_t i = 0; i < slices; ++i) {
        // Peak of the cells this slice covers (max, like a real min/max reduction).
        // span * i / slices without overflowing for long sources.
        const auto sliceStart = [&](std::int64_t k) {
            return sourceStart.value + (span / slices) * k + (span % slices) * k / slices;
        };
        const std::int64_t a = sliceStart(i);
        const std::int64_t b = sliceStart(i + 1);
        const std::int64_t firstCell = floorDiv(a, kCellTicks);
        const std::int64_t lastCell = std::max(firstCell, floorDiv(b - 1, kCellTicks));
        float peak = 0.0f;
        // Bounded: zoomed far out a slice spans many cells; sample at most 8 of them.
        const std::int64_t step = std::max<std::int64_t>(1, (lastCell - firstCell + 1) / 8);
        for (std::int64_t cell = firstCell; cell <= lastCell; cell += step)
            peak = std::max(peak, amplitude(seed, cell));
        out[static_cast<std::size_t>(i)] = peak;
    }
    return true;
}

} // namespace wabcut::app
