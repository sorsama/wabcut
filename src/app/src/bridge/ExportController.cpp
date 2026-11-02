#include <wabcut/ai/Provenance.h>
#include <wabcut/color/ColorSpaces.h>
#include <wabcut/model/ProjectIO.h>
#include "ExportController.h"

#include "BridgeIds.h"
#include "CaptionStyleMapping.h"
#include "CaptionsBridge.h"
#include "DiskSpace.h"
#include "ExportInternal.h"
#include "GamingExport.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"
#include "TranscriptBridge.h"

#include <wabcut/editor/SourceEdit.h>
#include <wabcut/export/Encoders.h>
#include <wabcut/export/ReviewFrames.h>
#include <wabcut/export/Stems.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/Timecode.h>
#include <wabcut/playback/video/Evaluate.h>

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QProcess>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QtQml/qqml.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <mutex>

namespace wabcut::app {

namespace {

namespace ex = exporter;

QString translate(const char* text) {
    return QCoreApplication::translate("ExportController", text);
}

QString fromUtf8(std::string_view s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

std::filesystem::path pathFromQString(const QString& s) {
    return std::filesystem::path(s.toStdU16String());
}
QString pathToQString(const std::filesystem::path& p) {
    return QDir::toNativeSeparators(QString::fromStdU16String(p.u16string()));
}

/// "0:07", "1:23", "1:02:03".
QString clockText(double seconds) {
    const auto total = static_cast<long long>(std::max(0.0, std::round(seconds)));
    const long long h = total / 3600;
    const long long m = (total / 60) % 60;
    const long long s = total % 60;
    return h > 0 ? QStringLiteral("%1:%2:%3")
                       .arg(h)
                       .arg(m, 2, 10, QLatin1Char('0'))
                       .arg(s, 2, 10, QLatin1Char('0'))
                 : QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

QString rateText(Rate rate) {
    const double fps = rate.toDouble();
    const bool whole = rate.den == 1 || std::abs(fps - std::round(fps)) < 1e-9;
    return translate("%1 fps").arg(whole ? QString::number(std::lround(fps)) : QString::number(fps, 'f', 2));
}

struct PresetText {
    const char* id;
    const char* name;
    const char* description;
    const char* icon;
};

// Translatable names of the built-in presets (exporter::builtInPresets() ids).
constexpr PresetText kPresetTexts[] = {
    {"match-source", QT_TRANSLATE_NOOP("ExportController", "Match Source"),
     QT_TRANSLATE_NOOP("ExportController", "H.264 · sequence size and rate"), "clapperboard"},
    {"youtube-1080p", QT_TRANSLATE_NOOP("ExportController", "YouTube 1080p"),
     QT_TRANSLATE_NOOP("ExportController", "H.264 1080p · 16 Mb/s"), "play"},
    {"youtube-4k", QT_TRANSLATE_NOOP("ExportController", "YouTube 4K"),
     QT_TRANSLATE_NOOP("ExportController", "H.264 2160p · 45 Mb/s"), "scaling"},
    {"vertical-1080x1920", QT_TRANSLATE_NOOP("ExportController", "TikTok / Reels"),
     QT_TRANSLATE_NOOP("ExportController", "9:16 · centre crop"), "move-vertical"},
    {"prores-422-hq", QT_TRANSLATE_NOOP("ExportController", "ProRes 422 HQ"),
     QT_TRANSLATE_NOOP("ExportController", "10-bit master · MOV"), "film"},
    {"audio-wav", QT_TRANSLATE_NOOP("ExportController", "Audio only"),
     QT_TRANSLATE_NOOP("ExportController", "WAV · 24-bit · 48 kHz"), "audio-lines"},
};

const PresetText* presetText(std::string_view id) {
    for (const PresetText& p : kPresetTexts) {
        if (id == p.id) {
            return &p;
        }
    }
    return nullptr;
}

QString presetName(std::string_view id) {
    if (const PresetText* p = presetText(id)) {
        return translate(p->name);
    }
    // The gaming presets (I.9) have no translated text yet: the export library's English name, not the raw id.
    const auto preset = ex::findPreset(id);
    return fromUtf8(preset ? std::string_view(preset->name) : id);
}

/// 0.6 (#52): a stem's name in the UI (the track roles of the Audio Track Mixer).
QString stemRoleName(TrackRole role) {
    switch (role) {
    case TrackRole::Dialogue:
        return translate("Dialogue");
    case TrackRole::Music:
        return translate("Music");
    case TrackRole::Effects:
        return translate("Effects");
    case TrackRole::None:
        break;
    }
    return translate("Other");
}

QString codecText(ex::VideoCodec c) {
    switch (c) {
    case ex::VideoCodec::H264:
        return QStringLiteral("H.264");
    case ex::VideoCodec::Hevc:
        return QStringLiteral("HEVC");
    case ex::VideoCodec::ProRes:
        return QStringLiteral("ProRes");
    case ex::VideoCodec::DnxHR:
        return QStringLiteral("DNxHR");
    case ex::VideoCodec::Av1:
        return QStringLiteral("AV1");
    case ex::VideoCodec::Gif:
        return QStringLiteral("GIF");
    case ex::VideoCodec::Vp9:
        return QStringLiteral("VP9");
    case ex::VideoCodec::Png:
        return QStringLiteral("PNG");
    case ex::VideoCodec::Tiff:
        return QStringLiteral("TIFF");
    case ex::VideoCodec::Dpx:
        return QStringLiteral("DPX");
    case ex::VideoCodec::Jpeg:
        return QStringLiteral("JPEG");
    case ex::VideoCodec::Targa:
        return QStringLiteral("Targa");
    case ex::VideoCodec::Exr:
        return QStringLiteral("OpenEXR");
    case ex::VideoCodec::Animation:
        return QStringLiteral("Animation");
    case ex::VideoCodec::CineForm:
        return QStringLiteral("CineForm");
    case ex::VideoCodec::Xdcam: // 0.6 (#73)
        return QStringLiteral("XDCAM HD422");
    case ex::VideoCodec::AvcIntra:
        return QStringLiteral("AVC-Intra 100");
    }
    return {};
}

/// 0.2: overrides as the export library reads them (QVariant values through JSON).
ex::Json jsonFromVariantMap(const QVariantMap& map) {
    const QByteArray bytes = QJsonDocument(QJsonObject::fromVariantMap(map)).toJson(QJsonDocument::Compact);
    try {
        return ex::Json::parse(bytes.constData(), bytes.constData() + bytes.size());
    } catch (...) {
        return ex::Json::object();
    }
}

/// 0.2: which group of the Export workspace a built-in preset belongs to.
QString presetCategory(std::string_view id) {
    static constexpr std::string_view kFormats[] = {"prores-4444-alpha", "animation-alpha", "cineform-alpha", "dnxhr-hq",
                                                    "dnxhr-444", "dnxhd-1080p-hq", "audio-mp3", "audio-aac",
                                                    "audio-aiff", "png-sequence", "tiff-sequence", "dpx-sequence",
                                                    "jpeg-sequence", "targa-sequence", "exr-sequence", "png-still",
                                                    // 0.6 (#73): broadcast delivery and Avid MediaFiles
                                                    "mxf-op1a-xdcam-hd422", "mxf-op1a-dnxhd-hq", "mxf-op1a-dnxhr-hq",
                                                    "mxf-opatom-dnxhd-hq", "mxf-op1a-avc-intra-100"};
    for (const std::string_view f : kFormats) {
        if (f == id) {
            return QStringLiteral("formats");
        }
    }
    if (id.find("hdr") != std::string_view::npos) {
        return QStringLiteral("hdr");
    }
    return QStringLiteral("general");
}

QString formatIcon(const ex::ExportSettings& s) {
    if (s.container == ex::Container::Image) {
        return QStringLiteral("image");
    }
    if (ex::isAudioOnly(s.container)) {
        return QStringLiteral("audio-lines");
    }
    return QStringLiteral("film");
}

std::mutex& presetFolderMutex() {
    static std::mutex m;
    return m;
}

std::filesystem::path& presetFolderOverride() {
    static std::filesystem::path folder;
    return folder;
}

/// 0.2: the Content Credentials thumbnail - the export's own picture in the middle of its range, rendered by the
/// exporter's renderer (redactions included; fail closed: no thumbnail rather than one without them), as SDR JPEG
/// at most 320 pixels wide. Runs on the export thread after the file is complete.
std::function<std::string(const std::filesystem::path&)> exportThumbnail(std::shared_ptr<const ProjectState> snapshot,
                                                                         SequenceId sequence, std::optional<TimeRange> range) {
    return [snapshot = std::move(snapshot), sequence, range](const std::filesystem::path&) -> std::string {
        if (!snapshot) {
            return {};
        }
        const Ticks at = range ? range->start + Ticks{range->duration.value / 2} : Ticks{};
        exporter::ReviewFrameOptions options;
        options.color = render::colorPipelineFor(snapshot->colorManagement);
        exporter::RenderedReviewFrames frames(options);
        auto frame = frames.frame(*snapshot, sequence, at, 320);
        return frame && frame->mime == "image/jpeg" ? frame->bytes : std::string();
    };
}

/// The project's colour management (I.9): an export's dynamic range (Auto, PQ, HLG) resolves against it.
ColorManagement managementOf(const ProjectController* controller) {
    const ProjectSnapshot snapshot = controller ? controller->snapshot() : ProjectSnapshot{};
    return snapshot ? snapshot->colorManagement : ColorManagement{};
}

QString pixelFormatText(ex::OutputPixelFormat f) {
    switch (f) {
    case ex::OutputPixelFormat::Yuv420p:
        return translate("8-bit 4:2:0");
    case ex::OutputPixelFormat::Yuv420p10:
        return translate("10-bit 4:2:0");
    case ex::OutputPixelFormat::Yuv422p:
        return translate("8-bit 4:2:2");
    case ex::OutputPixelFormat::Yuv422p10:
        return translate("10-bit 4:2:2");
    case ex::OutputPixelFormat::Pal8:
        return translate("256 colours");
    case ex::OutputPixelFormat::Yuv444p10:
        return translate("10-bit 4:4:4");
    case ex::OutputPixelFormat::Yuva444p10:
        return translate("10-bit 4:4:4 + alpha");
    case ex::OutputPixelFormat::Rgb24:
        return translate("8-bit RGB");
    case ex::OutputPixelFormat::Rgba:
        return translate("8-bit RGB + alpha");
    case ex::OutputPixelFormat::Rgb48:
        return translate("16-bit RGB");
    case ex::OutputPixelFormat::Rgba64:
        return translate("16-bit RGB + alpha");
    case ex::OutputPixelFormat::Gbrp10:
        return translate("10-bit RGB");
    case ex::OutputPixelFormat::Gbrp12:
        return translate("12-bit RGB");
    case ex::OutputPixelFormat::Gbrap12:
        return translate("12-bit RGB + alpha");
    case ex::OutputPixelFormat::Rgbf:
        return translate("half float RGB");
    case ex::OutputPixelFormat::Rgbaf:
        return translate("half float RGB + alpha");
    }
    return {};
}

std::filesystem::path outputFromOverride(const QVariant& value) {
    const QString text = value.toString();
    const QUrl url(text);
    const QString local = url.isLocalFile() ? url.toLocalFile() : text;
    return pathFromQString(QDir::fromNativeSeparators(local));
}

/// Characters file systems refuse, replaced in default file names.
QString safeFileName(QString name) {
    static const QString kBad = QStringLiteral("<>:\"/\\|?*");
    for (QChar& c : name) {
        if (kBad.contains(c) || c.unicode() < 32) {
            c = QLatin1Char('_');
        }
    }
    name = name.trimmed();
    while (name.endsWith(QLatin1Char('.'))) {
        name.chop(1); // Windows drops trailing dots: "a." would be "a"
    }
    // A sequence name comes from the project file: never a Windows device ("NUL" would swallow the export).
    static const QRegularExpression kDevice(QStringLiteral(R"(^(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(\..*)?$)"),
                                            QRegularExpression::CaseInsensitiveOption);
    if (kDevice.match(name).hasMatch()) {
        name.prepend(QLatin1Char('_'));
    }
    return name.isEmpty() ? translate("Export") : name;
}

} // namespace

// P1 round (2C): the helpers ExportBatches.cpp and ExportQueueKeeper.cpp share (ExportInternal.h).
QString exportdetail::safeFileName(QString name) {
    return wabcut::app::safeFileName(std::move(name));
}

QString exportdetail::presetDisplayName(std::string_view id) {
    return presetName(id);
}

// ---- ExportJobModel --------------------------------------------------------------------------------------

ExportJobModel::ExportJobModel(QObject* parent) : QAbstractListModel(parent) {}

int ExportJobModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant ExportJobModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }
    const Row& r = m_rows[static_cast<std::size_t>(index.row())];
    switch (role) {
    case JobIdRole:
        return r.jobId;
    case TitleRole:
        return r.title;
    case OutputPathRole:
        return r.outputPath;
    case PresetNameRole:
        return r.presetName;
    case StateRole:
        return r.state;
    case PhaseTextRole:
        return r.phaseText;
    case ProgressRole:
        return r.progress;
    case FpsTextRole:
        return r.fpsText;
    case EtaTextRole:
        return r.etaText;
    case EncoderRole:
        return r.encoder;
    case DetailRole:
        return r.detail;
    case FinishedRole:
        return r.finished;
    case OriginRole:
        return r.origin;
    case ErrorKindRole:
        return r.errorKind;
    case RetryableRole:
        // P1 (#20): a restored job retries from the request kept with it; an interrupted one always can.
        return (!r.presetId.isEmpty() || r.restored) &&
               (r.state == QLatin1String("failed") || r.state == QLatin1String("cancelled") ||
                r.state == QLatin1String("interrupted"));
    case RestoredRole:
        return r.restored;
    default:
        return {};
    }
}

QHash<int, QByteArray> ExportJobModel::roleNames() const {
    return {{JobIdRole, "jobId"},           {TitleRole, "title"},     {OutputPathRole, "outputPath"},
            {PresetNameRole, "presetName"}, {StateRole, "jobState"},  {PhaseTextRole, "phaseText"},
            {ProgressRole, "progress"},     {FpsTextRole, "fpsText"}, {EtaTextRole, "etaText"},
            {EncoderRole, "encoder"},       {DetailRole, "detail"},   {FinishedRole, "finished"},
            {OriginRole, "origin"},         {ErrorKindRole, "errorKind"}, {RetryableRole, "retryable"},
            {RestoredRole, "restored"}};
}

int ExportJobModel::activeCount() const {
    return static_cast<int>(
        std::count_if(m_rows.begin(), m_rows.end(), [](const Row& r) { return !r.finished; }));
}

void ExportJobModel::append(Row row) {
    if (!row.queuedAt.isValid())
        row.queuedAt = QDateTime::currentDateTimeUtc();
    const int at = rowCount();
    beginInsertRows({}, at, at);
    m_rows.push_back(std::move(row));
    endInsertRows();
    emit countChanged();
    emit activeCountChanged();
    refreshSummary();
}

const ExportJobModel::Row* ExportJobModel::update(exporter::ExportJobId queueId,
                                                  const exporter::ExportJobStatus& status) {
    const auto it =
        std::find_if(m_rows.begin(), m_rows.end(), [&](const Row& r) { return r.queueId == queueId; });
    if (it == m_rows.end()) {
        return nullptr;
    }
    Row& r = *it;
    const bool wasFinished = r.finished;
    r.held = status.held;
    const exporter::ExportProgress& p = status.progress;
    switch (p.phase) {
    case exporter::ExportPhase::Queued:
        // 0.2: sent to the queue and waiting for Start Queue (Media Encoder's "Ready").
        r.state = status.held ? QStringLiteral("held") : QStringLiteral("queued");
        r.phaseText = status.held ? translate("Ready") : translate("Queued");
        break;
    case exporter::ExportPhase::Paused:
        r.state = QStringLiteral("paused");
        r.phaseText = translate("Paused");
        break;
    case exporter::ExportPhase::Preparing:
        r.state = QStringLiteral("running");
        r.phaseText = translate("Preparing");
        break;
    case exporter::ExportPhase::Rendering:
        r.state = QStringLiteral("running");
        r.phaseText = translate("Rendering");
        break;
    case exporter::ExportPhase::Finalizing:
        r.state = QStringLiteral("running");
        r.phaseText = translate("Finishing");
        break;
    case exporter::ExportPhase::Done:
        r.state = QStringLiteral("done");
        r.phaseText = translate("Done");
        break;
    case exporter::ExportPhase::Failed:
        r.state = QStringLiteral("failed");
        r.phaseText = translate("Failed");
        break;
    case exporter::ExportPhase::Cancelled:
        r.state = QStringLiteral("cancelled");
        r.phaseText = translate("Cancelled");
        break;
    }
    r.finished = p.phase == exporter::ExportPhase::Done || p.phase == exporter::ExportPhase::Failed ||
                 p.phase == exporter::ExportPhase::Cancelled;
    r.progress = p.phase == exporter::ExportPhase::Done ? 1.0 : std::clamp(p.fraction(), 0.0, 1.0);
    r.fpsText = p.fps > 0.0 && !r.finished ? translate("%1 fps").arg(std::lround(p.fps)) : QString();
    r.etaText = p.etaSeconds && !r.finished ? translate("%1 left").arg(clockText(*p.etaSeconds)) : QString();
    if (!p.videoEncoder.empty()) {
        r.encoder = fromUtf8(p.videoEncoder);
    } else if (!p.audioEncoder.empty()) {
        r.encoder = fromUtf8(p.audioEncoder);
    }
    if (status.result) {
        const double factor = status.result->realtimeFactor;
        r.detail = translate("%1 · %2× realtime")
                       .arg(clockText(status.result->duration.toSeconds()))
                       .arg(factor, 0, 'f', factor >= 10.0 ? 0 : 1);
        // I.9 / 0.1.2: the file's size, and a size target's second pass (Discord).
        if (status.result->bytes > 0)
            r.detail += QStringLiteral(" · ") + translate("%1 MB").arg(static_cast<double>(status.result->bytes) / 1e6, 0, 'f', 1);
        if (status.result->attempts > 1)
            r.detail += QStringLiteral(" · ") + translate("encoded twice to fit the size");
        // 0.2 (I.9): an HDR file, with a PQ master's measured light levels.
        if (status.result->dynamicRange == ex::DynamicRange::Pq)
            r.detail += QStringLiteral(" · ") +
                        translate("HDR PQ · MaxCLL %1 / MaxFALL %2 nits").arg(status.result->maxCll).arg(status.result->maxFall);
        else if (status.result->dynamicRange == ex::DynamicRange::Hlg)
            r.detail += QStringLiteral(" · ") + translate("HDR HLG");
        // 0.6 (#73): the delivery - MXF's operational pattern, timecode, audio layout and UMID, the embedded captions.
        if (!status.result->operationalPattern.empty()) {
            r.detail += QStringLiteral(" · ") + translate("MXF %1 · TC %2").arg(fromUtf8(status.result->operationalPattern),
                                                                              fromUtf8(status.result->startTimecode));
            if (!status.result->audioLayout.empty())
                r.detail += QStringLiteral(" · ") + fromUtf8(status.result->audioLayout);
            if (!status.result->extraFiles.empty())
                r.detail += QStringLiteral(" · ") + (status.result->extraFiles.size() == 1
                                                         ? translate("1 more file")
                                                         : translate("%1 more files").arg(status.result->extraFiles.size()));
            // OP-Atom (more files): the picture file's UMID - FFmpeg gives every OP-Atom file a material package of its
            // own (docs/export.md), so the audio files are separate clips in Media Composer.
            if (!status.result->materialUmid.empty())
                r.detail += QStringLiteral(" · ")
                            + (status.result->extraFiles.empty() ? translate("UMID %1")
                                                                 : translate("picture UMID %1 (each file has its own)"))
                                  .arg(fromUtf8(status.result->materialUmid));
        }
        if (!status.result->embeddedCaptions.empty()) {
            const QString captions = fromUtf8(status.result->embeddedCaptions);
            const int cues = status.result->embeddedCaptionCues;
            r.detail += QStringLiteral(" · ") + (cues == 1 ? translate("%1 (1 caption)").arg(captions)
                                                           : translate("%1 (%2 captions)").arg(captions).arg(cues));
        }
        // 0.6 (#72): what SDR Conform mapped.
        if (status.result->sdrConform)
            r.detail += QStringLiteral(" · ") + translate("SDR Conform: %1 % tone-mapped, %2 % gamut-mapped")
                                                    .arg(status.result->sdrConform->toneMappedShare() * 100.0, 0, 'f', 1)
                                                    .arg(status.result->sdrConform->gamutMappedShare() * 100.0, 0, 'f', 1);
    } else if (status.error) {
        r.detail = ExportController::plainError(*status.error);
    }
    r.errorKind = status.error ? ExportController::errorKindOf(*status.error) : QString();
    const int row = static_cast<int>(it - m_rows.begin());
    emit dataChanged(index(row), index(row));
    if (wasFinished != r.finished) {
        emit activeCountChanged();
    }
    refreshSummary();
    return &r;
}

const ExportJobModel::Row* ExportJobModel::find(const QString& jobId) const {
    const auto it =
        std::find_if(m_rows.begin(), m_rows.end(), [&](const Row& r) { return r.jobId == jobId; });
    return it == m_rows.end() ? nullptr : &*it;
}

void ExportJobModel::setRequest(const QString& jobId, const QString& sequenceId, const QString& presetId,
                                const QVariantMap& overrides) {
    const auto it = std::find_if(m_rows.begin(), m_rows.end(), [&](const Row& r) { return r.jobId == jobId; });
    if (it == m_rows.end()) {
        return;
    }
    it->sequenceId = sequenceId;
    it->presetId = presetId;
    it->overrides = overrides;
    const int row = static_cast<int>(it - m_rows.begin());
    emit dataChanged(index(row), index(row), {RetryableRole});
}

void ExportJobModel::removeFinished() {
    for (int i = rowCount() - 1; i >= 0; --i) {
        if (m_rows[static_cast<std::size_t>(i)].finished) {
            beginRemoveRows({}, i, i);
            m_rows.erase(m_rows.begin() + i);
            endRemoveRows();
        }
    }
    emit countChanged();
    refreshSummary();
}

void ExportJobModel::removeFinished(exporter::ExportJobId queueId) {
    if (queueId == 0) {
        return; // P1 (#20): an unknown job (queueIdOf) - not a restored row outside the queue
    }
    for (int i = rowCount() - 1; i >= 0; --i) {
        const Row& r = m_rows[static_cast<std::size_t>(i)];
        if (r.queueId == queueId && r.finished) {
            beginRemoveRows({}, i, i);
            m_rows.erase(m_rows.begin() + i);
            endRemoveRows();
            emit countChanged();
            refreshSummary();
            return;
        }
    }
}

// 0.4: the title bar's Background tasks pill, the quit guard, keep-awake and the taskbar read this summary. Held rows
// wait for Start Queue; queued rows run on their own, so they count as running - but only while no row is paused: the
// queue has one worker, and a paused export holds it (PauseGate::wait), so the rows queued behind it wait on the user
// too (they are then in activeCount alone). The current job is the running one, else the first paused one, else the
// first queued one (held rows never are).
void ExportJobModel::refreshSummary() {
    Summary next;
    const Row* running = nullptr;
    const Row* paused = nullptr;
    const Row* queued = nullptr;
    int queuedRows = 0;
    for (const Row& r : m_rows) {
        if (r.finished) {
            continue;
        }
        if (r.state == QLatin1String("running")) {
            ++next.running;
            if (!running) {
                running = &r;
            }
        } else if (r.state == QLatin1String("paused")) {
            ++next.paused;
            if (!paused) {
                paused = &r;
            }
        } else if (r.state == QLatin1String("held")) {
            ++next.held;
        } else { // "queued": starts on its own once the worker is free
            ++queuedRows;
            if (!queued) {
                queued = &r;
            }
        }
    }
    if (next.paused == 0) {
        next.running += queuedRows;
    }
    const Row* current = running ? running : paused ? paused : queued;
    if (current) {
        next.jobId = current->jobId;
        next.title = current->title;
        next.state = current->state;
        next.progress = current->progress;
        next.eta = current->etaText;
    }
    if (next == m_summary) {
        return;
    }
    m_summary = std::move(next);
    emit summaryChanged();
}

// ---- ExportController ------------------------------------------------------------------------------------

/// The queue's listener runs on the export worker; it posts to the controller through this relay, which the
/// destructor cuts before the queue (and its worker) goes away.
struct ExportController::Relay {
    std::mutex mutex;
    ExportController* target = nullptr;

