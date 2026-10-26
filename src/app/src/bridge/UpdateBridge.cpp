#include "UpdateBridge.h"

#include "update/UpdateCrypto.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTimer>

#include <mutex>
#include <utility>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#ifndef WABCUT_VERSION
#define WABCUT_VERSION "0.0.0"
#endif
#ifndef WABCUT_UPDATE_RELEASES_URL
#define WABCUT_UPDATE_RELEASES_URL "https://github.com/sorsama/wabcut/releases"
#endif
#ifndef WABCUT_UPDATE_PUBLIC_KEY
#define WABCUT_UPDATE_PUBLIC_KEY ""
#endif

namespace wabcut::app {

namespace {

const QString kAutomaticKey = QStringLiteral("updates/automatic");
const QString kLastCheckKey = QStringLiteral("updates/lastCheck");
const QString kSkippedKey = QStringLiteral("updates/skipped");
constexpr qint64 kManifestLimit = 256 * 1024;
constexpr int kAutomaticIntervalHours = 20; // "once a day", whatever time Wabcut is started

std::mutex& pendingMutex() {
    static std::mutex m;
    return m;
}
std::optional<UpdateBridge::PendingInstall>& pending() {
    static std::optional<UpdateBridge::PendingInstall> p;
    return p;
}
UpdateBridge::Launcher& launcher() {
    static UpdateBridge::Launcher l;
    return l;
}
UpdateBridge::AuthenticodeCheck& authenticodeCheck() {
    static UpdateBridge::AuthenticodeCheck c;
    return c;
}
/// The installer's Authenticode signature (the test's check when one is set). Any thread.
update::AuthenticodeResult authenticode(const QString& file) {
    UpdateBridge::AuthenticodeCheck check;
    {
        std::lock_guard lock(pendingMutex());
        check = authenticodeCheck();
    }
    return check ? check(file) : update::checkAuthenticode(file);
}

/// `path` opened for reading without write or delete sharing (Windows): while it is open no other process can change,
/// replace or delete the file, and readers (the checks, msiexec's own read) still can. nullptr elsewhere or on failure.
void* lockFile(const QString& path) {
#if defined(Q_OS_WIN)
    HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(path).utf16()), GENERIC_READ,
                           FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    return h == INVALID_HANDLE_VALUE ? nullptr : h;
#else
    (void)path;
    return nullptr;
#endif
}

void unlockFile(void*& lock) {
#if defined(Q_OS_WIN)
    if (lock) {
        CloseHandle(static_cast<HANDLE>(lock));
    }
#endif
    lock = nullptr;
}

/// The scheduled installer's own lock, until Wabcut starts it at exit (or the install is called off). Under
/// pendingMutex().
void*& heldInstaller() {
    static void* h = nullptr;
    return h;
}

void holdInstaller(const QString& path) {
    unlockFile(heldInstaller());
    heldInstaller() = lockFile(path);
}

void releaseInstaller() {
    unlockFile(heldInstaller());
}

/// '...' for PowerShell (a single quote doubled).
QString psQuote(const QString& text) {
    QString escaped = text;
    escaped.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

QString megabytes(qint64 bytes) {
    return QLocale().toString(static_cast<double>(bytes) / (1024.0 * 1024.0), 'f', bytes < 10 * 1024 * 1024 ? 1 : 0);
}

QString systemRoot() {
    const QString root = qEnvironmentVariable("SystemRoot");
    return root.isEmpty() ? QStringLiteral("C:\\Windows") : QDir::toNativeSeparators(root);
}

} // namespace

UpdateBridge::Config UpdateBridge::defaultConfig() {
    Config c;
    c.releasesBase = QStringLiteral(WABCUT_UPDATE_RELEASES_URL);
    while (c.releasesBase.endsWith(QLatin1Char('/'))) {
        c.releasesBase.chop(1);
    }
    c.manifestUrl = QUrl(c.releasesBase + QStringLiteral("/latest/download/wabcut-update.json"));
    c.publicKeys = update::parsePublicKeys(QStringLiteral(WABCUT_UPDATE_PUBLIC_KEY));
    c.currentVersion = QStringLiteral(WABCUT_VERSION);
    c.downloadFolder = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/updates");
    return c;
}

UpdateBridge::UpdateBridge(QObject* parent) : QObject(parent), m_config(defaultConfig()) {}

UpdateBridge::~UpdateBridge() {
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
    }
    unlockFile(m_verifiedLock);
}

