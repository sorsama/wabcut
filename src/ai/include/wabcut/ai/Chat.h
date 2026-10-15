#pragma once

// Canonical chat types shared by every IChatModel adapter (Anthropic Messages, OpenAI Responses, Chat Completions,
// Gemini Interactions): messages made of typed parts, tool specs in a strict JSON-Schema subset (ToolSchema.h),
// structured output, usage and stream events. Adapters translate these to and from their wire formats; nothing
// here is vendor-specific except the opaque replay state in ReasoningPart / ToolCall::signature.

#include <wabcut/model/Time.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace wabcut::ai {

using Json = nlohmann::json;

/// Who wrote a message. The system prompt is ChatRequest::system, and tool results travel in User messages
/// (adapters move them to the role their API wants, e.g. Chat Completions' "tool" role).
enum class Role { User, Assistant };

struct TextPart {
    std::string text;

    bool operator==(const TextPart&) const = default;
};

/// An image, inline (bytes; base64 on the wire) or by HTTPS URL. Exactly one of `bytes` / `url` is set.
struct ImagePart {
    std::string mediaType;            ///< "image/png", "image/jpeg", "image/webp" or "image/gif"
    std::vector<std::uint8_t> bytes;  ///< encoded image file
    std::string url;                  ///< alternative to bytes

    bool operator==(const ImagePart&) const = default;
};

/// A video for models with native video input (ModelCapabilities::videoInput == Native). Either a provider file
/// reference that is already uploaded (`fileUri`) or a local file the adapter uploads first. Models with
/// VideoInput::Frames need sampled ImageParts instead; adapters reject a VideoPart with Unsupported.
struct VideoPart {
    std::string mediaType = "video/mp4";
    std::string fileUri;                   ///< provider file reference (e.g. a Gemini Files API URI)
    std::filesystem::path localFile;       ///< uploaded by the adapter when fileUri is empty
    std::optional<double> framesPerSecond; ///< sampling hint, where the provider supports one
    std::optional<Ticks> start;            ///< clip range inside the file (source time)
    std::optional<Ticks> end;

    bool operator==(const VideoPart&) const = default;
};

/// A tool call made by the model. `arguments` is the parsed object; `argumentsText` is the raw JSON text exactly as
/// the model produced it (streamed arguments are accumulated as text and parsed once at the end).
struct ToolCall {
    std::string id;            ///< provider call id, echoed in the ToolResult
    std::string name;
    Json arguments;            ///< object; null when argumentsText is not valid JSON
    std::string argumentsText;
    std::string signature;     ///< opaque provider state that must be echoed back (Gemini thought signature)

    bool operator==(const ToolCall&) const = default;
};

/// The result of running a tool, sent back to the model in a User message.
struct ToolResult {
    std::string callId;   ///< ToolCall::id
    std::string name;     ///< ToolCall::name (Gemini needs it)
    std::string content;  ///< what the tool returned, usually JSON text
    bool isError = false; ///< the tool failed; content explains why

    bool operator==(const ToolResult&) const = default;
};

/// Provider reasoning that has to be replayed verbatim in a tool loop (Anthropic thinking blocks with their
/// signature, OpenAI reasoning items with encrypted content, Gemini thoughts). Never shown as answer text.
/// Adapters replay only parts whose `provider` matches their own wire format and drop the others.
struct ReasoningPart {
    std::string provider;  ///< "anthropic", "openai-responses", "gemini"
    std::string text;      ///< thinking or summary text; often empty
    std::string signature; ///< Anthropic signature / redacted data, OpenAI encrypted_content, Gemini signature
    std::string id;        ///< OpenAI reasoning item id
    bool redacted = false; ///< Anthropic redacted_thinking (the data is in `signature`)

    bool operator==(const ReasoningPart&) const = default;
};

using ContentPart = std::variant<TextPart, ImagePart, VideoPart, ToolCall, ToolResult, ReasoningPart>;

struct ChatMessage {
    Role role = Role::User;
    std::vector<ContentPart> parts;

    static ChatMessage user(std::string text);
    static ChatMessage assistant(std::string text);
    /// A User message carrying tool results (one per call), the reply to an assistant turn with tool calls.
    static ChatMessage toolResults(std::vector<ToolResult> results);

    /// The concatenated text parts.
    std::string text() const;
    std::vector<ToolCall> toolCalls() const;

    bool operator==(const ChatMessage&) const = default;
};

/// A tool the model may call. `parameters` is a JSON Schema object in the strict subset of ToolSchema.h, so the
/// same spec works on every adapter. `name` matches [A-Za-z0-9_-]{1,64}.
struct ToolSpec {
    std::string name;
    std::string description;
    Json parameters = Json::object();
    bool strict = true; ///< ask the provider to guarantee schema-valid arguments, where it can

