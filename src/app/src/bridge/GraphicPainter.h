#pragma once

// GraphicPainter (C.13, M5o): a QPainter rasteriser of a motion graphic's draw list (GraphicResolve.h) into a QImage,
// for the Essential Graphics panel's template thumbnails and previews. It is a UI preview, not the renderer: the
// program monitor and exports draw graphics with the render owner's rasteriser (docs/internal/c13-graphics-handoff.md), which
// is the reference. What it approximates:
//   * shadows are an offset copy of the item's coverage without the blur;
//   * masks are clip paths (Add / Subtract / Intersect, inverted), feather and expansion ignored;
//   * blend modes map to QPainter's composition modes (Add = Plus), against what is below in the graphic;
//   * images are a neutral placeholder box (the media's picture is not decoded here);
//   * text: TextBlock (GraphicText.h), the layout the measurer uses; baseline shifts are not drawn.
// Thread-safe and allocation-only: it runs on worker threads (never the GUI thread: thumbnails are requested through
// an asynchronous image provider).

#include <wabcut/model/GraphicResolve.h>

#include <QColor>
#include <QImage>
#include <QSize>

namespace wabcut::app {

struct GraphicPaintOptions {
    /// Fill the image with this colour first (transparent by default).
    QColor background = Qt::transparent;
};

/// `list` fitted into `size` (contain, centred), drawn bottom to top.
QImage paintGraphic(const DrawList& list, QSize size, const GraphicPaintOptions& options = {});

} // namespace wabcut::app
