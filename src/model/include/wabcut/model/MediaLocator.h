#pragma once

// Where a media item's file is, how to recognise it again, and its proxy.
//
// A locator keeps both the absolute path and the path relative to the project file, so a project moved
// together with its media (another drive, another machine) relinks by itself. The fingerprint tells
// whether a file found at either path is still the same source; caches (thumbnails, peaks, proxies) key
// on it instead of the modification time alone.

#include <wabcut/model/Error.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>

namespace wabcut {

/// Identity of a source file.
struct SourceFingerprint {
    /// File size in bytes; -1 when unknown.
    std::int64_t size = -1;
    /// Last write time in nanoseconds since the Unix epoch; 0 when unknown.
    std::int64_t mtimeNs = 0;
    /// FNV-1a 64 of the first and last 64 KiB (the whole file when it is at most 128 KiB), as 16
    /// lowercase hex digits; empty when unknown.
    std::string sampleHash;

    bool isKnown() const { return size >= 0 && !sampleHash.empty(); }
    /// Same content as far as the fingerprint can tell (size and sample hash; mtime may differ after a copy).
    bool sameContentAs(const SourceFingerprint& o) const { return isKnown() && size == o.size && sampleHash == o.sampleHash; }
    bool operator==(const SourceFingerprint&) const = default;
};

/// How a proxy was made, so a stale proxy (source changed, settings changed) can be detected.
struct ProxyProvenance {
    std::string generator;     ///< e.g. "wabcut-proxy 1"
    std::string settings;      ///< generator-defined description (codec, size, ...)
    SourceFingerprint source;  ///< the source the proxy was made from
    std::uint64_t generation = 0; ///< bumped each time the proxy is regenerated

    bool operator==(const ProxyProvenance&) const = default;
};

struct ProxyMedia {
    std::string path; ///< UTF-8, absolute
    ProxyProvenance provenance;

    bool operator==(const ProxyMedia&) const = default;
};

struct MediaLocator {
    /// Absolute path (UTF-8).
    std::string path;
    /// Path relative to the project file's directory (UTF-8, '/' separators); empty when there is none
    /// (e.g. another drive).
    std::string relativePath;
    SourceFingerprint fingerprint;
    /// Set when the file could not be found or no longer matches its fingerprint.
    bool offline = false;
    std::optional<ProxyMedia> proxy;

    bool operator==(const MediaLocator&) const = default;
};

/// Bytes hashed at each end of a file for SourceFingerprint::sampleHash.
inline constexpr std::size_t kFingerprintSampleBytes = 64 * 1024;

/// Reads size, mtime and the sample hash of a file.
std::expected<SourceFingerprint, Error> fingerprintFile(const std::filesystem::path& file);

/// `media` relative to the directory of `projectFile`, UTF-8 with '/' separators; empty when no
/// relative path exists.
std::string relativeMediaPath(const std::filesystem::path& projectFile, const std::filesystem::path& media);

/// Finds a locator's file: the absolute path if it exists, else the relative path resolved against the
/// project file's directory, else nullopt.
std::optional<std::filesystem::path> resolveMediaPath(const MediaLocator& locator,
                                                      const std::filesystem::path& projectFile);

/// UTF-8 text of a path, and a path from UTF-8 text.
std::string pathToUtf8(const std::filesystem::path& path);
std::filesystem::path pathFromUtf8(const std::string& utf8);

} // namespace wabcut
