// The OS keychain for the Content Credentials signing key (AI-15): Windows Credential Manager, a generic credential
// "Wabcut/<entry>" persisted for this user on this computer (Windows encrypts it with the user's logon credentials).
// The app and wabcut-cli use the same entry, so both sign with the same identity.
#include <wabcut/ai/c2pa/ContentCredentials.h>

#include "Der.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>
#endif

namespace wabcut::ai::c2pa {

#ifdef _WIN32
namespace {

std::wstring targetName(std::string_view entry) {
    std::wstring out = L"Wabcut/";
    for (char c : entry) {
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    return out;
}

class CredentialManagerStore final : public SecretStore {
public:
    std::expected<std::optional<Secret>, AiError> read(std::string_view entry) override {
        PCREDENTIALW credential = nullptr;
        if (!CredReadW(targetName(entry).c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
            const DWORD error = GetLastError();
            if (error == ERROR_NOT_FOUND) {
                return std::optional<Secret>();
            }
            return std::unexpected(AiError(AiErrorCode::ProviderError, "Windows Credential Manager could not read the entry (error " +
                                                                           std::to_string(error) + ")"));
        }
        std::string value(reinterpret_cast<const char*>(credential->CredentialBlob), credential->CredentialBlobSize);
        SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
        CredFree(credential);
        return std::optional<Secret>(Secret(std::move(value)));
    }

    std::expected<void, AiError> write(std::string_view entry, const Secret& secret) override {
        const std::string_view value = secret.reveal();
        if (value.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
            return std::unexpected(AiError(AiErrorCode::InvalidRequest, "the key is too large for Windows Credential Manager (" +
                                                                            std::to_string(CRED_MAX_CREDENTIAL_BLOB_SIZE) + " bytes)"));
        }
        std::wstring target = targetName(entry);
        std::wstring user = L"Wabcut";
        CREDENTIALW credential{};
        credential.Type = CRED_TYPE_GENERIC;
        credential.TargetName = target.data();
        credential.CredentialBlobSize = static_cast<DWORD>(value.size());
        credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(value.data()));
        credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
        credential.UserName = user.data();
        if (!CredWriteW(&credential, 0)) {
            return std::unexpected(AiError(AiErrorCode::ProviderError, "Windows Credential Manager could not store the entry (error " +
                                                                           std::to_string(GetLastError()) + ")"));
        }
        return {};
    }

    std::expected<void, AiError> remove(std::string_view entry) override {
        if (!CredDeleteW(targetName(entry).c_str(), CRED_TYPE_GENERIC, 0) && GetLastError() != ERROR_NOT_FOUND) {
            return std::unexpected(AiError(AiErrorCode::ProviderError, "Windows Credential Manager could not remove the entry (error " +
                                                                           std::to_string(GetLastError()) + ")"));
        }
        return {};
    }

    std::string backendName() const override { return "Windows Credential Manager"; }
    bool persistent() const override { return true; }
};

} // namespace

std::unique_ptr<SecretStore> osKeychain() { return std::make_unique<CredentialManagerStore>(); }
#else
std::unique_ptr<SecretStore> osKeychain() { return nullptr; }
#endif

} // namespace wabcut::ai::c2pa
