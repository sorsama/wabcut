// OGraf packages (EBU Open Graphics), 0.7 (card 178): OGraf.h.
//
// The manifest follows https://ograf.ebu.io/v1/specification/json-schemas/graphics/schema.json: required "$schema", "id",
// "name", "main", "supportsRealTime" and "supportsNonRealTime"; optional "version", "description", "author" { name },
// "customActions" [{ id }], "actionDurations", "stepCount", "schema" (a JSON Schema object whose properties are the
// graphic's data: "type", "title", "description", "default", "enum", "gddType", "hidden", "order"), "renderRequirements",
// "thumbnails" [{ file }] and vendor fields starting with "v_". The data types are those of the Graphics Data Definition
// (https://ograf.ebu.io/v1/specification/docs/Specification.html, "Manifest Model").

#include <wabcut/interchange/OGraf.h>

#include <wabcut/interchange/Lottie.h>
#include <wabcut/interchange/Zip.h>

#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>

namespace wabcut::interchange {

namespace {

using Json = nlohmann::json;

constexpr const char* kSchemaV1 = "https://ograf.ebu.io/v1/specification/json-schemas/graphics/schema.json";
constexpr std::size_t kMaxText = 256;
constexpr std::size_t kMaxChoices = 64;
constexpr std::size_t kMaxCustomActions = 256;
constexpr std::size_t kMaxThumbnails = 64;
constexpr std::size_t kMaxLottieCandidates = 8;

std::string bounded(std::string text, std::size_t max) {
    std::erase_if(text, [](char c) { return static_cast<unsigned char>(c) < 0x20 && c != '\n' && c != '\t'; });
    if (text.size() > max) {
        std::size_t n = max;
        while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) {
            --n;
        }
        text.resize(n);
    }
    return text;
}

const Json* member(const Json& j, const char* key) {
    if (!j.is_object()) {
        return nullptr;
    }
    const auto it = j.find(key);
    return it == j.end() || it->is_null() ? nullptr : &*it;
}

std::string text(const Json& j, const char* key, std::size_t max = kMaxText) {
    const Json* m = member(j, key);
    return m && m->is_string() ? bounded(m->get<std::string>(), max) : std::string();
}

bool hasSuffix(const std::string& s, std::string_view suffix) {
    if (s.size() < suffix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[s.size() - suffix.size() + i])) != suffix[i]) {
            return false;
        }
    }
    return true;
}

bool startsWith(const std::string& s, std::string_view prefix) {
    return s.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), s.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           });
}

/// Lower-case letters and digits only: "Title Color" / "title_color" / "titleColor" compare equal.
std::string normal(std::string_view s) {
    std::string out;
    for (const char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return out;
}

OGrafValueKind kindOf(const std::string& type, const std::string& gddType, bool hasChoices) {
    if (type == "boolean") {
        return OGrafValueKind::Boolean;
    }
    if (type == "number" || type == "integer") {
        return OGrafValueKind::Number;
    }
    if (type != "string") {
        return OGrafValueKind::Other;
    }
    if (startsWith(gddType, "color")) {
        return OGrafValueKind::Color;
    }
    if (startsWith(gddType, "file-path") || gddType.find("image") != std::string::npos) {
        return OGrafValueKind::Image;
    }
    return hasChoices ? OGrafValueKind::Choice : OGrafValueKind::Text;
}

void note(ImportReport& report, ReportOutcome outcome, std::string reason, std::string_view file, const std::string& where) {
    report.add(outcome, ReportCategory::Graphic, std::move(reason), SourceLocation{std::string(file), where, 0});
}

} // namespace

