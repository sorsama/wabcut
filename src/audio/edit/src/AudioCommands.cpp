#include <wabcut/audio/edit/AudioCommands.h>

#include "AudioCommandHelpers.h"

#include <wabcut/audio/edit/AudioAnalysis.h>
#include <wabcut/audio/edit/Ducking.h>
#include <wabcut/audio/edit/EssentialSound.h>
#include <wabcut/audio/edit/GameAudio.h>
#include <wabcut/audio/edit/Loudness.h>
#include <wabcut/audio/edit/PluginLibrary.h>

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Schema.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/Resolve.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace wabcut::audio::edit {

using command::Capability;
using command::CommandOutput;
using command::ErrorCode;
using command::ExecutionContext;
using command::fail;
using command::Json;
using wabcut::edit::EffectTarget;
using wabcut::edit::ParamRef;
namespace schema = command::schema;
using namespace detail;

std::string_view toString(SpeechSource source) {
    switch (source) {
    case SpeechSource::Transcript:
        return "transcript";
    case SpeechSource::Energy:
        return "energy";
    case SpeechSource::Auto:
        break;
    }
    return "auto";
}

std::optional<SpeechSource> speechSourceFromString(std::string_view text) {
    for (const SpeechSource s : {SpeechSource::Auto, SpeechSource::Transcript, SpeechSource::Energy}) {
        if (text == toString(s)) {
            return s;
        }
    }
    return std::nullopt;
}

namespace {

// ---- Schema and parameter helpers ------------------------------------------------------------------------------

Json targetParam() {
    return Json{{"type", Json::array({"integer", "string"})},
                {"description", "\"master\" (the master audio bus) or an audio track: a label (\"A1\") or a track id. "
                                "Clip stacks are edited with clip.addEffect and the other clip.* commands."}};
}

Json audioEffectIds() {
    std::vector<std::string> ids;
    for (const EffectDescriptor& d : EffectRegistry::builtin().all()) {
        // Enhance Speech plays a media item's take and the Essential Sound record describes a clip: clip stacks only
        // (audio.enhanceSpeech, audio.essentialSound, clip.addEffect).
        if (d.kind == EffectKind::Audio && !d.intrinsic && d.id != "wabcut.audio.enhanceSpeech" && d.id != kEssentialSoundEffect) {
            ids.emplace_back(d.id);
        }
    }
    return schema::enumeration(std::move(ids), "The audio effect id (effect.list with kind \"audio\").");
}

Json effectInstanceParam(std::string description) {
    return schema::integer(std::move(description) + " The effect instance id (audio.effects lists them).", 1);
}

Json paramsObject(std::string description) { return Json{{"type", "object"}, {"description", std::move(description)}}; }

/// The stack a "target" names, and how it is reported.
struct Site {
    EffectTarget target;
    std::string label;
};

Site siteOf(ExecutionContext& c, const char* key = "target") {
    const Json& v = c.params()[key];
    if (v.is_string() && v.get<std::string>() == "master") {
        return {EffectTarget::masterAudio(), "master"};
    }
    const TrackId track = c.trackValue(v, std::string("/") + key);
    const Track* t = c.sequence().findAudioBus(track); // p02 audio: a submix track ("S1") too
    if (t == nullptr || t->kind != TrackKind::Audio) {
        fail(ErrorCode::InvalidParams, "the target must be \"master\", an audio track or a submix track",
             Json{{"path", std::string("/") + key}});
    }
    return {EffectTarget::ofTrack(track), c.trackLabel(track)};
}

std::vector<EffectParam> constantParams(ExecutionContext& c, const char* key) {
    std::vector<EffectParam> out;
    if (!c.has(key)) {
        return out;
    }
    for (const auto& item : c.params()[key].items()) {
        if (!item.value().is_number()) {
            fail(ErrorCode::InvalidParams, "parameter values are numbers", Json{{"path", std::string("/") + key + "/" + item.key()}});
        }
        out.push_back(EffectParam{item.key(), ParamValue(item.value().get<double>())});
    }
    return out;
}

EffectInstanceId instanceOf(ExecutionContext& c, const char* key = "effect") {
    return EffectInstanceId{static_cast<std::uint64_t>(c.integer(key))};
}

/// The parameter a keyframe command names: a parameter of "effect", or without one the fader (A.3): "volume" (an
/// audio track or the master) or "pan" (an audio track).
ParamRef paramOf(ExecutionContext& c) {
    if (c.has("effect")) {
        return ParamRef::ofEffect(instanceOf(c), c.string("param"));
    }
    const std::string param = c.string("param");
    if (param == "volume" || param == "volume.level") {
        return ParamRef::of(ParamId::VolumeDb);
    }
    if (param == "pan" || param == "panner.balance") {
        return ParamRef::of(ParamId::Pan);
    }
    fail(ErrorCode::InvalidParams, "without \"effect\", \"param\" names the fader: \"volume\" or \"pan\"",
         Json{{"path", "/param"}});
}

Json valueJson(const ParamValue& v) {
    if (const Curve* curve = v.curve()) {
        Json keys = Json::array();
        for (const Keyframe& k : curve->keys) {
            keys.push_back(Json{{"t", k.time.value}, {"v", k.value}, {"i", std::string(toString(k.interpolation))}});
        }
        return Json{{"keys", std::move(keys)}};
    }
    return v.constant();
}

Json stackJson(const EffectStack& stack) {
    Json out = Json::array();
    for (const EffectInstance& e : stack) {
        Json params = Json::object();
        for (const EffectParam& p : e.params) {
            params[p.key] = valueJson(p.value);
        }
        out.push_back(Json{{"id", e.id.value}, {"effect", e.effect}, {"version", e.version}, {"enabled", e.enabled},
                           {"evaluable", isEvaluable(e, EffectKind::Audio)}, {"params", std::move(params)}});
    }
    return out;
}

/// The optional [from, to) range of a command.
std::optional<TimeRange> rangeOf(ExecutionContext& c) {
    const std::optional<Ticks> from = c.optionalTime("from");
    const std::optional<Ticks> to = c.optionalTime("to");
    if (!from && !to) {
        return std::nullopt;
    }
    const Ticks start = from.value_or(Ticks{});
    const Ticks end = to.value_or(std::max(c.sequence().contentEnd(), start));
    if (end <= start) {
        fail(ErrorCode::InvalidParams, "\"to\" must come after \"from\"", Json{{"path", "/to"}});
    }
    return TimeRange::fromStartEnd(start, end);
}

// ---- track.setRole ---------------------------------------------------------------------------------------------

CommandOutput setRole(ExecutionContext& c) {
    wabcut::edit::TrackStateChange change;
    change.role = trackRoleFromString(c.string("role"));
    const TrackId track = c.track("track");
    return edited(c.take(wabcut::edit::setTrackState(c.state(), c.sequenceId(), track, change)),
                  Json{{"track", c.trackLabel(track)}, {"role", c.string("role")}});
}

// ---- Track and master effect stacks ----------------------------------------------------------------------------

CommandOutput effects(ExecutionContext& c) {
    const Site site = siteOf(c);
    const EffectStack* stack = wabcut::edit::effectStackOf(c.sequence(), site.target);
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"target", site.label}, {"effects", stackJson(stack != nullptr ? *stack : EffectStack{})}};
    // A.3: the fader after the stack (dB; tracks also the pan), constants or keyframes in timeline ticks.
    if (site.target.kind == EffectTarget::Kind::MasterAudio) {
        out.data["volume"] = valueJson(c.sequence().masterVolume);
    } else if (const Track* track = c.sequence().findAudioBus(site.target.track)) {
        out.data["volume"] = valueJson(track->volume);
        out.data["pan"] = valueJson(track->pan);
    }
    return out;
}

/// The effect an add command names: "effect" (built-in) or "plugin" (an audio plugin this computer has, A.6).
std::string effectToAdd(ExecutionContext& c) {
    if (c.has("plugin") == c.has("effect")) {
        fail(ErrorCode::InvalidParams, "give \"effect\" (a built-in effect) or \"plugin\" (plugins.list), not both");
    }
    if (!c.has("plugin")) {
        return c.string("effect");
    }
    const std::string id = c.string("plugin");
    if (!id.starts_with(kPluginEffectPrefix) || EffectRegistry::builtin().find(id) == nullptr) {
        fail(ErrorCode::NotFound, "no audio plugin '" + id + "' on this computer (plugins.list lists them)", Json{{"path", "/plugin"}});
    }
    return id;
}

CommandOutput addEffect(ExecutionContext& c) {
    const Site site = siteOf(c);
    std::optional<std::size_t> index;
    if (c.has("index")) {
        index = static_cast<std::size_t>(c.integer("index"));
    }
    return edited(c.take(wabcut::edit::addEffect(c.state(), c.sequenceId(), site.target, effectToAdd(c),
                                                 constantParams(c, "params"), index)),
                  Json{{"target", site.label}});
}

