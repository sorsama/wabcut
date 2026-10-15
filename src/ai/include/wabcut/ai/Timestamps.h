#pragma once

// Time conversions between the recogniser's units and source Ticks, all checked (model/Time.h: never by hand).
//
// The transcription PCM is 16 kHz mono and sample 0 is source time `origin` (AudioDecoder: output sample n of a
// decoder covers source time samplesToTicks(n)). A chunk starts at sample `chunkStart`; whisper reports times in
// centiseconds from the start of the chunk. So
//     source time = origin + chunkStart * (kTicksPerSecond / 16000) + cs * (kTicksPerSecond / 100),
// which is exact in ticks (both factors divide kTicksPerSecond).

#include <wabcut/model/Time.h>

#include <cstdint>
#include <optional>

namespace wabcut::ai {

inline constexpr int kTranscriptionSampleRate = 16000;
inline constexpr Rate kTranscriptionRate{kTranscriptionSampleRate, 1};

/// Centiseconds -> ticks; nullopt on overflow.
std::optional<Ticks> centisecondsToTicks(std::int64_t centiseconds);

/// 16 kHz sample index -> ticks; nullopt on overflow.
std::optional<Ticks> transcriptionSamplesToTicks(std::int64_t samples);

/// Source time of `centiseconds` into the chunk that starts at 16 kHz sample `chunkStart` of PCM whose sample 0
/// is at source time `origin`. nullopt on overflow or a negative position.
std::optional<Ticks> chunkTimeToSource(Ticks origin, std::int64_t chunkStart, std::int64_t centiseconds);

/// Centiseconds from the start of a chunk -> 16 kHz samples (for clamping against chunk lengths).
constexpr std::int64_t centisecondsToSamples(std::int64_t centiseconds) {
    return centiseconds * (kTranscriptionSampleRate / 100);
}

} // namespace wabcut::ai
