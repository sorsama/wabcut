#pragma once

// PacingBridge: the pacing heat strip (singleton `Pacing`, 0.7 card 180; docs/timeline.md "Pacing strip"). A thin strip under
// the timeline ruler colours the stretches of the edit where a viewer may get bored: a long shot without a cut, a picture that
// hardly moves, a long silence, slow speech (model/PacingMap.h grades them from measurements made on this machine; nothing is
// uploaded and no model is asked).
//
//   * `active` (the strip is shown): the analysis runs on the current sequence and again, after a short pause, after every
//     edit. The cuts, the transcript's words (when the sequence has one) and the shot lengths are instant; the media's
//     loudness and motion come from editor::activityMeasure (a cheap pass over the proxy or the original, cached per file)
//     measured on a worker thread, one file after another, with progress in `status` and the strip filling in as each
//     file is done. A file that cannot be measured (offline, unreadable) stays unknown: it is never reported as silent.
//   * `stretches`: [{ index, startSec, endSec, level (1 easing off, 2 slow, 3 drags), score, reasons: ["longShot", "still",
//     "silence", "slowSpeech"], headline, text (the tooltip: why, in numbers, and what to try), startText, endText }].
//   * stretchAt(seconds): the index of the stretch over a time, or -1.
//   * markStretch(i): the sequence's In and Out marks around the stretch and the playhead on its start (one undo step);
//     selectStretch(i): the clips that play in it; addMarker(i): a sequence marker named for the stretch with the reasons as
//     its comment (to come back to it); each is one undo step.

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/PacingMap.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class ProjectController;
class TranscriptBridge;

class PacingBridge : public QObject {
    Q_OBJECT
    /// The strip is on: the analysis runs and follows the edit.
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged FINAL)
    Q_PROPERTY(QVariantList stretches READ stretches NOTIFY stretchesChanged FINAL)
    /// Media are being measured (the strip fills in as each is done).
    Q_PROPERTY(bool busy READ busy NOTIFY statusChanged FINAL)
    Q_PROPERTY(double progress READ progress NOTIFY statusChanged FINAL)
    /// "Measuring footage: 2 of 5", "2 files could not be measured", or "".
    Q_PROPERTY(QString status READ status NOTIFY statusChanged FINAL)
    /// The strip's summary for the screen reader and its tooltip when nothing is under the pointer.
    Q_PROPERTY(QString summary READ summary NOTIFY stretchesChanged FINAL)

public:
    using Measure = command::ActivityServices::Measure;

    explicit PacingBridge(QObject* parent = nullptr);
    ~PacingBridge() override;
    static PacingBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setController(ProjectController* controller);
    void setTranscripts(TranscriptBridge* transcripts);
    /// Replaces the measuring function (tests; the default is editor::activityMeasure for the project's folder).
    void setMeasure(Measure measure);

    bool active() const { return m_active; }
    void setActive(bool on);
    QVariantList stretches() const { return m_items; }
    bool busy() const { return m_busy; }
    double progress() const { return m_progress; }
    QString status() const { return m_status; }
    QString summary() const;

    Q_INVOKABLE int stretchAt(double seconds) const;
    Q_INVOKABLE QVariantMap stretch(int index) const;
    Q_INVOKABLE bool markStretch(int index);
    Q_INVOKABLE bool selectStretch(int index);
    Q_INVOKABLE bool addMarker(int index);
    /// Runs the analysis now (the strip does it after a pause; tests and Settings use this).
    Q_INVOKABLE void refreshNow();

signals:
    void activeChanged();
    void stretchesChanged();
    void statusChanged();

private:
    void schedule();
    void recompute();
    void startMeasuring(const ProjectSnapshot& snapshot, const std::vector<MediaId>& missing);
    void cancelMeasuring();
    void measured(std::uint64_t job, const std::string& key, std::shared_ptr<const MediaActivity> activity, bool failed, int done, int total);
    void finished(std::uint64_t job);
    void setStatus(bool busy, double progress, const QString& status);
    QVariantMap describe(const PacingStretch& stretch, int index) const;
    std::string keyOf(const ProjectState& state, MediaId media) const;
    const MediaActivity* cachedActivity(const ProjectState& state, MediaId media) const;

    QPointer<ProjectController> m_controller;
    QPointer<TranscriptBridge> m_transcripts;
    QTimer m_timer;
    bool m_active = false;
    Measure m_measure;
    std::map<std::string, std::shared_ptr<const MediaActivity>> m_activity; // by media id and file
    std::set<std::string> m_failed;                                        // measured in vain: unknown, not asked again
    std::vector<PacingStretch> m_stretches;
    QVariantList m_items;
    Ticks m_end;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    std::uint64_t m_job = 0;
    bool m_measuring = false;
    bool m_busy = false;
    double m_progress = 0.0;
    QString m_status;
};

struct PacingSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::PacingBridge)
    QML_NAMED_ELEMENT(Pacing)
    QML_SINGLETON
public:
    static PacingBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return PacingBridge::create(qmlEngine, jsEngine); }
};

} // namespace wabcut::app
