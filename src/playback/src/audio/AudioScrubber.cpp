#include <wabcut/playback/audio/AudioScrubber.h>

#include <wabcut/playback/audio/AudioRedactions.h>
#include <wabcut/playback/audio/AudioTakes.h>
#include <wabcut/playback/audio/OfflineMixdown.h>

#include <wabcut/audio/AudioSource.h>
#include <wabcut/audio/RealTime.h>
#include <wabcut/audio/SpscQueue.h>
#include <wabcut/audio/TimeMath.h>

#include <wabcut/model/ProjectState.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace wabcut::playback {

namespace {

/// One window of the mix, interleaved at the engine's channel count, its fades already in it. Allocated and freed on
/// the worker; the audio thread only reads it.
struct ScrubWindow {
    std::vector<float> samples;
    int channels = 0;
    std::size_t frames() const noexcept {
        return channels > 0 ? samples.size() / static_cast<std::size_t>(channels) : 0;
    }
};

/// The engine's overlay. The worker hands windows over (`in`); the audio thread plays the newest, fades out the one it
/// replaces, and hands every window it is done with back (`out`) to be freed off the audio thread. A null hand-over is a
/// stop: what sounds fades out.
class ScrubOverlay final : public audio::AudioSource {
public:
    /// Windows that may wait for the audio thread; and more room to hand them back than can ever be out at once
    /// (the worker keeps at most kMaxOutstanding), so a hand-back never fails.
    static constexpr std::size_t kQueue = 16;
    static constexpr std::size_t kMaxOutstanding = 24;
    static constexpr std::size_t kReturn = 64;

    ScrubOverlay(int channels, int sampleRate, int fadeFrames)
        : m_channels(channels), m_rate{sampleRate, 1}, m_fadeFrames(std::max(fadeFrames, 1)) {}

    /// Frees every window still here (on the control thread that drops the last reference: the engine retires the
    /// overlay only once the audio thread is done with it).
    ~ScrubOverlay() override {
        delete m_current;
        delete m_fading;
        while (const auto* w = m_in.front()) {
            delete *w;
            m_in.pop();
        }
        while (const auto* w = m_out.front()) {
            delete *w;
            m_out.pop();
        }
    }

    // ---- Worker side (one thread at a time) ----
    bool hand(ScrubWindow* window) noexcept { return m_in.push(window); }
    /// The windows the audio thread is done with.
    template <typename Release>
    void reclaim(Release&& release) {
        while (const auto* w = m_out.front()) {
            ScrubWindow* window = *w;
            m_out.pop();
            release(window);
        }
    }

    // ---- Audio thread ----
    void render(std::span<float> out, int channels, Rate rate, Ticks) noexcept override {
        std::fill(out.begin(), out.end(), 0.0f);
        // The newest window handed over wins; the ones queued before it are skipped.
        bool arrived = false;
        ScrubWindow* next = nullptr;
        while (const auto* front = m_in.front()) {
            ScrubWindow* window = *front;
            m_in.pop();
            if (arrived && next != nullptr) {
                retire(next);
                bump(m_skipped, 1);
            }
            next = window;
            arrived = true;
        }
        if (arrived) {
            if (m_current != nullptr) {
                // Cut short: it fades out from where it is, under the new one.
                retire(m_fading);
                m_fading = m_current;
                m_fadePos = m_pos;
                m_fadeDone = 0;
                bump(m_cutShort, 1);
            }
            m_current = next;
            m_pos = 0;
            if (m_current != nullptr && m_current->channels != m_channels) {
                retire(m_current); // rendered for another format
                m_current = nullptr;
            }
            if (m_current != nullptr) {
                bump(m_started, 1);
            }
        }
        if (channels != m_channels || !(rate == m_rate)) {
            return; // the engine's format is not the one the windows were rendered in (it plays silence meanwhile)
        }
        const auto ch = static_cast<std::size_t>(channels);
        const std::size_t frames = out.size() / ch;
        if (m_fading != nullptr) {
            const std::size_t total = m_fading->frames();
            const float denominator = static_cast<float>(m_fadeFrames + 1);
            for (std::size_t i = 0; i < frames && m_fadeDone < m_fadeFrames && m_fadePos < total; ++i) {
                const float g = 1.0f - static_cast<float>(m_fadeDone + 1) / denominator;
                const float* s = m_fading->samples.data() + m_fadePos * ch;
                for (std::size_t c = 0; c < ch; ++c) {
                    out[i * ch + c] += s[c] * g;
                }
                ++m_fadePos;
                ++m_fadeDone;
            }
            if (m_fadeDone >= m_fadeFrames || m_fadePos >= total) {
                retire(m_fading);
                m_fading = nullptr;
            }
        }
        if (m_current != nullptr) {
            const std::size_t total = m_current->frames();
            const std::size_t n = std::min(frames, total - std::min(total, m_pos));
            const float* s = m_current->samples.data() + m_pos * ch;
            for (std::size_t k = 0; k < n * ch; ++k) {
                out[k] += s[k];
            }
            m_pos += n;
            bump(m_framesPlayed, n);
            if (m_pos >= total) {
                retire(m_current);
                m_current = nullptr;
            }
        }
    }

