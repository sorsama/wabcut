#include <wabcut/ai/Redact.h>
#include <wabcut/ai/Secrets.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

namespace wabcut::ai {

// --- Secret -----------------------------------------------------------------------------------------------------

Secret::Secret(Secret&& other) noexcept : m_value(std::move(other.m_value)) { other.wipe(); }

Secret& Secret::operator=(Secret&& other) noexcept {
    if (this != &other) {
        wipe();
        m_value = std::move(other.m_value);
        other.wipe();
    }
    return *this;
}

Secret::~Secret() { wipe(); }

void Secret::wipe() noexcept {
    // volatile writes so the compiler cannot drop the zeroing of a buffer that is about to be freed.
    volatile char* p = m_value.data();
    for (std::size_t i = 0; i < m_value.size(); ++i) {
        p[i] = '\0';
    }
    m_value.clear();
}

std::string Secret::last4() const { return m_value.size() > 8 ? m_value.substr(m_value.size() - 4) : std::string(); }

std::string Secret::masked() const {
    if (m_value.empty()) {
        return {};
    }
    return "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2" + last4(); // "••••" + last 4
}

std::string_view toString(SecretSource source) {
    switch (source) {
    case SecretSource::None:
        return "none";
    case SecretSource::Environment:
        return "environment";
    case SecretSource::Store:
        return "store";
    case SecretSource::Session:
        return "session";
    case SecretSource::PlaintextFile:
        return "plaintext";
    }
    return "none";
}

// --- MemorySecretStore ------------------------------------------------------------------------------------------

std::expected<std::optional<Secret>, AiError> MemorySecretStore::read(std::string_view providerId) {
    std::lock_guard lock(m_mutex);
    auto it = m_secrets.find(providerId);
    if (it == m_secrets.end()) {
        return std::optional<Secret>();
    }
    return std::optional<Secret>(it->second.clone());
}

std::expected<void, AiError> MemorySecretStore::write(std::string_view providerId, const Secret& secret) {
    std::lock_guard lock(m_mutex);
    m_secrets.insert_or_assign(std::string(providerId), secret.clone());
    return {};
}

std::expected<void, AiError> MemorySecretStore::remove(std::string_view providerId) {
    std::lock_guard lock(m_mutex);
    if (auto it = m_secrets.find(providerId); it != m_secrets.end()) {
        m_secrets.erase(it);
    }
    return {};
}

// --- PlaintextFileSecretStore -----------------------------------------------------------------------------------

namespace {

std::expected<Json, AiError> readKeyFile(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        return Json::object();
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::unexpected(AiError(AiErrorCode::ProviderError, "cannot read the key file"));
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    auto j = Json::parse(buffer.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return std::unexpected(AiError(AiErrorCode::ProviderError, "the key file is corrupt"));
    }
    return j;
}

std::expected<void, AiError> writeKeyFile(const std::filesystem::path& file, const Json& j) {
    std::error_code ec;
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), ec);
    }
    const auto temp = std::filesystem::path(file).concat(".tmp");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return std::unexpected(AiError(AiErrorCode::ProviderError, "cannot write the key file"));
        }
        out << j.dump(2);
    }
    std::filesystem::permissions(temp, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec); // best effort (no-op on Windows ACLs)
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        return std::unexpected(AiError(AiErrorCode::ProviderError, "cannot replace the key file"));
    }
    return {};
}

} // namespace

PlaintextFileSecretStore::PlaintextFileSecretStore(std::filesystem::path file) : m_file(std::move(file)) {}

std::expected<std::optional<Secret>, AiError> PlaintextFileSecretStore::read(std::string_view providerId) {
    auto j = readKeyFile(m_file);
    if (!j) {
        return std::unexpected(j.error());
    }
    auto it = j->find(std::string(providerId));
    if (it == j->end() || !it->is_string()) {
        return std::optional<Secret>();
    }
    return std::optional<Secret>(Secret(it->get<std::string>()));
}

std::expected<void, AiError> PlaintextFileSecretStore::write(std::string_view providerId, const Secret& secret) {
    auto j = readKeyFile(m_file);
    if (!j) {
        return std::unexpected(j.error());
    }
    (*j)[std::string(providerId)] = std::string(secret.reveal());
    return writeKeyFile(m_file, *j);
}

