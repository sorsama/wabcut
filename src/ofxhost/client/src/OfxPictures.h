#pragma once

// Pictures for OpenFX plug-ins (0.7): a decoded frame as the float RGBA a plug-in takes, and the plug-in's result as the
// CPU raster the compositor draws. Internal to wabcut_ofx.
//
// A plug-in gets the frame's R'G'B' as the file encodes them: the YCbCr matrix and range applied, the transfer function left
// on (a plug-in works on the picture as a colourist sees it, as in other hosts), premultiplied by the alpha channel when the
// frame has one. The result goes back the same way: 8-bit premultiplied R'G'B', tagged with the frame's primaries and
// transfer, so the compositor's colour pipeline reads it as it read the frame.

#include <wabcut/media/FrameHandle.h>
#include <wabcut/model/Error.h>
#include <wabcut/render/ColorPipeline.h>
#include <wabcut/render/FrameView.h>
#include <wabcut/render/Title.h>

#include <expected>
#include <functional>
#include <memory>
#include <vector>

namespace wabcut::ofxhost {

struct FloatPicture {
    int width = 0;
    int height = 0;
    std::vector<float> rgba; ///< premultiplied, R'G'B' full range, rows top-down, width x height x 4
    bool hasAlpha = false;
    render::ResolvedColor color; ///< what the pixels are encoded in (the frame's primaries and transfer)
};

/// The picture of a CPU frame. Errors: InvalidArgument (not CPU planes, a size that is not valid), Unsupported (a pixel
/// format this converter does not read; an HDR frame, whose 8-bit result would band).
std::expected<FloatPicture, Error> pictureFromFrame(const media::FrameHandle& frame);

/// The 8-bit premultiplied raster of `rgba` (float, premultiplied, width x height x 4, any values: clamped to 0..1).
std::shared_ptr<const render::TitleRaster> rasterFromFloat(const float* rgba, int width, int height);

/// The FrameView of `raster` with the colour tags of `color` (RGB, full range, the frame's primaries and transfer).
render::FrameView viewOfRaster(const std::shared_ptr<const render::TitleRaster>& raster, const render::ResolvedColor& color);

/// Runs `body(firstRow, endRow)` over bands of rows of a picture, on up to eight threads (small pictures on the calling one).
void forRowBands(int rows, const std::function<void(int, int)>& body);

} // namespace wabcut::ofxhost
