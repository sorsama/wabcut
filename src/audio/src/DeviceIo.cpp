#include "DeviceIo.h"

#include "DeviceIds.h" // encodeDeviceId, findDevice (shared with the capture device)

#include <miniaudio.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <utility>
#include <optional>
#include <thread>

// Helpers compiled into miniaudio_impl.c, where miniaudio's internal WASAPI wrappers are visible.
extern "C" {
ma_uint32 wabcut_ma_buffer_frames(const ma_device* pDevice);
ma_uint32 wabcut_ma_stream_latency_frames(ma_device* pDevice, int* measured);
}

namespace wabcut::audio::detail {

namespace {

std::int64_t steadyNowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string describe(const char* what, ma_result result) {
    return std::string(what) + ": " + ma_result_description(result);
}

// ---- miniaudio callbacks (trampolines) -------------------------------------------------------------------

void dataCallback(ma_device* device, void* output, const void* /*input*/, ma_uint32 frames) {
    static_cast<DeviceIo*>(device->pUserData)->onData(static_cast<float*>(output), frames);
}

void notificationCallback(const ma_device_notification* notification) {
    static_cast<DeviceIo*>(notification->pDevice->pUserData)
        ->onNotification(notification->pDevice, static_cast<int>(notification->type));
}

} // namespace

struct DeviceIo::Native {
    ma_context context{};
    bool contextReady = false;
    ma_device device{};
    bool deviceReady = false;
    ma_device_id selectedId{};
    bool isNull = false;

