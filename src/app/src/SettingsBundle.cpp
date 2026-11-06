#include "SettingsBundle.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrl>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <map>
#include <set>

namespace wabcut::app {

namespace {

constexpr const char* kContext = "wabcut::app::SettingsBundle";

QString tx(const char* text, int n = -1) {
    return QCoreApplication::translate(kContext, text, nullptr, n);
}

constexpr int kMaxDepth = 8;
constexpr qsizetype kMaxStringChars = 1024 * 1024;
constexpr qsizetype kMaxArray = 10000;
constexpr qsizetype kMaxObject = 1000;
constexpr qint64 kMaxTotalFileBytes = 24 * 1024 * 1024;

const QString kFormat = QStringLiteral("wabcut-settings");

// ---- Areas: the user's files a bundle carries --------------------------------------------------------------------------

enum class Root { Config, Data };

struct Area {
    const char* id;
    const char* title;
    Root root;
    const char* path; ///< a folder (flat) or a single file, below the root
    bool folder;
    const char* extension;
};

// clang-format off
constexpr std::array kAreas{
    Area{"keymap", QT_TRANSLATE_NOOP("wabcut::app::SettingsBundle", "Keyboard shortcuts"), Root::Config, "keymap", true, ".json"},
    Area{"exportPresets", QT_TRANSLATE_NOOP("wabcut::app::SettingsBundle", "Export presets"), Root::Config, "Export Presets", true, ".json"},
    Area{"effectLibrary", QT_TRANSLATE_NOOP("wabcut::app::SettingsBundle", "Effect presets"), Root::Data, "effects-library.json", false, ".json"},
    Area{"graphicTemplates", QT_TRANSLATE_NOOP("wabcut::app::SettingsBundle", "Graphic templates"), Root::Data, "templates", true, ".json"},
    Area{"spelling", QT_TRANSLATE_NOOP("wabcut::app::SettingsBundle", "Personal dictionary"), Root::Data, "spelling-dictionary.txt", false, ".txt"},
};
// clang-format on

const Area* findArea(const QString& id) {
    for (const Area& a : kAreas) {
        if (id == QLatin1String(a.id))
            return &a;
    }
    return nullptr;
}

QString rootDir(const Area& area, const SettingsBundleLocations& loc) {
    return area.root == Root::Config ? loc.configDir : loc.dataDir;
}

/// The folder of a folder area, or the folder that holds a single-file area.
QString areaFolder(const Area& area, const SettingsBundleLocations& loc) {
    const QString root = rootDir(area, loc);
    return area.folder ? QDir(root).filePath(QString::fromLatin1(area.path)) : root;
}

QString areaFile(const Area& area, const SettingsBundleLocations& loc, const QString& name) {
    return area.folder ? QDir(areaFolder(area, loc)).filePath(name)
                       : QDir(rootDir(area, loc)).filePath(QString::fromLatin1(area.path));
}

/// The file names an area holds on disk (valid names with its extension).
QStringList areaFileNames(const Area& area, const SettingsBundleLocations& loc) {
    QStringList out;
    if (area.folder) {
        const QDir dir(areaFolder(area, loc));
        for (const QFileInfo& info : dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name)) {
            if (info.fileName().endsWith(QLatin1String(area.extension), Qt::CaseInsensitive) &&
                SettingsBundle::isValidFileName(info.fileName()))
                out << info.fileName();
        }
    } else if (QFileInfo(areaFile(area, loc, {})).isFile()) {
        out << QString::fromLatin1(area.path);
    }
    return out;
}

bool nameFitsArea(const Area& area, const QString& name) {
    if (!SettingsBundle::isValidFileName(name) || !name.endsWith(QLatin1String(area.extension), Qt::CaseInsensitive))
        return false;
    return area.folder || name == QLatin1String(area.path);
}

/// A .json file must hold JSON; anything else is left out of a bundle (and refused when one is imported).
bool contentFitsArea(const Area& area, const QByteArray& content) {
    if (content.size() > SettingsBundle::kMaxFileBytes)
        return false;
    if (QLatin1String(area.extension) == QLatin1String(".json")) {
        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson(content, &error);
        return error.error == QJsonParseError::NoError && !doc.isNull();
    }
    return QString::fromUtf8(content).toUtf8() == content; // text: valid UTF-8
}

bool writeFileAtomically(const QString& path, const QByteArray& content) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    if (file.write(content) != content.size())
        return false;
    return file.commit();
}

// ---- Values ---------------------------------------------------------------------------------------------------------

