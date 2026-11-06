// Data tracks of a media file (DataStreams.h).

#include "FfmpegUtil.h"

#include <wabcut/media/DataStreams.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>

namespace wabcut::media {

using detail::ffmpegError;
using detail::FormatContextPtr;
using detail::openInput;

namespace {

/// The four characters of a codec tag (little endian in FFmpeg), printable ASCII only.
std::string tagText(std::uint32_t tag) {
    std::string out;
    for (int i = 0; i < 4; ++i) {
        const auto c = static_cast<char>((tag >> (8 * i)) & 0xFF);
        if (c < 0x20 || c > 0x7E) {
            return {};
        }
        out.push_back(c);
    }
    return out;
}

double seconds(std::int64_t ts, AVRational base) { return static_cast<double>(ts) * av_q2d(base); }

} // namespace

std::expected<std::vector<DataStreamInfo>, Error> listDataStreams(const std::string& path) {
    auto opened = openInput(path);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    const AVFormatContext* fmt = opened->get();
    std::vector<DataStreamInfo> out;
    for (unsigned i = 0; i < fmt->nb_streams && i < 256; ++i) {
        const AVStream* s = fmt->streams[i];
        if (!s->codecpar || s->codecpar->codec_type != AVMEDIA_TYPE_DATA) {
            continue;
        }
        DataStreamInfo info;
        info.index = static_cast<int>(i);
        info.tag = tagText(s->codecpar->codec_tag);
        if (const AVDictionaryEntry* handler = av_dict_get(s->metadata, "handler_name", nullptr, 0)) {
            info.handler.assign(handler->value, std::min<std::size_t>(std::char_traits<char>::length(handler->value), 128));
        }
        info.packets = s->nb_frames > 0 ? s->nb_frames : -1;
        out.push_back(std::move(info));
    }
    return out;
}

std::expected<void, Error> forEachDataPacket(const std::string& path, int streamIndex, const std::function<bool(const DataPacket&)>& each,
                                             const DataReadOptions& options) {
    auto opened = openInput(path);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    AVFormatContext* fmt = opened->get();
    if (streamIndex < 0 || static_cast<unsigned>(streamIndex) >= fmt->nb_streams ||
        fmt->streams[streamIndex]->codecpar->codec_type != AVMEDIA_TYPE_DATA) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "not a data stream"});
    }
    // Only this stream's packets are wanted: the demuxer skips the others' payloads (the MP4 reader works from its index).
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        fmt->streams[i]->discard = static_cast<int>(i) == streamIndex ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    }
    const AVStream* stream = fmt->streams[streamIndex];
    const double origin = detail::mediaOrigin(fmt).toSeconds();
    std::unique_ptr<AVPacket, detail::PacketDeleter> packet(av_packet_alloc());
    if (!packet) {
        return std::unexpected(Error{ErrorCode::Unknown, "out of memory"});
    }
    // A packet with no duration of its own lasts until the next one starts, so one packet is held back until the next arrives.
    std::optional<DataPacket> held;
    double lastGap = 0.0;
    std::size_t count = 0;
    std::size_t bytes = 0;
    bool stopped = false;
    const auto release = [&](DataPacket& p, const DataPacket* next) {
        if (!(p.duration > 0.0)) {
            p.duration = next && next->seconds > p.seconds ? next->seconds - p.seconds : lastGap;
        }
        lastGap = p.duration > 0.0 ? p.duration : lastGap;
        return each(p);
    };
    while (!stopped) {
        if (options.cancelled && options.cancelled()) {
            return std::unexpected(Error{ErrorCode::Cancelled, "cancelled"});
        }
        const int r = av_read_frame(fmt, packet.get());
        if (r == AVERROR_EOF) {
            break;
        }
        if (r < 0) {
            if (count == 0) {
                return std::unexpected(ffmpegError(ErrorCode::IoError, "Cannot read the data track of " + path, r));
            }
            break; // a damaged tail: what was read stands
        }
        if (packet->stream_index == streamIndex && packet->size > 0) {
            if (count >= options.maxPackets || bytes + static_cast<std::size_t>(packet->size) > options.maxBytes) {
                av_packet_unref(packet.get());
                break;
            }
            const std::int64_t ts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
            DataPacket p;
            p.seconds = ts != AV_NOPTS_VALUE ? seconds(ts, stream->time_base) - origin : (held ? held->seconds + lastGap : 0.0);
            p.duration = packet->duration > 0 ? seconds(packet->duration, stream->time_base) : 0.0;
            p.bytes.assign(packet->data, packet->data + packet->size);
            bytes += p.bytes.size();
            ++count;
            if (held && !release(*held, &p)) {
                stopped = true;
            }
            held = std::move(p);
        }
        av_packet_unref(packet.get());
        if (options.firstPackets > 0 && count >= options.firstPackets) {
            break;
        }
    }
    if (held && !stopped) {
        release(*held, nullptr);
    }
    return {};
}

std::expected<std::vector<DataPacket>, Error> readDataPackets(const std::string& path, int streamIndex, const DataReadOptions& options) {
    std::vector<DataPacket> out;
    // The bytes of every packet are kept here: the options' caps bound the copy (maxBytes counts what is read).
    const auto read = forEachDataPacket(
        path, streamIndex,
        [&out](const DataPacket& p) {
            out.push_back(p);
            return true;
        },
        options);
    if (!read) {
        return std::unexpected(read.error());
    }
    return out;
}

} // namespace wabcut::media