std::expected<OGrafManifest, Error> parseOGrafManifest(std::string_view json, std::string_view fileName) {
    const auto refuse = [](std::string why) {
        return std::unexpected(Error{ErrorCode::ParseError, "OGraf: " + std::move(why)});
    };
    if (json.size() > kMaxOGrafManifestBytes) {
        return refuse("the manifest is larger than 1 MiB");
    }
    if (!io::jsonNestingWithin(json)) {
        return refuse("the manifest nests too deep");
    }
    const Json root = Json::parse(json, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return refuse("the manifest is not a JSON object (UTF-8)");
    }
    OGrafManifest m;
    for (const char* required : {"$schema", "id", "name", "main"}) {
        const Json* v = member(root, required);
        if (!v || !v->is_string() || v->get_ref<const std::string&>().empty()) {
            return refuse(std::string("the manifest has no \"") + required + "\" text");
        }
    }
    for (const char* required : {"supportsRealTime", "supportsNonRealTime"}) {
        const Json* v = member(root, required);
        if (!v || !v->is_boolean()) {
            return refuse(std::string("the manifest has no \"") + required + "\" true or false");
        }
    }
    m.id = text(root, "id", 256);
    m.name = text(root, "name");
    m.version = text(root, "version", 64);
    m.description = text(root, "description", 1024);
    if (const Json* author = member(root, "author"); author && author->is_object()) {
        m.author = text(*author, "name");
    }
    m.supportsRealTime = root["supportsRealTime"].get<bool>();
    m.supportsNonRealTime = root["supportsNonRealTime"].get<bool>();
    if (text(root, "$schema", 512) != kSchemaV1) {
        note(m.report, ReportOutcome::Approximated, "the manifest names another OGraf version than v1: it is read as v1", fileName, "/$schema");
    }
    m.main = root["main"].get<std::string>();
    if (!isSafeZipPath(m.main) || m.main.back() == '/') {
        return refuse("\"main\" is not a path inside the package");
    }
    if (const Json* steps = member(root, "stepCount"); steps && steps->is_number()) {
        const double v = steps->get<double>();
        m.stepCount = std::isfinite(v) ? static_cast<std::int32_t>(std::clamp(v, -1.0, 10000.0)) : 1;
    }
    if (const Json* actions = member(root, "customActions"); actions && actions->is_array()) {
        for (std::size_t i = 0; i < actions->size() && m.customActions.size() < kMaxCustomActions; ++i) {
            const std::string id = text((*actions)[i], "id", 128);
            if (!id.empty()) {
                m.customActions.push_back(id);
            }
        }
    }
    if (const Json* lottie = member(root, "v_wabcut_lottie")) {
        if (!lottie->is_string() || !isSafeZipPath(lottie->get<std::string>()) || lottie->get<std::string>().back() == '/') {
            return refuse("\"v_wabcut_lottie\" is not a path inside the package");
        }
        m.lottie = lottie->get<std::string>();
    }
    if (const Json* thumbs = member(root, "thumbnails"); thumbs && thumbs->is_array()) {
        for (std::size_t i = 0; i < thumbs->size() && m.thumbnails.size() < kMaxThumbnails; ++i) {
            const std::string file = text((*thumbs)[i], "file", 1024);
            if (!file.empty() && isSafeZipPath(file)) {
                m.thumbnails.push_back(file);
            } else {
                note(m.report, ReportOutcome::Dropped, "a thumbnail path that leaves the package is ignored", fileName,
                     "/thumbnails/" + std::to_string(i));
            }
        }
    }
    // The data: schema.properties.
    if (const Json* schema = member(root, "schema"); schema && schema->is_object()) {
        if (const Json* props = member(*schema, "properties"); props && props->is_object()) {
            std::size_t index = 0;
            for (auto it = props->begin(); it != props->end(); ++it, ++index) {
                if (m.properties.size() >= kMaxOGrafProperties) {
                    note(m.report, ReportOutcome::Dropped,
                         "more than " + std::to_string(kMaxOGrafProperties) + " fields: the rest are not read", fileName, "/schema/properties");
                    break;
                }
                if (it.key().empty() || it.key().size() > 128 || !it->is_object()) {
                    continue;
                }
                const Json& p = *it;
                OGrafProperty out;
                out.key = bounded(it.key(), 128);
                out.title = text(p, "title");
                out.description = text(p, "description", 1024);
                out.gddType = text(p, "gddType", 64);
                if (const Json* hidden = member(p, "hidden"); hidden && hidden->is_boolean()) {
                    out.hidden = hidden->get<bool>();
                }
                if (const Json* order = member(p, "order"); order && order->is_number() && std::isfinite(order->get<double>())) {
                    out.order = order->get<double>();
                } else {
                    out.order = 1e9 + static_cast<double>(index); // unordered fields follow the ordered ones, as written
                }
                if (const Json* choices = member(p, "enum"); choices && choices->is_array()) {
                    for (std::size_t i = 0; i < choices->size() && out.choices.size() < kMaxChoices; ++i) {
                        if ((*choices)[i].is_string()) {
                            out.choices.push_back(bounded((*choices)[i].get<std::string>(), kMaxText));
                        }
                    }
                }
                out.kind = kindOf(text(p, "type", 32), out.gddType, !out.choices.empty());
                if (const Json* d = member(p, "default")) {
                    out.hasDefault = true;
                    if (d->is_string()) {
                        out.defaultText = bounded(d->get<std::string>(), kMaxText);
                    } else if (d->is_boolean()) {
                        out.defaultBool = d->get<bool>();
                    } else if (d->is_number() && std::isfinite(d->get<double>())) {
                        out.defaultNumber = d->get<double>();
                    } else {
                        out.hasDefault = false;
                    }
                }
                m.properties.push_back(std::move(out));
            }
            std::stable_sort(m.properties.begin(), m.properties.end(),
                             [](const OGrafProperty& a, const OGrafProperty& b) { return a.order < b.order; });
        }
    }
    // Keys the v1 schema does not know (it forbids them) are ignored.
    static const std::set<std::string> known{"$schema", "id", "version", "main", "name", "description", "author", "customActions",
                                             "actionDurations", "supportsRealTime", "supportsNonRealTime", "stepCount",
                                             "schema", "renderRequirements", "thumbnails"};
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (!known.contains(it.key()) && it.key().rfind("v_", 0) != 0) {
            note(m.report, ReportOutcome::Dropped, "the manifest key \"" + bounded(it.key(), 64) + "\" is not part of OGraf v1 and is ignored",
                 fileName, "/" + bounded(it.key(), 64));
        }
    }
    return m;
}

