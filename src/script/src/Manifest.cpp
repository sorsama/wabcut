#include <wabcut/script/Manifest.h>

#include <wabcut/model/ProjectIO.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <set>

namespace wabcut::script {

namespace {

constexpr std::size_t kMaxAuthorBytes = 128;
constexpr std::size_t kMaxLicenceBytes = 64;
constexpr std::size_t kMaxTitleBytes = 100;
constexpr std::size_t kMaxPrereleaseBytes = 32;
constexpr int kMaxVersionNumber = 999'999;

bool isLowerAlnum(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }
bool isAlnum(char c) { return isLowerAlnum(c) || (c >= 'A' && c <= 'Z'); }

/// Decodes one UTF-8 code point at `i` (the text is valid UTF-8: the JSON parser checked it).
char32_t codePointAt(std::string_view s, std::size_t& i) {
    const auto b = static_cast<unsigned char>(s[i]);
    const int extra = b < 0x80 ? 0 : b < 0xE0 ? 1 : b < 0xF0 ? 2 : 3;
    char32_t cp = extra == 0 ? b : extra == 1 ? (b & 0x1Fu) : extra == 2 ? (b & 0x0Fu) : (b & 0x07u);
    for (int k = 0; k < extra && i + 1 < s.size(); ++k) {
        cp = (cp << 6) | (static_cast<unsigned char>(s[++i]) & 0x3Fu);
    }
    ++i;
    return cp;
}

bool parseNumber(std::string_view s, int& out) {
    if (s.empty() || s.size() > 6 || (s.size() > 1 && s[0] == '0')) {
        return false;
    }
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && end == s.data() + s.size() && out <= kMaxVersionNumber;
}

class Checker {
public:
    explicit Checker(const Json& doc) : m_doc(doc) {}

    void issue(std::string field, std::string message) { m_issues.push_back({std::move(field), std::move(message)}); }
    std::vector<ManifestIssue> take() { return std::move(m_issues); }
    bool ok() const { return m_issues.empty(); }

    /// A string member; `required` reports a missing one. Empty optional = missing or wrong (reported).
    std::optional<std::string> text(const char* key, std::size_t maxBytes, bool required, bool multiline = false,
                                    const char* alternative = nullptr) {
        const char* found = m_doc.contains(key) ? key : (alternative && m_doc.contains(alternative) ? alternative : nullptr);
        if (!found) {
            if (required) {
                issue(key, "is required");
            }
            return std::nullopt;
        }
        const Json& v = m_doc[found];
        if (!v.is_string()) {
            issue(found, "must be a string");
            return std::nullopt;
        }
        const auto& s = v.get_ref<const std::string&>();
        if (s.empty() || s.size() > maxBytes) {
            issue(found, "must be 1 to " + std::to_string(maxBytes) + " bytes long");
            return std::nullopt;
        }
        if (!isDisplayText(s, multiline)) {
            issue(found, "contains control or invisible formatting characters");
            return std::nullopt;
        }
        return s;
    }

