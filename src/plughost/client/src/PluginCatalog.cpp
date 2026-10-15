#include <wabcut/plughost/PluginCatalog.h>

#include <wabcut/plughost/Ipc.h>
#include <wabcut/plughost/Protocol.h>

#include <wabcut/audio/RealTime.h>

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
#include <cstdlib>
#include <deque>
#include <fstream>
#include <cwctype>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

using nlohmann::json;

namespace wabcut::plughost {

using audio::rt::CheckedMutex;

const PluginParam* PluginInfo::param(std::string_view key) const {
    for (const PluginParam& p : params) {
        if (p.key() == key) {
            return &p;
        }
    }
    return nullptr;
}

std::optional<std::pair<std::string, std::string>> parsePluginEffectId(std::string_view effectId) {
    if (!effectId.starts_with(kPluginEffectPrefix)) {
        return std::nullopt;
    }
    const std::string_view rest = effectId.substr(kPluginEffectPrefix.size());
    const auto dot = rest.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= rest.size()) {
        return std::nullopt;
    }
    return std::pair<std::string, std::string>{std::string(rest.substr(0, dot)), std::string(rest.substr(dot + 1))};
}

namespace {

std::string pathKey(const std::filesystem::path& p) { return narrow(p.lexically_normal().wstring()); }

std::filesystem::path envPath(const wchar_t* name) {
    const wchar_t* v = _wgetenv(name);
    return v != nullptr ? std::filesystem::path(v) : std::filesystem::path();
}

/// What changes when a plugin file is updated: the binary's size and time.
struct Fingerprint {
    std::uintmax_t size = 0;
    std::int64_t modified = 0;
    bool operator==(const Fingerprint&) const = default;
};

Fingerprint fingerprintOf(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path binary = path;
    if (std::filesystem::is_directory(path, ec)) {
        const std::filesystem::path folder = path / "Contents" / "x86_64-win";
        binary = folder / path.filename();
        if (!std::filesystem::exists(binary, ec)) { // a vendor's own binary name (as the helper finds it)
            for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
                if (entry.is_regular_file(ec) && entry.path().extension() == ".vst3") {
                    binary = entry.path();
                    break;
                }
            }
        }
    }
    Fingerprint f;
    f.size = std::filesystem::file_size(binary, ec);
    if (ec) {
        f.size = 0;
    }
    const auto t = std::filesystem::last_write_time(binary, ec);
    f.modified = ec ? 0 : static_cast<std::int64_t>(t.time_since_epoch().count());
    return f;
}

json toJson(const PluginInfo& p) {
    json params = json::array();
    for (const PluginParam& q : p.params) {
        params.push_back(json{{"id", q.id},           {"name", q.name},       {"module", q.module}, {"unit", q.unit},
                              {"min", q.minValue},    {"max", q.maxValue},    {"default", q.defaultValue},
                              {"automatable", q.automatable}, {"stepped", q.stepped}, {"hidden", q.hidden}});
    }
    return json{{"format", p.format},     {"id", p.id},         {"name", p.name},       {"vendor", p.vendor},
                {"version", p.version},   {"category", p.category}, {"path", narrow(p.path.wstring())},
                {"latency", p.latency},   {"editor", p.editor}, {"effect", p.effect},   {"problem", p.problem},
                {"params", std::move(params)}};
}

/// A plugin from scan output or the cache. Both are untrusted (a plugin prints into a scan, a user edits the cache):
/// a field of the wrong type throws json::exception, which the callers turn into an error.
PluginInfo pluginFromJson(const json& j, const std::filesystem::path& path) {
    PluginInfo p;
    p.format = j.value("format", "");
    p.id = j.value("id", "");
    p.name = j.value("name", p.id);
    p.vendor = j.value("vendor", "");
    p.version = j.value("version", "");
    p.category = j.value("category", "");
    p.path = path;
    // Scanned at 48 kHz; a latency over the limit (or negative) is a plugin bug: the plugin is not offered. Read as a
    // double, so no value converts out of range.
    const double latency = j.value("latency", 0.0);
    const bool latencyOk = latency >= 0.0 && latency <= static_cast<double>(maxLatencyFrames(48000.0));
    p.latency = latencyOk ? static_cast<int>(latency) : 0;
    p.editor = j.value("editor", false);
    p.effect = j.value("effect", true);
    p.problem = j.value("problem", "");
    if (p.problem.empty() && !latencyOk) {
        p.problem = "it reports a latency of more than " + std::to_string(kMaxLatencySeconds) + " s";
    }
    if (const auto it = j.find("params"); it != j.end() && it->is_array()) {
        for (const json& q : *it) {
            PluginParam param;
            param.id = q.value("id", 0u);
            param.name = q.value("name", "");
            param.module = q.value("module", "");
            param.unit = q.value("unit", "");
            param.minValue = q.value("min", 0.0);
            param.maxValue = q.value("max", 1.0);
            param.defaultValue = q.value("default", 0.0);
            param.automatable = q.value("automatable", true);
            param.stepped = q.value("stepped", false);
            param.hidden = q.value("hidden", false);
            p.params.push_back(std::move(param));
        }
    }
    return p;
}

