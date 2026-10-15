#include <wabcut/editor/Activity.h>

#include <wabcut/media/VideoDecoder.h>
#include <wabcut/media/WaveformPeaks.h>
#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>

namespace wabcut::editor {

namespace {

/// The luma of a decoded frame shrunk to a grid (cells averaged over every 4th pixel each way), 0..255.
std::optional<std::vector<float>> lumaGrid(const media::FrameHandle& frame, int gridW, int gridH) {
    const media::CpuPlanes* cpu = frame.cpu();
    if (!cpu || frame.width <= 0 || frame.height <= 0) {
        return std::nullopt;
    }
    const media::Plane& p = cpu->planes[0];
    if (!p.data) {
        return std::nullopt;
    }
    using media::PixelFormat;
    const auto luma = [&](int x, int y) -> float {
        const std::uint8_t* row = p.data + static_cast<std::ptrdiff_t>(y) * p.stride;
        switch (frame.format) {
        case PixelFormat::NV12:
        case PixelFormat::YUV420P:
        case PixelFormat::YUV422P:
        case PixelFormat::YUV444P:
        case PixelFormat::YUVA420P:
            return row[x];
        case PixelFormat::P010: {
            std::uint16_t v = 0;
            std::memcpy(&v, row + static_cast<std::ptrdiff_t>(x) * 2, 2);
            return static_cast<float>(v >> 8);
        }
        case PixelFormat::YUV420P10:
        case PixelFormat::YUV422P10:
        case PixelFormat::YUV444P10:
        case PixelFormat::YUVA444P10: {
            std::uint16_t v = 0;
            std::memcpy(&v, row + static_cast<std::ptrdiff_t>(x) * 2, 2);
            return static_cast<float>((v & 0x3FF) >> 2);
        }
        case PixelFormat::RGBA8: {
            const std::uint8_t* px = row + static_cast<std::ptrdiff_t>(x) * 4;
            return static_cast<float>((54 * px[0] + 183 * px[1] + 19 * px[2]) >> 8);
        }
        case PixelFormat::Unknown:
            break;
        }
        return 0.0F;
    };
    if (frame.format == media::PixelFormat::Unknown) {
        return std::nullopt;
    }
    const int width = std::min(frame.width, p.width);
    const int height = std::min(frame.height, p.height);
    std::vector<float> grid(static_cast<std::size_t>(gridW) * static_cast<std::size_t>(gridH), 0.0F);
    for (int gy = 0; gy < gridH; ++gy) {
        const int y0 = gy * height / gridH;
        const int y1 = std::max(y0 + 1, (gy + 1) * height / gridH);
        for (int gx = 0; gx < gridW; ++gx) {
            const int x0 = gx * width / gridW;
            const int x1 = std::max(x0 + 1, (gx + 1) * width / gridW);
            float sum = 0.0F;
            int count = 0;
            for (int y = y0; y < y1; y += 4) {
                for (int x = x0; x < x1; x += 4) {
                    sum += luma(x, y);
                    ++count;
                }
            }
            grid[static_cast<std::size_t>(gy) * static_cast<std::size_t>(gridW) + static_cast<std::size_t>(gx)] =
                count > 0 ? sum / static_cast<float>(count) : 0.0F;
        }
    }
    return grid;
}

std::string mediaKey(const MediaItem& item) {
    return item.locator.path + "|" + std::to_string(item.locator.fingerprint.size) + "|" + item.locator.fingerprint.sampleHash +
           "|" + std::to_string(item.locator.fingerprint.mtimeNs) + "|" + (item.locator.proxy ? item.locator.proxy->path : "");
}

} // namespace

std::expected<MediaActivity, Error> measureMediaActivity(const MediaItem& item, const std::filesystem::path& projectFile,
                                                         const std::function<bool(double)>& progress,
                                                         const ActivityOptions& options) {
    if (options.samplesPerSecond < 1 || options.samplesPerSecond > 30 || options.gridWidth < 8 || options.gridWidth > 256) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "activity options out of range"});
    }
    if (item.isGenerator()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "generated media has no activity to measure"});
    }
    const auto original = resolveMediaPath(item.locator, projectFile);
    std::optional<std::filesystem::path> proxy;
    if (options.preferProxy && item.locator.proxy) {
        std::error_code ec;
        const std::filesystem::path p = pathFromUtf8(item.locator.proxy->path);
        if (std::filesystem::is_regular_file(p, ec)) {
            proxy = p;
        }
    }
    if (!original && !proxy) {
        return std::unexpected(Error{ErrorCode::NotFound, "'" + item.name + "' is offline"});
    }
    const auto keepGoing = [&](double fraction) { return !progress || progress(std::clamp(fraction, 0.0, 1.0)); };
    const int sps = options.samplesPerSecond;
    MediaActivity out;
    out.step = Ticks{kTicksPerSecond / sps};
    out.source = proxy ? "proxy" : "original";
    const bool hasVideo = !item.info.video.empty() && !item.info.isStillImage;
    const bool hasAudio = !item.info.audio.empty() && original.has_value();
    const double videoShare = hasAudio ? 0.8 : 1.0;

    if (hasVideo) {
        const VideoStreamInfo& stream = item.info.video.front();
        const Ticks duration = stream.duration.value > 0 ? stream.duration : item.info.duration;
        const std::int64_t count = std::min<std::int64_t>(
            static_cast<std::int64_t>(MediaActivity::kMaxSamples),
            (duration.value + out.step.value - 1) / std::max<std::int64_t>(1, out.step.value));
        media::DecoderOptions decode;
        decode.outputMode = media::OutputMode::Cpu;
        auto decoder = media::VideoDecoder::open(pathToUtf8(proxy ? *proxy : *original), proxy ? -1 : stream.index, decode);
        if (!decoder) {
            return std::unexpected(decoder.error());
        }
        const int gridW = options.gridWidth;
        const int gridH = std::max(1, static_cast<int>(std::lround(static_cast<double>(gridW) * std::max(1, stream.height) /
                                                                    std::max(1, stream.width))));
        std::optional<std::vector<float>> previous;
        for (std::int64_t i = 0; i < count; ++i) {
            if (!keepGoing(videoShare * static_cast<double>(i) / static_cast<double>(count))) {
                return std::unexpected(Error{ErrorCode::Cancelled, "measuring activity was stopped"});
            }
            auto frame = (*decoder)->frameAt(Ticks{out.step.value * i + out.step.value / 2});
            if (!frame) {
                if (frame.error().code == ErrorCode::NotFound) {
                    break; // past the last frame
                }
                return std::unexpected(frame.error());
            }
            auto grid = lumaGrid(*frame, gridW, gridH);
            if (!grid) {
                return std::unexpected(Error{ErrorCode::Unsupported, "frames of '" + item.name + "' cannot be read"});
            }
            float motion = 0.0F;
            if (previous) {
                double sum = 0.0;
                for (std::size_t k = 0; k < grid->size(); ++k) {
                    sum += std::abs((*grid)[k] - (*previous)[k]);
                }
                motion = static_cast<float>(std::clamp(sum / static_cast<double>(grid->size()) / 255.0 * 4.0, 0.0, 1.0));
            }
            out.motion.push_back(motion);
            previous = std::move(grid);
        }
        if (out.motion.size() > 1) {
            out.motion[0] = out.motion[1]; // the first sample has nothing to compare with
        }
    }

    if (hasAudio) {
        const AudioStreamInfo& stream = item.info.audio.front();
        const int rate = stream.sampleRate.den > 0 ? stream.sampleRate.num / stream.sampleRate.den : 48000;
        media::WaveformOptions waveform;
        waveform.stream = stream.index;
        waveform.samplesPerBin = {std::max(16, rate / sps)};
        std::stop_source stop;
        auto peaks = media::computeWaveformPeaks(pathToUtf8(*original), waveform, stop.get_token(), [&](float f) {
            if (!keepGoing(videoShare + (1.0 - videoShare) * static_cast<double>(f))) {
                stop.request_stop();
            }
        });
        // A stop asked for in the last progress report finds the peaks done (a short file is one read block): stopped.
        if (!peaks || stop.stop_requested()) {
            if (stop.stop_requested() || peaks.error().code == ErrorCode::Cancelled) {
                return std::unexpected(Error{ErrorCode::Cancelled, "measuring activity was stopped"});
            }
            return std::unexpected(peaks.error());
        }
        if (!peaks->levels.empty() && peaks->channels > 0) {
            const media::PeakLevel& level = peaks->levels.front();
            const std::int64_t bins = std::min<std::int64_t>(level.binCount, static_cast<std::int64_t>(MediaActivity::kMaxSamples));
            out.audio.reserve(static_cast<std::size_t>(bins));
            for (std::int64_t b = 0; b < bins; ++b) {
                int peak = 0;
                for (int ch = 0; ch < peaks->channels; ++ch) {
                    peak = std::max({peak, std::abs(static_cast<int>(level.minAt(b, ch, peaks->channels))),
                                     std::abs(static_cast<int>(level.maxAt(b, ch, peaks->channels)))});
                }
                const double amplitude = std::min(1.0, static_cast<double>(peak) / 32767.0);
                const double db = amplitude > 0.0 ? 20.0 * std::log10(amplitude) : -120.0;
                out.audio.push_back(static_cast<float>(std::clamp((db + 60.0) / 60.0, 0.0, 1.0)));
            }
        }
    }
    (void)keepGoing(1.0);
    if (out.audio.empty() && out.motion.empty()) {
        return std::unexpected(Error{ErrorCode::Unsupported, "'" + item.name + "' has neither picture nor sound to measure"});
    }
    if (const auto problem = mediaActivityProblem(out)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, *problem});
    }
    return out;
}

