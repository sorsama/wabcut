// ExportController's export queue across restarts (P1 round, stream 2C, #20; Adobe Media Encoder keeps its queue).
//
// The queue folder ("<AppConfig>/Export Queue") holds queue.json - one entry per unfinished job, in queue order - and a
// project snapshot per job ("<key>.wabcut", shared by the jobs queued from one snapshot): a job exports the project as
// it was when it was queued, unsaved edits included, so the project file on disk is not enough. queue.json is rewritten
// (atomically) at every change of the queue that matters: a job queued, started, paused, finished, cancelled or removed;
// progress alone does not write it. A crash mid-export leaves the job "running" in it.
//
// On the next start (restoreQueue(), from Main.qml):
//   held / queued   -> held again: Ready, waiting for Start Queue (nothing starts by itself - an export that ran by
//                      surprise after a restart could overwrite a file or fill a drive).
//   running/paused  -> "interrupted": a finished row with Retry (it exports again from the start into the same file).
//                      One whose file was written after it started finished before Wabcut closed: it comes back done.
// A finished, cancelled or removed job leaves the folder (its snapshot goes with the last job that names it).
// Only one Wabcut keeps the queue (a QLockFile in the folder); a second one keeps its queue in memory, as before.

#include "ExportController.h"

#include "BridgeIds.h"
#include "ExportInternal.h"

#include <wabcut/export/ExportPresets.h>
#include <wabcut/model/ProjectIO.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <set>

namespace wabcut::app {

namespace {

namespace ex = exporter;

QString translate(const char* text, const char* disambiguation = nullptr, int n = -1) {
    return QCoreApplication::translate("ExportController", text, disambiguation, n);
}

std::filesystem::path toPath(const QString& s) {
    return std::filesystem::path(s.toStdU16String());
}

QString fromPath(const std::filesystem::path& p) {
    return QDir::toNativeSeparators(QString::fromStdU16String(p.u16string()));
}

const QString kQueueFile = QStringLiteral("queue.json");
constexpr int kQueueVersion = 1;

/// Files of the queue folder written by this code: "<key>.wabcut" snapshots. Nothing else there is ever deleted.
bool isSnapshotFile(const QString& name) {
    static const QRegularExpression kName(QStringLiteral(R"(^q-[0-9a-f]{32}\.wabcut$)"));
    return kName.match(name).hasMatch();
}

/// A temporary file of an export into `output` ("<stem>.wabcut-partial-<tag><ext>", ExportJob / MxfDelivery) written
/// at or after `since`: that export never finished (a finished one renames it into place).
bool partialWrittenSince(const std::filesystem::path& output, const QDateTime& since) {
    const QFileInfo target(fromPath(output));
    const QString prefix = target.completeBaseName() + QStringLiteral(".wabcut-partial-");
    const QString suffix = target.suffix().isEmpty() ? QString() : QStringLiteral(".") + target.suffix();
    const QDir folder = target.absoluteDir();
    for (const QFileInfo& f : folder.entryInfoList({prefix + QLatin1Char('*')}, QDir::Files | QDir::Hidden)) {
        if (f.fileName().endsWith(suffix, Qt::CaseInsensitive) && f.lastModified().toUTC() >= since)
            return true;
    }
    return false;
}

} // namespace

/// What a kept job exports, independent of the project open now: enough to queue it again after a restart, and for a
/// restored job's Retry.
struct ExportController::KeptJob {
    ProjectSnapshot snapshot;
    QString snapshotFile; ///< "q-<uuid>.wabcut" in the queue folder ("" until written)
    std::uint64_t sequence = 0;
    ex::ExportSettings settings;
    std::filesystem::path output;
    std::filesystem::path projectFile;
    QString title;
    QString presetName;
    QString credentials;
    QString origin;
    QString sequenceKey; ///< the row's request (for retry and the queue rows' retryable role)
    QString presetId;
    QVariantMap overrides;
    QDateTime startedAt; ///< first seen running (UTC); a file written after it means the job finished
    /// The file at `output` when the job started (its last-modified time, UTC; invalid: none): the file it replaces. It
    /// still there after a crash is not the job's finished export (fix round 1, ARGON-11).
    QDateTime replacedModified;
    bool dropped = false; ///< cancelled for good (Cancel Exports and Quit, Clear): not written again
};

// ---- ExportJobModel ---------------------------------------------------------------------------------------------------

void ExportJobModel::removeFinishedRow(const QString& jobId) {
    for (int i = rowCount() - 1; i >= 0; --i) {
        const Row& r = m_rows[static_cast<std::size_t>(i)];
        if (r.jobId == jobId && r.finished) {
            beginRemoveRows({}, i, i);
            m_rows.erase(m_rows.begin() + i);
            endRemoveRows();
            emit countChanged();
            refreshSummary();
            return;
        }
    }
}

// ---- ExportController -------------------------------------------------------------------------------------------------

std::filesystem::path ExportController::defaultQueueFolder() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return toPath(QDir(base).filePath(QStringLiteral("Export Queue")));
}

