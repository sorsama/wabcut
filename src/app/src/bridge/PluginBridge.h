#pragma once

// PluginBridge (M5m, QML singleton `Plugins`): the audio plugins (A.6, docs/plugins.md) in the UI.
//
//   * The plugin browser (the Effects panel's "Audio Plugins"): the scanned CLAP / VST3 effects of this computer,
//     searched by `query` and grouped by vendor or category, dragged onto a clip, a track or the master like any
//     effect (mime application/x-wabcut-effect = "plugin.<format>.<id>"), or applied here (apply()): one undo step.
//   * Scanning: rescan() on a worker (crash-isolated by the helper; a plugin that crashes or hangs goes on the
//     blocklist), "Try again" (unblock), the user's folders. A background scan started by the app (PluginSystem's
//     scanInBackground) shows as `scanning` too.
//   * "Edit plugin..." (Effect Controls, the mixer's slot menu): the plugin's own window, opened by the helper (never in
//     this process). While an editor is open its state changes are polled and saved into the project
//     (captureState -> the effect's "plugin.state.*" data), merged into one undo step per editing session, so export has
//     the editor's sound. Opening an editor and reading a state are round trips to the helper: they run on a worker
//     of this bridge and their results come back through the event loop (review A5-P2-21), so a plugin sitting in a
//     modal dialog or a helper starting cold never holds the GUI thread. Errors arrive in `message`.
//   * Badges: effectInfo(effectKey) says whether an effect is a plugin and its state (missing on this computer, crashed,
//     failed, hung; blocks it missed on the master).
//
// The plugin support is PluginSystem::current() (the app starts it); tests give a catalog and effects of their own
// (setPlugins) and fakes for the editor (setEditorHooks), so no plugin window ever opens in a test. Built without
// WABCUT_WITH_AUDIO_PLUGINS, `available` is false and every action says so.
// Principle 9: QML sees keys ("c12", "t5", "master", "e7"), plugin effect ids and display text.

#include <wabcut/model/Effects.h>
#include <wabcut/model/Error.h>
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
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>

class QJSEngine;
class QQmlEngine;

namespace wabcut::plughost {
class PluginCatalog;
class PluginEffects;
}

namespace wabcut::app {

class ProjectController;

class PluginBridge : public QObject {
    Q_OBJECT
    /// Plugin support is running (a full Windows build with the helper).
    Q_PROPERTY(bool available READ available NOTIFY availableChanged FINAL)
    /// A scan runs (rescan() or the app's background scan).
    Q_PROPERTY(bool scanning READ scanning NOTIFY scanChanged FINAL)
    /// The file being scanned, and the scan's progress 0..1 (-1 unknown).
    Q_PROPERTY(QString scanText READ scanText NOTIFY scanChanged FINAL)
    Q_PROPERTY(double progress READ progress NOTIFY scanChanged FINAL)
    /// Filter: name, vendor or category containing this (any case).
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY pluginsChanged FINAL)
    /// "vendor" or "category". UX rework: remembered (QSettings "effects/pluginGroupBy").
    Q_PROPERTY(QString groupBy READ groupBy WRITE setGroupBy NOTIFY pluginsChanged FINAL)
    /// The effect plugins matching `query`: [{ id ("plugin.clap.<id>"), name, vendor, category, format ("CLAP" | "VST3"),
    /// version, latencyText ("" or "5.3 ms"), hasEditor, problem }], by name.
    Q_PROPERTY(QVariantList plugins READ plugins NOTIFY pluginsChanged FINAL)
    /// `plugins` grouped: [{ title, key, count, items: [...] }], by title. UX rework: `key` is stable and untranslated
    /// ("vendor:<vendor>" / "category:<category>"; "vendor:" / "category:" for the unknown ones).
    Q_PROPERTY(QVariantList groups READ groups NOTIFY pluginsChanged FINAL)
    /// Every effect plugin known (not filtered by `query`; the mixer's "+ Effect" menu), and how many.
    Q_PROPERTY(QVariantList allPlugins READ allPlugins NOTIFY pluginsChanged FINAL)
    Q_PROPERTY(int pluginCount READ pluginCount NOTIFY pluginsChanged FINAL)
    /// Files that crashed or hung while scanned: [{ file (full path), name (file name), reason }].
    Q_PROPERTY(QVariantList blocked READ blocked NOTIFY pluginsChanged FINAL)
    /// The folders scanned: [{ path, user (added by the user: removable) }].
    Q_PROPERTY(QVariantList folders READ folders NOTIFY pluginsChanged FINAL)
    /// The last scan's outcome ("12 plugins · 1 blocked") or an action's error.
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)
    /// Bumped when an effect's badge may have changed (the project, a crash): bind effectInfo() through it.
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged FINAL)

