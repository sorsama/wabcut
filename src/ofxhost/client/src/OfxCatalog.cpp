#include <wabcut/ofxhost/OfxCatalog.h>

#include "OfxParams.h"

#include <wabcut/plughost/Ipc.h>

#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

using nlohmann::json;
namespace ipc = wabcut::plughost;

namespace wabcut::ofxhost {

// ---- Parameters ---------------------------------------------------------------------------------------------------

int OfxParamInfo::components() const {
    switch (kind) {
    case OfxParamKind::Rgba:
        return 4;
    case OfxParamKind::Rgb:
    case OfxParamKind::Double3D:
    case OfxParamKind::Int3D:
        return 3;
    case OfxParamKind::Double2D:
    case OfxParamKind::Int2D:
        return 2;
    case OfxParamKind::String:
        return 0;
    default:
        return 1;
    }
}

std::vector<std::string> OfxParamInfo::keys() const {
    std::vector<std::string> out;
    const auto suffixes = componentSuffixes(kind);
    if (suffixes.empty()) {
        if (kind != OfxParamKind::String) {
            out.push_back(name);
        }
        return out;
    }
    for (const char* s : suffixes) {
        out.push_back(name + "." + s);
    }
    return out;
}

const OfxParamInfo* OfxPluginInfo::param(std::string_view n) const {
    for (const OfxParamInfo& p : params) {
        if (p.name == n) {
            return &p;
        }
    }
    return nullptr;
}

std::optional<std::string> ofxPluginIdOf(std::string_view effectId) {
    if (!effectId.starts_with(kOfxEffectPrefix) || effectId.size() == kOfxEffectPrefix.size()) {
        return std::nullopt;
    }
    return std::string(effectId.substr(kOfxEffectPrefix.size()));
}

namespace {

std::string pathKey(const std::filesystem::path& p) { return ipc::narrow(p.lexically_normal().wstring()); }

std::filesystem::path envPath(const wchar_t* name) {
    const wchar_t* v = _wgetenv(name);
    return v != nullptr ? std::filesystem::path(v) : std::filesystem::path();
}

struct Fingerprint {
    std::uintmax_t size = 0;
    std::int64_t modified = 0;
    bool operator==(const Fingerprint&) const = default;
};

Fingerprint fingerprintOf(const std::filesystem::path& path) {
    std::error_code ec;
    Fingerprint f;
    f.size = std::filesystem::file_size(path, ec);
    if (ec) {
        f.size = 0;
    }
    const auto t = std::filesystem::last_write_time(path, ec);
    f.modified = ec ? 0 : static_cast<std::int64_t>(t.time_since_epoch().count());
    return f;
}

OfxParamKind kindOfName(const std::string& s, bool& ok) {
    ok = true;
    if (s == "double") {
        return OfxParamKind::Double;
    }
    if (s == "int") {
        return OfxParamKind::Int;
    }
    if (s == "bool") {
        return OfxParamKind::Bool;
    }
    if (s == "choice" || s == "strchoice") {
        return OfxParamKind::Choice;
    }
    if (s == "rgb") {
        return OfxParamKind::Rgb;
    }
    if (s == "rgba") {
        return OfxParamKind::Rgba;
    }
    if (s == "double2d") {
        return OfxParamKind::Double2D;
    }
    if (s == "int2d") {
        return OfxParamKind::Int2D;
    }
    if (s == "double3d") {
        return OfxParamKind::Double3D;
    }
    if (s == "int3d") {
        return OfxParamKind::Int3D;
    }
    if (s == "string") {
        return OfxParamKind::String;
    }
    ok = false; // custom, bytes, parametric, buttons, groups and pages are not offered
    return OfxParamKind::Double;
}

std::string_view nameOfKind(OfxParamKind k) {
    switch (k) {
    case OfxParamKind::Double:
        return "double";
    case OfxParamKind::Int:
        return "int";
    case OfxParamKind::Bool:
        return "bool";
    case OfxParamKind::Choice:
        return "choice";
    case OfxParamKind::Rgb:
        return "rgb";
    case OfxParamKind::Rgba:
        return "rgba";
    case OfxParamKind::Double2D:
        return "double2d";
    case OfxParamKind::Int2D:
        return "int2d";
    case OfxParamKind::Double3D:
        return "double3d";
    case OfxParamKind::Int3D:
        return "int3d";
    case OfxParamKind::String:
        return "string";
    }
    return "double";
}

/// Numbers from a JSON array, never throwing (what a plug-in describes is untrusted).
std::vector<double> numberList(const json& j, const char* key, std::size_t limit = 8) {
    std::vector<double> out;
    if (!j.is_object()) {
        return out;
    }
    if (const auto it = j.find(key); it != j.end() && it->is_array()) {
        for (const json& v : *it) {
            if (out.size() >= limit) {
                break;
            }
            out.push_back(v.is_number() ? v.get<double>() : 0.0);
        }
    }
    return out;
}

std::string text(const json& j, const char* key, std::size_t limit = 4096) {
    std::string s;
    if (j.is_object()) {
        if (const auto it = j.find(key); it != j.end() && it->is_string()) {
            s = it->get<std::string>();
        }
    }
    if (s.size() > limit) {
        s.resize(limit);
    }
    return s;
}

bool flag(const json& j, const char* key, bool fallback) {
    if (j.is_object()) {
        if (const auto it = j.find(key); it != j.end() && it->is_boolean()) {
            return it->get<bool>();
        }
    }
    return fallback;
}

/// The effect-parameter-safe form of an identifier: the portable characters a model id may hold.
bool portableId(const std::string& id) {
    if (id.empty() || id.size() > 200) {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '_' || c == '-' || c == ':'; });
}

json toJson(const OfxPluginInfo& p) {
    json params = json::array();
    for (const OfxParamInfo& q : p.params) {
        params.push_back(json{{"name", q.name},
                              {"kind", std::string(nameOfKind(q.kind))},
                              {"label", q.label},
                              {"hint", q.hint},
                              {"animates", q.animates},
                              {"default", q.defaults},
                              {"min", q.mins},
                              {"max", q.maxs},
                              {"dmin", q.displayMins},
                              {"dmax", q.displayMaxs},
                              {"options", q.options},
                              {"text", q.textDefault},
                              {"multiline", q.multiline},
                              {"angle", q.angle},
                              {"scale", q.scale},
                              {"spatial", q.spatial},
                              {"pixels", q.pixels}});
    }
    return json{{"id", p.id},           {"name", p.name},        {"group", p.group},           {"description", p.description},
                {"version", p.version}, {"context", p.context},  {"depth", p.depth},           {"components", p.components},
                {"singleInstance", p.singleInstance}, {"path", ipc::narrow(p.path.wstring())}, {"problem", p.problem},
                {"params", std::move(params)}};
}

/// A parameter from a scan or the cache (both untrusted); false for the kinds that are not offered.
bool paramFromJson(const json& q, bool fromScan, OfxParamInfo& out) {
    if (!q.is_object()) {
        return false;
    }
    bool known = false;
    out.kind = kindOfName(text(q, "kind"), known);
    if (!known) {
        return false;
    }
    out.name = text(q, "name", 256);
    if (out.name.empty()) {
        return false;
    }
    out.label = text(q, "label", 256);
    if (out.label.empty()) {
        out.label = out.name;
    }
    out.hint = text(q, "hint");
    out.animates = flag(q, "animates", true);
    if (flag(q, "secret", false)) {
        return false; // hidden by the plug-in
    }
    const int n = out.components();
    if (out.kind == OfxParamKind::String) {
        out.textDefault = fromScan ? text(q, "default") : text(q, "text");
        out.multiline = fromScan ? text(q, "stringMode") == "OfxParamStringIsMultiLine" : flag(q, "multiline", false);
        return true;
    }
    out.defaults = numberList(q, "default");
    out.mins = numberList(q, "min");
    out.maxs = numberList(q, "max");
    out.displayMins = numberList(q, "dmin");
    out.displayMaxs = numberList(q, "dmax");
    for (auto* v : {&out.defaults, &out.mins, &out.maxs, &out.displayMins, &out.displayMaxs}) {
        v->resize(static_cast<std::size_t>(n), 0.0);
    }
    if (out.kind == OfxParamKind::Choice) {
        if (const auto it = q.find("options"); it != q.end() && it->is_array()) {
            for (const json& o : *it) {
                if (out.options.size() < kMaxOptionsKept && o.is_string()) {
                    out.options.push_back(o.get<std::string>());
                }
            }
        }
        if (out.options.empty()) {
            out.kind = OfxParamKind::Int; // a menu without labels is a number
        }
    }
    if (fromScan) {
        const std::string type = text(q, "doubleType");
        const std::string coords = text(q, "coords");
        out.angle = type == "OfxParamDoubleTypeAngle";
        out.scale = type == "OfxParamDoubleTypeScale";
        const bool position = type == "OfxParamDoubleTypeX" || type == "OfxParamDoubleTypeY" || type == "OfxParamDoubleTypeXY" ||
                              type == "OfxParamDoubleTypeXAbsolute" || type == "OfxParamDoubleTypeYAbsolute" ||
                              type == "OfxParamDoubleTypeXYAbsolute";
        out.spatial = position && coords == "OfxParamCoordinatesNormalised";
        out.pixels = position && !out.spatial;
    } else {
        out.angle = flag(q, "angle", false);
        out.scale = flag(q, "scale", false);
        out.spatial = flag(q, "spatial", false);
        out.pixels = flag(q, "pixels", false);
    }
    return true;
}

/// A plug-in from scan output or the cache. Both are untrusted (a plug-in prints into a scan, a user edits the cache).
OfxPluginInfo pluginFromJson(const json& j, const std::filesystem::path& path, bool fromScan) {
    OfxPluginInfo p;
    p.id = text(j, "id", 256);
    p.name = text(j, "name", 256);
    if (p.name.empty()) {
        p.name = p.id;
    }
    p.group = text(j, "group", 256);
    p.description = text(j, "description");
    p.version = text(j, "version", 64);
    p.context = text(j, "context", 32);
    p.depth = text(j, "depth", 16);
    p.components = text(j, "components", 16);
    p.singleInstance = flag(j, "singleInstance", false);
    p.path = path;
    p.problem = text(j, "problem");
    if (fromScan && !flag(j, "usable", false) && p.problem.empty()) {
        p.problem = "the plug-in is not usable";
    }
    if (p.problem.empty() && !portableId(p.id)) {
        p.problem = "its identifier has characters Wabcut cannot keep in a project";
    }
    if (const auto it = j.find("params"); it != j.end() && it->is_array()) {
        std::set<std::string> keys;
        for (const json& q : *it) {
            if (p.params.size() >= kMaxParams) {
                break;
            }
            OfxParamInfo param;
            if (!paramFromJson(q, fromScan, param)) {
                continue;
            }
            bool clash = false;
            for (const std::string& k : param.keys()) {
                clash = clash || !keys.insert(k).second;
            }
            if (param.kind == OfxParamKind::String) {
                clash = clash || !keys.insert(param.name).second;
            }
            if (!clash) {
                p.params.push_back(std::move(param));
            }
        }
    }
    return p;
}

/// The plug-ins' descriptors for the model: kept forever once handed out (EffectDescriptorProvider's contract).
class Descriptors final : public EffectDescriptorProvider {
public:
    const EffectDescriptor* find(std::string_view id) const override {
        std::lock_guard lock(m_mutex);
        const auto it = m_byId.find(id);
        return it != m_byId.end() ? it->second : nullptr;
    }

