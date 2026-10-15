// OpenAI-compatible Chat Completions (developers.openai.com .../chat/completions; docs.ollama.com/api/openai-compatibility;
// lmstudio.ai/docs/developer/openai-compat; llama.cpp tools/server README; openrouter.ai/docs; console.groq.com/docs;
// docs.mistral.ai). Verified 2026-10-11: "data:" chunks ending with [DONE]; tool-call deltas keyed by index; a final
// usage chunk with choices: [] when stream_options.include_usage is set; OpenRouter reports mid-stream errors as a
// top-level "error" with HTTP 200. Optional parameters are sent only when needed, since servers differ in what they
// accept (Ollama has no tool_choice).

#include "AdapterUtil.h"

#include <wabcut/ai/JsonAccess.h>

#include <wabcut/ai/ToolSchema.h>
#include <wabcut/ai_net/Adapters.h>

#include <map>

namespace wabcut::ai::net {

namespace {

StopReason stopReasonFrom(std::string_view reason) {
    if (reason == "stop") {
        return StopReason::EndTurn;
    }
    if (reason == "length") {
        return StopReason::MaxTokens;
    }
    if (reason == "tool_calls" || reason == "function_call") {
        return StopReason::ToolUse;
    }
    if (reason == "content_filter") {
        return StopReason::ContentFilter;
    }
    return StopReason::Other;
}

class ChatCompletionsChat final : public IChatModel {
public:
    explicit ChatCompletionsChat(ProviderContext context)
        : m_provider(context.provider), m_spec(context.model), m_baseUrl(std::move(context.baseUrl)),
          m_key(std::move(context.key)), m_transport(context.transport) {}

    const ModelSpec& spec() const override { return m_spec; }