QJsonValue toJson(const QVariant& value, int depth) {
    if (depth > kMaxDepth)
        return QJsonValue(QJsonValue::Undefined);
    switch (value.typeId()) {
    case QMetaType::Bool:
        return value.toBool();
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Short:
    case QMetaType::UShort:
    case QMetaType::Long:
    case QMetaType::ULong:
        return static_cast<double>(value.toLongLong());
    case QMetaType::Float:
    case QMetaType::Double:
        return value.toDouble();
    case QMetaType::QString:
        return value.toString();
    case QMetaType::QByteArray: {
        QJsonObject o;
        o.insert(QStringLiteral("$bytes"), QString::fromLatin1(value.toByteArray().toBase64()));
        return o;
    }
    case QMetaType::QStringList: {
        QJsonArray a;
        for (const QString& s : value.toStringList())
            a.append(s);
        return a;
    }
    case QMetaType::QVariantList: {
        QJsonArray a;
        for (const QVariant& v : value.toList()) {
            const QJsonValue j = toJson(v, depth + 1);
            if (j.isUndefined())
                return QJsonValue(QJsonValue::Undefined);
            a.append(j);
        }
        return a;
    }
    case QMetaType::QVariantMap: {
        QJsonObject o;
        const QVariantMap map = value.toMap();
        for (auto it = map.begin(); it != map.end(); ++it) {
            const QJsonValue j = toJson(it.value(), depth + 1);
            if (j.isUndefined())
                return QJsonValue(QJsonValue::Undefined);
            o.insert(it.key(), j);
        }
        return o;
    }
    case QMetaType::QUrl:
        return value.toUrl().toString();
    case QMetaType::QDateTime:
        return value.toDateTime().toString(Qt::ISODate);
    default:
        break;
    }
    if (value.canConvert<QString>() && value.isValid()) {
        const QString text = value.toString();
        if (!text.isEmpty())
            return text;
    }
    return QJsonValue(QJsonValue::Undefined);
}

/// The JSON value as a QSettings value; nothing for what a setting cannot be.
QVariant toVariant(const QJsonValue& value) {
    switch (value.type()) {
    case QJsonValue::Bool:
        return value.toBool();
    case QJsonValue::Double: {
        const double d = value.toDouble();
        if (std::abs(d) < 9e15 && d == static_cast<double>(static_cast<qint64>(d))) {
            const auto n = static_cast<qint64>(d);
            if (n >= INT_MIN && n <= INT_MAX)
                return static_cast<int>(n);
            return static_cast<qlonglong>(n);
        }
        return d;
    }
    case QJsonValue::String:
        return value.toString();
    case QJsonValue::Array: {
        QVariantList list;
        for (const QJsonValue& v : value.toArray())
            list.append(toVariant(v));
        return list;
    }
    case QJsonValue::Object: {
        const QJsonObject o = value.toObject();
        if (o.size() == 1 && o.contains(QStringLiteral("$bytes")))
            return QByteArray::fromBase64(o.value(QStringLiteral("$bytes")).toString().toLatin1());
        QVariantMap map;
        for (auto it = o.begin(); it != o.end(); ++it)
            map.insert(it.key(), toVariant(it.value()));
        return map;
    }
    default:
        return {};
    }
}

/// `value` cleaned for a bundle: Undefined when it holds anything a setting never does or anything secret-shaped.
QJsonValue cleanValue(const QJsonValue& value, int depth, QString* why) {
    const QJsonValue bad(QJsonValue::Undefined);
    if (depth > kMaxDepth) {
        if (why)
            *why = QStringLiteral("nested too deeply");
        return bad;
    }
    switch (value.type()) {
    case QJsonValue::Bool:
    case QJsonValue::Double:
        return value;
    case QJsonValue::String: {
        const QString s = value.toString();
        if (s.size() > kMaxStringChars) {
            if (why)
                *why = QStringLiteral("a text over 1 MB");
            return bad;
        }
        if (SettingsBundle::looksLikeSecret(s)) {
            if (why)
                *why = QStringLiteral("a value that looks like a secret");
            return bad;
        }
        return value;
    }
    case QJsonValue::Array: {
        const QJsonArray in = value.toArray();
        if (in.size() > kMaxArray) {
            if (why)
                *why = QStringLiteral("a list over %1 entries").arg(kMaxArray);
            return bad;
        }
        QJsonArray out;
        for (const QJsonValue& v : in) {
            const QJsonValue c = cleanValue(v, depth + 1, why);
            if (c.isUndefined())
                return bad;
            out.append(c);
        }
        return out;
    }
    case QJsonValue::Object: {
        const QJsonObject in = value.toObject();
        if (in.size() > kMaxObject) {
            if (why)
                *why = QStringLiteral("a table over %1 entries").arg(kMaxObject);
            return bad;
        }
        if (in.size() == 1 && in.contains(QStringLiteral("$bytes"))) {
            const QString text = in.value(QStringLiteral("$bytes")).toString();
            if (text.size() > kMaxStringChars) {
                if (why)
                    *why = QStringLiteral("binary data over 1 MB");
                return bad;
            }
            return value;
        }
        QJsonObject out;
        for (auto it = in.begin(); it != in.end(); ++it) {
            if (it.key().size() > 200 || SettingsBundle::isSecretKey(it.key())) {
                if (why)
                    *why = QStringLiteral("a field named like a secret");
                return bad;
            }
            const QJsonValue c = cleanValue(it.value(), depth + 1, why);
            if (c.isUndefined())
                return bad;
            out.insert(it.key(), c);
        }
        return out;
    }
    default: // null: a setting that has no value
        if (why)
            *why = QStringLiteral("an empty value");
        return bad;
    }
}

