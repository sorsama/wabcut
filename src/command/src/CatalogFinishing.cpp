// Built-in finishing commands: keyframes, effect stacks, transitions, speed and frame holds (model:
// FinishingOps.h), titles (TitleOps.h). Times in params are timeline times; keyframe times are converted to the clip's curve
// time (media time) here, so agents never see media time unless they ask for clip.details.

#include "Catalog.h"

#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/MotionOps.h>
#include <wabcut/model/Timecode.h>
#include <wabcut/model/TitleOps.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <numeric>

namespace wabcut::command::detail {

namespace {

const Clip& clipOf(ExecutionContext& c, ClipId id) { return c.sequence().findClip(id)->clip(); }

TrackKind kindOf(ExecutionContext& c, ClipId id) { return c.sequence().findClip(id)->track->kind; }

std::vector<std::string> interpolationNames() { return {"hold", "linear", "bezier", "easeIn", "easeOut", "easeInOut"}; }

Interpolation interpolationOf(ExecutionContext& c, const char* key, Interpolation fallback) {
    return c.has(key) ? *interpolationFromString(c.string(key)) : fallback;
}

std::string intrinsicKeys() {
    std::string out;
    for (const ParameterInfo& p : allParameters()) {
        out += (out.empty() ? "" : ", ") + std::string(p.key);
    }
    return out;
}

Json paramParam() {
    return schema::string("The parameter: a clip parameter (" + intrinsicKeys() +
                          "), or with \"effect\" a parameter key of that effect (see effect.list).");
}

Json effectInstanceParam(std::string description) {
    return schema::integer(std::move(description) + " The effect instance id (clip.details lists them).", 1);
}

Json keyIndexParam() { return schema::integer("Keyframe index (0 = first; clip.details lists the keys).", 0); }

/// The parameter named by "param" (and "effect").
edit::ParamRef paramRef(ExecutionContext& c) {
    const std::string key = c.string("param");
    if (c.has("effect")) {
        return edit::ParamRef::ofEffect(EffectInstanceId{static_cast<std::uint64_t>(c.integer("effect"))}, key);
    }
    const auto id = parameterByKey(key);
    if (!id) {
        fail(ErrorCode::InvalidParams, "\"" + key + "\" is not a clip parameter (" + intrinsicKeys() +
                                           "); for an effect parameter pass \"effect\"",
             Json{{"path", "/param"}});
    }
    return edit::ParamRef::of(*id);
}

/// The curve time at a timeline time inside the clip (its end included, for a final key).
Ticks curveTimeOf(ExecutionContext& c, const Clip& clip, const char* key) {
    const Ticks t = c.time(key);
    if (t < clip.start() || t > clip.end()) {
        fail(ErrorCode::InvalidParams,
             "the time lies outside the clip (" + c.timecode(clip.start()) + " to " + c.timecode(clip.end()) + ")",
             Json{{"path", std::string("/") + key}});
    }
    return clip.curveTimeAt(t);
}

/// The parameter's current value on the clip, or null (an unknown effect instance, an unset effect parameter).
const ParamValue* currentValue(const Clip& clip, const edit::ParamRef& ref) {
    if (ref.intrinsic) {
        return &clip.properties()[*ref.intrinsic];
    }
    for (const EffectInstance& e : clip.effects()) {
        if (e.id == ref.effect) {
            return e.find(ref.key);
        }
    }
    return nullptr;
}

/// The keyframe indices in "keys", or every key of the parameter's curve when absent.
std::vector<std::size_t> keyIndices(ExecutionContext& c, const Clip& clip, const edit::ParamRef& ref) {
    std::vector<std::size_t> out;
    if (c.has("keys")) {
        const Json& keys = c.params()["keys"];
        for (std::size_t i = 0; i < keys.size(); ++i) {
            out.push_back(c.indexValue(keys[i], "/keys/" + std::to_string(i)));
        }
        return out;
    }
    const ParamValue* v = currentValue(clip, ref);
    for (std::size_t i = 0; v && v->curve() && i < v->curve()->keys.size(); ++i) {
        out.push_back(i);
    }
    return out;
}

/// A timeline distance as a distance in the clip's curve time (negative on a reversed clip).
Ticks curveDistance(const Clip& clip, Ticks d) {
    if (clip.isFreeze() || clip.isRemapped()) {
        return d; // curve time runs with the timeline
    }
    const Ticks scaled{mulDiv(d.value, clip.speed.num, clip.speed.den, Rounding::Nearest).value_or(0)};
    return clip.reversed ? -scaled : scaled;
}

std::vector<EffectParam> effectParams(ExecutionContext& c, const char* key) {
    std::vector<EffectParam> out;
    if (!c.has(key)) {
        return out;
    }
    for (const auto& item : c.params()[key].items()) {
        out.push_back(EffectParam{item.key(), ParamValue(item.value().get<double>())});
    }
    return out;
}

Json paramsObject(std::string description) {
    return Json{{"type", "object"}, {"description", std::move(description)}};
}

/// 0.2: an effect's text data by key (Simple Text's "text", Image Matte Key's "image" = "media:<id>"); an empty string
/// removes the key.
std::vector<EffectData> effectTextData(ExecutionContext& c, const char* key) {
    std::vector<EffectData> out;
    if (!c.has(key)) {
        return out;
    }
    for (const auto& item : c.params()[key].items()) {
        if (!item.value().is_string()) {
            fail(ErrorCode::InvalidParams, "effect data values are strings", Json{{"path", "/" + std::string(key) + "/" + item.key()}});
        }
        EffectData entry{item.key(), item.value().get<std::string>()};
        if (const auto problem = effectDataProblem(entry)) {
            fail(ErrorCode::InvalidParams, *problem, Json{{"path", "/" + std::string(key) + "/" + item.key()}});
        }
        out.push_back(std::move(entry));
    }
    return out;
}

/// Speed from a percentage: exact for up to three decimals (50 -> 1/2, 33.333 -> 33333/100000).
Speed speedFromPercent(double percent) {
    const auto num = static_cast<std::int64_t>(std::llround(percent * 1000.0));
    std::int64_t den = 100000;
    const std::int64_t g = std::gcd(num, den);
    return Speed{static_cast<std::int32_t>(num / g), static_cast<std::int32_t>(den / g)};
}

edit::TransitionAlignment alignmentOf(ExecutionContext& c) {
    return transitionAlignmentFromString(c.string("alignment", "center")).value_or(edit::TransitionAlignment::Center);
}

Json alignmentParam() {
    return schema::enumeration({"center", "startAtCut", "endAtCut", "custom"},
                               "Where the transition sits on a cut between two clips: centered (default), starting at "
                               "the cut (the outgoing clip plays on), ending at it, or custom: the cut lies \"offset\" "
                               "into it (by default where it is now). Stored on the transition: a new duration keeps "
                               "it. Ignored for a one-sided transition at a clip end with no neighbour.");
}

Json offsetParam() {
    return timeParam("With alignment \"custom\": how much of the transition lies before the cut.");
}

Json fitParam() {
    return schema::enumeration({"exact", "availableHandles"},
                               "When the media lacks frames beyond the cut: \"exact\" fails with how much there is, "
                               "\"availableHandles\" shortens the transition to fit (default).");
}

std::vector<std::string> transitionIds(EffectKind kind) {
    std::vector<std::string> out;
    for (const EffectDescriptor& d : EffectRegistry::builtin().all()) {
        if (d.kind == kind) {
            out.emplace_back(d.id);
        }
    }
    return out;
}

std::vector<std::string> stackEffectIds() {
    std::vector<std::string> out;
    for (const EffectDescriptor& d : EffectRegistry::builtin().all()) {
        if (!d.intrinsic && (d.kind == EffectKind::Video || d.kind == EffectKind::Audio)) {
            out.emplace_back(d.id);
        }
    }
    return out;
}

std::string_view unitName(ParamUnit unit) {
    switch (unit) {
    case ParamUnit::Pixels:
        return "px";
    case ParamUnit::Factor:
        return "factor";
    case ParamUnit::Degrees:
        return "deg";
    case ParamUnit::Fraction:
        return "fraction";
    case ParamUnit::Decibels:
        return "dB";
    case ParamUnit::Balance:
        return "balance";
    case ParamUnit::Amount:
        return "amount";
    case ParamUnit::Stops:
        return "stops";
    case ParamUnit::Hertz:
        return "Hz";
    case ParamUnit::Milliseconds:
        return "ms";
    case ParamUnit::Ratio:
        return "ratio";
    }
    return "amount";
}

CommandOutput effectList(ExecutionContext& c) {
    const std::string kind = c.string("kind");
    Json effects = Json::array();
    for (const EffectDescriptor& d : EffectRegistry::builtin().all()) {
        if (!kind.empty() && toString(d.kind) != kind) {
            continue;
        }
        Json params = Json::array();
        for (const EffectParamInfo& p : d.params) {
            params.push_back(Json::array({p.key, unitName(p.unit), p.defaultValue, p.minValue, p.maxValue, p.animatable}));
        }
        Json entry{{"id", d.id},
                   {"name", d.displayName},
                   {"category", d.category},
                   {"kind", toString(d.kind)},
                   {"params", std::move(params)},
                   {"description", d.description}};
        if (d.intrinsic) {
            entry["builtIn"] = true;
        }
        effects.push_back(std::move(entry));
    }
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"paramFields", Json::array({"key", "unit", "default", "min", "max", "animatable"})},
                    {"effects", std::move(effects)}};
    return out;
}

