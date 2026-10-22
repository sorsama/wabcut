#pragma once

// Writing AAF files (AAF Low-Level Container Specification, structured storage): an object tree checked against the
// baseline model (AafModel.h) and stored in a compound file (Cfb.h).
//
//     aaf::File file;
//     aaf::Object& mob = file.contentStorage().append("Mobs", "CompositionMob");
//     mob.setMobId("MobID", id).setString("Name", "Main").setAuid("UsageCode", aaf::kUsageTopLevel);
//     file.write("Main.aaf");
//
// Every object is a storage whose CLSID is its class; its "properties" stream lists (pid, stored form, size) and the
// values. Strong references are child storages named after the property ("Header-2"); vectors and sets are numbered
// children ("Mobs-1901{0}") listed in an index stream ("Mobs-1901 index"); weak references are a key plus an index into
// the root's "referenced properties" stream (the property path of the set they refer into). The root also holds the
// MetaDictionary: every class and type definition of the model, so the file describes itself completely.
//
// Setters check the property against the object's class and the value against the property's type. A mistake is a
// programming error: it is recorded (problems()), the value is not set, and write() fails.

#include "AafModel.h"
#include "Cfb.h"

#include <wabcut/model/Error.h>

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::interchange::detail::aaf {

/// A basic SMPTE UMID (SMPTE ST 330), 32 bytes as AAF stores them.
struct MobId {
    std::array<std::uint8_t, 32> bytes{};

    /// Universal label 06.0a.2b.34.01.01.01.05.01.01.0f.20 (material type not identified, UUID/UL material number),
    /// length 13h, instance 0, material number `material` (stored like an AUID).
    static MobId fromMaterial(const Auid& material);
    bool isNull() const;
    std::string toString() const; ///< "urn:smpte:umid:060a2b34.01010105.01010f20.13000000.<material>"
    bool operator==(const MobId&) const = default;
};

/// TimeStamp (DateStruct + TimeStruct): the fraction is in hundredths of a second.
struct TimeStamp {
    std::int16_t year = 1970;
    std::uint8_t month = 1;
    std::uint8_t day = 1;
    std::uint8_t hour = 0;
    std::uint8_t minute = 0;
    std::uint8_t second = 0;
    std::uint8_t fraction = 0;
};

class File;

class Object {
public:
    /// A property's value (the serializer reads them; defined in AafFile.cpp).
    struct Value;

    ~Object();
    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;

    int classIndex() const { return m_class; }
    const std::vector<std::unique_ptr<Value>>& values() const { return m_values; }
    const ClassDef& classDef() const;

    Object& setString(std::string_view property, std::string_view utf8);
    /// Integers, enumerations (by element value) and renamed integers (PositionType, LengthType).
    Object& setInt(std::string_view property, std::int64_t value);
    Object& setBool(std::string_view property, bool value);
    /// AUID properties and extendible enumerations (UsageType, ...).
    Object& setAuid(std::string_view property, const Auid& value);
    Object& setMobId(std::string_view property, const MobId& value);
    Object& setRational(std::string_view property, std::int32_t numerator, std::int32_t denominator);
    Object& setTimeStamp(std::string_view property, const TimeStamp& value);
    /// An encoded value of a fixed-size record or an array type (checked against the type's size where it has one).
    Object& setRaw(std::string_view property, std::string bytes);
    /// An indirect value: its type (by name, e.g. "Rational", "aafInt32", "aafString") and its encoded bytes.
    Object& setIndirect(std::string_view property, std::string_view typeName, std::string valueBytes);
    /// A stream property: its bytes, in memory or from a file.
    Object& setStream(std::string_view property, cfb::StreamSource data);
    /// A weak reference to the object whose key (its unique identifier) is `key` (an AUID; MobIDs use setWeakMob).
    Object& setWeakRef(std::string_view property, const Auid& key);
    /// A vector or set of weak references.
    Object& setWeakRefs(std::string_view property, const std::vector<Auid>& keys);
    /// A strong reference: a new object of `className` (concrete, of the property's class or a subclass) owned by this one.
    Object& child(std::string_view property, std::string_view className);
    /// A new element of a strong reference vector (in order) or set (keyed by the element's unique identifier).
    Object& append(std::string_view property, std::string_view className);
    /// Makes a strong reference vector or set property exist, empty until elements are appended.
    Object& touch(std::string_view property);

    bool has(std::string_view property) const;
    /// The objects a strong reference vector / set property holds (empty when unset).
    std::vector<const Object*> elements(std::string_view property) const;

private:
    friend class File;
    Object(File& file, int classIndex);
    Value* slot(const PropertyDef& def);
    const PropertyDef* resolve(std::string_view property, std::string_view what);
    Object& setData(std::string_view property, std::string bytes, std::string_view what,
                    bool (*accept)(const TypeDef&), std::optional<std::size_t> size = std::nullopt);

    File* m_file;
    int m_class;
    std::vector<std::unique_ptr<Value>> m_values;
};

class File {
public:
    /// An empty AAF file: the root, the MetaDictionary (the whole baseline model) and a Header with its ContentStorage
    /// and Dictionary. The caller sets the Header's other properties.
    File();
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    Object& header() { return *m_header; }
    Object& contentStorage() { return *m_content; }
    Object& dictionary() { return *m_dictionary; }

    /// Programming errors met by setters (unknown property, wrong type, bad value).
    const std::vector<std::string>& problems() const { return m_problems; }
    /// Required properties not set, as "<class>.<property>" (checked on every object).
    std::vector<std::string> missingRequired() const;

    /// The compound file (4096-byte sectors, the AAF 4K binary kind). Errors: InvalidArgument (problems() or missing
    /// required properties), those of cfb::Builder.
    std::expected<void, Error> write(const std::filesystem::path& file) const;
    std::expected<std::string, Error> toBytes() const;

private:
    friend class Object;
    struct Serializer;
    std::unique_ptr<Object> make(int classIndex);
    Object& discarded(int classIndex);
    void problem(std::string message);
    std::expected<cfb::Builder, Error> build() const;

    std::unique_ptr<Object> m_root;
    Object* m_header = nullptr;
    Object* m_content = nullptr;
    Object* m_dictionary = nullptr;
    std::vector<std::string> m_problems;
    std::vector<std::unique_ptr<Object>> m_discarded;
};

/// "Name-1b01" style names of child storages: the property name squeezed to fit (AAF's mangling) and the pid in hex,
/// at most `size` - 1 characters.
std::string mangledName(std::string_view property, std::uint16_t pid, std::size_t size);

/// UTF-16LE with a terminating NUL, as AAF strings are stored (invalid UTF-8 becomes U+FFFD).
std::string utf16le(std::string_view utf8);

// Well-known identifiers.
inline constexpr Auid kFileKindAaf4K{0x0d010201, 0x0200, 0x0000, {0x06, 0x0e, 0x2b, 0x34, 0x03, 0x02, 0x01, 0x01}};
inline constexpr Auid kOpEditProtocol{0x0d011201, 0x0100, 0x0000, {0x06, 0x0e, 0x2b, 0x34, 0x04, 0x01, 0x01, 0x05}};
inline constexpr Auid kUsageTopLevel{0x0d010102, 0x0101, 0x0700, {0x06, 0x0e, 0x2b, 0x34, 0x04, 0x01, 0x01, 0x01}};
inline constexpr Auid kUsageLowerLevel{0x0d010102, 0x0101, 0x0800, {0x06, 0x0e, 0x2b, 0x34, 0x04, 0x01, 0x01, 0x01}};

} // namespace wabcut::interchange::detail::aaf
