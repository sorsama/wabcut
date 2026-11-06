#include "LibraryBridge.h"

#include "BridgeIds.h"
#include "CommandJob.h"
#include "EditingBridge.h"
#include "ExportController.h"
#include "MediaBridge.h"
#include "MediaReadinessBridge.h"
#include "NestedPictures.h"
#include "PosterFrames.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "SearchBridge.h"
#include "SequenceTabsBridge.h"
#include "SourceMonitorBridge.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/command/ImageSequenceServices.h>
#include <wabcut/editor/CameraCards.h>
#include <wabcut/editor/MediaFormats.h>
#include <wabcut/editor/MediaImport.h>
#include <wabcut/interchange/Xmp.h>
#include <wabcut/media/CodecSupport.h>
#include <wabcut/media/ContainerTags.h>
#include <wabcut/model/LayeredFile.h>
#include <wabcut/model/RedactionMedia.h>
#include <wabcut/ai/search/FootageSearch.h>
#include <wabcut/ai/search/SearchIndex.h>
#include <wabcut/media/Fingerprint.h>
#include <wabcut/model/Auditions.h>
#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditingOps.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/SequenceSettings.h>
#include <wabcut/model/Timecode.h>

#include <QClipboard>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QPointer>
#include <QProcess>
#include <QRectF>
#include <QQmlEngine>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QThread>
#include <QUrl>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace wabcut::app {

namespace {

QString text(const std::string& s) { return QString::fromStdString(s); }

/// The display rate of an item's timecode (LibraryModel uses the same).
Rate rateOf(const MediaItem& m) {
    if (m.info.hasVideo() && !m.info.isStillImage && m.info.video.front().frameRate.isValid())
        return m.info.video.front().frameRate;
    return kRate30;
}

/// The part of an item a fraction runs over: a subclip's range, else the whole media.
TimeRange spanOf(const MediaItem& m) {
    if (m.subclip)
        return m.subclip->range;
    return {Ticks{}, std::max(m.info.duration, Ticks{})};
}

/// Tests replace how a file is revealed; empty = the platform's file manager.
LibraryBridge::Revealer& revealOverride() {
    static LibraryBridge::Revealer instance;
    return instance;
}

bool revealFile(const QString& path) {
    if (revealOverride())
        return revealOverride()(path);
    const QString native = QDir::toNativeSeparators(path);
#if defined(Q_OS_WIN)
    return QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,") + native});
#elif defined(Q_OS_MACOS)
    return QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), native});
#else
    return QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(native).absolutePath()));
#endif
}

} // namespace

void LibraryBridge::setRevealer(Revealer revealer) { revealOverride() = std::move(revealer); }

bool LibraryBridge::revealPath(const QString& path) { return revealFile(path); }

LibraryBridge::AnalysisProbe defaultAnalysisProbe(std::function<bool(std::uint64_t)> indexed) {
    return [indexed = std::move(indexed)](const MediaItem& item, const std::filesystem::path& file) {
        LibraryMediaStatus status;
        if (file.empty())
            return status;
        const auto fingerprint = media::sourceFingerprint(pathToUtf8(file));
        if (!fingerprint)
            return status;
        const ai::AnalysisCache cache(ai::AnalysisCache::defaultRoot());
        // Keys use the container stream index (TranscriptionService, MediaAnalyzer); accept the stream's position too.
        for (std::size_t i = 0; i < item.info.audio.size() && !status.transcribed; ++i) {
            status.transcribed = cache.newest(*fingerprint, item.info.audio[i].index, "transcript").has_value() ||
                                 cache.newest(*fingerprint, static_cast<int>(i), "transcript").has_value();
        }
        for (std::size_t i = 0; i < item.info.video.size() && !status.shots; ++i)
            status.shots = cache.newest(*fingerprint, item.info.video[i].index, "shots").has_value();
        status.indexed = indexed && indexed(*fingerprint);
        return status;
    };
}

// ---- Construction ------------------------------------------------------------------------------------------------

LibraryBridge::LibraryBridge(QObject* parent)
    : QObject(parent), m_model(new LibraryModel(this)),
      m_scanGeneration(std::make_shared<std::atomic<std::uint64_t>>(0)) {
    for (const LibraryColumnInfo& info : libraryColumns())
        m_columns.push_back({info.column, info.defaultVisible, info.defaultWidth});
    // P1 round (#45): the footer's line follows what is listed and selected.
    connect(this, &LibraryBridge::selectionChanged, this, &LibraryBridge::summaryChanged);
    connect(this, &LibraryBridge::statsChanged, this, &LibraryBridge::summaryChanged);
}

LibraryBridge::~LibraryBridge() {
    m_scanGeneration->fetch_add(1);
    if (m_scan.joinable()) {
        m_scan.request_stop();
        m_scan.join();
    }
}

LibraryBridge* LibraryBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* library = new LibraryBridge;
    if (qmlEngine) {
        library->loadSettings();
        if (auto* smart = qmlEngine->singletonInstance<SearchBridge*>("Wabcut", "SmartSearch")) {
            auto opener = smart->searchOpener();
            library->setAnalysisProbe(defaultAnalysisProbe([opener](std::uint64_t fingerprint) {
                const auto search = opener ? opener() : nullptr;
                if (!search || !search->searchIndex())
                    return false;
                const auto source = search->searchIndex()->source(fingerprint);
                return source && (source->keyframes > 0 || source->phrases > 0);
            }));
            connect(smart, &SearchBridge::indexChanged, library, [library] {
                if (!library->m_scanning.load())
                    library->startScan(library->m_controller ? library->m_controller->snapshot() : nullptr);
            });
        } else {
            library->setAnalysisProbe(defaultAnalysisProbe({}));
        }
        library->setReadiness(qmlEngine->singletonInstance<MediaReadinessBridge*>("Wabcut", "MediaReadiness"));
        library->setSourceMonitor(qmlEngine->singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor"));
        library->setExport(qmlEngine->singletonInstance<ExportController*>("Wabcut", "ExportController"));
        library->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        // P1 round (#40): Find in Timeline opens the sequence in its tab.
        QPointer<QQmlEngine> tabsEngine(qmlEngine);
        library->setSequenceOpener([tabsEngine](const QString& sequence) {
            auto* tabs = tabsEngine ? tabsEngine->singletonInstance<SequenceTabsBridge*>("Wabcut", "SequenceTabs") : nullptr;
            return tabs && tabs->open(sequence);
        });
        // XMP written into a file (0.2, P.3): the item is relinked to it (its fingerprint changed).
        QPointer<QQmlEngine> engine(qmlEngine);
        library->setRelinker([engine](const QString& key, const std::filesystem::path& file) {
            auto* project = engine ? engine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile") : nullptr;
            return project && project->relink(key, QUrl::fromLocalFile(QString::fromStdU16String(file.u16string())), true);
        });
    }
    return library;
}

void LibraryBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    m_bin = {};
    m_tabs.clear();
    m_selection.clear();
    if (controller) {
        // Media, bins and sequences all reach the model through a new snapshot; only refresh when the panel's
        // inputs changed (pointer comparison of the shared nodes).
        connect(controller, &ProjectController::revisionChanged, this, [this] { refresh(); });
        // ProjectController re-resolves the media files (thumbnails) after the revision signal: list again.
        connect(controller, &ProjectController::mediaChanged, this, [this] { refresh(true); });
        connect(controller, &ProjectController::changed, this, [this](const QVariantMap& change) {
            if (change.value(QStringLiteral("kind")).toString() == QStringLiteral("reset")) {
                m_bin = {};
                m_tabs.clear();
                m_selection.clear();
                m_expansionLoaded = false; // P1 round (#46): the new project's twirl-down bins
                emit binChanged();
                emit tabsChanged();
                emit selectionChanged();
            }
        });
    }
    refresh(true);
}

void LibraryBridge::setReadiness(MediaReadinessBridge* readiness) {
    if (m_readiness == readiness)
        return;
    if (m_readiness)
        disconnect(m_readiness, nullptr, this, nullptr);
    m_readiness = readiness;
    if (readiness)
        connect(readiness, &MediaReadinessBridge::proxiesChanged, this, &LibraryBridge::refreshStatus);
    refreshStatus();
}

void LibraryBridge::setSourceMonitor(SourceMonitorBridge* source) {
    if (m_source == source)
        return;
    if (m_source)
        disconnect(m_source, nullptr, this, nullptr);
    m_source = source;
    if (source) {
        connect(source, &SourceMonitorBridge::sourceChanged, this, &LibraryBridge::subclipSourceChanged);
        connect(source, &SourceMonitorBridge::marksChanged, this, &LibraryBridge::subclipSourceChanged);
        // 0.2: a Media Browser clip shown in the Source Monitor imports (with the Ingest settings) on its first use.
        source->setImporter([self = QPointer<LibraryBridge>(this)](const QString& file) -> std::optional<MediaId> {
            if (!self)
                return std::nullopt;
            const auto imported = self->ingestClips({file});
            if (!imported || imported->empty() || !imported->front().contains("media"))
                return std::nullopt;
            return MediaId{imported->front()["media"].get<std::uint64_t>()};
        });
    }
    emit subclipSourceChanged();
}

void LibraryBridge::setExport(ExportController* exporter) {
    if (m_export == exporter)
        return;
    if (m_export)
        disconnect(m_export, nullptr, this, nullptr);
    m_export = exporter;
    if (exporter)
        connect(exporter, &ExportController::jobFinished, this, &LibraryBridge::transcodeFinished);
}

void LibraryBridge::setAnalysisProbe(AnalysisProbe probe) {
    m_probe = std::move(probe);
    m_analysis.clear();
    startScan(m_controller ? m_controller->snapshot() : nullptr);
}

void LibraryBridge::setMessage(const QString& message) {
    m_message = message;
    emit messageChanged();
}

// ---- Settings -----------------------------------------------------------------------------------------------------

QStringList LibraryBridge::encodeColumns(const std::vector<ColumnState>& columns) {
    QStringList out;
    for (const ColumnState& c : columns) {
        out << QStringLiteral("%1:%2:%3")
                   .arg(QLatin1String(libraryColumns()[static_cast<std::size_t>(c.column)].id))
                   .arg(c.width)
                   .arg(c.visible ? 1 : 0);
    }
    return out;
}

std::optional<std::vector<LibraryBridge::ColumnState>> LibraryBridge::decodeColumns(const QStringList& saved) {
    if (saved.isEmpty())
        return std::nullopt;
    std::vector<ColumnState> columns;
    for (const QString& entry : saved) {
        const QStringList parts = entry.split(QLatin1Char(':'));
        const auto column = parts.isEmpty() ? std::nullopt : libraryColumnFromId(parts.front());
        if (!column || std::any_of(columns.begin(), columns.end(), [&](const ColumnState& c) { return c.column == *column; }))
            continue;
        ColumnState state{*column, true, libraryColumns()[static_cast<std::size_t>(*column)].defaultWidth};
        if (parts.size() > 1)
            state.width = std::clamp(parts[1].toInt(), 40, 800);
        if (parts.size() > 2)
            state.visible = parts[2] != QStringLiteral("0");
        columns.push_back(state);
    }
    // Columns this build added since: at the end, as their defaults say.
    for (const LibraryColumnInfo& info : libraryColumns()) {
        if (std::none_of(columns.begin(), columns.end(), [&](const ColumnState& c) { return c.column == info.column; }))
            columns.push_back({info.column, info.defaultVisible, info.defaultWidth});
    }
    // The name always shows, first or not.
    for (ColumnState& c : columns) {
        if (c.column == LibraryColumn::Name)
            c.visible = true;
    }
    return columns;
}

void LibraryBridge::loadSettings() {
    QSettings settings;
    settings.beginGroup(QStringLiteral("library"));
    const QString savedView = settings.value(QStringLiteral("view"), QStringLiteral("grid")).toString();
    m_view = savedView == QStringLiteral("list") || savedView == QStringLiteral("freeform") ? savedView : QStringLiteral("grid");
    m_freeformLines = settings.value(QStringLiteral("freeformLines"), QStringList{QStringLiteral("duration"), QString()})
                          .toStringList()
                          .mid(0, 2);
    m_hoverScrub = settings.value(QStringLiteral("hoverScrub"), true).toBool();
    m_listThumbnails = settings.value(QStringLiteral("listThumbnails"), false).toBool();
    m_embedXmp = QSettings().value(QStringLiteral("metadata/embedXmp"), false).toBool();
    m_thumbnailScale = std::clamp(settings.value(QStringLiteral("thumbnailScale"), 1.0).toDouble(), 0.5, 2.0);
    m_ingest = settings.value(QStringLiteral("ingest"), false).toBool();
    if (const QString action = settings.value(QStringLiteral("ingestAction")).toString();
        action == QLatin1String("copy") || action == QLatin1String("transcode") || action == QLatin1String("proxies") ||
        action == QLatin1String("copyProxies"))
        m_ingestAction = action;
    m_ingestFolder = settings.value(QStringLiteral("ingestFolder")).toString();
    if (const QString preset = settings.value(QStringLiteral("ingestPreset")).toString(); !preset.isEmpty())
        m_ingestPreset = preset;
    if (auto columns = decodeColumns(settings.value(QStringLiteral("columns")).toStringList()))
        m_columns = std::move(*columns);
    // 0.7 (card 144): the named column layouts.
    const int presetCount = settings.beginReadArray(QStringLiteral("columnPresets"));
    for (int i = 0; i < presetCount && static_cast<int>(m_columnPresets.size()) < kMaxColumnPresets; ++i) {
        settings.setArrayIndex(i);
        const QString name = settings.value(QStringLiteral("name")).toString().trimmed();
        auto columns = decodeColumns(settings.value(QStringLiteral("columns")).toStringList());
        if (!name.isEmpty() && columns)
            m_columnPresets.push_back({name, std::move(*columns)});
    }
    settings.endArray();
    if (const auto sort = libraryColumnFromId(settings.value(QStringLiteral("sortColumn")).toString()))
        m_model->setSort(*sort, settings.value(QStringLiteral("sortAscending"), true).toBool());
    settings.endGroup();
    applyColumns();
}

void LibraryBridge::saveSettings() const {
    if (!m_persistent)
        return;
    QSettings settings;
    settings.beginGroup(QStringLiteral("library"));
    settings.setValue(QStringLiteral("view"), m_view);
    settings.setValue(QStringLiteral("hoverScrub"), m_hoverScrub);
    settings.setValue(QStringLiteral("listThumbnails"), m_listThumbnails);
    settings.setValue(QStringLiteral("thumbnailScale"), m_thumbnailScale);
    settings.setValue(QStringLiteral("freeformLines"), m_freeformLines);
    settings.setValue(QStringLiteral("ingest"), m_ingest);
    settings.setValue(QStringLiteral("ingestAction"), m_ingestAction);
    settings.setValue(QStringLiteral("ingestFolder"), m_ingestFolder);
    settings.setValue(QStringLiteral("ingestPreset"), m_ingestPreset);
    settings.setValue(QStringLiteral("columns"), encodeColumns(m_columns));
    settings.remove(QStringLiteral("columnPresets"));
    settings.beginWriteArray(QStringLiteral("columnPresets"), static_cast<int>(m_columnPresets.size()));
    for (std::size_t i = 0; i < m_columnPresets.size(); ++i) {
        settings.setArrayIndex(static_cast<int>(i));
        settings.setValue(QStringLiteral("name"), m_columnPresets[i].first);
        settings.setValue(QStringLiteral("columns"), encodeColumns(m_columnPresets[i].second));
    }
    settings.endArray();
    settings.setValue(QStringLiteral("sortColumn"), sortColumn());
    settings.setValue(QStringLiteral("sortAscending"), m_model->sortAscending());
    settings.endGroup();
}

// ---- View and columns ----------------------------------------------------------------------------------------------

void LibraryBridge::setView(const QString& view) {
    const QString next = view == QStringLiteral("list") || view == QStringLiteral("freeform") ? view : QStringLiteral("grid");
    if (next == m_view)
        return;
    const bool treeChanged = (next == QStringLiteral("list")) != (m_view == QStringLiteral("list"));
    m_view = next;
    saveSettings();
    emit viewChanged();
    if (treeChanged)
        refresh(true); // P1 round (#46): the list shows the twirl-down tree, the grid the bin alone
    else
        refreshFreeform();
}

void LibraryBridge::setScope(const QString& scope) {
    const QString next = scope == QStringLiteral("audio") ? QStringLiteral("audio") : QStringLiteral("all");
    if (next == m_scope)
        return;
    m_scope = next;
    emit scopeChanged();
    refresh(true);
}

void LibraryBridge::setHoverScrub(bool on) {
    if (on == m_hoverScrub)
        return;
    m_hoverScrub = on;
    saveSettings();
    emit hoverScrubChanged();
}

void LibraryBridge::setListThumbnails(bool on) {
    if (on == m_listThumbnails)
        return;
    m_listThumbnails = on;
    saveSettings();
    emit listThumbnailsChanged();
}

void LibraryBridge::setThumbnailScale(qreal scale) {
    // Whole steps between 0.5 and 2 (NaN and junk from QML land on the default).
    const qreal next = std::isfinite(scale) ? std::clamp(std::round(scale / kThumbnailScaleStep) * kThumbnailScaleStep,
                                                         kThumbnailScaleMin, kThumbnailScaleMax)
                                            : 1.0;
    if (qFuzzyCompare(next, m_thumbnailScale))
        return;
    m_thumbnailScale = next;
    saveSettings();
    emit thumbnailScaleChanged();
}

QVariantList LibraryBridge::columns() const {
    QVariantList out;
    for (const ColumnState& c : m_columns) {
        if (!c.visible)
            continue;
        const LibraryColumnInfo& info = libraryColumns()[static_cast<std::size_t>(c.column)];
        out.append(QVariantMap{{QStringLiteral("id"), QLatin1String(info.id)},
                               {QStringLiteral("title"), libraryColumnTitle(c.column)},
                               {QStringLiteral("width"), c.width},
                               {QStringLiteral("editable"), info.editable}});
    }
    return out;
}

QVariantList LibraryBridge::allColumns() const {
    QVariantList out;
    for (const ColumnState& c : m_columns) {
        out.append(QVariantMap{{QStringLiteral("id"), QLatin1String(libraryColumns()[static_cast<std::size_t>(c.column)].id)},
                               {QStringLiteral("title"), libraryColumnTitle(c.column)},
                               {QStringLiteral("visible"), c.visible}});
    }
    return out;
}

QString LibraryBridge::sortColumn() const {
    return QLatin1String(libraryColumns()[static_cast<std::size_t>(m_model->sortColumn())].id);
}

void LibraryBridge::applyColumns() {
    std::vector<LibraryColumn> visible;
    for (const ColumnState& c : m_columns) {
        if (c.visible)
            visible.push_back(c.column);
    }
    m_model->setColumns(std::move(visible));
    emit columnsChanged();
}

void LibraryBridge::sortBy(const QString& id) {
    const auto column = libraryColumnFromId(id);
    if (!column)
        return;
    const bool ascending = *column == m_model->sortColumn() ? !m_model->sortAscending() : true;
    m_model->setSort(*column, ascending);
    saveSettings();
    emit sortChanged();
}

void LibraryBridge::setColumnVisible(const QString& id, bool visible) {
    const auto column = libraryColumnFromId(id);
    if (!column || *column == LibraryColumn::Name)
        return; // the name always shows
    for (ColumnState& c : m_columns) {
        if (c.column == *column && c.visible != visible) {
            c.visible = visible;
            applyColumns();
            saveSettings();
        }
    }
}

void LibraryBridge::moveColumn(int from, int to) {
    // `from` and `to` are positions among the visible columns (what the header shows).
    std::vector<std::size_t> visible;
    for (std::size_t i = 0; i < m_columns.size(); ++i) {
        if (m_columns[i].visible)
            visible.push_back(i);
    }
    const auto count = static_cast<int>(visible.size());
    if (from < 0 || from >= count || to < 0 || to >= count || from == to)
        return;
    const ColumnState moved = m_columns[visible[static_cast<std::size_t>(from)]];
    m_columns.erase(m_columns.begin() + static_cast<std::ptrdiff_t>(visible[static_cast<std::size_t>(from)]));
    // Insert before (moving left) or after (moving right) the column now at the target position.
    std::vector<std::size_t> after;
    for (std::size_t i = 0; i < m_columns.size(); ++i) {
        if (m_columns[i].visible)
            after.push_back(i);
    }
    // Moving right, the target column now sits one position earlier: go after it; moving left, go before it.
    const auto at = static_cast<std::size_t>(std::clamp(to > from ? to - 1 : to, 0, static_cast<int>(after.size()) - 1));
    const std::size_t anchor = after[at];
    m_columns.insert(m_columns.begin() + static_cast<std::ptrdiff_t>(to > from ? anchor + 1 : anchor), moved);
    applyColumns();
    saveSettings();
}

void LibraryBridge::setColumnWidth(const QString& id, int width) {
    const auto column = libraryColumnFromId(id);
    if (!column)
        return;
    for (ColumnState& c : m_columns) {
        if (c.column == *column && c.width != std::clamp(width, 40, 800)) {
            c.width = std::clamp(width, 40, 800);
            saveSettings();
            emit columnsChanged();
        }
    }
}

void LibraryBridge::resetColumns() {
    m_columns.clear();
    for (const LibraryColumnInfo& info : libraryColumns())
        m_columns.push_back({info.column, info.defaultVisible, info.defaultWidth});
    m_model->setSort(LibraryColumn::Name, true);
    applyColumns();
    saveSettings();
    emit sortChanged();
}

QStringList LibraryBridge::columnPresets() const {
    QStringList out;
    for (const auto& preset : m_columnPresets)
        out << preset.first;
    return out;
}

bool LibraryBridge::saveColumnPreset(const QString& name) {
    const QString chosen = name.simplified().left(64);
    if (chosen.isEmpty()) {
        setMessage(tr("Give the column layout a name."));
        return false;
    }
    const auto same = [&](const std::pair<QString, std::vector<ColumnState>>& p) {
        return p.first.compare(chosen, Qt::CaseInsensitive) == 0;
    };
    if (const auto it = std::find_if(m_columnPresets.begin(), m_columnPresets.end(), same); it != m_columnPresets.end()) {
        it->second = m_columns;
    } else if (static_cast<int>(m_columnPresets.size()) >= kMaxColumnPresets) {
        setMessage(tr("Delete a column layout first: at most %1 can be kept.").arg(kMaxColumnPresets));
        return false;
    } else {
        m_columnPresets.push_back({chosen, m_columns});
    }
    setMessage({});
    saveSettings();
    emit columnPresetsChanged();
    return true;
}

bool LibraryBridge::applyColumnPreset(const QString& name) {
    const auto it = std::find_if(m_columnPresets.begin(), m_columnPresets.end(), [&](const auto& p) {
        return p.first.compare(name.simplified(), Qt::CaseInsensitive) == 0;
    });
    if (it == m_columnPresets.end())
        return false;
    m_columns = it->second;
    applyColumns();
    saveSettings();
    return true;
}

bool LibraryBridge::deleteColumnPreset(const QString& name) {
    const auto removed = std::erase_if(m_columnPresets, [&](const auto& p) {
        return p.first.compare(name.simplified(), Qt::CaseInsensitive) == 0;
    });
    if (removed == 0)
        return false;
    saveSettings();
    emit columnPresetsChanged();
    return true;
}

bool LibraryBridge::setNestBin(const QString& key, bool on) {
    const auto bin = parseBinKey(key);
    if (!bin) {
        setMessage(tr("Choose a bin."));
        return false;
    }
    const bool ok = apply(on ? QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "File Nested Sequences in Bin")
                             : QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Stop Filing Nested Sequences in Bin"),
                          [&](const ProjectState& s) { return edit::setNestBin(s, *bin, on); });
    if (ok)
        setMessage({});
    return ok;
}

// ---- Refresh ---------------------------------------------------------------------------------------------------------

