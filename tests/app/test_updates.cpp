// P0 round (stream E): opt-in updates (UpdateBridge, update/UpdateManifest, update/UpdateCrypto; docs/release.md
// "Updates"). Against a loopback fake HTTP server (a QTcpServer here: no network, no GitHub): a newer signed release is
// offered, the same version is not, a tampered manifest and one signed with another key are refused, a tampered installer
// (size, SHA-256) is refused and deleted, an installer the manifest calls signed needs a trusted Authenticode signature by
// the named publisher, a verified one is installed through msiexec after Wabcut closes (the launcher is injected: no
// installer ever runs), automatic checks are off by default and at most daily. A manifest signed by tools/update-keys.py
// verifies here (data/updates). In the window: Help > Check for Updates... and the dialog's stages (screenshots, dark and
// light), and Settings > General's Updates rows. Signing needs Windows CNG: elsewhere the signed cases are skipped.

#include "ShellFixture.h"

#include "ProjectLifecycle.h"
#include "UpdateBridge.h"
#include "update/UpdateCrypto.h"
#include "update/UpdateManifest.h"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThreadPool>

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

using namespace wabcut::app;
using namespace wabcut::test;
namespace update = wabcut::app::update;

namespace {

#if defined(_WIN32) // the helpers of the signed cases (signing needs Windows CNG)
// ---- A loopback HTTP/1.1 server: one response per connection ----
class FakeServer : public QObject {
public:
    struct Route {
        int status = 200;
        QByteArray body;
        QByteArray location; ///< a redirect's target
    };

    FakeServer() {
        REQUIRE(m_server.listen(QHostAddress::LocalHost, 0));
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { read(socket); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    void serve(const QString& path, QByteArray body) { m_routes[path] = {200, std::move(body), {}}; }
    void redirect(const QString& path, const QString& to) { m_routes[path] = {302, {}, url(to).toString().toLatin1()}; }
    QUrl url(const QString& path) const {
        return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(m_server.serverPort()).arg(path));
    }
    QStringList requests;
    QStringList userAgents;

private:
    void read(QTcpSocket* socket) {
        QByteArray& buffer = m_buffers[socket];
        buffer += socket->readAll();
        const qsizetype end = buffer.indexOf("\r\n\r\n");
        if (end < 0) {
            return;
        }
        const QList<QByteArray> lines = buffer.left(end).split('\n');
        m_buffers.remove(socket);
        const QList<QByteArray> request = lines.value(0).trimmed().split(' ');
        const QString path = QString::fromLatin1(request.value(1));
        requests << path;
        for (const QByteArray& line : lines) {
            if (line.toLower().startsWith("user-agent:")) {
                userAgents << QString::fromLatin1(line.mid(11).trimmed());
            }
        }
        const Route route = m_routes.value(path, Route{404, QByteArrayLiteral("not found"), {}});
        QByteArray response = "HTTP/1.1 " + QByteArray::number(route.status) +
                              (route.status == 200 ? " OK" : route.status == 302 ? " Found" : " Not Found") + "\r\n";
        if (!route.location.isEmpty()) {
            response += "Location: " + route.location + "\r\n";
        }
        response += "Content-Type: application/octet-stream\r\nContent-Length: " + QByteArray::number(route.body.size()) +
                    "\r\nConnection: close\r\n\r\n" + route.body;
        socket->write(response);
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
    QHash<QString, Route> m_routes;
    QHash<QTcpSocket*, QByteArray> m_buffers;
};

/// An ephemeral P-256 key (CNG) that signs the tests' manifests; the app is told to trust its public half.
class TestKey {
public:
    TestKey() {
        REQUIRE(BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&m_algorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0)));
        REQUIRE(BCRYPT_SUCCESS(BCryptGenerateKeyPair(m_algorithm, &m_key, 256, 0)));
        REQUIRE(BCRYPT_SUCCESS(BCryptFinalizeKeyPair(m_key, 0)));
    }
    ~TestKey() {
        if (m_key) {
            BCryptDestroyKey(m_key);
        }
        if (m_algorithm) {
            BCryptCloseAlgorithmProvider(m_algorithm, 0);
        }
    }
    TestKey(const TestKey&) = delete;
    TestKey& operator=(const TestKey&) = delete;

    QByteArray publicKey() const {
        ULONG size = 0;
        BCryptExportKey(m_key, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &size, 0);
        QByteArray blob(static_cast<qsizetype>(size), '\0');
        BCryptExportKey(m_key, nullptr, BCRYPT_ECCPUBLIC_BLOB, reinterpret_cast<PUCHAR>(blob.data()), size, &size, 0);
        return update::spkiFromP256Point(blob.mid(sizeof(BCRYPT_ECCKEY_BLOB), 64));
    }
    QByteArray sign(const QByteArray& message) const {
        QByteArray digest = QCryptographicHash::hash(message, QCryptographicHash::Sha256);
        ULONG size = 0;
        BCryptSignHash(m_key, nullptr, reinterpret_cast<PUCHAR>(digest.data()), 32, nullptr, 0, &size, 0);
        QByteArray signature(static_cast<qsizetype>(size), '\0');
        BCryptSignHash(m_key, nullptr, reinterpret_cast<PUCHAR>(digest.data()), 32, reinterpret_cast<PUCHAR>(signature.data()),
                       size, &size, 0);
        return signature; // r || s
    }

private:
    BCRYPT_ALG_HANDLE m_algorithm = nullptr;
    BCRYPT_KEY_HANDLE m_key = nullptr;
};

struct ReleaseSpec {
    QString version = QStringLiteral("9.9.9");
    QByteArray installer;
    QUrl installerUrl;
    QUrl page = QUrl(QStringLiteral("https://github.com/sorsama/wabcut/releases/tag/v9.9.9"));
    bool authenticode = false;
    QString publisher;
    std::optional<qint64> size; ///< the size the manifest claims (default: the installer's)
    QByteArray sha256;          ///< the checksum it claims (default: the installer's)
};

QByteArray payloadOf(const ReleaseSpec& spec) {
    const QJsonObject windows{
        {QStringLiteral("url"), spec.installerUrl.toString()},
        {QStringLiteral("size"), static_cast<double>(spec.size.value_or(spec.installer.size()))},
        {QStringLiteral("sha256"), QString::fromLatin1((spec.sha256.isEmpty()
                                                            ? QCryptographicHash::hash(spec.installer, QCryptographicHash::Sha256)
                                                            : spec.sha256)
                                                           .toHex())},
        {QStringLiteral("authenticode"), spec.authenticode},
        {QStringLiteral("publisher"), spec.publisher},
    };
    const QJsonObject release{
        {QStringLiteral("product"), QStringLiteral("wabcut")},
        {QStringLiteral("version"), spec.version},
        {QStringLiteral("released"), QStringLiteral("2026-11-02")},
        {QStringLiteral("notes"), QStringLiteral("## What's new\n\n- **Graphics card** in Settings\n- Updates from inside Wabcut\n")},
        {QStringLiteral("page"), spec.page.toString()},
        {QStringLiteral("windows"), windows},
    };
    return QJsonDocument(release).toJson(QJsonDocument::Compact);
}

QByteArray envelope(const QByteArray& payload, const QByteArray& signature) {
    const QJsonObject outer{{QStringLiteral("format"), QStringLiteral("wabcut-update")},
                            {QStringLiteral("version"), 1},
                            {QStringLiteral("key"), QStringLiteral("test")},
                            {QStringLiteral("payload"), QString::fromLatin1(payload.toBase64())},
                            {QStringLiteral("signature"), QString::fromLatin1(signature.toBase64())}};
    return QJsonDocument(outer).toJson();
}

/// About 300 KB that is not an MSI (the installer is never run).
QByteArray fakeInstaller(char seed = 'a') {
    QByteArray bytes;
    for (int i = 0; i < 300 * 1024; ++i) {
        bytes.push_back(static_cast<char>(seed + (i * 7) % 23));
    }
    return bytes;
}

/// A control of a dialog (a Popup): its QObject children, else the visual tree under its content item.
QObject* inDialog(QObject* dialog, const char* name) {
    const QString wanted = QString::fromLatin1(name);
    if (QObject* found = dialog->findChild<QObject*>(wanted)) {
        return found;
    }
    const std::function<QObject*(QQuickItem*)> walk = [&](QQuickItem* item) -> QObject* {
        if (!item) {
            return nullptr;
        }
        if (item->objectName() == wanted) {
            return item;
        }
        for (QQuickItem* child : item->childItems()) {
            if (QObject* found = walk(child)) {
                return found;
            }
        }
        return nullptr;
    };
    return walk(dialog->property("contentItem").value<QQuickItem*>());
}

struct Launched {
    QString program;
    QStringList arguments;
    int calls = 0;
};

/// The decoded -EncodedCommand of a PowerShell command line (UTF-16LE in base64).
QString decodedScript(const QStringList& arguments) {
    const qsizetype i = arguments.indexOf(QStringLiteral("-EncodedCommand"));
    if (i < 0 || i + 1 >= arguments.size()) {
        return {};
    }
    const QByteArray utf16 = QByteArray::fromBase64(arguments.at(i + 1).toLatin1());
    return QString::fromUtf16(reinterpret_cast<const char16_t*>(utf16.constData()), utf16.size() / 2);
}

/// A bridge on the fake server, the test's key and a temporary download folder; the launcher records instead of running.
struct Rig {
    FakeServer server;
    QTemporaryDir folder;
    UpdateBridge bridge;
    Launched launched;
    QList<QByteArray> keys;

