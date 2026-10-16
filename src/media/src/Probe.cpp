#include "FfmpegUtil.h"

#include <wabcut/media/Probe.h>
#include <wabcut/platform/FileTime.h>

extern "C" {
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace wabcut::media {

namespace {

bool isImageDemuxer(std::string_view name) {
    return name == "image2" || name == "image2pipe" || name.ends_with("_pipe");
}

} // namespace

std::expected<MediaInfo, Error> probe(const std::string& path) {
    auto opened = detail::openInput(path);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    AVFormatContext* fmt = opened->get();

    MediaInfo info;
    info.path = path;
    info.container = fmt->iformat && fmt->iformat->name ? fmt->iformat->name : "";
    if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0) {
        info.duration = detail::toTicks(fmt->duration, AVRational{1, AV_TIME_BASE}).value_or(Ticks{});
    }

    const std::filesystem::path fsPath = detail::pathFromUtf8(path);
    std::error_code ec;
    if (const auto size = std::filesystem::file_size(fsPath, ec); !ec) {
        info.fileSize = static_cast<std::int64_t>(size);
    }
    if (const auto mtime = std::filesystem::last_write_time(fsPath, ec); !ec) {
        const auto sys = platform::toSystemClock(mtime);
        info.modifiedTime = std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
    }

    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* st = fmt->streams[i];
        switch (st->codecpar->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            if (!detail::isAttachedPicture(st)) {
                info.video.push_back(detail::describeVideoStream(fmt, st));
            }
            break;
        case AVMEDIA_TYPE_AUDIO:
            info.audio.push_back(detail::describeAudioStream(fmt, st));
            break;
        default:
            break;
        }
    }

    if (info.video.empty() && info.audio.empty()) {
        return std::unexpected(Error{ErrorCode::Unsupported, "No audio or video streams in " + path});
    }
    info.isStillImage = info.audio.empty() && info.video.size() == 1 &&
                        (isImageDemuxer(info.container) || info.video.front().frameCount == 1);
    if (info.duration.value == 0) {
        for (const auto& v : info.video) {
            info.duration = std::max(info.duration, v.duration);
        }
        for (const auto& a : info.audio) {
            info.duration = std::max(info.duration, a.duration);
        }
    }
    return info;
}

} // namespace wabcut::media
