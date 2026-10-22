// XMP of media files (Xmp.h, 0.2 P.3).

#include <wabcut/interchange/Xmp.h>

#include "XmlUtil.h"

#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>

#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <system_error>

namespace wabcut::interchange {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kRdf = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";
constexpr std::string_view kXmlNs = "http://www.w3.org/XML/1998/namespace";
constexpr std::string_view kDc = "http://purl.org/dc/elements/1.1/";
constexpr std::string_view kXmp = "http://ns.adobe.com/xap/1.0/";
constexpr std::string_view kXmpDm = "http://ns.adobe.com/xmp/1.0/DynamicMedia/";

struct Schema {
    std::string_view ns;
    std::string_view prefix;
    std::string_view group;
};
constexpr std::array<Schema, 12> kSchemas{{
    {kDc, "dc", "Dublin Core"},
    {kXmp, "xmp", "Basic"},
    {kXmpDm, "xmpDM", "Dynamic Media"},
    {"http://ns.adobe.com/xap/1.0/rights/", "xmpRights", "Rights Management"},
    {"http://ns.adobe.com/xap/1.0/mm/", "xmpMM", "Media Management"},
    {"http://ns.adobe.com/photoshop/1.0/", "photoshop", "Photoshop"},
    {"http://ns.adobe.com/tiff/1.0/", "tiff", "TIFF"},
    {"http://ns.adobe.com/exif/1.0/", "exif", "EXIF"},
    {"http://ns.adobe.com/exif/1.0/aux/", "aux", "EXIF Auxiliary"},
    {"http://ns.adobe.com/camera-raw-settings/1.0/", "crs", "Camera Raw"},
    {"http://ns.adobe.com/creatorAtom/1.0/", "creatorAtom", "Creator Atom"},
    {"http://ns.adobe.com/xap/1.0/bj/", "xmpBJ", "Basic Job Ticket"},
}};

struct Label {
    std::string_view ns;
    std::string_view name;
    std::string_view label;
};
constexpr std::array<Label, 45> kLabels{{
    {kDc, "title", "Title"},
    {kDc, "creator", "Creator"},
    {kDc, "description", "Description"},
    {kDc, "subject", "Keywords"},
    {kDc, "rights", "Copyright"},
    {kDc, "format", "Format"},
    {kDc, "publisher", "Publisher"},
    {kDc, "contributor", "Contributor"},
    {kDc, "date", "Date"},
    {kDc, "language", "Language"},
    {kXmp, "CreatorTool", "Creator Tool"},
    {kXmp, "CreateDate", "Create Date"},
    {kXmp, "ModifyDate", "Modify Date"},
    {kXmp, "MetadataDate", "Metadata Date"},
    {kXmp, "Label", "Label"},
    {kXmp, "Rating", "Rating"},
    {kXmp, "Nickname", "Nickname"},
    {kXmpDm, "scene", "Scene"},
    {kXmpDm, "shotName", "Shot Name"},
    {kXmpDm, "takeNumber", "Take Number"},
    {kXmpDm, "logComment", "Log Comment"},
    {kXmpDm, "good", "Good"},
    {kXmpDm, "tapeName", "Tape Name"},
    {kXmpDm, "altTapeName", "Alternative Tape Name"},
    {kXmpDm, "shotDate", "Shot Date"},
    {kXmpDm, "shotDay", "Shot Day"},
    {kXmpDm, "shotLocation", "Shot Location"},
    {kXmpDm, "shotNumber", "Shot Number"},
    {kXmpDm, "shotSize", "Shot Size"},
    {kXmpDm, "cameraAngle", "Camera Angle"},
    {kXmpDm, "cameraLabel", "Camera Label"},
    {kXmpDm, "cameraModel", "Camera Model"},
    {kXmpDm, "cameraMove", "Camera Move"},
    {kXmpDm, "director", "Director"},
    {kXmpDm, "directorPhotography", "Director of Photography"},
    {kXmpDm, "client", "Client"},
    {kXmpDm, "comment", "Comment"},
    {kXmpDm, "artist", "Artist"},
    {kXmpDm, "album", "Album"},
    {kXmpDm, "genre", "Genre"},
    {kXmpDm, "composer", "Composer"},
    {kXmpDm, "engineer", "Engineer"},
    {kXmpDm, "projectName", "Project Name"},
    {kXmpDm, "videoFrameRate", "Video Frame Rate"},
    {kXmpDm, "audioSampleRate", "Audio Sample Rate"},
}};

const Schema* schemaOf(std::string_view ns) {
    for (const Schema& s : kSchemas) {
        if (s.ns == ns) {
            return &s;
        }
    }
    return nullptr;
}

std::string labelOf(std::string_view ns, std::string_view name) {
    for (const Label& l : kLabels) {
        if (l.ns == ns && l.name == name) {
            return std::string(l.label);
        }
    }
    return std::string(name);
}

/// The prefix and local name of a qualified XML name.
std::pair<std::string_view, std::string_view> splitName(std::string_view qname) {
    const auto colon = qname.find(':');
    if (colon == std::string_view::npos) {
        return {{}, qname};
    }
    return {qname.substr(0, colon), qname.substr(colon + 1)};
}

/// The namespace URI `prefix` is bound to at `node` (walking up the xmlns declarations).
std::string namespaceOf(pugi::xml_node node, std::string_view prefix) {
    const std::string attribute = prefix.empty() ? std::string("xmlns") : "xmlns:" + std::string(prefix);
    for (pugi::xml_node n = node; n; n = n.parent()) {
        if (n.type() != pugi::node_element) {
            continue;
        }
        if (const pugi::xml_attribute a = n.attribute(attribute.c_str())) {
            return a.value();
        }
    }
    if (prefix == "xml") {
        return std::string(kXmlNs);
    }
    return {};
}

bool isRdf(pugi::xml_node node, std::string_view local) {
    const auto [prefix, name] = splitName(node.name());
    return name == local && namespaceOf(node, prefix) == kRdf;
}

std::string trimmed(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

class PacketReader {
public:
    std::vector<XmpProperty> properties;

    void readDescription(pugi::xml_node description) {
        for (const pugi::xml_attribute a : description.attributes()) {
            const auto [prefix, name] = splitName(a.name());
            if (prefix == "xmlns" || std::string_view(a.name()) == "xmlns" || prefix == "xml") {
                continue;
            }
            const std::string ns = namespaceOf(description, prefix);
            if (ns == kRdf || ns.empty()) {
                continue;
            }
            add(ns, prefix, std::string(name), a.value());
        }
        for (const pugi::xml_node child : description.children()) {
            if (child.type() == pugi::node_element) {
                readProperty(child, {});
            }
        }
    }

private:
    void add(const std::string& ns, std::string_view filePrefix, std::string name, std::string_view value) {
        if (properties.size() >= kMaxXmpProperties) {
            return;
        }
        const Schema* schema = schemaOf(ns);
        XmpProperty p;
        p.ns = ns;
        p.prefix = schema ? std::string(schema->prefix) : std::string(filePrefix);
        p.label = name.find('/') == std::string::npos ? labelOf(ns, name) : name;
        p.name = std::move(name);
        p.group = schema ? std::string(schema->group) : p.prefix;
        p.value = trimmed(value);
        properties.push_back(std::move(p));
    }

    /// One property element: simple text, an rdf:Alt / Bag / Seq, or a structure (rdf:parseType="Resource" or a nested
    /// rdf:Description), read one level deep.
    void readProperty(pugi::xml_node element, const std::string& parentName) {
        const auto [prefix, local] = splitName(element.name());
        const std::string ns = namespaceOf(element, prefix);
        if (ns.empty() || ns == kRdf) {
            return;
        }
        const std::string name = parentName.empty() ? std::string(local) : parentName + "/" + std::string(local);
        // rdf:resource="..." is a URI value.
        if (const pugi::xml_attribute resource = element.attribute("rdf:resource")) {
            add(ns, prefix, name, resource.value());
            return;
        }
        pugi::xml_node container;
        for (const pugi::xml_node c : element.children()) {
            if (c.type() == pugi::node_element) {
                container = c;
                break;
            }
        }
        const bool resource = std::string_view(element.attribute("rdf:parseType").value()) == "Resource";
        if (!container) {
            add(ns, prefix, name, element.text().get());
            return;
        }
        if (isRdf(container, "Alt") || isRdf(container, "Bag") || isRdf(container, "Seq")) {
            std::string joined;
            std::string fallback;
            bool alt = isRdf(container, "Alt");
            for (const pugi::xml_node li : container.children()) {
                if (li.type() != pugi::node_element || !isRdf(li, "li")) {
                    continue;
                }
                const std::string text = trimmed(li.text().get());
                if (alt) {
                    const std::string_view lang = li.attribute("xml:lang").value();
                    if (lang == "x-default") {
                        joined = text;
                        break;
                    }
                    if (fallback.empty()) {
                        fallback = text;
                    }
                    continue;
                }
                if (text.empty()) {
                    continue; // a bag of structures: not shown
                }
                if (!joined.empty()) {
                    joined += ", ";
                }
                joined += text;
            }
            add(ns, prefix, name, alt && joined.empty() ? fallback : joined);
            return;
        }
        if (!parentName.empty()) {
            return; // structures in structures: not shown
        }
        pugi::xml_node fields = element;
        if (!resource && isRdf(container, "Description")) {
            fields = container;
            for (const pugi::xml_attribute a : container.attributes()) {
                const auto [ap, an] = splitName(a.name());
                const std::string ans = namespaceOf(container, ap);
                if (ap == "xmlns" || ans.empty() || ans == kRdf) {
                    continue;
                }
                add(ans, ap, name + "/" + std::string(an), a.value());
            }
        }
        for (const pugi::xml_node c : fields.children()) {
            if (c.type() == pugi::node_element) {
                readProperty(c, name);
            }
        }
    }
};

std::expected<std::string, Error> readBytes(const fs::path& file, std::uint64_t offset, std::size_t count) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot read " + pathToUtf8(file)});
    }
    in.seekg(static_cast<std::streamoff>(offset));
    std::string out(count, '\0');
    in.read(out.data(), static_cast<std::streamsize>(count));
    out.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, in.gcount())));
    return out;
}

