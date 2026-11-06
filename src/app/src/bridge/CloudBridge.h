#pragma once

// CloudBridge: review links and cloud projects on storage the user owns (0.7, docs/cloud.md), the QML singleton `Cloud`.
//
//   * Locations. A location is a folder a sync service mirrors (OneDrive, Dropbox, Google Drive, a share), a WebDAV collection or
//     an S3-compatible bucket, with a purpose: "projects" or "reviews" (a review folder is shared with strangers, so it never holds
//     a project). Settings are in QSettings (cloud/locations); passwords and keys are in the OS keychain (entry "cloud:<id>") and
//     never in a setting, a project, a log or a message. "Test" runs the conditional-write check a cloud project needs.
//   * A cloud project. The project stays an ordinary file; a sidecar `<project>.wabcut-cloud` links it to a copy on a "projects"
//     location. Upload (confirmed with its size), Open (from another computer), Sync now (push, pull, merge, with conflicts asked),
//     the editing lock (renewed every minute; a project somebody else is editing is read-only here), the revisions of the cloud
//     copy and the ones nothing reaches. Sync is the user's action (or after Save when the user turned that on).
//   * Review links. Render a review copy of the sequence (H.264, off the GUI thread, Cancel), see its size, then publish it with
//     the static review page to a "reviews" location (nothing is uploaded before that second step). Links can be revoked or
//     removed when expired. Comments reviewers send back are read from the shared folder or from a file and become review notes
//     in one undo step.
//   * Offline mode and the project's no-cloud lock stop every upload, publish and push - also to a folder, which a sync service
//     uploads - and, through the AI platform's transport, every request to WebDAV and S3.
//   * Off the GUI thread: every network call, every upload, the render. One job at a time; Cancel stops it.

#include <wabcut/cloud/CloudLock.h>
#include <wabcut/cloud/CloudProject.h>
#include <wabcut/cloud/RemoteStore.h>
#include <wabcut/cloud/ReviewLink.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/model/Merge.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

class AiController;
class QJSEngine;
class QQmlEngine;

namespace wabcut::ai {
class IHttpTransport;
class SecretStore;
} // namespace wabcut::ai

namespace wabcut::app {

class ProjectController;
class ProjectLifecycle;

/// One job off the GUI thread at a time. The work returns what to do on the GUI thread when it is done.
class CloudWorker {
public:
    using Finish = std::function<void()>;
    using Work = std::function<Finish(std::stop_token)>;
    ~CloudWorker();
    bool running() const { return m_running; }
    void cancel() { m_stop.request_stop(); }
    /// Cancels and waits (owners call it when something the worker uses goes away).
    void stop();
    /// False while another job runs. `synchronous` runs the work and its finish on the calling thread (tests).
    bool start(QObject* context, bool synchronous, Work work);

private:
    std::thread m_thread;
    std::stop_source m_stop;
    std::atomic<bool> m_running{false};
};

class CloudBridge : public QObject {
    Q_OBJECT

