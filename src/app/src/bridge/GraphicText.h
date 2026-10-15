#pragma once

// Text of motion graphics laid out with Qt (C.13, M5o): one layout for measuring and for drawing, so a box pinned to a
// name (Responsive Design - Position) hugs the text the app draws.
//
//   * TextBlock: the runs of a text layer (UTF-8 with their byte starts, fonts resolved) laid out with QTextLayout -
//     HarfBuzz shaping, so a Thai consonant keeps its vowel and tone marks; paragraph text wraps at `boxWidth` between
//     words (QTextBoundaryFinder's opportunities), and only inside a word that is wider than the box, never inside a
//     grapheme cluster. '\n' starts a line. Lines are the leading apart (0: 1.2 x the largest size on the block).
//   * The block hangs from the layer's origin as docs/internal/c13-graphics-handoff.md says: its top-left (Left, Justify), its
//     top-centre (Center) or its top-right (Right); blockOrigin() is that offset.
//   * QtTextMeasurer: the model's TextMeasurer over the same layout (GraphicsServices, ResolveOptions).
//
// Thread-safe: every call builds its own QTextLayout (QFont and the font database work off the GUI thread for QImage
// painting), so the thumbnail workers and the commands share one measurer.

#include <wabcut/model/GraphicResolve.h>

#include <QColor>
#include <QList>
#include <QPointF>
#include <QSizeF>
#include <QString>
#include <QTextLayout>

#include <memory>
#include <string>
#include <vector>

class QPainter;

namespace wabcut::app {

/// One styled piece of a text block (a TextRun with its font resolved, or a DrawTextRun).
struct TextBlockRun {
    std::size_t byteStart = 0;
    std::string text;
    std::string fontFamily; ///< empty: the platform's sans-serif
    std::string fontStyle;
    int fontWeight = 400;
    double fontSize = 72.0;
    QColor color = Qt::white;
    bool italic = false;
    bool underline = false;
    double tracking = 0.0; ///< 1/1000 em
    double leading = 0.0;  ///< line height, pixels; 0 = automatic
};

class TextBlock {
public:
    TextBlock(const std::vector<TextBlockRun>& runs, TextAlign align, double boxWidth, double boxHeight);

    /// The block's size in the layer's pixels (paragraph text: boxWidth wide; boxHeight when set).
    QSizeF size() const { return m_size; }
    /// Where the block's top-left sits relative to the layer's origin (Center: -width / 2; Right: -width).
    QPointF blockOrigin() const;
    /// The laid-out lines' texts (UTF-16), in order; for tests (a wrapped line never splits a cluster).
    QStringList lineTexts() const;
    /// The UTF-16 index of a byte offset of the joined UTF-8 text.
    int utf16Index(std::size_t byte) const;
    QString text() const { return m_text; }

    /// Draws the block with its top-left at `topLeft`. `clusters` (byte ranges with their animator values), when not
    /// empty, are drawn one by one: opacity multiplied, moved, scaled and rotated about their own centre after layout.
    void draw(QPainter& painter, QPointF topLeft, const std::vector<DrawCluster>& clusters = {}) const;

private:
    void layout();

    std::vector<TextBlockRun> m_runs;
    TextAlign m_align = TextAlign::Left;
    double m_boxWidth = 0.0;
    double m_boxHeight = 0.0;
    QString m_text;
    std::vector<int> m_utf16OfByte;
    std::unique_ptr<QTextLayout> m_layout;
    std::vector<double> m_lineX; ///< each line's x inside the block (alignment of point text)
    QSizeF m_size;
};

/// The model's text measurer over TextBlock (the runs' fonts resolved by the resolver are passed in `fonts`).
class QtTextMeasurer final : public TextMeasurer {
public:
    std::array<double, 2> measure(const TextContent& text, const std::vector<std::string>& fonts) const override;
};

/// The runs of a draw list's text (DrawText) as a TextBlock's.
std::vector<TextBlockRun> blockRuns(const DrawText& text);
QColor toQColor(const Rgba& rgba);

/// The app's measurer (one instance; GraphicsServices and the thumbnails share it).
std::shared_ptr<const TextMeasurer> qtTextMeasurer();

} // namespace wabcut::app
