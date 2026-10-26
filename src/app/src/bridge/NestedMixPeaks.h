#pragma once

// Exact waveforms of sequences (0.5, phase 2): the audio clip of a nested sequence, a multicam clip or a merged clip
// plays a mix (TrackRenderer's NestedMix: the tracks nestedTracksRead() gives, with their clip and track effects, gains,
// faders and pans, transitions and, for a nested sequence, its master stage). NestedMixPeaks renders that mix offline on
// a worker thread of its own (playback::OfflineMixdown over the project's media, at a reduced rate) and keeps only its
// peaks: the loudest sample of every 2 ms (MixPeaks; about 3.6 MB an hour), never the audio itself.
//
// A mix is cached by its sequence and the tracks it reads, with a stamp of everything it depends on (nestedMixStamp: the
// sequence and every sequence it nests, the media items their clips play, the redactions). A lookup whose stamp changed
// (an edit inside, a relink) renders the mix again and stops a render of the old content still running; until the new
// one is complete the caller draws its approximation (NestedPeaks.h, phase 1), as it does when a mix fails (offline or
// unreadable media). Partial results are published while a mix renders, so the rendered start is exact early on.
//
// Threading: lookup() is called from TimelineView::updatePaintNode (render thread, GUI thread blocked), never blocks on
// a render and only queues one. mixesChanged() is queued to this object's thread; sequencesChangedSince() says which
// sequences' mixes moved on. The worker is stopped and joined by the destructor (a render stops between two blocks).

#include <wabcut/model/Clip.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/Sequence.h>
#include <wabcut/model/Time.h>

#include <QObject>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <vector>

namespace wabcut::app {

/// The peaks of a rendered mix: for every bin of kBinFrames samples at kSampleRate (2 ms) from nested time 0, the
/// loudest |sample| of any channel (0..32767). Kept in chunks, so a partial result shares the chunks already done.
struct MixPeaks {
    static constexpr int kSampleRate = 16000;
    static constexpr int kBinFrames = 32;
    static constexpr std::size_t kChunkBins = 8192;
    static constexpr std::size_t kBlockBins = 64; ///< the coarse level: the loudest bin of each 64 (128 ms)
    struct Chunk {
        std::vector<std::uint16_t> bins;
        std::vector<std::uint16_t> blocks;
    };
    std::vector<std::shared_ptr<const Chunk>> chunks;
    std::size_t binCount = 0;
    bool complete = false;

    static Ticks binTicks();
    /// Nested time rendered so far (the whole mix once complete).
    Ticks reach() const;
    /// The loudest peak in [from, to) of nested time, 0..1; 0 where nothing was rendered.
    float peak(Ticks from, Ticks to) const;
    std::size_t bytes() const;
};

/// What NestedPeaks asks (TimelineView passes the real one; tests their own).
class NestedMixSource {
public:
    enum class State { Unavailable, Pending, Ready, Failed };
    struct Lookup {
        State state = State::Unavailable;
        /// Ready: the mix. Pending: what is rendered so far (null before the first part).
        std::shared_ptr<const MixPeaks> peaks;
    };
    virtual ~NestedMixSource() = default;
    /// The mix of `clip`, a nested, multicam or merged audio clip of `project`; a miss, or a mix whose content changed,
    /// queues a render. Thread-safe and non-blocking.
    virtual Lookup lookup(const Clip& clip, const ProjectSnapshot& project) const = 0;
    /// Sequences whose mixes moved on (a part rendered, done, failed) after `serial`; `now` receives the serial to pass
    /// next time.
    virtual std::vector<std::uint64_t> sequencesChangedSince(std::uint64_t serial, std::uint64_t& now) const = 0;
};

/// A stamp of what the mix of `sequence` depends on: it and every sequence it nests (an edit replaces a sequence's
/// pointer), the media items their audio clips play (a relink replaces an item) and the redactions. 0 when it does not
/// exist.
std::uint64_t nestedMixStamp(const ProjectState& state, SequenceId sequence);

class NestedMixPeaks final : public QObject, public NestedMixSource {
    Q_OBJECT
public:
    /// One render: the nested sequence, prepared (its audio only, its own routing left out as NestedMix leaves it), and how
    /// it is mixed.
    struct Request {
        ProjectSnapshot project;
        std::filesystem::path projectFile;
        std::shared_ptr<const Sequence> sequence;
        std::optional<std::vector<TrackId>> tracks; ///< a multicam clip's angle tracks (else every track)
        bool honourMuteSolo = true;                 ///< a nested sequence: its mute and solo; a multicam source: none
        bool masterEffects = true;                  ///< a nested sequence's master stage; a multicam source has none
        Ticks length;
    };
    /// Receives the rendered samples (interleaved) in order; false: stop.
    using Sink = std::function<bool(std::span<const float> interleaved, int channels)>;
    /// Renders `request` into `sink`; false when it failed or stopped.
    using Renderer = std::function<bool(const Request& request, const Sink& sink, std::stop_token stop)>;

