#pragma once

// Reading AAF files back (the writer's tests and round trips): the compound file's object tree with every property's
// stored bytes, strong references resolved into child objects. Names come from the baseline model (AafModel.h);
// properties it does not know keep their pid. The input is untrusted: nesting, object counts and sizes are bounded and
// every index stream is checked against what it lists.

#include "AafModel.h"
#include "Cfb.h"

#include <wabcut/model/Error.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::interchange::detail::aaf {

inline constexpr std::size_t kMaxReadDepth = 64;
inline constexpr std::size_t kMaxReadObjects = 2'000'000;

struct ReadObject {
    struct Property {
        std::uint16_t pid = 0;
        std::uint16_t form = 0;
        std::string data;                    ///< the value as stored (strong references: the child's storage name)
        std::vector<ReadObject> objects;     ///< strong reference (one) / vector / set elements, in index order
        std::vector<std::string> keys;       ///< set keys / weak reference keys
        std::uint16_t weakIndex = 0;         ///< weak references: index into ReadFile::weakPaths
        std::uint16_t keyPid = 0;
        std::string stream;                  ///< stream properties: the stream's bytes
    };

    Auid classId;
    int classIndex = -1; ///< in the model; -1 when the class is not a baseline one
    std::string storageName;
    std::vector<Property> properties;

    const Property* find(std::string_view name) const;
    const Property* find(std::uint16_t pid) const;
    std::string_view className() const;

    std::optional<std::string> string(std::string_view name) const; ///< UTF-16LE string -> UTF-8
    std::optional<std::int64_t> integer(std::string_view name) const; ///< by the property's type (signed or not)
    std::optional<Auid> auid(std::string_view name) const;
    std::optional<std::array<std::uint8_t, 32>> mobId(std::string_view name) const;
    std::optional<std::pair<std::int32_t, std::int32_t>> rational(std::string_view name) const;
    /// A weak reference's key, as an AUID.
    std::optional<Auid> weakKey(std::string_view name) const;
    /// An indirect value: its type and value bytes.
    std::optional<std::pair<Auid, std::string>> indirect(std::string_view name) const;
    const ReadObject* child(std::string_view name) const;
    std::vector<const ReadObject*> children(std::string_view name) const;
};

struct ReadFile {
    cfb::Clsid headerClsid{};
    ReadObject root;
    std::vector<std::vector<std::uint16_t>> weakPaths; ///< "referenced properties"
    std::size_t objectCount = 0;
};

/// Parses `bytes`. Errors: ParseError.
std::expected<ReadFile, Error> readAaf(std::string bytes);

/// UTF-16LE (with or without a terminating NUL) -> UTF-8.
std::string utf8FromUtf16le(std::string_view bytes);

} // namespace wabcut::interchange::detail::aaf
