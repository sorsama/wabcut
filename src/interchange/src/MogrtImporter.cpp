// Motion Graphics templates (.mogrt) made in Premiere Pro, imported approximately (Mogrt.h).

#include <wabcut/interchange/Mogrt.h>

#include "Importers.h" // gunzip

#include <wabcut/interchange/Zip.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/TitleOps.h>

#include <nlohmann/json.hpp>
#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace wabcut::interchange {

namespace {

using json = nlohmann::json;

Error unsupported(std::string message) { return Error{ErrorCode::Unsupported, std::move(message)}; }
Error parseError(std::string message) { return Error{ErrorCode::ParseError, "mogrt: " + std::move(message)}; }

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// A localised string: a plain string, {"strDB": [{"localeString", "str"}]} (en_US preferred), or {"en_US": "..."}.
std::optional<std::string> localized(const json& v) {
    if (v.is_string()) {
        return v.get<std::string>();
    }
    if (!v.is_object()) {
        return std::nullopt;
    }
    if (v.contains("strDB") && v["strDB"].is_array()) {
        std::optional<std::string> first;
        for (const json& e : v["strDB"]) {
            if (!e.is_object() || !e.contains("str") || !e["str"].is_string()) {
                continue;
            }
            const std::string locale = e.value("localeString", std::string());
            if (locale == "en_US") {
                return e["str"].get<std::string>();
            }
            if (!first) {
                first = e["str"].get<std::string>();
            }
        }
        return first;
    }
    for (const char* key : {"en_US", "en"}) {
        if (v.contains(key) && v[key].is_string()) {
            return v[key].get<std::string>();
        }
    }
    return std::nullopt;
}

/// The text a text property holds: a localised string, which may itself be Premiere's JSON text document
/// ({"textEditValue": "..."}).
std::optional<std::string> textValue(const json& v) {
    auto text = localized(v);
    if (!text && v.is_object() && v.contains("textEditValue") && v["textEditValue"].is_string()) {
        text = v["textEditValue"].get<std::string>();
    }
    if (text && !text->empty() && text->front() == '{' && io::jsonNestingWithin(*text, 16)) {
        const json doc = json::parse(*text, nullptr, false);
        if (doc.is_object() && doc.contains("textEditValue") && doc["textEditValue"].is_string()) {
            text = doc["textEditValue"].get<std::string>();
        }
    }
    return text;
}

/// A colour: [r, g, b(, a)] in 0..1 (or 0..255), {"red"/"r", ...}, or "#RRGGBB".
std::optional<std::array<double, 4>> colorValue(const json& v) {
    std::array<double, 4> c{0.0, 0.0, 0.0, 1.0};
    if (v.is_array() && (v.size() == 3 || v.size() == 4) &&
        std::all_of(v.begin(), v.end(), [](const json& n) { return n.is_number(); })) {
        double peak = 0.0;
        for (std::size_t i = 0; i < v.size(); ++i) {
            c[i] = v[i].get<double>();
            peak = std::max(peak, c[i]);
        }
        if (peak > 1.0) {
            for (double& x : c) {
                x /= 255.0;
            }
            if (v.size() == 3) {
                c[3] = 1.0;
            }
        }
    } else if (v.is_object()) {
        const auto part = [&](std::initializer_list<const char*> keys) -> std::optional<double> {
            for (const char* k : keys) {
                if (v.contains(k) && v[k].is_number()) {
                    return v[k].get<double>();
                }
            }
            return std::nullopt;
        };
        const auto r = part({"red", "r"});
        const auto g = part({"green", "g"});
        const auto b = part({"blue", "b"});
        if (!r || !g || !b) {
            return std::nullopt;
        }
        c = {*r, *g, *b, part({"alpha", "a"}).value_or(1.0)};
        if (std::max({c[0], c[1], c[2]}) > 1.0) {
            c = {c[0] / 255.0, c[1] / 255.0, c[2] / 255.0, c[3] > 1.0 ? c[3] / 255.0 : c[3]};
        }
    } else if (v.is_string() && v.get<std::string>().size() == 7 && v.get<std::string>().front() == '#') {
        const std::string s = v.get<std::string>();
        for (std::size_t i = 0; i < 3; ++i) {
            const auto hex = [](char ch) -> int {
                return std::isdigit(static_cast<unsigned char>(ch)) ? ch - '0' : std::tolower(static_cast<unsigned char>(ch)) - 'a' + 10;
            };
            const int hi = hex(s[1 + 2 * i]);
            const int lo = hex(s[2 + 2 * i]);
            if (hi < 0 || hi > 15 || lo < 0 || lo > 15) {
                return std::nullopt;
            }
            c[i] = (hi * 16 + lo) / 255.0;
        }
    } else {
        return std::nullopt;
    }
    for (double& x : c) {
        if (!std::isfinite(x)) {
            return std::nullopt;
        }
        x = std::clamp(x, 0.0, 1.0);
    }
    return c;
}

/// A field id from a label: [A-Za-z][A-Za-z0-9_.-]*, unique among `taken`.
std::string fieldIdFor(const std::string& label, std::set<std::string>& taken) {
    std::string id;
    bool upper = false;
    for (const char ch : label) {
        const auto u = static_cast<unsigned char>(ch);
        if (std::isalnum(u) && u < 0x80) {
            id.push_back(id.empty() ? static_cast<char>(std::tolower(u)) : upper ? static_cast<char>(std::toupper(u)) : ch);
            upper = false;
        } else {
            upper = !id.empty();
        }
        if (id.size() >= 40) {
            break;
        }
    }
    if (id.empty() || !std::isalpha(static_cast<unsigned char>(id.front()))) {
        id = "field" + id;
    }
    std::string unique = id;
    for (int n = 2; taken.contains(unique); ++n) {
        unique = id + std::to_string(n);
    }
    taken.insert(unique);
    return unique;
}

/// A template id from a capsule id or a name: [A-Za-z0-9][A-Za-z0-9_.-]*, at most 128 bytes, "mogrt." first.
std::string templateIdFor(const std::string& source) {
    std::string id = "mogrt.";
    for (const char ch : source) {
        const auto u = static_cast<unsigned char>(ch);
        if (u < 0x80 && (std::isalnum(u) || ch == '-' || ch == '_' || ch == '.')) {
            id.push_back(static_cast<char>(std::tolower(u)));
        } else if (id.back() != '-') {
            id.push_back('-');
        }
        if (id.size() >= 100) {
            break;
        }
    }
    while (id.size() > 6 && (id.back() == '-' || id.back() == '.')) {
        id.pop_back();
    }
    return id.size() > 6 ? id : std::string("mogrt.template");
}

std::string limitedText(std::string text, std::size_t max) {
    if (!edit::isValidUtf8(text)) {
        text.clear();
    }
    if (text.size() > max) {
        text.resize(max);
        while (!text.empty() && !edit::isValidUtf8(text)) {
            text.pop_back();
        }
    }
    return text;
}

/// A number in definition.json: a number, or {"value": n}.
std::optional<double> numberOf(const json& v) {
    if (v.is_number()) {
        return v.get<double>();
    }
    if (v.is_object() && v.contains("value") && v["value"].is_number()) {
        return v["value"].get<double>();
    }
    return std::nullopt;
}

// ---- The Premiere graphic inside a template (0.2 p03): its text layers' look ----------------------------------------------

/// What a text layer of the template's own Premiere graphic looks like (project.prgraphic: a ZIP holding the gzipped
/// Premiere project of the graphic; each text layer is a VideoFilterComponent "AE.ADBE Text" whose Source Text
/// parameter is a UTF-16 JSON blob after an 8-byte header: mTextParam {mAlignment, mStyleSheet {mText, mFontName,
/// mFontSize, mFillColor (0xRRGGBB), mTracking}, mShadow...}).
struct PremiereText {
    std::string name; ///< the layer's InstanceName
    std::string text;
    std::string fontName; ///< PostScript name, e.g. "BebasNeue-Regular"
    double fontSize = 0.0;
    std::optional<std::array<double, 4>> fill;
    double tracking = 0.0;
    std::optional<TextAlign> align;
    DropShadow shadow;
};

constexpr std::size_t kMaxGraphicProjectBytes = 16u * 1024u * 1024u;
constexpr std::size_t kMaxSourceTextBytes = 1u * 1024u * 1024u;

std::optional<std::string> fromBase64(std::string_view in, std::size_t maxBytes) {
    std::string out;
    out.reserve(in.size() * 3 / 4);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char ch : in) {
        int v = -1;
        if (ch >= 'A' && ch <= 'Z') {
            v = ch - 'A';
        } else if (ch >= 'a' && ch <= 'z') {
            v = ch - 'a' + 26;
        } else if (ch >= '0' && ch <= '9') {
            v = ch - '0' + 52;
        } else if (ch == '+') {
            v = 62;
        } else if (ch == '/') {
            v = 63;
        } else if (ch == '=' || ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
            continue;
        } else {
            return std::nullopt;
        }
        buffer = (buffer << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
            if (out.size() > maxBytes) {
                return std::nullopt;
            }
        }
    }
    return out;
}