    // Statistics (single writer: the audio thread).
    std::uint64_t started() const noexcept { return m_started.load(std::memory_order_relaxed); }
    std::uint64_t cutShort() const noexcept { return m_cutShort.load(std::memory_order_relaxed); }
    std::uint64_t skipped() const noexcept { return m_skipped.load(std::memory_order_relaxed); }
    std::uint64_t framesPlayed() const noexcept { return m_framesPlayed.load(std::memory_order_relaxed); }

private:
    void retire(ScrubWindow* window) noexcept {
        if (window != nullptr) {
            (void)m_out.push(window); // never full (kReturn > kMaxOutstanding)
        }
    }
    static void bump(std::atomic<std::uint64_t>& counter, std::size_t by) noexcept {
        counter.store(counter.load(std::memory_order_relaxed) + by, std::memory_order_relaxed);
    }

    const int m_channels;
    const Rate m_rate;
    const int m_fadeFrames;
    audio::SpscQueue<ScrubWindow*> m_in{kQueue};
    audio::SpscQueue<ScrubWindow*> m_out{kReturn};
    // Audio-thread state.
    ScrubWindow* m_current = nullptr;
    std::size_t m_pos = 0;
    ScrubWindow* m_fading = nullptr;
    std::size_t m_fadePos = 0;
    int m_fadeDone = 0;
    std::atomic<std::uint64_t> m_started{0};
    std::atomic<std::uint64_t> m_cutShort{0};
    std::atomic<std::uint64_t> m_skipped{0};
    std::atomic<std::uint64_t> m_framesPlayed{0};
};

/// Fades `window` in and out over `fadeFrames` (linear; at most half the window each).
void applyFades(ScrubWindow& window, int fadeFrames) {
    const std::size_t frames = window.frames();
    const auto ch = static_cast<std::size_t>(window.channels);
    const std::size_t fade = std::min(static_cast<std::size_t>(std::max(fadeFrames, 0)), frames / 2);
    const auto denominator = static_cast<float>(fade + 1);
    for (std::size_t i = 0; i < fade; ++i) {
        const float g = static_cast<float>(i + 1) / denominator;
        for (std::size_t c = 0; c < ch; ++c) {
            window.samples[i * ch + c] *= g;
            window.samples[(frames - 1 - i) * ch + c] *= g;
        }
    }
}

/// The window's frames in reverse order (each frame's channels stay as they are).
void reverseFrames(ScrubWindow& window) {
    const std::size_t frames = window.frames();
    const auto ch = static_cast<std::size_t>(window.channels);
    for (std::size_t i = 0; i < frames / 2; ++i) {
        const std::size_t j = frames - 1 - i;
        std::swap_ranges(window.samples.begin() + static_cast<std::ptrdiff_t>(i * ch),
                         window.samples.begin() + static_cast<std::ptrdiff_t>((i + 1) * ch),
                         window.samples.begin() + static_cast<std::ptrdiff_t>(j * ch));
    }
}

std::int64_t ticksToFramesCeil(Ticks t, std::int64_t ticksPerFrame) {
    return t.value <= 0 ? 0 : audio::timemath::mulDivSaturate(t.value, 1, ticksPerFrame, Rounding::Ceil);
}

} // namespace

struct AudioScrubber::Impl {
    Impl(audio::AudioEngine& e, std::shared_ptr<AudioFeed> f, AudioScrubOptions o)
        : engine(e), feed(std::move(f)), options(o) {}

    audio::AudioEngine& engine;
    std::shared_ptr<AudioFeed> feed;
    const AudioScrubOptions options;

