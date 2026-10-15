#pragma once

// API keys (docs/ai.md principle 3, docs/internal/ai-research.md B.5). Keys live in the OS secret store, one entry per
// provider, behind the SecretStore interface; the QtKeychain backend is in the app layer. Environment variables
// override stored keys (CI, wabcut-cli). Keys never reach project files, settings, logs or crash reports: Secret
// has no stream operator, masked() shows the last 4 characters only, and every key KeyRing loads is registered
// with the global Redactor.
//
// Without an OS backend there is no silent plaintext fallback: keys stay in memory for the session
// (MemorySecretStore) unless the user explicitly opts in to PlaintextFileSecretStore.

#include <wabcut/ai/AiError.h>
#include <wabcut/ai/Http.h>
#include <wabcut/ai/ModelRegistry.h>

#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wabcut::ai {

class Redactor;

/// A secret value. Move-only; the buffer is overwritten with zeros when the Secret is destroyed or reassigned
/// (best effort: copies the OS or a network stack made are outside our control).
class Secret {
public:
    Secret() = default;
    explicit Secret(std::string value) : m_value(std::move(value)) {}
    Secret(Secret&& other) noexcept;
    Secret& operator=(Secret&& other) noexcept;
    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;
    ~Secret();

    bool empty() const { return m_value.empty(); }
    /// The value, for building one request header. Do not store or log it.
    std::string_view reveal() const { return m_value; }
    /// The last 4 characters ("" for keys of 8 characters or fewer, which would reveal too much).
    std::string last4() const;
    /// "••••a1b2", for display.
    std::string masked() const;
    /// An explicit copy (e.g. to hand a key to an adapter on another thread).
    Secret clone() const { return Secret(m_value); }

private:
    void wipe() noexcept;
    std::string m_value;
};

/// Where a key came from.
enum class SecretSource { None, Environment, Store, Session, PlaintextFile };
std::string_view toString(SecretSource source);

/// Storage backend for API keys, keyed by provider id. Implementations: the app's QtKeychain store (Windows
/// Credential Manager / macOS Keychain / Secret Service), MemorySecretStore, PlaintextFileSecretStore.
/// Unless an implementation says otherwise, call it from one thread.
class SecretStore {
public:
    virtual ~SecretStore() = default;
    /// nullopt when there is no entry. Errors: Unauthorized (the user denied access), ProviderError (backend
    /// failure, e.g. no keyring service is running).
    virtual std::expected<std::optional<Secret>, AiError> read(std::string_view providerId) = 0;
    virtual std::expected<void, AiError> write(std::string_view providerId, const Secret& secret) = 0;
    /// Removing a missing entry succeeds.
    virtual std::expected<void, AiError> remove(std::string_view providerId) = 0;
    /// "Windows Credential Manager", "this session only", ...
    virtual std::string backendName() const = 0;
    /// False for stores that forget everything when the app exits.
    virtual bool persistent() const = 0;
};

/// Keys for this session only: the fallback when no OS secret store is available. Thread-safe.
class MemorySecretStore final : public SecretStore {
public:
    std::expected<std::optional<Secret>, AiError> read(std::string_view providerId) override;
    std::expected<void, AiError> write(std::string_view providerId, const Secret& secret) override;
    std::expected<void, AiError> remove(std::string_view providerId) override;
    std::string backendName() const override { return "this session only"; }
    bool persistent() const override { return false; }

private:
    std::mutex m_mutex;
    std::map<std::string, Secret, std::less<>> m_secrets;
};

/// Keys in a plaintext JSON file. ONLY for users who explicitly opt in after a warning (e.g. a headless Linux
/// machine without Secret Service). The file is created with owner-only permissions where the OS supports it.
class PlaintextFileSecretStore final : public SecretStore {
public:
    explicit PlaintextFileSecretStore(std::filesystem::path file);
    std::expected<std::optional<Secret>, AiError> read(std::string_view providerId) override;
    std::expected<void, AiError> write(std::string_view providerId, const Secret& secret) override;
    std::expected<void, AiError> remove(std::string_view providerId) override;
    std::string backendName() const override { return "plaintext file (opted in)"; }
    bool persistent() const override { return true; }

private:
    std::filesystem::path m_file;
};

