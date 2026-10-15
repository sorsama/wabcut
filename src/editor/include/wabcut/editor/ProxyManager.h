#pragma once

// Progressive media readiness (docs/architecture.md, "Proxies and preview quality"): clips are usable the moment
// they are imported, and a background queue makes proxies for media this machine cannot play in real time.
//
//   ResourceGovernor   foreground activity (playback, timeline gestures, scrubbing, export) that background jobs
//                      yield to: a job waits between two frames until the foreground is idle.
//   ProxyQueue         the proxy job queue: one worker thread, most urgent first (under the playhead, then on the
//                      timeline, then selected in the library, then the rest; within a rank, long recordings after
//                      shorter media); a more urgent job preempts a less urgent one (which is queued again), a long
//                      recording only at the very start of its transcode (see kLongSourceForProxy).
//   ProxyManager       the project's proxy registry, kept OUT of the undo history (a background completion must
//                      never become an undo step or land inside a gesture). It feeds the preview scheduler
//                      (playback::ProxyTable), writes MediaLocator::proxy (with provenance) into every save and
//                      autosave, and records each proxy in "<project> Proxies/proxies.json" so proxies are found
//                      again after a quit without saving. Stale proxies (the original's fingerprint changed, e.g.
//                      relinked to a re-rendered file) leave the preview and are regenerated.
//
// Ownership (Codex wave-3 P1-1): a proxy path from a project file or an index is untrusted. Wabcut deletes a proxy
// file (Delete proxies, a regeneration replacing it) only when it owns it: a file directly inside one of its managed
// proxy folders (this project's "<project> Proxies", the untitled proxy folder), neither the folder nor the file a
// link, recorded in that folder's index by Wabcut's generator, and not the original (by path in any case, hard link,
// symlink or junction). Any other proxy is attached: it may play in the preview, and deleting it only detaches it.
// Proxies that are the original are refused outright, and index entries must be bare file names.
//
// Threading: ResourceGovernor and ProxyQueue are thread-safe. ProxyManager belongs to the GUI thread (like
// ProjectDocument); poll() applies finished work there.

#include <wabcut/export/Proxy.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/MediaInfo.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/Time.h>
#include <wabcut/playback/video/Evaluate.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace wabcut::editor {

class ProjectDocument;

// ---- Policy -------------------------------------------------------------------------------------------------

enum class ProxyPolicy : std::uint8_t {
    Off,    ///< only proxies the user asks for
    Auto,   ///< media this machine cannot decode in real time (see proxyReason)
    Always, ///< every video file
};

/// What a trial open of the original found (VideoDecoder::isHardwareAccelerated()).
struct DecodeCapability {
    bool known = false;           ///< a trial open ran
    bool hardwareDecodes = false; ///< the GPU decoder took the stream
    int cpuCores = 0;             ///< std::thread::hardware_concurrency()
};

/// Frames a media item may miss in playback before Auto proxies it.
inline constexpr std::uint64_t kLateFramesForProxy = 3;

// The automatic policy's thresholds (proxyReason). Constants for now: they are tuned from measurements
// (docs/performance.md).

/// A high frame rate: 120/144/240 fps game captures. Every source frame has to be decoded even when the sequence shows
/// only every fourth (P-frames depend on each other), so a 1080p240 source decodes as many pixels per second as a 4K60
/// one, and a seek walks a 2-second GOP of 480 frames.
inline constexpr double kHighFrameRateForProxy = 100.0;
/// Above this many pixels a high-frame-rate source is proxied even when the GPU decodes it (1080p).
inline constexpr std::int64_t kHighFrameRatePixelsForProxy = std::int64_t{1920} * 1080;
/// UHD 4K: a long-GOP source this large is proxied when the GPU does not decode it.
inline constexpr std::int64_t kUhdPixelsForProxy = std::int64_t{3840} * 2160;
/// A long recording (a stream VOD, a session capture). Length alone is never a reason for a proxy (see proxyReason);
/// when another reason proxies one, its job waits behind shorter media of the same urgency and is preempted only
/// while it is under kLongJobPreemptProgress.
inline constexpr Ticks kLongSourceForProxy = Ticks::fromSeconds(30 * 60);
/// Progress under which a running long recording's proxy may still be preempted (under a minute of a 2-hour
/// 1440p60 transcode): the transcode cannot resume, so a later preemption would throw that work away.
inline constexpr double kLongJobPreemptProgress = 0.02;

