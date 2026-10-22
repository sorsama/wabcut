// A bounded ZIP reader (Zip.h).

#include <wabcut/interchange/Zip.h>

#include <wabcut/model/TitleOps.h>

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <limits>

namespace wabcut::interchange {

namespace {

constexpr std::uint32_t kLocalHeader = 0x04034b50;
constexpr std::uint32_t kCentralHeader = 0x02014b50;
constexpr std::uint32_t kEndOfDirectory = 0x06054b50;
constexpr std::size_t kEndRecordSize = 22;
constexpr std::size_t kCentralSize = 46;
constexpr std::size_t kLocalSize = 30;

Error damaged(const std::string& what) { return Error{ErrorCode::ParseError, "ZIP: " + what}; }

std::uint16_t u16(std::string_view d, std::size_t at) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(d[at]) | (static_cast<unsigned char>(d[at + 1]) << 8));
}

std::uint32_t u32(std::string_view d, std::size_t at) {
    return static_cast<std::uint32_t>(u16(d, at)) | (static_cast<std::uint32_t>(u16(d, at + 2)) << 16);
}

void put16(std::string& out, std::uint16_t v) {
    out.push_back(static_cast<char>(v & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
}

void put32(std::string& out, std::uint32_t v) {
    put16(out, static_cast<std::uint16_t>(v & 0xffff));
    put16(out, static_cast<std::uint16_t>(v >> 16));
}

bool sameAscii(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

std::uint32_t crcOf(std::string_view data) {
    uLong crc = crc32(0L, Z_NULL, 0);
    std::size_t at = 0;
    while (at < data.size()) {
        const auto n = static_cast<uInt>(std::min<std::size_t>(data.size() - at, std::numeric_limits<uInt>::max()));
        crc = crc32(crc, reinterpret_cast<const Bytef*>(data.data() + at), n);
        at += n;
    }
    return static_cast<std::uint32_t>(crc);
}

} // namespace

bool isSafeZipPath(std::string_view name) {
    if (name.empty() || name.size() > 1024 || name.front() == '/' || !edit::isValidUtf8(name)) {
        return false;
    }
    if (name.size() >= 2 && name[1] == ':') {
        return false; // a drive
    }
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '\\' || u < 0x20 || u == 0x7f) {
            return false;
        }
    }
    std::size_t start = 0;
    while (start < name.size()) {
        const std::size_t slash = name.find('/', start);
        const std::size_t end = slash == std::string_view::npos ? name.size() : slash;
        const std::string_view segment = name.substr(start, end - start);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        if (slash == std::string_view::npos || slash + 1 == name.size()) {
            break; // the last segment, or a folder's trailing '/'
        }
        start = slash + 1;
    }
    return true;
}

