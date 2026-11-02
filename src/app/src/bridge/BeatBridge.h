#pragma once

// BeatBridge (0.6, P1 round, D9 #8; QML singleton `Beats`): the user-facing beat tools. Beat detection, the beat grid,
// snapping to beats and cutting to beats already existed as commands (analysis.detectBeats, sequence.setBeatGrid,
// edit.snapToBeats, edit.cutToBeats); this bridge gives them menus: Sequence > Beat Grid (Detect Beats from Selected
// Clip, Snap Selected Clips to Beats, Cut Selected Clips to Beats…, Clear Beat Grid), the Clip menu and the clip
// right-click menu ("Detect Beats…"), the command palette and the keymap (no default keys: Premiere has none).
//
//   * detect(clip, minBpm, maxBpm): analysis.detectBeats on a CommandJob off the GUI thread with the analysis services,
//     in ONE undo step "Detect Beats": beat markers labelled 1-2-3-4 (downbeats red) and the sequence's beat grid.
//     The BPM range is optional (0 = auto); a known tempo narrows the search. finished(ok, cancelled, message) gives
//     the toast text ("128 BPM, 256 beats · 64 downbeat markers").
//   * snapSelection(): edit.snapToBeats on the selected clips (a quarter beat tolerance, both edges), one undo step
//     "Snap to Beats"; edges that cannot reach a beat are reported.
//   * cutSelection(beatsPerCut): edit.cutToBeats: the selected clips' media, one per group of N beats from the first
//     beat at or after the playhead, one undo step "Cut to Beats".
//   * clearGrid(): sequence.setBeatGrid { clear: true }, one undo step ("Set Beat Grid").
//   * commandAvailability(id) greys the menu rows and palette rows out with the reason: "Open a sequence first",
//     "Select a clip with sound", "Detect beats first", "No beat grid to clear", "Select a clip first".
//   * hasGrid / gridInfo mirror the sequence's beat grid (ProjectController.hasBeatGrid) for the menus.

#include "CommandJob.h"

#include <wabcut/model/Ids.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <filesystem>
#include <memory>

class QJSEngine;
class QQmlEngine;

namespace wabcut::command {
class CommandServices;
}

namespace wabcut::app {

class ProjectController;

class BeatBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY jobChanged FINAL)
    /// What the job does now ("Detecting beats in 'Music.mp4'").
    Q_PROPERTY(QString phase READ phase NOTIFY jobChanged FINAL)
    /// 0..1, or -1 when unknown.
    Q_PROPERTY(double progress READ progress NOTIFY jobChanged FINAL)
    /// The sequence has a beat grid: Snap to Beats, Cut to Beats and Clear Beat Grid can act (ProjectController's).
    Q_PROPERTY(bool hasGrid READ hasGrid NOTIFY gridChanged FINAL)
    /// The grid: { bpm, beats, beatsPerBar, phase } (ProjectController.beatGridInfo), empty without one.
    Q_PROPERTY(QVariantMap gridInfo READ gridInfo NOTIFY gridChanged FINAL)
    /// The last run's text (the toast's), or why it failed ("" before the first run).
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)

public:
    static constexpr const char* kDetect = "sequence.detectBeats";
    static constexpr const char* kSnap = "edit.snapToBeats";
    static constexpr const char* kCut = "sequence.cutToBeats";
    static constexpr const char* kClear = "sequence.clearBeatGrid";

    explicit BeatBridge(QObject* parent = nullptr);
    static BeatBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setController(ProjectController* controller);
    /// The analysis services (tests inject a MapAnalysisProvider; by default the app's MediaAnalysisProvider over the
    /// analysis cache, made again when the project file changes).
    void setServices(std::shared_ptr<command::CommandServices> services) { m_services = std::move(services); }
    /// Jobs on the calling thread (tests).
    void setSynchronous(bool synchronous) { m_job.setSynchronous(synchronous); }

    bool busy() const { return m_busy; }
    QString phase() const { return m_phase; }
    double progress() const { return m_progress; }
    bool hasGrid() const;
    QVariantMap gridInfo() const;
    QString message() const { return m_message; }

    /// The four ids this bridge answers for the menus and the palette.
    static bool handlesCommand(const QString& id);
    /// For the command palette and the menus: { handled, enabled, reason }.
    QVariantMap commandAvailability(const QString& id) const;
    /// The clip "Detect Beats…" acts on: the first selected clip with sound, in timeline order:
    /// { enabled, reason, clip ("c12"), name }. An audio clip comes before a video clip with sound.
    Q_INVOKABLE QVariantMap detectInfo() const;
    /// What Cut to Beats acts on: { enabled, reason, clips (how many are selected), beatsPerCut (1, 2, 4 or 8) }.
    Q_INVOKABLE QVariantMap cutInfo() const;
    /// Starts Detect Beats on `clipKey` ("" = the selection) with an optional tempo range (0 = auto). Returns "" when
    /// started, else why not.
    Q_INVOKABLE QString detect(const QString& clipKey = {}, double minBpm = 0, double maxBpm = 0);
    /// Trims the selected clips' edges to the nearest beats. Returns "" when it ran, else why not.
    Q_INVOKABLE QString snapSelection();
    /// Places the selected clips' media one per `beatsPerCut` beats from the playhead, overwriting. Returns "" when it
    /// ran, else why not.
    Q_INVOKABLE QString cutSelection(int beatsPerCut = 4);
    /// Removes the sequence's beat grid. Returns "" when it ran, else why not.
    Q_INVOKABLE QString clearGrid();
    Q_INVOKABLE void cancel();

signals:
    void jobChanged();
    void gridChanged();
    void messageChanged();
    /// A run ended (Detect Beats, Snap to Beats, Cut to Beats, Clear Beat Grid): `message` is the toast text (or the
    /// error; "Cancelled" when `cancelled`).
    void finished(bool ok, bool cancelled, const QString& message);

private:
    std::shared_ptr<command::CommandServices> services();
    void setJob(bool busy, const QString& phase, double progress);
    void setMessage(const QString& message);
    void refreshGrid();
    /// The tempo as the toast says it: "128", "128.5".
    static QString bpmText(double bpm);
    /// The toast text of a finished analysis.detectBeats result.
    static QString detectSummary(const command::CommandResult& result);

    QPointer<ProjectController> m_controller;
    std::shared_ptr<command::CommandServices> m_services;
    std::shared_ptr<command::CommandServices> m_built; ///< the app's, for the project file m_builtFor
    std::filesystem::path m_builtFor;
    CommandJob m_job;
    bool m_busy = false;
    QString m_phase;
    double m_progress = 0;
    QString m_message;
};

struct BeatBridgeForeign {
    Q_GADGET
    QML_FOREIGN(BeatBridge)
    QML_NAMED_ELEMENT(Beats)
    QML_SINGLETON
public:
    static BeatBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) {
        return BeatBridge::create(qmlEngine, jsEngine);
    }
};

} // namespace wabcut::app
