#pragma once

// MotionBridge: Premiere's direct manipulation of Motion in the Program Monitor (singleton `ProgramMotion`, P.2 E6;
// docs/monitors.md).
//
//   * Engaged by selecting Motion in Effect Controls (its header's direct-manipulation button) or by double-clicking
//     a clip's picture in the Program Monitor (the topmost visible video clip under the pointer is selected), for the
//     clip that receives property edits (the primary selection or its linked video partner). Selecting another clip
//     or Esc lets go.
//   * While the clip is under the playhead: its frame's bounding box (`quad`), the corner and edge handles, the anchor
//     point and, when Position is animated, the motion path (`path`: the anchor's track through the clip and its
//     keyframes). Everything is in fractions of the sequence canvas; the overlay (qml/monitor/MotionOverlay.qml) fills
//     the program frame.
//   * Gestures (hitTest() names them): inside the box = Position (Shift: horizontal or vertical only); a corner or edge
//     handle = Scale around the anchor point (uniform: Wabcut's Motion has one Scale); just outside a corner = Rotation
//     around the anchor point (Shift: 45-degree steps); the anchor point = Anchor Point, with Position following so
//     the picture stays put (Alt: Anchor Point only, so the picture moves); a motion path keyframe = that keyframe's
//     Position. Moves snap the box's edges and centre to the frame's edges and centre and to the monitor's guides
//     (snapping on, Ctrl off), showing `guides`.
//   * Each gesture is one undo step: edit::setMotion (MotionOps.h, the model op behind clip.setMotion) merged per
//     gesture. An animated parameter gets a key at the playhead (a path keyframe: at its own time), a constant one
//     changes everywhere, as in Premiere.
//   * Crop (`mode` "crop": Premiere 26's Toggle Crop Direct Manipulation, or the button on a Crop effect in Effect
//     Controls): the box is the cropped frame and its edge and corner handles set the clip's Crop effect (left, top,
//     right, bottom: fractions of the frame), keyed like Motion, one undo step per drag.
//
// The box follows what playback draws (playback::layerOf in Evaluate.cpp): the clip's frame (a media stream with its
// display rotation and pixel aspect, a title's canvas, a graphic's or synthetic item's frame, a nested sequence's
// canvas) placed by render::layerToCanvas.

#include <wabcut/model/Ids.h>
#include <wabcut/model/MotionOps.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/render/PreparedComposite.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <array>
#include <optional>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class ProjectController;

class MotionBridge : public QObject {
    Q_OBJECT
    /// Motion is selected for direct manipulation (Effect Controls' Motion, or a double-click in the monitor).
    Q_PROPERTY(bool engaged READ engaged WRITE setEngaged NOTIFY engagedChanged FINAL)
    /// The clip the box is for ("c42"; "" none).
    Q_PROPERTY(QString clip READ clip NOTIFY stateChanged FINAL)
    /// Engaged, and the clip is a video clip under the playhead: the box is shown.
    Q_PROPERTY(bool active READ active NOTIFY stateChanged FINAL)
    /// What the handles edit: "transform" (Motion) or "crop" (the clip's Crop effect).
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY stateChanged FINAL)
    /// The clip has a Crop effect for the crop handles.
    Q_PROPERTY(bool cropAvailable READ cropAvailable NOTIFY stateChanged FINAL)
    /// The clip frame's corners, top-left, top-right, bottom-right, bottom-left: [x0, y0, ... x3, y3] (fractions).
    Q_PROPERTY(QVariantList quad READ quad NOTIFY stateChanged FINAL)
    /// [{ kind: "scale-tl" | "scale-t" | "scale-tr" | "scale-r" | "scale-br" | "scale-b" | "scale-bl" | "scale-l", x, y }].
    Q_PROPERTY(QVariantList handles READ handles NOTIFY stateChanged FINAL)
    /// The anchor point: { x, y }.
    Q_PROPERTY(QVariantMap anchor READ anchor NOTIFY stateChanged FINAL)
    /// The motion path when Position is animated: { points: [x0, y0, x1, y1, ...], keys: [{ index, x, y, current }] }
    /// (current: the key at the playhead); {} otherwise.
    Q_PROPERTY(QVariantMap path READ path NOTIFY stateChanged FINAL)
    /// While a move snaps: [{ orientation: "vertical" | "horizontal", position (fraction) }].
    Q_PROPERTY(QVariantList guides READ guides NOTIFY guidesChanged FINAL)
    /// Moves snap (Premiere's Snap in Program Monitor); Ctrl turns it off for one drag.
    Q_PROPERTY(bool snapping READ snapping WRITE setSnapping NOTIFY snappingChanged FINAL)
    Q_PROPERTY(bool dragging READ dragging NOTIFY draggingChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged FINAL)

