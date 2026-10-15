#include "Encode.h"

#include <wabcut/export/Encoders.h>
#include <wabcut/model/MediaLocator.h>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace wabcut::exporter {

std::int32_t quantizePcmSample(float x, int bits) {
    const double scale = bits >= 24 ? 8'388'608.0 : 32'768.0; // 2^23 / 2^15
    const double lo = -scale;
    const double hi = scale - 1.0;
    double v = static_cast<double>(x) * scale;
    if (!(v == v)) { // NaN
        v = 0.0;
    }
    v = std::clamp(v, lo, hi);
    return static_cast<std::int32_t>(std::lrint(v));
}

namespace detail {

namespace {

// ---- Small helpers ----------------------------------------------------------------------------------------

AVPixelFormat toAvPixelFormat(media::PixelFormat f) {
    using media::PixelFormat;
    switch (f) {
    case PixelFormat::NV12:
        return AV_PIX_FMT_NV12;
    case PixelFormat::P010:
        return AV_PIX_FMT_P010LE;
    case PixelFormat::YUV420P:
        return AV_PIX_FMT_YUV420P;
    case PixelFormat::YUV420P10:
        return AV_PIX_FMT_YUV420P10LE;
    case PixelFormat::YUV422P:
        return AV_PIX_FMT_YUV422P;
    case PixelFormat::YUV422P10:
        return AV_PIX_FMT_YUV422P10LE;
    case PixelFormat::YUV444P:
        return AV_PIX_FMT_YUV444P;
    case PixelFormat::YUV444P10:
        return AV_PIX_FMT_YUV444P10LE;
    case PixelFormat::RGBA8:
        return AV_PIX_FMT_RGBA;
    case PixelFormat::YUVA420P:
        return AV_PIX_FMT_YUVA420P;
    case PixelFormat::YUVA444P10:
        return AV_PIX_FMT_YUVA444P10LE;
    case PixelFormat::Unknown:
        break;
    }
    return AV_PIX_FMT_NONE;
}

AVColorSpace toAvColorSpace(ColorMatrix m) {
    switch (m) {
    case ColorMatrix::BT601:
        return AVCOL_SPC_SMPTE170M;
    case ColorMatrix::BT709:
        return AVCOL_SPC_BT709;
    case ColorMatrix::BT2020NonConstant:
        return AVCOL_SPC_BT2020_NCL;
    case ColorMatrix::BT2020Constant:
        return AVCOL_SPC_BT2020_CL;
    case ColorMatrix::SMPTE240M:
        return AVCOL_SPC_SMPTE240M;
    case ColorMatrix::RGB:
        return AVCOL_SPC_RGB;
    case ColorMatrix::Unknown:
        break;
    }
    return AVCOL_SPC_BT709;
}

/// The export's output colour: BT.709, limited range, left-sited chroma. Primaries and transfer are the same
/// on both sides of every conversion, so swscale only applies the matrix (no gamut or transfer mapping).
void tagOutput(AVFrame* f) {
    f->colorspace = AVCOL_SPC_BT709;
    f->color_range = AVCOL_RANGE_MPEG;
    f->color_primaries = AVCOL_PRI_BT709;
    f->color_trc = AVCOL_TRC_BT709;
    f->chroma_location = AVCHROMA_LOC_LEFT;
}

std::expected<FramePtr, Error> allocVideoFrame(AVPixelFormat format, int width, int height) {
    FramePtr f(av_frame_alloc());
    if (!f) {
        return std::unexpected(Error{ErrorCode::EncoderError, "Out of memory (frame)"});
    }
    f->format = format;
    f->width = width;
    f->height = height;
    if (const int r = av_frame_get_buffer(f.get(), 0); r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::EncoderError, "Cannot allocate a video frame", r));
    }
    tagOutput(f.get());
    return f;
}

AVCodecID codecIdOf(VideoCodec c) {
    switch (c) {
    case VideoCodec::H264:
        return AV_CODEC_ID_H264;
    case VideoCodec::Hevc:
        return AV_CODEC_ID_HEVC;
    case VideoCodec::ProRes:
        return AV_CODEC_ID_PRORES;
    case VideoCodec::DnxHR:
        return AV_CODEC_ID_DNXHD;
    case VideoCodec::Av1:
        return AV_CODEC_ID_AV1;
    case VideoCodec::Gif:
        return AV_CODEC_ID_GIF;
    case VideoCodec::Vp9:
        return AV_CODEC_ID_VP9;
    }
    return AV_CODEC_ID_H264;
}

/// CPU layouts for an output format, preferred first (semi-planar first: hardware encoders take it natively).
std::vector<AVPixelFormat> inputFormatsFor(OutputPixelFormat f) {
    switch (f) {
    case OutputPixelFormat::Yuv420p:
        return {AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P};
    case OutputPixelFormat::Yuv420p10:
        return {AV_PIX_FMT_P010LE, AV_PIX_FMT_YUV420P10LE};
    case OutputPixelFormat::Yuv422p:
        return {AV_PIX_FMT_NV16, AV_PIX_FMT_YUV422P};
    case OutputPixelFormat::Yuv422p10:
        return {AV_PIX_FMT_P210LE, AV_PIX_FMT_YUV422P10LE};
    case OutputPixelFormat::Pal8:
        return {AV_PIX_FMT_PAL8};
    }
    return {AV_PIX_FMT_YUV420P};
}

