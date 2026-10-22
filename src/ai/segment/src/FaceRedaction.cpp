#include <wabcut/ai/segment/FaceRedaction.h>

#include <wabcut/ai/analysis/AnalysisCommands.h>
#include <wabcut/ai/vision/Tracker.h>
#include <wabcut/command/Schema.h>
#include <wabcut/model/ProjectOps.h>

#include <algorithm>
#include <cmath>
#include <map>

namespace wabcut::ai::segment {

namespace {

std::unexpected<Error> failure(ErrorCode code, std::string message) { return std::unexpected(Error(code, std::move(message))); }

struct Sighting {
    Ticks time{};
    MatteBox box; ///< normalised
};

Keyframe key(Ticks time, double value) {
    Keyframe k;
    k.time = time;
    k.value = value;
    k.interpolation = Interpolation::Linear;
    return k;
}

/// The sightings worth a key: the first, the last, and every one that moved or grew past a small share of the face.
std::vector<Sighting> thinned(const std::vector<Sighting>& all) {
    std::vector<Sighting> out;
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (out.empty() || i + 1 == all.size()) {
            out.push_back(all[i]);
            continue;
        }
        const MatteBox& a = out.back().box;
        const MatteBox& b = all[i].box;
        const double size = std::max({a.width, a.height, 1e-3});
        const double moved = std::max({std::abs((a.x + a.width / 2) - (b.x + b.width / 2)), std::abs((a.y + a.height / 2) - (b.y + b.height / 2)),
                                       std::abs(a.width - b.width), std::abs(a.height - b.height)});
        if (moved > size * 0.04) {
            out.push_back(all[i]);
        }
    }
    return out;
}

} // namespace

