#include <wabcut/ai/analysis/MediaAnalysis.h>

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/analysis/AnalysisJson.h>
#include <wabcut/media/AudioDecoder.h>
#include <wabcut/media/Fingerprint.h>
#include <wabcut/media/VideoDecoder.h>
#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace wabcut::ai::analysis {

namespace {

/// Reads Y, U, V (0..255) of a decoded frame at a position, whatever its CPU layout.
class YuvReader {
public:
    explicit YuvReader(const media::FrameHandle& f) : m_cpu(f.cpu()) {
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

    /// RGB at (x, y) with luma averaged over a 4 x 4 grid of the box [x0, x1) x [y0, y1).
    void rgbOfBox(int x0, int x1, int y0, int y1, std::uint8_t* out) const {
        if (m_rgba) {
            const media::Plane& p = m_cpu->planes[0];
            const std::uint8_t* px = p.data + static_cast<std::ptrdiff_t>((y0 + y1) / 2) * p.stride + ((x0 + x1) / 2) * 4;
            out[0] = px[0];
            out[1] = px[1];
            out[2] = px[2];
            return;
        }
        double y = 0.0;
        int n = 0;
        for (int sy = 0; sy < 4; ++sy) {
            const int yy = y0 + (y1 - y0) * (2 * sy + 1) / 8;
            for (int sx = 0; sx < 4; ++sx) {
                const int xx = x0 + (x1 - x0) * (2 * sx + 1) / 8;
                y += sample(0, xx, yy);
                ++n;
            }
        }
        y /= n;
        const int cx = ((x0 + x1) / 2) >> m_cx;
        const int cy = ((y0 + y1) / 2) >> m_cy;
        double u = 0.0;
        double v = 0.0;
        if (m_interleaved) {
            u = sample(1, 2 * cx, cy);
            v = sample(1, 2 * cx + 1, cy);
        } else {
            u = sample(1, cx, cy);
            v = sample(2, cx, cy);
        }
        const double luma = m_full ? y / 255.0 : (y - 16.0) / 219.0;
        const double cb = m_full ? (u - 128.0) / 255.0 : (u - 128.0) / 224.0;
        const double cr = m_full ? (v - 128.0) / 255.0 : (v - 128.0) / 224.0;
        const auto clamp8 = [](double value) {
            return static_cast<std::uint8_t>(std::clamp(std::lround(value * 255.0), 0L, 255L));
        };
        out[0] = clamp8(luma + m_kr * cr);
        out[1] = clamp8(luma - m_kgb * cb - m_kgr * cr);
        out[2] = clamp8(luma + m_kb * cb);
    }

private:
    double sample(int plane, int x, int y) const {
        const media::Plane& p = m_cpu->planes[static_cast<std::size_t>(plane)];
        x = std::clamp(x, 0, std::max(p.width * (m_interleaved && plane == 1 ? 2 : 1) - 1, 0));
        y = std::clamp(y, 0, std::max(p.height - 1, 0));
        const std::uint8_t* row = p.data + static_cast<std::ptrdiff_t>(y) * p.stride;
        if (m_wide) {
            std::uint16_t v = 0;
            std::memcpy(&v, row + static_cast<std::ptrdiff_t>(x) * 2, 2);
            return static_cast<double>(v >> m_shift);
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

AnalysisFrame shrink(const media::FrameHandle& f, int width) {
    AnalysisFrame out;
    out.pts = f.pts;
    out.duration = f.duration;
    out.width = std::max(1, std::min(width, f.width));
    out.height = std::max(1, static_cast<int>(std::lround(static_cast<double>(f.height) * out.width / std::max(f.width, 1))));
    out.rgb.assign(static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height) * 3, 0);
    const YuvReader reader(f);
    if (!reader.valid()) {
        return out;
    }
    for (int y = 0; y < out.height; ++y) {
        const int y0 = y * f.height / out.height;
        const int y1 = std::max(y0 + 1, (y + 1) * f.height / out.height);
        for (int x = 0; x < out.width; ++x) {
            const int x0 = x * f.width / out.width;
            const int x1 = std::max(x0 + 1, (x + 1) * f.width / out.width);
            reader.rgbOfBox(x0, x1, y0, y1, &out.rgb[(static_cast<std::size_t>(y) * out.width + x) * 3]);
        }
    }
    return out;
}

} // namespace

// --- VideoFrameSource ------------------------------------------------------------------------------------------

std::expected<std::unique_ptr<VideoFrameSource>, Error> VideoFrameSource::open(const std::string& path,
                                                                               int containerStream, int width) {
    auto decoder = media::VideoDecoder::open(path, containerStream);
    if (!decoder) {
        return std::unexpected(decoder.error());
    }
    std::unique_ptr<VideoFrameSource> source(new VideoFrameSource());
    source->m_decoder = std::move(*decoder);
    source->m_width = std::max(width, 16);
    source->m_next = source->m_decoder->firstFrameTime();
    return source;
}

VideoFrameSource::~VideoFrameSource() = default;

std::expected<std::optional<AnalysisFrame>, Error> VideoFrameSource::next() {
    if (m_done) {
        return std::optional<AnalysisFrame>{};
    }
    auto frame = m_decoder->frameAt(m_next);
    if (!frame) {
        if (frame.error().code == ErrorCode::NotFound) {
            m_done = true;
            return std::optional<AnalysisFrame>{};
        }
        return std::unexpected(frame.error());
    }
    const Ticks period = frame->duration.value > 0 ? frame->duration
                                                   : framesToTicks(1, m_decoder->streamInfo().frameRate.isValid()
                                                                          ? m_decoder->streamInfo().frameRate
                                                                          : kRate25);
    const Ticks next = frame->pts + period;
    if (next <= m_next && frame->pts < m_next) {
        m_done = true; // no progress: the stream ended
        return std::optional<AnalysisFrame>{};
    }
    m_next = std::max(next, m_next + Ticks{1});
    return std::optional<AnalysisFrame>{shrink(*frame, m_width)};
}

std::optional<Ticks> VideoFrameSource::duration() const {
    const Ticks d = m_decoder->streamInfo().duration;
    return d.value > 0 ? std::optional<Ticks>{d} : std::nullopt;
}

std::string VideoFrameSource::backend() const { return std::string(m_decoder->backendName()); }

int VideoFrameSource::streamIndex() const { return m_decoder->streamInfo().index; }

// --- Audio ---------------------------------------------------------------------------------------------------

std::expected<std::vector<float>, Error> readMonoAudio(const std::string& path, int containerStream, int sampleRate,
                                                       int* streamIndex, const AnalysisProgress& progress) {
    const Rate rate{sampleRate, 1};
    auto decoder = media::AudioDecoder::open(path, containerStream, rate, media::ChannelLayout::mono());
    if (!decoder) {
        return std::unexpected(decoder.error());
    }
    if (streamIndex) {
        *streamIndex = (*decoder)->streamInfo().index;
    }
    const Ticks total = (*decoder)->streamInfo().duration;
    std::vector<float> samples;
    std::vector<float> chunk(static_cast<std::size_t>(sampleRate));
    std::int64_t position = 0;
    for (int guard = 0; guard < 24 * 3600; ++guard) {
        auto read = (*decoder)->read(samplesToTicks(position, rate), chunk);
        if (!read) {
            return std::unexpected(read.error());
        }
        const std::size_t used = read->endOfStream ? read->leadingSilence + read->framesFromStream : chunk.size();
        samples.insert(samples.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(std::min(used, chunk.size())));
        position += static_cast<std::int64_t>(chunk.size());
        if (read->endOfStream || (read->framesFromStream == 0 && read->leadingSilence == 0)) {
            break;
        }
        if (progress && total.value > 0 && guard % 10 == 0 &&
            !progress(std::clamp(static_cast<double>(samplesToTicks(position, rate).value) / static_cast<double>(total.value), 0.0, 1.0) * 0.5)) {
            return std::unexpected(Error{ErrorCode::Cancelled, "reading the audio was stopped"});
        }
    }
    return samples;
}

// --- MediaAnalyzer ---------------------------------------------------------------------------------------------

MediaAnalyzer::MediaAnalyzer(MediaAnalyzerOptions options) : m_options(std::move(options)) {}

std::expected<ShotAnalysis, Error> MediaAnalyzer::shots(const std::string& path, int containerStream,
                                                        const ShotOptions& options, const AnalysisProgress& progress,
                                                        Stats* stats) {
    Stats local;
    Stats& s = stats ? *stats : local;
    s = Stats{};
    const auto fingerprint = media::sourceFingerprint(path);
    if (!fingerprint) {
        return std::unexpected(fingerprint.error());
    }
    auto source = VideoFrameSource::open(path, containerStream, m_options.frameWidth);
    if (!source) {
        return std::unexpected(source.error());
    }
    s.backend = (*source)->backend();
    const AnalysisKey key{*fingerprint, (*source)->streamIndex(), "shots",
                          std::string(ShotAnalysis::kAnalyzer) + "-" + std::to_string(ShotAnalysis::kVersion),
                          hashParams({{"options", options.fingerprint()}, {"width", std::to_string(m_options.frameWidth)}})};
    std::optional<AnalysisCache> cache;
    if (!m_options.cacheRoot.empty()) {
        cache.emplace(m_options.cacheRoot);
        if (m_options.useCache) {
            if (auto hit = cache->get(key); hit && *hit) {
                if (auto cached = shotsFromJson(**hit)) {
                    s.fromCache = true;
                    s.mediaDuration = cached->end - cached->start;
                    return cached;
                }
            }
        }
    }
    const auto start = std::chrono::steady_clock::now();
    auto result = detectShots(**source, options, progress);
    s.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (!result) {
        return result;
    }
    s.mediaDuration = result->end - result->start;
    if (cache) {
        (void)cache->put(key, toJson(*result));
    }
    return result;
}

std::expected<BeatAnalysis, Error> MediaAnalyzer::beats(const std::string& path, int containerStream,
                                                        const BeatOptions& options, const AnalysisProgress& progress,
                                                        Stats* stats) {
    Stats local;
    Stats& s = stats ? *stats : local;
    s = Stats{};
    auto tracker = makeBeatTracker(options.tracker);
    if (!tracker) {
        return std::unexpected(Error{ErrorCode::NotFound, "no beat tracker '" + options.tracker + "'"});
    }
    const auto fingerprint = media::sourceFingerprint(path);
    if (!fingerprint) {
        return std::unexpected(fingerprint.error());
    }
    int stream = containerStream;
    if (stream < 0) {
        auto probe = media::AudioDecoder::open(path, -1, Rate{m_options.beatSampleRate, 1}, media::ChannelLayout::mono());
        if (!probe) {
            return std::unexpected(probe.error());
        }
        stream = (*probe)->streamInfo().index;
    }
    const AnalysisKey key{*fingerprint, stream, "beats", std::string(BeatAnalysis::kAnalyzer) + "-" + tracker->id(),
                          hashParams({{"options", options.fingerprint()}, {"rate", std::to_string(m_options.beatSampleRate)}})};
    std::optional<AnalysisCache> cache;
    if (!m_options.cacheRoot.empty()) {
        cache.emplace(m_options.cacheRoot);
        if (m_options.useCache) {
            if (auto hit = cache->get(key); hit && *hit) {
                if (auto cached = beatsFromJson(**hit)) {
                    s.fromCache = true;
                    s.mediaDuration = cached->duration;
                    return cached;
                }
            }
        }
    }
    const auto start = std::chrono::steady_clock::now();
    auto samples = readMonoAudio(path, stream, m_options.beatSampleRate, nullptr, progress);
    if (!samples) {
        return std::unexpected(samples.error());
    }
    const AnalysisProgress second = progress ? AnalysisProgress([&](double f) { return progress(0.5 + 0.5 * f); })
                                             : AnalysisProgress{};
    auto result = tracker->track(MonoAudio{*samples, m_options.beatSampleRate, Ticks{}}, options, second);
    s.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (!result) {
        return result;
    }
    s.mediaDuration = result->duration;
    if (cache) {
        (void)cache->put(key, toJson(*result));
    }
    return result;
}

// --- MediaAnalysisProvider -------------------------------------------------------------------------------------

MediaAnalysisProvider::MediaAnalysisProvider(MediaAnalyzerOptions options, std::filesystem::path projectFile)
    : m_analyzer(std::move(options)), m_projectFile(std::move(projectFile)) {}

std::expected<std::string, Error> MediaAnalysisProvider::pathOf(const MediaItem& media) const {
    if (media.isGenerator()) {
        return std::unexpected(Error{ErrorCode::Unsupported, "'" + media.name + "' is generated: there is no file to analyse"});
    }
    if (auto path = resolveMediaPath(media.locator, m_projectFile)) {
        return pathToUtf8(*path);
    }
    return std::unexpected(Error{ErrorCode::NotFound, "'" + media.name + "' is offline"});
}

std::expected<ShotAnalysis, Error> MediaAnalysisProvider::shots(const MediaItem& media, int videoStream,
                                                                const ShotOptions& options, const AnalysisProgress& progress) {
    auto path = pathOf(media);
    if (!path) {
        return std::unexpected(path.error());
    }
    const int container = videoStream >= 0 && static_cast<std::size_t>(videoStream) < media.info.video.size()
                              ? media.info.video[static_cast<std::size_t>(videoStream)].index
                              : -1;
    std::lock_guard lock(m_mutex);
    return m_analyzer.shots(*path, container, options, progress);
}

std::expected<BeatAnalysis, Error> MediaAnalysisProvider::beats(const MediaItem& media, int audioStream,
                                                                const BeatOptions& options, const AnalysisProgress& progress) {
    auto path = pathOf(media);
    if (!path) {
        return std::unexpected(path.error());
    }
    const int container = audioStream >= 0 && static_cast<std::size_t>(audioStream) < media.info.audio.size()
                              ? media.info.audio[static_cast<std::size_t>(audioStream)].index
                              : -1;
    std::lock_guard lock(m_mutex);
    return m_analyzer.beats(*path, container, options, progress);
}

} // namespace wabcut::ai::analysis