std::string_view outputFormatName(OutputPixelFormat f) {
    switch (f) {
    case OutputPixelFormat::Yuv420p:
        return "yuv420p";
    case OutputPixelFormat::Yuv420p10:
        return "yuv420p10";
    case OutputPixelFormat::Yuv422p:
        return "yuv422p";
    case OutputPixelFormat::Yuv422p10:
        return "yuv422p10";
    case OutputPixelFormat::Pal8:
        return "pal8";
    }
    return "yuv420p";
}

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.ends_with(suffix);
}

void setOption(AVDictionary** opts, const char* key, std::string_view value) {
    av_dict_set(opts, key, std::string(value).c_str(), 0);
}
void setOption(AVDictionary** opts, const char* key, std::int64_t value) {
    av_dict_set_int(opts, key, value, 0);
}

struct DictionaryOwner {
    AVDictionary* dict = nullptr;
    ~DictionaryOwner() { av_dict_free(&dict); }
};

/// Encoder-specific rate control, preset and profile options.
void configureVideoOptions(const std::string& name, const VideoSettings& v, AVCodecContext* ctx,
                           AVDictionary** opts) {
    const bool quality = v.rateControl == RateControl::Quality;
    const std::int64_t maxRate = v.maxBitrate > 0 ? v.maxBitrate : v.bitrate * 3 / 2;
    auto setBitrate = [&] {
        ctx->bit_rate = v.bitrate;
        ctx->rc_max_rate = maxRate;
        ctx->rc_buffer_size = static_cast<int>(std::min<std::int64_t>(2 * maxRate, INT32_MAX));
    };

    if (name == "libx264" || name == "libx265") {
        setOption(opts, "preset", "medium");
        if (quality) {
            setOption(opts, "crf", v.quality);
        } else {
            setBitrate();
        }
        if (name == "libx265") {
            setOption(opts, "x265-params", "log-level=error"); // x265 logs to stderr by itself
        }
    } else if (endsWith(name, "_nvenc")) {
        setOption(opts, "preset", "p4"); // NVIDIA's "medium": ~1.7x the speed of p5 at 1080p on an RTX 3060
        setOption(opts, "tune", "hq");
        setOption(opts, "rc", "vbr");
        if (quality) {
            ctx->bit_rate = 0;
            setOption(opts, "cq", v.quality); // NVENC CQ, same 0..51 scale as CRF
        } else {
            setBitrate();
        }
    } else if (endsWith(name, "_amf")) {
        setOption(opts, "quality", "quality");
        if (quality) {
            setOption(opts, "rc", "cqp");
            setOption(opts, "qp_i", v.quality);
            setOption(opts, "qp_p", v.quality + 2);
            setOption(opts, "qp_b", v.quality + 4);
        } else {
            setOption(opts, "rc", "vbr_peak");
            setBitrate();
        }
    } else if (endsWith(name, "_qsv")) {
        setOption(opts, "preset", "slower");
        if (quality) {
            ctx->global_quality = std::max(1, v.quality); // ICQ, 1..51
        } else {
            setBitrate();
        }
    } else if (endsWith(name, "_videotoolbox")) {
        if (quality) {
            // VideoToolbox constant quality is 1..100, higher is better.
            ctx->flags |= AV_CODEC_FLAG_QSCALE;
            ctx->global_quality = FF_QP2LAMBDA * std::clamp(100 - 2 * v.quality, 1, 100);
        } else {
            setBitrate();
        }
    } else if (endsWith(name, "_vaapi")) {
        if (quality) {
            setOption(opts, "rc_mode", "CQP");
            ctx->global_quality = v.quality;
        } else {
            setOption(opts, "rc_mode", "VBR");
            setBitrate();
        }
    } else if (name == "libsvtav1") { // I.9
        setOption(opts, "preset", 8);   // SVT-AV1 presets 0 (slowest) .. 13; 8 keeps 1080p exports near real time
        if (quality) {
            setOption(opts, "crf", std::clamp(v.quality * 63 / 51, 1, 63)); // the 0..51 scale onto SVT-AV1's 1..63
        } else {
            setBitrate();
        }
    } else if (name == "libvpx-vp9") { // I.9 (gaming): WebM
        setOption(opts, "deadline", "good");
        setOption(opts, "cpu-used", 4); // libvpx's speed 0 (slowest) .. 8; 4 keeps 720p near real time
        setOption(opts, "row-mt", 1);
        if (quality) {
            ctx->bit_rate = 0; // constant quality
            setOption(opts, "crf", std::clamp(v.quality * 63 / 51, 0, 63)); // the 0..51 scale onto libvpx's 0..63
        } else {
            setBitrate();
        }
    } else if (name == "prores_ks") {
        constexpr std::string_view kProfiles[] = {"proxy", "lt", "standard", "hq"};
        setOption(opts, "profile", kProfiles[static_cast<int>(v.proresProfile)]);
        setOption(opts, "vendor", "apl0");
    } else if (name == "dnxhd") {
        constexpr std::string_view kProfiles[] = {"dnxhr_lb", "dnxhr_sq", "dnxhr_hq", "dnxhr_hqx"};
        setOption(opts, "profile", kProfiles[static_cast<int>(v.dnxhrProfile)]);
    }
}

