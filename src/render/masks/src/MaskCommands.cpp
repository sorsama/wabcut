#include <wabcut/masks/MaskCommands.h>

#include <wabcut/command/Schema.h>
#include <wabcut/command/SourceServices.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/FrameScaling.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/RedactionMedia.h>
#include <wabcut/model/Timecode.h>
#include <wabcut/model/TimeRemap.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <optional>
#include <span>

namespace wabcut::masks {

using command::CommandOutput;
using command::ErrorCode;
using command::ExecutionContext;
using command::fail;
using command::Json;
namespace schema = command::schema;

void TrackingServices::progress(const TrackProgress&) {}

std::expected<std::vector<DetectedRegion>, Error> TrackingServices::detect(const GrayImage&, std::string_view kind) {
    return std::unexpected(Error{wabcut::ErrorCode::Unsupported,
                                 "no " + std::string(kind) + " detector is installed (the AI-9 detector plugs in here)"});
}

// ---- Shared helpers ----------------------------------------------------------------------------------------------

MaskTransformValues maskTransformValues(const MaskAffine& m, double anchorX, double anchorY, double aspect,
                                        double previousRotation, bool flippedX) {
    aspect = aspect > 0.0 && std::isfinite(aspect) ? aspect : 1.0;
    // Back to aspect space: L = R(rot) * Shear(skew) * S(sx, sy) (Masks.cpp maskTransformAt), a QR split.
    const double m00 = m.a;
    const double m01 = m.b * aspect;
    const double m10 = m.d / aspect;
    const double m11 = m.e;
    const double ox = m.c * aspect;
    const double oy = m.f;
    MaskTransformValues v;
    double rot = std::atan2(m10, m00);
    v.scaleX = std::hypot(m00, m10);
    if (flippedX) {
        // The same matrix with a negative scaleX: turned half a turn further, both scales negated (R(pi) = -I). Keys of
        // a flipped mask stay on this branch, so they interpolate instead of collapsing through zero (wave-4 P2-36).
        rot += std::numbers::pi;
        v.scaleX = -v.scaleX;
    }
    const double cs = std::cos(rot);
    const double sn = std::sin(rot);
    const double u01 = cs * m01 + sn * m11;
    const double u11 = -sn * m01 + cs * m11;
    v.scaleY = u11;
    v.skew = std::abs(u11) > 1e-12 ? std::atan(u01 / u11) * 180.0 / std::numbers::pi : 0.0;
    double degrees = rot * 180.0 / std::numbers::pi;
    degrees += 360.0 * std::round((previousRotation - degrees) / 360.0);
    v.rotation = degrees;
    const double ax = anchorX * aspect;
    const double ay = anchorY;
    const double tx = ox - ax + (m00 * ax + m01 * ay);
    const double ty = oy - ay + (m10 * ax + m11 * ay);
    v.x = tx / aspect;
    v.y = ty;
    return v;
}

std::vector<Point2> maskPolygonAt(const Mask& mask, Ticks curveTime, double aspect) {
    constexpr int kSteps = 12;
    const std::vector<MaskVertex> outline = maskOutlineAt(mask, curveTime, aspect);
    std::vector<Point2> out;
    for (std::size_t i = 0; i < outline.size(); ++i) {
        const MaskVertex& a = outline[i];
        const MaskVertex& b = outline[(i + 1) % outline.size()];
        out.push_back({a.x, a.y});
        if (a.outX == 0.0 && a.outY == 0.0 && b.inX == 0.0 && b.inY == 0.0) {
            continue;
        }
        for (int k = 1; k < kSteps; ++k) {
            const double t = static_cast<double>(k) / kSteps;
            const double s = 1.0 - t;
            const double w0 = s * s * s, w1 = 3 * s * s * t, w2 = 3 * s * t * t, w3 = t * t * t;
            out.push_back({w0 * a.x + w1 * (a.x + a.outX) + w2 * (b.x + b.inX) + w3 * b.x,
                           w0 * a.y + w1 * (a.y + a.outY) + w2 * (b.y + b.inY) + w3 * b.y});
        }
    }
    return out;
}

namespace {

// ---- Schemas -------------------------------------------------------------------------------------------------

/// Registers `spec`. Its params are free-form JSON in places (region, words, color, detect): a value of the wrong type
/// makes nlohmann's get<>() throw, which the engine would report as an internal error. It is the caller's mistake, so
/// it becomes InvalidParams (Codex wave-4 P2-39).
void add(command::CommandRegistry& registry, command::CommandSpec spec) {
    spec.execute = [inner = std::move(spec.execute)](ExecutionContext& c) -> CommandOutput {
        try {
            return inner(c);
        } catch (const Json::exception& e) {
            fail(ErrorCode::InvalidParams, std::string("a parameter has the wrong type or shape: ") + e.what());
        }
    };
    [[maybe_unused]] const auto added = registry.add(std::move(spec));
}

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

Json freeObject(std::string description) { return Json{{"type", "object"}, {"description", std::move(description)}}; }
Json freeArray(std::string description) { return Json{{"type", "array"}, {"description", std::move(description)}}; }

Json effectParam() {
    return schema::integer("The effect instance id (clip.details lists them). mask.add: omit for the clip's opacity mask "
                           "(cut-outs); the others: default the effect holding the mask.",
                           1);
}

Json trackingParams(std::initializer_list<std::pair<const char*, Json>> extra, std::vector<std::string> required) {
    Json s = withSequence(extra, std::move(required));
    auto& p = s["properties"];
    p["time"] = timeParam("Where to start (inside the clip). Default the clip's first frame (backward: its last).");
    p["direction"] = schema::enumeration({"forward", "backward"}, "Default forward.");
    p["frames"] = schema::integer("Track at most this many frames (1 = one step). Default to the clip's end.", 1);
    p["model"] = schema::enumeration({"translation", "similarity", "affine"},
                                     "What the region may do: move; move, turn and scale; or any affine. Default similarity.");
    p["workingWidth"] = schema::integer("Width of the pictures the tracker looks at. Default 640.", 64, 4096);
    p["minConfidence"] = schema::number("Stop where the confidence falls below this. Default 0.3.", 0.0, 1.0);
    return s;
}

// ---- Lookups -------------------------------------------------------------------------------------------------

const Clip& clipOf(ExecutionContext& c, const ProjectState& state, ClipId id) {
    const Sequence* seq = state.findSequence(c.sequenceId());
    const auto loc = seq ? seq->findClip(id) : std::nullopt;
    if (!loc) {
        fail(ErrorCode::NotFound, "clip " + std::to_string(id.value) + " does not exist");
    }
    return loc->clip();
}

const VideoStreamInfo& videoOf(const ProjectState& state, const Clip& clip) {
    const MediaItem* item = state.findMedia(clip.media);
    if (clip.stream.kind != StreamKind::Video || !item || clip.stream.index < 0 ||
        clip.stream.index >= static_cast<int>(item->info.video.size())) {
        fail(ErrorCode::EditRejected, "clip " + std::to_string(clip.id.value) + " is not a video clip of a media item");
    }
    return item->info.video[static_cast<std::size_t>(clip.stream.index)];
}

double displayAspect(const VideoStreamInfo& v) {
    const double par = v.sampleAspect.isValid() ? v.sampleAspect.toDouble() : 1.0;
    return v.height > 0 ? v.width * par / v.height : 16.0 / 9.0;
}

/// The effect instance holding mask `id`: `effect` when given, else the one effect of the clip that has a mask with that
/// id. Mask ids are per effect, so several effects can hold a mask `id`: then the caller must name the effect
/// (InvalidParams) rather than have the first match edited (Codex wave-4 P2-30).
const EffectInstance& effectWithMask(const Clip& clip, EffectInstanceId effect, std::uint32_t id) {
    const EffectInstance* found = nullptr;
    std::string holders;
    for (const EffectInstance& e : clip.effects()) {
        if ((effect.isValid() ? e.id == effect : true) &&
            std::any_of(e.masks.begin(), e.masks.end(), [&](const Mask& m) { return m.id == id; })) {
            holders += (found ? ", " : "") + std::to_string(e.id.value);
            found = found ? found : &e;
            if (!effect.isValid() && found != &e) {
                fail(ErrorCode::InvalidParams,
                     "clip " + std::to_string(clip.id.value) + " has a mask " + std::to_string(id) + " on effects " + holders +
                         ": name the \"effect\"",
                     Json{{"path", "/effect"}});
            }
        }
    }
    if (!found) {
        fail(ErrorCode::NotFound, "clip " + std::to_string(clip.id.value) + " has no mask " + std::to_string(id) +
                                      (effect.isValid() ? " on effect " + std::to_string(effect.value) : ""));
    }
    return *found;
}

/// The effect mask.add put a mask in: `effect`, or without it the clip's opacity-mask effect ("wabcut.mask").
const EffectInstance& effectAddedTo(const Clip& clip, EffectInstanceId effect) {
    for (const EffectInstance& e : clip.effects()) {
        if (effect.isValid() ? e.id == effect : e.effect == "wabcut.mask") {
            return e;
        }
    }
    fail(ErrorCode::Internal, "the mask was added to no effect of clip " + std::to_string(clip.id.value));
}

const Mask& maskIn(const EffectInstance& e, std::uint32_t id) {
    return *std::find_if(e.masks.begin(), e.masks.end(), [&](const Mask& m) { return m.id == id; });
}

std::uint32_t maskIdParam(ExecutionContext& c) {
    const std::int64_t id = c.integer("mask", 0);
    if (id <= 0 || id > 0xFFFFFFFFll) {
        fail(ErrorCode::InvalidParams, "\"mask\" must be a mask id", Json{{"path", "/mask"}});
    }
    return static_cast<std::uint32_t>(id);
}

EffectInstanceId effectParamOf(ExecutionContext& c) { return EffectInstanceId{static_cast<std::uint64_t>(c.integer("effect", 0))}; }

// ---- Mask JSON ---------------------------------------------------------------------------------------------------

MaskVertex vertexOf(const Json& v, const std::string& path) {
    MaskVertex out;
    if (v.is_array() && v.size() == 2 && v[0].is_number() && v[1].is_number()) {
        out.x = v[0].get<double>();
        out.y = v[1].get<double>();
        return out;
    }
    if (!v.is_object() || !v.contains("x") || !v.contains("y")) {
        fail(ErrorCode::InvalidParams, "a vertex is [x, y] or {\"x\", \"y\", \"inX\", \"inY\", \"outX\", \"outY\"}", Json{{"path", path}});
    }
    const auto num = [&](const char* key) {
        if (!v.contains(key)) {
            return 0.0;
        }
        if (!v[key].is_number()) {
            fail(ErrorCode::InvalidParams, std::string(key) + " must be a number", Json{{"path", path + "/" + key}});
        }
        return v[key].get<double>();
    };
    out.x = num("x"), out.y = num("y"), out.inX = num("inX"), out.inY = num("inY"), out.outX = num("outX"), out.outY = num("outY");
    return out;
}

std::vector<MaskVertex> verticesOf(const Json& list, const std::string& path) {
    if (!list.is_array()) {
        fail(ErrorCode::InvalidParams, "vertices must be an array", Json{{"path", path}});
    }
    std::vector<MaskVertex> out;
    for (std::size_t i = 0; i < list.size(); ++i) {
        out.push_back(vertexOf(list[i], path + "/" + std::to_string(i)));
    }
    return out;
}

Json vertexJson(const MaskVertex& v) {
    return Json{{"x", v.x}, {"y", v.y}, {"inX", v.inX}, {"inY", v.inY}, {"outX", v.outX}, {"outY", v.outY}};
}

/// A parameter value: a number, or {"keys": [{"time": sequence time, "value", "interpolation"}]} (times through the
/// clip's mapping: curve time = media time).
ParamValue paramValueOf(ExecutionContext& c, const Clip& clip, const Json& v, const std::string& path) {
    if (v.is_number()) {
        return ParamValue(v.get<double>());
    }
    if (!v.is_object() || !v.contains("keys") || !v["keys"].is_array() || v["keys"].empty()) {
        fail(ErrorCode::InvalidParams, "a parameter is a number or {\"keys\": [{\"time\", \"value\"}, ...]}", Json{{"path", path}});
    }
    Curve curve;
    for (std::size_t i = 0; i < v["keys"].size(); ++i) {
        const Json& k = v["keys"][i];
        const std::string kp = path + "/keys/" + std::to_string(i);
        if (!k.is_object() || !k.contains("time") || !k.contains("value") || !k["value"].is_number()) {
            fail(ErrorCode::InvalidParams, "a key is {\"time\", \"value\"}", Json{{"path", kp}});
        }
        Keyframe key;
        key.time = clip.curveTimeAt(c.timeValue(k["time"], kp + "/time"));
        key.value = k["value"].get<double>();
        if (k.contains("interpolation")) {
            const auto interp = k["interpolation"].is_string() ? interpolationFromString(k["interpolation"].get<std::string>()) : std::nullopt;
            if (!interp) {
                fail(ErrorCode::InvalidParams, "unknown interpolation", Json{{"path", kp + "/interpolation"}});
            }
            key.interpolation = *interp;
        }
        const auto at = curve.insertionIndex(key.time);
        if (curve.keyAt(key.time)) {
            fail(ErrorCode::InvalidParams, "two keys at the same time", Json{{"path", kp}});
        }
        curve.keys.insert(curve.keys.begin() + static_cast<std::ptrdiff_t>(at), key);
    }
    return ParamValue(std::move(curve));
}

Json paramJson(ExecutionContext& c, const ParamValue& v) {
    if (const Curve* curve = v.curve()) {
        Json keys = Json::array();
        for (const Keyframe& k : curve->keys) {
            keys.push_back(Json{{"mediaTime", k.time.value}, {"value", k.value}, {"interpolation", std::string(toString(k.interpolation))}});
        }
        (void)c;
        return Json{{"keys", std::move(keys)}};
    }
    return v.constant();
}

Json maskJson(ExecutionContext& c, const Mask& m) {
    Json params = Json::object();
    for (const MaskParam& p : m.params) {
        params[p.key] = paramJson(c, p.value);
    }
    Json path = Json::array();
    for (const MaskPathKey& k : m.path) {
        Json vs = Json::array();
        for (const MaskVertex& v : k.vertices) {
            vs.push_back(vertexJson(v));
        }
        path.push_back(Json{{"mediaTime", k.time.value}, {"vertices", std::move(vs)}, {"interpolation", std::string(toString(k.interpolation))}});
    }
    return Json{{"id", m.id},
                {"name", m.name},
                {"shape", std::string(toString(m.shape))},
                {"mode", std::string(toString(m.mode))},
                {"inverted", m.inverted},
                {"enabled", m.enabled},
                {"params", std::move(params)},
                {"path", std::move(path)}};
}

/// Applies the mask fields of `p` (name, shape, mode, inverted, enabled, params, path / pathKeys) to `m`.
void applyMaskFields(ExecutionContext& c, const Clip& clip, const Json& p, Mask& m, const std::string& base = "") {
    if (p.contains("name")) {
        m.name = p["name"].get<std::string>();
    }
    if (p.contains("shape")) {
        const auto s = maskShapeFromString(p["shape"].get<std::string>());
        if (!s) {
            fail(ErrorCode::InvalidParams, "shape is ellipse, rectangle or path", Json{{"path", base + "/shape"}});
        }
        m.shape = *s;
    }
    if (p.contains("mode")) {
        const auto mode = maskModeFromString(p["mode"].get<std::string>());
        if (!mode) {
            fail(ErrorCode::InvalidParams, "mode is add, subtract or intersect", Json{{"path", base + "/mode"}});
        }
        m.mode = *mode;
    }
    if (p.contains("inverted")) {
        m.inverted = p["inverted"].get<bool>();
    }
    if (p.contains("enabled")) {
        m.enabled = p["enabled"].get<bool>();
    }
    if (p.contains("params")) {
        for (const auto& [key, value] : p["params"].items()) {
            if (!maskParam(key)) {
                fail(ErrorCode::InvalidParams, "unknown mask parameter \"" + key + "\"", Json{{"path", base + "/params/" + key}});
            }
            m.set(key, paramValueOf(c, clip, value, base + "/params/" + key));
        }
    }
    if (p.contains("path")) {
        m.path = {MaskPathKey{Ticks{}, verticesOf(p["path"], base + "/path"), Interpolation::Linear}};
    }
    if (p.contains("pathKeys")) {
        const Json& keys = p["pathKeys"];
        if (!keys.is_array() || keys.empty()) {
            fail(ErrorCode::InvalidParams, "pathKeys is a non-empty array", Json{{"path", base + "/pathKeys"}});
        }
        m.path.clear();
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const std::string kp = base + "/pathKeys/" + std::to_string(i);
            if (!keys[i].is_object() || !keys[i].contains("time") || !keys[i].contains("vertices")) {
                fail(ErrorCode::InvalidParams, "a path key is {\"time\", \"vertices\"}", Json{{"path", kp}});
            }
            MaskPathKey key;
            key.time = clip.curveTimeAt(c.timeValue(keys[i]["time"], kp + "/time"));
            key.vertices = verticesOf(keys[i]["vertices"], kp + "/vertices");
            m.path.push_back(std::move(key));
        }
        std::stable_sort(m.path.begin(), m.path.end(), [](const MaskPathKey& a, const MaskPathKey& b) { return a.time < b.time; });
    }
    if (const auto problem = maskProblem(m)) {
        fail(ErrorCode::InvalidParams, *problem);
    }
}

// ---- mask.* -------------------------------------------------------------------------------------------------------

CommandOutput maskAdd(ExecutionContext& c) {
    const ClipId id = c.clip("clip");
    const Clip& clip = clipOf(c, c.state(), id);
    videoOf(c.state(), clip);
    Mask m;
    applyMaskFields(c, clip, c.params(), m);
    if (m.shape == MaskShape::Path && m.path.empty()) {
        fail(ErrorCode::InvalidParams, "a path mask needs its \"path\" (or \"pathKeys\")", Json{{"path", "/path"}});
    }
    auto added = c.take(edit::addMask(c.state(), c.sequenceId(), id, effectParamOf(c), std::move(m)));
    CommandOutput out;
    const Clip& after = clipOf(c, added.state, id);
    const EffectInstance& holder = effectAddedTo(after, effectParamOf(c));
    out.data = Json{{"clip", id.value}, {"effect", holder.id.value}, {"mask", maskJson(c, maskIn(holder, added.id))}};
    out.createdIds.push_back(added.id);
    out.state = std::move(added.state);
    return out;
}

CommandOutput maskUpdate(ExecutionContext& c) {
    const ClipId id = c.clip("clip");
    const Clip& clip = clipOf(c, c.state(), id);
    const std::uint32_t maskId = maskIdParam(c);
    const EffectInstance& holder = effectWithMask(clip, effectParamOf(c), maskId);
    Mask m = maskIn(holder, maskId);
    applyMaskFields(c, clip, c.params(), m);
    const EffectInstanceId effect = holder.id;
    Json data = Json{{"clip", id.value}, {"effect", effect.value}, {"mask", maskJson(c, m)}};
    CommandOutput out;
    out.state = c.take(edit::updateMask(c.state(), c.sequenceId(), id, effect, std::move(m)));
    out.data = std::move(data);
    return out;
}

CommandOutput maskRemove(ExecutionContext& c) {
    const ClipId id = c.clip("clip");
    const Clip& clip = clipOf(c, c.state(), id);
    const std::uint32_t maskId = maskIdParam(c);
    const EffectInstanceId effect = effectWithMask(clip, effectParamOf(c), maskId).id;
    CommandOutput out;
    out.state = c.take(edit::removeMask(c.state(), c.sequenceId(), id, effect, maskId));
    return out;
}

CommandOutput maskList(ExecutionContext& c) {
    const ClipId id = c.clip("clip");
    const Clip& clip = clipOf(c, c.state(), id);
    Json effects = Json::array();
    for (const EffectInstance& e : clip.effects()) {
        if (e.masks.empty()) {
            continue;
        }
        Json masks = Json::array();
        for (const Mask& m : e.masks) {
            masks.push_back(maskJson(c, m));
        }
        effects.push_back(Json{{"effect", e.id.value}, {"kind", e.effect}, {"masks", std::move(masks)}});
    }
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"clip", id.value}, {"effects", std::move(effects)}};
    return out;
}

