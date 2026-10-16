#include <wabcut/render/GraphicRaster.h>

#include <wabcut/render/Mask.h>

#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QRawFont>
#include <QTextBoundaryFinder>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QTransform>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>
#include <optional>
#include <utility>

namespace wabcut::render {

namespace {

constexpr double kPi = std::numbers::pi;

QColor colorOf(const Rgba& c, double opacity = 1.0) {
    return QColor::fromRgbF(static_cast<float>(std::clamp(c[0], 0.0, 1.0)), static_cast<float>(std::clamp(c[1], 0.0, 1.0)),
                            static_cast<float>(std::clamp(c[2], 0.0, 1.0)),
                            static_cast<float>(std::clamp(c[3] * opacity, 0.0, 1.0)));
}

QTransform qtransformOf(const ::wabcut::Affine2D& m) { return QTransform(m.a, m.b, m.c, m.d, m.e, m.f); }

// ---- UTF-8 <-> UTF-16 ------------------------------------------------------------------------------------------------

/// UTF-16 index of every UTF-8 byte offset of `utf8` (size + 1 entries; a byte inside a sequence maps to its start).
std::vector<int> utf16Of(std::string_view utf8) {
    std::vector<int> map(utf8.size() + 1, 0);
    int u16 = 0;
    std::size_t i = 0;
    while (i < utf8.size()) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        const std::size_t len = lead < 0x80 ? 1 : lead >> 5 == 0x6 ? 2 : lead >> 4 == 0xE ? 3 : lead >> 3 == 0x1E ? 4 : 1;
        for (std::size_t k = 0; k < len && i + k < utf8.size(); ++k) {
            map[i + k] = u16;
        }
        u16 += len == 4 ? 2 : 1;
        i += len;
    }
    map[utf8.size()] = u16;
    return map;
}

// ---- Text ------------------------------------------------------------------------------------------------------------

/// The facts of a text block both measuring and drawing need (from a DrawText, or from the model's TextContent).
struct TextRunSpec {
    std::size_t byteStart = 0;
    std::string text;
    std::string family;
    std::string style;
    int weight = 400;
    double size = 72.0;
    bool italic = false;
    bool underline = false;
    double tracking = 0.0;
    double leading = 0.0;
    double baselineShift = 0.0;
    Rgba fill{1.0, 1.0, 1.0, 1.0};
};

struct TextSpec {
    std::vector<TextRunSpec> runs;
    TextAlign align = TextAlign::Left;
    double boxWidth = 0.0;
    double boxHeight = 0.0;
};

QFont fontOf(const TextRunSpec& run) {
    QFont font;
    if (!run.family.empty()) {
        font.setFamily(QString::fromStdString(run.family));
    } else {
        font.setFamily(QFontDatabase::systemFont(QFontDatabase::GeneralFont).family());
        font.setStyleHint(QFont::SansSerif);
    }
    if (!run.style.empty()) {
        font.setStyleName(QString::fromStdString(run.style));
    } else {
        font.setWeight(static_cast<QFont::Weight>(std::clamp(run.weight, 100, 900)));
        font.setItalic(run.italic);
    }
    font.setPixelSize(std::max(1, static_cast<int>(std::lround(run.size))));
    font.setUnderline(false); // drawn as a rectangle under the glyphs (it follows clusters and transforms)
    if (run.tracking != 0.0) {
        font.setLetterSpacing(QFont::AbsoluteSpacing, run.tracking / 1000.0 * run.size);
    }
    // Outlines without hinting: the same shapes at every size and on every machine with the same fonts (as titles).
    font.setHintingPreference(QFont::PreferNoHinting);
    font.setStyleStrategy(QFont::PreferAntialias);
    return font;
}

/// A laid-out block: the layout (lines positioned in the item's pixels, block top-left at `origin`), its size.
struct TextBlock {
    QString text;
    std::vector<int> utf16; ///< UTF-16 index of every byte offset of the joined text
    std::unique_ptr<QTextLayout> layout;
    QList<QTextLayout::FormatRange> formats;
    double width = 0.0;
    double height = 0.0;
    QPointF origin; ///< the block's top-left in the item's pixels (hangs from the origin by the alignment)
};

TextBlock layoutText(const TextSpec& spec) {
    TextBlock block;
    std::string joined;
    for (const TextRunSpec& r : spec.runs) {
        joined += r.text;
    }
    block.utf16 = utf16Of(joined);
    block.text = QString::fromUtf8(joined.data(), static_cast<qsizetype>(joined.size()));
    block.text.replace(QLatin1Char('\n'), QChar::LineSeparator); // a new line, same UTF-16 length
    const TextRunSpec base = spec.runs.empty() ? TextRunSpec{} : spec.runs.front();
    double maxSize = 0.0;
    double leading = 0.0;
    std::size_t at = 0;
    for (const TextRunSpec& r : spec.runs) {
        const int from = block.utf16[std::min(at, joined.size())];
        at += r.text.size();
        const int to = block.utf16[std::min(at, joined.size())];
        maxSize = std::max(maxSize, r.size);
        leading = std::max(leading, r.leading);
        if (to <= from) {
            continue;
        }
        QTextCharFormat format;
        format.setFont(fontOf(r));
        format.setFontUnderline(r.underline); // drawn by addGlyphs
        format.setForeground(colorOf(r.fill));
        if (r.baselineShift != 0.0 && r.size > 0.0) {
            format.setBaselineOffset(100.0 * r.baselineShift / r.size); // percent of the font height, up
        }
        block.formats.append(QTextLayout::FormatRange{from, to - from, format});
    }
    if (maxSize <= 0.0) {
        maxSize = base.size;
    }
    const double lineHeight = leading > 0.0 ? leading : 1.2 * maxSize;
    block.layout = std::make_unique<QTextLayout>(block.text, fontOf(base));
    block.layout->setFormats(block.formats);
    block.layout->setCacheEnabled(true);
    QTextOption option;
    const bool paragraph = spec.boxWidth > 0.0;
    option.setWrapMode(paragraph ? QTextOption::WordWrap : QTextOption::NoWrap);
    option.setAlignment(spec.align == TextAlign::Justify && paragraph ? Qt::AlignJustify : Qt::AlignLeft);
    block.layout->setTextOption(option);
    block.layout->beginLayout();
    std::vector<QTextLine> lines;
    for (;;) {
        QTextLine line = block.layout->createLine();
        if (!line.isValid()) {
            break;
        }
        line.setLineWidth(paragraph ? spec.boxWidth : 1e7);
        lines.push_back(line);
    }
    block.layout->endLayout();
    double natural = 0.0;
    for (const QTextLine& line : lines) {
        natural = std::max(natural, line.naturalTextWidth());
    }
    block.width = paragraph ? spec.boxWidth : natural;
    block.height = spec.boxHeight > 0.0 && paragraph ? spec.boxHeight : lineHeight * static_cast<double>(lines.size());
    // Lines: baseline i at i * lineHeight plus the font's ascent and descent centred in the line box; each line placed
    // inside the block by the alignment (justified lines fill it themselves).
    for (std::size_t i = 0; i < lines.size(); ++i) {
        QTextLine& line = lines[i];
        double x = 0.0;
        if (spec.align == TextAlign::Center) {
            x = (block.width - line.naturalTextWidth()) / 2.0;
        } else if (spec.align == TextAlign::Right) {
            x = block.width - line.naturalTextWidth();
        }
        const double top = static_cast<double>(i) * lineHeight + (lineHeight - line.ascent() - line.descent()) / 2.0;
        line.setPosition(QPointF(x, top));
    }
    block.origin = spec.align == TextAlign::Center  ? QPointF(-block.width / 2.0, 0.0)
                   : spec.align == TextAlign::Right ? QPointF(-block.width, 0.0)
                                                    : QPointF(0.0, 0.0);
    return block;
}

TextSpec specOf(const DrawText& text) {
    TextSpec spec;
    for (const DrawTextRun& r : text.runs) {
        spec.runs.push_back(TextRunSpec{r.byteStart, r.text, r.fontFamily, r.fontStyle, r.fontWeight, r.fontSize, r.italic,
                                        r.underline, r.tracking, r.leading, r.baselineShift, r.fill});
    }
    spec.align = text.align;
    spec.boxWidth = text.boxWidth;
    spec.boxHeight = text.boxHeight;
    return spec;
}

TextSpec specOf(const TextContent& text, const std::vector<std::string>& fonts) {
    TextSpec spec;
    std::size_t at = 0;
    for (std::size_t i = 0; i < text.runs.size(); ++i) {
        const TextRun& r = text.runs[i];
        const TextStyle& s = r.style;
        spec.runs.push_back(TextRunSpec{at, r.text, i < fonts.size() ? fonts[i] : s.fontFamily, s.fontStyle, s.fontWeight,
                                        s.fontSize, s.italic, s.underline, s.tracking, s.leading, s.baselineShift,
                                        Rgba{1.0, 1.0, 1.0, 1.0}});
        at += r.text.size();
    }
    spec.align = text.align;
    spec.boxWidth = text.boxWidth;
    spec.boxHeight = text.boxHeight;
    return spec;
}

class GraphicTextMeasurer final : public TextMeasurer {
public:
    std::array<double, 2> measure(const TextContent& text, const std::vector<std::string>& fonts) const override {
        const TextBlock block = layoutText(specOf(text, fonts));
        return {block.width, block.height};
    }
};

class QtLineBreaker final : public LineBreaker {
public:
    std::vector<std::size_t> breakOpportunities(std::string_view text, std::string_view) const override {
        const QString s = QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
        // UTF-16 index -> UTF-8 byte offset.
        std::vector<std::size_t> bytes(static_cast<std::size_t>(s.size()) + 1, 0);
        std::size_t at = 0;
        for (qsizetype i = 0; i < s.size(); ++i) {
            bytes[static_cast<std::size_t>(i)] = at;
            const char16_t c = s.at(i).unicode();
            if (QChar::isHighSurrogate(c) && i + 1 < s.size()) {
                bytes[static_cast<std::size_t>(i + 1)] = at;
                at += 4;
                ++i;
            } else {
                at += c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
            }
        }
        bytes[static_cast<std::size_t>(s.size())] = at;
        std::vector<std::size_t> out;
        QTextBoundaryFinder finder(QTextBoundaryFinder::Line, s);
        for (qsizetype p = finder.toNextBoundary(); p > 0 && p < s.size(); p = finder.toNextBoundary()) {
            out.push_back(bytes[static_cast<std::size_t>(p)]);
        }
        return out;
    }
};

/// The glyph outlines of [from, from + length) (UTF-16) of the block, per run colour, in the item's pixels; underlined
/// runs get their underline. Returns the pieces' union bounds.
struct GlyphPiece {
    QPainterPath path;
    QColor color;
};

QRectF addGlyphs(const TextBlock& block, int from, int length, std::vector<GlyphPiece>& pieces) {
    QRectF bounds;
    for (const QTextLayout::FormatRange& f : block.formats) {
        const int a = std::max(from, f.start);
        const int b = std::min(from + length, f.start + f.length);
        if (b <= a) {
            continue;
        }
        GlyphPiece piece;
        piece.color = f.format.foreground().color();
        const bool underline = f.format.fontUnderline();
        for (const QGlyphRun& run : block.layout->glyphRuns(a, b - a)) {
            const QRawFont raw = run.rawFont();
            const QList<quint32> indexes = run.glyphIndexes();
            const QList<QPointF> positions = run.positions();
            for (qsizetype i = 0; i < indexes.size(); ++i) {
                if (indexes[i] == 0) {
                    continue; // no font has it
                }
                QPainterPath glyph = raw.pathForGlyph(indexes[i]);
                glyph.translate(block.origin + positions[i]);
                piece.path.addPath(glyph);
            }
            if (underline && !positions.isEmpty()) {
                const QList<QPointF> advances = raw.advancesForGlyphIndexes(indexes);
                const double x0 = positions.front().x();
                const double x1 = positions.back().x() + (advances.isEmpty() ? 0.0 : advances.back().x());
                const double y = positions.front().y() + raw.underlinePosition();
                const double thickness = std::max(1.0, raw.lineThickness());
                piece.path.addRect(QRectF(block.origin + QPointF(x0, y), QSizeF(x1 - x0, thickness)));
            }
        }
        piece.path.setFillRule(Qt::WindingFill);
        if (!piece.path.isEmpty()) {
            bounds = bounds.united(piece.path.boundingRect());
            pieces.push_back(std::move(piece));
        }
    }
    return bounds;
}

// ---- Shapes ------------------------------------------------------------------------------------------------------------

/// A cubic Bezier vertex path (tangents relative to their vertex).
QPainterPath pathOf(const std::vector<MaskVertex>& v, bool closed) {
    QPainterPath path;
    if (v.empty()) {
        return path;
    }
    path.moveTo(v[0].x, v[0].y);
    const std::size_t segments = closed ? v.size() : v.size() - 1;
    for (std::size_t i = 0; i < segments; ++i) {
        const MaskVertex& p = v[i];
        const MaskVertex& q = v[(i + 1) % v.size()];
        path.cubicTo(p.x + p.outX, p.y + p.outY, q.x + q.inX, q.y + q.inY, q.x, q.y);
    }
    if (closed) {
        path.closeSubpath();
    }
    path.setFillRule(Qt::WindingFill);
    return path;
}

/// The path as a polyline fine enough for `scale` device pixels per item pixel (about a quarter pixel of chord error).
std::vector<QPointF> flatten(const std::vector<MaskVertex>& v, bool closed, double scale) {
    std::vector<QPointF> out;
    if (v.empty()) {
        return out;
    }
    out.emplace_back(v[0].x, v[0].y);
    const std::size_t segments = closed ? v.size() : v.size() - 1;
    for (std::size_t i = 0; i < segments; ++i) {
        const MaskVertex& p = v[i];
        const MaskVertex& q = v[(i + 1) % v.size()];
        const QPointF p0(p.x, p.y), c1(p.x + p.outX, p.y + p.outY), c2(q.x + q.inX, q.y + q.inY), p3(q.x, q.y);
        const double control = QLineF(p0, c1).length() + QLineF(c1, c2).length() + QLineF(c2, p3).length();
        const bool straight = p.outX == 0 && p.outY == 0 && q.inX == 0 && q.inY == 0;
        const int n = straight ? 1 : std::clamp(static_cast<int>(std::ceil(std::sqrt(control * scale) * 1.5)), 4, 512);
        for (int k = 1; k <= n; ++k) {
            const double t = static_cast<double>(k) / n;
            const double u = 1.0 - t;
            out.push_back(u * u * u * p0 + 3 * u * u * t * c1 + 3 * u * t * t * c2 + t * t * t * p3);
        }
    }
    return out;
}

/// After Effects' Trim Paths on a path: [start, end] of its length, shifted by offset / 360, wrapping on a closed path.
QPainterPath trimmed(const std::vector<MaskVertex>& v, bool closed, double start, double end, double offsetDegrees,
                     double scale) {
    const std::vector<QPointF> pts = flatten(v, closed, scale);
    if (pts.size() < 2) {
        return {};
    }
    std::vector<double> cumulative(pts.size(), 0.0);
    for (std::size_t i = 1; i < pts.size(); ++i) {
        cumulative[i] = cumulative[i - 1] + QLineF(pts[i - 1], pts[i]).length();
    }
    const double total = cumulative.back();
    if (total <= 0.0) {
        return {};
    }
    double a = std::clamp(std::min(start, end), 0.0, 1.0);
    double b = std::clamp(std::max(start, end), 0.0, 1.0);
    if (b - a <= 0.0) {
        return {};
    }
    const double shift = offsetDegrees / 360.0;
    a += shift;
    b += shift;
    const auto pointAt = [&](double fraction) {
        const double d = std::clamp(fraction, 0.0, 1.0) * total;
        const auto it = std::lower_bound(cumulative.begin(), cumulative.end(), d);
        const std::size_t i = std::clamp<std::size_t>(static_cast<std::size_t>(it - cumulative.begin()), 1, pts.size() - 1);
        const double seg = cumulative[i] - cumulative[i - 1];
        const double t = seg > 0.0 ? (d - cumulative[i - 1]) / seg : 0.0;
        return pts[i - 1] + t * (pts[i] - pts[i - 1]);
    };
    // One piece [from, to] in [0, 1] of the length.
    const auto piece = [&](QPainterPath& path, double from, double to, bool move) {
        const QPointF first = pointAt(from);
        if (move) {
            path.moveTo(first);
        } else {
            path.lineTo(first);
        }
        for (std::size_t i = 0; i < pts.size(); ++i) {
            const double f = cumulative[i] / total;
            if (f > from && f < to) {
                path.lineTo(pts[i]);
            }
        }
        path.lineTo(pointAt(to));
    };
    QPainterPath path;
    const double wrapA = a - std::floor(a);
    const double wrapB = wrapA + (b - a);
    if (wrapB <= 1.0) {
        piece(path, wrapA, wrapB, true);
    } else if (closed) {
        piece(path, wrapA, 1.0, true); // through the start point: one continuous line
        piece(path, 0.0, wrapB - 1.0, false);
    } else {
        piece(path, wrapA, 1.0, true);
        piece(path, 0.0, wrapB - 1.0, true);
    }
    path.setFillRule(Qt::WindingFill);
    return path;
}

QBrush brushOf(const DrawPaint& paint) {
    if (paint.kind == PaintKind::Linear || paint.kind == PaintKind::Radial) {
        QGradientStops stops;
        for (const auto& [offset, color] : paint.stops) {
            stops.append({std::clamp(offset, 0.0, 1.0), colorOf(color)});
        }
        if (stops.isEmpty()) {
            stops.append({0.0, colorOf(paint.color)});
        }
        if (paint.kind == PaintKind::Linear) {
            QLinearGradient g(QPointF(paint.start[0], paint.start[1]), QPointF(paint.end[0], paint.end[1]));
            g.setStops(stops);
            g.setSpread(QGradient::PadSpread);
            return QBrush(g);
        }
        const double radius = std::hypot(paint.end[0] - paint.start[0], paint.end[1] - paint.start[1]);
        QRadialGradient g(QPointF(paint.start[0], paint.start[1]), std::max(radius, 1e-6));
        g.setStops(stops);
        g.setSpread(QGradient::PadSpread);
        return QBrush(g);
    }
    return QBrush(colorOf(paint.color));
}

QPen penOf(const DrawStroke& stroke) {
    QPen pen(colorOf(stroke.color), std::max(stroke.width, 0.0));
    pen.setCosmetic(false);
    pen.setJoinStyle(stroke.join == LineJoin::Round ? Qt::RoundJoin : stroke.join == LineJoin::Bevel ? Qt::BevelJoin : Qt::MiterJoin);
    pen.setCapStyle(stroke.cap == LineCap::Round ? Qt::RoundCap : stroke.cap == LineCap::Square ? Qt::SquareCap : Qt::FlatCap);
    if (!stroke.dashes.empty() && stroke.width > 0.0) {
        QList<qreal> pattern;
        for (const double d : stroke.dashes) {
            pattern.append(std::max(d, 1e-3) / stroke.width); // Qt's dashes are in pen widths
        }
        if (pattern.size() % 2 == 1) {
            const QList<qreal> once = pattern;
            pattern.append(once); // dash, gap, dash... repeats (an odd list doubles, as SVG does)
        }
        pen.setDashPattern(pattern);
    }
    return pen;
}

QPainter::CompositionMode compositionOf(::wabcut::BlendMode mode) {
    switch (mode) {
    case ::wabcut::BlendMode::Multiply: return QPainter::CompositionMode_Multiply;
    case ::wabcut::BlendMode::Screen: return QPainter::CompositionMode_Screen;
    case ::wabcut::BlendMode::Overlay: return QPainter::CompositionMode_Overlay;
    case ::wabcut::BlendMode::Add: return QPainter::CompositionMode_Plus;
    case ::wabcut::BlendMode::Darken: return QPainter::CompositionMode_Darken;
    case ::wabcut::BlendMode::Lighten: return QPainter::CompositionMode_Lighten;
    case ::wabcut::BlendMode::Difference: return QPainter::CompositionMode_Difference;
    default: break; // Normal, and the modes QPainter has no operator for
    }
    return QPainter::CompositionMode_SourceOver;
}

// ---- Shadows and masks -------------------------------------------------------------------------------------------------

/// Gaussian blur (sigma in pixels) of one float channel, separable, edges clamped to zero.
void blur(std::vector<float>& a, int w, int h, double sigma) {
    if (sigma < 0.3 || w <= 0 || h <= 0) {
        return;
    }
    const int radius = std::max(1, static_cast<int>(std::ceil(3.0 * sigma)));
    std::vector<float> kernel(static_cast<std::size_t>(2 * radius + 1));
    double sum = 0.0;
    for (int i = -radius; i <= radius; ++i) {
        const double v = std::exp(-0.5 * i * i / (sigma * sigma));
        kernel[static_cast<std::size_t>(i + radius)] = static_cast<float>(v);
        sum += v;
    }
    for (float& k : kernel) {
        k = static_cast<float>(k / sum);
    }
    std::vector<float> tmp(a.size(), 0.0f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float s = 0.0f;
            for (int k = -radius; k <= radius; ++k) {
                const int xx = x + k;
                if (xx >= 0 && xx < w) {
                    s += kernel[static_cast<std::size_t>(k + radius)] * a[static_cast<std::size_t>(y) * w + xx];
                }
            }
            tmp[static_cast<std::size_t>(y) * w + x] = s;
        }
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float s = 0.0f;
            for (int k = -radius; k <= radius; ++k) {
                const int yy = y + k;
                if (yy >= 0 && yy < h) {
                    s += kernel[static_cast<std::size_t>(k + radius)] * tmp[static_cast<std::size_t>(yy) * w + x];
                }
            }
            a[static_cast<std::size_t>(y) * w + x] = s;
        }
    }
}

