#include "Background.h"
#include "FfmpegUtil.h"

#include <wabcut/media/Fingerprint.h>
#include <wabcut/media/ThumbnailService.h>
#include <wabcut/media/VideoDecoder.h>

extern "C" {
#include <libavutil/mathematics.h>
}

#include <cmath>

namespace wabcut::media {

using namespace detail;

namespace {

constexpr std::uint32_t kThumbMagic = 0x48544357; // "WCTH"
constexpr std::uint32_t kThumbVersion = 2;        // 2: keyed by source fingerprint

int swsColorspace(ColorMatrix m) {
    switch (m) {
    case ColorMatrix::BT709:
        return SWS_CS_ITU709;
    case ColorMatrix::BT601:
        return SWS_CS_ITU601;
    case ColorMatrix::BT2020NonConstant:
    case ColorMatrix::BT2020Constant:
        return SWS_CS_BT2020;
    case ColorMatrix::SMPTE240M:
        return SWS_CS_SMPTE240M;
    default:
        return SWS_CS_DEFAULT;
    }
}

/// Applies `o` to a tightly packed w x h RGBA image: rotate clockwise, then mirror the result horizontally.
std::vector<std::uint8_t> orientRgba(const std::vector<std::uint8_t>& src, int w, int h, Orientation o) {
    std::vector<std::uint8_t> dst(src.size());
    const auto* in = reinterpret_cast<const std::uint32_t*>(src.data());
    auto* out = reinterpret_cast<std::uint32_t*>(dst.data());
    const bool quarterTurn = o.rotationDegrees == 90 || o.rotationDegrees == 270;
    const int outW = quarterTurn ? h : w;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int ox = x;
            int oy = y;
            switch (o.rotationDegrees) {
            case 90:
                ox = h - 1 - y;
                oy = x;
                break;
            case 180:
                ox = w - 1 - x;
                oy = h - 1 - y;
                break;
            case 270:
                ox = y;
                oy = w - 1 - x;
                break;
            default:
                break;
            }
            if (o.mirrored) {
                ox = outW - 1 - ox;
            }
            out[static_cast<std::size_t>(oy) * outW + ox] = in[static_cast<std::size_t>(y) * w + x];
        }
    }
    return dst;
}

/// Scales `frame` to RGBA at the requested display size (sample aspect and orientation applied).
std::expected<Thumbnail, Error> makeThumbnail(const FrameHandle& frame, int height, SwsContextPtr& sws) {
    const CpuPlanes* cpu = frame.cpu();
    if (!cpu) {
        return std::unexpected(Error{ErrorCode::Unsupported, "Thumbnails need a CPU frame payload"});
    }
    const double sar = frame.sampleAspect.isValid() ? frame.sampleAspect.toDouble() : 1.0;
    const int rotation = frame.orientation.rotationDegrees;
    const bool quarterTurn = rotation == 90 || rotation == 270;
    double displayW = frame.width * sar;
    double displayH = frame.height;
    if (quarterTurn) {
        std::swap(displayW, displayH);
    }
    const int outH = height;
    const int outW = std::max(1, static_cast<int>(std::lround(outH * displayW / displayH)));
    const int scaledW = quarterTurn ? outH : outW; // size before rotation
    const int scaledH = quarterTurn ? outW : outH;

    const AVPixelFormat srcFormat = toAvPixelFormat(frame.format);
    if (srcFormat == AV_PIX_FMT_NONE) { // swscale asserts on unknown formats
        return std::unexpected(Error{ErrorCode::Unsupported, "Unknown frame format"});
    }
    sws.reset(sws_getCachedContext(
        sws.release(), frame.width, frame.height, srcFormat, scaledW, scaledH, AV_PIX_FMT_RGBA,
        SWS_AREA | SWS_ACCURATE_RND | SWS_BITEXACT | SWS_FULL_CHR_H_INT, nullptr, nullptr, nullptr));
    if (!sws) {
        return std::unexpected(Error{ErrorCode::Unsupported, "Cannot scale this pixel format"});
    }
    if (frame.format != PixelFormat::RGBA8) {
        const int* srcCoeffs = sws_getCoefficients(swsColorspace(frame.color.matrix));
        const int* dstCoeffs = sws_getCoefficients(SWS_CS_DEFAULT);
        sws_setColorspaceDetails(sws.get(), srcCoeffs, frame.color.range == ColorRange::Full ? 1 : 0,
                                 dstCoeffs, 1, 0, 1 << 16, 1 << 16);
    }

    const std::uint8_t* srcData[4] = {};
    int srcStride[4] = {};
    for (int i = 0; i < frame.planeCount(); ++i) {
        srcData[i] = cpu->planes[static_cast<std::size_t>(i)].data;
        srcStride[i] = cpu->planes[static_cast<std::size_t>(i)].stride;
    }
    std::vector<std::uint8_t> scaled(static_cast<std::size_t>(scaledW) * scaledH * 4);
    std::uint8_t* dstData[4] = {scaled.data(), nullptr, nullptr, nullptr};
    int dstStride[4] = {scaledW * 4, 0, 0, 0};
    const int r = sws_scale(sws.get(), srcData, srcStride, 0, frame.height, dstData, dstStride);
    if (r <= 0) {
        return std::unexpected(ffmpegError(ErrorCode::DecoderError, "Thumbnail scaling failed", r));
    }

    Thumbnail t;
    t.time = frame.pts;
    t.width = outW;
    t.height = outH;
    t.rgba = rotation == 0 && !frame.orientation.mirrored
                 ? std::move(scaled)
                 : orientRgba(scaled, scaledW, scaledH, frame.orientation);
    return t;
}

