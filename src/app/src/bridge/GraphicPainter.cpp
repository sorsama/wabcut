#include "GraphicPainter.h"

#include "GraphicText.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRadialGradient>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <optional>

namespace wabcut::app {

namespace {

QPainterPath pathOf(const std::vector<MaskVertex>& v, bool closed) {
    QPainterPath p;
    if (v.empty())
        return p;
    p.moveTo(v[0].x, v[0].y);
    const std::size_t n = v.size();
    const std::size_t segments = closed ? n : n - 1;
    for (std::size_t i = 0; i < segments; ++i) {
        const MaskVertex& a = v[i];
        const MaskVertex& b = v[(i + 1) % n];
        p.cubicTo(a.x + a.outX, a.y + a.outY, b.x + b.inX, b.y + b.inY, b.x, b.y);
    }
    if (closed)
        p.closeSubpath();
    return p;
}

/// Trim Paths: [start, end] of the outline (0..1), rotated by `offsetDegrees` / 360, as a polyline.
QPainterPath trimmed(const QPainterPath& path, double start, double end, double offsetDegrees) {
    if (start <= 0.0 && end >= 1.0 && offsetDegrees == 0.0)
        return path;
    const double length = path.length();
    if (length <= 0.0 || end <= start)
        return {};
    if (end - start >= 1.0)
        return path;
    QPainterPath out;
    const auto addRange = [&](double a, double b) {
        const int steps = std::max(2, static_cast<int>((b - a) * length / 2.0));
        out.moveTo(path.pointAtPercent(std::clamp(a, 0.0, 1.0)));
        for (int k = 1; k <= steps; ++k)
            out.lineTo(path.pointAtPercent(std::clamp(a + (b - a) * k / steps, 0.0, 1.0)));
    };
    double s = start + offsetDegrees / 360.0;
    s -= std::floor(s);
    const double e = s + (end - start);
    if (e <= 1.0) {
        addRange(s, e);
    } else {
        addRange(s, 1.0);
        addRange(0.0, e - 1.0);
    }
    return out;
}

QBrush brushOf(const DrawPaint& paint) {
    switch (paint.kind) {
    case PaintKind::None:
        return Qt::NoBrush;
    case PaintKind::Solid:
        return toQColor(paint.color);
    case PaintKind::Linear:
    case PaintKind::Radial: {
        QGradientStops stops;
        for (const auto& [offset, color] : paint.stops)
            stops.append({std::clamp(offset, 0.0, 1.0), toQColor(color)});
        if (paint.kind == PaintKind::Linear) {
            QLinearGradient g(paint.start[0], paint.start[1], paint.end[0], paint.end[1]);
            g.setStops(stops);
            return g;
        }
        const double radius = std::hypot(paint.end[0] - paint.start[0], paint.end[1] - paint.start[1]);
        QRadialGradient g(QPointF(paint.start[0], paint.start[1]), std::max(radius, 0.001));
        g.setStops(stops);
        return g;
    }
    }
    return Qt::NoBrush;
}

QPen penOf(const DrawStroke& stroke) {
    if (!stroke.enabled || stroke.width <= 0.0)
        return Qt::NoPen;
    QPen pen(toQColor(stroke.color), stroke.width);
    pen.setJoinStyle(stroke.join == LineJoin::Round ? Qt::RoundJoin : stroke.join == LineJoin::Bevel ? Qt::BevelJoin : Qt::MiterJoin);
    pen.setCapStyle(stroke.cap == LineCap::Round ? Qt::RoundCap : stroke.cap == LineCap::Square ? Qt::SquareCap : Qt::FlatCap);
    if (!stroke.dashes.empty()) {
        QList<qreal> pattern; // in units of the pen's width, an even count
        for (const double d : stroke.dashes)
            pattern.append(std::max(0.01, d / stroke.width));
        if (pattern.size() % 2 == 1)
            pattern += pattern;
        pen.setDashPattern(pattern);
    }
    return pen;
}

QPainter::CompositionMode compositionOf(BlendMode mode) {
    switch (mode) {
    case BlendMode::Normal:
        return QPainter::CompositionMode_SourceOver;
    case BlendMode::Multiply:
        return QPainter::CompositionMode_Multiply;
    case BlendMode::Screen:
        return QPainter::CompositionMode_Screen;
    case BlendMode::Overlay:
        return QPainter::CompositionMode_Overlay;
    case BlendMode::Add:
        return QPainter::CompositionMode_Plus;
    case BlendMode::Darken:
        return QPainter::CompositionMode_Darken;
    case BlendMode::Lighten:
        return QPainter::CompositionMode_Lighten;
    case BlendMode::Difference:
        return QPainter::CompositionMode_Difference;
    }
    return QPainter::CompositionMode_SourceOver;
}

/// The item's masks as one clip path in canvas pixels (owners multiply: their paths intersect), or empty for none.
std::optional<QPainterPath> matteOf(const std::vector<DrawMask>& masks, const QRectF& everything) {
    if (masks.empty())
        return std::nullopt;
    QPainterPath full;
    full.addRect(everything);
    std::vector<std::uint32_t> owners;
    std::map<std::uint32_t, QPainterPath> mattes;
    for (const DrawMask& mask : masks) {
        QPainterPath shape = pathOf(mask.outline, true);
        if (mask.inverted)
            shape = full.subtracted(shape);
        const auto it = mattes.find(mask.owner);
        if (it == mattes.end()) {
            owners.push_back(mask.owner);
            mattes[mask.owner] = mask.mode == MaskMode::Add ? shape
                                 : mask.mode == MaskMode::Subtract ? full.subtracted(shape)
                                                                   : full.intersected(shape);
            continue;
        }
        QPainterPath& m = it->second;
        m = mask.mode == MaskMode::Add ? m.united(shape) : mask.mode == MaskMode::Subtract ? m.subtracted(shape) : m.intersected(shape);
    }
    QPainterPath out = full;
    for (const std::uint32_t owner : owners)
        out = out.intersected(mattes[owner]);
    return out;
}

/// The item's own content (no opacity, blend, shadow or masks) in the painter's current transform (item pixels).
void drawContent(QPainter& painter, const DrawItem& item) {
    switch (item.kind) {
    case DrawKind::Shape: {
        const QPainterPath outline = pathOf(item.path, item.closed);
        const QPainterPath path = trimmed(outline, item.trimStart, item.trimEnd, item.trimOffset);
        painter.setPen(Qt::NoPen);
        painter.setBrush(brushOf(item.fill));
        if (item.fill.kind != PaintKind::None)
            painter.drawPath(path);
        const QPen pen = penOf(item.stroke);
        if (pen.style() != Qt::NoPen) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(pen);
            painter.drawPath(path);
        }
        break;
    }
    case DrawKind::Text: {
        const TextBlock block(blockRuns(item.text), item.text.align, item.text.boxWidth, item.text.boxHeight);
        const QPointF origin = block.blockOrigin();
        if (item.text.background) {
            const double pad = item.text.backgroundPadding;
            painter.setPen(Qt::NoPen);
            painter.setBrush(toQColor(item.text.backgroundColor));
            painter.drawRoundedRect(QRectF(origin, block.size()).adjusted(-pad, -pad, pad, pad), item.text.backgroundRadius,
                                    item.text.backgroundRadius);
        }
        block.draw(painter, origin, item.text.clusters);
        break;
    }
    case DrawKind::Image: {
        // A placeholder where the picture goes: the media is not decoded for previews.
        const QRectF box(-item.image.width / 2.0, -item.image.height / 2.0, item.image.width, item.image.height);
        painter.setPen(QPen(QColor(255, 255, 255, 90), std::max(1.0, box.width() / 200.0)));
        painter.setBrush(QColor(128, 128, 140, 110));
        painter.drawRect(box);
        painter.drawLine(box.topLeft(), box.bottomRight());
        painter.drawLine(box.topRight(), box.bottomLeft());
        break;
    }
    }
}

QTransform toQTransform(const Affine2D& m) { return QTransform(m.a, m.b, m.c, m.d, m.e, m.f); }

} // namespace

