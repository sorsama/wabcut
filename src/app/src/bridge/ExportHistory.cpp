// ExportController's export history (0.7, card 004; Media Encoder keeps an encoding log) and the glue of the pre-export
// check (card 176; the checks themselves are exporter::preExportCheck, src/export/src/PreExportCheck.cpp).
//
// History: when an export finishes (done), its file, preset, sequence, project and the request it was queued with
// (sequence, preset, overrides) are added, newest first, to a list of at most 100 kept in QSettings export/history as
// JSON. The Export Queue dialog's History tab lists them with Show, Open and Export Again; Export Again queues the same
// request on the project as it is now (the same file, or a numbered one when it exists). Entries of other projects
// stay listed (Show and Open work); Export Again says to open that project first.

#include "ExportController.h"

#include "BridgeIds.h"
#include "ExportInternal.h"
#include "ProjectController.h"

#include <wabcut/model/Time.h>
#include <wabcut/model/Timecode.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QPointer>
#include <QSettings>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace wabcut::app {

namespace {

namespace ex = exporter;

QString translate(const char* text, const char* disambiguation = nullptr, int n = -1) {
    return QCoreApplication::translate("ExportController", text, disambiguation, n);
}

const QString kHistoryKey = QStringLiteral("export/history");
const QString kCheckBeforeKey = QStringLiteral("export/checkBefore");
constexpr int kMaxHistory = 100;

QJsonArray loadHistoryJson() {
    const QSettings settings;
    return QJsonDocument::fromJson(settings.value(kHistoryKey).toString().toUtf8()).array();
}

void saveHistoryJson(const QJsonArray& entries) {
    QSettings settings;
    if (entries.isEmpty()) {
        settings.remove(kHistoryKey);
    } else {
        settings.setValue(kHistoryKey, QString::fromUtf8(QJsonDocument(entries).toJson(QJsonDocument::Compact)));
    }
}

QString nativePath(const QString& path) {
    return QDir::toNativeSeparators(path);
}

} // namespace

// ---- Card 004: history -------------------------------------------------------------------------------------------

