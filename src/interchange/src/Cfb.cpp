// Compound File Binary writer and reader (Cfb.h), after [MS-CFB] 2.2-2.6.

#include "Cfb.h"

#include "TextUtil.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <system_error>

namespace wabcut::interchange::detail::cfb {

namespace {

constexpr std::uint8_t kMagic[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
constexpr std::uint32_t kMiniSectorSize = 64;
constexpr std::size_t kHeaderDifatEntries = 109;
constexpr std::size_t kDirEntrySize = 128;
/// Highest regular sector number ([MS-CFB] MAXREGSECT).
constexpr std::uint64_t kMaxRegularSector = 0xFFFFFFFAu;

void putU16(std::string& out, std::size_t at, std::uint16_t v) {
    out[at] = static_cast<char>(v & 0xFF);
    out[at + 1] = static_cast<char>(v >> 8);
}
void putU32(std::string& out, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out[at + static_cast<std::size_t>(i)] = static_cast<char>((v >> (8 * i)) & 0xFF);
    }
}
void putU64(std::string& out, std::size_t at, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out[at + static_cast<std::size_t>(i)] = static_cast<char>((v >> (8 * i)) & 0xFF);
    }
}
std::uint16_t getU16(std::string_view in, std::size_t at) {
    return static_cast<std::uint16_t>(static_cast<std::uint8_t>(in[at]) | (static_cast<std::uint8_t>(in[at + 1]) << 8));
}
std::uint32_t getU32(std::string_view in, std::size_t at) {
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) {
        v = (v << 8) | static_cast<std::uint8_t>(in[at + static_cast<std::size_t>(i)]);
    }
    return v;
}
std::uint64_t getU64(std::string_view in, std::size_t at) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | static_cast<std::uint8_t>(in[at + static_cast<std::size_t>(i)]);
    }
    return v;
}

std::uint64_t ceilDiv(std::uint64_t a, std::uint64_t b) { return (a + b - 1) / b; }

char upperAscii(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }

/// [MS-CFB] 2.6.4 order: shorter names first, then by upper-case code unit (ASCII names here).
bool nameLess(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return a.size() < b.size();
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char x = upperAscii(a[i]);
        const char y = upperAscii(b[i]);
        if (x != y) {
            return static_cast<unsigned char>(x) < static_cast<unsigned char>(y);
        }
    }
    return false;
}

bool nameEqualNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (upperAscii(a[i]) != upperAscii(b[i])) {
            return false;
        }
    }
    return true;
}

Error parseFailure(std::string message) { return Error{ErrorCode::ParseError, "compound file: " + std::move(message)}; }

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Builder

class Builder::Sink {
public:
    virtual ~Sink() = default;
    virtual bool put(const char* data, std::size_t size) = 0;
    bool put(std::string_view data) { return put(data.data(), data.size()); }
    bool zeros(std::uint64_t count) {
        static const char kZeros[4096] = {};
        while (count > 0) {
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(count, sizeof kZeros));
            if (!put(kZeros, n)) {
                return false;
            }
            count -= n;
        }
        return true;
    }
};

namespace {

class StringSink final : public Builder::Sink {
public:
    explicit StringSink(std::string& out) : m_out(out) {}
    bool put(const char* data, std::size_t size) override {
        m_out.append(data, size);
        return true;
    }

private:
    std::string& m_out;
};

class FileSink final : public Builder::Sink {
public:
    explicit FileSink(std::ofstream& out) : m_out(out) {}
    bool put(const char* data, std::size_t size) override {
        m_out.write(data, static_cast<std::streamsize>(size));
        return static_cast<bool>(m_out);
    }

private:
    std::ofstream& m_out;
};

} // namespace

struct Builder::Layout {
    std::vector<std::uint32_t> start; // per entry: first (mini) sector, or kEndOfChain
    std::vector<bool> mini;
    std::vector<std::uint32_t> left;
    std::vector<std::uint32_t> right;
    std::vector<std::uint32_t> child;
    std::uint32_t bigSectors = 0;
    std::uint32_t miniSectors = 0;
    std::uint32_t miniStreamSectors = 0;
    std::uint32_t miniFatSectors = 0;
    std::uint32_t dirSectors = 0;
    std::uint32_t fatSectors = 0;
    std::uint32_t difatSectors = 0;
    std::uint32_t miniStreamStart = 0;
    std::uint32_t miniFatStart = 0;
    std::uint32_t dirStart = 0;
    std::uint32_t fatStart = 0;
    std::uint32_t difatStart = 0;
    std::uint32_t total = 0;
};

