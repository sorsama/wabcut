// audio.essentialSound, audio.remix and audio.remixRender (A.7, docs/ai.md "Essential Sound" and "Music Remix"):
// Premiere's Essential Sound panel and its Remix, as commands. Each is one undo step (a staged edit for the assistant)
// that dryRun previews. audio.essentialSound and audio.remix change the sequence only (EditSequence); audio.remixRender
// adds a media item, the rendered take, so it is an EditProject command (A.8).

#include "AudioCommandHelpers.h"

#include <wabcut/audio/edit/EssentialSound.h>
#include <wabcut/audio/edit/Remix.h>

#include <wabcut/model/EditOps.h>
#include <wabcut/model/RedactionMedia.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace wabcut::audio::edit {

using namespace detail;

namespace {

constexpr std::string_view kJoinTransition = "constantGain"; // the pieces are alike: their sum stays level

// ---- Options -------------------------------------------------------------------------------------------------------

std::optional<double> numberAt(const Json& o, const char* key, double lo, double hi, const std::string& path) {
    if (!o.is_object() || !o.contains(key)) {
        return std::nullopt;
    }
    const Json& v = o[key];
    if (!v.is_number() || !(v.get<double>() >= lo && v.get<double>() <= hi)) {
        fail(ErrorCode::InvalidParams, path + "/" + key + " must be a number from " + std::to_string(lo) + " to " + std::to_string(hi),
             Json{{"path", path + "/" + key}});
    }
    return v.get<double>();
}

std::optional<bool> booleanAt(const Json& o, const char* key, const std::string& path) {
    if (!o.is_object() || !o.contains(key)) {
        return std::nullopt;
    }
    if (!o[key].is_boolean()) {
        fail(ErrorCode::InvalidParams, path + "/" + key + " must be true or false", Json{{"path", path + "/" + key}});
    }
    return o[key].get<bool>();
}

std::optional<std::string> choiceAt(const Json& o, const char* key, std::span<const std::string_view> allowed, const std::string& path) {
    if (!o.is_object() || !o.contains(key)) {
        return std::nullopt;
    }
    const Json& v = o[key];
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        if (s == "none" || s.empty()) {
            return std::string();
        }
        if (std::ranges::find(allowed, std::string_view(s)) != allowed.end()) {
            return s;
        }
    }
    std::string list = "none";
    for (const std::string_view a : allowed) {
        list += ", " + std::string(a);
    }
    fail(ErrorCode::InvalidParams, path + "/" + key + " is one of: " + list, Json{{"path", path + "/" + key}});
}

const Json& objectAt(const Json& o, const char* key, const std::string& path) {
    static const Json kNone;
    if (!o.is_object() || !o.contains(key)) {
        return kNone;
    }
    if (!o[key].is_object()) {
        fail(ErrorCode::InvalidParams, path + "/" + key + " must be an object", Json{{"path", path + "/" + key}});
    }
    return o[key];
}

/// The preset's options with a request's "options" over them.
EssentialSoundOptions optionsFrom(const Json& o, EssentialSoundOptions base) {
    if (o.is_null()) {
        return base;
    }
    if (!o.is_object()) {
        fail(ErrorCode::InvalidParams, "options must be an object", Json{{"path", "/options"}});
    }
    const std::string root = "/options";
    if (auto v = booleanAt(o, "loudness", root)) {
        base.loudness = *v;
    }
    if (auto v = numberAt(o, "targetLufs", -60.0, 0.0, root)) {
        base.targetLufs = *v;
    }
    const Json& repair = objectAt(o, "repair", root);
    const std::string rp = root + "/repair";
    if (auto v = numberAt(repair, "noise", 0.0, 1.0, rp)) {
        base.reduceNoise = *v;
    }
    if (auto v = booleanAt(repair, "enhance", rp)) {
        base.enhanceSpeech = *v;
    }
    if (auto v = numberAt(repair, "rumble", 0.0, 500.0, rp)) {
        if (*v > 0.0 && *v < 20.0) {
            fail(ErrorCode::InvalidParams, "repair.rumble is 0 (off) or a frequency from 20 Hz", Json{{"path", rp + "/rumble"}});
        }
        base.rumbleHz = *v;
    }
    if (auto v = numberAt(repair, "hum", 0.0, 70.0, rp)) {
        if (*v > 0.0 && *v < 40.0) {
            fail(ErrorCode::InvalidParams, "repair.hum is 0 (off), 50 or 60 (40..70 Hz)", Json{{"path", rp + "/hum"}});
        }
        base.dehumHz = *v;
    }
    if (auto v = numberAt(repair, "gate", 0.0, 1.0, rp)) {
        base.gate = *v;
    }
    if (auto v = numberAt(repair, "deess", 0.0, 1.0, rp)) {
        base.deess = *v;
    }
    if (auto v = numberAt(repair, "reverb", 0.0, 1.0, rp)) {
        base.reduceReverb = *v;
    }
    const Json& clarity = objectAt(o, "clarity", root);
    const std::string cp = root + "/clarity";
    if (auto v = numberAt(clarity, "dynamics", 0.0, 1.0, cp)) {
        base.dynamics = *v;
    }
    if (auto v = choiceAt(clarity, "eq", clarityEqPresets(), cp)) {
        base.eq = *v;
    }
    if (auto v = numberAt(clarity, "eqAmount", 0.0, 1.0, cp)) {
        base.eqAmount = *v;
    }
    if (auto v = booleanAt(clarity, "makeup", cp)) {
        base.dynamicsMakeup = *v;
    }
    if (auto v = booleanAt(clarity, "limiter", cp)) {
        base.limiter = *v;
    }
    if (auto v = numberAt(clarity, "limiterCeiling", -24.0, 0.0, cp)) {
        base.limiterCeiling = *v;
    }
    const Json& creative = objectAt(o, "creative", root);
    const std::string crp = root + "/creative";
    if (auto v = choiceAt(creative, "reverb", reverbPresets(), crp)) {
        base.reverb = *v;
    }
    if (auto v = numberAt(creative, "amount", 0.0, 1.0, crp)) {
        base.reverbAmount = *v;
    }
    if (o.contains("ducking")) {
        const Json& d = o["ducking"];
        if (d.is_boolean()) {
            base.duck = d.get<bool>();
        } else {
            const Json& dd = objectAt(o, "ducking", root);
            const std::string dp = root + "/ducking";
            base.duck = booleanAt(dd, "enabled", dp).value_or(true);
            if (auto v = numberAt(dd, "amount", -60.0, 0.0, dp)) {
                base.ducking.amountDb = *v;
            }
            if (auto v = numberAt(dd, "attack", 0.0, 10.0, dp)) {
                base.ducking.attack = seconds(*v);
            }
            if (auto v = numberAt(dd, "release", 0.0, 10.0, dp)) {
                base.ducking.release = seconds(*v);
            }
            if (auto v = numberAt(dd, "hold", 0.0, 10.0, dp)) {
                base.ducking.hold = seconds(*v);
            }
        }
    }
    if (auto v = numberAt(o, "duration", 0.1, 36000.0, root)) {
        base.durationSeconds = *v;
    }
    if (auto v = numberAt(o, "width", 0.0, 2.0, root)) {
        base.width = *v;
    }
    if (auto v = numberAt(o, "pan", -1.0, 1.0, root)) {
        base.pan = *v;
    }
    return base;
}

