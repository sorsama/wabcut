#pragma once

// The AI editing assistant (docs/ai.md, "Editing assistant"): a tool-use loop over IChatModel that edits a
// STAGING copy of the project and never the live session.
//
//   run()          the model reads the project with the catalog's read commands and edits with its mutating
//                  commands (filtered by the permission setting). Every mutating call runs on a staging
//                  EditSession branched from the live snapshot, and the calls that changed something are recorded
//                  as one CommandBatch. When the model says it is done, verify() checks the staged result; failures
//                  go back to the model once, then the outcome reports what the checks found, not what the model
//                  claimed. Limits: steps, tool calls, tokens, cost, a timeout and cancellation.
//   applyStaged()  on Accept (GUI thread): the batch runs on the live session as ONE undo step, guarded by the
//                  revision the agent started from. If the user edited meanwhile, the batch is dry-run on the
//                  current state and applied only when it still makes the same clip changes and passes the
//                  checks; otherwise it is refused.
//
// Pure C++ (no Qt): the app's AgentController runs run() on a worker thread and applyStaged() on the GUI thread.

#include <wabcut/ai/Capability.h>
#include <wabcut/ai/Chat.h>
#include <wabcut/ai/ModelRegistry.h>
#include <wabcut/ai/agent/Verification.h>
#include <wabcut/command/Command.h>
#include <wabcut/command/Registry.h>
#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/ProjectState.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace wabcut::command {
class CommandServices;
}

namespace wabcut::ai::agent {

struct AgentLimits {
    int maxSteps = 16;              ///< model calls per run
    int maxToolCalls = 64;          ///< tool calls per run
    std::int64_t maxTokens = 600'000; ///< input + output tokens per run (0 = no limit)
    MicroUsd maxCost = 1'000'000;   ///< per run: US$1.00 (0 = no limit); stops before a call expected to exceed it
    std::chrono::milliseconds timeout = std::chrono::minutes(5);
    int maxOutputTokens = 4096;     ///< per model call
    int verificationRetries = 1;    ///< failed checks are fed back this many times
    std::size_t maxToolResultChars = 24'000; ///< longer read results are cut (the model is told to narrow them)
};

/// Project-level commands an agent may run without the EditProject capability, because they only ADD sequences (AI-9's
/// reframe and shorts, AI-11's rough cut from a script, AI-13's podcast workflow); verification checks that nothing
/// else changed.
std::vector<std::string> defaultAdditiveProjectCommands();

/// C.11: library organisation the assistant may do without EditProject (low risk, never deleting): bin.create, bin.rename,
/// bin.move, media.setAttributes, media.makeSubclip, media.setSubclip - not bin.remove. Part of
/// defaultAdditiveProjectCommands(); verification still refuses removed media and changed sequence content.
std::vector<std::string> defaultLibraryCommands();

/// A.3: MediaAnalysis commands an agent may run without that capability, because they only read: they render the staged
/// project's audio to measure it (audio.loudness), so "hit -14 LUFS" can be checked before the edit is offered; AI-13's
/// analysis.speakers reads who speaks when in a multicam source; A.9's audio.channelLevels finds the channels with sound.
std::vector<std::string> defaultAnalysisCommands();

struct AgentOptions {
    /// What the agent may do: Read + EditSequence by default (Preferences > AI can add EditProject).
    command::CapabilitySet allowed = command::CapabilitySet::of({command::Capability::Read, command::Capability::EditSequence});
    /// EditProject commands allowed anyway (default: reframe.createSequence, shorts.create, roughcut.fromScript,
    /// workflow.podcast); empty = none. Sequences they make in a request may then be edited by that request too.
    std::vector<std::string> additiveProjectCommands = defaultAdditiveProjectCommands();
    /// MediaAnalysis reads allowed anyway (default: audio.loudness, analysis.speakers, audio.channelLevels); empty = none.
    std::vector<std::string> analysisCommands = defaultAnalysisCommands();
    /// Sequences one request may add.
    std::size_t maxNewSequences = 10;
    /// The catalog (default: wabcut::ai::edit::editingCommands(), the built-in commands plus transcript commands).
    const command::CommandRegistry* registry = nullptr;
    /// Transcripts and audio energy for the transcript commands (TranscriptServices); null = none.
    std::shared_ptr<command::CommandServices> services;
    AgentLimits limits;
    /// Mark the stable prompt prefix (tools, system prompt) and the growing conversation for provider caching.
    bool promptCaching = true;
    /// Where the redacted transcript of each run is written ("agent-<time>.json"); empty = kept in memory only.
    std::filesystem::path transcriptDir;
    /// "Today" for prices (tests); default: the UTC date.
    std::function<std::chrono::year_month_day()> today;
    /// AI-15: tool calls through one JSON-schema constrained reply per step (ConstrainedTools.h). nullopt = when the
    /// model asks for it (ModelCapabilities::constrainedTools); true / false = always / never (the eval compares both).
    std::optional<bool> constrainedTools;
    /// AI-15: tools offered per request (ToolRouter.h). nullopt = for local models, what their context holds
    /// (toolBudgetFor(contextTokens)), all tools for the others; 0 = all tools.
    std::optional<std::size_t> toolBudget;
};

/// One thing the agent did, as the assistant panel lists it.
struct AgentStep {
    enum class Kind { Read, Edit, Error, Verify, Note };
    Kind kind = Kind::Note;
    std::string title;  ///< human-readable: "Remove 2 silences longer than 1 s", "Read the transcript (0:00–1:00)"
    std::string detail; ///< result or error ("3 clips changed on V1, A1"; "Clip 41 not found")
    std::string tool;   ///< command name ("transcript.cleanup"); empty for Verify / Note
    Json params;        ///< the call's params
    bool ok = true;