namespace {

/// P1 round (#40): every media item's and sequence's uses in `state`: clips on video / audio tracks, and placements (a
/// linked video / audio pair of one item counts once). Nested and multi-camera clips count for their sequence.
void computeUsage(const ProjectState& state, std::unordered_map<std::uint64_t, LibraryUsage>& media,
                  std::unordered_map<std::uint64_t, LibraryUsage>& sequences) {
    media.clear();
    sequences.clear();
    for (const SequencePtr& seq : state.sequences) {
        std::set<std::pair<std::uint64_t, std::uint64_t>> groups; // (item, link group) placed in this sequence
        const auto count = [&](const Clip& c, bool video) {
            const bool nested = c.isNested();
            const std::uint64_t id = nested ? c.sequence.value : c.media.value;
            if (id == 0)
                return;
            LibraryUsage& u = (nested ? sequences : media)[id];
            (video ? u.video : u.audio) += 1;
            if (!c.linkGroup.isValid() || groups.insert({(nested ? (1ULL << 63) : 0ULL) | id, c.linkGroup.value}).second)
                ++u.uses;
            // 0.6 (card #74): the other takes of an audition use their media too (Remove Unused keeps them).
            if (const Audition* audition = c.audition()) {
                for (const AuditionTake& take : audition->others) {
                    const bool takeNested = take.isNested();
                    const std::uint64_t takeId = takeNested ? take.sequence.value : take.media.value;
                    if (takeId == 0)
                        continue;
                    LibraryUsage& t = (takeNested ? sequences : media)[takeId];
                    (video ? t.video : t.audio) += 1;
                    if (!c.linkGroup.isValid()
                        || groups.insert({(takeNested ? (1ULL << 63) : 0ULL) | takeId, c.linkGroup.value}).second)
                        ++t.uses;
                }
            }
            // 0.7 (card 069): the nest a rendered clip can be restored to counts as used, so Remove Unused keeps it and a
            // delete asks first.
            if (const RenderedNest* rendered = c.renderedNest(); rendered && rendered->sequence.value != 0) {
                LibraryUsage& r = sequences[rendered->sequence.value];
                (video ? r.video : r.audio) += 1;
                if (!c.linkGroup.isValid() || groups.insert({(1ULL << 63) | rendered->sequence.value, c.linkGroup.value}).second)
                    ++r.uses;
            }
        };
        for (const TrackPtr& t : seq->videoTracks) {
            if (t) {
                for (const Clip& c : t->clips)
                    count(c, true);
            }
        }
        for (const TrackPtr& t : seq->audioTracks) {
            if (t) {
                for (const Clip& c : t->clips)
                    count(c, false);
            }
        }
    }
}

} // namespace

void LibraryBridge::recomputeStats(const ProjectState& state) {
    // P1 round (#40): what is used comes from the usage counts refresh() made.
    m_used.clear();
    for (const auto& [id, use] : m_usage)
        m_used.insert(id);
    // Card #169: a subclip plays its master's Master Clip Effects (MasterClipEffects.h), so the master of a used subclip is
    // in use - even when no clip plays it itself - and Remove Unused keeps it. (A subclip of a subclip: the chain.)
    for (const MediaPtr& m : state.media) {
        std::uint64_t id = m ? m->id.value : 0;
        for (const MediaItem* item = m.get(); item && item->subclip && m_used.contains(id);) {
            id = item->subclip->master.value;
            item = state.findMedia(item->subclip->master);
            if (!m_used.insert(id).second)
                break;
        }
    }
    m_unused.clear();
    for (const MediaPtr& m : state.media) {
        if (!m_used.contains(m->id.value))
            m_unused.insert(m->id.value);
    }
    // Duplicates: the same file content (size and sample hash) imported more than once. Subclips share their
    // master's file on purpose, and generated items have no file. 0.7 (card 025): one definition in the model, shared with
    // the search bins and Consolidate Duplicates.
    m_duplicates.clear();
    m_duplicateGroups.clear();
    for (const std::vector<MediaId>& ids : edit::duplicateMediaGroups(state)) {
        QVariantList rows;
        for (const MediaId id : ids) {
            const MediaItem* m = state.findMedia(id);
            if (!m)
                continue;
            m_duplicates.insert(m->id.value);
            rows.append(QVariantMap{{QStringLiteral("key"), mediaKey(m->id)}, {QStringLiteral("name"), text(m->name)}});
        }
        const MediaItem* first = state.findMedia(ids.front());
        m_duplicateGroups.append(QVariantMap{{QStringLiteral("name"), text(first ? first->name : std::string())},
                                             {QStringLiteral("items"), rows}});
    }
}

bool LibraryBridge::sequenceHasPicture(const ProjectState& state, SequenceId id,
                                       std::unordered_set<std::uint64_t>& seen) const {
    if (!seen.insert(id.value).second)
        return false;
    const Sequence* sequence = state.findSequence(id);
    if (!sequence)
        return false;
    for (const auto& track : sequence->videoTracks) {
        if (!track)
            continue;
        for (const Clip& clip : track->clips) {
            if (clip.graphic())
                return true;
            if (clip.isNested()) {
                if (sequenceHasPicture(state, clip.sequence, seen))
                    return true;
                continue;
            }
            const MediaItem* item = state.findMedia(clip.media);
            if (!item)
                continue;
            if (item->isGenerator())
                return true;
            // m_thumbnails has a URL only for media with a picture on disk (ProjectController skips offline media).
            if (const auto it = m_thumbnails.find(item->id.value); it != m_thumbnails.end() && !it->second.isEmpty())
                return true;
        }
    }
    return false;
}

QString LibraryBridge::thumbnailFor(const MediaItem& item) const {
    if (item.isGenerator() || !item.info.hasVideo())
        return {};
    const auto it = m_thumbnails.find(item.id.value);
    if (it == m_thumbnails.end() || it->second.isEmpty())
        return {};
    // The frame shown: the poster frame, else the middle of the media. Thumbnails are decoded from the raw file, so a
    // source redaction covering that frame hides the picture (review P2-9).
    const Ticks shown = item.posterFrame.value_or(Ticks{item.info.duration.value / 2});
    if (m_lastSnapshot) {
        for (const Redaction* r : redactionsCovering(*m_lastSnapshot, item.id)) { // a subclip's master's too (wave 5)
            if (r->enabled && r->source.contains(shown))
                return {};
        }
    }
    if (item.posterFrame) {
        const auto path = m_paths.find(item.id.value);
        if (path != m_paths.end())
            return PosterImageProvider::urlFor(path->second, *item.posterFrame, item.info.video.front().index);
    }
    return it->second;
}

void LibraryBridge::refresh(bool force) {
    const ProjectSnapshot snapshot = m_controller ? m_controller->snapshot() : nullptr;
    // Every edit (a trim on the timeline) gives a new snapshot; only what the panel shows makes it list again: media
    // nodes, bins, the sequences' library columns, how the media are used (P1 round, #40: the counts), and redactions
    // (thumbnails).
    std::unordered_map<std::uint64_t, LibraryUsage> usage;
    std::unordered_map<std::uint64_t, LibraryUsage> sequenceUsage;
    if (snapshot)
        computeUsage(*snapshot, usage, sequenceUsage);
    // P1 round (#46): the twirl-down bins of this project.
    const QString projectFile = m_controller ? QString::fromStdU16String(m_controller->projectFile().u16string()) : QString();
    if (!m_expansionLoaded || projectFile != m_expansionFile) {
        loadExpansion(projectFile);
        force = true;
    }
    const ProjectSnapshot last = m_lastSnapshot;
    const bool mediaSame = last && snapshot && last->media.size() == snapshot->media.size() &&
                           std::equal(snapshot->media.begin(), snapshot->media.end(), last->media.begin());
    const bool sequencesSame =
        last && snapshot && last->sequences.size() == snapshot->sequences.size() &&
        std::equal(snapshot->sequences.begin(), snapshot->sequences.end(), last->sequences.begin(),
                   [](const SequencePtr& a, const SequencePtr& b) {
                       return a->id == b->id && a->name == b->name && a->bin == b->bin && a->duration == b->duration &&
                              a->frameRate == b->frameRate && a->width == b->width && a->height == b->height &&
                              a->markIn == b->markIn && a->markOut == b->markOut;
                   });
    if (!force && mediaSame && sequencesSame && last->bins == snapshot->bins && usage == m_usage &&
        sequenceUsage == m_sequenceUsage && last->redactions == snapshot->redactions) {
        if (last->freeform != snapshot->freeform) { // 0.2 (P.3): only an arrangement changed
            m_lastSnapshot = snapshot;
            refreshFreeform();
        }
        return;
    }
    const bool mediaChanged = !last || !snapshot || last->media.size() != snapshot->media.size() ||
                              !std::equal(snapshot->media.begin(), snapshot->media.end(), last->media.begin(),
                                          [](const MediaPtr& a, const MediaPtr& b) {
                                              return a->id == b->id && a->locator == b->locator;
                                          });
    m_lastSnapshot = snapshot;
    m_usage = std::move(usage);
    m_sequenceUsage = std::move(sequenceUsage);
    if (snapshot) {
        if (m_bin.isValid() && !snapshot->findBin(m_bin)) {
            m_bin = {};
            emit binChanged();
        }
        const auto before = m_tabs.size();
        std::erase_if(m_tabs, [&](BinId b) { return !snapshot->findBin(b); });
        if (m_tabs.size() != before)
            emit tabsChanged();
        recomputeStats(*snapshot);
        // Thumbnails: ProjectController resolved the files (it skips offline media); the bridge adds poster frames.
        m_thumbnails.clear();
        for (const QVariant& v : m_controller->mediaItems()) {
            const QVariantMap m = v.toMap();
            if (const auto id = parseMediaKey(m.value(QStringLiteral("id")).toString()))
                m_thumbnails.emplace(id->value, m.value(QStringLiteral("thumbnail")).toString());
        }
        m_paths.clear();
        for (const MediaPtr& m : snapshot->media) {
            if (!m->isGenerator() && !m->locator.offline)
                m_paths.emplace(m->id.value, text(m->locator.path));
        }
    } else {
        m_used.clear();
        m_unused.clear();
        m_duplicates.clear();
        m_duplicateGroups.clear();
    }
    LibraryFilter filter = m_filter;
    filter.audioOnly = m_scope == QStringLiteral("audio");
    LibraryModel::Context context;
    context.projectFile = m_controller ? m_controller->projectFile() : std::filesystem::path{};
    context.thumbnail = [this](const MediaItem& item) { return thumbnailFor(item); };
    context.sequenceThumbnail = [this](const ProjectState& state, const Sequence& sequence) {
        // Its middle frame, rendered (I.9), when there is something to draw: an empty sequence or one of offline media
        // keeps the film icon instead of a render that can only fail (and warn "no picture").
        std::unordered_set<std::uint64_t> seen;
        return sequenceHasPicture(state, sequence.id, seen) ? SequenceThumbnailProvider::urlFor(state, sequence.id) : QString();
    };
    context.status = &m_status;
    context.used = &m_used;
    context.duplicates = &m_duplicates;
    context.usage = &m_usage;                  // P1 round (#40)
    context.sequenceUsage = &m_sequenceUsage;
    context.tree = m_view == QStringLiteral("list"); // P1 round (#46): twirl-down bins in the list
    context.expanded = &m_expanded;
    m_model->refresh(snapshot, m_bin, filter, context);
    pruneSelection();
    pruneCut();
    refreshFreeform();
    emit statsChanged();
    emit binChanged(); // the breadcrumb's names may have changed
    if (mediaChanged)
        startScan(snapshot);
}

void LibraryBridge::refreshStatus() {
    m_status = m_analysis;
    if (m_readiness) {
        const QVariantMap states = m_readiness->mediaStates();
        for (auto it = states.begin(); it != states.end(); ++it) {
            const auto id = parseMediaKey(it.key());
            if (!id)
                continue;
            const QVariantMap s = it.value().toMap();
            LibraryMediaStatus& status = m_status[id->value];
            status.proxyState = s.value(QStringLiteral("state")).toString();
            status.proxyProgress = s.value(QStringLiteral("progress")).toDouble();
        }
    }
    m_model->statusChanged();
}

void LibraryBridge::startScan(ProjectSnapshot snapshot) {
    const std::uint64_t generation = m_scanGeneration->fetch_add(1) + 1;
    if (m_scan.joinable()) {
        m_scan.request_stop();
        m_scan.join();
    }
    if (!snapshot || !m_probe) {
        m_scanning = false;
        return;
    }
    struct Job {
        MediaPtr item;
        std::filesystem::path file;
    };
    std::vector<Job> jobs;
    const std::filesystem::path projectFile = m_controller ? m_controller->projectFile() : std::filesystem::path{};
    for (const MediaPtr& m : snapshot->media) {
        if (!m->isGenerator() && !m->locator.offline && !m->locator.path.empty())
            jobs.push_back({m, {}});
    }
    auto run = [jobs = std::move(jobs), projectFile, probe = m_probe](std::stop_token stop) mutable {
        std::unordered_map<std::uint64_t, LibraryMediaStatus> out;
        for (Job& job : jobs) {
            if (stop.stop_requested())
                break;
            // A file that cannot be found has no analysis (the default probe returns nothing for an empty path).
            const auto file = resolveMediaPath(job.item->locator, projectFile);
            out[job.item->id.value] = probe(*job.item, file.value_or(std::filesystem::path{}));
        }
        return out;
    };
    const auto publish = [this, generation](std::unordered_map<std::uint64_t, LibraryMediaStatus> result) {
        if (m_scanGeneration->load() != generation)
            return;
        m_analysis = std::move(result);
        m_scanning = false;
        refreshStatus();
    };
    m_scanning = true;
    if (m_synchronous) {
        publish(run(std::stop_token{}));
        return;
    }
    m_scan = std::jthread([run = std::move(run), publish, self = QPointer<LibraryBridge>(this),
                           counter = m_scanGeneration, generation](std::stop_token stop) mutable {
        auto result = run(stop);
        if (stop.stop_requested() || counter->load() != generation)
            return;
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, publish, result = std::move(result)]() mutable {
                if (self)
                    publish(std::move(result));
            },
            Qt::QueuedConnection);
    });
}

bool LibraryBridge::waitForScan(int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (m_scanning.load() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(5);
    }
    QCoreApplication::processEvents();
    return !m_scanning.load();
}

// ---- Bins -----------------------------------------------------------------------------------------------------------

QVariantList LibraryBridge::breadcrumb() const {
    QVariantList out;
    const ProjectSnapshot snap = m_lastSnapshot;
    BinId bin = m_bin;
    std::size_t guard = 0;
    while (bin.isValid() && snap && guard++ <= snap->bins.size()) {
        const Bin* b = snap->findBin(bin);
        if (!b)
            break;
        out.prepend(QVariantMap{{QStringLiteral("key"), binKey(b->id)}, {QStringLiteral("name"), text(b->name)}});
        bin = b->parent;
    }
    out.prepend(QVariantMap{{QStringLiteral("key"), QString()}, {QStringLiteral("name"), tr("Project")}});
    return out;
}

QVariantList LibraryBridge::tabs() const {
    QVariantList out;
    const BinId current = m_bin;
    // The root tab shows whatever bin was opened in place; bin tabs show their own bin.
    const bool onTab = std::find(m_tabs.begin(), m_tabs.end(), current) != m_tabs.end() && current.isValid();
    out.append(QVariantMap{{QStringLiteral("key"), QString()}, {QStringLiteral("name"), tr("Project")},
                           {QStringLiteral("current"), !onTab}});
    for (const BinId b : m_tabs) {
        const Bin* bin = m_lastSnapshot ? m_lastSnapshot->findBin(b) : nullptr;
        out.append(QVariantMap{{QStringLiteral("key"), binKey(b)},
                               {QStringLiteral("name"), bin ? text(bin->name) : QString()},
                               {QStringLiteral("current"), b == current}});
    }
    return out;
}

void LibraryBridge::openBin(const QString& key) {
    BinId bin;
    if (!key.isEmpty()) {
        const auto parsed = parseBinKey(key);
        if (!parsed || !m_lastSnapshot || !m_lastSnapshot->findBin(*parsed))
            return;
        bin = *parsed;
    }
    if (bin == m_bin && !m_filter.active())
        return;
    m_bin = bin;
    // Opening a bin leaves Find (Premiere's Find is per bin view).
    if (m_filter.active()) {
        m_filter = {};
        emit filterChanged();
    }
    m_selection.clear();
    emit selectionChanged();
    emit binChanged();
    emit tabsChanged();
    refresh(true);
}

void LibraryBridge::openBinInTab(const QString& key) {
    const auto bin = parseBinKey(key);
    if (!bin || !m_lastSnapshot || !m_lastSnapshot->findBin(*bin))
        return;
    if (std::find(m_tabs.begin(), m_tabs.end(), *bin) == m_tabs.end())
        m_tabs.push_back(*bin);
    openBin(key);
    emit tabsChanged();
}

void LibraryBridge::closeTab(const QString& key) {
    const auto bin = parseBinKey(key);
    if (!bin)
        return;
    const auto it = std::find(m_tabs.begin(), m_tabs.end(), *bin);
    if (it == m_tabs.end())
        return;
    m_tabs.erase(it);
    emit tabsChanged();
    if (m_bin == *bin)
        openBin(QString());
}

void LibraryBridge::goUp() {
    if (!m_bin.isValid() || !m_lastSnapshot)
        return;
    const Bin* b = m_lastSnapshot->findBin(m_bin);
    openBin(b && b->parent.isValid() ? binKey(b->parent) : QString());
}

bool LibraryBridge::showItem(const QString& key) {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    if (!snap)
        return false;
    BinId bin;
    if (const auto b = parseBinKey(key)) {
        const Bin* found = snap->findBin(*b);
        if (!found)
            return false;
        bin = found->parent;
    } else if (const auto m = parseMediaKey(key)) {
        const MediaItem* found = snap->findMedia(*m);
        if (!found)
            return false;
        bin = found->bin;
    } else if (const auto q = parseSequenceKey(key)) {
        const Sequence* found = snap->findSequence(*q);
        if (!found)
            return false;
        bin = found->bin;
    } else {
        return false;
    }
    if (m_filter.active()) {
        m_filter = {};
        emit filterChanged();
    }
    if (m_scope != QStringLiteral("all")) {
        m_scope = QStringLiteral("all");
        emit scopeChanged();
    }
    m_bin = bin;
    emit binChanged();
    emit tabsChanged();
    refresh(true);
    select(key, Replace);
    emit itemShown(key);
    return true;
}

bool LibraryBridge::apply(const char* label,
                          const std::function<std::expected<ProjectState, Error>(const ProjectState&)>& fn) {
    if (!m_controller)
        return false;
    const bool ok = m_controller->applyEditFn(label, [&](const ProjectState& s) -> edit::EditOutcome {
        auto next = fn(s);
        if (!next)
            return std::unexpected(next.error());
        edit::EditResult result;
        result.changes = diff(s, *next);
        result.state = std::move(*next);
        return result;
    });
    if (!ok)
        setMessage(m_controller->lastError());
    return ok;
}

QString LibraryBridge::newBin(const QString& name) {
    if (!m_controller)
        return {};
    QString chosen = name.trimmed();
    if (chosen.isEmpty()) {
        // "Bin 01", "Bin 02"...: the first number not taken in this bin (Premiere's naming).
        const ProjectSnapshot snap = m_controller->snapshot();
        for (int n = 1; n < 1000 && chosen.isEmpty(); ++n) {
            const QString candidate = tr("Bin %1").arg(n, 2, 10, QLatin1Char('0'));
            const bool taken = std::any_of(snap->bins.begin(), snap->bins.end(), [&](const Bin& b) {
                return b.parent == m_bin && text(b.name) == candidate;
            });
            if (!taken)
                chosen = candidate;
        }
    }
    BinId created;
    const BinId parent = targetBin();
    if (!apply(QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "New Bin"), [&](const ProjectState& s) {
            auto added = edit::addBin(s, chosen.toStdString(), parent);
            if (!added)
                return std::expected<ProjectState, Error>(std::unexpected(added.error()));
            created = added->id;
            return std::expected<ProjectState, Error>(std::move(added->state));
        }))
        return {};
    const QString key = binKey(created);
    select(key, Replace);
    emit renameRequested(key);
    return key;
}

QString LibraryBridge::newBinFromSelection() {
    if (!m_controller || m_selection.isEmpty())
        return {};
    const QStringList keys = selection();
    std::vector<BinId> bins;
    std::vector<MediaId> media;
    std::vector<SequenceId> sequences;
    for (const QString& key : keys) {
        if (const auto b = parseBinKey(key))
            bins.push_back(*b);
        else if (const auto m = parseMediaKey(key))
            media.push_back(*m);
        else if (const auto q = parseSequenceKey(key))
            sequences.push_back(*q);
    }
    BinId created;
    const BinId parent = targetBin();
    // The bin is named after the first item, like Premiere.
    QString name = tr("Bin from selection");
    if (const MediaItem* first = !media.empty() ? m_controller->snapshot()->findMedia(media.front()) : nullptr)
        name = text(first->name);
    if (!apply(QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "New Bin from Selection"), [&](const ProjectState& s) {
            auto added = edit::addBin(s, name.toStdString(), parent);
            if (!added)
                return std::expected<ProjectState, Error>(std::unexpected(added.error()));
            created = added->id;
            return edit::moveToBin(added->state, created, bins, media, sequences);
        }))
        return {};
    const QString key = binKey(created);
    select(key, Replace);
    emit renameRequested(key);
    return key;
}

bool LibraryBridge::moveItems(const QStringList& keys, const QString& binText) {
    BinId target;
    if (!binText.isEmpty()) {
        const auto parsed = parseBinKey(binText);
        if (!parsed)
            return false;
        target = *parsed;
    }
    std::vector<BinId> bins;
    std::vector<MediaId> media;
    std::vector<SequenceId> sequences;
    for (const QString& key : keys) {
        if (const auto b = parseBinKey(key)) {
            if (*b != target)
                bins.push_back(*b);
        } else if (const auto m = parseMediaKey(key)) {
            media.push_back(*m);
        } else if (const auto q = parseSequenceKey(key)) {
            sequences.push_back(*q);
        }
    }
    if (bins.empty() && media.empty() && sequences.empty())
        return false;
    return apply(QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Move to Bin"), [&](const ProjectState& s) {
        return edit::moveToBin(s, target, bins, media, sequences);
    });
}

// ---- Move to Bin, Cut and Paste (P1 round, #41) ------------------------------------------------------------------

QVariantList LibraryBridge::binTree(const QStringList& moving) const {
    QVariantList out;
    out.append(QVariantMap{{QStringLiteral("key"), QString()},
                           {QStringLiteral("name"), tr("Project")},
                           {QStringLiteral("depth"), 0},
                           {QStringLiteral("disabled"), false},
                           {QStringLiteral("reason"), QString()}});
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    if (!snap)
        return out;
    std::unordered_set<std::uint64_t> movingBins;
    // Fix round 1 (ARGON-22): the bin all of `moving` are in now (0 = the root), when they share one: moving there does
    // nothing, so its row is disabled ("Already in this bin").
    std::optional<std::uint64_t> current;
    bool shared = !moving.isEmpty();
    for (const QString& key : moving) {
        std::optional<std::uint64_t> in;
        if (const auto b = parseBinKey(key)) {
            movingBins.insert(b->value);
            if (const Bin* bin = snap->findBin(*b))
                in = bin->parent.value;
        } else if (const auto m = parseMediaKey(key)) {
            if (const MediaItem* item = snap->findMedia(*m))
                in = item->bin.value;
        } else if (const auto q = parseSequenceKey(key)) {
            if (const Sequence* sequence = snap->findSequence(*q))
                in = sequence->bin.value;
        }
        if (!in || (current && *current != *in))
            shared = false;
        current = in;
    }
    if (!shared)
        current.reset();
    const QString here = tr("Already in this bin");
    if (current && *current == 0) {
        QVariantMap root = out.front().toMap();
        root.insert(QStringLiteral("disabled"), true);
        root.insert(QStringLiteral("reason"), here);
        root.insert(QStringLiteral("current"), true);
        out[0] = root;
    }
    std::map<std::uint64_t, std::vector<const Bin*>> children; // parent (0 = the root) -> bins, by name
    for (const Bin& b : snap->bins)
        children[b.parent.value].push_back(&b);
    for (auto& entry : children) {
        std::stable_sort(entry.second.begin(), entry.second.end(), [](const Bin* a, const Bin* b) {
            return QString::compare(text(a->name), text(b->name), Qt::CaseInsensitive) < 0;
        });
    }
    // Depth first; a bin inside a moving one is disabled too (it would move into itself).
    std::function<void(std::uint64_t, int, bool)> walk = [&](std::uint64_t parent, int depth, bool insideMoving) {
        const auto found = children.find(parent);
        if (found == children.end() || depth > 64)
            return;
        for (const Bin* b : found->second) {
            const bool moved = insideMoving || movingBins.contains(b->id.value);
            const bool isCurrent = current && *current == b->id.value;
            const QString reason = moved           ? tr("A bin cannot move into itself")
                                   : b->isSearch() ? tr("A search bin lists what it finds: nothing moves into it")
                                   : isCurrent     ? here
                                                   : QString();
            out.append(QVariantMap{{QStringLiteral("key"), binKey(b->id)},
                                   {QStringLiteral("name"), text(b->name)},
                                   {QStringLiteral("depth"), depth},
                                   {QStringLiteral("disabled"), !reason.isEmpty()},
                                   {QStringLiteral("reason"), reason},
                                   {QStringLiteral("current"), isCurrent}});
            if (!b->isSearch())
                walk(b->id.value, depth + 1, moved);
        }
    };
    walk(0, 1, false);
    return out;
}

