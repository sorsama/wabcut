#pragma once

// Internal: the helpers the audio command files share (AudioCommands.cpp, EssentialSoundCommands.cpp): schema pieces,
// the host's AudioServices, loudness measuring and matching, the audio clips a request names, long jobs with progress,
// and the duck of one music clip.

#include <wabcut/audio/edit/AudioAnalysis.h>
#include <wabcut/audio/edit/Ducking.h>
#include <wabcut/audio/edit/Loudness.h>

#include <wabcut/command/Registry.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Schema.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectOps.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <format>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace wabcut::audio::edit::detail {

using command::Capability;
using command::CommandOutput;
using command::ErrorCode;
using command::ExecutionContext;
using command::fail;
using command::Json;
using wabcut::edit::EffectTarget;
using wabcut::edit::ParamRef;
namespace schema = command::schema;

// ---- Schema and parameter helpers ------------------------------------------------------------------------------

inline Json withSequence(std::initializer_list<std::pair<const char*, Json>> properties, std::vector<std::string> required = {}) {
    Json s = schema::object(properties, std::move(required));
    s["properties"]["sequence"] = schema::integer("Sequence id; only needed when the project has several.", 1);
    return s;
}

inline Json timeParam(std::string description) {
    return Json{{"type", Json::array({"integer", "string"})},
                {"description", description + " Timecode in the sequence's rate (\"00:01:02:03\") or ticks."}};
}

inline Json trackParam(std::string description) {
    return Json{{"type", Json::array({"integer", "string"})},
                {"description", description + " A label (\"A1\") or a track id."}};
}

inline Json secondsParam(std::string description, double minimum, double maximum) {
    return schema::number(std::move(description), minimum, maximum);
}

inline Ticks seconds(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

inline Ticks secondsOr(ExecutionContext& c, const char* key, Ticks fallback) { return c.has(key) ? seconds(c.number(key)) : fallback; }

inline CommandOutput edited(wabcut::edit::EditResult result, Json data = nullptr) {
    CommandOutput out;
    out.state = std::move(result.state);
    for (const EffectInstanceId id : result.createdEffects) {
        out.createdIds.push_back(id.value);
    }
    out.data = std::move(data);
    return out;
}

inline Json levelJson(double v) { return std::isfinite(v) ? Json(std::round(v * 100.0) / 100.0) : Json(nullptr); }

inline Json statsJson(const LoudnessStats& s) {
    return Json{{"integratedLufs", levelJson(s.integratedLufs)},
                {"loudnessRangeLu", levelJson(s.rangeLu)},
                {"truePeakDbtp", levelJson(s.truePeakDbtp)},
                {"samplePeakDbfs", levelJson(s.samplePeakDbfs)},
                {"maxMomentaryLufs", levelJson(s.maxMomentaryLufs)},
                {"maxShortTermLufs", levelJson(s.maxShortTermLufs)},
                {"seconds", std::round(s.duration.toSeconds() * 1000.0) / 1000.0}};
}

inline double round3(double v) { return std::round(v * 1000.0) / 1000.0; }

inline std::string utcNow() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", now);
}

inline const Sequence& sequenceIn(const ProjectState& state, SequenceId id) { return *state.findSequence(id); }

// ---- Services --------------------------------------------------------------------------------------------------

inline AudioServices& servicesOf(ExecutionContext& c, bool loudness, bool speech) {
    auto* services = command::findService<AudioServices>(c.services());
    if (services == nullptr || (loudness && !services->loudness) || (speech && !services->speech)) {
        fail(ErrorCode::InvalidRequest,
             "no audio analysis here: the host provides none (wabcut-cli and the app measure through the media)");
    }
    return *services;
}

inline LoudnessStats measureOrFail(ExecutionContext& c, AudioServices& services, const ProjectState& state,
                                   const LoudnessTarget& target, std::optional<TimeRange> range) {
    auto stats = services.loudness->measure(state, c.sequenceId(), target, range, c.control());
    if (!stats) {
        fail(stats.error().code == wabcut::ErrorCode::Cancelled ? ErrorCode::Cancelled : ErrorCode::EditRejected,
             "loudness measurement failed: " + stats.error().message);
    }
    return *stats;
}

