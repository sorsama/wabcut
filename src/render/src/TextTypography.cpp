#include <wabcut/render/TextTypography.h>

#include <wabcut/model/Graphics.h>

#include <QFontInfo>
#include <QFontMetricsF>
#include <QImage>
#include <QLatin1StringView>
#include <QPainter>
#include <QPainterPath>
#include <QRawFont>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace wabcut::render {

namespace {

constexpr int kProbe = 96;                  // the outlines are measured at this pixel size, then scaled
constexpr double kMaxGap = 0.35 * kProbe;   // open space counts as at most this much (an "o" beside a "T")
constexpr double kMinAdjust = -0.18;        // em
constexpr double kMaxAdjust = 0.12;         // em
constexpr std::size_t kMaxFonts = 24;
constexpr std::size_t kMaxProfilesPerFont = 1024;

/// Where one glyph has ink, row by row from the cap height down to the baseline (probe pixels, from the glyph origin).
struct Profile {
    bool valid = false;
    double advance = 0.0;
    std::vector<float> left;
    std::vector<float> right;
    std::vector<bool> ink;
};

struct FontData {
    QRawFont raw;
    int rows = 0;       // cap height in probe pixels: the rows of every profile
    double xHeight = 0; // probe pixels
    double targetLower = kProbe * 0.1;
    double targetUpper = kProbe * 0.1;
    std::unordered_map<char32_t, Profile> profiles;
};

QFont probeOf(const QFont& font) {
    QFont probe = font;
    probe.setPixelSize(kProbe);
    probe.setLetterSpacing(QFont::AbsoluteSpacing, 0.0);
    probe.setKerning(false);
    probe.setCapitalization(QFont::MixedCase);
    probe.setHintingPreference(QFont::PreferNoHinting);
    return probe;
}

Profile profileOf(const QRawFont& raw, int rows, char32_t cp) {
    Profile p;
    const QString text = QString::fromUcs4(&cp, 1);
    const QList<quint32> glyphs = raw.glyphIndexesForString(text);
    if (glyphs.isEmpty() || glyphs.front() == 0) {
        return p;
    }
    const QList<QPointF> advances = raw.advancesForGlyphIndexes(glyphs);
    if (advances.isEmpty()) {
        return p;
    }
    p.advance = advances.front().x();
    const QPainterPath path = raw.pathForGlyph(glyphs.front());
    const int pad = kProbe / 2;
    const int width = static_cast<int>(std::ceil(std::max(p.advance, 1.0))) + 2 * pad;
    QImage image(width, rows, QImage::Format_Grayscale8);
    image.fill(0);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.translate(pad, rows); // the baseline is the bottom edge; the outline's y is up-negative
        painter.fillPath(path, Qt::white);
    }
    p.left.assign(static_cast<std::size_t>(rows), 0.0f);
    p.right.assign(static_cast<std::size_t>(rows), 0.0f);
    p.ink.assign(static_cast<std::size_t>(rows), false);
    for (int r = 0; r < rows; ++r) {
        const uchar* line = image.constScanLine(r);
        int first = -1;
        int last = -1;
        for (int x = 0; x < width; ++x) {
            if (line[x] > 127) {
                if (first < 0) {
                    first = x;
                }
                last = x;
            }
        }
        if (first >= 0) {
            p.ink[static_cast<std::size_t>(r)] = true;
            p.left[static_cast<std::size_t>(r)] = static_cast<float>(first - pad);
            p.right[static_cast<std::size_t>(r)] = static_cast<float>(last + 1 - pad);
        }
    }
    p.valid = true;
    return p;
}

const Profile* profileFor(FontData& data, char32_t cp) {
    auto it = data.profiles.find(cp);
    if (it == data.profiles.end()) {
        it = data.profiles.emplace(cp, profileOf(data.raw, data.rows, cp)).first;
    }
    return it->second.valid ? &it->second : nullptr;
}

/// The mean open space between the ink of `a` then `b` over the rows up to height `top` (probe pixels above the baseline).
double pairGap(const FontData& data, const Profile& a, const Profile& b, double top) {
    double sum = 0.0;
    int count = 0;
    for (int r = 0; r < data.rows; ++r) {
        const double height = data.rows - (r + 0.5);
        if (height > top) {
            continue;
        }
        ++count;
        const auto i = static_cast<std::size_t>(r);
        if (!a.ink[i] || !b.ink[i]) {
            sum += kMaxGap; // one side is open at this height
            continue;
        }
        const double gap = a.advance + static_cast<double>(b.left[i]) - static_cast<double>(a.right[i]);
        sum += std::clamp(gap, 0.0, kMaxGap);
    }
    return count > 0 ? sum / count : kMaxGap;
}

/// The spacing round and straight letters of this font settle at: the mean gap of n / o (x-height band) and H / O (cap
/// band) pairs. A font without those letters keeps the defaults (no adjustment is made from them).
void measureTargets(FontData& data) {
    const auto target = [&](std::initializer_list<char32_t> letters, double top, double fallback) {
        double sum = 0.0;
        int count = 0;
        for (const char32_t a : letters) {
            for (const char32_t b : letters) {
                const Profile* pa = profileFor(data, a);
                const Profile* pb = profileFor(data, b);
                if (pa && pb) {
                    sum += pairGap(data, *pa, *pb, top);
                    ++count;
                }
            }
        }
        return count > 0 ? sum / count : fallback;
    };
    data.targetLower = target({U'n', U'o'}, data.xHeight, data.targetLower);
    data.targetUpper = target({U'H', U'O'}, data.rows, data.targetUpper);
}

std::string keyOf(const QFont& font) {
    return font.family().toStdString() + "|" + font.styleName().toStdString() + "|" + std::to_string(static_cast<int>(font.weight())) +
           (font.italic() ? "|i" : "|r");
}

