// The Python package (python/wabcut, docs/python.md, 0.7): its generated files follow the catalog, and the server side of
// scripting - edit_transaction (a script's edits are one undo step, all or nothing) and the audit trail.

#include "McpHarness.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/PythonApi.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;
namespace mcp = wabcut::command::mcp;

namespace {

constexpr const char* kPicture = R"(
    V1 tgt |Aaaa..Bbbb|
    A1 tgt |Aaaa..Bbbb|
)";

std::string readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file);
    std::stringstream text;
    text << file.rdbuf();
    std::string out = text.str();
    out.erase(std::remove(out.begin(), out.end(), '\r'), out.end()); // checkouts may use CRLF
    return out;
}

std::size_t count(const std::string& text, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
        ++n;
    }
    return n;
}

mcp::McpServerOptions scriptOptions() {
    mcp::McpServerOptions o = editOptions();
    o.transactions = true;
    return o;
}

Json renameArgs(const char* name) { return Json{{"clips", Json::array({Json{{"track", "V1"}, {"time", 0}}})}, {"name", name}}; }

} // namespace

TEST_CASE("the Python package's generated files are up to date", "[command][python][docs]") {
    for (const auto& file : command::pythonApiFiles(wabcut::ai::edit::editingCommands())) {
        INFO(file.path);
        INFO("regenerate them: build\\win-msvc\\bin\\wabcut_command_docgen.exe docs/commands.md docs/mcp.md docs/extensions.md");
        CHECK(readFile(std::string(WABCUT_PYTHON_PACKAGE_DIR "/") + file.path) == file.text);
    }
}

TEST_CASE("the generated Python files cover every command and are deterministic", "[command][python]") {
    const auto& registry = wabcut::ai::edit::editingCommands();
    const auto first = command::pythonApiFiles(registry);
    const auto second = command::pythonApiFiles(registry);
    REQUIRE(first.size() == 2);
    CHECK(first[0].path == "catalog.json");
    CHECK(first[1].path == "_commands.pyi");
    for (std::size_t i = 0; i < first.size(); ++i) {
        CHECK(first[i].text == second[i].text);
    }

    std::set<std::string> newest;
    for (const command::CommandSpec& spec : registry.all()) {
        newest.insert(spec.name);
    }
    const std::string& catalog = first[0].text;
    CHECK(count(catalog, "\n  {\"aliases\"") == newest.size());
    for (const std::string& name : newest) {
        INFO(name);
        CHECK(catalog.find("\"name\":\"" + name + "\"") != std::string::npos);
    }

    const std::string& stub = first[1].text;
    // One def per command: a command that is also a group (export.queue) is that group's __call__.
    CHECK(count(stub, "\n    def ") == newest.size());
    CHECK(stub.find("class ClipCommands:") != std::string::npos);
    CHECK(stub.find("class ExportQueueCommands:") != std::string::npos);
    CHECK(stub.find("def __call__(") != std::string::npos);
    CHECK(stub.find("        from_: ") != std::string::npos);                   // a parameter named like a keyword
    CHECK(stub.find("    def import_(") != std::string::npos);                  // a command named like a keyword
    CHECK(stub.find("Literal[\"comment\", \"chapter\"") != std::string::npos);  // an enum
    CHECK(stub.find("\n    def import(") == std::string::npos);
    CHECK(command::isPythonReserved("from"));
    CHECK_FALSE(command::isPythonReserved("clip"));
}

TEST_CASE("edit_transaction is served only to clients that ask for it", "[command][mcp][python]") {
    McpHarness plain(kPicture, editOptions());
    const Json tools = plain.one(modernRequest(1, "tools/list"))["result"]["tools"];
    CHECK(std::none_of(tools.begin(), tools.end(), [](const Json& t) { return t["name"] == "edit_transaction"; }));
    CHECK(plain.server.tools().size() == tools.size());
    const Json refused = plain.one(callLine(2, "edit_transaction", Json{{"action", "begin"}}));
    CHECK(refused["error"]["code"] == -32602); // Unknown tool

    McpHarness script(kPicture, scriptOptions());
    const Json scriptTools = script.one(modernRequest(1, "tools/list"))["result"]["tools"];
    CHECK(scriptTools.size() == tools.size() + 1);
    CHECK(std::any_of(scriptTools.begin(), scriptTools.end(), [](const Json& t) { return t["name"] == "edit_transaction"; }));
}

