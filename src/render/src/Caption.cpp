#include <wabcut/render/Caption.h>

#include <QColor>
#include <QFont>
#include <QFontInfo>
#include <QGlyphRun>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRawFont>
#include <QString>
#include <QStringList>
#include <QTextBoundaryFinder>
#include <QTextLayout>
#include <QTextOption>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace wabcut::render {

namespace {

QColor colorOf(const std::array<double, 3>& rgb, double opacity = 1.0) {
    return QColor::fromRgbF(static_cast<float>(std::clamp(rgb[0], 0.0, 1.0)), static_cast<float>(std::clamp(rgb[1], 0.0, 1.0)),
                            static_cast<float>(std::clamp(rgb[2], 0.0, 1.0)), static_cast<float>(std::clamp(opacity, 0.0, 1.0)));
}

QFont fontOf(const CaptionStyle& style, double sizePx) {
    QFont font;
    if (!style.fontFamily.empty()) {
        font.setFamily(QString::fromStdString(style.fontFamily));
    } else {
        font.setStyleHint(QFont::SansSerif);
    }
    font.setWeight(static_cast<QFont::Weight>(std::clamp(style.weight, 100, 900)));
    font.setPixelSize(std::max(1, static_cast<int>(std::lround(sizePx))));
    font.setHintingPreference(QFont::PreferNoHinting); // the same shapes everywhere (as titles)
    font.setStyleStrategy(QFont::PreferAntialias);
    return font;
}

/// One wrapped line of the caption: where its characters are in the whole text, its size.
struct CaptionLine {
    const QTextLayout* layout = nullptr;
    int paragraphStart = 0; ///< UTF-16 offset of the paragraph in the caption text
    int start = 0;          ///< within the paragraph
    int length = 0;
    double width = 0.0;
    double height = 0.0;
    double y = 0.0; ///< the line's top inside its paragraph layout
};

/// Appends the outlines of `layout`'s glyphs for paragraph characters [from, from + length) to `path`, moved by
/// `offset`.
void addGlyphs(const QTextLayout& layout, int from, int length, QPointF offset, QPainterPath& path, TitleLayoutInfo& info,
               std::set<std::string>& fonts) {
    if (length <= 0) {
        return;
    }
    for (const QGlyphRun& run : layout.glyphRuns(from, length)) {
        const QRawFont raw = run.rawFont();
        fonts.insert(raw.familyName().toStdString());
        const QList<quint32> indexes = run.glyphIndexes();
        const QList<QPointF> positions = run.positions();
        for (qsizetype i = 0; i < indexes.size(); ++i) {
            ++info.glyphs;
            if (indexes[i] == 0) {
                ++info.missingGlyphs;
                continue;
            }
            QPainterPath glyph = raw.pathForGlyph(indexes[i]);
            glyph.translate(offset + positions[i]);
            path.addPath(glyph);
        }
    }
}

} // namespace

std::vector<std::pair<int, int>> captionWordSpans(const std::string& text) {
    std::vector<std::pair<int, int>> spans;
    const QString s = QString::fromStdString(text);
    QTextBoundaryFinder finder(QTextBoundaryFinder::Word, s);
    int start = 0;
    while (true) {
        const qsizetype next = finder.toNextBoundary();
        if (next < 0) {
            break;
        }
        const int end = static_cast<int>(next);
        bool word = false;
        for (int i = start; i < end && !word; ++i) {
            word = s[i].isLetterOrNumber() || s[i].isMark();
        }
        if (word) {
            spans.emplace_back(start, end);
        }
        start = end;
    }
    return spans;
}

CaptionRenderer::CaptionRenderer(std::size_t cacheSize) : m_cacheSize(std::max<std::size_t>(1, cacheSize)) {}

CaptionRenderer& CaptionRenderer::shared() {
    static CaptionRenderer renderer;
    return renderer;
}

std::shared_ptr<const TitleRaster> CaptionRenderer::render(const CaptionFrame& frame) {
    {
        std::lock_guard lock(m_mutex);
        for (auto it = m_cache.begin(); it != m_cache.end(); ++it) {
            if (it->first == frame) {
                m_cache.splice(m_cache.begin(), m_cache, it);
                return m_cache.front().second;
            }
        }
    }
    auto raster = renderUncached(frame); // outside the lock: rendering takes a few milliseconds
    std::lock_guard lock(m_mutex);
    m_cache.emplace_front(frame, raster);
    while (m_cache.size() > m_cacheSize) {
        m_cache.pop_back();
    }
    return raster;
}

