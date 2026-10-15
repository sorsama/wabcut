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

#include <wabcut/export/ExportJob.h>
#include <wabcut/ai/Chat.h>
#include <map>

#include <QAbstractListModel>
#include <QObject>
#include <QPointer>
#include <QString>
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

public:
    enum Role {
        JobIdRole = Qt::UserRole + 1, ///< "j1"
        TitleRole,                    ///< output file name
        OutputPathRole,               ///< native path
        PresetNameRole,
        StateRole,     ///< "jobState": "queued" | "running" | "done" | "failed" | "cancelled"
        PhaseTextRole, ///< "Queued", "Preparing", "Rendering", "Finishing", "Done", ...
        ProgressRole,  ///< 0..1
        FpsTextRole,   ///< "212 fps" ("" until known)
        EtaTextRole,   ///< "0:12 left" ("" until known)
        EncoderRole,   ///< encoder actually used, e.g. "h264_nvenc"
        DetailRole,    ///< done: "0:10 · 5.7× realtime"; failed: the plain-language error
        FinishedRole,  ///< done, failed or cancelled
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
    };

    explicit ExportJobModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int activeCount() const;

    void append(Row row);
    /// Applies a queue status to the row of `queueId`; returns the row (nullptr if unknown).
    const Row* update(exporter::ExportJobId queueId, const exporter::ExportJobStatus& status);
    const Row* find(const QString& jobId) const;
    void removeFinished();
    const std::vector<Row>& rows() const { return m_rows; }

signals:
    void countChanged();
    void activeCountChanged();

private:
    std::vector<Row> m_rows;
};

class ExportController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    /// The project exports read (snapshot, current sequence). Defaults to the ProjectController singleton of
    /// the controller's engine, looked up on first use.
    Q_PROPERTY(
        wabcut::app::ProjectController* project READ project WRITE setProject NOTIFY projectChanged FINAL)
    Q_PROPERTY(QVariantList presets READ presets CONSTANT FINAL)
    Q_PROPERTY(wabcut::app::ExportJobModel* jobs READ jobs CONSTANT FINAL)
    Q_PROPERTY(QVariantMap autoEncoders READ autoEncoders NOTIFY autoEncodersChanged FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged FINAL)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged FINAL)
    /// "Explorer" on Windows, "Finder" on macOS, "Files" elsewhere (for "Show in …" labels).
    Q_PROPERTY(QString fileManagerName READ fileManagerName CONSTANT FINAL)

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

    Q_INVOKABLE QString queueExport(const QString& sequenceId, const QString& presetId,
                                    const QVariantMap& overrides = {});
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

    // ---- C++ API ----
    void setProject(ProjectController* project);
    ProjectController* project() const;
    /// The project file media paths resolve against (MediaLocator::relativePath); default output folder.
    void setProjectFile(std::filesystem::path file) { m_projectFile = std::move(file); }
    /// Queues a fully built request (tests, integrations with their own settings). Returns the job id.
    QString queueRequest(exporter::ExportRequest request, const QString& presetName);
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

signals:
    void projectChanged();
    void jobQueued(const QString& jobId);
    void jobFinished(const QString& jobId, bool succeeded, const QString& message);
    void autoEncodersChanged();
    void busyChanged();
    void lastErrorChanged();

private:
    struct Relay;
    void onStatus(const exporter::ExportJobStatus& status);
    void setLastError(const QString& text);
    SequencePtr sequenceFor(const QString& sequenceId) const;

    mutable QPointer<ProjectController> m_project;
    std::filesystem::path m_projectFile;
    ExportJobModel* m_jobs = nullptr;
    QVariantMap m_autoEncoders;
    QString m_lastError;
    exporter::VideoSourceFactory m_videoSource;
    std::function<bool(const QString&)> m_reveal;
    std::uint64_t m_nextJob = 1;
    std::map<exporter::ExportJobId, ai::Json> m_provenance;
    std::shared_ptr<Relay> m_relay; // the queue's listener posts through it; cut in the destructor
    std::unique_ptr<exporter::ExportQueue> m_queue;
    std::jthread m_probe;
};

} // namespace wabcut::app