/// The plugins' descriptors for the model: kept forever once handed out (EffectDescriptorProvider's contract).
class Descriptors final : public EffectDescriptorProvider {
public:
    const EffectDescriptor* find(std::string_view id) const override {
        std::lock_guard lock(m_mutex);
        const auto it = m_byId.find(id);
        return it != m_byId.end() ? it->second : nullptr;
    }

    void update(const std::vector<PluginInfo>& plugins) {
        std::lock_guard lock(m_mutex);
        std::map<std::string, const EffectDescriptor*, std::less<>> next;
        for (const PluginInfo& p : plugins) {
            if (!p.effect || !p.problem.empty()) {
                continue;
            }
            const std::string id = p.effectId();
            const std::string signature = signatureOf(p);
            const auto old = m_owned.find(id);
            if (old != m_owned.end() && old->second->signature == signature) {
                next.emplace(id, &old->second->descriptor);
                continue;
            }
            Owned& o = m_storage.emplace_back();
            o.signature = signature;
            o.id = id;
            o.name = p.name;
            o.category = "Audio Plugins/" + (p.vendor.empty() ? std::string("Other") : p.vendor);
            o.description = p.vendor + " " + p.name + (p.version.empty() ? "" : " " + p.version) + " (" +
                            (p.format == "clap" ? "CLAP" : "VST3") + " plugin)";
            for (const PluginParam& q : p.params) {
                if (q.hidden) {
                    continue;
                }
                o.keys.push_back(q.key());
                o.names.push_back(q.module.empty() ? q.name : q.module + "/" + q.name);
                o.ranges.push_back(q);
            }
            for (std::size_t i = 0; i < o.keys.size(); ++i) {
                const PluginParam& q = o.ranges[i];
                o.params.push_back(EffectParamInfo{o.keys[i], o.names[i], ParamUnit::Amount, q.defaultValue,
                                                   std::min(q.minValue, q.maxValue), std::max(q.minValue, q.maxValue),
                                                   q.automatable});
            }
            o.descriptor.id = o.id;
            o.descriptor.version = 1;
            o.descriptor.displayName = o.name;
            o.descriptor.category = o.category;
            o.descriptor.kind = EffectKind::Audio;
            o.descriptor.params = o.params;
            o.descriptor.capabilities = p.latency > 0 ? EffectCapability::AudioStateful | EffectCapability::AudioLookahead
                                                      : EffectCapabilities(EffectCapability::AudioStateful);
            o.descriptor.description = o.description;
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
        std::vector<std::string> keys;
        std::vector<std::string> names;
        std::vector<PluginParam> ranges;
        std::vector<EffectParamInfo> params;
        EffectDescriptor descriptor;
    };

    static std::string signatureOf(const PluginInfo& p) { return toJson(p).dump(-1, ' ', false, json::error_handler_t::replace); }

    mutable CheckedMutex m_mutex;
    std::deque<Owned> m_storage; ///< never shrinks: descriptors stay valid
    std::map<std::string, Owned*, std::less<>> m_owned;
    std::map<std::string, const EffectDescriptor*, std::less<>> m_byId;
};

struct FileEntry {
    Fingerprint fingerprint;
    std::vector<PluginInfo> plugins;
    std::string error;
};

struct BlockedEntry {
    Fingerprint fingerprint;
    std::string reason;
};

/// Scans one plugin file in its own helper process. Sets `blocked` when the helper crashed or hung on it, `cancelled`
/// when `stop` was requested first (the helper is stopped at once; the file is not blamed).
std::expected<std::vector<PluginInfo>, std::string> scanFile(const std::filesystem::path& helper, const std::filesystem::path& path,
                                                             std::chrono::milliseconds timeout, const std::stop_token& stop,
                                                             bool& blocked, bool& cancelled) {
    blocked = false;
    cancelled = false;
    std::string error;
    auto child = ChildProcess::start(helper, {"--scan", narrow(path.wstring())}, {}, error);
    if (!child) {
        return std::unexpected("cannot start the plugin helper: " + error);
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
    // Waits in short slices so a stop (quitting the app, cancelling the scan) never waits for a hung plugin.
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
        if (!result.is_object() || !result.value("ok", false)) {
            return std::unexpected(result.is_object() ? result.value("error", std::string("not a plugin")) : std::string("no answer"));
        }
        std::vector<PluginInfo> out;
        const json plugins = result.value("plugins", json::array());
        if (!plugins.is_array()) {
            return std::unexpected(std::string("the scan's answer lists no plugins"));
        }
        for (const json& j : plugins) {
            if (!j.is_object()) {
                return std::unexpected(std::string("the scan's answer is malformed"));
            }
            out.push_back(pluginFromJson(j, path));
        }
        return out;
    } catch (const json::exception& e) {
        return std::unexpected(std::string("the scan's answer is malformed (") + e.what() + ")");
    }
}

/// Plugin files under `folder`: .clap files and .vst3 files or bundles (not descending into bundles).
void collect(const std::filesystem::path& folder, std::vector<std::filesystem::path>& out) {
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec)) {
        return;
    }
    std::filesystem::recursive_directory_iterator it(folder, std::filesystem::directory_options::skip_permission_denied, ec);
    for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        const std::filesystem::path& p = it->path();
        std::wstring ext = p.extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        const bool directory = it->is_directory(ec);
        if (ext == L".vst3") {
            out.push_back(p);
            if (directory) {
                it.disable_recursion_pending();
            }
        } else if (ext == L".clap" && !directory) {
            out.push_back(p);
        } else if (directory && it.depth() >= 8) {
            it.disable_recursion_pending();
        }
    }
}

} // namespace

