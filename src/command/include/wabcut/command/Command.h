#pragma once

// The typed edit-command protocol: requests, batches, results and errors.
//
// AI agents, wabcut-cli, scripting and the MCP server never run code against the project. They send
// commands: a stable name, an integer version and JSON parameters validated against the command's JSON
// Schema. A command maps onto the model's EditOps/ProjectOps, can be previewed as a ChangeSet (dryRun)
// before it is committed, is checked against the session revision the client last saw, and becomes one
// undo step (a batch of commands too). See docs/commands.md for the catalog.

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/ProjectState.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <expected>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::command {

using Json = nlohmann::json;

/// What a command may touch. A client (agent, CLI session, MCP connection) is granted a set of these.
enum class Capability : std::uint8_t {
    Read,          ///< queries only
    EditSequence,  ///< timeline edits inside a sequence
    EditProject,   ///< project structure: tracks, bins, media, sequences
    MediaAnalysis, ///< analysis jobs (transcripts, shots, beats); none in the catalog yet
    Network,       ///< anything that leaves the machine; none in the catalog yet
};

std::string_view toString(Capability capability);
std::optional<Capability> capabilityFromString(std::string_view text);

struct CapabilitySet {
    std::uint32_t bits = 0;

    static CapabilitySet all() { return {0x1F}; }
    static CapabilitySet of(std::initializer_list<Capability> capabilities) {
        CapabilitySet s;
        for (const Capability c : capabilities) {
            s.bits |= 1u << static_cast<std::uint32_t>(c);
        }
        return s;
    }
    bool has(Capability c) const { return (bits & (1u << static_cast<std::uint32_t>(c))) != 0; }
};

/// Stable, machine-readable error codes (their names are the JSON strings).
enum class ErrorCode {
    UnknownCommand,      ///< no command (or alias) with that name
    UnsupportedVersion,  ///< the command exists, but not in that version
    InvalidRequest,      ///< the request envelope is malformed
    InvalidParams,       ///< params fail the command's schema or refer to impossible values
    NotFound,            ///< a referenced sequence, track, clip, media or marker does not exist
    PermissionDenied,    ///< the client lacks the command's capability
    StaleRevision,       ///< expectedRevision is not the session's current revision
    IdempotencyConflict, ///< an idempotency key was reused for a different request
    EditRejected,        ///< the edit itself is not possible (locked track, no marks, nothing to close, ...)
    InvariantViolation,  ///< the result would break a model invariant (never committed)
    LockedTrack,         ///< the result would change a locked track (never committed)
    Cancelled,           ///< the caller stopped the request (ExecutionControl); nothing was committed
    TimedOut,            ///< the request ran past the caller's deadline (ExecutionControl); nothing was committed
    Internal,
};

std::string_view toString(ErrorCode code);

struct CommandError {
    ErrorCode code = ErrorCode::Internal;
    std::string message; ///< diagnostic English; not for display, not stable
    Json details;        ///< machine-readable context, e.g. {"issues": [...]} or {"index": 2}
};

struct CommandRequest {
    std::string command;
    /// nullopt means the current version of the command.
    std::optional<int> version;
    Json params = Json::object();
    /// The session revision the client based the request on; a different current revision fails with
    /// StaleRevision.
    std::optional<std::uint64_t> expectedRevision;
    /// Replays with the same key return the first result instead of editing again (per session).
    std::optional<std::string> idempotencyKey;
    /// Compute the result and its ChangeSet without committing.
    bool dryRun = false;
};

/// Several commands applied in order as ONE undo step: all of them or none.
struct CommandBatch {
    std::vector<CommandRequest> commands;
    std::string label; ///< undo label; defaults to "Batch (N commands)"
    std::optional<std::uint64_t> expectedRevision;
    std::optional<std::string> idempotencyKey;
    bool dryRun = false;
};

/// What one command produced (one entry per command of a batch).
struct StepResult {
    std::string command;
    std::vector<std::uint64_t> createdIds;
    Json data; ///< query result of a read command, otherwise null
    std::vector<std::string> warnings;
};

struct CommandResult {
    /// Set when the request failed; nothing was changed then.
    std::optional<CommandError> error;
    /// The session revision after the request (unchanged by failures, dry runs and queries).
    std::uint64_t revision = 0;
    /// What changed, or would change for a dry run.
    ChangeSet changes;
    /// Ids created by the request (clips, tracks, link groups, markers have none).
    std::vector<std::uint64_t> createdIds;
    std::vector<std::string> warnings;
    /// Query result of a single read command, otherwise null.
    Json data;
    /// Per-command results of a batch.
    std::vector<StepResult> steps;
    /// Dry run: the state the request would produce (a snapshot handle; never published).
    std::shared_ptr<const ProjectState> preview;
    bool dryRun = false;
    bool committed = false; ///< a new undo step was created
    bool replayed = false;  ///< answered from the idempotency cache

    bool ok() const { return !error.has_value(); }
};

// --- JSON wire format -------------------------------------------------------------------------------------

/// {"command": "...", "version": 1, "params": {...}, "expectedRevision": 7, "idempotencyKey": "...", "dryRun": false}
std::expected<CommandRequest, CommandError> requestFromJson(const Json& json);
/// {"commands": [request, ...], "label": "...", "expectedRevision": 7, "idempotencyKey": "...", "dryRun": false}
std::expected<CommandBatch, CommandError> batchFromJson(const Json& json);
/// True for a batch document (it has a "commands" array).
bool isBatchJson(const Json& json);

Json toJson(const CommandRequest& request);
Json toJson(const CommandBatch& batch);
/// The result as JSON. A dry-run preview is reported as {"preview": true} plus the ChangeSet; the snapshot
/// itself stays in process.
Json toJson(const CommandResult& result);
Json toJson(const ChangeSet& changes);
Json toJson(const CommandError& error);

} // namespace wabcut::command