    void update(const std::vector<OfxPluginInfo>& plugins) {
        std::lock_guard lock(m_mutex);
        std::map<std::string, const EffectDescriptor*, std::less<>> next;
        for (const OfxPluginInfo& p : plugins) {
            if (!p.problem.empty()) {
                continue;
            }
            const std::string id = p.effectId();
            const std::string signature = toJson(p).dump(-1, ' ', false, json::error_handler_t::replace);
            const auto old = m_owned.find(id);
            if (old != m_owned.end() && old->second->signature == signature) {
                next.emplace(id, &old->second->descriptor);
                continue;
            }
            Owned& o = m_storage.emplace_back();
            o.signature = signature;
            o.id = id;
            o.name = p.name;
            o.category = "OpenFX Plug-ins/" + (p.group.empty() ? std::string("Other") : p.group);
            o.description = p.name + (p.version.empty() ? "" : " " + p.version) + " (OpenFX plug-in)" +
                            (p.description.empty() ? "" : ": " + p.description);
            for (const OfxParamInfo& q : p.params) {
                if (q.kind == OfxParamKind::String) {
                    o.texts.push_back(EffectTextInfo{keep(o, q.name), keep(o, q.label), keep(o, q.textDefault), q.multiline});
                    continue;
                }
                for (const DescriptorParam& d : descriptorParams(q)) {
                    o.params.push_back(EffectParamInfo{keep(o, d.key), keep(o, d.display), d.unit, d.defaultValue, d.minValue, d.maxValue,
                                                       d.animatable, d.options.empty() ? std::string_view{} : keep(o, d.options)});
                }
            }
            o.descriptor.id = o.id;
            o.descriptor.version = 1;
            o.descriptor.displayName = o.name;
            o.descriptor.category = o.category;
            o.descriptor.kind = EffectKind::Video;
            o.descriptor.params = o.params;
            o.descriptor.capabilities = EffectCapability::UsesAlpha;
            o.descriptor.description = o.description;
            o.descriptor.texts = o.texts;
            m_owned[id] = &o;
            next.emplace(id, &o.descriptor);
        }
        m_byId = std::move(next);
    }

private:
    struct Owned {
        std::string signature;
        std::string id;
        std::string name;
        std::string category;
        std::string description;
        std::deque<std::string> strings; ///< stable addresses: the descriptor's views point here
        std::vector<EffectParamInfo> params;
        std::vector<EffectTextInfo> texts;
        EffectDescriptor descriptor;
    };