/// What the record keeps of the options (and what the report shows): the settings that are on.
Json optionsJson(const EssentialSoundOptions& o) {
    Json j{{"loudness", o.loudness}};
    if (o.loudness) {
        j["targetLufs"] = o.targetLufs;
    }
    Json repair = Json::object();
    if (o.reduceNoise > 0.0) {
        repair["noise"] = o.reduceNoise;
    }
    if (o.enhanceSpeech) {
        repair["enhance"] = true;
    }
    if (o.rumbleHz > 0.0) {
        repair["rumble"] = o.rumbleHz;
    }
    if (o.dehumHz > 0.0) {
        repair["hum"] = o.dehumHz;
    }
    if (o.gate > 0.0) {
        repair["gate"] = o.gate;
    }
    if (o.deess > 0.0) {
        repair["deess"] = o.deess;
    }
    if (o.reduceReverb > 0.0) {
        repair["reverb"] = o.reduceReverb;
    }
    if (!repair.empty()) {
        j["repair"] = std::move(repair);
    }
    Json clarity = Json::object();
    if (o.dynamics > 0.0) {
        clarity["dynamics"] = o.dynamics;
        if (o.dynamicsMakeup) {
            clarity["makeup"] = true;
        }
    }
    if (!o.eq.empty()) {
        clarity["eq"] = o.eq;
        clarity["eqAmount"] = o.eqAmount;
    }
    if (o.limiter) {
        clarity["limiter"] = true;
        clarity["limiterCeiling"] = o.limiterCeiling;
    }
    if (!clarity.empty()) {
        j["clarity"] = std::move(clarity);
    }
    if (!o.reverb.empty()) {
        Json creative{{"reverb", o.reverb}};
        if (o.reverbAmount >= 0.0) {
            creative["amount"] = o.reverbAmount;
        }
        j["creative"] = std::move(creative);
    }
    if (o.duck) {
        j["ducking"] = Json{{"amount", o.ducking.amountDb},
                            {"attack", round3(o.ducking.attack.toSeconds())},
                            {"release", round3(o.ducking.release.toSeconds())},
                            {"hold", round3(o.ducking.hold.toSeconds())}};
    }
    if (o.durationSeconds) {
        j["duration"] = *o.durationSeconds;
    }
    if (std::fabs(o.width - 1.0) > 1e-9) {
        j["width"] = o.width;
    }
    if (o.pan) {
        j["pan"] = *o.pan;
    }
    return j;
}

std::vector<std::string> allPresetIds() {
    std::vector<std::string> ids;
    for (const SoundType t : {SoundType::Dialogue, SoundType::Music, SoundType::Sfx, SoundType::Ambience}) {
        for (const EssentialSoundPreset& p : essentialSoundPresets(t)) {
            if (std::ranges::find(ids, std::string(p.id)) == ids.end()) {
                ids.emplace_back(p.id);
            }
        }
    }
    return ids;
}

std::string presetList(SoundType type) {
    std::string out;
    for (const EssentialSoundPreset& p : essentialSoundPresets(type)) {
        out += (out.empty() ? "" : ", ") + std::string(p.id);
    }
    return out;
}

// ---- Clips ---------------------------------------------------------------------------------------------------------

const Clip& clipIn(const ProjectState& state, SequenceId seq, ClipId id) { return sequenceIn(state, seq).findClip(id)->clip(); }

/// The clips a request names: "clips" (video clips stand for their linked audio), then every clip on "tracks" (A.10: one
/// click on the mic track of a recording), each once.
std::vector<ClipId> requestedClips(ExecutionContext& c) {
    std::vector<ClipId> out = audioClipsOf(c, c.clips("clips"));
    const std::vector<TrackId> tracks = c.tracks("tracks");
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const Track* track = c.sequence().findTrack(tracks[i]);
        const std::string path = "/tracks/" + std::to_string(i);
        if (track == nullptr || track->kind != TrackKind::Audio) {
            fail(ErrorCode::InvalidParams, "the tracks are audio tracks", Json{{"path", path}});
        }
        if (track->clips.empty()) {
            fail(ErrorCode::InvalidParams, c.trackLabel(tracks[i]) + " has no clips", Json{{"path", path}});
        }
        for (const Clip& clip : track->clips) {
            if (std::ranges::find(out, clip.id) == out.end()) {
                out.push_back(clip.id);
            }
        }
    }
    if (out.empty()) {
        fail(ErrorCode::InvalidParams, "give the \"clips\" or the \"tracks\" to apply Essential Sound to", Json{{"path", "/clips"}});
    }
    return out;
}

const Track& trackOf(const ProjectState& state, SequenceId seq, ClipId id) { return *sequenceIn(state, seq).findClip(id)->track; }

