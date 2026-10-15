#pragma once

// Batched vertex-coloured triangle meshes for the timeline scene graph (TimelineView, TimelineOverview).
//
// Every shape is appended to one indexed mesh, so a whole track (all its clip bodies, borders and
// waveforms) is one QSGGeometryNode and one draw call. Edges are anti-aliased without MSAA by a thin fringe
// whose outer vertices are fully transparent (the vertex-colour material blends premultiplied colours).

#include <QColor>
#include <QRectF>
#include <QSGGeometry>
#include <QSGGeometryNode>

#include <cstdint>
#include <span>
#include <vector>

namespace wabcut::app {

class MeshBuilder {
public:
    using Vertex = QSGGeometry::ColoredPoint2D;

    void clear() {
        m_vertices.clear();
        m_indices.clear();
    }
    bool isEmpty() const { return m_indices.empty(); }
    std::size_t vertexCount() const { return m_vertices.size(); }
    std::size_t indexCount() const { return m_indices.size(); }

    /// Axis-aligned rectangle, no anti-aliasing (tiny or pixel-aligned shapes).
    void addRect(const QRectF& r, QColor color);

    /// Filled triangle (no anti-aliasing: transition wedges inside an anti-aliased frame).
    void addTriangle(QPointF a, QPointF b, QPointF c, QColor color);

    /// A line through `points`, `width` px thick (a quad per segment; the volume rubber band).
    void addPolyline(std::span<const QPointF> points, qreal width, QColor color);

    /// Filled rounded rectangle with an anti-aliased edge (`aa` = fringe width in px). When `border` has
    /// a non-zero alpha, a `borderWidth` band just inside the edge is drawn in it.
    void addRoundedRect(const QRectF& r, qreal radius, QColor fill, QColor border = {}, qreal borderWidth = 0,
                        qreal aa = 0.75);

    /// Band around a rounded rectangle between `inner` and `outer` px outside its edge (negative = inside),
    /// colour fading from `innerColor` to `outerColor` (outlines, glows, drop shadows).
    void addRing(const QRectF& r, qreal radius, qreal inner, qreal outer, QColor innerColor, QColor outerColor);

    /// Stroke outline at `offset` px outside the edge, `width` thick, anti-aliased on both sides.
    void addOutline(const QRectF& r, qreal radius, qreal offset, qreal width, QColor color, qreal aa = 0.75);

    /// Mirrored envelope (waveform): `peaks` in [0, 1] drawn as columns from `x0`, each `step` px wide,
    /// symmetric around `centerY` with half-height `peak * halfHeight`.
    void addEnvelope(qreal x0, qreal step, qreal centerY, qreal halfHeight, std::span<const float> peaks,
                     QColor color);

    /// Rectified waveform (M5i): `peaks` in [0, 1] as columns rising from `baseY` to `peak * height` above it.
    void addColumns(qreal x0, qreal step, qreal baseY, qreal height, std::span<const float> peaks, QColor color);
    /// Copies the mesh into `node`'s geometry (reallocating only when it must grow) and marks it dirty.
    void commit(QSGGeometryNode* node) const;

    /// Geometry for a new mesh node (vertex colours, 32-bit indices, triangles).
    static QSGGeometryNode* createNode();

private:
    struct Rgba {
        uchar r = 0, g = 0, b = 0, a = 0;
    };
    static Rgba rgba(QColor premultiplied);
    std::uint32_t addVertex(qreal x, qreal y, Rgba c) {
        Vertex v;
        v.set(static_cast<float>(x), static_cast<float>(y), c.r, c.g, c.b, c.a);
        m_vertices.push_back(v);
        return static_cast<std::uint32_t>(m_vertices.size() - 1);
    }
    std::uint32_t addRingVertices(const std::vector<QPointF>& points, Rgba c);
    // Perimeter of a rounded rect grown by `offset`, `segments` points per corner, clockwise from top-left.
    void perimeter(const QRectF& r, qreal radius, qreal offset, std::vector<QPointF>& out) const;
    void ringIndices(std::uint32_t inner, std::uint32_t outer, std::uint32_t count);

    std::vector<Vertex> m_vertices;
    std::vector<std::uint32_t> m_indices;
    mutable std::vector<QPointF> m_scratchA;
    mutable std::vector<QPointF> m_scratchB;
};

/// Premultiplies a colour for QSGVertexColorMaterial.
QColor premultiplied(QColor c);

} // namespace wabcut::app
