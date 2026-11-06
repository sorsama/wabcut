// effect.aiStatus and effect.processAi (VideoCommands.h).

#include <wabcut/ai/video/VideoCommands.h>

#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/video/Targets.h>
#include <wabcut/ai/video/VideoModels.h>
#include <wabcut/command/Schema.h>

#include <algorithm>
#include <cmath>

namespace wabcut::ai::video {

using command::CommandOutput;
using command::ErrorCode;
using command::ExecutionContext;
using command::fail;
using command::Json;
namespace schema = command::schema;

namespace {

VideoServices* servicesOf(ExecutionContext& c) { return command::findService<VideoServices>(c.services()); }

std::filesystem::path rootOf(const VideoServices* services) {
    return services != nullptr && !services->modelRoot().empty() ? services->modelRoot() : ModelStore::defaultRoot();
}

int coveredOf(ProcessedCache* cache, const ClipTarget& t) {
    if (cache == nullptr || !t.identity.valid()) {
        return 0;
    }
    return static_cast<int>(std::min<std::size_t>(coveredCount(*cache, t), static_cast<std::size_t>(std::max(t.expected, 0))));
}

const char* kindName(JobKind kind) {
    return kind == JobKind::Depth  ? "depth"
           : kind == JobKind::Faces ? "faces"
           : kind == JobKind::Gaze  ? "gaze"
           : kind == JobKind::Interpolation ? "interpolation"
                                            : "pictures";
}

Json targetJson(const ClipTarget& t, ProcessedCache* cache, const ModelStore& store, bool helper) {
    const int covered = coveredOf(cache, t);
    Json models = Json::array();
    for (const Model m : modelsOf(t.chain)) {
        const ModelInfo* info = findModel(describe(m).storeId);
        models.push_back(Json{{"name", std::string(describe(m).name)},
                              {"installed", info != nullptr && store.isInstalled(*info)},
                              {"bytes", info != nullptr ? info->size : 0},
                              {"license", info != nullptr ? info->license : std::string()}});
    }
    const bool installed = modelsInstalled(t.chain, store);
    const char* state = covered >= t.expected && t.expected > 0 ? "ready"
                        : !helper                                ? "noHelper"
                        : !installed                             ? "needsModel"
                        : covered > 0                            ? "partial"
                                                                 : "notProcessed";
    return Json{{"clip", t.clip.value},
                {"kind", kindName(t.kind)},
                {"track", "V" + std::to_string(t.track + 1)},
                {"name", t.name},
                {"chain", aiChainLabel(t.chain)},
                {"state", state},
                {"frames", t.expected},
                {"processed", covered},
                {"outputWidth", t.width * t.chain.outputScale()},
                {"outputHeight", t.height * t.chain.outputScale()},
                {"models", std::move(models)},
                {"estimateSeconds", std::round(estimateSeconds(t.chain, t.width, t.height, std::max(0, t.expected - covered), cache, true))}};
}

const Clip* findClip(const Sequence& seq, ClipId id, int& track) {
    for (std::size_t i = 0; i < seq.videoTracks.size(); ++i) {
        for (const Clip& clip : seq.videoTracks[i]->clips) {
            if (clip.id == id) {
                track = static_cast<int>(i);
                return &clip;
            }
        }
    }
    return nullptr;
}

std::vector<ClipTarget> chosenTargets(ExecutionContext& c, bool onlyIncomplete, ProcessedCache* cache) {
    const Sequence& seq = c.sequence();
    std::vector<ClipTarget> out;
    if (c.has("clip")) {
        const ClipId id = c.clip("clip");
        int track = 0;
        const Clip* clip = findClip(seq, id, track);
        if (clip == nullptr) {
            fail(ErrorCode::NotFound, "that clip is not on a video track of the sequence");
        }
        auto target = clipTarget(c.state(), seq, *clip, track);
        if (!target) {
            fail(ErrorCode::InvalidParams, "AI effects work on video files: not on titles, graphics, still images or nested clips");
        }
        // AI Restore / Denoise pictures, AI Refocus's depth maps, AI Face Retouch's face maps and AI Frame Interpolation's pictures:
        // whatever the clip has.
        std::optional<ClipTarget> more[5] = {std::move(target), depthTarget(c.state(), seq, *clip, track),
                                             faceTarget(c.state(), seq, *clip, track), gazeTarget(c.state(), seq, *clip, track),
                                             interpolationTarget(c.state(), seq, *clip, track)};
        for (std::optional<ClipTarget>& t : more) {
            if (t && !t->chain.empty()) {
                out.push_back(std::move(*t));
            }
        }
        if (out.empty()) {
            fail(ErrorCode::InvalidParams, "the clip has no AI effect that changes the picture with its settings (AI Restore, AI Denoise, "
                                           "AI Refocus, AI Face Retouch, AI Eye Contact, or Time Interpolation set to AI Frame Interpolation on a retimed clip)");
        }
        return out;
    }
    for (ClipTarget& t : sequenceTargets(c.state(), seq, std::nullopt)) {
        if (!onlyIncomplete || coveredOf(cache, t) < t.expected) {
            out.push_back(std::move(t));
        }
    }
    return out;
}

CommandOutput aiStatus(ExecutionContext& c) {
    VideoServices* services = servicesOf(c);
    ProcessedCache* cache = services != nullptr ? services->cache() : nullptr;
    const ModelStore store(rootOf(services));
    const bool helper = services != nullptr && services->backend() != nullptr;
    Json list = Json::array();
    int pending = 0;
    for (const ClipTarget& t : chosenTargets(c, false, cache)) {
        Json j = targetJson(t, cache, store, helper);
        pending += j["state"] == "ready" ? 0 : 1;
        list.push_back(std::move(j));
    }
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"count", list.size()}, {"pending", pending}, {"clips", std::move(list)}};
    return out;
}

