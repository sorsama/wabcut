#pragma once

// Updates (singleton `Updates`; docs/release.md, "Updates"): opt-in update checks and installs.
//
//   * Off by default. Settings > General > Updates turns the daily check on ("updates/automatic"); Help > Check for
//     Updates... checks at once either way (UpdateDialog.qml).
//   * A check fetches the version manifest of the latest GitHub release (WABCUT_UPDATE_RELEASES_URL +
//     "/latest/download/wabcut-update.json"), verifies its ECDSA P-256 signature against the public key(s) built into
//     the app (WABCUT_UPDATE_PUBLIC_KEY; UpdateManifest.h) and compares versions. Nothing about the user or the projects
//     is sent: a plain GET with a "Wabcut/<version>" user agent.
//   * Windows, installed with the MSI: the installer is downloaded to the app data folder ("updates"), its size and
//     SHA-256 must match the manifest, and its Authenticode signature (and signer) must be trusted when the manifest says
//     it is signed. Then, with the user's consent, it installs as an MSI major upgrade after Wabcut has closed - never
//     during an export: "Install when Wabcut closes", or "Quit and Install" through the window's normal close (which asks
//     about unsaved changes and running exports). main() starts it after the event loop (launchPendingInstall()): a
//     hidden PowerShell waits for this process to end, runs msiexec /passive into the same folder (keeping the .wabcut
//     association) and starts Wabcut again.
//   * The portable ZIP, Linux and macOS: the dialog says a version is out and opens its release page (on Linux and macOS
//     nothing can verify the manifest, so it only ever links to the fixed release page of the announced version).
//
// Tests replace the configuration (a loopback fake server, their own key, a temporary folder) and the launcher, so no
// installer ever runs.

#include "update/UpdateCrypto.h"
#include "update/UpdateManifest.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace wabcut::app {

class UpdateBridge : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Updates)
    QML_SINGLETON

    /// Check once a day by itself (off by default; "updates/automatic").
    Q_PROPERTY(bool automatic READ automatic WRITE setAutomatic NOTIFY automaticChanged FINAL)
    /// A public key is built in (a build without one never checks).
    Q_PROPERTY(bool configured READ configured NOTIFY configChanged FINAL)
    /// "idle", "checking", "upToDate", "available", "downloading", "ready", "failed".
    Q_PROPERTY(QString state READ state NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString currentVersion READ currentVersion NOTIFY configChanged FINAL)
    /// The newest version the last check found ("" before one).
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString released READ released NOTIFY stateChanged FINAL)
    /// What is new (Markdown); empty when the manifest could not be verified here.
    Q_PROPERTY(QString notes READ notes NOTIFY stateChanged FINAL)
    /// Downloading: 0..1, and "42 of 180 MB".
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged FINAL)
    Q_PROPERTY(QString progressText READ progressText NOTIFY progressChanged FINAL)
    /// "failed": why, in words.
    Q_PROPERTY(QString error READ error NOTIFY stateChanged FINAL)
    /// The found release can be downloaded and installed here (Windows, installed with the MSI, a verified manifest
    /// with an installer); else installHint says why and the release page is the way.
    Q_PROPERTY(bool canInstall READ canInstall NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString installHint READ installHint NOTIFY stateChanged FINAL)
    /// The downloaded installer runs when Wabcut closes.
    Q_PROPERTY(bool installScheduled READ installScheduled NOTIFY installScheduledChanged FINAL)
    /// The version that installs when Wabcut closes ("" when none is scheduled).
    Q_PROPERTY(QString scheduledVersion READ scheduledVersion NOTIFY installScheduledChanged FINAL)
    /// "26/10/2026 14:05", when updates were last checked ("" never).
    Q_PROPERTY(QString lastChecked READ lastChecked NOTIFY lastCheckedChanged FINAL)

