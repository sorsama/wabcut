// The signing identity (AI-15): a local root and an ECDSA P-256 signing certificate made with Windows CNG, or the
// user's own PEM certificate and key. The C2PA certificate profile (c2pa-rs check_certificate_profile, C2PA 2.3
// 14.5.1) wants a v3 end-entity certificate with an allowed EKU, digitalSignature, an authority key identifier and an
// ECDSA-with-SHA256 signature from an issuer that is not itself: hence the root, whose private key is never kept.
#include <wabcut/ai/c2pa/ContentCredentials.h>

#include "Der.h"

#include <wabcut/ai/Sha256.h>
#include <wabcut/model/ProjectIO.h>

#include <array>
#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

namespace wabcut::ai::c2pa {

namespace {

using der::Bytes;

constexpr std::string_view kEcPublicKey = "1.2.840.10045.2.1";
constexpr std::string_view kP256 = "1.2.840.10045.3.1.7";
constexpr std::string_view kP384 = "1.3.132.0.34";
constexpr std::string_view kP521 = "1.3.132.0.35";
constexpr std::string_view kEcdsaSha256 = "1.2.840.10045.4.3.2";
constexpr std::string_view kRsa = "1.2.840.113549.1.1.1";
constexpr std::string_view kRsaPss = "1.2.840.113549.1.1.10";
constexpr std::string_view kEd25519 = "1.3.101.112";
constexpr std::string_view kCommonName = "2.5.4.3";
constexpr std::string_view kOrganization = "2.5.4.10";
constexpr std::string_view kBasicConstraints = "2.5.29.19";
constexpr std::string_view kKeyUsage = "2.5.29.15";
constexpr std::string_view kExtKeyUsage = "2.5.29.37";
constexpr std::string_view kSubjectKeyId = "2.5.29.14";
constexpr std::string_view kAuthorityKeyId = "2.5.29.35";
constexpr std::string_view kDocumentSigning = "1.3.6.1.5.5.7.3.36";
constexpr std::string_view kC2paClaimSigning = "1.3.6.1.4.1.62558.2.1";

constexpr std::string_view kOrganizationName = "Self-issued with Wabcut (not verified)";

AiError failure(AiErrorCode code, std::string message) { return AiError(code, std::move(message)); }

std::string isoTime(std::int64_t unixSeconds) {
    using namespace std::chrono;
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", sys_seconds{seconds{unixSeconds}});
}

[[maybe_unused]] Bytes name(std::string_view commonName) {
    return der::sequence({der::set({der::sequence({der::oid(kOrganization), der::utf8(kOrganizationName)})}),
                          der::set({der::sequence({der::oid(kCommonName), der::utf8(commonName)})})});
}

[[maybe_unused]] Bytes extension(std::string_view id, bool critical, const Bytes& value) {
    if (critical) {
        return der::sequence({der::oid(id), der::boolean(true), der::octetString(value)});
    }
    return der::sequence({der::oid(id), der::octetString(value)});
}

/// A key identifier: the first 20 bytes of the SHA-256 of the public key.
[[maybe_unused]] Bytes keyId(std::span<const std::uint8_t> publicPoint) {
    Sha256 hash;
    hash.update(std::span(reinterpret_cast<const std::byte*>(publicPoint.data()), publicPoint.size()));
    const auto digest = hash.finish();
    return Bytes(digest.begin(), digest.begin() + 20);
}

[[maybe_unused]] Bytes ecSpki(std::span<const std::uint8_t> point) {
    return der::sequence({der::sequence({der::oid(kEcPublicKey), der::oid(kP256)}), der::bitString(point)});
}

// --- Parsing ------------------------------------------------------------------------------------------------------

struct CertificateInfo {
    std::int64_t notBefore = 0;
    std::int64_t notAfter = 0;
    std::string commonName;
    Bytes publicKey; ///< the subject public key's bits (an EC point for EC keys)
};

std::optional<CertificateInfo> parseCertificate(std::span<const std::uint8_t> certificate) {
    const auto cert = der::read(certificate);
    if (!cert || cert->tag != der::tag::Sequence) {
        return std::nullopt;
    }
    const auto parts = der::children(cert->content);
    if (!parts || parts->size() != 3) {
        return std::nullopt;
    }
    const auto tbs = der::children((*parts)[0].content);
    if (!tbs || tbs->size() < 6) {
        return std::nullopt;
    }
    const std::size_t at = (*tbs)[0].tag == der::tag::context(0) ? 1 : 0;
    if (tbs->size() < at + 6) {
        return std::nullopt;
    }
    CertificateInfo info;
    const auto validity = der::children((*tbs)[at + 3].content);
    if (!validity || validity->size() != 2) {
        return std::nullopt;
    }
    const auto from = der::timeValue((*validity)[0]);
    const auto to = der::timeValue((*validity)[1]);
    if (!from || !to) {
        return std::nullopt;
    }
    info.notBefore = *from;
    info.notAfter = *to;
    if (const auto rdns = der::children((*tbs)[at + 4].content)) {
        for (const der::Element& rdn : *rdns) {
            const auto attributes = der::children(rdn.content);
            if (!attributes) {
                continue;
            }
            for (const der::Element& attribute : *attributes) {
                const auto pair = der::children(attribute.content);
                if (pair && pair->size() == 2 && der::oidText((*pair)[0].content) == kCommonName) {
                    info.commonName.assign((*pair)[1].content.begin(), (*pair)[1].content.end());
                }
            }
        }
    }
    const auto spki = der::children((*tbs)[at + 5].content);
    if (!spki || spki->size() != 2 || (*spki)[1].tag != der::tag::BitString || (*spki)[1].content.empty()) {
        return std::nullopt;
    }
    info.publicKey.assign((*spki)[1].content.begin() + 1, (*spki)[1].content.end());
    return info;
}

struct KeyInfo {
    std::string algorithm;
    Bytes publicKey; ///< EC keys: the point from the key's publicKey field (empty when the key does not carry it)
};

/// The algorithm of a PKCS#8 private key (and an EC key's public point). The caller wipes `pkcs8`.
std::optional<KeyInfo> parsePkcs8(std::span<const std::uint8_t> pkcs8) {
    const auto top = der::read(pkcs8);
    if (!top || top->tag != der::tag::Sequence) {
        return std::nullopt;
    }
    const auto parts = der::children(top->content);
    if (!parts || parts->size() < 3 || (*parts)[2].tag != der::tag::OctetString) {
        return std::nullopt;
    }
    const auto algorithm = der::children((*parts)[1].content);
    if (!algorithm || algorithm->empty()) {
        return std::nullopt;
    }
    const std::string id = der::oidText((*algorithm)[0].content);
    KeyInfo info;
    if (id == kEcPublicKey) {
        if (algorithm->size() < 2) {
            return std::nullopt;
        }
        const std::string curve = der::oidText((*algorithm)[1].content);
        info.algorithm = curve == kP256 ? "es256" : curve == kP384 ? "es384" : curve == kP521 ? "es512" : "";
        if (info.algorithm.empty()) {
            return std::nullopt;
        }
        if (const auto ecKey = der::read((*parts)[2].content)) {
            if (const auto fields = der::children(ecKey->content)) {
                for (const der::Element& f : *fields) {
                    if (f.tag == der::tag::context(1)) {
                        if (const auto bits = der::read(f.content); bits && bits->tag == der::tag::BitString && !bits->content.empty()) {
                            info.publicKey.assign(bits->content.begin() + 1, bits->content.end());
                        }
                    }
                }
            }
        }
    } else if (id == kRsa || id == kRsaPss) {
        info.algorithm = "ps256"; // C2PA signs with RSA-PSS only
    } else if (id == kEd25519) {
        info.algorithm = "ed25519";
    } else {
        return std::nullopt;
    }
    return info;
}

std::optional<std::string> readText(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

std::int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// --- CNG ------------------------------------------------------------------------------------------------------------

#ifdef _WIN32
class EcKey {
public:
    EcKey() = default;
    EcKey(const EcKey&) = delete;
    EcKey& operator=(const EcKey&) = delete;
    ~EcKey() {
        if (m_key) {
            BCryptDestroyKey(m_key);
        }
    }

    static std::expected<EcKey, AiError> generate() {
        EcKey k;
        BCRYPT_ALG_HANDLE alg = nullptr;
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0))) {
            return std::unexpected(failure(AiErrorCode::Unsupported, "ECDSA P-256 is not available (CNG)"));
        }
        const bool made = BCRYPT_SUCCESS(BCryptGenerateKeyPair(alg, &k.m_key, 256, 0)) && BCRYPT_SUCCESS(BCryptFinalizeKeyPair(k.m_key, 0));
        BCryptCloseAlgorithmProvider(alg, 0);
        if (!made) {
            return std::unexpected(failure(AiErrorCode::ProviderError, "could not generate a signing key (CNG)"));
        }
        auto blob = k.exportBlob(BCRYPT_ECCPUBLIC_BLOB);
        if (!blob || blob->size() != sizeof(BCRYPT_ECCKEY_BLOB) + 64) {
            return std::unexpected(failure(AiErrorCode::ProviderError, "could not read the public key (CNG)"));
        }
        k.m_point.push_back(0x04);
        k.m_point.insert(k.m_point.end(), blob->begin() + sizeof(BCRYPT_ECCKEY_BLOB), blob->end());
        return k;
    }

