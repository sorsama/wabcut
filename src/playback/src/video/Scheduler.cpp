#include "DecodeSession.h"
#include "PoolFrameSource.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/playback/video/CompositePool.h>
#include <wabcut/playback/video/Evaluate.h>
#include <wabcut/playback/video/Scheduler.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <map>
#include <unordered_map>

namespace wabcut::playback {

namespace {

constexpr std::int64_t kStatsIntervalNs = 250'000'000;
/// Pool epoch namespaces: one per scheduler.
std::atomic<std::uint32_t> g_nextPoolClient{1};
/// Upper bound for the re-aim lead after late targets (steps of the shuttle stride).
constexpr std::int64_t kMaxLeadSteps = 64;
/// C.16: the decode keys of preview-file layers (no clip id reaches this bit).
constexpr std::uint64_t kPreviewDecodeKey = std::uint64_t{1} << 62;

bool isPreviewLayer(const LayerSpec& layer) { return !layer.clip.isValid() && (layer.decodeKey & kPreviewDecodeKey) != 0 && layer.item; }

/// Steps a decoding target may fall behind before it is abandoned (about half a second at 60 Hz).
constexpr std::int64_t kMaxWaitSteps = 30;
/// P.2: Impl::pendingMedia while a picture waits for motion analysis rather than a decoder.
constexpr std::uint64_t kMotionPending = ~std::uint64_t{0};

std::int64_t ceilDiv(std::int64_t a, std::int64_t b) {
    return (a + b - 1) / b;
}

/// Whether a sequence `sequence` plays (nested or multicam, at any depth) changed between two snapshots. Unchanged
/// sequences are shared between snapshots, so a pointer compare per sequence is enough.
bool nestedSequenceChanged(const ProjectState& before, const ProjectState& after, const Sequence& sequence) {
    std::vector<SequenceId> pending = nestedSequencesOf(sequence);
    std::vector<SequenceId> seen;
    while (!pending.empty() && seen.size() < 4096) {
        const SequenceId id = pending.back();
        pending.pop_back();
        if (std::ranges::find(seen, id) != seen.end()) {
            continue;
        }
        seen.push_back(id);
        const Sequence* now = after.findSequence(id);
        if (now != before.findSequence(id)) {
            return true;
        }
        if (now) {
            for (const SequenceId inner : nestedSequencesOf(*now)) {
                pending.push_back(inner);
            }
        }
    }
    return false;
}

/// P.2: what a background motion analysis wakes when it finishes; cleared when the scheduler thread ends, since the
/// analyzer outlives every scheduler.
struct MotionWake {
    std::mutex mutex;
    std::function<void()> wake;

    void operator()() {
        std::lock_guard lock(mutex);
        if (wake) {
            wake();
        }
    }
};

/// P.2: the motion the analyzer has already (the scheduler waits for the rest before it publishes).
class FoundMotion final : public MotionSource {
public:
    std::shared_ptr<const motion::FlowPair> motion(const MotionRequest& request) override {
        return MotionAnalyzer::shared().find(request);
    }
};

/// Whether the composite of `layers` may need motion (an interpolation group or a flow mix).
bool needsMotion(const std::vector<LayerSpec>& layers) {
    return std::any_of(layers.begin(), layers.end(), [](const LayerSpec& l) {
        return l.interpolation || (l.transition && l.transition->kind == render::TransitionKind::FlowMorph);
    });
}

} // namespace

struct Scheduler::Impl {
    explicit Impl(std::shared_ptr<render::CompositeMailbox> box, std::shared_ptr<TransportClock> transport,
                  SchedulerOptions opts)
        : options(sanitize(opts, box ? box->capacity() : 8)), mailbox(std::move(box)),
          pool(options.compositePoolSize, 16), clock(std::move(transport)) {
        if (options.decodeBackend == DecodeBackend::Pool) {
            decoderPool = options.pool;
            if (!decoderPool) {
                media::DecoderPoolConfig config;
                config.maxVideoSessions = options.maxSessions;
                config.maxAudioSessions = 1;
                const int cpus = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
                config.workerThreads =
                    options.poolWorkerThreads > 0 ? options.poolWorkerThreads : std::clamp(cpus / 2, 4, 8);
                config.decoderOptions = options.decoder;
                decoderPool = std::make_shared<media::DecoderPool>(config);
            }
        }
        plan.resize(static_cast<std::size_t>(options.lookaheadFrames));
        for (auto& layers : plan) {
            layers.reserve(16);
        }
        targets.reserve(static_cast<std::size_t>(options.lookaheadFrames));
        frames.reserve(16);
        samples.reserve(options.collectTimings ? 1 << 16 : 0);
        previewSettings.useProxies = options.preferProxies;
        preview = previewSettings;
        motionWake->wake = [this] { wake(); };
    }

    /// Frames a source caches: the lookahead twice over, or a reverse window plus the lookahead.
    static int cacheFramesFor(const SchedulerOptions& o) {
        return std::max(2 * o.lookaheadFrames + 4, o.zeroCopy ? o.reverseWindowFrames + o.lookaheadFrames
                                                              : 2 * o.reverseWindowFrames + o.lookaheadFrames);
    }

    static SchedulerOptions sanitize(SchedulerOptions o, int mailboxCapacity) {
        o.lookaheadFrames = std::clamp(o.lookaheadFrames, 1, std::max(1, mailboxCapacity));
        o.maxSessions = std::max(o.maxSessions, 1);
        o.reverseWindowFrames = std::max(o.reverseWindowFrames, 2);
        if (o.compositePoolSize <= 0) {
            o.compositePoolSize = mailboxCapacity + o.lookaheadFrames + 8;
        }
        if (o.zeroCopy) {
            // Native frames pin decoder surfaces: hold few, and size the decoders' pools for them.
            o.reverseWindowFrames = std::min(o.reverseWindowFrames, 12);
            o.decoder.outputMode = media::OutputMode::Native;
            o.decoder.nativeSurfaces = cacheFramesFor(o) + mailboxCapacity + 4;
        }
        return o;
    }