// --- Fades --------------------------------------------------------------------------------------------------

CommandOutput fade(ExecutionContext& c) {
    const std::string edge = c.string("edge", "out");
    const Ticks duration = c.time("duration");
    if (duration.value <= 0) {
        fail(ErrorCode::InvalidParams, "a fade needs a positive duration", Json{{"path", "/duration"}});
    }
    ProjectState state = c.state();
    const SequenceId seq = c.sequenceId();
    for (const ClipId id : c.clips("clips")) {
        const Clip& clip = clipOf(c, id);
        const bool video = kindOf(c, id) == TrackKind::Video;
        const ParamId param = video ? ParamId::Opacity : ParamId::VolumeDb;
        const double silent = parameterInfo(param).minValue;
        const Ticks length = std::min(duration, clip.duration());
        const auto addKey = [&](Ticks timeline, double value) {
            const Clip& now = state.findSequence(seq)->findClip(id)->clip();
            state = c.take(edit::addKeyframe(state, seq, id, edit::ParamRef::of(param),
                                             Keyframe{now.curveTimeAt(timeline), value})).state;
        };
        const auto fullAt = [&](Ticks timeline) {
            const Clip& now = state.findSequence(seq)->findClip(id)->clip();
            return now.parameterAt(param, timeline);
        };
        if (edge == "in" || edge == "both") {
            const double full = fullAt(clip.start() + length);
            addKey(clip.start() + length, full);
            addKey(clip.start(), silent);
        }
        if (edge == "out" || edge == "both") {
            const double full = fullAt(clip.end() - length);
            addKey(clip.end() - length, full);
            addKey(clip.end(), silent);
        }
    }
    CommandOutput out;
    out.state = std::move(state);
    return out;
}

// --- Speed -------------------------------------------------------------------------------------------------

CommandOutput speedDuration(ExecutionContext& c) {
    const bool hasSpeed = c.has("speed");
    const bool hasDuration = c.has("duration");
    if (hasSpeed && hasDuration) {
        fail(ErrorCode::InvalidParams, "pass either \"speed\" or \"duration\"");
    }
    edit::SpeedChange change;
    change.ripple = c.boolean("ripple", false);
    change.keepDuration = c.boolean("keepDuration", false);
    if (change.keepDuration && hasDuration) {
        fail(ErrorCode::InvalidParams, "\"keepDuration\" keeps the length: pass a speed");
    }
    if (c.has("reverse")) {
        change.reversed = c.boolean("reverse", false);
    }
    const bool pitchGiven = c.has("maintainPitch");
    if (pitchGiven) {
        change.maintainPitch = c.boolean("maintainPitch", false);
    }
    // A.10: Maintain Audio Pitch is on by default where a clip first leaves 100 % (Premiere's dialog default); a clip
    // already retimed keeps its setting, and the model's default (old projects) is unchanged.
    const auto pitchFor = [&](ClipId id) -> std::optional<bool> {
        if (pitchGiven) {
            return change.maintainPitch;
        }
        const Clip& clip = clipOf(c, id);
        return clip.speed.isUnity() && !clip.reversed && !clip.isRemapped() ? std::optional<bool>(true) : std::nullopt;
    };
    const SequenceId seq = c.sequenceId();
    ProjectState state = c.state();
    const std::vector<ClipId> clips = c.clips("clips");
    // P.2: only the dialog's Time Interpolation changed (also on a time-remapped clip, whose speed cannot be set).
    const bool interpolationOnly = c.has("timeInterpolation") && !hasSpeed && !hasDuration && !c.has("reverse") &&
                                   !pitchGiven && !change.keepDuration && !change.ripple;
    if (interpolationOnly) {
        // The speed is left as it is.
    } else if (hasDuration) {
        // Each clip gets the speed that fits its own material into the duration.
        const Ticks duration = c.time("duration");
        for (const ClipId id : clips) {
            const auto speed = edit::speedForDuration(clipOf(c, id).sourceDuration(), duration);
            if (!speed) {
                fail(ErrorCode::InvalidParams, "no speed gives that duration", Json{{"path", "/duration"}});
            }
            change.speed = *speed;
            change.maintainPitch = pitchFor(id);
            state = c.take(edit::setSpeed(state, seq, std::vector<ClipId>{id}, change, c.linkMode())).state;
        }
    } else {
        for (const ClipId id : clips) {
            change.speed = hasSpeed ? speedFromPercent(c.number("speed")) : clipOf(c, id).speed;
            if (change.speed.num == 0) {
                fail(ErrorCode::InvalidParams, "clip " + std::to_string(id.value) + " is a frame hold");
            }
            change.maintainPitch = change.speed.isUnity() && !pitchGiven ? std::nullopt : pitchFor(id);
            state = c.take(edit::setSpeed(state, seq, std::vector<ClipId>{id}, change, c.linkMode())).state;
        }
    }
    if (c.has("timeInterpolation")) {
        // The video clips among them (and their linked video partners): one undo step with the speed change.
        const TimeInterpolation mode = timeInterpolationValue(c, "timeInterpolation");
        std::vector<ClipId> video;
        const Sequence* now = state.findSequence(seq);
        for (const ClipId id : clips) {
            const auto at = now ? now->findClip(id) : std::nullopt;
            if (at && at->clip().stream.kind == StreamKind::Video) {
                video.push_back(id);
            }
        }
        if (!video.empty()) {
            state = c.take(edit::setTimeInterpolation(state, seq, video, mode, c.linkMode())).state;
        } else if (interpolationOnly) {
            fail(ErrorCode::InvalidParams, "Time Interpolation applies to video clips; none was given",
                 Json{{"path", "/timeInterpolation"}});
        }
    }
    CommandOutput out;
    out.state = std::move(state);
    return out;
}

// --- Time remapping --------------------------------------------------------------------------------------------

/// Which link partners a time remap command follows.
enum class RemapFollow {
    All,    ///< every partner (turning remapping on or off)
    Time,   ///< remapped partners (edits at a timeline time); unremapped ones are skipped, not an error
    Index,  ///< remapped partners whose remap and timing equal the named clip's (key-index edits)
};

/// The clips a time remap command edits: the named clip and, with "linked" (default), its link partners on unlocked
/// tracks, as `follow` says.
std::vector<ClipId> remapTargets(ExecutionContext& c, ClipId id, RemapFollow follow) {
    std::vector<ClipId> out{id};
    const Clip& named = clipOf(c, id);
    if (c.linkMode() != edit::LinkMode::FollowLinks || !named.linkGroup.isValid()) {
        return out;
    }
    c.sequence().forEachTrack([&](const Track& t) {
        if (t.locked) {
            return;
        }
        for (const Clip& p : t.clips) {
            if (p.id == id || p.linkGroup != named.linkGroup) {
                continue;
            }
            const bool same = p.isRemapped() && named.isRemapped() && *p.timeRemap() == *named.timeRemap() &&
                              p.sourceIn - p.start() == named.sourceIn - named.start();
            if (follow == RemapFollow::All || (follow == RemapFollow::Time && p.isRemapped()) || same) {
                out.push_back(p.id);
            }
        }
    });
    return out;
}

/// The curve time of timeline time "key" on a remapped clip (inside the clip, its end included).
Ticks remapTimeOf(ExecutionContext& c, const Clip& clip, const char* key) {
    const Ticks t = c.time(key);
    if (t < clip.start() || t > clip.end()) {
        fail(ErrorCode::InvalidParams,
             "the time lies outside the clip (" + c.timecode(clip.start()) + " to " + c.timecode(clip.end()) + ")",
             Json{{"path", std::string("/") + key}});
    }
    return clip.sourceIn + (t - clip.start());
}

/// "extendClip": lengthens (or shortens) the named clip's tail by what its remap gained, rippling later clips.
ProjectState extendClip(ExecutionContext& c, ProjectState state, ClipId id, Ticks gained) {
    if (!c.boolean("extendClip", false) || gained.value == 0) {
        return state;
    }
    return c.take(edit::rippleTrim(state, c.sequenceId(), id, edit::Edge::Tail, gained, c.linkMode())).state;
}

Json extendParam() {
    return schema::boolean("Lengthen the clip by the time the remap gains (or shorten it by what it loses), "
                           "moving later clips like a ripple trim. Default false: the clip keeps its length.");
}