public:
    struct Config {
        QUrl manifestUrl;
        QList<QByteArray> publicKeys;
        QString releasesBase; ///< "https://github.com/sorsama/wabcut/releases"
        QString currentVersion;
        QString downloadFolder;
        bool allowLoopbackHttp = false;
        /// Installed with the MSI (and so updatable by one); nullopt = ask Windows (HKCU\Software\Wabcut\Installer).
        std::optional<bool> installedByMsi;
        /// The install folder the upgrade keeps ("" = the remembered one).
        QString installRoot;
    };
    /// The built-in configuration (CMake: WABCUT_UPDATE_RELEASES_URL, WABCUT_UPDATE_PUBLIC_KEY).
    static Config defaultConfig();

    /// What runs after Wabcut has closed. The installer is checked again against what the signed manifest said - size,
    /// SHA-256 and, when it is signed, its Authenticode signer - right before it starts (checkInstaller(), and once more
    /// by the PowerShell that starts msiexec), and Wabcut keeps the file open without write or delete sharing until it
    /// exits, so nothing can swap it in the meantime.
    struct PendingInstall {
        QString installer;   ///< the verified .msi
        QString version;     ///< the version it installs
        qint64 size = 0;     ///< the manifest's size and SHA-256 (32 bytes) of the installer
        QByteArray sha256;
        bool authenticode = false; ///< its Authenticode signature must be trusted (by `publisher`, when not empty)
        QString publisher;
        QString installRoot; ///< INSTALL_ROOT for msiexec ("" = the MSI's default)
        bool associate = false; ///< keep the .wabcut file association
        QString relaunch;    ///< the program started after the install ("" = none)
        qint64 waitForPid = 0;
    };
    using Launcher = std::function<bool(const QString& program, const QStringList& arguments)>;
    using AuthenticodeCheck = std::function<update::AuthenticodeResult(const QString& file)>;

    explicit UpdateBridge(QObject* parent = nullptr);
    ~UpdateBridge() override;

    // ---- C++ (main.cpp, tests) ----
    void setConfig(Config config);
    const Config& config() const { return m_config; }
    /// main(): the daily check while `automatic` is on (the first one shortly after start).
    void startAutomaticChecks();
    /// An automatic check now (main's timer; tests): skips a version the user skipped, and does nothing when the last
    /// check is less than 20 hours old unless `force`.
    void checkAutomatically(bool force = false);
    /// The installer scheduled by installWhenClosed() / quitAndInstall() (process-wide; main() reads it at exit).
    static std::optional<PendingInstall> pendingInstall();
    /// Replaces how the installer is started (tests); an empty function restores QProcess::startDetached.
    static void setLauncher(Launcher launcher);
    /// Replaces the Authenticode check (tests: a check that waits, to cancel during it); empty: WinVerifyTrust. It runs
    /// on a worker thread.
    static void setAuthenticodeCheckForTesting(AuthenticodeCheck check);
    /// main(), after the event loop: starts the scheduled installer through the launcher. False when none is scheduled
    /// or it could not start.
    static bool launchPendingInstall();
    /// The program and arguments that run `install` (a hidden PowerShell: wait, check the file's size, SHA-256 and
    /// signature again, msiexec /passive, start Wabcut again).
    static QStringList installerCommand(const PendingInstall& install, QString* program);
    /// Whether `install`'s file is still what the manifest described: "" when it is, else why not (English).
    static QString checkInstaller(const PendingInstall& install);

    bool automatic() const;
    void setAutomatic(bool on);
    bool configured() const { return !m_config.publicKeys.isEmpty() && m_config.manifestUrl.isValid(); }
    QString state() const { return m_state; }
    QString currentVersion() const { return m_config.currentVersion; }
    QString latestVersion() const { return m_release ? m_release->version : QString(); }
    QString released() const { return m_release ? m_release->released : QString(); }
    QString notes() const { return m_release && m_release->verified ? m_release->notes : QString(); }
    double progress() const { return m_progress; }
    QString progressText() const;
    QString error() const { return m_error; }
    bool canInstall() const;
    QString installHint() const;
    bool installScheduled() const;
    QString scheduledVersion() const;
    QString lastChecked() const;
    const std::optional<update::UpdateRelease>& release() const { return m_release; }

    // ---- QML ----
    /// Checks now and keeps the result for the dialog (a skipped version is offered again).
    Q_INVOKABLE void check();
    /// check(), unless one runs or a download is under way, or a verified download is ready (the dialog shows that
    /// instead of asking again). Help > Check for Updates... and Settings use it.
    Q_INVOKABLE void checkIfNeeded();
    /// Downloads and verifies the found release's installer ("ready" when it passed every check).
    Q_INVOKABLE void download();
    /// Stops a check or a download.
    Q_INVOKABLE void cancel();
    /// The verified installer runs after Wabcut closes.
    Q_INVOKABLE void installWhenClosed();
    /// installWhenClosed(), then asks the window to close (quitRequested: the normal close, with its questions).
    Q_INVOKABLE void quitAndInstall();
    Q_INVOKABLE void cancelInstall();
    /// Automatic checks stay quiet about this version.
    Q_INVOKABLE void skipThisVersion();
    /// The release page of the found version (or of the latest one).
    Q_INVOKABLE void openReleasePage();
    /// Help > Check for Updates..., Settings: shows the dialog (openRequested), checking now when `checkNow`.
    Q_INVOKABLE void openDialog(bool checkNow);

signals:
    void automaticChanged();
    void configChanged();
    void stateChanged();
    void progressChanged();
    void installScheduledChanged();
    void lastCheckedChanged();
    void openRequested(bool checkNow);
    /// An automatic check found a version the user has not skipped (Main shows a toast).
    void updateAvailable(const QString& version);
    /// quitAndInstall(): Main closes the window the normal way.
    void quitRequested();

private:
    void startCheck(bool manual);
    void finishCheck(QNetworkReply* reply);
    void finishDownload(QNetworkReply* reply);
    void verifyDownloaded(const QString& file);
    void setState(const QString& state, const QString& error = {});
    void fail(const QString& error);
    QNetworkAccessManager* network();
    QString installerPath() const;
    bool detectInstalledByMsi() const;

    Config m_config;
    QNetworkAccessManager* m_network = nullptr;
    QPointer<QNetworkReply> m_reply;
    QTimer* m_timer = nullptr;
    QString m_state = QStringLiteral("idle");
    QString m_error;
    std::optional<update::UpdateRelease> m_release;
    bool m_manual = false;
    double m_progress = 0.0;
    qint64 m_received = 0;
    std::unique_ptr<QFile> m_file;
    std::unique_ptr<QCryptographicHash> m_hash;
    QString m_downloaded; ///< the verified installer ("ready")
    QString m_verifying;  ///< the installer whose signature is being checked
    /// The last installer that passed every check, and the release it was checked against: kept (and locked: no write
    /// or delete sharing) until a new download replaces it, so a check again, after Later or a quit given up, finds it
    /// ready instead of downloading it a second time.
    QString m_verifiedFile;
    std::optional<update::UpdateRelease> m_verifiedRelease;
    void* m_verifiedLock = nullptr; ///< Windows HANDLE on m_verifiedFile
    void keepVerified(const QString& file);
    void forgetVerified();
    /// Bumped by every check, download and cancel: a signature check that finishes for an older one is ignored.
    std::uint64_t m_generation = 0;
    mutable std::optional<bool> m_installedByMsi; ///< detectInstalledByMsi(), asked once
};

} // namespace wabcut::app
