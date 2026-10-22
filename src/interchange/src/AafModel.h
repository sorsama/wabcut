#pragma once

// The AAF baseline object model (AAF Object Specification v1.1): class, property and type definitions, generated into
// AafModel.cpp by tools/gen_aaf_model.py. The AAF writer (AafFile.h) checks every property it sets against it, encodes
// values by its types and stores all of it in each file's MetaDictionary, so a reader that trusts only the file finds
// every definition it needs.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace wabcut::interchange::detail::aaf {

/// An AAF identifier (a SMPTE label or a UUID). Stored in files as Data1 (little-endian), Data2, Data3 (little-endian),
/// then the 8 bytes of Data4.
struct Auid {
    std::uint32_t d1 = 0;
    std::uint16_t d2 = 0;
    std::uint16_t d3 = 0;
    std::array<std::uint8_t, 8> d4{};

    /// The 16 bytes as files store them.
    std::array<std::uint8_t, 16> bytes() const;
    static Auid fromBytes(std::span<const std::uint8_t, 16> bytes);
    /// "0d010101-0101-2300-060e-2b3402060101" (canonical, lower case), and back.
    std::string toString() const;
    static std::optional<Auid> parse(std::string_view text);

    constexpr bool operator==(const Auid&) const = default;
    constexpr auto operator<=>(const Auid&) const = default;
};

enum class TypeKind : std::uint8_t {
    Integer,
    Enum,
    Record,
    FixedArray,
    VarArray,
    Set,
    String,
    Stream,
    Rename,
    ExtEnum,
    Indirect,
    Opaque,
    Character,
    GenericCharacter,
    StrongRef,
    WeakRef,
};

struct EnumElement {
    std::string_view name;
    std::int64_t value = 0;
};
struct RecordMember {
    std::string_view name;
    int type = -1; ///< index into types()
};
struct ExtEnumElement {
    std::string_view name;
    Auid value;
};

struct TypeDef {
    std::string_view name;
    Auid id;
    TypeKind kind = TypeKind::Integer;
    std::uint8_t size = 0;    ///< Integer: bytes; GenericCharacter: bytes per character
    bool isSigned = false;    ///< Integer
    int element = -1;         ///< Enum: its integer type; FixedArray / VarArray / Set / String: the element; Rename: the type
    std::uint32_t count = 0;  ///< FixedArray
    int refClass = -1;        ///< StrongRef / WeakRef: index into classes()
    std::span<const EnumElement> enumElements;
    std::span<const RecordMember> members;
    std::span<const ExtEnumElement> extEnumElements;
    std::span<const Auid> targetPath; ///< WeakRef: the properties leading to the set it refers into
};

struct PropertyDef {
    std::string_view name;
    Auid id;
    std::uint16_t pid = 0; ///< the local identification (tag) objects store the property under
    int type = -1;         ///< index into types()
    bool optional = true;
    bool unique = false;   ///< the object's key in a strong reference set
};

struct ClassDef {
    std::string_view name;
    Auid id;
    int parent = -1; ///< index into classes(); -1 for a root class
    bool concrete = false;
    int firstProperty = 0;
    int propertyCount = 0;

    std::span<const PropertyDef> ownProperties() const;
};

std::span<const TypeDef> types();
std::span<const PropertyDef> properties();
/// classes()[0] is the Root class (the file's root storage), which the MetaDictionary does not list.
std::span<const ClassDef> classes();

/// Lookups (by name, or by identifier); -1 / null when there is none.
int findType(std::string_view name);
int findType(const Auid& id);
int findClass(std::string_view name);
int findClass(const Auid& id);
/// The property `name` of class `classIndex` or one of its ancestors.
const PropertyDef* findProperty(int classIndex, std::string_view name);
const PropertyDef* findProperty(int classIndex, std::uint16_t pid);
/// The property with this identifier, in any class.
const PropertyDef* findPropertyById(const Auid& id);
/// Whether `classIndex` is `ancestorIndex` or derives from it.
bool isA(int classIndex, int ancestorIndex);
/// The fixed byte size of values of a type (integers, enums, records, fixed arrays of those); nullopt for variable ones.
std::optional<std::size_t> fixedSize(int typeIndex);

} // namespace wabcut::interchange::detail::aaf
