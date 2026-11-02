// ExportController's batch exports (P1 round, stream 2C), kept out of ExportController.cpp:
//   #22 several sequences at once (sequenceExports / queueSequences): the Library's Export… and Send to Export Queue…
//   #26 each marker range or selected clip as its own file (queueRanges, markerRanges, selectedClipRanges, describeRanges)
//   #25 the media usage report (mediaUsage / writeMediaUsage), a read command of the project written to a CSV or JSON file
// Every function here works on the GUI thread through the controller's own queueExportAs, so a batch job is an ordinary
// export job: it shows in the queue, keeps its request for Retry and is kept across restarts (ExportQueueKeeper.cpp).

#include "ExportController.h"

#include "BridgeIds.h"
#include "DiskSpace.h"
#include "ExportInternal.h"
#include "GamingExport.h"
#include "ProjectController.h"

#include <wabcut/command/Engine.h>
#include <wabcut/export/ExportPresets.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/Timecode.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace wabcut::app {

namespace {

namespace ex = exporter;

QString translate(const char* text, const char* disambiguation = nullptr, int n = -1) {
    return QCoreApplication::translate("ExportController", text, disambiguation, n);
}

QString fromUtf8(std::string_view s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

/// "0:07", "1:23", "1:02:03".
QString clockText(double seconds) {
    const auto total = static_cast<long long>(std::max(0.0, std::round(seconds)));
    const long long h = total / 3600;
    const long long m = (total / 60) % 60;
    const long long s = total % 60;
    return h > 0 ? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'))
                 : QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

QVariant ticksVariant(Ticks t) {
    return QVariant::fromValue<qlonglong>(t.value);
}

/// A local folder from a path or a file URL ("" for anything else).
QString localFolderOf(const QString& pathOrUrl) {
    if (pathOrUrl.isEmpty())
        return {};
    const QUrl url(pathOrUrl);
    const QString path = url.isLocalFile() ? url.toLocalFile() : pathOrUrl;
    return QDir::isAbsolutePath(path) ? QDir::cleanPath(path) : QString();
}

/// A file name compared on any platform: case and separators do not tell two outputs apart.
QString nameKey(const QString& path) {
    return QDir::cleanPath(QDir::fromNativeSeparators(path)).toLower();
}

/// `path` is taken when it is on disk, an unfinished queue row writes it, or this batch named it already.
bool pathTaken(const QString& path, const ExportJobModel& jobs, const QStringList& used) {
    if (QFileInfo::exists(path))
        return true;
    const QString key = nameKey(path);
    if (used.contains(key))
        return true;
    return std::any_of(jobs.rows().begin(), jobs.rows().end(), [&](const ExportJobModel::Row& r) {
        return !r.finished && nameKey(r.outputPath) == key;
    });
}

/// "<folder>/<base><ext>", "<base> (2)<ext>" ... until free (against the disk, the queue and `used`); remembered in
/// `used`. Native separators.
QString takeOutput(const QString& folder, const QString& base, const QString& ext, const ExportJobModel& jobs,
                   QStringList& used) {
    QString path = QDir(folder).filePath(base + ext);
    for (int n = 2; pathTaken(path, jobs, used) && n < 10000; ++n)
        path = QDir(folder).filePath(QStringLiteral("%1 (%2)%3").arg(base).arg(n).arg(ext));
    used << nameKey(path);
    return QDir::toNativeSeparators(path);
}

/// The sequence behind a "s…" key ("" = the current sequence).
SequencePtr sequenceOf(const ExportController& c, const QString& key) {
    ProjectController* project = c.project();
    if (!project)
        return nullptr;
    const ProjectSnapshot snapshot = project->snapshot();
    if (!snapshot)
        return nullptr;
    if (key.isEmpty())
        return snapshot->sequencePtr(project->currentSequenceId());
    const auto id = parseSequenceKey(key);
    return id ? snapshot->sequencePtr(*id) : nullptr;
}

/// The extension (with the dot) an export of these settings writes.
QString extensionOf(const QString& presetId, const QVariantMap& overrides, QString* error) {
    const auto settings = ExportController::settingsFor(presetId, overrides);
    if (!settings) {
        if (error)
            *error = ExportController::plainError(settings.error());
        return {};
    }
    return fromUtf8(ex::outputExtension(*settings));
}

/// The folder a batch writes to: the one asked for (it must exist), else the last export folder, else next to the
/// project (what defaultOutputPath picks). `error` says why a folder asked for is no good.
QString batchFolder(const ExportController& c, const QString& requested, QString* error) {
    if (!requested.isEmpty()) {
        const QString local = localFolderOf(requested);
        if (local.isEmpty() || !QFileInfo(local).isDir()) {
            if (error)
                *error = translate("The folder %1 is not there.").arg(QDir::toNativeSeparators(local.isEmpty() ? requested : local));
            return {};
        }
        return local;
    }
    return QFileInfo(c.defaultOutputPath(QString(), QStringLiteral("match-source"), {})).absolutePath();
}

QString timecodeText(const Sequence& sequence, Ticks t) {
    return QString::fromStdString(ticksToTimecode(t, sequence.frameRate, sequence.dropFrame));
}

/// Ticks from a number (ticks) or a timecode text in the sequence's rate.
std::optional<Ticks> ticksOf(const QVariant& value, const Sequence& sequence) {
    if (!value.isValid() || value.isNull())
        return std::nullopt;
    if (value.typeId() == QMetaType::QString) {
        const auto t = parseTimecode(value.toString().trimmed().toStdString(), sequence.frameRate, sequence.dropFrame);
        return t ? std::optional<Ticks>(*t) : std::nullopt;
    }
    bool ok = false;
    const qlonglong n = value.toLongLong(&ok);
    return ok ? std::optional<Ticks>(Ticks{n}) : std::nullopt;
}

/// One range of a ranges batch: what it is called, where it lies and the file it makes.
struct RangeItem {
    QString name;
    Ticks in;
    Ticks out;
    QString output;
};

/// Everything a ranges batch needs, checked: nothing is queued unless `error` is empty.
struct RangePlan {
    QString error;
    QString folder;
    QString extension;
    QVariantMap base; ///< the overrides every range's export shares (no range, output or folder keys)
    std::vector<RangeItem> items;
    QStringList offline; ///< offline media the ranges would read (each name once)
};

/// The batch's own keys and the ones it sets per range: none of them reach an export.
const QStringList& batchKeys() {
    static const QStringList keys{QStringLiteral("folder"),  QStringLiteral("output"),   QStringLiteral("range"),
                                  QStringLiteral("rangeIn"), QStringLiteral("rangeOut"), QStringLiteral("customRange"),
                                  QStringLiteral("hold")};
    return keys;
}

RangePlan planRanges(const ExportController& c, const QString& sequenceId, const QString& presetId,
                     const QVariantList& ranges, const QVariantMap& overrides) {
    RangePlan plan;
    const SequencePtr sequence = sequenceOf(c, sequenceId);
    if (!sequence) {
        plan.error = translate("There is no sequence to export.");
        return plan;
    }
    if (ranges.isEmpty()) {
        plan.error = translate("There are no ranges to export.");
        return plan;
    }
    plan.base = overrides;
    for (const QString& key : batchKeys())
        plan.base.remove(key);
    QString extensionError;
    plan.extension = extensionOf(presetId, plan.base, &extensionError);
    if (plan.extension.isEmpty()) {
        plan.error = extensionError;
        return plan;
    }
    // The folder: `folder`, else the folder of `output` (a file path), else the usual default.
    QString requested = overrides.value(QStringLiteral("folder")).toString();
    if (requested.isEmpty() && overrides.contains(QStringLiteral("output"))) {
        const QString local = localFolderOf(overrides.value(QStringLiteral("output")).toString());
        requested = QFileInfo(local).isDir() ? local : QFileInfo(local).absolutePath();
    }
    QString folderError;
    plan.folder = batchFolder(c, requested, &folderError);
    if (plan.folder.isEmpty()) {
        plan.error = folderError;
        return plan;
    }
    const QString sequenceName = exportdetail::safeFileName(QString::fromStdString(sequence->name));
    const Ticks end = sequence->duration;
    QStringList used;
    int number = 0;
    for (const QVariant& entry : ranges) {
        ++number;
        const QVariantMap r = entry.toMap();
        QString name = r.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty())
            name = translate("Range %1").arg(number);
        const auto in = ticksOf(r.value(QStringLiteral("in")), *sequence);
        auto out = ticksOf(r.value(QStringLiteral("out")), *sequence);
        if (!in || !out) {
            plan.error = translate("“%1” has no In and Out point.").arg(name);
            return plan;
        }
        if (end.value > 0 && out->value > end.value)
            out = end; // a marker that runs past the end of the sequence ends with it
        if (in->value < 0 || !(*in < *out)) {
            plan.error = translate("“%1” is empty: its Out point is not after its In point.").arg(name);
            return plan;
        }
        RangeItem item;
        item.name = name;
        item.in = *in;
        item.out = *out;
        item.output = takeOutput(plan.folder, sequenceName + QStringLiteral(" - ") + exportdetail::safeFileName(name),
                                 plan.extension, *c.jobs(), used);
        plan.items.push_back(std::move(item));
    }
    // Every export must be able to run before the first is queued: one that cannot (a preset that does not fit, media
    // that is offline) queues none.
    for (const RangeItem& item : plan.items) {
        QVariantMap o = plan.base;
        o[QStringLiteral("rangeIn")] = ticksVariant(item.in);
        o[QStringLiteral("rangeOut")] = ticksVariant(item.out);
        o[QStringLiteral("output")] = item.output;
        const QVariantMap check = c.describe(sequenceId, presetId, o);
        if (!check.value(QStringLiteral("ok")).toBool()) {
            plan.error = translate("“%1”: %2").arg(item.name, check.value(QStringLiteral("error")).toString());
            return plan;
        }
        for (const QString& name : check.value(QStringLiteral("offline")).toStringList()) {
            if (!plan.offline.contains(name))
                plan.offline << name;
        }
    }
    if (!plan.offline.isEmpty()) {
        plan.error = translate("%n offline file(s) in these exports: %1. Relink them to export.", "", static_cast<int>(plan.offline.size()))
                         .arg(plan.offline.join(QStringLiteral(", ")));
    }
    return plan;
}

/// A built-in, user or gaming preset's display name.
QString presetLabel(const GamingExport& gaming, const QString& gamingId, const QString& presetId) {
    if (!gamingId.isEmpty()) {
        for (const QVariant& p : gaming.presets()) {
            if (p.toMap().value(QStringLiteral("id")).toString() == gamingId)
                return p.toMap().value(QStringLiteral("name")).toString();
        }
    }
    if (const auto user = ExportController::userPresetStore()->find(presetId.toStdString()))
        return fromUtf8(user->name);
    return exportdetail::presetDisplayName(presetId.toStdString());
}

/// What a sequence exports with: the preset asked for, a gaming preset, or (matchShape) the one remembered for the
/// sequence's shape - else YouTube 1080p60 for a landscape and Shorts / TikTok / Reels for a vertical sequence.
struct Choice {
    QString presetId;
    QVariantMap overrides;
    QString label;
    QString error;
};

Choice choosePreset(const ExportController& c, const GamingExport& gaming, const QString& key, const Sequence& sequence,
                    const QVariantMap& options) {
    Choice out;
    QString presetId = options.value(QStringLiteral("preset")).toString();
    QString gamingId = options.value(QStringLiteral("gaming")).toString();
    const QString range = options.value(QStringLiteral("range")).toString();
    if (presetId.isEmpty() && gamingId.isEmpty()) { // matchShape
        const QVariantMap remembered = c.rememberedFor(key);
        gamingId = remembered.value(QStringLiteral("gamingPreset")).toString();
        if (gamingId.isEmpty()) {
            const QString last = remembered.value(QStringLiteral("preset")).toString();
            if (!last.isEmpty() && last != QLatin1String("match-source"))
                presetId = last;
            else
                gamingId = sequence.height > sequence.width ? QStringLiteral("vertical-1080x1920-60")
                                                             : QStringLiteral("youtube-1080p60");
        }
    }
    if (presetId.isEmpty()) {
        const QVariantMap plan = gaming.plan(gamingId, key, range);
        if (!plan.value(QStringLiteral("ok")).toBool()) {
            out.error = plan.value(QStringLiteral("error")).toString();
            if (out.error.isEmpty())
                out.error = translate("This preset is not available.");
            return out;
        }
        out.presetId = plan.value(QStringLiteral("presetId")).toString();
        out.overrides = plan.value(QStringLiteral("overrides")).toMap();
        out.label = presetLabel(gaming, gamingId, out.presetId);
        return out;
    }
    if (!ExportController::userPresetStore()->findAny(presetId.toStdString())) {
        out.error = translate("Unknown preset %1").arg(presetId);
        return out;
    }
    out.presetId = presetId;
    if (!range.isEmpty() && range != QLatin1String("whole"))
        out.overrides.insert(QStringLiteral("range"), range);
    out.label = presetLabel(gaming, QString(), presetId);
    return out;
}

} // namespace

// ---- #22: several sequences ----------------------------------------------------------------------------------------

QVariantList ExportController::sequenceExports(const QStringList& sequenceIds, const QVariantMap& options) const {
    QVariantList list;
    QString folderError;
    const QString folder = batchFolder(*this, options.value(QStringLiteral("folder")).toString(), &folderError);
    // The preset cards' plans: a local GamingExport over this controller's project (it only reads).
    GamingExport gaming;
    gaming.setController(project());
    gaming.setExport(const_cast<ExportController*>(this));
    QStringList used;
    for (const QString& key : sequenceIds) {
        if (key.isEmpty())
            continue;
        QVariantMap e{{QStringLiteral("key"), key},
                      {QStringLiteral("ok"), false},
                      {QStringLiteral("error"), QString()},
                      {QStringLiteral("offlineCount"), 0}};
        const SequencePtr sequence = sequenceOf(*this, key);
        if (!sequence) {
            e[QStringLiteral("error")] = translate("This sequence is not in the project any more.");
            list << e;
            continue;
        }
        e[QStringLiteral("name")] = QString::fromStdString(sequence->name);
        e[QStringLiteral("width")] = sequence->width;
        e[QStringLiteral("height")] = sequence->height;
        e[QStringLiteral("portrait")] = sequence->height > sequence->width;
        if (!folderError.isEmpty()) {
            e[QStringLiteral("error")] = folderError;
            list << e;
            continue;
        }
        const Choice choice = choosePreset(*this, gaming, key, *sequence, options);
        if (!choice.error.isEmpty()) {
            e[QStringLiteral("error")] = choice.error;
            list << e;
            continue;
        }
        QString extensionError;
        const QString extension = extensionOf(choice.presetId, choice.overrides, &extensionError);
        e[QStringLiteral("presetId")] = choice.presetId;
        e[QStringLiteral("overrides")] = choice.overrides;
        e[QStringLiteral("presetName")] = choice.label;
        if (extension.isEmpty()) {
            e[QStringLiteral("error")] = extensionError;
            list << e;
            continue;
        }
        const QString output = takeOutput(folder, exportdetail::safeFileName(QString::fromStdString(sequence->name)), extension,
                                          *jobs(), used);
        e[QStringLiteral("output")] = output;
        QVariantMap overrides = choice.overrides;
        overrides[QStringLiteral("output")] = output;
        const QVariantMap check = describe(key, choice.presetId, overrides);
        if (!check.value(QStringLiteral("ok")).toBool()) {
            e[QStringLiteral("error")] = check.value(QStringLiteral("error")).toString();
        } else {
            e[QStringLiteral("ok")] = true;
        }
        e[QStringLiteral("offlineCount")] = check.value(QStringLiteral("offlineCount")).toInt();
        e[QStringLiteral("offline")] = check.value(QStringLiteral("offline")).toStringList();
        list << e;
    }
    return list;
}

QVariantMap ExportController::queueSequences(const QStringList& sequenceIds, const QVariantMap& options) {
    QVariantMap result{{QStringLiteral("ok"), false},
                       {QStringLiteral("jobs"), QStringList()},
                       {QStringLiteral("count"), 0},
                       {QStringLiteral("error"), QString()}};
    const auto fail = [&](const QString& text) {
        setLastError(text);
        result[QStringLiteral("error")] = text;
        return result;
    };
    const QVariantList entries = sequenceExports(sequenceIds, options);
    if (entries.isEmpty())
        return fail(translate("Choose at least one sequence to export."));
    // All or nothing: a sequence that cannot export (no such preset, offline media, ...) queues none.
    for (const QVariant& entry : entries) {
        const QVariantMap e = entry.toMap();
        const QString name = e.value(QStringLiteral("name")).toString();
        if (!e.value(QStringLiteral("ok")).toBool())
            return fail(name.isEmpty() ? e.value(QStringLiteral("error")).toString()
                                       : translate("“%1”: %2").arg(name, e.value(QStringLiteral("error")).toString()));
        if (const QStringList offline = e.value(QStringLiteral("offline")).toStringList(); !offline.isEmpty())
            return fail(translate("“%1” has %n offline file(s): %2. Relink them to export.", "", static_cast<int>(offline.size()))
                            .arg(name, offline.join(QStringLiteral(", "))));
    }
    const bool hold = options.value(QStringLiteral("hold")).toBool();
    QStringList jobs;
    for (const QVariant& entry : entries) {
        const QVariantMap e = entry.toMap();
        QVariantMap overrides = e.value(QStringLiteral("overrides")).toMap();
        overrides[QStringLiteral("output")] = e.value(QStringLiteral("output")).toString();
        overrides[QStringLiteral("hold")] = hold;
        const QString job = queueExportAs(e.value(QStringLiteral("key")).toString(), e.value(QStringLiteral("presetId")).toString(),
                                          overrides, QStringLiteral("queue"));
        if (job.isEmpty()) {
            const QString why = lastError();
            for (const QString& queued : jobs)
                cancel(queued); // the ones already queued by this batch go again
            return fail(why);
        }
        jobs << job;
    }
    setLastError({});
    result[QStringLiteral("ok")] = true;
    result[QStringLiteral("jobs")] = jobs;
    result[QStringLiteral("count")] = static_cast<int>(jobs.size());
    return result;
}

// ---- #26: each marker range or selected clip as its own file ---------------------------------------------------------

QVariantList ExportController::markerRanges(const QString& sequenceId, int colorIndex) const {
    QVariantList list;
    const SequencePtr sequence = sequenceOf(*this, sequenceId);
    if (!sequence)
        return list;
    for (std::size_t i = 0; i < sequence->markers.size(); ++i) {
        const Marker& m = sequence->markers[i];
        if (m.duration.value <= 0 || (colorIndex >= 0 && m.colorIndex != colorIndex))
            continue;
        QString name = QString::fromStdString(m.name).trimmed();
        if (name.isEmpty())
            name = translate("Marker %1").arg(i + 1);
        list.append(QVariantMap{{QStringLiteral("in"), ticksVariant(m.start)},
                                {QStringLiteral("out"), ticksVariant(m.end())},
                                {QStringLiteral("inText"), timecodeText(*sequence, m.start)},
                                {QStringLiteral("outText"), timecodeText(*sequence, m.end())},
                                {QStringLiteral("seconds"), m.duration.toSeconds()},
                                {QStringLiteral("name"), name},
                                {QStringLiteral("colorIndex"), m.colorIndex}});
    }
    return list; // the sequence keeps its markers sorted by start
}

QVariantList ExportController::selectedClipRanges() const {
    QVariantList list;
    ProjectController* controller = project();
    if (!controller)
        return list;
    const SequencePtr sequence = controller->currentSequence();
    if (!sequence)
        return list;
    struct Span {
        Ticks in;
        Ticks out;
        QString name;
        LinkGroupId link;
    };
    std::vector<Span> spans;
    for (const QString& key : controller->selectedClips()) {
        const auto id = parseClipKey(key);
        const auto location = id ? sequence->findClip(*id) : std::nullopt;
        if (!location)
            continue;
        const Clip& clip = location->clip();
        if (clip.duration().value <= 0)
            continue;
        // A linked video and audio clip are one piece of the timeline: one file.
        const auto same = clip.linkGroup.isValid()
                              ? std::find_if(spans.begin(), spans.end(), [&](const Span& s) { return s.link == clip.linkGroup; })
                              : spans.end();
        if (same != spans.end()) {
            same->in = std::min(same->in, clip.start());
            same->out = std::max(same->out, clip.end());
            continue;
        }
        spans.push_back({clip.start(), clip.end(), QString::fromStdString(clip.name()).trimmed(), clip.linkGroup});
    }
    std::stable_sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.in < b.in || (a.in == b.in && a.out < b.out); });
    for (const Span& s : spans) {
        QString name = s.name;
        if (name.isEmpty())
            name = translate("Clip %1").arg(list.size() + 1);
        list.append(QVariantMap{{QStringLiteral("in"), ticksVariant(s.in)},
                                {QStringLiteral("out"), ticksVariant(s.out)},
                                {QStringLiteral("inText"), timecodeText(*sequence, s.in)},
                                {QStringLiteral("outText"), timecodeText(*sequence, s.out)},
                                {QStringLiteral("seconds"), (s.out - s.in).toSeconds()},
                                {QStringLiteral("name"), name}});
    }
    return list;
}