std::expected<ZipArchive, Error> ZipArchive::open(std::string_view data, ZipLimits limits) {
    if (data.size() > limits.maxArchiveBytes) {
        return std::unexpected(damaged("the archive is larger than " + std::to_string(limits.maxArchiveBytes) + " bytes"));
    }
    if (data.size() < kEndRecordSize) {
        return std::unexpected(damaged("not a ZIP archive"));
    }
    // The end-of-central-directory record: its comment is at most 65535 bytes.
    std::size_t end = std::string_view::npos;
    const std::size_t lowest = data.size() > kEndRecordSize + 0xffff ? data.size() - kEndRecordSize - 0xffff : 0;
    for (std::size_t at = data.size() - kEndRecordSize + 1; at-- > lowest;) {
        if (u32(data, at) == kEndOfDirectory && at + kEndRecordSize + u16(data, at + 20) == data.size()) {
            end = at;
            break;
        }
    }
    if (end == std::string_view::npos) {
        return std::unexpected(damaged("no end of central directory: not a ZIP archive, or damaged"));
    }
    const std::uint16_t disk = u16(data, end + 4);
    const std::uint16_t dirDisk = u16(data, end + 6);
    const std::uint16_t onDisk = u16(data, end + 8);
    const std::uint16_t count = u16(data, end + 10);
    const std::uint32_t dirSize = u32(data, end + 12);
    const std::uint32_t dirOffset = u32(data, end + 16);
    if (disk != 0 || dirDisk != 0 || onDisk != count) {
        return std::unexpected(damaged("multi-disk archives are not supported"));
    }
    if (count == 0xffff || dirSize == 0xffffffffu || dirOffset == 0xffffffffu) {
        return std::unexpected(damaged("ZIP64 archives are not supported"));
    }
    if (count > limits.maxEntries) {
        return std::unexpected(damaged("more than " + std::to_string(limits.maxEntries) + " entries"));
    }
    if (static_cast<std::uint64_t>(dirOffset) + dirSize > end) {
        return std::unexpected(damaged("the central directory lies outside the archive"));
    }
    ZipArchive archive;
    archive.m_limits = limits;
    std::size_t at = dirOffset;
    const std::size_t dirEnd = static_cast<std::size_t>(dirOffset) + dirSize;
    for (std::uint16_t i = 0; i < count; ++i) {
        if (at + kCentralSize > dirEnd || u32(data, at) != kCentralHeader) {
            return std::unexpected(damaged("a damaged central directory entry"));
        }
        const std::uint16_t flags = u16(data, at + 8);
        ZipEntry e;
        e.method = u16(data, at + 10);
        e.crc = u32(data, at + 16);
        e.compressedSize = u32(data, at + 20);
        e.size = u32(data, at + 24);
        const std::uint16_t nameLength = u16(data, at + 28);
        const std::uint16_t extraLength = u16(data, at + 30);
        const std::uint16_t commentLength = u16(data, at + 32);
        const std::uint16_t entryDisk = u16(data, at + 34);
        e.localOffset = u32(data, at + 42);
        const std::size_t next = at + kCentralSize + nameLength + extraLength + commentLength;
        if (next > dirEnd) {
            return std::unexpected(damaged("a damaged central directory entry"));
        }
        e.name = std::string(data.substr(at + kCentralSize, nameLength));
        if ((flags & 0x1) != 0) {
            return std::unexpected(damaged("'" + e.name + "' is encrypted"));
        }
        if (entryDisk != 0 || e.compressedSize == 0xffffffffu || e.size == 0xffffffffu || e.localOffset == 0xffffffffu) {
            return std::unexpected(damaged("ZIP64 and multi-disk entries are not supported"));
        }
        if (!isSafeZipPath(e.name)) {
            return std::unexpected(damaged("an entry has an unsafe name (an absolute path, \"..\" or a drive)"));
        }
        if (e.method != 0 && e.method != 8) {
            return std::unexpected(damaged("'" + e.name + "' uses an unsupported compression method"));
        }
        if (e.size > limits.maxEntryBytes) {
            return std::unexpected(damaged("'" + e.name + "' is larger than " + std::to_string(limits.maxEntryBytes) + " bytes"));
        }
        if (e.method == 0 && e.compressedSize != e.size) {
            return std::unexpected(damaged("'" + e.name + "' is stored with two sizes"));
        }
        // The local header must agree and the data must lie before the central directory.
        const std::uint64_t local = e.localOffset;
        if (local + kLocalSize > dirOffset || u32(data, static_cast<std::size_t>(local)) != kLocalHeader) {
            return std::unexpected(damaged("'" + e.name + "' has no local header"));
        }
        const auto localAt = static_cast<std::size_t>(local);
        const std::uint16_t localName = u16(data, localAt + 26);
        const std::uint16_t localExtra = u16(data, localAt + 28);
        if (data.substr(localAt + kLocalSize, std::min<std::size_t>(localName, data.size() - localAt - kLocalSize)) != e.name ||
            u16(data, localAt + 8) != e.method) {
            return std::unexpected(damaged("'" + e.name + "' does not match its local header"));
        }
        const std::uint64_t start = local + kLocalSize + localName + localExtra;
        if (start + e.compressedSize > dirOffset) {
            return std::unexpected(damaged("'" + e.name + "' runs past its archive"));
        }
        e.localOffset = start; // from here on: where its data starts
        for (const ZipEntry& other : archive.m_entries) {
            if (sameAscii(other.name, e.name)) {
                return std::unexpected(damaged("'" + e.name + "' is in the archive twice"));
            }
        }
        archive.m_entries.push_back(std::move(e));
        at = next;
    }
    archive.m_data = std::string(data);
    return archive;
}

