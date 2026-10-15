// C.11 MCP parity contract: every catalog command's documented example, run once through the CLI path
// (runCommandCli on a project file) and once as an MCP tool call on the same project, gives the same outcome - the
// same result data when it succeeds, the same error code when it fails. Commands MCP must not serve (file commands,
// network, analysis without --allow-analysis) are asserted refused instead: no command is skipped (S5-P1-1). AI-13
// found MCP returning a single edit tool's data only inside edit_batch steps; this keeps the two in step.

#include "McpHarness.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Cli.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <set>
#include <filesystem>
#include <string>

using namespace wabcut;
using namespace wabcut::test;
using command::Json;
namespace fs = std::filesystem;

namespace {

constexpr std::string_view kPicture = R"(
    V2     |..Dddd..............|
    V1 tgt |AaaaBbbbCccc........|
    A1 tgt |AaaaBbbbCccc........|
)";

/// The JSON without the keys that hold the time of day (two runs a moment apart differ there).
Json withoutClockTimes(Json j) {
    if (j.is_object()) {
        j.erase("createdAt");
        for (auto& [key, value] : j.items()) {
            value = withoutClockTimes(value);
        }
    } else if (j.is_array()) {
        for (Json& value : j) {
            value = withoutClockTimes(value);
        }
    }
    return j;
}

} // namespace

TEST_CASE("MCP serves every command as the CLI does or refuses it by its traits (C.11 and S5-P1-1)", "[command][mcp][parity]") {
    // Every command of the catalog the CLI serves, each with one assertion per trait - none is skipped:
    //   Network          never served: not listed, unknown by name
    //   touchesFiles     not listed, refused by name with PermissionDenied (an LLM client must not pick paths)
    //   MediaAnalysis    listed only with --allow-analysis, unknown by name without it
    //   Read + runsModel served read-only (they read the project and media, never change them; docs/mcp.md)
    //   everything else  its documented example gives the same result data (or the same error code) as the CLI
    const command::CommandRegistry& catalog = ai::edit::editingCommands();
    Timeline tl(kPicture);
    tl.linkMatching();
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "mcp-parity";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path project = dir / "parity.wabcut";
    REQUIRE(io::saveProject(tl.state, project, io::SaveOptions{0}));

    command::mcp::McpServerOptions options;
    options.allowed = command::CapabilitySet::of(
        {command::Capability::Read, command::Capability::EditSequence, command::Capability::EditProject});
    command::mcp::McpServerOptions analysis = options;
    analysis.allowed.bits |= command::CapabilitySet::of({command::Capability::MediaAnalysis}).bits;
    const auto names = [&](const command::mcp::McpServerOptions& o) {
        McpHarness h(Timeline(tl), o, catalog);
        std::set<std::string> out;
        for (const Json& tool : h.server.tools()) {
            out.insert(tool["name"].get<std::string>());
        }
        return out;
    };
    const std::set<std::string> listed = names(options);
    const std::set<std::string> listedWithAnalysis = names(analysis);
    const std::set<std::string> listedReadOnly = names(readOnlyOptions());
    const auto unknownByName = [&](const std::string& tool) {
        McpHarness h(Timeline(tl), options, catalog);
        const Json reply = h.one(callLine(1, tool, Json::object()));
        INFO(reply.dump());
        CHECK(reply.contains("error"));
        CHECK(reply["error"]["message"].get<std::string>().find("Unknown tool") != std::string::npos);
    };

    std::size_t compared = 0;
    std::size_t withData = 0;
    std::size_t files = 0;
    const auto specs = catalog.all();
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const command::CommandSpec& spec = specs[i];
        if (i + 1 < specs.size() && specs[i + 1].name == spec.name) {
            continue; // MCP serves the newest version of each command
        }
        const std::string tool = command::toolNameFor(spec.name);
        INFO("command " << spec.name);
        if (spec.capability == command::Capability::Network) {
            CHECK_FALSE(listedWithAnalysis.contains(tool));
            unknownByName(tool);
            continue;
        }
        if (spec.touchesFiles) {
            ++files;
            CHECK_FALSE(listedWithAnalysis.contains(tool));
            McpHarness h(Timeline(tl), analysis, catalog);
            const Json result = h.call(tool, Json::object());
            INFO(result.dump());
            CHECK(result["isError"] == true);
            CHECK(result["structuredContent"]["error"]["code"] == "PermissionDenied");
            CHECK(result["structuredContent"]["error"]["details"]["touchesFiles"] == true);
            continue;
        }
        if (spec.capability == command::Capability::MediaAnalysis) {
            CHECK_FALSE(listed.contains(tool));
            CHECK(listedWithAnalysis.contains(tool));
            unknownByName(tool);
            continue;
        }
        if (spec.capability == command::Capability::Read) {
            CHECK(listedReadOnly.contains(tool)); // runsModel reads included (search.footage, shorts.find, takes.find)
        } else {
            CHECK(listed.contains(tool));
            CHECK_FALSE(listedReadOnly.contains(tool));
        }
        const Json example = spec.example.is_object() ? spec.example : Json::object();
        const Json request{{"command", spec.name}, {"params", example}};
        const command::CliOutcome cli = command::runCommandCli(project, request.dump(), false, false, catalog);
        REQUIRE(cli.exitCode <= 1);
        const Json viaCli = Json::parse(cli.out);

        McpHarness mcp(Timeline(tl), options, catalog);
        mcp.session.reset(tl.state);
        const Json result = mcp.call(tool, example);
        const Json& structured = result["structuredContent"];
        INFO("CLI: " << viaCli.dump() << "\nMCP: " << structured.dump());
        if (viaCli.value("ok", false)) {
            REQUIRE(result["isError"] == false);
            const Json cliData = viaCli.value("data", Json());
            const Json mcpData = structured.value("data", Json());
            CHECK(withoutClockTimes(mcpData) == withoutClockTimes(cliData));
            withData += cliData.is_null() ? 0 : 1;
        } else {
            REQUIRE(result["isError"] == true);
            CHECK(structured["error"]["code"] == viaCli["error"]["code"]);
        }
        ++compared;
    }
    INFO(compared << " commands compared, " << withData << " with data, " << files << " file commands");
    CHECK(compared > 200);
    CHECK(withData > 20);
    CHECK(files >= 9);
}
