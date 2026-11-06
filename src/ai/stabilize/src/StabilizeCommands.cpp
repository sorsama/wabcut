// effect.analyzeStabilization and effect.analyzeRollingShutter (C.15 parked, finished in P.2): Warp Stabilizer's and
// Rolling Shutter Repair's Analyze, and 0.7's effect.analyzeAll (StabilizeCommands.h).

#include <wabcut/ai/stabilize/StabilizeCommands.h>

#include <wabcut/ai/reframe/CameraPath.h>
#include <wabcut/ai/stabilize/GyroMotion.h>
#include <wabcut/ai/stabilize/Stabilize.h>
#include <wabcut/command/Schema.h>
#include <wabcut/gyro/Gyro.h>
#include <wabcut/masks/MaskCommands.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/MasterClipEffects.h>
#include <wabcut/model/MotionOps.h>
#include <wabcut/model/Plural.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
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

/// The Method menu's value for "Gyro (camera metadata)" (0.7, card #130); the others are Premiere's.
constexpr long kGyroMethod = 4;

/// Premiere's Method menu: Position, Position Scale Rotation, Perspective, Subspace Warp (approximated by Perspective), and 0.7's
/// Gyro (camera metadata), whose correction is a corner pin as Perspective's is.
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

/// What an Analyze command works on: the clip, its media's video stream, the frames of the media it shows. Fix F3
/// (NEW-01): for an effect of the clip's media's Master Clip Effects (`master`: the item holding it), the analysis
/// belongs to the media: the frames are those every clip playing that effect shows (see masterSpan).
struct Target {
    ClipId clip;
    MediaId master;
    const MediaItem* item = nullptr;
    int streamIndex = 0;
    const VideoStreamInfo* stream = nullptr;
    Rate rate;
    std::int64_t first = 0;
    std::int64_t last = 0;
    std::vector<Ticks> times;
    // 0.7 (card #107): a time-remapped clip (speed ramps, holds, reverse) keys in curve time, which runs with the timeline
    // from its In: one entry per frame the clip shows - its curve time, and the analysed frame (a fractional index into
    // `times`) it shows then. Empty for every other clip: its curve time is the media time, so the analysed frames' own
    // times key it (at any constant speed, reversed too).
    bool remapped = false;
    std::vector<Ticks> outTimes;
    std::vector<double> outFrames;
    /// Time Interpolation mixes frames (Frame Blending, Optical Flow): the compensation is interpolated between the
    /// analysed frames. Frame Sampling shows one whole frame, which is the one its compensation belongs to.
    bool mixesFrames = false;
};

/// The effect a command names (`effect`), or the clip's first of its kind.
struct Subject {
    ClipId clip;
    std::optional<EffectInstanceId> effect;
};

/// Where an analysis reports its frames in a longer job: `offset` frames done before it, `total` frames in all.
struct ProgressWindow {
    int offset = 0;
    int total = 0;
};

/// The instance `id` of `clip`'s stack, else of its media's Master Clip Effects, in `state`.
const EffectInstance* playedInstance(const ProjectState& state, const Clip& clip, EffectInstanceId id) {
    if (const EffectInstance* own = findInstance(clip, id)) {
        return own;
    }
    const MediaItem* holder = clip.media.isValid() ? masterEffectHolder(state, clip.media, id) : nullptr;
    if (!holder) {
        return nullptr;
    }
    const auto it = std::ranges::find_if(holder->masterEffects, [&](const EffectInstance& e) { return e.id == id; });
    return it != holder->masterEffects.end() ? &*it : nullptr;
}

/// Fix F3 (NEW-01): the source frames [first, last] that the clips playing master clip effect `effect` (of `holder`)
/// show, over every sequence of the project (nested and multicam sources too): one contiguous span, so every instance
/// is keyed. Clips that turned their master clip effects off, nested clips and other streams do not count. False when
/// no clip plays it.
bool masterSpan(const ProjectState& state, MediaId holder, EffectInstanceId effect, int streamIndex, Rate rate,
                std::int64_t& first, std::int64_t& last) {
    bool any = false;
    for (const SequencePtr& seq : state.sequences) {
        for (const TrackPtr& track : seq->videoTracks) {
            for (const Clip& clip : track->clips) {
                if (clip.isNested() || clip.hasUnknownSource() || clip.stream.kind != StreamKind::Video ||
                    clip.stream.index != streamIndex || clip.content().masterEffectsOff || !clip.media.isValid()) {
                    continue;
                }
                const MediaItem* played = masterEffectHolder(state, clip.media, effect);
                if (!played || played->id != holder) {
                    continue;
                }
                const std::int64_t a = ticksToFrames(clip.sourceIn, rate);
                const std::int64_t b = std::max(a, ticksToFrames(clip.sourceOut() - Ticks{1}, rate));
                first = any ? std::min(first, a) : a;
                last = any ? std::max(last, b) : b;
                any = true;
            }
        }
    }
    return any;
}

