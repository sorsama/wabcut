#include "ProjectLifecycle.h"

#include "BridgeIds.h"
#include "MediaBridge.h"
#include "ProjectController.h"

#include <wabcut/editor/MediaImport.h>
#include <wabcut/editor/Recovery.h>
#include <wabcut/editor/SourceEdit.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/interchange/ImportSummary.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/Timecode.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QPointer>
#include <QImage>
#include <QThreadPool>
#include <QDir>
#include <QLocale>
#include <QSettings>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QStandardPaths>

#include <algorithm>
#include <climits>
#include <chrono>
#include <filesystem>

namespace wabcut::app {

namespace {

std::optional<MediaId> parseMediaKey(const QString& text) {
    return detail::parseId<MediaId>(QLatin1Char('m'), text);
}

std::filesystem::path toPath(const QUrl& url) {
    const QString local = url.isLocalFile() ? url.toLocalFile() : url.toString();
    return std::filesystem::path(local.toStdU16String());
}

QString toQString(const std::filesystem::path& path) {
    return QString::fromStdU16String(path.u16string());
}

QUrl toUrl(const std::filesystem::path& path) {
    return QUrl::fromLocalFile(toQString(path));
}

/// Sequence settings for a new project: the first video's size and rate (Premiere's "New Sequence from Clip"),
/// else 1920x1080 at 30 fps.
edit::SequenceSettings settingsFor(const QList<QUrl>& media) {
    edit::SequenceSettings settings;
    for (const QUrl& url : media) {
        auto item = editor::describeMediaFile(toPath(url));
        if (!item || !item->info.hasVideo() || item->info.isStillImage) {
            continue;
        }
        const VideoStreamInfo& v = item->info.video.front();
        if (v.width > 0 && v.height > 0) {
            const bool sideways = v.rotationDegrees == 90 || v.rotationDegrees == 270;
            settings.width = sideways ? v.height : v.width;
            settings.height = sideways ? v.width : v.height;
        }
        if (v.frameRate.num > 0 && v.frameRate.den > 0 && !v.variableFrameRate) {
            settings.frameRate = v.frameRate;
        }
        break;
    }
    return settings;
}

ProjectState emptyProject(const edit::SequenceSettings& settings) {
    auto created = edit::addSequence(ProjectState{}, settings);
    return created ? std::move(created->state) : ProjectState{};
}

QVariantMap reportToMap(const interchange::ImportResult& result, const std::filesystem::path& file) {
    using interchange::ReportOutcome;
    QVariantList entries;
    for (ReportOutcome outcome : {ReportOutcome::Dropped, ReportOutcome::Approximated}) {
        for (const interchange::ReportEntry* e : result.report.filter(outcome)) {
            QString location = QString::fromStdString(e->location.element);
            if (e->location.line > 0) {
                location = QStringLiteral("line %1 · %2").arg(e->location.line).arg(location);
            }
            entries.append(QVariantMap{
                {QStringLiteral("outcome"), QString::fromUtf8(interchange::toString(outcome).data())},
                {QStringLiteral("category"), QString::fromUtf8(interchange::toString(e->category).data())},
                {QStringLiteral("reason"), QString::fromStdString(e->reason)},
                {QStringLiteral("sequence"), QString::fromStdString(e->sequence)},
                {QStringLiteral("location"), location},
            });
        }
    }
    QVariantList unresolved;
    for (const interchange::UnresolvedMedia& m : result.unresolvedMedia) {
        unresolved.append(QVariantMap{{QStringLiteral("name"), QString::fromStdString(m.name)},
                                      {QStringLiteral("path"), QString::fromStdString(m.path)}});
    }
    interchange::SummaryOptions summary;
    summary.listClips = false;
    return {
        {QStringLiteral("file"), toQString(file.filename())},
        {QStringLiteral("format"), QString::fromUtf8(interchange::displayName(result.format).data())},
        {QStringLiteral("sequences"), static_cast<int>(result.sequences.size())},
        {QStringLiteral("media"), static_cast<int>(result.media.size())},
        {QStringLiteral("unresolved"), unresolved},
        {QStringLiteral("carried"), static_cast<int>(result.report.count(ReportOutcome::Carried))},
        {QStringLiteral("approximated"), static_cast<int>(result.report.count(ReportOutcome::Approximated))},
        {QStringLiteral("dropped"), static_cast<int>(result.report.count(ReportOutcome::Dropped))},
        {QStringLiteral("entries"), entries},
        {QStringLiteral("summary"), QString::fromStdString(interchange::formatSummary(result, summary))},
    };
}

} // namespace

ProjectLifecycle::ProjectLifecycle(ProjectController* controller, QObject* parent)
    : QObject(parent), m_controller(controller) {
    Q_ASSERT(controller);
    editor::ProjectDocumentOptions options;
    options.untitledAutosaveDir =
        toPath(QUrl::fromLocalFile(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
                                   QStringLiteral("/Autosave")));
    m_untitledAutosaveDir = options.untitledAutosaveDir;
    m_document = std::make_unique<editor::ProjectDocument>(controller->session(), options);
    scanRecovery();
    m_autosaveText = tr("Not autosaved yet");

    connect(controller, &ProjectController::revisionChanged, this, &ProjectLifecycle::onRevisionChanged);
    connect(controller, &ProjectController::sessionClosing, this, [this] {
        m_autosaveTimer.stop();
        cleanExit();
        m_document.reset();
        m_controller = nullptr;
    }, Qt::DirectConnection);

    m_autosaveTimer.setInterval(m_autosaveSeconds * 1000);
    connect(&m_autosaveTimer, &QTimer::timeout, this, [this] {
        if (!m_demo) {
            startBackgroundAutosave(); // the file work on a worker (P.1)
        }
    });
    m_autosaveTimer.start();
}

ProjectLifecycle::~ProjectLifecycle() {
    {
        std::lock_guard lock(m_scanGate->mutex);
        m_scanGate->owner = nullptr;
    }
    cleanExit();
}

void ProjectLifecycle::cleanExit() {
    waitForAutosave(); // an autosave still being written must not land after its files are removed
    // What is on disk as the project is now the truth (saved, or changes the user chose not to keep).
    if (m_document && !m_demo) {
        m_document->removeAutosaves();
    }
}

void ProjectLifecycle::rememberProject() {
    if (m_document && !m_document->isUntitled()) {
        QSettings().setValue(QStringLiteral("project/lastFile"), toQString(m_document->file()));
    }
}

namespace {

QString whenText(std::filesystem::file_time_type t) {
    const auto when = QDateTime::fromStdTimePoint(
        std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::clock_cast<std::chrono::system_clock>(t)));
    return QLocale().toString(when, QLocale::ShortFormat);
}

QString durationText(Ticks t) {
    const auto s = static_cast<qint64>(t.toSeconds() + 0.5);
    return QStringLiteral("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
}

/// A recovery version as QML shows it; `current` is the version saved now (for the comparison), if any.
QVariantMap versionMap(const editor::RecoveryEntry& e, const editor::ProjectSummary* current) {
    using Kind = editor::RecoveryEntry::Kind;
    const editor::ProjectSummary& s = e.summary;
    const QString kind = e.kind == Kind::Current    ? QStringLiteral("current")
                         : e.kind == Kind::Autosave ? QStringLiteral("autosave")
                         : e.kind == Kind::Backup   ? QStringLiteral("backup")
                                                    : QStringLiteral("untitled");
    const QString title = e.kind == Kind::Current    ? QObject::tr("Saved version")
                          : e.kind == Kind::Autosave ? (e.index == 1 ? QObject::tr("Autosave") : QObject::tr("Autosave %1").arg(e.index))
                          : e.kind == Kind::Backup   ? QObject::tr("Backup %1").arg(e.index)
                                                     : QObject::tr("Untitled project (autosave)");
    QVariantMap m{
        {QStringLiteral("url"), QUrl::fromLocalFile(QString::fromStdU16String(e.file.u16string()))},
        {QStringLiteral("file"), QString::fromStdU16String(e.file.filename().u16string())},
        {QStringLiteral("kind"), kind},
        {QStringLiteral("title"), title},
        {QStringLiteral("when"), whenText(s.modified)},
        {QStringLiteral("loadable"), s.loadable},
        {QStringLiteral("error"), QString::fromStdString(s.error)},
        {QStringLiteral("sequences"), s.sequences},
        {QStringLiteral("clips"), s.clips},
        {QStringLiteral("media"), s.media},
        {QStringLiteral("cues"), s.captionCues},
        {QStringLiteral("duration"), durationText(s.duration)},
        {QStringLiteral("sequence"), QString::fromStdString(s.mainSequence)},
        {QStringLiteral("newer"), e.newerThanProject},
        {QStringLiteral("poster"), QString()},
    };
    if (current && e.kind != Kind::Current && e.kind != Kind::UntitledAutosave && s.loadable) {
        QStringList diff;
        const auto delta = [&](int a, int b, const QString& what) {
            if (a != b) {
                diff << QStringLiteral("%1%2 %3").arg(a > b ? QStringLiteral("+") : QString()).arg(a - b).arg(what);
            }
        };
        delta(s.sequences, current->sequences, QObject::tr("sequences"));
        delta(s.clips, current->clips, QObject::tr("clips"));
        delta(s.media, current->media, QObject::tr("media"));
        if (s.duration != current->duration) {
            const double d = (s.duration - current->duration).toSeconds();
            diff << QObject::tr("%1%2 s long").arg(d > 0 ? QStringLiteral("+") : QString()).arg(d, 0, 'f', 1);
        }
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(s.modified - current->modified).count();
        const auto magnitude = static_cast<int>(std::min<long long>(std::llabs(seconds), INT_MAX));
        const QString age = magnitude < 60 ? QObject::tr("%n s", "", magnitude) : QObject::tr("%n min", "", magnitude / 60);
        diff << (seconds >= 0 ? QObject::tr("%1 newer than the saved version").arg(age)
                              : QObject::tr("%1 older than the saved version").arg(age));
        m.insert(QStringLiteral("comparison"), diff.isEmpty() ? QObject::tr("Same as the saved version") : diff.join(QStringLiteral(" · ")));
    } else {
        m.insert(QStringLiteral("comparison"), QString());
    }
    return m;
}

} // namespace

void ProjectLifecycle::scanRecoveryVersions() {
    std::filesystem::path project = m_document ? m_document->file() : std::filesystem::path{};
    if (project.empty()) {
        const QString last = QSettings().value(QStringLiteral("project/lastFile")).toString();
        if (!last.isEmpty()) {
            project = toPath(QUrl::fromLocalFile(last));
        }
    }
    const std::filesystem::path untitledDir = m_untitledAutosaveDir;
    const std::filesystem::path posters =
        toPath(QUrl::fromLocalFile(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))) / "recovery-posters";
    const std::uint64_t scan = ++m_versionsScan;
    m_versionsScanning = true;
    emit recoveryVersionsChanged();
    {
        std::lock_guard lock(m_scanGate->mutex);
        m_scanGate->owner = this;
    }
    const auto post = [gate = m_scanGate, scan](QVariantList list, bool finished) {
        std::lock_guard lock(gate->mutex);
        if (ProjectLifecycle* owner = gate->owner) {
            QMetaObject::invokeMethod(
                owner,
                [owner, scan, list = std::move(list), finished] {
                    if (scan != owner->m_versionsScan) {
                        return; // a newer scan runs
                    }
                    owner->m_versions = list;
                    owner->m_versionsScanning = !finished;
                    emit owner->recoveryVersionsChanged();
                },
                Qt::QueuedConnection);
        }
    };
    QThreadPool::globalInstance()->start([project, untitledDir, posters, post] {
        const std::vector<editor::RecoveryEntry> entries = editor::recoveryEntries(project, untitledDir);
        const editor::ProjectSummary* current = nullptr;
        for (const editor::RecoveryEntry& e : entries) {
            if (e.kind == editor::RecoveryEntry::Kind::Current) {
                current = &e.summary;
            }
        }
        QVariantList list;
        for (const editor::RecoveryEntry& e : entries) {
            list.append(versionMap(e, current));
        }
        post(list, false);
        // Posters: the picture a third of the way into each version's first sequence (export renderer, offscreen).
        std::error_code ec;
        std::filesystem::create_directories(posters, ec);
        for (qsizetype i = 0; i < list.size(); ++i) {
            const editor::RecoveryEntry& e = entries[static_cast<std::size_t>(i)];
            if (!e.summary.loadable) {
                continue;
            }
            auto state = io::loadProject(e.file);
            if (!state) {
                continue;
            }
            auto poster = editor::renderPoster(*state, e.project, 320);
            if (!poster) {
                continue;
            }
            const QImage image(poster->rgba.data(), poster->width, poster->height, poster->width * 4,
                               QImage::Format_RGBA8888);
            const std::filesystem::path file =
                posters / (std::to_string(std::hash<std::string>{}(pathToUtf8(e.file))) + "-" +
                           std::to_string(e.summary.modified.time_since_epoch().count()) + ".png");
            const QString path = QString::fromStdU16String(file.u16string());
            if (image.save(path)) {
                QVariantMap m = list[i].toMap();
                m.insert(QStringLiteral("poster"), QUrl::fromLocalFile(path));
                list[i] = m;
                post(list, false);
            }
        }
        post(list, true);
    });
}