QStringList ExportController::queueRanges(const QString& sequenceId, const QString& presetId, const QVariantList& ranges,
                                          const QVariantMap& overrides, bool hold) {
    if (!project()) {
        setLastError(translate("There is no project to export."));
        return {};
    }
    const RangePlan plan = planRanges(*this, sequenceId, presetId, ranges, overrides);
    if (!plan.error.isEmpty()) {
        setLastError(plan.error);
        return {};
    }
    QStringList jobs;
    for (const RangeItem& item : plan.items) {
        QVariantMap o = plan.base;
        o[QStringLiteral("rangeIn")] = ticksVariant(item.in);
        o[QStringLiteral("rangeOut")] = ticksVariant(item.out);
        o[QStringLiteral("output")] = item.output;
        o[QStringLiteral("hold")] = hold;
        const QString job = queueExportAs(sequenceId, presetId, o, hold ? QStringLiteral("queue") : QStringLiteral("export"));
        if (job.isEmpty()) {
            const QString why = lastError();
            for (const QString& queued : jobs)
                cancel(queued); // nothing of a refused batch stays queued
            setLastError(why);
            return {};
        }
        jobs << job;
    }
    setLastError({});
    return jobs;
}

QVariantMap ExportController::describeRanges(const QString& sequenceId, const QString& presetId, const QVariantList& ranges,
                                             const QVariantMap& overrides) const {
    QVariantMap d{{QStringLiteral("ok"), false},
                  {QStringLiteral("error"), QString()},
                  {QStringLiteral("count"), static_cast<int>(ranges.size())},
                  {QStringLiteral("outputs"), QStringList()},
                  {QStringLiteral("folder"), QString()},
                  {QStringLiteral("bytes"), 0.0},
                  {QStringLiteral("sizeText"), QString()},
                  {QStringLiteral("seconds"), 0.0},
                  {QStringLiteral("durationText"), QString()},
                  {QStringLiteral("offline"), QStringList()},
                  {QStringLiteral("lowSpace"), false},
                  {QStringLiteral("lowSpaceNote"), QString()}};
    const RangePlan plan = planRanges(*this, sequenceId, presetId, ranges, overrides);
    d[QStringLiteral("folder")] = QDir::toNativeSeparators(plan.folder);
    d[QStringLiteral("offline")] = plan.offline;
    QStringList outputs;
    for (const RangeItem& item : plan.items)
        outputs << QFileInfo(item.output).fileName();
    d[QStringLiteral("outputs")] = outputs;
    if (!plan.error.isEmpty()) {
        d[QStringLiteral("error")] = plan.error;
        return d;
    }
    // The size: GamingExport's estimate of every range, added up.
    GamingExport gaming;
    gaming.setController(project());
    gaming.setExport(const_cast<ExportController*>(this));
    double bytes = 0.0;
    double seconds = 0.0;
    for (const RangeItem& item : plan.items) {
        QVariantMap o = plan.base;
        o[QStringLiteral("rangeIn")] = ticksVariant(item.in);
        o[QStringLiteral("rangeOut")] = ticksVariant(item.out);
        o[QStringLiteral("output")] = item.output;
        const QVariantMap estimate = gaming.estimate(sequenceId, presetId, o);
        if (!estimate.value(QStringLiteral("ok")).toBool()) {
            d[QStringLiteral("error")] = estimate.value(QStringLiteral("error")).toString();
            return d;
        }
        bytes += estimate.value(QStringLiteral("bytes")).toDouble();
        seconds += estimate.value(QStringLiteral("seconds")).toDouble();
    }
    d[QStringLiteral("ok")] = true;
    d[QStringLiteral("bytes")] = bytes;
    d[QStringLiteral("sizeText")] = GamingExport::sizeText(bytes);
    d[QStringLiteral("seconds")] = seconds;
    d[QStringLiteral("durationText")] = clockText(seconds);
    const QString low = diskspace::lowSpaceMessage(plan.folder, bytes, translate("these exports"));
    d[QStringLiteral("lowSpace")] = !low.isEmpty();
    d[QStringLiteral("lowSpaceNote")] = low;
    return d;
}

