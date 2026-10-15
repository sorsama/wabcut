#include <wabcut/ai/Redact.h>
#include <wabcut/ai/RequestLog.h>
#include <wabcut/ai/Transport.h>
#include <wabcut/ai/ConsentLedger.h>
#include <wabcut/model/MediaLocator.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <system_error>

namespace wabcut::ai {

std::expected<HttpResponse, AiError> OfflineGateTransport::send(const HttpRequest& request,
    const HttpStreamHandlers& handlers, std::stop_token stop) {
    HttpRequest guarded = request;
    guarded.beforeDispatch.check = [this, local = isLoopbackUrl(request.url), previous = request.beforeDispatch]() -> std::expected<void, AiError> {
        if (!local && !m_consent.cloudAllowed())
            return std::unexpected(AiError{AiErrorCode::Offline, "Offline / no-cloud mode blocks this AI network request"});
        return previous ? previous.check() : std::expected<void, AiError>{};
    };
    if (auto allowed = guarded.beforeDispatch.check(); !allowed) return std::unexpected(allowed.error());
    return m_inner.send(guarded, handlers, stop);
}

namespace {

bool isRetryableStatus(int status) {
    switch (status) {
    case 408:
    case 429:
    case 500:
    case 502:
    case 503:
    case 504:
    case 529:
        return true;
    default:
        return false;
    }
}

std::int64_t fileSize(const std::filesystem::path& file) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    return ec ? 0 : static_cast<std::int64_t>(size);
}

// Long strings in a JSON body (base64 images, audio) are replaced by their length before logging.
void elideLongStrings(nlohmann::json& j) {
    if (j.is_string()) {
        const auto& s = j.get_ref<const std::string&>();
        if (s.size() > 256) {
            j = "<" + std::to_string(s.size()) + " chars>";
        }
    } else if (j.is_array() || j.is_object()) {
        for (auto& child : j) {
            elideLongStrings(child);
        }
    }
}

std::string summarizeRequest(const HttpRequest& request, const Redactor& redactor) {
    std::string summary;
    if (!request.bodyFile.empty()) {
        summary = "<file " + pathToUtf8(request.bodyFile.filename()) + ", " + std::to_string(fileSize(request.bodyFile)) +
                  " bytes>";
    } else if (!request.multipart.empty()) {
        for (const auto& field : request.multipart) {
            if (!summary.empty()) {
                summary += "; ";
            }
            if (!field.file.empty()) {
                summary += field.name + "=<file " + pathToUtf8(field.file.filename()) + ", " +
                           std::to_string(fileSize(field.file)) + " bytes>";
            } else {
                summary += field.name + "=" + field.value.substr(0, 200);
            }
        }
    } else if (!request.body.empty()) {
        auto j = nlohmann::json::parse(request.body, nullptr, false);
        if (!j.is_discarded()) {
            elideLongStrings(j);
            summary = j.dump();
        } else {
            summary = request.body.substr(0, RequestLog::kMaxSummaryBytes);
        }
    }
    summary = redactor.redact(summary);
    if (summary.size() > RequestLog::kMaxSummaryBytes) {
        summary.resize(RequestLog::kMaxSummaryBytes);
        summary += "...";
    }
    return summary;
}

std::int64_t requestBytes(const HttpRequest& request) {
    if (!request.bodyFile.empty()) {
        return fileSize(request.bodyFile);
    }
    std::int64_t total = static_cast<std::int64_t>(request.body.size());
    for (const auto& field : request.multipart) {
        total += field.file.empty() ? static_cast<std::int64_t>(field.value.size()) : fileSize(field.file);
    }
    return total;
}

} // namespace

std::chrono::milliseconds backoffDelay(const RetryPolicy& policy, int attempt, double unitRandom) {
    double base = static_cast<double>(policy.initialBackoff.count()) * std::pow(policy.multiplier, std::max(0, attempt - 1));
    base = std::min(base, static_cast<double>(policy.maxBackoff.count()));
    const double factor = 1.0 + policy.jitter * (2.0 * std::clamp(unitRandom, 0.0, 1.0) - 1.0);
    return std::chrono::milliseconds(static_cast<std::int64_t>(std::llround(std::max(0.0, base * factor))));
}

bool interruptibleSleep(std::chrono::milliseconds delay, std::stop_token stop) {
    std::mutex mutex;
    std::condition_variable_any cv;
    std::unique_lock lock(mutex);
    cv.wait_for(lock, stop, delay, [] { return false; });
    return !stop.stop_requested();
}

// --- RetryingTransport ------------------------------------------------------------------------------------------