bool ProjectLifecycle::openRecoveryCopy(const QUrl& file) {
    if (!m_document) {
        return false;
    }
    waitForAutosave(); // P.1: an autosave being written lands before the files change
    m_controller->setSelection({});
    // Untitled: never overwrites the project. Media resolve relative to the project the copy belongs to (P2-11).
    if (auto opened = m_document->recover(toPath(file), {}); !opened) {
        setError(tr("Cannot open %1: %2").arg(file.fileName(), QString::fromStdString(opened.error().message)));
        return false;
    }
    setError({});
    m_controller->setProjectFile(m_document->file());
    m_controller->setCurrentSequenceIndex(0);
    m_controller->setPlayhead({});
    m_demo = false;
    m_autosaveText = tr("Copy of %1").arg(file.fileName());
    emit autosaveChanged();
    adoptDocument();
    if (const int offline = offlineCount(); offline > 0) {
        emit offlineMediaFound(offline);
    }
    return true;
}

void ProjectLifecycle::scanRecovery() {
    // What to scan is read here (settings, the document's options); the files are read on a worker (P.1).
    const QString last = m_document ? QSettings().value(QStringLiteral("project/lastFile")).toString() : QString();
    const std::filesystem::path projectFile = last.isEmpty() ? std::filesystem::path{} : toPath(QUrl::fromLocalFile(last));
    const std::filesystem::path untitledDir = m_untitledAutosaveDir;
    const int copies = m_document ? m_document->autosaveCopies() : 1;
    const std::uint64_t scan = ++m_recoveryScanSerial;
    {
        std::lock_guard lock(m_scanGate->mutex);
        m_scanGate->owner = this;
    }
    m_recoveryScan = std::async(std::launch::async, [gate = m_scanGate, scan, last, projectFile, untitledDir, copies] {
        RecoveryScan out;
        out.lastProject = last;
        out.untitled = editor::ProjectDocument::untitledRecoveryFiles(untitledDir);
        if (!projectFile.empty()) {
            out.last = editor::ProjectDocument::newestUnsavedAutosave(projectFile, copies); // the newest one, as before
        }
        // Announce it, unless something collected it first or a newer scan replaced it.
        std::lock_guard lock(gate->mutex);
        if (ProjectLifecycle* owner = gate->owner) {
            QMetaObject::invokeMethod(
                owner,
                [owner, scan] {
                    if (scan == owner->m_recoveryScanSerial) {
                        owner->collectRecovery(false);
                    }
                },
                Qt::QueuedConnection);
        }
        return out;
    });
}

