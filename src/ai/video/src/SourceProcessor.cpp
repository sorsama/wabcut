#include <wabcut/ai/video/SourceProcessor.h>

#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/search/EmbedHelper.h>
#include <wabcut/ai/video/FrameConvert.h>
#include <wabcut/ai/video/Gaze.h>
#include <wabcut/ai/video/HelperBackend.h>
#include <wabcut/ai/video/MediaFrames.h>
#include <wabcut/ai/video/MorphFaces.h>
#include <wabcut/ai/video/Runner.h>
#include <wabcut/ai/video/Targets.h>
#include <wabcut/ai/video/VideoModels.h>

#include <algorithm>
#include <cmath>

namespace wabcut::ai::video {

CacheSourceProcessor::CacheSourceProcessor(std::shared_ptr<ProcessedCache> cache) : m_cache(std::move(cache)) {}

CacheSourceProcessor::~CacheSourceProcessor() {
    if (m_reader.joinable()) {
        m_reader.request_stop();
        m_aheadWake.notify_all();
        m_reader.join();
    }
}

void CacheSourceProcessor::readAhead(const CacheIdentity& id, Ticks pts, Ticks duration) {
    if (duration.value <= 0) {
        return;
    }
    const std::int64_t previous = m_lastPts.exchange(pts.value);
    const std::int64_t direction = pts.value >= previous ? 1 : -1;
    const Ticks tolerance{std::max<std::int64_t>(1, duration.value / 2)};
    {
        std::lock_guard lock(m_aheadMutex);
        if (!m_reader.joinable()) {
            m_reader = std::jthread([this](std::stop_token stop) { readerMain(stop); });
        }
        m_ahead.clear(); // only the latest position matters
        for (int k = 1; k <= 3; ++k) {
            m_ahead.push_back({id, Ticks{pts.value + direction * k * duration.value}, tolerance});
        }
    }
    m_aheadWake.notify_one();
}

void CacheSourceProcessor::readerMain(std::stop_token stop) {
    while (!stop.stop_requested()) {
        Ahead next;
        {
            std::unique_lock lock(m_aheadMutex);
            m_aheadWake.wait(lock, stop, [&] { return !m_ahead.empty(); });
            if (stop.stop_requested()) {
                return;
            }
            next = std::move(m_ahead.front());
            m_ahead.pop_front();
        }
        (void)m_cache->get(next.id, next.pts, next.tolerance); // reads it into the cache's memory
    }
}

void CacheSourceProcessor::setBackend(std::shared_ptr<IVideoBackend> backend) {
    std::lock_guard lock(m_backendMutex);
    m_backend = std::move(backend);
}

std::shared_ptr<IVideoBackend> CacheSourceProcessor::backend() const {
    std::lock_guard lock(m_backendMutex);
    return m_backend;
}

void CacheSourceProcessor::setModelRoot(std::filesystem::path root) {
    std::lock_guard lock(m_backendMutex);
    m_modelRoot = std::move(root);
}

std::shared_ptr<const StoredFrame> CacheSourceProcessor::makeNow(const playback::SourceStageQuery& query, const CacheIdentity& id) {
    const AiChain& chain = *query.chain;
    std::filesystem::path root;
    {
        std::lock_guard lock(m_backendMutex);
        root = m_modelRoot;
    }
    const std::shared_ptr<IVideoBackend> backend = this->backend();
    if (!backend) {
        playback::reportSourceStageFailure("The AI effects (" + aiChainLabel(chain) + ") cannot be processed: this build has no AI helper. "
                                           "Turn the effects off, or export with a build that includes it.");
        return nullptr;
    }
    const ModelStore store(root.empty() ? ModelStore::defaultRoot() : root);
    if (!modelsInstalled(chain, store)) {
        playback::reportSourceStageFailure("The AI effects (" + aiChainLabel(chain) + ") need models that are not downloaded: " +
                                           missingModelsText(chain, store) +
                                           ". Download them from the effect in Effect Controls, or turn the effect off.");
        return nullptr;
    }
    auto picture = pictureOf(*query.decoded);
    if (!picture) {
        playback::reportSourceStageFailure("The AI effects (" + aiChainLabel(chain) + ") cannot read the picture: " + picture.error().message);
        return nullptr;
    }
    auto made = processPicture(*picture, chain, *backend, m_cache.get(), playback::currentSourceStageStop());
    if (!made) {
        if (made.error().code != ErrorCode::Cancelled) {
            playback::reportSourceStageFailure("The AI effects (" + aiChainLabel(chain) + ") failed: " + made.error().message);
        }
        return nullptr;
    }
    auto stored = storedFrameOf(*made, *query.decoded);
    if (!stored) {
        playback::reportSourceStageFailure("The AI effects (" + aiChainLabel(chain) + ") cannot store the picture: " + stored.error().message);
        return nullptr;
    }
    stored->pts = query.decoded->pts.value;
    stored->duration = query.decoded->duration.value;
    // A failed write (a full disk) does not stop the export: the picture is used from memory this time.
    (void)m_cache->put(id, *stored);
    return std::make_shared<const StoredFrame>(std::move(*stored));
}

std::shared_ptr<IVideoBackend> CacheSourceProcessor::backendFor(const AiChain& chain, const char* what) {
    std::filesystem::path root;
    {
        std::lock_guard lock(m_backendMutex);
        root = m_modelRoot;
    }
    const std::shared_ptr<IVideoBackend> backend = this->backend();
    if (!backend) {
        playback::reportSourceStageFailure(std::string(what) + " cannot be made: this build has no AI helper. Turn the effect off, or export "
                                           "with a build that includes it.");
        return nullptr;
    }
    const ModelStore store(root.empty() ? ModelStore::defaultRoot() : root);
    if (!modelsInstalled(chain, store)) {
        playback::reportSourceStageFailure(std::string(what) + " needs a model that is not downloaded: " + missingModelsText(chain, store) +
                                           ". Download it from the effect in Effect Controls, or turn the effect off.");
        return nullptr;
    }
    return backend;
}

std::shared_ptr<const StoredFrame> CacheSourceProcessor::makeDepthNow(const playback::DepthQuery& query, const CacheIdentity& id) {
    const AiChain chain = aiDepthChain();
    const auto backend = backendFor(chain, "AI Refocus's depth map");
    if (!backend) {
        return nullptr;
    }
    auto picture = pictureOf(*query.decoded);
    if (!picture) {
        playback::reportSourceStageFailure("AI Refocus cannot read the picture: " + picture.error().message);
        return nullptr;
    }
    auto made = processPicture(*picture, chain, *backend, m_cache.get(), playback::currentSourceStageStop());
    if (!made) {
        if (made.error().code != ErrorCode::Cancelled) {
            playback::reportSourceStageFailure("AI Refocus failed: " + made.error().message);
        }
        return nullptr;
    }
    StoredFrame stored;
    stored.width = made->width;
    stored.height = made->height;
    stored.pts = query.decoded->pts.value;
    stored.duration = query.decoded->duration.value;
    StoredPlane plane;
    plane.width = made->width;
    plane.height = made->height;
    plane.bytesPerTexel = 1;
    plane.data = distanceMap8(*made);
    stored.planes.push_back(std::move(plane));
    (void)m_cache->put(id, stored); // a full disk does not stop the export: the map is used from memory this time
    return std::make_shared<const StoredFrame>(std::move(stored));
}

std::shared_ptr<const StoredFrame> CacheSourceProcessor::makeFacesNow(const playback::FaceQuery& query, const CacheIdentity& id) {
    const AiChain chain = aiFaceChain();
    const auto backend = backendFor(chain, "AI Face Retouch's face map");
    if (!backend) {
        return nullptr;
    }
    auto picture = pictureOf(*query.decoded);
    if (!picture) {
        playback::reportSourceStageFailure("AI Face Retouch cannot read the picture: " + picture.error().message);
        return nullptr;
    }
    auto made = processPicture(*picture, chain, *backend, m_cache.get(), playback::currentSourceStageStop());
    if (!made) {
        if (made.error().code != ErrorCode::Cancelled) {
            playback::reportSourceStageFailure("AI Face Retouch failed: " + made.error().message);
        }
        return nullptr;
    }
    auto stored = faceStoredFrame(*made, query.decoded->pts, query.decoded->duration);
    if (!stored) {
        playback::reportSourceStageFailure("AI Face Retouch cannot store the face map: " + stored.error().message);
        return nullptr;
    }
    (void)m_cache->put(id, *stored); // a full disk does not stop the export: the map is used from memory this time
    return std::make_shared<const StoredFrame>(std::move(*stored));
}

std::shared_ptr<const StoredFrame> CacheSourceProcessor::makeGazeNow(const playback::GazeQuery& query, const CacheIdentity& id) {
    const AiChain chain = aiGazeChain();
    const auto backend = backendFor(chain, "AI Eye Contact's eye measurements");
    if (!backend) {
        return nullptr;
    }
    auto picture = pictureOf(*query.decoded);
    if (!picture) {
        playback::reportSourceStageFailure("AI Eye Contact cannot read the picture: " + picture.error().message);
        return nullptr;
    }
    auto made = processPicture(*picture, chain, *backend, m_cache.get(), playback::currentSourceStageStop());
    if (!made) {
        if (made.error().code != ErrorCode::Cancelled) {
            playback::reportSourceStageFailure("AI Eye Contact failed: " + made.error().message);
        }
        return nullptr;
    }
    auto stored = gazeStoredFrame(*made, query.decoded->pts, query.decoded->duration);
    if (!stored) {
        playback::reportSourceStageFailure("AI Eye Contact cannot store the eye measurements: " + stored.error().message);
        return nullptr;
    }
    (void)m_cache->put(id, *stored); // a full disk does not stop the export: the measurements are used from memory this time
    return std::make_shared<const StoredFrame>(std::move(*stored));
}

std::shared_ptr<const StoredFrame> CacheSourceProcessor::makeInterpolationNow(const playback::InterpolationQuery& query, const CacheIdentity& id,
                                                                            std::int64_t key) {
    const AiChain chain = aiInterpolationChain();
    const auto backend = backendFor(chain, "AI Frame Interpolation's picture");
    if (!backend) {
        return nullptr;
    }
    auto a = pictureOf(*query.first);
    auto b = pictureOf(*query.second);
    if (!a || !b) {
        playback::reportSourceStageFailure("AI Frame Interpolation cannot read the frames: " + (!a ? a.error() : b.error()).message);
        return nullptr;
    }
    auto made = interpolatePicture(*a, *b, query.weight, *backend, m_cache.get(), playback::currentSourceStageStop());
    if (!made) {
        if (made.error().code != ErrorCode::Cancelled) {
            playback::reportSourceStageFailure("AI Frame Interpolation failed: " + made.error().message);
        }
        return nullptr;
    }
    auto stored = storedFrameOf(*made, *query.first);
    if (!stored) {
        playback::reportSourceStageFailure("AI Frame Interpolation cannot store the picture: " + stored.error().message);
        return nullptr;
    }
    stored->pts = key;
    (void)m_cache->put(id, *stored);
    return std::make_shared<const StoredFrame>(std::move(*stored));
}

playback::DepthResult CacheSourceProcessor::depth(const playback::DepthQuery& query) {
    playback::DepthResult result;
    if (!m_cache || query.item == nullptr) {
        result.missing = true;
        return result;
    }
    const CacheIdentity id = identityOf(*query.item, query.streamIndex, aiDepthChain());
    const bool haveDecoded = query.decoded != nullptr && query.decoded->isValid();
    const Ticks pts = haveDecoded ? query.decoded->pts : query.sourceTime;
    const std::int64_t duration = haveDecoded ? query.decoded->duration.value : 0;
    const Ticks tolerance{std::max<std::int64_t>(1, duration / 2)};
    std::shared_ptr<const StoredFrame> stored = id.valid() ? m_cache->get(id, pts, tolerance) : nullptr;
    if (!stored && id.valid() && haveDecoded && playback::currentSourceStageMode() == playback::SourceStageMode::ProcessMissing) {
        stored = makeDepthNow(query, id);
    }
    if (!stored || stored->planes.size() != 1 || stored->planes.front().bytesPerTexel != 1) {
        ++m_misses;
        result.missing = true;
        return result;
    }
    ++m_hits;
    if (haveDecoded) {
        readAhead(id, query.decoded->pts, query.decoded->duration);
    }
    auto map = std::make_shared<playback::DepthMap>();
    map->width = stored->planes.front().width;
    map->height = stored->planes.front().height;
    map->distance = stored->planes.front().data;
    result.map = std::move(map);
    return result;
}

playback::FaceResult CacheSourceProcessor::faces(const playback::FaceQuery& query) {
    playback::FaceResult result;
    if (!m_cache || query.item == nullptr) {
        result.missing = true;
        return result;
    }
    const CacheIdentity id = identityOf(*query.item, query.streamIndex, aiFaceChain());
    const bool haveDecoded = query.decoded != nullptr && query.decoded->isValid();
    const Ticks pts = haveDecoded ? query.decoded->pts : query.sourceTime;
    const std::int64_t duration = haveDecoded ? query.decoded->duration.value : 0;
    const Ticks tolerance{std::max<std::int64_t>(1, duration / 2)};
    std::shared_ptr<const StoredFrame> stored = id.valid() ? m_cache->get(id, pts, tolerance) : nullptr;
    if (!stored && id.valid() && haveDecoded && playback::currentSourceStageMode() == playback::SourceStageMode::ProcessMissing) {
        stored = makeFacesNow(query, id);
    }
    const auto usable = [](const StoredFrame& f) {
        return f.planes.size() == 3 && std::ranges::all_of(f.planes, [&](const StoredPlane& p) {
                   return p.bytesPerTexel == 1 && p.width == f.planes.front().width && p.height == f.planes.front().height;
               });
    };
    if (!stored || !usable(*stored)) {
        ++m_misses;
        result.missing = true;
        return result;
    }
    ++m_hits;
    if (haveDecoded) {
        readAhead(id, query.decoded->pts, query.decoded->duration);
    }
    auto map = std::make_shared<playback::FaceMap>();
    map->width = stored->planes.front().width;
    map->height = stored->planes.front().height;
    const std::size_t pixels = static_cast<std::size_t>(map->width) * static_cast<std::size_t>(map->height);
    map->coverage.resize(pixels * 3);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const std::vector<std::uint8_t>& plane = stored->planes[channel].data;
        for (std::size_t i = 0; i < pixels; ++i) {
            map->coverage[3 * i + channel] = plane[i];
        }
    }
    result.map = std::move(map);
    return result;
}

