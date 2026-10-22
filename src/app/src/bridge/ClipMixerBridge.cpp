#include "ClipMixerBridge.h"

#include "BridgeIds.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/playback/audio/TimelineAudioPlayer.h>

#include <QCoreApplication>
#include <QQmlEngine>

#include <algorithm>
#include <cmath>

namespace wabcut::app {

namespace {

QString tx(const char* text) {
    return QCoreApplication::translate("wabcut::app::ClipMixerBridge", text);
}

ParamId paramOf(const QString& param) { return param == QLatin1String("pan") ? ParamId::Pan : ParamId::VolumeDb; }

double clampParam(const QString& param, double value) {
    return param == QLatin1String("pan") ? std::clamp(value, -1.0, 1.0) : std::clamp(value, -144.0, 24.0);
}

} // namespace

ClipMixerBridge::ClipMixerBridge(QObject* parent) : QObject(parent) {
    m_timer.setInterval(33);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &ClipMixerBridge::pollWrite);
}

ClipMixerBridge::~ClipMixerBridge() = default;

ClipMixerBridge* ClipMixerBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* mixer = new ClipMixerBridge;
    if (qmlEngine) {
        mixer->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        mixer->setPlayback(qmlEngine->singletonInstance<PlaybackBridge*>("Wabcut", "Playback"));
    }
    return mixer;
}

void ClipMixerBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (m_controller) {
        connect(m_controller, &ProjectController::revisionChanged, this, &ClipMixerBridge::rebuild);
        connect(m_controller, &ProjectController::sequenceChanged, this, [this] {
            if (m_playing && !m_lanes.empty()) {
                m_timer.stop();
                commitPass();
            }
            rebuild();
        });
        connect(m_controller, &ProjectController::playheadChanged, this, &ClipMixerBridge::rebuild);
    }
    rebuild();
}

void ClipMixerBridge::setPlayback(PlaybackBridge* playback) {
    if (m_playback == playback)
        return;
    if (m_playback)
        disconnect(m_playback, nullptr, this, nullptr);
    m_playback = playback;
    if (playback)
        connect(playback, &PlaybackBridge::transportChanged, this, [this] { onPlayingChanged(m_playback && m_playback->playing()); });
}

const Clip* ClipMixerBridge::clipAt(TrackId track, const Sequence& seq) const {
    const Track* t = seq.findTrack(track);
    if (!t || t->kind != TrackKind::Audio || !m_controller)
        return nullptr;
    const auto index = t->clipIndexAt(m_controller->playhead());
    return index ? &t->clips[*index] : nullptr;
}

void ClipMixerBridge::rebuild() {
    QVariantList strips;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (seq) {
        std::map<QString, QVariantMap> rows; // shortName and colour by track key
        for (const QVariant& v : m_controller->audioTracks())
            rows[v.toMap().value(QStringLiteral("trackId")).toString()] = v.toMap();
        const Ticks t = m_controller->playhead();
        for (std::size_t i = 0; i < seq->audioTracks.size(); ++i) {
            const Track& track = *seq->audioTracks[i];
            const QString key = trackKey(track.id);
            const QVariantMap row = rows[key];
            const auto index = track.clipIndexAt(t);
            const Clip* clip = index ? &track.clips[*index] : nullptr;
            const auto value = [&](const QString& param) {
                const auto live = m_live.find({key, param});
                if (live != m_live.end() && m_playing && m_writeKeyframes.contains(key))
                    return live->second;
                return clip ? clip->parameterAt(paramOf(param), t) : 0.0;
            };
            const bool writing = std::any_of(m_held.begin(), m_held.end(),
                                             [&](const auto& h) { return h.first.first == key && h.second; }) &&
                                 m_playing && m_writeKeyframes.contains(key);
            strips.append(QVariantMap{
                {QStringLiteral("key"), key},
                {QStringLiteral("shortName"), row.value(QStringLiteral("shortName"), QStringLiteral("A%1").arg(i + 1))},
                {QStringLiteral("name"), QString::fromStdString(track.name)},
                {QStringLiteral("colorIndex"), row.value(QStringLiteral("colorIndex"), 0)},
                {QStringLiteral("clip"), clip ? clipKey(clip->id) : QString()},
                {QStringLiteral("clipName"), clip ? QString::fromStdString(clip->name()) : QString()},
                {QStringLiteral("gainDb"), value(QStringLiteral("volume"))},
                {QStringLiteral("pan"), value(QStringLiteral("pan"))},
                {QStringLiteral("automated"), clip && clip->properties()[ParamId::VolumeDb].isCurve()},
                {QStringLiteral("panAutomated"), clip && clip->properties()[ParamId::Pan].isCurve()},
                {QStringLiteral("writeKeyframes"), m_writeKeyframes.contains(key)},
                {QStringLiteral("writing"), writing},
                {QStringLiteral("muted"), track.muted},
                {QStringLiteral("solo"), track.solo},
            });
        }
    }
    if (strips == m_strips)
        return;
    m_strips = strips;
    emit stripsChanged();
}