QStringList LibraryBridge::cutKeys() const {
    QStringList out(m_cut.begin(), m_cut.end());
    out.sort();
    return out;
}

bool LibraryBridge::cutItems(const QStringList& keys) {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    QSet<QString> marked;
    for (const QString& key : keys) {
        const bool exists = snap && ((parseBinKey(key) && snap->findBin(*parseBinKey(key))) ||
                                     (parseMediaKey(key) && snap->findMedia(*parseMediaKey(key))) ||
                                     (parseSequenceKey(key) && snap->findSequence(*parseSequenceKey(key))));
        if (exists)
            marked.insert(key);
    }
    if (marked.isEmpty())
        return false;
    m_cut = marked;
    m_model->setCut(m_cut);
    emit cutChanged();
    emit notice(m_cut.size() == 1
                    ? tr("Cut 1 item: select a bin (or open one) and paste to move it there.")
                    : tr("Cut %1 items: select a bin (or open one) and paste to move them there.").arg(m_cut.size()));
    return true;
}

QString LibraryBridge::pasteTarget() const {
    // The selected bin, when exactly one is selected (and it is not itself cut), else the bin shown.
    if (m_selection.size() == 1) {
        const QString only = *m_selection.begin();
        const auto bin = parseBinKey(only);
        const Bin* found = bin && m_lastSnapshot ? m_lastSnapshot->findBin(*bin) : nullptr;
        if (found && !found->isSearch() && !m_cut.contains(only))
            return only;
    }
    return binKey(targetBin());
}

bool LibraryBridge::pasteItems() {
    pruneCut();
    if (m_cut.isEmpty()) {
        setMessage(tr("Nothing is cut: cut items in the Library first."));
        return false;
    }
    const QString target = pasteTarget();
    const QStringList keys = cutKeys();
    if (keys == QStringList{target}) {
        setMessage(tr("A bin cannot move into itself: select another bin, then paste."));
        return false;
    }
    if (!moveItems(keys, target))
        return false; // the reason is in `message`; the marks stay for another bin
    const auto bin = parseBinKey(target);
    const Bin* found = bin && m_lastSnapshot ? m_lastSnapshot->findBin(*bin) : nullptr;
    emit notice((keys.size() == 1 ? tr("Moved 1 item into “%1”.") : tr("Moved %1 items into “%2”.").arg(keys.size()))
                    .arg(found ? text(found->name) : tr("Project")));
    clearCut();
    return true;
}

void LibraryBridge::clearCut() {
    if (m_cut.isEmpty())
        return;
    m_cut.clear();
    m_model->setCut(m_cut);
    emit cutChanged();
}

void LibraryBridge::pruneCut() {
    if (m_cut.isEmpty())
        return;
    const ProjectSnapshot snap = m_lastSnapshot;
    const auto before = m_cut.size();
    m_cut.removeIf([&](const QString& key) {
        if (!snap)
            return true;
        if (const auto b = parseBinKey(key))
            return snap->findBin(*b) == nullptr;
        if (const auto m = parseMediaKey(key))
            return snap->findMedia(*m) == nullptr;
        if (const auto q = parseSequenceKey(key))
            return snap->findSequence(*q) == nullptr;
        return true;
    });
    if (m_cut.size() != before) {
        m_model->setCut(m_cut);
        emit cutChanged();
    }
}

void LibraryBridge::collectUnder(const QStringList& keys, std::vector<const MediaItem*>& media,
                                 std::vector<const Sequence*>& sequences) const {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : m_lastSnapshot;
    if (!snap)
        return;
    std::unordered_set<std::uint64_t> bins;
    std::unordered_set<std::uint64_t> seenMedia;
    std::unordered_set<std::uint64_t> seenSequences;
    for (const MediaItem* m : media)
        seenMedia.insert(m->id.value);
    for (const Sequence* s : sequences)
        seenSequences.insert(s->id.value);
    for (const QString& key : keys) {
        if (const auto b = parseBinKey(key)) {
            const Bin* found = snap->findBin(*b);
            if (found && !found->isSearch()) // a search bin lists what it finds: it holds nothing
                bins.insert(b->value);
        } else if (const auto m = parseMediaKey(key)) {
            const MediaItem* found = snap->findMedia(*m);
            if (found && seenMedia.insert(m->value).second)
                media.push_back(found);
        } else if (const auto q = parseSequenceKey(key)) {
            const Sequence* found = snap->findSequence(*q);
            if (found && seenSequences.insert(q->value).second)
                sequences.push_back(found);
        }
    }
    if (bins.empty())
        return;
    for (bool grew = true; grew;) { // the bins inside them, at any depth
        grew = false;
        for (const Bin& b : snap->bins) {
            if (b.parent.isValid() && bins.contains(b.parent.value) && !b.isSearch() && bins.insert(b.id.value).second)
                grew = true;
        }
    }
    for (const MediaPtr& m : snap->media) {
        if (m->bin.isValid() && bins.contains(m->bin.value) && seenMedia.insert(m->id.value).second)
            media.push_back(m.get());
    }
    for (const SequencePtr& s : snap->sequences) {
        if (s->bin.isValid() && bins.contains(s->bin.value) && seenSequences.insert(s->id.value).second)
            sequences.push_back(s.get());
    }
}

QString LibraryBridge::summaryText() const {
    const int listed = m_model->rowCount();
    QStringList parts{listed == 1 ? tr("1 item") : tr("%1 items").arg(listed)};
    QStringList keys;
    if (!m_selection.isEmpty()) {
        parts << tr("%n selected", nullptr, static_cast<int>(m_selection.size()));
        keys = QStringList(m_selection.begin(), m_selection.end());
    } else {
        keys.reserve(listed);
        for (int row = 0; row < listed; ++row)
            keys << m_model->keyOf(row);
    }
    std::vector<const MediaItem*> media;
    std::vector<const Sequence*> sequences;
    collectUnder(keys, media, sequences);
    std::int64_t bytes = 0;
    double seconds = 0.0;
    for (const MediaItem* m : media) {
        bytes += libraryFileBytes(*m);
        if (!m->isGenerator() && !m->info.isStillImage)
            seconds += (m->subclip ? m->subclip->range.duration : m->info.duration).toSeconds();
    }
    for (const Sequence* s : sequences)
        seconds += s->duration.toSeconds();
    if (bytes > 0)
        parts << librarySizeText(bytes);
    if (seconds >= 0.5) {
        const auto total = static_cast<long long>(std::llround(seconds));
        const long long hours = total / 3600;
        const long long minutes = total / 60 % 60;
        const long long secs = total % 60;
        parts << (hours > 0 ? QStringLiteral("%1:%2:%3").arg(hours).arg(minutes, 2, 10, QLatin1Char('0'))
                                  .arg(secs, 2, 10, QLatin1Char('0'))
                            : QStringLiteral("%1:%2").arg(minutes).arg(secs, 2, 10, QLatin1Char('0')));
    }
    return parts.join(QStringLiteral(" · "));
}

// ---- Metadata ---------------------------------------------------------------------------------------------------------

bool LibraryBridge::rename(const QString& key, const QString& name) {
    const std::string value = name.trimmed().toStdString();
    if (value.empty()) {
        setMessage(tr("A name cannot be empty."));
        return false;
    }
    if (const auto bin = parseBinKey(key)) {
        return apply(QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Rename Bin"),
                     [&](const ProjectState& s) { return edit::renameBin(s, *bin, value); });
    }
    if (const auto media = parseMediaKey(key)) {
        edit::MediaAttributeChange change;
        change.name = value;
        return apply(QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Rename"), [&](const ProjectState& s) {
            return edit::setMediaAttributes(s, std::vector<MediaId>{*media}, change);
        });
    }
    setMessage(tr("Rename sequences from the sequence menu."));
    return false;
}

std::vector<MediaId> LibraryBridge::mediaOf(const QStringList& keys) const {
    std::vector<MediaId> out;
    for (const QString& key : keys) {
        if (const auto m = parseMediaKey(key))
            out.push_back(*m);
    }
    return out;
}

bool LibraryBridge::setLabel(const QStringList& keys, int colorIndex) {
    const std::vector<MediaId> media = mediaOf(keys);
    // 0.2: sequences have a label too (Premiere; their timeline tabs show it with Show Sequence Label Color).
    std::vector<SequenceId> sequences;
    for (const QString& key : keys) {
        if (const auto s = parseSequenceKey(key))
            sequences.push_back(*s);
    }
    if (media.empty() && sequences.empty())
        return false;
    edit::MediaAttributeChange change;
    change.colorIndex = colorIndex;
    edit::SequenceSettingsChange label;
    label.colorIndex = colorIndex;
    // One undo step for the items and the sequences together.
    return apply(QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Label"), [&](const ProjectState& s) -> std::expected<ProjectState, Error> {
        ProjectState next = s;
        if (!media.empty()) {
            auto labelled = edit::setMediaAttributes(next, media, change);
            if (!labelled)
                return std::unexpected(labelled.error());
            next = std::move(*labelled);
        }
        for (const SequenceId seq : sequences) {
            auto labelled = edit::setSequenceSettings(next, seq, label);
            if (!labelled)
                return std::unexpected(labelled.error());
            next = std::move(labelled->state);
        }
        return next;
    });
}

bool LibraryBridge::setComment(const QString& key, const QString& value) {
    const auto media = parseMediaKey(key);
    if (!media)
        return false;
    edit::MediaAttributeChange change;
    change.comment = value.toStdString();
    return apply(QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Comment"), [&](const ProjectState& s) {
        return edit::setMediaAttributes(s, std::vector<MediaId>{*media}, change);
    });
}

bool LibraryBridge::setDescription(const QString& key, const QString& value) {
    const auto media = parseMediaKey(key);
    if (!media)
        return false;
    edit::MediaAttributeChange change;
    change.description = value.toStdString();
    return apply(QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Description"), [&](const ProjectState& s) {
        return edit::setMediaAttributes(s, std::vector<MediaId>{*media}, change);
    });
}

bool LibraryBridge::setPosterFrame(const QString& key, double fraction) {
    const auto media = parseMediaKey(key);
    if (!media || !m_controller)
        return false;
    const MediaItem* item = m_controller->snapshot()->findMedia(*media);
    if (!item)
        return false;
    edit::MediaAttributeChange change;
    if (fraction < 0) {
        change.posterFrame = std::optional<Ticks>();
    } else {
        // Snapped to a frame of the media, inside its span.
        const TimeRange span = spanOf(*item);
        const Rate rate = rateOf(*item);
        const Ticks at{span.start.value + static_cast<std::int64_t>(std::clamp(fraction, 0.0, 1.0) *
                                                                    static_cast<double>(span.duration.value))};
        Ticks snapped = snapToFrame(std::min(at, span.end() - Ticks{1}), rate);
        snapped = std::max(snapped, span.start);
        change.posterFrame = std::optional<Ticks>(snapped);
    }
    return apply(QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Poster Frame"), [&](const ProjectState& s) {
        return edit::setMediaAttributes(s, std::vector<MediaId>{*media}, change);
    });
}

namespace {

/// The bins, media and sequences `keys` name.
void splitKeys(const QStringList& keys, std::vector<BinId>& bins, std::vector<MediaId>& media,
               std::vector<SequenceId>& sequences) {
    for (const QString& key : keys) {
        if (const auto b = parseBinKey(key))
            bins.push_back(*b);
        else if (const auto m = parseMediaKey(key))
            media.push_back(*m);
        else if (const auto q = parseSequenceKey(key))
            sequences.push_back(*q);
    }
}

/// Whether bin `bin` is `root` or inside it.
bool binInside(const ProjectState& state, BinId bin, BinId root) {
    for (std::size_t guard = 0; bin.isValid() && guard <= state.bins.size(); ++guard) {
        if (bin == root)
            return true;
        const Bin* b = state.findBin(bin);
        bin = b ? b->parent : BinId{};
    }
    return false;
}

/// What deleting `bins`, `media` and `sequences` takes away: the media and sequences named and those inside the bins.
void deletedItems(const ProjectState& state, std::span<const BinId> bins, std::span<const MediaId> media,
                  std::span<const SequenceId> sequences, std::unordered_set<std::uint64_t>& deletedMedia,
                  std::unordered_set<std::uint64_t>& deletedSequences) {
    const auto inDeletedBin = [&](BinId bin) {
        return bin.isValid() && std::any_of(bins.begin(), bins.end(), [&](BinId root) { return binInside(state, bin, root); });
    };
    for (const MediaId m : media)
        deletedMedia.insert(m.value);
    for (const SequenceId q : sequences)
        deletedSequences.insert(q.value);
    for (const MediaPtr& m : state.media) {
        if (inDeletedBin(m->bin))
            deletedMedia.insert(m->id.value);
    }
    for (const SequencePtr& q : state.sequences) {
        if (inDeletedBin(q->bin))
            deletedSequences.insert(q->id.value);
    }
}

/// 0.6 (card #74): whether a take's source (Auditions.h forEachTakeSource: media, or a nested / multicam sequence) goes.
bool sourceDeleted(MediaId m, SequenceId q, const std::unordered_set<std::uint64_t>& deletedMedia,
                   const std::unordered_set<std::uint64_t>& deletedSequences) {
    return q.isValid() ? deletedSequences.contains(q.value) : m.isValid() && deletedMedia.contains(m.value);
}

/// How many of a clip's takes play what goes, of how many (one for a clip that is not an audition).
std::pair<std::size_t, std::size_t> deletedTakes(const Clip& c, const std::unordered_set<std::uint64_t>& deletedMedia,
                                                 const std::unordered_set<std::uint64_t>& deletedSequences) {
    std::size_t deleted = 0;
    std::size_t total = 0;
    forEachTakeSource(c, [&](MediaId m, SequenceId q) {
        ++total;
        deleted += sourceDeleted(m, q, deletedMedia, deletedSequences) ? 1 : 0;
    });
    return {deleted, total};
}

/// The clips of the sequences that stay that play what goes, by sequence. `lifted`: every take of the clip plays a
/// deleted item (an ordinary clip: its source goes), with their linked partners (Delete follows links). `losingTakes`:
/// auditions with other takes that stay - only the takes that play what goes leave them.
struct DeletedUses {
    std::map<std::uint64_t, std::vector<ClipId>> lifted;
    std::map<std::uint64_t, std::vector<ClipId>> losingTakes;
};

DeletedUses deletedUses(const ProjectState& state, std::span<const BinId> bins, std::span<const MediaId> media,
                        std::span<const SequenceId> sequences) {
    std::unordered_set<std::uint64_t> deletedMedia;
    std::unordered_set<std::uint64_t> deletedSequences;
    deletedItems(state, bins, media, sequences, deletedMedia, deletedSequences);
    DeletedUses out;
    for (const SequencePtr& seq : state.sequences) {
        if (deletedSequences.contains(seq->id.value))
            continue; // its clips go with it
        std::unordered_set<std::uint64_t> groups;
        std::vector<ClipId> clips;
        std::vector<ClipId> auditions;
        seq->forEachTrack([&](const Track& t) {
            for (const Clip& c : t.clips) {
                const auto [deleted, total] = deletedTakes(c, deletedMedia, deletedSequences);
                if (deleted == 0)
                    continue;
                if (deleted < total) {
                    auditions.push_back(c.id);
                    continue;
                }
                clips.push_back(c.id);
                if (c.linkGroup.isValid())
                    groups.insert(c.linkGroup.value);
            }
        });
        if (!clips.empty()) {
            // Their linked partners go too (Delete follows links).
            seq->forEachTrack([&](const Track& t) {
                for (const Clip& c : t.clips) {
                    if (c.linkGroup.isValid() && groups.contains(c.linkGroup.value) &&
                        std::find(clips.begin(), clips.end(), c.id) == clips.end())
                        clips.push_back(c.id);
                }
            });
            std::erase_if(auditions, [&](ClipId id) { return std::find(clips.begin(), clips.end(), id) != clips.end(); });
            out.lifted.emplace(seq->id.value, std::move(clips));
        }
        if (!auditions.empty())
            out.losingTakes.emplace(seq->id.value, std::move(auditions));
    }
    return out;
}

} // namespace

std::map<std::uint64_t, std::vector<ClipId>> clipsUsingLibraryItems(const ProjectState& state, std::span<const BinId> bins,
                                                                     std::span<const MediaId> media,
                                                                     std::span<const SequenceId> sequences) {
    return deletedUses(state, bins, media, sequences).lifted;
}

std::map<std::uint64_t, std::vector<ClipId>> auditionsLosingTakes(const ProjectState& state, std::span<const BinId> bins,
                                                                  std::span<const MediaId> media,
                                                                  std::span<const SequenceId> sequences) {
    return deletedUses(state, bins, media, sequences).losingTakes;
}

std::expected<ProjectState, Error> removeLibraryItems(const ProjectState& s, std::span<const BinId> binsGiven,
                                                      std::span<const MediaId> media, std::span<const SequenceId> sequences,
                                                      bool removeClips) {
    ProjectState state = s;
    std::unordered_set<std::uint64_t> deletedMedia;
    std::unordered_set<std::uint64_t> deletedSequences;
    deletedItems(s, binsGiven, media, sequences, deletedMedia, deletedSequences);
    if (removeClips) {
        // 0.6 (card #74): an audition that keeps other takes loses only the takes that play what goes (its partners'
        // same takes with it); the active take passes on as Remove Take passes it. The last such take first, so the
        // take order of the others holds.
        for (const auto& [sequence, clips] : auditionsLosingTakes(s, binsGiven, media, sequences)) {
            for (const ClipId id : clips) {
                for (std::size_t guard = 0; guard <= kMaxAuditionTakes; ++guard) {
                    const Sequence* seq = state.findSequence(SequenceId{sequence});
                    const auto location = seq ? seq->findClip(id) : std::nullopt;
                    if (!location)
                        break;
                    const Clip& clip = location->clip();
                    const auto [deleted, total] = deletedTakes(clip, deletedMedia, deletedSequences);
                    if (deleted == 0 || deleted == total)
                        break; // done, or every take goes: the clip is lifted below
                    std::size_t index = 0;
                    for (std::size_t i = 0; i < total; ++i) {
                        const Clip take = auditionTakeClip(clip, i);
                        if (sourceDeleted(take.media, take.sequence, deletedMedia, deletedSequences))
                            index = i;
                    }
                    auto next = edit::removeAuditionTake(state, SequenceId{sequence}, id, index);
                    if (!next)
                        return std::unexpected(next.error());
                    state = std::move(next->state);
                }
            }
        }
        // P1 round (#38): the clips that play what goes are lifted, in every sequence that stays (no ripple).
        for (const auto& [sequence, clips] : clipsUsingLibraryItems(state, binsGiven, media, sequences)) {
            auto next = edit::deleteClips(state, SequenceId{sequence}, clips, edit::LinkMode::FollowLinks);
            if (!next)
                return std::unexpected(next.error());
            state = std::move(next->state);
        }
    }
    // Every sequence that goes first - those named and those inside the deleted bins: they may play media that goes or
    // nest each other. One that another sequence that goes still nests waits for that one (any order of selection).
    std::vector<SequenceId> pending;
    for (const SequencePtr& q : state.sequences) {
        if (deletedSequences.contains(q->id.value))
            pending.push_back(q->id);
    }
    while (!pending.empty()) {
        std::optional<Error> refused;
        const std::size_t before = pending.size();
        std::erase_if(pending, [&](SequenceId q) {
            auto next = edit::removeSequence(state, q);
            if (!next) {
                if (!refused)
                    refused = next.error();
                return false;
            }
            state = std::move(*next);
            return true;
        });
        if (pending.size() == before)
            return std::unexpected(*refused); // used by a sequence that stays
    }
    // Then media outside the deleted bins, then the bins with what is left in them.
    for (const MediaId m : media) {
        if (!state.findMedia(m))
            continue;
        auto next = edit::removeMedia(state, m);
        if (!next)
            return std::unexpected(next.error());
        state = std::move(*next);
    }
    std::vector<BinId> bins(binsGiven.begin(), binsGiven.end());
    std::erase_if(bins, [&](BinId b) { return !state.findBin(b); });
    if (!bins.empty()) {
        auto next = edit::removeBins(state, bins);
        if (!next)
            return std::unexpected(next.error());
        state = std::move(*next);
    }
    return state;
}

bool LibraryBridge::deleteItems(const QStringList& keys, bool removeClips) {
    std::vector<BinId> bins;
    std::vector<MediaId> media;
    std::vector<SequenceId> sequences;
    splitKeys(keys, bins, media, sequences);
    if (bins.empty() && media.empty() && sequences.empty())
        return false;
    const char* label = removeClips ? QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Delete and Remove Clips")
                                    : QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Delete");
    const bool ok = apply(label, [&](const ProjectState& s) {
        return removeLibraryItems(s, bins, media, sequences, removeClips);
    });
    if (ok) {
        m_selection.clear();
        emit selectionChanged();
        m_model->setSelection(m_selection);
    }
    return ok;
}

QVariantMap LibraryBridge::deleteImpact(const QStringList& keys) const {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    std::vector<BinId> bins;
    std::vector<MediaId> media;
    std::vector<SequenceId> sequences;
    splitKeys(keys, bins, media, sequences);
    int clips = 0;
    QStringList names;
    QString firstName;
    int usedItems = 0;
    int takes = 0;
    int masterSubclips = 0; // card #169: subclips on timelines that lose the Master Clip Effects of an item that goes
    int masterClips = 0;
    QString masterName;
    QStringList unusedKeys;
    if (snap) {
        std::unordered_set<std::uint64_t> deletedMedia;
        std::unordered_set<std::uint64_t> deletedSequences;
        deletedItems(*snap, bins, media, sequences, deletedMedia, deletedSequences);
        for (const MediaPtr& m : snap->media) {
            if (!m || !m->subclip || deletedMedia.contains(m->id.value) || !deletedMedia.contains(m->subclip->master.value))
                continue;
            const MediaItem* master = snap->findMedia(m->subclip->master);
            const auto use = m_usage.find(m->id.value);
            if (!master || master->masterEffects.empty() || use == m_usage.end() || use->second.video + use->second.audio == 0)
                continue;
            ++masterSubclips;
            masterClips += use->second.video + use->second.audio;
            if (masterName.isEmpty())
                masterName = text(master->name);
        }
        const DeletedUses uses = deletedUses(*snap, bins, media, sequences);
        // The items being deleted that those clips play, keyed as media / sequence (a linked partner's own file is not
        // one of them); 0.6: an audition's other takes count (card #74).
        std::unordered_set<std::uint64_t> usedIds;
        const auto note = [&](const Clip& c) {
            forEachTakeSource(c, [&](MediaId m, SequenceId q) {
                if (!sourceDeleted(m, q, deletedMedia, deletedSequences))
                    return;
                if (!usedIds.insert(q.isValid() ? (1ULL << 63) | q.value : m.value).second || !firstName.isEmpty())
                    return;
                if (q.isValid()) {
                    if (const Sequence* nested = snap->findSequence(q))
                        firstName = text(nested->name);
                } else if (const MediaItem* item = snap->findMedia(m)) {
                    firstName = text(item->name);
                }
            });
        };
        std::map<std::uint64_t, QString> touched; // the sequences that lose clips or takes, in id order
        for (const auto* list : {&uses.lifted, &uses.losingTakes}) {
            for (const auto& [sequence, ids] : *list) {
                const Sequence* q = snap->findSequence(SequenceId{sequence});
                if (!q)
                    continue;
                clips += static_cast<int>(ids.size());
                touched.emplace(sequence, text(q->name));
                for (const ClipId id : ids) {
                    const auto loc = q->findClip(id);
                    if (!loc)
                        continue;
                    note(loc->clip());
                    if (list == &uses.losingTakes)
                        takes += static_cast<int>(deletedTakes(loc->clip(), deletedMedia, deletedSequences).first);
                }
            }
        }
        for (const auto& [id, name] : touched)
            names << name;
        usedItems = static_cast<int>(usedIds.size());
        // Fix round 1 (ARGON-12): the keys no sequence uses on their own (a bin: nothing inside it is used) - what
        // Delete with Options deletes when "Remove their clips from sequences" is off; the used ones stay.
        const auto none = [](const std::map<std::uint64_t, std::vector<ClipId>>& clipsById) {
            return std::ranges::all_of(clipsById, [](const auto& entry) { return entry.second.empty(); });
        };
        for (const QString& key : keys) {
            std::vector<BinId> oneBin;
            std::vector<MediaId> oneMedia;
            std::vector<SequenceId> oneSequence;
            splitKeys(QStringList{key}, oneBin, oneMedia, oneSequence);
            const DeletedUses one = deletedUses(*snap, oneBin, oneMedia, oneSequence);
            if (none(one.lifted) && none(one.losingTakes))
                unusedKeys << key;
        }
        // Fix round 3 (NEW-03): an item used only by another selected item that is itself unused goes with it (a nested
        // sequence that only an unused sequence nests; media only that sequence plays). Grow the set until it holds: each
        // pass ignores the uses of every key already in it, so the whole set is deleted together.
        for (bool grew = true; grew;) {
            grew = false;
            std::vector<BinId> goneBins;
            std::vector<MediaId> goneMedia;
            std::vector<SequenceId> goneSequences;
            splitKeys(unusedKeys, goneBins, goneMedia, goneSequences);
            for (const QString& key : keys) {
                if (unusedKeys.contains(key))
                    continue;
                std::vector<BinId> withBin = goneBins;
                std::vector<MediaId> withMedia = goneMedia;
                std::vector<SequenceId> withSequence = goneSequences;
                splitKeys(QStringList{key}, withBin, withMedia, withSequence);
                const DeletedUses widened = deletedUses(*snap, withBin, withMedia, withSequence);
                if (none(widened.lifted) && none(widened.losingTakes)) {
                    unusedKeys << key;
                    grew = true;
                }
            }
        }
    }
    return {{QStringLiteral("items"), static_cast<int>(keys.size())},
            {QStringLiteral("clips"), clips},
            // 0.6 (card #74): of those clips, auditions that keep other takes lose only these takes.
            {QStringLiteral("takes"), takes},
            {QStringLiteral("sequences"), static_cast<int>(names.size())},
            {QStringLiteral("sequenceNames"), names},
            {QStringLiteral("firstName"), firstName},
            {QStringLiteral("usedItems"), usedItems},
            // Card #169: subclips (and their clips) that would play without the Master Clip Effects of "masterName".
            {QStringLiteral("masterEffectSubclips"), masterSubclips},
            {QStringLiteral("masterEffectClips"), masterClips},
            {QStringLiteral("masterEffectName"), masterName},
            {QStringLiteral("unusedKeys"), unusedKeys}};
}

