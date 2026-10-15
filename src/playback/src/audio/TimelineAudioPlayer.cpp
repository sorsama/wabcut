#include <wabcut/playback/audio/TimelineAudioPlayer.h>

#include "Fader.h"
#include "LiveFader.h"
#include "MasterBus.h"
#include "TrackStream.h"

#include <wabcut/audio/Gain.h>
#include <wabcut/audio/LoudnessTap.h>
#include <wabcut/audio/Mixer.h>
#include <wabcut/audio/RealTime.h>
#include <wabcut/audio/TimeMath.h>
#include <wabcut/audio/dsp/TimeStretch.h>
#include <wabcut/playback/audio/ChannelLayout.h>
#include <wabcut/playback/audio/TrackRenderer.h>

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/Nesting.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <limits>
#include <thread>
#include <vector>

namespace wabcut::playback {

using audio::timemath::mulDivSaturate;
using detail::StreamCounters;
using detail::TrackStream;

namespace {

std::int64_t framesOf(Ticks t, std::int64_t tpf, Rounding rounding) {
    return t.value <= 0 ? 0 : mulDivSaturate(t.value, 1, tpf, rounding);
}

// =========================================================================================================
// The audio-callback side: one TrackStream per track slot, mixed through an audio::Mixer and the master bus.
//
// The master bus (the sequence's master audio effects) delays the mix by its latency L, so the tracks are read L
// frames ahead of the time being rendered: what leaves render(start) is the mix for `start`. At a discontinuity the
// master chain restarts and is primed with the L frames from the new position. At audible shuttle speeds the mix is
// read at the timeline rate (speed x frames per block) and time-stretched to the device rate (preview only).
// =========================================================================================================
class TimelineMixSource final : public audio::AudioSource {
public:
    /// `channels`: the mix (the master's format: 6 for 5.1); `deviceChannels`: what render() is asked for, converted
    /// from the mix when they differ (A.9: a 5.1 master previewed on a stereo device is downmixed, ITU-R BS.775).
    TimelineMixSource(int slots, int channels, int deviceChannels, int sampleRate, std::size_t ringFrames, std::size_t maxChunks,
                      int fadeFrames, int maxBlockFrames, std::int64_t maxWaitFrames, bool tapThread)
        : m_tap(channels, sampleRate, audio::LoudnessTapOptions{tapThread}), m_channels(channels),
          m_deviceChannels(deviceChannels), m_rate{sampleRate, 1},
          m_ticksPerFrame(m_rate.ticksPerUnit()),
          m_maxBlockFrames(maxBlockFrames), m_maxWaitFrames(maxWaitFrames),
          m_mixer(slots, channels, sampleRate, maxBlockFrames),
          m_scratch(static_cast<std::size_t>(maxBlockFrames) * static_cast<std::size_t>(channels)),
          m_prime(static_cast<std::size_t>(maxBlockFrames) * static_cast<std::size_t>(channels)), m_master(channels, sampleRate),
          m_stretch(channels, sampleRate, audio::kMaxShuttleSpeed, kShuttleInputFrames),
          m_stretchIn(static_cast<std::size_t>(kShuttleInputFrames) * static_cast<std::size_t>(channels)),
          m_faders(std::make_unique<std::atomic<const FaderProgram*>[]>(static_cast<std::size_t>(slots))),
          m_live(std::make_unique<detail::LiveFader[]>(static_cast<std::size_t>(slots))),
          m_slotTrack(std::make_unique<std::atomic<std::uint64_t>[]>(static_cast<std::size_t>(slots))),
          m_rampFrames(std::max(sampleRate / 100, 1)) {
        if (m_deviceChannels != m_channels) {
            m_toDevice = formatMatrix(formatForChannels(m_channels), m_channels, formatForChannels(m_deviceChannels), m_deviceChannels);
            m_deviceMix.assign(static_cast<std::size_t>(maxBlockFrames) * static_cast<std::size_t>(channels), 0.0f);
            // Review A5-P2-2: the soft clip guard runs on what the device gets, after the conversion (toDevice()).
            m_guardOnDevice = m_mixer.softClipEnabled();
            m_mixer.setSoftClipEnabled(false);
        }
        m_streams.reserve(static_cast<std::size_t>(slots));
        for (int i = 0; i < slots; ++i) {
            m_streams.push_back(std::make_unique<TrackStream>(ringFrames, channels, maxChunks, fadeFrames));
            m_faders[static_cast<std::size_t>(i)].store(nullptr);
            m_slotTrack[static_cast<std::size_t>(i)].store(0);
        }
        m_master.setLive(&m_masterLive);
    }

    /// The loop the transport wraps at, as the audio thread reads it (end <= start: none).
    struct LoopRange {
        std::int64_t start = 0;
        std::int64_t end = 0;
        bool active() const noexcept { return end > start; }
    };

    /// A track's fader and panner as the audio thread applies them (published by the feeder).
    struct FaderProgram {
        ParamValue volume;
        ParamValue pan;
    };

    // ---- Audio thread ------------------------------------------------------------------------------------
    void render(std::span<float> out, int channels, Rate rate, Ticks start) noexcept override {
        if ((channels != m_channels && channels != m_deviceChannels) || !(rate == m_rate)) {
            std::fill(out.begin(), out.end(), 0.0f);
            return;
        }
        if (channels == m_channels) {
            renderMix(out, start);
            return;
        }
        // A.9: the mix in its own format, then to the device's.
        const auto dc = static_cast<std::size_t>(channels);
        const auto mc = static_cast<std::size_t>(m_channels);
        const std::size_t frames = out.size() / dc;
        for (std::size_t done = 0; done < frames;) {
            const std::size_t n = std::min(frames - done, static_cast<std::size_t>(m_maxBlockFrames));
            const std::span<float> mix(m_deviceMix.data(), n * mc);
            renderMix(mix, start + Ticks{static_cast<std::int64_t>(done) * m_ticksPerFrame});
            toDevice(mix.data(), out.data() + done * dc, n);
            done += n;
        }
    }

    bool renderAtSpeed(std::span<float> out, int channels, Rate rate, Ticks start,
                       audio::TransportSpeed speed) noexcept override {
        if (channels == m_channels || channels != m_deviceChannels) {
            return renderMixAtSpeed(out, channels, rate, start, speed);
        }
        const auto dc = static_cast<std::size_t>(channels);
        const auto mc = static_cast<std::size_t>(m_channels);
        const std::size_t frames = out.size() / dc;
        if (frames > static_cast<std::size_t>(m_maxBlockFrames)) {
            return false;
        }
        const std::span<float> mix(m_deviceMix.data(), frames * mc);
        if (!renderMixAtSpeed(mix, m_channels, rate, start, speed)) {
            return false;
        }
        toDevice(mix.data(), out.data(), frames);
        return true;
    }

private:
    /// The mix converted to the device's channels. Review A5-P2-2: a 5.1 downmix sums up to 2.41 times the mix, so the
    /// master's soft clip guard runs here, on what the device gets, instead of on the mix (as OfflineMixdown does).
    void toDevice(const float* mix, float* out, std::size_t frames) noexcept {
        m_toDevice.apply(mix, out, frames);
        if (m_guardOnDevice) {
            const std::size_t samples = frames * static_cast<std::size_t>(m_deviceChannels);
            for (std::size_t s = 0; s < samples; ++s) {
                out[s] = audio::softClip(out[s]);
            }
        }
    }

    /// render() at the mix's channel count.
    void renderMix(std::span<float> out, Ticks start) noexcept {
        const int channels = m_channels;
        const auto ch = static_cast<std::size_t>(channels);
        const std::size_t frames = out.size() / ch;
        m_faderSeq.fetch_add(1, std::memory_order_seq_cst); // odd: rendering (the fader programs in use)
        m_master.beginRender();
        m_stretching = false; // a plain render ends any time-stretched run
        const LoopRange loop = loopRange();
        (void)startMaster(start, loop);
        Resync resync;
        mixAhead(out.first(frames * ch), start, loop, resync);
        m_masterNext = start + Ticks{static_cast<std::int64_t>(frames) * m_ticksPerFrame};
        m_master.endRender();
        m_faderSeq.fetch_add(1, std::memory_order_seq_cst); // even: done
        m_tap.push(out); // A.3: the live loudness of what leaves the player (not of shuttle audition)
        finish(resync, m_readNext.value);
    }