    EcKey(EcKey&& other) noexcept : m_key(std::exchange(other.m_key, nullptr)), m_point(std::move(other.m_point)) {}

    /// 0x04 || X || Y
    const Bytes& point() const { return m_point; }

    /// The DER ECDSA-Sig-Value of SHA-256(message).
    std::expected<Bytes, AiError> sign(std::span<const std::uint8_t> message) const {
        Sha256 hash;
        hash.update(std::span(reinterpret_cast<const std::byte*>(message.data()), message.size()));
        auto digest = hash.finish();
        std::array<std::uint8_t, 64> signature{};
        ULONG written = 0;
        if (!BCRYPT_SUCCESS(BCryptSignHash(m_key, nullptr, digest.data(), static_cast<ULONG>(digest.size()), signature.data(),
                                           static_cast<ULONG>(signature.size()), &written, 0)) ||
            written != 64) {
            return std::unexpected(failure(AiErrorCode::ProviderError, "could not sign the certificate (CNG)"));
        }
        return der::sequence({der::integer(std::span(signature.data(), 32)), der::integer(std::span(signature.data() + 32, 32))});
    }

    /// The private key as PKCS#8 PEM; every intermediate buffer is wiped.
    std::expected<Secret, AiError> pkcs8Pem() const {
        auto blob = exportBlob(BCRYPT_ECCPRIVATE_BLOB);
        if (!blob || blob->size() != sizeof(BCRYPT_ECCKEY_BLOB) + 96) {
            if (blob) {
                der::wipe(*blob);
            }
            return std::unexpected(failure(AiErrorCode::ProviderError, "could not export the signing key (CNG)"));
        }
        const std::span<const std::uint8_t> d(blob->data() + sizeof(BCRYPT_ECCKEY_BLOB) + 64, 32);
        Bytes ecPrivateKey = der::sequence({der::integer(1), der::octetString(d), der::explicitTag(1, der::bitString(m_point))});
        Bytes pkcs8 = der::sequence({der::integer(0), der::sequence({der::oid(kEcPublicKey), der::oid(kP256)}), der::octetString(ecPrivateKey)});
        Secret pem(der::pem("PRIVATE KEY", pkcs8));
        der::wipe(*blob);
        der::wipe(ecPrivateKey);
        der::wipe(pkcs8);
        return pem;
    }

private:
    std::optional<Bytes> exportBlob(LPCWSTR type) const {
        ULONG size = 0;
        if (!BCRYPT_SUCCESS(BCryptExportKey(m_key, nullptr, type, nullptr, 0, &size, 0))) {
            return std::nullopt;
        }
        Bytes out(size);
        if (!BCRYPT_SUCCESS(BCryptExportKey(m_key, nullptr, type, out.data(), size, &size, 0))) {
            der::wipe(out);
            return std::nullopt;
        }
        out.resize(size);
        return out;
    }

