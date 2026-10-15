// OpenAI Responses API (developers.openai.com/api/docs/guides/function-calling, .../reference/resources/responses).
// Verified 2026-10-11: GPT-6 Astra / 6.1 Sol need Responses for tool calling; typed SSE events; no [DONE] (the stream
// ends after response.completed / .failed / .incomplete); flat "error" events; strict json_schema rules. Requests are
// stateless (store: false) and ask for encrypted reasoning so reasoning items can be replayed in tool loops.

#include "AdapterUtil.h"

#include <wabcut/ai/JsonAccess.h>

#include <wabcut/ai/ToolSchema.h>
#include <wabcut/ai_net/Adapters.h>

#include <map>

namespace wabcut::ai::net {

namespace {

constexpr std::string_view kProvider = "openai-responses";

class OpenAiResponsesChat final : public IChatModel {
public:
    explicit OpenAiResponsesChat(ProviderContext context)
        : m_provider(context.provider), m_spec(context.model), m_baseUrl(std::move(context.baseUrl)),
          m_key(std::move(context.key)), m_transport(context.transport) {}

    const ModelSpec& spec() const override { return m_spec; }

    std::expected<ChatResponse, AiError> chat(const ChatRequest& request, const ChatEventHandler& onEvent,
                                              std::stop_token stop) override {
        if (auto valid = validateChatRequest(request); !valid) {
            return std::unexpected(valid.error());
        }
        if (auto video = rejectVideo(request, "OpenAI Responses"); !video) {
            return std::unexpected(video.error());
        }
        if (!request.tools.empty() && !m_spec.caps.toolsOnResponses) {
            return std::unexpected(AiError(AiErrorCode::Unsupported, "'" + m_spec.id + "' has no tool calling"));
        }
        const ProviderContext context{m_provider, m_spec, m_baseUrl, m_key.clone(), m_transport};
        HttpRequest http = jsonRequest(context, endpointUrl(context), buildBody(request), BodyMode::Sse);

        ChatAccumulator acc(onEvent);
        std::map<std::int64_t, std::size_t> items; // output_index -> part index
        std::optional<AiError> streamFailure;
        bool finished = false;

        auto usageFrom = [](const Json& u) {
            Usage usage;
            usage.inputTokens = intAt(u, {"input_tokens"});
            usage.cacheReadTokens = intAt(u, {"input_tokens_details", "cached_tokens"});
            usage.cacheWriteTokens = intAt(u, {"input_tokens_details", "cache_write_tokens"});
            usage.outputTokens = intAt(u, {"output_tokens"});
            usage.reasoningTokens = intAt(u, {"output_tokens_details", "reasoning_tokens"});
            return usage;
        };

        HttpStreamHandlers handlers;
        handlers.onEvent = [&](const SseEvent& event) {
            return guardEvent(acc, [&] {
                const auto data = Json::parse(event.data, nullptr, false);
                if (data.is_discarded() || !data.is_object()) {
                    return;
                }
                const std::string type = jsonStringOr(data, "type", event.event);
                const auto index = jsonInt(data, "output_index");
                auto partOf = [&]() -> std::optional<std::size_t> {
                    const auto it = index ? items.find(*index) : items.end();
                    return it == items.end() ? std::nullopt : std::optional<std::size_t>(it->second);
                };
                if (type == "response.created") {
                    acc.response().id = stringAt(data, {"response", "id"});
                    acc.response().model = stringAt(data, {"response", "model"});
                } else if (type == "response.output_item.added") {
                    const Json* item = jsonObject(data, "item");
                    if (!index || !item) {
                        acc.fail(malformedEvent("response.output_item.added without an output index or an item"));
                        return;
                    }
                    const std::string kind = jsonStringOr(*item, "type");
                    std::size_t part = ChatAccumulator::kNoPart;
                    if (kind == "message") {
                        part = acc.startText();
                    } else if (kind == "function_call") {
                        part = acc.startToolCall(jsonStringOr(*item, "call_id"), jsonStringOr(*item, "name"));
                        acc.appendArguments(part, jsonStringOr(*item, "arguments"));
                    } else if (kind == "reasoning") {
                        part = acc.startReasoning(ReasoningPart{std::string(kProvider), "", "", jsonStringOr(*item, "id"), false});
                    }
                    if (part != ChatAccumulator::kNoPart) {
                        items[*index] = part;
                    }
                } else if (type == "response.output_text.delta" || type == "response.refusal.delta") {
                    if (const auto part = partOf()) {
                        acc.appendText(*part, jsonStringOr(data, "delta"));
                    }
                    if (type == "response.refusal.delta") {
                        acc.response().stopReason = StopReason::Refusal;
                        acc.response().rawStopReason = "refusal";
                    }
                } else if (type == "response.function_call_arguments.delta") {
                    if (const auto part = partOf()) {
                        acc.appendArguments(*part, jsonStringOr(data, "delta"));
                    }
                } else if (type == "response.function_call_arguments.done") {
                    if (const auto part = partOf()) {
                        acc.setArguments(*part, jsonStringOr(data, "arguments"));
                        acc.finishToolCall(*part);
                    }
                } else if (type == "response.reasoning_summary_text.delta") {
                    if (const auto part = partOf()) {
                        acc.appendReasoning(*part, jsonStringOr(data, "delta"));
                    }
                } else if (type == "response.output_item.done") {
                    const Json* item = jsonObject(data, "item");
                    if (const auto part = partOf(); part && item && jsonStringOr(*item, "type") == "reasoning") {
                        acc.setSignature(*part, jsonStringOr(*item, "encrypted_content"));
                    }
                } else if (type == "response.completed" || type == "response.incomplete") {
                    const Json* response = jsonObject(data, "response");
                    if (!response) {
                        acc.fail(malformedEvent(type + " without a response"));
                        return;
                    }
                    const Json* usage = jsonObject(*response, "usage");
                    acc.updateUsage(usageFrom(usage ? *usage : Json::object()));
                    if (type == "response.incomplete") {
                        const std::string reason = stringAt(*response, {"incomplete_details", "reason"});
                        acc.response().rawStopReason = reason;
                        acc.response().stopReason =
                            reason == "max_output_tokens" ? StopReason::MaxTokens
                            : reason == "content_filter"  ? StopReason::ContentFilter
                                                          : StopReason::Other;
                    } else if (acc.response().stopReason != StopReason::Refusal) {
                        const bool tools = acc.hasToolCalls();
                        acc.response().stopReason = tools ? StopReason::ToolUse : StopReason::EndTurn;
                        acc.response().rawStopReason = tools ? "function_call" : "completed";
                    }
                    finished = true; // the server closes the stream (no [DONE] on Responses)
                } else if (type == "response.failed") {
                    const Json* error = jsonAt(data, {"response", "error"});
                    streamFailure = streamError(error ? jsonIdOr(*error, "code") : std::string(),
                                                error ? jsonStringOr(*error, "message") : std::string());
                } else if (type == "error") {
                    streamFailure = streamError(jsonIdOr(data, "code"), jsonStringOr(data, "message"));
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
        if (!finished) {
            return std::unexpected(AiError(AiErrorCode::Network, "the stream ended before response.completed"));
        }
        return acc.take();
    }

private:
    Json buildBody(const ChatRequest& request) const {
        Json body = {{"model", m_spec.id},
                     {"stream", true},
                     {"store", false},
                     {"max_output_tokens", request.maxOutputTokens},
                     {"include", Json::array({"reasoning.encrypted_content"})}};
        if (!request.system.empty()) {
            body["instructions"] = request.system;
        }
        Json input = Json::array();
        for (const auto& message : request.messages) {
            Json content = Json::array();
            auto flushContent = [&] {
                if (content.empty()) {
                    return;
                }
                input.push_back({{"role", message.role == Role::User ? "user" : "assistant"}, {"content", content}});
                content = Json::array();
            };
            for (const auto& part : message.parts) {
                if (const auto* t = std::get_if<TextPart>(&part)) {
                    if (!t->text.empty()) {
                        content.push_back(
                            {{"type", message.role == Role::User ? "input_text" : "output_text"}, {"text", t->text}});
                    }
                } else if (const auto* image = std::get_if<ImagePart>(&part)) {
                    content.push_back({{"type", "input_image"},
                                       {"image_url", image->url.empty() ? dataUrl(*image) : image->url},
                                       {"detail", "auto"}});
                } else if (const auto* call = std::get_if<ToolCall>(&part)) {
                    flushContent();
                    input.push_back({{"type", "function_call"},
                                     {"call_id", call->id},
                                     {"name", call->name},
                                     {"arguments", call->argumentsText.empty() ? call->arguments.dump() : call->argumentsText}});
                } else if (const auto* result = std::get_if<ToolResult>(&part)) {
                    flushContent();
                    input.push_back({{"type", "function_call_output"}, {"call_id", result->callId}, {"output", result->content}});
                } else if (const auto* r = std::get_if<ReasoningPart>(&part)) {
                    if (r->provider != kProvider || r->signature.empty()) {
                        continue;
                    }
                    flushContent();
                    input.push_back({{"type", "reasoning"}, {"id", r->id}, {"encrypted_content", r->signature}, {"summary", Json::array()}});
                }
            }
            flushContent();
        }
        body["input"] = std::move(input);

        const bool strict = m_spec.caps.strictSchema;
        if (!request.tools.empty()) {
            Json tools = Json::array();
            for (const auto& tool : request.tools) {
                const bool toolStrict = tool.strict && strict;
                tools.push_back({{"type", "function"},
                                 {"name", tool.name},
                                 {"description", tool.description},
                                 {"parameters", adaptSchema(tool.parameters,
                                                            toolStrict ? SchemaDialect::OpenAiStrict : SchemaDialect::OpenAiCompatible,
                                                            toolStrict)},
                                 {"strict", toolStrict}});
            }
            body["tools"] = std::move(tools);
            switch (request.toolChoice.mode) {
            case ToolChoiceMode::Auto:
                body["tool_choice"] = "auto";
                break;
            case ToolChoiceMode::None:
                body["tool_choice"] = "none";
                break;
            case ToolChoiceMode::Required:
                body["tool_choice"] = "required";
                break;
            case ToolChoiceMode::Named:
                body["tool_choice"] = {{"type", "function"}, {"name", request.toolChoice.name}};
                break;
            }
            body["parallel_tool_calls"] = request.toolChoice.allowParallel;
        }
        if (request.responseFormat) {
            const bool formatStrict = request.responseFormat->strict && strict;
            body["text"] = {{"format",
                             {{"type", "json_schema"},
                              {"name", request.responseFormat->name},
                              {"schema", adaptSchema(request.responseFormat->schema,
                                                     formatStrict ? SchemaDialect::OpenAiStrict : SchemaDialect::OpenAiCompatible,
                                                     formatStrict)},
                              {"strict", formatStrict}}}};
        }
        if (request.temperature && m_spec.caps.temperature) {
            body["temperature"] = *request.temperature;
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

std::unique_ptr<IChatModel> makeOpenAiResponsesChat(ProviderContext context) {
    return std::make_unique<OpenAiResponsesChat>(std::move(context));
}

} // namespace wabcut::ai::net
