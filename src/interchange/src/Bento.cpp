#include "Bento.h"

#include "TextUtil.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>

namespace wabcut::interchange::detail::bento {

namespace {

constexpr std::array<std::uint8_t, 8> kMagic{0xA4, 0x43, 0x4D, 0xA5, 0x48, 0x64, 0x72, 0xD7};
constexpr std::size_t kLabelSize = 24;

// TOC codes (Bento Specification 1.0d5, "Physical Stream Format").
constexpr std::uint8_t kNewObject = 1;
constexpr std::uint8_t kNewProperty = 2;
constexpr std::uint8_t kNewType = 3;
constexpr std::uint8_t kExplicitGen = 4;
constexpr std::uint8_t kOffset4Len4 = 5;
constexpr std::uint8_t kContdOffset4Len4 = 6;
constexpr std::uint8_t kOffset8Len4 = 7;
constexpr std::uint8_t kContdOffset8Len4 = 8;
constexpr std::uint8_t kImmediate0 = 9;
constexpr std::uint8_t kImmediate4 = 13;
constexpr std::uint8_t kContdImmediate4 = 14;
constexpr std::uint8_t kReferenceListId = 15;
constexpr std::uint8_t kEndOfBuffer = 24;
constexpr std::uint8_t kNop = 0xFF;

void put32(std::string& out, std::uint32_t v) {
    for (int i = 3; i >= 0; --i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
}
void put16(std::string& out, std::uint16_t v) {
    out.push_back(static_cast<char>(v >> 8));
    out.push_back(static_cast<char>(v & 0xFF));
}
std::uint32_t get32(std::string_view b, std::size_t at) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        v = (v << 8) | static_cast<std::uint8_t>(b[at + i]);
    }
    return v;
}
std::uint16_t get16(std::string_view b, std::size_t at) {
    return static_cast<std::uint16_t>((static_cast<std::uint8_t>(b[at]) << 8) | static_cast<std::uint8_t>(b[at + 1]));
}

Error parseError(std::string message) { return Error{ErrorCode::ParseError, "Bento container: " + std::move(message)}; }

} // namespace

class Builder::Sink {
public:
    virtual ~Sink() = default;
    virtual bool put(const char* data, std::size_t size) = 0;
    bool put(std::string_view data) { return put(data.data(), data.size()); }
};

namespace {

class StringSink final : public Builder::Sink {
public:
    std::string bytes;
    bool put(const char* data, std::size_t size) override {
        bytes.append(data, size);
        return true;
    }
};

class FileSink final : public Builder::Sink {
public:
    explicit FileSink(const std::filesystem::path& file) : m_out(file, std::ios::binary | std::ios::trunc) {}
    bool ok() const { return static_cast<bool>(m_out); }
    bool put(const char* data, std::size_t size) override {
        m_out.write(data, static_cast<std::streamsize>(size));
        return static_cast<bool>(m_out);
    }
    bool close() {
        m_out.close();
        return !m_out.fail();
    }

private:
    std::ofstream m_out;
};

} // namespace

Builder::Builder() = default;

std::uint32_t Builder::named(std::string_view name, std::uint32_t property) {
    if (const auto it = m_names.find(name); it != m_names.end()) {
        return it->second;
    }
    const std::uint32_t id = newObject();
    m_names.emplace(std::string(name), id);
    std::string bytes(name);
    bytes.push_back('\0');
    set(id, property, kType7BitAscii, std::move(bytes));
    return id;
}

void Builder::set(std::uint32_t object, std::uint32_t property, std::uint32_t type, std::string bytes) {
    m_objects[object][property].push_back(Value{type, cfb::StreamSource::of(std::move(bytes)), 0});
}

void Builder::setStream(std::uint32_t object, std::uint32_t property, std::uint32_t type, cfb::StreamSource data) {
    m_objects[object][property].push_back(Value{type, std::move(data), 0});
}

void Builder::setReference(std::uint32_t object, std::uint32_t property, std::uint32_t type, std::string bytes,
                           const std::vector<std::uint32_t>& targets) {
    Value v{type, cfb::StreamSource::of(std::move(bytes)), 0};
    if (!targets.empty()) {
        // The reference list: (key, object) pairs, the key being the object's own ID.
        std::string pairs;
        std::vector<std::uint32_t> unique = targets;
        std::sort(unique.begin(), unique.end());
        unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
        for (const std::uint32_t t : unique) {
            put32(pairs, t);
            put32(pairs, t);
        }
        v.referenceList = newObject();
        m_objects[v.referenceList][kPropObjReferences].push_back(
            Value{kTypeObjRefData, cfb::StreamSource::of(std::move(pairs)), 0});
    }
    m_objects[object][property].push_back(std::move(v));
}

