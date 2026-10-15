#include <wabcut/render/Title.h>

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
#include <QTextLayout>
#include <QTextOption>

#include <algorithm>
#include <cmath>
#include <set>

namespace wabcut::render {

namespace {

QFont fontOf(const TitleStyle& style) {
    QFont font;
    if (!style.fontFamily.empty()) {
        font.setFamily(QString::fromStdString(style.fontFamily));
    } else {
        font.setStyleHint(QFont::SansSerif);
    }
    if (!style.fontStyle.empty()) {
        font.setStyleName(QString::fromStdString(style.fontStyle));
    }
    font.setPixelSize(std::max(1, static_cast<int>(std::lround(style.sizePx))));
    if (style.tracking != 0.0) {
        font.setLetterSpacing(QFont::AbsoluteSpacing, style.tracking / 1000.0 * style.sizePx);
    }
    // Outlines without hinting: the same shapes at every size and on every machine with the same fonts.
    font.setHintingPreference(QFont::PreferNoHinting);
    font.setStyleStrategy(QFont::PreferAntialias);
    return font;
}

QColor colorOf(const std::array<double, 3>& rgb, double opacity = 1.0) {
    return QColor::fromRgbF(static_cast<float>(std::clamp(rgb[0], 0.0, 1.0)), static_cast<float>(std::clamp(rgb[1], 0.0, 1.0)),
                            static_cast<float>(std::clamp(rgb[2], 0.0, 1.0)), static_cast<float>(std::clamp(opacity, 0.0, 1.0)));
}

/// One laid-out paragraph (a line of the title).
struct LaidOutLine {
    std::unique_ptr<QTextLayout> layout;
    double width = 0.0;
    double height = 0.0;
};

std::vector<LaidOutLine> layOut(const TitleStyle& style, const QFont& font) {
    std::vector<LaidOutLine> lines;
    const QStringList paragraphs = QString::fromStdString(style.text).split(QLatin1Char('\n'));
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    for (const QString& paragraph : paragraphs) {
        LaidOutLine out;
        out.layout = std::make_unique<QTextLayout>(paragraph, font);
        out.layout->setTextOption(option);
        out.layout->setCacheEnabled(true);
        out.layout->beginLayout();
        QTextLine line = out.layout->createLine();
        if (line.isValid()) {
            line.setLineWidth(1e7);
            out.width = line.naturalTextWidth();
            out.height = line.height();
        }
        out.layout->endLayout();
        if (out.height <= 0.0) {
            out.height = QFontInfo(font).pixelSize() * 1.2;
        }
        lines.push_back(std::move(out));
    }
    return lines;
}

} // namespace

TitleRaster::TitleRaster(int width, int height, std::vector<std::uint8_t> rgba, TitleLayoutInfo info)
    : m_width(width), m_height(height), m_rgba(std::move(rgba)), m_info(std::move(info)) {}

FrameView TitleRaster::view(std::shared_ptr<const TitleRaster> self) const {
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

TitleRenderer::TitleRenderer(std::size_t cacheSize) : m_cacheSize(std::max<std::size_t>(1, cacheSize)) {}

TitleRenderer& TitleRenderer::shared() {
    static TitleRenderer renderer;
    return renderer;
}

std::shared_ptr<const TitleRaster> TitleRenderer::render(const TitleStyle& style) {
    {
        std::lock_guard lock(m_mutex);
        for (auto it = m_cache.begin(); it != m_cache.end(); ++it) {
            if (it->first == style) {
                m_cache.splice(m_cache.begin(), m_cache, it);
                return m_cache.front().second;
            }
        }
    }
    auto raster = renderUncached(style); // outside the lock: rendering may take milliseconds
    std::lock_guard lock(m_mutex);
    m_cache.emplace_front(style, raster);
    while (m_cache.size() > m_cacheSize) {
        m_cache.pop_back();
    }
    return raster;
}

std::shared_ptr<const TitleRaster> TitleRenderer::renderUncached(const TitleStyle& style) {
    const int width = std::max(1, style.frameWidth);
    const int height = std::max(1, style.frameHeight);
    const QFont font = fontOf(style);
    const std::vector<LaidOutLine> lines = layOut(style, font);

    double blockWidth = 0.0;
    double blockHeight = 0.0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        blockWidth = std::max(blockWidth, lines[i].width);
        blockHeight += i + 1 < lines.size() ? lines[i].height * style.lineSpacing : lines[i].height;
    }
    const double x0 = (width - blockWidth) / 2.0;
    double y = (height - blockHeight) / 2.0;

    TitleLayoutInfo info;
    info.lines = static_cast<int>(lines.size());
    QPainterPath outlines;
    std::set<std::string> fonts;
    for (const LaidOutLine& line : lines) {
        double x = x0;
        if (style.align == TitleAlign::Center) {
            x += (blockWidth - line.width) / 2.0;
        } else if (style.align == TitleAlign::Right) {
            x += blockWidth - line.width;
        }
        const QPointF origin(x, y);
        for (const QGlyphRun& run : line.layout->glyphRuns()) {
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
                glyph.translate(origin + positions[i]);
                outlines.addPath(glyph);
            }
        }
        y += line.height * style.lineSpacing;
    }
    outlines.setFillRule(Qt::WindingFill);
    info.fonts.assign(fonts.begin(), fonts.end());

    QImage image(width, height, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    if (!outlines.isEmpty()) {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        if (style.strokeWidth > 0.0) {
            // A pen of twice the width centred on the outline; the fill covers its inner half.
            QPen pen(colorOf(style.stroke, style.strokeOpacity), 2.0 * style.strokeWidth, Qt::SolidLine, Qt::RoundCap,
                     Qt::RoundJoin);
            painter.strokePath(outlines, pen);
        }
        painter.fillPath(outlines, colorOf(style.fill, style.fillOpacity));
        painter.end();
        QRectF bounds = outlines.boundingRect();
        if (style.strokeWidth > 0.0) {
            bounds.adjust(-style.strokeWidth, -style.strokeWidth, style.strokeWidth, style.strokeWidth);
        }
        info.bounds = {bounds.x(), bounds.y(), bounds.width(), bounds.height()};
    }
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (int row = 0; row < height; ++row) {
        std::copy_n(image.constScanLine(row), static_cast<std::size_t>(width) * 4,
                    rgba.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(width) * 4);
    }
    return std::make_shared<const TitleRaster>(width, height, std::move(rgba), std::move(info));
}

std::vector<int> TitleRenderer::clusterBoundaries(const TitleStyle& style) {
    const QFont font = fontOf(style);
    const QString text = QString::fromStdString(style.text);
    QTextLayout layout(text, font);
    layout.beginLayout();
    layout.createLine();
    layout.endLayout();
    std::vector<int> boundaries;
    for (int i = 0; i <= text.size(); ++i) {
        if (layout.isValidCursorPosition(i)) {
            boundaries.push_back(i);
        }
    }
    return boundaries;
}

} // namespace wabcut::render