/// Encodes one black frame and drains: proves a (hardware) encoder really works with these parameters.
std::expected<void, Error> testEncode(const VideoEncoder& enc) {
    auto frame = blackFrame(enc.inputFormat, enc.ctx->width, enc.ctx->height);
    if (!frame) {
        return std::unexpected(frame.error());
    }
    auto ready = prepareForEncoder(enc, std::move(*frame));
    if (!ready) {
        return std::unexpected(ready.error());
    }
    (*ready)->pts = 0;
    int r = avcodec_send_frame(enc.ctx.get(), ready->get());
    if (r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::EncoderError, "test frame rejected", r));
    }
    r = avcodec_send_frame(enc.ctx.get(), nullptr);
    if (r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::EncoderError, "flush rejected", r));
    }
    PacketPtr packet(av_packet_alloc());
    int packets = 0;
    for (;;) {
        r = avcodec_receive_packet(enc.ctx.get(), packet.get());
        if (r == AVERROR_EOF) {
            break;
        }
        if (r < 0) {
            return std::unexpected(ffmpegError(ErrorCode::EncoderError, "test frame failed", r));
        }
        ++packets;
        av_packet_unref(packet.get());
    }
    if (packets == 0) {
        return std::unexpected(Error{ErrorCode::EncoderError, "test frame produced no packet"});
    }
    return {};
}

std::expected<BufferRefPtr, Error> createHwFrames(AVHWDeviceType type, AVPixelFormat hwFormat,
                                                  AVPixelFormat swFormat, int width, int height) {
    AVBufferRef* device = nullptr;
    int r = av_hwdevice_ctx_create(&device, type, nullptr, nullptr, 0);
    if (r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::DeviceError, "no hardware device", r));
    }
    BufferRefPtr deviceRef(device);
    BufferRefPtr frames(av_hwframe_ctx_alloc(device));
    if (!frames) {
        return std::unexpected(Error{ErrorCode::DeviceError, "no hardware frame pool"});
    }
    auto* fc = reinterpret_cast<AVHWFramesContext*>(frames->data);
    fc->format = hwFormat;
    fc->sw_format = swFormat;
    fc->width = width;
    fc->height = height;
    fc->initial_pool_size = 16;
    r = av_hwframe_ctx_init(frames.get());
    if (r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::DeviceError, "hardware frame pool", r));
    }
    return frames;
}

} // namespace

AVPixelFormat avPixelFormatOf(media::PixelFormat format) {
    return toAvPixelFormat(format);
}

AVColorSpace avColorSpaceOf(ColorMatrix matrix) {
    return toAvColorSpace(matrix);
}

// ---- Muxer ------------------------------------------------------------------------------------------------

std::expected<Muxer, Error> Muxer::create(Container container, const std::filesystem::path& file) {
    media::detail::ensureFfmpegLogging();
    Muxer m;
    m.m_container = container;
    m.m_path = pathToUtf8(file);
    const char* format = container == Container::Mp4   ? "mp4"
                         : container == Container::Mov ? "mov"
                         : container == Container::Mkv ? "matroska"
                         : container == Container::Gif ? "gif"
                         : container == Container::WebM ? "webm"
                                                       : "wav";
    const int r = avformat_alloc_output_context2(&m.m_ctx, nullptr, format, m.m_path.c_str());
    if (r < 0 || m.m_ctx == nullptr) {
        return std::unexpected(ffmpegError(ErrorCode::Unsupported, "Cannot create the muxer", r));
    }
    return m;
}

Muxer::Muxer(Muxer&& other) noexcept
    : m_ctx(std::exchange(other.m_ctx, nullptr)), m_container(other.m_container),
      m_path(std::move(other.m_path)), m_open(std::exchange(other.m_open, false)),
      m_writeMutex(std::move(other.m_writeMutex)) {}

Muxer& Muxer::operator=(Muxer&& other) noexcept {
    if (this != &other) {
        abort();
        avformat_free_context(m_ctx);
        m_ctx = std::exchange(other.m_ctx, nullptr);
        m_container = other.m_container;
        m_path = std::move(other.m_path);
        m_open = std::exchange(other.m_open, false);
        m_writeMutex = std::move(other.m_writeMutex);
    }
    return *this;
}

Muxer::~Muxer() {
    abort();
    avformat_free_context(m_ctx);
}

bool Muxer::wantsGlobalHeader() const {
    return m_ctx && (m_ctx->oformat->flags & AVFMT_GLOBALHEADER) != 0;
}

std::expected<AVStream*, Error> Muxer::addStream(const AVCodecContext* encoder) {
    AVStream* st = avformat_new_stream(m_ctx, nullptr);
    if (!st) {
        return std::unexpected(Error{ErrorCode::EncoderError, "Cannot add a stream"});
    }
    const int r = avcodec_parameters_from_context(st->codecpar, encoder);
    if (r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::EncoderError, "Cannot copy the stream parameters", r));
    }
    st->time_base = encoder->time_base;
    if (encoder->codec_type == AVMEDIA_TYPE_VIDEO) {
        st->avg_frame_rate = encoder->framerate;
        if (encoder->codec_id == AV_CODEC_ID_HEVC &&
            (m_container == Container::Mp4 || m_container == Container::Mov)) {
            st->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1'); // what Apple players require
        }
    }
    return st;
}