    bool renderMixAtSpeed(std::span<float> out, int channels, Rate rate, Ticks start, audio::TransportSpeed speed) noexcept {
        const audio::TransportSpeed s = speed.normalized();
        if (channels != m_channels || !(rate == m_rate) || !audio::isAudibleShuttle(s)) {
            return false;
        }
        const auto ch = static_cast<std::size_t>(channels);
        const auto frames = static_cast<std::int64_t>(out.size() / ch);
        // Timeline this call covers: [start, end).
        const Ticks end = start + Ticks{mulDivSaturate(frames * m_ticksPerFrame, s.num, s.den, Rounding::Floor)};
        if (!m_stretching || start != m_stretchNext) {
            m_stretch.reset(); // a new run: it fades in over its first hop (10 ms)
            m_stretching = true;
            m_stretchCursor = start;
        }
        m_stretch.setSpeed(static_cast<double>(s.num) / static_cast<double>(s.den));
        m_faderSeq.fetch_add(1, std::memory_order_seq_cst);
        m_master.beginRender();
        const std::int64_t latency = startMaster(m_stretchCursor, LoopRange{});
        Resync resync;
        // Read the timeline up to `end` (whole frames from the run's start), mix it and feed the stretcher.
        std::int64_t input = std::max<std::int64_t>((end - m_stretchCursor).value / m_ticksPerFrame, 0);
        input = std::min<std::int64_t>(input, kShuttleInputFrames);
        for (std::int64_t done = 0; done < input;) {
            const std::int64_t n = std::min<std::int64_t>(input - done, m_maxBlockFrames);
            const Ticks at = m_stretchCursor + Ticks{(done + latency) * m_ticksPerFrame};
            const std::span<float> block(m_stretchIn.data(), static_cast<std::size_t>(n) * ch);
            mixAt(at, block, resync);
            m_stretch.push(block.data(), static_cast<int>(n));
            done += n;
        }
        m_stretchCursor += Ticks{input * m_ticksPerFrame};
        m_masterNext = m_stretchCursor;
        m_readNext = m_stretchCursor + Ticks{latency * m_ticksPerFrame};
        m_stretchNext = end;
        const int got = m_stretch.pull(out.data(), static_cast<int>(frames));
        std::fill(out.begin() + static_cast<std::ptrdiff_t>(got) * static_cast<std::ptrdiff_t>(ch), out.end(), 0.0f);
        m_master.endRender();
        m_faderSeq.fetch_add(1, std::memory_order_seq_cst);
        finish(resync, m_stretchCursor.value + latency * m_ticksPerFrame);
        return true;
    }

public:
    // ---- Feeder side ---------------------------------------------------------------------------------------
    int slots() const noexcept { return static_cast<int>(m_streams.size()); }
    int channels() const noexcept { return m_channels; }
    Rate rate() const noexcept { return m_rate; }
    TrackStream& stream(int slot) noexcept { return *m_streams[static_cast<std::size_t>(slot)]; }
    audio::Mixer& mixer() noexcept { return m_mixer; }
    const audio::Mixer& mixer() const noexcept { return m_mixer; }
    detail::MasterBus& master() noexcept { return m_master; }
    audio::LoudnessTap& tap() noexcept { return m_tap; }
    const audio::LoudnessTap& tap() const noexcept { return m_tap; }
    void setActive(int count) noexcept { m_active.store(count, std::memory_order_release); }
    /// Review A5-P2-7: the loop the transport wraps at (nullopt: none), so the master's read-ahead wraps with it. One
    /// writer at a time (the player holds its requests lock); the audio thread reads it without waiting.
    void setLoop(const std::optional<TimeRange>& loop) noexcept {
        const bool on = loop && !loop->isEmpty();
        m_loopSeq.fetch_add(1, std::memory_order_acq_rel); // odd: being written
        m_loopStart.store(on ? loop->start.value : 0, std::memory_order_relaxed);
        m_loopEnd.store(on ? loop->end().value : 0, std::memory_order_relaxed);
        m_loopSeq.fetch_add(1, std::memory_order_release); // even: done
    }
    /// Publishes slot `slot`'s fader (null: unity). The previous one may be in use until faderSeq() moves past
    /// the value read after this call (see MasterBus::update).
    void setFader(int slot, const FaderProgram* fader) noexcept {
        m_faders[static_cast<std::size_t>(slot)].store(fader, std::memory_order_seq_cst);
    }
    std::uint64_t faderSeq() const noexcept { return m_faderSeq.load(std::memory_order_seq_cst); }
    /// The live fader overrides (any control thread) and which track each slot plays (published by the feeder).
    detail::LiveFader& live(int slot) noexcept { return m_live[static_cast<std::size_t>(slot)]; }
    detail::LiveFader& masterLive() noexcept { return m_masterLive; }
    void setSlotTrack(int slot, TrackId track) noexcept {
        m_slotTrack[static_cast<std::size_t>(slot)].store(track.value, std::memory_order_release);
    }
    /// The slot playing `track`, or -1.
    int slotOf(TrackId track) const noexcept {
        for (int i = 0; i < slots(); ++i) {
            if (track.isValid() && m_slotTrack[static_cast<std::size_t>(i)].load(std::memory_order_acquire) == track.value) {
                return i;
            }
        }
        return -1;
    }

    std::uint64_t renders() const noexcept { return m_renders.load(std::memory_order_acquire); }
    std::int64_t consumerTime() const noexcept { return m_consumerTime.load(std::memory_order_relaxed); }
    std::uint64_t resyncSeq() const noexcept { return m_resyncSeq.load(std::memory_order_acquire); }
    /// P.1: the feeder has refilled every track from the target of seek `generation`: the engine may end its preroll.
    void setReady(std::uint64_t generation) noexcept { m_readyGeneration.store(generation, std::memory_order_release); }
    bool readyFor(std::uint64_t generation) const noexcept override {
        return generation != 0 && m_readyGeneration.load(std::memory_order_acquire) >= generation;
    }
    std::int64_t resyncTime() const noexcept { return m_resyncTime.load(std::memory_order_relaxed); }
    std::uint64_t underrunFrames() const noexcept { return m_underruns.load(std::memory_order_relaxed); }
    std::uint64_t skippedFrames() const noexcept { return m_skipped.load(std::memory_order_relaxed); }

private:
    /// Most timeline frames one renderAtSpeed() call reads (a 1024-frame device block at 4x, plus slack).
    static constexpr int kShuttleInputFrames = 8192;

    struct Resync {
        bool needed = false;
        std::int64_t at = 0;
    };

    /// The loop as last published (a write under way: the previous render's).
    LoopRange loopRange() noexcept {
        for (int attempt = 0; attempt < 3; ++attempt) {
            const std::uint64_t before = m_loopSeq.load(std::memory_order_acquire);
            if ((before & 1) != 0) {
                continue;
            }
            const LoopRange loop{m_loopStart.load(std::memory_order_relaxed), m_loopEnd.load(std::memory_order_relaxed)};
            std::atomic_thread_fence(std::memory_order_acquire);
            if (m_loopSeq.load(std::memory_order_relaxed) == before) {
                m_lastLoop = loop;
                break;
            }
        }
        return m_lastLoop;
    }

    /// Where the tracks are read for a render starting at `start`: the master's latency ahead, wrapped at the loop's
    /// end when the transport wraps there (it plays inside the loop or before it).
    Ticks readPosition(Ticks start, std::int64_t latency, const LoopRange& loop) const noexcept {
        std::int64_t t = start.value + latency * m_ticksPerFrame;
        if (loop.active() && start.value < loop.end && t >= loop.end) {
            t = loop.start + (t - loop.end) % (loop.end - loop.start);
        }
        return Ticks{t};
    }

    /// Restarts the master chain if `start` does not continue the stream (or its latency changed), priming it with
    /// the latency's worth of mix from `start`. Returns the latency in frames. Review A5-P2-7: a loop wrap continues
    /// the stream (the read-ahead wrapped with it, mixAhead), so the master plays on with its state instead of
    /// restarting (and a plug-in there replaying its pipeline's priming).
    std::int64_t startMaster(Ticks start, const LoopRange& loop) noexcept {
        const int latency = m_master.latencyFrames();
        const bool wrapped = loop.active() && start.value == loop.start && m_masterNext.value == loop.end;
        if (m_masterStarted && (start == m_masterNext || wrapped) && latency == m_masterLatency &&
            m_readNext == readPosition(start, latency, loop)) {
            return latency;
        }
        m_master.restart(start, m_ticksPerFrame);
        m_masterStarted = true;
        m_masterLatency = latency;
        m_readNext = start;
        Resync ignored;
        const auto ch = static_cast<std::size_t>(m_channels);
        for (std::int64_t done = 0; done < latency;) {
            const std::int64_t n = std::min<std::int64_t>(latency - done, m_maxBlockFrames);
            mixAhead(std::span<float>(m_prime.data(), static_cast<std::size_t>(n) * ch), start, loop, ignored);
            done += n;
        }
        return latency;
    }