playback::GazeResult CacheSourceProcessor::gaze(const playback::GazeQuery& query) {
    playback::GazeResult result;
    if (!m_cache || query.item == nullptr) {
        result.missing = true;
        return result;
    }
    const CacheIdentity id = identityOf(*query.item, query.streamIndex, aiGazeChain());
    const bool haveDecoded = query.decoded != nullptr && query.decoded->isValid();
    const Ticks pts = haveDecoded ? query.decoded->pts : query.sourceTime;
    const std::int64_t duration = haveDecoded ? query.decoded->duration.value : 0;
    const Ticks tolerance{std::max<std::int64_t>(1, duration / 2)};
    std::shared_ptr<const StoredFrame> stored = id.valid() ? m_cache->get(id, pts, tolerance) : nullptr;
    if (!stored && id.valid() && haveDecoded && playback::currentSourceStageMode() == playback::SourceStageMode::ProcessMissing) {
        stored = makeGazeNow(query, id);
    }
    if (!stored || stored->planes.size() != 1 || stored->planes.front().bytesPerTexel != 4) {
        ++m_misses;
        result.missing = true;
        return result;
    }
    ++m_hits;
    if (haveDecoded) {
        readAhead(id, query.decoded->pts, query.decoded->duration);
    }
    auto map = std::make_shared<playback::GazeMap>();
    map->faces = gazeFacesOfStored(*stored); // a frame without faces: a map with none (the picture stays as it is)
    result.map = std::move(map);
    return result;
}

