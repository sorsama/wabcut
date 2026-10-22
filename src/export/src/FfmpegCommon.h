#pragma once

// Private FFmpeg helpers of the export module (RAII owners, error text). Never include from public headers.

#include <wabcut/model/Error.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/mastering_display_metadata.h>
#include <libswscale/swscale.h>
}

#include <format>
#include <memory>
#include <string>
#include <string_view>

namespace wabcut::media::detail {
/// Defined in wabcut_media (Log.cpp): routes av_log() to the media log sink instead of stderr. Idempotent.
void ensureFfmpegLogging();
} // namespace wabcut::media::detail

namespace wabcut::exporter::detail {

struct CodecContextDeleter {
    void operator()(AVCodecContext* p) const noexcept { avcodec_free_context(&p); }
};
struct FrameDeleter {
    void operator()(AVFrame* p) const noexcept { av_frame_free(&p); }
};
struct PacketDeleter {
    void operator()(AVPacket* p) const noexcept { av_packet_free(&p); }
};
struct SwsContextDeleter {
    void operator()(SwsContext* p) const noexcept { sws_free_context(&p); }
};
struct BufferRefDeleter {
    void operator()(AVBufferRef* p) const noexcept { av_buffer_unref(&p); }
};

using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;
using BufferRefPtr = std::unique_ptr<AVBufferRef, BufferRefDeleter>;

inline std::string avErrorString(int averror) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    if (av_strerror(averror, buf, sizeof buf) < 0) {
        return std::format("FFmpeg error {}", averror);
    }
    return buf;
}

/// Error with "<what>: <FFmpeg message>" text.
inline Error ffmpegError(ErrorCode code, std::string_view what, int averror) {
    return Error{code, std::format("{}: {}", what, avErrorString(averror))};
}

} // namespace wabcut::exporter::detail
