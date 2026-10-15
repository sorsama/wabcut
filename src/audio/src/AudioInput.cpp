#include <wabcut/audio/AudioInput.h>

#include "DeviceIds.h"

#include <wabcut/audio/RealTime.h>
#include <wabcut/audio/SeqLock.h>
#include <wabcut/audio/SpscRingBuffer.h>

#include <miniaudio.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <thread>

namespace wabcut::audio {

namespace {

std::int64_t steadyNs(std::chrono::steady_clock::time_point t) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

void captureCallback(ma_device* device, void* /*output*/, const void* input, ma_uint32 frames) {
    static_cast<AudioInput*>(device->pUserData)->onCapture(static_cast<const float*>(input), frames,
                                                           steadyNs(std::chrono::steady_clock::now()));
}

std::string describe(const char* what, ma_result result) { return std::string(what) + ": " + ma_result_description(result); }

/// Raises `target` to `value` if it is larger (lock-free).
void raise(std::atomic<float>& target, float value) noexcept {
    float current = target.load(std::memory_order_relaxed);
    while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

} // namespace

struct AudioInput::Impl {
    struct Native {
        ma_context context{};
        ma_device device{};
        bool contextReady = false;
        bool deviceReady = false;
        ~Native() {
            if (deviceReady) {
                ma_device_uninit(&device);
            }
            if (contextReady) {
                ma_context_uninit(&context);
            }
        }
    };

    mutable rt::CheckedMutex control; ///< control side only (start/stop/queries)
    AudioInputConfig config;
    bool running = false;
    std::string name;
    std::string backend;
    InputLatency latency;
    std::unique_ptr<Native> native;

    std::unique_ptr<SpscFrameRing> capture;
    std::unique_ptr<SpscFrameRing> monitor;          ///< owned; the capture callback writes it
    std::atomic<SpscFrameRing*> monitorRing{nullptr}; ///< what readMonitor() reads (the output callback)
    /// readMonitor() calls so far, twice each: odd while one runs. start() frees a replaced monitor ring only once the
    /// call that may hold it is over, as the engine retires sources after its render block (A5-P2-11).
    std::atomic<std::uint64_t> monitorReads{0};
    std::atomic<bool> monitorEnabled{false};
    std::atomic<bool> live{false}; ///< the callback may run (rings and line are set up)

    // Capture side (the callback's thread).
    std::int64_t captured = 0;
    std::int64_t ringed = 0; ///< frames that went into the capture ring
    std::int64_t minOffsetNs = std::numeric_limits<std::int64_t>::max();
    SeqLock<CaptureLine> line{CaptureLine{}};

    // Gaps in the capture ring (frames lost when it was full), published to the consumer so the frames after a gap
    // keep their stream index. Entry i: the ring position (frames written before it) and the frames lost there. Past
    // kMaxGaps the positions after later gaps run early by the frames lost (gapsOverflowed).
    static constexpr int kMaxGaps = 64;
    std::atomic<std::int64_t> gapAt[kMaxGaps]{};
    std::atomic<std::int64_t> gapLength[kMaxGaps]{};
    std::atomic<int> gapCount{0};

    // Consumer side.
    std::int64_t readFrames = 0; ///< frames read from the ring
    std::int64_t skipped = 0;    ///< frames lost before them
    int gapsApplied = 0;