/// The matte of an item's masks over `rect` (device pixels): each owner's masks combined in order (the I.6 coverage,
/// render/Mask.h), owners multiplied. `toDevice` maps canvas pixels to device pixels (uniform scale `scale`).
std::vector<float> matteOf(const std::vector<DrawMask>& masks, const QRect& rect, const QTransform& toDevice, double scale) {
    const int w = rect.width();
    const int h = rect.height();
    std::vector<float> matte(static_cast<std::size_t>(w) * h, 1.0f);
    std::size_t i = 0;
    while (i < masks.size()) {
        MaskSpec spec;
        spec.aspect = static_cast<double>(w) / h;
        const std::uint32_t owner = masks[i].owner;
        for (; i < masks.size() && masks[i].owner == owner; ++i) {
            const DrawMask& m = masks[i];
            std::vector<OutlinePoint> outline;
            outline.reserve(m.outline.size());
            for (const MaskVertex& v : m.outline) {
                const QPointF p = toDevice.map(QPointF(v.x, v.y));
                outline.push_back(OutlinePoint{Vec2{(p.x() - rect.x()) / w, (p.y() - rect.y()) / h},
                                               Vec2{v.inX * scale / w, v.inY * scale / h},
                                               Vec2{v.outX * scale / w, v.outY * scale / h}});
            }
            MaskShapeSpec shape;
            shape.polygon = flattenOutline(outline);
            shape.feather = m.feather * scale / h;
            shape.expansion = m.expansion * scale / h;
            shape.opacity = m.opacity;
            shape.inverted = m.inverted;
            shape.mode = m.mode == MaskMode::Subtract ? 1 : m.mode == MaskMode::Intersect ? 2 : 0;
            spec.shapes.push_back(std::move(shape));
        }
        spec.seal();
        const std::vector<float> coverage = maskCoverageCpu(spec, w, h);
        for (std::size_t k = 0; k < matte.size() && k < coverage.size(); ++k) {
            matte[k] *= std::clamp(coverage[k], 0.0f, 1.0f);
        }
    }
    return matte;
}

