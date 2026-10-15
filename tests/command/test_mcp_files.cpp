// Review wave 5, S5-P1-1: an MCP client (an LLM agent that reads project content, so a prompt injection is enough to
// steer it) never reaches the commands that read or write files named in their parameters (CommandSpec::touchesFiles):
// they are not listed, and a call by name is refused with PermissionDenied before anything is read or written.

#include "McpHarness.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/model/MediaLocator.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <set>
#include <string>

using namespace wabcut;
using namespace wabcut::test;
using command::Json;
namespace fs = std::filesystem;

namespace {

constexpr std::string_view kPicture = R"(
    V1 tgt |AaaaBbbb|
    A1 tgt |AaaaBbbb|
)";

/// Every option a client can be given: edits of both kinds and the analysis commands.
command::mcp::McpServerOptions everything() {
    command::mcp::McpServerOptions o = editOptions();
    o.allowed.bits |= command::CapabilitySet::of({command::Capability::MediaAnalysis}).bits;
    return o;
}

std::set<std::string> listedTools(McpHarness& h) {
    std::set<std::string> names;
    for (const Json& t : h.server.tools()) {
        names.insert(t["name"].get<std::string>());
    }
    return names;
}

/// The example params with every file-like value pointing into `dir`.
Json withPathsIn(Json params, const fs::path& dir) {
    if (!params.is_object()) {
        params = Json::object();
    }
    for (const char* key : {"path", "file", "base", "theirs", "directory", "output", "folder"}) {
        if (params.contains(key) && params[key].is_string()) {
            params[key] = pathToUtf8(dir / ("target-" + std::string(key)));
        }
    }
    for (const char* key : {"files", "folders", "addFolders", "removeFolders"}) {
        if (params.contains(key) && params[key].is_array()) {
            params[key] = Json::array({pathToUtf8(dir)});
        }
    }
    return params;
}

} // namespace

TEST_CASE("MCP lists no file commands and refuses them by name, touching nothing (S5-P1-1)", "[command][mcp][security]") {
    const command::CommandRegistry& catalog = ai::edit::editingCommands();
    McpHarness h(kPicture, everything(), catalog);
    const std::set<std::string> listed = listedTools(h);
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "mcp-files";
    fs::remove_all(dir);
    fs::create_directories(dir);

    std::size_t fileCommands = 0;
    for (const command::CommandSpec& spec : catalog.all()) {
        if (!spec.touchesFiles) {
            continue;
        }
        ++fileCommands;
        const std::string tool = command::toolNameFor(spec.name);
        INFO("command " << spec.name);
        CHECK_FALSE(listed.contains(tool));
        const Json reply = h.one(callLine(++h.nextId, tool, withPathsIn(spec.example, dir)));
        INFO(reply.dump());
        REQUIRE(reply.contains("result"));
        const Json& result = reply["result"];
        CHECK(result["isError"] == true);
        CHECK(result["structuredContent"]["error"]["code"] == "PermissionDenied");
        CHECK(result["structuredContent"]["error"]["details"]["touchesFiles"] == true);
    }
    // history.export, review.exportPackage / importPackage, project.merge, cdl.export / import, lut.import,
    // plugins.scan, lottie.import.
    CHECK(fileCommands >= 9);
    CHECK(fs::is_empty(dir)); // nothing written there
    CHECK(*h.state() == h.tl.state); // and the project unchanged
}

TEST_CASE("Every file command names the parameters that hold its paths (S5-P1-1)", "[command][mcp][security]") {
    // A host that confines file access checks exactly these: a file command that named none could not run confined,
    // and a parameter it forgot would escape the check.
    for (const command::CommandSpec& spec : ai::edit::editingCommands().all()) {
        if (!spec.touchesFiles) {
            CHECK(spec.fileParams.empty());
            continue;
        }
        INFO("command " << spec.name);
        if (spec.name == "plugins.scan") {
            CHECK(spec.fileParams.empty()); // never confined: MCP never serves it
            continue;
        }
        REQUIRE_FALSE(spec.fileParams.empty());
        const Json properties = spec.params.value("properties", Json::object());
        for (const std::string& key : spec.fileParams) {
            CHECK(properties.contains(key));
        }
        // Every string parameter that could name a file is among them.
        for (const char* key : {"path", "file", "files", "folder", "folders", "base", "theirs", "directory", "output"}) {
            if (properties.contains(key)) {
                CHECK(std::find(spec.fileParams.begin(), spec.fileParams.end(), key) != spec.fileParams.end());
            }
        }
    }
}

TEST_CASE("With --allow-files file commands are served inside the allowed folders only (S5-P1-1)", "[command][mcp][security]") {
    const command::CommandRegistry& catalog = ai::edit::editingCommands();
    const fs::path base = fs::path(WABCUT_TEST_OUTPUT_DIR) / "mcp-files-confined";
    fs::remove_all(base);
    const fs::path root = base / "project";
    const fs::path outside = base / "elsewhere";
    fs::create_directories(root);
    fs::create_directories(outside);
    command::mcp::McpServerOptions options = everything();
    options.fileRoots = {root};
    McpHarness h(kPicture, options, catalog);
    const std::set<std::string> listed = listedTools(h);
    for (const char* tool : {"review_exportPackage", "review_importPackage", "cdl_import", "cdl_export", "lut_import",
                             "lottie_import", "project_merge"}) {
        INFO(tool);
        CHECK(listed.contains(tool));
    }
    CHECK_FALSE(listed.contains("plugins_scan"));

    // Inside: it runs.
    const Json inside = h.ok("review_exportPackage", Json{{"path", pathToUtf8(root / "package")}});
    CHECK(fs::exists(root / "package" / "notes.json"));
    CHECK(inside["data"]["notes"] == 0);

    const auto refused = [&](const Json& args, const char* why) {
        INFO(why << ": " << args.dump());
        const Json result = h.call("review_exportPackage", args);
        INFO(result.dump());
        CHECK(result["isError"] == true);
        CHECK(result["structuredContent"]["error"]["code"] == "PermissionDenied");
        CHECK(result["structuredContent"]["error"]["details"]["confined"] == true);
    };
    refused(Json{{"path", pathToUtf8(outside / "package")}}, "outside");
    refused(Json{{"path", pathToUtf8(root / ".." / "elsewhere" / "package")}}, "dot-dot");
    refused(Json{{"path", "package"}}, "relative");
    refused(Json{{"path", pathToUtf8(base)}}, "the parent");
    std::error_code ec;
    fs::create_directory_symlink(outside, root / "link", ec);
    if (!ec) {
        refused(Json{{"path", pathToUtf8(root / "link" / "package")}}, "through a link");
    } else {
        WARN("no symlink privilege here: the link case is not exercised (" << ec.message() << ")");
    }
    CHECK(fs::is_empty(outside));

    // A plug-in scan is never served, whatever the folders.
    const Json scan = h.call("plugins_scan", Json{{"addFolders", Json::array({pathToUtf8(root)})}});
    CHECK(scan["isError"] == true);
    CHECK(scan["structuredContent"]["error"]["code"] == "PermissionDenied");
    CHECK(scan["structuredContent"]["error"]["details"]["denied"] == true);
}
