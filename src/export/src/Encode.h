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
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace wabcut::exporter::detail {

// ---- Muxer ------------------------------------------------------------------------------------------------

/// One output file. Streams are added from opened encoders, then open() writes the header.
///
/// 0.2: Container::Image writes one file per video packet through FFmpeg's image2 muxer: `file` is then a pattern
/// "<folder>/<name>%06d<ext>" in a folder the export owns (frames numbered from 0).
class Muxer {
public:
    static std::expected<Muxer, Error> create(Container container, const std::filesystem::path& file);

    /// 0.2: file-level tags ("title", "comment", "timecode" - MOV / MP4 write a 'tmcd' track for it; the BWF fields
    /// "description", "originator", "time_reference", ...). Before open().
    void setMetadata(const char* key, const std::string& value);
    /// 0.2: a chapter in milliseconds from the start (MP4 / MOV / MKV / M4A). Before open().
    void addChapter(std::int64_t startMs, std::int64_t endMs, const std::string& title);
    /// 0.2: a Broadcast WAV 'bext' chunk (WAV only; its fields come from setMetadata). Before open().
    void setBroadcastWave(bool on) { m_bext = on; }
    /// 0.2: a stream's language tag (ISO 639-2).
    static void setLanguage(AVStream* stream, const std::string& language);

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
    bool m_bext = false;
    std::unique_ptr<std::mutex> m_writeMutex = std::make_unique<std::mutex>();
};

// ---- Colour (I.9) -----------------------------------------------------------------------------------------

/// The colour description an export writes: BT.709 (SDR), or BT.2020 with PQ or HLG; limited range Y'CbCr unless
/// the output is RGB (0.2: matrix RGB, full range) or JPEG (BT.601, full range).
struct ColorTags {
    AVColorPrimaries primaries = AVCOL_PRI_BT709;
    AVColorTransferCharacteristic transfer = AVCOL_TRC_BT709;
    AVColorSpace matrix = AVCOL_SPC_BT709;
    AVColorRange range = AVCOL_RANGE_MPEG;
};
ColorTags colorTagsOf(DynamicRange range);
/// 0.2: the tags of an output pixel format: colorTagsOf(range), made RGB / full range for RGB formats and BT.601 full
/// range for JPEG.
ColorTags colorTagsFor(DynamicRange range, VideoCodec codec, OutputPixelFormat format);

/// The HDR10 mastering display of a PQ export: Display P3 primaries, D65 white, `peakNits` down to 0.0001 nits (the
/// display most HDR10 masters are graded on; the project's HDR peak).
AVMasteringDisplayMetadata masteringDisplayFor(double peakNits);
/// Puts the mastering display and (when known) the content light levels on a stream: the MP4 / MOV muxer writes them
/// as 'mdcv' / 'clli' boxes when it writes the movie header (at the trailer), so levels measured during the export
/// still make it in. An existing entry is replaced.
void setHdrStreamMetadata(AVStream* stream, const AVMasteringDisplayMetadata* display, const AVContentLightMetadata* light);

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
    /// I.9, PQ exports: the HDR10 mastering display, given to the encoder before it opens (libx265 and NVENC write
    /// the SEI, SVT-AV1 the metadata OBU) through AVCodecContext::decoded_side_data.
    std::optional<AVMasteringDisplayMetadata> masteringDisplay;
    /// P.2: frames carry CEA-608 captions (AV_FRAME_DATA_A53_CC) that the encoder must write as A/53 caption SEI:
    /// only encoders that do are chosen (supportsA53Captions).
    bool a53Captions = false;
    /// 0.2 (smart rendering): slice threads only, so each frame's packet comes out as it goes in.
    bool noFrameDelay = false;
};

/// Whether FFmpeg's encoder `name` writes AV_FRAME_DATA_A53_CC side data into the stream (libx264, libx265 with its
/// a53cc option, NVENC, Quick Sync H.264, VA-API, VideoToolbox; not AMF or Media Foundation).
bool supportsA53Captions(std::string_view name);

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

