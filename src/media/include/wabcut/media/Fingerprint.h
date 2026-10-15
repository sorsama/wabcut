#pragma once

// Source fingerprints: a cheap content identity for media files (architecture rev 2: cache keys and
// MediaLocator).

#include <wabcut/model/Error.h>

#include <cstdint>
#include <expected>
#include <string>

namespace wabcut::media {

/// 64-bit identity of a file's content: its size, its modification time in nanoseconds and a hash of its
/// first and last 64 KiB. Renaming or moving the file keeps the fingerprint; editing, replacing or touching
/// it changes it. Never 0 (0 means "unknown" in SourceIdentity).
///
/// Reads at most 128 KiB. Results are memoised per (path, size, mtime), so repeated calls for an unchanged
/// file only stat it. Thread-safe. Errors: NotFound, IoError.
std::expected<std::uint64_t, Error> sourceFingerprint(const std::string& utf8Path);

} // namespace wabcut::media