    // Meters and counters (any thread).
    std::atomic<float> peak[2]{};
    std::atomic<double> sumSquares[2]{};
    std::atomic<std::uint64_t> meterFrames{0};
    std::atomic<bool> clipped{false};
    std::atomic<std::uint64_t> callbacks{0};
    std::atomic<std::uint64_t> framesCaptured{0};
    std::atomic<std::uint64_t> framesDropped{0};
    std::atomic<std::uint64_t> monitorDropped{0};
};

AudioInput::AudioInput() : m_impl(std::make_unique<Impl>()) {}

AudioInput::~AudioInput() { stop(); }

std::expected<std::vector<AudioDeviceInfo>, Error> AudioInput::enumerateInputDevices() {
    auto native = std::make_unique<Impl::Native>();
    ma_context_config contextConfig = ma_context_config_init();
    if (const ma_result r = ma_context_init(nullptr, 0, &contextConfig, &native->context); r != MA_SUCCESS) {
        return std::unexpected(Error(ErrorCode::DeviceError, describe("cannot open the audio system", r)));
    }
    native->contextReady = true;
    ma_device_info* playback = nullptr;
    ma_uint32 playbackCount = 0;
    ma_device_info* capture = nullptr;
    ma_uint32 captureCount = 0;
    if (const ma_result r = ma_context_get_devices(&native->context, &playback, &playbackCount, &capture, &captureCount);
        r != MA_SUCCESS) {
        return std::unexpected(Error(ErrorCode::DeviceError, describe("cannot list the input devices", r)));
    }
    std::vector<AudioDeviceInfo> devices;
    for (ma_uint32 i = 0; i < captureCount; ++i) {
        devices.push_back({detail::encodeDeviceId(native->context.backend, capture[i].id), detail::boundedString(capture[i].name),
                           capture[i].isDefault != 0});
    }
    return devices;
}

std::expected<void, Error> AudioInput::start(const AudioInputConfig& config) {
    std::lock_guard lock(m_impl->control);
    if (m_impl->running) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "the audio input is already running"));
    }
    if (config.sampleRate < 8000 || config.sampleRate > 384000 || config.channels < 1 || config.channels > 2 ||
        config.periodFrames < 16 || config.periodFrames > 16384 || config.ringSeconds <= 0.0 || config.monitorRingSeconds <= 0.0) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "invalid audio input configuration"));
    }
    if (config.backend == AudioBackend::NullRealtime) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "the input supports the Default and Null backends"));
    }
    Impl& d = *m_impl;
    d.config = config;
    d.capture = std::make_unique<SpscFrameRing>(static_cast<std::size_t>(config.ringSeconds * config.sampleRate), config.channels);
    // The output callback may still be inside readMonitor() with the previous monitor ring (the device switched during a
    // pass): the new one is published first, and the old one freed only once that call is over.
    auto monitor = std::make_unique<SpscFrameRing>(static_cast<std::size_t>(config.monitorRingSeconds * config.sampleRate), config.channels);
    d.monitorRing.store(monitor.get(), std::memory_order_seq_cst);
    const std::uint64_t reads = d.monitorReads.load(std::memory_order_seq_cst);
    if ((reads & 1U) != 0) {
        while (d.monitorReads.load(std::memory_order_acquire) == reads) {
            std::this_thread::yield();
        }
    }
    d.monitor = std::move(monitor);
    d.captured = 0;
    d.ringed = 0;
    d.readFrames = 0;
    d.skipped = 0;
    d.gapsApplied = 0;
    d.gapCount.store(0);
    d.minOffsetNs = std::numeric_limits<std::int64_t>::max();
    d.line.store(CaptureLine{false, 0, 0, config.sampleRate});
    for (int c = 0; c < 2; ++c) {
        d.peak[c].store(0.0f);
        d.sumSquares[c].store(0.0);
    }
    d.meterFrames.store(0);
    d.clipped.store(false);
    d.callbacks.store(0);
    d.framesCaptured.store(0);
    d.framesDropped.store(0);
    d.monitorDropped.store(0);

    if (config.backend == AudioBackend::Null) {
        d.name = "Null input";
        d.backend = "Null";
        d.latency = InputLatency{config.nullLatencyFrames, config.periodFrames,
                                 samplesToTicks(config.nullLatencyFrames, Rate{config.sampleRate, 1})};
        d.live.store(true, std::memory_order_release);
        d.running = true;
        return {};
    }

    auto native = std::make_unique<Impl::Native>();
    ma_context_config contextConfig = ma_context_config_init();
    if (const ma_result r = ma_context_init(nullptr, 0, &contextConfig, &native->context); r != MA_SUCCESS) {
        return std::unexpected(Error(ErrorCode::DeviceError, describe("cannot open the audio system", r)));
    }
    native->contextReady = true;
    std::optional<ma_device_id> id;
    if (!config.deviceId.empty()) {
        id = detail::findDevice(native->context, config.deviceId, ma_device_type_capture);
        if (!id) {
            return std::unexpected(Error(ErrorCode::NotFound, "the input device '" + config.deviceId + "' is not connected"));
        }
    }
    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_capture);
    deviceConfig.capture.format = ma_format_f32;
    deviceConfig.capture.channels = static_cast<ma_uint32>(config.channels);
    deviceConfig.capture.pDeviceID = id ? &*id : nullptr;
    deviceConfig.sampleRate = static_cast<ma_uint32>(config.sampleRate);
    deviceConfig.periodSizeInFrames = static_cast<ma_uint32>(config.periodFrames);
    deviceConfig.dataCallback = captureCallback;
    deviceConfig.pUserData = this;
    if (const ma_result r = ma_device_init(&native->context, &deviceConfig, &native->device); r != MA_SUCCESS) {
        return std::unexpected(Error(r == MA_NO_DEVICE ? ErrorCode::NotFound : ErrorCode::DeviceError,
                                     r == MA_NO_DEVICE ? std::string("there is no audio input device") : describe("cannot open the input", r)));
    }
    native->deviceReady = true;
    const ma_device& device = native->device;
    const double toEngine = static_cast<double>(config.sampleRate) / std::max<double>(device.capture.internalSampleRate, 1.0);
    const auto buffer = static_cast<std::int64_t>(
        std::llround(static_cast<double>(device.capture.internalPeriodSizeInFrames) * std::max<ma_uint32>(device.capture.internalPeriods, 1) * toEngine));
    d.latency = InputLatency{buffer, static_cast<std::int64_t>(std::llround(device.capture.internalPeriodSizeInFrames * toEngine)),
                             samplesToTicks(buffer, Rate{config.sampleRate, 1})};
    d.name = detail::boundedString(native->device.capture.name);
    d.backend = ma_get_backend_name(native->context.backend);
    d.live.store(true, std::memory_order_release);
    if (const ma_result r = ma_device_start(&native->device); r != MA_SUCCESS) {
        d.live.store(false, std::memory_order_release);
        return std::unexpected(Error(ErrorCode::DeviceError, describe("cannot start the input", r)));
    }
    d.native = std::move(native);
    d.running = true;
    return {};
}

