// MCP server conformance: lifecycle and version negotiation (2026-07-28 per request, 2025-11-25 / 2025-06-18
// by handshake), JSON-RPC errors, resources, cancellation, progress and the call timeout.

#include "McpHarness.h"

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::test;
namespace mcp = wabcut::command::mcp;

namespace {

constexpr const char* kPicture = R"(
    V1 tgt |Aaaa..Bbbb|
    A1 tgt |Aaaa..Bbbb|
)";

std::string initializeLine(Json id, const std::string& version) {
    return plainRequest(std::move(id), "initialize",
                        Json{{"protocolVersion", version},
                             {"capabilities", Json::object()},
                             {"clientInfo", Json{{"name", "conformance"}, {"version", "1"}}}});
}

int errorCode(const Json& reply) {
    INFO(reply.dump());
    REQUIRE(reply.contains("error"));
    return reply["error"]["code"].get<int>();
}

} // namespace

TEST_CASE("Legacy clients get the initialize handshake", "[command][mcp][lifecycle]") {
    McpHarness h(kPicture, readOnlyOptions());
    const Json init = h.one(initializeLine(1, "2025-11-25"));
    INFO(init.dump());
    REQUIRE(init.contains("result"));
    const Json& r = init["result"];
    CHECK(r["protocolVersion"] == "2025-11-25");
    CHECK(r["serverInfo"]["name"] == "wabcut");
    CHECK(r["serverInfo"]["version"] == "test");
    CHECK(r["capabilities"]["tools"].is_object());
    CHECK(r["capabilities"]["resources"].is_object());
    CHECK(r["instructions"].get<std::string>().find("read-only") != std::string::npos);
    CHECK_FALSE(r.contains("resultType")); // a 2026-07-28 field

    // initialized is a notification: no reply. Afterwards requests need no _meta.
    CHECK(h.exchange({notification("notifications/initialized")}).empty());
    const Json list = h.one(plainRequest(2, "tools/list"));
    REQUIRE(list.contains("result"));
    CHECK(list["result"]["tools"].size() == readCommandCount());
    CHECK_FALSE(list["result"].contains("resultType"));

    // A second initialize is refused.
    CHECK(errorCode(h.one(initializeLine(3, "2025-11-25"))) == -32600);
}

TEST_CASE("Version negotiation", "[command][mcp][lifecycle]") {
    SECTION("an older revision the server serves is echoed") {
        McpHarness h(kPicture, readOnlyOptions());
        CHECK(h.one(initializeLine(1, "2025-06-18"))["result"]["protocolVersion"] == "2025-06-18");
    }
    SECTION("an unknown revision gets the server's newest legacy revision (the client may disconnect)") {
        McpHarness h(kPicture, readOnlyOptions());
        CHECK(h.one(initializeLine(1, "2024-11-05"))["result"]["protocolVersion"] == "2025-11-25");
    }
    SECTION("a 2026-07-28 request with an unsupported version fails with -32022 and the supported list") {
        McpHarness h(kPicture, readOnlyOptions());
        Json params{{"_meta", Json{{"io.modelcontextprotocol/protocolVersion", "1900-01-01"},
                                   {"io.modelcontextprotocol/clientCapabilities", Json::object()}}}};
        const Json reply = h.one(plainRequest(1, "tools/list", params));
        CHECK(errorCode(reply) == -32022);
        CHECK(reply["error"]["data"]["supported"] == Json::array({"2026-07-28"}));
        CHECK(reply["error"]["data"]["requested"] == "1900-01-01");
    }
    SECTION("2026-07-28 requests must carry clientCapabilities") {
        McpHarness h(kPicture, readOnlyOptions());
        Json params{{"_meta", Json{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}}}};
        CHECK(errorCode(h.one(plainRequest(1, "tools/list", params))) == -32602);
    }
    SECTION("without _meta and without initialize, the error names every served revision") {
        McpHarness h(kPicture, readOnlyOptions());
        const Json reply = h.one(plainRequest(1, "tools/list"));
        CHECK(errorCode(reply) == -32602);
        CHECK(reply["error"]["data"]["supported"] == Json::array({"2026-07-28", "2025-11-25", "2025-06-18"}));
    }
}

TEST_CASE("2026-07-28 clients use server/discover and per-request metadata", "[command][mcp][lifecycle]") {
    McpHarness h(kPicture, editOptions());
    const Json discover = h.one(plainRequest("d1", "server/discover"));
    INFO(discover.dump());
    const Json& r = discover["result"];
    CHECK(discover["id"] == "d1");
    CHECK(r["resultType"] == "complete");
    CHECK(r["supportedVersions"] == Json::array({"2026-07-28"}));
    CHECK(r["_meta"]["io.modelcontextprotocol/serverInfo"]["name"] == "wabcut");
    CHECK(r["capabilities"]["tools"].is_object());
    CHECK(r["instructions"].get<std::string>().find("dryRun") != std::string::npos);

    const Json list = h.one(modernRequest(2, "tools/list"));
    CHECK(list["result"]["resultType"] == "complete");
    CHECK(list["result"]["tools"].size() == servedCatalogSize() + 3); // + batch, undo, redo (no project file: no save)

    // ping works in both eras, before anything else.
    CHECK(h.one(plainRequest(3, "ping"))["result"]["resultType"] == "complete");
    CHECK(h.one(modernRequest(4, "ping"))["result"] == Json{{"resultType", "complete"}});
}