QStringList wordsOf(const QString& segment) {
    QStringList words;
    QString current;
    const auto flush = [&] {
        if (!current.isEmpty())
            words << current.toLower();
        current.clear();
    };
    for (qsizetype i = 0; i < segment.size(); ++i) {
        const QChar c = segment[i];
        if (!c.isLetterOrNumber()) {
            flush();
            continue;
        }
        if (c.isUpper() && i > 0 && segment[i - 1].isLower())
            flush(); // camelCase: apiToken -> api, token
        current += c;
    }
    flush();
    return words;
}

QString isoNow() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

QByteArray readSmallFile(const QString& path, qint64 limit, bool* tooBig = nullptr) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    if (file.size() > limit) {
        if (tooBig)
            *tooBig = true;
        return {};
    }
    return file.readAll();
}

} // namespace

// ---- Locations ---------------------------------------------------------------------------------------------------------

SettingsBundleLocations SettingsBundleLocations::standard() {
    return {QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation),
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)};
}

QString SettingsBundleLocations::backupsDir() const {
    return QDir(configDir).filePath(QStringLiteral("backups"));
}

QString SettingsBundleLocations::stagedFile() const {
    return QDir(configDir).filePath(QStringLiteral("pending-settings.json"));
}

SettingsBundle::SettingsBundle(SettingsBundleLocations locations) : m_locations(std::move(locations)) {}

// ---- Policy ------------------------------------------------------------------------------------------------------------

bool SettingsBundle::isSecretKey(const QString& key) {
    const QString lower = key.toLower();
    if (lower == QLatin1String("ai/keys") || lower.startsWith(QLatin1String("ai/keys/")) ||
        lower.startsWith(QLatin1String("ai/plaintextkeys")))
        return true;
    static const std::set<QString> denied{
        QStringLiteral("secret"),     QStringLiteral("secrets"),    QStringLiteral("password"),
        QStringLiteral("passwords"),  QStringLiteral("passwd"),     QStringLiteral("token"),
        QStringLiteral("credential"), QStringLiteral("credentials"), QStringLiteral("authorization"),
        QStringLiteral("bearer"),     QStringLiteral("apikey"),     QStringLiteral("apikeys"),
    };
    for (const QString& segment : key.split(QLatin1Char('/'))) {
        const QStringList words = wordsOf(segment);
        for (qsizetype i = 0; i < words.size(); ++i) {
            if (denied.contains(words[i]))
                return true;
            if (i + 1 < words.size() && (words[i + 1] == QLatin1String("key") || words[i + 1] == QLatin1String("keys")) &&
                (words[i] == QLatin1String("api") || words[i] == QLatin1String("private") || words[i] == QLatin1String("access") ||
                 words[i] == QLatin1String("secret")))
                return true;
        }
    }
    return false;
}

bool SettingsBundle::isRedirectingKey(const QString& key) {
    const QString lower = key.toLower(); // the registry is not case-sensitive
    static const QStringList exact{QStringLiteral("ai/baseurls"), QStringLiteral("ai/customproviders"),
                                   QStringLiteral("ai/consent"), QStringLiteral("ai/spend"),
                                   QStringLiteral("export/watchfolders"), QStringLiteral("cloud")};
    for (const QString& name : exact) {
        if (lower == name || lower.startsWith(name + QLatin1Char('/')))
            return true;
    }
    return false;
}

