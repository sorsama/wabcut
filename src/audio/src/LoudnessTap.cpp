#include <wabcut/audio/LoudnessTap.h>

#include <wabcut/audio/RealTime.h>
#include <wabcut/audio/dsp/Loudness.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace wabcut::audio {

namespace {

std::size_t ringFrames(int sampleRate, std::chrono::milliseconds length) {
    const auto ms = std::max<std::int64_t>(length.count(), 20);
    return static_cast<std::size_t>(std::max<std::int64_t>(static_cast<std::int64_t>(sampleRate) * ms / 1000, 1024));
}

} // namespace

struct LoudnessTap::Worker {
    Worker(int channels, int sampleRate) : meter(channels, sampleRate), buffer(4096 * static_cast<std::size_t>(channels)) {}

    dsp::LoudnessMeter meter;
    std::vector<float> buffer;
    std::uint64_t appliedResets = 0;

    rt::CheckedMutex lock; ///< guards `quit` (never taken on the audio thread)
    std::condition_variable_any wake;
    bool quit = false;
    std::thread thread;
};

LoudnessTap::LoudnessTap(int channels, int sampleRate, LoudnessTapOptions options)
    : m_channels(std::clamp(channels, 1, 6)), m_sampleRate(std::max(sampleRate, 1)), m_options(options),
      m_ring(ringFrames(m_sampleRate, options.ringLength), m_channels),
      m_worker(std::make_unique<Worker>(m_channels, m_sampleRate)) {
    m_reading.store(LoudnessReading{});
    if (m_options.workerThread) {
        m_worker->thread = std::thread([this] {
            Worker& w = *m_worker;
            std::unique_lock guard(w.lock);
            while (!w.quit) {
                w.wake.wait_for(guard, m_options.interval, [&] { return w.quit; });
                if (w.quit) {
                    break;
                }
                guard.unlock();
                drain();
                guard.lock();
            }
        });
    }
}

LoudnessTap::~LoudnessTap() {
    if (m_worker->thread.joinable()) {
        {
            std::lock_guard guard(m_worker->lock);
            m_worker->quit = true;
        }
        m_worker->wake.notify_all();
        m_worker->thread.join();
    }
}

void LoudnessTap::push(std::span<const float> interleaved) noexcept {
    if (!m_enabled.load(std::memory_order_relaxed)) {
        return;
    }
    const std::size_t frames = interleaved.size() / static_cast<std::size_t>(m_channels);
    const std::size_t written = m_ring.write(interleaved.first(frames * static_cast<std::size_t>(m_channels)));
    if (written < frames) {
        m_dropped.fetch_add(frames - written, std::memory_order_relaxed);
    }
}

void LoudnessTap::reset() noexcept { m_resetRequests.fetch_add(1, std::memory_order_release); }

LoudnessReading LoudnessTap::reading() const noexcept { return m_reading.load(); }

void LoudnessTap::setEnabled(bool enabled) noexcept { m_enabled.store(enabled, std::memory_order_relaxed); }

void LoudnessTap::update() {
    if (!m_options.workerThread) {
        drain();
    }
}

void LoudnessTap::drain() {
    Worker& w = *m_worker;
    const std::uint64_t resets = m_resetRequests.load(std::memory_order_acquire);
    if (resets != w.appliedResets) {
        // Audio pushed before the reset is not part of the new integration.
        m_ring.discardAll();
        w.meter.reset();
        w.appliedResets = resets;
    }
    const auto ch = static_cast<std::size_t>(m_channels);
    for (;;) {
        const std::size_t got = m_ring.read(std::span<float>(w.buffer.data(), w.buffer.size()));
        if (got == 0) {
            break;
        }
        w.meter.add(w.buffer.data(), got);
        if (got * ch < w.buffer.size()) {
            break;
        }
    }
    LoudnessReading r;
    r.momentaryLufs = w.meter.momentaryLufs();
    r.shortTermLufs = w.meter.shortTermLufs();
    r.integratedLufs = w.meter.integratedLufs();
    r.truePeakDbtp = w.meter.truePeakDbtp();
    r.maxMomentaryLufs = w.meter.maxMomentaryLufs();
    r.maxShortTermLufs = w.meter.maxShortTermLufs();
    r.frames = w.meter.frames();
    r.droppedFrames = m_dropped.load(std::memory_order_relaxed);
    r.resets = w.appliedResets;
    m_reading.store(r);
}

} // namespace wabcut::audio