    bool operator==(const AgentStep&) const = default;
};

enum class AgentStatus {
    Staged,             ///< the model finished and every check passed
    NoChanges,          ///< the model finished without editing (an answer, or nothing to do)
    VerificationFailed, ///< the model finished but checks still fail after the retry
    StepLimit,          ///< stopped at maxSteps / maxToolCalls
    BudgetExceeded,     ///< token or cost budget, or the monthly spend cap
    TimedOut,
    Cancelled,
    Failed,             ///< the model call failed (network, key, refusal, ...)
};

std::string_view toString(AgentStatus status);

/// What the panel streams while the agent runs.
struct AgentEvent {
    enum class Kind {
        Phase,         ///< text: "Thinking", "Reading the transcript", "Checking the result"
        Text,          ///< text: a delta of the model's visible answer
        Reasoning,     ///< text: a delta of the model's reasoning summary (where the provider shows one)
        StepStarted,   ///< step: a tool call is about to run (title only)
        StepFinished,  ///< step: its result
        Usage,         ///< usage and cost so far
    };
    Kind kind = Kind::Phase;
    std::string text;
    AgentStep step;
    Usage usage;
    MicroUsd cost = 0;
};
using AgentEventHandler = std::function<void(const AgentEvent&)>;

struct AgentOutcome;

struct AgentRequest {
    std::string prompt;            ///< the user's words (or a quick action's prompt)
    SequenceId sequence;
    /// The live state to branch from: EditSession::stateForSave() (it carries the id high-water mark, so the
    /// batch creates the same ids when it is applied) and the revision it was taken at.
    std::shared_ptr<const ProjectState> base;
    std::uint64_t baseRevision = 0;
    /// Checked after the model is done; merged with parseConstraints(prompt) by the caller (QuickActions do).
    Constraints constraints;
    /// Refine: continue this earlier outcome (same base, its staged state and conversation, the batch grows).
    std::shared_ptr<const AgentOutcome> previous;
};

/// A command the model asked for that reads or writes files (CommandSpec::touchesFiles: cdl.import, lut.import, review
/// packages...). It never runs in the staged preview, nor on Accept (S5-P1-1: a model must not pick paths); the panel
/// shows it as "needs your confirmation" and the user runs it, seeing the path, through the app.
struct AgentConfirmation {
    std::string command; ///< "cdl.import"
    Json params;
    std::string title;   ///< as the step showed it ("Import CDL (clip 30, path C:/grades/a.cc)")

    bool operator==(const AgentConfirmation&) const = default;
};

struct AgentOutcome {
    AgentStatus status = AgentStatus::Failed;
    std::string answer;   ///< the model's final text
    std::string error;    ///< for Failed / BudgetExceeded / TimedOut / ...: what happened
    VerificationReport verification;
    bool verified = false; ///< the checks ran on the staged result (false when the run stopped before)
    std::vector<AgentStep> steps;

    SequenceId sequence;
    std::shared_ptr<const ProjectState> base;   ///< what the run branched from
    std::uint64_t baseRevision = 0;
    std::shared_ptr<const ProjectState> staged; ///< the staged result (stateForSave of the staging session)
    command::CommandBatch batch;                ///< the mutating commands that changed something, in order
    std::vector<AgentConfirmation> needsConfirmation; ///< file commands put aside for the user (never in `batch`)
    ChangeSet changes;                          ///< diff(base, staged)
    Constraints constraints;

