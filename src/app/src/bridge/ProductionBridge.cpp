#include "ProductionBridge.h"

#include "BridgeIds.h"
#include "LibraryBridge.h"
#include "LibraryModel.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/editor/Production.h>
#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/Merge.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/Timecode.h>

#include <QCoreApplication>
#include <QDir>
#include <QQmlEngine>

#include <algorithm>
#include <functional>
#include <map>
#include <stop_token>
#include <thread>

namespace wabcut::app {

namespace fs = std::filesystem;

namespace {

QString text(const std::string& s) { return QString::fromStdString(s); }

fs::path toPath(const QUrl& url) {
    const QString local = url.isLocalFile() ? url.toLocalFile() : url.toString();
    return fs::path(local.toStdU16String());
}

QString nativeText(const fs::path& p) { return QDir::toNativeSeparators(QString::fromStdU16String(p.u16string())); }

QUrl urlOf(const fs::path& p) { return QUrl::fromLocalFile(QString::fromStdU16String(p.u16string())); }

bool samePath(const fs::path& a, const fs::path& b) {
    if (a.empty() || b.empty())
        return false;
    std::error_code ec;
    if (fs::equivalent(a, b, ec))
        return true;
    return QString::compare(nativeText(a.lexically_normal()), nativeText(b.lexically_normal()), Qt::CaseInsensitive) == 0;
}

/// "b4" / "s3" / "m7" of the browsed project.
std::optional<std::pair<char, std::uint64_t>> parseKey(const QString& key) {
    if (key.size() < 2)
        return std::nullopt;
    bool ok = false;
    const std::uint64_t value = QStringView(key).mid(1).toULongLong(&ok);
    const char kind = key.front().toLatin1();
    if (!ok || value == 0 || (kind != 'b' && kind != 's' && kind != 'm'))
        return std::nullopt;
    return std::pair{kind, value};
}

} // namespace

ProductionBridge::ProductionBridge(ProjectLifecycle* lifecycle, QObject* parent) : QObject(parent), m_lifecycle(lifecycle) {
    if (lifecycle) {
        connect(lifecycle, &ProjectLifecycle::documentChanged, this, &ProductionBridge::followDocument);
        followDocument();
    }
}

ProductionBridge::~ProductionBridge() {
    // The search worker posts to this object: it ends before the object does.
    m_searchThread.request_stop();
    if (m_searchThread.joinable())
        m_searchThread.join();
}

ProductionBridge* ProductionBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* lifecycle = qmlEngine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    auto* bridge = new ProductionBridge(lifecycle);
    if (auto* library = qmlEngine->singletonInstance<LibraryBridge*>("Wabcut", "Library")) {
        QPointer<LibraryBridge> guard(library);
        QPointer<ProjectLifecycle> owner(lifecycle);
        bridge->setShowItem([guard](const QString& key) {
            if (guard)
                guard->showItem(key);
        });
        bridge->setImportBin([guard, owner]() -> BinId {
            if (!guard)
                return {};
            const BinId bin = guard->bin();
            // An open search bin holds nothing: imports go next to it.
            const ProjectController* controller = owner ? owner->controller() : nullptr;
            if (bin.isValid() && controller) {
                if (const Bin* b = controller->snapshot()->findBin(bin); b && b->isSearch())
                    return b->parent;
            }
            return bin;
        });
    }
    return bridge;
}

void ProductionBridge::setMessage(const QString& message) {
    m_message = message;
    emit messageChanged();
}

QString ProductionBridge::folder() const { return m_folder.empty() ? QString() : nativeText(m_folder); }

QUrl ProductionBridge::folderUrl() const { return m_folder.empty() ? QUrl() : urlOf(m_folder); }

QUrl ProductionBridge::browsing() const { return m_browsingFile.empty() ? QUrl() : urlOf(m_browsingFile); }

QString ProductionBridge::browsingName() const {
    return m_browsingFile.empty() ? QString() : QString::fromStdU16String(m_browsingFile.stem().u16string());
}

// ---- Productions ------------------------------------------------------------------------------------------------

bool ProductionBridge::makeProduction(const QUrl& folder, const QString& name) {
    auto made = editor::Production::create(toPath(folder), name.trimmed().toStdString());
    if (!made) {
        setMessage(text(made.error().message));
        return false;
    }
    m_folder = made->folder();
    m_name = text(made->name());
    resetSearch();
    loadSettings();
    emit productionChanged();
    refresh();
    setMessage(tr("Production “%1” is ready.").arg(m_name));
    return true;
}

