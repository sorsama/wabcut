#include <wabcut/script/Manager.h>

#include <wabcut/model/MediaLocator.h>
#include <wabcut/script/Storage.h>

#include <algorithm>
#include <fstream>

namespace wabcut::script {

namespace fs = std::filesystem;

struct ExtensionManager::Entry {
    std::string id;
    ExtensionScope scope = ExtensionScope::User;
    fs::path folder;
    std::expected<ExtensionBundle, Error> bundle = std::unexpected(Error{});
    std::unique_ptr<Relay> relay;
    std::unique_ptr<ExtensionHost> host; // after relay: destroyed first
    std::optional<StopReport> lastStop;
    std::string startError;
};

/// One extension's host delegate: forwards to the manager's delegate with the extension it is about.
class ExtensionManager::Relay : public HostDelegate {
public:
    Relay(ExtensionManager& manager, std::string id) : m_manager(manager), m_id(std::move(id)) {}

    void wake() override { m_manager.m_delegate.wake(); }
    void notify(const Notification& n) override {
        if (const Entry* e = m_manager.find(m_id)) {
            m_manager.m_delegate.notify(m_manager.infoOf(*e), n);
        }
    }
    void prompt(const PromptRequest& p) override {
        if (const Entry* e = m_manager.find(m_id)) {
            m_manager.m_delegate.prompt(m_manager.infoOf(*e), p);
        }
    }
    void promptClosed(std::uint64_t prompt) override { m_manager.m_delegate.promptClosed(m_id, prompt); }
    bool runLongCommand(LongCommand job) override {
        const Entry* e = m_manager.find(m_id);
        return e && m_manager.m_delegate.runLongCommand(m_manager.infoOf(*e), std::move(job));
    }
    void commandsChanged() override { m_manager.m_delegate.changed(); }
    void log(const LogLine& line) override { m_manager.m_delegate.log(m_id, line); }
    void stopped(const StopReport& report) override {
        Entry* e = m_manager.find(m_id);
        if (!e) {
            return;
        }
        e->lastStop = report;
        PermissionStore& store = m_manager.m_store;
        if (report.state == HostState::Crashed || report.state == HostState::Failed) {
            store.recordCrash(m_id, report.reason);
            m_manager.saveStore();
            m_manager.m_delegate.faulted(m_manager.infoOf(*e), report, !store.isEnabled(m_id));
        } else {
            store.recordCleanRun(m_id);
            m_manager.saveStore();
        }
        m_manager.m_delegate.changed();
    }

private:
    ExtensionManager& m_manager;
    std::string m_id;
};

ExtensionPaths extensionPathsUnder(const fs::path& appData) {
    return {appData / "extensions", appData / "extension-permissions.json", appData / "extension-storage", {}};
}

std::string_view toString(ExtensionStatus status) {
    switch (status) {
    case ExtensionStatus::Invalid:
        return "invalid";
    case ExtensionStatus::NeedsReview:
        return "needsReview";
    case ExtensionStatus::NeedsTrust:
        return "needsTrust";
    case ExtensionStatus::Disabled:
        return "disabled";
    case ExtensionStatus::Idle:
        return "idle";
    case ExtensionStatus::Starting:
        return "starting";
    case ExtensionStatus::Running:
        return "running";
    case ExtensionStatus::Failed:
        return "failed";
    }
    return "invalid";
}

ExtensionManager::ExtensionManager(ExtensionPaths paths, std::string wabcutVersion, ManagerDelegate& delegate,
                                   HostLimits limits)
    : m_paths(std::move(paths)), m_wabcutVersion(std::move(wabcutVersion)), m_version(parseVersion(m_wabcutVersion)),
      m_delegate(delegate), m_limits(limits), m_store(PermissionStore::load(m_paths.permissions)) {
    rescan();
}

ExtensionManager::~ExtensionManager() { detach(); }

// ---- Finding extensions --------------------------------------------------------------------------------------------

ExtensionManager::Entry* ExtensionManager::find(std::string_view id) {
    const auto it = m_entries.find(id);
    return it == m_entries.end() ? nullptr : it->second.get();
}

const ExtensionManager::Entry* ExtensionManager::find(std::string_view id) const {
    const auto it = m_entries.find(id);
    return it == m_entries.end() ? nullptr : it->second.get();
}

void ExtensionManager::rescan() {
    std::map<std::string, std::unique_ptr<Entry>, std::less<>> next;
    const auto add = [&](FoundExtension found, ExtensionScope scope) {
        auto entry = std::make_unique<Entry>();
        entry->scope = scope;
        entry->folder = found.folder;
        entry->bundle = std::move(found.bundle);
        std::string key = entry->bundle ? entry->bundle->manifest.id : "invalid:" + pathToUtf8(found.folder.filename());
        if (next.contains(key)) {
            // A project extension never takes the place of an installed one.
            entry->bundle = std::unexpected(Error{ErrorCode::InvalidArgument,
                                                  "an installed extension already uses the id \"" + key + "\""});
            key = "invalid:" + std::string(toString(scope)) + ":" + key;
        }
        entry->id = key;
        next[key] = std::move(entry);
    };
    for (FoundExtension& found : findExtensions(m_paths.userExtensions, ExtensionScope::User, m_version)) {
        add(std::move(found), ExtensionScope::User);
    }
    if (!m_project.empty()) {
        for (FoundExtension& found :
             findExtensions(projectExtensionsRoot(m_project), ExtensionScope::Project, m_version)) {
            add(std::move(found), ExtensionScope::Project);
        }
    }
    // A running extension whose files did not change keeps running.
    for (auto& [key, old] : m_entries) {
        auto it = next.find(key);
        const bool same = it != next.end() && old->bundle && it->second->bundle &&
                          old->bundle->contentHash == it->second->bundle->contentHash &&
                          old->folder == it->second->folder;
        if (same) {
            it->second->relay = std::move(old->relay);
            it->second->host = std::move(old->host);
            it->second->lastStop = old->lastStop;
        } else {
            if (old->host) {
                old->host->stop();
            }
            // The host's threads may still wake its relay until the host is destroyed: they go together.
            m_retired.push_back({std::move(old->relay), std::move(old->host)});
        }
    }
    m_entries = std::move(next);
    for (auto& [key, entry] : m_entries) {
        if (!entry->relay) {
            entry->relay = std::make_unique<Relay>(*this, key);
        }
    }
    startRunnable();
    m_delegate.changed();
}

ExtensionInfo ExtensionManager::infoOf(const Entry& e) const {
    ExtensionInfo info;
    info.id = e.id;
    info.scope = e.scope;
    info.folder = e.folder;
    info.lastStop = e.lastStop;
    if (!e.bundle) {
        info.loadError = e.bundle.error().message;
        return info;
    }
    const Manifest& m = e.bundle->manifest;
    info.manifest = m;
    const ExtensionGrant* grant = m_store.find(m.id);
    info.enabled = grant && grant->enabled;
    info.needsReview = m_store.needsReview(*e.bundle);
    info.disabledReason = grant ? grant->disabledReason : "";
    info.crashes = grant ? grant->crashes : 0;
    info.granted = m_store.effective(*e.bundle);
    if (e.host && e.host->state() == HostState::Ready) {
        info.commands = e.host->commands();
    } else {
        for (const DeclaredCommand& d : m.commands) {
            info.commands.push_back({d.id, d.title});
        }
    }
    if (!info.enabled) {
        info.status = grant && !grant->reviewedVersion.empty() ? ExtensionStatus::Disabled : ExtensionStatus::NeedsReview;
    } else if (e.scope == ExtensionScope::Project && (m_project.empty() || !m_store.isTrusted(m_project, *e.bundle))) {
        info.status = ExtensionStatus::NeedsTrust; // the trust dialog's "Trust and enable" reviews the grants too
    } else if (!m_store.isReviewed(*e.bundle)) {
        info.status = ExtensionStatus::NeedsReview; // not the files the user reviewed (S5-P2-4)
    } else if (e.host && e.host->state() == HostState::Starting) {
        info.status = ExtensionStatus::Starting;
    } else if (e.host && e.host->state() == HostState::Ready) {
        info.status = ExtensionStatus::Running;
    } else if (!e.startError.empty() || (e.lastStop && e.lastStop->state == HostState::Failed)) {
        info.status = ExtensionStatus::Failed;
        info.loadError = !e.startError.empty() ? e.startError : e.lastStop->reason;
    } else {
        info.status = ExtensionStatus::Idle;
    }
    return info;
}

std::vector<ExtensionInfo> ExtensionManager::list() const {
    std::vector<ExtensionInfo> out;
    for (const auto& [key, entry] : m_entries) {
        out.push_back(infoOf(*entry));
    }
    return out;
}

std::optional<ExtensionInfo> ExtensionManager::info(std::string_view id) const {
    const Entry* e = find(id);
    return e ? std::optional(infoOf(*e)) : std::nullopt;
}

ExtensionHost* ExtensionManager::host(std::string_view id) {
    Entry* e = find(id);
    return e ? e->host.get() : nullptr;
}

// ---- Running -------------------------------------------------------------------------------------------------------

bool ExtensionManager::runnable(const Entry& e) const {
    if (!m_session || !e.bundle || !m_store.isEnabled(e.id)) {
        return false;
    }
    if (!m_store.isReviewed(*e.bundle)) {
        return false; // never reviewed, or not the files that were (an update, a reinstall over it: S5-P2-4)
    }
    return e.scope == ExtensionScope::User || (!m_project.empty() && m_store.isTrusted(m_project, *e.bundle));
}

std::expected<void, Error> ExtensionManager::startEntry(Entry& e) {
    if (e.host && (e.host->state() == HostState::Starting || e.host->state() == HostState::Ready)) {
        return {};
    }
    if (!runnable(e)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, e.id + " may not run (review, enable or trust it first)"});
    }
    retire(e);
    e.startError.clear();
    HostSetup setup;
    setup.bundle = *e.bundle;
    setup.granted = m_store.effective(*e.bundle);
    setup.session = m_session;
    setup.registry = m_registry;
    setup.services = m_services;
    setup.helper = m_paths.helper;
    setup.storageFile = m_paths.storage.empty() ? fs::path{} : storageFileFor(m_paths.storage, e.id);
    setup.wabcutVersion = m_wabcutVersion;
    setup.limits = m_limits;
    auto host = ExtensionHost::start(std::move(setup), *e.relay);
    if (!host) {
        e.startError = host.error().message;
        StopReport report{HostState::Failed, "could not start: " + host.error().message, false, false};
        e.lastStop = report;
        m_delegate.faulted(infoOf(e), report, false);
        return std::unexpected(host.error());
    }
    e.host = std::move(*host);
    m_delegate.changed();
    return {};
}

