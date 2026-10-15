#pragma once

// Private: the libavcodec/libavformat/libswscale side of an export (muxer, encoders with hardware probing
// and fallback, frame conversion).

#include "FfmpegCommon.h"

#include <wabcut/export/ExportSettings.h>
#include <wabcut/media/FrameHandle.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace wabcut::exporter::detail {

// ---- Muxer ------------------------------------------------------------------------------------------------

/// One output file. Streams are added from opened encoders, then open() writes the header.
class Muxer {
public:
    static std::expected<Muxer, Error> create(Container container, const std::filesystem::path& file);

    Muxer() = default;
    Muxer(Muxer&& other) noexcept;
    Muxer& operator=(Muxer&& other) noexcept;
    ~Muxer();

    /// Encoders must set AV_CODEC_FLAG_GLOBAL_HEADER when this is true (MP4, MOV, MKV).
    bool wantsGlobalHeader() const;
    std::expected<AVStream*, Error> addStream(const AVCodecContext* encoder);
    /// Opens the file and writes the header.
    std::expected<void, Error> open();
    /// Rescales `packet` from the encoder's time base to the stream's and interleaves it (takes its data).
    /// Thread-safe (video and audio are encoded on different threads).
    std::expected<void, Error> write(AVPacket* packet, AVRational encoderTimeBase, AVStream* stream);
    /// Writes the trailer and closes the file.
    std::expected<void, Error> finish();
    /// Closes the file without a trailer (the caller deletes it).
    void abort() noexcept;

private:
    AVFormatContext* m_ctx = nullptr;
    Container m_container = Container::Mp4;
    std::string m_path; // UTF-8
    bool m_open = false;
    std::unique_ptr<std::mutex> m_writeMutex = std::make_unique<std::mutex>();
};

// ---- Video encoder ----------------------------------------------------------------------------------------

struct VideoEncodeParams {
    VideoSettings settings;
    int width = 0;
    int height = 0;
    Rate frameRate{};
    bool globalHeader = false;
    /// Colour description and pixel aspect written into the stream. Exports are BT.709 limited, square pixels;
    /// proxies keep their source's (Proxy.cpp).
    AVColorPrimaries primaries = AVCOL_PRI_BT709;
    AVColorTransferCharacteristic transfer = AVCOL_TRC_BT709;
    AVColorSpace matrix = AVCOL_SPC_BT709;
    AVColorRange range = AVCOL_RANGE_MPEG;
    AVChromaLocation chromaLocation = AVCHROMA_LOC_LEFT;
    AVRational sampleAspect{1, 1};
};

/// An opened video encoder.
struct VideoEncoder {
    std::string name;
    CodecContextPtr ctx;
    /// The CPU format frames are converted to (the encoder input, or the source of hardware uploads).
    AVPixelFormat inputFormat = AV_PIX_FMT_NONE;
    /// Set for encoders that take hardware frames (VA-API): frames are uploaded into this pool.
    BufferRefPtr hwFrames;
};

/// Opens `name` with the export's parameters. With `testEncode` the encoder must also encode and flush one
/// black frame (a hardware encoder can open and still fail on the first frame); that context is discarded.
std::expected<VideoEncoder, Error> openVideoEncoder(const std::string& name, const VideoEncodeParams& params,
                                                    bool testEncode);

struct VideoEncoderChoice {
    VideoEncoder encoder;
    std::vector<std::string> fallbacks; ///< "h264_amf: <reason>" for every candidate rejected first
};

/// VideoSettings::encoder "auto": hardware candidates (probed with a test frame), then the software encoder.
/// Anything else: exactly that encoder. Errors: Unsupported (listing every candidate's reason).
std::expected<VideoEncoderChoice, Error> chooseVideoEncoder(const VideoEncodeParams& params);

/// I.9 (gaming): GIF frames: RGB24 in, PAL8 out with a palette of 256 colours made for each frame (libavfilter's
/// palettegen stats_mode=single and paletteuse new=1, sierra2_4a dithering).
class PaletteGraph {
public:
    static std::expected<std::shared_ptr<PaletteGraph>, Error> create(int width, int height);
    ~PaletteGraph();
    PaletteGraph(const PaletteGraph&) = delete;
    PaletteGraph& operator=(const PaletteGraph&) = delete;
    std::expected<FramePtr, Error> apply(FramePtr rgb);

private:
    PaletteGraph() = default;
    AVFilterGraph* m_graph = nullptr;
    AVFilterContext* m_source = nullptr;
    AVFilterContext* m_sink = nullptr;
    std::int64_t m_next = 0;
};

/// Converts a source frame to the encoder input and applies the ScaleMode crop / pad. One per export.
class FrameConverter {
public:
    /// `renderWidth/Height`: what the source delivers; `width/height`: the encoded frame.
    static std::expected<FrameConverter, Error> create(int renderWidth, int renderHeight, int width,
                                                       int height, AVPixelFormat format);

    /// A new frame (refcounted, safe to hand to the encoder) holding `source` converted, BT.709 limited.
    std::expected<FramePtr, Error> convert(const media::FrameHandle& source);

    AVPixelFormat format() const { return m_format; }

private:
    SwsContextPtr m_sws;
    int m_renderWidth = 0;
    int m_renderHeight = 0;
    int m_width = 0;
    int m_height = 0;
    AVPixelFormat m_format = AV_PIX_FMT_NONE;
    std::shared_ptr<PaletteGraph> m_palette; ///< GIF (PAL8)
};

/// FFmpeg pixel format of a decoded frame layout (AV_PIX_FMT_NONE for Unknown).
AVPixelFormat avPixelFormatOf(media::PixelFormat format);
/// FFmpeg matrix coefficients of a colour matrix (BT.709 for Unknown).
AVColorSpace avColorSpaceOf(ColorMatrix matrix);

/// The black frame of a format (the test frame of an encoder probe, padding).
std::expected<FramePtr, Error> blackFrame(AVPixelFormat format, int width, int height);

/// Uploads a CPU frame into the encoder's hardware frame pool when it has one, else returns it as is.
std::expected<FramePtr, Error> prepareForEncoder(const VideoEncoder& encoder, FramePtr frame);

// ---- Audio encoder ----------------------------------------------------------------------------------------

/// AAC or PCM, fed interleaved float; cuts the input into the codec's frame size.
class AudioEncoder {
public:
    static std::expected<AudioEncoder, Error> open(const AudioSettings& settings, bool globalHeader);

    const std::string& name() const { return m_name; }
    AVCodecContext* context() const { return m_ctx.get(); }

    /// Encodes `interleaved` (settings.channels per frame) and writes the packets to `muxer`.
    std::expected<void, Error> encode(std::span<const float> interleaved, Muxer& muxer, AVStream* stream);
    /// Encodes what is buffered (a short last frame) and drains the encoder.
    std::expected<void, Error> flush(Muxer& muxer, AVStream* stream);

    std::int64_t samplesSent() const { return m_nextPts; }

private:
    std::expected<void, Error> sendFrame(const float* interleaved, int frames, Muxer& muxer,
                                         AVStream* stream);

    std::string m_name;
    CodecContextPtr m_ctx;
    PacketPtr m_packet;
    int m_channels = 2;
    int m_frameSize = 1024;
    std::vector<float> m_pending; // interleaved, less than one codec frame
    std::int64_t m_nextPts = 0;
};

/// Receives every packet the encoder has ready and writes it. `flushing`: the end of stream was sent.
std::expected<void, Error> drainEncoder(AVCodecContext* encoder, AVPacket* packet, Muxer& muxer,
                                        AVStream* stream, bool flushing);

} // namespace wabcut::exporter::detail