bool ProductionBridge::openProduction(const QUrl& folder) {
    auto opened = editor::Production::open(toPath(folder));
    if (!opened) {
        setMessage(text(opened.error().message));
        return false;
    }
    const bool changed = opened->folder() != m_folder;
    if (changed)
        resetSearch();
    m_folder = opened->folder();
    m_name = text(opened->name());
    const QVariantMap before = m_settings;
    loadSettings();
    if (changed || before != m_settings)
        emit productionChanged();
    refresh();
    return true;
}

void ProductionBridge::loadSettings() {
    m_settings.clear();
    if (m_folder.empty())
        return;
    auto production = editor::Production::open(m_folder);
    if (!production)
        return;
    const editor::ProductionSettings& s = production->settings();
    m_settings = {
        {QStringLiteral("previews"), text(s.previews)},
        {QStringLiteral("mediaCache"), text(s.mediaCache)},
        {QStringLiteral("previewsText"), s.previews.empty() ? tr("Next to each project") : nativeText(production->resolve(s.previews))},
        {QStringLiteral("mediaCacheText"), s.mediaCache.empty() ? tr("The app's cache folder")
                                                                : nativeText(production->resolve(s.mediaCache))},
    };
}

bool ProductionBridge::setSettings(const QVariantMap& settings) {
    auto production = editor::Production::open(m_folder);
    if (!production) {
        setMessage(tr("Open a production first."));
        return false;
    }
    editor::ProductionSettings next = production->settings();
    const auto pick = [&](const QString& name, std::string& field) {
        if (!settings.contains(name))
            return;
        const QVariant value = settings.value(name);
        QString path = value.metaType().id() == QMetaType::QUrl ? value.toUrl().toLocalFile() : value.toString();
        if (path.startsWith(QLatin1String("file:")))
            path = QUrl(path).toLocalFile();
        path = path.trimmed();
        if (path.isEmpty()) {
            field.clear();
            return;
        }
        const fs::path chosen = fs::path(QDir::fromNativeSeparators(path).toStdU16String()).lexically_normal();
        // Inside the production: relative, so the production keeps working when its folder moves.
        const fs::path relative = chosen.is_absolute() ? chosen.lexically_relative(m_folder) : chosen;
        const bool inside = !relative.empty() && *relative.begin() != fs::path("..") && !relative.is_absolute();
        field = pathToUtf8(inside ? relative : chosen);
    };
    pick(QStringLiteral("previews"), next.previews);
    pick(QStringLiteral("mediaCache"), next.mediaCache);
    if (auto saved = production->setSettings(next); !saved) {
        setMessage(text(saved.error().message));
        return false;
    }
    loadSettings();
    emit productionChanged();
    if (m_lifecycle)
        m_lifecycle->applyScratchDisks();
    setMessage(tr("Production Settings saved."));
    return true;
}

void ProductionBridge::closeProduction() {
    if (m_folder.empty())
        return;
    m_folder.clear();
    resetSearch();
    m_name.clear();
    m_entries.clear();
    m_settings.clear();
    emit productionChanged();
    emit entriesChanged();
}

void ProductionBridge::followDocument() {
    if (!m_lifecycle)
        return;
    const QString folder = m_lifecycle->productionFolder();
    if (!folder.isEmpty() && !samePath(fs::path(QDir::fromNativeSeparators(folder).toStdU16String()), m_folder))
        openProduction(QUrl::fromLocalFile(folder));
    else
        refresh(); // which project is current changed
}

void ProductionBridge::refresh() {
    QVariantList out;
    if (!m_folder.empty()) {
        if (auto production = editor::Production::open(m_folder)) {
            const fs::path current = m_lifecycle && !m_lifecycle->untitled()
                                         ? fs::path(QDir::fromNativeSeparators(m_lifecycle->filePath()).toStdU16String())
                                         : fs::path{};
            for (const editor::ProductionEntry& e : production->entries()) {
                const bool openHere = !e.folder && m_lifecycle && m_lifecycle->indexOfProject(urlOf(e.path)) >= 0;
                out.append(QVariantMap{
                    {QStringLiteral("open"), openHere},
                    {QStringLiteral("url"), urlOf(e.path)},
                    {QStringLiteral("relative"), text(e.relative)},
                    {QStringLiteral("name"), text(e.name)},
                    {QStringLiteral("folder"), e.folder},
                    {QStringLiteral("depth"), e.depth},
                    {QStringLiteral("locked"), e.lockedByOther},
                    {QStringLiteral("lockOwner"), e.lock ? text(e.lock->owner()) : QString()},
                    {QStringLiteral("current"), !e.folder && samePath(e.path, current)},
                    {QStringLiteral("browsing"), !e.folder && samePath(e.path, m_browsingFile)},
                });
            }
        }
    }
    if (out == m_entries)
        return;
    m_entries = std::move(out);
    emit entriesChanged();
}