command::ActivityServices::Measure activityMeasure(std::filesystem::path projectFile,
                                                   std::function<std::optional<MediaActivity>(const MediaItem&)> fromAnalysis,
                                                   ActivityOptions options) {
    struct Cache {
        std::mutex mutex;
        std::map<std::string, MediaActivity> measured;
    };
    auto cache = std::make_shared<Cache>();
    return [cache, projectFile = std::move(projectFile), fromAnalysis = std::move(fromAnalysis),
            options](const ProjectState& state, MediaId media, const std::function<bool(double)>& progress)
               -> std::expected<MediaActivity, Error> {
        const MediaItem* item = state.findMedia(media);
        if (!item) {
            return std::unexpected(Error{ErrorCode::NotFound, "no media " + std::to_string(media.value)});
        }
        if (fromAnalysis) {
            if (auto analysed = fromAnalysis(*item)) {
                return std::move(*analysed);
            }
        }
        const std::string key = mediaKey(*item);
        {
            const std::lock_guard lock(cache->mutex);
            if (const auto it = cache->measured.find(key); it != cache->measured.end()) {
                return it->second;
            }
        }
        auto measured = measureMediaActivity(*item, projectFile, progress, options);
        if (measured) {
            const std::lock_guard lock(cache->mutex);
            cache->measured[key] = *measured;
        }
        return measured;
    };
}

} // namespace wabcut::editor
