#include <wabcut/render/Caption.h>

#include <wabcut/render/TextTypography.h>

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
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
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
    applyFontFeatures(font, style.features); // 0.7 (card #138)
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
    QPointF offset; ///< where the paragraph layout's origin is on the frame, for this line (set once the block is placed)
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

/// Whether `c` belongs to a word (a letter, a digit or a mark that goes with one).
bool wordCharacter(QChar c) { return c.isLetterOrNumber() || c.isMark(); }

/// The marks "strip accents" removes after canonical decomposition: combining diacritics, Hebrew points and cantillation,
/// Arabic harakat. Thai marks (U+0E31..U+0E4E) are vowels and tones, and the Japanese voiced marks (U+3099, U+309A) change
/// the kana: neither is touched.
bool accentMark(char16_t u) {
    return (u >= 0x0300 && u <= 0x036F) || (u >= 0x0591 && u <= 0x05BD) || u == 0x05BF || u == 0x05C1 || u == 0x05C2 ||
           u == 0x05C4 || u == 0x05C5 || u == 0x05C7 || (u >= 0x064B && u <= 0x065F) || u == 0x0670;
}

QString withoutAccents(const QString& in) {
    // Letters that carry no combining mark but are "the same letter with a stroke or a different shape".
    struct Fold {
        char16_t from;
        const char16_t* to;
    };
    static constexpr Fold kFolds[] = {{u'ł', u"l"},  {u'Ł', u"L"},  {u'ø', u"o"},  {u'Ø', u"O"},  {u'đ', u"d"},  {u'Đ', u"D"},
                                      {u'ħ', u"h"},  {u'Ħ', u"H"},  {u'ı', u"i"},  {u'ð', u"d"},  {u'Ð', u"D"},  {u'þ', u"th"},
                                      {u'Þ', u"Th"}, {u'æ', u"ae"}, {u'Æ', u"AE"}, {u'œ', u"oe"}, {u'Œ', u"OE"}};
    const QString decomposed = in.normalized(QString::NormalizationForm_D);
    QString out;
    out.reserve(decomposed.size());
    for (const QChar c : decomposed) {
        const char16_t u = c.unicode();
        if (accentMark(u)) {
            continue;
        }
        bool folded = false;
        for (const Fold& f : kFolds) {
            if (f.from == u) {
                out += QString::fromUtf16(f.to);
                folded = true;
                break;
            }
        }
        if (!folded) {
            out += c;
        }
    }
    return out.normalized(QString::NormalizationForm_C);
}

/// Symbols that QChar::isPunct() counts as punctuation but that are not sentence punctuation: they stay.
bool keptPunctuation(QChar c) {
    switch (c.unicode()) {
    case u'%': case u'&': case u'#': case u'@': case u'*': case u'/': case u'\\': case u'\u2030': // per mille
        return true;
    default:
        return false;
    }
}

