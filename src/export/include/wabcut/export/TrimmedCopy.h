#pragma once

// Trimmed copies (I.4, Collect Files): the part of a media file a project uses, copied without re-encoding. Every
// stream is stream-copied from the keyframe at or before the kept range up to the next keyframe after it (whole
// GOPs, so every kept frame decodes), into the same container type. The copy starts its own time at that
// keyframe; `shift` says how much earlier everything moved (source time in the copy = source time in the original
// - shift), measured on the written file, so media-time data (clip In points, keyframe curves) can follow exactly.

#include <wabcut/model/Error.h>
#include <wabcut/model/Time.h>

#include <expected>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>

namespace wabcut::exporter {

struct TrimmedCopy {
    std::filesystem::path file;
    /// Original source time - copied source time (>= 0).
    Ticks shift{};
    /// Source time the copy covers (from its first to its last packet).
    Ticks duration{};
};

/// Whether trimmedCopy() can write this container (by file extension): .mp4/.m4v/.mov (MOV family) and .mkv.
bool canTrimCopy(const std::filesystem::path& file);

/// Copies [keep.start, keep.end()) of `source` (UTF-8 path, source time) into `output` (same container family),
/// keyframe-aligned as described above. Writes "<output>.partial" and renames on success; nothing is left behind on
/// an error or a stop request. `onProgress` gets 0..1. Errors: NotFound, Unsupported (container), Cancelled,
/// IoError, EncoderError.
std::expected<TrimmedCopy, Error> trimmedCopy(const std::string& source, const std::filesystem::path& output, TimeRange keep,
                                              std::stop_token stop = {}, const std::function<void(double)>& onProgress = {});

} // namespace wabcut::exporter
