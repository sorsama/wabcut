// Export Frame (P.2 E6): the frame rendered as an export renders it, then written as a still image (StillExport.h).

#include <wabcut/export/StillExport.h>

#include "FfmpegCommon.h"

#include <wabcut/export/RenderedVideoSource.h>
#include <wabcut/model/MediaLocator.h>

#include <QImageWriter>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

namespace wabcut::exporter {

namespace {

using namespace detail;

struct FormatInfo {
    StillFormat format;
    std::string_view name;
    std::string_view extension;
};
constexpr std::array kFormats{
    FormatInfo{StillFormat::Dpx, "dpx", ".dpx"},     FormatInfo{StillFormat::Jpeg, "jpeg", ".jpg"},
    FormatInfo{StillFormat::Png, "png", ".png"},     FormatInfo{StillFormat::Tiff, "tiff", ".tif"},
    FormatInfo{StillFormat::Targa, "targa", ".tga"}, FormatInfo{StillFormat::Bmp, "bmp", ".bmp"},
    FormatInfo{StillFormat::Gif, "gif", ".gif"},     FormatInfo{StillFormat::OpenExr, "openexr", ".exr"},
};

struct FormatContextDeleter {
    void operator()(AVFormatContext* p) const noexcept {
        if (p && p->pb && !(p->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&p->pb);
        }
        avformat_free_context(p);
    }
};
using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;

// ---- GIF: 256 colours ----

/// The ordered palette: 6 reds x 7 greens x 6 blues (252 colours; green, which the eye resolves best, gets a level more).
constexpr int kGifReds = 6, kGifGreens = 7, kGifBlues = 6;

int level(int index, int levels) { return (index * 255 + (levels - 1) / 2) / (levels - 1); }

/// Floyd-Steinberg error diffusion of `rgba` onto the ordered palette: indices into `frame`'s plane 0, the palette into
/// plane 1 (AV_PIX_FMT_PAL8: 0xAARRGGBB in native endianness).
void quantize(const QImage& rgba, AVFrame* frame) {
    auto* palette = reinterpret_cast<std::uint32_t*>(frame->data[1]);
    for (int i = 0; i < 256; ++i)
        palette[i] = 0xFF000000u;
    for (int r = 0; r < kGifReds; ++r)
        for (int g = 0; g < kGifGreens; ++g)
            for (int b = 0; b < kGifBlues; ++b)
                palette[(r * kGifGreens + g) * kGifBlues + b] =
                    0xFF000000u | (static_cast<std::uint32_t>(level(r, kGifReds)) << 16) |
                    (static_cast<std::uint32_t>(level(g, kGifGreens)) << 8) | static_cast<std::uint32_t>(level(b, kGifBlues));
    const int w = rgba.width();
    const int h = rgba.height();
    std::vector<std::array<float, 3>> current(static_cast<std::size_t>(w) + 2), next(static_cast<std::size_t>(w) + 2);
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* line = rgba.constScanLine(y);
        std::fill(next.begin(), next.end(), std::array<float, 3>{});
        for (int x = 0; x < w; ++x) {
            std::array<float, 3> c{};
            std::array<int, 3> q{};
            const std::array<int, 3> levels{kGifReds, kGifGreens, kGifBlues};
            for (int k = 0; k < 3; ++k) {
                c[static_cast<std::size_t>(k)] = std::clamp(line[x * 4 + k] + current[static_cast<std::size_t>(x) + 1][static_cast<std::size_t>(k)], 0.0f, 255.0f);
                const int n = levels[static_cast<std::size_t>(k)];
                q[static_cast<std::size_t>(k)] = static_cast<int>(std::lround(c[static_cast<std::size_t>(k)] * (n - 1) / 255.0f));
            }
            frame->data[0][static_cast<std::ptrdiff_t>(y) * frame->linesize[0] + x] =
                static_cast<std::uint8_t>((q[0] * kGifGreens + q[1]) * kGifBlues + q[2]);
            for (int k = 0; k < 3; ++k) {
                const float error = c[static_cast<std::size_t>(k)] - static_cast<float>(level(q[static_cast<std::size_t>(k)], levels[static_cast<std::size_t>(k)]));
                const auto xi = static_cast<std::size_t>(x) + 1;
                const auto ki = static_cast<std::size_t>(k);
                current[xi + 1][ki] += error * 7.0f / 16.0f;
                next[xi - 1][ki] += error * 3.0f / 16.0f;
                next[xi][ki] += error * 5.0f / 16.0f;
                next[xi + 1][ki] += error * 1.0f / 16.0f;
            }
        }
        std::swap(current, next);
    }
}

// ---- FFmpeg's image encoders ----

std::expected<void, Error> encodeWithFfmpeg(const QImage& rgba, const std::filesystem::path& file, StillFormat format) {
    media::detail::ensureFfmpegLogging();
    AVCodecID id = AV_CODEC_ID_NONE;
    AVPixelFormat pixels = AV_PIX_FMT_NONE;
    switch (format) {
    case StillFormat::Dpx:
        id = AV_CODEC_ID_DPX;
        pixels = AV_PIX_FMT_GBRP10LE; // 10-bit RGB, the DPX everyone reads
        break;
    case StillFormat::Jpeg:
        id = AV_CODEC_ID_MJPEG;
        pixels = AV_PIX_FMT_YUVJ444P;
        break;
    case StillFormat::Tiff:
        id = AV_CODEC_ID_TIFF;
        pixels = AV_PIX_FMT_RGB24;
        break;
    case StillFormat::Targa:
        id = AV_CODEC_ID_TARGA;
        pixels = AV_PIX_FMT_BGR24;
        break;
    case StillFormat::Bmp:
        id = AV_CODEC_ID_BMP;
        pixels = AV_PIX_FMT_BGR24;
        break;
    case StillFormat::Gif:
        id = AV_CODEC_ID_GIF;
        pixels = AV_PIX_FMT_PAL8;
        break;
    case StillFormat::Png:
    case StillFormat::OpenExr:
        return std::unexpected(Error{ErrorCode::Unsupported, "not an FFmpeg still format"});
    }
    const AVCodec* codec = avcodec_find_encoder(id);
    if (!codec) {
        return std::unexpected(Error{ErrorCode::Unsupported, std::string(stillFormatName(format)) + ": no encoder in this build"});
    }
    CodecContextPtr ctx(avcodec_alloc_context3(codec));
    if (!ctx) {
        return std::unexpected(Error{ErrorCode::IoError, "out of memory"});
    }
    ctx->width = rgba.width();
    ctx->height = rgba.height();
    ctx->pix_fmt = pixels;
    ctx->time_base = AVRational{1, 25};
    ctx->sample_aspect_ratio = AVRational{1, 1};
    if (format == StillFormat::Jpeg) {
        ctx->flags |= AV_CODEC_FLAG_QSCALE; // high quality, as Premiere's JPEG frames
        ctx->global_quality = FF_QP2LAMBDA * 2;
        ctx->color_range = AVCOL_RANGE_JPEG;
    }
    if (format == StillFormat::Tiff) {
        (void)av_opt_set(ctx->priv_data, "compression_algo", "lzw", 0);
    }
    if (const int e = avcodec_open2(ctx.get(), codec, nullptr); e < 0) {
        return std::unexpected(ffmpegError(ErrorCode::Unsupported, "Cannot open the image encoder", e));
    }
    FramePtr frame(av_frame_alloc());
    if (!frame) {
        return std::unexpected(Error{ErrorCode::IoError, "out of memory"});
    }
    frame->format = pixels;
    frame->width = rgba.width();
    frame->height = rgba.height();
    if (const int e = av_frame_get_buffer(frame.get(), 0); e < 0) {
        return std::unexpected(ffmpegError(ErrorCode::IoError, "Cannot allocate the frame", e));
    }
    if (format == StillFormat::Gif) {
        quantize(rgba, frame.get());
    } else {
        SwsContextPtr sws(sws_getContext(rgba.width(), rgba.height(), AV_PIX_FMT_RGBA, rgba.width(), rgba.height(), pixels,
                                         SWS_POINT | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT, nullptr, nullptr, nullptr));
        if (!sws) {
            return std::unexpected(Error{ErrorCode::Unsupported, "Cannot convert the picture for the encoder"});
        }
        const std::uint8_t* src[1] = {rgba.constBits()};
        const int stride[1] = {static_cast<int>(rgba.bytesPerLine())};
        sws_scale(sws.get(), src, stride, 0, rgba.height(), frame->data, frame->linesize);
    }
    frame->pts = 0;

    // One packet, into its file (image2; GIF has a muxer of its own for the header and trailer).
    const std::string path = pathToUtf8(file);
    AVFormatContext* raw = nullptr;
    if (const int e = avformat_alloc_output_context2(&raw, nullptr, format == StillFormat::Gif ? "gif" : "image2", path.c_str());
        e < 0 || !raw) {
        return std::unexpected(ffmpegError(ErrorCode::IoError, "Cannot set up the image file", e));
    }
    FormatContextPtr out(raw);
    AVStream* stream = avformat_new_stream(out.get(), nullptr);
    if (!stream) {
        return std::unexpected(Error{ErrorCode::IoError, "out of memory"});
    }
    stream->time_base = ctx->time_base;
    if (const int e = avcodec_parameters_from_context(stream->codecpar, ctx.get()); e < 0) {
        return std::unexpected(ffmpegError(ErrorCode::IoError, "Cannot describe the image", e));
    }
    if (!(out->oformat->flags & AVFMT_NOFILE)) {
        if (const int e = avio_open(&out->pb, path.c_str(), AVIO_FLAG_WRITE); e < 0) {
            return std::unexpected(ffmpegError(ErrorCode::IoError, "Cannot write " + path, e));
        }
    }
    AVDictionary* options = nullptr;
    if (format != StillFormat::Gif) {
        av_dict_set(&options, "update", "1", 0); // one file, not a numbered sequence
    }
    const int header = avformat_write_header(out.get(), &options);
    av_dict_free(&options);
    if (header < 0) {
        return std::unexpected(ffmpegError(ErrorCode::IoError, "Cannot write " + path, header));
    }
    PacketPtr packet(av_packet_alloc());
    bool wrote = false;
    for (AVFrame* send : {frame.get(), static_cast<AVFrame*>(nullptr)}) {
        if (const int e = avcodec_send_frame(ctx.get(), send); e < 0) {
            return std::unexpected(ffmpegError(ErrorCode::IoError, "Cannot encode the image", e));
        }
        while (avcodec_receive_packet(ctx.get(), packet.get()) == 0) {
            packet->stream_index = stream->index;
            av_packet_rescale_ts(packet.get(), ctx->time_base, stream->time_base);
            if (const int e = av_interleaved_write_frame(out.get(), packet.get()); e < 0) {
                return std::unexpected(ffmpegError(ErrorCode::IoError, "Cannot write " + path, e));
            }
            wrote = true;
        }
    }
    if (const int e = av_write_trailer(out.get()); e < 0 || !wrote) {
        return std::unexpected(ffmpegError(ErrorCode::IoError, "Cannot finish " + path, e < 0 ? e : AVERROR_UNKNOWN));
    }
    return {};
}

// ---- OpenEXR: Wabcut's own writer (scanline, uncompressed, half floats) ----

std::uint16_t toHalf(float value) {
    // Values here are in [0, 1]: normal halves, subnormals and zero; round to nearest.
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    const int exponent = static_cast<int>((bits >> 23) & 0xFFu) - 127 + 15;
    std::uint32_t mantissa = bits & 0x7FFFFFu;
    if (exponent <= 0) {
        if (exponent < -10)
            return static_cast<std::uint16_t>(sign);
        mantissa |= 0x800000u;
        const auto shift = static_cast<std::uint32_t>(14 - exponent);
        std::uint32_t half = mantissa >> shift;
        if ((mantissa >> (shift - 1)) & 1u)
            ++half;
        return static_cast<std::uint16_t>(sign | half);
    }
    if (exponent >= 31)
        return static_cast<std::uint16_t>(sign | 0x7C00u);
    std::uint32_t half = sign | (static_cast<std::uint32_t>(exponent) << 10) | (mantissa >> 13);
    if (mantissa & 0x1000u)
        ++half; // carries into the exponent correctly
    return static_cast<std::uint16_t>(half);
}

class Bytes {
public:
    void u8(std::uint8_t v) { m_data.push_back(v); }
    void i32(std::int32_t v) { le(static_cast<std::uint32_t>(v), 4); }
    void u64(std::uint64_t v) { le(v, 8); }
    void f32(float v) { le(std::bit_cast<std::uint32_t>(v), 4); }
    void u16(std::uint16_t v) { le(v, 2); }
    void text(std::string_view s) {
        m_data.insert(m_data.end(), s.begin(), s.end());
        m_data.push_back(0);
    }
    /// An attribute: name, type, size, then `value` writes the value.
    template <class Fn> void attribute(std::string_view name, std::string_view type, std::int32_t size, Fn value) {
        text(name);
        text(type);
        i32(size);
        value(*this);
    }
    std::size_t size() const { return m_data.size(); }
    std::vector<std::uint8_t>& data() { return m_data; }
    void patchU64(std::size_t at, std::uint64_t v) {
        for (int i = 0; i < 8; ++i)
            m_data[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
    }

private:
    void le(std::uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i)
            m_data.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    std::vector<std::uint8_t> m_data;
};

std::expected<void, Error> writeExr(const QImage& rgba, const std::filesystem::path& file) {
    const int w = rgba.width();
    const int h = rgba.height();
    // The export output transform encoded linear light with the BT.709 OETF: back to linear.
    std::array<std::uint16_t, 256> linear{};
    std::array<std::uint16_t, 256> alpha{};
    for (int v = 0; v < 256; ++v) {
        const double e = v / 255.0;
        const double l = e < 0.081 ? e / 4.5 : std::pow((e + 0.099) / 1.099, 1.0 / 0.45);
        linear[static_cast<std::size_t>(v)] = toHalf(static_cast<float>(l));
        alpha[static_cast<std::size_t>(v)] = toHalf(static_cast<float>(e));
    }
    Bytes b;
    b.i32(20000630); // magic
    b.i32(2);        // version 2, single-part scanline
    // Channels, alphabetical as the format requires: A, B, G, R; HALF, linear sampling 1 x 1.
    const std::array<std::string_view, 4> channels{"A", "B", "G", "R"};
    b.attribute("channels", "chlist", 4 * (2 + 16) + 1, [&](Bytes& out) {
        for (const std::string_view c : channels) {
            out.text(c);
            out.i32(1); // HALF
            out.u8(0);  // pLinear
            out.u8(0);
            out.u8(0);
            out.u8(0);
            out.i32(1);
            out.i32(1);
        }
        out.u8(0);
    });
    b.attribute("compression", "compression", 1, [](Bytes& out) { out.u8(0); }); // NO_COMPRESSION
    const auto box = [&](Bytes& out) {
        out.i32(0);
        out.i32(0);
        out.i32(w - 1);
        out.i32(h - 1);
    };
    b.attribute("dataWindow", "box2i", 16, box);
    b.attribute("displayWindow", "box2i", 16, box);
    b.attribute("lineOrder", "lineOrder", 1, [](Bytes& out) { out.u8(0); }); // INCREASING_Y
    b.attribute("pixelAspectRatio", "float", 4, [](Bytes& out) { out.f32(1.0f); });
    b.attribute("screenWindowCenter", "v2f", 8, [](Bytes& out) {
        out.f32(0.0f);
        out.f32(0.0f);
    });
    b.attribute("screenWindowWidth", "float", 4, [](Bytes& out) { out.f32(1.0f); });
    b.u8(0); // end of the header
    // The offset table (one block per scanline without compression), then the scanlines.
    const std::size_t table = b.size();
    for (int y = 0; y < h; ++y)
        b.u64(0);
    const auto lineBytes = static_cast<std::int32_t>(4 * 2 * w);
    for (int y = 0; y < h; ++y) {
        b.patchU64(table + static_cast<std::size_t>(y) * 8, b.size());
        b.i32(y);
        b.i32(lineBytes);
        const std::uint8_t* line = rgba.constScanLine(y);
        for (const int channel : {3, 2, 1, 0}) { // A, B, G, R
            for (int x = 0; x < w; ++x) {
                const std::uint8_t v = line[x * 4 + channel];
                b.u16(channel == 3 ? alpha[v] : linear[v]);
            }
        }
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(b.data().data()), static_cast<std::streamsize>(b.size()));
    out.close();
    if (!out) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot write " + pathToUtf8(file)});
    }
    return {};
}

} // namespace

