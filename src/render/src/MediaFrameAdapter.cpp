// The single adapter between media's FrameHandle and render (FrameView.h). Everything render knows about a
// decoded frame's memory goes through frameViewOf(). CPU payloads map to CPU plane views; native payloads
// (D3D11Texture, CVPixelBuffer, DrmPrime) are rejected until their importers exist (docs/zero-copy.md), which
// is safe because decoders fall back to OutputMode::Cpu today.

#include <wabcut/render/FrameView.h>

#if defined(_WIN32)
#include <d3d11.h>
#endif

#include <string>

namespace wabcut::render {

bool FrameView::isValid() const {
    if (storage == FrameStorage::D3D11Texture) {
        return width > 0 && height > 0 && nativeTexture != nullptr && layout == PlaneLayout::SemiPlanar &&
               planeCount == 2 && surfaceWidth >= width && surfaceHeight >= height && identity != nullptr;
    }
    if (width <= 0 || height <= 0 || planeCount != planeCountOf(layout) || identity == nullptr) {
        return false;
    }
    for (int i = 0; i < planeCount; ++i) {
        const PlaneView& p = planes[static_cast<std::size_t>(i)];
        if (p.data == nullptr || p.width <= 0 || p.height <= 0 || p.stride <= 0) {
            return false;
        }
    }
    return true;
}

namespace {

/// Native payloads: today D3D11 texture-array slices from d3d11va (NV12 / P010).
std::expected<FrameView, Error> nativeViewOf(const media::FrameHandle& frame) {
#if defined(_WIN32)
    if (const auto* native = std::get_if<media::D3D11Texture>(&frame.payload); native && native->texture) {
        D3D11_TEXTURE2D_DESC desc{};
        static_cast<ID3D11Texture2D*>(native->texture)->GetDesc(&desc);
        FrameView view;
        view.storage = FrameStorage::D3D11Texture;
        view.width = frame.width;
        view.height = frame.height;
        view.color = frame.color;
        view.pixelAspect = frame.sampleAspect.isValid() ? frame.sampleAspect.toDouble() : 1.0;
        view.layout = PlaneLayout::SemiPlanar;
        view.planeCount = 2;
        view.chromaShiftX = 1;
        view.chromaShiftY = 1;
        if (desc.Format == DXGI_FORMAT_NV12) {
            view.encoding = SampleEncoding::Bits8;
        } else if (desc.Format == DXGI_FORMAT_P010) {
            view.encoding = SampleEncoding::Bits10High;
        } else {
            return std::unexpected(Error{ErrorCode::Unsupported, "Unsupported D3D11 surface format"});
        }
        view.nativeTexture = native->texture;
        view.arraySlice = static_cast<int>(native->arraySlice);
        view.surfaceWidth = static_cast<int>(desc.Width);
        view.surfaceHeight = static_cast<int>(desc.Height);
        view.identity = native->owner.get(); // the decoder's frame reference: one per decoded picture
        view.owner = native->owner;
        view.keepAlive = native->owner;
        if (!view.isValid()) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "Invalid D3D11 frame"});
        }
        return view;
    }
#endif
    (void)frame;
    return std::unexpected(Error{ErrorCode::Unsupported, "This native frame payload is not importable"});
}

} // namespace

std::expected<FrameView, Error> frameViewOf(const media::FrameHandle& frame) {
    using media::PixelFormat;
    if (!frame.isValid()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "Invalid video frame"});
    }
    const media::CpuPlanes* cpu = frame.cpu();
    if (!cpu) {
        return nativeViewOf(frame);
    }

    FrameView view;
    view.width = frame.width;
    view.height = frame.height;
    view.color = frame.color;
    view.alpha = frame.alpha;
    view.pixelAspect = frame.sampleAspect.isValid() ? frame.sampleAspect.toDouble() : 1.0;
    view.identity = cpu->owner.get();
    view.owner = cpu->owner;
    view.keepAlive = cpu->owner;

    switch (frame.format) {
    case PixelFormat::NV12:
    case PixelFormat::P010:
        view.layout = PlaneLayout::SemiPlanar;
        view.encoding =
            frame.format == PixelFormat::NV12 ? SampleEncoding::Bits8 : SampleEncoding::Bits10High;
        view.chromaShiftX = 1;
        view.chromaShiftY = 1;
        break;
    case PixelFormat::YUV420P:
    case PixelFormat::YUV420P10:
        view.layout = PlaneLayout::Planar3;
        view.encoding =
            frame.format == PixelFormat::YUV420P ? SampleEncoding::Bits8 : SampleEncoding::Bits10Low;
        view.chromaShiftX = 1;
        view.chromaShiftY = 1;
        break;
    case PixelFormat::YUV422P:
    case PixelFormat::YUV422P10:
        view.layout = PlaneLayout::Planar3;
        view.encoding =
            frame.format == PixelFormat::YUV422P ? SampleEncoding::Bits8 : SampleEncoding::Bits10Low;
        view.chromaShiftX = 1;
        view.chromaShiftY = 0;
        break;
    case PixelFormat::YUV444P:
    case PixelFormat::YUV444P10:
        view.layout = PlaneLayout::Planar3;
        view.encoding =
            frame.format == PixelFormat::YUV444P ? SampleEncoding::Bits8 : SampleEncoding::Bits10Low;
        break;
    case PixelFormat::YUVA420P:
        view.layout = PlaneLayout::Planar4;
        view.encoding = SampleEncoding::Bits8;
        view.chromaShiftX = 1;
        view.chromaShiftY = 1;
        break;
    case PixelFormat::YUVA444P10:
        view.layout = PlaneLayout::Planar4;
        view.encoding = SampleEncoding::Bits10Low;
        break;
    case PixelFormat::RGBA8:
        view.layout = PlaneLayout::PackedRGBA;
        view.encoding = SampleEncoding::Bits8;
        break;
    case PixelFormat::Unknown:
        return std::unexpected(Error{ErrorCode::Unsupported, "Unknown pixel format"});
    }
    if (media::pixelFormatHasAlpha(frame.format) && view.alpha == AlphaMode::None) {
        view.alpha = AlphaMode::Straight; // FFmpeg's alpha outputs are straight unless tagged otherwise
    }

    view.planeCount = planeCountOf(view.layout);
    if (view.planeCount != frame.planeCount()) {
        return std::unexpected(
            Error{ErrorCode::Unsupported,
                  "Unexpected plane count for " + std::string(media::pixelFormatName(frame.format))});
    }
    for (int i = 0; i < view.planeCount; ++i) {
        const media::Plane& plane = cpu->planes[static_cast<std::size_t>(i)];
        view.planes[static_cast<std::size_t>(i)] =
            PlaneView{plane.data, plane.stride, plane.width, plane.height};
    }
    if (!view.isValid()) {
        return std::unexpected(
            Error{ErrorCode::InvalidArgument, "Video frame has missing or negative-stride planes"});
    }
    return view;
}

} // namespace wabcut::render
