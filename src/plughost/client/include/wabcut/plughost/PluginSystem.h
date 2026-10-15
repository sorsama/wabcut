#pragma once

// The process's audio plugin support in one piece (A.6, docs/plugins.md), for the app and wabcut-cli: the catalog
// (from its cache file, scanned in the background or on request), the host of the helper processes, and the plugin
// effects installed into the engine and the model. One per process: start() once, current() anywhere after.
//
//   auto& plugins = PluginSystem::start({.scanInBackground = true});  // the app at startup
//   parts.push_back(plugins.services());                                // plugins.list / plugins.scan
//   plugins.effects()->openEditor(effect);                              // "Edit plugin..." in Effect Controls

#include <wabcut/plughost/PluginCatalog.h>
#include <wabcut/plughost/PluginEffects.h>
#include <wabcut/plughost/PluginHost.h>

#include <wabcut/audio/edit/PluginLibrary.h>

#include <filesystem>
#include <memory>

namespace wabcut::plughost {

struct PluginSystemOptions {
    /// The catalog's cache (default: WABCUT_PLUGIN_CACHE when set, else PluginCatalog::defaultCacheFile()).
    std::filesystem::path cacheFile;
    /// The helper (default: next to this executable).
    std::filesystem::path helper;
    /// Scan new and changed plugin files on a worker right away (the app). Off: only the cache until plugins.scan.
    bool scanInBackground = false;
    HostOptions host;
};

class PluginSystem {
public:
    /// Creates the process's plugin support and installs it (a second call returns the first).
    static PluginSystem& start(PluginSystemOptions options = {});
    /// The started plugin support, or null.
    static PluginSystem* current();

    std::shared_ptr<PluginCatalog> catalog() const { return m_catalog; }
    std::shared_ptr<PluginHost> host() const { return m_host; }
    std::shared_ptr<PluginEffects> effects() const { return m_effects; }
    /// plugins.list and plugins.scan for the command engine (audio::edit::PluginServices).
    std::shared_ptr<audio::edit::PluginServices> services() const { return m_services; }

private:
    explicit PluginSystem(PluginSystemOptions options);

    std::shared_ptr<PluginCatalog> m_catalog;
    std::shared_ptr<PluginHost> m_host;
    std::shared_ptr<PluginEffects> m_effects;
    std::shared_ptr<audio::edit::PluginServices> m_services;
};

/// audio::edit::PluginLibrary over a catalog (what services() wraps).
std::shared_ptr<audio::edit::PluginLibrary> makePluginLibrary(std::shared_ptr<PluginCatalog> catalog);

} // namespace wabcut::plughost
