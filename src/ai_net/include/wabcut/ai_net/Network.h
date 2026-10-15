#pragma once

// The Qt network stack of the AI module (docs/ai.md: "ai_net: Qt network transport with SSE/NDJSON, retry with
// retry-after, cancellation and redaction").
//
// QtHttpTransport implements IHttpTransport with QNetworkAccessManager on a dedicated thread: send() may be called
// from any thread (typically AI job threads) and blocks until the exchange ends. TLS comes from the platform via
// Qt's TLS backend plugins. Timeouts: HttpRequest::idleTimeout is Qt's transfer timeout (no bytes either way for that
// long), HttpRequest::timeout bounds the whole exchange. Redirects are followed only within the same origin, so an
// Authorization header never travels to another host. A QCoreApplication must exist (Qt loads its TLS plugins
// through it).
//
// NetworkStack composes what the app uses: retry (honouring Retry-After) around the redacted request log around
// the Qt transport, so every attempt, including the 429s, shows up in the log.

#include <wabcut/ai/Http.h>
#include <wabcut/ai/Redact.h>
#include <wabcut/ai/RequestLog.h>
#include <wabcut/ai/Transport.h>

#include <memory>

namespace wabcut::ai::net {

class QtHttpTransport final : public IHttpTransport {
public:
    QtHttpTransport();
    ~QtHttpTransport() override;
    QtHttpTransport(const QtHttpTransport&) = delete;
    QtHttpTransport& operator=(const QtHttpTransport&) = delete;

    std::expected<HttpResponse, AiError> send(const HttpRequest& request, const HttpStreamHandlers& handlers,
                                              std::stop_token stop) override;

    /// Largest buffered body accepted (bigger responses fail with ProviderError). Streamed bodies are not limited.
    static constexpr std::int64_t kMaxBufferedBody = 256LL * 1024 * 1024;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class NetworkStack {
public:
    explicit NetworkStack(RequestLog& log, RetryPolicy policy = {}, const Redactor& redactor = Redactor::global());

    /// What adapters should use: retry -> request log -> Qt.
    IHttpTransport& transport() { return m_retrying; }
    QtHttpTransport& qt() { return m_qt; }

private:
    QtHttpTransport m_qt;
    LoggingTransport m_logging;
    RetryingTransport m_retrying;
};

} // namespace wabcut::ai::net
