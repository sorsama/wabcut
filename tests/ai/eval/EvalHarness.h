#pragma once

// The editing-assistant eval (AI-15, docs/ai.md "Local assistant tiers"): realistic editing requests over fixture
// projects, each with a deterministic success check on the staged result and a reference solution (the tool calls an
// editor would make). runEval() runs every case through the real EditAgent - routing, constrained tool calls, staging,
// verification - on any IChatModel and measures what the docs report: success rate, valid tool-call rate, latency per
// step and decode speed. Fixtures and checks are pure (no media, no network); the model is the only variable.
//
//   OracleModel    plays a case's reference solution (in the constrained envelope or as native tool calls, cleanly or
//                  with near-miss JSON): proves every case is solvable and every check sound, deterministically.
//   TimedChat      wraps a real model and times each call (time to first token, decode time).

#include "TimelineFixture.h"

#include <wabcut/ai/Capability.h>
#include <wabcut/ai/agent/EditAgent.h>

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::eval {

struct Fixture {
    std::string name;
    ProjectState state;
    SequenceId seq;
    std::map<char, MediaId> media; ///< media letters kept in the fixture
    std::map<std::string, BinId> bins;
};

/// "interview" (V1/A1: clips A 0-5 s, a 1 s gap, B, C; A2: music M, sync lock off; markers Hook and Outro),
/// "captions" (interview plus an English caption track of four cues), "multicam" (one multicam clip of three angles
/// D, E, F over 40 s on V1/A1), "library" (interview plus the bins Footage and Music).
Fixture makeFixture(std::string_view name);

struct ToolUse {
    std::string tool; ///< tool name, e.g. "marker_add"
    Json arguments = Json::object();
};

/// Failure description, or nullopt when the staged result does what the request asked.
using Check = std::function<std::optional<std::string>(const Fixture& base, const ProjectState& staged, const agent::AgentOutcome& outcome)>;

struct EvalCase {
    std::string id;       ///< "markers-01"
    std::string category; ///< trim, ripple, markers, captions, audio, colour, multicam, library, question
    std::string fixture;
    std::string prompt;
    Check check;
    std::vector<ToolUse> reference; ///< what the oracle calls
    std::string referenceAnswer = "Done.";
    bool question = false;          ///< answered without editing (status NoChanges)
};

/// The eval set: 56 cases.
const std::vector<EvalCase>& evalCases();

// --- Models --------------------------------------------------------------------------------------------------

enum class OracleStyle {
    Clean,       ///< exactly the envelope
    NearMiss,    ///< cycles through code fences, "tool"/"arguments" keys, arguments as a string, text around the JSON
    Lazy,        ///< answers at once without editing (every edit case must fail)
};

/// Plays one case's reference solution. In constrained mode it reads the offered tools from the envelope schema and,
/// like a grammar-constrained model, cannot call a tool that was not offered (it gives up instead: a routing miss).
class OracleModel final : public IChatModel {
public:
    OracleModel(const ModelSpec& spec, const EvalCase& c, const Fixture& fixture, OracleStyle style = OracleStyle::Clean);
    const ModelSpec& spec() const override { return m_spec; }
    std::expected<ChatResponse, AiError> chat(const ChatRequest& request, const ChatEventHandler& onEvent,
                                              std::stop_token stop) override;
    bool routingMiss() const { return m_routingMiss; }

private:
    ModelSpec m_spec;
    const EvalCase& m_case;
    const Fixture& m_fixture;
    OracleStyle m_style;
    bool m_routingMiss = false;
    int m_variant = 0;
};

struct CallTiming {
    double seconds = 0;     ///< the whole call
    double firstToken = 0;  ///< until the first streamed token (prompt processing)
    std::int64_t outputTokens = 0;
    std::int64_t inputTokens = 0;
};

/// Times each call of the wrapped model.
class TimedChat final : public IChatModel {
public:
    explicit TimedChat(IChatModel& inner) : m_inner(inner) {}
    const ModelSpec& spec() const override { return m_inner.spec(); }
    std::expected<ChatResponse, AiError> chat(const ChatRequest& request, const ChatEventHandler& onEvent,
                                              std::stop_token stop) override;
    std::vector<CallTiming> take();

private:
    IChatModel& m_inner;
    std::mutex m_mutex;
    std::vector<CallTiming> m_calls;
};

// --- Runs ----------------------------------------------------------------------------------------------------

struct CaseResult {
    std::string id;
    std::string category;
    bool success = false;
    std::string failure;              ///< the check's finding, or the run's status / error
    std::string status;               ///< AgentStatus
    int modelCalls = 0;
    int toolCalls = 0;
    int invalidToolCalls = 0;
    int malformedReplies = 0;
    bool routedReference = true;      ///< every reference tool was offered
    double seconds = 0;
    std::vector<CallTiming> calls;
    std::vector<std::string> tools;   ///< the calls the model made, in order
};

struct EvalSummary {
    std::string model;
    std::string mode;                 ///< "constrained" or "native"
    std::vector<CaseResult> cases;
    int passed() const;
    double successRate() const;
    /// Tool calls that named an offered tool with arguments matching its schema, over all tool calls; replies that
    /// were neither a call nor an answer count as invalid calls.
    double validToolCallRate() const;
    double routingRecall() const;     ///< cases whose reference tools were all offered
    double medianStepSeconds() const;
    double p90StepSeconds() const;
    double medianFirstTokenSeconds() const;
    double decodeTokensPerSecond() const; ///< output tokens over the time after the first token
    std::map<std::string, std::pair<int, int>> byCategory() const; ///< passed, total
    Json toJson() const;
};

struct RunOptions {
    std::optional<bool> constrained;  ///< nullopt: the model's capability
    std::vector<std::string> only;    ///< case ids or categories; empty = all
    int maxSteps = 10;
    std::chrono::milliseconds timeout = std::chrono::minutes(3);
    std::function<void(const CaseResult&)> progress;
};

/// Runs the cases on `model` (a factory per case, so a fake can be made for each), checking each staged result.
EvalSummary runEval(const std::function<IChatModel&(const EvalCase&, const Fixture&)>& model, const RunOptions& options);

} // namespace wabcut::ai::eval
