#pragma once

// G.2 gameplay highlights, the decoding side (full builds only: wabcut_ai_highlights_media links wabcut_media and
// FFmpeg): measures recordings for the highlight finder and keeps the measurements in the AnalysisCache by source
// fingerprint, so a second run (other signals, another chat log, other settings) decodes nothing.
//
//   measureAudio    one audio stream through AudioFeatureExtractor, streamed (never the whole file in memory)
//   measureVideo    the picture through VisualFeatureExtractor (every frame, decoded small, hardware when it can)
//   readChapters    a recording's chapter markers (OBS "Add Chapter Marker" in hybrid MP4 / MKV)
//   findMarkerFiles marker files next to a recording ("<file>.markers.txt", "<stem>_markers.csv", ...)
//   MediaHighlightProvider  the commands' HighlightProvider over a project's media

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/highlights/HighlightCommands.h>
#include <wabcut/model/GamingOps.h>

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace wabcut::ai::highlights {

/// `containerStream`: the stream's container index (AudioStreamInfo::index). Errors: the decoder's, Cancelled.
std::expected<AudioFeatures, Error> measureAudio(const std::string& path, int containerStream, const analysis::AnalysisProgress& progress = {});
/// `containerStream` -1 = the best video stream.
std::expected<VisualFeatures, Error> measureVideo(const std::string& path, int containerStream, int width = 160,
                                                  const analysis::AnalysisProgress& progress = {});
/// Chapter starts with their titles (source "chapter"); none when the file has no chapters. Errors: IoError.
std::expected<std::vector<Hint>, Error> readChapters(const std::string& path);
/// Markers from the marker files next to `recording` (source "marker file"); `notes` say which file was read or why
/// one could not be.
std::vector<Hint> findMarkerFiles(const std::filesystem::path& recording, std::vector<std::string>* notes = nullptr);

/// C.16's media activity (sequence.removeDeadTime) from the highlight measurements already in the analysis cache, so
/// nothing is decoded again: the loudest audio stream's level (-60..0 dBFS as 0..1) and the picture's motion (mean
/// absolute luma change, 0..1), 10 samples a second. nullopt when the recording was never measured. For
/// editor::activityMeasure's `fromAnalysis`.
std::optional<MediaActivity> cachedActivity(const MediaItem& media, const std::filesystem::path& cacheRoot,
                                            const std::filesystem::path& projectFile, int frameWidth = 160);

struct MediaHighlightOptions {
    /// The analysis cache (AnalysisCache::defaultRoot()); empty: no cache.
    std::filesystem::path cacheRoot;
    bool useCache = true;
    int frameWidth = 160;
};

class MediaHighlightProvider : public HighlightProvider {
public:
    /// `projectFile` resolves media paths relative to the project.
    MediaHighlightProvider(MediaHighlightOptions options, std::filesystem::path projectFile);
    std::expected<Measurement, Error> measure(const MediaItem& media, bool video, const analysis::AnalysisProgress& progress) override;
    void remember(const MediaItem& media, const std::vector<Moment>& moments) override;
    std::expected<void, Error> keepChat(const MediaItem& media, const ChatLog& chat, double offsetSeconds) override;
    std::optional<KeptChat> chatFor(const MediaItem& media) override;

private:
    std::expected<std::string, Error> pathOf(const MediaItem& media) const;
    std::expected<AnalysisKey, Error> chatKey(const MediaItem& media) const;
    MediaHighlightOptions m_options;
    std::filesystem::path m_projectFile;
    std::mutex m_mutex;
};

} // namespace wabcut::ai::highlights