namespace {

struct Step {
    std::string tool;
    Json arguments = Json::object();
};

/// The steps as ONE connection (a transaction lives as long as the connection does: the server rolls an open one back when
/// the input ends), and the CallToolResult of each.
std::vector<Json> connection(McpHarness& h, const std::vector<Step>& steps) {
    std::vector<std::string> lines;
    int id = 1000;
    for (const Step& step : steps) {
        lines.push_back(callLine(++id, step.tool, step.arguments));
    }
    const std::vector<Json> out = h.exchange(lines);
    REQUIRE(out.size() == steps.size());
    std::vector<Json> results;
    for (std::size_t i = 0; i < out.size(); ++i) {
        INFO(steps[i].tool << ": " << out[i].dump());
        REQUIRE(out[i].contains("result"));
        results.push_back(out[i]["result"]);
    }
    return results;
}

std::string errorCode(const Json& result) {
    REQUIRE(result["isError"] == true);
    return result["structuredContent"]["error"]["code"].get<std::string>();
}

} // namespace

TEST_CASE("a transaction makes everything the script edits one undo step", "[command][mcp][python]") {
    McpHarness h(kPicture, scriptOptions());
    const std::size_t steps = h.session.undoCount();
    const auto r = connection(h, {{"edit_transaction", Json{{"action", "begin"}, {"label", "My script"}}},
                                  {"clip_rename", renameArgs("One")},
                                  {"clip_rename", renameArgs("Two")},
                                  {"marker_add", Json{{"time", "00:00:01:00"}, {"name", "M"}}},
                                  {"edit_transaction", Json{{"action", "commit"}}}});
    CHECK(r[0]["structuredContent"]["transaction"] == "open");
    for (std::size_t i = 1; i <= 3; ++i) {
        CHECK(r[i]["isError"] == false);
    }
    CHECK(r[4]["structuredContent"]["transaction"] == "committed");
    CHECK(r[4]["structuredContent"]["committed"] == true);
    CHECK(h.session.undoCount() == steps + 1); // three edits, one step
    CHECK(h.session.undoLabel() == "My script");

    const Json undone = h.ok("edit_undo");
    CHECK(undone["undone"] == "My script");
    CHECK(h.session.undoCount() == steps); // all three went in one undo
    const ProjectSnapshot snap = h.state();
    CHECK(snap->sequences[0]->videoTracks[0]->clips[0].name() == "A");
    CHECK(snap->sequences[0]->markers.empty());
}

TEST_CASE("a rolled back transaction leaves the project as it was", "[command][mcp][python]") {
    McpHarness h(kPicture, scriptOptions());
    h.ok("clip_rename", renameArgs("Before"));
    const std::size_t steps = h.session.undoCount();
    const auto r = connection(h, {{"edit_transaction", Json{{"action", "begin"}}},
                                  {"clip_rename", renameArgs("During")},
                                  {"marker_add", Json{{"time", "00:00:01:00"}, {"name", "M"}}},
                                  {"edit_transaction", Json{{"action", "rollback"}}},
                                  {"edit_transaction", Json{{"action", "commit"}}}});
    CHECK(r[3]["structuredContent"]["transaction"] == "rolledBack");
    CHECK(errorCode(r[4]) == "EditRejected"); // none is open any more
    CHECK(h.session.undoCount() == steps);
    const ProjectSnapshot snap = h.state();
    CHECK(snap->sequences[0]->videoTracks[0]->clips[0].name() == "Before");
    CHECK(snap->sequences[0]->markers.empty());
}

TEST_CASE("a transaction that ends empty adds no undo step", "[command][mcp][python]") {
    McpHarness h(kPicture, scriptOptions());
    const std::size_t steps = h.session.undoCount();
    const auto r = connection(h, {{"edit_transaction", Json{{"action", "begin"}}},
                                  {"project_summary", Json::object()},
                                  {"edit_transaction", Json{{"action", "commit"}}}});
    CHECK(r[2]["structuredContent"]["committed"] == false);
    CHECK(h.session.undoCount() == steps);
}

TEST_CASE("undo, redo, save and a second transaction wait for the open one", "[command][mcp][python]") {
    mcp::McpServerOptions options = scriptOptions();
    options.projectPath = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "command" / "mcp" / "transaction.wabcut";
    McpHarness h(kPicture, options);
    h.ok("clip_rename", renameArgs("Before"));
    const auto r = connection(h, {{"edit_transaction", Json{{"action", "begin"}, {"label", "Open"}}},
                                  {"edit_undo", Json::object()},
                                  {"edit_redo", Json::object()},
                                  {"project_save", Json::object()},
                                  {"edit_transaction", Json{{"action", "begin"}}},
                                  {"edit_transaction", Json{{"action", "dance"}}},
                                  {"edit_transaction", Json{{"action", "commit"}}},
                                  {"edit_undo", Json::object()}});
    CHECK(errorCode(r[1]) == "EditRejected");
    CHECK(errorCode(r[2]) == "EditRejected");
    CHECK(errorCode(r[3]) == "EditRejected");
    CHECK(errorCode(r[4]) == "EditRejected");
    CHECK(errorCode(r[5]) == "InvalidParams");
    CHECK(r[6]["isError"] == false);
    CHECK(r[7]["isError"] == false); // allowed again once it has ended
}

