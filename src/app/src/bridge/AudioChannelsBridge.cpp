#include "AudioChannelsBridge.h"

#include "AiController.h"
#include "AppAudioServices.h"
#include "BridgeIds.h"
#include "ProjectController.h"
#include "TranscriptBridge.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/model/AudioChannelOps.h>
#include <wabcut/model/ProjectOps.h>

#include <QCoreApplication>
#include <QQmlEngine>

#include <algorithm>
#include <cmath>
#include <expected>

namespace wabcut::app {

using command::Json;

namespace {

std::optional<MediaId> parseMediaKey(const QString& text) { return detail::parseId<MediaId>(QLatin1Char('m'), text); }

QString formatName(ChannelFormat format) { return QString::fromLatin1(toString(format)); }

/// The source channel count of a clip's stream (2 when it was not probed), as audio.channels counts it.
int sourceChannelsOf(const ProjectState& state, const Clip& clip) {
    const MediaItem* media = state.findMedia(clip.media);
    if (media && clip.stream.index >= 0 && static_cast<std::size_t>(clip.stream.index) < media->info.audio.size()) {
        const int n = media->info.audio[static_cast<std::size_t>(clip.stream.index)].channels;
        if (n > 0)
            return n;
    }
    return 2;
}

int mediaSourceChannels(const MediaItem& item) {
    return !item.info.audio.empty() && item.info.audio.front().channels > 0 ? item.info.audio.front().channels : 2;
}

/// What a clip plays: its map, its media's, or the stream as it is.
AudioChannelMap playedMap(const ProjectState& state, const Clip& clip) {
    if (const AudioChannelMap* map = edit::effectiveChannelMap(clip, state.findMedia(clip.media)))
        return *map;
    return AudioChannelMap::identity(sourceChannelsOf(state, clip));
}

QVariantMap routeVariant(const ChannelRoute& route) {
    QVariantList sources;
    for (const std::int32_t s : route.sources)
        sources.append(s + 1);
    QVariantMap out{{QStringLiteral("sources"), sources}, {QStringLiteral("muted"), route.muted}};
    if (route.gainDb.isConstant())
        out.insert(QStringLiteral("gainDb"), route.gainDb.constant());
    else
        out.insert(QStringLiteral("gainAnimated"), true);
    return out;
}

QVariantList routesVariant(const AudioChannelMap& map) {
    QVariantList out;
    for (const ChannelRoute& r : map.channels)
        out.append(routeVariant(r));
    return out;
}

/// A map from the dialog: { format, channels: [{ sources: [1-based], muted }] }.
std::expected<AudioChannelMap, QString> mapFromVariant(const QVariantMap& v) {
    const auto format = channelFormatFromString(v.value(QStringLiteral("format")).toString().toStdString());
    if (!format)
        return std::unexpected(QCoreApplication::translate("wabcut::app::AudioChannelsBridge", "Choose a clip format"));
    AudioChannelMap map;
    map.format = *format;
    for (const QVariant& c : v.value(QStringLiteral("channels")).toList()) {
        const QVariantMap ch = c.toMap();
        ChannelRoute route;
        for (const QVariant& s : ch.value(QStringLiteral("sources")).toList()) {
            const int source = s.toInt();
            if (source >= 1 && source <= kMaxSourceChannels
                && std::find(route.sources.begin(), route.sources.end(), source - 1) == route.sources.end())
                route.sources.push_back(source - 1);
        }
        std::sort(route.sources.begin(), route.sources.end());
        route.muted = ch.value(QStringLiteral("muted")).toBool();
        map.channels.push_back(std::move(route));
    }
    if (const auto problem = channelMapProblem(map))
        return std::unexpected(QString::fromStdString(*problem));
    return map;
}

double clampDb(double db) { return std::clamp(std::isfinite(db) ? db : -144.0, -144.0, 24.0); }

} // namespace

QString channelBadge(const ProjectState& state, const Clip& clip) {
    if (clip.stream.kind != StreamKind::Audio || clip.isNested())
        return {};
    const AudioChannelMap* map = edit::effectiveChannelMap(clip, state.findMedia(clip.media));
    if (!map)
        return {};
    const auto& ch = map->channels;
    if (std::any_of(ch.begin(), ch.end(), [](const ChannelRoute& r) { return r.muted; }))
        return QStringLiteral("MUTED");
    const auto only = [&](std::size_t i, std::int32_t source) {
        return i < ch.size() && ch[i].sources.size() == 1 && ch[i].sources.front() == source;
    };
    switch (map->format) {
    case ChannelFormat::Mono:
        if (ch.size() == 1 && ch[0].sources.size() > 1)
            return QStringLiteral("SUM");
        if (ch.size() == 1 && ch[0].sources.size() == 1)
            return ch[0].sources.front() == 0 ? QStringLiteral("MONO") : QStringLiteral("CH %1").arg(ch[0].sources.front() + 1);
        return QStringLiteral("MAP");
    case ChannelFormat::Stereo:
        if (only(0, 1) && only(1, 1))
            return QStringLiteral("FILL L"); // Fill Left with Right: both sides play the right channel
        if (only(0, 0) && only(1, 0))
            return QStringLiteral("FILL R");
        if (only(0, 1) && only(1, 0))
            return QStringLiteral("SWAP");
        if (only(0, 0) && only(1, 1)) {
            const bool gains = std::any_of(ch.begin(), ch.end(), [](const ChannelRoute& r) { return !(r.gainDb == ParamValue()); });
            return gains ? QStringLiteral("CH VOL") : QString();
        }
        return QStringLiteral("MAP");
    case ChannelFormat::Surround51:
        return QStringLiteral("5.1");
    case ChannelFormat::Adaptive:
        break;
    }
    return QStringLiteral("MAP");
}

AudioChannelsBridge::AudioChannelsBridge(QObject* parent) : QObject(parent) {}

AudioChannelsBridge* AudioChannelsBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new AudioChannelsBridge;
    if (qmlEngine) {
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        QPointer<AiController> ai = qmlEngine->singletonInstance<AiController*>("Wabcut", "AiController");
        QPointer<TranscriptBridge> transcripts = qmlEngine->singletonInstance<TranscriptBridge*>("Wabcut", "Transcript");
        QPointer<ProjectController> controller = bridge->m_controller;
        bridge->setServiceFactory([ai, transcripts, controller] {
            AppAudioServiceOptions options;
            options.ai = ai.data();
            options.transcripts = transcripts.data();
            if (controller)
                options.projectFile = controller->projectFile();
            return appAudioServices(options);
        });
    }
    return bridge;
}

void AudioChannelsBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (m_controller) {
        const auto all = [this] {
            rebuildTracks();
            rebuildVolume();
        };
        connect(m_controller, &ProjectController::revisionChanged, this, all);
        connect(m_controller, &ProjectController::sequenceChanged, this, all);
        connect(m_controller, &ProjectController::selectionChanged, this, &AudioChannelsBridge::rebuildVolume);
        connect(m_controller, &ProjectController::selectedClipChanged, this, &AudioChannelsBridge::rebuildVolume);
        connect(m_controller, &ProjectController::playheadChanged, this, &AudioChannelsBridge::rebuildVolume);
    }
    rebuildTracks();
    rebuildVolume();
}

std::shared_ptr<command::CommandServices> AudioChannelsBridge::services() {
    if (!m_services && m_factory)
        m_services = m_factory();
    return m_services;
}

void AudioChannelsBridge::setMessage(const QString& message) {
    if (m_message == message)
        return;
    m_message = message;
    emit messageChanged();
}

QString AudioChannelsBridge::run(const std::string& command, Json params) {
    if (!m_controller)
        return tr("No project");
    const command::CommandResult result = CommandJob::runNow(m_controller, nullptr, command, std::move(params));
    const QString error = commandErrorText(result);
    setMessage(error);
    return error;
}

QString AudioChannelsBridge::runProject(const std::string& command, Json params) {
    if (!m_controller)
        return tr("No project");
    // Not CommandJob::runNow: it adds the current "sequence", which a project command's schema refuses.
    command::CommandEngine engine(m_controller->session(), ai::edit::editingCommands(), command::EnginePolicy{});
    command::CommandRequest request;
    request.command = command;
    request.params = std::move(params);
    const QString error = commandErrorText(engine.execute(request));
    setMessage(error);
    return error;
}

