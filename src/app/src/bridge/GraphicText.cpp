#include "GraphicText.h"

#include <QFont>
#include <QGlyphRun>
#include <QPainter>
#include <QTextLine>
#include <QTextOption>

#include <algorithm>
#include <cmath>

namespace wabcut::app {

namespace {

/// UTF-8 to UTF-16 with the UTF-16 index of every byte (continuation bytes map to their code point's start; one more
/// entry for the end). Invalid bytes become U+FFFD, one per byte.
QString decodeWithOffsets(const std::string& utf8, std::vector<int>& utf16OfByte) {
    QString out;
    out.reserve(static_cast<qsizetype>(utf8.size()));
    utf16OfByte.assign(utf8.size() + 1, 0);
    std::size_t i = 0;
    while (i < utf8.size()) {
        const auto b = static_cast<unsigned char>(utf8[i]);
        std::size_t len = 1;
        char32_t cp = 0xFFFD;
        if (b < 0x80) {
            cp = b;
        } else if ((b & 0xE0) == 0xC0) {
            len = 2;
        } else if ((b & 0xF0) == 0xE0) {
            len = 3;
        } else if ((b & 0xF8) == 0xF0) {
            len = 4;
        }
        if (len > 1) {
            bool ok = i + len <= utf8.size();
            char32_t value = len == 2 ? (b & 0x1F) : len == 3 ? (b & 0x0F) : (b & 0x07);
            for (std::size_t k = 1; ok && k < len; ++k) {
                const auto c = static_cast<unsigned char>(utf8[i + k]);
                if ((c & 0xC0) != 0x80)
                    ok = false;
                else
                    value = (value << 6) | (c & 0x3F);
            }
            if (ok && value <= 0x10FFFF && !(value >= 0xD800 && value <= 0xDFFF)) {
                cp = value;
            } else {
                len = 1;
                cp = 0xFFFD;
            }
        }
        const int at = static_cast<int>(out.size());
        for (std::size_t k = 0; k < len; ++k)
            utf16OfByte[i + k] = at;
        if (cp == U'\n') {
            out.append(QChar(QChar::LineSeparator)); // a line break QTextLayout honours
        } else {
            const char32_t one[] = {cp};
            out.append(QString::fromUcs4(one, 1));
        }
        i += len;
    }
    utf16OfByte[utf8.size()] = static_cast<int>(out.size());
    return out;
}

QFont fontOf(const TextBlockRun& run) {
    QFont font;
    if (run.fontFamily.empty()) {
        font.setStyleHint(QFont::SansSerif);
    } else {
        font.setFamily(QString::fromStdString(run.fontFamily));
    }
    if (!run.fontStyle.empty())
        font.setStyleName(QString::fromStdString(run.fontStyle));
    font.setPixelSize(std::max(1, static_cast<int>(std::lround(run.fontSize))));
    font.setWeight(static_cast<QFont::Weight>(std::clamp(run.fontWeight, 100, 900)));
    font.setItalic(run.italic);
    font.setUnderline(run.underline);
    if (run.tracking != 0.0)
        font.setLetterSpacing(QFont::AbsoluteSpacing, run.tracking / 1000.0 * run.fontSize);
    return font;
}

} // namespace

QColor toQColor(const Rgba& rgba) {
    QColor c;
    c.setRgbF(static_cast<float>(std::clamp(rgba[0], 0.0, 1.0)), static_cast<float>(std::clamp(rgba[1], 0.0, 1.0)),
              static_cast<float>(std::clamp(rgba[2], 0.0, 1.0)), static_cast<float>(std::clamp(rgba[3], 0.0, 1.0)));
    return c;
}

TextBlock::TextBlock(const std::vector<TextBlockRun>& runs, TextAlign align, double boxWidth, double boxHeight)
    : m_runs(runs), m_align(align), m_boxWidth(std::max(0.0, boxWidth)), m_boxHeight(std::max(0.0, boxHeight)) {
    layout();
}

void TextBlock::layout() {
    std::string joined;
    std::vector<std::pair<std::size_t, std::size_t>> byteRanges;
    for (const TextBlockRun& run : m_runs) {
        byteRanges.emplace_back(joined.size(), joined.size() + run.text.size());
        joined += run.text;
    }
    m_text = decodeWithOffsets(joined, m_utf16OfByte);

    QFont base = m_runs.empty() ? QFont() : fontOf(m_runs.front());
    m_layout = std::make_unique<QTextLayout>(m_text, base);
    QList<QTextLayout::FormatRange> formats;
    double largest = m_runs.empty() ? 72.0 : 0.0;
    double leading = 0.0;
    for (std::size_t i = 0; i < m_runs.size(); ++i) {
        const TextBlockRun& run = m_runs[i];
        largest = std::max(largest, run.fontSize);
        if (run.leading > 0.0)
            leading = std::max(leading, run.leading);
        QTextLayout::FormatRange range;
        range.start = utf16Index(byteRanges[i].first);
        range.length = utf16Index(byteRanges[i].second) - range.start;
        if (range.length <= 0)
            continue;
        range.format.setFont(fontOf(run));
        range.format.setForeground(run.color);
        formats.append(range);
    }
    m_layout->setFormats(formats);
    const bool paragraph = m_boxWidth > 0.0;
    QTextOption option;
    option.setWrapMode(paragraph ? QTextOption::WrapAtWordBoundaryOrAnywhere : QTextOption::NoWrap);
    option.setAlignment(m_align == TextAlign::Justify && paragraph ? Qt::AlignJustify : Qt::AlignLeft);
    option.setUseDesignMetrics(true);
    m_layout->setTextOption(option);

    const double lineHeight = leading > 0.0 ? leading : 1.2 * largest;
    std::vector<double> widths;
    m_layout->beginLayout();
    double y = 0.0;
    for (QTextLine line = m_layout->createLine(); line.isValid(); line = m_layout->createLine()) {
        line.setLineWidth(paragraph ? m_boxWidth : 1e6);
        // The line's own ascent sits inside the (possibly tighter or looser) line height, centred like CSS line-height.
        line.setPosition(QPointF(0.0, y + (lineHeight - line.height()) / 2.0));
        widths.push_back(line.naturalTextWidth());
        y += lineHeight;
    }
    m_layout->endLayout();

    double width = paragraph ? m_boxWidth : 0.0;
    if (!paragraph) {
        for (const double w : widths)
            width = std::max(width, w);
    }
    m_lineX.assign(widths.size(), 0.0);
    for (std::size_t i = 0; i < widths.size(); ++i) {
        const double slack = std::max(0.0, width - widths[i]);
        if (m_align == TextAlign::Center)
            m_lineX[i] = slack / 2.0;
        else if (m_align == TextAlign::Right)
            m_lineX[i] = slack;
        QTextLine line = m_layout->lineAt(static_cast<int>(i));
        line.setPosition(QPointF(m_lineX[i], line.position().y()));
    }
    const double height = m_boxHeight > 0.0 ? m_boxHeight : (widths.empty() ? 0.0 : y);
    m_size = QSizeF(width, height);
}

QPointF TextBlock::blockOrigin() const {
    switch (m_align) {
    case TextAlign::Center:
        return QPointF(-m_size.width() / 2.0, 0.0);
    case TextAlign::Right:
        return QPointF(-m_size.width(), 0.0);
    case TextAlign::Left:
    case TextAlign::Justify:
        break;
    }
    return QPointF(0.0, 0.0);
}

QStringList TextBlock::lineTexts() const {
    QStringList out;
    for (int i = 0; i < m_layout->lineCount(); ++i) {
        const QTextLine line = m_layout->lineAt(i);
        out << m_text.mid(line.textStart(), line.textLength());
    }
    return out;
}

int TextBlock::utf16Index(std::size_t byte) const {
    if (m_utf16OfByte.empty())
        return 0;
    return m_utf16OfByte[std::min(byte, m_utf16OfByte.size() - 1)];
}

void TextBlock::draw(QPainter& painter, QPointF topLeft, const std::vector<DrawCluster>& clusters) const {
    painter.save();
    if (m_boxHeight > 0.0)
        painter.setClipRect(QRectF(topLeft, m_size), Qt::IntersectClip);
    if (clusters.empty()) {
        m_layout->draw(&painter, topLeft);
        painter.restore();
        return;
    }
    // Animated text: each grapheme cluster on its own, with its opacity and its move, scale and rotation about its centre.
    const QList<QTextLayout::FormatRange> formats = m_layout->formats();
    const auto colorAt = [&](int index) {
        for (const QTextLayout::FormatRange& f : formats) {
            if (index >= f.start && index < f.start + f.length)
                return f.format.foreground().color();
        }
        return QColor(Qt::white);
    };
    for (const DrawCluster& cluster : clusters) {
        if (cluster.opacity <= 0.0)
            continue;
        const int from = utf16Index(cluster.byteStart);
        const int to = utf16Index(cluster.byteEnd);
        if (to <= from)
            continue;
        const QList<QGlyphRun> runs = m_layout->glyphRuns(from, to - from);
        QRectF box;
        for (const QGlyphRun& run : runs)
            box = box.united(run.boundingRect());
        painter.save();
        painter.setOpacity(painter.opacity() * std::clamp(cluster.opacity, 0.0, 1.0));
        const QPointF centre = topLeft + box.center();
        painter.translate(centre + QPointF(cluster.offsetX, cluster.offsetY));
        painter.rotate(cluster.rotation);
        painter.scale(cluster.scale, cluster.scale);
        painter.translate(-centre);
        painter.setPen(colorAt(from));
        for (const QGlyphRun& run : runs)
            painter.drawGlyphRun(topLeft, run);
        painter.restore();
    }
    painter.restore();
}

std::array<double, 2> QtTextMeasurer::measure(const TextContent& text, const std::vector<std::string>& fonts) const {
    std::vector<TextBlockRun> runs;
    runs.reserve(text.runs.size());
    std::size_t byte = 0;
    for (std::size_t i = 0; i < text.runs.size(); ++i) {
        const TextRun& r = text.runs[i];
        TextBlockRun run;
        run.byteStart = byte;
        run.text = r.text;
        run.fontFamily = i < fonts.size() ? fonts[i] : r.style.fontFamily;
        run.fontStyle = r.style.fontStyle;
        run.fontWeight = r.style.fontWeight;
        run.fontSize = r.style.fontSize;
        run.italic = r.style.italic;
        run.underline = r.style.underline;
        run.tracking = r.style.tracking;
        run.leading = r.style.leading;
        runs.push_back(std::move(run));
        byte += r.text.size();
    }
    const TextBlock block(runs, text.align, text.boxWidth, text.boxHeight);
    return {block.size().width(), block.size().height()};
}

std::vector<TextBlockRun> blockRuns(const DrawText& text) {
    std::vector<TextBlockRun> runs;
    runs.reserve(text.runs.size());
    for (const DrawTextRun& r : text.runs) {
        TextBlockRun run;
        run.byteStart = r.byteStart;
        run.text = r.text;
        run.fontFamily = r.fontFamily;
        run.fontStyle = r.fontStyle;
        run.fontWeight = r.fontWeight;
        run.fontSize = r.fontSize;
        run.color = toQColor(r.fill);
        run.italic = r.italic;
        run.underline = r.underline;
        run.tracking = r.tracking;
        run.leading = r.leading;
        runs.push_back(std::move(run));
    }
    return runs;
}

std::shared_ptr<const TextMeasurer> qtTextMeasurer() {
    static const std::shared_ptr<const TextMeasurer> measurer = std::make_shared<QtTextMeasurer>();
    return measurer;
}

} // namespace wabcut::app
