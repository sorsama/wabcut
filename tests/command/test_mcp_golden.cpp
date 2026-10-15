// A golden transcript of a short MCP session: what a client such as Claude Desktop sends today (the
// 2025-11-25 handshake), then a dry run, a commit, an undo and a resource read, then the 2026-07-28 discovery.
// Any change to the wire format shows up as a diff of tests/command/golden/mcp_session.jsonl.
//
// To accept an intended change: set WABCUT_UPDATE_GOLDEN=1, run this test once, review the diff.

#include "McpHarness.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace wabcut;
using namespace wabcut::test;

namespace {

std::vector<std::string> sessionInput(SequenceId seq) {
    const Json firstClip = Json::array({Json{{"track", "V1"}, {"time", "00:00:00:01"}}});
    return {
        plainRequest(1, "initialize",
                     Json{{"protocolVersion", "2025-11-25"},
                          {"capabilities", Json::object()},
                          {"clientInfo", Json{{"name", "golden-client"}, {"version", "1.0"}}}}),
        notification("notifications/initialized"),
        plainRequest(2, "tools/call", Json{{"name", "project_summary"}, {"arguments", Json::object()}}),
        plainRequest(3, "tools/call",
                     Json{{"name", "timeline_rippleDelete"}, {"arguments", Json{{"clips", firstClip}, {"dryRun", true}}}}),
        plainRequest(4, "tools/call",
                     Json{{"name", "timeline_rippleDelete"},
                          {"arguments", Json{{"clips", firstClip}, {"expectedRevision", 0}}}}),
        plainRequest(5, "tools/call", Json{{"name", "edit_undo"}, {"arguments", Json{{"expectedRevision", 1}}}}),
        plainRequest(6, "resources/read", Json{{"uri", "wabcut://sequence/" + std::to_string(seq.value)}}),
        plainRequest(7, "server/discover"),
        modernRequest(8, "ping"),
    };
}

std::string readText(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    std::string s = text.str();
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

} // namespace

TEST_CASE("Golden transcript of a short MCP session", "[command][mcp][golden]") {
    McpHarness h(Timeline(R"(
        V1 tgt |Aaaa..Bbbb|
        A1 tgt |Aaaa..Bbbb|
    )")
                     .linkMatching(),
                 editOptions());
    std::string transcript;
    for (const std::string& line : sessionInput(h.tl.seq)) {
        transcript += "-> " + line + "\n";
        command::mcp::MemoryTransport transport({line});
        h.server.serve(transport);
        for (const std::string& reply : transport.output) {
            transcript += "<- " + reply + "\n";
        }
    }

    const std::filesystem::path golden = WABCUT_COMMAND_TEST_DIR "/golden/mcp_session.jsonl";
    if (const char* update = std::getenv("WABCUT_UPDATE_GOLDEN"); update && std::string(update) == "1") {
        std::filesystem::create_directories(golden.parent_path());
        std::ofstream(golden, std::ios::binary) << transcript;
        WARN("rewrote " << golden.string());
        return;
    }
    const std::string expected = readText(golden);
    if (expected != transcript) {
        const auto actual = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "command" / "mcp_session.actual.jsonl";
        std::filesystem::create_directories(actual.parent_path());
        std::ofstream(actual, std::ios::binary) << transcript;
        FAIL("the MCP transcript changed; compare " << golden.string() << " with " << actual.string()
                                                     << " and set WABCUT_UPDATE_GOLDEN=1 to accept it");
    }
}