    /// [{ id, name, kind ("folder" | "webdav" | "s3"), kindName, purpose ("projects" | "reviews"), summary, hasSecret, folder, url,
    ///    user, endpoint, region, bucket, prefix, pathStyle, allowInsecure, mediaFolders, check ("" | "ok" | "problem"),
    ///    checkText }]
    Q_PROPERTY(QVariantList locations READ locations NOTIFY locationsChanged FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY jobChanged FINAL)
    /// What the running job is doing ("Uploading the review copy"), "" when idle.
    Q_PROPERTY(QString phase READ phase NOTIFY jobChanged FINAL)
    /// 0..1, or -1 when unknown.
    Q_PROPERTY(double progress READ progress NOTIFY jobChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)
    Q_PROPERTY(bool messageIsError READ messageIsError NOTIFY messageChanged FINAL)
    /// Why nothing may leave the computer now (offline mode / the project's no-cloud lock), "" when uploads are allowed.
    Q_PROPERTY(QString blockedReason READ blockedReason NOTIFY blockedChanged FINAL)
    /// The open project is saved to a file (links, cloud copies and review links need one).
    Q_PROPERTY(bool projectSaved READ projectSaved NOTIFY projectChanged FINAL)
    Q_PROPERTY(QString projectName READ projectName NOTIFY projectChanged FINAL)
    /// The open project's cloud copy: { linked, name, locationId, locationName, revision, syncedText, lock ("none" | "mine" | "other" |
    /// "unknown"), lockText, readOnly, autoSync, canSync, hint, revertGuard, conflictCount }
    Q_PROPERTY(QVariantMap project READ projectState NOTIFY cloudChanged FINAL)
    /// The conflicts a sync found, waiting for the user's choice: [{ id, what }].
    Q_PROPERTY(QVariantList conflicts READ conflicts NOTIFY cloudChanged FINAL)
    /// [{ projectId, name, revision, savedBy, savedText, label }] of the last refreshRemoteProjects().
    Q_PROPERTY(QVariantList remoteProjects READ remoteProjects NOTIFY remoteChanged FINAL)
    /// The cloud copy's revisions, newest first: [{ revision, savedBy, savedText, label, current, sizeText }], and the ones the
    /// current revision does not reach: [{ revision, savedBy, savedText }].
    Q_PROPERTY(QVariantList revisions READ revisions NOTIFY historyChanged FINAL)
    Q_PROPERTY(QVariantList unmerged READ unmerged NOTIFY historyChanged FINAL)
    /// The open project's review links: [{ id, title, createdText, expiresText, expired, locationName, kind, sizeText, url,
    /// canCopyLink, sequenceName, changed }].
    Q_PROPERTY(QVariantList links READ links NOTIFY linksChanged FINAL)
    /// The review copy: { state ("none" | "rendering" | "ready"), progress, path, bytes, sizeText, seconds, durationText, resolution,
    /// sequenceName, estimateText }.
    Q_PROPERTY(QVariantMap reviewCopy READ reviewCopy NOTIFY reviewCopyChanged FINAL)
    /// The last import of comments: { files, added, unchanged, markers, conflicts: [text], skipped, reviewers: [text] }.
    Q_PROPERTY(QVariantMap lastImport READ lastImport NOTIFY importChanged FINAL)

public:
    explicit CloudBridge(QObject* parent = nullptr);
    ~CloudBridge() override;
    static CloudBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    void setControllers(ProjectController* controller, ProjectLifecycle* lifecycle, AiController* ai);
    // ---- tests ----
    void setSynchronous(bool synchronous) { m_synchronous = synchronous; }
    void setTransportForTesting(ai::IHttpTransport* transport) { m_testTransport = transport; }
    void setSecretStoreForTesting(ai::SecretStore* store) { m_testSecrets = store; }
    void setRandomForTesting(cloud::RandomBytes random) { m_random = std::move(random); }
    void setClockForTesting(cloud::UnixClock clock) { m_clock = std::move(clock); }
    void setCacheFolderForTesting(std::filesystem::path folder) { m_cacheOverride = std::move(folder); }
    void setVideoSourceForTesting(exporter::VideoSourceFactory factory) { m_videoSource = std::move(factory); }
    /// Tests: replaces the render of the review copy (writes the file or fails).
    using RenderForTesting = std::function<std::expected<std::int64_t, Error>(const std::filesystem::path& output)>;
    void setRenderForTesting(RenderForTesting render) { m_testRender = std::move(render); }
    /// Skips the minute timer (tests drive heartbeat() themselves).
    void setHeartbeatEnabled(bool enabled);
    void heartbeat();