void ExtensionManager::stopEntry(Entry& e) {
    if (e.host) {
        e.host->stop();
        retire(e);
    }
}

void ExtensionManager::retire(Entry& e) {
    if (e.host) {
        // A host may be on the stack (its pump() reported a crash and the delegate restarts it): it is destroyed
        // only once no pump() runs.
        m_retired.push_back({nullptr, std::move(e.host)});
    }
}

void ExtensionManager::flushRetired() {
    if (m_pumpDepth == 0) {
        m_retired.clear();
    }
}

void ExtensionManager::startRunnable() {
    for (auto& [key, entry] : m_entries) {
        if (runnable(*entry) && !entry->host && !entry->lastStop) {
            (void)startEntry(*entry); // a failure is reported through faulted()
        }
    }
}

void ExtensionManager::saveStore() {
    if (m_paths.permissions.empty()) {
        return;
    }
    if (auto saved = m_store.save(); !saved) {
        m_delegate.log("", LogLine{"error", "cannot save the extension permissions: " + saved.error().message});
    }
}

void ExtensionManager::attach(EditSession& session, const command::CommandRegistry& registry,
                              std::shared_ptr<command::CommandServices> services, const fs::path& projectFile) {
    detach();
    m_session = &session;
    m_registry = &registry;
    m_services = std::move(services);
    m_project = projectFile;
    m_listener = session.addListener([this](const ChangeEvent& change) {
        const char* kind = change.kind == ChangeKind::Undo   ? "undo"
                           : change.kind == ChangeKind::Redo ? "redo"
                           : change.kind == ChangeKind::Edit ? "edit"
                                                             : "reset";
        publish(Event{EventKind::CommandApplied,
                      Json{{"label", std::string(change.label)}, {"kind", kind}, {"revision", m_session->revision()}}});
    });
    for (auto& [key, entry] : m_entries) {
        entry->lastStop.reset();
    }
    rescan();
}