QString ExportController::keptStateOf(const ExportJobModel::Row& row) {
    if (row.finished)
        return row.state == QLatin1String("interrupted") ? row.state : QString();
    if (row.held || row.state == QLatin1String("held"))
        return QStringLiteral("held");
    if (row.state == QLatin1String("running") || row.state == QLatin1String("paused"))
        return row.state;
    return QStringLiteral("queued");
}

void ExportController::keepJob(const QString& jobId, const ex::ExportRequest& request, const QString& presetName,
                               const QString& credentials, const QString& origin) {
    // A restored job's entry is made by restoreQueue() / retryKept() (it keeps its snapshot file); Ingest transcodes and
    // Render and Replace renders (0.7) take their file over when they finish, which a restart cannot do: they are not kept.
    if (m_restoring || m_queueFolder.empty() || origin == QLatin1String("transcode") || origin == QLatin1String("renderReplace") ||
        !request.snapshot)
        return;
    auto kept = std::make_shared<KeptJob>();
    kept->snapshot = request.snapshot;
    kept->sequence = request.sequence.value;
    kept->settings = request.settings;
    kept->output = request.output;
    kept->projectFile = request.projectFile;
    kept->title = QString::fromStdU16String(request.output.filename().u16string());
    kept->presetName = presetName;
    kept->credentials = credentials;
    kept->origin = origin;
    m_kept[jobId] = std::move(kept);
}

void ExportController::schedulePersist() {
    if (m_queueFolder.empty() || m_persistPending)
        return;
    m_persistPending = true;
    QTimer::singleShot(0, this, [this] {
        if (m_persistPending)
            persistQueue();
    });
}

std::vector<ExportController::PersistRow> ExportController::persistRowsNow() const {
    std::vector<PersistRow> rows;
    rows.reserve(static_cast<std::size_t>(m_jobs->rowCount()));
    for (const ExportJobModel::Row& row : m_jobs->rows())
        rows.push_back({row.jobId, keptStateOf(row), row.sequenceId, row.presetId, row.overrides});
    return rows;
}

void ExportController::onJobsChanged() {
    // Progress, speed and ETA change a row ten times a second; none of it is in queue.json (card #171).
    if (!m_queueFolder.empty() && persistRowsNow() != m_persistRows)
        schedulePersist();
}

