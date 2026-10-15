#include "SliceShape.h"

#include <QQuickWindow>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace wabcut::app {

namespace {

constexpr int kArcSegments = 5;

void arc(QList<QPointF>& out, QPointF centre, qreal radius, qreal fromDeg, qreal toDeg) {
    if (radius <= 0.01) {
        out.append(centre);
        return;
    }
    for (int i = 0; i <= kArcSegments; ++i) {
        const qreal a = (fromDeg + (toDeg - fromDeg) * i / kArcSegments) * std::numbers::pi / 180.0;
        out.append(QPointF(centre.x() + radius * std::cos(a), centre.y() + radius * std::sin(a)));
    }
}

/// Premultiplied vertex colour.
QSGGeometry::ColoredPoint2D vertex(QPointF p, const QColor& c, qreal alpha) {
    const qreal a = c.alphaF() * alpha;
    QSGGeometry::ColoredPoint2D v;
    v.set(static_cast<float>(p.x()), static_cast<float>(p.y()), static_cast<uchar>(std::lround(c.redF() * a * 255)),
          static_cast<uchar>(std::lround(c.greenF() * a * 255)), static_cast<uchar>(std::lround(c.blueF() * a * 255)),
          static_cast<uchar>(std::lround(a * 255)));
    return v;
}

QColor mix(const QColor& a, const QColor& b, qreal t) {
    return QColor::fromRgbF(static_cast<float>(a.redF() + (b.redF() - a.redF()) * t),
                            static_cast<float>(a.greenF() + (b.greenF() - a.greenF()) * t),
                            static_cast<float>(a.blueF() + (b.blueF() - a.blueF()) * t),
                            static_cast<float>(a.alphaF() + (b.alphaF() - a.alphaF()) * t));
}

} // namespace

SliceShape::SliceShape(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
}

template <class T>
void SliceShape::assign(T& field, const T& value) {
    if (field == value)
        return;
    field = value;
    m_dirty = true;
    update();
    emit shapeChanged();
}

void SliceShape::setKind(Kind kind) { assign(m_kind, kind); }
void SliceShape::setColor(const QColor& color) { assign(m_color, color); }
void SliceShape::setBeam(bool beam) { assign(m_beam, beam); }
void SliceShape::setBeamStart(const QColor& color) { assign(m_beamStart, color); }
void SliceShape::setBeamMid(const QColor& color) { assign(m_beamMid, color); }
void SliceShape::setBeamEnd(const QColor& color) { assign(m_beamEnd, color); }
void SliceShape::setRadius(qreal radius) { assign(m_radius, radius); }
void SliceShape::setDepth(qreal depth) { assign(m_depth, depth); }
void SliceShape::setAngle(qreal angle) { assign(m_angle, angle); }
void SliceShape::setMirrored(bool mirrored) { assign(m_mirrored, mirrored); }

void SliceShape::geometryChange(const QRectF& next, const QRectF& previous) {
    QQuickItem::geometryChange(next, previous);
    if (next.size() != previous.size()) {
        m_dirty = true;
        update();
    }
}

QList<QPointF> SliceShape::outline(Kind kind, QSizeF size, qreal radius, qreal depth, qreal angleDegrees, bool mirrored) {
    const qreal w = size.width();
    const qreal h = size.height();
    const qreal tanA = std::tan(angleDegrees * std::numbers::pi / 180.0);
    QList<QPointF> pts;
    if (kind == Kind::Lean) {
        const qreal lean = std::min(w, h * tanA);
        pts = {QPointF(lean, 0), QPointF(w, 0), QPointF(w - lean, h), QPointF(0, h)};
    } else {
        const qreal d = std::clamp(depth, 0.0, w);
        const qreal cutDrop = tanA > 0 ? std::min(h, d / tanA) : h;
        const qreal bottomInset = std::max(0.0, d - cutDrop * tanA);
        const qreal r = std::max(0.0, std::min({radius, h / 2, w / 2}));
        const qreal rCut = bottomInset > 0 ? 0.0 : std::max(0.0, std::min(r, h - cutDrop));
        const qreal sideX = w - bottomInset;
        arc(pts, QPointF(r, r), r, 180, 270);                       // top-left
        if (d > 0.01) {
            pts.append(QPointF(w - d, 0));                            // the slice starts
            pts.append(QPointF(sideX, cutDrop));                      // and meets the side (or the bottom)
        } else {
            arc(pts, QPointF(w - r, r), r, 270, 360);               // an ordinary top-right corner
        }
        arc(pts, QPointF(sideX - rCut, h - rCut), rCut, 0, 90);     // bottom-right
        arc(pts, QPointF(r, h - r), r, 90, 180);                    // bottom-left
    }
    // Drop points that coincide (zero-length edges break the normals).
    QList<QPointF> clean;
    for (const QPointF& p : pts) {
        if (clean.isEmpty() || QLineF(clean.back(), p).length() > 0.01)
            clean.append(p);
    }
    if (clean.size() > 1 && QLineF(clean.front(), clean.back()).length() <= 0.01)
        clean.removeLast();
    if (mirrored) {
        for (QPointF& p : clean)
            p.setX(w - p.x());
        std::reverse(clean.begin(), clean.end()); // keep it clockwise
    }
    return clean;
}

