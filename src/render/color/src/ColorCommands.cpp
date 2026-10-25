#include <wabcut/color/ColorCommands.h>

#include <wabcut/color/Cdl.h>
#include <wabcut/color/Looks.h>
#include <wabcut/color/Scopes.h>
#include <wabcut/command/Schema.h>
#include <wabcut/model/ColorManagement.h>
#include <wabcut/model/ColorManagementOps.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>

namespace wabcut::color {

using command::CommandOutput;
using command::ErrorCode;
using command::ExecutionContext;
using command::fail;
using command::Json;
namespace schema = command::schema;

namespace {

// ---- Schemas -------------------------------------------------------------------------------------------------

Json withSequence(std::initializer_list<std::pair<const char*, Json>> properties, std::vector<std::string> required = {}) {
    Json s = schema::object(properties, std::move(required));
    s["properties"]["sequence"] = schema::integer("Sequence id; only needed when the project has several.", 1);
    return s;
}

Json timeParam(std::string description) {
    return Json{{"type", Json::array({"integer", "string"})},
                {"description", description + " Timecode in the sequence's rate (\"00:01:02:03\") or ticks."}};
}

Json clipParam(std::string description) {
    return Json{{"type", Json::array({"integer", "object"})},
                {"description", description + " A clip id, or {\"track\", \"time\"} for the clip there."},
                {"properties", Json{{"track", Json{{"type", Json::array({"string", "integer"})}, {"description", "\"V1\" or a track id."}, {"minimum", 1}}},
                                    {"time", timeParam("A time inside the clip.")}}},
                {"required", Json::array({"track", "time"})},
                {"additionalProperties", false},
                {"minimum", 1}};
}

Json clipsParam(std::string description) { return schema::array(clipParam("A clip."), std::move(description), 1); }

Json numberMap(std::string description) { return Json{{"type", "object"}, {"description", std::move(description)}}; }

Json strengthParam() { return schema::number("How far to go, 0..1. Default 1.", 0.0, 1.0); }

Json gradeEffectParam() {
    return schema::integer("The grade's effect instance id when the clip has several (clip.details lists them). Default: its "
                           "first grade.",
                           1);
}

// ---- Helpers -------------------------------------------------------------------------------------------------

ColorServices& servicesOf(ExecutionContext& c) {
    auto* services = command::findService<ColorServices>(c.services());
    if (!services) {
        fail(ErrorCode::InvalidRequest, "pictures are not available here: the host renders none (wabcut-cli and the app do)");
    }
    return *services;
}

void add(command::CommandRegistry& registry, command::CommandSpec spec) {
    [[maybe_unused]] const auto added = registry.add(std::move(spec));
}

const EffectDescriptor& gradeDescriptor() { return *EffectRegistry::builtin().find(kGradeEffect); }

double rounded(double v, double unit) { return std::round(v / unit) * unit; }

/// The clips of "clips" or "clip".
std::vector<ClipId> clipsOf(ExecutionContext& c) {
    if (c.has("clips")) {
        return c.clips("clips");
    }
    if (c.has("clip")) {
        return {c.clip("clip")};
    }
    fail(ErrorCode::InvalidParams, "name the \"clip\" or \"clips\"");
}

const Clip& clipIn(const ProjectState& state, SequenceId sequence, ClipId id) {
    const auto loc = state.findSequence(sequence)->findClip(id);
    if (!loc) {
        fail(ErrorCode::NotFound, "clip " + std::to_string(id.value) + " does not exist");
    }
    return loc->clip();
}

/// The clip's grade, added (at the end of its stack) when it has none. Returns the state and the instance id.
std::pair<ProjectState, EffectInstanceId> ensureGrade(ExecutionContext& c, ProjectState state, ClipId clip, EffectInstanceId preferred) {
    const Clip& cl = clipIn(state, c.sequenceId(), clip);
    if (const EffectInstance* g = gradeOf(cl, preferred)) {
        return {std::move(state), g->id};
    }
    if (preferred.isValid()) {
        fail(ErrorCode::NotFound, "clip " + std::to_string(clip.value) + " has no grade " + std::to_string(preferred.value));
    }
    if (cl.stream.kind != StreamKind::Video) {
        fail(ErrorCode::EditRejected, "clip " + std::to_string(clip.value) + " is not a video clip");
    }
    const std::vector<ClipId> one{clip};
    edit::EditResult added = c.take(edit::addEffect(state, c.sequenceId(), one, std::string(kGradeEffect)));
    const EffectInstanceId id = added.createdEffects.at(0);
    return {std::move(added.state), id};
}

/// Parameters clamped into the descriptor's ranges (tools may land on an edge); warnings for clamped ones.
std::vector<EffectParam> clampedParams(const std::vector<EffectParam>& params, std::vector<std::string>& warnings) {
    std::vector<EffectParam> out;
    for (const EffectParam& p : params) {
        const EffectParamInfo* info = gradeDescriptor().param(p.key);
        if (!info) {
            continue;
        }
        const double v = p.value.constant();
        const double clamped = std::clamp(v, info->minValue, info->maxValue);
        if (clamped != v) {
            warnings.push_back(std::format("{} {} is outside {}..{}; set to {}", p.key, v, info->minValue, info->maxValue, clamped));
        }
        out.push_back(EffectParam{p.key, ParamValue(clamped)});
    }
    return out;
}

/// Writes `params` (only the given keys) to the clip's grade.
ProjectState setGrade(ExecutionContext& c, ProjectState state, ClipId clip, EffectInstanceId effect, std::vector<EffectParam> params,
                      std::vector<EffectData> data = {}) {
    std::sort(params.begin(), params.end(), [](const EffectParam& a, const EffectParam& b) { return a.key < b.key; });
    edit::EffectChange change;
    change.params = std::move(params);
    change.data = std::move(data);
    return c.take(edit::setEffect(state, c.sequenceId(), clip, effect, change)).state;
}

/// Basic parameters a tool computed, as effect params (only the fields it owns).
std::vector<EffectParam> basicParams(const GradeParams& p, std::initializer_list<std::string_view> keys) {
    std::vector<EffectParam> out;
    for (const EffectParam& e : gradeParamsToEffect(p, false)) {
        if (std::find(keys.begin(), keys.end(), e.key) != keys.end()) {
            out.push_back(e);
        }
    }
    out.push_back(EffectParam{"basic.enabled", ParamValue(1.0)});
    return out;
}

/// A time inside the clip: "time" when given (and inside), else the clip's middle frame.
Ticks sampleTime(ExecutionContext& c, const Clip& clip, const char* key = "time") {
    if (c.has(key)) {
        const Ticks t = c.time(key);
        if (t < clip.start() || t >= clip.end()) {
            fail(ErrorCode::InvalidParams, "the time lies outside the clip", Json{{"path", std::string("/") + key}});
        }
        return t;
    }
    const Ticks mid = clip.start() + Ticks{(clip.end() - clip.start()).value / 2};
    return std::max(clip.start(), snapToFrame(mid, c.sequence().frameRate));
}

/// 0.2 face priority: what the match looked at - the faces found on each picture.
struct FaceReport {
    bool asked = false;
    int found = -1; ///< faces on the picture (-1: not asked)
};

std::vector<ColorSample> samplesOf(ExecutionContext& c, const ProjectState& state, const FrameRequest& request, const char* what,
                                   FaceReport* faces = nullptr, std::vector<std::string>* warnings = nullptr) {
    auto image = servicesOf(c).frame(state, c.sequenceId(), request);
    if (!image) {
        fail(image.error().code == wabcut::ErrorCode::Cancelled ? ErrorCode::Cancelled : ErrorCode::EditRejected,
             std::string("cannot render ") + what + ": " + image.error().message);
    }
    std::vector<float> weights;
    if (faces && faces->asked) {
        // Face Detection: the faces' skin counts most, so the skin tones match first (Premiere's Color Match).
        auto* finder = command::findService<FaceFinder>(c.services());
        if (!finder) {
            fail(ErrorCode::EditRejected, "Face Detection needs the face detector (Settings > AI > Local models): match without it, or "
                                          "with \"skinPriority\"");
        }
        auto found = finder->faces(*image);
        if (!found) {
            fail(found.error().code == wabcut::ErrorCode::Cancelled ? ErrorCode::Cancelled : ErrorCode::EditRejected,
                 std::string("cannot find the faces on ") + what + ": " + found.error().message);
        }
        faces->found = static_cast<int>(found->size());
        weights = faceWeights(*image, *found);
        if (found->empty() && warnings) {
            warnings->push_back(std::string("no face on ") + what + ": it was matched as a whole");
        }
    }
    auto samples = samplePixels(*image, 4096, weights.empty() ? nullptr : &weights);
    if (samples.empty()) {
        fail(ErrorCode::EditRejected, std::string(what) + " shows nothing at that time");
    }
    return samples;
}

/// The grade spec of the clip's grade (its other stages) at `time`, for the tools.
GradeSpec gradeSpecAt(const ProjectState& state, const Clip& clip, EffectInstanceId effect, Ticks time) {
    for (const EffectInstance& e : clip.effects()) {
        if (e.id == effect) {
            return evaluateGrade(e, clip.curveTimeAt(time), LutLibrary::shared().resolver(state));
        }
    }
    return {};
}

Json paramsJson(const GradeParams& p, bool onlyChanged) {
    Json out = Json::object();
    for (const EffectParam& e : gradeParamsToEffect(p, onlyChanged)) {
        out[e.key] = rounded(e.value.constant(), 0.0001);
    }
    return out;
}

std::string_view curveKey(std::string_view name) {
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 9> kCurves{{
        {"master", gradekey::master}, {"red", gradekey::red}, {"green", gradekey::green},
        {"blue", gradekey::blue}, {"hueSat", gradekey::hueSat}, {"hueHue", gradekey::hueHue},
        {"hueLuma", gradekey::hueLuma}, {"lumaSat", gradekey::lumaSat}, {"satSat", gradekey::satSat},
    }};
    for (const auto& [n, key] : kCurves) {
        if (n == name) {
            return key;
        }
    }
    return {};
}

/// Validates a LUT reference ("" clears): a known built-in or an asset of the project.
std::string lutReference(ExecutionContext& c, const std::string& value, const char* path) {
    if (value.empty()) {
        return value;
    }
    if (value.starts_with(kBuiltinPrefix)) {
        if (!builtinLut(value)) {
            fail(ErrorCode::InvalidParams, "\"" + value + "\" is not a built-in LUT (lut.list lists them)", Json{{"path", path}});
        }
        return value;
    }
    const AssetId asset = assetOfReference(value);
    if (!asset.isValid() || !c.state().findAsset(asset)) {
        fail(ErrorCode::InvalidParams, "\"" + value + "\" is not a LUT of this project (lut.import adds one; lut.list lists them)",
             Json{{"path", path}});
    }
    return value;
}

// ---- color.grade.set / get -------------------------------------------------------------------------------------

CommandOutput gradeSet(ExecutionContext& c) {
    std::vector<EffectParam> params;
    std::vector<EffectData> data;
    if (c.has("params")) {
        for (const auto& item : c.params()["params"].items()) {
            const EffectParamInfo* info = gradeDescriptor().param(item.key());
            if (!info) {
                fail(ErrorCode::InvalidParams, "\"" + item.key() + "\" is not a grade parameter (effect.list lists wabcut.color.grade's)",
                     Json{{"path", "/params/" + item.key()}});
            }
            if (!item.value().is_number()) {
                fail(ErrorCode::InvalidParams, item.key() + " must be a number", Json{{"path", "/params/" + item.key()}});
            }
            params.push_back(EffectParam{item.key(), ParamValue(item.value().get<double>())});
        }
    }
    if (c.has("stages")) {
        for (const auto& item : c.params()["stages"].items()) {
            const std::string key = item.key() + ".enabled";
            if (!gradeDescriptor().param(key) || !item.value().is_boolean()) {
                fail(ErrorCode::InvalidParams, "stages are basic, creative, curves, wheels, hsl and vignette, each true or false",
                     Json{{"path", "/stages/" + item.key()}});
            }
            params.push_back(EffectParam{key, ParamValue(item.value().get<bool>() ? 1.0 : 0.0)});
        }
    }
    if (c.has("curves")) {
        for (const auto& item : c.params()["curves"].items()) {
            const std::string_view key = curveKey(item.key());
            if (key.empty() || !item.value().is_array()) {
                fail(ErrorCode::InvalidParams,
                     "curves are master, red, green, blue, hueSat, hueHue, hueLuma, lumaSat, satSat: arrays of [x, y] (empty = reset)",
                     Json{{"path", "/curves/" + item.key()}});
            }
            std::vector<EffectPoint> points;
            for (const Json& p : item.value()) {
                if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number()) {
                    fail(ErrorCode::InvalidParams, "a curve point is [x, y]", Json{{"path", "/curves/" + item.key()}});
                }
                points.push_back({p[0].get<double>(), p[1].get<double>()});
            }
            std::sort(points.begin(), points.end(), [](const EffectPoint& a, const EffectPoint& b) { return a.x < b.x; });
            data.push_back(EffectData{std::string(key), std::move(points)});
        }
    }
    if (c.has("inputLut")) {
        data.push_back(EffectData{std::string(gradekey::inputLut), lutReference(c, c.string("inputLut"), "/inputLut")});
    }
    if (c.has("look")) {
        data.push_back(EffectData{std::string(gradekey::look), lutReference(c, c.string("look"), "/look")});
    }
    if (params.empty() && data.empty() && !c.has("enabled")) {
        fail(ErrorCode::InvalidParams, "nothing to set: give params, stages, curves, inputLut, look or enabled");
    }
    std::sort(data.begin(), data.end(), [](const EffectData& a, const EffectData& b) { return a.key < b.key; });
    const EffectInstanceId preferred{static_cast<std::uint64_t>(c.integer("effect", 0))};
    ProjectState state = c.state();
    CommandOutput out;
    Json clips = Json::array();
    for (const ClipId clip : clipsOf(c)) {
        auto [next, effect] = ensureGrade(c, std::move(state), clip, preferred);
        state = std::move(next);
        edit::EffectChange change;
        change.params = params;
        std::sort(change.params.begin(), change.params.end(), [](const EffectParam& a, const EffectParam& b) { return a.key < b.key; });
        change.data = data;
        if (c.has("enabled")) {
            change.enabled = c.boolean("enabled", true);
        }
        state = c.take(edit::setEffect(state, c.sequenceId(), clip, effect, change)).state;
        out.createdIds.push_back(effect.value);
        clips.push_back(Json{{"clip", clip.value}, {"effect", effect.value}});
    }
    out.state = std::move(state);
    out.data = Json{{"clips", std::move(clips)}};
    return out;
}

CommandOutput gradeGet(ExecutionContext& c) {
    const ClipId id = c.clip("clip");
    const Clip& clip = clipIn(c.state(), c.sequenceId(), id);
    const EffectInstance* g = gradeOf(clip, EffectInstanceId{static_cast<std::uint64_t>(c.integer("effect", 0))});
    CommandOutput out;
    out.state = c.state();
    if (!g) {
        out.data = Json{{"clip", id.value}, {"grade", nullptr}};
        return out;
    }
    const Ticks t = sampleTime(c, clip);
    const GradeSpec spec = evaluateGrade(*g, clip.curveTimeAt(t), LutLibrary::shared().resolver(c.state()));
    Json curves = Json::object();
    for (const char* name : {"master", "red", "green", "blue", "hueSat", "hueHue", "hueLuma", "lumaSat", "satSat"}) {
        if (const EffectData* d = g->findData(curveKey(name)); d && d->points()) {
            Json pts = Json::array();
            for (const EffectPoint& p : *d->points()) {
                pts.push_back(Json::array({p.x, p.y}));
            }
            curves[name] = std::move(pts);
        }
    }
    Json animated = Json::array();
    for (const EffectParam& p : g->params) {
        if (p.value.isCurve()) {
            animated.push_back(p.key);
        }
    }
    const auto text = [&](std::string_view key) -> Json {
        const EffectData* d = g->findData(key);
        return d && d->text() ? Json(*d->text()) : Json(nullptr);
    };
    out.data = Json{{"clip", id.value},
                    {"effect", g->id.value},
                    {"enabled", g->enabled},
                    {"time", c.timecode(t)},
                    {"params", paramsJson(spec.params, c.boolean("all", false) ? false : true)},
                    {"animated", std::move(animated)},
                    {"curves", std::move(curves)},
                    {"inputLut", text(gradekey::inputLut)},
                    {"look", text(gradekey::look)},
                    {"missingLuts", spec.missingLuts},
                    {"active", spec.active()}};
    return out;
}

// ---- Auto / Match --------------------------------------------------------------------------------------------

CommandOutput autoCommand(ExecutionContext& c) {
    const EffectInstanceId preferred{static_cast<std::uint64_t>(c.integer("effect", 0))};
    ProjectState state = c.state();
    CommandOutput out;
    Json results = Json::array();
    for (const ClipId clip : clipsOf(c)) {
        auto [next, effect] = ensureGrade(c, std::move(state), clip, preferred);
        state = std::move(next);
        const Clip& cl = clipIn(state, c.sequenceId(), clip);
        const Ticks t = sampleTime(c, cl);
        c.checkpoint("analysing clip " + std::to_string(clip.value));
        const auto samples = samplesOf(c, state, FrameRequest{t, clip, effect}, "the clip");
        GradeSpec base = gradeSpecAt(state, cl, effect, t);
        const AutoResult result = autoCorrect(samples, base);
        state = setGrade(c, std::move(state), clip, effect,
                         clampedParams(basicParams(result.params, {"basic.temperature", "basic.tint", "basic.exposure", "basic.whites",
                                                                   "basic.blacks"}),
                                       out.warnings));
        results.push_back(Json{{"clip", clip.value},
                               {"effect", effect.value},
                               {"time", c.timecode(t)},
                               {"params", Json{{"basic.temperature", result.params.temperature},
                                               {"basic.tint", result.params.tint},
                                               {"basic.exposure", result.params.exposure},
                                               {"basic.whites", result.params.whites},
                                               {"basic.blacks", result.params.blacks}}},
                               {"castBefore", rounded(result.castBefore, 0.0001)},
                               {"castAfter", rounded(result.castAfter, 0.0001)},
                               {"summary", result.summary}});
        out.createdIds.push_back(effect.value);
    }
    out.state = std::move(state);
    out.data = Json{{"clips", std::move(results)}};
    return out;
}

struct Reference {
    std::vector<ColorSample> samples;
    std::string what;
    FaceReport faces;
};

/// The reference frame: {"clip", "time"} (that clip as it looks, alone) or a sequence time (the whole picture).
Reference referenceOf(ExecutionContext& c, const ProjectState& state, const Json& ref, bool faceDetection = false,
                      std::vector<std::string>* warnings = nullptr) {
    Reference r;
    r.faces.asked = faceDetection;
    if (ref.is_object() && ref.contains("clip")) {
        const ClipId clip = c.clipValue(ref["clip"], "/reference/clip");
        const Clip& cl = clipIn(state, c.sequenceId(), clip);
        Ticks t{};
        if (ref.contains("time")) {
            t = c.timeValue(ref["time"], "/reference/time");
            if (t < cl.start() || t >= cl.end()) {
                fail(ErrorCode::InvalidParams, "the reference time lies outside the reference clip", Json{{"path", "/reference/time"}});
            }
        } else {
            t = std::max(cl.start(), snapToFrame(cl.start() + Ticks{(cl.end() - cl.start()).value / 2}, c.sequence().frameRate));
        }
        r.samples = samplesOf(c, state, FrameRequest{t, clip, {}}, "the reference clip", &r.faces, warnings);
        r.what = "clip " + std::to_string(clip.value) + " at " + c.timecode(t);
        return r;
    }
    const Ticks t = ref.is_object() && ref.contains("time") ? c.timeValue(ref["time"], "/reference/time") : c.timeValue(ref, "/reference");
    r.samples = samplesOf(c, state, FrameRequest{t, {}, {}}, "the reference frame", &r.faces, warnings);
    r.what = "the sequence at " + c.timecode(t);
    return r;
}

Json matchOne(ExecutionContext& c, ProjectState& state, ClipId clip, EffectInstanceId preferred, const Reference& reference,
              const MatchOptions& options, std::vector<std::string>& warnings, bool faceDetection = false) {
    auto [next, effect] = ensureGrade(c, std::move(state), clip, preferred);
    state = std::move(next);
    const Clip& cl = clipIn(state, c.sequenceId(), clip);
    const Ticks t = sampleTime(c, cl, "__none");
    c.checkpoint("matching clip " + std::to_string(clip.value));
    FaceReport faces;
    faces.asked = faceDetection;
    const std::string what = "clip " + std::to_string(clip.value);
    const auto samples = samplesOf(c, state, FrameRequest{t, clip, effect}, what.c_str(), &faces, &warnings);
    const GradeSpec base = gradeSpecAt(state, cl, effect, t);
    const MatchResult result = colorMatch(samples, reference.samples, base, options);
    state = setGrade(c, std::move(state), clip, effect,
                     clampedParams(basicParams(result.params, {"basic.exposure", "basic.contrast", "basic.temperature", "basic.tint",
                                                               "basic.saturation"}),
                                   warnings));
    const auto stats = [](const OklabStats& s) {
        return Json{{"L", rounded(s.mean[0], 0.0001)}, {"a", rounded(s.mean[1], 0.0001)}, {"b", rounded(s.mean[2], 0.0001)},
                    {"sdL", rounded(s.deviation[0], 0.0001)}, {"sdA", rounded(s.deviation[1], 0.0001)}, {"sdB", rounded(s.deviation[2], 0.0001)}};
    };
    Json out{{"clip", clip.value},
             {"effect", effect.value},
             {"params", Json{{"basic.exposure", result.params.exposure},
                             {"basic.contrast", result.params.contrast},
                             {"basic.temperature", result.params.temperature},
                             {"basic.tint", result.params.tint},
                             {"basic.saturation", result.params.saturation}}},
             {"before", stats(result.current)},
             {"after", stats(result.achieved)},
             {"reference", stats(result.reference)},
             {"summary", result.summary}};
    if (faceDetection) {
        out["faces"] = faces.found;
    }
    return out;
}

MatchOptions matchOptions(ExecutionContext& c) {
    MatchOptions o;
    if (c.has("strength")) {
        o.strength = c.number("strength");
    }
    o.skinPriority = c.boolean("skinPriority", false);
    return o;
}

CommandOutput matchCommand(ExecutionContext& c) {
    ProjectState state = c.state();
    const bool faceDetection = c.boolean("faceDetection", false);
    CommandOutput out;
    const Reference reference = referenceOf(c, state, c.params()["reference"], faceDetection, &out.warnings);
    const MatchOptions options = matchOptions(c);
    const EffectInstanceId preferred{static_cast<std::uint64_t>(c.integer("effect", 0))};
    Json results = Json::array();
    for (const ClipId clip : clipsOf(c)) {
        results.push_back(matchOne(c, state, clip, preferred, reference, options, out.warnings, faceDetection));
        out.createdIds.push_back(results.back()["effect"].get<std::uint64_t>());
    }
    out.state = std::move(state);
    out.data = Json{{"reference", reference.what}, {"clips", std::move(results)}};
    if (faceDetection) {
        out.data["referenceFaces"] = reference.faces.found;
    }
    return out;
}

CommandOutput matchSceneCommand(ExecutionContext& c) {
    ProjectState state = c.state();
    const ClipId refClip = c.clip("reference");
    const Sequence& seq = c.sequence();
    const auto refLoc = seq.findClip(refClip);
    const std::string scope = c.string("scope", "scene");
    std::vector<ClipId> clips;
    if (c.has("clips")) {
        clips = c.clips("clips");
    } else if (scope == "scene") {
        clips = sceneClips(seq, refClip);
    } else if (scope == "track" || scope == "media") {
        for (const Clip& cl : refLoc->track->clips) {
            if (cl.enabled && (scope == "track" || sameSource(cl, refLoc->clip()))) {
                clips.push_back(cl.id);
            }
        }
        if (scope == "media") {
            seq.forEachTrack([&](const Track& t) {
                if (t.kind != TrackKind::Video || t.id == refLoc->track->id) {
                    return;
                }
                for (const Clip& cl : t.clips) {
                    if (sameSource(cl, refLoc->clip())) {
                        clips.push_back(cl.id);
                    }
                }
            });
        }
    }
    std::erase(clips, refClip);
    if (clips.empty()) {
        fail(ErrorCode::EditRejected, scope == "scene"
                                          ? "the reference clip's scene has no other clips (split the take at its shots first: clip.splitAtShots)"
                                          : "no other clips to match");
    }
    Json ref{{"clip", refClip.value}};
    const bool faceDetection = c.boolean("faceDetection", false);
    CommandOutput out;
    const Reference reference = referenceOf(c, state, ref, faceDetection, &out.warnings);
    const MatchOptions options = matchOptions(c);
    Json results = Json::array();
    for (const ClipId clip : clips) {
        results.push_back(matchOne(c, state, clip, {}, reference, options, out.warnings, faceDetection));
    }
    out.state = std::move(state);
    out.data = Json{{"reference", reference.what}, {"scope", c.has("clips") ? "clips" : scope}, {"clips", std::move(results)}};
    if (faceDetection) {
        out.data["referenceFaces"] = reference.faces.found;
    }
    return out;
}

// ---- color.grade.copy (0.4 Apply Grade) -----------------------------------------------------------------------

/// A key's curve time on `from` as the curve time on `to` at the same time from the clip's start (Paste Attributes'
/// mapping without stretching): exact when both play at the same rate.
Ticks mapKeyTime(const Clip& from, const Clip& to, Ticks t) {
    const auto rate = [](const Clip& cl) -> std::pair<std::int64_t, std::int64_t> {
        if (cl.isFreeze() || cl.isRemapped()) {
            return {1, 1};
        }
        return {cl.reversed ? -std::int64_t{cl.speed.num} : std::int64_t{cl.speed.num}, cl.speed.den};
    };
    const Ticks fromStart = from.curveTimeAt(from.start());
    const Ticks toStart = to.curveTimeAt(to.start());
    const auto a = rate(from);
    const auto b = rate(to);
    if (a.first * b.second == b.first * a.second) {
        return toStart + (t - fromStart);
    }
    const long double u = static_cast<long double>((t - fromStart).value) * static_cast<long double>(a.second) /
                          static_cast<long double>(a.first);
    const long double v = u * static_cast<long double>(b.first) / static_cast<long double>(b.second);
    return toStart + Ticks{static_cast<std::int64_t>(std::llround(static_cast<double>(v)))};
}

ParamValue mapKeys(const ParamValue& value, const Clip& from, const Clip& to) {
    const Curve* curve = value.curve();
    if (!curve) {
        return value;
    }
    Curve out = *curve;
    for (Keyframe& k : out.keys) {
        k.time = mapKeyTime(from, to, k.time);
    }
    std::sort(out.keys.begin(), out.keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    out.keys.erase(std::unique(out.keys.begin(), out.keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.time == b.time; }),
                   out.keys.end());
    return ParamValue(std::move(out));
}

/// The change that makes `target`'s grade (`current`, or none yet) equal to `source` on `from`: every descriptor
/// parameter (the source's value or curve, else the default), every data entry of the source (curves, LUTs), the
/// target's own other data entries removed, and the on / off state. Masks are not touched (a mask is drawn for one shot).
edit::EffectChange gradeCopyChange(const EffectInstance& source, const Clip& from, const Clip& target,
                                   const EffectInstance* current) {
    edit::EffectChange change;
    change.enabled = source.enabled;
    for (const EffectParamInfo& p : gradeDescriptor().params) {
        const ParamValue* v = source.find(p.key);
        change.params.push_back(EffectParam{std::string(p.key), v ? mapKeys(*v, from, target) : ParamValue(p.defaultValue)});
    }
    std::sort(change.params.begin(), change.params.end(), [](const EffectParam& a, const EffectParam& b) { return a.key < b.key; });
    change.data = source.data;
    if (current) {
        for (const EffectData& d : current->data) {
            if (!source.findData(d.key)) {
                EffectData gone{d.key, {}};
                if (d.text()) {
                    gone.value = std::string();
                }
                change.data.push_back(std::move(gone));
            }
        }
    }
    std::sort(change.data.begin(), change.data.end(), [](const EffectData& a, const EffectData& b) { return a.key < b.key; });
    return change;
}

CommandOutput gradeCopy(ExecutionContext& c) {
    const ClipId fromId = c.clip("from");
    const Clip from = clipIn(c.state(), c.sequenceId(), fromId);
    const EffectInstance* found = gradeOf(from);
    if (!found) {
        fail(ErrorCode::EditRejected, "clip " + std::to_string(fromId.value) + " has no grade to copy");
    }
    const EffectInstance source = *found;
    const std::string scope = c.string("scope", "clips");
    const bool replace = c.boolean("replace", true);
    const Sequence& seq = c.sequence();
    std::vector<ClipId> targets;
    if (scope == "media") {
        // Every video clip of the source's media (the same nested sequence and angle for nested / multicam clips).
        seq.forEachTrack([&](const Track& t) {
            if (t.kind != TrackKind::Video) {
                return;
            }
            for (const Clip& cl : t.clips) {
                if (cl.id != fromId && sameSource(cl, from)) {
                    targets.push_back(cl.id);
                }
            }
        });
    } else if (scope == "clips") {
        if (!c.has("clips")) {
            fail(ErrorCode::InvalidParams, "name the \"clips\" to copy the grade to, or use scope \"media\"");
        }
        targets = c.clips("clips");
    } else {
        fail(ErrorCode::InvalidParams, "scope is \"clips\" or \"media\"", Json{{"path", "/scope"}});
    }

    ProjectState state = c.state();
    CommandOutput out;
    Json results = Json::array();
    int changed = 0;
    for (const ClipId id : targets) {
        const auto skip = [&](const std::string& reason) {
            results.push_back(Json{{"clip", id.value}, {"result", "skipped"}, {"reason", reason}});
        };
        const auto loc = state.findSequence(c.sequenceId())->findClip(id);
        if (!loc) {
            fail(ErrorCode::NotFound, "clip " + std::to_string(id.value) + " does not exist");
        }
        const Clip target = loc->clip();
        if (id == fromId) {
            skip("the clip the grade comes from");
            continue;
        }
        if (target.stream.kind != StreamKind::Video) {
            skip("not a video clip");
            continue;
        }
        if (loc->track->locked) {
            skip("on a locked track");
            continue;
        }
        const EffectInstance* current = replace ? gradeOf(target) : nullptr;
        std::optional<EffectInstance> before = current ? std::optional<EffectInstance>(*current) : std::nullopt;
        EffectInstanceId effect;
        if (current) {
            effect = current->id;
        } else {
            const std::vector<ClipId> one{id};
            edit::EditResult added = c.take(edit::addEffect(state, c.sequenceId(), one, std::string(kGradeEffect)));
            effect = added.createdEffects.at(0);
            state = std::move(added.state);
        }
        const edit::EffectChange change = gradeCopyChange(source, from, target, before ? &*before : nullptr);
        state = c.take(edit::setEffect(state, c.sequenceId(), id, effect, change)).state;
        const EffectInstance* after = gradeOf(clipIn(state, c.sequenceId(), id), effect);
        const bool differs = !before || !after || !(*after == *before);
        if (differs) {
            ++changed;
            out.createdIds.push_back(effect.value);
        }
        results.push_back(Json{{"clip", id.value},
                               {"result", current ? (differs ? "replaced" : "skipped") : "added"},
                               {"reason", differs ? "" : "it already has this grade"},
                               {"effect", effect.value}});
    }
    if (changed == 0) {
        fail(ErrorCode::EditRejected, targets.empty() ? (scope == "media" ? "no other clips show this source" : "no clips to copy the grade to")
                                                      : "no clip changes: the targets are skipped or already have this grade",
             Json{{"clips", std::move(results)}});
    }
    out.state = std::move(state);
    out.data = Json{{"from", fromId.value}, {"scope", scope}, {"clips", std::move(results)}, {"changed", changed}};
    return out;
}

// ---- LUTs ----------------------------------------------------------------------------------------------------

CommandOutput lutImport(ExecutionContext& c) {
    const std::filesystem::path file = pathFromUtf8(c.string("path"));
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        fail(ErrorCode::NotFound, "no file at " + c.string("path"), Json{{"path", "/path"}});
    }
    std::string ext = pathToUtf8(file.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (ext != ".cube") {
        fail(ErrorCode::InvalidParams, "only .cube LUTs can be imported", Json{{"path", "/path"}});
    }
    const auto lut = loadCube(file);
    if (!lut) {
        fail(ErrorCode::EditRejected, "the LUT cannot be read: " + lut.error().message);
    }
    const std::filesystem::path absolute = std::filesystem::absolute(file, ec);
    // The same file again: the existing asset.
    for (const ProjectAsset& a : c.state().assets) {
        if (a.kind == kAssetKindLut && pathFromUtf8(a.locator.path) == absolute) {
            CommandOutput out;
            out.state = c.state();
            out.data = Json{{"asset", a.id.value}, {"reference", assetReference(a.id)}, {"name", a.name}, {"existing", true}};
            return out;
        }
    }
    ProjectAsset asset;
    asset.kind = std::string(kAssetKindLut);
    asset.name = c.has("name") ? c.string("name") : pathToUtf8(file.filename());
    asset.locator.path = pathToUtf8(absolute);
    if (auto fp = fingerprintFile(absolute)) {
        asset.locator.fingerprint = *fp;
    }
    auto created = c.take(edit::addAsset(c.state(), std::move(asset)));
    CommandOutput out;
    out.state = std::move(created.state);
    out.createdIds.push_back(created.id.value);
    out.data = Json{{"asset", created.id.value},
                    {"reference", assetReference(created.id)},
                    {"name", out.state.findAsset(created.id)->name},
                    {"title", lut->title},
                    {"size3d", lut->size3d},
                    {"size1d", lut->size1d}};
    return out;
}

CommandOutput lutList(ExecutionContext& c) {
    Json builtins = Json::array();
    for (const BuiltinLut& b : builtinLuts()) {
        builtins.push_back(Json{{"reference", std::string(kBuiltinPrefix) + std::string(b.name)},
                                {"name", b.displayName},
                                {"kind", b.input ? "input" : "look"},
                                {"description", b.description},
                                {"source", b.source}});
    }
    // Which clips use each asset.
    std::map<std::string, int> uses;
    for (const SequencePtr& seq : c.state().sequences) {
        seq->forEachTrack([&](const Track& t) {
            for (const Clip& cl : t.clips) {
                for (const EffectInstance& e : cl.effects()) {
                    for (const EffectData& d : e.data) {
                        if (d.text()) {
                            ++uses[*d.text()];
                        }
                    }
                }
            }
        });
    }
    Json assets = Json::array();
    for (const ProjectAsset& a : c.state().assets) {
        if (a.kind != kAssetKindLut) {
            continue;
        }
        const std::string ref = assetReference(a.id);
        assets.push_back(Json{{"asset", a.id.value},
                              {"reference", ref},
                              {"name", a.name},
                              {"path", a.locator.path},
                              {"offline", a.locator.offline},
                              {"usedBy", uses[ref]}});
    }
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"builtins", std::move(builtins)}, {"assets", std::move(assets)}};
    return out;
}