void UpdateBridge::keepVerified(const QString& file) {
    unlockFile(m_verifiedLock);
    m_verifiedFile = file;
    m_verifiedRelease = m_release;
    m_verifiedLock = lockFile(file); // from the moment it passed its checks: nothing can swap it before it is used
}

void UpdateBridge::forgetVerified() {
    unlockFile(m_verifiedLock);
    m_verifiedFile.clear();
    m_verifiedRelease.reset();
}

void UpdateBridge::setConfig(Config config) {
    cancel();
    cancelInstall(); // another configuration: what was scheduled under the old one goes
    forgetVerified();
    m_config = std::move(config);
    m_installedByMsi.reset();
    m_release.reset();
    m_downloaded.clear();
    setState(QStringLiteral("idle"));
    emit configChanged();
}

QNetworkAccessManager* UpdateBridge::network() {
    if (!m_network) {
        m_network = new QNetworkAccessManager(this);
        m_network->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy); // GitHub's latest/download redirects
    }
    return m_network;
}

bool UpdateBridge::automatic() const {
    return QSettings().value(kAutomaticKey, false).toBool();
}

void UpdateBridge::setAutomatic(bool on) {
    if (on == automatic()) {
        return;
    }
    QSettings().setValue(kAutomaticKey, on);
    emit automaticChanged();
    if (on && m_timer) {
        m_timer->start();
    }
}

QString UpdateBridge::lastChecked() const {
    const QDateTime when = QSettings().value(kLastCheckKey).toDateTime();
    return when.isValid() ? QLocale().toString(when.toLocalTime(), QLocale::ShortFormat) : QString();
}

QString UpdateBridge::progressText() const {
    if (!m_release || m_release->installerSize <= 0) {
        return {};
    }
    //: %1 and %2 are megabytes: "42 of 180 MB"
    return tr("%1 of %2 MB").arg(megabytes(m_received), megabytes(m_release->installerSize));
}

void UpdateBridge::setState(const QString& state, const QString& error) {
    if (state == m_state && error == m_error) {
        return;
    }
    m_state = state;
    m_error = error;
    emit stateChanged();
}

void UpdateBridge::fail(const QString& error) {
    m_file.reset();
    m_hash.reset();
    m_progress = 0.0;
    emit progressChanged();
    setState(QStringLiteral("failed"), error);
}

bool UpdateBridge::detectInstalledByMsi() const {
    if (m_config.installedByMsi) {
        return *m_config.installedByMsi;
    }
#if defined(Q_OS_WIN)
    // The MSI remembers its folder (cmake/packaging/wix/WabcutOptions.wxs); the program runs from <folder>\bin.
    const QSettings installer(QStringLiteral("HKEY_CURRENT_USER\\Software\\Wabcut\\Installer"), QSettings::NativeFormat);
    const QString root = QDir::cleanPath(QDir::fromNativeSeparators(installer.value(QStringLiteral("InstallRoot")).toString()));
    if (root.isEmpty() || root == QLatin1String(".")) {
        return false;
    }
    const QString bin = QDir::cleanPath(QCoreApplication::applicationDirPath());
    return bin.compare(root + QStringLiteral("/bin"), Qt::CaseInsensitive) == 0;
#else
    return false;
#endif
}

bool UpdateBridge::canInstall() const {
#if defined(Q_OS_WIN)
    if (!m_installedByMsi) {
        m_installedByMsi = detectInstalledByMsi();
    }
    return m_release && m_release->verified && !m_release->installerUrl.isEmpty() && *m_installedByMsi &&
           update::compareVersions(m_release->version, m_config.currentVersion) > 0;
#else
    return false;
#endif
}

QString UpdateBridge::installHint() const {
    if (!m_release || canInstall()) {
        return {};
    }
#if defined(Q_OS_WIN)
    if (m_release->installerUrl.isEmpty()) {
        return tr("This release has no Windows installer yet. Its page has the downloads.");
    }
    return tr("This copy of Wabcut was not installed with the installer (a portable copy or a build): download the new version from its release page.");
#else
    return tr("Download the new version from its release page.");
#endif
}

