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
#include <functional>
#include <memory>
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
    /// 0.6 (AAF import): the same for a file read on demand: the header, FATs and directory now, streams when read() or
    /// copy() asks, so a file of any size (embedded media) never has to fit in memory. Errors: IoError (cannot open or
    /// read it), ParseError.
    static std::expected<Reader, Error> openFile(const std::filesystem::path& file);

    const Clsid& headerClsid() const { return m_headerClsid; }
    std::uint16_t majorVersion() const { return m_majorVersion; }
    std::uint32_t sectorSize() const { return m_sectorSize; }
    /// Size of the whole compound file.
    std::uint64_t fileSize() const { return m_fileSize; }
    const std::vector<Entry>& entries() const { return m_entries; }
    const Entry& root() const { return m_entries.front(); }
    /// The child of storage `storage` named `name` (case-insensitive, as the format compares names).
    std::optional<std::size_t> child(std::size_t storage, std::string_view name) const;
    /// An entry by path from the root: "/Header-2/Content-3b03"; "/" is the root.
    std::optional<std::size_t> find(std::string_view path) const;
    /// A stream's bytes. Errors: ParseError (a broken chain, not a stream, or streams that share sectors so that
    /// reading them would read the file many times over), Unsupported (over kMaxReadStreamBytes), IoError.
    std::expected<std::string, Error> read(std::size_t stream) const;
    /// 0.6: a stream of any size, handed to `sink` in blocks of at most 1 MiB, in order. `sink` returns false to stop
    /// (Cancelled). Errors as read(), without the size limit.
    std::expected<void, Error> copy(std::size_t stream, const std::function<bool(std::string_view)>& sink) const;

private:
    class Source;
    static std::expected<Reader, Error> parse(Reader r);
    /// The sectors of a chain from `start`: at most `maxCount` of them (a stream needs no more than its size), each
    /// below `limit`. Chains that loop or leave the table are errors.
    std::expected<std::vector<std::uint32_t>, Error> chain(const std::vector<std::uint32_t>& table, std::uint32_t start,
                                                           std::uint64_t limit, std::uint64_t maxCount) const;
    /// Appends `size` bytes from `offset` of the file (zeros past its end, as a short last sector reads).
    std::expected<void, Error> fetch(std::uint64_t offset, std::size_t size, std::string& out) const;
    /// Counts bytes read from streams against the budget (a few times the file size).
    std::expected<void, Error> charge(std::uint64_t bytes) const;

    std::shared_ptr<Source> m_source;
    std::uint64_t m_fileSize = 0;
    mutable std::uint64_t m_bytesRead = 0;
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