playback::InterpolationResult CacheSourceProcessor::interpolate(const playback::InterpolationQuery& query) {
    playback::InterpolationResult result;
    if (!m_cache || query.item == nullptr || query.first == nullptr || query.second == nullptr || query.streamIndex < 0 ||
        query.streamIndex >= static_cast<int>(query.item->info.video.size())) {
        result.missing = true;
        return result;
    }
    const VideoStreamInfo& stream = query.item->info.video[static_cast<std::size_t>(query.streamIndex)];
    const Ticks period = stream.frameRate.isValid() ? framesToTicks(1, stream.frameRate) : framesToTicks(1, kRate30);
    const CacheIdentity id = identityOf(*query.item, query.streamIndex, aiInterpolationChain());
    const std::int64_t key = interpolationKey(query.sourceTime, period);
    std::shared_ptr<const StoredFrame> stored = id.valid() ? m_cache->get(id, Ticks{key}, Ticks{0}) : nullptr;
    if (stored && stored->width <= 1) {
        stored = nullptr; // the stub of a time that fell on a frame: nothing to show here
    }
    if (!stored && id.valid() && query.first->isValid() && query.second->isValid() &&
        playback::currentSourceStageMode() == playback::SourceStageMode::ProcessMissing) {
        stored = makeInterpolationNow(query, id, key);
    }
    if (!stored) {
        ++m_misses;
        result.missing = true;
        return result;
    }
    ++m_hits;
    result.frame = frameOf(std::move(stored), query.first);
    return result;
}