void ExportController::persistQueue() {
    m_persistPending = false;
    if (m_queueFolder.empty())
        return;
    ++m_persistRuns;
    m_persistRows = persistRowsNow();
    const QDir folder(fromPath(m_queueFolder));
    QJsonArray entries;
    std::set<QString> referenced;
    std::set<QString> live; // kept jobs with a row
    for (const ExportJobModel::Row& row : m_jobs->rows()) {
        const auto it = m_kept.find(row.jobId);
        if (it == m_kept.end())
            continue;
        live.insert(row.jobId);
        KeptJob& kept = *it->second;
        const QString state = keptStateOf(row);
        if (state.isEmpty() || kept.dropped)
            continue;
        if (state == QLatin1String("running") && !kept.startedAt.isValid()) {
            kept.startedAt = QDateTime::currentDateTimeUtc().addSecs(-1); // file times are coarse on some drives
            const QFileInfo replaced(fromPath(kept.output));
            if (replaced.isFile())
                kept.replacedModified = replaced.lastModified().toUTC();
        }
        if (row.sequenceId.size() || row.presetId.size()) {
            kept.sequenceKey = row.sequenceId;
            kept.presetId = row.presetId;
            kept.overrides = row.overrides;
        }
        if (kept.snapshotFile.isEmpty()) {
            // Jobs queued from one snapshot (Send to Queue with several presets, a batch) share its file.
            for (const auto& [id, other] : m_kept) {
                if (other.get() != &kept && other->snapshot == kept.snapshot && !other->snapshotFile.isEmpty() &&
                    QFileInfo::exists(folder.filePath(other->snapshotFile))) {
                    kept.snapshotFile = other->snapshotFile;
                    break;
                }
            }
        }
        if (kept.snapshotFile.isEmpty()) {
            const QString name = QStringLiteral("q-%1.wabcut").arg(QUuid::createUuid().toString(QUuid::Id128));
            io::SaveOptions options;
            options.backups = 0;
            if (const auto saved = io::saveProject(*kept.snapshot, toPath(folder.filePath(name)), options); !saved) {
                qWarning("Export queue: a job's project could not be kept: %s", saved.error().message.c_str());
                continue; // this job stays in memory only
            }
            kept.snapshotFile = name;
        }
        referenced.insert(kept.snapshotFile);
        QJsonObject e;
        e[QStringLiteral("state")] = state;
        e[QStringLiteral("title")] = kept.title;
        e[QStringLiteral("output")] = fromPath(kept.output);
        e[QStringLiteral("project")] = kept.snapshotFile;
        e[QStringLiteral("projectFile")] = fromPath(kept.projectFile);
        e[QStringLiteral("sequence")] = QString::number(kept.sequence);
        e[QStringLiteral("presetName")] = kept.presetName;
        e[QStringLiteral("credentials")] = kept.credentials;
        e[QStringLiteral("origin")] = kept.origin;
        e[QStringLiteral("sequenceKey")] = kept.sequenceKey;
        e[QStringLiteral("presetId")] = kept.presetId;
        e[QStringLiteral("overrides")] = QJsonObject::fromVariantMap(kept.overrides);
        if (kept.startedAt.isValid())
            e[QStringLiteral("startedAt")] = kept.startedAt.toString(Qt::ISODateWithMs);
        if (kept.replacedModified.isValid())
            e[QStringLiteral("replacedModified")] = kept.replacedModified.toString(Qt::ISODateWithMs);
        const std::string settings = ex::settingsToJson(kept.settings).dump();
        e[QStringLiteral("settings")] = QJsonDocument::fromJson(QByteArray::fromStdString(settings)).object();
        entries.append(e);
    }
    // Kept jobs whose rows are gone (Clear Finished, removed) are forgotten.
    for (auto it = m_kept.begin(); it != m_kept.end();) {
        if (!live.contains(it->first))
            it = m_kept.erase(it);
        else
            ++it;
    }
    const QJsonObject root{{QStringLiteral("version"), kQueueVersion}, {QStringLiteral("jobs"), entries}};
    const QString text = QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (text == m_persistedText)
        return; // nothing the queue holds changed: no write, and no sweep of the folder (card #171)
    {
        const QByteArray utf8 = text.toUtf8();
        if (const auto wrote = io::writeFileAtomically(toPath(folder.filePath(kQueueFile)),
                                                       std::string_view(utf8.constData(), static_cast<std::size_t>(utf8.size())));
            !wrote) {
            qWarning("Export queue: queue.json could not be written: %s", wrote.error().message.c_str());
            return; // the snapshots stay: the old queue.json may still name them
        }
        m_persistedText = text;
    }
    // Snapshots no entry names any more go (a finished job that may still be retried keeps its project in memory).
    for (auto& entry : m_kept) {
        KeptJob& kept = *entry.second;
        if (!kept.snapshotFile.isEmpty() && !referenced.contains(kept.snapshotFile))
            kept.snapshotFile.clear(); // written again if it is ever kept again
    }
    for (const QString& name : folder.entryList(QDir::Files)) {
        if (isSnapshotFile(name) && !referenced.contains(name))
            QFile::remove(folder.filePath(name));
    }
}