Target targetOf(const ProjectState& state, const Sequence& seq, const Subject& subject, std::string_view what) {
    Target t;
    t.clip = subject.clip;
    const auto location = seq.findClip(t.clip);
    if (!location) {
        fail(ErrorCode::NotFound, "the clip is not in this sequence");
    }
    const Clip& clip = location->clip();
    if (subject.effect && clip.media.isValid()) {
        if (!findInstance(clip, *subject.effect)) {
            if (const MediaItem* holder = masterEffectHolder(state, clip.media, *subject.effect)) {
                t.master = holder->id;
            }
        }
    }
    if (clip.stream.kind != StreamKind::Video) {
        fail(ErrorCode::InvalidParams, "only video clips have " + std::string(what));
    }
    if (clip.isNested() || clip.hasUnknownSource()) {
        fail(ErrorCode::EditRejected, "a nested or multicam clip cannot be analysed: analyse the clips inside it");
    }
    t.item = state.findMedia(clip.media);
    if (!t.item || t.item->isGenerator() || t.item->info.isStillImage || clip.stream.index < 0 ||
        static_cast<std::size_t>(clip.stream.index) >= t.item->info.video.size()) {
        fail(ErrorCode::EditRejected, "the clip shows no camera footage to analyse");
    }
    // A master clip effect is analysed on the media, whatever this one clip does with its time.
    if (clip.isFreeze() && !t.master.isValid()) {
        fail(ErrorCode::EditRejected, "a freeze frame does not move");
    }
    t.remapped = clip.isRemapped() && !t.master.isValid(); // 0.7 (card #107): keyed in curve time below
    t.streamIndex = clip.stream.index;
    t.stream = &t.item->info.video[static_cast<std::size_t>(clip.stream.index)];
    t.rate = t.stream->frameRate.isValid() ? t.stream->frameRate : seq.frameRate;
    // Every frame of the media the clip shows (a master clip effect: that every clip playing it shows).
    if (t.remapped) {
        // Speed ramps, holds and reverse segments: sourceIn / sourceOut are curve times there, the media range is the one
        // the remap curve visits over the clip.
        const TimeRange shown = clip.shownSourceRange(clip.start(), clip.end());
        t.first = std::max<std::int64_t>(ticksToFrames(shown.start, t.rate), 0);
        t.last = std::max(t.first, ticksToFrames(shown.end() - Ticks{1}, t.rate));
    } else {
        t.first = ticksToFrames(clip.sourceIn, t.rate);
        t.last = std::max(t.first, ticksToFrames(clip.sourceOut() - Ticks{1}, t.rate));
    }
    if (t.master.isValid()) {
        std::int64_t first = 0;
        std::int64_t last = 0;
        if (masterSpan(state, t.master, *subject.effect, t.streamIndex, t.rate, first, last)) {
            t.first = std::min(t.first, first);
            t.last = std::max(t.last, last);
        }
        t.first = std::max<std::int64_t>(t.first, 0);
        t.last = std::max(t.first, t.last);
    }
    if (t.last - t.first + 1 > static_cast<std::int64_t>(kMaxAnalysisFrames)) {
        fail(ErrorCode::EditRejected, "the clip is too long to analyse in one analysis");
    }
    for (std::int64_t f = t.first; f <= t.last; ++f) {
        t.times.push_back(framesToTicks(f, t.rate));
    }
    if (t.remapped) {
        // One entry per sequence frame the clip shows: its curve time (the remap's input) and which analysed frame the
        // remap shows there (media time in frames, from the first analysed).
        t.mixesFrames = clip.timeInterpolation() != TimeInterpolation::FrameSampling;
        const Ticks frame = std::max(framesToTicks(1, seq.frameRate), Ticks{1});
        const double framesPerTick = static_cast<double>(t.rate.num) / (static_cast<double>(t.rate.den) * static_cast<double>(kTicksPerSecond));
        for (Ticks at = clip.start(); at < clip.end(); at = at + frame) {
            const double shownFrame = static_cast<double>(clip.sourceTimeAt(at).value) * framesPerTick - static_cast<double>(t.first);
            t.outTimes.push_back(clip.curveTimeAt(at));
            t.outFrames.push_back(std::clamp(shownFrame, 0.0, static_cast<double>(t.times.size() - 1)));
        }
    }
    return t;
}

/// `values` (one per analysed frame) as one per key time of the target: itself for a clip keyed in media time, else
/// interpolated at the analysed frame each output frame shows (0.7, card #107).
std::vector<double> perKey(const Target& t, std::span<const double> values) {
    if (!t.remapped) {
        return std::vector<double>(values.begin(), values.end());
    }
    std::vector<double> out;
    out.reserve(t.outFrames.size());
    for (const double position : t.outFrames) {
        if (values.empty()) {
            out.push_back(0.0);
            continue;
        }
        // 1e-6 of a frame: a time that lands on a frame's start is that frame, not the one before.
        const auto i0 = std::min(static_cast<std::size_t>(std::max(position + 1e-6, 0.0)), values.size() - 1);
        const std::size_t i1 = std::min(i0 + 1, values.size() - 1);
        const double f = t.mixesFrames ? std::clamp(position - static_cast<double>(i0), 0.0, 1.0) : 0.0;
        out.push_back(values[i0] + (values[i1] - values[i0]) * f);
    }
    return out;
}

/// The times the keys go at: the analysed frames' own, or a remapped clip's curve times.
std::vector<Ticks> keyTimesOf(const Target& t, std::span<const Ticks> analysed) {
    return t.remapped ? t.outTimes : std::vector<Ticks>(analysed.begin(), analysed.end());
}

/// The instance of `effectId` to key: the one named, else the clip's first, else a new one at the top of the stack (added
/// to `state`).
EffectInstanceId instanceOf(ExecutionContext& c, ProjectState& state, SequenceId sequence, const Subject& subject, const Target& t,
                            std::string_view effectId) {
    const Clip& clip = state.findSequence(sequence)->findClip(t.clip)->clip();
    if (subject.effect) {
        const EffectInstance* e = playedInstance(state, clip, *subject.effect); // a master clip effect too (F3)
        if (!e || e->effect != effectId) {
            fail(ErrorCode::NotFound, "the clip has no " + std::string(effectId) + " with that id");
        }
        return *subject.effect;
    }
    const auto it = std::find_if(clip.effects().begin(), clip.effects().end(),
                                 [&](const EffectInstance& e) { return e.effect == effectId; });
    if (it != clip.effects().end()) {
        return it->id;
    }
    const ClipId clipId = t.clip;
    edit::EditResult added = c.take(edit::addEffect(state, sequence, std::span(&clipId, 1), std::string(effectId), {}, std::size_t{0}));
    state = std::move(added.state);
    return added.createdEffects.front();
}

