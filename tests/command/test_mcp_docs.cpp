// docs/mcp.md is generated and current, and its config snippets are valid.

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Registry.h>
#include <wabcut/command/mcp/McpDocs.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace wabcut::command;

namespace {

/// The bodies of the ```<language> fenced blocks.
std::vector<std::string> fencedBlocks(const std::string& md, const std::string& language) {
    std::vector<std::string> out;
    const std::string open = "```" + language + "\n";
    for (std::size_t at = md.find(open); at != std::string::npos; at = md.find(open, at)) {
        const std::size_t start = at + open.size();
        const std::size_t end = md.find("```", start);
        out.push_back(md.substr(start, end - start));
        at = end + 3;
    }
    return out;
}

} // namespace

TEST_CASE("docs/mcp.md is up to date", "[command][mcp][docs]") {
    std::ifstream file(WABCUT_DOCS_DIR "/mcp.md", std::ios::binary);
    REQUIRE(file);
    std::stringstream text;
    text << file.rdbuf();
    std::string onDisk = text.str();
    onDisk.erase(std::remove(onDisk.begin(), onDisk.end(), '\r'), onDisk.end());
    INFO("regenerate it: build\\win-msvc\\bin\\wabcut_command_docgen.exe docs/commands.md docs/mcp.md");
    CHECK(onDisk == mcp::mcpMarkdown(wabcut::ai::edit::editingCommands()));
}

TEST_CASE("The MCP setup snippets are valid client configuration", "[command][mcp][docs]") {
    const std::string md = mcp::mcpMarkdown(wabcut::ai::edit::editingCommands());
    const auto json = fencedBlocks(md, "json");
    REQUIRE(json.size() == 2); // Claude Desktop, Claude Code .mcp.json
    for (const std::string& block : json) {
        INFO(block);
        const Json config = Json::parse(block); // throws on invalid JSON
        const Json& server = config.at("mcpServers").at("wabcut");
        CHECK(server.at("command").is_string());
        const Json& args = server.at("args");
        REQUIRE(args.is_array());
        CHECK(args[0] == "mcp");
        CHECK(args[1] == "--project");
        CHECK(args[2].get<std::string>().ends_with(".wabcut"));
    }
    // The Claude Desktop entry points at an absolute Windows path with escaped backslashes.
    CHECK(Json::parse(json[0])["mcpServers"]["wabcut"]["command"] == "C:\\Program Files\\Wabcut\\wabcut-cli.exe");
    // Codex: a [mcp_servers.<name>] table with command and args.
    const auto toml = fencedBlocks(md, "toml");
    REQUIRE(toml.size() == 1);
    CHECK(toml[0].find("[mcp_servers.wabcut]\ncommand = \"wabcut-cli\"\nargs = [\"mcp\", \"--project\"") == 0);
    // Every tool is documented.
    CHECK(md.find("| `timeline_rippleDelete` |") != std::string::npos);
    CHECK(md.find("| `project_save` |") != std::string::npos);
    CHECK(md.find("MCP 2026-07-28") != std::string::npos);
}
