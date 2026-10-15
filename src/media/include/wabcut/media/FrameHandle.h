#pragma once

// FrameHandle: a decoded video frame as it travels from media to render (architecture rev 2, Principle 2).
//
// A FrameHandle is immutable and cheap to copy: timing/colour/identity metadata plus a tagged payload that
// says where the pixels live. Today decoders only produce CPU planes; the native GPU payloads are declared so
// render's FrameImporter and the playback scheduler can be written against the final shape. No native API
// header (D3D11, CoreVideo, libdrm) is included here: native objects are carried as opaque pointers/handles.
//
// Code outside media and render must not assume CPU memory: look at the metadata, and hand the frame to
// render.

#include <wabcut/model/MediaInfo.h>
#include <wabcut/model/Time.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <variant>

namespace wabcut::media {

/// Pixel layouts of a frame.
///
/// Samples of 8-bit formats are one byte. Samples of 10-bit formats are 16-bit little-endian words: the
/// planar YUVxxxP10 formats keep the value in the LOW 10 bits (FFmpeg's yuvXXXp10le), P010 keeps it in the
/// HIGH 10 bits (the D3D11/NVDEC layout). A "texel" is the unit a GPU texture would sample: one sample for
/// planar planes, an interleaved U,V pair for the chroma plane of NV12/P010, and an R,G,B,A quadruple for
/// RGBA8. 12-bit and 16-bit sources are delivered as 10-bit for now (the MVP pipeline is 10-bit).
enum class PixelFormat : std::uint8_t {
    Unknown,
    NV12,       ///< 8-bit 4:2:0. Plane 0 Y, plane 1 interleaved UV at half width and half height.
    P010,       ///< 10-bit 4:2:0. Same layout as NV12 with 16-bit samples (value << 6).
    YUV420P,    ///< 8-bit 4:2:0 planar: Y, U, V (U/V at half width and half height).
    YUV420P10,  ///< 10-bit 4:2:0 planar.
    YUV422P,    ///< 8-bit 4:2:2 planar (U/V at half width, full height).
    YUV422P10,  ///< 10-bit 4:2:2 planar (ProRes 422, H.264 High 4:2:2).
    YUV444P,    ///< 8-bit 4:4:4 planar.
    YUV444P10,  ///< 10-bit 4:4:4 planar.
    RGBA8,      ///< Packed 8-bit R, G, B, A in a single plane.
    YUVA420P,   ///< 8-bit 4:2:0 planar + full-resolution alpha plane (plane 3).
    YUVA444P10, ///< 10-bit 4:4:4 planar + alpha plane (ProRes 4444).
};

/// Short lower-case name, e.g. "nv12", "yuva444p10".
std::string_view pixelFormatName(PixelFormat format);
/// Number of planes the format uses (1..4); 0 for Unknown.
int pixelFormatPlaneCount(PixelFormat format);
/// Bits per component: 8 or 10; 0 for Unknown.
int pixelFormatBitDepth(PixelFormat format);
/// Bytes per texel of the given plane (see PixelFormat), 0 if the plane does not exist.
int pixelFormatBytesPerTexel(PixelFormat format, int plane);
/// True for formats with an alpha channel (RGBA8, YUVA*).
bool pixelFormatHasAlpha(PixelFormat format);

/// A stream time base (one unit = num/den seconds): the model's checked TimeBase. See StreamTime.h for
/// conversions.
using TimeBase = wabcut::TimeBase;

/// Which file (and which version of it) a frame came from.
struct SourceIdentity {
    /// Content fingerprint of the source file: size + modification time (ns) + a hash of its first and last
    /// 64 KiB (see sourceFingerprint() in Fingerprint.h). 0 = unknown.
    std::uint64_t fingerprint = 0;
    /// Caller-assigned generation of the media item (bumped on relink/replace); see DecoderOptions.
    std::uint32_t generation = 0;

    constexpr bool operator==(const SourceIdentity&) const = default;
};

/// Identifies the device that produced (and, for native payloads, owns) a frame's memory. `id` 0 is the CPU
/// (software decoding); hardware devices get small process-unique ids. `generation` increases whenever that
/// device is recreated (device loss / reset), which invalidates every frame, cache entry and GPU import made
/// with the old one.
struct DeviceToken {
    std::uint32_t id = 0;
    std::uint32_t generation = 0;

    constexpr bool isCpu() const { return id == 0; }
    constexpr bool operator==(const DeviceToken&) const = default;
};

/// Display orientation from the container's display matrix: rotate clockwise by `rotationDegrees`
/// (0/90/180/270), then, if `mirrored`, flip the rotated picture horizontally (FFmpeg's convention; e.g. {90,
/// mirrored} is a transpose).
struct Orientation {
    std::int16_t rotationDegrees = 0;
    bool mirrored = false;