// ---- Tracking -----------------------------------------------------------------------------------------------------

TrackingServices& trackingOf(ExecutionContext& c) {
    auto* services = command::findService<TrackingServices>(c.services());
    if (!services) {
        fail(ErrorCode::InvalidRequest, "pictures are not available here: the host decodes none for tracking (wabcut-cli and the app do)");
    }
    return *services;
}

struct TrackRun {
    std::vector<Ticks> times;      ///< media time of each frame (the start frame first)
    std::vector<Motion> totals;    ///< working pixels: start picture -> that picture (identity first)
    std::vector<double> confidence;
    std::string stopped;           ///< "end", "lost", "limit"
    Ticks lostAt;
    int width = 0;
    int height = 0;
};

TrackerOptions trackerOptionsOf(ExecutionContext& c) {
    TrackerOptions o;
    const std::string model = c.string("model", "similarity");
    o.model = model == "translation" ? MotionModel::Translation : model == "affine" ? MotionModel::Affine : MotionModel::Similarity;
    if (c.has("minConfidence")) {
        o.minConfidence = c.number("minConfidence");
    }
    return o;
}

/// Media time where tracking starts: the clip's frame at "time" (default its first frame on the timeline, backward its
/// last).
Ticks startTimeOf(ExecutionContext& c, const Clip& clip, bool forward) {
    if (clip.isRemapped()) {
        fail(ErrorCode::EditRejected, "Turn off Time Remapping before tracking this clip");
    }
    if (clip.isFreeze()) {
        fail(ErrorCode::EditRejected, "clip " + std::to_string(clip.id.value) + " is a frame hold: there is no motion to track");
    }
    Ticks t = forward ? clip.start() : clip.end() - Ticks{1};
    if (c.has("time")) {
        t = c.time("time");
        if (t < clip.start() || t >= clip.end()) {
            fail(ErrorCode::InvalidParams, "the start time lies outside the clip", Json{{"path", "/time"}});
        }
    }
    return clip.sourceTimeAt(t);
}

