#pragma once

// AppSettingsBridge (UX rework 2026-10-23, stream L; QML singleton `AppSettings`): Settings that are findable, and the
// General page's start-up choice.
//
//   * launchAction: what Wabcut shows when it starts - "home" (the Home screen, the default) or "reopen" (the last
//     project). projectToReopen(Home.recent) is the project to open instead of Home (an empty URL = show Home).
//     QSettings startup/launchAction.
//   * Settings > General > Projects needs nothing here: the default location for new projects is
//     Home.defaultFolderSetting (QSettings home/defaultFolder), the recent projects to keep Home.recentLimit
//     (home/recentLimit; stream H's HomeBridge), project locking ProjectFile.lockProjects (project/lockProjects).
//   * index / search(query): a static index of the Settings window's rows - { id, page, row, title, section,
//     pageTitle, detail, keywords, opens } - and a ranked search over it (title, keywords, section, page). `row` is
//     the row's key on its page (Phase 2: SettingsRow objectName "settingsRow_<row>", SettingsPage.reveal(row));
//     `opens` names the dialog a row only opens ("media", "plugins", "audio", "extensions", "shortcuts", "trainer",
//     "editing": SettingsWindow.runAction keys), "" for rows on the page itself.
//   * openSetting(id) emits openRequested(page, row): the command palette's "Settings" rows (paletteProvider()) and
//     anything else that wants a setting shown; Main opens the Settings window there.
//
//   * 0.7 (card 028) Backup and reset (Settings > General): exportSettings(url) writes every setting and the user's
//     shortcut / preset / template files to one JSON file (no secrets: API keys stay in the OS keychain), previewImport(url)
//     says what a file holds, importSettings(url) and resetAllSettings(keepRecents) back the current settings up and stage
//     the change; Main restarts Wabcut (AppShell.requestRestart) and main() applies it (SettingsBundle.h).
//
// Everything is per user and global (QSettings, the same store Preferences.qml writes).

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include "SettingsBundle.h"

#include <memory>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class PaletteProvider;

class AppSettingsBridge : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(AppSettings)
    QML_SINGLETON

    /// "home" or "reopen" (startup/launchAction).
    Q_PROPERTY(QString launchAction READ launchAction WRITE setLaunchAction NOTIFY launchActionChanged FINAL)
    /// The Settings search index, in page order: [{ id, page, row, title, section, pageTitle, detail, keywords, opens }].
    Q_PROPERTY(QVariantList index READ index CONSTANT FINAL)
    /// 0.7 (card 028): an import or reset is staged and waits for the restart.
    Q_PROPERTY(bool changeStaged READ changeStaged NOTIFY changeStagedChanged FINAL)

public:
    explicit AppSettingsBridge(QObject* parent = nullptr);

    QString launchAction() const;
    void setLaunchAction(const QString& action);
    QVariantList index() const;

    /// The project to open at launch instead of Home: with launchAction "reopen", the newest entry of `recent`
    /// (Home.recent: [{ url, path, missing }]) when it is not missing and its file exists; else an empty URL.
    Q_INVOKABLE QUrl projectToReopen(const QVariantList& recent) const;
    /// Index entries matching `query` (every word must match the title, a keyword, the section or the page), best
    /// first, at most `limit` (0 = all): the index's fields plus `score`. [] for a blank query.
    Q_INVOKABLE QVariantList search(const QString& query, int limit = 0) const;
    /// The index entry `id` ({} when unknown).
    Q_INVOKABLE QVariantMap entry(const QString& id) const;
    /// Asks the app to show setting `id` (openRequested(page, row)). False for an unknown id.
    Q_INVOKABLE bool openSetting(const QString& id);

    // ---- Backup and reset (0.7, card 028) ----
    bool changeStaged() const;
    /// Writes the settings bundle to `file`: { ok, error, file, settings, files, skipped }.
    Q_INVOKABLE QVariantMap exportSettings(const QUrl& file) const;
    /// What `file` holds, without changing anything: { ok, error, created, app, settings, files, skipped, skippedWhy,
    /// areas, lines, text }.
    Q_INVOKABLE QVariantMap previewImport(const QUrl& file) const;
    /// Backs the current settings up and stages `file` for the restart: { ok, error, backup, settings, files }.
    Q_INVOKABLE QVariantMap importSettings(const QUrl& file);
    /// What a reset would do, for the confirm dialog: { backupName, backupFolder }.
    Q_INVOKABLE QVariantMap planReset() const;
    /// Backs the current settings up (under `backupName`, else a timestamped name) and stages a reset: { ok, error, backup }.
    Q_INVOKABLE QVariantMap resetAllSettings(bool keepRecents, const QString& backupName = {});
    /// Calls a staged change off (the restart was cancelled).
    Q_INVOKABLE void discardStagedChange();
    /// Tests: the folders the bundle's files and backups live in.
    void setBundleLocations(const SettingsBundleLocations& locations) { m_bundle = SettingsBundle(locations); }

    // ---- C++ ----
    /// The command palette's "Settings" rows ("action:settings.<id>"), one per index entry; running one calls
    /// openSetting(id) on `bridge` (CommandPalette::create adds it).
    static std::shared_ptr<PaletteProvider> paletteProvider(AppSettingsBridge* bridge);

private:
    SettingsBundle m_bundle;

signals:
    void launchActionChanged();
    void changeStagedChanged();
    /// Show the Settings window on `page` (a SettingsWindow category key) and reveal `row` there ("" = the page).
    void openRequested(const QString& page, const QString& row);
};

} // namespace wabcut::app
