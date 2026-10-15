#pragma once

// Consent and the no-cloud locks (docs/ai.md principle 4, docs/internal/ai-research.md "Privacy and consent").
//
// Before the first cloud call per provider x capability the app shows a ConsentPrompt: what will be sent, to whom,
// the provider's data terms and the cost estimate. "Always allow" is recorded here; "Allow once" is a per-call
// decision the caller passes to authorize(). Two locks block every cloud call:
//   - offline mode: the user's global switch;
//   - the project lock: a project-level "Offline / no-cloud" setting (e.g. for footage under NDA). It belongs to the
//     project (ProjectState::aiNoCloud, saved with it): the app sets a lock source that reads the ACTIVE project,
//     and every state()/authorize() consults it, so a reopened, recovered or switched-to project decides for itself
//     and recorded consent never overrides it.
// Local providers on this machine (loopback URLs) need no consent and are allowed while offline; a "local"
// provider pointed at another host is treated as cloud.

#include <wabcut/ai/AiError.h>
#include <wabcut/ai/ModelRegistry.h>
#include <wabcut/ai/SpendGuard.h>

#include <expected>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai {

/// True for http(s) URLs whose host is localhost, 127.0.0.0/8 or [::1].
bool isLoopbackUrl(std::string_view url);

/// The provider sends data off this machine: not a local provider, or a local one configured with a remote URL.
bool isCloud(const ProviderSpec& provider, std::string_view baseUrl);

enum class ConsentState {
    NotNeeded,   ///< local provider on this machine
    Granted,     ///< "Always allow" is recorded
    NeedsPrompt, ///< ask the user first
    Blocked,     ///< offline mode or the project's no-cloud lock
};

struct ConsentRecord {
    std::string provider;
    Capability capability = Capability::Chat;
    std::string grantedAt; ///< ISO-8601 UTC
    std::string termsUrl;  ///< the terms the user saw

    bool operator==(const ConsentRecord&) const = default;
};

/// Everything the first-use prompt shows.
struct ConsentPrompt {
    std::string provider;
    std::string providerName;
    Capability capability = Capability::Chat;
    std::string payload;     ///< what is sent, e.g. "audio of 3 clips, 12 min" (from the caller)
    std::string destination; ///< host the data goes to, e.g. "api.deepgram.com"
    std::string termsUrl;
    std::string dataPolicy;
    CostEstimate cost;
};

ConsentPrompt makeConsentPrompt(const ProviderSpec& provider, std::string_view baseUrl, Capability capability,
                                std::string payload, CostEstimate cost);

/// Thread-safe.
class ConsentLedger {
public:
    ConsentLedger() = default;
    ConsentLedger(const ConsentLedger&) = delete;
    ConsentLedger& operator=(const ConsentLedger&) = delete;

    void setOfflineMode(bool offline);
    bool offlineMode() const;
    /// A lock set directly (callers without a project, e.g. tests); the source below is ORed in.
    void setProjectLock(bool locked);
    /// Reads the active project's lock (ProjectState::aiNoCloud) at every check. Called with the ledger's mutex
    /// held, from any thread: it must be quick and thread-safe (EditSession::snapshot() is). Null clears it.
    void setProjectLockSource(std::function<bool()> source);
    bool projectLock() const;
    /// The identity of the active project (it changes whenever another project is opened, created or recovered; 0 =
    /// unknown), read at every dispatch of a guarded call: an operation stays bound to the project it started on, so
    /// switching documents never lets it carry one project's data on under another's settings. Same rules as the
    /// lock source. Null clears it.
    void setProjectIdentitySource(std::function<std::uint64_t()> source);
    std::uint64_t projectIdentity() const;
    /// Neither lock is on.
    bool cloudAllowed() const;

    ConsentState state(const ProviderSpec& provider, std::string_view baseUrl, Capability capability) const;

    /// Ok when the call may go ahead: a local provider, a recorded consent, or `allowOnce` (the user just said
    /// "Allow once"). A recorded consent does not cover a call an extension makes (`caller` "extension:<id>",
    /// CallOptions::caller): only allowOnce does. Errors: Offline (a lock is on; allowOnce does not override it),
    /// ConsentRequired.
    std::expected<void, AiError> authorize(const ProviderSpec& provider, std::string_view baseUrl,
                                           Capability capability, bool allowOnce = false, std::string_view caller = {}) const;

    void grantAlways(const ProviderSpec& provider, Capability capability, std::string grantedAt);
    void revoke(std::string_view provider, Capability capability);
    void revokeAll();
    std::vector<ConsentRecord> records() const;

    /// Offline mode and the records (the project lock belongs to the project and is not included).
    Json toJson() const;
    std::expected<void, AiError> loadJson(const Json& json);
    void setOnChanged(std::function<void()> callback);

private:
    void notifyChanged();
    bool projectLockWhileHeld() const; ///< the project lock; m_mutex held

    mutable std::mutex m_mutex;
    bool m_offline = false;
    bool m_projectLock = false;
    std::function<bool()> m_projectLockSource;
    std::function<std::uint64_t()> m_projectIdentitySource;
    std::vector<ConsentRecord> m_records;
    std::function<void()> m_onChanged;
};

} // namespace wabcut::ai