/// A model Error as a command failure.
[[noreturn]] inline void failWith(const Error& e, const std::string& what) {
    const ErrorCode code = e.code == wabcut::ErrorCode::NotFound          ? ErrorCode::NotFound
                           : e.code == wabcut::ErrorCode::Cancelled       ? ErrorCode::Cancelled
                           : e.code == wabcut::ErrorCode::Unsupported     ? ErrorCode::EditRejected
                           : e.code == wabcut::ErrorCode::InvalidArgument ? ErrorCode::InvalidParams
                                                                          : ErrorCode::Internal;
    fail(code, what + ": " + e.message);
}

/// Runs a job with a progress callback that reports through checkpoint(); a stop requested there stops the job and is
/// rethrown once it returned.
template <class Job>
auto runJob(ExecutionContext& c, const std::string& what, Job job) {
    std::stop_source stop;
    std::exception_ptr stopped;
    int lastPercent = -1;
    auto result = job(stop.get_token(), [&](double fraction) {
        const int percent = static_cast<int>(std::lround(std::clamp(fraction, 0.0, 1.0) * 100.0));
        if (percent == lastPercent || stopped) {
            return;
        }
        lastPercent = percent;
        try {
            c.checkpoint(what + " (" + std::to_string(percent) + "%)");
        } catch (...) {
            stopped = std::current_exception();
            stop.request_stop();
        }
    });
    if (stopped) {
        std::rethrow_exception(stopped);
    }
    return result;
}

// ---- Loudness matching -----------------------------------------------------------------------------------------

/// Sets the normalisation gain of `site` in `state` to `gainDb` (adjusting the existing one or adding one at the
/// top of the stack, after an Essential Sound record).
inline ProjectState withGain(ExecutionContext& c, const ProjectState& state, const EffectTarget& site, double gainDb) {
    const EffectStack* stack = wabcut::edit::effectStackOf(sequenceIn(state, c.sequenceId()), site);
    const EffectInstance* gain = stack != nullptr ? normalizationGain(*stack) : nullptr;
    const std::vector<EffectParam> params{{"gain", ParamValue(gainDb)}};
    if (gain != nullptr) {
        return c.take(wabcut::edit::setEffect(state, c.sequenceId(), site, gain->id, wabcut::edit::EffectChange{std::nullopt, params}))
            .state;
    }
    const std::size_t at = stack != nullptr ? normalizationGainIndex(*stack) : 0;
    return c.take(wabcut::edit::addEffect(state, c.sequenceId(), site, std::string(kGainEffect), params, at)).state;
}

inline double currentGain(ExecutionContext& c, const ProjectState& state, const EffectTarget& site) {
    const EffectStack* stack = wabcut::edit::effectStackOf(sequenceIn(state, c.sequenceId()), site);
    const EffectInstance* gain = stack != nullptr ? normalizationGain(*stack) : nullptr;
    return gain != nullptr ? effectParameterAt(*gain, "gain", Ticks{}) : 0.0;
}

struct LoudnessMatch {
    ProjectState state;
    Json result;     ///< {"before", "after", "gainDb", "passes"} (or "gainDb" alone when there was nothing to measure)
    LoudnessStats now;
    bool measured = false;
    bool reached = false;
};

/// Measures `target` and moves `site`'s normalisation gain until the integrated loudness is within `tolerance` of
/// `lufs` (gain before nonlinear effects does not move loudness one for one: measured again after each step). With
/// `secant` (A.10, Essential Sound) each step after the first divides the error by the slope the last step measured
/// (loudness per dB of gain, kept within [0.2, 2]), so a compressor after the gain (a slope of 1/ratio above its
/// threshold) is matched in two or three passes instead of creeping up on the target.
inline LoudnessMatch matchLoudness(ExecutionContext& c, AudioServices& services, ProjectState state, const LoudnessTarget& target,
                                   const EffectTarget& site, double lufs, double tolerance, int maxPasses,
                                   std::optional<TimeRange> range, bool secant = false) {
    LoudnessMatch m;
    const LoudnessStats before = measureOrFail(c, services, state, target, range);
    m.result = Json{{"before", statsJson(before)}};
    if (!before.hasIntegrated()) {
        m.result["gainDb"] = levelJson(currentGain(c, state, site));
        m.state = std::move(state);
        return m;
    }
    m.measured = true;
    double gain = currentGain(c, state, site);
    LoudnessStats now = before;
    int passes = 0;
    double slope = 1.0; // loudness per dB of gain: 1 for a linear stack
    while (passes < maxPasses && std::fabs(lufs - now.integratedLufs) > tolerance) {
        const double next = nextNormalizationGainDb(gain, (lufs - now.integratedLufs) / slope);
        if (next == gain) {
            break; // at the Gain effect's limit
        }
        const double lastGain = gain;
        const double lastLufs = now.integratedLufs;
        gain = next;
        state = withGain(c, state, site, gain);
        ++passes;
        c.checkpoint("matching loudness");
        now = measureOrFail(c, services, state, target, range);
        if (secant && now.hasIntegrated() && std::isfinite(lastLufs)) {
            slope = std::clamp((now.integratedLufs - lastLufs) / (gain - lastGain), 0.2, 2.0);
        }
    }
    m.reached = std::fabs(lufs - now.integratedLufs) <= tolerance;
    m.now = now;
    m.result["gainDb"] = levelJson(gain);
    m.result["after"] = statsJson(now);
    m.result["passes"] = passes;
    m.state = std::move(state);
    return m;
}

