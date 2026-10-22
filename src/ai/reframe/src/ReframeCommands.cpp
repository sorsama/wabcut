#include <wabcut/ai/reframe/ReframeCommands.h>

#include <wabcut/command/Schema.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace wabcut::ai::reframe {

using command::CommandOutput;
using command::ErrorCode;
using command::ExecutionContext;
using command::fail;
using command::Json;
namespace schema = command::schema;

namespace {

constexpr std::size_t kPreviewSamples = 60; ///< per clip
constexpr std::size_t kPreviewClips = 60;

double rounded(double v, double scale) { return std::round(v * scale) / scale; }

vision::SubjectProvider* providerOf(ExecutionContext& c) {
    auto* services = command::findService<ReframeServices>(c.services());
    return services && services->subjects ? services->subjects.get() : nullptr;
}

std::function<bool(double)> progressFor(ExecutionContext& c, std::string what) {
    return [&c, what = std::move(what)](double fraction) {
        char text[32];
        std::snprintf(text, sizeof text, " (%d%%)", static_cast<int>(std::lround(fraction * 100.0)));
        c.checkpoint(what + text);
        return true;
    };
}

/// The subjects of each media item, through the host's provider (progress and cancel through the command).
SubjectSource subjectSourceFor(ExecutionContext& c, std::vector<std::string>& warnings) {
    vision::SubjectProvider* provider = providerOf(c);
    if (!provider) {
        warnings.push_back("no subject analysis here (the host provides none): clips are centred");
        return {};
    }
    return [&c, provider](const MediaItem& media, int stream) {
        return provider->subjects(media, stream, progressFor(c, "finding subjects in '" + media.name + "'"));
    };
}

std::vector<std::pair<const char*, Json>> optionParams() {
    return {
        {"aspect", Json{{"type", "string"},
                        {"description", "Aspect of the new sequence: \"9:16\" (vertical), \"1:1\", \"4:5\", \"16:9\" or any \"W:H\". "
                                        "Default \"9:16\"."},
                        {"minLength", 3},
                        {"maxLength", 7}}},
        {"mode", schema::enumeration({"follow", "faces", "speaker", "anchor"},
                                     "What the frame follows: \"follow\" the main subject of each shot (default), keep all "
                                     "\"faces\" in, the \"speaker\" who is talking, or a fixed \"anchor\" point.")},
        {"fit", schema::enumeration({"crop", "letterbox", "blur"},
                                    "Where the subjects cannot fit: \"crop\" anyway (default), zoom out with \"letterbox\" "
                                    "bars, or zoom out over a \"blur\"red fill.")},
        {"smoothness", schema::number("0 = follow closely .. 1 = move only when needed. Default 0.5.", 0.0, 1.0)},
        {"anchor", Json{{"type", "array"},
                        {"description", "Anchor mode: the point to keep centred, [x, y] in 0..1 of the picture."},
                        {"items", Json{{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}}},
                        {"minItems", 2},
                        {"maxItems", 2}}},
        {"margin", schema::number("Room kept around a subject, as a fraction of the frame. Default 0.04.", 0.0, 0.4)},
        {"width", schema::integer("Output width in pixels (with height). Default: the source's short side kept.", 16, 8192)},
        {"height", schema::integer("Output height in pixels (with width).", 16, 8192)},
        {"preview", schema::boolean("Report the frame path of each clip for drawing (default true).", true)},
    };
}

/// The options in the params, over `base`.
ReframeOptions optionsOf(ExecutionContext& c, ReframeOptions base) {
    if (c.has("aspect")) {
        const auto aspect = aspectFromString(c.string("aspect"));
        if (!aspect) {
            fail(ErrorCode::InvalidParams, "\"aspect\" must look like \"9:16\"");
        }
        base.aspect = *aspect;
    }
    if (c.has("mode")) {
        base.mode = *reframeModeFromString(c.string("mode"));
    }
    if (c.has("fit")) {
        base.fit = *fitModeFromString(c.string("fit"));
    }
    if (c.has("smoothness")) {
        base.smoothness = c.number("smoothness");
    }
    if (c.has("margin")) {
        base.margin = c.number("margin");
    }
    if (c.has("anchor")) {
        const Json& anchor = c.params()["anchor"];
        base.anchorX = anchor[0].get<double>();
        base.anchorY = anchor[1].get<double>();
    }
    if (c.has("width") != c.has("height")) {
        fail(ErrorCode::InvalidParams, "\"width\" and \"height\" go together");
    }
    if (c.has("width")) {
        base.width = static_cast<int>(c.integer("width"));
        base.height = static_cast<int>(c.integer("height"));
    }
    if (c.has("name")) {
        base.name = c.string("name");
    }
    return base;
}

Json sampledPath(const ClipPlan& plan) {
    Json path = Json::array();
    Json subjects = Json::array();
    const std::size_t n = plan.times.size();
    const std::size_t step = std::max<std::size_t>(1, (n + kPreviewSamples - 1) / kPreviewSamples);
    for (std::size_t i = 0; i < n; i += step) {
        const double w = plan.windowWidth * plan.zoom[i];
        const double h = plan.windowHeight * plan.zoom[i];
        const double t = rounded(plan.times[i].toSeconds(), 1000.0);
        path.push_back(Json::array({t, rounded(plan.centreX[i] - w / 2.0, 1e4), rounded(plan.centreY[i] - h / 2.0, 1e4), rounded(w, 1e4),
                                    rounded(h, 1e4)}));
        if (plan.subjects[i]) {
            const vision::Box& b = *plan.subjects[i];
            subjects.push_back(Json::array({t, rounded(b.x, 1e4), rounded(b.y, 1e4), rounded(b.w, 1e4), rounded(b.h, 1e4)}));
        }
    }
    return Json{{"path", std::move(path)}, {"subjects", std::move(subjects)}};
}

CommandOutput finish(ExecutionContext& c, ReframeResult result, const ReframeOptions& options, std::vector<std::string> warnings) {
    CommandOutput out;
    out.data = reframeResultJson(result.state, result, options, c.boolean("preview", true));
    out.createdIds.push_back(result.sequence.value);
    out.warnings = std::move(warnings);
    out.warnings.insert(out.warnings.end(), result.warnings.begin(), result.warnings.end());
    out.state = std::move(result.state);
    return out;
}

void failFrom(const Error& error) {
    fail(error.code == wabcut::ErrorCode::NotFound          ? ErrorCode::NotFound
         : error.code == wabcut::ErrorCode::InvalidArgument ? ErrorCode::InvalidParams
         : error.code == wabcut::ErrorCode::Cancelled       ? ErrorCode::Cancelled
                                                            : ErrorCode::EditRejected,
         error.message);
}

CommandOutput createSequence(ExecutionContext& c) {
    const Sequence& source = c.sequence();
    const ReframeOptions options = optionsOf(c, ReframeOptions{});
    std::vector<std::string> warnings;
    auto result = reframeSequence(c.state(), source.id, options, subjectSourceFor(c, warnings));
    if (!result) {
        failFrom(result.error());
    }
    return finish(c, std::move(*result), options, std::move(warnings));
}

CommandOutput rerun(ExecutionContext& c) {
    const Sequence& derived = c.sequence();
    if (!derived.derivedFrom || derived.derivedFrom->generator != kGenerator) {
        fail(ErrorCode::InvalidParams, "sequence '" + derived.name + "' was not made by reframe.createSequence");
    }
    const SequenceDerivation& from = *derived.derivedFrom;
    if (!c.state().findSequence(from.source)) {
        fail(ErrorCode::EditRejected, "source missing: sequence " + std::to_string(from.source.value) + ", which '" + derived.name +
                                          "' was reframed from, is no longer in the project");
    }
    auto recorded = optionsFromJson(from.params);
    if (!recorded) {
        fail(ErrorCode::EditRejected, recorded.error());
    }
    const ReframeOptions options = optionsOf(c, *recorded);
    std::vector<std::string> warnings;
    auto result = reframeSequence(c.state(), from.source, options, subjectSourceFor(c, warnings), derived.id);
    if (!result) {
        failFrom(result.error());
    }
    return finish(c, std::move(*result), options, std::move(warnings));
}

CommandOutput subjectsCommand(ExecutionContext& c) {
    vision::SubjectProvider* provider = providerOf(c);
    if (!provider) {
        fail(ErrorCode::InvalidRequest, "subject analysis is not available here: the host provides none (wabcut-cli analyses media files)");
    }
    const MediaId id = c.media("media");
    const MediaItem* media = c.state().findMedia(id);
    if (media->info.video.empty() || media->isGenerator()) {
        fail(ErrorCode::InvalidParams, "'" + media->name + "' has no video to analyse");
    }
    const auto stream = static_cast<int>(c.integer("stream", 0));
    if (stream >= static_cast<int>(media->info.video.size())) {
        fail(ErrorCode::InvalidParams, "'" + media->name + "' has no video stream " + std::to_string(stream));
    }
    auto analysis = provider->subjects(*media, stream, progressFor(c, "finding subjects in '" + media->name + "'"));
    if (!analysis) {
        failFrom(analysis.error());
    }
    std::size_t withFaces = 0;
    std::size_t withPeople = 0;
    for (const vision::SubjectSample& s : analysis->samples) {
        withFaces += s.faces.empty() ? 0 : 1;
        withPeople += s.people.empty() ? 0 : 1;
    }
    const auto [faceTracks, personTracks] = vision::countTracks(*analysis);
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"media", media->id.value},
                    {"name", media->name},
                    {"duration", rounded((analysis->end - analysis->start).toSeconds(), 1000.0)},
                    {"shots", analysis->shotCount()},
                    {"samples", analysis->samples.size()},
                    {"samplesWithFaces", withFaces},
                    {"samplesWithPeople", withPeople},
                    {"faces", faceTracks},
                    {"people", personTracks},
                    {"width", analysis->frameWidth},
                    {"height", analysis->frameHeight},
                    {"models", analysis->models}};
    if (analysis->models.empty()) {
        out.warnings.push_back("no face or person detector here (models not downloaded, or no wabcut-embed): salient regions only");
    }
    return out;
}

