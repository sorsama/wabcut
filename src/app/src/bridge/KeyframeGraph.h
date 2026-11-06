#pragma once

// KeyframeGraph: the Graph Editor's engine (0.7, card 96; ProjectController.effectControls.graph; docs/effects.md "Graph
// Editor"). The same keyframes as Effect Controls' lanes, drawn as curves in a panel of their own that can be docked as big as
// the user likes: the animated parameters of the clip Effect Controls shows, several at once, each in its own colour with a
// legend; the value graph and, below it, the speed graph (the curve's rate of change, with the speed handles of Bezier keys).
// Everything it edits is the clip's own curves through the Effect Controls model's edit path - one undo step per gesture,
// the key selection shared with the lanes - so the lanes, the inline value graphs and this editor are three views of one thing.
//
//   * Coordinates. x is a fraction of the clip (0..1, the lanes' x). A curve's value axis is normalised to its own range, y
//     = 0 at the top: curves of different units (a Scale in %, a Position in pixels) share the panel and each fills the
//     height; "Shared scale" gives every curve the union range instead. The panel's view window (viewStart..viewEnd in x,
//     valueStart..valueEnd in y) is zoomed and panned here; the data below is in the full 0..1 space and QML maps it.
//   * `curves`: [{ row, title, section, color (0..), active, lo, hi (the value at the bottom / top, UI units), unit,
//     decimals, path: [x0, y0, x1, y1, ...] (value graph), speedPath (the same x, y = speed), speedMax (UI units a second at the
//     speed graph's top), keys: [{ index, x, y, selected, shape, hasIn/inX/inY, hasOut/outX/outY (value handles, selected
//     Bezier keys only), speedY, sHasIn/sInX/sInY, sHasOut/sOutX/sOutY (speed handles), broken, timeText, valueText }] }].
//   * `legend`: [{ row, title, section, color, visible, active, keys, valueText }] - every animated parameter of the clip.
//   * Pointer gestures: press() (a key, a handle or nothing), dragTo() (moves the selected keys in time and value, or the
//     handle), release(), boxSelect(), addKeyAt(); the view: zoomTime/zoomValue/panTime/panValue/fit.
//   * Edits: setSelectionValue / setSelectionTime (numeric entry), nudge, ease(preset) (model/KeyframeEasing.h), and
//     the interpolation menu of Effect Controls (effectControls.setKeyInterpolation) applies to the same selection.

#include <wabcut/model/Ids.h>
#include <wabcut/model/Time.h>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <map>
#include <set>
#include <string>

namespace wabcut::app {

class EffectControlsModel;

class KeyframeGraph : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by ProjectController.effectControls.graph")

    /// The panel is open: the curves are kept up to date (nothing is computed while it is closed).
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged FINAL)
    Q_PROPERTY(QVariantList legend READ legend NOTIFY changed FINAL)
    Q_PROPERTY(QVariantList curves READ curves NOTIFY changed FINAL)
    /// { hasClip, clipTitle, subtitle, lengthSeconds, frameSeconds, playhead (0..1 of the clip, -1 outside), selCount,
    ///   selRow, selTitle, selTimeText, selValue, selUnit, selDecimals, selFrom, selTo, selStep, selShape, selMixed }
    Q_PROPERTY(QVariantMap info READ info NOTIFY changed FINAL)
    /// The playhead in the clip, 0..1; -1 when it is outside the clip.
    Q_PROPERTY(double playhead READ playhead NOTIFY playheadChanged FINAL)
    Q_PROPERTY(double viewStart READ viewStart NOTIFY viewChanged FINAL)
    Q_PROPERTY(double viewEnd READ viewEnd NOTIFY viewChanged FINAL)
    Q_PROPERTY(double valueStart READ valueStart NOTIFY viewChanged FINAL)
    Q_PROPERTY(double valueEnd READ valueEnd NOTIFY viewChanged FINAL)
    Q_PROPERTY(bool speedGraph READ speedGraph WRITE setSpeedGraph NOTIFY viewChanged FINAL)
    Q_PROPERTY(bool sharedScale READ sharedScale WRITE setSharedScale NOTIFY viewChanged FINAL)