Builder::Builder(std::uint32_t sectorSize) : m_sectorSize(sectorSize == 512 ? 512u : 4096u) {
    Entry root;
    root.name = "Root Entry";
    root.type = EntryType::Root;
    m_entries.push_back(std::move(root));
}

void Builder::setClsid(std::size_t entry, const Clsid& clsid) {
    if (entry < m_entries.size()) {
        m_entries[entry].clsid = clsid;
    }
}

std::size_t Builder::addStorage(std::size_t parent, std::string_view name) {
    return add(parent, name, EntryType::Storage, {});
}

std::size_t Builder::addStream(std::size_t parent, std::string_view name, StreamSource data) {
    return add(parent, name, EntryType::Stream, std::move(data));
}

std::size_t Builder::add(std::size_t parent, std::string_view name, EntryType type, StreamSource data) {
    auto fail = [&](std::string message) {
        if (!m_problem) {
            m_problem = std::move(message);
        }
    };
    if (name.empty() || name.size() > kMaxNameUnits) {
        fail("entry name \"" + std::string(name) + "\" is empty or longer than 31 characters");
    }
    for (const char c : name) {
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E || c == '/' || c == '\\' ||
            c == ':' || c == '!') {
            fail("entry name \"" + std::string(name) + "\" has a character compound files do not allow");
            break;
        }
    }
    if (parent >= m_entries.size() || m_entries[parent].type == EntryType::Stream) {
        fail("parent of \"" + std::string(name) + "\" is not a storage");
        parent = kRoot;
    }
    for (const std::size_t sibling : m_entries[parent].children) {
        if (nameEqualNoCase(m_entries[sibling].name, name)) {
            fail("two entries named \"" + std::string(name) + "\" in one storage");
            break;
        }
    }
    Entry e;
    e.name = std::string(name);
    e.type = type;
    e.parent = parent;
    e.data = std::move(data);
    const std::size_t index = m_entries.size();
    m_entries.push_back(std::move(e));
    m_entries[parent].children.push_back(index);
    return index;
}