CommandOutput setEffect(ExecutionContext& c) {
    const Site site = siteOf(c);
    wabcut::edit::EffectChange change;
    if (c.has("enabled")) {
        change.enabled = c.boolean("enabled", true);
    }
    change.params = constantParams(c, "params");
    return edited(c.take(wabcut::edit::setEffect(c.state(), c.sequenceId(), site.target, instanceOf(c), change)));
}

CommandOutput removeEffect(ExecutionContext& c) {
    const Site site = siteOf(c);
    return edited(c.take(wabcut::edit::removeEffect(c.state(), c.sequenceId(), site.target, instanceOf(c))));
}

CommandOutput moveEffect(ExecutionContext& c) {
    const Site site = siteOf(c);
    return edited(c.take(wabcut::edit::moveEffect(c.state(), c.sequenceId(), site.target, instanceOf(c),
                                                  static_cast<std::size_t>(c.integer("index")))));
}

CommandOutput addKeyframe(ExecutionContext& c) {
    const Site site = siteOf(c);
    Keyframe key;
    key.time = c.time("time"); // track and master curves run in timeline time
    key.value = c.number("value");
    key.interpolation = interpolationFromString(c.string("interpolation", "linear")).value_or(Interpolation::Linear);
    return edited(c.take(wabcut::edit::addKeyframe(c.state(), c.sequenceId(), site.target, paramOf(c), key)));
}

CommandOutput removeKeyframe(ExecutionContext& c) {
    const Site site = siteOf(c);
    return edited(c.take(wabcut::edit::removeKeyframe(c.state(), c.sequenceId(), site.target, paramOf(c),
                                                      static_cast<std::size_t>(c.integer("index")))));
}

CommandOutput moveKeyframe(ExecutionContext& c) {
    const Site site = siteOf(c);
    return edited(c.take(wabcut::edit::moveKeyframe(c.state(), c.sequenceId(), site.target, paramOf(c),
                                                    static_cast<std::size_t>(c.integer("index")), c.time("time"))));
}

// ---- Faders (A.3): track volume and pan, the master gain -----------------------------------------------------------

/// Sets a fader parameter to a constant, or with "time" puts a keyframe there (the parameter becomes, or stays,
/// animated). A constant replaces any keyframes.
CommandOutput setFader(ExecutionContext& c, const Site& site, ParamId id, const char* valueKey) {
    const double value = c.number(valueKey);
    const ParamRef ref = ParamRef::of(id);
    wabcut::edit::EditResult r;
    if (c.has("time")) {
        Keyframe key;
        key.time = c.time("time");
        key.value = value;
        key.interpolation = interpolationFromString(c.string("interpolation", "linear")).value_or(Interpolation::Linear);
        r = c.take(wabcut::edit::addKeyframe(c.state(), c.sequenceId(), site.target, ref, key));
    } else {
        r = c.take(wabcut::edit::setParameter(c.state(), c.sequenceId(), site.target, ref, ParamValue(value)));
    }
    const Sequence& after = sequenceIn(r.state, c.sequenceId());
    const ParamValue* now = &after.masterVolume;
    if (site.target.kind == EffectTarget::Kind::Track) {
        const Track* t = after.findAudioBus(site.target.track);
        now = id == ParamId::VolumeDb ? &t->volume : &t->pan;
    }
    return edited(std::move(r), Json{{"target", site.label}, {valueKey, value}, {"value", valueJson(*now)}});
}

/// An audio track named by "track" (not the master).
Site trackSite(ExecutionContext& c) {
    if (c.params()["track"].is_string() && c.params()["track"].get<std::string>() == "master") {
        fail(ErrorCode::InvalidParams, "the master's level is sequence.setMasterGain", Json{{"path", "/track"}});
    }
    return siteOf(c, "track");
}

CommandOutput setTrackVolume(ExecutionContext& c) { return setFader(c, trackSite(c), ParamId::VolumeDb, "db"); }

CommandOutput setTrackPan(ExecutionContext& c) { return setFader(c, trackSite(c), ParamId::Pan, "pan"); }

CommandOutput setMasterGain(ExecutionContext& c) {
    return setFader(c, Site{EffectTarget::masterAudio(), "master"}, ParamId::VolumeDb, "db");
}

// ---- Loudness --------------------------------------------------------------------------------------------------

struct NamedTarget {
    LoudnessTarget target;
    Json label;
};

std::vector<NamedTarget> loudnessTargets(ExecutionContext& c) {
    std::vector<NamedTarget> out;
    if (!c.has("targets")) {
        out.push_back({LoudnessTarget::mix(), "mix"});
        return out;
    }
    const Json& list = c.params()["targets"];
    for (std::size_t i = 0; i < list.size(); ++i) {
        const Json& t = list[i];
        const std::string path = "/targets/" + std::to_string(i);
        if (t.is_string() && t.get<std::string>() == "mix") {
            out.push_back({LoudnessTarget::mix(), "mix"});
        } else if (t.is_object() && t.contains("clip")) {
            const ClipId clip = c.clipValue(t["clip"], path + "/clip");
            out.push_back({LoudnessTarget::ofClip(clip), Json{{"clip", clip.value}}});
        } else if (t.is_object() && t.contains("track")) {
            const TrackId track = c.trackValue(t["track"], path + "/track");
            out.push_back({LoudnessTarget::ofTrack(track), Json{{"track", c.trackLabel(track)}}});
        } else {
            fail(ErrorCode::InvalidParams, "a target is \"mix\", {\"clip\": ...} or {\"track\": ...}", Json{{"path", path}});
        }
    }
    return out;
}

CommandOutput loudness(ExecutionContext& c) {
    AudioServices& services = servicesOf(c, true, false);
    const auto range = rangeOf(c);
    Json results = Json::array();
    for (const NamedTarget& t : loudnessTargets(c)) {
        c.checkpoint("measuring loudness");
        Json r = statsJson(measureOrFail(c, services, c.state(), t.target, range));
        r["target"] = t.label;
        results.push_back(std::move(r));
    }
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"results", std::move(results)}};
    return out;
}

/// Adds the master true-peak limiter (or updates the last effect of the master stack when it is a limiter).
ProjectState withMasterLimiter(ExecutionContext& c, const ProjectState& state, double ceiling, std::optional<double> releaseMs,
                               std::optional<double> lookaheadMs, std::uint64_t& id) {
    const EffectStack& stack = sequenceIn(state, c.sequenceId()).audioEffects;
    std::vector<EffectParam> params{{"ceiling", ParamValue(ceiling)}};
    if (releaseMs) {
        params.push_back({"release", ParamValue(*releaseMs)});
    }
    if (!stack.empty() && stack.back().effect == kLimiterEffect) {
        if (lookaheadMs) {
            params.push_back({"lookahead", ParamValue(*lookaheadMs)});
        }
        id = stack.back().id.value;
        return c.take(wabcut::edit::setEffect(state, c.sequenceId(), EffectTarget::masterAudio(), stack.back().id,
                                              wabcut::edit::EffectChange{true, params}))
            .state;
    }
    params.push_back({"lookahead", ParamValue(lookaheadMs.value_or(5.0))});
    auto r = c.take(wabcut::edit::addEffect(state, c.sequenceId(), EffectTarget::masterAudio(), std::string(kLimiterEffect),
                                            params));
    id = r.createdEffects.empty() ? 0 : r.createdEffects.front().value;
    return std::move(r.state);
}