bool SettingsBundle::holdsNetworkPath(const QJsonValue& value) {
    switch (value.type()) {
    case QJsonValue::String: {
        // Two separators then a name: \\host\share, //host/share, the mixed forms and \\?\UNC\host\share.
        const QString s = value.toString().trimmed();
        const auto separator = [](QChar c) { return c == QLatin1Char('/') || c == QLatin1Char('\\'); };
        return s.size() > 2 && separator(s[0]) && separator(s[1]) && !separator(s[2]);
    }
    case QJsonValue::Array:
        for (const QJsonValue& v : value.toArray()) {
            if (holdsNetworkPath(v))
                return true;
        }
        return false;
    case QJsonValue::Object: {
        const QJsonObject o = value.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) {
            if (holdsNetworkPath(it.value()))
                return true;
        }
        return false;
    }
    default:
        return false;
    }
}

bool SettingsBundle::looksLikeSecret(const QString& value) {
    if (value.size() < 12)
        return false;
    static const std::array<QRegularExpression, 11> patterns{
        QRegularExpression(QStringLiteral(R"(\bsk-[A-Za-z0-9_\-]{20,})")),
        QRegularExpression(QStringLiteral(R"(\bAIza[0-9A-Za-z_\-]{30,})")),
        QRegularExpression(QStringLiteral(R"(\b(?:ghp|gho|ghu|ghs|ghr)_[A-Za-z0-9]{20,}|\bgithub_pat_[A-Za-z0-9_]{20,})")),
        QRegularExpression(QStringLiteral(R"(\bxox[abprs]-[A-Za-z0-9\-]{10,})")),
        QRegularExpression(QStringLiteral(R"(\bBearer\s+[A-Za-z0-9._\-]{16,})"), QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral(R"(\bAKIA[0-9A-Z]{16}\b)")),
        QRegularExpression(QStringLiteral(R"(\bhf_[A-Za-z0-9]{30,})")),
        QRegularExpression(QStringLiteral(R"(://[^/\s:@]+:[^/\s@]+@)")),
        QRegularExpression(QStringLiteral(R"([?&](?:api[_-]?key|apikey|access[_-]?token|token|key|secret)=[^&\s]{8,})"),
                           QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(QStringLiteral(R"(-----BEGIN [A-Z ]*PRIVATE KEY-----)")),
        QRegularExpression(QStringLiteral(R"(\beyJ[A-Za-z0-9_\-]{10,}\.[A-Za-z0-9_\-]{10,}\.[A-Za-z0-9_\-]{10,})")),
    };
    return std::any_of(patterns.begin(), patterns.end(), [&](const QRegularExpression& p) { return p.match(value).hasMatch(); });
}

bool SettingsBundle::isMachineBoundKey(const QString& key) {
    static const QStringList keys{QStringLiteral("window/x"),      QStringLiteral("window/y"),
                                  QStringLiteral("window/width"),  QStringLiteral("window/height"),
                                  QStringLiteral("window/visibility")};
    return keys.contains(key) || key.startsWith(QLatin1String("window/geometry"));
}

bool SettingsBundle::isValidKey(const QString& key) {
    if (key.isEmpty() || key.size() > 200)
        return false;
    // Letters and digits of any script (a workspace named in Thai is a key too), "_", and "." "-" " " "(" ")" inside a part.
    static const QRegularExpression grammar(
        QStringLiteral(R"(^[\p{L}\p{N}_][\p{L}\p{N}_.\- ()]*(/[\p{L}\p{N}_][\p{L}\p{N}_.\- ()]*)*$)"),
        QRegularExpression::UseUnicodePropertiesOption);
    return grammar.match(key).hasMatch() && !key.contains(QLatin1String(".."));
}

bool SettingsBundle::isKeptByReset(const QString& key) {
    return key == QLatin1String("home/recentProjects") || key == QLatin1String("home/recentLimit") ||
           key == QLatin1String("home/starred") || key == QLatin1String("ai/consent") ||
           key.startsWith(QLatin1String("aiFeatures/")) || key == QLatin1String("updates/automatic");
}

bool SettingsBundle::isValidFileName(const QString& name) {
    if (name.isEmpty() || name.size() > 120 || name.startsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char('.')) ||
        name.endsWith(QLatin1Char(' ')))
        return false;
    static const QRegularExpression grammar(QStringLiteral(R"(^[A-Za-z0-9 ._()\-]+$)"));
    if (!grammar.match(name).hasMatch() || name.contains(QLatin1String("..")))
        return false;
    // Windows device names are not files, whatever the extension.
    static const QStringList reserved{QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),
                                      QStringLiteral("NUL"),  QStringLiteral("COM1"), QStringLiteral("COM2"),
                                      QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
                                      QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
                                      QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
                                      QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
                                      QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
                                      QStringLiteral("LPT9")};
    const QString stem = name.section(QLatin1Char('.'), 0, 0).trimmed().toUpper();
    return !reserved.contains(stem);
}

