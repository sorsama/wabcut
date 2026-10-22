#pragma once

// The Bento container format (Apple, Bento Specification 1.0d5, 1993) as OMF Interchange 2.0 files use it: a file of
// value data followed by a table of contents (the TOC: every value of every object, by object, property and type ID)
// and a 24-byte label at the very end that locates the TOC.
//
//   [value data ...][TOC, in 1024-byte blocks][label: magic A4 43 4D A5 48 64 72 D7, flags, block size (KiB),
//                                              major 2, minor 0, TOC offset, TOC size]
//
// All of it is big-endian (the TOC's fixed encoding and, in OMF files whose Header says 'MM', the values).
// Objects 1..0xFFFF are Bento's standard objects (object 1 is the TOC itself; in OMF also the Header); properties and
// types are objects too, named by a global name string. A value of an OMF object reference type carries a
// "reference list" (a bookkeeping object of key/object ID pairs) that resolves its 4-byte keys.
//
// The writer emits what the OMF Toolkit's Bento library reads (and what other OMF writers do): every object's entries
// sorted by property, objects in ID order, values of up to 4 bytes inline in the TOC, no object split across TOC
// blocks when it fits in one. The reader is for tests and takes untrusted files (sizes and counts bounded).

#include "Cfb.h"

#include <wabcut/model/Error.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::interchange::detail::bento {

/// Bento's standard objects (as the Bento library numbers them).
inline constexpr std::uint32_t kTocObject = 1;           ///< the TOC (OMF: also the Header object)
inline constexpr std::uint32_t kPropTocSeed = 2;         ///< next free object ID
inline constexpr std::uint32_t kPropTocMinSeed = 3;      ///< first non-standard ID (0x10000)
inline constexpr std::uint32_t kPropTocObject = 4;       ///< the TOC's own offset and size
inline constexpr std::uint32_t kPropTocContainer = 5;    ///< the whole container: offset 0, size
inline constexpr std::uint32_t kPropTocDeleted = 6;      ///< amount deleted (0)
inline constexpr std::uint32_t kTypeTocId = 0x13;        ///< TOC values' type
inline constexpr std::uint32_t kType7BitAscii = 0x15;    ///< global names
inline constexpr std::uint32_t kPropGlobalTypeName = 0x17;
inline constexpr std::uint32_t kPropGlobalPropName = 0x18;
inline constexpr std::uint32_t kPropObjReferences = 0x1F; ///< a reference list's pairs
inline constexpr std::uint32_t kTypeObjRefData = 0x20;
inline constexpr std::uint32_t kFirstUserId = 0x10000;
inline constexpr std::uint32_t kTocBlockSize = 1024;

/// One value of a property: inline bytes, or bytes copied from a file when written (embedded media).
struct Value {
    std::uint32_t type = 0;
    cfb::StreamSource data;
    std::uint32_t referenceList = 0; ///< object ID of the value's reference list, 0: none
};

class Builder {
public:
    /// Where write() / toBytes() put the bytes.
    class Sink;

    Builder();

    /// A new object ID (0x10000 and up, in order).
    std::uint32_t newObject() { return m_nextId++; }
    std::uint32_t nextId() const { return m_nextId; }

    /// The object of a global type or property name ("omfi:Int32", "OMFI:CPNT:Length"), declared on first use.
    std::uint32_t typeId(std::string_view name) { return named(name, kPropGlobalTypeName); }
    std::uint32_t propertyId(std::string_view name) { return named(name, kPropGlobalPropName); }

    /// Adds a value to property `property` of `object` (one value per type and property).
    void set(std::uint32_t object, std::uint32_t property, std::uint32_t type, std::string bytes);
    void setStream(std::uint32_t object, std::uint32_t property, std::uint32_t type, cfb::StreamSource data);
    /// A reference value (`bytes`: the keys the value holds) with its reference list for `targets` (key = object ID).
    void setReference(std::uint32_t object, std::uint32_t property, std::uint32_t type, std::string bytes,
                      const std::vector<std::uint32_t>& targets);

    /// Size of the container write() makes (bytes), and the container itself. Errors: IoError, Unsupported (over
    /// 2 GiB, what OMF readers take; Bento offsets are 32-bit).
    std::expected<std::uint64_t, Error> write(const std::filesystem::path& file) const;
    std::expected<std::string, Error> toBytes() const;

    using Properties = std::map<std::uint32_t, std::vector<Value>>;
    const std::map<std::uint32_t, Properties>& objects() const { return m_objects; }

private:
    std::uint32_t named(std::string_view name, std::uint32_t property);
    std::expected<std::uint64_t, Error> emit(Sink& sink) const;

    std::uint32_t m_nextId = kFirstUserId;
    std::map<std::string, std::uint32_t, std::less<>> m_names;
    std::map<std::uint32_t, Properties> m_objects;
};

// --- Reading ----------------------------------------------------------------------------------------------------

inline constexpr std::size_t kMaxTocBytes = 64u * 1024u * 1024u;
inline constexpr std::size_t kMaxValues = 4'000'000;

struct ReadValue {
    std::uint32_t type = 0;
    std::string data;                ///< the value's bytes (segments joined); empty for values not loaded
    std::uint64_t offset = 0;        ///< of the first segment, when not immediate
    std::uint64_t size = 0;
    bool immediate = false;
    std::uint32_t referenceList = 0;
};

struct Container {
    std::uint16_t majorVersion = 0;
    std::uint16_t minorVersion = 0;
    std::uint32_t blockSize = 0;     ///< bytes
    std::map<std::uint32_t, std::map<std::uint32_t, std::vector<ReadValue>>> objects;
    std::map<std::uint32_t, std::string> names; ///< type and property object -> global name

    const ReadValue* value(std::uint32_t object, std::uint32_t property) const;
    /// The property of `object` named `name`, if it has one.
    const ReadValue* value(std::uint32_t object, std::string_view name) const;
    /// The object a reference key of `value` stands for (via its reference list), if any.
    std::optional<std::uint32_t> resolve(const ReadValue& value, std::uint32_t key) const;
};

/// Parses a Bento container (label, TOC, value data up to `maxValueBytes` each; larger ones keep offset and size
/// only). Errors: ParseError.
std::expected<Container, Error> read(std::string_view bytes, std::size_t maxValueBytes = 16u * 1024u * 1024u);

} // namespace wabcut::interchange::detail::bento