std::string_view stillFormatName(StillFormat format) {
    for (const FormatInfo& f : kFormats) {
        if (f.format == format) {
            return f.name;
        }
    }
    return {};
}

std::optional<StillFormat> stillFormatFromName(std::string_view name) {
    for (const FormatInfo& f : kFormats) {
        if (f.name == name) {
            return f.format;
        }
    }
    return std::nullopt;
}

std::string_view stillExtension(StillFormat format) {
    for (const FormatInfo& f : kFormats) {
        if (f.format == format) {
            return f.extension;
        }
    }
    return {};
}

std::vector<StillFormat> stillFormats() {
    std::vector<StillFormat> out;
    for (const FormatInfo& f : kFormats) {
        out.push_back(f.format);
    }
    return out;
}

std::expected<QImage, Error> renderStill(const ProjectState& state, SequenceId sequenceId, Ticks time) {
    const Sequence* seq = state.findSequence(sequenceId);
    if (!seq || seq->width <= 0 || seq->height <= 0) {
        return std::unexpected(Error{ErrorCode::NotFound, "no such sequence"});
    }
    const Rate rate = seq->frameRate.isValid() ? seq->frameRate : kRate30;
    RenderedSourceOptions options;
    options.renderThreads = 1;
    options.maxInFlight = 1;
    auto source = renderedVideoSourceFactory(options)(state, *seq,
                                                      VideoSourceFormat{seq->width, seq->height, rate, snapToFrame(time, rate), 1, {}});
    if (!source) {
        return std::unexpected(source.error());
    }
    if (auto requested = (*source)->request(0); !requested) {
        return std::unexpected(requested.error());
    }
    auto picture = (*source)->next();
    (*source)->stop();
    if (!picture) {
        return std::unexpected(picture.error());
    }
    const media::CpuPlanes* cpu = picture->cpu();
    if (!cpu || picture->format != media::PixelFormat::RGBA8) {
        return std::unexpected(Error{ErrorCode::DeviceError, "the renderer delivered no RGBA picture"});
    }
    QImage image(picture->width, picture->height, QImage::Format_RGBA8888);
    const media::Plane& plane = cpu->planes[0];
    for (int y = 0; y < picture->height; ++y) {
        std::memcpy(image.scanLine(y), plane.data + static_cast<std::ptrdiff_t>(y) * plane.stride,
                    static_cast<std::size_t>(picture->width) * 4);
    }
    return image;
}

