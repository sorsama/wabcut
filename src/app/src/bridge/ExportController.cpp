#include <wabcut/ai/Provenance.h>
#include <wabcut/model/ProjectIO.h>
#include "ExportController.h"

#include "BridgeIds.h"
#include "CaptionStyleMapping.h"
#include "CaptionsBridge.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"
#include "TranscriptBridge.h"

#include <wabcut/export/Encoders.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/playback/video/Evaluate.h>

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QProcess>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>
#include <QtQml/qqml.h>

#include <algorithm>
#include <cmath>
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
    }
    return {};
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
    }
    return {};
}

std::optional<ex::Container> parseContainer(const QString& s) {
    if (s == QLatin1String("mp4")) {
        return ex::Container::Mp4;
    }
    if (s == QLatin1String("mov")) {
        return ex::Container::Mov;
    }
    if (s == QLatin1String("mkv")) {
        return ex::Container::Mkv;
    }
    if (s == QLatin1String("wav")) {
        return ex::Container::Wav;
    }
    if (s == QLatin1String("gif")) {
        return ex::Container::Gif;
    }
    if (s == QLatin1String("webm")) {
        return ex::Container::WebM;
    }
    return std::nullopt;
}

std::optional<ex::VideoCodec> parseCodec(const QString& s) {
    if (s == QLatin1String("h264")) {
        return ex::VideoCodec::H264;
    }
    if (s == QLatin1String("hevc")) {
        return ex::VideoCodec::Hevc;
    }
    if (s == QLatin1String("prores")) {
        return ex::VideoCodec::ProRes;
    }
    if (s == QLatin1String("dnxhr")) {
        return ex::VideoCodec::DnxHR;
    }
    if (s == QLatin1String("av1")) {
        return ex::VideoCodec::Av1;
    }
    if (s == QLatin1String("gif")) {
        return ex::VideoCodec::Gif;
    }
    if (s == QLatin1String("vp9")) {
        return ex::VideoCodec::Vp9;
    }
    return std::nullopt;
}

/// M5o: an output frame rate from "60", "59.94" or "60000/1001" (NTSC rates by their usual decimals); 0 = the sequence's.
std::optional<std::optional<Rate>> parseFrameRate(const QVariant& value) {
    const QString text = value.toString().trimmed();
    if (const qsizetype slash = text.indexOf(QLatin1Char('/')); slash > 0) {
        bool okNum = false;
        bool okDen = false;
        const int num = text.left(slash).toInt(&okNum);
        const int den = text.mid(slash + 1).toInt(&okDen);
        if (!okNum || !okDen || num <= 0 || den <= 0 || num / den > 1000)
            return std::nullopt;
        return std::optional<Rate>(Rate{num, den}.normalized());
    }
    bool ok = false;
    const double fps = text.toDouble(&ok);
    if (!ok || fps < 0 || fps > 1000)
        return std::nullopt;
    if (fps == 0)
        return std::optional<Rate>(); // the sequence rate
    for (const int base : {24, 30, 60, 120, 240}) { // 23.976, 29.97, 59.94, 119.88, 239.76
        const double ntsc = base * 1000.0 / 1001.0;
        if (std::abs(fps - ntsc) < 0.01)
            return std::optional<Rate>(Rate{base * 1000, 1001});
    }
    if (std::abs(fps - std::round(fps)) > 1e-6)
        return std::nullopt;
    return std::optional<Rate>(Rate{static_cast<std::int32_t>(std::lround(fps)), 1});
}

Error badOverride(const QString& key, const QVariant& value) {
    return Error{
        ErrorCode::InvalidArgument,
        QStringLiteral("Unsupported export option %1 = %2").arg(key, value.toString()).toStdString()};
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
    default:
        return {};
    }
}

QHash<int, QByteArray> ExportJobModel::roleNames() const {
    return {{JobIdRole, "jobId"},           {TitleRole, "title"},     {OutputPathRole, "outputPath"},
            {PresetNameRole, "presetName"}, {StateRole, "jobState"},  {PhaseTextRole, "phaseText"},
            {ProgressRole, "progress"},     {FpsTextRole, "fpsText"}, {EtaTextRole, "etaText"},
            {EncoderRole, "encoder"},       {DetailRole, "detail"},   {FinishedRole, "finished"}};
}