// ---- Export ------------------------------------------------------------------------------------------------------------

QJsonObject SettingsBundle::build(int* skipped) const {
    int left = 0;
    QJsonObject settings;
    {
        const QSettings store;
        for (const QString& key : store.allKeys()) {
            if (isSecretKey(key) || isMachineBoundKey(key) || key.toLower().startsWith(QLatin1String("cloud/")))
                continue; // not counted: never part of a bundle (the cloud locations are this computer's own accounts)
            if (!isValidKey(key) || settings.size() >= kMaxSettings) {
                ++left;
                continue;
            }
            const QJsonValue json = toJson(store.value(key), 0);
            if (json.isUndefined()) {
                ++left;
                continue;
            }
            QString why;
            const QJsonValue clean = cleanValue(json, 0, &why);
            if (clean.isUndefined()) {
                ++left; // a value that looks like a secret, or is too big
                continue;
            }
            settings.insert(key, clean);
        }
    }
    QJsonArray files;
    QJsonArray areaIds;
    qint64 total = 0;
    for (const Area& area : kAreas) {
        areaIds.append(QString::fromLatin1(area.id));
        for (const QString& name : areaFileNames(area, m_locations)) {
            bool tooBig = false;
            const QByteArray content = readSmallFile(areaFile(area, m_locations, name), kMaxFileBytes, &tooBig);
            if (tooBig || content.isEmpty() || !contentFitsArea(area, content) || files.size() >= kMaxFiles ||
                total + content.size() > kMaxTotalFileBytes) {
                ++left;
                continue;
            }
            total += content.size();
            files.append(QJsonObject{{QStringLiteral("area"), QString::fromLatin1(area.id)},
                                     {QStringLiteral("name"), name},
                                     {QStringLiteral("data"), QString::fromLatin1(content.toBase64())}});
        }
    }
    if (skipped)
        *skipped = left;
    QJsonObject bundle;
    bundle.insert(QStringLiteral("format"), kFormat);
    bundle.insert(QStringLiteral("version"), kFormatVersion);
    bundle.insert(QStringLiteral("created"), isoNow());
    bundle.insert(QStringLiteral("app"), QCoreApplication::applicationVersion());
    bundle.insert(QStringLiteral("os"), QSysInfo::productType());
    bundle.insert(QStringLiteral("areas"), areaIds);
    bundle.insert(QStringLiteral("settings"), settings);
    bundle.insert(QStringLiteral("files"), files);
    return bundle;
}

QVariantMap SettingsBundle::exportTo(const QString& file) const {
    int skipped = 0;
    const QJsonObject bundle = build(&skipped);
    const QByteArray text = QJsonDocument(bundle).toJson(QJsonDocument::Indented);
    if (file.isEmpty() || !writeFileAtomically(file, text)) {
        return {{QStringLiteral("ok"), false},
                {QStringLiteral("error"), tx("The settings file could not be written.")},
                {QStringLiteral("file"), file}};
    }
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("error"), QString()},
            {QStringLiteral("file"), file},
            {QStringLiteral("settings"), static_cast<int>(bundle.value(QStringLiteral("settings")).toObject().size())},
            {QStringLiteral("files"), static_cast<int>(bundle.value(QStringLiteral("files")).toArray().size())},
            {QStringLiteral("skipped"), skipped}};
}

// ---- Import ------------------------------------------------------------------------------------------------------------

