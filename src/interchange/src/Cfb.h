#pragma once

// Microsoft Compound File Binary format ([MS-CFB], "structured storage"): the container of AAF files. A small, strict
// implementation of what AAF needs:
//
//  * Builder writes a compound file from a tree of storages and streams: version 4 (4096-byte sectors, AAF's "4K"
//    binary) or version 3 (512-byte sectors). Streams under the 4096-byte cutoff go to the mini stream. Each storage's
//    children form a balanced binary tree in the format's name order, all nodes black (the format allows it). The
//    output is deterministic: the same tree gives the same bytes (no timestamps). Large streams (embedded audio) can
//    come from files on disk and are copied in chunks, so writing never holds them in memory.
//  * Reader reads one back (tests, and AAF detection). Its input is untrusted: every sector number, chain, directory
//    entry and size is checked; chains that loop or leave the file, trees that cycle and streams longer than their
//    chains are errors, never crashes.

#include <wabcut/model/Error.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::interchange::detail::cfb {

using Clsid = std::array<std::uint8_t, 16>;

inline constexpr std::uint32_t kFreeSector = 0xFFFFFFFFu;
inline constexpr std::uint32_t kEndOfChain = 0xFFFFFFFEu;
inline constexpr std::uint32_t kFatSector = 0xFFFFFFFDu;
inline constexpr std::uint32_t kDifatSector = 0xFFFFFFFCu;
inline constexpr std::uint32_t kNoStream = 0xFFFFFFFFu;
/// Streams shorter than this live in the mini stream (64-byte mini sectors).
inline constexpr std::uint64_t kMiniStreamCutoff = 4096;
inline constexpr std::size_t kMaxNameUnits = 31; ///< UTF-16 code units of an entry name (without the terminator)

/// A stream's bytes: in memory, or [offset, offset + size) of a file on disk.
struct StreamSource {
    std::string bytes;
    std::filesystem::path file;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;

    static StreamSource of(std::string bytes) {
        StreamSource s;
        s.bytes = std::move(bytes);
        return s;
    }
    static StreamSource ofFile(std::filesystem::path file, std::uint64_t offset, std::uint64_t size) {
        StreamSource s;
        s.file = std::move(file);
        s.offset = offset;
        s.size = size;
        return s;
    }
    bool fromFile() const { return !file.empty(); }
    std::uint64_t length() const { return fromFile() ? size : bytes.size(); }
};

enum class EntryType : std::uint8_t { Empty = 0, Storage = 1, Stream = 2, Root = 5 };

class Builder {
public:
    static constexpr std::size_t kRoot = 0;
    /// Where write() / toBytes() put the bytes.
    class Sink;

    /// 4096 (version 4) or 512 (version 3) byte sectors.
    explicit Builder(std::uint32_t sectorSize = 4096);

    std::uint32_t sectorSize() const { return m_sectorSize; }
    /// The CLSID in the file header (AAF: the file kind) and an entry's CLSID (AAF: the object's class).
    void setHeaderClsid(const Clsid& clsid) { m_headerClsid = clsid; }
    void setClsid(std::size_t entry, const Clsid& clsid);

    /// A child of storage `parent` named `name`: printable ASCII, 1..31 characters, unique among its siblings (ignoring
    /// case). A bad name is recorded (problem()) and the entry is still added, so building can go on; write() then
    /// fails.
    std::size_t addStorage(std::size_t parent, std::string_view name);
    std::size_t addStream(std::size_t parent, std::string_view name, StreamSource data);

    std::size_t entryCount() const { return m_entries.size(); }
    /// The first problem met while building (bad or duplicate name, a parent that is not a storage), if any.
    const std::optional<std::string>& problem() const { return m_problem; }

    /// The file, written to a temporary next to `file` and renamed over it. Errors: InvalidArgument (problem()),
    /// IoError (writing, or reading a file-backed stream that is shorter than its size).
    std::expected<void, Error> write(const std::filesystem::path& file) const;
    /// The file in memory (tests, small files).
    std::expected<std::string, Error> toBytes() const;

private:
    struct Entry {
        std::string name;
        EntryType type = EntryType::Storage;
        Clsid clsid{};
        std::size_t parent = 0;
        std::vector<std::size_t> children;
        StreamSource data;
    };
    struct Layout;

    std::size_t add(std::size_t parent, std::string_view name, EntryType type, StreamSource data);
    std::expected<void, Error> emit(Sink& sink) const;

    std::uint32_t m_sectorSize = 4096;
    Clsid m_headerClsid{};
    std::vector<Entry> m_entries;
    std::optional<std::string> m_problem;
};

/// Bounds of the reader (inputs are untrusted).
inline constexpr std::uint64_t kMaxReadStreamBytes = 512ull * 1024 * 1024;
inline constexpr std::size_t kMaxDirectoryEntries = 4'000'000;

class Reader {
public:
    struct Entry {
        std::string name; ///< UTF-8
        EntryType type = EntryType::Empty;
        Clsid clsid{};
        std::uint32_t startSector = kEndOfChain;
        std::uint64_t size = 0;
        std::vector<std::size_t> children; ///< in tree (name) order
    };

    /// Parses the header, FAT, mini FAT and directory of `bytes` (kept by the reader). Errors: ParseError.
    static std::expected<Reader, Error> open(std::string bytes);

    const Clsid& headerClsid() const { return m_headerClsid; }
    std::uint16_t majorVersion() const { return m_majorVersion; }
    std::uint32_t sectorSize() const { return m_sectorSize; }
    const std::vector<Entry>& entries() const { return m_entries; }
    const Entry& root() const { return m_entries.front(); }
    /// The child of storage `storage` named `name` (case-insensitive, as the format compares names).
    std::optional<std::size_t> child(std::size_t storage, std::string_view name) const;
    /// An entry by path from the root: "/Header-2/Content-3b03"; "/" is the root.
    std::optional<std::size_t> find(std::string_view path) const;
    /// A stream's bytes. Errors: ParseError (a broken chain, not a stream), Unsupported (over kMaxReadStreamBytes).
    std::expected<std::string, Error> read(std::size_t stream) const;

private:
    std::expected<std::vector<std::uint32_t>, Error> chain(const std::vector<std::uint32_t>& table, std::uint32_t start,
                                                           std::uint64_t limit) const;

    std::string m_bytes;
    Clsid m_headerClsid{};
    std::uint16_t m_majorVersion = 0;
    std::uint32_t m_sectorSize = 512;
    std::uint32_t m_miniSectorSize = 64;
    std::uint32_t m_miniCutoff = 4096;
    std::vector<std::uint32_t> m_fat;
    std::vector<std::uint32_t> m_miniFat;
    std::string m_miniStream;
    std::vector<Entry> m_entries;
};

} // namespace wabcut::interchange::detail::cfb