std::vector<ClipId> AudioChannelsBridge::audioClipIds(const QStringList& clipKeys) const {
    std::vector<ClipId> out;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq)
        return out;
    const auto add = [&](ClipId id) {
        if (std::find(out.begin(), out.end(), id) == out.end())
            out.push_back(id);
    };
    for (const QString& key : clipKeys) {
        const auto id = parseClipKey(key);
        const auto at = id ? seq->findClip(*id) : std::nullopt;
        if (!at)
            continue;
        const Clip& clip = at->clip();
        if (clip.stream.kind == StreamKind::Audio) {
            add(clip.id);
        } else if (clip.linkGroup.isValid()) {
            for (const TrackPtr& t : seq->audioTracks) {
                for (const Clip& other : t->clips) {
                    if (other.linkGroup == clip.linkGroup)
                        add(other.id);
                }
            }
        }
    }
    return out;
}

QStringList AudioChannelsBridge::audioClips(const QStringList& clipKeys) const {
    QStringList out;
    for (const ClipId id : audioClipIds(clipKeys))
        out << clipKey(id);
    return out;
}

void AudioChannelsBridge::openDialog(const QStringList& clipKeys) {
    const QStringList clips = audioClips(clipKeys);
    emit dialogRequested(QStringLiteral("clips"), clips.isEmpty() ? clipKeys : clips);
}

// ---- Reading ------------------------------------------------------------------------------------------------------

QStringList AudioChannelsBridge::channelLabels(const QString& format, int count) const {
    const auto f = channelFormatFromString(format.toStdString());
    if (!f)
        return {};
    switch (*f) {
    case ChannelFormat::Mono:
        return {tr("Mono")};
    case ChannelFormat::Stereo:
        //: Audio channel labels: left, right
        return {tr("L"), tr("R")};
    case ChannelFormat::Surround51:
        //: 5.1 audio channel labels: left, right, centre, low-frequency effects, left surround, right surround
        return {tr("L"), tr("R"), tr("C"), tr("LFE"), tr("Ls"), tr("Rs")};
    case ChannelFormat::Adaptive:
        break;
    }
    QStringList out;
    for (int i = 1; i <= std::max(1, count); ++i)
        out << QString::number(i);
    return out;
}

QVariantMap AudioChannelsBridge::info(const QStringList& clipKeys) const {
    QVariantMap out{{QStringLiteral("ok"), false}};
    if (!m_controller)
        return out;
    Json clips = Json::array();
    for (const QString& key : clipKeys) {
        if (const auto id = parseClipKey(key))
            clips.push_back(id->value);
    }
    if (clips.empty()) {
        out.insert(QStringLiteral("error"), tr("Select an audio clip"));
        return out;
    }
    const command::CommandResult result = CommandJob::runNow(m_controller.data(), nullptr, "audio.channels", Json{{"clips", clips}});
    if (!result.ok()) {
        out.insert(QStringLiteral("error"), commandErrorText(result));
        return out;
    }
    const Json& list = result.data["clips"];
    if (!list.is_array() || list.empty()) {
        out.insert(QStringLiteral("error"), tr("Select an audio clip"));
        return out;
    }
    const Json& first = list.front();
    const int sourceChannels = first.value("sourceChannels", 2);
    const Json& map = first["map"];
    QVariantList channels;
    for (const Json& ch : map["channels"]) {
        QVariantList sources;
        for (const Json& s : ch["sources"])
            sources.append(s.get<int>());
        QVariantMap route{{QStringLiteral("sources"), sources}, {QStringLiteral("muted"), ch.value("muted", false)}};
        if (ch.contains("gainDb")) {
            if (ch["gainDb"].is_number())
                route.insert(QStringLiteral("gainDb"), ch["gainDb"].get<double>());
            else
                route.insert(QStringLiteral("gainAnimated"), true);
        }
        channels.append(route);
    }
    const QString format = QString::fromStdString(map.value("format", std::string("stereo")));
    out = {{QStringLiteral("ok"), true},
           {QStringLiteral("clips"), static_cast<int>(list.size())},
           {QStringLiteral("sourceChannels"), sourceChannels},
           {QStringLiteral("mapFrom"), QString::fromStdString(first.value("mapFrom", std::string("none")))},
           {QStringLiteral("format"), format},
           {QStringLiteral("channels"), channels},
           {QStringLiteral("sourceLabels"), channelLabels(formatName(formatForChannels(sourceChannels)), sourceChannels)}};
    return out;
}

