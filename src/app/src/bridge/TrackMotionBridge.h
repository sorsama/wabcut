#pragma once

// TrackMotionBridge: "Track Motion" in Effect Controls (0.7, card #83; singleton `TrackMotion`, docs/masks.md "Track
// Motion"): pin a graphic, title, picture or censor bar to something that moves in the shot, and have it follow.
//
// It is the UI of the engine's track.transform job (masks/MaskCommands.h): the tracker follows a region of a footage
// clip's own frames and keys the Motion (Position; with "Position, scale, rotation" also Scale and Rotation) of another
// clip - or of the footage itself, to hold the subject still (Stabilize). The user
//   * picks the clip with the moving subject ("Subject in", a video clip under the playhead that shows footage) and the
//     clip that follows ("Follows", any other video clip under the playhead; both default from the clip Effect Controls
//     shows: a graphic is the follower of the footage under it, footage gets the clip above it as its follower);
//   * puts the playhead on the frame to start from, with the follower already over the subject there, and marks the
//     subject: a box dragged on the program monitor (a draggable, resizable box with corner handles), or a mask of the
//     footage clip (an AI Object Mask follows people and things this way);
//   * tracks forward or backward (a frame count or to the clip's end): a job on a worker thread with the media decoders'
//     pictures (editor::MediaTrackingService), progress, Cancel, and a per-frame confidence strip, stopping where the
//     picture is lost. The result is ordinary keyframes on the follower's Motion, applied as ONE undo step "Track Motion"
//     (never over an edit made while it ran).
// Geometry crosses into QML as fractions of the program canvas (the overlay fills the canvas). The box is kept in canvas
// fractions at the playhead it was drawn on and turned into frame coordinates through the footage clip's Motion when the
// track starts, so a footage clip that is scaled, rotated or moved tracks the right place.

#include "ConfidenceBuckets.h"

#include <wabcut/masks/MaskCommands.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <thread>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class ProjectController;

class TrackMotionBridge : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(TrackMotion)
    QML_SINGLETON

    /// A footage clip is under the playhead that can be tracked (Effect Controls shows the section).
    Q_PROPERTY(bool available READ available NOTIFY stateChanged FINAL)
    /// Effect Controls shows the Track Motion section (its panel-header button, Clip > Track Motion…): off by default so
    /// the panel keeps its room; the monitor draws the subject box only while it is on.
    Q_PROPERTY(bool panelOpen READ panelOpen WRITE setPanelOpen NOTIFY stateChanged FINAL)
    /// The clip with the moving subject (key "c12", "" = none) and the clip that follows it; each with the clips to choose
    /// from, [{key, name, track}] ("V2").
    Q_PROPERTY(QString subject READ subject WRITE setSubject NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString subjectName READ subjectName NOTIFY stateChanged FINAL)
    Q_PROPERTY(QVariantList subjects READ subjects NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString follower READ follower WRITE setFollower NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString followerName READ followerName NOTIFY stateChanged FINAL)
    Q_PROPERTY(QVariantList followers READ followers NOTIFY stateChanged FINAL)
    /// "Stabilize": key the subject clip itself so the subject stays where it was (no follower).
    Q_PROPERTY(bool stabilize READ stabilize WRITE setStabilize NOTIFY stateChanged FINAL)
    /// What follows the subject: "position" (a move) or "full" (a move, a turn and a size change: Position, Rotation and
    /// Scale).
    Q_PROPERTY(QString match READ match WRITE setMatch NOTIFY stateChanged FINAL)
    /// The subject area: a box on the monitor ([x0, y0, x1, y1], canvas fractions; empty = none) or a mask of the subject
    /// clip ("e12/3"; [{key, name}] to choose from). Choosing one clears the other.
    Q_PROPERTY(QVariantList box READ box NOTIFY geometryChanged FINAL)
    Q_PROPERTY(QString mask READ mask WRITE setMask NOTIFY stateChanged FINAL)
    Q_PROPERTY(QVariantList masks READ masks NOTIFY stateChanged FINAL)
    /// The next drag on the program monitor draws the box.
    Q_PROPERTY(bool boxTool READ boxTool WRITE setBoxTool NOTIFY toolChanged FINAL)
    /// Why Track cannot run now ("Draw a box around the subject first"), or "".
    Q_PROPERTY(QString blocker READ blocker NOTIFY stateChanged FINAL)
    /// Tracking: running, progress 0..1, per-frame confidence (0..1, -1 = lost), what happened.
    Q_PROPERTY(bool tracking READ tracking NOTIFY trackingChanged FINAL)
    Q_PROPERTY(double trackProgress READ trackProgress NOTIFY trackingChanged FINAL)
    Q_PROPERTY(QVariantList confidence READ confidence NOTIFY trackingChanged FINAL)
    /// How many frames each `confidence` value stands for (1 until a long track fills the bounded strip: then each value is
    /// the lowest confidence of its frames).
    Q_PROPERTY(int confidenceFramesPerValue READ confidenceFramesPerValue NOTIFY trackingChanged FINAL)
    Q_PROPERTY(int trackTotal READ trackTotal NOTIFY trackingChanged FINAL)
    Q_PROPERTY(QString trackStatus READ trackStatus NOTIFY trackingChanged FINAL)