bool LibraryBridge::requestDelete(const QStringList& keys, bool withOptions) {
    if (keys.isEmpty())
        return false;
    const QVariantMap impact = deleteImpact(keys);
    if (withOptions || impact.value(QStringLiteral("clips")).toInt() > 0 || impact.value(QStringLiteral("masterEffectClips")).toInt() > 0) {
        emit deleteConfirmRequested(keys, impact, withOptions);
        return true;
    }
    return deleteItems(keys);
}

bool LibraryBridge::deleteSelection() { return deleteItems(selection()); }

QVariantMap LibraryBridge::details(const QString& key) const {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    if (!snap)
        return {};
    if (const auto b = parseBinKey(key)) {
        const Bin* bin = snap->findBin(*b);
        return bin ? QVariantMap{{QStringLiteral("key"), key}, {QStringLiteral("kind"), QStringLiteral("bin")},
                                 {QStringLiteral("name"), text(bin->name)}, {QStringLiteral("searchBin"), bin->isSearch()},
                                 {QStringLiteral("nestTarget"), bin->nestTarget}}
                   : QVariantMap{};
    }
    const auto id = parseMediaKey(key);
    const MediaItem* m = id ? snap->findMedia(*id) : nullptr;
    if (!m)
        return {};
    const Rate rate = rateOf(*m);
    QVariantMap out{
        {QStringLiteral("key"), key},
        {QStringLiteral("kind"), m->isSubclip() ? QStringLiteral("subclip") : QStringLiteral("media")},
        {QStringLiteral("name"), text(m->name)},
        {QStringLiteral("comment"), text(m->comment)},
        {QStringLiteral("description"), text(m->description)},
        {QStringLiteral("label"), m->colorIndex},
        {QStringLiteral("path"), QDir::toNativeSeparators(text(m->locator.path))},
        {QStringLiteral("offline"), !m->isGenerator() && m->locator.offline},
        {QStringLiteral("posterText"), m->posterFrame ? text(ticksToTimecode(*m->posterFrame, rate, false)) : QString()},
    };
    if (m->subclip) {
        const SubclipSpec& s = *m->subclip;
        const auto last = ticksToFrames(s.range.end(), rate, Rounding::Ceil).value_or(1) - 1;
        out.insert(QStringLiteral("inText"), text(ticksToTimecode(s.range.start, rate, false)));
        out.insert(QStringLiteral("outText"), text(framesToTimecode(std::max<std::int64_t>(0, last), rate, false)));
        out.insert(QStringLiteral("restricted"), s.restricted);
        const MediaItem* master = snap->findMedia(s.master);
        out.insert(QStringLiteral("master"), master ? text(master->name) : QString());
    }
    return out;
}


// ---- Clip metadata and XMP (0.2, P.3) --------------------------------------------------------------------------------

bool LibraryBridge::setMetadata(const QStringList& keys, const QString& field, const QVariant& value) {
    const std::vector<MediaId> media = mediaOf(keys);
    if (media.empty())
        return false;
    edit::MediaAttributeChange change;
    const std::string textValue = value.toString().toStdString();
    const char* label = nullptr;
    if (field == QStringLiteral("comment")) {
        change.comment = textValue;
        label = QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Comment");
    } else if (field == QStringLiteral("description")) {
        change.description = textValue;
        label = QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Description");
    } else if (field == QStringLiteral("scene")) {
        change.scene = textValue;
        label = QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Scene");
    } else if (field == QStringLiteral("shot")) {
        change.shot = textValue;
        label = QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Shot");
    } else if (field == QStringLiteral("take")) {
        change.take = textValue;
        label = QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Take");
    } else if (field == QStringLiteral("logNote")) {
        change.logNote = textValue;
        label = QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Log Note");
    } else if (field == QStringLiteral("client")) {
        change.client = textValue;
        label = QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Client");
    } else if (field == QStringLiteral("tapeName")) {
        change.tapeName = textValue;
        label = QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Tape Name");
    } else if (field == QStringLiteral("good")) {
        change.good = value.toBool();
        label = QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Good");
    } else if (field == QStringLiteral("status")) {
        change.status = textValue;
        label = QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Status");
    } else if (field == QStringLiteral("cameraAngle")) {
        change.cameraAngle = textValue;
        label = QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Camera Angle");
    } else if (field == QStringLiteral("cameraLabel")) {
        change.cameraLabel = textValue;
        label = QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Camera Label");
    } else if (field == QStringLiteral("cameraModel")) {
        change.cameraModel = textValue;
        label = QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Camera Model");
    } else if (field == QStringLiteral("shotDate")) {
        change.shotDate = textValue;
        label = QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Shot Date");
    } else if (field == QStringLiteral("shotLocation")) {
        change.shotLocation = textValue;
        label = QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Shot Location");
    } else {
        return false;
    }
    return apply(label, [&](const ProjectState& s) { return edit::setMediaAttributes(s, media, change); });
}

QVariantMap LibraryBridge::clipMetadata(const QString& key) const {
    const auto id = parseMediaKey(key);
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const MediaItem* m = id && snap ? snap->findMedia(*id) : nullptr;
    if (!m)
        return {};
    const Rate rate = rateOf(*m);
    const QString kind = m->isSubclip()           ? tr("Subclip")
                         : m->isGenerator()        ? tr("Made in Wabcut")
                         : m->info.isStillImage    ? tr("Still image")
                         : m->info.hasVideo()      ? (m->info.hasAudio() ? tr("Video and audio") : tr("Video"))
                                                   : tr("Audio");
    const Ticks length = m->subclip ? m->subclip->range.duration : m->info.duration;
    return {
        {QStringLiteral("key"), key},
        {QStringLiteral("name"), text(m->name)},
        {QStringLiteral("type"), kind},
        {QStringLiteral("label"), m->colorIndex},
        {QStringLiteral("labelName"), libraryLabelName(m->colorIndex)},
        {QStringLiteral("fps"), libraryFrameRateText(*m)},
        {QStringLiteral("resolution"), libraryResolutionText(*m)},
        {QStringLiteral("audio"), libraryAudioText(*m)},
        {QStringLiteral("duration"), m->info.isStillImage || m->isGenerator() ? QString() : text(ticksToTimecode(length, rate, false))},
        {QStringLiteral("scene"), text(m->metadata.scene)},
        {QStringLiteral("shot"), text(m->metadata.shot)},
        {QStringLiteral("take"), text(m->metadata.take)},
        {QStringLiteral("logNote"), text(m->metadata.logNote)},
        {QStringLiteral("client"), text(m->metadata.client)},
        {QStringLiteral("good"), m->metadata.good},
        {QStringLiteral("tapeName"), m->timecode ? text(m->timecode->reelName) : QString()},
        {QStringLiteral("status"), text(m->metadata.status)},
        {QStringLiteral("cameraAngle"), text(m->metadata.cameraAngle)},
        {QStringLiteral("cameraLabel"), text(m->metadata.cameraLabel)},
        {QStringLiteral("cameraModel"), text(m->metadata.cameraModel)},
        {QStringLiteral("shotDate"), text(m->metadata.shotDate)},
        {QStringLiteral("shotLocation"), text(m->metadata.shotLocation)},
        {QStringLiteral("importedFrom"), m->importSource ? QDir::toNativeSeparators(text(m->importSource->project)) : QString()},
        {QStringLiteral("comment"), text(m->comment)},
        {QStringLiteral("description"), text(m->description)},
        {QStringLiteral("path"), m->isGenerator() ? QString() : QDir::toNativeSeparators(text(m->locator.path))},
        {QStringLiteral("offline"), !m->isGenerator() && m->locator.offline},
        {QStringLiteral("hasFile"), !m->isGenerator() && !m->locator.path.empty()},
    };
}

QVariantMap LibraryBridge::fileMetadata(const QString& key) const {
    const auto id = parseMediaKey(key);
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const MediaItem* m = id && snap ? snap->findMedia(*id) : nullptr;
    QVariantMap out{{QStringLiteral("xmpSource"), QString()}, {QStringLiteral("xmpFile"), QString()},
                    {QStringLiteral("xmp"), QVariantList{}}, {QStringLiteral("tags"), QVariantList{}},
                    {QStringLiteral("error"), QString()}};
    if (!m || m->isGenerator())
        return out;
    const auto file = resolveMediaPath(m->locator, m_controller->projectFile());
    if (!file) {
        out.insert(QStringLiteral("error"), tr("The file cannot be found."));
        return out;
    }
    if (auto packet = interchange::readMediaXmp(*file); packet && *packet) {
        out.insert(QStringLiteral("xmpSource"), (*packet)->source == interchange::XmpSource::Sidecar ? QStringLiteral("sidecar")
                                                                                                     : QStringLiteral("embedded"));
        out.insert(QStringLiteral("xmpFile"), QDir::toNativeSeparators(QString::fromStdU16String((*packet)->file.u16string())));
        QVariantList list;
        for (const interchange::XmpProperty& p : (*packet)->properties) {
            list.append(QVariantMap{{QStringLiteral("group"), text(p.group)}, {QStringLiteral("label"), text(p.label)},
                                    {QStringLiteral("name"), text(p.prefix + ":" + p.name)}, {QStringLiteral("value"), text(p.value)},
                                    {QStringLiteral("ns"), text(p.ns)}, {QStringLiteral("prefix"), text(p.prefix)},
                                    {QStringLiteral("property"), text(p.name)},
                                    // A structure's fields ("startTimecode/timeValue") are shown, not edited.
                                    {QStringLiteral("editable"), p.name.find('/') == std::string::npos}});
        }
        out.insert(QStringLiteral("xmp"), list);
    }
    // Where an edit of the File section goes (0.2, P.3): the file itself when the preference is on and it can take it.
    const bool intoFile = m_embedXmp && interchange::canEmbedXmp(*file);
    out.insert(QStringLiteral("writesTo"), intoFile ? QStringLiteral("file") : QStringLiteral("sidecar"));
    out.insert(QStringLiteral("writesToText"),
               intoFile ? tr("Edits are written into %1.").arg(QString::fromStdU16String(file->filename().u16string()))
                        : tr("Edits are written to %1.").arg(QString::fromStdU16String(interchange::xmpSidecarPath(*file).filename().u16string())));
    if (auto tags = media::readContainerTags(pathToUtf8(*file))) {
        QVariantList list;
        for (const media::ContainerTag& t : *tags) {
            const QString scope = t.stream < 0 ? tr("File") : tr("Stream %1 (%2)").arg(t.stream).arg(text(t.kind));
            list.append(QVariantMap{{QStringLiteral("scope"), scope}, {QStringLiteral("key"), text(t.key)},
                                    {QStringLiteral("value"), text(t.value)}});
        }
        out.insert(QStringLiteral("tags"), list);
    }
    return out;
}

int LibraryBridge::readXmp(const QStringList& keys, bool overwrite) {
    if (!m_controller)
        return 0;
    const ProjectSnapshot snap = m_controller->snapshot();
    const std::filesystem::path projectFile = m_controller->projectFile();
    // Read the files first (outside the edit), then apply every item's fields in one undo step.
    std::vector<std::pair<MediaId, edit::MediaAttributeChange>> changes;
    for (const MediaId id : mediaOf(keys)) {
        const MediaItem* item = snap->findMedia(id);
        if (!item || item->isGenerator())
            continue;
        const auto file = resolveMediaPath(item->locator, projectFile);
        if (!file)
            continue;
        auto packet = interchange::readMediaXmp(*file);
        if (!packet || !*packet)
            continue;
        const interchange::XmpClipFields f = interchange::clipFieldsFromXmp((*packet)->properties);
        edit::MediaAttributeChange change;
        const auto pick = [&](const std::optional<std::string>& value, const std::string& current, std::optional<std::string>& into) {
            if (value && (overwrite || current.empty()))
                into = *value;
        };
        pick(f.scene, item->metadata.scene, change.scene);
        pick(f.shot, item->metadata.shot, change.shot);
        pick(f.take, item->metadata.take, change.take);
        pick(f.logNote, item->metadata.logNote, change.logNote);
        pick(f.client, item->metadata.client, change.client);
        pick(f.comment, item->comment, change.comment);
        pick(f.description, item->description, change.description);
        pick(f.tapeName, item->timecode ? item->timecode->reelName : std::string(), change.tapeName);
        pick(f.cameraAngle, item->metadata.cameraAngle, change.cameraAngle);
        pick(f.cameraLabel, item->metadata.cameraLabel, change.cameraLabel);
        pick(f.cameraModel, item->metadata.cameraModel, change.cameraModel);
        pick(f.shotDate, item->metadata.shotDate, change.shotDate);
        pick(f.shotLocation, item->metadata.shotLocation, change.shotLocation);
        if (f.good && (overwrite || !item->metadata.good))
            change.good = *f.good;
        changes.emplace_back(id, std::move(change));
    }
    if (changes.empty()) {
        setMessage(tr("No XMP metadata was found in these files."));
        return 0;
    }
    const bool ok = apply(QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Read XMP Metadata"), [&](const ProjectState& s) {
        ProjectState state = s;
        for (const auto& [id, change] : changes) {
            auto next = edit::setMediaAttributes(state, std::vector<MediaId>{id}, change);
            if (!next)
                return std::expected<ProjectState, Error>(std::unexpected(next.error()));
            state = std::move(*next);
        }
        return std::expected<ProjectState, Error>(std::move(state));
    });
    if (!ok)
        return 0;
    setMessage(tr("Read XMP metadata of %n item(s).", nullptr, static_cast<int>(changes.size())));
    return static_cast<int>(changes.size());
}

int LibraryBridge::writeXmp(const QStringList& keys) {
    if (!m_controller)
        return 0;
    const ProjectSnapshot snap = m_controller->snapshot();
    int written = 0;
    int embedded = 0;
    QString problem;
    for (const MediaId id : mediaOf(keys)) {
        const MediaItem* item = snap->findMedia(id);
        if (!item || item->isGenerator())
            continue;
        const auto file = resolveMediaPath(item->locator, m_controller->projectFile());
        if (!file) {
            problem = tr("“%1” is offline: its file cannot be found.").arg(text(item->name));
            continue;
        }
        MediaItem located = *item;
        located.locator.path = pathToUtf8(*file);
        if (m_embedXmp && interchange::canEmbedXmp(*file)) {
            // 0.2 (P.3): into the file itself (the preference), through a checked copy.
            if (auto done = interchange::embedXmp(*file, interchange::linkedXmpEdits(located)); !done) {
                problem = text(done.error().message);
                continue;
            }
            ++written;
            ++embedded;
            refreshFingerprint(mediaKey(id), *file);
            continue;
        }
        if (auto done = interchange::writeXmpSidecar(located))
            ++written;
        else
            problem = text(done.error().message);
    }
    if (!problem.isEmpty())
        setMessage(problem);
    else if (embedded > 0)
        setMessage(tr("Wrote XMP into %n file(s).", nullptr, embedded) +
                   (written > embedded ? QStringLiteral(" ") + tr("%n XMP sidecar(s).", nullptr, written - embedded) : QString()));
    else
        setMessage(tr("Wrote %n XMP sidecar(s).", nullptr, written));
    return written;
}

void LibraryBridge::setEmbedXmp(bool on) {
    if (on == m_embedXmp)
        return;
    m_embedXmp = on;
    if (m_persistent)
        QSettings().setValue(QStringLiteral("metadata/embedXmp"), on);
    emit embedXmpChanged();
}

void LibraryBridge::refreshFingerprint(const QString& key, const std::filesystem::path& file) {
    // Writing into the file changed its bytes, so its fingerprint: the item is pointed at the same file again, or the
    // next open would take it for another file.
    if (m_relinker)
        m_relinker(key, file);
}

QVariantList LibraryBridge::xmpFieldChoices() const {
    QVariantList out;
    for (const interchange::XmpProperty& p : interchange::knownXmpProperties()) {
        if (p.name.find('/') != std::string::npos)
            continue;
        out.append(QVariantMap{{QStringLiteral("ns"), text(p.ns)}, {QStringLiteral("prefix"), text(p.prefix)},
                               {QStringLiteral("property"), text(p.name)}, {QStringLiteral("name"), text(p.prefix + ":" + p.name)},
                               {QStringLiteral("label"), text(p.label)}, {QStringLiteral("group"), text(p.group)}});
    }
    return out;
}

