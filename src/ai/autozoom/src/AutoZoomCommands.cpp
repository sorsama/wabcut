#include <wabcut/ai/autozoom/AutoZoomCommands.h>

#include <wabcut/ai/reframe/Reframe.h>
#include <wabcut/command/Schema.h>
#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace wabcut::ai::autozoom {

using command::CommandOutput;
using command::ErrorCode;
using command::ExecutionContext;
using command::fail;
using command::Json;
namespace schema = command::schema;

namespace {

double rounded(double v, double scale = 1000.0) { return std::round(v * scale) / scale; }

std::string readSidecarFile(const std::string& path) {
    const std::filesystem::path file(std::u8string(path.begin(), path.end()));
    std::string extension = pathToUtf8(file.extension());
    std::ranges::transform(extension, extension.begin(), [](char ch) { return static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); });
    if (extension != ".json" && extension != ".csv" && extension != ".txt" && extension != ".log") {
        fail(ErrorCode::InvalidParams, "\"sidecar\" must be a .json, .csv, .txt or .log file", Json{{"path", "/sidecar"}});
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec) {
        fail(ErrorCode::NotFound, "cannot read the cursor sidecar: " + ec.message(), Json{{"path", "/sidecar"}});
    }
    if (size > 128ull * 1024 * 1024) {
        fail(ErrorCode::InvalidParams, "the cursor sidecar is larger than 128 MiB", Json{{"path", "/sidecar"}});
    }
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

/// 0.2: the one screen recording a nested or multicam clip shows over its window (a multicam angle, the clip inside a
/// nest), at 100 % forwards.
const Clip& recordingInside(ExecutionContext& c, const Clip& clip) {
    const std::string name = "clip " + std::to_string(clip.id.value);
    const Track* track = reframe::nestedPictureTrack(c.state(), clip);
    if (!track) {
        fail(ErrorCode::InvalidParams, name + " shows layers of a nested sequence: auto zoom reads one recording (zoom the clip inside it)",
             Json{{"path", "/clip"}});
    }
    const Ticks from = std::min(clip.sourceIn, clip.sourceOut());
    const Ticks to = std::max(clip.sourceIn, clip.sourceOut());
    const Clip* found = nullptr;
    for (const Clip& leaf : track->clips) {
        if (!leaf.enabled || leaf.end() <= from || leaf.start() >= to) {
            continue;
        }
        if (found || leaf.isNested() || !leaf.media.isValid()) {
            fail(ErrorCode::InvalidParams, name + " shows several clips through its nested sequence: auto zoom reads one recording (zoom the clips inside it)",
                 Json{{"path", "/clip"}});
        }
        found = &leaf;
    }
    if (!found) {
        fail(ErrorCode::EditRejected, name + " shows no recording to zoom");
    }
    if (!found->speed.isUnity() || found->reversed || found->isRemapped() || found->isFreeze()) {
        fail(ErrorCode::InvalidParams, "the recording " + name + " shows is retimed inside its nested sequence: auto zoom needs it at 100 %",
             Json{{"path", "/clip"}});
    }
    return *found;
}

CommandOutput applyCommand(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    const ClipId clipId = c.clip("clip");
    auto placement = placementOf(c.state(), seq.id, clipId);
    if (!placement) {
        fail(placement.error().code == wabcut::ErrorCode::NotFound ? ErrorCode::NotFound : ErrorCode::InvalidParams, placement.error().message,
             Json{{"path", "/clip"}});
    }
    const Clip& clip = seq.findClip(clipId)->clip();
    // The recording analysed: the clip's own, or the one a nested or multicam clip shows (its activity is then moved into
    // the nested time and onto the nested canvas, and the zoom keys go on the nested clip itself).
    const Clip& recording = clip.isNested() ? recordingInside(c, clip) : clip;
    const MediaItem* recordingMedia = c.state().findMedia(recording.media);
    if (!recordingMedia || recording.stream.index < 0 || static_cast<std::size_t>(recording.stream.index) >= recordingMedia->info.video.size()) {
        fail(ErrorCode::NotFound, "the clip's media has no such video stream", Json{{"path", "/clip"}});
    }
    const MediaItem& media = *recordingMedia;
    ZoomOptions options;
    if (c.has("maxZoom")) {
        options.maxZoom = c.number("maxZoom");
    }
    if (c.has("hold")) {
        options.holdSeconds = c.number("hold");
    }
    if (c.has("idle")) {
        options.idleSeconds = c.number("idle");
    }
    if (c.has("transition")) {
        options.transitionSeconds = c.number("transition");
    }
    if (c.has("smoothness")) {
        options.smoothness = c.number("smoothness");
    }
    options.easing = easingFromString(c.string("easing", "easeInOut")).value_or(Easing::EaseInOut);
    options.clicks = c.boolean("clicks", true);
    options.typing = c.boolean("typing", true);
    options.dwell = c.boolean("dwell", true);
    const TimeRange source = TimeRange::fromStartEnd(clip.sourceIn, clip.sourceOut());
    // The recording's own time and picture size (a nested clip: the part of the recording its window shows).
    const reframe::DisplayGeometry recordingPicture =
        clip.isNested() ? reframe::displayGeometry(media.info.video[static_cast<std::size_t>(recording.stream.index)])
                        : reframe::DisplayGeometry{placement->pictureWidth, placement->pictureHeight};
    const TimeRange recordingSource =
        clip.isNested() ? TimeRange::fromStartEnd(recording.sourceIn + (std::max(recording.start(), source.start) - recording.start()),
                                                  recording.sourceIn + (std::min(recording.end(), source.end()) - recording.start()))
                        : source;
    const int pictureWidth = static_cast<int>(std::lround(recordingPicture.width));
    const int pictureHeight = static_cast<int>(std::lround(recordingPicture.height));
    auto* services = command::findService<AutoZoomServices>(c.services());
    const std::string detect = c.string("detect", "auto");

    std::optional<ActivityTrack> activity;
    std::vector<std::string> warnings;
    Json analysis = nullptr;
    const auto parsed = [&](std::expected<ActivityTrack, Error> track, const char* path) {
        if (!track) {
            fail(ErrorCode::InvalidParams, track.error().message, Json{{"path", path}});
        }
        return std::move(*track);
    };
    if (c.has("cursor")) {
        activity = parsed(parseCursorFile(c.string("cursor"), pictureWidth, pictureHeight), "/cursor");
    } else if (c.has("sidecar")) {
        if (!services || !services->readFiles) {
            fail(ErrorCode::PermissionDenied, "this client may not read files: pass the sidecar's content as \"cursor\"");
        }
        activity = parsed(parseCursorFile(readSidecarFile(c.string("sidecar")), pictureWidth, pictureHeight), "/sidecar");
    } else if (detect != "frames" && services && services->sidecar) {
        if (const auto content = services->sidecar(media)) {
            auto track = parseCursorFile(*content, pictureWidth, pictureHeight);
            if (track) {
                activity = std::move(*track);
            } else {
                warnings.push_back("the cursor sidecar next to the recording could not be read (" + track.error().message + "): detecting instead");
            }
        }
    }
    if (!activity && detect == "sidecar") {
        fail(ErrorCode::EditRejected, "no cursor sidecar for '" + media.name + "' (looked for <file>.cursor.json / .csv)");
    }
    if (!activity) {
        if (!services || !services->frames) {
            fail(ErrorCode::InvalidRequest, "frames are not available here: pass a cursor sidecar as \"cursor\"");
        }
        DetectOptions detectOptions;
        if (c.has("fps")) {
            detectOptions.fps = c.number("fps");
        }
        char key[160];
        std::snprintf(key, sizeof key, "%llu/%d/%lld/%lld/%.3f/%d", static_cast<unsigned long long>(media.id.value), recording.stream.index,
                      static_cast<long long>(recordingSource.start.value), static_cast<long long>(recordingSource.duration.value),
                      detectOptions.fps, detectOptions.width);
        if (auto remembered = services->cached(key)) {
            activity = std::move(*remembered);
            analysis = Json{{"cached", true}};
        } else {
            auto reader = services->frames(media, recording.stream.index);
            if (!reader) {
                fail(ErrorCode::EditRejected, "cannot read the frames of '" + media.name + "': " + reader.error().message);
            }
            DetectStats stats;
            auto detected = detectActivity(
                *reader, recordingSource, detectOptions,
                [&c](double f) {
                    char text[48];
                    std::snprintf(text, sizeof text, "looking for activity (%d%%)", static_cast<int>(std::lround(f * 100.0)));
                    c.checkpoint(text);
                    return true;
                },
                &stats);
            if (!detected) {
                fail(detected.error().code == wabcut::ErrorCode::Cancelled ? ErrorCode::Cancelled : ErrorCode::EditRejected,
                     "auto zoom analysis of '" + media.name + "' failed: " + detected.error().message);
            }
            activity = std::move(*detected);
            services->remember(key, *activity);
            analysis = Json{{"frames", stats.frames}, {"seconds", rounded(stats.seconds)}, {"framesPerSecond", rounded(stats.framesPerSecond, 10.0)}};
        }
    }
    if (clip.isNested()) {
        // From the recording's time and picture to what the nested clip shows: the nested time and canvas.
        const Sequence& inner = *c.state().findSequence(clip.sequence);
        const auto onCanvas = [&](Ticks mediaTime, double& x, double& y, double* w, double* h) {
            const Ticks nested = recording.start() + (mediaTime - recording.sourceIn);
            const reframe::CanvasPlacement on =
                reframe::canvasPlacement(recording, recordingPicture, inner.width, inner.height, recording.curveTimeAt(nested));
            x = on.offsetX + on.factorX * x;
            y = on.offsetY + on.factorY * y;
            if (w && h) {
                *w *= on.factorX;
                *h *= on.factorY;
            }
            return nested;
        };
        for (ActivityEvent& e : activity->events) {
            e.time = onCanvas(e.time, e.x, e.y, &e.width, &e.height);
        }
        for (CursorSample& s : activity->cursor) {
            s.time = onCanvas(s.time, s.x, s.y, nullptr, nullptr);
        }
    }
    c.checkpoint("planning the zoom");
    const ZoomPath path = planZoom(*activity, source, *placement, options);
    auto result = applyZoom(c.state(), seq.id, clipId, path, *placement, options);
    if (!result) {
        fail(ErrorCode::EditRejected, result.error().message);
    }
    warnings.insert(warnings.end(), result->warnings.begin(), result->warnings.end());
    if (result->replacedKeys > 0) {
        warnings.push_back("replaced " + std::to_string(result->replacedKeys) + " Position/Scale keyframes the clip had");
    }
    CommandOutput out;
    const Clip& edited = result->state.findSequence(seq.id)->findClip(clipId)->clip();
    out.data = zoomResultJson(edited, *result, *activity, c.boolean("preview", false));
    out.data["clip"] = clipId.value;
    out.data["options"] = Json::parse(optionsToJson(options));
    if (!analysis.is_null()) {
        out.data["analysis"] = std::move(analysis);
    }
    out.state = std::move(result->state);
    out.warnings = std::move(warnings);
    return out;
}

} // namespace

