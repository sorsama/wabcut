#pragma once

// A scripted IChatModel for the editing-assistant tests (owner: AI platform engineer): each chat() call records the
// request and answers with the next scripted turn, so a test reads like the conversation it checks. No network.

#include <wabcut/ai/Capability.h>
#include <wabcut/ai/Chat.h>
#include <wabcut/ai/ModelRegistry.h>

#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace wabcut::ai::test {

/// A model call: one tool, its arguments.
struct Call {
    std::string tool;
    Json arguments;
};

class ScriptedModel final : public IChatModel {
public:
    using Turn = std::function<std::expected<ChatResponse, AiError>(const ChatRequest&, const ChatEventHandler&, std::stop_token)>;

    /// The built-in registry's entry (priced) for `modelId`.
    explicit ScriptedModel(const std::string& modelId = "claude-sonnet-5-5") {
        const ModelSpec* model = ModelRegistry::builtin().model(modelId);
        REQUIRE(model != nullptr);
        m_spec = *model;
    }

    const ModelSpec& spec() const override { return m_spec; }

    /// A turn that calls tools (in parallel, as one assistant message).
    ScriptedModel& tools(std::vector<Call> calls, Usage usage = defaultUsage()) {
        ChatResponse r;
        r.message.role = Role::Assistant;
        for (Call& c : calls) {
            ToolCall call;
            call.id = "call_" + std::to_string(++m_callId);
            call.name = c.tool;
            call.arguments = c.arguments;
            call.argumentsText = c.arguments.dump();
            r.message.parts.push_back(std::move(call));
        }
        r.stopReason = StopReason::ToolUse;
        r.usage = usage;
        return turn(std::move(r));
    }
    ScriptedModel& tool(std::string name, Json arguments = Json::object(), Usage usage = defaultUsage()) {
        return tools({Call{std::move(name), std::move(arguments)}}, usage);
    }
    /// A final answer (no tool calls): the model says it is done.
    ScriptedModel& text(std::string answer, Usage usage = defaultUsage()) {
        ChatResponse r;
        r.message = ChatMessage::assistant(std::move(answer));
        r.stopReason = StopReason::EndTurn;
        r.usage = usage;
        return turn(std::move(r));
    }
    ScriptedModel& turn(ChatResponse response) {
        return custom([response](const ChatRequest&, const ChatEventHandler& onEvent, std::stop_token) {
            if (onEvent && !response.message.text().empty()) {
                onEvent(TextDelta{0, response.message.text()});
            }
            return std::expected<ChatResponse, AiError>(response);
        });
    }
    ScriptedModel& error(AiErrorCode code, std::string message = "scripted failure") {
        return custom([code, message](const ChatRequest&, const ChatEventHandler&, std::stop_token) {
            return std::expected<ChatResponse, AiError>(std::unexpected(AiError(code, message)));
        });
    }
    ScriptedModel& custom(Turn turn) {
        std::lock_guard lock(m_mutex);
        m_script.push_back(std::move(turn));
        return *this;
    }

    std::expected<ChatResponse, AiError> chat(const ChatRequest& request, const ChatEventHandler& onEvent,
                                              std::stop_token stop) override {
        Turn next;
        {
            std::lock_guard lock(m_mutex);
            m_requests.push_back(request);
            if (m_script.empty()) {
                return std::unexpected(AiError(AiErrorCode::ProviderError, "ScriptedModel: no scripted turn"));
            }
            next = std::move(m_script.front());
            m_script.pop_front();
        }
        if (stop.stop_requested()) {
            return std::unexpected(AiError(AiErrorCode::Cancelled, "cancelled"));
        }
        return next(request, onEvent, stop);
    }

    std::vector<ChatRequest> requests() const {
        std::lock_guard lock(m_mutex);
        return m_requests;
    }
    std::size_t remaining() const {
        std::lock_guard lock(m_mutex);
        return m_script.size();
    }

    static Usage defaultUsage() {
        Usage u;
        u.inputTokens = 2000;
        u.outputTokens = 100;
        return u;
    }

private:
    ModelSpec m_spec;
    mutable std::mutex m_mutex;
    std::deque<Turn> m_script;
    std::vector<ChatRequest> m_requests;
    int m_callId = 0;
};

/// A model that forwards to a shared ScriptedModel: hosts that create a model per run (AgentController's factory)
/// all play the same script.
class SharedModel final : public IChatModel {
public:
    explicit SharedModel(std::shared_ptr<ScriptedModel> model) : m_model(std::move(model)) {}
    const ModelSpec& spec() const override { return m_model->spec(); }
    std::expected<ChatResponse, AiError> chat(const ChatRequest& request, const ChatEventHandler& onEvent,
                                              std::stop_token stop) override {
        return m_model->chat(request, onEvent, stop);
    }

private:
    std::shared_ptr<ScriptedModel> m_model;
};

/// The tool results of the last user message of a request.
inline std::vector<ToolResult> toolResultsOf(const ChatRequest& request) {
    std::vector<ToolResult> out;
    if (request.messages.empty()) {
        return out;
    }
    for (const ContentPart& part : request.messages.back().parts) {
        if (const auto* r = std::get_if<ToolResult>(&part)) {
            out.push_back(*r);
        }
    }
    return out;
}

} // namespace wabcut::ai::test
