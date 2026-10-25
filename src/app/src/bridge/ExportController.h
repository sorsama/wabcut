#pragma once

// ExportController: the Export workspace's bridge to wabcut_export (QML singleton `ExportController`; owner:
// Export). It turns a preset + overrides into an exporter::ExportRequest on the current project snapshot,
// runs it on the export queue (one job at a time, off the GUI thread) and mirrors the queue into a list model
// with plain-language progress and errors. docs/export.md documents the API for the integration (top bar
// Export button, workspace switching).
//
// QML API:
//   presets                                 [{ id, name, description, icon }] (the six built-in presets)
//   jobs                                    ExportJobModel, oldest first
//   autoEncoders                            { h264, hevc, prores, dnxhr } -> encoder "auto" picks (after
//                                           probeEncoders(); "" until known)
//   queueExport(sequenceId, presetId, overrides) -> job id ("j1"), or "" with lastError set
//   describe(sequenceId, presetId, overrides)    -> what that export would write (see describe())
//   defaultOutputPath(sequenceId, presetId, overrides)
//   cancel(jobId) · cancelAll() · reveal(jobId) · clearFinished() · probeEncoders()
//   signals jobQueued(jobId), jobFinished(jobId, succeeded, message)
//   UX rework (L): job(jobId) -> the row as a map (for a toast) · openOutput(jobId) (the file in its default app);
//   the last choices, per user: lastPreset, lastGamingPreset, lastQuickPreset, lastFolder (+ lastFolderUrl), lastRange,
//   suggestedRange(sequenceId), forgetExportChoices(); defaultOutputPath() writes into lastFolder when it exists.
//
// `sequenceId` is ProjectController's opaque "s…" key ("" = the current sequence). `overrides`, all
// optional (docs/export.md has the table):
//   container    "mp4" | "mov" | "mkv" | "wav"          videoCodec   "h264" | "hevc" | "prores" | "dnxhr"
//   encoder      "auto" | "software" | <FFmpeg name>    quality      0..51 (CRF / CQ)
//   bitrateMbps  VBR target                             width, height (0 = the sequence size)
//   scaleMode    "fit" | "fill" | "stretch"             range        "whole" | "inOut" | "workArea"
//   video, audio bool                                   audioCodec   "aac" | "pcm16" | "pcm24"
//   sampleRate   Hz                                     audioBitrateKbps
//   output       local path or file URL (default: defaultOutputPath(); the extension follows the container)

#include "ProjectController.h"

#include <wabcut/ai/c2pa/ContentCredentials.h>

#include <wabcut/export/ExportCommandServices.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/export/ExportPresets.h>
#include <wabcut/export/WatchFolders.h>
#include <wabcut/ai/Chat.h>
#include <atomic>
#include <map>
#include <mutex>

#include <QAbstractListModel>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

/// The export queue as QML sees it. Times and rates are preformatted strings (principle 9).
class ExportJobModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by ExportController.jobs")
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged FINAL)
    Q_PROPERTY(int activeCount READ activeCount NOTIFY activeCountChanged FINAL)
    /// 0.4: a summary of the queue (summaryChanged). runningCount: jobs that run without the user - running or queued
    /// (not held, not paused). pausedCount: paused while running. heldCount: sent to the queue, waiting for Start Queue.
    Q_PROPERTY(int runningCount READ runningCount NOTIFY summaryChanged FINAL)
    Q_PROPERTY(int pausedCount READ pausedCount NOTIFY summaryChanged FINAL)
    Q_PROPERTY(int heldCount READ heldCount NOTIFY summaryChanged FINAL)
    /// The current job: the running one, else the first paused one, else the first queued one ("" / -1 when none).
    Q_PROPERTY(QString currentJobId READ currentJobId NOTIFY summaryChanged FINAL)
    Q_PROPERTY(QString currentTitle READ currentTitle NOTIFY summaryChanged FINAL)
    /// "running" | "paused" | "queued" | ""
    Q_PROPERTY(QString currentState READ currentState NOTIFY summaryChanged FINAL)
    /// 0..1, -1 when there is no current job
    Q_PROPERTY(double currentProgress READ currentProgress NOTIFY summaryChanged FINAL)
    /// The current row's etaText ("0:42 left"; "" until known)
    Q_PROPERTY(QString currentEta READ currentEta NOTIFY summaryChanged FINAL)