CommandOutput normalize(ExecutionContext& c) {
    AudioServices& services = servicesOf(c, true, false);
    double target = 0.0;
    double ceiling = -1.0;
    std::string preset = "custom";
    if (c.has("lufs")) {
        target = c.number("lufs");
        ceiling = c.has("truePeak") ? c.number("truePeak") : -1.0;
    } else {
        preset = c.string("preset", "youtube");
        const LoudnessPreset* p = findLoudnessPreset(preset);
        if (p == nullptr) {
            fail(ErrorCode::InvalidParams, "unknown preset \"" + preset + "\"", Json{{"path", "/preset"}});
        }
        target = p->targetLufs;
        ceiling = c.has("truePeak") ? c.number("truePeak") : p->truePeakDbtp;
    }
    const double tolerance = c.has("tolerance") ? c.number("tolerance") : 0.1;
    const int maxPasses = static_cast<int>(c.integer("maxPasses", 4));
    const auto range = rangeOf(c);

    // What gets a gain: each clip, a track, or the mix (the master bus, before its limiter).
    std::vector<std::pair<NamedTarget, EffectTarget>> sites;
    if (c.has("clips")) {
        for (const ClipId clip : c.clips("clips")) {
            sites.push_back({{LoudnessTarget::ofClip(clip), Json{{"clip", clip.value}}}, EffectTarget::ofClip(clip)});
        }
    } else if (c.has("track")) {
        const TrackId track = c.track("track");
        sites.push_back({{LoudnessTarget::ofTrack(track), Json{{"track", c.trackLabel(track)}}}, EffectTarget::ofTrack(track)});
    } else {
        sites.push_back({{LoudnessTarget::mix(), "mix"}, EffectTarget::masterAudio()});
    }

    ProjectState state = c.state();
    CommandOutput out;
    std::uint64_t limiterId = 0;
    const bool limit = c.boolean("limit", false);
    if (limit) {
        if (c.has("clips") || c.has("track")) {
            fail(ErrorCode::InvalidParams, "\"limit\" adds the master limiter: it goes with the mix", Json{{"path", "/limit"}});
        }
        state = withMasterLimiter(c, state, ceiling, std::nullopt, std::nullopt, limiterId);
    }
    Json results = Json::array();
    for (const auto& [named, site] : sites) {
        c.checkpoint("normalizing loudness");
        LoudnessMatch m = matchLoudness(c, services, std::move(state), named.target, site, target, tolerance, maxPasses, range);
        state = std::move(m.state);
        Json r = std::move(m.result);
        r["target"] = named.label;
        if (!m.measured) {
            out.warnings.push_back("nothing to measure (silent or shorter than 0.4 s); left as it is");
            results.push_back(std::move(r));
            continue;
        }
        if (!m.reached) {
            out.warnings.push_back("could not reach " + std::to_string(target) + " LUFS within " +
                                   std::to_string(tolerance) + " LU");
        }
        if (m.now.truePeakDbtp > ceiling + 0.05 && !limit) {
            out.warnings.push_back("true peak " + std::to_string(m.now.truePeakDbtp) + " dBTP is above " +
                                   std::to_string(ceiling) + " dBTP: add the master limiter (\"limit\": true)");
        }
        results.push_back(std::move(r));
    }
    out.state = std::move(state);
    if (limiterId != 0) {
        out.createdIds.push_back(limiterId);
    }
    out.data = Json{{"preset", preset}, {"targetLufs", target}, {"truePeakDbtp", ceiling}, {"results", std::move(results)}};
    return out;
}

CommandOutput masterLimiter(ExecutionContext& c) {
    std::uint64_t id = 0;
    const double ceiling = c.has("ceiling") ? c.number("ceiling") : -1.0;
    std::optional<double> release;
    std::optional<double> lookahead;
    if (c.has("release")) {
        release = c.number("release");
    }
    if (c.has("lookahead")) {
        lookahead = c.number("lookahead");
    }
    CommandOutput out;
    out.state = withMasterLimiter(c, c.state(), ceiling, release, lookahead, id);
    out.createdIds.push_back(id);
    out.data = Json{{"effect", id}, {"ceiling", ceiling}};
    return out;
}

// ---- Auto-ducking ----------------------------------------------------------------------------------------------

/// The tracks a duck used, for its report: label and name.
Json duckTracksJson(ExecutionContext& c, const std::vector<TrackId>& tracks) {
    Json out = Json::array();
    for (const TrackId id : tracks) {
        const Track* t = c.sequence().findTrack(id);
        out.push_back(Json{{"track", c.trackLabel(id)}, {"name", t != nullptr ? t->name : std::string()}});
    }
    return out;
}

CommandOutput autoDuck(ExecutionContext& c) {
    // A.10: "gameUnderVoice" takes the game audio (and the music) under a streamer's mic: its own timing, and without
    // "dialogue" / "music" the tracks chooseGameVoiceTracks picks by name, else by role. Explicit parameters still win.
    const std::string preset = c.string("preset", "default");
    const bool game = preset == "gameUnderVoice";
    if (!game && preset != "default") {
        fail(ErrorCode::InvalidParams, "preset is default or gameUnderVoice", Json{{"path", "/preset"}});
    }
    std::vector<TrackId> dialogue;
    std::vector<TrackId> music;
    if (game) {
        const GameVoiceTracks chosen = chooseGameVoiceTracks(c.sequence(), c.boolean("includeMusic", true));
        dialogue = c.has("dialogue") ? c.tracks("dialogue") : chosen.voice;
        music = c.has("music") ? c.tracks("music") : chosen.game;
        if (dialogue.empty()) {
            fail(ErrorCode::InvalidParams, "no voice track: name the microphone track \"Mic\", give it the dialogue role "
                                           "(track.setRole) or pass \"dialogue\"");
        }
        if (music.empty()) {
            fail(ErrorCode::InvalidParams, "no game track: name the game audio track \"Game\", give it the effects role "
                                           "(track.setRole) or pass \"music\"");
        }
    } else {
        dialogue = tracksOrRole(c, "dialogue", TrackRole::Dialogue);
        music = tracksOrRole(c, "music", TrackRole::Music);
        if (dialogue.empty()) {
            fail(ErrorCode::InvalidParams, "no dialogue tracks: give tracks the dialogue role (track.setRole) or pass \"dialogue\"");
        }
        if (music.empty()) {
            fail(ErrorCode::InvalidParams, "no music tracks: give tracks the music role (track.setRole) or pass \"music\"");
        }
    }
    AudioServices& services = servicesOf(c, false, true);
    DuckingOptions options = game ? gameUnderVoiceDucking() : DuckingOptions{};
    options.amountDb = c.has("amount") ? c.number("amount") : options.amountDb;
    options.attack = secondsOr(c, "attack", options.attack);
    options.release = secondsOr(c, "release", options.release);
    options.hold = secondsOr(c, "hold", options.hold);
    options.minSpeech = secondsOr(c, "minSpeech", options.minSpeech);
    const SpeechSource source = speechSourceFromString(c.string("source", "auto")).value_or(SpeechSource::Auto);
    const auto range = rangeOf(c);

    c.checkpoint("finding speech");
    auto speech = services.speech->speech(c.state(), c.sequenceId(), dialogue, source, range, c.control());
    if (!speech) {
        fail(speech.error().code == wabcut::ErrorCode::Cancelled ? ErrorCode::Cancelled : ErrorCode::EditRejected,
             "speech detection failed: " + speech.error().message);
    }
    const std::vector<DuckRegion> regions = duckRegions(*speech, options);

    CommandOutput out;
    ProjectState state = c.state();
    Json clips = Json::array();
    for (const TrackId trackId : music) {
        const Track* track = sequenceIn(state, c.sequenceId()).findTrack(trackId);
        if (track == nullptr || track->kind != TrackKind::Audio) {
            continue;
        }
        if (track->locked) {
            out.warnings.push_back(c.trackLabel(trackId) + " is locked: not ducked");
            continue;
        }
        std::vector<ClipId> ids;
        for (const Clip& clip : track->clips) {
            ids.push_back(clip.id);
        }
        for (const ClipId id : ids) {
            if (Json row = duckClip(c, state, id, c.trackLabel(trackId), regions, options, range); !row.is_null()) {
                clips.push_back(std::move(row));
            }
        }
    }
    Json duck = duckRegionsJson(c, regions);
    if (regions.empty()) {
        out.warnings.push_back("no speech found on the dialogue tracks");
    }
    out.state = std::move(state);
    out.data = Json{{"regions", std::move(duck)},
                    {"clips", std::move(clips)},
                    {"amountDb", options.amountDb},
                    {"source", std::string(toString(source))},
                    {"preset", preset},
                    {"dialogue", duckTracksJson(c, dialogue)},
                    {"music", duckTracksJson(c, music)}};
    return out;
}

// ---- Enhance Speech and stem separation (A.4) ------------------------------------------------------------------

/// 0.1.2: Enhance Speech and stems render a take of a media item; a multicam or nested clip has none (it plays a
/// sequence's mix), so it is refused with where to go instead of "media 0 does not exist".
void refuseNestedClips(ExecutionContext& c, const std::vector<ClipId>& clips, std::string_view what) {
    const Sequence& seq = c.sequence();
    for (const ClipId id : clips) {
        const auto loc = seq.findClip(id);
        if (loc && loc->clip().isNested()) {
            fail(ErrorCode::InvalidParams,
                 "clip " + std::to_string(id.value) + " plays a multicam or nested sequence: " + std::string(what) +
                     " works on media clips, so run it on the angle's clips inside that sequence (every use of it follows)",
                 Json{{"path", "/clips"}});
        }
    }
}