bool LibraryBridge::setFileMetadata(const QString& key, const QString& ns, const QString& prefix, const QString& property,
                                    const QString& value) {
    const auto id = parseMediaKey(key);
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const MediaItem* item = id && snap ? snap->findMedia(*id) : nullptr;
    if (!item || item->isGenerator())
        return false;
    if (ns.isEmpty() || property.isEmpty() || property.contains(QLatin1Char('/'))) {
        setMessage(tr("This property cannot be set here."));
        return false;
    }
    const auto file = resolveMediaPath(item->locator, m_controller->projectFile());
    if (!file) {
        setMessage(tr("“%1” is offline: its file cannot be found.").arg(text(item->name)));
        return false;
    }
    const interchange::XmpEdit edit{ns.toStdString(), prefix.toStdString(), property.toStdString(), value.toStdString()};
    const bool intoFile = m_embedXmp && interchange::canEmbedXmp(*file);
    // A linked property (Scene, Shot, the camera fields, ...) is the clip metadata too: both in one undo step.
    interchange::XmpProperty asRead;
    asRead.ns = edit.ns;
    asRead.prefix = edit.prefix;
    asRead.name = edit.name;
    asRead.value = edit.value;
    const interchange::XmpClipFields linked = interchange::clipFieldsFromXmp({asRead});
    if (intoFile) {
        if (auto done = interchange::embedXmp(*file, {edit}); !done) {
            setMessage(text(done.error().message)); // the file is as it was
            return false;
        }
        // A sidecar next to it is read first: keep it saying the same.
        std::error_code ec;
        if (std::filesystem::exists(interchange::xmpSidecarPath(*file), ec))
            (void)interchange::editXmpSidecar(*file, {edit});
    } else if (auto done = interchange::editXmpSidecar(*file, {edit}); !done) {
        setMessage(text(done.error().message));
        return false;
    }
    const bool gesture = intoFile || !linked.empty();
    if (gesture)
        m_controller->beginGesture(QString::fromLatin1(QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Edit File Metadata")));
    if (!linked.empty()) {
        edit::MediaAttributeChange change;
        change.scene = linked.scene;
        change.shot = linked.shot;
        change.take = linked.take;
        change.logNote = linked.logNote;
        change.client = linked.client;
        change.good = linked.good;
        change.tapeName = linked.tapeName;
        change.comment = linked.comment;
        change.description = linked.description;
        change.cameraAngle = linked.cameraAngle;
        change.cameraLabel = linked.cameraLabel;
        change.cameraModel = linked.cameraModel;
        change.shotDate = linked.shotDate;
        change.shotLocation = linked.shotLocation;
        const std::vector<MediaId> one{*id};
        apply(QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Edit File Metadata"),
              [&](const ProjectState& s) { return edit::setMediaAttributes(s, one, change); });
    }
    if (intoFile)
        refreshFingerprint(key, *file);
    if (gesture)
        m_controller->endGesture();
    setMessage(intoFile ? tr("Written into %1.").arg(QString::fromStdU16String(file->filename().u16string()))
                        : tr("Written to %1.").arg(QString::fromStdU16String(interchange::xmpSidecarPath(*file).filename().u16string())));
    emit fileMetadataChanged(key);
    return true;
}

// ---- Search Bins (0.2, P.3) ------------------------------------------------------------------------------------------

namespace {

BinSearch searchFromMap(const QVariantMap& m) {
    BinSearch s;
    s.text = m.value(QStringLiteral("text")).toString().trimmed().toStdString();
    s.field = m.value(QStringLiteral("field")).toString().toStdString();
    s.type = m.value(QStringLiteral("type")).toString().toStdString();
    s.label = m.value(QStringLiteral("label"), -1).toInt();
    s.good = m.value(QStringLiteral("good")).toBool();
    s.offline = m.value(QStringLiteral("offline")).toBool();
    s.generated = m.value(QStringLiteral("generated")).toBool();
    s.transcribed = m.value(QStringLiteral("transcribed")).toBool();
    s.proxy = m.value(QStringLiteral("proxy")).toBool();
    s.unused = m.value(QStringLiteral("unused")).toBool();
    s.duplicates = m.value(QStringLiteral("duplicates")).toBool();
    s.from = m.value(QStringLiteral("from")).toString().trimmed().toStdString();
    s.to = m.value(QStringLiteral("to")).toString().trimmed().toStdString();
    return s;
}

QVariantMap searchToMap(const BinSearch& s) {
    return {{QStringLiteral("text"), text(s.text)},         {QStringLiteral("field"), text(s.field)},
            {QStringLiteral("type"), text(s.type)},         {QStringLiteral("label"), s.label},
            {QStringLiteral("good"), s.good},               {QStringLiteral("offline"), s.offline},
            {QStringLiteral("generated"), s.generated},     {QStringLiteral("transcribed"), s.transcribed},
            {QStringLiteral("proxy"), s.proxy},             {QStringLiteral("unused"), s.unused},
            {QStringLiteral("duplicates"), s.duplicates},   {QStringLiteral("from"), text(s.from)},
            {QStringLiteral("to"), text(s.to)}};
}

} // namespace

BinId LibraryBridge::targetBin() const {
    const Bin* b = m_bin.isValid() && m_lastSnapshot ? m_lastSnapshot->findBin(m_bin) : nullptr;
    return b && b->isSearch() ? b->parent : m_bin;
}

bool LibraryBridge::searchBinOpen() const {
    const Bin* b = m_bin.isValid() && m_lastSnapshot ? m_lastSnapshot->findBin(m_bin) : nullptr;
    return b && b->isSearch();
}

QVariantMap LibraryBridge::currentSearch() const {
    const Bin* b = m_bin.isValid() && m_lastSnapshot ? m_lastSnapshot->findBin(m_bin) : nullptr;
    return b && b->isSearch() ? searchToMap(*b->search) : QVariantMap{};
}

QVariantList LibraryBridge::searchFields() const {
    QVariantList out{QVariantMap{{QStringLiteral("id"), QString()}, {QStringLiteral("title"), tr("All")}}};
    const std::pair<const char*, LibraryColumn> fields[] = {
        {"name", LibraryColumn::Name},         {"comment", LibraryColumn::Comment}, {"description", LibraryColumn::Description},
        {"scene", LibraryColumn::Scene},       {"shot", LibraryColumn::Shot},       {"take", LibraryColumn::Take},
        {"logNote", LibraryColumn::LogNote},   {"client", LibraryColumn::Client},   {"tapeName", LibraryColumn::TapeName},
        {"status", LibraryColumn::Status},     {"cameraAngle", LibraryColumn::CameraAngle},
        {"cameraLabel", LibraryColumn::CameraLabel}, {"cameraModel", LibraryColumn::CameraModel},
        {"shotDate", LibraryColumn::ShotDate}, {"shotLocation", LibraryColumn::ShotLocation},
        {"path", LibraryColumn::Path}};
    for (const auto& [id, column] : fields)
        out.append(QVariantMap{{QStringLiteral("id"), QLatin1String(id)}, {QStringLiteral("title"), libraryColumnTitle(column)}});
    return out;
}

QString LibraryBridge::newSearchBin(const QString& name, const QVariantMap& search) {
    if (!m_controller)
        return {};
    const BinSearch query = searchFromMap(search);
    if (const auto problem = binSearchProblem(query)) {
        setMessage(text(*problem));
        return {};
    }
    QString chosen = name.trimmed();
    if (chosen.isEmpty())
        chosen = query.text.empty() ? tr("Search Bin") : text(query.text);
    BinId created;
    const BinId parent = targetBin();
    if (!apply(QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Create Search Bin"), [&](const ProjectState& s) {
            auto added = edit::addSearchBin(s, chosen.toStdString(), query, parent);
            if (!added)
                return std::expected<ProjectState, Error>(std::unexpected(added.error()));
            created = added->id;
            return std::expected<ProjectState, Error>(std::move(added->state));
        }))
        return {};
    const QString key = binKey(created);
    select(key, Replace);
    return key;
}

QString LibraryBridge::createSearchBin(const QString& name) {
    const BinSearch query = m_filter.toSearch();
    QVariantMap map = searchToMap(query);
    const QString key = newSearchBin(name, map);
    if (key.isEmpty())
        return {};
    // Premiere: the search moves into the new bin; the Find bar is clear again.
    if (m_filter.active()) {
        m_filter = {};
        emit filterChanged();
        refresh(true);
    }
    select(key, Replace);
    return key;
}

bool LibraryBridge::editSearchBin(const QString& key, const QVariantMap& search) {
    const auto bin = parseBinKey(key);
    if (!bin)
        return false;
    const BinSearch query = searchFromMap(search);
    return apply(QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Edit Search Bin"),
                 [&](const ProjectState& s) { return edit::setBinSearch(s, *bin, query); });
}

QVariantMap LibraryBridge::searchOf(const QString& key) const {
    const auto bin = parseBinKey(key);
    const Bin* b = bin && m_lastSnapshot ? m_lastSnapshot->findBin(*bin) : nullptr;
    return b && b->isSearch() ? searchToMap(*b->search) : QVariantMap{};
}

// ---- Freeform view (0.2, P.3) ----------------------------------------------------------------------------------------

namespace {

std::optional<FreeformPlacement> placementOfKey(const QString& key) {
    if (const auto b = parseBinKey(key))
        return FreeformPlacement{FreeformPlacement::Kind::Bin, b->value};
    if (const auto m = parseMediaKey(key))
        return FreeformPlacement{FreeformPlacement::Kind::Media, m->value};
    if (const auto q = parseSequenceKey(key))
        return FreeformPlacement{FreeformPlacement::Kind::Sequence, q->value};
    return std::nullopt;
}

QString keyOfPlacement(const FreeformPlacement& p) {
    switch (p.kind) {
    case FreeformPlacement::Kind::Bin:
        return binKey(BinId{p.id});
    case FreeformPlacement::Kind::Sequence:
        return sequenceKey(SequenceId{p.id});
    case FreeformPlacement::Kind::Media:
        break;
    }
    return mediaKey(MediaId{p.id});
}

} // namespace

qreal LibraryBridge::freeformCellWidth() const { return edit::kFreeformCellWidth; }
qreal LibraryBridge::freeformCellHeight() const { return edit::kFreeformCellHeight; }
qreal LibraryBridge::freeformCardWidth() const { return edit::kFreeformCardWidth; }

bool LibraryBridge::freeformAvailable() const { return !m_filter.active() && !searchBinOpen(); }

void LibraryBridge::setFreeformLines(const QStringList& lines) {
    QStringList next = lines.mid(0, 2);
    while (next.size() < 2)
        next << QString();
    if (next == m_freeformLines)
        return;
    m_freeformLines = next;
    saveSettings();
    emit freeformLinesChanged();
}

void LibraryBridge::refreshFreeform() {
    QVariantMap positions;
    qreal scale = 1.0;
    QStringList layouts;
    const ProjectSnapshot snap = m_lastSnapshot;
    if (snap && m_view == QStringLiteral("freeform")) {
        const FreeformView* view = freeformAvailable() ? snap->findFreeform(m_bin) : nullptr;
        if (view) {
            scale = view->current.scale;
            for (const FreeformLayout& l : view->layouts)
                layouts << text(l.name);
        }
        // Placed items keep their place (back to front); the others follow on the grid below them, in list order.
        QHash<QString, int> listed;
        for (int row = 0; row < m_model->rowCount(); ++row)
            listed.insert(m_model->keyOf(row), row);
        int z = 0;
        double bottom = 0.0;
        if (view) {
            for (const FreeformPlacement& p : view->current.items) {
                const QString key = keyOfPlacement(p);
                if (!listed.contains(key) || positions.contains(key))
                    continue;
                positions.insert(key, QVariantMap{{QStringLiteral("x"), p.x}, {QStringLiteral("y"), p.y},
                                                  {QStringLiteral("size"), p.size}, {QStringLiteral("z"), z++}});
                bottom = std::max(bottom, p.y + edit::kFreeformCellHeight * scale * p.size);
            }
        }
        const double cellW = edit::kFreeformCellWidth * scale;
        const double cellH = edit::kFreeformCellHeight * scale;
        const double top = positions.isEmpty() ? 0.0 : std::ceil(bottom / cellH) * cellH;
        constexpr int kColumns = 5;
        int next = 0;
        for (int row = 0; row < m_model->rowCount(); ++row) {
            const QString key = m_model->keyOf(row);
            if (positions.contains(key))
                continue;
            positions.insert(key, QVariantMap{{QStringLiteral("x"), (next % kColumns) * cellW},
                                              {QStringLiteral("y"), top + (next / kColumns) * cellH},
                                              {QStringLiteral("size"), 1.0}, {QStringLiteral("z"), z++}});
            ++next;
        }
    }
    if (positions == m_freeform && qFuzzyCompare(scale, m_freeformScale) && layouts == m_freeformLayouts)
        return;
    m_freeform = std::move(positions);
    m_freeformScale = scale;
    m_freeformLayouts = std::move(layouts);
    emit freeformChanged();
}

QString LibraryBridge::cellText(const QString& key, const QString& column) const {
    const auto id = libraryColumnFromId(column);
    const int row = m_model->rowOf(key);
    return id && row >= 0 ? m_model->cellText(row, *id) : QString();
}

QStringList LibraryBridge::freeformStackOf(const QString& key) const {
    const QVariantMap me = m_freeform.value(key).toMap();
    if (me.isEmpty())
        return {};
    const auto rectOf = [&](const QVariantMap& p) {
        const double w = edit::kFreeformCardWidth * m_freeformScale * p.value(QStringLiteral("size")).toDouble();
        const double h = edit::kFreeformCellHeight * m_freeformScale * p.value(QStringLiteral("size")).toDouble() * 0.9;
        return QRectF(p.value(QStringLiteral("x")).toDouble(), p.value(QStringLiteral("y")).toDouble(), w, h);
    };
    const QRectF mine = rectOf(me);
    std::vector<std::pair<int, QString>> stack;
    for (auto it = m_freeform.begin(); it != m_freeform.end(); ++it) {
        const QVariantMap p = it.value().toMap();
        if (rectOf(p).intersects(mine))
            stack.emplace_back(p.value(QStringLiteral("z")).toInt(), it.key());
    }
    std::sort(stack.begin(), stack.end());
    QStringList out;
    for (auto& [z, k] : stack)
        out << k;
    return out;
}

/// The arrangement as the view shows it: every listed item with its place, back to front (the auto-placed ones become
/// placed with the first edit).
static FreeformArrangement shownArrangement(const QVariantMap& positions, qreal scale) {
    std::vector<std::pair<int, FreeformPlacement>> items;
    for (auto it = positions.begin(); it != positions.end(); ++it) {
        auto p = placementOfKey(it.key());
        if (!p)
            continue;
        const QVariantMap v = it.value().toMap();
        p->x = v.value(QStringLiteral("x")).toDouble();
        p->y = v.value(QStringLiteral("y")).toDouble();
        p->size = v.value(QStringLiteral("size"), 1.0).toDouble();
        items.emplace_back(v.value(QStringLiteral("z")).toInt(), *p);
    }
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    FreeformArrangement out;
    out.scale = scale;
    for (auto& [z, p] : items)
        out.items.push_back(p);
    return out;
}

bool LibraryBridge::moveFreeform(const QStringList& keys, qreal dx, qreal dy) {
    if (!freeformAvailable() || keys.isEmpty() || !std::isfinite(dx) || !std::isfinite(dy))
        return false;
    FreeformArrangement a = shownArrangement(m_freeform, m_freeformScale);
    std::vector<FreeformPlacement> moved;
    for (const QString& key : keys) {
        const auto p = placementOfKey(key);
        if (!p)
            continue;
        const auto it = std::find_if(a.items.begin(), a.items.end(),
                                     [&](const FreeformPlacement& x) { return x.kind == p->kind && x.id == p->id; });
        if (it == a.items.end())
            continue;
        FreeformPlacement next = *it;
        next.x = std::clamp(next.x + dx, -kFreeformMaxCoordinate, kFreeformMaxCoordinate);
        next.y = std::clamp(next.y + dy, -kFreeformMaxCoordinate, kFreeformMaxCoordinate);
        a.items.erase(it);
        moved.push_back(next);
    }
    if (moved.empty())
        return false;
    for (const FreeformPlacement& p : moved)
        a.items.push_back(p); // to the front: dropped over others, they stack on top
    const BinId bin = m_bin;
    return apply(QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Move in Freeform View"),
                 [&](const ProjectState& s) { return edit::setFreeformArrangement(s, bin, a); });
}

bool LibraryBridge::setFreeformSize(const QStringList& keys, qreal size) {
    if (!freeformAvailable() || keys.isEmpty())
        return false;
    FreeformArrangement a = shownArrangement(m_freeform, m_freeformScale);
    const double clamped = std::clamp(std::isfinite(size) ? size : 1.0, kFreeformMinSize, kFreeformMaxSize);
    for (FreeformPlacement& p : a.items) {
        if (keys.contains(keyOfPlacement(p)))
            p.size = clamped;
    }
    const BinId bin = m_bin;
    return apply(QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Clip Size"),
                 [&](const ProjectState& s) { return edit::setFreeformArrangement(s, bin, a); });
}

bool LibraryBridge::setFreeformScale(qreal scale) {
    if (!freeformAvailable())
        return false;
    FreeformArrangement a = shownArrangement(m_freeform, m_freeformScale);
    a.scale = std::clamp(std::isfinite(scale) ? scale : 1.0, kFreeformMinSize, kFreeformMaxSize);
    if (qFuzzyCompare(a.scale, m_freeformScale))
        return true;
    const BinId bin = m_bin;
    return apply(QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Clip Size"),
                 [&](const ProjectState& s) { return edit::setFreeformArrangement(s, bin, a); });
}

bool LibraryBridge::freeformResetToGrid(const QString& by, int columns, qreal width) {
    if (!freeformAvailable() || !m_lastSnapshot)
        return false;
    std::vector<std::pair<std::size_t, LibraryModel::Row>> rows;
    for (std::size_t i = 0; i < m_model->rows().size(); ++i)
        rows.emplace_back(i, m_model->rows()[i]);
    const auto nameOf = [](const LibraryModel::Row& r) {
        return text(r.kind == LibraryModel::Row::Kind::Bin        ? r.bin->name
                    : r.kind == LibraryModel::Row::Kind::Sequence ? r.sequence->name
                                                                  : r.media->name);
    };
    const auto keyOf = [&](const LibraryModel::Row& r) -> QVariant {
        const MediaItem* m = r.kind == LibraryModel::Row::Kind::Media ? r.media : nullptr;
        if (by == QStringLiteral("label"))
            return m ? m->colorIndex : -1;
        if (by == QStringLiteral("type"))
            return LibraryModel::kindOf(r);
        if (by == QStringLiteral("duration"))
            return r.kind == LibraryModel::Row::Kind::Sequence ? static_cast<qlonglong>(r.sequence->duration.value)
                   : m ? static_cast<qlonglong>(m->info.duration.value) : 0LL;
        if (by == QStringLiteral("date"))
            return m ? static_cast<qlonglong>(m->info.modifiedTime) : 0LL;
        return nameOf(r).toLower();
    };
    std::stable_sort(rows.begin(), rows.end(), [&](const auto& a, const auto& b) {
        if (a.second.kind != b.second.kind)
            return a.second.kind < b.second.kind;
        const QPartialOrdering o = QVariant::compare(keyOf(a.second), keyOf(b.second));
        if (o == QPartialOrdering::Less)
            return true;
        if (o == QPartialOrdering::Greater)
            return false;
        return QString::compare(nameOf(a.second), nameOf(b.second), Qt::CaseInsensitive) < 0;
    });
    std::vector<FreeformPlacement> items;
    for (const auto& [i, r] : rows) {
        if (auto p = placementOfKey(LibraryModel::keyFor(r)))
            items.push_back(*p);
    }
    const double cellW = edit::kFreeformCellWidth * m_freeformScale;
    const int perRow = columns > 0 ? columns : std::max(1, static_cast<int>(std::floor(std::max(0.0, double(width)) / cellW)));
    const FreeformArrangement a = edit::freeformGrid(items, perRow, m_freeformScale);
    const BinId bin = m_bin;
    return apply(QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Reset to Grid"),
                 [&](const ProjectState& s) { return edit::setFreeformArrangement(s, bin, a); });
}

bool LibraryBridge::freeformAlignToGrid(bool selectionOnly) {
    if (!freeformAvailable())
        return false;
    std::vector<FreeformPlacement> selected;
    if (selectionOnly) {
        for (const QString& key : selection()) {
            if (auto p = placementOfKey(key))
                selected.push_back(*p);
        }
    }
    const FreeformArrangement a = edit::alignFreeformToGrid(shownArrangement(m_freeform, m_freeformScale), selected);
    const BinId bin = m_bin;
    return apply(QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Align to Grid"),
                 [&](const ProjectState& s) { return edit::setFreeformArrangement(s, bin, a); });
}

bool LibraryBridge::saveFreeformLayout(const QString& name) {
    if (!freeformAvailable())
        return false;
    const FreeformArrangement a = shownArrangement(m_freeform, m_freeformScale);
    const BinId bin = m_bin;
    const std::string chosen = name.trimmed().toStdString();
    return apply(QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Save Layout"), [&](const ProjectState& s) {
        auto placed = edit::setFreeformArrangement(s, bin, a);
        if (!placed)
            return placed;
        return edit::saveFreeformLayout(*placed, bin, chosen);
    });
}

bool LibraryBridge::restoreFreeformLayout(const QString& name) {
    if (!freeformAvailable())
        return false;
    const BinId bin = m_bin;
    return apply(QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Restore Layout"),
                 [&](const ProjectState& s) { return edit::restoreFreeformLayout(s, bin, name.toStdString()); });
}

bool LibraryBridge::removeFreeformLayout(const QString& name) {
    if (!freeformAvailable())
        return false;
    const BinId bin = m_bin;
    return apply(QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Delete Layout"),
                 [&](const ProjectState& s) { return edit::removeFreeformLayout(s, bin, name.toStdString()); });
}

QString LibraryBridge::cycleFreeformStack(const QString& key, int direction) {
    const QStringList stack = freeformStackOf(key);
    if (stack.size() < 2 || !freeformAvailable())
        return stack.isEmpty() ? QString() : stack.back();
    FreeformArrangement a = shownArrangement(m_freeform, m_freeformScale);
    // Forward: the backmost item of the stack comes to the front; back: the front one goes behind the others.
    const QString moving = direction >= 0 ? stack.front() : stack.back();
    const auto p = placementOfKey(moving);
    const auto it = std::find_if(a.items.begin(), a.items.end(),
                                 [&](const FreeformPlacement& x) { return p && x.kind == p->kind && x.id == p->id; });
    if (it == a.items.end())
        return stack.back();
    const FreeformPlacement item = *it;
    a.items.erase(it);
    if (direction >= 0) {
        a.items.push_back(item);
    } else {
        // Just behind the backmost item of the stack.
        const auto back = placementOfKey(stack.front());
        const auto at = std::find_if(a.items.begin(), a.items.end(),
                                     [&](const FreeformPlacement& x) { return back && x.kind == back->kind && x.id == back->id; });
        a.items.insert(at, item);
    }
    const BinId bin = m_bin;
    apply(QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Stack Order"),
          [&](const ProjectState& s) { return edit::setFreeformArrangement(s, bin, a); });
    const QStringList now = freeformStackOf(key);
    return now.isEmpty() ? QString() : now.back();
}

// ---- Subclips ----------------------------------------------------------------------------------------------------------

bool LibraryBridge::canMakeSubclip() const {
    if (!m_source || !m_controller || !m_source->loaded())
        return false;
    const auto id = parseMediaKey(m_source->mediaKeyText());
    const MediaItem* m = id ? m_controller->snapshot()->findMedia(*id) : nullptr;
    return m && !m->isGenerator() && !m->info.isStillImage;
}

QString LibraryBridge::importImageSequence(const QUrl& firstFrame, double fps) {
    if (!m_controller)
        return {};
    const QString local = firstFrame.isLocalFile() ? firstFrame.toLocalFile() : firstFrame.toString();
    if (local.isEmpty()) {
        setMessage(tr("Choose the first image of the sequence."));
        return {};
    }
    // The same command the assistant, wabcut-cli and MCP run; this host reads the files for it.
    command::EnginePolicy policy;
    policy.services = std::make_shared<command::ImageSequenceServices>(
        [projectFile = m_controller->projectFile()](const std::filesystem::path& frame, Rate rate) {
            return editor::describeImageSequence(frame, rate, projectFile);
        });
    command::CommandEngine engine(m_controller->session(), ai::edit::editingCommands(), std::move(policy));
    command::CommandRequest request;
    request.command = "media.importImageSequence";
    request.params = command::Json{{"path", QDir::fromNativeSeparators(local).toStdString()}};
    if (fps > 0.0)
        request.params["frameRate"] = fps;
    if (targetBin().isValid())
        request.params["bin"] = targetBin().value;
    const command::CommandResult result = engine.execute(request);
    if (const QString error = commandErrorText(result); !error.isEmpty()) {
        setMessage(error);
        return {};
    }
    setMessage({});
    const QString key = result.createdIds.empty() ? QString() : mediaKey(MediaId{result.createdIds.front()});
    if (!key.isEmpty())
        select(key, Replace);
    return key;
}

// ---- Layered files and the Media Browser (0.2) ------------------------------------------------------------------

namespace {

// P1 round (#39): shared with Get Properties (defined there).
QVariantMap audioMap(const AudioStreamInfo& a);
QVariantMap infoMap(const MediaItem& item, int files, qint64 bytes);

std::string localPathOf(const QUrl& url) {
    const QString local = url.isLocalFile() ? url.toLocalFile() : url.toString();
    return QDir::fromNativeSeparators(local).toStdString();
}

QVariantMap toVariantMap(const command::Json& json);

QVariant toVariant(const command::Json& json) {
    if (json.is_boolean()) {
        return json.get<bool>();
    }
    if (json.is_number_integer()) {
        return QVariant::fromValue(json.get<qint64>());
    }
    if (json.is_number()) {
        return json.get<double>();
    }
    if (json.is_string()) {
        return QString::fromStdString(json.get<std::string>());
    }
    if (json.is_array()) {
        QVariantList list;
        for (const command::Json& v : json) {
            list.push_back(toVariant(v));
        }
        return list;
    }
    if (json.is_object()) {
        return toVariantMap(json);
    }
    return {};
}

QVariantMap toVariantMap(const command::Json& json) {
    QVariantMap map;
    for (auto it = json.begin(); it != json.end(); ++it) {
        map.insert(QString::fromStdString(it.key()), toVariant(it.value()));
    }
    return map;
}

} // namespace

bool LibraryBridge::isLayeredFile(const QUrl& file) const {
    const std::string path = localPathOf(file);
    const std::size_t dot = path.rfind('.');
    return dot != std::string::npos && isLayeredFileExtension(std::string_view(path).substr(dot));
}

QVariantMap LibraryBridge::layeredFileInfo(const QUrl& file) {
    if (!m_controller)
        return {};
    command::EnginePolicy policy;
    policy.services = editor::mediaFormatServices(m_controller->projectFile());
    command::CommandEngine engine(m_controller->session(), ai::edit::editingCommands(), std::move(policy));
    command::CommandRequest request;
    request.command = "media.layeredFileInfo";
    request.params = command::Json{{"path", localPathOf(file)}};
    const command::CommandResult result = engine.execute(request);
    if (const QString error = commandErrorText(result); !error.isEmpty()) {
        setMessage(error);
        return {};
    }
    setMessage({});
    return toVariantMap(result.data);
}

QString LibraryBridge::importLayeredFile(const QUrl& file, const QVariantMap& options) {
    if (!m_controller)
        return {};
    command::EnginePolicy policy;
    policy.services = editor::mediaFormatServices(m_controller->projectFile());
    command::CommandEngine engine(m_controller->session(), ai::edit::editingCommands(), std::move(policy));
    command::CommandRequest request;
    request.command = "media.importLayeredFile";
    request.params = command::Json{{"path", localPathOf(file)}};
    const QString as = options.value(QStringLiteral("as"), QStringLiteral("mergeAll")).toString();
    request.params["as"] = as.toStdString();
    request.params["footage"] = options.value(QStringLiteral("footage"), QStringLiteral("document")).toString().toStdString();
    if (options.contains(QStringLiteral("layers"))) {
        command::Json layers = command::Json::array();
        for (const QVariant& v : options.value(QStringLiteral("layers")).toList())
            layers.push_back(v.toInt());
        request.params["layers"] = std::move(layers);
    }
    if (m_bin.isValid())
        request.params["bin"] = m_bin.value;
    const command::CommandResult result = engine.execute(request);
    if (const QString error = commandErrorText(result); !error.isEmpty()) {
        setMessage(error);
        return {};
    }
    setMessage({});
    if (result.data.contains("media")) {
        const QString key = mediaKey(MediaId{result.data["media"].get<std::uint64_t>()});
        select(key, Replace);
        return key;
    }
    return result.data.contains("bin") ? binKey(BinId{result.data["bin"].get<std::uint64_t>()}) : QString();
}

QVariantList LibraryBridge::browserPlaces() const {
    QVariantList out;
    QStringList listed; // the folders listed so far (recent ones already there are not listed again)
    const auto sameFolder = [](const QString& a, const QString& b) {
#ifdef Q_OS_WIN
        return QDir::cleanPath(a).compare(QDir::cleanPath(b), Qt::CaseInsensitive) == 0;
#else
        return QDir::cleanPath(a) == QDir::cleanPath(b);
#endif
    };
    // `kind` (0.4): "folder" (Videos, Home), "capture" (a game capture tool's folder), "recent" (a dialog import's
    // folder), "drive". `checked`: the caller already knows the folder may be offered (recent ones: a folder on a
    // network share is not asked about, ProjectLifecycle::recentImportFolders).
    const auto add = [&](const QString& name, const QString& path, const QString& kind, bool checked = false) {
        if (path.isEmpty() || (!checked && !QFileInfo(path).isDir()))
            return;
        const QString clean = QDir::fromNativeSeparators(path);
        out.push_back(QVariantMap{{QStringLiteral("name"), name}, {QStringLiteral("path"), clean}, {QStringLiteral("kind"), kind}});
        listed << clean;
    };
    const QString videos = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    add(tr("Videos"), videos, QStringLiteral("folder"));
    add(tr("Home"), QDir::homePath(), QStringLiteral("folder"));
    // 0.4: the game capture tools' folders that exist (Game Bar, ShadowPlay, every OBS profile's recordings).
    const std::vector<editor::CaptureFolder> captures =
        editor::captureFolders(videos.isEmpty() ? std::filesystem::path() : std::filesystem::path(videos.toStdU16String()),
                               editor::obsConfigFolder());
    const auto obsCount = std::ranges::count_if(captures, [](const editor::CaptureFolder& c) { return c.tool == editor::CaptureTool::Obs; });
    for (const editor::CaptureFolder& capture : captures) {
        const QString path = QString::fromStdU16String(capture.path.u16string());
        switch (capture.tool) {
        case editor::CaptureTool::GameBar:
            add(tr("Game Bar captures"), path, QStringLiteral("capture"));
            break;
        case editor::CaptureTool::ShadowPlay:
            add(tr("NVIDIA ShadowPlay"), path, QStringLiteral("capture"));
            break;
        case editor::CaptureTool::Obs:
            add(obsCount > 1 ? tr("OBS recordings (%1)").arg(QString::fromStdString(capture.profile)) : tr("OBS recordings"), path,
                QStringLiteral("capture"));
            break;
        }
    }
    // The drives, listed last; read first so a recent folder that is a drive's root is not listed twice.
    std::vector<std::pair<QString, QString>> drives; // name, root
    for (const QStorageInfo& volume : QStorageInfo::mountedVolumes()) {
        if (!volume.isValid() || !volume.isReady())
            continue;
        const QString label = volume.displayName().isEmpty() ? volume.rootPath() : volume.displayName();
        drives.emplace_back(label == volume.rootPath() ? label : QStringLiteral("%1 (%2)").arg(label, QDir::toNativeSeparators(volume.rootPath())),
                            volume.rootPath());
    }
    // 0.4: the folders the last dialog imports came from (newest first), unless listed above or a drive's root (fix
    // round 1: "E:\" gave "Recent: " with no name). A root that is not a listed drive is named by its path.
    int recent = 0;
    for (const QString& folder : ProjectLifecycle::recentImportFolders()) {
        if (recent >= 5)
            break;
        const QString clean = QDir::fromNativeSeparators(folder);
        if (std::ranges::any_of(listed, [&](const QString& l) { return sameFolder(l, clean); }) ||
            std::ranges::any_of(drives, [&](const auto& d) { return sameFolder(d.second, clean); }))
            continue;
        const QString name = QDir(folder).dirName();
        add(tr("Recent: %1").arg(name.isEmpty() ? QDir::toNativeSeparators(folder) : name), folder, QStringLiteral("recent"), true);
        ++recent;
    }
    for (const auto& [name, root] : drives)
        add(name, root, QStringLiteral("drive"));
    return out;
}