    /// Mixes `out` from the read position m_readNext on, in blocks of at most maxBlockFrames. While the transport
    /// wraps at the loop's end (`start`, the render's transport time, is before it), so do the reads: a read never
    /// straddles the end, the next one starts at the loop's start, and the master's parameters follow the loop's time.
    void mixAhead(std::span<float> out, Ticks start, const LoopRange& loop, Resync& resync) noexcept {
        const auto ch = static_cast<std::size_t>(m_channels);
        const std::size_t frames = out.size() / ch;
        const bool wraps = loop.active() && start.value < loop.end;
        const auto wrapRead = [&] {
            if (wraps && m_readNext.value >= loop.end) {
                m_readNext = Ticks{loop.start + (m_readNext.value - loop.end) % (loop.end - loop.start)};
                m_master.continueAt(m_readNext);
            }
        };
        wrapRead();
        for (std::size_t done = 0; done < frames;) {
            std::size_t n = std::min(frames - done, static_cast<std::size_t>(m_maxBlockFrames));
            if (wraps && m_readNext.value < loop.end) {
                const std::int64_t untilEnd = (loop.end - m_readNext.value + m_ticksPerFrame - 1) / m_ticksPerFrame;
                n = std::min(n, static_cast<std::size_t>(std::max<std::int64_t>(untilEnd, 1)));
            }
            mixAt(m_readNext, out.subspan(done * ch, n * ch), resync);
            m_readNext += Ticks{static_cast<std::int64_t>(n) * m_ticksPerFrame};
            done += n;
            wrapRead();
        }
    }

    /// One mixer block (at most maxBlockFrames) of every active track read at timeline time `at`, through the
    /// master bus, into `out`.
    void mixAt(Ticks at, std::span<float> out, Resync& resync) noexcept {
        const auto ch = static_cast<std::size_t>(m_channels);
        const std::size_t n = out.size() / ch;
        const int active = m_active.load(std::memory_order_acquire);
        m_mixer.beginBlock(out);
        for (int slot = 0; slot < active; ++slot) {
            const std::span<float> scratch(m_scratch.data(), n * ch);
            if (!m_streams[static_cast<std::size_t>(slot)]->read(at, scratch, m_ticksPerFrame, m_maxWaitFrames, m_counters) &&
                !resync.needed) {
                resync.needed = true;
                resync.at = at.value;
            }
            applyTrackFader(slot, at, scratch);
            m_mixer.addBus(slot, scratch, m_channels);
        }
        m_mixer.endBlock(&m_master);
    }

    /// The track's fader and panner on its pre-fader audio (the rings carry it so), on the parameter grid counted from
    /// timeline 0: what export applies when it renders from the start. An automation pass's live override replaces
    /// them while it is set (ramped in and out; LiveFader).
    void applyTrackFader(int slot, Ticks at, std::span<float> block) noexcept {
        const FaderProgram* fader = m_faders[static_cast<std::size_t>(slot)].load(std::memory_order_seq_cst);
        const auto frames = static_cast<std::int64_t>(block.size() / static_cast<std::size_t>(m_channels));
        const std::int64_t first = at.value >= 0 ? at.value / m_ticksPerFrame : -((-at.value + m_ticksPerFrame - 1) / m_ticksPerFrame);
        detail::LiveFader& live = m_live[static_cast<std::size_t>(slot)];
        const bool stereo = m_channels == 2; // A.9: on a 5.1 mix the track's 5.1 panner (in the routing) replaces its panner
        if (live.engaged()) {
            const Ticks t{first * m_ticksPerFrame};
            const double gain = fader != nullptr ? detail::faderGain(fader->volume.valueAt(t)) : 1.0;
            const audio::StereoGains balance = fader != nullptr && stereo ? audio::stereoBalance(static_cast<float>(fader->pan.valueAt(t)))
                                                                          : audio::StereoGains{};
            if (live.apply(block.data(), frames, m_channels, gain, balance, m_rampFrames, stereo)) {
                return;
            }
        }
        if (fader != nullptr) {
            const std::int64_t tpf = m_ticksPerFrame;
            detail::applyFader(fader->volume, stereo ? &fader->pan : nullptr, first, first + frames, block.data(), m_channels,
                               [tpf](std::int64_t f) { return Ticks{f * tpf}; });
        }
    }

    void finish(const Resync& resync, std::int64_t consumed) noexcept {
        if (resync.needed) {
            m_resyncTime.store(resync.at, std::memory_order_relaxed);
            m_resyncSeq.fetch_add(1, std::memory_order_release); // publishes m_resyncTime
        }
        // Counters and position: single writer (this thread), so plain stores.
        m_underruns.store(m_counters.underrunFrames, std::memory_order_relaxed);
        m_skipped.store(m_counters.skippedFrames, std::memory_order_relaxed);
        m_consumerTime.store(consumed, std::memory_order_relaxed);
        m_renders.fetch_add(1, std::memory_order_release); // publishes m_consumerTime
    }

    audio::LoudnessTap m_tap; // A.3: fed by render(); its worker only touches the tap
    const int m_channels;
    const int m_deviceChannels;
    ChannelMatrix m_toDevice;       ///< A.9: mix -> device when they differ
    bool m_guardOnDevice = false;   ///< the master's soft clip guard runs after m_toDevice instead
    std::vector<float> m_deviceMix; ///< one block of the mix before that conversion
    const Rate m_rate;
    const std::int64_t m_ticksPerFrame;
    const int m_maxBlockFrames;
    const std::int64_t m_maxWaitFrames;
    std::vector<std::unique_ptr<TrackStream>> m_streams; // fixed after construction
    audio::Mixer m_mixer;
    std::vector<float> m_scratch;
    std::vector<float> m_prime;
    StreamCounters m_counters; // audio thread only

    // Master bus (audio thread state besides the published program).
    detail::MasterBus m_master;
    bool m_masterStarted = false;
    Ticks m_masterNext;
    int m_masterLatency = 0;
    Ticks m_readNext; ///< where the tracks are read next (the master's latency ahead, wrapped with the loop)
    LoopRange m_lastLoop;
    std::atomic<std::uint64_t> m_loopSeq{0}; ///< setLoop(): odd while it writes
    std::atomic<std::int64_t> m_loopStart{0};
    std::atomic<std::int64_t> m_loopEnd{0};

    // Shuttle audition (audio thread).
    audio::dsp::TimeStretch m_stretch;
    std::vector<float> m_stretchIn;
    bool m_stretching = false;
    Ticks m_stretchCursor; ///< next timeline frame to read
    Ticks m_stretchNext;   ///< the start a continuing renderAtSpeed() has

    // Track faders and the live overrides (automation passes).
    std::unique_ptr<std::atomic<const FaderProgram*>[]> m_faders;
    std::atomic<std::uint64_t> m_faderSeq{0};
    std::unique_ptr<detail::LiveFader[]> m_live;
    detail::LiveFader m_masterLive;
    std::unique_ptr<std::atomic<std::uint64_t>[]> m_slotTrack;
    const int m_rampFrames;

    std::atomic<int> m_active{0};
    std::atomic<std::uint64_t> m_renders{0};
    std::atomic<std::int64_t> m_consumerTime{0};
    std::atomic<std::uint64_t> m_resyncSeq{0};
    std::atomic<std::uint64_t> m_readyGeneration{0};
    std::atomic<std::int64_t> m_resyncTime{0};
    std::atomic<std::uint64_t> m_underruns{0};
    std::atomic<std::uint64_t> m_skipped{0};
};

/// Whether `track` plays any of `media` (sorted), also through its nested and multicam clips (C.8).
bool heardOn(const ProjectState& state, const Track& track, const std::vector<MediaId>& media) {
    const std::vector<MediaId> heard = mediaHeardOn(state, track);
    return std::any_of(heard.begin(), heard.end(), [&](MediaId m) { return std::binary_search(media.begin(), media.end(), m); });
}

/// Whether `track` has a nested or multicam clip that plays one of `sequences`, directly or deeper (C.8).
bool nestsAny(const ProjectState& state, const Track& track, const std::vector<SequenceId>& sequences) {
    return std::any_of(track.clips.begin(), track.clips.end(), [&](const Clip& c) {
        return c.isNested() && std::any_of(sequences.begin(), sequences.end(),
                                           [&](SequenceId s) { return sequenceContains(state, c.sequence, s); });
    });
}

const audio::LevelMeter& silentMeter() {
    static const audio::LevelMeter kSilent;
    return kSilent;
}

} // namespace

// =========================================================================================================
// The player: control side and feeder.
//
// Locking: `requests` guards what the control functions hand to the feeder (short critical sections, so the
// GUI never waits for decoding); `passLock` serialises feeder passes (the thread and feedOnce()) and guards
// the feeder state. Neither is ever taken on the audio thread (both are rt::CheckedMutex, so tests prove it).
// =========================================================================================================
struct TimelineAudioPlayer::Impl {
    Impl(audio::AudioEngine& e, std::shared_ptr<AudioFeed> f, TimelineAudioOptions o)
        : engine(e), feed(std::move(f)), options(o) {}

