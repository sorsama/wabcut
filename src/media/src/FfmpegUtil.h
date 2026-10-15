#pragma once

// Private FFmpeg helpers shared by the media implementation: RAII ownership, error mapping, exact time
// conversion, input opening and logging. Never include this from a public header.

#include <wabcut/media/FrameHandle.h>
#include <wabcut/media/Log.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/MediaInfo.h>
#include <wabcut/model/Time.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace wabcut::media::detail {

// ---- RAII
// ----------------------------------------------------------------------------------------------------

struct FormatContextDeleter {
    void operator()(AVFormatContext* p) const noexcept { avformat_close_input(&p); }
};
struct CodecContextDeleter {
    void operator()(AVCodecContext* p) const noexcept { avcodec_free_context(&p); }
};
struct FrameDeleter {
    void operator()(AVFrame* p) const noexcept { av_frame_free(&p); }
};
struct PacketDeleter {
    void operator()(AVPacket* p) const noexcept { av_packet_free(&p); }
};
struct SwrContextDeleter {
    void operator()(SwrContext* p) const noexcept { swr_free(&p); }
};
struct SwsContextDeleter {
    void operator()(SwsContext* p) const noexcept { sws_freeContext(p); }
};
struct BufferRefDeleter {
    void operator()(AVBufferRef* p) const noexcept { av_buffer_unref(&p); }
};

using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using SwrContextPtr = std::unique_ptr<SwrContext, SwrContextDeleter>;
using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;
using BufferRefPtr = std::unique_ptr<AVBufferRef, BufferRefDeleter>;

/// Shares ownership of an AVFrame (used as a CpuPlanes owner).
std::shared_ptr<AVFrame> shareFrame(FramePtr frame);

// ---- Errors
// --------------------------------------------------------------------------------------------------

std::string avErrorString(int averror);
/// Error with "<what>: <FFmpeg message>" text.
Error ffmpegError(ErrorCode code, std::string_view what, int averror);

// ---- Paths
// ---------------------------------------------------------------------------------------------------

/// UTF-8 string -> filesystem path (never through the ANSI code page on Windows).
std::filesystem::path pathFromUtf8(std::string_view utf8);
std::string pathToUtf8(const std::filesystem::path& path);

// ---- Time
// ----------------------------------------------------------------------------------------------------

/// Stream timestamp -> ticks (media::streamToTicks with an FFmpeg time base): nearest, checked; nullopt on
/// overflow or an invalid time base. The only place media converts stream time.
std::optional<Ticks> toTicks(std::int64_t ts, AVRational timeBase);
/// Ticks -> stream timestamp (media::ticksToStream): rounded down, checked.
std::optional<std::int64_t> toStreamTime(Ticks t, AVRational timeBase);

/// Origin of source time: the earliest start_time among the file's audio and video streams. Both decoders
/// measure source time from this point so the streams of one file stay in sync.
Ticks mediaOrigin(const AVFormatContext* fmt);

// ---- Opening
// -------------------------------------------------------------------------------------------------

/// avformat_open_input + avformat_find_stream_info with error mapping (NotFound / Unsupported / IoError).
std::expected<FormatContextPtr, Error> openInput(const std::string& utf8Path);

/// True for embedded cover art and other attached pictures.
bool isAttachedPicture(const AVStream* stream);

/// Resolves `streamIndex` (-1 = best) to a stream of `type`, or InvalidArgument / NotFound.
std::expected<AVStream*, Error> selectStream(AVFormatContext* fmt, AVMediaType type, int streamIndex);

// ---- Decoding
// --------------------------------------------------------------------------------------------------

/// The send/receive loop shared by the decoders: reads packets of one stream, feeds the codec, drains it at
/// the end of the file and skips corrupt packets. The caller owns the demuxer and codec context; call reset()
/// after every seek or codec change (together with avcodec_flush_buffers()).
class DecodeLoop {
public:
    DecodeLoop();

    void reset();

    /// Decodes the next frame into `out`. Returns false once the stream is fully drained.
    std::expected<bool, Error> receive(AVFormatContext* fmt, AVCodecContext* ctx, int streamIndex,
                                       AVFrame* out, std::string_view source);

    /// DTS of the last packet sent to the codec (AV_NOPTS_VALUE if none since reset()).
    std::int64_t lastPacketDts() const { return m_lastPacketDts; }
    bool isValid() const { return m_packet != nullptr; }

private:
    PacketPtr m_packet;
    bool m_demuxEof = false;
    bool m_codecEof = false;
    bool m_packetHeld = false;
    std::int64_t m_lastPacketDts = AV_NOPTS_VALUE;
};

// ---- Stream descriptions (shared by probe() and the decoders)
// -------------------------------------------------

ColorInfo colorInfoFrom(AVColorSpace space, AVColorRange range, AVColorPrimaries primaries,
                        AVColorTransferCharacteristic transfer, AVChromaLocation chroma);
/// Display orientation from the stream's display matrix (identity when absent).
Orientation orientationOf(const AVCodecParameters* par);
VideoStreamInfo describeVideoStream(AVFormatContext* fmt, AVStream* stream);
AudioStreamInfo describeAudioStream(const AVFormatContext* fmt, const AVStream* stream);

// ---- Pixel formats
// -------------------------------------------------------------------------------------------

/// FFmpeg pixel format with the same memory layout as `format` (AV_PIX_FMT_NONE for Unknown).
AVPixelFormat toAvPixelFormat(PixelFormat format);

// ---- Logging
// -------------------------------------------------------------------------------------------------

/// Installs the av_log callback once per process (idempotent, thread-safe). Called by every public entry
/// point that touches FFmpeg.
void ensureFfmpegLogging();
bool logEnabled(LogLevel level);
void logMessage(LogLevel level, std::string_view message);

template <typename... Args> void log(LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
    if (logEnabled(level)) {
        logMessage(level, std::format(fmt, std::forward<Args>(args)...));
    }
}

} // namespace wabcut::media::detail