/// "direction" is timeline time (forward = towards the clip's end, as the user sees it); a reversed clip shows its media
/// backwards, so the tracker runs the other way through the media (Codex wave-4 P2-34).
bool mediaForward(const Clip& clip, bool timelineForward) {
    return clip.reversed ? !timelineForward : timelineForward;
}

/// Tracks `region` (normalised) of the clip's media from media time `start`, forward or backward through the clip's
/// source range. Reports progress per frame; stops at the range's end, a lost frame or `maxFrames`.
TrackRun runTracker(ExecutionContext& c, TrackingServices& services, const ProjectState& state, const Clip& clip, Ticks start,
                    const std::vector<Point2>& region, bool forward, const TrackerOptions& options) {
    const int width = static_cast<int>(c.integer("workingWidth", 640));
    const auto frameAt = [&](Ticks t) { return services.frame(state, clip.media, clip.stream.index, t, width); };
    auto first = frameAt(start);
    if (!first) {
        fail(ErrorCode::EditRejected, "no picture of the clip's media to track: " + first.error().message);
    }
    TrackRun run;
    run.width = first->luma.width;
    run.height = first->luma.height;
    std::vector<Point2> polygon;
    for (const Point2& p : region) {
        polygon.push_back({p.x * run.width, p.y * run.height});
    }
    RegionTracker tracker(options);
    tracker.start(first->luma, std::move(polygon));
    run.times.push_back(first->start);
    run.totals.push_back(Motion{});
    run.confidence.push_back(1.0);
    const Ticks lo = clip.sourceIn;
    const Ticks hi = clip.sourceOut();
    const Ticks frameLength = first->duration.value > 0 ? first->duration : Ticks{kTicksPerSecond / 30};
    const std::int64_t span = forward ? (hi - first->start).value : (first->start - lo).value;
    const bool limited = c.has("frames");
    const std::int64_t limit = limited ? c.integer("frames") : std::int64_t{1} << 40;
    const int total = static_cast<int>(std::clamp<std::int64_t>(std::min(limit, span / frameLength.value), 0, 1 << 30));
    TrackingFrame previous = std::move(*first);
    run.stopped = "end";
    for (std::int64_t k = 1; k <= limit; ++k) {
        const Ticks next = forward ? previous.start + previous.duration : previous.start - Ticks{1};
        if ((forward && next >= hi) || (!forward && next < lo)) {
            break;
        }
        c.checkpoint("tracking frame " + std::to_string(k) + " of " + std::to_string(std::max<std::int64_t>(total, k)));
        auto picture = frameAt(next);
        if (!picture) {
            if (picture.error().code == wabcut::ErrorCode::NotFound) {
                break;
            }
            fail(ErrorCode::EditRejected, "a picture of the clip's media could not be decoded: " + picture.error().message);
        }
        if (picture->start == previous.start) {
            break; // no further frame in this direction
        }
        const TrackStep step = tracker.step(picture->luma);
        const int done = static_cast<int>(k);
        TrackProgress progress{done, std::max(total, done), picture->start, step.confidence, step.lost};
        services.progress(progress);
        if (step.lost) {
            run.stopped = "lost";
            run.lostAt = picture->start;
            break;
        }
        run.times.push_back(picture->start);
        run.totals.push_back(tracker.total());
        run.confidence.push_back(step.confidence);
        previous = std::move(*picture);
        if (limited && k == limit) {
            run.stopped = "limit";
        }
    }
    return run;
}