/// Why the automatic policy proxies `stream`, or "" when it does not:
///   * larger than UHD 4K (3840 x 2160 pixels or more than that many pixels);
///   * H.264 above 8-bit or other than 4:2:0 (10-bit / 4:2:2: no consumer hardware decoder before NVIDIA RTX 50)
///     unless the trial open says the GPU decodes it;
///   * long-GOP HEVC 10-bit on a weak machine: no hardware decoding and fewer than 8 cores, or larger than 1080p;
///   * a high frame rate (kHighFrameRateForProxy) above 1080p (kHighFrameRatePixelsForProxy), or without hardware
///     decoding ("240 fps 1080p without hardware decoding");
///   * UHD 4K (kUhdPixelsForProxy) long-GOP media (H.264, HEVC, AV1, VP9, ...; not intra codecs such as ProRes or
///     DNxHR) without hardware decoding;
///   * dropped frames in playback (`lateFrames` >= kLateFramesForProxy).
/// Sources with alpha are never proxied (the proxy codecs have no alpha). Proxies keep the source's frames and
/// timestamps (exporter::transcodeProxy), so a 240 fps proxy still has every frame slow motion uses.
///
/// Length is no reason on its own: a seek in a long recording costs what it costs in a short one with the same GOP
/// (it decodes from the keyframe before the target, never from the start); P.1 measured a proxy scrubbing no faster
/// than a hardware-decoded original (p95 135 ms vs 124 ms for HEVC 4K); and a proxy of a 2-hour 1440p60 VOD takes
/// about half an hour to make and is larger than the recording (ProRes Proxy 1280x720p60 is about 38 Mb/s, the
/// capture 24 Mb/s). A long recording that also matches one of the rules above (kLongSourceForProxy) still gets its
/// proxy, as a long job: see ProxyQueue.
std::string proxyReason(const VideoStreamInfo& stream, const DecodeCapability& capability, std::uint64_t lateFrames);
/// Whether proxyReason() needs a trial open of the original to decide (H.264 10-bit / 4:2:2, HEVC 10-bit, a high
/// frame rate up to 1080p, UHD 4K long-GOP).
bool needsCapabilityProbe(const VideoStreamInfo& stream);

std::string_view policyName(ProxyPolicy policy);
std::optional<ProxyPolicy> parsePolicy(std::string_view name);

// ---- Resource governor ----------------------------------------------------------------------------------------

class ResourceGovernor {
public:
    enum Activity : unsigned {
        Playback = 1u << 0,
        Gesture = 1u << 1, ///< a timeline drag/trim (EditSession::inGesture())
        Scrub = 1u << 2,
        Export = 1u << 3,
    };

    void setActive(Activity activity, bool active);
    /// Bitwise OR of the active activities.
    unsigned activities() const;
    bool busy() const { return activities() != 0; }
    /// Blocks while busy; returns false when `stop` is requested or `abort()` returns true meanwhile (polled).
    bool waitIdle(std::stop_token stop, const std::function<bool()>& abort = {}) const;
    /// Times a background job had to wait (diagnostics, tests).
    std::uint64_t waits() const { return m_waits.load(); }

private:
    mutable std::mutex m_mutex;
    mutable std::condition_variable_any m_cv;
    unsigned m_activities = 0;
    mutable std::atomic<std::uint64_t> m_waits{0};
};

// ---- Queue ------------------------------------------------------------------------------------------------------

/// Most urgent first.
enum class ProxyPriority : std::uint8_t { UnderPlayhead = 0, OnTimeline = 1, Selected = 2, Background = 3 };

enum class ProxyState : std::uint8_t {
    None,       ///< no proxy, nothing queued
    Queued,
    Generating,
    Paused,     ///< generating, waiting for the foreground (playback, a drag) or a queue pause
    Ready,
    Stale,      ///< the original changed since the proxy was made: it is not used and is being replaced
    Failed,
};

std::string_view stateName(ProxyState state);

struct ProxyJob {
    MediaId media;
    std::string source; ///< the original (UTF-8)
    std::filesystem::path output;
    exporter::ProxySettings settings;
    ProxyPriority priority = ProxyPriority::Background;
    std::string reason; ///< why it was queued ("H.264 4:2:2 10-bit: no hardware decoder", "requested", ...)
    SourceFingerprint fingerprint; ///< of the original, for the provenance
    std::uint64_t generation = 1;
    /// The original is a long recording (kLongSourceForProxy): queued after shorter media of the same priority, and
    /// preempted only under kLongJobPreemptProgress.
    bool longSource = false;
};

struct ProxyOutcome {
    ProxyJob job;
    std::expected<exporter::ProxyResult, Error> result;
};

struct ProxyJobStatus {
    MediaId media;
    ProxyState state = ProxyState::Queued;
    double progress = 0.0; ///< 0..1
    ProxyPriority priority = ProxyPriority::Background;
    std::string reason;
};

class ProxyQueue {
public:
    using Transcoder =
        std::function<std::expected<exporter::ProxyResult, Error>(const ProxyJob&, const exporter::ProxyControl&)>;