/// The raw motion of the target's media: from the cache, else tracked now (`cached` says which).
MotionAnalysis motionOf(ExecutionContext& c, const Target& t, Method method, bool detailed, bool& cached,
                        const ProgressWindow* window = nullptr) {
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
    const Progress progress = [&c, &t, tracking, window](double fraction) {
        const auto total = static_cast<int>(t.times.size());
        masks::TrackProgress p;
        p.total = total;
        p.frame = std::clamp(static_cast<int>(std::lround(fraction * total)), 0, total);
        p.time = t.times[static_cast<std::size_t>(std::clamp(p.frame - 1, 0, total - 1))];
        if (window) { // 0.7 (card #128): one bar over every effect of an Analyze All
            p.frame += window->offset;
            p.total = std::max(window->total, p.frame);
        }
        tracking->progress(p); // the app's progress bar
        char text[96];
        std::snprintf(text, sizeof text, "analyzing frame %d of %d", p.frame, p.total);
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

// ---- One analysis: the job behind the commands and Analyze All -----------------------------------------------------

/// What a Warp Stabilizer's analysis reads from the effect's settings.
struct WarpSettings {
    Method method = Method::Perspective;
    bool detailed = false;
    SmoothOptions smooth;
    /// 0.7 (card #130): Method "Gyro (camera metadata)": the camera's recorded rotation instead of the picture's motion
    /// (method is then Perspective: the correction is a corner pin).
    bool gyro = false;
    GyroOptions gyroOptions;
};

/// 0.7 (card #129): the Smoothness setting at each analysed frame (as SmoothOptions::smoothness, a fraction) when it is
/// keyframed; empty for a constant. A clip keyed in media time reads its curve at each frame's media time; a time-remapped
/// clip at the output frame that shows the analysed frame (the last one when several do; a frame no output frame shows
/// holds the one before it).
std::vector<double> smoothnessAtFrames(const EffectInstance& effect, const Target& t) {
    const ParamValue* setting = effect.find("smoothness");
    if (!setting || !setting->isCurve() || t.times.empty()) {
        return {};
    }
    const auto fraction = [](double percent) { return std::clamp(percent, 0.0, 1000.0) / 100.0; };
    const std::size_t n = t.times.size();
    std::vector<double> out(n, 0.0);
    if (!t.remapped) {
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = fraction(setting->valueAt(t.times[i]));
        }
        return out;
    }
    std::vector<char> known(n, 0);
    const std::size_t shown = std::min(t.outTimes.size(), t.outFrames.size());
    for (std::size_t j = 0; j < shown; ++j) {
        const auto i = static_cast<std::size_t>(std::clamp(std::llround(t.outFrames[j]), 0LL, static_cast<long long>(n) - 1));
        out[i] = fraction(setting->valueAt(t.outTimes[j]));
        known[i] = 1;
    }
    const auto firstKnown = std::find(known.begin(), known.end(), char{1});
    if (firstKnown == known.end()) {
        out.assign(n, fraction(setting->valueAt(Ticks{})));
        return out;
    }
    const auto first = static_cast<std::size_t>(firstKnown - known.begin());
    for (std::size_t i = 0; i < first; ++i) { // before the first frame shown: its value
        out[i] = out[first];
    }
    for (std::size_t i = first + 1; i < n; ++i) { // a frame nothing shows holds the one before it
        if (!known[i]) {
            out[i] = out[i - 1];
        }
    }
    return out;
}

WarpSettings warpSettingsOf(const EffectInstance& effect, const Target& t) {
    const auto setting = [&](std::string_view key, double fallback) { return effectParameterAt(effect, key, Ticks{}, fallback); };
    WarpSettings s;
    s.method = methodOf(setting("method", 3.0)); // Premiere's default: Subspace Warp
    s.gyro = std::lround(setting("method", 3.0)) >= kGyroMethod;
    s.gyroOptions.offsetSeconds = std::clamp(setting("gyroOffset", 0.0), -2000.0, 2000.0) / 1000.0;
    s.gyroOptions.horizontalFovDegrees = std::clamp(setting("gyroFov", 0.0), 0.0, 175.0);
    s.gyroOptions.readoutSeconds = std::clamp(setting("gyroReadout", 0.0), 0.0, 200.0) / 1000.0;
    s.gyroOptions.autoSync = setting("gyroSync", 1.0) >= 0.5;
    s.detailed = setting("detailedAnalysis", 0.0) >= 0.5;
    SmoothOptions& smooth = s.smooth;
    smooth.smoothness = std::clamp(setting("smoothness", 50.0), 0.0, 1000.0) / 100.0;
    smooth.smoothnessAt = smoothnessAtFrames(effect, t); // keyframed: the analysis follows the keys (0.7, card #129)
    smooth.noMotion = setting("result", 0.0) >= 0.5;
    smooth.framing = framingOf(setting("framing", 2.0));
    smooth.maxScale = setting("maxScale", 1.5);
    smooth.additionalScale = setting("additionalScale", 1.0);
    smooth.preserveScale = setting("preserveScale", 0.0) >= 0.5;
    smooth.actionSafeMargin = std::clamp(setting("actionSafeMargin", 0.0), 0.0, 49.0) / 100.0;
    smooth.cropLessSmoothMore = std::clamp(setting("cropLessSmoothMore", 50.0), 0.0, 100.0) / 100.0;
    smooth.rippleReduction = setting("rollingShutter", 0.0) >= 0.5 ? RippleReduction::Enhanced : RippleReduction::Automatic;
    smooth.frameRate = static_cast<double>(t.rate.num) / static_cast<double>(t.rate.den);
    return s;
}

/// The text kept in the effect's data "analysis": what the keys were made from (frames, settings, the file). Analyze All
/// compares it with what the clip and the settings are now (0.7, card #128).
std::string warpSummary(const WarpSettings& s, std::size_t frames, int lost, const Target& t) {
    // 0.7 (card #129): a keyframed Smoothness is told by a fingerprint of its values at the analysed frames (to 1e-5 of
    // a percent), so moving a key makes the analysis out of date.
    char keyed[40] = "";
    if (!s.smooth.smoothnessAt.empty()) {
        std::uint64_t hash = 1469598103934665603ULL; // FNV-1a
        for (const double v : s.smooth.smoothnessAt) {
            const auto step = static_cast<std::uint64_t>(std::llround(v * 1e5));
            for (int byte = 0; byte < 8; ++byte) {
                hash = (hash ^ ((step >> (8 * byte)) & 0xFFU)) * 1099511628211ULL;
            }
        }
        std::snprintf(keyed, sizeof keyed, "smoothnessKeys=%016llx;", static_cast<unsigned long long>(hash));
    }
    // 0.7 (card #130): the gyro method's own settings, only when it is the method (an older analysis keeps its text).
    char gyroPart[160] = "";
    if (s.gyro) {
        std::snprintf(gyroPart, sizeof gyroPart, "gyro=1;gyroOffset=%.6g;gyroFov=%.6g;gyroReadout=%.6g;gyroSync=%d;",
                      s.gyroOptions.offsetSeconds * 1000.0, s.gyroOptions.horizontalFovDegrees, s.gyroOptions.readoutSeconds * 1000.0,
                      s.gyroOptions.autoSync ? 1 : 0);
    }
    char summary[520];
    std::snprintf(summary, sizeof summary,
                  "frames=%zu;lost=%d;method=%d;detailed=%d;smoothness=%.6g;%s%sresult=%d;framing=%d;maxScale=%.6g;"
                  "additionalScale=%.6g;fingerprint=%s",
                  frames, lost, static_cast<int>(s.method), s.detailed ? 1 : 0, s.smooth.smoothness * 100.0, keyed, gyroPart,
                  s.smooth.noMotion ? 1 : 0, static_cast<int>(s.smooth.framing), s.smooth.maxScale, s.smooth.additionalScale,
                  t.item->locator.fingerprint.sampleHash.c_str());
    return summary;
}

std::string shutterSummary(bool detailed, std::size_t frames, const Target& t) {
    char summary[160];
    std::snprintf(summary, sizeof summary, "frames=%zu;lost=0;detailed=%d;fingerprint=%s", frames, detailed ? 1 : 0,
                  t.item->locator.fingerprint.sampleHash.c_str());
    return summary;
}

/// `summary` without its "lost=N;" part: frames that could not be matched are a result, not a setting.
std::string withoutLost(std::string summary) {
    const std::size_t at = summary.find("lost=");
    if (at != std::string::npos) {
        const std::size_t end = summary.find(';', at);
        summary.erase(at, end == std::string::npos ? std::string::npos : end - at + 1);
    }
    return summary;
}

/// Whether `effect` holds an analysis that matches what its clip shows and its settings now: analysed, over as many frames
/// of the same file, with the same settings. A clip trimmed longer, moved onto other footage, or a changed Smoothness
/// makes it stale (0.7, card #128: Analyze All re-does exactly those).
bool analysisCurrent(const EffectInstance& effect, const Target& t, bool warp) {
    const EffectData* status = effect.findData("status");
    if (!status || !status->text() || *status->text() != "analyzed") {
        return false;
    }
    const EffectData* summary = effect.findData("analysis");
    if (!summary || !summary->text()) {
        return true; // from before the summary was kept: nothing to compare, trust it
    }
    const std::string expected =
        warp ? warpSummary(warpSettingsOf(effect, t), t.times.size(), 0, t)
             : shutterSummary(effectParameterAt(effect, "detailedAnalysis", Ticks{}, 0.0) >= 0.5, t.times.size(), t);
    return withoutLost(*summary->text()) == withoutLost(expected);
}

// ---- 0.7 (card #130): the gyro method ---------------------------------------------------------------------------------

constexpr int kSyncWindowFrames = 48;  // consecutive frames per stretch Auto-sync measures
constexpr int kSyncWindowCount = 3;    // ... spread over the clip

/// Where the stretches Auto-sync measures start (analysed-frame indexes) and how long they are.
std::vector<std::pair<std::size_t, std::size_t>> syncWindowsOf(std::size_t frames) {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    if (frames < 8) {
        return out;
    }
    const std::size_t length = std::min<std::size_t>(frames, kSyncWindowFrames);
    const std::size_t count = frames >= 4 * length ? kSyncWindowCount : frames >= 2 * length ? 2 : 1;
    for (std::size_t i = 0; i < count; ++i) {
        const double at = count == 1 ? 0.5 : 0.1 + 0.8 * static_cast<double>(i) / static_cast<double>(count - 1);
        const auto start = static_cast<std::size_t>(at * static_cast<double>(frames - length));
        out.emplace_back(start, length);
    }
    return out;
}

/// The picture's motion on a few stretches of the clip (Similarity, cached per media like the full analysis): what Auto-sync
/// compares the gyro with. Empty when this host cannot decode frames.
std::vector<ImageMotionWindow> measureSyncWindows(ExecutionContext& c, const Target& t, const ProgressWindow* progressWindow) {
    std::vector<ImageMotionWindow> out;
    auto* tracking = command::findService<masks::TrackingServices>(c.services());
    if (!tracking) {
        return out;
    }
    const auto windows = syncWindowsOf(t.times.size());
    auto* services = command::findService<StabilizationServices>(c.services());
    AnalysisCache* cache = services ? services->cache() : nullptr;
    const auto fingerprint = fingerprintOf(*t.item);
    int total = 0;
    for (const auto& w : windows) {
        total += static_cast<int>(w.second);
    }
    int done = 0;
    const MediaId media = t.item->id;
    const int streamIndex = t.streamIndex;
    for (const auto& [start, length] : windows) {
        ImageMotionWindow window;
        window.times.assign(t.times.begin() + static_cast<std::ptrdiff_t>(start),
                            t.times.begin() + static_cast<std::ptrdiff_t>(start + length));
        std::optional<AnalysisKey> key;
        if (cache && fingerprint) {
            key = AnalysisKey{*fingerprint, streamIndex, "wabcut.stabilize.sync", "v1",
                              hashParams({{"width", std::to_string(kAnalysisWidth)},
                                          {"first", std::to_string(t.first + static_cast<std::int64_t>(start))},
                                          {"count", std::to_string(length)},
                                          {"rate", std::to_string(t.rate.num) + "/" + std::to_string(t.rate.den)}})};
            if (auto hit = cache->get(*key); hit && *hit) {
                if (auto parsed = motionAnalysisFromJson(**hit); parsed && parsed->frames.size() == length) {
                    window.motion = std::move(*parsed);
                    out.push_back(std::move(window));
                    done += static_cast<int>(length);
                    continue;
                }
            }
        }
        const FrameReader read = [&](Ticks time) -> std::expected<masks::GrayImage, Error> {
            auto frame = tracking->frame(c.state(), media, streamIndex, time, kAnalysisWidth);
            if (!frame) {
                return std::unexpected(frame.error());
            }
            return std::move(frame->luma);
        };
        AnalyzeOptions options;
        options.method = Method::Similarity;
        options.gridFeatures = 150;
        const Progress progress = [&](double fraction) {
            const auto inWindow = static_cast<long>(std::lround(fraction * static_cast<double>(length)));
            masks::TrackProgress p;
            p.total = progressWindow ? std::max(progressWindow->total, total) : total;
            p.frame = std::clamp(done + static_cast<int>(inWindow), 0, total) + (progressWindow ? progressWindow->offset : 0);
            p.time = window.times[static_cast<std::size_t>(std::clamp<long>(inWindow - 1, 0, static_cast<long>(length) - 1))];
            tracking->progress(p);
            c.checkpoint("measuring the picture to sync the gyro");
            return true;
        };
        auto result = analyzeMotion(window.times, read, options, progress);
        if (!result) {
            if (result.error().code == wabcut::ErrorCode::Cancelled) {
                c.checkpoint("motion analysis");
            }
            continue; // a stretch that cannot be read only costs Auto-sync some evidence
        }
        if (key && !c.dryRun()) {
            (void)cache->put(*key, toJson(*result));
        }
        window.motion = std::move(*result);
        out.push_back(std::move(window));
        done += static_cast<int>(length);
    }
    return out;
}

/// What the gyro method made of a clip.
struct GyroRun {
    Compensation comp;
    GyroSetup setup;
    std::string summary;       ///< for the effect's data "gyro"
    std::string camera;
    std::size_t uncovered = 0; ///< analysed frames the gyro does not reach (left as shot)
    std::vector<std::string> warnings;
};

GyroRun runGyro(ExecutionContext& c, const Target& t, const WarpSettings& settings, const ProgressWindow* progressWindow) {
    auto* services = command::findService<gyro::GyroServices>(c.services());
    if (!services) {
        fail(ErrorCode::InvalidRequest, "the gyro method needs the media's camera metadata, which this host does not provide (the "
                                        "app reads it from the file)");
    }
    auto track = services->read(c.state(), t.item->id, t.streamIndex);
    if (!track) {
        fail(ErrorCode::EditRejected, "no gyro data: " + track.error().message);
    }
    const int width = t.stream->width;
    const int height = t.stream->height;
    GyroRun run;
    GyroFit fit;
    if (settings.gyroOptions.autoSync) {
        const std::vector<ImageMotionWindow> windows = measureSyncWindows(c, t, progressWindow);
        fit = fitGyroToPictures(*track, windows, width);
    }
    run.setup = resolveGyroSetup(*track, settings.gyroOptions, fit, width);
    // Frames the gyro does not reach stay as shot.
    for (const Ticks time : t.times) {
        const double at = time.toSeconds() + run.setup.offsetSeconds + 0.5 * run.setup.readoutSeconds;
        if (at < track->firstTime() - 0.25 || at > track->lastTime() + 0.25) {
            ++run.uncovered;
        }
    }
    if (run.uncovered == t.times.size()) {
        fail(ErrorCode::EditRejected, "the gyro data does not cover this part of the clip (it runs from " +
                                          std::to_string(track->firstTime()) + " s to " + std::to_string(track->lastTime()) + " s)");
    }
    if (run.uncovered > 0) {
        run.warnings.push_back(wabcut::counted(static_cast<std::int64_t>(run.uncovered), "frame") +
                               (run.uncovered == 1 ? " lies outside the recorded gyro data and is left as shot"
                                                   : " lie outside the recorded gyro data and are left as shot"));
    }
    if (settings.gyroOptions.autoSync && !run.setup.synced) {
        run.warnings.push_back("the gyro was used with the camera's own timing: " + fit.note);
    }
    run.comp = stabilizeFromGyro(*track, t.times, run.setup, settings.smooth, width, height);
    run.camera = track->make + (track->model.empty() ? std::string() : " " + track->model);
    run.summary = gyroSummaryText(*track, run.setup, settings.gyroOptions.autoSync);
    return run;
}

struct WarpResult {
    EffectInstanceId instance;
    std::size_t frames = 0;
    int lost = 0;
    bool cached = false;
    Compensation comp;
    std::optional<GyroRun> gyro; ///< 0.7 (card #130): set when the method is the camera's gyro
};

/// Warp Stabilizer's analysis of `subject` (a clip of `sequence` in `state`): tracks (or reads the cached motion), smooths,
/// keys the compensating transform into the effect - added at the top of the clip's stack when it has none - and the
/// framing results; `state` becomes the edited project.
WarpResult analyzeWarp(ExecutionContext& c, ProjectState& state, SequenceId sequence, const Subject& subject,
                       const ProgressWindow* window) {
    const Target t = targetOf(state, *state.findSequence(sequence), subject, "a Warp Stabilizer");
    const EffectInstanceId instance = instanceOf(c, state, sequence, subject, t, kWarpStabilizerEffect);
    const Clip& current = state.findSequence(sequence)->findClip(t.clip)->clip();
    const EffectInstance& effect = *playedInstance(state, current, instance);
    const WarpSettings settings = warpSettingsOf(effect, t);
    const Method method = settings.method;

    WarpResult result;
    result.instance = instance;
    MotionAnalysis analysis; // the picture's motion; with the gyro method there is none
    if (settings.gyro) {
        result.gyro = runGyro(c, t, settings, window);
        result.comp = result.gyro->comp;
    } else {
        analysis = motionOf(c, t, method, settings.detailed, result.cached, window);
        // The compensation, keyed per analysed frame (sparse within a tolerance), plus the framing results. A time-remapped
        // clip is keyed per frame it shows, in curve time (0.7, card #107).
        result.comp = stabilize(analysis, settings.smooth, t.stream->width, t.stream->height);
    }
    const Compensation& comp = result.comp;
    const std::size_t n = std::min(comp.times.size(), t.times.size());
    const std::vector<Ticks> keyTimes = keyTimesOf(t, std::span(comp.times.data(), n));
    edit::EffectChange change;
    const auto channel = [&](const char* name, const std::vector<double>& values, double tolerance, double fallback) {
        if (values.size() >= n && n > 0) {
            const std::vector<double> keyed = perKey(t, std::span(values.data(), n));
            change.params.push_back(EffectParam{name, keysOf(keyTimes, keyed, tolerance)});
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
    for (const FrameMotion& f : analysis.frames) {
        result.lost += f.lost ? 1 : 0;
    }
    result.frames = settings.gyro ? t.times.size() : analysis.frames.size();
    if (settings.gyro) {
        result.lost = static_cast<int>(result.gyro->uncovered); // frames the gyro does not reach count as not matched
        change.data.push_back(EffectData{"gyro", result.gyro->summary});
    } else if (effect.findData("gyro")) {
        change.data.push_back(EffectData{"gyro", std::string()}); // switched back to the picture: the camera note goes
    }
    change.data.push_back(EffectData{"analysis", warpSummary(settings, result.frames, result.lost, t)});
    change.data.push_back(EffectData{"status", std::string(result.lost == static_cast<int>(result.frames) ? "failed" : "analyzed")});
    sortParams(change);
    edit::EditResult r = c.take(edit::setEffect(state, sequence, t.clip, instance, change));
    state = std::move(r.state);
    return result;
}

struct ShutterResult {
    EffectInstanceId instance;
    std::size_t frames = 0;
    bool cached = false;
};

/// Rolling Shutter Repair's analysis, as analyzeWarp() is Warp Stabilizer's.
ShutterResult analyzeShutter(ExecutionContext& c, ProjectState& state, SequenceId sequence, const Subject& subject,
                             const ProgressWindow* window) {
    const Target t = targetOf(state, *state.findSequence(sequence), subject, "Rolling Shutter Repair");
    const EffectInstanceId instance = instanceOf(c, state, sequence, subject, t, kRollingShutterRepairEffect);
    const Clip& current = state.findSequence(sequence)->findClip(t.clip)->clip();
    const EffectInstance& effect = *playedInstance(state, current, instance);
    const bool detailed = effectParameterAt(effect, "detailedAnalysis", Ticks{}, 0.0) >= 0.5;
    ShutterResult result;
    result.instance = instance;
    const MotionAnalysis analysis = motionOf(c, t, Method::Position, detailed, result.cached, window);
    const FrameVelocity v = velocities(analysis, t.stream->width, t.stream->height);
    const std::size_t n = std::min(v.times.size(), t.times.size());
    edit::EffectChange change;
    const std::vector<Ticks> keyTimes = keyTimesOf(t, std::span(v.times.data(), n)); // 0.7 (card #107)
    const std::vector<double> x = perKey(t, std::span(v.x.data(), n));
    const std::vector<double> y = perKey(t, std::span(v.y.data(), n));
    change.params.push_back(EffectParam{"motion.x", n > 0 ? keysOf(keyTimes, x, 0.05) : ParamValue(0.0)});
    change.params.push_back(EffectParam{"motion.y", n > 0 ? keysOf(keyTimes, y, 0.05) : ParamValue(0.0)});
    result.frames = analysis.frames.size();
    change.data.push_back(EffectData{"analysis", shutterSummary(detailed, analysis.frames.size(), t)});
    change.data.push_back(EffectData{"status", std::string("analyzed")});
    sortParams(change);
    edit::EditResult r = c.take(edit::setEffect(state, sequence, t.clip, instance, change));
    state = std::move(r.state);
    return result;
}

/// The effect a command names (`effect`), if it does.
Subject subjectOf(ExecutionContext& c) {
    Subject s;
    s.clip = c.clip("clip");
    if (c.has("effect")) {
        s.effect = EffectInstanceId{static_cast<std::uint64_t>(c.integer("effect"))};
    }
    return s;
}

CommandOutput analyzeStabilization(ExecutionContext& c) {
    ProjectState state = c.state();
    const WarpResult w = analyzeWarp(c, state, c.sequenceId(), subjectOf(c), nullptr);
    const Compensation& comp = w.comp;
    CommandOutput out;
    out.state = std::move(state);
    out.createdIds.push_back(w.instance.value);
    out.data = Json{{"clip", c.clip("clip").value},
                    {"effect", w.instance.value},
                    {"frames", w.frames},
                    {"lostFrames", w.lost},
                    {"cached", w.cached},
                    {"autoScale", comp.autoScale},
                    {"scaleCapped", comp.scaleCapped},
                    {"rollingShutterShare", comp.rollingShutterShare},
                    {"crop", Json{{"left", comp.cropLeft}, {"top", comp.cropTop}, {"right", comp.cropRight}, {"bottom", comp.cropBottom}}}};
    if (w.gyro) {
        const GyroSetup& setup = w.gyro->setup;
        out.data["gyro"] = Json{{"camera", w.gyro->camera},
                                {"offsetMs", setup.offsetSeconds * 1000.0},
                                {"fieldOfView", setup.horizontalFovDegrees},
                                {"fieldOfViewFrom", setup.fovFrom},
                                {"readoutMs", setup.readoutSeconds * 1000.0},
                                {"synced", setup.synced},
                                {"match", setup.fit.match},
                                {"axes", describeGyroAxes(setup.axes)},
                                {"framesOutsideGyro", w.gyro->uncovered}};
        out.warnings.insert(out.warnings.end(), w.gyro->warnings.begin(), w.gyro->warnings.end());
    }
    if (comp.scaleCapped) {
        out.warnings.push_back("the motion needs more than the maximum scale: borders may show (raise Maximum Scale or "
                               "lower Smoothness)");
    }
    return out;
}

CommandOutput analyzeRollingShutter(ExecutionContext& c) {
    ProjectState state = c.state();
    const ShutterResult s = analyzeShutter(c, state, c.sequenceId(), subjectOf(c), nullptr);
    CommandOutput out;
    out.state = std::move(state);
    out.createdIds.push_back(s.instance.value);
    out.data = Json{{"clip", c.clip("clip").value}, {"effect", s.instance.value}, {"frames", s.frames}, {"cached", s.cached}};
    return out;
}

/// 0.7 (card #128): effect.analyzeAll - every Warp Stabilizer and Rolling Shutter Repair of the sequence (and of the
/// sequences nested in it) that is not analysed, or whose analysis is out of date, in one job and one undo step.
CommandOutput analyzeAll(ExecutionContext& c) {
    ProjectState state = c.state();
    const std::string kind = c.string("kind", "all");
    const bool wantWarp = kind != "shutter";
    const bool wantShutter = kind != "warp";
    const bool reanalyze = c.boolean("reanalyze", false);

    std::vector<StabilizerEntry> jobs;
    Json skipped = Json::array();
    int current = 0;
    for (const StabilizerEntry& entry : surveyStabilizers(state, c.sequenceId(), c.boolean("nested", true))) {
        if (entry.warp ? !wantWarp : !wantShutter) {
            continue;
        }
        if (entry.need == StabilizerNeed::Skipped) {
            skipped.push_back(Json{{"clip", entry.clip.value}, {"effect", entry.effect.value}, {"reason", entry.reason}});
        } else if (entry.need == StabilizerNeed::Current && !reanalyze) {
            ++current;
        } else {
            jobs.push_back(entry);
        }
    }

    // 0.7 (card #130): a stabilizer on the Gyro method needs the camera's data, not the frames; a job that lacks what it needs
    // is reported under `skipped` below.
    if (!jobs.empty() && !command::findService<masks::TrackingServices>(c.services()) &&
        !command::findService<gyro::GyroServices>(c.services())) {
        fail(ErrorCode::InvalidRequest, "the analysis needs the media's frames, which this host does not provide (the app "
                                        "analyses media)");
    }
    int total = 0;
    for (const StabilizerEntry& job : jobs) {
        total += static_cast<int>(job.frames);
    }
    int done = 0;
    int analysed = 0;
    Json effects = Json::array();
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        const StabilizerEntry& job = jobs[i];
        c.checkpoint("analyzing effect " + std::to_string(i + 1) + " of " + std::to_string(jobs.size()));
        const ProgressWindow window{done, total};
        done += static_cast<int>(job.frames);
        try {
            if (job.warp) {
                analyzeWarp(c, state, job.sequence, Subject{job.clip, job.effect}, &window);
            } else {
                analyzeShutter(c, state, job.sequence, Subject{job.clip, job.effect}, &window);
            }
            ++analysed;
            effects.push_back(Json{{"clip", job.clip.value}, {"effect", job.effect.value}, {"kind", job.warp ? "warp" : "shutter"}});
        } catch (const command::CommandFailure& failure) {
            if (failure.error.code == ErrorCode::Cancelled) {
                throw;
            }
            skipped.push_back(Json{{"clip", job.clip.value}, {"effect", job.effect.value}, {"reason", failure.error.message}});
        }
    }
    CommandOutput out;
    out.state = std::move(state);
    out.data = Json{{"analyzed", analysed},
                    {"alreadyAnalyzed", current},
                    {"skipped", skipped},
                    {"effects", std::move(effects)},
                    {"frames", total}};
    if (!skipped.empty()) {
        out.warnings.push_back(wabcut::counted(static_cast<std::int64_t>(skipped.size()), "effect") +
                               (skipped.size() == 1 ? " was not analysed: " : " were not analysed: ") +
                               skipped.front().value("reason", std::string()));
    }
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

/// 0.7 (cards #107, #128): where every stabilizer stands (StabilizeCommands.h). One pass over the clips; a clip that cannot
/// be analysed (a freeze frame, no camera footage, a nested clip's own) is Skipped with the reason the command gives.
std::vector<StabilizerEntry> surveyStabilizers(const ProjectState& state, SequenceId sequence, bool nested) {
    std::vector<SequenceId> sequences{sequence};
    std::vector<int> depths{0};
    if (nested) {
        for (std::size_t i = 0; i < sequences.size(); ++i) {
            const Sequence* seq = state.findSequence(sequences[i]);
            if (!seq) {
                continue;
            }
            for (const TrackPtr& track : seq->videoTracks) {
                for (const Clip& clip : track->clips) {
                    if (clip.isNested() && state.findSequence(clip.sequence) &&
                        std::find(sequences.begin(), sequences.end(), clip.sequence) == sequences.end()) {
                        sequences.push_back(clip.sequence);
                        depths.push_back(depths[i] + 1);
                    }
                }
            }
        }
    }
    std::vector<StabilizerEntry> out;
    std::vector<std::uint64_t> seen; // an instance of a Master Clip Effect is met on every clip of its media
    for (std::size_t s = 0; s < sequences.size(); ++s) {
        const Sequence* seq = state.findSequence(sequences[s]);
        if (!seq) {
            continue;
        }
        for (const TrackPtr& track : seq->videoTracks) {
            for (const Clip& clip : track->clips) {
                if (clip.stream.kind != StreamKind::Video || clip.isNested() || clip.hasUnknownSource()) {
                    continue;
                }
                const auto consider = [&](const EffectInstance& e, bool master) {
                    const bool warp = e.effect == kWarpStabilizerEffect;
                    if (!warp && e.effect != kRollingShutterRepairEffect) {
                        return;
                    }
                    if (std::find(seen.begin(), seen.end(), e.id.value) != seen.end()) {
                        return;
                    }
                    seen.push_back(e.id.value);
                    StabilizerEntry entry;
                    entry.sequence = sequences[s];
                    entry.clip = clip.id;
                    entry.effect = e.id;
                    entry.warp = warp;
                    entry.where = clip.timelineRange;
                    entry.depth = depths[s];
                    entry.master = master;
                    if (!e.enabled) {
                        entry.need = StabilizerNeed::Skipped;
                        entry.reason = "the effect is switched off";
                        out.push_back(std::move(entry));
                        return;
                    }
                    try {
                        const Target t = targetOf(state, *seq, Subject{clip.id, e.id}, warp ? "a Warp Stabilizer" : "Rolling Shutter Repair");
                        entry.frames = t.times.size();
                        const EffectData* status = e.findData("status");
                        const bool analysed = status && status->text() && *status->text() == "analyzed";
                        entry.need = analysisCurrent(e, t, warp) ? StabilizerNeed::Current
                                     : analysed                  ? StabilizerNeed::OutOfDate
                                                                 : StabilizerNeed::NotAnalyzed;
                    } catch (const command::CommandFailure& failure) {
                        entry.need = StabilizerNeed::Skipped;
                        entry.reason = failure.error.message;
                    }
                    out.push_back(std::move(entry));
                };
                for (const EffectInstance& e : clip.effects()) {
                    consider(e, false);
                }
                if (clip.media.isValid()) {
                    for (const EffectInstance& e : masterEffectsOf(state, clip.media, EffectKind::Video)) {
                        consider(e, true);
                    }
                }
            }
        }
    }
    return out;
}


void registerStabilizeCommands(command::CommandRegistry& registry) {
    command::CommandSpec stabilization{
        "effect.analyzeStabilization", 1, "Analyze Stabilization",
        "Warp Stabilizer's Analyze: tracks the camera motion of a video clip's footage, smooths the camera path with the "
        "effect's settings (Result, Smoothness, Method, Preserve Scale, Framing, Maximum Scale, Action-Safe Margin, "
        "Additional Scale, Detailed Analysis, Rolling Shutter Ripple, Crop Less Smooth More) and keys the compensating "
        "transform per frame into the clip's Warp Stabilizer (added at the top of its stack when it has none), with the "
        "crop or auto-scale that hides the moving borders. The keys stay editable. Changing a setting and running it "
        "again re-smooths from the cached analysis. Method 4 (Gyro, camera metadata) takes the rotation the camera "
        "recorded in the file (GoPro HERO5 and later, Sony cameras with a gyro) instead of tracking the picture: it is "
        "synced to the picture on a few stretches of the clip (gyroSync, gyroOffset, gyroFov, gyroReadout), smoothed "
        "with the same settings and keyed as a corner pin; the result reports the camera, the sync and the field of "
        "view under `gyro`, and a file without gyro data is refused with the reason. A Warp Stabilizer in the clip's media's Master Clip Effects (named by "
        "`effect`) is analysed over the frames every clip of that media shows, so every instance plays it stabilized. "
        "Runs as a job with progress and cancel; a dry run analyses and previews the keys.",
        command::Capability::EditSequence,
        analyzeSchema("Its Warp Stabilizer instance (id), in the clip's stack or its media's Master Clip Effects. Default: "
                      "the clip's first (or a new one)."),
        Json{{"clip", 42}}, {}, analyzeStabilization};
    [[maybe_unused]] const auto addedStabilization = registry.add(std::move(stabilization));
    command::CommandSpec shutter{
        "effect.analyzeRollingShutter", 1, "Analyze Rolling Shutter",
        "Rolling Shutter Repair's analysis: measures the picture's motion per frame of a video clip's footage and keys it "
        "(motion.x, motion.y: source pixels per frame) into the clip's Rolling Shutter Repair (added at the top of its "
        "stack when it has none), which then skews each line back by its share of the scan (Rolling Shutter Rate, Scan "
        "Direction). Shares the cached analysis with Warp Stabilizer's Position method. One in the clip's media's Master "
        "Clip Effects (named by `effect`) is analysed over the frames every clip of that media shows.",
        command::Capability::EditSequence,
        analyzeSchema("Its Rolling Shutter Repair instance (id), in the clip's stack or its media's Master Clip Effects. "
                      "Default: the clip's first (or a new one)."),
        Json{{"clip", 42}}, {}, analyzeRollingShutter};
    [[maybe_unused]] const auto addedShutter = registry.add(std::move(shutter));
    Json allSchema = schema::object(
        {{"kind", schema::enumeration({"all", "warp", "shutter"}, "Which effects: Warp Stabilizer and Rolling Shutter Repair "
                                                                  "(\"all\", the default), or one of them.")},
         {"reanalyze", schema::boolean("Analyse every one again, not only those that are not analysed or out of date.", false)},
         {"nested", schema::boolean("Also the sequences nested in this one (their clips keep their own effects).", true)}});
    allSchema["properties"]["sequence"] = schema::integer("Sequence id; only needed when the project has several.", 1);
    command::CommandSpec all{
        "effect.analyzeAll", 1, "Analyze All Stabilizers",
        "Analyses every Warp Stabilizer and Rolling Shutter Repair of the sequence (and of the sequences nested in it, "
        "and of its clips' media's Master Clip Effects) that is not analysed yet or whose analysis is out of date - the clip "
        "was trimmed longer, moved onto other footage, or a setting such as Smoothness changed - in one job with one "
        "progress bar and one undo step. `reanalyze` redoes every one. The raw motion is cached per media, so redoing "
        "an effect whose footage was analysed before is quick. Effects that cannot be analysed (a nested clip's own, a "
        "freeze frame, a switched-off effect) are listed under `skipped`. Morph Cut needs no analysis step: it analyses "
        "itself as soon as it is applied. Runs as a job with progress and cancel.",
        command::Capability::EditSequence, std::move(allSchema), Json{{"reanalyze", false}}, {}, analyzeAll};
    [[maybe_unused]] const auto addedAll = registry.add(std::move(all));
}

} // namespace wabcut::ai::stabilize
