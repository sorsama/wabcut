#pragma once

// A local Model Context Protocol server over the command catalog, so external agents (Claude Desktop/Code,
// Codex, Cursor, ...) can read and, when allowed, edit a project. Pure C++, no network: it speaks JSON-RPC 2.0
// over an McpTransport (stdio for `wabcut-cli mcp`, in memory for tests).
//
// Protocol: MCP 2026-07-28 (stateless, per-request `_meta`, `server/discover`), and the earlier
// handshake-based revisions 2025-11-25 and 2025-06-18 for clients that open with `initialize` (a
// "dual-era" server in the spec's terms). Details and the safety model: docs/mcp.md.
//
//   tools        the catalog (Registry.h) as tools named with '_' (timeline_rippleDelete), plus edit_batch,
//                edit_undo, edit_redo and project_save. Edit tools take dryRun / expectedRevision /
//                idempotencyKey and return a ChangeSet summary plus the resulting clip layout.
//   resources    wabcut://project/summary and wabcut://sequence/{id} (compact JSON with the revision).
//   utilities    ping, notifications/cancelled, notifications/progress.
//
// Requests are handled one at a time. While a call runs, the server polls the transport at the engine's
// checkpoints (ExecutionControl), so a cancellation or the call timeout stops it before anything is committed.

#include <wabcut/command/Engine.h>
#include <wabcut/command/mcp/McpTransport.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::command::mcp {

/// The revision served statelessly (per-request `_meta`).
inline constexpr std::string_view kProtocolVersion = "2026-07-28";
/// Revisions served after an `initialize` handshake, newest first.
std::span<const std::string_view> legacyProtocolVersions();

enum class SaveMode {
    Manual, ///< only the project_save tool writes the file
    OnExit, ///< also save when the client disconnects, if anything changed
    Auto,   ///< also save after every change (edit, undo, redo)
};

/// One tool call, for the audit trail (McpServerOptions::audit).
struct McpAuditEntry {
    std::string tool;      ///< the tool as called ("clip_addEffect", "edit_batch")
    bool ok = true;        ///< false: the call came back as an error
    std::string code;      ///< the error code when it failed ("PermissionDenied")
    std::uint64_t revision = 0; ///< the session revision after the call
    bool committed = false;     ///< the call changed the project
    bool dryRun = false;
    std::size_t commands = 0;   ///< commands in the call (an edit_batch has several)
};

struct McpServerOptions {
    /// What the client may do. Read is always granted; EditSequence / EditProject enable the edit tools;
    /// MediaAnalysis the analysis commands (transcript.transcribe, audio.loudness; wabcut-cli mcp --allow-analysis).
    /// Network commands are never served.
    CapabilitySet allowed = CapabilitySet::of({Capability::Read});
    /// The project file. project_save writes it (or a copy next to it); empty means nothing can be saved.
    std::filesystem::path projectPath;
    /// Folders the commands that read or write files (CommandSpec::touchesFiles: review packages, CDL and LUT files,
    /// project.merge, lottie.import, ...) may use (wabcut-cli mcp --allow-files: the project's folder, plus each
    /// --files-folder). Empty (the default): those commands are not listed and are refused by name (S5-P1-1). Every
    /// path they are given must lie inside one of these folders once symlinks and junctions are resolved, and
    /// plugins.scan (native code) is never served.
    std::vector<std::filesystem::path> fileRoots;
    /// Commands that run a model (CommandSpec::runsModel) need MediaAnalysis in `allowed` even when they only read: off by
    /// default (wabcut-cli mcp serves the read-only ones), on for a Python script, whose "analyse media and use AI features"
    /// switch is MediaAnalysis.
    bool holdModelCommands = false;
    SaveMode saveMode = SaveMode::Manual;
    /// Longest a tool call may run. Checked at the engine's checkpoints (between batch steps, inside long
    /// commands, before committing); work between two checkpoints always finishes.
    std::chrono::milliseconds callTimeout{30'000};
    /// Largest accepted message (a longer line is refused unread).
    std::size_t maxMessageBytes = std::size_t{4} * 1024 * 1024;
    /// Deepest accepted JSON nesting.
    std::size_t maxJsonDepth = 64;
    /// Commits must name the revision they are based on. When off, a commit without expectedRevision is
    /// based on the current revision and the result says so (`assumedRevision`).
    bool requireExpectedRevision = false;
    std::string serverVersion = "dev";
    /// What commands may need besides the project (transcripts for the transcript.* tools).
    std::shared_ptr<CommandServices> services;
    /// Serves the edit_transaction tool (`wabcut-cli mcp --transactions`, 0.7): the Python package opens one around a
    /// script, so everything the script edits is ONE undo step - all of it or none: committed with the script, rolled
    /// back when the script fails, is cancelled or the connection drops. While one is open undo, redo and project_save
    /// are refused and autosave waits. Off by default: an AI agent gets only the plain tools.
    bool transactions = false;
    /// Called after every tool call (the audit trail: `wabcut-cli mcp --audit-log`). Never sees parameters or results.
    std::function<void(const McpAuditEntry&)> audit;
    /// Diagnostics (the CLI prints them to stderr). Never protocol output.
    std::function<void(std::string_view)> log;
    /// Time source for the call timeout (tests replace it).
    std::function<std::chrono::steady_clock::time_point()> clock;
};

class McpServer {
public:
    McpServer(EditSession& session, McpServerOptions options, const CommandRegistry& registry = builtinCommands());
    ~McpServer();
    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    /// Serves until the transport's input ends, then saves if saveMode is OnExit and something changed.
    void serve(McpTransport& transport);
    /// Handles one incoming message; replies and notifications go to `transport`.
    void handle(std::string_view line, McpTransport& transport);
    /// Ends the session as serve() does when its input ends: an open transaction is rolled back, and the project is
    /// saved when saveMode is OnExit and something changed. For hosts that feed handle() themselves. Idempotent.
    void finish();

    /// The tools this server offers (the tools/list entries).
    Json tools() const;
    /// The session revision last written to projectPath (0 = loaded state, never saved).
    std::uint64_t savedRevision() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace wabcut::command::mcp
