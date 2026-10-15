#pragma once

// Transport decorators: retry with backoff honouring Retry-After, and the redacted local request log. The ai_net
// factory wraps the Qt transport in both; tests wrap a fake transport to check the retry schedule exactly.

#include <wabcut/ai/Http.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <random>

namespace wabcut::ai {

class ConsentLedger;
// Global AI network gate, including model downloads/key checks outside provider calls.
// Place outside RetryingTransport; composes and rechecks the caller's dispatch guard on retries.
class OfflineGateTransport final : public IHttpTransport {
public:
    OfflineGateTransport(IHttpTransport& inner, ConsentLedger& consent) : m_inner(inner), m_consent(consent) {}
    std::expected<HttpResponse, AiError> send(const HttpRequest&, const HttpStreamHandlers&, std::stop_token) override;
private:
    IHttpTransport& m_inner;
    ConsentLedger& m_consent;
};
class RequestLog;
class Redactor;

struct RetryPolicy {
    int maxRetries = 3;                                        ///< retries after the first attempt
    std::chrono::milliseconds initialBackoff{1000};            ///< first retry delay without a Retry-After hint
    double multiplier = 2.0;
    std::chrono::milliseconds maxBackoff{std::chrono::seconds(30)};
    std::chrono::milliseconds maxRetryAfter{std::chrono::seconds(120)}; ///< longer hints are not waited for
    double jitter = 0.2;                                       ///< +-20% of the backoff (not of Retry-After)
};

/// Delay before retry number `attempt` (1-based) without a server hint: initialBackoff * multiplier^(attempt-1),
/// capped at maxBackoff, scaled by (1 + jitter * (2 * unitRandom - 1)) with unitRandom in [0, 1].
std::chrono::milliseconds backoffDelay(const RetryPolicy& policy, int attempt, double unitRandom);

/// Waits `delay` unless `stop` fires first. Returns false when stopped.
using Sleeper = std::function<bool(std::chrono::milliseconds delay, std::stop_token stop)>;
bool interruptibleSleep(std::chrono::milliseconds delay, std::stop_token stop);

/// Retries a request when the failure is transient:
///   - a Network error before any 2xx response arrived (nothing was streamed to the handlers yet), and
///   - 408, 429, 500, 502, 503, 504 and 529 responses that classifyHttpError() calls retryable (quota and billing
///     429s are not).
/// It waits the Retry-After hint when the response has one (up to maxRetryAfter; a longer hint returns the
/// response at once so the caller can report RateLimited with that retryAfter), otherwise backoffDelay().
class RetryingTransport final : public IHttpTransport {
public:
    RetryingTransport(IHttpTransport& inner, RetryPolicy policy = {}, Sleeper sleeper = interruptibleSleep,
                      std::uint64_t jitterSeed = std::random_device{}());

    /// Reentrant: several jobs may send through one RetryingTransport at once.
    std::expected<HttpResponse, AiError> send(const HttpRequest& request, const HttpStreamHandlers& handlers,
                                              std::stop_token stop) override;

    /// Retries performed by all send() calls so far (tests, diagnostics).
    int totalRetries() const { return m_totalRetries.load(std::memory_order_relaxed); }

private:
    double nextUnitRandom();

    IHttpTransport& m_inner;
    RetryPolicy m_policy;
    Sleeper m_sleeper;
    std::mutex m_randomMutex;
    std::mt19937_64 m_random;
    std::atomic<int> m_totalRetries{0};
};

/// Records every exchange in a RequestLog: method, redacted URL, status, sizes, duration and a redacted,
/// truncated copy of the request body. Never records response bodies of successful calls, file contents or
/// headers' secret values.
class LoggingTransport final : public IHttpTransport {
public:
    LoggingTransport(IHttpTransport& inner, RequestLog& log, const Redactor& redactor);

    std::expected<HttpResponse, AiError> send(const HttpRequest& request, const HttpStreamHandlers& handlers,
                                              std::stop_token stop) override;

private:
    IHttpTransport& m_inner;
    RequestLog& m_log;
    const Redactor& m_redactor;
};

} // namespace wabcut::ai
