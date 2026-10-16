#include "HomeBridge.h"

#include "MediaBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/Time.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QQmlEngine>
#include <QSettings>
#include <QStandardPaths>

#include <array>

namespace wabcut::app {

namespace {

const QString kRecentKey = QStringLiteral("home/recentProjects");
bool s_showAtLaunch = false;

struct RateChoice {
    const char* label;
    Rate rate;
};
constexpr std::array kRates{
    RateChoice{"23.976", kRate23_976}, RateChoice{"24", kRate24},     RateChoice{"25", kRate25},
    RateChoice{"29.97", kRate29_97},   RateChoice{"30", kRate30},     RateChoice{"50", kRate50},
    RateChoice{"59.94", kRate59_94},   RateChoice{"60", kRate60},
};

QString rateLabel(Rate rate) {
    for (const RateChoice& choice : kRates) {
        if (choice.rate.num == rate.num && choice.rate.den == rate.den) {
            return QString::fromLatin1(choice.label);
        }
    }
    return QLocale::c().toString(rate.toDouble(), 'g', 6);
}

bool samePath(const QString& a, const QString& b) {
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
    return QDir::cleanPath(a).compare(QDir::cleanPath(b), sensitivity) == 0;
}

/// The documents folder: next to the app (installed builds; Contents/Resources/docs in a macOS bundle), else the
/// source tree's docs/.
QString docsFolder() {
    const QString beside = QCoreApplication::applicationDirPath() + QStringLiteral("/docs");
    if (QFileInfo(beside).isDir()) {
        return beside;
    }
#ifdef Q_OS_MACOS
    const QString resources = QCoreApplication::applicationDirPath() + QStringLiteral("/../Resources/docs");
    if (QFileInfo(resources).isDir()) {
        return QDir::cleanPath(resources);
    }
#endif
#ifdef WABCUT_DOCS_DIR
    if (const QString source = QStringLiteral(WABCUT_DOCS_DIR); QFileInfo(source).isDir()) {
        return source;
    }
#endif
    return {};
}

} // namespace

HomeBridge::HomeBridge(QObject* parent) : QObject(parent) {
    load();
}

HomeBridge* HomeBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* home = new HomeBridge;
    if (qmlEngine) {
        home->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        home->setLifecycle(qmlEngine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile"));
    }
    return home;
}

void HomeBridge::setShowAtLaunch(bool show) {
    s_showAtLaunch = show;
}

bool HomeBridge::showAtLaunch() const {
    return s_showAtLaunch;
}

void HomeBridge::setLifecycle(ProjectLifecycle* lifecycle) {
    if (m_lifecycle == lifecycle) {
        return;
    }
    if (m_lifecycle) {
        disconnect(m_lifecycle, nullptr, this, nullptr);
    }
    m_lifecycle = lifecycle;
    if (m_lifecycle) {
        connect(m_lifecycle, &ProjectLifecycle::documentChanged, this, &HomeBridge::onDocumentChanged);
    }
    onDocumentChanged();
}

void HomeBridge::setController(ProjectController* controller) {
    m_controller = controller;
}

// ---- New project ----------------------------------------------------------------------------------------

QVariantList HomeBridge::presets() const {
    struct Preset {
        const char* id;
        QString name;
        int width;
        int height;
        Rate rate;
        const char* shape;
        bool rateChoice;
        bool custom;
    };
    const std::array presets{
        Preset{"1080p23976", tr("1080p"), 1920, 1080, kRate23_976, "landscape", false, false},
        Preset{"1080p25", tr("1080p"), 1920, 1080, kRate25, "landscape", false, false},
        Preset{"1080p2997", tr("1080p"), 1920, 1080, kRate29_97, "landscape", false, false},
        Preset{"1080p30", tr("1080p"), 1920, 1080, kRate30, "landscape", false, false},
        Preset{"1080p60", tr("1080p"), 1920, 1080, kRate60, "landscape", false, false},
        Preset{"uhd", tr("4K UHD"), 3840, 2160, kRate23_976, "landscape", true, false},
        Preset{"vertical", tr("Vertical 9:16"), 1080, 1920, kRate30, "portrait", true, false},
        Preset{"square", tr("Square 1:1"), 1080, 1080, kRate30, "square", true, false},
        Preset{"custom", tr("Custom"), 1920, 1080, kRate30, "landscape", true, true},
    };
    QVariantList out;
    for (const Preset& p : presets) {
        out.append(QVariantMap{
            {QStringLiteral("id"), QString::fromLatin1(p.id)},
            {QStringLiteral("name"), p.name},
            {QStringLiteral("rateLabel"), rateLabel(p.rate)},
            {QStringLiteral("sizeText"), tr("%1 × %2").arg(p.width).arg(p.height)},
            {QStringLiteral("width"), p.width},
            {QStringLiteral("height"), p.height},
            {QStringLiteral("rateNum"), p.rate.num},
            {QStringLiteral("rateDen"), p.rate.den},
            {QStringLiteral("shape"), QString::fromLatin1(p.shape)},
            {QStringLiteral("rateChoice"), p.rateChoice},
            {QStringLiteral("custom"), p.custom},
        });
    }
    return out;
}

QVariantList HomeBridge::rates() const {
    QVariantList out;
    for (const RateChoice& choice : kRates) {
        out.append(QVariantMap{{QStringLiteral("label"), QString::fromLatin1(choice.label)},
                               {QStringLiteral("rateNum"), choice.rate.num},
                               {QStringLiteral("rateDen"), choice.rate.den}});
    }
    return out;
}

QUrl HomeBridge::defaultFolder() const {
    for (const QVariant& entry : m_recent) {
        const QVariantMap map = entry.toMap();
        if (!map.value(QStringLiteral("missing")).toBool()) {
            return QUrl::fromLocalFile(QFileInfo(map.value(QStringLiteral("path")).toString()).absolutePath());
        }
    }
    QString movies = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (movies.isEmpty()) {
        movies = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    }
    return QUrl::fromLocalFile(movies + QStringLiteral("/Wabcut"));
}

QUrl HomeBridge::projectUrl(const QString& name, const QUrl& folder) const {
    const QString base = folder.isLocalFile() ? folder.toLocalFile() : folder.toString();
    return QUrl::fromLocalFile(QDir(base).filePath(name.trimmed() + QStringLiteral(".wabcut")));
}

QString HomeBridge::projectPath(const QString& name, const QUrl& folder) const {
    return QDir::toNativeSeparators(projectUrl(name, folder).toLocalFile());
}

QString HomeBridge::validateProject(const QString& name, const QUrl& folder) const {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        return tr("Name the project");
    }
    static const QString kInvalid = QStringLiteral("\\/:*?\"<>|");
    for (const QChar c : trimmed) {
        if (kInvalid.contains(c) || c.unicode() < 0x20) {
            return tr("A project name cannot contain %1").arg(QString(c));
        }
    }
    if (trimmed.endsWith(QLatin1Char('.'))) {
        return tr("A project name cannot end with a period");
    }
    if (folder.isEmpty() || (!folder.isLocalFile() && !QFileInfo(folder.toString()).isAbsolute())) {
        return tr("Choose where to save the project");
    }
    const QFileInfo dir(folder.isLocalFile() ? folder.toLocalFile() : folder.toString());
    if (dir.exists() && !dir.isDir()) {
        return tr("%1 is a file, not a folder").arg(QDir::toNativeSeparators(dir.absoluteFilePath()));
    }
    if (QFileInfo::exists(projectUrl(trimmed, folder).toLocalFile())) {
        return tr("“%1” already exists in this folder").arg(trimmed);
    }
    return {};
}