bool ProductionBridge::rename(const QString& name) {
    auto production = editor::Production::open(m_folder);
    if (!production)
        return false;
    if (auto renamed = production->rename(name.trimmed().toStdString()); !renamed) {
        setMessage(text(renamed.error().message));
        return false;
    }
    m_name = name.trimmed();
    emit productionChanged();
    return true;
}

QUrl ProductionBridge::newProject(const QString& relative, const QString& name) {
    auto production = editor::Production::open(m_folder);
    if (!production) {
        setMessage(tr("Open a production first."));
        return {};
    }
    ProjectState initial;
    initial.ids.next = 1;
    edit::SequenceSettings settings;
    if (auto seq = edit::addSequence(initial, settings))
        initial = std::move(seq->state);
    auto made = production->newProject(relative.toStdString(), name.trimmed().toStdString(), initial);
    if (!made) {
        setMessage(text(made.error().message));
        return {};
    }
    refresh();
    return urlOf(*made);
}

bool ProductionBridge::newFolder(const QString& relative, const QString& name) {
    auto production = editor::Production::open(m_folder);
    if (!production)
        return false;
    if (auto made = production->newFolder(relative.toStdString(), name.trimmed().toStdString()); !made) {
        setMessage(text(made.error().message));
        return false;
    }
    refresh();
    return true;
}

QUrl ProductionBridge::addProject(const QUrl& project, const QString& relative) {
    auto production = editor::Production::open(m_folder);
    if (!production) {
        setMessage(tr("Open a production first."));
        return {};
    }
    auto copy = production->addProject(toPath(project), relative.toStdString());
    if (!copy) {
        setMessage(text(copy.error().message));
        return {};
    }
    refresh();
    setMessage(tr("Added “%1” to the production.").arg(QString::fromStdU16String(copy->stem().u16string())));
    return urlOf(*copy);
}

// ---- Another project ------------------------------------------------------------------------------------------------

bool ProductionBridge::browse(const QUrl& project) {
    const fs::path file = toPath(project);
    const fs::path current = m_lifecycle && !m_lifecycle->untitled()
                                 ? fs::path(QDir::fromNativeSeparators(m_lifecycle->filePath()).toStdU16String())
                                 : fs::path{};
    if (samePath(file, current)) {
        setMessage(tr("This is the project being edited."));
        return false;
    }
    auto loaded = sourceState(file);
    if (!loaded) {
        setMessage(tr("Cannot open %1: %2").arg(QString::fromStdU16String(file.filename().u16string()), text(loaded.error().message)));
        return false;
    }
    m_browsingFile = file;
    m_browsed = std::move(*loaded);
    m_revealedKey.clear();
    listBrowsed();
    emit browsingChanged();
    refresh();
    return true;
}

void ProductionBridge::stopBrowsing() {
    if (m_browsingFile.empty())
        return;
    m_browsingFile.clear();
    m_browsed.reset();
    m_browseItems.clear();
    m_revealedKey.clear();
    emit browsingChanged();
    refresh();
}

void ProductionBridge::listBrowsed() {
    m_browseItems.clear();
    if (!m_browsed)
        return;
    const ProjectState& s = *m_browsed;
    constexpr int kMaxItems = 20000;
    const std::function<void(BinId, int)> list = [&](BinId bin, int depth) {
        if (depth > 32 || m_browseItems.size() >= kMaxItems)
            return;
        std::vector<const Bin*> bins;
        for (const Bin& b : s.bins) {
            if (b.parent == bin && !b.isSearch())
                bins.push_back(&b);
        }
        std::sort(bins.begin(), bins.end(), [](const Bin* a, const Bin* b) { return a->name < b->name; });
        for (const Bin* b : bins) {
            m_browseItems.append(QVariantMap{{QStringLiteral("key"), QStringLiteral("b%1").arg(b->id.value)},
                                             {QStringLiteral("kind"), QStringLiteral("bin")},
                                             {QStringLiteral("name"), text(b->name)},
                                             {QStringLiteral("depth"), depth},
                                             {QStringLiteral("meta"), QString()}});
            list(b->id, depth + 1);
        }
        for (const SequencePtr& q : s.sequences) {
            if (q->bin != bin)
                continue;
            m_browseItems.append(QVariantMap{
                {QStringLiteral("key"), QStringLiteral("s%1").arg(q->id.value)},
                {QStringLiteral("kind"), QStringLiteral("sequence")},
                {QStringLiteral("name"), text(q->name)},
                {QStringLiteral("depth"), depth},
                {QStringLiteral("meta"), QStringLiteral("%1 · %2 × %3")
                                             .arg(text(ticksToTimecode(q->duration, q->frameRate, false)))
                                             .arg(q->width)
                                             .arg(q->height)}});
        }
        for (const MediaPtr& m : s.media) {
            if (m->bin != bin)
                continue;
            LibraryModel::Row row{LibraryModel::Row::Kind::Media, m->id.value, nullptr, nullptr, m.get()};
            m_browseItems.append(QVariantMap{
                {QStringLiteral("key"), QStringLiteral("m%1").arg(m->id.value)},
                {QStringLiteral("kind"), LibraryModel::kindOf(row)},
                {QStringLiteral("name"), text(m->name)},
                {QStringLiteral("depth"), depth},
                {QStringLiteral("meta"), m->locator.offline && !m->isGenerator() ? tr("Offline")
                                         : m->info.hasVideo()                    ? libraryResolutionText(*m)
                                                                                 : libraryAudioText(*m)}});
        }
    };
    list(BinId{}, 0);
}