std::string thumbnailKey(std::uint64_t fingerprint, const ThumbnailOptions& o) {
    return std::format("thumbnails/v{}|fp={:016x}|count={}|height={}|stream={}|hw={}", kThumbVersion,
                       fingerprint, o.count, o.height, o.stream, o.allowHardware ? 1 : 0);
}

std::filesystem::path thumbnailFile(const std::filesystem::path& cacheDir, const std::string& key) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(key.data());
    return cacheDir / "thumbnails" / (hex64(fnv1a({data, key.size()})) + ".wthumb");
}

std::optional<ThumbnailStrip> loadStrip(const std::filesystem::path& file, const std::string& key) {
    const auto bytes = readWholeFile(file);
    if (!bytes || !ByteReader::verifyChecksum(*bytes)) {
        return std::nullopt;
    }
    ByteReader in(*bytes);
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::string storedKey;
    std::uint32_t count = 0;
    if (!in.pod(magic) || magic != kThumbMagic || !in.pod(version) || version != kThumbVersion ||
        !in.string(storedKey) || storedKey != key || !in.pod(count) || count > 1000) {
        return std::nullopt;
    }
    ThumbnailStrip strip;
    strip.thumbnails.resize(count);
    for (Thumbnail& t : strip.thumbnails) {
        std::uint32_t size = 0;
        if (!in.pod(t.time.value) || !in.pod(t.width) || !in.pod(t.height) || !in.pod(size) || t.width <= 0 ||
            t.height <= 0 || size != static_cast<std::uint64_t>(t.width) * t.height * 4 ||
            size > in.remaining()) {
            return std::nullopt;
        }
        t.rgba.resize(size);
        in.raw(t.rgba.data(), size);
    }
    strip.fromCache = true;
    return strip;
}

bool saveStrip(const std::filesystem::path& file, const std::string& key, const ThumbnailStrip& strip) {
    ByteWriter out;
    out.pod(kThumbMagic);
    out.pod(kThumbVersion);
    out.string(key);
    out.pod(static_cast<std::uint32_t>(strip.thumbnails.size()));
    for (const Thumbnail& t : strip.thumbnails) {
        out.pod(t.time.value);
        out.pod(t.width);
        out.pod(t.height);
        out.pod(static_cast<std::uint32_t>(t.rgba.size()));
        out.raw(t.rgba.data(), t.rgba.size());
    }
    out.checksum();
    return writeFileAtomic(file, out.bytes());
}

} // namespace