QString withoutPunctuation(const QString& in) {
    QString out;
    out.reserve(in.size());
    const auto at = [&](qsizetype i) { return i >= 0 && i < in.size() ? in[i] : QChar(u' '); };
    for (qsizetype i = 0; i < in.size(); ++i) {
        const QChar c = in[i];
        if (!c.isPunct() || keptPunctuation(c)) {
            out += c;
            continue;
        }
        const QChar before = at(i - 1);
        const QChar after = at(i + 1);
        const bool inside = wordCharacter(before) && wordCharacter(after);
        const char16_t u = c.unicode();
        // An apostrophe or hyphen between letters is part of the word (don't, well-known); the point or comma of a number
        // (3.5, 1,000) is part of the number.
        const bool apostrophe = u == u'\'' || u == 0x2019 || u == 0x02BC;
        const bool hyphen = u == u'-' || u == 0x2010 || u == 0x2011;
        const bool separator = (u == u'.' || u == u',') && before.isDigit() && after.isDigit();
        if (inside && (apostrophe || hyphen || separator)) {
            out += c;
            continue;
        }
        // A removed mark that was the only thing between two words must not glue them (yes,no; a dash with no spaces).
        if (inside && !out.isEmpty() && !out.endsWith(QLatin1Char(' '))) {
            out += QLatin1Char(' ');
        }
    }
    // One space between words and none at the ends of a line; line breaks stay.
    QStringList lines = out.split(QLatin1Char('\n'));
    for (QString& line : lines) {
        line = line.simplified();
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace

std::string captionDisplayText(const std::string& text, CaptionCase textCase, bool stripPunctuation, bool stripAccents,
                               const std::string& language) {
    if (textCase == CaptionCase::AsWritten && !stripPunctuation && !stripAccents) {
        return text;
    }
    QString s = QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
    if (stripAccents) {
        s = withoutAccents(s);
    }
    if (stripPunctuation) {
        s = withoutPunctuation(s);
    }
    if (textCase != CaptionCase::AsWritten) {
        const QString tag = QString::fromStdString(language).left(2).toLower();
        if (tag == QLatin1String("tr") || tag == QLatin1String("az")) {
            // Turkish and Azeri: I <-> dotless i, dotted I <-> i (QString and QLocale do not do this without ICU).
            if (textCase == CaptionCase::Lower) {
                s.replace(QChar(u'I'), QChar(0x0131));
                s.replace(QChar(0x0130), QChar(u'i'));
            } else {
                s.replace(QChar(u'i'), QChar(0x0130));
                s.replace(QChar(0x0131), QChar(u'I'));
            }
        }
        s = textCase == CaptionCase::Lower ? s.toLower() : s.toUpper();
    }
    return s.toUtf8().toStdString();
}

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

std::vector<char> captionEmphasisFlags(const std::string& text, const std::vector<std::pair<int, int>>& spans,
                                       bool autoRules, const std::vector<std::string>& keywords) {
    const QString s = QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
    std::vector<QString> lowered;
    lowered.reserve(keywords.size());
    for (const std::string& k : keywords) {
        lowered.push_back(QString::fromUtf8(k.data(), static_cast<qsizetype>(k.size())).toLower());
    }
    std::vector<char> flags(spans.size(), 0);
    for (std::size_t i = 0; i < spans.size(); ++i) {
        const QString word = s.mid(spans[i].first, spans[i].second - spans[i].first);
        const QString lower = word.toLower();
        bool emphasised = std::find(lowered.begin(), lowered.end(), lower) != lowered.end();
        if (!emphasised && autoRules) {
            int letters = 0;
            bool digit = false;
            for (const QChar c : word) {
                letters += c.isLetter() ? 1 : 0;
                digit = digit || c.isDigit();
            }
            const bool capitals = letters >= 2 && word == word.toUpper() && word != lower;
            const bool bang = spans[i].second < s.size() && s[spans[i].second] == QLatin1Char('!');
            emphasised = digit || capitals || bang;
        }
        flags[i] = emphasised ? 1 : 0;
    }
    return flags;
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

namespace {

/// A caption block laid out: its wrapped lines and the box around them, in pixels of the frame. The lines point into
/// `layouts`, so the block is moved as a whole, never copied.
struct CaptionBlock {
    QFont font;
    double scale = 1.0;
    double sizePx = 1.0;
    double strokePx = 0.0;
    double padding = 0.0;
    QString text;
    std::vector<std::unique_ptr<QTextLayout>> layouts;
    std::vector<CaptionLine> lines;
    double blockWidth = 0.0;
    double blockHeight = 0.0;
    double boxWidth = 0.0;
    double boxHeight = 0.0;
};

/// Lays `utf8` out in `style` on a width x height frame: paragraphs ('\n'), each wrapped at word boundaries (anywhere
/// for an over-long word). The one place that decides how tall a caption is, for drawing it and for stacking others.
CaptionBlock layoutBlock(const std::string& utf8, const CaptionStyle& style, int width, int height) {
    CaptionBlock block;
    block.scale = height / 1080.0;
    block.sizePx = std::max(1.0, style.sizePx * block.scale);
    block.strokePx = std::max(0.0, style.strokeWidth * block.scale);
    block.font = fontOf(style, block.sizePx);
    // The box's padding (0.35 em by default, 4 px at least when there is any), text wraps inside the box.
    block.padding = style.boxPadding > 0.0 ? std::max(4.0 * block.scale, block.sizePx * style.boxPadding) : 0.0;
    const double maxBoxWidth = std::clamp(style.maxWidth, 0.1, 1.0) * width;
    const double wrapWidth = std::max(block.sizePx, maxBoxWidth - 2.0 * block.padding);

    block.text = QString::fromStdString(utf8);
    const QStringList paragraphs = block.text.split(QLatin1Char('\n'));
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    // Card 073: each paragraph takes its direction from its first strong letter (Hebrew and Arabic run right to left);
    // the default is left to right whatever the text is.
    option.setTextDirection(Qt::LayoutDirectionAuto);
    // Lines start at x = 0 whatever the direction: the block aligns them itself (and "left" means the left of the frame).
    option.setAlignment(Qt::AlignLeft | Qt::AlignAbsolute);
    int paragraphStart = 0;
    for (const QString& paragraph : paragraphs) {
        auto layout = std::make_unique<QTextLayout>(paragraph, block.font);
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
            block.lines.push_back(out);
            y += line.height();
        }
        layout->endLayout();
        block.layouts.push_back(std::move(layout));
        paragraphStart += static_cast<int>(paragraph.size()) + 1;
    }

    for (std::size_t i = 0; i < block.lines.size(); ++i) {
        block.blockWidth = std::max(block.blockWidth, block.lines[i].width);
        block.blockHeight += i + 1 < block.lines.size() ? block.lines[i].height * style.lineSpacing : block.lines[i].height;
    }
    if (block.lines.empty()) {
        block.blockHeight = QFontInfo(block.font).pixelSize() * 1.2;
    }
    block.boxWidth = std::min(maxBoxWidth, block.blockWidth + 2.0 * block.padding);
    block.boxHeight = block.blockHeight + block.padding;
    return block;
}

} // namespace

double captionBoxHeight(const std::string& text, const CaptionStyle& style, int frameWidth, int frameHeight) {
    return layoutBlock(text, style, std::max(1, frameWidth), std::max(1, frameHeight)).boxHeight;
}

std::shared_ptr<const TitleRaster> CaptionRenderer::renderUncached(const CaptionFrame& frame) {
    const CaptionStyle& style = frame.style;
    const int width = std::max(1, frame.frameWidth);
    const int height = std::max(1, frame.frameHeight);
    CaptionBlock block = layoutBlock(frame.text, style, width, height);
    const double sizePx = block.sizePx;
    const double strokePx = block.strokePx;
    const double padding = block.padding;
    const QString& text = block.text;
    std::vector<CaptionLine>& lines = block.lines;
    const double boxWidth = block.boxWidth;
    const double boxHeight = block.boxHeight;
    const double safe = std::clamp(style.safeArea, 0.0, 0.45);
    double boxX = (width - boxWidth) / 2.0;
    double boxY = style.position == CaptionPosition::Top ? height * safe : height * (1.0 - safe) - boxHeight;
    if (!frame.closer.empty() && style.position != CaptionPosition::Custom) {
        // Card 116, another caption track on screen: this block sits beyond the ones nearer the edge, a gap apart.
        double shift = 0.0;
        for (const CaptionNeighbour& other : frame.closer) {
            shift += layoutBlock(other.text, other.style, width, height).boxHeight + 0.25 * sizePx;
        }
        boxY += style.position == CaptionPosition::Top ? shift : -shift;
        boxY = clampInto(boxY, 0.0, height - boxHeight); // never off the picture, whatever the stack's height
    }
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
    // Card 074: the characters of a word with its own effect (scale, lift, opacity) are drawn apart from the block.
    std::vector<int> fxOf(static_cast<std::size_t>(text.size()), -1);
    for (std::size_t j = 0; j < frame.wordFx.size(); ++j) {
        for (int i = std::max(0, frame.wordFx[j].begin); i < std::min(frame.wordFx[j].end, static_cast<int>(fxOf.size())); ++i) {
            fxOf[static_cast<std::size_t>(i)] = static_cast<int>(j);
        }
    }
    const auto kindAt = [&](int i) { return i < static_cast<int>(kind.size()) ? kind[static_cast<std::size_t>(i)] : kFill; };
    const auto fxAt = [&](int i) { return i < static_cast<int>(fxOf.size()) ? fxOf[static_cast<std::size_t>(i)] : -1; };

    // ---- Outlines: the fill, spoken (karaoke), and each colour span; a word with an effect keeps its paths apart.
    TitleLayoutInfo info;
    info.lines = static_cast<int>(lines.size());
    std::set<std::string> fonts;
    QPainterPath normal;
    QPainterPath spoken;
    std::vector<QPainterPath> spans(frame.colorSpans.size());
    std::vector<std::map<int, QPainterPath>> fxPaths(frame.wordFx.size()); // by effect, then by kind
    double y = boxY + padding / 2.0;
    for (CaptionLine& line : lines) {
        const double x = style.align == CaptionAlign::Left    ? boxX + padding
                         : style.align == CaptionAlign::Right ? boxX + boxWidth - padding - line.width
                                                              : boxX + (boxWidth - line.width) / 2.0;
        line.offset = QPointF(x, y - line.y);
        const int lineBegin = line.paragraphStart + line.start; // in the whole text
        const int lineEnd = lineBegin + line.length;
        for (int run = lineBegin; run < lineEnd;) {
            const int k = kindAt(run);
            const int fx = fxAt(run);
            int next = run + 1;
            while (next < lineEnd && kindAt(next) == k && fxAt(next) == fx) {
                ++next;
            }
            if (k != kHidden) {
                QPainterPath& path = fx >= 0 ? fxPaths[static_cast<std::size_t>(fx)][k]
                                     : k == kFill ? normal : k == kSpoken ? spoken : spans[static_cast<std::size_t>(k)];
                addGlyphs(*line.layout, line.start + (run - lineBegin), next - run, line.offset, path, info, fonts);
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
    // A word with an effect: scaled around its own centre, lifted, then drawn in its kind's colour at its own opacity.
    struct FxDraw {
        QPainterPath path;
        QColor color;
        double opacity = 1.0;
    };
    std::vector<FxDraw> fxDraws;
    for (std::size_t j = 0; j < fxPaths.size(); ++j) {
        const CaptionFrame::WordFx& fx = frame.wordFx[j];
        QRectF wordBounds;
        for (const auto& [k, path] : fxPaths[j]) {
            wordBounds = wordBounds.united(path.boundingRect());
        }
        if (wordBounds.isNull() && fxPaths[j].empty()) {
            continue;
        }
        const QPointF centre = wordBounds.center();
        QTransform moved;
        moved.translate(centre.x(), centre.y() - fx.lift * sizePx);
        moved.scale(std::max(0.0, fx.scale), std::max(0.0, fx.scale));
        moved.translate(-centre.x(), -centre.y());
        for (const auto& [k, path] : fxPaths[j]) {
            FxDraw d;
            d.path = moved.map(path);
            d.path.setFillRule(Qt::WindingFill);
            d.color = k == kFill      ? colorOf(style.fill)
                      : k == kSpoken ? colorOf(style.highlight)
                                     : colorOf(frame.colorSpans[static_cast<std::size_t>(k)].color);
            d.opacity = std::clamp(fx.opacity, 0.0, 1.0);
            fxDraws.push_back(std::move(d));
        }
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
    for (const FxDraw& d : fxDraws) {
        if (!d.path.isEmpty() && d.opacity > 0.0) {
            drawn.push_back(&d.path);
        }
    }

    // Card 074: the highlight bar's rectangle behind a word [begin, end): its advance on its line, the line's height.
    const auto wordRect = [&](int begin, int end) -> std::optional<QRectF> {
        if (begin < 0 || end <= begin) {
            return std::nullopt;
        }
        for (const CaptionLine& line : lines) {
            const int lineBegin = line.paragraphStart + line.start;
            const int lineEnd = lineBegin + line.length;
            if (begin < lineBegin || begin >= lineEnd) {
                continue;
            }
            const QTextLine textLine = line.layout->lineForTextPosition(begin - line.paragraphStart);
            if (!textLine.isValid()) {
                return std::nullopt;
            }
            double x0 = textLine.cursorToX(begin - line.paragraphStart);
            double x1 = textLine.cursorToX(std::min(end, lineEnd) - line.paragraphStart);
            if (x1 < x0) {
                std::swap(x0, x1);
            }
            return QRectF(line.offset.x() + x0, line.offset.y() + textLine.y(), x1 - x0, textLine.height());
        }
        return std::nullopt;
    };
    std::optional<QRectF> barRect;
    if (frame.bar.toBegin >= 0) {
        barRect = wordRect(frame.bar.toBegin, frame.bar.toEnd);
        const std::optional<QRectF> from = wordRect(frame.bar.fromBegin, frame.bar.fromEnd);
        if (barRect && from && frame.bar.t < 1.0 && std::abs(from->top() - barRect->top()) < 1.0) { // slides along a line
            const double t = std::clamp(frame.bar.t, 0.0, 1.0);
            barRect = QRectF(from->left() + (barRect->left() - from->left()) * t, barRect->top(),
                             from->width() + (barRect->width() - from->width()) * t, barRect->height());
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
        if (barRect) { // behind the text and its shadow, over the box
            const double padX = sizePx * 0.14;
            const QRectF bar = barRect->adjusted(-padX, barRect->height() * 0.07, padX, -barRect->height() * 0.05);
            painter.setPen(Qt::NoPen);
            painter.setBrush(colorOf(style.highlight, std::clamp(frame.bar.opacity, 0.0, 1.0)));
            painter.drawRoundedRect(bar, bar.height() * 0.28, bar.height() * 0.28);
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
            for (const QPainterPath* p : {&normal, &spoken}) {
                painter.strokePath(*p, pen);
            }
            for (const QPainterPath& p : spans) {
                painter.strokePath(p, pen);
            }
            for (const FxDraw& d : fxDraws) {
                painter.setOpacity(opacity * d.opacity);
                painter.strokePath(d.path, pen);
            }
            painter.setOpacity(opacity);
        }
        painter.fillPath(normal, colorOf(style.fill));
        painter.fillPath(spoken, colorOf(style.highlight));
        for (std::size_t s = 0; s < spans.size(); ++s) {
            painter.fillPath(spans[s], colorOf(frame.colorSpans[s].color));
        }
        for (const FxDraw& d : fxDraws) {
            painter.setOpacity(opacity * d.opacity);
            painter.fillPath(d.path, d.color);
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