CommandOutput enhanceSpeech(ExecutionContext& c) {
    auto* services = command::findService<AudioServices>(c.services());
    SpeechEnhancer* enhancer = services != nullptr ? services->enhancer.get() : nullptr;
    const bool render = c.boolean("render", true);
    if (render && enhancer == nullptr) {
        fail(ErrorCode::InvalidRequest, "no Enhance Speech here: the host has no local audio models (the app and wabcut-cli do; "
                                        "\"render\": false only applies the effect)");
    }
    const double strength = c.has("strength") ? c.number("strength") : 100.0;
    if (!(strength >= 0.0 && strength <= 100.0)) {
        fail(ErrorCode::InvalidParams, "strength is a percentage, 0 to 100", Json{{"path", "/strength"}});
    }
    const std::vector<ClipId> clips = audioClipsOf(c, c.clips("clips"));
    refuseNestedClips(c, clips, "Enhance Speech");
    const SequenceId seqId = c.sequenceId();
    const std::string model = enhancer != nullptr ? enhancer->modelVersion() : std::string();
    ProjectState state = c.state();
    CommandOutput out;

    Json takes = ensureEnhancedTakes(c, state, clips, enhancer, render, model);

    // The effect on every clip, enabled, at the strength: first in the stack (it replaces the source, before the
    // clip's level, its redactions and its other effects).
    const std::vector<EffectParam> params{{"strength", ParamValue(strength / 100.0)}};
    Json rows = Json::array();
    for (const ClipId id : clips) {
        const Clip& clip = state.findSequence(seqId)->findClip(id)->clip();
        std::uint64_t effectId = 0;
        if (const EffectInstance* e = findEffect(clip, kEnhanceSpeechEffect)) {
            effectId = e->id.value;
            wabcut::edit::EffectChange change;
            change.enabled = true;
            change.params = params;
            state = c.take(wabcut::edit::setEffect(state, seqId, id, e->id, change)).state;
        } else {
            auto added = c.take(wabcut::edit::addEffect(state, seqId, std::span<const ClipId>(&id, 1), std::string(kEnhanceSpeechEffect),
                                                        params, std::size_t{0}));
            if (!added.createdEffects.empty()) {
                effectId = added.createdEffects.front().value;
                out.createdIds.push_back(effectId);
            }
            state = std::move(added.state);
        }
        const Clip& now = state.findSequence(seqId)->findClip(id)->clip();
        const MediaItem* item = state.findMedia(now.media);
        rows.push_back(Json{{"clip", id.value},
                            {"effect", effectId},
                            {"strength", strength},
                            {"take", item != nullptr ? takeState(*item, now.stream.index, model) : "missing"}});
    }
    for (const Json& t : takes) {
        if (t.value("state", "") != "ready") {
            out.warnings.push_back("'" + t.value("name", "") + "' has no enhanced take yet: its clips play the original until it is rendered");
        }
    }
    out.state = std::move(state);
    out.data = Json{{"clips", std::move(rows)}, {"takes", std::move(takes)}, {"model", model.empty() ? Json(nullptr) : Json(model)}};
    return out;
}

CommandOutput separateStems(ExecutionContext& c) {
    auto* services = command::findService<AudioServices>(c.services());
    StemSeparator* separator = services != nullptr ? services->separator.get() : nullptr;
    if (separator == nullptr) {
        fail(ErrorCode::InvalidRequest, "no stem separation here: the host has no local audio models (the app and wabcut-cli do)");
    }
    if (c.has("model") && c.string("model") != "htdemucs") {
        fail(ErrorCode::InvalidParams, "unknown stem model \"" + c.string("model") + "\" (htdemucs)", Json{{"path", "/model"}});
    }
    // One clip (its media and stream) or one media item (its first audio stream, or "stream").
    MediaId media;
    int stream = 0;
    if (c.has("clip")) {
        const std::vector<ClipId> audio = audioClipsOf(c, {c.clip("clip")});
        refuseNestedClips(c, audio, "Separate Stems");
        const Clip& clip = c.sequence().findClip(audio.front())->clip();
        media = clip.media;
        stream = clip.stream.index;
    } else if (c.has("media")) {
        media = c.media("media");
        stream = static_cast<int>(c.integer("stream", 0));
    } else {
        fail(ErrorCode::InvalidParams, "name a \"clip\" or a \"media\" item to separate", Json{{"path", "/clip"}});
    }
    const MediaItem* item = c.state().findMedia(media);
    if (item == nullptr) {
        fail(ErrorCode::NotFound, "media " + std::to_string(media.value) + " does not exist");
    }
    const std::string what = "separating stems of '" + item->name + "'";
    c.checkpoint(what);
    const MediaItem copy = *item;
    auto result = runJob(c, what, [&](std::stop_token stop, const std::function<void(double)>& progress) {
        return separator->separate(copy, stream, stop, progress);
    });
    if (!result) {
        failWith(result.error(), what);
    }
    // The stems: new media items in a bin of their own next to the source, each recording where it came from.
    ProjectState state = c.state();
    CommandOutput out;
    auto bin = c.take(wabcut::edit::addBin(state, item->name + " stems", item->bin));
    state = std::move(bin.state);
    out.createdIds.push_back(bin.id.value);
    const std::string created = utcNow();
    Json rows = Json::array();
    for (const StemSeparator::Stem& stem : result->stems) {
        MediaItem m;
        m.bin = bin.id;
        m.name = item->name + " (" + stem.part + ")";
        m.locator.path = stem.path;
        m.locator.fingerprint = stem.fingerprint;
        m.info = stem.info;
        m.derived = DerivedProvenance{media, "separateStems", result->model, stem.part, created};
        auto added = c.take(wabcut::edit::addMedia(state, std::move(m)));
        state = std::move(added.state);
        out.createdIds.push_back(added.id.value);
        rows.push_back(Json{{"media", added.id.value}, {"part", stem.part}, {"name", item->name + " (" + stem.part + ")"}, {"path", stem.path}});
    }
    if (auto valid = validate(state, Validation::Structure); !valid) {
        fail(ErrorCode::InvariantViolation, valid.error().message);
    }
    out.state = std::move(state);
    Json data{{"source", media.value}, {"bin", bin.id.value}, {"model", result->model}, {"stems", std::move(rows)},
              {"seconds", round3(result->seconds)}};
    if (result->elapsed > 0) {
        data["elapsed"] = round3(result->elapsed);
        data["realtime"] = round3(result->seconds / result->elapsed);
        data["device"] = result->device;
    } else {
        data["reused"] = true;
    }
    out.data = std::move(data);
    return out;
}

// ---- Audio sync (A.5) ------------------------------------------------------------------------------------------

/// One recording of audio.syncClips: a clip (its media and audio stream; a video clip, its media's first audio
/// stream) or a media item / an angle of several.
struct SyncItem {
    std::optional<ClipId> clip;
    AudioSyncService::Angle angle;
    bool hasVideo = false;
    /// A clip's media time at its start (its sourceIn; a nested or multicam clip: what the recording it plays is at
    /// there), and whether something on the way to that recording is retimed.
    Ticks mediaIn;
    bool retimed = false;
};

bool isRetimed(const Clip& clip) { return clip.isRemapped() || !clip.speed.isUnity() || clip.reversed; }

/// 0.2: the recording a nested or multicam clip plays (a multicam clip: its active audio angle's; a picture: the shown
/// media's sound), found at its start or a little later (an angle that starts late), as a sync item.
std::optional<SyncItem> nestedSyncItem(const ProjectState& state, const Sequence& seq, const Clip& clip) {
    for (int quarter = 0; quarter < 4; ++quarter) {
        const Ticks t = clip.start() + Ticks{clip.duration().value * quarter / 4};
        for (const ResolvedLeaf& leaf : resolveLeaves(state, seq.id, clip, t)) {
            const MediaItem* item = state.findMedia(leaf.clip->media);
            if (item == nullptr || item->info.audio.empty()) {
                continue;
            }
            SyncItem s;
            s.clip = clip.id;
            s.angle.media = {item->id};
            s.angle.stream = leaf.clip->stream.kind == StreamKind::Audio ? leaf.clip->stream.index : 0;
            s.mediaIn = leaf.sourceTime - (t - clip.start());
            s.retimed = isRetimed(*leaf.clip) || std::ranges::any_of(leaf.path, [](const NestLevel& l) { return isRetimed(*l.clip); });
            const Sequence* inner = state.findSequence(clip.sequence);
            s.hasVideo = inner != nullptr && sequenceHasStream(*inner, StreamRef{StreamKind::Video, 0});
            return s;
        }
    }
    return std::nullopt;
}

