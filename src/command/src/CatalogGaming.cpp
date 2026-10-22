// Gaming edit commands (C.16, over GamingOps.h): clip.instantReplay, clip.zoomPunch, clip.shake, sequence.removeDeadTime
// and sequence.makeVersion. Each is one undo step; the timeline ones are EditSequence (the assistant and MCP clients see
// them), sequence.makeVersion adds a sequence (EditProject).

#include "Catalog.h"

#include <wabcut/model/BuiltinTemplates.h>
#include <wabcut/model/GamingOps.h>
#include <wabcut/model/Resolve.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace wabcut::command::detail {

namespace {

/// A duration param in seconds (numbers are what people and assistants say: "half a second").
Json secondsParam(std::string description, double minimum, double maximum) {
    return schema::number(std::move(description) + " Seconds.", minimum, maximum);
}

Ticks secondsValue(ExecutionContext& c, std::string_view key, Ticks fallback) {
    if (!c.has(key)) {
        return fallback;
    }
    const double s = c.number(key);
    if (!std::isfinite(s) || s < 0.0 || s > 86400.0) {
        fail(ErrorCode::InvalidParams, std::string(key) + " must be 0..86400 seconds");
    }
    return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))};
}

double numberValue(ExecutionContext& c, std::string_view key, double fallback) { return c.has(key) ? c.number(key) : fallback; }

/// A timeline time snapped to the start of its frame.
Ticks frameTime(ExecutionContext& c, Ticks t) { return snapToFrame(t, c.sequence().frameRate); }

Json rangeJson(ExecutionContext& c, TimeRange r) {
    return Json{{"start", c.timecode(r.start)},
                {"end", c.timecode(r.end())},
                {"startTicks", r.start.value},
                {"endTicks", r.end().value},
                {"seconds", static_cast<double>(r.duration.value) / static_cast<double>(kTicksPerSecond)}};
}

Json idList(const std::vector<ClipId>& ids) {
    Json out = Json::array();
    for (const ClipId id : ids) {
        out.push_back(id.value);
    }
    return out;
}

ErrorCode modelError(const wabcut::Error& e) {
    return e.code == wabcut::ErrorCode::NotFound ? ErrorCode::NotFound : ErrorCode::EditRejected;
}

std::optional<Interpolation> easingValue(ExecutionContext& c) {
    if (!c.has("easing")) {
        return std::nullopt;
    }
    const std::string e = c.string("easing");
    if (e == "linear") {
        return Interpolation::Linear;
    }
    if (e == "easeIn") {
        return Interpolation::EaseIn;
    }
    if (e == "easeOut") {
        return Interpolation::EaseOut;
    }
    if (e == "easeInOut") {
        return Interpolation::EaseInOut;
    }
    fail(ErrorCode::InvalidParams, "easing must be linear, easeIn, easeOut or easeInOut");
}

Json pointSchema(std::string description) {
    return schema::described(schema::object({{"x", schema::number("Across, a fraction of the frame width.", 0.0, 1.0)},
                                             {"y", schema::number("Down, a fraction of the frame height.", 0.0, 1.0)}},
                                            {"x", "y"}),
                             std::move(description));
}

// ---- Dead time ----------------------------------------------------------------------------------------------------------

/// Media activity for the listened tracks' media, measured once each through the host.
class ActivityCache {
public:
    ActivityCache(ExecutionContext& c, const ActivityServices& services) : m_c(c), m_services(services) {}

