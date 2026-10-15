#pragma once

// ExtensionsBridge (M5n, QML singleton `Extensions`): the extensions (C.12, docs/extensions.md and
// docs/internal/c12-extensions-handoff.md) in the UI.
//
//   * One wabcut::script::ExtensionManager for the app, on the GUI thread: the user's extensions (<app data>/extensions)
//     and the open project's (<project folder>/.wabcut/extensions). It attaches to the project's EditSession once the
//     event loop runs and again when another project file opens (its per-project extensions); detaches on close.
//     Commands run with the assistant's services (AgentController::services(), built on first use): the same consent,
//     spend cap and no-cloud rules as the assistant, and no script or sidecar files named by a client.
//   * The manager panel's rows (`extensions`), the permission / trust dialog's data (review, reviewFolder), and the
//     actions: enable with the capabilities chosen, disable, revoke, trust, distrust, install, uninstall, rescan.
//   * Palette commands ("ext:<id>/<command>"): the palette lists them in an "Extensions" group (paletteProvider()),
//     KeymapDispatcher registers them for shortcuts (setExtraCommands), and a key or the palette runs them (run()).
//   * What extensions show: notifications and toasts (notified), forms (`prompts`, answered with answerPrompt), crash
//     badges and toasts (faulted).
//   * Editor events for extensions: SelectionChanged, PlayheadMoved, ProjectSaved (CommandApplied is the manager's).
//
// Tests point it at their own folders and helper (setPaths) before anything runs.
// Principle 9: QML sees ids, keys and display text.

#include <wabcut/model/Ids.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

class QJSEngine;
class QQmlEngine;

namespace wabcut::command {
class CommandRegistry;
class CommandServices;
}
namespace wabcut::script {
class ExtensionManager;
struct ExtensionPaths;
struct LongCommand;
}

namespace wabcut::app {

class BridgeJob;
class KeymapDispatcher;
class PaletteProvider;
class ProjectController;
class ProjectLifecycle;

class ExtensionsBridge : public QObject {
    Q_OBJECT
    /// The extensions, user ones first, each: { id, name, version, author, licence, description, scope ("user" |
    /// "project"), project, folder, status ("invalid" | "needsReview" | "needsTrust" | "disabled" | "idle" |
    /// "starting" | "running" | "failed"), badge (its text), badgeKind ("review" | "trust" | "off" | "on" | "error" |
    /// "broken"), enabled, running, needsReview, granted: [{ capability, title }], commands: [{ id, title, paletteId,
    /// shortcut }], reason (why it is off, failed or broken), crashes, crashed (the last run ended in a crash) }.
    Q_PROPERTY(QVariantList extensions READ extensions NOTIFY extensionsChanged FINAL)
    Q_PROPERTY(int count READ count NOTIFY extensionsChanged FINAL)
    /// The palette commands: [{ paletteId, title, extensionId, commandId, runnable }].
    Q_PROPERTY(QVariantList paletteCommands READ paletteCommands NOTIFY extensionsChanged FINAL)
    /// The forms extensions opened (ui.prompt), oldest first: [{ key ("<extension>#<prompt>"), extensionId,
    /// extensionName, prompt, title, message, submitLabel, cancelLabel, fields: [{ id, type, label, value, ... }] }].
    Q_PROPERTY(QVariantList prompts READ prompts NOTIFY promptsChanged FINAL)
    /// Where installed extensions live (the manager's footer).
    Q_PROPERTY(QString userFolder READ userFolder NOTIFY extensionsChanged FINAL)
    /// The last action's error, or "".
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)
    /// An extension's AI-model command running as a command job (C.14 runLongCommand): { running, extension, text,
    /// progress (0..1, or -1 while unknown), queued (how many wait after it) }. cancelLongCommand() stops it.
    Q_PROPERTY(QVariantMap longCommand READ longCommand NOTIFY longCommandChanged FINAL)

public:
    explicit ExtensionsBridge(QObject* parent = nullptr);
    ~ExtensionsBridge() override;
    static ExtensionsBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    void setController(ProjectController* controller);
    void setLifecycle(ProjectLifecycle* lifecycle);
    void setDispatcher(KeymapDispatcher* dispatcher);
    /// What runs the extensions' commands needs (transcripts, analysis, AI): built on first use.
    void setServicesFactory(std::function<std::shared_ptr<command::CommandServices>()> factory);
    /// Where the extensions, grants and storage live and the helper (tests; the app: <app data>, or WABCUT_DATA_DIR
    /// like wabcut-cli). Replaces the manager (running extensions stop).
    void setPaths(const script::ExtensionPaths& paths);

    QVariantList extensions() const { return m_rows; }
    int count() const { return static_cast<int>(m_rows.size()); }
    QVariantList paletteCommands() const { return m_palette; }
    QVariantList prompts() const { return m_prompts; }
    QString userFolder() const;
    QString message() const { return m_message; }