void AudioInput::stop() noexcept {
    std::lock_guard lock(m_impl->control);
    Impl& d = *m_impl;
    if (!d.running) {
        return;
    }
    if (d.native) {
        ma_device_stop(&d.native->device);
    }
    d.live.store(false, std::memory_order_release);
    d.native.reset(); // uninit: no callback runs after this
    d.running = false;
    d.name.clear();
    d.backend.clear();
}

bool AudioInput::isRunning() const noexcept {
    std::lock_guard lock(m_impl->control);
    return m_impl->running;
}

AudioInputConfig AudioInput::config() const {
    std::lock_guard lock(m_impl->control);
    return m_impl->config;
}

std::string AudioInput::deviceName() const {
    std::lock_guard lock(m_impl->control);
    return m_impl->name;
}

std::string AudioInput::backendName() const {
    std::lock_guard lock(m_impl->control);
    return m_impl->backend;
}

InputLatency AudioInput::latency() const {
    std::lock_guard lock(m_impl->control);
    return m_impl->latency;
}

void AudioInput::onCapture(const float* interleaved, std::uint32_t frames, std::int64_t nowNs) noexcept {
    rt::ScopedRealtimeSection section;
    Impl& d = *m_impl;
    if (!d.live.load(std::memory_order_acquire) || interleaved == nullptr || frames == 0) {
        return;
    }
    const auto ch = static_cast<std::size_t>(d.config.channels);
    const std::span<const float> block(interleaved, static_cast<std::size_t>(frames) * ch);
    const std::size_t written = d.capture->write(block);
    d.ringed += static_cast<std::int64_t>(written);
    if (written < frames) {
        const auto lost = static_cast<std::int64_t>(frames - written);
        d.framesDropped.fetch_add(static_cast<std::uint64_t>(lost), std::memory_order_relaxed);
        const int g = d.gapCount.load(std::memory_order_relaxed);
        if (g < Impl::kMaxGaps) {
            d.gapAt[g].store(d.ringed, std::memory_order_relaxed);
            d.gapLength[g].store(lost, std::memory_order_relaxed);
            d.gapCount.store(g + 1, std::memory_order_release);
        }
    }
    if (d.monitorEnabled.load(std::memory_order_relaxed)) {
        const std::size_t monitored = d.monitor->write(block);
        if (monitored < frames) {
            d.monitorDropped.fetch_add(frames - monitored, std::memory_order_relaxed);
        }
    }
    // Meters.
    for (std::size_t c = 0; c < ch && c < 2; ++c) {
        float blockPeak = 0.0f;
        double sum = 0.0;
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float v = interleaved[static_cast<std::size_t>(i) * ch + c];
            blockPeak = std::max(blockPeak, std::fabs(v));
            sum += static_cast<double>(v) * v;
        }
        raise(d.peak[c], blockPeak);
        d.sumSquares[c].fetch_add(sum, std::memory_order_relaxed);
        if (blockPeak >= 0.999f) {
            d.clipped.store(true, std::memory_order_relaxed);
        }
    }
    d.meterFrames.fetch_add(frames, std::memory_order_relaxed);
    // The clock line: the block ends now; the earliest arrival over the session is the line (callbacks are late, never early).
    const std::int64_t end = d.captured + frames;
    const auto offset = nowNs - static_cast<std::int64_t>(static_cast<double>(end) * 1e9 / static_cast<double>(d.config.sampleRate));
    d.minOffsetNs = std::min(d.minOffsetNs, offset);
    d.captured = end;
    d.line.store(CaptureLine{true, d.minOffsetNs, end, d.config.sampleRate});
    d.callbacks.fetch_add(1, std::memory_order_relaxed);
    d.framesCaptured.fetch_add(frames, std::memory_order_relaxed);
}

