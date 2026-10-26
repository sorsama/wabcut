#include "FfmpegUtil.h"

#include <wabcut/media/Fingerprint.h>
#include <wabcut/media/GpuInfo.h>
#include <wabcut/media/RawDevelop.h>
#include <wabcut/media/VideoDecoder.h>
#include <wabcut/model/RawSourceSettings.h>

#if defined(_WIN32)
// Before FFmpeg's extern "C" block: d3d11.h declares C++ operators (hwcontext_d3d11va.h includes it again).
#include <d3d11_4.h> // ID3D11Multithread
#endif

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/raw_color_params.h>
#if defined(_WIN32)
#include <libavutil/hwcontext_d3d11va.h>
#endif
}

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace wabcut::media {

using namespace detail;

namespace {

constexpr std::string_view kSoftware = "software";
constexpr int kDefaultHardwareThreads = 4;

// ---- Hardware backends
// -----------------------------------------------------------------------------------------

struct BackendSpec {
    std::string_view name;
    AVHWDeviceType type;
    /// Empty: the native decoder with a hwaccel. Otherwise a dedicated wrapper decoder "<codec><suffix>"
    /// (QSV).
    std::string_view decoderSuffix;
};

const std::vector<BackendSpec>& platformBackends() {
#if defined(_WIN32)
    static const std::vector<BackendSpec> backends{
        {"d3d11va", AV_HWDEVICE_TYPE_D3D11VA, ""},
        {"cuda", AV_HWDEVICE_TYPE_CUDA, ""},
        {"qsv", AV_HWDEVICE_TYPE_QSV, "_qsv"},
    };
#elif defined(__APPLE__)
    static const std::vector<BackendSpec> backends{
        {"videotoolbox", AV_HWDEVICE_TYPE_VIDEOTOOLBOX, ""},
    };
#else
    static const std::vector<BackendSpec> backends{
        {"vaapi", AV_HWDEVICE_TYPE_VAAPI, ""},
        {"cuda", AV_HWDEVICE_TYPE_CUDA, ""},
    };
#endif
    return backends;
}

bool isKnownBackendName(std::string_view name) {
    return name.empty() || name == kSoftware || name == "d3d11va" || name == "cuda" || name == "qsv" ||
           name == "videotoolbox" || name == "vaapi";
}

/// Stream properties a hardware decoder accepts or refuses as a whole (profile, layout, size).
struct HwFormatKey {
    AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
    AVCodecID codec = AV_CODEC_ID_NONE;
    int profile = 0;
    int pixelFormat = 0;
    int width = 0;
    int height = 0;

    auto operator<=>(const HwFormatKey&) const = default;
};

/// Process-wide hardware devices, created on first use and shared by all decoders (FFmpeg's device contexts
/// are internally synchronised). A device that failed to be created is not retried for the rest of the
/// process, and a stream format a backend refused is not offered to that backend again (a timeline opens the
/// same clip many times; a failed NVDEC probe alone costs tens of milliseconds). Never destroyed: tearing GPU
/// devices down during static destruction races driver threads.
class HwDeviceRegistry {
public:
    bool isRejected(const HwFormatKey& key) {
        std::lock_guard lock(m_mutex);
        return m_rejected.contains(key);
    }

    void markRejected(const HwFormatKey& key) {
        std::lock_guard lock(m_mutex);
        m_rejected.insert(key);
    }

    static HwDeviceRegistry& instance() {
        static auto* registry = new HwDeviceRegistry();
        return *registry;
    }

    /// A new reference to the shared device of `type` (created on first use), or nullptr if it cannot be
    /// created. `token` receives the device's identity: id = the FFmpeg device type, generation = how many
    /// times the device has been created in this process (1 until device-loss recovery recreates one).
    BufferRefPtr acquire(AVHWDeviceType type, DeviceToken* token = nullptr, bool* shared = nullptr) {
        std::lock_guard lock(m_mutex);
        Slot& slot = m_slots[type];
        if (shared) {
            *shared = slot.shared && slot.device;
        }
        if (slot.failed) {
            return nullptr;
        }
        if (!slot.device) {
            AVBufferRef* device = nullptr;
            // Issue #10: d3d11va decodes on the preview's adapter (previewGpu(); FFmpeg takes the DXGI index), not
            // on whichever adapter Windows lists first.
            std::string adapter;
            if (type == AV_HWDEVICE_TYPE_D3D11VA) {
                if (const int index = previewAdapterIndex(); index >= 0) {
                    adapter = std::to_string(index);
                }
            }
            const int r = av_hwdevice_ctx_create(&device, type, adapter.empty() ? nullptr : adapter.c_str(), nullptr, 0);
            if (r < 0) {
                slot.failed = true;
                log(LogLevel::Info, "Hardware device '{}' is unavailable: {}",
                    av_hwdevice_get_type_name(type), avErrorString(r));
                return nullptr;
            }
            slot.device.reset(device);
            ++slot.generation;
        }
        if (token) {
            *token = DeviceToken{static_cast<std::uint32_t>(type), slot.generation};
        }
        return BufferRefPtr(av_buffer_ref(slot.device.get()));
    }

    /// Installs an externally created device for `type` (nullptr: back to creating one on demand) and returns
    /// its token.
    DeviceToken setShared(AVHWDeviceType type, BufferRefPtr device) {
        std::lock_guard lock(m_mutex);
        Slot& slot = m_slots[type];
        slot.shared = device != nullptr;
        slot.device = std::move(device);
        slot.failed = false;
        ++slot.generation;
        return slot.shared ? DeviceToken{static_cast<std::uint32_t>(type), slot.generation} : DeviceToken{};
    }

    DeviceToken sharedToken(AVHWDeviceType type) {
        std::lock_guard lock(m_mutex);
        const auto it = m_slots.find(type);
        return it != m_slots.end() && it->second.shared && it->second.device
                   ? DeviceToken{static_cast<std::uint32_t>(type), it->second.generation}
                   : DeviceToken{};
    }

private:
    struct Slot {
        BufferRefPtr device;
        bool failed = false;
        bool shared = false; ///< `device` was installed by setShared() (the renderer's device)
        std::uint32_t generation = 0;
    };
    std::mutex m_mutex;
    std::map<AVHWDeviceType, Slot> m_slots;
    std::set<HwFormatKey> m_rejected;
};

AVPixelFormat hwPixelFormat(const AVCodec* codec, AVHWDeviceType type) {
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (!config) {
            return AV_PIX_FMT_NONE;
        }
        if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0 && config->device_type == type) {
            return config->pix_fmt;
        }
    }
}

/// get_format() state for one codec context (AVCodecContext::opaque).
struct HwNegotiation {
    AVPixelFormat hwFormat = AV_PIX_FMT_NONE;
    /// The decoder did not offer the hardware format (or its hwaccel failed to initialise).
    bool rejected = false;
    /// OutputMode::Native on d3d11va: surfaces must be viewable by shaders and the pool must leave room for the
    /// frames consumers hold.
    bool shaderResource = false;
    int extraSurfaces = 0;
};

#if defined(_WIN32)
/// Gives the codec a d3d11va frames context whose surfaces shaders can sample (zero-copy).
void setupShaderResourceFrames(AVCodecContext* ctx, AVPixelFormat hwFormat, int extraSurfaces) {
    if (ctx->hw_frames_ctx || !ctx->hw_device_ctx) {
        return;
    }
    AVBufferRef* frames = nullptr;
    if (avcodec_get_hw_frames_parameters(ctx, ctx->hw_device_ctx, hwFormat, &frames) < 0 || !frames) {
        return; // FFmpeg creates its default (decoder-only) pool; ready() then falls back to readback
    }
    auto* framesCtx = reinterpret_cast<AVHWFramesContext*>(frames->data);
    auto* d3d = static_cast<AVD3D11VAFramesContext*>(framesCtx->hwctx);
    d3d->BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    framesCtx->initial_pool_size += std::max(0, extraSurfaces);
    if (av_hwframe_ctx_init(frames) < 0) {
        av_buffer_unref(&frames);
        return;
    }
    ctx->hw_frames_ctx = frames;
}
#endif

