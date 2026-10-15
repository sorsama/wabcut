#pragma once

// MaskBridge: masks, tracking and source-anchored redactions for QML (singleton `Masks`, I.6, docs/masks.md).
// Owner: Graphics.
//
// Effect Controls' mask section and the program monitor's mask overlay edit the masks of the clip Effect Controls
// shows (the primary selected video clip, else the topmost video clip under the playhead):
//   * a target - the clip's opacity mask (cut-outs) or one of its video effects, the grade included (local
//     corrections) - receives new masks: drawn on the monitor with the ellipse, rectangle or pen tool, or added
//     centred;
//   * the selected mask's handles on the monitor (centre, size, rotation, path points and their tangents, feather,
//     expansion) and its values in the section (mode, invert, on/off, feather, expansion, opacity) are typed edits
//     (model edit::updateMask), one undo step per drag or slider gesture; animated parameters get a key at the
//     playhead;
//   * Track (one frame back or forward, to the start or to the end) runs track.mask on a worker thread with the media
//     decoders' pictures (editor::MediaTrackingService), reporting its progress and a per-frame confidence, and
//     commits its keyframes as one undo step (cancellable).
// Geometry crosses into QML as fractions of the program canvas (the overlay fills the canvas), so QML only draws.
//
// The Source Monitor's redaction tool marks a region of the loaded source over its In..Out (else the whole media) as
// a redaction (blur, mosaic or solid; mute or bleep); the list shows the source's redactions, and the regions active
// at the source position are drawn over its picture.

#include <wabcut/masks/MaskCommands.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/Masks.h>

#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut {
struct Clip;
struct VideoStreamInfo;
} // namespace wabcut

namespace wabcut::app {

class ProjectController;
class SourceMonitorBridge;

class MaskBridge : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Masks)
    QML_SINGLETON

    /// The clip whose masks are shown ("" = none: no video clip selected or under the playhead).
    Q_PROPERTY(QString clip READ clip NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString clipName READ clipName NOTIFY stateChanged FINAL)
    /// What new masks go to: [{effect, name}]; effect "" = the clip's opacity mask.
    Q_PROPERTY(QVariantList targets READ targets NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString target READ target WRITE setTarget NOTIFY stateChanged FINAL)
    /// The target's masks: [{key, name, shape, mode, inverted, enabled, tracked}]; key = "e12/3".
    Q_PROPERTY(QVariantList masks READ masks NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString selected READ selected WRITE select NOTIFY stateChanged FINAL)
    /// The selected mask's values at the playhead: {name, shape, mode, inverted, enabled, feather, expansion,
    /// opacity, animated: [keys]} (empty when none is selected).
    Q_PROPERTY(QVariantMap current READ current NOTIFY stateChanged FINAL)
    /// The monitor tool: "" (select and drag handles), "ellipse", "rectangle" or "pen".
    Q_PROPERTY(QString tool READ tool WRITE setTool NOTIFY toolChanged FINAL)

    /// Overlay geometry, fractions of the program canvas: the target's mask outlines [{key, selected, points}],
    /// the selected mask's handles [{kind, index, x, y, fromX, fromY}] (tangent handles draw a line from (fromX,
    /// fromY)), its feather band's outer and inner outlines, and the pen's points so far.
    Q_PROPERTY(QVariantList outlines READ outlines NOTIFY geometryChanged FINAL)
    Q_PROPERTY(QVariantList handles READ handles NOTIFY geometryChanged FINAL)
    Q_PROPERTY(QVariantList featherOuter READ featherOuter NOTIFY geometryChanged FINAL)
    Q_PROPERTY(QVariantList featherInner READ featherInner NOTIFY geometryChanged FINAL)
    Q_PROPERTY(QVariantList penPoints READ penPoints NOTIFY geometryChanged FINAL)

    /// Tracking: running, progress 0..1, per-frame confidence (0..1, -1 = lost), what happened.
    Q_PROPERTY(bool tracking READ tracking NOTIFY trackingChanged FINAL)
    Q_PROPERTY(double trackProgress READ trackProgress NOTIFY trackingChanged FINAL)
    Q_PROPERTY(QVariantList confidence READ confidence NOTIFY trackingChanged FINAL)
    /// Frames the running (or last) track goes through at most (the strip's full width).
    Q_PROPERTY(int trackTotal READ trackTotal NOTIFY trackingChanged FINAL)
    Q_PROPERTY(QString trackStatus READ trackStatus NOTIFY trackingChanged FINAL)

    /// The loaded source's redactions: [{key, name, style, audio, enabled, start, end (fractions of the media),
    /// range (text)}]; the regions active at the source position [{key, style, points}] (fractions of the frame).
    Q_PROPERTY(QVariantList redactions READ redactions NOTIFY redactionsChanged FINAL)
    Q_PROPERTY(QVariantList activeRegions READ activeRegions NOTIFY redactionsChanged FINAL)
    /// The Source Monitor's redaction tool is armed (the next drag over the picture marks a region).
    Q_PROPERTY(bool redactTool READ redactTool WRITE setRedactTool NOTIFY redactionsChanged FINAL)
    /// Style of new redactions: "blur", "mosaic" or "solid"; their sound: "none", "mute" or "bleep".
    Q_PROPERTY(QString redactStyle READ redactStyle WRITE setRedactStyle NOTIFY redactionsChanged FINAL)
    Q_PROPERTY(QString redactAudio READ redactAudio WRITE setRedactAudio NOTIFY redactionsChanged FINAL)

    /// The last edit's problem, or what a tool did.
    Q_PROPERTY(QString status READ status NOTIFY statusChanged FINAL)