std::expected<FaceRedactionResult, Error> detectFaceRedactions(SegmentServices& s, analysis::AnalysisProvider* shots, const MediaItem& media,
                                                               int videoStream, const FaceRedactionOptions& o, std::stop_token stop,
                                                               const std::function<bool(double, const std::string&)>& checkpoint) {
    if (videoStream < 0 || videoStream >= static_cast<int>(media.info.video.size())) {
        return failure(ErrorCode::InvalidArgument, "'" + media.name + "' has no such video stream");
    }
    if (!s.detector) {
        return failure(ErrorCode::Unsupported, "no face detector is installed here (Settings > AI > Local models: the subject detectors)");
    }
    if (!s.frames) {
        return failure(ErrorCode::Unsupported, "the host decodes no pictures");
    }
    auto frames = s.frames(media, videoStream);
    if (!frames) {
        return std::unexpected(frames.error());
    }
    FrameSource& source = **frames;
    const Ticks fd = source.frameDuration();
    const TimeRange all = source.frames();
    if (fd.value <= 0 || all.duration.value <= 0) {
        return failure(ErrorCode::InvalidArgument, "'" + media.name + "' has no frames");
    }
    TimeRange span = all;
    if (o.span.duration.value > 0) {
        const Ticks a = std::max(o.span.start, all.start), b = std::min(o.span.end(), all.end());
        if (b <= a) {
            return failure(ErrorCode::InvalidArgument, "the span lies outside the media");
        }
        span = TimeRange::fromStartEnd(a, b);
    }
    const double perSecond = std::clamp(o.samplesPerSecond, 1.0, 30.0);
    const std::int64_t step = std::max<std::int64_t>(1, std::llround(static_cast<double>(kTicksPerSecond) / perSecond / fd.value));
    const Ticks interval{fd.value * step};
    std::vector<Ticks> times;
    for (Ticks t = all.start + Ticks{(span.start - all.start).value / fd.value * fd.value}; t < span.end(); t = t + interval) {
        if (t >= span.start) {
            times.push_back(t);
        }
    }
    if (times.empty()) {
        times.push_back(span.start);
    }
    std::vector<Ticks> cuts;
    if (shots) {
        auto found = shots->shots(media, videoStream, analysis::ShotOptions{}, [&](double) { return !stop.stop_requested(); });
        if (!found) {
            return std::unexpected(found.error());
        }
        for (const analysis::ShotCut& c : found->cuts) {
            cuts.push_back(c.time);
        }
    }
    vision::TrackerOptions tracking;
    tracking.minHits = 1;  // fail closed: a face seen once is redacted
    tracking.matchIou = 0.2f;
    tracking.maxLost = std::max(2, static_cast<int>(std::lround(perSecond))); // a second without a sighting
    vision::ByteTracker tracker(tracking);
    std::map<int, std::vector<Sighting>> faces;
    FaceRedactionResult result;
    constexpr std::size_t kBatch = 8;
    const double aspect = source.aspect();
    std::size_t next = 0;
    std::size_t cut = 0;
    while (next < times.size()) {
        if (stop.stop_requested() || (checkpoint && !checkpoint(static_cast<double>(next) / times.size(), "Finding faces"))) {
            return failure(ErrorCode::Cancelled, "cancelled");
        }
        std::vector<search::Image> batch;
        std::vector<Ticks> at;
        for (; next < times.size() && batch.size() < kBatch; ++next) {
            auto picture = source.pictureAt(times[next], 640);
            if (!picture) {
                return std::unexpected(picture.error());
            }
            batch.push_back(naturalPicture(*picture, aspect, 640));
            at.push_back(times[next]);
        }
        auto found = s.detector->detect(batch, stop);
        if (!found) {
            return std::unexpected(found.error());
        }
        for (std::size_t i = 0; i < batch.size() && i < found->size(); ++i) {
            ++result.samples;
            while (cut < cuts.size() && cuts[cut] <= at[i]) {
                tracker.reset(); // a new shot: new faces
                ++cut;
            }
            std::vector<vision::Detection> detections;
            for (const vision::Detection& d : (*found)[i].faces) {
                if (d.box.h / static_cast<float>(batch[i].height) >= o.minFaceHeight) {
                    detections.push_back(d);
                }
            }
            result.detections += static_cast<int>(detections.size());
            const std::vector<int> ids = tracker.update(detections);
            for (std::size_t k = 0; k < detections.size() && k < ids.size(); ++k) {
                if (ids[k] < 0) {
                    continue;
                }
                const vision::Box b = vision::normalized(detections[k].box, static_cast<float>(batch[i].width), static_cast<float>(batch[i].height));
                const double x0 = std::clamp<double>(b.x, 0.0, 1.0), y0 = std::clamp<double>(b.y, 0.0, 1.0);
                const double x1 = std::clamp<double>(b.x + b.w, 0.0, 1.0), y1 = std::clamp<double>(b.y + b.h, 0.0, 1.0);
                if (x1 > x0 && y1 > y0) {
                    faces[ids[k]].push_back(Sighting{at[i], MatteBox{x0, y0, x1 - x0, y1 - y0}});
                }
            }
        }
    }
    // One redaction per face, in the order they first appear.
    std::vector<std::vector<Sighting>> ordered;
    for (auto& entry : faces) {
        ordered.push_back(std::move(entry.second));
    }
    std::ranges::sort(ordered, {}, [](const std::vector<Sighting>& v) { return v.front().time; });
    const Ticks half{interval.value / 2 + fd.value};
    const double grow = 1.0 + 2.0 * std::clamp(o.margin, 0.0, 1.0);
    for (std::size_t n = 0; n < ordered.size(); ++n) {
        const std::vector<Sighting> keys = thinned(ordered[n]);
        Redaction r;
        r.media = media.id;
        r.name = o.name + " " + std::to_string(n + 1);
        const Ticks from = std::max(span.start, ordered[n].front().time - half);
        const Ticks to = std::min(span.end(), ordered[n].back().time + half);
        r.source = TimeRange::fromStartEnd(from, std::max(to, from + fd));
        r.style = o.style;
        r.strength = o.strength;
        r.region.shape = MaskShape::Ellipse;
        r.region.name = r.name;
        Curve cx, cy, sx, sy;
        for (const Sighting& k : keys) {
            cx.keys.push_back(key(k.time, k.box.x + k.box.width / 2.0));
            cy.keys.push_back(key(k.time, k.box.y + k.box.height / 2.0));
            sx.keys.push_back(key(k.time, std::min(1.5, k.box.width * grow)));
            sy.keys.push_back(key(k.time, std::min(1.5, k.box.height * grow)));
        }
        r.region.set("center.x", ParamValue(std::move(cx)));
        r.region.set("center.y", ParamValue(std::move(cy)));
        r.region.set("size.x", ParamValue(std::move(sx)));
        r.region.set("size.y", ParamValue(std::move(sy)));
        r.region.set("feather", ParamValue(0.01));
        result.redactions.push_back(std::move(r));
    }
    return result;
}