QVariantMap AudioChannelsBridge::mediaInfo(const QStringList& mediaKeys) const {
    QVariantMap out{{QStringLiteral("ok"), false}};
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const auto id = mediaKeys.isEmpty() ? std::nullopt : parseMediaKey(mediaKeys.front());
    const MediaItem* item = snap && id ? snap->findMedia(*id) : nullptr;
    if (!item || item->info.audio.empty()) {
        out.insert(QStringLiteral("error"), tr("Choose a media item with sound"));
        return out;
    }
    const int sourceChannels = mediaSourceChannels(*item);
    const AudioChannelMap map = item->channels ? *item->channels : AudioChannelMap::identity(sourceChannels);
    out = {{QStringLiteral("ok"), true},
           {QStringLiteral("clips"), static_cast<int>(mediaKeys.size())},
           {QStringLiteral("sourceChannels"), sourceChannels},
           {QStringLiteral("mapFrom"), item->channels ? QStringLiteral("media") : QStringLiteral("none")},
           {QStringLiteral("format"), formatName(map.format)},
           {QStringLiteral("channels"), routesVariant(map)},
           {QStringLiteral("sourceLabels"), channelLabels(formatName(formatForChannels(sourceChannels)), sourceChannels)}};
    return out;
}

void AudioChannelsBridge::rebuildTracks() {
    QVariantMap formats;
    QVariantMap surround;
    QString master;
    if (const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr) {
        for (const TrackPtr& t : seq->audioTracks) {
            const QString key = trackKey(t->id);
            formats.insert(key, formatName(t->format));
            surround.insert(key, QVariantMap{{QStringLiteral("x"), t->surround.x.constant()},
                                             {QStringLiteral("y"), t->surround.y.constant()},
                                             {QStringLiteral("lfeDb"), t->surround.lfeDb.constant()}});
        }
        master = formatName(seq->masterFormat);
    }
    if (formats == m_trackFormats && surround == m_surround && master == m_masterFormat)
        return;
    m_trackFormats = std::move(formats);
    m_surround = std::move(surround);
    m_masterFormat = master;
    emit tracksChanged();
}

void AudioChannelsBridge::rebuildVolume() {
    QVariantMap next{{QStringLiteral("available"), false}};
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const auto target = snap && seq ? m_controller->propertyTarget(StreamKind::Audio) : std::nullopt;
    const auto loc = target ? seq->findClip(*target) : std::nullopt;
    if (loc && loc->clip().stream.kind == StreamKind::Audio && !loc->clip().isNested()) {
        const Clip& clip = loc->clip();
        const AudioChannelMap map = playedMap(*snap, clip);
        const Ticks at = m_controller->curveTimeAtPlayhead(clip);
        const QStringList labels = channelLabels(formatName(map.format), map.channelCount());
        QVariantList channels;
        for (int i = 0; i < map.channelCount(); ++i) {
            const ChannelRoute& r = map.channels[static_cast<std::size_t>(i)];
            const Curve* curve = r.gainDb.curve();
            channels.append(QVariantMap{{QStringLiteral("channel"), i + 1},
                                        {QStringLiteral("label"), i < labels.size() ? labels[i] : QString::number(i + 1)},
                                        {QStringLiteral("db"), r.gainDb.valueAt(at)},
                                        {QStringLiteral("animated"), curve != nullptr},
                                        {QStringLiteral("keyed"), curve != nullptr && curve->keyAt(at).has_value()},
                                        {QStringLiteral("muted"), r.muted}});
        }
        next = {{QStringLiteral("available"), true},
                {QStringLiteral("clip"), clipKey(clip.id)},
                {QStringLiteral("name"), QString::fromStdString(clip.name())},
                {QStringLiteral("format"), formatName(map.format)},
                {QStringLiteral("channels"), channels}};
    }
    if (next == m_volume)
        return;
    m_volume = std::move(next);
    emit volumeChanged();
}

// ---- Modify > Audio Channels --------------------------------------------------------------------------------------

