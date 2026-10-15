#pragma once

// Frame-accurate video decoding with runtime hardware-backend probing and automatic software fallback.

#include <wabcut/media/FrameHandle.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/MediaInfo.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::media {

/// Which FrameHandle payload the decoder produces.
enum class OutputMode : std::uint8_t {
    Cpu,    ///< CpuPlanes: hardware frames are read back to NV12/P010 (the MVP path).
    /// Native GPU payloads where the platform path exists, else Cpu (consumers must always look at
    /// FrameHandle::payloadKind()). Today: d3d11va decoding on the shared D3D11 device (setSharedD3D11Device)
    /// delivers D3D11Texture payloads (zero-copy, docs/zero-copy.md); every other backend/device falls back to
    /// Cpu. Such a decoder prefers d3d11va over the readback policy order, since nothing is read back.
    Native,
};

struct DecoderOptions {
    /// Try hardware backends before software.
    bool allowHardware = true;
    /// Backend to try first: one of "d3d11va", "cuda", "qsv", "videotoolbox", "vaapi", or "software" (= do
    /// not try hardware at all). Empty means the platform order (see hardwareBackendOrder()). A backend that
    /// is not available on this platform/build is skipped. Any other name is rejected with
    /// ErrorCode::InvalidArgument.
    std::string preferredBackend;
    /// Decoder frame threads. 0 = automatic: FFmpeg's choice from the CPU count for software decoding, 4 for
    /// hardware decoding (keeps GPU decode and the GPU->CPU transfer overlapped without pinning many
    /// surfaces).
    int threadCount = 0;
    OutputMode outputMode = OutputMode::Cpu;
    /// OutputMode::Native: extra decoder surfaces for the frames consumers hold at once (scheduler caches,
    /// mailboxes, frames in flight on the GPU). A native payload pins its surface until released; holding more
    /// than this starves the decoder. Consumers must release native frames within a few frame periods.
    int nativeSurfaces = 16;
    /// Stamped into FrameHandle::source.generation (the caller's media-item generation, bumped on
    /// relink/replace).
    std::uint32_t sourceGeneration = 0;
    /// Sequence conform (playback, export): frameAt(t) also takes a frame stamped up to half a tick of the stream's
    /// time base after `t`. Container timestamps are rounded to that time base (a 144 fps capture in a 1/15360 MP4
    /// timescale, OBS's millisecond MKV), so the frame meant for a sequence frame's time can sit just after it. Off: the
    /// latest frame at or before `t`, exactly.
    bool snapToStreamTicks = false;
};

/// Hardware backends VideoDecoder tries, in order (software is always the last resort).
///
/// Policy for the CPU-readback MVP: the platform chain (Windows d3d11va, cuda, qsv; macOS videotoolbox; Linux
/// vaapi, cuda) ordered by measured readback throughput for the GPUs present. With an NVIDIA GPU, cuda goes
/// first (1080p60 H.264 on an RTX 3060: cuda ~670 fps at ~0.8 ms CPU per frame vs d3d11va ~430 fps at ~2.5
/// ms, the D3D11 staging-texture map being a synchronous GPU wait). Overridden by setHardwareBackendOrder(),
/// else by the WABCUT_HW_BACKENDS environment variable (comma-separated names, e.g. "d3d11va,cuda").
/// DecoderOptions::preferredBackend still goes first for one decoder.
std::vector<std::string> hardwareBackendOrder();

/// Replaces the order for the whole process with exactly `order` (backends not listed are not tried; names
/// that are unknown or not available on this platform are ignored). An empty list restores the policy.
/// Thread-safe; affects decoders opened afterwards.
void setHardwareBackendOrder(std::vector<std::string> order);

/// Zero-copy (Windows): d3d11va decoders opened afterwards decode on `device` (ID3D11Device*) and its immediate
/// `context` (ID3D11DeviceContext*), normally the renderer's QRhi device, so OutputMode::Native frames can be
/// sampled by the renderer without any copy. The device is made multithread-protected (decoder threads and the
/// render thread share its immediate context). Returns the DeviceToken that native frames from it carry (its
/// generation increases with every call); errors: Unsupported (not Windows, or the device has no video support),
/// DeviceError. nullptr reverts to media's own device. Decoders opened earlier keep the device they had.
std::expected<DeviceToken, Error> setSharedD3D11Device(void* device, void* context);
/// Token of the shared D3D11 device, or an invalid token ({0, 0}) when none is set.
DeviceToken sharedD3D11Device();

