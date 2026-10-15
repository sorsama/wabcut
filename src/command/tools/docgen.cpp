// wabcut_command_docgen <commands.md> [<mcp.md> [<extensions.md>]]: writes the command catalog reference
// (docs/commands.md) and the MCP server reference (docs/mcp.md) for the catalog wabcut-cli serves: the built-in commands
// plus the transcript commands (wabcut/ai/edit/TranscriptCommands.h); and regenerates the commands-by-capability section
// of docs/extensions.md (between its markers, wabcut/script/Docs.h).

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Cli.h>
#include <wabcut/command/mcp/McpDocs.h>
#include <wabcut/script/Docs.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

bool writeFile(const char* path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", path);
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 4) {
        std::fprintf(stderr, "usage: wabcut_command_docgen <commands.md> [<mcp.md> [<extensions.md>]]\n");
        return 2;
    }
    const auto& registry = wabcut::ai::edit::editingCommands();
    if (!writeFile(argv[1], wabcut::command::catalogMarkdown(registry))) {
        return 1;
    }
    if (argc >= 3 && !writeFile(argv[2], wabcut::command::mcp::mcpMarkdown(registry))) {
        return 1;
    }
    if (argc == 4) {
        std::ifstream in(argv[3], std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        const auto updated = wabcut::script::withExtensionCommands(text.str(), registry);
        if (!updated) {
            std::fprintf(stderr, "%s has no generated-section markers\n", argv[3]);
            return 1;
        }
        if (!writeFile(argv[3], *updated)) {
            return 1;
        }
    }
    return 0;
}