    bool operator==(const ToolSpec&) const = default;
};

/// Converts the command catalog's JSON-Schema export (wabcut::command::exportTools(..., ToolFormat::JsonSchema):
/// [{"toolName", "description", "parameters", ...}]) into tool specs. Entries without a toolName are skipped.
std::vector<ToolSpec> toolSpecsFromCommandExport(const Json& exported);

enum class ToolChoiceMode {
    Auto,     ///< the model decides (the only mode some models accept with strict tools)
    None,     ///< tools are listed but must not be called
    Required, ///< the model must call some tool
    Named,    ///< the model must call `name`
};

struct ToolChoice {
    ToolChoiceMode mode = ToolChoiceMode::Auto;
    std::string name;          ///< for Named
    bool allowParallel = true; ///< several tool calls in one turn

    bool operator==(const ToolChoice&) const = default;
};

/// Structured output: the final text must be JSON matching `schema` (strict subset, ToolSchema.h).
struct ResponseFormat {
    std::string name = "result"; ///< [A-Za-z0-9_-]{1,64}
    Json schema = Json::object();
    bool strict = true;

    bool operator==(const ResponseFormat&) const = default;
};

struct ChatRequest {
    std::string system;
    std::vector<ChatMessage> messages;
    std::vector<ToolSpec> tools;
    ToolChoice toolChoice;
    std::optional<ResponseFormat> responseFormat;
    int maxOutputTokens = 4096;
    std::optional<double> temperature; ///< omitted unless set: several current models reject anything but 1.0
    std::vector<std::string> stopSequences;
    /// Ask the provider to cache the prompt prefix (tools, system prompt, the conversation so far) for the next
    /// call of a tool loop. Anthropic needs explicit breakpoints (cache_control on the last tool, the system
    /// prompt and the last message); OpenAI and Gemini cache long prefixes automatically and ignore this.
    bool cachePrompt = false;
};

enum class StopReason {
    EndTurn,      ///< the model finished its answer
    ToolUse,      ///< the model wants tool results
    MaxTokens,    ///< cut at maxOutputTokens
    StopSequence,
    Refusal,      ///< the model declined (safety); the text explains
    ContentFilter,
    Other,        ///< anything else; ChatResponse::rawStopReason has the provider's value
};

std::string_view toString(StopReason reason);

/// Token and media usage of one call. Input counts include cached tokens; output counts include reasoning.
struct Usage {
    std::int64_t inputTokens = 0;
    std::int64_t outputTokens = 0;
    std::int64_t cacheReadTokens = 0;  ///< part of inputTokens billed at the cached rate
    std::int64_t cacheWriteTokens = 0; ///< part of inputTokens billed at the cache-write rate
    std::int64_t reasoningTokens = 0;  ///< part of outputTokens
    std::int64_t audioMilliseconds = 0;
    std::int64_t characters = 0;       ///< text-to-speech / translation input characters
    std::int64_t videoMilliseconds = 0; ///< generated video (billed per second)
    std::int64_t images = 0;            ///< generated images
    /// Generated media: the price tier that applies (PriceRule::tier), e.g. "720p", "1080p+audio"; empty = untiered.
    std::string mediaTier;

    Usage& operator+=(const Usage& other);
    bool operator==(const Usage&) const = default;
};

struct ChatResponse {
    ChatMessage message;       ///< Assistant: reasoning, text and tool calls in the order the model produced them
    StopReason stopReason = StopReason::EndTurn;
    std::string rawStopReason; ///< the provider's value, e.g. "end_turn", "tool_calls"
    Usage usage;
    std::string model;         ///< as reported by the provider
    std::string id;            ///< provider response id (for support requests)
};

// --- Streaming ------------------------------------------------------------------------------------------------
// Events refer to parts by their index in ChatResponse::message.parts, so a UI can render text as it streams
// and show tool calls as soon as their name is known.

struct TextDelta {
    std::size_t part = 0;
    std::string text;
};
struct ToolCallStarted {
    std::size_t part = 0;
    std::string id;
    std::string name;
};
struct ToolCallArgumentsDelta {
    std::size_t part = 0;
    std::string text; ///< a fragment of the arguments JSON
};
struct ReasoningDelta {
    std::size_t part = 0;
    std::string text;
};
struct UsageUpdate {
    Usage usage; ///< cumulative so far
};

using ChatEvent = std::variant<TextDelta, ToolCallStarted, ToolCallArgumentsDelta, ReasoningDelta, UsageUpdate>;
using ChatEventHandler = std::function<void(const ChatEvent&)>;

} // namespace wabcut::ai