struct PluginCatalog::Impl {
    CatalogOptions options;
    mutable CheckedMutex mutex;
    std::map<std::string, FileEntry> files;
    std::map<std::string, BlockedEntry> blocked;
    std::vector<std::filesystem::path> userFolders;
    std::shared_ptr<Descriptors> descriptors = std::make_shared<Descriptors>();
    std::atomic<bool> scanning{false};

    std::vector<PluginInfo> allPlugins() const {
        std::vector<PluginInfo> out;
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
            for (const PluginInfo& p : entry.plugins) {
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
            folders.push_back(narrow(folder.wstring()));
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

    std::vector<std::filesystem::path> folders() const {
        std::vector<std::filesystem::path> out = options.folders.empty() ? standardFolders() : options.folders;
        out.insert(out.end(), userFolders.begin(), userFolders.end());
        return out;
    }
};

PluginCatalog::PluginCatalog(CatalogOptions options) : m_impl(std::make_shared<Impl>()) {
    if (options.helper.empty()) {
        options.helper = defaultHelper();
    }
    m_impl->options = std::move(options);
}

PluginCatalog::~PluginCatalog() {
    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_worker.join();
    }
}

std::vector<std::filesystem::path> PluginCatalog::standardFolders() {
    std::vector<std::filesystem::path> out;
    const std::filesystem::path common = envPath(L"COMMONPROGRAMFILES");
    const std::filesystem::path local = envPath(L"LOCALAPPDATA");
    for (const auto& folder : {common / "CLAP", local / "Programs" / "Common" / "CLAP", common / "VST3",
                               local / "Programs" / "Common" / "VST3"}) {
        std::error_code ec;
        if (!folder.empty() && std::filesystem::is_directory(folder, ec)) {
            out.push_back(folder);
        }
    }
    return out;
}

std::filesystem::path PluginCatalog::defaultCacheFile() {
    const std::filesystem::path local = envPath(L"LOCALAPPDATA");
    return local.empty() ? std::filesystem::path() : local / "Wabcut" / "plugins.json";
}

std::filesystem::path PluginCatalog::defaultHelper() {
    wchar_t buffer[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return std::filesystem::path(std::wstring(buffer, n)).parent_path() / "wabcut-plughost.exe";
}

bool PluginCatalog::load() {
    Impl& d = *m_impl;
    if (d.options.cacheFile.empty()) {
        return false;
    }
    std::ifstream in(d.options.cacheFile, std::ios::binary);
    if (!in) {
        return false;
    }
    std::stringstream text;
    text << in.rdbuf();
    const json j = json::parse(text.str(), nullptr, false);
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
                f.plugins.push_back(pluginFromJson(p, std::filesystem::path(widen(path))));
            }
            files[path] = std::move(f);
        }
        const json blockedEntries = j.value("blocked", json::object());
        for (const auto& [path, entry] : blockedEntries.items()) {
            blocked[path] = BlockedEntry{{entry.value("size", std::uintmax_t{0}), entry.value("modified", std::int64_t{0})},
                                         entry.value("reason", "")};
        }
        for (const json& folder : j.value("userFolders", json::array())) {
            userFolders.emplace_back(widen(folder.get<std::string>()));
        }
    } catch (const json::exception&) {
        return false;
    }
    std::lock_guard lock(d.mutex);
    d.files = std::move(files);
    d.blocked = std::move(blocked);
    d.userFolders = std::move(userFolders);
    d.publish();
    return true;
}

