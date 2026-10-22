#pragma once

// Signed C2PA Content Credentials for exports (AI-15, docs/ai.md "Content Credentials").
//
//   manifestDefinition()  what the export says about itself: created / edited with Wabcut <version>, the ingredients
//                         (every source file in the exported range, with its SHA-256; AI-generated sources marked as
//                         trainedAlgorithmicMedia), the AI-generated segments (gen.extend / broll / image, dub and
//                         narration voices: provider, model and a prompt HASH, never the prompt) and the redactions
//                         applied (style, time ranges and the region's bounds; never a picture of a source). 0.2
//                         (Premiere's Export Settings > Content Credentials): the producer's name (schema.org
//                         CreativeWork author), a "do not train" request (CAWG training and data mining), and a
//                         thumbnail of the export itself (its own picture, redactions burned in) when asked.
//   localIdentity()       the signer: a per-user identity made on first use - a local root certificate and an ECDSA
//                         P-256 signing certificate it issues (C2PA does not accept a self-signed signing
//                         certificate). The root's private key is discarded at once; the signing key goes to the OS
//                         keychain (Windows Credential Manager) and is never written to a file or logged. A
//                         certificate from a real CA can be used instead (identityFromPem()).
//   signFile()            signs a finished MP4 / MOV in place: a signed copy next to it is verified, then replaces it.
//                         On any failure the file is untouched. No network: no timestamp authority, no OCSP, no remote
//                         manifests (every HTTP request of the SDK is refused).
//   verifyFile()          reads and validates the manifest with the same library: signature, asset hash (any change
//                         to the file fails it), certificate profile, and trust in the anchors given.
//
// The local identity proves that the file is unchanged since this computer signed it and what it says about AI and
// redactions; it does not prove WHO signed it to anyone else (viewers show the signer as unknown / untrusted) until the
// user signs with a certificate from a CA on the C2PA trust list.

#include <wabcut/ai/AiError.h>
#include <wabcut/ai/Chat.h>
#include <wabcut/ai/Secrets.h>
#include <wabcut/model/ProjectState.h>

#include <atomic>
#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace wabcut::ai::c2pa {

/// The c2pa library is in this build and loads ("0.91.2"); empty when signing is unavailable here.
std::string sdkVersion();
bool available();

// --- Identity ----------------------------------------------------------------------------------------------------

struct SigningIdentity {
    std::string certificateChainPem; ///< the signing certificate first, then any intermediates (not the root)
    Secret privateKeyPem;            ///< PKCS#8 "BEGIN PRIVATE KEY"; only ever in memory and the OS keychain
    std::string algorithm = "es256"; ///< es256, es384, es512, ps256, ps384, ps512 or ed25519
    std::string rootPem;             ///< the local identity's root (the trust anchor that verifies it); "" if unknown
    std::string subject;             ///< the signing certificate's subject, for display
    std::string notAfter;            ///< ISO-8601 UTC
    bool local = true;               ///< self-issued here (not a CA the world trusts)
};

/// Makes a new local identity (Windows CNG): a root valid for `validity` and a signing certificate it issues (EKU
/// documentSigning and C2PA claim signing, digitalSignature, not a CA). The root's private key is dropped.
/// Unsupported on platforms without CNG.
std::expected<SigningIdentity, AiError> generateIdentity(std::chrono::system_clock::time_point now = std::chrono::system_clock::now(),
                                                         std::chrono::days validity = std::chrono::days(2 * 366));

/// Where the local identity's certificates live (public data): %APPDATA%\Wabcut\content-credentials, or
/// WABCUT_C2PA_DIR. The private key is in the keychain under kKeychainEntry.
std::filesystem::path defaultIdentityDir();
inline constexpr std::string_view kKeychainEntry = "content-credentials-signing-key";

/// The per-user identity: certificates from `dir` and the key from `keys`; with `create`, a new one is made (and
/// stored) when there is none, it expired, or the key no longer matches the certificate.
std::expected<SigningIdentity, AiError> localIdentity(SecretStore& keys, const std::filesystem::path& dir, bool create = true);
/// Forgets the local identity (the keychain entry and the certificate files): the next export makes a new one.
std::expected<void, AiError> resetLocalIdentity(SecretStore& keys, const std::filesystem::path& dir);

/// A user-provided identity: a PEM certificate chain (signing certificate first) and a PEM private key, e.g. from a CA
/// on the C2PA trust list. The algorithm is read from the key ("es256" for P-256, ...).
std::expected<SigningIdentity, AiError> identityFromPem(std::string certificateChainPem, Secret privateKeyPem);

/// The OS keychain for the signing key: Windows Credential Manager (target "Wabcut/<entry>", local machine
/// persistence, protected by Windows for the user). Elsewhere: nullptr.
std::unique_ptr<SecretStore> osKeychain();