    static std::string_view keep(Owned& o, const std::string& s) {
        o.strings.push_back(s);
        return o.strings.back();
    }

    mutable std::mutex m_mutex;
    std::deque<Owned> m_storage; ///< never shrinks: descriptors stay valid
    std::map<std::string, Owned*, std::less<>> m_owned;
    std::map<std::string, const EffectDescriptor*, std::less<>> m_byId;
};

struct FileEntry {
    Fingerprint fingerprint;
    std::vector<OfxPluginInfo> plugins;
    std::string error;
};

struct BlockedEntry {
    Fingerprint fingerprint;
    std::string reason;
};

/// Scans one plug-in binary in its own helper process. Sets `blocked` when the helper crashed or hung on it, `cancelled`
/// when `stop` was requested first (the helper is stopped at once; the file is not blamed).
std::expected<std::vector<OfxPluginInfo>, std::string> scanFile(const std::filesystem::path& helper, const std::filesystem::path& path,
                                                                std::chrono::milliseconds timeout, const std::stop_token& stop,
                                                                bool& blocked, bool& cancelled) {
    blocked = false;
    cancelled = false;
    std::string error;
    auto child = ipc::ChildProcess::start(helper, {"--scan", ipc::narrow(path.wstring())}, {}, error, ipc::ChildLimits::restricted());
    if (!child) {
        return std::unexpected("cannot start the OpenFX helper: " + error);
    }
    std::optional<std::string> line;
    std::thread reader([&] {
        try {
            line = child->readLine();
            while (child->readLine()) {
            }
        } catch (...) {
            child->terminate(); // out of memory reading it: no answer
        }
    });
    // Waits in short slices so a stop (quitting the app, cancelling the scan) never waits for a hung plug-in.
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::optional<std::uint32_t> code = child->wait(std::chrono::milliseconds(50));
    while (!code) {
        if (stop.stop_requested()) {
            child->terminate();
            reader.join();
            cancelled = true;
            return std::unexpected(std::string("the scan was cancelled"));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        code = child->wait(std::chrono::milliseconds(50));
    }
    if (!code) {
        child->terminate();
        reader.join();
        blocked = true;
        return std::unexpected("did not finish scanning within " + std::to_string(timeout.count() / 1000) + " s");
    }
    reader.join();
    const json result = line ? json::parse(*line, nullptr, false) : json();
    if (*code != 0 && *code != 2) {
        blocked = true;
        char hex[16];
        std::snprintf(hex, sizeof hex, "0x%08X", static_cast<unsigned>(*code));
        return std::unexpected(std::string("crashed while being scanned (exit code ") + hex + ")");
    }
    try {
        if (!result.is_object() || !flag(result, "ok", false)) {
            return std::unexpected(result.is_object() ? text(result, "error", 512) : std::string("no answer"));
        }
        std::vector<OfxPluginInfo> out;
        const auto plugins = result.find("plugins");
        if (plugins == result.end() || !plugins->is_array()) {
            return std::unexpected(std::string("the scan's answer lists no plug-ins"));
        }
        for (const json& j : *plugins) {
            if (!j.is_object()) {
                return std::unexpected(std::string("the scan's answer is malformed"));
            }
            out.push_back(pluginFromJson(j, path, true));
        }
        return out;
    } catch (const json::exception& e) {
        return std::unexpected(std::string("the scan's answer is malformed (") + e.what() + ")");
    }
}

bool endsWithNoCase(std::wstring s, std::wstring_view suffix) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s.size() >= suffix.size() && std::wstring_view(s).substr(s.size() - suffix.size()) == suffix;
}

/// Plug-in binaries under `folder`: the .ofx files inside `<name>.ofx.bundle/Contents/Win64`, and loose .ofx files. Bundles
/// are not descended into further (their resources hold no plug-ins).
void collect(const std::filesystem::path& folder, std::vector<std::filesystem::path>& out) {
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec)) {
        return;
    }
    std::filesystem::recursive_directory_iterator it(folder, std::filesystem::directory_options::skip_permission_denied, ec);
    for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        const std::filesystem::path& p = it->path();
        const bool directory = it->is_directory(ec);
        const std::wstring name = p.filename().wstring();
        if (directory && endsWithNoCase(name, L".ofx.bundle")) {
            for (const char* sub : {"Win64", "x86_64-win"}) {
                const std::filesystem::path binaries = p / "Contents" / sub;
                std::error_code e2;
                for (const auto& entry : std::filesystem::directory_iterator(binaries, e2)) {
                    if (entry.is_regular_file(e2) && endsWithNoCase(entry.path().filename().wstring(), L".ofx")) {
                        out.push_back(entry.path());
                    }
                }
            }
            it.disable_recursion_pending();
        } else if (!directory && endsWithNoCase(name, L".ofx")) {
            out.push_back(p);
        } else if (directory && it.depth() >= 6) {
            it.disable_recursion_pending();
        }
    }
}

} // namespace

