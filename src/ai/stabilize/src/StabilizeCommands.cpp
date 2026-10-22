// effect.analyzeStabilization and effect.analyzeRollingShutter (C.15 parked, finished in P.2): Warp Stabilizer's and
// Rolling Shutter Repair's Analyze (StabilizeCommands.h).

#include <wabcut/ai/stabilize/StabilizeCommands.h>

#include <wabcut/ai/reframe/CameraPath.h>
#include <wabcut/ai/stabilize/Stabilize.h>
#include <wabcut/command/Schema.h>
#include <wabcut/masks/MaskCommands.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/MotionOps.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace wabcut::ai::stabilize {

using command::CommandOutput;
using command::ErrorCode;
using command::ExecutionContext;
using command::fail;
using command::Json;
namespace schema = command::schema;

namespace {

/// The media's content fingerprint as the analysis cache's 64-bit key, when it is known.
std::optional<std::uint64_t> fingerprintOf(const MediaItem& item) {
    const SourceFingerprint& f = item.locator.fingerprint;
    if (!f.isKnown()) {
        return std::nullopt;
    }
    std::uint64_t hash = 0;
    const auto [end, ec] = std::from_chars(f.sampleHash.data(), f.sampleHash.data() + f.sampleHash.size(), hash, 16);
    if (ec != std::errc{} || end != f.sampleHash.data() + f.sampleHash.size()) {
        return std::nullopt;
    }
    return hash ^ (static_cast<std::uint64_t>(f.size) * 0x9E3779B97F4A7C15ULL);
}

const EffectInstance* findInstance(const Clip& clip, EffectInstanceId id) {
    for (const EffectInstance& e : clip.effects()) {
        if (e.id == id) {
            return &e;
        }
    }
    return nullptr;
}

/// Premiere's Method menu: Position, Position Scale Rotation, Perspective, Subspace Warp (approximated by Perspective).
Method methodOf(double v) {
    const auto m = static_cast<int>(std::lround(v));
    return m <= 0 ? Method::Position : m == 1 ? Method::Similarity : Method::Perspective;
}

Framing framingOf(double v) {
    switch (static_cast<int>(std::lround(v))) {
    case 0:
        return Framing::StabilizeOnly;
    case 1:
        return Framing::Crop;
    case 3:
        return Framing::SynthesizeEdges;
    default:
        return Framing::CropAutoScale;
    }
}

/// Sparse keys of one channel: Bezier keys within `tolerance` of every analysed frame.
ParamValue keysOf(std::span<const Ticks> times, std::span<const double> values, double tolerance) {
    if (times.empty()) {
        return ParamValue(0.0);
    }
    Curve curve = reframe::fitCurve(times, values, tolerance);
    if (curve.keys.size() <= 1) {
        return ParamValue(curve.keys.empty() ? values.front() : curve.keys.front().value);
    }
    return ParamValue(std::move(curve));
}

/// What an Analyze command works on: the clip, its media's video stream, the frames of the media it shows.
struct Target {
    ClipId clip;
    const MediaItem* item = nullptr;
    int streamIndex = 0;
    const VideoStreamInfo* stream = nullptr;
    Rate rate;
    std::int64_t first = 0;
    std::int64_t last = 0;
    std::vector<Ticks> times;
};

Target targetOf(ExecutionContext& c, std::string_view what) {
    Target t;
    t.clip = c.clip("clip");
    const Sequence& seq = c.sequence();
    const auto location = seq.findClip(t.clip);
    if (!location) {
        fail(ErrorCode::NotFound, "the clip is not in this sequence");
    }
    const Clip& clip = location->clip();
    if (clip.stream.kind != StreamKind::Video) {
        fail(ErrorCode::InvalidParams, "only video clips have " + std::string(what));
    }
    if (clip.isNested() || clip.hasUnknownSource()) {
        fail(ErrorCode::EditRejected, "a nested or multicam clip cannot be analysed: analyse the clips inside it");
    }
    t.item = c.state().findMedia(clip.media);
    if (!t.item || t.item->isGenerator() || t.item->info.isStillImage || clip.stream.index < 0 ||
        static_cast<std::size_t>(clip.stream.index) >= t.item->info.video.size()) {
        fail(ErrorCode::EditRejected, "the clip shows no camera footage to analyse");
    }
    if (clip.isRemapped()) {
        fail(ErrorCode::EditRejected, "analysing a time-remapped clip is not supported: analyse it, then remap");
    }
    if (clip.isFreeze()) {
        fail(ErrorCode::EditRejected, "a freeze frame does not move");
    }
    t.streamIndex = clip.stream.index;
    t.stream = &t.item->info.video[static_cast<std::size_t>(clip.stream.index)];
    t.rate = t.stream->frameRate.isValid() ? t.stream->frameRate : seq.frameRate;
    // Every frame of the media the clip shows.
    t.first = ticksToFrames(clip.sourceIn, t.rate);
    t.last = std::max(t.first, ticksToFrames(clip.sourceOut() - Ticks{1}, t.rate));
    if (t.last - t.first + 1 > static_cast<std::int64_t>(kMaxAnalysisFrames)) {
        fail(ErrorCode::EditRejected, "the clip is too long to analyse in one analysis");
    }
    for (std::int64_t f = t.first; f <= t.last; ++f) {
        t.times.push_back(framesToTicks(f, t.rate));
    }
    return t;
}

/// The instance of `effectId` to key: the one named ("effect"), else the clip's first, else a new one at the top of the
/// stack (added to `state`).
EffectInstanceId instanceOf(ExecutionContext& c, ProjectState& state, const Target& t, std::string_view effectId) {
    const Clip& clip = c.sequence().findClip(t.clip)->clip();
    if (c.has("effect")) {
        const EffectInstanceId id{static_cast<std::uint64_t>(c.integer("effect"))};
        const EffectInstance* e = findInstance(clip, id);
        if (!e || e->effect != effectId) {
            fail(ErrorCode::NotFound, "the clip has no " + std::string(effectId) + " with that id");
        }
        return id;
    }
    const auto it = std::find_if(clip.effects().begin(), clip.effects().end(),
                                 [&](const EffectInstance& e) { return e.effect == effectId; });
    if (it != clip.effects().end()) {
        return it->id;
    }
    const ClipId clipId = t.clip;
    edit::EditResult added = c.take(edit::addEffect(state, c.sequenceId(), std::span(&clipId, 1), std::string(effectId), {},
                                                    std::size_t{0}));
    state = std::move(added.state);
    return added.createdEffects.front();
}

/// The raw motion of the target's media: from the cache, else tracked now (`cached` says which).
MotionAnalysis motionOf(ExecutionContext& c, const Target& t, Method method, bool detailed, bool& cached) {
    auto* tracking = command::findService<masks::TrackingServices>(c.services());
    if (!tracking) {
        fail(ErrorCode::InvalidRequest, "the analysis needs the media's frames, which this host does not provide (the app "
                                        "analyses media)");
    }
    auto* services = command::findService<StabilizationServices>(c.services());
    AnalysisCache* cache = services ? services->cache() : nullptr;
    const auto fingerprint = fingerprintOf(*t.item);
    std::optional<AnalysisKey> key;
    if (cache && fingerprint) {
        key = AnalysisKey{*fingerprint, t.streamIndex, std::string(MotionAnalysis::kAnalyzer),
                          "v" + std::to_string(MotionAnalysis::kVersion),
                          hashParams({{"method", std::to_string(static_cast<int>(method))},
                                      {"detailed", detailed ? "1" : "0"},
                                      {"width", std::to_string(kAnalysisWidth)},
                                      {"first", std::to_string(t.first)},
                                      {"last", std::to_string(t.last)},
                                      {"rate", std::to_string(t.rate.num) + "/" + std::to_string(t.rate.den)}})};
    }
    cached = false;
    if (key) {
        if (auto hit = cache->get(*key); hit && *hit) {
            if (auto parsed = motionAnalysisFromJson(**hit); parsed && parsed->frames.size() == t.times.size()) {
                cached = true;
                return std::move(*parsed);
            }
        }
    }
    const MediaId media = t.item->id;
    const int streamIndex = t.streamIndex;
    const FrameReader read = [&](Ticks time) -> std::expected<masks::GrayImage, Error> {
        auto frame = tracking->frame(c.state(), media, streamIndex, time, kAnalysisWidth);
        if (!frame) {
            return std::unexpected(frame.error());
        }
        return std::move(frame->luma);
    };
    AnalyzeOptions options;
    options.method = method;
    options.detailed = detailed;
    const Progress progress = [&c, &t, tracking](double fraction) {
        const auto total = static_cast<int>(t.times.size());
        masks::TrackProgress p;
        p.total = total;
        p.frame = std::clamp(static_cast<int>(std::lround(fraction * total)), 0, total);
        p.time = t.times[static_cast<std::size_t>(std::clamp(p.frame - 1, 0, total - 1))];
        tracking->progress(p); // the app's progress bar
        char text[96];
        std::snprintf(text, sizeof text, "analyzing frame %d of %d", p.frame, total);
        c.checkpoint(text); // throws when the caller stops the request
        return true;
    };
    auto result = analyzeMotion(t.times, read, options, progress);
    if (!result) {
        if (result.error().code == wabcut::ErrorCode::Cancelled) {
            c.checkpoint("motion analysis"); // report the caller's stop as such
        }
        fail(ErrorCode::EditRejected, "the motion analysis failed: " + result.error().message);
    }
    if (key && !c.dryRun()) {
        (void)cache->put(*key, toJson(*result)); // a cache that cannot be written only costs a re-analysis
    }
    return std::move(*result);
}

void sortParams(edit::EffectChange& change) {
    std::sort(change.params.begin(), change.params.end(), [](const EffectParam& a, const EffectParam& b) { return a.key < b.key; });
    std::sort(change.data.begin(), change.data.end(), [](const EffectData& a, const EffectData& b) { return a.key < b.key; });
}

CommandOutput analyzeStabilization(ExecutionContext& c) {
    const Target t = targetOf(c, "a Warp Stabilizer");
    ProjectState state = c.state();
    const EffectInstanceId instance = instanceOf(c, state, t, kWarpStabilizerEffect);
    const Clip& current = state.findSequence(c.sequenceId())->findClip(t.clip)->clip();
    const EffectInstance& effect = *findInstance(current, instance);
    const auto setting = [&](std::string_view key, double fallback) { return effectParameterAt(effect, key, Ticks{}, fallback); };
    const Method method = methodOf(setting("method", 3.0)); // Premiere's default: Subspace Warp
    const bool detailed = setting("detailedAnalysis", 0.0) >= 0.5;
    SmoothOptions smooth;
    smooth.smoothness = std::clamp(setting("smoothness", 50.0), 0.0, 1000.0) / 100.0;
    smooth.noMotion = setting("result", 0.0) >= 0.5;
    smooth.framing = framingOf(setting("framing", 2.0));
    smooth.maxScale = setting("maxScale", 1.5);
    smooth.additionalScale = setting("additionalScale", 1.0);
    smooth.preserveScale = setting("preserveScale", 0.0) >= 0.5;
    smooth.actionSafeMargin = std::clamp(setting("actionSafeMargin", 0.0), 0.0, 49.0) / 100.0;
    smooth.cropLessSmoothMore = std::clamp(setting("cropLessSmoothMore", 50.0), 0.0, 100.0) / 100.0;
    smooth.rippleReduction = setting("rollingShutter", 0.0) >= 0.5 ? RippleReduction::Enhanced : RippleReduction::Automatic;
    smooth.frameRate = static_cast<double>(t.rate.num) / static_cast<double>(t.rate.den);

    bool cached = false;
    const MotionAnalysis analysis = motionOf(c, t, method, detailed, cached);

    // The compensation, keyed per analysed frame (sparse within a tolerance), plus the framing results.
    const Compensation comp = stabilize(analysis, smooth, t.stream->width, t.stream->height);
    const std::size_t n = std::min(comp.times.size(), t.times.size());
    const std::span<const Ticks> keyTimes(comp.times.data(), n);
    edit::EffectChange change;
    const auto channel = [&](const char* name, const std::vector<double>& values, double tolerance, double fallback) {
        if (values.size() >= n && n > 0) {
            change.params.push_back(EffectParam{name, keysOf(keyTimes, std::span(values.data(), n), tolerance)});
        } else {
            change.params.push_back(EffectParam{name, ParamValue(fallback)});
        }
    };
    static constexpr const char* kCorners[8] = {"corner.tl.x", "corner.tl.y", "corner.tr.x", "corner.tr.y",
                                                "corner.br.x", "corner.br.y", "corner.bl.x", "corner.bl.y"};
    if (method == Method::Perspective) {
        for (std::size_t k = 0; k < 8; ++k) {
            std::vector<double> values;
            for (std::size_t i = 0; i < comp.corners.size(); ++i) {
                values.push_back(comp.corners[i][k]);
            }
            channel(kCorners[k], values, 0.05, 0.0);
        }
        for (const char* name : {"transform.x", "transform.y", "transform.rotation"}) {
            change.params.push_back(EffectParam{name, ParamValue(0.0)});
        }
        change.params.push_back(EffectParam{"transform.scale", ParamValue(1.0)});
    } else {
        channel("transform.x", comp.x, 0.05, 0.0);
        channel("transform.y", comp.y, 0.05, 0.0);
        channel("transform.rotation", comp.rotation, 0.01, 0.0);
        channel("transform.scale", comp.scale, 1e-4, 1.0);
        for (const char* name : kCorners) {
            change.params.push_back(EffectParam{name, ParamValue(0.0)});
        }
    }
    change.params.push_back(EffectParam{"autoScale", ParamValue(std::clamp(comp.autoScale, 1.0, 10.0))});
    change.params.push_back(EffectParam{"crop.left", ParamValue(std::clamp(comp.cropLeft, 0.0, 0.5))});
    change.params.push_back(EffectParam{"crop.top", ParamValue(std::clamp(comp.cropTop, 0.0, 0.5))});
    change.params.push_back(EffectParam{"crop.right", ParamValue(std::clamp(comp.cropRight, 0.0, 0.5))});
    change.params.push_back(EffectParam{"crop.bottom", ParamValue(std::clamp(comp.cropBottom, 0.0, 0.5))});
    int lost = 0;
    for (const FrameMotion& f : analysis.frames) {
        lost += f.lost ? 1 : 0;
    }
    char summary[256];
    std::snprintf(summary, sizeof summary,
                  "frames=%zu;lost=%d;method=%d;detailed=%d;smoothness=%.6g;result=%d;framing=%d;maxScale=%.6g;"
                  "additionalScale=%.6g;fingerprint=%s",
                  analysis.frames.size(), lost, static_cast<int>(method), detailed ? 1 : 0, smooth.smoothness * 100.0,
                  smooth.noMotion ? 1 : 0, static_cast<int>(smooth.framing), smooth.maxScale, smooth.additionalScale,
                  t.item->locator.fingerprint.sampleHash.c_str());
    change.data.push_back(EffectData{"analysis", std::string(summary)});
    change.data.push_back(
        EffectData{"status", std::string(lost == static_cast<int>(analysis.frames.size()) ? "failed" : "analyzed")});
    sortParams(change);
    edit::EditResult r = c.take(edit::setEffect(state, c.sequenceId(), t.clip, instance, change));

    CommandOutput out;
    out.state = std::move(r.state);
    out.createdIds.push_back(instance.value);
    out.data = Json{{"clip", t.clip.value},
                    {"effect", instance.value},
                    {"frames", analysis.frames.size()},
                    {"lostFrames", lost},
                    {"cached", cached},
                    {"autoScale", comp.autoScale},
                    {"scaleCapped", comp.scaleCapped},
                    {"rollingShutterShare", comp.rollingShutterShare},
                    {"crop", Json{{"left", comp.cropLeft}, {"top", comp.cropTop}, {"right", comp.cropRight}, {"bottom", comp.cropBottom}}}};
    if (comp.scaleCapped) {
        out.warnings.push_back("the motion needs more than the maximum scale: borders may show (raise Maximum Scale or "
                               "lower Smoothness)");
    }
    return out;
}

CommandOutput analyzeRollingShutter(ExecutionContext& c) {
    const Target t = targetOf(c, "Rolling Shutter Repair");
    ProjectState state = c.state();
    const EffectInstanceId instance = instanceOf(c, state, t, kRollingShutterRepairEffect);
    const Clip& current = state.findSequence(c.sequenceId())->findClip(t.clip)->clip();
    const EffectInstance& effect = *findInstance(current, instance);
    const bool detailed = effectParameterAt(effect, "detailedAnalysis", Ticks{}, 0.0) >= 0.5;
    bool cached = false;
    const MotionAnalysis analysis = motionOf(c, t, Method::Position, detailed, cached);
    const FrameVelocity v = velocities(analysis, t.stream->width, t.stream->height);
    const std::size_t n = std::min(v.times.size(), t.times.size());
    edit::EffectChange change;
    const std::span<const Ticks> keyTimes(v.times.data(), n);
    change.params.push_back(EffectParam{"motion.x", n > 0 ? keysOf(keyTimes, std::span(v.x.data(), n), 0.05) : ParamValue(0.0)});
    change.params.push_back(EffectParam{"motion.y", n > 0 ? keysOf(keyTimes, std::span(v.y.data(), n), 0.05) : ParamValue(0.0)});
    change.data.push_back(EffectData{"status", std::string("analyzed")});
    sortParams(change);
    edit::EditResult r = c.take(edit::setEffect(state, c.sequenceId(), t.clip, instance, change));
    CommandOutput out;
    out.state = std::move(r.state);
    out.createdIds.push_back(instance.value);
    out.data = Json{{"clip", t.clip.value}, {"effect", instance.value}, {"frames", analysis.frames.size()}, {"cached", cached}};
    return out;
}

Json analyzeSchema(const char* effectDescription) {
    Json s = schema::object({{"clip", schema::integer("The video clip (id).", 1)},
                             {"effect", schema::integer(effectDescription, 1)}},
                            {"clip"});
    s["properties"]["sequence"] = schema::integer("Sequence id; only needed when the project has several.", 1);
    return s;
}

} // namespace