    /// Measures every media item on `tracks` (once); returns the ones that failed with why.
    std::vector<std::pair<MediaId, std::string>> measure(const Sequence& seq, const std::vector<TrackId>& tracks) {
        std::vector<MediaId> todo;
        for (const TrackId id : tracks) {
            const Track* t = seq.findTrack(id);
            if (!t) {
                continue;
            }
            for (const Clip& clip : t->clips) {
                // 0.2: a nested or multicam clip is measured through the media it plays (its active angles).
                for (const MediaId media : mediaPlayedBy(m_c.state(), clip)) {
                    const MediaItem* item = m_c.state().findMedia(media);
                    if (item && !item->isGenerator() && std::find(todo.begin(), todo.end(), media) == todo.end()) {
                        todo.push_back(media);
                    }
                }
            }
        }
        std::vector<std::pair<MediaId, std::string>> failed;
        for (std::size_t i = 0; i < todo.size(); ++i) {
            const auto progress = [&](double fraction) {
                const double overall = (static_cast<double>(i) + std::clamp(fraction, 0.0, 1.0)) / static_cast<double>(todo.size());
                ExecutionControl* control = m_c.control();
                return !control || !control->checkpoint("measuring activity (" + std::to_string(static_cast<int>(overall * 100.0)) + "%)");
            };
            auto measured = m_services.activity(m_c.state(), todo[i], progress);
            if (!measured) {
                if (measured.error().code == wabcut::ErrorCode::Cancelled) {
                    m_c.checkpoint("measuring activity"); // the caller stopped: report it as such
                }
                failed.emplace_back(todo[i], measured.error().message);
                continue;
            }
            if (const auto problem = mediaActivityProblem(*measured)) {
                failed.emplace_back(todo[i], *problem);
                continue;
            }
            m_activity.emplace(todo[i].value, std::move(*measured));
        }
        return failed;
    }

    const MediaActivity* find(MediaId id) const {
        const auto it = m_activity.find(id.value);
        return it == m_activity.end() ? nullptr : &it->second;
    }

private:
    ExecutionContext& m_c;
    const ActivityServices& m_services;
    std::map<std::uint64_t, MediaActivity> m_activity;
};

Json curveJson(const ActivityCurve& curve, std::size_t every) {
    Json values = Json::array();
    for (std::size_t i = 0; i < curve.values.size(); i += every) {
        values.push_back(std::round(curve.values[i] * 1000.0) / 1000.0);
    }
    return values;
}

/// Ranges from a "ranges" param (the preview's list, possibly edited), whole frames inside each.
std::vector<TimeRange> rangesParam(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    const Ticks frame = framesToTicks(1, seq.frameRate);
    std::vector<TimeRange> out;
    const Json& list = c.params()["ranges"];
    for (std::size_t i = 0; i < list.size(); ++i) {
        const std::string path = "/ranges/" + std::to_string(i);
        const Ticks start = c.timeValue(list[i]["start"], path + "/start");
        const Ticks end = c.timeValue(list[i]["end"], path + "/end");
        if (end <= start) {
            fail(ErrorCode::InvalidParams, "a range must end after it starts", Json{{"path", path}});
        }
        Ticks first = snapToFrame(start, seq.frameRate);
        if (first < start) {
            first += frame;
        }
        const Ticks last = snapToFrame(end, seq.frameRate);
        if (last > first) {
            out.push_back(TimeRange::fromStartEnd(first, last));
        }
    }
    return out;
}

