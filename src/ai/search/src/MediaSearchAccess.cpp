#include <wabcut/ai/search/MediaSearchAccess.h>

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/analysis/MediaAnalysis.h>
#include <wabcut/ai/edit/MediaServices.h>
#include <wabcut/ai/search/HelperEmbedder.h>
#include <wabcut/media/Fingerprint.h>
#include <wabcut/media/VideoDecoder.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/platform/FileTime.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <numeric>
#include <thread>

namespace wabcut::ai::search {

namespace {

/// Y, U, V of a decoded frame at a position (0..255), whatever its CPU layout; after MediaAnalysis's reader.
class YuvSampler {
public:
    explicit YuvSampler(const media::FrameHandle& f) : m_cpu(f.cpu()) {
        using media::PixelFormat;
        switch (f.format) {
        case PixelFormat::NV12:
            m_interleaved = true;
            m_cx = m_cy = 1;
            break;
        case PixelFormat::P010:
            m_interleaved = true;
            m_cx = m_cy = 1;
            m_wide = true;
            m_shift = 8;
            break;
        case PixelFormat::YUV420P:
        case PixelFormat::YUVA420P:
            m_cx = m_cy = 1;
            break;
        case PixelFormat::YUV420P10:
            m_cx = m_cy = 1;
            m_wide = true;
            m_shift = 2;
            break;
        case PixelFormat::YUV422P:
            m_cx = 1;
            break;
        case PixelFormat::YUV422P10:
            m_cx = 1;
            m_wide = true;
            m_shift = 2;
            break;
        case PixelFormat::YUV444P:
            break;
        case PixelFormat::YUV444P10:
        case PixelFormat::YUVA444P10:
            m_wide = true;
            m_shift = 2;
            break;
        case PixelFormat::RGBA8:
            m_rgba = true;
            break;
        case PixelFormat::Unknown:
            m_cpu = nullptr;
            break;
        }
        const bool bt601 = f.color.matrix == ColorMatrix::BT601 || (f.color.matrix == ColorMatrix::Unknown && f.height < 720);
        const bool bt2020 = f.color.matrix == ColorMatrix::BT2020NonConstant || f.color.matrix == ColorMatrix::BT2020Constant;
        m_kr = bt601 ? 1.402 : bt2020 ? 1.4746 : 1.5748;
        m_kgb = bt601 ? 0.344136 : bt2020 ? 0.16455 : 0.1873;
        m_kgr = bt601 ? 0.714136 : bt2020 ? 0.57135 : 0.4681;
        m_kb = bt601 ? 1.772 : bt2020 ? 1.8814 : 1.8556;
        m_full = f.color.range == ColorRange::Full;
    }

    bool valid() const { return m_cpu != nullptr; }

    /// RGB of the source point (x, y), luma averaged over a 3 x 3 grid `step` texels apart.
    void rgbAt(double x, double y, double stepX, double stepY, std::uint8_t* out) const {
        const int ix = static_cast<int>(x);
        const int iy = static_cast<int>(y);
        if (m_rgba) {
            const media::Plane& p = m_cpu->planes[0];
            const int cx = std::clamp(ix, 0, std::max(p.width - 1, 0));
            const int cy = std::clamp(iy, 0, std::max(p.height - 1, 0));
            const std::uint8_t* px = p.data + static_cast<std::ptrdiff_t>(cy) * p.stride + cx * 4;
            out[0] = px[0];
            out[1] = px[1];
            out[2] = px[2];
            return;
        }
        double luma = 0.0;
        for (int sy = -1; sy <= 1; ++sy) {
            for (int sx = -1; sx <= 1; ++sx) {
                luma += sample(0, static_cast<int>(x + sx * stepX / 3.0), static_cast<int>(y + sy * stepY / 3.0));
            }
        }
        luma /= 9.0;
        const int cx = ix >> m_cx;
        const int cy = iy >> m_cy;
        double u = 0.0;
        double v = 0.0;
        if (m_interleaved) {
            u = sample(1, 2 * cx, cy);
            v = sample(1, 2 * cx + 1, cy);
        } else {
            u = sample(1, cx, cy);
            v = sample(2, cx, cy);
        }
        const double l = m_full ? luma / 255.0 : (luma - 16.0) / 219.0;
        const double cb = m_full ? (u - 128.0) / 255.0 : (u - 128.0) / 224.0;
        const double cr = m_full ? (v - 128.0) / 255.0 : (v - 128.0) / 224.0;
        const auto clamp8 = [](double value) {
            return static_cast<std::uint8_t>(std::clamp(std::lround(value * 255.0), 0L, 255L));
        };
        out[0] = clamp8(l + m_kr * cr);
        out[1] = clamp8(l - m_kgb * cb - m_kgr * cr);
        out[2] = clamp8(l + m_kb * cb);
    }

private:
    double sample(int plane, int x, int y) const {
        const media::Plane& p = m_cpu->planes[static_cast<std::size_t>(plane)];
        x = std::clamp(x, 0, std::max(p.width * (m_interleaved && plane == 1 ? 2 : 1) - 1, 0));
        y = std::clamp(y, 0, std::max(p.height - 1, 0));
        const std::uint8_t* row = p.data + static_cast<std::ptrdiff_t>(y) * p.stride;
        if (m_wide) {
            std::uint16_t value = 0;
            std::memcpy(&value, row + static_cast<std::ptrdiff_t>(x) * 2, 2);
            return static_cast<double>(value >> m_shift);
        }
        return row[x];
    }

