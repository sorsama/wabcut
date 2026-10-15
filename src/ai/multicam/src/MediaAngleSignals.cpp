#include <wabcut/ai/multicam/MediaAngleSignals.h>

#include <wabcut/media/AudioDecoder.h>
#include <wabcut/model/MediaLocator.h>

#include <cmath>
#include <map>
#include <mutex>
#include <tuple>

namespace wabcut::ai::multicam {

namespace {

constexpr int kRate = 16000;

class MediaAngleSignals final : public AngleSignals {
public:
    MediaAngleSignals(std::shared_ptr<vision::SubjectProvider> faces, std::filesystem::path projectFile)
        : m_faces(std::move(faces)), m_projectFile(std::move(projectFile)) {}

    std::expected<std::vector<float>, Error> levels(const MediaItem& media, int audioStream, TimeRange source, Ticks frame,
                                                    std::stop_token stop) override {
        if (audioStream < 0 || static_cast<std::size_t>(audioStream) >= media.info.audio.size()) {
            return std::unexpected(Error{ErrorCode::NotFound, "'" + media.name + "' has no audio stream " + std::to_string(audioStream + 1)});
        }
        const auto path = resolveMediaPath(media.locator, m_projectFile);
        if (!path) {
            return std::unexpected(Error{ErrorCode::NotFound, "'" + media.name + "' is offline"});
        }
        const auto key = std::make_tuple(pathToUtf8(*path), audioStream, source.start.value, source.duration.value, frame.value);
        {
            std::lock_guard lock(m_mutex);
            if (const auto it = m_levels.find(key); it != m_levels.end()) {
                return it->second;
            }
        }
        auto decoder = media::AudioDecoder::open(pathToUtf8(*path), media.info.audio[static_cast<std::size_t>(audioStream)].index,
                                                 Rate{kRate, 1}, 1);
        if (!decoder) {
            return std::unexpected(decoder.error());
        }
        const std::int64_t frameSamples = std::max<std::int64_t>(1, frame.value * kRate / kTicksPerSecond);
        const std::int64_t total = source.duration.value * kRate / kTicksPerSecond;
        std::vector<float> out;
        out.reserve(static_cast<std::size_t>(total / frameSamples + 1));
        std::vector<float> block(static_cast<std::size_t>(kRate * 4));
        std::int64_t at = 0;
        double sum = 0;
        std::int64_t count = 0;
        while (at < total) {
            if (stop.stop_requested()) {
                return std::unexpected(Error{ErrorCode::Cancelled, "cancelled"});
            }
            const std::size_t want = static_cast<std::size_t>(std::min<std::int64_t>(static_cast<std::int64_t>(block.size()), total - at));
            auto read = (*decoder)->read(source.start + samplesToTicks(at, Rate{kRate, 1}), std::span<float>(block.data(), want));
            if (!read) {
                return std::unexpected(read.error());
            }
            for (std::size_t i = 0; i < want; ++i) {
                sum += static_cast<double>(block[i]) * block[i];
                if (++count == frameSamples) {
                    const double rms = std::sqrt(sum / static_cast<double>(count));
                    out.push_back(rms > 1e-5 ? static_cast<float>(20.0 * std::log10(rms)) : -100.0f);
                    sum = 0;
                    count = 0;
                }
            }
            at += static_cast<std::int64_t>(want);
            if (read->endOfStream) {
                break;
            }
        }
        if (count > 0) {
            const double rms = std::sqrt(sum / static_cast<double>(count));
            out.push_back(rms > 1e-5 ? static_cast<float>(20.0 * std::log10(rms)) : -100.0f);
        }
        std::lock_guard lock(m_mutex);
        m_levels[key] = out;
        return out;
    }

    std::expected<vision::SubjectAnalysis, Error> faces(const MediaItem& media, int videoStream, const std::function<bool(double)>& progress) override {
        if (!m_faces) {
            return std::unexpected(Error{ErrorCode::Unsupported, "no face analysis here"});
        }
        return m_faces->subjects(media, videoStream, progress);
    }

private:
    std::shared_ptr<vision::SubjectProvider> m_faces;
    std::filesystem::path m_projectFile;
    std::mutex m_mutex;
    std::map<std::tuple<std::string, int, std::int64_t, std::int64_t, std::int64_t>, std::vector<float>> m_levels;
};

} // namespace

std::vector<float> rmsLevels(std::span<const float> samples, int rate, Ticks frame) {
    const std::int64_t n = std::max<std::int64_t>(1, frame.value * rate / kTicksPerSecond);
    std::vector<float> out;
    for (std::size_t i = 0; i < samples.size(); i += static_cast<std::size_t>(n)) {
        double sum = 0;
        const std::size_t end = std::min(samples.size(), i + static_cast<std::size_t>(n));
        for (std::size_t k = i; k < end; ++k) {
            sum += static_cast<double>(samples[k]) * samples[k];
        }
        const double rms = std::sqrt(sum / static_cast<double>(end - i));
        out.push_back(rms > 1e-5 ? static_cast<float>(20.0 * std::log10(rms)) : -100.0f);
    }
    return out;
}

std::shared_ptr<AngleSignals> makeMediaAngleSignals(std::shared_ptr<vision::SubjectProvider> faces, std::filesystem::path projectFile) {
    return std::make_shared<MediaAngleSignals>(std::move(faces), std::move(projectFile));
}

} // namespace wabcut::ai::multicam
