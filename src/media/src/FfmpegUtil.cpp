#include "FfmpegUtil.h"

#include <wabcut/media/Psd.h>
#include <wabcut/media/RawHelper.h>
#include <wabcut/media/StreamTime.h>
#include <wabcut/model/ImageSequence.h>
#include <wabcut/model/LayeredFile.h>
#include <wabcut/model/RawSourceSettings.h>
#include <wabcut/model/SpannedClip.h>

extern "C" {
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <system_error>

namespace wabcut::media::detail {

namespace {

/// What an input reads instead of a file (custom AVIOContext): bytes in memory, or a RAW helper's frames.
struct CustomInput {
    virtual ~CustomInput() = default;
    virtual std::int64_t size() const = 0;
    /// Up to `size` bytes at `pos` into `buf`; 0 at the end, < 0 on errors.
    virtual int readAt(std::int64_t pos, std::uint8_t* buf, int size) = 0;
    std::int64_t pos = 0;
};

struct MemoryInput final : CustomInput {
    explicit MemoryInput(std::string bytes) : data(std::move(bytes)) {}
    std::string data;
    std::int64_t size() const override { return static_cast<std::int64_t>(data.size()); }
    int readAt(std::int64_t at, std::uint8_t* buf, int want) override {
        const std::size_t n = std::min(static_cast<std::size_t>(want), data.size() - static_cast<std::size_t>(at));
        std::memcpy(buf, data.data() + at, n);
        return static_cast<int>(n);
    }
};

/// A RAW helper's frames as one file of frames back to back (media/RawHelper.h): frame k at k * frame bytes.
struct HelperInput final : CustomInput {
    std::unique_ptr<RawHelper> helper;
    std::vector<std::uint8_t> frame;
    std::int64_t cached = -1;
    std::int64_t size() const override {
        return static_cast<std::int64_t>(helper->frameBytes()) * helper->header().frames;
    }
    int readAt(std::int64_t at, std::uint8_t* buf, int want) override {
        const auto bytes = static_cast<std::int64_t>(helper->frameBytes());
        const std::int64_t index = at / bytes;
        if (index != cached) {
            if (!helper->readFrame(index, frame.data())) {
                cached = -1;
                return AVERROR(EIO);
            }
            cached = index;
        }
        const std::int64_t offset = at - index * bytes;
        const auto n = static_cast<int>(std::min<std::int64_t>(want, bytes - offset));
        std::memcpy(buf, frame.data() + offset, static_cast<std::size_t>(n));
        return n;
    }
};

int readCustom(void* opaque, std::uint8_t* buf, int size) {
    auto* in = static_cast<CustomInput*>(opaque);
    if (in->pos >= in->size()) {
        return AVERROR_EOF;
    }
    const int n = in->readAt(in->pos, buf, size);
    if (n > 0) {
        in->pos += n;
    }
    return n == 0 ? AVERROR_EOF : n;
}

std::int64_t seekCustom(void* opaque, std::int64_t offset, int whence) {
    auto* in = static_cast<CustomInput*>(opaque);
    const std::int64_t size = in->size();
    if ((whence & AVSEEK_SIZE) != 0) {
        return size;
    }
    std::int64_t target = offset;
    switch (whence & ~AVSEEK_FORCE) {
    case SEEK_CUR:
        target += in->pos;
        break;
    case SEEK_END:
        target += size;
        break;
    default:
        break;
    }
    if (target < 0 || target > size) {
        return AVERROR(EINVAL);
    }
    in->pos = target;
    return target;
}

/// Opens `input` with the demuxer `format` (and `options`) through a custom AVIOContext; the context owns `input`
/// from then on (FormatContextDeleter frees it).
int openCustomInput(AVFormatContext** out, std::unique_ptr<CustomInput> owned, const AVInputFormat* format,
                    AVDictionary** options) {
    constexpr int kBuffer = 64 * 1024;
    auto* buffer = static_cast<std::uint8_t*>(av_malloc(kBuffer));
    if (!buffer) {
        return AVERROR(ENOMEM);
    }
    CustomInput* input = owned.release();
    AVIOContext* pb = avio_alloc_context(buffer, kBuffer, 0, input, &readCustom, nullptr, &seekCustom);
    if (!pb) {
        av_free(buffer);
        delete input;
        return AVERROR(ENOMEM);
    }
    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt) {
        av_freep(&pb->buffer);
        avio_context_free(&pb);
        delete input;
        return AVERROR(ENOMEM);
    }
    fmt->pb = pb;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    const int r = avformat_open_input(&fmt, "", format, options); // frees fmt (not pb) when it fails
    if (r < 0) {
        av_freep(&pb->buffer);
        avio_context_free(&pb);
        delete input;
        return r;
    }
    *out = fmt;
    return 0;
}

/// FFmpeg's concat script of a spanned clip: each part with the duration it plays, so the demuxer can seek.
std::string concatScript(const SpannedClipSpec& spec) {
    std::string out = "ffconcat version 1.0\n";
    for (std::size_t i = 0; i < spec.parts.size(); ++i) {
        std::string file = pathToUtf8(spec.partPath(i));
        std::ranges::replace(file, '\\', '/');
        std::string quoted = "'";
        for (const char c : file) {
            quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
        }
        quoted += "'";
        out += "file " + quoted + "\n";
        out += std::format("duration {}.{:06}\n", spec.parts[i].microseconds / 1'000'000,
                           spec.parts[i].microseconds % 1'000'000);
    }
    return out;
}

} // namespace

void FormatContextDeleter::operator()(AVFormatContext* p) const noexcept {
    AVIOContext* custom = p && (p->flags & AVFMT_FLAG_CUSTOM_IO) != 0 ? p->pb : nullptr;
    avformat_close_input(&p);
    if (custom) {
        delete static_cast<CustomInput*>(custom->opaque);
        av_freep(&custom->buffer);
        avio_context_free(&custom);
    }
}

std::shared_ptr<AVFrame> shareFrame(FramePtr frame) {
    return std::shared_ptr<AVFrame>(frame.release(), FrameDeleter{});
}

std::string avErrorString(int averror) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    if (av_strerror(averror, buf, sizeof buf) < 0) {
        return std::format("FFmpeg error {}", averror);
    }
    return buf;
}