std::expected<void, Error> Builder::emit(Sink& sink) const {
    if (m_problem) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "compound file: " + *m_problem});
    }
    const std::uint64_t ss = m_sectorSize;
    const std::size_t n = m_entries.size();
    Layout L;
    L.start.assign(n, kEndOfChain);
    L.mini.assign(n, false);
    L.left.assign(n, kNoStream);
    L.right.assign(n, kNoStream);
    L.child.assign(n, kNoStream);

    // Sectors of the big streams, then mini sectors of the small ones, in entry order.
    std::uint64_t big = 0;
    std::uint64_t mini = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const Entry& e = m_entries[i];
        if (e.type != EntryType::Stream) {
            continue;
        }
        const std::uint64_t len = e.data.length();
        if (m_sectorSize == 512 && len > 0xFFFFFFFFull) {
            return std::unexpected(Error{ErrorCode::Unsupported, "compound file: stream over 4 GB in a version 3 file"});
        }
        if (len >= kMiniStreamCutoff) {
            L.start[i] = static_cast<std::uint32_t>(big);
            big += ceilDiv(len, ss);
        } else if (len > 0) {
            L.mini[i] = true;
            L.start[i] = static_cast<std::uint32_t>(mini);
            mini += ceilDiv(len, kMiniSectorSize);
        }
        if (big > kMaxRegularSector || mini > kMaxRegularSector) {
            return std::unexpected(Error{ErrorCode::Unsupported, "compound file: too large"});
        }
    }
    L.bigSectors = static_cast<std::uint32_t>(big);
    L.miniSectors = static_cast<std::uint32_t>(mini);
    L.miniStreamSectors = static_cast<std::uint32_t>(ceilDiv(mini * kMiniSectorSize, ss));
    L.miniFatSectors = static_cast<std::uint32_t>(ceilDiv(mini * 4, ss));
    L.dirSectors = static_cast<std::uint32_t>(ceilDiv(n * kDirEntrySize, ss));
    const std::uint64_t base =
        std::uint64_t{L.bigSectors} + L.miniStreamSectors + L.miniFatSectors + L.dirSectors;
    const std::uint64_t perFat = ss / 4;
    std::uint64_t fat = 0;
    std::uint64_t difat = 0;
    for (int round = 0; round < 64; ++round) {
        const std::uint64_t fat2 = ceilDiv(base + fat + difat, perFat);
        const std::uint64_t difat2 = fat2 > kHeaderDifatEntries ? ceilDiv(fat2 - kHeaderDifatEntries, perFat - 1) : 0;
        if (fat2 == fat && difat2 == difat) {
            break;
        }
        fat = fat2;
        difat = difat2;
    }
    if (base + fat + difat > kMaxRegularSector) {
        return std::unexpected(Error{ErrorCode::Unsupported, "compound file: too large"});
    }
    L.fatSectors = static_cast<std::uint32_t>(fat);
    L.difatSectors = static_cast<std::uint32_t>(difat);
    L.miniStreamStart = L.bigSectors;
    L.miniFatStart = L.miniStreamStart + L.miniStreamSectors;
    L.dirStart = L.miniFatStart + L.miniFatSectors;
    L.fatStart = L.dirStart + L.dirSectors;
    L.difatStart = L.fatStart + L.fatSectors;
    L.total = L.difatStart + L.difatSectors;

    // Each storage's children: a balanced binary search tree in name order.
    for (std::size_t i = 0; i < n; ++i) {
        const Entry& e = m_entries[i];
        if (e.children.empty()) {
            continue;
        }
        std::vector<std::size_t> sorted = e.children;
        std::sort(sorted.begin(), sorted.end(),
                  [&](std::size_t a, std::size_t b) { return nameLess(m_entries[a].name, m_entries[b].name); });
        std::function<std::uint32_t(std::size_t, std::size_t)> build = [&](std::size_t lo, std::size_t hi) {
            if (lo >= hi) {
                return kNoStream;
            }
            const std::size_t mid = lo + (hi - lo) / 2;
            const std::size_t node = sorted[mid];
            L.left[node] = build(lo, mid);
            L.right[node] = build(mid + 1, hi);
            return static_cast<std::uint32_t>(node);
        };
        L.child[i] = build(0, sorted.size());
    }

    // --- header ---------------------------------------------------------------------------------------------------
    std::string header(static_cast<std::size_t>(ss), '\0');
    std::memcpy(header.data(), kMagic, sizeof kMagic);
    std::memcpy(header.data() + 8, m_headerClsid.data(), 16);
    putU16(header, 24, 0x003E);
    putU16(header, 26, m_sectorSize == 512 ? 3 : 4);
    putU16(header, 28, 0xFFFE);
    putU16(header, 30, m_sectorSize == 512 ? 9 : 12);
    putU16(header, 32, 6);
    putU32(header, 40, m_sectorSize == 512 ? 0 : L.dirSectors);
    putU32(header, 44, L.fatSectors);
    putU32(header, 48, L.dirStart);
    putU32(header, 52, 0);
    putU32(header, 56, static_cast<std::uint32_t>(kMiniStreamCutoff));
    putU32(header, 60, L.miniFatSectors > 0 ? L.miniFatStart : kEndOfChain);
    putU32(header, 64, L.miniFatSectors);
    putU32(header, 68, L.difatSectors > 0 ? L.difatStart : kEndOfChain);
    putU32(header, 72, L.difatSectors);
    for (std::size_t i = 0; i < kHeaderDifatEntries; ++i) {
        putU32(header, 76 + 4 * i, i < L.fatSectors ? L.fatStart + static_cast<std::uint32_t>(i) : kFreeSector);
    }
    if (!sink.put(header)) {
        return std::unexpected(Error{ErrorCode::IoError, "compound file: cannot write"});
    }

    auto padTo = [&](std::uint64_t written, std::uint64_t unit) {
        const std::uint64_t rest = written % unit;
        return rest == 0 ? true : sink.zeros(unit - rest);
    };

    // --- big streams ----------------------------------------------------------------------------------------------
    std::vector<char> buffer;
    for (std::size_t i = 0; i < n; ++i) {
        const Entry& e = m_entries[i];
        if (e.type != EntryType::Stream || L.mini[i] || e.data.length() == 0) {
            continue;
        }
        if (!e.data.fromFile()) {
            if (!sink.put(e.data.bytes) || !padTo(e.data.bytes.size(), ss)) {
                return std::unexpected(Error{ErrorCode::IoError, "compound file: cannot write"});
            }
            continue;
        }
        std::ifstream in(e.data.file, std::ios::binary);
        if (!in) {
            return std::unexpected(Error{ErrorCode::IoError, "compound file: cannot read " + utf8FromPath(e.data.file)});
        }
        in.seekg(static_cast<std::streamoff>(e.data.offset));
        buffer.resize(1 << 20);
        std::uint64_t left = e.data.size;
        while (left > 0) {
            const std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(left, buffer.size()));
            in.read(buffer.data(), static_cast<std::streamsize>(chunk));
            if (static_cast<std::size_t>(in.gcount()) != chunk) {
                return std::unexpected(
                    Error{ErrorCode::IoError, "compound file: " + utf8FromPath(e.data.file) + " is shorter than expected"});
            }
            if (!sink.put(buffer.data(), chunk)) {
                return std::unexpected(Error{ErrorCode::IoError, "compound file: cannot write"});
            }
            left -= chunk;
        }
        if (!padTo(e.data.size, ss)) {
            return std::unexpected(Error{ErrorCode::IoError, "compound file: cannot write"});
        }
    }

    // --- the mini stream ------------------------------------------------------------------------------------------
    std::uint64_t miniBytes = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!L.mini[i]) {
            continue;
        }
        const StreamSource& data = m_entries[i].data;
        std::string loaded;
        if (data.fromFile()) { // a small file (a very short clip's audio): read it whole
            std::ifstream in(data.file, std::ios::binary);
            loaded.resize(static_cast<std::size_t>(data.size));
            if (in) {
                in.seekg(static_cast<std::streamoff>(data.offset));
                in.read(loaded.data(), static_cast<std::streamsize>(loaded.size()));
            }
            if (!in || static_cast<std::size_t>(in.gcount()) != loaded.size()) {
                return std::unexpected(
                    Error{ErrorCode::IoError, "compound file: " + utf8FromPath(data.file) + " is shorter than expected"});
            }
        }
        const std::string& bytes = data.fromFile() ? loaded : data.bytes;
        if (!sink.put(bytes) || !padTo(bytes.size(), kMiniSectorSize)) {
            return std::unexpected(Error{ErrorCode::IoError, "compound file: cannot write"});
        }
        miniBytes += ceilDiv(bytes.size(), kMiniSectorSize) * kMiniSectorSize;
    }
    if (!padTo(miniBytes, ss)) {
        return std::unexpected(Error{ErrorCode::IoError, "compound file: cannot write"});
    }

    // --- the mini FAT ---------------------------------------------------------------------------------------------
    if (L.miniFatSectors > 0) {
        std::string miniFat(static_cast<std::size_t>(L.miniFatSectors * ss), '\xFF');
        for (std::size_t i = 0; i < n; ++i) {
            if (!L.mini[i]) {
                continue;
            }
            const std::uint32_t count = static_cast<std::uint32_t>(ceilDiv(m_entries[i].data.length(), kMiniSectorSize));
            for (std::uint32_t k = 0; k < count; ++k) {
                putU32(miniFat, 4 * static_cast<std::size_t>(L.start[i] + k), k + 1 < count ? L.start[i] + k + 1 : kEndOfChain);
            }
        }
        if (!sink.put(miniFat)) {
            return std::unexpected(Error{ErrorCode::IoError, "compound file: cannot write"});
        }
    }

    // --- the directory --------------------------------------------------------------------------------------------
    std::string dir(static_cast<std::size_t>(L.dirSectors * ss), '\0');
    for (std::size_t slot = 0; slot < static_cast<std::size_t>(L.dirSectors * ss / kDirEntrySize); ++slot) {
        const std::size_t at = slot * kDirEntrySize;
        if (slot >= n) {
            putU32(dir, at + 68, kNoStream);
            putU32(dir, at + 72, kNoStream);
            putU32(dir, at + 76, kNoStream);
            continue;
        }
        const Entry& e = m_entries[slot];
        for (std::size_t c = 0; c < e.name.size(); ++c) {
            putU16(dir, at + 2 * c, static_cast<std::uint16_t>(static_cast<unsigned char>(e.name[c])));
        }
        putU16(dir, at + 64, static_cast<std::uint16_t>(2 * (e.name.size() + 1)));
        dir[at + 66] = static_cast<char>(e.type);
        dir[at + 67] = 1; // black
        putU32(dir, at + 68, L.left[slot]);
        putU32(dir, at + 72, L.right[slot]);
        putU32(dir, at + 76, L.child[slot]);
        std::memcpy(dir.data() + at + 80, e.clsid.data(), 16);
        if (e.type == EntryType::Root) {
            putU32(dir, at + 116, L.miniSectors > 0 ? L.miniStreamStart : kEndOfChain);
            putU64(dir, at + 120, miniBytes);
        } else if (e.type == EntryType::Stream) {
            putU32(dir, at + 116, e.data.length() == 0 ? kEndOfChain : L.start[slot]);
            putU64(dir, at + 120, e.data.length());
        }
    }
    if (!sink.put(dir)) {
        return std::unexpected(Error{ErrorCode::IoError, "compound file: cannot write"});
    }

    // --- the FAT --------------------------------------------------------------------------------------------------
    std::string fatBytes(static_cast<std::size_t>(L.fatSectors * ss), '\xFF');
    auto chainRun = [&](std::uint32_t first, std::uint32_t count) {
        for (std::uint32_t k = 0; k < count; ++k) {
            putU32(fatBytes, 4 * static_cast<std::size_t>(first + k), k + 1 < count ? first + k + 1 : kEndOfChain);
        }
    };
    for (std::size_t i = 0; i < n; ++i) {
        const Entry& e = m_entries[i];
        if (e.type == EntryType::Stream && !L.mini[i] && e.data.length() > 0) {
            chainRun(L.start[i], static_cast<std::uint32_t>(ceilDiv(e.data.length(), ss)));
        }
    }
    chainRun(L.miniStreamStart, L.miniStreamSectors);
    chainRun(L.miniFatStart, L.miniFatSectors);
    chainRun(L.dirStart, L.dirSectors);
    for (std::uint32_t k = 0; k < L.fatSectors; ++k) {
        putU32(fatBytes, 4 * static_cast<std::size_t>(L.fatStart + k), kFatSector);
    }
    for (std::uint32_t k = 0; k < L.difatSectors; ++k) {
        putU32(fatBytes, 4 * static_cast<std::size_t>(L.difatStart + k), kDifatSector);
    }
    if (!sink.put(fatBytes)) {
        return std::unexpected(Error{ErrorCode::IoError, "compound file: cannot write"});
    }

    // --- the DIFAT beyond the header's 109 entries -----------------------------------------------------------------
    if (L.difatSectors > 0) {
        std::string difatBytes(static_cast<std::size_t>(L.difatSectors * ss), '\xFF');
        std::size_t next = kHeaderDifatEntries;
        for (std::uint32_t d = 0; d < L.difatSectors; ++d) {
            const std::size_t base0 = static_cast<std::size_t>(d * ss);
            for (std::size_t k = 0; k + 1 < perFat; ++k) {
                if (next < L.fatSectors) {
                    putU32(difatBytes, base0 + 4 * k, L.fatStart + static_cast<std::uint32_t>(next));
                    ++next;
                }
            }
            putU32(difatBytes, base0 + 4 * static_cast<std::size_t>(perFat - 1),
                   d + 1 < L.difatSectors ? L.difatStart + d + 1 : kEndOfChain);
        }
        if (!sink.put(difatBytes)) {
            return std::unexpected(Error{ErrorCode::IoError, "compound file: cannot write"});
        }
    }
    return {};
}

