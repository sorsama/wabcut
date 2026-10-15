#pragma once

// ShortcutsBridge: the Keyboard Shortcuts editor (M5f, Edit > Keyboard Shortcuts, Ctrl+Alt+K / ⌥⌘K), the QML
// singleton `Shortcuts`. Premiere's editor, over the keymap engine (docs/keymap.md, "Keyboard Shortcuts editor").
//
//   * A US keyboard (`keyboard`: every key with its position in key units) coloured by what each key does with the
//     modifiers held (keyAssignments): application commands, panel commands, or both.
//   * The command list (`commands`, sections by menu group) with search (names, Premiere names, ids and shortcut
//     text) and a scope filter; commandsForKey lists what a key does in every scope.
//   * Editing: a chord typed or a command dropped on a key. checkAssign reports first what changes: commands in the
//     same scope that lose the chord (a conflict to reassign or cancel) and application/panel shadowing; assign,
//     unassign, resetCommand, resetAll.
//   * Key sets: "Premiere Pro (default)" (the built-in table), "Wabcut" (a few changes, kWabcutPreset) and the
//     user's own sets (saved next to the overrides file, under sets/); `modified` when the keys differ from the set.
//   * Import a Premiere .kys (with the commands Wabcut has no equivalent for) or an overrides .json; export the
//     user's overrides as .json.
//
// Every edit is made on a copy of the dispatcher's KeymapManager with the engine API and committed at once with
// KeymapDispatcher::setOverrides, so the keys that are dispatched change immediately and the overrides file is
// saved. Key codes and modifiers cross into QML as Qt's int values (principle 9: no 64-bit values).

#include <wabcut/keymap/KeymapManager.h>

#include <QAbstractListModel>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class KeymapDispatcher;

/// The editor's command list: { commandId, name, premiereName, group, scope, scopeName, application, shortcuts
/// (display text), chords (storage text), overridden, available }.
class ShortcutCommandModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by Shortcuts.commands")
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged FINAL)

public:
    enum Role {
        CommandIdRole = Qt::UserRole + 1,
        NameRole,
        PremiereNameRole,
        GroupRole,
        ScopeRole,
        ScopeNameRole,
        ApplicationRole,
        ShortcutsRole,
        ChordsRole,
        OverriddenRole,
        AvailableRole,
    };

    using QAbstractListModel::QAbstractListModel;

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE int indexOf(const QString& commandId) const;

    void reset(std::vector<QVariantMap> rows);

signals:
    void countChanged();

private:
    std::vector<QVariantMap> m_rows;
};

class ShortcutsBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(wabcut::app::ShortcutCommandModel* commands READ commands CONSTANT FINAL)
    Q_PROPERTY(QString search READ search WRITE setSearch NOTIFY filterChanged FINAL)
    /// "" (every command), "application", "panels", or a scope id ("timeline", ...).
    Q_PROPERTY(QString scopeFilter READ scopeFilter WRITE setScopeFilter NOTIFY filterChanged FINAL)
    /// The scope filter's choices: [{ id, name, count }].
    Q_PROPERTY(QVariantList scopes READ scopes NOTIFY changed FINAL)
    /// [{ id ("16777264", "53k" for a keypad key), key, keypad, label, x, y, w, h (key units), modifier (the Qt
    /// modifier bit a modifier key toggles, else 0) }]
    Q_PROPERTY(QVariantList keyboard READ keyboard NOTIFY changed FINAL)
    /// The modifier toggles for this platform: [{ bit, label }] (Ctrl Shift Alt, or ⌘ ⌥ ⇧ ⌃).
    Q_PROPERTY(QVariantList modifiers READ modifiers NOTIFY changed FINAL)
    Q_PROPERTY(bool macModifiers READ macModifiers NOTIFY changed FINAL)
    /// Bumps on every change of the keys (and the key set): bindings that call keyAssignments() read it.
    Q_PROPERTY(int revision READ revision NOTIFY changed FINAL)
    /// [{ id, name, builtin }]: "premiere", "wabcut", then "user:<name>" sets.
    Q_PROPERTY(QVariantList presets READ presets NOTIFY changed FINAL)
    Q_PROPERTY(QString preset READ preset NOTIFY changed FINAL)
    Q_PROPERTY(QString presetName READ presetName NOTIFY changed FINAL)
    /// The keys differ from the chosen key set.
    Q_PROPERTY(bool modified READ modified NOTIFY changed FINAL)