Json remapIndexParam() { return schema::integer("Remap key index (0 = first; clip.details lists them).", 0); }

} // namespace

namespace {

// --- Titles --------------------------------------------------------------------------------------------------

/// The style parameters of "wabcut.title.style" as an object schema (numbers within each parameter's range).
Json titleStyleParam() {
    Json properties = Json::object();
    if (const EffectDescriptor* d = EffectRegistry::builtin().find(edit::kTitleStyleEffect)) {
        for (const EffectParamInfo& p : d->params) {
            properties[std::string(p.key)] =
                schema::number(std::string(p.displayName) + " (" + std::string(unitName(p.unit)) + ", default " +
                                   std::format("{}", p.defaultValue) + ").",
                               p.minValue, p.maxValue);
        }
    }
    return Json{{"type", "object"},
                {"description", "Style values by parameter key (\"wabcut.title.style\"); others take their defaults. "
                                "Animate them later with clip.addKeyframe on the style effect."},
                {"properties", std::move(properties)},
                {"additionalProperties", false}};
}

/// The title named by "clip" (a clip showing it) or "media".
MediaId titleOf(ExecutionContext& c) {
    if (c.has("clip")) {
        const ClipId id = c.clip("clip");
        return c.sequence().findClip(id)->clip().media;
    }
    if (c.has("media")) {
        return c.media("media");
    }
    fail(ErrorCode::InvalidParams, "name the title by \"clip\" (a clip showing it) or \"media\"");
}

Json titleTarget(std::initializer_list<std::pair<const char*, Json>> extra, std::vector<std::string> required) {
    Json s = withSequence(extra, std::move(required));
    s["properties"]["clip"] = clipParam("A clip of the title.");
    s["properties"]["media"] = mediaParam("Or the title's media item.");
    return s;
}

/// Whether a video track holds nothing in [at, at + duration).
bool isFree(const Track& track, Ticks at, Ticks duration) {
    const Ticks end = at + duration;
    return std::none_of(track.clips.begin(), track.clips.end(),
                        [&](const Clip& clip) { return clip.start() < end && clip.end() > at; });
}

/// The track title.add places the title on, by "placement"; `state` gains a new top video track when needed.
TrackId titleTrack(ExecutionContext& c, ProjectState& state, Ticks at, Ticks duration) {
    const Sequence& seq = c.sequence();
    // A named track: on it, overwriting (as asked). No track: the first free track from the targeted one up, so a title or
    // graphic dropped at the playhead never overwrites the footage under it (C.16 release fix; Premiere's graphics go on
    // a free track above too).
    const std::string placement = c.string("placement", c.has("track") ? "targeted" : "freeTrackAtOrAbove");
    TrackId base;
    if (c.has("track")) {
        base = c.track("track");
    } else {
        for (const TrackId id : edit::targetedTracks(seq)) {
            const Track* t = seq.findTrack(id);
            if (t && t->kind == TrackKind::Video && !t->locked) {
                base = id;
                break;
            }
        }
    }
    if (placement == "targeted") {
        if (!base.isValid()) {
            fail(ErrorCode::InvalidParams, "no targeted unlocked video track: name one with \"track\"");
        }
        return base;
    }
    if (placement == "freeTrackAtOrAbove") {
        // From the named or targeted track (V1 when there is none) upwards: the first unlocked video track with
        // nothing where the title goes.
        bool reached = !base.isValid();
        for (const TrackPtr& t : seq.videoTracks) {
            reached = reached || t->id == base;
            if (reached && !t->locked && isFree(*t, at, duration)) {
                return t->id;
            }
        }
        if (c.has("track") && seq.findTrack(base)->kind != TrackKind::Video) {
            fail(ErrorCode::InvalidParams, "\"track\" must be a video track", Json{{"path", "/track"}});
        }
    }
    // "newTrack", or no free track: a new video track on top.
    auto created = c.take(edit::addTrack(state, c.sequenceId(), TrackKind::Video));
    state = std::move(created.state);
    return created.id;
}

CommandOutput titleEdited(ExecutionContext& c, edit::EditResult result, MediaId title) {
    std::size_t clips = 0;
    c.sequence().forEachTrack([&](const Track& t) {
        clips += static_cast<std::size_t>(
            std::count_if(t.clips.begin(), t.clips.end(), [&](const Clip& clip) { return clip.media == title; }));
    });
    return edited(std::move(result), Json{{"media", title.value}, {"clipsInSequence", clips}});
}

} // namespace

TrackId videoPlacementTrack(ExecutionContext& context, ProjectState& state, Ticks at, Ticks duration) {
    return titleTrack(context, state, at, duration);
}