/// Puts the Essential Sound record and the effects `options` build at the top of a clip's stack, replacing the ones an
/// earlier application left (the clip's other effects stay, after them). The created effects' report.
Json applyStack(ExecutionContext& c, ProjectState& state, ClipId id, SoundType type, std::string_view preset,
                const EssentialSoundOptions& options, CommandOutput& out) {
    const SequenceId seq = c.sequenceId();
    std::vector<EffectInstanceId> old;
    for (const EffectInstance& e : clipIn(state, seq, id).effects()) {
        if (isEssentialSoundEffect(e)) {
            old.push_back(e.id);
        }
    }
    for (const EffectInstanceId e : old) {
        state = c.take(wabcut::edit::removeEffect(state, seq, id, e)).state;
    }
    const auto addAt = [&](const std::string& effect, std::vector<EffectParam> params, std::size_t index, std::vector<EffectData> data) {
        auto added = c.take(wabcut::edit::addEffect(state, seq, std::span<const ClipId>(&id, 1), effect, std::move(params), index));
        const EffectInstanceId created = added.createdEffects.front();
        state = std::move(added.state);
        wabcut::edit::EffectChange change;
        change.data = std::move(data);
        state = c.take(wabcut::edit::setEffect(state, seq, id, created, change)).state;
        out.createdIds.push_back(created.value);
        return created;
    };
    Json effects = Json::array();
    const EffectInstanceId record =
        addAt(std::string(kEssentialSoundEffect), {}, 0, essentialSoundRecord(type, preset, optionsJson(options).dump()));
    effects.push_back(Json{{"id", record.value}, {"effect", std::string(kEssentialSoundEffect)}, {"section", "record"}});
    std::size_t index = 1;
    for (BuiltEffect& b : buildEssentialSoundStack(type, options)) {
        const EffectInstanceId e = addAt(b.effect, std::move(b.params), index++, {EffectData{std::string(kSectionKey), b.section}});
        effects.push_back(Json{{"id", e.value}, {"effect", b.effect}, {"section", b.section}});
    }
    return effects;
}

/// Removes the Essential Sound record and the effects it built from a clip; how many went.
std::size_t clearStack(ExecutionContext& c, ProjectState& state, ClipId id) {
    std::vector<EffectInstanceId> old;
    for (const EffectInstance& e : clipIn(state, c.sequenceId(), id).effects()) {
        if (isEssentialSoundEffect(e)) {
            old.push_back(e.id);
        }
    }
    for (const EffectInstanceId e : old) {
        state = c.take(wabcut::edit::removeEffect(state, c.sequenceId(), id, e)).state;
    }
    return old.size();
}

// ---- Remix -------------------------------------------------------------------------------------------------------

struct RemixRequest {
    Ticks target;
    bool keepIntro = true;
    bool keepOutro = true;
    Ticks crossfade{-1};
    bool take = false;
};

struct RemixOutcome {
    std::vector<ClipId> clips; ///< the clips the music plays as now, in order
    Json report;
};

Json secondsJson(Ticks t) { return round3(t.toSeconds()); }