std::expected<void, Error> writeStill(const QImage& picture, const std::filesystem::path& file, StillFormat format) {
    if (picture.isNull() || picture.width() <= 0 || picture.height() <= 0) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "no picture to write"});
    }
    const QImage rgba = picture.format() == QImage::Format_RGBA8888 ? picture : picture.convertToFormat(QImage::Format_RGBA8888);
    switch (format) {
    case StillFormat::Png: {
        QImageWriter writer(QString::fromStdString(pathToUtf8(file)), "PNG");
        if (!writer.write(rgba.convertToFormat(QImage::Format_RGB888))) {
            return std::unexpected(Error{ErrorCode::IoError, "Cannot write " + pathToUtf8(file) + ": " + writer.errorString().toStdString()});
        }
        return {};
    }
    case StillFormat::OpenExr:
        return writeExr(rgba, file);
    default:
        return encodeWithFfmpeg(rgba, file, format);
    }
}

std::shared_ptr<command::FrameExportServices> stillExportServices(command::FrameExportServices::Describer describer) {
    std::vector<std::string> names;
    for (const StillFormat f : stillFormats()) {
        names.emplace_back(stillFormatName(f));
    }
    command::FrameExportServices::Writer writer =
        [](const ProjectState& state, SequenceId sequence, Ticks time, const std::filesystem::path& file,
           const std::string& name) -> std::expected<command::FrameExportServices::Written, Error> {
        const std::optional<StillFormat> format = stillFormatFromName(name);
        if (!format) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "unknown still format " + name});
        }
        auto picture = renderStill(state, sequence, time);
        if (!picture) {
            return std::unexpected(picture.error());
        }
        if (auto written = writeStill(*picture, file, *format); !written) {
            return std::unexpected(written.error());
        }
        return command::FrameExportServices::Written{file, picture->width(), picture->height()};
    };
    return std::make_shared<command::FrameExportServices>(std::move(writer), std::move(describer), std::move(names));
}

} // namespace wabcut::exporter
