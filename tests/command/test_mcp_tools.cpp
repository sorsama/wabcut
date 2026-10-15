// MCP tools: the generated tool list, read tools, dry run vs commit, revisions, read-only refusal, undo/redo,
// batches and saving.

#include "McpHarness.h"

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/mcp/McpCli.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <regex>
#include <set>

using namespace wabcut;
using namespace wabcut::test;
namespace mcp = wabcut::command::mcp;

namespace {

constexpr const char* kPicture = R"(
    V1 tgt |Aaaa..Bbbb|
    A1 tgt |Aaaa..Bbbb|
)";

Json firstClipSelector() { return Json::array({Json{{"track", "V1"}, {"time", "00:00:00:01"}}}); }

std::filesystem::path outputDir() {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "command" / "mcp";
    std::filesystem::create_directories(dir);
    return dir;
}

std::filesystem::path freshFile(const char* name) {
    const auto file = outputDir() / name;
    std::filesystem::remove(file);
    for (int i = 1; i <= 5; ++i) {
        std::filesystem::remove(io::backupPath(file, i));
    }
    return file;
}

} // namespace

TEST_CASE("tools/list: valid schemas, annotations from the capability", "[command][mcp][tools]") {
    mcp::McpServerOptions options = editOptions();
    options.projectPath = outputDir() / "unused.wabcut";
    McpHarness h(kPicture, options);
    const Json tools = h.one(modernRequest(1, "tools/list"))["result"]["tools"];
    REQUIRE(tools.size() == servedCatalogSize() + 4); // + batch, undo, redo, save
    const std::regex namePattern("^[a-zA-Z0-9_-]{1,64}$");
    std::set<std::string> names;
    for (const Json& tool : tools) {
        const std::string name = tool["name"].get<std::string>();
        INFO(name);
        names.insert(name);
        CHECK(std::regex_match(name, namePattern));
        CHECK_FALSE(tool["title"].get<std::string>().empty());
        CHECK_FALSE(tool["description"].get<std::string>().empty());
        CHECK(tool["inputSchema"]["type"] == "object");
        CHECK(command::unsupportedSchemaKeywords(tool["inputSchema"]).empty());
        CHECK(command::unsupportedSchemaKeywords(tool["outputSchema"]).empty());
        const Json& a = tool["annotations"];
        CHECK(a["openWorldHint"] == false);
        const command::CommandSpec* spec = command::builtinCommands().find(name);
        if (spec) {
            const bool read = spec->capability == command::Capability::Read;
            CHECK(a["readOnlyHint"] == read);
            CHECK(a["destructiveHint"] == !read);
            CHECK(tool["inputSchema"]["properties"].contains("dryRun") == !read);
            // The catalog example is valid input for the tool.
            CHECK(command::validateAgainstSchema(tool["inputSchema"], spec->example).empty());
        }
    }
    CHECK(names.size() == tools.size());
    for (const char* extra : {"edit_batch", "edit_undo", "edit_redo", "project_save"}) {
        CHECK(names.contains(extra));
    }
    // Envelope names never clash with command parameters.
    for (const command::CommandSpec& spec : command::builtinCommands().all()) {
        for (const char* key : {"dryRun", "expectedRevision", "idempotencyKey"}) {
            CHECK_FALSE(spec.params["properties"].contains(key));
        }
    }
}

TEST_CASE("Read tools answer with structured content", "[command][mcp][tools]") {
    McpHarness h(kPicture, readOnlyOptions());
    const Json project = h.ok("project_summary");
    CHECK(project["ok"] == true);
    CHECK(project["revision"] == 0);
    CHECK(project["data"]["sequences"].size() == 1);
    const Json found = h.ok("clip_find", Json{{"name", "b"}});
    CHECK(found["data"]["clips"].size() == 2);
    const Json details = h.ok("clip_details", Json{{"clip", Json{{"track", "A1"}, {"time", "00:00:00:07"}}}});
    CHECK(details["data"]["name"] == "B");
    // Problems the model can fix come back as tool errors.
    CHECK(h.toolError("clip_details", Json{{"clip", 9999}}) == "NotFound");
    CHECK(h.toolError("sequence_summary", Json{{"bogus", 1}}) == "InvalidParams");
    CHECK(h.toolError("sequence_summary", Json{{"dryRun", true}}) == "InvalidParams"); // reads take no envelope
}

