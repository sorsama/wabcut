#include "NestedMixPeaks.h"

#include <wabcut/editor/ProjectDocument.h>
#include <wabcut/model/Resolve.h>
#include <wabcut/playback/audio/AudioRedactions.h>
#include <wabcut/playback/audio/AudioTakes.h>
#include <wabcut/playback/audio/MediaAudioFeed.h>
#include <wabcut/playback/audio/OfflineMixdown.h>

#include <QMetaObject>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_set>

namespace wabcut::app {

namespace {

constexpr auto kPublishInterval = std::chrono::milliseconds(250);

std::uint64_t mixHash(std::uint64_t key, std::uint64_t value) {
    key ^= value + 0x9E3779B97F4A7C15ull + (key << 6) + (key >> 2);
    return key * 0xFF51AFD7ED558CCDull;
}

std::uint64_t address(const void* p) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p));
}

void stampOf(const ProjectState& state, SequenceId id, std::size_t depth, std::uint64_t& key,
             std::unordered_set<std::uint64_t>& seen) {
    if (depth > kMaxNestingDepth || !seen.insert(id.value).second)
        return;
    const SequencePtr sequence = state.sequencePtr(id);
    key = mixHash(key, address(sequence.get()));
    if (!sequence)
        return;
    for (const TrackPtr& track : sequence->audioTracks) {
        if (!track)
            continue;
        for (const Clip& c : track->clips) {
            if (c.isNested())
                stampOf(state, c.sequence, depth + 1, key, seen);
            else if (c.media.isValid())
                key = mixHash(key, address(state.findMedia(c.media))); // a relink, take or new metadata replaces the item
        }
    }
}

/// MediaAudioFeed reads locators through an EditSession: one over the checked project, takes included.
class MixFeed final : public playback::AudioFeed {
public:
    explicit MixFeed(const ProjectState& state) : m_session(state), m_feed(m_session) {}
    std::expected<std::size_t, Error> read(MediaId media, int streamOrdinal, Ticks sourceTime, std::span<float> interleaved,
                                           int channels, Rate rate) override {
        return m_feed.read(media, streamOrdinal, sourceTime, interleaved, channels, rate);
    }
    std::expected<std::size_t, Error> readTake(const std::string& path, Ticks sourceTime, std::span<float> interleaved,
                                               int channels, Rate rate) override {
        return m_feed.readTake(path, sourceTime, interleaved, channels, rate);
    }

private:
    EditSession m_session;
    playback::MediaAudioFeed m_feed; // after m_session: it reads the session
};

/// Turns rendered samples into MixPeaks bins as they come.
class PeakBuilder {
public:
    PeakBuilder() { reserve(); }
    void add(std::span<const float> interleaved, int channels) {
        const std::size_t frames = interleaved.size() / static_cast<std::size_t>(channels);
        for (std::size_t f = 0; f < frames; ++f) {
            for (int c = 0; c < channels; ++c)
                m_peak = std::max(m_peak, std::abs(interleaved[f * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)]));
            if (++m_fill == MixPeaks::kBinFrames)
                push();
        }
    }
    /// What is built so far (`complete`: the last, partial bin too).
    std::shared_ptr<const MixPeaks> snapshot(bool complete) {
        if (complete && m_fill > 0)
            push();
        auto peaks = std::make_shared<MixPeaks>();
        peaks->chunks = m_done;
        if (!m_current.bins.empty())
            peaks->chunks.push_back(std::make_shared<const MixPeaks::Chunk>(m_current));
        peaks->binCount = m_bins;
        peaks->complete = complete;
        return peaks;
    }

private:
    void push() {
        const auto value = static_cast<std::uint16_t>(std::lround(std::min(1.0f, m_peak) * 32767.0f));
        m_current.bins.push_back(value);
        const std::size_t block = (m_current.bins.size() - 1) / MixPeaks::kBlockBins;
        if (m_current.blocks.size() <= block)
            m_current.blocks.push_back(value);
        else
            m_current.blocks[block] = std::max(m_current.blocks[block], value);
        ++m_bins;
        m_peak = 0;
        m_fill = 0;
        if (m_current.bins.size() == MixPeaks::kChunkBins) {
            m_done.push_back(std::make_shared<const MixPeaks::Chunk>(std::move(m_current)));
            m_current = {};
            reserve();
        }
    }
    void reserve() { // exactly a chunk: no growth slack kept in the cache
        m_current.bins.reserve(MixPeaks::kChunkBins);
        m_current.blocks.reserve(MixPeaks::kChunkBins / MixPeaks::kBlockBins);
    }

    std::vector<std::shared_ptr<const MixPeaks::Chunk>> m_done;
    MixPeaks::Chunk m_current;
    std::size_t m_bins = 0;
    float m_peak = 0;
    int m_fill = 0;
};

} // namespace