// ---- #25: the media usage report ---------------------------------------------------------------------------------------

QVariantMap ExportController::mediaUsage(const QString& sequenceId, const QVariantMap& options) const {
    QVariantMap result{{QStringLiteral("ok"), false}, {QStringLiteral("text"), QString()}, {QStringLiteral("uses"), 0},
                       {QStringLiteral("media"), 0}, {QStringLiteral("error"), QString()}};
    ProjectController* controller = project();
    const SequencePtr sequence = sequenceOf(*this, sequenceId);
    if (!controller || !sequence) {
        result[QStringLiteral("error")] = translate("There is no sequence to report on.");
        return result;
    }
    // sequence.mediaUsage, read only, on the open project (unsaved edits included).
    command::EnginePolicy policy;
    policy.allowed = command::CapabilitySet::of({command::Capability::Read});
    command::CommandEngine engine(controller->session(), command::builtinCommands(), policy);
    command::CommandRequest request;
    request.command = "sequence.mediaUsage";
    request.params = command::Json{{"sequence", sequence->id.value}};
    const QString range = options.value(QStringLiteral("range")).toString();
    if (!range.isEmpty())
        request.params["range"] = range.toStdString();
    const QString format = options.value(QStringLiteral("format")).toString();
    if (!format.isEmpty())
        request.params["format"] = format.toStdString();
    if (options.contains(QStringLiteral("flatten")))
        request.params["flatten"] = options.value(QStringLiteral("flatten")).toBool();
    const command::CommandResult r = engine.execute(request);
    if (!r.ok()) {
        const std::string message = r.error->message;
        result[QStringLiteral("error")] = message == "the sequence has no In and Out marks"
                                              ? translate("Set an In and an Out point first.")
                                              : QString::fromStdString(message);
        return result;
    }
    result[QStringLiteral("ok")] = true;
    result[QStringLiteral("text")] = QString::fromStdString(r.data.value("text", std::string()));
    result[QStringLiteral("uses")] = r.data.value("uses", 0);
    result[QStringLiteral("media")] = r.data.value("media", 0);
    return result;
}