SettingsBundle::Parsed SettingsBundle::parse(const QByteArray& json) {
    Parsed out;
    const auto fail = [&](const QString& error) {
        out.ok = false;
        out.error = error;
        return out;
    };
    if (json.size() > kMaxBundleBytes)
        return fail(tx("This file is too large to be a Wabcut settings file."));
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return fail(tx("This is not a Wabcut settings file."));
    const QJsonObject in = doc.object();
    if (in.value(QStringLiteral("format")).toString() != kFormat)
        return fail(tx("This is not a Wabcut settings file."));
    const int version = in.value(QStringLiteral("version")).toInt(0);
    if (version < 1)
        return fail(tx("This is not a Wabcut settings file."));
    if (version > kFormatVersion)
        return fail(tx("This settings file was made by a newer Wabcut. Update Wabcut to read it."));
    if (!in.value(QStringLiteral("settings")).isObject() && !in.value(QStringLiteral("settings")).isUndefined())
        return fail(tx("This is not a Wabcut settings file."));
    if (!in.value(QStringLiteral("files")).isArray() && !in.value(QStringLiteral("files")).isUndefined())
        return fail(tx("This is not a Wabcut settings file."));

    const auto note = [&](const QString& why) {
        ++out.skipped;
        if (out.skippedWhy.size() < 5)
            out.skippedWhy << why;
    };

    out.formatVersion = version;
    out.created = in.value(QStringLiteral("created")).toString().left(40).simplified();
    out.app = in.value(QStringLiteral("app")).toString().left(40).simplified();

    // Settings.
    const QJsonObject settings = in.value(QStringLiteral("settings")).toObject();
    if (settings.size() > kMaxSettings)
        return fail(tx("This settings file holds too many settings."));
    QJsonObject cleanSettings;
    for (auto it = settings.begin(); it != settings.end(); ++it) {
        const QString& key = it.key();
        if (isSecretKey(key)) {
            note(QStringLiteral("%1: a secret").arg(key));
            continue;
        }
        if (isMachineBoundKey(key))
            continue; // this computer's window place: kept as it is, not counted
        if (isRedirectingKey(key)) {
            note(QStringLiteral("%1: it decides where keys or media go, so a file never sets it").arg(key.left(60)));
            continue;
        }
        if (!isValidKey(key)) {
            note(QStringLiteral("%1: not a setting name").arg(key.left(60)));
            continue;
        }
        QString why;
        const QJsonValue clean = cleanValue(it.value(), 0, &why);
        if (clean.isUndefined()) {
            note(QStringLiteral("%1: %2").arg(key, why));
            continue;
        }
        if (holdsNetworkPath(clean)) {
            note(QStringLiteral("%1: a place on the network").arg(key.left(60)));
            continue;
        }
        cleanSettings.insert(key, clean);
    }
    out.settings = static_cast<int>(cleanSettings.size());

    // Files.
    const QJsonArray files = in.value(QStringLiteral("files")).toArray();
    if (files.size() > kMaxFiles)
        return fail(tx("This settings file holds too many files."));
    QJsonArray cleanFiles;
    std::set<QString> seen;
    std::map<QString, int> perArea;
    qint64 total = 0;
    for (const QJsonValue& value : files) {
        const QJsonObject f = value.toObject();
        const QString areaId = f.value(QStringLiteral("area")).toString();
        const QString name = f.value(QStringLiteral("name")).toString();
        const Area* area = findArea(areaId);
        if (!area || !nameFitsArea(*area, name)) {
            note(QStringLiteral("%1/%2: not a file Wabcut keeps").arg(areaId.left(40), name.left(60)));
            continue;
        }
        if (!seen.insert(areaId + QLatin1Char('/') + name.toLower()).second)
            continue;
        const QString text = f.value(QStringLiteral("data")).toString();
        if (text.size() > kMaxFileBytes * 4 / 3 + 8) {
            note(QStringLiteral("%1: too large").arg(name));
            continue;
        }
        const QByteArray content = QByteArray::fromBase64(text.toLatin1());
        if (content.isEmpty() || !contentFitsArea(*area, content) || total + content.size() > kMaxTotalFileBytes) {
            note(QStringLiteral("%1: not valid content").arg(name));
            continue;
        }
        total += content.size();
        ++perArea[areaId];
        cleanFiles.append(QJsonObject{{QStringLiteral("area"), areaId},
                                      {QStringLiteral("name"), name},
                                      {QStringLiteral("data"), QString::fromLatin1(content.toBase64())}});
    }
    out.files = static_cast<int>(cleanFiles.size());

    // The areas the import replaces: the ones the bundle lists; else the ones it has files for.
    QJsonArray areaIds;
    std::set<QString> listed;
    for (const QJsonValue& v : in.value(QStringLiteral("areas")).toArray()) {
        if (findArea(v.toString()) && listed.insert(v.toString()).second)
            areaIds.append(v.toString());
    }
    for (const auto& [id, count] : perArea) {
        if (listed.insert(id).second)
            areaIds.append(id);
    }
    for (const Area& a : kAreas) {
        if (listed.contains(QString::fromLatin1(a.id)))
            out.areas.append(QVariantMap{{QStringLiteral("id"), QString::fromLatin1(a.id)},
                                         {QStringLiteral("title"), tx(a.title)},
                                         {QStringLiteral("files"), perArea.contains(QString::fromLatin1(a.id)) ? perArea[QString::fromLatin1(a.id)] : 0}});
    }

    out.bundle.insert(QStringLiteral("format"), kFormat);
    out.bundle.insert(QStringLiteral("version"), version);
    out.bundle.insert(QStringLiteral("created"), out.created);
    out.bundle.insert(QStringLiteral("app"), out.app);
    out.bundle.insert(QStringLiteral("areas"), areaIds);
    out.bundle.insert(QStringLiteral("settings"), cleanSettings);
    out.bundle.insert(QStringLiteral("files"), cleanFiles);
    out.ok = true;
    return out;
}

