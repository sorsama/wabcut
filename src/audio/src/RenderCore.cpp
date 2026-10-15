#include "RenderCore.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>

namespace wabcut::audio::detail {

// ================================================================================================
// Source lifetime
//
// The audio thread must never own a std::shared_ptr: dropping the last reference there would run a
// destructor and free memory on the audio thread. So the control side keeps the shared_ptr and publishes
// only the raw pointer in m_sourcePtr. The audio thread brackets every block with two increments of
// m_blockCounter (odd = inside a block) and loads the pointer inside the bracket.
//
// After swapping in a new pointer, the control side reads the counter (all four operations are seq_cst,
// which gives the Dekker-style guarantee we need: in the single total order either the audio thread's
// "enter block" increment precedes the control thread's counter load, or the audio thread's pointer load
// follows the control thread's pointer store):
//   * counter even -> the audio thread was not inside a block; any later block loads the new pointer, so
//     the old source can be released immediately;
//   * counter odd  -> the audio thread may be using the old pointer in that very block; it is released as
//     soon as the counter has moved on (the block's closing increment is a seq_cst RMW, so everything the
//     audio thread did with the pointer happens-before the control thread observes the new counter).
// Retired sources therefore live at most one block longer than needed, are released by whichever control
// call comes next (or by the watchdog's periodic collect), and are always destroyed off the audio thread.
//
// Clock pipeline
//
//   callback (frame D, latency L, steady time t)  --DLL-->  ClockSample {position, host time, latency, epoch}
//   ClockSample + TransportMap  -->  presented device time at any host time  -->  timeline Ticks
//
// The DLL only filters the device clock; the TransportMap only maps device time to the timeline. Readers add
// two engine-level guarantees on top of TransportMap::transportTime(): never past the audio rendered so far,
// and never backwards (a global high-water mark on presented device time, which is continuous across seeks
// and device reopens because the device frame counter is).
// ================================================================================================

namespace {

constexpr double kNanosPerSecond = 1e9;

// Delay-locked loop gains, per update (one update per callback). For the error recursion
// e' = (1 - k1 - k2)·e + ..., the characteristic polynomial z² - (2 - k1 - k2)·z + (1 - k1) has complex roots
// of radius sqrt(1 - k1) ≈ 0.95 for these values: well damped, settling in ~20 callbacks (≈200 ms at a
// 10 ms period) while averaging out scheduling jitter of the callback thread. The frequency term is what
// tracks drift between the device crystal and the steady clock, so the clock follows the device without ever
// being periodically snapped.
constexpr double kPhaseGain = 0.1;
constexpr double kFrequencyGain = 0.005;
// Never let the frequency estimate leave ±0.5 % of nominal; real crystals are within ±0.01 %.
constexpr double kMaxRateDeviation = 0.005;
// Consecutive late callbacks after which a negative error is trusted (an underrun, not a scheduling hiccup).
constexpr int kLateBlocksBeforeResync = 3;

} // namespace

// ---- Control side ------------------------------------------------------------------------------------

void RenderCore::configure(int channels, int sampleRate, int maxBlockFrames, int declickFrames) {
    std::lock_guard lock(m_mutex);
    m_channels = std::min(channels, kMaxChannels);
    m_maxBlockFrames = maxBlockFrames;
    m_declickFrames = std::max(declickFrames, 0);
    if (sampleRate != m_sampleRate || m_map.sampleRate() != Rate{sampleRate, 1}) {
        // A new frame domain: carry the transport state over to a map at the new rate.
        const TransportMap old = m_map;
        TransportMap fresh(Rate{sampleRate, 1}, old.transportTime(0));
        fresh.setSpeed(old.playSpeed(), 0);
        if (old.isPlaying()) {
            fresh.play(0);
        }
        fresh.setLoop(old.loop(), 0);
        m_map = fresh;
        m_deviceFrame = 0;
    }
    m_sampleRate = sampleRate;
    m_ticksPerFrame = Rate{sampleRate, 1}.ticksPerUnit();
    publish();
}

void RenderCore::goLive() {
    std::lock_guard lock(m_mutex);
    // Audio-thread state. No audio thread runs yet; the device start (thread creation) or the caller's first
    // pull() orders these writes before the first process().
    m_map.rebase(m_deviceEpoch.load(std::memory_order_acquire), m_deviceFrame, 0); // position carries over
    m_deviceFrame = 0;
    m_modelValid = false;
    m_baseFrame = 0.0;
    m_baseNs = 0;
    m_framesPerSecond = static_cast<double>(m_sampleRate);
    m_avgBlockFrames = 0.0;
    m_lateBlocks = 0;
    m_prevAudible = false;
    m_prevSource = nullptr;
    m_tailPos = -1;
    m_fadeInPos = -1;
    std::fill(std::begin(m_lastOut), std::end(m_lastOut), 0.0f);
    m_latencyFrames = m_deviceLatencyFrames.load(std::memory_order_relaxed) +
                      m_offsetFrames.load(std::memory_order_relaxed);
    m_modelResetRequested.store(false, std::memory_order_relaxed);
    m_callbacks.store(0, std::memory_order_relaxed);
    m_framesRendered.store(0, std::memory_order_relaxed);
    m_resyncs.store(0, std::memory_order_relaxed);
    m_lastFrames.store(0, std::memory_order_relaxed);
    m_lastCallbackNs.store(0, std::memory_order_relaxed);
    m_driftPpm.store(0.0, std::memory_order_relaxed);
    // Readers only touch the high-water mark when they see a running clock, and the snapshot store below
    // (release) orders this reset before that.
    m_presentedHighWater.store(std::numeric_limits<std::int64_t>::min(), std::memory_order_relaxed);

    m_live.store(true, std::memory_order_release);
    publish();
}

void RenderCore::goIdle(std::int64_t nowNs) {
    std::lock_guard lock(m_mutex); // holds off seek() so a seek cannot slip between the two lines below
    const Ticks position = clockAt(nowNs); // lock-free; includes a pending seek's target
    m_live.store(false, std::memory_order_release);
    // Freeze where the listener was (audio rendered but not yet heard is gone with the device), keeping the
    // play state, speed and loop for the next start().
    m_map.seek(position, m_deviceFrame);
    m_appliedSeekGeneration = m_requestValue.seekGeneration;
    m_modelValid = false;
    publish();
    m_retired.clear(); // no audio thread any more: nothing can still be using them
}

void RenderCore::setSource(std::shared_ptr<AudioSource> source) {
    std::lock_guard lock(m_mutex);
    std::shared_ptr<AudioSource> old = std::exchange(m_source, std::move(source));
    m_sourcePtr.store(m_source.get(), std::memory_order_seq_cst);
    const std::uint64_t counter = m_blockCounter.load(std::memory_order_seq_cst);
    if (old) {
        m_retired.push_back({std::move(old), counter});
    }
    collectRetiredLocked();
}

std::shared_ptr<AudioSource> RenderCore::source() const {
    std::lock_guard lock(m_mutex);
    return m_source;
}

void RenderCore::collectRetired() {
    std::lock_guard lock(m_mutex);
    collectRetiredLocked();
}

void RenderCore::collectRetiredLocked() {
    if (m_retired.empty()) {
        return;
    }
    const std::uint64_t now = m_blockCounter.load(std::memory_order_seq_cst);
    // Safe when the audio thread was outside a block at swap time (even), or has left that block since.
    std::erase_if(m_retired, [now](const Retired& r) { return (r.blockCounter & 1U) == 0 || r.blockCounter != now; });
}

void RenderCore::storeRequestLocked() {
    m_request.store(m_requestValue);
    if (!m_live.load(std::memory_order_acquire)) {
        // No audio thread: the control side (serialised by m_mutex) is the only writer of the transport.
        applyRequest(m_requestValue);
        publish();
    }
    collectRetiredLocked();
}

std::uint64_t RenderCore::seek(Ticks target, Ticks preroll) {
    std::lock_guard lock(m_mutex);
    m_requestValue.seekGeneration += 1;
    m_requestValue.seekTarget = target.value;
    m_requestValue.seekPreroll = std::max<std::int64_t>(preroll.value, 0);
    storeRequestLocked();
    return m_requestValue.seekGeneration;
}

void RenderCore::setPlaying(bool playing) {
    std::lock_guard lock(m_mutex);
    m_requestValue.playing = playing ? 1 : 0;
    storeRequestLocked();
}

bool RenderCore::playing() const {
    std::lock_guard lock(m_mutex);
    return m_requestValue.playing != 0;
}

void RenderCore::setSpeed(TransportSpeed speed) {
    if (!speed.isValid()) {
        return;
    }
    const TransportSpeed s = speed.normalized();
    std::lock_guard lock(m_mutex);
    m_requestValue.speedNum = s.num;
    m_requestValue.speedDen = s.den;
    storeRequestLocked();
}

TransportSpeed RenderCore::speed() const {
    std::lock_guard lock(m_mutex);
    return {m_requestValue.speedNum, m_requestValue.speedDen};
}

void RenderCore::setLoop(std::optional<TimeRange> range) {
    std::lock_guard lock(m_mutex);
    const bool active = range.has_value() && !range->isEmpty();
    m_requestValue.loopGeneration += 1;
    m_requestValue.loopStart = active ? range->start.value : 0;
    m_requestValue.loopEnd = active ? range->end().value : 0;
    storeRequestLocked();
}

std::optional<TimeRange> RenderCore::loop() const {
    std::lock_guard lock(m_mutex);
    if (m_requestValue.loopEnd <= m_requestValue.loopStart) {
        return std::nullopt;
    }
    return TimeRange::fromStartEnd(Ticks{m_requestValue.loopStart}, Ticks{m_requestValue.loopEnd});
}

// ---- Transport application (audio thread, or control side while idle) ---------------------------------------

void RenderCore::applyRequest(const TransportRequest& request) noexcept {
    const std::int64_t at = m_deviceFrame;
    // Speed and play state first, so a seek arriving together with resume() gets its preroll.
    const TransportSpeed speed{request.speedNum, request.speedDen};
    if (speed.isValid() && !(speed == m_map.playSpeed())) {
        m_map.setSpeed(speed, at);
    }
    const bool wantPlaying = request.playing != 0;
    if (wantPlaying && !m_map.isPlaying()) {
        m_map.play(at);
    } else if (!wantPlaying && m_map.isPlaying()) {
        m_map.pause(at);
    }
    if (request.loopGeneration != m_appliedLoopGeneration) {
        std::optional<TimeRange> range;
        if (request.loopEnd > request.loopStart) {
            range = TimeRange::fromStartEnd(Ticks{request.loopStart}, Ticks{request.loopEnd});
        }
        m_map.setLoop(range, at);
        m_appliedLoopGeneration = request.loopGeneration;
    }
    if (request.seekGeneration != m_appliedSeekGeneration) {
        const std::int64_t preroll =
            request.seekPreroll > 0
                ? timemath::mulDivSaturate(request.seekPreroll, 1, m_ticksPerFrame, Rounding::Ceil)
                : 0;
        m_map.seek(Ticks{request.seekTarget}, at, preroll);
        m_appliedSeekGeneration = request.seekGeneration;
    }
}

ClockSample RenderCore::makeClockSample(std::int64_t latencyFrames) const noexcept {
    ClockSample c;
    c.negotiatedRate = Rate{m_sampleRate, 1};
    c.latencyFrames = latencyFrames;
    c.deviceEpoch = m_map.deviceEpoch();
    c.quality = m_offsetFrames.load(std::memory_order_relaxed) != 0
                    ? LatencyQuality::UserCalibrated
                    : m_deviceLatencyQuality.load(std::memory_order_relaxed);
    c.running = m_modelValid && m_live.load(std::memory_order_relaxed);
    if (m_modelValid) {
        // Publish the DLL line as integers: a whole presented frame and the host time it is heard, which keeps
        // the sub-frame phase without floating point in the sample.
        const double whole = std::floor(m_baseFrame);
        const auto adjustNs = static_cast<std::int64_t>(
            std::llround((m_baseFrame - whole) * kNanosPerSecond / m_framesPerSecond));
        c.devicePositionFrames = static_cast<std::int64_t>(whole) + latencyFrames;
        c.hostTimeNs = m_baseNs - adjustNs;
    } else {
        c.devicePositionFrames = m_deviceFrame;
        c.hostTimeNs = 0;
    }
    return c;
}

void RenderCore::publish() noexcept {
    ClockSnapshot snap{};
    snap.clock = makeClockSample(m_latencyFrames);
    snap.map = m_map;
    snap.map.setClock(snap.clock);
    snap.capFrame = m_deviceFrame;
    snap.appliedSeekGeneration = m_appliedSeekGeneration;
    snap.live = m_live.load(std::memory_order_relaxed) ? 1 : 0;
    m_snapshot.store(snap);
}

// ---- Audio thread ------------------------------------------------------------------------------------

void RenderCore::process(float* out, std::uint32_t frames, std::int64_t nowNs) noexcept {
    const rt::ScopedRealtimeSection realtime; // audit hook: allocations / CheckedMutex locks here are violations
    const rt::ScopedDenormalsOff denormals;   // FTZ / DAZ: a filter ringing down in silence never costs denormal math
    m_blockCounter.fetch_add(1, std::memory_order_seq_cst); // enter block (odd), see "Source lifetime"
    AudioSource* const source = m_sourcePtr.load(std::memory_order_seq_cst);

    // 1. A new device epoch (reopen, switch, fallback, reroute): the stream's timing starts over. Device frames
    //    stay continuous in this engine, so the transport continues where rendering left off.
    const std::uint32_t epoch = m_deviceEpoch.load(std::memory_order_acquire);
    if (epoch != m_map.deviceEpoch()) {
        m_map.rebase(epoch, m_deviceFrame, m_deviceFrame);
        m_modelResetRequested.store(true, std::memory_order_relaxed);
    }

    // 2. Transport commands, applied at the block boundary. tryLoad never spins: if a control thread is in the
    //    middle of writing a request we apply it next block.
    TransportRequest request;
    if (m_request.tryLoad(request)) {
        applyRequest(request);
    }
    //    A seek's preroll lasts until the source has the target's audio, at most the preroll asked for (P.1: a
    //    fixed 100 ms of silence before every play from a new position).
    if (source != nullptr && m_map.prerollHolding(m_deviceFrame) && source->readyFor(m_appliedSeekGeneration)) {
        m_map.endPreroll(m_deviceFrame);
    }

    // 3. Render, piece by piece (a piece ends at a transport change or a loop wrap). Shuttle policy: the source
    //    is heard at normal speed (render) and at the audible shuttle speeds, 1/4x to 4x forward, through
    //    renderAtSpeed (time-stretched, if the source supports it); at any other speed (paused, prerolling, reverse,
    //    8x) the block is silent while the transport still advances at that speed.
    const auto channels = static_cast<std::size_t>(m_channels);
    const Rate rate{m_sampleRate, 1};
    std::uint32_t done = 0;
    while (done < frames) {
        const std::int64_t remaining = std::min<std::int64_t>(frames - done, m_maxBlockFrames);
        const TransportMap::Piece piece = m_map.piece(m_deviceFrame + done, remaining);
        const auto n = static_cast<std::uint32_t>(std::min<std::int64_t>(piece.frames, remaining));
        float* const chunk = out + static_cast<std::size_t>(done) * channels;
        const std::size_t samples = static_cast<std::size_t>(n) * channels;
        bool audible = false;
        if (source != nullptr && piece.speed.isNormal()) {
            source->render(std::span<float>(chunk, samples), m_channels, rate, piece.timeline);
            audible = true;
        } else if (source != nullptr && isAudibleShuttle(piece.speed)) {
            audible = source->renderAtSpeed(std::span<float>(chunk, samples), m_channels, rate, piece.timeline, piece.speed);
        }
        if (audible) {
            // Last line of defence for the device: never output NaN/inf, whatever the graph did.
            for (std::size_t i = 0; i < samples; ++i) {
                if (!std::isfinite(chunk[i])) {
                    chunk[i] = 0.0f;
                }
            }
        } else {
            std::fill_n(chunk, samples, 0.0f);
        }
        declickPiece(chunk, n, audible, source, piece.timeline, piece.speed);
        done += n;
    }

    // 4. Clock. The first frame of this block is heard `latency` frames after the frames already queued, so at
    //    callback time the frame being heard is m_deviceFrame - latency.
    const std::int64_t latency = m_deviceLatencyFrames.load(std::memory_order_relaxed) +
                                 m_offsetFrames.load(std::memory_order_relaxed);
    m_latencyFrames = latency;
    updateModel(static_cast<double>(m_deviceFrame - latency), nowNs, frames, latency);
    m_deviceFrame += frames;
    publish();

    // Statistics: single writer, so plain load+store instead of read-modify-write.
    m_callbacks.store(m_callbacks.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    m_framesRendered.store(m_framesRendered.load(std::memory_order_relaxed) + frames, std::memory_order_relaxed);
    m_lastFrames.store(frames, std::memory_order_relaxed);
    m_lastCallbackNs.store(nowNs, std::memory_order_relaxed);
    m_driftPpm.store((m_framesPerSecond / static_cast<double>(m_sampleRate) - 1.0) * 1e6, std::memory_order_relaxed);

    m_blockCounter.fetch_add(1, std::memory_order_seq_cst); // leave block (even)
}

// De-click. The transport cuts the waveform wherever it starts, stops or jumps: play from silence, pause,
// seek, the end of a preroll, a loop wrap, a source swap, a shuttle speed change. A step in the output there is
// an audible click, so over m_declickFrames (2 ms by default):
//   * audio that starts (or restarts at a discontinuity) fades in linearly, and
//   * the last value the device was given decays linearly to zero underneath whatever follows.
// The output is therefore continuous at every transport edge: its steps are bounded by |last| / frames plus the
// signal's own slope. Continuity is judged on the timeline: a piece that begins exactly where the previous
// audible piece ended, from the same source, is a continuation and passes untouched. Callback-only state.
void RenderCore::declickPiece(float* chunk, std::uint32_t frames, bool audible, AudioSource* source,
                              Ticks timeline, TransportSpeed speed) noexcept {
    if (frames == 0) {
        return;
    }
    const int fadeFrames = m_declickFrames;
    const auto channels = static_cast<std::size_t>(m_channels);
    if (fadeFrames > 0) {
        if (audible) {
            // A speed change is a seam too: the source switches between plain and time-stretched rendering.
            const bool continuous = m_prevAudible && source == m_prevSource && timeline.value == m_expectedTimeline &&
                                    speed == m_prevSpeed;
            if (!continuous) {
                if (m_prevAudible) { // a jump while sounding: the old waveform decays out under the new one
                    std::copy_n(m_lastOut, channels, m_tailFrom);
                    m_tailPos = 0;
                }
                m_fadeInPos = 0;
            }
        } else {
            if (m_prevAudible) {
                std::copy_n(m_lastOut, channels, m_tailFrom);
                m_tailPos = 0;
            }
            m_fadeInPos = -1;
        }
        const float denominator = static_cast<float>(fadeFrames + 1);
        for (std::uint32_t i = 0; i < frames && (m_fadeInPos >= 0 || m_tailPos >= 0); ++i) {
            float* const frame = chunk + static_cast<std::size_t>(i) * channels;
            if (m_fadeInPos >= 0) {
                const float g = static_cast<float>(m_fadeInPos + 1) / denominator;
                for (std::size_t c = 0; c < channels; ++c) {
                    frame[c] *= g;
                }
                if (++m_fadeInPos >= fadeFrames) {
                    m_fadeInPos = -1;
                }
            }
            if (m_tailPos >= 0) {
                const float g = 1.0f - static_cast<float>(m_tailPos + 1) / denominator;
                for (std::size_t c = 0; c < channels; ++c) {
                    frame[c] += m_tailFrom[c] * g;
                }
                if (++m_tailPos >= fadeFrames) {
                    m_tailPos = -1;
                }
            }
        }
    }
    if (audible) {
        const std::int64_t advance = timemath::mulDivSaturate(frames, m_ticksPerFrame, 1, Rounding::Floor);
        m_expectedTimeline =
            timemath::addSaturate(timeline.value, speed.isNormal() ? advance
                                                                   : timemath::mulDivSaturate(advance, speed.num, speed.den,
                                                                                              Rounding::Floor));
        m_prevSource = source;
        m_prevSpeed = speed;
    }
    m_prevAudible = audible;
    std::copy_n(chunk + static_cast<std::size_t>(frames - 1) * channels, channels, m_lastOut);
}

void RenderCore::resync(double observedFrame, std::int64_t nowNs) noexcept {
    m_baseFrame = observedFrame;
    m_baseNs = nowNs;
    m_framesPerSecond = static_cast<double>(m_sampleRate);
    m_modelValid = true;
    m_lateBlocks = 0;
    m_resyncs.store(m_resyncs.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

void RenderCore::updateModel(double observedFrame, std::int64_t nowNs, std::uint32_t frames,
                             std::int64_t latencyFrames) noexcept {
    const double blockFrames = static_cast<double>(frames);
    m_avgBlockFrames = m_avgBlockFrames <= 0.0 ? blockFrames : 0.9 * m_avgBlockFrames + 0.1 * blockFrames;

    const bool resetRequested = m_modelResetRequested.exchange(false, std::memory_order_acq_rel);
    if (!m_modelValid || resetRequested) {
        resync(observedFrame, nowNs);
        return;
    }

    const std::int64_t dtNs = std::max<std::int64_t>(nowNs - m_baseNs, 0);
    const double predicted = m_baseFrame + static_cast<double>(dtNs) * m_framesPerSecond / kNanosPerSecond;
    const double error = observedFrame - predicted;
    // Jitter tolerance: half a callback period, but at least 2 ms.
    const double tolerance = std::max(0.5 * m_avgBlockFrames, 0.002 * static_cast<double>(m_sampleRate));

    if (error > tolerance) {
        // The device consumed more than the model says (stream start-up burst, reroute, missed callbacks
        // without an underrun). Jump forward to the truth; readers never see time go backwards from this.
        resync(observedFrame, nowNs);
        return;
    }
    if (error < -tolerance) {
        // We are behind the model. Usually this is one late callback (the thread was woken late; the next one
        // is on time again), which must not disturb the clock, so hold the model's course. If it persists, or
        // is larger than everything that was queued, the device really underran or restarted: re-anchor
        // (readers then hold the clock until the new anchor catches up with what they have already shown).
        ++m_lateBlocks;
        const double hardLimit = static_cast<double>(latencyFrames) + 2.0 * m_avgBlockFrames;
        if (m_lateBlocks >= kLateBlocksBeforeResync || -error > hardLimit) {
            resync(observedFrame, nowNs);
        } else {
            m_baseFrame = predicted;
            m_baseNs = nowNs;
        }
        return;
    }

    // Normal case: second-order loop. Correct the phase by a fraction of the error and the rate by a smaller
    // fraction of the error per elapsed time.
    m_lateBlocks = 0;
    m_baseFrame = predicted + kPhaseGain * error;
    if (dtNs > 0) {
        const double nominal = static_cast<double>(m_sampleRate);
        m_framesPerSecond += kFrequencyGain * error * kNanosPerSecond / static_cast<double>(dtNs);
        m_framesPerSecond = std::clamp(m_framesPerSecond, nominal * (1.0 - kMaxRateDeviation),
                                       nominal * (1.0 + kMaxRateDeviation));
    }
    m_baseNs = nowNs;
}

// ---- Readers -------------------------------------------------------------------------------------------

Ticks RenderCore::clockAt(std::int64_t nowNs) const noexcept {
    const ClockSnapshot snap = m_snapshot.load();
    const TransportRequest request = m_request.load();

    // A seek the audio thread has not applied yet: report the target right away (the user expects the
    // playhead to land where they clicked; the clock then holds there until that audio is heard).
    if (request.seekGeneration != snap.appliedSeekGeneration) {
        return Ticks{request.seekTarget};
    }
    if (snap.live == 0 || !snap.clock.running) {
        return snap.map.transportTime(nowNs); // no clock yet (or stopped): the commanded position
    }

    // Presented device time, interpolated from the clock sample; times before the sample are evaluated as of
    // the sample, and nothing beyond the rendered audio can have been heard (a stalled device holds the clock).
    std::int64_t presented = snap.clock.presentedDeviceTicksAt(std::max(nowNs, snap.clock.hostTimeNs));
    const std::int64_t rendered = timemath::mulDivSaturate(snap.capFrame, snap.clock.negotiatedRate.ticksPerUnit(),
                                                           1, Rounding::Floor);
    presented = std::min(presented, rendered);

    // Monotonic guard across all readers. Relaxed ordering is enough: the CAS loop only needs atomicity of the
    // max itself, nothing else is published through it, and callers that need cross-thread monotonicity
    // already have a happens-before relation between their calls.
    std::int64_t highWater = m_presentedHighWater.load(std::memory_order_relaxed);
    while (presented > highWater &&
           !m_presentedHighWater.compare_exchange_weak(highWater, presented, std::memory_order_relaxed)) {
    }
    presented = std::max(presented, highWater);
    return snap.map.timelineAtDeviceTicks(presented);
}

} // namespace wabcut::audio::detail