// ---- Export .cube (0.2) ------------------------------------------------------------------------------------------

CommandOutput lutExport(ExecutionContext& c) {
    const std::filesystem::path file = pathFromUtf8(c.string("path"));
    std::string ext = pathToUtf8(file.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (ext != ".cube") {
        fail(ErrorCode::InvalidParams, "the file ends in .cube", Json{{"path", "/path"}});
    }
    std::error_code ec;
    if (std::filesystem::exists(file, ec) && !c.boolean("overwrite", false)) {
        fail(ErrorCode::EditRejected, "the file exists; pass \"overwrite\": true to replace it", Json{{"path", "/path"}});
    }
    const ClipId id = c.clip("clip");
    const Clip& cl = clipIn(c.state(), c.sequenceId(), id);
    const EffectInstance* g = gradeOf(cl, EffectInstanceId{static_cast<std::uint64_t>(c.integer("effect", 0))});
    if (!g) {
        fail(ErrorCode::NotFound, "clip " + std::to_string(id.value) + " has no colour grade", Json{{"path", "/clip"}});
    }
    const Ticks t = sampleTime(c, cl);
    const GradeSpec spec = evaluateGrade(*g, cl.curveTimeAt(t), LutLibrary::shared().resolver(c.state()));
    const int size = static_cast<int>(c.integer("size", 33));
    std::string title = c.string("title");
    if (title.empty()) {
        title = cl.name().empty() ? std::string("Wabcut grade") : cl.name();
    }
    const Lut lut = gradeToLut(spec, size, title);
    if (!c.dryRun()) {
        if (auto written = io::writeFileAtomically(file, writeCube(lut)); !written) {
            fail(ErrorCode::EditRejected, "cannot write the LUT: " + written.error().message, Json{{"path", "/path"}});
        }
    }
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"path", pathToUtf8(file)},
                    {"size", lut.size3d},
                    {"title", lut.title},
                    {"time", t.value},
                    {"enabled", g->enabled},
                    {"leftOut", gradeLutOmissions(spec, *g)}};
    if (!g->enabled) {
        out.warnings.push_back("the grade is turned off; the LUT holds it as if it were on");
    }
    return out;
}