/// GPU vendors present ("NVIDIA", "AMD", "Intel", "Microsoft", "Apple" or a hex PCI id), the primary adapter
/// first. Software adapters are not listed.
std::vector<std::string> gpuVendors();

struct VideoDecoderStats {
    std::uint64_t framesDecoded = 0; ///< Frames produced by the codec, including ones skipped while seeking.
    std::uint64_t seeks = 0;         ///< Demuxer seeks (each random access costs one or more).
    /// Hardware backends passed over for the next one or software: failed attempts, plus backends skipped
    /// because they already refused this stream format earlier in the process.
    std::uint64_t hardwareFallbacks = 0;
    /// Frames frameAt() returned (the same frame asked for twice counts twice). Fewer than framesDecoded when
    /// frames are decoded only to step over them: a seek's walk from the keyframe, or a 240 fps source read at a
    /// 60 fps sequence's times (four decoded per frame returned).
    std::uint64_t framesDelivered = 0;
    /// GPU -> CPU transfers. Native hwaccel frames are read back only when they are returned, so frames stepped over
    /// never are; QSV's wrapper decoders copy every frame at once (they recycle their surfaces).
    std::uint64_t framesReadBack = 0;
    /// Pixel-format conversions (formats FrameHandle cannot carry as they are, e.g. 12-bit), also done only for
    /// frames that are returned.
    std::uint64_t framesConverted = 0;
};

/// Decodes one video stream of one file.
///
/// Threading: single-thread-affine. One instance must only be used by one thread at a time (it may move
/// between threads between calls). Separate instances are independent and may run concurrently, even on the
/// same file. The FrameHandles it returns are immutable and may be shared with any thread; they stay valid
/// after the decoder dies.
///
/// Backend selection (open): hardware backends are tried in platform order (preferredBackend first). A
/// backend is used only if the codec has a hardware config for it, the device can be created, the codec
/// opens, AND the first frame actually comes out of the hardware. Anything else (no device, unsupported
/// profile such as H.264 10-bit or 4:2:2 on pre-RTX-50 NVIDIA GPUs, a failing first frame) moves on to the
/// next backend and finally to software. Formats a backend refused are remembered for the rest of the
/// process, so reopening such a clip is cheap. A hardware error later in the stream also falls back to
/// software and retries the request transparently. Hardware frames are transferred to CPU memory as NV12
/// (8-bit) or P010 (10-bit). Software frames keep their native layout when FrameHandle supports it, otherwise
/// they are converted to the closest supported format.
class VideoDecoder {
public:
    /// Opens `path` (UTF-8). `streamIndex` is the container stream index (VideoStreamInfo::index); -1 picks
    /// the best video stream. Errors: NotFound, InvalidArgument (not a video stream / bad options),
    /// Unsupported (no decoder), DecoderError (no frame could be decoded with any backend), IoError.
    static std::expected<std::unique_ptr<VideoDecoder>, Error>
    open(const std::string& path, int streamIndex = -1, const DecoderOptions& options = {});

    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    /// Returns the frame that CONTAINS `sourceTime`: the last frame whose pts <= sourceTime (a frame lasts
    /// until the next one starts, so VFR gaps resolve to the frame on screen). The frame's [pts, pts +
    /// duration) is exactly that span: its duration is the distance to the next presentation timestamp (the
    /// decoder looks one frame ahead before delivering); the last frame keeps its container duration, or the
    /// nominal frame period. So the returned frame always contains(sourceTime) (after the clamp below). Times
    /// before the first frame return the first frame. Calls with non-decreasing times decode forward without
    /// seeking; other jumps seek to the nearest preceding keyframe and decode forward, so every result is
    /// frame-exact. Errors: InvalidArgument (sourceTime < 0, or a time the stream's time base cannot
    /// represent), NotFound (past the end of the last frame), DecoderError (incl. timestamps out of range),
    /// IoError.
    std::expected<FrameHandle, Error> frameAt(Ticks sourceTime);

    /// Backend that decodes right now: "d3d11va", "cuda", "qsv", "videotoolbox", "vaapi" or "software".
    std::string_view backendName() const;
    bool isHardwareAccelerated() const;

    /// Probe-style description of the decoded stream (same values probe() reports).
    const VideoStreamInfo& streamInfo() const;
    /// Start of the first frame in source time (usually zero).
    Ticks firstFrameTime() const;
    /// Source identity stamped on every frame: sourceFingerprint() of the file +
    /// DecoderOptions::sourceGeneration.
    SourceIdentity source() const;
    VideoDecoderStats stats() const;

private:
    struct Impl;
    explicit VideoDecoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace wabcut::media