public:
    explicit TrackMotionBridge(QObject* parent = nullptr);
    ~TrackMotionBridge() override;
    static TrackMotionBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    bool available() const { return m_subject.isValid(); }
    bool panelOpen() const { return m_panelOpen; }
    void setPanelOpen(bool open);
    QString subject() const;
    QString subjectName() const { return m_subjectName; }
    QVariantList subjects() const { return m_subjects; }
    void setSubject(const QString& key);
    QString follower() const;
    QString followerName() const { return m_followerName; }
    QVariantList followers() const { return m_followers; }
    void setFollower(const QString& key);
    bool stabilize() const { return m_stabilize; }
    void setStabilize(bool stabilize);
    QString match() const { return m_match; }
    void setMatch(const QString& match);
    QVariantList box() const;
    QString mask() const { return m_mask; }
    void setMask(const QString& key);
    QVariantList masks() const { return m_masks; }
    bool boxTool() const { return m_boxTool; }
    void setBoxTool(bool on);
    QString blocker() const { return m_blocker; }
    bool tracking() const { return m_tracking; }
    double trackProgress() const { return m_trackProgress; }
    QVariantList confidence() const { return m_confidence; }
    int confidenceFramesPerValue() const { return static_cast<int>(m_stripFramesPerValue); }
    int trackTotal() const { return m_trackTotal; }
    QString trackStatus() const { return m_trackStatus; }

    /// Sets the subject box from two corners (canvas fractions, any order, kept inside the canvas, at least a few pixels
    /// wide); chooses the box over a mask. True when it was set.
    Q_INVOKABLE bool setBox(double x0, double y0, double x1, double y1);
    /// A box around the follower's centre (else the canvas centre), 20 % of the canvas wide: where to start from.
    Q_INVOKABLE void placeBox();
    Q_INVOKABLE void clearBox();
    /// Tracks from the playhead: "forward" or "backward", `frames` frames (0 = to the clip's end). False (and why, as
    /// `blocker` / trackStatus) when it cannot run.
    Q_INVOKABLE bool track(const QString& direction, int frames);
    /// Stop: the job ends at the frame it has reached and what was tracked until then is kept (ONE undo step "Track Motion";
    /// stopping before the first frame changes nothing).
    Q_INVOKABLE void cancelTrack();

    // ---- C++ (tests, hosts) ----
    /// Run tracking on the calling thread (tests).
    void setSynchronous(bool on) { m_synchronous = on; }
    /// Pictures for the tracker (default: the media decoders, editor::MediaTrackingService).
    void setTrackingServices(std::shared_ptr<masks::TrackingServices> services) { m_trackingServices = std::move(services); }
    void setController(ProjectController* controller);
    /// The track.transform request the last track() sent, as JSON text (tests: the region the box was turned into).
    std::string lastRequestJson() const { return m_lastRequest; }

public slots:
    void refresh();

signals:
    void stateChanged();
    void geometryChanged();
    void toolChanged();
    void trackingChanged();

private:
    struct Placement;
    ProjectController* controller() const;
    void attach();
    void setBlocker(const QString& blocker);
    std::optional<Placement> placementAtPlayhead() const;
    void finishTrack(bool ok, const QString& message, const ProjectSnapshot& base, std::optional<ProjectState> result,
                     ClipId subject, Ticks lastMedia);
    /// One more frame done (GUI thread): the strip's bucket, and the progress announced at most ten times a second.
    void noteProgress(const masks::TrackProgress& progress);
    /// Publishes the strip's buckets as `confidence`.
    void publishConfidence();

    ProjectController* m_controller = nullptr;
    bool m_attached = false;
    bool m_panelOpen = false;
    bool m_forceEmit = false; ///< a property changed: refresh() announces it even when the lists did not
    // The clip the panel is for, the two clips and whether the user picked them (else they follow the selection).
    ClipId m_basis;
    ClipId m_subject;
    ClipId m_follower;
    bool m_subjectChosen = false;
    bool m_followerChosen = false;
    QString m_subjectName;
    QString m_followerName;
    QVariantList m_subjects;
    QVariantList m_followers;
    QVariantList m_masks;
    bool m_stabilize = false;
    QString m_match = QStringLiteral("position");
    std::array<double, 4> m_box{};
    bool m_hasBox = false;
    QString m_mask;
    bool m_boxTool = false;
    QString m_blocker;

    // Tracking.
    bool m_tracking = false;
    double m_trackProgress = 0.0;
    int m_trackTotal = 0;
    QVariantList m_confidence;
    ConfidenceBuckets m_strip; ///< bounded (at most 480 buckets, each the lowest confidence of its frames)
    std::size_t m_stripFramesPerValue = 1; ///< of `m_confidence`, as last published
    QElapsedTimer m_announced; ///< since progress was last announced
    std::string m_lastRequest;
    QString m_trackStatus;
    /// Stop pressed (or the bridge going away): the pictures run out, so the tracker ends where it is with what it has.
    std::shared_ptr<std::atomic<bool>> m_cancel;
    std::jthread m_worker;
    bool m_synchronous = false;
    std::shared_ptr<masks::TrackingServices> m_trackingServices;
};

} // namespace wabcut::app