QString AudioChannelsBridge::applyMapEdit(const QStringList& clipKeys, const QVariantMap& v) {
    if (!m_controller)
        return tr("No project");
    const auto parsed = mapFromVariant(v);
    if (!parsed) {
        setMessage(parsed.error());
        return parsed.error();
    }
    // Video clips stand for their linked audio (setClipChannels refuses a video clip).
    const std::vector<ClipId> clips = audioClipIds(clipKeys);
    if (clips.empty())
        return tr("Select an audio clip");
    const SequenceId sequence = m_controller->currentSequenceId();
    const AudioChannelMap wanted = *parsed;
    const bool ok = m_controller->applyEditFn("Modify Audio Channels", [&](const ProjectState& state) -> edit::EditOutcome {
        ProjectState current = state;
        edit::EditOutcome last = std::unexpected(Error(ErrorCode::InvalidArgument, "no clip"));
        for (const ClipId id : clips) {
            const Sequence* seq = current.findSequence(sequence);
            const auto at = seq ? seq->findClip(id) : std::nullopt;
            if (!at)
                return std::unexpected(Error(ErrorCode::NotFound, "no clip " + std::to_string(id.value)));
            // Each clip keeps its channels' own volumes (Channel Volume, keyframes included).
            AudioChannelMap map = wanted;
            const AudioChannelMap before = playedMap(current, at->clip());
            for (std::size_t i = 0; i < map.channels.size() && i < before.channels.size(); ++i)
                map.channels[i].gainDb = before.channels[i].gainDb;
            const ClipId one[] = {id};
            last = edit::setClipChannels(current, sequence, one, std::move(map));
            if (!last)
                return last;
            current = last->state;
        }
        return last;
    });
    const QString error = ok ? QString() : m_controller->lastError();
    setMessage(error);
    return error;
}

QString AudioChannelsBridge::applyMap(const QStringList& clipKeys, const QVariantMap& map) { return applyMapEdit(clipKeys, map); }

QString AudioChannelsBridge::applyPreset(const QStringList& clipKeys, const QString& preset, int channel) {
    Json clips = Json::array();
    for (const QString& key : clipKeys) {
        if (const auto id = parseClipKey(key))
            clips.push_back(id->value);
    }
    if (clips.empty())
        return tr("Select an audio clip");
    Json params{{"clips", clips}, {"preset", preset.toStdString()}};
    if (preset == QLatin1String("mono"))
        params["channel"] = std::max(1, channel);
    return run("audio.setChannels", std::move(params));
}

QString AudioChannelsBridge::applyMediaMap(const QStringList& mediaKeys, const QVariantMap& v) {
    const auto parsed = mapFromVariant(v);
    if (!parsed) {
        setMessage(parsed.error());
        return parsed.error();
    }
    Json media = Json::array();
    for (const QString& key : mediaKeys) {
        if (const auto id = parseMediaKey(key))
            media.push_back(id->value);
    }
    if (media.empty())
        return tr("Choose a media item with sound");
    Json channels = Json::array();
    for (const ChannelRoute& r : parsed->channels) {
        Json sources = Json::array();
        for (const std::int32_t s : r.sources)
            sources.push_back(s + 1);
        channels.push_back(Json{{"sources", std::move(sources)}, {"muted", r.muted}});
    }
    return runProject("audio.setMediaChannels",
                      Json{{"media", media}, {"map", Json{{"format", std::string(toString(parsed->format))}, {"channels", channels}}}});
}

QString AudioChannelsBridge::applyMediaPreset(const QStringList& mediaKeys, const QString& preset, int channel) {
    Json media = Json::array();
    for (const QString& key : mediaKeys) {
        if (const auto id = parseMediaKey(key))
            media.push_back(id->value);
    }
    if (media.empty())
        return tr("Choose a media item with sound");
    Json params{{"media", media}, {"preset", preset.toStdString()}};
    if (preset == QLatin1String("mono"))
        params["channel"] = std::max(1, channel);
    return runProject("audio.setMediaChannels", std::move(params));
}

QString AudioChannelsBridge::fill(const QStringList& clipKeys, const QString& from) {
    Json clips = Json::array();
    for (const QString& key : clipKeys) {
        if (const auto id = parseClipKey(key))
            clips.push_back(id->value);
    }
    if (clips.empty())
        return tr("Select an audio clip");
    return run("audio.fillChannel", Json{{"clips", clips}, {"from", from == QLatin1String("left") ? "left" : "right"}});
}

QString AudioChannelsBridge::swap(const QStringList& clipKeys) { return applyPreset(clipKeys, QStringLiteral("swap")); }

QString AudioChannelsBridge::splitToMono(const QString& key) {
    const auto id = parseClipKey(key);
    if (!id)
        return tr("Select an audio clip");
    return run("audio.splitToMono", Json{{"clip", id->value}});
}

// ---- Channel Volume -----------------------------------------------------------------------------------------------