std::mutex g_mutex;
std::unordered_map<std::string, std::unique_ptr<FontData>> g_fonts;

FontData* fontDataFor(const QFont& font) {
    const std::string key = keyOf(font);
    if (const auto it = g_fonts.find(key); it != g_fonts.end()) {
        return it->second.get();
    }
    const QFont probe = probeOf(font);
    auto data = std::make_unique<FontData>();
    data->raw = QRawFont::fromFont(probe);
    if (!data->raw.isValid()) {
        return nullptr;
    }
    const QFontMetricsF metrics(probe);
    const double cap = metrics.capHeight() > 1.0 ? metrics.capHeight() : 0.7 * kProbe;
    data->rows = std::clamp(static_cast<int>(std::lround(cap)), 16, kProbe);
    data->xHeight = metrics.xHeight() > 1.0 ? std::min<double>(metrics.xHeight(), data->rows) : 0.5 * kProbe;
    measureTargets(*data);
    if (g_fonts.size() >= kMaxFonts) {
        g_fonts.clear();
    }
    FontData* raw = data.get();
    g_fonts.emplace(key, std::move(data));
    return raw;
}

bool setInScript(char32_t cp) {
    switch (QChar::script(cp)) {
    case QChar::Script_Latin:
    case QChar::Script_Greek:
    case QChar::Script_Cyrillic:
    case QChar::Script_Common:
    case QChar::Script_Inherited:
        return true;
    default:
        return false;
    }
}

bool isOpen(char32_t cp) { return QChar::isSpace(cp) || cp == static_cast<char32_t>(QChar::LineSeparator) || cp == static_cast<char32_t>(QChar::ParagraphSeparator) || cp < 0x20; }

bool isCapital(char32_t cp) { return QChar::isUpper(cp) || QChar::isDigit(cp); }

} // namespace

void applyFontFeatures(QFont& font, std::string_view features) {
    if (features.empty()) {
        return;
    }
    const auto parsed = parseFontFeatures(features);
    if (!parsed) {
        return;
    }
    for (const FontFeature& f : *parsed) {
        if (const auto tag = QFont::Tag::fromString(QLatin1StringView(f.tag.data(), 4))) {
            font.setFeature(*tag, f.value);
        }
    }
}

std::vector<double> opticalSpacings(const QFont& font, const QString& text) {
    const qsizetype units = text.size();
    if (units < 2) {
        return {};
    }
    // The code points, each at its first UTF-16 unit.
    struct Point {
        qsizetype at;
        char32_t cp;
    };
    std::vector<Point> points;
    for (qsizetype i = 0; i < units; ++i) {
        char32_t cp = text.at(i).unicode();
        if (QChar::isHighSurrogate(cp) && i + 1 < units && QChar::isLowSurrogate(text.at(i + 1).unicode())) {
            cp = QChar::surrogateToUcs4(static_cast<char16_t>(cp), text.at(i + 1).unicode());
            points.push_back({i, cp});
            ++i;
            continue;
        }
        points.push_back({i, cp});
    }
    if (points.size() < 2) {
        return {};
    }
    const bool upperCase = font.capitalization() == QFont::AllUppercase;
    for (Point& p : points) {
        if (!setInScript(p.cp)) {
            return {}; // joined and complex scripts keep the shaper's spacing
        }
        if (upperCase) {
            p.cp = QChar::toUpper(p.cp);
        }
    }
    const double size = font.pixelSize() > 0 ? font.pixelSize() : QFontInfo(font).pixelSize();
    std::vector<double> out(static_cast<std::size_t>(units), 0.0);
    std::lock_guard lock(g_mutex);
    FontData* data = fontDataFor(font);
    if (!data || size <= 0.0) {
        return {};
    }
    if (data->profiles.size() > kMaxProfilesPerFont) {
        data->profiles.clear(); // between texts, never while a pair holds two profiles
    }
    for (std::size_t k = 0; k + 1 < points.size(); ++k) {
        const char32_t a = points[k].cp;
        const char32_t b = points[k + 1].cp;
        if (isOpen(a) || isOpen(b)) {
            continue;
        }
        const Profile* pa = profileFor(*data, a);
        const Profile* pb = profileFor(*data, b);
        if (!pa || !pb) {
            continue;
        }
        const bool capitals = isCapital(a) && isCapital(b); // capitals are spaced over the cap band, the rest over the x-height
        const double gap = pairGap(*data, *pa, *pb, capitals ? static_cast<double>(data->rows) : data->xHeight);
        const double target = capitals ? data->targetUpper : data->targetLower;
        const double adjust = std::clamp((target - gap) / kProbe, kMinAdjust, kMaxAdjust);
        out[static_cast<std::size_t>(points[k].at)] = adjust * size;
    }
    return out;
}

void appendTextRanges(QList<QTextLayout::FormatRange>& out, const QString& text, int from, int to, const QTextCharFormat& format,
                      bool optical, double trackingPx) {
    if (to <= from) {
        return;
    }
    if (optical && to - from > 1) {
        const QString part = text.mid(from, to - from);
        const std::vector<double> spacing = opticalSpacings(format.font(), part);
        if (!spacing.empty()) {
            for (int i = 0; i < part.size();) {
                const int length = part.at(i).isHighSurrogate() && i + 1 < part.size() ? 2 : 1;
                QTextCharFormat one = format;
                QFont font = one.font();
                font.setLetterSpacing(QFont::AbsoluteSpacing, trackingPx + spacing[static_cast<std::size_t>(i)]);
                one.setFont(font);
                out.append(QTextLayout::FormatRange{from + i, length, one});
                i += length;
            }
            return;
        }
    }
    out.append(QTextLayout::FormatRange{from, to - from, format});
}

} // namespace wabcut::render
