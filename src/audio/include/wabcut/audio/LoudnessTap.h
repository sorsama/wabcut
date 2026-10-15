#pragma once

// LoudnessTap (A.3): live EBU R128 / ITU-R BS.1770-4 loudness of an audio stream, measured off the audio thread.
//
//   audio thread   push(block): copies the block into a lock-free SPSC frame ring. No allocation, no lock, no
//                  system call; a full ring drops what does not fit and counts it (droppedFrames).
//   worker thread  every `interval` (or update() in manual mode): drains the ring into dsp::LoudnessMeter, which is
//                  libebur128 (K-weighting, 400 ms momentary and 3 s short-term windows, gated integration, 4x
//                  oversampled true peak, the channel map of EBU R128 s1), and publishes a LoudnessReading through a
//                  SeqLock.
//   any thread     reading(), reset(), setEnabled().
//
// So the live values are exactly what an offline libebur128 measurement of the same samples gives, up to the audio
// still in the ring when a reading is published (at most one interval).
//
// reset() starts the integration again (integrated loudness, maxima, true peak): audio pushed before it is dropped,
// the next audio the worker sees is the first counted.

#include <wabcut/audio/SeqLock.h>
#include <wabcut/audio/SpscRingBuffer.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>

namespace wabcut::audio {

struct LoudnessReading {
    double momentaryLufs = -std::numeric_limits<double>::infinity();   ///< last 400 ms
    double shortTermLufs = -std::numeric_limits<double>::infinity();   ///< last 3 s
    double integratedLufs = -std::numeric_limits<double>::infinity();  ///< gated, since the last reset
    double truePeakDbtp = -std::numeric_limits<double>::infinity();    ///< highest since the last reset
    double maxMomentaryLufs = -std::numeric_limits<double>::infinity();
    double maxShortTermLufs = -std::numeric_limits<double>::infinity();
    std::int64_t frames = 0;         ///< measured since the last reset
    std::uint64_t droppedFrames = 0; ///< pushed while the ring was full (never measured); since construction
    std::uint64_t resets = 0;        ///< resets applied so far (the reading belongs to the integration after them)
};

struct LoudnessTapOptions {
    /// Run the worker thread (production). false: nothing is measured until the owner calls update() (tests).
    bool workerThread = true;
    std::chrono::milliseconds interval{20};
    /// Ring capacity; what the worker may lag behind before blocks are dropped.
    std::chrono::milliseconds ringLength{1000};
};

class LoudnessTap {
public:
    /// `channels` 1..6 (L, R, C, LFE, Ls, Rs; one channel = dual mono). Allocates everything; starts the worker.
    LoudnessTap(int channels, int sampleRate, LoudnessTapOptions options = {});
    ~LoudnessTap(); ///< stops the worker
    LoudnessTap(const LoudnessTap&) = delete;
    LoudnessTap& operator=(const LoudnessTap&) = delete;

    int channels() const noexcept { return m_channels; }
    int sampleRate() const noexcept { return m_sampleRate; }

    /// Audio thread (one producer): `interleaved` holds channels() interleaved channels. RT-safe.
    void push(std::span<const float> interleaved) noexcept;

    /// Any thread.
    void reset() noexcept;
    LoudnessReading reading() const noexcept;
    /// Off: push() does nothing and the worker idles (readings keep their last values). On by default.
    void setEnabled(bool enabled) noexcept;
    bool enabled() const noexcept { return m_enabled.load(std::memory_order_relaxed); }

    /// Manual mode (workerThread false): measures what was pushed and publishes a reading now. Not on the audio thread.
    void update();

private:
    struct Worker;
    void drain();

    const int m_channels;
    const int m_sampleRate;
    const LoudnessTapOptions m_options;
    SpscFrameRing m_ring;
    SeqLock<LoudnessReading> m_reading;
    std::atomic<bool> m_enabled{true};
    std::atomic<std::uint64_t> m_resetRequests{0};
    std::atomic<std::uint64_t> m_dropped{0};
    std::unique_ptr<Worker> m_worker; ///< the meter, the drain buffer and (with workerThread) the thread
};

} // namespace wabcut::audio