/// UTF-16LE -> UTF-8 (unpaired surrogates become U+FFFD); stops at a NUL.
std::string utf8FromUtf16le(std::string_view bytes) {
    std::string out;
    const auto unit = [&](std::size_t i) {
        return static_cast<char32_t>(static_cast<unsigned char>(bytes[i]) | (static_cast<unsigned char>(bytes[i + 1]) << 8));
    };
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        char32_t c = unit(i);
        if (c == 0) {
            break;
        }
        if (c >= 0xD800 && c <= 0xDBFF && i + 3 < bytes.size()) {
            const char32_t low = unit(i + 2);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            } else {
                c = 0xFFFD;
            }
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

/// The first value of a style sheet entry: {"mParamValues": [[0, value], ...]}.
const json* sheetValue(const json& sheet, const char* key) {
    if (!sheet.contains(key) || !sheet[key].is_object() || !sheet[key].contains("mParamValues")) {
        return nullptr;
    }
    const json& values = sheet[key]["mParamValues"];
    if (!values.is_array() || values.empty() || !values[0].is_array() || values[0].size() < 2) {
        return nullptr;
    }
    return &values[0][1];
}

std::array<double, 4> packedColor(std::int64_t v) {
    const auto c = static_cast<std::uint32_t>(v) & 0xFFFFFFu;
    return {((c >> 16) & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0, 1.0};
}

std::optional<PremiereText> sourceText(const std::string& blob) {
    if (blob.size() < 10 || blob.size() > kMaxSourceTextBytes) {
        return std::nullopt;
    }
    const std::string text = utf8FromUtf16le(std::string_view(blob).substr(8));
    if (!io::jsonNestingWithin(text)) {
        return std::nullopt;
    }
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("mTextParam") || !j["mTextParam"].is_object()) {
        return std::nullopt;
    }
    const json& p = j["mTextParam"];
    PremiereText out;
    if (p.contains("mAlignment") && p["mAlignment"].is_number_integer()) {
        const auto a = p["mAlignment"].get<std::int64_t>();
        out.align = a == 0 ? TextAlign::Left : a == 1 ? TextAlign::Right : a == 2 ? TextAlign::Center : TextAlign::Justify;
    }
    const auto number = [&](const json& o, const char* key) -> std::optional<double> {
        if (o.contains(key) && o[key].is_number() && std::isfinite(o[key].get<double>())) {
            return o[key].get<double>();
        }
        return std::nullopt;
    };
    if (p.contains("mShadowVisible") && p["mShadowVisible"].is_boolean() && p["mShadowVisible"].get<bool>()) {
        out.shadow.enabled = true;
        if (p.contains("mShadowColor") && p["mShadowColor"].is_number_integer()) {
            out.shadow.color.rgba = packedColor(p["mShadowColor"].get<std::int64_t>());
        }
        out.shadow.opacity = std::clamp(number(p, "mShadowOpacity").value_or(75.0) / 100.0, 0.0, 1.0);
        out.shadow.angle = std::clamp(number(p, "mShadowAngle").value_or(135.0), -3600.0, 3600.0);
        out.shadow.distance = std::clamp(number(p, "mShadowOffset").value_or(6.0), 0.0, 1000.0);
        out.shadow.softness = std::clamp(number(p, "mShadowBlur").value_or(8.0), 0.0, 1000.0);
    }
    if (p.contains("mStyleSheet") && p["mStyleSheet"].is_object()) {
        const json& sheet = p["mStyleSheet"];
        if (sheet.contains("mText") && sheet["mText"].is_string()) {
            out.text = limitedText(sheet["mText"].get<std::string>(), 4096);
        }
        if (const json* v = sheetValue(sheet, "mFontName"); v && v->is_string()) {
            out.fontName = limitedText(v->get<std::string>(), kMaxFontBytes);
        }
        if (const json* v = sheetValue(sheet, "mFontSize"); v && v->is_number() && std::isfinite(v->get<double>())) {
            out.fontSize = std::clamp(v->get<double>(), 1.0, 2000.0);
        }
        if (const json* v = sheetValue(sheet, "mFillColor"); v && v->is_number_integer()) {
            out.fill = packedColor(v->get<std::int64_t>());
        }
        if (const json* v = sheetValue(sheet, "mTracking"); v && v->is_number() && std::isfinite(v->get<double>())) {
            out.tracking = std::clamp(v->get<double>(), -1000.0, 10000.0);
        }
    }
    return out;
}

/// The text layers of the template's Premiere graphic, in the graphic's order; empty when there is none or it does not
/// read (the import goes on without them).
std::vector<PremiereText> premiereTexts(const ZipArchive& zip) {
    std::vector<PremiereText> out;
    const ZipEntry* entry = zip.find("project.prgraphic");
    if (!entry) {
        return out;
    }
    auto bytes = zip.read(*entry);
    if (!bytes) {
        return out;
    }
    ZipLimits limits;
    limits.maxEntryBytes = kMaxGraphicProjectBytes;
    limits.maxTotalBytes = kMaxGraphicProjectBytes;
    auto inner = ZipArchive::open(*bytes, limits);
    if (!inner) {
        return out;
    }
    std::string xml;
    for (const ZipEntry& e : inner->entries()) {
        if (lower(e.name).ends_with(".prproj")) {
            auto raw = inner->read(e);
            if (!raw) {
                return out;
            }
            if (detail::hasGzipMagic(*raw)) {
                auto unpacked = detail::gunzip(*raw, kMaxGraphicProjectBytes);
                if (!unpacked) {
                    return out;
                }
                xml = std::move(*unpacked);
            } else {
                xml = std::move(*raw);
            }
            break;
        }
    }
    if (xml.empty()) {
        return out;
    }
    pugi::xml_document doc;
    if (!doc.load_buffer(xml.data(), xml.size(), pugi::parse_default & ~pugi::parse_doctype, pugi::encoding_utf8)) {
        return out;
    }
    const pugi::xml_node project = doc.first_child();
    std::map<std::string, pugi::xml_node> objects;
    for (const pugi::xml_node& n : project.children()) {
        if (const pugi::xml_attribute id = n.attribute("ObjectID")) {
            objects.emplace(id.value(), n);
        }
    }
    for (const pugi::xml_node& n : project.children("VideoFilterComponent")) {
        if (std::string_view(n.child_value("MatchName")) != "AE.ADBE Text" || out.size() >= 64) {
            continue;
        }
        const pugi::xml_node component = n.child("Component");
        for (const pugi::xml_node& param : component.child("Params").children("Param")) {
            const auto obj = objects.find(param.attribute("ObjectRef").value());
            if (obj == objects.end() || std::string_view(obj->second.child_value("Name")) != "Source Text") {
                continue;
            }
            const auto blob = fromBase64(obj->second.child_value("StartKeyframeValue"), kMaxSourceTextBytes);
            if (!blob) {
                break;
            }
            if (auto t = sourceText(*blob)) {
                t->name = limitedText(component.child_value("InstanceName"), kMaxGraphicNameBytes);
                out.push_back(std::move(*t));
            }
            break;
        }
    }
    return out;
}

/// A font family and style from a PostScript name: "BebasNeue-Regular" -> Bebas Neue, 400; "Montserrat-SemiBoldItalic"
/// -> Montserrat, 600, italic; "ArialMT" -> Arial.
void applyFontName(TextStyle& style, std::string_view postScript) {
    std::string family(postScript.substr(0, postScript.find('-')));
    std::string face = postScript.find('-') == std::string_view::npos ? std::string() : std::string(postScript.substr(postScript.find('-') + 1));
    for (const std::string_view suffix : {std::string_view("MT"), std::string_view("PS")}) { // ArialMT, TimesNewRomanPS
        if (family.size() > suffix.size() + 2 && family.ends_with(suffix)) {
            family.resize(family.size() - suffix.size());
        }
    }
    std::string spaced;
    for (std::size_t i = 0; i < family.size(); ++i) {
        const char c = family[i];
        if (i > 0 && std::isupper(static_cast<unsigned char>(c)) && std::islower(static_cast<unsigned char>(family[i - 1]))) {
            spaced.push_back(' ');
        }
        spaced.push_back(c);
    }
    style.fontFamily = limitedText(spaced, kMaxFontBytes);
    const std::string f = lower(face);
    style.italic = f.find("italic") != std::string::npos || f.find("oblique") != std::string::npos;
    style.fontWeight = f.find("thin") != std::string::npos         ? 100
                       : f.find("extralight") != std::string::npos ? 200
                       : f.find("light") != std::string::npos      ? 300
                       : f.find("medium") != std::string::npos     ? 500
                       : f.find("semibold") != std::string::npos   ? 600
                       : f.find("extrabold") != std::string::npos  ? 800
                       : f.find("black") != std::string::npos || f.find("heavy") != std::string::npos ? 900
                       : f.find("bold") != std::string::npos       ? 700
                                                                   : 400;
}

} // namespace