bool UpdateBridge::installScheduled() const {
    std::lock_guard lock(pendingMutex());
    return pending().has_value();
}

QString UpdateBridge::scheduledVersion() const {
    std::lock_guard lock(pendingMutex());
    return pending() ? pending()->version : QString();
}

void UpdateBridge::openDialog(bool checkNow) {
    emit openRequested(checkNow);
    if (checkNow) {
        checkIfNeeded();
    }
}

void UpdateBridge::check() {
    startCheck(true);
}

void UpdateBridge::checkIfNeeded() {
    if (m_state == QLatin1String("checking") || m_state == QLatin1String("downloading") ||
        (m_state == QLatin1String("ready") && QFileInfo::exists(m_downloaded))) {
        return; // the dialog shows what is under way, or the verified download, instead of asking again
    }
    check();
}

void UpdateBridge::checkAutomatically(bool force) {
    if (!configured() || (m_state != QLatin1String("idle") && m_state != QLatin1String("upToDate") &&
                          m_state != QLatin1String("failed"))) {
        return;
    }
    const QDateTime last = QSettings().value(kLastCheckKey).toDateTime();
    if (!force && last.isValid() && last.secsTo(QDateTime::currentDateTimeUtc()) < kAutomaticIntervalHours * 3600) {
        return;
    }
    startCheck(false);
}

void UpdateBridge::startAutomaticChecks() {
    if (!m_timer) {
        m_timer = new QTimer(this);
        m_timer->setSingleShot(false);
        connect(m_timer, &QTimer::timeout, this, [this] {
            m_timer->setInterval(60 * 60 * 1000); // after the first one: look every hour whether a day has passed
            if (automatic()) {
                checkAutomatically();
            }
        });
    }
    m_timer->setInterval(20 * 1000); // not in the way of the start-up
    m_timer->start();
}