    /// Runs fn(controller) on the controller's thread, unless the controller is gone.
    template <class Fn> void post(Fn fn) {
        std::lock_guard lock(mutex);
        if (target) {
            QMetaObject::invokeMethod(
                target, [t = target, fn = std::move(fn)] { fn(t); }, Qt::QueuedConnection);
        }
    }
};

ExportController::ExportController(QObject* parent)
    : QObject(parent), m_jobs(new ExportJobModel(this)), m_relay(std::make_shared<Relay>()) {
    m_relay->target = this;
    m_queue = std::make_unique<ex::ExportQueue>([relay = m_relay](const ex::ExportJobStatus& status) {
        relay->post([status](ExportController* target) { target->onStatus(status); });
    });
    m_autoEncoders = {{QStringLiteral("h264"), QString()},
                      {QStringLiteral("hevc"), QString()},
                      {QStringLiteral("prores"), QString()},
                      {QStringLiteral("dnxhr"), QString()}};
    // 0.2: Media Encoder's Watch Folders, queued on this controller's queue (their jobs join the list as they come).
    ex::WatchFolderService::Options watchOptions;
    m_watch = std::make_shared<ex::WatchFolderService>(*m_queue, watchOptions);
    connect(m_jobs, &ExportJobModel::activeCountChanged, this, &ExportController::busyChanged);
    // P1 (#29): while exports run, their drives are checked every 30 s (diskSpaceLow under 2 GB, once per drive).
    connect(m_jobs, &ExportJobModel::activeCountChanged, this, [this] {
        if (m_jobs->activeCount() > 0 && !m_spaceTimer) {
            auto* timer = new QTimer(this);
            timer->setInterval(30'000);
            connect(timer, &QTimer::timeout, this, &ExportController::checkDiskSpace);
            timer->start();
            m_spaceTimer = timer;
        } else if (m_jobs->activeCount() == 0 && m_spaceTimer) {
            m_spaceTimer->deleteLater();
            m_spaceTimer = nullptr;
            m_spaceWarned.clear();
        }
    });
    // UX rework (L): a deleted user preset changes what lastPreset / lastQuickPreset read.
    connect(this, &ExportController::presetsChanged, this, &ExportController::exportMemoryChanged);
    // AI-15: the Content Credentials setting ("ai" until the user picks another).
    QSettings settings;
    const QString credentials = settings.value(QStringLiteral("export/contentCredentials"), QStringLiteral("ai")).toString();
    if (ai::c2pa::credentialsModeFromString(credentials.toStdString())) {
        m_contentCredentials = credentials;
    }
    // 0.2: what the credentials say besides the export itself, and who signs them.
    m_credentialsProducer = settings.value(QStringLiteral("export/c2paProducer")).toString();
    m_credentialsThumbnail = settings.value(QStringLiteral("export/c2paThumbnail"), true).toBool();
    m_credentialsDoNotTrain = settings.value(QStringLiteral("export/c2paDoNotTrain"), false).toBool();
    m_credentialsCertificate = settings.value(QStringLiteral("export/c2paCertificate")).toString();
    m_credentialsKey = settings.value(QStringLiteral("export/c2paKey")).toString();
    // 0.2: the CPU limit preference and the watch folders.
    m_cpuLimit = std::clamp(settings.value(QStringLiteral("export/cpuLimit"), 100).toInt(), 10, 100);
    ex::setExportThreadLimit(ex::threadLimitForPercent(m_cpuLimit));
    const QByteArray watched = settings.value(QStringLiteral("export/watchFolders")).toByteArray();
    if (!watched.isEmpty()) {
        try {
            const ex::Json list = ex::Json::parse(watched.constData(), watched.constData() + watched.size());
            for (const ex::Json& w : list.is_array() ? list : ex::Json::array()) {
                const std::string presetId = w.value("preset", "");
                const auto preset = userPresetStore()->findAny(presetId);
                if (!preset) {
                    continue;
                }
                ex::WatchFolder folder;
                folder.folder = pathFromUtf8(w.value("folder", ""));
                folder.output = pathFromUtf8(w.value("output", ""));
                folder.presetId = presetId;
                folder.settings = preset->settings;
                folder.moveSources = w.value("moveSources", true);
                if (!folder.folder.empty()) {
                    (void)m_watch->add(std::move(folder));
                }
            }
        } catch (...) {
            // A broken setting watches nothing.
        }
    }
}

ExportController::~ExportController() {
    if (m_persistPending) {
        persistQueue(); // P1 (#20): a change not written yet (the queue below is cancelled, not "finished")
    }
    {
        std::lock_guard lock(m_relay->mutex);
        m_relay->target = nullptr;
    }
    m_commandServices.reset(); // 0.2: the watch folders queue on m_queue: they stop first
    m_watch.reset();
    m_queue.reset(); // cancels what is queued or running, then joins the worker
    if (m_probe.joinable()) {
        m_probe.join();
    }
}

ExportController* ExportController::create(QQmlEngine*, QJSEngine*) {
    // The ProjectController singleton is looked up on first use (project()): during this factory the engine
    // cannot hand out another singleton yet.
    return new ExportController;
}

void ExportController::setProject(ProjectController* project) {
    // P1 (#21): project() may have found the singleton already (lazily): the sequence watch is made here either way.
    if (m_project != project || (project && !m_sequenceConnection)) {
        const bool changed = m_project != project;
        m_project = project;
        // P1 (#21): another sequence of the other shape changes what the remembered presets read.
        disconnect(m_sequenceConnection);
        m_lastPortrait = portraitSequence({});
        if (project) {
            m_sequenceConnection = connect(project, &ProjectController::sequenceChanged, this, [this] {
                const bool portrait = portraitSequence({});
                if (portrait != m_lastPortrait) {
                    m_lastPortrait = portrait;
                    emit exportMemoryChanged();
                }
            });
        }
        if (changed) {
            emit projectChanged();
        }
    }
}

ProjectController* ExportController::project() const {
    if (!m_project) {
        if (QQmlEngine* engine = qmlEngine(this)) {
            m_project = engine->singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        }
    }
    return m_project;
}

QString ExportController::fileManagerName() const {
#if defined(Q_OS_WIN)
    return translate("Explorer");
#elif defined(Q_OS_MACOS)
    return translate("Finder");
#else
    return translate("Files");
#endif
}

QVariantList ExportController::presets() const {
    QVariantList list;
    for (const ex::Preset& p : ex::builtInPresets()) {
        const PresetText* text = presetText(p.id);
        list.append(QVariantMap{
            {QStringLiteral("id"), fromUtf8(p.id)},
            {QStringLiteral("name"), presetName(p.id)},
            {QStringLiteral("description"), text ? translate(text->description) : fromUtf8(p.description)},
            {QStringLiteral("icon"), text ? QString::fromLatin1(text->icon) : formatIcon(p.settings)},
            {QStringLiteral("category"), presetCategory(p.id)},
            {QStringLiteral("user"), false}});
    }
    // 0.2: the user's presets (Save Preset, Import Preset).
    for (const ex::Preset& p : userPresetStore()->list()) {
        list.append(QVariantMap{{QStringLiteral("id"), fromUtf8(p.id)},
                                {QStringLiteral("name"), fromUtf8(p.name)},
                                {QStringLiteral("description"), fromUtf8(p.description)},
                                {QStringLiteral("icon"), formatIcon(p.settings)},
                                {QStringLiteral("category"), QStringLiteral("user")},
                                {QStringLiteral("user"), true}});
    }
    return list;
}

std::shared_ptr<ex::PresetStore> ExportController::userPresetStore() {
    std::lock_guard lock(presetFolderMutex());
    static std::shared_ptr<ex::PresetStore> store;
    std::filesystem::path folder = presetFolderOverride();
    if (folder.empty()) {
        const QString config = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        folder = config.isEmpty() ? std::filesystem::path() : pathFromQString(config) / "Export Presets";
    }
    if (!store || store->folder() != folder) {
        store = std::make_shared<ex::PresetStore>(folder);
    }
    return store;
}

void ExportController::setUserPresetFolder(const std::filesystem::path& folder) {
    std::lock_guard lock(presetFolderMutex());
    presetFolderOverride() = folder;
}

std::expected<ex::ExportSettings, Error> ExportController::settingsFor(const QString& presetId,
                                                                       const QVariantMap& overrides) {
    // 0.2: built-in or user presets; the override keys are the export library's (ExportPresets.h applyOverrides).
    const auto preset = userPresetStore()->findAny(presetId.toStdString());
    if (!preset) {
        return std::unexpected(Error{ErrorCode::InvalidArgument,
                                     QStringLiteral("Unknown preset %1").arg(presetId).toStdString()});
    }
    ex::ExportSettings s = preset->settings;
    s.presetId = preset->id;
    if (auto ok = ex::applyOverrides(s, jsonFromVariantMap(overrides)); !ok) {
        return std::unexpected(ok.error());
    }
    if (auto ok = ex::validate(s); !ok) {
        return std::unexpected(ok.error());
    }
    return s;
}

SequencePtr ExportController::sequenceFor(const QString& sequenceId) const {
    ProjectController* projectController = project();
    if (!projectController) {
        return nullptr;
    }
    const ProjectSnapshot snapshot = projectController->snapshot();
    if (!snapshot) {
        return nullptr;
    }
    if (sequenceId.isEmpty()) {
        return snapshot->sequencePtr(projectController->currentSequenceId());
    }
    const auto id = parseSequenceKey(sequenceId);
    return id ? snapshot->sequencePtr(*id) : nullptr;
}

QString ExportController::defaultOutputPath(const QString& sequenceId, const QString& presetId,
                                            const QVariantMap& overrides) const {
    const auto settings = settingsFor(presetId, overrides);
    const SequencePtr sequence = sequenceFor(sequenceId);
    // UX rework (L): the folder the user last exported to, then next to the project.
    QString folder = lastFolder();
    if (folder.isEmpty() && !m_projectFile.empty()) {
        folder = pathToQString(m_projectFile.parent_path());
    }
    if (folder.isEmpty()) {
        folder = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    }
    if (folder.isEmpty()) {
        folder = QDir::homePath();
    }
    const QString base = safeFileName(sequence ? QString::fromStdString(sequence->name) : QString());
    const QString ext = fromUtf8(settings ? ex::outputExtension(*settings) : ex::fileExtension(ex::Container::Mp4));
    auto taken = [&](const QString& path) {
        if (QFileInfo::exists(path)) {
            return true;
        }
        for (const ExportJobModel::Row& r : m_jobs->rows()) {
            if (!r.finished && QDir::cleanPath(r.outputPath) == QDir::cleanPath(path)) {
                return true;
            }
        }
        return false;
    };
    QString path = QDir(folder).filePath(base + ext);
    for (int n = 2; taken(path) && n < 1000; ++n) {
        path = QDir(folder).filePath(QStringLiteral("%1 (%2)%3").arg(base).arg(n).arg(ext));
    }
    return QDir::toNativeSeparators(path);
}

QVariantMap ExportController::describe(const QString& sequenceId, const QString& presetId,
                                       const QVariantMap& rawOverrides) const {
    QVariantMap d;
    const SequencePtr sequence = sequenceFor(sequenceId);
    d[QStringLiteral("rangeAvailable")] =
        QVariantMap{{QStringLiteral("whole"), sequence != nullptr},
                    {QStringLiteral("inOut"), sequence && sequence->markedRange().has_value()},
                    {QStringLiteral("workArea"), sequence && sequence->workArea.has_value()},
                    {QStringLiteral("custom"), sequence != nullptr}};
    auto fail = [&](const Error& e) {
        d[QStringLiteral("ok")] = false;
        d[QStringLiteral("error")] = plainError(e);
        return d;
    };
    const auto prepared = preparedOverrides(rawOverrides, sequence.get());
    if (!prepared) {
        return fail(prepared.error());
    }
    const QVariantMap& overrides = *prepared;
    const auto settings = settingsFor(presetId, overrides);
    if (!settings) {
        return fail(settings.error());
    }
    if (!sequence) {
        return fail(Error{ErrorCode::NotFound, "There is no sequence to export"});
    }
    const auto r = ex::resolve(*settings, *sequence, managementOf(project()));
    if (!r) {
        return fail(r.error());
    }
    const ex::ExportSettings& s = *settings;
    d[QStringLiteral("ok")] = true;
    d[QStringLiteral("container")] = fromUtf8(ex::containerName(s.container));
    d[QStringLiteral("videoCodec")] = fromUtf8(ex::videoCodecName(s.video.codec));
    if (s.video.enabled) {
        // 0.6 (#73): MXF names its operational pattern.
        const QString containerText = s.container == ex::Container::Mxf         ? QStringLiteral("MXF OP1a")
                                      : s.container == ex::Container::MxfOpAtom ? QStringLiteral("MXF OP-Atom")
                                                                                : fromUtf8(ex::containerName(s.container)).toUpper();
        d[QStringLiteral("format")] =
            QStringLiteral("%1 · %2 · %3").arg(containerText, codecText(s.video.codec), pixelFormatText(s.video.pixelFormat));
        QString resolution = QStringLiteral("%1 × %2").arg(r->width).arg(r->height);
        if (r->renderWidth > r->width || r->renderHeight > r->height) {
            resolution += translate(" · centre crop");
        } else if (r->renderWidth < r->width || r->renderHeight < r->height) {
            resolution += translate(" · letterboxed");
        }
        d[QStringLiteral("resolution")] = resolution;
        d[QStringLiteral("frameRate")] = rateText(r->frameRate);
        d[QStringLiteral("frames")] = QString::number(r->frameCount);
        const QString codecKey = fromUtf8(ex::videoCodecName(s.video.codec));
        const QString picked = m_autoEncoders.value(codecKey).toString();
        d[QStringLiteral("encoder")] =
            s.video.encoder == "auto"
                ? (picked.isEmpty() ? translate("Auto · hardware first") : translate("Auto · %1").arg(picked))
                : QString::fromStdString(s.video.encoder);
        const bool sized = s.video.rateControl == ex::RateControl::TargetSize;
        d[QStringLiteral("quality")] =
            s.video.codec == ex::VideoCodec::ProRes || s.video.codec == ex::VideoCodec::DnxHR
                ? translate("Profile")
            : s.video.rateControl == ex::RateControl::Quality
                ? translate("Constant quality %1").arg(s.video.quality)
            : sized
                // P1 (#28): a size limit: what fits, and the bitrate that leaves the picture.
                ? translate("Fits %1 MB (≈ %2 Mb/s)")
                      .arg(QString::number(static_cast<double>(s.video.targetBytes) / 1e6, 'g', 4))
                      .arg(static_cast<double>(r->targetVideoBitrate) / 1e6, 0, 'f', 1)
                : translate("%1 Mb/s").arg(static_cast<double>(s.video.bitrate) / 1e6, 0, 'f', 1);
        d[QStringLiteral("targetMB")] = sized ? static_cast<double>(s.video.targetBytes) / 1e6 : 0.0;
        // P1 (#28): too few bits for the picture (under 0.03 bits a pixel at the output size and rate) blocks up.
        const double rate = sized ? static_cast<double>(r->targetVideoBitrate)
                            : s.video.rateControl == ex::RateControl::Bitrate ? static_cast<double>(s.video.bitrate)
                                                                              : 0.0;
        d[QStringLiteral("targetMbps")] = sized ? static_cast<double>(r->targetVideoBitrate) / 1e6 : 0.0;
        const double pixelRate = static_cast<double>(r->width) * r->height * r->frameRate.toDouble();
        const bool lowBitrate = rate > 0 && pixelRate > 0 && rate / pixelRate < 0.03;
        d[QStringLiteral("lowBitrate")] = lowBitrate;
        d[QStringLiteral("lowBitrateNote")] =
            lowBitrate ? translate("%1 Mb/s is little for %2 × %3 at %4: the picture will block up. A smaller frame size, a "
                                   "lower frame rate or a shorter range looks better.")
                             .arg(rate / 1e6, 0, 'f', 1)
                             .arg(r->width)
                             .arg(r->height)
                             .arg(rateText(r->frameRate))
                       : QString();
        // 0.2 (I.9): what the picture is written as, and whether this format can carry HDR from this sequence.
        const ColorManagement management = managementOf(project());
        d[QStringLiteral("dynamicRangeSetting")] = fromUtf8(ex::dynamicRangeName(s.video.dynamicRange));
        d[QStringLiteral("hdr")] = r->dynamicRange != ex::DynamicRange::Sdr;
        d[QStringLiteral("hdrAvailable")] =
            management.managed() && (s.video.codec == ex::VideoCodec::Hevc || s.video.codec == ex::VideoCodec::Av1 ||
                                     s.video.codec == ex::VideoCodec::ProRes);
        d[QStringLiteral("dynamicRange")] =
            r->dynamicRange == ex::DynamicRange::Pq    ? translate("HDR · Rec.2100 PQ · HDR10 metadata, %1 nits").arg(management.peakNits)
            : r->dynamicRange == ex::DynamicRange::Hlg ? translate("HDR · Rec.2100 HLG")
            : management.isHdr()                       ? translate("SDR · tone-mapped from HDR")
            : r->outputSpace != OutputColorSpace::Rec709 ? translate("SDR") // 0.6 (#72): the chain below names the space
                                                         : translate("SDR · Rec.709");
        // 0.6 (#72): the colour chain (the output colour space, SDR Conform, tone mapping or clipping) when it is more
        // than Rec.709 as it is.
        if (management.managed() || r->outputSpace != OutputColorSpace::Rec709) {
            ColorManagement effective = management;
            if (r->sdrConform) {
                effective.sdrConform = *r->sdrConform;
            }
            const color::OutputTarget target =
                r->dynamicRange == ex::DynamicRange::Pq    ? color::OutputTarget::Pq
                : r->dynamicRange == ex::DynamicRange::Hlg ? color::OutputTarget::Hlg
                                                           : color::outputTargetOf(r->outputSpace, color::OutputTarget::Sdr);
            d[QStringLiteral("colorChain")] = fromUtf8(color::describeOutput(effective, target, r->sdrConform.has_value()));
            d[QStringLiteral("dynamicRange")] = d.value(QStringLiteral("dynamicRange")).toString() + QStringLiteral(" · ") +
                                                d.value(QStringLiteral("colorChain")).toString();
        }
    } else {
        d[QStringLiteral("format")] = fromUtf8(ex::containerName(s.container)).toUpper();
        d[QStringLiteral("resolution")] = translate("No video");
    }
    // A.9: the channels the file gets (as the master resolves to 5.1 for a 5.1 master when the codec takes it).
    const int channels = ex::exportAudioChannels(s.audio, sequence->masterFormat);
    const QString channelText = channels == 1   ? translate("mono")
                                : channels == 2 ? translate("stereo")
                                : channels == 6 ? translate("5.1")
                                                : translate("%1 channels").arg(channels);
    d[QStringLiteral("audioChannels")] = s.audio.channels;
    d[QStringLiteral("audioCodec")] = fromUtf8(ex::audioCodecName(s.audio.codec));
    d[QStringLiteral("audio")] =
        !s.audio.enabled ? translate("No audio")
        : s.audio.codec == ex::AudioCodec::Aac
            ? translate("AAC · %1 kHz · %2 kb/s · %3").arg(s.audio.sampleRate / 1000.0).arg(s.audio.bitrate / 1000).arg(channelText)
        : s.audio.codec == ex::AudioCodec::Ac3
            ? translate("AC-3 · %1 kHz · %2 kb/s · %3").arg(s.audio.sampleRate / 1000.0).arg(s.audio.bitrate / 1000).arg(channelText)
        : s.audio.codec == ex::AudioCodec::Mp3
            ? translate("MP3 · %1 kHz · %2 kb/s · %3").arg(s.audio.sampleRate / 1000.0).arg(s.audio.bitrate / 1000).arg(channelText)
            : translate("PCM %1-bit · %2 kHz · %3")
                  .arg(s.audio.codec == ex::AudioCodec::Pcm16 ? 16 : 24)
                  .arg(s.audio.sampleRate / 1000.0)
                  .arg(channelText);
    d[QStringLiteral("duration")] = clockText(r->duration.toSeconds());
    // Premiere's Range names (0.2).
    d[QStringLiteral("range")] = s.range == ex::RangeMode::InOut      ? translate("Source In/Out")
                                 : s.range == ex::RangeMode::WorkArea ? translate("Work Area")
                                 : s.range == ex::RangeMode::Custom   ? translate("Custom")
                                                                      : translate("Entire Source");
    d[QStringLiteral("rangeSetting")] = fromUtf8(ex::rangeModeName(s.range));
    d[QStringLiteral("rangeIn")] = fromUtf8(ticksToTimecode(r->range.start, sequence->frameRate, sequence->dropFrame));
    d[QStringLiteral("rangeOut")] = fromUtf8(ticksToTimecode(r->range.end(), sequence->frameRate, sequence->dropFrame));
    // 0.2: what the format can do (the drawer shows the rows that apply).
    const ex::VideoCodec vc = s.video.codec;
    d[QStringLiteral("extension")] = fromUtf8(ex::outputExtension(s));
    d[QStringLiteral("audioOnly")] = ex::isAudioOnly(s.container);
    d[QStringLiteral("isImage")] = s.container == ex::Container::Image;
    d[QStringLiteral("exportAsSequence")] = s.video.exportAsSequence;
    d[QStringLiteral("alpha")] = s.video.enabled && ex::hasAlpha(s.video.pixelFormat);
    d[QStringLiteral("alphaAvailable")] =
        s.video.enabled && (vc == ex::VideoCodec::ProRes || vc == ex::VideoCodec::Png || vc == ex::VideoCodec::Tiff ||
                            vc == ex::VideoCodec::Targa || vc == ex::VideoCodec::Animation || vc == ex::VideoCodec::Exr ||
                            vc == ex::VideoCodec::CineForm);
    d[QStringLiteral("subtitlesAvailable")] =
        s.video.enabled && (s.container == ex::Container::Mp4 || s.container == ex::Container::Mov || s.container == ex::Container::Mkv);
    d[QStringLiteral("subtitles")] = s.subtitles.enabled;
    d[QStringLiteral("smartRenderAvailable")] = vc == ex::VideoCodec::ProRes || vc == ex::VideoCodec::DnxHR;
    // 0.6 (#73): MXF delivery - the operational pattern, the reel, how the audio is laid out - and embedded captions:
    // what the format can carry and where this export puts them.
    const bool mxf = ex::isMxf(s.container);
    d[QStringLiteral("mxf")] = mxf;
    d[QStringLiteral("operationalPattern")] = s.container == ex::Container::Mxf         ? QStringLiteral("OP1a")
                                              : s.container == ex::Container::MxfOpAtom ? QStringLiteral("OP-Atom")
                                                                                        : QString();
    d[QStringLiteral("reelName")] = fromUtf8(s.mxf.reelName);
    d[QStringLiteral("mxfAudioLayout")] = fromUtf8(ex::mxfAudioLayoutName(s.mxf.audioLayout));
    if (mxf && s.audio.enabled) {
        const int mixChannels = channels <= 1 ? 1 : channels >= 6 && sequence->masterFormat == ChannelFormat::Surround51 ? 6 : 2;
        QString layout = s.container == ex::Container::MxfOpAtom
                             ? (channels == 1 ? translate("1 mono audio file") : translate("%1 mono audio files").arg(channels))
                         : s.mxf.audioLayout == ex::MxfAudioLayout::OneTrack ? translate("one %1-channel track").arg(channels)
                         : channels == 1 ? translate("1 mono track")
                                         : translate("%1 mono tracks").arg(channels);
        if (channels > mixChannels) {
            layout += translate(" · %1 the %2 mix, %3 silent")
                          .arg(mixChannels == 1 ? QStringLiteral("1") : QStringLiteral("1-%1").arg(mixChannels),
                               mixChannels == 6 ? translate("5.1") : translate("stereo"),
                               mixChannels + 1 == channels ? QString::number(channels)
                                                           : QStringLiteral("%1-%2").arg(mixChannels + 1).arg(channels));
        }
        d[QStringLiteral("audio")] = translate("PCM %1-bit · %2 kHz · %3")
                                         .arg(s.audio.codec == ex::AudioCodec::Pcm16 ? 16 : 24)
                                         .arg(s.audio.sampleRate / 1000.0)
                                         .arg(layout);
    }
    d[QStringLiteral("captionMode")] = fromUtf8(ex::captionExportName(s.captions.mode));
    d[QStringLiteral("captionStandard")] = fromUtf8(ex::captionStandardName(s.captions.standard));
    d[QStringLiteral("cc608Channel")] = static_cast<int>(s.captions.channel608);
    d[QStringLiteral("cc708Service")] = static_cast<int>(s.captions.service708);
    d[QStringLiteral("captionCarriage")] = fromUtf8(ex::captionCarriageName(s.captions.carriage));
    d[QStringLiteral("captionDisplay")] = static_cast<int>(s.captions.display608);
    d[QStringLiteral("captionTrackAvailable")] = s.container == ex::Container::Mxf || s.container == ex::Container::Mov;
    d[QStringLiteral("captionInVideoAvailable")] =
        s.video.enabled && (vc == ex::VideoCodec::H264 || vc == ex::VideoCodec::Hevc || vc == ex::VideoCodec::Xdcam);
    if (s.captions.mode == ex::CaptionExport::Embed) {
        if (const auto plan = ex::embeddedCaptionPlan(s)) {
            d[QStringLiteral("embeddedCaptions")] = fromUtf8(ex::describeEmbeddedCaptions(s, *plan));
        }
    }
    d[QStringLiteral("chapterMarkers")] = static_cast<int>(std::count_if(
        sequence->markers.begin(), sequence->markers.end(), [&](const Marker& m) {
            return m.type == MarkerType::Chapter && m.start >= r->range.start && m.start < r->range.end();
        }));
    // P1 (#27): the original media this export would read that is offline or missing (the export would stop on it).
    QStringList offline;
    if (const ProjectSnapshot snapshot = project() ? project()->snapshot() : ProjectSnapshot{}) {
        for (const ex::MissingOriginal& m : ex::missingOriginals(*snapshot, *sequence, s, m_projectFile)) {
            offline << QString::fromStdString(m.name.empty() ? m.path : m.name);
        }
    }
    d[QStringLiteral("offline")] = offline;
    d[QStringLiteral("offlineCount")] = static_cast<int>(offline.size());
    QString output = overrides.contains(QStringLiteral("output"))
                         ? pathToQString(outputFromOverride(overrides.value(QStringLiteral("output"))))
                         : defaultOutputPath(sequenceId, presetId, overrides);
    if (!output.isEmpty()) { // the format decides the extension
        std::filesystem::path path = pathFromQString(output);
        path.replace_extension(std::string(ex::outputExtension(s)));
        output = pathToQString(path);
    }
    d[QStringLiteral("output")] = output;
    // 0.6 (#52): the stems a "By track role" export writes next to this one, and the line the settings drawer shows.
    QVariantList stems;
    QStringList stemNames;
    for (const ex::StemRole& role : ex::stemRoles(*sequence)) {
        const QString path = output.isEmpty() ? QString() : pathToQString(ex::stemOutput(pathFromQString(output), role.role));
        const QString file = QFileInfo(path).fileName();
        const QString name = stemRoleName(role.role);
        stems.append(QVariantMap{{QStringLiteral("key"), fromUtf8(role.key)},
                                 {QStringLiteral("name"), name},
                                 {QStringLiteral("suffix"), fromUtf8(role.suffix)},
                                 {QStringLiteral("tracks"), role.tracks},
                                 {QStringLiteral("path"), path},
                                 {QStringLiteral("file"), file}});
        stemNames << (file.isEmpty() ? name : translate("%1 (%2)").arg(name, file));
    }
    d[QStringLiteral("stems")] = stems;
    d[QStringLiteral("stemsOn")] = overrides.value(QStringLiteral("stems")).toString() == QLatin1String("role");
    d[QStringLiteral("stemsText")] =
        sequence->multicam ? translate("A multicam source sequence has no stems: export the sequence it is cut into.")
        : stems.isEmpty()  ? translate("No audio track plays in this export, so there is nothing to split into stems.")
                           : (stems.size() == 1
                              ? translate("Also writes 1 stem, 24-bit WAV, next to the export: %1. Same range and sample "
                                          "rate as the export, no loudness normalisation.")
                                    .arg(stemNames.join(QStringLiteral(", ")))
                              : translate("Also writes %1 stems, 24-bit WAV, next to the export: %2. Same range and sample "
                                          "rate as the export, no loudness normalisation.")
                                    .arg(stems.size())
                                    .arg(stemNames.join(QStringLiteral(", "))));
    return d;
}

QString ExportController::queueExport(const QString& sequenceId, const QString& presetId,
                                      const QVariantMap& rawOverrides) {
    return queueExportAs(sequenceId, presetId, rawOverrides, QStringLiteral("export"));
}

QString ExportController::queueExportAs(const QString& sequenceId, const QString& presetId,
                                        const QVariantMap& rawOverrides, const QString& origin) {
    if (!project()) {
        setLastError(translate("There is no project to export."));
        return {};
    }
    const SequencePtr sequence = sequenceFor(sequenceId);
    if (!sequence) {
        setLastError(translate("There is no sequence to export."));
        return {};
    }
    const auto prepared = preparedOverrides(rawOverrides, sequence.get());
    if (!prepared) {
        setLastError(plainError(prepared.error()));
        return {};
    }
    const QVariantMap& overrides = *prepared;
    auto settings = settingsFor(presetId, overrides);
    if (!settings) {
        setLastError(plainError(settings.error()));
        return {};
    }
    // 0.6 (#52): a stem of an export ("stemOnly", Stems.h): audio only, of the project with that role's tracks soloed.
    const QString stemKey = overrides.value(QStringLiteral("stemOnly")).toString();
    std::optional<TrackRole> stemRole;
    if (!stemKey.isEmpty()) {
        stemRole = ex::stemRoleFromKey(stemKey.toStdString());
        if (!stemRole) {
            setLastError(translate("Unknown stem \"%1\".").arg(stemKey));
            return {};
        }
        *settings = ex::stemSettings(*settings);
    }
    if (settings->captions.mode != ex::CaptionExport::None) {
        // The caption track the program monitor shows (Captions panel; PlaybackBridge draws the same), in that track's
        // own style (no override, I.9).
        if (QQmlEngine* engine = qmlEngine(this)) {
            if (auto* captions = engine->singletonInstance<CaptionsBridge*>("Wabcut", "Captions")) {
                if (const auto id = parseCaptionTrackKey(captions->track())) {
                    settings->captions.track = id->value;
                }
            }
        }
        settings->captions.karaoke = captionKaraoke();
        // Burned-in karaoke highlights the words the program monitor highlights: the same transcript word timings,
        // snapshotted into the request (Codex wave-3 P2-8). The transcript is the current sequence's.
        const SequencePtr current = project()->currentSequence();
        if (settings->captions.mode == ex::CaptionExport::BurnIn && settings->captions.karaoke && current &&
            current->id == sequence->id) {
            if (QQmlEngine* engine = qmlEngine(this)) {
                if (auto* transcript = engine->singletonInstance<TranscriptBridge*>("Wabcut", "Transcript")) {
                    playback::CaptionSettings which;
                    if (settings->captions.track) {
                        which.track = CaptionTrackId{*settings->captions.track};
                    }
                    if (const CaptionTrack* track = playback::captionTrackFor(*sequence, which)) {
                        settings->captions.words = captionWordTimings(*track, transcript->sequenceTranscript());
                    }
                }
            }
        }
    }
    if (const auto r = ex::resolve(*settings, *sequence, managementOf(project())); !r) {
        setLastError(plainError(r.error()));
        return {};
    }
    std::filesystem::path output = overrides.contains(QStringLiteral("output"))
                                       ? outputFromOverride(overrides.value(QStringLiteral("output")))
                                       : pathFromQString(defaultOutputPath(sequenceId, presetId, overrides));
    if (output.empty()) {
        setLastError(translate("Choose where to save the export."));
        return {};
    }
    if (stemRole && !overrides.contains(QStringLiteral("output"))) {
        output = ex::stemOutput(output, *stemRole); // next to the master's default file
    }
    output.replace_extension(std::string(ex::outputExtension(*settings))); // the format decides (0.2: stills too)

    ex::ExportRequest request;
    request.snapshot = project()->snapshot();
    if (stemRole && request.snapshot) {
        const bool masterEffects = overrides.value(QStringLiteral("stemsMasterEffects"), true).toBool();
        request.snapshot = std::make_shared<const ProjectState>(
            ex::stemProject(*request.snapshot, sequence->id, *stemRole, masterEffects));
    }
    request.sequence = sequence->id;
    request.settings = *settings;
    request.output = output;
    request.projectFile = m_projectFile;
    request.videoSource = m_videoSource;
    setLastError({});
    const auto user = settings->presetId.starts_with("user-") ? userPresetStore()->find(settings->presetId) : std::nullopt;
    const bool chosenOutput = overrides.contains(QStringLiteral("output")) && !stemRole;
    const QString label = stemRole ? translate("%1 stem").arg(stemRoleName(*stemRole))
                          : user   ? fromUtf8(user->name)
                                   : presetName(settings->presetId);
    // Stems carry no Content Credentials (WAV); the master export does.
    const QString job = queueRequestAs(newJobId(), std::move(request), label, overrides.value(QStringLiteral("hold")).toBool(),
                                       stemRole ? QStringLiteral("off")
                                                : overrides.value(QStringLiteral("contentCredentials")).toString(),
                                       origin);
    // P1 (#27): the request stays with the job, so a failed one can be retried as it was (its own sequence, not the
    // current one then).
    if (!job.isEmpty()) {
        m_jobs->setRequest(job, sequenceKey(sequence->id), presetId, rawOverrides);
    }
    // UX rework (L): a destination the user chose is where the next export goes by default.
    if (!job.isEmpty() && chosenOutput) {
        setLastFolder(pathToQString(output.parent_path()));
    }
    // 0.6 (#52): "By track role": the stems right after the export, named after its file ("<name>_DX.wav", ...), each a
    // job of its own (retried on its own).
    if (!job.isEmpty() && !stemRole && overrides.value(QStringLiteral("stems")).toString() == QLatin1String("role")) {
        for (const ex::StemRole& role : ex::stemRoles(*sequence)) {
            QVariantMap stem = rawOverrides;
            stem.remove(QStringLiteral("stems"));
            stem[QStringLiteral("stemOnly")] = fromUtf8(role.key);
            stem[QStringLiteral("output")] = pathToQString(ex::stemOutput(output, role.role));
            queueExportAs(sequenceKey(sequence->id), presetId, stem, origin);
        }
    }
    return job;
}

QString ExportController::quickExport(const QString& presetId, const QString& output, const QVariantMap& extra) {
    QVariantMap overrides = extra;
    if (!output.isEmpty()) {
        overrides[QStringLiteral("output")] = output;
    }
    return queueExportAs(QString(), presetId, overrides, QStringLiteral("quick"));
}

std::expected<ExportController::Transcode, Error> ExportController::queueTranscode(const MediaItem& source,
                                                                                  const QString& presetId,
                                                                                  const std::filesystem::path& folder) {
    auto settings = settingsFor(presetId, {});
    if (!settings) {
        return std::unexpected(settings.error());
    }
    std::error_code ec;
    if (folder.empty() || !std::filesystem::is_directory(folder, ec)) {
        return std::unexpected(Error{ErrorCode::NotFound, "The folder to transcode into is not there"});
    }
    // The item alone in a project: on V1 (and its audio on A1..) of a sequence of its own size and rate.
    edit::SequenceSettings sequence;
    sequence.name = source.name;
    if (source.info.hasVideo()) {
        const VideoStreamInfo& v = source.info.video.front();
        if (v.width > 0 && v.height > 0) {
            sequence.width = v.width;
            sequence.height = v.height;
        }
        if (v.frameRate.num > 0 && v.frameRate.den > 0) {
            sequence.frameRate = v.frameRate;
        }
    }
    MediaItem copy = source;
    copy.id = MediaId{};
    copy.bin = BinId{};
    copy.markIn.reset();
    copy.markOut.reset();
    copy.subclip.reset();
    copy.locator.proxy.reset(); // the original is what is transcoded
    auto withMedia = edit::addMedia(ProjectState{}, copy);
    if (!withMedia) {
        return std::unexpected(withMedia.error());
    }
    auto withSequence = edit::addSequence(withMedia->state, sequence);
    if (!withSequence) {
        return std::unexpected(withSequence.error());
    }
    EditSession session(std::move(withSequence->state));
    if (auto dropped = editor::dropMedia(session, {.media = withMedia->id, .sequence = withSequence->id, .at = Ticks{}});
        !dropped) {
        return std::unexpected(dropped.error());
    }
    const ProjectSnapshot snap = session.snapshot();
    if (const auto r = ex::resolve(*settings, *snap->findSequence(withSequence->id), snap->colorManagement); !r) {
        return std::unexpected(r.error());
    }
    // "<name><extension>", numbered "<name> (2)<extension>", ... when taken (like every other export's file name).
    const std::filesystem::path stem = pathFromUtf8(source.name.empty() ? std::string("Transcoded") : source.name).filename();
    const std::string ext(ex::outputExtension(*settings));
    std::filesystem::path output = folder / stem;
    output += ext;
    for (int n = 2; std::filesystem::exists(output, ec) && n < 10000; ++n) {
        output = folder / stem;
        output += " (" + std::to_string(n) + ")" + ext;
    }
    ex::ExportRequest request;
    request.snapshot = snap;
    request.sequence = withSequence->id;
    request.settings = *settings;
    request.output = output;
    request.videoSource = m_videoSource;
    const auto user = settings->presetId.starts_with("user-") ? userPresetStore()->find(settings->presetId) : std::nullopt;
    const QString job = queueRequestAs(newJobId(), std::move(request),
                                       user ? fromUtf8(user->name) : presetName(settings->presetId), false, QString(),
                                       QStringLiteral("transcode"));
    return Transcode{job, output};
}

QStringList ExportController::sendToQueue(const QString& sequenceId, const QStringList& presetIds,
                                          const QVariantMap& overrides, bool hold) {
    // Every destination is checked first: nothing is queued when one of them cannot export.
    for (const QString& id : presetIds) {
        const QVariantMap check = describe(sequenceId, id, overrides);
        if (!check.value(QStringLiteral("ok")).toBool()) {
            setLastError(check.value(QStringLiteral("error")).toString());
            return {};
        }
    }
    QStringList jobs;
    for (const QString& id : presetIds) {
        QVariantMap o = overrides;
        o[QStringLiteral("hold")] = hold;
        if (presetIds.size() > 1) {
            o.remove(QStringLiteral("output")); // each destination gets its own default name
        }
        if (id != presetIds.front()) {
            o.remove(QStringLiteral("stems")); // 0.6 (#52): one set of stems, after the first destination
        }
        const QString job = queueExportAs(sequenceId, id, o, QStringLiteral("queue"));
        if (!job.isEmpty()) {
            jobs << job;
        }
    }
    return jobs;
}

void ExportController::setContentCredentials(const QString& mode) {
    if (mode == m_contentCredentials || !ai::c2pa::credentialsModeFromString(mode.toStdString())) {
        return;
    }
    m_contentCredentials = mode;
    QSettings().setValue(QStringLiteral("export/contentCredentials"), mode);
    emit contentCredentialsChanged();
}

bool ExportController::contentCredentialsAvailable() const { return ai::c2pa::available(); }

void ExportController::setCredentialsProducer(const QString& name) {
    const QString trimmed = name.trimmed().left(256);
    if (trimmed == m_credentialsProducer) {
        return;
    }
    m_credentialsProducer = trimmed;
    QSettings().setValue(QStringLiteral("export/c2paProducer"), trimmed);
    emit contentCredentialsChanged();
}

void ExportController::setCredentialsThumbnail(bool on) {
    if (on == m_credentialsThumbnail) {
        return;
    }
    m_credentialsThumbnail = on;
    QSettings().setValue(QStringLiteral("export/c2paThumbnail"), on);
    emit contentCredentialsChanged();
}

void ExportController::setCredentialsDoNotTrain(bool on) {
    if (on == m_credentialsDoNotTrain) {
        return;
    }
    m_credentialsDoNotTrain = on;
    QSettings().setValue(QStringLiteral("export/c2paDoNotTrain"), on);
    emit contentCredentialsChanged();
}

void ExportController::setCredentialsCertificate(const QString& path) {
    const QString local = path.startsWith(QLatin1String("file:")) ? QUrl(path).toLocalFile() : path;
    if (local == m_credentialsCertificate) {
        return;
    }
    m_credentialsCertificate = local;
    QSettings().setValue(QStringLiteral("export/c2paCertificate"), local);
    emit contentCredentialsChanged();
}

void ExportController::setCredentialsKey(const QString& path) {
    const QString local = path.startsWith(QLatin1String("file:")) ? QUrl(path).toLocalFile() : path;
    if (local == m_credentialsKey) {
        return;
    }
    m_credentialsKey = local;
    QSettings().setValue(QStringLiteral("export/c2paKey"), local);
    emit contentCredentialsChanged();
}

QVariantMap ExportController::signingIdentity() const {
    QVariantMap out{{QStringLiteral("local"), m_credentialsCertificate.isEmpty() || m_credentialsKey.isEmpty()},
                    {QStringLiteral("ready"), false},
                    {QStringLiteral("subject"), QString()},
                    {QStringLiteral("notAfter"), QString()},
                    {QStringLiteral("error"), QString()}};
    const auto fill = [&](const ai::c2pa::SigningIdentity& id) {
        out[QStringLiteral("ready")] = true;
        out[QStringLiteral("subject")] = QString::fromStdString(id.subject);
        out[QStringLiteral("notAfter")] = QString::fromStdString(id.notAfter);
    };
    if (!out.value(QStringLiteral("local")).toBool()) {
        if (m_testIdentity) {
            if (auto id = m_testIdentity()) {
                fill(*id);
            }
            return out;
        }
        const auto read = [](const QString& file) -> std::optional<std::string> {
            std::ifstream in(std::filesystem::path(file.toStdWString()), std::ios::binary);
            if (!in) {
                return std::nullopt;
            }
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        };
        auto chain = read(m_credentialsCertificate);
        auto key = read(m_credentialsKey);
        if (!chain || !key) {
            out[QStringLiteral("error")] = tr("The certificate or key file cannot be read.");
            return out;
        }
        auto id = ai::c2pa::identityFromPem(std::move(*chain), ai::Secret(std::move(*key)));
        if (!id) {
            out[QStringLiteral("error")] = QString::fromStdString(id.error().message);
            return out;
        }
        fill(*id);
        return out;
    }
    // The local identity, if one was made already (the first signed export makes it).
    if (auto keychain = ai::c2pa::osKeychain()) {
        if (auto id = ai::c2pa::localIdentity(*keychain, ai::c2pa::defaultIdentityDir(), false)) {
            fill(*id);
        }
    }
    return out;
}

ai::c2pa::IdentityProvider ExportController::credentialsIdentity() const {
    if (m_testIdentity) {
        return m_testIdentity;
    }
    // The user's own certificate when the settings name its PEM files (docs/ai.md "Content Credentials"); else the
    // per-user local identity, made on first use, its key in the OS keychain. Read on the export thread.
    QSettings settings;
    const std::filesystem::path certificate(settings.value(QStringLiteral("export/c2paCertificate")).toString().toStdWString());
    const std::filesystem::path key(settings.value(QStringLiteral("export/c2paKey")).toString().toStdWString());
    return [certificate, key]() -> std::expected<ai::c2pa::SigningIdentity, ai::AiError> {
        if (!certificate.empty() && !key.empty()) {
            const auto read = [](const std::filesystem::path& file) -> std::optional<std::string> {
                std::ifstream in(file, std::ios::binary);
                if (!in) {
                    return std::nullopt;
                }
                return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            };
            auto chain = read(certificate);
            auto pem = read(key);
            if (!chain || !pem) {
                return std::unexpected(ai::AiError(ai::AiErrorCode::InvalidRequest, "the signing certificate or key file cannot be read"));
            }
            return ai::c2pa::identityFromPem(std::move(*chain), ai::Secret(std::move(*pem)));
        }
        auto keychain = ai::c2pa::osKeychain();
        if (!keychain) {
            return std::unexpected(ai::AiError(ai::AiErrorCode::Unsupported, "there is no OS keychain for the signing key here"));
        }
        return ai::c2pa::localIdentity(*keychain, ai::c2pa::defaultIdentityDir(), true);
    };
}

QString ExportController::newJobId() {
    return QStringLiteral("j%1").arg(m_nextJob.fetch_add(1));
}

ex::ExportJobId ExportController::queueIdOf(const QString& jobId) const {
    std::lock_guard lock(m_idsMutex);
    const auto it = m_queueIds.find(jobId);
    return it == m_queueIds.end() ? 0 : it->second;
}

QString ExportController::queueRequest(ex::ExportRequest request, const QString& presetLabel, const QString& credentials) {
    return queueRequestAs(newJobId(), std::move(request), presetLabel, false, credentials);
}

QString ExportController::queueRequestAs(const QString& jobIdIn, ex::ExportRequest request, const QString& presetLabel,
                                         bool hold, const QString& credentials, const QString& origin) {
    ExportJobModel::Row row;
    row.jobId = jobIdIn;
    row.held = hold;
    row.origin = origin;
    if (hold) {
        row.state = QStringLiteral("held");
    }
    row.title = QString::fromStdU16String(request.output.filename().u16string());
    row.outputPath = pathToQString(request.output);
    row.presetName = presetLabel;
    row.phaseText = hold ? translate("Ready") : translate("Queued");
    const QString jobId = row.jobId;
    ai::Json provenance;
    std::optional<TimeRange> exported;
    if (request.snapshot) if (const auto* seq = request.snapshot->findSequence(request.sequence)) {
        const auto resolved = ex::resolve(request.settings, *seq, request.snapshot->colorManagement);
        if (resolved) {
            provenance = ai::provenanceReport(*request.snapshot, request.sequence, resolved->range);
            exported = resolved->range;
        }
    }
    // AI-15: signed Content Credentials in MP4 / MOV exports when the setting asks for them. The finalize step signs the
    // complete file on the export thread; it never fails the export (ExportRequest::finalize).
    std::shared_ptr<std::atomic<bool>> signedFlag;
    const auto mode = ai::c2pa::credentialsModeFromString((credentials.isEmpty() ? m_contentCredentials : credentials).toStdString())
                          .value_or(ai::c2pa::CredentialsMode::Ai);
    const std::string format = ai::c2pa::formatForPath(request.output);
    if (request.snapshot && !request.finalize && mode != ai::c2pa::CredentialsMode::Off && !format.empty() &&
        ai::c2pa::available() &&
        (mode == ai::c2pa::CredentialsMode::Always || ai::c2pa::containsGenerated(*request.snapshot, request.sequence, exported))) {
        ai::c2pa::ManifestOptions options;
        options.title = row.title.toStdString();
        options.format = format;
        options.softwareVersion = WABCUT_VERSION;
        options.range = exported;
        options.projectFile = request.projectFile;
        // 0.2: Premiere's Content Credentials settings - the producer, a thumbnail of the export, no AI training.
        options.producer = m_credentialsProducer.toStdString();
        options.doNotTrain = m_credentialsDoNotTrain;
        if (m_credentialsThumbnail) {
            options.thumbnail = exportThumbnail(request.snapshot, request.sequence, exported);
        }
        signedFlag = std::make_shared<std::atomic<bool>>(false);
        request.finalize = ai::c2pa::exportFinalizer(request.snapshot, request.sequence, std::move(options), credentialsIdentity(), signedFlag);
    }
    row.restored = m_restoring; // P1 (#20): brought back from the last session (or its Retry)
    keepJob(jobId, request, presetLabel, credentials, origin); // P1 (#20): kept across restarts
    row.queueId = m_queue->enqueue(std::move(request), hold);
    {
        std::lock_guard lock(m_idsMutex);
        m_queueIds[row.jobId] = row.queueId;
    }
    if (!provenance.is_null()) m_provenance[row.queueId] = std::move(provenance);
    if (signedFlag) m_signed[row.queueId] = std::move(signedFlag);
    m_jobs->append(std::move(row));
    emit jobQueued(jobId);
    return jobId;
}

std::string ExportController::provenanceSidecarText(const ai::Json& provenance) {
    // Replace invalid UTF-8 instead of throwing out of a slot (review P2-48; project strings are validated on load).
    return provenance.dump(2, ' ', false, ai::Json::error_handler_t::replace);
}

void ExportController::onStatus(const ex::ExportJobStatus& status) {
    // 0.4: a finished job is reported with its result or error (ExportQueue::run); a terminal phase without either
    // would finish the row without its reason or warnings - the report that has them follows.
    const bool terminal = status.progress.phase == ex::ExportPhase::Done ||
                          status.progress.phase == ex::ExportPhase::Failed ||
                          status.progress.phase == ex::ExportPhase::Cancelled;
    if (terminal && !status.result && !status.error) {
        return;
    }
    // 0.2: a job queued by a watch folder appears in the list when it reports first.
    if (std::none_of(m_jobs->rows().begin(), m_jobs->rows().end(), [&](const ExportJobModel::Row& r) { return r.queueId == status.id; })) {
        ExportJobModel::Row row;
        row.jobId = newJobId();
        row.queueId = status.id;
        row.title = QString::fromStdU16String(status.output.filename().u16string());
        row.outputPath = pathToQString(status.output);
        row.presetName = presetName(status.presetId);
        row.phaseText = translate("Queued");
        row.origin = QStringLiteral("watch");
        {
            std::lock_guard lock(m_idsMutex);
            m_queueIds[row.jobId] = row.queueId;
        }
        m_jobs->append(std::move(row));
        if (status.progress.phase != ex::ExportPhase::Queued) {
            emit watchFoldersChanged(); // its counters moved
        }
    }
    const bool wasFinished = [&] {
        for (const auto& r : m_jobs->rows()) {
            if (r.queueId == status.id) {
                return r.finished;
            }
        }
        return true;
    }();
    const ExportJobModel::Row* row = m_jobs->update(status.id, status);
    if (row && row->finished && !wasFinished) {
        bool signedFile = false;
        if (auto it = m_signed.find(status.id); it != m_signed.end()) {
            signedFile = it->second->load();
            m_signed.erase(it);
        }
        if (auto it = m_provenance.find(status.id); it != m_provenance.end()) {
            if (row->state == QLatin1String("done")) {
                auto sidecar = std::filesystem::path(row->outputPath.toStdWString()); sidecar += ".provenance.json";
                if (signedFile) {
                    // The signed manifest in the file says it: no unsigned sidecar next to it, nor one left from before.
                    std::error_code ec;
                    std::filesystem::remove(sidecar, ec);
                } else if (auto wrote = io::writeFileAtomically(sidecar, provenanceSidecarText(it->second)); !wrote)
                    setLastError(tr("Export finished, but its unsigned provenance sidecar could not be written."));
            }
            m_provenance.erase(it);
        }
        if (status.result && !status.result->warnings.empty()) { // e.g. "Content Credentials not signed: ..."
            QStringList warnings;
            for (const std::string& w : status.result->warnings)
                warnings << QString::fromStdString(w);
            setLastError(tr("Export finished. %1").arg(warnings.join(QLatin1Char(' '))));
        }
        emit jobFinished(row->jobId, row->state == QLatin1String("done"), row->detail);
        if (m_watch && !m_watch->folders().empty()) {
            emit watchFoldersChanged(); // a watch folder's counters may have moved
        }
    }
}

void ExportController::cancel(const QString& jobId) {
    if (const ExportJobModel::Row* row = m_jobs->find(jobId)) {
        m_queue->cancel(row->queueId);
    }
}

void ExportController::cancelAll() {
    for (const ExportJobModel::Row& r : m_jobs->rows()) {
        if (!r.finished) {
            m_queue->cancel(r.queueId);
        }
    }
    // P1 (#20): cancelled for good, also when Wabcut closes before the cancellations are reported (Cancel Exports and
    // Quit): nothing of them comes back next time.
    dropKept(true);
}

void ExportController::start(const QString& jobId) {
    if (const ExportJobModel::Row* row = m_jobs->find(jobId)) {
        m_queue->start(row->queueId);
    }
}

void ExportController::startQueue() {
    for (const ExportJobModel::Row& r : m_jobs->rows()) {
        if (!r.finished && r.held) {
            m_queue->start(r.queueId);
        }
    }
}

void ExportController::pause(const QString& jobId) {
    if (const ExportJobModel::Row* row = m_jobs->find(jobId)) {
        m_queue->pause(row->queueId);
    }
}

void ExportController::resume(const QString& jobId) {
    if (const ExportJobModel::Row* row = m_jobs->find(jobId)) {
        m_queue->resume(row->queueId);
    }
}

// UX rework fix (L): `jobId` may also be the absolute path of an export's file. A toast's "Show in Explorer" whose job
// row went meanwhile (Clear Finished, an export "remove" command) shows the file it remembered that way; false when
// that file is gone (the toast says so) - a job's own file falls back to its folder, as before.
bool ExportController::reveal(const QString& jobId) {
    QString path;
    if (const ExportJobModel::Row* row = m_jobs->find(jobId)) {
        path = row->outputPath;
    } else if (const QString given = QDir::fromNativeSeparators(jobId); QDir::isAbsolutePath(given)) {
        if (!QFileInfo(given).isFile()) {
            return false;
        }
        path = QDir::toNativeSeparators(QDir::cleanPath(given));
    } else {
        return false;
    }
    if (m_reveal) {
        return m_reveal(path);
    }
    const QFileInfo file(path);
#if defined(Q_OS_WIN)
    if (file.exists()) {
        return QProcess::startDetached(
            QStringLiteral("explorer.exe"),
            {QStringLiteral("/select,") + QDir::toNativeSeparators(file.filePath())});
    }
#elif defined(Q_OS_MACOS)
    if (file.exists()) {
        return QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), file.filePath()});
    }