QVariantList LibraryBridge::browseFolder(const QString& folder) {
    QVariantList out;
    const std::filesystem::path dir = pathFromUtf8(QDir::fromNativeSeparators(folder).toStdString());
    std::error_code ec;
    if (folder.isEmpty() || !std::filesystem::is_directory(dir, ec)) {
        setMessage(tr("This folder cannot be opened."));
        return out;
    }
    setMessage({});
    for (const editor::BrowserEntry& e : editor::browseFolder(dir)) {
        // 0.4: `url` too, so QML never builds file URLs by hand (folder rows import through ProjectFile.importFiles).
        QVariantMap entry{{QStringLiteral("folder"), e.folder},
                          {QStringLiteral("name"), QString::fromStdString(e.name)},
                          {QStringLiteral("path"), QString::fromStdString(pathToUtf8(e.path))},
                          {QStringLiteral("url"), QUrl::fromLocalFile(QString::fromStdString(pathToUtf8(e.path)))}};
        if (e.clip) {
            entry.insert(QStringLiteral("card"), QString::fromUtf8(editor::displayName(e.clip->card)));
            entry.insert(QStringLiteral("files"), static_cast<int>(e.clip->files.size()));
            entry.insert(QStringLiteral("spanned"), e.clip->spanned());
            entry.insert(QStringLiteral("audioChannels"), static_cast<int>(e.clip->audio.size()));
            entry.insert(QStringLiteral("bytes"), QVariant::fromValue(static_cast<qint64>(e.clip->bytes())));
            entry.insert(QStringLiteral("thumbnail"), ThumbnailImageProvider::urlFor(pathToUtf8(e.path)));
            // Hover scrub: ten frames through the clip in one image.
            entry.insert(QStringLiteral("strip"), ThumbnailImageProvider::stripUrlFor(pathToUtf8(e.path), kBrowserStripFrames));
        }
        out.push_back(entry);
    }
    return out;
}

QVariantMap LibraryBridge::browserFileInfo(const QString& file) {
    const QUrl url(file);
    const QString local = QDir::fromNativeSeparators(url.isLocalFile() ? url.toLocalFile() : file);
    const std::filesystem::path path = pathFromUtf8(local.toStdString());
    const editor::CameraClip clip = editor::cameraClipOf(path);
    auto described = editor::describeCameraClip(clip, m_controller ? m_controller->projectFile() : std::filesystem::path());
    if (!described || described->empty()) {
        setMessage(described ? tr("%1 is not media Wabcut reads.").arg(local) : text(described.error().message));
        return {};
    }
    const MediaItem& item = described->front();
    // P1 round (#39): the fields Get Properties shows too (infoMap), plus where the clip is.
    QVariantMap out = infoMap(item, static_cast<int>(clip.files.size()), static_cast<qint64>(clip.bytes()));
    out.insert(QStringLiteral("path"), local);
    out.insert(QStringLiteral("card"), QString::fromUtf8(editor::displayName(clip.card)));
    // A P2 clip's sound is in files of its own: each is an item of the clip.
    QVariantList audio;
    for (const MediaItem& part : *described) {
        for (const AudioStreamInfo& a : part.info.audio)
            audio.push_back(audioMap(a));
    }
    out.insert(QStringLiteral("audio"), audio);
    setMessage({});
    return out;
}

QString LibraryBridge::parentFolder(const QString& folder) const {
    const std::filesystem::path dir = pathFromUtf8(QDir::fromNativeSeparators(folder).toStdString());
    const std::filesystem::path parent = dir.parent_path();
    if (parent.empty() || parent == dir)
        return {};
    return QString::fromStdString(pathToUtf8(parent));
}

QString LibraryBridge::importCameraClips(const QStringList& files, const QString& copyTo) {
    const auto imported = runCameraImport(files, copyTo);
    if (!imported || imported->empty())
        return {};
    // A P2 clip opens as its merged clip; any other as its item.
    const command::Json& first = imported->front();
    const QString key = first.contains("merged") ? sequenceKey(SequenceId{first["merged"].get<std::uint64_t>()})
                                                 : mediaKey(MediaId{first["media"].get<std::uint64_t>()});
    select(key, Replace);
    return key;
}

QString LibraryBridge::importFromBrowser(const QStringList& files) {
    const auto imported = ingestClips(files);
    if (!imported || imported->empty())
        return {};
    const command::Json& first = imported->front();
    const QString key = first.contains("merged") ? sequenceKey(SequenceId{first["merged"].get<std::uint64_t>()})
                                                 : mediaKey(MediaId{first["media"].get<std::uint64_t>()});
    select(key, Replace);
    return key;
}

std::optional<command::Json> LibraryBridge::ingestClips(const QStringList& files) {
    // 0.7 (card 022): "copyProxies" copies the files into the folder, imports the copies and makes proxies of those.
    const bool copyProxies = m_ingest && m_ingestAction == QLatin1String("copyProxies");
    const bool copying = (m_ingest && m_ingestAction == QLatin1String("copy")) || copyProxies;
    const bool transcoding = m_ingest && m_ingestAction == QLatin1String("transcode");
    const bool proxies = (m_ingest && m_ingestAction == QLatin1String("proxies")) || copyProxies;
    if ((copying || transcoding) && m_ingestFolder.isEmpty()) {
        setMessage(tr("Choose the folder Ingest copies or transcodes the clips to (Ingest Settings)."));
        return std::nullopt;
    }
    if (transcoding && !m_export) {
        setMessage(tr("Transcoding needs the export queue."));
        return std::nullopt;
    }
    auto imported = runCameraImport(files, copying ? m_ingestFolder : QString());
    if (!imported || !m_controller)
        return imported;
    std::vector<MediaId> media;
    QStringList keys;
    for (const command::Json& entry : *imported) {
        if (entry.contains("media") && entry["media"].get<std::uint64_t>() != 0) {
            media.push_back(MediaId{entry["media"].get<std::uint64_t>()});
            keys << mediaKey(media.back());
        }
    }
    if (proxies) {
        const int queued = m_readiness ? m_readiness->generateProxies(keys) : 0;
        if (copyProxies)
            setMessage(queued > 0 ? tr("Copied to %1; creating proxies of %n clip(s).", "", queued)
                                        .arg(QDir::toNativeSeparators(m_ingestFolder))
                                  : tr("Copied to %1; there is nothing to make a proxy of.")
                                        .arg(QDir::toNativeSeparators(m_ingestFolder)));
        else
            setMessage(queued > 0 ? tr("Creating proxies of %n clip(s).", "", queued)
                                  : tr("Imported; there is nothing to make a proxy of."));
    } else if (transcoding) {
        const std::filesystem::path folder = pathFromUtf8(m_ingestFolder.toStdString());
        const ProjectSnapshot snap = m_controller->snapshot();
        int queued = 0;
        QString problem;
        for (const MediaId id : media) {
            const MediaItem* item = snap->findMedia(id);
            if (!item)
                continue;
            auto job = m_export->queueTranscode(*item, m_ingestPreset, folder);
            if (!job) {
                problem = tr("%1 cannot be transcoded: %2").arg(text(item->name), ExportController::plainError(job.error()));
                continue;
            }
            m_transcodes[job->jobId] = PendingTranscode{id, job->output};
            ++queued;
        }
        setMessage(!problem.isEmpty() ? problem
                                      : tr("Transcoding %n clip(s) into %1: each switches to its new file when the "
                                           "export queue has made it.", "", queued)
                                            .arg(QDir::toNativeSeparators(m_ingestFolder)));
    }
    return imported;
}

void LibraryBridge::transcodeFinished(const QString& jobId, bool succeeded, const QString& message) {
    const auto it = m_transcodes.find(jobId);
    if (it == m_transcodes.end())
        return;
    const PendingTranscode done = it->second;
    m_transcodes.erase(it);
    if (!m_controller)
        return;
    const MediaItem* item = m_controller->snapshot()->findMedia(done.media);
    if (!item)
        return; // removed meanwhile: the file stays where it was made
    if (!succeeded) {
        setMessage(tr("Transcoding %1 failed (%2): it keeps its original file.").arg(text(item->name), message));
        return;
    }
    auto replacement = editor::describeMediaFile(done.output, m_controller->projectFile());
    if (!replacement) {
        setMessage(tr("The transcoded file of %1 cannot be read: it keeps its original file.").arg(text(item->name)));
        return;
    }
    const MediaId media = done.media;
    if (apply(QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Use Transcoded Media"),
              [&](const ProjectState& s) { return edit::replaceFootage(s, media, *replacement, false); }))
        setMessage(tr("%1 now plays its transcoded file.").arg(text(item->name)));
}

std::optional<command::Json> LibraryBridge::runCameraImport(const QStringList& files, const QString& copyTo) {
    if (!m_controller || files.isEmpty())
        return std::nullopt;
    command::EnginePolicy policy;
    // P0 round: a recording's chapter markers come in as clip markers, as with File > Import (Preferences > Media).
    policy.services = editor::mediaFormatServices(m_controller->projectFile(), EditingBridge::chapterMarkersSetting());
    command::CommandEngine engine(m_controller->session(), ai::edit::editingCommands(), std::move(policy));
    command::CommandRequest request;
    request.command = "media.importCameraClips";
    command::Json list = command::Json::array();
    for (const QString& f : files)
        list.push_back(QDir::fromNativeSeparators(f).toStdString());
    request.params = command::Json{{"files", std::move(list)}};
    if (m_bin.isValid())
        request.params["bin"] = m_bin.value;
    if (!copyTo.isEmpty()) {
        const QUrl url(copyTo);
        const QString folder = url.isLocalFile() ? url.toLocalFile() : copyTo;
        request.params["copyTo"] = QDir::fromNativeSeparators(folder).toStdString();
    }
    const command::CommandResult result = engine.execute(request);
    if (const QString error = commandErrorText(result); !error.isEmpty()) {
        setMessage(error);
        return std::nullopt;
    }
    setMessage({});
    // P0 round: a toast says how many chapter markers came in (ProjectFile.importFiles' chapterSummary). A notice, not
    // `message` (fix round 1): the Media Browser shows `message` as its empty / unreadable text.
    if (const int markers = result.data.value("markers", 0); markers > 0)
        emit notice(markers == 1 ? tr("1 chapter marker became a clip marker.")
                                 : tr("%1 chapter markers became clip markers.").arg(markers));
    return std::optional<command::Json>(std::in_place, result.data.value("imported", command::Json::array()));
}

void LibraryBridge::setIngest(bool on) {
    if (on == m_ingest)
        return;
    m_ingest = on;
    saveSettings();
    emit ingestChanged();
}

void LibraryBridge::setIngestAction(const QString& action) {
    if (action == m_ingestAction ||
        (action != QLatin1String("copy") && action != QLatin1String("transcode") && action != QLatin1String("proxies") &&
         action != QLatin1String("copyProxies")))
        return;
    m_ingestAction = action;
    saveSettings();
    emit ingestChanged();
}

void LibraryBridge::setIngestFolder(const QString& folder) {
    const QUrl url(folder);
    const QString local = folder.isEmpty() ? QString() : QDir::fromNativeSeparators(url.isLocalFile() ? url.toLocalFile() : folder);
    if (local == m_ingestFolder)
        return;
    m_ingestFolder = local;
    saveSettings();
    emit ingestChanged();
}

void LibraryBridge::setIngestPreset(const QString& preset) {
    if (preset.isEmpty() || preset == m_ingestPreset)
        return;
    m_ingestPreset = preset;
    saveSettings();
    emit ingestChanged();
}

void LibraryBridge::setFieldEditing(bool editing) {
    if (editing == m_fieldEditing)
        return;
    m_fieldEditing = editing;
    emit fieldEditingChanged();
}

QString LibraryBridge::makeSubclip(const QString& name, bool restricted) {
    if (!canMakeSubclip()) {
        setMessage(tr("Open a clip in the Source Monitor and mark an In and Out first."));
        return {};
    }
    return makeSubclipOf(m_source->mediaKeyText(), name, restricted);
}

QString LibraryBridge::makeSubclipOf(const QString& key, const QString& name, bool restricted) {
    const auto id = parseMediaKey(key);
    if (!id || !m_controller)
        return {};
    const MediaItem* m = m_controller->snapshot()->findMedia(*id);
    if (!m)
        return {};
    // Between the marks; an unset mark is the start or end of what the item may use.
    const edit::SourceMarks marks = edit::storedSourceMarks(*m);
    const SourceBounds bounds = m->sourceBounds({m->info.hasVideo() ? StreamKind::Video : StreamKind::Audio, 0});
    const Ticks in = marks.in.value_or(bounds.start);
    const Ticks out = marks.out.value_or(bounds.end.value_or(m->info.duration));
    if (out <= in) {
        setMessage(tr("Mark an In before the Out to make a subclip."));
        return {};
    }
    MediaId created;
    const BinId bin = targetBin();
    const std::string chosen = name.trimmed().toStdString();
    if (!apply(QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Make Subclip"), [&](const ProjectState& s) {
            auto made = edit::makeSubclip(s, *id, TimeRange::fromStartEnd(in, out), chosen, restricted, bin);
            if (!made)
                return std::expected<ProjectState, Error>(std::unexpected(made.error()));
            created = made->id;
            return std::expected<ProjectState, Error>(std::move(made->state));
        }))
        return {};
    const QString created_ = mediaKey(created);
    select(created_, Replace);
    if (const MediaItem* sub = m_controller->snapshot()->findMedia(created))
        setMessage(tr("Made subclip “%1”.").arg(text(sub->name)));
    return created_;
}

bool LibraryBridge::editSubclip(const QString& key, const QString& inText, const QString& outText, bool restricted) {
    const auto id = parseMediaKey(key);
    const MediaItem* m = id && m_controller ? m_controller->snapshot()->findMedia(*id) : nullptr;
    if (!m || !m->subclip)
        return false;
    const Rate rate = rateOf(*m);
    const auto in = parseTimecodeFrames(inText.toStdString(), rate, false);
    const auto out = parseTimecodeFrames(outText.toStdString(), rate, false);
    if (!in || !out || *out < *in) {
        setMessage(tr("Type an In and an Out timecode, the Out on or after the In."));
        return false;
    }
    const auto start = framesToTicks(*in, rate, Rounding::Floor);
    const auto end = framesToTicks(*out + 1, rate, Rounding::Floor); // the Out frame is included
    if (!start || !end)
        return false;
    return apply(QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Edit Subclip"), [&](const ProjectState& s) {
        return edit::setSubclip(s, *id, TimeRange::fromStartEnd(*start, *end), restricted);
    });
}

// ---- Proxies for the selection or a bin (P1 round, #44) -----------------------------------------------------------

namespace {

/// What can have a proxy: video (not stills, titles, generators or sound alone), as ProxyManager makes them.
bool proxyable(const MediaItem& m) { return !m.isGenerator() && !m.info.isStillImage && m.info.hasVideo(); }

bool proxyBeingMade(const QString& state) {
    return state == QLatin1String("generating") || state == QLatin1String("paused") || state == QLatin1String("queued");
}

} // namespace

QStringList LibraryBridge::mediaInBin(const QString& bin, bool recursive) const {
    QStringList out;
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    if (!snap)
        return out;
    BinId target;
    if (!bin.isEmpty()) {
        const auto parsed = parseBinKey(bin);
        if (!parsed || !snap->findBin(*parsed))
            return out;
        target = *parsed;
    }
    std::unordered_set<std::uint64_t> bins{target.value}; // 0 = the root
    for (bool grew = recursive; grew;) {
        grew = false;
        for (const Bin& b : snap->bins) {
            if (bins.contains(b.parent.value) && !b.isSearch() && bins.insert(b.id.value).second)
                grew = true;
        }
    }
    for (const MediaPtr& m : snap->media) {
        if (bins.contains(m->bin.value))
            out << mediaKey(m->id);
    }
    return out;
}

QStringList LibraryBridge::proxyTargets(const QString& key) const {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    if (!snap)
        return {};
    QStringList keys;
    if (parseBinKey(key)) {
        keys = mediaInBin(key, true);
    } else if (m_selection.contains(key)) {
        std::vector<const MediaItem*> media;
        std::vector<const Sequence*> sequences;
        collectUnder(selection(), media, sequences);
        for (const MediaItem* m : media)
            keys << mediaKey(m->id);
    } else {
        keys << key;
    }
    QStringList out;
    for (const QString& k : keys) {
        const auto id = parseMediaKey(k);
        const MediaItem* m = id ? snap->findMedia(*id) : nullptr;
        if (m && proxyable(*m))
            out << k;
    }
    return out;
}

QVariantMap LibraryBridge::proxyPlan(const QString& key) const {
    int create = 0;
    int ready = 0;
    int making = 0;
    int deletable = 0;
    const QStringList targets = proxyTargets(key);
    for (const QString& k : targets) {
        const auto id = parseMediaKey(k);
        const auto it = id ? m_status.find(id->value) : m_status.end();
        const QString state = it != m_status.end() ? it->second.proxyState : QString();
        if (state == QLatin1String("ready"))
            ++ready;
        else if (proxyBeingMade(state))
            ++making;
        else
            ++create; // none, failed or out of date
        if (!state.isEmpty() && state != QLatin1String("failed"))
            ++deletable;
    }
    return {{QStringLiteral("targets"), static_cast<int>(targets.size())},
            {QStringLiteral("create"), create},
            {QStringLiteral("ready"), ready},
            {QStringLiteral("making"), making},
            {QStringLiteral("deletable"), deletable}};
}

QString LibraryBridge::createProxies(const QString& key) {
    const QStringList targets = proxyTargets(key);
    if (targets.isEmpty())
        return tr("Nothing here can have a proxy: proxies are made for video.");
    QStringList wanted;
    int ready = 0;
    int making = 0;
    for (const QString& k : targets) {
        const auto it = m_status.find(parseMediaKey(k)->value);
        const QString state = it != m_status.end() ? it->second.proxyState : QString();
        if (state == QLatin1String("ready"))
            ++ready;
        else if (proxyBeingMade(state))
            ++making;
        else
            wanted << k;
    }
    const int queued = wanted.isEmpty() || !m_readiness ? 0 : m_readiness->generateProxies(wanted);
    QStringList parts{tr("%n queued", nullptr, queued)};
    if (ready > 0)
        parts << tr("%n with a proxy already", nullptr, ready);
    if (making > 0)
        parts << tr("%n being made", nullptr, making);
    if (wanted.size() > queued)
        parts << tr("%n not queued (the Proxy column says why)", nullptr, static_cast<int>(wanted.size()) - queued);
    const QString result = parts.join(QStringLiteral(", ")) + QLatin1Char('.');
    emit notice(result);
    return result;
}

QString LibraryBridge::remakeProxies(const QString& key) {
    const QStringList targets = proxyTargets(key);
    QStringList ready;
    QStringList again;
    for (const QString& k : targets) {
        const auto it = m_status.find(parseMediaKey(k)->value);
        const QString state = it != m_status.end() ? it->second.proxyState : QString();
        if (proxyBeingMade(state))
            continue;
        if (state == QLatin1String("ready") || state == QLatin1String("stale"))
            ready << k;
        again << k;
    }
    if (!m_readiness || again.isEmpty())
        return tr("Nothing to remake.");
    if (!ready.isEmpty())
        m_readiness->deleteProxies(ready);
    const int queued = m_readiness->generateProxies(again);
    const QString result = tr("Remaking %n prox(ies).", nullptr, queued);
    emit notice(result);
    return result;
}

QString LibraryBridge::deleteProxies(const QString& key) {
    QStringList doomed;
    for (const QString& k : proxyTargets(key)) {
        const auto it = m_status.find(parseMediaKey(k)->value);
        const QString state = it != m_status.end() ? it->second.proxyState : QString();
        if (!state.isEmpty() && state != QLatin1String("failed"))
            doomed << k;
    }
    if (!m_readiness || doomed.isEmpty())
        return tr("No proxies to delete.");
    const int deleted = m_readiness->deleteProxies(doomed);
    const QString result = tr("Deleted or cancelled %n prox(ies).", nullptr, std::max(deleted, static_cast<int>(doomed.size())));
    emit notice(result);
    return result;
}

// ---- Get Properties (P1 round, #39) --------------------------------------------------------------------------------

namespace {

QVariantMap videoMap(const VideoStreamInfo& v) {
    return {{QStringLiteral("codec"), text(v.codec)},
            {QStringLiteral("profile"), text(v.profile)},
            {QStringLiteral("width"), v.width},
            {QStringLiteral("height"), v.height},
            {QStringLiteral("frameRate"), std::round(v.frameRate.toDouble() * 1000.0) / 1000.0},
            {QStringLiteral("variableFrameRate"), v.variableFrameRate},
            {QStringLiteral("pixelFormat"), text(v.pixelFormat)},
            {QStringLiteral("bitDepth"), v.bitDepth},
            {QStringLiteral("chroma"), text(v.chromaSubsampling)},
            {QStringLiteral("fieldOrder"), v.fieldOrder == FieldOrder::UpperFirst   ? QStringLiteral("upper")
                                           : v.fieldOrder == FieldOrder::LowerFirst ? QStringLiteral("lower")
                                                                                    : QStringLiteral("progressive")},
            {QStringLiteral("alpha"), v.alpha != AlphaMode::None},
            {QStringLiteral("rotation"), v.rotationDegrees},
            // 0.7 (card "Dolby Vision 8.4 and HDR10+"): the Dolby Vision configuration the container declares, and HDR10+.
            {QStringLiteral("dolbyVision"), v.dolbyVision ? QVariant(QVariantMap{{QStringLiteral("profile"), text(v.dolbyVision->name())},
                                                                                 {QStringLiteral("level"), v.dolbyVision->level},
                                                                                 {QStringLiteral("blCompatibilityId"), v.dolbyVision->blCompatibilityId},
                                                                                 {QStringLiteral("rpu"), v.dolbyVision->rpuPresent},
                                                                                 {QStringLiteral("enhancementLayer"), v.dolbyVision->elPresent}})
                                                          : QVariant()},
            {QStringLiteral("hdr10Plus"), v.hdr10Plus},
            {QStringLiteral("frames"), QVariant::fromValue(static_cast<qint64>(v.frameCount))}};
}

QVariantMap audioMap(const AudioStreamInfo& a) {
    QStringList channelNames; // 0.7 (card 062): a poly WAV's channel names ("" for a channel with none)
    for (const std::string& name : a.channelNames)
        channelNames << text(name);
    return {{QStringLiteral("codec"), text(a.codec)},
            {QStringLiteral("channels"), a.channels},
            {QStringLiteral("layout"), text(a.channelLayout)},
            {QStringLiteral("sampleRate"), static_cast<int>(std::lround(a.sampleRate.toDouble()))},
            {QStringLiteral("title"), text(a.title)},
            {QStringLiteral("channelNames"), channelNames}};
}

/// What browserFileInfo and mediaProperties share about one item: { name, container, duration (timecode), seconds,
/// still, files, bytes, modified, startTimecode, video {...} (no picture: absent), audio [...] }.
QVariantMap infoMap(const MediaItem& item, int files, qint64 bytes) {
    const MediaInfo& info = item.info;
    const Rate rate = info.hasVideo() && info.video.front().frameRate.num > 0 ? info.video.front().frameRate : Rate{30, 1};
    const Ticks length = item.subclip ? item.subclip->range.duration : info.duration;
    QVariantMap out{{QStringLiteral("name"), text(item.name)},
                    {QStringLiteral("files"), files},
                    {QStringLiteral("bytes"), QVariant::fromValue(bytes)},
                    {QStringLiteral("container"), text(info.container)},
                    {QStringLiteral("duration"), text(ticksToTimecode(length, rate, false))},
                    {QStringLiteral("seconds"), length.toSeconds()},
                    {QStringLiteral("still"), info.isStillImage}};
    if (info.modifiedTime > 0)
        out.insert(QStringLiteral("modified"), QDateTime::fromSecsSinceEpoch(info.modifiedTime).toString(Qt::ISODate));
    if (info.startTimecode)
        out.insert(QStringLiteral("startTimecode"), text(ticksToTimecode(*info.startTimecode, rate, false)));
    if (info.hasVideo())
        out.insert(QStringLiteral("video"), videoMap(info.video.front()));
    QVariantList audio;
    for (const AudioStreamInfo& a : info.audio)
        audio.push_back(audioMap(a));
    out.insert(QStringLiteral("audio"), audio);
    return out;
}

QString numberText(double value) { return QString::number(value, 'g', 6); }

} // namespace