SettingsBundle::Parsed SettingsBundle::parseFile(const QString& file) {
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) {
        Parsed out;
        out.error = tx("The file could not be opened.");
        return out;
    }
    if (f.size() > kMaxBundleBytes) {
        Parsed out;
        out.error = tx("This file is too large to be a Wabcut settings file.");
        return out;
    }
    return parse(f.readAll());
}

QVariantMap SettingsBundle::previewMap(const Parsed& parsed) {
    if (!parsed.ok)
        return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), parsed.error}};
    QStringList lines;
    lines << tx("%n setting(s)", parsed.settings);
    for (const QVariant& a : parsed.areas) {
        const QVariantMap m = a.toMap();
        lines << QStringLiteral("%1: %2").arg(m.value(QStringLiteral("title")).toString(), tx("%n file(s)", m.value(QStringLiteral("files")).toInt()));
    }
    if (parsed.skipped > 0)
        lines << tx("%n item(s) left out: not valid settings, secrets, or places that decide where keys and media go", parsed.skipped);
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("error"), QString()},
            {QStringLiteral("created"), parsed.created},
            // When it was made, in the reader's own format ("11/4/2026 10:45 PM"); the raw text when it is not a date.
            {QStringLiteral("createdText"), QDateTime::fromString(parsed.created, Qt::ISODate).isValid()
                                                ? QLocale().toString(QDateTime::fromString(parsed.created, Qt::ISODate).toLocalTime(), QLocale::ShortFormat)
                                                : parsed.created},
            {QStringLiteral("app"), parsed.app},
            {QStringLiteral("settings"), parsed.settings},
            {QStringLiteral("files"), parsed.files},
            {QStringLiteral("skipped"), parsed.skipped},
            {QStringLiteral("skippedWhy"), parsed.skippedWhy},
            {QStringLiteral("areas"), parsed.areas},
            {QStringLiteral("lines"), lines},
            {QStringLiteral("text"), lines.join(QLatin1Char('\n'))}};
}

// ---- Backups and staging ---------------------------------------------------------------------------------------------

QString SettingsBundle::nextBackupName() const {
    const QString stem = QStringLiteral("settings-") + QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd-HHmmss"));
    const QDir dir(m_locations.backupsDir());
    QString name = stem + QStringLiteral(".json");
    for (int n = 2; dir.exists(name); ++n)
        name = QStringLiteral("%1-%2.json").arg(stem).arg(n);
    return name;
}

QString SettingsBundle::writeBackup(const QString& wanted) const {
    const QDir dir(m_locations.backupsDir());
    QString name = wanted;
    if (name.isEmpty() || !isValidFileName(name) || !name.endsWith(QLatin1String(".json")) || dir.exists(name))
        name = nextBackupName();
    const QString path = dir.filePath(name);
    const QByteArray text = QJsonDocument(build()).toJson(QJsonDocument::Indented);
    if (!writeFileAtomically(path, text))
        return {};
    // Only the newest few are kept.
    QStringList backups = dir.entryList({QStringLiteral("settings-*.json")}, QDir::Files, QDir::Name);
    while (backups.size() > kKeptBackups)
        QFile::remove(dir.filePath(backups.takeFirst()));
    return path;
}

QVariantMap SettingsBundle::stageImport(const QString& file) const {
    const Parsed parsed = parseFile(file);
    if (!parsed.ok)
        return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), parsed.error}};
    const QString backup = writeBackup();
    if (backup.isEmpty())
        return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), tx("A backup of the current settings could not be written, so nothing was changed.")}};
    QJsonObject staged;
    staged.insert(QStringLiteral("action"), QStringLiteral("import"));
    staged.insert(QStringLiteral("bundle"), parsed.bundle);
    if (!writeFileAtomically(m_locations.stagedFile(), QJsonDocument(staged).toJson(QJsonDocument::Compact)))
        return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), tx("The settings could not be prepared.")}};
    return {{QStringLiteral("ok"), true}, {QStringLiteral("error"), QString()}, {QStringLiteral("backup"), backup},
            {QStringLiteral("settings"), parsed.settings}, {QStringLiteral("files"), parsed.files}};
}