#endif
    return QDesktopServices::openUrl(QUrl::fromLocalFile(file.absolutePath()));
}

void ExportController::clearFinished() {
    m_jobs->removeFinished();
}

void ExportController::checkDiskSpace() {
    constexpr double kLow = 2e9; // under 2 GB a long export may not fit any more
    for (const ExportJobModel::Row& r : m_jobs->rows()) {
        if (r.finished || r.held || r.outputPath.isEmpty()) {
            continue;
        }
        const QString folder = QFileInfo(r.outputPath).absolutePath();
        const qint64 free = diskspace::freeBytesAt(folder);
        const QString volume = diskspace::volumeNameOf(folder);
        if (free < 0 || static_cast<double>(free) >= kLow || m_spaceWarned.contains(volume)) {
            continue;
        }
        m_spaceWarned << volume;
        //: While an export runs, e.g. "Only 1.4 GB left on Data while exporting Trailer.mp4: free some space, or the
        //: export may stop when the drive is full."
        emit diskSpaceLow(translate("Only %1 left on %2 while exporting %3: free some space, or the export may stop when "
                                    "the drive is full.")
                              .arg(diskspace::bytesText(static_cast<double>(free)), volume, r.title));
    }
}

QString ExportController::retryOutputPath(const QString& path, const QDateTime& since, const QDateTime& replaced) const {
    const QFileInfo was(path);
    const QString wanted = was.absoluteFilePath();
    const auto writtenByJob = [&](const QString& candidate) {
        return std::any_of(m_jobs->rows().begin(), m_jobs->rows().end(), [&](const ExportJobModel::Row& r) {
            return !r.finished && QDir::cleanPath(QDir::fromNativeSeparators(r.outputPath)) == QDir::cleanPath(candidate);
        });
    };
    if (!writtenByJob(wanted)) {
        const QFileInfo now(wanted);
        if (!now.exists())
            return wanted;
        const QDateTime modified = now.lastModified().toUTC();
        if (now.isFile() && ((replaced.isValid() && modified == replaced) || (since.isValid() && modified <= since)))
            return wanted; // the file the export was to replace (the user chose it)
    }
    // "Clip (2).mp4", "Clip (3).mp4", ... from the name without an earlier number.
    static const QRegularExpression kNumbered(QStringLiteral(R"(^(.*) \((\d+)\)$)"));
    QString base = was.completeBaseName();
    if (const auto m = kNumbered.match(base); m.hasMatch())
        base = m.captured(1);
    const QString ext = was.suffix().isEmpty() ? QString() : QStringLiteral(".") + was.suffix();
    QString output = wanted;
    for (int n = 2; n < 1000 && (QFileInfo::exists(output) || writtenByJob(output)); ++n)
        output = was.dir().filePath(QStringLiteral("%1 (%2)%3").arg(base).arg(n).arg(ext));
    return output;
}

