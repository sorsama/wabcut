// Anthropic Messages API (platform.claude.com/docs/en/api/messages/create, .../build-with-claude/streaming).
// Verified 2026-10-11: x-api-key + anthropic-version 2023-06-01 (from the registry's provider headers), strict tools
// without a beta header, output_config.format for structured output, forced tool_choice rejected by Opus/Sonnet 5.5
// and Fable 5.1 (ModelCapabilities::forcedToolChoice), temperature only 1.0 on current models.

#include "AdapterUtil.h"

#include <wabcut/ai/JsonAccess.h>

#include <wabcut/ai/ToolSchema.h>
#include <wabcut/ai_net/Adapters.h>

#include <map>

namespace wabcut::ai::net {

namespace {

constexpr std::string_view kProvider = "anthropic";

StopReason stopReasonFrom(std::string_view reason) {
    if (reason == "end_turn") {
        return StopReason::EndTurn;
    }
    if (reason == "tool_use") {
        return StopReason::ToolUse;
    }
    if (reason == "max_tokens" || reason == "model_context_window_exceeded") {
        return StopReason::MaxTokens;
    }
    if (reason == "stop_sequence") {
        return StopReason::StopSequence;
    }
    if (reason == "refusal") {
        return StopReason::Refusal;
    }
    return StopReason::Other;
}

class AnthropicChat final : public IChatModel {
public:
    explicit AnthropicChat(ProviderContext context)
        : m_provider(context.provider), m_spec(context.model), m_baseUrl(std::move(context.baseUrl)),
          m_key(std::move(context.key)), m_transport(context.transport) {}

    const ModelSpec& spec() const override { return m_spec; }