namespace {

/// Edits one channel's volume of the clip in a copy of what it plays (a media map becomes the clip's own).
template <class Change>
edit::EditOutcome editChannel(const ProjectState& state, SequenceId sequence, ClipId id, int channel, Change&& change) {
    const Sequence* seq = state.findSequence(sequence);
    const auto at = seq ? seq->findClip(id) : std::nullopt;
    if (!at)
        return std::unexpected(Error(ErrorCode::NotFound, "no clip " + std::to_string(id.value)));
    AudioChannelMap map = playedMap(state, at->clip());
    if (channel < 1 || channel > map.channelCount())
        return std::unexpected(Error(ErrorCode::InvalidArgument, "the clip has no channel " + std::to_string(channel)));
    change(map.channels[static_cast<std::size_t>(channel - 1)].gainDb);
    const ClipId one[] = {id};
    return edit::setClipChannels(state, sequence, one, std::move(map));
}

} // namespace

QString AudioChannelsBridge::setChannelVolume(int channel, double db) {
    const auto id = parseClipKey(m_volume.value(QStringLiteral("clip")).toString());
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const auto loc = id && seq ? seq->findClip(*id) : std::nullopt;
    if (!loc)
        return tr("Select an audio clip");
    const Ticks at = m_controller->curveTimeAtPlayhead(loc->clip());
    const double value = clampDb(db);
    const SequenceId sequence = seq->id;
    const ClipId clip = *id;
    const std::string merge = "channelVolume:" + std::to_string(clip.value) + ":" + std::to_string(channel);
    const bool ok = m_controller->applyEditFn(
        "Channel Volume",
        [&](const ProjectState& s) {
            return editChannel(s, sequence, clip, channel, [&](ParamValue& gain) {
                if (const Curve* curve = gain.curve()) {
                    Curve next = *curve;
                    if (const auto key = next.keyAt(at)) {
                        next.keys[*key].value = value;
                    } else {
                        Keyframe k;
                        k.time = at;
                        k.value = value;
                        next.keys.insert(next.keys.begin() + static_cast<std::ptrdiff_t>(next.insertionIndex(at)), k);
                    }
                    gain = ParamValue(std::move(next));
                } else {
                    gain = ParamValue(value);
                }
            });
        },
        merge);
    const QString error = ok ? QString() : m_controller->lastError();
    setMessage(error);
    return error;
}

void AudioChannelsBridge::endChannelVolume() {
    if (m_controller)
        m_controller->endPropertyGesture();
}

QString AudioChannelsBridge::toggleChannelKey(int channel) {
    const auto id = parseClipKey(m_volume.value(QStringLiteral("clip")).toString());
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const auto loc = id && seq ? seq->findClip(*id) : std::nullopt;
    if (!loc)
        return tr("Select an audio clip");
    const Ticks at = m_controller->curveTimeAtPlayhead(loc->clip());
    const SequenceId sequence = seq->id;
    const ClipId clip = *id;
    const bool ok = m_controller->applyEditFn("Channel Volume Keyframe", [&](const ProjectState& s) {
        return editChannel(s, sequence, clip, channel, [&](ParamValue& gain) {
            const double value = gain.valueAt(at);
            Curve next = gain.curve() ? *gain.curve() : Curve{};
            if (const auto key = next.keyAt(at)) {
                next.keys.erase(next.keys.begin() + static_cast<std::ptrdiff_t>(*key));
                gain = next.keys.empty() ? ParamValue(value) : ParamValue(std::move(next));
                return;
            }
            Keyframe k;
            k.time = at;
            k.value = value;
            next.keys.insert(next.keys.begin() + static_cast<std::ptrdiff_t>(next.insertionIndex(at)), k);
            gain = ParamValue(std::move(next));
        });
    });
    const QString error = ok ? QString() : m_controller->lastError();
    setMessage(error);
    return error;
}

QString AudioChannelsBridge::setChannelAnimated(int channel, bool animated) {
    const auto id = parseClipKey(m_volume.value(QStringLiteral("clip")).toString());
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const auto loc = id && seq ? seq->findClip(*id) : std::nullopt;
    if (!loc)
        return tr("Select an audio clip");
    const Ticks at = m_controller->curveTimeAtPlayhead(loc->clip());
    const SequenceId sequence = seq->id;
    const ClipId clip = *id;
    const bool ok = m_controller->applyEditFn(animated ? "Animate Channel Volume" : "Channel Volume", [&](const ProjectState& s) {
        return editChannel(s, sequence, clip, channel, [&](ParamValue& gain) {
            const double value = gain.valueAt(at);
            if (animated && gain.isConstant()) {
                Keyframe k;
                k.time = at;
                k.value = value;
                gain = ParamValue(Curve{{k}});
            } else if (!animated && gain.isCurve()) {
                gain = ParamValue(value);
            }
        });
    });
    const QString error = ok ? QString() : m_controller->lastError();
    setMessage(error);
    return error;
}