    std::expected<ChatResponse, AiError> chat(const ChatRequest& request, const ChatEventHandler& onEvent,
                                              std::stop_token stop) override {
        if (auto valid = validateChatRequest(request); !valid) {
            return std::unexpected(valid.error());
        }
        if (auto video = rejectVideo(request, "Chat Completions"); !video) {
            return std::unexpected(video.error());
        }
        const ProviderContext context{m_provider, m_spec, m_baseUrl, m_key.clone(), m_transport};
        const std::string url = joinUrl(m_baseUrl, m_spec.endpoint.empty() ? "/chat/completions" : m_spec.endpoint);
        HttpRequest http = jsonRequest(context, url, buildBody(request), BodyMode::Sse);
        if (!m_provider.local) {
            http.idleTimeout = std::chrono::seconds(120);
        } else {
            http.idleTimeout = std::chrono::minutes(10); // a local model may take long to load
        }

        ChatAccumulator acc(onEvent);
        std::optional<std::size_t> textPart;
        std::optional<std::size_t> reasoningPart;
        std::map<std::int64_t, std::size_t> calls; // tool_calls[].index -> part index
        std::optional<AiError> streamFailure;
        bool done = false;
        bool sawFinish = false;

        HttpStreamHandlers handlers;
        handlers.onEvent = [&](const SseEvent& event) {
            if (event.data == "[DONE]") {
                done = true;
                return true;
            }
            return guardEvent(acc, [&] {
                const auto chunk = Json::parse(event.data, nullptr, false);
                if (chunk.is_discarded() || !chunk.is_object()) {
                    return;
                }
                if (const Json* e = jsonObject(chunk, "error")) {
                    streamFailure = streamError(jsonIdOr(*e, "code"), jsonStringOr(*e, "message"));
                    return;
                }
                if (acc.response().id.empty()) {
                    acc.response().id = jsonStringOr(chunk, "id");
                    acc.response().model = jsonStringOr(chunk, "model");
                }
                if (const Json* u = jsonObject(chunk, "usage")) {
                    Usage usage;
                    usage.inputTokens = intAt(*u, {"prompt_tokens"});
                    usage.cacheReadTokens = intAt(*u, {"prompt_tokens_details", "cached_tokens"});
                    usage.outputTokens = intAt(*u, {"completion_tokens"});
                    usage.reasoningTokens = intAt(*u, {"completion_tokens_details", "reasoning_tokens"});
                    acc.updateUsage(usage);
                }
                const Json* choices = jsonArray(chunk, "choices");
                if (!choices || choices->empty() || !(*choices)[0].is_object()) {
                    return;
                }
                const Json& choice = (*choices)[0];
                const Json noDelta = Json::object();
                const Json* deltaPtr = jsonObject(choice, "delta");
                const Json& delta = deltaPtr ? *deltaPtr : noDelta;
                for (const char* key : {"reasoning_content", "reasoning"}) { // llama-server / Ollama / OpenRouter thinking
                    if (const auto text = jsonString(delta, key); text && !text->empty()) {
                        if (!reasoningPart) {
                            reasoningPart = acc.startReasoning(ReasoningPart{"chat-completions", "", "", "", false});
                        }
                        acc.appendReasoning(*reasoningPart, *text);
                        break;
                    }
                }
                for (const char* key : {"content", "refusal"}) {
                    if (const auto text = jsonString(delta, key); text && !text->empty()) {
                        if (!textPart) {
                            textPart = acc.startText();
                        }
                        acc.appendText(*textPart, *text);
                    }
                }
                if (const Json* toolCalls = jsonArray(delta, "tool_calls")) {
                    for (const auto& tc : *toolCalls) {
                        if (!tc.is_object()) {
                            acc.fail(malformedEvent("a tool call that is not an object"));
                            return;
                        }
                        const std::int64_t index = jsonInt(tc, "index").value_or(static_cast<std::int64_t>(calls.size()));
                        auto it = calls.find(index);
                        if (it == calls.end()) {
                            std::string id = jsonStringOr(tc, "id");
                            if (id.empty()) {
                                id = "call_" + std::to_string(index); // Ollama sends no ids
                            }
                            const std::size_t part = acc.startToolCall(std::move(id), stringAt(tc, {"function", "name"}));
                            if (part == ChatAccumulator::kNoPart) {
                                return;
                            }
                            it = calls.emplace(index, part).first;
                        }
                        if (const Json* fn = jsonObject(tc, "function")) {
                            if (const Json* args = jsonMember(*fn, "arguments")) {
                                // Strings are fragments; some local servers send the whole object at once.
                                acc.appendArguments(it->second, args->is_string() ? args->get_ref<const std::string&>()
                                                                                  : args->dump());
                            }
                        }
                    }
                }
                if (const auto reason = jsonString(choice, "finish_reason")) {
                    if (*reason == "error") {
                        streamFailure = streamError("error", "the provider ended the stream with an error");
                    } else {
                        sawFinish = true;
                        acc.response().rawStopReason = *reason;
                        acc.response().stopReason = stopReasonFrom(*reason);
                    }
                }
            });
        };

        auto response = m_transport.send(http, handlers, stop);
        if (acc.error()) {
            return std::unexpected(*acc.error()); // a malformed event or a response past its bounds
        }
        if (streamFailure) {
            return std::unexpected(*streamFailure);
        }
        if (!response) {
            return std::unexpected(response.error());
        }
        if (!response->ok()) {
            return std::unexpected(httpError(*response));
        }
        if (!done && !sawFinish) {
            return std::unexpected(AiError(AiErrorCode::Network, "the stream ended before [DONE]"));
        }
        if (acc.hasToolCalls() && acc.response().stopReason == StopReason::EndTurn) {
            acc.response().stopReason = StopReason::ToolUse; // some servers report "stop" with tool calls
        }
        return acc.take();
    }

private:
    Json buildBody(const ChatRequest& request) const {
        Json messages = Json::array();
        if (!request.system.empty()) {
            messages.push_back({{"role", "system"}, {"content", request.system}});
        }
        for (const auto& message : request.messages) {
            if (message.role == Role::User) {
                Json content = Json::array();
                bool onlyText = true;
                for (const auto& part : message.parts) {
                    if (const auto* result = std::get_if<ToolResult>(&part)) {
                        messages.push_back({{"role", "tool"}, {"tool_call_id", result->callId}, {"content", result->content}});
                    } else if (const auto* t = std::get_if<TextPart>(&part)) {
                        content.push_back({{"type", "text"}, {"text", t->text}});
                    } else if (const auto* image = std::get_if<ImagePart>(&part)) {
                        onlyText = false;
                        content.push_back(
                            {{"type", "image_url"}, {"image_url", {{"url", image->url.empty() ? dataUrl(*image) : image->url}}}});
                    }
                }
                if (content.empty()) {
                    continue;
                }
                if (onlyText) {
                    std::string text;
                    for (const auto& c : content) {
                        text += c["text"].get<std::string>();
                    }
                    messages.push_back({{"role", "user"}, {"content", text}});
                } else {
                    messages.push_back({{"role", "user"}, {"content", std::move(content)}});
                }
            } else {
                Json assistant = {{"role", "assistant"}};
                std::string text;
                Json toolCalls = Json::array();
                for (const auto& part : message.parts) {
                    if (const auto* t = std::get_if<TextPart>(&part)) {
                        text += t->text;
                    } else if (const auto* call = std::get_if<ToolCall>(&part)) {
                        toolCalls.push_back({{"id", call->id},
                                             {"type", "function"},
                                             {"function",
                                              {{"name", call->name},
                                               {"arguments", call->argumentsText.empty() ? call->arguments.dump()
                                                                                         : call->argumentsText}}}});
                    }
                }
                assistant["content"] = text.empty() && !toolCalls.empty() ? Json(nullptr) : Json(text);
                if (!toolCalls.empty()) {
                    assistant["tool_calls"] = std::move(toolCalls);
                }
                messages.push_back(std::move(assistant));
            }
        }

        Json body = {{"model", m_spec.id},
                     {"messages", std::move(messages)},
                     {"stream", true},
                     {"stream_options", {{"include_usage", true}}},
                     {"max_tokens", request.maxOutputTokens}};
        const bool strict = m_spec.caps.strictSchema;
        if (!request.tools.empty()) {
            Json tools = Json::array();
            for (const auto& tool : request.tools) {
                const bool toolStrict = tool.strict && strict;
                Json fn = {{"name", tool.name},
                           {"description", tool.description},
                           {"parameters", adaptSchema(tool.parameters,
                                                      toolStrict ? SchemaDialect::OpenAiStrict : SchemaDialect::OpenAiCompatible,
                                                      toolStrict)}};
                if (toolStrict) {
                    fn["strict"] = true;
                }
                tools.push_back({{"type", "function"}, {"function", std::move(fn)}});
            }
            body["tools"] = std::move(tools);
            // Only forced choices are sent: "auto" is the default everywhere and some servers reject the field.
            if (m_spec.caps.forcedToolChoice) {
                if (request.toolChoice.mode == ToolChoiceMode::Required) {
                    body["tool_choice"] = "required";
                } else if (request.toolChoice.mode == ToolChoiceMode::Named) {
                    body["tool_choice"] = {{"type", "function"}, {"function", {{"name", request.toolChoice.name}}}};
                }
            }
            if (request.toolChoice.mode == ToolChoiceMode::None) {
                body["tool_choice"] = "none";
            }
            if (!request.toolChoice.allowParallel) {
                body["parallel_tool_calls"] = false;
            }
        }
        if (request.responseFormat) {
            const bool formatStrict = request.responseFormat->strict && strict;
            body["response_format"] = {
                {"type", "json_schema"},
                {"json_schema",
                 {{"name", request.responseFormat->name},
                  {"schema", adaptSchema(request.responseFormat->schema,
                                         formatStrict ? SchemaDialect::OpenAiStrict : SchemaDialect::OpenAiCompatible, formatStrict)},
                  {"strict", formatStrict}}}};
        }
        if (request.temperature && m_spec.caps.temperature) {
            body["temperature"] = *request.temperature;
        }
        if (!request.stopSequences.empty()) {
            body["stop"] = request.stopSequences;
        }
        return body;
    }

