// AAF reader (AafReader.h): structured storage -> object tree, bounded.

#include "AafReader.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace wabcut::interchange::detail::aaf {

namespace {

std::uint16_t u16(std::string_view s, std::size_t at) {
    return static_cast<std::uint16_t>(static_cast<std::uint8_t>(s[at]) | (static_cast<std::uint8_t>(s[at + 1]) << 8));
}
std::uint32_t u32(std::string_view s, std::size_t at) {
    return static_cast<std::uint32_t>(u16(s, at)) | (static_cast<std::uint32_t>(u16(s, at + 2)) << 16);
}

Error bad(std::string message) { return Error{ErrorCode::ParseError, "AAF: " + std::move(message)}; }

struct Context {
    const cfb::Reader& cfb;
    std::size_t objects = 0;
};

std::expected<ReadObject, Error> readObject(Context& ctx, std::size_t storage, std::size_t depth);

std::expected<std::string, Error> readStreamNamed(Context& ctx, std::size_t storage, std::string_view name) {
    const auto entry = ctx.cfb.child(storage, name);
    if (!entry || ctx.cfb.entries()[*entry].type != cfb::EntryType::Stream) {
        return std::unexpected(bad("missing stream \"" + std::string(name) + "\""));
    }
    return ctx.cfb.read(*entry);
}

std::expected<void, Error> readCollection(Context& ctx, std::size_t storage, std::size_t depth, ReadObject::Property& p) {
    const std::string index = utf8FromUtf16le(p.data);
    auto bytes = readStreamNamed(ctx, storage, index + " index");
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    const std::string_view b = *bytes;
    const bool strong = p.form == 0x32 || p.form == 0x3A;
    if (strong) {
        if (b.size() < 12) {
            return std::unexpected(bad("short index " + index));
        }
        const std::uint32_t count = u32(b, 0);
        std::size_t at = 12;
        std::size_t keySize = 0;
        if (p.form == 0x3A) {
            if (b.size() < 15) {
                return std::unexpected(bad("short set index " + index));
            }
            p.keyPid = u16(b, 12);
            keySize = static_cast<std::uint8_t>(b[14]);
            at = 15;
        }
        const std::size_t entrySize = p.form == 0x3A ? 8 + keySize : 4;
        if (count > (b.size() - at) / entrySize) {
            return std::unexpected(bad("index " + index + " lists more entries than it holds"));
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t local = u32(b, at);
            if (p.form == 0x3A) {
                p.keys.emplace_back(b.substr(at + 8, keySize));
            }
            at += entrySize;
            char name[16];
            std::snprintf(name, sizeof name, "{%x}", local);
            const auto child = ctx.cfb.child(storage, index + name);
            if (!child || ctx.cfb.entries()[*child].type != cfb::EntryType::Storage) {
                return std::unexpected(bad("missing element " + index + name));
            }
            auto element = readObject(ctx, *child, depth + 1);
            if (!element) {
                return std::unexpected(element.error());
            }
            p.objects.push_back(std::move(*element));
        }
        return {};
    }
    // Weak reference vector / set.
    if (b.size() < 9) {
        return std::unexpected(bad("short weak index " + index));
    }
    const std::uint32_t count = u32(b, 0);
    p.weakIndex = u16(b, 4);
    p.keyPid = u16(b, 6);
    const std::size_t keySize = static_cast<std::uint8_t>(b[8]);
    if (keySize == 0 || count > (b.size() - 9) / keySize) {
        return std::unexpected(bad("weak index " + index + " lists more keys than it holds"));
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        p.keys.emplace_back(b.substr(9 + i * keySize, keySize));
    }
    return {};
}

std::expected<ReadObject, Error> readObject(Context& ctx, std::size_t storage, std::size_t depth) {
    if (depth > kMaxReadDepth) {
        return std::unexpected(bad("objects nest too deeply"));
    }
    if (++ctx.objects > kMaxReadObjects) {
        return std::unexpected(bad("too many objects"));
    }
    const cfb::Reader::Entry& entry = ctx.cfb.entries()[storage];
    ReadObject o;
    o.classId = Auid::fromBytes(std::span<const std::uint8_t, 16>(entry.clsid.data(), 16));
    o.classIndex = findClass(o.classId);
    o.storageName = entry.name;
    const auto propsEntry = ctx.cfb.child(storage, "properties");
    if (!propsEntry) {
        return o; // an object without properties
    }
    auto bytes = ctx.cfb.read(*propsEntry);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    const std::string_view b = *bytes;
    if (b.size() < 4 || static_cast<std::uint8_t>(b[0]) != 0x4C) {
        return std::unexpected(bad("properties of " + entry.name + " are not little-endian"));
    }
    const std::uint16_t count = u16(b, 2);
    std::size_t tableAt = 4;
    std::size_t dataAt = 4 + 6 * std::size_t{count};
    if (dataAt > b.size()) {
        return std::unexpected(bad("short property table in " + entry.name));
    }
    for (std::uint16_t i = 0; i < count; ++i, tableAt += 6) {
        ReadObject::Property p;
        p.pid = u16(b, tableAt);
        p.form = u16(b, tableAt + 2);
        const std::uint16_t size = u16(b, tableAt + 4);
        if (dataAt + size > b.size()) {
            return std::unexpected(bad("property value past the end in " + entry.name));
        }
        p.data = std::string(b.substr(dataAt, size));
        dataAt += size;
        switch (p.form) {
        case 0x22: {
            const auto child = ctx.cfb.child(storage, utf8FromUtf16le(p.data));
            if (!child || ctx.cfb.entries()[*child].type != cfb::EntryType::Storage) {
                return std::unexpected(bad("missing strong reference " + utf8FromUtf16le(p.data)));
            }
            auto element = readObject(ctx, *child, depth + 1);
            if (!element) {
                return std::unexpected(element.error());
            }
            p.objects.push_back(std::move(*element));
            break;
        }
        case 0x32:
        case 0x3A:
        case 0x12:
        case 0x1A:
            if (auto done = readCollection(ctx, storage, depth, p); !done) {
                return std::unexpected(done.error());
            }
            break;
        case 0x02:
            if (p.data.size() < 5) {
                return std::unexpected(bad("short weak reference in " + entry.name));
            }
            p.weakIndex = u16(p.data, 0);
            p.keyPid = u16(p.data, 2);
            p.keys.push_back(p.data.substr(5));
            break;
        case 0x42: {
            if (p.data.empty() || p.data[0] != '\x55') {
                return std::unexpected(bad("stream property without a name in " + entry.name));
            }
            auto stream = readStreamNamed(ctx, storage, utf8FromUtf16le(std::string_view(p.data).substr(1)));
            if (!stream) {
                return std::unexpected(stream.error());
            }
            p.stream = std::move(*stream);
            break;
        }
        default:
            break;
        }
        o.properties.push_back(std::move(p));
    }
    return o;
}

const TypeDef* propertyType(const ReadObject& o, const ReadObject::Property& p) {
    if (o.classIndex < 0) {
        return nullptr;
    }
    const PropertyDef* def = findProperty(o.classIndex, p.pid);
    if (!def) {
        return nullptr;
    }
    const TypeDef* t = &types()[static_cast<std::size_t>(def->type)];
    for (int guard = 0; t->kind == TypeKind::Rename && guard < 8; ++guard) {
        t = &types()[static_cast<std::size_t>(t->element)];
    }
    return t;
}

} // namespace

