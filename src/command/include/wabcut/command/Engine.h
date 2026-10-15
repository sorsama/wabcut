#pragma once

// CommandEngine: runs requests and batches against an EditSession.
//
// Every request is checked before anything changes, in release builds too:
//   1. the command and version exist, and the client holds its capability;
//   2. expectedRevision (when given) is the session's current revision;
//   3. the params pass the command's JSON Schema, and the references in them resolve;
//   4. the result passes validate() and leaves every locked track's clips and transitions as they were.
// A dry run stops there and returns the ChangeSet plus a preview snapshot. Otherwise the new state is
// committed as one undo step (a batch too) labelled with the command's title, and the ChangeSet is the
// session's diff. Idempotency keys make replays return the first result. The engine belongs to the
// session's thread (the GUI thread), like EditSession's mutating members.
//
// An optional ExecutionControl (ExecutionContext.h) follows a long request: it is asked before each command
// of a batch, inside long commands and right before the commit, and can stop the request there (Cancelled /
// TimedOut, nothing committed).

#include <wabcut/command/Command.h>
#include <wabcut/command/Registry.h>
#include <wabcut/model/EditSession.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

namespace wabcut::command {

class ExecutionControl;
class CommandServices;

struct EnginePolicy {
    /// Capabilities this client may use (an agent might get Read + EditSequence only).
    CapabilitySet allowed = CapabilitySet::all();
    /// Idempotency keys remembered (oldest forgotten first).
    std::size_t idempotencyCacheSize = 1024;
    /// What commands may need besides the project (transcripts, ...); see CommandServices.
    std::shared_ptr<CommandServices> services;
    /// Whether commands that run an AI model (CommandSpec::runsModel) may run; false refuses them with
    /// PermissionDenied whatever their capability (an extension without MediaAnalysis, C.12).
    bool allowModelCommands = true;
    /// Whether commands that read or write the files their parameters name (CommandSpec::touchesFiles) may run; false
    /// refuses them with PermissionDenied (extensions, C.12; MCP clients and the assistant, S5-P1-1) - unless
    /// `fileRoots` confines them.
    bool allowFileCommands = true;
    /// When allowFileCommands is false: folders file commands may still use (MCP's --allow-files: the project's folder
    /// and a user-configured allow-list). Such a command runs only when every file its CommandSpec::fileParams name is an
    /// absolute path inside one of them once symlinks and junctions are resolved; empty = file commands are refused.
    std::vector<std::filesystem::path> fileRoots;
    /// Commands this client never runs, whatever its capabilities ("plugins.scan": loading native code is the app's).
    std::vector<std::string> deniedCommands;
    /// Prefix of every undo label this engine commits ("Beat Markers" -> "Beat Markers: Add Marker"), so the
    /// history names the client that made the edit; empty = the plain label.
    std::string undoLabelPrefix;
};

class CommandEngine {
public:
    explicit CommandEngine(EditSession& session, const CommandRegistry& registry = builtinCommands(),
                           EnginePolicy policy = {});

    CommandResult execute(const CommandRequest& request, ExecutionControl* control = nullptr);
    CommandResult execute(const CommandBatch& batch, ExecutionControl* control = nullptr);
    /// Parses a request or batch document and executes it.
    CommandResult executeJson(const Json& document, ExecutionControl* control = nullptr);

    std::uint64_t revision() const { return m_session.revision(); }
    const CommandRegistry& registry() const { return m_registry; }
    const EnginePolicy& policy() const { return m_policy; }
    EditSession& session() const { return m_session; }

private:
    struct Remembered {
        std::string fingerprint;
        CommandResult result;
    };

    CommandResult run(const std::vector<CommandRequest>& commands, const std::string& label,
                      std::optional<std::uint64_t> expectedRevision, const std::optional<std::string>& idempotencyKey,
                      bool dryRun, bool batch, ExecutionControl* control);
    CommandResult failure(CommandError error) const;
    void remember(const std::string& key, std::string fingerprint, const CommandResult& result);

    EditSession& m_session;
    const CommandRegistry& m_registry;
    EnginePolicy m_policy;
    std::unordered_map<std::string, Remembered> m_idempotency;
    std::deque<std::string> m_idempotencyOrder;
};

} // namespace wabcut::command