class KeyRing;

/// Keys left in a plaintext key file by the plaintext opt-in that AI-10 removed (review wave 4, P2-5): how many
/// provider entries hold a key (0 when there is no such file or it cannot be read).
std::size_t legacyKeyCount(const std::filesystem::path& file);

struct LegacyKeyImport {
    std::vector<std::string> imported; ///< provider ids whose key moved into the key ring's store
    std::vector<std::string> unknown;  ///< entries of providers this build does not know (left out)
    bool fileRemoved = false;          ///< the file was overwritten and deleted
    std::string keptBecause;           ///< why the file stays although nothing failed ("" when it was removed)
    std::string error;                 ///< why something failed ("" when all went well)
};
/// Moves every key of the plaintext file into `keys` (its store: the OS keychain, or this session only), then removes
/// the file with secureDeleteFile() when every key it holds is safe elsewhere. The file stays - and says why - when a
/// key the store refuses, when the store keeps keys for this session only, or when it holds keys of providers this
/// build does not know (review wave 5, A5-P2-17: those would be lost for good); deletePlaintextKeys-style callers can
/// still remove it with secureDeleteFile() once the user decides.
LegacyKeyImport importLegacyKeys(const std::filesystem::path& file, KeyRing& keys, std::span<const ProviderSpec> providers);
/// Overwrites the file with zeros, flushes it and deletes it (best effort against casual recovery; SSDs and backups
/// may keep copies). False when it is still there afterwards.
bool secureDeleteFile(const std::filesystem::path& file);

/// Reads an environment variable; injectable for tests.
using EnvLookup = std::function<std::optional<std::string>(std::string_view name)>;
std::optional<std::string> systemEnv(std::string_view name);

/// What the UI may show about a provider's key: never the key itself.
struct KeyStatus {
    bool hasKey = false;
    SecretSource source = SecretSource::None;
    std::string masked; ///< "••••a1b2"
};

/// Resolves the key of a provider: the provider's environment variable first, then the store. Keys are cached in
/// memory after the first read, so job threads do not touch the store. get()/status() are thread-safe; save() and
/// remove() call the store and must run on the store's thread.
class KeyRing {
public:
    explicit KeyRing(SecretStore& store, EnvLookup env = systemEnv, Redactor* redactor = nullptr);

    /// Errors: NoCredentials, or the store's error.
    std::expected<Secret, AiError> get(const ProviderSpec& provider);
    KeyStatus status(const ProviderSpec& provider);
    /// Writes the key to the store (an environment override still wins while it is set).
    std::expected<void, AiError> save(const ProviderSpec& provider, Secret key);
    std::expected<void, AiError> remove(const ProviderSpec& provider);
    /// Forgets cached keys (e.g. after the store changed outside the app).
    void clearCache();

    SecretStore& store() { return m_store; }

private:
    std::optional<std::string> envValue(const ProviderSpec& provider) const;

    SecretStore& m_store;
    EnvLookup m_env;
    Redactor* m_redactor;
    std::mutex m_mutex;
    std::map<std::string, std::optional<std::string>, std::less<>> m_cache; // nullopt: no entry in the store
};

/// The URL for `path` below `baseUrl` (no doubled or missing slash). Full URLs in `path` are returned unchanged.
std::string joinUrl(std::string_view baseUrl, std::string_view path);

/// Checks a key with the provider's KeyCheck call (a cheap authenticated GET such as /v1/models). Success means
/// the provider accepted the key. Errors: Unauthorized (rejected), QuotaExceeded (valid but out of credit),
/// Network, RateLimited, ProviderError; InvalidRequest when the provider has no key check.
std::expected<void, AiError> validateKey(IHttpTransport& transport, const ProviderSpec& provider,
                                         std::string_view baseUrl, const Secret& key, std::stop_token stop);

} // namespace wabcut::ai