std::uint64_t be(const unsigned char* p, int bytes) {
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) {
        v = (v << 8) | p[i];
    }
    return v;
}

constexpr std::array<unsigned char, 16> kXmpUuid{0xBE, 0x7A, 0xCF, 0xCB, 0x97, 0xA9, 0x42, 0xE8,
                                                 0x9C, 0x71, 0x99, 0x94, 0x91, 0xE3, 0xAF, 0xAC};

/// ISO-BMFF (MP4 / QuickTime): the payload of Adobe's XMP 'uuid' box at the top level or of moov/udta/'XMP_', walking
/// box headers only. nullopt when the file is not ISO-BMFF or has no such box.
std::optional<std::string> isoBmffXmp(const fs::path& file, std::uint64_t fileSize) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    const auto header = [&](std::uint64_t at, std::uint64_t end, std::uint64_t& size, std::string& type,
                            std::uint64_t& body) {
        if (at + 8 > end) {
            return false;
        }
        unsigned char h[16];
        in.clear();
        in.seekg(static_cast<std::streamoff>(at));
        in.read(reinterpret_cast<char*>(h), 8);
        if (in.gcount() != 8) {
            return false;
        }
        size = be(h, 4);
        type.assign(reinterpret_cast<const char*>(h + 4), 4);
        body = at + 8;
        if (size == 1) {
            in.read(reinterpret_cast<char*>(h + 8), 8);
            if (in.gcount() != 8) {
                return false;
            }
            size = be(h + 8, 8);
            body = at + 16;
        } else if (size == 0) {
            size = end - at;
        }
        return size >= body - at && at + size <= end;
    };
    const auto payload = [&](std::uint64_t from, std::uint64_t to) -> std::optional<std::string> {
        if (to <= from || to - from > kXmpScanBytes) {
            return std::nullopt;
        }
        auto bytes = readBytes(file, from, static_cast<std::size_t>(to - from));
        return bytes ? std::optional<std::string>(std::move(*bytes)) : std::nullopt;
    };
    // The first box must look like ISO-BMFF.
    {
        std::uint64_t size = 0;
        std::uint64_t body = 0;
        std::string type;
        if (!header(0, fileSize, size, type, body) ||
            (type != "ftyp" && type != "moov" && type != "mdat" && type != "wide" && type != "free" && type != "skip" &&
             type != "uuid")) {
            return std::nullopt;
        }
    }
    const std::function<std::optional<std::string>(std::uint64_t, std::uint64_t, int)> walk =
        [&](std::uint64_t at, std::uint64_t end, int depth) -> std::optional<std::string> {
        for (int boxes = 0; at < end && boxes < 4096; ++boxes) {
            std::uint64_t size = 0;
            std::uint64_t body = 0;
            std::string type;
            if (!header(at, end, size, type, body)) {
                return std::nullopt;
            }
            if (type == "uuid" && depth == 0 && size >= body - at + 16) {
                unsigned char id[16];
                in.clear();
                in.seekg(static_cast<std::streamoff>(body));
                in.read(reinterpret_cast<char*>(id), 16);
                if (in.gcount() == 16 && std::equal(id, id + 16, kXmpUuid.begin())) {
                    return payload(body + 16, at + size);
                }
            } else if (type == "XMP_" && depth > 0) {
                return payload(body, at + size);
            } else if ((type == "moov" && depth == 0) || (type == "udta" && depth == 1)) {
                if (auto found = walk(body, at + size, depth + 1)) {
                    return found;
                }
            }
            at += size;
        }
        return std::nullopt;
    };
    return walk(0, fileSize, 0);
}