QString ExportController::retry(const QString& jobId) {
    const ExportJobModel::Row* row = m_jobs->find(jobId);
    if (row && row->restored) {
        return retryKept(jobId); // P1 (#20): its kept project and settings, not the project open now
    }
    if (!row || row->presetId.isEmpty() ||
        (row->state != QLatin1String("failed") && row->state != QLatin1String("cancelled"))) {
        setLastError(translate("This export cannot be retried: export it again from the Export panel."));
        return {};
    }
    // The same request on the project as it is now, into the same file (a failed or cancelled export never wrote it:
    // a file there from before it was queued is the one it was to replace), or a numbered one when another job writes
    // that name now or a file was written there since.
    QVariantMap overrides = row->overrides;
    overrides.remove(QStringLiteral("hold")); // a retry starts at once
    if (!row->outputPath.isEmpty()) {
        overrides[QStringLiteral("output")] = QDir::toNativeSeparators(retryOutputPath(row->outputPath, row->queuedAt));
    }
    const QString sequenceId = row->sequenceId;
    const QString presetId = row->presetId;
    const QString origin = row->origin;
    const bool chosenOutput = row->overrides.contains(QStringLiteral("output"));
    const QString folderBefore = lastFolder();
    const QString job = queueExportAs(sequenceId, presetId, overrides, origin); // `row` may move: not used after this
    if (!chosenOutput) {
        setLastFolder(folderBefore); // a retry of a default name does not make its folder the next default
    }
    return job;
}