void ExtensionManager::detach() {
    for (auto& [key, entry] : m_entries) {
        stopEntry(*entry);
    }
    if (m_session && m_listener != 0) {
        m_session->removeListener(m_listener);
    }
    m_listener = 0;
    m_session = nullptr;
    m_registry = nullptr;
    m_services.reset();
    if (!m_project.empty()) {
        m_project.clear();
        rescan();
    }
    flushRetired();
}

void ExtensionManager::pump() {
    ++m_pumpDepth;
    std::vector<std::string> ids;
    for (const auto& [key, entry] : m_entries) {
        if (entry->host) {
            ids.push_back(key);
        }
    }
    for (const std::string& id : ids) {
        Entry* e = find(id);
        if (!e || !e->host) {
            continue;
        }
        ExtensionHost* host = e->host.get();
        host->pump();
        if (e->host.get() == host && host->state() != HostState::Starting && host->state() != HostState::Ready) {
            retire(*e); // its report is in lastStop
        }
    }
    --m_pumpDepth;
    flushRetired();
}

std::optional<std::chrono::steady_clock::time_point> ExtensionManager::nextDeadline() const {
    std::optional<std::chrono::steady_clock::time_point> next;
    for (const auto& [key, entry] : m_entries) {
        if (entry->host) {
            if (auto t = entry->host->nextDeadline()) {
                next = next ? std::min(*next, *t) : *t;
            }
        }
    }
    return next;
}