public:
    enum Role {
        JobIdRole = Qt::UserRole + 1, ///< "j1"
        TitleRole,                    ///< output file name
        OutputPathRole,               ///< native path
        PresetNameRole,
        StateRole,     ///< "jobState": "held" | "queued" | "running" | "paused" | "done" | "failed" | "cancelled"
        PhaseTextRole, ///< "Queued", "Preparing", "Rendering", "Finishing", "Done", ...
        ProgressRole,  ///< 0..1
        FpsTextRole,   ///< "212 fps" ("" until known)
        EtaTextRole,   ///< "0:12 left" ("" until known)
        EncoderRole,   ///< encoder actually used, e.g. "h264_nvenc"
        DetailRole,    ///< done: "0:10 · 5.7× realtime"; failed: the plain-language error
        FinishedRole,  ///< done, failed or cancelled
        /// UX rework (L), "origin": what queued it - "export" (the Export workspace, queueExport), "quick" (Quick
        /// Export), "queue" (Send to Media Encoder), "transcode" (Media Browser Ingest), "watch" (a watch folder) or
        /// "command" (the export.* commands, queueRequest).
        OriginRole,
        /// 0.4, "errorKind": what stopped a failed job (ExportController::errorKindOf): "offline", "space", "access",
        /// "folder", "encoder" or "" (none, or another problem).
        ErrorKindRole,
    };
    Q_ENUM(Role)

    struct Row {
        QString jobId;
        exporter::ExportJobId queueId = 0;
        QString title;
        QString outputPath;
        QString presetName;
        QString state = QStringLiteral("queued");
        QString phaseText;
        double progress = 0.0;
        QString fpsText;
        QString etaText;
        QString encoder;
        QString detail;
        bool finished = false;
        bool held = false; ///< 0.2: sent to the queue, waiting for Start Queue (Media Encoder's "Ready")
        QString origin = QStringLiteral("command"); ///< UX rework (L): OriginRole
        QString errorKind;                          ///< 0.4: ErrorKindRole
    };

    explicit ExportJobModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int activeCount() const;
    int runningCount() const { return m_summary.running; }
    int pausedCount() const { return m_summary.paused; }
    int heldCount() const { return m_summary.held; }
    QString currentJobId() const { return m_summary.jobId; }
    QString currentTitle() const { return m_summary.title; }
    QString currentState() const { return m_summary.state; }
    double currentProgress() const { return m_summary.progress; }
    QString currentEta() const { return m_summary.eta; }

    void append(Row row);
    /// Applies a queue status to the row of `queueId`; returns the row (nullptr if unknown).
    const Row* update(exporter::ExportJobId queueId, const exporter::ExportJobStatus& status);
    const Row* find(const QString& jobId) const;
    void removeFinished();
    /// 0.2: drops the row of `queueId` if it has finished (export.queue.remove).
    void removeFinished(exporter::ExportJobId queueId);
    const std::vector<Row>& rows() const { return m_rows; }

signals:
    void countChanged();
    void activeCountChanged();
    /// 0.4: runningCount, pausedCount, heldCount or the current job changed.
    void summaryChanged();

private:
    struct Summary {
        int running = 0, paused = 0, held = 0;
        QString jobId, title, state, eta;
        double progress = -1.0;
        bool operator==(const Summary&) const = default;
    };
    /// 0.4: recomputes m_summary from m_rows; emits summaryChanged() only when it differs.
    void refreshSummary();

    std::vector<Row> m_rows;
    Summary m_summary;
};