struct OfxCatalog::Impl {
    OfxCatalogOptions options;
    mutable std::mutex mutex;
    std::map<std::string, FileEntry> files;
    std::map<std::string, BlockedEntry> blocked;
    std::vector<std::filesystem::path> userFolders;
    std::shared_ptr<Descriptors> descriptors = std::make_shared<Descriptors>();
    std::atomic<bool> scanning{false};
    std::function<void()> changed;

    std::vector<OfxPluginInfo> allPlugins() const {
        std::vector<OfxPluginInfo> out;
        for (const auto& [path, entry] : files) {
            out.insert(out.end(), entry.plugins.begin(), entry.plugins.end());
        }
        return out;
    }

    /// Caller holds `mutex`.
    void publish() { descriptors->update(allPlugins()); }

    /// Caller holds `mutex`.
    void save() const {
        if (options.cacheFile.empty()) {
            return;
        }
        json j{{"version", 1}};
        json f = json::object();
        for (const auto& [path, entry] : files) {
            json plugins = json::array();
            for (const OfxPluginInfo& p : entry.plugins) {
                plugins.push_back(toJson(p));
            }
            f[path] = json{{"size", entry.fingerprint.size}, {"modified", entry.fingerprint.modified}, {"error", entry.error},
                           {"plugins", std::move(plugins)}};
        }
        j["files"] = std::move(f);
        json b = json::object();
        for (const auto& [path, entry] : blocked) {
            b[path] = json{{"size", entry.fingerprint.size}, {"modified", entry.fingerprint.modified}, {"reason", entry.reason}};
        }
        j["blocked"] = std::move(b);
        json folders = json::array();
        for (const auto& folder : userFolders) {
            folders.push_back(ipc::narrow(folder.wstring()));
        }
        j["userFolders"] = std::move(folders);
        std::error_code ec;
        std::filesystem::create_directories(options.cacheFile.parent_path(), ec);
        const std::filesystem::path temp = options.cacheFile.wstring() + L".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out << j.dump(1, ' ', false, json::error_handler_t::replace);
            if (!out) {
                return;
            }
        }
        std::filesystem::rename(temp, options.cacheFile, ec);
    }

    std::vector<std::filesystem::path> allFolders() const {
        std::vector<std::filesystem::path> out = options.folders.empty() ? OfxCatalog::standardFolders() : options.folders;
        out.insert(out.end(), userFolders.begin(), userFolders.end());
        return out;
    }
};

