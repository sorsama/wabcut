#include <wabcut/color/Cdl.h>

#include <wabcut/model/ProjectIO.h>

#include <pugixml.hpp>

#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>

namespace wabcut::color {

namespace {

std::string_view localName(const char* name) {
    std::string_view n(name);
    if (const auto colon = n.find(':'); colon != std::string_view::npos) {
        n.remove_prefix(colon + 1);
    }
    return n;
}

pugi::xml_node child(const pugi::xml_node& node, std::string_view name) {
    for (pugi::xml_node c = node.first_child(); c; c = c.next_sibling()) {
        if (c.type() == pugi::node_element && localName(c.name()) == name) {
            return c;
        }
    }
    return {};
}

std::expected<std::vector<double>, Error> numbers(const pugi::xml_node& node, std::size_t count, std::string_view what) {
    std::vector<double> out;
    std::string_view text = node.text().get();
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
            ++i;
        }
        if (i >= text.size()) {
            break;
        }
        std::size_t j = i;
        while (j < text.size() && !std::isspace(static_cast<unsigned char>(text[j]))) {
            ++j;
        }
        std::string_view token = text.substr(i, j - i);
        if (!token.empty() && token.front() == '+') {
            token.remove_prefix(1);
        }
        double v = 0.0;
        const auto [end, ec] = std::from_chars(token.data(), token.data() + token.size(), v);
        if (ec != std::errc() || end != token.data() + token.size() || !std::isfinite(v)) {
            return std::unexpected(Error{ErrorCode::ParseError, std::format("{} has a value that is not a number", what)});
        }
        out.push_back(v);
        i = j;
    }
    if (out.size() != count) {
        return std::unexpected(Error{ErrorCode::ParseError, std::format("{} needs {} numbers", what, count)});
    }
    return out;
}

std::expected<Cdl, Error> readCorrection(const pugi::xml_node& cc) {
    Cdl cdl;
    cdl.id = cc.attribute("id").as_string();
    const pugi::xml_node sop = child(cc, "SOPNode");
    if (sop) {
        if (const pugi::xml_node d = child(sop, "Description")) {
            cdl.description = d.text().get();
        }
        const auto read3 = [&](std::string_view name, Rgb& out) -> std::expected<void, Error> {
            const pugi::xml_node n = child(sop, name);
            if (!n) {
                return {};
            }
            const auto v = numbers(n, 3, name);
            if (!v) {
                return std::unexpected(v.error());
            }
            out = {(*v)[0], (*v)[1], (*v)[2]};
            return {};
        };
        if (auto ok = read3("Slope", cdl.slope); !ok) {
            return std::unexpected(ok.error());
        }
        if (auto ok = read3("Offset", cdl.offset); !ok) {
            return std::unexpected(ok.error());
        }
        if (auto ok = read3("Power", cdl.power); !ok) {
            return std::unexpected(ok.error());
        }
    }
    pugi::xml_node sat = child(cc, "SatNode");
    if (!sat) {
        sat = child(cc, "SATNode");
    }
    if (sat) {
        if (const pugi::xml_node s = child(sat, "Saturation")) {
            const auto v = numbers(s, 1, "Saturation");
            if (!v) {
                return std::unexpected(v.error());
            }
            cdl.saturation = (*v)[0];
        }
    }
    for (std::size_t c = 0; c < 3; ++c) {
        if (cdl.slope[c] < 0.0 || cdl.power[c] <= 0.0) {
            return std::unexpected(Error{ErrorCode::ParseError, "a CDL slope must not be negative and its power must be above 0"});
        }
    }
    if (cdl.saturation < 0.0) {
        return std::unexpected(Error{ErrorCode::ParseError, "a CDL saturation must not be negative"});
    }
    return cdl;
}

void collect(const pugi::xml_node& node, std::vector<pugi::xml_node>& out, int depth) {
    if (depth > 8) {
        return;
    }
    for (pugi::xml_node c = node.first_child(); c; c = c.next_sibling()) {
        if (c.type() != pugi::node_element) {
            continue;
        }
        if (localName(c.name()) == "ColorCorrection") {
            out.push_back(c);
        } else {
            collect(c, out, depth + 1);
        }
    }
}