AVPixelFormat negotiateFormat(AVCodecContext* ctx, const AVPixelFormat* formats) {
    auto* negotiation = static_cast<HwNegotiation*>(ctx->opaque);
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == negotiation->hwFormat) {
#if defined(_WIN32)
            if (negotiation->shaderResource && *p == AV_PIX_FMT_D3D11) {
                setupShaderResourceFrames(ctx, *p, negotiation->extraSurfaces);
            }
#endif
            return *p;
        }
    }
    // FFmpeg calls get_format again without the hardware format when the hwaccel cannot handle the stream
    // (e.g. an unsupported profile). Decode in software for now; open() notices and moves on to the next
    // backend.
    negotiation->rejected = true;
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*p);
        if (desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0) {
            return *p;
        }
    }
    return AV_PIX_FMT_NONE;
}

// ---- Pixel formats
// ---------------------------------------------------------------------------------------------

struct FormatChoice {
    PixelFormat format = PixelFormat::Unknown;
    bool fullRange = false; ///< The FFmpeg format implies full range (yuvj*).
};

/// Formats a decoded frame can be handed out in without conversion.
std::optional<FormatChoice> nativeFormat(int avFormat) {
    switch (static_cast<AVPixelFormat>(avFormat)) {
    case AV_PIX_FMT_NV12:
        return FormatChoice{PixelFormat::NV12};
    case AV_PIX_FMT_P010LE:
        return FormatChoice{PixelFormat::P010};
    case AV_PIX_FMT_YUV420P:
        return FormatChoice{PixelFormat::YUV420P};
    case AV_PIX_FMT_YUVJ420P:
        return FormatChoice{PixelFormat::YUV420P, true};
    case AV_PIX_FMT_YUV422P:
        return FormatChoice{PixelFormat::YUV422P};
    case AV_PIX_FMT_YUVJ422P:
        return FormatChoice{PixelFormat::YUV422P, true};
    case AV_PIX_FMT_YUV444P:
        return FormatChoice{PixelFormat::YUV444P};
    case AV_PIX_FMT_YUVJ444P:
        return FormatChoice{PixelFormat::YUV444P, true};
    case AV_PIX_FMT_YUV420P10LE:
        return FormatChoice{PixelFormat::YUV420P10};
    case AV_PIX_FMT_YUV422P10LE:
        return FormatChoice{PixelFormat::YUV422P10};
    case AV_PIX_FMT_YUV444P10LE:
        return FormatChoice{PixelFormat::YUV444P10};
    case AV_PIX_FMT_RGBA:
        return FormatChoice{PixelFormat::RGBA8};
    case AV_PIX_FMT_YUVA420P:
        return FormatChoice{PixelFormat::YUVA420P};
    case AV_PIX_FMT_YUVA444P10LE:
        return FormatChoice{PixelFormat::YUVA444P10};
    default:
        return std::nullopt;
    }
}

/// Closest supported layout for formats FrameHandle cannot carry as they are (12/16-bit, 4:1:1, RGB, gray,
/// other alpha layouts, ...). Alpha is kept: YUV with alpha becomes YUVA420P (8-bit 4:2:0) or YUVA444P10,
/// RGB(A) becomes RGBA8. Precision above 10 bits is reduced to 10 bits (the MVP pipeline is 10-bit).
PixelFormat conversionTarget(int avFormat) {
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(avFormat));
    if (!desc) {
        return PixelFormat::Unknown;
    }
    const bool highDepth = desc->comp[0].depth > 8;
    if ((desc->flags & AV_PIX_FMT_FLAG_RGB) != 0) {
        return PixelFormat::RGBA8;
    }
    if ((desc->flags & AV_PIX_FMT_FLAG_ALPHA) != 0) {
        const bool subsampled420 =
            desc->nb_components >= 3 && desc->log2_chroma_w == 1 && desc->log2_chroma_h == 1;
        return subsampled420 && !highDepth ? PixelFormat::YUVA420P : PixelFormat::YUVA444P10;
    }
    if (desc->nb_components < 3) { // gray (+alpha)
        return highDepth ? PixelFormat::YUV420P10 : PixelFormat::YUV420P;
    }
    if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 1) {
        return highDepth ? PixelFormat::YUV420P10 : PixelFormat::YUV420P;
    }
    if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 0) {
        return highDepth ? PixelFormat::YUV422P10 : PixelFormat::YUV422P;
    }
    return highDepth ? PixelFormat::YUV444P10 : PixelFormat::YUV444P;
}

/// 0.2 camera RAW: a colour filter array (ProRes RAW, CinemaDNG through FFmpeg's TIFF decoder), developed into 10-bit
/// Rec.709 Y'CbCr 4:4:4 (RawDevelop.h).
bool isBayer(int avFormat) {
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(avFormat));
    return desc && (desc->flags & AV_PIX_FMT_FLAG_BAYER) != 0;
}

/// Fills Unknown colour fields with the conventional defaults so render never has to guess.
ColorInfo resolveColor(ColorInfo c, PixelFormat format, int width, int height, bool fullRange) {
    const bool rgb = format == PixelFormat::RGBA8;
    const bool hd = width > 1024 || height > 576;
    if (rgb) {
        c.matrix = ColorMatrix::RGB;
        c.range = ColorRange::Full;
    } else {
        if (c.matrix == ColorMatrix::Unknown || c.matrix == ColorMatrix::RGB) {
            c.matrix = hd ? ColorMatrix::BT709 : ColorMatrix::BT601;
        }
        if (fullRange) {
            c.range = ColorRange::Full;
        } else if (c.range == ColorRange::Unknown) {
            c.range = ColorRange::Limited;
        }
    }
    if (c.primaries == ColorPrimaries::Unknown) {
        switch (c.matrix) {
        case ColorMatrix::BT2020NonConstant:
        case ColorMatrix::BT2020Constant:
            c.primaries = ColorPrimaries::BT2020;
            break;
        case ColorMatrix::BT601:
            c.primaries = height == 576 ? ColorPrimaries::BT601_625 : ColorPrimaries::BT601_525;
            break;
        default:
            c.primaries = ColorPrimaries::BT709;
            break;
        }
    }
    if (c.transfer == TransferFunction::Unknown) {
        c.transfer = rgb ? TransferFunction::SRGB : TransferFunction::BT709;
    }
    if (c.chromaLocation == ChromaLocation::Unknown) {
        switch (format) {
        case PixelFormat::NV12:
        case PixelFormat::P010:
        case PixelFormat::YUV420P:
        case PixelFormat::YUV420P10:
        case PixelFormat::YUVA420P:
            // MPEG-2/H.264/HEVC default: co-sited with the left luma sample; JPEG-style full range is
            // centred.
            c.chromaLocation = fullRange ? ChromaLocation::Center : ChromaLocation::Left;
            break;
        case PixelFormat::YUV422P:
        case PixelFormat::YUV422P10:
            c.chromaLocation = ChromaLocation::Left;
            break;
        default: // no chroma subsampling
            c.chromaLocation = ChromaLocation::Center;
            break;
        }
    }
    return c;
}

CpuPlanes cpuPlanesOf(PixelFormat format, const AVFrame* f) {
    const int w = f->width;
    const int h = f->height;
    const int halfW = (w + 1) / 2;
    const int halfH = (h + 1) / 2;
    int cw = w;
    int ch = h;
    switch (format) {
    case PixelFormat::NV12:
    case PixelFormat::P010:
    case PixelFormat::YUV420P:
    case PixelFormat::YUV420P10:
    case PixelFormat::YUVA420P:
        cw = halfW;
        ch = halfH;
        break;
    case PixelFormat::YUV422P:
    case PixelFormat::YUV422P10:
        cw = halfW;
        break;
    default:
        break;
    }
    CpuPlanes cpu;
    const int n = pixelFormatPlaneCount(format);
    for (int i = 0; i < n; ++i) {
        const bool fullSize = i == 0 || i == 3; // luma and alpha
        cpu.planes[static_cast<std::size_t>(i)] =
            Plane{f->data[i], f->linesize[i], fullSize ? w : cw, fullSize ? h : ch};
    }
    return cpu;
}