// ---- Packages ---------------------------------------------------------------------------------------------------------

std::expected<OGrafPackage, Error> openOGrafZip(std::string_view bytes) {
    if (bytes.size() > kMaxOGrafPackageBytes) {
        return std::unexpected(Error{ErrorCode::ParseError, "OGraf: the package is larger than 64 MiB"});
    }
    ZipLimits limits;
    limits.maxEntries = kMaxOGrafPackageFiles;
    limits.maxEntryBytes = kMaxLottieBytes;
    limits.maxTotalBytes = 3 * static_cast<std::uint64_t>(kMaxLottieBytes);
    limits.maxArchiveBytes = kMaxOGrafPackageBytes;
    auto zip = ZipArchive::open(bytes, limits);
    if (!zip) {
        return std::unexpected(Error{ErrorCode::ParseError, "OGraf: " + zip.error().message});
    }
    auto archive = std::make_shared<ZipArchive>(std::move(*zip));
    OGrafPackage package;
    for (const ZipEntry& e : archive->entries()) {
        if (!e.isFolder()) {
            package.files.push_back(e.name);
        }
    }
    package.read = [archive](const std::string& path) -> std::expected<std::string, Error> {
        const ZipEntry* entry = archive->find(path);
        if (!entry || entry->isFolder()) {
            return std::unexpected(Error{ErrorCode::NotFound, "OGraf: " + path.substr(0, 128) + " is not in the package"});
        }
        return archive->read(*entry);
    };
    return package;
}

