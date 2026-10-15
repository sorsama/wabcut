#pragma once

// EditCursors (0.1.3): the editor's tool cursors, so the pointer says what a press will do, on hover and while the
// press holds: trim and ripple brackets on clip edges, roll, rate stretch, slip and slide, the razor, the pen, track
// select, the hand and zoom tools, moving and inserting clips, keyframes and rubber bands.
//
// Every cursor is drawn from Boxicons glyphs (src/app/resources/cursors, MIT; see the LICENSE there): a few layers on
// a 32 px canvas (a bracket half, an arrow, a badge), filled with ink or a trim colour (Premiere's red trims and
// yellow ripples, the same colours as the trim edit points), outlined in white over a soft shadow so it reads on dark
// lanes and light monitors alike, and rendered at the screen's device pixel ratio. Cursors are cached per key, ratio
// and colours. C++ items call setCursor(editcursors::cursor(...)); QML uses CursorArea:
//
//   CursorArea { anchors.fill: parent; shape: pressed ? "grab" : "hand" }
//
// Keys: move, insert, hand, grab, zoomIn, zoomOut, point, vertical, razor, pen, penAdd, trackForward,
// trackBackward, trimHead, trimTail, rippleHead, rippleTail, roll, rateHead, rateTail, slip, slide, remix, and the
// workbench's resizeH / resizeV (gutters, track heights).
// editcursors::cursor() gives the platform's arrow for "" (or an unknown key); a CursorArea with "" has no cursor of its
// own, so what lies under it shows its cursor.

#include <QColor>
#include <QCursor>
#include <QImage>
#include <QPoint>
#include <QQuickItem>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

namespace wabcut::app {

/// The tints of the trim cursors (TimelineStyle's trimRollColor and trimRippleColor).
struct EditCursorColors {
    QColor trim{0xF4, 0x3F, 0x5E};
    QColor ripple{0xFB, 0xBF, 0x24};
    bool operator==(const EditCursorColors&) const = default;
};

namespace editcursors {

/// Every cursor key, in a stable order (tests and the cursor sheet).
QStringList keys();
bool known(const QString& key);
/// The cursor for `key` at device pixel ratio `dpr`; the arrow for "" and unknown keys.
QCursor cursor(const QString& key, qreal dpr = 1.0, const EditCursorColors& colors = {});
/// The picture behind a cursor (ARGB32 premultiplied, its device pixel ratio set): what tests and the sheet look at.
/// Null for "" and unknown keys.
QImage image(const QString& key, qreal dpr = 1.0, const EditCursorColors& colors = {});
/// The hot spot in logical pixels of the 32 px canvas.
QPoint hotSpot(const QString& key);

} // namespace editcursors

/// A QML item that only sets the pointer over its area to an edit cursor (it takes no presses and no hover, so
/// whatever lies under it keeps working). Disabled (itself or through a parent), invisible or with `shape` "", it has
/// no cursor at all and the items under it show theirs: Qt Quick's cursor lookup would not skip a disabled item that
/// keeps a cursor.
class CursorArea : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    /// An edit cursor key (see the header), or "" for none of its own.
    Q_PROPERTY(QString shape READ shape WRITE setShape NOTIFY shapeChanged FINAL)

public:
    explicit CursorArea(QQuickItem* parent = nullptr);

    QString shape() const { return m_shape; }
    void setShape(const QString& shape);

signals:
    void shapeChanged();

protected:
    void itemChange(ItemChange change, const ItemChangeData& data) override;

private:
    void apply();

    QString m_shape;
};

} // namespace wabcut::app