QString ClipMixerBridge::setParam(const QString& track, const QString& param, double value) {
    if (!m_controller)
        return tx("No project");
    const auto id = parseTrackKey(track);
    const SequencePtr seq = m_controller->currentSequence();
    const Clip* clip = id && seq ? clipAt(*id, *seq) : nullptr;
    if (!clip)
        return tx("No clip under the playhead on this track");
    value = clampParam(param, value);
    // Playing with Write Keyframes: the pass records it (written at stop), heard at once through the live override.
    if (m_playing && m_writeKeyframes.contains(track)) {
        m_live[{track, param}] = value;
        pollWrite();
        return {};
    }
    const char* label = param == QLatin1String("pan") ? QT_TRANSLATE_NOOP("wabcut::app::ClipMixerBridge", "Clip Pan")
                                                       : QT_TRANSLATE_NOOP("wabcut::app::ClipMixerBridge", "Clip Volume");
    const std::string merge = "clipMixer:" + track.toStdString() + ":" + param.toStdString();
    const bool ok = m_controller->setParameterAtPlayhead(clip->id, edit::ParamRef::of(paramOf(param)), value, label, merge);
    return ok ? QString() : m_controller->lastError();
}

QString ClipMixerBridge::setGainDb(const QString& track, double gainDb) { return setParam(track, QStringLiteral("volume"), gainDb); }

QString ClipMixerBridge::setPan(const QString& track, double pan) { return setParam(track, QStringLiteral("pan"), pan); }

void ClipMixerBridge::endGesture() {
    if (m_controller)
        m_controller->endPropertyGesture();
}

QString ClipMixerBridge::resetGain(const QString& track) {
    endGesture();
    const QString error = setParam(track, QStringLiteral("volume"), 0.0);
    endGesture();
    return error;
}

QString ClipMixerBridge::resetPan(const QString& track) {
    endGesture();
    const QString error = setParam(track, QStringLiteral("pan"), 0.0);
    endGesture();
    return error;
}

void ClipMixerBridge::touch(const QString& track, const QString& param, bool held) {
    m_held[{track, param}] = held;
    if (!held)
        m_live.erase({track, param}); // back to what the clip has
    pollWrite();
    rebuild();
}

void ClipMixerBridge::setWriteKeyframes(const QString& track, bool on) {
    if (on)
        m_writeKeyframes.insert(track);
    else
        m_writeKeyframes.erase(track);
    rebuild();
}

void ClipMixerBridge::setMuted(const QString& track, bool muted) {
    if (m_controller)
        m_controller->setTrackFlag(track, QStringLiteral("muted"), muted);
}

void ClipMixerBridge::setSolo(const QString& track, bool solo) {
    if (m_controller)
        m_controller->setTrackFlag(track, QStringLiteral("solo"), solo);
}

void ClipMixerBridge::onPlayingChanged(bool playing) {
    // The source view's playback writes no keyframes.
    if (playing && m_playback && m_playback->sourceMode())
        playing = false;
    if (playing == m_playing)
        return;
    m_playing = playing;
    if (playing) {
        m_lanes.clear();
        m_passSequence = m_controller ? m_controller->currentSequenceId() : SequenceId{};
        m_timer.start();
    } else {
        m_timer.stop();
        pollWrite(); // the last sample, at the stop
        commitPass();
        m_live.clear();
        pushLive();
    }
    rebuild();
}