std::expected<OGrafPackage, Error> openOGrafFolder(const std::string& utf8Folder) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = pathFromUtf8(utf8Folder);
    if (!fs::is_directory(root, ec)) {
        return std::unexpected(Error{ErrorCode::IoError, "OGraf: the folder cannot be read"});
    }
    auto files = std::make_shared<std::map<std::string, fs::path>>();
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        if (it.depth() >= 6) {
            it.disable_recursion_pending();
        }
        const fs::file_status status = it->symlink_status(ec); // a link is never followed
        if (ec || !fs::is_regular_file(status)) {
            continue;
        }
        std::string relative = pathToUtf8(fs::relative(it->path(), root, ec));
        std::replace(relative.begin(), relative.end(), '\\', '/');
        if (ec || !isSafeZipPath(relative)) {
            ec.clear();
            continue;
        }
        if (files->size() >= kMaxOGrafPackageFiles) {
            break;
        }
        files->emplace(std::move(relative), it->path());
    }
    OGrafPackage package;
    for (const auto& [name, path] : *files) {
        package.files.push_back(name);
    }
    package.read = [files](const std::string& name) -> std::expected<std::string, Error> {
        const auto found = files->find(name);
        if (found == files->end()) {
            return std::unexpected(Error{ErrorCode::NotFound, "OGraf: " + name.substr(0, 128) + " is not in the package"});
        }
        std::error_code size_ec;
        const auto size = std::filesystem::file_size(found->second, size_ec);
        if (size_ec || size > kMaxLottieBytes) {
            return std::unexpected(Error{ErrorCode::IoError, "OGraf: " + name.substr(0, 128) + " cannot be read or is larger than 16 MiB"});
        }
        std::ifstream in(found->second, std::ios::binary);
        std::string data(static_cast<std::size_t>(size), '\0');
        if (!in.read(data.data(), static_cast<std::streamsize>(size))) {
            return std::unexpected(Error{ErrorCode::IoError, "OGraf: " + name.substr(0, 128) + " cannot be read"});
        }
        return data;
    };
    return package;
}