// ---- redaction.detectFaces ------------------------------------------------------------------------------------------

namespace {

using command::CommandOutput;
using command::ExecutionContext;
using command::fail;
using command::Json;
namespace schema = command::schema;

Json clipParam() {
    const Json at{{"type", Json::array({"integer", "string"})}, {"description", "A time inside the clip: timecode or ticks."}};
    return Json{{"type", Json::array({"integer", "object"})},
                {"description", "A video clip (its media between the clip's In and Out): a clip id, or {\"track\", \"time\"}."},
                {"properties", Json{{"track", Json{{"type", Json::array({"string", "integer"})}, {"description", "\"V1\" or a track id."}, {"minimum", 1}}},
                                    {"time", at}}},
                {"required", Json::array({"track", "time"})},
                {"additionalProperties", false},
                {"minimum", 1}};
}

CommandOutput detectFaces(ExecutionContext& c) {
    auto* services = command::findService<SegmentServices>(c.services());
    if (!services) {
        fail(command::ErrorCode::InvalidRequest, "face redaction needs the face detector and the media's pictures: the host provides none "
                                                 "(wabcut-cli and the app do)");
    }
    const MediaItem* media = nullptr;
    FaceRedactionOptions o;
    if (c.has("clip")) {
        const ClipId id = c.clip("clip");
        const Sequence* seq = c.state().findSequence(c.sequenceId());
        const auto loc = seq ? seq->findClip(id) : std::nullopt;
        if (!loc) {
            fail(command::ErrorCode::NotFound, "clip " + std::to_string(id.value) + " does not exist");
        }
        const Clip& clip = loc->clip();
        if (clip.isNested() || clip.stream.kind != StreamKind::Video) {
            fail(command::ErrorCode::EditRejected, "redactions cover a media item's picture: give a video clip of a media item (a nested clip's "
                                                   "redactions go on the clips inside)");
        }
        media = c.state().findMedia(clip.media);
        const Ticks a = clip.sourceTimeAt(clip.start()), b = clip.sourceTimeAt(clip.end() - Ticks{1});
        o.span = TimeRange::fromStartEnd(std::min(a, b), std::max(a, b) + Ticks{1});
    } else {
        media = c.state().findMedia(MediaId{static_cast<std::uint64_t>(c.integer("media", 0))});
        if (!media) {
            fail(command::ErrorCode::InvalidParams, "give a \"media\" item or a \"clip\"", Json{{"path", "/media"}});
        }
        const Ticks in = c.has("in") ? c.time("in") : Ticks{};
        const Ticks out = c.has("out") ? c.time("out") : media->info.duration;
        if (out <= in) {
            fail(command::ErrorCode::InvalidParams, "Out must come after In", Json{{"path", "/out"}});
        }
        o.span = TimeRange::fromStartEnd(in, out);
    }
    if (!media || !media->info.hasVideo()) {
        fail(command::ErrorCode::EditRejected, "the media has no picture");
    }
    if (c.has("style")) {
        const auto style = redactionStyleFromString(c.string("style"));
        if (!style) {
            fail(command::ErrorCode::InvalidParams, "style is blur, mosaic or solid", Json{{"path", "/style"}});
        }
        o.style = *style;
    }
    if (c.has("strength")) {
        o.strength = c.number("strength");
    }
    if (c.has("samplesPerSecond")) {
        o.samplesPerSecond = c.number("samplesPerSecond");
    }
    if (c.has("minFaceHeight")) {
        o.minFaceHeight = c.number("minFaceHeight");
    }
    if (c.has("margin")) {
        o.margin = c.number("margin");
    }
    if (c.has("name")) {
        o.name = c.string("name");
    }
    auto* analysisServices = command::findService<analysis::AnalysisServices>(c.services());
    auto found = detectFaceRedactions(*services, analysisServices ? analysisServices->provider.get() : nullptr, *media, 0, o, {},
                                      [&c](double, const std::string& message) {
                                          c.checkpoint(message);
                                          return true;
                                      });
    if (!found) {
        fail(found.error().code == ErrorCode::Cancelled ? command::ErrorCode::Cancelled : command::ErrorCode::EditRejected,
             found.error().message);
    }
    ProjectState state = c.state();
    CommandOutput out;
    Json made = Json::array();
    for (Redaction& r : found->redactions) {
        const std::string name = r.name;
        const TimeRange source = r.source;
        auto added = c.take(edit::addRedaction(state, std::move(r)));
        state = std::move(added.state);
        out.createdIds.push_back(added.id.value);
        made.push_back(Json{{"redaction", added.id.value}, {"name", name}, {"in", source.start.value}, {"out", source.end().value}});
    }
    out.state = std::move(state);
    out.data = Json{{"media", media->id.value}, {"faces", made.size()}, {"redactions", std::move(made)}, {"samples", found->samples},
                    {"detections", found->detections}};
    if (found->redactions.empty()) {
        out.warnings.push_back("no face found: nothing was redacted");
    }
    return out;
}

} // namespace