/// Working-pixel motion as a normalised one.
MaskAffine normalisedOf(const Motion& m, int width, int height) {
    const double w = width;
    const double h = height;
    return {m.a, m.b * h / w, m.c / w, m.d * w / h, m.e, m.f / h};
}

MaskAffine compose(const MaskAffine& outer, const MaskAffine& inner) {
    return {outer.a * inner.a + outer.b * inner.d, outer.a * inner.b + outer.b * inner.e, outer.a * inner.c + outer.b * inner.f + outer.c,
            outer.d * inner.a + outer.e * inner.d, outer.d * inner.b + outer.e * inner.e, outer.d * inner.c + outer.e * inner.f + outer.f};
}

/// `value` with keys `keys` replacing its keys inside [first, last] (a constant keeps its value outside).
ParamValue withKeys(const ParamValue& value, const std::vector<Keyframe>& keys) {
    if (keys.empty()) {
        return value;
    }
    const Ticks first = std::min(keys.front().time, keys.back().time);
    const Ticks last = std::max(keys.front().time, keys.back().time);
    Curve curve;
    if (const Curve* old = value.curve()) {
        for (const Keyframe& k : old->keys) {
            if (k.time < first || k.time > last) {
                curve.keys.push_back(k);
            }
        }
    }
    for (const Keyframe& k : keys) {
        curve.keys.push_back(k);
    }
    std::sort(curve.keys.begin(), curve.keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    return ParamValue(std::move(curve));
}

Json runJson(ExecutionContext& c, const TrackRun& run, bool forward) {
    Json confidence = Json::array();
    for (const double v : run.confidence) {
        confidence.push_back(std::round(v * 1000.0) / 1000.0);
    }
    (void)c;
    Json out{{"frames", static_cast<int>(run.times.size()) - 1},
             {"direction", forward ? "forward" : "backward"},
             {"stopped", run.stopped},
             {"firstMediaTime", run.times.front().value},
             {"lastMediaTime", run.times.back().value},
             {"confidence", std::move(confidence)}};
    if (run.stopped == "lost") {
        out["lostAtMediaTime"] = run.lostAt.value;
    }
    return out;
}

CommandOutput trackMask(ExecutionContext& c) {
    TrackingServices& services = trackingOf(c);
    const ClipId id = c.clip("clip");
    const ProjectState& state = c.state();
    const Clip& clip = clipOf(c, state, id);
    const VideoStreamInfo& stream = videoOf(state, clip);
    const std::uint32_t maskId = maskIdParam(c);
    const EffectInstance& holder = effectWithMask(clip, effectParamOf(c), maskId);
    const EffectInstanceId effect = holder.id;
    Mask mask = maskIn(holder, maskId);
    const bool forward = c.string("direction", "forward") != "backward";
    const TrackerOptions options = trackerOptionsOf(c);
    const Ticks start = startTimeOf(c, clip, forward);
    const double aspect = displayAspect(stream);
    const std::vector<Point2> region = maskPolygonAt(mask, start, aspect);
    if (region.size() < 3) {
        fail(ErrorCode::EditRejected, "the mask has no area to track");
    }
    const bool inMedia = mediaForward(clip, forward);
    const TrackRun run = runTracker(c, services, state, clip, start, region, inMedia, options);
    // Each frame's mask transform: the tracked motion after the transform at the start.
    const Ticks t0 = run.times.front();
    const MaskAffine m0 = maskTransformAt(mask, t0, aspect);
    const double anchorX = maskValue(mask, "transform.anchorX", t0);
    const double anchorY = maskValue(mask, "transform.anchorY", t0);
    const char* keys[] = {"transform.x", "transform.y", "transform.scaleX", "transform.scaleY", "transform.rotation", "transform.skew"};
    const std::size_t written = options.model == MotionModel::Translation ? 2 : options.model == MotionModel::Similarity ? 5 : 6;
    std::vector<std::vector<Keyframe>> curves(written);
    double rotation = maskValue(mask, "transform.rotation", t0);
    const bool flipped = maskValue(mask, "transform.scaleX", t0) < 0.0;
    for (std::size_t k = 0; k < run.times.size(); ++k) {
        const MaskAffine mk = compose(normalisedOf(run.totals[k], run.width, run.height), m0);
        const MaskTransformValues v = maskTransformValues(mk, anchorX, anchorY, aspect, rotation, flipped);
        rotation = v.rotation;
        const double values[] = {v.x, v.y, v.scaleX, v.scaleY, v.rotation, v.skew};
        for (std::size_t p = 0; p < written; ++p) {
            const MaskParamInfo* info = maskParam(keys[p]);
            const double value = k == 0 ? maskValue(mask, keys[p], t0) : std::clamp(values[p], info->minValue, info->maxValue);
            curves[p].push_back(Keyframe{run.times[k], value});
        }
    }
    if (!inMedia) {
        for (auto& curve : curves) {
            std::reverse(curve.begin(), curve.end());
        }
    }
    for (std::size_t p = 0; p < written; ++p) {
        const ParamValue* old = mask.find(keys[p]);
        mask.set(keys[p], withKeys(old ? *old : ParamValue(maskParam(keys[p])->defaultValue), curves[p]));
    }
    CommandOutput out;
    out.state = c.take(edit::updateMask(state, c.sequenceId(), id, effect, mask));
    out.data = runJson(c, run, forward);
    out.data["clip"] = id.value;
    out.data["effect"] = effect.value;
    out.data["mask"] = maskId;
    out.data["keyframes"] = run.times.size() * written;
    if (run.stopped == "lost") {
        out.warnings.push_back("the picture was lost at media time " + std::to_string(run.lostAt.value) +
                               " ticks; keyframes stop at the last frame tracked");
    }
    return out;
}

/// Canvas position of a frame point of a clip (sequence pixels), as the compositor places it (render::layerToCanvas).
Point2 placeOnCanvas(const Clip& clip, Ticks curveTime, const VideoStreamInfo& v, const Sequence& seq, Point2 q) {
    const ClipProperties& p = clip.properties();
    const double par = v.sampleAspect.isValid() ? v.sampleAspect.toDouble() : 1.0;
    const double rad = p.value(ParamId::Rotation, curveTime) * std::numbers::pi / 180.0;
    const double s = p.value(ParamId::Scale, curveTime) * // P.2: Scale to Frame Size
                     scaleToFrameFactor(clip, streamPictureSize(v), seq.width, seq.height);
    const double x = s * (par * (q.x - v.width / 2.0) - p.value(ParamId::AnchorX, curveTime));
    const double y = s * (q.y - v.height / 2.0 - p.value(ParamId::AnchorY, curveTime));
    return {seq.width / 2.0 + p.value(ParamId::PositionX, curveTime) + std::cos(rad) * x - std::sin(rad) * y,
            seq.height / 2.0 + p.value(ParamId::PositionY, curveTime) + std::sin(rad) * x + std::cos(rad) * y};
}

/// The timeline time at which a clip shows media time `media`.
Ticks timelineTimeOf(const Clip& clip, Ticks media) {
    const Ticks offset = clip.reversed ? clip.sourceOut() - Ticks{1} - media : media - clip.sourceIn;
    return clip.start() + toTimelineTicks(offset, clip.speed);
}

CommandOutput trackTransform(ExecutionContext& c) {
    TrackingServices& services = trackingOf(c);
    const ClipId id = c.clip("clip");
    const ProjectState& state = c.state();
    const Sequence& seq = c.sequence();
    const Clip& clip = clipOf(c, state, id);
    const VideoStreamInfo& stream = videoOf(state, clip);
    const double aspect = displayAspect(stream);
    const bool forward = c.string("direction", "forward") != "backward";
    TrackerOptions options = trackerOptionsOf(c);
    if (options.model == MotionModel::Affine) {
        options.model = MotionModel::Similarity; // Motion has no skew
    }
    const Ticks start = startTimeOf(c, clip, forward);
    // The region: a mask's outline, a rectangle, or a detector's find (the AI-9 seam).
    std::vector<Point2> region;
    const Json& r = c.params().contains("region") ? c.params()["region"] : Json();
    if (r.is_object() && r.contains("mask")) {
        const auto maskId = static_cast<std::uint32_t>(r["mask"].get<std::int64_t>());
        const EffectInstance& holder = effectWithMask(clip, EffectInstanceId{r.value("effect", std::uint64_t{0})}, maskId);
        region = maskPolygonAt(maskIn(holder, maskId), start, aspect);
    } else if (r.is_object() && r.contains("detect")) {
        const int width = static_cast<int>(c.integer("workingWidth", 640));
        auto picture = services.frame(state, clip.media, clip.stream.index, start, width);
        if (!picture) {
            fail(ErrorCode::EditRejected, "no picture of the clip's media: " + picture.error().message);
        }
        auto found = services.detect(picture->luma, r["detect"].get<std::string>());
        if (!found) {
            fail(ErrorCode::EditRejected, found.error().message);
        }
        if (found->empty()) {
            fail(ErrorCode::EditRejected, "nothing of that kind was found in the first frame");
        }
        const DetectedRegion& d = found->front();
        region = {{d.x, d.y}, {d.x + d.width, d.y}, {d.x + d.width, d.y + d.height}, {d.x, d.y + d.height}};
    } else if (r.is_object() && r.contains("x") && r.contains("y") && r.contains("width") && r.contains("height")) {
        const double x = r["x"].get<double>(), y = r["y"].get<double>(), w = r["width"].get<double>(), h = r["height"].get<double>();
        if (!(w > 0.0 && h > 0.0)) {
            fail(ErrorCode::InvalidParams, "the region needs a size", Json{{"path", "/region"}});
        }
        region = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
    } else {
        fail(ErrorCode::InvalidParams, "\"region\" is {\"x\", \"y\", \"width\", \"height\"}, {\"mask\": id} or {\"detect\": kind}",
             Json{{"path", "/region"}});
    }
    const ClipId targetId = c.has("target") ? c.clip("target") : id;
    const Clip& target = clipOf(c, state, targetId);
    const std::string mode = c.string("mode", targetId == id ? "stabilize" : "follow");
    if (mode == "stabilize" && targetId != id) {
        fail(ErrorCode::InvalidParams, "stabilize keys the tracked clip itself: leave out \"target\"", Json{{"path", "/mode"}});
    }
    const TrackRun run = runTracker(c, services, state, clip, start, region, mediaForward(clip, forward), options);

    Point2 centre;
    for (const Point2& p : region) {
        centre.x += p.x / static_cast<double>(region.size());
        centre.y += p.y / static_cast<double>(region.size());
    }
    // The feature in source pixels at frame k, and where the tracked clip shows it.
    const Point2 f0{centre.x * run.width, centre.y * run.height};
    const double toSourceX = static_cast<double>(stream.width) / run.width;
    const double toSourceY = static_cast<double>(stream.height) / run.height;
    const auto sourcePoint = [&](std::size_t k) {
        const Point2 p = run.totals[k].map(f0);
        return Point2{p.x * toSourceX, p.y * toSourceY};
    };
    const Ticks startTimeline = timelineTimeOf(clip, run.times.front());
    const Ticks c0 = clip.curveTimeAt(startTimeline);
    const Point2 canvas0 = placeOnCanvas(clip, c0, stream, seq, sourcePoint(0));
    const ClipProperties& tp = target.properties();
    const Ticks targetStart = target.curveTimeAt(std::clamp(startTimeline, target.start(), target.end() - Ticks{1}));
    std::vector<Keyframe> px, py, scale, rotation;
    const bool similarity = options.model == MotionModel::Similarity;
    double rotationPrevious = 0.0;
    for (std::size_t k = 0; k < run.times.size(); ++k) {
        const Ticks timeline = timelineTimeOf(clip, run.times[k]);
        if (timeline < target.start() || timeline >= target.end()) {
            continue;
        }
        const Ticks ct = target.curveTimeAt(timeline);
        const Motion& m = run.totals[k];
        double theta = std::atan2(m.d, m.a) * 180.0 / std::numbers::pi;
        theta += 360.0 * std::round((rotationPrevious - theta) / 360.0);
        rotationPrevious = theta;
        const double s = std::sqrt(std::max(1e-12, std::abs(m.determinant())));
        double x = 0.0, y = 0.0, sc = tp.value(ParamId::Scale, ct), rot = tp.value(ParamId::Rotation, ct);
        if (mode == "follow") {
            const Point2 now = placeOnCanvas(clip, clip.curveTimeAt(timeline), stream, seq, sourcePoint(k));
            x = tp.value(ParamId::PositionX, targetStart) + (now.x - canvas0.x);
            y = tp.value(ParamId::PositionY, targetStart) + (now.y - canvas0.y);
            if (similarity) {
                sc = tp.value(ParamId::Scale, targetStart) * s;
                rot = tp.value(ParamId::Rotation, targetStart) + theta;
            }
        } else {
            // Stabilize: the feature stays where it was at the start, upright and the same size.
            Clip held = clip;
            ClipProperties props = clip.properties();
            if (similarity) {
                sc = clip.properties().value(ParamId::Scale, c0) / s;
                rot = clip.properties().value(ParamId::Rotation, c0) - theta;
            }
            props[ParamId::Scale] = ParamValue(sc);
            props[ParamId::Rotation] = ParamValue(rot);
            props[ParamId::PositionX] = ParamValue(0.0);
            props[ParamId::PositionY] = ParamValue(0.0);
            props[ParamId::AnchorX] = ParamValue(clip.properties().value(ParamId::AnchorX, ct));
            props[ParamId::AnchorY] = ParamValue(clip.properties().value(ParamId::AnchorY, ct));
            held.setProperties(props);
            const Point2 unplaced = placeOnCanvas(held, ct, stream, seq, sourcePoint(k));
            x = canvas0.x - unplaced.x;
            y = canvas0.y - unplaced.y;
        }
        px.push_back(Keyframe{ct, x});
        py.push_back(Keyframe{ct, y});
        scale.push_back(Keyframe{ct, sc});
        rotation.push_back(Keyframe{ct, rot});
    }
    if (px.empty()) {
        fail(ErrorCode::EditRejected, "the target clip does not overlap the tracked frames");
    }
    const auto sorted = [](std::vector<Keyframe> keys) {
        std::sort(keys.begin(), keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
        keys.erase(std::unique(keys.begin(), keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.time == b.time; }), keys.end());
        return keys;
    };
    ProjectState next = state;
    const auto setParam = [&](ParamId pid, const std::vector<Keyframe>& keys) {
        const Clip& t = clipOf(c, next, targetId);
        const ParameterInfo& info = parameterInfo(pid);
        std::vector<Keyframe> clamped = sorted(keys);
        for (Keyframe& k : clamped) {
            k.value = std::clamp(k.value, info.minValue, info.maxValue);
        }
        next = c.take(edit::setParameter(next, c.sequenceId(), targetId, edit::ParamRef::of(pid), withKeys(t.properties()[pid], clamped))).state;
    };
    setParam(ParamId::PositionX, px);
    setParam(ParamId::PositionY, py);
    if (similarity) {
        setParam(ParamId::Scale, scale);
        setParam(ParamId::Rotation, rotation);
    }
    CommandOutput out;
    out.state = std::move(next);
    out.data = runJson(c, run, forward);
    out.data["clip"] = id.value;
    out.data["target"] = targetId.value;
    out.data["mode"] = mode;
    out.data["keyframes"] = px.size() * (similarity ? 4 : 2);
    if (run.stopped == "lost") {
        out.warnings.push_back("the picture was lost at media time " + std::to_string(run.lostAt.value) +
                               " ticks; keyframes stop at the last frame tracked");
    }
    return out;
}

// ---- redaction.* ---------------------------------------------------------------------------------------------------

std::optional<RedactionId> redactionIdOf(ExecutionContext& c) {
    const std::int64_t id = c.integer("redaction", 0);
    if (id <= 0) {
        return std::nullopt;
    }
    return RedactionId{static_cast<std::uint64_t>(id)};
}

const Redaction& redactionIn(ExecutionContext& c, const ProjectState& state, RedactionId id) {
    const auto it = std::find_if(state.redactions.begin(), state.redactions.end(), [&](const Redaction& r) { return r.id == id; });
    if (it == state.redactions.end()) {
        fail(ErrorCode::NotFound, "redaction " + std::to_string(id.value) + " does not exist");
    }
    (void)c;
    return *it;
}

/// A media time: ticks, or a timecode in the media's own rate (command::sourceTimecodeRate, as the source commands and
/// the Source Monitor read them). Not the sequence's rate: a 24p source in a 30p sequence would move every boundary
/// and leave edge frames unredacted (Codex wave-4 P2-28).
Ticks mediaTimeValue(const Json& v, const std::string& path, Rate mediaRate) {
    if (v.is_number_integer()) {
        return Ticks{v.get<std::int64_t>()};
    }
    if (v.is_string()) {
        const auto parsed = parseTimecode(v.get<std::string>(), mediaRate, false);
        if (!parsed) {
            fail(ErrorCode::InvalidParams, parsed.error().message, Json{{"path", path}});
        }
        return *parsed;
    }
    fail(ErrorCode::InvalidParams, "expected ticks or a timecode in the media's rate", Json{{"path", path}});
}

TimeRange timeRangeOf(const Json& v, const std::string& path, Rate mediaRate) {
    if (!v.is_object() || !v.contains("start") || !v.contains("end")) {
        fail(ErrorCode::InvalidParams, "a media time range is {\"start\", \"end\"}", Json{{"path", path}});
    }
    const Ticks a = mediaTimeValue(v["start"], path + "/start", mediaRate);
    const Ticks b = mediaTimeValue(v["end"], path + "/end", mediaRate);
    if (b <= a || a.value < 0) {
        fail(ErrorCode::InvalidParams, "the range must start at or after 0 and end after it starts", Json{{"path", path}});
    }
    return TimeRange::fromStartEnd(a, b);
}

/// A redaction's region from the params (Codex wave-4 P2-27): either a box {x, y, width, height[, shape, feather]}, all
/// four numbers and a positive size, or mask fields; any other key is refused instead of silently ignored, an inverted
/// region (which would expose what it should hide) is refused, and the result must have an area.
Mask redactionRegionOf(ExecutionContext& c, const Json& g, Mask current) {
    if (!g.is_object()) {
        fail(ErrorCode::InvalidParams, "region is an object", Json{{"path", "/region"}});
    }
    const bool box = g.contains("x") || g.contains("y") || g.contains("width") || g.contains("height");
    static constexpr std::string_view kBoxKeys[] = {"x", "y", "width", "height", "shape", "feather"};
    static constexpr std::string_view kMaskKeys[] = {"name", "shape", "mode", "params", "path", "pathKeys", "enabled", "inverted"};
    for (const auto& [key, value] : g.items()) {
        const auto known = [&](std::span<const std::string_view> keys) { return std::ranges::find(keys, key) != keys.end(); };
        if (box ? !known(kBoxKeys) : !known(kMaskKeys)) {
            fail(ErrorCode::InvalidParams,
                 "unknown region key \"" + key + "\"" +
                     (box ? " (a box is x, y, width, height, shape and feather)" : " (mask fields: shape, params, path, pathKeys)"),
                 Json{{"path", "/region/" + key}});
        }
    }
    if (g.contains("inverted") && g["inverted"].get<bool>()) {
        fail(ErrorCode::InvalidParams, "a redaction region cannot be inverted: it would show what it should hide",
             Json{{"path", "/region/inverted"}});
    }
    Mask m = std::move(current);
    if (box) {
        double v[4] = {};
        const char* keys[] = {"x", "y", "width", "height"};
        for (int i = 0; i < 4; ++i) {
            if (!g.contains(keys[i]) || !g[keys[i]].is_number() || !std::isfinite(g[keys[i]].get<double>())) {
                fail(ErrorCode::InvalidParams, "a region box needs x, y, width and height (0..1 of the frame)",
                     Json{{"path", std::string("/region/") + keys[i]}});
            }
            v[i] = g[keys[i]].get<double>();
        }
        if (!(v[2] > 0.0) || !(v[3] > 0.0)) {
            fail(ErrorCode::InvalidParams, "a region box needs a positive width and height", Json{{"path", "/region"}});
        }
        const std::string shape = g.value("shape", std::string("rectangle"));
        if (shape != "rectangle" && shape != "ellipse") {
            fail(ErrorCode::InvalidParams, "a region box is a rectangle or an ellipse", Json{{"path", "/region/shape"}});
        }
        m.shape = shape == "ellipse" ? MaskShape::Ellipse : MaskShape::Rectangle;
        m.set("center.x", v[0] + v[2] / 2.0);
        m.set("center.y", v[1] + v[3] / 2.0);
        m.set("size.x", v[2]);
        m.set("size.y", v[3]);
        if (g.contains("feather")) {
            m.set("feather", g["feather"].get<double>());
        }
    } else {
        // Region keyframes are in media time: a clip-free context maps "time" values straight to media time.
        Clip identity;
        identity.timelineRange = {Ticks{}, Ticks{std::int64_t{1} << 62}};
        applyMaskFields(c, identity, g, m, "/region");
    }
    if (m.shape != MaskShape::Path) {
        for (const char* key : {"size.x", "size.y"}) {
            const ParamValue* size = m.find(key);
            if (size && size->isConstant() && !(size->constant() > 0.0)) {
                fail(ErrorCode::InvalidParams, std::string("the region has no area: ") + key + " is 0",
                     Json{{"path", "/region"}});
            }
        }
    }
    m.mode = MaskMode::Add;
    m.inverted = false;
    return m;
}

Json redactionJson(const Redaction& r) {
    Json words = Json::array();
    for (const TimeRange& w : r.audioRanges) {
        words.push_back(Json{{"start", w.start.value}, {"end", w.end().value}});
    }
    Json exceptions = Json::array();
    for (const ClipId e : r.exceptions) {
        exceptions.push_back(e.value);
    }
    return Json{{"id", r.id.value},
                {"media", r.media.value},
                {"name", r.name},
                {"enabled", r.enabled},
                {"source", Json{{"start", r.source.start.value}, {"end", r.source.end().value}}},
                {"shape", std::string(toString(r.region.shape))},
                {"style", std::string(toString(r.style))},
                {"strength", r.strength},
                {"color", Json::array({r.colorR, r.colorG, r.colorB})},
                {"audio", std::string(toString(r.audio))},
                {"words", std::move(words)},
                {"exceptions", std::move(exceptions)}};
}

/// Applies the redaction fields of the params to `r`.
void applyRedactionFields(ExecutionContext& c, Redaction& r, bool adding) {
    const Json& p = c.params();
    if (p.contains("clip") && (p.contains("in") || p.contains("out"))) {
        // Marked through a clip: its media and the media time it shows between "in" and "out" (sequence times).
        const ClipId id = c.clip("clip");
        const Clip& clip = clipOf(c, c.state(), id);
        const Ticks in = c.has("in") ? c.time("in") : clip.start();
        const Ticks out = c.has("out") ? c.time("out") : clip.end();
        if (out <= in || in < clip.start() || out > clip.end()) {
            fail(ErrorCode::InvalidParams, "\"in\" and \"out\" must lie inside the clip, in order", Json{{"path", "/in"}});
        }
        const auto [a, b] = clip.isRemapped()
                                ? remapShownRange(*clip.timeRemap(), clip.curveTimeAt(in), clip.curveTimeAt(out))
                                : std::pair{clip.sourceTimeAt(in), clip.sourceTimeAt(out - Ticks{1})};
        r.media = clip.media;
        r.source = TimeRange::fromStartEnd(std::min(a, b), std::max(a, b) + Ticks{1});
    } else if (p.contains("media")) {
        r.media = c.media("media");
    } else if (adding) {
        fail(ErrorCode::InvalidParams, "name the \"media\" (with \"source\") or a \"clip\" with \"in\" and \"out\"");
    }
    const MediaItem* item = c.state().findMedia(r.media);
    if (!item) {
        fail(ErrorCode::NotFound, "media " + std::to_string(r.media.value) + " does not exist");
    }
    const Rate mediaRate = command::sourceTimecodeRate(c.state(), *item);
    if (p.contains("source")) {
        r.source = timeRangeOf(p["source"], "/source", mediaRate);
    } else if (adding && !p.contains("clip")) {
        fail(ErrorCode::InvalidParams, "\"source\": the media time range to redact", Json{{"path", "/source"}});
    }
    if (p.contains("name")) {
        r.name = p["name"].get<std::string>();
    }
    if (p.contains("enabled")) {
        r.enabled = p["enabled"].get<bool>();
    }
    if (p.contains("region")) {
        r.region = redactionRegionOf(c, p["region"], r.region);
    }
    if (p.contains("style")) {
        const auto s = redactionStyleFromString(p["style"].get<std::string>());
        if (!s) {
            fail(ErrorCode::InvalidParams, "style is blur, mosaic or solid", Json{{"path", "/style"}});
        }
        r.style = *s;
    }
    if (p.contains("strength")) {
        r.strength = p["strength"].get<double>();
    }
    if (p.contains("color")) {
        const Json& col = p["color"];
        if (!col.is_array() || col.size() != 3) {
            fail(ErrorCode::InvalidParams, "color is [r, g, b] in 0..1", Json{{"path", "/color"}});
        }
        r.colorR = col[0].get<double>(), r.colorG = col[1].get<double>(), r.colorB = col[2].get<double>();
    }
    if (p.contains("audio")) {
        const auto a = redactionAudioFromString(p["audio"].get<std::string>());
        if (!a) {
            fail(ErrorCode::InvalidParams, "audio is none, mute or bleep", Json{{"path", "/audio"}});
        }
        r.audio = *a;
    }
    if (p.contains("words")) {
        r.audioRanges.clear();
        for (std::size_t i = 0; i < p["words"].size(); ++i) {
            r.audioRanges.push_back(timeRangeOf(p["words"][i], "/words/" + std::to_string(i), mediaRate));
        }
    }
    if (const auto problem = redactionProblem(r)) {
        fail(ErrorCode::InvalidParams, *problem);
    }
}

CommandOutput redactionAdd(ExecutionContext& c) {
    Redaction r;
    r.region.shape = MaskShape::Ellipse;
    r.region.set("size.x", 0.3);
    r.region.set("size.y", 0.3);
    applyRedactionFields(c, r, true);
    auto added = c.take(edit::addRedaction(c.state(), std::move(r)));
    CommandOutput out;
    out.data = redactionJson(*std::find_if(added.state.redactions.begin(), added.state.redactions.end(),
                                           [&](const Redaction& x) { return x.id == added.id; }));
    out.createdIds.push_back(added.id.value);
    out.state = std::move(added.state);
    return out;
}

CommandOutput redactionUpdate(ExecutionContext& c) {
    const auto id = redactionIdOf(c);
    if (!id) {
        fail(ErrorCode::InvalidParams, "\"redaction\": the redaction's id", Json{{"path", "/redaction"}});
    }
    Redaction r = redactionIn(c, c.state(), *id);
    applyRedactionFields(c, r, false);
    CommandOutput out;
    out.data = redactionJson(r);
    out.state = c.take(edit::updateRedaction(c.state(), std::move(r)));
    return out;
}

CommandOutput redactionRemove(ExecutionContext& c) {
    const auto id = redactionIdOf(c);
    if (!id) {
        fail(ErrorCode::InvalidParams, "\"redaction\": the redaction's id", Json{{"path", "/redaction"}});
    }
    redactionIn(c, c.state(), *id);
    CommandOutput out;
    out.state = c.take(edit::removeRedaction(c.state(), *id));
    return out;
}

CommandOutput redactionOverride(ExecutionContext& c) {
    const auto id = redactionIdOf(c);
    if (!id) {
        fail(ErrorCode::InvalidParams, "\"redaction\": the redaction's id", Json{{"path", "/redaction"}});
    }
    redactionIn(c, c.state(), *id);
    const ClipId clip = c.clip("clip");
    const bool redact = c.boolean("redact", false);
    std::vector<ClipId> clips{clip};
    if (c.boolean("linked", true)) {
        const Clip& cl = clipOf(c, c.state(), clip);
        if (cl.linkGroup.isValid()) {
            const Sequence& seq = c.sequence();
            for (const auto* list : {&seq.videoTracks, &seq.audioTracks}) {
                for (const TrackPtr& t : *list) {
                    for (const Clip& other : t->clips) {
                        if (other.id != clip && other.linkGroup == cl.linkGroup) {
                            clips.push_back(other.id);
                        }
                    }
                }
            }
        }
    }
    ProjectState state = c.state();
    for (const ClipId each : clips) {
        state = c.take(edit::setRedactionOverride(state, *id, each, redact));
    }
    CommandOutput out;
    Json changed = Json::array();
    for (const ClipId each : clips) {
        changed.push_back(each.value);
    }
    out.data = Json{{"redaction", id->value}, {"clips", std::move(changed)}, {"redact", redact}};
    out.state = std::move(state);
    return out;
}

CommandOutput redactionList(ExecutionContext& c) {
    std::optional<MediaId> media;
    if (c.has("media")) {
        media = c.media("media");
    }
    Json list = Json::array();
    for (const Redaction& r : c.state().redactions) {
        // A media item's redactions include its subclip master's and stem source's (review wave 5, RedactionMedia.h).
        if (!media || redactionCovers(c.state(), r, *media)) {
            list.push_back(redactionJson(r));
        }
    }
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"redactions", std::move(list)}};
    return out;
}

