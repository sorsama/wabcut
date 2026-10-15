#include <wabcut/ai/segment/MediaSegment.h>

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/edit/MediaServices.h>
#include <wabcut/ai/search/MediaSearchAccess.h>
#include <wabcut/ai/vision/HelperDetector.h>
#include <wabcut/media/Fingerprint.h>
#include <wabcut/media/VideoDecoder.h>
#include <wabcut/render/Matte.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>

namespace wabcut::ai::segment {

// ---- MediaFrameSource ---------------------------------------------------------------------------------------------

namespace {
constexpr int kBackwardBlock = 12; ///< frames decoded at once when going backward (36 MB of 1024 x 1024 pictures)
}

struct MediaFrameSource::Impl {
    std::unique_ptr<media::VideoDecoder> decoder;
    Ticks frameDuration{};
    TimeRange frames;
    double aspect = 16.0 / 9.0;
    struct Picture {
        Ticks start{};
        Ticks end{};
        int size = 0;
        search::Image image;
    };
    std::deque<Picture> kept; ///< most recent first
    Ticks last{std::numeric_limits<std::int64_t>::min()};
    int decoded = 0;

    const Picture* find(Ticks t, int size) const {
        const auto it = std::ranges::find_if(kept, [&](const Picture& p) { return p.size == size && t >= p.start && t < p.end; });
        return it == kept.end() ? nullptr : &*it;
    }

    std::expected<const Picture*, Error> decode(Ticks t, int size) {
        auto frame = decoder->frameAt(std::max(t, decoder->firstFrameTime()));
        if (!frame) {
            return std::unexpected(frame.error());
        }
        ++decoded;
        if (const Picture* had = find(frame->pts, size)) {
            return had;
        }
        Picture p;
        p.start = frame->pts;
        p.end = frame->pts + (frame->duration.value > 0 ? frame->duration : frameDuration);
        p.size = size;
        p.image = search::storedPicture(*frame, size, size);
        if (!p.image.valid()) {
            return std::unexpected(Error(ErrorCode::DecoderError, "a frame of the video could not be read"));
        }
        kept.push_front(std::move(p));
        while (kept.size() > static_cast<std::size_t>(kBackwardBlock) + 2) {
            kept.pop_back();
        }
        return &kept.front();
    }
};

MediaFrameSource::MediaFrameSource() : m(std::make_unique<Impl>()) {}
MediaFrameSource::~MediaFrameSource() = default;

std::expected<std::unique_ptr<MediaFrameSource>, Error> MediaFrameSource::open(const std::string& path, int containerStream) {
    auto decoder = media::VideoDecoder::open(path, containerStream);
    if (!decoder) {
        return std::unexpected(decoder.error());
    }
    std::unique_ptr<MediaFrameSource> source(new MediaFrameSource());
    Impl& m = *source->m;
    m.decoder = std::move(*decoder);
    const VideoStreamInfo& v = m.decoder->streamInfo();
    m.frameDuration = v.frameRate.isValid() && v.frameRate.isExact() ? Ticks{v.frameRate.ticksPerUnit()} : Ticks{kTicksPerSecond / 30};
    const Ticks first = m.decoder->firstFrameTime();
    Ticks duration = v.duration;
    if (duration.value <= 0 && v.frameCount > 0) {
        duration = Ticks{m.frameDuration.value * v.frameCount};
    }
    m.frames = TimeRange{first, duration.value > 0 ? duration : m.frameDuration};
    const double par = v.sampleAspect.isValid() ? v.sampleAspect.toDouble() : 1.0;
    m.aspect = v.width > 0 && v.height > 0 ? v.width * par / v.height : 16.0 / 9.0;
    return source;
}

std::expected<search::Image, Error> MediaFrameSource::pictureAt(Ticks t, int size) {
    if (const Impl::Picture* had = m->find(t, size)) {
        m->last = t;
        return had->image;
    }
    if (t < m->last) {
        // Going backward: decode the block of frames up to `t` in one forward pass (one seek), and keep it.
        const Ticks from = std::max(m->frames.start, t - Ticks{m->frameDuration.value * (kBackwardBlock - 1)});
        for (Ticks u = from; u < t; u = u + m->frameDuration) {
            if (!m->find(u, size)) {
                if (auto got = m->decode(u, size); !got) {
                    return std::unexpected(got.error());
                }
            }
        }
    }
    auto got = m->decode(t, size);
    if (!got) {
        return std::unexpected(got.error());
    }
    m->last = t;
    return (*got)->image;
}

Ticks MediaFrameSource::frameDuration() const { return m->frameDuration; }
TimeRange MediaFrameSource::frames() const { return m->frames; }
double MediaFrameSource::aspect() const { return m->aspect; }
int MediaFrameSource::decoded() const { return m->decoded; }

// ---- Services -----------------------------------------------------------------------------------------------------

namespace {

/// One HelperSegmenter, lent to one run at a time (a run's prompt and tracked frames share the model's memory).
class SegmenterLender : public std::enable_shared_from_this<SegmenterLender> {
public:
    explicit SegmenterLender(HelperSegmenterOptions options) : m_options(std::move(options)) {}