void registerFinishingCommands(CommandRegistry& registry) {
    // --- Keyframes ----------------------------------------------------------------------------------------
    const auto keyTarget = [](std::initializer_list<std::pair<const char*, Json>> extra, std::vector<std::string> required) {
        Json s = withSequence(extra, std::move(required));
        s["properties"]["clip"] = clipParam("The clip.");
        s["properties"]["param"] = paramParam();
        s["properties"]["effect"] = effectInstanceParam("For an effect parameter:");
        s["required"].push_back("clip");
        s["required"].push_back("param");
        return s;
    };
    addBuiltin(registry,
               {"clip.addKeyframe", 1, "Add Keyframe",
                "Adds a keyframe at a timeline time inside the clip (or replaces the one there). A parameter that "
                "was constant becomes animated with just this key. Keyframes stick to the media: slipping the clip "
                "moves them with the picture, trims hide but keep them.",
                Capability::EditSequence,
                keyTarget({{"time", timeParam("Where the key goes (inside the clip).")},
                           {"value", schema::number("The value, in the parameter's unit.")},
                           {"interpolation",
                            schema::enumeration(interpolationNames(), "Shape after this key. Default linear.")}},
                          {"time", "value"}),
                Json{{"clip", 42}, {"param", "opacity.opacity"}, {"time", "00:00:04:00"}, {"value", 0.0}},
                {"keyframes.add"},
                [](ExecutionContext& c) {
                    const ClipId id = c.clip("clip");
                    const edit::ParamRef ref = paramRef(c);
                    Keyframe key{curveTimeOf(c, clipOf(c, id), "time"), c.number("value"),
                                 interpolationOf(c, "interpolation", Interpolation::Linear)};
                    return edited(c.take(edit::addKeyframe(c.state(), c.sequenceId(), id, ref, key)));
                }});
    addBuiltin(registry,
               {"clip.moveKeyframe", 1, "Move Keyframe", "Moves a keyframe to another timeline time inside the clip.",
                Capability::EditSequence,
                keyTarget({{"index", keyIndexParam()}, {"time", timeParam("The new time (inside the clip).")}},
                          {"index", "time"}),
                Json{{"clip", 42}, {"param", "opacity.opacity"}, {"index", 1}, {"time", "00:00:06:00"}}, {},
                [](ExecutionContext& c) {
                    const ClipId id = c.clip("clip");
                    const edit::ParamRef ref = paramRef(c);
                    return edited(c.take(edit::moveKeyframe(c.state(), c.sequenceId(), id, ref,
                                                            static_cast<std::size_t>(c.integer("index")),
                                                            curveTimeOf(c, clipOf(c, id), "time"))));
                }});
    addBuiltin(registry,
               {"clip.removeKeyframe", 1, "Remove Keyframe",
                "Removes a keyframe; removing the last one leaves the parameter constant at its value.",
                Capability::EditSequence, keyTarget({{"index", keyIndexParam()}}, {"index"}),
                Json{{"clip", 42}, {"param", "opacity.opacity"}, {"index", 0}}, {"keyframes.remove"},
                [](ExecutionContext& c) {
                    const edit::ParamRef ref = paramRef(c);
                    return edited(c.take(edit::removeKeyframe(c.state(), c.sequenceId(), c.clip("clip"), ref,
                                                              static_cast<std::size_t>(c.integer("index")))));
                }});
    addBuiltin(
        registry,
        {"clip.setKeyframe", 1, "Set Keyframe",
         "Changes a keyframe's value, interpolation (hold, linear, bezier, easeIn, easeOut, easeInOut) or Bezier "
         "handles (slope in units per second, influence 0..1 of the segment).",
         Capability::EditSequence,
         keyTarget({{"index", keyIndexParam()},
                    {"value", schema::number("New value.")},
                    {"interpolation", schema::enumeration(interpolationNames(), "New interpolation.")},
                    {"inSlope", schema::number("Bezier: arriving slope.")},
                    {"inInfluence", schema::number("Bezier: arriving influence.", 0.0, 1.0)},
                    {"outSlope", schema::number("Bezier: leaving slope.")},
                    {"outInfluence", schema::number("Bezier: leaving influence.", 0.0, 1.0)}},
                   {"index"}),
         Json{{"clip", 42}, {"param", "volume.level"}, {"index", 0}, {"interpolation", "easeInOut"}}, {},
         [](ExecutionContext& c) {
             const ClipId id = c.clip("clip");
             const edit::ParamRef ref = paramRef(c);
             edit::KeyframeChange change;
             if (c.has("value")) {
                 change.value = c.number("value");
             }
             if (c.has("interpolation")) {
                 change.interpolation = interpolationOf(c, "interpolation", Interpolation::Linear);
             }
             const auto index = static_cast<std::size_t>(c.integer("index"));
             const ParamValue* current = currentValue(clipOf(c, id), ref);
             const Keyframe* existing =
                 current && current->curve() && index < current->curve()->keys.size() ? &current->curve()->keys[index]
                                                                                        : nullptr;
             if (c.has("inSlope") || c.has("inInfluence")) {
                 BezierHandle h = existing ? existing->in : BezierHandle{};
                 h.slope = c.has("inSlope") ? c.number("inSlope") : h.slope;
                 h.influence = c.has("inInfluence") ? c.number("inInfluence") : h.influence;
                 change.in = h;
             }
             if (c.has("outSlope") || c.has("outInfluence")) {
                 BezierHandle h = existing ? existing->out : BezierHandle{};
                 h.slope = c.has("outSlope") ? c.number("outSlope") : h.slope;
                 h.influence = c.has("outInfluence") ? c.number("outInfluence") : h.influence;
                 change.out = h;
             }
             return edited(c.take(edit::setKeyframe(c.state(), c.sequenceId(), id, ref, index, change)));
         }});
    addBuiltin(
        registry,
        {"keyframes.move", 1, "Move Keyframes",
         "Moves several keyframes of one parameter together by a timeline distance, keeping their spacing. The "
         "distance is clamped so the keys stay within the clip (keys already outside it do not move further out); "
         "a moved key landing on another key replaces it. Returns the moved keys' new indices.",
         Capability::EditSequence,
         keyTarget({{"keys", schema::array(keyIndexParam(), "The keys to move. Default: all of them.")},
                    {"delta", deltaParam("How far they move; positive = later.")}},
                   {"delta"}),
         Json{{"clip", 42}, {"param", "motion.scale"}, {"keys", Json::array({1, 2})}, {"delta", "00:00:00:12"}}, {},
         [](ExecutionContext& c) {
             const ClipId id = c.clip("clip");
             const edit::ParamRef ref = paramRef(c);
             const Clip& clip = clipOf(c, id);
             const std::vector<std::size_t> keys = keyIndices(c, clip, ref);
             const Ticks delta = curveDistance(clip, c.time("delta"));
             std::vector<Ticks> times;
             if (const ParamValue* v = currentValue(clip, ref); v && v->curve()) {
                 for (const std::size_t k : keys) {
                     if (k < v->curve()->keys.size()) {
                         times.push_back(v->curve()->keys[k].time);
                     }
                 }
             }
             edit::EditResult r = c.take(edit::moveKeyframes(c.state(), c.sequenceId(), id, ref, keys, delta));
             Json indices = Json::array();
             const Clip& after = r.state.findSequence(c.sequenceId())->findClip(id)->clip();
             if (const ParamValue* v = currentValue(after, ref); v && v->curve()) {
                 std::sort(times.begin(), times.end());
                 for (const Ticks t : times) {
                     indices.push_back(*v->curve()->keyAt(t + r.appliedDelta));
                 }
             }
             const bool clamped = r.appliedDelta != delta;
             return edited(std::move(r), Json{{"indices", std::move(indices)}, {"clamped", clamped}});
         }});
    addBuiltin(registry,
               {"keyframes.setInterpolation", 1, "Set Keyframe Interpolation",
                "Sets the interpolation (hold, linear, bezier, easeIn, easeOut, easeInOut) of several keyframes of "
                "one parameter at once.",
                Capability::EditSequence,
                keyTarget({{"keys", schema::array(keyIndexParam(), "The keys. Default: all of them.")},
                           {"interpolation", schema::enumeration(interpolationNames(), "The new interpolation.")}},
                          {"interpolation"}),
                Json{{"clip", 42}, {"param", "motion.scale"}, {"interpolation", "easeInOut"}}, {},
                [](ExecutionContext& c) {
                    const ClipId id = c.clip("clip");
                    const edit::ParamRef ref = paramRef(c);
                    edit::KeyframeChange change;
                    change.interpolation = interpolationOf(c, "interpolation", Interpolation::Linear);
                    const std::vector<std::size_t> keys = keyIndices(c, clipOf(c, id), ref);
                    if (keys.empty()) {
                        fail(ErrorCode::InvalidParams, "the parameter has no keyframes", Json{{"path", "/param"}});
                    }
                    ProjectState state = c.state();
                    for (const std::size_t k : keys) {
                        state = c.take(edit::setKeyframe(state, c.sequenceId(), id, ref, k, change)).state;
                    }
                    CommandOutput out;
                    out.state = std::move(state);
                    return out;
                }});
    addBuiltin(registry,
               {"clip.setAnimated", 1, "Toggle Animation",
                "Premiere's stopwatch. On: the constant value becomes one keyframe at \"time\" (default: the clip's "
                "start). Off: all keyframes are deleted and the parameter keeps the value it has at \"time\".",
                Capability::EditSequence,
                keyTarget({{"animated", schema::boolean("true to animate, false to make constant.")},
                           {"time", timeParam("The reference time (inside the clip). Default: the clip start.")}},
                          {"animated"}),
                Json{{"clip", 42}, {"param", "motion.scale"}, {"animated", true}}, {},
                [](ExecutionContext& c) {
                    const ClipId id = c.clip("clip");
                    const edit::ParamRef ref = paramRef(c);
                    const Clip& clip = clipOf(c, id);
                    const Ticks at = c.has("time") ? curveTimeOf(c, clip, "time") : clip.curveTimeAt(clip.start());
                    return edited(c.take(
                        edit::setAnimated(c.state(), c.sequenceId(), id, ref, c.boolean("animated", true), at)));
                }});
    addBuiltin(registry,
               {"clip.fade", 1, "Fade",
                "Fades clips in and/or out with keyframes: opacity on video clips (to 0), level on audio clips (to "
                "-144 dB). The fade ends (or starts) at the value the parameter has there.",
                Capability::EditSequence,
                withSequence({{"clips", clipsParam("Clips to fade.")},
                              {"edge", schema::enumeration({"in", "out", "both"}, "Which end. Default out.")},
                              {"duration", timeParam("Fade length (at most the clip length).")}},
                             {"clips", "duration"}),
                Json{{"clips", Json::array({42, 43})}, {"edge", "both"}, {"duration", "00:00:01:00"}}, {}, fade});

    // --- Effects ------------------------------------------------------------------------------------------
    addBuiltin(registry,
               {"effect.list", 1, "List Effects",
                "The effects and transitions Wabcut knows: id, name, category, kind (video, audio, videoTransition, "
                "audioTransition) and parameters as arrays in paramFields order. builtIn effects are part of every "
                "clip (set their parameters with clip.setParam or keyframes); the others go into a clip's stack.",
                Capability::Read,
                schema::object({{"kind", schema::enumeration({"video", "audio", "videoTransition", "audioTransition"},
                                                             "Only effects of this kind.")}}),
                Json{{"kind", "video"}}, {}, effectList});
    addBuiltin(registry,
               {"clip.addEffect", 1, "Apply Effect",
                "Adds an effect to the stack of each clip (video effects on video clips, audio effects on audio "
                "clips). Parameters not given take their defaults. An audio plugin (\"plugin\": its effect id from "
                "plugins.list) goes on audio clips the same way. The new instance ids are returned in createdIds.",
                Capability::EditSequence,
                withSequence({{"clips", clipsParam("Clips to add the effect to.")},
                              {"effect", schema::enumeration(stackEffectIds(), "The effect id (effect.list).")},
                              {"plugin", schema::string("Or an audio plugin's effect id (plugins.list), e.g. \"plugin.vst3.<id>\".")},
                              {"params", paramsObject("Parameter values by key, e.g. {\"radius\": 12}.")},
                              {"index", schema::integer("Position in the stack (0 = first). Default: last.", 0)}},
                             {"clips"}),
                Json{{"clips", Json::array({42})}, {"effect", "wabcut.blur.gaussian"}, {"params", Json{{"radius", 12}}}},
                {},
                [](ExecutionContext& c) {
                    std::optional<std::size_t> index;
                    if (c.has("index")) {
                        index = static_cast<std::size_t>(c.integer("index"));
                    }
                    // A.6: "plugin" names an audio plugin effect (one this computer has) instead of a built-in effect.
                    if (c.has("plugin") == c.has("effect")) {
                        fail(ErrorCode::InvalidParams, "give \"effect\" (a built-in effect) or \"plugin\" (plugins.list), not both");
                    }
                    std::string effect = c.has("plugin") ? c.string("plugin") : c.string("effect");
                    if (c.has("plugin") && (!effect.starts_with(kPluginEffectPrefix) || EffectRegistry::builtin().find(effect) == nullptr)) {
                        fail(ErrorCode::NotFound, "no audio plugin '" + effect + "' on this computer (plugins.list lists them)",
                             Json{{"path", "/plugin"}});
                    }
                    return edited(c.take(edit::addEffect(c.state(), c.sequenceId(), c.clips("clips"), effect,
                                                         effectParams(c, "params"), index)));
                }});
    addBuiltin(registry,
               {"clip.removeEffect", 1, "Remove Effect", "Removes an effect instance from a clip's stack.",
                Capability::EditSequence,
                withSequence({{"clip", clipParam("The clip.")}, {"effect", effectInstanceParam("The effect to remove.")}},
                             {"clip", "effect"}),
                Json{{"clip", 42}, {"effect", 77}}, {},
                [](ExecutionContext& c) {
                    return edited(c.take(edit::removeEffect(c.state(), c.sequenceId(), c.clip("clip"),
                                                            EffectInstanceId{static_cast<std::uint64_t>(c.integer("effect"))})));
                }});
    addBuiltin(registry,
               {"clip.moveEffect", 1, "Reorder Effect", "Moves an effect to another position in the clip's stack.",
                Capability::EditSequence,
                withSequence({{"clip", clipParam("The clip.")},
                              {"effect", effectInstanceParam("The effect to move.")},
                              {"index", schema::integer("New position (0 = first).", 0)}},
                             {"clip", "effect", "index"}),
                Json{{"clip", 42}, {"effect", 77}, {"index", 0}}, {},
                [](ExecutionContext& c) {
                    return edited(c.take(edit::moveEffect(c.state(), c.sequenceId(), c.clip("clip"),
                                                          EffectInstanceId{static_cast<std::uint64_t>(c.integer("effect"))},
                                                          static_cast<std::size_t>(c.integer("index")))));
                }});
    addBuiltin(registry,
               {"clip.setEffect", 1, "Set Effect",
                "Turns an effect on or off or sets its parameters (constants; animate them with clip.addKeyframe and "
                "\"effect\") or its text data (\"data\": Simple Text's {\"text\": \"...\"}, Image Matte Key's "
                "{\"image\": \"media:<id>\"} naming a still image in the project; an empty string removes it).",
                Capability::EditSequence,
                withSequence({{"clip", clipParam("The clip.")},
                              {"effect", effectInstanceParam("The effect.")},
                              {"enabled", schema::boolean("On or off.")},
                              {"params", paramsObject("Parameter values by key.")},
                              {"data", paramsObject("Text data by key (strings, at most 1024 bytes each).")}},
                             {"clip", "effect"}),
                Json{{"clip", 42}, {"effect", 77}, {"params", Json{{"saturation", 0}}}}, {},
                [](ExecutionContext& c) {
                    edit::EffectChange change;
                    if (c.has("enabled")) {
                        change.enabled = c.boolean("enabled", true);
                    }
                    change.params = effectParams(c, "params");
                    change.data = effectTextData(c, "data");
                    return edited(c.take(edit::setEffect(c.state(), c.sequenceId(), c.clip("clip"),
                                                         EffectInstanceId{static_cast<std::uint64_t>(c.integer("effect"))},
                                                         change)));
                }});

    // --- Transitions ---------------------------------------------------------------------------------------
    std::vector<std::string> allTransitions = transitionIds(EffectKind::VideoTransition);
    for (const std::string& id : transitionIds(EffectKind::AudioTransition)) {
        allTransitions.push_back(id);
    }
    addBuiltin(
        registry,
        {"transition.add", 1, "Add Transition",
         "Adds a transition at a clip's head or tail: on the cut when the neighbouring clip meets that edge, otherwise "
         "inside the clip (a fade from/to black). The media must have frames beyond the cut; by default the "
         "transition is shortened to what there is. Default effect: crossDissolve on video, constantPower on audio.",
         Capability::EditSequence,
         withSequence({{"clip", clipParam("The clip.")},
                       {"edge", edgeParam()},
                       {"duration", timeParam("Transition length. Default 1 second.")},
                       {"alignment", alignmentParam()},
                       {"offset", offsetParam()},
                       {"effect", schema::enumeration(allTransitions, "The transition (effect.list).")},
                       {"params", paramsObject("Constant parameters, e.g. a wipe's {\"angle\": 90}.")},
                       {"fit", fitParam()}},
                      {"clip", "edge"}),
         Json{{"clip", 42}, {"edge", "tail"}, {"duration", "00:00:00:12"}}, {},
         [](ExecutionContext& c) {
             const ClipId id = c.clip("clip");
             const bool video = kindOf(c, id) == TrackKind::Video;
             const Rate rate = c.sequence().frameRate;
             const Ticks second = framesToTicks((static_cast<std::int64_t>(rate.num) + rate.den / 2) / rate.den, rate);
             const Ticks duration = c.optionalTime("duration").value_or(second);
             const std::string effect = c.string("effect", video ? "crossDissolve" : "constantPower");
             const auto edge = c.string("edge") == "head" ? edit::Edge::Head : edit::Edge::Tail;
             auto at = c.take(edit::transitionAtEdge(c.state(), c.sequenceId(), id, edge, duration, alignmentOf(c), effect));
             if (at.spec.alignment == edit::TransitionAlignment::Custom && c.has("offset")) {
                 at.spec.before = std::clamp(c.time("offset"), Ticks{}, duration);
                 at.spec.after = duration - at.spec.before;
             }
             at.spec.params = effectParams(c, "params");
             const auto fit = c.string("fit", "availableHandles") == "exact" ? edit::HandleFit::Exact
                                                                            : edit::HandleFit::UseAvailable;
             return edited(c.take(edit::addTransition(c.state(), c.sequenceId(), at.track, at.spec, fit)));
         }});
    addBuiltin(
        registry,
        {"transition.set", 1, "Set Transition",
         "Changes a transition: its effect, parameters, its length on either side of the cut (\"before\", "
         "\"after\"), a new total \"duration\" placed by its alignment, or the alignment itself (which re-splits "
         "it; \"custom\" with \"offset\" puts the cut that far in).",
         Capability::EditSequence,
         withSequence({{"transition", schema::integer("Transition id (sequence.summary and clip.details list them).", 1)},
                       {"duration", timeParam("New total length, split by alignment.")},
                       {"alignment", alignmentParam()},
                       {"offset", offsetParam()},
                       {"before", timeParam("New length before the cut.")},
                       {"after", timeParam("New length after the cut.")},
                       {"effect", schema::enumeration(allTransitions, "New transition effect.")},
                       {"params", paramsObject("New constant parameters (replace all).")},
                       {"fit", fitParam()}},
                      {"transition"}),
         Json{{"transition", 90}, {"duration", "00:00:01:00"}, {"effect", "dipToBlack"}}, {},
         [](ExecutionContext& c) {
             const TransitionId id{static_cast<std::uint64_t>(c.integer("transition"))};
             edit::TransitionChange change;
             change.duration = c.optionalTime("duration");
             if (c.has("alignment")) {
                 change.alignment = alignmentOf(c);
             }
             if (c.has("offset")) {
                 // Custom: the cut that far in, the rest after it.
                 Ticks total{};
                 c.sequence().forEachTrack([&](const Track& t) {
                     for (const Transition& tr : t.transitions) {
                         if (tr.id == id) {
                             total = tr.duration();
                         }
                     }
                 });
                 total = change.duration.value_or(total);
                 change.alignment = edit::TransitionAlignment::Custom;
                 change.before = std::clamp(c.time("offset"), Ticks{}, total);
                 change.after = total - *change.before;
             }
             if (c.has("before")) {
                 change.before = c.time("before");
             }
             if (c.has("after")) {
                 change.after = c.time("after");
             }
             if (c.has("effect")) {
                 change.effect = c.string("effect");
             }
             if (c.has("params")) {
                 change.params = effectParams(c, "params");
             }
             const auto fit = c.string("fit", "availableHandles") == "exact" ? edit::HandleFit::Exact
                                                                            : edit::HandleFit::UseAvailable;
             return edited(c.take(edit::setTransition(c.state(), c.sequenceId(), id, change, fit)));
         }});
    addBuiltin(registry,
               {"transition.remove", 1, "Remove Transition", "Deletes a transition; the edit point becomes a cut.",
                Capability::EditSequence, withSequence({{"transition", schema::integer("Transition id.", 1)}}, {"transition"}),
                Json{{"transition", 90}}, {},
                [](ExecutionContext& c) {
                    return edited(c.take(edit::removeTransition(c.state(), c.sequenceId(),
                                                                TransitionId{static_cast<std::uint64_t>(c.integer("transition"))})));
                }});
    const auto applyDefault = [](TrackKind kind) {
        return [kind](ExecutionContext& c) {
            edit::DefaultTransitions defaults;
            if (c.has("duration")) {
                (kind == TrackKind::Video ? defaults.videoDuration : defaults.audioDuration) = c.time("duration");
            }
            return edited(c.take(edit::applyDefaultTransition(c.state(), c.sequenceId(), c.time("time"), kind, defaults)));
        };
    };
    addBuiltin(registry,
               {"sequence.applyVideoTransition", 1, "Apply Video Transition",
                "Ctrl+D: a cross dissolve on the edit point nearest the playhead on each targeted video track, "
                "shortened to the media there is.",
                Capability::EditSequence,
                withSequence({{"time", timeParam("The playhead.")},
                              {"duration", timeParam("Length. Default 1 second.")}},
                             {"time"}),
                Json{{"time", "00:00:10:00"}}, {}, applyDefault(TrackKind::Video)});
    addBuiltin(registry,
               {"sequence.applyAudioTransition", 1, "Apply Audio Transition",
                "Ctrl+Shift+D: a constant-power crossfade on the edit point nearest the playhead on each targeted "
                "audio track.",
                Capability::EditSequence,
                withSequence({{"time", timeParam("The playhead.")},
                              {"duration", timeParam("Length. Default 1 second.")}},
                             {"time"}),
                Json{{"time", "00:00:10:00"}}, {}, applyDefault(TrackKind::Audio)});
    addBuiltin(registry,
               {"sequence.applyDefaultTransitions", 1, "Apply Default Transitions to Selection",
                "Shift+D: default transitions on both ends of each clip (cross dissolve on video, constant power on "
                "audio), on the cut where a neighbour meets the clip, otherwise a fade inside it.",
                Capability::EditSequence,
                withSequence({{"clips", clipsParam("The selected clips.")},
                              {"videoDuration", timeParam("Video length. Default 1 second.")},
                              {"audioDuration", timeParam("Audio length. Default 1 second.")}},
                             {"clips"}),
                Json{{"clips", Json::array({42, 43})}}, {},
                [](ExecutionContext& c) {
                    edit::DefaultTransitions defaults;
                    defaults.videoDuration = c.optionalTime("videoDuration");
                    defaults.audioDuration = c.optionalTime("audioDuration");
                    return edited(c.take(edit::applyDefaultTransitions(c.state(), c.sequenceId(), c.clips("clips"), defaults)));
                }});

    // --- Speed ---------------------------------------------------------------------------------------------
    addBuiltin(
        registry,
        {"clip.speedDuration", 1, "Speed/Duration",
         "Ctrl+R: plays each clip's material at a new speed (percent; 200 = twice as fast) or fits it into a new "
         "duration; can reverse it and keep audio pitch. With ripple, later material on the track and sync-locked "
         "tracks moves; without, the clip only grows into the gap after it.",
         Capability::EditSequence,
         withSequence({{"clips", clipsParam("Clips to retime.")},
                       {"speed", schema::number("Speed in percent (100 = normal).", 0.001, 100000.0)},
                       {"duration", timeParam("New length instead of a speed.")},
                       {"reverse", schema::boolean("Play backwards.")},
                       {"maintainPitch", schema::boolean("Audio: keep the pitch (time-stretch) instead of varispeed. "
                                                         "Default: on for a clip leaving 100 %, else unchanged.")},
                       {"ripple", schema::boolean("Shift trailing clips. Default false.", false)},
                       {"keepDuration",
                        schema::boolean("Speed and duration unlinked: keep each clip's length and play more or less "
                                        "of its media (the source Out moves). Default false.",
                                        false)},
                       {"timeInterpolation",
                        timeInterpolationParam("The dialog's Time Interpolation (P.2, as clip.setTimeInterpolation): "
                                               "Frame Sampling, Frame Blending or Optical Flow for the video clips. "
                                               "Default: unchanged.")},
                       {"linked", linkedParam(true)}},
                      {"clips"}),
         Json{{"clips", Json::array({42})}, {"speed", 50}, {"ripple", true}}, {"clip.speed"}, speedDuration});
    addBuiltin(
        registry,
        {"clip.frameHold", 1, "Frame Hold",
         "Holds the frame at a time. Without duration (Add Frame Hold) the clip is cut there and the rest shows that "
         "frame; with a duration (Insert Frame Hold Segment) a still of that length is inserted, rippling the track "
         "and sync-locked tracks. Effects keep animating through the hold.",
         Capability::EditSequence,
         withSequence({{"clip", clipParam("A video clip.")},
                       {"time", timeParam("The frame to hold (inside the clip).")},
                       {"duration", timeParam("Insert a hold segment of this length instead.")}},
                      {"clip", "time"}),
         Json{{"clip", 42}, {"time", "00:00:03:00"}, {"duration", "00:00:02:00"}}, {},
         [](ExecutionContext& c) {
             return edited(c.take(edit::frameHold(c.state(), c.sequenceId(), c.clip("clip"), c.time("time"),
                                                  c.optionalTime("duration"))));
         }});

    // --- Captions (made by captions.generate from the transcript) ---------------------------------------
    addBuiltin(registry,
               {"captions.remove", 1, "Delete Caption Track", "Deletes a caption track of the sequence.",
                Capability::EditSequence,
                withSequence({{"captionTrack", schema::integer("Caption track id (sequence.summary lists them).", 1)}},
                             {"captionTrack"}),
                Json{{"captionTrack", 57}}, {},
                [](ExecutionContext& c) {
                    const CaptionTrackId id{static_cast<std::uint64_t>(c.integer("captionTrack"))};
                    return edited(c.take(edit::removeCaptionTrack(c.state(), c.sequenceId(), id)));
                }});

    // --- Time remapping (TimeRemap.h) -------------------------------------------------------------------------
    const auto remapTarget = [](std::initializer_list<std::pair<const char*, Json>> extra, std::vector<std::string> required) {
        Json s = withSequence(extra, std::move(required));
        s["properties"]["clip"] = clipParam("The clip.");
        s["properties"]["linked"] = schema::boolean("Linked partners take part too (default true).");
        s["required"].push_back("clip");
        return s;
    };
    addBuiltin(
        registry,
        {"timeRemap.enable", 1, "Time Remapping",
         "Turns time remapping on (speed ramps, holds and reverse inside one clip; the clip shows the same frames, "
         "also beyond its ends for transitions and trims; keyframes of a clip that was not at 100 % then animate at "
         "timeline rate) or off (back to a constant speed: the one it shows if a single straight segment covers it, "
         "else 100 % from its first frame). Audio follows the remap (A.10): pitch kept (Maintain Audio Pitch, on by "
         "default when remapping starts) or varispeed; holds are silent.",
         Capability::EditSequence,
         remapTarget({{"enabled", schema::boolean("true (default) on, false off.")},
                      {"maintainPitch", schema::boolean("Audio: keep the pitch in ramps (time-stretch) instead of "
                                                        "varispeed. Default true when turning remapping on.")}},
                     {}),
         Json{{"clip", 42}, {"enabled", true}}, {},
         [](ExecutionContext& c) {
             const ClipId id = c.clip("clip");
             const bool enabled = c.boolean("enabled", true);
             const bool keepPitch = c.boolean("maintainPitch", true);
             ProjectState state = c.state();
             for (const ClipId target : remapTargets(c, id, RemapFollow::All)) {
                 // A.10: the clip's Maintain Audio Pitch before it is remapped (a remapped clip takes no speed change).
                 const auto loc = state.findSequence(c.sequenceId())->findClip(target);
                 const Clip* clip = loc ? &loc->clip() : nullptr;
                 if (enabled && clip != nullptr && !clip->isRemapped() && !clip->isFreeze() &&
                     clip->stream.kind == StreamKind::Audio && clip->maintainPitch != keepPitch) {
                     edit::SpeedChange pitch;
                     pitch.speed = clip->speed;
                     pitch.maintainPitch = keepPitch;
                     pitch.keepDuration = true;
                     state = c.take(edit::setSpeed(state, c.sequenceId(), std::vector<ClipId>{target}, pitch,
                                                   edit::LinkMode::IgnoreLinks))
                                 .state;
                 }
                 state = c.take(edit::setTimeRemapEnabled(state, c.sequenceId(), target, enabled)).state;
             }
             CommandOutput out;
             out.state = std::move(state);
             return out;
         }});
    addBuiltin(registry,
               {"timeRemap.addKey", 1, "Add Time Remap Key",
                "Adds a remap key at the sampled source position of a timeline time inside a remapped clip. "
                "Holds stay holds; the new key is otherwise linear and can reshape a smooth ramp. Returns its index.",
                Capability::EditSequence, remapTarget({{"time", timeParam("Where the key goes (inside the clip).")}}, {"time"}),
                Json{{"clip", 42}, {"time", "00:00:02:00"}}, {},
                [](ExecutionContext& c) {
                    const ClipId id = c.clip("clip");
                    ProjectState state = c.state();
                    const Ticks curveTime = remapTimeOf(c, clipOf(c, id), "time");
                    for (const ClipId target : remapTargets(c, id, RemapFollow::Time)) {
                        const Clip& clip = clipOf(c, target);
                        state = c.take(edit::addTimeRemapKey(state, c.sequenceId(), target,
                                                             clip.sourceIn + (c.time("time") - clip.start()))).state;
                    }
                    const Clip& after = state.findSequence(c.sequenceId())->findClip(id)->clip();
                    CommandOutput out;
                    out.data = Json{{"index", *after.timeRemap()->keyAt(curveTime)}};
                    out.state = std::move(state);
                    return out;
                }});
    addBuiltin(
        registry,
        {"timeRemap.moveKey", 1, "Move Time Remap Key",
         "Moves a remap key to another timeline time. Without \"ripple\" it stays between its neighbours and both "
         "segments around it change speed; with \"ripple\" the later keys move too, so only the segment before it "
         "changes speed (Premiere's speed keyframe drag).",
         Capability::EditSequence,
         remapTarget({{"index", remapIndexParam()},
                      {"time", timeParam("The new time (inside the clip).")},
                      {"ripple", schema::boolean("Move the later keys by as much (default false).")},
                      {"extendClip", extendParam()}},
                     {"index", "time"}),
         Json{{"clip", 42}, {"index", 1}, {"time", "00:00:03:00"}, {"ripple", true}}, {},
         [](ExecutionContext& c) {
             const ClipId id = c.clip("clip");
             const Clip& clip = clipOf(c, id);
             const std::size_t index = c.indexValue(c.params()["index"], "/index");
             const Ticks curveTime = remapTimeOf(c, clip, "time");
             const bool ripple = c.boolean("ripple", false);
             const Ticks old = clip.isRemapped() && index < clip.timeRemap()->keys.size()
                                   ? clip.timeRemap()->keys[index].time
                                   : curveTime;
             ProjectState state = c.state();
             for (const ClipId target : remapTargets(c, id, RemapFollow::Index)) {
                 state = c.take(edit::moveTimeRemapKey(state, c.sequenceId(), target, index, curveTime, ripple)).state;
             }
             const bool visible = old < clip.sourceOut();
             CommandOutput out;
             out.state = extendClip(c, std::move(state), id, ripple && visible ? curveTime - old : Ticks{});
             return out;
         }});
    addBuiltin(registry,
               {"timeRemap.setInterpolation", 1, "Set Time Remap Interpolation",
                "Shapes the remap around keys: linear (constant speed between keys), hold (the frame holds until the "
                "next key, then jumps), or easeIn / easeOut / easeInOut / bezier for a smooth speed ramp.",
                Capability::EditSequence,
                remapTarget({{"keys", schema::array(remapIndexParam(), "The keys. Default: all of them.")},
                             {"interpolation", schema::enumeration(interpolationNames(), "The new interpolation.")}},
                            {"interpolation"}),
                Json{{"clip", 42}, {"keys", Json::array({1, 2})}, {"interpolation", "easeInOut"}}, {},
                [](ExecutionContext& c) {
                    const ClipId id = c.clip("clip");
                    const Clip& clip = clipOf(c, id);
                    std::vector<std::size_t> keys;
                    if (c.has("keys")) {
                        const Json& list = c.params()["keys"];
                        for (std::size_t i = 0; i < list.size(); ++i) {
                            keys.push_back(c.indexValue(list[i], "/keys/" + std::to_string(i)));
                        }
                    } else if (const Curve* remap = clip.timeRemap()) {
                        for (std::size_t i = 0; i < remap->keys.size(); ++i) {
                            keys.push_back(i);
                        }
                    }
                    edit::TimeRemapKeyChange change;
                    change.interpolation = interpolationOf(c, "interpolation", Interpolation::Linear);
                    ProjectState state = c.state();
                    for (const ClipId target : remapTargets(c, id, RemapFollow::Index)) {
                        for (const std::size_t k : keys) {
                            state = c.take(edit::setTimeRemapKey(state, c.sequenceId(), target, k, change)).state;
                        }
                    }
                    if (!clip.isRemapped()) {
                        state = c.take(edit::setTimeRemapKey(state, c.sequenceId(), id, 0, change)).state; // the error
                    }
                    CommandOutput out;
                    out.state = std::move(state);
                    return out;
                }});
    addBuiltin(
        registry,
        {"timeRemap.setKey", 1, "Set Time Remap Key",
         "Changes a remap key's source position, interpolation and Bezier handles together. Omitted fields keep "
         "their values. Slopes are source ticks per second of curve time (254016000000 = 100% forwards); "
         "influence is a fraction of the adjacent segment. Source bounds and monotonic segments are validated.",
         Capability::EditSequence,
         remapTarget({{"index", remapIndexParam()},
                      {"source", schema::integer("Source position in whole ticks (not timeline time).", 0, kMaxRemapSource)},
                      {"interpolation", schema::enumeration(interpolationNames(), "The key's interpolation.")},
                      {"inSlope", schema::number("Bezier: arriving slope in source ticks per second.")},
                      {"inInfluence", schema::number("Bezier: arriving influence.", 0.0, 1.0)},
                      {"outSlope", schema::number("Bezier: leaving slope in source ticks per second.")},
                      {"outInfluence", schema::number("Bezier: leaving influence.", 0.0, 1.0)}},
                     {"index"}),
         Json{{"clip", 42}, {"index", 1}, {"interpolation", "bezier"}, {"outSlope", kTicksPerSecond}}, {},
         [](ExecutionContext& c) {
             const ClipId id = c.clip("clip");
             const Clip& clip = clipOf(c, id);
             const Curve* remap = clip.timeRemap();
             if (!remap) {
                 fail(ErrorCode::EditRejected, "time remapping is off for this clip");
             }
             const std::size_t index = c.indexValue(c.params()["index"], "/index");
             if (index >= remap->keys.size()) {
                 fail(ErrorCode::NotFound, "there is no remap key at that index");
             }
             for (const char* field : {"inSlope", "inInfluence", "outSlope", "outInfluence"}) {
                 if (c.has(field) && !std::isfinite(c.number(field))) {
                     fail(ErrorCode::InvalidParams, "Bezier handles must be finite", Json{{"path", std::string("/") + field}});
                 }
             }
             Curve next = *remap;
             Keyframe& key = next.keys[index];
             if (c.has("source")) {
                 key.value = static_cast<double>(c.integer("source"));
             }
             if (c.has("interpolation")) {
                 key.interpolation = interpolationOf(c, "interpolation", Interpolation::Linear);
             }
             if (c.has("inSlope")) {
                 key.in.slope = c.number("inSlope");
             }
             if (c.has("inInfluence")) {
                 key.in.influence = c.number("inInfluence");
             }
             if (c.has("outSlope")) {
                 key.out.slope = c.number("outSlope");
             }
             if (c.has("outInfluence")) {
                 key.out.influence = c.number("outInfluence");
             }
             ProjectState state = c.state();
             for (const ClipId target : remapTargets(c, id, RemapFollow::Index)) {
                 state = c.take(edit::setTimeRemap(state, c.sequenceId(), target, next, clipOf(c, target).sourceIn)).state;
             }
             CommandOutput out;
             out.state = std::move(state);
             return out;
         }});
    addBuiltin(registry,
               {"timeRemap.removeKey", 1, "Remove Time Remap Key", "Removes a remap key (not the only one).",
                Capability::EditSequence, remapTarget({{"index", remapIndexParam()}}, {"index"}),
                Json{{"clip", 42}, {"index", 1}}, {},
                [](ExecutionContext& c) {
                    const ClipId id = c.clip("clip");
                    const std::size_t index = c.indexValue(c.params()["index"], "/index");
                    ProjectState state = c.state();
                    for (const ClipId target : remapTargets(c, id, RemapFollow::Index)) {
                        state = c.take(edit::removeTimeRemapKey(state, c.sequenceId(), target, index)).state;
                    }
                    CommandOutput out;
                    out.state = std::move(state);
                    return out;
                }});
    addBuiltin(
        registry,
        {"timeRemap.setSpeed", 1, "Set Segment Speed",
         "Sets the speed of one segment of a remapped clip (between two keys; the one playing at \"time\", or "
         "\"segment\" by index): its end key and the keys after it move, so later material plays earlier or later.",
         Capability::EditSequence,
         remapTarget({{"segment", schema::integer("Segment index (key index to the next key).", 0)},
                      {"time", timeParam("Or a timeline time inside the segment.")},
                      {"speed", schema::number("Speed in percent (positive; the direction stays).", 0.001, 100000.0)},
                      {"extendClip", extendParam()}},
                     {"speed"}),
         Json{{"clip", 42}, {"time", "00:00:01:00"}, {"speed", 50}}, {},
         [](ExecutionContext& c) {
             const ClipId id = c.clip("clip");
             const Clip& clip = clipOf(c, id);
             const Curve* remap = clip.timeRemap();
             std::size_t segment = 0;
             if (c.has("segment")) {
                 segment = c.indexValue(c.params()["segment"], "/segment");
             } else if (c.has("time")) {
                 const Ticks t = remapTimeOf(c, clip, "time");
                 const std::size_t next = remap ? remap->insertionIndex(t + Ticks{1}) : 0;
                 if (!remap || next == 0 || next >= remap->keys.size()) {
                     fail(ErrorCode::InvalidParams, "no remap segment plays at that time (it is before the first key "
                                                    "or after the last)",
                          Json{{"path", "/time"}});
                 }
                 segment = next - 1;
             } else {
                 fail(ErrorCode::InvalidParams, "name the segment by \"segment\" or \"time\"");
             }
             const Speed speed = speedFromPercent(c.number("speed"));
             const Ticks old = remap && segment + 1 < remap->keys.size() ? remap->keys[segment + 1].time : Ticks{};
             ProjectState state = c.state();
             for (const ClipId target : remapTargets(c, id, RemapFollow::Index)) {
                 state = c.take(edit::setTimeRemapSpeed(state, c.sequenceId(), target, segment, speed)).state;
             }
             const Clip& after = state.findSequence(c.sequenceId())->findClip(id)->clip();
             const Ticks gained = after.timeRemap()->keys[segment + 1].time - old;
             const bool visible = remap->keys[segment].time < clip.sourceOut();
             CommandOutput out;
             out.data = Json{{"segment", segment}};
             out.state = extendClip(c, std::move(state), id, visible ? gained : Ticks{});
             return out;
         }});
    const auto insertSegment = [](bool reverse) {
        return [reverse](ExecutionContext& c) {
            const ClipId id = c.clip("clip");
            const Clip& clip = clipOf(c, id);
            const Ticks duration = c.time("duration");
            ProjectState state = c.state();
            remapTimeOf(c, clip, "time"); // inside the clip
            for (const ClipId target : remapTargets(c, id, RemapFollow::Time)) {
                const Clip& t = clipOf(c, target);
                const Ticks at = t.sourceIn + (c.time("time") - t.start());
                state = c.take(reverse ? edit::addTimeRemapReverse(state, c.sequenceId(), target, at, duration)
                                       : edit::addTimeRemapHold(state, c.sequenceId(), target, at, duration))
                            .state;
            }
            CommandOutput out;
            out.state = extendClip(c, std::move(state), id, reverse ? duration + duration : duration);
            return out;
        };
    };
    addBuiltin(registry,
               {"timeRemap.addHold", 1, "Add Hold Segment",
                "Holds the frame shown at a timeline time for a duration inside a remapped clip; the rest of its remap "
                "plays later.",
                Capability::EditSequence,
                remapTarget({{"time", timeParam("Where the hold starts (inside the clip).")},
                             {"duration", timeParam("How long the frame holds.")},
                             {"extendClip", extendParam()}},
                            {"time", "duration"}),
                Json{{"clip", 42}, {"time", "00:00:02:00"}, {"duration", "00:00:01:00"}, {"extendClip", true}}, {},
                insertSegment(false)});
    addBuiltin(registry,
               {"timeRemap.addReverse", 1, "Add Reverse Segment",
                "From a timeline time inside a remapped clip, plays the media backwards at 100 % for a duration, then "
                "forwards again over the same frames (a boomerang); the rest of the remap plays later by twice the "
                "duration. Needs that much media before the point.",
                Capability::EditSequence,
                remapTarget({{"time", timeParam("Where the reverse starts (inside the clip).")},
                             {"duration", timeParam("How long it plays backwards.")},
                             {"extendClip", extendParam()}},
                            {"time", "duration"}),
                Json{{"clip", 42}, {"time", "00:00:02:00"}, {"duration", "00:00:00:12"}}, {},
                insertSegment(true)});

    // --- Titles (TitleOps.h): a generator media item + a clip + its "wabcut.title.style" effect ----------
    addBuiltin(
        registry,
        {"title.add", 1, "New Title",
         "Adds a text title as a clip on a video track: a title media item holding the "
         "text and font, and the clip's \"wabcut.title.style\" effect holding size, fill and stroke colours, stroke "
         "width, shadow, alignment, line spacing and tracking. Place, scale and rotate it with the clip's Transform "
         "(clip.setParam motion.*). \"placement\" chooses the track: the named one (overwriting; the default when "
         "\"track\" is given), the first free one at or above the named or targeted one (the default without \"track\": "
         "nothing under the title is overwritten), or a new track on top. createdIds: the clip, the style effect, the "
         "title media.",
         Capability::EditSequence,
         withSequence({{"text", schema::string("The text (UTF-8; a newline starts a new line).")},
                       {"at", timeParam("Where the title starts.")},
                       {"duration", timeParam("How long it lasts. Default 5 seconds.")},
                       {"track", trackParam("A video track. Default: the first targeted unlocked video track.")},
                       {"placement",
                        schema::enumeration({"targeted", "freeTrackAtOrAbove", "newTrack"},
                                            "\"targeted\": on \"track\", overwriting what is there (the default "
                                            "when \"track\" is given). \"freeTrackAtOrAbove\": the first unlocked "
                                            "video track from \"track\" (or the targeted one) upwards with nothing in "
                                            "the title's time range, a new top track when none is free (the default "
                                            "without \"track\"). \"newTrack\": a new video track on top.")},
                       {"fontFamily", schema::string("Font family. Default \"Arial\".")},
                       {"fontStyle", schema::string("Font style. Default \"Regular\".")},
                       {"style", titleStyleParam()}},
                      {"text", "at"}),
         Json{{"text", "Chapter One"}, {"at", "00:00:10:00"}, {"track", "V2"}, {"style", Json{{"size", 120}}}}, {},
         [](ExecutionContext& c) {
             edit::TitleFont font;
             font.family = c.string("fontFamily", font.family);
             font.style = c.string("fontStyle", font.style);
             const Ticks at = c.time("at");
             const Ticks duration = c.optionalTime("duration").value_or(edit::kDefaultStillDuration);
             ProjectState state = c.state();
             const TrackId track = titleTrack(c, state, at, duration);
             edit::EditResult r = c.take(edit::addTitle(state, c.sequenceId(), track, at, duration, c.string("text"),
                                                        effectParams(c, "style"), font));
             const ClipId clip = r.placed.front();
             const MediaId media = r.createdMedia;
             const EffectInstanceId style = r.createdEffects.front();
             const Sequence& after = *r.state.findSequence(c.sequenceId());
             const Clip& placed = after.findClip(clip)->clip();
             Json data{{"clip", clip.value},
                       {"media", media.value},
                       {"styleEffect", style.value},
                       {"track", "V" + std::to_string(after.trackPosition(track)->index + 1)},
                       {"newTrack", c.sequence().findTrack(track) == nullptr},
                       {"start", c.timecode(placed.start())},
                       {"end", c.timecode(placed.end())}};
             CommandOutput out = edited(std::move(r), std::move(data));
             out.createdIds.push_back(media.value);
             return out;
         }});
    addBuiltin(registry,
               {"title.setText", 1, "Edit Title Text",
                "Replaces a title's text. Every clip of the title shows the new text.", Capability::EditSequence,
                titleTarget({{"text", schema::string("The new text (UTF-8; a newline starts a new line).")}}, {"text"}),
                Json{{"clip", 42}, {"text", "Chapter Two"}}, {},
                [](ExecutionContext& c) {
                    const MediaId title = titleOf(c);
                    return titleEdited(c, c.take(edit::setTitleText(c.state(), title, c.string("text"))), title);
                }});
    addBuiltin(registry,
               {"title.setFont", 1, "Set Title Font", "Changes a title's font family and/or style.",
                Capability::EditSequence,
                titleTarget({{"fontFamily", schema::string("Font family, e.g. \"Noto Sans Thai\".")},
                             {"fontStyle", schema::string("Font style, e.g. \"Bold\".")}},
                            {}),
                Json{{"clip", 42}, {"fontFamily", "Noto Sans Thai"}, {"fontStyle", "Bold"}}, {},
                [](ExecutionContext& c) {
                    const MediaId title = titleOf(c);
                    std::optional<std::string> family;
                    std::optional<std::string> style;
                    if (c.has("fontFamily")) {
                        family = c.string("fontFamily");
                    }
                    if (c.has("fontStyle")) {
                        style = c.string("fontStyle");
                    }
                    if (!family && !style) {
                        fail(ErrorCode::InvalidParams, "give \"fontFamily\" and/or \"fontStyle\"");
                    }
                    return titleEdited(c, c.take(edit::setTitleFont(c.state(), title, family, style)), title);
                }});
}

} // namespace wabcut::command::detail