public:
    using EditorOpener = std::function<std::expected<void, Error>(const EffectInstance& effect)>;
    using StateCapturer = std::function<std::expected<std::string, Error>(const EffectInstance& effect)>;
    /// The live instance's state-change count (PluginInstance::stateChanges), or nullopt when nothing plays it.
    using ChangeCounter = std::function<std::optional<std::uint64_t>(const EffectInstance& effect)>;

    explicit PluginBridge(QObject* parent = nullptr);
    ~PluginBridge() override;
    static PluginBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    void setController(ProjectController* controller);
    /// The catalog and the plugin effects (tests; the app uses PluginSystem::current()). Null: unavailable.
    void setPlugins(std::shared_ptr<plughost::PluginCatalog> catalog, std::shared_ptr<plughost::PluginEffects> effects);
    /// Tests: what opens an editor, captures a state and reads the change count (default: the plugin effects').
    void setEditorHooks(EditorOpener opener, StateCapturer capturer, ChangeCounter counter);
    /// Scans on the calling thread (tests).
    void setSynchronous(bool synchronous) { m_synchronous = synchronous; }
    /// Tests: whether groupBy is kept in QSettings (default true; the constructor read it).
    void setPersistent(bool persistent) { m_persistent = persistent; }

    bool available() const;
    bool scanning() const;
    QString scanText() const { return m_scanText; }
    double progress() const { return m_progress; }
    QString query() const { return m_query; }
    void setQuery(const QString& query);
    QString groupBy() const { return m_groupBy; }
    void setGroupBy(const QString& groupBy);
    QVariantList plugins() const { return m_plugins; }
    QVariantList groups() const;
    QVariantList allPlugins() const { return m_all; }
    int pluginCount() const { return m_pluginCount; }
    QVariantList blocked() const { return m_blocked; }
    QVariantList folders() const { return m_folders; }
    QString message() const { return m_message; }
    int revision() const { return m_revision; }

    /// Scans the folders again (new and changed files; `all`: every file). Returns "" when started, else why not.
    Q_INVOKABLE QString rescan(bool all = false);
    /// "Try again": takes a file off the blocklist and scans.
    Q_INVOKABLE QString unblock(const QString& file);
    Q_INVOKABLE QString addFolder(const QUrl& folder);
    Q_INVOKABLE QString removeFolder(const QString& path);
    /// The settings dialog (folders, rescan, blocklist): Main.qml opens it.
    Q_INVOKABLE void openSettings() { emit settingsRequested(); }

    /// Adds the plugin to `target`: a clip ("c12"; a video clip's linked audio clip), an audio track ("t5") or "master".
    /// One undo step. Returns "" or the reason.
    Q_INVOKABLE QString apply(const QString& pluginId, const QString& target);
    /// Adds it to the selected audio clip (the primary selection, or its linked audio).
    Q_INVOKABLE QString applyToSelection(const QString& pluginId);
    /// UX rework (the Effects panel's double-click / Enter): adds the plugin to EVERY selected audio clip (a selected
    /// video clip stands for its linked audio clip), one instance each, as one undo step ("Add Plugin"). Returns { ok,
    /// error ("" or why not), message (the status line's text), clips (how many clips got it) }.
    Q_INVOKABLE QVariantMap applyToSelectedClips(const QString& pluginId);
    /// The untranslated group key of a plugin row ({vendor, category}) for `groupBy` ("vendor:Wabcut").
    static QString groupKey(const QVariantMap& plugin, const QString& groupBy);
    /// What an effect of the current sequence is: { plugin, name, vendor, format, hasEditor, kind ("" | "missing" |
    /// "crashed" | "failed" | "hung"), text (the badge's), missedBlocks }; plugin false for other effects.
    Q_INVOKABLE QVariantMap effectInfo(const QString& effectKey) const;
    /// "Edit plugin...": opens the plugin's own window (in the helper). "" or the reason.
    Q_INVOKABLE QString openEditor(const QString& effectKey);
    /// Saves the editor's state into the project now (one undo step). "" or the reason.
    Q_INVOKABLE QString captureState(const QString& effectKey);
    /// Effects whose editor was opened this session ("e7"), polled for state changes.
    Q_INVOKABLE QStringList editing() const;

    /// One poll of the open editors (the timer's; tests call it): a state that changed is captured.
    void pollEditors();
    /// One poll of the badges and the background scan (the timer's; tests call it).
    void pollStatus();