TEST_CASE("Malformed messages get JSON-RPC errors", "[command][mcp][errors]") {
    mcp::McpServerOptions options = readOnlyOptions();
    options.maxMessageBytes = 2048;
    options.maxJsonDepth = 16;
    McpHarness h(kPicture, options);

    const Json parse = h.one("{\"jsonrpc\": \"2.0\", \"id\": 1, \"method\": ");
    CHECK(errorCode(parse) == -32700);
    CHECK(parse["id"].is_null());
    CHECK(errorCode(h.one("[{\"jsonrpc\": \"2.0\", \"id\": 1, \"method\": \"ping\"}]")) == -32600); // no batches
    CHECK(errorCode(h.one("42")) == -32600);
    const Json version = h.one(R"({"jsonrpc": "1.0", "id": 7, "method": "ping"})");
    CHECK(errorCode(version) == -32600);
    CHECK(version["id"] == 7);
    CHECK(errorCode(h.one(R"({"jsonrpc": "2.0", "id": null, "method": "ping"})")) == -32600);
    CHECK(errorCode(h.one(R"({"jsonrpc": "2.0", "id": 1.5, "method": "ping"})")) == -32600);
    CHECK(errorCode(h.one(R"({"jsonrpc": "2.0", "id": 8})")) == -32600);
    CHECK(errorCode(h.one(R"({"jsonrpc": "2.0", "id": 9, "method": "ping", "params": [1]})")) == -32602);
    CHECK(errorCode(h.one(modernRequest(10, "prompts/list"))) == -32601);
    CHECK(errorCode(h.one(modernRequest(11, "tools/call", Json{{"arguments", Json::object()}}))) == -32602);
    CHECK(errorCode(h.one(callLine(12, "project_summary", Json::array()))) == -32602);
    const Json unknown = h.one(callLine(13, "timeline_explode"));
    CHECK(errorCode(unknown) == -32602);
    CHECK(unknown["error"]["message"] == "Unknown tool: timeline_explode");

    SECTION("limits") {
        const Json big = h.one(modernRequest(14, "ping", Json{{"pad", std::string(4096, 'x')}}));
        CHECK(errorCode(big) == -32600);
        CHECK(big["error"]["message"] == "Message too large");
        std::string deep = std::string(40, '[') + std::string(40, ']');
        CHECK(errorCode(h.one(R"({"jsonrpc": "2.0", "id": 15, "method": "ping", "params": {"x": )" + deep + "}}")) ==
              -32600);
    }
    SECTION("things that get no reply") {
        CHECK(h.exchange({"", "   ", notification("notifications/unknown"),
                          R"({"jsonrpc": "2.0", "id": 5, "result": {}})", // a response: we send no requests
                          notification("notifications/cancelled", Json{{"requestId", 999}}),
                          notification("notifications/cancelled", Json{{"bogus", true}})})
                  .empty());
    }
    SECTION("the server keeps going after errors") {
        const auto out = h.exchange({"garbage", modernRequest(20, "ping")});
        REQUIRE(out.size() == 2);
        CHECK(out[1]["id"] == 20);
    }
}

TEST_CASE("Resources: project summary and sequences", "[command][mcp][resources]") {
    McpHarness h(kPicture, readOnlyOptions());
    const Json list = h.one(modernRequest(1, "resources/list"))["result"];
    REQUIRE(list["resources"].size() == 2);
    CHECK(list["resources"][0]["uri"] == "wabcut://project/summary");
    const std::string sequenceUri = "wabcut://sequence/" + std::to_string(h.tl.seq.value);
    CHECK(list["resources"][1]["uri"] == sequenceUri);
    CHECK(list["resources"][1]["mimeType"] == "application/json");

    const Json templates = h.one(modernRequest(2, "resources/templates/list"))["result"];
    CHECK(templates["resourceTemplates"][0]["uriTemplate"] == "wabcut://sequence/{id}");

    const Json project = h.one(modernRequest(3, "resources/read", Json{{"uri", "wabcut://project/summary"}}))["result"];
    CHECK(project["resultType"] == "complete");
    REQUIRE(project["contents"].size() == 1);
    CHECK(project["contents"][0]["uri"] == "wabcut://project/summary");
    const Json summary = Json::parse(project["contents"][0]["text"].get<std::string>());
    CHECK(summary["revision"] == 0);
    CHECK(summary["sequences"][0]["name"] == "Fixture");

    const Json sequence = h.one(modernRequest(4, "resources/read", Json{{"uri", sequenceUri}}))["result"];
    const Json layout = Json::parse(sequence["contents"][0]["text"].get<std::string>());
    CHECK(layout["tracks"][0]["track"] == "V1");
    CHECK(layout["tracks"][0]["clips"].size() == 2);

    for (const char* bad : {"wabcut://sequence/999", "wabcut://sequence/abc", "wabcut://sequence/", "file:///etc/passwd",
                            "wabcut://project/other"}) {
        INFO(bad);
        const Json reply = h.one(modernRequest(5, "resources/read", Json{{"uri", bad}}));
        CHECK(errorCode(reply) == -32602);
        CHECK(reply["error"]["data"]["uri"] == bad);
    }
    CHECK(errorCode(h.one(modernRequest(6, "resources/read"))) == -32602);
}