// ---- Items -------------------------------------------------------------------------------------------------------------

struct Fit {
    QTransform toDevice; ///< canvas pixels -> device pixels
    double scale = 1.0;
};

/// Draws the item's content (full opacity, normal blending) with `painter` set up for the item's pixels.
void drawContent(QPainter& painter, const DrawItem& item, const TextBlock* text, const QImage* picture, double deviceScale,
                 GraphicRasterInfo& info) {
    switch (item.kind) {
    case DrawKind::Shape: {
        const bool trim = item.trimStart > 0.0 || item.trimEnd < 1.0 || std::fmod(item.trimOffset, 360.0) != 0.0;
        const bool whole = item.trimStart <= 0.0 && item.trimEnd >= 1.0;
        const QPainterPath path = trim && !whole ? trimmed(item.path, item.closed, item.trimStart, item.trimEnd, item.trimOffset, deviceScale)
                                                 : pathOf(item.path, item.closed);
        if (path.isEmpty()) {
            return;
        }
        if (item.fill.kind != PaintKind::None) {
            painter.fillPath(path, brushOf(item.fill));
        }
        if (item.stroke.enabled && item.stroke.width > 0.0) {
            painter.strokePath(path, penOf(item.stroke));
        }
        ++info.shapes;
        return;
    }
    case DrawKind::Text: {
        if (!text) {
            return;
        }
        ++info.texts;
        const DrawText& t = item.text;
        if (t.background && text->width > 0.0) {
            const double pad = std::max(t.backgroundPadding, 0.0);
            const QRectF box(text->origin.x() - pad, text->origin.y() - pad, text->width + 2 * pad, text->height + 2 * pad);
            QPainterPath bg;
            bg.addRoundedRect(box, std::max(t.backgroundRadius, 0.0), std::max(t.backgroundRadius, 0.0));
            painter.fillPath(bg, colorOf(t.backgroundColor));
        }
        painter.save();
        if (t.boxHeight > 0.0 && t.boxWidth > 0.0) {
            painter.setClipRect(QRectF(text->origin, QSizeF(text->width, text->height)), Qt::IntersectClip);
        }
        if (t.clusters.empty()) {
            std::vector<GlyphPiece> pieces;
            addGlyphs(*text, 0, static_cast<int>(text->text.size()), pieces);
            for (const GlyphPiece& p : pieces) {
                painter.fillPath(p.path, p.color);
            }
        } else {
            // Each cluster on its own, after layout: faded, then moved, scaled and rotated about its own centre.
            for (const DrawCluster& c : t.clusters) {
                const std::size_t a = std::min(c.byteStart, text->utf16.size() - 1);
                const std::size_t b = std::min(c.byteEnd, text->utf16.size() - 1);
                const int from = text->utf16[a];
                const int to = text->utf16[b];
                if (to <= from || c.opacity <= 0.0) {
                    continue;
                }
                std::vector<GlyphPiece> pieces;
                const QRectF bounds = addGlyphs(*text, from, to - from, pieces);
                if (pieces.empty()) {
                    continue;
                }
                const QPointF centre = bounds.center();
                QTransform local;
                local.translate(centre.x() + c.offsetX, centre.y() + c.offsetY);
                local.rotate(c.rotation);
                local.scale(c.scale, c.scale);
                local.translate(-centre.x(), -centre.y());
                painter.save();
                painter.setTransform(local * painter.transform());
                painter.setOpacity(painter.opacity() * std::clamp(c.opacity, 0.0, 1.0));
                for (const GlyphPiece& p : pieces) {
                    painter.fillPath(p.path, p.color);
                }
                painter.restore();
                ++info.animatedClusters;
            }
        }
        painter.restore();
        return;
    }
    case DrawKind::Image: {
        ++info.images;
        if (!picture || picture->isNull() || item.image.width <= 0.0 || item.image.height <= 0.0) {
            ++info.missingImages;
            return;
        }
        const QRectF box(-item.image.width / 2.0, -item.image.height / 2.0, item.image.width, item.image.height);
        const double iw = picture->width();
        const double ih = picture->height();
        QRectF target = box;
        painter.save();
        switch (item.image.fit) {
        case ImageFit::Contain:
        case ImageFit::Cover: {
            const double s = item.image.fit == ImageFit::Contain ? std::min(box.width() / iw, box.height() / ih)
                                                                 : std::max(box.width() / iw, box.height() / ih);
            target = QRectF(-iw * s / 2.0, -ih * s / 2.0, iw * s, ih * s);
            if (item.image.fit == ImageFit::Cover) {
                painter.setClipRect(box, Qt::IntersectClip);
            }
            break;
        }
        case ImageFit::None:
            target = QRectF(-iw / 2.0, -ih / 2.0, iw, ih);
            break;
        case ImageFit::Fill:
            break;
        }
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(target, *picture);
        painter.restore();
        return;
    }
    }
}