signals:
    void availableChanged();
    void scanChanged();
    void pluginsChanged();
    void messageChanged();
    void revisionChanged();
    void settingsRequested();

private:
    struct Found;
    struct EditorWork; ///< the worker the editor's helper round trips run on (PluginBridge.cpp)
    std::optional<Found> findEffect(EffectInstanceId id) const;
    /// `work` (a helper round trip) on the editor worker, then `done` with its result on the GUI thread; both here in
    /// synchronous mode (tests).
    void editorJob(std::function<std::function<void(PluginBridge&)>()> work);
    void refresh();
    void setMessage(const QString& message);
    void finishScan(int plugins, int scanned, int blockedNow, const QStringList& errors, bool cancelled);
    QString storeState(EffectInstanceId effect, const std::string& base64, bool merge);
    void bumpRevision();
    std::uint64_t statusSignature() const;

    QPointer<ProjectController> m_controller;
    std::shared_ptr<plughost::PluginCatalog> m_catalog;
    std::shared_ptr<plughost::PluginEffects> m_effects;
    EditorOpener m_opener;
    StateCapturer m_capturer;
    ChangeCounter m_counter;
    bool m_synchronous = false;
    bool m_persistent = true;

    QString m_query;
    QString m_groupBy = QStringLiteral("vendor");
    QVariantList m_all;     ///< every effect plugin (unfiltered), by name
    QVariantList m_plugins; ///< filtered by m_query
    int m_pluginCount = 0;
    QVariantList m_blocked;
    QVariantList m_folders;
    QString m_message;
    int m_revision = 0;
    std::uint64_t m_statusSignature = 0;

    bool m_ownScan = false; ///< rescan() runs (else a background scan may)
    bool m_wasScanning = false;
    QString m_scanText;
    double m_progress = -1;
    std::jthread m_scanThread;
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);

    /// Editors opened this session: effect id -> the change count last saved (nullopt: not known yet).
    std::map<std::uint64_t, std::optional<std::uint64_t>> m_editing;
    std::map<std::uint64_t, bool> m_capturing; ///< an editor's state being read on the worker (one at a time)
    std::unique_ptr<EditorWork> m_editorWork;
    QTimer m_editorTimer;
    QTimer m_statusTimer;
};

struct PluginBridgeForeign {
    Q_GADGET
    QML_FOREIGN(PluginBridge)
    QML_NAMED_ELEMENT(Plugins)
    QML_SINGLETON
public:
    static PluginBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return PluginBridge::create(qmlEngine, jsEngine); }
};

} // namespace wabcut::app