    constexpr bool operator==(const Orientation&) const = default;
};

/// One image plane in CPU memory. `width`/`height` are in texels (see PixelFormat); `stride` is in bytes.
struct Plane {
    const std::uint8_t* data = nullptr;
    int stride = 0;
    int width = 0;
    int height = 0;
};

// ---- Payloads
// ------------------------------------------------------------------------------------------------------
//
// Every payload holds `owner`, the reference that keeps its memory alive, and `device`. Release rules:
// - A payload may be released (its last FrameHandle copy destroyed) on any thread EXCEPT the audio callback
//   (Principle 8: no final release of large objects there).
// - Native payloads reference a surface of the decoder's pool. Holding them stalls decoding once the pool
// runs dry,
//   so consumers import/copy them and drop the handle within a couple of frame periods; long-lived caching of
//   native payloads is the FrameCache's job, which accounts for it.

/// Pixels in CPU memory (the only payload decoders produce in the MVP).
struct CpuPlanes {
    std::array<Plane, 4> planes{}; ///< pixelFormatPlaneCount(format) of them are used.
    std::shared_ptr<const void> owner;
    DeviceToken device; ///< CPU for software decoding; the hardware device for GPU->CPU readbacks.
};

/// Reserved (not produced in the MVP): a slice of a D3D11 texture array, as decoded by d3d11va.
struct D3D11Texture {
    void* texture = nullptr; ///< ID3D11Texture2D*; `owner` holds the reference.
    unsigned arraySlice = 0;
    std::shared_ptr<const void> owner;
    DeviceToken device; ///< The ID3D11Device it belongs to; render must import with the same device.
};

/// Reserved (not produced in the MVP): a CoreVideo pixel buffer from VideoToolbox.
struct CVPixelBuffer {
    void* pixelBuffer = nullptr; ///< CVPixelBufferRef; `owner` holds the retain.
    std::shared_ptr<const void> owner;
    DeviceToken device;
};

/// Reserved (not produced in the MVP): a DRM PRIME descriptor (VA-API / V4L2 export).
struct DrmPrime {
    std::array<int, 4> objectFds{-1, -1, -1, -1}; ///< dma-buf file descriptors; `owner` closes them.
    int objectCount = 0;
    std::array<std::uint32_t, 4> offsets{}; ///< Per plane, within its object.
    std::array<std::uint32_t, 4> pitches{}; ///< Per plane, bytes per row.
    std::array<int, 4> planeObject{};       ///< Which object each plane lives in.
    int planeCount = 0;
    std::uint64_t modifier = 0; ///< DRM format modifier.
    std::shared_ptr<const void> owner;
    DeviceToken device;
};

using FramePayload = std::variant<std::monostate, CpuPlanes, D3D11Texture, CVPixelBuffer, DrmPrime>;

enum class PayloadKind : std::uint8_t { None, CpuPlanes, D3D11Texture, CVPixelBuffer, DrmPrime };

// ---- FrameHandle
// -----------------------------------------------------------------------------------------------------

struct FrameHandle {
    /// Start of the frame in source time: ticks since the media origin (the earliest start among the file's
    /// audio and video streams), so audio and video of one file share a time base.
    Ticks pts{};
    /// How long the frame is displayed. [pts, pts + duration) is the source time range the frame covers.
    Ticks duration{};
    /// The frame's timestamp as the stream stores it (best-effort PTS in `timeBase` units). Stable across
    /// decoders, backends and seeks: FrameCache keys use it.
    std::int64_t streamPts = 0;
    TimeBase timeBase{};

    int width = 0;  ///< Display width in pixels (cropping applied, before orientation).
    int height = 0; ///< Display height in pixels.
    PixelFormat format = PixelFormat::Unknown;
    /// Effective colour description. Decoders resolve untagged streams to the conventional defaults (BT.709
    /// for HD, BT.601 for SD, limited range for YUV, left-sited 4:2:0 chroma), so render never sees Unknown.
    ColorInfo color{};
    AlphaMode alpha = AlphaMode::None; ///< None unless the format has alpha.
    Rate sampleAspect{1, 1};
    Orientation orientation{};
    SourceIdentity source{};

    FramePayload payload;

    PayloadKind payloadKind() const { return static_cast<PayloadKind>(payload.index()); }
    /// The CPU planes, or nullptr for any other payload.
    const CpuPlanes* cpu() const { return std::get_if<CpuPlanes>(&payload); }
    /// Device of the payload ({0, 0} = CPU / no payload).
    DeviceToken device() const;

    bool isValid() const;
    int planeCount() const { return pixelFormatPlaneCount(format); }
    /// Bytes the payload occupies: sum of |stride| * height for CPU planes; for native payloads an estimate
    /// from the format and size (what the surface costs in GPU memory).
    std::size_t byteSize() const;
    TimeRange timeRange() const { return {pts, duration}; }
    bool contains(Ticks t) const { return t >= pts && t < pts + duration; }
};

/// Rev-1 name. Kept for one release so existing code keeps compiling; new code uses FrameHandle.
using VideoFrame = FrameHandle;

} // namespace wabcut::media