bool isDigits(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

/// The properties Wabcut writes (Premiere's links), in writing order: namespace, local name, value ("" = remove), and
/// whether it is a language alternative.
/// How a property is written: simple text, a language alternative, an unordered bag or an ordered sequence.
enum class Form : std::uint8_t { Simple, Alt, Bag, Seq };

Form formOf(std::string_view ns, std::string_view name) {
    if (ns == kDc && (name == "title" || name == "description" || name == "rights")) {
        return Form::Alt;
    }
    if (ns == kDc && name == "subject") {
        return Form::Bag;
    }
    if (ns == kDc && name == "creator") {
        return Form::Seq;
    }
    return Form::Simple;
}

std::vector<XmpEdit> linkedEdits(const MediaItem& item) {
    const ClipMetadata& m = item.metadata;
    const std::string tape = item.timecode ? item.timecode->reelName : std::string();
    const auto dm = [](const char* name, std::string value) {
        return XmpEdit{std::string(kXmpDm), "xmpDM", name, std::move(value)};
    };
    return {
        XmpEdit{std::string(kDc), "dc", "title", item.name},
        XmpEdit{std::string(kDc), "dc", "description", item.description},
        dm("scene", m.scene),
        dm("shotName", m.shot),
        dm("takeNumber", isDigits(m.take) ? m.take : std::string()),
        dm("logComment", m.logNote),
        dm("good", m.good ? std::string("True") : std::string()),
        dm("tapeName", tape),
        dm("client", m.client),
        dm("comment", item.comment),
        dm("cameraAngle", m.cameraAngle),
        dm("cameraLabel", m.cameraLabel),
        dm("cameraModel", m.cameraModel),
        dm("shotDate", m.shotDate),
        dm("shotLocation", m.shotLocation),
    };
}

constexpr std::string_view kXpacketHeader = "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n";
constexpr std::string_view kXpacketTrailer = "<?xpacket end=\"w\"?>\n";

/// The DOM edit both writers share: every edited property removed from every rdf:Description (whatever prefix the file
/// gave it), the ones with a value added to the first, in their form.
std::expected<std::string, Error> applyEdits(std::string_view existing, const std::vector<XmpEdit>& edits) {
    std::string base(existing);
    if (base.empty()) {
        base = std::string(kXpacketHeader) +
               "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"Wabcut\">\n"
               " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
               "  <rdf:Description rdf:about=\"\"/>\n"
               " </rdf:RDF>\n"
               "</x:xmpmeta>\n" +
               std::string(kXpacketTrailer);
    }
    pugi::xml_document doc;
    if (!doc.load_buffer(base.data(), base.size(), pugi::parse_default | pugi::parse_pi | pugi::parse_declaration,
                         pugi::encoding_utf8)) {
        return std::unexpected(Error{ErrorCode::ParseError, "the XMP is not well-formed XML"});
    }
    std::vector<pugi::xml_node> descriptions;
    std::vector<pugi::xml_node> pending;
    for (pugi::xml_node c = doc.last_child(); c; c = c.previous_sibling()) {
        pending.push_back(c);
    }
    pugi::xml_node rdfRoot;
    while (!pending.empty()) {
        const pugi::xml_node node = pending.back();
        pending.pop_back();
        if (node.type() != pugi::node_element) {
            continue;
        }
        if (isRdf(node, "RDF") && !rdfRoot) {
            rdfRoot = node;
        }
        if (isRdf(node, "Description") && isRdf(node.parent(), "RDF")) {
            descriptions.push_back(node);
            continue;
        }
        for (pugi::xml_node c = node.last_child(); c; c = c.previous_sibling()) {
            pending.push_back(c);
        }
    }
    if (!rdfRoot) {
        return std::unexpected(Error{ErrorCode::ParseError, "the XMP has no rdf:RDF"});
    }
    if (descriptions.empty()) {
        pugi::xml_node d = rdfRoot.append_child("rdf:Description");
        d.append_attribute("rdf:about") = "";
        descriptions.push_back(d);
    }
    const auto edited = [&](std::string_view ns, std::string_view name) {
        return std::any_of(edits.begin(), edits.end(), [&](const XmpEdit& e) { return e.ns == ns && e.name == name; });
    };
    for (pugi::xml_node d : descriptions) {
        std::vector<pugi::xml_attribute> attributes;
        for (pugi::xml_attribute a : d.attributes()) {
            const auto [prefix, name] = splitName(a.name());
            if (edited(namespaceOf(d, prefix), name)) {
                attributes.push_back(a);
            }
        }
        for (const pugi::xml_attribute& a : attributes) {
            d.remove_attribute(a);
        }
        std::vector<pugi::xml_node> elements;
        for (pugi::xml_node c : d.children()) {
            if (c.type() != pugi::node_element) {
                continue;
            }
            const auto [prefix, name] = splitName(c.name());
            if (edited(namespaceOf(c, prefix), name)) {
                elements.push_back(c);
            }
        }
        for (const pugi::xml_node& c : elements) {
            d.remove_child(c);
        }
    }
    pugi::xml_node target = descriptions.front();
    std::map<std::string, std::string> prefixes; // namespace -> prefix usable at `target`
    const auto prefixFor = [&](std::string_view ns, std::string_view wanted) {
        if (const auto it = prefixes.find(std::string(ns)); it != prefixes.end()) {
            return it->second;
        }
        std::string prefix = wanted.empty() ? std::string("ns") : std::string(wanted);
        const std::string base = prefix;
        for (int n = 1; n < 100; ++n) {
            const std::string bound = namespaceOf(target, prefix);
            if (bound == ns) {
                break;
            }
            if (bound.empty()) {
                target.append_attribute(("xmlns:" + prefix).c_str()) = std::string(ns).c_str();
                break;
            }
            prefix = base + std::to_string(n);
        }
        prefixes.emplace(std::string(ns), prefix);
        return prefix;
    };
    const std::string rdf = namespaceOf(target, "rdf") == kRdf ? std::string("rdf") : prefixFor(kRdf, "rdf");
    for (const XmpEdit& e : edits) {
        if (e.value.empty() || e.name.empty() || e.ns.empty() || e.name.find('/') != std::string::npos ||
            e.ns.size() > 1024 || e.name.size() > 256) {
            continue;
        }
        const std::string tag = prefixFor(e.ns, e.prefix) + ":" + e.name;
        pugi::xml_node element = target.append_child(tag.c_str());
        switch (formOf(e.ns, e.name)) {
        case Form::Simple:
            element.text().set(e.value.c_str());
            break;
        case Form::Alt: {
            pugi::xml_node li = element.append_child((rdf + ":Alt").c_str()).append_child((rdf + ":li").c_str());
            li.append_attribute("xml:lang") = "x-default";
            li.text().set(e.value.c_str());
            break;
        }
        case Form::Bag:
        case Form::Seq: {
            pugi::xml_node list =
                element.append_child((rdf + (formOf(e.ns, e.name) == Form::Bag ? ":Bag" : ":Seq")).c_str());
            std::size_t start = 0;
            while (start <= e.value.size()) {
                const std::size_t comma = e.value.find(',', start);
                const std::string item = trimmed(std::string_view(e.value).substr(
                    start, comma == std::string::npos ? std::string::npos : comma - start));
                if (!item.empty()) {
                    list.append_child((rdf + ":li").c_str()).text().set(item.c_str());
                }
                if (comma == std::string::npos) {
                    break;
                }
                start = comma + 1;
            }
            break;
        }
        }
    }
    std::ostringstream text;
    doc.save(text, " ", pugi::format_default | pugi::format_no_declaration, pugi::encoding_utf8);
    std::string out = text.str();
    if (std::string_view(base).starts_with("<?xml")) {
        out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" + out;
    }
    return out;
}

} // namespace

