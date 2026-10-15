#include "GamingExport.h"

#include "BridgeIds.h"
#include "ExportController.h"
#include "ProjectController.h"

#include <wabcut/export/Encoders.h>
#include <wabcut/export/ExportSettings.h>

#include <QCoreApplication>
#include <QQmlEngine>

#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>
#include <utility>

namespace wabcut::app {

namespace {

namespace ex = exporter;

constexpr const char* kContext = "wabcut::app::GamingExport";

QString tx(const char* text) {
    return QCoreApplication::translate(kContext, text);
}

struct Group {
    const char* id;
    const char* name;
    const char* icon;
};

constexpr Group kGroups[] = {
    {"youtube", QT_TRANSLATE_NOOP("wabcut::app::GamingExport", "YouTube"), "play"},
    {"vertical", QT_TRANSLATE_NOOP("wabcut::app::GamingExport", "Shorts, TikTok and Reels"), "move-vertical"},
    {"twitch", QT_TRANSLATE_NOOP("wabcut::app::GamingExport", "Twitch"), "zap"},
    {"discord", QT_TRANSLATE_NOOP("wabcut::app::GamingExport", "Discord"), "message-square"},
    {"memes", QT_TRANSLATE_NOOP("wabcut::app::GamingExport", "GIF and WebM"), "image"},
};

/// A gaming preset: a built-in base preset and the overrides it sets. A size target (Discord) computes size and
/// bitrate from the range's length instead.
struct Plan {
    const char* id;
    const char* group;
    const char* name;
    const char* description;
    const char* icon;
    const char* base; ///< exporter::builtInPresets() id
    int width;        ///< 0: from the size target
    int height;
    double videoMbps;
    double maxMbps;
    int audioKbps;
    int fpsCap;               ///< the output never runs faster than this (and never faster than the sequence)
    std::int64_t targetBytes; ///< > 0: a file size target (decimal bytes, as upload limits are stated)
    const char* unavailable;  ///< why it cannot be used yet, or nullptr
};

constexpr Plan kPlans[] = {
    {"youtube-1080p60", "youtube", QT_TRANSLATE_NOOP("wabcut::app::GamingExport", "YouTube 1080p60"),
     QT_TRANSLATE_NOOP("wabcut::app::GamingExport", "H.264 · 1920×1080 · up to 60 fps · 18 Mb/s"), "play", "youtube-1080p",
     1920, 1080, 18, 27, 384, 60, 0, nullptr},
    {"youtube-1440p60", "youtube", QT_TRANSLATE_NOOP("wabcut::app::GamingExport", "YouTube 1440p60"),
     QT_TRANSLATE_NOOP("wabcut::app::GamingExport", "H.264 · 2560×1440 · up to 60 fps · 30 Mb/s"), "play", "youtube-1080p",
     2560, 1440, 30, 45, 384, 60, 0, nullptr},
    {"youtube-4k60", "youtube", QT_TRANSLATE_NOOP("wabcut::app::GamingExport", "YouTube 4K60"),
     QT_TRANSLATE_NOOP("wabcut::app::GamingExport", "H.264 · 3840×2160 · up to 60 fps · 68 Mb/s"), "scaling", "youtube-4k",
     3840, 2160, 68, 85, 384, 60, 0, nullptr},
    {"vertical-1080x1920-60", "vertical", QT_TRANSLATE_NOOP("wabcut::app::GamingExport", "Shorts / TikTok / Reels"),
     QT_TRANSLATE_NOOP("wabcut::app::GamingExport", "H.264 · 1080×1920 · up to 60 fps · 15 Mb/s"), "move-vertical",
     "vertical-1080x1920", 1080, 1920, 15, 22, 320, 60, 0, nullptr},
    {"twitch-1080p60", "twitch", QT_TRANSLATE_NOOP("wabcut::app::GamingExport", "Twitch upload"),
     QT_TRANSLATE_NOOP("wabcut::app::GamingExport", "H.264 · 1920×1080 · up to 60 fps · 8 Mb/s"), "zap", "youtube-1080p",
     1920, 1080, 8, 10, 160, 60, 0, nullptr},
    {"discord-10mb", "discord", QT_TRANSLATE_NOOP("wabcut::app::GamingExport", "Discord 10 MB"),
     QT_TRANSLATE_NOOP("wabcut::app::GamingExport", "H.264 · sized to fit 10 MB"), "message-square", "youtube-1080p", 0, 0, 0,
     0, 128, 60, 10'000'000, nullptr},
    {"discord-25mb", "discord", QT_TRANSLATE_NOOP("wabcut::app::GamingExport", "Discord 25 MB"),
     QT_TRANSLATE_NOOP("wabcut::app::GamingExport", "H.264 · sized to fit 25 MB"), "message-square", "youtube-1080p", 0, 0, 0,
     0, 128, 60, 25'000'000, nullptr},
    {"discord-50mb", "discord", QT_TRANSLATE_NOOP("wabcut::app::GamingExport", "Discord 50 MB"),
     QT_TRANSLATE_NOOP("wabcut::app::GamingExport", "H.264 · sized to fit 50 MB"), "message-square", "youtube-1080p", 0, 0, 0,
     0, 128, 60, 50'000'000, nullptr},
    {"gif", "memes", QT_TRANSLATE_NOOP("wabcut::app::GamingExport", "GIF"),
     QT_TRANSLATE_NOOP("wabcut::app::GamingExport", "An animated GIF for memes and chats"), "image", "youtube-1080p", 0, 0, 0,
     0, 0, 30, 0, QT_TRANSLATE_NOOP("wabcut::app::GamingExport", "Coming soon: the exporter has no GIF output yet")},
    {"webm", "memes", QT_TRANSLATE_NOOP("wabcut::app::GamingExport", "WebM"),
     QT_TRANSLATE_NOOP("wabcut::app::GamingExport", "VP9 or AV1 WebM for memes and the web"), "film", "youtube-1080p", 0, 0, 0,
     0, 0, 60, 0, QT_TRANSLATE_NOOP("wabcut::app::GamingExport", "Coming soon: the exporter has no WebM output yet")},
};

/// I.9: the exporter's own GIF and WebM presets; the cards use them once the exporter has them.
const char* exporterPresetFor(const Plan& p) {
    if (std::string_view(p.id) == "gif")
        return "gif-480p15";
    if (std::string_view(p.id) == "webm")
        return "webm-vp9-720p";
    return nullptr;
}

bool exporterHas(const char* id) {
    if (!id)
        return false;
    static const std::vector<ex::Preset> presets = ex::builtInPresets();
    return std::any_of(presets.begin(), presets.end(), [id](const ex::Preset& p) { return p.id == id; });
}

/// Usable now: no "coming soon", or the exporter has the preset that replaces it.
bool usable(const Plan& p) { return p.unavailable == nullptr || exporterHas(exporterPresetFor(p)); }

const Plan* findPlan(const QString& id) {
    for (const Plan& p : kPlans) {
        if (id == QLatin1String(p.id))
            return &p;
    }
    return nullptr;
}

/// The share of a size target the picture and sound may use: room for the container and encoder overshoot.
constexpr double kTargetShare = 0.95;
/// MP4 and its index, on top of the streams.
constexpr double kContainerOverhead = 1.01;

bool isHardware(const std::string& encoder) {
    for (const char* tag : {"nvenc", "_amf", "_qsv", "videotoolbox", "vaapi"}) {
        if (encoder.find(tag) != std::string::npos)
            return true;
    }
    return false;
}

QString clockText(double seconds) {
    const auto total = static_cast<long long>(std::max(0.0, std::floor(seconds)));
    const long long h = total / 3600;
    const long long m = (total / 60) % 60;
    const long long s = total % 60;
    return h > 0 ? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'))
                 : QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

} // namespace

GamingExport::GamingExport(QObject* parent) : QObject(parent) {}

GamingExport* GamingExport::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new GamingExport;
    if (qmlEngine) {
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        bridge->setExport(qmlEngine->singletonInstance<ExportController*>("Wabcut", "ExportController"));
    }
    return bridge;
}

ProjectController* GamingExport::controller() const {
    return m_controller;
}

ExportController* GamingExport::exporter() const {
    return m_export;
}

QVariantList GamingExport::groups() const {
    QVariantList out;
    for (const Group& g : kGroups) {
        out.append(QVariantMap{{QStringLiteral("id"), QString::fromLatin1(g.id)},
                               {QStringLiteral("name"), tx(g.name)},
                               {QStringLiteral("icon"), QString::fromLatin1(g.icon)}});
    }
    return out;
}

QVariantList GamingExport::presets() const {
    QVariantList out;
    for (const Plan& p : kPlans) {
        out.append(QVariantMap{{QStringLiteral("id"), QString::fromLatin1(p.id)},
                               {QStringLiteral("group"), QString::fromLatin1(p.group)},
                               {QStringLiteral("name"), tx(p.name)},
                               {QStringLiteral("description"), tx(p.description)},
                               {QStringLiteral("icon"), QString::fromLatin1(p.icon)},
                               {QStringLiteral("available"), usable(p)},
                               {QStringLiteral("reason"), usable(p) ? QString() : tx(p.unavailable)},
                               {QStringLiteral("targetBytes"), static_cast<qlonglong>(p.targetBytes)}});
    }
    return out;
}

std::int64_t GamingExport::targetVideoBitrate(std::int64_t targetBytes, double seconds, std::int64_t audioBitrate) {
    if (targetBytes <= 0 || seconds <= 0)
        return 0;
    // Every byte counted with the container's overhead, so the estimate stays under the target.
    const double streamBits = static_cast<double>(targetBytes) * 8.0 * kTargetShare / kContainerOverhead;
    const double video = streamBits / seconds - static_cast<double>(audioBitrate);
    return video <= 0 ? 0 : static_cast<std::int64_t>(std::floor(video));
}

double GamingExport::encodeSeconds(const std::string& codec, const std::string& encoder, int width, int height,
                                   std::int64_t frames) {
    if (frames <= 0)
        return 0.0;
    // Frames per second of the whole export (render + encode) at 1920x1080 and 3840x2160, measured by P.1's export
    // benchmark on the reference machine (tests/perf/baselines). AMF and Quick Sync are not measured: 80 % of NVENC.
    const bool hardware = isHardware(encoder);
    double fps1080 = 40.6;
    double fps2160 = 10.5;
    if (codec == "h264") {
        fps1080 = hardware ? 83.2 : 40.6;
        fps2160 = hardware ? 25.6 : 10.5;
    } else if (codec == "hevc") {
        fps1080 = hardware ? 80.8 : 14.4;
        fps2160 = hardware ? 24.4 : 3.6;
    } else if (codec == "prores" || codec == "dnxhr") {
        fps1080 = 60.0;
        fps2160 = 15.0;
    }
    if (hardware && encoder.find("nvenc") == std::string::npos) {
        fps1080 *= 0.8;
        fps2160 *= 0.8;
    }
    const double ratio = std::max(1.0, static_cast<double>(width) * height) / (1920.0 * 1080.0);
    const double exponent = std::log(fps1080 / fps2160) / std::log(4.0);
    const double fps = fps1080 * std::pow(ratio, -exponent);
    // A margin for a busy machine and effects heavier than the benchmark's, plus the start.
    return 2.0 + static_cast<double>(frames) / fps * 1.3;
}

QString GamingExport::sizeText(double bytes) {
    if (bytes < 1e6)
        return tr("%1 KB").arg(std::max(1.0, std::round(bytes / 1e3)));
    if (bytes < 1e8) {
        const double mb = bytes / 1e6;
        if (std::abs(mb - std::round(mb)) < 1e-9) // a stated limit: "10 MB"
            return tr("%1 MB").arg(std::lround(mb));
        return tr("%1 MB").arg(mb, 0, 'f', 1);
    }
    if (bytes < 1e9)
        return tr("%1 MB").arg(std::round(bytes / 1e6));
    return tr("%1 GB").arg(bytes / 1e9, 0, 'f', 2);
}

QString GamingExport::encodeText(double seconds) {
    const auto total = static_cast<long long>(std::ceil(std::max(1.0, seconds)));
    if (total < 60)
        return tr("about %1 s").arg(total);
    if (total < 3600)
        return tr("about %1 min %2 s").arg(total / 60).arg(total % 60);
    return tr("about %1 h %2 min").arg(total / 3600).arg((total / 60) % 60);
}

QString GamingExport::encoderFor(const QString& codec, const std::string& configured) const {
    if (configured != "auto")
        return QString::fromStdString(configured);
    // The encoder "auto" picked (ExportController.probeEncoders); unknown until probed.
    if (ExportController* e = exporter())
        return e->autoEncoders().value(codec).toString();
    return {};
}

QVariantMap GamingExport::plan(const QString& id, const QString& sequenceId, const QString& range) const {
    QVariantMap out{{QStringLiteral("id"), id}, {QStringLiteral("ok"), false}};
    const Plan* p = findPlan(id);
    if (!p) {
        out[QStringLiteral("error")] = tr("Unknown preset %1").arg(id);
        return out;
    }
    out[QStringLiteral("presetId")] = QString::fromLatin1(p->base);
    out[QStringLiteral("available")] = usable(*p);
    out[QStringLiteral("reason")] = usable(*p) ? QString() : tx(p->unavailable);
    if (p->unavailable && usable(*p)) {
        // I.9: the exporter's GIF / WebM preset as it is (its size, rate and codec), for the chosen range.
        QVariantMap overrides;
        if (!range.isEmpty() && range != QLatin1String("whole"))
            overrides[QStringLiteral("range")] = range;
        out[QStringLiteral("presetId")] = QString::fromLatin1(exporterPresetFor(*p));
        out[QStringLiteral("ok")] = true;
        out[QStringLiteral("overrides")] = overrides;
        return out;
    }
    if (p->unavailable) {
        out[QStringLiteral("error")] = tx(p->unavailable);
        return out;
    }
    ProjectController* c = controller();
    const ProjectSnapshot snapshot = c ? c->snapshot() : nullptr;
    SequencePtr sequence;
    if (snapshot) {
        const auto key = sequenceId.isEmpty() ? std::optional<SequenceId>(c->currentSequenceId()) : parseSequenceKey(sequenceId);
        sequence = key ? snapshot->sequencePtr(*key) : nullptr;
    }
    if (!sequence) {
        out[QStringLiteral("error")] = tr("There is no sequence to export");
        return out;
    }

    QVariantMap overrides;
    QStringList notes;
    if (!range.isEmpty() && range != QLatin1String("whole"))
        overrides[QStringLiteral("range")] = range;
    overrides[QStringLiteral("audioBitrateKbps")] = p->audioKbps;
    int fpsCap = p->fpsCap;
    const bool portrait = sequence->height > sequence->width;

    if (p->targetBytes > 0) {
        // 0.1.2: the exporter's own size target (its discord-* preset, RateControl::TargetSize): the video gets what the
        // sound and the container leave over the range, and a file that still ends over the size is encoded once more
        // at the rate that fits (ExportResult::bytes / attempts). The range's length picks the picture size and rate.
        const QString base = QString::fromLatin1(p->id);
        out[QStringLiteral("presetId")] = base;
        overrides.remove(QStringLiteral("audioBitrateKbps")); // the preset's own sound
        auto settings = ExportController::settingsFor(base, overrides);
        if (!settings) {
            out[QStringLiteral("error")] = ExportController::plainError(settings.error());
            return out;
        }
        auto resolved = ex::resolve(*settings, *sequence);
        if (resolved && resolved->targetVideoBitrate < 1'000'000 && settings->audio.bitrate > 96'000) {
            // A long range: the sound gives up some of its share.
            overrides[QStringLiteral("audioBitrateKbps")] = 96;
            settings = ExportController::settingsFor(base, overrides);
            if (settings)
                resolved = ex::resolve(*settings, *sequence);
        }
        if (!settings) {
            out[QStringLiteral("error")] = ExportController::plainError(settings.error());
            return out;
        }
        if (!resolved) {
            if (resolved.error().message.find("fits at most") == std::string::npos) {
                out[QStringLiteral("error")] = ExportController::plainError(resolved.error());
                return out;
            }
            const double longest = static_cast<double>(p->targetBytes) * 8.0 * (1.0 - ex::kTargetSizeMargin) /
                                   static_cast<double>(ex::kTargetSizeMinVideoBitrate + settings->audio.bitrate);
            out[QStringLiteral("error")] = tr("Too long for %1: about %2 fits. Set In and Out around the moment, or pick a "
                                              "bigger size.")
                                               .arg(sizeText(static_cast<double>(p->targetBytes)), clockText(longest));
            return out;
        }
        const std::int64_t video = resolved->targetVideoBitrate;
        // The picture the bitrate can carry: 1080p from 5 Mb/s, 720p from 2.5 Mb/s, else 480p.
        int w = video >= 5'000'000 ? 1920 : video >= 2'500'000 ? 1280 : 854;
        int h = video >= 5'000'000 ? 1080 : video >= 2'500'000 ? 720 : 480;
        if (portrait)
            std::swap(w, h);
        if (video < 1'500'000)
            fpsCap = std::min(fpsCap, 30);
        overrides[QStringLiteral("width")] = w;
        overrides[QStringLiteral("height")] = h;
        notes << tr("Stays under %1: if the first pass ends over, it is encoded once more at the rate that fits (two-pass).")
                     .arg(sizeText(static_cast<double>(p->targetBytes)));
    } else {
        overrides[QStringLiteral("width")] = p->width;
        overrides[QStringLiteral("height")] = p->height;
        overrides[QStringLiteral("bitrateMbps")] = p->videoMbps;
        overrides[QStringLiteral("maxBitrateMbps")] = p->maxMbps;
        if (QLatin1String(p->group) == QLatin1String("vertical")) {
            if (!portrait)
                notes << tr("This %1×%2 sequence is centre-cropped to 9:16, so its sides are cut. A vertical layout frames "
                            "the action.")
                             .arg(sequence->width)
                             .arg(sequence->height);
        } else if (p->width > sequence->width || p->height > sequence->height) {
            notes << tr("Upscaled from %1×%2: the platform gives higher resolutions more bitrate, but no detail is added.")
                         .arg(sequence->width)
                         .arg(sequence->height);
        }
    }
    // Frame rates are capped, never raised.
    const double sequenceFps = sequence->frameRate.toDouble();
    if (sequenceFps > fpsCap + 0.01) {
        overrides[QStringLiteral("frameRate")] = fpsCap;
        notes << tr("A %1 fps sequence exports at %2 fps.").arg(std::round(sequenceFps * 100) / 100).arg(fpsCap);
    }
    out[QStringLiteral("ok")] = true;
    out[QStringLiteral("overrides")] = overrides;
    out[QStringLiteral("notes")] = notes;
    return out;
}

QVariantMap GamingExport::estimate(const QString& sequenceId, const QString& presetId, const QVariantMap& overrides) const {
    QVariantMap out{{QStringLiteral("ok"), false}};
    const auto settings = ExportController::settingsFor(presetId, overrides);
    if (!settings) {
        out[QStringLiteral("error")] = ExportController::plainError(settings.error());
        return out;
    }
    ProjectController* c = controller();
    const ProjectSnapshot snapshot = c ? c->snapshot() : nullptr;
    SequencePtr sequence;
    if (snapshot) {
        const auto key = sequenceId.isEmpty() ? std::optional<SequenceId>(c->currentSequenceId()) : parseSequenceKey(sequenceId);
        sequence = key ? snapshot->sequencePtr(*key) : nullptr;
    }
    if (!sequence) {
        out[QStringLiteral("error")] = tr("There is no sequence to export");
        return out;
    }
    const auto r = ex::resolve(*settings, *sequence);
    if (!r) {
        out[QStringLiteral("error")] = ExportController::plainError(r.error());
        return out;
    }
    const ex::ExportSettings& s = *settings;
    const double seconds = r->duration.toSeconds();
    const double fps = r->frameRate.toDouble();
    bool approximate = false;
    QStringList notes;

    double videoBps = 0;
    if (s.video.enabled) {
        const double pixelRate = static_cast<double>(r->width) * r->height * fps;
        switch (s.video.codec) {
        case ex::VideoCodec::ProRes: {
            const double scale = s.video.proresProfile == ex::ProResProfile::Proxy ? 0.2
                                 : s.video.proresProfile == ex::ProResProfile::Lt  ? 0.46
                                 : s.video.proresProfile == ex::ProResProfile::Standard ? 0.67
                                                                                        : 1.0;
            videoBps = 220e6 * scale * pixelRate / (1920.0 * 1080.0 * 29.97); // ProRes 422 HQ: ~220 Mb/s at 1080p30
            break;
        }
        case ex::VideoCodec::DnxHR: {
            const double scale = s.video.dnxhrProfile == ex::DnxhrProfile::Lb ? 0.2
                                 : s.video.dnxhrProfile == ex::DnxhrProfile::Sq ? 0.65
                                                                                 : 1.0;
            videoBps = 180e6 * scale * pixelRate / (1920.0 * 1080.0 * 29.97); // DNxHR HQ: ~180 Mb/s at 1080p30
            break;
        }
        case ex::VideoCodec::H264:
        case ex::VideoCodec::Hevc:
            if (s.video.rateControl == ex::RateControl::TargetSize) {
                videoBps = static_cast<double>(r->targetVideoBitrate); // the exporter's size target (Discord)
            } else if (s.video.rateControl == ex::RateControl::Bitrate) {
                videoBps = static_cast<double>(s.video.bitrate);
            } else {
                // Constant quality: bits per pixel of fast game footage at CRF 18, doubling every 6 steps lower.
                const double bpp = 0.11 * std::pow(2.0, (18.0 - s.video.quality) / 6.0) *
                                   (s.video.codec == ex::VideoCodec::Hevc ? 0.65 : 1.0);
                videoBps = bpp * pixelRate;
                approximate = true;
                notes << tr("Constant quality: the size depends on the picture.");
            }
            break;
        }
    }
    double audioBps = 0;
    if (s.audio.enabled) {
        const int channels = ex::exportAudioChannels(s.audio, sequence->masterFormat);
        switch (s.audio.codec) {
        case ex::AudioCodec::Aac:
        case ex::AudioCodec::Ac3:
            audioBps = static_cast<double>(s.audio.bitrate);
            break;
        case ex::AudioCodec::Pcm16:
            audioBps = static_cast<double>(s.audio.sampleRate) * channels * 16;
            break;
        case ex::AudioCodec::Pcm24:
            audioBps = static_cast<double>(s.audio.sampleRate) * channels * 24;
            break;
        }
    }
    const double bytes = (videoBps + audioBps) * seconds / 8.0 * kContainerOverhead;

    const std::string_view codecName = ex::videoCodecName(s.video.codec);
    const QString codec = QString::fromUtf8(codecName.data(), static_cast<qsizetype>(codecName.size()));
    QString encoder = s.video.enabled ? encoderFor(codec, s.video.encoder) : QString();
    // Not probed yet: time the software encoder (the slower case).
    const std::string timed = encoder.isEmpty() ? std::string(ex::softwareEncoder(s.video.codec)) : encoder.toStdString();
    const double encodeSecs = s.video.enabled ? encodeSeconds(codec.toStdString(), timed, r->width, r->height, r->frameCount)
                                              : 1.0 + seconds * 0.02;
    QString encoderText;
    if (!s.video.enabled) {
        encoderText = tr("Audio only");
    } else if (encoder.isEmpty()) {
        encoderText = tr("Auto · hardware when available");
    } else if (encoder.contains(QLatin1String("nvenc"))) {
        encoderText = tr("NVENC (NVIDIA GPU)");
    } else if (encoder.contains(QLatin1String("_amf"))) {
        encoderText = tr("AMF (AMD GPU)");
    } else if (encoder.contains(QLatin1String("_qsv"))) {
        encoderText = tr("Quick Sync (Intel GPU)");
    } else if (encoder == QLatin1String("libx264")) {
        encoderText = tr("x264 (CPU)");
    } else if (encoder == QLatin1String("libx265")) {
        encoderText = tr("x265 (CPU)");
    } else {
        encoderText = encoder;
    }

    out[QStringLiteral("ok")] = true;
    out[QStringLiteral("bytes")] = bytes;
    out[QStringLiteral("sizeText")] = sizeText(bytes);
    out[QStringLiteral("seconds")] = seconds;
    out[QStringLiteral("durationText")] = clockText(seconds);
    out[QStringLiteral("encodeSeconds")] = encodeSecs;
    out[QStringLiteral("encodeText")] = encodeText(encodeSecs);
    out[QStringLiteral("encoder")] = encoder;
    out[QStringLiteral("encoderText")] = encoderText;
    out[QStringLiteral("hardware")] = isHardware(timed);
    out[QStringLiteral("videoMbps")] = videoBps / 1e6;
    out[QStringLiteral("audioKbps")] = audioBps / 1e3;
    out[QStringLiteral("width")] = s.video.enabled ? r->width : 0;
    out[QStringLiteral("height")] = s.video.enabled ? r->height : 0;
    out[QStringLiteral("fpsText")] = s.video.enabled ? QString::number(std::round(fps * 100) / 100) : QString();
    out[QStringLiteral("approximate")] = approximate;
    out[QStringLiteral("notes")] = notes;
    return out;
}

QVariantMap GamingExport::estimateFor(const QString& id, const QString& sequenceId, const QString& range) const {
    const QVariantMap p = plan(id, sequenceId, range);
    if (!p.value(QStringLiteral("ok")).toBool())
        return p;
    QVariantMap e = estimate(sequenceId, p.value(QStringLiteral("presetId")).toString(),
                             p.value(QStringLiteral("overrides")).toMap());
    QStringList notes = p.value(QStringLiteral("notes")).toStringList();
    notes << e.value(QStringLiteral("notes")).toStringList();
    e[QStringLiteral("notes")] = notes;
    e[QStringLiteral("id")] = id;
    e[QStringLiteral("presetId")] = p.value(QStringLiteral("presetId"));
    e[QStringLiteral("overrides")] = p.value(QStringLiteral("overrides"));
    e[QStringLiteral("approximate")] = e.value(QStringLiteral("approximate")).toBool() ||
                                       findPlan(id)->targetBytes > 0;
    return e;
}

} // namespace wabcut::app