std::string escape(std::string_view s) {
    std::string out;
    for (const char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

/// The usual six decimals when they read back exactly, else the shortest text that does: a tiny power (1e-7) was
/// written as 0.000000, which no CDL reader accepts as a power (C.10 fuzzing).
std::string decimal(double v) {
    std::string fixed = std::format("{:.6f}", v);
    double back = 0.0;
    const auto [end, ec] = std::from_chars(fixed.data(), fixed.data() + fixed.size(), back);
    if (ec == std::errc() && end == fixed.data() + fixed.size() && back == v) {
        return fixed;
    }
    return std::format("{}", v);
}

std::string triple(const Rgb& v) { return decimal(v[0]) + " " + decimal(v[1]) + " " + decimal(v[2]); }

void writeCorrection(std::ostringstream& out, const Cdl& cdl, const std::string& indent) {
    out << indent << "<ColorCorrection" << (cdl.id.empty() ? std::string() : " id=\"" + escape(cdl.id) + "\"") << ">\n";
    out << indent << "  <SOPNode>\n";
    if (!cdl.description.empty()) {
        out << indent << "    <Description>" << escape(cdl.description) << "</Description>\n";
    }
    out << indent << "    <Slope>" << triple(cdl.slope) << "</Slope>\n";
    out << indent << "    <Offset>" << triple(cdl.offset) << "</Offset>\n";
    out << indent << "    <Power>" << triple(cdl.power) << "</Power>\n";
    out << indent << "  </SOPNode>\n";
    out << indent << "  <SatNode>\n";
    out << indent << "    <Saturation>" << decimal(cdl.saturation) << "</Saturation>\n";
    out << indent << "  </SatNode>\n";
    out << indent << "</ColorCorrection>\n";
}

} // namespace

bool Cdl::isIdentity() const {
    return slope == Rgb{1, 1, 1} && offset == Rgb{0, 0, 0} && power == Rgb{1, 1, 1} && saturation == 1.0;
}

CdlFormat cdlFormatOf(const std::filesystem::path& file) {
    std::string ext = pathToUtf8(file.extension());
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext == ".cdl" ? CdlFormat::Cdl : ext == ".ccc" ? CdlFormat::Ccc : CdlFormat::Cc;
}

std::expected<std::vector<Cdl>, Error> parseCdl(std::string_view xml) {
    if (xml.size() > 16u * 1024u * 1024u) {
        return std::unexpected(Error{ErrorCode::ParseError, "the CDL file is too large"});
    }
    pugi::xml_document doc;
    const pugi::xml_parse_result parsed = doc.load_buffer(xml.data(), xml.size(), pugi::parse_default, pugi::encoding_auto);
    if (!parsed) {
        return std::unexpected(Error{ErrorCode::ParseError, std::string("not valid XML: ") + parsed.description()});
    }
    std::vector<pugi::xml_node> nodes;
    const pugi::xml_node root = doc.document_element();
    if (localName(root.name()) == "ColorCorrection") {
        nodes.push_back(root);
    } else {
        collect(root, nodes, 0);
    }
    if (nodes.empty()) {
        return std::unexpected(Error{ErrorCode::ParseError, "no ColorCorrection in the file"});
    }
    std::vector<Cdl> out;
    for (const pugi::xml_node& n : nodes) {
        auto cdl = readCorrection(n);
        if (!cdl) {
            return std::unexpected(cdl.error());
        }
        out.push_back(std::move(*cdl));
    }
    return out;
}

std::expected<std::vector<Cdl>, Error> loadCdl(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot open the CDL file"});
    }
    std::ostringstream text;
    text << in.rdbuf();
    return parseCdl(text.str());
}

std::string writeCdl(const std::vector<Cdl>& corrections, CdlFormat format) {
    std::ostringstream out;
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    if (format == CdlFormat::Cc && corrections.size() == 1) {
        writeCorrection(out, corrections.front(), "");
        return out.str();
    }
    if (format == CdlFormat::Cdl) {
        out << "<ColorDecisionList xmlns=\"urn:ASC:CDL:v1.01\">\n";
        for (const Cdl& c : corrections) {
            out << "  <ColorDecision>\n";
            writeCorrection(out, c, "    ");
            out << "  </ColorDecision>\n";
        }
        out << "</ColorDecisionList>\n";
        return out.str();
    }
    out << "<ColorCorrectionCollection xmlns=\"urn:ASC:CDL:v1.01\">\n";
    for (const Cdl& c : corrections) {
        writeCorrection(out, c, "  ");
    }
    out << "</ColorCorrectionCollection>\n";
    return out.str();
}

std::expected<void, Error> saveCdl(const std::vector<Cdl>& corrections, const std::filesystem::path& file) {
    return io::writeFileAtomically(file, writeCdl(corrections, cdlFormatOf(file)));
}

Cdl cdlOf(const GradeParams& p) {
    Cdl cdl;
    for (std::size_t c = 0; c < 3; ++c) {
        cdl.slope[c] = p.gain[c] - p.lift[c];
        cdl.offset[c] = p.lift[c] + p.offset[c];
        cdl.power[c] = p.gamma[c];
    }
    cdl.saturation = p.cdlSaturation;
    return cdl;
}

void applyCdl(GradeParams& p, const Cdl& cdl) {
    p.lift = {0, 0, 0};
    p.gain = cdl.slope;
    p.offset = cdl.offset;
    p.gamma = cdl.power;
    p.cdlSaturation = cdl.saturation;
}

Rgb applyCdlToPixel(const Cdl& cdl, const Rgb& in) {
    Rgb e{};
    for (std::size_t c = 0; c < 3; ++c) {
        e[c] = std::pow(std::clamp(in[c] * cdl.slope[c] + cdl.offset[c], 0.0, 1.0), cdl.power[c]);
    }
    if (cdl.saturation != 1.0) {
        const double y = luma(e);
        for (double& v : e) {
            v = std::clamp(y + (v - y) * cdl.saturation, 0.0, 1.0);
        }
    }
    return e;
}

} // namespace wabcut::color