Json maskFieldsSchema(std::initializer_list<std::pair<const char*, Json>> extra, std::vector<std::string> required) {
    Json s = withSequence(extra, std::move(required));
    auto& p = s["properties"];
    p["name"] = schema::string("Display name, e.g. \"Face\".");
    p["shape"] = schema::enumeration({"ellipse", "rectangle", "path"}, "Default ellipse.");
    p["mode"] = schema::enumeration({"add", "subtract", "intersect"}, "How it combines with the masks before it. Default add.");
    p["inverted"] = schema::boolean("Cover the outside instead.");
    p["enabled"] = schema::boolean("On or off.");
    p["params"] = freeObject("Parameter -> number or {\"keys\": [{\"time\", \"value\", \"interpolation\"}]} (key times are "
                             "sequence times on the clip): center.x/.y, size.x/.y (0..1 of the frame), rotation, roundness, "
                             "feather and expansion (frame heights), opacity, transform.x/.y/.scaleX/.scaleY/.rotation/.skew/"
                             ".anchorX/.anchorY.");
    p["path"] = freeArray("Path shape: its vertices, [x, y] or {x, y, inX, inY, outX, outY} (tangents as offsets), 0..1 of "
                          "the frame.");
    p["pathKeys"] = freeArray("Animated path: [{\"time\", \"vertices\"}], every key with the same vertex count.");
    return s;
}