/// A decoded frame plus whether its duration came from the stream (true) or the nominal rate (false).
///
/// Only a frame that is delivered gets a payload: decoding forward to a target (a seek's walk from the keyframe, a
/// 240 fps source read at a 60 fps sequence's times) steps over frames that are never shown, and those cost the
/// decode alone, no GPU -> CPU copy, no pixel-format conversion, no payload allocation.
struct DecodedFrame {
    FrameHandle frame;
    /// Hardware surface not yet read back: `frame` has its metadata but no payload until it is delivered.
    FramePtr hw;
    /// Software frame (or a QSV frame already in CPU memory) not wrapped yet; converted to frame.format first when
    /// `convertOnReady`.
    FramePtr raw;
    bool convertOnReady = false;
    DeviceToken rawDevice{}; ///< CpuPlanes::device of the payload made from `raw`
    bool fullRange = false;
};

} // namespace

namespace {

std::mutex& orderMutex() {
    static std::mutex m;
    return m;
}

std::vector<std::string>& orderOverride() {
    static std::vector<std::string> names;
    return names;
}

std::vector<std::string> policyOrder() {
#if defined(_WIN32)
    // Issue #10: the GPU the preview renders on decides (previewGpu(): the high-performance one, or the Settings
    // choice at start), so decode and preview share a GPU; an NVIDIA card that is not that GPU is not tried first.
    const GpuSelection gpu = previewGpu();
    const bool nvidia = gpu.adapter ? gpu.adapter->vendor == "NVIDIA" : false;
#else
    // Linux picks the preview's GPU itself (PRIME): an NVIDIA card anywhere goes first, as before.
    const auto vendors = gpuVendors();
    const bool nvidia = std::find(vendors.begin(), vendors.end(), "NVIDIA") != vendors.end();
#endif
#if defined(_WIN32)
    // A GPU the user chose (Settings > Graphics card, Power saving / a GPU in Windows Graphics settings, or a
    // QT_D3D_ADAPTER_INDEX set before start): only the backends that decode on it - d3d11va on its adapter, CUDA when it
    // is NVIDIA's, Quick Sync when it is Intel's - and then software, rather than waking the GPU the user turned away
    // from (a laptop's discrete card on battery).
    const bool pinned = gpu.reason == GpuReason::UserChoice || gpu.reason == GpuReason::WindowsPowerSaving ||
                        gpu.reason == GpuReason::WindowsSpecific || gpu.reason == GpuReason::Override;
    if (pinned && gpu.adapter) {
        if (nvidia) {
            return {"cuda", "d3d11va"};
        }
        if (gpu.adapter->vendor == "Intel") {
            return {"d3d11va", "qsv"};
        }
        return {"d3d11va"};
    }
    return nvidia ? std::vector<std::string>{"cuda", "d3d11va", "qsv"}
                  : std::vector<std::string>{"d3d11va", "cuda", "qsv"};
#elif defined(__APPLE__)
    (void)nvidia;
    return {"videotoolbox"};
#else
    return nvidia ? std::vector<std::string>{"cuda", "vaapi"} : std::vector<std::string>{"vaapi", "cuda"};
#endif
}

std::vector<std::string> parseList(std::string_view text) {
    std::vector<std::string> names;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find(',', start), text.size());
        std::string_view item = text.substr(start, end - start);
        while (!item.empty() && item.front() == ' ') {
            item.remove_prefix(1);
        }
        while (!item.empty() && item.back() == ' ') {
            item.remove_suffix(1);
        }
        if (!item.empty()) {
            names.emplace_back(item);
        }
        start = end + 1;
    }
    return names;
}

/// The effective order as specs of this platform: override, else environment, else policy.
std::vector<const BackendSpec*> effectiveOrder() {
    std::vector<std::string> names;
    {
        std::lock_guard lock(orderMutex());
        names = orderOverride();
    }
    if (names.empty()) {
        if (const char* env = std::getenv("WABCUT_HW_BACKENDS")) {
            names = parseList(env);
        }
    }
    if (names.empty()) {
        names = policyOrder();
    }
    std::vector<const BackendSpec*> specs;
    for (const std::string& name : names) {
        const auto& all = platformBackends();
        const auto it =
            std::find_if(all.begin(), all.end(), [&](const BackendSpec& s) { return s.name == name; });
        if (it != all.end() && std::find(specs.begin(), specs.end(), &*it) == specs.end()) {
            specs.push_back(&*it);
        }
    }
    return specs;
}

} // namespace

std::expected<DeviceToken, Error> setSharedD3D11Device(void* device, void* context) {
#if defined(_WIN32)
    HwDeviceRegistry& registry = HwDeviceRegistry::instance();
    if (!device || !context) {
        registry.setShared(AV_HWDEVICE_TYPE_D3D11VA, nullptr);
        return DeviceToken{};
    }
    auto* d3dDevice = static_cast<ID3D11Device*>(device);
    auto* d3dContext = static_cast<ID3D11DeviceContext*>(context);
    ID3D11VideoDevice* video = nullptr;
    if (FAILED(d3dDevice->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void**>(&video)))) {
        return std::unexpected(Error{ErrorCode::Unsupported, "The D3D11 device has no video support"});
    }
    video->Release();
    // Decoder threads and the render thread share the immediate context.
    ID3D11Multithread* multithread = nullptr;
    if (SUCCEEDED(d3dContext->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&multithread)))) {
        multithread->SetMultithreadProtected(TRUE);
        multithread->Release();
    }
    AVBufferRef* ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!ref) {
        return std::unexpected(Error{ErrorCode::DeviceError, "Out of memory"});
    }
    auto* hw = static_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(ref->data)->hwctx);
    d3dDevice->AddRef(); // released by FFmpeg with the device context
    d3dContext->AddRef();
    hw->device = d3dDevice;
    hw->device_context = d3dContext;
    if (const int r = av_hwdevice_ctx_init(ref); r < 0) {
        av_buffer_unref(&ref);
        return std::unexpected(ffmpegError(ErrorCode::DeviceError, "Cannot use the shared D3D11 device", r));
    }
    return registry.setShared(AV_HWDEVICE_TYPE_D3D11VA, BufferRefPtr(ref));
#else
    (void)device;
    (void)context;
    return std::unexpected(Error{ErrorCode::Unsupported, "D3D11 is Windows only"});
#endif
}

DeviceToken sharedD3D11Device() {
#if defined(_WIN32)
    return HwDeviceRegistry::instance().sharedToken(AV_HWDEVICE_TYPE_D3D11VA);
#else
    return {};
#endif
}

std::vector<std::string> hardwareBackendOrder() {
    std::vector<std::string> names;
    for (const BackendSpec* spec : effectiveOrder()) {
        names.emplace_back(spec->name);
    }
    return names;
}

void setHardwareBackendOrder(std::vector<std::string> order) {
    std::lock_guard lock(orderMutex());
    orderOverride() = std::move(order);
}

// ---- Impl
// ------------------------------------------------------------------------------------------------------

struct VideoDecoder::Impl {
    std::string path;
    DecoderOptions options;
    RawSourceSettings rawSettings; ///< 0.2: what the path asks of a RAW clip's development (RawSourceSettings.h)

    FormatContextPtr fmt;
    AVStream* stream = nullptr;
    int streamIndex = -1;
    const AVCodec* codec = nullptr;       ///< Native software decoder.
    const AVCodec* activeCodec = nullptr; ///< Decoder behind `ctx` (a wrapper such as h264_qsv for QSV).
    CodecContextPtr ctx;
    FramePtr frame{av_frame_alloc()};
    DecodeLoop loop;
    SwsContextPtr sws;