QVariantMap ExportController::restoreQueue() {
    QVariantMap result{{QStringLiteral("count"), 0},       {QStringLiteral("held"), 0},
                       {QStringLiteral("interrupted"), 0}, {QStringLiteral("done"), 0},
                       {QStringLiteral("skipped"), QStringList()}};
    if (m_restoreRan)
        return result;
    m_restoreRan = true;
    // Tests keep a queue only in a folder of their own (every shell of a test process shares one config location).
    std::filesystem::path folderPath = m_queueFolderOverride;
    if (folderPath.empty()) {
        if (QStandardPaths::isTestModeEnabled())
            return result;
        folderPath = defaultQueueFolder();
    }
    const QString folderText = fromPath(folderPath);
    if (!QDir().mkpath(folderText))
        return result;
    auto lock = std::make_shared<QLockFile>(QDir(folderText).filePath(QStringLiteral("queue.lock")));
    if (!lock->tryLock(0)) {
        result[QStringLiteral("locked")] = true; // another Wabcut keeps the queue
        return result;
    }
    m_queueLock = lock;
    m_queueFolder = folderPath;

    const QDir folder(folderText);
    QFile file(folder.filePath(kQueueFile));
    QJsonArray entries;
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        file.close();
        if (doc.isObject() && doc.object().value(QStringLiteral("version")).toInt() == kQueueVersion)
            entries = doc.object().value(QStringLiteral("jobs")).toArray();
    }
    std::map<QString, ProjectSnapshot> snapshots;
    QStringList skipped;
    int held = 0;
    int interrupted = 0;
    int done = 0;
    for (const QJsonValue& value : entries) {
        const QJsonObject e = value.toObject();
        const QString title = e.value(QStringLiteral("title")).toString();
        const QString projectName = e.value(QStringLiteral("project")).toString();
        const QString state = e.value(QStringLiteral("state")).toString();
        auto skip = [&] { skipped << (title.isEmpty() ? translate("An export") : title); };
        if (!isSnapshotFile(projectName)) {
            skip();
            continue;
        }
        ProjectSnapshot snapshot;
        if (const auto known = snapshots.find(projectName); known != snapshots.end()) {
            snapshot = known->second;
        } else if (auto loaded = io::loadProject(toPath(folder.filePath(projectName)))) {
            snapshot = std::make_shared<const ProjectState>(std::move(*loaded));
            snapshots[projectName] = snapshot;
        }
        const QByteArray settingsText = QJsonDocument(e.value(QStringLiteral("settings")).toObject()).toJson(QJsonDocument::Compact);
        std::optional<ex::ExportSettings> settings;
        try {
            if (auto s = ex::settingsFromJson(ex::Json::parse(settingsText.constData(), settingsText.constData() + settingsText.size())))
                settings = std::move(*s);
        } catch (...) {
        }
        bool sequenceOk = false;
        const std::uint64_t sequence = e.value(QStringLiteral("sequence")).toString().toULongLong(&sequenceOk);
        if (!snapshot || !settings || !sequenceOk || !snapshot->findSequence(SequenceId{sequence})) {
            skip();
            continue;
        }
        auto kept = std::make_shared<KeptJob>();
        kept->snapshot = snapshot;
        kept->snapshotFile = projectName;
        kept->sequence = sequence;
        kept->settings = *settings;
        kept->output = toPath(e.value(QStringLiteral("output")).toString());
        kept->projectFile = toPath(e.value(QStringLiteral("projectFile")).toString());
        kept->title = title.isEmpty() ? QString::fromStdU16String(kept->output.filename().u16string()) : title;
        kept->presetName = e.value(QStringLiteral("presetName")).toString();
        kept->credentials = e.value(QStringLiteral("credentials")).toString();
        kept->origin = e.value(QStringLiteral("origin")).toString(QStringLiteral("export"));
        kept->sequenceKey = e.value(QStringLiteral("sequenceKey")).toString();
        kept->presetId = e.value(QStringLiteral("presetId")).toString();
        kept->overrides = e.value(QStringLiteral("overrides")).toObject().toVariantMap();
        const QDateTime startedAt = QDateTime::fromString(e.value(QStringLiteral("startedAt")).toString(), Qt::ISODateWithMs);
        const QDateTime replacedModified =
            QDateTime::fromString(e.value(QStringLiteral("replacedModified")).toString(), Qt::ISODateWithMs);
        const QString jobId = newJobId();
        if (state == QLatin1String("held") || state == QLatin1String("queued")) {
            ex::ExportRequest request;
            request.snapshot = snapshot;
            request.sequence = SequenceId{sequence};
            request.settings = *settings;
            request.output = kept->output;
            request.projectFile = kept->projectFile;
            request.videoSource = m_videoSource;
            m_kept[jobId] = kept;
            m_restoring = true;
            queueRequestAs(jobId, std::move(request), kept->presetName, true, kept->credentials, kept->origin);
            m_restoring = false;
            m_jobs->setRequest(jobId, kept->sequenceKey, kept->presetId, kept->overrides);
            ++held;
            continue;
        }
        if (state != QLatin1String("running") && state != QLatin1String("paused") && state != QLatin1String("interrupted")) {
            skip();
            continue;
        }
        // Running or paused when Wabcut closed. A file written after the job started is the finished export (it is
        // renamed into place only when complete): the job had finished; otherwise it comes back to be retried. Not
        // when it is the file the job was to replace (still there, untouched), or when the job's temporary file is
        // still there (it never got renamed into place: the export did not finish).
        kept->startedAt = startedAt; // for Retry: a file there from before is the one to replace
        kept->replacedModified = replacedModified;
        const QFileInfo out(fromPath(kept->output));
        const QDateTime outModified = out.lastModified().toUTC();
        const bool finished = state != QLatin1String("interrupted") && startedAt.isValid() && out.isFile() &&
                              out.size() > 0 && outModified >= startedAt &&
                              !(replacedModified.isValid() && outModified == replacedModified) &&
                              !partialWrittenSince(kept->output, startedAt);
        ExportJobModel::Row row;
        row.jobId = jobId;
        row.queueId = 0; // not in the queue: Retry queues it
        row.title = kept->title;
        row.outputPath = fromPath(kept->output);
        row.presetName = kept->presetName;
        row.origin = kept->origin;
        row.finished = true;
        row.restored = true;
        row.sequenceId = kept->sequenceKey;
        row.presetId = kept->presetId;
        row.overrides = kept->overrides;
        if (finished) {
            row.state = QStringLiteral("done");
            row.phaseText = translate("Done");
            row.progress = 1.0;
            row.detail = translate("Finished before Wabcut closed");
            ++done;
        } else {
            row.state = QStringLiteral("interrupted");
            row.phaseText = translate("Interrupted");
            row.detail = translate("Wabcut closed while this exported. Retry exports it again from the start.");
            m_kept[jobId] = kept;
            ++interrupted;
        }
        m_jobs->append(std::move(row));
    }
    // From now on every change of the queue is kept.
    connect(m_jobs, &QAbstractItemModel::rowsInserted, this, &ExportController::schedulePersist);
    connect(m_jobs, &QAbstractItemModel::rowsRemoved, this, &ExportController::schedulePersist);
    connect(m_jobs, &QAbstractItemModel::dataChanged, this, &ExportController::onJobsChanged);
    persistQueue(); // the restored jobs under their new ids; files nothing names any more go
    emit keepsQueueChanged();
    result[QStringLiteral("count")] = held + interrupted + done;
    result[QStringLiteral("held")] = held;
    result[QStringLiteral("interrupted")] = interrupted;
    result[QStringLiteral("done")] = done;
    result[QStringLiteral("skipped")] = skipped;
    return result;
}