playback::SourceStageResult CacheSourceProcessor::process(const playback::SourceStageQuery& query) {
    playback::SourceStageResult result;
    if (!m_cache || query.item == nullptr || query.chain == nullptr || query.chain->empty()) {
        result.missing = query.chain != nullptr && !query.chain->empty();
        return result;
    }
    const CacheIdentity id = identityOf(*query.item, query.streamIndex, *query.chain);
    const bool haveDecoded = query.decoded != nullptr && query.decoded->isValid();
    const Ticks pts = haveDecoded ? query.decoded->pts : query.sourceTime;
    const std::int64_t duration = haveDecoded ? query.decoded->duration.value : 0;
    const Ticks tolerance{std::max<std::int64_t>(1, duration / 2)};
    std::shared_ptr<const StoredFrame> stored = id.valid() ? m_cache->get(id, pts, tolerance) : nullptr;
    if (!stored && id.valid() && haveDecoded && playback::currentSourceStageMode() == playback::SourceStageMode::ProcessMissing) {
        stored = makeNow(query, id);
    }
    if (!stored) {
        ++m_misses;
        result.missing = true;
        return result;
    }
    ++m_hits;
    if (haveDecoded) {
        readAhead(id, query.decoded->pts, query.decoded->duration);
    }
    const double amount = std::clamp(query.chain->amount, 0.0, 1.0);
    if (!haveDecoded || amount >= 0.999) {
        result.frame = frameOf(stored, haveDecoded ? query.decoded : nullptr);
        return result;
    }
    // Amount below 100 %: the processed picture mixed with the decoded one (at the processed size), quantised to 1/64 so a
    // keyframed Amount does not mix a new picture for every tiny step.
    const int quantised = static_cast<int>(std::lround(amount * 64.0));
    {
        std::lock_guard lock(m_mixMutex);
        if (m_mixed.frame && m_mixed.source == stored.get() && m_mixed.amount == quantised && m_mixed.decodedPts == query.decoded->pts.value &&
            m_mixed.directory == id.directoryName()) {
            result.frame = frameOf(m_mixed.frame, query.decoded);
            return result;
        }
    }
    auto processed = pictureOf(*stored);
    auto original = pictureOf(*query.decoded);
    if (!processed || !original) {
        result.frame = frameOf(stored, query.decoded); // cannot mix: the processed picture as it is
        return result;
    }
    const Picture base = resized(*original, processed->width, processed->height);
    const Picture blended = mixed(base, *processed, static_cast<double>(quantised) / 64.0);
    auto mixedStored = storedFrameOf(blended, *query.decoded);
    if (!mixedStored) {
        result.frame = frameOf(stored, query.decoded);
        return result;
    }
    mixedStored->pts = stored->pts;
    mixedStored->duration = stored->duration;
    auto shared = std::make_shared<const StoredFrame>(std::move(*mixedStored));
    {
        std::lock_guard lock(m_mixMutex);
        m_mixed = Mixed{stored.get(), id.directoryName(), query.decoded->pts.value, quantised, shared};
    }
    result.frame = frameOf(std::move(shared), query.decoded);
    return result;
}