    // Hardware state.
    const BackendSpec* backend = nullptr; ///< nullptr = software.
    BufferRefPtr device;
    DeviceToken deviceToken; ///< Identity of `device` (stamped on frames read back from it).
    /// OutputMode::Native on the shared D3D11 device: frames are delivered as D3D11Texture payloads (zero-copy).
    bool nativeOutput = false;
    HwNegotiation negotiation;
    bool lastFrameWasHardware = false;
    /// CPU buffers for hardware transfers (av_buffer_pool_uninit on reset).
    AVBufferPool* transferPool = nullptr;
    int transferPoolSize = 0;

    // Timing.
    VideoStreamInfo info;
    Ticks origin{};
    Ticks nominalDuration{};
    Ticks firstFrameStart{};
    /// Half a tick of the stream's time base: how far a rounded timestamp can sit after the time it stands for
    /// (DecoderOptions::snapToStreamTicks; else 0).
    Ticks halfStreamTick{};
    Ticks lastEnd{};

    // Stamped on every frame.
    Orientation orientation;
    SourceIdentity source;

    // Decode position.
    std::optional<DecodedFrame> current; ///< Last frame stepped over, in presentation order.
    std::optional<DecodedFrame> pending; ///< Next frame (decoded, not yet stepped over).

    VideoDecoderStats stats;
    bool demuxerUsed = false; ///< Packets were read since openDemuxer() (a retry needs a fresh demuxer).

    ~Impl() { resetTransferPool(); }

    void resetTransferPool() {
        if (transferPool) {
            av_buffer_pool_uninit(&transferPool); // outstanding buffers keep the pool alive until released
        }
        transferPoolSize = 0;
    }

    bool hardwareActive() const { return backend != nullptr; }

    // -- Opening --