QVariantMap LibraryBridge::mediaProperties(const QString& key) const {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    if (!snap)
        return {};
    if (const auto q = parseSequenceKey(key)) {
        const Sequence* s = snap->findSequence(*q);
        if (!s)
            return {};
        return {{QStringLiteral("key"), key},
                {QStringLiteral("kind"), QStringLiteral("sequence")},
                {QStringLiteral("name"), text(s->name)},
                {QStringLiteral("width"), s->width},
                {QStringLiteral("height"), s->height},
                {QStringLiteral("frameRate"), std::round(s->frameRate.toDouble() * 1000.0) / 1000.0},
                {QStringLiteral("duration"), text(ticksToTimecode(s->duration, s->frameRate, s->dropFrame))},
                {QStringLiteral("seconds"), s->duration.toSeconds()},
                {QStringLiteral("videoTracks"), static_cast<int>(s->videoTracks.size())},
                {QStringLiteral("audioTracks"), static_cast<int>(s->audioTracks.size())}};
    }
    if (const auto b = parseBinKey(key)) {
        const Bin* bin = snap->findBin(*b);
        if (!bin)
            return {};
        std::vector<const MediaItem*> media;
        std::vector<const Sequence*> sequences;
        collectUnder({key}, media, sequences);
        qint64 bytes = 0;
        for (const MediaItem* m : media)
            bytes += libraryFileBytes(*m);
        return {{QStringLiteral("key"), key},
                {QStringLiteral("kind"), QStringLiteral("bin")},
                {QStringLiteral("name"), text(bin->name)},
                {QStringLiteral("media"), static_cast<int>(media.size())},
                {QStringLiteral("sequences"), static_cast<int>(sequences.size())},
                {QStringLiteral("bytes"), QVariant::fromValue(bytes)}};
    }
    const auto id = parseMediaKey(key);
    const MediaItem* m = id ? snap->findMedia(*id) : nullptr;
    if (!m)
        return {};
    const qint64 bytes = libraryFileBytes(*m);
    QVariantMap out = infoMap(*m, 1, bytes);
    out.insert(QStringLiteral("key"), key);
    out.insert(QStringLiteral("kind"), LibraryModel::kindOf(LibraryModel::Row{LibraryModel::Row::Kind::Media, id->value,
                                                                                nullptr, nullptr, m}));
    out.insert(QStringLiteral("offline"), !m->isGenerator() && m->locator.offline);
    if (!m->isGenerator())
        out.insert(QStringLiteral("path"), QDir::toNativeSeparators(text(m->locator.path)));
    const double seconds = m->info.duration.toSeconds();
    if (bytes > 0 && seconds > 0.0 && !m->info.isStillImage && !m->isGenerator())
        out.insert(QStringLiteral("bitrate"), static_cast<double>(bytes) * 8.0 / seconds);
    if (const auto it = m_status.find(id->value); it != m_status.end() && !it->second.proxyState.isEmpty())
        out.insert(QStringLiteral("proxy"), it->second.proxyState);
    int uses = 0;
    for (const SequencePtr& s : snap->sequences) {
        s->forEachTrack([&](const Track& t) {
            for (const Clip& c : t.clips)
                uses += clipUsesMedia(c, m->id) ? 1 : 0; // 0.6: an audition's other takes count (card #74)
        });
    }
    out.insert(QStringLiteral("uses"), uses);
    if (m->subclip) {
        const QVariantMap d = details(key);
        out.insert(QStringLiteral("master"), d.value(QStringLiteral("master")));
        out.insert(QStringLiteral("inText"), d.value(QStringLiteral("inText")));
        out.insert(QStringLiteral("outText"), d.value(QStringLiteral("outText")));
    }
    return out;
}

namespace {

/// 0.7 (card 103): the name and the plain sentence for one entry of media::newerCodecSupport().
QString codecSupportName(const QString& id) {
    return id == QLatin1String("apv")        ? LibraryBridge::tr("APV (Advanced Professional Video)")
           : id == QLatin1String("hevc-422") ? LibraryBridge::tr("HEVC 4:2:2")
           : id == QLatin1String("av1-422")  ? LibraryBridge::tr("AV1 4:2:2")
                                             : LibraryBridge::tr("MV-HEVC (spatial video)");
}

QString codecSupportText(const media::CodecSupport& c) {
    const QString id = QString::fromStdString(c.id);
    if (!c.decodes()) {
        return LibraryBridge::tr("This build cannot play it.");
    }
    if (id == QLatin1String("apv")) {
        return c.exports() ? LibraryBridge::tr("Plays and exports.")
                           : LibraryBridge::tr("Plays. Decode only: Wabcut cannot export APV; export as ProRes, DNxHR or H.264.");
    }
    if (id == QLatin1String("hevc-422")) {
        if (!c.exports()) {
            return LibraryBridge::tr("Plays (8, 10 and 12-bit). Decode only: no HEVC encoder here takes 4:2:2; export as ProRes 422 or H.264.");
        }
        return c.encoderEightBitOnly
                   ? LibraryBridge::tr("Plays (8, 10 and 12-bit). Exports 4:2:2 at 8 bits only; 10-bit 4:2:2 HEVC is decode-only (export "
                       "it as ProRes 422 or H.264 4:2:2 10-bit).")
                   : LibraryBridge::tr("Plays and exports 4:2:2 (8 and 10-bit).");
    }
    if (id == QLatin1String("av1-422")) {
        return c.exports() ? LibraryBridge::tr("Plays and exports.")
                           : LibraryBridge::tr("Plays. Decode only: the AV1 encoders here write 4:2:0; export as ProRes, H.264 4:2:2 10-bit "
                               "or HEVC.");
    }
    return LibraryBridge::tr("Plays the base view as ordinary HEVC; the second eye is not used. Decode only: there is no MV-HEVC encoder.");
}

} // namespace

QVariantList LibraryBridge::codecSupport() const {
    QVariantList list;
    for (const media::CodecSupport& c : media::newerCodecSupport()) {
        const QString id = QString::fromStdString(c.id);
        list.append(QVariantMap{{QStringLiteral("id"), id},
                                {QStringLiteral("name"), codecSupportName(id)},
                                {QStringLiteral("plays"), c.decodes()},
                                {QStringLiteral("exports"), c.exports()},
                                {QStringLiteral("text"), codecSupportText(c)}});
    }
    return list;
}

QVariantList LibraryBridge::propertyLines(const QVariantMap& info) const {
    QVariantList lines;
    const auto add = [&](const QString& label, const QString& value) {
        if (!value.trimmed().isEmpty())
            lines.append(QVariantMap{{QStringLiteral("label"), label}, {QStringLiteral("value"), value}});
    };
    if (info.isEmpty() || !info.contains(QStringLiteral("name")))
        return lines;
    const QString kind = info.value(QStringLiteral("kind")).toString();
    const QString dot = QStringLiteral(" · ");
    if (kind == QLatin1String("sequence")) {
        add(tr("Type"), tr("Sequence"));
        add(tr("Frame size"), QStringLiteral("%1 × %2").arg(info.value(QStringLiteral("width")).toInt())
                                  .arg(info.value(QStringLiteral("height")).toInt()));
        add(tr("Frame rate"), tr("%1 fps").arg(numberText(info.value(QStringLiteral("frameRate")).toDouble())));
        add(tr("Duration"), info.value(QStringLiteral("duration")).toString());
        add(tr("Tracks"), tr("%n video", nullptr, info.value(QStringLiteral("videoTracks")).toInt()) + QStringLiteral(", ") +
                              tr("%n audio", nullptr, info.value(QStringLiteral("audioTracks")).toInt()));
        return lines;
    }
    if (kind == QLatin1String("bin")) {
        add(tr("Type"), tr("Bin"));
        add(tr("Contents"), tr("%n media item(s)", nullptr, info.value(QStringLiteral("media")).toInt()) + QStringLiteral(", ") +
                                tr("%n sequence(s)", nullptr, info.value(QStringLiteral("sequences")).toInt()));
        add(tr("Size"), librarySizeText(info.value(QStringLiteral("bytes")).toLongLong()));
        return lines;
    }
    if (kind == QLatin1String("title")) {
        add(tr("Type"), tr("Title (made in Wabcut)"));
        add(tr("Duration"), info.value(QStringLiteral("duration")).toString());
        return lines;
    }
    const bool still = info.value(QStringLiteral("still")).toBool();
    add(tr("Format"), info.value(QStringLiteral("container")).toString());
    if (!still)
        add(tr("Duration"), info.value(QStringLiteral("duration")).toString());
    const QVariantMap v = info.value(QStringLiteral("video")).toMap();
    if (!v.isEmpty()) {
        const QString profile = v.value(QStringLiteral("profile")).toString();
        QStringList video{v.value(QStringLiteral("codec")).toString() + (profile.isEmpty() ? QString() : QLatin1Char(' ') + profile),
                          QStringLiteral("%1 × %2").arg(v.value(QStringLiteral("width")).toInt())
                              .arg(v.value(QStringLiteral("height")).toInt())};
        if (!still) {
            video << tr("%1 fps").arg(numberText(v.value(QStringLiteral("frameRate")).toDouble())) +
                         (v.value(QStringLiteral("variableFrameRate")).toBool() ? tr(" (variable)") : QString());
        }
        video.removeAll(QString());
        add(tr("Video"), video.join(dot));
        QStringList pixels{v.value(QStringLiteral("pixelFormat")).toString(),
                           v.value(QStringLiteral("bitDepth")).toInt() > 0
                               ? tr("%1-bit").arg(v.value(QStringLiteral("bitDepth")).toInt()) : QString(),
                           v.value(QStringLiteral("chroma")).toString(),
                           v.value(QStringLiteral("alpha")).toBool() ? tr("alpha") : QString()};
        pixels.removeAll(QString());
        add(tr("Pixels"), pixels.join(dot));
        // 0.7 (card 103): APV, 4:2:2 HEVC and 4:2:2 AV1 say what Wabcut does with them (they play; some only decode).
        if (const auto support = media::codecSupportFor(v.value(QStringLiteral("codec")).toString().toStdString(),
                                                        v.value(QStringLiteral("chroma")).toString().toStdString())) {
            add(tr("Codec support"), codecSupportText(*support));
        }
        if (!still) {
            const QString order = v.value(QStringLiteral("fieldOrder")).toString();
            add(tr("Fields"), order == QLatin1String("upper")   ? tr("Upper field first")
                              : order == QLatin1String("lower") ? tr("Lower field first")
                                                                : tr("Progressive"));
        }
        if (const int rotation = v.value(QStringLiteral("rotation")).toInt(); rotation != 0)
            add(tr("Rotation"), tr("%1°").arg(rotation));
        // 0.7: dynamic HDR metadata. Dolby Vision as the container declares it (profile.compatibility, level; 8.4 is an HLG base,
        // 8.1 an HDR10 one), HDR10+ as the first frames carry it.
        QStringList dynamicHdr;
        const QVariantMap dolby = v.value(QStringLiteral("dolbyVision")).toMap();
        if (!dolby.isEmpty()) {
            const int compat = dolby.value(QStringLiteral("blCompatibilityId")).toInt();
            QStringList parts{tr("Dolby Vision profile %1").arg(dolby.value(QStringLiteral("profile")).toString()),
                              tr("level %1").arg(dolby.value(QStringLiteral("level")).toInt())};
            if (compat == 1)
                parts << tr("HDR10 base layer");
            else if (compat == 2)
                parts << tr("SDR base layer");
            else if (compat == 4)
                parts << tr("HLG base layer");
            parts << (dolby.value(QStringLiteral("rpu")).toBool() ? tr("RPU in the stream") : tr("no RPU"));
            dynamicHdr << parts.join(QStringLiteral(", "));
        }
        if (v.value(QStringLiteral("hdr10Plus")).toBool())
            dynamicHdr << tr("HDR10+ (SMPTE ST 2094-40) in the first frames");
        if (!dynamicHdr.isEmpty())
            add(tr("Dynamic HDR"), dynamicHdr.join(dot));
    }
    const QVariantList audio = info.value(QStringLiteral("audio")).toList();
    for (int i = 0; i < audio.size(); ++i) {
        const QVariantMap a = audio[i].toMap();
        QStringList parts{a.value(QStringLiteral("codec")).toString(),
                          tr("%n channel(s)", nullptr, a.value(QStringLiteral("channels")).toInt()),
                          tr("%1 Hz").arg(a.value(QStringLiteral("sampleRate")).toInt()),
                          a.value(QStringLiteral("title")).toString()};
        parts.removeAll(QString());
        add(audio.size() > 1 ? tr("Audio %1").arg(i + 1) : tr("Audio"), parts.join(dot));
        // 0.7 (card 062): the names a field recorder gave the channels of a poly WAV, "1 Boom · 2 Lav A".
        QStringList named;
        const QStringList names = a.value(QStringLiteral("channelNames")).toStringList();
        for (int c = 0; c < names.size(); ++c) {
            if (!names[c].isEmpty())
                named << QString::number(c + 1) + QLatin1Char(' ') + names[c];
        }
        if (!named.isEmpty())
            add(audio.size() > 1 ? tr("Audio %1 channels").arg(i + 1) : tr("Audio channels"), named.join(dot));
    }
    add(tr("Start timecode"), info.value(QStringLiteral("startTimecode")).toString());
    const int files = info.value(QStringLiteral("files")).toInt();
    const QString size = librarySizeText(info.value(QStringLiteral("bytes")).toLongLong());
    if (!size.isEmpty())
        add(tr("Size"), (files > 1 ? tr("%1 files, ").arg(files) : QString()) + size);
    if (const double bitrate = info.value(QStringLiteral("bitrate")).toDouble(); bitrate > 0.0) {
        add(tr("Data rate"), bitrate >= 1.0e6 ? tr("%1 Mbps").arg(QLocale().toString(bitrate / 1.0e6, 'f', 1))
                                              : tr("%1 kbps").arg(QLocale().toString(std::round(bitrate / 1.0e3), 'f', 0)));
    }
    add(tr("Camera card"), info.value(QStringLiteral("card")).toString());
    add(tr("Modified"), info.value(QStringLiteral("modified")).toString().replace(QLatin1Char('T'), QLatin1Char(' ')));
    if (info.contains(QStringLiteral("master"))) {
        add(tr("Subclip of"), info.value(QStringLiteral("master")).toString());
        add(tr("Subclip range"), tr("%1 to %2").arg(info.value(QStringLiteral("inText")).toString(),
                                                   info.value(QStringLiteral("outText")).toString()));
    }
    if (info.contains(QStringLiteral("key"))) { // a Library item (the Media Browser's file has its own path line)
        const QString proxy = info.value(QStringLiteral("proxy")).toString();
        add(tr("Proxy"), proxy == QLatin1String("ready")        ? tr("Ready")
                         : proxyBeingMade(proxy)                 ? tr("Being made")
                         : proxy == QLatin1String("stale")       ? tr("Out of date")
                         : proxy == QLatin1String("failed")      ? tr("Failed")
                                                                 : tr("None"));
        const int uses = info.value(QStringLiteral("uses")).toInt();
        add(tr("Used"), uses > 0 ? tr("%n clip(s) in sequences", nullptr, uses) : tr("Not used in a sequence"));
        add(tr("Location"), info.value(QStringLiteral("path")).toString() +
                                (info.value(QStringLiteral("offline")).toBool() ? tr(" (offline: the file cannot be found)")
                                                                                 : QString()));
    }
    return lines;
}

QString LibraryBridge::propertiesText(const QString& key) const {
    const QVariantMap info = mediaProperties(key);
    if (info.isEmpty())
        return {};
    QStringList out{info.value(QStringLiteral("name")).toString()};
    for (const QVariant& line : propertyLines(info)) {
        const QVariantMap l = line.toMap();
        out << l.value(QStringLiteral("label")).toString() + QStringLiteral(": ") + l.value(QStringLiteral("value")).toString();
    }
    return out.join(QLatin1Char('\n'));
}

bool LibraryBridge::copyProperties(const QString& key) const {
    const QString textOut = propertiesText(key);
    QClipboard* clipboard = qobject_cast<QGuiApplication*>(QCoreApplication::instance()) ? QGuiApplication::clipboard() : nullptr;
    if (textOut.isEmpty() || !clipboard)
        return false;
    clipboard->setText(textOut);
    return true;
}

QString LibraryBridge::propertiesTarget(bool libraryFocused) const {
    const auto librarySelection = [this]() -> QString {
        for (const QString& k : selection()) {
            if (parseMediaKey(k) || parseSequenceKey(k) || parseBinKey(k))
                return k;
        }
        return {};
    };
    if (libraryFocused) {
        if (const QString k = librarySelection(); !k.isEmpty())
            return k;
    }
    if (m_source && m_source->loaded()) {
        if (const QString k = m_source->mediaKeyText(); parseMediaKey(k))
            return k;
    }
    if (m_controller) {
        const SequencePtr sequence = m_controller->currentSequence();
        for (const QString& c : m_controller->selectedClips()) {
            const auto clipId = parseClipKey(c);
            const auto at = clipId && sequence ? sequence->findClip(*clipId) : std::nullopt;
            if (!at)
                continue;
            const Clip& clip = at->clip();
            if (clip.isNested())
                return sequenceKey(clip.sequence);
            if (clip.media.isValid())
                return mediaKey(clip.media);
        }
    }
    return libraryFocused ? QString() : librarySelection();
}

void LibraryBridge::requestProperties(const QString& key) {
    if (!mediaProperties(key).isEmpty())
        emit propertiesRequested(key);
}

void LibraryBridge::requestLinkMedia(const QStringList& keys) {
    // Only items that are offline (a title or a linked item has nothing to relink).
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    QStringList offline;
    for (const QString& key : keys) {
        const auto id = parseMediaKey(key);
        const MediaItem* m = id && snap ? snap->findMedia(*id) : nullptr;
        if (m && !m->isGenerator() && m->locator.offline && !offline.contains(key))
            offline << key;
    }
    if (offline.isEmpty()) {
        setMessage(tr("These items are not offline: nothing to link."));
        return;
    }
    emit linkMediaRequested(offline);
}

// ---- Housekeeping ----------------------------------------------------------------------------------------------------

bool LibraryBridge::reveal(const QString& key) {
    const auto id = parseMediaKey(key);
    const MediaItem* m = id && m_controller ? m_controller->snapshot()->findMedia(*id) : nullptr;
    if (!m || m->isGenerator())
        return false;
    const auto file = resolveMediaPath(m->locator, m_controller->projectFile());
    if (!file) {
        setMessage(tr("“%1” is offline: its file cannot be found.").arg(text(m->name)));
        return false;
    }
    return revealFile(QString::fromStdU16String(file->u16string()));
}

void LibraryBridge::selectUnused() {
    // Find > Unused lists them across bins; select them all.
    LibraryFilter filter;
    filter.unused = true;
    m_filter = filter;
    emit filterChanged();
    refresh(true);
    selectAll();
}

int LibraryBridge::removeUnused() {
    if (!m_controller || m_unused.empty())
        return 0;
    const std::vector<std::uint64_t> ids(m_unused.begin(), m_unused.end());
    int removed = 0;
    const bool ok = apply(QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Remove Unused"), [&](const ProjectState& s) {
        ProjectState state = s;
        removed = 0;
        for (const std::uint64_t id : ids) {
            auto next = edit::removeMedia(state, MediaId{id});
            if (!next)
                return std::expected<ProjectState, Error>(std::unexpected(next.error()));
            state = std::move(*next);
            ++removed;
        }
        return std::expected<ProjectState, Error>(std::move(state));
    });
    if (!ok)
        return 0;
    setMessage(tr("Removed %n unused item(s).", nullptr, removed));
    return removed;
}

QVariantList LibraryBridge::consolidatePreview() const {
    QVariantList out;
    const ProjectSnapshot snapshot = m_controller ? m_controller->snapshot() : nullptr;
    if (!snapshot)
        return out;
    const auto merged = edit::consolidateDuplicates(*snapshot);
    if (!merged)
        return out;
    for (const edit::ConsolidateGroup& g : merged->groups) {
        QVariantList removed;
        for (std::size_t i = 0; i < g.removed.size(); ++i) {
            removed.append(QVariantMap{{QStringLiteral("key"), mediaKey(g.removed[i])},
                                       {QStringLiteral("name"), text(i < g.removedNames.size() ? g.removedNames[i] : std::string())}});
        }
        QVariantList skipped;
        for (const edit::ConsolidateSkip& s : g.skipped)
            skipped.append(QVariantMap{{QStringLiteral("name"), text(s.name)}, {QStringLiteral("reason"), text(s.reason)}});
        QStringList notes;
        for (const std::string& n : g.notes)
            notes << text(n);
        out.append(QVariantMap{{QStringLiteral("keeper"), g.keeper.isValid() ? mediaKey(g.keeper) : QString()},
                               {QStringLiteral("keeperName"), text(g.keeperName)},
                               {QStringLiteral("removed"), removed},
                               {QStringLiteral("clips"), g.repointedClips},
                               {QStringLiteral("skipped"), skipped},
                               {QStringLiteral("notes"), notes}});
    }
    return out;
}

int LibraryBridge::consolidateDuplicates() {
    if (!m_controller)
        return 0;
    edit::ConsolidateResult report;
    const bool ok = apply(QT_TRANSLATE_NOOP("wabcut::app::LibraryBridge", "Consolidate Duplicates"),
                          [&](const ProjectState& s) -> std::expected<ProjectState, Error> {
                              auto merged = edit::consolidateDuplicates(s);
                              if (!merged)
                                  return std::unexpected(merged.error());
                              report = std::move(*merged);
                              return report.state;
                          });
    if (!ok)
        return 0;
    const int removed = report.removedItems();
    int skipped = 0;
    for (const edit::ConsolidateGroup& g : report.groups)
        skipped += static_cast<int>(g.skipped.size());
    QString said = removed > 0 ? tr("Merged %n duplicate item(s).", nullptr, removed) : tr("There are no duplicate items to merge.");
    if (report.repointedClips() > 0)
        said += QLatin1Char(' ') + tr("%n clip(s) now play the kept item.", nullptr, report.repointedClips());
    if (skipped > 0)
        said += QLatin1Char(' ') + tr("%n item(s) were left because their settings differ.", nullptr, skipped);
    setMessage(said);
    return removed;
}

bool LibraryBridge::passesFilters(const QString& key) const {
    const auto id = parseMediaKey(key);
    if (!id || !m_lastSnapshot)
        return false;
    const MediaItem* m = m_lastSnapshot->findMedia(*id);
    if (!m)
        return false;
    LibraryFilter filter = m_filter;
    filter.text.clear(); // the Smart query replaces the text
    LibraryModel::Row row{LibraryModel::Row::Kind::Media, id->value, nullptr, nullptr, m};
    return m_model->passes(row, filter);
}

void LibraryBridge::clearFilters() {
    if (!m_filter.active())
        return;
    m_filter = {};
    emit filterChanged();
    refresh(true);
}

// ---- Find -----------------------------------------------------------------------------------------------------------

void LibraryBridge::setFindText(const QString& value) {
    if (value == m_filter.text)
        return;
    m_filter.text = value;
    emit filterChanged();
    refresh(true);
}

QVariantMap LibraryBridge::filters() const {
    return {
        {QStringLiteral("type"), m_filter.type},           {QStringLiteral("label"), m_filter.label},
        {QStringLiteral("transcribed"), m_filter.transcribed}, {QStringLiteral("offline"), m_filter.offline},
        {QStringLiteral("proxy"), m_filter.proxy},         {QStringLiteral("generated"), m_filter.generated},
        {QStringLiteral("unused"), m_filter.unused},       {QStringLiteral("duplicates"), m_filter.duplicates},
        {QStringLiteral("from"), m_filter.from},           {QStringLiteral("to"), m_filter.to},
    };
}