class FrameEffects;

/// Converts a source frame to the encoder input and applies the ScaleMode crop / pad. One per export.
class FrameConverter {
public:
    /// `renderWidth/Height`: what the source delivers; `width/height`: the encoded frame.
    static std::expected<FrameConverter, Error> create(int renderWidth, int renderHeight, int width,
                                                       int height, AVPixelFormat format, ColorTags tags = {});

    /// 0.2: the whole setup - Use Maximum Render Quality's larger source, the picture effects, alpha.
    struct Setup {
        int sourceWidth = 0; ///< what the source delivers (the render size, or larger: scaled down with Lanczos)
        int sourceHeight = 0;
        int renderWidth = 0; ///< the canvas at the output scale, before the ScaleMode crop / pad
        int renderHeight = 0;
        int width = 0; ///< the encoded frame
        int height = 0;
        AVPixelFormat format = AV_PIX_FMT_NONE;
        ColorTags tags;
        std::shared_ptr<const FrameEffects> effects; ///< applied at the output size (null: none)
        bool alpha = false;                          ///< the output keeps the source's alpha; pads are transparent
        bool deep = false;                           ///< effects work at 16 bits per channel (10-bit sources)
    };
    static std::expected<FrameConverter, Error> create(const Setup& setup);

    /// A new frame (refcounted, safe to hand to the encoder) holding `source` converted, limited range, tagged with
    /// the converter's colour (BT.709, or BT.2020 PQ / HLG: the source carries the same, so only the matrix and the
    /// chroma are converted). `frameIndex`: the output frame (the effects' timecode).
    std::expected<FramePtr, Error> convert(const media::FrameHandle& source, std::int64_t frameIndex = 0);

    AVPixelFormat format() const { return m_format; }

private:
    /// 0.2: through an RGBA work frame at the output size (Lanczos pre-scale, effects).
    std::expected<FramePtr, Error> convertThroughWork(AVFrame* view, int viewW, int viewH, int cropX, int cropY,
                                                      std::int64_t frameIndex);

    SwsContextPtr m_sws;
    SwsContextPtr m_swsWork; ///< 0.2: source view -> work format
    SwsContextPtr m_swsPre;  ///< 0.2: Use Maximum Render Quality: source -> render size (Lanczos)
    int m_sourceWidth = 0;
    int m_sourceHeight = 0;
    int m_renderWidth = 0;
    int m_renderHeight = 0;
    int m_width = 0;
    int m_height = 0;
    AVPixelFormat m_format = AV_PIX_FMT_NONE;
    std::shared_ptr<PaletteGraph> m_palette; ///< GIF (PAL8)
    ColorTags m_tags;
    std::shared_ptr<const FrameEffects> m_effects;
    bool m_alpha = false;
    bool m_deep = false;
};

/// FFmpeg pixel format of a decoded frame layout (AV_PIX_FMT_NONE for Unknown).
AVPixelFormat avPixelFormatOf(media::PixelFormat format);
/// FFmpeg matrix coefficients of a colour matrix (BT.709 for Unknown).
AVColorSpace avColorSpaceOf(ColorMatrix matrix);

/// The black frame of a format (the test frame of an encoder probe, padding).
std::expected<FramePtr, Error> blackFrame(AVPixelFormat format, int width, int height, ColorTags tags = {});

/// Uploads a CPU frame into the encoder's hardware frame pool when it has one, else returns it as is.
std::expected<FramePtr, Error> prepareForEncoder(const VideoEncoder& encoder, FramePtr frame);

// ---- Audio encoder ----------------------------------------------------------------------------------------

/// AAC or PCM, fed interleaved float; cuts the input into the codec's frame size.
class AudioEncoder {
public:
    /// `bigEndian`: PCM as AIFF stores it (0.2).
    static std::expected<AudioEncoder, Error> open(const AudioSettings& settings, bool globalHeader, bool bigEndian = false);

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