std::expected<void, Error> Muxer::open() {
    if ((m_ctx->oformat->flags & AVFMT_NOFILE) == 0) {
        // FFmpeg's file protocol takes UTF-8 paths on Windows.
        const int r = avio_open(&m_ctx->pb, m_path.c_str(), AVIO_FLAG_WRITE);
        if (r < 0) {
            return std::unexpected(ffmpegError(ErrorCode::IoError, "Cannot create " + m_path, r));
        }
    }
    m_open = true;
    DictionaryOwner opts;
    if (m_container == Container::Mp4) {
        setOption(&opts.dict, "movflags", "+faststart"); // index first: plays while downloading
    } else if (m_container == Container::Wav) {
        setOption(&opts.dict, "rf64", "auto"); // > 4 GiB
    }
    const int r = avformat_write_header(m_ctx, &opts.dict);
    if (r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::EncoderError, "Cannot write the file header", r));
    }
    return {};
}

std::expected<void, Error> Muxer::write(AVPacket* packet, AVRational encoderTimeBase, AVStream* stream) {
    std::lock_guard lock(*m_writeMutex);
    av_packet_rescale_ts(packet, encoderTimeBase, stream->time_base);
    packet->stream_index = stream->index;
    const int r = av_interleaved_write_frame(m_ctx, packet);
    if (r < 0) {
        av_packet_unref(packet);
        return std::unexpected(ffmpegError(ErrorCode::IoError, "Cannot write to " + m_path, r));
    }
    return {};
}

std::expected<void, Error> Muxer::finish() {
    int r = av_write_trailer(m_ctx);
    if (r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::IoError, "Cannot finish " + m_path, r));
    }
    if (m_ctx->pb) {
        r = avio_closep(&m_ctx->pb);
        if (r < 0) {
            return std::unexpected(ffmpegError(ErrorCode::IoError, "Cannot close " + m_path, r));
        }
    }
    m_open = false;
    return {};
}

void Muxer::abort() noexcept {
    if (m_open && m_ctx && m_ctx->pb) {
        avio_closep(&m_ctx->pb);
    }
    m_open = false;
}

// ---- Video encoder ----------------------------------------------------------------------------------------

std::expected<VideoEncoder, Error> openVideoEncoder(const std::string& name, const VideoEncodeParams& params,
                                                    bool probe) {
    media::detail::ensureFfmpegLogging();
    const VideoSettings& v = params.settings;
    const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
    if (!codec || !av_codec_is_encoder(codec)) {
        return std::unexpected(Error{ErrorCode::Unsupported, "not in this FFmpeg build"});
    }
    if (codec->id != codecIdOf(v.codec)) {
        return std::unexpected(
            Error{ErrorCode::InvalidArgument, std::format("does not encode {}", videoCodecName(v.codec))});
    }
    VideoEncoder enc;
    enc.name = name;
    enc.ctx.reset(avcodec_alloc_context3(codec));
    if (!enc.ctx) {
        return std::unexpected(Error{ErrorCode::EncoderError, "Out of memory (encoder)"});
    }
    AVCodecContext* ctx = enc.ctx.get();
    ctx->width = params.width;
    ctx->height = params.height;
    ctx->time_base = AVRational{params.frameRate.den, params.frameRate.num};
    ctx->framerate = AVRational{params.frameRate.num, params.frameRate.den};
    ctx->sample_aspect_ratio = params.sampleAspect;
    ctx->color_primaries = params.primaries;
    ctx->color_trc = params.transfer;
    ctx->colorspace = params.matrix;
    ctx->color_range = params.range;
    ctx->chroma_sample_location = params.chromaLocation;
    ctx->thread_count = 0;
    if (params.globalHeader) {
        ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if (v.codec == VideoCodec::H264 || v.codec == VideoCodec::Hevc || v.codec == VideoCodec::Av1 ||
        v.codec == VideoCodec::Vp9) {
        const auto gop = ticksToFrames(v.keyframeInterval, params.frameRate, Rounding::Nearest);
        ctx->gop_size = static_cast<int>(std::clamp<std::int64_t>(gop.value_or(250), 1, 1000));
    }

    // Pixel format: the first CPU layout of the output format the encoder takes.
    const void* config = nullptr;
    int count = 0;
    int r = avcodec_get_supported_config(ctx, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &config, &count);
    if (r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::EncoderError, "pixel formats", r));
    }
    const auto* supported = static_cast<const AVPixelFormat*>(config);
    auto isSupported = [&](AVPixelFormat f) {
        return supported == nullptr || std::find(supported, supported + count, f) != supported + count;
    };
    const std::vector<AVPixelFormat> wanted = inputFormatsFor(v.pixelFormat);
    const bool vaapi = endsWith(name, "_vaapi");
    if (vaapi) {
        if (!isSupported(AV_PIX_FMT_VAAPI)) {
            return std::unexpected(Error{ErrorCode::Unsupported, "no VA-API input"});
        }
        // VA-API surfaces: 4:2:0 only (NV12 / P010).
        const AVPixelFormat sw = wanted.front();
        if (sw != AV_PIX_FMT_NV12 && sw != AV_PIX_FMT_P010LE) {
            return std::unexpected(Error{ErrorCode::Unsupported,
                                         std::format("cannot encode {}", outputFormatName(v.pixelFormat))});
        }
        auto frames =
            createHwFrames(AV_HWDEVICE_TYPE_VAAPI, AV_PIX_FMT_VAAPI, sw, params.width, params.height);
        if (!frames) {
            return std::unexpected(frames.error());
        }
        enc.hwFrames = std::move(*frames);
        ctx->hw_frames_ctx = av_buffer_ref(enc.hwFrames.get());
        ctx->pix_fmt = AV_PIX_FMT_VAAPI;
        enc.inputFormat = sw;
    } else {
        for (const AVPixelFormat f : wanted) {
            // Semi-planar layouts only for encoders that list them (never pick NV12 for an unlisted "any").
            if (supported == nullptr ? f == wanted.back() : isSupported(f)) {
                enc.inputFormat = f;
                break;
            }
        }
        if (enc.inputFormat == AV_PIX_FMT_NONE) {
            return std::unexpected(Error{ErrorCode::Unsupported,
                                         std::format("cannot encode {}", outputFormatName(v.pixelFormat))});
        }
        ctx->pix_fmt = enc.inputFormat;
    }

    DictionaryOwner opts;
    configureVideoOptions(name, v, ctx, &opts.dict);
    r = avcodec_open2(ctx, codec, &opts.dict);
    if (r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::EncoderError, "cannot open", r));
    }
    if (probe) {
        if (auto ok = testEncode(enc); !ok) {
            return std::unexpected(ok.error());
        }
    }
    return enc;
}