    // ---- Requests (requestsLock; never taken on the audio thread) ----
    mutable audio::rt::CheckedMutex requestsLock;
    std::condition_variable_any wake;
    const EditSession* session = nullptr;
    SequenceId sequence;
    /// The request waiting: where, and whether it plays backwards (a backward drag).
    struct Request {
        Ticks at;
        bool reversed = false;
    };
    std::optional<Request> pending;
    std::chrono::steady_clock::time_point pendingSince;
    bool stopPending = false;
    /// Counts cancels (cancel(), another sequence or session): a window rendered across one is not handed over.
    std::uint64_t cancels = 0;
    bool quit = false;
    std::optional<std::chrono::steady_clock::time_point> lastHandOver;
    std::atomic<bool> started{false};
    std::thread worker;

    // ---- Worker state (workLock: the worker or process(), one at a time) ----
    mutable audio::rt::CheckedMutex workLock;
    std::shared_ptr<ScrubOverlay> overlay;
    int channels = 2;
    int sampleRate = 48000;
    std::int64_t tpf = Rate{48000, 1}.ticksPerUnit();
    ProjectSnapshot mixSnapshot;
    SequenceId mixSequence;
    std::unique_ptr<OfflineMixdown> mixdown;
    std::size_t outstanding = 0;

    // ---- Statistics (any thread) ----
    std::atomic<std::uint64_t> requests{0};
    std::atomic<std::uint64_t> replaced{0};
    std::atomic<std::uint64_t> rendered{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> workerPasses{0};
    std::atomic<std::int64_t> lastWindowFrames{0};
    std::atomic<std::int64_t> lastWindowStart{0};
    std::atomic<bool> lastWindowReversed{false};
    std::atomic<std::int64_t> lastLatencyMicros{0};

    Ticks windowLength(Rate frameRate) const noexcept {
        Ticks frame{kTicksPerSecond / 30};
        if (frameRate.isValid()) {
            frame = frameRate.isExact() ? Ticks{frameRate.ticksPerUnit()}
                                        : Ticks{audio::timemath::mulDivSaturate(kTicksPerSecond, frameRate.den, frameRate.num,
                                                                                Rounding::Ceil)};
        }
        const Ticks longest = std::max(options.maxWindow, options.minWindow);
        return std::min(std::max(frame, options.minWindow), longest);
    }

    /// The window of `sequence`'s mix at `at` (workLock held), or null when there is no such sequence: from `at` on, or
    /// `reversed`, the window that ends at `at` played backwards (it starts at `at` and runs back in time). `start` is
    /// set to where the window starts in the sequence.
    std::unique_ptr<ScrubWindow> renderWindow(const EditSession& from, SequenceId id, Ticks at, bool reversed, Ticks& start) {
        const ProjectSnapshot snapshot = from.snapshot();
        const SequencePtr seq = snapshot ? snapshot->sequencePtr(id) : nullptr;
        if (!seq) {
            return nullptr;
        }
        const std::int64_t frames = std::max<std::int64_t>(ticksToFramesCeil(windowLength(seq->frameRate), tpf), 1);
        start = reversed ? Ticks{std::max<std::int64_t>(at.value - frames * tpf, 0)} : at;
        if (!mixdown || snapshot != mixSnapshot || id != mixSequence) {
            // Another revision: a mixdown of it (the next windows of the same revision only seek it).
            mixdown.reset();
            OfflineMixdownOptions mixOptions;
            mixOptions.renderer.maxPreroll = options.effectPreroll;
            mixOptions.renderer.maxClipPreroll = options.clipPreroll;
            mixOptions.redactions = AudioRedactions::of(*snapshot);
            mixOptions.takes = AudioTakes::of(*snapshot);
            mixOptions.project = snapshot;
            mixdown = std::make_unique<OfflineMixdown>(seq, channels, sampleRate, start, *feed, std::move(mixOptions));
            mixSnapshot = snapshot;
            mixSequence = id;
        } else {
            mixdown->seek(start);
        }
        auto window = std::make_unique<ScrubWindow>();
        window->channels = channels;
        window->samples.assign(static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels), 0.0f);
        mixdown->render(window->samples);
        if (reversed) {
            reverseFrames(*window); // tape-like: the sound runs back from the playhead
        }
        applyFades(*window, static_cast<int>(ticksToFramesCeil(options.fade, tpf)));
        return window;
    }