    std::optional<Version> version(const char* key) {
        const auto s = text(key, 64, true);
        if (!s) {
            return std::nullopt;
        }
        auto v = parseVersion(*s);
        if (!v) {
            issue(key, "must be a version like \"1.2.0\" or \"1.2.0-beta.1\"");
        }
        return v;
    }

private:
    const Json& m_doc;
    std::vector<ManifestIssue> m_issues;
};

/// Characters that draw nothing, change the direction or the lines of the text around them, or draw whatever a
/// font decides (S5-P2-2): General Category Cf, Zl, Zp and Co, the variation selectors, the tags, the Hangul fillers
/// and the noncharacters. Controls (Cc) are checked by the caller.
bool isInvisibleOrFormat(char32_t cp) {
    struct Range {
        char32_t first, last;
    };
    static constexpr Range kRanges[] = {
        {0x00AD, 0x00AD},   // soft hyphen
        {0x034F, 0x034F},   // combining grapheme joiner
        {0x0600, 0x0605},   // Arabic number signs (Cf)
        {0x061C, 0x061C},   // Arabic letter mark
        {0x06DD, 0x06DD},   {0x070F, 0x070F}, {0x0890, 0x0891}, {0x08E2, 0x08E2}, // more Cf
        {0x115F, 0x1160},   // Hangul fillers
        {0x17B4, 0x17B5},   // Khmer inherent vowels (invisible)
        {0x180B, 0x180F},   // Mongolian variation selectors and vowel separator
        {0x200B, 0x200F},   // zero-width space, joiners, LRM / RLM
        {0x2028, 0x202E},   // line and paragraph separators, bidi embeddings and overrides
        {0x2060, 0x206F},   // word joiner, invisible operators, bidi isolates, deprecated format characters
        {0x3164, 0x3164},   // Hangul filler
        {0xD800, 0xF8FF},   // surrogates, private use
        {0xFDD0, 0xFDEF},   // noncharacters
        {0xFE00, 0xFE0F},   // variation selectors
        {0xFEFF, 0xFEFF},   // zero-width no-break space
        {0xFFA0, 0xFFA0},   // halfwidth Hangul filler
        {0xFFF0, 0xFFFB},   // specials, interlinear annotations
        {0x110BD, 0x110BD}, {0x110CD, 0x110CD}, {0x13430, 0x1343F}, // more Cf
        {0x1BCA0, 0x1BCA3}, // shorthand format controls
        {0x1D173, 0x1D17A}, // musical symbol format characters
        {0xE0000, 0xE0FFF}, // tags, variation selectors supplement
        {0xF0000, 0x10FFFF}, // private use planes
    };
    if ((cp & 0xFFFE) == 0xFFFE) {
        return true; // U+xFFFE and U+xFFFF of every plane are noncharacters
    }
    return std::ranges::any_of(kRanges, [cp](const Range& r) { return cp >= r.first && cp <= r.last; });
}

/// Lower-case ASCII letters and digits for what `text` looks like: case folded, fullwidth forms, accents and the
/// Cyrillic and Greek letters that look like Latin ones mapped to Latin, confusable digits and letter pairs to the
/// letters they imitate, everything else dropped. Two names with the same skeleton look alike.
std::string skeleton(std::string_view text) {
    static constexpr std::pair<char32_t, char> kLookAlikes[] = {
        // Cyrillic
        {0x0410, 'a'}, {0x0430, 'a'}, {0x0412, 'b'}, {0x0432, 'b'}, {0x042C, 'b'}, {0x044C, 'b'}, {0x0421, 'c'},
        {0x0441, 'c'}, {0x0501, 'd'}, {0x0415, 'e'}, {0x0435, 'e'}, {0x0406, 'i'}, {0x0456, 'i'}, {0x0408, 'j'},
        {0x0458, 'j'}, {0x041A, 'k'}, {0x043A, 'k'}, {0x041C, 'm'}, {0x043C, 'm'}, {0x041D, 'h'}, {0x043D, 'h'},
        {0x041E, 'o'}, {0x043E, 'o'}, {0x0420, 'p'}, {0x0440, 'p'}, {0x051A, 'q'}, {0x051B, 'q'}, {0x0405, 's'},
        {0x0455, 's'}, {0x0422, 't'}, {0x0442, 't'}, {0x0423, 'y'}, {0x0443, 'y'}, {0x051C, 'w'}, {0x051D, 'w'},
        {0x0425, 'x'}, {0x0445, 'x'},
        // Greek
        {0x0391, 'a'}, {0x03B1, 'a'}, {0x0392, 'b'}, {0x03B2, 'b'}, {0x0395, 'e'}, {0x03B5, 'e'}, {0x0396, 'z'},
        {0x0397, 'h'}, {0x0399, 'i'}, {0x03B9, 'i'}, {0x039A, 'k'}, {0x03BA, 'k'}, {0x039C, 'm'}, {0x039D, 'n'},
        {0x03BD, 'v'}, {0x039F, 'o'}, {0x03BF, 'o'}, {0x03A1, 'p'}, {0x03C1, 'p'}, {0x03A4, 't'}, {0x03C4, 't'},
        {0x03A5, 'y'}, {0x03C5, 'u'}, {0x03A7, 'x'}, {0x03C7, 'x'}, {0x03C9, 'w'},
    };
    static constexpr std::string_view kAccented = // U+00C0 to U+00FF, '-' for none
        "aaaaaaaceeeeiiii" "dnooooo-ouuuuyts" "aaaaaaaceeeeiiii" "dnooooo-ouuuuyty";
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        const char32_t cp = codePointAt(text, i);
        char32_t c = cp;
        if (c >= 0xFF01 && c <= 0xFF5E) {
            c -= 0xFEE0; // fullwidth ASCII
        }
        char mapped = 0;
        if (c < 0x80) {
            const char a = static_cast<char>(c);
            mapped = (a >= 'A' && a <= 'Z') ? static_cast<char>(a - 'A' + 'a') : isLowerAlnum(a) ? a : 0;
        } else if (c >= 0xC0 && c <= 0xFF) {
            mapped = kAccented[c - 0xC0] == '-' ? 0 : kAccented[c - 0xC0];
        } else {
            for (const auto& [from, to] : kLookAlikes) {
                if (from == c) {
                    mapped = to;
                }
            }
        }
        if (mapped == '0') {
            mapped = 'o';
        } else if (mapped == '1' || mapped == 'i') {
            mapped = 'l'; // l, I and 1 look alike in most interface fonts
        }
        if (mapped != 0) {
            out += mapped;
        }
    }
    for (const auto& [pair, single] : {std::pair<std::string_view, std::string_view>{"rn", "m"}, {"vv", "w"}}) {
        for (std::size_t at = out.find(pair); at != std::string::npos; at = out.find(pair, at)) {
            out.replace(at, pair.size(), single);
        }
    }
    return out;
}

} // namespace