// ---- Clips -----------------------------------------------------------------------------------------------------

/// The audio clips a request names: audio clips as they are, video clips by the audio clips linked to them. In
/// request order, each once.
inline std::vector<ClipId> audioClipsOf(ExecutionContext& c, const std::vector<ClipId>& requested) {
    const Sequence& seq = c.sequence();
    std::vector<ClipId> out;
    const auto add = [&](ClipId id) {
        if (std::ranges::find(out, id) == out.end()) {
            out.push_back(id);
        }
    };
    for (const ClipId id : requested) {
        const auto at = seq.findClip(id);
        if (!at) {
            fail(ErrorCode::NotFound, "no clip " + std::to_string(id.value) + " in the sequence");
        }
        const Clip& clip = at->clip();
        if (clip.stream.kind == StreamKind::Audio) {
            add(id);
            continue;
        }
        bool linked = false;
        if (clip.linkGroup.isValid()) {
            for (const TrackPtr& t : seq.audioTracks) {
                for (const Clip& other : t->clips) {
                    if (other.linkGroup == clip.linkGroup) {
                        add(other.id);
                        linked = true;
                    }
                }
            }
        }
        if (!linked) {
            fail(ErrorCode::InvalidParams, "clip " + std::to_string(id.value) + " has no audio", Json{{"path", "/clips"}});
        }
    }
    return out;
}

inline const EffectInstance* findEffect(const Clip& clip, std::string_view effect) {
    for (const EffectInstance& e : clip.effects()) {
        if (e.effect == effect) {
            return &e;
        }
    }
    return nullptr;
}

inline std::vector<TrackId> tracksOrRole(ExecutionContext& c, const char* key, TrackRole role) {
    if (c.has(key)) {
        return c.tracks(key);
    }
    std::vector<TrackId> out;
    for (const TrackPtr& t : c.sequence().audioTracks) {
        if (t->role == role) {
            out.push_back(t->id);
        }
    }
    return out;
}

inline constexpr std::string_view kEnhanceSpeechEffect = "wabcut.audio.enhanceSpeech";

/// Where a media stream's enhanced take stands: "ready", "stale" or "missing" (AudioCommands.cpp).
std::string takeState(const MediaItem& item, int stream, std::string_view model);
/// Renders (or finds) the enhanced take of each media stream `clips` play and records it on the media item (with
/// `render` false or no enhancer, only reports where each stands). The takes' report.
Json ensureEnhancedTakes(ExecutionContext& c, ProjectState& state, const std::vector<ClipId>& clips, SpeechEnhancer* enhancer,
                         bool render, const std::string& model);

/// Ducks one music clip of `state` under `regions` (its volume keyframes over `range`); the clip's report, or null when
/// nothing changed.
Json duckClip(ExecutionContext& c, ProjectState& state, ClipId id, const std::string& trackLabel,
              const std::vector<DuckRegion>& regions, const DuckingOptions& options, std::optional<TimeRange> range);

/// The duck regions' report.
Json duckRegionsJson(ExecutionContext& c, const std::vector<DuckRegion>& regions);

} // namespace wabcut::audio::edit::detail

namespace wabcut::audio::edit {

/// A.7 (EssentialSoundCommands.cpp): audio.essentialSound, audio.remix and audio.remixRender.
void registerEssentialSoundCommands(command::CommandRegistry& registry);
/// A.8 (VoiceOverCommands.cpp): track.setRecordArm, record.takes and record.useTake.
void registerVoiceOverCommands(command::CommandRegistry& registry);
/// A.9 (AudioChannelCommands.cpp): channel maps, split to mono, track formats, the 5.1 panner and master.
void registerChannelCommands(command::CommandRegistry& registry);

} // namespace wabcut::audio::edit