class ExportController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    /// The project exports read (snapshot, current sequence). Defaults to the ProjectController singleton of
    /// the controller's engine, looked up on first use.
    Q_PROPERTY(
        wabcut::app::ProjectController* project READ project WRITE setProject NOTIFY projectChanged FINAL)
    /// 0.2: built-in presets then the user's ({..., user: true, category}); changes after Save / Import / Delete Preset.
    Q_PROPERTY(QVariantList presets READ presets NOTIFY presetsChanged FINAL)
    /// 0.2 (Media Encoder's Watch Folders): [{ id, folder, preset, presetName, output, moveSources, queued, done, failed,
    /// lastError }]. Persisted (export/watchFolders).
    Q_PROPERTY(QVariantList watchFolders READ watchFolders NOTIFY watchFoldersChanged FINAL)
    /// 0.2: the CPU limit preference for exports, in percent of the logical processors (100 = no limit; 75, 50, 25).
    /// Persisted (export/cpuLimit).
    Q_PROPERTY(int cpuLimit READ cpuLimit WRITE setCpuLimit NOTIFY cpuLimitChanged FINAL)
    Q_PROPERTY(wabcut::app::ExportJobModel* jobs READ jobs CONSTANT FINAL)
    Q_PROPERTY(QVariantMap autoEncoders READ autoEncoders NOTIFY autoEncodersChanged FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged FINAL)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged FINAL)
    /// "Explorer" on Windows, "Finder" on macOS, "Files" elsewhere (for "Show in …" labels).
    Q_PROPERTY(QString fileManagerName READ fileManagerName CONSTANT FINAL)
    /// AI-15: signed C2PA Content Credentials in MP4 / MOV exports: "off", "ai" (exports with AI-generated material; the
    /// default) or "always". A signed export carries the manifest instead of the unsigned provenance sidecar; signing
    /// never fails an export (a warning in lastError, and the sidecar as before). Persisted; queueExport's override
    /// "contentCredentials" decides for one export.
    Q_PROPERTY(QString contentCredentials READ contentCredentials WRITE setContentCredentials NOTIFY contentCredentialsChanged FINAL)
    /// The C2PA library is in this build and loads.
    Q_PROPERTY(bool contentCredentialsAvailable READ contentCredentialsAvailable CONSTANT FINAL)
    /// 0.2 (Premiere's Export Settings > Content Credentials and its preferences): the producer's name in the manifest
    /// ("" = none), a thumbnail of the export in it (default on), and the request that generative AI not train on it.
    /// Persisted (export/c2paProducer, export/c2paThumbnail, export/c2paDoNotTrain).
    Q_PROPERTY(QString credentialsProducer READ credentialsProducer WRITE setCredentialsProducer NOTIFY contentCredentialsChanged FINAL)
    Q_PROPERTY(bool credentialsThumbnail READ credentialsThumbnail WRITE setCredentialsThumbnail NOTIFY contentCredentialsChanged FINAL)
    Q_PROPERTY(bool credentialsDoNotTrain READ credentialsDoNotTrain WRITE setCredentialsDoNotTrain NOTIFY contentCredentialsChanged FINAL)
    /// The signing certificate: a PEM chain (the signing certificate first) and its PEM private key; both empty = the
    /// per-user local identity (docs/export.md "Content Credentials"). Persisted (export/c2paCertificate, export/c2paKey).
    Q_PROPERTY(QString credentialsCertificate READ credentialsCertificate WRITE setCredentialsCertificate NOTIFY contentCredentialsChanged FINAL)
    Q_PROPERTY(QString credentialsKey READ credentialsKey WRITE setCredentialsKey NOTIFY contentCredentialsChanged FINAL)

    // ---- UX rework (L): export remembers the last choices (global, per user; QSettings export/*) ----
    /// The Export workspace's last built-in or user preset (export/lastPreset). Reads "match-source" when nothing is
    /// stored or the preset is gone; writing an unknown id is ignored.
    Q_PROPERTY(QString lastPreset READ lastPreset WRITE setLastPreset NOTIFY exportMemoryChanged FINAL)
    /// The Export workspace's last gaming preset (a GamingExport id; "" = a built-in or user preset was chosen last)
    /// (export/lastGamingPreset). Reads "" when the id is not a GamingExport preset (any more).
    Q_PROPERTY(QString lastGamingPreset READ lastGamingPreset WRITE setLastGamingPreset NOTIFY exportMemoryChanged FINAL)
    /// Quick Export's last preset (export/lastQuickPreset); "match-source" when none or gone.
    Q_PROPERTY(QString lastQuickPreset READ lastQuickPreset WRITE setLastQuickPreset NOTIFY exportMemoryChanged FINAL)
    /// The folder the last export with a chosen destination went to (export/lastFolder; native separators). Set by
    /// queueExport / quickExport / sendToQueue whenever their overrides carry an "output"; "" when none was chosen or
    /// the folder is gone. defaultOutputPath() writes there, else next to the project. Writable (a folder path or URL).
    Q_PROPERTY(QString lastFolder READ lastFolder WRITE setLastFolder NOTIFY exportMemoryChanged FINAL)
    /// lastFolder as a URL for FileDialog.currentFolder (empty when there is none).
    Q_PROPERTY(QUrl lastFolderUrl READ lastFolderUrl NOTIFY exportMemoryChanged FINAL)
    /// The range the user last chose in the Export workspace: "whole", "inOut", "workArea", or "" (never chosen; also
    /// what "custom" and junk store) (export/lastRange). QML writes it only when the user picks a range.
    Q_PROPERTY(QString lastRange READ lastRange WRITE setLastRange NOTIFY exportMemoryChanged FINAL)

