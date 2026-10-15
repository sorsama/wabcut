#include <wabcut/plughost/PluginSystem.h>

#include <wabcut/plughost/Ipc.h>

#include <cstdlib>
#include <mutex>

namespace wabcut::plughost {

namespace {

class CatalogLibrary final : public audio::edit::PluginLibrary {
public:
    explicit CatalogLibrary(std::shared_ptr<PluginCatalog> catalog) : m_catalog(std::move(catalog)) {}

    std::vector<audio::edit::PluginListing> plugins() const override {
        std::vector<audio::edit::PluginListing> out;
        for (const PluginInfo& p : m_catalog->plugins()) {
            audio::edit::PluginListing l;
            l.effect = p.effectId();
            l.format = p.format;
            l.name = p.name;
            l.vendor = p.vendor;
            l.version = p.version;
            l.category = p.category;
            l.path = narrow(p.path.wstring());
            l.latency = p.latency;
            l.editor = p.editor;
            l.effectPlugin = p.effect;
            l.problem = p.problem;
            for (const PluginParam& q : p.params) {
                if (!q.hidden) {
                    l.params.push_back({q.key(), q.module.empty() ? q.name : q.module + "/" + q.name, q.minValue, q.maxValue,
                                        q.defaultValue, q.automatable});
                }
            }
            out.push_back(std::move(l));
        }
        return out;
    }

    std::vector<audio::edit::BlockedPluginListing> blocked() const override {
        std::vector<audio::edit::BlockedPluginListing> out;
        for (const BlockedPlugin& b : m_catalog->blocked()) {
            out.push_back({narrow(b.path.wstring()), b.reason});
        }
        return out;
    }

    std::vector<std::string> folders() const override {
        std::vector<std::string> out;
        const auto& fixed = m_catalog->options().folders;
        for (const auto& f : fixed.empty() ? PluginCatalog::standardFolders() : fixed) {
            out.push_back(narrow(f.wstring()));
        }
        for (const auto& f : m_catalog->userFolders()) {
            out.push_back(narrow(f.wstring()));
        }
        return out;
    }

    std::expected<audio::edit::PluginScanSummary, Error> scan(const audio::edit::PluginScanOptions& options, std::stop_token stop,
                                                             const std::function<void(double)>& progress) override {
        for (const auto& f : options.addFolders) {
            m_catalog->addUserFolder(std::filesystem::path(widen(f)));
        }
        for (const auto& f : options.removeFolders) {
            m_catalog->removeUserFolder(std::filesystem::path(widen(f)));
        }
        if (options.clearBlocklist) {
            m_catalog->clearBlocklist();
        }
        for (const auto& f : options.unblock) {
            m_catalog->unblock(std::filesystem::path(widen(f)));
        }
        const ScanReport report = m_catalog->scan(
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
        for (const BlockedPlugin& b : report.blocked) {
            s.blocked.push_back({narrow(b.path.wstring()), b.reason});
        }
        return s;
    }

private:
    std::shared_ptr<PluginCatalog> m_catalog;
};

std::mutex gMutex;
PluginSystem* gCurrent = nullptr;

} // namespace

std::shared_ptr<audio::edit::PluginLibrary> makePluginLibrary(std::shared_ptr<PluginCatalog> catalog) {
    return std::make_shared<CatalogLibrary>(std::move(catalog));
}

PluginSystem::PluginSystem(PluginSystemOptions options) {
    CatalogOptions catalog;
    catalog.cacheFile = options.cacheFile;
    if (catalog.cacheFile.empty()) {
        // WABCUT_PLUGIN_CACHE points tests and scripts at a catalog of their own.
        const wchar_t* fromEnv = _wgetenv(L"WABCUT_PLUGIN_CACHE");
        catalog.cacheFile = fromEnv != nullptr && *fromEnv != 0 ? std::filesystem::path(fromEnv) : PluginCatalog::defaultCacheFile();
    }
    catalog.helper = options.helper;
    m_catalog = std::make_shared<PluginCatalog>(catalog);
    m_catalog->load();
    if (options.host.helper.empty()) {
        options.host.helper = options.helper;
    }
    m_host = PluginHost::create(options.host);
    m_effects = PluginEffects::create(m_host, m_catalog);
    m_services = std::make_shared<audio::edit::PluginServices>(makePluginLibrary(m_catalog));
    installPluginEffects(m_effects);
    if (options.scanInBackground) {
        m_catalog->startScan();
    }
}

PluginSystem& PluginSystem::start(PluginSystemOptions options) {
    std::lock_guard lock(gMutex);
    if (gCurrent == nullptr) {
        gCurrent = new PluginSystem(std::move(options)); // lives as long as the process (the engine holds its factory)
    }
    return *gCurrent;
}

PluginSystem* PluginSystem::current() {
    std::lock_guard lock(gMutex);
    return gCurrent;
}

} // namespace wabcut::plughost