std::expected<ThumbnailStrip, Error> extractThumbnails(const std::string& path,
                                                       const ThumbnailOptions& options, std::stop_token stop,
                                                       const std::function<void(float)>& progress) {
    if (options.count < 1 || options.count > 1000 || options.height < 8 || options.height > 2160) {
        return std::unexpected(
            Error{ErrorCode::InvalidArgument, "Thumbnail count must be 1..1000, height 8..2160"});
    }
    DecoderOptions decoderOptions;
    decoderOptions.allowHardware = options.allowHardware;
    auto decoder = VideoDecoder::open(path, options.stream, decoderOptions);
    if (!decoder) {
        return std::unexpected(decoder.error());
    }
    VideoDecoder& dec = **decoder;
    const VideoStreamInfo& info = dec.streamInfo();
    const Ticks start = dec.firstFrameTime();
    const Ticks length = info.duration;

    ThumbnailStrip strip;
    strip.thumbnails.reserve(static_cast<std::size_t>(options.count));
    SwsContextPtr sws;
    std::optional<FrameHandle> previous;
    for (int i = 0; i < options.count; ++i) {
        if (stop.stop_requested()) {
            return std::unexpected(Error{ErrorCode::Cancelled, "Cancelled"});
        }
        // Centre of the i-th of `count` equal slices (avoids the black first frame and the end of the
        // stream).
        const Ticks t =
            start + Ticks{av_rescale(length.value, 2 * i + 1, 2 * static_cast<std::int64_t>(options.count))};
        auto frame = dec.frameAt(t);
        if (!frame && frame.error().code == ErrorCode::NotFound && previous) {
            frame = *previous; // container duration longer than the decodable frames
        }
        if (!frame) {
            return std::unexpected(frame.error());
        }
        auto thumb = makeThumbnail(*frame, options.height, sws);
        if (!thumb) {
            return std::unexpected(thumb.error());
        }
        strip.thumbnails.push_back(std::move(*thumb));
        previous = std::move(*frame);
        if (progress) {
            progress(static_cast<float>(i + 1) / static_cast<float>(options.count));
        }
    }
    return strip;
}

// ---- ThumbnailService
// --------------------------------------------------------------------------------------------

struct ThumbnailService::Workers {
    explicit Workers(int threads) : pool(threads) {}
    WorkerPool pool;
};

ThumbnailService::ThumbnailService(std::filesystem::path cacheDir, int workerThreads)
    : m_cacheDir(std::move(cacheDir)), m_workers(std::make_unique<Workers>(std::max(1, workerThreads))) {}

ThumbnailService::~ThumbnailService() = default;

std::filesystem::path ThumbnailService::cacheFileFor(const std::string& path,
                                                     const ThumbnailOptions& options) const {
    const auto fingerprint = sourceFingerprint(path);
    return fingerprint ? thumbnailFile(m_cacheDir, thumbnailKey(*fingerprint, options))
                       : std::filesystem::path{};
}

std::expected<ThumbnailStrip, Error>
ThumbnailService::getOrExtract(const std::string& path, const ThumbnailOptions& options, std::stop_token stop,
                               const std::function<void(float)>& progress) {
    const auto fingerprint = sourceFingerprint(path);
    if (!fingerprint) {
        return std::unexpected(fingerprint.error());
    }
    const std::string key = thumbnailKey(*fingerprint, options);
    const std::filesystem::path file = thumbnailFile(m_cacheDir, key);
    if (auto cached = loadStrip(file, key)) {
        return std::move(*cached);
    }
    auto strip = extractThumbnails(path, options, stop, progress);
    if (strip && !saveStrip(file, key, *strip)) {
        log(LogLevel::Warning, "Could not write the thumbnail cache file {}", pathToUtf8(file));
    }
    return strip;
}

Job<ThumbnailStrip> ThumbnailService::request(const std::string& path, const ThumbnailOptions& options,
                                              Callback onDone) {
    return submitJob<ThumbnailStrip>(
        m_workers->pool, std::move(onDone),
        [this, path, options](std::stop_token stop, const std::function<void(float)>& progress) {
            return getOrExtract(path, options, stop, progress);
        });
}

} // namespace wabcut::media