std::expected<VideoEncoderChoice, Error> chooseVideoEncoder(const VideoEncodeParams& params) {
    const VideoSettings& v = params.settings;
    const bool automatic = v.encoder.empty() || v.encoder == "auto";
    const std::vector<std::string> names =
        automatic ? encoderCandidates(v.codec) : std::vector<std::string>{v.encoder};
    const std::string_view software = softwareEncoder(v.codec);
    VideoEncoderChoice choice;
    for (const std::string& name : names) {
        if (name != software) {
            // Hardware: open a throwaway context and encode a test frame first.
            if (auto probe = openVideoEncoder(name, params, true); !probe) {
                choice.fallbacks.push_back(std::format("{}: {}", name, probe.error().message));
                continue;
            }
        }
        auto enc = openVideoEncoder(name, params, false);
        if (!enc) {
            choice.fallbacks.push_back(std::format("{}: {}", name, enc.error().message));
            continue;
        }
        choice.encoder = std::move(*enc);
        return choice;
    }
    std::string reasons;
    for (const std::string& f : choice.fallbacks) {
        reasons += (reasons.empty() ? "" : "; ") + f;
    }
    return std::unexpected(
        Error{ErrorCode::Unsupported,
              std::format("No working {} encoder for {}x{} {} ({})", videoCodecName(v.codec), params.width,
                          params.height, outputFormatName(v.pixelFormat), reasons)});
}

} // namespace detail

std::expected<EncoderProbe, Error> probeVideoEncoder(const VideoSettings& video, int width, int height,
                                                     Rate frameRate) {
    auto choice =
        detail::chooseVideoEncoder(detail::VideoEncodeParams{video, width, height, frameRate, false});
    if (!choice) {
        return std::unexpected(choice.error());
    }
    return EncoderProbe{choice->encoder.name, std::move(choice->fallbacks)};
}

namespace detail {

std::expected<FramePtr, Error> blackFrame(AVPixelFormat format, int width, int height) {
    auto f = allocVideoFrame(format, width, height);
    if (!f) {
        return f;
    }
    ptrdiff_t linesizes[4] = {};
    for (int i = 0; i < 4; ++i) {
        linesizes[i] = (*f)->linesize[i];
    }
    const int r = av_image_fill_black((*f)->data, linesizes, format, AVCOL_RANGE_MPEG, width, height);
    if (r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::EncoderError, "Cannot clear a frame", r));
    }
    return f;
}

std::expected<FramePtr, Error> prepareForEncoder(const VideoEncoder& encoder, FramePtr frame) {
    if (!encoder.hwFrames) {
        return frame;
    }
    FramePtr hw(av_frame_alloc());
    int r = av_hwframe_get_buffer(encoder.hwFrames.get(), hw.get(), 0);
    if (r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::DeviceError, "Cannot get a hardware frame", r));
    }
    r = av_hwframe_transfer_data(hw.get(), frame.get(), 0);
    if (r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::DeviceError, "Cannot upload a frame", r));
    }
    av_frame_copy_props(hw.get(), frame.get());
    return hw;
}

// ---- Frame conversion -------------------------------------------------------------------------------------

