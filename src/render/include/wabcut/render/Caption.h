#pragma once

// Captions, burned in by the compositor (I.4): the cue under the playhead drawn as the top layer of the composite,
// so the monitor and an export with "Burn in captions" show the same pixels. Rendered on the CPU like titles
// (Title.h): text shaped by Qt (QTextLayout over HarfBuzz, font fallback, Thai OK), wrapped to the caption width,
// aligned left / centre / right inside the block, placed in the safe area at the bottom, at the top or at a custom
// centre, over an optional rounded background box, with an optional outline and a soft drop shadow. Karaoke: the
// characters before `highlightEnd` (the words already spoken) take the highlight colour. The model's caption style
// (C.7, model/CaptionStyle.h) maps onto CaptionStyle one to one (playback::captionStyleOf); its animations are
// evaluated per frame into CaptionFrame's opacity, scale and hidden ranges.

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

    bool operator==(const CaptionStyle&) const = default;
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
    /// Fade and pop animations: the caption's opacity, and its scale around the block's centre.
    double opacity = 1.0;
    double scale = 1.0;
    int frameWidth = 1920;
    int frameHeight = 1080;
    CaptionStyle style;

    bool operator==(const CaptionFrame&) const = default;
};

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
