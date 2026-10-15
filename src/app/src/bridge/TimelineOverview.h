#pragma once

// TimelineOverview: the timeline minimap. One row per track with every clip as a thin coloured span, drawn as a
// single batched mesh (sub-pixel clips merged), so it costs the same for 10 or 10,000 clips. It follows the
// TimelineView it is attached to: same sequence, same content extent. The visible-range window on top of it is
// plain QML (TimelineMinimap.qml).

#include "TimelineView.h"

#include <QPointer>
#include <QQuickItem>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

namespace wabcut::app {

class TimelineOverview : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(wabcut::app::TimelineView* view READ view WRITE setView NOTIFY viewChanged FINAL)
    /// Theme.clipColors.
    Q_PROPERTY(QVariantList clipColors READ clipColors WRITE setClipColors NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(double clipOpacity READ clipOpacity WRITE setClipOpacity NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(double rowGap READ rowGap WRITE setRowGap NOTIFY appearanceChanged FINAL)

public:
    explicit TimelineOverview(QQuickItem* parent = nullptr);

    TimelineView* view() const { return m_view; }
    void setView(TimelineView* view);
    QVariantList clipColors() const { return m_colorList; }
    void setClipColors(const QVariantList& colors);
    double clipOpacity() const { return m_opacity; }
    void setClipOpacity(double opacity);
    double rowGap() const { return m_rowGap; }
    void setRowGap(double gap);

    /// Rectangles drawn in the last frame (for tests).
    int lastRectCount() const { return m_lastRects; }

signals:
    void viewChanged();
    void appearanceChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    void connectController();
    void markDirty();

    QPointer<TimelineView> m_view;
    QMetaObject::Connection m_controllerConnection;
    QVariantList m_colorList;
    std::vector<QColor> m_colors;
    double m_opacity = 0.8;
    double m_rowGap = 1;
    bool m_dirty = true;
    int m_lastRects = 0;
};

} // namespace wabcut::app
