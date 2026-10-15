#pragma once

// ThemeFiles (0.1.3, Settings > Appearance): reads and writes Wabcut theme files, the custom colours a user exports to
// share or back up, and checks the themes kept in the settings. A theme file is UTF-8 JSON:
//
//   { "format": "wabcut-theme", "version": 1, "name": "Midnight", "dark": true, "accent": "ice",
//     "colors": { "canvas": "#03050C", "panel": "#0F1422", ... }, "clipColors": ["#A48BEB", ... 16 entries] }
//
// Everything is checked on the way in and out (normalize): colour keys outside Theme's tokens are dropped (a newer
// Wabcut's), a colour is "#RRGGBB" or "#AARRGGBB" (stored upper case), clipColors has exactly 16 "#RRGGBB" entries or
// is left out, the name is trimmed to 64 characters, an unknown accent is left out (the current one stays). A file of
// another format, a newer version, or over 256 KiB is refused with a user-facing reason.
//
//   ThemeFiles.exportTheme(url, theme)  -> "" or why it failed
//   ThemeFiles.importTheme(url)         -> { ok, error, theme }
//   ThemeFiles.normalize(theme)         -> { ok, error, theme }

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <expected>

namespace wabcut::app {

class ThemeFiles : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit ThemeFiles(QObject* parent = nullptr);

    static constexpr int kVersion = 1;
    static constexpr qint64 kMaxFileBytes = 256 * 1024;
    static constexpr int kMaxNameLength = 64;
    static constexpr int kClipColorCount = 16;

    /// The colour tokens a theme may set (Theme.colorTokenKeys).
    static const QStringList& colorKeys();
    /// The accents a theme may name (Theme.accentOrder).
    static const QStringList& accents();

    /// The checked theme (see the header), or a user-facing reason it cannot be used.
    static std::expected<QVariantMap, QString> normalizeTheme(const QVariantMap& theme);
    /// The theme as the file's JSON (normalized first).
    static std::expected<QByteArray, QString> toJson(const QVariantMap& theme);
    static std::expected<QVariantMap, QString> fromJson(const QByteArray& json);

    Q_INVOKABLE QString exportTheme(const QUrl& file, const QVariantMap& theme) const;
    Q_INVOKABLE QVariantMap importTheme(const QUrl& file) const;
    Q_INVOKABLE QVariantMap normalize(const QVariantMap& theme) const;
    /// "Midnight.wabcut-theme.json" style file name for a theme name (characters Windows refuses replaced).
    Q_INVOKABLE QString suggestedFileName(const QString& themeName) const;
};

} // namespace wabcut::app