std::expected<void, AiError> PlaintextFileSecretStore::remove(std::string_view providerId) {
    auto j = readKeyFile(m_file);
    if (!j) {
        return std::unexpected(j.error());
    }
    if (j->erase(std::string(providerId)) == 0) {
        return {};
    }
    return writeKeyFile(m_file, *j);
}

// --- Legacy plaintext keys (review wave 4, P2-5) ------------------------------------------------------------

std::size_t legacyKeyCount(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        return 0;
    }
    auto j = readKeyFile(file);
    if (!j || !j->is_object()) {
        return 0;
    }
    return static_cast<std::size_t>(std::count_if(j->begin(), j->end(), [](const Json& v) { return v.is_string() && !v.get<std::string>().empty(); }));
}

bool secureDeleteFile(const std::filesystem::path& file) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (!ec) {
        std::fstream out(file, std::ios::binary | std::ios::in | std::ios::out);
        if (out) {
            const std::string zeros(4096, '\0');
            for (std::uintmax_t written = 0; written < size;) {
                const auto chunk = static_cast<std::streamsize>(std::min<std::uintmax_t>(zeros.size(), size - written));
                out.write(zeros.data(), chunk);
                written += static_cast<std::uintmax_t>(chunk);
            }
            out.flush();
        }
    }
    std::filesystem::remove(file, ec);
    return !std::filesystem::exists(file, ec);
}

LegacyKeyImport importLegacyKeys(const std::filesystem::path& file, KeyRing& keys, std::span<const ProviderSpec> providers) {
    LegacyKeyImport out;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        return out;
    }
    auto j = readKeyFile(file);
    if (!j || !j->is_object()) {
        out.error = "the old key file cannot be read";
        return out;
    }
    bool failed = false;
    for (auto it = j->begin(); it != j->end(); ++it) {
        if (!it->is_string() || it->get<std::string>().empty()) {
            continue;
        }
        const auto spec = std::ranges::find_if(providers, [&](const ProviderSpec& p) { return p.id == it.key(); });
        if (spec == providers.end()) {
            out.unknown.push_back(it.key());
            continue;
        }
        if (auto saved = keys.save(*spec, Secret(it->get<std::string>())); !saved) {
            failed = true;
            out.error = "the key of " + spec->id + " could not be stored: " + saved.error().message;
            continue;
        }
        out.imported.push_back(spec->id);
    }
    if (failed) {
        return out;
    }
    // Only when every key in it is safe elsewhere (review wave 5, A5-P2-17).
    if (!keys.store().persistent()) {
        out.keptBecause = "the keys are kept for this session only (no keychain is available), so the file still holds them for the next start";
        return out;
    }
    if (!out.unknown.empty()) {
        std::string names;
        for (const std::string& id : out.unknown) {
            names += (names.empty() ? "" : ", ") + id;
        }
        out.keptBecause = "it also holds keys of providers this version does not know (" + names + ")";
        return out;
    }
    out.fileRemoved = secureDeleteFile(file);
    if (!out.fileRemoved) {
        out.error = "the old key file could not be deleted";
    }
    return out;
}

// --- Environment ------------------------------------------------------------------------------------------------

std::optional<std::string> systemEnv(std::string_view name) {
    const std::string key(name);
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, key.c_str()) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string out(value);
    std::free(value);
#else
    const char* value = std::getenv(key.c_str());
    if (value == nullptr) {
        return std::nullopt;
    }
    std::string out(value);
#endif
    if (out.empty()) {
        return std::nullopt;
    }
    return out;
}

// --- KeyRing ----------------------------------------------------------------------------------------------------

KeyRing::KeyRing(SecretStore& store, EnvLookup env, Redactor* redactor)
    : m_store(store), m_env(std::move(env)), m_redactor(redactor ? redactor : &Redactor::global()) {}

std::optional<std::string> KeyRing::envValue(const ProviderSpec& provider) const {
    if (provider.envVar.empty() || !m_env) {
        return std::nullopt;
    }
    return m_env(provider.envVar);
}