// ---- CDL -----------------------------------------------------------------------------------------------------

std::filesystem::path cdlPath(ExecutionContext& c) {
    const std::filesystem::path file = pathFromUtf8(c.string("path"));
    std::string ext = pathToUtf8(file.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (ext != ".cc" && ext != ".ccc" && ext != ".cdl") {
        fail(ErrorCode::InvalidParams, "a CDL file ends in .cc, .ccc or .cdl", Json{{"path", "/path"}});
    }
    return file;
}

CommandOutput cdlExport(ExecutionContext& c) {
    const std::filesystem::path file = cdlPath(c);
    std::error_code ec;
    if (std::filesystem::exists(file, ec) && !c.boolean("overwrite", false)) {
        fail(ErrorCode::EditRejected, "the file exists; pass \"overwrite\": true to replace it", Json{{"path", "/path"}});
    }
    std::vector<Cdl> corrections;
    for (const ClipId clip : clipsOf(c)) {
        const Clip& cl = clipIn(c.state(), c.sequenceId(), clip);
        const EffectInstance* g = gradeOf(cl, EffectInstanceId{static_cast<std::uint64_t>(c.integer("effect", 0))});
        GradeParams p = g ? gradeParamsAt(*g, cl.curveTimeAt(sampleTime(c, cl))) : GradeParams{};
        Cdl cdl = cdlOf(p);
        cdl.id = cl.name().empty() ? "clip" + std::to_string(clip.value) : cl.name();
        cdl.description = "Wabcut clip " + std::to_string(clip.value);
        corrections.push_back(std::move(cdl));
    }
    if (const CdlFormat format = cdlFormatOf(file); format == CdlFormat::Cc && corrections.size() > 1) {
        fail(ErrorCode::InvalidParams, "a .cc file holds one correction; use .ccc or .cdl for several clips", Json{{"path", "/path"}});
    }
    if (auto saved = saveCdl(corrections, file); !saved) {
        fail(ErrorCode::EditRejected, "cannot write the CDL: " + saved.error().message);
    }
    CommandOutput out;
    out.state = c.state();
    Json list = Json::array();
    for (const Cdl& cdl : corrections) {
        list.push_back(Json{{"id", cdl.id},
                            {"slope", Json::array({cdl.slope[0], cdl.slope[1], cdl.slope[2]})},
                            {"offset", Json::array({cdl.offset[0], cdl.offset[1], cdl.offset[2]})},
                            {"power", Json::array({cdl.power[0], cdl.power[1], cdl.power[2]})},
                            {"saturation", cdl.saturation}});
    }
    out.data = Json{{"path", pathToUtf8(file)}, {"corrections", std::move(list)}};
    return out;
}

CommandOutput cdlImport(ExecutionContext& c) {
    const std::filesystem::path file = cdlPath(c);
    const auto corrections = loadCdl(file);
    if (!corrections) {
        fail(ErrorCode::EditRejected, "the CDL cannot be read: " + corrections.error().message);
    }
    ProjectState state = c.state();
    CommandOutput out;
    Json results = Json::array();
    const std::vector<ClipId> clips = clipsOf(c);
    for (std::size_t i = 0; i < clips.size(); ++i) {
        const Clip& cl = clipIn(state, c.sequenceId(), clips[i]);
        // By id (the clip's name), else by position, else the first.
        const Cdl* chosen = nullptr;
        if (c.has("index")) {
            const auto index = static_cast<std::size_t>(c.integer("index"));
            if (index >= corrections->size()) {
                fail(ErrorCode::InvalidParams, std::format("the file has {} corrections", corrections->size()), Json{{"path", "/index"}});
            }
            chosen = &(*corrections)[index];
        } else {
            for (const Cdl& cdl : *corrections) {
                if (!cdl.id.empty() && cdl.id == cl.name()) {
                    chosen = &cdl;
                }
            }
            if (!chosen) {
                chosen = &(*corrections)[std::min(i, corrections->size() - 1)];
            }
        }
        auto [next, effect] = ensureGrade(c, std::move(state), clips[i], {});
        state = std::move(next);
        GradeParams p;
        applyCdl(p, *chosen);
        std::vector<EffectParam> params;
        for (const EffectParam& e : gradeParamsToEffect(p, false)) {
            if (e.key.starts_with("wheels.")) {
                params.push_back(e);
            }
        }
        state = setGrade(c, std::move(state), clips[i], effect, clampedParams(params, out.warnings));
        results.push_back(Json{{"clip", clips[i].value}, {"effect", effect.value}, {"correction", chosen->id}});
        out.createdIds.push_back(effect.value);
    }
    out.state = std::move(state);
    out.data = Json{{"clips", std::move(results)}};
    return out;
}

// ---- Scopes --------------------------------------------------------------------------------------------------

CommandOutput scopesRead(ExecutionContext& c) {
    FrameRequest request;
    if (c.has("clip")) {
        const ClipId clip = c.clip("clip");
        const Clip& cl = clipIn(c.state(), c.sequenceId(), clip);
        request.solo = clip;
        request.time = sampleTime(c, cl);
    } else {
        request.time = c.has("time") ? c.time("time") : Ticks{};
    }
    request.maxWidth = 960;
    auto image = servicesOf(c).frame(c.state(), c.sequenceId(), request);
    if (!image) {
        fail(ErrorCode::EditRejected, "cannot render the picture: " + image.error().message);
    }
    const ScopeData data = computeScopes(image->rgba.data(), image->width, image->height, 4);
    const ScopeStats s = scopeStats(data);
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"time", c.timecode(request.time)},
                    {"exposure", s.exposure},
                    {"cast", s.cast},
                    {"luma", Json{{"min", rounded(s.lumaMin, 0.1)},
                                  {"max", rounded(s.lumaMax, 0.1)},
                                  {"mean", rounded(s.lumaMean, 0.1)},
                                  {"p1", rounded(s.p01, 0.1)},
                                  {"p5", rounded(s.p05, 0.1)},
                                  {"median", rounded(s.p50, 0.1)},
                                  {"p95", rounded(s.p95, 0.1)},
                                  {"p99", rounded(s.p99, 0.1)}}},
                    {"units", "IRE (0 = black, 100 = reference white)"},
                    {"clippedHighlights", rounded(s.clippedHigh, 0.0001)},
                    {"crushedBlacks", rounded(s.clippedLow, 0.0001)},
                    {"meanRgb", Json::array({rounded(s.meanRgb[0], 0.001), rounded(s.meanRgb[1], 0.001), rounded(s.meanRgb[2], 0.001)})},
                    {"saturation", rounded(s.saturationMean, 0.1)},
                    {"castHue", rounded(s.castHue, 0.1)},
                    {"castStrength", rounded(s.castStrength, 0.01)},
                    {"pixels", s.pixels}};
    return out;
}

