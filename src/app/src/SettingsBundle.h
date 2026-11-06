#pragma once

// SettingsBundle (0.7, card 028): export, import and reset of everything Wabcut remembers about the user.
//
// What a bundle holds (one JSON file, format "wabcut-settings", version 1):
//   * "settings": every QSettings key (Preferences, workspaces, recents, window choices, AI provider choices...) except
//     secrets and what only fits this computer's screen. API keys live in the operating system's keychain and are never in
//     QSettings, the bundle or any file this class writes; a key that still looks like a secret (its name or its value)
//     is left out and counted.
//   * "files": the user's files in "areas" - the keyboard shortcut overrides and key sets (<config>/keymap), export presets
//     (<config>/Export Presets), the effect presets library and graphic templates and the personal spelling dictionary
//     (<data>/...). The legacy plaintext <config>/ai-keys.json is never read.
// Import treats the file as untrusted input: size and nesting caps, a strict key and file-name grammar (a relative name
// of one folder level, no "..", no drive letters), allowed folders and extensions, every .json file must parse, values
// of known secret shapes are dropped. The caller shows preview() first.
//
// Applying is staged: stageImport() / stageReset() write a backup bundle of the current settings (<config>/backups) and a
// "pending" file; applyStaged() - run by main() after the event loop ended for a restart, and again at the next start as
// a safety net - replaces QSettings and the files, so a running Wabcut never works against settings that change under it.
//
// reset (keepRecents): clears QSettings except the key registry of the keychain (ai/keys/...), what only fits this screen
// and - when keepRecents - the recent and starred projects and the opt-ins (AI consent and features, update checks); the
// keyboard shortcut overrides go. Presets, templates and the dictionary are the user's content, not settings: they stay.

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

namespace wabcut::app {

/// The two folders the bundle's files live in, and where backups and the staged change go.
struct SettingsBundleLocations {
    QString configDir; ///< QStandardPaths::AppConfigLocation
    QString dataDir;   ///< QStandardPaths::AppDataLocation
    static SettingsBundleLocations standard();
    QString backupsDir() const;
    QString stagedFile() const;
};

class SettingsBundle {
public:
    static constexpr int kFormatVersion = 1;
    static constexpr qint64 kMaxBundleBytes = 16 * 1024 * 1024;
    static constexpr qint64 kMaxFileBytes = 2 * 1024 * 1024;
    static constexpr int kMaxSettings = 5000;
    static constexpr int kMaxFiles = 400;
    static constexpr int kKeptBackups = 20;

    explicit SettingsBundle(SettingsBundleLocations locations = SettingsBundleLocations::standard());

    // ---- Key and value policy (pure) ----
    /// A setting that is, or points at, a secret: never exported, never imported. ai/keys/... (the key registry), a
    /// segment with the word secret, password, token, credential, authorization, bearer, or an api key / private key.
    static bool isSecretKey(const QString& key);
    /// A setting that decides where Wabcut sends the user's keys or media: the AI providers' addresses and custom providers,
    /// the AI consent and spend records, the export watch folders and the cloud locations. A settings file never sets them
    /// (one from someone else could point them at that person's server or share); the cloud locations are not exported either.
    static bool isRedirectingKey(const QString& key);
    /// Text that names a place on the network (a UNC path such as \\server\share or //server/share), also nested in a list or map.
    static bool holdsNetworkPath(const QJsonValue& value);
    /// A value shaped like an API key, a bearer token or a URL with a password in it.
    static bool looksLikeSecret(const QString& value);
    /// Keys that only fit this computer (the window's place), never exported or replaced by an import.
    static bool isMachineBoundKey(const QString& key);
    /// The key grammar an import accepts ("group/name", letters, digits, "._- ", at most 200 characters).
    static bool isValidKey(const QString& key);
    /// Kept by a reset with keepRecents: recent and starred projects and the opt-ins.
    static bool isKeptByReset(const QString& key);
    /// A file name inside an area: one level, letters, digits, " ._()-", no leading dot, not a reserved Windows name.
    static bool isValidFileName(const QString& name);

    // ---- Export ----
    /// The bundle of the current settings and files. `skipped` (when given) counts what was left out as secret or too big.
    QJsonObject build(int* skipped = nullptr) const;
    /// Writes build() to `file` atomically. { ok, error, settings, files, skipped, file }.
    QVariantMap exportTo(const QString& file) const;

    // ---- Import ----
    /// A parsed and cleaned bundle ready to apply. Unusable input: ok == false and `error`.
    struct Parsed {
        bool ok = false;
        QString error;
        QJsonObject bundle;       ///< the cleaned bundle (only valid entries)
        int formatVersion = 0;
        QString created;
        QString app;
        int settings = 0;
        int skipped = 0;          ///< entries refused (secrets, bad names, wrong types, too big)
        QStringList skippedWhy;   ///< a few of the reasons, for the preview
        QVariantList areas;       ///< [{ id, title, files }]
        int files = 0;
    };
    static Parsed parse(const QByteArray& json);
    static Parsed parseFile(const QString& file);
    /// preview() as a map for QML: { ok, error, created, app, settings, files, skipped, areas, text }.
    static QVariantMap previewMap(const Parsed& parsed);

    /// Writes a backup of the current settings and files to backupsDir() and returns its path ("" if it could not be
    /// written). `name` is the file name wanted ("" = settings-<date>-<time>.json); a taken name gets a number.
    QString writeBackup(const QString& name = {}) const;
    /// The name writeBackup() would use now.
    QString nextBackupName() const;

    // ---- Staging and applying ----
    /// Validates the file, backs up the current settings, stages the import. { ok, error, backup }.
    QVariantMap stageImport(const QString& file) const;
    /// Backs up, stages a reset. { ok, error, backup }.
    QVariantMap stageReset(bool keepRecents, const QString& backupName = {}) const;
    bool hasStaged() const;
    void discardStaged() const;
    /// Applies the staged change, if any, and removes it. Returns what it did ("" if nothing was staged).
    QString applyStaged() const;

    // ---- Direct application (what applyStaged runs) ----
    static void applyBundle(const QJsonObject& bundle, const SettingsBundleLocations& locations);
    static void applyReset(bool keepRecents, const SettingsBundleLocations& locations);

    const SettingsBundleLocations& locations() const { return m_locations; }

private:
    SettingsBundleLocations m_locations;
};

} // namespace wabcut::app
