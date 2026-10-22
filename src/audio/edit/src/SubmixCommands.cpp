// p02 audio (docs/audio.md "Submixes and sends"): Premiere's audio submix tracks, track output assignment and sends.
//
//   sequence.addSubmixTrack   a submix track of a format (EditSequence; sequence.removeTrack deletes one)
//   track.setOutput           an audio track's or submix's output: the Mix or a submix (EditSequence)
//   track.addSend             a send from an audio track to a submix (EditSequence)
//   track.setSend             a send's submix, pre/post-fader, mute, level and pan (EditSequence)
//   track.removeSend          a send (EditSequence)
//   audio.routing             every audio track's and submix's output and sends (Read)
//
// Submix tracks are labelled S1..Sn in params and reports (A1..An are the audio tracks); sends are numbered 1..5 (A..E
// in Premiere's mixer). Their effects, fader and panner use the existing audio.addEffect / track.setVolume /
// track.setPan with the submix as the target, and their mute, solo and name track.setState.

#include "AudioCommandHelpers.h"

#include <wabcut/model/SubmixOps.h>

namespace wabcut::audio::edit {

namespace {

using namespace detail;

void add(command::CommandRegistry& registry, command::CommandSpec spec) {
    [[maybe_unused]] const auto added = registry.add(std::move(spec));
}

/// A submix track named by `key` ("S1" or an id).
TrackId submixParam(ExecutionContext& c, const char* key) {
    const TrackId id = c.track(key);
    if (c.sequence().findSubmix(id) == nullptr) {
        fail(ErrorCode::InvalidParams, c.trackLabel(id) + " is not a submix track (submixes are S1, S2, ...)",
             Json{{"path", std::string("/") + key}});
    }
    return id;
}

/// `value` with a key at `time` (a constant becomes a curve of that one key; an existing key there is replaced).
ParamValue withKey(const ParamValue& value, Ticks time, double v) {
    Curve curve;
    if (const Curve* existing = value.curve()) {
        curve = *existing;
    }
    Keyframe key;
    key.time = time;
    key.value = v;
    if (const auto at = curve.keyAt(time)) {
        curve.keys[*at] = key;
    } else {
        curve.keys.insert(curve.keys.begin() + static_cast<std::ptrdiff_t>(curve.insertionIndex(time)), key);
    }
    return ParamValue(std::move(curve));
}

Json valueReport(const ParamValue& v) {
    if (const Curve* curve = v.curve()) {
        Json keys = Json::array();
        for (const Keyframe& k : curve->keys) {
            keys.push_back(Json{{"time", k.time.value}, {"value", round3(k.value)}});
        }
        return Json{{"keyframes", std::move(keys)}};
    }
    return round3(v.constant());
}

Json sendJson(ExecutionContext& c, const TrackSend& s, std::size_t index) {
    return Json{{"send", index + 1},
                {"submix", c.trackLabel(s.submix)},
                {"preFader", s.preFader},
                {"muted", s.muted},
                {"level", valueReport(s.level)},
                {"pan", valueReport(s.pan)}};
}

std::string outputLabel(ExecutionContext& c, TrackId output) { return output.isValid() ? c.trackLabel(output) : "mix"; }

CommandOutput addSubmixTrack(ExecutionContext& c) {
    const std::string formatName = c.string("format", "stereo");
    const auto format = channelFormatFromString(formatName);
    if (!format) {
        fail(ErrorCode::InvalidParams, "unknown channel format '" + formatName + "' (mono, stereo, 5.1, adaptive)",
             Json{{"path", "/format"}});
    }
    auto result = c.take(wabcut::edit::addSubmixTrack(c.state(), c.sequenceId(), *format, c.string("name", "")));
    const Sequence& after = sequenceIn(result.state, c.sequenceId());
    const TrackId id = after.submixTracks.back()->id;
    CommandOutput out = edited(std::move(result), Json{{"track", "S" + std::to_string(after.submixTracks.size())},
                                                       {"id", id.value},
                                                       {"format", formatName}});
    out.createdIds.push_back(id.value);
    return out;
}

CommandOutput setOutput(ExecutionContext& c) {
    const TrackId track = c.track("track");
    TrackId output;
    const Json& v = c.params()["output"];
    if (!(v.is_string() && (v.get<std::string>() == "mix" || v.get<std::string>() == "master"))) {
        output = submixParam(c, "output");
    }
    return edited(c.take(wabcut::edit::setTrackOutput(c.state(), c.sequenceId(), track, output)),
                  Json{{"track", c.trackLabel(track)}, {"output", outputLabel(c, output)}});
}

/// The send fields the params set, over `send`.
void applySendParams(ExecutionContext& c, TrackSend& send) {
    if (c.has("submix")) {
        send.submix = submixParam(c, "submix");
    }
    if (c.has("preFader")) {
        send.preFader = c.boolean("preFader", false);
    }
    if (c.has("muted")) {
        send.muted = c.boolean("muted", false);
    }
    const std::optional<Ticks> time = c.optionalTime("time");
    if (c.has("level")) {
        const double db = c.number("level");
        send.level = time ? withKey(send.level, *time, db) : ParamValue(db);
    }
    if (c.has("pan")) {
        const double pan = c.number("pan");
        send.pan = time ? withKey(send.pan, *time, pan) : ParamValue(pan);
    }
}

std::size_t sendIndex(ExecutionContext& c, TrackId track) {
    const Track* t = c.sequence().findTrack(track);
    const auto index = static_cast<std::size_t>(c.integer("send")) - 1;
    if (t == nullptr || index >= t->sends.size()) {
        fail(ErrorCode::NotFound, c.trackLabel(track) + " has no send " + std::to_string(index + 1), Json{{"path", "/send"}});
    }
    return index;
}

CommandOutput addSend(ExecutionContext& c) {
    const TrackId track = c.track("track");
    TrackSend send;
    send.submix = submixParam(c, "submix");
    applySendParams(c, send);
    auto result = c.take(wabcut::edit::addTrackSend(c.state(), c.sequenceId(), track, send));
    const Track* after = sequenceIn(result.state, c.sequenceId()).findTrack(track);
    const std::size_t index = after != nullptr && !after->sends.empty() ? after->sends.size() - 1 : 0;
    return edited(std::move(result), Json{{"track", c.trackLabel(track)}, {"send", sendJson(c, send, index)}});
}

CommandOutput setSend(ExecutionContext& c) {
    const TrackId track = c.track("track");
    const std::size_t index = sendIndex(c, track);
    TrackSend send = c.sequence().findTrack(track)->sends[index];
    applySendParams(c, send);
    return edited(c.take(wabcut::edit::setTrackSend(c.state(), c.sequenceId(), track, index, send)),
                  Json{{"track", c.trackLabel(track)}, {"send", sendJson(c, send, index)}});
}

CommandOutput removeSend(ExecutionContext& c) {
    const TrackId track = c.track("track");
    const std::size_t index = sendIndex(c, track);
    return edited(c.take(wabcut::edit::removeTrackSend(c.state(), c.sequenceId(), track, index)),
                  Json{{"track", c.trackLabel(track)}, {"removed", index + 1}});
}

CommandOutput routing(ExecutionContext& c) {
    const Sequence& s = c.sequence();
    const auto busJson = [&](const Track& t) {
        Json sends = Json::array();
        for (std::size_t i = 0; i < t.sends.size(); ++i) {
            sends.push_back(sendJson(c, t.sends[i], i));
        }
        Json out{{"track", c.trackLabel(t.id)}, {"output", outputLabel(c, t.output)}, {"sends", std::move(sends)}};
        if (!t.name.empty()) {
            out["name"] = t.name;
        }
        return out;
    };
    Json tracks = Json::array();
    for (const TrackPtr& t : s.audioTracks) {
        tracks.push_back(busJson(*t));
    }
    Json submixes = Json::array();
    for (const TrackPtr& t : s.submixTracks) {
        Json j = busJson(*t);
        j["format"] = std::string(toString(t->format));
        j["effects"] = static_cast<std::int64_t>(t->effects.size());
        j["muted"] = t->muted;
        j["solo"] = t->solo;
        submixes.push_back(std::move(j));
    }
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"tracks", std::move(tracks)}, {"submixes", std::move(submixes)}};
    return out;
}