/// The item's device bounds (its content transformed), before shadow and masks.
QRectF deviceBounds(const DrawItem& item, const TextBlock* text, const QTransform& toItem) {
    QRectF local;
    switch (item.kind) {
    case DrawKind::Shape: {
        local = pathOf(item.path, item.closed).boundingRect();
        const double pad = item.stroke.enabled ? item.stroke.width * (item.stroke.join == LineJoin::Miter ? 2.0 : 0.75) + 1.0 : 0.0;
        local.adjust(-pad, -pad, pad, pad);
        break;
    }
    case DrawKind::Text:
        if (text) {
            std::vector<GlyphPiece> pieces;
            local = addGlyphs(*text, 0, static_cast<int>(text->text.size()), pieces);
            local = local.united(QRectF(text->origin, QSizeF(text->width, text->height)));
            const double pad = item.text.background ? std::max(item.text.backgroundPadding, 0.0) : 0.0;
            double move = 0.0;
            for (const DrawCluster& c : item.text.clusters) { // animated clusters may leave the block
                move = std::max({move, std::abs(c.offsetX), std::abs(c.offsetY)});
                if (c.scale > 1.0 || c.rotation != 0.0) {
                    move = std::max(move, std::max(local.width(), local.height()) * std::max(c.scale, 1.0));
                }
            }
            local.adjust(-pad - move, -pad - move, pad + move, pad + move);
        }
        break;
    case DrawKind::Image:
        local = QRectF(-item.image.width / 2.0, -item.image.height / 2.0, item.image.width, item.image.height);
        break;
    }
    return toItem.mapRect(local).adjusted(-2.0, -2.0, 2.0, 2.0);
}