bool isDisplayText(std::string_view s, bool multiline) {
    for (std::size_t i = 0; i < s.size();) {
        const char32_t cp = codePointAt(s, i);
        if ((cp < 0x20 && !(multiline && cp == '\n')) || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) {
            return false;
        }
        if (isInvisibleOrFormat(cp)) {
            return false;
        }
    }
    return true;
}

std::optional<std::string> reservedName(std::string_view name) {
    // The editor's own name and the editors and companies people would trust a window titled with (S5-P2-2).
    static constexpr std::string_view kReserved[] = {"Wabcut",     "Adobe",          "Premiere",  "After Effects",
                                                     "DaVinci",    "Blackmagic",     "Final Cut", "Media Composer",
                                                     "Microsoft",  "Anthropic"};
    const std::string look = skeleton(name);
    for (const std::string_view word : kReserved) {
        if (look.starts_with(skeleton(word))) {
            return std::string(word);
        }
    }
    return std::nullopt;
}

std::optional<Version> parseVersion(std::string_view text) {
    Version v;
    const std::size_t dash = text.find('-');
    std::string_view core = text.substr(0, dash);
    if (dash != std::string_view::npos) {
        const std::string_view pre = text.substr(dash + 1);
        if (pre.empty() || pre.size() > kMaxPrereleaseBytes ||
            !std::all_of(pre.begin(), pre.end(), [](char c) { return isAlnum(c) || c == '.' || c == '-'; }) ||
            pre.front() == '.' || pre.back() == '.' || pre.find("..") != std::string_view::npos) {
            return std::nullopt;
        }
        v.prerelease = std::string(pre);
    }
    std::array<int*, 3> parts{&v.major, &v.minor, &v.patch};
    for (std::size_t k = 0; k < parts.size(); ++k) {
        const std::size_t dot = core.find('.');
        const bool last = k + 1 == parts.size();
        if (last != (dot == std::string_view::npos) || !parseNumber(core.substr(0, dot), *parts[k])) {
            return std::nullopt;
        }
        core = last ? std::string_view{} : core.substr(dot + 1);
    }
    return v;
}