void ProjectLifecycle::collectRecovery(bool queuedNotify) const {
    if (!m_recoveryScan.valid()) {
        return;
    }
    const RecoveryScan scan = m_recoveryScan.get(); // waits only while the worker still runs
    QVariantList found;
    const auto describe = [](const editor::RecoveryFile& r, const QString& project) {
        const auto when = QDateTime::fromStdTimePoint(std::chrono::time_point_cast<std::chrono::milliseconds>(
            std::chrono::clock_cast<std::chrono::system_clock>(r.modified)));
        return QVariantMap{
            {QStringLiteral("url"), toUrl(r.file)},
            {QStringLiteral("name"), project.isEmpty() ? tr("Untitled project") : QFileInfo(project).completeBaseName()},
            {QStringLiteral("when"), QLocale().toString(when, QLocale::ShortFormat)},
            {QStringLiteral("project"), project.isEmpty() ? QUrl() : QUrl::fromLocalFile(project)},
        };
    };
    for (const editor::RecoveryFile& r : scan.untitled) {
        found.append(describe(r, {}));
    }
    if (scan.last) {
        found.append(describe(*scan.last, scan.lastProject));
    }
    if (found == m_recovery) {
        return; // nothing to announce (the scan at construction used to emit before anything listened)
    }
    m_recovery = std::move(found);
    auto* self = const_cast<ProjectLifecycle*>(this);
    if (queuedNotify) { // from the property's getter: never emit inside a binding's evaluation
        QMetaObject::invokeMethod(self, [self] { emit self->recoveryChanged(); }, Qt::QueuedConnection);
    } else {
        emit self->recoveryChanged();
    }
}

