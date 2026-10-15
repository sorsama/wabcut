#include "XmlUtil.h"

#include "TextUtil.h"

#include <algorithm>
#include <cstring>

namespace wabcut::interchange::detail {

std::expected<void, Error> parseXml(std::string_view data, pugi::xml_document& doc, std::string_view what) {
    // parse_default without parse_doctype keeps DOCTYPE out of the tree; pugixml never expands entities
    // other than the five predefined ones and numeric references.
    const pugi::xml_parse_result result =
        doc.load_buffer(data.data(), data.size(), pugi::parse_default, pugi::encoding_auto);
    if (!result) {
        const XmlLocator locator(data, {});
        return std::unexpected(parseError(std::string(what) + " is not well-formed XML (line " +
                                          std::to_string(locator.lineOf(result.offset)) +
                                          "): " + result.description()));
    }
    if (!doc.document_element()) {
        return std::unexpected(parseError(std::string(what) + " has no root element"));
    }
    // pugixml passes bytes through unchecked: text that is not UTF-8 (a file from an older Windows editor without an
    // encoding declaration, a numeric reference to a surrogate) is read as Windows-1252, so every imported name is
    // valid UTF-8 and the project saves.
    std::vector<pugi::xml_node> pending{doc.document_element()};
    while (!pending.empty()) {
        pugi::xml_node node = pending.back();
        pending.pop_back();
        if ((node.type() == pugi::node_pcdata || node.type() == pugi::node_cdata) && !isValidUtf8(node.value())) {
            node.set_value(toUtf8Lenient(node.value()).c_str());
        }
        for (pugi::xml_attribute a : node.attributes()) {
            if (!isValidUtf8(a.value())) {
                a.set_value(toUtf8Lenient(a.value()).c_str());
            }
        }
        for (const pugi::xml_node child : node.children()) {
            pending.push_back(child);
        }
    }
    return {};
}

XmlLocator::XmlLocator(std::string_view data, std::string file) : m_file(std::move(file)) {
    m_lineStarts.push_back(0);
    for (std::size_t i = 0; i < data.size(); ++i) {
        if (data[i] == '\n') {
            m_lineStarts.push_back(i + 1);
        }
    }
}

std::int32_t XmlLocator::lineOf(std::ptrdiff_t offset) const {
    if (offset < 0) {
        return 0;
    }
    const auto it =
        std::upper_bound(m_lineStarts.begin(), m_lineStarts.end(), static_cast<std::size_t>(offset));
    return static_cast<std::int32_t>(it - m_lineStarts.begin());
}

SourceLocation XmlLocator::locate(const pugi::xml_node& node) const {
    return {m_file, pathOf(node), lineOf(node.offset_debug())};
}

std::string XmlLocator::pathOf(const pugi::xml_node& node) const {
    constexpr std::size_t kSmallParent = 32; // fewer children: counted each time, as elementPath does
    std::vector<std::string> parts;
    for (pugi::xml_node n = node; n && n.type() == pugi::node_element; n = n.parent()) {
        const pugi::xml_node parent = n.parent();
        Position position;
        if (m_indexedParents.contains(parent.internal_object())) {
            position = m_positions.at(n.internal_object());
        } else {
            std::size_t children = 0;
            for (pugi::xml_node s = parent.first_child(); s && children <= kSmallParent; s = s.next_sibling()) {
                ++children;
            }
            if (children > kSmallParent) {
                // Index every element child of this parent once.
                std::unordered_map<std::string_view, std::size_t> totals;
                std::vector<pugi::xml_node> elements;
                for (pugi::xml_node s = parent.first_child(); s; s = s.next_sibling()) {
                    if (s.type() == pugi::node_element) {
                        m_positions[s.internal_object()].index = ++totals[s.name()];
                        elements.push_back(s);
                    }
                }
                for (const pugi::xml_node& s : elements) {
                    m_positions[s.internal_object()].total = totals[s.name()];
                }
                m_indexedParents.insert(parent.internal_object());
                position = m_positions.at(n.internal_object());
            } else {
                for (pugi::xml_node s = parent.first_child(); s; s = s.next_sibling()) {
                    if (s.type() == pugi::node_element && std::strcmp(s.name(), n.name()) == 0) {
                        ++position.total;
                        if (s == n) {
                            position.index = position.total;
                        }
                    }
                }
            }
        }
        std::string part = n.name();
        if (position.total > 1) {
            part += "[" + std::to_string(position.index) + "]";
        }
        parts.push_back(std::move(part));
    }
    std::string path;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        path += "/" + *it;
    }
    return path;
}