    struct Slot {
        TrackPtr track;          ///< as of the last evaluated snapshot (null: no track at this index)
        std::uint64_t epoch = 0; ///< epoch of the data being written
        std::int64_t cursor = 0; ///< timeline time of the next frame to write
        /// Frames of this epoch queued past the playhead. Counted (written, less what the transport played), not
        /// derived from the cursor: inside a loop shorter than the lead the queue holds several passes of it, which the
        /// cursor cannot tell apart, and filling "up to the lead" by the cursor would fill the ring with repeats and
        /// leave no room for a refill (an edit or loop change while paused then played a block of silence).
        std::int64_t ahead = 0;
        bool aheadKnown = false; ///< false after a flush: measured from the cursor on the next fill
        /// Renders the track's audio (clip and track effects carry state across chunks; restarts with pre-roll
        /// whenever the cursor jumps). Pre-fader: the callback applies the fader (fader).
        std::unique_ptr<TrackRenderer> renderer;
        /// The fader program the callback applies to this slot (owned here, published to the source).
        std::unique_ptr<const TimelineMixSource::FaderProgram> fader;
        TrackId publishedTrack; ///< the track the source was told this slot plays
    };

    /// What the control side asks for; copied by each pass.
    struct Requests {
        const EditSession* session = nullptr;
        SequenceId sequence;
        bool reevaluate = false;       ///< session/sequence changed: diff everything again
        std::uint64_t seekGeneration = 0;
        std::int64_t seekTarget = 0;
        std::uint64_t seekSerial = 0;  ///< increments with every seek()
        bool resyncAtPlayhead = false; ///< speed back to 1/1 or sequence switch: refill from the playhead
        std::optional<TimeRange> loop;
        std::uint64_t loopSerial = 0;
    };

    audio::AudioEngine& engine;
    std::shared_ptr<AudioFeed> feed;
    const TimelineAudioOptions options;

    mutable audio::rt::CheckedMutex requestsLock;
    std::condition_variable_any wake;
    Requests requests;
    bool quit = false;
    bool woken = false;
    std::atomic<std::uint64_t> seekSerial{0}; ///< lets a long fill notice a newer seek and stop early
    std::atomic<bool> started{false};
    std::thread feeder;

    mutable audio::rt::CheckedMutex passLock;
    // ---- Feeder state (passLock) ----
    /// Created by start(); replaced (rebuildSource) when the master's format changes. Feeder state: the control
    /// functions read `published` instead.
    std::shared_ptr<TimelineMixSource> source;
    /// Review A5-P2-3: `source` for any other thread (meters, live mix, stats), which holds it while it uses it.
    std::atomic<std::shared_ptr<TimelineMixSource>> published;
    bool installed = false; ///< start() has handed `source` to the engine
    std::int64_t tpf = Rate{48000, 1}.ticksPerUnit();
    const EditSession* session = nullptr;
    SequenceId sequence;
    std::uint64_t appliedSeekSerial = 0;
    bool seekPending = false;
    std::uint64_t pendingSeekGeneration = 0;
    bool readyAfterFill = false; ///< the next complete fill answers the pending seek (TimelineMixSource::setReady)
    std::int64_t pendingSeekTarget = 0;
    std::uint64_t appliedLoopSerial = 0;
    std::optional<TimeRange> loop;
    std::uint64_t epochCounter = 0;
    std::uint64_t appliedResync = 0;
    std::int64_t anchorTime = 0;       ///< playhead estimate at the last anchor (seek/resync)
    std::optional<std::int64_t> lastHead; ///< the playhead the last fill saw
    std::uint64_t rendersAtAnchor = 0; ///< source->renders() then
    std::int64_t lastResyncAnchor = std::numeric_limits<std::int64_t>::min();
    ProjectSnapshot lastSnapshot;
    std::shared_ptr<const AudioRedactions> redactionTable; ///< what the renderers apply
    std::shared_ptr<const AudioTakes> takeTable;           ///< the enhanced takes they play (A.4)
    std::vector<Slot> slots;
    int deviceChannels = 2; ///< the engine's (A.9: the mix follows the master's format and is converted to it)
    /// A.9: a source replaced when the master's format changed, kept (with the fader programs it points to) until the
    /// engine has let go of it.
    struct RetiredSource {
        std::shared_ptr<TimelineMixSource> source;
        std::vector<std::unique_ptr<const TimelineMixSource::FaderProgram>> faders;
    };
    std::vector<RetiredSource> retiredSources;
    /// Fader programs replaced while the callback may still use them, with the render sequence at retirement.
    std::vector<std::pair<std::unique_ptr<const TimelineMixSource::FaderProgram>, std::uint64_t>> retiredFaders;
    std::vector<float> chunk;
    TrackRenderStats renderStats;

    // ---- Statistics (any thread) ----
    std::atomic<std::uint64_t> passes{0};
    std::atomic<std::uint64_t> framesFed{0};
    std::atomic<std::uint64_t> seeks{0};
    std::atomic<std::uint64_t> resyncs{0};
    std::atomic<std::uint64_t> editFlushes{0};
    std::atomic<std::uint64_t> feedErrors{0};
    std::atomic<std::uint64_t> droppedTracks{0};
    std::atomic<int> activeTracks{0};

    // ---- Helpers (passLock held) ----------------------------------------------------------------------------
    std::int64_t leadFrames() const { return std::max<std::int64_t>(framesOf(options.leadTime, tpf, Rounding::Ceil), 1); }
    std::int64_t cutoverFrames() const { return framesOf(options.editCutover, tpf, Rounding::Ceil); }

    std::optional<std::int64_t> loopLength() const {
        if (!loop || loop->isEmpty()) {
            return std::nullopt;
        }
        return loop->duration.value;
    }

    /// Timeline position `frames` frames after `t`, wrapping at the loop end exactly like TransportMap.
    std::int64_t advance(std::int64_t t, std::int64_t frames) const {
        const std::int64_t raw = t + frames * tpf;
        if (const auto len = loopLength(); len && t < loop->end().value && raw >= loop->end().value) {
            const std::int64_t r = (raw - loop->start.value) % *len;
            return loop->start.value + (r < 0 ? r + *len : r);
        }
        return raw;
    }

    /// At most `limit` frames from `t`, stopping where the loop wraps.
    std::int64_t framesUntilWrap(std::int64_t t, std::int64_t limit) const {
        if (loopLength() && t < loop->end().value) {
            return std::min(limit, mulDivSaturate(loop->end().value - t, 1, tpf, Rounding::Ceil));
        }
        return limit;
    }

    /// The end of loop `before`, where audio queued under it wraps, when it lies ahead of `head` and the current
    /// loop no longer wraps there or earlier (removed, or ending later): the transport plays straight on past it.
    std::optional<std::int64_t> passedWrap(const std::optional<TimeRange>& before, std::int64_t head) const {
        if (!before || before->isEmpty() || head >= before->end().value) {
            return std::nullopt;
        }
        const std::int64_t wrap = before->end().value;
        if (loopLength() && head < loop->end().value && loop->end().value <= wrap) {
            return std::nullopt; // the new loop wraps first: the cutover (loop-aware) handles it
        }
        return wrap;
    }

    /// Whether the transport wrapped at the current loop's end since the last fill (from before its end back into
    /// it) where loop `before` would not have: the audio queued under `before` went on straight there.
    bool wrappedAtNewEnd(const std::optional<TimeRange>& before, std::int64_t head) const {
        return loopLength() && lastHead && *lastHead < loop->end().value && head < *lastHead && head >= loop->start.value &&
               (!before || before->isEmpty() || before->end() != loop->end());
    }

    /// How far `cursor` is ahead of `playhead`, in frames (loop-aware); negative = behind.
    std::int64_t aheadFrames(std::int64_t playhead, std::int64_t cursor) const {
        std::int64_t d = cursor - playhead;
        if (d < 0) {
            if (const auto len = loopLength(); len && playhead >= loop->start.value && playhead < loop->end().value) {
                d += *len; // the cursor already wrapped back into the loop
            }
        }
        return d >= 0 ? d / tpf : -((-d + tpf - 1) / tpf);
    }