std::optional<ActivityTrack> AutoZoomServices::cached(const std::string& key) const {
    std::lock_guard lock(m_mutex);
    const auto it = m_cache.find(key);
    return it == m_cache.end() ? std::nullopt : std::optional(it->second);
}

void AutoZoomServices::remember(const std::string& key, ActivityTrack track) {
    std::lock_guard lock(m_mutex);
    if (m_cache.size() > 64) {
        m_cache.clear();
    }
    m_cache[key] = std::move(track);
}

Json zoomResultJson(const Clip& clip, const ZoomResult& result, const ActivityTrack& activity, bool preview, double previewFps) {
    const Placement& p = result.placement;
    const auto timeline = [&](Ticks source) -> double {
        const auto t = clip.timelineTimeOfCurveTime(source);
        return rounded((t ? *t : clip.start()).toSeconds());
    };
    Json segments = Json::array();
    for (const ZoomSegment& s : result.path.segments) {
        segments.push_back(Json{{"start", timeline(s.start)},
                                {"end", timeline(s.end)},
                                {"sourceStart", rounded(s.start.toSeconds())},
                                {"sourceEnd", rounded(s.end.toSeconds())},
                                {"zoom", rounded(s.zoom, 100.0)},
                                {"x", rounded(s.x)},
                                {"y", rounded(s.y)},
                                {"events", s.events},
                                {"reason", s.reason}});
    }
    Json counts = Json::object();
    for (const ActivityEvent& e : activity.events) {
        counts[std::string(toString(e.kind))] = counts.value(std::string(toString(e.kind)), 0) + 1;
    }
    Json out{{"origin", activity.origin},
             {"events", std::move(counts)},
             {"cursorSamples", activity.cursor.size()},
             {"segments", std::move(segments)},
             {"keyframes", result.keyframes},
             {"replacedKeys", result.replacedKeys},
             {"maxZoom", rounded(result.path.zoom.empty() ? 1.0 : *std::ranges::max_element(result.path.zoom), 100.0)},
             {"outsideFrame", rounded(maxOutside(clip, p), 10000.0)}};
    if (preview) {
        Json samples = Json::array();
        const double length = clip.sourceDuration().toSeconds();
        const std::size_t n = static_cast<std::size_t>(std::floor(length * previewFps)) + 1;
        for (std::size_t k = 0; k < n; ++k) {
            const Ticks t = clip.sourceIn + Ticks{static_cast<std::int64_t>(std::llround(static_cast<double>(k) / previewFps * static_cast<double>(kTicksPerSecond)))};
            if (t >= clip.sourceOut()) {
                break;
            }
            const double z = clip.properties().value(ParamId::Scale, t) / p.scale;
            const double wx = p.positionX - clip.properties().value(ParamId::PositionX, t) / z; // frame px
            const double wy = p.positionY - clip.properties().value(ParamId::PositionY, t) / z;
            // The window (frame px) as a part of the picture (unrotated placement).
            const double x = 0.5 + ((wx - p.positionX) / p.scale + p.anchorX) / p.pictureWidth;
            const double y = 0.5 + ((wy - p.positionY) / p.scale + p.anchorY) / p.pictureHeight;
            const double w = p.frameWidth / z / p.scale / p.pictureWidth;
            const double h = p.frameHeight / z / p.scale / p.pictureHeight;
            samples.push_back(Json::array({timeline(t), rounded(z, 1000.0), rounded(x), rounded(y), rounded(w), rounded(h)}));
        }
        out["path"] = std::move(samples);
        out["pathColumns"] = Json::array({"t", "zoom", "x", "y", "w", "h"});
        Json events = Json::array();
        for (const ActivityEvent& e : activity.events) {
            if (e.time >= clip.sourceIn && e.time < clip.sourceOut()) {
                events.push_back(Json{{"t", timeline(e.time)}, {"kind", toString(e.kind)}, {"x", rounded(e.x)}, {"y", rounded(e.y)}});
                if (events.size() >= 500) {
                    break;
                }
            }
        }
        out["activity"] = std::move(events);
    }
    return out;
}