    explicit Rig(QList<QByteArray> trusted) : keys(std::move(trusted)) {
        REQUIRE(folder.isValid());
        UpdateBridge::Config c;
        c.manifestUrl = server.url(QStringLiteral("/latest/download/wabcut-update.json"));
        c.publicKeys = keys;
        c.releasesBase = QStringLiteral("https://github.com/sorsama/wabcut/releases");
        c.currentVersion = QStringLiteral("0.4.0");
        c.downloadFolder = folder.filePath(QStringLiteral("updates"));
        c.allowLoopbackHttp = true;
        c.installedByMsi = true;
        c.installRoot = QStringLiteral("C:\\Users\\Test User\\AppData\\Local\\Programs\\Wabcut\\");
        bridge.setConfig(c);
        UpdateBridge::setLauncher([this](const QString& program, const QStringList& arguments) {
            launched.program = program;
            launched.arguments = arguments;
            ++launched.calls;
            return true;
        });
    }
    ~Rig() {
        bridge.cancelInstall();
        UpdateBridge::setLauncher({});
    }
    /// Serves `manifest` where GitHub's latest/download link would redirect to it.
    void publish(const QByteArray& manifest) {
        server.serve(QStringLiteral("/assets/wabcut-update.json"), manifest);
        server.redirect(QStringLiteral("/latest/download/wabcut-update.json"), QStringLiteral("/assets/wabcut-update.json"));
    }
    bool settled() const {
        const QString s = bridge.state();
        return s != QLatin1String("checking") && s != QLatin1String("downloading");
    }
    void check() {
        bridge.check();
        REQUIRE(waitFor([&] { return settled(); }, 30'000));
    }
    void download() {
        bridge.download();
        REQUIRE(waitFor([&] { return settled(); }, 60'000));
    }
    QStringList files() const {
        return QDir(folder.filePath(QStringLiteral("updates"))).entryList(QDir::Files);
    }
};
#endif

} // namespace

TEST_CASE("Updates: the manifest format the release tool writes and the built-in public key", "[app][updates]") {
    // The release's key (CMake's WABCUT_UPDATE_PUBLIC_KEY) is a P-256 key, and the default address is GitHub's latest release.
    const QList<QByteArray> keys = update::parsePublicKeys(QStringLiteral(WABCUT_UPDATE_PUBLIC_KEY_DEFAULT));
    REQUIRE(keys.size() == 1);
    CHECK(update::p256PointFromSpki(keys.front()).size() == 64);
    CHECK(update::parsePublicKeys(QStringLiteral("not base64!;") + QStringLiteral(WABCUT_UPDATE_PUBLIC_KEY_DEFAULT)).size() == 1);
    CHECK(update::parsePublicKeys(QStringLiteral("AAAA")).isEmpty());
    const UpdateBridge::Config defaults = UpdateBridge::defaultConfig();
    CHECK(defaults.manifestUrl.toString() ==
          QStringLiteral("https://github.com/sorsama/wabcut/releases/latest/download/wabcut-update.json"));
    CHECK(defaults.publicKeys == keys);

    CHECK(update::compareVersions(QStringLiteral("0.10.0"), QStringLiteral("0.9.9")) > 0);
    CHECK(update::compareVersions(QStringLiteral("0.4.0"), QStringLiteral("0.4.0")) == 0);
    CHECK(update::compareVersions(QStringLiteral("0.4.0"), QStringLiteral("1.0.0")) < 0);
    CHECK(update::releasePage(QStringLiteral("https://github.com/sorsama/wabcut/releases/"), QStringLiteral("0.5.0")) ==
          QUrl(QStringLiteral("https://github.com/sorsama/wabcut/releases/tag/v0.5.0")));
    CHECK(update::releasePage(QStringLiteral("https://github.com/sorsama/wabcut/releases"), QStringLiteral("0.5.0/../x")).isEmpty());

#if defined(_WIN32)
    // tools/update-keys.py signed data/updates/wabcut-update.json with a throwaway key (its public half is next to it):
    // Python's DER signature, turned into r || s, verifies with CNG here.
    const QString dir = QStringLiteral(WABCUT_APP_TEST_DIR "/data/updates/");
    QFile manifestFile(dir + QStringLiteral("wabcut-update.json"));
    QFile keyFile(dir + QStringLiteral("test-public-key.txt"));
    QFile installerFile(dir + QStringLiteral("installer.bin"));
    REQUIRE(manifestFile.open(QIODevice::ReadOnly));
    REQUIRE(keyFile.open(QIODevice::ReadOnly));
    REQUIRE(installerFile.open(QIODevice::ReadOnly));
    const QByteArray manifest = manifestFile.readAll();
    const QList<QByteArray> toolKey = update::parsePublicKeys(QString::fromLatin1(keyFile.readAll()));
    REQUIRE(toolKey.size() == 1);
    const auto release = update::readManifest(manifest, {.publicKeys = toolKey});
    INFO((release ? QString() : release.error()).toStdString());
    REQUIRE(release.has_value());
    CHECK(release->verified);
    CHECK(release->version == QStringLiteral("9.8.7"));
    CHECK(release->released == QStringLiteral("2026-10-26"));
    CHECK(release->notes.contains(QStringLiteral("The test release")));
    CHECK(release->installerUrl.toString() ==
          QStringLiteral("https://github.com/sorsama/wabcut/releases/download/v9.8.7/wabcut-9.8.7-windows-x64.msi"));
    const QByteArray installer = installerFile.readAll();
    CHECK(release->installerSize == installer.size());
    CHECK(release->installerSha256 == QCryptographicHash::hash(installer, QCryptographicHash::Sha256));
    // The release key did not sign it.
    CHECK_FALSE(update::readManifest(manifest, {.publicKeys = keys}).has_value());
#endif
}

TEST_CASE("Updates: a newer signed release is offered, verified and installed after Wabcut closes", "[app][updates]") {
#if !defined(_WIN32)
    SKIP("manifest signatures are checked with Windows CNG");
#else
    resetSettings();
    TestKey key;
    Rig rig({key.publicKey()});
    CHECK_FALSE(rig.bridge.automatic()); // opt-in
    ReleaseSpec spec;
    spec.installer = fakeInstaller();
    spec.installerUrl = rig.server.url(QStringLiteral("/download/v9.9.9/wabcut-9.9.9-windows-x64.msi"));
    rig.server.serve(QStringLiteral("/download/v9.9.9/wabcut-9.9.9-windows-x64.msi"), spec.installer);
    const QByteArray payload = payloadOf(spec);
    rig.publish(envelope(payload, key.sign(payload)));

    rig.check();
    INFO(rig.bridge.error().toStdString());
    REQUIRE(rig.bridge.state() == QStringLiteral("available"));
    CHECK(rig.bridge.latestVersion() == QStringLiteral("9.9.9"));
    CHECK(rig.bridge.released() == QStringLiteral("2026-11-02"));
    CHECK(rig.bridge.notes().contains(QStringLiteral("Graphics card")));
    CHECK(rig.bridge.canInstall());
    CHECK_FALSE(rig.bridge.lastChecked().isEmpty());
    // GitHub's latest/download link redirects to the asset; the request names Wabcut and nothing else.
    CHECK(rig.server.requests.contains(QStringLiteral("/latest/download/wabcut-update.json")));
    CHECK(rig.server.requests.contains(QStringLiteral("/assets/wabcut-update.json")));
    REQUIRE_FALSE(rig.server.userAgents.isEmpty());
    CHECK(rig.server.userAgents.front() == QStringLiteral("Wabcut/0.4.0"));

    // Download: size and SHA-256 match, so it is ready; nothing runs yet.
    rig.download();
    REQUIRE(rig.bridge.state() == QStringLiteral("ready"));
    CHECK(rig.bridge.progress() == 1.0);
    CHECK(rig.files() == QStringList{QStringLiteral("wabcut-9.9.9-windows-x64.msi")});
    QFile saved(QDir(rig.folder.filePath(QStringLiteral("updates"))).filePath(QStringLiteral("wabcut-9.9.9-windows-x64.msi")));
    REQUIRE(saved.open(QIODevice::ReadOnly));
    CHECK(saved.readAll() == spec.installer);
    saved.close();
    CHECK(rig.launched.calls == 0);
    CHECK_FALSE(UpdateBridge::pendingInstall().has_value());

    // "Quit and Install": scheduled, and the window is asked to close the normal way.
    int quits = 0;
    QObject::connect(&rig.bridge, &UpdateBridge::quitRequested, [&] { ++quits; });
    rig.bridge.quitAndInstall();
    CHECK(quits == 1);
    CHECK(rig.bridge.installScheduled());
    const auto pending = UpdateBridge::pendingInstall();
    REQUIRE(pending.has_value());
    CHECK(pending->installer == QDir::toNativeSeparators(saved.fileName()));
    CHECK(pending->installRoot == QStringLiteral("C:\\Users\\Test User\\AppData\\Local\\Programs\\Wabcut"));
    CHECK(pending->waitForPid == QCoreApplication::applicationPid());

    // After the event loop main() starts it: a hidden PowerShell that waits for Wabcut to end, runs msiexec /passive into
    // the same folder and starts Wabcut again. Never msiexec itself in a test.
    REQUIRE(UpdateBridge::launchPendingInstall());
    CHECK(rig.launched.calls == 1);
    CHECK(rig.launched.program.endsWith(QStringLiteral("\\System32\\WindowsPowerShell\\v1.0\\powershell.exe"), Qt::CaseInsensitive));
    CHECK(rig.launched.arguments.contains(QStringLiteral("-NonInteractive")));
    const QString script = decodedScript(rig.launched.arguments);
    INFO(script.toStdString());
    CHECK(script.contains(QStringLiteral("Wait-Process -Id %1").arg(QCoreApplication::applicationPid())));
    CHECK(script.contains(QStringLiteral("\\System32\\msiexec.exe'")));
    CHECK(script.contains(QStringLiteral("'/passive'")));
    CHECK(script.contains(QStringLiteral("'/norestart'")));
    CHECK(script.contains(QStringLiteral("$msi = '%1'").arg(pending->installer)));
    CHECK(script.contains(QStringLiteral("'INSTALL_ROOT=\"C:\\Users\\Test User\\AppData\\Local\\Programs\\Wabcut\"'")));
    CHECK(script.contains(QStringLiteral("$app = '%1'").arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()))));
    // Once only, and nothing when nothing is scheduled.
    CHECK_FALSE(rig.bridge.installScheduled());
    CHECK_FALSE(UpdateBridge::launchPendingInstall());
    CHECK(rig.launched.calls == 1);

    // A quote in a path stays one PowerShell string.
    UpdateBridge::PendingInstall odd;
    odd.installer = QStringLiteral("C:\\Users\\O'Neil\\x.msi");
    QString program;
    CHECK(decodedScript(UpdateBridge::installerCommand(odd, &program)).contains(QStringLiteral("$msi = 'C:\\Users\\O''Neil\\x.msi'")));
    resetSettings();
#endif
}

TEST_CASE("Updates: the same or an older version is not offered", "[app][updates]") {
#if !defined(_WIN32)
    SKIP("manifest signatures are checked with Windows CNG");
#else
    resetSettings();
    TestKey key;
    for (const char* version : {"0.4.0", "0.3.9"}) {
        INFO(version);
        Rig rig({key.publicKey()});
        ReleaseSpec spec;
        spec.version = QString::fromLatin1(version);
        spec.installer = fakeInstaller();
        spec.installerUrl = rig.server.url(QStringLiteral("/x.msi"));
        const QByteArray payload = payloadOf(spec);
        rig.publish(envelope(payload, key.sign(payload)));
        rig.check();
        CHECK(rig.bridge.state() == QStringLiteral("upToDate"));
        CHECK_FALSE(rig.bridge.canInstall());
        rig.bridge.download(); // nothing to download
        CHECK(rig.bridge.state() == QStringLiteral("upToDate"));
        CHECK(rig.files().isEmpty());
    }
    resetSettings();
#endif
}

TEST_CASE("Updates: a tampered manifest or one signed with another key is refused", "[app][updates]") {
#if !defined(_WIN32)
    SKIP("manifest signatures are checked with Windows CNG");
#else
    resetSettings();
    TestKey key;
    TestKey other;
    Rig rig({key.publicKey()});
    ReleaseSpec spec;
    spec.installer = fakeInstaller();
    spec.installerUrl = rig.server.url(QStringLiteral("/x.msi"));
    const QByteArray payload = payloadOf(spec);
    const auto refused = [&](const QByteArray& manifest, const char* why) {
        INFO(why);
        rig.publish(manifest);
        rig.check();
        CHECK(rig.bridge.state() == QStringLiteral("failed"));
        CHECK(rig.bridge.error().contains(QString::fromLatin1(why)));
        CHECK(rig.bridge.latestVersion().isEmpty()); // nothing from it is kept
        CHECK_FALSE(rig.bridge.canInstall());
    };

    // The payload changed after signing (a newer version, another installer address): the signature no longer matches.
    QByteArray tampered = payload;
    tampered.replace("9.9.9", "9.9.8");
    refused(envelope(tampered, key.sign(payload)), "not signed by Wabcut");
    // A valid signature by a key the app does not trust.
    refused(envelope(payload, other.sign(payload)), "not signed by Wabcut");
    // A broken or missing signature.
    QByteArray flipped = key.sign(payload);
    flipped[10] = static_cast<char>(flipped[10] ^ 0x5A);
    refused(envelope(payload, flipped), "not signed by Wabcut");
    refused(envelope(payload, key.sign(payload).left(40)), "not signed by Wabcut");
    refused(envelope(payload, {}), "not signed by Wabcut");
    // Not a manifest at all.
    refused(QByteArrayLiteral("<html>rate limited</html>"), "not readable");
    QJsonObject wrongFormat = QJsonDocument::fromJson(envelope(payload, key.sign(payload))).object();
    wrongFormat.insert(QStringLiteral("format"), QStringLiteral("something-else"));
    refused(QJsonDocument(wrongFormat).toJson(), "not Wabcut's");
    // Signed, but the installer address is plain http on the internet.
    ReleaseSpec insecure = spec;
    insecure.installerUrl = QUrl(QStringLiteral("http://example.com/wabcut.msi"));
    const QByteArray insecurePayload = payloadOf(insecure);
    refused(envelope(insecurePayload, key.sign(insecurePayload)), "installer incompletely");
    // No manifest on the release: a 404.
    Rig empty({key.publicKey()});
    empty.check();
    CHECK(empty.bridge.state() == QStringLiteral("failed"));
    CHECK(empty.bridge.error().contains(QStringLiteral("No update information")));

    // The same manifest, untouched, passes.
    rig.publish(envelope(payload, key.sign(payload)));
    rig.check();
    CHECK(rig.bridge.state() == QStringLiteral("available"));
    resetSettings();
#endif
}

TEST_CASE("Updates: a release without its manifest is still found through releases/latest, and never installed",
          "[app][updates]") {
    // Where GitHub's releases/latest redirects: the version alone.
    CHECK(update::versionOfReleasePage(QUrl(QStringLiteral("https://github.com/sorsama/wabcut/releases/tag/v0.5.1"))) ==
          QStringLiteral("0.5.1"));
    CHECK(update::versionOfReleasePage(QUrl(QStringLiteral("https://example.com/releases/tag/1.2.3/"))) == QStringLiteral("1.2.3"));
    CHECK(update::versionOfReleasePage(QUrl(QStringLiteral("https://github.com/sorsama/wabcut/releases"))).isEmpty());
    CHECK(update::versionOfReleasePage(QUrl(QStringLiteral("https://github.com/sorsama/wabcut/releases/tag/nightly"))).isEmpty());
    CHECK(update::versionOfReleasePage(QUrl(QStringLiteral("https://github.com/sorsama/wabcut/releases/v0.5.1"))).isEmpty());
#if !defined(_WIN32)
    SKIP("the fake update server is set up with the signed cases (Windows CNG)");
#else
    resetSettings();
    TestKey key;
    // The release has no wabcut-update.json (a 404); its latest link redirects to `tag`.
    const auto rigFor = [&](const QString& tag) {
        auto rig = std::make_unique<Rig>(QList<QByteArray>{key.publicKey()});
        UpdateBridge::Config c = rig->bridge.config();
        c.latestUrl = rig->server.url(QStringLiteral("/latest"));
        rig->bridge.setConfig(c);
        rig->server.redirect(QStringLiteral("/latest"), tag);
        return rig;
    };

    {
        INFO("a newer version: offered with its release page only");
        auto rig = rigFor(QStringLiteral("/releases/tag/v9.9.9"));
        rig->check();
        CHECK(rig->bridge.state() == QStringLiteral("available"));
        CHECK(rig->bridge.latestVersion() == QStringLiteral("9.9.9"));
        CHECK(rig->server.requests ==
              QStringList({QStringLiteral("/latest/download/wabcut-update.json"), QStringLiteral("/latest")}));
        CHECK_FALSE(rig->bridge.canInstall());
        CHECK(rig->bridge.installHint().contains(QStringLiteral("release page")));
        CHECK(rig->bridge.notes().isEmpty());
        REQUIRE(rig->bridge.release().has_value());
        CHECK_FALSE(rig->bridge.release()->verified);
        CHECK(rig->bridge.release()->installerUrl.isEmpty());
        CHECK(rig->bridge.release()->page == QUrl(QStringLiteral("https://github.com/sorsama/wabcut/releases/tag/v9.9.9")));
        rig->bridge.download(); // nothing it could verify
        CHECK(rig->bridge.state() == QStringLiteral("available"));
        CHECK(rig->files().isEmpty());
        CHECK(rig->server.requests.size() == 2);
    }
    {
        INFO("an automatic check says so too");
        auto rig = rigFor(QStringLiteral("/releases/tag/v9.9.9"));
        QStringList announced;
        QObject::connect(&rig->bridge, &UpdateBridge::updateAvailable, [&](const QString& v) { announced << v; });
        rig->bridge.checkAutomatically(true);
        REQUIRE(waitFor([&] { return rig->settled(); }, 30'000));
        CHECK(rig->bridge.state() == QStringLiteral("available"));
        CHECK(announced == QStringList{QStringLiteral("9.9.9")});
    }
    {
        INFO("the version running: up to date");
        auto rig = rigFor(QStringLiteral("/releases/tag/v0.4.0"));
        rig->check();
        CHECK(rig->bridge.state() == QStringLiteral("upToDate"));
        CHECK(rig->bridge.latestVersion() == QStringLiteral("0.4.0"));
    }
    {
        INFO("no release yet (GitHub's latest goes to the list): no update information");
        auto rig = rigFor(QStringLiteral("/releases"));
        rig->check();
        CHECK(rig->bridge.state() == QStringLiteral("failed"));
        CHECK(rig->bridge.error().contains(QStringLiteral("No update information")));
        CHECK(rig->bridge.latestVersion().isEmpty());
    }
    resetSettings();
#endif
}

TEST_CASE("Updates: a tampered installer is refused by its size and SHA-256 and deleted", "[app][updates]") {
#if !defined(_WIN32)
    SKIP("manifest signatures are checked with Windows CNG");
#else
    resetSettings();
    TestKey key;
    const QByteArray good = fakeInstaller('a');
    const auto attempt = [&](const QByteArray& served, const char* why) {
        INFO(why);
        Rig rig({key.publicKey()});
        ReleaseSpec spec;
        spec.installer = good;
        spec.installerUrl = rig.server.url(QStringLiteral("/x.msi"));
        rig.server.serve(QStringLiteral("/x.msi"), served);
        const QByteArray payload = payloadOf(spec);
        rig.publish(envelope(payload, key.sign(payload)));
        rig.check();
        REQUIRE(rig.bridge.state() == QStringLiteral("available"));
        rig.download();
        CHECK(rig.bridge.state() == QStringLiteral("failed"));
        CHECK(rig.bridge.error().contains(QString::fromLatin1(why)));
        CHECK(rig.files().isEmpty()); // neither the .part nor an .msi is left
        rig.bridge.installWhenClosed(); // nothing to schedule
        CHECK_FALSE(rig.bridge.installScheduled());
        CHECK_FALSE(UpdateBridge::launchPendingInstall());
        CHECK(rig.launched.calls == 0);
    };
    QByteArray changed = good;
    changed[1000] = static_cast<char>(changed[1000] ^ 1); // same size, one bit flipped
    attempt(changed, "SHA-256");
    attempt(good + QByteArrayLiteral("trailing bytes"), "larger");
    attempt(good.left(good.size() - 10), "not the size");
    resetSettings();
#endif
}

TEST_CASE("Updates: an installer the manifest calls signed needs a trusted signature by the named publisher",
          "[app][updates]") {
#if !defined(_WIN32)
    SKIP("Authenticode is checked on Windows");
#else
    resetSettings();
    TestKey key;
    const auto run = [&](const QByteArray& installer, const QString& publisher) {
        auto rig = std::make_unique<Rig>(QList<QByteArray>{key.publicKey()});
        ReleaseSpec spec;
        spec.installer = installer;
        spec.installerUrl = rig->server.url(QStringLiteral("/x.msi"));
        spec.authenticode = true;
        spec.publisher = publisher;
        rig->server.serve(QStringLiteral("/x.msi"), installer);
        const QByteArray payload = payloadOf(spec);
        rig->publish(envelope(payload, key.sign(payload)));
        rig->check();
        REQUIRE(rig->bridge.state() == QStringLiteral("available"));
        rig->download();
        return rig;
    };
    // Unsigned bytes, though the manifest says signed.
    {
        const auto rig = run(fakeInstaller(), QString());
        CHECK(rig->bridge.state() == QStringLiteral("failed"));
        CHECK(rig->bridge.error().contains(QStringLiteral("not signed")));
        CHECK(rig->files().isEmpty());
    }
    // A file with a real, trusted Authenticode signature: Qt's own Qt6Core.dll (the checks look at the signature, not
    // the file type). Accepted from its publisher, refused from anybody else's name.
    QFile qt(QCoreApplication::applicationDirPath() + QStringLiteral("/Qt6Core.dll"));
    const update::AuthenticodeResult qtSignature = update::checkAuthenticode(qt.fileName());
    if (!qtSignature.trusted || !qt.open(QIODevice::ReadOnly)) {
        SKIP("no Authenticode-signed Qt6Core.dll next to the tests: " << qtSignature.error.toStdString());
    }
    CHECK_FALSE(qtSignature.signer.isEmpty());
    const QByteArray signedBytes = qt.readAll();
    {
        const auto rig = run(signedBytes, qtSignature.signer);
        INFO(rig->bridge.error().toStdString());
        CHECK(rig->bridge.state() == QStringLiteral("ready"));
    }
    {
        const auto rig = run(signedBytes, QStringLiteral("Wabcut Contributors"));
        CHECK(rig->bridge.state() == QStringLiteral("failed"));
        CHECK(rig->bridge.error().contains(QStringLiteral("signed by")));
        CHECK(rig->files().isEmpty());
    }
    // A signed file changed after signing.
    QByteArray broken = signedBytes;
    broken[broken.size() / 2] = static_cast<char>(broken[broken.size() / 2] ^ 0x01);
    {
        const auto rig = run(broken, qtSignature.signer);
        CHECK(rig->bridge.state() == QStringLiteral("failed"));
        CHECK(rig->files().isEmpty());
    }
    resetSettings();
#endif
}

TEST_CASE("Updates: automatic checks are off by default, at most daily and quiet about a skipped version", "[app][updates]") {
#if !defined(_WIN32)
    SKIP("manifest signatures are checked with Windows CNG");
#else
    resetSettings();
    TestKey key;
    Rig rig({key.publicKey()});
    ReleaseSpec spec;
    spec.installer = fakeInstaller();
    spec.installerUrl = rig.server.url(QStringLiteral("/x.msi"));
    const QByteArray payload = payloadOf(spec);
    rig.publish(envelope(payload, key.sign(payload)));
    QStringList announced;
    QObject::connect(&rig.bridge, &UpdateBridge::updateAvailable, [&](const QString& v) { announced << v; });

    CHECK_FALSE(rig.bridge.automatic());
    CHECK_FALSE(QSettings().value(QStringLiteral("updates/automatic"), false).toBool());
    rig.bridge.setAutomatic(true);
    CHECK(rig.bridge.automatic());
    CHECK(QSettings().value(QStringLiteral("updates/automatic")).toBool());

    rig.bridge.checkAutomatically();
    REQUIRE(waitFor([&] { return rig.settled(); }, 30'000));
    CHECK(rig.bridge.state() == QStringLiteral("available"));
    CHECK(announced == QStringList{QStringLiteral("9.9.9")});
    // Within the day: no second request.
    rig.bridge.setConfig(rig.bridge.config()); // back to idle, as at the next start
    const qsizetype requests = rig.server.requests.size();
    rig.bridge.checkAutomatically();
    spin(300);
    CHECK(rig.server.requests.size() == requests);
    CHECK(rig.bridge.state() == QStringLiteral("idle"));
    // Skipped: an automatic check stays quiet about it; a check by hand still shows it.
    rig.bridge.check();
    REQUIRE(waitFor([&] { return rig.settled(); }, 30'000));
    rig.bridge.skipThisVersion();
    rig.bridge.setConfig(rig.bridge.config());
    rig.bridge.checkAutomatically(true);
    REQUIRE(waitFor([&] { return rig.settled(); }, 30'000));
    CHECK(rig.bridge.state() == QStringLiteral("available"));
    CHECK(announced.size() == 1);
    // No key built in: never asks.
    UpdateBridge::Config unsigned_ = rig.bridge.config();
    unsigned_.publicKeys.clear();
    rig.bridge.setConfig(unsigned_);
    CHECK_FALSE(rig.bridge.configured());
    const qsizetype before = rig.server.requests.size();
    rig.bridge.checkAutomatically(true);
    spin(200);
    CHECK(rig.server.requests.size() == before);
    resetSettings();
#endif
}

TEST_CASE("Updates: Help > Check for Updates and the dialog in the window", "[app][updates][qml]") {
#if !defined(_WIN32)
    SKIP("manifest signatures are checked with Windows CNG");
#else
    resetSettings();
    clearMessages();
    TestKey key;
    FakeServer server;
    QTemporaryDir folder;
    REQUIRE(folder.isValid());
    ReleaseSpec spec;
    spec.installer = fakeInstaller();
    spec.installerUrl = server.url(QStringLiteral("/x.msi"));
    server.serve(QStringLiteral("/x.msi"), spec.installer);
    const QByteArray payload = payloadOf(spec);
    server.serve(QStringLiteral("/latest/download/wabcut-update.json"), envelope(payload, key.sign(payload)));
    Launched launched;
    UpdateBridge::setLauncher([&](const QString&, const QStringList&) {
        ++launched.calls;
        return true;
    });

    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* updates = shell.engine.singletonInstance<UpdateBridge*>("Wabcut", "Updates");
    REQUIRE(updates != nullptr);
    UpdateBridge::Config c = updates->config();
    c.manifestUrl = server.url(QStringLiteral("/latest/download/wabcut-update.json"));
    c.publicKeys = {key.publicKey()};
    c.currentVersion = QStringLiteral("0.4.0");
    c.downloadFolder = folder.filePath(QStringLiteral("updates"));
    c.allowLoopbackHttp = true;
    c.installedByMsi = true;
    updates->setConfig(c);

    // Help > Check for Updates… opens the dialog and checks at once.
    QObject* help = shell.object("helpMenu");
    REQUIRE(help != nullptr);
    QObject* item = help->findChild<QObject*>(QStringLiteral("helpItem_updates"));
    REQUIRE(item != nullptr);
    REQUIRE(QMetaObject::invokeMethod(item, "triggered"));
    QObject* loader = shell.object("updateDialogLoader");
    REQUIRE(loader != nullptr);
    REQUIRE(waitFor([&] { return loader->property("item").value<QObject*>() != nullptr; }));
    QObject* dialog = loader->property("item").value<QObject*>();
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
    REQUIRE(waitFor([&] { return dialog->property("stage").toString() == QStringLiteral("available"); }, 30'000));
    auto* downloadButton = qobject_cast<QQuickItem*>(inDialog(dialog, "updateDownload"));
    REQUIRE(downloadButton != nullptr);
    CHECK(downloadButton->isVisible());
    auto* notes = qobject_cast<QQuickItem*>(inDialog(dialog, "updateNotes"));
    REQUIRE(notes != nullptr);
    CHECK(notes->property("text").toString().contains(QStringLiteral("Graphics card")));
    spin(300);
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/update-dialog-available.png")));
    shell.setAppearance(false, "ultraviolet");
    REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/update-dialog-available-light.png")));
    shell.setAppearance(true, "ultraviolet");
    REQUIRE(shell.settle(true, shell.themeColor("accentFill")));

    // Download and Install: verified, then the install waits for the quit.
    REQUIRE(QMetaObject::invokeMethod(downloadButton, "clicked"));
    REQUIRE(waitFor([&] { return dialog->property("stage").toString() == QStringLiteral("ready"); }, 60'000));
    spin(300);
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/update-dialog-ready.png")));
    shell.setAppearance(false, "ultraviolet");
    REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/update-dialog-ready-light.png")));
    auto* onQuit = qobject_cast<QQuickItem*>(inDialog(dialog, "updateInstallOnQuit"));
    REQUIRE(onQuit != nullptr);
    REQUIRE(QMetaObject::invokeMethod(onQuit, "clicked"));
    REQUIRE(waitFor([&] { return !dialog->property("opened").toBool(); }));
    CHECK(updates->installScheduled());
    // Opened again (Settings or the palette): it says when it installs.
    updates->openDialog(false);
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
    CHECK(dialog->property("stage").toString() == QStringLiteral("scheduled"));
    REQUIRE(QMetaObject::invokeMethod(dialog, "close"));
    REQUIRE(waitFor([&] { return !dialog->property("opened").toBool(); }));
    updates->cancelInstall();
    CHECK(launched.calls == 0);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
    UpdateBridge::setLauncher({});
    resetSettings();
#endif
}

TEST_CASE("Settings General: the Updates rows turn automatic checks on and open the update dialog", "[app][updates][settings][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* updates = shell.engine.singletonInstance<UpdateBridge*>("Wabcut", "Updates");
    REQUIRE(updates != nullptr);
    // No server: a check fails at once, which is all this test needs (nothing leaves the machine).
    UpdateBridge::Config c = updates->config();
    c.manifestUrl = QUrl(QStringLiteral("http://127.0.0.1:9/latest/download/wabcut-update.json"));
    c.allowLoopbackHttp = true;
    updates->setConfig(c);
    CHECK_FALSE(updates->automatic());

    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "openSettings", Q_ARG(QString, QStringLiteral("general")),
                                      Q_ARG(QString, QStringLiteral("checkUpdates"))));
    QObject* loader = shell.object("settingsWindowLoader");
    REQUIRE(loader != nullptr);
    QObject* window = loader->property("item").value<QObject*>();
    REQUIRE(window != nullptr);
    REQUIRE(waitFor([&] { return window->property("opened").toBool(); }));
    QQuickItem* page = nullptr;
    REQUIRE(waitFor([&] {
        REQUIRE(QMetaObject::invokeMethod(window, "page", Q_RETURN_ARG(QQuickItem*, page), Q_ARG(QString, QStringLiteral("general"))));
        return page != nullptr && page->isVisible();
    }));
    const std::function<QQuickItem*(QQuickItem*, const QString&)> find = [&](QQuickItem* root, const QString& name) -> QQuickItem* {
        if (!root)
            return nullptr;
        if (root->objectName() == name)
            return root;
        for (QQuickItem* child : root->childItems()) {
            if (QQuickItem* found = find(child, name))
                return found;
        }
        return nullptr;
    };
    QQuickItem* toggle = find(page, QStringLiteral("autoUpdateToggle"));
    QQuickItem* row = find(page, QStringLiteral("checkUpdatesRow"));
    QQuickItem* button = find(page, QStringLiteral("checkUpdatesButton"));
    REQUIRE(toggle != nullptr);
    REQUIRE(row != nullptr);
    REQUIRE(button != nullptr);
    CHECK_FALSE(toggle->property("checked").toBool());
    CHECK(row->property("label").toString() == QStringLiteral("Wabcut %1").arg(updates->currentVersion()));
    CHECK(row->property("description").toString() == QStringLiteral("Not checked for updates yet."));
    bool revealed = false;
    REQUIRE(QMetaObject::invokeMethod(window, "revealRow", Q_RETURN_ARG(bool, revealed), Q_ARG(QString, QStringLiteral("general")),
                                      Q_ARG(QString, QStringLiteral("autoUpdate"))));
    CHECK(revealed);

    // The switch: on, kept in the settings.
    REQUIRE(QMetaObject::invokeMethod(toggle, "toggle"));
    REQUIRE(QMetaObject::invokeMethod(toggle, "toggled"));
    CHECK(updates->automatic());
    CHECK(QSettings().value(QStringLiteral("updates/automatic")).toBool());
    spin(1500); // the reveal's flash fades
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/settings-updates.png")));
    shell.setAppearance(false, "ultraviolet");
    REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/settings-updates-light.png")));

    // Check for Updates…: Settings closes, the dialog opens and checks (here it cannot reach the server and says so).
    REQUIRE(QMetaObject::invokeMethod(button, "clicked"));
    REQUIRE(waitFor([&] { return !window->property("visible").toBool(); }));
    QObject* dialogLoader = shell.object("updateDialogLoader");
    REQUIRE(dialogLoader != nullptr);
    REQUIRE(waitFor([&] { return dialogLoader->property("item").value<QObject*>() != nullptr; }));
    QObject* dialog = dialogLoader->property("item").value<QObject*>();
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
    REQUIRE(waitFor([&] { return dialog->property("stage").toString() == QStringLiteral("failed"); }, 30'000));
    CHECK(updates->error().contains(QStringLiteral("Could not reach the update server")));
    CHECK_FALSE(updates->lastChecked().isEmpty());
    // Fix round 1: the failure is error-coloured TEXT (the light theme here), not the fill colour.
    QQuickItem* status = find(dialog->property("contentItem").value<QQuickItem*>(), QStringLiteral("updateStatus"));
    REQUIRE(status != nullptr);
    CHECK(sameColor(status->property("color").value<QColor>(), shell.themeColor("dangerText"), 0));
    REQUIRE(QMetaObject::invokeMethod(dialog, "close"));
    REQUIRE(waitFor([&] { return !dialog->property("opened").toBool(); }));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
    resetSettings();
}

// ---- Fix round 1 -----------------------------------------------------------------------------------------------------

TEST_CASE("Updates: a scheduled installer is held, checked again before it starts and kept by a new check",
          "[app][updates]") {
#if !defined(_WIN32)
    SKIP("manifest signatures are checked with Windows CNG");
#else
    resetSettings();
    TestKey key;
    Rig rig({key.publicKey()});
    ReleaseSpec spec;
    spec.installer = fakeInstaller();
    spec.installerUrl = rig.server.url(QStringLiteral("/v9.9.9.msi"));
    rig.server.serve(QStringLiteral("/v9.9.9.msi"), spec.installer);
    QByteArray payload = payloadOf(spec);
    rig.publish(envelope(payload, key.sign(payload)));
    rig.check();
    rig.download();
    REQUIRE(rig.bridge.state() == QStringLiteral("ready"));
    rig.bridge.installWhenClosed();
    REQUIRE(rig.bridge.installScheduled());
    const auto pending = UpdateBridge::pendingInstall();
    REQUIRE(pending.has_value());
    CHECK(pending->version == QStringLiteral("9.9.9"));
    CHECK(pending->size == spec.installer.size());
    CHECK(pending->sha256 == QCryptographicHash::hash(spec.installer, QCryptographicHash::Sha256));
    CHECK(UpdateBridge::checkInstaller(*pending).isEmpty());
    CHECK(rig.bridge.scheduledVersion() == QStringLiteral("9.9.9"));

    // Held: nobody can write, replace or delete it while Wabcut runs.
    QFile writer(pending->installer);
    CHECK_FALSE(writer.open(QIODevice::ReadWrite));
    CHECK_FALSE(QFile::remove(pending->installer));
    QFile reader(pending->installer);
    CHECK(reader.open(QIODevice::ReadOnly)); // reading is fine
    reader.close();

    // The script checks it once more after Wabcut has gone.
    QString program;
    const QString script = decodedScript(UpdateBridge::installerCommand(*pending, &program));
    CHECK(script.contains(QStringLiteral("Get-FileHash -LiteralPath $msi -Algorithm SHA256).Hash -eq '%1'")
                              .arg(QString::fromLatin1(pending->sha256.toHex().toUpper()))));
    CHECK(script.contains(QStringLiteral(".Length -eq %1").arg(spec.installer.size())));
    CHECK(script.contains(QStringLiteral("if ($ok) { Start-Process -FilePath ")));

    // A check again finds the same version: still ready and scheduled, the file untouched.
    rig.check();
    CHECK(rig.bridge.state() == QStringLiteral("ready"));
    CHECK(rig.bridge.installScheduled());
    CHECK(rig.files() == QStringList{QStringLiteral("wabcut-9.9.9-windows-x64.msi")});

    // A newer version: offered, the scheduled one stays until the new one is downloaded, which replaces it.
    ReleaseSpec newer;
    newer.version = QStringLiteral("9.9.10");
    newer.installer = fakeInstaller('k');
    newer.installerUrl = rig.server.url(QStringLiteral("/v9.9.10.msi"));
    rig.server.serve(QStringLiteral("/v9.9.10.msi"), newer.installer);
    payload = payloadOf(newer);
    rig.publish(envelope(payload, key.sign(payload)));
    rig.check();
    CHECK(rig.bridge.state() == QStringLiteral("available"));
    CHECK(rig.bridge.installScheduled());
    CHECK(rig.bridge.scheduledVersion() == QStringLiteral("9.9.9"));
    rig.download();
    REQUIRE(rig.bridge.state() == QStringLiteral("ready"));
    CHECK_FALSE(rig.bridge.installScheduled());
    CHECK(rig.files() == QStringList{QStringLiteral("wabcut-9.9.10-windows-x64.msi")});

    // The check before the start: a copy changed after its checks fails them (the real file cannot change: it is locked
    // from its verification on - fix round 2).
    rig.bridge.installWhenClosed();
    const auto changed = UpdateBridge::pendingInstall();
    REQUIRE(changed.has_value());
    UpdateBridge::PendingInstall copy = *changed;
    copy.installer = rig.folder.filePath(QStringLiteral("copy.msi"));
    {
        QFile file(copy.installer);
        REQUIRE(file.open(QIODevice::WriteOnly));
        file.write(newer.installer + QByteArrayLiteral("tampered"));
    }
    CHECK(UpdateBridge::checkInstaller(copy).contains(QStringLiteral("size")));
    {
        QFile file(copy.installer);
        REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QByteArray bytes = newer.installer;
        bytes[7] = static_cast<char>(bytes[7] ^ 1); // same size, other bytes: the checksum catches it
        file.write(bytes);
    }
    CHECK(UpdateBridge::checkInstaller(copy).contains(QStringLiteral("SHA-256")));
    // Another configuration calls a scheduled install off and lets go of the file.
    REQUIRE(rig.bridge.installScheduled());
    rig.bridge.setConfig(rig.bridge.config());
    CHECK_FALSE(rig.bridge.installScheduled());
    QFile freed(changed->installer);
    CHECK(freed.open(QIODevice::ReadWrite));
    freed.close();
    resetSettings();
#endif
}

TEST_CASE("Updates: a verified download stays ready after Later, a quit given up and a check again, locked meanwhile",
          "[app][updates]") {
#if !defined(_WIN32)
    SKIP("manifest signatures are checked with Windows CNG");
#else
    resetSettings();
    TestKey key;
    Rig rig({key.publicKey()});
    ReleaseSpec spec;
    spec.installer = fakeInstaller();
    spec.installerUrl = rig.server.url(QStringLiteral("/v9.9.9.msi"));
    rig.server.serve(QStringLiteral("/v9.9.9.msi"), spec.installer);
    QByteArray payload = payloadOf(spec);
    rig.publish(envelope(payload, key.sign(payload)));
    const auto downloads = [&] { return rig.server.requests.count(QStringLiteral("/v9.9.9.msi")); };
    const auto checks = [&] { return rig.server.requests.count(QStringLiteral("/latest/download/wabcut-update.json")); };
    rig.check();
    rig.download();
    REQUIRE(rig.bridge.state() == QStringLiteral("ready"));
    REQUIRE(downloads() == 1);
    const QString msi = QDir(rig.folder.filePath(QStringLiteral("updates"))).filePath(QStringLiteral("wabcut-9.9.9-windows-x64.msi"));

    // Locked as soon as it passed its checks, scheduled or not (fix round 2): no writing, no deleting, reading is fine.
    CHECK_FALSE(rig.bridge.installScheduled());
    QFile writer(msi);
    CHECK_FALSE(writer.open(QIODevice::ReadWrite));
    CHECK_FALSE(QFile::remove(msi));
    QFile reader(msi);
    CHECK(reader.open(QIODevice::ReadOnly));
    reader.close();

    // Later, then Help > Check for Updates... (or Settings' button): nothing is asked again, the download is ready.
    const qsizetype checked = checks();
    rig.bridge.openDialog(true);
    spin(200);
    CHECK(checks() == checked);
    CHECK(rig.bridge.state() == QStringLiteral("ready"));

    // A check that does run (an automatic one, Try Again): the same release finds the same verified file - ready.
    rig.check();
    CHECK(checks() == checked + 1);
    CHECK(rig.bridge.state() == QStringLiteral("ready"));
    CHECK(downloads() == 1);
    CHECK(rig.files() == QStringList{QStringLiteral("wabcut-9.9.9-windows-x64.msi")});

    // Quit and Install given up (the install called off), then a check again: still ready, still not downloaded twice.
    rig.bridge.installWhenClosed();
    REQUIRE(rig.bridge.installScheduled());
    rig.bridge.cancelInstall();
    rig.check();
    CHECK(rig.bridge.state() == QStringLiteral("ready"));
    CHECK(downloads() == 1);
    CHECK_FALSE(writer.open(QIODevice::ReadWrite)); // still locked: the verified download is kept
    rig.bridge.installWhenClosed();
    CHECK(rig.bridge.installScheduled());
    rig.bridge.cancelInstall();

    // The release was re-published with another installer: not the verified one, so it is offered for download.
    ReleaseSpec rebuilt = spec;
    rebuilt.installer = fakeInstaller('q');
    rig.server.serve(QStringLiteral("/v9.9.9.msi"), rebuilt.installer);
    payload = payloadOf(rebuilt);
    rig.publish(envelope(payload, key.sign(payload)));
    rig.check();
    CHECK(rig.bridge.state() == QStringLiteral("available"));
    // Downloading it lets go of the old file and replaces it.
    rig.download();
    REQUIRE(rig.bridge.state() == QStringLiteral("ready"));
    CHECK(downloads() == 2);
    QFile replaced(msi);
    REQUIRE(replaced.open(QIODevice::ReadOnly));
    CHECK(replaced.readAll() == rebuilt.installer);
    resetSettings();
#endif
}

TEST_CASE("Updates: Cancel while the signature is checked, and an older check's late answer, change nothing",
          "[app][updates]") {
#if !defined(_WIN32)
    SKIP("manifest signatures are checked with Windows CNG");
#else
    resetSettings();
    // A signature check that waits until the test lets it answer (it runs on a worker thread).
    std::array<std::atomic<bool>, 4> gates{};
    std::atomic<int> started{0};
    UpdateBridge::setAuthenticodeCheckForTesting([&](const QString&) {
        const int index = started.fetch_add(1);
        while (index < static_cast<int>(gates.size()) && !gates[static_cast<std::size_t>(index)].load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return update::AuthenticodeResult{true, QStringLiteral("Wabcut Test Signer"), {}};
    });
    struct Restore {
        std::array<std::atomic<bool>, 4>& open;
        ~Restore() {
            for (auto& g : open)
                g = true;
            QThreadPool::globalInstance()->waitForDone(10'000); // no check may outlive the gates it reads
            UpdateBridge::setAuthenticodeCheckForTesting({});
        }
    } restore{gates};

    TestKey key;
    Rig rig({key.publicKey()});
    ReleaseSpec spec;
    spec.installer = fakeInstaller();
    spec.installerUrl = rig.server.url(QStringLiteral("/x.msi"));
    spec.authenticode = true;
    spec.publisher = QStringLiteral("Wabcut Test Signer");
    rig.server.serve(QStringLiteral("/x.msi"), spec.installer);
    const QByteArray payload = payloadOf(spec);
    rig.publish(envelope(payload, key.sign(payload)));
    rig.check();
    REQUIRE(rig.bridge.state() == QStringLiteral("available"));

    // Downloaded; its signature check waits. Cancel works now (it used to do nothing here).
    rig.bridge.download();
    REQUIRE(waitFor([&] { return started.load() == 1; }, 30'000));
    CHECK(rig.bridge.state() == QStringLiteral("downloading"));
    rig.bridge.cancel();
    CHECK(rig.bridge.state() == QStringLiteral("available"));
    CHECK(rig.files().isEmpty()); // the unverified file went
    // Its late answer is ignored.
    gates[0] = true;
    spin(300);
    CHECK(rig.bridge.state() == QStringLiteral("available"));

    // An older check answering during a newer download.
    rig.bridge.download();
    REQUIRE(waitFor([&] { return started.load() == 2; }, 30'000));
    rig.bridge.cancel();
    rig.bridge.download(); // the newer one
    REQUIRE(waitFor([&] { return started.load() == 3; }, 30'000));
    gates[1] = true; // the cancelled one answers "trusted" now: it must not make the newer one ready
    spin(300);
    CHECK(rig.bridge.state() == QStringLiteral("downloading"));
    gates[2] = true;
    REQUIRE(waitFor([&] { return rig.bridge.state() == QStringLiteral("ready"); }));
    CHECK(rig.files() == QStringList{QStringLiteral("wabcut-9.9.9-windows-x64.msi")});

    // The check right before the installer starts uses the same signature check (and the pinned signer).
    rig.bridge.installWhenClosed();
    const auto pending = UpdateBridge::pendingInstall();
    REQUIRE(pending.has_value());
    CHECK(pending->authenticode);
    CHECK(pending->publisher == QStringLiteral("Wabcut Test Signer"));
    gates[3] = true;
    QThreadPool::globalInstance()->waitForDone(10'000);
    // A signature that no longer passes at the start: nothing is started.
    UpdateBridge::setAuthenticodeCheckForTesting(
        [](const QString&) { return update::AuthenticodeResult{false, {}, QStringLiteral("the file is not signed")}; });
    CHECK_FALSE(UpdateBridge::launchPendingInstall());
    CHECK(rig.launched.calls == 0);
    // It passes: started.
    UpdateBridge::setAuthenticodeCheckForTesting(
        [](const QString&) { return update::AuthenticodeResult{true, QStringLiteral("Wabcut Test Signer"), {}}; });
    rig.bridge.installWhenClosed();
    REQUIRE(rig.bridge.installScheduled());
    CHECK(UpdateBridge::launchPendingInstall());
    CHECK(rig.launched.calls == 1);
    const QString script = decodedScript(rig.launched.arguments);
    CHECK(script.contains(QStringLiteral("$sig = Get-AuthenticodeSignature -LiteralPath $msi")));
    CHECK(script.contains(QStringLiteral("GetNameInfo('SimpleName', $false) -ceq 'Wabcut Test Signer'")));
    resetSettings();
#endif
}

TEST_CASE("Updates: Quit and Install, then staying open at Save changes, calls the install off; Later leaves a download",
          "[app][updates][qml]") {
#if !defined(_WIN32)
    SKIP("manifest signatures are checked with Windows CNG");
#else
    resetSettings();
    clearMessages();
    TestKey key;
    FakeServer server;
    QTemporaryDir folder;
    REQUIRE(folder.isValid());
    ReleaseSpec spec;
    spec.installer = fakeInstaller();
    spec.installerUrl = server.url(QStringLiteral("/x.msi"));
    server.serve(QStringLiteral("/x.msi"), spec.installer);
    const QByteArray payload = payloadOf(spec);
    server.serve(QStringLiteral("/latest/download/wabcut-update.json"), envelope(payload, key.sign(payload)));
    int launched = 0;
    UpdateBridge::setLauncher([&](const QString&, const QStringList&) {
        ++launched;
        return true;
    });

    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* updates = shell.engine.singletonInstance<UpdateBridge*>("Wabcut", "Updates");
    REQUIRE(updates != nullptr);
    UpdateBridge::Config c = updates->config();
    c.manifestUrl = server.url(QStringLiteral("/latest/download/wabcut-update.json"));
    c.publicKeys = {key.publicKey()};
    c.currentVersion = QStringLiteral("0.4.0");
    c.downloadFolder = folder.filePath(QStringLiteral("updates"));
    c.allowLoopbackHttp = true;
    c.installedByMsi = true;
    updates->setConfig(c);
    int closes = 0;
    QObject::connect(shell.window.get(), &QQuickWindow::closing, shell.window.get(), [&](QQuickCloseEvent*) { ++closes; });

    // Ready, in the dialog: Later is there (it used to be missing at this stage) and schedules nothing.
    updates->openDialog(true);
    QObject* loader = shell.object("updateDialogLoader");
    REQUIRE(loader != nullptr);
    REQUIRE(waitFor([&] { return loader->property("item").value<QObject*>() != nullptr; }));
    QObject* dialog = loader->property("item").value<QObject*>();
    REQUIRE(waitFor([&] { return dialog->property("stage").toString() == QStringLiteral("available"); }, 30'000));
    CHECK(inDialog(dialog, "updateNotesScrollBar") != nullptr); // the notes scroll like every text well
    updates->download();
    REQUIRE(waitFor([&] { return dialog->property("stage").toString() == QStringLiteral("ready"); }, 60'000));
    auto* later = qobject_cast<QQuickItem*>(inDialog(dialog, "updateClose"));
    REQUIRE(later != nullptr);
    CHECK(later->isVisible());
    CHECK(later->property("text").toString() == QStringLiteral("Later"));
    // Background work running: the note is warning-coloured text that reads on light panels too.
    shell.setAppearance(false, "ultraviolet");
    REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
    dialog->setProperty("backgroundWorkRunning", true);
    auto* busy = qobject_cast<QQuickItem*>(inDialog(dialog, "updateBackgroundWork"));
    REQUIRE(busy != nullptr);
    CHECK(busy->isVisible());
    CHECK(sameColor(busy->property("color").value<QColor>(), shell.themeColor("warningText"), 0));
    dialog->setProperty("backgroundWorkRunning", false);
    REQUIRE(QMetaObject::invokeMethod(later, "clicked"));
    REQUIRE(waitFor([&] { return !dialog->property("opened").toBool(); }));
    CHECK_FALSE(updates->installScheduled());
    CHECK(updates->state() == QStringLiteral("ready"));

    // A titled project with unsaved changes: Quit and Install asks "Save changes?"; Cancel there keeps Wabcut open and
    // calls the install off (with a toast), instead of installing at some later quit.
    auto* file = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    REQUIRE(file != nullptr);
    REQUIRE(file->saveAs(QUrl::fromLocalFile(folder.filePath(QStringLiteral("Film.wabcut")))));
    QQmlExpression mark(qmlContext(shell.window.get()), shell.window.get(), QStringLiteral("Markers.add(false)"));
    mark.evaluate();
    REQUIRE_FALSE(mark.hasError());
    REQUIRE(waitFor([&] { return file->dirty(); }));
    QObject* unsaved = shell.object("unsavedChangesDialog");
    REQUIRE(unsaved != nullptr);
    updates->quitAndInstall();
    CHECK(updates->installScheduled());
    REQUIRE(waitFor([&] { return unsaved->property("visible").toBool(); }));
    CHECK(closes == 1);
    spin(600); // still asking: armed
    CHECK(updates->installScheduled());
    REQUIRE(QMetaObject::invokeMethod(unsaved, "close")); // Cancel / Esc
    REQUIRE(waitFor([&] { return !updates->installScheduled(); }, 5000));
    QQuickItem* toast = shell.item("toast");
    REQUIRE(toast != nullptr);
    CHECK(toast->property("text").toString().contains(QStringLiteral("the update was not installed")));
    CHECK(launched == 0);
    // Following the toast (Help > Check for Updates...): the verified download is ready at once, not downloaded again.
    const qsizetype manifestRequests = server.requests.count(QStringLiteral("/latest/download/wabcut-update.json"));
    QObject* help = shell.object("helpMenu");
    REQUIRE(help != nullptr);
    QObject* checkItem = help->findChild<QObject*>(QStringLiteral("helpItem_updates"));
    REQUIRE(checkItem != nullptr);
    REQUIRE(QMetaObject::invokeMethod(checkItem, "triggered"));
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
    spin(200);
    CHECK(dialog->property("stage").toString() == QStringLiteral("ready"));
    CHECK(server.requests.count(QStringLiteral("/latest/download/wabcut-update.json")) == manifestRequests);
    CHECK(server.requests.count(QStringLiteral("/x.msi")) == 1);
    REQUIRE(QMetaObject::invokeMethod(dialog, "close"));
    REQUIRE(waitFor([&] { return !dialog->property("opened").toBool(); }));

    // Saved (nothing asks): the close goes through and the install stays armed for main() to start.
    REQUIRE(file->save());
    REQUIRE_FALSE(file->dirty());
    updates->quitAndInstall();
    REQUIRE(waitFor([&] { return closes == 2; }));
    spin(600);
    CHECK(updates->installScheduled());
    updates->cancelInstall();
    UpdateBridge::setLauncher({});
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
    resetSettings();
#endif
}