public:
    explicit KeyframeGraph(EffectControlsModel* controls);

    bool active() const { return m_active; }
    void setActive(bool on);
    QVariantList legend() const { return m_legend; }
    QVariantList curves() const { return m_curves; }
    QVariantMap info() const { return m_info; }
    double playhead() const;
    double viewStart() const { return m_viewStart; }
    double viewEnd() const { return m_viewEnd; }
    double valueStart() const { return m_valueStart; }
    double valueEnd() const { return m_valueEnd; }
    bool speedGraph() const { return m_speed; }
    void setSpeedGraph(bool on);
    bool sharedScale() const { return m_shared; }
    void setSharedScale(bool on);

    // ---- The view ----
    /// Zooms the time axis by `factor` (> 1 in) keeping `at` (0..1 across the shown window) where it is; at least three
    /// frames fill the window, the whole clip at the most.
    Q_INVOKABLE void zoomTime(double factor, double at);
    Q_INVOKABLE void panTime(double dx);
    /// The same for the value axis (`at` 0..1 down the shown window).
    Q_INVOKABLE void zoomValue(double factor, double at);
    Q_INVOKABLE void panValue(double dy);
    /// Fits the window to the selected keys (`selectionOnly`, when there are some) or to every key of the shown curves.
    Q_INVOKABLE void fit(bool selectionOnly);
    /// The whole clip, each curve over its whole range.
    Q_INVOKABLE void resetView();

    // ---- The legend ----
    Q_INVOKABLE void setCurveVisible(const QString& row, bool visible);
    /// The curve whose value axis the panel labels and whose handle the numeric fields name.
    Q_INVOKABLE void setActiveRow(const QString& row);
    Q_INVOKABLE void showAll();
    /// Only the parameters that have a selected key.
    Q_INVOKABLE void showSelected();
    /// Only `row`.
    Q_INVOKABLE void isolate(const QString& row);

    // ---- Pointer ----
    /// A press at (x, y) of `pane` ("value" or "speed"; x 0..1 of the clip, y 0..1 down the pane in the curve's own units),
    /// the hit radius (tolX of x, tolY of y). Returns "key" (selected as `modifiers` say - Shift adds, Ctrl toggles - and
    /// ready to drag), "handle" (a Bezier handle of a selected key, ready to drag) or "none" (QML draws a selection box).
    Q_INVOKABLE QString press(const QString& pane, double x, double y, double tolX, double tolY, int modifiers);
    /// Moves what press() took to (x, y): the selected keys by the distance from the press (whole frames in time; in value
    /// by the same share of every curve's height - the caller holds an axis by passing the press coordinate back), or the
    /// handle (Shift: level with its key; Alt: the two sides apart). One undo step per gesture, merged until release().
    Q_INVOKABLE void dragTo(const QString& pane, double x, double y, int modifiers);
    Q_INVOKABLE void release();
    /// Selects the keys inside the box of `pane` (Shift / Ctrl add to the selection); a box that is only a point clears.
    Q_INVOKABLE void boxSelect(const QString& pane, double x0, double y0, double x1, double y1, int modifiers);
    Q_INVOKABLE void clearSelection();
    /// A key on the curve nearest to (x, y) of the value pane, within tolY, at the frame under x with the value there.
    Q_INVOKABLE bool addKeyAt(double x, double y, double tolY);
    /// The playhead to x (0..1 of the clip).
    Q_INVOKABLE void seek(double x);
    /// The timecode at x (0..1 of the clip): the time ruler's labels.
    Q_INVOKABLE QString timeText(double x) const;
    /// What is under the pointer in words ("Opacity key at 00:00:02:00: 85 %", the curve's name, ""): the tooltip.
    Q_INVOKABLE QString describeAt(const QString& pane, double x, double y, double tolX, double tolY) const;

    // ---- Numeric entry and keys ----
    /// The value (UI units) of every selected key of the active curve; one undo step "Set Keyframe Value".
    Q_INVOKABLE bool setSelectionValue(double value);
    /// Moves the selected keys so that the first of them sits at the timecode `text` (whole frames; the others keep their
    /// distances); one undo step "Move Keyframes".
    Q_INVOKABLE bool setSelectionTime(const QString& text);
    /// Moves the selected keys by whole frames and/or `valueSteps` steps of their parameter; one undo step.
    Q_INVOKABLE void nudge(int frames, int valueSteps);
    /// Shapes the segment after every selected key like the easing preset `id` (KeyframeEasing.h); the number of segments.
    Q_INVOKABLE int ease(const QString& id);
    /// [{ id, name, group, x1, y1, x2, y2, thumb: [x0, y0, ...] }] in menu order.
    Q_INVOKABLE QVariantList easingPresets() const;

    /// EffectControlsModel::refresh() ran: rebuild what the panel shows (nothing when it is closed).
    void onRowsRefreshed();

signals:
    void activeChanged();
    /// The legend, the curves or the info changed.
    void changed();
    void viewChanged();
    void playheadChanged();

private:
    struct Press {
        bool down = false;
        QString kind; ///< "key" | "handle"
        QString pane;
        QString row;
        double x = 0, y = 0;
        bool started = false;
        std::size_t index = 0; ///< a handle's key
        bool out = true;
    };
    struct Window {
        double lo = 0, hi = 1; ///< a curve's value at the bottom and at the top of its normalised axis (UI units): v = hi - y (hi - lo)
        double speedMax = 1;
    };

    void rebuild(bool force);
    void setView(double start, double end, double vStart, double vEnd);
    double minSpan() const;
    const Window* windowOf(const QString& row) const;

    QPointer<EffectControlsModel> m_ec;
    bool m_active = false;
    bool m_speed = false;
    bool m_shared = false;
    double m_viewStart = 0, m_viewEnd = 1;
    double m_valueStart = 0, m_valueEnd = 1;
    QString m_activeRow;
    std::set<QString> m_hidden;
    ClipId m_clip;
    QVariantList m_legend, m_curves;
    QVariantMap m_info;
    std::map<QString, Window> m_windows;    ///< per shown curve, as built
    std::map<QString, Window> m_frozen;     ///< while a gesture runs: the windows it started with
    Press m_press;
    int m_builtRevision = -1;
    std::size_t m_builtSelection = 0;
    bool m_dirty = true;
    bool m_building = false;
};

} // namespace wabcut::app