void drawItem(QImage& canvas, const DrawItem& item, const Fit& fit, const GraphicImageSource* images, GraphicRasterInfo& info) {
    ++info.items;
    if (item.opacity <= 0.0) {
        return;
    }
    const QTransform toItem = qtransformOf(item.transform) * fit.toDevice; // item pixels -> device pixels
    // Device pixels per item pixel (for flattening and blur sizes): the transform's geometric mean scale.
    const double deviceScale = std::sqrt(std::max(std::abs(toItem.determinant()), 1e-12));
    std::optional<TextBlock> text;
    if (item.kind == DrawKind::Text) {
        text = layoutText(specOf(item.text));
    }
    QImage picture;
    if (item.kind == DrawKind::Image && images && item.image.media.isValid()) {
        picture = images->picture(item.image.media);
    }
    QRectF bounds = deviceBounds(item, text ? &*text : nullptr, toItem);
    double shadowSigma = 0.0;
    QPointF shadowOffset;
    if (item.shadow.enabled && item.shadow.opacity > 0.0) {
        shadowSigma = std::max(item.shadow.softness, 0.0) / 3.0 * fit.scale;
        const double angle = item.shadow.angle * kPi / 180.0;
        shadowOffset = QPointF(std::cos(angle), std::sin(angle)) * item.shadow.distance * fit.scale;
        const double reach = 3.0 * shadowSigma + 2.0;
        const QRectF shadowRect = bounds.translated(shadowOffset).adjusted(-reach, -reach, reach, reach);
        bounds = bounds.united(shadowRect);
    }
    const QRect rect = bounds.toAlignedRect().intersected(canvas.rect());
    if (rect.isEmpty()) {
        return;
    }
    // The item alone in a layer the size of its bounds: masks and opacity apply to it as a whole.
    QImage layer(rect.size(), QImage::Format_RGBA8888_Premultiplied);
    layer.fill(Qt::transparent);
    {
        QPainter p(&layer);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setTransform(toItem * QTransform::fromTranslate(-rect.x(), -rect.y()));
        drawContent(p, item, text ? &*text : nullptr, picture.isNull() ? nullptr : &picture, deviceScale, info);
    }
    const int w = rect.width();
    const int h = rect.height();
    if (!item.masks.empty()) {
        ++info.maskedItems;
        const std::vector<float> matte = matteOf(item.masks, rect, fit.toDevice, fit.scale);
        for (int y = 0; y < h; ++y) {
            std::uint8_t* row = layer.scanLine(y);
            for (int x = 0; x < w; ++x) {
                const float k = matte[static_cast<std::size_t>(y) * w + x];
                for (int c = 0; c < 4; ++c) {
                    row[x * 4 + c] = static_cast<std::uint8_t>(std::lround(row[x * 4 + c] * k));
                }
            }
        }
    }
    QPainter painter(&canvas);
    if (item.shadow.enabled && item.shadow.opacity > 0.0) {
        // The coverage, blurred, in the shadow colour, offset: just below the item.
        ++info.shadows;
        std::vector<float> alpha(static_cast<std::size_t>(w) * h);
        for (int y = 0; y < h; ++y) {
            const std::uint8_t* row = layer.constScanLine(y);
            for (int x = 0; x < w; ++x) {
                alpha[static_cast<std::size_t>(y) * w + x] = row[x * 4 + 3] / 255.0f;
            }
        }
        blur(alpha, w, h, shadowSigma);
        const QColor sc = colorOf(item.shadow.color, item.shadow.opacity);
        QImage shadow(rect.size(), QImage::Format_RGBA8888_Premultiplied);
        for (int y = 0; y < h; ++y) {
            std::uint8_t* row = shadow.scanLine(y);
            for (int x = 0; x < w; ++x) {
                const double a = std::clamp(static_cast<double>(alpha[static_cast<std::size_t>(y) * w + x]) * sc.alphaF(), 0.0, 1.0);
                row[x * 4 + 0] = static_cast<std::uint8_t>(std::lround(sc.redF() * a * 255.0));
                row[x * 4 + 1] = static_cast<std::uint8_t>(std::lround(sc.greenF() * a * 255.0));
                row[x * 4 + 2] = static_cast<std::uint8_t>(std::lround(sc.blueF() * a * 255.0));
                row[x * 4 + 3] = static_cast<std::uint8_t>(std::lround(a * 255.0));
            }
        }
        painter.setOpacity(std::clamp(item.opacity, 0.0, 1.0));
        painter.drawImage(QPointF(rect.topLeft()) + shadowOffset, shadow);
    }
    painter.setOpacity(std::clamp(item.opacity, 0.0, 1.0));
    painter.setCompositionMode(compositionOf(item.blend));
    painter.drawImage(rect.topLeft(), layer);
}

} // namespace

