// AAF object tree and its structured-storage serialisation (AafFile.h), plus the model lookups (AafModel.h).

#include "AafFile.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>
#include <unordered_map>

namespace wabcut::interchange::detail::aaf {

// ---------------------------------------------------------------------------------------------------------------------
// Model lookups

std::array<std::uint8_t, 16> Auid::bytes() const {
    std::array<std::uint8_t, 16> out{};
    for (int i = 0; i < 4; ++i) {
        out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((d1 >> (8 * i)) & 0xFF);
    }
    out[4] = static_cast<std::uint8_t>(d2 & 0xFF);
    out[5] = static_cast<std::uint8_t>(d2 >> 8);
    out[6] = static_cast<std::uint8_t>(d3 & 0xFF);
    out[7] = static_cast<std::uint8_t>(d3 >> 8);
    std::copy(d4.begin(), d4.end(), out.begin() + 8);
    return out;
}

Auid Auid::fromBytes(std::span<const std::uint8_t, 16> b) {
    Auid a;
    a.d1 = static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
           (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
    a.d2 = static_cast<std::uint16_t>(b[4] | (b[5] << 8));
    a.d3 = static_cast<std::uint16_t>(b[6] | (b[7] << 8));
    std::copy(b.begin() + 8, b.end(), a.d4.begin());
    return a;
}

std::string Auid::toString() const {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", d1, d2, d3, d4[0], d4[1], d4[2],
                  d4[3], d4[4], d4[5], d4[6], d4[7]);
    return buf;
}

std::optional<Auid> Auid::parse(std::string_view text) {
    std::string hex;
    for (const char c : text) {
        if (c == '-' || c == '{' || c == '}') {
            continue;
        }
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return std::nullopt;
        }
        hex.push_back(c);
    }
    if (hex.size() != 32) {
        return std::nullopt;
    }
    auto nibbles = [&](std::size_t at, std::size_t count) {
        std::uint32_t v = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const char c = hex[at + i];
            v = (v << 4) | static_cast<std::uint32_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
        }
        return v;
    };
    Auid a;
    a.d1 = nibbles(0, 8);
    a.d2 = static_cast<std::uint16_t>(nibbles(8, 4));
    a.d3 = static_cast<std::uint16_t>(nibbles(12, 4));
    for (std::size_t i = 0; i < 8; ++i) {
        a.d4[i] = static_cast<std::uint8_t>(nibbles(16 + 2 * i, 2));
    }
    return a;
}

std::span<const PropertyDef> ClassDef::ownProperties() const {
    return properties().subspan(static_cast<std::size_t>(firstProperty), static_cast<std::size_t>(propertyCount));
}

namespace {

struct Indexes {
    std::unordered_map<std::string_view, int> typeByName;
    std::map<Auid, int> typeById;
    std::unordered_map<std::string_view, int> classByName;
    std::map<Auid, int> classById;
    std::map<Auid, const PropertyDef*> propertyById;

    Indexes() {
        for (std::size_t i = 0; i < types().size(); ++i) {
            typeByName.emplace(types()[i].name, static_cast<int>(i));
            typeById.emplace(types()[i].id, static_cast<int>(i));
        }
        for (std::size_t i = 0; i < classes().size(); ++i) {
            classByName.emplace(classes()[i].name, static_cast<int>(i));
            classById.emplace(classes()[i].id, static_cast<int>(i));
        }
        for (const PropertyDef& p : properties()) {
            propertyById.emplace(p.id, &p);
        }
    }
};

const Indexes& indexes() {
    static const Indexes kIndexes;
    return kIndexes;
}

} // namespace

int findType(std::string_view name) {
    const auto it = indexes().typeByName.find(name);
    return it == indexes().typeByName.end() ? -1 : it->second;
}
int findType(const Auid& id) {
    const auto it = indexes().typeById.find(id);
    return it == indexes().typeById.end() ? -1 : it->second;
}
int findClass(std::string_view name) {
    const auto it = indexes().classByName.find(name);
    return it == indexes().classByName.end() ? -1 : it->second;
}
int findClass(const Auid& id) {
    const auto it = indexes().classById.find(id);
    return it == indexes().classById.end() ? -1 : it->second;
}

const PropertyDef* findProperty(int classIndex, std::string_view name) {
    for (int c = classIndex; c >= 0 && c < static_cast<int>(classes().size()); c = classes()[static_cast<std::size_t>(c)].parent) {
        for (const PropertyDef& p : classes()[static_cast<std::size_t>(c)].ownProperties()) {
            if (p.name == name) {
                return &p;
            }
        }
    }
    return nullptr;
}

const PropertyDef* findProperty(int classIndex, std::uint16_t pid) {
    for (int c = classIndex; c >= 0 && c < static_cast<int>(classes().size()); c = classes()[static_cast<std::size_t>(c)].parent) {
        for (const PropertyDef& p : classes()[static_cast<std::size_t>(c)].ownProperties()) {
            if (p.pid == pid) {
                return &p;
            }
        }
    }
    return nullptr;
}