void ExtensionManager::publish(const Event& event) {
    for (auto& [key, entry] : m_entries) {
        if (entry->host) {
            entry->host->publish(event);
        }
    }
}

bool ExtensionManager::answerPrompt(std::string_view extensionId, std::uint64_t prompt, std::optional<Json> values) {
    Entry* e = find(extensionId);
    return e && e->host && e->host->answerPrompt(prompt, std::move(values));
}

std::vector<PaletteCommand> ExtensionManager::paletteCommands() const {
    std::vector<PaletteCommand> out;
    for (const auto& [key, entry] : m_entries) {
        if (!runnable(*entry)) {
            continue;
        }
        const ExtensionInfo info = infoOf(*entry);
        for (const RegisteredCommand& c : info.commands) {
            out.push_back({std::string(kPaletteIdPrefix) + entry->id + "/" + c.id, info.manifest->name + ": " + c.title,
                           entry->id, c.id});
        }
    }
    return out;
}

std::expected<void, Error> ExtensionManager::run(std::string_view paletteId, Json args, ExtensionHost::Done done) {
    const std::size_t slash = paletteId.find('/');
    if (!paletteId.starts_with(kPaletteIdPrefix) || slash == std::string_view::npos) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "not an extension command: " + std::string(paletteId)});
    }
    const std::string_view id = paletteId.substr(kPaletteIdPrefix.size(), slash - kPaletteIdPrefix.size());
    Entry* e = find(id);
    if (!e) {
        return std::unexpected(Error{ErrorCode::NotFound, "no extension " + std::string(id)});
    }
    if (!e->host || (e->host->state() != HostState::Starting && e->host->state() != HostState::Ready)) {
        if (auto started = startEntry(*e); !started) {
            return started;
        }
    }
    e->host->invoke(std::string(paletteId.substr(slash + 1)), std::move(args), std::move(done));
    return {};
}

// ---- The permission flow -------------------------------------------------------------------------------------------

std::expected<ExtensionReview, Error> ExtensionManager::review(std::string_view id) const {
    const Entry* e = find(id);
    if (!e) {
        return std::unexpected(Error{ErrorCode::NotFound, "no extension " + std::string(id)});
    }
    if (!e->bundle) {
        return std::unexpected(e->bundle.error());
    }
    return m_store.review(*e->bundle, m_project);
}

std::expected<ExtensionReview, Error> ExtensionManager::reviewFolder(const fs::path& folder) const {
    auto bundle = loadExtension(folder, ExtensionScope::User, m_version);
    if (!bundle) {
        return std::unexpected(bundle.error());
    }
    return m_store.review(*bundle);
}