// ---- Public ----------------------------------------------------------------------------------------------------------

const TextMeasurer& graphicTextMeasurer() {
    static const GraphicTextMeasurer measurer;
    return measurer;
}

const LineBreaker& qtLineBreaker() {
    static const QtLineBreaker breaker;
    return breaker;
}

GraphicRaster::GraphicRaster(int width, int height, std::vector<std::uint8_t> rgba, GraphicRasterInfo info)
    : m_width(width), m_height(height), m_rgba(std::move(rgba)), m_info(info) {}

FrameView GraphicRaster::view(std::shared_ptr<const GraphicRaster> self) const {
    FrameView v;
    v.width = m_width;
    v.height = m_height;
    v.layout = PlaneLayout::PackedRGBA;
    v.encoding = SampleEncoding::Bits8;
    v.planeCount = 1;
    v.planes[0] = PlaneView{m_rgba.data(), m_width * 4, m_width, m_height};
    v.color.matrix = ColorMatrix::RGB;
    v.color.range = ColorRange::Full;
    v.color.primaries = ColorPrimaries::BT709;
    v.color.transfer = TransferFunction::SRGB;
    v.alpha = AlphaMode::Premultiplied;
    v.identity = this; // one raster = one picture: the importer keeps it resident across frames
    v.owner = self;
    v.keepAlive = std::move(self);
    return v;
}