void ClipMixerBridge::pollWrite() {
    if (!m_playing || !m_controller || m_controller->currentSequenceId() != m_passSequence) {
        pushLive();
        return;
    }
    const SequencePtr seq = m_controller->currentSequence();
    if (!seq)
        return;
    const Ticks t = m_controller->playhead();
    std::set<std::pair<std::uint64_t, QString>> sampled;
    for (auto& [lane, value] : m_live) {
        const QString& track = lane.first;
        const QString& param = lane.second;
        const auto held = m_held.find(lane);
        const auto id = parseTrackKey(track);
        if (!m_writeKeyframes.contains(track) || held == m_held.end() || !held->second || !id)
            continue;
        const Clip* clip = clipAt(*id, *seq);
        if (!clip)
            continue;
        Lane& l = m_lanes[{clip->id.value, param}];
        sampled.insert({clip->id.value, param});
        const Ticks at = clip->curveTimeAt(t); // keys of a clip are in its curve time
        if (l.open && !l.segments.empty() && at < l.segments.back().end)
            l.open = false; // a jump back (a loop): a new stretch
        if (!l.open) {
            l.segments.push_back(AutomationSegment{at, at, {}});
            l.open = true;
        }
        AutomationSegment& segment = l.segments.back();
        if (segment.samples.empty() || at > segment.samples.back().first)
            segment.samples.emplace_back(at, value);
        segment.end = std::max(segment.end, at);
    }
    // Lanes not written this tick (released, or the playhead left the clip) close their stretch.
    for (auto& [key, l] : m_lanes) {
        if (!sampled.contains(key))
            l.open = false;
    }
    pushLive();
    rebuild();
}

void ClipMixerBridge::commitPass() {
    if (!m_controller)
        return;
    std::map<std::pair<std::uint64_t, QString>, Lane> lanes = std::move(m_lanes);
    m_lanes.clear();
    std::erase_if(lanes, [](auto& entry) {
        std::erase_if(entry.second.segments, [](const AutomationSegment& s) { return s.samples.empty(); });
        return entry.second.segments.empty();
    });
    if (lanes.empty())
        return;
    const SequenceId sequence = m_passSequence;
    // Every clip the pass wrote, in one edit: one undo step.
    const bool ok = m_controller->applyEditFn(
        QT_TRANSLATE_NOOP("wabcut::app::ClipMixerBridge", "Clip Mixer Keyframes"), [&](const ProjectState& state) -> edit::EditOutcome {
            ProjectState next = state;
            for (const auto& [key, lane] : lanes) {
                const Sequence* seq = next.findSequence(sequence);
                const auto at = seq ? seq->findClip(ClipId{key.first}) : std::nullopt;
                if (!at)
                    continue;
                const ParamId id = paramOf(key.second);
                ParamValue value = applyAutomationPass(at->clip().properties()[id], lane.segments,
                                                       id == ParamId::Pan ? 0.005 : 0.05);
                auto edited = edit::setParameter(next, sequence, ClipId{key.first}, edit::ParamRef::of(id), std::move(value));
                if (!edited)
                    return std::unexpected(edited.error());
                next = std::move(edited->state);
            }
            edit::EditResult result;
            result.changes = diff(state, next);
            result.state = std::move(next);
            return result;
        });
    (void)ok;
}

void ClipMixerBridge::pushLive() {
    // While a Write Keyframes pass holds a clip's level, the track plays at its fader plus the change of the clip's level.
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    std::set<std::uint64_t> sent;
    if (seq && m_playing) {
        const Ticks t = m_controller->playhead();
        for (const auto& [lane, value] : m_live) {
            const auto id = parseTrackKey(lane.first);
            if (lane.second != QLatin1String("volume") || !id || !m_writeKeyframes.contains(lane.first))
                continue;
            const Track* track = seq->findTrack(*id);
            const Clip* clip = clipAt(*id, *seq);
            if (!track || !clip)
                continue;
            const double change = value - clip->parameterAt(ParamId::VolumeDb, t);
            const double gain = std::clamp(track->volume.valueAt(t) + change, -144.0, 24.0);
            bool applied = false;
            if (m_liveMixSink) {
                applied = m_liveMixSink(*id, gain);
            } else if (editor::PlaybackSession* session = m_playback ? m_playback->session() : nullptr) {
                if (playback::TimelineAudioPlayer* player = session->audioPlayer())
                    applied = player->setLiveTrackMix(*id, gain, std::nullopt);
            }
            if (applied)
                sent.insert(id->value);
        }
    }
    for (const std::uint64_t track : m_liveSent) {
        if (sent.contains(track))
            continue;
        if (m_liveMixSink) {
            m_liveMixSink(TrackId{track}, std::nullopt);
        } else if (editor::PlaybackSession* session = m_playback ? m_playback->session() : nullptr) {
            if (playback::TimelineAudioPlayer* player = session->audioPlayer())
                player->setLiveTrackMix(TrackId{track}, std::nullopt, std::nullopt);
        }
    }
    m_liveSent = std::move(sent);
}

} // namespace wabcut::app