    const media::CpuPlanes* m_cpu = nullptr;
    bool m_interleaved = false;
    bool m_wide = false;
    bool m_rgba = false;
    bool m_full = false;
    int m_shift = 0;
    int m_cx = 0;
    int m_cy = 0;
    double m_kr = 1.5748;
    double m_kgb = 0.1873;
    double m_kgr = 0.4681;
    double m_kb = 1.8556;
};

/// The frame as the viewer sees it (sample aspect, rotation and mirroring applied), `width` pixels wide at most.
Image picture(const media::FrameHandle& f, int width) {
    Image out;
    const YuvSampler sampler(f);
    if (!sampler.valid() || f.width <= 0 || f.height <= 0) {
        return out;
    }
    const double aspect = f.sampleAspect.num > 0 && f.sampleAspect.den > 0
                              ? static_cast<double>(f.sampleAspect.num) / static_cast<double>(f.sampleAspect.den)
                              : 1.0;
    const int rotation = ((f.orientation.rotationDegrees % 360) + 360) % 360;
    const bool quarter = rotation == 90 || rotation == 270;
    const double displayW = f.width * aspect;
    const double displayH = f.height;
    const double rotatedW = quarter ? displayH : displayW;
    const double rotatedH = quarter ? displayW : displayH;
    out.width = std::max(1, std::min(width, static_cast<int>(std::lround(rotatedW))));
    out.height = std::max(1, static_cast<int>(std::lround(rotatedH * out.width / rotatedW)));
    out.rgb.assign(static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height) * 3, 0);
    const double stepX = static_cast<double>(f.width) / (quarter ? out.height : out.width);
    const double stepY = static_cast<double>(f.height) / (quarter ? out.width : out.height);
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            double ox = (x + 0.5) / out.width;
            const double oy = (y + 0.5) / out.height;
            if (f.orientation.mirrored) {
                ox = 1.0 - ox;
            }
            double u = ox;
            double v = oy;
            switch (rotation) { // undo a clockwise rotation
            case 90:
                u = oy;
                v = 1.0 - ox;
                break;
            case 180:
                u = 1.0 - ox;
                v = 1.0 - oy;
                break;
            case 270:
                u = 1.0 - oy;
                v = ox;
                break;
            default:
                break;
            }
            sampler.rgbAt(u * f.width, v * f.height, stepX, stepY, &out.rgb[(static_cast<std::size_t>(y) * out.width + x) * 3]);
        }
    }
    return out;
}

int containerVideoStream(const MediaItem& media) { return media.info.video.empty() ? -1 : media.info.video.front().index; }

} // namespace

Image storedPicture(const media::FrameHandle& f, int width, int height) {
    Image out;
    const YuvSampler sampler(f);
    if (!sampler.valid() || f.width <= 0 || f.height <= 0 || width <= 0 || height <= 0) {
        return out;
    }
    out.width = width;
    out.height = height;
    out.rgb.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3, 0);
    const double stepX = static_cast<double>(f.width) / width;
    const double stepY = static_cast<double>(f.height) / height;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            sampler.rgbAt((x + 0.5) * stepX, (y + 0.5) * stepY, stepX, stepY, &out.rgb[(static_cast<std::size_t>(y) * width + x) * 3]);
        }
    }
    return out;
}