OfxCatalog::OfxCatalog(OfxCatalogOptions options) : m_impl(std::make_shared<Impl>()) {
    if (options.helper.empty()) {
        options.helper = defaultHelper();
    }
    m_impl->options = std::move(options);
}

OfxCatalog::~OfxCatalog() {
    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_worker.join();
    }
}

std::vector<std::filesystem::path> OfxCatalog::standardFolders() {
    std::vector<std::filesystem::path> out;
    const auto add = [&out](const std::filesystem::path& folder) {
        std::error_code ec;
        if (!folder.empty() && std::filesystem::is_directory(folder, ec) &&
            std::find(out.begin(), out.end(), folder.lexically_normal()) == out.end()) {
            out.push_back(folder.lexically_normal());
        }
    };
    add(envPath(L"COMMONPROGRAMFILES") / "OFX" / "Plugins");
    // OFX_PLUGIN_PATH: extra folders, separated by ';' (the OpenFX convention on Windows).
    if (const wchar_t* extra = _wgetenv(L"OFX_PLUGIN_PATH"); extra != nullptr) {
        std::wstring list = extra;
        std::size_t start = 0;
        while (start <= list.size()) {
            const std::size_t end = list.find(L';', start);
            const std::wstring entry = list.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
            if (!entry.empty()) {
                add(std::filesystem::path(entry));
            }
            if (end == std::wstring::npos) {
                break;
            }
            start = end + 1;
        }
    }
    return out;
}