/// Remixes the music clip `id` of `state` to `request.target`: its pieces put on its track in its place (the first
/// keeps its id; constant-gain transitions on the joins), or with `request.take` one clip of a rendered take.
RemixOutcome remixClip(ExecutionContext& c, ProjectState& state, ClipId id, const RemixRequest& request, CommandOutput& out) {
    const SequenceId seqId = c.sequenceId();
    const auto location = sequenceIn(state, seqId).findClip(id);
    if (!location) {
        fail(ErrorCode::NotFound, "no clip " + std::to_string(id.value) + " in the sequence");
    }
    // Copies: `state` changes below, and what `location` points into goes with the state it replaces.
    const Clip clip = location->clip();
    const Track track = *location->track;
    const std::size_t index = location->index;
    const std::string label = c.trackLabel(track.id);
    if (clip.stream.kind != StreamKind::Audio || clip.isNested() || !clip.media.isValid()) {
        fail(ErrorCode::InvalidParams, "clip " + std::to_string(id.value) + " is not a music clip of a media file", Json{{"path", "/clip"}});
    }
    if (clip.isRemapped() || clip.reversed || !clip.speed.isUnity()) {
        fail(ErrorCode::InvalidParams, "clip " + std::to_string(id.value) + " plays at another speed, reversed or remapped: remix it at 100 %",
             Json{{"path", "/clip"}});
    }
    if (clip.linkGroup.isValid()) {
        fail(ErrorCode::EditRejected, "clip " + std::to_string(id.value) + " is linked to other clips: unlink it to remix its music");
    }
    if (track.locked) {
        fail(ErrorCode::EditRejected, label + " is locked");
    }
    auto* services = command::findService<AudioServices>(c.services());
    if (services == nullptr || !services->music) {
        fail(ErrorCode::InvalidRequest, "no Music Remix here: the host has no music analysis (the app and wabcut-cli do)");
    }
    const MediaItem* item = state.findMedia(clip.media);
    if (item == nullptr) {
        fail(ErrorCode::NotFound, "media " + std::to_string(clip.media.value) + " does not exist");
    }
    // A rendered remix take is new media that rearranges the music in time: the media times of a mute or bleep mean
    // nothing in it, so it would play what the redaction hides (review wave 5 A5-P1-3). Refused, never rendered
    // unredacted. (Without a take the pieces are clips of the same media: its redactions apply to them as to any clip.)
    if (request.take) {
        const TimeRange used{clip.sourceIn, clip.duration()};
        for (const Redaction* r : redactionsCovering(state, clip.media)) {
            const bool hit = r->enabled && r->audio != RedactionAudio::None && r->appliesTo(clip.id) &&
                             (r->audioRanges.empty() ? r->source.overlaps(used)
                                                     : std::ranges::any_of(r->audioRanges, [&](const TimeRange& w) { return w.overlaps(used); }));
            if (hit) {
                fail(ErrorCode::EditRejected, "clip " + std::to_string(id.value) + " plays muted or bleeped audio (redaction \"" +
                                                  r->name + "\"): a rendered remix would rearrange it unredacted; remix without a take, or a stretch without redactions");
            }
        }
    }
    const MediaItem media = *item;
    const std::string what = "analysing the music of '" + media.name + "'";
    c.checkpoint(what);
    const auto analysisStart = std::chrono::steady_clock::now();
    auto analysis = runJob(c, what, [&](std::stop_token stop, const std::function<void(double)>& progress) {
        return services->music->analyze(media, clip.stream.index, stop, progress);
    });
    if (!analysis) {
        failWith(analysis.error(), what);
    }
    const double analysisSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - analysisStart).count();
    RemixOptions options;
    options.target = request.target;
    options.keepIntro = request.keepIntro;
    options.keepOutro = request.keepOutro;
    options.crossfade = request.crossfade;
    auto plan = planRemix(*analysis, TimeRange{clip.sourceIn, clip.duration()}, options);
    if (!plan) {
        failWith(plan.error(), "cannot remix '" + clip.name() + "'");
    }
    // Room on the track: a longer remix may not run into the next clip.
    const Ticks end = clip.start() + plan->duration;
    if (index + 1 < track.clips.size()) {
        const Clip& next = track.clips[index + 1];
        if (end > next.start()) {
            fail(ErrorCode::EditRejected, "the remix (" + std::to_string(static_cast<double>(std::lround(plan->duration.toSeconds() * 10.0)) / 10.0) +
                                              " s) would run into clip " + std::to_string(next.id.value) + " at " + c.timecode(next.start()) +
                                              " on " + label + ": make room, or choose a shorter duration");
        }
    }
    RemixOutcome result;
    Json segments = Json::array();
    Json joins = Json::array();
    if (request.take) {
        const std::string renderWhat = "rendering the remix of '" + media.name + "'";
        auto take = runJob(c, renderWhat, [&](std::stop_token stop, const std::function<void(double)>& progress) {
            return services->music->render(media, clip.stream.index, *plan, stop, progress);
        });
        if (!take) {
            failWith(take.error(), renderWhat);
        }
        MediaItem m;
        m.bin = media.bin;
        m.name = media.name + " (remix " + std::to_string(std::lround(plan->duration.toSeconds())) + " s)";
        m.locator.path = take->path;
        m.locator.fingerprint = take->fingerprint;
        m.info = take->info;
        m.derived = DerivedProvenance{media.id, "remix", std::string(MusicAnalysis::kAnalyzer),
                                      std::format("{:.2f} s", plan->duration.toSeconds()), utcNow()};
        auto added = c.take(wabcut::edit::addMedia(state, std::move(m)));
        state = std::move(added.state);
        out.createdIds.push_back(added.id.value);
        // The clip plays the take from its start, for the take's length.
        Sequence seq = sequenceIn(state, seqId);
        for (TrackPtr& t : seq.audioTracks) {
            if (t->id != track.id) {
                continue;
            }
            Track next = *t;
            Clip& piece = next.clips.mutableAt(index);
            piece.media = added.id;
            piece.stream = StreamRef{StreamKind::Audio, 0};
            piece.sourceIn = Ticks{};
            const Ticks length = take->info.duration.value > 0 ? std::min(plan->duration, take->info.duration) : plan->duration;
            piece.timelineRange = TimeRange{clip.start(), length};
            std::erase_if(next.transitions, [&](const Transition& tr) { return tr.outgoing == id; });
            t = std::make_shared<const Track>(std::move(next));
        }
        seq.duration = std::max(seq.duration, end);
        state = state.withSequence(std::make_shared<const Sequence>(std::move(seq)));
        result.clips = {id};
        segments.push_back(Json{{"clip", id.value}, {"media", added.id.value}, {"start", c.timecode(clip.start())},
                                {"seconds", secondsJson(plan->duration)}, {"path", take->path}});
        if (clip.properties()[ParamId::VolumeDb].isCurve()) {
            out.warnings.push_back("clip " + std::to_string(id.value) + "'s volume keyframes stay at their times in the take");
        }
    } else {
        // The pieces in the clip's place: copies of it (they share its name, level and effects) over each source range.
        Track next = track;
        next.clips.erase(index, index + 1);
        Ticks at = clip.start();
        for (std::size_t k = 0; k < plan->segments.size(); ++k) {
            Clip piece = clip;
            piece.id = k == 0 ? clip.id : state.ids.allocate<ClipId>();
            piece.sourceIn = plan->segments[k].sourceStart;
            piece.timelineRange = TimeRange{at, plan->segments[k].duration()};
            next.clips.insert(index + k, piece);
            result.clips.push_back(piece.id);
            segments.push_back(Json{{"clip", piece.id.value},
                                    {"start", c.timecode(at)},
                                    {"startTicks", at.value},
                                    {"source", Json::array({secondsJson(plan->segments[k].sourceStart), secondsJson(plan->segments[k].sourceEnd)})},
                                    {"seconds", secondsJson(plan->segments[k].duration())}});
            at = at + plan->segments[k].duration();
        }
        // A transition at the old tail no longer meets its clip; one at the head stays with the first piece.
        const auto before = next.transitions.size();
        std::erase_if(next.transitions, [&](const Transition& tr) { return tr.outgoing == id; });
        if (next.transitions.size() != before) {
            out.warnings.push_back("the transition at the end of clip " + std::to_string(id.value) + " was removed (the music ends elsewhere now)");
        }
        Sequence seq = sequenceIn(state, seqId);
        for (TrackPtr& t : seq.audioTracks) {
            if (t->id == track.id) {
                t = std::make_shared<const Track>(std::move(next));
            }
        }
        seq.duration = std::max(seq.duration, end);
        state = state.withSequence(std::make_shared<const Sequence>(std::move(seq)));
        // A short constant-gain crossfade at each join.
        for (std::size_t k = 0; k < plan->joins.size(); ++k) {
            wabcut::edit::TransitionSpec spec;
            spec.outgoing = result.clips[k];
            spec.incoming = result.clips[k + 1];
            spec.before = Ticks{plan->crossfade.value / 2};
            spec.after = Ticks{plan->crossfade.value - plan->crossfade.value / 2};
            spec.effect = std::string(kJoinTransition);
            // Custom: the sides as given. (Center would re-split them on whole video frames, and a crossfade shorter than
            // two frames would start at the cut instead of ending on the landing beat's attack.)
            spec.alignment = TransitionAlignment::Custom;
            auto added = c.take(wabcut::edit::addTransition(state, seqId, track.id, spec, wabcut::edit::HandleFit::UseAvailable));
            state = std::move(added.state);
            const RemixJoin& j = plan->joins[k];
            joins.push_back(Json{{"at", c.timecode(clip.start() + j.at)},
                                 {"atTicks", (clip.start() + j.at).value},
                                 {"from", secondsJson(j.fromBeat)},
                                 {"to", secondsJson(j.toBeat)},
                                 {"confidence", round3(j.confidence)},
                                 {"onBar", j.onBar},
                                 {"transition", added.createdTransitions.empty() ? Json(nullptr) : Json(added.createdTransitions.front().value)}});
        }
    }
    if (request.take) {
        for (const RemixJoin& j : plan->joins) {
            joins.push_back(Json{{"at", c.timecode(clip.start() + j.at)}, {"from", secondsJson(j.fromBeat)}, {"to", secondsJson(j.toBeat)},
                                 {"confidence", round3(j.confidence)}, {"onBar", j.onBar}});
        }
    }
    if (auto valid = validate(state, Validation::Strict); !valid) {
        fail(ErrorCode::InvariantViolation, valid.error().message);
    }
    double weakest = 1.0;
    for (const RemixJoin& j : plan->joins) {
        weakest = std::min(weakest, j.confidence);
    }
    if (!plan->joins.empty() && weakest < 0.35) {
        out.warnings.push_back("a join of the remix of clip " + std::to_string(id.value) + " is not seamless (confidence " +
                               std::to_string(std::lround(weakest * 100.0)) + " %): listen to it");
    }
    if (std::llabs(plan->error().value) * 2 > std::llround(plan->beatSeconds * static_cast<double>(kTicksPerSecond))) {
        out.warnings.push_back("the remix is " + std::to_string(round3(plan->error().toSeconds())) + " s from the target: the music has no joins that fit better");
    }
    result.report = Json{{"clip", id.value},
                         {"track", label},
                         {"output", request.take ? "take" : "edits"},
                         {"targetSeconds", secondsJson(plan->target)},
                         {"seconds", secondsJson(plan->duration)},
                         {"errorSeconds", secondsJson(plan->error())},
                         {"bpm", std::round(analysis->bpm * 10.0) / 10.0},
                         {"beatSeconds", round3(plan->beatSeconds)},
                         {"beatConfidence", round3(analysis->beatConfidence)},
                         {"introEnd", secondsJson(plan->introEnd)},
                         {"outroStart", secondsJson(plan->outroStart)},
                         {"crossfadeMs", std::lround(plan->crossfade.toSeconds() * 1000.0)},
                         {"segments", std::move(segments)},
                         {"joins", std::move(joins)},
                         {"analysisSeconds", round3(analysisSeconds)}};
    return result;
}