    Native() = default;
    Native(const Native&) = delete;
    Native& operator=(const Native&) = delete;
    ~Native() {
        if (deviceReady) {
            ma_device_uninit(&device); // stops the device and joins its thread: no callback runs after this
        }
        if (contextReady) {
            ma_context_uninit(&context);
        }
    }
};

namespace {
std::mutex g_hookMutex;
std::function<void(const char*)> g_hook;

void runHook(const char* operation) {
    std::function<void(const char*)> hook;
    {
        std::lock_guard lock(g_hookMutex);
        hook = g_hook;
    }
    if (hook) {
        hook(operation);
    }
}

/// Marks what the owning thread is doing in the backend for the lifetime of the scope.
class BusyScope {
public:
    BusyScope(std::atomic<const char*>& slot, const char* what) noexcept : m_slot(slot) {
        m_slot.store(what, std::memory_order_relaxed);
    }
    ~BusyScope() { m_slot.store("", std::memory_order_relaxed); }
    BusyScope(const BusyScope&) = delete;
    BusyScope& operator=(const BusyScope&) = delete;

private:
    std::atomic<const char*>& m_slot;
};
} // namespace

void setDeviceOperationHookForTest(std::function<void(const char*)> hook) {
    std::lock_guard lock(g_hookMutex);
    g_hook = std::move(hook);
}

DeviceIo::DeviceIo(std::shared_ptr<RenderCore> core) noexcept : m_core(std::move(core)) {}

DeviceIo::~DeviceIo() { close(); }

bool DeviceIo::abandon(std::chrono::milliseconds timeout) noexcept {
    // Dekker-style with onData (both seq_cst): either the callback sees m_abandoned and stays away from the
    // core, or this thread sees its m_inCallback and waits for it to leave.
    m_abandoned.store(true, std::memory_order_seq_cst);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (m_inCallback.load(std::memory_order_seq_cst) != 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

bool DeviceIo::isOpen() const noexcept { return m_native != nullptr; }

bool DeviceIo::isStarted() const noexcept {
    return m_native && ma_device_get_state(&m_native->device) == ma_device_state_started;
}

void DeviceIo::close() noexcept {
    if (!m_native) {
        return;
    }
    const BusyScope busy(m_busy, "closing the device");
    runHook("close");
    m_native.reset();
}

std::expected<DeviceStatus, Error> DeviceIo::open(const DeviceOpenParams& params) {
    close();
    const BusyScope busy(m_busy, "opening the device");
    runHook("open");
    if (isAbandoned()) {
        return std::unexpected(Error{ErrorCode::DeviceError, "device handler was abandoned"});
    }
    m_params = params;
    // Whatever opens below is a new device epoch: the audio thread rebases the transport and re-anchors the
    // clock at the first callback of the new stream.
    m_core->bumpDeviceEpoch();

    // Opens and starts `native.device` on `native.context`; on failure leaves nothing initialised.
    auto initAndStart = [this](Native& native, const ma_device_id* id, std::string& error) -> bool {
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = static_cast<ma_uint32>(m_params.channels);
        config.playback.pDeviceID = id;
        config.sampleRate = static_cast<ma_uint32>(m_params.sampleRate);
        config.periodSizeInFrames = static_cast<ma_uint32>(m_params.periodFrames);
        config.performanceProfile = ma_performance_profile_low_latency;
        config.noPreSilencedOutputBuffer = MA_TRUE; // RenderCore::process writes every sample
        config.noClip = MA_FALSE;                   // keep miniaudio's final hard clip as a backstop
        config.noFixedSizedCallback = MA_TRUE;      // no intermediary buffer: less latency, simpler latency maths
        config.dataCallback = dataCallback;
        config.notificationCallback = notificationCallback;
        config.pUserData = this;

        ma_result result = ma_device_init(&native.context, &config, &native.device);
        if (result != MA_SUCCESS) {
            error = describe("cannot open the output device", result);
            return false;
        }
        native.deviceReady = true;
        result = ma_device_start(&native.device);
        if (result != MA_SUCCESS) {
            ma_device_uninit(&native.device);
            native.deviceReady = false;
            error = describe("cannot start the output device", result);
            return false;
        }
        return true;
    };

    const std::uint64_t latencyUpdatesBefore = m_latencyUpdates.load(std::memory_order_acquire);
    std::string error;
    std::string fallbackReason;
    bool preferredMissing = false;
    bool onPreferred = true;

    if (!params.nullBackend) {
        auto native = std::make_unique<Native>();
        const ma_context_config contextConfig = ma_context_config_init();
        const ma_result result = ma_context_init(nullptr, 0, &contextConfig, &native->context);
        if (result == MA_SUCCESS) {
            native->contextReady = true;
            bool opened = false;
            if (!params.deviceId.empty()) {
                if (const auto id = findDevice(native->context, params.deviceId, ma_device_type_playback)) {
                    native->selectedId = *id;
                    opened = initAndStart(*native, &native->selectedId, error);
                } else {
                    preferredMissing = true;
                    error = "output device not found: " + params.deviceId;
                }
            }
            if (!opened) {
                if (!params.deviceId.empty()) {
                    fallbackReason = error;
                }
                onPreferred = params.deviceId.empty();
                opened = initAndStart(*native, nullptr, error);
            }
            if (opened) {
                m_native = std::move(native);
            }
        } else {
            error = describe("cannot initialise the audio backend", result);
        }
    }

    if (!m_native && (params.nullBackend || params.fallbackToNull)) {
        auto native = std::make_unique<Native>();
        const ma_backend backends[] = {ma_backend_null};
        const ma_context_config contextConfig = ma_context_config_init();
        std::string nullError;
        if (ma_context_init(backends, 1, &contextConfig, &native->context) == MA_SUCCESS) {
            native->contextReady = true;
            native->isNull = true;
            if (initAndStart(*native, nullptr, nullError)) {
                m_native = std::move(native);
                onPreferred = params.nullBackend;
                if (!params.nullBackend) {
                    fallbackReason = error.empty() ? "no output device could be opened" : error;
                }
            }
        }
        if (!m_native && error.empty()) {
            error = nullError.empty() ? "cannot initialise the null audio backend" : nullError;
        }
    }

    if (!m_native) {
        return std::unexpected(Error{preferredMissing && !params.fallbackToNull ? ErrorCode::NotFound
                                                                                : ErrorCode::DeviceError,
                                     error});
    }

    // The "started" notification computes the latency on miniaudio's thread; ma_device_start() can return a
    // moment before it has run, so give it a little time (it is normally already done).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (m_latencyUpdates.load(std::memory_order_acquire) == latencyUpdatesBefore &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    DeviceStatus status;
    for (int attempt = 0; attempt < 10 && !readStatus(status); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    status.onPreferredDevice = onPreferred;
    status.fallbackReason = fallbackReason;
    return status;
}

static_assert(sizeof(ma_device_id) <= sizeof(DeviceIo::RawStatus::id));
static_assert(sizeof(std::declval<ma_device&>().playback.name) <= sizeof(DeviceIo::RawStatus::name));

void DeviceIo::captureStatus(ma_device* device) noexcept {
    RawStatus raw;
    raw.backend = static_cast<std::int32_t>(device->pContext->backend);
    raw.isNull = device->pContext->backend == ma_backend_null ? 1 : 0;
    std::memcpy(raw.name, device->playback.name, sizeof(device->playback.name));
    raw.name[sizeof(raw.name) - 1] = '\0';
    std::memcpy(raw.id, &device->playback.id, sizeof(ma_device_id));
    publishStatus(raw);
}

void DeviceIo::publishStatus(RawStatus raw) noexcept {
    // SeqLock::store needs one writer at a time. Notifications can arrive on different miniaudio threads (a
    // reroute's thread, the worker), so writers take turns on a flag. This runs in notification callbacks,
    // never in the data callback, and the critical section is a ~600-byte copy.
    while (m_statusWriter.exchange(true, std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    raw.generation = ++m_statusGeneration;
    m_status.store(raw);
    m_statusWriter.store(false, std::memory_order_release);
}

bool DeviceIo::readStatus(DeviceStatus& status) const {
    const RawStatus raw = m_status.load(); // a consistent copy, whatever miniaudio is doing right now
    if (raw.generation == 0) {
        return false;
    }
    ma_device_id id{};
    std::memcpy(&id, raw.id, sizeof(ma_device_id));
    const auto backend = static_cast<ma_backend>(raw.backend);
    DeviceStatus s;
    s.name = boundedString(raw.name);
    s.id = raw.isNull != 0 ? std::string{} : encodeDeviceId(backend, id);
    s.backendName = ma_get_backend_name(backend);
    s.bufferFrames = bufferFrames();
    s.streamFrames = streamFrames();
    s.periodFrames = periodFrames();
    s.isNull = raw.isNull != 0;
    s.onPreferredDevice = status.onPreferredDevice;
    s.fallbackReason = status.fallbackReason;
    status = std::move(s);
    return true;
}

void DeviceIo::onData(float* out, std::uint32_t frames) noexcept {
    m_inCallback.fetch_add(1, std::memory_order_seq_cst);
    if (m_abandoned.load(std::memory_order_seq_cst)) {
        // Another device drives the core now; this one only plays silence until it is finally closed.
        std::fill_n(out, static_cast<std::size_t>(frames) * static_cast<std::size_t>(m_params.channels), 0.0f);
    } else {
        m_core->process(out, frames, steadyNowNs());
    }
    m_inCallback.fetch_sub(1, std::memory_order_seq_cst);
}

void DeviceIo::onNotification(ma_device* device, int type) noexcept {
    if (isAbandoned()) {
        return; // must not touch the core any more
    }
    switch (static_cast<ma_device_notification_type>(type)) {
    case ma_device_notification_type_started:
        // A (re)started stream: new timing, possibly a new device after a reroute. Status before latency: open()
        // waits for the latency update and then reads the status.
        captureStatus(device);
        updateLatency(device);
        m_core->requestModelReset();
        break;
    case ma_device_notification_type_rerouted:
        // The OS moved the stream to another device (e.g. the default changed): a new device epoch.
        captureStatus(device);
        m_reroutes.fetch_add(1, std::memory_order_release);
        m_core->bumpDeviceEpoch();
        break;
    default:
        break;
    }
    m_notifications.fetch_add(1, std::memory_order_release);
}

void DeviceIo::updateLatency(ma_device* device) noexcept {
    // Device frames -> engine frames (the device may run at another rate behind miniaudio's resampler).
    const std::int64_t engineRate = m_params.sampleRate;
    const std::int64_t deviceRate = device->playback.internalSampleRate != 0 ? device->playback.internalSampleRate
                                                                              : engineRate;
    auto toEngineFrames = [&](std::int64_t deviceFrames) {
        return timemath::mulDivSaturate(deviceFrames, engineRate, deviceRate, Rounding::Nearest);
    };

    // Frames queued ahead of a freshly rendered one: the device buffer, plus whatever the sample-rate converter
    // holds back. Stream latency: what the OS adds on top (WASAPI; elsewhere unknown, 0).
    const auto queued = static_cast<std::int64_t>(wabcut_ma_buffer_frames(device)) +
                        static_cast<std::int64_t>(ma_data_converter_get_output_latency(&device->playback.converter));
    int measured = 0;
    const auto stream = static_cast<std::int64_t>(wabcut_ma_stream_latency_frames(device, &measured));

    const std::int64_t bufferFrames = toEngineFrames(queued);
    const std::int64_t streamFrames = toEngineFrames(stream);
    m_bufferFrames.store(bufferFrames, std::memory_order_relaxed);
    m_streamFrames.store(streamFrames, std::memory_order_relaxed);
    m_periodFrames.store(toEngineFrames(device->playback.internalPeriodSizeInFrames), std::memory_order_relaxed);
    m_core->setDeviceLatencyQuality(measured != 0 ? LatencyQuality::Measured : LatencyQuality::Estimated);
    m_core->setDeviceLatencyFrames(bufferFrames + streamFrames);
    m_latencyUpdates.fetch_add(1, std::memory_order_release);
}

bool DeviceIo::canOpen(const DeviceOpenParams& params) {
    auto native = std::make_unique<Native>();
    const ma_context_config contextConfig = ma_context_config_init();
    if (ma_context_init(nullptr, 0, &contextConfig, &native->context) != MA_SUCCESS) {
        return false;
    }
    native->contextReady = true;
    const ma_device_id* id = nullptr;
    if (!params.deviceId.empty()) {
        const auto found = findDevice(native->context, params.deviceId, ma_device_type_playback);
        if (!found) {
            return false;
        }
        native->selectedId = *found;
        id = &native->selectedId;
    }
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = static_cast<ma_uint32>(params.channels);
    config.playback.pDeviceID = id;
    config.sampleRate = static_cast<ma_uint32>(params.sampleRate);
    config.periodSizeInFrames = static_cast<ma_uint32>(params.periodFrames);
    config.dataCallback = [](ma_device*, void* output, const void*, ma_uint32 frames) {
        // Never started; present only because miniaudio requires a callback.
        std::fill_n(static_cast<float*>(output), frames, 0.0f);
    };
    if (ma_device_init(&native->context, &config, &native->device) != MA_SUCCESS) {
        return false;
    }
    native->deviceReady = true;
    return true; // ~Native closes the probe device and its context
}

std::expected<std::vector<AudioDeviceInfo>, Error> DeviceIo::enumerate() {
    auto native = std::make_unique<Native>();
    const ma_context_config contextConfig = ma_context_config_init();
    ma_result result = ma_context_init(nullptr, 0, &contextConfig, &native->context);
    if (result != MA_SUCCESS) {
        return std::unexpected(Error{ErrorCode::DeviceError, describe("cannot initialise the audio backend", result)});
    }
    native->contextReady = true;

    ma_device_info* playback = nullptr;
    ma_uint32 count = 0;
    result = ma_context_get_devices(&native->context, &playback, &count, nullptr, nullptr);
    if (result != MA_SUCCESS) {
        return std::unexpected(Error{ErrorCode::DeviceError, describe("cannot enumerate output devices", result)});
    }
    std::vector<AudioDeviceInfo> devices;
    devices.reserve(count);
    for (ma_uint32 i = 0; i < count; ++i) {
        devices.push_back({encodeDeviceId(native->context.backend, playback[i].id), boundedString(playback[i].name),
                           playback[i].isDefault != 0});
    }
    return devices;
}

} // namespace wabcut::audio::detail