TEST_CASE("Progress notifications follow a long call", "[command][mcp][progress]") {
    McpHarness h(R"(
        V1 tgt |Aaaaaaaaaa|
    )",
                 editOptions());
    const Json batch{{"commands", Json::array({Json{{"command", "sequence.addEdit"}, {"params", Json{{"time", 2}}}},
                                               Json{{"command", "marker.add"}, {"params", Json{{"time", 0}}}},
                                               Json{{"command", "sequence.summary"}}})}};
    const auto out = h.exchange({callLine(1, "edit_batch", batch, Json{{"progressToken", "tok"}})});
    std::vector<double> progress;
    for (const Json& m : out) {
        if (m.value("method", "") == "notifications/progress") {
            CHECK(m["params"]["progressToken"] == "tok");
            progress.push_back(m["params"]["progress"].get<double>());
        }
    }
    // One per step and one before the commit, counting up; the reply comes last.
    CHECK(progress == std::vector<double>{1, 2, 3, 4});
    REQUIRE(out.back()["id"] == 1);
    CHECK(out.back()["result"]["structuredContent"]["committed"] == true);
    CHECK(out.back()["result"]["structuredContent"]["steps"].size() == 3);

    // Without a progress token there are no notifications.
    CHECK(h.exchange({callLine(2, "sequence_summary", Json::object())}).size() == 1);
}

TEST_CASE("notifications/cancelled stops the call before anything is committed", "[command][mcp][cancel]") {
    McpHarness h(R"(
        V1 tgt |Aaaaaaaaaa|
    )",
                 editOptions());
    const Json batch{{"commands", Json::array({Json{{"command", "sequence.addEdit"}, {"params", Json{{"time", 2}}}},
                                               Json{{"command", "sequence.addEdit"}, {"params", Json{{"time", 4}}}}})}};

    SECTION("a running call: no reply, no change, the next request is served") {
        const auto out = h.exchange({callLine(1, "edit_batch", batch, Json{{"progressToken", 1}}),
                                     notification("notifications/cancelled",
                                                  Json{{"requestId", 1}, {"reason", "user pressed stop"}}),
                                     modernRequest(2, "ping")});
        CHECK(replyFor(out, 1).is_null());
        CHECK(h.revision() == 0);
        CHECK(h.session.undoCount() == 0);
        REQUIRE_FALSE(replyFor(out, 2).is_null());
        for (const Json& m : out) {
            CHECK(m.value("method", "") != "notifications/progress"); // stopped at the first checkpoint
        }
    }
    SECTION("a queued request is skipped") {
        const auto out = h.exchange({callLine(1, "edit_batch", batch), modernRequest("later", "ping"),
                                     notification("notifications/cancelled", Json{{"requestId", "later"}}),
                                     modernRequest(3, "ping")});
        CHECK_FALSE(replyFor(out, 1).is_null());
        CHECK(replyFor(out, "later").is_null());
        CHECK_FALSE(replyFor(out, 3).is_null());
        CHECK(h.revision() == 1);
    }
    SECTION("a cancellation for a finished request is ignored") {
        CHECK(h.exchange({callLine(1, "edit_batch", batch)}).size() == 1);
        CHECK(h.exchange({notification("notifications/cancelled", Json{{"requestId", 1}})}).empty());
        CHECK(h.revision() == 1);
        CHECK_FALSE(h.exchange({modernRequest(1, "ping")}).empty()); // not remembered as cancelled for later
    }
}

TEST_CASE("The call timeout stops a call at a checkpoint", "[command][mcp][timeout]") {
    mcp::McpServerOptions options = editOptions();
    options.callTimeout = std::chrono::milliseconds(0);
    McpHarness h(R"(
        V1 tgt |Aaaaaaaaaa|
    )",
                 options);
    CHECK(h.toolError("edit_batch", Json{{"commands", Json::array({Json{{"command", "sequence.addEdit"},
                                                                         {"params", Json{{"time", 2}}}}})}}) == "TimedOut");
    CHECK(h.toolError("timeline_removeSilences", Json{{"ranges", Json::array({Json{{"start", 0}, {"end", 2540160000000}}})}}) ==
          "TimedOut");
    CHECK(h.revision() == 0);
    // Reads and dry runs have no checkpoint left to stop at; they always finish.
    CHECK(h.ok("sequence_summary")["ok"] == true);
}