void ExportController::clearRestored() {
    QStringList remove;
    for (const ExportJobModel::Row& r : m_jobs->rows()) {
        if (!r.restored)
            continue;
        if (!r.finished) {
            if (const auto it = m_kept.find(r.jobId); it != m_kept.end())
                it->second->dropped = true;
            m_queue->cancel(r.queueId);
        } else if (r.state == QLatin1String("interrupted")) {
            remove << r.jobId;
        }
    }
    for (const QString& id : remove)
        m_jobs->removeFinishedRow(id);
    persistQueue();
}

void ExportController::dropKept(bool unfinishedOnly) {
    for (const ExportJobModel::Row& r : m_jobs->rows()) {
        if (unfinishedOnly && r.finished)
            continue;
        if (const auto it = m_kept.find(r.jobId); it != m_kept.end())
            it->second->dropped = true;
    }
    persistQueue();
}

QString ExportController::retryKept(const QString& jobId) {
    const ExportJobModel::Row* row = m_jobs->find(jobId);
    const auto it = m_kept.find(jobId);
    if (!row || it == m_kept.end() || !row->finished || row->state == QLatin1String("done")) {
        setLastError(translate("This export cannot be retried: export it again from the Export panel."));
        return {};
    }
    const std::shared_ptr<KeptJob> kept = it->second;
    // The same file: an interrupted export left no file of its own (it writes a temporary one), so a file there from
    // before it started is the one it was to replace. "name (2).ext" when a file was written there since or another job
    // writes it now (fix round 1, ARGON-11).
    const QString output = retryOutputPath(fromPath(kept->output),
                                           kept->startedAt.isValid() ? kept->startedAt : row->queuedAt,
                                           kept->replacedModified);
    ex::ExportRequest request;
    request.snapshot = kept->snapshot;
    request.sequence = SequenceId{kept->sequence};
    request.settings = kept->settings;
    request.output = toPath(output);
    request.projectFile = kept->projectFile;
    request.videoSource = m_videoSource;
    auto next = std::make_shared<KeptJob>(*kept);
    next->output = request.output;
    next->title = QString::fromStdU16String(request.output.filename().u16string());
    next->startedAt = {};
    next->replacedModified = {};
    next->dropped = false;
    const bool interrupted = row->state == QLatin1String("interrupted");
    const QString job = newJobId();
    m_kept[job] = next;
    m_restoring = true;
    queueRequestAs(job, std::move(request), kept->presetName, false, kept->credentials, kept->origin);
    m_restoring = false;
    m_jobs->setRequest(job, kept->sequenceKey, kept->presetId, kept->overrides);
    if (interrupted) {
        // The interrupted row is replaced by its retry.
        m_kept.erase(jobId);
        m_jobs->removeFinishedRow(jobId);
    }
    setLastError({});
    persistQueue();
    return job;
}

} // namespace wabcut::app