QImage paintGraphic(const DrawList& list, QSize size, const GraphicPaintOptions& options) {
    if (size.isEmpty())
        return {};
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(options.background);
    if (list.width <= 0 || list.height <= 0)
        return image;
    const double scale = std::min(static_cast<double>(size.width()) / list.width, static_cast<double>(size.height()) / list.height);
    const QTransform canvasToImage = QTransform::fromTranslate((size.width() - list.width * scale) / 2.0,
                                                              (size.height() - list.height * scale) / 2.0)
                                         .scale(scale, scale);
    const QRectF everything(-kMaxGraphicCanvas, -kMaxGraphicCanvas, 3.0 * kMaxGraphicCanvas, 3.0 * kMaxGraphicCanvas);

    QPainter painter(&image);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform | QPainter::TextAntialiasing);
    for (const DrawItem& item : list.items) {
        if (item.opacity <= 0.0)
            continue;
        const QTransform itemToImage = toQTransform(item.transform) * canvasToImage;
        painter.save();
        if (const auto matte = matteOf(item.masks, everything)) {
            painter.setTransform(canvasToImage);
            painter.setClipPath(*matte);
        }
        painter.setOpacity(std::clamp(item.opacity, 0.0, 1.0));
        painter.setCompositionMode(compositionOf(item.blend));
        if (item.shadow.enabled && item.shadow.opacity > 0.0) {
            // The item's coverage, offset along the shadow's angle in canvas pixels and tinted (no blur here).
            const double radians = item.shadow.angle * std::numbers::pi / 180.0;
            const QTransform shift = QTransform::fromTranslate(std::cos(radians) * item.shadow.distance,
                                                               std::sin(radians) * item.shadow.distance);
            QImage coverage(size, QImage::Format_ARGB32_Premultiplied);
            coverage.fill(Qt::transparent);
            {
                QPainter shadow(&coverage);
                shadow.setRenderHints(painter.renderHints());
                shadow.setTransform(toQTransform(item.transform) * shift * canvasToImage);
                drawContent(shadow, item);
                shadow.resetTransform();
                shadow.setCompositionMode(QPainter::CompositionMode_SourceIn);
                shadow.fillRect(coverage.rect(), toQColor(item.shadow.color));
            }
            painter.save();
            painter.resetTransform();
            painter.setOpacity(painter.opacity() * std::clamp(item.shadow.opacity, 0.0, 1.0));
            painter.drawImage(QPointF(0.0, 0.0), coverage);
            painter.restore();
        }
        painter.setTransform(itemToImage);
        drawContent(painter, item);
        painter.restore();
    }
    painter.end();
    return image;
}

} // namespace wabcut::app
