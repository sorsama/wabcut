#include <wabcut/ai/workflow/WorkflowCommands.h>

#include <wabcut/command/Schema.h>

#include <algorithm>
#include <stdexcept>

namespace wabcut::ai::workflow {

using command::CommandOutput;
using command::ErrorCode;
using command::ExecutionContext;
using command::fail;
using command::Json;
namespace schema = command::schema;

PodcastOptions podcastOptionsOf(ExecutionContext& c) {
    PodcastOptions o;
    if (c.has("cameras")) {
        o.cameras = c.mediaList("cameras");
    }
    if (c.has("mics")) {
        o.mics = c.mediaList("mics");
    }
    if (c.has("media")) {
        // Split: what has a picture is a camera, the rest are microphones.
        for (const MediaId m : c.mediaList("media")) {
            const MediaItem* item = c.state().findMedia(m);
            (item && item->info.hasVideo() ? o.cameras : o.mics).push_back(m);
        }
    }
    o.name = c.string("name", o.name);
    o.sync = c.string("sync", o.sync);
    if (c.has("enhanceStrength")) {
        o.enhanceStrength = c.number("enhanceStrength");
    }
    if (c.has("lufs")) {
        o.lufs = c.number("lufs");
    }
    o.fillers = c.boolean("fillers", o.fillers);
    if (c.has("silences")) {
        o.silences = c.number("silences");
    }
    o.switchPreset = c.string("switchPreset", o.switchPreset);
    if (c.has("angles")) {
        o.angles = c.params()["angles"];
    }
    o.captionPreset = c.string("captionPreset", o.captionPreset);
    if (c.has("maxChapters")) {
        o.maxChapters = static_cast<std::size_t>(c.integer("maxChapters"));
    }
    if (c.has("minChapterSeconds")) {
        o.minChapterSeconds = c.number("minChapterSeconds");
    }
    o.shorts = c.boolean("shorts", o.shorts);
    if (c.has("shortsCount")) {
        o.shortsCount = static_cast<std::size_t>(c.integer("shortsCount"));
    }
    if (c.has("skip")) {
        for (const Json& s : c.params()["skip"]) {
            if (const auto k = stepFromString(s.get<std::string>())) {
                o.skip.insert(*k);
            } else {
                fail(ErrorCode::InvalidParams, "unknown step \"" + s.get<std::string>() + "\"", Json{{"path", "/skip"}});
            }
        }
    }
    return o;
}

Json planToJson(const PodcastPlan& plan) {
    Json steps = Json::array();
    for (std::size_t i = 0; i < plan.steps.size(); ++i) {
        const StepPlan& s = plan.steps[i];
        Json row{{"step", std::string(toString(s.kind))}, {"index", i + 1}, {"title", s.title}, {"detail", s.detail},
                 {"enabled", s.enabled}, {"heavy", s.heavy}};
        if (!s.command.empty()) {
            row["command"] = s.command;
        }
        if (!s.reason.empty()) {
            row["reason"] = s.reason;
        }
        steps.push_back(std::move(row));
    }
    return Json{{"steps", std::move(steps)}, {"cost", plan.cost}, {"warnings", plan.warnings}};
}

namespace {

CommandOutput podcastCommand(ExecutionContext& c) {
    const PodcastOptions options = podcastOptionsOf(c);
    PodcastContext context = c.has("context") ? contextFromJson(c.params()["context"]) : PodcastContext{};
    if (options.cameras.empty() && !context.multicam.isValid()) {
        fail(ErrorCode::InvalidParams, "pass the camera recordings (\"cameras\" or \"media\"), or a \"context\" with the multicam source",
             Json{{"path", "/cameras"}});
    }
    const PodcastPlan plan = planPodcast(c.state(), options, context, c.services());
    CommandOutput out;
    out.state = c.state();
    out.data = planToJson(plan);
    out.warnings = plan.warnings;
    if (!c.boolean("run", false)) {
        out.data["context"] = contextToJson(context);
        return out;
    }
    std::vector<StepKind> only;
    if (c.has("steps")) {
        for (const Json& s : c.params()["steps"]) {
            const auto k = stepFromString(s.get<std::string>());
            if (!k) {
                fail(ErrorCode::InvalidParams, "unknown step \"" + s.get<std::string>() + "\"", Json{{"path", "/steps"}});
            }
            only.push_back(*k);
        }
    }
    // The host's services for the steps' commands (not owned here: they outlive this command).
    const std::shared_ptr<command::CommandServices> services(std::shared_ptr<void>{}, c.services());
    ProjectState state = c.state();
    Json results = Json::array();
    std::size_t done = 0;
    const std::size_t total = static_cast<std::size_t>(
        std::count_if(plan.steps.begin(), plan.steps.end(), [&](const StepPlan& s) { return s.enabled && (only.empty() || std::ranges::find(only, s.kind) != only.end()); }));
    for (const StepPlan& s : plan.steps) {
        const bool chosen = only.empty() || std::ranges::find(only, s.kind) != only.end();
        if (!chosen) {
            continue;
        }
        if (!s.enabled) {
            results.push_back(Json{{"step", std::string(toString(s.kind))}, {"status", "skipped"}, {"reason", s.reason}});
            continue;
        }
        c.checkpoint("Step " + std::to_string(done + 1) + " of " + std::to_string(total) + ": " + s.title + " (" +
                     std::to_string(total > 0 ? done * 100 / total : 0) + "%)");
        auto r = runStep(s.kind, state, context, options, services, c.control());
        if (!r) {
            // Nothing done yet, or cancelled: an error. Otherwise the steps before it stay (one undo step) and the run stops.
            if (done == 0 || r.error().code == ErrorCode::Cancelled) {
                fail(r.error().code == ErrorCode::Cancelled ? ErrorCode::Cancelled : ErrorCode::EditRejected,
                     "step \"" + s.title + "\" failed: " + r.error().message, Json{{"step", std::string(toString(s.kind))}, {"results", results}});
            }
            results.push_back(Json{{"step", std::string(toString(s.kind))}, {"status", "failed"}, {"message", r.error().message}});
            out.warnings.push_back("step \"" + s.title + "\" failed: " + r.error().message + "; the steps before it are done, the rest did not run");
            break;
        }
        state = std::move(r->state);
        results.push_back(Json{{"step", std::string(toString(s.kind))}, {"status", "done"}, {"report", r->report}, {"warnings", r->warnings}});
        out.warnings.insert(out.warnings.end(), r->warnings.begin(), r->warnings.end());
        ++done;
    }
    out.state = std::move(state);
    out.data["results"] = std::move(results);
    out.data["context"] = contextToJson(context);
    if (context.sequence.isValid()) {
        out.createdIds.push_back(context.sequence.value);
    }
    return out;
}

std::vector<std::string> stepNames() {
    std::vector<std::string> names;
    for (const StepKind k : allSteps()) {
        names.emplace_back(toString(k));
    }
    return names;
}

} // namespace

void registerWorkflowCommands(command::CommandRegistry& registry) {
    const auto mediaList = [](const char* description) {
        return schema::array(Json{{"type", Json::array({"integer", "string"})}, {"description", "A media item (id or name)."}}, description);
    };
    const Json stepEnum = Json{{"type", "string"}, {"enum", stepNames()}};
    Json params = schema::object(
        {{"cameras", mediaList("The camera recordings (the picture angles).")},
         {"mics", mediaList("The microphone recordings (audio only). Default: the cameras' own sound.")},
         {"media", mediaList("Or all recordings: those with a picture are cameras, the rest microphones.")},
         {"name", Json{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}, {"description", "The episode's name (default Podcast)."}}},
         {"sync", schema::enumeration({"audio", "timecode", "inPoints", "outPoints", "markers"}, "How the recordings are synced (default audio).")},
         {"enhanceStrength", schema::number("Enhance Speech strength 0..100 (default 70).", 0.0, 100.0)},
         {"lufs", schema::number("Loudness target (default -16, podcasts).", -36.0, -5.0)},
         {"fillers", schema::boolean("Remove filler words (default true).")},
         {"silences", schema::number("Shorten pauses longer than this many seconds (default 1; 0 keeps pauses).", 0.0, 30.0)},
         {"switchPreset", schema::enumeration({"calm", "interview", "energetic"}, "The camera switching preset (default calm).")},
         {"angles", schema::array(schema::object({{"angle", schema::integer("The angle: 1 = the first camera; the microphones come after the cameras.", 1, 64)},
                                                  {"role", schema::enumeration({"closeUp", "wide", "unknown"}, "What it shows.")},
                                                  {"speaker", Json{{"type", "string"}, {"maxLength", 128},
                                                                   {"description", "Whom a close-up shows: a speaker's name (\"Mic 1\": the first microphone's speaker) or key."}}}},
                                                 {"angle"}),
                                  "Who is on which camera, for the switching (default: the analysis guesses).")},
         {"captionPreset", Json{{"type", "string"}, {"maxLength", 64}, {"description", "The caption preset (default Podcast)."}}},
         {"maxChapters", schema::integer("At most this many chapters (default 12).", 2, 100)},
         {"minChapterSeconds", schema::number("A chapter lasts at least this long (default 60 s).", 10.0, 3600.0)},
         {"shorts", schema::boolean("Also make vertical shorts of the best moments (default false).")},
         {"shortsCount", schema::integer("How many shorts (default 3).", 1, 10)},
         {"skip", schema::array(stepEnum, "Steps to leave out.")},
         {"steps", schema::array(stepEnum, "Run only these steps (with \"run\").")},
         {"context", schema::object({{"multicam", schema::integer("Multicam source id.", 0)},
                                     {"sequence", schema::integer("Episode sequence id.", 0)},
                                     {"transcribed", schema::array(schema::integer("A media item the run transcribed.", 0), "What the run transcribed.")},
                                     {"youtubeChapters", Json{{"type", "string"}, {"maxLength", 20000}, {"description", "The YouTube chapters text."}}}},
                                    {}, "What earlier runs made (a previous result's \"context\", as it is).")},
         {"run", schema::boolean("Run the steps (default false: return the plan only).")}});
    params["properties"]["skip"]["maxItems"] = 10;
    params["properties"]["steps"]["maxItems"] = 10;
    params["properties"]["angles"]["maxItems"] = 16;
    if (auto r = registry.add(command::runsModel({"workflow.podcast", 1, "Podcast Workflow",
                               "The one-click podcast / interview edit, on this computer (local, free): sync the cameras and microphones (multicam by "
                               "audio), lay out the episode (the multicam picture, each microphone on its own dialogue track), transcribe, Enhance "
                               "Speech, loudness to -16 LUFS, remove fillers and long pauses, switch cameras to whoever speaks, captions, chapters from "
                               "the topics (and YouTube chapters), optionally shorts. Without \"run\" it returns the plan (every step, on or off and "
                               "why); with \"run\" it runs the enabled steps (or \"steps\"), as one undo step; \"context\" continues an earlier run.",
                               command::Capability::EditProject, std::move(params), Json{{"media", Json::array({3, 4, 5, 6})}}, {}, podcastCommand}));
        !r) {
        throw std::logic_error(r.error());
    }
}

} // namespace wabcut::ai::workflow