CommandOutput removeDeadTime(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    const bool apply = c.boolean("apply", false);
    Json data;
    std::vector<TimeRange> remove;
    std::vector<std::string> warnings;
    if (c.has("ranges")) {
        if (!apply) {
            fail(ErrorCode::InvalidParams, "\"ranges\" are applied: set \"apply\": true (leave both out to preview)");
        }
        remove = rangesParam(c);
    } else {
        DeadTimeOptions options;
        options.minSeconds = numberValue(c, "minSeconds", options.minSeconds);
        options.sensitivity = numberValue(c, "sensitivity", options.sensitivity);
        options.handleSeconds = numberValue(c, "handleSeconds", options.handleSeconds);
        options.tracks = c.tracks("tracks");
        if (c.boolean("inOut", false)) {
            const auto marked = seq.markedRange();
            if (!marked) {
                fail(ErrorCode::InvalidParams, "\"inOut\" needs an In and an Out mark");
            }
            options.range = *marked;
        }
        if (const auto problem = deadTimeOptionsProblem(options)) {
            fail(ErrorCode::InvalidParams, *problem);
        }
        const auto* services = findService<ActivityServices>(c.services());
        if (!services) {
            fail(ErrorCode::InvalidRequest,
                 "finding dead time is not available here: the host cannot measure media activity (the app and wabcut-cli "
                 "can); pass the \"ranges\" to remove instead");
        }
        ActivityCache cache(c, *services);
        const std::vector<TrackId> tracks = options.tracks.empty() ? defaultActivityTracks(seq) : options.tracks;
        for (const auto& [media, why] : cache.measure(seq, tracks)) {
            const MediaItem* item = c.state().findMedia(media);
            warnings.push_back("'" + (item ? item->name : std::to_string(media.value)) + "' could not be measured (" + why +
                               "): its time is kept");
        }
        auto found = findDeadTime(c.state(), seq.id, [&](MediaId id) { return cache.find(id); }, options);
        if (!found) {
            fail(modelError(found.error()) == ErrorCode::NotFound ? ErrorCode::NotFound : ErrorCode::InvalidParams,
                 found.error().message);
        }
        Json ranges = Json::array();
        Ticks total{};
        for (const DeadTimeRange& r : found->ranges) {
            Json entry = rangeJson(c, r.remove);
            entry["stretch"] = rangeJson(c, r.stretch);
            entry["activity"] = std::round(r.activity * 1000.0) / 1000.0;
            ranges.push_back(std::move(entry));
            remove.push_back(r.remove);
            total += r.remove.duration;
        }
        Json tracksUsed = Json::array();
        for (const TrackId id : found->tracks) {
            tracksUsed.push_back(id.value);
        }
        data = Json{{"ranges", std::move(ranges)},
                    {"removeSeconds", static_cast<double>(total.value) / static_cast<double>(kTicksPerSecond)},
                    {"threshold", found->threshold},
                    {"tracks", std::move(tracksUsed)}};
        if (c.boolean("includeCurve", false)) {
            // At most ~2000 points: enough to draw it.
            const std::size_t every = std::max<std::size_t>(1, found->combined.values.size() / 2000);
            data["curve"] = Json{{"start", found->combined.start.value},
                                 {"step", found->combined.step.value * static_cast<std::int64_t>(every)},
                                 {"activity", curveJson(found->combined, every)},
                                 {"audio", curveJson(found->audio, every)},
                                 {"motion", curveJson(found->motion, every)}};
        }
    }
    CommandOutput out;
    if (!apply) {
        out.state = c.state();
        data["applied"] = false;
        out.data = std::move(data);
        out.warnings = std::move(warnings);
        return out;
    }
    Ticks removed{};
    for (const TimeRange& r : remove) {
        removed += r.duration;
    }
    edit::EditResult r = c.take(edit::removeTimeRanges(c.state(), seq.id, remove, c.boolean("rippleMarkers", true)));
    if (data.is_null()) {
        data = Json::object();
        Json ranges = Json::array();
        for (const TimeRange& range : remove) {
            ranges.push_back(rangeJson(c, range));
        }
        data["ranges"] = std::move(ranges);
    }
    data["applied"] = true;
    data["removedSeconds"] = static_cast<double>(removed.value) / static_cast<double>(kTicksPerSecond);
    out = edited(std::move(r), std::move(data));
    out.warnings = std::move(warnings);
    return out;
}

} // namespace

