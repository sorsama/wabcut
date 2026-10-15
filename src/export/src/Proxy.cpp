#include <wabcut/export/Proxy.h>

#include "Encode.h"
#include "MediaTime.h"

#include <wabcut/media/Probe.h>
#include <wabcut/media/VideoDecoder.h>
#include <wabcut/model/MediaLocator.h>

extern "C" {
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cstring>
#include <numeric>
#include <system_error>
#include <unordered_map>

namespace wabcut::exporter {

namespace {

using detail::CodecContextPtr;
using detail::FramePtr;
using detail::PacketPtr;
using detail::SwsContextPtr;
using detail::ffmpegError;
using detail::mediaOriginOf;
using detail::ticksToUnits;

struct FormatInputDeleter {
    void operator()(AVFormatContext* p) const noexcept { avformat_close_input(&p); }
};
using FormatInputPtr = std::unique_ptr<AVFormatContext, FormatInputDeleter>;

constexpr std::string_view kCodecNames[] = {"prores_ks proxy", "dnxhr_lb", "h264 intra"};

/// Time base whose unit divides both `a` and `b` exactly (so timestamps of both convert without rounding), or
/// nullopt when that would not fit a MOV track timescale.
std::optional<AVRational> commonTimeBase(AVRational a, AVRational b) {
    const auto unitDen = [](AVRational r) -> std::int64_t {
        const std::int64_t g = std::gcd<std::int64_t>(r.num, r.den);
        return r.num > 0 && r.den > 0 && r.num / g == 1 ? r.den / g : 0;
    };
    const std::int64_t da = unitDen(a);
    const std::int64_t db = unitDen(b);
    if (da <= 0 || db <= 0) {
        return std::nullopt;
    }
    const std::int64_t l = std::lcm(da, db);
    if (l > (std::int64_t{1} << 30)) {
        return std::nullopt;
    }
    return AVRational{1, static_cast<int>(l)};
}

/// The stream that sets the origin (its time base must divide the proxy's video time base).
const AVStream* originStream(const AVFormatContext* fmt) {
    const AVStream* best = nullptr;
    Ticks origin{};
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream* st = fmt->streams[i];
        const AVMediaType type = st->codecpar->codec_type;
        if ((type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO) ||
            (st->disposition & AV_DISPOSITION_ATTACHED_PIC) || st->start_time == AV_NOPTS_VALUE) {
            continue;
        }
        const Ticks t{av_rescale_rnd(st->start_time, static_cast<std::int64_t>(st->time_base.num) * kTicksPerSecond,
                                     st->time_base.den, AV_ROUND_NEAR_INF)};
        if (!best || t < origin) {
            best = st;
            origin = t;
        }
    }
    return best;
}

std::string timecodeOf(const AVFormatContext* fmt, const AVStream* video) {
    if (const AVDictionaryEntry* e = av_dict_get(video->metadata, "timecode", nullptr, 0)) {
        return e->value;
    }
    if (const AVDictionaryEntry* e = av_dict_get(fmt->metadata, "timecode", nullptr, 0)) {
        return e->value;
    }
    for (unsigned i = 0; i < fmt->nb_streams; ++i) { // a QuickTime tmcd track
        if (const AVDictionaryEntry* e = av_dict_get(fmt->streams[i]->metadata, "timecode", nullptr, 0)) {
            return e->value;
        }
    }
    return {};
}

VideoSettings videoSettingsFor(const ProxySettings& s) {
    VideoSettings v;
    switch (s.codec) {
    case ProxyCodec::ProResProxy:
        v.codec = VideoCodec::ProRes;
        v.encoder = "prores_ks";
        v.proresProfile = ProResProfile::Proxy;
        v.pixelFormat = OutputPixelFormat::Yuv422p10;
        break;
    case ProxyCodec::DnxhrLb:
        v.codec = VideoCodec::DnxHR;
        v.encoder = "dnxhd";
        v.dnxhrProfile = DnxhrProfile::Lb;
        v.pixelFormat = OutputPixelFormat::Yuv422p;
        break;
    case ProxyCodec::H264Intra:
        v.codec = VideoCodec::H264;
        v.encoder = "auto"; // NVENC/AMF/QSV first, probed with a test frame, then libx264
        v.quality = 20;
        v.pixelFormat = OutputPixelFormat::Yuv420p;
        break;
    }
    return v;
}

/// Downscales decoded frames into the encoder's input format without touching the colour (same matrix and range
/// on both sides, so swscale only resamples and converts the bit depth / chroma layout).
class Downscaler {
public:
    std::expected<FramePtr, Error> convert(const media::FrameHandle& source, int width, int height,
                                           AVPixelFormat format, AVColorSpace matrix, AVColorRange range) {
        const media::CpuPlanes* cpu = source.cpu();
        if (!cpu) {
            return std::unexpected(Error{ErrorCode::Unsupported, "The decoder delivered a GPU frame"});
        }
        const AVPixelFormat srcFormat = detail::avPixelFormatOf(source.format);
        if (srcFormat == AV_PIX_FMT_NONE) {
            return std::unexpected(Error{ErrorCode::Unsupported, "The decoder delivered an unknown pixel format"});
        }
        if (!m_sws) {
            m_sws.reset(sws_alloc_context());
            if (!m_sws) {
                return std::unexpected(Error{ErrorCode::EncoderError, "Out of memory (scaler)"});
            }
            m_sws->flags = SWS_AREA | SWS_ACCURATE_RND;
            m_sws->threads = 0;
        }
        FramePtr src(av_frame_alloc()); // a view of the decoded planes
        FramePtr dst(av_frame_alloc());
        if (!src || !dst) {
            return std::unexpected(Error{ErrorCode::EncoderError, "Out of memory (frame)"});
        }
        src->format = srcFormat;
        src->width = source.width;
        src->height = source.height;
        for (int p = 0; p < source.planeCount() && p < 4; ++p) {
            const media::Plane& plane = cpu->planes[static_cast<std::size_t>(p)];
            src->data[p] = const_cast<std::uint8_t*>(plane.data);
            src->linesize[p] = plane.stride;
        }
        const bool rgb = (av_pix_fmt_desc_get(srcFormat)->flags & AV_PIX_FMT_FLAG_RGB) != 0;
        src->colorspace = rgb ? AVCOL_SPC_RGB : matrix;
        src->color_range = rgb ? AVCOL_RANGE_JPEG : range;
        dst->format = format;
        dst->width = width;
        dst->height = height;
        if (const int r = av_frame_get_buffer(dst.get(), 0); r < 0) {
            return std::unexpected(ffmpegError(ErrorCode::EncoderError, "Cannot allocate a proxy frame", r));
        }
        dst->colorspace = matrix;
        dst->color_range = range;
        if (const int r = sws_scale_frame(m_sws.get(), dst.get(), src.get()); r < 0) {
            return std::unexpected(ffmpegError(ErrorCode::EncoderError, "Cannot scale a frame", r));
        }
        return dst;
    }

private:
    SwsContextPtr m_sws;
};

/// The colour tags a proxy carries: the source stream's where it states them, else the effective values the
/// decoder resolved for the first frame (an untagged HD source must not turn into an "SD = BT.601" proxy).
struct ColorTags {
    AVColorPrimaries primaries = AVCOL_PRI_UNSPECIFIED;
    AVColorTransferCharacteristic transfer = AVCOL_TRC_UNSPECIFIED;
    AVColorSpace matrix = AVCOL_SPC_UNSPECIFIED;
    AVColorRange range = AVCOL_RANGE_UNSPECIFIED;
    AVChromaLocation chroma = AVCHROMA_LOC_UNSPECIFIED;
};

AVColorPrimaries avPrimaries(ColorPrimaries p) {
    switch (p) {
    case ColorPrimaries::BT709:
        return AVCOL_PRI_BT709;
    case ColorPrimaries::BT601_525:
        return AVCOL_PRI_SMPTE170M;
    case ColorPrimaries::BT601_625:
        return AVCOL_PRI_BT470BG;
    case ColorPrimaries::BT2020:
        return AVCOL_PRI_BT2020;
    case ColorPrimaries::DCIP3:
        return AVCOL_PRI_SMPTE431;
    case ColorPrimaries::DisplayP3:
        return AVCOL_PRI_SMPTE432;
    case ColorPrimaries::Unknown:
        break;
    }
    return AVCOL_PRI_UNSPECIFIED;
}

AVColorTransferCharacteristic avTransfer(TransferFunction t) {
    switch (t) {
    case TransferFunction::BT709:
        return AVCOL_TRC_BT709;
    case TransferFunction::SRGB:
        return AVCOL_TRC_IEC61966_2_1;
    case TransferFunction::Linear:
        return AVCOL_TRC_LINEAR;
    case TransferFunction::Gamma22:
        return AVCOL_TRC_GAMMA22;
    case TransferFunction::Gamma28:
        return AVCOL_TRC_GAMMA28;
    case TransferFunction::PQ:
        return AVCOL_TRC_SMPTE2084;
    case TransferFunction::HLG:
        return AVCOL_TRC_ARIB_STD_B67;
    case TransferFunction::Unknown:
        break;
    }
    return AVCOL_TRC_UNSPECIFIED;
}

AVChromaLocation avChroma(ChromaLocation c) {
    switch (c) {
    case ChromaLocation::Left:
        return AVCHROMA_LOC_LEFT;
    case ChromaLocation::Center:
        return AVCHROMA_LOC_CENTER;
    case ChromaLocation::TopLeft:
        return AVCHROMA_LOC_TOPLEFT;
    case ChromaLocation::Top:
        return AVCHROMA_LOC_TOP;
    case ChromaLocation::BottomLeft:
        return AVCHROMA_LOC_BOTTOMLEFT;
    case ChromaLocation::Bottom:
        return AVCHROMA_LOC_BOTTOM;
    case ChromaLocation::Unknown:
        break;
    }
    return AVCHROMA_LOC_UNSPECIFIED;
}

ColorTags colorTagsFor(const AVCodecParameters* sp, const ColorInfo& effective) {
    ColorTags t;
    t.primaries = sp->color_primaries != AVCOL_PRI_UNSPECIFIED ? sp->color_primaries : avPrimaries(effective.primaries);
    t.transfer = sp->color_trc != AVCOL_TRC_UNSPECIFIED ? sp->color_trc : avTransfer(effective.transfer);
    t.matrix = sp->color_space != AVCOL_SPC_UNSPECIFIED ? sp->color_space : detail::avColorSpaceOf(effective.matrix);
    t.range = sp->color_range != AVCOL_RANGE_UNSPECIFIED ? sp->color_range
              : effective.range == ColorRange::Full      ? AVCOL_RANGE_JPEG
                                                         : AVCOL_RANGE_MPEG;
    t.chroma = sp->chroma_location != AVCHROMA_LOC_UNSPECIFIED ? sp->chroma_location
                                                                : avChroma(effective.chromaLocation);
    return t;
}

struct PartialFile {
    std::filesystem::path path;
    bool keep = false;
    ~PartialFile() {
        if (!keep && !path.empty()) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }
};

} // namespace

std::string describeProxySettings(const ProxySettings& s) {
    return std::format("{} 1/{} audio={}", kCodecNames[static_cast<int>(s.codec)], s.divisor,
                       s.copyAudio ? "copy" : "none");
}

bool parseProxySettings(std::string_view text, ProxySettings& out) {
    for (int c = 0; c < 3; ++c) {
        for (const int divisor : {2, 4}) {
            for (const bool audio : {true, false}) {
                const ProxySettings s{static_cast<ProxyCodec>(c), divisor, audio};
                if (describeProxySettings(s) == text) {
                    out = s;
                    return true;
                }
            }
        }
    }
    return false;
}

std::pair<int, int> proxyFrameSize(int width, int height, int divisor) {
    const int d = std::max(1, divisor);
    const auto side = [d](int v) { return std::max(2, ((v + d) / (2 * d)) * 2); }; // nearest even of v / d
    return {side(width), side(height)};
}

std::expected<ProxyResult, Error> transcodeProxy(const std::string& source, const std::filesystem::path& output,
                                                 const ProxySettings& settings, const ProxyControl& control) {
    media::detail::ensureFfmpegLogging();
    if (settings.divisor != 2 && settings.divisor != 4) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "Proxies are half or quarter resolution"});
    }
    const auto cancelled = [] { return std::unexpected(Error{ErrorCode::Cancelled, "Proxy generation cancelled"}); };

    // ---- Source: our own demuxer for stream parameters and audio packets; VideoDecoder for the frames.
    AVFormatContext* rawIn = nullptr;
    if (const int r = avformat_open_input(&rawIn, source.c_str(), nullptr, nullptr); r < 0) {
        return std::unexpected(ffmpegError(r == AVERROR(ENOENT) ? ErrorCode::NotFound : ErrorCode::IoError,
                                           "Cannot open " + source, r));
    }
    FormatInputPtr in(rawIn);
    if (const int r = avformat_find_stream_info(in.get(), nullptr); r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::IoError, "Cannot read " + source, r));
    }
    auto decoder = media::VideoDecoder::open(source, -1, media::DecoderOptions{});
    if (!decoder) {
        return std::unexpected(decoder.error());
    }
    const VideoStreamInfo& info = (*decoder)->streamInfo();
    if (info.alpha != AlphaMode::None) {
        return std::unexpected(Error{ErrorCode::Unsupported, "Media with alpha is not proxied (the proxy codecs have no alpha)"});
    }
    if (info.index < 0 || info.index >= static_cast<int>(in->nb_streams)) {
        return std::unexpected(Error{ErrorCode::Unsupported, "No video stream"});
    }
    const AVStream* srcVideo = in->streams[info.index];
    const Ticks firstFrame = (*decoder)->firstFrameTime();
    auto first = (*decoder)->frameAt(firstFrame); // its effective colour completes the tags below
    if (!first) {
        return std::unexpected(first.error());
    }
    const Ticks origin = mediaOriginOf(in.get());
    const AVStream* originSt = originStream(in.get());

    // The proxy's video time base: exact for both the video timestamps and the origin.
    AVRational videoTb = srcVideo->time_base;
    if (originSt && originSt != srcVideo) {
        if (auto common = commonTimeBase(srcVideo->time_base, originSt->time_base)) {
            videoTb = *common;
        }
    }

    // ---- Encoder.
    const auto [width, height] = proxyFrameSize(info.width, info.height, settings.divisor);
    detail::VideoEncodeParams params;
    params.settings = videoSettingsFor(settings);
    params.width = width;
    params.height = height;
    params.frameRate = info.frameRate.num > 0 && info.frameRate.den > 0 ? info.frameRate : Rate{30, 1};
    if (settings.codec == ProxyCodec::H264Intra) {
        params.settings.keyframeInterval = framesToTicks(1, params.frameRate, Rounding::Floor).value_or(Ticks{1});
    }
    const AVCodecParameters* sp = srcVideo->codecpar;
    const ColorTags tags = colorTagsFor(sp, first->color);
    params.primaries = tags.primaries;
    params.transfer = tags.transfer;
    params.matrix = tags.matrix;
    params.range = tags.range;
    params.chromaLocation = tags.chroma;
    // Same displayed shape: the pixel aspect absorbs the rounding of the proxy size.
    const AVRational sar = info.sampleAspect.num > 0 && info.sampleAspect.den > 0
                               ? AVRational{static_cast<int>(info.sampleAspect.num), static_cast<int>(info.sampleAspect.den)}
                               : AVRational{1, 1};
    av_reduce(&params.sampleAspect.num, &params.sampleAspect.den,
              static_cast<std::int64_t>(sar.num) * info.width * height,
              static_cast<std::int64_t>(sar.den) * width * info.height, 1 << 30);

    std::error_code ec;
    std::filesystem::create_directories(output.parent_path(), ec);
    PartialFile partial{std::filesystem::path(output).concat(".partial")};
    auto muxer = detail::Muxer::create(Container::Mov, partial.path);
    if (!muxer) {
        return std::unexpected(muxer.error());
    }
    params.globalHeader = muxer->wantsGlobalHeader();
    auto choice = detail::chooseVideoEncoder(params);
    if (!choice) {
        return std::unexpected(choice.error());
    }
    detail::VideoEncoder& enc = choice->encoder;
    AVCodecContext* ectx = enc.ctx.get();

    auto videoSt = muxer->addStream(ectx);
    if (!videoSt) {
        return std::unexpected(videoSt.error());
    }
    AVStream* outVideo = *videoSt;
    outVideo->time_base = videoTb;
    outVideo->sample_aspect_ratio = params.sampleAspect;
    outVideo->codecpar->sample_aspect_ratio = params.sampleAspect;
    outVideo->codecpar->color_primaries = tags.primaries;
    outVideo->codecpar->color_trc = tags.transfer;
    outVideo->codecpar->color_space = tags.matrix;
    outVideo->codecpar->color_range = tags.range;
    outVideo->codecpar->chroma_location = tags.chroma;
    for (int i = 0; i < sp->nb_coded_side_data; ++i) { // display matrix (rotation), mastering metadata, ...
        const AVPacketSideData& sd = sp->coded_side_data[i];
        if (sd.type == AV_PKT_DATA_DISPLAYMATRIX || sd.type == AV_PKT_DATA_MASTERING_DISPLAY_METADATA ||
            sd.type == AV_PKT_DATA_CONTENT_LIGHT_LEVEL) {
            if (AVPacketSideData* copy = av_packet_side_data_new(&outVideo->codecpar->coded_side_data,
                                                                 &outVideo->codecpar->nb_coded_side_data, sd.type,
                                                                 sd.size, 0)) {
                std::memcpy(copy->data, sd.data, sd.size);
            }
        }
    }
    ProxyResult result;
    if (const std::string tc = timecodeOf(in.get(), srcVideo); !tc.empty()) {
        av_dict_set(&outVideo->metadata, "timecode", tc.c_str(), 0); // MOV writes a tmcd track
        result.timecode = true;
    }

    // ---- Audio: copied packet for packet, timestamps moved to the same origin.
    std::unordered_map<int, AVStream*> audioOut; // source stream index -> output stream
    if (settings.copyAudio) {
        for (unsigned i = 0; i < in->nb_streams; ++i) {
            const AVStream* st = in->streams[i];
            if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO ||
                avformat_query_codec(av_guess_format("mov", nullptr, nullptr), st->codecpar->codec_id,
                                     FF_COMPLIANCE_NORMAL) != 1) {
                continue; // not storable in MOV: left out (the timeline reads the original's audio anyway)
            }
            CodecContextPtr copy(avcodec_alloc_context3(nullptr));
            if (!copy || avcodec_parameters_to_context(copy.get(), st->codecpar) < 0) {
                return std::unexpected(Error{ErrorCode::EncoderError, "Cannot copy an audio stream"});
            }
            copy->codec_tag = 0;
            copy->time_base = st->time_base;
            auto added = muxer->addStream(copy.get());
            if (!added) {
                return std::unexpected(added.error());
            }
            (*added)->time_base = st->time_base;
            av_dict_copy(&(*added)->metadata, st->metadata, 0);
            audioOut.emplace(static_cast<int>(i), *added);
        }
    }
    result.audioStreams = static_cast<int>(audioOut.size());
    if (auto opened = muxer->open(); !opened) {
        return std::unexpected(opened.error());
    }

    PacketPtr audioPacket(av_packet_alloc());
    PacketPtr packet(av_packet_alloc());
    if (!audioPacket || !packet) {
        return std::unexpected(Error{ErrorCode::EncoderError, "Out of memory (packet)"});
    }
    bool audioPending = false;
    bool audioDone = audioOut.empty();
    // Writes the copied audio up to `untilSeconds` (relative to the origin) so the file stays interleaved.
    const auto pumpAudio = [&](double untilSeconds) -> std::expected<void, Error> {
        while (!audioDone) {
            if (!audioPending) {
                const int r = av_read_frame(in.get(), audioPacket.get());
                if (r == AVERROR_EOF) {
                    audioDone = true;
                    break;
                }
                if (r < 0) {
                    return std::unexpected(ffmpegError(ErrorCode::IoError, "Cannot read " + source, r));
                }
                const auto it = audioOut.find(audioPacket->stream_index);
                const std::int64_t stamp = audioPacket->pts != AV_NOPTS_VALUE ? audioPacket->pts : audioPacket->dts;
                if (it == audioOut.end() || stamp == AV_NOPTS_VALUE) {
                    av_packet_unref(audioPacket.get());
                    continue;
                }
                const AVRational tb = in->streams[audioPacket->stream_index]->time_base;
                const std::int64_t shift = ticksToUnits(origin, tb);
                if (audioPacket->pts != AV_NOPTS_VALUE) {
                    audioPacket->pts -= shift;
                }
                if (audioPacket->dts != AV_NOPTS_VALUE) {
                    audioPacket->dts -= shift;
                }
                audioPending = true;
            }
            const AVRational tb = in->streams[audioPacket->stream_index]->time_base;
            const std::int64_t stamp = audioPacket->pts != AV_NOPTS_VALUE ? audioPacket->pts : audioPacket->dts;
            if (static_cast<double>(stamp) * av_q2d(tb) > untilSeconds) {
                break;
            }
            AVStream* out = audioOut.at(audioPacket->stream_index);
            if (auto w = muxer->write(audioPacket.get(), tb, out); !w) {
                return std::unexpected(w.error());
            }
            audioPending = false;
        }
        return {};
    };

    // Encoders see a frame counter as pts; the source timestamps are put back on the packets.
    struct Stamp {
        std::int64_t pts = 0;
        std::int64_t duration = 0;
    };
    std::unordered_map<std::int64_t, Stamp> stamps;
    const auto drain = [&]() -> std::expected<void, Error> {
        for (;;) {
            const int r = avcodec_receive_packet(ectx, packet.get());
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
                return {};
            }
            if (r < 0) {
                return std::unexpected(ffmpegError(ErrorCode::EncoderError, "Cannot encode the proxy", r));
            }
            const auto it = stamps.find(packet->pts);
            if (it == stamps.end()) {
                av_packet_unref(packet.get());
                return std::unexpected(Error{ErrorCode::EncoderError, "The encoder reordered or dropped frames"});
            }
            packet->pts = it->second.pts;
            packet->dts = it->second.pts; // intra only: decode order is presentation order
            packet->duration = it->second.duration;
            stamps.erase(it);
            if (auto a = pumpAudio(static_cast<double>(packet->pts) * av_q2d(videoTb) + 0.5); !a) {
                return a;
            }
            if (auto w = muxer->write(packet.get(), videoTb, outVideo); !w) {
                return w;
            }
        }
    };

    // ---- Frames, in source order: the frame containing t, then the one after it (VFR-safe).
    const std::int64_t expected = std::max<std::int64_t>(info.frameCount, 0);
    Downscaler scaler;
    std::int64_t index = 0;
    std::optional<media::FrameHandle> pending = std::move(*first);
    Ticks t = firstFrame;
    for (;;) {
        if (control.stop.stop_requested() || (control.yield && !control.yield())) {
            return cancelled();
        }
        std::expected<media::FrameHandle, Error> frame =
            pending ? std::expected<media::FrameHandle, Error>(std::move(*pending)) : (*decoder)->frameAt(t);
        pending.reset();
        if (!frame) {
            if (frame.error().code == ErrorCode::NotFound) {
                break; // past the last frame
            }
            return std::unexpected(frame.error());
        }
        auto converted = scaler.convert(*frame, width, height, enc.inputFormat, tags.matrix, tags.range);
        if (!converted) {
            return std::unexpected(converted.error());
        }
        (*converted)->pts = index;
        (*converted)->color_primaries = tags.primaries;
        (*converted)->color_trc = tags.transfer;
        (*converted)->chroma_location = tags.chroma;
        const Ticks next = frame->pts + std::max(frame->duration, Ticks{1});
        const std::int64_t start = ticksToUnits(frame->pts, videoTb);
        stamps[index] = {start, std::max<std::int64_t>(1, ticksToUnits(next, videoTb) - start)};
        auto ready = detail::prepareForEncoder(enc, std::move(*converted));
        if (!ready) {
            return std::unexpected(ready.error());
        }
        if (const int r = avcodec_send_frame(ectx, ready->get()); r < 0) {
            return std::unexpected(ffmpegError(ErrorCode::EncoderError, "Cannot encode the proxy", r));
        }
        if (auto d = drain(); !d) {
            return std::unexpected(d.error());
        }
        ++index;
        if (control.onProgress) {
            control.onProgress(index, std::max(expected, index));
        }
        t = next;
    }
    if (index == 0) {
        return std::unexpected(Error{ErrorCode::DecoderError, "No frame could be decoded"});
    }
    if (const int r = avcodec_send_frame(ectx, nullptr); r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::EncoderError, "Cannot finish the proxy", r));
    }
    if (auto d = drain(); !d) {
        return std::unexpected(d.error());
    }
    if (auto a = pumpAudio(1e18); !a) {
        return std::unexpected(a.error());
    }
    if (auto f = muxer->finish(); !f) {
        return std::unexpected(f.error());
    }
    decoder->reset();

    // ---- Check: the proxy maps frame for frame onto the source.
    auto check = media::VideoDecoder::open(pathToUtf8(partial.path), -1,
                                           media::DecoderOptions{.allowHardware = false});
    if (!check) {
        return std::unexpected(Error{ErrorCode::EncoderError, "The written proxy cannot be read: " + check.error().message});
    }
    const VideoStreamInfo& written = (*check)->streamInfo();
    if ((*check)->firstFrameTime() != firstFrame) {
        return std::unexpected(Error{ErrorCode::InvalidArgument,
                                     std::format("The proxy starts at {} s, the source at {} s",
                                                 (*check)->firstFrameTime().toSeconds(), firstFrame.toSeconds())});
    }
    if (written.frameCount >= 0 && written.frameCount != index) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, std::format("The proxy has {} frames, {} were written",
                                                                             written.frameCount, index)});
    }
    check->reset();

    std::filesystem::rename(partial.path, output, ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot move the proxy into place: " + ec.message()});
    }
    partial.keep = true;
    result.file = output;
    result.encoder = enc.name;
    result.fallbacks = std::move(choice->fallbacks);
    result.width = width;
    result.height = height;
    result.frames = index;
    result.firstFrame = firstFrame;
    return result;
}

} // namespace wabcut::exporter