// ---- Colour management (I.9, docs/color.md "Colour management") ----

Json managementJson(const ColorManagement& m) {
    return Json{{"mode", std::string(toString(m.mode))},
                {"name", std::string(displayName(m.mode))},
                {"hdr", m.isHdr()},
                {"managed", m.managed()},
                {"peakNits", m.peakNits},
                {"referenceWhiteNits", m.referenceWhiteNits},
                {"autoDetectLog", m.autoDetectLog}};
}

std::vector<std::string> modeIds() {
    std::vector<std::string> ids;
    for (const ColorManagementMode m : allColorManagementModes()) {
        ids.emplace_back(toString(m));
    }
    return ids;
}

std::vector<std::string> inputSpaceIds() {
    std::vector<std::string> ids{"auto"};
    for (const InputColorSpace s : allInputColorSpaces()) {
        ids.emplace_back(toString(s));
    }
    return ids;
}

CommandOutput managementGet(ExecutionContext& c) {
    CommandOutput out;
    out.state = c.state();
    out.data = managementJson(c.state().colorManagement);
    Json modes = Json::array();
    for (const ColorManagementMode m : allColorManagementModes()) {
        modes.push_back(Json{{"id", std::string(toString(m))},
                             {"name", std::string(displayName(m))},
                             {"description", std::string(description(m))}});
    }
    Json spaces = Json::array();
    for (const InputColorSpace s : allInputColorSpaces()) {
        spaces.push_back(Json{{"id", std::string(toString(s))}, {"name", std::string(displayName(s))}});
    }
    out.data["modes"] = std::move(modes);
    out.data["inputSpaces"] = std::move(spaces);
    return out;
}