std::string elementPath(const pugi::xml_node& node) {
    std::vector<std::string> parts;
    for (pugi::xml_node n = node; n && n.type() == pugi::node_element; n = n.parent()) {
        std::string part = n.name();
        std::size_t index = 0;
        std::size_t total = 0;
        for (pugi::xml_node s = n.parent().first_child(); s; s = s.next_sibling()) {
            if (s.type() == pugi::node_element && std::strcmp(s.name(), n.name()) == 0) {
                ++total;
                if (s == n) {
                    index = total;
                }
            }
        }
        if (total > 1) {
            part += "[" + std::to_string(index) + "]";
        }
        parts.push_back(std::move(part));
    }
    std::string path;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        path += "/" + *it;
    }
    return path;
}

std::string_view childText(const pugi::xml_node& node, const char* name) {
    return trim(node.child(name).child_value());
}

std::optional<std::int64_t> childInt(const pugi::xml_node& node, const char* name) {
    const pugi::xml_node child = node.child(name);
    if (!child) {
        return std::nullopt;
    }
    return parseInt64(child.child_value());
}

std::optional<double> childDouble(const pugi::xml_node& node, const char* name) {
    const pugi::xml_node child = node.child(name);
    if (!child) {
        return std::nullopt;
    }
    return parseDouble(child.child_value());
}

std::optional<bool> childBool(const pugi::xml_node& node, const char* name) {
    const pugi::xml_node child = node.child(name);
    if (!child) {
        return std::nullopt;
    }
    return parseBool(child.child_value());
}

std::string_view attrText(const pugi::xml_node& node, const char* name) {
    return trim(node.attribute(name).value());
}

std::optional<std::int64_t> attrInt(const pugi::xml_node& node, const char* name) {
    const pugi::xml_attribute a = node.attribute(name);
    if (!a) {
        return std::nullopt;
    }
    return parseInt64(a.value());
}

std::string rootElementName(std::string_view data) {
    // UTF-8 BOM.
    if (data.starts_with("\xEF\xBB\xBF")) {
        data.remove_prefix(3);
    }
    std::size_t i = 0;
    auto skipBlanks = [&] {
        while (i < data.size() && (data[i] == ' ' || data[i] == '\t' || data[i] == '\r' || data[i] == '\n')) {
            ++i;
        }
    };
    for (int guard = 0; guard < 64; ++guard) {
        skipBlanks();
        if (i >= data.size() || data[i] != '<') {
            return {};
        }
        const std::string_view rest = data.substr(i);
        if (rest.starts_with("<?")) {
            const auto end = rest.find("?>");
            if (end == std::string_view::npos) {
                return {};
            }
            i += end + 2;
        } else if (rest.starts_with("<!--")) {
            const auto end = rest.find("-->");
            if (end == std::string_view::npos) {
                return {};
            }
            i += end + 3;
        } else if (rest.starts_with("<!")) {
            // DOCTYPE, possibly with an internal subset in brackets.
            std::size_t j = 2;
            int depth = 0;
            while (j < rest.size() && !(rest[j] == '>' && depth == 0)) {
                if (rest[j] == '[') {
                    ++depth;
                } else if (rest[j] == ']') {
                    --depth;
                }
                ++j;
            }
            if (j >= rest.size()) {
                return {};
            }
            i += j + 1;
        } else {
            std::size_t j = 1;
            while (j < rest.size() && rest[j] != '>' && rest[j] != '/' && rest[j] != ' ' && rest[j] != '\t' &&
                   rest[j] != '\r' && rest[j] != '\n') {
                ++j;
            }
            return std::string(rest.substr(1, j - 1));
        }
    }
    return {};
}

} // namespace wabcut::interchange::detail
