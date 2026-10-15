#pragma once

// The command-related wabcut-cli subcommands, as plain functions (main.cpp only parses arguments), plus the
// catalog reference generator behind docs/commands.md.
//
//   wabcut-cli cmd <project.wabcut> --json '<request or batch>' [--dry-run] [--save]
//   wabcut-cli tools --format anthropic|openai|jsonschema

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace wabcut::command {

struct CliOutcome {
    int exitCode = 0;  ///< 0 ok, 1 the command failed, 2 bad usage or unreadable project
    std::string out;   ///< JSON for stdout
    std::string err;   ///< diagnostics for stderr
};

/// Loads the project, runs the request or batch, prints the result as JSON. With `save` (and a committed
/// change) the project is saved back atomically with its rotated .bak copies. `dryRun` forces a dry run.
/// `registry` and `services` are what the engine serves (wabcut-cli: the transcript commands and transcripts).
CliOutcome runCommandCli(const std::filesystem::path& project, std::string_view requestJson, bool dryRun, bool save,
                         const CommandRegistry& registry = builtinCommands(),
                         std::shared_ptr<CommandServices> services = nullptr);

/// The tool definitions in the given format ("anthropic", "openai" or "jsonschema").
CliOutcome runToolsCli(std::string_view format, const CommandRegistry& registry = builtinCommands());

/// docs/commands.md: the catalog reference, generated from the registry.
std::string catalogMarkdown(const CommandRegistry& registry);

} // namespace wabcut::command
