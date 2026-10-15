#pragma once

// FrameImporter: media frames -> RHI textures, per QRhi (contract rev 2, "render").
//
// CPU payloads are uploaded into a pool of reusable texture sets (one texture per plane: NV12/P010 as R8+RG8
// or R16+RG16, planar YUV/YUVA as R8/R16 planes, RGBA8 as one RGBA8 texture). A set is keyed by its geometry
// (layout, encoding, plane sizes) and remembers which frame it holds, so a frame that is already resident
// (paused playback, repeated frames of a 30 fps clip in a 60 fps sequence, a redraw after resize) is not
// uploaded again.
//
// Zero-copy: a FrameStorage::D3D11Texture view (a decoder surface on this QRhi's D3D11 device) is not uploaded. The
// importer wraps the texture array once per slice (QRhiTexture::createFrom + setArrayRange, R8/RG8 or R16/RG16 plane
// views), caches those views, and keeps the frame (which pins the surface in the decoder's pool) only until the GPU
// is done with it, exactly like an upload source. Surfaces of another device are rejected (counted).
//
// GPU lifetime: a set used in frame F is rewritten or destroyed only once `retireLatency` frames have begun
// since F (QRhi's frames in flight), so the GPU has finished reading it; the source memory of an upload is
// kept alive at least that long too (QRhi may read it when the frame is submitted). Preview and export use
// separate importers (separate pools). Native payloads (D3D11 textures from the decoder) will import through
// the same interface.
//
// Threading: an importer belongs to the thread that records frames on its QRhi. It never blocks; it never
// decodes.

#include <wabcut/model/Error.h>
#include <wabcut/render/FrameView.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <vector>

class QRhi;
class QRhiResourceUpdateBatch;
class QRhiTexture;

namespace wabcut::render {

enum class TexturePool : std::uint8_t { Preview, Export };

struct FrameImporterOptions {
    TexturePool pool = TexturePool::Preview;
    /// Upper bound on texture sets (each holds one frame). Enough for every layer of a frame times the frames
    /// in flight plus slack; a request beyond it fails that layer (counted in stats().poolExhausted).
    int maxTextureSets = 48;
    /// Frames a set stays untouched after its last use. -1 = QRhi::FramesInFlight of the QRhi.
    int retireLatencyFrames = -1;
};

/// A frame resident on the GPU, valid for the current frame (until the next beginFrame()).
struct ImportedFrame {
    std::array<QRhiTexture*, 4> planes{}; ///< planeCountOf(layout) textures; the rest are null.
    PlaneLayout layout = PlaneLayout::Planar3;
    SampleEncoding encoding = SampleEncoding::Bits8;
    int width = 0;
    int height = 0;
    int chromaShiftX = 0;
    int chromaShiftY = 0;
    /// Texel size of the chroma planes (plane 1), for chroma siting math. Equal to width/height for
    /// 4:4:4/RGBA.
    int chromaWidth = 0;
    int chromaHeight = 0;
    ColorInfo color{};
    AlphaMode alpha = AlphaMode::None;
    double pixelAspect = 1.0;
    /// Pool slot (stable while the set exists) and the slot's set serial, which changes whenever the slot's
    /// textures are recreated: consumers cache per-set GPU state (shader resource bindings) under (slot,
    /// serial).
    int slot = -1;
    std::uint64_t setSerial = 0;
    /// False when the frame was already resident (no upload this time).
    bool uploaded = false;
    /// The planes are texture-array views of a decoder surface (zero-copy): sample with sampler2DArray, layer 0.
    bool arrayTextures = false;
    /// Size of the luma texture (CPU: the plane; zero-copy: the padded decoder surface). The displayed picture is
    /// its top-left width x height.
    int textureWidth = 0;
    int textureHeight = 0;
};

struct FrameImporterStats {
    std::uint64_t frames = 0;       ///< beginFrame() calls.
    std::uint64_t imports = 0;      ///< Successful import() calls.
    std::uint64_t uploads = 0;      ///< Imports that uploaded pixels.
    std::uint64_t residentHits = 0; ///< Imports served by an already-resident set.
    std::uint64_t bytesUploaded = 0;
    std::uint64_t setsCreated = 0;
    std::uint64_t setsDestroyed = 0;
    std::uint64_t poolExhausted = 0; ///< Imports refused because every set was busy and the pool was full.
    std::uint64_t nativeImports = 0;  ///< Zero-copy imports (decoder surfaces sampled in place).
    std::uint64_t nativeRejected = 0; ///< Native frames from another device (they cannot be sampled here).
    std::size_t nativeViews = 0;      ///< Cached decoder-surface views.
    std::size_t liveSets = 0;
    std::size_t liveTextureBytes = 0;
    std::size_t retainedFrames = 0; ///< Sets currently keeping source memory alive.
    std::uint64_t deviceGeneration = 0;
};

class FrameImporter {
public:
    explicit FrameImporter(QRhi* rhi, FrameImporterOptions options = {});
    ~FrameImporter();
    FrameImporter(const FrameImporter&) = delete;
    FrameImporter& operator=(const FrameImporter&) = delete;