std::string toString(const Version& v) {
    std::string s = std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
    if (!v.prerelease.empty()) {
        s += "-" + v.prerelease;
    }
    return s;
}

int compareVersions(const Version& a, const Version& b) {
    for (const auto [x, y] : {std::pair{a.major, b.major}, std::pair{a.minor, b.minor}, std::pair{a.patch, b.patch}}) {
        if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    if (a.prerelease == b.prerelease) {
        return 0;
    }
    if (a.prerelease.empty() || b.prerelease.empty()) {
        return a.prerelease.empty() ? 1 : -1; // 1.0.0-beta < 1.0.0
    }
    return a.prerelease < b.prerelease ? -1 : 1;
}

command::CapabilitySet Manifest::requested() const {
    command::CapabilitySet set;
    for (const command::Capability c : capabilities) {
        set.bits |= command::CapabilitySet::of({c}).bits;
    }
    return set;
}

std::vector<std::string> Manifest::files() const {
    std::vector<std::string> out = scripts;
    out.push_back(main);
    return out;
}

std::string ManifestError::summary() const {
    std::string s;
    for (const ManifestIssue& i : issues) {
        if (!s.empty()) {
            s += "; ";
        }
        s += i.field.empty() ? i.message : i.field + ": " + i.message;
    }
    return s;
}

command::CapabilitySet grantableCapabilities() {
    using command::Capability;
    return command::CapabilitySet::of(
        {Capability::Read, Capability::EditSequence, Capability::EditProject, Capability::MediaAnalysis});
}

bool isValidExtensionId(std::string_view id) {
    // Reverse-DNS: two or more labels of [a-z0-9] with inner '-', separated by dots.
    if (id.empty() || id.size() > kMaxExtensionIdBytes || id.find('.') == std::string_view::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start <= id.size()) {
        const std::size_t dot = std::min(id.find('.', start), id.size());
        const std::string_view label = id.substr(start, dot - start);
        if (label.empty() || !isLowerAlnum(label.front()) || !isLowerAlnum(label.back()) ||
            !std::all_of(label.begin(), label.end(), [](char c) { return isLowerAlnum(c) || c == '-'; })) {
            return false;
        }
        start = dot + 1;
    }
    return true;
}

bool isValidScriptPath(std::string_view path) {
    if (path.size() < 4 || path.size() > kMaxScriptPathBytes || !path.ends_with(".js")) {
        return false;
    }
    std::size_t start = 0;
    int depth = 0;
    while (start <= path.size()) {
        const std::size_t slash = std::min(path.find('/', start), path.size());
        const std::string_view segment = path.substr(start, slash - start);
        if (segment.empty() || segment == "." || segment == ".." || ++depth > 8 ||
            !std::all_of(segment.begin(), segment.end(), [](char c) { return isAlnum(c) || c == '_' || c == '-' || c == '.'; })) {
            return false;
        }
        start = slash + 1;
    }
    return true;
}

bool isValidCommandId(std::string_view id) {
    return !id.empty() && id.size() <= 64 && ((id[0] >= 'a' && id[0] <= 'z') || (id[0] >= 'A' && id[0] <= 'Z')) &&
           std::all_of(id.begin(), id.end(), [](char c) { return isAlnum(c) || c == '_' || c == '-' || c == '.'; });
}

std::expected<Manifest, ManifestError> parseManifest(std::string_view text, const std::optional<Version>& running) {
    const auto fail = [](std::string field, std::string message) {
        return std::unexpected(ManifestError{{ManifestIssue{std::move(field), std::move(message)}}});
    };
    if (text.size() > kMaxManifestBytes) {
        return fail("", "the manifest is larger than " + std::to_string(kMaxManifestBytes / 1024) + " KiB");
    }
    if (!io::jsonNestingWithin(text, kMaxManifestNesting)) {
        return fail("", "the manifest nests deeper than " + std::to_string(kMaxManifestNesting) + " levels");
    }
    const Json doc = Json::parse(text, nullptr, false);
    if (doc.is_discarded()) {
        return fail("", "the manifest is not valid JSON (UTF-8)");
    }
    if (!doc.is_object()) {
        return fail("", "the manifest must be a JSON object");
    }

    Checker c(doc);
    Manifest m;
    static const std::set<std::string, std::less<>> kKnown{
        "manifestVersion", "id",           "name",         "version", "author", "licence", "license",
        "description",     "minWabcutVersion", "capabilities", "main",    "scripts", "commands"};
    for (const auto& [key, value] : doc.items()) {
        if (!kKnown.contains(key)) {
            if (m.warnings.size() < 16) {
                m.warnings.push_back("unknown key \"" + key.substr(0, 64) + "\" ignored");
            }
        }
    }

    if (!doc.contains("manifestVersion") || !doc["manifestVersion"].is_number_integer()) {
        c.issue("manifestVersion", "is required and must be an integer");
    } else if (doc["manifestVersion"].get<std::int64_t>() != kManifestVersion) {
        c.issue("manifestVersion", "must be " + std::to_string(kManifestVersion) +
                                       " (a newer manifest needs a newer Wabcut)");
    }
    if (auto id = c.text("id", kMaxExtensionIdBytes, true)) {
        if (isValidExtensionId(*id)) {
            m.id = std::move(*id);
        } else {
            c.issue("id", "must be a lower-case reverse-DNS name such as \"com.example.beat-markers\"");
        }
    }
    m.name = c.text("name", kMaxNameBytes, true).value_or("");
    if (const auto reserved = reservedName(m.name); reserved && !m.name.empty()) {
        c.issue("name", "is reserved: an extension's name may not start with \"" + *reserved +
                            "\" (or look like it), so it cannot pose as the editor or another product");
    }
    if (auto v = c.version("version")) {
        m.version = std::move(*v);
    }
    m.author = c.text("author", kMaxAuthorBytes, true).value_or("");
    m.licence = c.text("licence", kMaxLicenceBytes, true, false, "license").value_or("");
    m.description = c.text("description", kMaxDescriptionBytes, false, true).value_or("");
    if (auto v = c.version("minWabcutVersion")) {
        m.minWabcutVersion = std::move(*v);
        if (running && compareVersions(*running, m.minWabcutVersion) < 0) {
            c.issue("minWabcutVersion", "the extension needs Wabcut " + toString(m.minWabcutVersion) +
                                            " or later (this is " + toString(*running) + ")");
        }
    }

    if (!doc.contains("capabilities") || !doc["capabilities"].is_array()) {
        c.issue("capabilities", "is required and must be an array (it may be empty)");
    } else {
        const Json& caps = doc["capabilities"];
        if (caps.size() > 8) {
            c.issue("capabilities", "lists more than 8 entries");
        } else {
            for (std::size_t i = 0; i < caps.size(); ++i) {
                const std::string field = "capabilities[" + std::to_string(i) + "]";
                const auto cap = caps[i].is_string() ? command::capabilityFromString(caps[i].get<std::string>())
                                                     : std::nullopt;
                if (!cap) {
                    c.issue(field, "must be one of Read, EditSequence, EditProject, MediaAnalysis");
                } else if (!grantableCapabilities().has(*cap)) {
                    c.issue(field, "extensions cannot be granted " + std::string(command::toString(*cap)) +
                                       ": they have no network access (cloud AI only through commands)");
                } else if (std::find(m.capabilities.begin(), m.capabilities.end(), *cap) != m.capabilities.end()) {
                    c.issue(field, "is listed twice");
                } else {
                    m.capabilities.push_back(*cap);
                }
            }
        }
    }

    if (auto main = c.text("main", kMaxScriptPathBytes, true)) {
        if (isValidScriptPath(*main)) {
            m.main = std::move(*main);
        } else {
            c.issue("main", "must be a relative path to a .js file inside the extension (no \"..\")");
        }
    }
    if (doc.contains("scripts")) {
        const Json& scripts = doc["scripts"];
        if (!scripts.is_array() || scripts.size() + 1 > kMaxScripts) {
            c.issue("scripts", "must be an array of at most " + std::to_string(kMaxScripts - 1) + " paths");
        } else {
            for (std::size_t i = 0; i < scripts.size(); ++i) {
                const std::string field = "scripts[" + std::to_string(i) + "]";
                if (!scripts[i].is_string() || !isValidScriptPath(scripts[i].get_ref<const std::string&>())) {
                    c.issue(field, "must be a relative path to a .js file inside the extension (no \"..\")");
                    continue;
                }
                const auto& path = scripts[i].get_ref<const std::string&>();
                if (path == m.main || std::find(m.scripts.begin(), m.scripts.end(), path) != m.scripts.end()) {
                    c.issue(field, "is listed twice (or is the main script)");
                } else {
                    m.scripts.push_back(path);
                }
            }
        }
    }
    if (doc.contains("commands")) {
        const Json& commands = doc["commands"];
        if (!commands.is_array() || commands.size() > kMaxDeclaredCommands) {
            c.issue("commands", "must be an array of at most " + std::to_string(kMaxDeclaredCommands) + " commands");
        } else {
            for (std::size_t i = 0; i < commands.size(); ++i) {
                const std::string field = "commands[" + std::to_string(i) + "]";
                const Json& cmd = commands[i];
                if (!cmd.is_object()) {
                    c.issue(field, "must be an object {\"id\", \"title\"}");
                    continue;
                }
                Checker inner(cmd);
                DeclaredCommand d;
                const auto id = inner.text("id", 64, true);
                const auto title = inner.text("title", kMaxTitleBytes, true);
                for (ManifestIssue& issue : inner.take()) {
                    c.issue(field + "." + issue.field, std::move(issue.message));
                }
                if (!id || !title) {
                    continue;
                }
                if (!isValidCommandId(*id)) {
                    c.issue(field + ".id", "must match [A-Za-z][A-Za-z0-9._-]* (at most 64 characters)");
                } else if (std::any_of(m.commands.begin(), m.commands.end(),
                                       [&](const DeclaredCommand& other) { return other.id == *id; })) {
                    c.issue(field + ".id", "is declared twice");
                } else {
                    m.commands.push_back({*id, *title});
                }
            }
        }
    }

    if (!c.ok()) {
        return std::unexpected(ManifestError{c.take()});
    }
    return m;
}

Json toJson(const Manifest& m) {
    Json caps = Json::array();
    for (const command::Capability cap : m.capabilities) {
        caps.push_back(command::toString(cap));
    }
    Json commands = Json::array();
    for (const DeclaredCommand& d : m.commands) {
        commands.push_back({{"id", d.id}, {"title", d.title}});
    }
    Json j{{"manifestVersion", kManifestVersion},
           {"id", m.id},
           {"name", m.name},
           {"version", toString(m.version)},
           {"author", m.author},
           {"licence", m.licence},
           {"minWabcutVersion", toString(m.minWabcutVersion)},
           {"capabilities", caps},
           {"main", m.main}};
    if (!m.description.empty()) {
        j["description"] = m.description;
    }
    if (!m.scripts.empty()) {
        j["scripts"] = m.scripts;
    }
    if (!commands.empty()) {
        j["commands"] = commands;
    }
    return j;
}

} // namespace wabcut::script