TEST_CASE("Dry run previews; commit changes the project and reports revisions", "[command][mcp][edit]") {
    McpHarness h(Timeline(kPicture).linkMatching(), editOptions());
    const Json args{{"clips", firstClipSelector()}};

    Json preview = args;
    preview["dryRun"] = true;
    const Json dry = h.ok("timeline_rippleDelete", preview);
    CHECK(dry["dryRun"] == true);
    CHECK(dry["committed"] == false);
    CHECK(dry["revision"] == 0);
    CHECK(dry["basedOnRevision"] == 0);
    const Json& changes = dry["changes"]["sequences"][0];
    CHECK(changes["removedClips"].size() == 2);
    CHECK(changes["tracks"][0]["track"] == "V1");
    CHECK(changes["tracks"][0]["ranges"][0] == Json::array({"00:00:00:00", "00:00:00:10"}));
    // The layout shows the result: B moved to frame 2.
    const Json& v1 = dry["layout"][0]["tracks"][0];
    CHECK(v1["track"] == "V1");
    REQUIRE(v1["clips"].size() == 1);
    CHECK(v1["clips"][0][1] == "B");
    CHECK(v1["clips"][0][2] == "00:00:00:02");
    CHECK(h.revision() == 0);
    CHECK_TIMELINE(*h.state(), h.tl.seq, kPicture);

    const Json commit = h.ok("timeline_rippleDelete", args);
    CHECK(commit["committed"] == true);
    CHECK(commit["dryRun"] == false);
    CHECK(commit["basedOnRevision"] == 0);
    CHECK(commit["revision"] == 1);
    CHECK(commit["assumedRevision"] == true);
    CHECK(commit["layout"] == dry["layout"]);
    CHECK_TIMELINE(*h.state(), h.tl.seq, R"(
        V1 |..Bbbb....|
        A1 |..Bbbb....|
    )");

    // With an explicit, current revision.
    const Json cut = h.ok("sequence_addEdit", Json{{"time", "00:00:00:03"}, {"expectedRevision", 1}});
    CHECK(cut["revision"] == 2);
    CHECK_FALSE(cut.contains("assumedRevision"));
    CHECK(cut["createdIds"].size() == 2);
}

TEST_CASE("A stale expectedRevision edits nothing", "[command][mcp][edit]") {
    McpHarness h(kPicture, editOptions());
    h.ok("sequence_addEdit", Json{{"time", "00:00:00:02"}});
    const Json result = h.call("sequence_addEdit", Json{{"time", "00:00:00:07"}, {"expectedRevision", 0}});
    REQUIRE(result["isError"] == true);
    CHECK(result["structuredContent"]["error"]["code"] == "StaleRevision");
    CHECK(result["structuredContent"]["revision"] == 1);
    CHECK(result["content"][0]["text"].get<std::string>().starts_with("StaleRevision: "));
    CHECK(h.revision() == 1);

    SECTION("servers that require expectedRevision refuse a commit without one") {
        mcp::McpServerOptions options = editOptions();
        options.requireExpectedRevision = true;
        McpHarness strict(kPicture, options);
        CHECK(strict.toolError("sequence_addEdit", Json{{"time", 2}}) == "InvalidParams");
        CHECK(strict.ok("sequence_addEdit", Json{{"time", 2}, {"dryRun", true}})["dryRun"] == true);
        CHECK(strict.ok("sequence_addEdit", Json{{"time", 2}, {"expectedRevision", 0}})["revision"] == 1);
    }
    SECTION("an idempotency key makes a retry safe") {
        const Json first = h.ok("sequence_addEdit", Json{{"time", 7}, {"idempotencyKey", "k1"}});
        const Json again = h.ok("sequence_addEdit", Json{{"time", 7}, {"idempotencyKey", "k1"}});
        CHECK(again["replayed"] == true);
        CHECK(again["revision"] == first["revision"]);
        CHECK(h.session.undoCount() == 2);
    }
}

TEST_CASE("A read-only server refuses edits with a readable tool error", "[command][mcp][readonly]") {
    McpHarness h(kPicture, readOnlyOptions());
    const Json tools = h.one(modernRequest(1, "tools/list"))["result"]["tools"];
    REQUIRE(tools.size() == readCommandCount());
    for (const Json& tool : tools) {
        CHECK(tool["annotations"]["readOnlyHint"] == true);
    }
    for (const std::string tool : {"timeline_rippleDelete", "sequence_addTrack", "edit_undo", "edit_batch", "project_save"}) {
        INFO(tool);
        const Json result = h.call(tool, Json::object());
        REQUIRE(result["isError"] == true);
        CHECK(result["structuredContent"]["error"]["code"] == "PermissionDenied");
        CHECK(result["content"][0]["text"].get<std::string>().find("--allow-edit") != std::string::npos);
    }
    // Even a dry run is refused: the client has no edit capability.
    CHECK(h.toolError("timeline_rippleDelete", Json{{"clips", firstClipSelector()}, {"dryRun", true}}) ==
          "PermissionDenied");
    CHECK(h.revision() == 0);
}

