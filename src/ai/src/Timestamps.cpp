#include <wabcut/ai/Timestamps.h>

#include "JsonUtil.h"

#include <chrono>
#include <format>

namespace wabcut::ai {

static_assert(kTicksPerSecond % 100 == 0 && kTicksPerSecond % kTranscriptionSampleRate == 0,
              "centiseconds and 16 kHz samples must be whole numbers of ticks");

std::optional<Ticks> centisecondsToTicks(std::int64_t centiseconds) {
    return mulDiv(Ticks{centiseconds}, kTicksPerSecond, 100, Rounding::Nearest);
}

std::optional<Ticks> transcriptionSamplesToTicks(std::int64_t samples) {
    return framesToTicks(samples, kTranscriptionRate, Rounding::Nearest);
}

std::optional<Ticks> chunkTimeToSource(Ticks origin, std::int64_t chunkStart, std::int64_t centiseconds) {
    if (chunkStart < 0 || centiseconds < 0) {
        return std::nullopt;
    }
    const auto start = transcriptionSamplesToTicks(chunkStart);
    const auto offset = centisecondsToTicks(centiseconds);
    if (!start || !offset) {
        return std::nullopt;
    }
    const auto chunk = checkedAdd(origin, *start);
    return chunk ? checkedAdd(*chunk, *offset) : std::nullopt;
}

namespace detail {

std::string utcNowIso8601() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", now);
}

} // namespace detail

} // namespace wabcut::ai
