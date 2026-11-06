// wabcut_command_docgen <commands.md> [<mcp.md> [<extensions.md>]] [--python <package folder>]: writes the command
// catalog reference (docs/commands.md) and the MCP server reference (docs/mcp.md) for the catalog wabcut-cli serves: the
// built-in commands plus the transcript commands (wabcut/ai/edit/TranscriptCommands.h); regenerates the
// commands-by-capability section of docs/extensions.md (between its markers, wabcut/script/Docs.h); and, with --python,
// the generated files of the `wabcut` Python package (python/wabcut: catalog.json and _commands.pyi,
// wabcut/command/PythonApi.h). Without --python it writes them when python/wabcut sits next to the docs folder.

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Cli.h>
#include <wabcut/command/PythonApi.h>
#include <wabcut/command/mcp/McpDocs.h>
#include <wabcut/script/Docs.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool writeFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", path.string().c_str());
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<const char*> files;
    std::filesystem::path pythonFolder;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--python" && i + 1 < argc) {
            pythonFolder = argv[++i];
        } else {
            files.push_back(argv[i]);
        }
    }
    if (files.empty() || files.size() > 3) {
        std::fprintf(stderr,
                     "usage: wabcut_command_docgen <commands.md> [<mcp.md> [<extensions.md>]] [--python <package folder>]\n");
        return 2;
    }
    const auto& registry = wabcut::ai::edit::editingCommands();
    if (!writeFile(files[0], wabcut::command::catalogMarkdown(registry))) {
        return 1;
    }
    if (files.size() >= 2 && !writeFile(files[1], wabcut::command::mcp::mcpMarkdown(registry))) {
        return 1;
    }
    if (files.size() == 3) {
        std::ifstream in(files[2], std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        const auto updated = wabcut::script::withExtensionCommands(text.str(), registry);
        if (!updated) {
            std::fprintf(stderr, "%s has no generated-section markers\n", files[2]);
            return 1;
        }
        if (!writeFile(files[2], *updated)) {
            return 1;
        }
    }
    if (pythonFolder.empty()) {
        // The usual regeneration (`docs/commands.md docs/mcp.md docs/extensions.md` from the tree root) also refreshes the
        // Python package next to docs/, so its generated files cannot go stale behind the catalog.
        const std::filesystem::path beside =
            std::filesystem::absolute(files[0]).parent_path().parent_path() / "python" / "wabcut";
        std::error_code ec;
        if (std::filesystem::exists(beside / "__init__.py", ec)) {
            pythonFolder = beside;
        }
    }
    if (!pythonFolder.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(pythonFolder, ec);
        for (const auto& file : wabcut::command::pythonApiFiles(registry)) {
            if (!writeFile(pythonFolder / file.path, file.text)) {
                return 1;
            }
        }
    }
    return 0;
}