    /// `transcoder` defaults to exporter::transcodeProxy (tests pass a fake).
    explicit ProxyQueue(std::shared_ptr<ResourceGovernor> governor, Transcoder transcoder = {});
    ~ProxyQueue(); ///< cancels the running job and joins the worker
    ProxyQueue(const ProxyQueue&) = delete;
    ProxyQueue& operator=(const ProxyQueue&) = delete;

    /// Queues `job`; replaces a queued (not running) job for the same media. Order: priority, then shorter media before
    /// long recordings (ProxyJob::longSource), then first come first served.
    void enqueue(ProxyJob job);
    /// Re-ranks a queued job (a running one keeps running unless something more urgent waits and it may be
    /// preempted: under 75 %, a long recording under kLongJobPreemptProgress).
    void setPriority(MediaId media, ProxyPriority priority);
    void cancel(MediaId media);
    void cancelAll();
    /// Holds the queue between frames (in addition to the governor).
    void setPaused(bool paused);
    bool paused() const;
    bool contains(MediaId media) const;
    /// Queued and running jobs, the running one first.
    std::vector<ProxyJobStatus> statuses() const;
    /// Jobs that finished (succeeded, failed or cancelled) since the last call.
    std::vector<ProxyOutcome> takeFinished();
    /// Called on the worker thread when something changed (progress, a finished job).
    void setListener(std::function<void()> listener);
    /// Media ids in the order the worker started them (tests).
    std::vector<MediaId> startOrder() const;
    /// Waits until nothing is queued or running (tests); false on timeout.
    bool waitIdle(std::chrono::milliseconds timeout) const;

private:
    struct Entry {
        ProxyJob job;
        std::uint64_t sequence = 0;
    };
    void run(std::stop_token stop);
    std::optional<Entry> takeNextLocked();
    /// Whether a more urgent job may preempt the running one (it is far from done; a long recording barely started).
    bool preemptibleLocked() const;
    void notify();

    std::shared_ptr<ResourceGovernor> m_governor;
    Transcoder m_transcoder;
    mutable std::mutex m_mutex;
    mutable std::condition_variable_any m_cv;
    std::vector<Entry> m_queued;
    std::optional<Entry> m_running;
    std::stop_source m_runningStop;
    bool m_preempt = false;
    double m_progress = 0.0;
    bool m_waiting = false;
    bool m_paused = false;
    std::uint64_t m_sequence = 0;
    std::vector<ProxyOutcome> m_finished;
    std::vector<MediaId> m_started;
    std::function<void()> m_listener;
    std::jthread m_thread; // last: stops first
};

// ---- Manager ------------------------------------------------------------------------------------------------

struct ProxyManagerOptions {
    /// Where an untitled project's proxies go ("<root>/Untitled Proxies"); a saved project's go next to it.
    std::filesystem::path untitledProxyRoot;
    exporter::ProxySettings settings{};
    ProxyPolicy policy = ProxyPolicy::Auto;
    std::shared_ptr<ResourceGovernor> governor;
    ProxyQueue::Transcoder transcoder;
    /// Trial-open originals to learn whether the GPU decodes them (Auto). Off: unknown = not hardware decoded.
    bool probeHardware = true;
};

struct ProxyStatus {
    MediaId media;
    ProxyState state = ProxyState::None;
    double progress = 0.0;
    std::string detail;          ///< queue reason, failure, or "ProRes Proxy 1/2"
    std::filesystem::path file;  ///< the proxy (Ready / Stale)
    bool originalOffline = false;
};

struct ProxyCounts {
    int ready = 0;
    int wanted = 0; ///< ready + queued + generating + stale + failed: the "n" of "Proxies ready/n"
    int queued = 0;
    int generating = 0;
    int failed = 0;
    int stale = 0;
};

class ProxyManager {
public:
    ProxyManager(ProjectDocument& document, ProxyManagerOptions options);
    ~ProxyManager();
    ProxyManager(const ProxyManager&) = delete;
    ProxyManager& operator=(const ProxyManager&) = delete;

    ProxyPolicy policy() const { return m_options.policy; }
    void setPolicy(ProxyPolicy policy);
    const exporter::ProxySettings& settings() const { return m_options.settings; }
    void setSettings(const exporter::ProxySettings& settings);
    ResourceGovernor& governor() { return *m_options.governor; }
    ProxyQueue& queue() { return *m_queue; }

