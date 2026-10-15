#pragma once

// The user's extensions and the open project's, and their lifecycle (docs/extensions.md): what the app's extension
// manager panel, permission dialog and command palette are built on (docs/internal/c12-extensions-handoff.md).
//
//   user extensions     <app data>/extensions/<id>/, installed by the user; they run once the user grants them
//   project extensions  <project folder>/.wabcut/extensions/<id>/; untrusted input, so they also need the user's
//                       trust for that project and that exact content (PermissionStore::trust)
//
// An enabled, reviewed (and for a project extension trusted) extension starts when a project is attached and when
// it is enabled. A crashed or stopped one starts again when one of its commands runs, unless it was disabled
// (kMaxCrashes crashes in a row disable it). A project extension never shadows an installed one with the same id.
//
// Threads: like ExtensionHost, everything belongs to the session's thread; ManagerDelegate::wake() comes from any
// thread and asks for pump().

#include <wabcut/script/Host.h>
#include <wabcut/script/Permissions.h>

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::script {

struct ExtensionPaths {
    std::filesystem::path userExtensions; ///< <app data>/extensions
    std::filesystem::path permissions;    ///< <app data>/extension-permissions.json
    std::filesystem::path storage;        ///< <app data>/extension-storage
    std::filesystem::path helper;         ///< wabcut-scripthost; empty = scriptHostPath()
};
/// The usual layout under the app's data folder (QStandardPaths::AppDataLocation in the app and the CLI).
ExtensionPaths extensionPathsUnder(const std::filesystem::path& appData);

enum class ExtensionStatus {
    Invalid,     ///< the folder does not hold a loadable extension (loadError says why)
    NeedsReview, ///< never granted, or a new version asks for more
    NeedsTrust,  ///< a project extension the user has not trusted (for this content)
    Disabled,    ///< switched off (disabledReason when the editor did it)
    Idle,        ///< enabled, not running (no project attached, or stopped / crashed and not restarted yet)
    Starting,
    Running,
    Failed,      ///< the last start failed (lastStop / loadError)
};
std::string_view toString(ExtensionStatus status);

struct ExtensionInfo {
    std::string id; ///< the manifest's id, else the folder's name
    ExtensionScope scope = ExtensionScope::User;
    std::filesystem::path folder;
    std::optional<Manifest> manifest;
    std::string loadError;
    ExtensionStatus status = ExtensionStatus::Invalid;
    command::CapabilitySet granted; ///< effective
    bool enabled = false;
    /// Never reviewed, or this version asks for more than was granted (it runs with the old grants until then).
    bool needsReview = false;
    std::string disabledReason;
    int crashes = 0;
    /// The running extension's registered commands, else the manifest's declared ones.
    std::vector<RegisteredCommand> commands;
    std::optional<StopReport> lastStop;
};

/// A command for the palette and the shortcut editor.
struct PaletteCommand {
    std::string paletteId; ///< "ext:<extension id>/<command id>": stable, what a keymap binds
    std::string title;     ///< "<extension name>: <command title>"
    std::string extensionId;
    std::string commandId;
};
inline constexpr std::string_view kPaletteIdPrefix = "ext:";

class ManagerDelegate {
public:
    virtual ~ManagerDelegate() = default;
    /// Any thread: call ExtensionManager::pump() on the session thread soon.
    virtual void wake() = 0;
    /// The list, a status or the palette commands changed.
    virtual void changed() {}
    virtual void notify(const ExtensionInfo& /*extension*/, const Notification& /*notification*/) {}
    virtual void prompt(const ExtensionInfo& /*extension*/, const PromptRequest& /*prompt*/) {}
    virtual void promptClosed(std::string_view /*extensionId*/, std::uint64_t /*prompt*/) {}
    virtual void log(std::string_view /*extensionId*/, const LogLine& /*line*/) {}
    /// An extension crashed or failed to start (the panel shows a badge; disabled says whether it was switched off).
    virtual void faulted(const ExtensionInfo& /*extension*/, const StopReport& /*report*/, bool /*disabled*/) {}
    /// An extension runs a command that runs an AI model (HostDelegate::runLongCommand): run it as a command job and
    /// return true. False (the default): the extension is told the editor cannot run it here.
    virtual bool runLongCommand(const ExtensionInfo& /*extension*/, LongCommand /*job*/) { return false; }
};

class ExtensionManager {
public:
    ExtensionManager(ExtensionPaths paths, std::string wabcutVersion, ManagerDelegate& delegate, HostLimits limits = {});
    ~ExtensionManager();
    ExtensionManager(const ExtensionManager&) = delete;
    ExtensionManager& operator=(const ExtensionManager&) = delete;

    /// The project the extensions work on: finds its per-project extensions and starts every runnable one.
    void attach(EditSession& session, const command::CommandRegistry& registry,
                std::shared_ptr<command::CommandServices> services, const std::filesystem::path& projectFile);
    /// Stops every extension (the project closes).
    void detach();
    /// Reads the folders again (an extension was added or changed by hand).
    void rescan();

    std::vector<ExtensionInfo> list() const;
    std::optional<ExtensionInfo> info(std::string_view id) const;

    /// What the install / enable dialog shows for a known extension, or for a folder before it is installed.
    std::expected<ExtensionReview, Error> review(std::string_view id) const;
    std::expected<ExtensionReview, Error> reviewFolder(const std::filesystem::path& folder) const;
    /// Copies the extension's manifest and scripts (nothing else) into the user folder, replacing an older copy.
    /// It is not enabled: enable() after the user reviewed it. Returns the id.
    std::expected<std::string, Error> install(const std::filesystem::path& folder);
    /// Removes a user extension, its grants and its storage.
    std::expected<void, Error> uninstall(std::string_view id);
    /// The permission dialog's OK: grants `chosen` (within what it asks for) and starts it.
    std::expected<void, Error> enable(std::string_view id, command::CapabilitySet chosen);
    void disable(std::string_view id);
    /// Takes every grant back, stops and disables it.
    void revoke(std::string_view id);
    /// Trusts a per-project extension (its current content) for the attached project, then starts it if enabled.
    std::expected<void, Error> trust(std::string_view id);
    void distrust(std::string_view id);

    std::vector<PaletteCommand> paletteCommands() const;
    /// Runs a palette command (starting its extension again if it stopped). `done` runs on the session thread.
    std::expected<void, Error> run(std::string_view paletteId, Json args, ExtensionHost::Done done);
    /// An editor event for every running extension that listens to its kind (throttled per extension).
    void publish(const Event& event);
    bool answerPrompt(std::string_view extensionId, std::uint64_t prompt, std::optional<Json> values);
    void pump();
    std::optional<std::chrono::steady_clock::time_point> nextDeadline() const;

    ExtensionHost* host(std::string_view id);
    const PermissionStore& permissions() const { return m_store; }

private:
    struct Entry;
    class Relay;
    Entry* find(std::string_view id);
    const Entry* find(std::string_view id) const;
    ExtensionInfo infoOf(const Entry& entry) const;
    bool runnable(const Entry& entry) const;
    std::expected<void, Error> startEntry(Entry& entry);
    void stopEntry(Entry& entry);
    void retire(Entry& entry);
    void flushRetired();
    void startRunnable();
    void saveStore();

    ExtensionPaths m_paths;
    std::string m_wabcutVersion;
    std::optional<Version> m_version;
    ManagerDelegate& m_delegate;
    HostLimits m_limits;
    PermissionStore m_store;
    std::map<std::string, std::unique_ptr<Entry>, std::less<>> m_entries;
    EditSession* m_session = nullptr;
    const command::CommandRegistry* m_registry = nullptr;
    std::shared_ptr<command::CommandServices> m_services;
    std::filesystem::path m_project;
    std::uint64_t m_listener = 0;
    struct Retired {
        std::unique_ptr<Relay> relay;        ///< set when its entry went away
        std::unique_ptr<ExtensionHost> host; ///< destroyed before the relay it may still wake
    };
    std::vector<Retired> m_retired;
    int m_pumpDepth = 0;
};

} // namespace wabcut::script