CommandOutput managementSet(ExecutionContext& c) {
    ColorManagement m = c.state().colorManagement;
    if (c.has("mode")) {
        const auto mode = colorManagementModeFromString(c.string("mode"));
        if (!mode) {
            fail(ErrorCode::InvalidParams, "unknown colour management \"" + c.string("mode") + "\"", Json{{"path", "/mode"}});
        }
        m.mode = *mode;
    }
    if (c.has("peakNits")) {
        m.peakNits = c.number("peakNits");
    }
    if (c.has("referenceWhiteNits")) {
        m.referenceWhiteNits = c.number("referenceWhiteNits");
    }
    if (c.has("autoDetectLog")) {
        m.autoDetectLog = c.boolean("autoDetectLog", true);
    }
    CommandOutput out;
    out.state = c.take(edit::setColorManagement(c.state(), m));
    out.data = managementJson(m);
    return out;
}

/// A video clip's input colour space: its override, what Auto reads (the stream's tags, or the camera log its metadata
/// names when the project auto-detects log), the detected log, and the one in effect.
Json inputSpaceJson(const ProjectState& state, const Clip& clip) {
    const MediaItem* media = state.findMedia(clip.media);
    const VideoStreamInfo* stream = media && clip.stream.kind == StreamKind::Video && clip.stream.index >= 0 &&
                                            static_cast<std::size_t>(clip.stream.index) < media->info.video.size()
                                        ? &media->info.video[static_cast<std::size_t>(clip.stream.index)]
                                        : nullptr;
    const ColorManagement& m = state.colorManagement;
    const bool detect = m.autoDetectLog && m.mode != ColorManagementMode::Disabled;
    const InputColorSpace automatic = stream ? autoInputColorSpace(*stream, detect) : InputColorSpace::Rec709;
    const std::optional<InputColorSpace> detected = stream ? detectedLogSpace(*stream) : std::nullopt;
    const InputColorSpace effective = clip.inputColorSpace().value_or(automatic);
    return Json{{"clip", clip.id.value},
                {"override", clip.inputColorSpace() ? Json(std::string(toString(*clip.inputColorSpace()))) : Json(nullptr)},
                {"auto", std::string(toString(automatic))},
                {"detected", detected ? Json(std::string(toString(*detected))) : Json(nullptr)},
                {"effective", std::string(toString(effective))},
                {"name", std::string(displayName(effective))}};
}

