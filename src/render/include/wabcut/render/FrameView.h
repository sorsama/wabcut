#pragma once

// FrameView: what the FrameImporter needs to know about a frame's pixels, independent of the media frame
// type.
//
// frameViewOf() is the ONE adapter from media's frame type to render (Principle 2). When media's rev-2
// FrameHandle lands, only src/render/src/MediaFrameAdapter.cpp changes: a CpuPlanes payload maps to a CPU
// FrameView as today, and native payloads (D3D11 texture + array slice, see docs/zero-copy.md) get their own
// view kind there.

#include <wabcut/media/FrameHandle.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/MediaInfo.h>
#include <wabcut/render/ColorPipeline.h>

#include <array>
#include <cstdint>
#include <expected>
#include <memory>

namespace wabcut::render {

/// How a frame's planes are arranged; selects the sampling path of the layer shader.
enum class PlaneLayout : std::uint8_t {
    SemiPlanar, ///< Y + interleaved CbCr (NV12, P010): R + RG textures.
    Planar3,    ///< Y, Cb, Cr: three R textures.
    Planar4,    ///< Y, Cb, Cr, A: four R textures.
    PackedRGBA, ///< One RGBA texture.
};

/// Where a frame's pixels live.
enum class FrameStorage : std::uint8_t {
    CpuPlanes,    ///< `planes` point at CPU memory; the importer uploads them.
    D3D11Texture, ///< A slice of a decoder's D3D11 texture array, sampled in place (zero-copy, docs/zero-copy.md).
};

/// One CPU plane. `width`/`height` are in texels of the plane's texture; `stride` is bytes between rows (>
/// 0).
struct PlaneView {
    const std::uint8_t* data = nullptr;
    int stride = 0;
    int width = 0;
    int height = 0;
};

struct FrameView {
    FrameStorage storage = FrameStorage::CpuPlanes;
    int width = 0;  ///< Display width in stored pixels.
    int height = 0; ///< Display height.
    PlaneLayout layout = PlaneLayout::Planar3;
    SampleEncoding encoding = SampleEncoding::Bits8;
    /// log2 chroma subsampling (4:2:0 = 1,1; 4:2:2 = 1,0; 4:4:4 = 0,0). Ignored for PackedRGBA.
    int chromaShiftX = 0;
    int chromaShiftY = 0;
    int planeCount = 0;
    std::array<PlaneView, 4> planes{};
    ColorInfo color{};
    AlphaMode alpha = AlphaMode::None;
    /// Sample (pixel) aspect ratio, width / height of one stored pixel.
    double pixelAspect = 1.0;
    /// Identity of the pixel data: equal identities (while `owner` is alive) mean equal pixels, so the
    /// importer can skip the upload of a frame that is already resident.
    const void* identity = nullptr;
    /// The object whose lifetime defines `identity`. The importer only watches it (weak reference): once it
    /// dies, the identity may be reused by another frame and a resident copy no longer matches.
    std::shared_ptr<const void> owner;
    /// Keeps the memory behind `planes` alive; the importer holds it until the GPU has consumed the upload.
    /// Defaults to `owner`. Producers pass something whose final release is cheap on the render thread (e.g.
    /// a pooled PreparedComposite) so decoder memory is never freed there.
    std::shared_ptr<const void> keepAlive;

    /// FrameStorage::D3D11Texture: the texture array (ID3D11Texture2D*), the frame's slice and the allocated
    /// (padded, e.g. 1088-row) surface size. Layout is SemiPlanar (NV12/P010); `planes` are unused.
    void* nativeTexture = nullptr;
    int arraySlice = 0;
    int surfaceWidth = 0;
    int surfaceHeight = 0;

    bool isValid() const;
};

/// Number of planes a layout uses.
constexpr int planeCountOf(PlaneLayout layout) {
    switch (layout) {
    case PlaneLayout::SemiPlanar:
        return 2;
    case PlaneLayout::Planar3:
        return 3;
    case PlaneLayout::Planar4:
        return 4;
    case PlaneLayout::PackedRGBA:
        return 1;
    }
    return 0;
}

/// The single adapter from media's frame type. Errors: InvalidArgument (invalid frame), Unsupported (a
/// payload or pixel format render cannot import).
std::expected<FrameView, Error> frameViewOf(const media::FrameHandle& frame);

} // namespace wabcut::render
