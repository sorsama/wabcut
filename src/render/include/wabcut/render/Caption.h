#pragma once

// Captions, burned in by the compositor (I.4): the cue under the playhead drawn as the top layer of the composite,
// so the monitor and an export with "Burn in captions" show the same pixels. Rendered on the CPU like titles
// (Title.h): text shaped by Qt (QTextLayout over HarfBuzz, font fallback, Thai OK), wrapped to the caption width,
// aligned left / centre / right inside the block, placed in the safe area at the bottom, at the top or at a custom
// centre, over an optional rounded background box, with an optional outline and a soft drop shadow. Karaoke: the
// characters before `highlightEnd` (the words already spoken) take the highlight colour. The model's caption style
// (C.7, model/CaptionStyle.h) maps onto CaptionStyle one to one (playback::captionStyleOf); its animations are
// evaluated per frame into CaptionFrame's opacity, scale and hidden ranges, and (card 074) per word into `wordFx`
// (a word's scale, lift and opacity, drawn around its own centre) and `bar` (a highlight bar sliding between words).
// Text is laid out by Qt's bidirectional algorithm: the paragraph direction follows the first strong letter, so Hebrew
// and Arabic captions read right to left with their punctuation on the correct side (card 073).

#include <wabcut/render/Title.h>

#include <array>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace wabcut::render {

enum class CaptionPosition : std::uint8_t { Bottom, Top, Custom };
enum class CaptionAlign : std::uint8_t { Left, Center, Right };

/// How captions look. Sizes are in pixels of a 1080-line frame and scale with the frame height (the convention of
/// the Captions panel's style); box and shadow sizes are fractions of the font size (em), as in the model's style.
struct CaptionStyle {
    /// Font family; empty = the platform's sans-serif. Missing glyphs fall back to other fonts.
    std::string fontFamily;
    /// 100..900 (400 regular, 600 semibold, 700 bold).
    int weight = 600;
    double sizePx = 48.0;
    /// Text colour (display-referred, sRGB-encoded 0..1).
    std::array<double, 3> fill{1.0, 1.0, 1.0};
    /// Outline width (0 = none) and colour: legibility without a box.
    double strokeWidth = 0.0;
    std::array<double, 3> stroke{0.0, 0.0, 0.0};
    /// Drop shadow of the text (and its outline): 0 opacity = none; offset `shadowDistance` em at `shadowAngle`
    /// (degrees clockwise from +x, on screen), blurred by `shadowSoftness` em (the blur radius).
    double shadowOpacity = 0.0;
    std::array<double, 3> shadowColor{0.0, 0.0, 0.0};
    double shadowDistance = 0.08;
    double shadowAngle = 135.0;
    double shadowSoftness = 0.1;
    /// Background box behind the text block, `boxPadding` em around the text, corners `boxRadius` em (at most the
    /// padding).
    bool box = true;
    std::array<double, 3> boxColor{16.0 / 255.0, 16.0 / 255.0, 19.0 / 255.0};
    double boxOpacity = 0.78;
    double boxPadding = 0.35;
    double boxRadius = 0.18;
    CaptionPosition position = CaptionPosition::Bottom;
    /// Inset of the safe area from each frame edge (fraction of the frame; 0.1 = title-safe).
    double safeArea = 0.1;
    /// Custom position: the centre of the block (fractions of the frame), kept inside the safe area.
    double centerX = 0.5;
    double centerY = 0.85;
    /// How lines align inside the block.
    CaptionAlign align = CaptionAlign::Center;
    /// Widest text block (fraction of the frame width); longer lines wrap.
    double maxWidth = 0.8;
    double lineSpacing = 1.0;
    /// Karaoke: colour of the words already spoken.
    std::array<double, 3> highlight{1.0, 0.82, 0.25};
    /// 0.7 (card #138): OpenType features for the shaper (parseFontFeatures syntax; render/TextTypography.h).
    std::string features;

    bool operator==(const CaptionStyle&) const = default;
};

/// Card 116: a caption block of another track that is on screen together with a frame's own (bilingual captions),
/// nearer to the screen edge: only what decides its height.
struct CaptionNeighbour {
    std::string text;
    CaptionStyle style;
    bool operator==(const CaptionNeighbour&) const = default;
};