void registerGamingCommands(CommandRegistry& registry) {
    addBuiltin(
        registry,
        {"clip.instantReplay", 1, "Instant Replay",
         "Replays a moment right after itself in slow motion, Premiere-style instant replay in one step: the moment's clips "
         "are copied and inserted right after it (everything later moves, sync-locked tracks stay in sync) with a speed "
         "ramp (100 % easing into \"speed\" and back), the REPLAY stinger template over the replay and a white flash at "
         "each cut. The moment is a video clip (\"clip\": its span, and its linked clips), or a range (\"in\"/\"out\", "
         "default the sequence In/Out marks: every clip of media overlapping it). Graphics and titles are not replayed. "
         "The replay is silent unless \"includeAudio\" (slowed, the pitch drops). One undo step.",
         Capability::EditSequence,
         withSequence({{"clip", clipParam("The moment: this video clip's span.")},
                       {"in", timeParam("The moment's start (instead of a clip). Default the sequence In mark.")},
                       {"out", timeParam("The moment's end, exclusive. Default the sequence Out mark (its frame included).")},
                       {"speed", schema::number("Slow-motion speed (0.5 = half speed). Default 0.5.", kMinReplaySpeed, kMaxReplaySpeed)},
                       {"ramp", schema::boolean("Ease from 100 % into the slow speed and back out (time remapping).", true)},
                       {"label", schema::boolean("Lay the REPLAY stinger over the replay.", true)},
                       {"labelTemplate", schema::string("The stinger template's id (template.list). Default \"" +
                                                        std::string(kReplayTemplateId) + "\".")},
                       {"labelText", schema::string("The stinger's text, when its template has a \"label\" field.")},
                       {"flash", schema::boolean("A dip-to-white flash at the cut into the replay and out of it.", true)},
                       {"flashSeconds", secondsParam("Length of each flash. Default 0.2.", 0.04, 2.0)},
                       {"includeAudio", schema::boolean("Replay the audio too, slowed. Default: the replay is silent.", false)},
                       {"rippleMarkers", schema::boolean("Sequence markers after the moment move with the material.", true)}}),
         Json{{"clip", 42}, {"speed", 0.5}}, {},
         [](ExecutionContext& c) {
             const Sequence& seq = c.sequence();
             ReplayOptions options;
             if (c.has("clip")) {
                 if (c.has("in") || c.has("out")) {
                     fail(ErrorCode::InvalidParams, "give a clip or in/out, not both");
                 }
                 options.clip = c.clip("clip");
             } else {
                 const auto in = c.has("in") ? std::optional<Ticks>(frameTime(c, c.time("in"))) : seq.markIn;
                 std::optional<Ticks> out;
                 if (c.has("out")) {
                     out = frameTime(c, c.time("out"));
                 } else if (seq.markOut) {
                     out = *seq.markOut;
                 }
                 if (!in || !out) {
                     fail(ErrorCode::InvalidParams, "give a clip, or in and out (or set the sequence In and Out marks)");
                 }
                 if (*out <= *in) {
                     fail(ErrorCode::InvalidParams, "the moment must end after it starts");
                 }
                 options.range = TimeRange::fromStartEnd(*in, *out);
             }
             options.speed = numberValue(c, "speed", options.speed);
             options.ramp = c.boolean("ramp", options.ramp);
             options.flash = c.boolean("flash", options.flash);
             options.flashDuration = secondsValue(c, "flashSeconds", Ticks{});
             options.includeAudio = c.boolean("includeAudio", options.includeAudio);
             options.rippleMarkers = c.boolean("rippleMarkers", options.rippleMarkers);
             std::optional<FoundTemplate> label;
             if (c.boolean("label", true)) {
                 label = findTemplate(c, c.string("labelTemplate", std::string(kReplayTemplateId)), {});
                 options.label = &label->tmpl;
                 if (c.has("labelText")) {
                     const auto field = std::find_if(label->tmpl.fields.begin(), label->tmpl.fields.end(),
                                                     [](const TemplateField& f) { return f.id == "label" && f.kind == FieldKind::Text; });
                     if (field == label->tmpl.fields.end()) {
                         fail(ErrorCode::InvalidParams, "the stinger template has no \"label\" text field");
                     }
                     FieldValue v;
                     v.text = c.string("labelText");
                     options.labelValues.emplace_back("label", std::move(v));
                 }
             }
             auto replay = edit::instantReplay(c.state(), seq.id, options);
             if (!replay) {
                 fail(modelError(replay.error()), replay.error().message);
             }
             Json flashes = Json::array();
             for (const TransitionId id : replay->flashes) {
                 flashes.push_back(id.value);
             }
             Json data{{"moment", rangeJson(c, replay->moment)},
                       {"replay", rangeJson(c, replay->replay)},
                       {"clips", idList(replay->clips)},
                       {"label", replay->label.isValid() ? Json(replay->label.value) : Json(nullptr)},
                       {"flashes", std::move(flashes)}};
             CommandOutput out = edited(std::move(replay->edit), std::move(data));
             out.warnings = std::move(replay->notes);
             return out;
         }});

    addBuiltin(
        registry,
        {"clip.zoomPunch", 1, "Zoom Punch",
         "A punch-in zoom on a moment of a video clip: Motion Scale keys ease in to \"scale\" times the clip's scale, hitting "
         "at \"at\", hold, and ease back out (\"attack\" and \"release\" are shares of \"seconds\"). With \"focus\" the "
         "zoom closes in on that point of the frame (Position keys too). Keys already inside the punch are replaced. One "
         "undo step; the keys stay editable.",
         Capability::EditSequence,
         withSequence({{"clip", clipParam("The video clip.")},
                       {"at", timeParam("When the punch hits (full zoom).")},
                       {"scale", schema::number("Zoom at the peak, a factor of the clip's scale. Default 1.3.", 1.01, 10.0)},
                       {"seconds", secondsParam("The whole punch. Default 0.5.", 0.02, 30.0)},
                       {"attack", schema::number("Share of the punch easing in. Default 0.2.", 0.05, 0.9)},
                       {"release", schema::number("Share easing back out (0 = cut back). Default 0.4.", 0.0, 0.9)},
                       {"easing", schema::enumeration({"linear", "easeIn", "easeOut", "easeInOut"}, "The keys' easing. Default easeInOut.")},
                       {"focus", pointSchema("The point the zoom closes in on. Default: the clip's anchor.")}},
                      {"clip", "at"}),
         Json{{"clip", 42}, {"at", "00:00:12:10"}, {"scale", 1.4}}, {},
         [](ExecutionContext& c) {
             ZoomPunchOptions o;
             o.scale = numberValue(c, "scale", o.scale);
             o.duration = secondsValue(c, "seconds", o.duration);
             o.attack = numberValue(c, "attack", o.attack);
             o.release = numberValue(c, "release", o.release);
             if (const auto easing = easingValue(c)) {
                 o.easing = *easing;
             }
             if (c.has("focus")) {
                 const Json& f = c.params()["focus"];
                 o.focus = std::array<double, 2>{f["x"].get<double>(), f["y"].get<double>()};
             }
             if (const auto problem = zoomPunchProblem(o)) {
                 fail(ErrorCode::InvalidParams, *problem);
             }
             const ClipId clip = c.clip("clip");
             auto r = edit::zoomPunch(c.state(), c.sequenceId(), clip, frameTime(c, c.time("at")), o);
             if (!r) {
                 fail(modelError(r.error()), r.error().message);
             }
             return edited(std::move(*r), Json{{"clip", clip.value}});
         }});

    addBuiltin(
        registry,
        {"clip.shake", 1, "Camera Shake",
         "A camera shake on a video clip from \"at\": Motion Position (and Rotation) keys of a seeded random pattern that "
         "decays over \"seconds\", plus a slight zoom so the clip's edges do not show (\"cover\"). The same request makes "
         "the same shake (\"seed\" picks another). Keys already inside the shake are replaced. One undo step.",
         Capability::EditSequence,
         withSequence({{"clip", clipParam("The video clip.")},
                       {"at", timeParam("When the shake starts (the impact).")},
                       {"intensity", schema::number("Largest offset in sequence pixels. Default 24.", 0.5, 1000.0)},
                       {"seconds", secondsParam("How long it shakes. Default 0.5.", 0.02, 30.0)},
                       {"frequency", schema::number("Direction changes per second. Default 14.", 1.0, 60.0)},
                       {"rotation", schema::number("Largest rotation in degrees (0 = none). Default 0.", 0.0, 45.0)},
                       {"seed", schema::integer("The pattern's seed (0 = from the clip and the time).", 0)},
                       {"cover", schema::boolean("Zoom in slightly so the edges stay hidden.", true)}},
                      {"clip", "at"}),
         Json{{"clip", 42}, {"at", "00:00:12:10"}, {"intensity", 30}}, {},
         [](ExecutionContext& c) {
             ShakeOptions o;
             o.intensity = numberValue(c, "intensity", o.intensity);
             o.duration = secondsValue(c, "seconds", o.duration);
             o.frequency = numberValue(c, "frequency", o.frequency);
             o.rotation = numberValue(c, "rotation", o.rotation);
             o.seed = c.has("seed") ? c.params()["seed"].get<std::uint64_t>() : 0;
             o.cover = c.boolean("cover", o.cover);
             if (const auto problem = shakeProblem(o)) {
                 fail(ErrorCode::InvalidParams, *problem);
             }
             const ClipId clip = c.clip("clip");
             auto r = edit::shake(c.state(), c.sequenceId(), clip, frameTime(c, c.time("at")), o);
             if (!r) {
                 fail(modelError(r.error()), r.error().message);
             }
             return edited(std::move(*r), Json{{"clip", clip.value}});
         }});

    addBuiltin(
        registry,
        {"sequence.removeDeadTime", 1, "Remove Dead Time",
         "Finds low-activity stretches of gameplay - game audio and picture motion both low (loading screens, walking, "
         "waiting) for at least \"minSeconds\" - and removes them with handles kept, like a silence cleanup. Without "
         "\"apply\" it only lists the ranges (the preview: nothing changes); review them, then call again with \"apply\": "
         "true (the same detection) or with the \"ranges\" to remove. Removing ripple-deletes on every unlocked track and "
         "the captions in one undo step; markers move with the material. Listens to the gameplay track (the lowest video "
         "track with media or a nested clip) and the unmuted audio tracks unless \"tracks\" says otherwise. Nested and "
         "multicam clips are measured through what they show and play (the active angle) and cut in place, staying "
         "nested / multicam. Media the host cannot measure is never cut.",
         Capability::EditSequence,
         withSequence({{"minSeconds", secondsParam("Shortest stretch that counts. Default 3.", 0.5, 3600.0)},
                       {"sensitivity", schema::number("0..1: higher finds more dead time. Default 0.5.", 0.0, 1.0)},
                       {"handleSeconds", secondsParam("Kept at each end of a stretch. Default 0.3.", 0.0, 5.0)},
                       {"tracks", tracksParam("Tracks to listen to and watch. Default: the gameplay track and the unmuted audio tracks.")},
                       {"inOut", schema::boolean("Only look between the sequence In and Out marks.", false)},
                       {"apply", schema::boolean("Remove the ranges (default: only list them).", false)},
                       {"ranges", schema::array(schema::object({{"start", timeParam("Range start.")},
                                                                {"end", timeParam("Range end (exclusive).")}},
                                                               {"start", "end"}),
                                                "With apply: exactly these ranges (the preview's, edited) instead of detecting.", 1)},
                       {"rippleMarkers", schema::boolean("Markers move with the material.", true)},
                       {"includeCurve", schema::boolean("Also return the activity curves (to draw them).", false)}}),
         Json{{"minSeconds", 4}, {"sensitivity", 0.6}}, {}, removeDeadTime});

    addBuiltin(
        registry,
        {"sequence.makeVersion", 1, "Make Vertical or Square Version",
         "Makes a 9:16 Shorts (1080x1920) or 1:1 square (1080x1080) version of a 16:9 gaming edit as a new sequence: with a "
         "facecam (a track named like a webcam - facecam, webcam, cam, camera -, a picture-in-picture made with "
         "layout.apply, \"facecamTrack\" or \"facecamRegion\") Shorts get the vertical split (gameplay on top, facecam "
         "below) and square versions the gameplay over a blurred fill with the facecam in a corner; without one, Auto "
         "Reframe when the host has it, else the gameplay fitted to the width over a blurred copy of itself. Captions move "
         "into the version's safe area. The 16:9 sequence is not changed. One undo step.",
         Capability::EditProject,
         withSequence({{"format", schema::enumeration({"shorts", "square"}, "The version. Default shorts.")},
                       {"method", schema::enumeration({"auto", "layout", "reframe", "blurFill"},
                                                      "How the gameplay is laid out. Default auto.")},
                       {"name", schema::string("The new sequence's name. Default \"<name> (Shorts)\" / \"(Square)\".")},
                       {"facecamTrack", trackParam("The facecam's video track (default: detected).")},
                       {"facecamRegion", schema::described(schema::object({{"x", schema::number("Left, a fraction.", 0.0, 1.0)},
                                                                           {"y", schema::number("Top, a fraction.", 0.0, 1.0)},
                                                                           {"width", schema::number("Width, a fraction.", 0.01, 1.0)},
                                                                           {"height", schema::number("Height, a fraction.", 0.01, 1.0)}},
                                                                          {"x", "y", "width", "height"}),
                                                           "Where the capture itself shows the webcam (no facecam track).")},
                       {"split", schema::number("Shorts: the gameplay's share of the height. Default 0.6.", 0.2, 0.8)},
                       {"facecamOnTop", schema::boolean("Shorts: the facecam above the gameplay.", false)},
                       {"corner", schema::enumeration({"topLeft", "topRight", "bottomLeft", "bottomRight"},
                                                      "Square: the facecam box's corner. Default bottomRight.")},
                       {"captions", schema::boolean("Move captions into the version's safe area.", true)}}),
         Json{{"format", "shorts"}}, {},
         [](ExecutionContext& c) {
             VersionOptions o;
             if (c.has("format")) {
                 const auto format = versionFormatFromString(c.string("format"));
                 if (!format) {
                     fail(ErrorCode::InvalidParams, "format must be shorts or square");
                 }
                 o.format = *format;
             }
             if (c.has("method")) {
                 const auto method = versionMethodFromString(c.string("method"));
                 if (!method) {
                     fail(ErrorCode::InvalidParams, "unknown method");
                 }
                 o.method = *method;
             }
             o.name = c.string("name");
             if (c.has("facecamTrack")) {
                 o.facecamTrack = c.track("facecamTrack");
             }
             if (c.has("facecamRegion")) {
                 const Json& r = c.params()["facecamRegion"];
                 o.facecamRegion = NormalizedRect{r["x"].get<double>(), r["y"].get<double>(), r["width"].get<double>(),
                                                  r["height"].get<double>()};
                 o.layout.facecamRegion = o.facecamRegion;
             }
             o.layout.split = numberValue(c, "split", o.layout.split);
             o.layout.facecamOnTop = c.boolean("facecamOnTop", o.layout.facecamOnTop);
             if (c.has("corner")) {
                 const auto corner = layoutCornerFromString(c.string("corner"));
                 if (!corner) {
                     fail(ErrorCode::InvalidParams, "unknown corner");
                 }
                 o.layout.corner = *corner;
             }
             if (const auto problem = layoutOptionsProblem(o.layout)) {
                 fail(ErrorCode::InvalidParams, *problem);
             }
             o.captions = c.boolean("captions", o.captions);
             if (const auto* services = findService<VersionServices>(c.services())) {
                 o.reframe = services->reframe();
             }
             auto version = edit::makeVersion(c.state(), c.sequenceId(), o);
             if (!version) {
                 fail(modelError(version.error()), version.error().message);
             }
             const Sequence& made = *version->state.findSequence(version->sequence);
             CommandOutput out;
             out.state = std::move(version->state);
             out.createdIds.push_back(version->sequence.value);
             out.data = Json{{"sequence", version->sequence.value},
                             {"name", made.name},
                             {"width", made.width},
                             {"height", made.height},
                             {"method", std::string(toString(version->method))},
                             {"facecamTrack", version->facecamTrack.isValid() ? Json(version->facecamTrack.value) : Json(nullptr)},
                             {"clipsLaidOut", version->clipsLaidOut},
                             {"captionTracks", version->captionTracks}};
             out.warnings = std::move(version->notes);
             return out;
         }});
}

} // namespace wabcut::command::detail
