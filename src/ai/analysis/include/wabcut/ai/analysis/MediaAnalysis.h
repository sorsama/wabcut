#pragma once

// Media-backed analysis (full builds only: wabcut_ai_analysis_media links wabcut_media):
//
//   VideoFrameSource       every frame of a video stream, decoded (hardware when the decoder can) and shrunk to a
//                          small RGB picture (160 px wide by default) for the shot detectors
//   readMonoAudio          an audio stream as mono float samples at a chosen rate
//   MediaAnalyzer          shots and beats of a file, cached in the AnalysisCache by source fingerprint, stream,
//                          analyser version and settings (like transcripts, AI-2)
//   MediaAnalysisProvider  the commands' AnalysisProvider over a project's media (paths from their locators)

#include <wabcut/ai/analysis/AnalysisCommands.h>
#include <wabcut/ai/analysis/BeatDetection.h>
#include <wabcut/ai/analysis/ShotDetection.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace wabcut::media {
class VideoDecoder;
}

namespace wabcut::ai::analysis {

class VideoFrameSource : public FrameSource {
public:
    /// `containerStream`: VideoStreamInfo::index, -1 = the best video stream. Errors: the decoder's.
    static std::expected<std::unique_ptr<VideoFrameSource>, Error> open(const std::string& path, int containerStream,
                                                                        int width = 160);
    ~VideoFrameSource() override;
    std::expected<std::optional<AnalysisFrame>, Error> next() override;
    std::optional<Ticks> duration() const override;
    /// The decoder's backend ("software", "d3d11va", ...).
    std::string backend() const;
    /// The stream's container index.
    int streamIndex() const;

private:
    VideoFrameSource() = default;
    std::unique_ptr<media::VideoDecoder> m_decoder;
    int m_width = 160;
    Ticks m_next;
    bool m_done = false;
};

/// The audio stream as mono samples at `sampleRate` from the media origin. `containerStream` -1 = the best one;
/// `streamIndex` receives the stream's container index. Errors: the decoder's, Cancelled.
std::expected<std::vector<float>, Error> readMonoAudio(const std::string& path, int containerStream, int sampleRate,
                                                       int* streamIndex = nullptr, const AnalysisProgress& progress = {});

struct MediaAnalyzerOptions {
    /// The analysis cache (AnalysisCache::defaultRoot()); empty: no cache.
    std::filesystem::path cacheRoot;
    bool useCache = true;
    int frameWidth = 160;
    int beatSampleRate = 22050;
};

class MediaAnalyzer {
public:
    explicit MediaAnalyzer(MediaAnalyzerOptions options);

    struct Stats {
        bool fromCache = false;
        double seconds = 0.0;      ///< wall time of the analysis (0 from the cache)
        std::string backend;       ///< video decoder backend
        Ticks mediaDuration;       ///< length of what was analysed
    };

    /// `containerStream`: -1 = the best stream.
    std::expected<ShotAnalysis, Error> shots(const std::string& path, int containerStream, const ShotOptions& options,
                                             const AnalysisProgress& progress = {}, Stats* stats = nullptr);
    std::expected<BeatAnalysis, Error> beats(const std::string& path, int containerStream, const BeatOptions& options,
                                             const AnalysisProgress& progress = {}, Stats* stats = nullptr);

private:
    MediaAnalyzerOptions m_options;
};

class MediaAnalysisProvider : public AnalysisProvider {
public:
    /// `projectFile` resolves media paths relative to the project.
    MediaAnalysisProvider(MediaAnalyzerOptions options, std::filesystem::path projectFile);
    std::expected<ShotAnalysis, Error> shots(const MediaItem& media, int videoStream, const ShotOptions& options,
                                             const AnalysisProgress& progress) override;
    std::expected<BeatAnalysis, Error> beats(const MediaItem& media, int audioStream, const BeatOptions& options,
                                             const AnalysisProgress& progress) override;

private:
    std::expected<std::string, Error> pathOf(const MediaItem& media) const;
    MediaAnalyzer m_analyzer;
    std::filesystem::path m_projectFile;
    std::mutex m_mutex;
};

} // namespace wabcut::ai::analysis