    QVariantList locations() const { return m_locationRows; }
    bool busy() const { return m_worker.running(); }
    QString phase() const { return m_phase; }
    double progress() const { return m_progress; }
    QString message() const { return m_message; }
    bool messageIsError() const { return m_messageIsError; }
    QString blockedReason() const;
    bool projectSaved() const;
    QString projectName() const;
    QVariantMap projectState() const { return m_cloudRow; }
    QVariantList conflicts() const { return m_conflictRows; }
    QVariantList remoteProjects() const { return m_remoteRows; }
    QVariantList revisions() const { return m_revisionRows; }
    QVariantList unmerged() const { return m_unmergedRows; }
    QVariantList links() const { return m_linkRows; }
    QVariantMap reviewCopy() const { return m_reviewCopy; }
    QVariantMap lastImport() const { return m_lastImport; }

    // ---- locations ----
    /// Adds a location (no `id`) or changes one (`id`): { name, kind, purpose, folder | url, user | endpoint, region, bucket,
    /// prefix, pathStyle, allowInsecure, mediaFolders, secretA, secretB }. A secret left empty keeps the stored one. Returns
    /// { ok, id, error }.
    Q_INVOKABLE QVariantMap saveLocation(const QVariantMap& spec);
    Q_INVOKABLE bool removeLocation(const QString& id);
    /// Checks a location off the GUI thread (reachable, may hold cloud projects); the answer is in `locations`.
    Q_INVOKABLE void testLocation(const QString& id);
    /// The media folders of a location (where this computer keeps the media of its cloud projects).
    Q_INVOKABLE void setMediaFolders(const QString& id, const QStringList& folders);

    // ---- the cloud project ----
    Q_INVOKABLE void refreshCloud();
    /// What uploading the open project sends: { ok, error, bytes, sizeText, destination, notes: [text] }.
    Q_INVOKABLE QVariantMap uploadPreview(const QString& locationId);
    Q_INVOKABLE void createCloudProject(const QString& locationId, const QString& name);
    Q_INVOKABLE void refreshRemoteProjects(const QString& locationId);
    /// Downloads a cloud project into `folder` (as "<name>.wabcut") and opens it.
    Q_INVOKABLE void openCloudProject(const QString& locationId, const QString& projectId, const QUrl& folder);
    Q_INVOKABLE void syncNow(bool sendAnyway = false);
    /// After a sync asked: "mine" / "theirs" for every conflict, or `choices` { conflictId: "mine" | "theirs" }.
    Q_INVOKABLE void resolveConflicts(const QString& mode, const QVariantMap& choices = {});
    Q_INVOKABLE void setAutoSync(bool on);
    Q_INVOKABLE bool unlinkProject();
    Q_INVOKABLE void refreshHistory();
    /// Saves a revision of the cloud copy as "<name> (cloud revision N).wabcut" in `folder` and opens it.
    Q_INVOKABLE void openRevisionAsCopy(int revision, const QUrl& folder);
    Q_INVOKABLE void takeLock();
    /// Looks for this project's media in the location's media folders (File > Link Media's search).
    Q_INVOKABLE int relinkMedia();
    Q_INVOKABLE void cancelJob();

    // ---- review links ----
    /// What a review copy of the current sequence would be: { ok, error, sequenceName, seconds, durationText, resolution, bytes,
    /// sizeText, range }. `options`: { resolution: 540 | 720 | 1080, range: "whole" | "inOut" | "workArea" }.
    Q_INVOKABLE QVariantMap estimateReviewCopy(const QVariantMap& options);
    Q_INVOKABLE void prepareReviewCopy(const QVariantMap& options);
    Q_INVOKABLE void discardReviewCopy();
    /// Uploads the prepared copy and the page: { locationId, title, expiryDays (0 = none), removeWhenExpired, instructions }.
    Q_INVOKABLE void publishReviewLink(const QVariantMap& options);
    Q_INVOKABLE void revokeLink(const QString& id);
    Q_INVOKABLE void removeExpiredLinks();
    /// Reads the comment files reviewers left in the link's folder.
    Q_INVOKABLE void collectComments(const QString& id, bool markers);
    /// Imports a comments file the user picked, for link `id`.
    Q_INVOKABLE void importCommentsFile(const QString& id, const QUrl& file, bool markers);
    Q_INVOKABLE bool copyLink(const QString& id);
    /// The local folder of a folder location's link (to share with the sync service), "" otherwise.
    Q_INVOKABLE QString linkFolder(const QString& id) const;

signals:
    void locationsChanged();
    void jobChanged();
    void messageChanged();
    void blockedChanged();
    void projectChanged();
    void cloudChanged();
    void remoteChanged();
    void historyChanged();
    void linksChanged();
    void reviewCopyChanged();
    void importChanged();
    /// A cloud project was opened or created (the dialog moves to its page).
    void cloudProjectReady();

private:
    struct Location;
    struct Secrets;
    class Impl;
    friend class Impl;