void add(command::CommandRegistry& registry, command::CommandSpec spec) {
    [[maybe_unused]] const auto added = registry.add(std::move(spec));
}

} // namespace

Json reframeResultJson(const ProjectState& state, const ReframeResult& result, const ReframeOptions& options, bool preview) {
    const Sequence* seq = state.findSequence(result.sequence);
    Json clips = Json::array();
    std::size_t count = 0;
    for (const auto& [clipId, plan] : result.plans) {
        Json j{{"clip", clipId}, {"guide", plan.guide}, {"zoomedOut", plan.zoomedOut}, {"cuts", plan.cuts.size()}};
        if (seq) {
            if (const auto at = seq->findClip(ClipId{clipId})) {
                const Clip& clip = at->clip();
                j["name"] = clip.name();
                j["start"] = rounded(clip.start().toSeconds(), 1000.0);
                j["sourceIn"] = rounded(clip.sourceIn.toSeconds(), 1000.0);
                j["sourceOut"] = rounded(clip.sourceOut().toSeconds(), 1000.0);
                j["media"] = clip.media.value;
                if (clip.isNested()) {
                    j["nested"] = clip.sequence.value; // 0.2: a nested or multicam clip, reframed as one picture
                }
            }
        }
        if (preview && count++ < kPreviewClips) {
            j.update(sampledPath(plan));
        }
        clips.push_back(std::move(j));
    }
    Json data{{"sequence", result.sequence.value},
              {"clipsReframed", result.clipsReframed},
              {"keyframes", result.keyframes},
              {"aspect", toString(options.aspect)},
              {"mode", toString(options.mode)},
              {"fit", toString(options.fit)},
              {"smoothness", rounded(options.smoothness, 1000.0)},
              {"clips", std::move(clips)}};
    if (seq) {
        data["name"] = seq->name;
        data["width"] = seq->width;
        data["height"] = seq->height;
        if (seq->derivedFrom) {
            data["source"] = seq->derivedFrom->source.value;
        }
    }
    return data;
}