    std::expected<ChatResponse, AiError> chat(const ChatRequest& request, const ChatEventHandler& onEvent,
                                              std::stop_token stop) override {
        if (auto valid = validateChatRequest(request); !valid) {
            return std::unexpected(valid.error());
        }
        if (auto video = rejectVideo(request, "Anthropic Messages"); !video) {
            return std::unexpected(video.error());
        }
        const Json body = buildBody(request);
        const ProviderContext context{m_provider, m_spec, m_baseUrl, m_key.clone(), m_transport};
        HttpRequest http = jsonRequest(context, endpointUrl(context), body, BodyMode::Sse);

        ChatAccumulator acc(onEvent);
        std::map<std::int64_t, std::size_t> blocks; // content block index -> part index
        Usage usage;
        std::optional<AiError> streamFailure;
        bool stopped = false;

        HttpStreamHandlers handlers;
        handlers.onEvent = [&](const SseEvent& event) {
            return guardEvent(acc, [&] {
                const auto data = Json::parse(event.data, nullptr, false);
                if (data.is_discarded() || !data.is_object()) {
                    return; // keep-alive noise
                }
                const std::string type = jsonStringOr(data, "type", event.event);
                // The part of the content block named by "index"; none for a delta before its block started.
                auto partOf = [&]() -> std::optional<std::size_t> {
                    const auto index = jsonInt(data, "index");
                    const auto it = index ? blocks.find(*index) : blocks.end();
                    return it == blocks.end() ? std::nullopt : std::optional<std::size_t>(it->second);
                };
                if (type == "message_start") {
                    const Json* message = jsonObject(data, "message");
                    if (!message) {
                        acc.fail(malformedEvent("message_start without a message"));
                        return;
                    }
                    acc.response().id = jsonStringOr(*message, "id");
                    acc.response().model = jsonStringOr(*message, "model");
                    usage.cacheReadTokens = intAt(*message, {"usage", "cache_read_input_tokens"});
                    usage.cacheWriteTokens = intAt(*message, {"usage", "cache_creation_input_tokens"});
                    usage.inputTokens = intAt(*message, {"usage", "input_tokens"}) + usage.cacheReadTokens + usage.cacheWriteTokens;
                    usage.outputTokens = intAt(*message, {"usage", "output_tokens"});
                    acc.updateUsage(usage);
                } else if (type == "content_block_start") {
                    const auto index = jsonInt(data, "index");
                    const Json* block = jsonObject(data, "content_block");
                    if (!index || !block) {
                        acc.fail(malformedEvent("content_block_start without an index or a content block"));
                        return;
                    }
                    const std::string kind = jsonStringOr(*block, "type");
                    std::size_t part = ChatAccumulator::kNoPart;
                    if (kind == "text") {
                        part = acc.startText();
                        acc.appendText(part, jsonStringOr(*block, "text"));
                    } else if (kind == "tool_use") {
                        part = acc.startToolCall(jsonStringOr(*block, "id"), jsonStringOr(*block, "name"));
                        if (const Json* input = jsonObject(*block, "input"); input && !input->empty()) {
                            acc.setArguments(part, input->dump());
                        }
                    } else if (kind == "thinking") {
                        part = acc.startReasoning(ReasoningPart{std::string(kProvider), jsonStringOr(*block, "thinking"),
                                                                jsonStringOr(*block, "signature"), "", false});
                    } else if (kind == "redacted_thinking") {
                        part = acc.startReasoning(ReasoningPart{std::string(kProvider), "", jsonStringOr(*block, "data"), "", true});
                    }
                    if (part != ChatAccumulator::kNoPart) {
                        blocks[*index] = part;
                    }
                } else if (type == "content_block_delta") {
                    const auto part = partOf();
                    const Json* delta = jsonObject(data, "delta");
                    if (!part || !delta) {
                        return;
                    }
                    const std::string kind = jsonStringOr(*delta, "type");
                    if (kind == "text_delta") {
                        acc.appendText(*part, jsonStringOr(*delta, "text"));
                    } else if (kind == "input_json_delta") {
                        acc.appendArguments(*part, jsonStringOr(*delta, "partial_json"));
                    } else if (kind == "thinking_delta") {
                        acc.appendReasoning(*part, jsonStringOr(*delta, "thinking"));
                    } else if (kind == "signature_delta") {
                        acc.appendSignature(*part, jsonStringOr(*delta, "signature"));
                    }
                } else if (type == "content_block_stop") {
                    if (const auto part = partOf(); part && acc.toolCall(*part)) {
                        acc.finishToolCall(*part);
                    }
                } else if (type == "message_delta") {
                    const std::string reason = stringAt(data, {"delta", "stop_reason"});
                    if (!reason.empty()) {
                        acc.response().rawStopReason = reason;
                        acc.response().stopReason = stopReasonFrom(reason);
                    }
                    if (jsonObject(data, "usage")) {
                        usage.outputTokens = intAt(data, {"usage", "output_tokens"}); // cumulative
                        acc.updateUsage(usage);
                    }
                } else if (type == "message_stop") {
                    stopped = true;
                } else if (type == "error") {
                    streamFailure = streamError(stringAt(data, {"error", "type"}), stringAt(data, {"error", "message"}));
                }
                // ping and unknown events are ignored
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
        if (!stopped) {
            return std::unexpected(AiError(AiErrorCode::Network, "the stream ended before message_stop"));
        }
        return acc.take();
    }

private:
    Json buildBody(const ChatRequest& request) const {
        const bool strict = m_spec.caps.strictSchema;
        Json body = {{"model", m_spec.id}, {"max_tokens", request.maxOutputTokens}, {"stream", true}};
        const Json ephemeral = {{"type", "ephemeral"}};
        if (!request.system.empty()) {
            if (request.cachePrompt) {
                // Breakpoint after tools + system: the stable prefix of every call in a tool loop.
                body["system"] = Json::array({{{"type", "text"}, {"text", request.system}, {"cache_control", ephemeral}}});
            } else {
                body["system"] = request.system;
            }
        }
        Json messages = Json::array();
        for (const auto& message : request.messages) {
            Json content = Json::array();
            Json results = Json::array(); // tool_result blocks must come first in a user message
            for (const auto& part : message.parts) {
                if (const auto* t = std::get_if<TextPart>(&part)) {
                    if (!t->text.empty()) {
                        content.push_back({{"type", "text"}, {"text", t->text}});
                    }
                } else if (const auto* image = std::get_if<ImagePart>(&part)) {
                    Json source = image->url.empty()
                                      ? Json{{"type", "base64"}, {"media_type", image->mediaType}, {"data", base64Encode(image->bytes)}}
                                      : Json{{"type", "url"}, {"url", image->url}};
                    content.push_back({{"type", "image"}, {"source", std::move(source)}});
                } else if (const auto* call = std::get_if<ToolCall>(&part)) {
                    Json input = call->arguments.is_object() ? call->arguments : Json::object();
                    if (!call->arguments.is_object()) {
                        if (auto parsed = parseToolArguments(call->argumentsText)) {
                            input = *parsed;
                        }
                    }
                    content.push_back({{"type", "tool_use"}, {"id", call->id}, {"name", call->name}, {"input", input}});
                } else if (const auto* result = std::get_if<ToolResult>(&part)) {
                    Json block = {{"type", "tool_result"}, {"tool_use_id", result->callId}, {"content", result->content}};
                    if (result->isError) {
                        block["is_error"] = true;
                    }
                    results.push_back(std::move(block));
                } else if (const auto* r = std::get_if<ReasoningPart>(&part)) {
                    if (r->provider != kProvider) {
                        continue; // other providers' reasoning is not replayable here
                    }
                    content.push_back(r->redacted ? Json{{"type", "redacted_thinking"}, {"data", r->signature}}
                                                  : Json{{"type", "thinking"}, {"thinking", r->text}, {"signature", r->signature}});
                }
            }
            for (auto& block : content) {
                results.push_back(std::move(block));
            }
            if (results.empty()) {
                continue;
            }
            messages.push_back({{"role", message.role == Role::User ? "user" : "assistant"}, {"content", std::move(results)}});
        }
        if (request.cachePrompt && !messages.empty() && messages.back()["role"] == "user") { // thinking blocks take no breakpoint
            // Breakpoint at the end of the conversation: the next call of the loop reads it from the cache.
            Json& last = messages.back()["content"];
            if (last.is_array() && !last.empty()) {
                last.back()["cache_control"] = ephemeral;
            }
        }
        body["messages"] = std::move(messages);

        if (!request.tools.empty()) {
            Json tools = Json::array();
            for (const auto& tool : request.tools) {
                const bool toolStrict = tool.strict && strict;
                Json t = {{"name", tool.name},
                          {"description", tool.description},
                          {"input_schema", adaptSchema(tool.parameters, SchemaDialect::Anthropic, toolStrict)}};
                if (toolStrict) {
                    t["strict"] = true;
                }
                tools.push_back(std::move(t));
            }
            if (request.cachePrompt) {
                tools.back()["cache_control"] = ephemeral; // tools alone stay cached when the system prompt changes
            }
            body["tools"] = std::move(tools);
            Json choice;
            switch (request.toolChoice.mode) {
            case ToolChoiceMode::None:
                choice = {{"type", "none"}};
                break;
            case ToolChoiceMode::Required:
                // Opus/Sonnet 5.5 and Fable 5.1 reject forced choices: auto + strict tools instead.
                choice = {{"type", m_spec.caps.forcedToolChoice ? "any" : "auto"}};
                break;
            case ToolChoiceMode::Named:
                choice = m_spec.caps.forcedToolChoice ? Json{{"type", "tool"}, {"name", request.toolChoice.name}}
                                                      : Json{{"type", "auto"}};
                break;
            case ToolChoiceMode::Auto:
                choice = {{"type", "auto"}};
                break;
            }
            if (!request.toolChoice.allowParallel && request.toolChoice.mode != ToolChoiceMode::None) {
                choice["disable_parallel_tool_use"] = true;
            }
            body["tool_choice"] = std::move(choice);
        }
        if (request.responseFormat) {
            body["output_config"] = {
                {"format",
                 {{"type", "json_schema"},
                  {"schema", adaptSchema(request.responseFormat->schema, SchemaDialect::Anthropic, request.responseFormat->strict)}}}};
        }
        if (request.temperature && m_spec.caps.temperature) {
            body["temperature"] = *request.temperature;
        }
        if (!request.stopSequences.empty()) {
            body["stop_sequences"] = request.stopSequences;
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

std::unique_ptr<IChatModel> makeAnthropicChat(ProviderContext context) {
    return std::make_unique<AnthropicChat>(std::move(context));
}

} // namespace wabcut::ai::net