CommandOutput inputSpaceSet(ExecutionContext& c) {
    const std::vector<ClipId> clips = clipsOf(c);
    const std::string name = c.string("space");
    std::optional<InputColorSpace> space;
    if (name != "auto") {
        space = inputColorSpaceFromString(name);
        if (!space) {
            fail(ErrorCode::InvalidParams, "unknown input colour space \"" + name + "\"", Json{{"path", "/space"}});
        }
    }
    edit::EditResult r = c.take(edit::setClipInputColorSpace(c.state(), c.sequenceId(), clips, space));
    CommandOutput out;
    out.state = std::move(r.state);
    Json list = Json::array();
    for (const ClipId id : clips) {
        list.push_back(inputSpaceJson(out.state, clipIn(out.state, c.sequenceId(), id)));
    }
    out.data = Json{{"clips", std::move(list)}};
    if (out.state.colorManagement.mode == ColorManagementMode::Disabled) {
        out.warnings.push_back("colour management is disabled, which reads every clip as it is: input colour spaces apply "
                               "once a colour setup is chosen (color.management.set)");
    }
    return out;
}

CommandOutput inputSpaceGet(ExecutionContext& c) {
    const std::vector<ClipId> clips = clipsOf(c);
    CommandOutput out;
    out.state = c.state();
    Json list = Json::array();
    for (const ClipId id : clips) {
        const Clip& clip = clipIn(c.state(), c.sequenceId(), id);
        if (clip.stream.kind != StreamKind::Video || clip.isNested()) {
            fail(ErrorCode::InvalidParams, "clip " + std::to_string(id.value) + " is not a video clip of a media item");
        }
        list.push_back(inputSpaceJson(c.state(), clip));
    }
    out.data = Json{{"clips", std::move(list)}, {"management", managementJson(c.state().colorManagement)}};
    return out;
}

} // namespace

const EffectInstance* gradeOf(const Clip& clip, EffectInstanceId preferred) {
    for (const EffectInstance& e : clip.effects()) {
        if (e.effect == kGradeEffect && (!preferred.isValid() || e.id == preferred)) {
            return &e;
        }
    }
    return nullptr;
}