// ---- UX rework (L): finished jobs for toasts, and the remembered choices ---------------------------------------------

namespace {

const QString kLastPresetKey = QStringLiteral("export/lastPreset");
const QString kLastGamingPresetKey = QStringLiteral("export/lastGamingPreset");
const QString kLastQuickPresetKey = QStringLiteral("export/lastQuickPreset");
const QString kLastFolderKey = QStringLiteral("export/lastFolder");
const QString kLastRangeKey = QStringLiteral("export/lastRange");
const QString kLastTargetKey = QStringLiteral("export/lastTargetMB"); // P1 (#28)
const QString kDefaultPreset = QStringLiteral("match-source");
// P1 (#21): a vertical sequence's own choices (the keys above are the landscape ones, as before).
const QString kPortraitSuffix = QStringLiteral("Portrait");

/// The key of `key` for a sequence of this shape.
QString shapedKey(const QString& key, bool portrait) {
    return portrait ? key + kPortraitSuffix : key;
}

bool presetExists(const QString& id) {
    return !id.isEmpty() && ExportController::userPresetStore()->findAny(id.toStdString()).has_value();
}

bool gamingPresetExists(const QString& id) {
    if (id.isEmpty())
        return false;
    const GamingExport gaming;
    const QVariantList presets = gaming.presets();
    return std::any_of(presets.begin(), presets.end(),
                       [&](const QVariant& p) { return p.toMap().value(QStringLiteral("id")).toString() == id; });
}

/// A local folder from a path or a file URL ("" for anything else).
QString localFolderOf(const QString& pathOrUrl) {
    if (pathOrUrl.isEmpty())
        return {};
    const QUrl url(pathOrUrl);
    const QString path = url.isLocalFile() ? url.toLocalFile() : pathOrUrl;
    return QDir::isAbsolutePath(path) ? QDir::cleanPath(path) : QString();
}

} // namespace