ScanReport PluginCatalog::scan(std::stop_token stop, const std::function<void(double, std::string_view)>& progress, bool rescan) {
    Impl& d = *m_impl;
    d.scanning.store(true);
    ScanReport report;
    std::vector<std::filesystem::path> candidates;
    std::set<std::string> knownBefore;
    {
        std::lock_guard lock(d.mutex);
        for (const auto& folder : d.folders()) {
            collect(folder, candidates);
        }
        for (const PluginInfo& p : d.allPlugins()) {
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
            report.blocked.push_back(BlockedPlugin{path, result.error()});
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
    {
        std::lock_guard lock(d.mutex);
        if (!report.cancelled) {
            d.files = std::move(found); // files that went away are forgotten
        } else {
            for (auto& [key, entry] : found) {
                d.files[key] = std::move(entry);
            }
        }
        for (const PluginInfo& p : d.allPlugins()) {
            ++report.plugins;
            report.added += knownBefore.contains(p.effectId()) ? 0 : 1;
        }
        d.publish();
        d.save();
    }
    if (progress) {
        progress(1.0, {});
    }
    d.scanning.store(false);
    return report;
}

void PluginCatalog::startScan(std::function<void(const ScanReport&)> done, bool rescan) {
    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_worker.join();
    }
    m_impl->scanning.store(true);
    m_worker = std::jthread([this, done = std::move(done), rescan](std::stop_token stop) {
        // A thread body: nothing may escape it (std::terminate would end the editor).
        ScanReport report;
        try {
            report = scan(stop, {}, rescan);
        } catch (const std::exception& e) {
            report.errors.push_back(std::string("the plugin scan failed: ") + e.what());
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

bool PluginCatalog::scanning() const { return m_impl->scanning.load(); }

std::vector<PluginInfo> PluginCatalog::plugins() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->allPlugins();
}

std::optional<PluginInfo> PluginCatalog::find(std::string_view effectId) const {
    std::lock_guard lock(m_impl->mutex);
    for (const auto& [path, entry] : m_impl->files) {
        for (const PluginInfo& p : entry.plugins) {
            if (p.effectId() == effectId) {
                return p;
            }
        }
    }
    return std::nullopt;
}

std::vector<BlockedPlugin> PluginCatalog::blocked() const {
    std::lock_guard lock(m_impl->mutex);
    std::vector<BlockedPlugin> out;
    for (const auto& [path, entry] : m_impl->blocked) {
        out.push_back(BlockedPlugin{std::filesystem::path(widen(path)), entry.reason});
    }
    return out;
}

void PluginCatalog::unblock(const std::filesystem::path& path) {
    std::lock_guard lock(m_impl->mutex);
    m_impl->blocked.erase(pathKey(path));
    m_impl->save();
}

void PluginCatalog::clearBlocklist() {
    std::lock_guard lock(m_impl->mutex);
    m_impl->blocked.clear();
    m_impl->save();
}

std::vector<std::filesystem::path> PluginCatalog::userFolders() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->userFolders;
}

void PluginCatalog::addUserFolder(const std::filesystem::path& folder) {
    std::lock_guard lock(m_impl->mutex);
    const std::filesystem::path normal = folder.lexically_normal();
    if (std::find(m_impl->userFolders.begin(), m_impl->userFolders.end(), normal) == m_impl->userFolders.end()) {
        m_impl->userFolders.push_back(normal);
        m_impl->save();
    }
}

void PluginCatalog::removeUserFolder(const std::filesystem::path& folder) {
    std::lock_guard lock(m_impl->mutex);
    std::erase(m_impl->userFolders, folder.lexically_normal());
    m_impl->save();
}

std::shared_ptr<const EffectDescriptorProvider> PluginCatalog::descriptors() const { return m_impl->descriptors; }

const CatalogOptions& PluginCatalog::options() const { return m_impl->options; }

} // namespace wabcut::plughost