    /// The callback's position if it has rendered since the last anchor, else the anchor. The callback reports
    /// the end of its last block; the engine splits blocks at a loop wrap, so that end is exactly the loop end
    /// when playback is about to wrap: report the wrapped position then, as the transport will.
    std::int64_t playhead() const {
        const std::int64_t t = source->renders() != rendersAtAnchor ? source->consumerTime() : anchorTime;
        if (loopLength() && t >= loop->end().value && t < loop->end().value + tpf) {
            return loop->start.value + (t - loop->end().value);
        }
        return t;
    }

    void flushSlot(Slot& slot, int index, std::int64_t cutover, std::int64_t cursor) {
        slot.epoch = ++epochCounter;
        slot.cursor = cursor;
        slot.aheadKnown = false;
        source->stream(index).flush(slot.epoch, cutover);
    }

    void flushAll(std::int64_t cutover, std::int64_t cursor) {
        for (int i = 0; i < static_cast<int>(slots.size()); ++i) {
            flushSlot(slots[static_cast<std::size_t>(i)], i, cutover, cursor);
        }
    }

    void anchor(std::int64_t at) {
        anchorTime = at;
        rendersAtAnchor = source->renders();
        flushAll(TrackStream::kImmediate, at);
    }

    /// A.9: the mix's channel count: 6 for a 5.1 master, else the device's.
    int mixChannelsFor(const Sequence* seq) const {
        return seq != nullptr && seq->masterFormat == ChannelFormat::Surround51 ? 6 : deviceChannels;
    }

    /// Creates the source (mixing `mixChannels` channels, handing the device deviceChannels) and the slots' renderers.
    void buildSource(int mixChannels) {
        const audio::AudioDeviceConfig config = engine.config();
        const Rate rate{config.sampleRate, 1};
        const std::int64_t lead = leadFrames();
        const std::int64_t chunkFrames = options.chunkFrames;
        // Room for a full lead of old audio plus a full lead of new audio after an edit, plus slack.
        const auto ringFrames = std::bit_ceil(static_cast<std::size_t>(2 * lead + 4 * chunkFrames + config.maxBlockFrames));
        const auto fadeFrames = static_cast<int>(framesOf(options.edgeFade, tpf, Rounding::Nearest));
        const std::int64_t maxWait = lead + 2 * cutoverFrames() + chunkFrames;
        source = std::make_shared<TimelineMixSource>(options.maxTracks, mixChannels, deviceChannels, config.sampleRate, ringFrames,
                                                     std::size_t{4096}, fadeFrames, config.maxBlockFrames, maxWait,
                                                     options.feederThread);
        {
            std::lock_guard lock(requestsLock); // one writer at a time (setLoop() holds it too)
            source->setLoop(requests.loop);
        }
        published.store(source, std::memory_order_release);
        slots.clear();
        slots.resize(static_cast<std::size_t>(options.maxTracks));
        TrackRendererOptions rendererOptions;
        rendererOptions.edgeFade = options.edgeFade;
        rendererOptions.trackFader = false; // the callback applies the faders (and an automation pass's live override)
        for (Slot& slot : slots) {
            slot.renderer = std::make_unique<TrackRenderer>(mixChannels, rate, rendererOptions);
        }
        chunk.assign(static_cast<std::size_t>(chunkFrames) * static_cast<std::size_t>(mixChannels), 0.0f);
    }

    /// A.9: the master's format changed: a new source mixing `mixChannels`, installed in the engine in place of the old
    /// one, everything evaluated again and filled from `head`.
    void rebuildSource(int mixChannels, std::int64_t head) {
        RetiredSource retired;
        retired.source = source;
        for (Slot& slot : slots) {
            if (slot.fader) {
                retired.faders.push_back(std::move(slot.fader));
            }
        }
        for (auto& [fader, mark] : retiredFaders) {
            if (fader) {
                retired.faders.push_back(std::move(fader));
            }
        }
        retiredFaders.clear();
        buildSource(mixChannels);
        if (engine.source() == retired.source) {
            engine.setSource(source);
        }
        retiredSources.push_back(std::move(retired));
        lastSnapshot = nullptr; // evaluate every track again into the new rings
        redactionTable.reset();
        takeTable.reset();
        lastHead.reset();
        anchor(head);
        for (int i = 0; i < options.maxTracks; ++i) {
            applyTrackFlags(i, nullptr);
        }
    }

    void applyTrackFlags(int index, const Track* track) {
        audio::Mixer& mixer = source->mixer();
        mixer.setBusMuted(index, track == nullptr || track->muted || !track->enabled);
        mixer.setBusSoloed(index, track != nullptr && track->solo);
        publishFader(index, track);
    }

    /// The track's fader and panner to the callback (a new program when they changed; the old one retired), and which
    /// track the slot plays: a slot that now plays another track drops its live override.
    void publishFader(int index, const Track* track) {
        Slot& slot = slots[static_cast<std::size_t>(index)];
        const TrackId id = track != nullptr ? track->id : TrackId{};
        if (id != slot.publishedTrack) {
            slot.publishedTrack = id;
            source->setSlotTrack(index, id);
            source->live(index).clear();
        }
        const bool unity = track == nullptr || (track->volume == ParamValue() && track->pan == ParamValue());
        if (unity ? slot.fader == nullptr
                  : slot.fader != nullptr && slot.fader->volume == track->volume && slot.fader->pan == track->pan) {
            return;
        }
        std::unique_ptr<const TimelineMixSource::FaderProgram> next;
        if (!unity) {
            next = std::make_unique<const TimelineMixSource::FaderProgram>(TimelineMixSource::FaderProgram{track->volume, track->pan});
        }
        source->setFader(index, next.get());
        retiredFaders.emplace_back(std::move(slot.fader), source->faderSeq());
        slot.fader = std::move(next);
    }

    /// Frees the fader programs the callback is done with.
    void collectFaders() {
        const std::uint64_t now = source->faderSeq();
        std::erase_if(retiredFaders, [now](const auto& retired) { return retired.second % 2 == 0 || now > retired.second; });
    }

    /// Where a track must be refilled from after an edit that changed `ranges` of it, or nullopt when none of
    /// the audio already queued for it, [head, cursor), is affected (then the new track is simply used for what
    /// is filled next). Never earlier than `cutover`: audio about to be heard is not replaced.
    std::optional<std::int64_t> refillPoint(const std::vector<TimeRange>& ranges, std::int64_t head,
                                            std::int64_t cursor, std::int64_t cutover) const {
        if (loopLength()) {
            return ranges.empty() ? std::nullopt : std::optional<std::int64_t>(cutover); // wrapped queue: coarse
        }
        for (const TimeRange& r : ranges) { // sorted and disjoint
            if (r.end().value <= head || r.start.value >= cursor) {
                continue;
            }
            return std::max(cutover, std::max(r.start.value, head));
        }
        return std::nullopt;
    }

    void refill(Slot& slot, int index, std::int64_t from) {
        flushSlot(slot, index, from, from);
        editFlushes.fetch_add(1, std::memory_order_relaxed);
    }