    Usage usage;
    MicroUsd cost = 0;
    int modelCalls = 0;
    int toolCalls = 0;
    /// AI-15: calls of a tool that does not exist, or with arguments that are not an object or do not match the tool's
    /// schema (each one goes back to the model as an error).
    int invalidToolCalls = 0;
    /// AI-15, constrained tool calls: replies that were neither a call nor an answer, and replies that parsed only with
    /// a tolerance (ConstrainedTools.h).
    int malformedReplies = 0;
    int repairedReplies = 0;
    std::vector<std::string> offeredTools;      ///< the tools the model was offered (routed on small contexts)
    std::string model;                          ///< ModelSpec::id

    std::string systemPrompt;
    std::vector<ChatMessage> conversation;      ///< for Refine
    Json transcript;                            ///< redacted record of the run (also written to transcriptDir)
    std::filesystem::path transcriptFile;

    bool hasChanges() const { return !changes.empty() && !batch.commands.empty(); }
    /// Accept is offered when something was staged and the run was not cancelled or failed; the report
    /// shows whether the checks passed.
    bool canApply() const;
};

class EditAgent {
public:
    explicit EditAgent(AgentOptions options = {});

    /// Runs one agent turn on the caller's thread (a worker). `model` is a guarded IChatModel from AiPlatform
    /// (consent, spend cap and request log apply to each call). `stop` cancels; the run also stops at the timeout.
    /// Never throws: anything unexpected (arguments that break a description, a bug) ends the run as Failed with the
    /// reason, and the live project is never touched (the run edits a staging branch).
    AgentOutcome run(IChatModel& model, const AgentRequest& request, const AgentEventHandler& onEvent,
                     std::stop_token stop);

    const AgentOptions& options() const { return m_options; }
    const command::CommandRegistry& registry() const { return *m_options.registry; }

    /// The tool specs the model gets (catalog filtered by the allowed capabilities). Stable order.
    std::vector<ToolSpec> tools() const;
    /// The system prompt for `sequence` at `revision`: fixed rules plus a compact project summary, computed once
    /// per (revision, sequence) so every call of a run, and of later runs on the same revision, sends the same
    /// prefix (provider prompt caches hit).
    std::string systemPrompt(const ProjectState& state, std::uint64_t revision, SequenceId sequence) const;

private:
    AgentOutcome runUnguarded(IChatModel& model, const AgentRequest& request, const AgentEventHandler& onEvent,
                              std::stop_token stop);

    AgentOptions m_options;
    mutable std::mutex m_mutex;
    mutable std::map<std::pair<std::uint64_t, std::uint64_t>, std::string> m_summaries; // (revision, sequence)
    mutable std::vector<ToolSpec> m_tools;
};

/// A Failed outcome for `request` (its base, sequence and constraints) with `error`: what a run that stopped on an
/// exception reports.
AgentOutcome failedOutcome(const AgentRequest& request, std::string modelId, std::string error);

/// A compact text summary of the project and one sequence for the system prompt: sequence settings, tracks with
/// clip counts and extents, the first clips of each track, markers, caption tracks and media names.
std::string projectSummary(const ProjectState& state, SequenceId sequence, std::size_t maxClipsPerTrack = 12);

struct ApplyOptions {
    const command::CommandRegistry* registry = nullptr; ///< default: editingCommands()
    command::CapabilitySet allowed = command::CapabilitySet::of({command::Capability::Read, command::Capability::EditSequence});
    /// As AgentOptions::additiveProjectCommands: a batch may hold these EditProject commands, no others.
    std::vector<std::string> additiveProjectCommands = defaultAdditiveProjectCommands();
    std::shared_ptr<command::CommandServices> services;
};

struct ApplyResult {
    enum class Status {
        Applied, ///< committed as one undo step at the revision the agent started from
        Rebased, ///< the session had moved on; the dry run on the current state made the same changes, committed
        Refused, ///< stale and the batch no longer applies the same way (or no longer passes the checks)
        Failed,  ///< the commit itself failed (nothing changed)
    };
    Status status = Status::Failed;
    std::string message;
    std::uint64_t revision = 0;       ///< the session's revision afterwards
    VerificationReport verification;  ///< the re-run checks (Rebased / Refused)
};

/// Applies an outcome's batch to the live session as one undo step labelled `label` (default "AI: <prompt>").
/// GUI thread (it mutates `live`).
ApplyResult applyStaged(EditSession& live, const AgentOutcome& outcome, const ApplyOptions& options = {},
                        std::string label = {});

/// "AI: Remove silences longer than 1 s" (at most ~48 characters of the prompt).
std::string undoLabelFor(std::string_view prompt);

/// The redacted JSON form of a conversation (images and videos replaced by their size).
Json conversationToJson(const std::vector<ChatMessage>& messages);

} // namespace wabcut::ai::agent