    // locations
    void loadLocations();
    void saveLocations() const;
    void rebuildLocationRows();
    Location* findLocation(const QString& id);
    const Location* findLocation(const QString& id) const;
    std::expected<std::unique_ptr<cloud::RemoteStore>, QString> openStore(const Location& location);
    QString kindName(const QString& kind) const;
    cloud::Progress progressSink(const QString& phase);
    ai::IHttpTransport& transport();
    ai::SecretStore& secretStore();
    std::optional<Secrets> readSecrets(const QString& id);
    bool writeSecrets(const QString& id, const Secrets& secrets);

    // state
    void setMessage(const QString& text, bool error = false);
    void setPhase(const QString& phase, double progress);
    void onDocumentChanged();
    void rebuildCloudRow();
    void rebuildLinkRows();
    bool start(const QString& phase, CloudWorker::Work work);
    cloud::Identity identity() const;
    std::filesystem::path cacheFolder() const;
    std::filesystem::path projectFile() const;
    void applySyncResult(cloud::SyncResult result, std::uint64_t startedAtRevision);
    cloud::SyncRequest syncRequest(bool sendAnyway) const;
    void startSync(cloud::SyncRequest request);
    void rememberSyncStep();
    bool syncStepIntact() const;
    void acquireLockAsync();
    void releaseLockNow();
    void updateReadOnly();
    void afterOpen(const std::filesystem::path& file, const QString& locationId);
    // review
    cloud::ReviewLinkRecord* findLink(const QString& id);
    void importComments(const std::vector<std::pair<QString, std::string>>& files, const cloud::ReviewLinkRecord& record, bool markers);

    QPointer<ProjectController> m_controller;
    QPointer<ProjectLifecycle> m_lifecycle;
    QPointer<AiController> m_ai;
    ai::IHttpTransport* m_testTransport = nullptr;
    ai::SecretStore* m_testSecrets = nullptr;
    cloud::RandomBytes m_random;
    cloud::UnixClock m_clock;
    std::filesystem::path m_cacheOverride;
    exporter::VideoSourceFactory m_videoSource;
    RenderForTesting m_testRender;
    bool m_synchronous = false;

    CloudWorker m_worker;
    QString m_phase;
    double m_progress = -1.0;
    QString m_message;
    bool m_messageIsError = false;
    std::unique_ptr<Impl> d;

    QVariantList m_locationRows;
    QVariantMap m_cloudRow;
    QVariantList m_conflictRows;
    QVariantList m_remoteRows;
    QVariantList m_revisionRows;
    QVariantList m_unmergedRows;
    QVariantList m_linkRows;
    QVariantMap m_reviewCopy;
    QVariantMap m_lastImport;
    QTimer m_beat;
};

/// The QML singleton `Cloud`. The bridge is default-constructible, so Qt would call its constructor and skip create();
/// the foreign gadget keeps create() (it wires the project, lifecycle and AI controllers) the way the other bridges do.
struct CloudBridgeForeign {
    Q_GADGET
    QML_FOREIGN(CloudBridge)
    QML_NAMED_ELEMENT(Cloud)
    QML_SINGLETON
public:
    static CloudBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return CloudBridge::create(qmlEngine, jsEngine); }
};

} // namespace wabcut::app
