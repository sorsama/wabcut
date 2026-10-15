// Fuzz harness: MCP JSON-RPC messages (command/mcp/McpServer.h) - an external agent's requests, untrusted. Each input is
// up to 16 lines, handled in order by a fresh server with every edit capability over a small project with every shape
// (ScaleProject.h) and the catalog without its file commands (FuzzRegistry.h). After each message every reply is one
// JSON-RPC 2.0 object and the project passes validate(Strict).

#include "FuzzSupport.h"

#include "FuzzRegistry.h"
#include "ScaleProject.h"

#include <wabcut/command/mcp/McpServer.h>
#include <wabcut/model/EditSession.h>

#include <chrono>

using namespace wabcut;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static const ProjectState base = test::makeScaleProject(test::smallScaleSpec()).state;
    EditSession session(base);
    command::mcp::McpServerOptions options;
    options.allowed = command::CapabilitySet::of(
        {command::Capability::Read, command::Capability::EditSequence, command::Capability::EditProject});
    options.callTimeout = std::chrono::seconds(5);
    command::mcp::McpServer server(session, options, fuzz::safeCommands());
    command::mcp::MemoryTransport transport;

    const std::string_view input = fuzz::text(data, size);
    std::size_t start = 0;
    for (int line = 0; line < 16 && start <= input.size(); ++line) {
        std::size_t end = input.find('\n', start);
        if (end == std::string_view::npos) {
            end = input.size();
        }
        server.handle(input.substr(start, end - start), transport);
        for (const std::string& reply : transport.output) {
            const command::Json j = command::Json::parse(reply, nullptr, false);
            WABCUT_FUZZ_CHECK(j.is_object() && j.value("jsonrpc", std::string()) == "2.0", reply);
        }
        transport.output.clear();
        const auto valid = validate(*session.snapshot(), Validation::Strict);
        WABCUT_FUZZ_CHECK(valid, valid ? std::string() : valid.error().message);
        start = end + 1;
    }
    return 0;
}