    explicit NestedMixPeaks(QObject* parent = nullptr);
    ~NestedMixPeaks() override;

    Lookup lookup(const Clip& clip, const ProjectSnapshot& project) const override;
    std::vector<std::uint64_t> sequencesChangedSince(std::uint64_t serial, std::uint64_t& now) const override;

    /// Where media paths relative to the project are resolved (GUI thread).
    void setProjectFile(std::filesystem::path file);
    /// Tests: renders with `renderer` instead of the media (null: the media again).
    void setRenderer(Renderer renderer);
    /// Peaks kept in memory (default 96 MiB), least recently used first out.
    void setBudget(std::size_t bytes);

    struct Stats {
        int started = 0;
        int completed = 0;
        int cancelled = 0;
        int failed = 0;
        std::size_t bytes = 0;
        std::size_t entries = 0;
    };
    Stats stats() const;

    /// The default renderer: playback::OfflineMixdown over the project's media files (checked against the project file),
    /// at MixPeaks::kSampleRate, stereo. Fails when a clip's media cannot be read.
    static bool renderWithMedia(const Request& request, const Sink& sink, std::stop_token stop);

signals:
    /// A mix moved on (queued to this object's thread).
    void mixesChanged();

private:
    struct Entry {
        SequenceId sequence;
        State state = State::Pending;
        std::uint64_t stamp = 0;               ///< what `peaks` (or the failure) is of
        std::shared_ptr<const MixPeaks> peaks; ///< Ready: the mix
        std::uint64_t pendingStamp = 0;        ///< being rendered or queued (0: none)
        std::shared_ptr<const MixPeaks> partial; ///< what is rendered of pendingStamp so far
        std::uint64_t lastUse = 0;
    };
    struct Job {
        std::uint64_t key = 0;
        std::uint64_t stamp = 0;
        Request request;
    };
    struct Running {
        std::uint64_t key = 0;
        std::uint64_t stamp = 0;
        std::stop_source stop{std::nostopstate};
    };
    void enqueue(Job job) const; // m_mutex held
    void worker(std::stop_token stop);
    void publish(const Job& job, std::shared_ptr<const MixPeaks> partial);
    void finish(const Job& job, std::shared_ptr<const MixPeaks> peaks, bool cancelled);
    void markChanged(SequenceId sequence) const; // m_mutex held
    void evictOverBudget();                      // m_mutex held

    mutable std::mutex m_mutex;
    std::filesystem::path m_projectFile;
    Renderer m_renderer;
    mutable std::unordered_map<std::uint64_t, Entry> m_entries;
    mutable std::deque<Job> m_jobs;
    mutable Running m_running;
    mutable std::uint64_t m_clock = 0;
    mutable std::uint64_t m_serial = 0;
    mutable std::unordered_map<std::uint64_t, std::uint64_t> m_changedAt; ///< sequence -> serial of its last change
    mutable ProjectSnapshot m_memoProject;                               ///< the snapshot m_stampMemo is of
    mutable std::unordered_map<std::uint64_t, std::uint64_t> m_stampMemo; ///< sequence -> nestedMixStamp
    std::size_t m_budget = std::size_t{96} << 20;
    std::size_t m_bytes = 0;
    Stats m_stats;
    mutable std::condition_variable_any m_wake;
    /// Started on the first render; declared last: stopped and joined before anything above goes.
    mutable std::jthread m_thread;
};

} // namespace wabcut::app