TEST_CASE("Analysis tools only when allowed; network tools never", "[command][mcp][readonly]") {
    static const command::CommandRegistry registry = [] {
        command::CommandRegistry r = command::builtinCommands();
        const command::Executor answer = [](command::ExecutionContext& c) {
            command::CommandOutput out;
            out.state = c.state();
            out.data = Json{{"measured", true}};
            return out;
        };
        REQUIRE(r.add({"test.measure", 1, "Measure", "Analyses media.", command::Capability::MediaAnalysis,
                       command::schema::object({}), Json::object(), {}, answer}));
        REQUIRE(r.add({"test.upload", 1, "Upload", "Leaves the machine.", command::Capability::Network,
                       command::schema::object({}), Json::object(), {}, answer}));
        return r;
    }();
    const auto listed = [](McpHarness& h) {
        std::set<std::string> names;
        for (const Json& t : h.one(modernRequest(1, "tools/list"))["result"]["tools"]) {
            names.insert(t["name"].get<std::string>());
        }
        return names;
    };
    {
        McpHarness h(kPicture, editOptions(), registry); // edits, but not analysis
        const auto names = listed(h);
        CHECK_FALSE(names.contains("test_measure"));
        CHECK_FALSE(names.contains("test_upload"));
        CHECK(h.one(callLine(2, "test_measure")).contains("error")); // an unknown tool
    }
    {
        command::mcp::McpServerOptions options = readOnlyOptions(); // what --allow-analysis adds, plus Network
        options.allowed = command::CapabilitySet::of(
            {command::Capability::Read, command::Capability::MediaAnalysis, command::Capability::Network});
        McpHarness h(kPicture, options, registry);
        const auto names = listed(h);
        CHECK(names.contains("test_measure"));
        CHECK_FALSE(names.contains("test_upload"));
        CHECK(h.ok("test_measure")["data"]["measured"] == true);
        CHECK(h.one(callLine(3, "test_upload")).contains("error"));
        CHECK(h.toolError("timeline_rippleDelete", Json::object()) == "PermissionDenied"); // still read-only
    }
}

TEST_CASE("Undo and redo through tools, with revision checks", "[command][mcp][undo]") {
    McpHarness h(Timeline(kPicture).linkMatching(), editOptions());
    CHECK(h.toolError("edit_undo") == "EditRejected"); // nothing to undo yet
    h.ok("timeline_rippleDelete", Json{{"clips", firstClipSelector()}});

    CHECK(h.toolError("edit_undo", Json{{"expectedRevision", 0}}) == "StaleRevision");
    const Json undo = h.ok("edit_undo", Json{{"expectedRevision", 1}});
    CHECK(undo["undone"] == "Ripple Delete");
    CHECK(undo["revision"] == 2);
    CHECK(undo["canUndo"] == false);
    CHECK(undo["canRedo"] == true);
    CHECK(undo["changes"]["sequences"][0]["addedClips"].size() == 2);
    CHECK_TIMELINE(*h.state(), h.tl.seq, kPicture);

    const Json redo = h.ok("edit_redo");
    CHECK(redo["redone"] == "Ripple Delete");
    CHECK(redo["revision"] == 3);
    CHECK_TIMELINE(*h.state(), h.tl.seq, R"(
        V1 |..Bbbb....|
        A1 |..Bbbb....|
    )");
    CHECK(h.toolError("edit_redo") == "EditRejected");
    CHECK(h.toolError("edit_undo", Json{{"expectedRevision", "3"}}) == "InvalidParams");
}

TEST_CASE("edit_batch is one undo step, all or nothing", "[command][mcp][batch]") {
    McpHarness h(kPicture, editOptions());
    const Json batch{{"commands", Json::array({Json{{"command", "sequence.addEdit"}, {"params", Json{{"time", "00:00:00:02"}}}},
                                               Json{{"command", "marker.add"}, {"params", Json{{"time", 0}, {"name", "Start"}}}}})},
                     {"label", "Cut and mark"}};
    Json dry = batch;
    dry["dryRun"] = true;
    CHECK(h.ok("edit_batch", dry)["committed"] == false);
    const Json done = h.ok("edit_batch", batch);
    CHECK(done["committed"] == true);
    CHECK(h.session.undoCount() == 1);
    CHECK(h.session.undoLabel() == "Cut and mark");

    Json broken = batch;
    broken["commands"][1]["params"]["time"] = "nonsense";
    CHECK(h.toolError("edit_batch", broken) == "InvalidParams");
    CHECK(h.session.undoCount() == 1);
    CHECK(h.toolError("edit_batch", Json{{"commands", Json::array({Json{{"command", "timeline.explode"}}})}}) ==
          "InvalidParams");
    CHECK(h.toolError("edit_batch", Json{{"commands", Json::array()}}) == "InvalidParams");
}

