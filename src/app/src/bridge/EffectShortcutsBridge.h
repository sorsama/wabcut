#pragma once

// EffectShortcutsBridge: keyboard shortcuts for individual effects and presets (0.7, card #82; singleton `EffectShortcuts`,
// docs/effects.md "Keyboard shortcuts for effects and presets", docs/keymap.md).
//
// Every effect of the Effects panel (video and audio; not transitions, which have Apply Default Transitions, or audio
// plug-ins) and every preset (Wabcut's and the user's) is a run-time command of the keymap (KeymapDispatcher's
// setExtraCommandsFor, source "effects"): "fx:<effect id>" in the group "Apply Effect" and "preset:<name>" in "Apply
// Preset", Application scope, no default key, so the Keyboard Shortcuts editor lists them (searchable by name) and the
// user binds a key like any other command. A key applies the effect or preset to the selected clips as one undo step
// (EffectControlsModel::applyToSelectedClips: every selected clip of the item's kind, a clip of the other kind standing for
// its linked partner), and the bridge says how it went with `applied` (a toast).
//
// A key bound to an effect that is gone (a deleted preset) stays in the user's overrides and lists as "(unavailable)", as
// an extension's does; it does nothing. The list follows the preset library: saving, importing or deleting a preset
// updates the commands (the effects themselves never change while the app runs).

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class EffectPresetsBridge;
class KeymapDispatcher;
class ProjectController;

class EffectShortcutsBridge : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(EffectShortcuts)
    QML_SINGLETON

    /// How many effects and presets have a command (a key can be bound to).
    Q_PROPERTY(int count READ count NOTIFY commandsChanged FINAL)

public:
    explicit EffectShortcutsBridge(QObject* parent = nullptr);
    static EffectShortcutsBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    void setController(ProjectController* controller);
    void setDispatcher(KeymapDispatcher* dispatcher);
    void setPresets(EffectPresetsBridge* presets);

    int count() const { return m_count; }

    /// The keymap command of an Effects panel item: "fx:<id>" for an effect id (or "effect:<id>"), "preset:<name>" for a
    /// preset; "" for a transition, an audio plug-in or an unknown item.
    Q_INVOKABLE QString commandFor(const QString& itemId) const;
    /// The shortcut text of that command ("Ctrl+Alt+B"), "" when none is bound.
    Q_INVOKABLE QString shortcutOf(const QString& itemId) const;
    /// Applies the effect or preset a command names to the selected clips (the key's action, also for tests and the
    /// palette): { ok, message, clips }; `applied` is emitted with the message.
    Q_INVOKABLE QVariantMap run(const QString& commandId);
    /// Rebuilds the commands from the effects and the preset library (after the library changed).
    Q_INVOKABLE void refresh();

signals:
    void commandsChanged();
    /// A key (or run) applied an effect or preset: the toast's text and whether it worked.
    void applied(const QString& message, bool ok);

private slots:
    void onCommandTriggered(const QString& commandId);

private:
    QPointer<ProjectController> m_controller;
    QPointer<KeymapDispatcher> m_dispatcher;
    QPointer<EffectPresetsBridge> m_presets;
    int m_count = 0;
};

} // namespace wabcut::app
