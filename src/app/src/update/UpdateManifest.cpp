#include "UpdateManifest.h"

#include "UpdateCrypto.h"

#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QVersionNumber>

namespace wabcut::app::update {

namespace {

const QRegularExpression& versionPattern() {
    static const QRegularExpression pattern(QStringLiteral("^(0|[1-9][0-9]{0,4})\\.(0|[1-9][0-9]{0,4})\\.(0|[1-9][0-9]{0,4})$"));
    return pattern;
}

bool isLoopback(const QUrl& url) {
    const QString host = url.host();
    if (host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0) {
        return true;
    }
    const QHostAddress address(host);
    return !address.isNull() && address.isLoopback();
}

/// An address the app may fetch or open: https, or http on the loopback interface in tests.
bool acceptable(const QUrl& url, const ManifestOptions& options) {
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty()) {
        return false;
    }
    if (url.scheme() == QLatin1String("https")) {
        return true;
    }
    return options.allowLoopbackHttp && url.scheme() == QLatin1String("http") && isLoopback(url);
}

QByteArray fromBase64(const QJsonValue& value) {
    const auto decoded = QByteArray::fromBase64Encoding(value.toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    return decoded ? *decoded : QByteArray();
}

} // namespace

std::expected<UpdateRelease, QString> readManifest(const QByteArray& manifest, const ManifestOptions& options) {
    QJsonParseError error{};
    const QJsonDocument envelope = QJsonDocument::fromJson(manifest, &error);
    if (error.error != QJsonParseError::NoError || !envelope.isObject()) {
        return std::unexpected(QStringLiteral("the update information is not readable"));
    }
    const QJsonObject outer = envelope.object();
    if (outer.value(QLatin1String("format")).toString() != QLatin1String("wabcut-update")) {
        return std::unexpected(QStringLiteral("the update information is not Wabcut's"));
    }
    if (outer.value(QLatin1String("version")).toInt() != 1) {
        return std::unexpected(QStringLiteral("the update information needs a newer Wabcut to read it"));
    }
    const QByteArray payload = fromBase64(outer.value(QLatin1String("payload")));
    const QByteArray signature = fromBase64(outer.value(QLatin1String("signature")));
    if (payload.isEmpty()) {
        return std::unexpected(QStringLiteral("the update information is not readable"));
    }

    bool verified = false;
    if (canVerifySignatures()) {
        for (const QByteArray& key : options.publicKeys) {
            if (verifyP256Signature(key, payload, signature)) {
                verified = true;
                break;
            }
        }
        if (!verified) {
            return std::unexpected(QStringLiteral("the update information is not signed by Wabcut"));
        }
    } else if (!options.acceptUnverified) {
        return std::unexpected(QStringLiteral("updates cannot be checked on this system"));
    }

    const QJsonDocument inner = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !inner.isObject()) {
        return std::unexpected(QStringLiteral("the update information is not readable"));
    }
    const QJsonObject release = inner.object();
    if (release.value(QLatin1String("product")).toString() != QLatin1String("wabcut")) {
        return std::unexpected(QStringLiteral("the update information is not Wabcut's"));
    }
    UpdateRelease out;
    out.verified = verified;
    out.version = release.value(QLatin1String("version")).toString();
    if (!versionPattern().match(out.version).hasMatch()) {
        return std::unexpected(QStringLiteral("the update information has no valid version"));
    }
    out.released = release.value(QLatin1String("released")).toString().left(32);
    out.notes = release.value(QLatin1String("notes")).toString().left(20000);
    out.page = QUrl(release.value(QLatin1String("page")).toString());
    if (!acceptable(out.page, options)) {
        return std::unexpected(QStringLiteral("the update information has no valid release page"));
    }
    const QJsonValue windows = release.value(QLatin1String("windows"));
    if (windows.isObject()) {
        const QJsonObject w = windows.toObject();
        out.installerUrl = QUrl(w.value(QLatin1String("url")).toString());
        out.installerSize = static_cast<qint64>(w.value(QLatin1String("size")).toDouble(-1));
        out.installerSha256 = QByteArray::fromHex(w.value(QLatin1String("sha256")).toString().toLatin1());
        out.authenticode = w.value(QLatin1String("authenticode")).toBool(false);
        out.publisher = w.value(QLatin1String("publisher")).toString().left(256);
        const bool hexOk = w.value(QLatin1String("sha256")).toString().size() == 64 && out.installerSha256.size() == 32;
        // Up to 4 GiB: an MSI cannot be larger.
        if (!acceptable(out.installerUrl, options) || out.installerSize <= 0 || out.installerSize > (qint64{4} << 30) || !hexOk) {
            return std::unexpected(QStringLiteral("the update information describes the installer incompletely"));
        }
    }
    return out;
}

QList<QByteArray> parsePublicKeys(const QString& list) {
    QList<QByteArray> keys;
    for (const QString& part : list.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const auto decoded =
            QByteArray::fromBase64Encoding(part.trimmed().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (decoded && p256PointFromSpki(*decoded).size() == 64) {
            keys.push_back(*decoded);
        }
    }
    return keys;
}

int compareVersions(const QString& a, const QString& b) {
    return QVersionNumber::compare(QVersionNumber::fromString(a), QVersionNumber::fromString(b));
}

QUrl releasePage(const QString& releasesBase, const QString& version) {
    if (!versionPattern().match(version).hasMatch()) {
        return {};
    }
    QString base = releasesBase;
    while (base.endsWith(QLatin1Char('/'))) {
        base.chop(1);
    }
    return QUrl(base + QStringLiteral("/tag/v") + version);
}

QString versionOfReleasePage(const QUrl& page) {
    QString path = page.path();
    while (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
    if (slash < 0 || !path.left(slash).endsWith(QLatin1String("/tag"))) {
        return {};
    }
    QString version = path.mid(slash + 1);
    if (version.startsWith(QLatin1Char('v')) || version.startsWith(QLatin1Char('V'))) {
        version.remove(0, 1);
    }
    return versionPattern().match(version).hasMatch() ? version : QString();
}

} // namespace wabcut::app::update