std::expected<FrameConverter, Error> FrameConverter::create(int renderWidth, int renderHeight, int width,
                                                            int height, AVPixelFormat format) {
    FrameConverter c;
    c.m_sws.reset(sws_alloc_context());
    if (!c.m_sws) {
        return std::unexpected(Error{ErrorCode::EncoderError, "Out of memory (scaler)"});
    }
    c.m_sws->flags = SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INP;
    c.m_sws->threads = 0; // automatic
    c.m_renderWidth = renderWidth;
    c.m_renderHeight = renderHeight;
    c.m_width = width;
    c.m_height = height;
    c.m_format = format;
    if (format == AV_PIX_FMT_PAL8) { // GIF: convert to RGB, then a palette of 256 colours for each frame
        auto palette = PaletteGraph::create(width, height);
        if (!palette) {
            return std::unexpected(palette.error());
        }
        c.m_palette = std::move(*palette);
    }
    return c;
}

std::expected<FramePtr, Error> FrameConverter::convert(const media::FrameHandle& source) {
    const media::CpuPlanes* cpu = source.cpu();
    if (!cpu) {
        return std::unexpected(Error{ErrorCode::Unsupported, "The video source must deliver CPU frames"});
    }
    if (source.width != m_renderWidth || source.height != m_renderHeight) {
        return std::unexpected(
            Error{ErrorCode::InvalidArgument,
                  std::format("The video source delivered {}x{}, the export asked for {}x{}", source.width,
                              source.height, m_renderWidth, m_renderHeight)});
    }
    const AVPixelFormat srcFormat = toAvPixelFormat(source.format);
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(srcFormat);
    if (srcFormat == AV_PIX_FMT_NONE || !desc) {
        return std::unexpected(
            Error{ErrorCode::Unsupported, "The video source delivered an unknown pixel format"});
    }

    // ScaleMode::Fill: centre crop of the rendered canvas (offsets even so chroma stays sited).
    const int viewW = std::min(m_renderWidth, m_width);
    const int viewH = std::min(m_renderHeight, m_height);
    const int cropX = ((m_renderWidth - viewW) / 2) & ~1;
    const int cropY = ((m_renderHeight - viewH) / 2) & ~1;
    int pixsteps[4] = {};
    av_image_fill_max_pixsteps(pixsteps, nullptr, desc);

    FramePtr src(av_frame_alloc()); // a view of the source memory, not refcounted
    src->format = srcFormat;
    src->width = viewW;
    src->height = viewH;
    for (int p = 0; p < source.planeCount() && p < 4; ++p) {
        const media::Plane& plane = cpu->planes[static_cast<std::size_t>(p)];
        const bool chroma = (p == 1 || p == 2) && !(desc->flags & AV_PIX_FMT_FLAG_RGB);
        const int x = chroma ? cropX >> desc->log2_chroma_w : cropX;
        const int y = chroma ? cropY >> desc->log2_chroma_h : cropY;
        src->data[p] = const_cast<std::uint8_t*>(plane.data) + static_cast<std::ptrdiff_t>(y) * plane.stride +
                       static_cast<std::ptrdiff_t>(x) * pixsteps[p];
        src->linesize[p] = plane.stride;
    }
    if (desc->flags & AV_PIX_FMT_FLAG_RGB) {
        src->colorspace = AVCOL_SPC_RGB;
        src->color_range = AVCOL_RANGE_JPEG;
    } else {
        src->colorspace = toAvColorSpace(source.color.matrix);
        src->color_range = source.color.range == ColorRange::Full ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    }
    src->color_primaries = AVCOL_PRI_BT709;
    src->color_trc = AVCOL_TRC_BT709;
    src->chroma_location = AVCHROMA_LOC_LEFT;

    const bool padded = viewW < m_width || viewH < m_height; // ScaleMode::Fit
    const AVPixelFormat work = m_palette ? AV_PIX_FMT_RGB24 : m_format; // GIF: RGB first, the palette after
    auto converted = allocVideoFrame(work, viewW, viewH);
    if (!converted) {
        return converted;
    }
    if (m_palette) {
        (*converted)->colorspace = AVCOL_SPC_RGB;
        (*converted)->color_range = AVCOL_RANGE_JPEG;
    }
    int r = sws_scale_frame(m_sws.get(), converted->get(), src.get());
    if (r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::EncoderError, "Cannot convert a frame", r));
    }
    if (!padded) {
        return m_palette ? m_palette->apply(std::move(*converted)) : std::move(converted);
    }

    auto out = blackFrame(work, m_width, m_height);
    if (!out) {
        return out;
    }
    const AVPixFmtDescriptor* dstDesc = av_pix_fmt_desc_get(work);
    int dstSteps[4] = {};
    av_image_fill_max_pixsteps(dstSteps, nullptr, dstDesc);
    const int padX = ((m_width - viewW) / 2) & ~1;
    const int padY = ((m_height - viewH) / 2) & ~1;
    const int planes = av_pix_fmt_count_planes(work);
    for (int p = 0; p < planes; ++p) {
        const bool chroma = (p == 1 || p == 2) && !(dstDesc->flags & AV_PIX_FMT_FLAG_RGB);
        const int sx = chroma ? dstDesc->log2_chroma_w : 0;
        const int sy = chroma ? dstDesc->log2_chroma_h : 0;
        const int rows = -((-viewH) >> sy); // ceil
        const int bytes = (-((-viewW) >> sx)) * dstSteps[p];
        std::uint8_t* dst = (*out)->data[p] + static_cast<std::ptrdiff_t>(padY >> sy) * (*out)->linesize[p] +
                            static_cast<std::ptrdiff_t>(padX >> sx) * dstSteps[p];
        av_image_copy_plane(dst, (*out)->linesize[p], (*converted)->data[p], (*converted)->linesize[p], bytes,
                            rows);
    }
    return m_palette ? m_palette->apply(std::move(*out)) : std::move(out);
}