std::filesystem::path OfxCatalog::defaultCacheFile() {
    const std::filesystem::path local = envPath(L"LOCALAPPDATA");
    return local.empty() ? std::filesystem::path() : local / "Wabcut" / "ofx-plugins.json";
}

std::filesystem::path OfxCatalog::defaultHelper() {
    wchar_t buffer[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return std::filesystem::path(std::wstring(buffer, n)).parent_path() / "wabcut-ofxhost.exe";
}

bool OfxCatalog::load() {
    Impl& d = *m_impl;
    if (d.options.cacheFile.empty()) {
        return false;
    }
    std::ifstream in(d.options.cacheFile, std::ios::binary);
    if (!in) {
        return false;
    }
    std::stringstream textStream;
    textStream << in.rdbuf();
    const json j = json::parse(textStream.str(), nullptr, false);
    // Parsed in full before anything is replaced: a cache with a field of the wrong type is not read (never thrown).
    std::map<std::string, FileEntry> files;
    std::map<std::string, BlockedEntry> blocked;
    std::vector<std::filesystem::path> userFolders;
    try {
        if (!j.is_object() || j.value("version", 0) != 1) {
            return false;
        }
        // Named copies: items() refers to the object, and a temporary in the loop header would be gone by then.
        const json fileEntries = j.value("files", json::object());
        for (const auto& [path, entry] : fileEntries.items()) {
            FileEntry f;
            f.fingerprint = {entry.value("size", std::uintmax_t{0}), entry.value("modified", std::int64_t{0})};
            f.error = entry.value("error", "");
            for (const json& p : entry.value("plugins", json::array())) {
                f.plugins.push_back(pluginFromJson(p, std::filesystem::path(ipc::widen(path)), false));
            }
            files[path] = std::move(f);
        }
        const json blockedEntries = j.value("blocked", json::object());
        for (const auto& [path, entry] : blockedEntries.items()) {
            blocked[path] = BlockedEntry{{entry.value("size", std::uintmax_t{0}), entry.value("modified", std::int64_t{0})},
                                         entry.value("reason", "")};
        }
        for (const json& folder : j.value("userFolders", json::array())) {
            userFolders.emplace_back(ipc::widen(folder.get<std::string>()));
        }
    } catch (const json::exception&) {
        return false;
    }
    std::function<void()> callback;
    {
        std::lock_guard lock(d.mutex);
        d.files = std::move(files);
        d.blocked = std::move(blocked);
        d.userFolders = std::move(userFolders);
        d.publish();
        callback = d.changed;
    }
    if (callback) {
        callback();
    }
    return true;
}

OfxScanReport OfxCatalog::scan(std::stop_token stop, const std::function<void(double, std::string_view)>& progress, bool rescan) {
    Impl& d = *m_impl;
    d.scanning.store(true);
    OfxScanReport report;
    std::vector<std::filesystem::path> candidates;
    std::set<std::string> knownBefore;
    {
        std::lock_guard lock(d.mutex);
        for (const auto& folder : d.allFolders()) {
            collect(folder, candidates);
        }
        for (const OfxPluginInfo& p : d.allPlugins()) {
            knownBefore.insert(p.effectId());
        }
    }
    report.files = static_cast<int>(candidates.size());
    std::map<std::string, FileEntry> found;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (stop.stop_requested()) {
            report.cancelled = true;
            break;
        }
        const std::filesystem::path& path = candidates[i];
        const std::string key = pathKey(path);
        const Fingerprint fp = fingerprintOf(path);
        if (progress) {
            progress(static_cast<double>(i) / static_cast<double>(std::max<std::size_t>(candidates.size(), 1)), key);
        }
        {
            std::lock_guard lock(d.mutex);
            if (const auto b = d.blocked.find(key); b != d.blocked.end()) {
                if (b->second.fingerprint == fp) {
                    ++report.skipped;
                    continue;
                }
                d.blocked.erase(b); // the file changed: try it again
            }
            if (const auto f = d.files.find(key); !rescan && f != d.files.end() && f->second.fingerprint == fp) {
                found[key] = f->second;
                ++report.cached;
                continue;
            }
        }
        bool blocked = false;
        bool cancelled = false;
        auto result = scanFile(d.options.helper, path, d.options.scanTimeout, stop, blocked, cancelled);
        if (cancelled) {
            report.cancelled = true;
            break;
        }
        ++report.scanned;
        std::lock_guard lock(d.mutex);
        if (blocked) {
            d.blocked[key] = BlockedEntry{fp, result.error()};
            report.blocked.push_back(OfxBlocked{path, result.error()});
            continue;
        }
        FileEntry entry;
        entry.fingerprint = fp;
        if (result) {
            entry.plugins = std::move(*result);
        } else {
            entry.error = result.error();
            report.errors.push_back(key + ": " + result.error());
        }
        found[key] = std::move(entry);
    }
    std::function<void()> callback;
    {
        std::lock_guard lock(d.mutex);
        if (!report.cancelled) {
            d.files = std::move(found); // files that went away are forgotten
        } else {
            for (auto& [key, entry] : found) {
                d.files[key] = std::move(entry);
            }
        }
        for (const OfxPluginInfo& p : d.allPlugins()) {
            ++report.plugins;
            report.added += knownBefore.contains(p.effectId()) ? 0 : 1;
        }
        d.publish();
        d.save();
        callback = d.changed;
    }
    if (progress) {
        progress(1.0, {});
    }
    d.scanning.store(false);
    if (callback) {
        callback();
    }
    return report;
}