    bool process(std::chrono::steady_clock::time_point now) {
        std::lock_guard work(workLock);
        if (!overlay) {
            return false;
        }
        overlay->reclaim([this](ScrubWindow* window) {
            delete window;
            outstanding -= std::min<std::size_t>(outstanding, 1);
        });
        std::optional<Request> at;
        std::chrono::steady_clock::time_point since;
        bool stop = false;
        const EditSession* from = nullptr;
        SequenceId id;
        std::uint64_t cancelled = 0;
        {
            std::lock_guard lock(requestsLock);
            stop = std::exchange(stopPending, false);
            cancelled = cancels;
            if (pending && (!lastHandOver || now >= *lastHandOver + options.minInterval)) {
                at = pending;
                since = pendingSince;
                pending.reset();
            }
            from = session;
            id = sequence;
        }
        if (stop) {
            (void)overlay->hand(nullptr); // fades out what sounds (a full queue: newer windows replace it anyway)
        }
        if (!at) {
            return false;
        }
        // Scrubbing is for a stopped transport: playing, the program is heard (and a window would sound over it).
        if (!engine.isPaused() || from == nullptr || !id.isValid() || outstanding >= ScrubOverlay::kMaxOutstanding) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        Ticks start;
        std::unique_ptr<ScrubWindow> window = renderWindow(*from, id, at->at, at->reversed, start);
        if (!window) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const auto frames = static_cast<std::int64_t>(window->frames());
        {
            // Cancelled while it rendered (playback started, another sequence): not heard.
            std::lock_guard lock(requestsLock);
            if (cancels != cancelled) {
                dropped.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
        if (!overlay->hand(window.get())) {
            dropped.fetch_add(1, std::memory_order_relaxed); // nothing plays the overlay (a silent engine): freed here
            return false;
        }
        (void)window.release(); // the overlay's now (handed back to be freed)
        ++outstanding;
        {
            std::lock_guard lock(requestsLock);
            lastHandOver = now;
        }
        rendered.fetch_add(1, std::memory_order_relaxed);
        lastWindowFrames.store(frames, std::memory_order_relaxed);
        lastWindowStart.store(start.value, std::memory_order_relaxed);
        lastWindowReversed.store(at->reversed, std::memory_order_relaxed);
        lastLatencyMicros.store(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - since).count(),
                                std::memory_order_relaxed);
        return true;
    }

    void workerMain() {
        const audio::rt::ScopedDenormalsOff denormals; // the renderers' filters ring down in silence
        for (;;) {
            {
                std::unique_lock lock(requestsLock);
                wake.wait(lock, [this] { return quit || stopPending || pending.has_value(); });
                if (quit) {
                    return;
                }
                // The rate limit: the next window waits for its turn; a newer request meanwhile replaces this one.
                if (pending && !stopPending && lastHandOver) {
                    const auto due = *lastHandOver + options.minInterval;
                    wake.wait_until(lock, due, [this] { return quit || stopPending; });
                    if (quit) {
                        return;
                    }
                }
            }
            workerPasses.fetch_add(1, std::memory_order_relaxed);
            process(std::chrono::steady_clock::now());
        }
    }
};

AudioScrubber::AudioScrubber(audio::AudioEngine& engine, std::shared_ptr<AudioFeed> feed, AudioScrubOptions options)
    : m_impl(std::make_unique<Impl>(engine, std::move(feed), options)) {}

AudioScrubber::~AudioScrubber() { stop(); }

void AudioScrubber::setSession(const EditSession* session) {
    Impl& d = *m_impl;
    std::lock_guard lock(d.requestsLock);
    if (d.session == session) {
        return;
    }
    d.session = session;
    d.pending.reset();
    d.stopPending = true;
    ++d.cancels;
    d.wake.notify_all();
}

void AudioScrubber::setSequence(SequenceId sequence) {
    Impl& d = *m_impl;
    std::lock_guard lock(d.requestsLock);
    if (d.sequence == sequence) {
        return;
    }
    d.sequence = sequence;
    d.pending.reset();
    d.stopPending = true;
    ++d.cancels;
    d.wake.notify_all();
}

std::expected<void, Error> AudioScrubber::start() {
    Impl& d = *m_impl;
    if (d.started.load(std::memory_order_acquire)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "audio scrubber already started"});
    }
    if (!d.engine.isRunning() || !d.feed) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "audio scrubbing needs a running engine and a feed"});
    }
    const audio::AudioDeviceConfig config = d.engine.config();
    const Rate rate{config.sampleRate, 1};
    if (!rate.isExact() || config.channels < 1) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "unsupported engine format for audio scrubbing"});
    }
    {
        std::lock_guard work(d.workLock);
        d.channels = config.channels;
        d.sampleRate = config.sampleRate;
        d.tpf = rate.ticksPerUnit();
        d.mixdown.reset();
        d.mixSnapshot.reset();
        d.outstanding = 0;
        d.overlay = std::make_shared<ScrubOverlay>(d.channels, d.sampleRate,
                                                   static_cast<int>(ticksToFramesCeil(d.options.fade, d.tpf)));
    }
    {
        std::lock_guard lock(d.requestsLock);
        d.pending.reset();
        d.stopPending = false;
        d.quit = false;
        d.lastHandOver.reset();
    }
    d.engine.setOverlay(d.overlay);
    d.started.store(true, std::memory_order_release);
    if (d.options.workerThread) {
        d.worker = std::thread([&d] { d.workerMain(); });
    }
    return {};
}

