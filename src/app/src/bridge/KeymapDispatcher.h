#pragma once

// KeymapDispatcher: the Qt bridge over the Qt-free keymap module (docs/keymap.md, "App integration").
//
// - Watches one QQuickWindow (event filter) and turns its key presses AND releases into commands and J/K/L
//   transport intents, resolved against the focused item's nearest KeymapScope, then Application.
// - Owns the KeymapManager (the built-in Premiere table for the running OS plus the user's overrides,
//   persisted under QStandardPaths::AppConfigLocation) and the KeyDispatcher / TransportGesture.
// - Applies the routing rules: text editing, NumberField editing, IME composition and popups/dialogs keep
//   their keys (a focused item claims a key by accepting QEvent::ShortcutOverride); AltGr text is typing, not
//   Ctrl+Alt shortcuts; keypad Enter/Delete act like Return/Delete.
// - Emits commandTriggered(id) and transportIntent(...), and serves shortcutText(id) for tooltips and menus.
//
// GUI thread only. In QML it is the singleton `KeymapDispatcher` of the `Wabcut` module; panels declare their
// scope with the attached property `KeymapScope.scope`.
//
// Run-time commands (C.12 extensions, M5n): setExtraCommands() adds commands such as "ext:<extension>/<command>" to
// the registry the keymap is built on (Application scope, no default key), so they are assignable in the shortcut
// editor and their keys emit commandTriggered(id). An "ext:" id the user bound whose extension is gone stays
// registered as unavailable, so its shortcut is kept, not deleted.

#include <wabcut/keymap/KeyDispatcher.h>
#include <wabcut/keymap/KeymapManager.h>
#include <wabcut/keymap/Platform.h>
#include <wabcut/keymap/TransportGesture.h>

#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <optional>
#include <vector>

class QKeyEvent;

namespace wabcut::app {

/// The attached object behind `KeymapScope.*`.
class KeymapScopeAttached : public QObject {
    Q_OBJECT
    QML_ANONYMOUS
    /// The keymap scope of this item and its descendants: "timeline", "programMonitor", "sourceMonitor",
    /// "project", "effectControls", "effects", "audioMixer", "history", "mediaBrowser", "metadata" or
    /// "application". Empty (the default) inherits the parent's scope.
    Q_PROPERTY(QString scope READ scope WRITE setScope NOTIFY scopeChanged)
    /// True for items that consume keys themselves (custom editors, drag-to-edit fields): no editor shortcut
    /// fires while focus is in this item or its descendants.
    Q_PROPERTY(bool suppressShortcuts READ suppressShortcuts WRITE setSuppressShortcuts NOTIFY
                   suppressShortcutsChanged)

public:
    explicit KeymapScopeAttached(QObject* parent = nullptr);

    QString scope() const { return m_scope; }
    void setScope(const QString& scope);
    bool suppressShortcuts() const { return m_suppress; }
    void setSuppressShortcuts(bool suppress);

signals:
    void scopeChanged();
    void suppressShortcutsChanged();

private:
    QString m_scope;
    bool m_suppress = false;
};

/// QML: `KeymapScope.scope: "timeline"` on a panel's root item.
class KeymapScope : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("KeymapScope is only available as an attached property.")
    QML_ATTACHED(wabcut::app::KeymapScopeAttached)

public:
    static KeymapScopeAttached* qmlAttachedProperties(QObject* object);
};

class KeymapDispatcher : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    /// The window whose keys are dispatched. When unset, the dispatcher attaches to the first top-level
    /// QQuickWindow that becomes the focus window.
    Q_PROPERTY(QQuickWindow* window READ window WRITE setWindow NOTIFY windowChanged)
    /// "windows", "macos" or "linux": the running OS by default. Other values exist for tests and tooling;
    /// changing it reloads the matching table and override file.
    Q_PROPERTY(QString platform READ platform WRITE setPlatform NOTIFY keymapChanged)
    /// The JSON file holding the user's overrides (default: <AppConfigLocation>/keymap/overrides-<os>.json).
    Q_PROPERTY(QString overridesPath READ overridesPath WRITE setOverridesPath NOTIFY keymapChanged)
    /// Increments whenever bindings change; read it in bindings that call shortcutText() so they refresh.
    Q_PROPERTY(int revision READ revision NOTIFY keymapChanged)

