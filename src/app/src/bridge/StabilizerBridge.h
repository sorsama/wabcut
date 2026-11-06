#pragma once

// StabilizerBridge: Analyze for Warp Stabilizer and Rolling Shutter Repair in QML (singleton `Stabilizer`, P.2,
// docs/motion.md "Warp Stabilizer").
//
// Effect Controls' header of such an effect shows its analysis state and an Analyze / Cancel button (StabilizerStatus
// .qml). analyze(section) runs effect.analyzeStabilization (or effect.analyzeRollingShutter) on a worker thread with the
// media decoders' pictures (editor::MediaTrackingService) and the analysis cache (so changing a setting and analysing
// again re-smooths without decoding), reports its progress ("Analyzing in background", as Premiere's banner), and
// applies the keyed instance to the project as one undo step. Cancel stops it between two frames.
// 0.7 (cards #107, #128): analyzeAll() runs effect.analyzeAll over every stabilizer of the sequence that is not analysed
// or out of date (one progress bar, one undo step); overview() and exportNeeds() tell the Effect Controls header and the
// Export panel how many are (an export renders an un-analysed stabilizer unstabilized, so the panel warns first).

#include <wabcut/ai/stabilize/StabilizeCommands.h>
#include <wabcut/gyro/Gyro.h>
#include <wabcut/masks/MaskCommands.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace wabcut::app {

class ProjectController;

class StabilizerBridge : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Stabilizer)
    QML_SINGLETON

    /// The effect being analysed ("e12"; "" = none).
    Q_PROPERTY(QString section READ section NOTIFY changed FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY changed FINAL)
    /// 0..1 of the frames analysed.
    Q_PROPERTY(double progress READ progress NOTIFY changed FINAL)
    /// The last analysis' outcome ("Analysis complete", "Cancelled", why it failed).
    Q_PROPERTY(QString message READ message NOTIFY changed FINAL)

public:
    explicit StabilizerBridge(QObject* parent = nullptr);
    ~StabilizerBridge() override;

    QString section() const { return m_section; }
    bool busy() const { return !m_section.isEmpty(); }
    double progress() const { return m_progress; }
    QString message() const { return m_message; }

    /// What the header of effect `section` shows: {stabilizer: bool, kind: "warp" | "shutter", status: "" (not
    /// analysed) | "analyzed" | "failed"}. stabilizer is false for every other effect.
    Q_INVOKABLE QVariantMap effectInfo(const QString& section) const;
    /// Starts the analysis of effect `section` (a Warp Stabilizer or Rolling Shutter Repair of a clip of the current
    /// sequence). False when one is running or the effect is not one of them.
    Q_INVOKABLE bool analyze(const QString& section);
    Q_INVOKABLE void cancel();

    // ---- 0.7 (cards #107, #128): Analyze All and the export warning ----
    /// Where the current sequence's stabilizers stand (Warp Stabilizer and Rolling Shutter Repair, the sequences nested in
    /// it and Master Clip Effects too): { count, pending (not analysed or out of date), notAnalyzed, outOfDate, current,
    /// skipped }. Cheap to call again: kept for the project snapshot it was read from.
    Q_INVOKABLE QVariantMap overview() const;
    /// Analyze All: every stabilizer of the current sequence that is not analysed, or out of date (`reanalyze`: every one),
    /// in one job (effect.analyzeAll on a worker, one progress bar, Cancel) applied as ONE undo step "Analyze All
    /// Stabilizers". False when one is running or there is nothing to do (the message says so).
    Q_INVOKABLE bool analyzeAll(bool reanalyze = false);
    /// The stabilizers an export of `range` ("whole", "inOut", "workArea", "custom": whole) would render without their
    /// analysis: { count, names: ["V2: Interview.mov (Warp Stabilizer)", ...], pendingOnly }. The Export panel warns with it.
    Q_INVOKABLE QVariantMap exportNeeds(const QString& range) const;

    // ---- Tests ----
    void setTrackingServices(std::shared_ptr<masks::TrackingServices> services) { m_pictures = std::move(services); }
    /// 0.7 (card #130): the camera's gyro for the Gyro method (tests give a fake; the app reads the media file).
    void setGyroServices(std::shared_ptr<gyro::GyroServices> services) { m_gyro = std::move(services); }
    /// Runs analyze() on the calling thread (tests).
    void setSynchronous(bool synchronous) { m_synchronous = synchronous; }
    void setCacheRoot(std::filesystem::path root) { m_cacheRoot = std::move(root); }

signals:
    void changed();

private:
    struct Found;
    ProjectController* controller() const;
    std::optional<Found> find(const QString& section) const;
    void finish(bool ok, const QString& message, ClipId clip, EffectInstanceId effect, std::optional<ProjectState> result);
    /// The survey of the current sequence for the snapshot it was made from.
    const std::vector<ai::stabilize::StabilizerEntry>& survey() const;
    void finishAll(bool ok, const QString& message, const ProjectSnapshot& base, std::optional<ProjectState> result);

    mutable ProjectSnapshot m_surveyOf;
    mutable SequenceId m_surveySequence;
    mutable std::vector<ai::stabilize::StabilizerEntry> m_survey;
    QString m_section;
    double m_progress = 0.0;
    QString m_message;
    std::shared_ptr<masks::TrackingServices> m_pictures;
    std::shared_ptr<gyro::GyroServices> m_gyro;
    std::filesystem::path m_cacheRoot;
    bool m_synchronous = false;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    std::jthread m_worker;
};

} // namespace wabcut::app
