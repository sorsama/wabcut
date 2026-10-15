#pragma once

// Constrained tool calls (AI-15, docs/ai.md "Local assistant tiers"): small local models often emit tool calls that do
// not parse, name tools that do not exist or put arguments in the wrong shape. With ModelCapabilities::constrainedTools
// the assistant does not send the native `tools` field; it asks for ONE JSON object per step,
//
//     {"name": "<tool>", "parameters": {...}}     a tool call (the tool's own parameter schema)
//     {"answer": "<text>"}                         done, or the answer to a question
//
// as a JSON-schema response format, which llama-server compiles into a grammar: every token the model samples keeps
// the reply valid against the envelope, so the call parses, the tool exists and the arguments have the declared shape.
// The tools are listed in the system prompt as compact signatures (far fewer tokens than the JSON schemas a chat
// template would inline), and earlier calls and results become plain turns of the same format.
//
// Keys are chosen so their alphabetical order (nlohmann::json sorts them; llama.cpp keeps the order it is given) is
// the order a model should write them in: "name" before "parameters".
//
// Replies from a server that does not enforce the format are parsed tolerantly (code fences, text around the object,
// "tool"/"arguments" or OpenAI's {"function": {...}} spelling, arguments as a JSON string, a dotted command name); a
// reply that still is not a call or an answer is asked for once more, then ends the turn as text.

#include <wabcut/ai/Capability.h>
#include <wabcut/ai/Chat.h>

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::agent {

/// The response-format schema for `request`'s tools and tool choice: anyOf one object per tool and (unless the
/// choice is Required or Named) the answer object; for ToolChoiceMode::None only the answer.
Json toolEnvelopeSchema(const std::vector<ToolSpec>& tools, const ToolChoice& choice = {});

/// One line per tool for the system prompt: "marker_add(time*: int|string, name: string, type: comment|chapter|...)
/// - Adds a sequence marker." Parameter descriptions are cut to their first sentence; `sequence` is left out (the
/// assistant fills it in).
std::string toolSignature(const ToolSpec& tool);

struct ParsedReply {
    std::optional<ToolCall> call; ///< a tool call (its id is empty: the caller numbers calls)
    std::optional<std::string> answer;
    bool repaired = false;        ///< parsed only with a tolerance (fences, other key names, a string of arguments, ...)
    std::string error;            ///< neither a call nor an answer: why
};

/// Parses a reply in the envelope format. Text that is not JSON at all (no '{') is an answer.
ParsedReply parseEnvelopeReply(std::string_view text, const std::vector<ToolSpec>& tools);

struct ConstrainedStats {
    int replies = 0;   ///< model replies (retries included)
    int calls = 0;     ///< tool calls returned to the assistant
    int answers = 0;
    int repaired = 0;  ///< replies parsed only with a tolerance
    int malformed = 0; ///< replies that were neither a call nor an answer (each one is asked for again once)
};

/// Wraps a chat model so tool calls go through the envelope. Requests without tools, or with a response format of
/// their own, pass through unchanged; so do native tool calls a server returns anyway.
class ConstrainedToolChat final : public IChatModel {
public:
    explicit ConstrainedToolChat(IChatModel& inner);

    const ModelSpec& spec() const override { return m_inner.spec(); }
    std::expected<ChatResponse, AiError> chat(const ChatRequest& request, const ChatEventHandler& onEvent,
                                              std::stop_token stop) override;

    /// The request as the inner model gets it (the agent estimates tokens on this).
    ChatRequest prepare(const ChatRequest& request) const;

    ConstrainedStats stats() const;

private:
    IChatModel& m_inner;
    mutable std::mutex m_mutex;
    ConstrainedStats m_stats;
    int m_nextId = 0;
};

} // namespace wabcut::ai::agent