QSGNode* SliceShape::updatePaintNode(QSGNode* old, UpdatePaintNodeData*) {
    if (width() <= 0 || height() <= 0) {
        delete old;
        return nullptr;
    }
    auto* node = static_cast<QSGGeometryNode*>(old);
    if (!node) {
        node = new QSGGeometryNode;
        auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0, 0,
                                         QSGGeometry::UnsignedShortType);
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        node->setGeometry(geometry);
        node->setFlag(QSGNode::OwnsGeometry);
        node->setMaterial(new QSGVertexColorMaterial);
        node->setFlag(QSGNode::OwnsMaterial);
        m_dirty = true;
    }
    if (!m_dirty)
        return node;
    m_dirty = false;

    const QList<QPointF> pts = outline(m_kind, size(), m_radius, m_depth, m_angle, m_mirrored);
    const int n = static_cast<int>(pts.size());
    QSGGeometry* g = node->geometry();
    if (n < 3) {
        g->allocate(0, 0);
        node->markDirty(QSGNode::DirtyGeometry);
        return node;
    }
    // One device pixel of feather, half inside and half outside the outline.
    const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    const qreal feather = 0.5 / std::max<qreal>(1.0, dpr);
    const auto colourAt = [&](qreal x) {
        if (!m_beam)
            return m_color;
        const qreal t = std::clamp(x / std::max<qreal>(1.0, width()), 0.0, 1.0);
        return t < 0.55 ? mix(m_beamStart, m_beamMid, t / 0.55) : mix(m_beamMid, m_beamEnd, (t - 0.55) / 0.45);
    };

    // Vertices: the centre, then n inner and n outer ring vertices.
    g->allocate(1 + 2 * n, 3 * n * 3);
    QSGGeometry::ColoredPoint2D* v = g->vertexDataAsColoredPoint2D();
    quint16* idx = g->indexDataAsUShort();
    QPointF centre;
    for (const QPointF& p : pts)
        centre += p;
    centre /= n;
    v[0] = vertex(centre, colourAt(centre.x()), 1.0);
    for (int i = 0; i < n; ++i) {
        const QPointF prev = pts[(i + n - 1) % n];
        const QPointF here = pts[i];
        const QPointF next = pts[(i + 1) % n];
        // Outward normals of the two edges (clockwise outline in y-down coordinates: left-hand normals point out).
        const auto normal = [](QPointF a, QPointF b) {
            const QPointF d = b - a;
            const qreal len = std::hypot(d.x(), d.y());
            return len > 0 ? QPointF(d.y() / len, -d.x() / len) : QPointF();
        };
        const QPointF n1 = normal(prev, here);
        const QPointF n2 = normal(here, next);
        QPointF m = n1 + n2;
        const qreal mlen = std::hypot(m.x(), m.y());
        m = mlen > 0 ? m / mlen : n1;
        const qreal cosHalf = std::max(0.25, m.x() * n1.x() + m.y() * n1.y());
        const QPointF offset = m * (feather / cosHalf);
        const QColor c = colourAt(here.x());
        v[1 + i] = vertex(here - offset, c, 1.0);
        v[1 + n + i] = vertex(here + offset, c, 0.0);
    }
    int k = 0;
    for (int i = 0; i < n; ++i) {
        const auto inner = static_cast<quint16>(1 + i);
        const auto innerNext = static_cast<quint16>(1 + (i + 1) % n);
        const auto outer = static_cast<quint16>(1 + n + i);
        const auto outerNext = static_cast<quint16>(1 + n + (i + 1) % n);
        idx[k++] = 0;
        idx[k++] = inner;
        idx[k++] = innerNext;
        idx[k++] = inner;
        idx[k++] = outer;
        idx[k++] = outerNext;
        idx[k++] = inner;
        idx[k++] = outerNext;
        idx[k++] = innerNext;
    }
    node->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
    return node;
}

} // namespace wabcut::app