    std::expected<void, Error> openDemuxer() {
        auto opened = openInput(path);
        if (!opened) {
            return std::unexpected(opened.error());
        }
        fmt = std::move(*opened);
        auto selected = selectStream(fmt.get(), AVMEDIA_TYPE_VIDEO, streamIndex);
        if (!selected) {
            return std::unexpected(selected.error());
        }
        stream = *selected;
        streamIndex = stream->index;
        demuxerUsed = false;
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            fmt->streams[i]->discard = static_cast<int>(i) == streamIndex ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
        }
        resetDecodeState();
        return {};
    }

    std::expected<CodecContextPtr, Error> openCodec(const AVCodec* c, AVBufferRef* hwDevice) {
        CodecContextPtr context(avcodec_alloc_context3(c));
        if (!context) {
            return std::unexpected(Error{ErrorCode::DecoderError, "Out of memory"});
        }
        int r = avcodec_parameters_to_context(context.get(), stream->codecpar);
        if (r < 0) {
            return std::unexpected(ffmpegError(ErrorCode::DecoderError, "Invalid codec parameters", r));
        }
        context->pkt_timebase = stream->time_base;
        if (hwDevice) {
            context->hw_device_ctx = av_buffer_ref(hwDevice);
            context->opaque = &negotiation;
            context->get_format = negotiateFormat;
            // Frame threads keep several frames in flight, so the GPU decodes while the CPU copies the
            // previous frame out (1080p60 H.264 on d3d11va: ~300 -> ~420 fps, random access ~120 -> ~75 ms).
            // Each thread pins one more surface in the decoder's pool, hence the modest default.
            context->thread_count = options.threadCount > 0 ? options.threadCount : kDefaultHardwareThreads;
            if (negotiation.shaderResource) {
                // Zero-copy: there is no readback to overlap, and one codec context keeps the shader-visible
                // frames context the only surface pool.
                context->thread_count = 1;
            }
            context->thread_type = FF_THREAD_FRAME;
            // Up to two decoded surfaces are held back for lazy readback (the frame on hand and the
            // lookahead).
            context->extra_hw_frames = 2;
        } else {
            context->thread_count = std::max(0, options.threadCount);
            context->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        }
        r = avcodec_open2(context.get(), c, nullptr);
        if (r < 0) {
            return std::unexpected(
                ffmpegError(ErrorCode::DecoderError, std::format("Cannot open the {} decoder", c->name), r));
        }
        return context;
    }

    /// Tries one hardware backend: codec support, device, open, and a first frame that really came from the
    /// GPU.
    std::expected<void, std::string> tryHardware(const BackendSpec& spec) {
        const AVCodec* hwCodec = codec;
        if (!spec.decoderSuffix.empty()) {
            const std::string name =
                std::string(avcodec_get_name(stream->codecpar->codec_id)) + std::string(spec.decoderSuffix);
            hwCodec = avcodec_find_decoder_by_name(name.c_str());
            if (!hwCodec) {
                return std::unexpected("no " + name + " decoder in this build");
            }
        }
        const AVPixelFormat hwFormat = hwPixelFormat(hwCodec, spec.type);
        if (hwFormat == AV_PIX_FMT_NONE) {
            return std::unexpected(std::format("{} has no {} support", hwCodec->name, spec.name));
        }
        const AVCodecParameters* par = stream->codecpar;
        const HwFormatKey formatKey{spec.type,   par->codec_id, par->profile,
                                    par->format, par->width,    par->height};
        HwDeviceRegistry& registry = HwDeviceRegistry::instance();
        if (registry.isRejected(formatKey)) {
            return std::unexpected("this profile/format was already refused by the hardware decoder");
        }
        DeviceToken token;
        bool sharedDevice = false;
        BufferRefPtr hwDevice = registry.acquire(spec.type, &token, &sharedDevice);
        if (!hwDevice) {
            return std::unexpected("device creation failed");
        }
        negotiation = HwNegotiation{hwFormat, false};
#if defined(_WIN32)
        // Zero-copy only on the renderer's device: a private device's surfaces could not be sampled.
        nativeOutput = options.outputMode == OutputMode::Native && spec.type == AV_HWDEVICE_TYPE_D3D11VA && sharedDevice;
        negotiation.shaderResource = nativeOutput;
        negotiation.extraSurfaces = std::max(0, options.nativeSurfaces);
#else
        (void)sharedDevice;
        nativeOutput = false;
#endif
        auto context = openCodec(hwCodec, hwDevice.get());
        if (!context) {
            return std::unexpected(context.error().message);
        }
        ctx = std::move(*context);
        activeCodec = hwCodec;
        backend = &spec;
        device = std::move(hwDevice);
        deviceToken = token;

        auto first = decodeNext();
        std::string reason;
        if (!first) {
            reason = first.error().message;
        } else if (!*first) {
            reason = "the stream produced no frames";
        } else if (!lastFrameWasHardware) {
            reason = negotiation.rejected ? "codec profile/format not supported by the hardware decoder"
                                          : "frames did not come from the hardware decoder";
        } else if (auto readback = ready(**first); !readback) {
            reason = readback.error().message; // the GPU -> CPU path must work too
        }
        if (!reason.empty()) {
            if (negotiation.rejected) {
                registry.markRejected(formatKey);
            }
            dropCodec();
            return std::unexpected(reason);
        }
        pending = std::move(**first);
        firstFrameStart = pending->frame.pts;
        return {};
    }

    std::expected<void, Error> openSoftware() {
        dropCodec();
        auto context = openCodec(codec, nullptr);
        if (!context) {
            return std::unexpected(context.error());
        }
        ctx = std::move(*context);
        activeCodec = codec;
        return {};
    }

    /// Wrapper decoders (QSV's h264_qsv & co.) keep frames queued inside the hardware session and pair them
    /// with the wrong timestamps after avcodec_flush_buffers(), which breaks frame accuracy after a seek.
    /// They get a fresh context instead; native decoders (software and hwaccels) flush correctly.
    bool needsFreshContextOnSeek() const { return backend && !backend->decoderSuffix.empty(); }

    void dropCodec() {
        ctx.reset();
        activeCodec = nullptr;
        backend = nullptr;
        device.reset();
        deviceToken = {};
        resetTransferPool();
        resetDecodeState();
    }

    // -- Decoding --

    void resetDecodeState() {
        if (ctx) {
            avcodec_flush_buffers(ctx.get());
        }
        loop.reset();
        current.reset();
        pending.reset();
        lastEnd = Ticks{};
    }

    /// Copies a hardware frame into pooled CPU memory (NV12/P010).
    std::expected<FramePtr, Error> transferToCpu(const AVFrame* hw) {
        if (!hw->hw_frames_ctx) {
            return std::unexpected(Error{ErrorCode::DeviceError, "Hardware frame without a frames context"});
        }
        const auto* framesCtx = reinterpret_cast<const AVHWFramesContext*>(hw->hw_frames_ctx->data);
        AVPixelFormat* formats = nullptr;
        int r = av_hwframe_transfer_get_formats(hw->hw_frames_ctx, AV_HWFRAME_TRANSFER_DIRECTION_FROM,
                                                &formats, 0);
        if (r < 0 || !formats || formats[0] == AV_PIX_FMT_NONE) {
            av_freep(&formats);
            return std::unexpected(ffmpegError(ErrorCode::DeviceError, "No transfer format", r));
        }
        const AVPixelFormat swFormat = formats[0];
        av_freep(&formats);

        constexpr int kAlign = 64;
        const int size = av_image_get_buffer_size(swFormat, framesCtx->width, framesCtx->height, kAlign);
        if (size <= 0) {
            return std::unexpected(ffmpegError(ErrorCode::DeviceError, "Bad transfer size", size));
        }
        if (!transferPool || transferPoolSize != size) {
            resetTransferPool();
            transferPool = av_buffer_pool_init(static_cast<std::size_t>(size), nullptr);
            transferPoolSize = size;
        }
        FramePtr sw(av_frame_alloc());
        if (!sw || !transferPool) {
            return std::unexpected(Error{ErrorCode::DeviceError, "Out of memory"});
        }
        sw->format = swFormat;
        sw->width = framesCtx->width;
        sw->height = framesCtx->height;
        sw->buf[0] = av_buffer_pool_get(transferPool);
        if (!sw->buf[0]) {
            return std::unexpected(Error{ErrorCode::DeviceError, "Out of memory"});
        }
        r = av_image_fill_arrays(sw->data, sw->linesize, sw->buf[0]->data, swFormat, sw->width, sw->height,
                                 kAlign);
        if (r < 0) {
            return std::unexpected(ffmpegError(ErrorCode::DeviceError, "Cannot lay out transfer buffer", r));
        }
        r = av_hwframe_transfer_data(sw.get(), hw, 0);
        if (r < 0) {
            return std::unexpected(
                ffmpegError(ErrorCode::DeviceError, "GPU -> CPU frame transfer failed", r));
        }
        r = av_frame_copy_props(sw.get(), hw);
        if (r < 0) {
            return std::unexpected(ffmpegError(ErrorCode::DeviceError, "Cannot copy frame properties", r));
        }
        sw->width = hw->width; // the pool buffer is sized for the aligned surface; expose the cropped picture
        sw->height = hw->height;
        return sw;
    }

    /// 0.2 camera RAW: develops a Bayer frame (RawDevelop.h) - ProRes RAW with the levels, white balance, matrix and
    /// gain it carries, CinemaDNG as FFmpeg hands it (linear, white balanced camera RGB) - and the item's RAW Source
    /// Settings: exposure, and white balance from the as-shot light (ProRes RAW's temperature, else 6500 K).
    std::expected<FramePtr, Error> developBayer(const AVFrame* src) {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(src->format));
        if (!desc || src->width < 2 || src->height < 2) {
            return std::unexpected(Error{ErrorCode::Unsupported, "Cannot develop this RAW frame"});
        }
        const std::string_view name = desc->name;
        RawDevelopParams params;
        params.pattern = name.find("bggr") != std::string_view::npos   ? BayerPattern::BGGR
                         : name.find("grbg") != std::string_view::npos ? BayerPattern::GRBG
                         : name.find("gbrg") != std::string_view::npos ? BayerPattern::GBRG
                                                                       : BayerPattern::RGGB;
        double shotKelvin = 6500.0;
        if (const AVFrameSideData* sd = av_frame_get_side_data(src, AV_FRAME_DATA_RAW_COLOR_PARAMS);
            sd && sd->size >= sizeof(AVRawColorParams)) {
            const auto* raw = reinterpret_cast<const AVRawColorParams*>(sd->data);
            const double black = raw->black_level.den > 0 ? av_q2d(raw->black_level) : 0.0;
            const double white = raw->white_level.den > 0 ? av_q2d(raw->white_level) : 1.0;
            if (white > black && black >= 0.0 && white <= 1.0) {
                params.black = black;
                params.white = white;
            }
            if (raw->wb_cct >= 1000 && raw->wb_cct <= 50000) {
                shotKelvin = raw->wb_cct;
            }
            if (raw->type == AV_RAW_COLOR_PARAMS_PRORES_RAW) {
                const AVProResRawColorParams& pr = raw->codec.prores_raw;
                const auto q = [](AVRational r, double fallback) {
                    const double v = r.den != 0 ? av_q2d(r) : fallback;
                    return std::isfinite(v) && v > 0.0 && v < 1000.0 ? v : fallback;
                };
                params.wbRed = q(pr.wb_red, 1.0);
                params.wbBlue = q(pr.wb_blue, 1.0);
                params.gain = q(pr.gain, 1.0);
                params.hasMatrix = true;
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 3; ++c) {
                        const AVRational e = pr.color_matrix[r][c];
                        params.cameraToXyz[static_cast<std::size_t>(r * 3 + c)] = e.den != 0 ? av_q2d(e) : (r == c ? 1.0 : 0.0);
                    }
                }
            }
        }
        params.exposureStops = rawSettings.exposure;
        if (rawSettings.white) {
            params.whiteAdaptation =
                whiteBalanceAdaptation(shotKelvin, rawSettings.white->temperature, rawSettings.white->tint);
        }
        // The samples as 16-bit values in host order.
        // Bayer descriptors split the bits among their pseudo components (16-bit: 4 / 8 / 4): ask for the sample size.
        const bool wide = av_get_bits_per_pixel(desc) > 8;
        const bool bigEndian = (desc->flags & AV_PIX_FMT_FLAG_BE) != 0;
        std::vector<std::uint16_t> samples;
        const std::uint16_t* cfa = nullptr;
        std::size_t stride = 0;
        if (wide && !bigEndian && src->linesize[0] % 2 == 0) {
            cfa = reinterpret_cast<const std::uint16_t*>(src->data[0]);
            stride = static_cast<std::size_t>(src->linesize[0]) / 2;
        } else {
            samples.resize(static_cast<std::size_t>(src->width) * src->height);
            for (int y = 0; y < src->height; ++y) {
                const std::uint8_t* row = src->data[0] + static_cast<std::ptrdiff_t>(y) * src->linesize[0];
                for (int x = 0; x < src->width; ++x) {
                    samples[static_cast<std::size_t>(y) * src->width + x] =
                        !wide       ? static_cast<std::uint16_t>(row[x])
                        : bigEndian ? static_cast<std::uint16_t>((row[2 * x] << 8) | row[2 * x + 1])
                                    : static_cast<std::uint16_t>(row[2 * x] | (row[2 * x + 1] << 8));
                }
            }
            cfa = samples.data();
            stride = static_cast<std::size_t>(src->width);
        }
        FramePtr dst(av_frame_alloc());
        dst->format = AV_PIX_FMT_YUV444P10LE;
        dst->width = src->width;
        dst->height = src->height;
        int r = av_frame_get_buffer(dst.get(), 0);
        if (r < 0) {
            return std::unexpected(ffmpegError(ErrorCode::DecoderError, "Out of memory", r));
        }
        developRaw(cfa, stride, src->width, src->height, wide ? 65535 : 255, params,
                   reinterpret_cast<std::uint16_t*>(dst->data[0]), reinterpret_cast<std::uint16_t*>(dst->data[1]),
                   reinterpret_cast<std::uint16_t*>(dst->data[2]), static_cast<std::size_t>(dst->linesize[0]) / 2);
        av_frame_copy_props(dst.get(), src);
        dst->colorspace = AVCOL_SPC_BT709;
        dst->color_range = AVCOL_RANGE_MPEG;
        dst->color_primaries = AVCOL_PRI_BT709;
        dst->color_trc = AVCOL_TRC_BT709;
        return dst;
    }

    std::expected<FramePtr, Error> convert(const AVFrame* src, PixelFormat target) {
        if (isBayer(src->format)) {
            return developBayer(src); // 0.2 camera RAW
        }
        const AVPixelFormat dstFormat = toAvPixelFormat(target);
        if (dstFormat == AV_PIX_FMT_NONE) { // swscale asserts on unknown formats
            return std::unexpected(Error{ErrorCode::Unsupported, "No conversion target"});
        }
        sws.reset(sws_getCachedContext(sws.release(), src->width, src->height,
                                       static_cast<AVPixelFormat>(src->format), src->width, src->height,
                                       dstFormat, SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT,
                                       nullptr, nullptr, nullptr));
        if (!sws) {
            return std::unexpected(
                Error{ErrorCode::Unsupported,
                      std::format("Cannot convert {} frames",
                                  av_get_pix_fmt_name(static_cast<AVPixelFormat>(src->format)))});
        }
        FramePtr dst(av_frame_alloc());
        dst->format = dstFormat;
        dst->width = src->width;
        dst->height = src->height;
        int r = av_frame_get_buffer(dst.get(), 0);
        if (r < 0) {
            return std::unexpected(ffmpegError(ErrorCode::DecoderError, "Out of memory", r));
        }
        r = sws_scale(sws.get(), src->data, src->linesize, 0, src->height, dst->data, dst->linesize);
        if (r < 0) {
            return std::unexpected(ffmpegError(ErrorCode::DecoderError, "Pixel format conversion failed", r));
        }
        av_frame_copy_props(dst.get(), src);
        return dst;
    }

    /// Turns the codec's current output (`frame`) into a DecodedFrame.
    std::expected<DecodedFrame, Error> takeFrame() {
        FramePtr src(av_frame_alloc());
        av_frame_move_ref(src.get(), frame.get());

        lastFrameWasHardware = hardwareActive() && src->format == negotiation.hwFormat;
        if (lastFrameWasHardware && !lazyReadback()) {
            auto cpu = transferToCpu(src.get());
            if (!cpu) {
                return std::unexpected(cpu.error());
            }
            src = std::move(*cpu);
            ++stats.framesReadBack;
        }

        DecodedFrame out;
        const std::int64_t ts =
            src->best_effort_timestamp != AV_NOPTS_VALUE ? src->best_effort_timestamp : src->pts;
        // Checked conversions: a timestamp outside the representable range is an error, never a wrapped
        // value.
        Ticks start = lastEnd;
        if (ts != AV_NOPTS_VALUE) {
            const auto absolute = toTicks(ts, stream->time_base);
            const auto relative = absolute ? checkedSub(*absolute, origin) : std::nullopt;
            if (!relative) {
                return std::unexpected(Error{ErrorCode::DecoderError, "Frame timestamp out of range"});
            }
            start = *relative;
        }
        // The packet's duration until the next frame is known; deliver() replaces it with the distance to the
        // next presentation timestamp.
        Ticks duration = nominalDuration;
        if (ts != AV_NOPTS_VALUE && src->duration > 0) {
            const auto endStamp = checkedAdd(ts, src->duration);
            const auto end = endStamp ? toTicks(*endStamp, stream->time_base) : std::nullopt;
            const auto relativeEnd = end ? checkedSub(*end, origin) : std::nullopt;
            if (relativeEnd && *relativeEnd > start) {
                duration = *relativeEnd - start;
            }
        }
        lastEnd = checkedAdd(start, duration).value_or(start);

        std::int64_t streamPts = ts;
        if (ts == AV_NOPTS_VALUE) {
            const auto absolute = checkedAdd(start, origin);
            const auto stamp = absolute ? toStreamTime(*absolute, stream->time_base) : std::nullopt;
            if (!stamp) {
                return std::unexpected(Error{ErrorCode::DecoderError, "Frame timestamp out of range"});
            }
            streamPts = *stamp;
        }

        // A surface still on the GPU is described by its frames context's software format.
        const bool onGpu = src->hw_frames_ctx && src->format == negotiation.hwFormat;
        const int pixelFormat =
            onGpu ? reinterpret_cast<const AVHWFramesContext*>(src->hw_frames_ctx->data)->sw_format
                  : src->format;
        FormatChoice choice;
        bool needsConversion = false;
        if (auto native = nativeFormat(pixelFormat)) {
            choice = *native;
        } else {
            choice.format = isBayer(pixelFormat) ? PixelFormat::YUV444P10 : conversionTarget(pixelFormat); // 0.2 RAW
            if (choice.format == PixelFormat::Unknown) {
                return std::unexpected(Error{ErrorCode::Unsupported, "Unknown pixel format"});
            }
            choice.fullRange = src->color_range == AVCOL_RANGE_JPEG;
            // Converted on delivery (ready()); the metadata below is the same before and after the conversion
            // (same size, av_frame_copy_props).
            needsConversion = !onGpu;
        }
        out.fullRange = choice.fullRange;

        FrameHandle& v = out.frame;
        v.pts = start;
        v.duration = duration;
        v.streamPts = streamPts;
        v.timeBase = TimeBase{stream->time_base.num, stream->time_base.den};
        v.width = src->width;
        v.height = src->height;
        v.format = choice.format;
        v.color = resolveColor(colorInfoFrom(src->colorspace, src->color_range, src->color_primaries,
                                             src->color_trc, src->chroma_location),
                               choice.format, src->width, src->height, choice.fullRange);
        if (isBayer(pixelFormat)) { // 0.2: what developBayer makes of it
            v.color = ColorInfo{ColorMatrix::BT709, ColorRange::Limited, ColorPrimaries::BT709, TransferFunction::BT709,
                                ChromaLocation::Left};
            out.fullRange = false;
        }
        v.alpha = !pixelFormatHasAlpha(choice.format) ? AlphaMode::None
                  : src->alpha_mode == AVALPHA_MODE_PREMULTIPLIED
                      ? AlphaMode::Premultiplied
                      : AlphaMode::Straight; // unspecified: straight
        v.sampleAspect = src->sample_aspect_ratio.num > 0 && src->sample_aspect_ratio.den > 0
                             ? Rate{src->sample_aspect_ratio.num, src->sample_aspect_ratio.den}
                             : info.sampleAspect;
        v.orientation = orientation;
        v.source = source;
        if (onGpu) {
            out.hw = std::move(src); // read back on delivery, see ready()
            return out;
        }
        out.rawDevice = lastFrameWasHardware ? deviceToken : DeviceToken{};
        out.convertOnReady = needsConversion;
        out.raw = std::move(src); // wrapped (and converted) on delivery, see ready()
        return out;
    }

    /// Hwaccel surfaces (native decoders) are read back lazily; wrapper decoders (QSV) recycle their surfaces
    /// internally and are copied at once.
    bool lazyReadback() const { return backend && backend->decoderSuffix.empty(); }

    /// Completes `d` for delivery: reads a pending GPU surface back into CPU planes, or (zero-copy) hands the
    /// surface itself out as a D3D11Texture payload; a software frame is wrapped as CPU planes (converted first when
    /// its format needs it). Once done, `d` keeps the payload, so delivering it again costs nothing.
    std::expected<FrameHandle, Error> ready(DecodedFrame& d) {
#if defined(_WIN32)
        if (d.hw && nativeOutput && d.hw->format == AV_PIX_FMT_D3D11 && d.hw->hw_frames_ctx) {
            const auto* frames = reinterpret_cast<const AVHWFramesContext*>(d.hw->hw_frames_ctx->data);
            const auto* d3d = static_cast<const AVD3D11VAFramesContext*>(frames->hwctx);
            if ((d3d->BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0) {
                D3D11Texture native;
                native.texture = d.hw->data[0];
                native.arraySlice = static_cast<unsigned>(reinterpret_cast<std::intptr_t>(d.hw->data[1]));
                native.device = deviceToken;
                native.owner = shareFrame(std::move(d.hw)); // pins the surface until the last handle is gone
                d.frame.payload = std::move(native);
                return d.frame;
            }
        }
#endif
        if (d.hw) {
            auto cpuFrame = transferToCpu(d.hw.get());
            if (!cpuFrame) {
                return std::unexpected(cpuFrame.error());
            }
            ++stats.framesReadBack;
            FramePtr sw = std::move(*cpuFrame);
            if (!nativeFormat(sw->format)) { // e.g. P016/P012 surfaces from 12-bit streams
                auto converted = convert(sw.get(), d.frame.format);
                if (!converted) {
                    return std::unexpected(converted.error());
                }
                ++stats.framesConverted;
                sw = std::move(*converted);
            }
            CpuPlanes cpu = cpuPlanesOf(d.frame.format, sw.get());
            cpu.device = deviceToken;
            cpu.owner = shareFrame(std::move(sw));
            d.frame.payload = std::move(cpu);
            d.hw.reset();
        }
        if (d.raw) {
            FramePtr converted;
            if (d.convertOnReady) {
                auto made = convert(d.raw.get(), d.frame.format);
                if (!made) {
                    return std::unexpected(made.error()); // `raw` stays: a later call may try again
                }
                ++stats.framesConverted;
                converted = std::move(*made);
            }
            FramePtr planes = converted ? std::move(converted) : std::move(d.raw);
            CpuPlanes cpu = cpuPlanesOf(d.frame.format, planes.get());
            cpu.device = d.rawDevice;
            cpu.owner = shareFrame(std::move(planes));
            d.frame.payload = std::move(cpu);
            d.raw.reset();
        }
        return d.frame;
    }

    std::expected<std::optional<FrameHandle>, Error> deliver(DecodedFrame& d) {
        // Display duration = distance to the next presentation timestamp. Without a next frame (end of
        // stream) the last frame keeps its container duration, or the nominal frame period.
        if (pending && &d != &*pending && pending->frame.pts > d.frame.pts) {
            d.frame.duration = pending->frame.pts - d.frame.pts;
        }
        auto f = ready(d);
        if (!f) {
            return std::unexpected(f.error());
        }
        return std::optional<FrameHandle>(std::move(*f));
    }

    /// Next frame in presentation order; nullopt once the stream is fully drained.
    std::expected<std::optional<DecodedFrame>, Error> decodeNext() {
        demuxerUsed = true;
        auto got = loop.receive(fmt.get(), ctx.get(), streamIndex, frame.get(), path);
        if (!got) {
            return std::unexpected(got.error());
        }
        if (!*got) {
            return std::nullopt;
        }
        ++stats.framesDecoded;
        auto taken = takeFrame();
        if (!taken) {
            return std::unexpected(taken.error());
        }
        return std::optional<DecodedFrame>(std::move(*taken));
    }

    // -- Positioning --

    std::expected<void, Error> seekTo(Ticks target) {
        const auto absolute = checkedAdd(target, origin);
        const auto stamp = absolute ? toStreamTime(*absolute, stream->time_base) : std::nullopt;
        if (!stamp) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "Seek target out of range"});
        }
        const std::int64_t ts = *stamp;
        int r = av_seek_frame(fmt.get(), streamIndex, ts, AVSEEK_FLAG_BACKWARD);
        if (r < 0) {
            r = avformat_seek_file(fmt.get(), streamIndex, INT64_MIN, ts, ts, 0);
        }
        if (r < 0) {
            return std::unexpected(ffmpegError(ErrorCode::IoError, "Seeking in " + path + " failed", r));
        }
        ++stats.seeks;
        if (needsFreshContextOnSeek()) {
            auto context = openCodec(activeCodec, device.get());
            if (!context) {
                return std::unexpected(context.error());
            }
            ctx = std::move(*context); // replaces the flush in resetDecodeState()
        }
        resetDecodeState();
        return {};
    }

    bool covers(const DecodedFrame& f, Ticks t) const {
        if (t < f.frame.pts) {
            return false;
        }
        return pending ? t < pending->frame.pts
                       : t < f.frame.pts + f.frame.duration; // no next frame: stream end
    }

    /// Whether decoding forward from the current position is cheaper than seeking.
    bool shouldDecodeForward(Ticks t) {
        const DecodedFrame* reference = pending ? &*pending : (current ? &*current : nullptr);
        if (!reference || (current && t < current->frame.pts) || (!current && t < reference->frame.pts)) {
            return false;
        }
        const Ticks distance = t - reference->frame.pts;
        if (distance.value <= nominalDuration.value * 8) {
            return true;
        }
        const auto absolute = checkedAdd(t, origin);
        const auto stamp = absolute ? toStreamTime(*absolute, stream->time_base) : std::nullopt;
        if (stamp && avformat_index_get_entries_count(stream) > 0 && loop.lastPacketDts() != AV_NOPTS_VALUE) {
            // Seek only if a keyframe lies between what the decoder has already read and the target.
            const AVIndexEntry* key =
                avformat_index_get_entry_from_timestamp(stream, *stamp, AVSEEK_FLAG_BACKWARD);
            return !key || key->timestamp <= loop.lastPacketDts();
        }
        return distance < Ticks::fromSeconds(1);
    }

    /// Steps forward to the frame containing `t`. nullopt: the next available frame already starts after `t`
    /// (the caller has to start earlier).
    /// Steps forward to the frame containing `t`. A frame is only delivered once the next one is decoded (or
    /// the stream has ended), so its duration is exactly the time until the next presentation timestamp.
    /// nullopt: the next available frame already starts after `t` (the caller has to start earlier).
    std::expected<std::optional<FrameHandle>, Error> decodeForward(Ticks t) {
        for (;;) {
            if (!pending) {
                auto next = decodeNext();
                if (!next) {
                    return std::unexpected(next.error());
                }
                if (*next) {
                    pending = std::move(**next);
                }
            }
            if (pending && pending->frame.pts <= t) {
                current = std::move(pending);
                pending.reset();
                continue;
            }
            // `pending` (if any) starts after `t`; `current` is the candidate.
            if (!current || current->frame.pts > t) {
                return std::optional<FrameHandle>();
            }
            if (!pending && t >= current->frame.pts + current->frame.duration) {
                return std::unexpected(Error{ErrorCode::NotFound, "Time is past the end of the video"});
            }
            return deliver(*current);
        }
    }

    std::expected<FrameHandle, Error> seekAndDecode(Ticks t) {
        Ticks seekTarget = t;
        Ticks backoff = std::max(Ticks{nominalDuration.value * 2}, Ticks{kTicksPerSecond / 10});
        for (int attempt = 0; attempt < 16; ++attempt) {
            if (auto r = seekTo(seekTarget); !r) {
                return std::unexpected(r.error());
            }
            auto found = decodeForward(t);
            if (!found) {
                return std::unexpected(found.error());
            }
            if (*found) {
                return std::move(**found);
            }
            // The keyframe we landed on is presented after `t` (e.g. B-frame reordering or an index keyed by
            // DTS).
            if (!pending || seekTarget <= firstFrameStart - Ticks::fromSeconds(1)) {
                // Already seeked to the very beginning: nothing precedes this frame, so it is the answer.
                if (pending) {
                    auto first = decodeForward(pending->frame.pts);
                    if (!first) {
                        return std::unexpected(first.error());
                    }
                    if (*first) {
                        return std::move(**first);
                    }
                }
                return std::unexpected(Error{ErrorCode::NotFound, "No frame found"});
            }
            seekTarget = std::min(seekTarget, pending->frame.pts) - backoff;
            backoff = Ticks{backoff.value * 2};
        }
        return std::unexpected(
            Error{ErrorCode::DecoderError, "Could not find a keyframe before the requested time"});
    }

    std::expected<FrameHandle, Error> locate(Ticks t) {
        if (current && covers(*current, t)) {
            return ready(*current);
        }
        if (shouldDecodeForward(t)) {
            auto found = decodeForward(t);
            if (!found) {
                return std::unexpected(found.error());
            }
            if (*found) {
                return std::move(**found);
            }
        }
        return seekAndDecode(t);
    }

    std::expected<FrameHandle, Error> frameAt(Ticks t) {
        auto result = locateOrFallBack(t);
        if (result) {
            ++stats.framesDelivered;
        }
        return result;
    }

    std::expected<FrameHandle, Error> locateOrFallBack(Ticks t) {
        if (t.value < 0) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "Negative source time"});
        }
        // Sequence conform (DecoderOptions::snapToStreamTicks): container timestamps are rounded to the stream's time
        // base, so a frame meant for `t` can be stamped up to half a tick after it (a 144 fps capture in a 1/15360 MP4
        // timescale, OBS's millisecond MKV timestamps): such a frame is the one on screen at `t` (I.9, gaming).
        t = std::max(t + halfStreamTick, firstFrameStart);
        auto result = locate(t);
        if (result || !hardwareActive() || result.error().code == ErrorCode::NotFound) {
            return result;
        }
        // The GPU decoder failed mid-stream (device loss, a profile change, ...): continue in software.
        log(LogLevel::Warning, "{}: {} decoding failed ({}); switching to software", path, backend->name,
            result.error().message);
        ++stats.hardwareFallbacks;
        if (auto sw = openSoftware(); !sw) {
            return std::unexpected(sw.error());
        }
        return seekAndDecode(t);
    }
};