void registerStabilizeCommands(command::CommandRegistry& registry) {
    command::CommandSpec stabilization{
        "effect.analyzeStabilization", 1, "Analyze Stabilization",
        "Warp Stabilizer's Analyze: tracks the camera motion of a video clip's footage, smooths the camera path with the "
        "effect's settings (Result, Smoothness, Method, Preserve Scale, Framing, Maximum Scale, Action-Safe Margin, "
        "Additional Scale, Detailed Analysis, Rolling Shutter Ripple, Crop Less Smooth More) and keys the compensating "
        "transform per frame into the clip's Warp Stabilizer (added at the top of its stack when it has none), with the "
        "crop or auto-scale that hides the moving borders. The keys stay editable. Changing a setting and running it "
        "again re-smooths from the cached analysis. Runs as a job with progress and cancel; a dry run analyses and "
        "previews the keys.",
        command::Capability::EditSequence,
        analyzeSchema("Its Warp Stabilizer instance (id). Default: the clip's first (or a new one)."),
        Json{{"clip", 42}}, {}, analyzeStabilization};
    [[maybe_unused]] const auto addedStabilization = registry.add(std::move(stabilization));
    command::CommandSpec shutter{
        "effect.analyzeRollingShutter", 1, "Analyze Rolling Shutter",
        "Rolling Shutter Repair's analysis: measures the picture's motion per frame of a video clip's footage and keys it "
        "(motion.x, motion.y: source pixels per frame) into the clip's Rolling Shutter Repair (added at the top of its "
        "stack when it has none), which then skews each line back by its share of the scan (Rolling Shutter Rate, Scan "
        "Direction). Shares the cached analysis with Warp Stabilizer's Position method.",
        command::Capability::EditSequence,
        analyzeSchema("Its Rolling Shutter Repair instance (id). Default: the clip's first (or a new one)."),
        Json{{"clip", 42}}, {}, analyzeRollingShutter};
    [[maybe_unused]] const auto addedShutter = registry.add(std::move(shutter));
}

} // namespace wabcut::ai::stabilize