void registerFaceRedactionCommands(command::CommandRegistry& registry) {
    Json params = schema::object(
        {{"media", schema::integer("The media item (with \"in\" / \"out\" in its time), or give a \"clip\".", 1)},
         {"clip", clipParam()},
         {"sequence", schema::integer("Sequence id of the clip; only needed when the project has several.", 1)},
         {"in", Json{{"type", Json::array({"integer", "string"})}, {"description", "Media time to start (default the start)."}}},
         {"out", Json{{"type", Json::array({"integer", "string"})}, {"description", "Media time to end (default the end)."}}},
         {"style", schema::enumeration({"blur", "mosaic", "solid"}, "How the faces are covered (default blur).")},
         {"strength", schema::number("Blur radius / mosaic cell, in frame heights (default 0.05).", 0.0, 0.5)},
         {"samplesPerSecond", schema::number("Pictures looked at per second (default 8).", 1.0, 30.0)},
         {"minFaceHeight", schema::number("Leave out faces smaller than this share of the frame height (default 0.02).", 0.0, 0.5)},
         {"margin", schema::number("Grow each face's box by this share of its size on every side (default 0.25).", 0.0, 1.0)},
         {"name", Json{{"type", "string"}, {"description", "The redactions' name (\"Face\": \"Face 1\", \"Face 2\"...)."}, {"maxLength", 100}}}},
        {});
    [[maybe_unused]] const auto added = registry.add(command::runsModel(
        {"redaction.detectFaces", 1, "Redact Faces",
         "Finds every face of a media item (or a clip's part of it) with the face detector on this computer, follows each one and "
         "covers it with its own source-anchored redaction (blur, mosaic or solid) that moves with it: every use of the media in "
         "every sequence is covered. Fail closed: a face seen once is redacted, with a margin. One undo step; dryRun previews it.",
         command::Capability::EditProject, std::move(params), Json{{"clip", 42}, {"style", "mosaic"}}, {}, detectFaces}));
}

} // namespace wabcut::ai::segment