    void run();
    void pass();
    /// P.2: asks the analyzer for the motion `layers` will need, where their frames are already decoded (lookahead).
    void prefetchMotion(const std::vector<LayerSpec>& layers, const Sequence& seq);
    enum class Publish { Done, NotReady, NoComposite };
    /// Publishes `target` if every layer's frame is decoded (failed layers are left out).
    Publish publishIfReady(Ticks target, const std::vector<LayerSpec>& layers, const Sequence& seq,
                           Ticks frame);
    detail::FrameSource* sessionFor(const LayerSpec& layer, std::int64_t now);
    /// evaluateVideoLayers, then (C.16) the preview file instead of the layers inside a valid rendered segment.
    void evaluate(const ProjectState& snapshot, const Sequence& seq, Ticks t, std::vector<LayerSpec>& layers);
    /// Applies what changed between two snapshots to the held still, the sessions and the lookahead. Returns
    /// true when composites already published ahead of the playhead are affected (the caller starts a new
    /// epoch).
    bool invalidate(const ProjectState& before, const ProjectState& after, SequenceId sequenceId, Ticks frame,
                    Ticks span);
    void reapSessions(std::int64_t now);
    void refreshStats(std::int64_t now);
    /// Counts `late` missed targets against the media the scheduler waited for, else every decoded layer of the
    /// target (the automatic proxy policy reads lateFramesByMedia).
    void blameLate(std::uint64_t media, const std::vector<LayerSpec>& layers, std::uint64_t late) {
        if (late == 0 || media == kMotionPending) {
            return; // P.2: waiting for motion analysis is no decoder's fault
        }
        if (media != 0) {
            lateByMedia[media] += late;
            return;
        }
        for (const LayerSpec& l : layers) {
            if (needsDecode(l)) {
                lateByMedia[l.media.value] += late;
            }
        }
    }

    void wake() {
        {
            std::lock_guard lock(mutex);
            wakeFlag = true;
        }
        cv.notify_one();
    }

    const SchedulerOptions options;
    std::shared_ptr<render::CompositeMailbox> mailbox;
    CompositePool pool;

    // Control, guarded by `mutex`.
    std::mutex mutex;
    std::condition_variable cv;
    bool stop = false;
    bool wakeFlag = false;
    const EditSession* session = nullptr;
    ProjectSnapshot fixedSnapshot;
    SequenceId sequence;
    std::shared_ptr<TransportClock> clock;
    std::uint64_t requestedEpoch = 0;
    std::function<void(int)> boundary;
    std::atomic<bool> stopAtEnd{true}; // A.8: off during a voice-over pass
    PreviewSettings previewSettings; // what the preview decodes (setPreview)
    std::uint64_t previewVersion = 0;
    std::optional<CaptionSettings> captionSettings; // burned-in captions (setCaptions)
    std::uint64_t captionVersion = 0;
    ColorPreview colorSettings; // the Color workspace's views (setColorPreview)
    std::uint64_t colorVersion = 0;
    std::optional<TimeRange> loopRange; // P.2, Premiere's Loop (setLoop)

    // Statistics.
    std::atomic<std::uint64_t> passes{0}, published{0}, skippedLate{0}, decodeRequests{0}, poolExhausted{0},
        invalidations{0};
    std::atomic<int> bypassedEffects{0};
    std::atomic<int> proxyLayers{0}, originalLayers{0}, offlineProxyLayers{0}, decodedDivisor{1};
    std::atomic<std::uint64_t> deviceChanges{0}, staleDeviceFrames{0};
    mutable std::mutex statsMutex;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> lateByMediaStats;
    std::string backends;
    int activeSessions = 0;
    std::uint64_t decodedFrames = 0;
    std::uint64_t decodeErrors = 0;
    std::uint64_t poolFramesDecoded = 0;
    std::uint64_t poolFramesDelivered = 0;
    std::vector<std::int64_t> samples;
    std::int64_t lastStatsNs = 0;

    // Scheduler thread only.
    // Declared before `sessions` so the sources die first (and the pool after every callback drained).
    std::shared_ptr<media::DecoderPool> decoderPool;
    std::shared_ptr<detail::PoolCallbackGate> poolGate = std::make_shared<detail::PoolCallbackGate>();
    const std::uint32_t poolClient = g_nextPoolClient.fetch_add(1);
    std::unordered_map<std::uint64_t, std::unique_ptr<detail::FrameSource>> sessions;
    std::uint64_t epoch = ~std::uint64_t{0};
    bool haveNext = false;
    Ticks nextTarget{};
    // P.2 Loop: the playhead frame of the previous playing pass of this epoch, to see the transport wrap.
    bool haveLastPresent = false;
    Ticks lastPresentFrame{};
    std::int64_t leadSteps = 1; // how far ahead of the playhead to re-aim after a late target
    bool boundaryDone = false;
    struct {
        Ticks target{};
        std::uint64_t epoch = 0;
        bool valid = false;
        std::vector<std::uint64_t> clips; ///< the sessions (sessionKey) it decoded: they stay while it is shown
    } lastStill;
    ProjectSnapshot lastSnapshot;
    std::uint64_t revision = 0;
    std::vector<TimeRange> dirty; // scratch: changed ranges of video tracks
    std::vector<std::vector<LayerSpec>> plan;
    std::vector<Ticks> targets;
    std::vector<media::VideoFrame> frames;
    PreviewSettings preview;           // copy of previewSettings for this pass
    std::uint64_t appliedPreviewVersion = 0;
    std::optional<CaptionSettings> captions; // copy of captionSettings for this pass
    std::uint64_t appliedCaptionVersion = 0;
    ColorPreview colorPreview; // copy of colorSettings for this pass
    std::uint64_t appliedColorVersion = 0;
    std::unordered_map<std::uint64_t, std::uint64_t> lateByMedia; // media id -> late targets
    std::uint64_t pendingMedia = 0; // the media the last NotReady publish waited for (0 = none)
    // C.16 preview files: the media items standing for them (LayerSpec::item points here; kept while the cache has
    // the file) and, per snapshot revision and preview settings, which entries are still valid (hashing a segment is
    // not free).
    std::unordered_map<std::string, std::unique_ptr<MediaItem>> previewItems;
    std::pair<std::uint64_t, std::uint64_t> previewChecked{~std::uint64_t{0}, ~std::uint64_t{0}};
    struct PreviewFileState {
        bool valid = false;
        std::string key; ///< file name, hash and write time: what previewItems and the decoder are keyed by
    };
    std::unordered_map<std::string, PreviewFileState> previewValid; ///< by file name
    // Zero-copy: the renderer's shared device the sessions decode on (media::sharedD3D11Device()).
    media::DeviceToken sharedDevice{};
    bool sharedDeviceKnown = false;
    // P.2: flow mixes' motion (MotionAnalyzer::shared()): what wakes this scheduler when an analysis it asked for ends,
    // and how many the latest picture waits for (the monitor's "Analyzing in background").
    std::shared_ptr<MotionWake> motionWake = std::make_shared<MotionWake>();
    std::atomic<int> motionWaits{0};
    FoundMotion foundMotion;