std::expected<std::string, Error> Builder::toBytes() const {
    std::string out;
    StringSink sink(out);
    if (auto done = emit(sink); !done) {
        return std::unexpected(done.error());
    }
    return out;
}

std::expected<void, Error> Builder::write(const std::filesystem::path& file) const {
    std::filesystem::path temp = file;
    temp += ".wabcut-partial";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return std::unexpected(Error{ErrorCode::IoError, "Cannot write " + utf8FromPath(file)});
        }
        FileSink sink(out);
        auto done = emit(sink);
        out.flush();
        if (!done || !out) {
            out.close();
            std::error_code ec;
            std::filesystem::remove(temp, ec);
            return std::unexpected(done ? Error{ErrorCode::IoError, "Cannot write " + utf8FromPath(file)} : done.error());
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return std::unexpected(Error{ErrorCode::IoError, "Cannot write " + utf8FromPath(file)});
    }
    return {};
}

// ---------------------------------------------------------------------------------------------------------------------
// Reader

std::expected<Reader, Error> Reader::open(std::string bytes) {
    Reader r;
    r.m_bytes = std::move(bytes);
    const std::string_view in = r.m_bytes;
    if (in.size() < 512 || std::memcmp(in.data(), kMagic, sizeof kMagic) != 0) {
        return std::unexpected(parseFailure("not a compound file"));
    }
    std::memcpy(r.m_headerClsid.data(), in.data() + 8, 16);
    r.m_majorVersion = getU16(in, 26);
    if (getU16(in, 28) != 0xFFFE) {
        return std::unexpected(parseFailure("unknown byte order"));
    }
    const std::uint16_t shift = getU16(in, 30);
    if (!(r.m_majorVersion == 3 && shift == 9) && !(r.m_majorVersion == 4 && shift == 12)) {
        return std::unexpected(parseFailure("unsupported version or sector size"));
    }
    r.m_sectorSize = 1u << shift;
    if (getU16(in, 32) != 6) {
        return std::unexpected(parseFailure("unsupported mini sector size"));
    }
    const std::uint32_t fatCount = getU32(in, 44);
    const std::uint32_t dirStart = getU32(in, 48);
    r.m_miniCutoff = getU32(in, 56);
    const std::uint32_t miniFatStart = getU32(in, 60);
    const std::uint32_t miniFatCount = getU32(in, 64);
    std::uint32_t difatStart = getU32(in, 68);
    const std::uint32_t difatCount = getU32(in, 72);
    if (r.m_miniCutoff != 4096) {
        return std::unexpected(parseFailure("unsupported mini stream cutoff"));
    }
    const std::uint64_t ss = r.m_sectorSize;
    if (in.size() < ss) {
        return std::unexpected(parseFailure("truncated header"));
    }
    const std::uint64_t sectorCount = (in.size() - ss + ss - 1) / ss; // a short last sector reads as zeros
    if (fatCount > sectorCount || difatCount > sectorCount || miniFatCount > sectorCount) {
        return std::unexpected(parseFailure("sector counts larger than the file"));
    }

    // The FAT's sectors: the header's DIFAT, then the DIFAT chain.
    std::vector<std::uint32_t> fatSectors;
    for (std::size_t i = 0; i < kHeaderDifatEntries && fatSectors.size() < fatCount; ++i) {
        fatSectors.push_back(getU32(in, 76 + 4 * i));
    }
    const std::uint64_t perSector = ss / 4;
    for (std::uint32_t d = 0; d < difatCount && fatSectors.size() < fatCount; ++d) {
        if (difatStart >= sectorCount) {
            return std::unexpected(parseFailure("DIFAT sector outside the file"));
        }
        const std::size_t base = static_cast<std::size_t>((difatStart + 1) * ss);
        if (base + ss > in.size()) {
            return std::unexpected(parseFailure("truncated DIFAT"));
        }
        for (std::uint64_t k = 0; k + 1 < perSector && fatSectors.size() < fatCount; ++k) {
            fatSectors.push_back(getU32(in, base + static_cast<std::size_t>(4 * k)));
        }
        difatStart = getU32(in, base + static_cast<std::size_t>(4 * (perSector - 1)));
    }
    if (fatSectors.size() != fatCount) {
        return std::unexpected(parseFailure("FAT sector list is short"));
    }
    r.m_fat.reserve(static_cast<std::size_t>(fatCount * perSector));
    for (const std::uint32_t s : fatSectors) {
        if (s >= sectorCount) {
            return std::unexpected(parseFailure("FAT sector outside the file"));
        }
        const std::size_t base = static_cast<std::size_t>((std::uint64_t{s} + 1) * ss);
        for (std::uint64_t k = 0; k < perSector; ++k) {
            const std::size_t at = base + static_cast<std::size_t>(4 * k);
            r.m_fat.push_back(at + 4 <= in.size() ? getU32(in, at) : kFreeSector);
        }
    }

    auto readChain = [&](std::uint32_t start, std::uint64_t maxBytes) -> std::expected<std::string, Error> {
        auto sectors = r.chain(r.m_fat, start, sectorCount);
        if (!sectors) {
            return std::unexpected(sectors.error());
        }
        std::string out;
        for (const std::uint32_t s : *sectors) {
            if (out.size() >= maxBytes) {
                break;
            }
            const std::size_t base = static_cast<std::size_t>((std::uint64_t{s} + 1) * ss);
            const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(ss, in.size() - std::min<std::size_t>(base, in.size())));
            out.append(in.substr(std::min(base, in.size()), take));
            out.append(static_cast<std::size_t>(ss) - take, '\0');
        }
        return out;
    };

    // Directory.
    auto dir = readChain(dirStart, std::uint64_t{kMaxDirectoryEntries} * kDirEntrySize);
    if (!dir) {
        return std::unexpected(dir.error());
    }
    const std::size_t entryCount = std::min(dir->size() / kDirEntrySize, kMaxDirectoryEntries);
    if (entryCount == 0) {
        return std::unexpected(parseFailure("no directory"));
    }
    struct Raw {
        std::uint32_t left;
        std::uint32_t right;
        std::uint32_t child;
    };
    std::vector<Raw> raw(entryCount);
    r.m_entries.resize(entryCount);
    for (std::size_t i = 0; i < entryCount; ++i) {
        const std::size_t at = i * kDirEntrySize;
        Entry& e = r.m_entries[i];
        const std::uint8_t type = static_cast<std::uint8_t>((*dir)[at + 66]);
        e.type = type == 1 ? EntryType::Storage : type == 2 ? EntryType::Stream : type == 5 ? EntryType::Root : EntryType::Empty;
        std::uint16_t nameBytes = getU16(*dir, at + 64);
        if (nameBytes > 64) {
            nameBytes = 64;
        }
        std::u16string name;
        for (std::size_t c = 0; c + 1 < nameBytes; c += 2) {
            const char16_t u = static_cast<char16_t>(getU16(*dir, at + c));
            if (u == 0) {
                break;
            }
            name.push_back(u);
        }
        // UTF-16 -> UTF-8 (lone surrogates become U+FFFD).
        for (std::size_t c = 0; c < name.size(); ++c) {
            std::uint32_t cp = name[c];
            if (cp >= 0xD800 && cp <= 0xDBFF && c + 1 < name.size() && name[c + 1] >= 0xDC00 && name[c + 1] <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (name[c + 1] - 0xDC00);
                ++c;
            } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                cp = 0xFFFD;
            }
            if (cp < 0x80) {
                e.name.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
                e.name.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                e.name.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                e.name.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                e.name.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                e.name.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                e.name.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                e.name.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                e.name.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                e.name.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }
        std::memcpy(e.clsid.data(), dir->data() + at + 80, 16);
        e.startSector = getU32(*dir, at + 116);
        e.size = getU64(*dir, at + 120);
        if (r.m_majorVersion == 3) {
            e.size &= 0xFFFFFFFFull; // version 3 ignores the high half
        }
        raw[i] = {getU32(*dir, at + 68), getU32(*dir, at + 72), getU32(*dir, at + 76)};
    }
    if (r.m_entries[0].type != EntryType::Root) {
        return std::unexpected(parseFailure("the first directory entry is not the root"));
    }

    // Children of each storage: walk its tree (iteratively; each entry may appear once in the whole directory).
    std::vector<bool> seen(entryCount, false);
    seen[0] = true;
    std::vector<std::size_t> storages{0};
    while (!storages.empty()) {
        const std::size_t parent = storages.back();
        storages.pop_back();
        const std::uint32_t top = raw[parent].child;
        if (top == kNoStream) {
            continue;
        }
        // In-order traversal with an explicit stack.
        std::vector<std::uint32_t> stack;
        std::uint32_t node = top;
        while (node != kNoStream || !stack.empty()) {
            while (node != kNoStream) {
                if (node >= entryCount || seen[node]) {
                    return std::unexpected(parseFailure("directory tree is malformed"));
                }
                seen[node] = true;
                stack.push_back(node);
                node = raw[node].left;
            }
            node = stack.back();
            stack.pop_back();
            if (r.m_entries[node].type == EntryType::Empty || r.m_entries[node].type == EntryType::Root) {
                return std::unexpected(parseFailure("directory tree names an empty entry"));
            }
            r.m_entries[parent].children.push_back(node);
            if (r.m_entries[node].type == EntryType::Storage) {
                storages.push_back(node);
            }
            node = raw[node].right;
        }
    }

    // Mini FAT and mini stream.
    if (miniFatCount > 0) {
        auto miniFat = readChain(miniFatStart, std::uint64_t{miniFatCount} * ss);
        if (!miniFat) {
            return std::unexpected(miniFat.error());
        }
        for (std::size_t k = 0; k + 4 <= miniFat->size(); k += 4) {
            r.m_miniFat.push_back(getU32(*miniFat, k));
        }
        const Entry& root = r.m_entries[0];
        if (root.size > kMaxReadStreamBytes) {
            return std::unexpected(parseFailure("mini stream too large"));
        }
        if (root.startSector != kEndOfChain && root.size > 0) {
            auto ms = readChain(root.startSector, root.size);
            if (!ms) {
                return std::unexpected(ms.error());
            }
            if (ms->size() < root.size) {
                return std::unexpected(parseFailure("mini stream shorter than its size"));
            }
            ms->resize(static_cast<std::size_t>(root.size));
            r.m_miniStream = std::move(*ms);
        }
    }
    return r;
}

