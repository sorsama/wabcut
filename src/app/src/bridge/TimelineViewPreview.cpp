// TimelineView's external preview (owner: AI platform engineer): a read-only overlay that shows an edit before it is
// applied, over the current sequence. Kept apart from the view's own rendering and input: it only reads the rows,
// scroll, zoom and style, and paints in a child item above the clips. Removed content is struck through
// (Theme.danger, or the style's "previewRemovedColor"), new content is a dashed ghost clip and new markers are
// dashed lines (the selection colour, or "previewAddedColor").

#include "TimelineView.h"

#include <QPainter>
#include <QPainterPath>
#include <QQuickPaintedItem>

#include <algorithm>
#include <cmath>

namespace wabcut::app {

namespace {

QColor withAlpha(QColor c, double alpha) {
    c.setAlphaF(static_cast<float>(std::clamp(alpha, 0.0, 1.0) * c.alphaF()));
    return c;
}

QColor styleColor(const QVariantMap& style, const char* key, const QColor& fallback) {
    const QVariant v = style.value(QLatin1String(key));
    const QColor c = v.typeId() == QMetaType::QColor ? v.value<QColor>() : QColor(v.toString());
    return c.isValid() ? c : fallback;
}

} // namespace

class ExternalPreviewItem final : public QQuickPaintedItem {
public:
    explicit ExternalPreviewItem(TimelineView* view) : QQuickPaintedItem(view), m_view(view) {
        setObjectName(QStringLiteral("timelineExternalPreview"));
        setZ(10); // above the lanes and clips
        setAcceptedMouseButtons(Qt::NoButton); // read-only: input goes to the view underneath
        setAntialiasing(true);
    }

    void paint(QPainter* p) override {
        const TimelineView& v = *m_view;
        const TimelineView::ExternalPreview* preview = v.m_externalPreview.get();
        if (!preview) {
            return;
        }
        const TimelineStyle& s = v.m_style;
        const QColor removed = styleColor(v.m_styleMap, "previewRemovedColor", QColor(0xF4, 0x3F, 0x5E));
        const QColor added = styleColor(v.m_styleMap, "previewAddedColor", s.selectionColor);
        const double w = width();
        const double radius = s.clipRadius;
        p->setRenderHint(QPainter::Antialiasing);

        const auto rectOf = [&](TrackId track, Ticks start, Ticks end) -> std::optional<QRectF> {
            const auto row = std::ranges::find_if(v.m_rows, [&](const TimelineView::Row& r) { return r.track->id == track; });
            if (row == v.m_rows.end()) {
                return std::nullopt;
            }
            const double x0 = v.xOfTicks(start);
            const double x1 = std::max(v.xOfTicks(end), x0 + 2.0); // a frame at low zoom stays visible
            if (x1 < 0 || x0 > w) {
                return std::nullopt;
            }
            return QRectF(x0, row->y - v.m_scrollY + 1.0, x1 - x0, row->height - 2.0);
        };

        for (const auto& r : preview->removed) {
            const auto rect = rectOf(r.track, r.start, r.end);
            if (!rect) {
                continue;
            }
            QPainterPath shape;
            shape.addRoundedRect(*rect, radius, radius);
            p->fillPath(shape, withAlpha(removed, 0.28));
            p->save();
            p->setClipPath(shape);
            p->setPen(QPen(withAlpha(removed, 0.55), 1.0));
            for (double x = rect->left() - rect->height(); x < rect->right(); x += 7.0) { // hatching
                p->drawLine(QPointF(x, rect->bottom()), QPointF(x + rect->height(), rect->top()));
            }
            p->restore();
            p->setPen(QPen(removed, 2.0));
            p->drawLine(QPointF(rect->left() + 2, rect->center().y()), QPointF(rect->right() - 2, rect->center().y()));
            p->setPen(QPen(removed, 1.0));
            p->setBrush(Qt::NoBrush);
            p->drawRoundedRect(rect->adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
        }
        for (const auto& r : preview->added) {
            const auto rect = rectOf(r.track, r.start, r.end);
            if (!rect) {
                continue;
            }
            p->setBrush(withAlpha(added, 0.22));
            QPen pen(added, 1.5, Qt::DashLine);
            p->setPen(pen);
            p->drawRoundedRect(rect->adjusted(0.75, 0.75, -0.75, -0.75), radius, radius);
        }
        // Volume curves as they would be (M5g: auto-duck): a dashed rubber band over each clip.
        for (const auto& curve : preview->volumes) {
            const auto rect = rectOf(curve.track, curve.start, curve.end);
            if (!rect || curve.points.size() < 2) {
                continue;
            }
            const QRectF area = TimelineView::volumeArea(*rect);
            QPainterPath line;
            for (std::size_t i = 0; i < curve.points.size(); ++i) {
                const QPointF point(v.xOfTicks(curve.points[i].first),
                                    area.top() + TimelineView::volumeFraction(curve.points[i].second) * area.height());
                if (i == 0) {
                    line.moveTo(point);
                } else {
                    line.lineTo(point);
                }
            }
            p->save();
            p->setClipRect(*rect);
            p->setBrush(Qt::NoBrush);
            p->setPen(QPen(withAlpha(added, 0.45), 4.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p->drawPath(line);
            p->setPen(QPen(added, 2.0, Qt::DashLine, Qt::RoundCap, Qt::RoundJoin));
            p->drawPath(line);
            p->setPen(Qt::NoPen);
            p->setBrush(added);
            for (std::size_t i = 1; i + 1 < curve.points.size(); ++i) {
                const QPointF point(v.xOfTicks(curve.points[i].first),
                                    area.top() + TimelineView::volumeFraction(curve.points[i].second) * area.height());
                p->drawEllipse(point, 3.0, 3.0);
            }
            p->restore();
        }
        const double bottom = std::min(height(), v.m_contentHeight - v.m_scrollY);
        for (const Ticks t : preview->markers) {
            const double x = std::round(v.xOfTicks(t));
            if (x < -2 || x > w + 2) {
                continue;
            }
            p->setPen(QPen(added, 2.0, Qt::DashLine));
            p->drawLine(QPointF(x, 0), QPointF(x, bottom));
            p->setPen(Qt::NoPen);
            p->setBrush(added);
            QPainterPath flag;
            flag.moveTo(x - 5, 0);
            flag.lineTo(x + 5, 0);
            flag.lineTo(x, 7);
            flag.closeSubpath();
            p->drawPath(flag);
        }
    }

private:
    TimelineView* m_view;
};

void TimelineView::setExternalPreview(ExternalPreview preview) {
    m_externalPreview = std::make_unique<ExternalPreview>(std::move(preview));
    if (!m_externalPreviewItem) {
        auto* item = new ExternalPreviewItem(this);
        item->setSize(size());
        const auto resize = [this, item] { item->setSize(size()); };
        const auto repaint = [item] { item->update(); };
        connect(this, &QQuickItem::widthChanged, item, resize);
        connect(this, &QQuickItem::heightChanged, item, resize);
        connect(this, &TimelineView::viewChanged, item, repaint);
        connect(this, &TimelineView::layoutChanged, item, repaint);
        connect(this, &TimelineView::styleChanged, item, repaint);
        m_externalPreviewItem = item;
    }
    m_externalPreviewItem->setVisible(true);
    m_externalPreviewItem->update();
}

void TimelineView::clearExternalPreview() {
    m_externalPreview.reset();
    if (m_externalPreviewItem) {
        m_externalPreviewItem->setVisible(false);
    }
}

} // namespace wabcut::app