Json redactionFieldsSchema(std::initializer_list<std::pair<const char*, Json>> extra, std::vector<std::string> required) {
    Json s = withSequence(extra, std::move(required));
    auto& p = s["properties"];
    p["media"] = Json{{"type", Json::array({"integer", "string"})}, {"description", "The media item (id or exact name)."}};
    p["source"] = freeObject("{\"start\", \"end\"}: the media time it covers (ticks, or a timecode).");
    p["clip"] = clipParam("Or mark through a clip: its media, between \"in\" and \"out\".");
    p["in"] = timeParam("Sequence time inside the clip where the marked range starts.");
    p["out"] = timeParam("Sequence time inside the clip where it ends.");
    p["name"] = schema::string("e.g. \"Licence plate\".");
    p["enabled"] = schema::boolean("On or off.");
    p["region"] = freeObject("The area: {\"x\", \"y\", \"width\", \"height\", \"shape\": \"rectangle\"|\"ellipse\", \"feather\"} "
                             "(0..1 of the frame), or mask fields {\"shape\", \"params\", \"path\", \"pathKeys\"} (key times are "
                             "media times). Default a centred ellipse.");
    p["style"] = schema::enumeration({"blur", "mosaic", "solid"}, "Default blur.");
    p["strength"] = schema::number("Blur radius or mosaic cell in frame heights. Default 0.05.", 0.0, 1.0);
    p["color"] = freeArray("Solid: [r, g, b], 0..1.");
    p["audio"] = schema::enumeration({"none", "mute", "bleep"}, "What happens to its sound. Default none.");
    p["words"] = freeArray("Only these media time ranges are muted or bleeped (transcript words): [{\"start\", \"end\"}].");
    return s;
}

} // namespace