void OfxCatalog::startScan(std::function<void(const OfxScanReport&)> done, bool rescan) {
    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_worker.join();
    }
    m_impl->scanning.store(true);
    m_worker = std::jthread([this, done = std::move(done), rescan](std::stop_token stop) {
        // A thread body: nothing may escape it (std::terminate would end the editor).
        OfxScanReport report;
        try {
            report = scan(stop, {}, rescan);
        } catch (const std::exception& e) {
            report.errors.push_back(std::string("the OpenFX scan failed: ") + e.what());
            m_impl->scanning.store(false);
        }
        try {
            if (done) {
                done(report);
            }
        } catch (...) {
        }
    });
}

bool OfxCatalog::scanning() const { return m_impl->scanning.load(); }

std::vector<OfxPluginInfo> OfxCatalog::plugins() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->allPlugins();
}

std::optional<OfxPluginInfo> OfxCatalog::find(std::string_view effectId) const {
    std::lock_guard lock(m_impl->mutex);
    for (const auto& [path, entry] : m_impl->files) {
        for (const OfxPluginInfo& p : entry.plugins) {
            if (p.effectId() == effectId) {
                return p;
            }
        }
    }
    return std::nullopt;
}

std::vector<OfxBlocked> OfxCatalog::blocked() const {
    std::lock_guard lock(m_impl->mutex);
    std::vector<OfxBlocked> out;
    for (const auto& [path, entry] : m_impl->blocked) {
        out.push_back(OfxBlocked{std::filesystem::path(ipc::widen(path)), entry.reason});
    }
    return out;
}

