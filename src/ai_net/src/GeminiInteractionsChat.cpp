// Google Gemini Interactions API (ai.google.dev/gemini-api/docs/interactions, .../streaming, .../function-calling,
// .../video-understanding, .../files). Verified 2026-10-11: POST /v1beta/interactions with x-goog-api-key; stateless
// history as an array of steps (user_input, model_output, function_call, function_result, thought) with thought
// steps resent verbatim; SSE events interaction.created / step.start / step.delta / step.stop /
// interaction.completed, then [DONE]; native video by Files API URI (resumable upload, poll until ACTIVE).
// Where the docs disagree (streamed function arguments as "arguments_delta"/"arguments" vs "partial_arguments"),
// both forms are parsed. temperature is deprecated on current models and never sent.

#include "AdapterUtil.h"

#include <wabcut/ai/JsonAccess.h>

#include <wabcut/ai/ToolSchema.h>
#include <wabcut/ai_net/Adapters.h>
#include <wabcut/model/MediaLocator.h>

#include <filesystem>
#include <map>
#include <system_error>

namespace wabcut::ai::net {

namespace {

constexpr std::string_view kProvider = "gemini";

/// The documented host of the Files API's resumable upload URL (ai.google.dev/api/files, "upload_url").
constexpr std::string_view kUploadHost = "generativelanguage.googleapis.com";

/// The upload URL comes from the server's response, and the next request carries the API key and the user's video,
/// so it is checked before anything is sent (fail closed): HTTPS, the Files API's own host (or the configured base
/// URL's host when that is HTTPS too), no userinfo, the default port, the /upload/ path. The error names only the
/// host: the URL's query carries the upload session id.
std::expected<void, AiError> checkUploadUrl(std::string_view url, std::string_view baseUrl) {
    const auto parts = parseHttpUrl(url);
    const auto base = parseHttpUrl(baseUrl);
    auto refuse = [&](std::string why) -> std::expected<void, AiError> {
        return std::unexpected(AiError(AiErrorCode::ProviderError,
                                       "refused to upload the video: the Files API returned an upload URL " + why +
                                           "; nothing was sent there"));
    };
    if (!parts) {
        return refuse("that is not a valid https URL");
    }
    if (parts->scheme != "https") {
        return refuse("without HTTPS (" + parts->host + ")");
    }
    if (!parts->userinfo.empty()) {
        return refuse("with credentials in it");
    }
    const bool documentedHost = parts->host == kUploadHost;
    const bool baseHost = base && base->scheme == "https" && base->userinfo.empty() && parts->host == base->host;
    if (!documentedHost && !baseHost) {
        return refuse("on another host (" + parts->host + ")");
    }
    if (parts->port && *parts->port != 443) {
        return refuse("on port " + std::to_string(*parts->port));
    }
    if (!parts->path.starts_with("/upload/")) {
        return refuse("that is not the upload endpoint");
    }
    return {};
}

/// A Files API resource name, "files/<id>": anything else could make the status poll leave the API's path.
bool isFileName(std::string_view name) {
    constexpr std::string_view prefix = "files/";
    if (!name.starts_with(prefix) || name.size() == prefix.size() || name.size() > prefix.size() + 128) {
        return false;
    }
    return std::all_of(name.begin() + prefix.size(), name.end(), [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
    });
}

class GeminiInteractionsChat final : public IChatModel {
public:
    GeminiInteractionsChat(ProviderContext context, AdapterOptions options)
        : m_provider(context.provider), m_spec(context.model), m_baseUrl(std::move(context.baseUrl)),
          m_key(std::move(context.key)), m_transport(context.transport), m_options(std::move(options)) {}

    const ModelSpec& spec() const override { return m_spec; }