std::expected<std::unique_ptr<VideoPictureReader>, Error> VideoPictureReader::open(const std::string& path, int containerStream,
                                                                                  int width) {
    auto decoder = media::VideoDecoder::open(path, containerStream);
    if (!decoder) {
        return std::unexpected(decoder.error());
    }
    std::unique_ptr<VideoPictureReader> reader(new VideoPictureReader());
    reader->m_decoder = std::move(*decoder);
    reader->m_width = std::max(width, 16);
    return reader;
}

VideoPictureReader::~VideoPictureReader() = default;

std::expected<Image, Error> VideoPictureReader::at(Ticks time) {
    auto frame = m_decoder->frameAt(std::max(time, m_decoder->firstFrameTime()));
    if (!frame) {
        return std::unexpected(frame.error());
    }
    Image image = picture(*frame, m_width);
    if (!image.valid()) {
        return std::unexpected(Error(ErrorCode::DecoderError, "a frame of the video could not be read"));
    }
    return image;
}

std::string VideoPictureReader::backend() const { return std::string(m_decoder->backendName()); }

int VideoPictureReader::streamIndex() const { return m_decoder->streamInfo().index; }

MediaSearchAccess::MediaSearchAccess(MediaSearchAccessOptions options) : m_options(std::move(options)) {}

void MediaSearchAccess::setProjectFile(std::filesystem::path projectFile) {
    std::lock_guard lock(m_mutex);
    m_options.projectFile = std::move(projectFile);
}

std::optional<std::string> MediaSearchAccess::pathOf(const MediaItem& media) const {
    std::filesystem::path projectFile;
    {
        std::lock_guard lock(m_mutex);
        projectFile = m_options.projectFile;
    }
    if (auto path = resolveMediaPath(media.locator, projectFile)) {
        return pathToUtf8(*path);
    }
    if (!media.info.path.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(pathFromUtf8(media.info.path), ec)) {
            return media.info.path;
        }
    }
    return std::nullopt;
}

std::optional<std::uint64_t> MediaSearchAccess::fingerprint(const MediaItem& media) {
    const auto path = pathOf(media);
    if (!path) {
        return std::nullopt;
    }
    const auto fingerprint = media::sourceFingerprint(*path); // memoised per (path, size, mtime)
    return fingerprint ? std::optional<std::uint64_t>(*fingerprint) : std::nullopt;
}

std::string MediaSearchAccess::date(const MediaItem& media) {
    const auto path = pathOf(media);
    if (!path) {
        return {};
    }
    std::error_code ec;
    const auto written = std::filesystem::last_write_time(pathFromUtf8(*path), ec);
    if (ec) {
        return {};
    }
    const std::time_t t = std::chrono::system_clock::to_time_t(platform::toSystemClock(written));
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &t) != 0) {
        return {};
    }
#else
    if (localtime_r(&t, &local) == nullptr) {
        return {};
    }
#endif
    char text[16];
    return std::strftime(text, sizeof text, "%Y-%m-%d", &local) > 0 ? std::string(text) : std::string();
}

std::expected<analysis::ShotAnalysis, Error> MediaSearchAccess::shots(const MediaItem& media, const analysis::AnalysisProgress& progress) {
    const auto path = pathOf(media);
    if (!path) {
        return std::unexpected(Error(ErrorCode::NotFound, "the file is offline"));
    }
    analysis::MediaAnalyzerOptions options;
    options.cacheRoot = m_options.cacheRoot;
    options.useCache = !m_options.cacheRoot.empty();
    analysis::MediaAnalyzer analyzer(options);
    return analyzer.shots(*path, containerVideoStream(media), analysis::ShotOptions{}, progress);
}