std::expected<std::vector<std::uint32_t>, Error> Reader::chain(const std::vector<std::uint32_t>& table, std::uint32_t start,
                                                               std::uint64_t limit) const {
    std::vector<std::uint32_t> out;
    std::uint32_t s = start;
    while (s != kEndOfChain) {
        if (s >= table.size() || s >= limit) {
            return std::unexpected(parseFailure("sector chain leaves the file"));
        }
        if (out.size() >= table.size()) {
            return std::unexpected(parseFailure("sector chain loops"));
        }
        out.push_back(s);
        s = table[s];
    }
    return out;
}

std::optional<std::size_t> Reader::child(std::size_t storage, std::string_view name) const {
    if (storage >= m_entries.size()) {
        return std::nullopt;
    }
    for (const std::size_t c : m_entries[storage].children) {
        if (nameEqualNoCase(m_entries[c].name, name)) {
            return c;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> Reader::find(std::string_view path) const {
    std::size_t at = 0;
    std::size_t pos = 0;
    while (pos < path.size()) {
        if (path[pos] == '/') {
            ++pos;
            continue;
        }
        const std::size_t end = std::min(path.find('/', pos), path.size());
        const auto next = child(at, path.substr(pos, end - pos));
        if (!next) {
            return std::nullopt;
        }
        at = *next;
        pos = end;
    }
    return at;
}

std::expected<std::string, Error> Reader::read(std::size_t stream) const {
    if (stream >= m_entries.size() || m_entries[stream].type != EntryType::Stream) {
        return std::unexpected(parseFailure("not a stream"));
    }
    const Entry& e = m_entries[stream];
    if (e.size > kMaxReadStreamBytes) {
        return std::unexpected(Error{ErrorCode::Unsupported, "compound file: stream too large"});
    }
    if (e.size == 0) {
        return std::string{};
    }
    std::string out;
    if (e.size < m_miniCutoff) {
        auto sectors = chain(m_miniFat, e.startSector, m_miniFat.size());
        if (!sectors) {
            return std::unexpected(sectors.error());
        }
        for (const std::uint32_t s : *sectors) {
            const std::size_t base = static_cast<std::size_t>(s) * m_miniSectorSize;
            if (base + m_miniSectorSize > m_miniStream.size()) {
                return std::unexpected(parseFailure("mini sector outside the mini stream"));
            }
            out.append(m_miniStream, base, m_miniSectorSize);
        }
    } else {
        const std::uint64_t ss = m_sectorSize;
        const std::uint64_t sectorCount = (m_bytes.size() - ss + ss - 1) / ss;
        auto sectors = chain(m_fat, e.startSector, sectorCount);
        if (!sectors) {
            return std::unexpected(sectors.error());
        }
        for (const std::uint32_t s : *sectors) {
            if (out.size() >= e.size) {
                break;
            }
            const std::size_t base = static_cast<std::size_t>((std::uint64_t{s} + 1) * ss);
            const std::size_t take = base < m_bytes.size() ? std::min<std::size_t>(static_cast<std::size_t>(ss), m_bytes.size() - base) : 0;
            out.append(m_bytes, std::min(base, m_bytes.size()), take);
            out.append(static_cast<std::size_t>(ss) - take, '\0');
        }
    }
    if (out.size() < e.size) {
        return std::unexpected(parseFailure("stream \"" + e.name + "\" is longer than its sectors"));
    }
    out.resize(static_cast<std::size_t>(e.size));
    return out;
}

} // namespace wabcut::interchange::detail::cfb