QString HomeBridge::createProject(const QString& name, const QUrl& folder, const QVariantMap& sequence) {
    if (!m_lifecycle) {
        return tr("Projects are not available");
    }
    if (const QString why = validateProject(name, folder); !why.isEmpty()) {
        return why;
    }
    const QString dir = folder.isLocalFile() ? folder.toLocalFile() : folder.toString();
    if (!QDir().mkpath(dir)) {
        return tr("Cannot create the folder %1").arg(QDir::toNativeSeparators(dir));
    }
    QVariantMap settings = sequence;
    settings.remove(QStringLiteral("name")); // the sequence keeps its default name ("Sequence 01")
    if (!m_lifecycle->newProjectWith(settings)) {
        return m_lifecycle->lastError();
    }
    if (!m_lifecycle->saveAs(projectUrl(name, folder))) {
        return m_lifecycle->lastError();
    }
    return {};
}

// ---- Recent projects ------------------------------------------------------------------------------------

void HomeBridge::load() {
    const QVariantList stored = QSettings().value(kRecentKey).toList();
    const QDateTime now = QDateTime::currentDateTime();
    const QLocale locale;
    QVariantList entries;
    for (const QVariant& value : stored) {
        const QVariantMap e = value.toMap();
        const QString path = e.value(QStringLiteral("path")).toString();
        if (path.isEmpty()) {
            continue;
        }
        const QFileInfo info(path);
        const QDateTime opened = QDateTime::fromString(e.value(QStringLiteral("opened")).toString(), Qt::ISODate);
        QString openedText;
        if (opened.isValid()) {
            const qint64 days = opened.date().daysTo(now.date());
            const QString time = locale.toString(opened.time(), QLocale::ShortFormat);
            openedText = days == 0   ? tr("Today, %1").arg(time)
                         : days == 1 ? tr("Yesterday, %1").arg(time)
                                     : locale.toString(opened.date(), QLocale::ShortFormat);
        }
        const bool missing = !info.exists();
        entries.append(QVariantMap{
            {QStringLiteral("url"), QUrl::fromLocalFile(info.absoluteFilePath())},
            {QStringLiteral("path"), QDir::toNativeSeparators(info.absoluteFilePath())},
            {QStringLiteral("name"), info.completeBaseName()},
            {QStringLiteral("folder"), QDir::toNativeSeparators(info.absolutePath())},
            {QStringLiteral("openedText"), openedText},
            {QStringLiteral("format"), e.value(QStringLiteral("format")).toString()},
            {QStringLiteral("thumbnail"), missing ? QString() : e.value(QStringLiteral("thumbnail")).toString()},
            {QStringLiteral("missing"), missing},
        });
    }
    m_recent = entries;
}

