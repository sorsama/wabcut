#pragma once

// The cache-backed SourceFrameProcessor (playback/video/SourceStage.h): the evaluator hands it a layer's decoded picture and
// AI chain; it returns the processed picture the Runner stored for that frame (the planes the compositor reads, kept alive
// by the cache's LRU, so a repeated frame is not uploaded twice), mixed with the decoded one by the chain's Amount; or
// says the picture is missing. In playback it never runs a model and never waits: it stays real time whatever the chain
// costs. In an export (playback::ScopedSourceStageMode ProcessMissing) a missing picture is made on the spot with the
// backend - the decoded picture of an export is the original at full size - so an export is never short of one; when it
// cannot, it reports why (no model on this computer, no helper) and the export stops with that reason.

#include <wabcut/ai/video/Backend.h>
#include <wabcut/ai/video/ProcessedCache.h>
#include <wabcut/ai/video/VideoCommands.h>
#include <wabcut/playback/video/SourceStage.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

namespace wabcut::ai::video {

class CacheSourceProcessor final : public playback::SourceFrameProcessor {
public:
    explicit CacheSourceProcessor(std::shared_ptr<ProcessedCache> cache);
    ~CacheSourceProcessor() override;

    playback::SourceStageResult process(const playback::SourceStageQuery& query) override;
    /// AI Refocus's depth map (the stored 8-bit plane) and AI Frame Interpolation's picture: from the cache; in an export the
    /// missing one is made on the spot with the backend, as above.
    playback::DepthResult depth(const playback::DepthQuery& query) override;
    playback::InterpolationResult interpolate(const playback::InterpolationQuery& query) override;
    /// AI Face Retouch's face map (the stored skin, mouth and eyes planes): same rules as depth().
    playback::FaceResult faces(const playback::FaceQuery& query) override;
    /// AI Eye Contact's eye measurements (the stored table of faces, eyes and irises): same rules as depth().
    playback::GazeResult gaze(const playback::GazeQuery& query) override;

    std::shared_ptr<ProcessedCache> cache() const { return m_cache; }
    /// What makes a missing picture in an export (the helper's backend; tests use a fake). Null: exports cannot make them.
    void setBackend(std::shared_ptr<IVideoBackend> backend);
    std::shared_ptr<IVideoBackend> backend() const;
    /// The model store the exports look in (default ModelStore::defaultRoot() at the time).
    void setModelRoot(std::filesystem::path root);
    /// Pictures served and pictures missing since the start (tests, the bridge's summary).
    std::uint64_t hits() const { return m_hits.load(); }
    std::uint64_t misses() const { return m_misses.load(); }

private:
    std::shared_ptr<const StoredFrame> makeNow(const playback::SourceStageQuery& query, const CacheIdentity& id);
    std::shared_ptr<const StoredFrame> makeDepthNow(const playback::DepthQuery& query, const CacheIdentity& id);
    std::shared_ptr<const StoredFrame> makeFacesNow(const playback::FaceQuery& query, const CacheIdentity& id);
    std::shared_ptr<const StoredFrame> makeGazeNow(const playback::GazeQuery& query, const CacheIdentity& id);
    std::shared_ptr<const StoredFrame> makeInterpolationNow(const playback::InterpolationQuery& query, const CacheIdentity& id, std::int64_t key);
    /// The backend and model store for an on-the-spot picture of `chain`, or null after reporting why there is none.
    std::shared_ptr<IVideoBackend> backendFor(const AiChain& chain, const char* what);
    /// Playback reads the next few pictures in its direction ahead of time (inflating a 1080p picture takes tens of
    /// milliseconds: more than a frame of budget when done on the scheduler's thread at the last moment).
    void readAhead(const CacheIdentity& id, Ticks pts, Ticks duration);
    void readerMain(std::stop_token stop);
    struct Ahead {
        CacheIdentity id;
        Ticks pts;
        Ticks tolerance;
    };
    std::mutex m_aheadMutex;
    std::condition_variable_any m_aheadWake;
    std::deque<Ahead> m_ahead;
    std::atomic<std::int64_t> m_lastPts{0};
    std::jthread m_reader; // started by the first hit, joined by the destructor

    std::shared_ptr<ProcessedCache> m_cache;
    mutable std::mutex m_backendMutex;
    std::shared_ptr<IVideoBackend> m_backend;
    std::filesystem::path m_modelRoot;
    std::atomic<std::uint64_t> m_hits{0};
    std::atomic<std::uint64_t> m_misses{0};
    /// The last mixed picture (Amount below 100 %): a paused or scrubbed frame is mixed once.
    std::mutex m_mixMutex;
    struct Mixed {
        const StoredFrame* source = nullptr;
        std::string directory;
        std::int64_t decodedPts = 0;
        int amount = -1;
        std::shared_ptr<const StoredFrame> frame;
    } m_mixed;
};

/// Makes a CacheSourceProcessor over `cache` and installs it for the process (playback and export).
std::shared_ptr<CacheSourceProcessor> installSourceProcessor(std::shared_ptr<ProcessedCache> cache);

/// The process-wide AI video services, made on the first call: the cache (ProcessedCache::defaultRoot()), the processor
/// installed for playback and export, and the helper backend (wabcut-embed next to the executable; nothing starts until the
/// first picture is made). The app and wabcut-cli call it at start so projects with AI effects show their pictures at once.
struct Services {
    std::shared_ptr<ProcessedCache> cache;
    std::shared_ptr<CacheSourceProcessor> processor;
    std::shared_ptr<IVideoBackend> backend;
    /// The helper is part of this build and next to the executable.
    bool helperAvailable = false;
};
Services& services();

/// The services of effect.aiStatus / effect.processAi over the process-wide ones: the cache, the helper backend (null when the
/// build has no helper: the command is then refused) and the decoder-backed frame sources.
std::shared_ptr<VideoServices> makeVideoServices();

} // namespace wabcut::ai::video