CommandOutput processAi(ExecutionContext& c) {
    VideoServices* services = servicesOf(c);
    if (services == nullptr || services->cache() == nullptr || services->backend() == nullptr || !services->sources()) {
        fail(ErrorCode::InvalidRequest, "this host cannot process AI effects (the app and wabcut-cli can, with the AI helper "
                                        "next to them)");
    }
    const ModelStore store(rootOf(services));
    std::vector<ClipTarget> targets = chosenTargets(c, true, services->cache());
    CommandOutput out;
    out.state = c.state();
    Json clips = Json::array();
    if (targets.empty()) {
        out.data = Json{{"processed", 0}, {"skipped", 0}, {"clips", std::move(clips)}, {"message", "every AI effect is already processed"}};
        return out;
    }
    for (const ClipTarget& t : targets) {
        if (!modelsInstalled(t.chain, store)) {
            fail(ErrorCode::InvalidRequest, "download the models first: " + missingModelsText(t.chain, store) +
                                                " (Effect Controls > the effect's header, or Settings > AI)");
        }
        if (t.path.empty()) {
            fail(ErrorCode::EditRejected, "the media of \"" + t.name + "\" is offline: relink it first");
        }
    }
    if (c.dryRun()) {
        for (const ClipTarget& t : targets) {
            clips.push_back(Json{{"clip", t.clip.value}, {"chain", aiChainLabel(t.chain)}, {"frames", t.expected}});
        }
        out.data = Json{{"processed", 0}, {"skipped", 0}, {"clips", std::move(clips)}, {"dryRun", true}};
        return out;
    }
    int total = 0;
    for (const ClipTarget& t : targets) {
        total += std::max(1, t.expected);
    }
    int doneBefore = 0;
    int made = 0;
    int skipped = 0;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const ClipTarget& t = targets[i];
        c.checkpoint("processing " + std::to_string(i + 1) + " of " + std::to_string(targets.size()) + ": " + t.name);
        std::unique_ptr<IFrameSource> source = services->sources()(t.path, t.containerStream);
        if (!source) {
            fail(ErrorCode::EditRejected, "cannot open the media of \"" + t.name + "\"");
        }
        Runner runner(*source, *services->backend(), *services->cache());
        Job job;
        job.kind = t.kind;
        job.times = t.times;
        job.framePeriod = t.framePeriod;
        job.identity = t.identity;
        job.chain = t.chain;
        job.range = t.range;
        job.expectedFrames = t.expected;
        job.sourceWidth = t.width;
        job.sourceHeight = t.height;
        job.provenance.chain = aiChainLabel(t.chain);
        job.provenance.modelVersion = t.identity.modelVersion;
        const int base = doneBefore;
        // A cancel arrives as the engine's checkpoint exception: it leaves the runner's loop, and what is stored stays stored.
        auto result = runner.run(
            job,
            [&](const Progress& p) {
                c.checkpoint("processing frame " + std::to_string(base + p.processed + p.skipped) + " of " + std::to_string(total));
            },
            {});
        if (!result) {
            fail(ErrorCode::EditRejected, "processing \"" + t.name + "\" stopped: " + result.error().message);
        }
        made += result->processed;
        skipped += result->skipped;
        doneBefore += std::max(1, t.expected);
        clips.push_back(Json{{"clip", t.clip.value},
                             {"chain", aiChainLabel(t.chain)},
                             {"frames", t.expected},
                             {"made", result->processed},
                             {"alreadyProcessed", result->skipped},
                             {"device", result->device}});
    }
    out.data = Json{{"processed", made}, {"skipped", skipped}, {"clips", std::move(clips)}};
    return out;
}

} // namespace