std::expected<Secret, AiError> KeyRing::get(const ProviderSpec& provider) {
    if (auto env = envValue(provider)) {
        m_redactor->addSecret(*env);
        return Secret(std::move(*env));
    }
    {
        std::lock_guard lock(m_mutex);
        if (auto it = m_cache.find(provider.id); it != m_cache.end()) {
            if (!it->second) {
                return std::unexpected(AiError(AiErrorCode::NoCredentials, "no key for " + provider.name));
            }
            return Secret(*it->second);
        }
    }
    auto stored = m_store.read(provider.id);
    if (!stored) {
        return std::unexpected(stored.error());
    }
    std::lock_guard lock(m_mutex);
    if (!*stored) {
        m_cache[provider.id] = std::nullopt;
        return std::unexpected(AiError(AiErrorCode::NoCredentials, "no key for " + provider.name));
    }
    std::string value((*stored)->reveal());
    m_redactor->addSecret(value);
    m_cache[provider.id] = value;
    return Secret(std::move(value));
}

KeyStatus KeyRing::status(const ProviderSpec& provider) {
    KeyStatus status;
    if (auto env = envValue(provider)) {
        status.hasKey = true;
        status.source = SecretSource::Environment;
        status.masked = Secret(std::move(*env)).masked();
        return status;
    }
    auto key = get(provider);
    if (key) {
        status.hasKey = true;
        status.source = m_store.persistent() ? SecretSource::Store : SecretSource::Session;
        status.masked = key->masked();
    }
    return status;
}

std::expected<void, AiError> KeyRing::save(const ProviderSpec& provider, Secret key) {
    if (key.empty()) {
        return std::unexpected(AiError(AiErrorCode::InvalidRequest, "empty key"));
    }
    if (auto r = m_store.write(provider.id, key); !r) {
        return r;
    }
    m_redactor->addSecret(key.reveal());
    std::lock_guard lock(m_mutex);
    m_cache[provider.id] = std::string(key.reveal());
    return {};
}

std::expected<void, AiError> KeyRing::remove(const ProviderSpec& provider) {
    if (auto r = m_store.remove(provider.id); !r) {
        return r;
    }
    std::lock_guard lock(m_mutex);
    if (auto it = m_cache.find(provider.id); it != m_cache.end() && it->second) {
        m_redactor->removeSecret(*it->second);
    }
    m_cache[provider.id] = std::nullopt;
    return {};
}

void KeyRing::clearCache() {
    std::lock_guard lock(m_mutex);
    m_cache.clear();
}

// --- Key validation ---------------------------------------------------------------------------------------------

std::string joinUrl(std::string_view baseUrl, std::string_view path) {
    if (path.starts_with("http://") || path.starts_with("https://")) {
        return std::string(path);
    }
    std::string out(baseUrl);
    while (!out.empty() && out.back() == '/') {
        out.pop_back();
    }
    if (!path.empty() && path.front() != '/' && path.front() != '?') {
        out.push_back('/');
    }
    out.append(path);
    return out;
}

std::expected<void, AiError> validateKey(IHttpTransport& transport, const ProviderSpec& provider,
                                         std::string_view baseUrl, const Secret& key, std::stop_token stop) {
    if (provider.keyCheck.path.empty()) {
        return std::unexpected(AiError(AiErrorCode::InvalidRequest, provider.name + " has no key check"));
    }
    if (provider.keyRequired && key.empty()) {
        return std::unexpected(AiError(AiErrorCode::NoCredentials, "no key to check"));
    }
    HttpRequest request;
    request.method = provider.keyCheck.method.empty() ? "GET" : provider.keyCheck.method;
    request.url = joinUrl(baseUrl.empty() ? std::string_view(provider.baseUrl) : baseUrl, provider.keyCheck.path);
    request.timeout = std::chrono::seconds(20);
    request.idleTimeout = std::chrono::seconds(20);
    applyAuth(request.headers, provider.auth, key.reveal());
    for (const auto& h : provider.extraHeaders) { // e.g. anthropic-version
        request.headers.push_back(h);
    }
    auto response = transport.send(request, {}, stop);
    if (!response) {
        return std::unexpected(response.error());
    }
    if (!response->ok()) {
        return std::unexpected(classifyHttpError(response->status, response->headers, response->body));
    }
    return {};
}

} // namespace wabcut::ai