std::expected<void, Error> AudioInput::push(std::span<const float> interleaved, std::chrono::steady_clock::time_point now) noexcept {
    Impl& d = *m_impl;
    if (!d.live.load(std::memory_order_acquire) || d.config.backend != AudioBackend::Null) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "push() is for a running Null input"));
    }
    const auto ch = static_cast<std::size_t>(d.config.channels);
    if (interleaved.size() % ch != 0) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "push() needs whole frames"));
    }
    onCapture(interleaved.data(), static_cast<std::uint32_t>(interleaved.size() / ch), steadyNs(now));
    return {};
}

std::size_t AudioInput::read(std::span<float> interleaved) noexcept {
    Impl& d = *m_impl;
    if (!d.capture) {
        return 0;
    }
    // The gaps reached: the frames from here on come after them in the stream. A read stops at the next gap.
    const int gaps = d.gapCount.load(std::memory_order_acquire);
    while (d.gapsApplied < gaps && d.gapAt[d.gapsApplied].load(std::memory_order_relaxed) <= d.readFrames) {
        d.skipped += d.gapLength[d.gapsApplied].load(std::memory_order_relaxed);
        ++d.gapsApplied;
    }
    const auto ch = static_cast<std::size_t>(d.config.channels);
    std::size_t room = interleaved.size() / ch;
    if (d.gapsApplied < gaps) {
        room = std::min(room, static_cast<std::size_t>(d.gapAt[d.gapsApplied].load(std::memory_order_relaxed) - d.readFrames));
    }
    const std::size_t n = d.capture->read(interleaved.first(room * ch));
    d.readFrames += static_cast<std::int64_t>(n);
    return n;
}

std::size_t AudioInput::readable() const noexcept { return m_impl->capture ? m_impl->capture->readableFrames() : 0; }

std::int64_t AudioInput::readPosition() const noexcept { return m_impl->readFrames + m_impl->skipped; }

std::size_t AudioInput::readMonitor(std::span<float> interleaved, std::size_t keepFrames) noexcept {
    Impl& d = *m_impl;
    // Odd from before the ring is looked up until after its last use: start() waits for that before freeing it.
    d.monitorReads.fetch_add(1, std::memory_order_seq_cst);
    SpscFrameRing* ring = d.monitorRing.load(std::memory_order_seq_cst);
    std::size_t n = 0;
    if (ring != nullptr && d.live.load(std::memory_order_acquire)) {
        const auto ch = static_cast<std::size_t>(ring->channels());
        const std::size_t wanted = interleaved.size() / ch;
        std::size_t available = ring->readableFrames();
        // Keep the backlog to `keepFrames` past this call: drop the oldest (the span is the scratch space).
        while (available > wanted + keepFrames && wanted > 0) {
            const std::size_t drop = std::min(available - (wanted + keepFrames), wanted);
            const std::size_t dropped = ring->read(interleaved.first(drop * ch));
            d.monitorDropped.fetch_add(dropped, std::memory_order_relaxed);
            available -= dropped;
            if (dropped == 0) {
                break;
            }
        }
        n = ring->read(interleaved);
    }
    d.monitorReads.fetch_add(1, std::memory_order_release);
    return n;
}

void AudioInput::setMonitorEnabled(bool enabled) noexcept { m_impl->monitorEnabled.store(enabled, std::memory_order_relaxed); }

CaptureLine AudioInput::captureLine() const noexcept { return m_impl->line.load(); }

InputLevels AudioInput::levels() noexcept {
    Impl& d = *m_impl;
    InputLevels out;
    const auto frames = d.meterFrames.exchange(0, std::memory_order_relaxed);
    for (int c = 0; c < 2; ++c) {
        out.peak[c] = d.peak[c].exchange(0.0f, std::memory_order_relaxed);
        const double sum = d.sumSquares[c].exchange(0.0, std::memory_order_relaxed);
        out.rms[c] = frames > 0 ? static_cast<float>(std::sqrt(sum / static_cast<double>(frames))) : 0.0f;
    }
    out.clipped = d.clipped.exchange(false, std::memory_order_relaxed);
    return out;
}

AudioInputStats AudioInput::stats() const noexcept {
    const Impl& d = *m_impl;
    return AudioInputStats{d.callbacks.load(std::memory_order_relaxed), d.framesCaptured.load(std::memory_order_relaxed),
                           d.framesDropped.load(std::memory_order_relaxed), d.monitorDropped.load(std::memory_order_relaxed)};
}

} // namespace wabcut::audio