TEST_CASE("project_save writes the project or a copy next to it, nothing else", "[command][mcp][save]") {
    const auto file = freshFile("save.wabcut");
    Timeline tl(kPicture);
    REQUIRE(io::saveProject(tl.state, file));
    mcp::McpServerOptions options = editOptions();
    options.projectPath = file;
    McpHarness h(tl, options);

    h.ok("sequence_addEdit", Json{{"time", "00:00:00:02"}});
    CHECK(h.server.savedRevision() == 0);
    const Json saved = h.ok("project_save");
    CHECK(saved["copy"] == false);
    CHECK(h.server.savedRevision() == 1);
    CHECK(io::loadProject(file)->sequences[0]->videoTracks[0]->clips.size() == 3);

    const auto copy = freshFile("Rough cut (v2).wabcut");
    CHECK(h.ok("project_save", Json{{"copyName", "Rough cut (v2).wabcut"}})["copy"] == true);
    CHECK(std::filesystem::exists(copy));
    CHECK(h.server.savedRevision() == 1);

    for (const char* bad : {"../escape.wabcut", "sub/dir.wabcut", "C:\\x.wabcut", "notes.txt", ".wabcut", "CON.wabcut",
                            "lpt1.wabcut", "a..b.wabcut", ".hidden.wabcut"}) {
        INFO(bad);
        CHECK(h.toolError("project_save", Json{{"copyName", bad}}) == "InvalidParams");
    }
    CHECK_FALSE(std::filesystem::exists(outputDir().parent_path() / "escape.wabcut"));
}

TEST_CASE("Save modes: on exit and after every change", "[command][mcp][save]") {
    Timeline tl(kPicture);
    SECTION("--save-on-exit saves once the client disconnects") {
        const auto file = freshFile("on-exit.wabcut");
        REQUIRE(io::saveProject(tl.state, file));
        mcp::McpServerOptions options = editOptions();
        options.projectPath = file;
        options.saveMode = mcp::SaveMode::OnExit;
        McpHarness h(tl, options);
        h.exchange({callLine(1, "sequence_addEdit", Json{{"time", 2}})}); // serve() ends: saved
        CHECK(io::loadProject(file)->sequences[0]->videoTracks[0]->clips.size() == 3);
        CHECK(h.server.savedRevision() == 1);
    }
    SECTION("--autosave saves after each change, undo included") {
        const auto file = freshFile("auto.wabcut");
        REQUIRE(io::saveProject(tl.state, file));
        mcp::McpServerOptions options = editOptions();
        options.projectPath = file;
        options.saveMode = mcp::SaveMode::Auto;
        McpHarness h(tl, options);
        CHECK(h.ok("sequence_addEdit", Json{{"time", 2}})["saved"] == true);
        CHECK(io::loadProject(file)->sequences[0]->videoTracks[0]->clips.size() == 3);
        CHECK(h.ok("edit_undo")["saved"] == true);
        CHECK(io::loadProject(file)->sequences[0]->videoTracks[0]->clips.size() == 2);
        CHECK_FALSE(h.ok("sequence_addEdit", Json{{"time", 2}, {"dryRun", true}}).contains("saved"));
    }
}

TEST_CASE("wabcut-cli mcp arguments", "[command][mcp][cli]") {
    const auto parse = [](std::vector<std::string_view> args) { return mcp::parseMcpArguments(args); };
    const auto basic = parse({"--project", "a.wabcut"});
    REQUIRE(basic);
    CHECK(basic->project == "a.wabcut");
    CHECK_FALSE(basic->allowEdit);
    CHECK(basic->saveMode == mcp::SaveMode::Manual);
    const auto edit = parse({"--project", "a.wabcut", "--allow-edit", "--autosave", "--timeout-ms", "500"});
    REQUIRE(edit);
    CHECK(edit->allowEdit);
    CHECK(edit->saveMode == mcp::SaveMode::Auto);
    CHECK(edit->timeout == std::chrono::milliseconds(500));
    CHECK_FALSE(edit->allowAnalysis);
    const auto analysis = parse({"--project", "a.wabcut", "--allow-analysis"});
    REQUIRE(analysis);
    CHECK(analysis->allowAnalysis);
    CHECK_FALSE(analysis->allowEdit);
    CHECK_FALSE(parse({}));
    CHECK_FALSE(parse({"--project", "a.wabcut", "--autosave"}));             // needs --allow-edit
    CHECK_FALSE(parse({"--project", "a.wabcut", "--allow-edit", "--autosave", "--save-on-exit"}));
    CHECK_FALSE(parse({"--project", "a.wabcut", "--timeout-ms", "0"}));
    CHECK_FALSE(parse({"--project", "a.wabcut", "--shell"}));
}