const ZipEntry* ZipArchive::find(std::string_view name) const {
    const auto it = std::find_if(m_entries.begin(), m_entries.end(), [&](const ZipEntry& e) { return sameAscii(e.name, name); });
    return it == m_entries.end() ? nullptr : &*it;
}

std::expected<std::string, Error> ZipArchive::read(const ZipEntry& entry) const {
    if (m_read + entry.size > m_limits.maxTotalBytes) {
        return std::unexpected(damaged("reading more than " + std::to_string(m_limits.maxTotalBytes) + " bytes from one archive"));
    }
    const std::string_view packed = std::string_view(m_data).substr(static_cast<std::size_t>(entry.localOffset),
                                                                     static_cast<std::size_t>(entry.compressedSize));
    std::string out;
    if (entry.method == 0 || entry.size == 0) {
        out = entry.method == 0 ? std::string(packed) : std::string();
    } else {
        z_stream stream{};
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) { // raw deflate
            return std::unexpected(Error{ErrorCode::Unknown, "zlib initialisation failed"});
        }
        out.resize(static_cast<std::size_t>(entry.size));
        stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(packed.data()));
        stream.avail_in = static_cast<uInt>(packed.size());
        stream.next_out = reinterpret_cast<Bytef*>(out.data());
        stream.avail_out = static_cast<uInt>(out.size());
        int status = inflate(&stream, Z_FINISH);
        // More output than declared: refused (a zip bomb, or a lie), not grown.
        if (status == Z_BUF_ERROR && stream.avail_out == 0 && stream.avail_in > 0) {
            inflateEnd(&stream);
            return std::unexpected(damaged("'" + entry.name + "' inflates past its declared size"));
        }
        const uLong produced = stream.total_out;
        inflateEnd(&stream);
        if (status != Z_STREAM_END || produced != entry.size) {
            return std::unexpected(damaged("'" + entry.name + "' is damaged"));
        }
    }
    if (out.size() != entry.size || crcOf(out) != entry.crc) {
        return std::unexpected(damaged("'" + entry.name + "' fails its CRC check"));
    }
    m_read += entry.size;
    return out;
}

std::string writeStoredZip(std::span<const std::pair<std::string, std::string>> files) {
    std::string out;
    std::string directory;
    for (const auto& [name, bytes] : files) {
        const std::uint32_t crc = crcOf(bytes);
        const auto offset = static_cast<std::uint32_t>(out.size());
        put32(out, kLocalHeader);
        put16(out, 20); // version needed
        put16(out, 0);  // flags
        put16(out, 0);  // stored
        put16(out, 0);  // time
        put16(out, 0x21); // date (1980-01-01)
        put32(out, crc);
        put32(out, static_cast<std::uint32_t>(bytes.size()));
        put32(out, static_cast<std::uint32_t>(bytes.size()));
        put16(out, static_cast<std::uint16_t>(name.size()));
        put16(out, 0);
        out += name;
        out += bytes;
        put32(directory, kCentralHeader);
        put16(directory, 20); // made by
        put16(directory, 20); // needed
        put16(directory, 0);
        put16(directory, 0);
        put16(directory, 0);
        put16(directory, 0x21);
        put32(directory, crc);
        put32(directory, static_cast<std::uint32_t>(bytes.size()));
        put32(directory, static_cast<std::uint32_t>(bytes.size()));
        put16(directory, static_cast<std::uint16_t>(name.size()));
        put16(directory, 0); // extra
        put16(directory, 0); // comment
        put16(directory, 0); // disk
        put16(directory, 0); // internal attributes
        put32(directory, 0); // external attributes
        put32(directory, offset);
        directory += name;
    }
    const auto dirOffset = static_cast<std::uint32_t>(out.size());
    out += directory;
    put32(out, kEndOfDirectory);
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<std::uint16_t>(files.size()));
    put16(out, static_cast<std::uint16_t>(files.size()));
    put32(out, static_cast<std::uint32_t>(directory.size()));
    put32(out, dirOffset);
    put16(out, 0);
    return out;
}

} // namespace wabcut::interchange
