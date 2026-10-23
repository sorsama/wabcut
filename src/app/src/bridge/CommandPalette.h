#pragma once

// CommandPalette: the searchable list behind the command palette overlay (Ctrl/⌘+Shift+P) — and the shell of the
// future AI command bar.
//
// Rows come from providers (PaletteProvider):
//   * keymap commands (every KeymapDispatcher CommandId: display name, menu group, current shortcut), run through
//     KeymapDispatcher::trigger() so they take the same route as their keys; commands the app cannot run yet, or
//     cannot run now, stay listed but disabled with the reason ("Select a clip first", "Not available yet");
//   * typed edit commands from the command catalog (CommandBridge), with parameter prompts, a dry-run preview and
//     apply through CommandEngine;
//   * "Ask AI…", a pinned row that opens the AI panel (a placeholder until the AI platform's AiController adds a
//     provider of its own: providers may return their own rows for a query and refresh them later);
//   * the shell's own rows (UX rework, stream S): QML hands over rows it knows and C++ does not - "Open panel: Library",
//     "Workspace: Color", "Open Recent: My film" - with setShellActions(); choosing one emits shellActionRequested(id)
//     for Main.qml to run.
// Fuzzy providers are ranked here (fuzzyScore: subsequence matches with bonuses for word starts, runs, prefixes and
// initials); recent commands come first for an empty query and get a boost otherwise.

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <memory>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class CommandBridge;
class KeymapDispatcher;
class ProjectController;

/// One palette row.
struct PaletteItem {
    QString key;       ///< unique: "<provider>:<id>"
    QString title;
    QString group;     ///< menu group or kind, shown dimmed
    QString shortcut;  ///< display text of the current shortcut
    QString detail;    ///< one line under the title (descriptions, the AI query)
    bool enabled = true;
    QString reason;    ///< why a disabled row cannot run now
    QString section;   ///< "commands", "edits" or "ai"
    QVariantList params;    ///< prompts (edit commands)
    QStringList keywords;   ///< more searchable text (ids, Premiere names)
    bool pinned = false;    ///< shown after the ranked rows whatever the query (Ask AI…)
};

class PaletteProvider {
public:
    virtual ~PaletteProvider() = default;
    virtual QString id() const = 0;
    /// Rows for `query`. Fuzzy providers return every row and the palette ranks them; others (AI suggestions)
    /// return their own rows, in order.
    virtual std::vector<PaletteItem> items(const QString& query) const = 0;
    virtual bool fuzzy() const { return true; }
    /// Runs a row with its prompt values; "" on success, else a message to show.
    virtual QString activate(const PaletteItem& item, const QVariantMap& params) = 0;
    /// Dry run of a row with prompts: { ok, summary, error }; {} when the provider has none.
    virtual QVariantMap preview(const PaletteItem& item, const QVariantMap& params);
    /// Set by the palette; a provider whose rows change later (asynchronous suggestions) calls notifyChanged().
    void setChangedCallback(std::function<void()> callback) { m_changed = std::move(callback); }

protected:
    void notifyChanged() const {
        if (m_changed)
            m_changed();
    }

private:
    std::function<void()> m_changed;
};

/// 0 when `pattern` is not a case-insensitive subsequence of `text`; otherwise higher for better matches (word
/// starts, consecutive runs, an early first match).
int fuzzyScore(QStringView pattern, QStringView text);

