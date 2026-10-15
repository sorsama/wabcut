#include "ThemeFiles.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>

namespace wabcut::app {

namespace {

const QString kFormat = QStringLiteral("wabcut-theme");

bool isColor(const QVariant& value, bool allowAlpha) {
    if (value.typeId() != QMetaType::QString)
        return false;
    static const QRegularExpression rgb(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
    static const QRegularExpression argb(QStringLiteral("^#[0-9A-Fa-f]{8}$"));
    const QString s = value.toString();
    return rgb.match(s).hasMatch() || (allowAlpha && argb.match(s).hasMatch());
}

QVariantMap result(bool ok, const QString& error, const QVariantMap& theme = {}) {
    return {{QStringLiteral("ok"), ok}, {QStringLiteral("error"), error}, {QStringLiteral("theme"), theme}};
}

QString localPath(const QUrl& file) {
    return file.isLocalFile() ? file.toLocalFile() : (file.scheme().isEmpty() ? file.toString() : QString());
}

} // namespace

ThemeFiles::ThemeFiles(QObject* parent) : QObject(parent) {}

const QStringList& ThemeFiles::colorKeys() {
    static const QStringList keys{
        QStringLiteral("canvas"),       QStringLiteral("sunken"),        QStringLiteral("panel"),
        QStringLiteral("raised"),       QStringLiteral("hover"),         QStringLiteral("border"),
        QStringLiteral("borderStrong"), QStringLiteral("rimLight"),      QStringLiteral("textPrimary"),
        QStringLiteral("textSecondary"), QStringLiteral("textTertiary"), QStringLiteral("textDisabled"),
        QStringLiteral("accentFill"),   QStringLiteral("accentOnFill"),  QStringLiteral("accentStroke"),
        QStringLiteral("accentSoft"),   QStringLiteral("accentDeep"),    QStringLiteral("beamMid"),
        QStringLiteral("beamEnd"),      QStringLiteral("playhead"),      QStringLiteral("success"),
        QStringLiteral("warning"),      QStringLiteral("danger"),        QStringLiteral("info")};
    return keys;
}

const QStringList& ThemeFiles::accents() {
    static const QStringList names{QStringLiteral("ultraviolet"), QStringLiteral("infrared"), QStringLiteral("phosphor"),
                                   QStringLiteral("sodium"), QStringLiteral("ice")};
    return names;
}

std::expected<QVariantMap, QString> ThemeFiles::normalizeTheme(const QVariantMap& theme) {
    const QVariant format = theme.value(QStringLiteral("format"));
    if (format.isValid() && format.toString() != kFormat)
        return std::unexpected(tr("This is not a Wabcut theme file."));
    const QVariant version = theme.value(QStringLiteral("version"), kVersion);
    bool numeric = false;
    const int v = version.toInt(&numeric);
    if (!numeric || v < 1)
        return std::unexpected(tr("The theme file has no valid version."));
    if (v > kVersion)
        return std::unexpected(tr("This theme was made by a newer Wabcut (theme version %1).").arg(v));

    QVariantMap out;
    out.insert(QStringLiteral("format"), kFormat);
    out.insert(QStringLiteral("version"), kVersion);
    out.insert(QStringLiteral("name"), theme.value(QStringLiteral("name")).toString().simplified().left(kMaxNameLength));
    const QVariant dark = theme.value(QStringLiteral("dark"));
    out.insert(QStringLiteral("dark"), dark.isValid() ? dark.toBool() : true);
    const QString accent = theme.value(QStringLiteral("accent")).toString();
    if (accents().contains(accent))
        out.insert(QStringLiteral("accent"), accent);

    QVariantMap colors;
    const QVariantMap given = theme.value(QStringLiteral("colors")).toMap();
    for (const QString& key : colorKeys()) {
        const QVariant value = given.value(key);
        if (isColor(value, true))
            colors.insert(key, value.toString().toUpper());
    }
    out.insert(QStringLiteral("colors"), colors);

    const QVariantList clips = theme.value(QStringLiteral("clipColors")).toList();
    if (!clips.isEmpty()) {
        if (clips.size() != kClipColorCount)
            return std::unexpected(tr("A theme's clip label colours must list all %1 labels.").arg(kClipColorCount));
        QVariantList checked;
        for (const QVariant& c : clips) {
            if (!isColor(c, false))
                return std::unexpected(tr("The clip label colour \"%1\" is not a #RRGGBB colour.").arg(c.toString()));
            checked.push_back(c.toString().toUpper());
        }
        out.insert(QStringLiteral("clipColors"), checked);
    }
    if (colors.isEmpty() && !out.contains(QStringLiteral("clipColors")) && !dark.isValid()
        && !out.contains(QStringLiteral("accent")))
        return std::unexpected(tr("The theme sets no colours."));
    return out;
}

std::expected<QByteArray, QString> ThemeFiles::toJson(const QVariantMap& theme) {
    auto normalized = normalizeTheme(theme);
    if (!normalized)
        return std::unexpected(normalized.error());
    // A fixed key order keeps exported files readable and diff-friendly.
    QJsonObject object;
    for (auto it = normalized->cbegin(); it != normalized->cend(); ++it)
        object.insert(it.key(), QJsonValue::fromVariant(it.value()));
    return QJsonDocument(object).toJson(QJsonDocument::Indented);
}

std::expected<QVariantMap, QString> ThemeFiles::fromJson(const QByteArray& json) {
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return std::unexpected(tr("The theme file is not valid JSON."));
    const QJsonObject object = doc.object();
    if (object.value(QStringLiteral("format")).toString() != kFormat)
        return std::unexpected(tr("This is not a Wabcut theme file."));
    return normalizeTheme(object.toVariantMap());
}

QString ThemeFiles::exportTheme(const QUrl& file, const QVariantMap& theme) const {
    const QString path = localPath(file);
    if (path.isEmpty())
        return tr("Themes can only be saved to a file on this computer.");
    auto json = toJson(theme);
    if (!json)
        return json.error();
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly))
        return tr("Could not write %1: %2").arg(path, out.errorString());
    out.write(*json);
    if (!out.commit())
        return tr("Could not write %1: %2").arg(path, out.errorString());
    return {};
}

QVariantMap ThemeFiles::importTheme(const QUrl& file) const {
    const QString path = localPath(file);
    if (path.isEmpty())
        return result(false, tr("Themes can only be read from a file on this computer."));
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly))
        return result(false, tr("Could not read %1: %2").arg(path, in.errorString()));
    if (in.size() > kMaxFileBytes)
        return result(false, tr("The file is too large to be a theme."));
    auto theme = fromJson(in.readAll());
    if (!theme)
        return result(false, theme.error());
    return result(true, {}, *theme);
}

QVariantMap ThemeFiles::normalize(const QVariantMap& theme) const {
    auto normalized = normalizeTheme(theme);
    return normalized ? result(true, {}, *normalized) : result(false, normalized.error());
}

QString ThemeFiles::suggestedFileName(const QString& themeName) const {
    QString base = themeName.simplified();
    static const QRegularExpression unsafe(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1F]"));
    base.replace(unsafe, QStringLiteral("-"));
    if (base.isEmpty())
        base = tr("Theme");
    return base + QStringLiteral(".wabcut-theme.json");
}

} // namespace wabcut::app