// ---- GIF palettes ------------------------------------------------------------------------------------------

PaletteGraph::~PaletteGraph() {
    avfilter_graph_free(&m_graph);
}

std::expected<std::shared_ptr<PaletteGraph>, Error> PaletteGraph::create(int width, int height) {
    auto g = std::shared_ptr<PaletteGraph>(new PaletteGraph());
    g->m_graph = avfilter_graph_alloc();
    if (!g->m_graph) {
        return std::unexpected(Error{ErrorCode::EncoderError, "Out of memory (palette filter)"});
    }
    const std::string args = std::format("video_size={}x{}:pix_fmt={}:time_base=1/100:pixel_aspect=1/1", width, height,
                                         static_cast<int>(AV_PIX_FMT_RGB24));
    int r = avfilter_graph_create_filter(&g->m_source, avfilter_get_by_name("buffer"), "in", args.c_str(), nullptr,
                                         g->m_graph);
    if (r >= 0) {
        r = avfilter_graph_create_filter(&g->m_sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr,
                                         g->m_graph);
    }
    if (r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::EncoderError, "Cannot create the palette filter", r));
    }
    // A palette of the 256 colours each frame needs (stats_mode=single), applied to that frame with error diffusion.
    AVFilterInOut* outputs = avfilter_inout_alloc();
    AVFilterInOut* inputs = avfilter_inout_alloc();
    outputs->name = av_strdup("in");
    outputs->filter_ctx = g->m_source;
    outputs->pad_idx = 0;
    outputs->next = nullptr;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = g->m_sink;
    inputs->pad_idx = 0;
    inputs->next = nullptr;
    r = avfilter_graph_parse_ptr(g->m_graph,
                                 "split[a][b];[a]palettegen=stats_mode=single:max_colors=256[p];"
                                 "[b][p]paletteuse=new=1:dither=sierra2_4a",
                                 &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (r >= 0) {
        r = avfilter_graph_config(g->m_graph, nullptr);
    }
    if (r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::EncoderError, "Cannot set up the palette filter", r));
    }
    return g;
}

std::expected<FramePtr, Error> PaletteGraph::apply(FramePtr rgb) {
    rgb->pts = m_next++;
    int r = av_buffersrc_add_frame_flags(m_source, rgb.get(), AV_BUFFERSRC_FLAG_KEEP_REF);
    if (r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::EncoderError, "Cannot feed the palette filter", r));
    }
    FramePtr out(av_frame_alloc());
    r = av_buffersink_get_frame(m_sink, out.get());
    if (r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::EncoderError, "The palette filter gave no frame", r));
    }
    out->pts = AV_NOPTS_VALUE; // the export sets its own
    return out;
}

// ---- Audio encoder ----------------------------------------------------------------------------------------