namespace {

/// `v` clamped into [lo, hi]; the middle when the range is empty (a block larger than the safe area).
double clampInto(double v, double lo, double hi) {
    return hi < lo ? (lo + hi) / 2.0 : std::clamp(v, lo, hi);
}

/// An alpha-only Gaussian blur (sigma in pixels), in place.
void blurAlpha(std::vector<float>& a, int w, int h, double sigma) {
    const int radius = std::max(1, static_cast<int>(std::ceil(3.0 * sigma)));
    std::vector<float> kernel(static_cast<std::size_t>(2 * radius + 1));
    double sum = 0.0;
    for (int k = -radius; k <= radius; ++k) {
        const double v = std::exp(-0.5 * k * k / (sigma * sigma));
        kernel[static_cast<std::size_t>(k + radius)] = static_cast<float>(v);
        sum += v;
    }
    for (float& v : kernel) {
        v = static_cast<float>(v / sum);
    }
    std::vector<float> tmp(a.size());
    const auto at = [](int i, int n) { return std::clamp(i, 0, n - 1); };
    for (int y = 0; y < h; ++y) {
        const float* row = a.data() + static_cast<std::size_t>(y) * w;
        float* out = tmp.data() + static_cast<std::size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            float s = 0.0f;
            for (int k = -radius; k <= radius; ++k) {
                s += kernel[static_cast<std::size_t>(k + radius)] * row[at(x + k, w)];
            }
            out[x] = s;
        }
    }
    for (int y = 0; y < h; ++y) {
        float* out = a.data() + static_cast<std::size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            float s = 0.0f;
            for (int k = -radius; k <= radius; ++k) {
                s += kernel[static_cast<std::size_t>(k + radius)] * tmp[static_cast<std::size_t>(at(y + k, h)) * w + x];
            }
            out[x] = s;
        }
    }
}

