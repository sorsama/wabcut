#include <wabcut/interchange/Docx.h>

#include <wabcut/interchange/Zip.h>

#include <pugixml.hpp>

#include <array>
#include <utility>

namespace wabcut::interchange {

namespace {

constexpr std::size_t kMaxDocumentXmlBytes = 16u * 1024u * 1024u;

/// `text` escaped for XML character data and attribute values; control characters other than tab are dropped (XML 1.0
/// cannot hold them).
std::string xmlEscaped(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        const auto u = static_cast<unsigned char>(ch);
        switch (ch) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            if (u >= 0x20 || ch == '\t') {
                out.push_back(ch);
            }
        }
    }
    return out;
}

/// A run's text as w:t pieces with w:br between lines and w:tab for tabs.
std::string runContent(std::string_view text) {
    std::string out;
    std::size_t start = 0;
    const auto piece = [&](std::string_view s) {
        std::size_t at = 0;
        while (at <= s.size()) {
            const std::size_t tab = std::min(s.find('\t', at), s.size());
            if (tab > at) {
                out += "<w:t xml:space=\"preserve\">" + xmlEscaped(s.substr(at, tab - at)) + "</w:t>";
            }
            if (tab == s.size()) {
                break;
            }
            out += "<w:tab/>";
            at = tab + 1;
        }
    };
    while (start <= text.size()) {
        const std::size_t nl = std::min(text.find('\n', start), text.size());
        piece(text.substr(start, nl - start));
        if (nl == text.size()) {
            break;
        }
        out += "<w:br/>";
        start = nl + 1;
    }
    return out;
}

void appendText(const pugi::xml_node& node, std::string& out, std::size_t depth) {
    if (depth > 64) {
        return;
    }
    for (const pugi::xml_node& child : node.children()) {
        const std::string_view name = child.name();
        if (name == "w:t") {
            out += child.text().get();
        } else if (name == "w:br" || name == "w:cr") {
            out += '\n';
        } else if (name == "w:tab") {
            out += '\t';
        } else if (name == "w:p") {
            appendText(child, out, depth + 1);
            out += '\n';
        } else if (name == "w:del" || name == "w:instrText" || name == "w:delText") {
            // Tracked deletions and field codes are not the document's text.
        } else {
            appendText(child, out, depth + 1);
        }
    }
}

} // namespace

std::string writeDocx(std::span<const DocxParagraph> paragraphs, std::string_view title) {
    std::string body;
    for (const DocxParagraph& p : paragraphs) {
        body += "<w:p>";
        if (p.spaceAfter > 0) {
            body += "<w:pPr><w:spacing w:after=\"" + std::to_string(p.spaceAfter * 20) + "\"/></w:pPr>";
        }
        for (const DocxRun& r : p.runs) {
            body += "<w:r>";
            if (r.bold || r.muted) {
                body += "<w:rPr>";
                if (r.bold) {
                    body += "<w:b/>";
                }
                if (r.muted) {
                    body += "<w:color w:val=\"6E6E73\"/>";
                }
                body += "</w:rPr>";
            }
            body += runContent(r.text) + "</w:r>";
        }
        body += "</w:p>";
    }
    const std::string document =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body>" +
        body +
        "<w:sectPr><w:pgSz w:w=\"11906\" w:h=\"16838\"/><w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" "
        "w:left=\"1440\" w:header=\"708\" w:footer=\"708\" w:gutter=\"0\"/></w:sectPr></w:body></w:document>";
    const std::string contentTypes =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/word/document.xml\" "
        "ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
        "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>"
        "</Types>";
    const std::string rels =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
        "Target=\"word/document.xml\"/>"
        "<Relationship Id=\"rId2\" "
        "Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" "
        "Target=\"docProps/core.xml\"/>"
        "</Relationships>";
    const std::string documentRels =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"></Relationships>";
    const std::string core =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>" +
        xmlEscaped(title) + "</dc:title><dc:creator>Wabcut</dc:creator></cp:coreProperties>";
    const std::array<std::pair<std::string, std::string>, 5> files{{
        {"[Content_Types].xml", contentTypes},
        {"_rels/.rels", rels},
        {"word/document.xml", document},
        {"word/_rels/document.xml.rels", documentRels},
        {"docProps/core.xml", core},
    }};
    return writeStoredZip(files);
}

std::expected<std::string, Error> readDocxText(std::string_view data) {
    ZipLimits limits;
    limits.maxEntryBytes = kMaxDocumentXmlBytes;
    limits.maxTotalBytes = kMaxDocumentXmlBytes;
    auto archive = ZipArchive::open(data, limits);
    if (!archive) {
        return std::unexpected(Error{ErrorCode::ParseError, "not a Word document: " + archive.error().message});
    }
    const ZipEntry* entry = archive->find("word/document.xml");
    if (!entry) {
        return std::unexpected(Error{ErrorCode::ParseError, "not a Word document (no word/document.xml)"});
    }
    auto xml = archive->read(*entry);
    if (!xml) {
        return std::unexpected(xml.error());
    }
    pugi::xml_document doc;
    // No DOCTYPE or entities are honoured beyond XML's own (pugixml never loads external ones).
    const pugi::xml_parse_result parsed =
        doc.load_buffer(xml->data(), xml->size(), pugi::parse_default & ~pugi::parse_doctype, pugi::encoding_utf8);
    if (!parsed) {
        return std::unexpected(Error{ErrorCode::ParseError, std::string("the document's XML is damaged: ") + parsed.description()});
    }
    const pugi::xml_node body = doc.child("w:document").child("w:body");
    if (!body) {
        return std::unexpected(Error{ErrorCode::ParseError, "not a Word document (no w:body)"});
    }
    std::string out;
    appendText(body, out, 0);
    return out;
}

} // namespace wabcut::interchange