public:
    explicit ExportController(QObject* parent = nullptr);
    ~ExportController() override;

    /// QML singleton factory: uses the ProjectController singleton of the same engine.
    static ExportController* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    // ---- QML API ----
    QVariantList presets() const;
    ExportJobModel* jobs() const { return m_jobs; }
    QVariantMap autoEncoders() const { return m_autoEncoders; }
    bool busy() const { return m_jobs->activeCount() > 0; }
    QString lastError() const { return m_lastError; }
    QString fileManagerName() const;
    QString contentCredentials() const { return m_contentCredentials; }
    void setContentCredentials(const QString& mode);
    bool contentCredentialsAvailable() const;
    QString credentialsProducer() const { return m_credentialsProducer; }
    void setCredentialsProducer(const QString& name);
    bool credentialsThumbnail() const { return m_credentialsThumbnail; }
    void setCredentialsThumbnail(bool on);
    bool credentialsDoNotTrain() const { return m_credentialsDoNotTrain; }
    void setCredentialsDoNotTrain(bool on);
    QString credentialsCertificate() const { return m_credentialsCertificate; }
    void setCredentialsCertificate(const QString& path);
    QString credentialsKey() const { return m_credentialsKey; }
    void setCredentialsKey(const QString& path);
    /// What signs Content Credentials: { local (the per-user identity), ready (it exists, or the certificate reads),
    /// subject, notAfter, error }. Never makes the local identity (the first signed export does).
    Q_INVOKABLE QVariantMap signingIdentity() const;

    Q_INVOKABLE QString queueExport(const QString& sequenceId, const QString& presetId,
                                    const QVariantMap& overrides = {});
    /// 0.2, Premiere's Quick Export: the current sequence with `presetId` to `output` ("" = the default name).
    /// UX rework (L): `overrides` (queueExport's keys, e.g. { range: "inOut" }) apply on top; `output` wins over theirs.
    Q_INVOKABLE QString quickExport(const QString& presetId, const QString& output = QString(),
                                    const QVariantMap& overrides = {});
    /// 0.2, Send to Media Encoder / several destinations: one job per preset, the same overrides; `hold` = they wait
    /// for Start Queue. Returns the job ids ([] with lastError on a problem: nothing is queued then).
    Q_INVOKABLE QStringList sendToQueue(const QString& sequenceId, const QStringList& presetIds,
                                        const QVariantMap& overrides = {}, bool hold = true);
    /// 0.2 (Media Encoder's queue): start a held job / every held job, pause a running or queued job, resume it.
    Q_INVOKABLE void start(const QString& jobId);
    Q_INVOKABLE void startQueue();
    Q_INVOKABLE void pause(const QString& jobId);
    Q_INVOKABLE void resume(const QString& jobId);
    /// 0.2, Save Preset: `presetId` + `overrides` under `name` -> { ok, id, error }.
    Q_INVOKABLE QVariantMap savePreset(const QString& name, const QString& description, const QString& presetId,
                                       const QVariantMap& overrides = {}, bool overwrite = false);
    /// 0.2, Import Preset (a file path or URL) -> { ok, id, name, error }.
    Q_INVOKABLE QVariantMap importPreset(const QString& file, bool overwrite = false);
    /// 0.2: writes a preset to a file others can import.
    Q_INVOKABLE bool exportPreset(const QString& id, const QString& file);
    Q_INVOKABLE bool deletePreset(const QString& id);
    /// 0.2, Watch Folders: { ok, id, error }.
    Q_INVOKABLE QVariantMap addWatchFolder(const QString& folder, const QString& presetId, const QString& output = QString(),
                                           bool moveSources = true);
    Q_INVOKABLE void removeWatchFolder(const QString& id);
    QVariantList watchFolders() const;
    int cpuLimit() const { return m_cpuLimit; }
    void setCpuLimit(int percent);
    /// What an export would write: { ok, error (plain language), container, videoCodec, format, resolution,
    /// frameRate, duration, frames, encoder, quality, audio, range, output,
    /// rangeAvailable: { whole, inOut, workArea } }.
    Q_INVOKABLE QVariantMap describe(const QString& sequenceId, const QString& presetId,
                                     const QVariantMap& overrides = {}) const;
    Q_INVOKABLE QString defaultOutputPath(const QString& sequenceId, const QString& presetId,
                                          const QVariantMap& overrides = {}) const;
    Q_INVOKABLE void cancel(const QString& jobId);
    Q_INVOKABLE void cancelAll();
    /// Shows the job's output in the file manager (the file selected where the platform allows it).
    Q_INVOKABLE bool reveal(const QString& jobId);
    Q_INVOKABLE void clearFinished();
    /// Probes which encoder "auto" picks per codec, in the background (autoEncodersChanged when done).
    Q_INVOKABLE void probeEncoders();

    // ---- UX rework (L) ----
    /// One job as a map, for a toast on jobFinished: { jobId, title, outputPath, outputUrl, folderUrl, presetName, state
    /// ("held" | "queued" | "running" | "paused" | "done" | "failed" | "cancelled"), detail, finished, origin (OriginRole),
    /// errorKind (0.4, ErrorKindRole), exists (the output file is on disk) }; {} for an unknown id (e.g. after
    /// clearFinished()).
    Q_INVOKABLE QVariantMap job(const QString& jobId) const;
    /// Opens the job's finished output in the system's default app for it ("Open"). False when the job is unknown,
    /// not done, or the file is gone. 0.4: `jobId` may also be the absolute path of an export's file (as reveal()):
    /// that file opens while it exists (after Clear Finished, say).
    Q_INVOKABLE bool openOutput(const QString& jobId);
    /// 0.4: what kind of problem stopped an export (the toast picks its action from it): "offline" (original media
    /// offline: Relink…), "space" (disk full), "access" (no permission), "folder" (the folder is gone), "encoder", or "".
    static QString errorKindOf(const Error& error);
    /// The range to preselect for `sequenceId` ("" = the current sequence) before the user picks one: lastRange when
    /// that range is available for the sequence, else "inOut" when it has In/Out marks, else "whole".
    Q_INVOKABLE QString suggestedRange(const QString& sequenceId) const;
    /// The folder of `path` (a file or folder path, or a file URL) as a URL, for FileDialog.currentFolder ("" -> {}).
    Q_INVOKABLE QUrl folderUrlOf(const QString& path) const;
    /// "Reset to defaults": forgets lastPreset, lastGamingPreset, lastQuickPreset, lastFolder and lastRange.
    Q_INVOKABLE void forgetExportChoices();
    QString lastPreset() const;
    void setLastPreset(const QString& id);
    QString lastGamingPreset() const;
    void setLastGamingPreset(const QString& id);
    QString lastQuickPreset() const;
    void setLastQuickPreset(const QString& id);
    QString lastFolder() const;
    void setLastFolder(const QString& folder);
    QUrl lastFolderUrl() const;
    QString lastRange() const;
    void setLastRange(const QString& range);
    /// Replaces "open the output" (tests must not launch apps). Receives the native output path.
    void setOpenHandler(std::function<bool(const QString&)> handler) { m_open = std::move(handler); }

    // ---- C++ API ----
    void setProject(ProjectController* project);
    ProjectController* project() const;
    /// The project file media paths resolve against (MediaLocator::relativePath); default output folder.
    void setProjectFile(std::filesystem::path file);
    /// Queues a fully built request (tests, integrations with their own settings). Returns the job id. `credentials`:
    /// "off", "ai" or "always" for this export ("" = the setting); a request with its own finalize step keeps it.
    QString queueRequest(exporter::ExportRequest request, const QString& presetName, const QString& credentials = QString());
    /// Tests: the signing identity of Content Credentials (default: the user's certificate when the settings name one,
    /// else the per-user local identity, its key in the OS keychain).
    void setCredentialsIdentityForTests(ai::c2pa::IdentityProvider identity) { m_testIdentity = std::move(identity); }
    /// Video source for jobs queued from now on (tests: the synthetic source). Empty = the rendered source.
    void setVideoSourceFactory(exporter::VideoSourceFactory factory) { m_videoSource = std::move(factory); }
    /// Replaces "show in file manager" (tests must not open windows). Receives the native output path.
    void setRevealHandler(std::function<bool(const QString&)> handler) { m_reveal = std::move(handler); }
    /// Builds the settings of `presetId` + `overrides` (InvalidArgument for unknown keys/values).
    static std::expected<exporter::ExportSettings, Error> settingsFor(const QString& presetId,
                                                                      const QVariantMap& overrides);
    /// The plain-language text the UI shows for an export error.
    static QString plainError(const Error& error);
    /// The text of the unsigned provenance sidecar written next to a finished export.
    static std::string provenanceSidecarText(const ai::Json& provenance);
    /// 0.2: the user's export presets ("Export Presets" in the app's config folder; tests point it elsewhere).
    static std::shared_ptr<exporter::PresetStore> userPresetStore();
    static void setUserPresetFolder(const std::filesystem::path& folder);
    /// 0.2: the export.* commands' services over this controller's queue (the assistant and the command palette).
    std::shared_ptr<exporter::ExportCommandServices> commandServices();
    /// Queues a request under a job id reserved before (the export commands). GUI thread. `origin`: OriginRole.
    QString queueRequestAs(const QString& jobId, exporter::ExportRequest request, const QString& presetName, bool hold,
                           const QString& credentials = QString(), const QString& origin = QStringLiteral("command"));
    /// 0.2 Media Browser Ingest > Transcode: queues `item` alone (its whole media, at its own size and rate: a project
    /// of its own, as a watch folder makes) with `presetId` into "<folder>/<name><extension>" (numbered when taken).
    /// Returns the job id and the output, or the error (InvalidArgument: preset; NotFound: the folder).
    struct Transcode {
        QString jobId;
        std::filesystem::path output;
    };
    std::expected<Transcode, Error> queueTranscode(const MediaItem& item, const QString& presetId,
                                                   const std::filesystem::path& folder);

