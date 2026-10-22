#include <wabcut/ai/segment/ObjectMask.h>

#include <wabcut/ai/analysis/AnalysisCommands.h>
#include <wabcut/command/Schema.h>
#include <wabcut/model/ProjectOps.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace wabcut::ai::segment {

using command::CommandOutput;
using command::ExecutionContext;
using command::fail;
using command::Json;
namespace schema = command::schema;
using CommandError = command::ErrorCode;

namespace {

/// Registers `spec`; a parameter of the wrong JSON type is the caller's mistake (InvalidParams), not an internal error.
void add(command::CommandRegistry& registry, command::CommandSpec spec) {
    spec.execute = [inner = std::move(spec.execute)](ExecutionContext& c) -> CommandOutput {
        try {
            return inner(c);
        } catch (const Json::exception& e) {
            fail(CommandError::InvalidParams, std::string("a parameter has the wrong type or shape: ") + e.what());
        }
    };
    [[maybe_unused]] const auto added = registry.add(std::move(spec));
}

[[noreturn]] void failWith(const Error& e) {
    switch (e.code) {
    case ErrorCode::InvalidArgument:
        fail(CommandError::InvalidParams, e.message);
    case ErrorCode::Cancelled:
        fail(CommandError::Cancelled, e.message);
    default:
        fail(CommandError::EditRejected, e.message);
    }
}

SegmentServices& servicesOf(ExecutionContext& c) {
    auto* services = command::findService<SegmentServices>(c.services());
    if (!services) {
        fail(CommandError::InvalidRequest, "object masks need the segmentation model and the media's pictures: the host provides none "
                                           "(wabcut-cli and the app do)");
    }
    return *services;
}

analysis::AnalysisProvider* shotsOf(ExecutionContext& c) {
    auto* analysisServices = command::findService<analysis::AnalysisServices>(c.services());
    return analysisServices ? analysisServices->provider.get() : nullptr;
}

const Clip& clipOf(ExecutionContext& c, ClipId id) {
    const Sequence* seq = c.state().findSequence(c.sequenceId());
    const auto loc = seq ? seq->findClip(id) : std::nullopt;
    if (!loc) {
        fail(CommandError::NotFound, "clip " + std::to_string(id.value) + " does not exist");
    }
    return loc->clip();
}

double aspectOf(const MediaItem& media, int stream) {
    const VideoStreamInfo& v = media.info.video[static_cast<std::size_t>(stream)];
    const double par = v.sampleAspect.isValid() ? v.sampleAspect.toDouble() : 1.0;
    return v.width > 0 && v.height > 0 ? v.width * par / v.height : 16.0 / 9.0;
}

/// What a clip's object mask is made from: its media's video stream, or (0.2) what a nested or multicam clip shows (the
/// nested sequence's canvas, a multicam source's active angle) at nested time.
struct ClipPicture {
    const MediaItem* media = nullptr; ///< null: the nested picture
    int stream = 0;
    double aspect = 16.0 / 9.0;
    SequenceId nested;
};

ClipPicture pictureOf(const ProjectState& state, const Clip& clip) {
    if (clip.stream.kind != StreamKind::Video) {
        fail(CommandError::EditRejected, "clip " + std::to_string(clip.id.value) + " is not a video clip");
    }
    // Mask curve time is media (or nested) time only for clips that play their source: under Time Remapping or a frame
    // hold it runs with the timeline (docs/masks.md), and the mattes would not line up.
    if (clip.isRemapped()) {
        fail(CommandError::EditRejected, "Turn off Time Remapping before selecting an object in this clip");
    }
    if (clip.isFreeze()) {
        fail(CommandError::EditRejected, "clip " + std::to_string(clip.id.value) + " is a frame hold: select the object on the clip it holds");
    }
    ClipPicture p;
    if (clip.isNested()) {
        const Sequence* nested = state.findSequence(clip.sequence);
        if (!nested || nested->width <= 0 || nested->height <= 0) {
            fail(CommandError::EditRejected, "clip " + std::to_string(clip.id.value) + " plays a nested sequence that no longer exists");
        }
        p.nested = clip.sequence;
        p.aspect = static_cast<double>(nested->width) / nested->height;
        return p;
    }
    const MediaItem* item = state.findMedia(clip.media);
    if (!item || clip.stream.index < 0 || clip.stream.index >= static_cast<int>(item->info.video.size())) {
        fail(CommandError::EditRejected, "clip " + std::to_string(clip.id.value) + " is not a video clip of a media item");
    }
    p.media = item;
    p.stream = clip.stream.index;
    p.aspect = aspectOf(*item, clip.stream.index);
    return p;
}

/// Runs `request` on the clip's picture.
std::expected<ObjectResult, Error> segmentPicture(SegmentServices& s, ExecutionContext& c, const Clip& clip, const ClipPicture& picture,
                                                  const ObjectRequest& request, const std::function<bool(double, const std::string&)>& checkpoint) {
    if (picture.media) {
        return segmentObject(s, shotsOf(c), *picture.media, picture.stream, request, {}, checkpoint);
    }
    return segmentNested(s, c.state(), clip, request, {}, checkpoint);
}

/// The media time of the clip's frame at "time" (default its first frame on the timeline).
Ticks mediaTimeOf(ExecutionContext& c, const Clip& clip) {
    Ticks t = clip.start();
    if (c.has("time")) {
        t = c.time("time");
        if (t < clip.start() || t >= clip.end()) {
            fail(CommandError::InvalidParams, "the time lies outside the clip", Json{{"path", "/time"}});
        }
    }
    return clip.sourceTimeAt(t);
}

/// The media span of the clip, or of In / Out ("in", "out": timeline times inside it).
TimeRange spanOf(ExecutionContext& c, const Clip& clip) {
    Ticks in = clip.start(), out = clip.end();
    if (c.has("in")) {
        in = std::clamp(c.time("in"), clip.start(), clip.end());
    }
    if (c.has("out")) {
        out = std::clamp(c.time("out"), clip.start(), clip.end());
    }
    if (out <= in) {
        fail(CommandError::InvalidParams, "Out must come after In, inside the clip", Json{{"path", "/out"}});
    }
    const Ticks a = clip.sourceTimeAt(in), b = clip.sourceTimeAt(out - Ticks{1});
    return TimeRange::fromStartEnd(std::min(a, b), std::max(a, b) + Ticks{1});
}

/// "forward" is timeline time (towards the clip's end, as the user sees it); a reversed clip shows its media
/// backwards, so the model runs the other way through the media.
Direction directionOf(ExecutionContext& c, std::string_view key, Direction fallback, const Clip& clip) {
    Direction d = fallback;
    if (c.has(key)) {
        const auto parsed = directionFromString(c.string(key));
        if (!parsed) {
            fail(CommandError::InvalidParams, std::string(key) + " is none, forward, backward or both", Json{{"path", "/" + std::string(key)}});
        }
        d = *parsed;
    }
    if (clip.reversed && (d == Direction::Forward || d == Direction::Backward)) {
        return d == Direction::Forward ? Direction::Backward : Direction::Forward;
    }
    return d;
}

double unitNumber(const Json& v, const std::string& path) {
    if (!v.is_number()) {
        fail(CommandError::InvalidParams, "must be a number", Json{{"path", path}});
    }
    const double x = v.get<double>();
    if (!std::isfinite(x) || x < 0.0 || x > 1.0) {
        fail(CommandError::InvalidParams, "must lie inside the frame: 0..1", Json{{"path", path}});
    }
    return x;
}

/// The clicks ("points") and box ("box") of the request, at media time `at`.
Prompt promptOf(ExecutionContext& c, Ticks at) {
    Prompt p;
    p.time = at;
    const Json& params = c.params();
    if (params.contains("points")) {
        const Json& list = params["points"];
        if (!list.is_array() || list.size() > kMaxPromptPoints) {
            fail(CommandError::InvalidParams, "points is a list of at most " + std::to_string(kMaxPromptPoints) + " clicks", Json{{"path", "/points"}});
        }
        for (std::size_t i = 0; i < list.size(); ++i) {
            const std::string path = "/points/" + std::to_string(i);
            if (!list[i].is_object() || !list[i].contains("x") || !list[i].contains("y")) {
                fail(CommandError::InvalidParams, "a click is {\"x\", \"y\", \"positive\"}", Json{{"path", path}});
            }
            PromptPoint point;
            point.x = unitNumber(list[i]["x"], path + "/x");
            point.y = unitNumber(list[i]["y"], path + "/y");
            point.positive = list[i].value("positive", true);
            p.points.push_back(point);
        }
    }
    if (params.contains("box")) {
        const Json& b = params["box"];
        if (!b.is_object()) {
            fail(CommandError::InvalidParams, "box is {\"x\", \"y\", \"width\", \"height\"}", Json{{"path", "/box"}});
        }
        MatteBox box;
        box.x = unitNumber(b.value("x", Json(-1)), "/box/x");
        box.y = unitNumber(b.value("y", Json(-1)), "/box/y");
        box.width = std::min(unitNumber(b.value("width", Json(-1)), "/box/width"), 1.0 - box.x);
        box.height = std::min(unitNumber(b.value("height", Json(-1)), "/box/height"), 1.0 - box.y);
        if (box.width <= 0.0 || box.height <= 0.0) {
            fail(CommandError::InvalidParams, "the box has no area", Json{{"path", "/box"}});
        }
        p.box = box;
    }
    return p;
}

std::function<bool(double, const std::string&)> checkpointOf(ExecutionContext& c) {
    return [&c](double, const std::string& message) {
        c.checkpoint(message);
        return true;
    };
}

std::uint32_t maskIdParam(ExecutionContext& c) {
    const std::int64_t id = c.integer("mask", 0);
    if (id <= 0 || id > 0xFFFFFFFFll) {
        fail(CommandError::InvalidParams, "\"mask\" must be a mask id", Json{{"path", "/mask"}});
    }
    return static_cast<std::uint32_t>(id);
}

EffectInstanceId effectParamOf(ExecutionContext& c) { return EffectInstanceId{static_cast<std::uint64_t>(c.integer("effect", 0))}; }

/// The effect holding mask `id`: `effect` when given, else the clip's one effect with such a mask.
const EffectInstance& effectWithMask(const Clip& clip, EffectInstanceId effect, std::uint32_t id) {
    const EffectInstance* found = nullptr;
    for (const EffectInstance& e : clip.effects()) {
        if ((!effect.isValid() || e.id == effect) && std::ranges::any_of(e.masks, [&](const Mask& m) { return m.id == id; })) {
            if (found) {
                fail(CommandError::InvalidParams, "clip " + std::to_string(clip.id.value) + " has a mask " + std::to_string(id) +
                                                      " on several effects: name the \"effect\"",
                     Json{{"path", "/effect"}});
            }
            found = &e;
        }
    }
    if (!found) {
        fail(CommandError::NotFound, "clip " + std::to_string(clip.id.value) + " has no mask " + std::to_string(id));
    }
    return *found;
}

const Mask& maskIn(const EffectInstance& e, std::uint32_t id) {
    const auto it = std::ranges::find_if(e.masks, [&](const Mask& m) { return m.id == id; });
    if (it == e.masks.end()) {
        fail(CommandError::NotFound, "no mask " + std::to_string(id));
    }
    return *it;
}

/// An object mask's track, from the analysis cache.
std::shared_ptr<const MatteTrack> baseTrackOf(SegmentServices& s, const Mask& m) {
    if (m.shape != MaskShape::Matte || !m.matte) {
        fail(CommandError::InvalidParams, "mask " + std::to_string(m.id) + " is not an object mask (mask.objectSelect makes one)",
             Json{{"path", "/mask"}});
    }
    auto got = MatteStore(s.cacheRoot).get(m.matte->track);
    if (!got || !*got) {
        fail(CommandError::EditRejected, "this computer has no mattes of mask " + std::to_string(m.id) +
                                             " (made elsewhere, or the cache was cleared): select the object again (mask.objectSelect)");
    }
    return std::make_shared<const MatteTrack>(std::move(**got));
}

std::string capitalised(std::string_view s) {
    std::string out(s);
    if (!out.empty()) {
        out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    }
    return out;
}

/// What a run did, for the caller (and the UI's confidence strip).
Json runJson(ExecutionContext& c, const Clip& clip, const ObjectResult& r) {
    const MatteTrack& t = *r.track;
    float lowest = 1.0f;
    double sum = 0.0;
    for (const MatteFrame& f : t.frames) {
        lowest = std::min(lowest, f.confidence);
        sum += f.confidence;
    }
    // At most 120 values: each the lowest confidence of its share of the frames.
    Json strip = Json::array();
    const std::size_t n = t.frames.size();
    const std::size_t buckets = std::min<std::size_t>(n, 120);
    for (std::size_t b = 0; b < buckets; ++b) {
        float v = 1.0f;
        for (std::size_t i = b * n / buckets; i < (b + 1) * n / buckets; ++i) {
            v = std::min(v, t.frames[i].confidence);
        }
        strip.push_back(std::round(v * 100.0f) / 100.0f);
    }
    const auto cut = [&](const std::optional<Ticks>& media) -> Json {
        if (!media) {
            return nullptr;
        }
        const auto at = analysis::timelineTimeOfSource(clip, *media);
        return at ? Json(c.timecode(*at)) : Json(nullptr);
    };
    return Json{{"track", t.identity()},
                {"frames", n},
                {"lost", r.lost},
                {"cached", r.cached},
                {"segmented", r.made},
                {"seconds", std::round(r.seconds * 100.0) / 100.0},
                {"framesPerSecond", std::round(r.framesPerSecond * 10.0) / 10.0},
                {"device", r.device},
                {"stoppedAtCutBefore", cut(r.stoppedBefore)},
                {"stoppedAtCutAfter", cut(r.stoppedAfter)},
                {"confidence", Json{{"lowest", n ? std::round(lowest * 100.0f) / 100.0f : 0.0f},
                                    {"mean", n ? std::round(sum / static_cast<double>(n) * 100.0) / 100.0 : 0.0},
                                    {"strip", std::move(strip)}}}};
}

// ---- Commands -------------------------------------------------------------------------------------------------------

CommandOutput objectSelect(ExecutionContext& c) {
    SegmentServices& s = servicesOf(c);
    const ClipId id = c.clip("clip");
    const Clip& clip = clipOf(c, id);
    const ClipPicture picture = pictureOf(c.state(), clip);
    const Ticks at = mediaTimeOf(c, clip);
    Prompt prompt = promptOf(c, at);
    std::string name = c.string("name");
    if (c.has("subject")) {
        const std::string subject = c.string("subject");
        if (!prompt.points.empty() || prompt.box) {
            fail(CommandError::InvalidParams, "give a subject, or clicks and a box, not both", Json{{"path", "/subject"}});
        }
        if (!s.detector) {
            fail(CommandError::EditRejected, "no subject detector is installed here (Settings > AI > Local models): click on the object instead");
        }
        if (picture.media ? !s.frames : !s.nestedFrames) {
            fail(CommandError::InvalidRequest, "the host decodes no pictures");
        }
        auto frames = picture.media ? s.frames(*picture.media, picture.stream) : s.nestedFrames(c.state(), clip);
        if (!frames) {
            failWith(frames.error());
        }
        auto frame = (*frames)->pictureAt(at, 1024);
        if (!frame) {
            failWith(frame.error());
        }
        auto box = findSubject(*s.detector, *frame, picture.aspect, subject, c.string("colour"), c.string("pick", "largest"));
        if (!box) {
            failWith(box.error());
        }
        prompt.box = *box;
        if (name.empty()) {
            name = capitalised(c.has("colour") ? c.string("colour") + " " + subject : subject);
        }
    } else if (prompt.points.empty() && !prompt.box) {
        fail(CommandError::InvalidParams, "click on the object (\"points\"), draw a box around it (\"box\"), or name it (\"subject\")");
    }
    ObjectRequest request;
    request.prompts = {prompt};
    request.direction = directionOf(c, "propagate", Direction::Both, clip);
    request.span = spanOf(c, clip);
    request.acrossShots = c.boolean("acrossShots", false);
    auto result = segmentPicture(s, c, clip, picture, request, checkpointOf(c));
    if (!result) {
        failWith(result.error());
    }
    Mask m = objectMask(*result->track, picture.media ? picture.media->id : MediaId{}, name.empty() ? "Object" : name,
                        c.has("feather") ? c.number("feather") : kDefaultMatteFeather, c.has("expansion") ? c.number("expansion") : 0.0,
                        picture.nested);
    m.inverted = c.boolean("inverted", false);
    if (c.has("mode")) {
        const auto mode = maskModeFromString(c.string("mode"));
        if (!mode) {
            fail(CommandError::InvalidParams, "mode is add, subtract or intersect", Json{{"path", "/mode"}});
        }
        m.mode = *mode;
    }
    const EffectInstanceId effect = effectParamOf(c);
    auto added = c.take(edit::addMask(c.state(), c.sequenceId(), id, effect, std::move(m)));
    EffectInstanceId holder = effect;
    if (!holder.isValid()) {
        const Sequence* seq = added.state.findSequence(c.sequenceId());
        if (const auto loc = seq ? seq->findClip(id) : std::nullopt) {
            for (const EffectInstance& e : loc->clip().effects()) {
                if (e.effect == "wabcut.mask") {
                    holder = e.id;
                }
            }
        }
    }
    CommandOutput out;
    out.data = runJson(c, clip, *result);
    out.data["clip"] = id.value;
    out.data["effect"] = holder.value;
    out.data["mask"] = added.id;
    out.data["name"] = name.empty() ? "Object" : name;
    if (prompt.box) {
        out.data["box"] = Json{{"x", prompt.box->x}, {"y", prompt.box->y}, {"width", prompt.box->width}, {"height", prompt.box->height}};
    }
    out.createdIds.push_back(added.id);
    out.state = std::move(added.state);
    return out;
}

/// mask.refine and mask.propagate: re-run on an object mask's track and point the mask at the result.
CommandOutput rerun(ExecutionContext& c, bool refine) {
    SegmentServices& s = servicesOf(c);
    const ClipId id = c.clip("clip");
    const Clip& clip = clipOf(c, id);
    const ClipPicture picture = pictureOf(c.state(), clip);
    const std::uint32_t maskId = maskIdParam(c);
    const EffectInstance& holder = effectWithMask(clip, effectParamOf(c), maskId);
    Mask m = maskIn(holder, maskId);
    const std::shared_ptr<const MatteTrack> base = baseTrackOf(s, m);
    // A nested picture's mask follows its nest (after an angle switch it re-tracks the angle shown now); a media
    // mask its media stream.
    const bool sameSource = picture.media ? m.matte->media == picture.media->id && m.matte->stream == clip.stream.index &&
                                                !m.matte->sequence.isValid()
                                          : m.matte->sequence == picture.nested;
    if (!sameSource) {
        fail(CommandError::EditRejected, "mask " + std::to_string(maskId) + " was made from other media: select the object again");
    }
    ObjectRequest request;
    request.base = base;
    request.prompts = base->prompts;
    request.acrossShots = c.boolean("acrossShots", false);
    if (refine) {
        const Ticks at = mediaTimeOf(c, clip);
        Prompt more = promptOf(c, at);
        if (more.points.empty() && !more.box) {
            fail(CommandError::InvalidParams, "click on the object (\"points\") or draw a box around it (\"box\")");
        }
        const Ticks half{std::max<std::int64_t>(1, base->frameDuration.value / 2)};
        const auto same = std::ranges::find_if(request.prompts, [&](const Prompt& p) { return std::abs((p.time - at).value) < half.value; });
        const bool prompted = same != request.prompts.end();
        if (prompted) {
            Prompt merged = *same;
            merged.points.insert(merged.points.end(), more.points.begin(), more.points.end());
            if (merged.points.size() > kMaxPromptPoints) {
                fail(CommandError::InvalidParams, "too many clicks on one frame", Json{{"path", "/points"}});
            }
            if (more.box) {
                merged.box = more.box;
            }
            request.prompts.erase(same);
            request.prompts.push_back(std::move(merged));
        } else {
            if (request.prompts.size() >= kMaxPrompts) {
                fail(CommandError::InvalidParams, "too many prompts on one object: select it again");
            }
            request.prompts.push_back(std::move(more));
        }
        // A correction re-tracks the frames the mask already covers: from a prompted frame both ways, else onwards.
        request.direction = directionOf(c, "propagate", prompted ? Direction::Both : (clip.reversed ? Direction::Backward : Direction::Forward), clip);
        const TimeRange covered = base->span();
        const TimeRange clipSpan = spanOf(c, clip);
        const Ticks a = std::max(covered.start, clipSpan.start), b = std::min(covered.end(), clipSpan.end());
        request.span = b > a ? TimeRange::fromStartEnd(a, b) : clipSpan;
        if (!(at >= request.span->start && at < request.span->end())) {
            request.span = clipSpan; // a frame the mask did not cover yet
        }
    } else {
        request.direction = directionOf(c, "direction", Direction::Both, clip);
        request.span = spanOf(c, clip);
    }
    auto result = segmentPicture(s, c, clip, picture, request, checkpointOf(c));
    if (!result) {
        failWith(result.error());
    }
    setMatteTrack(m, *result->track, picture.media ? picture.media->id : MediaId{}, picture.nested);
    const EffectInstanceId effect = holder.id;
    CommandOutput out;
    out.data = runJson(c, clip, *result);
    out.data["clip"] = id.value;
    out.data["effect"] = effect.value;
    out.data["mask"] = maskId;
    out.data["prompts"] = result->track->prompts.size();
    out.state = c.take(edit::updateMask(c.state(), c.sequenceId(), id, effect, std::move(m)));
    return out;
}

CommandOutput maskRefine(ExecutionContext& c) { return rerun(c, true); }
CommandOutput maskPropagate(ExecutionContext& c) { return rerun(c, false); }

// ---- Schemas --------------------------------------------------------------------------------------------------------

Json timeParam(std::string description) {
    return Json{{"type", Json::array({"integer", "string"})},
                {"description", description + " Timecode in the sequence's rate (\"00:01:02:03\") or ticks."}};
}

Json clipParam() {
    return Json{{"type", Json::array({"integer", "object"})},
                {"description", "A clip id, or {\"track\", \"time\"} for the clip there."},
                {"properties", Json{{"track", Json{{"type", Json::array({"string", "integer"})}, {"description", "\"V1\" or a track id."}, {"minimum", 1}}},
                                    {"time", timeParam("A time inside the clip.")}}},
                {"required", Json::array({"track", "time"})},
                {"additionalProperties", false},
                {"minimum", 1}};
}

Json promptParams(Json params) {
    auto& p = params["properties"];
    Json point = schema::object({{"x", schema::number("0..1 of the frame width.", 0.0, 1.0)},
                                 {"y", schema::number("0..1 of the frame height.", 0.0, 1.0)},
                                 {"positive", schema::boolean("true: part of the object (default); false: not part of it.", true)}},
                                {"x", "y"}, "A click on the frame.");
    p["points"] = schema::array(std::move(point), "Clicks on the object (positive) and off it (negative), normalised frame coordinates.");
    p["points"]["maxItems"] = static_cast<std::int64_t>(kMaxPromptPoints);
    p["box"] = schema::object({{"x", schema::number("Left, 0..1.", 0.0, 1.0)},
                               {"y", schema::number("Top, 0..1.", 0.0, 1.0)},
                               {"width", schema::number("0..1 of the frame width.", 0.0, 1.0)},
                               {"height", schema::number("0..1 of the frame height.", 0.0, 1.0)}},
                              {"x", "y", "width", "height"}, "A box around the object, normalised frame coordinates.");
    p["time"] = timeParam("The frame the clicks or the box are on, inside the clip. Default the clip's first frame.");
    return params;
}

Json withCommon(std::initializer_list<std::pair<const char*, Json>> properties, std::vector<std::string> required) {
    Json s = schema::object(properties, std::move(required));
    auto& p = s["properties"];
    p["sequence"] = schema::integer("Sequence id; only needed when the project has several.", 1);
    p["clip"] = clipParam();
    p["acrossShots"] = schema::boolean("Keep tracking across shot cuts (default false: stop at the cuts around the frame).", false);
    return s;
}

} // namespace