void registerVideoCommands(command::CommandRegistry& registry) {
    Json statusSchema = schema::object(
        {{"clip", schema::integer("Only this clip (default: every clip of the sequence with AI effects).", 1)}});
    statusSchema["properties"]["sequence"] = schema::integer("Sequence id; only needed when the project has several.", 1);
    command::CommandSpec status{
        "effect.aiStatus", 1, "AI Effects Status",
        "Where the AI video effects stand: AI Restore and AI Denoise (\"kind\": \"pictures\"), AI Refocus's depth maps "
        "(\"depth\"), AI Face Retouch's face maps (\"faces\"), AI Eye Contact's eye measurements (\"gaze\") and AI Frame Interpolation's in-between pictures (\"interpolation\"). "
        "For each video clip of the sequence "
        "with such an effect (or the one named), the chain they form (\"Restore 2x, Denoise\", \"Depth maps\", \"Frame "
        "interpolation\"), the models it needs and whether each is on this computer (size, licence), how many of the clip's "
        "pictures are processed, the output size and about how long the rest takes. States: ready, partial, notProcessed, "
        "needsModel, noHelper. A read: it changes nothing.",
        command::Capability::Read, std::move(statusSchema), Json::object(), {}, aiStatus};
    [[maybe_unused]] const auto addedStatus = registry.add(std::move(status));

    Json processSchema = schema::object(
        {{"clip", schema::integer("Only this clip (default: every clip of the sequence whose AI pictures are incomplete).", 1)}});
    processSchema["properties"]["sequence"] = schema::integer("Sequence id; only needed when the project has several.", 1);
    command::CommandSpec process{
        "effect.processAi", 1, "Process AI Effects",
        "Makes the pictures the AI video effects show: for one clip (or every clip of the sequence whose pictures are "
        "incomplete) the part of its media it uses goes through the chain's models - denoise, deblur, upscale, in that order, "
        "on this computer - and is stored in the processed cache, where playback and exports find it. Also AI Refocus's depth "
        "map, AI Face Retouch's face map and AI Eye Contact's eye measurements of every frame, and, for a retimed clip whose Time Interpolation is AI Frame Interpolation, the in-between "
        "picture of every output frame. Pictures already made are skipped, so a stopped run continues. Needs the models "
        "downloaded and the AI helper. A job with progress and cancel; it changes nothing in the project (no undo step). A "
        "dry run lists what it would process.",
        command::Capability::MediaAnalysis, std::move(processSchema), Json{{"clip", 42}}, {}, processAi};
    [[maybe_unused]] const auto addedProcess = registry.add(command::runsModel(std::move(process)));
}

} // namespace wabcut::ai::video
