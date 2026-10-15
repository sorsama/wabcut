#include <wabcut/ai/ConsentLedger.h>
#include <wabcut/ai/JsonAccess.h>

#include <algorithm>
#include <cctype>

namespace wabcut::ai {

namespace {

/// Lower-cased host of an http(s) URL, without brackets or port; empty when the URL is not http(s).
std::string hostOf(std::string_view url) {
    std::size_t start = 0;
    if (url.starts_with("http://")) {
        start = 7;
    } else if (url.starts_with("https://")) {
        start = 8;
    } else {
        return {};
    }
    std::string_view rest = url.substr(start);
    rest = rest.substr(0, rest.find_first_of("/?#"));
    if (const auto at = rest.rfind('@'); at != std::string_view::npos) {
        rest.remove_prefix(at + 1); // user-info
    }
    std::string host;
    if (rest.starts_with("[")) {
        host = std::string(rest.substr(1, rest.find(']') - 1));
    } else {
        host = std::string(rest.substr(0, rest.find(':')));
    }
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return std::tolower(c); });
    return host;
}

} // namespace

bool isLoopbackUrl(std::string_view url) {
    const std::string host = hostOf(url);
    if (host == "localhost" || host == "::1") {
        return true;
    }
    // 127.0.0.0/8, dotted quad only.
    if (!host.starts_with("127.")) {
        return false;
    }
    int dots = 0;
    for (char c : host) {
        if (c == '.') {
            ++dots;
        } else if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return dots == 3;
}

bool isCloud(const ProviderSpec& provider, std::string_view baseUrl) {
    const std::string_view url = baseUrl.empty() ? std::string_view(provider.baseUrl) : baseUrl;
    return !(provider.local && isLoopbackUrl(url));
}

ConsentPrompt makeConsentPrompt(const ProviderSpec& provider, std::string_view baseUrl, Capability capability,
                                std::string payload, CostEstimate cost) {
    ConsentPrompt prompt;
    prompt.provider = provider.id;
    prompt.providerName = provider.name;
    prompt.capability = capability;
    prompt.payload = std::move(payload);
    prompt.destination = hostOf(baseUrl.empty() ? std::string_view(provider.baseUrl) : baseUrl);
    prompt.termsUrl = provider.termsUrl;
    prompt.dataPolicy = provider.dataPolicy;
    prompt.cost = cost;
    return prompt;
}

void ConsentLedger::setOfflineMode(bool offline) {
    {
        std::lock_guard lock(m_mutex);
        if (m_offline == offline) {
            return;
        }
        m_offline = offline;
    }
    notifyChanged();
}

bool ConsentLedger::offlineMode() const {
    std::lock_guard lock(m_mutex);
    return m_offline;
}

void ConsentLedger::setProjectLock(bool locked) {
    std::lock_guard lock(m_mutex);
    m_projectLock = locked;
}

void ConsentLedger::setProjectLockSource(std::function<bool()> source) {
    std::lock_guard lock(m_mutex);
    m_projectLockSource = std::move(source);
}

void ConsentLedger::setProjectIdentitySource(std::function<std::uint64_t()> source) {
    std::lock_guard lock(m_mutex);
    m_projectIdentitySource = std::move(source);
}

std::uint64_t ConsentLedger::projectIdentity() const {
    std::lock_guard lock(m_mutex);
    return m_projectIdentitySource ? m_projectIdentitySource() : 0;
}

bool ConsentLedger::projectLockWhileHeld() const { return m_projectLock || (m_projectLockSource && m_projectLockSource()); }

bool ConsentLedger::projectLock() const {
    std::lock_guard lock(m_mutex);
    return projectLockWhileHeld();
}

bool ConsentLedger::cloudAllowed() const {
    std::lock_guard lock(m_mutex);
    return !m_offline && !projectLockWhileHeld();
}

ConsentState ConsentLedger::state(const ProviderSpec& provider, std::string_view baseUrl, Capability capability) const {
    if (!isCloud(provider, baseUrl)) {
        return ConsentState::NotNeeded;
    }
    std::lock_guard lock(m_mutex);
    if (m_offline || projectLockWhileHeld()) {
        return ConsentState::Blocked; // recorded consent never overrides a lock
    }
    const bool granted = std::any_of(m_records.begin(), m_records.end(), [&](const ConsentRecord& r) {
        return r.provider == provider.id && r.capability == capability;
    });
    return granted ? ConsentState::Granted : ConsentState::NeedsPrompt;
}

std::expected<void, AiError> ConsentLedger::authorize(const ProviderSpec& provider, std::string_view baseUrl,
                                                      Capability capability, bool allowOnce, std::string_view caller) const {
    ConsentState consent = state(provider, baseUrl, capability);
    // "Always allow" is the user's answer for the app's own features: an extension's call needs the user's answer to
    // its own prompt every time (review wave 5, S5-P2-9).
    if (consent == ConsentState::Granted && caller.starts_with("extension:")) {
        consent = ConsentState::NeedsPrompt;
    }
    switch (consent) {
    case ConsentState::NotNeeded:
    case ConsentState::Granted:
        return {};
    case ConsentState::Blocked:
        return std::unexpected(AiError(AiErrorCode::Offline, projectLock() ? "this project is locked to no-cloud AI"
                                                                           : "offline mode is on"));
    case ConsentState::NeedsPrompt:
        if (allowOnce) {
            return {};
        }
        return std::unexpected(AiError(AiErrorCode::ConsentRequired, "no consent for " + provider.name + " (" +
                                                                         std::string(toString(capability)) + ")"));
    }
    return {};
}

void ConsentLedger::grantAlways(const ProviderSpec& provider, Capability capability, std::string grantedAt) {
    {
        std::lock_guard lock(m_mutex);
        std::erase_if(m_records,
                      [&](const ConsentRecord& r) { return r.provider == provider.id && r.capability == capability; });
        m_records.push_back({provider.id, capability, std::move(grantedAt), provider.termsUrl});
    }
    notifyChanged();
}

void ConsentLedger::revoke(std::string_view provider, Capability capability) {
    {
        std::lock_guard lock(m_mutex);
        std::erase_if(m_records, [&](const ConsentRecord& r) { return r.provider == provider && r.capability == capability; });
    }
    notifyChanged();
}

void ConsentLedger::revokeAll() {
    {
        std::lock_guard lock(m_mutex);
        m_records.clear();
    }
    notifyChanged();
}

std::vector<ConsentRecord> ConsentLedger::records() const {
    std::lock_guard lock(m_mutex);
    return m_records;
}

Json ConsentLedger::toJson() const {
    std::lock_guard lock(m_mutex);
    Json records = Json::array();
    for (const auto& r : m_records) {
        records.push_back({{"provider", r.provider},
                           {"capability", toString(r.capability)},
                           {"grantedAt", r.grantedAt},
                           {"termsUrl", r.termsUrl}});
    }
    return {{"version", 1}, {"offline", m_offline}, {"records", records}};
}

std::expected<void, AiError> ConsentLedger::loadJson(const Json& json) {
    if (!json.is_object() || jsonIntOr(json, "version", 0) != 1) {
        return std::unexpected(AiError(AiErrorCode::InvalidRequest, "consent data: unsupported format"));
    }
    std::vector<ConsentRecord> records;
    if (auto it = json.find("records"); it != json.end() && it->is_array()) {
        for (const auto& r : *it) {
            const auto capability = capabilityFromString(jsonStringOr(r, "capability"));
            const std::string provider = jsonStringOr(r, "provider");
            if (!capability || provider.empty()) {
                continue; // unknown capability from a newer version: drop the record, ask again
            }
            records.push_back({provider, *capability, jsonStringOr(r, "grantedAt"), jsonStringOr(r, "termsUrl")});
        }
    }
    {
        std::lock_guard lock(m_mutex);
        m_offline = jsonBoolOr(json, "offline", false);
        m_records = std::move(records);
    }
    return {};
}

void ConsentLedger::setOnChanged(std::function<void()> callback) {
    std::lock_guard lock(m_mutex);
    m_onChanged = std::move(callback);
}

void ConsentLedger::notifyChanged() {
    std::function<void()> callback;
    {
        std::lock_guard lock(m_mutex);
        callback = m_onChanged;
    }
    if (callback) {
        callback();
    }
}

} // namespace wabcut::ai