    /// Applies a new snapshot. With the previous snapshot of the same sequence at hand, the model's ChangeSet
    /// (diff(), proportional to what changed) narrows the work: header-only changes (mute, solo, enable) go to
    /// the mixer, and a track is refilled only where an edited range overlaps audio already queued, from that
    /// range's start (or the cutover). Track-list changes, media changes (relink) and the first evaluation fall
    /// back to comparing every slot.
    void applySnapshot(const ProjectState* before, const ProjectState* after, const Sequence* seq, std::int64_t head,
                       std::int64_t cutover) {
        const std::vector<TrackPtr> empty;
        const std::vector<TrackPtr>& tracks = seq != nullptr ? seq->audioTracks : empty;
        const int count = std::min(static_cast<int>(tracks.size()), static_cast<int>(slots.size()));
        auto trackAt = [&](int i) { return i < count ? tracks[static_cast<std::size_t>(i)] : TrackPtr{}; };

        bool coarse = before == nullptr || after == nullptr || seq == nullptr;
        ChangeSet changes;
        const SequenceChange* change = nullptr;
        if (!coarse) {
            changes = diff(*before, *after);
            change = changes.sequence(sequence);
            coarse = !changes.modifiedMedia.empty() || !changes.removedMedia.empty() ||
                     (change != nullptr && change->trackListChanged);
        }

        if (coarse) {
            for (int i = 0; i < static_cast<int>(slots.size()); ++i) {
                Slot& slot = slots[static_cast<std::size_t>(i)];
                const TrackPtr next = trackAt(i);
                if (next == slot.track) {
                    continue;
                }
                const bool wasEvaluated = slot.track != nullptr;
                // The fader and panner are applied by the callback (publishFader): changing them refills nothing.
                const bool clipsChanged = !next || !slot.track || next->clips != slot.track->clips ||
                                          next->transitions != slot.track->transitions || next->effects != slot.track->effects ||
                                          next->format != slot.track->format || next->surround != slot.track->surround;
                slot.track = next;
                applyTrackFlags(i, next.get());
                if (clipsChanged) {
                    if (wasEvaluated) {
                        refill(slot, i, cutover); // an edit
                    } else {
                        flushSlot(slot, i, cutover, cutover); // first evaluation of this slot
                    }
                }
            }
        } else {
            for (int i = 0; i < count; ++i) {
                Slot& slot = slots[static_cast<std::size_t>(i)];
                const TrackPtr next = trackAt(i);
                const TrackChange* trackChange = change != nullptr ? change->track(next->id) : nullptr;
                const TrackPtr previous = slot.track;
                slot.track = next; // pointer-identical when untouched; always the current revision for future fills
                if (trackChange == nullptr) {
                    continue;
                }
                if (trackChange->headerChanged) {
                    applyTrackFlags(i, next.get());
                }
                // The track stack and the transitions change what every queued frame sounds like: refill from the cutover.
                // Clip edits refill from the edited range. The fader and panner go to the callback (applyTrackFlags).
                const bool wholeTrack =
                    trackChange->transitionsChanged ||
                    (trackChange->headerChanged && previous &&
                     (previous->effects != next->effects || previous->format != next->format || previous->surround != next->surround));
                if (wholeTrack) {
                    refill(slot, i, cutover);
                } else if (const auto from = refillPoint(trackChange->ranges, head, slot.cursor, cutover)) {
                    refill(slot, i, *from);
                }
            }
        }
        // C.8: an edit inside a sequence that a track nests (directly or deeper) changes what its nested and multicam
        // clips play, wherever they are: refill those tracks from the cutover, where their nested mixes restart.
        if (before != nullptr && after != nullptr && seq != nullptr) {
            std::vector<SequenceId> nestedChanges = changes.removedSequences;
            for (const SequenceChange& c : changes.sequences) {
                if (c.sequence != sequence) {
                    nestedChanges.push_back(c.sequence);
                }
            }
            for (int i = 0; i < count && !nestedChanges.empty(); ++i) {
                Slot& slot = slots[static_cast<std::size_t>(i)];
                if (slot.track && nestsAny(*after, *slot.track, nestedChanges)) {
                    refill(slot, i, cutover);
                }
            }
        }
        // Redaction mutes and bleeps (I.6): resolved here, off the audio thread. Queued audio is refilled only on the
        // tracks that play a media item whose sound changed (review P2-12): renaming a redaction, moving its region or a
        // video-only redaction refills nothing. A media change can change which items share a redaction root (a new
        // subclip or stem of redacted media hears its source's bleeps: review wave 5 E5-P1-1, A5-P1-3).
        if (before == nullptr || after == nullptr || before->redactions != after->redactions || before->media != after->media) {
            auto table = after != nullptr ? AudioRedactions::of(*after) : nullptr;
            const std::vector<MediaId> changed = AudioRedactions::changedMedia(redactionTable.get(), table.get());
            if (before == nullptr || !changed.empty()) {
                redactionTable = std::move(table);
                for (int i = 0; i < static_cast<int>(slots.size()); ++i) {
                    Slot& slot = slots[static_cast<std::size_t>(i)];
                    slot.renderer->setRedactions(redactionTable);
                    const bool affected = slot.track && after != nullptr && heardOn(*after, *slot.track, changed);
                    if (before != nullptr && affected) {
                        refill(slot, i, cutover);
                    }
                }
            }
        }
        // A.4: the enhanced takes clips with Enhance Speech play (the table checks their files: resolved here, off the
        // audio thread, when the media change). Queued audio is refilled on the tracks that play a media item whose
        // usable takes changed (a take recorded, rendered again or forgotten).
        if (before == nullptr || after == nullptr || before->media != after->media) {
            auto table = after != nullptr ? AudioTakes::of(*after) : nullptr;
            const std::vector<MediaId> changed = AudioTakes::changedMedia(takeTable.get(), table.get());
            if (before == nullptr || !changed.empty()) {
                takeTable = std::move(table);
                for (int i = 0; i < static_cast<int>(slots.size()); ++i) {
                    Slot& slot = slots[static_cast<std::size_t>(i)];
                    slot.renderer->setAudioTakes(takeTable);
                    const bool affected = slot.track && after != nullptr && heardOn(*after, *slot.track, changed);
                    if (before != nullptr && affected) {
                        refill(slot, i, cutover);
                    }
                }
            }
        }
        // A.9: a media item's channel map changes how every clip of it sounds (clips without a map of their own): refill
        // the tracks that play it.
        if (before != nullptr && after != nullptr && before->media != after->media) {
            std::vector<MediaId> remapped;
            for (const MediaPtr& m : after->media) {
                const MediaItem* old = before->findMedia(m->id);
                if (old != nullptr && old->channels != m->channels) {
                    remapped.push_back(m->id);
                }
            }
            for (int i = 0; i < static_cast<int>(slots.size()) && !remapped.empty(); ++i) {
                Slot& slot = slots[static_cast<std::size_t>(i)];
                if (slot.track && heardOn(*after, *slot.track, remapped)) {
                    refill(slot, i, cutover);
                }
            }
        }
        droppedTracks.store(static_cast<std::uint64_t>(std::max(static_cast<int>(tracks.size()) - count, 0)),
                            std::memory_order_relaxed);
        activeTracks.store(count, std::memory_order_relaxed);
        source->setActive(count);
    }