std::shared_ptr<CacheSourceProcessor> installSourceProcessor(std::shared_ptr<ProcessedCache> cache) {
    auto processor = std::make_shared<CacheSourceProcessor>(std::move(cache));
    playback::setSourceFrameProcessor(processor);
    return processor;
}

Services& services() {
    static Services instance = [] {
        Services s;
        s.cache = std::make_shared<ProcessedCache>(ProcessedCache::defaultRoot());
        s.processor = installSourceProcessor(s.cache);
        const std::filesystem::path helper = search::defaultEmbedExecutable();
        std::error_code ec;
        s.helperAvailable = !helper.empty() && std::filesystem::exists(helper, ec);
        if (s.helperAvailable) {
            s.backend = std::make_shared<HelperBackend>(HelperBackendOptions{});
            s.processor->setBackend(s.backend);
            playback::setFaceFinder(makeFaceFinder()); // 0.7: Morph Cut's Face Aware (finds nothing until YuNet is on this computer)
        }
        return s;
    }();
    return instance;
}

std::shared_ptr<VideoServices> makeVideoServices() {
    Services& s = services();
    return std::make_shared<VideoServices>(s.cache, s.backend, [](const std::string& path, int containerStream) -> std::unique_ptr<IFrameSource> {
        return std::make_unique<DecoderFrameSource>(path, containerStream);
    });
}

} // namespace wabcut::ai::video