public:
    explicit MaskBridge(QObject* parent = nullptr);
    ~MaskBridge() override;
    static MaskBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    QString clip() const;
    QString clipName() const { return m_clipName; }
    QVariantList targets() const { return m_targets; }
    QString target() const { return m_target; }
    void setTarget(const QString& effect);
    QVariantList masks() const { return m_masks; }
    QString selected() const { return m_selected; }
    void select(const QString& key);
    QVariantMap current() const { return m_current; }
    QString tool() const { return m_tool; }
    void setTool(const QString& tool);
    QVariantList outlines() const { return m_outlines; }
    QVariantList handles() const { return m_handles; }
    QVariantList featherOuter() const { return m_featherOuter; }
    QVariantList featherInner() const { return m_featherInner; }
    QVariantList penPoints() const;
    bool tracking() const { return m_tracking; }
    double trackProgress() const { return m_trackProgress; }
    QVariantList confidence() const { return m_confidence; }
    int trackTotal() const { return m_trackTotal; }
    QString trackStatus() const { return m_trackStatus; }
    QVariantList redactions() const { return m_redactions; }
    QVariantList activeRegions() const { return m_activeRegions; }
    bool redactTool() const { return m_redactTool; }
    void setRedactTool(bool on);
    QString redactStyle() const { return m_redactStyle; }
    void setRedactStyle(const QString& style);
    QString redactAudio() const { return m_redactAudio; }
    void setRedactAudio(const QString& audio);
    QString status() const { return m_status; }

    // ---- Masks ----
    /// Adds a mask of `shape` ("ellipse", "rectangle") to the target, centred; selects it.
    Q_INVOKABLE bool addMask(const QString& shape);
    /// Adds an ellipse or rectangle spanning a drag on the monitor (canvas fractions); selects it.
    Q_INVOKABLE bool drawShape(const QString& shape, double x0, double y0, double x1, double y1);
    /// Pen: a click adds a corner point; dragging from it pulls its tangents (canvas fractions).
    Q_INVOKABLE void penPoint(double x, double y);
    Q_INVOKABLE void penTangent(double x, double y);
    /// Closes the pen's path into a mask (at least 3 points); false when there are too few.
    Q_INVOKABLE bool penClose();
    Q_INVOKABLE void penCancel();
    Q_INVOKABLE bool removeMask(const QString& key);
    Q_INVOKABLE bool setMode(const QString& key, const QString& mode);
    Q_INVOKABLE bool setInverted(const QString& key, bool inverted);
    Q_INVOKABLE bool setEnabled(const QString& key, bool enabled);
    Q_INVOKABLE bool rename(const QString& key, const QString& name);
    /// A parameter of the selected mask (feather, expansion, opacity, ...), merged per gesture until endEdit().
    Q_INVOKABLE bool setValue(const QString& param, double value);
    Q_INVOKABLE void endEdit();
    /// Monitor handles: press, drag and release (canvas fractions). The handle kinds come from `handles`.
    Q_INVOKABLE void beginDrag(const QString& kind, int index, double x, double y);
    Q_INVOKABLE void drag(double x, double y);
    Q_INVOKABLE void endDrag();
    /// The topmost mask outline of the target under a canvas point ("" = none); used to select by clicking.
    Q_INVOKABLE QString maskAt(double x, double y) const;

    // ---- Tracking ----
    /// Tracks the selected mask from the playhead: "forward" or "backward", `frames` frames (0 = to the clip's end).
    Q_INVOKABLE bool track(const QString& direction, int frames);
    Q_INVOKABLE void cancelTrack();

    // ---- Redactions (the Source Monitor) ----
    /// Marks the region spanning a drag over the source picture (frame fractions) over the source's In..Out (else
    /// the whole media), with redactStyle and redactAudio.
    Q_INVOKABLE bool addRedaction(double x0, double y0, double x1, double y1);
    Q_INVOKABLE bool removeRedaction(const QString& key);
    Q_INVOKABLE bool setRedactionStyle(const QString& key, const QString& style);
    Q_INVOKABLE bool setRedactionAudio(const QString& key, const QString& audio);
    Q_INVOKABLE bool setRedactionEnabled(const QString& key, bool enabled);
    /// "Don't redact here" on the selected clip and its linked partners (or redact it again).
    Q_INVOKABLE bool setRedactionOverride(const QString& key, bool redact);

    // ---- C++ (tests, hosts) ----
    /// Run tracking on the calling thread (tests).
    void setSynchronous(bool on) { m_synchronous = on; }
    /// Pictures for the tracker (default: the media decoders, editor::MediaTrackingService).
    void setTrackingServices(std::shared_ptr<masks::TrackingServices> services) { m_trackingServices = std::move(services); }
    ClipId clipId() const { return m_clip; }