    /// Re-reads the project (after open / new / import / relink; cheap when the media list did not change):
    /// adopts saved proxies (MediaLocator::proxy) and those recorded in the proxy folder's index, forgets missing
    /// files, marks stale proxies, and queues what the policy asks for.
    void refresh(bool force = false);
    /// Priority hints from the UI: media under the playhead, on the visible timeline, selected in the library.
    void setFocus(const std::vector<MediaId>& underPlayhead, const std::vector<MediaId>& onTimeline,
                  const std::vector<MediaId>& selected);
    /// Playback stats (SchedulerStats::lateFramesByMedia): Auto proxies media that keeps dropping frames.
    void setLateFrames(const std::vector<std::pair<std::uint64_t, std::uint64_t>>& late);
    /// Queues proxies for `media` regardless of the policy (Library > Create proxy). Returns how many were queued.
    int generate(const std::vector<MediaId>& media);
    /// Cancels queued work and removes the proxies of `media`: files Wabcut owns are deleted, attached ones only
    /// detached (the originals and anything outside the managed folders are never touched). Returns how many proxies
    /// were removed.
    int deleteProxies(const std::vector<MediaId>& media);
    /// Applies finished jobs and background probes (GUI thread). Returns true when something changed.
    bool poll();
    /// Trial opens (Auto) still running or not yet applied by poll().
    bool probing() const { return !m_probing.empty(); }

    /// What the preview scheduler decodes (Scheduler::setPreview). Rebuilt when the registry changes.
    std::shared_ptr<const playback::ProxyTable> table() const { return m_table; }
    std::vector<ProxyStatus> statuses() const;
    ProxyStatus status(MediaId media) const;
    ProxyCounts counts() const;
    std::optional<std::filesystem::path> proxyFile(MediaId media) const;
    /// Where new proxies of this project go.
    std::filesystem::path proxyFolder() const;
    /// Every proxy file the project uses, and its originals (the cache inspector never deletes these).
    std::set<std::filesystem::path> filesInUse() const;
    /// Writes the registry into `state`'s MediaLocator::proxy fields (ProjectDocument's save filter).
    void applyTo(ProjectState& state) const;

    /// Called after poll()/refresh() changed something (GUI thread).
    std::function<void()> onChanged;

private:
    struct Entry {
        ProxyMedia proxy;
        bool stale = false;
        bool owned = false; ///< made by Wabcut in a managed folder: it may be deleted (see the file comment)
    };
    void rebuildTable();
    /// False when nothing was queued (the proxy folder is a link: nothing is written through it).
    bool queueFor(const MediaItem& item, ProxyPriority priority, std::string reason);
    /// The folders whose files Wabcut may own: this project's proxy folder and the untitled one.
    std::vector<std::filesystem::path> managedFolders() const;
    /// `file` is (or reaches) the original of any media item of `state`.
    static bool isOriginal(const std::filesystem::path& file, const ProjectState& state);
    /// Wabcut owns `file` (see the file comment).
    bool owns(const std::filesystem::path& file, const ProjectState& state) const;
    /// isOriginal(), with the originals' set of the refresh that runs (P.1) when there is one.
    bool originalOf(const std::filesystem::path& file, const ProjectState& state) const;
    /// What one refresh() reads once instead of once per media item: the originals' set and the proxy indexes.
    struct RefreshCache;
    RefreshCache* m_refreshCache = nullptr;
    ProxyPriority priorityOf(MediaId media) const;
    std::uint64_t nextGeneration(MediaId media) const;
    void adoptFromIndex(const MediaItem& item, const ProjectState& state);
    void recordInIndex(const std::filesystem::path& file, const ProxyMedia& proxy, const std::string& sourcePath);
    void startProbes(std::vector<std::pair<MediaId, std::string>> items);
    void changed();

    ProjectDocument& m_document;
    ProxyManagerOptions m_options;
    std::unique_ptr<ProxyQueue> m_queue;
    std::map<std::uint64_t, Entry> m_registry;    // media id -> proxy (Ready or Stale)
    std::map<std::uint64_t, std::string> m_failed; // media id -> error
    std::map<std::uint64_t, std::string> m_reasons; // media id -> why Auto/Always queued it (kept for display)
    std::set<std::uint64_t> m_known;              // media the registry has decided about (applyTo)
    std::set<std::uint64_t> m_declined;           // Auto looked and said no (not re-probed)
    std::set<std::uint64_t> m_userDeleted;        // the user deleted its proxy: never queued automatically again
    std::map<std::uint64_t, std::uint64_t> m_late;
    std::set<std::uint64_t> m_underPlayhead, m_onTimeline, m_selected;
    std::shared_ptr<const playback::ProxyTable> m_table = std::make_shared<playback::ProxyTable>();
    std::string m_signature; // media list the last refresh saw
    // Hardware probes (Auto) run on their own thread; results are applied by poll().
    std::mutex m_probeMutex;
    std::vector<std::pair<MediaId, DecodeCapability>> m_probed;
    std::set<std::uint64_t> m_probing;
    struct ProbeThread {
        std::shared_ptr<std::atomic<bool>> done;
        std::jthread thread;
    };
    std::vector<ProbeThread> m_probeThreads;
    std::atomic<bool> m_dirty{false};
};

} // namespace wabcut::editor
