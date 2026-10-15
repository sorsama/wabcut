#pragma once

// `wabcut-cli mcp`: serves one project over stdio. main.cpp only hands over the arguments.
//
//   wabcut-cli mcp --project <file.wabcut> [--allow-edit] [--allow-analysis] [--save-on-exit | --autosave]
//                  [--timeout-ms <n>]
//
// Read-only unless --allow-edit; the analysis commands (transcript.transcribe, audio.loudness) only with
// --allow-analysis. stdout carries only protocol messages; diagnostics go to stderr.

#include <wabcut/command/mcp/McpServer.h>

#include <chrono>
#include <expected>
#include <filesystem>
#include <span>
#include <vector>
#include <string>
#include <string_view>

namespace wabcut::command::mcp {

struct McpCliOptions {
    std::filesystem::path project;
    bool allowEdit = false;
    bool allowAnalysis = false; ///< serve the MediaAnalysis commands (they decode media and can run long)
    /// Serve the commands that read or write files, confined to the project's folder and `filesFolders` (S5-P1-1).
    bool allowFiles = false;
    std::vector<std::filesystem::path> filesFolders; ///< --files-folder (each implies allowFiles)
    SaveMode saveMode = SaveMode::Manual;
    std::chrono::milliseconds timeout{30'000};
};

/// Parses the arguments after "mcp"; the error is a one-line usage problem.
std::expected<McpCliOptions, std::string> parseMcpArguments(std::span<const std::string_view> args);

/// The `mcp` usage text.
std::string_view mcpUsage();

/// Loads the project and serves it on stdin/stdout until the client disconnects. Returns the process exit
/// code: 0 after a normal disconnect, 2 when the project cannot be loaded. `registry` and `services` are what
/// the tools are made of (wabcut-cli: the transcript commands and the transcripts of the project's media).
int runMcpStdio(const McpCliOptions& options, std::string serverVersion,
                const CommandRegistry& registry = builtinCommands(), std::shared_ptr<CommandServices> services = nullptr);

} // namespace wabcut::command::mcp
