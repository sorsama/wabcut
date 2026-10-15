#pragma once

// TrimBridge: Premiere's trim mode, for QML (singleton `Trim`, M5i).
//
//   * Edit points: an edit point is a clip's head or tail. A click on an edge (TimelineView.editPointClicked) selects
//     one (Shift adds one on another track); Trim Edit (Shift+T) selects the edit points nearest the playhead on the
//     targeted tracks (every track when none is targeted) as a roll and opens the Trim view. Linked partners' edges at
//     the same time come along when linked selection is on.
//   * Trim type: "regular" (one side, nothing moves), "ripple" (one side, later material follows) or "roll" (both
//     sides, the cut moves). Toggle Trim Type (Shift+T in the Timeline) cycles roll → ripple → regular; with nothing
//     selected it is Trim Edit.
//   * Trims: Trim Backward / Forward (1 frame) and Many (the Large trim offset, default 5), the Trim view's buttons
//     and dynamic trimming all use the model's trim ops (edit::trimEdge, rippleTrim, rollEdit) on every edit point,
//     one undo step per trim. The first edit point's applied (clamped) delta is used for the others, so they stay in
//     sync.
//   * Dynamic trimming (J/K/L while edit points are selected): J or L plays from the edit point and the trim follows
//     playback; K stops and trims by how far it played. K+J/L taps trim a frame at a time.
//   * Play around: plays from the preroll before the edit to the postroll after it, in a loop, until stopped.
//   * The Trim view (Program Monitor): the outgoing and incoming frames (each rendered with only its track visible),
//     their source timecodes and the offsets trimmed since entering trim mode.
//
// Principle 9: QML sees clip keys ("c…"), text and frame counts.

#include <wabcut/model/EditOps.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/Time.h>

#include <QImage>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <optional>
#include <vector>

class QJSEngine;
class QQmlEngine;

namespace wabcut::app {

class PlaybackBridge;
class ProgramToolsBridge;
class ProjectController;

class TrimBridge : public QObject {
    Q_OBJECT
    /// Edit points are selected.
    Q_PROPERTY(bool active READ active NOTIFY pointsChanged FINAL)
    /// [{ clip: "c…", edge: "head"|"tail" }] for TimelineView.trimPoints.
    Q_PROPERTY(QVariantList points READ points NOTIFY pointsChanged FINAL)
    /// "regular", "ripple" or "roll".
    Q_PROPERTY(QString type READ type WRITE setType NOTIFY pointsChanged FINAL)
    /// The Program Monitor shows the Trim view (Trim Edit opened it; closes with the edit points).
    Q_PROPERTY(bool viewOpen READ viewOpen NOTIFY pointsChanged FINAL)
    Q_PROPERTY(bool dynamic READ dynamic NOTIFY dynamicChanged FINAL)
    Q_PROPERTY(bool playingAround READ playingAround NOTIFY dynamicChanged FINAL)
    /// The first edit point: the clip names on each side ("" for a gap), the source timecodes of the outgoing clip's last
    /// frame and the incoming clip's first frame, and the edit's sequence timecode.
    Q_PROPERTY(QString outgoingName READ outgoingName NOTIFY detailsChanged FINAL)
    Q_PROPERTY(QString incomingName READ incomingName NOTIFY detailsChanged FINAL)
    Q_PROPERTY(QString outgoingText READ outgoingText NOTIFY detailsChanged FINAL)
    Q_PROPERTY(QString incomingText READ incomingText NOTIFY detailsChanged FINAL)
    Q_PROPERTY(QString editText READ editText NOTIFY detailsChanged FINAL)
    /// Frames the outgoing Out and the incoming In moved since trim mode started (Premiere's trim offsets).
    Q_PROPERTY(int outgoingOffset READ outgoingOffset NOTIFY detailsChanged FINAL)
    Q_PROPERTY(int incomingOffset READ incomingOffset NOTIFY detailsChanged FINAL)
    /// The Trim view's pictures (null until rendered; rendered off the GUI thread).
    Q_PROPERTY(QImage outgoingFrame READ outgoingFrame NOTIFY framesChanged FINAL)
    Q_PROPERTY(QImage incomingFrame READ incomingFrame NOTIFY framesChanged FINAL)
    /// Preferences: the Large trim offset and the play-around preroll/postroll (seconds).
    Q_PROPERTY(int largeOffset READ largeOffset WRITE setLargeOffset NOTIFY settingsChanged FINAL)
    Q_PROPERTY(double preroll READ preroll WRITE setPreroll NOTIFY settingsChanged FINAL)
    Q_PROPERTY(double postroll READ postroll WRITE setPostroll NOTIFY settingsChanged FINAL)
    /// Linked partners' edges come along (TimelineView.linkedSelection).
    Q_PROPERTY(bool linkedSelection READ linkedSelection WRITE setLinkedSelection NOTIFY settingsChanged FINAL)

public:
    enum class Type { Regular, Ripple, Roll };