void registerMaskCommands(command::CommandRegistry& registry) {
    using command::Capability;
    add(registry,
        {"mask.add", 1, "Add Mask",
         "Adds a mask to a clip: to an effect (\"effect\"; the effect then only changes the masked area - a local correction, "
         "including the colour grade) or, without \"effect\", to the clip's opacity mask (it cuts the clip out). Shapes: "
         "ellipse and rectangle (center, size, rotation, roundness) or a Bezier \"path\". Edge: feather and expansion; "
         "opacity; invert; add/subtract/intersect with the masks before it. Every parameter can be keyframed. Returns the "
         "mask's id.",
         Capability::EditSequence, maskFieldsSchema({{"clip", clipParam("The video clip.")}, {"effect", effectParam()}}, {"clip"}),
         Json{{"clip", 42}, {"shape", "ellipse"}, {"params", Json{{"center.x", 0.4}, {"size.x", 0.3}, {"size.y", 0.45}, {"feather", 0.05}}}},
         {}, maskAdd});
    add(registry,
        {"mask.update", 1, "Update Mask",
         "Changes a mask: only the fields given (name, shape, mode, inverted, enabled, params, path / pathKeys). A number "
         "replaces an animated parameter; {\"keys\": [...]} replaces its keys.",
         Capability::EditSequence,
         maskFieldsSchema({{"clip", clipParam("The clip.")}, {"effect", effectParam()}, {"mask", schema::integer("The mask id.", 1)}},
                          {"clip", "mask"}),
         Json{{"clip", 42}, {"mask", 1}, {"inverted", true}, {"params", Json{{"expansion", 0.02}}}}, {}, maskUpdate});
    add(registry,
        {"mask.remove", 1, "Remove Mask", "Removes a mask from its effect.", Capability::EditSequence,
         withSequence({{"clip", clipParam("The clip.")}, {"effect", effectParam()}, {"mask", schema::integer("The mask id.", 1)}},
                      {"clip", "mask"}),
         Json{{"clip", 42}, {"mask", 1}}, {}, maskRemove});
    add(registry,
        {"mask.list", 1, "List Masks", "A clip's masks by effect, with their parameters (keys at media times) and paths.",
         Capability::Read, withSequence({{"clip", clipParam("The clip.")}}, {"clip"}), Json{{"clip", 42}}, {}, maskList});
    add(registry,
        {"track.mask", 1, "Track Mask",
         "Tracks a mask through the clip's media, frame by frame from \"time\" (forward or backward, to the clip's end or "
         "\"frames\" frames), with pyramidal Lucas-Kanade on features inside the mask and a robust fit of its motion "
         "(\"model\"). Its transform.* parameters get ordinary keyframes (one undo step). Stops where the picture is lost "
         "(confidence below \"minConfidence\"); data lists each frame's confidence. A job: it reports progress and can be "
         "cancelled.",
         Capability::EditSequence,
         trackingParams({{"clip", clipParam("The clip.")}, {"effect", effectParam()}, {"mask", schema::integer("The mask id.", 1)}},
                        {"clip", "mask"}),
         Json{{"clip", 42}, {"mask", 1}, {"direction", "forward"}}, {}, trackMask});
    add(registry,
        {"track.transform", 1, "Track Motion",
         "Tracks a region of a clip (\"region\": {x, y, width, height} 0..1 of the frame, {\"mask\": id}, or {\"detect\": "
         "\"face\"} once a detector is installed) and keys a clip's Motion with it (one undo step): \"follow\" moves "
         "\"target\" with the region (position; with the similarity model also scale and rotation), \"stabilize\" keys the "
         "tracked clip so the region stays still. A job: it reports progress and can be cancelled.",
         Capability::EditSequence,
         trackingParams({{"clip", clipParam("The tracked clip.")},
                         {"region", freeObject("What to track.")},
                         {"target", clipParam("The clip that follows. Default the tracked clip (stabilize).")},
                         {"mode", schema::enumeration({"follow", "stabilize"}, "Default follow with a target, else stabilize.")}},
                        {"clip", "region"}),
         Json{{"clip", 42}, {"region", Json{{"x", 0.4}, {"y", 0.3}, {"width", 0.2}, {"height", 0.2}}}, {"target", 57}}, {},
         trackTransform});
    add(registry,
        {"redaction.add", 1, "Add Redaction",
         "Marks a region of a media item in SOURCE time for privacy: every use of that range in every sequence (trimmed, "
         "sped up, reversed, reused) is blurred, pixelated or covered in preview, export and burn-in, before the clip's own "
         "effects; \"audio\" mutes or bleeps it (only \"words\" when given). Mark by \"media\" + \"source\", or through a "
         "\"clip\" between \"in\" and \"out\". The region can be keyframed or tracked (track.mask on a clip's mask, then "
         "copied) - its key times are media times. Returns the redaction's id.",
         Capability::EditProject, redactionFieldsSchema({}, {}),
         Json{{"clip", 42}, {"in", "00:00:03:00"}, {"out", "00:00:06:00"}, {"style", "mosaic"},
              {"region", Json{{"x", 0.6}, {"y", 0.55}, {"width", 0.15}, {"height", 0.08}}}},
         {}, redactionAdd});
    add(registry,
        {"redaction.update", 1, "Update Redaction", "Changes a redaction: only the fields given.", Capability::EditProject,
         redactionFieldsSchema({{"redaction", schema::integer("The redaction id.", 1)}}, {"redaction"}),
         Json{{"redaction", 3}, {"style", "solid"}, {"color", Json::array({0, 0, 0})}}, {}, redactionUpdate});
    add(registry,
        {"redaction.remove", 1, "Remove Redaction", "Removes a redaction everywhere.", Capability::EditProject,
         withSequence({{"redaction", schema::integer("The redaction id.", 1)}}, {"redaction"}), Json{{"redaction", 3}}, {},
         redactionRemove});
    add(registry,
        {"redaction.override", 1, "Redaction Override",
         "\"Don't redact here\": the redaction skips this clip (and, with \"linked\", its linked partners), or applies again "
         "with \"redact\": true. A clip later cut from an excepted one is redacted again until excepted too.",
         Capability::EditProject,
         withSequence({{"redaction", schema::integer("The redaction id.", 1)},
                       {"clip", clipParam("The clip.")},
                       {"redact", schema::boolean("true: redact this clip again. Default false (except it).")},
                       {"linked", schema::boolean("Its linked partners too. Default true.")}},
                      {"redaction", "clip"}),
         Json{{"redaction", 3}, {"clip", 42}}, {}, redactionOverride});
    add(registry,
        {"redaction.list", 1, "List Redactions", "The project's redactions (of one media item with \"media\").", Capability::Read,
         withSequence({{"media", Json{{"type", Json::array({"integer", "string"})}, {"description", "Only this media item."}}}}),
         Json::object(), {}, redactionList});
}

} // namespace wabcut::masks