Json levelParam() { return schema::number("Send volume, dB (-144 = silent .. +24; 0 = unity).", -144.0, 24.0); }
Json panParam() { return schema::number("Send pan: -1 left .. 1 right.", -1.0, 1.0); }

} // namespace

void registerSubmixCommands(command::CommandRegistry& registry) {
    add(registry,
        {"sequence.addSubmixTrack", 1, "Add Audio Submix Track",
         "Adds an audio submix track (Premiere's Sequence > Add Tracks > Audio Submix Tracks): a bus that audio tracks are "
         "assigned to (track.setOutput) or send to (track.addSend). Its own effects (audio.addEffect with the submix as the "
         "target), fader and panner (track.setVolume / track.setPan), mute and solo (track.setState) apply to the sum, and it "
         "outputs to the Mix. Submixes are labelled S1, S2, ... A mono submix folds its input to mono and pans it; the "
         "format is fixed once made. Use it to put one reverb or compressor on several tracks (all the voices, the music). "
         "sequence.removeTrack deletes one (its tracks go to the Mix). One undo step.",
         Capability::EditSequence,
         withSequence({{"format", schema::enumeration({"mono", "stereo", "5.1", "adaptive"}, "The submix's channels (default stereo).")},
                       {"name", schema::string("A name (default \"Submix n\").")}}),
         Json{{"format", "stereo"}, {"name", "Voices"}}, {}, addSubmixTrack});
    add(registry,
        {"track.setOutput", 1, "Set Track Output",
         "Premiere's track output assignment: where an audio track or a submix goes, \"mix\" (the master) or a submix track "
         "(\"S1\"). A submix cannot output into itself or into a submix that feeds it. One undo step.",
         Capability::EditSequence,
         withSequence({{"track", trackParam("The audio track or submix (\"S1\").")},
                       {"output", Json{{"type", Json::array({"integer", "string"})},
                                       {"description", "\"mix\", or a submix track (\"S1\" or its id)."}}}},
                      {"track", "output"}),
         Json{{"track", "A2"}, {"output", "S1"}}, {}, setOutput});
    add(registry,
        {"track.addSend", 1, "Add Track Send",
         "A send from an audio track to a submix (Premiere's Audio Track Mixer sends; at most 5 per track): a copy of the "
         "track's sound at its own level and pan, post-fader (follows the track's fader) unless \"preFader\". A muted or "
         "soloed-away track is silent in its sends too. Use it for a shared reverb: send each voice to a submix with the "
         "reverb. One undo step.",
         Capability::EditSequence,
         withSequence({{"track", trackParam("The audio track.")},
                       {"submix", trackParam("The submix track (\"S1\").")},
                       {"preFader", schema::boolean("Pre-Fader: ignore the track's fader and panner (default post-fader).")},
                       {"muted", schema::boolean("Mute the send.")},
                       {"level", levelParam()},
                       {"pan", panParam()}},
                      {"track", "submix"}),
         Json{{"track", "A1"}, {"submix", "S1"}, {"level", -6}}, {}, addSend});
    add(registry,
        {"track.setSend", 1, "Set Track Send",
         "Changes send \"send\" (1..5) of an audio track: its submix, pre- or post-fader, mute, level (dB) and pan; with "
         "\"time\" the level and pan given become keyframes there (send automation). One undo step.",
         Capability::EditSequence,
         withSequence({{"track", trackParam("The audio track.")},
                       {"send", schema::integer("The send, 1..5.", 1, static_cast<std::int64_t>(kMaxTrackSends))},
                       {"submix", trackParam("Another submix track.")},
                       {"preFader", schema::boolean("Pre-Fader (true) or Post-Fader (false).")},
                       {"muted", schema::boolean("Mute the send.")},
                       {"level", levelParam()},
                       {"pan", panParam()},
                       {"time", timeParam("Put the level and pan as keyframes here instead of constants.")}},
                      {"track", "send"}),
         Json{{"track", "A1"}, {"send", 1}, {"preFader", true}}, {}, setSend});
    add(registry,
        {"track.removeSend", 1, "Remove Track Send",
         "Removes send \"send\" (1..5) of an audio track; the later sends move up. One undo step.", Capability::EditSequence,
         withSequence({{"track", trackParam("The audio track.")},
                       {"send", schema::integer("The send, 1..5.", 1, static_cast<std::int64_t>(kMaxTrackSends))}},
                      {"track", "send"}),
         Json{{"track", "A1"}, {"send", 1}}, {}, removeSend});
    add(registry,
        {"audio.routing", 1, "Audio Routing",
         "Every audio track's output (\"mix\" or a submix) and sends, and every submix track with its format, output, sends, "
         "effect count, mute and solo. A read: it changes nothing.",
         Capability::Read, withSequence({}), Json::object(), {}, routing});
}

} // namespace wabcut::audio::edit