bool ProjectLifecycle::recover(const QUrl& file) {
    if (!m_document) {
        return false;
    }
    waitForAutosave(); // P.1: an autosave being written lands before the files change
    collectRecovery(false);
    QVariantMap entry;
    for (const QVariant& v : std::as_const(m_recovery)) {
        if (v.toMap().value(QStringLiteral("url")).toUrl() == file) {
            entry = v.toMap();
        }
    }
    const QUrl project = entry.value(QStringLiteral("project")).toUrl();
    const std::filesystem::path recoveryFile = toPath(file);
    m_controller->setSelection({});
    if (auto recovered = m_document->recover(recoveryFile, project.isEmpty() ? std::filesystem::path{} : toPath(project));
        !recovered) {
        setError(tr("Cannot recover: %1").arg(QString::fromStdString(recovered.error().message)));
        return false;
    }
    setError({});
    m_controller->setProjectFile(m_document->file());
    m_controller->setCurrentSequenceIndex(0);
    m_controller->setPlayhead({});
    m_demo = false;
    if (!project.isEmpty()) {
        rememberProject();
    }
    // The recovered revision is written again as this document's autosave right away. Nothing is deleted here: an
    // untitled source goes only once that write succeeded (the document retires it), and a failed write (disk full,
    // no permission) keeps it for the next start.
    autosaveNow();
    dismissRecovery(file);
    m_autosaveText = tr("Recovered");
    emit autosaveChanged();
    adoptDocument();
    if (const int offline = offlineCount(); offline > 0) {
        emit offlineMediaFound(offline);
    }
    return true;
}

void ProjectLifecycle::dismissRecovery(const QUrl& file) {
    collectRecovery(false);
    QVariantList kept;
    for (const QVariant& v : std::as_const(m_recovery)) {
        if (v.toMap().value(QStringLiteral("url")).toUrl() != file) {
            kept.append(v);
        }
    }
    m_recovery = std::move(kept);
    emit recoveryChanged();
}

void ProjectLifecycle::discardRecovery(const QUrl& file) {
    waitForAutosave(); // P.1: an autosave being written lands before the files change
    collectRecovery(false);
    QVariantList kept;
    for (const QVariant& v : std::as_const(m_recovery)) {
        if (v.toMap().value(QStringLiteral("url")).toUrl() == file) {
            std::error_code ec;
            std::filesystem::remove(toPath(file), ec);
        } else {
            kept.append(v);
        }
    }
    m_recovery = std::move(kept);
    emit recoveryChanged();
}

void ProjectLifecycle::discardAllRecovery() {
    collectRecovery(false);
    const QVariantList all = m_recovery;
    for (const QVariant& v : all) {
        discardRecovery(v.toMap().value(QStringLiteral("url")).toUrl());
    }
}

ProjectLifecycle* ProjectLifecycle::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* controller = qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    // ProjectController is default-constructible, so QML builds that singleton with its constructor and never
    // calls ProjectController::create(), which is where the library's thumbnail provider was meant to be
    // registered (Qt prefers the constructor over a static create()). Register it here, before any card asks.
    if (!qmlEngine->imageProvider(QLatin1String(ThumbnailImageProvider::kProviderId))) {
        qmlEngine->addImageProvider(QLatin1String(ThumbnailImageProvider::kProviderId),
                                    new ThumbnailImageProvider(controller->mediaCacheDirectory().toStdU16String()));
    }
    return new ProjectLifecycle(controller);
}

// ---- Properties -------------------------------------------------------------------------------------

QString ProjectLifecycle::name() const {
    if (m_demo) {
        return tr("Demo project");
    }
    return m_document ? QString::fromStdString(m_document->displayName()) : QString();
}

QString ProjectLifecycle::windowTitle() const {
    //: Window title: project name, then the app name
    const QString title = tr("%1 — Wabcut").arg(name());
    return dirty() ? QStringLiteral("● ") + title : title;
}

bool ProjectLifecycle::dirty() const {
    return m_document && !m_demo && m_document->isDirty();
}

bool ProjectLifecycle::untitled() const {
    return !m_document || m_document->isUntitled();
}

QString ProjectLifecycle::filePath() const {
    return m_document ? QDir::toNativeSeparators(toQString(m_document->file())) : QString();
}

QUrl ProjectLifecycle::folder() const {
    if (m_document && !m_document->isUntitled()) {
        return toUrl(m_document->file().parent_path());
    }
    return QUrl::fromLocalFile(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation));
}

void ProjectLifecycle::setAutosaveIntervalSeconds(int seconds) {
    seconds = std::max(5, seconds);
    if (seconds == m_autosaveSeconds) {
        return;
    }
    m_autosaveSeconds = seconds;
    m_autosaveTimer.setInterval(seconds * 1000);
    emit autosaveChanged();
}

QVariantList ProjectLifecycle::offlineMedia() const {
    QVariantList list;
    if (!m_document) {
        return list;
    }
    const ProjectSnapshot snap = m_document->session().snapshot();
    for (const MediaPtr& media : snap->media) {
        if (media->locator.offline) {
            list.append(QVariantMap{{QStringLiteral("id"), mediaKey(media->id)},
                                    {QStringLiteral("name"), QString::fromStdString(media->name)},
                                    {QStringLiteral("path"), QString::fromStdString(media->locator.path)}});
        }
    }
    // LUT assets (I.5) are found and relinked with the media; their ids are their references ("asset:<id>").
    for (const editor::AssetStatus& asset : m_document->assetStatus()) {
        if (asset.offline) {
            list.append(QVariantMap{{QStringLiteral("id"), QString::fromStdString(assetReference(asset.asset))},
                                    {QStringLiteral("name"), QString::fromStdString(asset.name)},
                                    {QStringLiteral("path"), QString::fromStdString(asset.path)},
                                    {QStringLiteral("kind"), QString::fromStdString(asset.kind)}});
        }
    }
    return list;
}

int ProjectLifecycle::offlineCount() const {
    if (!m_document || m_demo) {
        return 0;
    }
    const auto assets = m_document->assetStatus();
    return static_cast<int>(m_document->offlineMedia().size()) +
           static_cast<int>(std::ranges::count_if(assets, [](const editor::AssetStatus& a) { return a.offline; }));
}

QStringList ProjectLifecycle::importNameFilters() const {
    return {
        tr("Media and timelines (*.mp4 *.mov *.mkv *.mxf *.avi *.m4v *.webm *.wav *.mp3 *.aac *.m4a *.flac *.png "
           "*.jpg *.jpeg *.tif *.tiff *.xml *.fcpxml *.otio *.edl *.prproj)"),
        tr("Timelines (*.xml *.fcpxml *.otio *.edl *.prproj)"),
        tr("All files (*)"),
    };
}

