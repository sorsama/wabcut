#include <wabcut/export/TrimmedCopy.h>

#include "Encode.h"
#include "MediaTime.h"

#include <wabcut/media/VideoDecoder.h>
#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <cctype>
#include <deque>
#include <iterator>
#include <optional>
#include <ranges>
#include <system_error>
#include <vector>

namespace wabcut::exporter {

namespace {

using detail::CodecContextPtr;
using detail::ffmpegError;
using detail::mediaOriginOf;
using detail::PacketPtr;
using detail::ticksToUnits;
using detail::unitsToTicks;

struct FormatInputDeleter {
    void operator()(AVFormatContext* p) const noexcept { avformat_close_input(&p); }
};
using FormatInputPtr = std::unique_ptr<AVFormatContext, FormatInputDeleter>;

std::optional<Container> containerOf(const std::filesystem::path& file) {
    std::string ext = pathToUtf8(file.extension());
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".mp4" || ext == ".m4v") {
        return Container::Mp4;
    }
    if (ext == ".mov") {
        return Container::Mov;
    }
    if (ext == ".mkv") {
        return Container::Mkv;
    }
    return std::nullopt;
}

std::expected<FormatInputPtr, Error> openInput(const std::string& source) {
    AVFormatContext* raw = nullptr;
    if (const int r = avformat_open_input(&raw, source.c_str(), nullptr, nullptr); r < 0) {
        return std::unexpected(ffmpegError(r == AVERROR(ENOENT) ? ErrorCode::NotFound : ErrorCode::IoError,
                                           "Cannot open " + source, r));
    }
    FormatInputPtr in(raw);
    if (const int r = avformat_find_stream_info(in.get(), nullptr); r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::IoError, "Cannot read " + source, r));
    }
    return in;
}