public:
    explicit MotionBridge(QObject* parent = nullptr);
    static MotionBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setController(ProjectController* controller);

    bool engaged() const { return m_engaged; }
    void setEngaged(bool engaged);
    QString clip() const;
    bool active() const { return m_geometry.has_value() && (!m_crop || m_geometry->crop.has_value()); }
    QString mode() const { return m_crop ? QStringLiteral("crop") : QStringLiteral("transform"); }
    void setMode(const QString& mode);
    bool cropAvailable() const;
    QVariantList quad() const;
    QVariantList handles() const;
    QVariantMap anchor() const;
    QVariantMap path() const { return m_crop ? QVariantMap{} : m_pathJs; }
    QVariantList guides() const { return m_guidesJs; }
    bool snapping() const { return m_snapping; }
    void setSnapping(bool snapping);
    bool dragging() const { return m_drag.has_value(); }
    QString message() const { return m_message; }

    /// Extra lines a move snaps to (the monitor's guides), in fractions of the canvas.
    void setSnapLines(std::vector<double> vertical, std::vector<double> horizontal);

    /// Recomputes the box (also on every edit, selection and playhead change).
    Q_INVOKABLE void refresh();
    /// The topmost video clip whose picture is under a point of the canvas (fractions) at the playhead ("" none).
    Q_INVOKABLE QString clipAt(double fx, double fy) const;
    /// Double-click: selects the clip under the point and engages Motion on it. False when there is none.
    Q_INVOKABLE bool engageAt(double fx, double fy);
    /// What a press at (fx, fy) would drag: "anchor", "key:<n>" (a motion path keyframe), a "scale-*" handle,
    /// "rotate", "move", or "" (outside). `reachX` / `reachY`: the grab distance in fractions of the width / height.
    Q_INVOKABLE QString hitTest(double fx, double fy, double reachX, double reachY) const;
    /// Starts a gesture (see hitTest). False when there is nothing to drag.
    Q_INVOKABLE bool beginDrag(const QString& kind, double fx, double fy);
    /// The pointer moved: applies the gesture. `snapReach`: the snapping distance as a fraction of the width.
    Q_INVOKABLE void dragTo(double fx, double fy, bool shift, bool ctrl, bool alt, double snapReach);
    Q_INVOKABLE void endDrag();
    /// Esc during a drag: undoes what it changed.
    Q_INVOKABLE void cancelDrag();
    /// Moves Position by (dx, dy) sequence pixels: Premiere's Nudge Selected Object (Ctrl+arrows). One undo step.
    Q_INVOKABLE bool nudge(double dx, double dy);
    /// Effect Controls: whether `section` ("e12") is a Crop effect of the clip Effect Controls shows.
    Q_INVOKABLE bool isCropSection(const QString& section) const;
    /// Toggle Crop Direct Manipulation: the crop handles on (engaging the selected clip) or off.
    Q_INVOKABLE void toggleCrop();

    /// Where a clip's frame sits on the canvas (render::layerToCanvas): `frameToCanvas` maps frame pixels (top-left
    /// origin) to sequence pixels. `quarter` is the stream's display rotation in quarter turns (the anchor is stored
    /// in displayed pixels and turned like playback does).
    struct Geometry {
        ClipId clip;
        SequenceId sequence;
        Ticks curveTime{};
        double frameWidth = 0, frameHeight = 0;
        double pixelAspect = 1.0;
        int quarter = 0;
        double canvasWidth = 1920, canvasHeight = 1080;
        /// P.2 (E5) Scale to Frame Size: the fit of the picture into the sequence frame, applied before Motion's Scale
        /// (scaleToFrameFactor; 1 without it), as playback draws it.
        double frameScale = 1.0;
        edit::MotionValues values;
        render::Affine2D frameToCanvas;
        /// The clip's (first enabled) Crop effect and its edges at the curve time, fractions of the frame.
        struct Crop {
            EffectInstanceId effect;
            double left = 0, top = 0, right = 0, bottom = 0;
        };
        std::optional<Crop> crop;
    };
    /// The geometry of `clip` in `seq` at curve time `curveTime` (nullopt: nothing to place, e.g. an audio clip or an
    /// adjustment layer).
    static std::optional<Geometry> geometryOf(const ProjectState& project, const Sequence& seq, const Clip& clip,
                                              Ticks curveTime);
    /// The canvas placement of `values` for the frame of `g`.
    static render::Affine2D placement(const Geometry& g, const edit::MotionValues& values);

signals:
    void engagedChanged();
    void stateChanged();
    void guidesChanged();
    void snappingChanged();
    void draggingChanged();
    void messageChanged();

private:
    struct PathKey {
        Ticks curveTime{};
        double x = 0, y = 0; ///< canvas pixels
        bool current = false;
    };
    struct Drag {
        QString kind;
        Geometry start;
        double px = 0, py = 0; ///< the pointer at the start (canvas pixels)
        std::optional<PathKey> key;
        bool committed = false;
        std::string mergeKey;
    };

    void onSelectionChanged();
    void publish();
    void setMessage(const QString& message);
    void setGuides(QVariantList guides);
    std::optional<ClipId> targetClip() const;
    /// Applies `change` at `curveTime` as part of the current gesture (or alone: `mergeKey` empty).
    bool apply(const Geometry& g, Ticks curveTime, const edit::MotionChange& change, const char* label,
               const std::string& mergeKey);
    /// Sets the Crop effect's edges at `curveTime` (keyed where animated) as part of the current gesture.
    bool applyCrop(const Geometry& g, const Geometry::Crop& crop, const std::string& mergeKey);
    /// The crop rectangle's corners on the canvas (top-left, top-right, bottom-right, bottom-left).
    static std::array<render::Vec2, 4> cropCorners(const Geometry& g);

    QPointer<ProjectController> m_controller;
    bool m_engaged = false;
    bool m_crop = false; ///< the crop handles instead of Motion's
    std::optional<ClipId> m_clip; ///< engaged on this clip
    bool m_selecting = false;     ///< engageAt() is changing the selection itself
    std::optional<Geometry> m_geometry;
    std::vector<PathKey> m_pathKeys;
    QVariantMap m_pathJs;
    QVariantList m_guidesJs;
    std::vector<double> m_snapVertical;   ///< guides, fractions of the width
    std::vector<double> m_snapHorizontal; ///< guides, fractions of the height
    bool m_snapping = true;
    std::optional<Drag> m_drag;
    quint64 m_gestures = 0;
    QString m_message;
};

struct MotionBridgeSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::MotionBridge)
    QML_NAMED_ELEMENT(ProgramMotion)
    QML_SINGLETON
public:
    static MotionBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return MotionBridge::create(qmlEngine, jsEngine); }
};

} // namespace wabcut::app