int ExportJobModel::activeCount() const {
    return static_cast<int>(
        std::count_if(m_rows.begin(), m_rows.end(), [](const Row& r) { return !r.finished; }));
}

void ExportJobModel::append(Row row) {
    const int at = rowCount();
    beginInsertRows({}, at, at);
    m_rows.push_back(std::move(row));
    endInsertRows();
    emit countChanged();
    emit activeCountChanged();
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
    const exporter::ExportProgress& p = status.progress;
    switch (p.phase) {
    case exporter::ExportPhase::Queued:
        r.state = QStringLiteral("queued");
        r.phaseText = translate("Queued");
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
    } else if (status.error) {
        r.detail = ExportController::plainError(*status.error);
    }
    const int row = static_cast<int>(it - m_rows.begin());
    emit dataChanged(index(row), index(row));
    if (wasFinished != r.finished) {
        emit activeCountChanged();
    }
    return &r;
}

const ExportJobModel::Row* ExportJobModel::find(const QString& jobId) const {
    const auto it =
        std::find_if(m_rows.begin(), m_rows.end(), [&](const Row& r) { return r.jobId == jobId; });
    return it == m_rows.end() ? nullptr : &*it;
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
    connect(m_jobs, &ExportJobModel::activeCountChanged, this, &ExportController::busyChanged);
}

ExportController::~ExportController() {
    {
        std::lock_guard lock(m_relay->mutex);
        m_relay->target = nullptr;
    }
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
    if (m_project != project) {
        m_project = project;
        emit projectChanged();
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
            {QStringLiteral("icon"), QString::fromLatin1(text ? text->icon : "film")}});
    }
    return list;
}