QVariantMap ExportController::job(const QString& jobId) const {
    const ExportJobModel::Row* row = m_jobs->find(jobId);
    if (!row) {
        return {};
    }
    const QFileInfo file(row->outputPath);
    return {{QStringLiteral("jobId"), row->jobId},
            {QStringLiteral("title"), row->title},
            {QStringLiteral("outputPath"), QDir::toNativeSeparators(row->outputPath)},
            {QStringLiteral("outputUrl"), QUrl::fromLocalFile(file.absoluteFilePath())},
            {QStringLiteral("folderUrl"), QUrl::fromLocalFile(file.absolutePath())},
            {QStringLiteral("presetName"), row->presetName},
            {QStringLiteral("state"), row->state},
            {QStringLiteral("detail"), row->detail},
            {QStringLiteral("finished"), row->finished},
            {QStringLiteral("origin"), row->origin},
            {QStringLiteral("errorKind"), row->errorKind},
            {QStringLiteral("exists"), file.exists()}};
}

// 0.4: `jobId` may also be the absolute path of an export's file (as reveal()): a toast's "Open" whose job row went
// meanwhile (Clear Finished) opens the file it remembered, while that file is there.
bool ExportController::openOutput(const QString& jobId) {
    QString path;
    if (const ExportJobModel::Row* row = m_jobs->find(jobId)) {
        if (row->state != QLatin1String("done")) {
            return false;
        }
        path = row->outputPath;
    } else if (const QString given = QDir::fromNativeSeparators(jobId); QDir::isAbsolutePath(given)) {
        path = QDir::toNativeSeparators(QDir::cleanPath(given));
    } else {
        return false;
    }
    if (!QFileInfo(path).isFile()) {
        return false;
    }
    if (m_open) {
        return m_open(path);
    }
    return QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()));
}

