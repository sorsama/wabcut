#pragma once

// Media-backed subject analysis (full builds only: wabcut_ai_vision_media links wabcut_media): a video's frames at
// 640 px (search::VideoPictureReader: sample aspect and rotation applied), its C.5 shots (MediaAnalyzer, cached), the
// detectors in wabcut-embed, and the result cached in the AnalysisCache by source fingerprint, stream, detector version
// and sampling settings, so a second reframe or shorts run over the same file costs nothing.

#include <wabcut/ai/vision/HelperDetector.h>
#include <wabcut/ai/vision/Subjects.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace wabcut::ai::vision {

struct MediaSubjectOptions {
    std::filesystem::path projectFile; ///< resolves project-relative media paths
    std::filesystem::path cacheRoot;   ///< the AnalysisCache (shots and subjects); empty = no cache
    int width = 640;                   ///< analysis picture width
    SubjectOptions analysis;
};

/// Statistics of the last analysis (perf reports, the job pill).
struct SubjectStats {
    bool fromCache = false;
    double seconds = 0.0;   ///< wall time (0 from the cache)
    double analysedSeconds = 0.0;
    std::size_t samples = 0;
    std::string backend;    ///< the video decoder's
    std::string device;     ///< the detectors' ("" = saliency only)
};

class MediaSubjectProvider : public SubjectProvider {
public:
    /// `detector` may be null: saliency only (the models are not downloaded or the helper is not built).
    MediaSubjectProvider(MediaSubjectOptions options, std::shared_ptr<SubjectDetector> detector);

    void setProjectFile(std::filesystem::path projectFile);
    std::expected<SubjectAnalysis, Error> subjects(const MediaItem& media, int videoStream,
                                                   const std::function<bool(double)>& progress) override;
    std::optional<SubjectAnalysis> cached(const MediaItem& media, int videoStream) override;
    SubjectStats lastStats() const;

private:
    std::expected<std::string, Error> pathOf(const MediaItem& media) const;

    MediaSubjectOptions m_options;
    std::shared_ptr<SubjectDetector> m_detector;
    mutable std::mutex m_mutex;
    SubjectStats m_stats;
};

/// Analyses a file directly (the CLI and perf runs). `containerStream` -1 = the best video stream.
std::expected<SubjectAnalysis, Error> analyzeFileSubjects(const std::string& path, int containerStream, SubjectDetector* detector,
                                                          const MediaSubjectOptions& options, const SubjectProgress& progress = {},
                                                          SubjectStats* stats = nullptr, std::stop_token stop = {});

/// A cached analysis of a file (no decoding beyond the fingerprint), or nullopt.
std::optional<SubjectAnalysis> cachedFileSubjects(const std::string& path, int containerStream, const SubjectDetector* detector,
                                                  const MediaSubjectOptions& options);

/// Where the editor's subject analysis comes from: the downloaded detectors in wabcut-embed, or nothing (saliency
/// only) with the reason. `helper` empty = next to the running executable; `modelRoot` empty = ModelStore::defaultRoot().
struct SubjectDetectorChoice {
    std::shared_ptr<HelperDetector> detector;
    std::string unavailableReason;
};
SubjectDetectorChoice openSubjectDetector(const std::filesystem::path& modelRoot, const std::filesystem::path& helper, int threads = 0);

} // namespace wabcut::ai::vision