std::string utf8FromUtf16le(std::string_view bytes) {
    std::string out;
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        std::uint32_t cp = u16(bytes, i);
        if (cp == 0) {
            break;
        }
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < bytes.size()) {
            const std::uint32_t low = u16(bytes, i + 2);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            } else {
                cp = 0xFFFD;
            }
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::expected<ReadFile, Error> readAaf(std::string bytes) {
    auto cfb = cfb::Reader::open(std::move(bytes));
    if (!cfb) {
        return std::unexpected(cfb.error());
    }
    ReadFile file;
    file.headerClsid = cfb->headerClsid();
    Context ctx{*cfb};
    auto root = readObject(ctx, 0, 0);
    if (!root) {
        return std::unexpected(root.error());
    }
    file.root = std::move(*root);
    file.objectCount = ctx.objects;
    if (const auto refs = cfb->child(0, "referenced properties")) {
        auto data = cfb->read(*refs);
        if (!data) {
            return std::unexpected(data.error());
        }
        const std::string_view b = *data;
        if (b.size() < 7 || static_cast<std::uint8_t>(b[0]) != 0x4C) {
            return std::unexpected(bad("bad referenced properties"));
        }
        const std::uint16_t paths = u16(b, 1);
        const std::uint32_t pids = u32(b, 3);
        if (pids > (b.size() - 7) / 2) {
            return std::unexpected(bad("referenced properties list more pids than they hold"));
        }
        std::vector<std::uint16_t> path;
        for (std::uint32_t i = 0; i < pids; ++i) {
            const std::uint16_t pid = u16(b, 7 + 2 * static_cast<std::size_t>(i));
            if (pid == 0) {
                file.weakPaths.push_back(std::move(path));
                path.clear();
            } else {
                path.push_back(pid);
            }
        }
        if (file.weakPaths.size() != paths) {
            return std::unexpected(bad("referenced properties count mismatch"));
        }
    }
    return file;
}