public:
    explicit ShortcutsBridge(QObject* parent = nullptr);

    static ShortcutsBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setDispatcher(KeymapDispatcher* dispatcher);

    ShortcutCommandModel* commands() { return &m_commands; }
    QString search() const { return m_search; }
    void setSearch(const QString& search);
    QString scopeFilter() const { return m_scopeFilter; }
    void setScopeFilter(const QString& scope);
    QVariantList scopes() const;
    QVariantList keyboard() const;
    QVariantList modifiers() const;
    bool macModifiers() const;
    int revision() const { return m_revision; }
    QVariantList presets() const;
    QString preset() const { return m_preset; }
    QString presetName() const;
    bool modified() const;

    /// What each key does with exactly `modifiers` held (Qt modifier bits, without the keypad bit): { keyId: { kind:
    /// "application" | "panel" | "both", names: [command names] } } for the keys that do something.
    Q_INVOKABLE QVariantMap keyAssignments(int modifiers) const;
    /// What `key` with `modifiers` (the keypad bit for keypad keys) does, in every scope: [{ commandId, name, scope,
    /// scopeName, application }].
    Q_INVOKABLE QVariantList commandsForKey(int key, int modifiers) const;
    /// A key event as a chord in the platform's storage text ("Ctrl+Shift+K", "Shift+Cmd+K"); "" for a modifier
    /// on its own or a key the keymap cannot hold.
    Q_INVOKABLE QString chordText(int key, int modifiers) const;
    /// The display text of a chord ("⇧⌘K" on macOS).
    Q_INVOKABLE QString displayChord(const QString& chordText) const;
    /// A command's name (whatever the list's filter).
    Q_INVOKABLE QString commandName(const QString& commandId) const;

    /// What assigning `chordText` to `commandId` would do: { ok, error, chord (display), chordText, already,
    /// conflicts: [{ commandId, name, scopeName }] (same scope: they lose the chord), shadows: [{ commandId, name,
    /// scopeName, message }] (an application command and a panel command on the same chord: the panel's wins
    /// while it has focus) }.
    Q_INVOKABLE QVariantMap checkAssign(const QString& commandId, const QString& chordText) const;
    /// Assigns the chord, taking it from the commands in the same scope. { ok, error, lost: [names] }
    Q_INVOKABLE QVariantMap assign(const QString& commandId, const QString& chordText);
    /// Removes one chord from a command. Returns "" or the error.
    Q_INVOKABLE QString unassign(const QString& commandId, const QString& chordText);
    Q_INVOKABLE bool resetCommand(const QString& commandId);
    /// Back to Premiere Pro's default keys (the key set too).
    Q_INVOKABLE void resetAll();

    /// Switches key set; the keys become the set's. Returns "" or the error.
    Q_INVOKABLE QString applyPreset(const QString& id);
    /// Saves the current keys as a user key set (replacing one with that name). Returns "" or the error.
    Q_INVOKABLE QString savePreset(const QString& name);
    Q_INVOKABLE QString deletePreset(const QString& id);

    /// Reads a Premiere .kys or an overrides .json without applying it: { ok, error, kind ("kys" | "json"),
    /// fileName, changed (commands that get other keys), unmapped: [Premiere commands Wabcut does not have],
    /// warnings: [...] }.
    Q_INVOKABLE QVariantMap previewKeySet(const QUrl& file) const;
    /// Applies a .kys (merged into the current keys, as Premiere's import does) or an overrides .json (replacing
    /// them). Returns previewKeySet's report.
    Q_INVOKABLE QVariantMap importKeySet(const QUrl& file);
    /// Writes the user's overrides (the changes from Premiere's defaults) as .json. Returns "" or the error.
    Q_INVOKABLE QString exportOverrides(const QUrl& file) const;

signals:
    void changed();
    void filterChanged();

private:
    const keymap::KeymapManager* keymap() const;
    keymap::Platform platform() const;
    std::vector<keymap::CommandOverride> presetOverrides(const QString& id, QString* error) const;
    QString setsFolder() const;
    QString commit(const keymap::KeymapManager& next);
    QVariantMap readKeySet(const QUrl& file, std::vector<keymap::CommandOverride>* overrides) const;
    void rebuild();
    void onKeymapChanged();

    QPointer<KeymapDispatcher> m_dispatcher;
    ShortcutCommandModel m_commands;
    QString m_search;
    QString m_scopeFilter;
    QString m_preset = QStringLiteral("premiere");
    int m_revision = 0;
};

/// QML: the singleton `Shortcuts` (over the engine's KeymapDispatcher).
struct ShortcutsBridgeSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::ShortcutsBridge)
    QML_NAMED_ELEMENT(Shortcuts)
    QML_SINGLETON
public:
    static ShortcutsBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return ShortcutsBridge::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