QVariantMap ExportController::writeMediaUsage(const QString& file, const QString& sequenceId, const QVariantMap& options) {
    QVariantMap result = mediaUsage(sequenceId, options);
    if (!result.value(QStringLiteral("ok")).toBool())
        return result;
    const QUrl url(file);
    const QString path = QDir::toNativeSeparators(url.isLocalFile() ? url.toLocalFile() : file);
    if (path.isEmpty() || !QDir::isAbsolutePath(QDir::fromNativeSeparators(path))) {
        result[QStringLiteral("ok")] = false;
        result[QStringLiteral("error")] = translate("Choose where to save the report.");
        return result;
    }
    QString text = result.value(QStringLiteral("text")).toString();
    // Spreadsheets (Excel) read a CSV as UTF-8 only with a byte order mark: names with accents or Thai stay readable.
    QByteArray bytes = text.toUtf8();
    if (!path.endsWith(QLatin1String(".json"), Qt::CaseInsensitive))
        bytes.prepend("\xEF\xBB\xBF");
    const auto wrote = io::writeFileAtomically(std::filesystem::path(path.toStdU16String()),
                                               std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
    if (!wrote) {
        result[QStringLiteral("ok")] = false;
        result[QStringLiteral("error")] = translate("The report could not be written to %1: %2")
                                              .arg(path, QString::fromStdString(wrote.error().message));
        return result;
    }
    result.remove(QStringLiteral("text"));
    result[QStringLiteral("path")] = path;
    return result;
}

} // namespace wabcut::app