// ---- Tracks, master, 5.1 ------------------------------------------------------------------------------------------

QString AudioChannelsBridge::setTrackFormat(const QString& key, const QString& format) {
    const auto id = parseTrackKey(key);
    if (!id)
        return tr("Choose an audio track");
    return run("track.setFormat", Json{{"track", id->value}, {"format", format.toStdString()}});
}

QString AudioChannelsBridge::setMasterFormat(const QString& format) {
    return run("sequence.setMasterFormat", Json{{"format", format.toStdString()}});
}

QString AudioChannelsBridge::setSurroundPan(const QString& key, double x, double y, double lfeDb) {
    const auto id = parseTrackKey(key);
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!id || !seq || !seq->findTrack(*id))
        return tr("Choose an audio track");
    edit::TrackStateChange change;
    SurroundPan pan;
    pan.x = ParamValue(std::clamp(std::isfinite(x) ? x : 0.0, -1.0, 1.0));
    pan.y = ParamValue(std::clamp(std::isfinite(y) ? y : 1.0, -1.0, 1.0));
    pan.lfeDb = ParamValue(std::clamp(std::isfinite(lfeDb) ? lfeDb : -144.0, -144.0, 0.0));
    change.surround = pan;
    const SequenceId sequence = seq->id;
    const TrackId track = *id;
    const std::string merge = "surround:" + std::to_string(track.value);
    const bool ok = m_controller->applyEditFn(
        "Track 5.1 Panner", [&](const ProjectState& s) { return edit::setTrackState(s, sequence, track, change); }, merge);
    const QString error = ok ? QString() : m_controller->lastError();
    setMessage(error);
    return error;
}

void AudioChannelsBridge::endSurroundPan() {
    if (m_controller)
        m_controller->endPropertyGesture();
}

// ---- Measuring ----------------------------------------------------------------------------------------------------

QString AudioChannelsBridge::measure(const QStringList& clipKeys) {
    if (!m_controller)
        return tr("No project");
    if (m_busy || m_job.running())
        return tr("Still measuring");
    Json clips = Json::array();
    for (const QString& key : clipKeys) {
        if (const auto id = parseClipKey(key))
            clips.push_back(id->value);
    }
    if (clips.empty())
        return tr("Select an audio clip");
    m_busy = true;
    emit busyChanged();
    setMessage({});
    QPointer<AudioChannelsBridge> self(this);
    const bool started = m_job.start(
        this, m_controller, services(), {"audio.channelLevels", Json{{"clips", clips}}, "Measure Channel Levels"}, {},
        [self](const command::CommandResult& result) {
            if (!self)
                return;
            self->m_busy = false;
            emit self->busyChanged();
            if (!result.ok()) {
                self->setMessage(commandErrorText(result));
                return;
            }
            QVariantList rows;
            for (const Json& c : result.data["clips"]) {
                QVariantList channels;
                for (const Json& ch : c["channels"]) {
                    const auto level = [](const Json& v) { return v.is_number() ? v.get<double>() : -144.0; };
                    channels.append(QVariantMap{{QStringLiteral("channel"), ch.value("channel", 0)},
                                                {QStringLiteral("peakDb"), level(ch["peakDb"])},
                                                {QStringLiteral("rmsDb"), level(ch["rmsDb"])}});
                }
                QVariantList silent;
                for (const Json& s : c["silentChannels"])
                    silent.append(s.get<int>());
                rows.append(QVariantMap{{QStringLiteral("clip"), clipKey(ClipId{c.value("clip", std::uint64_t{0})})},
                                        {QStringLiteral("channels"), channels},
                                        {QStringLiteral("silent"), silent},
                                        {QStringLiteral("seconds"), c.value("measuredSeconds", 0.0)}});
            }
            self->m_levels = {{QStringLiteral("clips"), rows}};
            emit self->levelsChanged();
        });
    if (!started) {
        m_busy = false;
        emit busyChanged();
        return tr("Still measuring");
    }
    return {};
}

void AudioChannelsBridge::cancel() {
    if (m_job.running())
        m_job.cancel();
}

} // namespace wabcut::app