void registerReframeCommands(command::CommandRegistry& registry) {
    using command::Capability;
    add(registry,
        command::runsModel({"analysis.subjects", 1, "Find Subjects",
         "Finds the faces, people and salient regions of a media item over time, shot by shot (5-10 samples a second, "
         "denser while the picture moves; faces and people tracked within each shot). Reframing uses it; the result is "
         "cached per file, so running it again is cheap. Runs locally: nothing leaves the computer.",
         Capability::MediaAnalysis,
         schema::object({{"media", Json{{"type", Json::array({"integer", "string"})}, {"description", "Media id or exact name."}, {"minimum", 1}}},
                         {"stream", schema::integer("Video stream index (default 0).", 0, 15)}},
                        {"media"}),
         Json{{"media", 1}}, {}, subjectsCommand}));

    auto createParams = optionParams();
    createParams.push_back({"name", schema::string("Name of the new sequence. Default: the source's name and the aspect.")});
    Json create{{"type", "object"}, {"properties", Json::object()}, {"additionalProperties", false}};
    for (auto& [key, value] : createParams) {
        create["properties"][key] = std::move(value);
    }
    create["properties"]["sequence"] = schema::integer("The sequence to reframe; only needed when the project has several.", 1);
    add(registry,
        command::runsModel({"reframe.createSequence", 1, "Auto Reframe",
         "Makes a copy of a sequence at another aspect (\"9:16\" for vertical shorts, \"1:1\", \"4:5\", \"16:9\") whose video "
         "clips keep their subjects in frame: per shot, a smooth virtual camera (still, then steady pans) follows the main "
         "subject, all faces, the speaker or an anchor. The result is ordinary, editable Position/Scale keyframes on each "
         "clip (nothing is rendered); the source sequence is not changed. Run analysis.subjects first for long media "
         "(otherwise this runs it). The new sequence remembers its source, so reframe.rerun can make it again.",
         Capability::EditProject, create, Json{{"aspect", "9:16"}, {"mode", "follow"}}, {}, createSequence}));

    Json again{{"type", "object"}, {"properties", Json::object()}, {"additionalProperties", false}};
    for (auto& [key, value] : optionParams()) {
        again["properties"][key] = std::move(value);
    }
    again["properties"]["sequence"] = schema::integer("The reframed sequence to make again.", 1);
    add(registry,
        command::runsModel({"reframe.rerun", 1, "Reframe Again",
         "Makes a reframed sequence again from its source sequence (after the source was edited), with the parameters it "
         "was made with, or new ones given here. It replaces the reframed sequence's contents (its manual keyframe changes "
         "too); its id, name and bin stay. Fails when the source sequence was deleted.",
         Capability::EditProject, again, Json{{"sequence", 2}}, {}, rerun}));
}

} // namespace wabcut::ai::reframe