// ---- VideoDecoder
// ------------------------------------------------------------------------------------------------

VideoDecoder::VideoDecoder(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
VideoDecoder::~VideoDecoder() = default;

std::expected<std::unique_ptr<VideoDecoder>, Error>
VideoDecoder::open(const std::string& path, int streamIndex, const DecoderOptions& options) {
    if (!isKnownBackendName(options.preferredBackend)) {
        return std::unexpected(
            Error{ErrorCode::InvalidArgument, "Unknown decode backend '" + options.preferredBackend + "'"});
    }
    auto impl = std::make_unique<Impl>();
    impl->path = path;
    impl->options = options;
    impl->rawSettings = rawSourceSettingsOf(path);
    impl->streamIndex = streamIndex;
    if (!impl->frame || !impl->loop.isValid()) {
        return std::unexpected(Error{ErrorCode::DecoderError, "Out of memory"});
    }
    if (auto r = impl->openDemuxer(); !r) {
        return std::unexpected(r.error());
    }
    impl->codec = avcodec_find_decoder(impl->stream->codecpar->codec_id);
    if (!impl->codec) {
        return std::unexpected(
            Error{ErrorCode::Unsupported,
                  std::format("No decoder for {}", avcodec_get_name(impl->stream->codecpar->codec_id))});
    }
    impl->info = describeVideoStream(impl->fmt.get(), impl->stream);
    impl->origin = mediaOrigin(impl->fmt.get());
    impl->orientation = orientationOf(impl->stream->codecpar);
    if (auto fingerprint = sourceFingerprint(path)) {
        impl->source = SourceIdentity{*fingerprint, options.sourceGeneration};
    } else {
        log(LogLevel::Warning, "{}: no source fingerprint ({})", path, fingerprint.error().message);
        impl->source = SourceIdentity{0, options.sourceGeneration};
    }
    impl->nominalDuration = impl->info.frameRate.isValid()
                                ? Ticks{kTicksPerSecond * impl->info.frameRate.den / impl->info.frameRate.num}
                                : Ticks{kTicksPerSecond / 30};
    if (options.snapToStreamTicks && impl->stream && impl->stream->time_base.num > 0 && impl->stream->time_base.den > 0) {
        impl->halfStreamTick = Ticks{kTicksPerSecond * impl->stream->time_base.num / impl->stream->time_base.den / 2};
    }

    // Hardware candidates: the preferred backend first, then the effective order (see
    // hardwareBackendOrder()).
    std::vector<const BackendSpec*> candidates;
    if (options.allowHardware && options.preferredBackend != kSoftware) {
        const auto& all = platformBackends();
        const auto preferred = std::find_if(
            all.begin(), all.end(), [&](const BackendSpec& s) { return s.name == options.preferredBackend; });
        if (preferred != all.end()) {
            candidates.push_back(&*preferred);
        } else if (!options.preferredBackend.empty()) {
            log(LogLevel::Info, "Decode backend '{}' is not available on this platform",
                options.preferredBackend);
        }
#if defined(_WIN32)
        // Zero-copy needs d3d11va on the shared device; without a readback the readback-throughput policy
        // does not apply.
        if (options.outputMode == OutputMode::Native && candidates.empty() &&
            !HwDeviceRegistry::instance().sharedToken(AV_HWDEVICE_TYPE_D3D11VA).isCpu()) {
            const auto d3d11 = std::find_if(all.begin(), all.end(),
                                            [](const BackendSpec& s) { return s.type == AV_HWDEVICE_TYPE_D3D11VA; });
            if (d3d11 != all.end()) {
                candidates.push_back(&*d3d11);
            }
        }
#endif
        for (const BackendSpec* spec : effectiveOrder()) {
            if (std::find(candidates.begin(), candidates.end(), spec) == candidates.end()) {
                candidates.push_back(spec);
            }
        }
    }

    bool dirty = false; // the demuxer has been read by a failed attempt
    for (const BackendSpec* spec : candidates) {
        if (dirty) {
            if (auto r = impl->openDemuxer(); !r) {
                return std::unexpected(r.error());
            }
            dirty = false;
        }
        auto attempt = impl->tryHardware(*spec);
        if (attempt) {
            log(LogLevel::Debug, "{}: decoding {} with {}", path, impl->info.codec, spec->name);
            break;
        }
        dirty = impl->demuxerUsed;
        ++impl->stats.hardwareFallbacks;
        log(LogLevel::Info, "{}: {} not used for {} ({})", path, spec->name, impl->info.codec,
            attempt.error());
    }

    if (!impl->hardwareActive()) {
        if (dirty) {
            if (auto r = impl->openDemuxer(); !r) {
                return std::unexpected(r.error());
            }
        }
        if (auto r = impl->openSoftware(); !r) {
            return std::unexpected(r.error());
        }
        auto first = impl->decodeNext();
        if (!first) {
            return std::unexpected(first.error());
        }
        if (!*first) {
            return std::unexpected(
                Error{ErrorCode::DecoderError, "The video stream contains no decodable frames"});
        }
        impl->pending = std::move(**first);
        impl->firstFrameStart = impl->pending->frame.pts;
    }
    return std::unique_ptr<VideoDecoder>(new VideoDecoder(std::move(impl)));
}

std::expected<FrameHandle, Error> VideoDecoder::frameAt(Ticks sourceTime) {
    return m_impl->frameAt(sourceTime);
}

std::string_view VideoDecoder::backendName() const {
    return m_impl->backend ? m_impl->backend->name : kSoftware;
}

bool VideoDecoder::isHardwareAccelerated() const {
    return m_impl->hardwareActive();
}

const VideoStreamInfo& VideoDecoder::streamInfo() const {
    return m_impl->info;
}

Ticks VideoDecoder::firstFrameTime() const {
    return m_impl->firstFrameStart;
}

SourceIdentity VideoDecoder::source() const {
    return m_impl->source;
}

VideoDecoderStats VideoDecoder::stats() const {
    return m_impl->stats;
}

} // namespace wabcut::media
