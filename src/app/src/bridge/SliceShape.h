#pragma once

// SliceShape: the slice (design language v2), drawn in the scene graph. Used by SliceRect.qml and SliceMark.qml.
//
//   kind Slice  a rectangle whose top-right corner (top-left when `mirrored`) is cut by a straight line at `angle`
//               degrees from the vertical, starting `depth` px in from the corner; the other corners are rounded by
//               `radius` (the bottom corner under the cut keeps what roundness the cut leaves it).
//   kind Lean   a parallelogram whose left and right edges lean by `angle` (the slice mark, the transition band).
//
// One convex outline, triangulated as a fan, with a one-device-pixel feather around it (vertex antialiasing, no
// multisampling, so it renders the same on every backend and in offscreen grabs). Filled with `color`, or with the UV
// beam (`beam`: beamStart → beamMid → beamEnd, left to right) through per-vertex colours. The geometry is rebuilt only
// when the size or a property changes.

#include <QColor>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

namespace wabcut::app {

class SliceShape : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(Kind kind READ kind WRITE setKind NOTIFY shapeChanged FINAL)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY shapeChanged FINAL)
    Q_PROPERTY(bool beam READ beam WRITE setBeam NOTIFY shapeChanged FINAL)
    Q_PROPERTY(QColor beamStart READ beamStart WRITE setBeamStart NOTIFY shapeChanged FINAL)
    Q_PROPERTY(QColor beamMid READ beamMid WRITE setBeamMid NOTIFY shapeChanged FINAL)
    Q_PROPERTY(QColor beamEnd READ beamEnd WRITE setBeamEnd NOTIFY shapeChanged FINAL)
    Q_PROPERTY(qreal radius READ radius WRITE setRadius NOTIFY shapeChanged FINAL)
    Q_PROPERTY(qreal depth READ depth WRITE setDepth NOTIFY shapeChanged FINAL)
    Q_PROPERTY(qreal angle READ angle WRITE setAngle NOTIFY shapeChanged FINAL)
    Q_PROPERTY(bool mirrored READ mirrored WRITE setMirrored NOTIFY shapeChanged FINAL)

public:
    enum class Kind { Slice, Lean };
    Q_ENUM(Kind)

    explicit SliceShape(QQuickItem* parent = nullptr);

    Kind kind() const { return m_kind; }
    void setKind(Kind kind);
    QColor color() const { return m_color; }
    void setColor(const QColor& color);
    bool beam() const { return m_beam; }
    void setBeam(bool beam);
    QColor beamStart() const { return m_beamStart; }
    void setBeamStart(const QColor& color);
    QColor beamMid() const { return m_beamMid; }
    void setBeamMid(const QColor& color);
    QColor beamEnd() const { return m_beamEnd; }
    void setBeamEnd(const QColor& color);
    qreal radius() const { return m_radius; }
    void setRadius(qreal radius);
    qreal depth() const { return m_depth; }
    void setDepth(qreal depth);
    qreal angle() const { return m_angle; }
    void setAngle(qreal angle);
    bool mirrored() const { return m_mirrored; }
    void setMirrored(bool mirrored);

    /// The outline (clockwise, convex) for a size and these parameters; exposed for tests.
    static QList<QPointF> outline(Kind kind, QSizeF size, qreal radius, qreal depth, qreal angleDegrees, bool mirrored);

signals:
    void shapeChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData* data) override;
    void geometryChange(const QRectF& next, const QRectF& previous) override;

private:
    template <class T>
    void assign(T& field, const T& value);

    Kind m_kind = Kind::Slice;
    QColor m_color{Qt::white};
    bool m_beam = false;
    QColor m_beamStart{0x38, 0x00, 0xBA};
    QColor m_beamMid{0x7B, 0x3B, 0xFF};
    QColor m_beamEnd{0xE0, 0x40, 0xFB};
    qreal m_radius = 6;
    qreal m_depth = 6;
    qreal m_angle = 12;
    bool m_mirrored = false;
    bool m_dirty = true;
};

} // namespace wabcut::app
