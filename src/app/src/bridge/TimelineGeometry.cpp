#include "TimelineGeometry.h"

#include <QSGVertexColorMaterial>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>

namespace wabcut::app {

namespace {

constexpr int kCornerSegments = 2;                    // points per corner = segments + 1
constexpr int kPerimeterPoints = 4 * (kCornerSegments + 1);

// cos/sin of the corner arc samples (quarter circle).
struct CornerTable {
    std::array<double, kCornerSegments + 1> cos{};
    std::array<double, kCornerSegments + 1> sin{};
    CornerTable() {
        for (int i = 0; i <= kCornerSegments; ++i) {
            const double a = (std::numbers::pi / 2.0) * i / kCornerSegments;
            cos[static_cast<std::size_t>(i)] = std::cos(a);
            sin[static_cast<std::size_t>(i)] = std::sin(a);
        }
    }
};
const CornerTable& corner() {
    static const CornerTable table;
    return table;
}

QColor transparentOf(QColor c) {
    c.setAlpha(0);
    return c;
}

} // namespace

QColor premultiplied(QColor c) {
    const int a = c.alpha();
    if (a == 255)
        return c;
    return QColor(c.red() * a / 255, c.green() * a / 255, c.blue() * a / 255, a);
}

MeshBuilder::Rgba MeshBuilder::rgba(QColor c) {
    return {static_cast<uchar>(c.red()), static_cast<uchar>(c.green()), static_cast<uchar>(c.blue()),
            static_cast<uchar>(c.alpha())};
}

std::uint32_t MeshBuilder::addRingVertices(const std::vector<QPointF>& points, Rgba c) {
    const auto first = static_cast<std::uint32_t>(m_vertices.size());
    for (const QPointF& p : points)
        addVertex(p.x(), p.y(), c);
    return first;
}

void MeshBuilder::addRect(const QRectF& r, QColor color) {
    if (r.width() <= 0 || r.height() <= 0 || color.alpha() == 0)
        return;
    const Rgba c = rgba(premultiplied(color));
    const std::uint32_t a = addVertex(r.left(), r.top(), c);
    addVertex(r.right(), r.top(), c);
    addVertex(r.right(), r.bottom(), c);
    addVertex(r.left(), r.bottom(), c);
    m_indices.insert(m_indices.end(), {a, a + 1, a + 2, a, a + 2, a + 3});
}

void MeshBuilder::addTriangle(QPointF p0, QPointF p1, QPointF p2, QColor color) {
    if (color.alpha() == 0)
        return;
    const Rgba c = rgba(premultiplied(color));
    const std::uint32_t a = addVertex(p0.x(), p0.y(), c);
    addVertex(p1.x(), p1.y(), c);
    addVertex(p2.x(), p2.y(), c);
    m_indices.insert(m_indices.end(), {a, a + 1, a + 2});
}

void MeshBuilder::perimeter(const QRectF& r, qreal radius, qreal offset, std::vector<QPointF>& out) const {
    out.clear();
    const qreal rad = std::clamp(radius, 0.0, std::min(r.width(), r.height()) / 2.0);
    // The offset grows (or shrinks) the radius around fixed corner centres. Shrunk past zero, a corner becomes
    // the square corner of the inset rectangle.
    const qreal grow = rad + offset;
    const CornerTable& t = corner();
    const QPointF centres[4] = {
        {r.left() + rad, r.top() + rad},
        {r.right() - rad, r.top() + rad},
        {r.right() - rad, r.bottom() - rad},
        {r.left() + rad, r.bottom() - rad},
    };
    constexpr double diagonals[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (int c = 0; c < 4; ++c) {
        for (int i = 0; i <= kCornerSegments; ++i) {
            if (grow <= 0) {
                out.emplace_back(centres[c].x() + diagonals[c][0] * grow, centres[c].y() + diagonals[c][1] * grow);
                continue;
            }
            const double cs = t.cos[static_cast<std::size_t>(i)];
            const double sn = t.sin[static_cast<std::size_t>(i)];
            double dx = 0, dy = 0;
            switch (c) {
            case 0: dx = -cs; dy = -sn; break; // top-left: from left to top
            case 1: dx = sn; dy = -cs; break;  // top-right: from top to right
            case 2: dx = cs; dy = sn; break;   // bottom-right: from right to bottom
            case 3: dx = -sn; dy = cs; break;  // bottom-left: from bottom to left
            }
            out.emplace_back(centres[c].x() + dx * grow, centres[c].y() + dy * grow);
        }
    }
}

void MeshBuilder::ringIndices(std::uint32_t inner, std::uint32_t outer, std::uint32_t count) {
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t j = (i + 1) % count;
        m_indices.insert(m_indices.end(), {inner + i, outer + i, outer + j, inner + i, outer + j, inner + j});
    }
}

void MeshBuilder::addPolyline(std::span<const QPointF> points, qreal width, QColor color) {
    const qreal half = width / 2;
    for (std::size_t i = 1; i < points.size(); ++i) {
        const QPointF a = points[i - 1];
        const QPointF b = points[i];
        const QPointF d = b - a;
        const qreal length = std::hypot(d.x(), d.y());
        if (length <= 1e-6)
            continue;
        const QPointF n(-d.y() / length * half, d.x() / length * half);
        addTriangle(a + n, b + n, b - n, color);
        addTriangle(a + n, b - n, a - n, color);
    }
}

void MeshBuilder::addRoundedRect(const QRectF& r, qreal radius, QColor fill, QColor border, qreal borderWidth,
                                 qreal aa) {
    if (r.width() <= 0 || r.height() <= 0)
        return;
    if (r.width() < 2 * aa + 1 || r.height() < 2 * aa + 1) {
        addRect(r, fill);
        return;
    }
    const bool hasBorder = border.alpha() > 0 && borderWidth > 0;
    const Rgba fillP = rgba(premultiplied(fill));
    const Rgba edgeP = rgba(premultiplied(hasBorder ? border : fill));
    const Rgba edgeOut = rgba(premultiplied(transparentOf(hasBorder ? border : fill)));
    constexpr auto n = static_cast<std::uint32_t>(kPerimeterPoints);
    m_vertices.reserve(m_vertices.size() + 1 + 4 * n);
    m_indices.reserve(m_indices.size() + 15 * n);

    const std::uint32_t centre = addVertex(r.center().x(), r.center().y(), fillP);
    // Fill polygon: up to the border (or to the middle of the AA fringe).
    perimeter(r, radius, hasBorder ? -borderWidth : -aa / 2, m_scratchA);
    const std::uint32_t fillRing = addRingVertices(m_scratchA, fillP);
    for (std::uint32_t i = 0; i < n; ++i)
        m_indices.insert(m_indices.end(), {centre, fillRing + i, fillRing + (i + 1) % n});

    std::uint32_t edgeInner = fillRing;
    if (hasBorder) {
        // Border band: a hard colour change at -borderWidth, solid up to the fringe.
        const std::uint32_t borderStart = addRingVertices(m_scratchA, edgeP);
        perimeter(r, radius, -aa / 2, m_scratchB);
        const std::uint32_t borderEnd = addRingVertices(m_scratchB, edgeP);
        ringIndices(borderStart, borderEnd, n);
        edgeInner = borderEnd;
    }
    // Anti-aliased fringe.
    perimeter(r, radius, aa / 2, m_scratchB);
    const std::uint32_t outer = addRingVertices(m_scratchB, edgeOut);
    ringIndices(edgeInner, outer, n);
}

void MeshBuilder::addRing(const QRectF& r, qreal radius, qreal inner, qreal outer, QColor innerColor,
                          QColor outerColor) {
    if (r.width() <= 0 || r.height() <= 0 || (innerColor.alpha() == 0 && outerColor.alpha() == 0))
        return;
    constexpr auto n = static_cast<std::uint32_t>(kPerimeterPoints);
    perimeter(r, radius, inner, m_scratchA);
    const std::uint32_t a = addRingVertices(m_scratchA, rgba(premultiplied(innerColor)));
    perimeter(r, radius, outer, m_scratchB);
    const std::uint32_t b = addRingVertices(m_scratchB, rgba(premultiplied(outerColor)));
    ringIndices(a, b, n);
}

void MeshBuilder::addOutline(const QRectF& r, qreal radius, qreal offset, qreal width, QColor color, qreal aa) {
    const QColor clear = transparentOf(color);
    addRing(r, radius, offset - aa / 2, offset + aa / 2, clear, color);
    addRing(r, radius, offset + aa / 2, offset + width - aa / 2, color, color);
    addRing(r, radius, offset + width - aa / 2, offset + width + aa / 2, color, clear);
}

void MeshBuilder::addEnvelope(qreal x0, qreal step, qreal centerY, qreal halfHeight, std::span<const float> peaks,
                              QColor color) {
    if (peaks.empty() || color.alpha() == 0)
        return;
    const Rgba c = rgba(premultiplied(color));
    const std::uint32_t first = static_cast<std::uint32_t>(m_vertices.size());
    m_vertices.reserve(m_vertices.size() + 2 * (peaks.size() + 1));
    m_indices.reserve(m_indices.size() + 6 * peaks.size());
    for (std::size_t i = 0; i <= peaks.size(); ++i) {
        const float peak = peaks[std::min(i, peaks.size() - 1)];
        const qreal x = x0 + static_cast<qreal>(i) * step;
        const qreal h = std::max<qreal>(0.5, peak * halfHeight);
        addVertex(x, centerY - h, c);
        addVertex(x, centerY + h, c);
    }
    for (std::uint32_t i = 0; i < peaks.size(); ++i) {
        const std::uint32_t a = first + 2 * i;
        m_indices.insert(m_indices.end(), {a, a + 1, a + 3, a, a + 3, a + 2});
    }
}

void MeshBuilder::addColumns(qreal x0, qreal step, qreal baseY, qreal height, std::span<const float> peaks,
                             QColor color) {
    if (peaks.empty() || color.alpha() == 0)
        return;
    const Rgba c = rgba(premultiplied(color));
    const std::uint32_t first = static_cast<std::uint32_t>(m_vertices.size());
    m_vertices.reserve(m_vertices.size() + 2 * (peaks.size() + 1));
    m_indices.reserve(m_indices.size() + 6 * peaks.size());
    for (std::size_t i = 0; i <= peaks.size(); ++i) {
        const float peak = peaks[std::min(i, peaks.size() - 1)];
        const qreal x = x0 + static_cast<qreal>(i) * step;
        addVertex(x, baseY - std::max<qreal>(0.5, peak * height), c);
        addVertex(x, baseY, c);
    }
    for (std::uint32_t i = 0; i < peaks.size(); ++i) {
        const std::uint32_t a = first + 2 * i;
        m_indices.insert(m_indices.end(), {a, a + 1, a + 3, a, a + 3, a + 2});
    }
}

void MeshBuilder::commit(QSGGeometryNode* node) const {
    QSGGeometry* g = node->geometry();
    const int vc = static_cast<int>(m_vertices.size());
    const int ic = static_cast<int>(m_indices.size());
    if (g->vertexCount() != vc || g->indexCount() != ic)
        g->allocate(vc, ic);
    if (vc > 0)
        std::memcpy(g->vertexData(), m_vertices.data(), m_vertices.size() * sizeof(Vertex));
    if (ic > 0)
        std::memcpy(g->indexData(), m_indices.data(), m_indices.size() * sizeof(std::uint32_t));
    node->markDirty(QSGNode::DirtyGeometry);
}

QSGGeometryNode* MeshBuilder::createNode() {
    auto* node = new QSGGeometryNode;
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0, 0, QSGGeometry::UnsignedIntType);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry);
    node->setMaterial(new QSGVertexColorMaterial);
    node->setFlag(QSGNode::OwnsMaterial);
    return node;
}

} // namespace wabcut::app
