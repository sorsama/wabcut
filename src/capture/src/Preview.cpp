#include <wabcut/capture/Preview.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace wabcut::capture {

namespace {

AVPixelFormat avFormatOf(PixelFormat pixel) {
    switch (pixel) {
    case PixelFormat::Nv12:
        return AV_PIX_FMT_NV12;
    case PixelFormat::Yuy2:
        return AV_PIX_FMT_YUYV422;
    case PixelFormat::Uyvy:
        return AV_PIX_FMT_UYVY422;
    case PixelFormat::I420:
        return AV_PIX_FMT_YUV420P;
    case PixelFormat::P010:
        return AV_PIX_FMT_P010LE;
    case PixelFormat::Bgr24:
        return AV_PIX_FMT_BGR24;
    case PixelFormat::Bgra:
        return AV_PIX_FMT_BGR0;
    }
    return AV_PIX_FMT_NONE;
}

struct FrameDeleter {
    void operator()(AVFrame* f) const noexcept { av_frame_free(&f); }
};
struct SwsDeleter {
    void operator()(SwsContext* c) const noexcept { sws_free_context(&c); }
};

int evenAtLeast2(double v) {
    return std::max(2, static_cast<int>(std::lround(v / 2.0)) * 2);
}

} // namespace

std::expected<PreviewImage, Error> makePreview(const CaptureFrame& frame, Rate pixelAspect, int maxWidth) {
    if (!frame.valid() || maxWidth < 2) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "No picture to show"});
    }
    const double par = pixelAspect.isValid() ? static_cast<double>(pixelAspect.num) / pixelAspect.den : 1.0;
    const double shownWidth = frame.width * std::clamp(par, 0.25, 4.0);
    const double scale = std::min(1.0, static_cast<double>(maxWidth) / shownWidth);
    const int outW = evenAtLeast2(shownWidth * scale);
    const int outH = evenAtLeast2(frame.height * scale);

    std::unique_ptr<AVFrame, FrameDeleter> src(av_frame_alloc());
    std::unique_ptr<AVFrame, FrameDeleter> dst(av_frame_alloc());
    std::unique_ptr<SwsContext, SwsDeleter> sws(sws_alloc_context());
    if (!src || !dst || !sws) {
        return std::unexpected(Error{ErrorCode::EncoderError, "Out of memory (preview)"});
    }
    src->format = avFormatOf(frame.pixel);
    src->width = frame.width;
    src->height = frame.height;
    for (int p = 0; p < planeCount(frame.pixel) && p < 3; ++p) {
        src->data[p] = const_cast<std::uint8_t*>(frame.plane(p));
        src->linesize[p] = frame.stride(p);
    }
    const bool rgb = frame.pixel == PixelFormat::Bgr24 || frame.pixel == PixelFormat::Bgra;
    src->colorspace = rgb ? AVCOL_SPC_RGB : (frame.height >= 720 ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M);
    src->color_range = rgb ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    dst->format = AV_PIX_FMT_RGBA;
    dst->width = outW;
    dst->height = outH;
    if (av_frame_get_buffer(dst.get(), 0) < 0) {
        return std::unexpected(Error{ErrorCode::EncoderError, "Out of memory (preview)"});
    }
    dst->colorspace = AVCOL_SPC_RGB;
    dst->color_range = AVCOL_RANGE_JPEG;
    sws->flags = SWS_FAST_BILINEAR;
    if (sws_scale_frame(sws.get(), dst.get(), src.get()) < 0) {
        return std::unexpected(Error{ErrorCode::EncoderError, "Cannot convert the picture"});
    }
    PreviewImage image;
    image.width = outW;
    image.height = outH;
    image.rgba.resize(static_cast<std::size_t>(outW) * static_cast<std::size_t>(outH) * 4);
    for (int y = 0; y < outH; ++y) {
        std::memcpy(image.rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(outW) * 4,
                    dst->data[0] + static_cast<std::ptrdiff_t>(y) * dst->linesize[0], static_cast<std::size_t>(outW) * 4);
    }
    return image;
}

} // namespace wabcut::capture