void HomeBridge::store(const QVariantList& entries) {
    QSettings settings;
    settings.setValue(kRecentKey, entries);
    settings.sync();
    load();
    emit recentChanged();
}

void HomeBridge::refreshRecent() {
    load();
    emit recentChanged();
}

void HomeBridge::removeRecent(const QUrl& file) {
    const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    QVariantList entries = QSettings().value(kRecentKey).toList();
    entries.removeIf([&](const QVariant& e) {
        return samePath(e.toMap().value(QStringLiteral("path")).toString(), path);
    });
    store(entries);
}

void HomeBridge::clearRecent() {
    store({});
}

void HomeBridge::rememberProject(const QUrl& file) {
    const QString path = QFileInfo(file.isLocalFile() ? file.toLocalFile() : file.toString()).absoluteFilePath();
    if (path.isEmpty()) {
        return;
    }
    QVariantList entries = QSettings().value(kRecentKey).toList();
    entries.removeIf([&](const QVariant& e) {
        return samePath(e.toMap().value(QStringLiteral("path")).toString(), path);
    });
    const bool current = m_lifecycle && samePath(QDir::fromNativeSeparators(m_lifecycle->filePath()), path);
    entries.prepend(QVariantMap{
        {QStringLiteral("path"), path},
        {QStringLiteral("opened"), QDateTime::currentDateTime().toString(Qt::ISODate)},
        {QStringLiteral("format"), current ? currentFormat() : QString()},
        {QStringLiteral("thumbnail"), current ? currentThumbnail() : QString()},
    });
    while (entries.size() > kMaxRecent) {
        entries.removeLast();
    }
    store(entries);
}