std::expected<std::uint64_t, Error> Builder::emit(Sink& sink) const {
    // Value data first, in TOC order; values of up to 4 bytes go into the TOC.
    std::map<const Value*, std::uint64_t> offsets;
    std::uint64_t at = 0;
    std::vector<char> buffer;
    for (const auto& [id, properties] : m_objects) {
        for (const auto& [property, values] : properties) {
            for (const Value& v : values) {
                const std::uint64_t size = v.data.length();
                if (size <= 4 && !v.data.fromFile()) {
                    continue;
                }
                offsets[&v] = at;
                if (!v.data.fromFile()) {
                    if (!sink.put(v.data.bytes)) {
                        return std::unexpected(Error{ErrorCode::IoError, "OMF: cannot write"});
                    }
                } else {
                    std::ifstream in(v.data.file, std::ios::binary);
                    if (!in) {
                        return std::unexpected(Error{ErrorCode::IoError, "OMF: cannot read " + utf8FromPath(v.data.file)});
                    }
                    in.seekg(static_cast<std::streamoff>(v.data.offset));
                    buffer.resize(1 << 20);
                    std::uint64_t left = size;
                    while (left > 0) {
                        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(left, buffer.size()));
                        in.read(buffer.data(), static_cast<std::streamsize>(n));
                        if (static_cast<std::size_t>(in.gcount()) != n || !sink.put(buffer.data(), n)) {
                            return std::unexpected(
                                Error{ErrorCode::IoError, "OMF: " + utf8FromPath(v.data.file) + " is shorter than expected"});
                        }
                        left -= n;
                    }
                }
                at += size;
            }
        }
    }
    const std::uint64_t tocOffset = at;

    // The TOC object's own entries: the seeds, the TOC and the whole container (filled in once the sizes are known).
    auto encodeToc = [&](std::uint32_t tocSize, std::uint32_t containerSize) {
        std::string toc;
        std::size_t blockStart = 0;
        auto endBlock = [&]() {
            toc.push_back(static_cast<char>(kEndOfBuffer));
            while ((toc.size() - blockStart) % kTocBlockSize != 0) {
                toc.push_back(static_cast<char>(kEndOfBuffer));
            }
            blockStart = toc.size();
        };
        struct Entry {
            std::uint32_t property;
            std::uint32_t type;
            std::string data; // immediate bytes (<= 4) or offset+length
            bool immediate;
            std::uint32_t refList;
        };
        auto entriesOf = [&](std::uint32_t id, const Properties& properties) {
            std::vector<Entry> entries;
            if (id == kTocObject) {
                std::string seed, minSeed, tocRef, containerRef, deleted;
                put32(seed, m_nextId);
                put32(minSeed, kFirstUserId);
                put32(tocRef, static_cast<std::uint32_t>(tocOffset));
                put32(tocRef, tocSize);
                put32(containerRef, 0);
                put32(containerRef, containerSize);
                put32(deleted, 0);
                entries.push_back({kPropTocSeed, kTypeTocId, seed, true, 0});
                entries.push_back({kPropTocMinSeed, kTypeTocId, minSeed, true, 0});
                entries.push_back({kPropTocObject, kTypeTocId, tocRef, false, 0});
                entries.push_back({kPropTocContainer, kTypeTocId, containerRef, false, 0});
                entries.push_back({kPropTocDeleted, kTypeTocId, deleted, true, 0});
            }
            for (const auto& [property, values] : properties) {
                for (const Value& v : values) {
                    Entry e{property, v.type, {}, false, v.referenceList};
                    if (const auto it = offsets.find(&v); it != offsets.end()) {
                        put32(e.data, static_cast<std::uint32_t>(it->second));
                        put32(e.data, static_cast<std::uint32_t>(v.data.length()));
                    } else {
                        e.immediate = true;
                        e.data = v.data.bytes;
                    }
                    entries.push_back(std::move(e));
                }
            }
            return entries;
        };
        auto encode = [&](std::string& out, std::uint32_t id, const std::vector<Entry>& entries, std::size_t from,
                          std::size_t to) {
            for (std::size_t i = from; i < to; ++i) {
                const Entry& e = entries[i];
                if (i == from) {
                    out.push_back(static_cast<char>(kNewObject));
                    put32(out, id);
                    put32(out, e.property);
                    put32(out, e.type);
                } else if (e.property != entries[i - 1].property) {
                    out.push_back(static_cast<char>(kNewProperty));
                    put32(out, e.property);
                    put32(out, e.type);
                } else {
                    out.push_back(static_cast<char>(kNewType));
                    put32(out, e.type);
                }
                if (e.refList != 0) {
                    out.push_back(static_cast<char>(kReferenceListId));
                    put32(out, e.refList);
                }
                if (e.immediate) {
                    out.push_back(static_cast<char>(kImmediate0 + e.data.size()));
                    if (!e.data.empty()) { // Immediate0 has no field; the others a 4-byte one, data first
                        std::string field = e.data;
                        field.resize(4, '\0');
                        out += field;
                    }
                } else {
                    out.push_back(static_cast<char>(kOffset4Len4));
                    out += e.data;
                }
            }
        };
        std::vector<std::uint32_t> ids;
        ids.push_back(kTocObject);
        for (const auto& [id, properties] : m_objects) {
            if (id != kTocObject) {
                ids.push_back(id);
            }
        }
        static const Properties kNone;
        for (const std::uint32_t id : ids) {
            const auto it = m_objects.find(id);
            const std::vector<Entry> entries = entriesOf(id, it != m_objects.end() ? it->second : kNone);
            // An object goes into one block: when it does not fit in what is left of this one (keeping a byte for
            // EndOfBuffer) it starts the next. One longer than a block is split, each part opening with NewObject.
            std::size_t from = 0;
            while (from < entries.size()) {
                const std::size_t room = kTocBlockSize - 1 - (toc.size() - blockStart);
                std::string whole;
                encode(whole, id, entries, from, entries.size());
                if (whole.size() <= room) {
                    toc += whole;
                    break;
                }
                if (toc.size() > blockStart && whole.size() <= kTocBlockSize - 1) {
                    endBlock();
                    continue;
                }
                std::size_t to = from;
                std::string piece;
                for (std::size_t k = from + 1; k <= entries.size(); ++k) {
                    std::string trial;
                    encode(trial, id, entries, from, k);
                    if (trial.size() > room) {
                        break;
                    }
                    piece = std::move(trial);
                    to = k;
                }
                if (to > from) {
                    toc += piece;
                    from = to;
                }
                endBlock(); // an entry is at most 27 bytes, so a fresh block always takes the next one
            }
        }
        toc.push_back(static_cast<char>(kEndOfBuffer));
        return toc;
    };
    const std::string probe = encodeToc(0, 0);
    const std::uint64_t tocSize = probe.size();
    const std::uint64_t pad = (4 - (tocOffset + tocSize) % 4) % 4;
    const std::uint64_t total = tocOffset + tocSize + pad + kLabelSize;
    if (total > std::numeric_limits<std::int32_t>::max()) {
        return std::unexpected(
            Error{ErrorCode::Unsupported, "OMF files cannot be larger than 2 GB (what OMF readers take): use Separate Audio"});
    }
    const std::string toc = encodeToc(static_cast<std::uint32_t>(tocSize), static_cast<std::uint32_t>(total));
    std::string tail = toc;
    tail.append(static_cast<std::size_t>(pad), '\0');
    tail.append(reinterpret_cast<const char*>(kMagic.data()), kMagic.size());
    put16(tail, 0);                                    // flags
    put16(tail, kTocBlockSize / 1024);                 // TOC block size, KiB
    put16(tail, 2);                                    // major version
    put16(tail, 0);                                    // minor version
    put32(tail, static_cast<std::uint32_t>(tocOffset));
    put32(tail, static_cast<std::uint32_t>(tocSize));
    if (!sink.put(tail)) {
        return std::unexpected(Error{ErrorCode::IoError, "OMF: cannot write"});
    }
    return total;
}