    std::thread thread;
};

detail::FrameSource* Scheduler::Impl::sessionFor(const LayerSpec& layer, std::int64_t now) {
    const ResolvedSource resolved = resolveSource(*layer.item, preview);
    if (!resolved.path) {
        return nullptr; // offline without a proxy: the layer is left out
    }
    const std::string* path = resolved.path;
    // A proxy holds one video stream; the original's container index means nothing there.
    const int stream = resolved.proxy ? -1 : layer.containerStream;
    const int targetDivisor = std::max(1, preview.decodeDivisor);
    auto it = sessions.find(sessionKey(layer));
    if (it != sessions.end()) {
        if (it->second->path() == *path && it->second->stream() == stream &&
            it->second->targetDivisor == targetDivisor) {
            it->second->lastUsedNs = now;
            return it->second.get();
        }
        sessions.erase(it); // relinked, proxy toggled, another resolution or stream: a new decoder
    }
    if (static_cast<int>(sessions.size()) >= options.maxSessions) {
        // Close the least recently used session that this pass has not touched.
        auto victim = sessions.end();
        for (auto s = sessions.begin(); s != sessions.end(); ++s) {
            if (s->second->lastUsedNs < now &&
                (victim == sessions.end() || s->second->lastUsedNs < victim->second->lastUsedNs)) {
                victim = s;
            }
        }
        if (victim != sessions.end()) {
            sessions.erase(victim);
        }
    }
    const int cacheFrames = cacheFramesFor(options);
    const int streamHeight = layer.item->info.video[static_cast<std::size_t>(layer.streamIndex)].height;
    std::unique_ptr<detail::FrameSource> created;
    if (decoderPool) {
        detail::PoolFrameSourceOptions o;
        o.cacheFrames = cacheFrames;
        o.reverseWindowFrames = options.reverseWindowFrames;
        const Rate rate = layer.item->info.video[static_cast<std::size_t>(layer.streamIndex)].frameRate;
        o.frameDuration = framesToTicks(1, rate, Rounding::Floor).value_or(Ticks{kTicksPerSecond / 30});
        o.client = poolClient;
        o.collectTimings = options.collectTimings;
        o.streamHeight = streamHeight;
        o.targetDivisor = targetDivisor;
        created = std::make_unique<detail::PoolFrameSource>(decoderPool, poolGate, *path, stream, o,
                                                            [this] { wake(); });
    } else {
        detail::DecodeSessionOptions o;
        o.decoder = options.decoder;
        o.reverseWindowFrames = options.reverseWindowFrames;
        o.cacheFrames = cacheFrames;
        o.collectTimings = options.collectTimings;
        o.streamHeight = streamHeight;
        o.targetDivisor = targetDivisor;
        created = std::make_unique<detail::DecodeSession>(*path, stream, o, [this] { wake(); });
    }
    created->lastUsedNs = now;
    created->proxy = resolved.proxy;
    created->originalOffline = resolved.originalOffline;
    created->targetDivisor = targetDivisor;
    created->beginEpoch(epoch);
    auto* raw = created.get();
    sessions.emplace(sessionKey(layer), std::move(created));
    return raw;
}

void Scheduler::Impl::evaluate(const ProjectState& snapshot, const Sequence& seq, Ticks t, std::vector<LayerSpec>& layers) {
    evaluateVideoLayers(snapshot, seq, t, layers, options.cullOccluded, &preview, captions ? &*captions : nullptr);
    PreviewCache* cache = preview.previews.get();
    if (!cache) {
        return;
    }
    if (previewChecked != std::pair{revision, appliedPreviewVersion}) {
        previewChecked = {revision, appliedPreviewVersion};
        previewValid.clear();
        // Files the cache no longer lists: their decoders go (an open file cannot be deleted on Windows) and so do their
        // items. Only specs of this pass point at items, and none has been evaluated yet in it.
        const std::vector<PreviewEntry> listed = cache->entries();
        bool closed = false;
        for (auto it = previewItems.begin(); it != previewItems.end();) {
            const bool kept = std::any_of(listed.begin(), listed.end(), [&](const PreviewEntry& e) { return e.file == it->second->name; });
            if (kept) {
                ++it;
                continue;
            }
            sessions.erase(it->second->id.value);
            it = previewItems.erase(it);
            closed = true;
        }
        if (closed && decoderPool) {
            decoderPool->closeIdleSessions(); // the pool's decoder of the file too
        }
    }
    const std::optional<PreviewEntry> entry = cache->lookup(seq.id, t);
    if (!entry) {
        return;
    }
    auto known = previewValid.find(entry->file);
    if (known == previewValid.end()) {
        // Valid while its hash is the snapshot's and the file is there. Its key holds the hash and the file's write
        // time, so a file rendered again under the same name gets a new decoder rather than an old one's frames.
        std::error_code ec;
        const std::filesystem::path path = cache->folder() / entry->file;
        PreviewFileState state;
        state.valid = renderContentHash(snapshot, seq.id, entry->range) == entry->hash && std::filesystem::is_regular_file(path, ec);
        const auto written = std::filesystem::last_write_time(path, ec);
        // long long: libc++'s file clock counts in an __int128, which std::to_string does not take.
        const auto ticks = ec ? 0LL : static_cast<long long>(written.time_since_epoch().count());
        state.key = entry->file + "#" + entry->hash + "#" + std::to_string(ticks);
        known = previewValid.emplace(entry->file, std::move(state)).first;
    }
    if (!known->second.valid) {
        return; // an edit the cache has not been told about yet, or a deleted file: composite live
    }
    std::unique_ptr<MediaItem>& item = previewItems[known->second.key];
    if (!item) {
        item = std::make_unique<MediaItem>();
        // A media id and decode key no clip or media id reaches (nested keys use bit 63): one decoder per rendered file.
        item->id = MediaId{kPreviewDecodeKey | (std::hash<std::string>{}(known->second.key) & (kPreviewDecodeKey - 1))};
        item->name = entry->file;
        item->locator.path = pathToUtf8(cache->folder() / entry->file);
        VideoStreamInfo video;
        video.index = -1;
        video.width = seq.width;
        video.height = seq.height;
        video.frameRate = seq.frameRate;
        video.duration = entry->range.duration;
        item->info.video.push_back(video);
    }
    LayerSpec file;
    file.item = item.get();
    file.media = item->id;
    file.streamIndex = 0;
    file.containerStream = -1;
    file.sourceTime = t - entry->range.start;
    file.decodeKey = item->id.value;
    // Only the burned-in caption of this sequence stays on top (evaluateVideoLayers appends it last: no clip, top level).
    // The preview file has no captions (rendered without them), so nothing is drawn twice.
    std::optional<LayerSpec> caption;
    if (captions && !layers.empty() && layers.back().caption && !layers.back().clip.isValid() && !layers.back().nestPath) {
        caption = std::move(layers.back());
    }
    layers.clear();
    layers.push_back(std::move(file));
    if (caption) {
        layers.push_back(std::move(*caption));
    }
}

void Scheduler::Impl::reapSessions(std::int64_t now) {
    const std::int64_t idle = static_cast<std::int64_t>(options.sessionIdleMs) * 1'000'000;
    for (auto it = sessions.begin(); it != sessions.end();) {
        if (now - it->second->lastUsedNs > idle) {
            it = sessions.erase(it); // joins the worker; its decoder and frames are released there
        } else {
            ++it;
        }
    }
}

void Scheduler::Impl::refreshStats(std::int64_t now) {
    if (now - lastStatsNs < kStatsIntervalNs && !options.collectTimings) {
        return;
    }
    lastStatsNs = now;
    std::map<std::string, int> counts;
    std::uint64_t decoded = 0;
    std::uint64_t errors = 0;
    for (const auto& [id, s] : sessions) {
        const std::string b = s->backend();
        ++counts[b.empty() ? std::string("opening") : b];
        decoded += s->decodedFrames();
        errors += s->errors();
    }
    std::string text;
    for (const auto& [name, count] : counts) {
        if (!text.empty()) {
            text += ", ";
        }
        text += name + " x" + std::to_string(count);
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> late(lateByMedia.begin(), lateByMedia.end());
    std::ranges::sort(late);
    const media::DecoderPoolStats poolStats = decoderPool ? decoderPool->stats() : media::DecoderPoolStats{};
    std::lock_guard lock(statsMutex);
    lateByMediaStats = std::move(late);
    backends = std::move(text);
    activeSessions = static_cast<int>(sessions.size());
    decodedFrames = decoded;
    decodeErrors = errors;
    poolFramesDecoded = poolStats.videoFramesDecoded;
    poolFramesDelivered = poolStats.videoFramesDelivered;
    if (options.collectTimings) {
        for (const auto& [id, s] : sessions) {
            s->takeSamples(samples);
        }
    }
}

Scheduler::Impl::Publish Scheduler::Impl::publishIfReady(Ticks target, const std::vector<LayerSpec>& layers,
                                                         const Sequence& seq, Ticks frame) {
    frames.clear();
    for (const LayerSpec& layer : layers) {
        if (!needsDecode(layer)) {
            frames.emplace_back(); // drawn, not decoded (titles)
            continue;
        }
        auto it = sessions.find(sessionKey(layer));
        media::VideoFrame f;
        Ticks wanted = layer.sourceTime;
        if (layer.interpolationSecond) {
            // P.2 Time Interpolation: the frame after the group's first one (decoded just before it).
            if (frames.empty() || !frames.back().isValid()) {
                frames.emplace_back(); // no first frame: the group shows nothing
                continue;
            }
            wanted = interpolationNextTime(frames.back());
        }
        auto state = it == sessions.end() ? detail::FrameSource::Lookup::Failed : it->second->lookup(wanted, f);
        if (state == detail::FrameSource::Lookup::Pending && layer.interpolationSecond) {
            // Not asked for yet when the first guess was another frame (variable frame rate): ask now.
            if (it->second->request(wanted, epoch, false, true)) {
                decodeRequests.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (state == detail::FrameSource::Lookup::Failed && layer.interpolationSecond) {
            state = detail::FrameSource::Lookup::Ready; // past the media's end: the first frame alone
            f = {};
        }
        if (state == detail::FrameSource::Lookup::Pending) {
            frames.clear();
            pendingMedia = layer.media.value;
            return Publish::NotReady;
        }
        if (state == detail::FrameSource::Lookup::Failed && isPreviewLayer(layer)) {
            // C.16: a preview file that does not decode (damaged, truncated): forget it - its decoder, the cache entry
            // and the file - and composite this target live on the next pass (the C.15 contract).
            previewValid[layer.item->name].valid = false;
            sessions.erase(sessionKey(layer));
            if (preview.previews) {
                preview.previews->remove(seq.id, TimeRange{target, Ticks{1}});
            }
            frames.clear();
            wake(); // another pass at once, now live
            return Publish::NotReady;
        }
        if (options.zeroCopy && sharedDeviceKnown && f.isValid() && f.payloadKind() != media::PayloadKind::CpuPlanes &&
            f.device() != sharedDevice) {
            // A surface of a device the renderer no longer uses (decoded across a device change, or by a pool
            // session that was busy when the others were closed): decode it again on the current device.
            staleDeviceFrames.fetch_add(1, std::memory_order_relaxed);
            sessions.erase(sessionKey(layer));
            if (decoderPool) {
                decoderPool->closeIdleSessions();
            }
            frames.clear();
            pendingMedia = layer.media.value;
            return Publish::NotReady;
        }
        frames.push_back(std::move(f)); // a failed lookup leaves an invalid frame: the layer is skipped
    }
    // P.2: a flow mix (Optical Flow, Morph Cut) waits for its motion like a frame still decoding; an analysis that
    // failed (a picture that does not decode) mixes as a dissolve instead.
    const render::CanvasSize canvas{seq.width, seq.height};
    if (needsMotion(layers)) {
        int waits = 0;
        MotionAnalyzer& analyzer = MotionAnalyzer::shared();
        for (const MotionRequest& request : motionRequestsOf(layers, frames, canvas)) {
            if (analyzer.find(request) || analyzer.failed(request)) {
                continue;
            }
            analyzer.request(request, true, [wakeUp = motionWake] { (*wakeUp)(); });
            ++waits;
        }
        motionWaits.store(waits, std::memory_order_relaxed);
        if (waits > 0) {
            frames.clear();
            pendingMedia = kMotionPending; // not the media's fault: no proxy is made for it
            return Publish::NotReady;
        }
    } else {
        motionWaits.store(0, std::memory_order_relaxed);
    }
    auto composite = pool.acquire();
    if (!composite) {
        poolExhausted.fetch_add(1, std::memory_order_relaxed);
        frames.clear();
        return Publish::NoComposite;
    }
    composite->revision = revision;
    composite->seekEpoch = epoch;
    composite->targetTime = target;
    composite->frameDuration = frame;
    composite->canvas = {seq.width, seq.height};
    composite->color = options.color;
    if (lastSnapshot) {
        composite->color.management = lastSnapshot->colorManagement; // the project's colour management (I.9)
    }
    // What the picture is made of (the monitor's quality badge), before the frames move into the composite.
    int proxies = 0;
    int originals = 0;
    int offline = 0;
    int coarsest = 1;
    for (std::size_t j = 0; j < layers.size() && j < frames.size(); ++j) {
        if (!frames[j].isValid()) {
            continue;
        }
        const auto s = sessions.find(sessionKey(layers[j]));
        const bool proxy = s != sessions.end() && s->second->proxy;
        (proxy ? proxies : originals) += 1;
        offline += s != sessions.end() && s->second->originalOffline ? 1 : 0;
        const LayerSpec& l = layers[j];
        const int streamHeight = l.item->info.video[static_cast<std::size_t>(l.streamIndex)].height;
        if (frames[j].height > 0 && streamHeight > frames[j].height) {
            coarsest = std::max(coarsest, (streamHeight + frames[j].height / 2) / frames[j].height);
        }
    }
    proxyLayers.store(proxies, std::memory_order_relaxed);
    originalLayers.store(originals, std::memory_order_relaxed);
    offlineProxyLayers.store(offline, std::memory_order_relaxed);
    decodedDivisor.store(coarsest, std::memory_order_relaxed);
    pendingMedia = 0;
    buildCompositeLayers(layers, frames, composite->layers, &foundMotion, canvas);
    frames.clear();
    composite->colorView = colorPreview.view;
    if (colorPreview.reference) {
        placeSideBySide(composite->layers, composite->canvas, colorPreview.reference);
    }
    int bypassed = 0;
    for (const render::CompositeLayer& layer : composite->layers) {
        bypassed += layer.bypassedEffects;
    }
    bypassedEffects.store(bypassed, std::memory_order_relaxed);
    composite->publishedNs = steadyNowNs();
    if (mailbox->publish(std::move(composite))) {
        published.fetch_add(1, std::memory_order_relaxed);
    }
    return Publish::Done;
}

bool Scheduler::Impl::invalidate(const ProjectState& before, const ProjectState& after, SequenceId sequenceId,
                                 Ticks frame, Ticks span) {
    const Sequence* oldSeq = before.findSequence(sequenceId);
    const Sequence* newSeq = after.findSequence(sequenceId);
    if (!newSeq) {
        return false;
    }
    const bool mediaChanged = before.media != after.media; // shared_ptr compare: shared items are equal
    // Source-anchored redactions (I.6) live outside the sequence and reach any clip of their media.
    const bool redactionsChanged = before.redactions != after.redactions;
    // C.8: an edit inside a sequence this one plays (nested or multicam, at any depth) changes what its clips show.
    const bool nestedChanged = nestedSequenceChanged(before, after, *newSeq);
    // Burned-in caption text depends on the speakers' names and colours (showSpeakers) and on where bleeped speech
    // plays, which audio clips decide (review wave 5 E5-P2-11).
    const bool speakersChanged = captions && before.speakers != after.speakers;
    // I.9: the working space and output of every picture.
    const bool colorChanged = before.colorManagement != after.colorManagement;
    // C.13: the brand kit themes graphics and caption styles that name it.
    const bool brandChanged = before.brandKit != after.brandKit;
    if (oldSeq == newSeq && !mediaChanged && !redactionsChanged && !nestedChanged && !speakersChanged && !colorChanged &&
        !brandChanged) {
        return false; // structural sharing: this sequence and the media are untouched
    }
    bool all = oldSeq == nullptr || redactionsChanged || nestedChanged || speakersChanged || colorChanged || brandChanged;
    dirty.clear();
    if (oldSeq && oldSeq != newSeq) {
        const SequenceChange change = diffSequence(*oldSeq, *newSeq);
        all = all || change.settingsChanged || change.trackListChanged || (captions && change.captionsChanged);
        for (const TrackChange& tc : change.tracks) {
            const Track* track = newSeq->findTrack(tc.track);
            if (!track) {
                track = oldSeq->findTrack(tc.track);
            }
            if (!track || track->kind != TrackKind::Video) {
                if (captions && track && hasSpeechRedactions(after)) {
                    // A moved or trimmed audio clip moves the bleeped speech the burned-in captions mask.
                    dirty.insert(dirty.end(), tc.ranges.begin(), tc.ranges.end());
                    all = all || tc.added || tc.removed;
                }
                continue; // otherwise audio edits never change the picture
            }
            if (tc.added || tc.removed || tc.headerChanged || tc.transitionsChanged) {
                all = true; // e.g. a track's output toggled: visible across the whole timeline
                break;
            }
            dirty.insert(dirty.end(), tc.ranges.begin(), tc.ranges.end());
        }
        for (ClipId removed : change.removedClips) {
            sessions.erase(removed.value);
        }
    }
    if (mediaChanged) {
        // Relink, offline and proxy changes are rare; sessions also notice a new path themselves
        // (sessionFor).
        const ChangeSet media = diff(before, after);
        all = all || !media.modifiedMedia.empty() || !media.removedMedia.empty();
    }
    if (!all && dirty.empty()) {
        return false;
    }
    const auto touches = [&](Ticks from, Ticks to) {
        if (all) {
            return true;
        }
        const TimeRange window = TimeRange::fromStartEnd(from, to);
        return std::any_of(dirty.begin(), dirty.end(),
                           [&](const TimeRange& r) { return r.overlaps(window); });
    };
    bool restart = false;
    if (lastStill.valid && touches(lastStill.target, lastStill.target + frame)) {
        lastStill.valid = false; // the paused picture shows an edited range: evaluate it again
        invalidations.fetch_add(1, std::memory_order_relaxed);
    }
    if (haveNext) {
        // Published but not yet shown: within lookahead (+ lead) steps of the next target.
        const Ticks reach{span.value * (options.lookaheadFrames + leadSteps + 2)};
        if (touches(nextTarget - reach, nextTarget + reach)) {
            restart = true;
            invalidations.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return restart;
}

void Scheduler::Impl::pass() {
    ProjectSnapshot snapshot;
    SequenceId sequenceId;
    std::shared_ptr<TransportClock> transport;
    std::uint64_t wantedEpoch = 0;
    std::optional<TimeRange> loop;
    {
        std::lock_guard lock(mutex);
        snapshot = session ? session->snapshot() : fixedSnapshot; // acquired once per pass
        sequenceId = sequence;
        transport = clock;
        wantedEpoch = requestedEpoch;
        loop = loopRange;
        if (previewVersion != appliedPreviewVersion) {
            preview = previewSettings;
            appliedPreviewVersion = previewVersion;
            lastStill.valid = false; // the paused picture is re-rendered from the new sources
        }
        if (captionVersion != appliedCaptionVersion) {
            captions = captionSettings;
            appliedCaptionVersion = captionVersion;
            lastStill.valid = false;
        }
        if (colorVersion != appliedColorVersion) {
            colorPreview = colorSettings;
            appliedColorVersion = colorVersion;
            lastStill.valid = false;
        }
    }
    passes.fetch_add(1, std::memory_order_relaxed);
    const std::int64_t now = steadyNowNs();
    if (!snapshot || !transport || !mailbox) {
        reapSessions(now);
        refreshStats(now);
        return;
    }
    const Sequence* seq = snapshot->findSequence(sequenceId);
    const Rate rate = seq ? seq->frameRate : Rate{};
    if (!seq || !rate.isValid() || !rate.isExact()) {
        reapSessions(now);
        refreshStats(now);
        return;
    }
    const Ticks frame = framesToTicks(1, rate, Rounding::Floor).value_or(Ticks{1});
    if (options.zeroCopy) {
        const media::DeviceToken shared = media::sharedD3D11Device();
        if (!sharedDeviceKnown) {
            sharedDevice = shared;
            sharedDeviceKnown = true;
        } else if (shared != sharedDevice) {
            // The renderer's device changed: nothing decoded on the old one can be drawn any more. Close every
            // session (decoders and cached surfaces), drop the old composites, and decode again on the new device.
            sharedDevice = shared;
            sessions.clear();
            if (decoderPool) {
                decoderPool->closeIdleSessions();
            }
            const std::uint64_t restarted = mailbox->beginSeek(render::SeekDisplay::Black);
            {
                std::lock_guard lock(mutex);
                requestedEpoch = std::max(requestedEpoch, restarted);
                wantedEpoch = requestedEpoch;
            }
            lastStill.valid = false;
            deviceChanges.fetch_add(1, std::memory_order_relaxed);
        }
    }

    const TransportSpeed speed = transport->effectiveSpeed().normalized();
    const int dir = speed.isZero() ? 0 : speed.isReverse() ? -1 : 1;
    // Shuttle: show every n-th sequence frame at n x (the display cadence stays the sequence rate).
    const std::int64_t stride =
        std::max<std::int64_t>(1, ceilDiv(std::abs(static_cast<std::int64_t>(speed.num)), speed.den));
    const Ticks span{frame.value * stride};

    if (snapshot != lastSnapshot) {
        // An edit, undo or redo: re-evaluate only what it touched.
        if (lastSnapshot && invalidate(*lastSnapshot, *snapshot, sequenceId, frame, span)) {
            const std::uint64_t restarted = mailbox->beginSeek(render::SeekDisplay::HoldPrevious);
            std::lock_guard lock(mutex);
            requestedEpoch = std::max(requestedEpoch, restarted);
            wantedEpoch = requestedEpoch;
        }
        lastSnapshot = snapshot;
        ++revision;
        // P.2: Morph Cuts are analysed in the background as soon as they are applied (Premiere's "Analyzing in
        // background"), not only when the playhead reaches them.
        MotionAnalyzer& analyzer = MotionAnalyzer::shared();
        for (const MotionRequest& request : morphCutRequests(*snapshot, *seq, &preview)) {
            if (!analyzer.find(request) && !analyzer.failed(request)) {
                analyzer.request(request, false, [wakeUp = motionWake] { (*wakeUp)(); });
            }
        }
    }
    if (wantedEpoch != epoch) {
        epoch = wantedEpoch;
        haveNext = false;
        haveLastPresent = false;
        leadSteps = 1;
        lastStill.valid = false;
        boundaryDone = false;
        if (decoderPool) {
            decoderPool->setEpoch(poolClient, epoch); // stale requests of this scheduler are cancelled at once
        }
        for (auto& [id, s] : sessions) {
            s->beginEpoch(epoch);
        }
    }

    const Ticks present = transport->transportTime(now + options.displayLatencyNs);
    // The sequence's explicit length (contract rev 2), never shorter than its content.
    const Ticks end = std::max(seq->duration, seq->contentEnd());
    // P.2 Loop: playback inside the loop goes no further than its end (forward) or its start (reverse), where the
    // transport wraps.
    const Ticks runEnd = loop && dir > 0 && present < loop->end() ? std::min(end, loop->end()) : end;
    const Ticks runStart = loop && dir < 0 && present >= loop->start ? loop->start : Ticks{0};

    targets.clear();
    Ticks step{};
    if (dir == 0) {
        const Ticks t = snapToFrame(present, rate);
        if (!(lastStill.valid && lastStill.target == t && lastStill.epoch == epoch)) {
            targets.push_back(t);
        } else {
            // The paused picture stays: keep its clips' sessions (decoder and frame cache) alive, so the next step,
            // scrub or play does not open and decode them again after sessionIdleMs (P.1).
            for (const std::uint64_t clip : lastStill.clips) {
                if (const auto it = sessions.find(clip); it != sessions.end()) {
                    it->second->lastUsedNs = now;
                }
            }
        }
    } else {
        step = Ticks{dir * span.value};
        const Ticks presentFrame = snapToFrame(present, rate);
        // P.2 Loop: the transport jumped back to the loop's start (forward; to its end in reverse) without a new
        // epoch. Restart at the playhead; what was queued for the end of the run is not shown.
        const bool wrapped = loop && haveLastPresent &&
                             (dir > 0 ? presentFrame < lastPresentFrame : presentFrame > lastPresentFrame);
        lastPresentFrame = presentFrame;
        haveLastPresent = true;
        if (!haveNext || wrapped) {
            nextTarget = presentFrame;
            haveNext = true;
        }
        if (wrapped) {
            leadSteps = 1;
            mailbox->clearPending();
        }
        const std::int64_t behind = dir > 0 ? (present - nextTarget).value : (nextTarget - present).value;
        const bool inSequence = dir > 0 ? nextTarget < runEnd : nextTarget >= runStart;
        bool waiting = false;
        if (behind >= span.value && inSequence) {
            // The next target's slot has passed. At shuttle speeds decoding one target can take longer than
            // its slot (a GOP walk), so: if it is decoded, show it late (reduced cadence) and aim further
            // ahead by how late it was; if it is still decoding and not hopelessly late, keep waiting for it
            // rather than re-aiming at a frame that would be late too (the picture would freeze forever).
            const std::int64_t missed = behind / span.value;
            evaluate(*snapshot, *seq, nextTarget, plan[0]);
            const std::uint64_t waitedFor = pendingMedia; // set by the NotReady publishes of this target
            const Publish result = publishIfReady(nextTarget, plan[0], *seq, frame);
            if (result == Publish::Done) {
                leadSteps = std::clamp<std::int64_t>(missed + 1, 1, kMaxLeadSteps);
                nextTarget += Ticks{(missed + leadSteps) * step.value};
                skippedLate.fetch_add(static_cast<std::uint64_t>(missed + leadSteps - 1),
                                      std::memory_order_relaxed);
                blameLate(waitedFor, plan[0], static_cast<std::uint64_t>(missed + leadSteps - 1));
            } else if (result == Publish::NotReady && missed < kMaxWaitSteps) {
                waiting = true;
            } else {
                nextTarget += Ticks{(missed + leadSteps) * step.value};
                skippedLate.fetch_add(static_cast<std::uint64_t>(missed + leadSteps),
                                      std::memory_order_relaxed);
                blameLate(pendingMedia != 0 ? pendingMedia : waitedFor, plan[0],
                          static_cast<std::uint64_t>(missed + leadSteps));
            }
        }
        if (waiting) {
            targets.push_back(nextTarget); // only the in-flight target: nothing else may delay it
        }
        // The transport moved without a new epoch (e.g. a loop wrap): restart at the playhead.
        const std::int64_t ahead =
            dir > 0 ? (nextTarget - presentFrame).value : (presentFrame - nextTarget).value;
        if (!waiting &&
            (ahead > span.value * (options.lookaheadFrames + kMaxLeadSteps) || ahead < -span.value)) {
            nextTarget = presentFrame;
        }
        // Lookahead is bounded relative to the playhead, not to what was already published: the decoders may
        // run faster than real time, and running further ahead would only overflow the mailbox.
        const Ticks horizon = presentFrame + Ticks{(options.lookaheadFrames + leadSteps) * step.value};
        for (int k = 0; k < options.lookaheadFrames && !waiting; ++k) {
            const Ticks t = nextTarget + Ticks{k * step.value};
            if ((dir > 0 && (t >= runEnd || t > horizon)) || (dir < 0 && (t < runStart || t < horizon))) {
                break;
            }
            targets.push_back(t);
        }
        const bool atEnd = dir > 0 && present >= end && stopAtEnd.load(std::memory_order_relaxed);
        if (!boundaryDone && (atEnd || (dir < 0 && present.value <= 0))) {
            boundaryDone = true;
            std::function<void(int)> callback;
            {
                std::lock_guard lock(mutex);
                callback = boundary;
            }
            if (callback) {
                callback(dir);
            }
        }
    }

    // Evaluate every target and queue its decodes, so the sessions work ahead. Work for targets the playhead
    // has passed is dropped first (at shuttle speeds the decoder would otherwise chase abandoned frames).
    for (std::size_t i = 0; i < targets.size(); ++i) {
        evaluate(*snapshot, *seq, targets[i], plan[i]);
    }
    // While playing, the source-time step between two targets of a layer (its clip's speed here), from the same
    // decoder's layer in the neighbouring target: the step its reverse windows take (FrameSource::targetStep).
    const auto stepOf = [&](std::size_t i, const LayerSpec& layer) {
        const std::size_t other = i + 1 < targets.size() ? i + 1 : (i > 0 ? i - 1 : i);
        if (other != i) {
            for (const LayerSpec& l : plan[other]) {
                if (needsDecode(l) && sessionKey(l) == sessionKey(layer)) {
                    return Ticks{std::abs(l.sourceTime.value - layer.sourceTime.value)};
                }
            }
        }
        return Ticks{};
    };
    for (std::size_t i = 0; i < targets.size(); ++i) {
        for (const LayerSpec& layer : plan[i]) {
            if (!needsDecode(layer)) {
                continue;
            }
            detail::FrameSource* s = sessionFor(layer, now);
            if (!s) {
                continue;
            }
            if (dir != 0) {
                if (const Ticks layerStep = stepOf(i, layer); layerStep.value > 0) {
                    s->targetStep = layerStep;
                }
            }
            // P.2: the frames of an interpolation group share a session and lie on both sides of the source time, so
            // neither may drop the other's request.
            const bool pairFrame = layer.interpolationSecond || (layer.clip.isValid() && !layer.nested &&
                                                                 std::any_of(plan[i].begin(), plan[i].end(), [&](const LayerSpec& l) {
                                                                     return l.interpolationSecond && sessionKey(l) == sessionKey(layer);
                                                                 }));
            if (i == 0 && dir != 0 && !pairFrame) {
                s->dropPassed(layer.sourceTime, dir < 0);
            }
            if (s->request(layer.sourceTime, epoch, dir < 0, i == 0)) {
                decodeRequests.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (needsMotion(plan[i])) {
            prefetchMotion(plan[i], *seq);
        }
    }

    // Publish in presentation order while every layer is ready.
    for (std::size_t i = 0; i < targets.size(); ++i) {
        if (publishIfReady(targets[i], plan[i], *seq, frame) != Publish::Done) {
            break;
        }
        if (dir == 0) {
            lastStill.target = targets[i];
            lastStill.epoch = epoch;
            lastStill.valid = true;
            lastStill.clips.clear();
            for (const LayerSpec& layer : plan[i]) {
                if (needsDecode(layer)) {
                    lastStill.clips.push_back(sessionKey(layer)); // C.16: preview files and nested leaves too
                }
            }
        } else {
            nextTarget = targets[i] + step;
            leadSteps = std::max<std::int64_t>(1, leadSteps - 1); // on time again: decay the lead
        }
    }
    frames.clear();
    reapSessions(now);
    refreshStats(now);
}

void Scheduler::Impl::prefetchMotion(const std::vector<LayerSpec>& layers, const Sequence& seq) {
    // The frames decoded so far (nothing is requested here): the second frame of an interpolation group at the time
    // after its first one, as publishIfReady reads it.
    std::vector<media::VideoFrame> found;
    found.reserve(layers.size());
    for (const LayerSpec& layer : layers) {
        media::VideoFrame f;
        if (needsDecode(layer)) {
            const auto it = sessions.find(sessionKey(layer));
            const bool firstReady = !found.empty() && found.back().isValid();
            if (it != sessions.end() && (!layer.interpolationSecond || firstReady)) {
                const Ticks wanted = layer.interpolationSecond ? interpolationNextTime(found.back()) : layer.sourceTime;
                if (it->second->lookup(wanted, f) != detail::FrameSource::Lookup::Ready) {
                    f = {};
                    if (layer.interpolationSecond) {
                        it->second->request(wanted, epoch, false, false); // the exact next frame, for the lookahead
                    }
                }
            }
        }
        found.push_back(std::move(f));
    }
    MotionAnalyzer& analyzer = MotionAnalyzer::shared();
    for (const MotionRequest& request : motionRequestsOf(layers, found, {seq.width, seq.height})) {
        if (!analyzer.find(request) && !analyzer.failed(request)) {
            analyzer.request(request, false, [wakeUp = motionWake] { (*wakeUp)(); });
        }
    }
}

void Scheduler::Impl::run() {
    for (;;) {
        bool moving = false;
        {
            std::unique_lock lock(mutex);
            moving = clock && clock->isPlaying();
            // While the transport moves, follow the clock closely; otherwise wait for a wake-up.
            cv.wait_for(lock, std::chrono::milliseconds(moving ? 2 : 50), [&] { return stop || wakeFlag; });
            if (stop) {
                break;
            }
            wakeFlag = false;
        }
        pass();
    }
    {
        std::lock_guard lock(motionWake->mutex); // P.2: analyses finishing later wake nothing
        motionWake->wake = nullptr;
    }
    sessions.clear(); // joins session workers / stops pool sources before the scheduler thread ends
    // Pool callbacks may still be running on pool workers: let them finish before the pool can be released, so
    // its last reference is never dropped on one of its own threads.
    poolGate->waitIdle(std::chrono::seconds(10));
}

// ---- Scheduler ----

Scheduler::Scheduler(std::shared_ptr<render::CompositeMailbox> mailbox, std::shared_ptr<TransportClock> clock,
                     SchedulerOptions options)
    : m_impl(std::make_unique<Impl>(std::move(mailbox), std::move(clock), options)) {
    m_impl->thread = std::thread([impl = m_impl.get()] { impl->run(); });
}

Scheduler::~Scheduler() {
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->stop = true;
    }
    m_impl->cv.notify_all();
    if (m_impl->thread.joinable()) {
        m_impl->thread.join();
    }
}

void Scheduler::setSession(const EditSession* session, SequenceId sequence) {
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->session = session;
        m_impl->fixedSnapshot.reset();
        m_impl->sequence = sequence;
    }
    m_impl->wake();
}

void Scheduler::setSnapshot(ProjectSnapshot snapshot, SequenceId sequence) {
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->session = nullptr;
        m_impl->fixedSnapshot = std::move(snapshot);
        m_impl->sequence = sequence;
    }
    m_impl->wake();
}

void Scheduler::setClock(std::shared_ptr<TransportClock> clock) {
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->clock = std::move(clock);
    }
    m_impl->wake();
}

void Scheduler::retarget(std::uint64_t epoch) {
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->requestedEpoch = std::max(m_impl->requestedEpoch, epoch);
    }
    m_impl->wake();
}

void Scheduler::setStopAtEnd(bool stop) {
    m_impl->stopAtEnd.store(stop, std::memory_order_relaxed);
    wake();
}

bool Scheduler::stopAtEnd() const { return m_impl->stopAtEnd.load(std::memory_order_relaxed); }

void Scheduler::setLoop(std::optional<TimeRange> range) {
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->loopRange = range && !range->isEmpty() ? range : std::nullopt;
    }
    wake();
}

void Scheduler::setBoundaryCallback(std::function<void(int)> callback) {
    std::lock_guard lock(m_impl->mutex);
    m_impl->boundary = std::move(callback);
}

void Scheduler::wake() {
    m_impl->wake();
}

void Scheduler::setPreview(PreviewSettings preview) {
    {
        std::lock_guard lock(m_impl->mutex);
        // C.16: the same preview cache set again is how the host says its files changed (a render finished): the
        // preview-file memo and the held picture are refreshed (a new version), so equal settings with a cache count.
        if (preview == m_impl->previewSettings && !preview.previews) {
            return;
        }
        m_impl->previewSettings = std::move(preview);
        ++m_impl->previewVersion;
    }
    m_impl->wake();
}

void Scheduler::setCaptions(std::optional<CaptionSettings> captions) {
    {
        std::lock_guard lock(m_impl->mutex);
        if (captions == m_impl->captionSettings) {
            return;
        }
        m_impl->captionSettings = std::move(captions);
        ++m_impl->captionVersion;
    }
    m_impl->wake();
}

void Scheduler::setColorPreview(ColorPreview preview) {
    {
        std::lock_guard lock(m_impl->mutex);
        if (preview == m_impl->colorSettings) {
            return;
        }
        m_impl->colorSettings = std::move(preview);
        ++m_impl->colorVersion;
    }
    m_impl->wake();
}

void placeSideBySide(std::vector<render::CompositeLayer>& layers, render::CanvasSize canvas,
                     const std::shared_ptr<const render::StillImage>& reference) {
    // canvas' = centre + offset + (canvas - centre) / 2: position and scale halve, the offset moves to a half.
    const double quarter = canvas.width / 4.0;
    for (render::CompositeLayer& layer : layers) {
        layer.transform.position = {quarter + layer.transform.position.x / 2.0, layer.transform.position.y / 2.0};
        layer.transform.scale /= 2.0;
    }
    if (!reference || reference->width() <= 0 || reference->height() <= 0) {
        return;
    }
    render::CompositeLayer still;
    still.view = reference->view(reference);
    still.graphics = true; // a grabbed still is display-referred, like a title (I.9)
    still.transform.position = {-quarter, 0.0};
    still.transform.scale = std::min(canvas.width / 2.0 / reference->width(), static_cast<double>(canvas.height) / reference->height());
    layers.insert(layers.begin(), std::move(still));
}

PreviewSettings Scheduler::preview() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->previewSettings;
}

SchedulerStats Scheduler::stats() const {
    SchedulerStats s;
    s.passes = m_impl->passes.load(std::memory_order_relaxed);
    s.published = m_impl->published.load(std::memory_order_relaxed);
    s.skippedLate = m_impl->skippedLate.load(std::memory_order_relaxed);
    s.decodeRequests = m_impl->decodeRequests.load(std::memory_order_relaxed);
    s.poolExhausted = m_impl->poolExhausted.load(std::memory_order_relaxed);
    s.invalidations = m_impl->invalidations.load(std::memory_order_relaxed);
    s.bypassedEffects = m_impl->bypassedEffects.load(std::memory_order_relaxed);
    s.proxyLayers = m_impl->proxyLayers.load(std::memory_order_relaxed);
    s.originalLayers = m_impl->originalLayers.load(std::memory_order_relaxed);
    s.offlineProxyLayers = m_impl->offlineProxyLayers.load(std::memory_order_relaxed);
    s.decodeDivisor = m_impl->decodedDivisor.load(std::memory_order_relaxed);
    s.deviceChanges = m_impl->deviceChanges.load(std::memory_order_relaxed);
    s.staleDeviceFrames = m_impl->staleDeviceFrames.load(std::memory_order_relaxed);
    s.motionWaits = m_impl->motionWaits.load(std::memory_order_relaxed);
    std::lock_guard lock(m_impl->statsMutex);
    s.lateFramesByMedia = m_impl->lateByMediaStats;
    s.decodedFrames = m_impl->decodedFrames;
    s.decodeErrors = m_impl->decodeErrors;
    s.activeSessions = m_impl->activeSessions;
    s.decodeBackends = m_impl->backends;
    s.poolFramesDecoded = m_impl->poolFramesDecoded;
    s.poolFramesDelivered = m_impl->poolFramesDelivered;
    return s;
}

std::vector<std::int64_t> Scheduler::takeDecodeSamples() {
    std::vector<std::int64_t> out;
    std::lock_guard lock(m_impl->statsMutex);
    out.swap(m_impl->samples);
    m_impl->samples.reserve(m_impl->options.collectTimings ? 1 << 16 : 0);
    return out;
}

} // namespace wabcut::playback