// ---- audio.remix, audio.remixRender ---------------------------------------------------------------------------------

/// The music clip and target of a remix request.
std::pair<ClipId, RemixRequest> remixRequestOf(ExecutionContext& c, bool take) {
    const std::vector<ClipId> clips = audioClipsOf(c, {c.clip("clip")});
    if (clips.size() != 1) {
        fail(ErrorCode::InvalidParams, "the clip has several audio clips linked to it: name the music clip", Json{{"path", "/clip"}});
    }
    const ClipId id = clips.front();
    const Clip& clip = clipIn(c.state(), c.sequenceId(), id);
    RemixRequest request;
    const int targets = (c.has("duration") ? 1 : 0) + (c.has("end") ? 1 : 0) + (c.has("fit") ? 1 : 0);
    if (targets != 1) {
        fail(ErrorCode::InvalidParams, "give one of \"duration\" (seconds), \"end\" (a time) or \"fit\": \"video\"");
    }
    if (c.has("duration")) {
        request.target = seconds(c.number("duration"));
    } else if (c.has("end")) {
        request.target = c.time("end") - clip.start();
    } else {
        Ticks videoEnd{};
        for (const TrackPtr& t : c.sequence().videoTracks) {
            if (!t->clips.empty()) {
                videoEnd = std::max(videoEnd, t->clips.back().end());
            }
        }
        if (videoEnd <= clip.start()) {
            fail(ErrorCode::InvalidParams, "there is no video after the music's start to fit it to", Json{{"path", "/fit"}});
        }
        request.target = videoEnd - clip.start();
    }
    if (request.target.value <= 0) {
        fail(ErrorCode::InvalidParams, "the remix must end after the clip's start", Json{{"path", "/end"}});
    }
    request.keepIntro = c.boolean("keepIntro", true);
    request.keepOutro = c.boolean("keepOutro", true);
    if (c.has("crossfade")) {
        request.crossfade = seconds(c.number("crossfade") / 1000.0);
    }
    request.take = take;
    return {id, request};
}

CommandOutput remixWith(ExecutionContext& c, bool take) {
    const auto [id, request] = remixRequestOf(c, take);
    ProjectState state = c.state();
    CommandOutput out;
    RemixOutcome r = remixClip(c, state, id, request, out);
    out.state = std::move(state);
    out.data = std::move(r.report);
    return out;
}

CommandOutput remix(ExecutionContext& c) { return remixWith(c, false); }

CommandOutput remixRender(ExecutionContext& c) { return remixWith(c, true); }

// ---- audio.essentialSound ------------------------------------------------------------------------------------------

/// The tracks whose speech music and ambience duck under: dialogue-role tracks and tracks with dialogue clips.
std::vector<TrackId> dialogueTracksOf(const Sequence& seq, const std::set<TrackId>& except) {
    std::vector<TrackId> out;
    for (const TrackPtr& t : seq.audioTracks) {
        if (except.contains(t->id)) {
            continue;
        }
        bool dialogue = t->role == TrackRole::Dialogue;
        for (const Clip& clip : t->clips) {
            if (dialogue) {
                break;
            }
            dialogue = recordedSoundType(clip.effects()) == SoundType::Dialogue;
        }
        if (dialogue) {
            out.push_back(t->id);
        }
    }
    return out;
}