/// The text's drop shadow: `paths` (with the outline when `strokePx` > 0) in `color`, blurred by `blurRadius` pixels,
/// as an image placed at the returned top-left (before the offset).
QImage shadowImage(const std::vector<const QPainterPath*>& paths, double strokePx, double blurRadius,
                   const std::array<double, 3>& color, QPointF& topLeft) {
    QRectF bounds;
    for (const QPainterPath* p : paths) {
        bounds = bounds.united(p->boundingRect());
    }
    const double margin = std::ceil(strokePx + 2.0 * blurRadius + 2.0);
    const QRect area = bounds.adjusted(-margin, -margin, margin, margin).toAlignedRect();
    if (area.isEmpty()) {
        return {};
    }
    QImage mask(area.size(), QImage::Format_ARGB32_Premultiplied);
    mask.fill(Qt::transparent);
    {
        QPainter p(&mask);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.translate(-area.topLeft());
        for (const QPainterPath* path : paths) {
            if (strokePx > 0.0) {
                p.strokePath(*path, QPen(Qt::black, 2.0 * strokePx, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            }
            p.fillPath(*path, Qt::black);
        }
    }
    const int w = area.width();
    const int h = area.height();
    std::vector<float> alpha(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(mask.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            alpha[static_cast<std::size_t>(y) * w + x] = static_cast<float>(qAlpha(row[x])) / 255.0f;
        }
    }
    const double sigma = blurRadius / 2.0; // a blur radius of 2 sigma, as CSS and the Essential Graphics panel
    if (sigma >= 0.3) {
        blurAlpha(alpha, w, h, sigma);
    }
    QImage out(area.size(), QImage::Format_RGBA8888_Premultiplied);
    const double r = std::clamp(color[0], 0.0, 1.0);
    const double g = std::clamp(color[1], 0.0, 1.0);
    const double b = std::clamp(color[2], 0.0, 1.0);
    for (int y = 0; y < h; ++y) {
        uchar* row = out.scanLine(y);
        for (int x = 0; x < w; ++x) {
            const double a = std::clamp(static_cast<double>(alpha[static_cast<std::size_t>(y) * w + x]), 0.0, 1.0);
            row[4 * x + 0] = static_cast<uchar>(std::lround(255.0 * r * a));
            row[4 * x + 1] = static_cast<uchar>(std::lround(255.0 * g * a));
            row[4 * x + 2] = static_cast<uchar>(std::lround(255.0 * b * a));
            row[4 * x + 3] = static_cast<uchar>(std::lround(255.0 * a));
        }
    }
    topLeft = area.topLeft();
    return out;
}

} // namespace

std::shared_ptr<const TitleRaster> CaptionRenderer::renderUncached(const CaptionFrame& frame) {
    const CaptionStyle& style = frame.style;
    const int width = std::max(1, frame.frameWidth);
    const int height = std::max(1, frame.frameHeight);
    const double scale = height / 1080.0;
    const double sizePx = std::max(1.0, style.sizePx * scale);
    const double strokePx = std::max(0.0, style.strokeWidth * scale);
    const QFont font = fontOf(style, sizePx);
    // The box's padding (0.35 em by default, 4 px at least when there is any), text wraps inside the box.
    const double padding = style.boxPadding > 0.0 ? std::max(4.0 * scale, sizePx * style.boxPadding) : 0.0;
    const double maxBoxWidth = std::clamp(style.maxWidth, 0.1, 1.0) * width;
    const double wrapWidth = std::max(sizePx, maxBoxWidth - 2.0 * padding);

    // ---- Layout: paragraphs ('\n'), each wrapped at word boundaries (anywhere for an over-long word).
    const QString text = QString::fromStdString(frame.text);
    const QStringList paragraphs = text.split(QLatin1Char('\n'));
    std::vector<std::unique_ptr<QTextLayout>> layouts;
    std::vector<CaptionLine> lines;
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    int paragraphStart = 0;
    for (const QString& paragraph : paragraphs) {
        auto layout = std::make_unique<QTextLayout>(paragraph, font);
        layout->setTextOption(option);
        layout->setCacheEnabled(true);
        layout->beginLayout();
        double y = 0.0;
        while (true) {
            QTextLine line = layout->createLine();
            if (!line.isValid()) {
                break;
            }
            line.setLineWidth(wrapWidth);
            line.setPosition(QPointF(0.0, y));
            CaptionLine out;
            out.layout = layout.get();
            out.paragraphStart = paragraphStart;
            out.start = line.textStart();
            out.length = line.textLength();
            out.width = line.naturalTextWidth();
            out.height = line.height();
            out.y = y;
            lines.push_back(out);
            y += line.height();
        }
        layout->endLayout();
        layouts.push_back(std::move(layout));
        paragraphStart += static_cast<int>(paragraph.size()) + 1;
    }

    double blockWidth = 0.0;
    double blockHeight = 0.0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        blockWidth = std::max(blockWidth, lines[i].width);
        blockHeight += i + 1 < lines.size() ? lines[i].height * style.lineSpacing : lines[i].height;
    }
    if (lines.empty()) {
        blockHeight = QFontInfo(font).pixelSize() * 1.2;
    }
    const double boxWidth = std::min(maxBoxWidth, blockWidth + 2.0 * padding);
    const double boxHeight = blockHeight + padding;
    const double safe = std::clamp(style.safeArea, 0.0, 0.45);
    double boxX = (width - boxWidth) / 2.0;
    double boxY = style.position == CaptionPosition::Top ? height * safe : height * (1.0 - safe) - boxHeight;
    if (style.position == CaptionPosition::Custom) {
        // The block centred at the custom position, kept inside the safe area.
        boxX = clampInto(std::clamp(style.centerX, 0.0, 1.0) * width - boxWidth / 2.0, width * safe,
                         width * (1.0 - safe) - boxWidth);
        boxY = clampInto(std::clamp(style.centerY, 0.0, 1.0) * height - boxHeight / 2.0, height * safe,
                         height * (1.0 - safe) - boxHeight);
    }

    // ---- What each character is drawn as: hidden (word by word), a speaker's colour, spoken (karaoke), or the fill.
    constexpr int kFill = -1;
    constexpr int kSpoken = -2;
    constexpr int kHidden = -3;
    std::vector<int> kind(static_cast<std::size_t>(text.size()), kFill);
    const auto mark = [&](int begin, int end, int value) {
        for (int i = std::max(0, begin); i < std::min(end, static_cast<int>(kind.size())); ++i) {
            kind[static_cast<std::size_t>(i)] = value;
        }
    };
    if (frame.highlightEnd >= 0) {
        mark(0, frame.highlightEnd, kSpoken);
    }
    for (const auto& [begin, end] : frame.highlights) {
        mark(begin, end, kSpoken);
    }
    for (std::size_t s = 0; s < frame.colorSpans.size(); ++s) {
        mark(frame.colorSpans[s].begin, frame.colorSpans[s].end, static_cast<int>(s));
    }
    for (const auto& [begin, end] : frame.hidden) {
        mark(begin, end, kHidden);
    }

    // ---- Outlines: the fill, spoken (karaoke), and each colour span.
    TitleLayoutInfo info;
    info.lines = static_cast<int>(lines.size());
    std::set<std::string> fonts;
    QPainterPath normal;
    QPainterPath spoken;
    std::vector<QPainterPath> spans(frame.colorSpans.size());
    double y = boxY + padding / 2.0;
    for (const CaptionLine& line : lines) {
        const double x = style.align == CaptionAlign::Left    ? boxX + padding
                         : style.align == CaptionAlign::Right ? boxX + boxWidth - padding - line.width
                                                              : boxX + (boxWidth - line.width) / 2.0;
        const QPointF offset(x, y - line.y);
        const int lineBegin = line.paragraphStart + line.start; // in the whole text
        const int lineEnd = lineBegin + line.length;
        for (int run = lineBegin; run < lineEnd;) {
            const int k = run < static_cast<int>(kind.size()) ? kind[static_cast<std::size_t>(run)] : kFill;
            int next = run + 1;
            while (next < lineEnd && (next < static_cast<int>(kind.size()) ? kind[static_cast<std::size_t>(next)] : kFill) == k) {
                ++next;
            }
            if (k != kHidden) {
                QPainterPath& path = k == kFill ? normal : k == kSpoken ? spoken : spans[static_cast<std::size_t>(k)];
                addGlyphs(*line.layout, line.start + (run - lineBegin), next - run, offset, path, info, fonts);
            }
            run = next;
        }
        y += line.height * style.lineSpacing;
    }
    normal.setFillRule(Qt::WindingFill);
    spoken.setFillRule(Qt::WindingFill);
    for (QPainterPath& p : spans) {
        p.setFillRule(Qt::WindingFill);
    }
    info.fonts.assign(fonts.begin(), fonts.end());
    std::vector<const QPainterPath*> drawn;
    for (const QPainterPath* p : {&normal, &spoken}) {
        if (!p->isEmpty()) {
            drawn.push_back(p);
        }
    }
    for (const QPainterPath& p : spans) {
        if (!p.isEmpty()) {
            drawn.push_back(&p);
        }
    }

    QImage image(width, height, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    const double opacity = std::clamp(frame.opacity, 0.0, 1.0);
    if (!lines.empty() && !drawn.empty() && opacity > 0.0 && frame.scale > 0.0) {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.setOpacity(opacity);
        const QRectF box(boxX, boxY, boxWidth, boxHeight);
        if (frame.scale != 1.0) { // pop: around the block's centre
            painter.translate(box.center());
            painter.scale(frame.scale, frame.scale);
            painter.translate(-box.center());
        }
        if (style.box) {
            const double radius = std::min(padding, sizePx * std::max(0.0, style.boxRadius));
            painter.setPen(Qt::NoPen);
            painter.setBrush(colorOf(style.boxColor, style.boxOpacity));
            painter.drawRoundedRect(box, radius, radius);
        }
        if (style.shadowOpacity > 0.0) {
            QPointF topLeft;
            const QImage shadow = shadowImage(drawn, strokePx, std::max(0.0, style.shadowSoftness) * sizePx,
                                              style.shadowColor, topLeft);
            if (!shadow.isNull()) {
                const double angle = style.shadowAngle * 3.14159265358979323846 / 180.0;
                const double distance = std::max(0.0, style.shadowDistance) * sizePx;
                painter.setOpacity(opacity * std::clamp(style.shadowOpacity, 0.0, 1.0));
                painter.drawImage(topLeft + QPointF(distance * std::cos(angle), distance * std::sin(angle)), shadow);
                painter.setOpacity(opacity);
            }
        }
        if (strokePx > 0.0) {
            const QPen pen(colorOf(style.stroke), 2.0 * strokePx, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            for (const QPainterPath* p : drawn) {
                painter.strokePath(*p, pen);
            }
        }
        painter.fillPath(normal, colorOf(style.fill));
        painter.fillPath(spoken, colorOf(style.highlight));
        for (std::size_t s = 0; s < spans.size(); ++s) {
            painter.fillPath(spans[s], colorOf(frame.colorSpans[s].color));
        }
        painter.end();
        QRectF bounds = box;
        if (!style.box) {
            bounds = QRectF();
            for (const QPainterPath* p : drawn) {
                bounds = bounds.united(p->boundingRect());
            }
            if (strokePx > 0.0) {
                bounds.adjust(-strokePx, -strokePx, strokePx, strokePx);
            }
        }
        info.bounds = {bounds.x(), bounds.y(), bounds.width(), bounds.height()};
    }
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (int row = 0; row < height; ++row) {
        std::memcpy(rgba.data() + static_cast<std::size_t>(row) * width * 4, image.constScanLine(row),
                    static_cast<std::size_t>(width) * 4);
    }
    return std::make_shared<const TitleRaster>(width, height, std::move(rgba), std::move(info));
}

} // namespace wabcut::render
