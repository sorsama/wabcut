#pragma once

// SequenceTimeBar (0.1.3): the Program Monitor's time bar (Premiere's monitor time ruler). It maps the current
// sequence - all of it, or a zoomed window of it - onto its width and draws, as one batched mesh, where the sequence
// has content (gaps show), its edit points, its markers, the In/Out range and time ticks. The playhead, the hover
// readout, the zoom bar and the input are QML on top (ProgramTimeBar.qml); a playhead move never rebuilds the mesh,
// only the x the QML playhead reads (playheadX).
//
// Times stay in C++ (principle 9): QML seeks and zooms by x (seekToX, zoomAt) and reads x positions back.

#include "ProjectController.h"

#include <QColor>
#include <QPointer>
#include <QQuickItem>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

namespace wabcut::app {

class SequenceTimeBar : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(wabcut::app::ProjectController* controller READ controller WRITE setController NOTIFY controllerChanged
                   FINAL)
    // ---- Colours (Theme tokens) ----
    Q_PROPERTY(QColor contentColor READ contentColor WRITE setContentColor NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(QColor editColor READ editColor WRITE setEditColor NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(QColor tickColor READ tickColor WRITE setTickColor NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(QColor rangeColor READ rangeColor WRITE setRangeColor NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(QColor markColor READ markColor WRITE setMarkColor NOTIFY appearanceChanged FINAL)
    /// Theme.markerColors (a marker's colorIndex picks one).
    Q_PROPERTY(QVariantList markerColors READ markerColors WRITE setMarkerColors NOTIFY appearanceChanged FINAL)
    // ---- The visible window: fractions [0, 1] of the bar's duration (the zoom) ----
    Q_PROPERTY(double viewStart READ viewStart NOTIFY viewChanged FINAL)
    Q_PROPERTY(double viewEnd READ viewEnd NOTIFY viewChanged FINAL)
    Q_PROPERTY(bool zoomed READ zoomed NOTIFY viewChanged FINAL)
    // ---- Positions for the QML overlays (x in this item; NaN when outside the window or unset) ----
    Q_PROPERTY(double playheadX READ playheadX NOTIFY playheadXChanged FINAL)
    Q_PROPERTY(double inX READ inX NOTIFY marksChanged FINAL)
    Q_PROPERTY(double outX READ outX NOTIFY marksChanged FINAL)
    Q_PROPERTY(bool hasIn READ hasIn NOTIFY marksChanged FINAL)
    Q_PROPERTY(bool hasOut READ hasOut NOTIFY marksChanged FINAL)
    /// A sequence is shown (false: nothing to seek in).
    Q_PROPERTY(bool active READ active NOTIFY layoutChanged FINAL)

public:
    explicit SequenceTimeBar(QQuickItem* parent = nullptr);

    ProjectController* controller() const { return m_controller; }
    void setController(ProjectController* controller);

    QColor contentColor() const { return m_contentColor; }
    void setContentColor(const QColor& c) { setColor(m_contentColor, c); }
    QColor editColor() const { return m_editColor; }
    void setEditColor(const QColor& c) { setColor(m_editColor, c); }
    QColor tickColor() const { return m_tickColor; }
    void setTickColor(const QColor& c) { setColor(m_tickColor, c); }
    QColor rangeColor() const { return m_rangeColor; }
    void setRangeColor(const QColor& c) { setColor(m_rangeColor, c); }
    QColor markColor() const { return m_markColor; }
    void setMarkColor(const QColor& c) { setColor(m_markColor, c); }
    QVariantList markerColors() const { return m_markerColorList; }
    void setMarkerColors(const QVariantList& colors);

    double viewStart() const { return m_viewStart; }
    double viewEnd() const { return m_viewEnd; }
    bool zoomed() const { return m_viewStart > 0 || m_viewEnd < 1; }
    double playheadX() const;
    double inX() const;
    double outX() const;
    bool hasIn() const;
    bool hasOut() const;
    bool active() const;

    /// Moves the playhead to the frame under x (clamped to the sequence).
    Q_INVOKABLE void seekToX(double x);
    /// The timecode of the frame under x ("" without a sequence), for the hover readout.
    Q_INVOKABLE QString timecodeAt(double x) const;
    /// 0.2 UX (stream T), the marker hover tooltip: the sequence marker nearest x within `reach` px (a range marker
    /// anywhere along its bar) as { index, x, endX, name, comment, timecode, durationText ("" for a point marker),
    /// colorIndex, type }; {} when none is that close.
    Q_INVOKABLE QVariantMap markerAt(double x, double reach = 6.0) const;
    /// Zooms the window by `factor` (> 1 in) keeping the time under x in place; the whole sequence at most.
    Q_INVOKABLE void zoomAt(double factor, double x);
    /// Sets the window (fractions; kept in [0, 1] and at least a few frames wide).
    Q_INVOKABLE void setView(double start, double end);
    /// Pans the window by `fraction` of the whole duration.
    Q_INVOKABLE void panBy(double fraction);
    Q_INVOKABLE void resetZoom() { setView(0, 1); }

    /// Rectangles in the last mesh (tests).
    int lastRectCount() const { return m_lastRects; }
    /// The bar's duration: the sequence's length (at least its content, at least a few seconds).
    Ticks span() const;

signals:
    void controllerChanged();
    void appearanceChanged();
    void viewChanged();
    void playheadXChanged();
    void marksChanged();
    void layoutChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    void setColor(QColor& member, const QColor& value);
    void markDirty();
    void onPlayhead();
    void onSequence();
    /// x of time t in the current window (may lie outside [0, width]).
    double xOf(Ticks t) const;
    Ticks timeAtX(double x) const;

    QPointer<ProjectController> m_controller;
    QColor m_contentColor{0x8A, 0x82, 0xA0, 0x60};
    QColor m_editColor{0x8A, 0x82, 0xA0};
    QColor m_tickColor{0x8A, 0x82, 0xA0, 0x60};
    QColor m_rangeColor{0x8B, 0x5C, 0xFF, 0x24};
    QColor m_markColor{0x8B, 0x5C, 0xFF};
    QVariantList m_markerColorList;
    std::vector<QColor> m_markerColors;
    double m_viewStart = 0;
    double m_viewEnd = 1;
    bool m_dirty = true;
    int m_lastRects = 0;
};

} // namespace wabcut::app
