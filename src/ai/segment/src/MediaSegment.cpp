#include <wabcut/ai/segment/MediaSegment.h>

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/edit/MediaServices.h>
#include <wabcut/ai/search/MediaSearchAccess.h>
#include <wabcut/ai/vision/HelperDetector.h>
#include <wabcut/editor/ProjectDocument.h>
#include <wabcut/export/RenderedVideoSource.h>
#include <wabcut/media/Fingerprint.h>
#include <wabcut/media/VideoDecoder.h>
#include <wabcut/model/Resolve.h>
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

// ---- NestedFrameSource --------------------------------------------------------------------------------------------

namespace {
constexpr int kNestedLongSide = 1024; ///< the rendered picture's longer side (the model's input is 1024 square)
constexpr std::int64_t kNestedRunAhead = 900; ///< frames one forward renderer run may cover (30 s at 30 fps)

/// `frame` (RGBA8 or anything search::storedPicture reads) at `size` x `size`, stretched: a box average per texel.
search::Image squarePicture(const media::FrameHandle& frame, int size) {
    const media::CpuPlanes* cpu = frame.cpu();
    if (!cpu || frame.format != media::PixelFormat::RGBA8) {
        return search::storedPicture(frame, size, size);
    }
    search::Image out;
    out.width = size;
    out.height = size;
    out.rgb.assign(static_cast<std::size_t>(size) * size * 3, 0);
    const media::Plane& plane = cpu->planes[0];
    for (int y = 0; y < size; ++y) {
        const int y0 = y * frame.height / size, y1 = std::max(y0 + 1, (y + 1) * frame.height / size);
        for (int x = 0; x < size; ++x) {
            const int x0 = x * frame.width / size, x1 = std::max(x0 + 1, (x + 1) * frame.width / size);
            std::uint32_t sum[3] = {0, 0, 0};
            std::uint32_t n = 0;
            for (int yy = y0; yy < std::min(y1, frame.height); ++yy) {
                const std::uint8_t* row = plane.data + static_cast<std::ptrdiff_t>(yy) * plane.stride;
                for (int xx = x0; xx < std::min(x1, frame.width); ++xx) {
                    sum[0] += row[xx * 4];
                    sum[1] += row[xx * 4 + 1];
                    sum[2] += row[xx * 4 + 2];
                    ++n;
                }
            }
            std::uint8_t* px = &out.rgb[(static_cast<std::size_t>(y) * size + x) * 3];
            for (int c = 0; c < 3; ++c) {
                px[c] = static_cast<std::uint8_t>(n ? (sum[c] + n / 2) / n : 0);
            }
        }
    }
    return out;
}
} // namespace

struct NestedFrameSource::Impl {
    ProjectState shown;
    SequencePtr sequence;
    exporter::VideoSourceFactory factory;
    int width = 0;
    int height = 0;
    Rate rate{30, 1};
    Ticks frameDuration{};
    TimeRange frames;
    double aspect = 16.0 / 9.0;
    // The renderer run going forward: frames [from, from + total * fd), `taken` delivered so far.
    std::unique_ptr<exporter::IVideoFrameSource> running;
    Ticks from{};
    std::int64_t total = 0;
    std::int64_t requested = 0;
    std::int64_t taken = 0;
    int runs = 0;
    struct Picture {
        Ticks time{};
        int size = 0;
        search::Image image;
    };
    std::deque<Picture> kept; ///< most recent first

    ~Impl() { stop(); }
    void stop() {
        if (running) {
            running->stop();
            running.reset();
        }
    }
    const Picture* find(Ticks t, int size) const {
        const auto it = std::ranges::find_if(kept, [&](const Picture& p) { return p.size == size && p.time == t; });
        return it == kept.end() ? nullptr : &*it;
    }
    Ticks onGrid(Ticks t) const { return frames.start + Ticks{(t - frames.start).value / frameDuration.value * frameDuration.value}; }
    Ticks nextTime() const { return from + Ticks{frameDuration.value * taken}; }

    std::expected<void, Error> start(Ticks at, std::int64_t count) {
        stop();
        exporter::VideoSourceFormat format;
        format.width = width;
        format.height = height;
        format.frameRate = rate;
        format.start = at;
        format.frameCount = std::max<std::int64_t>(1, count);
        auto source = factory(shown, *sequence, format);
        if (!source) {
            return std::unexpected(source.error());
        }
        running = std::move(*source);
        from = at;
        total = format.frameCount;
        requested = 0;
        taken = 0;
        ++runs;
        return {};
    }
    /// The running renderer's next frame, kept at `size`.
    std::expected<const Picture*, Error> take(int size) {
        while (requested < total && requested - taken < std::max(1, running->maxInFlight())) {
            if (auto r = running->request(requested); !r) {
                return std::unexpected(r.error());
            }
            ++requested;
        }
        auto frame = running->next();
        if (!frame) {
            return std::unexpected(frame.error());
        }
        Picture p;
        p.time = nextTime();
        p.size = size;
        p.image = squarePicture(*frame, size);
        ++taken;
        if (!p.image.valid()) {
            return std::unexpected(Error(ErrorCode::DecoderError, "a frame of the nested sequence could not be rendered"));
        }
        kept.push_front(std::move(p));
        while (kept.size() > 14) {
            kept.pop_back();
        }
        return &kept.front();
    }
};