void AudioScrubber::stop() {
    Impl& d = *m_impl;
    if (!d.started.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    {
        std::lock_guard lock(d.requestsLock);
        d.quit = true;
        d.pending.reset();
        d.wake.notify_all();
    }
    if (d.worker.joinable()) {
        d.worker.join();
    }
    std::lock_guard work(d.workLock);
    if (d.engine.overlay() == d.overlay) {
        d.engine.setOverlay(nullptr); // the engine frees it (and the windows it still holds) off the audio thread
    }
    d.overlay.reset();
    d.mixdown.reset();
    d.mixSnapshot.reset();
    d.outstanding = 0;
}

bool AudioScrubber::isStarted() const noexcept { return m_impl->started.load(std::memory_order_acquire); }

void AudioScrubber::scrub(Ticks at, int direction) {
    Impl& d = *m_impl;
    if (!d.started.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard lock(d.requestsLock);
    if (d.pending) {
        d.replaced.fetch_add(1, std::memory_order_relaxed); // not heard: the playhead is elsewhere now
    } else {
        d.pendingSince = std::chrono::steady_clock::now();
    }
    d.pending = Impl::Request{Ticks{std::max<std::int64_t>(at.value, 0)}, direction < 0};
    d.requests.fetch_add(1, std::memory_order_relaxed);
    d.wake.notify_all();
}

void AudioScrubber::cancel() {
    Impl& d = *m_impl;
    std::lock_guard lock(d.requestsLock);
    d.pending.reset();
    d.stopPending = true;
    ++d.cancels;
    d.wake.notify_all();
}

bool AudioScrubber::process(std::chrono::steady_clock::time_point now) { return m_impl->process(now); }

Ticks AudioScrubber::windowLength(Rate frameRate) const noexcept { return m_impl->windowLength(frameRate); }

AudioScrubStats AudioScrubber::stats() const noexcept {
    const Impl& d = *m_impl;
    AudioScrubStats s;
    s.requests = d.requests.load(std::memory_order_relaxed);
    s.replaced = d.replaced.load(std::memory_order_relaxed);
    s.rendered = d.rendered.load(std::memory_order_relaxed);
    s.dropped = d.dropped.load(std::memory_order_relaxed);
    s.workerPasses = d.workerPasses.load(std::memory_order_relaxed);
    s.lastWindowFrames = d.lastWindowFrames.load(std::memory_order_relaxed);
    s.lastWindowStart = Ticks{d.lastWindowStart.load(std::memory_order_relaxed)};
    s.lastWindowReversed = d.lastWindowReversed.load(std::memory_order_relaxed);
    s.lastLatencyMicros = d.lastLatencyMicros.load(std::memory_order_relaxed);
    // The overlay's counters: the one installed now (reading its pointer races stop() otherwise).
    if (const std::shared_ptr<audio::AudioSource> source = d.engine.overlay()) {
        if (const auto* overlay = dynamic_cast<const ScrubOverlay*>(source.get())) {
            s.started = overlay->started();
            s.cutShort = overlay->cutShort();
            s.skipped = overlay->skipped();
            s.framesPlayed = overlay->framesPlayed();
        }
    }
    return s;
}

} // namespace wabcut::playback