signals:
    void projectChanged();
    void jobQueued(const QString& jobId);
    void jobFinished(const QString& jobId, bool succeeded, const QString& message);
    void autoEncodersChanged();
    void busyChanged();
    void lastErrorChanged();
    void contentCredentialsChanged();
    void presetsChanged();
    void watchFoldersChanged();
    void cpuLimitChanged();
    /// UX rework (L): lastPreset, lastGamingPreset, lastQuickPreset, lastFolder or lastRange changed.
    void exportMemoryChanged();

private:
    struct Relay;
    /// queueExport with the row's origin (OriginRole).
    QString queueExportAs(const QString& sequenceId, const QString& presetId, const QVariantMap& overrides,
                          const QString& origin);
    void onStatus(const exporter::ExportJobStatus& status);
    void setLastError(const QString& text);
    SequencePtr sequenceFor(const QString& sequenceId) const;
    /// 0.2: QML's extra override keys made into settings keys: rangeIn / rangeOut (timecodes in the sequence's rate) into
    /// customRange, file URLs of the effects into paths.
    std::expected<QVariantMap, Error> preparedOverrides(const QVariantMap& overrides, const Sequence* sequence) const;
    QString newJobId();
    exporter::ExportJobId queueIdOf(const QString& jobId) const;
    void saveWatchFolders() const;
    ai::c2pa::IdentityProvider credentialsIdentity() const;

    mutable QPointer<ProjectController> m_project;
    std::filesystem::path m_projectFile;
    ExportJobModel* m_jobs = nullptr;
    QVariantMap m_autoEncoders;
    QString m_lastError;
    exporter::VideoSourceFactory m_videoSource;
    std::function<bool(const QString&)> m_reveal;
    std::function<bool(const QString&)> m_open; ///< UX rework (L): openOutput's handler (tests)
    std::atomic<std::uint64_t> m_nextJob{1};
    mutable std::mutex m_idsMutex; ///< 0.2: job ids <-> queue ids and the project file, read by the command thread
    std::map<QString, exporter::ExportJobId> m_queueIds;
    std::filesystem::path m_sharedProjectFile;
    int m_cpuLimit = 100;
    std::map<exporter::ExportJobId, ai::Json> m_provenance;
    QString m_contentCredentials = QStringLiteral("ai");
    QString m_credentialsProducer;
    bool m_credentialsThumbnail = true;
    bool m_credentialsDoNotTrain = false;
    QString m_credentialsCertificate;
    QString m_credentialsKey;
    std::map<exporter::ExportJobId, std::shared_ptr<std::atomic<bool>>> m_signed; ///< set when the job signed its file
    ai::c2pa::IdentityProvider m_testIdentity;
    std::shared_ptr<Relay> m_relay; // the queue's listener posts through it; cut in the destructor
    std::unique_ptr<exporter::ExportQueue> m_queue;
    std::shared_ptr<exporter::WatchFolderService> m_watch;              ///< 0.2: after the queue (it queues there)
    std::shared_ptr<exporter::ExportCommandServices> m_commandServices; ///< 0.2: made on first use
    std::jthread m_probe;
};

} // namespace wabcut::app
