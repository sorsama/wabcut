#pragma once

// Titles v1: a static text layer, rendered on the CPU into a frame-size RGBA image the compositor imports like
// any other layer (premultiplied, sRGB-encoded). Preview and export call the same code with the same inputs, so
// they get the same pixels (no QML, no animation; kinetic captions come later through keyframe curves).
//
// Text is shaped by Qt (QTextLayout over HarfBuzz, with font fallback), so complex scripts such as Thai get
// correct clusters and mark placement; the glyph runs' outlines (QRawFont::pathForGlyph) are stroked, then
// filled. The text block is centred in the frame; `align` places each line inside the block; the layer's Motion
// (position, scale, rotation) moves it. A drop shadow is the Drop Shadow effect on the layer.

#include <wabcut/render/FrameView.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace wabcut::render {

enum class TitleAlign : std::uint8_t { Left, Center, Right };

struct TitleStyle {
    /// UTF-8; '\n' starts a new line. No automatic wrapping.
    std::string text;
    /// Font family; empty = the platform's default sans-serif. Missing glyphs fall back to other fonts.
    std::string fontFamily;
    /// Style name within the family ("Bold", "Italic", "Bold Italic", ...); empty = regular.
    std::string fontStyle;
    /// Font size in frame (sequence) pixels.
    double sizePx = 96.0;
    /// Fill colour, display-referred (sRGB-encoded) 0..1, and its opacity.
    std::array<double, 3> fill{1.0, 1.0, 1.0};
    double fillOpacity = 1.0;
    /// Outer stroke width in pixels (0 = none), colour and opacity.
    double strokeWidth = 0.0;
    std::array<double, 3> stroke{0.0, 0.0, 0.0};
    double strokeOpacity = 1.0;
    TitleAlign align = TitleAlign::Center;
    /// Line advance as a multiple of the font's line height.
    double lineSpacing = 1.0;
    /// Extra letter spacing in thousandths of an em (Premiere's tracking).
    double tracking = 0.0;
    /// The title's frame: normally the sequence size.
    int frameWidth = 1920;
    int frameHeight = 1080;

    bool operator==(const TitleStyle&) const = default;
};

/// What the layout produced (diagnostics and tests).
struct TitleLayoutInfo {
    int lines = 0;
    int glyphs = 0;
    /// Glyphs no font could provide (rendered as nothing).
    int missingGlyphs = 0;
    /// Bounding box of the text in frame pixels: x, y, width, height.
    std::array<double, 4> bounds{};
    /// Font families the glyph runs actually used (fallbacks included).
    std::vector<std::string> fonts;
};

/// One rendered title: frame-size RGBA8, premultiplied alpha, sRGB-encoded.
class TitleRaster {
public:
    TitleRaster(int width, int height, std::vector<std::uint8_t> rgba, TitleLayoutInfo info);

    int width() const { return m_width; }
    int height() const { return m_height; }
    const std::vector<std::uint8_t>& rgba() const { return m_rgba; }
    const TitleLayoutInfo& info() const { return m_info; }
    /// A FrameView of the pixels, kept alive by `self` (the shared_ptr this raster lives in).
    FrameView view(std::shared_ptr<const TitleRaster> self) const;

private:
    int m_width = 0;
    int m_height = 0;
    std::vector<std::uint8_t> m_rgba;
    TitleLayoutInfo m_info;
};

/// Renders titles and keeps the most recent ones (a title is usually shown for many frames). Thread-safe; may run
/// on any thread (QPainter on a QImage), but needs a QGuiApplication for fonts.
class TitleRenderer {
public:
    explicit TitleRenderer(std::size_t cacheSize = 16);

    std::shared_ptr<const TitleRaster> render(const TitleStyle& style);

    /// The process-wide renderer the playback scheduler and export share.
    static TitleRenderer& shared();

    /// Renders without the cache.
    static std::shared_ptr<const TitleRaster> renderUncached(const TitleStyle& style);

    /// Grapheme-cluster boundaries of `text` as the title layout sees them (UTF-16 offsets of every valid cursor
    /// position, 0 and the end included). Lets tests check complex-script shaping.
    static std::vector<int> clusterBoundaries(const TitleStyle& style);

private:
    std::mutex m_mutex;
    std::size_t m_cacheSize;
    std::list<std::pair<TitleStyle, std::shared_ptr<const TitleRaster>>> m_cache; // most recent first
};

} // namespace wabcut::render