std::expected<std::vector<XmpProperty>, Error> parseXmp(std::string_view packet) {
    pugi::xml_document doc;
    if (auto parsed = detail::parseXml(packet, doc, "the XMP packet"); !parsed) {
        return std::unexpected(parsed.error());
    }
    PacketReader reader;
    std::vector<pugi::xml_node> pending{doc.document_element()};
    std::size_t visited = 0;
    while (!pending.empty() && visited++ < 100000) {
        const pugi::xml_node node = pending.back();
        pending.pop_back();
        if (isRdf(node, "Description") && isRdf(node.parent(), "RDF")) {
            reader.readDescription(node);
            continue;
        }
        for (pugi::xml_node c = node.last_child(); c; c = c.previous_sibling()) {
            if (c.type() == pugi::node_element) {
                pending.push_back(c);
            }
        }
    }
    return reader.properties;
}

fs::path xmpSidecarPath(const fs::path& media) {
    fs::path out = media;
    out.replace_extension(".xmp");
    return out;
}

std::optional<std::string_view> findXmpPacket(std::string_view bytes) {
    for (const auto& [opening, closing] : {std::pair<std::string_view, std::string_view>{"<x:xmpmeta", "</x:xmpmeta>"},
                                            std::pair<std::string_view, std::string_view>{"<rdf:RDF", "</rdf:RDF>"}}) {
        const auto start = bytes.find(opening);
        if (start == std::string_view::npos) {
            continue;
        }
        const auto end = bytes.find(closing, start);
        if (end == std::string_view::npos) {
            continue;
        }
        return bytes.substr(start, end + closing.size() - start);
    }
    return std::nullopt;
}