// ---- MixPeaks -------------------------------------------------------------------------------------------------------

Ticks MixPeaks::binTicks() {
    return framesToTicks(kBinFrames, Rate{kSampleRate, 1});
}

Ticks MixPeaks::reach() const {
    return Ticks{binTicks().value * static_cast<std::int64_t>(binCount)};
}

float MixPeaks::peak(Ticks from, Ticks to) const {
    if (to <= from || binCount == 0)
        return 0.0f;
    const std::int64_t width = binTicks().value;
    const auto count = static_cast<std::int64_t>(binCount);
    const std::int64_t b0 = std::clamp(floorDiv(from.value, width), std::int64_t{0}, count);
    std::int64_t b1 = std::clamp(floorDiv(to.value + width - 1, width), std::int64_t{0}, count);
    if (b1 <= b0) {
        if (b0 >= count)
            return 0.0f;
        b1 = b0 + 1; // narrower than a bin: the bin it is in
    }
    std::uint16_t best = 0;
    for (std::int64_t b = b0; b < b1;) {
        const auto chunkIndex = static_cast<std::size_t>(b) / kChunkBins;
        const auto offset = static_cast<std::size_t>(b) % kChunkBins;
        if (chunkIndex >= chunks.size() || !chunks[chunkIndex])
            break;
        const Chunk& chunk = *chunks[chunkIndex];
        if (offset >= chunk.bins.size())
            break;
        // Whole blocks from the coarse level.
        if (offset % kBlockBins == 0 && b + static_cast<std::int64_t>(kBlockBins) <= b1
            && offset + kBlockBins <= chunk.bins.size()) {
            best = std::max(best, chunk.blocks[offset / kBlockBins]);
            b += static_cast<std::int64_t>(kBlockBins);
            continue;
        }
        best = std::max(best, chunk.bins[offset]);
        ++b;
    }
    return static_cast<float>(best) / 32767.0f;
}

std::size_t MixPeaks::bytes() const {
    std::size_t total = sizeof(MixPeaks);
    for (const auto& chunk : chunks) {
        if (chunk)
            total += (chunk->bins.capacity() + chunk->blocks.capacity()) * sizeof(std::uint16_t);
    }
    return total;
}