    // ---- One feeder pass (passLock held) ---------------------------------------------------------------------
    void pass() {
        if (!source || !started.load(std::memory_order_acquire)) {
            return;
        }
        Requests req;
        {
            std::lock_guard lock(requestsLock);
            req = requests;
            requests.reevaluate = false;
            requests.resyncAtPlayhead = false;
        }
        passes.fetch_add(1, std::memory_order_relaxed);
        if (req.session != session || req.sequence != sequence || req.reevaluate) {
            session = req.session;
            sequence = req.sequence;
            lastSnapshot = nullptr;
        }
        const ProjectSnapshot snapshot = session != nullptr ? session->snapshot() : nullptr; // once per pass
        const Sequence* seq = snapshot ? snapshot->findSequence(sequence) : nullptr;
        // A.9: a master of another format mixes in another channel count.
        std::erase_if(retiredSources, [](const RetiredSource& r) { return r.source.use_count() == 1; });
        // Review A5-P2-3: not while another source wraps ours in the engine (a voice-over pass): that one would go on
        // playing the old source, fed no more, and put it back at its end. The new format applies once ours plays
        // again.
        if (const int wanted = mixChannelsFor(seq);
            wanted != source->channels() && (!installed || engine.source() == source)) {
            rebuildSource(wanted, playhead());
        }

        // 1. Seek: latest wins; applied once the engine has applied it, so a callback still at the old position
        //    never sees (and drops) the new data. The engine prerolls meanwhile.
        if (req.seekSerial != appliedSeekSerial) {
            appliedSeekSerial = req.seekSerial;
            seekPending = true;
            pendingSeekGeneration = req.seekGeneration;
            pendingSeekTarget = req.seekTarget;
        }
        if (seekPending && engine.appliedSeekGeneration() >= pendingSeekGeneration) {
            seekPending = false;
            anchor(pendingSeekTarget);
            readyAfterFill = true;
        }
        // 2. Loop changes (the engine wraps at the same place; refill from the cutover).
        const bool loopChanged = req.loopSerial != appliedLoopSerial;
        const std::optional<TimeRange> previousLoop = loop;
        if (loopChanged) {
            appliedLoopSerial = req.loopSerial;
            loop = req.loop;
        }
        // 3. Transport jumps the player was not told about, reported by the callback.
        if (const std::uint64_t serial = source->resyncSeq(); serial != appliedResync) {
            appliedResync = serial;
            const std::int64_t at = source->resyncTime();
            if (!seekPending && !(at >= lastResyncAnchor - options.editCutover.value && at <= lastResyncAnchor)) {
                lastResyncAnchor = advance(at, cutoverFrames());
                anchor(lastResyncAnchor);
                resyncs.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (req.resyncAtPlayhead && !seekPending) {
            anchor(engine.clock().value);
        }

        // 4. Edits and loop changes take over from a cutover ahead of the playhead (at once while paused).
        const std::int64_t head = playhead();
        const std::int64_t cutover = advance(head, engine.isPaused() ? 0 : cutoverFrames());
        if (loopChanged) {
            // The queued audio follows the old loop. Where the transport parts from it before the cutover, refill
            // from there, or the audio between that point and the cutover would be missing (a gap):
            //   * the old loop's wrap, when the transport now plays straight on past it (removed, or ending later);
            //   * the playhead, when the transport already wrapped at the new loop's end, where the queue did not.
            std::int64_t from = cutover;
            std::int64_t marker = cutover;
            if (const auto wrap = passedWrap(previousLoop, head);
                wrap && *wrap < head + (engine.isPaused() ? 0 : cutoverFrames()) * tpf) {
                from = marker = *wrap;
            } else if (wrappedAtNewEnd(previousLoop, head)) {
                from = head;
                marker = TrackStream::kImmediate;
            }
            flushAll(marker, from);
        }
        if (snapshot != lastSnapshot) {
            applySnapshot(lastSnapshot.get(), snapshot.get(), seq, head, cutover);
            for (Slot& slot : slots) {
                slot.renderer->setProject(snapshot); // C.8: the sequences nested and multicam clips play
            }
            lastSnapshot = snapshot;
            source->master().update(seq != nullptr ? seq->audioEffects : EffectStack{},
                                    seq != nullptr ? seq->masterVolume : ParamValue{});
        }
        source->master().collect();
        collectFaders();
        if (engine.isPaused()) {
            // An automation pass ends with playback, however the transport was stopped (the app stops it through the
            // playback controller): the faders return to their automation.
            for (int i = 0; i < source->slots(); ++i) {
                source->live(i).clear();
            }
            source->masterLive().clear();
        }
        if (seekPending) {
            return; // nothing to fill until the seek target is known to be live
        }

        // 5. Fill every active track up to the lead.
        const int active = activeTracks.load(std::memory_order_relaxed);
        const std::int64_t lead = leadFrames();
        const auto ch = static_cast<std::size_t>(source->channels());
        // How far the transport moved since the last fill: forwards, or across a loop wrap. Anything else (a jump
        // back) makes every slot measure its queue from the cursor again.
        std::optional<std::int64_t> moved;
        if (lastHead) {
            std::int64_t d = head - *lastHead;
            if (const auto len = loopLength(); d < 0 && len && *lastHead < loop->end().value && head >= loop->start.value) {
                d += *len; // wrapped once
            }
            if (d >= 0) {
                moved = mulDivSaturate(d, 1, tpf, Rounding::Nearest);
            }
        }
        lastHead = head;
        for (int i = 0; i < active; ++i) {
            Slot& slot = slots[static_cast<std::size_t>(i)];
            if (!slot.track) {
                slot.aheadKnown = false;
                continue;
            }
            TrackStream& stream = source->stream(i);
            if (slot.aheadKnown && moved) {
                slot.ahead -= *moved;
            } else {
                slot.ahead = aheadFrames(head, slot.cursor);
                slot.aheadKnown = true;
            }
            if (slot.ahead < 0) {
                // The callback overtook the feeder (slow feed): jump ahead instead of feeding stale audio.
                flushSlot(slot, i, TrackStream::kImmediate, advance(head, cutoverFrames()));
                slot.ahead = aheadFrames(head, slot.cursor);
                slot.aheadKnown = true;
            }
            while (slot.ahead < lead) {
                if (seekSerial.load(std::memory_order_acquire) != appliedSeekSerial) {
                    return; // a newer seek arrived: this audio is obsolete (latest seek wins)
                }
                const auto writable = static_cast<std::int64_t>(stream.writableFrames());
                const std::int64_t n =
                    framesUntilWrap(slot.cursor, std::min<std::int64_t>({options.chunkFrames, writable, lead - slot.ahead}));
                if (n <= 0) {
                    break;
                }
                const std::span<float> buffer(chunk.data(), static_cast<std::size_t>(n) * ch);
                slot.renderer->render(*slot.track, Ticks{slot.cursor}, buffer, *feed, renderStats);
                if (!stream.write(slot.epoch, Ticks{slot.cursor}, buffer)) {
                    break;
                }
                slot.cursor = advance(slot.cursor, n);
                slot.ahead += n;
                framesFed.fetch_add(static_cast<std::uint64_t>(n), std::memory_order_relaxed);
            }
        }
        for (std::size_t i = static_cast<std::size_t>(std::max(active, 0)); i < slots.size(); ++i) {
            slots[i].aheadKnown = false; // not filled this pass: measured again when they are
        }
        if (readyAfterFill) {
            // Every track holds its lead from the seek target on (a newer seek would have returned above): the engine
            // can end the preroll now instead of after the full TimelineAudioOptions::preroll (P.1).
            readyAfterFill = false;
            source->setReady(pendingSeekGeneration);
        }
        feedErrors.store(renderStats.feedErrors, std::memory_order_relaxed);
    }

    void feederMain() {
        const audio::rt::ScopedDenormalsOff denormals; // review A5-P2-9: the renderers' filters ring down in silence
        for (;;) {
            {
                std::unique_lock lock(requestsLock);
                wake.wait_for(lock, options.feederInterval, [this] { return quit || woken; });
                woken = false;
                if (quit) {
                    return;
                }
            }
            std::lock_guard lock(passLock);
            pass();
        }
    }

    /// requestsLock held.
    void notify() {
        woken = true;
        wake.notify_all();
    }
};

// ---- Public API --------------------------------------------------------------------------------------------

TimelineAudioPlayer::TimelineAudioPlayer(audio::AudioEngine& engine, std::shared_ptr<AudioFeed> feed,
                                         TimelineAudioOptions options)
    : m_impl(std::make_unique<Impl>(engine, std::move(feed), options)) {}

TimelineAudioPlayer::~TimelineAudioPlayer() { stop(); }

void TimelineAudioPlayer::setSession(const EditSession* session) {
    std::lock_guard lock(m_impl->requestsLock);
    m_impl->requests.session = session;
    m_impl->requests.reevaluate = true;
    m_impl->requests.resyncAtPlayhead = true;
    m_impl->notify();
}

void TimelineAudioPlayer::setSequence(SequenceId sequence) {
    std::lock_guard lock(m_impl->requestsLock);
    m_impl->requests.sequence = sequence;
    m_impl->requests.reevaluate = true;
    m_impl->requests.resyncAtPlayhead = true;
    m_impl->notify();
}

std::expected<void, Error> TimelineAudioPlayer::start() {
    Impl& d = *m_impl;
    std::lock_guard pass(d.passLock);
    if (d.started.load(std::memory_order_acquire)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "timeline audio player already started"});
    }
    if (!d.feed) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "timeline audio player needs an AudioFeed"});
    }
    const audio::AudioDeviceConfig config = d.engine.config();
    const Rate rate{config.sampleRate, 1};
    if (!rate.isExact() || config.channels < 1 || d.options.maxTracks < 1 || d.options.chunkFrames < 1) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "unsupported engine format or player options"});
    }
    d.tpf = rate.ticksPerUnit();
    d.deviceChannels = config.channels;
    d.retiredFaders.clear();
    d.retiredSources.clear();
    d.redactionTable.reset();
    d.takeTable.reset();
    d.buildSource(config.channels); // the first pass switches to 5.1 when the master is
    d.lastSnapshot = nullptr;
    d.lastHead.reset();
    d.seekPending = false;
    {
        std::lock_guard lock(d.requestsLock);
        d.appliedSeekSerial = d.requests.seekSerial;
        d.appliedLoopSerial = d.requests.loopSerial - 1; // adopt the current loop on the first pass
        d.requests.resyncAtPlayhead = false;
        d.quit = false;
    }
    d.loop.reset();
    d.started.store(true, std::memory_order_release);
    d.anchor(d.engine.clock().value);
    for (int i = 0; i < d.options.maxTracks; ++i) {
        d.applyTrackFlags(i, nullptr);
    }
    d.installed = false;
    d.pass(); // evaluate the snapshot and prime the rings before the callback can see the source
    d.engine.setSource(d.source);
    d.installed = true;
    if (d.options.feederThread) {
        d.feeder = std::thread([&d] { d.feederMain(); });
    }
    return {};
}