CommandOutput essentialSound(ExecutionContext& c) {
    const SequenceId seqId = c.sequenceId();
    const std::vector<ClipId> clips = requestedClips(c);
    ProjectState state = c.state();
    CommandOutput out;
    const std::string typeText = c.string("type", "");

    // Clearing: the record and the effects it built go; the clip's other effects stay.
    if (typeText == "none") {
        Json cleared = Json::array();
        for (const ClipId id : clips) {
            cleared.push_back(Json{{"clip", id.value}, {"removed", clearStack(c, state, id)}});
        }
        out.state = std::move(state);
        out.data = Json{{"type", "none"}, {"clips", std::move(cleared)}};
        return out;
    }

    // Each clip's type: the request's, else the one recorded on it, else its track's role.
    struct Target {
        ClipId clip;
        TrackId track;
        SoundType type;
    };
    std::vector<Target> targets;
    for (const ClipId id : clips) {
        std::optional<SoundType> type;
        if (!typeText.empty()) {
            type = soundTypeFromString(typeText);
            if (!type) {
                fail(ErrorCode::InvalidParams, "type is dialogue, music, sfx, ambience or none", Json{{"path", "/type"}});
            }
        } else {
            type = recordedSoundType(clipIn(state, seqId, id).effects());
            if (!type) {
                type = soundTypeOf(trackOf(state, seqId, id).role);
            }
            if (!type) {
                fail(ErrorCode::InvalidParams, "clip " + std::to_string(id.value) +
                                                   " has no type yet and its track has no role: give \"type\" (dialogue, music, sfx, ambience)",
                     Json{{"path", "/type"}});
            }
        }
        targets.push_back({id, trackOf(state, seqId, id).id, *type});
    }
    const SoundType first = targets.front().type;
    for (const Target& t : targets) {
        if (t.type != first) {
            fail(ErrorCode::InvalidParams, "the clips are of different types: apply Essential Sound to each type on its own", Json{{"path", "/clips"}});
        }
    }
    const std::string presetId = c.string("preset", "default");
    const EssentialSoundPreset* preset = findEssentialSoundPreset(first, presetId);
    if (preset == nullptr) {
        fail(ErrorCode::InvalidParams, "no " + std::string(toString(first)) + " preset \"" + presetId + "\" (" + presetList(first) + ")",
             Json{{"path", "/preset"}});
    }
    const EssentialSoundOptions options = optionsFrom(c.has("options") ? c.params()["options"] : Json(), preset->options);
    if (options.duck && first == SoundType::Dialogue) {
        out.warnings.push_back("dialogue is not ducked (ducking lowers music and ambience under it)");
    }
    if (options.durationSeconds && first != SoundType::Music) {
        fail(ErrorCode::InvalidParams, "duration (Remix) is for music clips", Json{{"path", "/options/duration"}});
    }

    // 1. The stacks.
    Json rows = Json::array();
    std::map<std::uint64_t, std::size_t> rowOf;
    for (const Target& t : targets) {
        c.checkpoint("Essential Sound");
        Json effects = applyStack(c, state, t.clip, first, preset->id, options, out);
        rowOf[t.clip.value] = rows.size();
        rows.push_back(Json{{"clip", t.clip.value}, {"track", c.trackLabel(t.track)}, {"type", std::string(toString(first))}, {"effects", std::move(effects)}});
    }
    // 2. Track roles, so the tracks are found as dialogue or music by auto-ducking and the next request.
    Json roles = Json::array();
    if (c.boolean("setTrackRole", true) && trackRoleOf(first) != TrackRole::None) {
        std::set<TrackId> done;
        for (const Target& t : targets) {
            const Track* track = sequenceIn(state, seqId).findTrack(t.track);
            if (track == nullptr || track->role != TrackRole::None || done.contains(t.track) || track->locked) {
                continue;
            }
            done.insert(t.track);
            wabcut::edit::TrackStateChange change;
            change.role = trackRoleOf(first);
            state = c.take(wabcut::edit::setTrackState(state, seqId, t.track, change)).state;
            roles.push_back(Json{{"track", c.trackLabel(t.track)}, {"role", std::string(toString(trackRoleOf(first)))}});
        }
    }
    // 3. Enhance Speech: the effect only. Its take is recorded on the media item, a project change this sequence edit
    //    does not make (A.8): audio.enhanceSpeech (EditProject) renders it.
    Json takes = Json::array();
    if (options.enhanceSpeech) {
        auto* services = command::findService<AudioServices>(c.services());
        SpeechEnhancer* enhancer = services != nullptr ? services->enhancer.get() : nullptr;
        std::vector<ClipId> ids;
        for (const Target& t : targets) {
            ids.push_back(t.clip);
        }
        takes = ensureEnhancedTakes(c, state, ids, nullptr, false, enhancer != nullptr ? enhancer->modelVersion() : std::string());
        for (const Json& t : takes) {
            if (t.value("state", "") != "ready") {
                out.warnings.push_back("'" + t.value("name", "") + "' has no enhanced take yet: its clips play the original until "
                                       "audio.enhanceSpeech renders it");
            }
        }
    }
    // 4. Position (sound effects): the clip's panner.
    if (options.pan) {
        for (const Target& t : targets) {
            state = c.take(wabcut::edit::setParameter(state, seqId, t.clip, ParamRef::of(ParamId::Pan), ParamValue(*options.pan))).state;
            rows[rowOf[t.clip.value]]["pan"] = *options.pan;
        }
    }
    // 5. Loudness: each clip on its own, with its new stack, matched to the target. The steps follow the slope measured
    //    (A.10): with a compressor after the Gain a dB of Gain moves the loudness by less than a dB.
    if (options.loudness) {
        auto* services = command::findService<AudioServices>(c.services());
        if (services == nullptr || !services->loudness) {
            out.warnings.push_back("loudness not matched: no audio analysis here (the app and wabcut-cli measure through the media)");
        } else {
            for (const Target& t : targets) {
                c.checkpoint("matching loudness");
                LoudnessMatch m = matchLoudness(c, *services, std::move(state), LoudnessTarget::ofClip(t.clip), EffectTarget::ofClip(t.clip),
                                                options.targetLufs, 0.5, 3, std::nullopt, true);
                state = std::move(m.state);
                if (!m.measured) {
                    out.warnings.push_back("clip " + std::to_string(t.clip.value) + " is silent or too short to measure: its loudness is left as it is");
                } else if (!m.reached) {
                    out.warnings.push_back("clip " + std::to_string(t.clip.value) + " could not reach " + std::to_string(options.targetLufs) + " LUFS");
                }
                m.result["targetLufs"] = options.targetLufs;
                rows[rowOf[t.clip.value]]["loudness"] = std::move(m.result);
            }
        }
    }
    // 6. Duration (music): Remix. The clip becomes its pieces, which share its stack.
    std::vector<ClipId> playing;
    for (const Target& t : targets) {
        playing.push_back(t.clip);
    }
    if (options.durationSeconds) {
        playing.clear();
        for (const Target& t : targets) {
            RemixRequest request;
            request.target = seconds(*options.durationSeconds);
            RemixOutcome r = remixClip(c, state, t.clip, request, out);
            playing.insert(playing.end(), r.clips.begin(), r.clips.end());
            rows[rowOf[t.clip.value]]["remix"] = std::move(r.report);
        }
    }
    // 7. Ducking (music, ambience): volume keyframes under the dialogue's speech.
    Json regionsReport = Json::array();
    if (options.duck && first != SoundType::Dialogue) {
        std::set<TrackId> own;
        for (const Target& t : targets) {
            own.insert(t.track);
        }
        const std::vector<TrackId> dialogue = dialogueTracksOf(sequenceIn(state, seqId), own);
        auto* services = command::findService<AudioServices>(c.services());
        if (dialogue.empty()) {
            out.warnings.push_back("not ducked: no dialogue to duck under (give dialogue clips the dialogue type, or a track the dialogue role)");
        } else if (services == nullptr || !services->speech) {
            out.warnings.push_back("not ducked: no audio analysis here to find the speech");
        } else {
            c.checkpoint("finding speech");
            auto speech = services->speech->speech(state, seqId, dialogue, SpeechSource::Auto, std::nullopt, c.control());
            if (!speech) {
                fail(speech.error().code == wabcut::ErrorCode::Cancelled ? ErrorCode::Cancelled : ErrorCode::EditRejected,
                     "speech detection failed: " + speech.error().message);
            }
            const std::vector<DuckRegion> regions = duckRegions(*speech, options.ducking);
            Json ducked = Json::array();
            for (const ClipId id : playing) {
                if (Json row = duckClip(c, state, id, c.trackLabel(trackOf(state, seqId, id).id), regions, options.ducking, std::nullopt);
                    !row.is_null()) {
                    ducked.push_back(std::move(row));
                }
            }
            regionsReport = duckRegionsJson(c, regions);
            if (regions.empty()) {
                out.warnings.push_back("no speech found on the dialogue tracks");
            }
            for (Json& row : rows) {
                Json mine = Json::array();
                for (const Json& d : ducked) {
                    // The clip itself, or one of its remix pieces (they follow it in `playing`).
                    if (d["clip"] == row["clip"] || (row.contains("remix") && std::ranges::any_of(row["remix"]["segments"], [&](const Json& s) {
                                                         return s["clip"] == d["clip"];
                                                     }))) {
                        mine.push_back(d);
                    }
                }
                row["ducked"] = std::move(mine);
            }
        }
    }
    out.state = std::move(state);
    Json data{{"type", std::string(toString(first))},
              {"preset", std::string(preset->id)},
              {"presetName", std::string(preset->name)},
              {"options", optionsJson(options)},
              {"clips", std::move(rows)}};
    if (!roles.empty()) {
        data["tracks"] = std::move(roles);
    }
    if (!takes.empty()) {
        data["takes"] = std::move(takes);
    }
    if (!regionsReport.empty()) {
        data["regions"] = std::move(regionsReport);
    }
    out.data = std::move(data);
    return out;
}