    explicit TrimBridge(QObject* parent = nullptr);
    static TrimBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);
    void setController(ProjectController* controller);
    void setPlayback(PlaybackBridge* playback);
    void setProgramTools(ProgramToolsBridge* tools);

    bool active() const { return !m_points.empty(); }
    QVariantList points() const;
    QString type() const;
    void setType(const QString& type);
    bool viewOpen() const { return m_viewOpen && active(); }
    bool dynamic() const { return m_dynamic; }
    bool playingAround() const { return m_loop.isActive(); }
    QString outgoingName() const;
    QString incomingName() const;
    QString outgoingText() const;
    QString incomingText() const;
    QString editText() const;
    int outgoingOffset() const { return m_outOffset; }
    int incomingOffset() const { return m_inOffset; }
    QImage outgoingFrame() const { return m_outFrame; }
    QImage incomingFrame() const { return m_inFrame; }
    int largeOffset() const { return m_largeOffset; }
    void setLargeOffset(int frames);
    double preroll() const { return m_preroll; }
    void setPreroll(double seconds);
    double postroll() const { return m_postroll; }
    void setPostroll(double seconds);
    bool linkedSelection() const { return m_linked; }
    void setLinkedSelection(bool on);

    /// Trim Edit (Shift+T): the edit points nearest the playhead on the targeted tracks, as a roll; opens the Trim view.
    Q_INVOKABLE bool trimEdit();
    /// Toggle Trim Type (Shift+T in the Timeline): cycles the type, or Trim Edit when nothing is selected.
    Q_INVOKABLE void toggleType();
    /// A click on a clip edge (TimelineView.editPointClicked): selects that edit point; Shift adds it.
    Q_INVOKABLE void selectEditPoint(const QString& clipId, const QString& edge, const QString& type, int modifiers);
    /// Leaves trim mode.
    Q_INVOKABLE void clear();
    Q_INVOKABLE void openView();
    /// Trims every edit point by `frames` (+ = later). False, with nothing changed, when no trim is possible.
    Q_INVOKABLE bool trim(int frames);
    /// Runs a keymap command this bridge owns (sequence.toggleTrimType, sequence.trimBackward / Forward (Many),
    /// sequence.trimEdit); false for others.
    Q_INVOKABLE bool runCommand(const QString& commandId);
    /// J/K/L while edit points are selected (KeymapDispatcher.transportIntent): dynamic trimming.
    Q_INVOKABLE void transportIntent(int kind, int speedNum, int speedDen, int frames);
    /// Plays around the edit in a loop; stopAround() (or K) ends it.
    Q_INVOKABLE void playAround();
    Q_INVOKABLE void stopAround();
    /// Renders the Trim view's pictures again (after a trim; QML calls it when the view opens).
    Q_INVOKABLE void refreshFrames();

    /// C++ / tests: the first edit point's cut, if any.
    std::optional<Ticks> editTime() const;

signals:
    void pointsChanged();
    void detailsChanged();
    void framesChanged();
    void dynamicChanged();
    void settingsChanged();

private:
    struct Point {
        ClipId clip;
        bool head = false;
    };
    struct Sides {
        const Clip* outgoing = nullptr;
        const Clip* incoming = nullptr;
        TrackId track;
        Ticks cut;
    };
    std::optional<Sides> sidesOf(const Point& p) const;
    void setPoints(std::vector<Point> points);
    /// Adds the linked partners' edges at the same time.
    std::vector<Point> withLinkedEdges(std::vector<Point> points) const;
    /// The trim on the given state (one op per edit point not covered by an earlier one's links).
    edit::EditOutcome applyTrim(const ProjectState& state, Ticks delta, Ticks* applied) const;
    void onModelChanged();
    void onSelectionChanged();
    void loopTick();
    void finishDynamic();

    QPointer<ProjectController> m_controller;
    SequenceId m_sequence; ///< the sequence the edit points are in (switching sequences leaves trim mode)
    QPointer<PlaybackBridge> m_playback;
    QPointer<ProgramToolsBridge> m_tools;
    std::vector<Point> m_points;
    Type m_type = Type::Roll;
    bool m_viewOpen = false;
    bool m_dynamic = false;
    Ticks m_dynamicStart; ///< the playhead (the cut) when dynamic trimming started
    int m_outOffset = 0;
    int m_inOffset = 0;
    int m_largeOffset = 5;
    double m_preroll = 2.0;
    double m_postroll = 2.0;
    bool m_linked = true;
    bool m_selecting = false; ///< our own selection changes do not leave trim mode
    QTimer m_loop;
    Ticks m_loopFrom, m_loopTo;
    QImage m_outFrame, m_inFrame;
    int m_frameSerial = 0;
};

struct TrimSingleton {
    Q_GADGET
    QML_FOREIGN(wabcut::app::TrimBridge)
    QML_NAMED_ELEMENT(Trim)
    QML_SINGLETON
public:
    static TrimBridge* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine) { return TrimBridge::create(qmlEngine, jsEngine); }
};

} // namespace wabcut::app
