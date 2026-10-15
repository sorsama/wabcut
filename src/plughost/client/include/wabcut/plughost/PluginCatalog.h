#pragma once

// The audio plugins this computer has (A.6, docs/plugins.md): scanned by the wabcut-plughost helper, one helper per
// plugin file so a plugin that crashes or hangs while scanned only goes on the blocklist; cached between runs; and
// offered to the model as effect descriptors ("plugin.<format>.<id>", Effects.h's EffectDescriptorProvider) whose
// parameters are the plugin's automatable parameters ("p<id>", keyframable like any effect parameter).
//
// Threading: every member may be called from any thread (an internal lock); scan() takes seconds and belongs on a
// worker (startScan() runs one).

#include <wabcut/model/Effects.h>
#include <wabcut/model/Error.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace wabcut::plughost {

struct PluginParam {
    std::uint32_t id = 0;
    std::string name;
    std::string module;
    std::string unit;
    double minValue = 0.0;
    double maxValue = 1.0;
    double defaultValue = 0.0;
    bool automatable = true;
    bool stepped = false;
    bool hidden = false;

    /// The effect parameter key: "p<id>".
    std::string key() const { return "p" + std::to_string(id); }
};

struct PluginInfo {
    std::string format; ///< "clap" or "vst3"
    std::string id;     ///< CLAP plugin id, VST3 class id (32 hex digits)
    std::string name;
    std::string vendor;
    std::string version;
    std::string category;
    std::filesystem::path path; ///< the .clap file or .vst3 file / bundle
    std::vector<PluginParam> params;
    int latency = 0;     ///< frames at 48 kHz, as scanned (instances report their own)
    bool editor = false; ///< has a GUI the helper can open
    bool effect = true;  ///< an audio effect (instruments are listed, not offered)
    std::string problem; ///< why it cannot be used, when it cannot

    std::string effectId() const { return std::string(kPluginEffectPrefix) + format + "." + id; }
    const PluginParam* param(std::string_view key) const;
};

/// The effect id's format and plugin id ("plugin.vst3.ABC..." -> {"vst3", "ABC..."}), or nullopt.
std::optional<std::pair<std::string, std::string>> parsePluginEffectId(std::string_view effectId);

struct BlockedPlugin {
    std::filesystem::path path;
    std::string reason; ///< "crashed while being scanned (exit code ...)", "did not answer within 30 s"
};

struct CatalogOptions {
    /// Where the catalog is kept between runs (JSON). Empty: not kept.
    std::filesystem::path cacheFile;
    /// The helper (default: wabcut-plughost next to this executable).
    std::filesystem::path helper;
    /// The folders scanned; empty: the standard ones (standardFolders()) and the user's.
    std::vector<std::filesystem::path> folders;
    /// How long one plugin file may take to scan.
    std::chrono::milliseconds scanTimeout{30000};
};

struct ScanReport {
    int files = 0;       ///< plugin files found
    int scanned = 0;     ///< scanned now (new or changed)
    int cached = 0;      ///< unchanged since a previous scan
    int skipped = 0;     ///< on the blocklist
    int plugins = 0;     ///< plugins known after the scan
    int added = 0;       ///< plugins new in this scan
    std::vector<BlockedPlugin> blocked; ///< files that went on the blocklist in this scan
    std::vector<std::string> errors;    ///< files that are not plugins, or failed to load (not blocked)
    bool cancelled = false;
};

class PluginCatalog {
public:
    explicit PluginCatalog(CatalogOptions options = {});
    ~PluginCatalog();
    PluginCatalog(const PluginCatalog&) = delete;
    PluginCatalog& operator=(const PluginCatalog&) = delete;

    /// Windows: %COMMONPROGRAMFILES%\CLAP, %LOCALAPPDATA%\Programs\Common\CLAP, %COMMONPROGRAMFILES%\VST3 and
    /// %LOCALAPPDATA%\Programs\Common\VST3 (those that exist).
    static std::vector<std::filesystem::path> standardFolders();
    /// The default cache file: %LOCALAPPDATA%\Wabcut\plugins.json.
    static std::filesystem::path defaultCacheFile();
    static std::filesystem::path defaultHelper();

    /// Reads the cache (plugins, blocklist, user folders). False when there is none or it cannot be read.
    bool load();
    /// Scans the folders: new and changed files through the helper, one at a time, each in its own process.
    /// Unchanged files keep their cached result; blocklisted ones are skipped. `rescan` scans every file again.
    ScanReport scan(std::stop_token stop = {}, const std::function<void(double, std::string_view)>& progress = {},
                    bool rescan = false);
    /// Runs scan() on a worker; `done` is called on it when finished.
    void startScan(std::function<void(const ScanReport&)> done = {}, bool rescan = false);
    bool scanning() const;

    std::vector<PluginInfo> plugins() const;
    std::optional<PluginInfo> find(std::string_view effectId) const;
    std::vector<BlockedPlugin> blocked() const;
    /// Takes a file off the blocklist (it is scanned again next time).
    void unblock(const std::filesystem::path& path);
    void clearBlocklist();

    std::vector<std::filesystem::path> userFolders() const;
    void addUserFolder(const std::filesystem::path& folder);
    void removeUserFolder(const std::filesystem::path& folder);

    /// The model's view of the plugins (Effects.h): install it with setEffectDescriptorProvider().
    std::shared_ptr<const EffectDescriptorProvider> descriptors() const;

    const CatalogOptions& options() const;

private:
    struct Impl;
    std::shared_ptr<Impl> m_impl;
    std::jthread m_worker;
};

} // namespace wabcut::plughost
