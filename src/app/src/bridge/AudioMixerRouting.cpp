// p02 audio: the Audio Track Mixer's routing (AudioMixerBridge.h): submix strips, track output assignment and sends,
// as Premiere's mixer has them (a send's level and pan, Pre-Fader / Post-Fader, mute; "Create ... Submix" in the output
// and send menus). Every change is a command or one edit: one undo step (a level drag merges until endSendGesture()).

#include "AudioMixerBridge.h"

#include "BridgeIds.h"
#include "ProjectController.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/SubmixOps.h>

#include <QCoreApplication>

namespace wabcut::app {

using command::Json;

QVariantList mixerEffectsOf(const EffectStack& stack); // AudioMixerBridge.cpp

namespace {

QString tx(const char* text) {
    return QCoreApplication::translate("wabcut::app::AudioMixerBridge", text);
}

QString submixName(const Sequence& seq, TrackId id) {
    const auto index = seq.submixIndex(id);
    const Track* t = seq.findSubmix(id);
    if (!index || !t)
        return {};
    return t->name.empty() ? tx("Submix %1").arg(*index + 1) : QString::fromStdString(t->name);
}

QString formatName(ChannelFormat format) {
    switch (format) {
    case ChannelFormat::Mono: return tx("Mono");
    case ChannelFormat::Stereo: return tx("Stereo");
    case ChannelFormat::Surround51: return tx("5.1");
    case ChannelFormat::Adaptive: return tx("Adaptive");
    }
    return {};
}

std::optional<ChannelFormat> newSubmixFormat(const QString& choice) {
    if (!choice.startsWith(QLatin1String("new:")))
        return std::nullopt;
    return channelFormatFromString(choice.mid(4).toStdString());
}

/// Two model edits as one: `second` runs on what `first` made (with the submix it created).
edit::EditOutcome withNewSubmix(const ProjectState& state, SequenceId sequence, ChannelFormat format,
                                const std::function<edit::EditOutcome(const ProjectState&, TrackId)>& then) {
    auto made = edit::addSubmixTrack(state, sequence, format);
    if (!made)
        return made;
    if (made->createdTracks.empty())
        return std::unexpected(Error(ErrorCode::InvalidArgument, "no submix was made"));
    auto done = then(made->state, made->createdTracks.front());
    if (!done)
        return done;
    edit::EditResult result;
    result.changes = diff(state, done->state);
    result.state = std::move(done->state);
    result.createdTracks = made->createdTracks;
    return result;
}

} // namespace

void AudioMixerBridge::addRouting(QVariantMap& strip, const Track& track, const Sequence& seq) const {
    strip.insert(QStringLiteral("output"), track.output.isValid() ? trackKey(track.output) : QStringLiteral("mix"));
    strip.insert(QStringLiteral("outputName"), track.output.isValid() ? submixName(seq, track.output) : tx("Mix"));
    QVariantList sends;
    const Ticks t = m_controller ? m_controller->playhead() : Ticks{};
    for (std::size_t i = 0; i < track.sends.size(); ++i) {
        const TrackSend& s = track.sends[i];
        sends.append(QVariantMap{{QStringLiteral("index"), static_cast<int>(i + 1)},
                                 {QStringLiteral("submix"), trackKey(s.submix)},
                                 {QStringLiteral("submixName"), submixName(seq, s.submix)},
                                 {QStringLiteral("preFader"), s.preFader},
                                 {QStringLiteral("muted"), s.muted},
                                 {QStringLiteral("levelDb"), s.level.valueAt(t)},
                                 {QStringLiteral("pan"), s.pan.valueAt(t)},
                                 {QStringLiteral("automated"), s.level.isCurve() || s.pan.isCurve()}});
    }
    strip.insert(QStringLiteral("sends"), sends);
}

QVariantList AudioMixerBridge::buildSubmixes(const Sequence& seq) const {
    QVariantList out;
    const Ticks t = m_controller ? m_controller->playhead() : Ticks{};
    for (std::size_t k = 0; k < seq.submixTracks.size(); ++k) {
        const Track& s = *seq.submixTracks[k];
        const QString key = trackKey(s.id);
        QVariantMap strip{{QStringLiteral("key"), key},
                          {QStringLiteral("name"), submixName(seq, s.id)},
                          {QStringLiteral("shortName"), QStringLiteral("S%1").arg(k + 1)},
                          {QStringLiteral("format"), QString::fromLatin1(toString(s.format).data(),
                                                                         static_cast<qsizetype>(toString(s.format).size()))},
                          {QStringLiteral("formatName"), formatName(s.format)},
                          {QStringLiteral("muted"), s.muted},
                          {QStringLiteral("solo"), s.solo},
                          {QStringLiteral("gainDb"), s.volume.valueAt(t)},
                          {QStringLiteral("pan"), s.pan.valueAt(t)},
                          {QStringLiteral("automated"), s.volume.isCurve() || s.pan.isCurve()},
                          {QStringLiteral("automation"), automationMode(key)},
                          {QStringLiteral("meter"), -2 - static_cast<int>(k)},
                          {QStringLiteral("effects"), mixerEffectsOf(s.effects)}};
        addRouting(strip, s, seq);
        out.append(strip);
    }
    return out;
}

QVariantList AudioMixerBridge::buildOutputs(const Sequence& seq) const {
    QVariantList out{QVariantMap{{QStringLiteral("id"), QStringLiteral("mix")}, {QStringLiteral("name"), tx("Mix")}}};
    for (const TrackPtr& s : seq.submixTracks)
        out.append(QVariantMap{{QStringLiteral("id"), trackKey(s->id)}, {QStringLiteral("name"), submixName(seq, s->id)}});
    return out;
}

QVariantList AudioMixerBridge::submixFormats() const {
    return {QVariantMap{{QStringLiteral("id"), QStringLiteral("new:mono")}, {QStringLiteral("name"), tx("Create Mono Submix")}},
            QVariantMap{{QStringLiteral("id"), QStringLiteral("new:stereo")}, {QStringLiteral("name"), tx("Create Stereo Submix")}},
            QVariantMap{{QStringLiteral("id"), QStringLiteral("new:5.1")}, {QStringLiteral("name"), tx("Create 5.1 Submix")}},
            QVariantMap{{QStringLiteral("id"), QStringLiteral("new:adaptive")}, {QStringLiteral("name"), tx("Create Adaptive Submix")}}};
}

QString AudioMixerBridge::addSubmix(const QString& format) {
    const auto parsed = channelFormatFromString(format.toStdString());
    if (!parsed)
        return tx("Unknown submix format");
    const command::CommandResult r = run("sequence.addSubmixTrack", Json{{"format", format.toStdString()}});
    return r.ok() ? QString() : QString::fromStdString(r.error->message);
}

QString AudioMixerBridge::removeSubmix(const QString& submix) {
    const auto id = parseTrackKey(submix);
    if (!id)
        return tx("Choose a submix");
    const command::CommandResult r = run("sequence.removeTrack", Json{{"track", id->value}});
    return r.ok() ? QString() : QString::fromStdString(r.error->message);
}

QString AudioMixerBridge::setOutput(const QString& target, const QString& output) {
    const auto track = parseTrackKey(target);
    if (!track || !m_controller)
        return tx("Choose an audio track or a submix");
    if (const auto format = newSubmixFormat(output)) {
        const SequenceId sequence = m_controller->currentSequenceId();
        const TrackId id = *track;
        const bool ok = m_controller->applyEditFn(
            QT_TRANSLATE_NOOP("wabcut::app::AudioMixerBridge", "Assign to New Submix"), [&](const ProjectState& state) {
                return withNewSubmix(state, sequence, *format, [&](const ProjectState& s, TrackId submix) {
                    return edit::setTrackOutput(s, sequence, id, submix);
                });
            });
        return ok ? QString() : m_controller->lastError();
    }
    Json to = "mix";
    if (output != QLatin1String("mix")) {
        const auto submix = parseTrackKey(output);
        if (!submix)
            return tx("Choose the Mix or a submix");
        to = submix->value;
    }
    const command::CommandResult r = run("track.setOutput", Json{{"track", track->value}, {"output", to}});
    return r.ok() ? QString() : QString::fromStdString(r.error->message);
}

QString AudioMixerBridge::addSend(const QString& track, const QString& submix) {
    const auto id = parseTrackKey(track);
    if (!id || !m_controller)
        return tx("Choose an audio track");
    if (const auto format = newSubmixFormat(submix)) {
        const SequenceId sequence = m_controller->currentSequenceId();
        const TrackId from = *id;
        const bool ok = m_controller->applyEditFn(
            QT_TRANSLATE_NOOP("wabcut::app::AudioMixerBridge", "Send to New Submix"), [&](const ProjectState& state) {
                return withNewSubmix(state, sequence, *format, [&](const ProjectState& s, TrackId bus) {
                    TrackSend send;
                    send.submix = bus;
                    return edit::addTrackSend(s, sequence, from, send);
                });
            });
        return ok ? QString() : m_controller->lastError();
    }
    const auto to = parseTrackKey(submix);
    if (!to)
        return tx("Choose a submix");
    const command::CommandResult r = run("track.addSend", Json{{"track", id->value}, {"submix", to->value}});
    return r.ok() ? QString() : QString::fromStdString(r.error->message);
}

QString AudioMixerBridge::editSend(const QString& track, int send, const char* label, const std::string& merge,
                                   const std::function<void(TrackSend&)>& change) {
    const auto id = parseTrackKey(track);
    if (!id || !m_controller || send < 1)
        return tx("No such send");
    const SequenceId sequence = m_controller->currentSequenceId();
    const TrackId trackId = *id;
    const auto index = static_cast<std::size_t>(send - 1);
    const bool ok = m_controller->applyEditFn(
        label,
        [&](const ProjectState& state) -> edit::EditOutcome {
            const Sequence* seq = state.findSequence(sequence);
            const Track* t = seq ? seq->findTrack(trackId) : nullptr;
            if (!t || index >= t->sends.size())
                return std::unexpected(Error(ErrorCode::NotFound, "the send is gone"));
            TrackSend next = t->sends[index];
            change(next);
            return edit::setTrackSend(state, sequence, trackId, index, std::move(next));
        },
        merge);
    return ok ? QString() : m_controller->lastError();
}

QString AudioMixerBridge::setSendLevel(const QString& track, int send, double db) {
    const double value = std::clamp(db, -144.0, 24.0);
    const Ticks at = m_controller ? m_controller->playhead() : Ticks{};
    return editSend(track, send, QT_TRANSLATE_NOOP("wabcut::app::AudioMixerBridge", "Send Volume"),
                    "send:" + track.toStdString() + ":" + std::to_string(send) + ":level", [&](TrackSend& s) {
                        if (const Curve* curve = s.level.curve()) {
                            Curve c = *curve; // automated: the key at the playhead, as the faders do
                            if (const auto key = c.keyAt(at))
                                c.keys[*key].value = value;
                            else
                                c.keys.insert(c.keys.begin() + static_cast<std::ptrdiff_t>(c.insertionIndex(at)), Keyframe{at, value});
                            s.level = ParamValue(std::move(c));
                        } else {
                            s.level = ParamValue(value);
                        }
                    });
}

QString AudioMixerBridge::setSendPan(const QString& track, int send, double pan) {
    const double value = std::clamp(pan, -1.0, 1.0);
    return editSend(track, send, QT_TRANSLATE_NOOP("wabcut::app::AudioMixerBridge", "Send Pan"),
                    "send:" + track.toStdString() + ":" + std::to_string(send) + ":pan", [&](TrackSend& s) { s.pan = ParamValue(value); });
}

void AudioMixerBridge::endSendGesture() {
    endFaderGesture();
}

QString AudioMixerBridge::setSendOption(const QString& track, int send, const QString& option, bool value) {
    if (option == QLatin1String("preFader"))
        return editSend(track, send, value ? QT_TRANSLATE_NOOP("wabcut::app::AudioMixerBridge", "Pre-Fader Send")
                                           : QT_TRANSLATE_NOOP("wabcut::app::AudioMixerBridge", "Post-Fader Send"),
                        {}, [&](TrackSend& s) { s.preFader = value; });
    if (option == QLatin1String("muted"))
        return editSend(track, send, QT_TRANSLATE_NOOP("wabcut::app::AudioMixerBridge", "Mute Send"), {},
                        [&](TrackSend& s) { s.muted = value; });
    return tx("Unknown send option");
}

QString AudioMixerBridge::setSendSubmix(const QString& track, int send, const QString& submix) {
    const auto to = parseTrackKey(submix);
    if (!to)
        return tx("Choose a submix");
    return editSend(track, send, QT_TRANSLATE_NOOP("wabcut::app::AudioMixerBridge", "Send Assignment"), {},
                    [&](TrackSend& s) { s.submix = *to; });
}

QString AudioMixerBridge::removeSend(const QString& track, int send) {
    const auto id = parseTrackKey(track);
    if (!id || send < 1)
        return tx("No such send");
    const command::CommandResult r = run("track.removeSend", Json{{"track", id->value}, {"send", send}});
    return r.ok() ? QString() : QString::fromStdString(r.error->message);
}

} // namespace wabcut::app