void HomeBridge::onDocumentChanged() {
    if (!m_lifecycle || m_lifecycle->demo() || m_lifecycle->untitled()) {
        m_lastPath.clear();
        return;
    }
    const QString path = QDir::fromNativeSeparators(m_lifecycle->filePath());
    if (path.isEmpty()) {
        return;
    }
    if (!samePath(path, m_lastPath)) {
        m_lastPath = path;
        rememberProject(QUrl::fromLocalFile(path));
        return;
    }
    // The same project changed (saved, edited): keep its thumbnail and format up to date, in place.
    QVariantList entries = QSettings().value(kRecentKey).toList();
    for (QVariant& value : entries) {
        QVariantMap e = value.toMap();
        if (!samePath(e.value(QStringLiteral("path")).toString(), path)) {
            continue;
        }
        const QString thumbnail = currentThumbnail();
        const QString format = currentFormat();
        if (e.value(QStringLiteral("thumbnail")).toString() == thumbnail &&
            e.value(QStringLiteral("format")).toString() == format) {
            return;
        }
        e.insert(QStringLiteral("thumbnail"), thumbnail);
        e.insert(QStringLiteral("format"), format);
        value = e;
        store(entries);
        return;
    }
}

QString HomeBridge::currentThumbnail() const {
    if (!m_controller) {
        return {};
    }
    const ProjectSnapshot snapshot = m_controller->snapshot();
    if (!snapshot) {
        return {};
    }
    for (const MediaPtr& media : snapshot->media) {
        if (!media->info.hasVideo() || media->info.isStillImage || media->locator.offline) {
            continue;
        }
        if (const auto path = resolveMediaPath(media->locator, m_controller->projectFile())) {
            return ThumbnailImageProvider::urlFor(pathToUtf8(*path));
        }
    }
    return {};
}

QString HomeBridge::currentFormat() const {
    return m_controller ? m_controller->formatText() : QString();
}

// ---- Import and links -----------------------------------------------------------------------------------

QVariantList HomeBridge::importSources() const {
    const QString all = tr("All files (*)");
    return {
        QVariantMap{{QStringLiteral("id"), QStringLiteral("premiere")},
                    {QStringLiteral("name"), tr("Premiere Pro")},
                    {QStringLiteral("steps"), tr("In Premiere: File › Export › Final Cut Pro XML")},
                    {QStringLiteral("formats"), tr("XML · .prproj (experimental)")},
                    {QStringLiteral("nameFilters"),
                     QStringList{tr("Premiere Pro timelines (*.xml *.prproj)"), all}}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("resolve")},
                    {QStringLiteral("name"), tr("DaVinci Resolve")},
                    {QStringLiteral("steps"), tr("In Resolve: File › Export › Timeline › FCPXML or OTIO")},
                    {QStringLiteral("formats"), tr("FCPXML · OTIO · EDL")},
                    {QStringLiteral("nameFilters"),
                     QStringList{tr("Resolve timelines (*.fcpxml *.otio *.xml *.edl)"), all}}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("finalcut")},
                    {QStringLiteral("name"), tr("Final Cut Pro")},
                    {QStringLiteral("steps"), tr("In Final Cut Pro: File › Export XML…")},
                    {QStringLiteral("formats"), tr("FCPXML")},
                    {QStringLiteral("nameFilters"), QStringList{tr("Final Cut Pro XML (*.fcpxml *.xml)"), all}}},
    };
}

QStringList HomeBridge::timelineNameFilters() const {
    return {tr("Timelines (*.xml *.fcpxml *.otio *.edl *.prproj)"), tr("All files (*)")};
}

QVariantList HomeBridge::links() const {
    const QString folder = docsFolder();
    if (folder.isEmpty()) {
        return {};
    }
    struct Link {
        const char* id;
        const char* file;
        QString title;
        QString detail;
    };
    const std::array links{
        Link{"import", "interchange.md", tr("Import from other editors"),
             tr("What carries over from Premiere Pro, Resolve and Final Cut Pro")},
        Link{"shortcuts", "keymap.md", tr("Keyboard shortcuts"), tr("Premiere Pro's keys, and how to change them")},
        Link{"whatsnew", "roadmap.md", tr("What's new"), tr("What shipped lately, and what comes next")},
    };
    QVariantList out;
    for (const Link& link : links) {
        const QString path = QDir(folder).filePath(QString::fromLatin1(link.file));
        if (!QFileInfo::exists(path)) {
            continue;
        }
        out.append(QVariantMap{{QStringLiteral("id"), QString::fromLatin1(link.id)},
                               {QStringLiteral("title"), link.title},
                               {QStringLiteral("detail"), link.detail},
                               {QStringLiteral("url"), QUrl::fromLocalFile(path)}});
    }
    return out;
}

} // namespace wabcut::app