// ---- State ------------------------------------------------------------------------------------------

void ProjectLifecycle::setError(const QString& message) {
    if (message == m_lastError) {
        return;
    }
    m_lastError = message;
    emit lastErrorChanged();
}

void ProjectLifecycle::adoptDocument() {
    m_lastDirty = dirty();
    m_lastOfflineCount = offlineCount();
    emit documentChanged();
    emit mediaStatusChanged();
}

void ProjectLifecycle::onRevisionChanged() {
    if (!m_document) {
        return;
    }
    const bool nowDirty = dirty();
    if (nowDirty != m_lastDirty) {
        m_lastDirty = nowDirty;
        emit documentChanged();
    }
    const int offline = offlineCount();
    if (offline != m_lastOfflineCount) {
        m_lastOfflineCount = offline;
        emit mediaStatusChanged();
    }
}

// ---- File -------------------------------------------------------------------------------------------

void ProjectLifecycle::startNewProject(const QList<QUrl>& firstMedia) {
    startNewProjectWith(settingsFor(firstMedia));
}

void ProjectLifecycle::startNewProjectWith(const edit::SequenceSettings& settings) {
    waitForAutosave(); // P.1: an autosave being written lands before the files change
    m_controller->setSelection({});
    m_document->newProject(emptyProject(settings));
    m_controller->setProjectFile({});
    m_controller->setCurrentSequenceIndex(0);
    m_controller->setPlayhead({});
    m_demo = false;
    m_autosaveText = tr("Not autosaved yet");
    emit autosaveChanged();
    adoptDocument();
}

void ProjectLifecycle::newProject() {
    if (!m_document) {
        return;
    }
    setError({});
    startNewProject({});
}

QUrl ProjectLifecycle::launchProject() const {
    return launchProjectIn(QCoreApplication::arguments());
}

QUrl ProjectLifecycle::launchProjectIn(const QStringList& arguments) {
    for (qsizetype i = 1; i < arguments.size(); ++i) {
        const QString& argument = arguments[i];
        if (argument == QLatin1String("--rhi")) {
            ++i; // its value
            continue;
        }
        if (argument.startsWith(QLatin1Char('-')) || !argument.endsWith(QLatin1String(".wabcut"), Qt::CaseInsensitive)) {
            continue;
        }
        const QFileInfo info(argument);
        if (info.isFile()) {
            return QUrl::fromLocalFile(info.absoluteFilePath());
        }
    }
    return {};
}

bool ProjectLifecycle::open(const QUrl& file) {
    if (!m_document) {
        return false;
    }
    waitForAutosave(); // P.1: an autosave being written lands before the files change
    const std::filesystem::path path = toPath(file);
    m_controller->setSelection({});
    if (auto opened = m_document->open(path); !opened) {
        setError(tr("Cannot open %1: %2").arg(toQString(path.filename()), QString::fromStdString(opened.error().message)));
        return false;
    }
    setError({});
    m_controller->setProjectFile(m_document->file());
    m_controller->setCurrentSequenceIndex(0);
    m_controller->setPlayhead({});
    m_demo = false;
    m_autosaveText = tr("Opened %1").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm")));
    emit autosaveChanged();
    adoptDocument();
    if (const int offline = offlineCount(); offline > 0) {
        emit offlineMediaFound(offline);
    }
    // A newer autosave of this project means an unclean exit lost work since the last save: offer it.
    const QString project = toQString(m_document->file());
    QSettings().setValue(QStringLiteral("project/lastFile"), project);
    scanRecovery();
    collectRecovery(false); // this project's answer is needed now (no parse at all after a clean exit)
    const bool newer = std::ranges::any_of(m_recovery, [&](const QVariant& v) {
        return v.toMap().value(QStringLiteral("project")).toUrl() == QUrl::fromLocalFile(project);
    });
    if (newer) {
        emit recoveryAvailable();
    }
    return true;
}

bool ProjectLifecycle::save() {
    if (!m_document) {
        return false;
    }
    waitForAutosave(); // P.1: an autosave being written lands before the files change
    if (m_document->isUntitled()) {
        setError(tr("The project has no file yet"));
        return false;
    }
    if (auto saved = m_document->save(); !saved) {
        setError(tr("Cannot save: %1").arg(QString::fromStdString(saved.error().message)));
        return false;
    }
    setError({});
    m_autosaveText = tr("Saved %1").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm")));
    emit autosaveChanged();
    adoptDocument();
    return true;
}

bool ProjectLifecycle::saveAs(const QUrl& file) {
    if (!m_document) {
        return false;
    }
    waitForAutosave(); // P.1: an autosave being written lands before the files change
    std::filesystem::path path = toPath(file);
    if (path.extension() != io::kProjectExtension) {
        path += std::string(io::kProjectExtension);
    }
    if (auto saved = m_document->saveAs(path); !saved) {
        setError(tr("Cannot save %1: %2").arg(toQString(path.filename()), QString::fromStdString(saved.error().message)));
        return false;
    }
    setError({});
    m_controller->setProjectFile(m_document->file());
    m_demo = false;
    rememberProject();
    m_autosaveText = tr("Saved %1").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm")));
    emit autosaveChanged();
    adoptDocument();
    return true;
}

void ProjectLifecycle::startBackgroundAutosave() {
    if (!m_document || m_demo) {
        return;
    }
    if (m_autosaveWrite.valid()) {
        if (m_autosaveWrite.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return; // the previous one is still writing: the next tick writes the newer revision
        }
        collectAutosave();
    }
    std::optional<editor::ProjectDocument::AutosaveJob> job = m_document->prepareAutosave();
    if (!job) {
        return;
    }
    m_autosaveJob = std::move(job);
    const std::uint64_t serial = ++m_autosaveSerial;
    {
        std::lock_guard lock(m_scanGate->mutex);
        m_scanGate->owner = this;
    }
    m_autosaveWrite = std::async(std::launch::async, [job = &*m_autosaveJob, gate = m_scanGate, serial] {
        std::expected<void, Error> written = job->write();
        std::lock_guard lock(gate->mutex);
        if (ProjectLifecycle* owner = gate->owner) {
            QMetaObject::invokeMethod(
                owner,
                [owner, serial] {
                    if (serial == owner->m_autosaveSerial && owner->m_autosaveWrite.valid()) {
                        owner->collectAutosave();
                    }
                },
                Qt::QueuedConnection);
        }
        return written;
    });
}