    BCRYPT_KEY_HANDLE m_key = nullptr;
    Bytes m_point;
};

Bytes randomSerial() {
    Bytes serial(16);
    BCryptGenRandom(nullptr, serial.data(), static_cast<ULONG>(serial.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    serial[0] = static_cast<std::uint8_t>((serial[0] & 0x7F) | 0x40); // positive, 16 bytes long
    return serial;
}

std::expected<Bytes, AiError> issue(const EcKey& issuerKey, const Bytes& issuerName, const Bytes& issuerKeyId, const Bytes& subjectName,
                                    const Bytes& subjectPoint, std::int64_t notBefore, std::int64_t notAfter, bool ca) {
    std::vector<Bytes> extensions;
    if (ca) {
        extensions.push_back(extension(kBasicConstraints, true, der::sequence({der::boolean(true), der::integer(0)})));
        const std::uint8_t usage = 0x06; // keyCertSign (bit 5) and cRLSign (bit 6)
        extensions.push_back(extension(kKeyUsage, true, der::bitString(std::span(&usage, 1), 1)));
    } else {
        extensions.push_back(extension(kBasicConstraints, true, der::sequence(std::vector<Bytes>{})));
        const std::uint8_t usage = 0x80; // digitalSignature (bit 0)
        extensions.push_back(extension(kKeyUsage, true, der::bitString(std::span(&usage, 1), 7)));
        extensions.push_back(extension(kExtKeyUsage, false, der::sequence({der::oid(kDocumentSigning), der::oid(kC2paClaimSigning)})));
    }
    extensions.push_back(extension(kSubjectKeyId, false, der::octetString(keyId(subjectPoint))));
    extensions.push_back(extension(kAuthorityKeyId, false, der::sequence({der::tlv(der::tag::context(0, false), issuerKeyId)})));
    const Bytes algorithm = der::sequence({der::oid(kEcdsaSha256)});
    const Bytes tbs = der::sequence({der::explicitTag(0, der::integer(2)), der::integer(randomSerial()), algorithm, issuerName,
                                     der::sequence({der::time(notBefore), der::time(notAfter)}), subjectName, ecSpki(subjectPoint),
                                     der::explicitTag(3, der::sequence(extensions))});
    auto signature = issuerKey.sign(tbs);
    if (!signature) {
        return std::unexpected(signature.error());
    }
    return der::sequence({tbs, algorithm, der::bitString(*signature)});
}
#endif

} // namespace

std::expected<SigningIdentity, AiError> generateIdentity(std::chrono::system_clock::time_point now, std::chrono::days validity) {
#ifdef _WIN32
    auto root = EcKey::generate();
    if (!root) {
        return std::unexpected(root.error());
    }
    auto leaf = EcKey::generate();
    if (!leaf) {
        return std::unexpected(leaf.error());
    }
    const std::int64_t start = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count() - 3600;
    const std::int64_t end = start + 3600 + std::chrono::duration_cast<std::chrono::seconds>(validity).count();
    const Bytes rootId = keyId(root->point());
    std::string tag;
    for (std::size_t i = 0; i < 4; ++i) {
        tag += std::format("{:02x}", rootId[i]);
    }
    const Bytes rootName = name("Wabcut local root " + tag);
    const std::string commonName = "Wabcut user " + tag + " (self-issued)";
    auto rootCert = issue(*root, rootName, rootId, rootName, root->point(), start, end, true);
    if (!rootCert) {
        return std::unexpected(rootCert.error());
    }
    auto leafCert = issue(*root, rootName, rootId, name(commonName), leaf->point(), start, end, false);
    if (!leafCert) {
        return std::unexpected(leafCert.error());
    }
    auto key = leaf->pkcs8Pem();
    if (!key) {
        return std::unexpected(key.error());
    }
    // The root's key goes out of scope here: nothing can issue another certificate in this identity's name.
    SigningIdentity identity;
    identity.certificateChainPem = der::pem("CERTIFICATE", *leafCert);
    identity.rootPem = der::pem("CERTIFICATE", *rootCert);
    identity.privateKeyPem = std::move(*key);
    identity.algorithm = "es256";
    identity.subject = "CN=" + commonName + ", O=" + std::string(kOrganizationName);
    identity.notAfter = isoTime(end);
    identity.local = true;
    return identity;
#else
    (void)now;
    (void)validity;
    return std::unexpected(failure(AiErrorCode::Unsupported, "making a local signing identity needs Windows (CNG) in this build"));
#endif
}

std::expected<SigningIdentity, AiError> identityFromPem(std::string certificateChainPem, Secret privateKeyPem) {
    const auto certificates = der::pemBlocks(certificateChainPem, "CERTIFICATE");
    if (certificates.empty()) {
        return std::unexpected(failure(AiErrorCode::InvalidRequest, "the certificate file has no PEM certificate"));
    }
    const auto cert = parseCertificate(certificates.front());
    if (!cert) {
        return std::unexpected(failure(AiErrorCode::InvalidRequest, "the signing certificate could not be read"));
    }
    auto keys = der::pemBlocks(privateKeyPem.reveal(), "PRIVATE KEY");
    if (keys.empty()) {
        return std::unexpected(failure(AiErrorCode::InvalidRequest,
                                       "the private key is not a PKCS#8 PEM key (\"BEGIN PRIVATE KEY\"; convert it with openssl pkcs8 -topk8 -nocrypt)"));
    }
    const auto key = parsePkcs8(keys.front());
    for (Bytes& k : keys) {
        der::wipe(k);
    }
    if (!key) {
        return std::unexpected(failure(AiErrorCode::InvalidRequest, "unsupported private key (EC P-256/384/521, RSA or Ed25519)"));
    }
    if (!key->publicKey.empty() && key->publicKey != cert->publicKey) {
        return std::unexpected(failure(AiErrorCode::InvalidRequest, "the private key does not belong to the signing certificate"));
    }
    if (cert->notAfter <= nowSeconds()) {
        return std::unexpected(failure(AiErrorCode::InvalidRequest, "the signing certificate expired on " + isoTime(cert->notAfter)));
    }
    SigningIdentity identity;
    identity.certificateChainPem = std::move(certificateChainPem);
    identity.privateKeyPem = std::move(privateKeyPem);
    identity.algorithm = key->algorithm;
    identity.subject = "CN=" + cert->commonName;
    identity.notAfter = isoTime(cert->notAfter);
    identity.local = false;
    return identity;
}

std::filesystem::path defaultIdentityDir() {
#ifdef _WIN32
    // Wide environment strings: a user name outside the ANSI code page must not break the path.
    const auto env = [](const wchar_t* name) -> std::optional<std::filesystem::path> {
        wchar_t* value = nullptr;
        std::size_t size = 0;
        if (_wdupenv_s(&value, &size, name) != 0 || value == nullptr) {
            return std::nullopt;
        }
        std::filesystem::path out(value);
        std::free(value);
        if (out.empty()) {
            return std::nullopt;
        }
        return out;
    };
    if (auto dir = env(L"WABCUT_C2PA_DIR")) {
        return *dir;
    }
    if (auto appData = env(L"APPDATA")) {
        return *appData / L"Wabcut" / L"content-credentials";
    }
#else
    if (auto dir = systemEnv("WABCUT_C2PA_DIR")) {
        return *dir;
    }
    if (auto config = systemEnv("XDG_CONFIG_HOME")) {
        return std::filesystem::path(*config) / "wabcut" / "content-credentials";
    }
    if (auto home = systemEnv("HOME")) {
        return std::filesystem::path(*home) / ".config" / "wabcut" / "content-credentials";
    }
#endif
    return std::filesystem::temp_directory_path() / "wabcut-content-credentials";
}

std::expected<SigningIdentity, AiError> localIdentity(SecretStore& keys, const std::filesystem::path& dir, bool create) {
    const auto chainFile = dir / "signing-certificate.pem";
    const auto rootFile = dir / "root-certificate.pem";
    auto stored = keys.read(kKeychainEntry);
    if (!stored) {
        return std::unexpected(stored.error());
    }
    if (*stored && !(*stored)->empty()) {
        const auto chain = readText(chainFile);
        const auto root = readText(rootFile);
        if (chain && root) {
            auto identity = identityFromPem(*chain, std::move(**stored));
            // Still valid for a day, and the key in the keychain belongs to the certificate on disk.
            const auto rootCerts = der::pemBlocks(*root, "CERTIFICATE");
            if (identity && !rootCerts.empty()) {
                const auto leaf = parseCertificate(der::pemBlocks(*chain, "CERTIFICATE").front());
                if (leaf && leaf->notAfter > nowSeconds() + 86400) {
                    identity->rootPem = *root;
                    identity->local = true;
                    identity->subject += ", O=" + std::string(kOrganizationName);
                    return identity;
                }
            }
        }
    }
    if (!create) {
        return std::unexpected(failure(AiErrorCode::NoCredentials, "no local Content Credentials identity yet"));
    }
    auto made = generateIdentity();
    if (!made) {
        return std::unexpected(made.error());
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (auto ok = io::writeFileAtomically(chainFile, made->certificateChainPem); !ok) {
        return std::unexpected(failure(AiErrorCode::ProviderError, "could not save the signing certificate: " + ok.error().message));
    }
    if (auto ok = io::writeFileAtomically(rootFile, made->rootPem); !ok) {
        return std::unexpected(failure(AiErrorCode::ProviderError, "could not save the root certificate: " + ok.error().message));
    }
    if (auto ok = keys.write(kKeychainEntry, made->privateKeyPem); !ok) {
        std::filesystem::remove(chainFile, ec);
        std::filesystem::remove(rootFile, ec);
        return std::unexpected(failure(ok.error().code, "could not keep the signing key in " + keys.backendName() + ": " + ok.error().message));
    }
    return made;
}

std::expected<void, AiError> resetLocalIdentity(SecretStore& keys, const std::filesystem::path& dir) {
    if (auto ok = keys.remove(kKeychainEntry); !ok) {
        return std::unexpected(ok.error());
    }
    std::error_code ec;
    std::filesystem::remove(dir / "signing-certificate.pem", ec);
    std::filesystem::remove(dir / "root-certificate.pem", ec);
    return {};
}

} // namespace wabcut::ai::c2pa