RetryingTransport::RetryingTransport(IHttpTransport& inner, RetryPolicy policy, Sleeper sleeper, std::uint64_t jitterSeed)
    : m_inner(inner), m_policy(policy), m_sleeper(std::move(sleeper)), m_random(jitterSeed) {}

double RetryingTransport::nextUnitRandom() {
    std::lock_guard lock(m_randomMutex);
    return std::uniform_real_distribution<double>(0.0, 1.0)(m_random);
}

std::expected<HttpResponse, AiError> RetryingTransport::send(const HttpRequest& request,
                                                             const HttpStreamHandlers& handlers, std::stop_token stop) {
    for (int attempt = 0;; ++attempt) {
        if (attempt > 0 && request.beforeDispatch) {
            // Consent or the no-cloud lock may have changed while waiting: a retry is a new dispatch.
            if (auto allowed = request.beforeDispatch.check(); !allowed) {
                return std::unexpected(allowed.error());
            }
        }
        bool headDelivered = false;
        HttpStreamHandlers wrapped = handlers;
        wrapped.onHead = [&](const HttpResponseHead& head) {
            headDelivered = true;
            return handlers.onHead ? handlers.onHead(head) : true;
        };
        auto result = m_inner.send(request, wrapped, stop);
        if (!request.retrySafe) return result;

        std::chrono::milliseconds wait{0};
        if (!result) {
            // Only failures before anything was streamed to the caller can be retried transparently.
            if (result.error().code != AiErrorCode::Network || headDelivered || attempt >= m_policy.maxRetries) {
                return result;
            }
            wait = backoffDelay(m_policy, attempt + 1, nextUnitRandom());
        } else {
            const HttpResponse& response = *result;
            if (response.ok() || !isRetryableStatus(response.status) || attempt >= m_policy.maxRetries) {
                return result;
            }
            const AiError error = classifyHttpError(response.status, response.headers, response.body);
            if (!error.isRetryable()) {
                return result; // quota / billing 429s
            }
            if (error.retryAfter) {
                if (*error.retryAfter > m_policy.maxRetryAfter) {
                    return result; // the caller reports RateLimited with the hint
                }
                wait = *error.retryAfter;
            } else {
                wait = backoffDelay(m_policy, attempt + 1, nextUnitRandom());
            }
        }
        m_totalRetries.fetch_add(1, std::memory_order_relaxed);
        if (!m_sleeper(wait, stop)) {
            return std::unexpected(AiError(AiErrorCode::Cancelled, "cancelled while waiting to retry"));
        }
    }
}

// --- LoggingTransport -------------------------------------------------------------------------------------------

LoggingTransport::LoggingTransport(IHttpTransport& inner, RequestLog& log, const Redactor& redactor)
    : m_inner(inner), m_log(log), m_redactor(redactor) {}

std::expected<HttpResponse, AiError> LoggingTransport::send(const HttpRequest& request,
                                                            const HttpStreamHandlers& handlers, std::stop_token stop) {
    RequestLogEntry entry;
    entry.kind = RequestLogEntry::Kind::Http;
    entry.time = nowIso8601();
    entry.method = request.method;
    entry.url = m_redactor.redactUrl(request.url);
    entry.bytesSent = requestBytes(request);
    entry.requestSummary = summarizeRequest(request, m_redactor);

    std::int64_t received = 0;
    HttpStreamHandlers counting = handlers;
    if (handlers.onEvent) {
        counting.onEvent = [&](const SseEvent& e) {
            received += static_cast<std::int64_t>(e.data.size());
            return handlers.onEvent(e);
        };
    }
    if (handlers.onLine) {
        counting.onLine = [&](std::string_view line) {
            received += static_cast<std::int64_t>(line.size());
            return handlers.onLine(line);
        };
    }
    if (handlers.onData) {
        counting.onData = [&](std::span<const std::byte> bytes) {
            received += static_cast<std::int64_t>(bytes.size());
            return handlers.onData(bytes);
        };
    }

    const auto start = std::chrono::steady_clock::now();
    auto result = m_inner.send(request, counting, stop);
    entry.durationMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if (result) {
        entry.status = result->status;
        entry.bytesReceived = received + static_cast<std::int64_t>(result->body.size());
        if (!result->ok()) {
            const AiError error = classifyHttpError(result->status, result->headers, result->body);
            entry.error = std::string(toString(error.code));
            entry.detail = error.message; // already redacted
        }
    } else {
        entry.bytesReceived = received;
        entry.error = std::string(toString(result.error().code));
        entry.detail = m_redactor.redact(result.error().message);
    }
    m_log.add(std::move(entry));
    return result;
}

} // namespace wabcut::ai