std::expected<std::uint64_t, Error> Builder::write(const std::filesystem::path& file) const {
    std::filesystem::path temp = file;
    temp += ".wabcut-partial";
    std::uint64_t size = 0;
    {
        FileSink sink(temp);
        if (!sink.ok()) {
            return std::unexpected(Error{ErrorCode::IoError, "Cannot write " + utf8FromPath(file)});
        }
        auto done = emit(sink);
        if (!done || !sink.close()) {
            std::error_code ec;
            std::filesystem::remove(temp, ec);
            return std::unexpected(done ? Error{ErrorCode::IoError, "Cannot write " + utf8FromPath(file)} : done.error());
        }
        size = *done;
    }
    std::error_code ec;
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return std::unexpected(Error{ErrorCode::IoError, "Cannot write " + utf8FromPath(file)});
    }
    return size;
}

std::expected<std::string, Error> Builder::toBytes() const {
    StringSink sink;
    auto done = emit(sink);
    if (!done) {
        return std::unexpected(done.error());
    }
    return std::move(sink.bytes);
}

// --- Reading ------------------------------------------------------------------------------------------------------

const ReadValue* Container::value(std::uint32_t object, std::uint32_t property) const {
    const auto o = objects.find(object);
    if (o == objects.end()) {
        return nullptr;
    }
    const auto p = o->second.find(property);
    return p == o->second.end() || p->second.empty() ? nullptr : &p->second.front();
}