    ProviderSpec m_provider;
    ModelSpec m_spec;
    std::string m_baseUrl;
    Secret m_key;
    IHttpTransport& m_transport;
};

} // namespace

std::unique_ptr<IChatModel> makeChatCompletionsChat(ProviderContext context) {
    return std::make_unique<ChatCompletionsChat>(std::move(context));
}


namespace {
class ManagedChat final : public IChatModel {
public:
    ManagedChat(ProviderContext c, PrepareLocal prepare, std::function<void(bool)> busy)
        : m_provider(c.provider), m_model(c.model), m_transport(c.transport), m_prepare(std::move(prepare)), m_busy(std::move(busy)) {}
    const ModelSpec& spec() const override { return m_model; }
    std::expected<ChatResponse, AiError> chat(const ChatRequest& r, const ChatEventHandler& events, std::stop_token stop) override {
        // Preparing the runtime touches the file system and starts a process on the chat job thread, which does not
        // catch: anything thrown becomes this call's error (review wave 4, P2-6).
        std::expected<std::pair<std::string, Secret>, AiError> endpoint = std::unexpected(AiError());
        try {
            endpoint = m_prepare(stop);
        } catch (const std::exception& e) {
            return std::unexpected(AiError(AiErrorCode::ProviderError, std::string("the local assistant could not start: ") + e.what()));
        } catch (...) {
            return std::unexpected(AiError(AiErrorCode::ProviderError, "the local assistant could not start"));
        }
        if (!endpoint) return std::unexpected(endpoint.error());
        m_busy(true);
        struct Finish { std::function<void(bool)>& callback; ~Finish() { callback(false); } } finish{m_busy};
        auto adapter = makeChatCompletionsChat({m_provider, m_model, endpoint->first, std::move(endpoint->second), m_transport});
        return adapter->chat(r, events, stop);
    }
private:
    ProviderSpec m_provider; ModelSpec m_model; IHttpTransport& m_transport;
    PrepareLocal m_prepare; std::function<void(bool)> m_busy;
};
}
std::unique_ptr<IChatModel> makeManagedChat(ProviderContext c, PrepareLocal prepare, std::function<void(bool)> busy) {
    return std::make_unique<ManagedChat>(std::move(c), std::move(prepare), std::move(busy));
}

} // namespace wabcut::ai::net
