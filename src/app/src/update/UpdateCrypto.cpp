#include "UpdateCrypto.h"

#include <QCryptographicHash>
#include <QDir>

#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#endif

namespace wabcut::app::update {

namespace {

// SEQUENCE { SEQUENCE { OID id-ecPublicKey, OID prime256v1 }, BIT STRING { 0x00, 0x04 (uncompressed) ... } }: the DER
// of every P-256 SubjectPublicKeyInfo up to the point's 64 bytes.
constexpr unsigned char kSpkiPrefix[] = {0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01, 0x06,
                                          0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04};
constexpr qsizetype kPrefixSize = static_cast<qsizetype>(sizeof(kSpkiPrefix));

#if defined(_WIN32)
QString trustError(LONG status) {
    switch (static_cast<HRESULT>(status)) {
    case TRUST_E_NOSIGNATURE:
    case TRUST_E_SUBJECT_FORM_UNKNOWN: // no signature format knows the file: it cannot carry one
        return QStringLiteral("the file is not signed");
    case TRUST_E_BAD_DIGEST:
        return QStringLiteral("the file was changed after it was signed");
    case TRUST_E_EXPLICIT_DISTRUST:
        return QStringLiteral("the signing certificate is blocked");
    case CERT_E_UNTRUSTEDROOT:
    case CERT_E_CHAINING:
        return QStringLiteral("the signing certificate is not trusted");
    case CERT_E_EXPIRED:
        return QStringLiteral("the signing certificate has expired");
    case TRUST_E_SUBJECT_NOT_TRUSTED:
        return QStringLiteral("the signature is not trusted");
    default:
        return QStringLiteral("the signature check failed (0x%1)").arg(static_cast<quint32>(status), 8, 16, QLatin1Char('0'));
    }
}
#endif

} // namespace

bool canVerifySignatures() {
#if defined(_WIN32)
    return true;
#else
    return false;
#endif
}

QByteArray p256PointFromSpki(const QByteArray& spki) {
    if (spki.size() != kPrefixSize + 64 ||
        std::memcmp(spki.constData(), kSpkiPrefix, static_cast<std::size_t>(kPrefixSize)) != 0) {
        return {};
    }
    return spki.mid(kPrefixSize);
}

QByteArray spkiFromP256Point(const QByteArray& point) {
    if (point.size() != 64) {
        return {};
    }
    return QByteArray(reinterpret_cast<const char*>(kSpkiPrefix), kPrefixSize) + point;
}

bool verifyP256Signature(const QByteArray& publicKeySpki, const QByteArray& message, const QByteArray& signature) {
#if defined(_WIN32)
    const QByteArray point = p256PointFromSpki(publicKeySpki);
    if (point.size() != 64 || signature.size() != 64) {
        return false;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0))) {
        return false;
    }
    QByteArray blob(static_cast<qsizetype>(sizeof(BCRYPT_ECCKEY_BLOB)) + 64, '\0');
    BCRYPT_ECCKEY_BLOB header{};
    header.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    header.cbKey = 32;
    std::memcpy(blob.data(), &header, sizeof(header));
    std::memcpy(blob.data() + sizeof(header), point.constData(), 64);
    BCRYPT_KEY_HANDLE key = nullptr;
    bool valid = false;
    if (BCRYPT_SUCCESS(BCryptImportKeyPair(algorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
                                           reinterpret_cast<PUCHAR>(blob.data()), static_cast<ULONG>(blob.size()), 0))) {
        QByteArray digest = QCryptographicHash::hash(message, QCryptographicHash::Sha256);
        QByteArray rs = signature;
        valid = BCRYPT_SUCCESS(BCryptVerifySignature(key, nullptr, reinterpret_cast<PUCHAR>(digest.data()),
                                                     static_cast<ULONG>(digest.size()), reinterpret_cast<PUCHAR>(rs.data()),
                                                     static_cast<ULONG>(rs.size()), 0));
        BCryptDestroyKey(key);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return valid;
#else
    (void)publicKeySpki;
    (void)message;
    (void)signature;
    return false;
#endif
}

AuthenticodeResult checkAuthenticode(const QString& path) {
    AuthenticodeResult result;
#if defined(_WIN32)
    const std::wstring file = QDir::toNativeSeparators(path).toStdWString();
    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = file.c_str();
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    // No online revocation check: the update works offline once downloaded, and the installer's bytes are already pinned
    // by the signed manifest's SHA-256. Authenticode adds who signed it.
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &fileInfo;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    const LONG status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    result.trusted = status == ERROR_SUCCESS;
    if (result.trusted) {
        CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(data.hWVTStateData);
        CRYPT_PROVIDER_SGNR* signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
        if (signer && signer->csCertChain > 0 && signer->pasCertChain && signer->pasCertChain[0].pCert) {
            wchar_t name[512] = {};
            CertGetNameStringW(signer->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name,
                               static_cast<DWORD>(std::size(name)));
            result.signer = QString::fromWCharArray(name);
        }
    } else {
        result.error = trustError(status);
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
#else
    (void)path;
    result.error = QStringLiteral("not supported here");
#endif
    return result;
}

} // namespace wabcut::app::update