Error ffmpegError(ErrorCode code, std::string_view what, int averror) {
    return Error{code, std::format("{}: {}", what, avErrorString(averror))};
}

std::filesystem::path pathFromUtf8(std::string_view utf8) {
    return std::filesystem::path(
        std::u8string_view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string pathToUtf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::optional<Ticks> toTicks(std::int64_t ts, AVRational timeBase) {
    return streamToTicks(ts, TimeBase{timeBase.num, timeBase.den});
}

std::optional<std::int64_t> toStreamTime(Ticks t, AVRational timeBase) {
    return ticksToStream(t, TimeBase{timeBase.num, timeBase.den});
}

AVPixelFormat toAvPixelFormat(PixelFormat format) {
    switch (format) {
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

bool isAttachedPicture(const AVStream* stream) {
    return (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
}

Ticks mediaOrigin(const AVFormatContext* fmt) {
    bool found = false;
    Ticks origin{};
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream* st = fmt->streams[i];
        const AVMediaType type = st->codecpar->codec_type;
        if ((type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO) || isAttachedPicture(st) ||
            st->start_time == AV_NOPTS_VALUE) {
            continue;
        }
        const auto t = toTicks(st->start_time, st->time_base);
        if (t && (!found || *t < origin)) {
            origin = *t;
            found = true;
        }
    }
    if (!found && fmt->start_time != AV_NOPTS_VALUE) {
        origin = toTicks(fmt->start_time, AVRational{1, AV_TIME_BASE}).value_or(Ticks{});
    }
    return origin;
}

std::expected<FormatContextPtr, Error> openInput(const std::string& utf8Path) {
    ensureFfmpegLogging();
    std::error_code ec;
    // 0.2: an image sequence's path (ImageSequence.h) opens its numbered stills with the image2 demuxer, from its first
    // number at its frame rate; its first frame stands for it in the existence check.
    const std::optional<ImageSequenceSpec> sequence = parseImageSequencePath(utf8Path);
    // 0.2: a spanned clip (SpannedClip.h) plays its parts through the concat demuxer; a layered file's item
    // (LayeredFile.h) is its layers composited into one picture, read by the PAM demuxer from memory.
    const std::optional<SpannedClipSpec> spanned = sequence ? std::nullopt : parseSpannedClipPath(utf8Path);
    const std::optional<LayeredFileSpec> layered = sequence || spanned ? std::nullopt : parseLayeredFilePath(utf8Path);
    // 0.2: a RAW file's Source Settings (RawSourceSettings.h) are the decoder's to apply: the file is the path before them.
    const std::string file(withoutRawSourceSettings(utf8Path));
    const std::filesystem::path checked = sequence  ? sequence->firstFramePath()
                                          : spanned ? spanned->firstPath()
                                          : layered ? layered->file
                                                    : pathFromUtf8(file);
    if (utf8Path.empty() || !std::filesystem::is_regular_file(checked, ec)) {
        return std::unexpected(Error{ErrorCode::NotFound, "File not found: " + utf8Path});
    }
    AVFormatContext* raw = nullptr;
    int r = 0;
    std::string helperTransfer; // a RAW helper's frames: the transfer its header names
    if (sequence) {
        const AVInputFormat* image2 = av_find_input_format("image2");
        if (!image2) {
            return std::unexpected(Error{ErrorCode::Unsupported, "This FFmpeg build cannot read image sequences"});
        }
        AVDictionary* options = nullptr;
        av_dict_set(&options, "pattern_type", "sequence", 0);
        av_dict_set(&options, "start_number", std::to_string(sequence->first).c_str(), 0);
        av_dict_set(&options, "start_number_range", "1", 0);
        av_dict_set(&options, "framerate",
                    std::format("{}/{}", sequence->frameRate.num, sequence->frameRate.den).c_str(), 0);
        const std::string pattern = imageSequencePattern(*sequence);
        r = avformat_open_input(&raw, pattern.c_str(), image2, &options);
        av_dict_free(&options);
    } else if (spanned) {
        for (std::size_t i = 1; i < spanned->parts.size(); ++i) {
            if (!std::filesystem::is_regular_file(spanned->partPath(i), ec)) {
                return std::unexpected(
                    Error{ErrorCode::NotFound, "Part of a spanned clip not found: " + pathToUtf8(spanned->partPath(i))});
            }
        }
        const AVInputFormat* concat = av_find_input_format("concat");
        if (!concat) {
            return std::unexpected(Error{ErrorCode::Unsupported, "This FFmpeg build cannot read spanned clips"});
        }
        AVDictionary* options = nullptr;
        av_dict_set(&options, "safe", "0", 0); // absolute paths
        r = openCustomInput(&raw, std::make_unique<MemoryInput>(concatScript(*spanned)), concat, &options);
        av_dict_free(&options);
    } else if (layered) {
        auto picture = compositePsd(layered->file, layered->layers, layered->documentSize);
        if (!picture) {
            return std::unexpected(picture.error());
        }
        const AVInputFormat* pam = av_find_input_format("pam_pipe");
        if (!pam) {
            return std::unexpected(Error{ErrorCode::Unsupported, "This FFmpeg build cannot read layered files"});
        }
        r = openCustomInput(&raw, std::make_unique<MemoryInput>(toPam(*picture)), pam, nullptr);
    } else if (const auto helper = rawHelperFor(pathFromUtf8(file))) {
        // 0.2 camera RAW through a helper built with the vendor's SDK (RawHelper.h): its frames as rawvideo.
        auto started = RawHelper::start(*helper, pathFromUtf8(file));
        if (!started) {
            return std::unexpected(started.error());
        }
        const RawHelperHeader header = (*started)->header();
        const AVPixelFormat pixFmt = av_get_pix_fmt(header.pixelFormat.c_str());
        const int frameBytes = pixFmt == AV_PIX_FMT_NONE ? -1 : av_image_get_buffer_size(pixFmt, header.width, header.height, 1);
        const AVInputFormat* rawvideo = av_find_input_format("rawvideo");
        if (frameBytes <= 0 || !rawvideo) {
            return std::unexpected(Error{ErrorCode::Unsupported, "The RAW helper's pixel format is not supported"});
        }
        (*started)->setFrameBytes(static_cast<std::size_t>(frameBytes));
        auto input = std::make_unique<HelperInput>();
        input->frame.resize(static_cast<std::size_t>(frameBytes));
        input->helper = std::move(*started);
        AVDictionary* options = nullptr;
        av_dict_set(&options, "video_size", std::format("{}x{}", header.width, header.height).c_str(), 0);
        av_dict_set(&options, "pixel_format", header.pixelFormat.c_str(), 0);
        av_dict_set(&options, "framerate", std::format("{}/{}", header.rateNum, header.rateDen).c_str(), 0);
        r = openCustomInput(&raw, std::move(input), rawvideo, &options);
        av_dict_free(&options);
        if (r >= 0) {
            helperTransfer = header.colorTransfer;
        }
    } else {
        // FFmpeg's file protocol accepts UTF-8 paths on Windows (it converts them to UTF-16 itself).
        r = avformat_open_input(&raw, file.c_str(), nullptr, nullptr);
    }
    if (r < 0) {
        const ErrorCode code = r == AVERROR(ENOENT)             ? ErrorCode::NotFound
                               : r == AVERROR_INVALIDDATA       ? ErrorCode::Unsupported
                               : r == AVERROR_DEMUXER_NOT_FOUND ? ErrorCode::Unsupported
                                                                : ErrorCode::IoError;
        return std::unexpected(ffmpegError(code, "Cannot open " + utf8Path, r));
    }
    FormatContextPtr fmt(raw);
    r = avformat_find_stream_info(fmt.get(), nullptr);
    if (r < 0) {
        return std::unexpected(
            ffmpegError(ErrorCode::Unsupported, "Cannot read stream info of " + utf8Path, r));
    }
    if (!helperTransfer.empty() && fmt->nb_streams == 1) {
        AVCodecParameters* par = fmt->streams[0]->codecpar;
        par->color_trc = helperTransfer == "linear" ? AVCOL_TRC_LINEAR
                         : helperTransfer == "srgb" ? AVCOL_TRC_IEC61966_2_1
                                                    : AVCOL_TRC_BT709;
        par->color_primaries = AVCOL_PRI_BT709;
    }
    if (spanned) {
        // The concat demuxer takes each stream's start from its first packet: an AAC track's priming packet (-1024)
        // would move the media origin (mediaOrigin) before the first picture. The first part's own starts (its edit
        // list already applied), less the part's container start the demuxer subtracts from its packets (MPEG-TS
        // starts near 1.4 s), keep a spanned clip's source time the same as that file's.
        AVFormatContext* first = nullptr;
        const std::string firstPath = pathToUtf8(spanned->firstPath());
        if (avformat_open_input(&first, firstPath.c_str(), nullptr, nullptr) >= 0) {
            FormatContextPtr part(first);
            if (avformat_find_stream_info(part.get(), nullptr) >= 0) {
                const std::int64_t partStart = part->start_time == AV_NOPTS_VALUE ? 0 : part->start_time;
                for (unsigned i = 0; i < fmt->nb_streams && i < part->nb_streams; ++i) {
                    if (part->streams[i]->start_time != AV_NOPTS_VALUE) {
                        const std::int64_t start = av_rescale_q(part->streams[i]->start_time, part->streams[i]->time_base,
                                                                AVRational{1, AV_TIME_BASE}) -
                                                   partStart;
                        fmt->streams[i]->start_time =
                            av_rescale_q(start, AVRational{1, AV_TIME_BASE}, fmt->streams[i]->time_base);
                    }
                }
                fmt->start_time = 0;
            }
        }
    }
    return fmt;
}

DecodeLoop::DecodeLoop() : m_packet(av_packet_alloc()) {}

void DecodeLoop::reset() {
    av_packet_unref(m_packet.get());
    m_demuxEof = false;
    m_codecEof = false;
    m_packetHeld = false;
    m_lastPacketDts = AV_NOPTS_VALUE;
}

std::expected<bool, Error> DecodeLoop::receive(AVFormatContext* fmt, AVCodecContext* ctx, int streamIndex,
                                               AVFrame* out, std::string_view source) {
    for (;;) {
        if (m_codecEof) {
            return false;
        }
        int r = avcodec_receive_frame(ctx, out);
        if (r == 0) {
            return true;
        }
        if (r == AVERROR_EOF) {
            m_codecEof = true;
            return false;
        }
        if (r != AVERROR(EAGAIN)) {
            return std::unexpected(ffmpegError(ErrorCode::DecoderError, "Decoding failed", r));
        }
        if (m_demuxEof) {
            m_codecEof = true; // defensive: a draining decoder must not ask for more input
            return false;
        }
        if (!m_packetHeld) {
            r = av_read_frame(fmt, m_packet.get());
            if (r == AVERROR_EOF || (r < 0 && fmt->pb && avio_feof(fmt->pb))) {
                avcodec_send_packet(ctx, nullptr); // enter draining mode
                m_demuxEof = true;
                continue;
            }
            if (r < 0) {
                return std::unexpected(
                    ffmpegError(ErrorCode::IoError, std::format("Reading {} failed", source), r));
            }
            if (m_packet->stream_index != streamIndex) {
                av_packet_unref(m_packet.get());
                continue;
            }
        }
        r = avcodec_send_packet(ctx, m_packet.get());
        if (r == AVERROR(EAGAIN)) {
            m_packetHeld = true; // the codec wants its output drained first; resend after the next receive
            continue;
        }
        m_packetHeld = false;
        if (m_packet->dts != AV_NOPTS_VALUE) {
            m_lastPacketDts = m_packet->dts;
        }
        av_packet_unref(m_packet.get());
        if (r == AVERROR_INVALIDDATA) {
            log(LogLevel::Warning, "{}: skipping a corrupt packet", source);
        } else if (r < 0) {
            return std::unexpected(ffmpegError(ErrorCode::DecoderError, "Decoding failed", r));
        }
    }
}

std::expected<AVStream*, Error> selectStream(AVFormatContext* fmt, AVMediaType type, int streamIndex) {
    const char* typeName = av_get_media_type_string(type);
    const auto usable = [&](const AVStream* st) {
        return st->codecpar->codec_type == type && !(type == AVMEDIA_TYPE_VIDEO && isAttachedPicture(st));
    };
    if (streamIndex < 0) {
        const int best = av_find_best_stream(fmt, type, -1, -1, nullptr, 0);
        if (best >= 0 && usable(fmt->streams[best])) {
            return fmt->streams[best];
        }
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            if (usable(fmt->streams[i])) {
                return fmt->streams[i];
            }
        }
        return std::unexpected(Error{ErrorCode::NotFound, std::format("No {} stream in the file", typeName)});
    }
    if (static_cast<unsigned>(streamIndex) >= fmt->nb_streams) {
        return std::unexpected(
            Error{ErrorCode::InvalidArgument, std::format("Stream {} does not exist", streamIndex)});
    }
    AVStream* st = fmt->streams[streamIndex];
    if (!usable(st)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument,
                                     std::format("Stream {} is not a {} stream", streamIndex, typeName)});
    }
    return st;
}

} // namespace wabcut::media::detail