std::expected<std::string, Error> ExtensionManager::install(const fs::path& folder) {
    auto bundle = loadExtension(folder, ExtensionScope::User, m_version);
    if (!bundle) {
        return std::unexpected(bundle.error());
    }
    const std::string id = bundle->manifest.id;
    const fs::path target = m_paths.userExtensions / pathFromUtf8(id);
    std::error_code ec;
    if (Entry* e = find(id); e && e->scope == ExtensionScope::User) {
        stopEntry(*e);
    }
    if (fs::exists(fs::symlink_status(target, ec))) {
        if (fs::symlink_status(target, ec).type() != fs::file_type::directory) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, pathToUtf8(target) + " is not a plain folder"});
        }
        fs::remove_all(target, ec);
        if (ec) {
            return std::unexpected(Error{ErrorCode::IoError, "cannot replace " + pathToUtf8(target) + ": " + ec.message()});
        }
    }
    const auto write = [&](const std::string& relative, const std::string& bytes) -> std::expected<void, Error> {
        const fs::path file = target / pathFromUtf8(relative);
        fs::create_directories(file.parent_path(), ec);
        std::ofstream out(file, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            return std::unexpected(Error{ErrorCode::IoError, "cannot write " + pathToUtf8(file)});
        }
        return {};
    };
    // Exactly what was checked (not a second read of the source folder, and nothing it does not name).
    if (auto w = write(std::string(kManifestFileName), bundle->manifestText); !w) {
        return std::unexpected(w.error());
    }
    for (const ScriptSource& script : bundle->scripts) {
        if (auto w = write(script.path, script.source); !w) {
            return std::unexpected(w.error());
        }
    }
    rescan();
    return id;
}

std::expected<void, Error> ExtensionManager::uninstall(std::string_view id) {
    Entry* e = find(id);
    if (!e || e->scope != ExtensionScope::User) {
        return std::unexpected(Error{ErrorCode::NotFound, "no installed extension " + std::string(id)});
    }
    stopEntry(*e);
    const fs::path target = m_paths.userExtensions / pathFromUtf8(std::string(id));
    std::error_code ec;
    if (fs::symlink_status(target, ec).type() == fs::file_type::directory) {
        fs::remove_all(target, ec);
    }
    if (ec) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot remove " + pathToUtf8(target) + ": " + ec.message()});
    }
    if (!m_paths.storage.empty()) {
        fs::remove(storageFileFor(m_paths.storage, id), ec);
    }
    m_store.forget(id);
    saveStore();
    rescan();
    return {};
}

std::expected<void, Error> ExtensionManager::enable(std::string_view id, command::CapabilitySet chosen) {
    Entry* e = find(id);
    if (!e || !e->bundle) {
        return std::unexpected(Error{ErrorCode::NotFound, "no loadable extension " + std::string(id)});
    }
    const bool newAuthor = m_store.review(*e->bundle).authorChanged;
    if (auto granted = m_store.grant(*e->bundle, chosen); !granted) {
        return granted;
    }
    if (newAuthor && !m_paths.storage.empty()) {
        // Someone else's extension under this id: it never reads what the earlier one stored.
        std::error_code ec;
        fs::remove(storageFileFor(m_paths.storage, e->id), ec);
    }
    saveStore();
    stopEntry(*e); // new grants take effect in a new process
    e->lastStop.reset();
    e->startError.clear();
    if (runnable(*e)) {
        (void)startEntry(*e);
    }
    m_delegate.changed();
    return {};
}

void ExtensionManager::disable(std::string_view id) {
    m_store.setEnabled(id, false);
    saveStore();
    if (Entry* e = find(id)) {
        stopEntry(*e);
    }
    m_delegate.changed();
}

void ExtensionManager::revoke(std::string_view id) {
    m_store.revoke(id);
    saveStore();
    if (Entry* e = find(id)) {
        stopEntry(*e);
    }
    m_delegate.changed();
}

std::expected<void, Error> ExtensionManager::trust(std::string_view id) {
    Entry* e = find(id);
    if (!e || !e->bundle || e->scope != ExtensionScope::Project || m_project.empty()) {
        return std::unexpected(Error{ErrorCode::NotFound, "no per-project extension " + std::string(id)});
    }
    m_store.trust(m_project, *e->bundle);
    saveStore();
    if (runnable(*e) && !e->host) {
        e->lastStop.reset();
        (void)startEntry(*e);
    }
    m_delegate.changed();
    return {};
}

void ExtensionManager::distrust(std::string_view id) {
    if (!m_project.empty()) {
        m_store.distrust(m_project, id);
        saveStore();
    }
    if (Entry* e = find(id)) {
        stopEntry(*e);
    }
    m_delegate.changed();
}

} // namespace wabcut::script
