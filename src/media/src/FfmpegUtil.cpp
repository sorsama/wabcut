#include "FfmpegUtil.h"

#include <wabcut/media/StreamTime.h>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

#include <cerrno>
#include <system_error>

namespace wabcut::media::detail {

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
    if (utf8Path.empty() || !std::filesystem::is_regular_file(pathFromUtf8(utf8Path), ec)) {
        return std::unexpected(Error{ErrorCode::NotFound, "File not found: " + utf8Path});
    }
    AVFormatContext* raw = nullptr;
    // FFmpeg's file protocol accepts UTF-8 paths on Windows (it converts them to UTF-16 itself).
    int r = avformat_open_input(&raw, utf8Path.c_str(), nullptr, nullptr);
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