QVariantMap SettingsBundle::stageReset(bool keepRecents, const QString& backupName) const {
    const QString backup = writeBackup(backupName);
    if (backup.isEmpty())
        return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), tx("A backup of the current settings could not be written, so nothing was changed.")}};
    QJsonObject staged;
    staged.insert(QStringLiteral("action"), QStringLiteral("reset"));
    staged.insert(QStringLiteral("keepRecents"), keepRecents);
    if (!writeFileAtomically(m_locations.stagedFile(), QJsonDocument(staged).toJson(QJsonDocument::Compact)))
        return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), tx("The reset could not be prepared.")}};
    return {{QStringLiteral("ok"), true}, {QStringLiteral("error"), QString()}, {QStringLiteral("backup"), backup}};
}

bool SettingsBundle::hasStaged() const {
    return QFileInfo(m_locations.stagedFile()).isFile();
}

void SettingsBundle::discardStaged() const {
    QFile::remove(m_locations.stagedFile());
}

QString SettingsBundle::applyStaged() const {
    if (!hasStaged())
        return {};
    const QByteArray content = readSmallFile(m_locations.stagedFile(), kMaxBundleBytes + 4096);
    QFile::remove(m_locations.stagedFile()); // once: a change that fails must not run again at every start
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(content, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return {};
    const QJsonObject staged = doc.object();
    const QString action = staged.value(QStringLiteral("action")).toString();
    if (action == QLatin1String("reset")) {
        applyReset(staged.value(QStringLiteral("keepRecents")).toBool(true), m_locations);
        return QStringLiteral("reset");
    }
    if (action == QLatin1String("import")) {
        // Checked again: the file sat on disk between the two runs.
        const Parsed parsed = parse(QJsonDocument(staged.value(QStringLiteral("bundle")).toObject()).toJson(QJsonDocument::Compact));
        if (!parsed.ok)
            return {};
        applyBundle(parsed.bundle, m_locations);
        return QStringLiteral("import");
    }
    return {};
}

// ---- Applying ------------------------------------------------------------------------------------------------------------

void SettingsBundle::applyBundle(const QJsonObject& bundle, const SettingsBundleLocations& locations) {
    {
        QSettings store;
        for (const QString& key : store.allKeys()) {
            if (!isSecretKey(key) && !isMachineBoundKey(key))
                store.remove(key);
        }
        const QJsonObject settings = bundle.value(QStringLiteral("settings")).toObject();
        for (auto it = settings.begin(); it != settings.end(); ++it) {
            if (!isValidKey(it.key()) || isSecretKey(it.key()) || isMachineBoundKey(it.key()))
                continue;
            const QVariant value = toVariant(it.value());
            if (value.isValid())
                store.setValue(it.key(), value);
        }
        store.sync();
    }
    const QJsonArray areas = bundle.value(QStringLiteral("areas")).toArray();
    for (const QJsonValue& id : areas) {
        const Area* area = findArea(id.toString());
        if (!area)
            continue;
        for (const QString& name : areaFileNames(*area, locations))
            QFile::remove(areaFile(*area, locations, name));
    }
    for (const QJsonValue& value : bundle.value(QStringLiteral("files")).toArray()) {
        const QJsonObject f = value.toObject();
        const Area* area = findArea(f.value(QStringLiteral("area")).toString());
        const QString name = f.value(QStringLiteral("name")).toString();
        if (!area || !nameFitsArea(*area, name))
            continue;
        writeFileAtomically(areaFile(*area, locations, name), QByteArray::fromBase64(f.value(QStringLiteral("data")).toString().toLatin1()));
    }
}

void SettingsBundle::applyReset(bool keepRecents, const SettingsBundleLocations& locations) {
    {
        QSettings store;
        for (const QString& key : store.allKeys()) {
            if (isSecretKey(key) || isMachineBoundKey(key) || (keepRecents && isKeptByReset(key)))
                continue;
            store.remove(key);
        }
        store.sync();
    }
    // The shortcut overrides of every system; presets, templates and the dictionary are content and stay.
    const QDir keymap(areaFolder(*findArea(QStringLiteral("keymap")), locations));
    for (const QString& name : keymap.entryList({QStringLiteral("overrides-*.json")}, QDir::Files))
        QFile::remove(keymap.filePath(name));
}

} // namespace wabcut::app