/// Keymap commands. `trigger` runs a command id (KeymapDispatcher::trigger in the app).
class KeymapCommandProvider final : public PaletteProvider {
public:
    struct Command {
        QString id, title, group, premiereName;
    };
    KeymapCommandProvider(std::vector<Command> commands, std::function<QString(const QString&)> shortcutText,
                          std::function<QVariantMap(const QString&)> availability,
                          std::function<bool(const QString&)> trigger);
    /// The keymap's commands, shortcut texts and trigger from the dispatcher; availability from the controller, then
    /// from `moreAvailability` (the bridges with their own commandAvailability: the Library, Multi-Camera) for the
    /// ids the controller does not handle.
    static std::shared_ptr<KeymapCommandProvider> forApp(KeymapDispatcher* dispatcher, ProjectController* controller,
                                                         std::function<QVariantMap(const QString&)> moreAvailability = {});
    /// Commands the shell runs outside ProjectController: the timeline view, Main.qml's router (its runCommand switch and
    /// the bridges it asks: Editing, Monitors, Library, Multi-Camera, Graphics, Moments, Markers, Trim) - the keys work,
    /// so the palette lists them as ready.
    static bool viewHandles(const QString& id);
    /// Whether keymap command `id` can run now, as its palette row says: { enabled, reason } - the controller's or a
    /// bridge's answer for its own ids, ready for the ids the shell routes (viewHandles), else "Not available yet".
    QVariantMap stateOf(const QString& id) const;
    /// The current shortcut text of `id` ("" without one).
    QString shortcutOf(const QString& id) const { return m_shortcutText ? m_shortcutText(id) : QString(); }

    QString id() const override { return QStringLiteral("command"); }
    std::vector<PaletteItem> items(const QString& query) const override;
    QString activate(const PaletteItem& item, const QVariantMap& params) override;

private:
    std::vector<Command> m_commands;
    std::function<QString(const QString&)> m_shortcutText;
    std::function<QVariantMap(const QString&)> m_availability;
    std::function<bool(const QString&)> m_trigger;
};

/// Typed edit commands from the command catalog.
class EditCommandProvider final : public PaletteProvider {
public:
    explicit EditCommandProvider(CommandBridge* bridge);
    QString id() const override { return QStringLiteral("edit"); }
    std::vector<PaletteItem> items(const QString& query) const override;
    QString activate(const PaletteItem& item, const QVariantMap& params) override;
    QVariantMap preview(const PaletteItem& item, const QVariantMap& params) override;

private:
    QPointer<CommandBridge> m_bridge;
};

/// App actions that are neither keymap nor catalog commands (New Title): fixed rows with a reason when they cannot
/// run and a function that runs them.
class ActionProvider final : public PaletteProvider {
public:
    struct Action {
        QString id, title, group, detail;
        QStringList keywords;
        std::function<QString()> reason; ///< "" when it can run
        std::function<QString()> run;    ///< "" on success, else a message
    };
    explicit ActionProvider(std::vector<Action> actions) : m_actions(std::move(actions)) {}
    QString id() const override { return QStringLiteral("action"); }
    std::vector<PaletteItem> items(const QString& query) const override;
    QString activate(const PaletteItem& item, const QVariantMap& params) override;

private:
    std::vector<Action> m_actions;
};

/// Rows the shell (QML) hands over: panels, workspaces, recent projects - whatever Main.qml can run and C++ cannot list.
/// Each has an id QML chose ("panel:library"); running one calls `run` with that id (the palette emits
/// shellActionRequested).
class ShellActionProvider final : public PaletteProvider {
public:
    explicit ShellActionProvider(std::function<void(const QString&)> run) : m_run(std::move(run)) {}
    /// Replaces the rows: [{ id, title, group, detail, keywords: [string], shortcut, enabled (default true), reason }].
    /// Rows without an id or a title are skipped, and so is a second row with the same id. Returns how many were kept.
    int setActions(const QVariantList& actions);
    QString id() const override { return QStringLiteral("shell"); }
    std::vector<PaletteItem> items(const QString& query) const override;
    QString activate(const PaletteItem& item, const QVariantMap& params) override;

private:
    std::vector<PaletteItem> m_items;
    std::function<void(const QString&)> m_run;
};

/// "Ask AI…": one pinned row; activating it asks the palette to open the AI panel.
class AskAiProvider final : public PaletteProvider {
public:
    explicit AskAiProvider(std::function<void(const QString&)> open) : m_open(std::move(open)) {}
    QString id() const override { return QStringLiteral("ai"); }
    std::vector<PaletteItem> items(const QString& query) const override;
    bool fuzzy() const override { return false; }
    QString activate(const PaletteItem& item, const QVariantMap& params) override;

private:
    std::function<void(const QString&)> m_open;
    mutable QString m_query;
};