void registerAutoZoomCommands(command::CommandRegistry& registry) {
    Json params{
        {"type", "object"},
        {"properties",
         Json{{"sequence", schema::integer("Sequence id; only needed when the project has several.", 1)},
              {"clip", Json{{"type", Json::array({"integer", "object"})},
                            {"description", "The screen-recording clip: a clip id, or {\"track\", \"time\"} for the clip there."},
                            {"properties", Json{{"track", Json{{"type", Json::array({"string", "integer"})},
                                                                {"description", "Track of the clip: \"V1\", ... or a track id."},
                                                                {"minimum", 1}}},
                                                {"time", Json{{"type", Json::array({"integer", "string"})},
                                                               {"description", "A time inside the clip: timecode or ticks."}}}}},
                            {"required", Json::array({"track", "time"})},
                            {"additionalProperties", false},
                            {"minimum", 1}}},
              {"maxZoom", schema::number("Largest zoom, 1.1..4 (default 2).", 1.1, 4.0)},
              {"hold", schema::number("Seconds to stay zoomed after the last click or keystroke there (default 1.5).", 0.2, 20.0)},
              {"idle", schema::number("Activity closer than this (seconds) in one area shares a zoom; longer quiet zooms out (default 2.5).", 0.2, 30.0)},
              {"transition", schema::number("Shortest zoom in / out / pan, in seconds (default 0.6).", 0.1, 5.0)},
              {"easing", schema::enumeration({"easeInOut", "linear", "smooth"}, "Key interpolation: ease in and out (default), linear, or the smooth optimised path.")},
              {"smoothness", schema::number("Camera smoothness, 0 (follows closely) .. 1 (moves only when it must); default 0.5.", 0.0, 1.0)},
              {"clicks", schema::boolean("Zoom on clicks (default true).", true)},
              {"typing", schema::boolean("Zoom on typing (default true).", true)},
              {"dwell", schema::boolean("Zoom where the cursor comes to rest (default true).", true)},
              {"detect", schema::enumeration({"auto", "sidecar", "frames"}, "Where activity comes from: a cursor sidecar if there is one, else the frames (default); only a sidecar; only the frames.")},
              {"fps", schema::number("Frames analysed per second when detecting (default 10).", 2.0, 30.0)},
              {"cursor", Json{{"type", "string"}, {"description", "A cursor sidecar's content: Wabcut JSON ({\"format\": \"wabcut.cursor\", ...}) or CSV t,x,y[,event]."}, {"minLength", 2}, {"maxLength", 16777216}}},
              {"sidecar", Json{{"type", "string"}, {"description", "A cursor sidecar file (only where the host allows reading files)."}, {"minLength", 1}, {"maxLength", 4096}}},
              {"preview", schema::boolean("Also return the window over time and the activity, for drawing (default false).", false)}}},
        {"required", Json::array({"clip"})},
        {"additionalProperties", false}};
    [[maybe_unused]] const auto added = registry.add(
        {"autozoom.apply", 1, "Auto Zoom",
         "Zooms a screen recording in where things happen (clicks, typing, where the cursor rests) and back out when nothing "
         "does, like Screen Studio: smooth, editable Position and Scale keyframes on the clip (the AI-9 camera optimiser), "
         "never showing outside the frame. Activity comes from a cursor sidecar when there is one, else from the frames. "
         "A multicam or nested clip is zoomed as one picture from the recording it shows (the active angle), keys on the "
         "clip itself. Running it again replaces its keys. One undo step; dryRun with \"preview\" returns the zoom path.",
         command::Capability::EditSequence, std::move(params), Json{{"clip", 42}, {"maxZoom", 2.0}, {"easing", "easeInOut"}}, {}, applyCommand});
}

} // namespace wabcut::ai::autozoom