void add(command::CommandRegistry& registry, command::CommandSpec spec) {
    [[maybe_unused]] const auto added = registry.add(std::move(spec));
}

Json remixSchema() {
    return withSequence({{"clip", schema::integer("The music clip.", 1)},
                         {"duration", schema::number("The target length in seconds.", 0.1, 36000.0)},
                         {"end", timeParam("Or where the music should end.")},
                         {"fit", schema::enumeration({"video"}, "Or end with the last video clip.")},
                         {"keepIntro", schema::boolean("Play the intro as it is, once (default true).")},
                         {"keepOutro", schema::boolean("Play the outro as it is, once (default true).")},
                         {"crossfade", schema::number("Join crossfade in ms (default: an eighth of a beat, 10..50).", 0.0, 500.0)}},
                        {"clip"});
}

std::vector<std::string> withNone(std::span<const std::string_view> values) {
    std::vector<std::string> out{"none"};
    for (const std::string_view v : values) {
        out.emplace_back(v);
    }
    return out;
}

} // namespace

void registerEssentialSoundCommands(command::CommandRegistry& registry) {
    const Json fraction = schema::number("0 = off .. 1.", 0.0, 1.0);
    const Json options = schema::object(
        {{"loudness", schema::boolean("Match the clip's loudness (Premiere's Auto-Match; default true).")},
         {"targetLufs", schema::number("The loudness target, LUFS (defaults: dialogue -23, music and sfx -25, ambience -30).", -60.0, 0.0)},
         {"repair", schema::object({{"noise", schema::number("Reduce Noise (RNNoise) amount, 0 = off .. 1.", 0.0, 1.0)},
                                    {"enhance", schema::boolean("Enhance Speech (DeepFilterNet3, a rendered take on this computer).")},
                                    {"rumble", schema::number("Reduce Rumble: high-pass at this frequency, Hz (0 = off; 80 is typical).", 0.0, 500.0)},
                                    {"hum", schema::number("DeHum: 50 or 60 Hz mains hum and harmonics (0 = off).", 0.0, 70.0)},
                                    {"gate", schema::number("Noise Gate (an expander with a high ratio, after Reduce Noise) amount, 0 = off .. 1; "
                                                            "it closes under the voice's loudness target.", 0.0, 1.0)},
                                    {"deess", schema::number("DeEss amount, 0 = off .. 1.", 0.0, 1.0)},
                                    {"reverb", schema::number("Reduce Reverb (an expander) amount, 0 = off .. 1.", 0.0, 1.0)}})},
         {"clarity", schema::object({{"dynamics", schema::number("Dynamics (a compressor) amount, 0 = off .. 1.", 0.0, 1.0)},
                                     {"makeup", schema::boolean("The compressor gives back the gain it takes from a voice at the "
                                                                "loudness target (default false; streamerVoice: true).")},
                                     {"eq", schema::enumeration(withNone(clarityEqPresets()), "An EQ preset.")},
                                     {"eqAmount", fraction},
                                     {"limiter", schema::boolean("A true-peak limiter, the last effect of the stack (default false; "
                                                                 "streamerVoice: true).")},
                                     {"limiterCeiling", schema::number("The limiter's ceiling, dBTP (default -1).", -24.0, 0.0)}})},
         {"creative", schema::object({{"reverb", schema::enumeration(withNone(reverbPresets()), "A reverb preset.")},
                                      {"amount", schema::number("The reverb's mix, 0..1 (default: the preset's).", 0.0, 1.0)}})},
         {"ducking", schema::object({{"enabled", schema::boolean("Duck under dialogue (default true when \"ducking\" is given).")},
                                     {"amount", schema::number("How far it goes down, dB (default -12).", -60.0, 0.0)},
                                     {"attack", schema::number("Seconds to go down (default 0.25).", 0.0, 10.0)},
                                     {"release", schema::number("Seconds to come back (default 0.75).", 0.0, 10.0)},
                                     {"hold", schema::number("Seconds to stay down after speech (default 0.25).", 0.0, 10.0)}})},
         {"duration", schema::number("Music: Remix the clip to this many seconds.", 0.1, 36000.0)},
         {"width", schema::number("Stereo width, 0 (mono) .. 1 (unchanged) .. 2.", 0.0, 2.0)},
         {"pan", schema::number("Sound effects: position, -1 (left) .. 1 (right).", -1.0, 1.0)}});
    add(registry,
        command::runsModel({"audio.essentialSound", 1, "Essential Sound",
         "Premiere's Essential Sound panel: tags clips as dialogue, music, sfx or ambience and applies a preset of that type as ordinary, "
         "editable effects at the top of each clip's stack (an Essential Sound record with the type and options, then one marked effect per "
         "setting: Gain for loudness; Dialogue Repair: Reduce Rumble (high-pass), DeHum, Reduce Noise or Enhance Speech, Noise Gate, "
         "Reduce Reverb (expander), DeEss; Clarity: EQ preset (podcastVoice, phone, radio, streamerVoice), Dynamics (compressor) and a "
         "true-peak limiter last; Creative: reverb (smallRoom, largeRoom, hall, plate, outside); Stereo Width), then matches each clip's "
         "loudness to the type's target. Music and ambience can duck under the dialogue (volume keyframes) and music can be remixed to a "
         "duration (\"duration\"); sound effects take a position (\"pan\"). Applying again replaces what Essential Sound put there; "
         "\"type\": \"none\" removes it. Presets - dialogue: default, streamerVoice (a gaming mic at -16 LUFS: rumble cut, Reduce Noise, "
         "gate, de-esser, EQ, compressor with makeup, limiter at -1 dBTP), cleanUpNoisy, podcastVoice, phone, radio, reduceRoomEcho, "
         "removeHum, smallRoom, largeRoom, outside; music: default, background (ducked), wide; sfx: default, background; ambience: "
         "default, underDialogue (ducked), wide. \"options\" change a preset's settings. \"tracks\" applies it to every clip of those "
         "tracks (the mic track of a recording in one request). Without \"type\" a clip keeps its recorded type, else its track's role. "
         "One undo step; dryRun previews it.",
         Capability::EditSequence,
         withSequence({{"clips", schema::array(Json{{"type", Json::array({"integer", "object"})}}, "The clips (video clips stand for their linked audio).", 1)},
                       {"tracks", schema::array(trackParam("An audio track."), "Or every clip on these audio tracks.", 1)},
                       {"type", schema::enumeration({"dialogue", "music", "sfx", "ambience", "none"}, "The clips' type; none removes Essential Sound.")},
                       {"preset", schema::enumeration(allPresetIds(), "A preset of the type (default: default).")},
                       {"options", options},
                       {"setTrackRole", schema::boolean("Give a track without a role the type's role (default true).")}}),
         Json{{"clips", Json::array({12, 15})}, {"type", "dialogue"}, {"preset", "podcastVoice"}, {"options", Json{{"repair", Json{{"hum", 60}}}}}},
         {}, essentialSound}));
    add(registry,
        {"audio.remix", 1, "Remix Music",
         "Premiere's Remix: retimes a music clip to a target length by rearranging its own bars - repeating or skipping sections where "
         "the music around the jump sounds alike (beats and bars, then chroma and MFCC self-similarity) - never by stretching it. The "
         "intro and the outro are kept. The clip becomes its pieces on its track (the first keeps its id; they share its level and "
         "effects) with a short constant-gain crossfade on each join just before a beat; audio.remixRender makes one rendered take "
         "instead. The report lists every join with its confidence (0..1). Target: \"duration\" (seconds), \"end\" (a timeline "
         "time) or \"fit\": \"video\" (the end of the last video clip). One undo step; dryRun previews it.",
         Capability::EditSequence, remixSchema(), Json{{"clip", 31}, {"fit", "video"}}, {}, remix});
    add(registry,
        {"audio.remixRender", 1, "Render Music Remix",
         "audio.remix rendered into one file: the remix of the music clip (the same plan: its own bars rearranged, intro and outro "
         "kept, crossfaded joins) becomes a new media item derived from the song (a float WAV in the user's cache), and the clip "
         "plays it from its start. The report lists the joins like audio.remix's. Adds media, so it is a project edit. One undo "
         "step; dryRun previews it.",
         Capability::EditProject, remixSchema(), Json{{"clip", 31}, {"duration", 60}}, {}, remixRender});
}

} // namespace wabcut::audio::edit
