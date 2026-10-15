#include "TimelineOverview.h"

#include "TimelineGeometry.h"

#include <QSGGeometryNode>

#include <algorithm>

namespace wabcut::app {

TimelineOverview::TimelineOverview(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents);
    setClip(true);
}

void TimelineOverview::setView(TimelineView* view) {
    if (m_view == view)
        return;
    if (m_view)
        disconnect(m_view, nullptr, this, nullptr);
    disconnect(m_controllerConnection);
    m_view = view;
    if (m_view) {
        // Content extent changes with the sequence; rows with the layout. Scrolling does not matter here.
        connect(m_view, &TimelineView::layoutChanged, this, &TimelineOverview::markDirty);
        connect(m_view, &TimelineView::controllerChanged, this, &TimelineOverview::connectController);
        connectController();
    }
    markDirty();
    emit viewChanged();
}

void TimelineOverview::connectController() {
    disconnect(m_controllerConnection);
    if (m_view && m_view->controller())
        m_controllerConnection = connect(m_view->controller(), &ProjectController::changed, this,
                                         &TimelineOverview::markDirty);
    markDirty();
}

void TimelineOverview::markDirty() {
    m_dirty = true;
    update();
}

void TimelineOverview::setClipColors(const QVariantList& colors) {
    m_colorList = colors;
    m_colors.clear();
    for (const QVariant& v : colors) {
        const QColor c = v.metaType() == QMetaType::fromType<QColor>() ? v.value<QColor>() : QColor(v.toString());
        if (c.isValid())
            m_colors.push_back(c);
    }
    m_dirty = true;
    update();
    emit appearanceChanged();
}

void TimelineOverview::setClipOpacity(double opacity) {
    if (opacity == m_opacity)
        return;
    m_opacity = opacity;
    m_dirty = true;
    update();
    emit appearanceChanged();
}

void TimelineOverview::setRowGap(double gap) {
    if (gap == m_rowGap)
        return;
    m_rowGap = gap;
    m_dirty = true;
    update();
    emit appearanceChanged();
}

void TimelineOverview::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    m_dirty = true;
    update();
}

QSGNode* TimelineOverview::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    auto* node = static_cast<QSGGeometryNode*>(oldNode);
    if (!node) {
        node = MeshBuilder::createNode();
        m_dirty = true;
    }
    if (!m_dirty)
        return node;
    m_dirty = false;

    MeshBuilder mesh;
    int rects = 0;
    const ProjectController* controller = m_view ? m_view->controller() : nullptr;
    const SequencePtr seq = controller ? controller->currentSequence() : nullptr;
    const double extent = controller ? controller->contentDuration().toSeconds() : 0;
    if (seq && extent > 0 && width() > 0 && !m_view->rows().empty()) {
        const auto& rows = m_view->rows();
        const double rowHeight = (height() - m_rowGap * static_cast<double>(rows.size() - 1)) / static_cast<double>(rows.size());
        const double scale = width() / extent;
        for (std::size_t r = 0; r < rows.size(); ++r) {
            const double y = static_cast<double>(r) * (rowHeight + m_rowGap);
            double runX0 = 0, runX1 = -1;
            QColor runColor;
            const auto flush = [&] {
                if (runX1 > runX0) {
                    mesh.addRect(QRectF(runX0, y, std::max(1.0, runX1 - runX0), rowHeight), runColor);
                    ++rects;
                }
                runX1 = -1;
            };
            for (const Clip& c : rows[r].track->clips) {
                const double x0 = c.start().toSeconds() * scale;
                const double x1 = c.end().toSeconds() * scale - 1.0; // 1 px gap where it fits
                QColor color = m_colors.empty() ? QColor(Qt::gray)
                                                : m_colors[static_cast<std::size_t>(
                                                      ((c.colorIndex % static_cast<int>(m_colors.size())) +
                                                       static_cast<int>(m_colors.size())) %
                                                      static_cast<int>(m_colors.size()))];
                color.setAlphaF(static_cast<float>(m_opacity));
                // Merge clips that touch at this scale (one rectangle per pixel run).
                if (runX1 >= 0 && x0 <= runX1 + 1.0 && (x1 - x0 < 2.0 || color == runColor)) {
                    runX1 = std::max(runX1, x1);
                    continue;
                }
                flush();
                runX0 = x0;
                runX1 = std::max(x1, x0 + 1.0);
                runColor = color;
            }
            flush();
        }
    }
    mesh.commit(node);
    m_lastRects = rects;
    return node;
}

} // namespace wabcut::app