void TimelineAudioPlayer::stop() {
    Impl& d = *m_impl;
    if (!d.started.load(std::memory_order_acquire)) {
        return;
    }
    {
        std::lock_guard lock(d.requestsLock);
        d.quit = true;
        d.wake.notify_all();
    }
    if (d.feeder.joinable()) {
        d.feeder.join();
    }
    std::lock_guard pass(d.passLock);
    clearLiveMix();
    if (d.engine.source() == d.source) {
        d.engine.setSource(nullptr); // the engine releases our source off the audio thread
    }
    d.started.store(false, std::memory_order_release);
}

bool TimelineAudioPlayer::isStarted() const noexcept { return m_impl->started.load(std::memory_order_acquire); }

void TimelineAudioPlayer::play() {
    if (m_impl->engine.isPaused()) {
        resetLoudness(); // integrated loudness counts from the start of this play
    }
    m_impl->engine.resume();
}

void TimelineAudioPlayer::pause() {
    m_impl->engine.pause();
    clearLiveMix(); // the automation pass ends with playback (the fader returns to its automation, as in Premiere)
}

bool TimelineAudioPlayer::setLiveTrackMix(TrackId track, std::optional<double> gainDb, std::optional<double> pan) {
    const std::shared_ptr<TimelineMixSource> source = m_impl->published.load(std::memory_order_acquire);
    if (source == nullptr || !m_impl->started.load(std::memory_order_acquire)) {
        return false;
    }
    if (!track.isValid()) {
        source->masterLive().set(gainDb, pan);
        return true;
    }
    const int slot = source->slotOf(track);
    if (slot < 0) {
        return false;
    }
    source->live(slot).set(gainDb, pan);
    return true;
}

void TimelineAudioPlayer::clearLiveMix() noexcept {
    if (const std::shared_ptr<TimelineMixSource> source = m_impl->published.load(std::memory_order_acquire)) {
        for (int i = 0; i < source->slots(); ++i) {
            source->live(i).clear();
        }
        source->masterLive().clear();
    }
}

void TimelineAudioPlayer::seek(Ticks target) {
    Impl& d = *m_impl;
    std::lock_guard lock(d.requestsLock);
    d.requests.seekGeneration = d.engine.seek(target, d.options.preroll);
    d.requests.seekTarget = target.value;
    d.requests.seekSerial += 1;
    d.seekSerial.store(d.requests.seekSerial, std::memory_order_release);
    d.seeks.fetch_add(1, std::memory_order_relaxed);
    d.notify();
}

void TimelineAudioPlayer::setSpeed(audio::TransportSpeed speed) {
    Impl& d = *m_impl;
    const audio::TransportSpeed before = d.engine.speed();
    d.engine.setSpeed(speed);
    // Audible shuttle speeds consume the track streams like normal play; only after a silent speed (reverse, 8x)
    // has the transport moved past what is queued.
    if (speed.isValid() && speed.normalized().isNormal() && !before.isNormal() && !audio::isAudibleShuttle(before)) {
        std::lock_guard lock(d.requestsLock);
        d.requests.resyncAtPlayhead = true; // the transport moved while muted: refill from where it is now
        d.notify();
    }
}

void TimelineAudioPlayer::setLoop(std::optional<TimeRange> range) {
    Impl& d = *m_impl;
    std::lock_guard lock(d.requestsLock);
    d.engine.setLoop(range);
    d.requests.loop = range && !range->isEmpty() ? range : std::nullopt;
    d.requests.loopSerial += 1;
    if (const std::shared_ptr<TimelineMixSource> source = d.published.load(std::memory_order_acquire)) {
        source->setLoop(d.requests.loop); // the callback's read-ahead wraps with the engine from now on
    }
    d.notify();
}

Ticks TimelineAudioPlayer::position() const noexcept { return m_impl->engine.clock(); }

void TimelineAudioPlayer::feedOnce() {
    std::lock_guard lock(m_impl->passLock);
    const audio::rt::ScopedDenormalsOff denormals;
    m_impl->pass();
    if (!m_impl->options.feederThread && m_impl->source) {
        m_impl->source->tap().update(); // manual mode: the loudness tap measures here too
    }
}

std::shared_ptr<audio::AudioSource> TimelineAudioPlayer::wrapEngineSource(const SourceWrap& wrap) {
    Impl& d = *m_impl;
    std::lock_guard pass(d.passLock); // no rebuildSource() in between
    std::shared_ptr<audio::AudioSource> program = d.engine.source();
    std::shared_ptr<audio::AudioSource> wrapper = wrap(program);
    if (!wrapper) {
        return nullptr;
    }
    d.engine.setSource(std::move(wrapper));
    return program;
}

void TimelineAudioPlayer::unwrapEngineSource(const std::shared_ptr<audio::AudioSource>& wrapper,
                                             const std::shared_ptr<audio::AudioSource>& program) {
    Impl& d = *m_impl;
    std::lock_guard pass(d.passLock);
    if (wrapper == nullptr || d.engine.source() != wrapper) {
        return; // someone else replaced it since
    }
    if (d.started.load(std::memory_order_acquire) && d.installed) {
        d.engine.setSource(d.source); // the player's current source: a format change waited for this
    } else {
        d.engine.setSource(program);
    }
}

MeterSnapshot::MeterSnapshot(const audio::LevelMeter& meter) noexcept
    : m_channels(std::clamp(meter.channels(), 0, audio::LevelMeter::kMaxChannels)), m_clipped(meter.clipped()) {
    for (int c = 0; c < m_channels; ++c) {
        m_peak[static_cast<std::size_t>(c)] = meter.peak(c);
        m_rms[static_cast<std::size_t>(c)] = meter.rms(c);
    }
}

MeterSnapshot TimelineAudioPlayer::trackMeter(int index) const noexcept {
    const std::shared_ptr<TimelineMixSource> source = m_impl->published.load(std::memory_order_acquire);
    if (source == nullptr || index < 0 || index >= source->slots()) {
        return MeterSnapshot(silentMeter());
    }
    return MeterSnapshot(source->mixer().busMeter(index));
}

void TimelineAudioPlayer::clearTrackClip(int index) const noexcept {
    const std::shared_ptr<TimelineMixSource> source = m_impl->published.load(std::memory_order_acquire);
    if (source != nullptr && index >= 0 && index < source->slots()) {
        source->mixer().busMeter(index).clearClip();
    }
}

audio::LoudnessReading TimelineAudioPlayer::loudness() const noexcept {
    const std::shared_ptr<TimelineMixSource> source = m_impl->published.load(std::memory_order_acquire);
    return source != nullptr ? source->tap().reading() : audio::LoudnessReading{};
}

void TimelineAudioPlayer::resetLoudness() noexcept {
    if (const std::shared_ptr<TimelineMixSource> source = m_impl->published.load(std::memory_order_acquire)) {
        source->tap().reset();
    }
}

void TimelineAudioPlayer::setLoudnessEnabled(bool enabled) noexcept {
    if (const std::shared_ptr<TimelineMixSource> source = m_impl->published.load(std::memory_order_acquire)) {
        source->tap().setEnabled(enabled);
    }
}

MeterSnapshot TimelineAudioPlayer::masterMeter() const noexcept {
    const std::shared_ptr<TimelineMixSource> source = m_impl->published.load(std::memory_order_acquire);
    return MeterSnapshot(source != nullptr ? source->mixer().masterMeter() : silentMeter());
}

void TimelineAudioPlayer::clearMasterClip() const noexcept {
    if (const std::shared_ptr<TimelineMixSource> source = m_impl->published.load(std::memory_order_acquire)) {
        source->mixer().masterMeter().clearClip();
    }
}

TimelineAudioStats TimelineAudioPlayer::stats() const noexcept {
    const Impl& d = *m_impl;
    TimelineAudioStats s;
    s.feederPasses = d.passes.load(std::memory_order_relaxed);
    s.framesFed = d.framesFed.load(std::memory_order_relaxed);
    s.seeks = d.seeks.load(std::memory_order_relaxed);
    s.resyncs = d.resyncs.load(std::memory_order_relaxed);
    s.editFlushes = d.editFlushes.load(std::memory_order_relaxed);
    s.feedErrors = d.feedErrors.load(std::memory_order_relaxed);
    s.droppedTracks = d.droppedTracks.load(std::memory_order_relaxed);
    s.activeTracks = d.activeTracks.load(std::memory_order_relaxed);
    if (const std::shared_ptr<TimelineMixSource> source = d.published.load(std::memory_order_acquire)) {
        s.underrunFrames = source->underrunFrames();
        s.skippedFrames = source->skippedFrames();
    }
    return s;
}

} // namespace wabcut::playback
