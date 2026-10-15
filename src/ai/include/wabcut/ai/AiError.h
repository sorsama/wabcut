#pragma once

// AiError: the error type of every AI provider call (docs/ai.md, "Provider contract"). AI APIs return
// std::expected<T, AiError>. toError() maps it onto the shared wabcut::Error at module boundaries that only speak
// wabcut::Error.
//
// `code` is the stable, machine-readable part. `message` is diagnostic English: never shown to users verbatim and
// never asserted in tests. Messages and provider codes are already redacted (no keys, no Authorization headers).

#include <wabcut/model/Error.h>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace wabcut::ai {

enum class AiErrorCode {
    NoCredentials,    ///< no key is stored or set in the environment for the provider
    Unauthorized,     ///< the provider rejected the key (401/403)
    RateLimited,      ///< 429 or overloaded; retryAfter says when to try again, if the provider said
    QuotaExceeded,    ///< the account is out of credit or hit a provider-side spend limit (402, some 429s)
    PayloadTooLarge,  ///< the request or upload is over the provider's limit (413, or checked before sending)
    Network,          ///< DNS, TLS, connection reset, timeout
    ProviderError,    ///< the provider failed (5xx) or answered something this client cannot parse
    Cancelled,        ///< the caller's stop token was triggered
    ModelMissing,     ///< the model is unknown to the registry or a local model is not downloaded
    Deprecated,       ///< the model is past its deprecation date in the registry
    InvalidRequest,   ///< the request was rejected as malformed (400), or failed validation before sending
    Unsupported,      ///< the model or provider lacks the capability (e.g. native video, tools on this API)
    ConsentRequired,  ///< no consent recorded for this provider x capability (ConsentLedger)
    Offline,          ///< cloud AI is disabled (global offline mode or the project's no-cloud lock)
    SpendCapExceeded, ///< the call would exceed the provider's monthly spend cap (SpendGuard)
    InsufficientResources, ///< not enough GPU memory or RAM to run a local model (the resource governor refused)
};

std::string_view toString(AiErrorCode code);

struct AiError {
    AiErrorCode code = AiErrorCode::ProviderError;
    std::string message;                            ///< diagnostic English, redacted
    std::optional<std::chrono::milliseconds> retryAfter; ///< RateLimited (and some 5xx): the provider's hint
    int httpStatus = 0;                             ///< 0 when no HTTP response was received
    std::string providerCode;                       ///< the provider's own error type/code, e.g. "rate_limit_error"

    AiError() = default;
    AiError(AiErrorCode c, std::string msg) : code(c), message(std::move(msg)) {}

    /// True for failures that may succeed if the same request is sent again later: RateLimited, Network and
    /// 5xx ProviderErrors. Quota, auth and validation errors are never retryable.
    bool isRetryable() const;
};

/// The shared error for module boundaries that only speak wabcut::Error: Cancelled -> Cancelled,
/// Network -> IoError, InvalidRequest -> InvalidArgument, ModelMissing -> NotFound, Unsupported -> Unsupported,
/// everything else -> Unknown. The message is prefixed with the AiErrorCode name ("RateLimited: ...").
Error toError(const AiError& error);

} // namespace wabcut::ai