std::vector<SyncItem> syncItems(ExecutionContext& c) {
    std::vector<SyncItem> items;
    if (c.has("clips")) {
        const Sequence& seq = c.sequence();
        for (const ClipId id : c.clips("clips")) {
            const auto at = seq.findClip(id);
            if (!at) {
                fail(ErrorCode::NotFound, "no clip " + std::to_string(id.value) + " in the sequence");
            }
            const Clip& clip = at->clip();
            if (clip.isNested()) {
                auto nested = nestedSyncItem(c.state(), seq, clip);
                if (!nested) {
                    fail(ErrorCode::InvalidParams, "clip " + std::to_string(id.value) + " plays no recording with audio to sync",
                         Json{{"path", "/clips"}});
                }
                items.push_back(std::move(*nested));
                continue;
            }
            const MediaItem* item = c.state().findMedia(clip.media);
            if (item == nullptr || item->info.audio.empty()) {
                fail(ErrorCode::InvalidParams, "clip " + std::to_string(id.value) + " has no audio to sync", Json{{"path", "/clips"}});
            }
            SyncItem s;
            s.clip = id;
            s.angle.media = {clip.media};
            s.angle.stream = clip.stream.kind == StreamKind::Audio ? clip.stream.index : 0;
            s.hasVideo = !item->info.video.empty();
            s.mediaIn = clip.sourceIn;
            items.push_back(std::move(s));
        }
    } else if (c.has("media")) {
        const Json& list = c.params()["media"];
        for (std::size_t i = 0; i < list.size(); ++i) {
            SyncItem s;
            const std::string path = "/media/" + std::to_string(i);
            if (list[i].is_array()) {
                for (std::size_t k = 0; k < list[i].size(); ++k) {
                    s.angle.media.push_back(c.mediaValue(list[i][k], path + "/" + std::to_string(k)));
                }
            } else {
                s.angle.media.push_back(c.mediaValue(list[i], path));
            }
            if (s.angle.media.empty()) {
                fail(ErrorCode::InvalidParams, "an empty angle", Json{{"path", path}});
            }
            s.angle.stream = static_cast<int>(c.integer("stream", 0));
            const MediaItem* first = c.state().findMedia(s.angle.media.front());
            s.hasVideo = first != nullptr && !first->info.video.empty();
            items.push_back(std::move(s));
        }
    } else {
        fail(ErrorCode::InvalidParams, "name the \"clips\" or the \"media\" to sync", Json{{"path", "/clips"}});
    }
    if (items.size() < 2) {
        fail(ErrorCode::InvalidParams, "audio sync needs two recordings or more", Json{{"path", c.has("clips") ? "/clips" : "/media"}});
    }
    return items;
}

CommandOutput syncClips(ExecutionContext& c) {
    auto* services = command::findService<AudioServices>(c.services());
    if (services == nullptr || !services->sync) {
        fail(ErrorCode::InvalidRequest, "no audio sync here: the host provides none (wabcut-cli and the app sync through the media)");
    }
    const std::vector<SyncItem> items = syncItems(c);
    // The reference: the clip or media named, else the first with a picture (the camera), else the first.
    std::size_t reference = 0;
    if (c.has("reference")) {
        const std::uint64_t id = static_cast<std::uint64_t>(c.integer("reference"));
        const auto it = std::ranges::find_if(items, [&](const SyncItem& s) {
            return s.clip ? s.clip->value == id : std::ranges::find(s.angle.media, MediaId{id}) != s.angle.media.end();
        });
        if (it == items.end()) {
            fail(ErrorCode::InvalidParams, "the reference is not one of the recordings", Json{{"path", "/reference"}});
        }
        reference = static_cast<std::size_t>(it - items.begin());
    } else if (const auto it = std::ranges::find_if(items, &SyncItem::hasVideo); it != items.end()) {
        reference = static_cast<std::size_t>(it - items.begin());
    }
    SyncOptions options;
    if (c.has("minConfidence")) {
        options.minConfidence = c.number("minConfidence");
    }
    if (c.has("minOverlap")) {
        options.minOverlapSeconds = c.number("minOverlap");
    }
    std::vector<AudioSyncService::Angle> angles;
    for (const SyncItem& s : items) {
        angles.push_back(s.angle);
    }
    const ProjectState& state = c.state();
    auto result = runJob(c, "syncing by audio", [&](std::stop_token stop, const std::function<void(double)>& progress) {
        return services->sync->sync(state, angles, reference, options, stop, progress);
    });
    if (!result) {
        failWith(result.error(), "audio sync");
    }

    CommandOutput out;
    out.state = c.state();
    Json rows = Json::array();
    for (std::size_t i = 0; i < items.size(); ++i) {
        const SyncMatch& m = result->report.matches[i];
        Json row{{"synced", m.synced}, {"confidence", round3(m.confidence)}};
        if (items[i].clip) {
            row["clip"] = items[i].clip->value;
        }
        Json media = Json::array();
        for (std::size_t k = 0; k < result->order[i].size(); ++k) {
            media.push_back(Json{{"media", result->order[i][k].value}, {"start", c.timecode(result->starts[i][k])},
                                 {"startTicks", result->starts[i][k].value},
                                 {"startSeconds", std::round(result->starts[i][k].toSeconds() * 1e6) / 1e6}});
        }
        row["media"] = std::move(media);
        if (m.synced) {
            row["offsetTicks"] = result->starts[i].front().value;
            row["offsetSeconds"] = std::round(result->starts[i].front().toSeconds() * 1e6) / 1e6;
            row["overlapSeconds"] = round3(m.overlapSeconds);
            row["driftPpm"] = std::round(m.driftPpm * 100.0) / 100.0;
            row["driftSignificant"] = m.driftSignificant;
            if (m.driftSignificant) {
                row["speedProposal"] = m.speedProposal;
                out.warnings.push_back("recording " + std::to_string(i) + " drifts " + std::to_string(m.driftPpm) +
                                       " ppm against the reference: a speed of " + std::to_string(m.speedProposal) +
                                       " keeps it in sync over its length (not applied)");
            }
        } else {
            row["problem"] = m.problem;
            out.warnings.push_back("recording " + std::to_string(i) + " did not sync: " + m.problem);
        }
        rows.push_back(std::move(row));
    }
    Json data{{"reference", reference}, {"recordings", std::move(rows)}, {"cached", result->cached},
              {"seconds", round3(result->report.seconds)}};

    // Premiere's Synchronize: the other clips move to where their audio lines up with the reference clip's, and the
    // external audio (clips of media without a picture) links to the reference (dual-system sound). One undo step.
    if (c.boolean("apply", false)) {
        if (!items.front().clip) {
            fail(ErrorCode::InvalidParams, "\"apply\" moves clips: sync \"clips\", not media", Json{{"path", "/apply"}});
        }
        ProjectState next = c.state();
        const SequenceId seqId = c.sequenceId();
        const auto clipAt = [&](const ProjectState& s, ClipId id) -> const Clip& { return s.findSequence(seqId)->findClip(id)->clip(); };
        const Clip& ref = clipAt(next, *items[reference].clip);
        const Ticks refStart = ref.start();
        const Ticks refIn = items[reference].mediaIn;
        const LinkGroupId refGroup = ref.linkGroup;
        // Targets: start_i = start_ref - in_ref + in_i + offset_i (unity-speed, forward clips; `in` is the media time at
        // the clip's start, through a nested or multicam clip's time maps - such a clip moves whole, angles intact).
        std::vector<std::pair<std::size_t, Ticks>> targets;
        Ticks lowest = refStart;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (i == reference || !result->report.matches[i].synced) {
                continue;
            }
            const Clip& clip = clipAt(next, *items[i].clip);
            if (refGroup.isValid() && clip.linkGroup == refGroup) {
                continue; // already moves with the reference
            }
            if (isRetimed(clip) || isRetimed(ref) || items[i].retimed || items[reference].retimed) {
                out.warnings.push_back("clip " + std::to_string(clip.id.value) + " is retimed or reversed: not moved");
                continue;
            }
            const Ticks target = refStart - refIn + items[i].mediaIn + result->starts[i].front();
            targets.push_back({i, target});
            lowest = std::min(lowest, target);
        }
        // Nothing may start before 0: when something would, everything (the reference too) moves later together.
        const Ticks shift = lowest < Ticks{} ? Ticks{} - lowest : Ticks{};
        Json moved = Json::array();
        std::vector<LinkGroupId> movedGroups;
        if (shift.value > 0) {
            next = c.take(wabcut::edit::moveClips(next, seqId, std::span<const ClipId>(&*items[reference].clip, 1), shift)).state;
            moved.push_back(Json{{"clip", items[reference].clip->value}, {"by", c.timecode(shift)}});
        }
        for (const auto& [i, target] : targets) {
            const ClipId id = *items[i].clip;
            const LinkGroupId group = clipAt(next, id).linkGroup;
            if (group.isValid() && std::ranges::find(movedGroups, group) != movedGroups.end()) {
                continue; // its partner moved it
            }
            const Ticks delta = target + shift - clipAt(next, id).start();
            if (delta.value != 0) {
                next = c.take(wabcut::edit::moveClips(next, seqId, std::span<const ClipId>(&id, 1), delta)).state;
                moved.push_back(Json{{"clip", id.value}, {"by", c.timecode(delta)}, {"byTicks", delta.value}});
            }
            if (group.isValid()) {
                movedGroups.push_back(group);
            }
        }
        // Dual-system sound: the reference's clips and the synced clips without a picture become one link group.
        Json linked = Json::array();
        if (c.boolean("link", true) && items[reference].hasVideo) {
            std::vector<ClipId> group;
            const Sequence& seq = *next.findSequence(seqId);
            const Clip& r = clipAt(next, *items[reference].clip);
            seq.forEachTrack([&](const Track& t) {
                for (const Clip& other : t.clips) {
                    if (other.id == r.id || (r.linkGroup.isValid() && other.linkGroup == r.linkGroup)) {
                        group.push_back(other.id);
                    }
                }
            });
            for (const auto& [i, target] : targets) {
                (void)target;
                if (!items[i].hasVideo) {
                    group.push_back(*items[i].clip);
                    linked.push_back(items[i].clip->value);
                }
            }
            if (!linked.empty() && group.size() >= 2) {
                next = c.take(wabcut::edit::linkClips(next, seqId, group)).state;
            }
        }
        data["moved"] = std::move(moved);
        data["linked"] = std::move(linked);
        out.state = std::move(next);
    }
    out.data = std::move(data);
    return out;
}

