#pragma once

// The process's OpenFX support in one piece (0.7, docs/plugins.md), for the app and wabcut-cli: the catalog (from its cache
// file, scanned in the background or on request), the host of the helper processes, and the OpenFX effects installed into the
// model (descriptors) and the playback layer (the processor). One per process: start() once, current() anywhere after.
//
//   auto& ofx = OfxSystem::start({.scanInBackground = true});   // the app at startup
//   ofx.effects()->status(effectInstance);                       // the badge of an effect in Effect Controls

#include <wabcut/ofxhost/OfxCatalog.h>
#include <wabcut/ofxhost/OfxEffects.h>
#include <wabcut/ofxhost/OfxPluginHost.h>

#include <wabcut/audio/edit/PluginLibrary.h>

#include <filesystem>
#include <memory>

namespace wabcut::ofxhost {

struct OfxSystemOptions {
    /// The catalog's cache (default: WABCUT_OFX_CACHE when set, else OfxCatalog::defaultCacheFile()).
    std::filesystem::path cacheFile;
    /// The helper (default: next to this executable).
    std::filesystem::path helper;
    /// The folders scanned instead of the standard ones (tests); empty: the standard folders and the user's.
    std::vector<std::filesystem::path> folders;
    /// Scan new and changed plug-in binaries on a worker right away (the app). Off: only the cache until a scan is asked for.
    bool scanInBackground = false;
    OfxHostOptions host;
};

class OfxSystem {
public:
    /// Creates the process's OpenFX support and installs it (a second call returns the first).
    static OfxSystem& start(OfxSystemOptions options = {});
    /// The started support, or null.
    static OfxSystem* current();

    std::shared_ptr<OfxCatalog> catalog() const { return m_catalog; }
    std::shared_ptr<OfxPluginHost> host() const { return m_host; }
    std::shared_ptr<OfxEffects> effects() const { return m_effects; }

private:
    explicit OfxSystem(OfxSystemOptions options);

    std::shared_ptr<OfxCatalog> m_catalog;
    std::shared_ptr<OfxPluginHost> m_host;
    std::shared_ptr<OfxEffects> m_effects;
};

/// The catalog as a plugin library for plugins.list and plugins.scan (format "openfx"; OfxSystem installs it with
/// audio::edit::setSecondaryPluginLibrary).
std::shared_ptr<audio::edit::PluginLibrary> makeOfxPluginLibrary(std::shared_ptr<OfxCatalog> catalog);

} // namespace wabcut::ofxhost