void ProjectLifecycle::collectAutosave() {
    if (!m_autosaveWrite.valid()) {
        return;
    }
    const std::expected<void, Error> written = m_autosaveWrite.get();
    std::optional<editor::ProjectDocument::AutosaveJob> job = std::exchange(m_autosaveJob, std::nullopt);
    if (!written) {
        setError(tr("Autosave failed: %1").arg(QString::fromStdString(written.error().message)));
        return;
    }
    if (m_document && job) {
        m_document->finishAutosave(*job);
    }
    m_autosaveText = tr("Autosaved %1").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm")));
    emit autosaveChanged();
}

void ProjectLifecycle::waitForAutosave() {
    if (m_autosaveWrite.valid()) {
        m_autosaveWrite.wait();
        collectAutosave();
    }
}

bool ProjectLifecycle::autosaveNow() {
    if (!m_document || m_demo) {
        return false;
    }
    waitForAutosave();
    auto written = m_document->autosave();
    if (!written) {
        setError(tr("Autosave failed: %1").arg(QString::fromStdString(written.error().message)));
        return false;
    }
    if (*written) {
        m_autosaveText = tr("Autosaved %1").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm")));
        emit autosaveChanged();
    }
    return *written;
}

// ---- Import -----------------------------------------------------------------------------------------

QVariantMap ProjectLifecycle::importFiles(const QList<QUrl>& files, const QVariantMap& options) {
    QVariantMap summary;
    if (!m_document) {
        return summary;
    }
    // Home screen options (M5f); see the header.
    const bool freshProject = options.value(QStringLiteral("newProject")).toBool();
    const bool openImported = options.value(QStringLiteral("openImported")).toBool();
    const bool experimentalPremiere = options.value(QStringLiteral("experimentalPremiereProject")).toBool();
    QList<QUrl> media;
    QList<QUrl> timelines;
    QVariantList projects;
    m_importedMedia.clear();
    for (const QUrl& url : files) {
        switch (editor::classifyImportFile(toPath(url))) {
        case editor::ImportFileKind::Media:
            media.append(url);
            break;
        case editor::ImportFileKind::Interchange:
            timelines.append(url);
            break;
        case editor::ImportFileKind::Project:
            projects.append(url);
            break;
        }
    }
    // The empty first sequence of a project started for timelines only: the first import replaces it.
    std::optional<SequenceId> placeholder;
    if ((m_demo || freshProject) && (!media.isEmpty() || !timelines.isEmpty())) {
        startNewProject(media); // the demo is not a project: importing starts a real one
        if (freshProject && media.isEmpty() && !m_controller->snapshot()->sequences.empty()) {
            placeholder = m_controller->snapshot()->sequences.front()->id;
        }
    }
    std::optional<SequenceId> firstImported;

    QVariantList failed;
    int added = 0;
    int existing = 0;
    if (!media.isEmpty()) {
        std::vector<std::filesystem::path> paths;
        for (const QUrl& url : media) {
            paths.push_back(toPath(url));
        }
        const editor::MediaImportResult result =
            editor::importMediaFiles(m_document->session(), paths, {.projectFile = m_document->file()});
        m_importedMedia.clear();
        for (const MediaId id : result.byFile) {
            if (id) {
                m_importedMedia.push_back(id);
            }
        }
        added = static_cast<int>(result.added.size());
        existing = static_cast<int>(result.alreadyInProject.size());
        for (const editor::FailedImport& f : result.failed) {
            failed.append(QVariantMap{{QStringLiteral("name"), toQString(f.file.filename())},
                                      {QStringLiteral("error"), QString::fromStdString(f.error.message)}});
        }
    }
    int imported = 0;
    for (const QUrl& url : timelines) {
        const std::filesystem::path path = toPath(url);
        editor::TimelineImportOptions timelineOptions;
        timelineOptions.projectFile = m_document->file();
        timelineOptions.mediaSearchRoots = {path.parent_path()};
        timelineOptions.experimentalPremiereProject = experimentalPremiere;
        if (placeholder && imported == 0) {
            timelineOptions.dropSequences = {*placeholder};
        }
        auto result = editor::importTimelineFile(m_document->session(), path, timelineOptions);
        if (!result) {
            failed.append(QVariantMap{{QStringLiteral("name"), toQString(path.filename())},
                                      {QStringLiteral("error"), QString::fromStdString(result.error().message)}});
            continue;
        }
        ++imported;
        if (!firstImported && !result->sequences.empty()) {
            firstImported = result->sequences.front();
        }
        emit importReport(reportToMap(*result, path));
    }
    if (openImported) {
        int firstIndex = -1;
        if (firstImported) {
            const auto& sequences = m_controller->snapshot()->sequences;
            const auto it = std::ranges::find_if(sequences, [&](const SequencePtr& s) { return s->id == *firstImported; });
            if (it != sequences.end()) {
                firstIndex = static_cast<int>(it - sequences.begin());
                m_controller->setCurrentSequenceIndex(firstIndex);
                m_controller->setPlayhead({});
            }
        }
        summary.insert(QStringLiteral("firstSequence"), firstIndex);
    }
    if (failed.isEmpty()) {
        setError({});
    } else {
        setError(tr("%n file(s) could not be imported", "", static_cast<int>(failed.size())));
    }
    adoptDocument();
    summary.insert(QStringLiteral("added"), added);
    summary.insert(QStringLiteral("existing"), existing);
    summary.insert(QStringLiteral("failed"), failed);
    summary.insert(QStringLiteral("timelines"), imported);
    summary.insert(QStringLiteral("projects"), projects);
    return summary;
}

// ---- Home screen (M5f) ------------------------------------------------------------------------------

