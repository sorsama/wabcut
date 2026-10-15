#pragma once

// The media-backed SearchMediaAccess (full builds only: wabcut_ai_search_media links wabcut_media): files found
// through their locators, identified by media::sourceFingerprint, shots from the C.5 MediaAnalyzer (cached), keyframes
// decoded with media::VideoDecoder (sample aspect and display rotation applied) and transcripts from the analysis
// cache (CachedTranscriptProvider).

#include <wabcut/ai/search/FootageSearch.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace wabcut::media {
class VideoDecoder;
struct FrameHandle;
}

namespace wabcut::ai::search {

/// A video's pictures at non-decreasing times (one decoder kept open, so it decodes forward without seeking), as the
/// viewer sees them (sample aspect and rotation applied), `width` pixels wide at most. AI-9's subject analysis reads
/// its frames with it.
class VideoPictureReader {
public:
    /// `containerStream`: VideoStreamInfo::index, -1 = the best video stream. Errors: the decoder's.
    static std::expected<std::unique_ptr<VideoPictureReader>, Error> open(const std::string& path, int containerStream, int width);
    ~VideoPictureReader();
    /// The picture on screen at `time` (source time). Errors: NotFound past the last frame, the decoder's.
    std::expected<Image, Error> at(Ticks time);
    /// The decoder's backend ("software", "d3d11va", ...).
    std::string backend() const;
    /// The container index of the stream it decodes.
    int streamIndex() const;

private:
    VideoPictureReader() = default;
    std::unique_ptr<media::VideoDecoder> m_decoder;
    int m_width = 0;
};

/// A decoded frame as STORED (no rotation, no mirroring, sample aspect ignored), resampled to exactly `width` x
/// `height` RGB: the picture masks and mattes are drawn in (normalised stored-frame coordinates, AI-14). Invalid when
/// the frame cannot be read.
Image storedPicture(const media::FrameHandle& frame, int width, int height);

struct MediaSearchAccessOptions {
    std::filesystem::path projectFile; ///< resolves project-relative media paths
    std::filesystem::path cacheRoot;   ///< the AnalysisCache (shots, transcripts); empty = no cache
};

class MediaSearchAccess : public SearchMediaAccess {
public:
    explicit MediaSearchAccess(MediaSearchAccessOptions options);

    void setProjectFile(std::filesystem::path projectFile);

    std::optional<std::uint64_t> fingerprint(const MediaItem& media) override;
    std::string date(const MediaItem& media) override;
    std::expected<analysis::ShotAnalysis, Error> shots(const MediaItem& media, const analysis::AnalysisProgress& progress) override;
    std::expected<std::vector<Image>, Error> frames(const MediaItem& media, std::span<const Ticks> times, int width,
                                                    std::stop_token stop) override;
    std::shared_ptr<const Transcript> transcript(const MediaItem& media) override;

private:
    std::optional<std::string> pathOf(const MediaItem& media) const;

    mutable std::mutex m_mutex;
    MediaSearchAccessOptions m_options;
};

/// Where an editor's smart search lives. Defaults: the shared index in the user cache, the AnalysisCache, the
/// ModelStore and the wabcut-embed next to the running executable.
struct SearchHostOptions {
    std::filesystem::path projectFile;
    std::filesystem::path indexDir;  ///< SearchIndex::defaultDir() when empty
    std::filesystem::path cacheRoot; ///< AnalysisCache::defaultRoot() when empty
    std::filesystem::path modelRoot; ///< ModelStore::defaultRoot() when empty
    std::filesystem::path helper;    ///< defaultEmbedExecutable() when empty
    int threads = 0;                 ///< helper CPU threads; 0 = half the cores (indexing runs beside editing)
};

/// Why visual search cannot run here ("" when the helper is built and the SigLIP 2 files are downloaded).
std::string visualSearchUnavailableReason(const SearchHostOptions& options);

/// The editor's smart search over `options.projectFile`'s media: SigLIP 2 in wabcut-embed when available, otherwise
/// transcripts only (with the reason set). Errors: the index's (IoError, Unsupported).
std::expected<std::shared_ptr<FootageSearch>, Error> openFootageSearch(const SearchHostOptions& options);

} // namespace wabcut::ai::search
