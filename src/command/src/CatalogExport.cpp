// 0.2 (export parity): the export.* commands - Quick Export, exports with several presets and destinations, the
// Media Encoder-style queue, user presets and watch folders - over the host's ExportServices (ExportServices.h). They
// never change the project: exports are queued after the request succeeded (CommandOutput::afterCommit), and a dry run
// only says what would be written.

#include "Catalog.h"

#include <wabcut/command/ExportServices.h>

#include <string>
#include <vector>

namespace wabcut::command::detail {

namespace {

ExportServices& exportServices(ExecutionContext& c) {
    auto* services = findService<ExportServices>(c.services());
    if (!services) {
        fail(ErrorCode::InvalidRequest, "this host has no export services (the app and wabcut-cli have them)");
    }
    return *services;
}

CommandOutput readOutput(ExecutionContext& c, Json data) {
    CommandOutput out;
    out.state = c.state();
    out.data = std::move(data);
    return out;
}

template <class T> T take(std::expected<T, CommandError> r) {
    if (!r) {
        throw CommandFailure{std::move(r.error())};
    }
    return std::move(*r);
}

void take(std::expected<void, CommandError> r) {
    if (!r) {
        throw CommandFailure{std::move(r.error())};
    }
}

Json pathSchema(std::string description) {
    Json s = schema::string(std::move(description));
    s["maxLength"] = 4096;
    return s;
}

Json overridesSchema() {
    Json s = schema::object(
        {}, {},
        "Settings on top of the preset (ExportController's keys, docs/export.md): container, videoCodec, encoder, quality, "
        "bitrateMbps, maxBitrateMbps, frameRate, width, height, scaleMode, range (whole | inOut | workArea | custom), "
        "video, audio, audioCodec, audioChannels, sampleRate, audioBitrateKbps, dynamicRange, captions, pixelFormat, "
        "alpha (Include Alpha Channel), proresProfile, dnxhrProfile, exportAsSequence, subtitles, and the objects "
        "effects, metadata and render (export.describe shows their fields).");
    s.erase("additionalProperties"); // open (absent), not "true": portable tool schemas allow no open objects
    return s;
}

/// The range params: "range" plus "in" / "out" (a custom range) folded into the overrides.
Json overridesOf(ExecutionContext& c) {
    Json overrides = c.has("overrides") ? c.params().at("overrides") : Json::object();
    if (!overrides.is_object()) {
        fail(ErrorCode::InvalidParams, "\"overrides\" must be an object", Json{{"path", "/overrides"}});
    }
    if (c.has("range")) {
        overrides["range"] = c.string("range");
    }
    const auto in = c.optionalTime("in");
    const auto out = c.optionalTime("out");
    if (in || out) {
        if (!in || !out || !(*in < *out) || in->value < 0) {
            fail(ErrorCode::InvalidParams, "a custom range needs \"in\" before \"out\"", Json{{"path", "/in"}});
        }
        overrides["customRange"] = Json{{"start", in->value}, {"end", out->value}};
    }
    return overrides;
}

/// Queues `outputs` after the request succeeded; the data names the reserved job ids and what each will write.
CommandOutput queueOutputs(ExecutionContext& c, std::vector<ExportServices::Output> outputs, bool hold) {
    ExportServices& services = exportServices(c);
    Json planned = Json::array();
    for (const ExportServices::Output& o : outputs) {
        planned.push_back(take(services.describe(c.state(), c.sequenceId(), o)));
    }
    CommandOutput out;
    out.state = c.state();
    if (c.dryRun()) {
        out.data = Json{{"queued", false}, {"exports", std::move(planned)}};
        return out;
    }
    std::vector<std::string> jobs = services.reserveJobs(outputs.size());
    for (std::size_t i = 0; i < planned.size() && i < jobs.size(); ++i) {
        planned[i]["job"] = jobs[i];
        // describe() resolved the default output name: the job writes exactly what it reported.
        outputs[i].output = planned[i].value("output", outputs[i].output);
    }
    auto snapshot = std::make_shared<const ProjectState>(c.state());
    const SequenceId sequence = c.sequenceId();
    out.afterCommit.push_back([&services, snapshot, sequence, outputs = std::move(outputs), jobs, hold]() mutable {
        services.queue(snapshot, sequence, std::move(outputs), std::move(jobs), hold);
    });
    out.data = Json{{"queued", true}, {"held", hold}, {"exports", std::move(planned)}};
    return out;
}

} // namespace

void registerExportCommands(CommandRegistry& registry) {
    const Json presetParam = schema::string("A preset id (export.presets: a built-in one such as \"match-source\", or a "
                                            "user preset \"user-...\").");
    const Json rangeParam = schema::enumeration({"whole", "inOut", "workArea", "custom"},
                                                "Premiere's Range: Entire Source (default), Source In/Out, Work Area, Custom.");
    const Json jobParam = schema::string("A job id from export.queue.list (\"j1\", ...).");

    addBuiltin(registry,
               {"export.presets", 1, "Export Presets",
                "The export presets: the built-in ones (YouTube, Shorts / TikTok / Reels, Twitch, Discord sizes, ProRes / "
                "DNxHR / DNxHD masters, ProRes 4444 / QuickTime Animation / CineForm with alpha, MP3 / AAC / AIFF / WAV "
                "audio, PNG / TIFF / DPX / JPEG / Targa / OpenEXR image sequences, HDR) and the user's saved ones: id, "
                "name, description, builtIn, and the full settings.",
                Capability::Read, schema::object({}), Json::object(), {},
                [](ExecutionContext& c) { return readOutput(c, Json{{"presets", exportServices(c).presets()}}); }});

    addBuiltin(registry,
               {"export.describe", 1, "Describe Export",
                "What an export of the sequence with a preset (and overrides) would write, without exporting: the output "
                "file, the resolved settings (container, codecs, size, frame rate, effects, metadata, ...), the range and "
                "its frame count.",
                Capability::Read,
                withSequence({{"preset", presetParam},
                              {"overrides", overridesSchema()},
                              {"output", pathSchema("The output file (default: next to the project, named after the sequence).")},
                              {"range", rangeParam},
                              {"in", timeParam("Custom range start (with \"out\").")},
                              {"out", timeParam("Custom range end.")}}),
                Json{{"preset", "youtube-1080p"}, {"overrides", Json{{"quality", 18}}}}, {},
                [](ExecutionContext& c) {
                    ExportServices::Output o{c.string("preset", "match-source"), overridesOf(c), c.string("output")};
                    return readOutput(c, take(exportServices(c).describe(c.state(), c.sequenceId(), o)));
                }});

    addBuiltin(registry,
               command::touchesFiles(
                   {"export.quick", 1, "Quick Export",
                    "Premiere's Quick Export: exports the sequence with one preset (default \"match-source\") to \"output\" "
                    "(default: next to the project, named after the sequence) in the background, through the export "
                    "queue. Returns the job and what it writes; export.queue.list follows it.",
                    Capability::Read,
                    withSequence({{"preset", presetParam},
                                  {"output", pathSchema("The output file; its extension follows the format.")},
                                  {"overrides", overridesSchema()},
                                  {"range", rangeParam},
                                  {"in", timeParam("Custom range start (with \"out\").")},
                                  {"out", timeParam("Custom range end.")}}),
                    Json{{"preset", "youtube-1080p60"}, {"output", "C:/Exports/Episode 12.mp4"}}, {},
                    [](ExecutionContext& c) {
                        return queueOutputs(c, {ExportServices::Output{c.string("preset", "match-source"), overridesOf(c),
                                                                       c.string("output")}},
                                            false);
                    }},
                   {"output"}));

    Json presetsList = schema::array(presetParam, "One export per preset (Premiere's several destinations).", 1);
    presetsList["maxItems"] = 32;
    Json outputsList = schema::array(pathSchema("An output file (\"\" = the default name)."),
                                     "The output file of each preset, in order (default: next to the project).");
    outputsList["maxItems"] = 32;
    addBuiltin(registry,
               command::touchesFiles(
                   {"export.queue", 1, "Send to Media Encoder",
                    "Queues exports of the sequence, one per preset in \"presets\" (to the matching \"outputs\"), with the "
                    "same overrides: Premiere's Export with several destinations and Send to Media Encoder. They run one "
                    "after another in the background. With \"hold\" they wait in the queue until export.queue.start (Media "
                    "Encoder's queue before Start Queue).",
                    Capability::Read,
                    withSequence({{"presets", presetsList},
                                  {"outputs", outputsList},
                                  {"overrides", overridesSchema()},
                                  {"range", rangeParam},
                                  {"in", timeParam("Custom range start (with \"out\").")},
                                  {"out", timeParam("Custom range end.")},
                                  {"hold", schema::boolean("Queue without starting (default false).")}},
                                 {"presets"}),
                    Json{{"presets", Json::array({"youtube-1080p60", "shorts-1080x1920-60"})}}, {},
                    [](ExecutionContext& c) {
                        const Json& presets = c.params().at("presets");
                        const Json outputs = c.has("outputs") ? c.params().at("outputs") : Json::array();
                        if (!outputs.empty() && outputs.size() != presets.size()) {
                            fail(ErrorCode::InvalidParams, "\"outputs\" needs one file per preset", Json{{"path", "/outputs"}});
                        }
                        const Json overrides = overridesOf(c);
                        std::vector<ExportServices::Output> list;
                        for (std::size_t i = 0; i < presets.size(); ++i) {
                            list.push_back({presets[i].get<std::string>(), overrides,
                                            outputs.empty() ? std::string() : outputs[i].get<std::string>()});
                        }
                        return queueOutputs(c, std::move(list), c.boolean("hold", false));
                    }},
                   {"outputs"}));

    addBuiltin(registry,
               {"export.queue.list", 1, "Export Queue",
                "The export queue: every job with its output, preset, state (held, queued, running, paused, done, failed, "
                "cancelled), progress 0..1 and detail (the result or the error).",
                Capability::Read, schema::object({}), Json::object(), {},
                [](ExecutionContext& c) { return readOutput(c, Json{{"jobs", exportServices(c).jobs()}}); }});

    struct Control {
        const char* name;
        const char* title;
        const char* action;
        const char* description;
    };
    constexpr Control kControls[] = {
        {"export.queue.start", "Start Queue", "start", "Starts held jobs (Media Encoder's Start Queue)."},
        {"export.queue.pause", "Pause", "pause", "Pauses jobs: a running one between two frames, a queued one is held."},
        {"export.queue.resume", "Resume", "resume", "Resumes paused jobs and starts held ones."},
        {"export.queue.cancel", "Stop", "cancel", "Cancels jobs (a running export leaves no file)."},
        {"export.queue.remove", "Remove", "remove", "Removes finished jobs from the list."},
    };
    for (const Control& control : kControls) {
        const std::string action = control.action;
        addBuiltin(registry,
                   {control.name, 1, control.title,
                    std::string(control.description) + " \"jobs\": the job ids; \"all\": every job of the queue.",
                    Capability::EditProject, // the host's queue: not for read-only clients
                    schema::object({{"jobs", schema::array(jobParam, "The jobs.", 1)},
                                    {"all", schema::boolean("Every job (default false).")}}),
                    Json{{"jobs", Json::array({"j1"})}}, {},
                    [action](ExecutionContext& c) {
                        ExportServices& services = exportServices(c);
                        std::vector<std::string> jobs;
                        if (c.boolean("all", false)) {
                            for (const Json& j : services.jobs()) {
                                jobs.push_back(j.value("job", ""));
                            }
                        } else if (c.has("jobs")) {
                            for (const Json& j : c.params().at("jobs")) {
                                jobs.push_back(j.get<std::string>());
                            }
                        } else {
                            fail(ErrorCode::InvalidParams, "name \"jobs\" or set \"all\"", Json{{"path", "/jobs"}});
                        }
                        CommandOutput out = readOutput(c, Json{{"jobs", jobs}, {"action", action}});
                        if (!c.dryRun()) {
                            for (const std::string& job : jobs) {
                                const auto ok = services.control(job, action);
                                if (!ok && !c.boolean("all", false)) {
                                    throw CommandFailure{ok.error()};
                                }
                            }
                        }
                        return out;
                    }});
    }

    Json nameSchema = schema::string("The preset's name (1..256 bytes); its id is \"user-\" and the name in lowercase.");
    nameSchema["minLength"] = 1;
    nameSchema["maxLength"] = 256;
    // Save and Delete write only the host's own presets folder (no file the caller names): edits of the user's library,
    // like template.save to the user's templates.
    addBuiltin(registry,
                   {"export.preset.save", 1, "Save Preset",
                    "Premiere's Save Preset: stores a preset's settings with overrides on top as a user preset (a JSON file "
                    "in the user's export presets folder). \"overwrite\" replaces one of the same name.",
                    Capability::EditProject,
                    schema::object({{"name", nameSchema},
                                    {"description", schema::string("A one-line description.")},
                                    {"preset", presetParam},
                                    {"overrides", overridesSchema()},
                                    {"overwrite", schema::boolean("Replace a preset of the same name (default false).")}},
                                   {"name"}),
                    Json{{"name", "My YouTube"}, {"preset", "youtube-1080p"}, {"overrides", Json{{"bitrateMbps", 20}}}}, {},
                    [](ExecutionContext& c) {
                        ExportServices::Output from{c.string("preset", "match-source"), overridesOf(c), ""};
                        return readOutput(c, take(exportServices(c).savePreset(c.string("name"), c.string("description"), from,
                                                                                 c.boolean("overwrite", false), c.dryRun())));
                    }});

    addBuiltin(registry,
               command::touchesFiles(
                   {"export.preset.import", 1, "Import Preset",
                    "Premiere's Import Preset: reads a Wabcut export preset file (.json) and stores it with the user's "
                    "presets. The file is checked like any untrusted input (types, ranges, sizes).",
                    Capability::Read,
                    schema::object({{"file", pathSchema("The preset file.")},
                                    {"overwrite", schema::boolean("Replace a preset of the same name (default false).")}},
                                   {"file"}),
                    Json{{"file", "C:/Presets/Client delivery.json"}}, {},
                    [](ExecutionContext& c) {
                        return readOutput(c, take(exportServices(c).importPreset(c.string("file"), c.boolean("overwrite", false),
                                                                                   c.dryRun())));
                    }},
                   {"file"}));

    addBuiltin(registry,
               command::touchesFiles(
                   {"export.preset.export", 1, "Export Preset",
                    "Writes a preset (built-in or the user's) to a preset file others can import.",
                    Capability::Read,
                    schema::object({{"id", presetParam}, {"file", pathSchema("The file to write.")}}, {"id", "file"}),
                    Json{{"id", "user-my-youtube"}, {"file", "C:/Presets/My YouTube.json"}}, {},
                    [](ExecutionContext& c) {
                        take(exportServices(c).exportPreset(c.string("id"), c.string("file"), c.dryRun()));
                        return readOutput(c, Json{{"id", c.string("id")}, {"file", c.string("file")}, {"written", !c.dryRun()}});
                    }},
                   {"file"}));

    addBuiltin(registry,
                   {"export.preset.delete", 1, "Delete Preset",
                    "Deletes a user preset (built-in presets cannot be deleted).", Capability::EditProject,
                    schema::object({{"id", presetParam}}, {"id"}), Json{{"id", "user-my-youtube"}}, {},
                    [](ExecutionContext& c) {
                        take(exportServices(c).deletePreset(c.string("id"), c.dryRun()));
                        return readOutput(c, Json{{"id", c.string("id")}, {"deleted", !c.dryRun()}});
                    }});

    addBuiltin(registry,
               {"export.watchFolder.list", 1, "Watch Folders",
                "Media Encoder's Watch Folders: each folder, its preset and output folder, whether sources move into "
                "\"Source\" once encoded, and how many files are queued, done and failed.",
                Capability::Read, schema::object({}), Json::object(), {},
                [](ExecutionContext& c) { return readOutput(c, Json{{"folders", exportServices(c).watchFolders()}}); }});

    addBuiltin(registry,
               command::touchesFiles(
                   {"export.watchFolder.add", 1, "Add Watch Folder",
                    "Watches a folder: every media file dropped into it (once it stops growing) is exported with the "
                    "preset into \"output\" (default \"<folder>/Output\"), through the export queue; with \"moveSources\" "
                    "(default true) the source then moves into \"<folder>/Source\".",
                    Capability::Read,
                    schema::object({{"folder", pathSchema("The folder to watch (created when missing).")},
                                    {"preset", presetParam},
                                    {"output", pathSchema("Where the exports go.")},
                                    {"moveSources", schema::boolean("Move encoded sources into Source (default true).")}},
                                   {"folder", "preset"}),
                    Json{{"folder", "D:/Ingest/Drop here"}, {"preset", "youtube-1080p"}}, {},
                    [](ExecutionContext& c) {
                        return readOutput(c, take(exportServices(c).addWatchFolder(c.string("folder"), c.string("preset"),
                                                                                     c.string("output"),
                                                                                     c.boolean("moveSources", true), c.dryRun())));
                    }},
                   {"folder", "output"}));

    addBuiltin(registry,
               {"export.watchFolder.remove", 1, "Remove Watch Folder", "Stops watching a folder (its queued exports go on).",
                Capability::EditProject, // the host's watch folders: not for read-only clients
                schema::object({{"id", schema::string("The watch folder's id (export.watchFolder.list).")}}, {"id"}),
                Json{{"id", "w1"}}, {},
                [](ExecutionContext& c) {
                    take(exportServices(c).removeWatchFolder(c.string("id"), c.dryRun()));
                    return readOutput(c, Json{{"id", c.string("id")}, {"removed", !c.dryRun()}});
                }});
}

} // namespace wabcut::command::detail
