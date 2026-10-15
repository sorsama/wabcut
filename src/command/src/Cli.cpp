#include <wabcut/command/Cli.h>

#include <wabcut/command/Engine.h>
#include <wabcut/command/ToolExport.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>

#include <algorithm>

namespace wabcut::command {

CliOutcome runCommandCli(const std::filesystem::path& project, std::string_view requestJson, bool dryRun, bool save,
                         const CommandRegistry& registry, std::shared_ptr<CommandServices> services) {
    Json document = Json::parse(requestJson, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return {2, {}, "--json is not a JSON object\n"};
    }
    if (dryRun) {
        document["dryRun"] = true;
    }
    auto loaded = io::loadProject(project);
    if (!loaded) {
        return {2, {}, "cannot load " + pathToUtf8(project) + ": " + loaded.error().message + "\n"};
    }
    EditSession session(std::move(*loaded));
    CommandEngine engine(session, registry, EnginePolicy{CapabilitySet::all(), 1024, std::move(services)});
    const CommandResult result = engine.executeJson(document);
    Json out = toJson(result);
    if (!result.ok()) {
        return {1, out.dump(2) + "\n", result.error->message + "\n"};
    }
    std::string err;
    if (save) {
        if (result.committed) {
            if (auto saved = io::saveProject(session.stateForSave(), project); !saved) {
                return {2, out.dump(2) + "\n", "cannot save " + pathToUtf8(project) + ": " + saved.error().message + "\n"};
            }
            out["saved"] = true;
        } else {
            out["saved"] = false;
            err = "nothing changed; the project was not saved\n";
        }
    }
    return {0, out.dump(2) + "\n", err};
}

CliOutcome runToolsCli(std::string_view format, const CommandRegistry& registry) {
    const auto parsed = toolFormatFromString(format);
    if (!parsed) {
        return {2, {}, "unknown format \"" + std::string(format) + "\" (anthropic, openai or jsonschema)\n"};
    }
    return {0, exportTools(registry, *parsed).dump(2) + "\n", {}};
}

namespace {

std::string typeText(const Json& schema) {
    if (!schema.is_object()) {
        return "any";
    }
    if (schema.contains("enum")) {
        std::string out;
        for (const Json& v : schema["enum"]) {
            out += (out.empty() ? "" : " \\| ") + std::string("`") + v.get<std::string>() + "`";
        }
        return out;
    }
    std::string out;
    if (schema.contains("type")) {
        const Json& type = schema["type"];
        if (type.is_string()) {
            out = type.get<std::string>();
        } else {
            for (const Json& t : type) {
                out += (out.empty() ? "" : " \\| ") + t.get<std::string>();
            }
        }
    }
    if (out == "array" && schema.contains("items")) {
        out = typeText(schema["items"]) + "[]";
        if (out.find(' ') != std::string::npos) {
            out = "(" + typeText(schema["items"]) + ")[]";
        }
    }
    if (schema.contains("minimum") || schema.contains("maximum")) {
        out += " (" + (schema.contains("minimum") ? schema["minimum"].dump() : std::string()) + ".." +
               (schema.contains("maximum") ? schema["maximum"].dump() : std::string()) + ")";
    }
    if (schema.contains("minItems")) {
        out += ", min " + schema["minItems"].dump();
    }
    return out;
}

std::string cell(std::string text) {
    std::string out;
    for (const char c : text) {
        if (c == '|') {
            out += "\\|";
        } else if (c == '\n') {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

void parameterRows(std::string& md, const Json& schema, const std::string& prefix) {
    if (!schema.contains("properties")) {
        return;
    }
    std::vector<std::string> required;
    if (schema.contains("required")) {
        required = schema["required"].get<std::vector<std::string>>();
    }
    for (const auto& item : schema["properties"].items()) {
        const Json& p = item.value();
        const bool isRequired = std::find(required.begin(), required.end(), item.key()) != required.end();
        md += "| `" + prefix + item.key() + "` | " + typeText(p) + " | " + (isRequired ? "yes" : "") + " | " +
              cell(p.value("description", std::string())) + " |\n";
        if (p.contains("items") && p["items"].is_object() && p["items"].value("type", Json()) == "object") {
            parameterRows(md, p["items"], prefix + item.key() + "[].");
        } else if (p.value("type", Json()) == "object") {
            parameterRows(md, p, prefix + item.key() + ".");
        }
    }
}

} // namespace

std::string catalogMarkdown(const CommandRegistry& registry) {
    std::string md;
    md += "# Edit commands\n\n";
    md += "<!-- Generated from the command registry by wabcut_command_docgen; do not edit by hand.\n"
          "     Regenerate: build\\win-msvc\\bin\\wabcut_command_docgen.exe docs/commands.md docs/mcp.md\n"
          "     (the command test suite fails while this file is out of date). -->\n\n";
    md += "The typed command protocol (`src/command`, library `wabcut_command`) is the one way agents, the CLI, "
          "scripts and MCP clients edit a project. Commands are thin, validated adapters over the model's "
          "`edit::` operations: the model decides what an edit does, the protocol names it, checks it and "
          "reports what changed.\n\n";
    md += "## Requests\n\n";
    md += "```json\n"
          "{\"command\": \"timeline.rippleDelete\", \"version\": 1, \"params\": {\"clips\": [42]},\n"
          " \"expectedRevision\": 7, \"idempotencyKey\": \"agent-run-3/step-12\", \"dryRun\": false}\n"
          "```\n\n";
    md += "* `command`: a name below, one of its aliases, or its LLM tool name (`.` written as `_`).\n"
          "* `version`: optional; the newest version when left out.\n"
          "* `expectedRevision`: optional; the request fails with `StaleRevision` unless the session is at that "
          "revision. Every result reports the current `revision`.\n"
          "* `idempotencyKey`: optional; a successful request is remembered per session, and a replay with the "
          "same key returns the first result (`\"replayed\": true`) instead of editing again. Reusing a key for a "
          "different request fails with `IdempotencyConflict`.\n"
          "* `dryRun`: validate and compute the result and its ChangeSet without committing. In process the "
          "result carries the would-be state as a preview snapshot. Nothing outside the project changes either: what "
          "a command does to the host (the clipboard, a version saved to disk) happens only once a request commits, "
          "never for a dry run or a batch that fails.\n\n";
    md += "A **batch** applies several commands in order as one undo step, all or nothing:\n\n";
    md += "```json\n"
          "{\"commands\": [{\"command\": \"sequence.addEdit\", \"params\": {\"time\": \"00:00:05:00\"}},\n"
          "              {\"command\": \"clip.setParam\", \"params\": {\"clips\": [{\"track\": \"V1\", \"time\": "
          "\"00:00:06:00\"}],\n"
          "                                                 \"param\": \"opacity.opacity\", \"value\": 0.5}}],\n"
          " \"label\": \"Dim the second half\", \"expectedRevision\": 7, \"dryRun\": true}\n"
          "```\n\n";
    md += "`expectedRevision`, `idempotencyKey` and `dryRun` belong to the batch, not to its commands. Each "
          "command sees the state the previous ones produced; read commands inside a batch report in `steps`.\n\n";
    md += "## References\n\n";
    md += "* **Times** are integer ticks (254016000000 per second) or timecode strings in the sequence's rate "
          "(`\"00:01:02:03\"`, `;` before the frames for drop-frame). Deltas may be negative (`\"-00:00:00:05\"`).\n"
          "* **Tracks** are labels (`\"V1\"` is the bottom video track, `\"A1\"` the first audio track) or track ids.\n"
          "* **Clips** are clip ids or `{\"track\": \"V1\", \"time\": \"00:00:03:00\"}` for the clip under that "
          "time.\n"
          "* **Media** is a media id or the media item's exact name.\n"
          "* **Sequence**: every sequence command takes an optional `sequence` id; it may be left out when the "
          "project has exactly one sequence.\n\n";
    md += "## Results\n\n";
    md += "```json\n"
          "{\"ok\": true, \"revision\": 8, \"changes\": {\"sequences\": [{\"sequence\": 2, \"tracks\": [...], "
          "\"addedClips\": [], \"removedClips\": [42], ...}]},\n"
          " \"dryRun\": false, \"committed\": true}\n"
          "```\n\n";
    md += "`changes` (the model's ChangeSet: per sequence the touched tracks and time ranges, added, removed and "
          "modified clips, property roles and other `changed` parts), `createdIds`, `warnings`, `data` (a read "
          "command's answer) and `steps` (per command of a batch) are left out when empty.\n\n";
    md += "Failures change nothing and carry `{\"ok\": false, \"error\": {\"code\", \"message\", \"details\"}}`. "
          "Codes: `UnknownCommand`, `UnsupportedVersion`, `InvalidRequest`, `InvalidParams` (details.issues lists "
          "JSON-pointer paths), `NotFound`, `PermissionDenied`, `StaleRevision`, `IdempotencyConflict`, "
          "`EditRejected` (the model refused the edit), `InvariantViolation`, `LockedTrack`, `Cancelled`, "
          "`TimedOut` (the caller stopped a long request; see `ExecutionControl`), `Internal`. In a "
          "batch, details.index names the failing command.\n\n";
    md += "Every request is checked in release builds too: params against the schema, references against the "
          "project, the result against the model invariants (`validate()`), and locked tracks must come out "
          "unchanged.\n\n";
    md += "**Capabilities** scope what a client may run: `Read`, `EditSequence`, `EditProject`, `MediaAnalysis`, "
          "`Network` (the last two have no commands yet).\n\n";
    md += "## Command-line and LLM tools\n\n";
    md += "```\n"
          "wabcut-cli cmd <project.wabcut> --json '<request or batch>' [--dry-run] [--save]\n"
          "wabcut-cli tools --format anthropic|openai|jsonschema\n"
          "wabcut-cli mcp --project <file.wabcut> [--allow-edit] [--allow-analysis] [--save-on-exit | --autosave]\n"
          "```\n\n";
    md += "`mcp` serves the catalog to MCP clients such as Claude Desktop, Claude Code and Codex; see "
          "[docs/mcp.md](mcp.md).\n\n";
    md += "`tools` prints the catalog as Anthropic Messages API `tools`, OpenAI Chat Completions `tools` or plain "
          "JSON Schema definitions. Each CLI run is a fresh session (revision 0).\n\n";
    md += "**Transcript commands** (`transcript.*`, `captions.generate`; library `wabcut_ai_edit`, see "
          "[docs/ai.md](ai.md#text-based-editing)) edit the video by editing what it says. They need the "
          "transcripts of the project's media, which the host provides (`EnginePolicy::services`): `wabcut-cli` "
          "reads them from the analysis cache that transcription fills, and without them the commands fail with "
          "`InvalidRequest`. Their word indices refer to `transcript.text` / `transcript.words` at the request's "
          "revision, so pass `expectedRevision`. `wabcut-cli transcript-edit` and `wabcut-cli captions` wrap them "
          "for scripts.\n\n";
    md += "**Media-analysis commands** (`analysis.*`, `clip.splitAtShots`; library `wabcut_ai_analysis`, see "
          "[docs/ai.md](ai.md#media-analysis-shots-and-beats)) detect shots and beats in the project's media (cached "
          "per source file); the host provides the analysis (`AnalysisServices`), else they fail with "
          "`InvalidRequest`. `analysis.detectBeats` sets the sequence beat grid that `edit.snapToBeats`, "
          "`edit.cutToBeats` and timeline snapping use; `sequence.setBeatGrid` sets it by hand or from taps.\n\n";

    md += "## Catalog\n\n";
    md += "| Command | Capability | Title | Aliases |\n|---|---|---|---|\n";
    const auto specs = registry.all();
    for (const CommandSpec& spec : specs) {
        std::string aliases;
        for (const std::string& a : spec.aliases) {
            aliases += (aliases.empty() ? "" : ", ") + std::string("`") + a + "`";
        }
        md += "| [`" + spec.name + "`](#" + spec.name + "-v" + std::to_string(spec.version) + ") | " +
              std::string(toString(spec.capability)) + " | " + cell(spec.title) + " | " + aliases + " |\n";
    }
    md += "\n";
    for (const CommandSpec& spec : specs) {
        md += "<a id=\"" + spec.name + "-v" + std::to_string(spec.version) + "\"></a>\n\n";
        md += "### `" + spec.name + "` v" + std::to_string(spec.version) + ": " + spec.title + "\n\n";
        md += spec.description + "\n\n";
        md += "Capability `" + std::string(toString(spec.capability)) + "`. Tool name `" + toolNameFor(spec.name) + "`.";
        if (!spec.aliases.empty()) {
            md += " Aliases:";
            for (const std::string& a : spec.aliases) {
                md += " `" + a + "`";
            }
            md += ".";
        }
        if (spec.runsModel) {
            md += " Runs an AI model (extensions need MediaAnalysis for it too).";
        }
        if (spec.touchesFiles) {
            md += " Reads or writes the files its parameters name (extensions never run it).";
        }
        md += "\n\n";
        if (spec.params.contains("properties") && !spec.params["properties"].empty()) {
            md += "| Parameter | Type | Required | Description |\n|---|---|---|---|\n";
            parameterRows(md, spec.params, "");
            md += "\n";
        } else {
            md += "No parameters.\n\n";
        }
        md += "```json\n" + Json{{"command", spec.name}, {"params", spec.example}}.dump() + "\n```\n\n";
    }
    return md;
}

} // namespace wabcut::command