void UpdateBridge::startCheck(bool manual) {
    if (m_state == QLatin1String("downloading")) {
        return;
    }
    if (m_reply) {
        QNetworkReply* previous = m_reply;
        m_reply = nullptr;
        previous->disconnect(this);
        previous->abort();
        previous->deleteLater();
    }
    ++m_generation;
    m_manual = manual;
    m_release.reset(); // a check that fails keeps nothing from an earlier one
    m_downloaded.clear();
    if (!configured()) {
        setState(QStringLiteral("failed"), tr("This build of Wabcut has no update key, so it cannot check for updates."));
        return;
    }
    setState(QStringLiteral("checking"));
    QNetworkRequest request(m_config.manifestUrl);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Wabcut/%1").arg(m_config.currentVersion));
    request.setRawHeader("Accept", "application/json");
    request.setTransferTimeout(30 * 1000);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    QNetworkReply* reply = network()->get(request);
    m_reply = reply;
    connect(reply, &QNetworkReply::downloadProgress, this, [reply](qint64 received, qint64) {
        if (received > kManifestLimit) {
            reply->setProperty("wabcutTooLarge", true);
            reply->abort(); // not a manifest
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] { finishCheck(reply); });
}

void UpdateBridge::finishCheck(QNetworkReply* reply) {
    reply->deleteLater();
    if (m_reply == reply) {
        m_reply = nullptr;
    }
    QSettings().setValue(kLastCheckKey, QDateTime::currentDateTimeUtc());
    emit lastCheckedChanged();
    if (reply->property("wabcutTooLarge").toBool()) {
        fail(tr("The update information is not readable."));
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        //: %1 is the network's own message
        fail(status == 404 ? tr("No update information was found for the latest release.")
                           : tr("Could not reach the update server: %1").arg(reply->errorString()));
        return;
    }
    const QByteArray body = reply->readAll();
    if (body.size() > kManifestLimit) {
        fail(tr("The update information is not readable."));
        return;
    }
    update::ManifestOptions options;
    options.publicKeys = m_config.publicKeys;
    options.acceptUnverified = !update::canVerifySignatures();
    options.allowLoopbackHttp = m_config.allowLoopbackHttp;
    auto release = update::readManifest(body, options);
    if (!release) {
        qWarning("Wabcut updates: refused the manifest: %s", qPrintable(release.error()));
        //: %1 says why, e.g. "the update information is not signed by Wabcut"
        fail(tr("The update was refused: %1.").arg(release.error()));
        return;
    }
    m_downloaded.clear();
    if (update::compareVersions(release->version, m_config.currentVersion) <= 0) {
        m_release = std::move(*release);
        setState(QStringLiteral("upToDate"));
        return;
    }
    const bool skipped = QSettings().value(kSkippedKey).toString() == release->version;
    m_release = std::move(*release);
    // This very installer is downloaded and verified already (and locked since; perhaps scheduled for the quit, perhaps
    // left with Later or a quit given up): it is ready again, without a second download.
    const auto same = [this](const update::UpdateRelease& r) {
        return r.version == m_release->version && r.installerSize == m_release->installerSize &&
               r.installerSha256 == m_release->installerSha256 && r.authenticode == m_release->authenticode &&
               r.publisher == m_release->publisher;
    };
    QString ready;
    if (m_verifiedRelease && same(*m_verifiedRelease) && QFileInfo::exists(m_verifiedFile)) {
        ready = m_verifiedFile;
    } else if (const auto scheduled = pendingInstall();
               scheduled && scheduled->version == m_release->version && scheduled->sha256 == m_release->installerSha256 &&
               QFileInfo::exists(scheduled->installer)) {
        ready = QDir::fromNativeSeparators(scheduled->installer);
    }
    if (!ready.isEmpty()) {
        m_downloaded = ready;
        m_progress = 1.0;
        emit progressChanged();
        setState(QStringLiteral("ready"));
        return;
    }
    setState(QStringLiteral("available"));
    if (!m_manual && !skipped) {
        emit updateAvailable(m_release->version);
    }
}

QString UpdateBridge::installerPath() const {
    return QDir(m_config.downloadFolder)
        .filePath(QStringLiteral("wabcut-%1-windows-x64.msi").arg(m_release ? m_release->version : QString()));
}

void UpdateBridge::download() {
    if (!canInstall() || m_state == QLatin1String("downloading")) {
        return;
    }
    if (m_state == QLatin1String("ready") && QFileInfo::exists(m_downloaded)) {
        return;
    }
    // A new download replaces what was scheduled and what was verified: the old installer is not run, and its file may go.
    cancelInstall();
    forgetVerified();
    ++m_generation;
    QDir().mkpath(m_config.downloadFolder);
    // Earlier downloads go: only the one being fetched stays.
    for (const QFileInfo& old : QDir(m_config.downloadFolder).entryInfoList({QStringLiteral("wabcut-*.msi*")}, QDir::Files)) {
        QFile::remove(old.absoluteFilePath());
    }
    m_file = std::make_unique<QFile>(installerPath() + QStringLiteral(".part"));
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        //: %1 is a folder
        fail(tr("Could not save the update in %1.").arg(QDir::toNativeSeparators(m_config.downloadFolder)));
        return;
    }
    m_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    m_received = 0;
    m_progress = 0.0;
    emit progressChanged();
    setState(QStringLiteral("downloading"));
    QNetworkRequest request(m_release->installerUrl);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Wabcut/%1").arg(m_config.currentVersion));
    request.setTransferTimeout(60 * 1000); // no data for a minute
    QNetworkReply* reply = network()->get(request);
    m_reply = reply;
    const qint64 expected = m_release->installerSize;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, expected] {
        if (!m_file || !m_hash) {
            return;
        }
        const QByteArray chunk = reply->readAll();
        m_received += chunk.size();
        if (m_received > expected) {
            reply->abort(); // finishDownload says why
            return;
        }
        m_hash->addData(chunk);
        if (m_file->write(chunk) != chunk.size()) {
            reply->abort();
            return;
        }
        m_progress = expected > 0 ? static_cast<double>(m_received) / static_cast<double>(expected) : 0.0;
        emit progressChanged();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] { finishDownload(reply); });
}