std::expected<std::optional<XmpPacket>, Error> readMediaXmp(const fs::path& media, const XmpReadOptions& options) {
    std::error_code ec;
    // A sidecar wins: "<stem>.xmp", then "<name>.<ext>.xmp".
    fs::path withExtension = media;
    withExtension += ".xmp";
    for (const fs::path& sidecar : {xmpSidecarPath(media), withExtension}) {
        if (sidecar == media || !fs::is_regular_file(sidecar, ec)) {
            continue;
        }
        const auto size = fs::file_size(sidecar, ec);
        if (ec || size > kXmpScanBytes) {
            continue;
        }
        auto bytes = readBytes(sidecar, 0, static_cast<std::size_t>(size));
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        const auto packet = findXmpPacket(*bytes);
        if (!packet) {
            continue;
        }
        auto properties = parseXmp(*packet);
        if (properties) {
            return XmpPacket{XmpSource::Sidecar, sidecar, std::move(*properties)};
        }
    }
    const auto size = fs::file_size(media, ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot read " + pathToUtf8(media)});
    }
    std::optional<std::string> text = isoBmffXmp(media, size);
    if (!text && size > options.scanFilesUpTo) {
        return std::optional<XmpPacket>{};
    }
    if (!text) {
        // Packet scanning: the head and the tail of the file (all of a small one).
        auto head = readBytes(media, 0, static_cast<std::size_t>(std::min<std::uintmax_t>(size, kXmpScanBytes)));
        if (!head) {
            return std::unexpected(head.error());
        }
        if (const auto packet = findXmpPacket(*head)) {
            text = std::string(*packet);
        } else if (size > kXmpScanBytes) {
            const std::uintmax_t from = std::max<std::uintmax_t>(kXmpScanBytes, size - kXmpScanBytes);
            auto tail = readBytes(media, from, static_cast<std::size_t>(size - from));
            if (!tail) {
                return std::unexpected(tail.error());
            }
            if (const auto found = findXmpPacket(*tail)) {
                text = std::string(*found);
            }
        }
    } else if (const auto packet = findXmpPacket(*text)) {
        text = std::string(*packet);
    }
    if (!text) {
        return std::optional<XmpPacket>{};
    }
    auto properties = parseXmp(*text);
    if (!properties) {
        return std::optional<XmpPacket>{}; // a broken packet inside a media file: as if there were none
    }
    return XmpPacket{XmpSource::Embedded, media, std::move(*properties)};
}

