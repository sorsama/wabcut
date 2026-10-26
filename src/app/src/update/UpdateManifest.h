#pragma once

// The version manifest a release publishes next to its packages (wabcut-update.json; docs/release.md, "Updates"):
//
//   { "format": "wabcut-update", "version": 1, "key": "<key id>",
//     "payload": "<base64 of the release JSON, the exact bytes that were signed>",
//     "signature": "<base64 of the ECDSA P-256 / SHA-256 signature, r || s>" }
//
// and the release JSON inside it:
//
//   { "product": "wabcut", "version": "0.5.0", "released": "2026-11-02", "notes": "<Markdown>",
//     "page": "https://github.com/sorsama/wabcut/releases/tag/v0.5.0",
//     "windows": { "url": ".../wabcut-0.5.0-windows-x64.msi", "size": 187654321, "sha256": "<64 hex>",
//                  "authenticode": true, "publisher": "<the signing certificate's name>" } }
//
// The payload is signed as bytes, so no JSON canonical form is needed. tools/update-keys.py writes and signs it in the
// release workflow. readManifest() refuses a manifest whose signature does not verify against one of the app's public
// keys, and one that does not describe a Wabcut release completely (version x.y.z, https addresses, size, SHA-256).

#include <QByteArray>
#include <QList>
#include <QString>
#include <QUrl>

#include <expected>

namespace wabcut::app::update {

struct UpdateRelease {
    QString version;        ///< "0.5.0"
    QString released;       ///< "2026-11-02" (may be empty)
    QString notes;          ///< what is new, Markdown (may be empty)
    QUrl page;              ///< the release page
    QUrl installerUrl;      ///< the Windows MSI (empty when the release has none)
    qint64 installerSize = 0;
    QByteArray installerSha256; ///< 32 bytes
    bool authenticode = false;  ///< the MSI carries an Authenticode signature (check it)
    QString publisher;          ///< the name it is signed with ("" = any trusted signer)
    bool verified = false;      ///< the manifest's signature was checked against the app's key
};

struct ManifestOptions {
    /// The app's public keys (DER SubjectPublicKeyInfo, P-256); one of them must have signed the payload.
    QList<QByteArray> publicKeys;
    /// Where no signature can be checked (not Windows): read the release anyway, marked unverified, so the app can say a
    /// version is out (it then links only to the fixed release page, never to an address from the manifest).
    bool acceptUnverified = false;
    /// Tests: http:// addresses on 127.0.0.1 / localhost are accepted (the loopback fake server).
    bool allowLoopbackHttp = false;
};

/// The release a manifest describes, or why it is refused (English, for the dialog and the log).
std::expected<UpdateRelease, QString> readManifest(const QByteArray& manifest, const ManifestOptions& options);

/// The public keys of a list ("<base64 SPKI>[;<base64 SPKI>...]", as WABCUT_UPDATE_PUBLIC_KEY holds them); malformed
/// entries are left out.
QList<QByteArray> parsePublicKeys(const QString& list);

/// < 0, 0, > 0 as `a` is older than, the same as, or newer than `b` (x.y.z, numerically).
int compareVersions(const QString& a, const QString& b);

/// The release page of a version under the releases base URL ("https://github.com/sorsama/wabcut/releases" ->
/// ".../releases/tag/v0.5.0"); empty for a version that is not x.y.z.
QUrl releasePage(const QString& releasesBase, const QString& version);

/// The version a release page's address names (".../releases/tag/v0.5.1" -> "0.5.1", where GitHub's releases/latest
/// redirects); empty when its last part is not [v]x.y.z.
QString versionOfReleasePage(const QUrl& page);

} // namespace wabcut::app::update
