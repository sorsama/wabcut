#pragma once

// Motion graphics on the CPU (C.13, docs/internal/c13-graphics-handoff.md; I.9): a resolved draw list (model/GraphicResolve.h)
// rasterised into a frame-size, premultiplied, sRGB-encoded RGBA image that the compositor imports like a title, so the
// program monitor and an export draw the same pixels from the same list (Principle 5).
//
//   * The design canvas (DrawList::width x height) is fitted into the frame - contain, centred - and rasterised at the
//     frame's resolution: graphics are vector, never scaled up from the design size.
//   * Items are drawn bottom to top. Shapes: cubic Bezier paths, trimmed before filling and stroking (After Effects' Trim
//     Paths: start, end, offset along the path's length), solid / linear / radial fills (non-zero winding), centred
//     strokes with dashes, joins and caps over the fill. Text: QTextLayout over HarfBuzz (fallback fonts, Thai clusters
//     with their marks), point or paragraph text (wrapped between words, justified, clipped to the box), a background
//     box; while a text-range animator is active every grapheme cluster is drawn on its own after layout, faded, moved,
//     scaled and rotated about its own centre. Images: a project picture fitted into the item's box.
//   * Per item: its drop shadow (the item's coverage in the shadow colour, offset, Gaussian-blurred, just below it), its
//     masks (the I.6 coverage, render/Mask.h, of each owner's outlines combined in list order; owners multiply), its
//     opacity applied to the whole item (fill and stroke together) and its blend mode against what is below it in the
//     graphic. Blending happens in the encoded (display-referred) values, as After Effects does in an 8-bit project.
//
// graphicTextMeasurer() is the TextMeasurer the resolver pins with: the same layout code as drawing, so a box pinned to
// a name hugs the drawn name. qtLineBreaker() gives the resolver Qt's Unicode line breaking (Thai between words where
// Qt knows them), the breaks the layout uses.

#include <wabcut/model/GraphicResolve.h>
#include <wabcut/render/FrameView.h>

#include <QImage>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <vector>

namespace wabcut::render {

/// Pictures for image items (brand logos and other project media): the media item's still, display-referred, any
/// QImage format. A null image draws nothing (the item counts as missing).
class GraphicImageSource {
public:
    virtual ~GraphicImageSource() = default;
    virtual QImage picture(MediaId media) const = 0;
};

/// Measures text exactly as the rasteriser lays it out (for ResolveOptions::measurer and command::GraphicsServices).
const TextMeasurer& graphicTextMeasurer();
/// Qt's Unicode line breaking (QTextBoundaryFinder) as a LineBreaker (for ResolveOptions::lineBreaker).
const LineBreaker& qtLineBreaker();

/// What a raster holds (tests, diagnostics).
struct GraphicRasterInfo {
    int items = 0;
    int shapes = 0;
    int texts = 0;
    int images = 0;
    int missingImages = 0; ///< image items whose picture was not available
    int shadows = 0;
    int maskedItems = 0;
    int animatedClusters = 0; ///< text clusters drawn on their own (text-range animators)
};

class GraphicRaster {
public:
    GraphicRaster(int width, int height, std::vector<std::uint8_t> rgba, GraphicRasterInfo info);

    int width() const { return m_width; }
    int height() const { return m_height; }
    /// Premultiplied, sRGB-encoded R, G, B, A bytes, top row first, width * 4 per row.
    const std::vector<std::uint8_t>& rgba() const { return m_rgba; }
    const GraphicRasterInfo& info() const { return m_info; }
    /// A FrameView of the pixels, kept alive by `self` (the shared_ptr this raster lives in). One raster = one identity,
    /// so an unchanged graphic stays resident on the GPU.
    FrameView view(std::shared_ptr<const GraphicRaster> self) const;

private:
    int m_width = 0;
    int m_height = 0;
    std::vector<std::uint8_t> m_rgba;
    GraphicRasterInfo m_info;
};

/// Rasterises `list` into a `width` x `height` frame (the design canvas fitted, contain, centred). Needs a
/// QGuiApplication (fonts); any thread.
std::shared_ptr<const GraphicRaster> rasterizeGraphic(const DrawList& list, int width, int height,
                                                      const GraphicImageSource* images = nullptr);

/// The last raster of each graphic clip (by a caller's key: the clip and its nest path, and what its pictures are): an
/// equal draw list at the same size reuses it - most graphics are still most of the time. Thread-safe.
class GraphicRasterCache {
public:
    explicit GraphicRasterCache(std::size_t capacity = 64);
    static GraphicRasterCache& shared();

    std::shared_ptr<const GraphicRaster> raster(std::uint64_t key, const DrawList& list, int width, int height,
                                                const GraphicImageSource* images = nullptr);
    /// Rasterisations so far (equal lists reused do not count).
    std::uint64_t rasterizations() const { return m_rasterizations.load(); }
    void clear();

private:
    struct Entry {
        std::uint64_t key = 0;
        DrawList list;
        int width = 0;
        int height = 0;
        std::shared_ptr<const GraphicRaster> raster;
    };
    std::size_t m_capacity;
    std::mutex m_mutex;
    std::list<Entry> m_entries; // most recent first
    std::atomic<std::uint64_t> m_rasterizations{0};
};

} // namespace wabcut::render
