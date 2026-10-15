#pragma once

// Drives an McpServer over an in-memory transport: a Timeline picture (tests/model/TimelineFixture.h) in an
// EditSession, JSON-RPC lines in, parsed replies out.

#include "TimelineFixture.h"

#include <wabcut/command/Schema.h>
#include <wabcut/command/mcp/McpServer.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

namespace wabcut::test {

using command::Json;

/// Commands in the built-in catalog, and how many of them only read.
inline std::size_t catalogSize() { return command::builtinCommands().all().size(); }
/// The commands an MCP server lists: the catalog without the ones that touch files (S5-P1-1).
inline std::size_t servedCatalogSize() {
    std::size_t n = 0;
    for (const command::CommandSpec& s : command::builtinCommands().all()) {
        // File commands need --allow-files; analysis commands (C.15's render.*) --allow-analysis; Network is never served.
        const bool hidden = s.touchesFiles || s.capability == command::Capability::MediaAnalysis ||
                            s.capability == command::Capability::Network;
        n += hidden ? 0 : 1;
    }
    return n;
}
inline std::size_t readCommandCount(const command::CommandRegistry& registry = command::builtinCommands()) {
    std::size_t n = 0;
    for (const command::CommandSpec& s : registry.all()) {
        // File commands (interchange.export, X.1) are listed only when the server may touch files.
        n += s.capability == command::Capability::Read && !s.touchesFiles ? 1 : 0;
    }
    return n;
}

/// _meta for a 2026-07-28 request.
inline Json modernMeta() {
    return Json{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                {"io.modelcontextprotocol/clientCapabilities", Json::object()}};
}

/// A 2026-07-28 request line.
inline std::string modernRequest(Json id, const std::string& method, Json params = Json::object()) {
    Json meta = modernMeta();
    if (params.contains("_meta")) {
        meta.update(params["_meta"]);
    }
    params["_meta"] = std::move(meta);
    return Json{{"jsonrpc", "2.0"}, {"id", std::move(id)}, {"method", method}, {"params", std::move(params)}}.dump();
}

/// A request line without _meta (legacy session or malformed on purpose).
inline std::string plainRequest(Json id, const std::string& method, Json params = Json::object()) {
    return Json{{"jsonrpc", "2.0"}, {"id", std::move(id)}, {"method", method}, {"params", std::move(params)}}.dump();
}

inline std::string notification(const std::string& method, Json params = Json::object()) {
    return Json{{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}}.dump();
}

inline std::string callLine(Json id, const std::string& tool, Json arguments = Json::object(), Json meta = Json::object()) {
    Json params{{"name", tool}, {"arguments", std::move(arguments)}};
    if (!meta.empty()) {
        params["_meta"] = std::move(meta);
    }
    return modernRequest(std::move(id), "tools/call", std::move(params));
}

inline command::mcp::McpServerOptions editOptions() {
    command::mcp::McpServerOptions o;
    o.allowed = command::CapabilitySet::of(
        {command::Capability::Read, command::Capability::EditSequence, command::Capability::EditProject});
    o.serverVersion = "test";
    return o;
}

inline command::mcp::McpServerOptions readOnlyOptions() {
    command::mcp::McpServerOptions o;
    o.serverVersion = "test";
    return o;
}

struct McpHarness {
    /// `registry` must outlive the harness.
    McpHarness(Timeline timeline, command::mcp::McpServerOptions options,
               const command::CommandRegistry& registry = command::builtinCommands())
        : tl(std::move(timeline)), session(tl.state), server(session, std::move(options), registry) {}
    McpHarness(std::string_view picture, command::mcp::McpServerOptions options,
               const command::CommandRegistry& registry = command::builtinCommands())
        : McpHarness(Timeline(picture), std::move(options), registry) {}

    /// Feeds the lines (one serve() run) and returns every message the server wrote.
    std::vector<Json> exchange(std::vector<std::string> lines) {
        command::mcp::MemoryTransport transport(std::move(lines));
        server.serve(transport);
        std::vector<Json> out;
        for (const std::string& line : transport.output) {
            INFO(line);
            REQUIRE(line.find('\n') == std::string::npos);
            out.push_back(Json::parse(line));
            CHECK(out.back()["jsonrpc"] == "2.0");
        }
        return out;
    }

    /// One request; requires exactly one reply and returns it.
    Json one(std::string line) {
        const auto out = exchange({std::move(line)});
        REQUIRE(out.size() == 1);
        return out.front();
    }

    /// tools/call; returns the CallToolResult after checking it against the tool's outputSchema.
    Json call(const std::string& tool, Json arguments = Json::object()) {
        const Json reply = one(callLine(++nextId, tool, std::move(arguments)));
        INFO(reply.dump());
        REQUIRE(reply.contains("result"));
        checkToolResult(tool, reply["result"]);
        return reply["result"];
    }

    /// The structured result of a successful call.
    Json ok(const std::string& tool, Json arguments = Json::object()) {
        const Json result = call(tool, std::move(arguments));
        INFO(result.dump());
        REQUIRE(result["isError"] == false);
        return result["structuredContent"];
    }

    /// The error code of a call that ended with isError.
    std::string toolError(const std::string& tool, Json arguments = Json::object()) {
        const Json result = call(tool, std::move(arguments));
        INFO(result.dump());
        REQUIRE(result["isError"] == true);
        return result["structuredContent"]["error"]["code"].get<std::string>();
    }

    void checkToolResult(const std::string& tool, const Json& result) {
        CHECK(result["resultType"] == "complete");
        REQUIRE(result["content"].size() == 1);
        CHECK(result["content"][0]["type"] == "text");
        const Json& structured = result["structuredContent"];
        if (!result["isError"].get<bool>()) {
            // The text block is the same JSON (MCP: structured content is also returned serialized).
            CHECK(Json::parse(result["content"][0]["text"].get<std::string>()) == structured);
        }
        for (const Json& t : server.tools()) {
            if (t["name"] == tool) {
                const auto issues = command::validateAgainstSchema(t["outputSchema"], structured);
                INFO(structured.dump());
                CHECK(issues.empty());
            }
        }
    }

    std::uint64_t revision() const { return session.revision(); }
    ProjectSnapshot state() const { return session.snapshot(); }

    Timeline tl;
    EditSession session;
    command::mcp::McpServer server;
    int nextId = 100;
};

/// The reply with this id among `messages`, or null.
inline Json replyFor(const std::vector<Json>& messages, const Json& id) {
    for (const Json& m : messages) {
        if (m.contains("id") && m["id"] == id && !m.contains("method")) {
            return m;
        }
    }
    return nullptr;
}

} // namespace wabcut::test