fs::path ProductionBridge::currentFile() const {
    return m_lifecycle && !m_lifecycle->untitled() ? fs::path(QDir::fromNativeSeparators(m_lifecycle->filePath()).toStdU16String())
                                                   : fs::path{};
}

std::expected<ProjectState, Error> ProductionBridge::sourceState(const fs::path& project) const {
    if (m_lifecycle) {
        if (const ProjectSnapshot open = m_lifecycle->openProjectState(project))
            return editor::preparedForImport(*open, project, currentFile());
    }
    return editor::loadProjectForImport(project, currentFile());
}

QVariantMap ProductionBridge::runImport(const ProjectState& source, const fs::path& sourceFile, const QStringList& keys,
                                        bool everything, const QString& binName) {
    QVariantMap out{{QStringLiteral("ok"), false}};
    ProjectController* controller = m_lifecycle ? m_lifecycle->controller() : nullptr;
    if (!controller)
        return out;
    edit::ProjectImportSelection selection;
    selection.everything = everything;
    for (const QString& key : keys) {
        const auto parsed = parseKey(key);
        if (!parsed)
            continue;
        if (parsed->first == 'b')
            selection.bins.push_back(BinId{parsed->second});
        else if (parsed->first == 's')
            selection.sequences.push_back(SequenceId{parsed->second});
        else
            selection.media.push_back(MediaId{parsed->second});
    }
    edit::ProjectImportOptions options;
    options.into = m_importBin ? m_importBin() : BinId{};
    options.binName = binName.trimmed().toStdString();
    // Copies remember where they came from (Reveal in Source Project, relinks offered).
    std::error_code ec;
    const fs::path absolute = fs::absolute(sourceFile, ec);
    options.sourceProject = pathToUtf8((ec ? sourceFile : absolute).lexically_normal());
    edit::ProjectImportResult imported;
    const bool ok = controller->applyEditFn(QT_TRANSLATE_NOOP("wabcut::app::ProductionBridge", "Import from Project"),
                                            [&](const ProjectState& s) -> edit::EditOutcome {
                                                auto result = edit::importProjectItems(s, source, selection, options);
                                                if (!result)
                                                    return std::unexpected(result.error());
                                                imported = std::move(*result);
                                                edit::EditResult outcome;
                                                outcome.changes = diff(s, imported.state);
                                                outcome.state = imported.state;
                                                return outcome;
                                            });
    if (!ok) {
        setMessage(controller->lastError());
        return out;
    }
    QStringList notes;
    for (const std::string& n : imported.notes)
        notes << text(n);
    out = {{QStringLiteral("ok"), true},
           {QStringLiteral("sequences"), static_cast<int>(imported.sequences.size())},
           {QStringLiteral("media"), static_cast<int>(imported.media.size())},
           {QStringLiteral("bins"), static_cast<int>(imported.bins.size())},
           {QStringLiteral("reusedMedia"), static_cast<int>(imported.reusedMedia)},
           {QStringLiteral("notes"), notes}};
    setMessage(tr("Imported %n sequence(s)", nullptr, static_cast<int>(imported.sequences.size())) +
               tr(" and %n media item(s)", nullptr, static_cast<int>(imported.media.size())) +
               (imported.reusedMedia > 0 ? tr("; %n already in this project", nullptr, static_cast<int>(imported.reusedMedia))
                                         : QString()) +
               QStringLiteral("."));
    return out;
}

QVariantMap ProductionBridge::importItems(const QStringList& keys, const QString& binName) {
    if (!m_browsed) {
        setMessage(tr("Open another project first."));
        return {{QStringLiteral("ok"), false}};
    }
    // The browsed copy may be stale: read it again so the import brings what it holds now (an open project as it is in
    // memory, else what is saved).
    if (auto fresh = sourceState(m_browsingFile)) {
        m_browsed = std::move(*fresh);
        listBrowsed();
        emit browsingChanged();
    }
    return runImport(*m_browsed, m_browsingFile, keys, false, binName);
}