NestedFrameSource::NestedFrameSource() : m(std::make_unique<Impl>()) {}
NestedFrameSource::~NestedFrameSource() = default;

std::expected<std::unique_ptr<NestedFrameSource>, Error> NestedFrameSource::open(const ProjectState& state, const Clip& clip,
                                                                               const std::filesystem::path& projectFile,
                                                                               exporter::VideoSourceFactory factory) {
    const Sequence* nested = clip.isNested() ? state.findSequence(clip.sequence) : nullptr;
    if (!nested || nested->width <= 0 || nested->height <= 0) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "the clip plays no nested or multicam sequence"));
    }
    std::unique_ptr<NestedFrameSource> source(new NestedFrameSource());
    Impl& m = *source->m;
    // The nest as the clip shows it: only the video tracks it reads (a multicam clip's active angle), no captions; a
    // multicam source has no master stage of its own.
    auto seq = std::make_shared<Sequence>(*nested);
    const std::vector<std::size_t> read = nestedTracksRead(*nested, clip);
    for (std::size_t i = 0; i < seq->videoTracks.size(); ++i) {
        if (std::ranges::find(read, i) == read.end()) {
            auto track = std::make_shared<Track>(*seq->videoTracks[i]);
            track->clips = ClipList{};
            track->transitions.clear();
            track->effects.clear();
            seq->videoTracks[i] = std::move(track);
        }
    }
    seq->captionTracks.clear();
    if (seq->multicam) {
        seq->effects.clear();
    }
    m.sequence = seq;
    m.shown = editor::withMediaChecked(state, projectFile).withSequence(seq);
    m.factory = factory ? std::move(factory) : exporter::renderedVideoSourceFactory([] {
        exporter::RenderedSourceOptions o;
        o.renderThreads = 1;
        o.maxInFlight = 2;
        return o;
    }());
    const double scale = std::min(1.0, static_cast<double>(kNestedLongSide) / std::max(nested->width, nested->height));
    m.width = std::max(16, static_cast<int>(std::lround(nested->width * scale)) & ~1);
    m.height = std::max(16, static_cast<int>(std::lround(nested->height * scale)) & ~1);
    m.rate = nested->frameRate.isValid() ? nested->frameRate : Rate{30, 1};
    m.frameDuration = m.rate.isExact() ? Ticks{m.rate.ticksPerUnit()} : framesToTicks(1, m.rate, Rounding::Floor).value_or(Ticks{kTicksPerSecond / 30});
    m.frames = TimeRange{Ticks{0}, nested->duration.value > 0 ? nested->duration : m.frameDuration};
    m.aspect = static_cast<double>(nested->width) / nested->height;
    return source;
}

std::expected<search::Image, Error> NestedFrameSource::pictureAt(Ticks t, int size) {
    const Ticks at = m->onGrid(std::clamp(t, m->frames.start, m->frames.end() - Ticks{1}));
    if (const Impl::Picture* had = m->find(at, size)) {
        return had->image;
    }
    const auto remaining = [&](Ticks from) { return std::max<std::int64_t>(1, (m->frames.end() - from).value / m->frameDuration.value); };
    if (m->running && at == m->nextTime() && m->taken < m->total) {
        auto got = m->take(size); // the run going forward
        if (!got) {
            return std::unexpected(got.error());
        }
        return (*got)->image;
    }
    if (m->running && at > m->nextTime() && m->taken < m->total && (at - m->nextTime()).value / m->frameDuration.value < 8) {
        while (m->nextTime() < at) { // a few frames ahead: skip forward in the run
            if (auto got = m->take(size); !got) {
                return std::unexpected(got.error());
            }
        }
        auto got = m->take(size);
        if (!got) {
            return std::unexpected(got.error());
        }
        return (*got)->image;
    }
    // Backward (or a jump): render a block that ends at `at` and keep it, so the next frames back are kept too. A jump
    // forward starts a run from `at`.
    const bool back = !m->kept.empty() && at < m->kept.front().time;
    const Ticks start = back ? std::max(m->frames.start, at - Ticks{m->frameDuration.value * 11}) : at;
    const std::int64_t count = back ? (at - start).value / m->frameDuration.value + 1 : std::min(kNestedRunAhead, remaining(at));
    if (auto started = m->start(start, count); !started) {
        return std::unexpected(started.error());
    }
    while (m->nextTime() <= at) {
        if (auto got = m->take(size); !got) {
            return std::unexpected(got.error());
        }
    }
    if (back) {
        m->stop();
    }
    if (const Impl::Picture* had = m->find(at, size)) {
        return had->image;
    }
    return std::unexpected(Error(ErrorCode::DecoderError, "a frame of the nested sequence could not be rendered"));
}

Ticks NestedFrameSource::frameDuration() const { return m->frameDuration; }
TimeRange NestedFrameSource::frames() const { return m->frames; }
double NestedFrameSource::aspect() const { return m->aspect; }
int NestedFrameSource::runs() const { return m->runs; }
const ProjectState& NestedFrameSource::shown() const { return m->shown; }

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
    services->nestedFrames = [projectFile, renderer = o.nestedRenderer](const ProjectState& state,
                                                                        const Clip& clip) -> std::expected<std::unique_ptr<FrameSource>, Error> {
        auto source = NestedFrameSource::open(state, clip, projectFile, renderer);
        if (!source) {
            return std::unexpected(source.error());
        }
        return std::unique_ptr<FrameSource>(std::move(*source));
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
