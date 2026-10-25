#include "EditCursors.h"

#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QPainter>
#include <QPixmap>
#include <QQuickWindow>
#include <QSvgRenderer>

#include <cmath>
#include <memory>
#include <numbers>
#include <vector>

namespace wabcut::app {

namespace {

// The canvas every cursor is laid out on (logical px): Windows' standard cursor size.
constexpr double kCanvas = 32;
// The white outline around the glyphs and the shadow under it (logical px).
constexpr double kOutline = 1.6;
constexpr double kShadowOffset = 1.0;
constexpr double kShadowOpacity = 0.35;
const QColor kInk(0x14, 0x14, 0x18);

enum class Tint { Ink, Trim, Ripple };

// One glyph drawn on the canvas: its 24-unit box starts at `origin` and is scaled by `k` (canvas px per unit); `clip`
// (glyph units) keeps part of it (half a bracket).
struct Layer {
    const char* glyph = "";
    QPointF origin;
    double k = 1;
    QRectF clip;
    Tint tint = Tint::Ink;
};

struct Recipe {
    QPoint hot;
    std::vector<Layer> layers;
};

// A whole glyph, its 24-unit box `size` px wide, centred on (cx, cy).
Layer whole(const char* glyph, double cx, double cy, double size, Tint tint = Tint::Ink) {
    const double k = size / 24.0;
    return {glyph, QPointF(cx - 12 * k, cy - 12 * k), k, {}, tint};
}

// Half of Boxicons' bracket (units: "[" x 4..10 with its bar at 4..6, "]" x 14..20 with its bar at 18..20, both y
// 2..22), `height` px tall, its bar centred on barX.
Layer bracketOpeningRight(double barX, double cy, double height, Tint tint) {
    const double k = height / 20.0;
    return {"bxf-bracket", QPointF(barX - 5 * k, cy - 12 * k), k, QRectF(0, 0, 12, 24), tint};
}
Layer bracketOpeningLeft(double barX, double cy, double height, Tint tint) {
    const double k = height / 20.0;
    return {"bxf-bracket", QPointF(barX - 19 * k, cy - 12 * k), k, QRectF(12, 0, 12, 24), tint};
}

// Boxicons' move-horizontal arrow (units x 2..22, y 7..17), `width` px wide, centred on (cx, cy).
Layer arrowAcross(double cx, double cy, double width, Tint tint) {
    const double k = width / 20.0;
    return {"bxf-move-horizontal", QPointF(cx - 12 * k, cy - 12 * k), k, {}, tint};
}

// A trim-like cursor: the bracket on the edge (the hot spot is on its bar) and a badge outside the clip.
Recipe edgeCursor(bool head, Tint tint, const char* badge, double badgeSize) {
    Recipe r;
    if (head) { // the clip starts at the bar: "[" with the badge to its left
        r.hot = QPoint(19, 16);
        r.layers.push_back(bracketOpeningRight(19, 16, 22, tint));
        r.layers.push_back(badge ? whole(badge, 9, 16, badgeSize, tint) : arrowAcross(9.4, 16, 14, tint));
    } else { // the clip ends at the bar: "]" with the badge to its right
        r.hot = QPoint(13, 16);
        r.layers.push_back(bracketOpeningLeft(13, 16, 22, tint));
        r.layers.push_back(badge ? whole(badge, 23, 16, badgeSize, tint) : arrowAcross(22.6, 16, 14, tint));
    }
    return r;
}

const QHash<QString, Recipe>& recipes() {
    static const QHash<QString, Recipe> table = [] {
        QHash<QString, Recipe> t;
        // Moving clips (the hold of a Selection drag) and Ctrl's insert move.
        t.insert(QStringLiteral("move"), {QPoint(16, 16), {whole("bxf-move", 16, 16, 24)}});
        t.insert(QStringLiteral("insert"), {QPoint(13, 13), {whole("bxf-move", 13, 13, 22), whole("bxf-plus-big", 25, 25, 13.2)}});
        // 0.4, Ctrl+Alt's rearrange: the swap arrows (the clips trade places). Alt's copy is the system copy cursor
        // (cursor() below; not a recipe).
        t.insert(QStringLiteral("rearrange"), {QPoint(16, 16), {whole("bxf-swap-horizontal", 16, 16, 24)}});
        // The hand tool, open over the lanes and holding while it drags.
        t.insert(QStringLiteral("hand"), {QPoint(16, 16), {whole("bx-hand", 16, 16, 24)}});
        t.insert(QStringLiteral("grab"), {QPoint(16, 16), {whole("bxf-hand", 16, 16, 24)}});
        // The zoom tool: the hot spot is the lens' centre (units 10.5, 10.5).
        t.insert(QStringLiteral("zoomIn"), {QPoint(14, 14), {whole("bxf-search-big-plus", 16, 16, 24)}});
        t.insert(QStringLiteral("zoomOut"), {QPoint(14, 14), {whole("bxf-search-big-minus", 16, 16, 24)}});
        // A keyframe under the pointer (its fingertip, units 10, 2) and a rubber band's level (up and down).
        t.insert(QStringLiteral("point"), {QPoint(14, 6), {whole("bxf-finger-up", 16, 16, 24)}});
        t.insert(QStringLiteral("vertical"), {QPoint(16, 16), {whole("bxf-move-vertical", 16, 16, 24)}});
        // The workbench's gutters and a track's height (the whole window's cursors are Boxicons, not just the lanes').
        t.insert(QStringLiteral("resizeH"), {QPoint(16, 16), {whole("bxf-move-horizontal", 16, 16, 24)}});
        t.insert(QStringLiteral("resizeV"), {QPoint(16, 16), {whole("bxf-move-vertical", 16, 16, 24)}});
        // The razor cuts where the blades cross (units 11.3, 12.7).
        t.insert(QStringLiteral("razor"), {QPoint(15, 17), {whole("bxf-cut", 16, 16, 24)}});
        // The pen's nib (units 3.4, 3.4); on the volume line it adds a key (the plus badge).
        t.insert(QStringLiteral("pen"), {QPoint(7, 7), {whole("bxf-cursor-pen", 16, 16, 24)}});
        t.insert(QStringLiteral("penAdd"), {QPoint(5, 5), {whole("bxf-cursor-pen", 13, 13, 22), whole("bxf-plus-big", 25, 25, 13.2)}});
        // Track select: everything from the bar on, forward or backward.
        t.insert(QStringLiteral("trackForward"), {QPoint(9, 16), {whole("bxf-arrow-from-left-stroke", 16, 16, 24)}});
        t.insert(QStringLiteral("trackBackward"), {QPoint(23, 16), {whole("bxf-arrow-from-right-stroke", 16, 16, 24)}});
        // Edges: regular trims red, ripple trims yellow (Premiere; the trim edit points use the same colours).
        t.insert(QStringLiteral("trimHead"), edgeCursor(true, Tint::Trim, nullptr, 0));
        t.insert(QStringLiteral("trimTail"), edgeCursor(false, Tint::Trim, nullptr, 0));
        t.insert(QStringLiteral("rippleHead"), edgeCursor(true, Tint::Ripple, nullptr, 0));
        t.insert(QStringLiteral("rippleTail"), edgeCursor(false, Tint::Ripple, nullptr, 0));
        // Rate stretch: the edge with a stopwatch; the Remix tool: a music clip's end with a note.
        t.insert(QStringLiteral("rateHead"), edgeCursor(true, Tint::Ink, "bxf-stopwatch", 18.7));
        t.insert(QStringLiteral("rateTail"), edgeCursor(false, Tint::Ink, "bxf-stopwatch", 18.7));
        t.insert(QStringLiteral("remix"), edgeCursor(false, Tint::Ink, "bxf-music", 15.6));
        // Roll: the cut between two clips ("][") moves both ways.
        t.insert(QStringLiteral("roll"),
                 {QPoint(16, 12),
                  {bracketOpeningLeft(13.5, 12, 18, Tint::Trim), bracketOpeningRight(18.5, 12, 18, Tint::Trim),
                   arrowAcross(16, 26.5, 16, Tint::Trim)}});
        // Slip: the source moves inside the clip's bounds; slide: the clip moves between its neighbours.
        t.insert(QStringLiteral("slip"), {QPoint(16, 16), {whole("bxf-bracket", 16, 16, 26.4), whole("bxf-swap-horizontal", 16, 16, 12)}});
        t.insert(QStringLiteral("slide"),
                 {QPoint(16, 16),
                  {bracketOpeningLeft(5, 16, 18, Tint::Ink), bracketOpeningRight(27, 16, 18, Tint::Ink),
                   arrowAcross(16, 16, 15, Tint::Ink)}});
        return t;
    }();
    return table;
}

QSvgRenderer* glyphRenderer(const char* glyph) {
    static QHash<QString, std::shared_ptr<QSvgRenderer>> renderers;
    const QString name = QString::fromLatin1(glyph);
    auto it = renderers.find(name);
    if (it == renderers.end()) {
        auto renderer = std::make_shared<QSvgRenderer>(QStringLiteral(":/qt/qml/Wabcut/resources/cursors/%1.svg").arg(name));
        it = renderers.insert(name, std::move(renderer));
    }
    return it.value()->isValid() ? it.value().get() : nullptr;
}

QColor tintColor(Tint tint, const EditCursorColors& colors) {
    switch (tint) {
    case Tint::Trim:
        return colors.trim;
    case Tint::Ripple:
        return colors.ripple;
    case Tint::Ink:
        break;
    }
    return kInk;
}

// `source`'s alpha filled with `color`.
QImage recoloured(const QImage& source, const QColor& color) {
    QImage out = source;
    QPainter p(&out);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(out.rect(), color);
    return out;
}

QImage render(const Recipe& recipe, qreal dpr, const EditCursorColors& colors) {
    const int px = static_cast<int>(std::ceil(kCanvas * dpr));
    QImage ink(px, px, QImage::Format_ARGB32_Premultiplied);
    ink.fill(Qt::transparent);
    for (const Layer& layer : recipe.layers) {
        QSvgRenderer* svg = glyphRenderer(layer.glyph);
        if (!svg)
            continue;
        QImage glyph(px, px, QImage::Format_ARGB32_Premultiplied);
        glyph.fill(Qt::transparent);
        {
            QPainter p(&glyph);
            p.setRenderHint(QPainter::Antialiasing);
            p.scale(dpr, dpr);
            if (!layer.clip.isEmpty())
                p.setClipRect(QRectF(layer.origin + layer.clip.topLeft() * layer.k, layer.clip.size() * layer.k));
            svg->render(&p, QRectF(layer.origin, QSizeF(24 * layer.k, 24 * layer.k)));
        }
        QPainter(&ink).drawImage(0, 0, recoloured(glyph, tintColor(layer.tint, colors)));
    }
    // The outline: the glyphs' coverage spread by kOutline in every direction (two rings of offsets), in white.
    QImage spread(px, px, QImage::Format_ARGB32_Premultiplied);
    spread.fill(Qt::transparent);
    {
        QPainter p(&spread);
        for (const double fraction : {0.5, 1.0}) {
            const double r = kOutline * dpr * fraction;
            for (int i = 0; i < 16; ++i) {
                const double a = i * std::numbers::pi / 8;
                p.drawImage(QPointF(r * std::cos(a), r * std::sin(a)), ink);
            }
        }
    }
    QImage out(px, px, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    {
        QPainter p(&out);
        p.setOpacity(kShadowOpacity);
        p.drawImage(QPointF(0, kShadowOffset * dpr), recoloured(spread, Qt::black));
        p.setOpacity(1);
        p.drawImage(0, 0, recoloured(spread, Qt::white));
        p.drawImage(0, 0, ink);
    }
    out.setDevicePixelRatio(dpr);
    return out;
}

QString cacheKey(const QString& key, qreal dpr, const EditCursorColors& colors) {
    return QStringLiteral("%1|%2|%3|%4").arg(key).arg(dpr).arg(colors.trim.rgba()).arg(colors.ripple.rgba());
}

} // namespace

namespace editcursors {

QStringList keys() {
    return {QStringLiteral("move"),       QStringLiteral("insert"),       QStringLiteral("hand"),
            QStringLiteral("grab"),       QStringLiteral("zoomIn"),       QStringLiteral("zoomOut"),
            QStringLiteral("point"),      QStringLiteral("vertical"),     QStringLiteral("razor"),
            QStringLiteral("pen"),        QStringLiteral("penAdd"),       QStringLiteral("trackForward"),
            QStringLiteral("trackBackward"), QStringLiteral("trimHead"),  QStringLiteral("trimTail"),
            QStringLiteral("rippleHead"), QStringLiteral("rippleTail"),   QStringLiteral("roll"),
            QStringLiteral("rateHead"),   QStringLiteral("rateTail"),     QStringLiteral("slip"),
            QStringLiteral("slide"),      QStringLiteral("remix"),        QStringLiteral("resizeH"),
            QStringLiteral("resizeV"),    QStringLiteral("rearrange")};
}

bool known(const QString& key) {
    return recipes().contains(key);
}

QImage image(const QString& key, qreal dpr, const EditCursorColors& colors) {
    const auto it = recipes().find(key);
    if (it == recipes().end())
        return {};
    return render(it.value(), dpr > 0 ? dpr : 1.0, colors);
}

QPoint hotSpot(const QString& key) {
    const auto it = recipes().find(key);
    return it == recipes().end() ? QPoint() : it.value().hot;
}

QCursor cursor(const QString& key, qreal dpr, const EditCursorColors& colors) {
    // 0.4, Alt-drag copies a clip: the system's copy cursor, as Premiere shows it.
    if (key == QLatin1String("copy"))
        return QCursor(Qt::DragCopyCursor);
    if (!known(key))
        return QCursor(Qt::ArrowCursor);
    static QHash<QString, QCursor> cache;
    const QString k = cacheKey(key, dpr, colors);
    const auto it = cache.constFind(k);
    if (it != cache.constEnd())
        return it.value();
    const QPoint hot = hotSpot(key);
    QCursor c(QPixmap::fromImage(image(key, dpr, colors)), hot.x(), hot.y());
    cache.insert(k, c);
    return c;
}

} // namespace editcursors

// ---- CursorArea ---------------------------------------------------------------------------------------------------

CursorArea::CursorArea(QQuickItem* parent) : QQuickItem(parent) {
    apply();
}

void CursorArea::setShape(const QString& shape) {
    if (shape == m_shape)
        return;
    m_shape = shape;
    apply();
    emit shapeChanged();
}

void CursorArea::itemChange(ItemChange change, const ItemChangeData& data) {
    QQuickItem::itemChange(change, data);
    // A parent's enabled or visible change reaches here too (Qt Quick sends the effective change down the tree).
    if (change == ItemSceneChange || change == ItemDevicePixelRatioHasChanged || change == ItemEnabledHasChanged
        || change == ItemVisibleHasChanged || change == ItemParentHasChanged)
        apply();
}

void CursorArea::apply() {
    // Disabled (itself or through a parent), hidden or without a shape, it has no cursor of its own: Qt Quick's cursor
    // lookup does not skip a disabled item that has one, so a disabled overlay would cover the cursors of everything
    // under it (0.1.3: the timeline showed the zoom tool's magnifier whatever the tool).
    if (!isEnabled() || !isVisible() || !editcursors::known(m_shape)) {
        unsetCursor();
        return;
    }
    const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : qGuiApp->devicePixelRatio();
    setCursor(editcursors::cursor(m_shape, dpr));
}

} // namespace wabcut::app
