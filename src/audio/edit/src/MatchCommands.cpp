// 0.7 (docs/audio.md "Match"): learning from a reference. audio.eqMatch listens to a reference (a clip, a track, a range of
// the mix, or a media file of the project) and to the target clips, and gives each target a Graphic Equalizer (30 Bands)
// whose sliders are the smoothed difference of the two average spectra (MatchCurve.h: 24 bands from 79 Hz to 16 kHz, at
// most +-18 dB, the overall level left alone); the instance is marked as an EQ Match (effect data) and carries both
// spectra, so the panel can draw them again after a reload. audio.eqMatchReset takes it off again. audio.levelMatch moves
// the target's loudness (or true peak) to the reference's with the normalisation gain, optionally limiting to a
// headroom, and reports what it measured before and after. Everything runs through the host's LoudnessAnalyzer
// (measureMatch: the real render path, so preview, export and the measurement hear the same thing); each command is one
// undo step.

#include "AudioCommandHelpers.h"

#include <wabcut/audio/edit/MatchCurve.h>

#include <wabcut/model/Resolve.h>

#include <cmath>
#include <numeric>

namespace wabcut::audio::edit {

namespace {

using namespace detail;

constexpr std::string_view kEqMatchEffect = "wabcut.audio.graphicEq30";
/// Effect data of an EQ Match instance: the marker (text), the reference's and the target's spectra (points: Hz, dB).
constexpr const char* kMarkerKey = "eqMatch";
constexpr const char* kReferenceKey = "eqMatchReference";
constexpr const char* kTargetKey = "eqMatchTarget";
/// The longest stretch of a media file used as a reference when no range is given.
constexpr double kMediaReferenceSeconds = 600.0;

void add(command::CommandRegistry& registry, command::CommandSpec spec) {
    [[maybe_unused]] const auto added = registry.add(std::move(spec));
}

Json referenceParam() {
    return Json{{"type", Json::array({"object", "string"})},
                {"description", "What to match: \"mix\" (the sequence's mix), {\"clip\": id}, {\"track\": \"A1\"} (a track on its "
                                "own), or {\"media\": id [, \"stream\": n]} (an audio file of the project, its audio stream n "
                                "from 0). Clips and the mix are heard as they render, effects included. Limit the stretch with "
                                "referenceFrom / referenceTo (a media file: from its start)."}};
}

// ---- Measuring ---------------------------------------------------------------------------------------------------------

/// What is measured as the reference: a target of a sequence of a state (the project's, or a scratch one for a media file).
struct Source {
    ProjectState state;
    SequenceId sequence;
    LoudnessTarget target;
    std::optional<TimeRange> range;
    Json label;
};

bool isMatchInstance(const EffectInstance& e) { return e.effect == kEqMatchEffect && e.findData(kMarkerKey) != nullptr; }

/// A scratch sequence of the project's rates that holds one audio stream of `media`, so a file is measured on the same
/// render path as a clip.
Source mediaSource(ExecutionContext& c, MediaId id, int stream, std::optional<Ticks> from, std::optional<Ticks> to) {
    const MediaItem* item = c.state().findMedia(id);
    if (item == nullptr) {
        fail(ErrorCode::NotFound, "no such media item", Json{{"path", "/reference/media"}});
    }
    if (stream < 0 || stream >= static_cast<int>(item->info.audio.size())) {
        fail(ErrorCode::InvalidParams, "\"" + item->name + "\" has no audio stream " + std::to_string(stream),
             Json{{"path", "/reference/stream"}});
    }
    const Ticks length = item->info.audio[static_cast<std::size_t>(stream)].duration.value > 0
                             ? item->info.audio[static_cast<std::size_t>(stream)].duration
                             : item->info.duration;
    const Ticks start = from.value_or(Ticks{});
    Ticks end = to.value_or(Ticks{std::min(length.value, start.value + seconds(kMediaReferenceSeconds).value)});
    end = Ticks{std::min(end.value, length.value)};
    if (end <= start) {
        fail(ErrorCode::InvalidParams, "the reference range is empty", Json{{"path", "/referenceTo"}});
    }
    const Sequence& current = c.sequence();
    wabcut::edit::SequenceSettings settings;
    settings.name = "Match reference";
    settings.frameRate = current.frameRate;
    settings.width = current.width;
    settings.height = current.height;
    settings.audioRate = current.audioRate;
    settings.videoTracks = 0;
    settings.audioTracks = 1;
    auto created = c.take(wabcut::edit::addSequence(c.state(), settings));
    ProjectState state = std::move(created.state);
    const Sequence& scratch = *state.findSequence(created.id);
    wabcut::edit::SourceClip source;
    source.media = id;
    source.sourceRange = TimeRange::fromStartEnd(start, end);
    source.patches = {wabcut::edit::SourcePatch{StreamRef{StreamKind::Audio, stream}, scratch.audioTracks.front()->id}};
    auto placed = c.take(wabcut::edit::overwriteEdit(state, created.id, source, Ticks{}));
    return Source{std::move(placed.state), created.id, LoudnessTarget::mix(), std::nullopt, Json{{"media", id.value}}};
}

Source referenceOf(ExecutionContext& c) {
    if (!c.has("reference")) {
        fail(ErrorCode::InvalidParams, "\"reference\" says what to match", Json{{"path", "/reference"}});
    }
    const Json& ref = c.params()["reference"];
    const std::optional<Ticks> from = c.optionalTime("referenceFrom");
    const std::optional<Ticks> to = c.optionalTime("referenceTo");
    std::optional<TimeRange> range;
    if (from || to) {
        const Ticks start = from.value_or(Ticks{});
        const Ticks end = to.value_or(std::max(c.sequence().contentEnd(), start));
        if (end <= start) {
            fail(ErrorCode::InvalidParams, "\"referenceTo\" must come after \"referenceFrom\"", Json{{"path", "/referenceTo"}});
        }
        range = TimeRange::fromStartEnd(start, end);
    }
    if (ref.is_string() && ref.get<std::string>() == "mix") {
        return Source{c.state(), c.sequenceId(), LoudnessTarget::mix(), range, "mix"};
    }
    if (ref.is_object() && ref.contains("clip")) {
        const ClipId clip = c.clipValue(ref["clip"], "/reference/clip");
        return Source{c.state(), c.sequenceId(), LoudnessTarget::ofClip(clip), range, Json{{"clip", clip.value}}};
    }
    if (ref.is_object() && ref.contains("track")) {
        const TrackId track = c.trackValue(ref["track"], "/reference/track");
        return Source{c.state(), c.sequenceId(), LoudnessTarget::ofTrack(track), range, Json{{"track", c.trackLabel(track)}}};
    }
    if (ref.is_object() && ref.contains("media")) {
        const MediaId media = c.mediaValue(ref["media"], "/reference/media");
        return mediaSource(c, media, static_cast<int>(ref.value("stream", 0)), from, to);
    }
    fail(ErrorCode::InvalidParams, "a reference is \"mix\", {\"clip\"}, {\"track\"} or {\"media\"}", Json{{"path", "/reference"}});
}

MatchStats measureMatchOrFail(ExecutionContext& c, AudioServices& services, const ProjectState& state, SequenceId sequence,
                              const LoudnessTarget& target, std::optional<TimeRange> range, const char* what) {
    auto stats = services.loudness->measureMatch(state, sequence, target, range, c.control());
    if (!stats) {
        failWith(stats.error(), what);
    }
    return *stats;
}

Json matchStatsJson(const MatchStats& s) {
    Json j = statsJson(s.loudness);
    j["rmsDbfs"] = levelJson(s.rmsDbfs);
    j["crestDb"] = levelJson(s.crestDb);
    j["audibleSeconds"] = round3(s.spectrum.activeSeconds);
    return j;
}

/// The clip with every EQ Match removed from its stack (what the target sounds like without the match, so learning again
/// does not learn from its own correction).
ProjectState withoutMatch(ExecutionContext& c, ProjectState state, ClipId clip) {
    for (;;) {
        const auto at = sequenceIn(state, c.sequenceId()).findClip(clip);
        const EffectInstance* found = nullptr;
        if (at) {
            for (const EffectInstance& e : at->clip().effects()) {
                if (isMatchInstance(e)) {
                    found = &e;
                    break;
                }
            }
        }
        if (found == nullptr) {
            return state;
        }
        state = c.take(wabcut::edit::removeEffect(state, c.sequenceId(), clip, found->id)).state;
    }
}

// ---- audio.eqMatch -----------------------------------------------------------------------------------------------------

EffectData pointsOf(const char* key, const SpectrumProfile& profile) {
    std::vector<EffectPoint> points;
    for (int b = 0; b < kMatchBandCount; ++b) {
        const double db = profile.levelDb[static_cast<std::size_t>(b)];
        if (std::isfinite(db)) {
            points.push_back({std::round(matchBandHz(b) * 100.0) / 100.0, std::round(db * 100.0) / 100.0});
        }
    }
    return EffectData{key, std::move(points)};
}

Json curveJson(const EqMatchCurve& curve, const SpectrumProfile& reference, const SpectrumProfile& target) {
    Json bands = Json::array();
    for (int b = 0; b < kMatchBandCount; ++b) {
        const auto i = static_cast<std::size_t>(b);
        bands.push_back(Json{{"band", b + 1},
                             {"hz", std::round(matchBandHz(b) * 10.0) / 10.0},
                             {"referenceDb", levelJson(reference.levelDb[i])},
                             {"targetDb", levelJson(target.levelDb[i])},
                             {"gainDb", std::round(curve.gainDb[i] * 100.0) / 100.0}});
    }
    return Json{{"bands", std::move(bands)}, {"levelOffsetDb", std::round(curve.levelOffsetDb * 100.0) / 100.0}};
}

/// The mean of the targets' spectra, each weighted by the audible time it had (power, not dB).
SpectrumProfile combined(const std::vector<MatchStats>& stats) {
    if (stats.size() == 1) {
        return stats.front().spectrum;
    }
    SpectrumProfile out;
    std::array<double, kMatchBandCount> power{};
    std::array<double, kMatchBandCount> weight{};
    for (const MatchStats& s : stats) {
        if (!s.spectrum.usable()) {
            continue;
        }
        out.seconds += s.spectrum.seconds;
        out.activeSeconds += s.spectrum.activeSeconds;
        out.sampleRate = std::max(out.sampleRate, s.spectrum.sampleRate);
        for (std::size_t b = 0; b < power.size(); ++b) {
            if (std::isfinite(s.spectrum.levelDb[b])) {
                power[b] += std::pow(10.0, s.spectrum.levelDb[b] / 10.0) * s.spectrum.activeSeconds;
                weight[b] += s.spectrum.activeSeconds;
            }
        }
    }
    for (std::size_t b = 0; b < power.size(); ++b) {
        if (weight[b] > 0.0) {
            out.levelDb[b] = 10.0 * std::log10(power[b] / weight[b]);
        }
    }
    return out;
}

CommandOutput eqMatch(ExecutionContext& c) {
    AudioServices& services = servicesOf(c, true, false);
    if (!c.has("clips")) {
        fail(ErrorCode::InvalidParams, "\"clips\" names the clips to match", Json{{"path", "/clips"}});
    }
    const std::vector<ClipId> clips = audioClipsOf(c, c.clips("clips"));
    const auto range = [&]() -> std::optional<TimeRange> {
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
    }();
    EqMatchOptions options;
    options.amount = c.has("amount") ? c.number("amount") / 100.0 : 1.0;
    options.smoothing = c.has("smoothing") ? c.number("smoothing") / 100.0 : 0.5;
    options.maxGainDb = c.has("maxGain") ? c.number("maxGain") : kMatchMaxGainDb;

    c.checkpoint("listening to the reference");
    const Source reference = referenceOf(c);
    const MatchStats refStats = measureMatchOrFail(c, services, reference.state, reference.sequence, reference.target,
                                                   reference.range, "measuring the reference");
    if (!refStats.spectrum.usable()) {
        fail(ErrorCode::EditRejected, "the reference is silent or has less than a second of audible sound: nothing to match");
    }

    ProjectState state = c.state();
    std::vector<MatchStats> targets;
    Json clipRows = Json::array();
    for (const ClipId id : clips) {
        c.checkpoint("listening to the target");
        state = withoutMatch(c, std::move(state), id);
        targets.push_back(measureMatchOrFail(c, services, state, c.sequenceId(), LoudnessTarget::ofClip(id), range,
                                             "measuring the target"));
        if (!targets.back().spectrum.usable()) {
            fail(ErrorCode::EditRejected, "clip " + std::to_string(id.value) +
                                              " is silent or has less than a second of audible sound: nothing to match");
        }
    }
    const SpectrumProfile target = combined(targets);
    const EqMatchCurve curve = fitEqMatch(refStats.spectrum, target, options);
    if (!curve.valid) {
        fail(ErrorCode::EditRejected, "the two spectra share too few bands to match (a very low sample rate?)");
    }

    CommandOutput out;
    const bool apply = c.boolean("apply", true);
    if (apply) {
        for (const ClipId id : clips) {
            std::vector<EffectParam> params;
            for (int b = 0; b < kMatchBandCount; ++b) {
                params.push_back({"band" + std::to_string(b + 1), ParamValue(std::round(curve.gainDb[static_cast<std::size_t>(b)] * 100.0) / 100.0)});
            }
            const EffectStack* stack = wabcut::edit::effectStackOf(sequenceIn(state, c.sequenceId()), EffectTarget::ofClip(id));
            std::size_t at = stack != nullptr ? normalizationGainIndex(*stack) : 0;
            if (stack != nullptr && normalizationGain(*stack) != nullptr) {
                ++at; // after the normalisation gain, before the dynamics
            }
            auto added = c.take(wabcut::edit::addEffect(state, c.sequenceId(), EffectTarget::ofClip(id), std::string(kEqMatchEffect),
                                                        std::move(params), at));
            const EffectInstanceId instance = added.createdEffects.front();
            wabcut::edit::EffectChange marker;
            marker.data = {EffectData{kMarkerKey, std::string("v1")}, pointsOf(kReferenceKey, refStats.spectrum),
                           pointsOf(kTargetKey, target)};
            state = c.take(wabcut::edit::setEffect(added.state, c.sequenceId(), EffectTarget::ofClip(id), instance, marker)).state;
            out.createdIds.push_back(instance.value);
            clipRows.push_back(Json{{"clip", id.value}, {"effect", instance.value}});
        }
    }
    out.state = apply ? std::move(state) : c.state();
    out.data = Json{{"applied", apply},
                    {"reference", Json{{"source", reference.label}, {"stats", matchStatsJson(refStats)}}},
                    {"target", Json{{"clips", Json(clips.size())}, {"stats", matchStatsJson(targets.front())}}},
                    {"curve", curveJson(curve, refStats.spectrum, target)},
                    {"clips", std::move(clipRows)}};
    if (curve.levelOffsetDb > 6.0 || curve.levelOffsetDb < -6.0) {
        out.warnings.push_back("the reference is " + std::to_string(static_cast<int>(std::lround(std::fabs(curve.levelOffsetDb)))) +
                               " dB " + (curve.levelOffsetDb > 0 ? "louder" : "quieter") +
                               " than the target: EQ Match leaves the level alone, use audio.levelMatch for that");
    }
    return out;
}

CommandOutput eqMatchReset(ExecutionContext& c) {
    if (!c.has("clips")) {
        fail(ErrorCode::InvalidParams, "\"clips\" names the clips", Json{{"path", "/clips"}});
    }
    ProjectState state = c.state();
    int removed = 0;
    for (const ClipId id : audioClipsOf(c, c.clips("clips"))) {
        for (;;) {
            const auto at = sequenceIn(state, c.sequenceId()).findClip(id);
            const EffectInstance* found = nullptr;
            for (const EffectInstance& e : at->clip().effects()) {
                if (isMatchInstance(e)) {
                    found = &e;
                    break;
                }
            }
            if (found == nullptr) {
                break;
            }
            state = c.take(wabcut::edit::removeEffect(state, c.sequenceId(), id, found->id)).state;
            ++removed;
        }
    }
    CommandOutput out;
    out.state = std::move(state);
    out.data = Json{{"removed", removed}};
    if (removed == 0) {
        out.warnings.push_back("none of these clips has an EQ Match");
    }
    return out;
}

// ---- audio.levelMatch --------------------------------------------------------------------------------------------------

struct LevelResult {
    ProjectState state;
    MatchStats before;
    MatchStats after;
    double gainDb = 0.0;
    int passes = 0;
    bool measured = false;
    bool reached = false;
};

/// Moves `site`'s normalisation gain until the target's integrated loudness (or, in peak mode, its true peak) is within
/// `tolerance` of `want`; measured again after each step (gain before nonlinear effects does not move a level one for one).
LevelResult matchLevel(ExecutionContext& c, AudioServices& services, ProjectState state, const LoudnessTarget& target,
                       const EffectTarget& site, bool peak, double want, double tolerance, int maxPasses,
                       std::optional<TimeRange> range) {
    LevelResult r;
    const auto level = [&](const MatchStats& s) { return peak ? s.loudness.truePeakDbtp : s.loudness.integratedLufs; };
    r.before = measureMatchOrFail(c, services, state, c.sequenceId(), target, range, "measuring the target");
    r.after = r.before;
    r.gainDb = currentGain(c, state, site);
    if (!std::isfinite(level(r.before))) {
        r.state = std::move(state);
        return r;
    }
    r.measured = true;
    while (r.passes < maxPasses && std::fabs(want - level(r.after)) > tolerance) {
        const double next = nextNormalizationGainDb(r.gainDb, want - level(r.after));
        if (next == r.gainDb) {
            break; // at the Gain effect's limit
        }
        r.gainDb = next;
        state = withGain(c, state, site, next);
        ++r.passes;
        c.checkpoint("matching the level");
        r.after = measureMatchOrFail(c, services, state, c.sequenceId(), target, range, "measuring the target");
        if (!std::isfinite(level(r.after))) {
            break;
        }
    }
    r.reached = std::fabs(want - level(r.after)) <= tolerance;
    r.state = std::move(state);
    return r;
}

CommandOutput levelMatch(ExecutionContext& c) {
    AudioServices& services = servicesOf(c, true, false);
    const std::string mode = c.string("mode", "loudness");
    if (mode != "loudness" && mode != "peak") {
        fail(ErrorCode::InvalidParams, "\"mode\" is \"loudness\" or \"peak\"", Json{{"path", "/mode"}});
    }
    const bool peak = mode == "peak";
    const double headroom = c.has("headroom") ? c.number("headroom") : 1.0;
    const bool limit = c.boolean("limit", false);
    const double tolerance = c.has("tolerance") ? c.number("tolerance") : 0.1;
    const int maxPasses = static_cast<int>(c.integer("maxPasses", 4));
    std::optional<TimeRange> range;
    if (c.has("from") || c.has("to")) {
        const Ticks start = c.optionalTime("from").value_or(Ticks{});
        const Ticks end = c.optionalTime("to").value_or(std::max(c.sequence().contentEnd(), start));
        if (end <= start) {
            fail(ErrorCode::InvalidParams, "\"to\" must come after \"from\"", Json{{"path", "/to"}});
        }
        range = TimeRange::fromStartEnd(start, end);
    }

    c.checkpoint("measuring the reference");
    const Source reference = referenceOf(c);
    const MatchStats refStats = measureMatchOrFail(c, services, reference.state, reference.sequence, reference.target,
                                                   reference.range, "measuring the reference");
    const double want = peak ? refStats.loudness.truePeakDbtp : refStats.loudness.integratedLufs;
    if (!std::isfinite(want)) {
        fail(ErrorCode::EditRejected, peak ? "the reference is silent: it has no peak to match"
                                           : "the reference is silent or shorter than 0.4 s: it has no loudness to match");
    }

    struct Site {
        LoudnessTarget target;
        EffectTarget site;
        Json label;
    };
    std::vector<Site> sites;
    if (c.has("clips")) {
        for (const ClipId clip : audioClipsOf(c, c.clips("clips"))) {
            sites.push_back({LoudnessTarget::ofClip(clip), EffectTarget::ofClip(clip), Json{{"clip", clip.value}}});
        }
    } else if (c.has("track")) {
        const TrackId track = c.track("track");
        sites.push_back({LoudnessTarget::ofTrack(track), EffectTarget::ofTrack(track), Json{{"track", c.trackLabel(track)}}});
    } else {
        sites.push_back({LoudnessTarget::mix(), EffectTarget::masterAudio(), "mix"});
    }

    CommandOutput out;
    ProjectState state = c.state();
    Json results = Json::array();
    std::uint64_t limiterId = 0;
    for (const Site& s : sites) {
        c.checkpoint("matching the level");
        LevelResult m = matchLevel(c, services, std::move(state), s.target, s.site, peak, want, tolerance, maxPasses, range);
        state = std::move(m.state);
        Json row{{"target", s.label}};
        row["before"] = matchStatsJson(m.before);
        if (!m.measured) {
            row["gainDb"] = levelJson(m.gainDb);
            out.warnings.push_back("nothing to measure in one target (silent or shorter than 0.4 s); left as it is");
            results.push_back(std::move(row));
            continue;
        }
        if (!m.reached) {
            out.warnings.push_back("could not reach the reference " + std::string(peak ? "peak" : "loudness") + " within " +
                                   std::to_string(tolerance) + " dB");
        }
        double truePeak = m.after.loudness.truePeakDbtp;
        if (std::isfinite(truePeak) && truePeak > -headroom + 0.05) {
            if (limit) {
                // Gain plus limiting: a true-peak limiter at the headroom, last on the target's stack (a limiter that ends it
                // already is moved to the ceiling instead).
                const EffectStack* stack = wabcut::edit::effectStackOf(sequenceIn(state, c.sequenceId()), s.site);
                if (stack != nullptr && !stack->empty() && stack->back().effect == kLimiterEffect) {
                    const EffectInstanceId existing = stack->back().id;
                    state = c.take(wabcut::edit::setEffect(state, c.sequenceId(), s.site, existing,
                                                           wabcut::edit::EffectChange{true, {{"ceiling", ParamValue(-headroom)}}}))
                                .state;
                    limiterId = existing.value;
                } else {
                    auto added = c.take(wabcut::edit::addEffect(state, c.sequenceId(), s.site, std::string(kLimiterEffect),
                                                                {{"ceiling", ParamValue(-headroom)}, {"lookahead", ParamValue(5.0)}}));
                    state = std::move(added.state);
                    limiterId = added.createdEffects.front().value;
                }
                out.createdIds.push_back(limiterId);
                m.after = measureMatchOrFail(c, services, state, c.sequenceId(), s.target, range, "measuring the result");
                row["limited"] = true;
            } else {
                out.warnings.push_back("the true peak is " + std::to_string(truePeak) + " dBTP, above the " +
                                       std::to_string(-headroom) + " dBTP headroom: use \"limit\": true, or match the peak");
            }
        }
        row["gainDb"] = levelJson(m.gainDb);
        row["after"] = matchStatsJson(m.after);
        row["passes"] = m.passes;
        results.push_back(std::move(row));
    }
    out.state = std::move(state);
    out.data = Json{{"mode", mode},
                    {"reference", Json{{"source", reference.label}, {"stats", matchStatsJson(refStats)}}},
                    {"headroomDb", headroom},
                    {"results", std::move(results)}};
    return out;
}

} // namespace

void registerMatchCommands(command::CommandRegistry& registry) {
    const Json clipsParam = schema::array(Json{{"type", Json::array({"integer", "object"})}},
                                          "The audio clips (video clips stand for their linked audio).");
    const Json rangeFrom = timeParam("Learn only from this time of the target (default: the start).");
    const Json rangeTo = timeParam("Learn only up to this time of the target (default: the end).");
    add(registry,
        {"audio.eqMatch", 1, "EQ Match",
         "Matches the tone of clips to a reference (Audition's Match EQ, Premiere's tone matching): listens to the reference "
         "(a clip, a track, a range of the mix or a media file) and to the target clips, averages the spectrum of each over "
         "the stretches that are not pauses, and gives every target a Graphic Equalizer (30 Bands) whose sliders are the "
         "smoothed difference: 24 bands from 79 Hz to 16 kHz, at most +-18 dB, a minimum-phase correction that costs no "
         "latency and plays the same in preview and export. The overall level is left alone (audio.levelMatch matches "
         "that). An earlier EQ Match on the clip is replaced. The instance is an ordinary effect, editable in Effect "
         "Controls, and keeps both spectra for the panel. \"apply\": false only learns and reports the curve. One undo "
         "step. Refused: a silent reference or target, or one with less than a second of audible sound.",
         Capability::EditSequence,
         withSequence({{"reference", referenceParam()},
                       {"referenceFrom", timeParam("Use the reference from this time.")},
                       {"referenceTo", timeParam("Use the reference up to this time.")},
                       {"clips", clipsParam},
                       {"from", rangeFrom},
                       {"to", rangeTo},
                       {"amount", schema::number("How much of the correction to apply, percent. Default 100.", 0.0, 100.0)},
                       {"smoothing", schema::number("How smooth the curve is, percent: 0 follows each band, 100 is gentle. Default 50.", 0.0, 100.0)},
                       {"maxGain", schema::number("The most any band may be moved, dB. Default 18.", 0.0, 18.0)},
                       {"apply", schema::boolean("Add the effect (default true); false: only learn and report.")}},
                      {"reference", "clips"}),
         Json{{"reference", Json{{"clip", 12}}}, {"clips", Json::array({15})}}, {}, eqMatch});
    add(registry,
        {"audio.eqMatchReset", 1, "Reset EQ Match",
         "Takes the EQ Match off the clips (the effect audio.eqMatch added; other equalizers stay). One undo step.",
         Capability::EditSequence, withSequence({{"clips", clipsParam}}, {"clips"}), Json{{"clips", Json::array({15})}}, {},
         eqMatchReset});
    add(registry,
        {"audio.levelMatch", 1, "Level Match",
         "Matches the loudness of the target to a reference (Premiere's Essential Sound loudness match, Audition's Match "
         "Loudness): measures the reference (a clip, a track, a range of the mix or a media file) and the target (clips, a "
         "track, or the mix) the way export hears them (EBU R128 integrated loudness, true peak, RMS, crest factor) and moves "
         "the target's normalisation gain until it meets the reference, in \"mode\": \"loudness\" (integrated loudness, "
         "default) or \"peak\" (true peak). \"headroom\" is the true peak the result may not pass (dB under full scale, "
         "default 1): with \"limit\" a true-peak limiter at that ceiling is added (gain plus limiting), otherwise a warning "
         "says so. Reports the measurements before and after. One undo step. Refused: a silent reference.",
         Capability::EditSequence,
         withSequence({{"reference", referenceParam()},
                       {"referenceFrom", timeParam("Use the reference from this time.")},
                       {"referenceTo", timeParam("Use the reference up to this time.")},
                       {"clips", clipsParam},
                       {"track", trackParam("Or match this audio track.")},
                       {"from", timeParam("Measure only from this time of the target.")},
                       {"to", timeParam("Measure only up to this time of the target.")},
                       {"mode", schema::enumeration({"loudness", "peak"}, "What to match. Default loudness.")},
                       {"headroom", schema::number("The true peak the result may not pass, dB under full scale. Default 1.", 0.0, 24.0)},
                       {"limit", schema::boolean("Add a true-peak limiter at the headroom when the result passes it.")},
                       {"tolerance", schema::number("Stop within this many dB of the reference. Default 0.1.", 0.01, 3.0)},
                       {"maxPasses", schema::integer("Measure-and-adjust passes at most. Default 4.", 1, 8)}},
                      {"reference"}),
         Json{{"reference", Json{{"clip", 12}}}, {"clips", Json::array({15})}, {"limit", true}}, {}, levelMatch});
}

} // namespace wabcut::audio::edit