std::expected<ex::ExportSettings, Error> ExportController::settingsFor(const QString& presetId,
                                                                       const QVariantMap& overrides) {
    const auto preset = ex::findPreset(presetId.toStdString());
    if (!preset) {
        return std::unexpected(Error{ErrorCode::InvalidArgument,
                                     QStringLiteral("Unknown preset %1").arg(presetId).toStdString()});
    }
    ex::ExportSettings s = preset->settings;
    auto has = [&](const char* key) {
        const auto it = overrides.find(QLatin1String(key));
        return it != overrides.end() && it->isValid() && !it->isNull();
    };
    auto value = [&](const char* key) { return overrides.value(QLatin1String(key)); };

    // Container and codec first: they decide the defaults of the fields below.
    if (has("container")) {
        const auto c = parseContainer(value("container").toString());
        if (!c) {
            return std::unexpected(badOverride(QStringLiteral("container"), value("container")));
        }
        s.container = *c;
        if (*c == ex::Container::Wav) {
            s.video.enabled = false;
            if (s.audio.codec == ex::AudioCodec::Aac) {
                s.audio.codec = ex::AudioCodec::Pcm24;
            }
        } else if (*c == ex::Container::Mp4 && s.audio.codec != ex::AudioCodec::Aac) {
            s.audio.codec = ex::AudioCodec::Aac; // MP4 carries AAC
        } else if (*c == ex::Container::WebM) {
            s.audio.codec = ex::AudioCodec::Opus; // WebM carries Opus
        } else if (*c == ex::Container::Gif) {
            s.audio.enabled = false; // a GIF has no sound
        }
    }
    if (has("videoCodec")) {
        const auto c = parseCodec(value("videoCodec").toString());
        if (!c) {
            return std::unexpected(badOverride(QStringLiteral("videoCodec"), value("videoCodec")));
        }
        if (*c != s.video.codec) {
            s.video.codec = *c;
            s.video.encoder = "auto";
            s.video.pixelFormat = *c == ex::VideoCodec::ProRes  ? ex::OutputPixelFormat::Yuv422p10
                                  : *c == ex::VideoCodec::DnxHR ? ex::OutputPixelFormat::Yuv422p
                                                                : ex::OutputPixelFormat::Yuv420p;
            s.video.dnxhrProfile = ex::DnxhrProfile::Hq;
            s.video.proresProfile = ex::ProResProfile::Hq;
        }
    }
    if (has("video")) {
        s.video.enabled = value("video").toBool();
    }
    if (has("audio")) {
        s.audio.enabled = value("audio").toBool();
    }
    if (has("encoder")) {
        const QString e = value("encoder").toString();
        s.video.encoder = e == QLatin1String("software") ? std::string(ex::softwareEncoder(s.video.codec))
                                                         : e.toStdString();
    }
    if (has("quality")) {
        s.video.rateControl = ex::RateControl::Quality;
        s.video.quality = value("quality").toInt();
    }
    if (has("bitrateMbps")) {
        s.video.rateControl = ex::RateControl::Bitrate;
        s.video.bitrate = std::llround(value("bitrateMbps").toDouble() * 1e6);
        s.video.maxBitrate = s.video.bitrate * 3 / 2;
    }
    if (has("maxBitrateMbps")) { // M5o: a tight cap for size targets (Discord); 0 = 1.5 x the bitrate
        const double cap = value("maxBitrateMbps").toDouble();
        if (cap < 0)
            return std::unexpected(badOverride(QStringLiteral("maxBitrateMbps"), value("maxBitrateMbps")));
        s.video.maxBitrate = cap == 0 ? s.video.bitrate * 3 / 2 : std::llround(cap * 1e6);
    }
    if (has("frameRate")) { // M5o: the output frame rate (gaming presets cap at 60); 0 = the sequence's
        const auto rate = parseFrameRate(value("frameRate"));
        if (!rate)
            return std::unexpected(badOverride(QStringLiteral("frameRate"), value("frameRate")));
        s.video.frameRate = *rate;
    }
    if (has("width") || has("height")) {
        const int w = value("width").toInt();
        const int h = value("height").toInt();
        if (w <= 0 || h <= 0) { // 0 = the sequence size
            s.video.width.reset();
            s.video.height.reset();
        } else {
            s.video.width = w;
            s.video.height = h;
        }
    }
    if (has("scaleMode")) {
        const QString m = value("scaleMode").toString();
        if (m == QLatin1String("fit")) {
            s.video.scaleMode = ex::ScaleMode::Fit;
        } else if (m == QLatin1String("fill")) {
            s.video.scaleMode = ex::ScaleMode::Fill;
        } else if (m == QLatin1String("stretch")) {
            s.video.scaleMode = ex::ScaleMode::Stretch;
        } else {
            return std::unexpected(badOverride(QStringLiteral("scaleMode"), value("scaleMode")));
        }
    }
    if (has("range")) {
        const QString r = value("range").toString();
        if (r == QLatin1String("whole")) {
            s.range = ex::RangeMode::WholeSequence;
        } else if (r == QLatin1String("inOut")) {
            s.range = ex::RangeMode::InOut;
        } else if (r == QLatin1String("workArea")) {
            s.range = ex::RangeMode::WorkArea;
        } else {
            return std::unexpected(badOverride(QStringLiteral("range"), value("range")));
        }
    }
    if (has("captions")) { // I.4: off, burned in (the monitor's look), or a sidecar next to the file
        const QString c = value("captions").toString();
        if (c == QLatin1String("off")) {
            s.captions.mode = ex::CaptionExport::None;
        } else if (c == QLatin1String("burnIn")) {
            s.captions.mode = ex::CaptionExport::BurnIn;
        } else if (c == QLatin1String("srt")) {
            s.captions.mode = ex::CaptionExport::Srt;
        } else if (c == QLatin1String("vtt")) {
            s.captions.mode = ex::CaptionExport::Vtt;
        } else {
            return std::unexpected(badOverride(QStringLiteral("captions"), value("captions")));
        }
    }
    if (has("audioCodec")) {
        const QString a = value("audioCodec").toString();
        if (a == QLatin1String("aac")) {
            s.audio.codec = ex::AudioCodec::Aac;
        } else if (a == QLatin1String("pcm16")) {
            s.audio.codec = ex::AudioCodec::Pcm16;
        } else if (a == QLatin1String("pcm24")) {
            s.audio.codec = ex::AudioCodec::Pcm24;
        } else if (a == QLatin1String("ac3")) { // A.9: Dolby Digital (5.1); a WAV keeps its PCM
            if (s.container != ex::Container::Wav) {
                s.audio.codec = ex::AudioCodec::Ac3;
            }
        } else {
            return std::unexpected(badOverride(QStringLiteral("audioCodec"), value("audioCodec")));
        }
    }
    if (has("audioChannels")) { // A.9: 0 = as the master (5.1 for a 5.1 master when the codec takes it), 1, 2, 6
        const int channels = value("audioChannels").toInt();
        if (channels != 0 && channels != 1 && channels != 2 && channels != 6) {
            return std::unexpected(badOverride(QStringLiteral("audioChannels"), value("audioChannels")));
        }
        s.audio.channels = channels;
    }
    if (has("sampleRate")) {
        s.audio.sampleRate = value("sampleRate").toInt();
    }
    if (has("audioBitrateKbps")) {
        s.audio.bitrate = std::llround(value("audioBitrateKbps").toDouble() * 1000.0);
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
    const ex::Container container = settings ? settings->container : ex::Container::Mp4;
    const SequencePtr sequence = sequenceFor(sequenceId);
    QString folder;
    if (!m_projectFile.empty()) {
        folder = pathToQString(m_projectFile.parent_path());
    }
    if (folder.isEmpty()) {
        folder = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    }
    if (folder.isEmpty()) {
        folder = QDir::homePath();
    }
    const QString base = safeFileName(sequence ? QString::fromStdString(sequence->name) : QString());
    const QString ext = fromUtf8(ex::fileExtension(container));
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
                                       const QVariantMap& overrides) const {
    QVariantMap d;
    const SequencePtr sequence = sequenceFor(sequenceId);
    d[QStringLiteral("rangeAvailable")] =
        QVariantMap{{QStringLiteral("whole"), sequence != nullptr},
                    {QStringLiteral("inOut"), sequence && sequence->markedRange().has_value()},
                    {QStringLiteral("workArea"), sequence && sequence->workArea.has_value()}};
    auto fail = [&](const Error& e) {
        d[QStringLiteral("ok")] = false;
        d[QStringLiteral("error")] = plainError(e);
        return d;
    };
    const auto settings = settingsFor(presetId, overrides);
    if (!settings) {
        return fail(settings.error());
    }
    if (!sequence) {
        return fail(Error{ErrorCode::NotFound, "There is no sequence to export"});
    }
    const auto r = ex::resolve(*settings, *sequence);
    if (!r) {
        return fail(r.error());
    }
    const ex::ExportSettings& s = *settings;
    d[QStringLiteral("ok")] = true;
    d[QStringLiteral("container")] = fromUtf8(ex::containerName(s.container));
    d[QStringLiteral("videoCodec")] = fromUtf8(ex::videoCodecName(s.video.codec));
    if (s.video.enabled) {
        d[QStringLiteral("format")] =
            QStringLiteral("%1 · %2 · %3")
                .arg(fromUtf8(ex::containerName(s.container)).toUpper(), codecText(s.video.codec),
                     pixelFormatText(s.video.pixelFormat));
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
        d[QStringLiteral("quality")] =
            s.video.codec == ex::VideoCodec::ProRes || s.video.codec == ex::VideoCodec::DnxHR
                ? translate("Profile")
            : s.video.rateControl == ex::RateControl::Quality
                ? translate("Constant quality %1").arg(s.video.quality)
                : translate("%1 Mb/s").arg(static_cast<double>(s.video.bitrate) / 1e6, 0, 'f', 1);
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
            : translate("PCM %1-bit · %2 kHz · %3")
                  .arg(s.audio.codec == ex::AudioCodec::Pcm16 ? 16 : 24)
                  .arg(s.audio.sampleRate / 1000.0)
                  .arg(channelText);
    d[QStringLiteral("duration")] = clockText(r->duration.toSeconds());
    d[QStringLiteral("range")] = s.range == ex::RangeMode::InOut      ? translate("In to Out")
                                 : s.range == ex::RangeMode::WorkArea ? translate("Work area")
                                                                      : translate("Whole sequence");
    d[QStringLiteral("output")] =
        overrides.contains(QStringLiteral("output"))
            ? pathToQString(outputFromOverride(overrides.value(QStringLiteral("output"))))
            : defaultOutputPath(sequenceId, presetId, overrides);
    return d;
}

QString ExportController::queueExport(const QString& sequenceId, const QString& presetId,
                                      const QVariantMap& overrides) {
    if (!project()) {
        setLastError(translate("There is no project to export."));
        return {};
    }
    const SequencePtr sequence = sequenceFor(sequenceId);
    if (!sequence) {
        setLastError(translate("There is no sequence to export."));
        return {};
    }
    auto settings = settingsFor(presetId, overrides);
    if (!settings) {
        setLastError(plainError(settings.error()));
        return {};
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
    if (const auto r = ex::resolve(*settings, *sequence); !r) {
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
    output.replace_extension(std::string(ex::fileExtension(settings->container))); // the container decides

    ex::ExportRequest request;
    request.snapshot = project()->snapshot();
    request.sequence = sequence->id;
    request.settings = *settings;
    request.output = output;
    request.projectFile = m_projectFile;
    request.videoSource = m_videoSource;
    setLastError({});
    return queueRequest(std::move(request), presetName(settings->presetId));
}

QString ExportController::queueRequest(ex::ExportRequest request, const QString& presetLabel) {
    ExportJobModel::Row row;
    row.jobId = QStringLiteral("j%1").arg(m_nextJob++);
    row.title = QString::fromStdU16String(request.output.filename().u16string());
    row.outputPath = pathToQString(request.output);
    row.presetName = presetLabel;
    row.phaseText = translate("Queued");
    const QString jobId = row.jobId;
    ai::Json provenance;
    if (request.snapshot) if (const auto* seq = request.snapshot->findSequence(request.sequence)) {
        const auto resolved = ex::resolve(request.settings, *seq);
        if (resolved) provenance = ai::provenanceReport(*request.snapshot, request.sequence, resolved->range);
    }
    row.queueId = m_queue->enqueue(std::move(request));
    if (!provenance.is_null()) m_provenance[row.queueId] = std::move(provenance);
    m_jobs->append(std::move(row));
    emit jobQueued(jobId);
    return jobId;
}

std::string ExportController::provenanceSidecarText(const ai::Json& provenance) {
    // Replace invalid UTF-8 instead of throwing out of a slot (review P2-48; project strings are validated on load).
    return provenance.dump(2, ' ', false, ai::Json::error_handler_t::replace);
}

void ExportController::onStatus(const ex::ExportJobStatus& status) {
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
        if (auto it = m_provenance.find(status.id); it != m_provenance.end()) {
            if (row->state == QLatin1String("done")) {
                auto sidecar = std::filesystem::path(row->outputPath.toStdWString()); sidecar += ".provenance.json";
                if (auto wrote = io::writeFileAtomically(sidecar, provenanceSidecarText(it->second)); !wrote)
                    setLastError(tr("Export finished, but its unsigned provenance sidecar could not be written."));
            }
            m_provenance.erase(it);
        }
        emit jobFinished(row->jobId, row->state == QLatin1String("done"), row->detail);
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
}

bool ExportController::reveal(const QString& jobId) {
    const ExportJobModel::Row* row = m_jobs->find(jobId);
    if (!row) {
        return false;
    }
    if (m_reveal) {
        return m_reveal(row->outputPath);
    }
    const QFileInfo file(row->outputPath);
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

} // namespace wabcut::app