const PropertyDef* findPropertyById(const Auid& id) {
    const auto it = indexes().propertyById.find(id);
    return it == indexes().propertyById.end() ? nullptr : it->second;
}

bool isA(int classIndex, int ancestorIndex) {
    for (int c = classIndex; c >= 0 && c < static_cast<int>(classes().size()); c = classes()[static_cast<std::size_t>(c)].parent) {
        if (c == ancestorIndex) {
            return true;
        }
    }
    return false;
}

std::optional<std::size_t> fixedSize(int typeIndex) {
    if (typeIndex < 0 || typeIndex >= static_cast<int>(types().size())) {
        return std::nullopt;
    }
    const TypeDef& t = types()[static_cast<std::size_t>(typeIndex)];
    switch (t.kind) {
    case TypeKind::Integer:
        return t.size;
    case TypeKind::Enum:
    case TypeKind::Rename:
        return fixedSize(t.element);
    case TypeKind::ExtEnum:
        return 16;
    case TypeKind::FixedArray: {
        const auto e = fixedSize(t.element);
        return e ? std::optional<std::size_t>(*e * t.count) : std::nullopt;
    }
    case TypeKind::Record: {
        std::size_t total = 0;
        for (const RecordMember& m : t.members) {
            const auto e = fixedSize(m.type);
            if (!e) {
                return std::nullopt;
            }
            total += *e;
        }
        return total;
    }
    default:
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Encoding helpers

namespace {

const TypeDef& typeAt(int index) { return types()[static_cast<std::size_t>(index)]; }

/// The type a rename stands for (renames of renames too).
const TypeDef& unrenamed(int index) {
    const TypeDef* t = &typeAt(index);
    for (int guard = 0; t->kind == TypeKind::Rename && guard < 8; ++guard) {
        t = &typeAt(t->element);
    }
    return *t;
}

void putLe(std::string& out, std::uint64_t v, std::size_t bytes) {
    for (std::size_t i = 0; i < bytes; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
}

std::string auidBytes(const Auid& a) {
    const auto b = a.bytes();
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

constexpr std::uint16_t kFormData = 0x82;
constexpr std::uint16_t kFormStream = 0x42;
constexpr std::uint16_t kFormStrong = 0x22;
constexpr std::uint16_t kFormStrongVector = 0x32;
constexpr std::uint16_t kFormStrongSet = 0x3A;
constexpr std::uint16_t kFormWeak = 0x02;
constexpr std::uint16_t kFormWeakVector = 0x12;
constexpr std::uint16_t kFormWeakSet = 0x1A;

/// OperationGroup::Parameters: a vector in the specification, stored as a set by the AAF SDK (and everyone since), keyed
/// by the Parameter's Definition under pid 0x1B01.
constexpr Auid kOperationGroupParameters{0x06010104, 0x060a, 0x0000, {0x06, 0x0e, 0x2b, 0x34, 0x01, 0x01, 0x01, 0x02}};

std::uint16_t storedForm(const PropertyDef& p) {
    const TypeDef& t = unrenamed(p.type);
    switch (t.kind) {
    case TypeKind::StrongRef:
        return kFormStrong;
    case TypeKind::WeakRef:
        return kFormWeak;
    case TypeKind::Stream:
        return kFormStream;
    case TypeKind::VarArray:
    case TypeKind::Set: {
        const TypeDef& e = unrenamed(t.element);
        if (e.kind == TypeKind::StrongRef) {
            return t.kind == TypeKind::Set || p.id == kOperationGroupParameters ? kFormStrongSet : kFormStrongVector;
        }
        if (e.kind == TypeKind::WeakRef) {
            return t.kind == TypeKind::Set ? kFormWeakSet : kFormWeakVector;
        }
        return kFormData;
    }
    default:
        return kFormData;
    }
}

/// The unique identifier property of objects of `classIndex` (Mob: MobID, definitions: Identification), or null.
const PropertyDef* uniqueProperty(int classIndex) {
    for (int c = classIndex; c >= 0; c = classes()[static_cast<std::size_t>(c)].parent) {
        for (const PropertyDef& p : classes()[static_cast<std::size_t>(c)].ownProperties()) {
            if (p.unique) {
                return &p;
            }
        }
    }
    return nullptr;
}

std::string hex(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%x", v);
    return buf;
}

} // namespace

std::string utf16le(std::string_view utf8) {
    std::string out;
    out.reserve(utf8.size() * 2 + 2);
    auto unit = [&](std::uint32_t u) {
        out.push_back(static_cast<char>(u & 0xFF));
        out.push_back(static_cast<char>((u >> 8) & 0xFF));
    };
    for (std::size_t i = 0; i < utf8.size();) {
        const auto c = static_cast<unsigned char>(utf8[i]);
        std::uint32_t cp = 0xFFFD;
        std::size_t len = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
        }
        if (len > 1) {
            bool ok = i + len <= utf8.size();
            std::uint32_t v = c & (0x7F >> len);
            for (std::size_t k = 1; ok && k < len; ++k) {
                const auto cc = static_cast<unsigned char>(utf8[i + k]);
                ok = (cc & 0xC0) == 0x80;
                v = (v << 6) | (cc & 0x3F);
            }
            const bool overlong = (len == 2 && v < 0x80) || (len == 3 && v < 0x800) || (len == 4 && v < 0x10000);
            if (ok && !overlong && v <= 0x10FFFF && !(v >= 0xD800 && v <= 0xDFFF)) {
                cp = v;
            } else {
                len = 1;
            }
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            unit(0xD800 + (cp >> 10));
            unit(0xDC00 + (cp & 0x3FF));
        } else {
            unit(cp);
        }
        i += len;
    }
    unit(0);
    return out;
}

std::string mangledName(std::string_view property, std::uint16_t pid, std::size_t size) {
    const std::string p = hex(pid);
    const std::size_t maxSize = size - p.size() - 2;
    std::string name(property);
    if (name.size() > maxSize) {
        const std::size_t half = maxSize / 2;
        std::string squeezed;
        for (std::size_t i = 0; i < maxSize; ++i) {
            if (i < half) {
                squeezed.push_back(name[i]);
            } else if (i == half) {
                squeezed.push_back('-');
            } else {
                squeezed.push_back(name[name.size() - (maxSize - i)]);
            }
        }
        name = std::move(squeezed);
    }
    return name + "-" + p;
}

MobId MobId::fromMaterial(const Auid& material) {
    MobId id;
    constexpr std::uint8_t kLabel[12] = {0x06, 0x0a, 0x2b, 0x34, 0x01, 0x01, 0x01, 0x05, 0x01, 0x01, 0x0f, 0x20};
    std::copy(std::begin(kLabel), std::end(kLabel), id.bytes.begin());
    id.bytes[12] = 0x13;
    const auto m = material.bytes();
    std::copy(m.begin(), m.end(), id.bytes.begin() + 16);
    return id;
}

bool MobId::isNull() const {
    return std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t b) { return b == 0; });
}

std::string MobId::toString() const {
    std::string out = "urn:smpte:umid:";
    char buf[4];
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i > 0 && i % 4 == 0) {
            out.push_back('.');
        }
        std::snprintf(buf, sizeof buf, "%02x", bytes[i]);
        out += buf;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// Object

struct Object::Value {
    const PropertyDef* def = nullptr;
    std::uint16_t form = 0;
    std::string data;
    std::unique_ptr<Object> strong;
    std::vector<std::unique_ptr<Object>> elements;
    cfb::StreamSource stream;
    std::vector<std::uint16_t> path;
    std::uint16_t keyPid = 0;
    std::uint8_t keySize = 0;
    std::vector<std::string> keys;
};

Object::Object(File& file, int classIndex) : m_file(&file), m_class(classIndex) {}

Object::~Object() = default;

const ClassDef& Object::classDef() const { return classes()[static_cast<std::size_t>(m_class)]; }

const PropertyDef* Object::resolve(std::string_view property, std::string_view what) {
    const PropertyDef* def = findProperty(m_class, property);
    if (!def) {
        m_file->problem(std::string(classDef().name) + " has no property " + std::string(property) + " (" +
                        std::string(what) + ")");
    }
    return def;
}

Object::Value* Object::slot(const PropertyDef& def) {
    for (auto& v : m_values) {
        if (v->def == &def) {
            return v.get();
        }
    }
    auto v = std::make_unique<Value>();
    v->def = &def;
    v->form = storedForm(def);
    m_values.push_back(std::move(v));
    return m_values.back().get();
}

bool Object::has(std::string_view property) const {
    const PropertyDef* def = findProperty(m_class, property);
    return def && std::any_of(m_values.begin(), m_values.end(), [&](const auto& v) { return v->def == def; });
}

std::vector<const Object*> Object::elements(std::string_view property) const {
    std::vector<const Object*> out;
    const PropertyDef* def = findProperty(m_class, property);
    for (const auto& v : m_values) {
        if (v->def == def) {
            for (const auto& e : v->elements) {
                out.push_back(e.get());
            }
            if (v->strong) {
                out.push_back(v->strong.get());
            }
        }
    }
    return out;
}

Object& Object::setData(std::string_view property, std::string bytes, std::string_view what,
                        bool (*accept)(const TypeDef&), std::optional<std::size_t> size) {
    const PropertyDef* def = resolve(property, what);
    if (!def) {
        return *this;
    }
    const TypeDef& t = unrenamed(def->type);
    if (storedForm(*def) != kFormData || !accept(t)) {
        m_file->problem(std::string(classDef().name) + "." + std::string(property) + " is " + std::string(t.name) +
                        ", not " + std::string(what));
        return *this;
    }
    if (size && bytes.size() != *size) {
        m_file->problem(std::string(classDef().name) + "." + std::string(property) + ": " + std::to_string(bytes.size()) +
                        " bytes for a " + std::to_string(*size) + "-byte " + std::string(t.name));
        return *this;
    }
    if (bytes.size() > 0xFFFF) {
        m_file->problem(std::string(classDef().name) + "." + std::string(property) + " is longer than 64 KB");
        return *this;
    }
    slot(*def)->data = std::move(bytes);
    return *this;
}

Object& Object::setString(std::string_view property, std::string_view utf8) {
    return setData(property, utf16le(utf8), "a string",
                   [](const TypeDef& t) { return t.kind == TypeKind::String; });
}

Object& Object::setInt(std::string_view property, std::int64_t value) {
    const PropertyDef* def = resolve(property, "an integer");
    if (!def) {
        return *this;
    }
    const TypeDef& t = unrenamed(def->type);
    const TypeDef* integer = &t;
    if (t.kind == TypeKind::Enum) {
        const bool known = std::any_of(t.enumElements.begin(), t.enumElements.end(),
                                       [&](const EnumElement& e) { return e.value == value; });
        if (!known) {
            m_file->problem(std::string(classDef().name) + "." + std::string(property) + ": " + std::to_string(value) +
                            " is not a " + std::string(t.name));
            return *this;
        }
        integer = &unrenamed(t.element);
    }
    if (integer->kind != TypeKind::Integer) {
        m_file->problem(std::string(classDef().name) + "." + std::string(property) + " is " + std::string(t.name) +
                        ", not an integer");
        return *this;
    }
    const int bits = 8 * integer->size;
    const bool fits = integer->size >= 8 ||
                      (integer->isSigned ? value >= -(std::int64_t{1} << (bits - 1)) && value < (std::int64_t{1} << (bits - 1))
                                         : value >= 0 && value < (std::int64_t{1} << bits));
    if (!fits || (integer->size >= 8 && !integer->isSigned && value < 0)) {
        m_file->problem(std::string(classDef().name) + "." + std::string(property) + ": " + std::to_string(value) +
                        " does not fit " + std::string(integer->name));
        return *this;
    }
    std::string bytes;
    putLe(bytes, static_cast<std::uint64_t>(value), integer->size);
    return setData(property, std::move(bytes), "an integer", [](const TypeDef& tt) {
        return tt.kind == TypeKind::Integer || tt.kind == TypeKind::Enum;
    });
}

Object& Object::setBool(std::string_view property, bool value) {
    return setData(property, std::string(1, value ? '\x01' : '\x00'), "a Boolean",
                   [](const TypeDef& t) { return t.name == "Boolean"; });
}

Object& Object::setAuid(std::string_view property, const Auid& value) {
    return setData(property, auidBytes(value), "an AUID",
                   [](const TypeDef& t) { return t.name == "AUID" || t.kind == TypeKind::ExtEnum; });
}

Object& Object::setMobId(std::string_view property, const MobId& value) {
    return setData(property, std::string(reinterpret_cast<const char*>(value.bytes.data()), value.bytes.size()),
                   "a MobID", [](const TypeDef& t) { return t.name == "MobIDType"; });
}

Object& Object::setRational(std::string_view property, std::int32_t numerator, std::int32_t denominator) {
    std::string bytes;
    putLe(bytes, static_cast<std::uint32_t>(numerator), 4);
    putLe(bytes, static_cast<std::uint32_t>(denominator), 4);
    return setData(property, std::move(bytes), "a Rational", [](const TypeDef& t) { return t.name == "Rational"; });
}

Object& Object::setTimeStamp(std::string_view property, const TimeStamp& v) {
    std::string bytes;
    putLe(bytes, static_cast<std::uint16_t>(v.year), 2);
    bytes.push_back(static_cast<char>(v.month));
    bytes.push_back(static_cast<char>(v.day));
    bytes.push_back(static_cast<char>(v.hour));
    bytes.push_back(static_cast<char>(v.minute));
    bytes.push_back(static_cast<char>(v.second));
    bytes.push_back(static_cast<char>(v.fraction));
    return setData(property, std::move(bytes), "a TimeStamp", [](const TypeDef& t) { return t.name == "TimeStamp"; });
}

Object& Object::setRaw(std::string_view property, std::string bytes) {
    const PropertyDef* def = findProperty(m_class, property);
    const std::optional<std::size_t> size = def ? fixedSize(def->type) : std::nullopt;
    return setData(property, std::move(bytes), "encoded data", [](const TypeDef&) { return true; }, size);
}

Object& Object::setIndirect(std::string_view property, std::string_view typeName, std::string valueBytes) {
    const int type = findType(typeName);
    if (type < 0) {
        m_file->problem("no type " + std::string(typeName) + " for an indirect value");
        return *this;
    }
    if (const auto size = fixedSize(type); size && *size != valueBytes.size()) {
        m_file->problem("indirect " + std::string(typeName) + " value of " + std::to_string(valueBytes.size()) + " bytes");
        return *this;
    }
    std::string bytes(1, '\x4C');
    bytes += auidBytes(typeAt(type).id);
    bytes += valueBytes;
    return setData(property, std::move(bytes), "an indirect value",
                   [](const TypeDef& t) { return t.kind == TypeKind::Indirect; });
}

Object& Object::setStream(std::string_view property, cfb::StreamSource data) {
    const PropertyDef* def = resolve(property, "a stream");
    if (!def) {
        return *this;
    }
    if (storedForm(*def) != kFormStream) {
        m_file->problem(std::string(classDef().name) + "." + std::string(property) + " is not a stream");
        return *this;
    }
    slot(*def)->stream = std::move(data);
    return *this;
}

namespace {

/// The pid path, key pid and key size of a weak reference type.
struct WeakTarget {
    std::vector<std::uint16_t> path;
    std::uint16_t keyPid = 0;
    std::uint8_t keySize = 0;
    bool ok = false;
};

WeakTarget weakTarget(const TypeDef& weak) {
    WeakTarget out;
    for (const Auid& a : weak.targetPath) {
        const PropertyDef* p = findPropertyById(a);
        if (!p) {
            return out;
        }
        out.path.push_back(p->pid);
    }
    const PropertyDef* key = uniqueProperty(weak.refClass);
    if (!key) {
        return out;
    }
    out.keyPid = key->pid;
    out.keySize = static_cast<std::uint8_t>(fixedSize(key->type).value_or(16));
    out.ok = !out.path.empty();
    return out;
}

} // namespace

Object& Object::setWeakRef(std::string_view property, const Auid& key) {
    const PropertyDef* def = resolve(property, "a weak reference");
    if (!def) {
        return *this;
    }
    const TypeDef& t = unrenamed(def->type);
    const WeakTarget target = t.kind == TypeKind::WeakRef ? weakTarget(t) : WeakTarget{};
    if (!target.ok || target.keySize != 16) {
        m_file->problem(std::string(classDef().name) + "." + std::string(property) + " is not a weak reference by AUID");
        return *this;
    }
    Value* v = slot(*def);
    v->path = target.path;
    v->keyPid = target.keyPid;
    v->keySize = target.keySize;
    v->keys = {auidBytes(key)};
    return *this;
}

Object& Object::setWeakRefs(std::string_view property, const std::vector<Auid>& keys) {
    const PropertyDef* def = resolve(property, "weak references");
    if (!def) {
        return *this;
    }
    const TypeDef& t = unrenamed(def->type);
    const bool collection = t.kind == TypeKind::VarArray || t.kind == TypeKind::Set;
    const WeakTarget target =
        collection && unrenamed(t.element).kind == TypeKind::WeakRef ? weakTarget(unrenamed(t.element)) : WeakTarget{};
    if (!target.ok || target.keySize != 16) {
        m_file->problem(std::string(classDef().name) + "." + std::string(property) + " is not a list of weak references");
        return *this;
    }
    Value* v = slot(*def);
    v->path = target.path;
    v->keyPid = target.keyPid;
    v->keySize = target.keySize;
    v->keys.clear();
    for (const Auid& k : keys) {
        v->keys.push_back(auidBytes(k));
    }
    return *this;
}

Object& Object::child(std::string_view property, std::string_view className) {
    const PropertyDef* def = resolve(property, "a strong reference");
    const int cls = findClass(className);
    const TypeDef* t = def ? &unrenamed(def->type) : nullptr;
    if (!def || cls < 0 || t->kind != TypeKind::StrongRef || !classes()[static_cast<std::size_t>(cls)].concrete ||
        !isA(cls, t->refClass)) {
        if (def) {
            m_file->problem(std::string(classDef().name) + "." + std::string(property) + " cannot hold a " +
                            std::string(className));
        }
        return m_file->discarded(cls);
    }
    Value* v = slot(*def);
    v->strong = m_file->make(cls);
    return *v->strong;
}

Object& Object::append(std::string_view property, std::string_view className) {
    const PropertyDef* def = resolve(property, "a strong reference list");
    const int cls = findClass(className);
    const TypeDef* t = def ? &unrenamed(def->type) : nullptr;
    const bool collection = t && (t->kind == TypeKind::VarArray || t->kind == TypeKind::Set);
    const TypeDef* e = collection ? &unrenamed(t->element) : nullptr;
    if (!def || cls < 0 || !e || e->kind != TypeKind::StrongRef || !classes()[static_cast<std::size_t>(cls)].concrete ||
        !isA(cls, e->refClass)) {
        if (def) {
            m_file->problem(std::string(classDef().name) + "." + std::string(property) + " cannot hold a " +
                            std::string(className));
        }
        return m_file->discarded(cls);
    }
    Value* v = slot(*def);
    v->elements.push_back(m_file->make(cls));
    return *v->elements.back();
}

Object& Object::touch(std::string_view property) {
    const PropertyDef* def = resolve(property, "a strong reference list");
    if (!def) {
        return *this;
    }
    const std::uint16_t form = storedForm(*def);
    if (form != kFormStrongVector && form != kFormStrongSet) {
        m_file->problem(std::string(classDef().name) + "." + std::string(property) + " is not a strong reference list");
        return *this;
    }
    slot(*def);
    return *this;
}

// ---------------------------------------------------------------------------------------------------------------------
// File

File::~File() = default;

std::unique_ptr<Object> File::make(int classIndex) { return std::unique_ptr<Object>(new Object(*this, classIndex)); }

Object& File::discarded(int classIndex) {
    // Not part of the tree: a misused setter's caller can go on chaining calls; problems() already fails the write.
    m_discarded.push_back(make(classIndex < 0 ? 1 : classIndex));
    return *m_discarded.back();
}

void File::problem(std::string message) {
    if (m_problems.size() < 100) {
        m_problems.push_back(std::move(message));
    }
}

File::File() {
    m_root = make(0); // Root
    Object& meta = m_root->child("MetaDictionary", "MetaDictionary");

    // The MetaDictionary: every class (except Root) with its own properties, then every type, in model order.
    for (std::size_t i = 1; i < classes().size(); ++i) {
        const ClassDef& c = classes()[i];
        Object& cd = meta.append("ClassDefinitions", "ClassDefinition");
        cd.setString("Name", c.name).setAuid("Identification", c.id).setBool("IsConcrete", c.concrete);
        cd.setWeakRef("ParentClass", c.parent >= 0 ? classes()[static_cast<std::size_t>(c.parent)].id : c.id);
        const PropertyDef* propsDef = findProperty(cd.classIndex(), "Properties");
        cd.slot(*propsDef); // an empty set for classes without properties, as other writers store it
        for (const PropertyDef& p : c.ownProperties()) {
            Object& pd = cd.append("Properties", "PropertyDefinition");
            pd.setString("Name", p.name)
                .setBool("IsOptional", p.optional)
                .setBool("IsUniqueIdentifier", p.unique)
                .setInt("LocalIdentification", p.pid)
                .setAuid("Identification", p.id)
                .setAuid("Type", typeAt(p.type).id);
        }
    }
    auto stringArray = [](auto&& names) {
        std::string out;
        for (const auto& n : names) {
            out += utf16le(n);
        }
        return out;
    };
    for (const TypeDef& t : types()) {
        static constexpr std::string_view kClassOf[] = {
            "TypeDefinitionInteger",       "TypeDefinitionEnumeration", "TypeDefinitionRecord",
            "TypeDefinitionFixedArray",    "TypeDefinitionVariableArray", "TypeDefinitionSet",
            "TypeDefinitionString",        "TypeDefinitionStream",      "TypeDefinitionRename",
            "TypeDefinitionExtendibleEnumeration", "TypeDefinitionIndirect", "TypeDefinitionOpaque",
            "TypeDefinitionCharacter",     "TypeDefinitionGenericCharacter", "TypeDefinitionStrongObjectReference",
            "TypeDefinitionWeakObjectReference"};
        Object& td = meta.append("TypeDefinitions", kClassOf[static_cast<std::size_t>(t.kind)]);
        td.setString("Name", t.name).setAuid("Identification", t.id);
        switch (t.kind) {
        case TypeKind::Integer:
            td.setInt("Size", t.size).setBool("IsSigned", t.isSigned);
            break;
        case TypeKind::Enum: {
            std::vector<std::string_view> names;
            std::string values;
            for (const EnumElement& e : t.enumElements) {
                names.push_back(e.name);
                putLe(values, static_cast<std::uint64_t>(e.value), 8);
            }
            td.setWeakRef("ElementType", typeAt(t.element).id).setRaw("ElementNames", stringArray(names));
            td.setRaw("ElementValues", std::move(values));
            break;
        }
        case TypeKind::Record: {
            std::vector<std::string_view> names;
            std::vector<Auid> memberTypes;
            for (const RecordMember& m : t.members) {
                names.push_back(m.name);
                memberTypes.push_back(typeAt(m.type).id);
            }
            td.setRaw("MemberNames", stringArray(names)).setWeakRefs("MemberTypes", memberTypes);
            break;
        }
        case TypeKind::FixedArray:
            td.setWeakRef("ElementType", typeAt(t.element).id).setInt("ElementCount", t.count);
            break;
        case TypeKind::VarArray:
        case TypeKind::Set:
        case TypeKind::String:
            td.setWeakRef("ElementType", typeAt(t.element).id);
            break;
        case TypeKind::Rename:
            td.setWeakRef("RenamedType", typeAt(t.element).id);
            break;
        case TypeKind::ExtEnum: {
            std::vector<std::string_view> names;
            std::string values;
            for (const ExtEnumElement& e : t.extEnumElements) {
                names.push_back(e.name);
                values += auidBytes(e.value);
            }
            td.setRaw("ElementNames", stringArray(names)).setRaw("ElementValues", std::move(values));
            break;
        }
        case TypeKind::GenericCharacter:
            td.setInt("CharacterSize", t.size);
            break;
        case TypeKind::StrongRef:
            td.setWeakRef("ReferencedType", classes()[static_cast<std::size_t>(t.refClass)].id);
            break;
        case TypeKind::WeakRef: {
            std::string path;
            for (const Auid& a : t.targetPath) {
                path += auidBytes(a);
            }
            td.setWeakRef("ReferencedType", classes()[static_cast<std::size_t>(t.refClass)].id)
                .setRaw("TargetSet", std::move(path));
            break;
        }
        case TypeKind::Stream:
        case TypeKind::Indirect:
        case TypeKind::Opaque:
        case TypeKind::Character:
            break;
        }
    }

    m_header = &m_root->child("Header", "Header");
    m_content = &m_header->child("Content", "ContentStorage");
    m_dictionary = &m_header->child("Dictionary", "Dictionary");
}

std::vector<std::string> File::missingRequired() const {
    std::vector<std::string> out;
    std::vector<const Object*> stack{m_root.get()};
    while (!stack.empty()) {
        const Object* o = stack.back();
        stack.pop_back();
        for (int c = o->m_class; c >= 0; c = classes()[static_cast<std::size_t>(c)].parent) {
            for (const PropertyDef& p : classes()[static_cast<std::size_t>(c)].ownProperties()) {
                // ObjClass is the storage's CLSID, never a stored property.
                if (p.optional || p.name == "ObjClass") {
                    continue;
                }
                const bool set = std::any_of(o->m_values.begin(), o->m_values.end(),
                                             [&](const auto& v) { return v->def == &p; });
                if (!set) {
                    out.push_back(std::string(o->classDef().name) + "." + std::string(p.name));
                }
            }
        }
        for (const auto& v : o->m_values) {
            if (v->strong) {
                stack.push_back(v->strong.get());
            }
            for (const auto& e : v->elements) {
                stack.push_back(e.get());
            }
        }
    }
    return out;
}

struct File::Serializer {
    cfb::Builder builder{4096};
    std::vector<std::vector<std::uint16_t>> weakPaths;
    std::optional<std::string> error;

    std::uint16_t weakIndex(const std::vector<std::uint16_t>& path) {
        for (std::size_t i = 0; i < weakPaths.size(); ++i) {
            if (weakPaths[i] == path) {
                return static_cast<std::uint16_t>(i);
            }
        }
        weakPaths.push_back(path);
        return static_cast<std::uint16_t>(weakPaths.size() - 1);
    }

    void fail(std::string message) {
        if (!error) {
            error = std::move(message);
        }
    }

    /// The key a strong reference set files `element` under, and the pid it names.
    std::pair<std::uint16_t, std::string> setKey(const Object& element, const PropertyDef& setProperty) {
        const PropertyDef* key = setProperty.id == kOperationGroupParameters ? findProperty(element.classIndex(), "Definition")
                                                                             : uniqueProperty(element.classIndex());
        if (!key) {
            fail(std::string(element.classDef().name) + " has no key for a set");
            return {0, {}};
        }
        for (const auto& v : element.values()) {
            if (v->def == key) {
                return {setProperty.id == kOperationGroupParameters ? std::uint16_t{0x1B01} : key->pid, v->data};
            }
        }
        fail(std::string(element.classDef().name) + " in a set without its " + std::string(key->name));
        return {key->pid, {}};
    }

    void object(const Object& o, std::size_t storage) {
        const auto clsid = o.classDef().id.bytes();
        builder.setClsid(storage, clsid);
        std::string table;
        std::string values;
        for (const auto& vp : o.values()) {
            const Object::Value& v = *vp;
            const PropertyDef& p = *v.def;
            std::string data;
            switch (v.form) {
            case kFormData:
                data = v.data;
                break;
            case kFormStream: {
                const std::string name = mangledName(p.name, p.pid, 32);
                data = std::string(1, '\x55') + utf16le(name);
                builder.addStream(storage, name, v.stream);
                break;
            }
            case kFormStrong: {
                const std::string name = mangledName(p.name, p.pid, 32);
                data = utf16le(name);
                if (v.strong) {
                    object(*v.strong, builder.addStorage(storage, name));
                }
                break;
            }
            case kFormStrongVector:
            case kFormStrongSet: {
                const std::string index = mangledName(p.name, p.pid, 22);
                data = utf16le(index);
                std::string indexBytes;
                const auto n = static_cast<std::uint32_t>(v.elements.size());
                putLe(indexBytes, n, 4);
                putLe(indexBytes, n, 4);           // next free local key
                putLe(indexBytes, 0xFFFFFFFFu, 4); // last free key
                if (v.form == kFormStrongSet) {
                    std::set<std::string> seen;
                    std::string entries;
                    std::uint16_t keyPid = 0;
                    std::uint8_t keySize = 16;
                    for (std::uint32_t k = 0; k < n; ++k) {
                        auto [pid, key] = setKey(*v.elements[k], p);
                        keyPid = pid;
                        keySize = static_cast<std::uint8_t>(key.size());
                        if (!seen.insert(key).second) {
                            fail(std::string(o.classDef().name) + "." + std::string(p.name) + " holds two objects with one key");
                        }
                        putLe(entries, k, 4);
                        putLe(entries, 1, 4); // reference count
                        entries += key;
                    }
                    if (n == 0) { // an empty set still names its key
                        const TypeDef& e = unrenamed(unrenamed(p.type).element);
                        if (const PropertyDef* key = uniqueProperty(e.refClass)) {
                            keyPid = key->pid;
                            keySize = static_cast<std::uint8_t>(fixedSize(key->type).value_or(16));
                        }
                    }
                    putLe(indexBytes, keyPid, 2);
                    indexBytes.push_back(static_cast<char>(keySize));
                    indexBytes += entries;
                } else {
                    for (std::uint32_t k = 0; k < n; ++k) {
                        putLe(indexBytes, k, 4);
                    }
                }
                for (std::uint32_t k = 0; k < n; ++k) {
                    object(*v.elements[k], builder.addStorage(storage, index + "{" + hex(k) + "}"));
                }
                builder.addStream(storage, index + " index", cfb::StreamSource::of(std::move(indexBytes)));
                break;
            }
            case kFormWeak:
                putLe(data, weakIndex(v.path), 2);
                putLe(data, v.keyPid, 2);
                data.push_back(static_cast<char>(v.keySize));
                data += v.keys.empty() ? std::string() : v.keys.front();
                break;
            case kFormWeakVector:
            case kFormWeakSet: {
                const std::string index = mangledName(p.name, p.pid, 22);
                data = utf16le(index);
                std::string indexBytes;
                putLe(indexBytes, v.keys.size(), 4);
                putLe(indexBytes, weakIndex(v.path), 2);
                putLe(indexBytes, v.keyPid, 2);
                indexBytes.push_back(static_cast<char>(v.keySize));
                for (const std::string& k : v.keys) {
                    indexBytes += k;
                }
                builder.addStream(storage, index + " index", cfb::StreamSource::of(std::move(indexBytes)));
                break;
            }
            default:
                fail("unknown stored form");
                break;
            }
            if (data.size() > 0xFFFF) {
                fail(std::string(o.classDef().name) + "." + std::string(p.name) + " is longer than 64 KB");
            }
            putLe(table, p.pid, 2);
            putLe(table, v.form, 2);
            putLe(table, data.size(), 2);
            values += data;
        }
        std::string props;
        props.push_back('\x4C'); // little-endian
        props.push_back('\x20'); // property set version 32
        putLe(props, o.values().size(), 2);
        props += table;
        props += values;
        builder.addStream(storage, "properties", cfb::StreamSource::of(std::move(props)));
    }
};

std::expected<cfb::Builder, Error> File::build() const {
    if (!m_problems.empty()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "AAF: " + m_problems.front()});
    }
    if (const auto missing = missingRequired(); !missing.empty()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "AAF: required property not set: " + missing.front()});
    }
    Serializer s;
    s.builder.setHeaderClsid(kFileKindAaf4K.bytes());
    s.object(*m_root, cfb::Builder::kRoot);
    std::string refs;
    refs.push_back('\x4C');
    std::size_t pidCount = 0;
    for (const auto& path : s.weakPaths) {
        pidCount += path.size() + 1;
    }
    putLe(refs, s.weakPaths.size(), 2);
    putLe(refs, pidCount, 4);
    for (const auto& path : s.weakPaths) {
        for (const std::uint16_t pid : path) {
            putLe(refs, pid, 2);
        }
        putLe(refs, 0, 2);
    }
    s.builder.addStream(cfb::Builder::kRoot, "referenced properties", cfb::StreamSource::of(std::move(refs)));
    if (s.error) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "AAF: " + *s.error});
    }
    return std::move(s.builder);
}

std::expected<void, Error> File::write(const std::filesystem::path& file) const {
    auto builder = build();
    if (!builder) {
        return std::unexpected(builder.error());
    }
    return builder->write(file);
}

std::expected<std::string, Error> File::toBytes() const {
    auto builder = build();
    if (!builder) {
        return std::unexpected(builder.error());
    }
    return builder->toBytes();
}

} // namespace wabcut::interchange::detail::aaf