bool copied(const AVStream* st) {
    const AVMediaType type = st->codecpar->codec_type;
    return (type == AVMEDIA_TYPE_VIDEO && !(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) || type == AVMEDIA_TYPE_AUDIO;
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

bool canTrimCopy(const std::filesystem::path& file) {
    return containerOf(file).has_value();
}

std::expected<TrimmedCopy, Error> trimmedCopy(const std::string& source, const std::filesystem::path& output, TimeRange keep,
                                              std::stop_token stop, const std::function<void(double)>& onProgress) {
    media::detail::ensureFfmpegLogging();
    const auto container = containerOf(output);
    if (!container || !containerOf(pathFromUtf8(source))) {
        return std::unexpected(Error{ErrorCode::Unsupported, "Trimmed copies are made of MP4, MOV and MKV files"});
    }
    auto opened = openInput(source);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    FormatInputPtr in = std::move(*opened);
    const Ticks origin = mediaOriginOf(in.get());
    int ref = av_find_best_stream(in.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (ref >= 0 && !copied(in->streams[ref])) {
        ref = -1;
    }
    const bool refIsVideo = ref >= 0;
    if (ref < 0) {
        ref = av_find_best_stream(in.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    }
    if (ref < 0) {
        return std::unexpected(Error{ErrorCode::Unsupported, "No audio or video to copy"});
    }
    const AVRational refTb = in->streams[ref]->time_base;
    const Ticks startAbs = origin + keep.start;
    const Ticks endAbs = origin + keep.end();
    const Ticks margin{kTicksPerSecond / 4}; // other streams run on a little past the kept range

    std::error_code ec;
    std::filesystem::create_directories(output.parent_path(), ec);
    PartialFile partial{std::filesystem::path(output).concat(".partial")};
    auto muxer = detail::Muxer::create(*container, partial.path);
    if (!muxer) {
        return std::unexpected(muxer.error());
    }
    std::vector<AVStream*> outOf(in->nb_streams, nullptr);
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        const AVStream* st = in->streams[i];
        if (!copied(st)) {
            continue;
        }
        CodecContextPtr copy(avcodec_alloc_context3(nullptr));
        if (!copy || avcodec_parameters_to_context(copy.get(), st->codecpar) < 0) {
            return std::unexpected(Error{ErrorCode::EncoderError, "Cannot copy a stream"});
        }
        copy->codec_tag = 0;
        copy->time_base = st->time_base;
        copy->framerate = st->avg_frame_rate;
        auto added = muxer->addStream(copy.get());
        if (!added) {
            return std::unexpected(added.error());
        }
        AVStream* out = *added;
        out->time_base = st->time_base;
        out->avg_frame_rate = st->avg_frame_rate;
        out->sample_aspect_ratio = st->sample_aspect_ratio;
        out->disposition = st->disposition;
        av_dict_copy(&out->metadata, st->metadata, 0);
        outOf[i] = out;
    }
    if (auto ok = muxer->open(); !ok) {
        return std::unexpected(ok.error());
    }

    // Find the reference keyframe at or before the range: seek there; if the demuxer lands after it, step back.
    PacketPtr packet(av_packet_alloc());
    if (!packet) {
        return std::unexpected(Error{ErrorCode::EncoderError, "Out of memory (packet)"});
    }
    std::deque<PacketPtr> early; // other streams' packets read before the keyframe
    std::optional<Ticks> keyframe;
    const Ticks backoffs[] = {Ticks{}, Ticks::fromSeconds(5), Ticks::fromSeconds(30), Ticks::fromSeconds(1'000'000)};
    for (const Ticks back : backoffs) {
        early.clear();
        const Ticks target = startAbs - back;
        int r = 0;
        if (target <= origin) {
            r = avformat_seek_file(in.get(), -1, INT64_MIN, 0, 0, 0);
            if (r < 0) {
                r = av_seek_frame(in.get(), ref, INT64_MIN, AVSEEK_FLAG_BACKWARD);
            }
        } else {
            r = av_seek_frame(in.get(), ref, ticksToUnits(target, refTb), AVSEEK_FLAG_BACKWARD);
        }
        if (r < 0) {
            continue;
        }
        while (true) {
            if (stop.stop_requested()) {
                return std::unexpected(Error{ErrorCode::Cancelled, "Collect Files cancelled"});
            }
            if (av_read_frame(in.get(), packet.get()) < 0) {
                break;
            }
            const int si = packet->stream_index;
            const std::int64_t ts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
            if (si != ref) {
                if (outOf[static_cast<std::size_t>(si)] && ts != AV_NOPTS_VALUE) {
                    early.emplace_back(av_packet_clone(packet.get()));
                }
                av_packet_unref(packet.get());
                continue;
            }
            if (ts == AV_NOPTS_VALUE || (refIsVideo && !(packet->flags & AV_PKT_FLAG_KEY))) {
                av_packet_unref(packet.get());
                continue;
            }
            keyframe = unitsToTicks(ts, refTb);
            break; // `packet` holds the keyframe
        }
        if (keyframe && *keyframe <= startAbs) {
            break;
        }
        if (keyframe && back == backoffs[std::size(backoffs) - 1]) {
            break; // the file starts after the range: copy from its beginning
        }
        av_packet_unref(packet.get());
        keyframe.reset();
    }
    if (!keyframe) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "Nothing to copy in that range"});
    }
    const Ticks k = *keyframe;

    const auto write = [&](AVPacket* p) -> std::expected<void, Error> {
        const AVStream* st = in->streams[p->stream_index];
        const std::int64_t offset = ticksToUnits(k, st->time_base);
        if (p->pts != AV_NOPTS_VALUE) {
            p->pts -= offset;
        }
        if (p->dts != AV_NOPTS_VALUE) {
            p->dts -= offset;
        }
        return muxer->write(p, st->time_base, outOf[static_cast<std::size_t>(p->stream_index)]);
    };
    if (auto w = write(packet.get()); !w) {
        return std::unexpected(w.error());
    }
    for (PacketPtr& e : early) {
        const AVStream* st = in->streams[e->stream_index];
        if (unitsToTicks(e->pts != AV_NOPTS_VALUE ? e->pts : e->dts, st->time_base) >= k) {
            if (auto w = write(e.get()); !w) {
                return std::unexpected(w.error());
            }
        }
    }
    early.clear();

    // Copy on: the reference stream up to the first keyframe at or after the range's end, the others a little past
    // it.
    std::vector<bool> done(in->nb_streams, false);
    bool refDone = false;
    while (true) {
        if (stop.stop_requested()) {
            return std::unexpected(Error{ErrorCode::Cancelled, "Collect Files cancelled"});
        }
        if (av_read_frame(in.get(), packet.get()) < 0) {
            break;
        }
        const int si = packet->stream_index;
        const auto index = static_cast<std::size_t>(si);
        const std::int64_t ts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
        if (!outOf[index] || done[index] || ts == AV_NOPTS_VALUE) {
            av_packet_unref(packet.get());
            continue;
        }
        const Ticks at = unitsToTicks(ts, in->streams[si]->time_base);
        if (si == ref) {
            if (at >= endAbs && (!refIsVideo || (packet->flags & AV_PKT_FLAG_KEY))) {
                refDone = true;
                done[index] = true;
                av_packet_unref(packet.get());
            } else {
                if (onProgress && endAbs > k) {
                    onProgress(std::clamp((at - k).toSeconds() / (endAbs - k).toSeconds(), 0.0, 1.0));
                }
                if (auto w = write(packet.get()); !w) {
                    return std::unexpected(w.error());
                }
            }
        } else if (at >= endAbs + margin) {
            done[index] = true;
            av_packet_unref(packet.get());
        } else if (at >= k) {
            if (auto w = write(packet.get()); !w) {
                return std::unexpected(w.error());
            }
        } else {
            av_packet_unref(packet.get());
        }
        if (refDone && std::ranges::all_of(std::views::iota(0u, in->nb_streams),
                                           [&](unsigned i) { return !outOf[i] || done[i]; })) {
            break;
        }
    }
    if (auto f = muxer->finish(); !f) {
        return std::unexpected(f.error());
    }

    // The shift, measured on the written file: where the keyframe's frame (or the first sample) starts now.
    TrimmedCopy result;
    const Ticks oldStart = k - origin;
    Ticks newStart{};
    if (refIsVideo) {
        auto decoder = media::VideoDecoder::open(pathToUtf8(partial.path), -1, media::DecoderOptions{.allowHardware = false});
        if (!decoder) {
            return std::unexpected(Error{ErrorCode::EncoderError, "The trimmed copy cannot be read: " + decoder.error().message});
        }
        newStart = (*decoder)->firstFrameTime();
    } else {
        auto check = openInput(pathToUtf8(partial.path));
        if (!check) {
            return std::unexpected(check.error());
        }
        const Ticks newOrigin = mediaOriginOf(check->get());
        for (unsigned i = 0; i < (*check)->nb_streams; ++i) {
            const AVStream* st = (*check)->streams[i];
            if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && st->start_time != AV_NOPTS_VALUE) {
                newStart = unitsToTicks(st->start_time, st->time_base) - newOrigin;
                break;
            }
        }
    }
    {
        auto check = openInput(pathToUtf8(partial.path));
        if (check && (*check)->duration != AV_NOPTS_VALUE) {
            result.duration = Ticks{av_rescale((*check)->duration, kTicksPerSecond, AV_TIME_BASE)};
        }
    }
    result.shift = oldStart - newStart;
    std::filesystem::rename(partial.path, output, ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot move the trimmed copy into place: " + ec.message()});
    }
    partial.keep = true;
    result.file = output;
    return result;
}

} // namespace wabcut::exporter
