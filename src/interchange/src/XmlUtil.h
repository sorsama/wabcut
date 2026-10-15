#pragma once

// pugixml helpers for the XML importers: parsing with line-numbered errors, element paths and lines for
// report locations, and strict typed reads of child elements.

#include <wabcut/interchange/ImportReport.h>
#include <wabcut/model/Error.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <pugixml.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wabcut::interchange::detail {

/// Parses `data` into `doc`. Errors carry the 1-based line of the problem. DOCTYPE content is skipped
/// (no entity expansion), so hostile documents cannot expand.
std::expected<void, Error> parseXml(std::string_view data, pugi::xml_document& doc, std::string_view what);

/// Maps nodes back to the source: "/xmeml/sequence/media/video/track[2]/clipitem[3]" plus the line.
class XmlLocator {
public:
    XmlLocator(std::string_view data, std::string file);

    SourceLocation locate(const pugi::xml_node& node) const;
    std::int32_t lineOf(std::ptrdiff_t offset) const;
    const std::string& file() const { return m_file; }

private:
    /// elementPath with the sibling positions of large parents cached (X.1: a track of 25,000 clipitems would make
    /// every locate count its siblings again).
    std::string pathOf(const pugi::xml_node& node) const;
    struct Position {
        std::size_t index = 0; ///< 1-based among the same-named siblings
        std::size_t total = 0; ///< same-named siblings, itself included
    };
    std::vector<std::size_t> m_lineStarts;
    std::string m_file;
    mutable std::unordered_map<const pugi::xml_node_struct*, Position> m_positions;
    mutable std::unordered_set<const pugi::xml_node_struct*> m_indexedParents;
};

/// Element path; an index is added for an element that has same-named siblings.
std::string elementPath(const pugi::xml_node& node);

/// Trimmed text of the named child element ("" when missing).
std::string_view childText(const pugi::xml_node& node, const char* name);
/// Strict integer/bool/double reads of a child element; nullopt when missing or malformed.
std::optional<std::int64_t> childInt(const pugi::xml_node& node, const char* name);
std::optional<double> childDouble(const pugi::xml_node& node, const char* name);
std::optional<bool> childBool(const pugi::xml_node& node, const char* name);
/// Strict reads of an attribute.
std::string_view attrText(const pugi::xml_node& node, const char* name);
std::optional<std::int64_t> attrInt(const pugi::xml_node& node, const char* name);

/// Name of the document element of `data` without building a DOM (skips the XML declaration, comments,
/// processing instructions and DOCTYPE). Empty when `data` does not start like XML.
std::string rootElementName(std::string_view data);

} // namespace wabcut::interchange::detail
