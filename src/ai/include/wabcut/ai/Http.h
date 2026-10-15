#pragma once

// IHttpTransport: the one network seam of the AI module (docs/ai.md, "Decisions from the research brief"). Provider
// code in wabcut_ai and wabcut_ai_net talks HTTP only through it, so every adapter is testable with recorded
// fixtures and no test touches the network. Implementations:
//   - QtHttpTransport (wabcut_ai_net): QNetworkAccessManager on its own thread, TLS from the platform;
//   - RetryingTransport / LoggingTransport (here): decorators for retry-after/backoff and the redacted request log;
//   - test fakes that replay recorded responses through the same StreamDecoder.
//
// Calls block the calling thread (a job thread, never the GUI or audio thread) until the response is complete,
// failed or cancelled through the stop token. Streamed bodies are decoded into SSE events or NDJSON lines by
// StreamDecoder, so every transport parses streams identically.

#include <wabcut/ai/AiError.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai {

struct HttpHeader {
    std::string name;
    std::string value;

    bool operator==(const HttpHeader&) const = default;
};
using HttpHeaders = std::vector<HttpHeader>;

/// The parts of an absolute http(s) URL.
struct UrlParts {
    std::string scheme;      ///< "http" or "https" (lower case)
    std::string userinfo;    ///< what precedes '@' in the authority; empty when there is none
    std::string host;        ///< lower case; an IPv6 address without its brackets
    std::optional<int> port; ///< only when the URL names one
    std::string path;        ///< from the first '/', '?' or '#' after the authority (may be empty)
};

/// Strict parse of an absolute http(s) URL, for checking URLs that come from servers before anything is sent to
/// them: nullopt for other schemes, an empty host, a bad port, whitespace, control characters or backslashes
/// (which browsers and some HTTP stacks read as '/', a classic way to hide the real host).
std::optional<UrlParts> parseHttpUrl(std::string_view url);

/// The value of the first header named `name` (ASCII case-insensitive).
std::optional<std::string_view> findHeader(const HttpHeaders& headers, std::string_view name);
/// ASCII case-insensitive comparison, for header names.
bool equalsIgnoreCase(std::string_view a, std::string_view b);

/// How a 2xx response body is delivered.
enum class BodyMode {
    Buffered, ///< whole body in HttpResponse::body
    Sse,      ///< text/event-stream, decoded into SseEvents (HttpStreamHandlers::onEvent)
    Ndjson,   ///< newline-delimited JSON, one line per HttpStreamHandlers::onLine call
    Raw,      ///< bytes as they arrive (HttpStreamHandlers::onData), for downloads
};

/// One part of a multipart/form-data body: a text field (`value`) or a file (`file`, streamed from disk).
struct MultipartField {
    std::string name;
    std::string value;
    std::filesystem::path file;
    std::string fileName;    ///< filename= in Content-Disposition; defaults to file.filename()
    std::string contentType; ///< for files, e.g. "audio/flac"

    bool operator==(const MultipartField&) const = default;
};

/// A check the transports run before EVERY dispatch of a request, retries included: the guarded AI calls set it to
/// "the call's project still holds consent and no lock" (review wave 3, P1-5), so a lock turned on, or another
/// project opened, while a multi-request operation runs stops the next request before it leaves. An error is the
/// request's result and nothing is sent. Compares equal to any other check, so requests stay comparable.
struct DispatchCheck {
    std::function<std::expected<void, AiError>()> check;

    explicit operator bool() const { return static_cast<bool>(check); }
    bool operator==(const DispatchCheck&) const { return true; }
};

struct HttpRequest {
    std::string method = "POST";
    std::string url;
    HttpHeaders headers;
    std::string body;                     ///< usually JSON text
    std::filesystem::path bodyFile;       ///< when set, the body is this file, streamed (uploads)
    std::vector<MultipartField> multipart; ///< when not empty, a multipart/form-data body built from these
    BodyMode responseMode = BodyMode::Buffered;
    std::chrono::milliseconds timeout{std::chrono::minutes(10)};    ///< the whole exchange
    std::chrono::milliseconds idleTimeout{std::chrono::seconds(90)}; ///< longest silence while sending or receiving
    bool retrySafe = true; ///< false for non-idempotent paid job submissions
    /// true: the transport follows same-origin redirects itself (and fails on others). false: 3xx answers come back as
    /// responses (buffered, with their Location header) for the caller to follow on purpose (Redirects.h).
    bool followRedirects = true;
    DispatchCheck beforeDispatch; ///< run before each dispatch (RetryingTransport: before each retry)

    bool operator==(const HttpRequest&) const = default;
};

/// Status and headers of a 2xx response, before its body.
struct HttpResponseHead {
    int status = 0;
    HttpHeaders headers;
};

/// The final response. For streamed modes `body` is empty on 2xx (it went to the handlers); non-2xx bodies are
/// always buffered here so callers can classify the error (classifyHttpError).
struct HttpResponse {
    int status = 0;
    HttpHeaders headers;
    std::string body;