void LibraryBridge::setFilters(const QVariantMap& f) {
    // The map replaces the filters (a key left out is off); the Find text is separate.
    LibraryFilter next;
    next.text = m_filter.text;
    next.type = f.value(QStringLiteral("type")).toString();
    next.label = f.value(QStringLiteral("label"), -1).toInt();
    next.transcribed = f.value(QStringLiteral("transcribed")).toBool();
    next.offline = f.value(QStringLiteral("offline")).toBool();
    next.proxy = f.value(QStringLiteral("proxy")).toBool();
    next.generated = f.value(QStringLiteral("generated")).toBool();
    next.unused = f.value(QStringLiteral("unused")).toBool();
    next.duplicates = f.value(QStringLiteral("duplicates")).toBool();
    next.from = f.value(QStringLiteral("from")).toString().trimmed();
    next.to = f.value(QStringLiteral("to")).toString().trimmed();
    if (next == m_filter)
        return;
    m_filter = next;
    emit filterChanged();
    refresh(true);
}

int LibraryBridge::filterCount() const {
    const LibraryFilter& f = m_filter;
    return (!f.type.isEmpty() ? 1 : 0) + (f.label >= 0 ? 1 : 0) + (f.transcribed ? 1 : 0) + (f.offline ? 1 : 0) +
           (f.proxy ? 1 : 0) + (f.generated ? 1 : 0) + (f.unused ? 1 : 0) + (f.duplicates ? 1 : 0) +
           (!f.from.isEmpty() || !f.to.isEmpty() ? 1 : 0);
}

// ---- Selection --------------------------------------------------------------------------------------------------------

QStringList LibraryBridge::selection() const {
    // In the order the items are listed (keys not listed now, e.g. outside a Find result, after them). Sorting the
    // selected keys by row keeps this O(selection), not O(items): views read it while 10,000 items are listed.
    std::vector<std::pair<int, QString>> keyed;
    keyed.reserve(static_cast<std::size_t>(m_selection.size()));
    for (const QString& key : m_selection) {
        const int row = m_model->rowOf(key);
        keyed.emplace_back(row < 0 ? std::numeric_limits<int>::max() : row, key);
    }
    std::sort(keyed.begin(), keyed.end());
    QStringList out;
    out.reserve(static_cast<qsizetype>(keyed.size()));
    for (auto& entry : keyed)
        out << std::move(entry.second);
    return out;
}

QStringList LibraryBridge::labelNames() const {
    QStringList out;
    for (int i = 0; i < 16; ++i)
        out << libraryLabelName(i);
    return out;
}

bool LibraryBridge::mediaSelected() const {
    return std::any_of(m_selection.begin(), m_selection.end(), [](const QString& k) { return parseMediaKey(k).has_value(); });
}

void LibraryBridge::select(const QString& key, int mode) {
    switch (mode) {
    case Toggle:
        if (!m_selection.remove(key))
            m_selection.insert(key);
        m_anchor = key;
        break;
    case Add:
        m_selection.insert(key);
        m_anchor = key;
        break;
    case Range: {
        const int a = m_model->rowOf(m_anchor.isEmpty() ? key : m_anchor);
        const int b = m_model->rowOf(key);
        if (a < 0 || b < 0) {
            m_selection = {key};
            m_anchor = key;
            break;
        }
        m_selection.clear();
        for (int row = std::min(a, b); row <= std::max(a, b); ++row)
            m_selection.insert(m_model->keyOf(row));
        break;
    }
    default:
        m_selection = key.isEmpty() ? QSet<QString>{} : QSet<QString>{key};
        m_anchor = key;
        break;
    }
    m_model->setSelection(m_selection);
    if (m_readiness)
        m_readiness->setLibrarySelection(QStringList(m_selection.begin(), m_selection.end()));
    emit selectionChanged();
}

void LibraryBridge::selectAll() {
    m_selection.clear();
    for (int row = 0; row < m_model->rowCount(); ++row)
        m_selection.insert(m_model->keyOf(row));
    m_model->setSelection(m_selection);
    emit selectionChanged();
}

void LibraryBridge::clearSelection() {
    if (m_selection.isEmpty())
        return;
    m_selection.clear();
    m_model->setSelection(m_selection);
    emit selectionChanged();
}

void LibraryBridge::pruneSelection() {
    const ProjectSnapshot snap = m_lastSnapshot;
    const auto before = m_selection.size();
    m_selection.removeIf([&](const QString& key) {
        if (!snap)
            return true;
        if (const auto b = parseBinKey(key))
            return snap->findBin(*b) == nullptr;
        if (const auto m = parseMediaKey(key))
            return snap->findMedia(*m) == nullptr;
        if (const auto q = parseSequenceKey(key))
            return snap->findSequence(*q) == nullptr;
        return true;
    });
    m_model->setSelection(m_selection);
    if (m_selection.size() != before)
        emit selectionChanged();
}

// ---- Usage and Find in Timeline (P1 round, #40) ---------------------------------------------------------------------

QVariantList LibraryBridge::usages(const QString& key) const {
    QVariantList out;
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const auto media = parseMediaKey(key);
    const auto sequence = parseSequenceKey(key);
    if (!snap || (!media && !sequence))
        return out;
    struct Use {
        const Sequence* sequence = nullptr;
        ClipId clip;
        std::string clipName;
        Ticks start;
        QStringList tracks;
        std::string takeName; ///< 0.6: the audition take that plays it when that is not the active one
    };
    std::vector<Use> uses;
    for (const SequencePtr& seq : snap->sequences) {
        std::map<std::uint64_t, std::size_t> byGroup; // link group -> its use in `uses`
        const auto visit = [&](const std::vector<TrackPtr>& tracks, QChar letter) {
            for (std::size_t i = 0; i < tracks.size(); ++i) {
                if (!tracks[i])
                    continue;
                const QString label = QStringLiteral("%1%2").arg(letter).arg(i + 1);
                for (const Clip& c : tracks[i]->clips) {
                    const auto plays = [&](const Clip& take) {
                        return media ? (!take.isNested() && take.media == *media)
                                     : (take.isNested() && take.sequence == *sequence);
                    };
                    // 0.6 (card #74): an audition whose other take plays it is a use too (Find in Timeline shows the
                    // audition and names the take).
                    std::string viaTake;
                    bool match = plays(c);
                    for (std::size_t k = 0; !match && c.audition() && k < takeCount(c); ++k) {
                        if (k != c.audition()->active && plays(auditionTakeClip(c, k))) {
                            match = true;
                            viaTake = takeName(c, k);
                        }
                    }
                    if (!match)
                        continue;
                    if (c.linkGroup.isValid()) {
                        if (const auto it = byGroup.find(c.linkGroup.value); it != byGroup.end()) {
                            Use& use = uses[it->second];
                            if (!use.tracks.contains(label))
                                use.tracks << label;
                            use.start = std::min(use.start, c.start());
                            continue;
                        }
                        byGroup.emplace(c.linkGroup.value, uses.size());
                    }
                    uses.push_back({seq.get(), c.id, c.name(), c.start(), {label}, std::move(viaTake)});
                }
            }
        };
        visit(seq->videoTracks, QLatin1Char('V'));
        visit(seq->audioTracks, QLatin1Char('A'));
    }
    std::stable_sort(uses.begin(), uses.end(), [](const Use& a, const Use& b) {
        if (a.sequence != b.sequence) {
            const int c = QString::compare(text(a.sequence->name), text(b.sequence->name), Qt::CaseInsensitive);
            if (c != 0)
                return c < 0;
            return a.sequence->id.value < b.sequence->id.value;
        }
        return a.start < b.start;
    });
    for (const Use& use : uses) {
        const Sequence& seq = *use.sequence;
        //: Find in Timeline: an audition clip whose other (not active) take uses the item, e.g. "Hero (take “Alt 2”)"
        const QString clipName = use.takeName.empty() ? text(use.clipName)
                                                      : tr("%1 (take “%2”)").arg(text(use.clipName), text(use.takeName));
        out.append(QVariantMap{
            {QStringLiteral("sequenceKey"), sequenceKey(seq.id)},
            {QStringLiteral("sequenceName"), text(seq.name)},
            {QStringLiteral("clipId"), clipKey(use.clip)},
            {QStringLiteral("clipName"), clipName},
            {QStringLiteral("takeName"), text(use.takeName)},
            {QStringLiteral("timecode"), text(ticksToTimecode(use.start + seq.startTimecode, seq.frameRate, seq.dropFrame))},
            {QStringLiteral("tracks"), use.tracks.join(QStringLiteral(", "))},
            {QStringLiteral("seconds"), use.start.toSeconds()},
        });
    }
    return out;
}

QString LibraryBridge::usageText(const QString& key) const {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    LibraryUsage use;
    if (const auto media = parseMediaKey(key)) {
        if (const auto it = m_usage.find(media->value); it != m_usage.end())
            use = it->second;
    } else if (const auto sequence = parseSequenceKey(key)) {
        if (const auto it = m_sequenceUsage.find(sequence->value); it != m_sequenceUsage.end())
            use = it->second;
    } else {
        return {};
    }
    if (use.uses == 0)
        return tr("Not used in any sequence");
    QSet<QString> sequences;
    for (const QVariant& v : usages(key))
        sequences.insert(v.toMap().value(QStringLiteral("sequenceKey")).toString());
    // Whole English words (no "time(s)"): the tooltip is read at a glance.
    const QString used = use.uses == 1 ? tr("Used once") : tr("Used %1 times").arg(use.uses);
    const QString where = sequences.size() == 1 ? tr("in 1 sequence") : tr("in %1 sequences").arg(sequences.size());
    //: The Library's usage badge, e.g. "Used 3 times in 2 sequences (video 3, audio 3)"
    return tr("%1 %2 (video %3, audio %4)").arg(used, where).arg(use.video).arg(use.audio);
}

bool LibraryBridge::findInTimeline(const QString& sequenceKeyText, const QString& clipId) {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const auto sequence = parseSequenceKey(sequenceKeyText);
    const auto clip = parseClipKey(clipId);
    const Sequence* seq = snap && sequence ? snap->findSequence(*sequence) : nullptr;
    if (!seq || !clip) {
        setMessage(tr("That sequence is no longer in the project"));
        return false;
    }
    const auto location = seq->findClip(*clip);
    if (!location) {
        setMessage(tr("That clip is no longer in %1").arg(text(seq->name)));
        return false;
    }
    const Ticks start = location->clip().start();
    // Its tab (SequenceTabs keeps the tabs), else the controller's current sequence.
    if (m_controller->currentSequenceId() != *sequence && !(m_openSequence && m_openSequence(sequenceKeyText))) {
        for (std::size_t i = 0; i < snap->sequences.size(); ++i) {
            if (snap->sequences[i]->id == *sequence)
                m_controller->setCurrentSequenceIndex(static_cast<int>(i));
        }
    }
    if (m_controller->currentSequenceId() != *sequence)
        return false;
    m_controller->setPlayhead(start);
    m_controller->selectClip(clipId, ProjectController::Replace, true);
    emit revealInTimelineRequested(clipId);
    return true;
}

// ---- Twirl-down bins (P1 round, #46) ---------------------------------------------------------------------------------

namespace {

/// QSettings key of a project file's twirl-down bins ("library/expanded/<hash of its path>").
QString expansionKey(const QString& projectFile) {
    QString path = QDir::cleanPath(projectFile);
#if defined(Q_OS_WIN)
    path = path.toLower();
#endif
    return QStringLiteral("library/expanded/") +
           QString::fromLatin1(QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha1).toHex().left(20));
}

} // namespace

void LibraryBridge::loadExpansion(const QString& projectFile) {
    m_expanded.clear();
    m_expansionFile = projectFile;
    m_expansionLoaded = true;
    if (projectFile.isEmpty() || !m_persistent)
        return;
    for (const QString& key : QSettings().value(expansionKey(projectFile)).toStringList()) {
        if (const auto bin = parseBinKey(key))
            m_expanded.insert(bin->value);
    }
}

void LibraryBridge::saveExpansion() const {
    if (m_expansionFile.isEmpty() || !m_persistent)
        return;
    QStringList keys;
    for (const std::uint64_t bin : m_expanded) {
        if (!m_lastSnapshot || m_lastSnapshot->findBin(BinId{bin}))
            keys << binKey(BinId{bin});
    }
    keys.sort();
    QSettings settings;
    if (keys.isEmpty())
        settings.remove(expansionKey(m_expansionFile));
    else
        settings.setValue(expansionKey(m_expansionFile), keys);
}

void LibraryBridge::setExpanded(const QString& key, bool expanded, bool recursive) {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const auto id = parseBinKey(key);
    const Bin* bin = snap && id ? snap->findBin(*id) : nullptr;
    if (!bin || bin->isSearch())
        return;
    std::vector<BinId> bins{*id};
    if (recursive) { // Alt+click: every bin inside it too
        for (std::size_t i = 0; i < bins.size(); ++i) {
            for (const Bin& b : snap->bins) {
                if (b.parent == bins[i] && !b.isSearch())
                    bins.push_back(b.id);
            }
        }
    }
    bool changed = false;
    for (const BinId b : bins)
        changed = (expanded ? m_expanded.insert(b.value).second : m_expanded.erase(b.value) > 0) || changed;
    if (!changed)
        return;
    if (!expanded) {
        // What a collapse hides is no longer selected (Delete never acts on rows nobody sees): the bin is, instead.
        const qsizetype before = m_selection.size();
        m_selection.removeIf([&](const QString& k) {
            const QString parent = parentOf(k);
            const auto p = parseBinKey(parent);
            return p && binInside(*snap, *p, *id);
        });
        if (m_selection.size() != before) {
            m_selection.insert(key);
            m_anchor = key;
            m_model->setSelection(m_selection);
            emit selectionChanged();
        }
    }
    saveExpansion();
    refresh(true);
    emit expandedChanged();
}

bool LibraryBridge::isExpanded(const QString& key) const {
    const auto id = parseBinKey(key);
    return id && m_expanded.contains(id->value);
}

QString LibraryBridge::parentOf(const QString& key) const {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    if (!snap)
        return {};
    if (const auto b = parseBinKey(key)) {
        const Bin* bin = snap->findBin(*b);
        return bin ? binKey(bin->parent) : QString();
    }
    if (const auto m = parseMediaKey(key)) {
        const MediaItem* item = snap->findMedia(*m);
        return item ? binKey(item->bin) : QString();
    }
    if (const auto q = parseSequenceKey(key)) {
        const Sequence* seq = snap->findSequence(*q);
        return seq ? binKey(seq->bin) : QString();
    }
    return {};
}

bool LibraryBridge::treeShown() const { return m_model->tree(); }

// ---- Commands ---------------------------------------------------------------------------------------------------------

namespace {

/// UX rework (L): Premiere's Project panel selection keys -> selectionMoveRequested(where, extend).
struct SelectionMove {
    const char* command;
    const char* where;
    bool extend;
};
constexpr std::array kSelectionMoves{
    SelectionMove{"project.moveSelectionUp", "up", false},
    SelectionMove{"project.moveSelectionDown", "down", false},
    SelectionMove{"project.moveSelectionLeft", "left", false},
    SelectionMove{"project.moveSelectionRight", "right", false},
    SelectionMove{"project.moveSelectionHome", "home", false},
    SelectionMove{"project.moveSelectionEnd", "end", false},
    SelectionMove{"project.moveSelectionPageUp", "pageUp", false},
    SelectionMove{"project.moveSelectionPageDown", "pageDown", false},
    SelectionMove{"project.extendSelectionUp", "up", true},
    SelectionMove{"project.extendSelectionDown", "down", true},
    SelectionMove{"project.extendSelectionLeft", "left", true},
    SelectionMove{"project.extendSelectionRight", "right", true},
};

const SelectionMove* selectionMoveOf(const QString& id) {
    for (const SelectionMove& move : kSelectionMoves) {
        if (id == QLatin1String(move.command))
            return &move;
    }
    return nullptr;
}

/// 0.7 (card 021): Premiere's Next / Previous Column Field and Next / Previous Row Field while a list cell is open ->
/// fieldStepRequested(column, row).
struct FieldStep {
    const char* command;
    int column;
    int row;
};
constexpr std::array kFieldSteps{
    FieldStep{"project.nextColumnField", 1, 0},
    FieldStep{"project.previousColumnField", -1, 0},
    FieldStep{"project.nextRowField", 0, 1},
    FieldStep{"project.previousRowField", 0, -1},
};

const FieldStep* fieldStepOf(const QString& id) {
    for (const FieldStep& step : kFieldSteps) {
        if (id == QLatin1String(step.command))
            return &step;
    }
    return nullptr;
}

/// +1 for the zoom-in keys, -1 for the zoom-out keys, 0 otherwise.
int thumbnailStepOf(const QString& id) {
    if (id == QLatin1String("project.thumbnailSizeNext") || id == QLatin1String("project.zoomIn"))
        return 1;
    if (id == QLatin1String("project.thumbnailSizePrevious") || id == QLatin1String("project.zoomOut"))
        return -1;
    return 0;
}

} // namespace

bool LibraryBridge::handlesCommand(const QString& id) {
    static const QStringList kCommands{
        QStringLiteral("project.newBin"),   QStringLiteral("project.delete"),     QStringLiteral("project.listView"),
        QStringLiteral("project.iconView"), QStringLiteral("project.toggleView"), QStringLiteral("project.hoverScrub"),
        QStringLiteral("project.openInSourceMonitor"), QStringLiteral("clip.makeSubclip"), QStringLiteral("edit.find"),
        QStringLiteral("file.getProperties"), // P1 round (#39): Main.qml opens the Properties dialog
        QStringLiteral("project.deleteWithOptions"), // P1 round (#38): Ctrl/Cmd+Delete, the delete confirmation
        // 0.7 (card 021): Tab / Shift+Tab and Enter / Shift+Enter between the cells of the list view while one is open.
        QStringLiteral("project.nextColumnField"), QStringLiteral("project.previousColumnField"),
        QStringLiteral("project.nextRowField"), QStringLiteral("project.previousRowField"),
        // 0.7 (card 025): Edit > Consolidate Duplicates… and Remove Unused… (Main.qml opens the Library's confirmations).
        QStringLiteral("edit.consolidateDuplicates"), QStringLiteral("edit.removeUnused"),
    };
    return kCommands.contains(id) || thumbnailStepOf(id) != 0 || selectionMoveOf(id) != nullptr;
}

QVariantMap LibraryBridge::commandAvailability(const QString& id) const {
    // P1 round (#41): Cut and Paste of Library items (the Library has the focus: Main.qml and the menu bar ask). Without
    // a selection or anything cut they are not the Library's: the timeline's Cut and Paste apply.
    if (id == QStringLiteral("edit.cut") && !m_selection.isEmpty())
        return {{QStringLiteral("handled"), true}, {QStringLiteral("enabled"), true}, {QStringLiteral("reason"), QString()}};
    if (id == QStringLiteral("edit.paste") && !m_cut.isEmpty())
        return {{QStringLiteral("handled"), true}, {QStringLiteral("enabled"), true}, {QStringLiteral("reason"), QString()}};
    if (!handlesCommand(id))
        return {{QStringLiteral("handled"), false}};
    bool enabled = true;
    QString reason;
    if (id == QStringLiteral("file.getProperties") && propertiesTarget(true).isEmpty() && propertiesTarget(false).isEmpty()) {
        enabled = false;
        reason = tr("Select a clip in the Library, the Source Monitor or the Timeline first");
    }
    const int zoom = thumbnailStepOf(id);
    if (zoom > 0 && m_thumbnailScale >= kThumbnailScaleMax) {
        enabled = false;
        reason = tr("The thumbnails are at their largest size");
    } else if (zoom < 0 && m_thumbnailScale <= kThumbnailScaleMin) {
        enabled = false;
        reason = tr("The thumbnails are at their smallest size");
    } else if (selectionMoveOf(id) && m_model->rowCount() == 0) {
        enabled = false;
        reason = tr("Nothing is listed in the Library");
    } else if ((id == QStringLiteral("project.delete") || id == QStringLiteral("project.deleteWithOptions")) &&
               m_selection.isEmpty()) {
        enabled = false;
        reason = tr("Select items in the Library first");
    } else if (fieldStepOf(id) != nullptr && !m_fieldEditing) {
        enabled = false;
        reason = tr("Edit a cell in the Library list first");
    } else if (id == QStringLiteral("edit.consolidateDuplicates") && m_duplicateGroups.isEmpty()) {
        enabled = false;
        reason = tr("There are no duplicate media items in the project");
    } else if (id == QStringLiteral("edit.removeUnused") && m_unused.empty()) {
        enabled = false;
        reason = tr("Every item in the project is used by a sequence");
    } else if (id == QStringLiteral("project.openInSourceMonitor") && !mediaSelected()) {
        enabled = false;
        reason = tr("Select a clip in the Library first");
    } else if (id == QStringLiteral("clip.makeSubclip") && !canMakeSubclip()) {
        enabled = false;
        reason = tr("Open a clip in the Source Monitor first");
    }
    return {{QStringLiteral("handled"), true}, {QStringLiteral("enabled"), enabled}, {QStringLiteral("reason"), reason}};
}

bool LibraryBridge::runCommand(const QString& id) {
    // P1 round (#41): Edit > Cut and Paste while the Library has the focus (Main.qml asks only then).
    if (id == QStringLiteral("edit.cut"))
        return !m_selection.isEmpty() && cutItems(selection());
    if (id == QStringLiteral("edit.paste")) {
        if (m_cut.isEmpty())
            return false; // nothing cut here: the timeline's Paste
        pasteItems();     // a refusal says why (message); the key was the Library's either way
        return true;
    }
    // 0.7 (card 021): the list view moves on (and the key is the Library's only while a cell is open).
    if (const FieldStep* step = fieldStepOf(id)) {
        if (!m_fieldEditing)
            return false;
        emit fieldStepRequested(step->column, step->row);
        return true;
    }
    if (id == QStringLiteral("project.newBin"))
        return !newBin().isEmpty();
    // P1 round (#38): items sequences use are asked about first (the panel's confirmation); Ctrl/Cmd+Delete always asks.
    if (id == QStringLiteral("project.delete"))
        return requestDelete(selection(), false);
    if (id == QStringLiteral("project.deleteWithOptions"))
        return requestDelete(selection(), true);
    if (id == QStringLiteral("project.listView")) {
        setView(QStringLiteral("list"));
        return true;
    }
    if (id == QStringLiteral("project.iconView")) {
        setView(QStringLiteral("grid"));
        return true;
    }
    if (id == QStringLiteral("project.toggleView")) {
        setView(m_view == QStringLiteral("list") ? QStringLiteral("grid") : QStringLiteral("list"));
        return true;
    }
    if (id == QStringLiteral("project.hoverScrub")) {
        setHoverScrub(!m_hoverScrub);
        return true;
    }
    if (id == QStringLiteral("project.openInSourceMonitor")) {
        for (const QString& key : selection()) {
            if (parseMediaKey(key) && m_source)
                return m_source->load(key);
        }
        return false;
    }
    if (id == QStringLiteral("edit.find")) {
        emit findRequested();
        return true;
    }
    // 0.7 (card 025): the panel asks before it merges or removes (Edit > Consolidate Duplicates…, Remove Unused…).
    if (id == QStringLiteral("edit.consolidateDuplicates") || id == QStringLiteral("edit.removeUnused")) {
        if (!commandAvailability(id).value(QStringLiteral("enabled")).toBool())
            return false;
        emit housekeepingRequested(id == QStringLiteral("edit.consolidateDuplicates") ? QStringLiteral("consolidate")
                                                                                       : QStringLiteral("removeUnused"));
        return true;
    }
    if (id == QStringLiteral("clip.makeSubclip")) {
        if (!canMakeSubclip())
            return false;
        emit makeSubclipRequested(); // the panel asks for the name, then calls makeSubclip()
        return true;
    }
    // UX rework (L): the zoom keys step the card size (at a bound they still count as handled, so "=" and "-" never
    // fall through to the sequence's zoom while the Library has focus).
    if (const int zoom = thumbnailStepOf(id); zoom != 0) {
        setThumbnailScale(m_thumbnailScale + zoom * kThumbnailScaleStep);
        return true;
    }
    // The selection keys: the panel knows its columns and page size, so it moves the selection.
    if (const SelectionMove* move = selectionMoveOf(id)) {
        if (m_model->rowCount() == 0)
            return true; // nothing to move through; still the Library's key
        emit selectionMoveRequested(QString::fromLatin1(move->where), move->extend);
        return true;
    }
    return false;
}

} // namespace wabcut::app