const ReadValue* Container::value(std::uint32_t object, std::string_view name) const {
    const auto o = objects.find(object);
    if (o == objects.end()) {
        return nullptr;
    }
    for (const auto& [property, values] : o->second) {
        const auto n = names.find(property);
        if (n != names.end() && n->second == name && !values.empty()) {
            return &values.front();
        }
    }
    return nullptr;
}

std::optional<std::uint32_t> Container::resolve(const ReadValue& value, std::uint32_t key) const {
    const ReadValue* list = value.referenceList != 0 ? this->value(value.referenceList, kPropObjReferences) : nullptr;
    if (!list) {
        return std::nullopt;
    }
    for (std::size_t at = 0; at + 8 <= list->data.size(); at += 8) {
        if (get32(list->data, at) == key) {
            return get32(list->data, at + 4);
        }
    }
    return std::nullopt;
}

std::expected<Container, Error> read(std::string_view bytes, std::size_t maxValueBytes) {
    if (bytes.size() < kLabelSize) {
        return std::unexpected(parseError("too short"));
    }
    const std::string_view label = bytes.substr(bytes.size() - kLabelSize);
    if (!std::equal(kMagic.begin(), kMagic.end(), reinterpret_cast<const std::uint8_t*>(label.data()))) {
        return std::unexpected(parseError("no Bento label"));
    }
    Container c;
    const std::uint32_t blockKiB = get16(label, 10);
    c.majorVersion = get16(label, 12);
    c.minorVersion = get16(label, 14);
    const std::uint64_t tocOffset = get32(label, 16);
    const std::uint64_t tocSize = get32(label, 20);
    if (blockKiB == 0 || blockKiB > 65536) {
        return std::unexpected(parseError("bad TOC block size"));
    }
    c.blockSize = blockKiB * 1024u;
    if (tocSize == 0 || tocSize > kMaxTocBytes || tocOffset + tocSize > bytes.size() - kLabelSize) {
        return std::unexpected(parseError("the TOC lies outside the file"));
    }
    const std::string_view toc = bytes.substr(static_cast<std::size_t>(tocOffset), static_cast<std::size_t>(tocSize));
    const std::uint64_t dataEnd = bytes.size(); // the container value (object 1) spans the whole file

    std::uint32_t object = 0;
    std::uint32_t property = 0;
    std::uint32_t type = 0;
    std::uint32_t pendingRefList = 0;
    ReadValue* current = nullptr;
    std::size_t values = 0;
    auto startValue = [&]() -> std::expected<ReadValue*, Error> {
        if (object == 0 || property == 0 || type == 0) {
            return std::unexpected(parseError("a value before its object, property and type"));
        }
        if (++values > kMaxValues) {
            return std::unexpected(parseError("too many values"));
        }
        auto& list = c.objects[object][property];
        list.push_back(ReadValue{});
        ReadValue* v = &list.back();
        v->type = type;
        v->referenceList = pendingRefList;
        pendingRefList = 0;
        return v;
    };
    auto addSegment = [&](ReadValue& v, std::uint64_t offset, std::uint64_t size, bool first) -> std::expected<void, Error> {
        if (offset > dataEnd || size > dataEnd - offset) {
            return std::unexpected(parseError("a value lies outside the file"));
        }
        if (first) {
            v.offset = offset;
        }
        v.size += size;
        if (v.size <= maxValueBytes) {
            v.data.append(bytes.substr(static_cast<std::size_t>(offset), static_cast<std::size_t>(size)));
        } else {
            v.data.clear();
        }
        return {};
    };
    for (std::size_t block = 0; block < toc.size(); block += c.blockSize) {
        const std::string_view b = toc.substr(block, std::min<std::size_t>(c.blockSize, toc.size() - block));
        std::size_t at = 0;
        auto need = [&](std::size_t n) { return at + n <= b.size(); };
        bool end = false;
        while (at < b.size() && !end) {
            const auto code = static_cast<std::uint8_t>(b[at++]);
            switch (code) {
            case kNewObject:
                if (!need(12)) {
                    return std::unexpected(parseError("truncated TOC"));
                }
                object = get32(b, at);
                property = get32(b, at + 4);
                type = get32(b, at + 8);
                at += 12;
                current = nullptr;
                break;
            case kNewProperty:
                if (!need(8)) {
                    return std::unexpected(parseError("truncated TOC"));
                }
                property = get32(b, at);
                type = get32(b, at + 4);
                at += 8;
                current = nullptr;
                break;
            case kNewType:
                if (!need(4)) {
                    return std::unexpected(parseError("truncated TOC"));
                }
                type = get32(b, at);
                at += 4;
                current = nullptr;
                break;
            case kExplicitGen:
            case kReferenceListId:
                if (!need(4)) {
                    return std::unexpected(parseError("truncated TOC"));
                }
                if (code == kReferenceListId) {
                    pendingRefList = get32(b, at);
                }
                at += 4;
                break;
            case kOffset4Len4:
            case kContdOffset4Len4:
            case kOffset8Len4:
            case kContdOffset8Len4: {
                const bool wide = code == kOffset8Len4 || code == kContdOffset8Len4;
                const bool continued = code == kContdOffset4Len4 || code == kContdOffset8Len4;
                if (!need(wide ? 12 : 8)) {
                    return std::unexpected(parseError("truncated TOC"));
                }
                const std::uint64_t offset =
                    wide ? (static_cast<std::uint64_t>(get32(b, at)) << 32) | get32(b, at + 4) : get32(b, at);
                const std::uint64_t size = get32(b, at + (wide ? 8 : 4));
                at += wide ? 12 : 8;
                if (!continued || !current) {
                    auto v = startValue();
                    if (!v) {
                        return std::unexpected(v.error());
                    }
                    current = *v;
                }
                if (auto ok = addSegment(*current, offset, size, !continued); !ok) {
                    return std::unexpected(ok.error());
                }
                break;
            }
            case kContdImmediate4:
            case 9:
            case 10:
            case 11:
            case 12:
            case 13: {
                const std::size_t n = code == kContdImmediate4 ? 4 : static_cast<std::size_t>(code - kImmediate0);
                const std::size_t field = code == kImmediate0 ? 0 : 4;
                if (!need(field)) {
                    return std::unexpected(parseError("truncated TOC"));
                }
                if (code != kContdImmediate4 || !current) {
                    auto v = startValue();
                    if (!v) {
                        return std::unexpected(v.error());
                    }
                    current = *v;
                    current->immediate = true;
                }
                current->data.append(b.substr(at, n));
                current->size += n;
                at += field;
                break;
            }
            case kEndOfBuffer:
                end = true;
                break;
            case kNop:
                break;
            default:
                return std::unexpected(parseError("unknown TOC code " + std::to_string(code)));
            }
        }
    }
    static_cast<void>(kImmediate4);
    for (const auto& [id, properties] : c.objects) {
        for (const std::uint32_t nameProperty : {kPropGlobalTypeName, kPropGlobalPropName}) {
            const auto p = properties.find(nameProperty);
            if (p != properties.end() && !p->second.empty()) {
                std::string name = p->second.front().data;
                if (!name.empty() && name.back() == '\0') {
                    name.pop_back();
                }
                c.names[id] = std::move(name);
            }
        }
    }
    return c;
}

} // namespace wabcut::interchange::detail::bento