XmpClipFields clipFieldsFromXmp(const std::vector<XmpProperty>& properties) {
    XmpClipFields out;
    for (const XmpProperty& p : properties) {
        if (p.ns == kXmpDm) {
            if (p.name == "scene") {
                out.scene = p.value;
            } else if (p.name == "shotName") {
                out.shot = p.value;
            } else if (p.name == "takeNumber") {
                out.take = p.value;
            } else if (p.name == "logComment") {
                out.logNote = p.value;
            } else if (p.name == "client") {
                out.client = p.value;
            } else if (p.name == "good") {
                std::string v = p.value;
                std::transform(v.begin(), v.end(), v.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
                out.good = v == "true" || v == "1";
            } else if (p.name == "tapeName") {
                out.tapeName = p.value;
            } else if (p.name == "comment") {
                out.comment = p.value;
            } else if (p.name == "cameraAngle") {
                out.cameraAngle = p.value;
            } else if (p.name == "cameraLabel") {
                out.cameraLabel = p.value;
            } else if (p.name == "cameraModel") {
                out.cameraModel = p.value;
            } else if (p.name == "shotDate") {
                out.shotDate = p.value;
            } else if (p.name == "shotLocation") {
                out.shotLocation = p.value;
            }
        } else if (p.ns == kDc && p.name == "description") {
            out.description = p.value;
        }
    }
    return out;
}

namespace {

std::string cut(std::string text, std::size_t limit) {
    if (text.size() <= limit) {
        return text;
    }
    std::size_t at = limit;
    while (at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) {
        --at;
    }
    text.resize(at);
    return text;
}

} // namespace

void applyXmpClipFields(MediaItem& item, const XmpClipFields& f, bool overwrite) {
    const auto put = [&](const std::optional<std::string>& value, std::string& into, std::size_t limit) {
        if (value && (overwrite || into.empty())) {
            into = cut(*value, limit);
        }
    };
    put(f.scene, item.metadata.scene, ClipMetadata::kMaxFieldBytes);
    put(f.shot, item.metadata.shot, ClipMetadata::kMaxFieldBytes);
    put(f.take, item.metadata.take, ClipMetadata::kMaxFieldBytes);
    put(f.client, item.metadata.client, ClipMetadata::kMaxFieldBytes);
    put(f.logNote, item.metadata.logNote, MediaItem::kMaxTextBytes);
    put(f.comment, item.comment, MediaItem::kMaxTextBytes);
    put(f.description, item.description, MediaItem::kMaxTextBytes);
    put(f.cameraAngle, item.metadata.cameraAngle, ClipMetadata::kMaxFieldBytes);
    put(f.cameraLabel, item.metadata.cameraLabel, ClipMetadata::kMaxFieldBytes);
    put(f.cameraModel, item.metadata.cameraModel, ClipMetadata::kMaxFieldBytes);
    put(f.shotDate, item.metadata.shotDate, ClipMetadata::kMaxFieldBytes);
    put(f.shotLocation, item.metadata.shotLocation, ClipMetadata::kMaxFieldBytes);
    if (f.good && (overwrite || !item.metadata.good)) {
        item.metadata.good = *f.good;
    }
    if (f.tapeName && !f.tapeName->empty() && (overwrite || !item.timecode || item.timecode->reelName.empty())) {
        if (!item.timecode) {
            item.timecode = MediaTimecode{};
        }
        item.timecode->reelName = cut(*f.tapeName, MediaTimecode::kMaxReelNameBytes);
    }
}

std::string xmpPacketFor(const MediaItem& item) { return editedXmpPacket({}, linkedEdits(item)).value_or(std::string()); }

std::vector<XmpProperty> knownXmpProperties() {
    std::vector<XmpProperty> out;
    for (const Label& l : kLabels) {
        const Schema* s = schemaOf(l.ns);
        XmpProperty p;
        p.ns = std::string(l.ns);
        p.prefix = s ? std::string(s->prefix) : std::string();
        p.name = std::string(l.name);
        p.label = std::string(l.label);
        p.group = s ? std::string(s->group) : p.prefix;
        out.push_back(std::move(p));
    }
    return out;
}

std::vector<XmpEdit> linkedXmpEdits(const MediaItem& item) { return linkedEdits(item); }

std::expected<std::string, Error> editedXmpPacket(std::string_view existing, const std::vector<XmpEdit>& edits) {
    return applyEdits(existing, edits);
}

std::expected<fs::path, Error> editXmpSidecar(const fs::path& media, const std::vector<XmpEdit>& edits,
                                              const fs::path& sidecarIn) {
    const fs::path sidecar = sidecarIn.empty() ? xmpSidecarPath(media) : sidecarIn;
    std::error_code ec;
    std::string existing;
    if (fs::exists(sidecar, ec)) {
        const auto size = fs::file_size(sidecar, ec);
        if (ec || size > kXmpScanBytes) {
            return std::unexpected(Error{ErrorCode::IoError, "cannot read " + pathToUtf8(sidecar)});
        }
        auto bytes = readBytes(sidecar, 0, static_cast<std::size_t>(size));
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        existing = std::move(*bytes);
    }
    auto text = applyEdits(existing, edits);
    if (!text) {
        return std::unexpected(Error{ErrorCode::ParseError, pathToUtf8(sidecar) + ": " + text.error().message});
    }
    if (auto written = io::writeFileAtomically(sidecar, *text); !written) {
        return std::unexpected(written.error());
    }
    return sidecar;
}

std::expected<fs::path, Error> writeXmpSidecar(const MediaItem& item, const fs::path& sidecar) {
    if (item.isGenerator() || item.locator.path.empty()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "'" + item.name + "' has no file to write XMP for"});
    }
    return editXmpSidecar(pathFromUtf8(item.locator.path), linkedEdits(item), sidecar);
}

