#pragma once

// The checks an update passes before Wabcut installs it (docs/release.md, "Updates"): the version manifest's ECDSA
// P-256 / SHA-256 signature against the public key built into the app, and the installer's Authenticode signature.
// Windows only (CNG / BCrypt and WinVerifyTrust: no extra library); on other platforms nothing verifies, and updates
// are only announced (UpdateBridge).

#include <QByteArray>
#include <QString>

namespace wabcut::app::update {

/// Whether this build can check manifest signatures (Windows).
bool canVerifySignatures();

/// The raw point X || Y (64 bytes) of a DER SubjectPublicKeyInfo holding a P-256 (prime256v1) key; empty for anything
/// else.
QByteArray p256PointFromSpki(const QByteArray& spki);
/// The DER SubjectPublicKeyInfo of a P-256 point X || Y (64 bytes); empty for another size.
QByteArray spkiFromP256Point(const QByteArray& point);

/// True when `signature` (r || s, 64 bytes, big-endian) is the ECDSA P-256 signature of SHA-256(`message`) by the key
/// `publicKeySpki` (DER SubjectPublicKeyInfo). False on any mismatch or malformed input, and where nothing verifies.
bool verifyP256Signature(const QByteArray& publicKeySpki, const QByteArray& message, const QByteArray& signature);

struct AuthenticodeResult {
    bool trusted = false; ///< a valid signature that chains to a root Windows trusts
    QString signer;       ///< the signing certificate's name ("Wabcut Contributors"), when trusted
    QString error;        ///< why not, in English (for the log and the failure text)
};

/// The Authenticode signature of `path` (an .msi, .exe or .dll; WinVerifyTrust, no UI, no online revocation check).
/// Elsewhere: not trusted, "not supported here".
AuthenticodeResult checkAuthenticode(const QString& path);

} // namespace wabcut::app::update
