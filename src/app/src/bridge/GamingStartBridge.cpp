#include "GamingStartBridge.h"

#include "BridgeIds.h"
#include "CommandJob.h"
#include "HomeBridge.h"
#include "MediaReadinessBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/command/Command.h>
#include <wabcut/media/Probe.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Time.h>
#include <wabcut/model/Vfr.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QMetaObject>
#include <QThreadPool>

#include <cmath>
#include <exception>
#include <mutex>
#include <utility>
#include <vector>

namespace wabcut::app {

/// The worker's way back: cleared when the bridge goes, under the mutex the worker posts under.
struct GamingStartBridge::Gate {
    std::mutex mutex;
    GamingStartBridge* bridge = nullptr;
};

namespace {

constexpr double kHighFps = 60.0;          // above it: proxies suggested
constexpr int k4kLines = 2160;             // UHD and up
constexpr double kLongSeconds = 20.0 * 60; // 20 minutes and longer

const QStringList& recordingSuffixes() {
    static const QStringList suffixes{QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("mov"),
                                      QStringLiteral("m4v"), QStringLiteral("webm"), QStringLiteral("flv"),
                                      QStringLiteral("ts"),  QStringLiteral("m2ts"), QStringLiteral("avi"),
                                      QStringLiteral("wmv")};
    return suffixes;
}

QString localPath(const QUrl& file) {
    return file.isLocalFile() ? file.toLocalFile() : file.toString();
}

/// "60", "59.94", "23.976".
QString rateText(Rate rate) {
    if (!rate.isValid())
        return {};
    if (rate.den == 1)
        return QString::number(rate.num);
    QString text = QString::number(static_cast<double>(rate.num) / rate.den, 'f', 3);
    while (text.endsWith(QLatin1Char('0')))
        text.chop(1);
    if (text.endsWith(QLatin1Char('.')))
        text.chop(1);
    return text;
}

/// "42:17", "1:02:03".
QString durationText(double seconds) {
    const auto total = static_cast<qint64>(std::floor(std::max(0.0, seconds)));
    const qint64 h = total / 3600;
    const qint64 m = (total / 60) % 60;
    const qint64 s = total % 60;
    if (h > 0)
        return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

QString codecText(const std::string& codec) {
    const QString c = QString::fromStdString(codec).toLower();
    if (c == QLatin1String("h264"))
        return QStringLiteral("H.264");
    if (c == QLatin1String("hevc") || c == QLatin1String("h265"))
        return QStringLiteral("HEVC");
    if (c == QLatin1String("prores"))
        return QStringLiteral("ProRes");
    if (c == QLatin1String("mpeg4"))
        return QStringLiteral("MPEG-4");
    return c.toUpper(); // AV1, VP9, ...
}

QString layoutText(const AudioStreamInfo& audio) {
    const QString layout = QString::fromStdString(audio.channelLayout);
    if (layout == QLatin1String("mono") || (layout.isEmpty() && audio.channels == 1))
        return GamingStartBridge::tr("Mono");
    if (layout == QLatin1String("stereo") || (layout.isEmpty() && audio.channels == 2))
        return GamingStartBridge::tr("Stereo");
    if (layout.startsWith(QLatin1String("5.1")))
        return QStringLiteral("5.1");
    if (layout.startsWith(QLatin1String("7.1")))
        return QStringLiteral("7.1");
    return GamingStartBridge::tr("%n channels", "", audio.channels);
}

} // namespace

GamingStartBridge::GamingStartBridge(QObject* parent) : QObject(parent), m_gate(std::make_shared<Gate>()) {
    m_gate->bridge = this;
}

GamingStartBridge::~GamingStartBridge() {
    std::lock_guard lock(m_gate->mutex);
    m_gate->bridge = nullptr;
}

GamingStartBridge* GamingStartBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new GamingStartBridge;
    if (qmlEngine) {
        bridge->setHome(qmlEngine->singletonInstance<HomeBridge*>("Wabcut", "Home"));
        bridge->setLifecycle(qmlEngine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile"));
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        bridge->setEngine(qmlEngine); // MediaReadiness: resolved when proxies are asked for
    }
    return bridge;
}

void GamingStartBridge::setHome(HomeBridge* home) {
    m_home = home;
}

void GamingStartBridge::setLifecycle(ProjectLifecycle* lifecycle) {
    m_lifecycle = lifecycle;
}

void GamingStartBridge::setController(ProjectController* controller) {
    m_controller = controller;
}

void GamingStartBridge::setEngine(QQmlEngine* engine) {
    m_engine = engine;
}

void GamingStartBridge::setProbe(Probe probe) {
    m_probe = std::move(probe);
}

void GamingStartBridge::setProxyRequester(ProxyRequester requester) {
    m_proxyRequester = std::move(requester);
}

QStringList GamingStartBridge::nameFilters() const {
    return {tr("Game recordings (*.mp4 *.mkv *.mov *.m4v *.webm *.flv *.ts *.m2ts *.avi *.wmv)"), tr("All files (*)")};
}

QString GamingStartBridge::suggestedTrackName(int position) {
    switch (position) {
    case 0:
        return tr("Game");
    case 1:
        return tr("Mic");
    case 2:
        return tr("Discord");
    default:
        return tr("Audio %1").arg(position + 1);
    }
}

QString GamingStartBridge::suggestedTrackName(const MediaInfo& info, int position) {
    // 0.4: the names later drops of such a recording route by (edit::sourceTrackNames: the stream titles, else Game /
    // Mic for two untitled streams with video, else Track 1, Track 2, ...); kept as they are (untranslated), so the
    // tracks match the next recording.
    const std::vector<edit::SourceTrackName> names = edit::sourceTrackNames(info);
    if (position >= 0 && static_cast<std::size_t>(position) < names.size() &&
        !names[static_cast<std::size_t>(position)].name.empty())
        return QString::fromStdString(names[static_cast<std::size_t>(position)].name);
    return suggestedTrackName(position); // a single untitled stream
}

QString GamingStartBridge::trackRoleOf(const QString& name) {
    const edit::SourceTrackName named = edit::sourceTrackName(name.toStdString());
    const std::string_view role = toString(named.role);
    return QString::fromUtf8(role.data(), static_cast<qsizetype>(role.size()));
}

QVariantMap GamingStartBridge::describe(const MediaInfo& info, const QString& file) {
    const QFileInfo fileInfo(file);
    QVariantMap out{{QStringLiteral("ok"), false},
                    {QStringLiteral("error"), QString()},
                    {QStringLiteral("url"), QUrl::fromLocalFile(file)},
                    {QStringLiteral("file"), QDir::toNativeSeparators(file)},
                    {QStringLiteral("name"), fileInfo.completeBaseName()}};
    if (info.video.empty() || info.isStillImage) {
        out.insert(QStringLiteral("error"), tr("%1 has no video. Choose a game recording.").arg(fileInfo.fileName()));
        return out;
    }
    const VideoStreamInfo& video = info.video.front();
    // The picture as shown (a phone capture can be stored turned).
    const bool turned = video.rotationDegrees == 90 || video.rotationDegrees == 270;
    const int width = turned ? video.height : video.width;
    const int height = turned ? video.width : video.height;
    // The nominal rate: a variable rate capture's measured rate snapped to a standard one (C.15's nominalRate; the
    // sequence then conforms the capture by timestamp, so nothing drifts).
    // A constant rate a sequence cannot run at (not a whole number of ticks per frame) is snapped the same way, so the
    // rate shown is the one "Match my media" creates (UX rework).
    const bool rateKnown = video.frameRate.isValid();
    Rate rate = !rateKnown ? Rate{60, 1} : video.variableFrameRate ? nominalRate(video.frameRate) : video.frameRate;
    if (rateKnown && !rate.isExact()) {
        if (const Rate snapped = nominalRate(video.frameRate); snapped.isExact())
            rate = snapped;
    }
    const double fps = static_cast<double>(rate.num) / rate.den;
    const double seconds = (info.duration.value > 0 ? info.duration : video.duration).toSeconds();
    const bool highFps = fps > kHighFps + 0.5;
    const bool is4k = std::min(width, height) >= k4kLines;
    const bool isLong = seconds >= kLongSeconds;
    QString hdr;
    if (video.color.transfer == TransferFunction::PQ)
        hdr = QStringLiteral("PQ");
    else if (video.color.transfer == TransferFunction::HLG)
        hdr = QStringLiteral("HLG");

    QStringList reasons;
    if (is4k)
        reasons << tr("4K");
    if (highFps)
        reasons << tr("%1 fps").arg(rateText(rate));
    if (isLong)
        reasons << tr("%1 long").arg(durationText(seconds));
    const bool proxies = !reasons.isEmpty();

    // 0.4: each stream's name from its title (as later drops route by), its role, and where the name comes from.
    const std::vector<edit::SourceTrackName> names = edit::sourceTrackNames(info);
    const auto originText = [](edit::SourceTrackName::Origin origin) {
        switch (origin) {
        case edit::SourceTrackName::Origin::Title:
            return QStringLiteral("title");
        case edit::SourceTrackName::Origin::Recognised:
            return QStringLiteral("recognised");
        case edit::SourceTrackName::Origin::Inferred:
            return QStringLiteral("inferred");
        case edit::SourceTrackName::Origin::None:
            break;
        }
        return QStringLiteral("none");
    };
    QVariantList audio;
    for (std::size_t i = 0; i < info.audio.size(); ++i) {
        const AudioStreamInfo& a = info.audio[i];
        const QString suggested = suggestedTrackName(info, static_cast<int>(i));
        audio.append(QVariantMap{{QStringLiteral("index"), static_cast<int>(i)},
                                 {QStringLiteral("stream"), a.index},
                                 {QStringLiteral("channels"), a.channels},
                                 {QStringLiteral("layout"), layoutText(a)},
                                 {QStringLiteral("codec"), codecText(a.codec)},
                                 {QStringLiteral("suggested"), suggested},
                                 {QStringLiteral("title"), QString::fromStdString(a.title).trimmed()},
                                 {QStringLiteral("role"), trackRoleOf(suggested)},
                                 {QStringLiteral("origin"),
                                  originText(i < names.size() ? names[i].origin : edit::SourceTrackName::Origin::None)}});
    }
    // OBS often records the full mix on Track 1: with 3+ streams and stream 1 untitled or "Track 1", say so.
    bool mixHint = false;
    if (info.audio.size() >= 3) {
        const std::string& first = info.audio.front().title;
        mixHint = QString::fromStdString(first).trimmed().isEmpty() || edit::sourceTrackName(first).name == "Track 1";
    }

    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("width"), width);
    out.insert(QStringLiteral("height"), height);
    out.insert(QStringLiteral("fps"), QVariantMap{{QStringLiteral("num"), rate.num},
                                                  {QStringLiteral("den"), rate.den},
                                                  {QStringLiteral("text"), rateText(rate)}});
    out.insert(QStringLiteral("vfr"), video.variableFrameRate || !rateKnown);
    out.insert(QStringLiteral("durationSeconds"), seconds);
    out.insert(QStringLiteral("durationText"), durationText(seconds));
    out.insert(QStringLiteral("videoCodec"), codecText(video.codec));
    out.insert(QStringLiteral("hdr"), hdr);
    out.insert(QStringLiteral("sizeText"), QStringLiteral("%1 × %2").arg(width).arg(height));
    out.insert(QStringLiteral("fileSizeText"),
               info.fileSize > 0 ? QLocale().formattedDataSize(info.fileSize, 1, QLocale::DataSizeTraditionalFormat)
                                 : QString());
    out.insert(QStringLiteral("highFps"), highFps);
    out.insert(QStringLiteral("is4k"), is4k);
    out.insert(QStringLiteral("isLong"), isLong);
    out.insert(QStringLiteral("proxies"), proxies);
    out.insert(QStringLiteral("proxyReason"),
               proxies ? tr("%1: proxies keep editing smooth; the export uses the original.").arg(reasons.join(QStringLiteral(", ")))
                       : QString());
    out.insert(QStringLiteral("audio"), audio);
    out.insert(QStringLiteral("mixHint"), mixHint);
    return out;
}

bool GamingStartBridge::isRecording(const QUrl& file) const {
    const QString path = localPath(file);
    return !path.isEmpty() && recordingSuffixes().contains(QFileInfo(path).suffix().toLower());
}

void GamingStartBridge::inspect(const QUrl& file) {
    const std::uint64_t generation = ++m_generation;
    m_info.reset();
    if (!m_inspecting) {
        m_inspecting = true;
        emit inspectingChanged();
    }
    Probe probe = m_probe ? m_probe : Probe([](const std::string& path) { return media::probe(path); });
    QThreadPool::globalInstance()->start([gate = m_gate, probe = std::move(probe), path = localPath(file).toStdString(),
                                          generation, file] {
        std::expected<MediaInfo, Error> info = std::unexpected(Error{ErrorCode::Unknown, "the probe failed"});
        try { // QThreadPool does not catch: an exception would end the process
            info = probe(path);
        } catch (const std::exception& e) {
            info = std::unexpected(Error{ErrorCode::Unknown, e.what()});
        } catch (...) {
        }
        std::lock_guard lock(gate->mutex);
        if (GamingStartBridge* self = gate->bridge) {
            QMetaObject::invokeMethod(
                self, [self, generation, file, info = std::move(info)] { self->finishInspect(generation, file, info); },
                Qt::QueuedConnection);
        }
    });
}

void GamingStartBridge::finishInspect(std::uint64_t generation, const QUrl& file, const std::expected<MediaInfo, Error>& info) {
    if (generation != m_generation)
        return; // a later inspect() or clear() wins
    const QString path = localPath(file);
    if (info) {
        m_recording = describe(*info, path);
        if (m_recording.value(QStringLiteral("ok")).toBool())
            m_info = *info;
    } else {
        const QFileInfo fileInfo(path);
        m_recording = QVariantMap{{QStringLiteral("ok"), false},
                                  {QStringLiteral("error"), tr("Cannot read %1: %2")
                                                                .arg(fileInfo.fileName(),
                                                                     QString::fromStdString(info.error().message))},
                                  {QStringLiteral("url"), file},
                                  {QStringLiteral("file"), QDir::toNativeSeparators(path)},
                                  {QStringLiteral("name"), fileInfo.completeBaseName()}};
    }
    m_inspecting = false;
    emit inspectingChanged();
    emit recordingChanged();
}

void GamingStartBridge::clear() {
    ++m_generation;
    m_info.reset();
    const bool had = !m_recording.isEmpty();
    m_recording.clear();
    if (m_inspecting) {
        m_inspecting = false;
        emit inspectingChanged();
    }
    if (had)
        emit recordingChanged();
}

int GamingStartBridge::requestProxies(const QStringList& mediaKeys) {
    if (m_proxyRequester)
        return m_proxyRequester(mediaKeys);
    auto* readiness = m_engine ? m_engine->singletonInstance<MediaReadinessBridge*>("Wabcut", "MediaReadiness") : nullptr;
    return readiness ? readiness->generateProxies(mediaKeys) : 0;
}

QString GamingStartBridge::create(const QString& name, const QUrl& folder, const QVariantList& tracks) {
    if (!m_info || !m_recording.value(QStringLiteral("ok")).toBool())
        return tr("Choose a game recording first.");
    if (!m_home || !m_lifecycle || !m_controller)
        return tr("Projects are not available.");
    const QVariantMap recording = m_recording; // copies: the steps below change the project, not these
    const QString file = QDir::fromNativeSeparators(recording.value(QStringLiteral("file")).toString());

    // The plan per audio stream (checked against the recording before anything is made): every stream, included under
    // its suggested name unless `tracks` says otherwise.
    QVariantList plan;
    std::vector<bool> given(m_info->audio.size(), false);
    for (const QVariant& value : tracks) {
        const QVariantMap track = value.toMap();
        bool ok = false;
        const int index = track.value(QStringLiteral("index")).toInt(&ok);
        if (!ok || index < 0 || index >= static_cast<int>(given.size()))
            return tr("The recording has no audio stream %1.").arg(track.value(QStringLiteral("index")).toString());
        given[static_cast<std::size_t>(index)] = true;
        plan.append(track);
    }
    for (std::size_t i = 0; i < given.size(); ++i) {
        if (!given[i])
            plan.append(QVariantMap{{QStringLiteral("index"), static_cast<int>(i)},
                                    {QStringLiteral("name"), suggestedTrackName(*m_info, static_cast<int>(i))},
                                    {QStringLiteral("include"), true}});
    }

    // Home's media start does the steps (UX rework): the project at the recording's size and nominal frame rate (saved
    // and opened), the recording imported, one audio track per included stream, the picture on V1 and the included
    // streams on A1, A2, ... named, proxies when the recording asks for them, and a save. A failure after the project
    // was made comes back as its message (the project stays open).
    const QVariantMap fps = recording.value(QStringLiteral("fps")).toMap();
    const QVariantMap result = m_home->createFromMediaWith(
        name, folder,
        QVariantMap{{QStringLiteral("width"), recording.value(QStringLiteral("width"))},
                    {QStringLiteral("height"), recording.value(QStringLiteral("height"))},
                    {QStringLiteral("rateNum"), fps.value(QStringLiteral("num"))},
                    {QStringLiteral("rateDen"), fps.value(QStringLiteral("den"))}},
        {QUrl::fromLocalFile(file)},
        QVariantMap{{QStringLiteral("tracks"), plan},
                    {QStringLiteral("proxies"),
                     recording.value(QStringLiteral("proxies")).toBool() ? QStringLiteral("on") : QStringLiteral("off")}},
        [this](const QStringList& keys) { return requestProxies(keys); });
    if (!result.value(QStringLiteral("ok")).toBool())
        return result.value(QStringLiteral("error")).toString();
    emit created(result.value(QStringLiteral("project")).toUrl());
    return result.value(QStringLiteral("warning")).toString();
}

} // namespace wabcut::app