void UpdateBridge::finishDownload(QNetworkReply* reply) {
    reply->deleteLater();
    if (m_reply == reply) {
        m_reply = nullptr;
    }
    if (!m_file || !m_hash || !m_release) {
        return; // cancelled
    }
    const QString part = m_file->fileName();
    const bool wrote = m_file->error() == QFile::NoError;
    m_file->close();
    const auto discard = [&] { QFile::remove(part); };
    if (m_received > m_release->installerSize) {
        discard();
        fail(tr("The downloaded installer is larger than the update information says. It was deleted."));
        return;
    }
    if (reply->error() != QNetworkReply::NoError || !wrote) {
        discard();
        fail(wrote ? tr("The download stopped: %1").arg(reply->errorString())
                   : tr("Could not save the update in %1.").arg(QDir::toNativeSeparators(m_config.downloadFolder)));
        return;
    }
    if (m_received != m_release->installerSize) {
        discard();
        fail(tr("The downloaded installer is not the size the update information says. It was deleted."));
        return;
    }
    if (m_hash->result() != m_release->installerSha256) {
        discard();
        fail(tr("The downloaded installer does not match its SHA-256 checksum. It was deleted."));
        return;
    }
    m_hash.reset();
    m_file.reset();
    const QString msi = installerPath();
    QFile::remove(msi);
    if (!QFile::rename(part, msi)) {
        discard();
        fail(tr("Could not save the update in %1.").arg(QDir::toNativeSeparators(m_config.downloadFolder)));
        return;
    }
    verifyDownloaded(msi);
}

void UpdateBridge::verifyDownloaded(const QString& file) {
    if (!m_release->authenticode) {
        keepVerified(file);
        m_downloaded = file;
        m_progress = 1.0;
        emit progressChanged();
        setState(QStringLiteral("ready"));
        return;
    }
    // WinVerifyTrust hashes the whole file: off the GUI thread. Cancel (or a new check or download) while it runs bumps
    // the generation, and its answer is then ignored.
    QPointer<UpdateBridge> self(this);
    const QString publisher = m_release->publisher;
    const std::uint64_t generation = m_generation;
    m_verifying = file;
    QThreadPool::globalInstance()->start([self, file, publisher, generation] {
        const update::AuthenticodeResult result = authenticode(file);
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, file, publisher, result, generation] {
                if (!self || self->m_generation != generation || self->m_state != QLatin1String("downloading")) {
                    return;
                }
                self->m_verifying.clear();
                const bool rightSigner = publisher.isEmpty() || result.signer == publisher;
                if (!result.trusted || !rightSigner) {
                    QFile::remove(file);
                    self->fail(!result.trusted
                                   //: %1 says why, e.g. "the file is not signed"
                                   ? tr("The downloaded installer's signature is not valid (%1). It was deleted.").arg(result.error)
                                   //: %1 the certificate's name, %2 the expected one
                                   : tr("The downloaded installer is signed by \"%1\", not \"%2\". It was deleted.")
                                         .arg(result.signer, publisher));
                    return;
                }
                self->keepVerified(file);
                self->m_downloaded = file;
                self->m_progress = 1.0;
                emit self->progressChanged();
                self->setState(QStringLiteral("ready"));
            },
            Qt::QueuedConnection);
    });
}

void UpdateBridge::cancel() {
    ++m_generation; // a signature check still running is forgotten
    if (m_reply) {
        QNetworkReply* reply = m_reply;
        m_reply = nullptr;
        const bool downloading = m_state == QLatin1String("downloading");
        const QString part = m_file ? m_file->fileName() : QString();
        m_file.reset();
        m_hash.reset();
        reply->disconnect(this); // no finished handler: this is not a failure
        reply->abort();
        reply->deleteLater();
        if (!part.isEmpty()) {
            QFile::remove(part);
        }
        m_verifying.clear();
        m_progress = 0.0;
        emit progressChanged();
        setState(downloading && m_release ? QStringLiteral("available") : QStringLiteral("idle"));
        return;
    }
    if (m_state == QLatin1String("downloading")) {
        // Downloaded, its signature being checked: the check's answer is ignored, and the unverified file goes (now, or
        // with the next download if the check still has it open).
        if (!m_verifying.isEmpty()) {
            QFile::remove(m_verifying);
            m_verifying.clear();
        }
        m_progress = 0.0;
        emit progressChanged();
        setState(m_release ? QStringLiteral("available") : QStringLiteral("idle"));
    }
}