QVariantMap ProductionBridge::importProject(const QUrl& project) {
    const fs::path file = toPath(project);
    auto loaded = sourceState(file);
    if (!loaded) {
        setMessage(tr("Cannot open %1: %2").arg(QString::fromStdU16String(file.filename().u16string()), text(loaded.error().message)));
        return {{QStringLiteral("ok"), false}};
    }
    return runImport(*loaded, file, {}, true, QString::fromStdU16String(file.stem().u16string()));
}

// ---- Search across the production (0.7, card 094) -------------------------------------------------------------------

namespace {

/// What a user calls the field a word was found in.
QString fieldWord(const std::string& field) {
    const auto word = [](const char* source) { return QCoreApplication::translate("wabcut::app::ProductionBridge", source); };
    if (field == "name")
        return word("name");
    if (field == "comment")
        return word("comment");
    if (field == "description")
        return word("description");
    if (field == "scene")
        return word("scene");
    if (field == "shot")
        return word("shot");
    if (field == "take")
        return word("take");
    if (field == "logNote")
        return word("log note");
    if (field == "client")
        return word("client");
    if (field == "tapeName")
        return word("tape name");
    if (field == "status")
        return word("status");
    if (field == "cameraAngle")
        return word("camera angle");
    if (field == "cameraLabel")
        return word("camera label");
    if (field == "cameraModel")
        return word("camera model");
    if (field == "shotDate")
        return word("shot date");
    if (field == "shotLocation")
        return word("shot location");
    if (field == "path")
        return word("file path");
    if (field == "marker")
        return word("marker");
    if (field == "bin")
        return word("bin");
    return text(field);
}

/// "0:01:30" for a length in ticks.
QString lengthText(Ticks length) {
    const std::int64_t seconds = std::max<std::int64_t>(0, length.value / kTicksPerSecond);
    return QStringLiteral("%1:%2:%3")
        .arg(seconds / 3600)
        .arg((seconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

/// A project's index with the projects of `older` it lacks (a stopped scan read only the first ones).
std::shared_ptr<const editor::ProductionIndex> withEarlierProjects(const std::shared_ptr<const editor::ProductionIndex>& fresh,
                                                                   const std::shared_ptr<const editor::ProductionIndex>& older) {
    if (!older || !fresh || older->folder != fresh->folder)
        return fresh;
    auto out = std::make_shared<editor::ProductionIndex>(*fresh);
    for (const editor::ProjectIndex& project : older->projects) {
        const bool known = std::any_of(out->projects.begin(), out->projects.end(),
                                       [&](const editor::ProjectIndex& p) { return p.file == project.file; });
        if (!known)
            out->projects.push_back(project);
    }
    return out;
}

} // namespace

QVariantMap ProductionBridge::resultRow(const editor::SearchHit& hit) const {
    QString meta;
    if (hit.sequence) {
        meta = QStringLiteral("%1 · %2 × %3")
                   .arg(text(ticksToTimecode(hit.duration, hit.frameRate.isValid() ? hit.frameRate : kRate30, false)))
                   .arg(hit.width)
                   .arg(hit.height);
    } else if (hit.offline) {
        meta = tr("Offline");
    } else if (hit.kind == QLatin1String("video")) {
        meta = QStringLiteral("%1 × %2 · %3").arg(hit.width).arg(hit.height).arg(lengthText(hit.duration));
    } else if (hit.kind == QLatin1String("still")) {
        meta = QStringLiteral("%1 × %2").arg(hit.width).arg(hit.height);
    } else if (hit.kind == QLatin1String("audio") || hit.kind == QLatin1String("subclip")) {
        meta = lengthText(hit.duration);
    }
    QStringList matched;
    for (const std::string& field : hit.matched)
        matched << fieldWord(field);
    return {{QStringLiteral("key"), QStringLiteral("%1%2").arg(hit.sequence ? QLatin1Char('s') : QLatin1Char('m')).arg(hit.id)},
            {QStringLiteral("kind"), text(hit.kind)},
            {QStringLiteral("name"), text(hit.name)},
            {QStringLiteral("location"), text(hit.location)},
            {QStringLiteral("meta"), meta},
            {QStringLiteral("matched"), matched},
            {QStringLiteral("project"), urlOf(hit.project)},
            {QStringLiteral("projectName"), text(hit.projectName)},
            {QStringLiteral("projectRelative"), text(hit.projectRelative)},
            {QStringLiteral("projectOpen"), hit.projectOpen},
            {QStringLiteral("current"), samePath(hit.project, currentFile())},
            {QStringLiteral("offline"), hit.offline}};
}

bool ProductionBridge::search(const QString& words, const QString& field, const QString& type, const QString& project) {
    if (m_folder.empty()) {
        setMessage(tr("Open a production first."));
        return false;
    }
    editor::SearchQuery query;
    query.text = words.trimmed().toStdString();
    query.field = field.trimmed().toStdString();
    query.type = type.trimmed().toStdString();
    query.project = project.toStdString();
    if (const auto problem = editor::searchProblem(query)) {
        setMessage(tr("This search cannot run: %1").arg(text(*problem)));
        return false;
    }
    if (query.text.empty() && query.type.empty()) {
        clearSearch();
        return true;
    }
    m_pendingSearch = std::move(query);
    if (m_scanRunning) {
        // One worker at a time: the running one stops at its next project (what it read is kept), then this one starts.
        m_searchThread.request_stop();
        return true;
    }
    startPendingSearch();
    return true;
}

void ProductionBridge::startPendingSearch() {
    if (!m_pendingSearch)
        return;
    const editor::SearchQuery query = std::move(*m_pendingSearch);
    m_pendingSearch.reset();
    const std::uint64_t generation = ++m_searchGeneration;
    // The projects open in this session, as they are in memory: read here, the worker must not touch the lifecycle.
    auto open = std::make_shared<std::vector<std::pair<fs::path, ProjectSnapshot>>>();
    if (m_lifecycle) {
        for (const QVariant& v : m_lifecycle->openProjects()) {
            const QString path = v.toMap().value(QStringLiteral("path")).toString();
            if (path.isEmpty())
                continue;
            const fs::path file(QDir::fromNativeSeparators(path).toStdU16String());
            if (ProjectSnapshot state = m_lifecycle->openProjectState(file))
                open->emplace_back(file, std::move(state));
        }
    }
    m_scanRunning = true;
    m_searching = true;
    m_searchProgress = -1;
    m_searchStatus = tr("Reading the projects");
    emit searchChanged();

    const bool direct = m_synchronous;
    auto work = [this, generation, direct, folder = m_folder, query, open, previous = m_searchIndex](std::stop_token stop) {
        const auto post = [this, direct](auto fn) {
            QMetaObject::invokeMethod(this, std::move(fn), direct ? Qt::DirectConnection : Qt::QueuedConnection);
        };
        auto production = editor::Production::open(folder);
        if (!production) {
            editor::SearchResult failed;
            failed.unreadable.push_back(production.error().message);
            post([this, generation, failed] { onSearchDone(generation, nullptr, failed, false); });
            return;
        }
        editor::ScanOptions scan;
        scan.previous = previous.get();
        scan.openState = [&open](const fs::path& file) -> std::shared_ptr<const ProjectState> {
            for (const auto& [path, state] : *open) {
                if (samePath(file, path))
                    return state;
            }
            return nullptr;
        };
        scan.progress = [&](int done, int total, const std::string& current) {
            const QString name = text(current);
            post([this, generation, done, total, name] { onSearchProgress(generation, done, total, name); });
        };
        auto index = std::make_shared<const editor::ProductionIndex>(editor::scanProduction(*production, scan, stop));
        const bool stopped = stop.stop_requested();
        editor::SearchResult result;
        if (!stopped)
            result = editor::searchProduction(*index, query);
        post([this, generation, index, result = std::move(result), stopped] {
            onSearchDone(generation, index, result, stopped);
        });
    };
    if (direct) {
        work(std::stop_token{});
        return;
    }
    if (m_searchThread.joinable())
        m_searchThread.join(); // an earlier worker has posted its last message already
    m_searchThread = std::jthread(std::move(work));
}

void ProductionBridge::onSearchProgress(std::uint64_t generation, int done, int total, const QString& current) {
    if (generation != m_searchGeneration || !m_searching)
        return;
    m_searchProgress = total > 0 ? std::clamp(static_cast<double>(done) / total, 0.0, 1.0) : -1.0;
    m_searchStatus = current.isEmpty() ? tr("Searching") : tr("Reading %1 (%2 of %3)").arg(current).arg(done + 1).arg(total);
    emit searchChanged();
}

void ProductionBridge::onSearchDone(std::uint64_t generation, std::shared_ptr<const editor::ProductionIndex> index,
                                    editor::SearchResult result, bool stopped) {
    m_scanRunning = false;
    if (index && stopped)
        m_searchIndex = withEarlierProjects(index, m_searchIndex); // what was read stays for the next search
    else if (index)
        m_searchIndex = std::move(index);
    if (m_pendingSearch) {
        startPendingSearch(); // a newer query was asked while this ran: its answer is the one to show
        return;
    }
    m_searching = false;
    m_searchProgress = -1;
    m_searchStatus.clear();
    if (generation != m_searchGeneration || stopped) {
        emit searchChanged(); // cancelled or cleared: the earlier results stay (or are gone, when cleared)
        return;
    }
    m_hits = std::move(result.hits);
    m_searchResults.clear();
    for (const editor::SearchHit& hit : m_hits)
        m_searchResults.append(resultRow(hit));
    m_searched = true;
    const int total = static_cast<int>(result.total);
    QString summary;
    if (!result.unreadable.empty() && !m_searchIndex) {
        summary = text(result.unreadable.front());
    } else if (total == 0) {
        summary = tr("No results in %1").arg(tr("%n project(s)", "", result.projects));
    } else {
        summary = tr("%n result(s) in %1", "", total).arg(tr("%n project(s)", "", result.projects));
        if (static_cast<std::size_t>(total) > m_hits.size())
            summary += QStringLiteral(" · ") + tr("the first %1 are shown").arg(m_hits.size());
    }
    if (!result.unreadable.empty() && m_searchIndex)
        summary += QStringLiteral(" · ") + tr("%n project(s) could not be read", "", static_cast<int>(result.unreadable.size()));
    m_searchSummary = summary;
    emit searchChanged();
}

void ProductionBridge::cancelSearch() {
    m_pendingSearch.reset();
    if (m_scanRunning)
        m_searchThread.request_stop();
}

void ProductionBridge::clearSearch() {
    m_pendingSearch.reset();
    ++m_searchGeneration; // a worker still running has its answer dropped
    if (m_scanRunning)
        m_searchThread.request_stop();
    m_hits.clear();
    m_searchResults.clear();
    m_searchSummary.clear();
    m_searched = false;
    m_searching = m_scanRunning;
    if (!m_scanRunning) {
        m_searchProgress = -1;
        m_searchStatus.clear();
    }
    emit searchChanged();
}

void ProductionBridge::resetSearch() {
    m_searchIndex.reset();
    clearSearch();
}

bool ProductionBridge::browseResult(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= m_hits.size())
        return false;
    const editor::SearchHit& hit = m_hits[static_cast<std::size_t>(index)];
    const QString key = QStringLiteral("%1%2").arg(hit.sequence ? QLatin1Char('s') : QLatin1Char('m')).arg(hit.id);
    if (samePath(hit.project, currentFile())) {
        if (m_showItem)
            m_showItem(key); // the project being edited: its own Library shows it
        return true;
    }
    if (!browse(urlOf(hit.project)))
        return false;
    m_revealedKey = key; // browse() cleared it; the panel scrolls to the item and picks it
    emit browsingChanged();
    return true;
}

QVariantMap ProductionBridge::importResult(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= m_hits.size())
        return {{QStringLiteral("ok"), false}};
    const editor::SearchHit& hit = m_hits[static_cast<std::size_t>(index)];
    if (samePath(hit.project, currentFile())) {
        setMessage(tr("This item is already in the project being edited."));
        return {{QStringLiteral("ok"), false}};
    }
    auto loaded = sourceState(hit.project);
    if (!loaded) {
        setMessage(tr("Cannot open %1: %2").arg(QString::fromStdU16String(hit.project.filename().u16string()), text(loaded.error().message)));
        return {{QStringLiteral("ok"), false}};
    }
    const QString key = QStringLiteral("%1%2").arg(hit.sequence ? QLatin1Char('s') : QLatin1Char('m')).arg(hit.id);
    return runImport(*loaded, hit.project, {key}, false, {});
}

QUrl ProductionBridge::resultProject(int index) const {
    if (index < 0 || static_cast<std::size_t>(index) >= m_hits.size())
        return {};
    return urlOf(m_hits[static_cast<std::size_t>(index)].project);
}

// ---- Where imported items came from ----------------------------------------------------------------------------------

QVariantMap ProductionBridge::sourceOf(const QString& key) const {
    const ProjectController* controller = m_lifecycle ? m_lifecycle->controller() : nullptr;
    const ProjectSnapshot snap = controller ? controller->snapshot() : nullptr;
    if (!snap)
        return {};
    const ImportSource* source = nullptr;
    QChar kind;
    if (const auto media = detail::parseId<MediaId>(QLatin1Char('m'), key)) {
        if (const MediaItem* m = snap->findMedia(*media); m && m->importSource)
            source = &*m->importSource;
        kind = QLatin1Char('m');
    } else if (const auto sequence = parseSequenceKey(key)) {
        if (const Sequence* q = snap->findSequence(*sequence); q && q->importSource)
            source = &*q->importSource;
        kind = QLatin1Char('s');
    }
    if (!source || source->project.empty() || source->id == 0)
        return {};
    const fs::path project = pathFromUtf8(source->project);
    std::error_code ec;
    return {{QStringLiteral("project"), urlOf(project)},
            {QStringLiteral("projectName"), QString::fromStdU16String(project.stem().u16string())},
            {QStringLiteral("key"), QStringLiteral("%1%2").arg(kind).arg(source->id)},
            {QStringLiteral("open"), m_lifecycle && m_lifecycle->indexOfProject(urlOf(project)) >= 0},
            {QStringLiteral("exists"), fs::is_regular_file(project, ec)}};
}

QVariantMap ProductionBridge::revealSource(const QString& key) {
    QVariantMap out{{QStringLiteral("ok"), false}, {QStringLiteral("switched"), false}};
    const QVariantMap source = sourceOf(key);
    if (source.isEmpty()) {
        setMessage(tr("This item was not imported from another project."));
        return out;
    }
    const QUrl project = source.value(QStringLiteral("project")).toUrl();
    const QString itemKey = source.value(QStringLiteral("key")).toString();
    out.insert(QStringLiteral("project"), project);
    out.insert(QStringLiteral("key"), itemKey);
    if (source.value(QStringLiteral("open")).toBool()) {
        // Open in this session: that project comes into the panels, the item shown in its bin.
        if (!m_lifecycle->openAlongside(project)) {
            setMessage(m_lifecycle->lastError());
            return out;
        }
        if (m_showItem)
            m_showItem(itemKey);
        out.insert(QStringLiteral("ok"), true);
        out.insert(QStringLiteral("switched"), true);
        return out;
    }
    if (!source.value(QStringLiteral("exists")).toBool()) {
        setMessage(tr("“%1” is not where it was: %2").arg(source.value(QStringLiteral("projectName")).toString(),
                                                           QDir::toNativeSeparators(project.toLocalFile())));
        return out;
    }
    if (!browse(project))
        return out;
    const bool there = std::ranges::any_of(m_browseItems, [&](const QVariant& v) {
        return v.toMap().value(QStringLiteral("key")).toString() == itemKey;
    });
    if (!there) {
        setMessage(tr("The item is no longer in “%1”.").arg(browsingName()));
        out.insert(QStringLiteral("ok"), true);
        return out;
    }
    m_revealedKey = itemKey;
    emit browsingChanged();
    out.insert(QStringLiteral("ok"), true);
    return out;
}

QVariantList ProductionBridge::relinkOffers() {
    QVariantList out;
    const ProjectController* controller = m_lifecycle ? m_lifecycle->controller() : nullptr;
    const ProjectSnapshot snap = controller ? controller->snapshot() : nullptr;
    if (!snap)
        return out;
    std::map<std::string, std::optional<ProjectState>> sources; // each source project read once
    for (const MediaPtr& m : snap->media) {
        if (!m->importSource || m->isGenerator() || m->importSource->project.empty())
            continue;
        auto [at, inserted] = sources.try_emplace(m->importSource->project);
        if (inserted) {
            auto state = sourceState(pathFromUtf8(m->importSource->project));
            if (state)
                at->second = std::move(*state);
        }
        if (!at->second)
            continue;
        const MediaItem* origin = at->second->findMedia(MediaId{m->importSource->id});
        if (!origin || origin->isGenerator() || origin->locator.offline || origin->locator.path.empty())
            continue;
        const fs::path to = pathFromUtf8(origin->locator.path);
        const fs::path from = pathFromUtf8(m->locator.path);
        std::error_code ec;
        if ((!m->locator.offline && samePath(from, to)) || !fs::is_regular_file(to, ec))
            continue;
        out.append(QVariantMap{{QStringLiteral("key"), QStringLiteral("m%1").arg(m->id.value)},
                               {QStringLiteral("name"), text(m->name)},
                               {QStringLiteral("from"), nativeText(from)},
                               {QStringLiteral("to"), nativeText(to)},
                               {QStringLiteral("project"), QString::fromStdU16String(pathFromUtf8(m->importSource->project).stem().u16string())},
                               {QStringLiteral("offline"), m->locator.offline}});
    }
    return out;
}

int ProductionBridge::applyRelinkOffers(const QStringList& keys) {
    ProjectController* controller = m_lifecycle ? m_lifecycle->controller() : nullptr;
    if (!controller)
        return 0;
    const QVariantList offers = relinkOffers();
    int relinked = 0;
    // One undo step for all of them.
    controller->beginGesture(QString::fromLatin1(QT_TRANSLATE_NOOP("wabcut::app::ProductionBridge", "Relink to Source")));
    for (const QVariant& v : offers) {
        const QVariantMap offer = v.toMap();
        const QString key = offer.value(QStringLiteral("key")).toString();
        if (!keys.isEmpty() && !keys.contains(key))
            continue;
        // The source project relinked it: the same file, whatever its fingerprint says now.
        if (m_lifecycle->relink(key, QUrl::fromLocalFile(QDir::fromNativeSeparators(offer.value(QStringLiteral("to")).toString())), true))
            ++relinked;
    }
    controller->endGesture();
    if (relinked > 0)
        setMessage(tr("Relinked %n item(s) to their source project's files.", nullptr, relinked));
    return relinked;
}

} // namespace wabcut::app