std::expected<MogrtImport, Error> importMogrt(std::string_view data, std::string_view fileName) {
    if (data.size() > kMaxMogrtBytes) {
        return std::unexpected(parseError("larger than 64 MiB"));
    }
    auto zip = ZipArchive::open(data);
    if (!zip) {
        return std::unexpected(parseError(zip.error().message));
    }
    const ZipEntry* definition = zip->find("definition.json");
    if (!definition) {
        return std::unexpected(parseError("no definition.json: not a Motion Graphics template"));
    }
    if (definition->size > kMaxMogrtDefinitionBytes) {
        return std::unexpected(parseError("definition.json is larger than 4 MiB"));
    }
    auto text = zip->read(*definition);
    if (!text) {
        return std::unexpected(parseError(text.error().message));
    }
    if (!io::jsonNestingWithin(*text)) {
        return std::unexpected(parseError("definition.json nests too deep"));
    }
    const json def = json::parse(*text, nullptr, false);
    if (def.is_discarded() || !def.is_object()) {
        return std::unexpected(parseError("definition.json is not a JSON object"));
    }

    MogrtImport out;
    const std::string file(fileName);
    const auto at = [&](std::string element) { return SourceLocation{file, std::move(element), 0}; };

    // Who made it: After Effects compositions cannot be run.
    std::string author = def.contains("authorApp") && def["authorApp"].is_string() ? def["authorApp"].get<std::string>() : std::string();
    bool afterEffects = lower(author).find("ae") == 0 || lower(author).find("after") != std::string::npos;
    for (const ZipEntry& e : zip->entries()) {
        const std::string name = lower(e.name);
        if (name.ends_with(".aegraphic") || name.ends_with(".aep") || name.ends_with(".aepx")) {
            afterEffects = true;
        }
    }
    if (afterEffects) {
        return std::unexpected(unsupported("this Motion Graphics template was made in After Effects: Wabcut cannot run "
                                           "After Effects compositions (export the composition as Lottie with Bodymovin "
                                           "and import that instead); templates made in Premiere Pro import"));
    }
    out.authorApp = author.empty() ? std::string("Premiere Pro") : limitedText(author, 64);

    // Name, canvas and length.
    GraphicTemplate& t = out.tmpl;
    std::string stem(fileName);
    if (const std::size_t slash = stem.find_last_of("/\\"); slash != std::string::npos) {
        stem = stem.substr(slash + 1);
    }
    if (const std::size_t dot = stem.rfind('.'); dot != std::string::npos && dot > 0) {
        stem = stem.substr(0, dot);
    }
    std::string name;
    for (const char* key : {"capsuleNameLocalized", "nameLocalized", "name"}) {
        if (def.contains(key)) {
            if (auto n = localized(def[key])) {
                name = *n;
                break;
            }
        }
    }
    json info = json::object();
    if (def.contains("sourceInfoLocalized") && def["sourceInfoLocalized"].is_object() && !def["sourceInfoLocalized"].empty()) {
        const json& all = def["sourceInfoLocalized"];
        info = all.contains("en_US") ? all["en_US"] : all.begin().value();
        if (name.empty() && info.is_object() && info.contains("name")) {
            name = localized(info["name"]).value_or(std::string());
        }
    }
    t.name = limitedText(name.empty() ? stem : name, kMaxGraphicNameBytes);
    if (t.name.empty()) {
        t.name = "Motion Graphics template";
    }
    const std::string capsule = def.contains("capsuleID") && def["capsuleID"].is_string() ? def["capsuleID"].get<std::string>() : std::string();
    t.id = templateIdFor(capsule.empty() ? t.name : capsule);
    t.description = "Imported from " + limitedText(std::string(fileName), 200) + " (made in " + out.authorApp + ")";
    std::int32_t width = 1920;
    std::int32_t height = 1080;
    if (info.is_object()) {
        const auto dims = [&](const char* w, const char* h) {
            const auto a = info.contains(w) ? numberOf(info[w]) : std::nullopt;
            const auto b = info.contains(h) ? numberOf(info[h]) : std::nullopt;
            if (a && b && *a >= 16 && *b >= 16 && *a <= kMaxGraphicCanvas && *b <= kMaxGraphicCanvas) {
                width = static_cast<std::int32_t>(*a);
                height = static_cast<std::int32_t>(*b);
                return true;
            }
            return false;
        };
        if (!dims("width", "height") && !dims("frameWidth", "frameHeight")) {
            out.report.add(ReportOutcome::Approximated, ReportCategory::SequenceSetting, "no frame size: 1920 x 1080",
                           at("/sourceInfoLocalized"));
        }
        if (info.contains("duration")) {
            if (const auto d = numberOf(info["duration"]); d && *d > 0) {
                // Premiere's ticks are Wabcut's (254,016,000,000 per second); small numbers are seconds.
                const double ticks = *d > 100000.0 ? *d : *d * static_cast<double>(kTicksPerSecond);
                if (ticks >= static_cast<double>(kTicksPerSecond) / 100 && ticks <= 3600.0 * static_cast<double>(kTicksPerSecond)) {
                    t.duration = Ticks{static_cast<std::int64_t>(std::llround(ticks))};
                }
            }
        }
    }
    t.graphic.name = t.name;
    t.graphic.width = width;
    t.graphic.height = height;

    // Essential Properties -> text layers and fields.
    const json controls = def.contains("clientControls") && def["clientControls"].is_array() ? def["clientControls"] : json::array();
    std::vector<std::pair<std::string, std::string>> texts;      // label, default
    std::vector<std::pair<std::string, std::array<double, 4>>> colors;
    std::vector<std::pair<std::string, bool>> checkboxes;
    std::size_t index = 0;
    for (const json& c : controls) {
        const std::string path = "/clientControls/" + std::to_string(index++);
        if (!c.is_object()) {
            continue;
        }
        std::string label = c.contains("uiName") ? localized(c["uiName"]).value_or(std::string()) : std::string();
        if (label.empty() && c.contains("name")) {
            label = localized(c["name"]).value_or(std::string());
        }
        label = limitedText(label.empty() ? std::string("Property ") + std::to_string(index) : label, kMaxGraphicNameBytes);
        const json value = c.contains("value") ? c["value"] : json();
        std::string type;
        if (c.contains("type")) {
            type = c["type"].is_string() ? lower(c["type"].get<std::string>()) : c["type"].is_number_integer() ? std::to_string(c["type"].get<std::int64_t>()) : "";
        }
        const bool textType = type == "6" || type.find("text") != std::string::npos;
        const bool colorType = type == "2" || type.find("color") != std::string::npos;
        const bool checkType = type == "4" || type.find("check") != std::string::npos;
        if (colorType || (!textType && !checkType && colorValue(value))) {
            if (const auto rgba = colorValue(value)) {
                colors.emplace_back(label, *rgba);
                continue;
            }
        }
        if (checkType || value.is_boolean()) {
            checkboxes.emplace_back(label, value.is_boolean() ? value.get<bool>() : true);
            continue;
        }
        if (textType || value.is_string() || (value.is_object() && localized(value))) {
            if (auto s = textValue(value)) {
                if (texts.size() < kMaxTemplateFields && texts.size() < 32) {
                    texts.emplace_back(label, limitedText(*s, 4096));
                    continue;
                }
            }
        }
        const std::string kind = value.is_number() ? "slider" : value.is_array() ? "point or list" : type.empty() ? "unknown" : "type " + type;
        out.report.add(ReportOutcome::Dropped, ReportCategory::Graphic,
                       "Essential Property '" + label + "' (" + kind + "): Wabcut templates have no field for it", at(path));
    }
    if (texts.empty()) {
        return std::unexpected(unsupported("the template has no text property Wabcut can edit (its Essential Properties are "
                                           "sliders, points or media only)"));
    }

    // A lower third: the texts stacked from the lower left in a group, a box pinned around the group.
    std::set<std::string> fieldIds;
    const double w = width;
    const double h = height;
    GraphicLayer box;
    box.id = 1;
    box.name = "Box";
    box.kind = LayerKind::Shape;
    box.shape.kind = ShapeKind::Rectangle;
    box.shape.cornerRadius = std::round(h * 0.008);
    box.shape.fill.color.rgba = colors.empty() ? std::array<double, 4>{0.0, 0.0, 0.0, 0.6} : colors.front().second;
    box.pin = LayerPin{2, LayerPin::kLeft | LayerPin::kTop | LayerPin::kRight | LayerPin::kBottom,
                       {-std::round(w * 0.015), -std::round(h * 0.012), std::round(w * 0.015), std::round(h * 0.012)}};
    GraphicLayer group;
    group.id = 2;
    group.name = "Text";
    group.kind = LayerKind::Group;
    double y = h * 0.70;
    std::uint32_t nextId = 3;
    // The look of the template's own text layers (its Premiere graphic), matched by their text, else in order.
    const std::vector<PremiereText> looks = premiereTexts(*zip);
    std::vector<bool> lookUsed(looks.size(), false);
    for (std::size_t i = 0; i < texts.size(); ++i) {
        GraphicLayer l;
        l.id = nextId++;
        l.name = texts[i].first;
        l.kind = LayerKind::Text;
        TextStyle style;
        style.fontSize = std::round(h * (i == 0 ? 0.055 : 0.035));
        style.fontWeight = i == 0 ? 700 : 400;
        if (i + 1 < colors.size()) {
            style.fill.rgba = colors[i + 1].second;
        }
        std::optional<std::size_t> look;
        for (std::size_t k = 0; k < looks.size() && !look; ++k) {
            if (!lookUsed[k] && !looks[k].text.empty() && lower(looks[k].text) == lower(texts[i].second)) {
                look = k;
            }
        }
        for (std::size_t k = 0; k < looks.size() && !look; ++k) {
            if (!lookUsed[k]) {
                look = k;
            }
        }
        if (look) {
            const PremiereText& p = looks[*look];
            lookUsed[*look] = true;
            if (!p.fontName.empty()) {
                applyFontName(style, p.fontName);
            }
            if (p.fontSize > 0.0) {
                style.fontSize = p.fontSize;
            }
            if (p.fill && i + 1 >= colors.size()) {
                style.fill.rgba = *p.fill;
            }
            style.tracking = p.tracking;
            if (p.align) {
                l.text.align = *p.align;
            }
            l.shadow = p.shadow;
            out.report.add(ReportOutcome::Carried, ReportCategory::Graphic,
                           "text property '" + texts[i].first + "': the look of the template's text layer '" +
                               (p.name.empty() ? std::string("text") : p.name) + "' (font " + p.fontName +
                               ", size, colour, tracking, alignment, shadow)",
                           at("/project.prgraphic"));
        }
        l.text.runs = {TextRun{texts[i].second, style}};
        l.transform.x = std::round(w * 0.08);
        l.transform.y = std::round(y);
        y += style.fontSize * 1.3;
        group.children.push_back(std::move(l));
        TemplateField f;
        f.id = fieldIdFor(texts[i].first, fieldIds);
        f.label = texts[i].first;
        f.kind = FieldKind::Text;
        f.layer = group.children.back().id;
        f.property = "text";
        f.value.text = texts[i].second;
        t.fields.push_back(std::move(f));
        out.report.add(ReportOutcome::Carried, ReportCategory::Graphic, "text property '" + texts[i].first + "': a text field",
                       at("/clientControls"));
    }
    for (std::size_t i = 0; i < colors.size() && t.fields.size() < kMaxTemplateFields; ++i) {
        const bool toBox = i == 0;
        if (!toBox && i - 1 >= group.children.size()) {
            out.report.add(ReportOutcome::Dropped, ReportCategory::Graphic,
                           "colour property '" + colors[i].first + "': nothing left to colour", at("/clientControls"));
            continue;
        }
        TemplateField f;
        f.id = fieldIdFor(colors[i].first, fieldIds);
        f.label = colors[i].first;
        f.kind = FieldKind::Color;
        f.layer = toBox ? box.id : group.children[i - 1].id;
        f.property = "fill";
        f.value.color.rgba = colors[i].second;
        t.fields.push_back(std::move(f));
        out.report.add(ReportOutcome::Approximated, ReportCategory::Graphic,
                       "colour property '" + colors[i].first + "': " + (toBox ? "the box's colour" : "the colour of text " + std::to_string(i)),
                       at("/clientControls"));
    }
    for (std::size_t i = 0; i < checkboxes.size(); ++i) {
        if (i > 0 || t.fields.size() >= kMaxTemplateFields) {
            out.report.add(ReportOutcome::Dropped, ReportCategory::Graphic,
                           "checkbox '" + checkboxes[i].first + "': only the first one is kept (it shows the box)", at("/clientControls"));
            continue;
        }
        TemplateField f;
        f.id = fieldIdFor(checkboxes[i].first, fieldIds);
        f.label = checkboxes[i].first;
        f.kind = FieldKind::Checkbox;
        f.layer = box.id;
        f.property = "visible";
        f.value.flag = checkboxes[i].second;
        box.visible = checkboxes[i].second;
        t.fields.push_back(std::move(f));
        out.report.add(ReportOutcome::Approximated, ReportCategory::Graphic, "checkbox '" + checkboxes[i].first + "': shows the box",
                       at("/clientControls"));
    }
    t.graphic.layers = {std::move(box), std::move(group)};
    out.report.add(ReportOutcome::Approximated, ReportCategory::Graphic,
                   looks.empty() ? std::string("the template's layout and animation (in its Premiere graphic) are not read: "
                                               "Wabcut lays the texts out as a lower third")
                                 : std::string("the template's text looks are read, but not its layout, shapes or animation: "
                                               "Wabcut lays the texts out as a lower third"),
                   at("/"));
    for (const ZipEntry& e : zip->entries()) {
        const std::string entry = lower(e.name);
        if (entry.ends_with(".png") || entry.ends_with(".jpg") || entry.ends_with(".jpeg") || entry.ends_with(".mp4") ||
            entry.ends_with(".mov")) {
            if (entry.find("thumb") == std::string::npos) {
                out.report.add(ReportOutcome::Dropped, ReportCategory::Media, "media in the template: " + e.name, at(e.name));
            }
        }
    }
    if (const auto problem = templateProblem(t)) {
        return std::unexpected(parseError("internal: " + *problem));
    }
    return out;
}

} // namespace wabcut::interchange
