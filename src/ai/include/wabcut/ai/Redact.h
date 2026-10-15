#pragma once

// One redaction filter for everything that leaves the AI module as text: logs, the request log, error messages and
// crash annotations (docs/internal/ai-research.md B.5). It covers the auth headers of every supported provider
// (Authorization, x-api-key, xi-api-key, x-goog-api-key, Ocp-Apim-Subscription-Key, DeepL-Auth-Key), the
// "Bearer/Token/Key <secret>" forms, ?key= / api_key= query parameters, and every secret value registered with
// addSecret() (keys loaded by KeyRing are registered automatically).

#include <wabcut/ai/Http.h>

#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai {

inline constexpr std::string_view kRedacted = "[REDACTED]";

/// True for header names that carry credentials (ASCII case-insensitive).
bool isSecretHeader(std::string_view name);

class Redactor {
public:
    /// The process-wide redactor (thread-safe).
    static Redactor& global();

    /// Registers a secret value so it is masked wherever it appears. Values shorter than 8 characters are ignored
    /// (too likely to match ordinary text).
    void addSecret(std::string_view value);
    void removeSecret(std::string_view value);
    /// Forgets every registered value (tests).
    void clearSecrets();

    std::string redact(std::string_view text) const;
    /// `text` with only the registered secrets replaced by `replacement` (the crash log, which has its own patterns).
    std::string redactSecrets(std::string_view text, std::string_view replacement) const;
    std::string redactUrl(std::string_view url) const;
    HttpHeaders redactHeaders(const HttpHeaders& headers) const;

private:
    mutable std::mutex m_mutex;
    std::vector<std::string> m_secrets;
};

} // namespace wabcut::ai