// --- ReadObject accessors ----------------------------------------------------------------------------------------

std::string_view ReadObject::className() const {
    return classIndex >= 0 ? classes()[static_cast<std::size_t>(classIndex)].name : std::string_view("?");
}

const ReadObject::Property* ReadObject::find(std::uint16_t pid) const {
    for (const Property& p : properties) {
        if (p.pid == pid) {
            return &p;
        }
    }
    return nullptr;
}

const ReadObject::Property* ReadObject::find(std::string_view name) const {
    if (classIndex < 0) {
        return nullptr;
    }
    const PropertyDef* def = findProperty(classIndex, name);
    return def ? find(def->pid) : nullptr;
}

std::optional<std::string> ReadObject::string(std::string_view name) const {
    const Property* p = find(name);
    if (!p || p->form != 0x82) {
        return std::nullopt;
    }
    return utf8FromUtf16le(p->data);
}

std::optional<std::int64_t> ReadObject::integer(std::string_view name) const {
    const Property* p = find(name);
    if (!p || p->form != 0x82 || p->data.empty() || p->data.size() > 8) {
        return std::nullopt;
    }
    const TypeDef* t = propertyType(*this, *p);
    bool isSigned = false;
    if (t && t->kind == TypeKind::Integer) {
        isSigned = t->isSigned;
    }
    std::uint64_t v = 0;
    for (std::size_t i = p->data.size(); i-- > 0;) {
        v = (v << 8) | static_cast<std::uint8_t>(p->data[i]);
    }
    if (isSigned && p->data.size() < 8 && (v >> (8 * p->data.size() - 1)) != 0) {
        v |= ~std::uint64_t{0} << (8 * p->data.size());
    }
    return static_cast<std::int64_t>(v);
}

std::optional<Auid> ReadObject::auid(std::string_view name) const {
    const Property* p = find(name);
    if (!p || p->data.size() != 16) {
        return std::nullopt;
    }
    std::array<std::uint8_t, 16> b{};
    std::memcpy(b.data(), p->data.data(), 16);
    return Auid::fromBytes(b);
}

std::optional<std::array<std::uint8_t, 32>> ReadObject::mobId(std::string_view name) const {
    const Property* p = find(name);
    if (!p || p->data.size() != 32) {
        return std::nullopt;
    }
    std::array<std::uint8_t, 32> b{};
    std::memcpy(b.data(), p->data.data(), 32);
    return b;
}

std::optional<std::pair<std::int32_t, std::int32_t>> ReadObject::rational(std::string_view name) const {
    const Property* p = find(name);
    if (!p || p->data.size() != 8) {
        return std::nullopt;
    }
    return std::pair{static_cast<std::int32_t>(u32(p->data, 0)), static_cast<std::int32_t>(u32(p->data, 4))};
}

std::optional<Auid> ReadObject::weakKey(std::string_view name) const {
    const Property* p = find(name);
    if (!p || p->form != 0x02 || p->keys.empty() || p->keys.front().size() != 16) {
        return std::nullopt;
    }
    std::array<std::uint8_t, 16> b{};
    std::memcpy(b.data(), p->keys.front().data(), 16);
    return Auid::fromBytes(b);
}

std::optional<std::pair<Auid, std::string>> ReadObject::indirect(std::string_view name) const {
    const Property* p = find(name);
    if (!p || p->data.size() < 17 || static_cast<std::uint8_t>(p->data[0]) != 0x4C) {
        return std::nullopt;
    }
    std::array<std::uint8_t, 16> b{};
    std::memcpy(b.data(), p->data.data() + 1, 16);
    return std::pair{Auid::fromBytes(b), p->data.substr(17)};
}

const ReadObject* ReadObject::child(std::string_view name) const {
    const Property* p = find(name);
    return p && p->form == 0x22 && !p->objects.empty() ? &p->objects.front() : nullptr;
}

std::vector<const ReadObject*> ReadObject::children(std::string_view name) const {
    std::vector<const ReadObject*> out;
    if (const Property* p = find(name)) {
        for (const ReadObject& o : p->objects) {
            out.push_back(&o);
        }
    }
    return out;
}

} // namespace wabcut::interchange::detail::aaf