    bool ok() const { return status >= 200 && status < 300; }
};

/// One server-sent event (WHATWG HTML, "Server-sent events"). `event` is empty for unnamed events ("message").
struct SseEvent {
    std::string event;
    std::string data; ///< data lines joined with '\n'
    std::string id;

    bool operator==(const SseEvent&) const = default;
};

/// Callbacks for a request. Returning false from any of them aborts the request with AiErrorCode::Cancelled.
/// Handlers run on the transport's thread; they must be quick and must not call back into the transport.
struct HttpStreamHandlers {
    std::function<bool(const HttpResponseHead&)> onHead;    ///< once, for a 2xx response, before its body
    std::function<bool(const SseEvent&)> onEvent;           ///< BodyMode::Sse
    std::function<bool(std::string_view line)> onLine;      ///< BodyMode::Ndjson (line without its newline)
    std::function<bool(std::span<const std::byte>)> onData; ///< BodyMode::Raw
    std::function<void(std::uint64_t sent, std::uint64_t total)> onUploadProgress;
};

class IHttpTransport {
public:
    virtual ~IHttpTransport() = default;

    /// Sends `request` and blocks until the response is complete. HTTP error statuses are not errors at this
    /// level: they come back as an HttpResponse with the buffered error body. Errors: Network (DNS, TLS,
    /// connection, timeout), Cancelled (stop requested or a handler returned false), InvalidRequest (malformed
    /// URL, unreadable body file).
    virtual std::expected<HttpResponse, AiError> send(const HttpRequest& request, const HttpStreamHandlers& handlers,
                                                      std::stop_token stop) = 0;
};

/// Bounds of a streamed (SSE / NDJSON) response body (docs/ai.md, "Security"). A stream past them is aborted with
/// PayloadTooLarge instead of growing the editor's memory without end. Raw downloads are not limited here (the model
/// store checks their declared size).
struct StreamLimits {
    std::uint64_t maxTotalBytes = std::uint64_t{64} * 1024 * 1024; ///< the whole body
    std::size_t maxLineBytes = std::size_t{1} * 1024 * 1024;       ///< one line, terminated or not
    std::size_t maxEventBytes = std::size_t{4} * 1024 * 1024;      ///< one SSE event's joined data
};

/// Incremental decoder for streamed bodies, shared by every transport. Feed it bytes as they arrive; it calls the
/// matching handler for each complete SSE event (comments and keep-alives skipped, multi-line data joined, CR,
/// LF and CRLF line ends) or NDJSON line (blank lines skipped). In Raw mode it forwards the bytes. feed() and
/// finish() return false when a handler asked to stop. Per the SSE spec an event that is not terminated by a
/// blank line before the end of the stream is discarded.
class StreamDecoder {
public:
    StreamDecoder(BodyMode mode, const HttpStreamHandlers& handlers, StreamLimits limits = {});

    /// False when a handler asked to stop, a handler threw, or the stream broke a limit; error() says which of the
    /// last two it was (transports then fail the request with that error rather than Cancelled).
    bool feed(std::span<const std::byte> bytes);
    bool feed(std::string_view text);
    bool finish();
    const std::optional<AiError>& error() const { return m_error; }

private:
    bool processLine(std::string_view line);
    bool dispatchEvent();
    bool overflow(std::string what);
    /// Calls a handler; an exception becomes error() (ProviderError) and stops the stream.
    template <class Fn>
    bool call(Fn&& fn);

    BodyMode m_mode;
    const HttpStreamHandlers& m_handlers;
    StreamLimits m_limits;
    std::uint64_t m_total = 0;
    std::optional<AiError> m_error;
    std::string m_buffer;    // bytes of the current, unterminated line
    bool m_lastWasCr = false;
    bool m_started = false;  // a UTF-8 BOM is skipped at the very start
    std::string m_eventType;
    std::string m_data;
    std::string m_lastEventId;
    bool m_hasData = false;
};

/// Maps a non-2xx response onto an AiError: 401/403 Unauthorized; 402 and quota/billing codes QuotaExceeded;
/// 413 PayloadTooLarge; 429/529/503 "overloaded" RateLimited (retryAfter from Retry-After or the provider's body);
/// other 4xx InvalidRequest; 5xx ProviderError. Understands the error bodies of Anthropic, OpenAI-style APIs,
/// Google, Deepgram, ElevenLabs and AssemblyAI; the message is redacted.
AiError classifyHttpError(int status, const HttpHeaders& headers, std::string_view body);

/// Parses a Retry-After value: delay-seconds ("120", "1.5") or an IMF-fixdate HTTP date relative to `now`.
std::optional<std::chrono::milliseconds> parseRetryAfter(std::string_view value,
                                                         std::chrono::system_clock::time_point now);

/// The provider's retry hint: Retry-After, or retry-after-ms (OpenAI-style), whichever is present.
std::optional<std::chrono::milliseconds> retryAfterFrom(const HttpHeaders& headers,
                                                        std::chrono::system_clock::time_point now);

} // namespace wabcut::ai