// --- Manifest ----------------------------------------------------------------------------------------------------

struct ManifestOptions {
    std::string title;               ///< the output file name
    std::string format = "video/mp4";///< "video/mp4" or "video/quicktime"
    std::string softwareVersion;     ///< "0.1.0"
    std::optional<TimeRange> range;  ///< the exported range (timeline); nullopt = the whole sequence
    std::filesystem::path projectFile; ///< resolves relative media paths
    bool hashSources = true;         ///< SHA-256 of every source file (reads them in full)
    std::stop_token stop;
    /// 0.2: the producer's name (Premiere's identity in Content Credentials), written as the schema.org CreativeWork
    /// author; "" = none.
    std::string producer;
    /// 0.2: ask generative AI not to train on, use or mine the export (Premiere's "I request that generative AI models
    /// not train or use my content"): a cawg.training-mining assertion with every use "notAllowed".
    bool doNotTrain = false;
    /// 0.2: the export's thumbnail, JPEG bytes of a picture of the finished export ("" = none); called by
    /// exportFinalizer with the finished file. Unset = no thumbnail.
    std::function<std::string(const std::filesystem::path& file)> thumbnail;
};

/// The manifest definition (c2pa Builder JSON) for an export of `sequence`.
Json manifestDefinition(const ProjectState& state, SequenceId sequence, const ManifestOptions& options);

/// "video/mp4" for .mp4 / .m4v, "video/quicktime" for .mov, "" for formats the library cannot sign.
std::string formatForPath(const std::filesystem::path& file);

// --- Signing and verification ------------------------------------------------------------------------------------

struct SignResult {
    std::int64_t manifestBytes = 0;
    std::string activeManifest;      ///< the manifest's label
};

/// Signs `file` (MP4 / MOV) in place with `identity` and `definition`. The signed copy is verified before it replaces
/// the file; on any error the file is left exactly as it was. `format`: the MIME type ("" = from the file's extension;
/// an export's temporary file has none). `thumbnailJpeg`: the manifest's thumbnail ("" = none). Errors: Unsupported
/// (no library here, a format it cannot sign), InvalidRequest (a bad identity or definition), ProviderError (the
/// library failed), Cancelled.
std::expected<SignResult, AiError> signFile(const std::filesystem::path& file, const Json& definition, const SigningIdentity& identity,
                                            std::stop_token stop = {}, std::string format = {}, const std::string& thumbnailJpeg = {});

struct Verification {
    bool hasManifest = false;
    bool valid = false;        ///< signature, asset hash and certificate profile all good
    bool trusted = false;      ///< the signer chains to one of the trust anchors
    std::vector<std::string> failures; ///< validation codes that failed, e.g. "assertion.dataHash.mismatch"
    std::vector<std::string> codes;    ///< every validation code reported
    Json manifest;             ///< the active manifest (c2pa Reader JSON)
    std::string signer;        ///< the signature's issuer / common name as the library reports it
};

/// Reads and validates the manifest in `file`. `trustAnchorsPem`: the roots to trust (e.g. the local identity's
/// root); empty = none (a valid local signature then reads as untrusted). No network.
std::expected<Verification, AiError> verifyFile(const std::filesystem::path& file, const std::string& trustAnchorsPem = {},
                                                std::string format = {});

// --- Export ------------------------------------------------------------------------------------------------------

enum class CredentialsMode {
    Off,    ///< no Content Credentials (the unsigned provenance sidecar as before)
    Ai,     ///< sign exports that contain AI-generated material (the default)
    Always, ///< sign every export
};
std::string_view toString(CredentialsMode mode);
std::optional<CredentialsMode> credentialsModeFromString(std::string_view text);

/// Whether the export of `range` contains AI-generated media (MediaItem::generated) on an enabled clip.
bool containsGenerated(const ProjectState& state, SequenceId sequence, std::optional<TimeRange> range);

/// The identity an export signs with: the user's own (identityFromPem) or the local one. Called on the export thread.
using IdentityProvider = std::function<std::expected<SigningIdentity, AiError>()>;

/// An ExportRequest::finalize step: builds the manifest of `snapshot` and signs the finished file. Returns "" when it
/// signed, else the warning to show (the export stays complete and unsigned; the caller writes the unsigned sidecar).
/// `signedFlag` (optional) is set to true when it signed.
std::function<std::string(const std::filesystem::path&, std::stop_token)> exportFinalizer(std::shared_ptr<const ProjectState> snapshot,
    SequenceId sequence, ManifestOptions options, IdentityProvider identity, std::shared_ptr<std::atomic<bool>> signedFlag = {});

} // namespace wabcut::ai::c2pa