std::expected<std::vector<Image>, Error> MediaSearchAccess::frames(const MediaItem& media, std::span<const Ticks> times, int width,
                                                                   std::stop_token stop) {
    const auto path = pathOf(media);
    if (!path) {
        return std::unexpected(Error(ErrorCode::NotFound, "the file is offline"));
    }
    auto decoder = media::VideoDecoder::open(*path, containerVideoStream(media));
    if (!decoder) {
        return std::unexpected(decoder.error());
    }
    // In time order, so the decoder runs forward and seeks only across gaps.
    std::vector<std::size_t> order(times.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) { return times[a] < times[b]; });
    std::vector<Image> out(times.size());
    Image previous;
    for (const std::size_t i : order) {
        if (stop.stop_requested()) {
            return std::unexpected(Error(ErrorCode::Cancelled, "cancelled"));
        }
        auto frame = (*decoder)->frameAt(std::max(times[i], (*decoder)->firstFrameTime()));
        if (!frame && frame.error().code != ErrorCode::NotFound) {
            return std::unexpected(frame.error());
        }
        Image image = frame ? picture(*frame, width) : previous; // past the last frame: the last one seen
        if (!image.valid()) {
            image.width = std::max(width, 16);
            image.height = std::max(width * 9 / 16, 9);
            image.rgb.assign(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 3, 0);
        }
        previous = image;
        out[i] = std::move(image);
    }
    return out;
}

std::shared_ptr<const Transcript> MediaSearchAccess::transcript(const MediaItem& media) {
    if (m_options.cacheRoot.empty() || !media.info.hasAudio()) {
        return nullptr;
    }
    std::filesystem::path projectFile;
    {
        std::lock_guard lock(m_mutex);
        projectFile = m_options.projectFile;
    }
    // A fresh provider each time: it remembers misses, and a transcript may have been made since.
    const edit::CachedTranscriptProvider provider(m_options.cacheRoot, projectFile);
    return provider.transcript(media, 0);
}

// --- The editor's search ------------------------------------------------------------------------------------------

namespace {

SearchHostOptions withDefaults(SearchHostOptions o) {
    if (o.indexDir.empty()) {
        o.indexDir = SearchIndex::defaultDir();
    }
    if (o.cacheRoot.empty()) {
        o.cacheRoot = AnalysisCache::defaultRoot();
    }
    if (o.modelRoot.empty()) {
        o.modelRoot = ModelStore::defaultRoot();
    }
    if (o.helper.empty()) {
        o.helper = defaultEmbedExecutable();
    }
    if (o.threads <= 0) {
        o.threads = std::max(1, static_cast<int>(std::thread::hardware_concurrency() / 2));
    }
    return o;
}

} // namespace

std::string visualSearchUnavailableReason(const SearchHostOptions& options) {
    const SearchHostOptions o = withDefaults(options);
    std::error_code ec;
    if (!std::filesystem::exists(o.helper, ec)) {
        return "this build has no embedding helper (wabcut-embed is built with WABCUT_WITH_ONNX)";
    }
    const ModelStore store(o.modelRoot);
    for (const ModelInfo* file : siglipModelFiles()) {
        if (file == nullptr || !store.isInstalled(*file)) {
            return "the SigLIP 2 model is not downloaded";
        }
    }
    return {};
}

std::expected<std::shared_ptr<FootageSearch>, Error> openFootageSearch(const SearchHostOptions& options) {
    const SearchHostOptions o = withDefaults(options);
    auto index = SearchIndex::open(o.indexDir, siglipModelVersion(), 768);
    if (!index) {
        return std::unexpected(index.error());
    }
    auto access = std::make_shared<MediaSearchAccess>(MediaSearchAccessOptions{o.projectFile, o.cacheRoot});
    const std::string reason = visualSearchUnavailableReason(o);
    std::shared_ptr<Embedder> embedder;
    if (reason.empty()) {
        HelperEmbedderOptions helper = siglipOptions(ModelStore(o.modelRoot));
        helper.executable = o.helper;
        helper.threads = o.threads;
        embedder = std::make_shared<HelperEmbedder>(std::move(helper));
    }
    FootageSearchOptions searchOptions;
    searchOptions.cacheRoot = o.cacheRoot;
    auto search = std::make_shared<FootageSearch>(searchOptions, std::shared_ptr<SearchIndex>(index->release()), access, embedder);
    search->setVisualUnavailableReason(reason);
    return search;
}

} // namespace wabcut::ai::search
