#pragma once

// Internal: the miniaudio playback device. The only code that knows about miniaudio types besides
// miniaudio_impl.c; nothing here is visible to users of wabcut_audio.

#include "RenderCore.h"

#include <wabcut/audio/SeqLock.h>

#include <wabcut/audio/AudioEngine.h>

#include <wabcut/model/Error.h>

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

struct ma_device; // miniaudio.h: typedef struct ma_device ma_device;

#include <chrono>
#include <functional>

namespace wabcut::audio::detail {

struct DeviceOpenParams {
    int sampleRate = 48000;
    int channels = 2;
    int periodFrames = 512;
    bool nullBackend = false;   ///< NullRealtime instead of the system backend.
    std::string deviceId;       ///< Preferred device ("" = system default).
    bool fallbackToNull = true; ///< System backend: use the null backend if nothing else opens.
};

/// Description of the open device. Latency figures are in engine frames.
struct DeviceStatus {
    std::string name;
    std::string id;
    std::string backendName;
    std::int64_t bufferFrames = 0;
    std::int64_t streamFrames = 0;
    std::int64_t periodFrames = 0;
    bool onPreferredDevice = true; ///< False when we fell back from the selected device (or to null).
    std::string fallbackReason;    ///< Why the preferred device is not in use (empty when it is).
    bool isNull = false;
};

/// DeviceIo::RawStatus. At namespace scope, not nested: SeqLock<RawStatus> checks that it is default
/// constructible inside DeviceIo, and GCC does not see a nested class's default member initializers until the
/// enclosing class is complete.
struct DeviceRawStatus {
    std::uint64_t generation = 0; ///< increments with every capture; 0 = nothing captured yet
    std::int32_t backend = 0;     ///< ma_backend
    std::uint32_t isNull = 0;
    char name[256]{};             ///< UTF-8, NUL-terminated
    unsigned char id[256]{};      ///< the ma_device_id bytes
};

/// Owns one miniaudio context + playback device and connects its callback to a RenderCore.
///
/// Threading: open()/close()/readStatus() are control-side and must all be called from one thread, the
/// engine's device supervisor thread. miniaudio pairs CoInitializeEx/CoUninitialize on whichever thread
/// creates and destroys a context, so opening on one thread and closing on another could unbalance COM on a
/// GUI thread. Keeping every device operation on one dedicated thread also gives all of them the same COM
/// apartment. The data callback runs on miniaudio's audio thread and only calls RenderCore::process().
/// The notification callback (miniaudio's audio or OS notification threads) only stores atomics: it must not
/// restart or uninit the device (miniaudio forbids it), and it must not allocate or lock either because it can
/// run on the audio thread. The watchdog polls those atomics.
class DeviceIo {
public:
    /// Shares ownership of the core: an abandoned DeviceIo (see abandon()) may outlive the engine.
    explicit DeviceIo(std::shared_ptr<RenderCore> core) noexcept;
    ~DeviceIo();

    DeviceIo(const DeviceIo&) = delete;
    DeviceIo& operator=(const DeviceIo&) = delete;

    /// Close any open device, then open and start one: the preferred device, else the system default, else
    /// (if allowed) the null backend. Returns a description of what was opened.
    std::expected<DeviceStatus, Error> open(const DeviceOpenParams& params);
    void close() noexcept;
    bool isOpen() const noexcept;
    /// True while miniaudio reports the device as started.
    bool isStarted() const noexcept;

    /// Name, id and latency of the open device, from the snapshot published by the device's own notifications
    /// (never from miniaudio's live fields, which a reroute rewrites on another thread). Returns false until a
    /// snapshot exists. Any thread.
    bool readStatus(DeviceStatus& status) const;

    /// Identity of the device as captured on miniaudio's thread: fixed-size, trivially copyable.
    using RawStatus = DeviceRawStatus;
    /// Publishes a status snapshot (single writer at a time; concurrent writers wait for each other). Called
    /// from the notification callback; public so tests can hammer it.
    void publishStatus(RawStatus status) noexcept;