// Nested clips have no media item, so comparing Clip::media alone would make every nested clip one source.
bool sameSource(const Clip& a, const Clip& b) {
    if (a.isNested() || b.isNested()) {
        return a.isNested() && b.isNested() && a.sequence == b.sequence && a.angles == b.angles;
    }
    return a.media.isValid() && a.media == b.media;
}

std::vector<ClipId> sceneClips(const Sequence& sequence, ClipId reference) {
    const auto loc = sequence.findClip(reference);
    if (!loc) {
        return {};
    }
    const Track& track = *loc->track;
    std::size_t index = 0;
    for (; index < track.clips.size() && track.clips[index].id != reference; ++index) {
    }
    const Clip& ref = track.clips[index];
    std::size_t first = index, last = index;
    while (first > 0 && sameSource(track.clips[first - 1], ref) && track.clips[first - 1].end() == track.clips[first].start()) {
        --first;
    }
    while (last + 1 < track.clips.size() && sameSource(track.clips[last + 1], ref) &&
           track.clips[last].end() == track.clips[last + 1].start()) {
        ++last;
    }
    std::vector<ClipId> out;
    for (std::size_t i = first; i <= last; ++i) {
        out.push_back(track.clips[i].id);
    }
    return out;
}

void registerColorCommands(command::CommandRegistry& registry) {
    using command::Capability;
    const Json clipRefs = clipsParam("Clips to change (or \"clip\" for one).");
    add(registry,
        {"color.grade.set", 1, "Set Color Grade",
         "Sets a clip's colour grade (wabcut.color.grade; added at the end of the stack when the clip has none): "
         "\"params\" by registry key (only those given change; effect.list lists them with ranges), \"stages\" on or off "
         "(basic, creative, curves, wheels, hsl, vignette), \"curves\" as [x, y] points (master/red/green/blue x and y in "
         "0..1; hueSat/hueHue/hueLuma x = hue in turns and lumaSat/satSat y with 0.5 = no change; [] resets), and the "
         "\"inputLut\" / \"look\" (\"builtin:<name>\" or \"asset:<id>\" from lut.list; \"\" removes). A constant replaces an "
         "animated parameter. One undo step for every clip.",
         Capability::EditSequence,
         withSequence({{"clip", clipParam("The clip.")},
                       {"clips", clipRefs},
                       {"effect", gradeEffectParam()},
                       {"params", numberMap("Parameter key -> number, e.g. {\"basic.exposure\": 0.5, \"basic.saturation\": 120}.")},
                       {"stages", numberMap("Stage -> true/false, e.g. {\"hsl\": false}.")},
                       {"curves", numberMap("Curve name -> [[x, y], ...].")},
                       {"inputLut", schema::string("Input LUT reference (Basic), or \"\" for none.")},
                       {"look", schema::string("Look LUT reference (Creative), or \"\" for none.")},
                       {"enabled", schema::boolean("Turn the whole grade on or off.")}}),
         Json{{"clip", Json{{"track", "V1"}, {"time", "00:00:05:00"}}}, {"params", Json{{"basic.exposure", 0.4}, {"basic.contrast", 15}}}},
         {}, gradeSet});
    add(registry,
        {"color.grade.get", 1, "Get Color Grade",
         "A clip's grade at a time (default: its middle): parameters (only those changed unless \"all\"), animated ones, "
         "curves, LUTs, missing LUTs and whether it does anything. \"grade\": null when the clip has none.",
         Capability::Read,
         withSequence({{"clip", clipParam("The clip.")},
                       {"effect", gradeEffectParam()},
                       {"time", timeParam("When (inside the clip).")},
                       {"all", schema::boolean("List every parameter, not only those changed. Default false.")}},
                      {"clip"}),
         Json{{"clip", 42}}, {}, gradeGet});
    add(registry,
        {"color.grade.copy", 1, "Apply Grade",
         "Copies a clip's whole colour grade (every parameter with its keyframes, curves, LUTs, stage switches and the on / "
         "off state) over other clips' grades: \"clips\", or with \"scope\": \"media\" every other video clip that shows "
         "the same source (the same media item; for nested clips the same sequence, and for multicam the same angle). Each "
         "target's first grade is replaced in place (its id, stack position and its own masks stay); a target without a "
         "grade gets one at the end of its stack. \"replace\": false appends a second grade instead, as Paste Attributes "
         "does. Audio clips, clips on locked tracks and the source itself are skipped (listed with the reason). One undo "
         "step; refused when the source has no grade or no target changes.",
         Capability::EditSequence,
         withSequence({{"from", clipParam("The clip whose grade is copied.")},
                       {"clips", clipsParam("The clips to apply the grade to (scope \"clips\").")},
                       {"scope", schema::enumeration({"clips", "media"}, "Which clips: \"clips\" (default) or every video clip "
                                                                         "of the source's media (\"media\").")},
                       {"replace", schema::boolean("Replace each target's grade (default true); false appends another grade.")}},
                      {"from"}),
         Json{{"from", 42}, {"scope", "media"}}, {}, gradeCopy});
    add(registry,
        {"color.auto", 1, "Auto Color",
         "Auto colour: looks at the clip (at \"time\", default its middle) without its grade and sets the grade's white "
         "balance (temperature, tint) and levels (exposure, whites, blacks) as ordinary, editable Basic parameters. One undo "
         "step.",
         Capability::EditSequence,
         withSequence({{"clip", clipParam("The clip.")}, {"clips", clipRefs}, {"effect", gradeEffectParam()},
                       {"time", timeParam("The frame to analyse (inside the clip).")}}),
         Json{{"clips", Json::array({42, 43})}}, {}, autoCommand});
    add(registry,
        {"color.match", 1, "Color Match",
         "Makes clips look like a reference frame: \"reference\" is {\"clip\", \"time\"} (that clip as it looks, alone) or a "
         "sequence time (the whole picture). Fits the grade's exposure, contrast, temperature, tint and saturation so the "
         "clip's colour statistics (Oklab mean and spread) move to the reference's, by \"strength\"; \"skinPriority\" weighs "
         "skin tones more, \"faceDetection\" (Premiere's Face Detection) the faces' skin. Editable parameters, one undo step.",
         Capability::EditSequence,
         withSequence({{"clip", clipParam("The clip to change.")},
                       {"clips", clipRefs},
                       {"effect", gradeEffectParam()},
                       {"reference", Json{{"type", Json::array({"object", "integer", "string"})},
                                          {"description", "{\"clip\": ..., \"time\": ...} or a sequence time."}}},
                       {"strength", strengthParam()},
                       {"skinPriority", schema::boolean("Weigh skin tones 4x. Default false.")},
                       {"faceDetection", schema::boolean("Face Detection: find the faces on both pictures and match their skin "
                                                         "tones first (needs the face detector). Default false.")}},
                      {"reference"}),
         Json{{"clips", Json::array({44, 45})}, {"reference", Json{{"clip", 43}}}}, {}, matchCommand});
    add(registry,
        {"color.matchScene", 1, "Match Scene",
         "Shot-to-shot consistency: matches every clip of the reference clip's scene to it - \"scene\" (default) is the run "
         "of clips on its track cut from the same take without gaps (what clip.splitAtShots makes), \"track\" every clip on "
         "its track, \"media\" every clip of the same media item, or an explicit \"clips\" list. One undo step; dryRun "
         "previews it.",
         Capability::EditSequence,
         withSequence({{"reference", clipParam("The clip the others should look like.")},
                       {"scope", schema::enumeration({"scene", "track", "media"}, "Which clips. Default scene.")},
                       {"clips", clipsParam("Or exactly these clips.")},
                       {"strength", strengthParam()},
                       {"skinPriority", schema::boolean("Weigh skin tones 4x. Default false.")},
                       {"faceDetection", schema::boolean("Face Detection: find the faces on both pictures and match their skin "
                                                         "tones first (needs the face detector). Default false.")}},
                      {"reference"}),
         Json{{"reference", Json{{"track", "V1"}, {"time", "00:00:12:00"}}}}, {}, matchSceneCommand});
    add(registry,
        command::touchesFiles({"lut.import", 1, "Import LUT",
         "Adds a .cube LUT file (1D or 3D, up to 65 points per side) to the project as an asset: it relinks and is "
         "collected like media. Returns its reference for color.grade.set's inputLut / look. Importing the same file again "
         "returns the existing asset.",
         Capability::EditProject,
         Json{{"type", "object"},
              {"properties", Json{{"path", schema::string("The .cube file.")}, {"name", schema::string("Display name. Default the file name.")}}},
              {"required", Json::array({"path"})},
              {"additionalProperties", false}},
         Json{{"path", "C:/LUTs/FilmLook.cube"}}, {}, lutImport}, {"path"}));
    add(registry,
        command::touchesFiles({"lut.export", 1, "Export .cube",
         "Writes a clip's colour grade as a 3D LUT .cube file (Premiere's Lumetri panel > Export .cube): \"size\" points per "
         "side (default 33, as Premiere), display-referred Rec.709 in and out, evaluated at \"time\" (default the clip's "
         "middle). Every pixel-local part of the grade goes in - input LUT, Basic, Creative, Curves, Color Wheels, the HSL "
         "secondary through its qualifier; the vignette, the secondary's denoise / blur / sharpen and masks cannot and are "
         "listed in \"leftOut\". Outputs are clamped to 0..1. Refuses to replace a file unless \"overwrite\"; a dry run writes "
         "nothing.",
         Capability::Read,
         withSequence({{"clip", clipParam("The clip.")},
                       {"effect", gradeEffectParam()},
                       {"time", timeParam("When (inside the clip). Default its middle.")},
                       {"path", schema::string("The .cube file to write.")},
                       {"size", schema::integer("Lattice points per side, 2..65. Default 33.", 2, kMaxLut3dSize)},
                       {"title", schema::string("The TITLE line. Default the clip's name.")},
                       {"overwrite", schema::boolean("Replace an existing file. Default false.")}},
                      {"clip", "path"}),
         Json{{"clip", 42}, {"path", "C:/grades/shot42.cube"}}, {}, lutExport}, {"path"}));
    add(registry,
        {"lut.list", 1, "List LUTs",
         "The built-in looks and camera input LUTs (with where their math comes from) and the project's LUT assets (path, "
         "offline, how many effects use each).",
         Capability::Read, Json{{"type", "object"}, {"properties", Json::object()}, {"additionalProperties", false}}, Json::object(),
         {}, lutList});
    add(registry,
        command::touchesFiles({"cdl.export", 1, "Export CDL",
         "Writes clips' colour wheels as an ASC CDL: slope = gain - lift, offset = lift + offset, power = gamma, saturation. "
         ".cc holds one clip; .ccc and .cdl several (ids are the clip names). Refuses to replace a file unless \"overwrite\".",
         Capability::EditProject,
         withSequence({{"clip", clipParam("The clip.")},
                       {"clips", clipRefs},
                       {"effect", gradeEffectParam()},
                       {"path", schema::string("The .cc, .ccc or .cdl file to write.")},
                       {"overwrite", schema::boolean("Replace an existing file. Default false.")}},
                      {"path"}),
         Json{{"clip", 42}, {"path", "C:/grades/shot42.cc"}}, {}, cdlExport}, {"path"}));
    add(registry,
        command::touchesFiles({"cdl.import", 1, "Import CDL",
         "Sets clips' colour wheels from an ASC CDL file (.cc, .ccc, .cdl): the correction whose id is the clip's name, else "
         "the one at the clip's position, or \"index\". Values beyond the wheels' ranges are clamped (with a warning).",
         Capability::EditSequence,
         withSequence({{"clip", clipParam("The clip.")},
                       {"clips", clipRefs},
                       {"path", schema::string("The CDL file.")},
                       {"index", schema::integer("Which correction in the file (0 = first).", 0)}},
                      {"path"}),
         Json{{"clip", 42}, {"path", "C:/grades/shot42.cc"}}, {}, cdlImport}, {"path"}));
    add(registry,
        {"scopes.read", 1, "Read Scopes",
         "Scope statistics of the picture at a time (or of one clip alone): luma percentiles in IRE, clipped highlights and "
         "crushed blacks, mean colour, saturation and colour cast, with a verdict (\"underexposed\", \"overexposed\", \"low "
         "contrast\", \"clipped highlights\", \"crushed blacks\" or \"ok\"). Read only.",
         Capability::Read,
         withSequence({{"time", timeParam("Sequence time. Default 0.")}, {"clip", clipParam("Only this clip, at \"time\" or its middle.")}}),
         Json{{"time", "00:00:10:00"}}, {}, scopesRead});
    add(registry,
        {"color.management.get", 1, "Get Colour Management",
         "The project's colour management, Premiere Pro's sequence Color Setup: \"mode\" (rec709 = Direct Rec.709 (SDR), "
         "the default; hdr-hlg = Direct HLG (HDR); hdr-pq = Direct PQ (HDR); aces = Wide Gamut (Tone Mapped), ACES 1.3 "
         "with the grade in ACEScct; wide-gamut = Wide Gamut (No Tone Mapping); off = Disable Color Management), the HDR "
         "peak and graphics (reference) white in nits, whether log media is auto-detected, and the modes and input colour "
         "spaces there are (ids, names and descriptions, for color.management.set and color.inputSpace.set).",
         Capability::Read, Json{{"type", "object"}, {"properties", Json::object()}, {"additionalProperties", false}}, Json::object(),
         {}, managementGet});
    add(registry,
        {"color.management.set", 1, "Set Colour Management",
         "Sets the project's colour management (Premiere Pro's sequence Color Setup): \"mode\" rec709 (Direct Rec.709 "
         "(SDR), the default: SDR media as it is, HDR media and log clips tone-mapped into SDR on input), hdr-hlg or hdr-pq "
         "(Direct HLG / PQ (HDR): a linear Rec.2020 working space, SDR clips and titles at the graphics white; the monitor "
         "shows a tone-mapped SDR preview, HDR exports carry HLG or PQ), aces (Wide Gamut (Tone Mapped): ACES 1.3, clips "
         "read through their input colour space into ACEScg, the grade works in ACEScct, the ACES output transforms), "
         "wide-gamut (Wide Gamut (No Tone Mapping): ACEScg, clipped at the output's white) or off (Disable Color "
         "Management: no conversion at all). \"peakNits\" (100..10000, default 1000) is the HDR mastering peak, "
         "\"referenceWhiteNits\" (80..400, default 203) the graphics white where SDR white lands, \"autoDetectLog\" "
         "(default true) reads clips as the camera log their metadata names. Only the given fields change. One undo step.",
         Capability::EditProject,
         Json{{"type", "object"},
              {"properties", Json{{"mode", schema::enumeration(modeIds(), "The colour setup.")},
                                  {"peakNits", schema::number("HDR mastering peak in nits, 100..10000.", 100.0, 10000.0)},
                                  {"referenceWhiteNits", schema::number("HDR graphics (reference) white in nits, 80..400.", 80.0, 400.0)},
                                  {"autoDetectLog", schema::boolean("Read clips as the camera log their metadata names.")}}},
              {"additionalProperties", false}},
         Json{{"mode", "hdr-pq"}, {"peakNits", 1000}}, {}, managementSet});
    add(registry,
        {"color.inputSpace.set", 1, "Set Input Colour Space",
         "Sets how video clips' pictures are read into the working space (Premiere Pro's Override Media Color Space): "
         "\"auto\" follows the stream's colour tags (BT.709, BT.2020, PQ, HLG, sRGB, linear) and the camera log the "
         "media's metadata names, or a space by id - camera log formats are chosen here when the metadata does not say "
         "(s-log3, s-log3-cine, v-log, logc3, logc4, canon-log3, canon-log2, red-log3g10, apple-log, acescct; "
         "color.management.get lists them all). Direct Rec.709 tone-maps log and HDR clips into SDR. One undo step for "
         "every clip.",
         Capability::EditSequence,
         withSequence({{"clip", clipParam("The clip.")}, {"clips", clipRefs},
                       {"space", schema::enumeration(inputSpaceIds(), "The input colour space, or \"auto\".")}},
                      {"space"}),
         Json{{"clips", Json::array({42, 43})}, {"space", "s-log3-cine"}}, {}, inputSpaceSet});
    add(registry,
        {"color.inputSpace.get", 1, "Get Input Colour Space",
         "Video clips' input colour spaces: the override (null = auto), what auto reads (the stream's tags or the detected "
         "camera log), the log the media's metadata names (null = none), and the one in effect, with the project's colour "
         "management. Read only.",
         Capability::Read,
         withSequence({{"clip", clipParam("The clip.")}, {"clips", clipRefs}}),
         Json{{"clip", 42}}, {}, inputSpaceGet});
}

} // namespace wabcut::color