void ExportController::recordHistory(const ExportJobModel::Row& row) {
    if (row.state != QLatin1String("done") || row.outputPath.isEmpty()) {
        return;
    }
    QJsonObject entry;
    entry[QStringLiteral("when")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    entry[QStringLiteral("title")] = row.title;
    entry[QStringLiteral("outputPath")] = nativePath(row.outputPath);
    entry[QStringLiteral("presetName")] = row.presetName;
    entry[QStringLiteral("presetId")] = row.presetId;
    entry[QStringLiteral("sequenceId")] = row.sequenceId;
    entry[QStringLiteral("origin")] = row.origin;
    entry[QStringLiteral("detail")] = row.detail;
    if (!row.presetId.isEmpty()) {
        QVariantMap overrides = row.overrides;
        overrides.remove(QStringLiteral("hold")); // an export again starts at once
        entry[QStringLiteral("overrides")] = QJsonObject::fromVariantMap(overrides);
    }
    QString sequenceName;
    if (const SequencePtr sequence = row.sequenceId.isEmpty() ? nullptr : sequenceFor(row.sequenceId)) {
        sequenceName = QString::fromStdString(sequence->name);
    }
    entry[QStringLiteral("sequenceName")] = sequenceName;
    entry[QStringLiteral("projectPath")] = m_projectFile.empty() ? QString() : nativePath(QString::fromStdU16String(m_projectFile.u16string()));
    entry[QStringLiteral("projectName")] = m_projectFile.empty() ? translate("Untitled project")
                                                                   : QString::fromStdU16String(m_projectFile.stem().u16string());
    QJsonArray entries = loadHistoryJson();
    entries.prepend(entry);
    while (entries.size() > kMaxHistory) {
        entries.removeLast();
    }
    saveHistoryJson(entries);
    m_historyLoaded = false;
    emit historyChanged();
}

QVariantList ExportController::history() const {
    if (!m_historyLoaded) {
        const_cast<ExportController*>(this)->rebuildHistory(); // no signal from inside a binding
    }
    return m_history;
}

void ExportController::refreshHistory() {
    rebuildHistory();
    emit historyChanged();
}

void ExportController::rebuildHistory() {
    QVariantList list;
    const QJsonArray entries = loadHistoryJson();
    int index = 0;
    for (const QJsonValue& value : entries) {
        const QJsonObject e = value.toObject();
        const QString path = e.value(QStringLiteral("outputPath")).toString();
        const QFileInfo file(path);
        const QDateTime when = QDateTime::fromString(e.value(QStringLiteral("when")).toString(), Qt::ISODate);
        QVariantMap row;
        row[QStringLiteral("index")] = index++;
        row[QStringLiteral("when")] = e.value(QStringLiteral("when")).toString();
        row[QStringLiteral("whenText")] = when.isValid() ? QLocale().toString(when.toLocalTime(), QLocale::ShortFormat) : QString();
        row[QStringLiteral("title")] = e.value(QStringLiteral("title")).toString();
        row[QStringLiteral("outputPath")] = path;
        row[QStringLiteral("folder")] = nativePath(file.absolutePath());
        row[QStringLiteral("presetName")] = e.value(QStringLiteral("presetName")).toString();
        row[QStringLiteral("sequenceName")] = e.value(QStringLiteral("sequenceName")).toString();
        row[QStringLiteral("projectName")] = e.value(QStringLiteral("projectName")).toString();
        row[QStringLiteral("detail")] = e.value(QStringLiteral("detail")).toString();
        row[QStringLiteral("exists")] = file.isFile();
        row[QStringLiteral("canRepeat")] = !e.value(QStringLiteral("presetId")).toString().isEmpty();
        list.append(row);
    }
    m_history = std::move(list);
    m_historyLoaded = true;
}

QString ExportController::exportAgain(int index) {
    const QJsonArray entries = loadHistoryJson();
    if (index < 0 || index >= entries.size()) {
        setLastError(translate("That export is not in the history any more."));
        return {};
    }
    const QJsonObject e = entries.at(index).toObject();
    const QString presetId = e.value(QStringLiteral("presetId")).toString();
    if (presetId.isEmpty()) {
        setLastError(translate("This export cannot be repeated from the history: export it again from the Export panel."));
        return {};
    }
    const QString project = e.value(QStringLiteral("projectPath")).toString();
    const QString current = m_projectFile.empty() ? QString() : nativePath(QString::fromStdU16String(m_projectFile.u16string()));
    if (QDir::cleanPath(QDir::fromNativeSeparators(project)).compare(QDir::cleanPath(QDir::fromNativeSeparators(current)),
                                                                     Qt::CaseInsensitive) != 0) {
        setLastError(translate("This export was made from the project “%1”: open it first, then export again.")
                         .arg(e.value(QStringLiteral("projectName")).toString()));
        return {};
    }
    const QString sequenceId = e.value(QStringLiteral("sequenceId")).toString();
    if (!sequenceFor(sequenceId)) {
        setLastError(translate("The sequence “%1” is not in this project any more.")
                         .arg(e.value(QStringLiteral("sequenceName")).toString()));
        return {};
    }
    QVariantMap overrides = e.value(QStringLiteral("overrides")).toObject().toVariantMap();
    overrides.remove(QStringLiteral("hold"));
    const QString output = e.value(QStringLiteral("outputPath")).toString();
    if (!output.isEmpty()) {
        // The same file when it is not there (or an unfinished job would not write it), else "name (2).ext".
        overrides[QStringLiteral("output")] = nativePath(retryOutputPath(output, QDateTime(), QDateTime()));
    }
    const bool chosenOutput = !output.isEmpty();
    const QString folderBefore = lastFolder();
    const QString origin = e.value(QStringLiteral("origin")).toString();
    const QString job = queueExportAs(sequenceId, presetId, overrides, origin.isEmpty() || origin == QLatin1String("watch")
                                                                          ? QStringLiteral("export") : origin);
    if (chosenOutput) {
        setLastFolder(folderBefore); // an export again does not change where the next new export goes
    }
    return job;
}

bool ExportController::revealHistory(int index) {
    const QJsonArray entries = loadHistoryJson();
    if (index < 0 || index >= entries.size()) {
        return false;
    }
    const QString path = entries.at(index).toObject().value(QStringLiteral("outputPath")).toString();
    if (QFileInfo(path).isFile()) {
        return reveal(path);
    }
    const QString folder = QFileInfo(path).absolutePath();
    if (!QDir(folder).exists()) {
        return false;
    }
    if (m_reveal) {
        return m_reveal(folder);
    }
    return QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

bool ExportController::openHistory(int index) {
    const QJsonArray entries = loadHistoryJson();
    if (index < 0 || index >= entries.size()) {
        return false;
    }
    return openOutput(entries.at(index).toObject().value(QStringLiteral("outputPath")).toString());
}

void ExportController::removeHistory(int index) {
    QJsonArray entries = loadHistoryJson();
    if (index < 0 || index >= entries.size()) {
        return;
    }
    entries.removeAt(index);
    saveHistoryJson(entries);
    m_historyLoaded = false;
    emit historyChanged();
}

void ExportController::clearHistory() {
    saveHistoryJson({});
    m_historyLoaded = false;
    emit historyChanged();
}

// ---- Card 176: the pre-export check ----------------------------------------------------------------------------------

namespace {

QString durationText(Ticks t) {
    const double seconds = t.toSeconds();
    if (seconds < 60.0) {
        return translate("%1 s").arg(seconds, 0, 'f', seconds < 10.0 ? 1 : 0);
    }
    const int total = static_cast<int>(std::lround(seconds));
    return QStringLiteral("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QLatin1Char('0'));
}

/// "16:9", "9:16", ... for the shapes people know, else "1.85:1".
QString aspectText(double ratio) {
    struct Known {
        double ratio;
        const char* text;
    };
    static constexpr Known kKnown[] = {{16.0 / 9.0, "16:9"}, {9.0 / 16.0, "9:16"}, {4.0 / 3.0, "4:3"}, {3.0 / 4.0, "3:4"},
                                       {1.0, "1:1"},         {21.0 / 9.0, "21:9"}, {4.0 / 5.0, "4:5"}, {5.0 / 4.0, "5:4"},
                                       {2.0, "2:1"},         {1.0 / 2.0, "1:2"}};
    for (const Known& k : kKnown) {
        if (std::abs(ratio / k.ratio - 1.0) < 0.02) {
            return QString::fromLatin1(k.text);
        }
    }
    return ratio >= 1.0 ? QStringLiteral("%1:1").arg(ratio, 0, 'f', 2) : QStringLiteral("1:%1").arg(1.0 / ratio, 0, 'f', 2);
}

QString fixText(ex::CheckFix fix) {
    switch (fix) {
    case ex::CheckFix::Relink:
        return translate("Relink…");
    case ex::CheckFix::CloseGap:
        return translate("Close gap");
    case ex::CheckFix::TurnOnLoudness:
        return translate("Normalize loudness");
    case ex::CheckFix::MatchSequenceSize:
        return translate("Use the sequence size");
    case ex::CheckFix::TurnOnCaptions:
        return translate("Burn captions in");
    case ex::CheckFix::None:
        break;
    }
    return {};
}

/// One row as QML sees it: the sentence in the user's language, and the place and fix.
QVariantMap rowOf(const ex::CheckIssue& i, const Sequence& sequence) {
    QVariantMap row;
    const QString subject = QString::fromStdString(i.subject);
    QString text;
    QString detail;
    const int count = static_cast<int>(std::min<std::int64_t>(i.count, 1'000'000));
    if (i.more) {
        text = translate("…and %n more like this", nullptr, count);
    } else {
        switch (i.kind) {
        case ex::CheckKind::InvalidSettings:
            text = translate("These export settings cannot be used: %1").arg(subject);
            break;
        case ex::CheckKind::OfflineMedia:
            text = translate("“%1” is offline or missing: the export would stop on it.").arg(subject);
            detail = translate("Relink the file, then export.");
            break;
        case ex::CheckKind::Gap:
            text = translate("No picture for %1: the export shows black there.").arg(durationText(i.length));
            detail = translate("Close the gap, or put something over it.");
            break;
        case ex::CheckKind::FlashFrame: {
            const int frames = static_cast<int>(ticksToFrames(i.length, sequence.frameRate, Rounding::Nearest).value_or(1));
            text = translate("Flash frame: “%1” is only %n frame(s) long.", nullptr, std::max(1, frames)).arg(subject);
            detail = translate("A stray cut leaves a clip this short; it flashes by in the export.");
            break;
        }
        case ex::CheckKind::MixedFrameRate:
            if (subject == QLatin1String("variable")) {
                text = translate("Variable frame rate in %n clip(s).", nullptr, count);
                detail = translate("Variable-rate recordings (screen and game captures) can drift out of sync with the sound.");
            } else {
                text = translate("%n clip(s) at %1 in a %2 sequence.", nullptr, count)
                           .arg(subject, QString::number(sequence.frameRate.toDouble(), 'g', 5) + QStringLiteral(" fps"));
                detail = translate("Frames are repeated or dropped to fit.");
            }
            break;
        case ex::CheckKind::UnanalysedStabilizer:
            text = translate("The stabilizer on “%1” was never analyzed: it exports unstabilized.").arg(subject);
            detail = translate("Analyze it in Effect Controls first.");
            break;
        case ex::CheckKind::SlowEffects:
            text = translate("Slow effects (%1) on %n clip(s).", nullptr, count).arg(subject);
            detail = translate("The export takes longer than the timeline's length.");
            break;
        case ex::CheckKind::SizeMismatch:
            if (subject == QLatin1String("upscale")) {
                text = translate("The export (%1 pixels wide) is larger than the sequence (%2): the picture is scaled up, not sharper.")
                           .arg(static_cast<int>(i.other))
                           .arg(static_cast<int>(i.value));
            } else {
                text = translate("The export is %1 but the sequence is %2.").arg(aspectText(i.other), aspectText(i.value));
                detail = subject == QLatin1String("crop")      ? translate("The picture is cropped to fill the frame.")
                         : subject == QLatin1String("stretch") ? translate("The picture is stretched to the frame.")
                                                               : translate("The picture is fitted with black bars.");
            }
            break;
        case ex::CheckKind::CaptionsNotInFile:
            text = translate("%n caption(s) in the sequence will not be in the file.", nullptr, count);
            detail = translate("Captions are off for this export.");
            break;
        case ex::CheckKind::CaptionsMissing:
            text = translate("Captions are on for this export, but the sequence has none in the exported range.");
            break;
        case ex::CheckKind::LoudnessOffTarget:
            text = translate("The loudness is %1 LUFS; %2 plays at %3 LUFS.")
                       .arg(i.value, 0, 'f', 1)
                       .arg(subject.isEmpty() ? translate("the destination") : subject)
                       .arg(i.other, 0, 'f', 0);
            detail = i.value < i.other ? translate("It will sound quiet next to other videos.")
                                       : translate("The platform turns it down, and loud peaks may distort.");
            break;
        case ex::CheckKind::DynamicMetadataDropped:
            if (subject == QLatin1String("dolbyVision")) {
                text = translate("Dolby Vision metadata of %n clip(s) is not kept.", nullptr, count);
                detail = translate("The file has HDR10 or HLG metadata only: Dolby Vision is not written by this build.");
            } else {
                text = translate("HDR10+ metadata of %n clip(s) is not kept.", nullptr, count);
                detail = translate("A render of edited pictures cannot keep it. Set Dynamic metadata to HDR10+ to measure it again.");
            }
            break;
        case ex::CheckKind::AudioClips:
            text = translate("The sound peaks at %1 dBTP: it clips.").arg(i.value, 0, 'f', 1);
            detail = translate("Normalizing the loudness brings the peaks under the limit.");
            break;
        }
    }
    row[QStringLiteral("kind")] = QString::fromLatin1(ex::checkKindName(i.kind).data());
    row[QStringLiteral("severity")] = QString::fromLatin1(ex::checkSeverityName(i.severity).data());
    row[QStringLiteral("text")] = text;
    row[QStringLiteral("detail")] = detail;
    row[QStringLiteral("hasPlace")] = i.at.has_value();
    row[QStringLiteral("ticks")] = i.at ? QVariant::fromValue<qlonglong>(i.at->value) : QVariant();
    row[QStringLiteral("timecode")] =
        i.at ? QString::fromStdString(ticksToTimecode(*i.at, sequence.frameRate, sequence.dropFrame)) : QString();
    row[QStringLiteral("fix")] = QString::fromLatin1(ex::checkFixName(i.fix).data());
    row[QStringLiteral("fixText")] = fixText(i.fix);
    row[QStringLiteral("clip")] = clipKey(i.clip);
    row[QStringLiteral("track")] = trackKey(i.track);
    row[QStringLiteral("more")] = i.more;
    return row;
}

} // namespace

QVariantList ExportController::preExportCheck(const QString& sequenceId, const QString& presetId,
                                              const QVariantMap& rawOverrides, const QVariantMap& platform) {
    m_lastCheck.clear();
    QVariantList rows;
    const SequencePtr sequence = sequenceFor(sequenceId);
    if (!sequence || !project()) {
        return rows;
    }
    const auto invalid = [&](const Error& e) {
        ex::CheckIssue issue;
        issue.kind = ex::CheckKind::InvalidSettings;
        issue.severity = ex::CheckSeverity::Problem;
        issue.subject = plainError(e).toStdString();
        m_lastCheck.push_back(issue);
        rows.append(rowOf(issue, *sequence));
        return rows;
    };
    const auto prepared = preparedOverrides(rawOverrides, sequence.get());
    if (!prepared) {
        return invalid(prepared.error());
    }
    const auto settings = settingsFor(presetId, *prepared);
    if (!settings) {
        return invalid(settings.error());
    }
    const ProjectSnapshot snapshot = project()->snapshot();
    if (!snapshot) {
        return rows;
    }
    ex::CheckContext context;
    context.projectFile = m_projectFile;
    // A measurement belongs to the project as it was measured: any edit makes a new snapshot and drops it.
    if (m_measuredLoudness.value(QStringLiteral("ok")).toBool() && m_measuredSnapshot == snapshot.get() &&
        m_measuredKey == sequenceId + QLatin1Char('|') + presetId) {
        context.loudness = ex::LoudnessReading{m_measuredLoudness.value(QStringLiteral("integratedLufs")).toDouble(),
                                               m_measuredLoudness.value(QStringLiteral("truePeakDbtp")).toDouble()};
    }
    if (platform.contains(QStringLiteral("lufs"))) {
        context.platformLufs = platform.value(QStringLiteral("lufs")).toDouble();
        context.platformName = platform.value(QStringLiteral("name")).toString().toStdString();
    }
    m_lastCheck = ex::preExportCheck(*snapshot, *sequence, *settings, context);
    for (const ex::CheckIssue& issue : m_lastCheck) {
        rows.append(rowOf(issue, *sequence));
    }
    return rows;
}

bool ExportController::goToCheckRow(const QVariantMap& row) {
    if (!project() || !row.value(QStringLiteral("hasPlace")).toBool()) {
        return false;
    }
    project()->setPlayhead(Ticks{row.value(QStringLiteral("ticks")).toLongLong()});
    return true;
}

void ExportController::measureLoudness(const QString& sequenceId, const QString& presetId, const QVariantMap& rawOverrides) {
    if (m_measuring || !project()) {
        return;
    }
    const SequencePtr sequence = sequenceFor(sequenceId);
    const auto fail = [&](const QString& why) {
        m_measuredLoudness = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), why}};
        emit measuringLoudnessChanged();
    };
    if (!sequence) {
        return fail(translate("There is no sequence to measure."));
    }
    const auto prepared = preparedOverrides(rawOverrides, sequence.get());
    if (!prepared) {
        return fail(plainError(prepared.error()));
    }
    const auto settings = settingsFor(presetId, *prepared);
    if (!settings) {
        return fail(plainError(settings.error()));
    }
    const ProjectSnapshot snapshot = project()->snapshot();
    if (!snapshot) {
        return fail(translate("There is no project to measure."));
    }
    m_measuring = true;
    m_measuredLoudness = {};
    emit measuringLoudnessChanged();
    const std::filesystem::path projectFile = m_projectFile;
    const QString key = sequenceId + QLatin1Char('|') + presetId;
    const ex::ExportSettings exportSettings = *settings;
    QPointer<ExportController> self(this);
    auto measurer = m_loudnessMeasurer;
    m_measureThread = std::jthread([=](const std::stop_token& stop) {
        std::expected<ex::LoudnessReading, Error> reading = std::unexpected(Error{ErrorCode::Cancelled, "Cancelled"});
        if (measurer) {
            const auto [lufs, peak] = measurer(stop);
            reading = ex::LoudnessReading{lufs, peak};
        } else {
            reading = ex::measureExportLoudness(*snapshot, *sequence, exportSettings, projectFile, stop);
        }
        QMetaObject::invokeMethod(
            self.data(),
            [self, snapshot, key, reading] {
                if (!self) {
                    return;
                }
                self->m_measuring = false;
                if (reading) {
                    self->m_measuredLoudness = {{QStringLiteral("ok"), true},
                                                {QStringLiteral("integratedLufs"), reading->integratedLufs},
                                                {QStringLiteral("truePeakDbtp"), reading->truePeakDbtp}};
                    self->m_measuredSnapshot = snapshot.get();
                    self->m_measuredKey = key;
                } else {
                    self->m_measuredLoudness = {{QStringLiteral("ok"), false},
                                                {QStringLiteral("cancelled"), reading.error().code == ErrorCode::Cancelled},
                                                {QStringLiteral("error"), QString::fromStdString(reading.error().message)}};
                }
                emit self->measuringLoudnessChanged();
            },
            Qt::QueuedConnection);
    });
}

void ExportController::cancelLoudnessMeasure() {
    m_measureThread.request_stop();
}

bool ExportController::checkBeforeExport() const {
    return QSettings().value(kCheckBeforeKey, true).toBool();
}

void ExportController::setCheckBeforeExport(bool on) {
    if (on == checkBeforeExport()) {
        return;
    }
    QSettings().setValue(kCheckBeforeKey, on);
    emit checkBeforeExportChanged();
}

} // namespace wabcut::app