    std::expected<std::shared_ptr<Segmenter>, Error> lend(std::stop_token stop) {
        std::unique_lock lock(m_mutex);
        while (m_busy) {
            if (stop.stop_requested()) {
                return std::unexpected(Error(ErrorCode::Cancelled, "cancelled"));
            }
            m_free.wait_for(lock, std::chrono::milliseconds(100));
        }
        if (!m_segmenter) {
            m_segmenter = std::make_shared<HelperSegmenter>(m_options);
        }
        m_busy = true;
        std::shared_ptr<HelperSegmenter> kept = m_segmenter;
        return std::shared_ptr<Segmenter>(kept.get(), [self = shared_from_this(), kept](Segmenter*) {
            std::lock_guard back(self->m_mutex);
            self->m_busy = false;
            self->m_free.notify_one();
        });
    }

private:
    HelperSegmenterOptions m_options;
    std::mutex m_mutex;
    std::condition_variable m_free;
    bool m_busy = false;
    std::shared_ptr<HelperSegmenter> m_segmenter;
};

std::mutex& installMutex() {
    static std::mutex mutex;
    return mutex;
}

} // namespace

std::shared_ptr<SegmentServices> mediaSegmentServices(MediaSegmentOptions o) {
    auto services = std::make_shared<SegmentServices>();
    const std::filesystem::path cacheRoot = o.cacheRoot.empty() ? AnalysisCache::defaultRoot() : o.cacheRoot;
    const ModelStore store(o.modelRoot.empty() ? ModelStore::defaultRoot() : o.modelRoot);
    HelperSegmenterOptions helper = segmenterOptions(store);
    helper.executable = o.helper;
    helper.arguments = o.helperArguments;
    const bool installed = segmentModelInstalled(store) || !o.helperArguments.empty();
    auto lender = std::make_shared<SegmenterLender>(helper);
    services->model = helper.modelVersion;
    services->segmenter = [lender, installed](std::stop_token stop) -> std::expected<std::shared_ptr<Segmenter>, Error> {
        if (!installed) {
            return std::unexpected(Error(ErrorCode::NotFound, "SAM 2.1, the AI Object Mask's model (190 MB), is not downloaded: the app downloads it "
                                                              "(Settings > AI > Local models)"));
        }
        return lender->lend(std::move(stop));
    };
    const std::filesystem::path projectFile = o.projectFile;
    const auto pathOf = [projectFile](const MediaItem& media) -> std::expected<std::string, Error> {
        auto path = edit::mediaFilePath(media, projectFile);
        if (!path) {
            return std::unexpected(Error(ErrorCode::NotFound, "'" + media.name + "' is offline"));
        }
        return *path;
    };
    services->frames = [pathOf](const MediaItem& media, int videoStream) -> std::expected<std::unique_ptr<FrameSource>, Error> {
        auto path = pathOf(media);
        if (!path) {
            return std::unexpected(path.error());
        }
        const int stream = videoStream >= 0 && static_cast<std::size_t>(videoStream) < media.info.video.size()
                               ? media.info.video[static_cast<std::size_t>(videoStream)].index
                               : -1;
        auto source = MediaFrameSource::open(*path, stream);
        if (!source) {
            return std::unexpected(source.error());
        }
        return std::unique_ptr<FrameSource>(std::move(*source));
    };
    services->fingerprint = [pathOf](const MediaItem& media) -> std::expected<std::uint64_t, Error> {
        auto path = pathOf(media);
        if (!path) {
            return std::unexpected(path.error());
        }
        return media::sourceFingerprint(*path);
    };
    services->cacheRoot = cacheRoot;
    if (o.detector && vision::subjectModelsInstalled(store)) {
        vision::HelperDetectorOptions detector = vision::subjectDetectorOptions(store);
        detector.executable = o.helper;
        detector.allClasses = true;
        services->detector = std::make_shared<vision::HelperDetector>(std::move(detector));
    }
    services->remember = [](std::shared_ptr<const MatteTrack> track) {
        std::lock_guard lock(installMutex());
        if (auto source = std::dynamic_pointer_cast<CacheMatteSource>(render::matteSource())) {
            source->remember(std::move(track));
        }
    };
    return services;
}

std::shared_ptr<CacheMatteSource> installMatteSource(const std::filesystem::path& cacheRoot) {
    const std::filesystem::path root = cacheRoot.empty() ? AnalysisCache::defaultRoot() : cacheRoot;
    std::lock_guard lock(installMutex());
    if (auto current = std::dynamic_pointer_cast<CacheMatteSource>(render::matteSource()); current && current->root() == root) {
        return current;
    }
    auto source = std::make_shared<CacheMatteSource>(root);
    render::setMatteSource(source);
    return source;
}

} // namespace wabcut::ai::segment