QString ExportController::suggestedRange(const QString& sequenceId) const {
    const SequencePtr sequence = sequenceFor(sequenceId);
    const QString remembered = lastRange();
    if (sequence && !remembered.isEmpty()) {
        if (remembered == QLatin1String("whole")) {
            return remembered;
        }
        if (remembered == QLatin1String("inOut") && sequence->markedRange().has_value()) {
            return remembered;
        }
        if (remembered == QLatin1String("workArea") && sequence->workArea.has_value()) {
            return remembered;
        }
    }
    return sequence && sequence->markedRange().has_value() ? QStringLiteral("inOut") : QStringLiteral("whole");
}

QUrl ExportController::folderUrlOf(const QString& pathOrUrl) const {
    const QString path = localFolderOf(pathOrUrl);
    if (path.isEmpty()) {
        return {};
    }
    const QFileInfo info(path);
    return QUrl::fromLocalFile(info.isDir() ? info.absoluteFilePath() : info.absolutePath());
}

void ExportController::forgetExportChoices() {
    QSettings settings;
    for (const QString& key : {kLastPresetKey, kLastGamingPresetKey, kLastQuickPresetKey, kLastFolderKey, kLastRangeKey,
                               kLastTargetKey,
                               shapedKey(kLastPresetKey, true), shapedKey(kLastGamingPresetKey, true),
                               shapedKey(kLastQuickPresetKey, true)}) {
        settings.remove(key);
    }
    emit exportMemoryChanged();
}

bool ExportController::portraitSequence(const QString& sequenceId) const {
    const SequencePtr sequence = sequenceFor(sequenceId);
    return sequence && sequence->height > sequence->width;
}

QVariantMap ExportController::rememberedFor(const QString& sequenceId) const {
    const bool portrait = portraitSequence(sequenceId);
    const QSettings settings;
    const QString preset = settings.value(shapedKey(kLastPresetKey, portrait)).toString();
    const QString gaming = settings.value(shapedKey(kLastGamingPresetKey, portrait)).toString();
    const QString quick = settings.value(shapedKey(kLastQuickPresetKey, portrait)).toString();
    return {{QStringLiteral("portrait"), portrait},
            {QStringLiteral("preset"), presetExists(preset) ? preset : kDefaultPreset},
            {QStringLiteral("gamingPreset"), gamingPresetExists(gaming) ? gaming : QString()},
            {QStringLiteral("quickPreset"), presetExists(quick) ? quick : kDefaultPreset}};
}

// P1 (#21): the preset choices read and write the current sequence's shape (landscape: the keys of old).
QString ExportController::lastPreset() const {
    const QString id = QSettings().value(shapedKey(kLastPresetKey, portraitSequence({}))).toString();
    return presetExists(id) ? id : kDefaultPreset;
}

void ExportController::setLastPreset(const QString& id) {
    const QString key = shapedKey(kLastPresetKey, portraitSequence({}));
    if (!presetExists(id) || QSettings().value(key).toString() == id) {
        return;
    }
    QSettings().setValue(key, id);
    emit exportMemoryChanged();
}

QString ExportController::lastGamingPreset() const {
    const QString id = QSettings().value(shapedKey(kLastGamingPresetKey, portraitSequence({}))).toString();
    return gamingPresetExists(id) ? id : QString();
}

void ExportController::setLastGamingPreset(const QString& id) {
    const QString key = shapedKey(kLastGamingPresetKey, portraitSequence({}));
    if ((!id.isEmpty() && !gamingPresetExists(id)) || QSettings().value(key).toString() == id) {
        return;
    }
    QSettings().setValue(key, id);
    emit exportMemoryChanged();
}

QString ExportController::lastQuickPreset() const {
    const QString id = QSettings().value(shapedKey(kLastQuickPresetKey, portraitSequence({}))).toString();
    return presetExists(id) ? id : kDefaultPreset;
}

void ExportController::setLastQuickPreset(const QString& id) {
    const QString key = shapedKey(kLastQuickPresetKey, portraitSequence({}));
    if (!presetExists(id) || QSettings().value(key).toString() == id) {
        return;
    }
    QSettings().setValue(key, id);
    emit exportMemoryChanged();
}

QString ExportController::lastFolder() const {
    const QString folder = QSettings().value(kLastFolderKey).toString();
    return !folder.isEmpty() && QFileInfo(folder).isDir() ? QDir::toNativeSeparators(folder) : QString();
}

void ExportController::setLastFolder(const QString& folder) {
    const QString path = localFolderOf(folder);
    if (QDir::cleanPath(QSettings().value(kLastFolderKey).toString()) == path) {
        return;
    }
    QSettings().setValue(kLastFolderKey, path);
    emit exportMemoryChanged();
}

QUrl ExportController::lastFolderUrl() const {
    const QString folder = lastFolder();
    return folder.isEmpty() ? QUrl() : QUrl::fromLocalFile(QDir::fromNativeSeparators(folder));
}

QString ExportController::lastRange() const {
    const QString range = QSettings().value(kLastRangeKey).toString();
    return range == QLatin1String("whole") || range == QLatin1String("inOut") || range == QLatin1String("workArea")
               ? range
               : QString();
}

void ExportController::setLastRange(const QString& range) {
    const QString next = range == QLatin1String("whole") || range == QLatin1String("inOut") || range == QLatin1String("workArea")
                             ? range
                             : QString();
    if (QSettings().value(kLastRangeKey).toString() == next) {
        return;
    }
    QSettings().setValue(kLastRangeKey, next);
    emit exportMemoryChanged();
}

double ExportController::lastTargetMB() const {
    const double mb = QSettings().value(kLastTargetKey, 0.0).toDouble();
    return mb >= 1.0 && mb <= 4000.0 ? mb : 0.0;
}

void ExportController::setLastTargetMB(double megabytes) {
    const double next = megabytes >= 1.0 && megabytes <= 4000.0 ? megabytes : 0.0;
    if (std::abs(lastTargetMB() - next) < 1e-9) {
        return;
    }
    QSettings().setValue(kLastTargetKey, next);
    emit exportMemoryChanged();
}

void ExportController::probeEncoders() {
    if (m_probe.joinable()) {
        return; // probing or probed: encoders do not change while the app runs
    }
    m_probe = std::jthread([relay = m_relay] {
        QVariantMap picks;
        for (const ex::VideoCodec codec :
             {ex::VideoCodec::H264, ex::VideoCodec::Hevc, ex::VideoCodec::ProRes, ex::VideoCodec::DnxHR}) {
            ex::VideoSettings video;
            video.codec = codec;
            video.pixelFormat = codec == ex::VideoCodec::ProRes  ? ex::OutputPixelFormat::Yuv422p10
                                : codec == ex::VideoCodec::DnxHR ? ex::OutputPixelFormat::Yuv422p
                                                                 : ex::OutputPixelFormat::Yuv420p;
            const auto probe = ex::probeVideoEncoder(video, 1920, 1080, kRate30);
            picks[fromUtf8(ex::videoCodecName(codec))] = probe ? fromUtf8(probe->encoder) : QString();
        }
        relay->post([picks](ExportController* target) {
            target->m_autoEncoders = picks;
            emit target->autoEncodersChanged();
        });
    });
}

void ExportController::setLastError(const QString& text) {
    if (text != m_lastError) {
        m_lastError = text;
        emit lastErrorChanged();
    }
}

void ExportController::setProjectFile(std::filesystem::path file) {
    m_projectFile = std::move(file);
    std::lock_guard lock(m_idsMutex);
    m_sharedProjectFile = m_projectFile;
}

std::expected<QVariantMap, Error> ExportController::preparedOverrides(const QVariantMap& overrides,
                                                                       const Sequence* sequence) const {
    QVariantMap out = overrides;
    // Custom range: In / Out as timecodes in the sequence's rate (or ticks).
    if (out.contains(QStringLiteral("rangeIn")) || out.contains(QStringLiteral("rangeOut"))) {
        const QVariant in = out.take(QStringLiteral("rangeIn"));
        const QVariant outPoint = out.take(QStringLiteral("rangeOut"));
        if (sequence && in.isValid() && outPoint.isValid() && !in.isNull() && !outPoint.isNull()) {
            const auto ticksOf = [&](const QVariant& v) -> std::optional<Ticks> {
                bool isNumber = false;
                const qlonglong n = v.toLongLong(&isNumber);
                if (isNumber && v.typeId() != QMetaType::QString) {
                    return Ticks{n};
                }
                auto t = parseTimecode(v.toString().trimmed().toStdString(), sequence->frameRate, sequence->dropFrame);
                return t ? std::optional<Ticks>(*t) : std::nullopt;
            };
            const auto start = ticksOf(in);
            const auto end = ticksOf(outPoint);
            if (!start || !end || !(*start < *end) || start->value < 0) {
                return std::unexpected(Error{ErrorCode::InvalidArgument, "Custom range: the In point must be before the Out point"});
            }
            out[QStringLiteral("customRange")] =
                QVariantMap{{QStringLiteral("start"), QVariant::fromValue<qlonglong>(start->value)},
                            {QStringLiteral("end"), QVariant::fromValue<qlonglong>(end->value)}};
        }
    }
    // Files the effects read, chosen in a file dialog (URLs).
    if (out.contains(QStringLiteral("effects"))) {
        QVariantMap fx = out.value(QStringLiteral("effects")).toMap();
        const auto local = [](QVariantMap section, const QString& key) {
            const QString value = section.value(key).toString();
            if (value.startsWith(QLatin1String("file:"))) {
                section[key] = QUrl(value).toLocalFile();
            }
            return section;
        };
        if (fx.contains(QStringLiteral("imageOverlay"))) {
            fx[QStringLiteral("imageOverlay")] = local(fx.value(QStringLiteral("imageOverlay")).toMap(), QStringLiteral("image"));
        }
        if (fx.contains(QStringLiteral("look"))) {
            fx[QStringLiteral("look")] = local(fx.value(QStringLiteral("look")).toMap(), QStringLiteral("lut"));
        }
        out[QStringLiteral("effects")] = fx;
    }
    // Timecodes typed in the panel (Set Start Timecode, the Timecode Overlay's starting timecode) into ticks.
    const auto timecodeField = [&](QVariantMap section, const QString& key) -> std::expected<QVariantMap, Error> {
        const QVariant v = section.value(key);
        if (v.typeId() != QMetaType::QString) {
            return section;
        }
        const QString text = v.toString().trimmed();
        if (text.isEmpty()) {
            section.remove(key);
            return section;
        }
        const Rate rate = sequence ? sequence->frameRate : kRate30;
        const auto t = parseTimecode(text.toStdString(), rate, sequence && sequence->dropFrame);
        if (!t || t->value < 0) {
            return std::unexpected(Error{ErrorCode::InvalidArgument,
                                         QStringLiteral("%1 is not a timecode (hh:mm:ss:ff)").arg(text).toStdString()});
        }
        section[key] = QVariant::fromValue<qlonglong>(t->value);
        return section;
    };
    if (out.contains(QStringLiteral("metadata"))) {
        auto m = timecodeField(out.value(QStringLiteral("metadata")).toMap(), QStringLiteral("startTimecode"));
        if (!m) {
            return std::unexpected(m.error());
        }
        out[QStringLiteral("metadata")] = *m;
    }
    if (out.contains(QStringLiteral("effects"))) {
        QVariantMap fx = out.value(QStringLiteral("effects")).toMap();
        if (fx.contains(QStringLiteral("timecodeOverlay"))) {
            auto tc = timecodeField(fx.value(QStringLiteral("timecodeOverlay")).toMap(), QStringLiteral("startTimecode"));
            if (!tc) {
                return std::unexpected(tc.error());
            }
            fx[QStringLiteral("timecodeOverlay")] = *tc;
            out[QStringLiteral("effects")] = fx;
        }
    }
    return out;
}

