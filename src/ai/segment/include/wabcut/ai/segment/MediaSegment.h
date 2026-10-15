#pragma once

// The media side of the AI Object Mask (AI-14; full builds only: wabcut_ai_segment_media links wabcut_media and
// wabcut_render):
//
//   MediaFrameSource      a video's pictures for the model: decoded (hardware when available), as STORED (masks and
//                         mattes live in stored-frame coordinates), stretched to the model's square input. Backward
//                         runs decode a block of frames ahead and keep it, so they do not seek at every frame.
//   mediaSegmentServices  the object-mask commands' services for a project file: the pictures through the media's
//                         locator, source fingerprints, SAM 2.1 in wabcut-embed (one model kept loaded per services and
//                         lent to one run at a time, so a refining click does not reload it), the subject detector
//                         (faces and COCO objects, when downloaded) and the analysis cache.
//   installMatteSource    the compositor's render::MatteSource over the analysis cache (CacheMatteSource), for preview,
//                         export, thumbnails and scopes alike.

#include <wabcut/ai/segment/HelperSegmenter.h>
#include <wabcut/ai/segment/MatteCache.h>
#include <wabcut/ai/segment/ObjectMask.h>

#include <filesystem>
#include <memory>
#include <string>

namespace wabcut::ai::segment {

class MediaFrameSource final : public FrameSource {
public:
    /// `containerStream`: VideoStreamInfo::index, -1 = the best video stream. Errors: the decoder's.
    static std::expected<std::unique_ptr<MediaFrameSource>, Error> open(const std::string& path, int containerStream);
    ~MediaFrameSource() override;

    std::expected<search::Image, Error> pictureAt(Ticks mediaTime, int size) override;
    Ticks frameDuration() const override;
    TimeRange frames() const override;
    double aspect() const override;
    /// Frames decoded so far (tests: backward runs decode in blocks).
    int decoded() const;

private:
    MediaFrameSource();
    struct Impl;
    std::unique_ptr<Impl> m;
};

struct MediaSegmentOptions {
    std::filesystem::path projectFile; ///< resolves relative media paths
    std::filesystem::path cacheRoot;   ///< the analysis cache; empty = AnalysisCache::defaultRoot()
    std::filesystem::path modelRoot;   ///< the model store; empty = ModelStore::defaultRoot()
    std::filesystem::path helper;      ///< wabcut-embed; empty = next to the running executable
    std::vector<std::string> helperArguments; ///< tests script a fake helper with them
    bool detector = true;              ///< offer "subject" prompts when the detectors are downloaded
};

/// The services of mask.objectSelect / refine / propagate for a project. Remembers new tracks in the installed matte
/// source (installMatteSource) when there is one.
std::shared_ptr<SegmentServices> mediaSegmentServices(MediaSegmentOptions options);

/// Installs a CacheMatteSource over `cacheRoot` (empty = AnalysisCache::defaultRoot()) as the process's matte source,
/// or returns the one already installed for that root.
std::shared_ptr<CacheMatteSource> installMatteSource(const std::filesystem::path& cacheRoot = {});

} // namespace wabcut::ai::segment
