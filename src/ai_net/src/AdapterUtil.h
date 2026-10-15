#pragma once

// Helpers shared by the wire adapters (private to wabcut_ai_adapters).

#include <wabcut/ai/Capability.h>
#include <wabcut/ai/Http.h>
#include <wabcut/ai/ModelRegistry.h>
#include <wabcut/ai/Provider.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::net {

std::string base64Encode(std::span<const std::uint8_t> bytes);
/// Standard or URL-safe base64, padding optional, whitespace skipped; nullopt on any other character.
std::optional<std::vector<std::uint8_t>> base64Decode(std::string_view text);
/// "data:image/png;base64,...."
std::string dataUrl(const ImagePart& image);

/// Seconds (as providers send them) -> ticks, at microsecond precision. nullopt for NaN, negatives or overflow.
std::optional<Ticks> ticksFromSeconds(double seconds);
std::optional<Ticks> ticksFromMilliseconds(std::int64_t milliseconds);
/// "12.5s" (Gemini offsets); whole microseconds at most.
std::string secondsString(Ticks t);

/// "en-US", "en_us", "EN" -> "en"; ISO 639-3 codes of common languages ("eng", "tha", "jpn") -> 639-1. Unknown
/// codes are returned lower-cased.
std::string normalizeLanguage(std::string_view code);

/// The endpoint URL of a model: its endpoint below the base URL.
std::string endpointUrl(const ProviderContext& context);

/// A JSON POST with the provider's auth and extra headers.
HttpRequest jsonRequest(const ProviderContext& context, std::string url, const Json& body, BodyMode mode);
/// Auth + extra headers only (uploads, GETs).
HttpHeaders providerHeaders(const ProviderContext& context);

/// classifyHttpError for a finished non-2xx response.
AiError httpError(const HttpResponse& response);

/// Parses a buffered JSON body; ProviderError when it is not a JSON object.
std::expected<Json, AiError> parseJsonBody(std::string_view body, std::string_view what);

/// An error event inside a 2xx stream (or a failed status), turned into an AiError: `status` picks the category
/// the provider's code maps to (429 for rate limits, 529 overloaded, 500 otherwise).
AiError streamError(std::string_view providerCode, std::string_view message);

/// Rejects parts the adapter cannot carry (VideoPart on APIs without native video).
std::expected<void, AiError> rejectVideo(const ChatRequest& request, std::string_view api);

std::int64_t intAt(const Json& j, std::initializer_list<const char*> path);
std::string stringAt(const Json& j, std::initializer_list<const char*> path);

/// Bounds of one streamed response (docs/ai.md, "Security"): a provider (or something in between) that streams
/// without end is cut off with an explicit error instead of filling memory.
inline constexpr std::size_t kMaxResponseText = std::size_t{8} * 1024 * 1024;  ///< text + reasoning of a response
inline constexpr std::size_t kMaxToolArguments = std::size_t{1} * 1024 * 1024; ///< one tool call's arguments
inline constexpr std::size_t kMaxResponseParts = 1024;                         ///< parts of a response

/// Accumulates the parts of a streamed assistant message and emits ChatEvents as they arrive. It never throws:
/// a delta for a part that does not exist or has another kind, or a response past the bounds above, records an
/// error (ProviderError / PayloadTooLarge), and handlers return !failed() so the transport aborts the stream.
class ChatAccumulator {
public:
    /// What start*() returns when the response cannot take another part (see failed()).
    static constexpr std::size_t kNoPart = static_cast<std::size_t>(-1);

    explicit ChatAccumulator(const ChatEventHandler& onEvent) : m_onEvent(onEvent) {}

    std::size_t startText();
    void appendText(std::size_t part, std::string_view text);
    std::size_t startToolCall(std::string id, std::string name);
    void appendArguments(std::size_t part, std::string_view text);
    void setArguments(std::size_t part, std::string text);
    /// Parses the accumulated arguments (an empty text becomes {}); invalid JSON leaves arguments null.
    void finishToolCall(std::size_t part);
    std::size_t startReasoning(ReasoningPart part);
    void appendReasoning(std::size_t part, std::string_view text);
    /// Replay state of a reasoning part (counted against the response's text bound).
    void appendSignature(std::size_t part, std::string_view signature);
    void setSignature(std::size_t part, std::string signature);
    /// The part when it exists and has that kind, else nullptr.
    ReasoningPart* reasoning(std::size_t part);
    ToolCall* toolCall(std::size_t part);
    void updateUsage(const Usage& usage);

    /// Records a malformed event or an overflow (the first error wins).
    void fail(AiError error);
    bool failed() const { return m_error.has_value(); }
    const std::optional<AiError>& error() const { return m_error; }

    bool hasToolCalls() const;
    ChatResponse& response() { return m_response; }
    /// Finishes every tool call and drops empty text parts.
    ChatResponse take();

private:
    bool newPart();
    bool addText(std::size_t bytes);

    const ChatEventHandler& m_onEvent;
    ChatResponse m_response;
    std::size_t m_textBytes = 0;
    std::optional<AiError> m_error;
};

/// ProviderError for an event that does not have the documented shape.
AiError malformedEvent(std::string_view what);

/// Runs `body` (an adapter's handling of one stream event) and turns any exception it throws (a JSON type error, an
/// out-of-range index) into a ProviderError recorded on `acc`: the request fails cleanly instead of throwing on the
/// transport's thread. Returns !acc.failed(), what a stream handler returns to keep (or stop) the stream.
template <class Body>
bool guardEvent(ChatAccumulator& acc, Body&& body) {
    try {
        body();
    } catch (const std::exception& e) {
        acc.fail(AiError(AiErrorCode::ProviderError, std::string("malformed provider event: ") + e.what()));
    } catch (...) {
        acc.fail(AiError(AiErrorCode::ProviderError, "malformed provider event"));
    }
    return !acc.failed();
}

/// Builds a Transcript from provider words: times are made monotonic, labels become 0, 1, ... in order of first
/// appearance, and segments come from the provider's utterances or, without them, from speaker changes and
/// pauses longer than 1.5 s.
class TranscriptBuilder {
public:
    explicit TranscriptBuilder(const TranscribeRequest& request);

    /// `speaker` is the provider's label ("A", "speaker_0", "1"); empty when unknown.
    void addWord(std::string text, Ticks t0, Ticks t1, float confidence, std::string_view speaker);
    void addUtterance(Ticks t0, Ticks t1);
    void setLanguage(std::string_view language);

    /// Builds and validates the transcript. ProviderError if the provider's data is inconsistent.
    std::expected<Transcript, AiError> build(Provenance provenance);

private:
    std::optional<int> speakerId(std::string_view label);

    const TranscribeRequest& m_request;
    Transcript m_transcript;
    std::vector<std::pair<Ticks, Ticks>> m_utterances;
    std::vector<std::string> m_labels;
};

/// Provenance of a cloud transcription.
Provenance cloudProvenance(const ProviderContext& context, const TranscribeRequest& request, std::string modelUsed,
                           std::string modelVersion);

} // namespace wabcut::ai::net