void UpdateBridge::installWhenClosed() {
    if (m_state != QLatin1String("ready") || !QFileInfo::exists(m_downloaded)) {
        return;
    }
    PendingInstall install;
    install.installer = QDir::toNativeSeparators(m_downloaded);
    install.version = m_release ? m_release->version : QString();
    install.size = m_release ? m_release->installerSize : 0;
    install.sha256 = m_release ? m_release->installerSha256 : QByteArray();
    install.authenticode = m_release && m_release->authenticode;
    install.publisher = m_release ? m_release->publisher : QString();
    QString root = m_config.installRoot;
#if defined(Q_OS_WIN)
    if (root.isEmpty()) {
        const QSettings installer(QStringLiteral("HKEY_CURRENT_USER\\Software\\Wabcut\\Installer"), QSettings::NativeFormat);
        root = installer.value(QStringLiteral("InstallRoot")).toString();
    }
    const QSettings association(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\.wabcut"), QSettings::NativeFormat);
    install.associate = association.value(QStringLiteral("Default")).toString() == QLatin1String("Wabcut.Project");
#endif
    while (root.endsWith(QLatin1Char('\\')) || root.endsWith(QLatin1Char('/'))) {
        root.chop(1);
    }
    install.installRoot = QDir::toNativeSeparators(root);
    install.relaunch = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    install.waitForPid = QCoreApplication::applicationPid();
    {
        std::lock_guard lock(pendingMutex());
        pending() = install;
        holdInstaller(install.installer);
    }
    emit installScheduledChanged();
}

void UpdateBridge::quitAndInstall() {
    installWhenClosed();
    if (installScheduled()) {
        emit quitRequested();
    }
}

void UpdateBridge::cancelInstall() {
    {
        std::lock_guard lock(pendingMutex());
        if (!pending()) {
            return;
        }
        pending().reset();
        releaseInstaller();
    }
    emit installScheduledChanged();
}

void UpdateBridge::skipThisVersion() {
    if (m_release) {
        QSettings().setValue(kSkippedKey, m_release->version);
    }
}

void UpdateBridge::openReleasePage() {
    // A verified manifest's page; otherwise only the fixed release page of the announced version (Linux, macOS).
    QUrl page = m_release && m_release->verified ? m_release->page : QUrl();
    if (page.isEmpty() && m_release) {
        page = update::releasePage(m_config.releasesBase, m_release->version);
    }
    if (page.isEmpty()) {
        page = QUrl(m_config.releasesBase + QStringLiteral("/latest"));
    }
    QDesktopServices::openUrl(page);
}

std::optional<UpdateBridge::PendingInstall> UpdateBridge::pendingInstall() {
    std::lock_guard lock(pendingMutex());
    return pending();
}

void UpdateBridge::setLauncher(Launcher l) {
    std::lock_guard lock(pendingMutex());
    launcher() = std::move(l);
}

void UpdateBridge::setAuthenticodeCheckForTesting(AuthenticodeCheck check) {
    std::lock_guard lock(pendingMutex());
    authenticodeCheck() = std::move(check);
}

QStringList UpdateBridge::installerCommand(const PendingInstall& install, QString* program) {
    QStringList msiArguments{QStringLiteral("'/i'"), QStringLiteral("('\"' + $msi + '\"')"), QStringLiteral("'/passive'"),
                             QStringLiteral("'/norestart'")};
    if (!install.installRoot.isEmpty()) {
        msiArguments << psQuote(QStringLiteral("INSTALL_ROOT=\"%1\"").arg(install.installRoot));
    }
    if (install.associate) {
        msiArguments << QStringLiteral("'WABCUT_ASSOCIATE=1'");
    }
    QStringList script;
    script << QStringLiteral("$ErrorActionPreference = 'SilentlyContinue'");
    if (install.waitForPid > 0) {
        script << QStringLiteral("Wait-Process -Id %1 -Timeout 120").arg(install.waitForPid);
    }
    script << QStringLiteral("$msi = %1").arg(psQuote(install.installer));
    // Once Wabcut has gone (and with it its lock on the file), the file is checked once more right before msiexec opens it.
    script << QStringLiteral("$ok = Test-Path -LiteralPath $msi");
    if (install.size > 0) {
        script << QStringLiteral("$ok = $ok -and ((Get-Item -LiteralPath $msi).Length -eq %1)").arg(install.size);
    }
    if (install.sha256.size() == 32) {
        script << QStringLiteral("$ok = $ok -and ((Get-FileHash -LiteralPath $msi -Algorithm SHA256).Hash -eq '%1')")
                      .arg(QString::fromLatin1(install.sha256.toHex().toUpper()));
    }
    if (install.authenticode) {
        script << QStringLiteral("$sig = Get-AuthenticodeSignature -LiteralPath $msi")
               << QStringLiteral("$ok = $ok -and ($sig.Status -eq 'Valid')");
        if (!install.publisher.isEmpty()) {
            script << QStringLiteral("$ok = $ok -and ($sig.SignerCertificate.GetNameInfo('SimpleName', $false) -ceq %1)")
                          .arg(psQuote(install.publisher));
        }
    }
    script << QStringLiteral("if ($ok) { Start-Process -FilePath %1 -ArgumentList @(%2) -Wait }")
                  .arg(psQuote(systemRoot() + QStringLiteral("\\System32\\msiexec.exe")), msiArguments.join(QStringLiteral(", ")));
    if (!install.relaunch.isEmpty()) {
        script << QStringLiteral("$app = %1").arg(psQuote(install.relaunch))
               << QStringLiteral("if (Test-Path -LiteralPath $app) { Start-Process -FilePath $app }");
    }
    const QString text = script.join(QLatin1Char('\n'));
    // -EncodedCommand: UTF-16LE in base64, so no path needs quoting on a command line.
    QByteArray utf16(reinterpret_cast<const char*>(text.utf16()), text.size() * 2);
    if (program) {
        *program = systemRoot() + QStringLiteral("\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");
    }
    return {QStringLiteral("-NoProfile"),   QStringLiteral("-NonInteractive"), QStringLiteral("-ExecutionPolicy"),
            QStringLiteral("Bypass"),       QStringLiteral("-WindowStyle"),    QStringLiteral("Hidden"),
            QStringLiteral("-EncodedCommand"), QString::fromLatin1(utf16.toBase64())};
}

QString UpdateBridge::checkInstaller(const PendingInstall& install) {
    QFile file(install.installer);
    if (!file.open(QIODevice::ReadOnly)) {
        return QStringLiteral("the installer is gone");
    }
    if (install.sha256.size() != 32 || install.size <= 0 || file.size() != install.size) {
        return QStringLiteral("the installer is not the size the update information says");
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file) || hash.result() != install.sha256) {
        return QStringLiteral("the installer does not match its SHA-256 checksum");
    }
    file.close();
    if (install.authenticode) {
        const update::AuthenticodeResult signature = authenticode(install.installer);
        if (!signature.trusted) {
            return QStringLiteral("the installer's signature is not valid (%1)").arg(signature.error);
        }
        if (!install.publisher.isEmpty() && signature.signer != install.publisher) {
            return QStringLiteral("the installer is signed by \"%1\", not \"%2\"").arg(signature.signer, install.publisher);
        }
    }
    return {};
}

bool UpdateBridge::launchPendingInstall() {
    std::optional<PendingInstall> install;
    Launcher run;
    {
        std::lock_guard lock(pendingMutex());
        install = std::exchange(pending(), std::nullopt);
        run = launcher();
    }
    // Still locked while it is checked (the lock lets readers in); let go only right before the script starts, which
    // checks it once more after Wabcut has gone.
    const auto letGo = [] {
        std::lock_guard lock(pendingMutex());
        releaseInstaller();
    };
    if (!install || !QFileInfo::exists(install->installer)) {
        letGo();
        return false;
    }
    if (const QString problem = checkInstaller(*install); !problem.isEmpty()) {
        letGo();
        qWarning("Wabcut updates: not installing %s: %s", qPrintable(install->installer), qPrintable(problem));
        return false;
    }
    letGo();
    QString program;
    const QStringList arguments = installerCommand(*install, &program);
    if (run) {
        return run(program, arguments);
    }
    return QProcess::startDetached(program, arguments);
}

} // namespace wabcut::app