void OfxCatalog::unblock(const std::filesystem::path& path) {
    std::lock_guard lock(m_impl->mutex);
    m_impl->blocked.erase(pathKey(path));
    m_impl->save();
}

void OfxCatalog::clearBlocklist() {
    std::lock_guard lock(m_impl->mutex);
    m_impl->blocked.clear();
    m_impl->save();
}

std::vector<std::filesystem::path> OfxCatalog::userFolders() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->userFolders;
}

void OfxCatalog::addUserFolder(const std::filesystem::path& folder) {
    std::lock_guard lock(m_impl->mutex);
    const std::filesystem::path normal = folder.lexically_normal();
    if (std::find(m_impl->userFolders.begin(), m_impl->userFolders.end(), normal) == m_impl->userFolders.end()) {
        m_impl->userFolders.push_back(normal);
        m_impl->save();
    }
}

void OfxCatalog::removeUserFolder(const std::filesystem::path& folder) {
    std::lock_guard lock(m_impl->mutex);
    std::erase(m_impl->userFolders, folder.lexically_normal());
    m_impl->save();
}

std::vector<std::filesystem::path> OfxCatalog::folders() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->allFolders();
}

std::shared_ptr<const EffectDescriptorProvider> OfxCatalog::descriptors() const { return m_impl->descriptors; }

void OfxCatalog::setChangedCallback(std::function<void()> callback) {
    std::lock_guard lock(m_impl->mutex);
    m_impl->changed = std::move(callback);
}

const OfxCatalogOptions& OfxCatalog::options() const { return m_impl->options; }

} // namespace wabcut::ofxhost