namespace {

/// One top-level box of an ISO-BMFF file, as written.
struct TopBox {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;   ///< the bytes it spans (to the end of the file for a size-0 box)
    std::uint32_t rawSize = 0; ///< the 32-bit size field (0: to the end, 1: a 64-bit size follows)
    std::string type;
    bool xmp = false;          ///< Adobe's XMP 'uuid' box
};

/// The top-level boxes (nullopt when the file is not ISO-BMFF or a box is malformed), and the offsets of the type
/// fields of moov/udta/'XMP_' boxes.
std::optional<std::vector<TopBox>> topBoxes(const fs::path& file, std::uint64_t fileSize, std::vector<std::uint64_t>* xmpInMoov) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    const auto readHeader = [&](std::uint64_t at, std::uint64_t end, std::uint64_t& size, std::uint32_t& raw,
                                std::string& type, std::uint64_t& body) {
        if (at + 8 > end) {
            return false;
        }
        unsigned char h[16];
        in.clear();
        in.seekg(static_cast<std::streamoff>(at));
        in.read(reinterpret_cast<char*>(h), 8);
        if (in.gcount() != 8) {
            return false;
        }
        raw = static_cast<std::uint32_t>(be(h, 4));
        type.assign(reinterpret_cast<const char*>(h + 4), 4);
        size = raw;
        body = at + 8;
        if (raw == 1) {
            in.read(reinterpret_cast<char*>(h + 8), 8);
            if (in.gcount() != 8) {
                return false;
            }
            size = be(h + 8, 8);
            body = at + 16;
        } else if (raw == 0) {
            size = end - at;
        }
        return size >= body - at && at + size <= end;
    };
    std::vector<TopBox> out;
    std::uint64_t at = 0;
    for (int n = 0; at < fileSize && n < 100000; ++n) {
        TopBox box;
        std::uint64_t body = 0;
        if (!readHeader(at, fileSize, box.size, box.rawSize, box.type, body)) {
            return std::nullopt;
        }
        box.offset = at;
        if (n == 0 && box.type != "ftyp" && box.type != "moov" && box.type != "mdat" && box.type != "wide" &&
            box.type != "free" && box.type != "skip" && box.type != "uuid") {
            return std::nullopt;
        }
        if (box.type == "uuid" && box.size >= body - at + 16) {
            unsigned char id[16];
            in.clear();
            in.seekg(static_cast<std::streamoff>(body));
            in.read(reinterpret_cast<char*>(id), 16);
            box.xmp = in.gcount() == 16 && std::equal(id, id + 16, kXmpUuid.begin());
        }
        if (box.type == "moov" && xmpInMoov) {
            // moov/udta/XMP_ (QuickTime): their type fields.
            std::uint64_t child = body;
            for (int m = 0; child < at + box.size && m < 4096; ++m) {
                std::uint64_t size = 0;
                std::uint64_t childBody = 0;
                std::uint32_t raw = 0;
                std::string type;
                if (!readHeader(child, at + box.size, size, raw, type, childBody)) {
                    break;
                }
                if (type == "udta") {
                    std::uint64_t grand = childBody;
                    for (int k = 0; grand < child + size && k < 4096; ++k) {
                        std::uint64_t gs = 0;
                        std::uint64_t gb = 0;
                        std::uint32_t gr = 0;
                        std::string gt;
                        if (!readHeader(grand, child + size, gs, gr, gt, gb)) {
                            break;
                        }
                        if (gt == "XMP_") {
                            xmpInMoov->push_back(grand + 4);
                        }
                        grand += gs;
                    }
                }
                child += size;
            }
        }
        out.push_back(box);
        at += box.size;
    }
    return out;
}

std::string be32Text(std::uint64_t v) {
    return {static_cast<char>((v >> 24) & 0xFF), static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 8) & 0xFF),
            static_cast<char>(v & 0xFF)};
}

} // namespace

bool canEmbedXmp(const fs::path& media) {
    std::error_code ec;
    const auto size = fs::file_size(media, ec);
    return !ec && topBoxes(media, size, nullptr).has_value();
}