std::vector<std::string> ografManifests(const OGrafPackage& package) {
    std::vector<std::string> out;
    for (const std::string& f : package.files) {
        if (hasSuffix(f, ".ograf.json")) {
            out.push_back(f);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string ografTemplateId(std::string_view graphicId) {
    std::string id = "ograf.";
    for (const char c : graphicId) {
        id += (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.' || c == '-') ? c : '-';
    }
    if (id.size() > 128) {
        id.resize(128);
    }
    return id;
}

namespace {

/// Whether a file's bytes are a Lottie animation (JSON with layers and a frame rate) or a dotLottie archive.
bool isLottieFile(const std::string& bytes) {
    if (looksLikeZip(bytes)) {
        const auto ids = dotLottieAnimationIds(bytes);
        return ids && !ids->empty();
    }
    if (bytes.size() > kMaxLottieBytes || !io::jsonNestingWithin(bytes)) {
        return false;
    }
    const Json root = Json::parse(bytes, nullptr, false);
    return root.is_object() && root.contains("layers") && root["layers"].is_array() && root.contains("fr") && root["fr"].is_number();
}

/// The names (normalised) of layer `id` and of the groups around it, innermost first; empty when it is not in `layers`.
bool namesAlong(const std::vector<GraphicLayer>& layers, std::uint32_t id, std::vector<std::string>& out) {
    for (const GraphicLayer& l : layers) {
        if (l.id == id) {
            out.push_back(normal(l.name));
            return true;
        }
        if (namesAlong(l.children, id, out)) {
            out.push_back(normal(l.name));
            return true;
        }
    }
    return false;
}

/// The Lottie field (Text or Color) named like `property`, searched by the names of its layer and the layers around it.
TemplateField* findField(GraphicTemplate& tmpl, FieldKind kind, const OGrafProperty& p, const std::set<std::string>& taken) {
    const std::string key = normal(p.key);
    const std::string title = normal(p.title);
    for (TemplateField& f : tmpl.fields) {
        if (f.kind != kind || (kind == FieldKind::Color && f.property != "fill" && f.property != "stroke") || taken.contains(f.id)) {
            continue;
        }
        std::vector<std::string> names;
        namesAlong(tmpl.graphic.layers, f.layer, names);
        for (const std::string& name : names) {
            if (name.empty()) {
                continue;
            }
            const bool plain = name == key || (!title.empty() && name == title);
            const bool colored = kind == FieldKind::Color &&
                                 (key == name + "color" || key == name + "colour" ||
                                  (!title.empty() && (title == name + "color" || title == name + "colour")));
            if (plain || colored) {
                return &f;
            }
        }
    }
    return nullptr;
}

const GraphicLayer* findNamedLayer(const std::vector<GraphicLayer>& layers, const std::string& wanted) {
    for (const GraphicLayer& l : layers) {
        if (normal(l.name) == wanted) {
            return &l;
        }
        if (const GraphicLayer* inner = findNamedLayer(l.children, wanted)) {
            return inner;
        }
    }
    return nullptr;
}

bool validFieldId(const std::string& id) { return isValidFieldId(id); }

std::string describeFields(const OGrafManifest& m) {
    std::string out;
    std::size_t shown = 0;
    for (const OGrafProperty& p : m.properties) {
        if (shown++ >= 8) {
            out += ", ...";
            break;
        }
        out += (out.empty() ? "" : ", ") + (p.title.empty() ? p.key : p.title);
    }
    return out;
}

} // namespace

std::expected<OGrafManifest, Error> readOGrafManifest(const OGrafPackage& package, std::string_view manifestFile,
                                                      std::string* chosenOut) {
    const std::vector<std::string> manifests = ografManifests(package);
    if (manifests.empty()) {
        return std::unexpected(Error{ErrorCode::ParseError, "OGraf: the package holds no manifest (*.ograf.json)"});
    }
    std::string chosen;
    if (!manifestFile.empty()) {
        const auto it = std::find(manifests.begin(), manifests.end(), std::string(manifestFile));
        if (it == manifests.end()) {
            return std::unexpected(Error{ErrorCode::NotFound, "OGraf: " + std::string(manifestFile).substr(0, 128) + " is not a manifest of the package"});
        }
        chosen = *it;
    } else if (manifests.size() == 1) {
        chosen = manifests.front();
    } else {
        std::string list;
        for (std::size_t i = 0; i < manifests.size() && i < 8; ++i) {
            list += (list.empty() ? "" : ", ") + manifests[i];
        }
        return std::unexpected(Error{ErrorCode::ParseError,
                                     "OGraf: the package holds " + std::to_string(manifests.size()) + " graphics (" + list.substr(0, 400) + "): name one"});
    }
    auto manifestBytes = package.read(chosen);
    if (!manifestBytes) {
        return std::unexpected(manifestBytes.error());
    }
    if (chosenOut) {
        *chosenOut = chosen;
    }
    return parseOGrafManifest(*manifestBytes, chosen);
}

std::expected<OGrafImport, Error> importOGraf(const OGrafPackage& package, std::string_view manifestFile) {
    std::string chosen;
    auto manifest = readOGrafManifest(package, manifestFile, &chosen);
    if (!manifest) {
        return std::unexpected(manifest.error());
    }

    // The Lottie version: the file the manifest names, else the only Lottie animation in the package.
    std::string lottiePath;
    std::string lottieBytes;
    if (!manifest->lottie.empty()) {
        if (std::find(package.files.begin(), package.files.end(), manifest->lottie) == package.files.end()) {
            return std::unexpected(Error{ErrorCode::ParseError, "OGraf: v_wabcut_lottie names " + manifest->lottie.substr(0, 128) + ", which is not in the package"});
        }
        auto bytes = package.read(manifest->lottie);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        lottiePath = manifest->lottie;
        lottieBytes = std::move(*bytes);
    } else {
        std::vector<std::string> found;
        std::string foundBytes;
        std::size_t checked = 0;
        for (const std::string& f : package.files) {
            if (!(hasSuffix(f, ".lottie") || (hasSuffix(f, ".json") && !hasSuffix(f, ".ograf.json"))) || f == manifest->main) {
                continue;
            }
            if (++checked > kMaxLottieCandidates) {
                break;
            }
            auto bytes = package.read(f);
            if (bytes && isLottieFile(*bytes)) {
                found.push_back(f);
                foundBytes = std::move(*bytes);
            }
        }
        if (found.size() > 1) {
            return std::unexpected(Error{ErrorCode::ParseError, "OGraf: the package holds " + std::to_string(found.size()) +
                                                                    " Lottie animations: name the right one in the manifest (\"v_wabcut_lottie\")"});
        }
        if (found.size() == 1) {
            lottiePath = found.front();
            lottieBytes = std::move(foundBytes);
        }
    }
    if (lottiePath.empty()) {
        std::string why = "OGraf: '" + manifest->name + "' is drawn by a web page (" + manifest->main.substr(0, 128) +
                          ") with scripts. Wabcut has no web engine and runs no scripts, so it cannot draw this graphic.";
        if (!manifest->properties.empty()) {
            why += " Its fields (" + describeFields(*manifest) + ") were read.";
        }
        why += " A package that also holds a Lottie version of the graphic (one .json or .lottie animation, or the one named by \"v_wabcut_lottie\" in the manifest) imports as a motion graphic.";
        return std::unexpected(Error{ErrorCode::Unsupported, std::move(why)});
    }

    auto lottie = looksLikeZip(lottieBytes) ? importDotLottie(lottieBytes, lottiePath) : importLottie(lottieBytes, lottiePath);
    if (!lottie) {
        return std::unexpected(lottie.error());
    }

    OGrafImport out;
    out.manifest = std::move(*manifest);
    out.lottieFile = lottiePath;
    out.duration = lottie->duration;
    out.frameRate = lottie->frameRate;
    out.report.append(out.manifest.report);
    out.report.append(lottie->report);
    note(out.report, ReportOutcome::Dropped,
         "the graphic's web page (" + out.manifest.main.substr(0, 128) + ") is not run: the Lottie animation " + lottiePath.substr(0, 128) + " draws it",
         chosen, "/main");

    GraphicTemplate& tmpl = out.tmpl;
    tmpl.id = ografTemplateId(out.manifest.id);
    tmpl.name = out.manifest.name;
    tmpl.description = out.manifest.description;
    tmpl.graphic = std::move(lottie->graphic);
    tmpl.graphic.name = out.manifest.name;
    tmpl.fields = std::move(lottie->fields);
    tmpl.duration = lottie->duration;

    // The manifest's fields name the Lottie's: a layer called like a field is its layer.
    std::set<std::string> renamed; // field ids the manifest took
    std::set<std::string> ids;     // every id in use
    for (const TemplateField& f : tmpl.fields) {
        ids.insert(f.id);
    }
    const auto retitle = [&](TemplateField& f, const OGrafProperty& p) {
        const std::string wanted = validFieldId(p.key) ? p.key : std::string();
        if (!wanted.empty() && (wanted == f.id || !ids.contains(wanted))) {
            ids.erase(f.id);
            f.id = wanted;
            ids.insert(f.id);
        }
        f.label = bounded(p.title.empty() ? p.key : p.title, 96);
        renamed.insert(f.id);
    };
    for (const OGrafProperty& p : out.manifest.properties) {
        const std::string where = "/schema/properties/" + p.key;
        TemplateField* f = nullptr;
        switch (p.kind) {
        case OGrafValueKind::Text:
        case OGrafValueKind::Choice:
            f = findField(tmpl, FieldKind::Text, p, renamed);
            break;
        case OGrafValueKind::Color:
            f = findField(tmpl, FieldKind::Color, p, renamed);
            break;
        case OGrafValueKind::Boolean: {
            const GraphicLayer* layer = findNamedLayer(tmpl.graphic.layers, normal(p.key));
            if (!layer && !p.title.empty()) {
                layer = findNamedLayer(tmpl.graphic.layers, normal(p.title));
            }
            if (layer && tmpl.fields.size() < kMaxTemplateFields) {
                TemplateField checkbox;
                checkbox.id = validFieldId(p.key) && !ids.contains(p.key) ? p.key : "show" + std::to_string(tmpl.fields.size() + 1);
                checkbox.label = bounded(p.title.empty() ? p.key : p.title, 96);
                checkbox.kind = FieldKind::Checkbox;
                checkbox.layer = layer->id;
                checkbox.property = "visible";
                checkbox.value.flag = layer->visible;
                ids.insert(checkbox.id);
                renamed.insert(checkbox.id);
                tmpl.fields.push_back(std::move(checkbox));
                continue;
            }
            break;
        }
        default:
            break;
        }
        if (f) {
            retitle(*f, p);
        } else {
            note(out.report, ReportOutcome::Dropped,
                 "the field \"" + p.key + "\" has no layer of the Lottie with its name" +
                     (p.kind == OGrafValueKind::Text || p.kind == OGrafValueKind::Color || p.kind == OGrafValueKind::Boolean
                          ? " (name a layer like the field to connect them)"
                          : " (Wabcut sets texts, colors and show or hide)"),
                 chosen, where);
        }
    }
    if (const auto problem = templateProblem(tmpl)) {
        return std::unexpected(Error{ErrorCode::ParseError, "internal: the imported OGraf template " + *problem});
    }
    return out;
}

} // namespace wabcut::interchange
