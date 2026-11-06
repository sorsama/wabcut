#include <wabcut/ofxhost/OfxSystem.h>

#include "OfxParams.h"

#include <wabcut/plughost/Ipc.h>

#include <wabcut/model/Effects.h>
#include <wabcut/playback/video/ExternalEffects.h>

#include <cstdlib>
#include <mutex>

namespace wabcut::ofxhost {

namespace {

std::mutex gMutex;
OfxSystem* gCurrent = nullptr;

namespace ipc = wabcut::plughost;

/// The catalog for plugins.list and plugins.scan.
class OfxLibrary final : public audio::edit::PluginLibrary {
public:
    explicit OfxLibrary(std::shared_ptr<OfxCatalog> catalog) : m_catalog(std::move(catalog)) {}

    std::vector<audio::edit::PluginListing> plugins() const override {
        std::vector<audio::edit::PluginListing> out;
        for (const OfxPluginInfo& p : m_catalog->plugins()) {
            audio::edit::PluginListing l;
            l.effect = p.effectId();
            l.format = "openfx";
            l.name = p.name;
            l.vendor = p.group;
            l.version = p.version;
            l.category = "OpenFX Plug-ins/" + (p.group.empty() ? std::string("Other") : p.group);
            l.path = ipc::narrow(p.path.wstring());
            l.problem = p.problem;
            for (const OfxParamInfo& q : p.params) {
                for (const DescriptorParam& d : descriptorParams(q)) {
                    l.params.push_back({d.key, d.display, d.minValue, d.maxValue, d.defaultValue, d.animatable});
                }
            }
            out.push_back(std::move(l));
        }
        return out;
    }

    std::vector<audio::edit::BlockedPluginListing> blocked() const override {
        std::vector<audio::edit::BlockedPluginListing> out;
        for (const OfxBlocked& b : m_catalog->blocked()) {
            out.push_back({ipc::narrow(b.path.wstring()), b.reason});
        }
        return out;
    }

    std::vector<std::string> folders() const override {
        std::vector<std::string> out;
        for (const auto& f : m_catalog->folders()) {
            out.push_back(ipc::narrow(f.wstring()));
        }
        return out;
    }

    std::expected<audio::edit::PluginScanSummary, Error> scan(const audio::edit::PluginScanOptions& options, std::stop_token stop,
                                                             const std::function<void(double)>& progress) override {
        // The folders and the blocklist are shared with the audio plugins' command: a folder added there is scanned for OpenFX
        // bundles too, and a file taken off the blocklist is tried again.
        for (const auto& f : options.addFolders) {
            m_catalog->addUserFolder(std::filesystem::path(ipc::widen(f)));
        }
        for (const auto& f : options.removeFolders) {
            m_catalog->removeUserFolder(std::filesystem::path(ipc::widen(f)));
        }
        if (options.clearBlocklist) {
            m_catalog->clearBlocklist();
        }
        for (const auto& f : options.unblock) {
            m_catalog->unblock(std::filesystem::path(ipc::widen(f)));
        }
        const OfxScanReport report = m_catalog->scan(
            stop,
            [&](double fraction, std::string_view) {
                if (progress) {
                    progress(fraction);
                }
            },
            options.rescan);
        audio::edit::PluginScanSummary s;
        s.files = report.files;
        s.scanned = report.scanned;
        s.cached = report.cached;
        s.skipped = report.skipped;
        s.plugins = report.plugins;
        s.added = report.added;
        s.errors = report.errors;
        s.cancelled = report.cancelled;
        for (const OfxBlocked& b : report.blocked) {
            s.blocked.push_back({ipc::narrow(b.path.wstring()), b.reason});
        }
        return s;
    }

private:
    std::shared_ptr<OfxCatalog> m_catalog;
};

} // namespace

std::shared_ptr<audio::edit::PluginLibrary> makeOfxPluginLibrary(std::shared_ptr<OfxCatalog> catalog) {
    return std::make_shared<OfxLibrary>(std::move(catalog));
}

OfxSystem::OfxSystem(OfxSystemOptions options) {
    OfxCatalogOptions catalog;
    catalog.cacheFile = options.cacheFile;
    if (catalog.cacheFile.empty()) {
        // WABCUT_OFX_CACHE points tests and scripts at a catalog of their own.
        const wchar_t* fromEnv = _wgetenv(L"WABCUT_OFX_CACHE");
        catalog.cacheFile = fromEnv != nullptr && *fromEnv != 0 ? std::filesystem::path(fromEnv) : OfxCatalog::defaultCacheFile();
    }
    catalog.helper = options.helper;
    catalog.folders = options.folders;
    m_catalog = std::make_shared<OfxCatalog>(catalog);
    if (options.host.helper.empty()) {
        options.host.helper = options.helper;
    }
    m_host = OfxPluginHost::create(options.host);
    m_effects = OfxEffects::create(m_host, m_catalog);
    // Descriptors first, then the cache: the plug-ins are known to the model before any project can name them.
    setSecondaryEffectDescriptorProvider(m_catalog->descriptors());
    m_catalog->load();
    playback::setExternalEffectProcessor(m_effects);
    audio::edit::setSecondaryPluginLibrary(makeOfxPluginLibrary(m_catalog)); // plugins.list / plugins.scan list and scan them too
    if (options.scanInBackground) {
        m_catalog->startScan();
    }
}

OfxSystem& OfxSystem::start(OfxSystemOptions options) {
    std::lock_guard lock(gMutex);
    if (gCurrent == nullptr) {
        gCurrent = new OfxSystem(std::move(options)); // lives as long as the process (the playback layer holds its processor)
    }
    return *gCurrent;
}

OfxSystem* OfxSystem::current() {
    std::lock_guard lock(gMutex);
    return gCurrent;
}

} // namespace wabcut::ofxhost
