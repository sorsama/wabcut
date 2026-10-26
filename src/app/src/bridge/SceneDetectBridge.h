#pragma once

// SceneDetectBridge (0.5, QML singleton `SceneDetect`): Premiere's Scene Edit Detection… on the selected video clips.
// The Clip menu, the clip right-click menu, the keymap's clip.sceneEditDetection (no default key) and the command
// palette open SceneEditDetectionDialog.qml (Main.qml).
//
//   * targets(): the selected clips that show video, in timeline order: clips of media with a picture on unlocked video
//     tracks, and nested / multicam clips (clip.splitAtShots reads through them); a selected audio clip stands for its
//     linked video. availability() says why there is none ("Select a video clip first", "The video track is locked").
//   * run(clips, options): one job off the GUI thread (CommandJob::startBatch: one copy of the project, its progress and
//     Cancel) and ONE undo step "Scene Edit Detection": with `markers`, analysis.detectShots marks the cuts inside each
//     media clip (sequence markers "Shot n", cyan; nested and multicam clips get none), then with `cuts`,
//     clip.splitAtShots razors every clip at its cuts, the linked audio too. The analysis is cached per source file, so
//     both together analyse each file once.
//   * The options are remembered (QSettings sceneDetect/cuts, markers, detector, sensitivity; defaults: cuts on, markers
//     off, the combined detector, Sensitivity 50). Sensitivity (0..100) is the content threshold inverted, so a higher
//     value finds more edits: threshold = 27 x 2^((50 - sensitivity) / 50), 54 at 0, the detector's default 27 at 50,
//     13.5 at 100. Only the combined and content detectors use it.
//   * finished(ok, cancelled, message): the toast text, built from the result data ("Split 3 clips at 41 cuts", "Added
//     12 markers at scene edits in 2 clips", "No scene edits found in the clip"), or why it failed.

#include "CommandJob.h"

#include <wabcut/model/Ids.h>
#include <wabcut/model/Sequence.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut::command {
class CommandServices;
}

namespace wabcut::app {

class ProjectController;

class SceneDetectBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY jobChanged FINAL)
    /// What the job does now ("detecting shots in 'Gameplay.mkv'").
    Q_PROPERTY(QString phase READ phase NOTIFY jobChanged FINAL)
    /// 0..1, or -1 when unknown.
    Q_PROPERTY(double progress READ progress NOTIFY jobChanged FINAL)
    /// The remembered options: { cuts, markers, detector ("combined" | "content" | "adaptive" | "threshold"),
    /// sensitivity (0..100) }.
    Q_PROPERTY(QVariantMap options READ options NOTIFY optionsChanged FINAL)
    /// The last run's text (the toast's), or why it failed ("" before the first run).
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)

public:
    static constexpr const char* kCommand = "clip.sceneEditDetection";

    explicit SceneDetectBridge(QObject* parent = nullptr);
    static SceneDetectBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setController(ProjectController* controller);
    /// The analysis services (tests inject a MapAnalysisProvider; by default the app's MediaAnalysisProvider over the
    /// analysis cache, made again when the project file changes).
    void setServices(std::shared_ptr<command::CommandServices> services) { m_services = std::move(services); }
    /// Jobs on the calling thread (tests).
    void setSynchronous(bool synchronous) { m_job.setSynchronous(synchronous); }

    bool busy() const { return m_busy; }
    QString phase() const { return m_phase; }
    double progress() const { return m_progress; }
    QVariantMap options() const;
    QString message() const { return m_message; }

    /// clip.sceneEditDetection (Main.qml opens the dialog).
    static bool handlesCommand(const QString& id);
    /// For the command palette and the menus: { handled, enabled, reason }.
    QVariantMap commandAvailability(const QString& id) const;
    /// The clips a run on these keys ("c12"; empty = the selection) acts on, in timeline order.
    Q_INVOKABLE QStringList targets(const QStringList& clipKeys = {}) const;
    /// For the selection: { enabled, reason (a sentence), note (the clip menu's short reason: "no video clip"), clips
    /// (targets()), count, nested (how many of them are nested or multicam clips), sequence ("s3": the sequence they are
    /// in, which the dialog keeps with them) }.
    Q_INVOKABLE QVariantMap availability() const;
    /// Starts Scene Edit Detection on the clips ("" = the selection) with `options` ({ cuts, markers, detector,
    /// sensitivity }; the ones given are remembered; fix round 1: `sequence` ("s3"), the sequence the clips are in, so a
    /// dialog opened on them still acts on them after another sequence became current). Returns "" when started, else
    /// why not.
    Q_INVOKABLE QString run(const QStringList& clipKeys, const QVariantMap& options = {});
    Q_INVOKABLE void cancel();
    /// Remembers one option ("cuts", "markers", "detector", "sensitivity").
    Q_INVOKABLE void setOption(const QString& name, const QVariant& value);
    /// The content threshold the detectors get for a Sensitivity (0..100).
    static double thresholdFor(double sensitivity);
    /// The toast text of a finished run: `result` the job's (a batch's steps, or one command's data), `clips` how many
    /// clips it ran on.
    static QString summary(const command::CommandResult& result, bool cuts, bool markers, int clips);

signals:
    void jobChanged();
    void optionsChanged();
    void messageChanged();
    /// A run ended: `message` is the toast text (or the error; "Cancelled" when `cancelled`).
    void finished(bool ok, bool cancelled, const QString& message);

private:
    struct Target {
        ClipId clip;
        bool nested = false;
    };
    /// The clips of `ids` in `seq` that show video (an audio clip gives its linked video), in timeline order; `locked`
    /// counts video clips left out because their track is locked.
    std::vector<Target> collect(const Sequence* seq, const std::vector<ClipId>& ids, int* locked) const;
    std::shared_ptr<command::CommandServices> services();
    void setJob(bool busy, const QString& phase, double progress);
    void setMessage(const QString& message);

    QPointer<ProjectController> m_controller;
    std::shared_ptr<command::CommandServices> m_services;
    std::shared_ptr<command::CommandServices> m_built; ///< the app's, for the project file m_builtFor
    std::filesystem::path m_builtFor;
    CommandJob m_job;
    bool m_busy = false;
    QString m_phase;
    double m_progress = 0;
    QString m_message;
    // The remembered options (read from QSettings in the constructor).
    bool m_cuts = true;
    bool m_markers = false;
    QString m_detector = QStringLiteral("combined");
    double m_sensitivity = 50.0;
};

struct SceneDetectBridgeForeign {
    Q_GADGET
    QML_FOREIGN(SceneDetectBridge)
    QML_NAMED_ELEMENT(SceneDetect)
    QML_SINGLETON
public:
    static SceneDetectBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return SceneDetectBridge::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