void add(command::CommandRegistry& registry, command::CommandSpec spec) {
    [[maybe_unused]] const auto added = registry.add(std::move(spec));
}

} // namespace

namespace detail {

/// Where a media stream's enhanced take stands: "ready" (rendered with `model` from the file the item has, and there),
/// "stale" (another model, or another file), "missing" (none, or its file is gone). The effect plays the take only
/// when it is ready; the UI shows the others with a "render enhancement" action (audio.enhanceSpeech).
std::string takeState(const MediaItem& item, int stream, std::string_view model) {
    const AudioTake* take = item.audioTake(stream, kEnhanceSpeechTake);
    if (take == nullptr) {
        return "missing";
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(pathFromUtf8(take->path), ec)) {
        return "missing";
    }
    const bool otherFile = take->source.isKnown() && item.locator.fingerprint.isKnown() && !take->source.sameContentAs(item.locator.fingerprint);
    return (!model.empty() && take->model != model) || otherFile ? "stale" : "ready";
}

Json ensureEnhancedTakes(ExecutionContext& c, ProjectState& state, const std::vector<ClipId>& clips, SpeechEnhancer* enhancer,
                         bool render, const std::string& model) {
    const SequenceId seqId = c.sequenceId();
    // Each media stream once: render (or find) its take and record it on the media item.
    std::vector<std::pair<MediaId, int>> streams;
    for (const ClipId id : clips) {
        const Clip& clip = state.findSequence(seqId)->findClip(id)->clip();
        const std::pair<MediaId, int> key{clip.media, clip.stream.index};
        if (std::ranges::find(streams, key) == streams.end()) {
            streams.push_back(key);
        }
    }
    Json takes = Json::array();
    for (const auto& [media, stream] : streams) {
        const MediaItem* item = state.findMedia(media);
        if (item == nullptr) {
            fail(ErrorCode::NotFound, "media " + std::to_string(media.value) + " does not exist");
        }
        Json row{{"media", media.value}, {"name", item->name}, {"stream", stream}};
        const std::string before = takeState(*item, stream, model);
        if (!render || enhancer == nullptr || before == "ready") {
            row["state"] = before;
            row["rendered"] = false;
            if (const AudioTake* t = item->audioTake(stream, kEnhanceSpeechTake)) {
                row["path"] = t->path;
            }
            takes.push_back(std::move(row));
            continue;
        }
        const std::string what = "enhancing speech in '" + item->name + "'";
        c.checkpoint(what);
        const MediaItem copy = *item;
        auto result = runJob(c, what, [&](std::stop_token stop, const std::function<void(double)>& progress) {
            return enhancer->render(copy, stream, stop, progress);
        });
        if (!result) {
            failWith(result.error(), what);
        }
        const AudioTake* existing = item->audioTake(stream, kEnhanceSpeechTake);
        AudioTake take;
        take.stream = stream;
        take.kind = std::string(kEnhanceSpeechTake);
        take.path = result->path;
        take.model = result->model;
        take.source = result->source;
        take.generation = existing != nullptr ? existing->generation + 1 : 1;
        state = c.take(wabcut::edit::setAudioTake(state, media, std::move(take)));
        row["state"] = "ready";
        row["rendered"] = !result->reused;
        row["path"] = result->path;
        row["model"] = result->model;
        row["seconds"] = round3(result->seconds);
        if (!result->reused) {
            row["elapsed"] = round3(result->elapsed);
            row["realtime"] = result->elapsed > 0 ? round3(result->seconds / result->elapsed) : 0.0;
            row["device"] = result->device;
        }
        takes.push_back(std::move(row));
    }
    return takes;
}

Json duckClip(ExecutionContext& c, ProjectState& state, ClipId id, const std::string& trackLabel,
              const std::vector<DuckRegion>& regions, const DuckingOptions& options, std::optional<TimeRange> range) {
    const auto location = sequenceIn(state, c.sequenceId()).findClip(id);
    if (!location) {
        return nullptr;
    }
    const Clip& clip = location->clip();
    const auto volume = duckedVolume(clip, regions, options, range);
    if (!volume || *volume == clip.properties()[ParamId::VolumeDb]) {
        return nullptr;
    }
    // The planned volume curve (A.3): every key with its timeline time and the duck region it comes from (null: the
    // clip's own level, or a key kept outside the range), so a dry run can be inspected and drawn. Read before the edit
    // (the clip reference is into the state it replaces).
    Json keyframes = Json::array();
    std::vector<std::size_t> used;
    if (const Curve* curve = volume->curve()) {
        for (const Keyframe& k : curve->keys) {
            std::optional<Ticks> at = clip.timelineTimeOfCurveTime(k.time);
            if (!at && !clip.isRemapped() && !clip.reversed && clip.speed.isUnity()) {
                at = clip.start() + (k.time - clip.curveTimeAt(clip.start())); // the key at the clip's end edge
            }
            Json region = nullptr;
            for (std::size_t r = 0; at && r < regions.size(); ++r) {
                if (*at >= regions[r].downStart && *at <= regions[r].upEnd) {
                    region = r;
                    if (std::find(used.begin(), used.end(), r) == used.end()) {
                        used.push_back(r);
                    }
                    break;
                }
            }
            keyframes.push_back(Json{{"time", at ? Json(c.timecode(*at)) : Json(nullptr)},
                                     {"ticks", at ? Json(at->value) : Json(nullptr)},
                                     {"curveTicks", k.time.value},
                                     {"db", k.value},
                                     {"interpolation", std::string(toString(k.interpolation))},
                                     {"region", region}});
        }
    }
    std::sort(used.begin(), used.end());
    const std::size_t keys = volume->curve() != nullptr ? volume->curve()->keys.size() : 0;
    state = c.take(wabcut::edit::setParameter(state, c.sequenceId(), id, ParamRef::of(ParamId::VolumeDb), *volume)).state;
    return Json{{"clip", id.value}, {"track", trackLabel}, {"keys", keys}, {"keyframes", std::move(keyframes)}, {"regions", used}};
}

Json duckRegionsJson(ExecutionContext& c, const std::vector<DuckRegion>& regions) {
    Json duck = Json::array();
    for (const DuckRegion& r : regions) {
        duck.push_back(Json{{"speech", Json::array({c.timecode(r.speech.start), c.timecode(r.speech.end())})},
                            {"down", c.timecode(r.downStart)},
                            {"up", c.timecode(r.upEnd)},
                            {"upStart", c.timecode(r.upStart)},
                            {"ticks", Json{{"speech", Json::array({r.speech.start.value, r.speech.end().value})},
                                           {"down", r.downStart.value},
                                           {"upStart", r.upStart.value},
                                           {"up", r.upEnd.value}}}});
    }
    return duck;
}

} // namespace detail