std::uint64_t nestedMixStamp(const ProjectState& state, SequenceId sequence) {
    if (!state.findSequence(sequence))
        return 0;
    std::uint64_t key = mixHash(0x4D49'5800ull, sequence.value);
    std::unordered_set<std::uint64_t> seen;
    stampOf(state, sequence, 0, key, seen);
    // Audio redactions reach any clip of their media (I.6): their list is shared between revisions until it changes.
    key = mixHash(key, address(state.redactions.get().data()));
    key = mixHash(key, state.redactions.size());
    return key == 0 ? 1 : key;
}

// ---- NestedMixPeaks ---------------------------------------------------------------------------------------------------

NestedMixPeaks::NestedMixPeaks(QObject* parent) : QObject(parent) {}

NestedMixPeaks::~NestedMixPeaks() {
    if (m_thread.joinable()) { // a render stops after its current block
        m_thread.request_stop();
        m_wake.notify_all();
        m_thread.join();
    }
}

void NestedMixPeaks::setProjectFile(std::filesystem::path file) {
    std::lock_guard lock(m_mutex);
    m_projectFile = std::move(file);
}

void NestedMixPeaks::setRenderer(Renderer renderer) {
    std::lock_guard lock(m_mutex);
    m_renderer = std::move(renderer);
}

void NestedMixPeaks::setBudget(std::size_t bytes) {
    std::lock_guard lock(m_mutex);
    m_budget = bytes;
    evictOverBudget();
}

NestedMixPeaks::Stats NestedMixPeaks::stats() const {
    std::lock_guard lock(m_mutex);
    Stats out = m_stats;
    out.bytes = m_bytes;
    out.entries = m_entries.size();
    return out;
}

NestedMixSource::Lookup NestedMixPeaks::lookup(const Clip& clip, const ProjectSnapshot& project) const {
    if (!clip.isNested() || clip.stream.kind != StreamKind::Audio || !project)
        return {};
    std::lock_guard lock(m_mutex);
    const SequencePtr nested = project->sequencePtr(clip.sequence);
    if (!nested)
        return {};
    // Which mix: the sequence and the tracks the clip reads (a multicam clip: its angles').
    const std::vector<std::size_t> read = nestedTracksRead(*nested, clip);
    std::uint64_t key = mixHash(0x4E4D'0000ull, clip.sequence.value);
    for (const std::size_t t : read)
        key = mixHash(key, t + 1);
    // What it depends on, once per sequence and snapshot.
    if (m_memoProject != project) {
        m_memoProject = project;
        m_stampMemo.clear();
    }
    auto [memo, inserted] = m_stampMemo.try_emplace(clip.sequence.value, 0);
    if (inserted)
        memo->second = nestedMixStamp(*project, clip.sequence);
    if (memo->second == 0)
        return {};
    std::uint64_t stamp = memo->second;
    for (const std::size_t t : read)
        stamp = mixHash(stamp, t + 1);
    stamp = stamp == 0 ? 1 : stamp;

    auto [it, fresh] = m_entries.try_emplace(key);
    Entry& e = it->second;
    e.sequence = clip.sequence;
    e.lastUse = ++m_clock;
    if (!fresh && e.stamp == stamp && e.pendingStamp == 0 && (e.state == State::Ready || e.state == State::Failed))
        return {e.state, e.peaks};
    if (e.pendingStamp != stamp) {
        // New, or what it plays changed: render it (again). A render of the old content still running stops.
        if (m_running.key == key && m_running.stamp != stamp)
            m_running.stop.request_stop();
        e.pendingStamp = stamp;
        e.partial.reset();
        Job job;
        job.key = key;
        job.stamp = stamp;
        job.request.project = project;
        job.request.projectFile = m_projectFile;
        // The nested sequence's sound as NestedMix plays it: its audio tracks summed (no video, no routing of its own).
        auto prepared = std::make_shared<Sequence>(*nested);
        prepared->videoTracks.clear();
        prepared->submixTracks.clear();
        for (TrackPtr& track : prepared->audioTracks) {
            if (track && (track->output.isValid() || !track->sends.empty())) {
                auto direct = std::make_shared<Track>(*track);
                direct->output = {};
                direct->sends.clear();
                track = std::move(direct);
            }
        }
        if (nested->multicam) {
            // A multicam (or merged) clip: its angles' tracks whatever their flags; a multicam source has no master stage.
            std::vector<TrackId> tracks;
            for (const std::size_t t : read) {
                if (t < nested->audioTracks.size() && nested->audioTracks[t])
                    tracks.push_back(nested->audioTracks[t]->id);
            }
            job.request.tracks = std::move(tracks);
            job.request.honourMuteSolo = false;
            job.request.masterEffects = false;
        }
        job.request.length = std::max(nested->contentEnd(), Ticks{});
        job.request.sequence = std::move(prepared);
        enqueue(std::move(job));
    }
    return {State::Pending, e.partial};
}

void NestedMixPeaks::enqueue(Job job) const {
    // One job per mix: a newer revision replaces a queued one.
    const auto queued = std::ranges::find_if(m_jobs, [&](const Job& j) { return j.key == job.key; });
    if (queued != m_jobs.end())
        *queued = std::move(job);
    else
        m_jobs.push_back(std::move(job));
    if (!m_thread.joinable())
        m_thread = std::jthread([self = const_cast<NestedMixPeaks*>(this)](std::stop_token stop) { self->worker(std::move(stop)); });
    m_wake.notify_one();
}

void NestedMixPeaks::worker(std::stop_token stop) {
    for (;;) {
        Job job;
        Renderer renderer;
        std::stop_source jobStop;
        {
            std::unique_lock lock(m_mutex);
            if (!m_wake.wait(lock, stop, [&] { return !m_jobs.empty(); }))
                return;
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
            renderer = m_renderer ? m_renderer : Renderer(&NestedMixPeaks::renderWithMedia);
            m_running = Running{job.key, job.stamp, jobStop};
            ++m_stats.started;
        }
        // Stopping the service stops the render too.
        const std::stop_callback forward(stop, [&jobStop] { jobStop.request_stop(); });
        PeakBuilder builder;
        auto lastPublish = std::chrono::steady_clock::now();
        const Sink sink = [&](std::span<const float> interleaved, int channels) {
            if (channels <= 0)
                return false;
            builder.add(interleaved, channels);
            if (const auto now = std::chrono::steady_clock::now(); now - lastPublish >= kPublishInterval) {
                lastPublish = now;
                publish(job, builder.snapshot(false));
            }
            return !jobStop.stop_requested();
        };
        bool ok = false;
        try {
            ok = renderer(job.request, sink, jobStop.get_token());
        } catch (...) {
            ok = false; // a render that throws is a failed one: the approximation stays
        }
        if (stop.stop_requested())
            return;
        const bool cancelled = jobStop.stop_requested();
        finish(job, ok && !cancelled ? builder.snapshot(true) : nullptr, cancelled);
    }
}

void NestedMixPeaks::publish(const Job& job, std::shared_ptr<const MixPeaks> partial) {
    {
        std::lock_guard lock(m_mutex);
        const auto e = m_entries.find(job.key);
        if (e == m_entries.end() || e->second.pendingStamp != job.stamp)
            return;
        e->second.partial = std::move(partial);
        markChanged(e->second.sequence);
    }
    QMetaObject::invokeMethod(this, &NestedMixPeaks::mixesChanged, Qt::QueuedConnection);
}

void NestedMixPeaks::finish(const Job& job, std::shared_ptr<const MixPeaks> peaks, bool cancelled) {
    {
        std::lock_guard lock(m_mutex);
        m_running = {};
        if (cancelled) {
            ++m_stats.cancelled; // a newer render of this mix is queued
            return;
        }
        const auto e = m_entries.find(job.key);
        if (e == m_entries.end() || e->second.pendingStamp != job.stamp)
            return; // evicted, or asked for other content meanwhile
        Entry& entry = e->second;
        entry.pendingStamp = 0;
        entry.partial.reset();
        entry.stamp = job.stamp;
        if (entry.peaks)
            m_bytes -= std::min(m_bytes, entry.peaks->bytes());
        if (peaks) {
            entry.state = State::Ready;
            m_bytes += peaks->bytes();
            entry.peaks = std::move(peaks);
            ++m_stats.completed;
            evictOverBudget();
        } else {
            entry.state = State::Failed;
            entry.peaks.reset();
            ++m_stats.failed;
        }
        markChanged(entry.sequence);
    }
    QMetaObject::invokeMethod(this, &NestedMixPeaks::mixesChanged, Qt::QueuedConnection);
}

void NestedMixPeaks::markChanged(SequenceId sequence) const {
    m_changedAt[sequence.value] = ++m_serial;
}

std::vector<std::uint64_t> NestedMixPeaks::sequencesChangedSince(std::uint64_t serial, std::uint64_t& now) const {
    std::lock_guard lock(m_mutex);
    now = m_serial;
    std::vector<std::uint64_t> out;
    for (const auto& [sequence, at] : m_changedAt) {
        if (at > serial)
            out.push_back(sequence);
    }
    return out;
}

void NestedMixPeaks::evictOverBudget() {
    // Least recently used complete mixes first; the most recent one stays whatever its size.
    while (m_bytes > m_budget) {
        auto victim = m_entries.end();
        std::uint64_t newest = 0;
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            newest = std::max(newest, it->second.lastUse);
            if (it->second.pendingStamp == 0 && it->second.peaks
                && (victim == m_entries.end() || it->second.lastUse < victim->second.lastUse))
                victim = it;
        }
        if (victim == m_entries.end() || victim->second.lastUse == newest)
            return;
        m_bytes -= std::min(m_bytes, victim->second.peaks->bytes());
        m_entries.erase(victim); // rendered again when next needed
    }
}

bool NestedMixPeaks::renderWithMedia(const Request& request, const Sink& sink, std::stop_token stop) {
    if (!request.project || !request.sequence)
        return false;
    // Media checked on disk on this worker (relative paths, relinks), as the nested filmstrips are.
    const auto checked = std::make_shared<const ProjectState>(editor::withMediaChecked(*request.project, request.projectFile));
    playback::OfflineMixdownOptions options;
    options.tracks = request.tracks;
    options.honourMuteSolo = request.honourMuteSolo;
    options.masterEffects = request.masterEffects;
    options.redactions = playback::AudioRedactions::of(*checked);
    options.takes = playback::AudioTakes::of(*checked);
    options.project = checked; // nested clips inside find their sequences there
    MixFeed feed(*checked);
    constexpr int kChannels = 2;
    playback::OfflineMixdown mix(request.sequence, kChannels, MixPeaks::kSampleRate, Ticks{}, feed, options);
    const std::int64_t perFrame = Rate{MixPeaks::kSampleRate, 1}.ticksPerUnit();
    const std::int64_t frames = (request.length.value + perFrame - 1) / perFrame;
    constexpr std::int64_t kBlock = 4096;
    std::vector<float> block(static_cast<std::size_t>(kBlock) * kChannels);
    for (std::int64_t done = 0; done < frames; done += kBlock) {
        if (stop.stop_requested())
            return false;
        const std::int64_t n = std::min(kBlock, frames - done);
        const std::span<float> view(block.data(), static_cast<std::size_t>(n) * kChannels);
        mix.render(view);
        if (mix.feedErrors() > 0)
            return false; // media that cannot be read (offline, broken): no mix rather than a silent one
        if (!sink(view, kChannels))
            return false;
    }
    return true;
}

} // namespace wabcut::app