QVariantMap ExportController::savePreset(const QString& name, const QString& description, const QString& presetId,
                                         const QVariantMap& overrides, bool overwrite) {
    QVariantMap out{{QStringLiteral("ok"), false}};
    const auto prepared = preparedOverrides(overrides, sequenceFor(QString()).get());
    if (!prepared) {
        out[QStringLiteral("error")] = plainError(prepared.error());
        return out;
    }
    QVariantMap clean = *prepared;
    clean.remove(QStringLiteral("output")); // a preset is the format, not a file
    clean.remove(QStringLiteral("contentCredentials"));
    auto settings = settingsFor(presetId, clean);
    if (!settings) {
        out[QStringLiteral("error")] = plainError(settings.error());
        return out;
    }
    auto saved = userPresetStore()->save(name.toStdString(), description.toStdString(), std::move(*settings), overwrite);
    if (!saved) {
        out[QStringLiteral("error")] = plainError(saved.error());
        return out;
    }
    emit presetsChanged();
    out[QStringLiteral("ok")] = true;
    out[QStringLiteral("id")] = fromUtf8(saved->id);
    out[QStringLiteral("name")] = fromUtf8(saved->name);
    return out;
}

QVariantMap ExportController::importPreset(const QString& file, bool overwrite) {
    QVariantMap out{{QStringLiteral("ok"), false}};
    auto imported = userPresetStore()->importFile(outputFromOverride(file), overwrite);
    if (!imported) {
        out[QStringLiteral("error")] = plainError(imported.error());
        return out;
    }
    emit presetsChanged();
    out[QStringLiteral("ok")] = true;
    out[QStringLiteral("id")] = fromUtf8(imported->id);
    out[QStringLiteral("name")] = fromUtf8(imported->name);
    return out;
}

bool ExportController::exportPreset(const QString& id, const QString& file) {
    std::filesystem::path path = outputFromOverride(file);
    if (path.extension().empty()) {
        path += ".json";
    }
    if (auto ok = userPresetStore()->exportFile(id.toStdString(), path); !ok) {
        setLastError(plainError(ok.error()));
        return false;
    }
    return true;
}

bool ExportController::deletePreset(const QString& id) {
    if (auto ok = userPresetStore()->remove(id.toStdString()); !ok) {
        setLastError(plainError(ok.error()));
        return false;
    }
    emit presetsChanged();
    return true;
}

QVariantList ExportController::watchFolders() const {
    QVariantList list;
    if (!m_watch) {
        return list;
    }
    for (const ex::WatchFolderStatus& f : m_watch->folders()) {
        const auto preset = userPresetStore()->findAny(f.folder.presetId);
        list.append(QVariantMap{
            {QStringLiteral("id"), QStringLiteral("w%1").arg(f.id)},
            {QStringLiteral("folder"), pathToQString(f.folder.folder)},
            {QStringLiteral("preset"), fromUtf8(f.folder.presetId)},
            {QStringLiteral("presetName"), preset ? (preset->id.starts_with("user-") ? fromUtf8(preset->name) : presetName(preset->id))
                                                  : fromUtf8(f.folder.presetId)},
            {QStringLiteral("output"), pathToQString(f.folder.output.empty() ? f.folder.folder / "Output" : f.folder.output)},
            {QStringLiteral("moveSources"), f.folder.moveSources},
            {QStringLiteral("queued"), f.queued},
            {QStringLiteral("done"), f.done},
            {QStringLiteral("failed"), f.failed},
            {QStringLiteral("lastError"), fromUtf8(f.lastError)}});
    }
    return list;
}

QVariantMap ExportController::addWatchFolder(const QString& folder, const QString& presetId, const QString& output,
                                             bool moveSources) {
    QVariantMap out{{QStringLiteral("ok"), false}};
    const auto preset = userPresetStore()->findAny(presetId.toStdString());
    if (!preset) {
        out[QStringLiteral("error")] = translate("Choose a preset for the watch folder.");
        return out;
    }
    ex::WatchFolder w;
    w.folder = outputFromOverride(folder);
    w.output = output.isEmpty() ? std::filesystem::path() : outputFromOverride(output);
    w.presetId = preset->id;
    w.settings = preset->settings;
    w.moveSources = moveSources;
    auto id = m_watch->add(std::move(w));
    if (!id) {
        out[QStringLiteral("error")] = plainError(id.error());
        return out;
    }
    saveWatchFolders();
    emit watchFoldersChanged();
    out[QStringLiteral("ok")] = true;
    out[QStringLiteral("id")] = QStringLiteral("w%1").arg(*id);
    return out;
}

void ExportController::removeWatchFolder(const QString& id) {
    bool ok = false;
    const std::uint64_t number = QStringView(id).mid(id.startsWith(QLatin1Char('w')) ? 1 : 0).toULongLong(&ok);
    if (ok && m_watch->remove(number)) {
        saveWatchFolders();
        emit watchFoldersChanged();
    }
}

void ExportController::saveWatchFolders() const {
    ex::Json list = ex::Json::array();
    for (const ex::WatchFolderStatus& f : m_watch->folders()) {
        list.push_back(ex::Json{{"folder", pathToUtf8(f.folder.folder)},
                                {"output", pathToUtf8(f.folder.output)},
                                {"preset", f.folder.presetId},
                                {"moveSources", f.folder.moveSources}});
    }
    QSettings().setValue(QStringLiteral("export/watchFolders"), QByteArray::fromStdString(list.dump()));
}

void ExportController::setCpuLimit(int percent) {
    percent = std::clamp(percent, 10, 100);
    if (percent == m_cpuLimit) {
        return;
    }
    m_cpuLimit = percent;
    ex::setExportThreadLimit(ex::threadLimitForPercent(percent));
    QSettings().setValue(QStringLiteral("export/cpuLimit"), percent);
    emit cpuLimitChanged();
}

std::shared_ptr<ex::ExportCommandServices> ExportController::commandServices() {
    if (m_commandServices) {
        return m_commandServices;
    }
    // The commands run off the GUI thread: queueing posts to it, the queue and the id map are thread-safe.
    ex::ExportCommandServices::Host host;
    QPointer<ExportController> self(this);
    host.newJobId = [self]() { return self ? self->newJobId().toStdString() : std::string("j0"); };
    host.enqueue = [self](std::string job, ex::ExportRequest request, std::string presetName, bool hold) {
        if (!self) {
            return;
        }
        auto shared = std::make_shared<ex::ExportRequest>(std::move(request));
        QMetaObject::invokeMethod(
            self.data(),
            [self, job = QString::fromStdString(job), shared, name = QString::fromStdString(presetName), hold]() {
                if (self) {
                    self->queueRequestAs(job, std::move(*shared), name, hold);
                }
            },
            Qt::QueuedConnection);
    };
    ex::ExportQueue* queue = m_queue.get();
    host.jobs = [self, queue]() {
        ex::Json out = ex::Json::array();
        if (!self) {
            return out;
        }
        std::map<ex::ExportJobId, QString> names;
        {
            std::lock_guard lock(self->m_idsMutex);
            for (const auto& [job, id] : self->m_queueIds) {
                names[id] = job;
            }
        }
        for (const ex::ExportJobStatus& s : queue->jobs()) {
            const auto it = names.find(s.id);
            const std::string state = s.held && s.progress.phase == ex::ExportPhase::Queued ? "held" : std::string(ex::exportPhaseName(s.progress.phase));
            ex::Json j{{"job", it == names.end() ? std::string() : it->second.toStdString()},
                       {"output", pathToUtf8(s.output)},
                       {"preset", s.presetId},
                       {"state", state == "preparing" || state == "rendering" || state == "finalizing" ? "running" : state},
                       {"progress", s.progress.phase == ex::ExportPhase::Done ? 1.0 : s.progress.fraction()}};
            if (s.error) {
                j["detail"] = s.error->message;
            }
            out.push_back(std::move(j));
        }
        return out;
    };
    host.control = [self, queue](std::string_view job, std::string_view action) -> std::expected<void, Error> {
        const ex::ExportJobId id = self ? self->queueIdOf(QString::fromUtf8(job.data(), static_cast<qsizetype>(job.size()))) : 0;
        if (id == 0) {
            return std::unexpected(Error{ErrorCode::NotFound, "No export job \"" + std::string(job) + "\""});
        }
        if (action == "start") {
            queue->start(id);
        } else if (action == "pause") {
            queue->pause(id);
        } else if (action == "resume") {
            queue->resume(id);
        } else if (action == "cancel") {
            queue->cancel(id);
        } else if (action == "remove") {
            // Only that job, and only once it has finished (ExportQueue::remove: no effect on others).
            queue->remove(id);
            QMetaObject::invokeMethod(
                self.data(), [self, id] { if (self) self->m_jobs->removeFinished(id); }, Qt::QueuedConnection);
        } else {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "Unknown action " + std::string(action)});
        }
        return {};
    };
    host.defaultOutput = [self](const Sequence& sequence, const ex::ExportSettings& settings) {
        std::filesystem::path folder;
        if (self) {
            std::lock_guard lock(self->m_idsMutex);
            folder = self->m_sharedProjectFile.parent_path();
        }
        if (folder.empty()) {
            folder = pathFromQString(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation));
        }
        const QString base = safeFileName(QString::fromStdString(sequence.name));
        const QString ext = fromUtf8(ex::outputExtension(settings));
        std::filesystem::path path = folder / pathFromQString(base + ext);
        std::error_code ec;
        for (int n = 2; std::filesystem::exists(path, ec) && n < 1000; ++n) {
            path = folder / pathFromQString(QStringLiteral("%1 (%2)%3").arg(base).arg(n).arg(ext));
        }
        return path;
    };
    host.projectFile = m_projectFile;
    host.videoSource = m_videoSource;
    host.presets = userPresetStore();
    host.watch = m_watch;
    host.presetsChanged = [self] {
        QMetaObject::invokeMethod(self.data(), [self] { if (self) emit self->presetsChanged(); }, Qt::QueuedConnection);
    };
    host.watchFoldersChanged = [self] {
        QMetaObject::invokeMethod(self.data(), [self] {
            if (self) {
                self->saveWatchFolders();
                emit self->watchFoldersChanged();
            }
        }, Qt::QueuedConnection);
    };
    m_commandServices = std::make_shared<ex::ExportCommandServices>(std::move(host));
    return m_commandServices;
}

QString ExportController::plainError(const Error& error) {
    const QString message = QString::fromStdString(error.message);
    switch (error.code) {
    case ErrorCode::Cancelled:
        return translate("Export cancelled.");
    case ErrorCode::NotFound: {
        static const QString kOffline = QStringLiteral("Original media offline or missing: ");
        if (message.startsWith(kOffline)) {
            QString list = message.mid(kOffline.size());
            list = list.left(list.indexOf(QLatin1String(". Relink")));
            // Names only: "a.mp4 (C:/media/a.mp4), b.wav" -> "a.mp4, b.wav" (the library has the paths).
            static const QRegularExpression kPaths(QStringLiteral(R"( \([^)]*\))"));
            list.remove(kPaths);
            return translate("Some original media is offline or missing: %1. Relink it, then export again.")
                .arg(list);
        }
        return translate("Something the export needs was not found: %1").arg(message);
    }
    case ErrorCode::Unsupported:
        if (message.startsWith(QLatin1String("No working"))) {
            return translate("No encoder on this computer can write this format. Choose another codec or "
                             "preset. (%1)")
                .arg(message);
        }
        return translate("This export is not supported here: %1").arg(message);
    case ErrorCode::IoError:
        if (message.contains(QLatin1String("output folder does not exist"))) {
            return translate("The folder does not exist. Choose another location.");
        }
        if (message.contains(QLatin1String("No space"))) {
            return translate("The disk is full. Free some space or choose another drive.");
        }
        if (message.contains(QLatin1String("Permission denied"))) {
            return translate("Wabcut cannot write to that location. Choose another folder.");
        }
        return translate("The file could not be written: %1").arg(message);
    case ErrorCode::InvalidArgument:
        return message.endsWith(QLatin1Char('.')) ? message : message + QLatin1Char('.');
    case ErrorCode::EncoderError:
        return translate("The encoder stopped with an error. Try another encoder or preset. (%1)")
            .arg(message);
    case ErrorCode::DecoderError:
        return translate("A clip could not be decoded: %1").arg(message);
    case ErrorCode::DeviceError:
        return translate("The GPU renderer is not available: %1").arg(message);
    default:
        return message;
    }
}

// 0.4: follows plainError()'s branches; the failed-export toast offers Relink… for "offline".
QString ExportController::errorKindOf(const Error& error) {
    const QString message = QString::fromStdString(error.message);
    switch (error.code) {
    case ErrorCode::NotFound:
        // The prefix plainError() reads as "original media offline".
        return message.startsWith(QLatin1String("Original media offline or missing: ")) ? QStringLiteral("offline")
                                                                                           : QString();
    case ErrorCode::IoError:
        if (message.contains(QLatin1String("output folder does not exist"))) {
            return QStringLiteral("folder");
        }
        if (message.contains(QLatin1String("No space"))) {
            return QStringLiteral("space");
        }
        if (message.contains(QLatin1String("Permission denied"))) {
            return QStringLiteral("access");
        }
        return {};
    case ErrorCode::Unsupported:
        return message.startsWith(QLatin1String("No working")) ? QStringLiteral("encoder") : QString();
    case ErrorCode::EncoderError:
        return QStringLiteral("encoder");
    default:
        return {};
    }
}

} // namespace wabcut::app