public slots:
    void refresh();
    /// An edit refreshes only when it can change what the panel shows (M5n, P.1): the target clip (or the primary
    /// clip it follows) or their media, project-wide changes (redactions, media), or, when the target is the clip
    /// under the playhead, a video track changed there.
    void onHistory();

signals:
    void stateChanged();
    void toolChanged();
    void geometryChanged();
    void trackingChanged();
    void redactionsChanged();
    void statusChanged();

private:
    struct Placement;
    struct Located;
    ProjectController* controller() const;
    SourceMonitorBridge* sourceMonitor() const;
    void attach();
    void refreshGeometry();
    void refreshRedactions();
    void setStatus(const QString& status);
    std::optional<Located> locate(const QString& key) const;
    std::optional<Placement> placement() const;
    /// Where `clip` of `sequence` shows its stored frame on the canvas at `curveTime`, as playback places it
    /// (Evaluate.cpp: the stream's display rotation, pixel aspect and the clip's Motion).
    static std::optional<Placement> placementOf(const ProjectState& project, const Sequence& sequence, const Clip& clip,
                                                Ticks curveTime);
    /// The Source Monitor's picture: its private clip's placement at the source position (Codex wave-4 P1-2: the
    /// redaction tool works in displayed fractions, redactions are stored in stored-frame fractions).
    std::optional<Placement> sourcePlacement() const;
    /// Applies `mask` to its effect as an edit (`mergeKey` for gestures).
    bool commitMask(EffectInstanceId effect, const Mask& mask, const char* label, const std::string& mergeKey = {});
    bool addMaskWith(Mask mask, const char* label);
    bool editMask(const QString& key, const char* label, const std::function<void(Mask&, Ticks)>& change,
                  const std::string& mergeKey = {});
    bool editRedaction(const QString& key, const char* label, const std::function<void(Redaction&)>& change);
    void finishTrack(bool ok, const QString& message, std::optional<ProjectState> result, ClipId clip, EffectInstanceId effect,
                     std::uint32_t mask, Ticks lastTimeline);

    bool m_attached = false;
    ClipId m_clip;
    /// m_clip is the selection's video clip (else the clip under the playhead, or none).
    bool m_targetFromSelection = false;
    QString m_clipName;
    QVariantList m_targets;
    QString m_target;
    QVariantList m_masks;
    QString m_selected;
    QVariantMap m_current;
    QString m_tool;
    QVariantList m_outlines;
    QVariantList m_handles;
    QVariantList m_featherOuter;
    QVariantList m_featherInner;
    std::vector<MaskVertex> m_pen; ///< frame-normalised points of the path being drawn
    QString m_status;

    // A handle drag.
    QString m_dragKind;
    int m_dragIndex = -1;
    double m_dragOffsetX = 0.0;
    double m_dragOffsetY = 0.0;
    int m_dragSerial = 0;

    // Tracking.
    bool m_tracking = false;
    double m_trackProgress = 0.0;
    int m_trackTotal = 0;
    QVariantList m_confidence;
    QString m_trackStatus;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    std::jthread m_worker;
    bool m_synchronous = false;
    std::shared_ptr<masks::TrackingServices> m_trackingServices;

    // Redactions.
    QVariantList m_redactions;
    QVariantList m_activeRegions;
    bool m_redactTool = false;
    QString m_redactStyle = QStringLiteral("blur");
    QString m_redactAudio = QStringLiteral("none");
};

} // namespace wabcut::app