    std::expected<ChatResponse, AiError> chat(const ChatRequest& request, const ChatEventHandler& onEvent,
                                              std::stop_token stop) override {
        if (auto valid = validateChatRequest(request); !valid) {
            return std::unexpected(valid.error());
        }
        // Upload local videos first (native video input).
        ChatRequest prepared = request;
        for (auto& message : prepared.messages) {
            for (auto& part : message.parts) {
                auto* video = std::get_if<VideoPart>(&part);
                if (!video) {
                    continue;
                }
                if (m_spec.caps.videoInput != VideoInput::Native) {
                    return std::unexpected(AiError(AiErrorCode::Unsupported, "'" + m_spec.id + "' takes no video"));
                }
                if (video->fileUri.empty()) {
                    auto uri = uploadVideo(*video, stop);
                    if (!uri) {
                        return std::unexpected(uri.error());
                    }
                    video->fileUri = std::move(*uri);
                }
            }
        }

        const ProviderContext context{m_provider, m_spec, m_baseUrl, m_key.clone(), m_transport};
        HttpRequest http = jsonRequest(context, endpointUrl(context) + "?alt=sse", buildBody(prepared), BodyMode::Sse);

        ChatAccumulator acc(onEvent);
        std::map<std::int64_t, std::size_t> steps; // step index -> part index
        std::map<std::int64_t, Json> thoughts;     // step index -> thought step being rebuilt for verbatim replay
        std::optional<AiError> streamFailure;
        bool completed = false;
        std::string status;

        HttpStreamHandlers handlers;
        handlers.onEvent = [&](const SseEvent& event) {
            if (event.data == "[DONE]") {
                return true;
            }
            return guardEvent(acc, [&] {
                const auto data = Json::parse(event.data, nullptr, false);
                if (data.is_discarded() || !data.is_object()) {
                    return;
                }
                const std::string type = jsonStringOr(data, "event_type", event.event);
                const auto index = jsonInt(data, "index");
                auto partOf = [&]() -> std::optional<std::size_t> {
                    const auto it = index ? steps.find(*index) : steps.end();
                    return it == steps.end() ? std::nullopt : std::optional<std::size_t>(it->second);
                };
                if (type == "interaction.created") {
                    acc.response().id = stringAt(data, {"interaction", "id"});
                    acc.response().model = stringAt(data, {"interaction", "model"});
                } else if (type == "step.start") {
                    const Json* step = jsonObject(data, "step");
                    if (!index || !step) {
                        acc.fail(malformedEvent("step.start without an index or a step"));
                        return;
                    }
                    const std::string kind = jsonStringOr(*step, "type");
                    std::size_t part = ChatAccumulator::kNoPart;
                    if (kind == "model_output") {
                        part = acc.startText();
                        if (const Json* content = jsonArray(*step, "content")) {
                            for (const auto& c : *content) {
                                acc.appendText(part, jsonStringOr(c, "text"));
                            }
                        }
                    } else if (kind == "function_call") {
                        part = acc.startToolCall(jsonStringOr(*step, "id"), jsonStringOr(*step, "name"));
                        if (const Json* args = jsonObject(*step, "arguments"); args && !args->empty()) {
                            acc.setArguments(part, args->dump());
                        }
                    } else if (kind == "thought") {
                        thoughts[*index] = *step;
                        part = acc.startReasoning(ReasoningPart{std::string(kProvider), "", "", "", false});
                    }
                    if (part != ChatAccumulator::kNoPart) {
                        steps[*index] = part;
                    }
                } else if (type == "step.delta") {
                    const auto part = partOf();
                    const Json* delta = jsonObject(data, "delta");
                    if (!part || !delta) {
                        return;
                    }
                    const std::string kind = jsonStringOr(*delta, "type");
                    if (kind == "text") {
                        acc.appendText(*part, jsonStringOr(*delta, "text"));
                    } else if (kind == "arguments_delta" || kind == "arguments") {
                        std::string fragment = jsonStringOr(*delta, "arguments");
                        if (fragment.empty()) {
                            fragment = jsonStringOr(*delta, "partial_arguments");
                        }
                        acc.appendArguments(*part, fragment);
                    } else if (kind == "thought_signature") {
                        std::string signature = jsonStringOr(*delta, "signature");
                        if (signature.size() > kMaxToolArguments) {
                            acc.fail(AiError(AiErrorCode::PayloadTooLarge, "a thought signature is too large"));
                            return;
                        }
                        thoughts[*index]["signature"] = std::move(signature);
                    } else if (kind == "thought_summary") {
                        const std::string text =
                            jsonObject(*delta, "content") ? stringAt(*delta, {"content", "text"}) : jsonStringOr(*delta, "text");
                        acc.appendReasoning(*part, text);
                        if (acc.failed()) {
                            return;
                        }
                        Json& summary = thoughts[*index]["summary"];
                        if (!summary.is_array()) {
                            summary = Json::array();
                        }
                        if (!text.empty()) {
                            summary.push_back({{"type", "text"}, {"text", text}});
                        }
                    }
                } else if (type == "step.stop") {
                    const auto part = partOf();
                    if (!part) {
                        return;
                    }
                    if (acc.toolCall(*part)) {
                        acc.finishToolCall(*part);
                    } else if (auto t = thoughts.find(*index); t != thoughts.end()) {
                        acc.setSignature(*part, t->second.dump()); // the whole step, replayed verbatim
                    }
                } else if (type == "interaction.completed") {
                    const Json* interaction = jsonObject(data, "interaction");
                    if (!interaction) {
                        acc.fail(malformedEvent("interaction.completed without an interaction"));
                        return;
                    }
                    status = jsonStringOr(*interaction, "status", "completed");
                    Usage usage;
                    usage.inputTokens = intAt(*interaction, {"usage", "total_input_tokens"});
                    usage.cacheReadTokens = intAt(*interaction, {"usage", "total_cached_tokens"});
                    usage.reasoningTokens = intAt(*interaction, {"usage", "total_thought_tokens"});
                    usage.outputTokens = intAt(*interaction, {"usage", "total_output_tokens"}) + usage.reasoningTokens;
                    acc.updateUsage(usage);
                    completed = true;
                } else if (type == "error") {
                    const Json* error = jsonObject(data, "error");
                    streamFailure = streamError(error ? jsonIdOr(*error, "code") : std::string(),
                                                error ? jsonStringOr(*error, "message") : std::string());
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
        if (!completed) {
            return std::unexpected(AiError(AiErrorCode::Network, "the stream ended before interaction.completed"));
        }
        if (status == "failed" || status == "cancelled") {
            return std::unexpected(AiError(status == "failed" ? AiErrorCode::ProviderError : AiErrorCode::Cancelled,
                                           "the interaction " + status));
        }
        acc.response().rawStopReason = status;
        if (status == "incomplete") {
            acc.response().stopReason = StopReason::MaxTokens;
        } else {
            acc.response().stopReason = acc.hasToolCalls() ? StopReason::ToolUse : StopReason::EndTurn;
        }
        return acc.take();
    }

private:
    Json contentFor(const ContentPart& part) const {
        if (const auto* t = std::get_if<TextPart>(&part)) {
            return {{"type", "text"}, {"text", t->text}};
        }
        if (const auto* image = std::get_if<ImagePart>(&part)) {
            return image->url.empty() ? Json{{"type", "image"}, {"data", base64Encode(image->bytes)}, {"mime_type", image->mediaType}}
                                      : Json{{"type", "image"}, {"uri", image->url}, {"mime_type", image->mediaType}};
        }
        if (const auto* video = std::get_if<VideoPart>(&part)) {
            Json v = {{"type", "video"}, {"uri", video->fileUri}, {"mime_type", video->mediaType}};
            if (video->framesPerSecond || video->start || video->end) {
                Json processing = {{"type", "static"}};
                if (video->framesPerSecond) {
                    processing["fps"] = *video->framesPerSecond;
                }
                if (video->start) {
                    processing["start_offset"] = secondsString(*video->start);
                }
                if (video->end) {
                    processing["end_offset"] = secondsString(*video->end);
                }
                v["processing"] = std::move(processing);
            }
            return v;
        }
        return nullptr;
    }

    Json buildBody(const ChatRequest& request) const {
        Json body = {{"model", m_spec.id}, {"stream", true}, {"store", false}};
        if (!request.system.empty()) {
            body["system_instruction"] = request.system;
        }
        Json steps = Json::array();
        for (const auto& message : request.messages) {
            Json content = Json::array();
            auto flush = [&] {
                if (content.empty()) {
                    return;
                }
                steps.push_back({{"type", message.role == Role::User ? "user_input" : "model_output"}, {"content", content}});
                content = Json::array();
            };
            for (const auto& part : message.parts) {
                if (const auto* call = std::get_if<ToolCall>(&part)) {
                    flush();
                    Json args = call->arguments.is_object() ? call->arguments : Json::object();
                    if (!call->arguments.is_object()) {
                        if (auto parsed = parseToolArguments(call->argumentsText)) {
                            args = *parsed;
                        }
                    }
                    steps.push_back({{"type", "function_call"}, {"id", call->id}, {"name", call->name}, {"arguments", args}});
                } else if (const auto* result = std::get_if<ToolResult>(&part)) {
                    flush();
                    Json r = {{"type", "function_result"}, {"name", result->name}, {"call_id", result->callId}, {"result", result->content}};
                    if (result->isError) {
                        r["is_error"] = true;
                    }
                    steps.push_back(std::move(r));
                } else if (const auto* reasoning = std::get_if<ReasoningPart>(&part)) {
                    if (reasoning->provider != kProvider) {
                        continue;
                    }
                    auto step = Json::parse(reasoning->signature, nullptr, false);
                    if (!step.is_discarded() && step.is_object()) {
                        flush();
                        steps.push_back(std::move(step));
                    }
                } else if (Json c = contentFor(part); !c.is_null()) {
                    content.push_back(std::move(c));
                }
            }
            flush();
        }
        body["input"] = std::move(steps);

        Json generation = {{"max_output_tokens", request.maxOutputTokens}};
        if (!request.stopSequences.empty()) {
            generation["stop_sequences"] = request.stopSequences;
        }
        if (!request.tools.empty()) {
            Json tools = Json::array();
            for (const auto& tool : request.tools) {
                tools.push_back({{"type", "function"},
                                 {"name", tool.name},
                                 {"description", tool.description},
                                 {"parameters", adaptSchema(tool.parameters, SchemaDialect::Gemini, tool.strict)}});
            }
            body["tools"] = std::move(tools);
            switch (request.toolChoice.mode) {
            case ToolChoiceMode::Auto:
                break; // the default
            case ToolChoiceMode::None:
                generation["tool_choice"] = "none";
                break;
            case ToolChoiceMode::Required:
                generation["tool_choice"] = "any";
                break;
            case ToolChoiceMode::Named:
                generation["tool_choice"] = {{"allowed_tools", {{"mode", "any"}, {"tools", Json::array({request.toolChoice.name})}}}};
                break;
            }
        }
        body["generation_config"] = std::move(generation);
        if (request.responseFormat) {
            body["response_format"] = {{"type", "text"},
                                       {"mime_type", "application/json"},
                                       {"schema", adaptSchema(request.responseFormat->schema, SchemaDialect::Gemini,
                                                              request.responseFormat->strict)}};
        }
        return body;
    }

    /// Files API resumable upload, then waits until the file is ACTIVE. Returns the file URI.
    std::expected<std::string, AiError> uploadVideo(const VideoPart& video, std::stop_token stop) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(video.localFile, ec);
        if (ec) {
            return std::unexpected(AiError(AiErrorCode::InvalidRequest, "cannot read the video file"));
        }
        if (m_spec.maxUploadBytes > 0 && static_cast<std::int64_t>(size) > m_spec.maxUploadBytes) {
            return std::unexpected(AiError(AiErrorCode::PayloadTooLarge, "the video is larger than the Files API allows"));
        }
        const ProviderContext context{m_provider, m_spec, m_baseUrl, m_key.clone(), m_transport};

        HttpRequest start;
        start.method = "POST";
        start.url = joinUrl(m_baseUrl, "/upload/v1beta/files");
        start.headers = providerHeaders(context);
        start.headers.push_back({"X-Goog-Upload-Protocol", "resumable"});
        start.headers.push_back({"X-Goog-Upload-Command", "start"});
        start.headers.push_back({"X-Goog-Upload-Header-Content-Length", std::to_string(size)});
        start.headers.push_back({"X-Goog-Upload-Header-Content-Type", video.mediaType});
        start.headers.push_back({"content-type", "application/json"});
        start.body = Json{{"file", {{"display_name", pathToUtf8(video.localFile.filename())}}}}.dump();
        auto started = m_transport.send(start, {}, stop);
        if (!started) {
            return std::unexpected(started.error());
        }
        if (!started->ok()) {
            return std::unexpected(httpError(*started));
        }
        const auto uploadUrl = findHeader(started->headers, "x-goog-upload-url");
        if (!uploadUrl || uploadUrl->empty()) {
            return std::unexpected(AiError(AiErrorCode::ProviderError, "the Files API returned no upload URL"));
        }
        if (auto trusted = checkUploadUrl(*uploadUrl, m_baseUrl); !trusted) {
            return std::unexpected(trusted.error()); // never send the key or the video to an unchecked URL
        }

        HttpRequest upload;
        upload.method = "POST";
        upload.url = std::string(*uploadUrl);
        upload.headers = providerHeaders(context);
        upload.headers.push_back({"X-Goog-Upload-Offset", "0"});
        upload.headers.push_back({"X-Goog-Upload-Command", "upload, finalize"});
        upload.headers.push_back({"content-type", video.mediaType});
        upload.bodyFile = video.localFile;
        upload.timeout = std::chrono::hours(2);
        auto uploaded = m_transport.send(upload, {}, stop);
        if (!uploaded) {
            return std::unexpected(uploaded.error());
        }
        if (!uploaded->ok()) {
            return std::unexpected(httpError(*uploaded));
        }
        auto file = parseJsonBody(uploaded->body, "Files API upload");
        if (!file) {
            return std::unexpected(file.error());
        }
        const Json* nested = jsonObject(*file, "file");
        Json info = nested ? *nested : *file;
        if (!info.is_object()) {
            return std::unexpected(AiError(AiErrorCode::ProviderError, "the Files API answered with no file"));
        }

        const auto deadline = std::chrono::steady_clock::now() + m_options.maxPollTime;
        while (jsonStringOr(info, "state", "ACTIVE") == "PROCESSING") {
            if (std::chrono::steady_clock::now() > deadline) {
                return std::unexpected(AiError(AiErrorCode::Network, "the video was still processing at the deadline"));
            }
            if (!m_options.sleeper(m_options.filePollInterval, stop)) {
                return std::unexpected(AiError(AiErrorCode::Cancelled, "cancelled while the video was processing"));
            }
            const std::string name = jsonStringOr(info, "name");
            if (!isFileName(name)) {
                return std::unexpected(AiError(AiErrorCode::ProviderError, "the Files API returned an invalid file name"));
            }
            HttpRequest poll;
            poll.method = "GET";
            poll.url = joinUrl(m_baseUrl, "/v1beta/" + name);
            poll.headers = providerHeaders(context);
            auto polled = m_transport.send(poll, {}, stop);
            if (!polled) {
                return std::unexpected(polled.error());
            }
            if (!polled->ok()) {
                return std::unexpected(httpError(*polled));
            }
            auto j = parseJsonBody(polled->body, "Files API");
            if (!j) {
                return std::unexpected(j.error());
            }
            const Json* polledFile = jsonObject(*j, "file");
            info = polledFile ? *polledFile : *j;
            if (!info.is_object()) {
                return std::unexpected(AiError(AiErrorCode::ProviderError, "the Files API answered with no file"));
            }
        }
        if (jsonStringOr(info, "state") == "FAILED") {
            return std::unexpected(AiError(AiErrorCode::ProviderError, "Gemini could not process the video"));
        }
        const std::string uri = jsonStringOr(info, "uri");
        if (uri.empty()) {
            return std::unexpected(AiError(AiErrorCode::ProviderError, "the Files API returned no file URI"));
        }
        return uri;
    }

    ProviderSpec m_provider;
    ModelSpec m_spec;
    std::string m_baseUrl;
    Secret m_key;
    IHttpTransport& m_transport;
    AdapterOptions m_options;
};

} // namespace

std::unique_ptr<IChatModel> makeGeminiInteractionsChat(ProviderContext context, AdapterOptions options) {
    return std::make_unique<GeminiInteractionsChat>(std::move(context), std::move(options));
}

} // namespace wabcut::ai::net