    /// The permission dialog for an extension: { ok, error, id, name, version, author, licence, description, scope,
    /// project, folder, update, needsTrust, notes: [text], requests: [{ capability, title, explanation, granted }] }.
    Q_INVOKABLE QVariantMap review(const QString& id) const;
    /// The same for a folder before it is installed ("Install from folder...").
    Q_INVOKABLE QVariantMap reviewFolder(const QUrl& folder) const;
    /// Install from folder: copies it into the user folder, then enables it with `capabilities` (names: "Read",
    /// "EditSequence"...). "" or the reason.
    Q_INVOKABLE QString install(const QUrl& folder, const QStringList& capabilities);
    /// The permission dialog's Allow: grants `capabilities` (within the request) and starts it. "" or the reason.
    Q_INVOKABLE QString enable(const QString& id, const QStringList& capabilities);
    /// A project extension's "Trust and enable": enable(), then trust it for this project and this content.
    Q_INVOKABLE QString trustAndEnable(const QString& id, const QStringList& capabilities);
    Q_INVOKABLE QString disable(const QString& id);
    /// Takes every grant back, stops and disables it.
    Q_INVOKABLE QString revoke(const QString& id);
    Q_INVOKABLE QString distrust(const QString& id);
    /// Removes a user extension, its grants and its storage.
    Q_INVOKABLE QString uninstall(const QString& id);
    /// Reads the folders again.
    Q_INVOKABLE void rescan();
    /// The extension's log, oldest first: [{ level, message }].
    Q_INVOKABLE QVariantList log(const QString& id) const;
    /// Runs a palette command ("ext:<id>/<command>"). "" when it started; a failure later is notified().
    Q_INVOKABLE QString run(const QString& paletteId);
    /// A form's answers (field id -> value) or Cancel. False when they do not fit the form (keep it open then).
    Q_INVOKABLE bool answerPrompt(const QString& key, const QVariantMap& values);
    Q_INVOKABLE void cancelPrompt(const QString& key);
    /// Puts text on the clipboard (a read-only result's Copy).
    Q_INVOKABLE void copyText(const QString& text) const;
    /// Opens the manager (Main.qml shows it).
    Q_INVOKABLE void openManager() { emit managerRequested(); }
    QVariantMap longCommand() const { return m_longState; }
    /// Stops the running AI-model command at its next checkpoint: nothing is committed, the extension gets Cancelled.
    Q_INVOKABLE void cancelLongCommand();

    /// C.14: runs an extension's AI-model command (ManagerDelegate::runLongCommand) like CommandJob does - on a
    /// scratch copy of the project in a worker, with the extension's own policy (its grants, its undo label prefix,
    /// no file commands), progress and Cancel - then commits it as one undo step on the GUI thread and calls
    /// job.finish there. One runs at a time; others wait their turn. False when there is no project.
    bool startLongCommand(const QString& extension, script::LongCommand job);
    /// Tests: the registry long commands run against (default: the editing commands the manager attaches with).
    void setLongCommandRegistry(const command::CommandRegistry* registry) { m_longRegistry = registry; }

    /// The palette's "Extensions" group (CommandPalette adds it).
    std::shared_ptr<PaletteProvider> paletteProvider();
    /// Handles what the helpers sent now (the queued pump's; tests call it while they wait).
    void pump();
    /// The manager (tests).
    script::ExtensionManager* manager();

signals:
    void extensionsChanged();
    void promptsChanged();
    void messageChanged();
    /// A notification from an extension: level "info" | "warning" | "error" | "toast"; Main shows it as a toast with
    /// the extension's name as its source.
    void notified(const QString& source, const QString& level, const QString& text);
    /// An extension crashed or failed to start; `disabled`: it was turned off (3 crashes in a row).
    void faulted(const QString& id, const QString& name, const QString& reason, bool disabled);
    /// A line was added to an extension's log (log(id)).
    void logged(const QString& id);
    void managerRequested();
    void longCommandChanged();

private:
    struct PendingLong;
    void runNextLong();
    void setLongState(const QString& extension, const QString& text, double progress);
    class Delegate;
    class Provider;
    class LazyServices;
    void ensureManager();
    void attach();
    void detach();
    void refresh();
    void schedule();
    void setMessage(const QString& message);
    QString result(const QString& error);
    void onDocumentChanged();
    void publishSelection();
    void publishPlayhead();
    void onCommandTriggered(const QString& id);
    /// What the rows show that pump() can change without the manager's changed() (a helper that became ready,
    /// stopped or crashed): the statuses, crash counts and commands.
    std::string statusSignature() const;
    void addPrompt(const QString& extensionId, std::uint64_t prompt, const QVariantMap& form);
    void removePrompt(const QString& key);

    std::unique_ptr<Delegate> m_delegate;
    std::unique_ptr<script::ExtensionPaths> m_paths;
    std::unique_ptr<script::ExtensionManager> m_manager;
    std::function<std::shared_ptr<command::CommandServices>()> m_servicesFactory;
    std::shared_ptr<command::CommandServices> m_services;
    std::shared_ptr<Provider> m_provider;
    QPointer<ProjectController> m_controller;
    QPointer<ProjectLifecycle> m_lifecycle;
    QPointer<KeymapDispatcher> m_dispatcher;
    bool m_attached = false;
    std::filesystem::path m_attachedFile;
    bool m_wasDirty = false;
    bool m_anyRunning = false;
    std::string m_signature; ///< statusSignature() when the rows were built
    QTimer m_timer; ///< pumps at the manager's next deadline (every 100 ms while something runs)
    std::shared_ptr<std::atomic<bool>> m_pumpQueued = std::make_shared<std::atomic<bool>>(false);

    std::deque<std::unique_ptr<PendingLong>> m_longQueue;
    std::unique_ptr<BridgeJob> m_longJob;
    const command::CommandRegistry* m_longRegistry = nullptr;
    QVariantMap m_longState;

    QVariantList m_rows;
    QVariantList m_palette;
    QVariantList m_prompts;
    QString m_message;
};

/// QML registration through the factory (a default-constructible class registered as its own singleton would be built
/// with its default constructor, and create() would never run).
struct ExtensionsSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::ExtensionsBridge)
    QML_NAMED_ELEMENT(Extensions)
    QML_SINGLETON
public:
    static ExtensionsBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return ExtensionsBridge::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
