#pragma once

// The command catalog: each command's stable name, version, capability, description, parameter schema
// and executor.
//
// Names are dotted and match the keymap's CommandIds where the action exists there ("clip.insert",
// "sequence.lift", "timeline.rippleDelete", "marker.add"); a command may also answer to aliases. LLM
// tool names use '_' for '.', since the major APIs only allow [A-Za-z0-9_-] (toolNameFor()).

#include <wabcut/command/Command.h>

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::command {

class ExecutionContext;

/// What an executor produced. Mutating commands return the new state; read commands leave it as the input
/// state and fill `data`.
struct CommandOutput {
    ProjectState state;
    std::vector<std::uint64_t> createdIds;
    Json data;
    std::vector<std::string> warnings;
    /// What the command does to the host besides the project (the clipboard, a version saved to disk). The engine
    /// runs these in order once the whole request or batch has succeeded, never for a dry run or a failure
    /// (E5-P2-7), so a previewed plan or a batch that fails later leaves the host as it was. Never throw.
    std::vector<std::function<void()>> afterCommit;
};

/// Maps validated params onto model operations. It reports problems by throwing CommandFailure (see
/// ExecutionContext.h); the engine catches it, so nothing escapes the command module.
using Executor = std::function<CommandOutput(ExecutionContext& context)>;

struct CommandSpec {
    std::string name;         ///< stable dotted name, e.g. "timeline.rippleDelete"
    int version = 1;
    std::string title;        ///< short human title, also the undo label, e.g. "Ripple Delete"
    std::string description;  ///< what it does, for people and LLM tool definitions
    Capability capability = Capability::Read;
    Json params;              ///< JSON Schema of the params object (Schema.h subset)
    Json example;             ///< example params (docs and tests)
    std::vector<std::string> aliases;
    Executor execute;
    /// The command can run an AI model: local inference (a helper such as wabcut-infer or wabcut-embed, the local
    /// assistant) or a cloud provider, also when only some parameters or a missing cache lead there. Hosts may hold
    /// such commands back (EnginePolicy::allowModelCommands): extensions need MediaAnalysis for them (C.12).
    bool runsModel = false;
    /// The command reads or writes files the caller names in its parameters (an export, an import, a merge), or loads
    /// code from them (plugins.scan). Hosts may hold such commands back (EnginePolicy::allowFileCommands): extensions
    /// never run them, since they have no file access (C.12).
    bool touchesFiles = false;
    /// The parameters of a touchesFiles command that name files or folders (S5-P1-1): a host that confines file access
    /// (EnginePolicy::fileRoots) checks them. A touchesFiles command that names none cannot run confined.
    std::vector<std::string> fileParams;

    bool mutates() const {
        return capability == Capability::EditSequence || capability == Capability::EditProject;
    }
};

class CommandRegistry {
public:
    /// Adds a command. Fails (and adds nothing) when the name+version or an alias is taken, or the schema
    /// uses keywords outside the supported subset.
    std::expected<void, std::string> add(CommandSpec spec);

    /// Looks a command up by name, alias or LLM tool name; nullopt version = the newest version.
    const CommandSpec* find(std::string_view name, std::optional<int> version = std::nullopt) const;
    /// True when `name` resolves to a command in some version.
    bool knows(std::string_view name) const;

    /// Every command, sorted by name then version.
    std::span<const CommandSpec> all() const { return m_specs; }

private:
    std::string canonicalName(std::string_view name) const;

    std::vector<CommandSpec> m_specs;
    std::vector<std::pair<std::string, std::string>> m_aliases; // alias -> name
};

/// The built-in catalog (built once, immutable).
const CommandRegistry& builtinCommands();

/// Adds the built-in catalog to a registry.
void registerBuiltinCommands(CommandRegistry& registry);

/// "timeline.rippleDelete" -> "timeline_rippleDelete" (catalog names never contain '_').
std::string toolNameFor(std::string_view commandName);

/// `spec` marked as running an AI model (CommandSpec::runsModel), for registrations:
/// `add(registry, command::runsModel({"captions.translate", 1, ...}))`.
inline CommandSpec runsModel(CommandSpec spec) {
    spec.runsModel = true;
    return spec;
}

/// `spec` marked as touching the files its parameters name (CommandSpec::touchesFiles); `fileParams` are those
/// parameters (CommandSpec::fileParams).
inline CommandSpec touchesFiles(CommandSpec spec, std::vector<std::string> fileParams = {}) {
    spec.touchesFiles = true;
    spec.fileParams = std::move(fileParams);
    return spec;
}

} // namespace wabcut::command