std::shared_ptr<const GraphicRaster> rasterizeGraphic(const DrawList& list, int width, int height, const GraphicImageSource* images) {
    width = std::max(1, width);
    height = std::max(1, height);
    QImage canvas(width, height, QImage::Format_RGBA8888_Premultiplied);
    canvas.fill(Qt::transparent);
    // The design canvas fitted into the frame: contain, centred.
    Fit fit;
    const double dw = list.width > 0 ? list.width : width;
    const double dh = list.height > 0 ? list.height : height;
    fit.scale = std::min(width / dw, height / dh);
    fit.toDevice = QTransform::fromScale(fit.scale, fit.scale) *
                   QTransform::fromTranslate((width - dw * fit.scale) / 2.0, (height - dh * fit.scale) / 2.0);
    GraphicRasterInfo info;
    for (const DrawItem& item : list.items) {
        drawItem(canvas, item, fit, images, info);
    }
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (int y = 0; y < height; ++y) {
        std::memcpy(rgba.data() + static_cast<std::size_t>(y) * width * 4, canvas.constScanLine(y), static_cast<std::size_t>(width) * 4);
    }
    return std::make_shared<const GraphicRaster>(width, height, std::move(rgba), info);
}

GraphicRasterCache::GraphicRasterCache(std::size_t capacity) : m_capacity(std::max<std::size_t>(1, capacity)) {}

GraphicRasterCache& GraphicRasterCache::shared() {
    static GraphicRasterCache cache;
    return cache;
}

std::shared_ptr<const GraphicRaster> GraphicRasterCache::raster(std::uint64_t key, const DrawList& list, int width, int height,
                                                                const GraphicImageSource* images) {
    {
        std::lock_guard lock(m_mutex);
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            if (it->key == key && it->width == width && it->height == height && it->list == list) {
                m_entries.splice(m_entries.begin(), m_entries, it);
                return m_entries.front().raster;
            }
        }
    }
    auto raster = rasterizeGraphic(list, width, height, images);
    ++m_rasterizations;
    std::lock_guard lock(m_mutex);
    std::erase_if(m_entries, [&](const Entry& e) { return e.key == key; });
    m_entries.push_front(Entry{key, list, width, height, raster});
    while (m_entries.size() > m_capacity) {
        m_entries.pop_back();
    }
    return raster;
}

void GraphicRasterCache::clear() {
    std::lock_guard lock(m_mutex);
    m_entries.clear();
}

} // namespace wabcut::render