class CommandPalette : public QObject {
    Q_OBJECT

    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged FINAL)
    /// Ranked rows: [{ key, title, group, shortcut, detail, enabled, reason, section, params, pinned, recent }].
    Q_PROPERTY(QVariantList results READ results NOTIFY resultsChanged FINAL)
    /// Keys of the recently run rows, newest first.
    Q_PROPERTY(QStringList recent READ recent NOTIFY recentChanged FINAL)

public:
    explicit CommandPalette(QObject* parent = nullptr);
    ~CommandPalette() override;
    /// QML singleton: providers for the app's keymap, command catalog and the AI row.
    static CommandPalette* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    void addProvider(std::shared_ptr<PaletteProvider> provider);

    QString query() const { return m_query; }
    void setQuery(const QString& query);
    QVariantList results() const { return m_results; }
    QStringList recent() const { return m_recent; }

    /// Re-reads every provider (availability changes with the selection; call when the overlay opens).
    Q_INVOKABLE void refresh();
    /// Runs a row; "" on success, else a message to show. Running records it as recent.
    Q_INVOKABLE QString activate(const QString& key, const QVariantMap& params = {});
    /// Dry run of a row with prompts: { ok, summary, error }.
    Q_INVOKABLE QVariantMap preview(const QString& key, const QVariantMap& params);
    /// Prompt values to start from (edit commands).
    Q_INVOKABLE QVariantMap defaults(const QString& key) const;
    /// The shell's rows (ShellActionProvider), replacing the ones set before: [{ id, title, group, detail,
    /// keywords: [string], shortcut, enabled (default true), reason }]. Their palette keys are "shell:<id>"; choosing
    /// one emits shellActionRequested(id). Returns how many rows were kept (rows without id or title are skipped).
    Q_INVOKABLE int setShellActions(const QVariantList& actions);
    /// UX rework (stream S), for menus (the menu bar): the state of keymap command `id` now, as the palette would list
    /// it: { known (a keymap command), title, group, shortcut, enabled, reason ("Nothing to undo", "Select a clip
    /// first", "Not available yet") }. The typed edits' keys (marker.add, marker.clearAll, effects.newCustomBin, ...),
    /// which the palette lists as edits with prompts, report enabled. A call, not a binding: re-read it when the menu
    /// opens (e.g. `enabled: menu.visible && CommandPalette.commandState(id).enabled`).
    Q_INVOKABLE QVariantMap commandState(const QString& id) const;
    /// Recent rows are remembered across runs (QSettings "palette/recent"); tests switch it off.
    void setPersistRecent(bool persist) { m_persistRecent = persist; }

signals:
    void queryChanged();
    void resultsChanged();
    void recentChanged();
    /// "Ask AI…" was chosen with the current query.
    void aiRequested(const QString& query);
    /// A Help row (M5j onboarding): "welcome", "tour", "trainer" or "whatsNew".
    void helpRequested(const QString& what);
    /// A shell row (setShellActions) was chosen: its id, as QML gave it ("panel:library").
    void shellActionRequested(const QString& id);

private:
    struct Entry {
        PaletteItem item;
        PaletteProvider* provider = nullptr;
    };
    void rebuild();
    const Entry* find(const QString& key) const;
    void remember(const QString& key);

    std::vector<std::shared_ptr<PaletteProvider>> m_providers;
    std::shared_ptr<ShellActionProvider> m_shell; // added at the first setShellActions (after the app's providers)
    std::shared_ptr<KeymapCommandProvider> m_keymap; // the keymap's rows (commandState), the first one added
    std::vector<Entry> m_entries; // last collected rows (all providers)
    QString m_query;
    QVariantList m_results;
    QStringList m_recent;
    QPointer<CommandBridge> m_bridge;
    bool m_persistRecent = false;
};

/// QML registration through the factory. Qt prefers a default constructor over create() when both exist, and the
/// class stays default-constructible for C++ and tests, so the singleton is registered by this foreign wrapper.
struct CommandPaletteSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::CommandPalette)
    QML_NAMED_ELEMENT(CommandPalette)
    QML_SINGLETON
public:
    static CommandPalette* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return CommandPalette::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