TEST_CASE("a connection that ends with a transaction open rolls it back", "[command][mcp][python]") {
    McpHarness h(kPicture, scriptOptions());
    h.ok("clip_rename", renameArgs("Before"));
    const std::size_t steps = h.session.undoCount();
    // The input ends after begin and an edit (the script crashed or Wabcut stopped it).
    connection(h, {{"edit_transaction", Json{{"action", "begin"}}}, {"clip_rename", renameArgs("Lost")}});
    CHECK(h.session.undoCount() == steps);
    CHECK(h.state()->sequences[0]->videoTracks[0]->clips[0].name() == "Before");
}

TEST_CASE("a failed command inside a transaction leaves the transaction open and the project untouched", "[command][mcp][python]") {
    McpHarness h(kPicture, scriptOptions());
    const auto r = connection(h, {{"edit_transaction", Json{{"action", "begin"}}},
                                  {"clip_rename", renameArgs("Kept for now")},
                                  {"clip_rename", Json{{"clips", Json::array({4242})}, {"name", "x"}}},
                                  {"project_summary", Json::object()},
                                  {"edit_transaction", Json{{"action", "rollback"}}}});
    CHECK(errorCode(r[2]) == "NotFound");
    CHECK(r[3]["isError"] == false); // the session is still usable
    CHECK(r[4]["structuredContent"]["transaction"] == "rolledBack");
    CHECK(h.state()->sequences[0]->videoTracks[0]->clips[0].name() == "A");
}

TEST_CASE("autosave waits for the commit of a transaction", "[command][mcp][python]") {
    mcp::McpServerOptions options = scriptOptions();
    options.projectPath = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "command" / "mcp" / "transaction-autosave.wabcut";
    std::filesystem::create_directories(options.projectPath.parent_path());
    std::filesystem::remove(options.projectPath);
    options.saveMode = mcp::SaveMode::Auto;
    McpHarness h(kPicture, options);
    const auto r = connection(h, {{"edit_transaction", Json{{"action", "begin"}}},
                                  {"clip_rename", renameArgs("Pending")},
                                  {"edit_transaction", Json{{"action", "commit"}}}});
    CHECK_FALSE(r[1]["structuredContent"].contains("saved")); // not inside the transaction
    CHECK(r[2]["structuredContent"]["saved"] == true);        // at the commit
    CHECK(std::filesystem::exists(options.projectPath));
}

TEST_CASE("the audit trail records every tool call, never its parameters", "[command][mcp][python]") {
    std::vector<mcp::McpAuditEntry> entries;
    mcp::McpServerOptions options = scriptOptions();
    options.audit = [&](const mcp::McpAuditEntry& e) { entries.push_back(e); };
    McpHarness h(kPicture, options);
    h.ok("project_summary");
    h.ok("clip_rename", renameArgs("SecretName"));
    h.toolError("clip_rename", Json{{"clips", Json::array({4242})}, {"name", "x"}});
    h.ok("clip_rename", Json{{"clips", Json::array({Json{{"track", "V1"}, {"time", 0}}})}, {"name", "Dry"}, {"dryRun", true}});
    h.ok("edit_batch", Json{{"commands", Json::array({Json{{"command", "marker.add"}, {"params", Json{{"time", "00:00:01:00"}}}},
                                                    Json{{"command", "marker.add"}, {"params", Json{{"time", "00:00:02:00"}}}}})}});
    h.ok("edit_undo");
    REQUIRE(entries.size() == 6);
    CHECK(entries[0].tool == "project_summary");
    CHECK(entries[0].ok);
    CHECK_FALSE(entries[0].committed);
    CHECK(entries[1].tool == "clip_rename");
    CHECK(entries[1].committed);
    CHECK(entries[1].commands == 1);
    CHECK(entries[1].revision == 1);
    CHECK_FALSE(entries[2].ok);
    CHECK(entries[2].code == "NotFound");
    CHECK(entries[3].dryRun);
    CHECK_FALSE(entries[3].committed);
    CHECK(entries[4].tool == "edit_batch");
    CHECK(entries[4].commands == 2);
    CHECK(entries[5].tool == "edit_undo");
    CHECK(entries[5].commands == 0);
}