void registerObjectMaskCommands(command::CommandRegistry& registry) {
    std::vector<std::string> colours, picks{"largest", "center", "left", "right"};
    for (const char* name : {"red", "orange", "yellow", "green", "cyan", "blue", "purple", "pink", "white", "black", "grey"}) {
        colours.emplace_back(name);
    }
    Json select = promptParams(withCommon(
        {{"subject", Json{{"type", "string"},
                          {"description", "Instead of clicks: what to find on the frame, \"face\" or a COCO class (\"person\", \"car\", \"dog\", "
                                          "\"bicycle\", ...)."},
                          {"minLength", 2},
                          {"maxLength", 40}}},
         {"colour", schema::enumeration(colours, "With \"subject\": the one of this colour (\"the red car\").")},
         {"pick", schema::enumeration(picks, "With \"subject\": which one when there are several (default the largest).")},
         {"effect", schema::integer("The effect the mask limits (clip.details lists them): blur the background (with \"inverted\"), grade "
                                    "only the subject. Omit for the clip's opacity mask: a cut-out that removes the background.",
                                    1)},
         {"name", Json{{"type", "string"}, {"description", "The mask's name. Default the subject's, or \"Object\"."}, {"maxLength", 128}}},
         {"propagate", schema::enumeration({"none", "forward", "backward", "both"},
                                           "Track the object through the clip from the frame: default both ways (a job: progress, cancel); "
                                           "none = this frame only (refine first, then mask.propagate).")},
         {"in", timeParam("Track no earlier than this (inside the clip).")},
         {"out", timeParam("Track no later than this (inside the clip).")},
         {"feather", schema::number("Soft edge, in frame heights (default 0.004).", 0.0, 0.5)},
         {"expansion", schema::number("Grow (+) or choke (-) the matte, in frame heights (default 0).", -0.5, 0.5)},
         {"inverted", schema::boolean("Everything but the object (default false).", false)},
         {"mode", schema::enumeration({"add", "subtract", "intersect"}, "How it combines with the masks before it (default add).")}},
        {"clip"}));
    add(registry, // C.12: SAM 2.1 runs on this computer (CommandSpec::runsModel)
        command::runsModel({"mask.objectSelect", 1, "Select Object",
         "AI Object Mask: segments the person or object you click on (positive and negative clicks), draw a box around, or name "
         "(\"subject\": a face, a person, a car...) on a frame of a clip with SAM 2.1 (local, on this computer), and tracks it through "
         "the shot. The result is a mask that follows it, on an effect of the clip (\"blur the background behind the speaker\": a Gaussian "
         "Blur with an inverted object mask; \"brighten the red car\": a grade limited to it) or on the clip's opacity (remove the "
         "background). Stops at shot cuts unless acrossShots. On a nested or multicam clip it segments what the clip shows (the nest's "
         "picture, the active angle) and masks the clip itself; nothing is flattened. One undo step; dryRun previews it.",
         command::Capability::EditSequence, std::move(select),
         Json{{"clip", 42}, {"time", "00:00:05:00"}, {"points", Json::array({Json{{"x", 0.52}, {"y", 0.40}, {"positive", true}}})}}, {}, objectSelect}));

    Json refineParams = promptParams(withCommon(
        {{"mask", schema::integer("The object mask (mask.objectSelect's \"mask\").", 1)},
         {"effect", schema::integer("The effect holding the mask; default the one that has it.", 1)},
         {"propagate", schema::enumeration({"none", "forward", "backward", "both"},
                                           "Re-track from the frame over the frames the mask covers: default both ways on a frame that was "
                                           "clicked before, else forward.")},
         {"in", timeParam("Re-track no earlier than this.")},
         {"out", timeParam("Re-track no later than this.")}},
        {"clip", "mask"}));
    add(registry,
        command::runsModel({"mask.refine", 1, "Refine Object Mask",
         "More clicks (positive: part of the object; negative: not) or a new box on a frame of an object mask: segments it again there and "
         "re-tracks from that frame. One undo step.",
         command::Capability::EditSequence, std::move(refineParams),
         Json{{"clip", 42}, {"mask", 1}, {"time", "00:00:07:12"}, {"points", Json::array({Json{{"x", 0.61}, {"y", 0.55}, {"positive", false}}})}}, {},
         maskRefine}));

    Json propagateParams = withCommon(
        {{"mask", schema::integer("The object mask.", 1)},
         {"effect", schema::integer("The effect holding the mask; default the one that has it.", 1)},
         {"direction", schema::enumeration({"none", "forward", "backward", "both"}, "Default both.")},
         {"in", timeParam("Track no earlier than this (inside the clip).")},
         {"out", timeParam("Track no later than this.")}},
        {"clip", "mask"});
    add(registry,
        command::runsModel({"mask.propagate", 1, "Track Object Mask",
         "Tracks an object mask's object through the clip (or In / Out) from its last clicked frame: after a selection made with "
         "\"propagate\": \"none\", or across a shot cut with acrossShots. A job with progress; one undo step.",
         command::Capability::EditSequence, std::move(propagateParams), Json{{"clip", 42}, {"mask", 1}, {"direction", "forward"}}, {}, maskPropagate}));
}

} // namespace wabcut::ai::segment