std::expected<AudioEncoder, Error> AudioEncoder::open(const AudioSettings& settings, bool globalHeader) {
    media::detail::ensureFfmpegLogging();
    AudioEncoder enc;
    enc.m_name = std::string(audioCodecName(settings.codec));
    const AVCodec* codec = avcodec_find_encoder_by_name(enc.m_name.c_str());
    if (!codec) {
        return std::unexpected(Error{ErrorCode::Unsupported, enc.m_name + " is not in this FFmpeg build"});
    }
    enc.m_ctx.reset(avcodec_alloc_context3(codec));
    if (!enc.m_ctx) {
        return std::unexpected(Error{ErrorCode::EncoderError, "Out of memory (audio encoder)"});
    }
    AVCodecContext* ctx = enc.m_ctx.get();
    ctx->sample_rate = settings.sampleRate;
    av_channel_layout_default(&ctx->ch_layout, settings.channels);
    ctx->time_base = AVRational{1, settings.sampleRate};
    ctx->sample_fmt = settings.codec == AudioCodec::Aac || settings.codec == AudioCodec::Ac3     ? AV_SAMPLE_FMT_FLTP
                      : settings.codec == AudioCodec::Pcm16 || settings.codec == AudioCodec::Opus ? AV_SAMPLE_FMT_S16
                                                                                                   : AV_SAMPLE_FMT_S32;
    if (settings.codec == AudioCodec::Aac || settings.codec == AudioCodec::Ac3 || settings.codec == AudioCodec::Opus) {
        ctx->bit_rate = settings.bitrate;
    }
    if (globalHeader) {
        ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    const int r = avcodec_open2(ctx, codec, nullptr);
    if (r < 0) {
        return std::unexpected(
            ffmpegError(ErrorCode::EncoderError, "Cannot open the " + enc.m_name + " encoder", r));
    }
    enc.m_channels = settings.channels;
    enc.m_frameSize = ctx->frame_size > 0 ? ctx->frame_size : 1024; // PCM: any size
    enc.m_packet.reset(av_packet_alloc());
    enc.m_pending.reserve(static_cast<std::size_t>(enc.m_frameSize) *
                          static_cast<std::size_t>(enc.m_channels));
    return enc;
}

std::expected<void, Error> AudioEncoder::encode(std::span<const float> input, Muxer& muxer,
                                                AVStream* stream) {
    const std::size_t per = static_cast<std::size_t>(m_frameSize) * static_cast<std::size_t>(m_channels);
    std::size_t pos = 0;
    if (!m_pending.empty()) {
        const std::size_t take = std::min(per - m_pending.size(), input.size());
        m_pending.insert(m_pending.end(), input.begin(), input.begin() + static_cast<std::ptrdiff_t>(take));
        pos = take;
        if (m_pending.size() < per) {
            return {};
        }
        if (auto ok = sendFrame(m_pending.data(), m_frameSize, muxer, stream); !ok) {
            return ok;
        }
        m_pending.clear();
    }
    while (input.size() - pos >= per) {
        if (auto ok = sendFrame(input.data() + pos, m_frameSize, muxer, stream); !ok) {
            return ok;
        }
        pos += per;
    }
    m_pending.insert(m_pending.end(), input.begin() + static_cast<std::ptrdiff_t>(pos), input.end());
    return {};
}

std::expected<void, Error> AudioEncoder::flush(Muxer& muxer, AVStream* stream) {
    if (!m_pending.empty()) {
        const int frames = static_cast<int>(m_pending.size() / static_cast<std::size_t>(m_channels));
        if (auto ok = sendFrame(m_pending.data(), frames, muxer, stream); !ok) {
            return ok;
        }
        m_pending.clear();
    }
    const int r = avcodec_send_frame(m_ctx.get(), nullptr);
    if (r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::EncoderError, "Cannot flush the audio encoder", r));
    }
    return drainEncoder(m_ctx.get(), m_packet.get(), muxer, stream, true);
}

std::expected<void, Error> AudioEncoder::sendFrame(const float* interleaved, int frames, Muxer& muxer,
                                                   AVStream* stream) {
    AVCodecContext* ctx = m_ctx.get();
    FramePtr f(av_frame_alloc());
    f->nb_samples = frames;
    f->format = ctx->sample_fmt;
    f->sample_rate = ctx->sample_rate;
    int r = av_channel_layout_copy(&f->ch_layout, &ctx->ch_layout);
    if (r >= 0) {
        r = av_frame_get_buffer(f.get(), 0);
    }
    if (r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::EncoderError, "Cannot allocate an audio frame", r));
    }
    const auto n = static_cast<std::size_t>(frames);
    const auto ch = static_cast<std::size_t>(m_channels);
    switch (ctx->sample_fmt) {
    case AV_SAMPLE_FMT_FLTP:
        for (std::size_t c = 0; c < ch; ++c) {
            auto* dst = reinterpret_cast<float*>(f->extended_data[c]);
            for (std::size_t i = 0; i < n; ++i) {
                dst[i] = interleaved[i * ch + c];
            }
        }
        break;
    case AV_SAMPLE_FMT_S16: {
        auto* dst = reinterpret_cast<std::int16_t*>(f->data[0]);
        for (std::size_t i = 0; i < n * ch; ++i) {
            dst[i] = static_cast<std::int16_t>(quantizePcmSample(interleaved[i], 16));
        }
        break;
    }
    default: { // S32 carrying 24-bit samples (pcm_s24le keeps the top 24 bits)
        auto* dst = reinterpret_cast<std::int32_t*>(f->data[0]);
        for (std::size_t i = 0; i < n * ch; ++i) {
            dst[i] = quantizePcmSample(interleaved[i], 24) * 256;
        }
        break;
    }
    }
    f->pts = m_nextPts;
    m_nextPts += frames;
    r = avcodec_send_frame(ctx, f.get());
    if (r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::EncoderError, "Audio encoding failed", r));
    }
    return drainEncoder(ctx, m_packet.get(), muxer, stream, false);
}

std::expected<void, Error> drainEncoder(AVCodecContext* encoder, AVPacket* packet, Muxer& muxer,
                                        AVStream* stream, bool flushing) {
    for (;;) {
        const int r = avcodec_receive_packet(encoder, packet);
        if (r == AVERROR(EAGAIN)) {
            if (flushing) {
                return std::unexpected(Error{ErrorCode::EncoderError, "Encoder stalled while flushing"});
            }
            return {};
        }
        if (r == AVERROR_EOF) {
            return {};
        }
        if (r < 0) {
            return std::unexpected(ffmpegError(ErrorCode::EncoderError, "Encoding failed", r));
        }
        if (packet->duration == 0 && encoder->codec_type == AVMEDIA_TYPE_VIDEO) {
            packet->duration = 1; // one frame in the encoder's time base
        }
        if (auto ok = muxer.write(packet, encoder->time_base, stream); !ok) {
            return ok;
        }
    }
}

} // namespace detail
} // namespace wabcut::exporter