void registerAudioCommands(command::CommandRegistry& registry) {
    std::vector<std::string> presets;
    for (const LoudnessPreset& p : loudnessPresets()) {
        presets.emplace_back(p.id);
    }
    const Json rangeFrom = timeParam("Only from this time (default: the start).");
    const Json rangeTo = timeParam("Only up to this time (default: the end).");

    add(registry, {"track.setRole", 1, "Set Track Role",
                   "What an audio track carries: dialogue, music or effects (Premiere's Essential Sound types), or none. "
                   "audio.autoDuck lowers music-role tracks under dialogue-role tracks.",
                   Capability::EditSequence,
                   withSequence({{"track", trackParam("The audio track.")},
                                 {"role", schema::enumeration({"none", "dialogue", "music", "effects"}, "The role.")}},
                                {"track", "role"}),
                   Json{{"track", "A1"}, {"role", "dialogue"}}, {}, setRole});
    const Json faderTime = timeParam("Put a keyframe here instead of setting a constant (the fader becomes animated).");
    const Json faderInterpolation = schema::enumeration({"hold", "linear", "bezier", "easeIn", "easeOut", "easeInOut"},
                                                        "The keyframe's interpolation. Default linear.");
    add(registry, {"track.setVolume", 1, "Set Track Volume",
                   "Sets an audio track's fader (Premiere's track Volume): dB, 0 = unity, applied to the track's mix after its "
                   "effects. With \"time\" it sets a keyframe there (track automation, in timeline time) instead of a "
                   "constant; a constant replaces the track's volume keyframes. One undo step.",
                   Capability::EditSequence,
                   withSequence({{"track", trackParam("The audio track.")},
                                 {"db", schema::number("The level in dB (-144 = silence .. +24).", -144.0, 24.0)},
                                 {"time", faderTime},
                                 {"interpolation", faderInterpolation}},
                                {"track", "db"}),
                   Json{{"track", "A2"}, {"db", -6}}, {}, setTrackVolume});
    add(registry, {"track.setPan", 1, "Set Track Pan",
                   "Sets an audio track's panner: -1 = left .. 0 = centre .. +1 = right, a balance on the track's stereo "
                   "mix (centre leaves it unchanged; towards a side the other side follows the -3 dB constant-power curve "
                   "down to silence). With \"time\" it sets a keyframe there. One undo step.",
                   Capability::EditSequence,
                   withSequence({{"track", trackParam("The audio track.")},
                                 {"pan", schema::number("The balance, -1 .. +1.", -1.0, 1.0)},
                                 {"time", faderTime},
                                 {"interpolation", faderInterpolation}},
                                {"track", "pan"}),
                   Json{{"track", "A3"}, {"pan", -0.3}}, {}, setTrackPan});
    add(registry, {"sequence.setMasterGain", 1, "Set Master Gain",
                   "Sets the master fader: dB, 0 = unity, applied to the sequence's mix after the master effects and before "
                   "the meters (what export renders). With \"time\" it sets a keyframe there. One undo step.",
                   Capability::EditSequence,
                   withSequence({{"db", schema::number("The level in dB (-144 = silence .. +24).", -144.0, 24.0)},
                                 {"time", faderTime},
                                 {"interpolation", faderInterpolation}},
                                {"db"}),
                   Json{{"db", -1.5}}, {}, setMasterGain});
    add(registry, {"audio.effects", 1, "Audio Effect Stack",
                   "The effect stack of an audio track or of the master audio bus: instance ids, effect ids, enabled, "
                   "parameter values (constants or keyframes in timeline ticks), and the fader after it: \"volume\" (dB) "
                   "and, for tracks, \"pan\".",
                   Capability::Read, withSequence({{"target", targetParam()}}, {"target"}), Json{{"target", "master"}}, {},
                   effects});
    add(registry, {"audio.addEffect", 1, "Add Track or Master Effect",
                   "Adds an audio effect to an audio track's stack (applied to the track's mix, before its fader) or to "
                   "the master bus (applied to the sequence's mix, before the master fader). Parameters not given take "
                   "their defaults. An audio plugin (\"plugin\": its effect id from plugins.list) goes in the same way; its "
                   "parameters are \"p<id>\" keys. The new instance id is returned in createdIds.",
                   Capability::EditSequence,
                   withSequence({{"target", targetParam()},
                                 {"effect", audioEffectIds()},
                                 {"plugin", schema::string("Or an audio plugin's effect id (plugins.list), e.g. \"plugin.vst3.<id>\".")},
                                 {"params", paramsObject("Constant parameter values by key, e.g. {\"ceiling\": -1}.")},
                                 {"index", schema::integer("Position in the stack (0 = first). Default: last.", 0)}},
                                {"target"}),
                   Json{{"target", "A1"}, {"effect", "wabcut.audio.compressor"}, {"params", Json{{"threshold", -24}}}}, {},
                   addEffect});
    add(registry, {"audio.setEffect", 1, "Set Track or Master Effect",
                   "Enables or disables an effect of a track or master stack, or sets constant parameters.",
                   Capability::EditSequence,
                   withSequence({{"target", targetParam()},
                                 {"effect", effectInstanceParam("The effect.")},
                                 {"enabled", schema::boolean("On or bypassed.")},
                                 {"params", paramsObject("Parameter values by key.")}},
                                {"target", "effect"}),
                   Json{{"target", "master"}, {"effect", 77}, {"params", Json{{"ceiling", -2}}}}, {}, setEffect});
    add(registry, {"audio.removeEffect", 1, "Remove Track or Master Effect",
                   "Removes an effect from a track or master stack.", Capability::EditSequence,
                   withSequence({{"target", targetParam()}, {"effect", effectInstanceParam("The effect.")}}, {"target", "effect"}),
                   Json{{"target", "A1"}, {"effect", 77}}, {}, removeEffect});
    add(registry, {"audio.moveEffect", 1, "Reorder Track or Master Effect",
                   "Moves an effect to another position in a track or master stack.", Capability::EditSequence,
                   withSequence({{"target", targetParam()},
                                 {"effect", effectInstanceParam("The effect.")},
                                 {"index", schema::integer("New position (0 = first).", 0)}},
                                {"target", "effect", "index"}),
                   Json{{"target", "master"}, {"effect", 77}, {"index", 0}}, {}, moveEffect});
    add(registry, {"audio.addKeyframe", 1, "Add Track or Master Keyframe",
                   "Adds a keyframe to a parameter of a track or master effect, or (no \"effect\") to the fader: "
                   "\"volume\" or \"pan\" - at a timeline time (track and master curves run in timeline time). A "
                   "constant parameter becomes a curve.",
                   Capability::EditSequence,
                   withSequence({{"target", targetParam()},
                                 {"effect", effectInstanceParam("The effect; omit it for the fader.")},
                                 {"param", schema::string("The parameter key (effect.list), or the fader's \"volume\" / \"pan\".")},
                                 {"time", timeParam("Where the key goes.")},
                                 {"value", schema::number("The value, in the parameter's unit.")},
                                 {"interpolation", schema::enumeration({"hold", "linear", "bezier", "easeIn", "easeOut", "easeInOut"},
                                                                       "Default linear.")}},
                                {"target", "param", "time", "value"}),
                   Json{{"target", "A2"}, {"param", "volume"}, {"time", "00:00:10:00"}, {"value", -18}}, {}, addKeyframe});
    add(registry, {"audio.removeKeyframe", 1, "Remove Track or Master Keyframe",
                   "Removes keyframe `index` of a track or master effect parameter or of the fader (\"volume\" / \"pan\", "
                   "no \"effect\"); the last one leaves a constant.",
                   Capability::EditSequence,
                   withSequence({{"target", targetParam()},
                                 {"effect", effectInstanceParam("The effect; omit it for the fader.")},
                                 {"param", schema::string("The parameter key, or \"volume\" / \"pan\".")},
                                 {"index", schema::integer("Keyframe index (0 = first; audio.effects lists them).", 0)}},
                                {"target", "param", "index"}),
                   Json{{"target", "master"}, {"effect", 77}, {"param", "ceiling"}, {"index", 0}}, {}, removeKeyframe});
    add(registry, {"audio.moveKeyframe", 1, "Move Track or Master Keyframe",
                   "Moves keyframe `index` of a track or master effect parameter or of the fader (\"volume\" / \"pan\", "
                   "no \"effect\") to another timeline time; fails if another key is there.",
                   Capability::EditSequence,
                   withSequence({{"target", targetParam()},
                                 {"effect", effectInstanceParam("The effect; omit it for the fader.")},
                                 {"param", schema::string("The parameter key, or \"volume\" / \"pan\".")},
                                 {"index", schema::integer("Keyframe index (0 = first).", 0)},
                                 {"time", timeParam("The key's new time.")}},
                                {"target", "param", "index", "time"}),
                   Json{{"target", "A2"}, {"param", "volume"}, {"index", 1}, {"time", "00:00:12:00"}}, {}, moveKeyframe});
    add(registry, {"audio.loudness", 1, "Measure Loudness",
                   "EBU R128 / ITU-R BS.1770 loudness as it renders: integrated LUFS, loudness range (LU), true peak "
                   "(dBTP), sample peak, loudest momentary and short-term LUFS. Targets: the mix (with mute/solo and the "
                   "master effects: what export writes), a track (its clips and track effects) or a clip on its own.",
                   Capability::MediaAnalysis,
                   withSequence({{"targets", schema::array(Json{{"type", Json::array({"string", "object"})},
                                                               {"description", "\"mix\", {\"clip\": id} or {\"track\": \"A1\"}."}},
                                                          "What to measure (default: the mix).", 1)},
                                 {"from", rangeFrom},
                                 {"to", rangeTo}}),
                   Json{{"targets", Json::array({"mix", Json{{"track", "A1"}}})}}, {}, loudness});
    add(registry, {"audio.normalize", 1, "Normalize Loudness",
                   "Sets a gain so the loudness hits a delivery target: YouTube / Spotify -14 LUFS, podcasts and Apple "
                   "Podcasts -16, broadcast EBU R128 -23, ATSC -24, or a custom \"lufs\". The gain is the first effect of "
                   "each clip's, the track's or the master's stack (adjusted when it is already there), measured again and "
                   "refined until within tolerance, so compressors and limiters after it are accounted for. With \"limit\" "
                   "the master gets the true-peak limiter at the preset's ceiling first. One undo step.",
                   Capability::EditSequence,
                   withSequence({{"preset", schema::enumeration(presets, "The delivery target (default youtube).")},
                                 {"lufs", schema::number("A custom integrated loudness target instead of a preset.", -70.0, 0.0)},
                                 {"truePeak", schema::number("True-peak ceiling in dBTP (default: the preset's, -1).", -24.0, 0.0)},
                                 {"clips", schema::array(Json{{"type", Json::array({"integer", "object"})}}, "Normalize each of these clips.", 1)},
                                 {"track", trackParam("Normalize this track.")},
                                 {"limit", schema::boolean("Add or update the master true-peak limiter (mix only).")},
                                 {"tolerance", schema::number("Allowed error in LU (default 0.1).", 0.01, 3.0)},
                                 {"maxPasses", schema::integer("Most measure-and-adjust passes (default 4).", 1, 10)},
                                 {"from", rangeFrom},
                                 {"to", rangeTo}}),
                   Json{{"preset", "podcast"}, {"limit", true}}, {}, normalize});
    add(registry, {"audio.masterLimiter", 1, "Master True-Peak Limiter",
                   "Puts a true-peak brick-wall limiter at the end of the master audio bus (or updates the one there): no "
                   "true peak of the mix goes above the ceiling (default -1 dBTP, the streaming and EBU R128 maximum).",
                   Capability::EditSequence,
                   withSequence({{"ceiling", schema::number("Ceiling in dBTP (default -1).", -24.0, 0.0)},
                                 {"release", schema::number("Release in ms (default 50).", 1.0, 1000.0)},
                                 {"lookahead", schema::number("Lookahead in ms (default 5).", 0.0, 20.0)}}),
                   Json{{"ceiling", -1}}, {}, masterLimiter});
    add(registry, {"audio.autoDuck", 1, "Auto-Duck Music",
                   "Lowers music under speech: finds speech on the dialogue tracks (transcript words, or the audio level "
                   "where there is no transcript) and writes volume keyframes on the music tracks' clips - down by "
                   "\"amount\" over \"attack\" before each phrase, back up over \"release\" after \"hold\", bridging short "
                   "pauses. Replaces the music clips' volume keyframes in the range (like Premiere's Auto Ducking); the keys "
                   "stay editable. \"preset\": \"gameUnderVoice\" ducks a gaming recording's game audio and music under the "
                   "streamer's mic: the tracks named Mic (else the dialogue-role ones) are the speech, the tracks named Game "
                   "or Desktop Audio (else the effects-role ones) and the music tracks duck, -10 dB with a 0.12 s attack, "
                   "0.35 s hold and 0.8 s release; given parameters still win. The report names the tracks used. One undo "
                   "step; dryRun previews it.",
                   Capability::EditSequence,
                   withSequence({{"preset", schema::enumeration({"default", "gameUnderVoice"},
                                                                "The timing and tracks to start from (default: default).")},
                                 {"dialogue", schema::array(trackParam("A track."), "Speech tracks (default: the dialogue-role tracks).", 1)},
                                 {"music", schema::array(trackParam("A track."), "Tracks to duck (default: the music-role tracks).", 1)},
                                 {"includeMusic", schema::boolean("gameUnderVoice: the music tracks duck with the game (default true).")},
                                 {"amount", schema::number("How far the music goes down, dB (default -12).", -60.0, 0.0)},
                                 {"attack", secondsParam("Seconds to go down, ending where speech starts (default 0.25).", 0.0, 10.0)},
                                 {"release", secondsParam("Seconds to come back up (default 0.75).", 0.0, 10.0)},
                                 {"hold", secondsParam("Seconds to stay down after speech (default 0.25).", 0.0, 10.0)},
                                 {"minSpeech", secondsParam("Ignore speech shorter than this (default 0.12).", 0.0, 5.0)},
                                 {"source", schema::enumeration({"auto", "transcript", "energy"},
                                                                "Where speech is found (default auto: transcripts, else the audio level).")},
                                 {"from", rangeFrom},
                                 {"to", rangeTo}}),
                   Json{{"amount", -15}, {"release", 1.0}}, {}, autoDuck});
    add(registry, command::runsModel({"audio.enhanceSpeech", 1, "Enhance Speech",
                   "Studio-quality dialogue (Premiere's Enhance Speech, Resolve's Voice Isolation): renders each clip's media "
                   "with DeepFilterNet3 on this computer (no audio leaves it; the model downloads on first use), records the "
                   "enhanced take on the media item and puts the Enhance Speech effect first in each clip's stack: the clip "
                   "plays the take blended with the original by \"strength\" (toggle the effect to compare), before its "
                   "level and redactions. A take already rendered for the same file and model is reused. Video clips stand "
                   "for their linked audio. With \"render\": false only the effect is applied (it plays the original until "
                   "the take is rendered). Records the take on the media item, so it is a project edit. One undo step.",
                   Capability::EditProject,
                   withSequence({{"clips", schema::array(Json{{"type", Json::array({"integer", "object"})}}, "The clips to enhance.", 1)},
                                 {"strength", schema::number("Mix of the enhanced take, percent (default 100).", 0.0, 100.0)},
                                 {"render", schema::boolean("Render missing takes (default true).")}},
                                {"clips"}),
                   Json{{"clips", Json::array({12, 15})}, {"strength", 80}}, {}, enhanceSpeech}));
    add(registry, command::runsModel({"audio.separateStems", 1, "Separate Stems",
                   "Splits a music or mixed clip's audio into vocals, drums, bass and other with HTDemucs on this computer "
                   "(no audio leaves it; the model downloads on first use, and the job refuses on a computer with too little "
                   "free memory). Each stem becomes a new media item, linked to the source by its provenance (\"separated "
                   "from X with htdemucs\"), in a bin named after the source. One undo step.",
                   Capability::EditProject,
                   withSequence({{"clip", schema::integer("The clip to separate (video clips stand for their linked audio).", 1)},
                                 {"media", schema::integer("Or a media item.", 1)},
                                 {"stream", schema::integer("Its audio stream (with \"media\"; default 0).", 0)},
                                 {"model", schema::enumeration({"htdemucs"}, "The separation model (default htdemucs).")}}),
                   Json{{"clip", 21}}, {}, separateStems}));
    add(registry, {"audio.syncClips", 1, "Synchronize by Audio",
                   "Lines recordings of one event up by their audio: a camera's scratch audio and a separate recorder "
                   "(dual-system sound), or several cameras. Returns each recording's offset to the reference (the one "
                   "named, else the first clip with a picture), its confidence, the overlap and the clock drift between the "
                   "devices (ppm, with the speed that would cancel it). \"media\" entries may be arrays: a camera's recording "
                   "split into several files, played back to back in the order they were recorded. With clips and "
                   "\"apply\" (Premiere's Synchronize), the other clips move to where they line up with the reference, and "
                   "the external audio (clips without a picture) links to it; one undo step, dryRun previews it.",
                   Capability::EditSequence,
                   withSequence({{"clips", schema::array(Json{{"type", Json::array({"integer", "object"})}}, "Clips to sync.", 2)},
                                 {"media", schema::array(Json{{"type", Json::array({"integer", "string", "array"})}},
                                                         "Media items to sync (an array: one recording in several files).", 2)},
                                 {"reference", schema::integer("The clip or media id the others are synced to.", 1)},
                                 {"stream", schema::integer("The audio stream of each media item (with \"media\"; default 0).", 0)},
                                 {"apply", schema::boolean("Move the clips into sync (with \"clips\").")},
                                 {"link", schema::boolean("With \"apply\": link the external audio to the reference (default true).")},
                                 {"minConfidence", schema::number("Least confidence that syncs (0..1, default 0.5).", 0.0, 1.0)},
                                 {"minOverlap", secondsParam("Least common audio in seconds (default 5).", 0.5, 3600.0)}}),
                   Json{{"clips", Json::array({12, 18})}, {"apply", true}}, {}, syncClips});
    registerPluginCommands(registry); // A.6: plugins.list, plugins.scan
    registerEssentialSoundCommands(registry); // A.7: audio.essentialSound, audio.remix, audio.remixRender
    registerVoiceOverCommands(registry);      // A.8: track.setRecordArm, record.takes, record.useTake
    registerChannelCommands(registry);        // A.9: audio channels, split to mono, track formats, 5.1
    registerSubmixCommands(registry);         // p02 audio: submix tracks, track outputs and sends
    registerAutoTagCommands(registry);        // p02 audio: audio.autoTag
}

} // namespace wabcut::audio::edit