bool ProjectLifecycle::newProjectWith(const QVariantMap& sequence) {
    if (!m_document) {
        return false;
    }
    edit::SequenceSettings settings;
    if (const QString name = sequence.value(QStringLiteral("name")).toString().trimmed(); !name.isEmpty()) {
        settings.name = name.toStdString();
    }
    settings.width = sequence.value(QStringLiteral("width"), settings.width).toInt();
    settings.height = sequence.value(QStringLiteral("height"), settings.height).toInt();
    settings.frameRate = Rate{sequence.value(QStringLiteral("rateNum"), settings.frameRate.num).toInt(),
                              sequence.value(QStringLiteral("rateDen"), settings.frameRate.den).toInt()};
    // The same checks as edit::addSequence, before anything changes.
    if (auto check = edit::addSequence(ProjectState{}, settings); !check) {
        setError(tr("Cannot create the sequence: %1").arg(QString::fromStdString(check.error().message)));
        return false;
    }
    setError({});
    startNewProjectWith(settings);
    return true;
}

// ---- Export Timeline (0.1.1) ----------------------------------------------------------------------------------------

namespace {

QString exportFormatName(const QString& format) {
    if (format == QLatin1String("fcp7-xml"))
        return QStringLiteral("Final Cut Pro 7 XML");
    if (format == QLatin1String("fcpxml"))
        return QStringLiteral("FCPXML");
    if (format == QLatin1String("otio"))
        return QStringLiteral("OpenTimelineIO");
    if (format == QLatin1String("edl"))
        return QStringLiteral("CMX 3600 EDL");
    return format;
}

} // namespace

bool ProjectLifecycle::exportTimeline(const QString& format, int sequenceIndex, const QUrl& file) {
    if (!m_controller || m_exportingTimeline)
        return false;
    const ProjectSnapshot base = m_controller->snapshot();
    if (sequenceIndex < 0 || sequenceIndex >= static_cast<int>(base->sequences.size())) {
        setError(tr("Choose a sequence to export"));
        return false;
    }
    const QString local = file.toLocalFile();
    if (local.isEmpty()) {
        setError(tr("Choose where to save the timeline"));
        return false;
    }
    command::CommandRequest request;
    request.command = "interchange.export";
    request.params = command::Json{{"sequence", base->sequences[static_cast<std::size_t>(sequenceIndex)]->id.value},
                                   {"format", format.toStdString()},
                                   {"path", QDir::fromNativeSeparators(local).toStdString()}};
    setError({});
    m_exportingTimeline = true;
    QPointer<ProjectLifecycle> self(this);
    const QString fileName = QFileInfo(local).fileName();
    // The command runs on a worker, on a scratch session over the snapshot: the GUI thread only gets the report.
    QThreadPool::globalInstance()->start([self, base, request, fileName, format, local] {
        command::CommandResult result;
        try {
            EditSession scratch(*base);
            command::CommandEngine engine(scratch, ai::edit::editingCommands(), command::EnginePolicy{});
            result = engine.execute(request);
        } catch (const std::exception& e) {
            result.error = command::CommandError{command::ErrorCode::Internal, e.what(), {}};
        }
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, result = std::move(result), fileName, format, local] {
                if (!self)
                    return;
                self->m_exportingTimeline = false;
                QVariantMap report{{QStringLiteral("exported"), true},
                                   {QStringLiteral("ok"), result.ok()},
                                   {QStringLiteral("file"), fileName},
                                   {QStringLiteral("path"), local},
                                   {QStringLiteral("format"), exportFormatName(format)}};
                if (!result.ok()) {
                    const QString error = tr("The timeline could not be exported: %1")
                                              .arg(QString::fromStdString(result.error->message));
                    report.insert(QStringLiteral("error"), error);
                    self->setError(error);
                    emit self->timelineExported(report);
                    return;
                }
                const command::Json& data = result.data;
                const command::Json counts = data.value("report", command::Json::object());
                QVariantList entries;
                for (const command::Json& e : counts.value("entries", command::Json::array())) {
                    entries.append(QVariantMap{
                        {QStringLiteral("outcome"), QString::fromStdString(e.value("outcome", std::string()))},
                        {QStringLiteral("category"), QString::fromStdString(e.value("category", std::string()))},
                        {QStringLiteral("reason"), QString::fromStdString(e.value("reason", std::string()))},
                        {QStringLiteral("location"), QString::fromStdString(e.value("where", std::string()))},
                    });
                }
                const int sequences = static_cast<int>(data.value("sequences", command::Json::array()).size());
                report.insert(QStringLiteral("sequences"), sequences);
                report.insert(QStringLiteral("carried"), counts.value("carried", 0));
                report.insert(QStringLiteral("approximated"), counts.value("approximated", 0));
                report.insert(QStringLiteral("dropped"), counts.value("dropped", 0));
                report.insert(QStringLiteral("entries"), entries);
                //: e.g. "Final Cut Pro 7 XML · 2 sequences · C:\Projects\cut.xml"
                report.insert(QStringLiteral("summary"), tr("%1 · %n sequence(s) · %2", "", sequences)
                                                             .arg(exportFormatName(format), QDir::toNativeSeparators(local)));
                emit self->timelineExported(report);
            },
            Qt::QueuedConnection);
    });
    return true;
}

QVariantMap ProjectLifecycle::detectImport(const QUrl& file) const {
    const std::filesystem::path path = toPath(file);
    QVariantMap out{{QStringLiteral("format"), QString()},     {QStringLiteral("formatName"), QString()},
                    {QStringLiteral("supported"), true},       {QStringLiteral("experimental"), false},
                    {QStringLiteral("guidance"), QString()}};
    switch (editor::classifyImportFile(path)) {
    case editor::ImportFileKind::Media:
        out.insert(QStringLiteral("kind"), QStringLiteral("media"));
        return out;
    case editor::ImportFileKind::Project:
        out.insert(QStringLiteral("kind"), QStringLiteral("project"));
        return out;
    case editor::ImportFileKind::Interchange:
        break;
    }
    const interchange::Detection detection = interchange::ImportRouter::detect(path);
    const bool experimental = detection.format == interchange::Format::PremiereProject;
    out.insert(QStringLiteral("kind"), QStringLiteral("timeline"));
    out.insert(QStringLiteral("format"), QString::fromUtf8(interchange::toString(detection.format).data()));
    out.insert(QStringLiteral("formatName"), QString::fromUtf8(interchange::displayName(detection.format).data()));
    out.insert(QStringLiteral("supported"), detection.supported);
    out.insert(QStringLiteral("experimental"), experimental);
    out.insert(QStringLiteral("guidance"), QString::fromStdString(detection.guidance));
    return out;
}