public:
    enum class TransportKind {
        ShuttleStep, ///< J/L: speed is the new ladder speed (±1, ±2, ±4, ±8)
        SetSpeed,    ///< K+J / K+L held: play at speed (∓1/4) until Stop
        StepFrames,  ///< K held + tap J/L: step `frames` frames (±1)
        Stop,
    };
    Q_ENUM(TransportKind)

    explicit KeymapDispatcher(QObject* parent = nullptr);
    ~KeymapDispatcher() override;

    QQuickWindow* window() const { return m_window; }
    void setWindow(QQuickWindow* window);
    QString platform() const;
    void setPlatform(const QString& platform);
    QString overridesPath() const { return m_overridesPath; }
    void setOverridesPath(const QString& path);
    int revision() const { return m_revision; }

    /// The primary shortcut of a command for display ("Ctrl+K", "⌘K"), or "" when unbound or unknown.
    Q_INVOKABLE QString shortcutText(const QString& commandId) const;
    /// Every shortcut of a command for display, primary first.
    Q_INVOKABLE QStringList shortcutTexts(const QString& commandId) const;
    /// Runs a command from a menu or button as if its key had been tapped. False for unknown ids.
    Q_INVOKABLE bool trigger(const QString& commandId);
    /// The scope id that applies to `item` (its nearest KeymapScope, else "application").
    Q_INVOKABLE QString scopeAt(QQuickItem* item) const;
    /// The playback controller reports the transport speed it actually runs at, so J/L climb from there.
    Q_INVOKABLE void syncTransportSpeed(int speedNum, int speedDen);
    /// A window of its own for a panel (0.1.3 panel docking, FloatingDock): its keys reach the same shortcuts as the
    /// main window's, resolved against its own focused item. removeWindow() before it goes.
    Q_INVOKABLE void addWindow(QQuickWindow* window);
    Q_INVOKABLE void removeWindow(QQuickWindow* window);

    // Shortcut editing; every change is saved to overridesPath.
    /// Adds a shortcut (chord text in the platform's format, e.g. "Ctrl+Shift+K" or "Shift+Cmd+K") to a
    /// command, taking it from any other command in the same scope. Returns {ok: bool, lost: [ids of the
    /// commands that lost the shortcut], error: string}.
    Q_INVOKABLE QVariantMap assignShortcut(const QString& commandId, const QString& chordText);
    Q_INVOKABLE bool resetShortcut(const QString& commandId);
    Q_INVOKABLE void resetAllShortcuts();
    /// Imports a Premiere Pro key set (.kys) as overrides. Returns "" on success, else the error message.
    Q_INVOKABLE QString importPremiereKeySet(const QUrl& file);

    const keymap::KeymapManager& keymap() const { return *m_keymap; }
    bool isGestureActive() const { return m_dispatcher->isGestureActive(); }

    // ---- Keyboard Shortcuts editor (M5f) ----
    /// Replaces every user override at once (presets, user key sets, the editor's edits made on a copy of
    /// keymap()): applied to a copy of the defaults, then swapped in, saved to overridesPath and announced with
    /// keymapChanged. On an error (unknown command, a chord of the other platform family) nothing changes.
    std::expected<void, Error> setOverrides(std::span<const keymap::CommandOverride> overrides);

    // ---- Run-time commands (C.12 extensions) ----
    struct ExtraCommand {
        QString id;    ///< "ext:<extension id>/<command id>"
        QString title; ///< "<extension name>: <command title>"
        friend bool operator==(const ExtraCommand&, const ExtraCommand&) = default;
    };
    /// Replaces the run-time commands (the extensions' palette commands): listed by keymap().registry() in the
    /// "Extensions" group, bound in the Application scope with no default key, their user shortcuts kept. Ids of
    /// built-in commands are ignored.
    void setExtraCommands(const QList<ExtraCommand>& commands);
    /// A run-time command that exists now (false for an unavailable leftover kept for its shortcut).
    Q_INVOKABLE bool isExtraCommandAvailable(const QString& commandId) const;

signals:
    void commandTriggered(const QString& commandId);
    /// `speedNum/speedDen` is set for ShuttleStep and SetSpeed, `frames` for StepFrames.
    void transportIntent(wabcut::app::KeymapDispatcher::TransportKind kind, int speedNum, int speedDen,
                         int frames);
    void windowChanged();
    void keymapChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool handleKeyPress(QKeyEvent* event, QQuickWindow* window);
    bool handleKeyRelease(QKeyEvent* event);
    bool isSuppressed(QKeyEvent* event, QQuickItem* focus) const;
    bool focusClaimsKey(QKeyEvent* event, QQuickItem* focus) const;
    std::optional<keymap::KeyChord> chordOf(const QKeyEvent* event) const;
    keymap::Scope scopeFor(QQuickItem* item) const;
    void deliver(const keymap::KeyResult& result);
    void deliver(const std::vector<keymap::TransportIntent>& intents);
    void updateGestureTimer();
    void resetGesture();
    void attachIfUnset(QWindow* window);
    void reload();
    bool saveOverrides();
    void changed();
    struct Registry;
    /// The built-in commands plus the run-time ones and the bound "ext:" ids in `bound`; null when there are none
    /// (the keymap then uses CommandRegistry::builtin()).
    std::unique_ptr<Registry> buildRegistry(const QSet<QString>& bound) const;

    QPointer<QQuickWindow> m_window;
    QList<QPointer<QQuickWindow>> m_panelWindows; ///< addWindow()
    bool m_explicitWindow = false;
    keymap::Platform m_platform;
    QString m_overridesPath;
    bool m_customOverridesPath = false;
    std::unique_ptr<keymap::KeymapManager> m_keymap;
    std::unique_ptr<keymap::KeyDispatcher> m_dispatcher;
    QTimer m_gestureTimer;
    QSet<int> m_consumedKeys; ///< keys whose press was consumed: their release is consumed too
    int m_revision = 0;
    QList<ExtraCommand> m_extra;
    std::unique_ptr<Registry> m_registry; ///< the keymap's registry when there are run-time commands
    /// Registries of earlier keymaps: copies of a keymap (the shortcut editor's) refer to their commands.
    std::vector<std::unique_ptr<Registry>> m_retiredRegistries;
};

} // namespace wabcut::app
