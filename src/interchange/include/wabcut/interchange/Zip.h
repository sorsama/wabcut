#pragma once

// A bounded ZIP reader (0.2, for Motion Graphics templates, .mogrt - Mogrt.h). ZIP files come from anywhere, so the
// reader trusts nothing: the end-of-central-directory record is searched in the last 64 KiB, the central directory
// must lie inside the file, every entry's local header must match it, names must be safe relative paths
// (isSafeZipPath: no "..", no root or drive, no backslashes or control characters), entries are stored or deflated
// only (no encryption, no ZIP64, no multi-disk archives), counts and sizes are bounded (ZipLimits) - the uncompressed
// size an entry declares is enforced while inflating - and every entry's CRC-32 is checked. Nothing is written to disk.
//
// writeStoredZip makes a small archive (stored, no compression) for tests and fuzz seeds.

#include <wabcut/model/Error.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wabcut::interchange {

struct ZipLimits {
    std::size_t maxEntries = 4096;
    std::uint64_t maxEntryBytes = 16u * 1024u * 1024u;  ///< one entry, uncompressed
    std::uint64_t maxTotalBytes = 64u * 1024u * 1024u;  ///< every entry read, uncompressed
    std::size_t maxArchiveBytes = 256u * 1024u * 1024u; ///< the archive itself
};

struct ZipEntry {
    std::string name;            ///< '/'-separated, safe (isSafeZipPath); folders end in '/'
    std::uint16_t method = 0;    ///< 0 stored, 8 deflated
    std::uint32_t crc = 0;
    std::uint64_t compressedSize = 0;
    std::uint64_t size = 0;      ///< uncompressed
    std::uint64_t localOffset = 0;

    bool isFolder() const { return !name.empty() && name.back() == '/'; }
};

/// Whether a ZIP entry name may be used as a relative path: not empty, no leading '/', no drive ("C:"), no backslash,
/// no "." or ".." segment, no empty segment (except a trailing '/' of a folder), no control characters, at most 1024
/// bytes of UTF-8.
bool isSafeZipPath(std::string_view name);

class ZipArchive {
public:
    /// Reads the directory of `data` (copied). Errors: ParseError (not a ZIP, damaged, unsafe names, encrypted,
    /// ZIP64 or multi-disk, an unsupported method, beyond the limits).
    static std::expected<ZipArchive, Error> open(std::string_view data, ZipLimits limits = {});

    const std::vector<ZipEntry>& entries() const { return m_entries; }
    /// The entry with that name (ASCII case-insensitive), or null.
    const ZipEntry* find(std::string_view name) const;
    /// The entry's bytes: inflated within its declared size and checked against its CRC-32. Errors: ParseError, also
    /// once ZipLimits::maxTotalBytes have been read from this archive.
    std::expected<std::string, Error> read(const ZipEntry& entry) const;

private:
    std::string m_data;
    std::vector<ZipEntry> m_entries;
    ZipLimits m_limits;
    mutable std::uint64_t m_read = 0;
};

/// A ZIP of `files` (name, bytes), stored without compression; names are used as given.
std::string writeStoredZip(std::span<const std::pair<std::string, std::string>> files);

} // namespace wabcut::interchange