/// One caption picture: what the compositor draws for a cue (or stacked cues) at one instant.
struct CaptionFrame {
    /// UTF-8; '\n' breaks a line (lines also wrap at the style's width).
    std::string text;
    /// Karaoke: UTF-16 offset up to which the text is highlighted; -1 = no highlight.
    int highlightEnd = -1;
    /// More karaoke ranges, UTF-16 [begin, end): the words already spoken of stacked cues after the first.
    std::vector<std::pair<int, int>> highlights;
    /// UTF-16 ranges drawn in their own colour instead of the fill: speakers' names (showSpeakers).
    struct ColorSpan {
        int begin = 0;
        int end = 0;
        std::array<double, 3> color{};
        bool operator==(const ColorSpan&) const = default;
    };
    std::vector<ColorSpan> colorSpans;
    /// Word-by-word animation: UTF-16 ranges not drawn yet (the layout stays the whole text's).
    std::vector<std::pair<int, int>> hidden;
    /// Per-word animation (card 074): a word's UTF-16 range [begin, end) drawn `scale` times its size around its own
    /// centre, `lift` em higher (the font size; negative = lower) and at `opacity`. The layout stays the whole text's.
    struct WordFx {
        int begin = 0;
        int end = 0;
        double scale = 1.0;
        double lift = 0.0;
        double opacity = 1.0;
        bool operator==(const WordFx&) const = default;
    };
    std::vector<WordFx> wordFx;
    /// A highlight bar behind a word, in the style's highlight colour: it sits at the word [toBegin, toEnd) (UTF-16),
    /// or, while `t` < 1 and the words share a line, `t` of the way from the word [fromBegin, fromEnd). toBegin < 0 = none.
    struct Bar {
        int fromBegin = -1;
        int fromEnd = -1;
        int toBegin = -1;
        int toEnd = -1;
        double t = 1.0;
        double opacity = 1.0;
        bool operator==(const Bar&) const = default;
    };
    Bar bar;
    /// Fade and pop animations: the caption's opacity, and its scale around the block's centre.
    double opacity = 1.0;
    double scale = 1.0;
    int frameWidth = 1920;
    int frameHeight = 1080;
    CaptionStyle style;
    /// Card 116: the blocks of other caption tracks shown together with this one that sit between it and the edge its
    /// style is placed at (bottom: below it; top: above it), nearest to the edge first. The block is moved away from
    /// the edge by their heights and a gap, so tracks stack instead of overlapping; Custom placement is never moved.
    std::vector<CaptionNeighbour> closer;

    bool operator==(const CaptionFrame&) const = default;
};

/// The height of the box a block of `text` in `style` is drawn in on a frameWidth x frameHeight frame, in pixels (what
/// CaptionRenderer lays out; the stack of bilingual tracks is measured with it). Needs a QGuiApplication for fonts.
double captionBoxHeight(const std::string& text, const CaptionStyle& style, int frameWidth, int frameHeight);

/// The case captions are drawn and exported in (the model's CaptionTextCase).
enum class CaptionCase : std::uint8_t { AsWritten, Lower, Upper };

/// `text` as a caption shows it (card 114), for a caption track in `language` (a BCP 47 tag; "tr" and "az" turn I into
/// dotless i, as Turkish and Azeri do). `stripAccents` removes the diacritics of Latin, Greek and Cyrillic letters (and
/// Hebrew points and Arabic harakat) and folds a few letters that carry no mark to plain ones (Polish l-stroke, o-slash,
/// d-stroke, Icelandic eth and thorn, ae and oe ligatures); Thai vowel and tone marks and Japanese voiced marks stay.
/// `stripPunctuation` removes commas, full stops, question and exclamation marks, quotes, brackets, dashes between words
/// and the like in every script (a removed mark between two words leaves one space); apostrophes and hyphens inside a word,
/// the separators inside a number (3.5, 1,000) and symbols such as % & # @ + / stay. Idempotent. The cue text itself is
/// never changed: this is applied when a caption is drawn or exported.
std::string captionDisplayText(const std::string& text, CaptionCase textCase, bool stripPunctuation, bool stripAccents,
                               const std::string& language = {});

/// Which words of `text` are emphasised (card 074; `spans` as captionWordSpans gives them, the result has one entry per
/// span: 1 = emphasised). A word is emphasised when it is one of `keywords` (compared without case, so "GG" finds "gg")
/// or, with `autoRules`, when it contains a digit, is written in capitals (two letters or more) or is followed by "!".
std::vector<char> captionEmphasisFlags(const std::string& text, const std::vector<std::pair<int, int>>& spans,
                                       bool autoRules, const std::vector<std::string>& keywords);

/// Word spans of `text` (UTF-16 [begin, end) offsets of every word, in order), as karaoke counts them: Unicode
/// word boundaries (QTextBoundaryFinder), so Thai, written without spaces, splits into its words as well.
std::vector<std::pair<int, int>> captionWordSpans(const std::string& text);

/// Renders caption frames (frame-size RGBA8, premultiplied, sRGB-encoded; the TitleRaster type) and keeps the most
/// recent ones: a cue is shown for many frames, a karaoke cue changes only when a word starts. Thread-safe; needs a
/// QGuiApplication for fonts.
class CaptionRenderer {
public:
    explicit CaptionRenderer(std::size_t cacheSize = 16);
    std::shared_ptr<const TitleRaster> render(const CaptionFrame& frame);
    /// The process-wide renderer the playback scheduler and export share.
    static CaptionRenderer& shared();
    static std::shared_ptr<const TitleRaster> renderUncached(const CaptionFrame& frame);

private:
    std::mutex m_mutex;
    std::size_t m_cacheSize;
    std::list<std::pair<CaptionFrame, std::shared_ptr<const TitleRaster>>> m_cache; // most recent first
};

} // namespace wabcut::render
