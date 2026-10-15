#include <wabcut/color/ColorCommands.h>

#include <wabcut/color/Cdl.h>
#include <wabcut/color/Looks.h>
#include <wabcut/color/Scopes.h>
#include <wabcut/command/Schema.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/MediaLocator.h>
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

/// Whether two clips show the same source (0.1.2): the same media item, or the same nested sequence - for a multicam
/// clip also the same video angle, since each angle is another camera. Nested clips have no media item, so comparing
/// Clip::media alone would make every nested clip one source.
bool sameSource(const Clip& a, const Clip& b) {
    if (a.isNested() || b.isNested()) {
        return a.isNested() && b.isNested() && a.sequence == b.sequence && a.angles == b.angles;
    }
    return a.media.isValid() && a.media == b.media;
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

std::vector<ColorSample> samplesOf(ExecutionContext& c, const ProjectState& state, const FrameRequest& request, const char* what) {
    auto image = servicesOf(c).frame(state, c.sequenceId(), request);
    if (!image) {
        fail(image.error().code == wabcut::ErrorCode::Cancelled ? ErrorCode::Cancelled : ErrorCode::EditRejected,
             std::string("cannot render ") + what + ": " + image.error().message);
    }
    auto samples = samplePixels(*image, 4096);
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
};

/// The reference frame: {"clip", "time"} (that clip as it looks, alone) or a sequence time (the whole picture).
Reference referenceOf(ExecutionContext& c, const ProjectState& state, const Json& ref) {
    Reference r;
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
        r.samples = samplesOf(c, state, FrameRequest{t, clip, {}}, "the reference clip");
        r.what = "clip " + std::to_string(clip.value) + " at " + c.timecode(t);
        return r;
    }
    const Ticks t = ref.is_object() && ref.contains("time") ? c.timeValue(ref["time"], "/reference/time") : c.timeValue(ref, "/reference");
    r.samples = samplesOf(c, state, FrameRequest{t, {}, {}}, "the reference frame");
    r.what = "the sequence at " + c.timecode(t);
    return r;
}

Json matchOne(ExecutionContext& c, ProjectState& state, ClipId clip, EffectInstanceId preferred, const Reference& reference,
              const MatchOptions& options, std::vector<std::string>& warnings) {
    auto [next, effect] = ensureGrade(c, std::move(state), clip, preferred);
    state = std::move(next);
    const Clip& cl = clipIn(state, c.sequenceId(), clip);
    const Ticks t = sampleTime(c, cl, "__none");
    c.checkpoint("matching clip " + std::to_string(clip.value));
    const auto samples = samplesOf(c, state, FrameRequest{t, clip, effect}, "the clip");
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
    return Json{{"clip", clip.value},
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
    const Reference reference = referenceOf(c, state, c.params()["reference"]);
    const MatchOptions options = matchOptions(c);
    const EffectInstanceId preferred{static_cast<std::uint64_t>(c.integer("effect", 0))};
    CommandOutput out;
    Json results = Json::array();
    for (const ClipId clip : clipsOf(c)) {
        results.push_back(matchOne(c, state, clip, preferred, reference, options, out.warnings));
        out.createdIds.push_back(results.back()["effect"].get<std::uint64_t>());
    }
    out.state = std::move(state);
    out.data = Json{{"reference", reference.what}, {"clips", std::move(results)}};
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
    const Reference reference = referenceOf(c, state, ref);
    const MatchOptions options = matchOptions(c);
    CommandOutput out;
    Json results = Json::array();
    for (const ClipId clip : clips) {
        results.push_back(matchOne(c, state, clip, {}, reference, options, out.warnings));
    }
    out.state = std::move(state);
    out.data = Json{{"reference", reference.what}, {"scope", c.has("clips") ? "clips" : scope}, {"clips", std::move(results)}};
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

} // namespace

const EffectInstance* gradeOf(const Clip& clip, EffectInstanceId preferred) {
    for (const EffectInstance& e : clip.effects()) {
        if (e.effect == kGradeEffect && (!preferred.isValid() || e.id == preferred)) {
            return &e;
        }
    }
    return nullptr;
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
         "skin tones more. Editable parameters, one undo step.",
         Capability::EditSequence,
         withSequence({{"clip", clipParam("The clip to change.")},
                       {"clips", clipRefs},
                       {"effect", gradeEffectParam()},
                       {"reference", Json{{"type", Json::array({"object", "integer", "string"})},
                                          {"description", "{\"clip\": ..., \"time\": ...} or a sequence time."}}},
                       {"strength", strengthParam()},
                       {"skinPriority", schema::boolean("Weigh skin tones 4x. Default false.")}},
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
                       {"skinPriority", schema::boolean("Weigh skin tones 4x. Default false.")}},
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
}

} // namespace wabcut::color