std::expected<void, Error> embedXmp(const fs::path& media, const std::vector<XmpEdit>& edits) {
    std::error_code ec;
    const auto size = fs::file_size(media, ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot read " + pathToUtf8(media)});
    }
    std::vector<std::uint64_t> moovXmp;
    const auto boxes = topBoxes(media, size, &moovXmp);
    if (!boxes || boxes->empty()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "XMP can be written into MP4 and MOV files only"});
    }
    // The packet: the file's own, edited.
    std::string existing;
    if (auto inside = isoBmffXmp(media, size)) {
        if (const auto packet = findXmpPacket(*inside)) {
            existing = std::string(*packet);
        }
    }
    auto edited = applyEdits(existing, edits);
    if (!edited) {
        return std::unexpected(edited.error());
    }
    std::string packet = std::move(*edited);
    if (packet.find("<?xpacket") == std::string::npos) {
        packet = std::string(kXpacketHeader) + packet + std::string(kXpacketTrailer);
    }
    // Padding before the trailer, as Adobe's writers leave it (room for later edits by other tools).
    if (const auto trailer = packet.rfind("<?xpacket end"); trailer != std::string::npos) {
        packet.insert(trailer, std::string(2048, ' ') + "\n");
    }
    const TopBox& last = boxes->back();
    if (last.rawSize == 0 && (last.size > 0xFFFFFFFFull)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument,
                                     pathToUtf8(media.filename()) + " ends with a box of open size too large to close"});
    }

    // The copy, edited, checked, then put in place.
    fs::path temp = media;
    temp += ".wabcut-xmp.tmp";
    fs::remove(temp, ec);
    const auto fail = [&](std::string what) {
        std::error_code ignore;
        fs::remove(temp, ignore);
        return std::unexpected(Error{ErrorCode::IoError, std::move(what)});
    };
    if (!fs::copy_file(media, temp, fs::copy_options::overwrite_existing, ec) || ec) {
        return fail("cannot copy " + pathToUtf8(media.filename()) + " to write its XMP");
    }
    std::uint64_t truncateAt = 0;
    {
        std::fstream io(temp, std::ios::in | std::ios::out | std::ios::binary);
        if (!io) {
            return fail("cannot open the copy of " + pathToUtf8(media.filename()));
        }
        const auto writeAt = [&](std::uint64_t at, const std::string& bytes) {
            io.seekp(static_cast<std::streamoff>(at));
            io.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            return static_cast<bool>(io);
        };
        bool ok = true;
        // A last box of open size (to the end) gets its size written, so the new box does not fall inside it.
        if (last.rawSize == 0) {
            ok = ok && writeAt(last.offset, be32Text(last.size));
        }
        for (const TopBox& b : *boxes) {
            if (!b.xmp) {
                continue;
            }
            if (&b == &last) {
                truncateAt = b.offset; // a trailing old XMP box is cut off
            } else {
                ok = ok && writeAt(b.offset + 4, "free");
            }
        }
        for (const std::uint64_t typeAt : moovXmp) {
            ok = ok && writeAt(typeAt, "free");
        }
        io.flush();
        if (!ok || !io) {
            return fail("cannot write the copy of " + pathToUtf8(media.filename()));
        }
    }
    if (truncateAt > 0) {
        fs::resize_file(temp, truncateAt, ec);
        if (ec) {
            return fail("cannot write the copy of " + pathToUtf8(media.filename()));
        }
    }
    {
        std::ofstream out(temp, std::ios::binary | std::ios::app);
        const std::uint64_t boxSize = 8 + 16 + packet.size();
        const std::string header = be32Text(boxSize) + "uuid" + std::string(reinterpret_cast<const char*>(kXmpUuid.data()), 16);
        out.write(header.data(), static_cast<std::streamsize>(header.size()));
        out.write(packet.data(), static_cast<std::streamsize>(packet.size()));
        out.flush();
        if (!out) {
            return fail("cannot write the copy of " + pathToUtf8(media.filename()));
        }
    }
    // Check the copy: still ISO-BMFF, the media data where it was, and our XMP found first.
    const auto copySize = fs::file_size(temp, ec);
    std::vector<std::uint64_t> copyMoovXmp;
    const std::optional<std::vector<TopBox>> copyBoxes =
        ec ? std::optional<std::vector<TopBox>>{} : topBoxes(temp, copySize, &copyMoovXmp);
    bool same = copyBoxes.has_value() && copyMoovXmp.empty();
    if (same) {
        for (const TopBox& b : *boxes) {
            if (b.type != "mdat" && b.type != "moov") {
                continue;
            }
            same = same && std::any_of(copyBoxes->begin(), copyBoxes->end(), [&](const TopBox& c) {
                       return c.offset == b.offset && c.type == b.type && c.size == b.size;
                   });
        }
    }
    bool parsed = false;
    if (same) {
        if (const std::optional<std::string> readBack = isoBmffXmp(temp, copySize)) {
            const std::optional<std::string_view> found = findXmpPacket(*readBack);
            parsed = found && parseXmp(*found).has_value();
        }
    }
    if (!same || !parsed) {
        return fail("the copy of " + pathToUtf8(media.filename()) + " did not check out; the file was not changed");
    }
    fs::rename(temp, media, ec);
    if (ec) {
        return fail(pathToUtf8(media.filename()) + " could not be replaced (is it open in another program?); it was not changed");
    }
    return {};
}

} // namespace wabcut::interchange