    /// Incremented by every device notification (started, stopped, rerouted, interruption). Any thread.
    std::uint64_t notificationCount() const noexcept { return m_notifications.load(std::memory_order_acquire); }
    /// Incremented when the OS rerouted the stream to another device. Any thread.
    std::uint64_t rerouteCount() const noexcept { return m_reroutes.load(std::memory_order_acquire); }

    /// The latency the RenderCore currently uses (buffer + stream), in engine frames. Any thread.
    std::int64_t bufferFrames() const noexcept { return m_bufferFrames.load(std::memory_order_relaxed); }
    std::int64_t streamFrames() const noexcept { return m_streamFrames.load(std::memory_order_relaxed); }
    std::int64_t periodFrames() const noexcept { return m_periodFrames.load(std::memory_order_relaxed); }

    /// Output devices of the system backend (opens a temporary context).
    static std::expected<std::vector<AudioDeviceInfo>, Error> enumerate();

    /// True if the system backend could open `params.deviceId` (or the default device when empty) right now.
    /// Initialises a throw-away device on its own context without starting it, so the device that is currently
    /// playing is not interrupted. Call from the supervisor thread.
    static bool canOpen(const DeviceOpenParams& params);

    /// Disowns the device: from now on its data callback renders silence without touching the RenderCore, and
    /// its notifications are ignored. Waits (bounded) for a data callback in progress to finish, so after it
    /// returns the core may be driven by another device. Any thread; used when the supervisor thread that owns
    /// this DeviceIo is stuck inside the backend. Returns false if a callback was still running after `timeout`.
    bool abandon(std::chrono::milliseconds timeout) noexcept;
    bool isAbandoned() const noexcept { return m_abandoned.load(std::memory_order_seq_cst); }

    /// What the owning thread is doing in the backend right now ("" when idle), for timeout diagnostics.
    const char* busyWith() const noexcept { return m_busy.load(std::memory_order_relaxed); }

    // Called by the miniaudio callbacks (implementation detail; public for the C trampolines).
    void onData(float* out, std::uint32_t frames) noexcept;
    void onNotification(ma_device* device, int type) noexcept;

private:
    struct Native; // miniaudio context + device, defined in DeviceIo.cpp

    /// Recompute the latency of `device` and hand it to the RenderCore. Runs in the "started" notification,
    /// i.e. on miniaudio's thread once the (re)started stream's parameters are final.
    void updateLatency(ma_device* device) noexcept;
    /// Snapshot `device`'s name and id. Only from a notification at a point where miniaudio is not rewriting
    /// them: "started" (the worker thread, before its data loop; a reroute must stop the worker first) and
    /// "rerouted" (the rerouting thread itself, right after it rewrote them).
    void captureStatus(ma_device* device) noexcept;

    std::shared_ptr<RenderCore> m_core;
    std::atomic<bool> m_abandoned{false};
    std::atomic<int> m_inCallback{0};
    std::atomic<const char*> m_busy{""};
    std::unique_ptr<Native> m_native;
    DeviceOpenParams m_params;

    std::atomic<std::uint64_t> m_notifications{0};
    std::atomic<std::uint64_t> m_reroutes{0};
    std::atomic<std::int64_t> m_bufferFrames{0};
    std::atomic<std::int64_t> m_streamFrames{0};
    std::atomic<std::int64_t> m_periodFrames{0};
    std::atomic<std::uint64_t> m_latencyUpdates{0};
    SeqLock<RawStatus> m_status;
    std::atomic<bool> m_statusWriter{false};
    std::uint64_t m_statusGeneration = 0; ///< guarded by m_statusWriter
};

/// Test hook: called on the supervisor thread at the start of every backend operation ("open", "close") with
/// the operation's name; a test can block in it to simulate a backend call that never returns. Not for
/// production use. Pass an empty function to remove it.
void setDeviceOperationHookForTest(std::function<void(const char* operation)> hook);

} // namespace wabcut::audio::detail