    QRhi* rhi() const { return m_rhi; }
    TexturePool pool() const { return m_options.pool; }
    int retireLatency() const { return m_retireLatency; }

    /// Starts a frame: advances the frame counter and makes sets whose last use is old enough reusable.
    /// Call once per recorded frame, before the first import() of that frame.
    void beginFrame();

    /// Makes `frame` available as textures for the current frame, adding any upload to `updates` (which must
    /// be submitted before the textures are sampled). Errors: InvalidArgument (invalid view), Unsupported
    /// (texture format not supported by the QRhi), DeviceError (texture creation failed or the pool is
    /// exhausted).
    std::expected<ImportedFrame, Error> import(const FrameView& frame, QRhiResourceUpdateBatch* updates);

    /// Same for a media frame, through frameViewOf(). `keepAlive`, when given, replaces the frame's own owner
    /// as what the importer retains (e.g. the pooled PreparedComposite that holds the frame, so the final
    /// release of decoder memory never happens on the render thread).
    std::expected<ImportedFrame, Error> import(const media::VideoFrame& frame,
                                               QRhiResourceUpdateBatch* updates,
                                               std::shared_ptr<const void> keepAlive = {});

    /// Destroys every texture and forgets every resident frame (device loss, QRhi teardown). Bumps the device
    /// generation; slots get new serials.
    void releaseResources();

    std::uint64_t deviceGeneration() const { return m_deviceGeneration; }
    FrameImporterStats stats() const;

    /// Whether `rhi` has the texture formats `layout`/`encoding` need.
    static bool supports(QRhi* rhi, PlaneLayout layout, SampleEncoding encoding);

private:
    struct SetKey {
        PlaneLayout layout = PlaneLayout::Planar3;
        SampleEncoding encoding = SampleEncoding::Bits8;
        std::array<int, 8> planeSizes{}; // (width, height) per plane
        bool operator==(const SetKey&) const = default;
    };
    struct TextureSet;
    struct NativeSet;

    std::expected<ImportedFrame, Error> importNative(const FrameView& frame);
    bool onThisDevice(void* d3dTexture);

    static SetKey keyOf(const FrameView& frame);
    bool isRetired(const TextureSet& set) const;
    std::expected<void, Error> createTextures(TextureSet& set, const SetKey& key);
    void destroyTextures(TextureSet& set);
    void upload(TextureSet& set, const FrameView& frame, QRhiResourceUpdateBatch* updates);
    ImportedFrame describe(const TextureSet& set, int slot, const FrameView& frame, bool uploaded) const;

    QRhi* m_rhi = nullptr;
    FrameImporterOptions m_options;
    int m_retireLatency = 2;
    std::uint64_t m_frame = 0;
    std::uint64_t m_nextSerial = 1;
    std::uint64_t m_deviceGeneration = 1;
    std::vector<std::unique_ptr<TextureSet>> m_sets;
    std::vector<std::unique_ptr<NativeSet>> m_native;
    std::vector<std::pair<void*, bool>> m_deviceChecks; // texture array -> on this QRhi's device
    FrameImporterStats m_stats;
};

} // namespace wabcut::render