// ---- Relink -----------------------------------------------------------------------------------------

QVariantList ProjectLifecycle::relinkCandidates(const QString& mediaId, const QList<QUrl>& folders) const {
    QVariantList list;
    const auto id = parseMediaKey(mediaId);
    if (!m_document || !id) {
        return list;
    }
    std::vector<std::filesystem::path> roots;
    for (const QUrl& url : folders) {
        roots.push_back(toPath(url));
    }
    for (const editor::RelinkCandidate& c : m_document->findRelinkCandidates(*id, roots)) {
        list.append(QVariantMap{{QStringLiteral("url"), toUrl(c.file)},
                                {QStringLiteral("path"), QDir::toNativeSeparators(toQString(c.file))},
                                {QStringLiteral("matches"), c.fingerprintMatches}});
    }
    return list;
}

bool ProjectLifecycle::relink(const QString& mediaId, const QUrl& file, bool force) {
    if (const AssetId asset = assetOfReference(mediaId.toStdString()); asset.isValid()) { // a LUT asset (I.5)
        if (!m_document) {
            return false;
        }
        if (auto relinked = m_document->relinkAsset(asset, toPath(file), force); !relinked) {
            setError(QString::fromStdString(relinked.error().message));
            return false;
        }
        setError({});
        adoptDocument();
        return true;
    }
    const auto id = parseMediaKey(mediaId);
    if (!m_document || !id) {
        return false;
    }
    if (auto relinked = m_document->relink(*id, toPath(file), force); !relinked) {
        setError(QString::fromStdString(relinked.error().message));
        return false;
    }
    setError({});
    adoptDocument();
    return true;
}

int ProjectLifecycle::relinkAll(const QList<QUrl>& folders) {
    if (!m_document) {
        return 0;
    }
    std::vector<std::filesystem::path> roots;
    for (const QUrl& url : folders) {
        roots.push_back(toPath(url));
    }
    auto relinked = m_document->relinkAll(roots);
    if (!relinked) {
        setError(QString::fromStdString(relinked.error().message));
        return 0;
    }
    setError({});
    adoptDocument();
    return static_cast<int>(relinked->size());
}

// ---- Library -> timeline ----------------------------------------------------------------------------

bool ProjectLifecycle::dropMedia(const QString& mediaId, const QString& timecode, const QString& trackId,
                                 bool overwrite) {
    const auto media = parseMediaKey(mediaId);
    // A sequence from the Project panel: nested where it is dropped (C.8).
    const auto nested = media ? std::nullopt : parseSequenceKey(mediaId);
    const SequencePtr sequence = m_controller ? m_controller->currentSequence() : nullptr;
    if (!m_document || (!media && !nested) || !sequence) {
        return false;
    }
    const auto at = parseTimecode(timecode.toStdString(), sequence->frameRate, sequence->dropFrame);
    if (!at) {
        setError(QString::fromStdString(at.error().message));
        return false;
    }
    editor::MediaDrop drop;
    drop.media = media.value_or(MediaId{});
    drop.sourceSequence = nested.value_or(SequenceId{});
    drop.sequence = sequence->id;
    drop.at = *at;
    drop.track = parseTrackKey(trackId).value_or(TrackId{});
    drop.mode = overwrite ? editor::DropMode::Overwrite : editor::DropMode::Insert;
    auto edited = editor::dropMedia(m_document->session(), drop);
    if (!edited) {
        setError(QString::fromStdString(edited.error().message));
        return false;
    }
    setError({});
    m_controller->setSelection(edited->placed, edited->placed.empty() ? ClipId{} : edited->placed.front());
    return true;
}

QVariantMap ProjectLifecycle::dropFiles(const QList<QUrl>& files, const QString& timecode, const QString& trackId,
                                        bool overwrite) {
    QVariantMap summary = importFiles(files);
    const std::vector<MediaId> media = m_importedMedia;
    const SequencePtr sequence = m_controller ? m_controller->currentSequence() : nullptr;
    int placed = 0;
    if (m_document && sequence && !media.empty()) {
        auto at = parseTimecode(timecode.toStdString(), sequence->frameRate, sequence->dropFrame);
        if (!at) {
            at = Ticks{};
        }
        // The track the files were dropped on, if the sequence (a new one, after the demo) still has it.
        TrackId track = parseTrackKey(trackId).value_or(TrackId{});
        if (track && !sequence->findTrack(track)) {
            track = TrackId{};
        }
        std::vector<ClipId> clips;
        QString error;
        for (const MediaId id : media) {
            editor::MediaDrop drop;
            drop.media = id;
            drop.sequence = sequence->id;
            drop.at = *at;
            drop.track = track;
            drop.mode = overwrite ? editor::DropMode::Overwrite : editor::DropMode::Insert;
            auto edited = editor::dropMedia(m_document->session(), drop);
            if (!edited) {
                error = QString::fromStdString(edited.error().message);
                continue;
            }
            ++placed;
            // The next file follows this one.
            const SequencePtr after = m_controller->snapshot()->sequencePtr(sequence->id);
            for (const ClipId clip : edited->placed) {
                clips.push_back(clip);
                if (!after) {
                    continue;
                }
                if (const auto found = after->findClip(clip)) {
                    *at = std::max(*at, found->clip().end());
                }
            }
        }
        if (!error.isEmpty()) {
            setError(error);
        }
        if (!clips.empty()) {
            m_controller->setSelection(clips, clips.front());
        }
    }
    summary.insert(QStringLiteral("placed"), placed);
    return summary;
}

bool ProjectLifecycle::insertModifierHeld() const {
    return QGuiApplication::queryKeyboardModifiers().testFlag(Qt::ControlModifier);
}

} // namespace wabcut::app
