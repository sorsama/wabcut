#include <wabcut/audio/AudioEngine.h>

#include "DeviceIo.h"
#include "RenderCore.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <tuple>

namespace wabcut::audio {

namespace {

using namespace std::chrono_literals;

std::int64_t toNs(std::chrono::steady_clock::time_point t) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

std::int64_t steadyNowNs() noexcept { return toNs(std::chrono::steady_clock::now()); }

constexpr std::int64_t kNs = 1'000'000'000;

/// Nearest whole frame (half away from zero).
std::int64_t ticksToFramesRounded(Ticks t, int sampleRate) noexcept {
    return timemath::mulDivSaturate(t.value, 1, Rate{sampleRate, 1}.ticksPerUnit(), Rounding::Nearest);
}

std::expected<void, Error> validate(const AudioDeviceConfig& c) {
    auto bad = [](std::string message) { return std::unexpected(Error{ErrorCode::InvalidArgument, std::move(message)}); };
    if (c.sampleRate < 8000 || c.sampleRate > 768000 || !Rate{c.sampleRate, 1}.isExact()) {
        return bad("unsupported sample rate " + std::to_string(c.sampleRate));
    }
    if (c.channels < 1 || c.channels > 32) {
        return bad("unsupported channel count " + std::to_string(c.channels));
    }
    if (c.periodFrames < 16 || c.periodFrames > 16384) {
        return bad("period must be 16..16384 frames");
    }
    if (c.maxBlockFrames < 16 || c.maxBlockFrames > (1 << 20)) {
        return bad("maxBlockFrames must be 16..1048576");
    }
    if (c.nullLatencyFrames < 0) {
        return bad("nullLatencyFrames must not be negative");
    }
    if (c.declickTime < Ticks{} || c.declickTime > Ticks::fromSeconds(1)) {
        return bad("declickTime must be 0..1 s");
    }
    if (c.deviceTimeout < 50ms || c.deviceTimeout > 600s) {
        return bad("deviceTimeout must be 50 ms..600 s");
    }
    return {};
}

detail::DeviceOpenParams openParams(const AudioDeviceConfig& config) {
    detail::DeviceOpenParams p;
    p.sampleRate = config.sampleRate;
    p.channels = config.channels;
    p.periodFrames = config.periodFrames;
    p.nullBackend = config.backend == AudioBackend::NullRealtime;
    p.deviceId = config.deviceId;
    p.fallbackToNull = config.fallbackToNull;
    return p;
}

using Result = std::expected<void, Error>;

// =========================================================================================================
// Device supervisor: one thread that owns a DeviceIo, so every miniaudio context/device operation happens on
// that thread (see DeviceIo.h for why). It executes open/select/reopen commands from the control API and, every
// 50 ms, checks the device:
//   * stopped on its own (unplugged, invalidated, driver reset) for longer than a grace period -> reopen;
//     the grace is longer right after a device notification, so an OS-side reroute can finish first;
//   * started but no callback for max(1 s, 8 device buffers) -> hung stream, reopen;
//   * running on a substitute (default instead of the selected device, or the null device) -> look for the
//     preferred device every few seconds (with back-off) and move back when it reappears;
//   * name/latency changed after a reroute -> refresh the published status;
//   * release sources retired by setSource().
// Reopening never touches the RenderCore's transport or device frame counter, so the clock carries over.
//
// Bounded operations. miniaudio has waits without timeouts on this path (ma_device_stop waits for its worker
// thread, ma_device_start for the stream to start, WASAPI init for its command thread), and the OS audio stack
// can stall in them (device changes, a busy audio service, a virtual mixer holding the endpoint). So nothing
// waits for a supervisor without a deadline: a control call that runs out of time, or the engine's monitor
// seeing a backend call exceed it, *abandons* the supervisor. Its DeviceIo is disowned (its callbacks render
// silence and never touch the RenderCore again), its thread is detached and ends whenever the backend returns,
// and everything it uses stays alive through shared ownership. A fresh supervisor then opens the configured
// chain again (selected -> default -> null); if that stalls as well it runs on the null device, so the editor
// keeps a clock, and probes for the real device with a back-off that grows with every wedge.
// =========================================================================================================
class Supervisor {
public:
    enum class CommandKind { Open, Select, Reopen };
    using Promise = std::promise<Result>;

    /// How a supervisor starts (see launch()).
    struct Options {
        /// start() semantics: a failed initial open ends the thread (the caller reports the error). Otherwise
        /// (recovery) the thread keeps retrying once a second.
        bool exitOnOpenFailure = true;
        /// Set when this supervisor replaces a wedged one: reported through deviceError() until the next
        /// successful device change.
        std::string wedgeNote;
        /// Set when the replacement had to fall back to the null device because the configured devices stall:
        /// the configuration to move back to once a probe opens it.
        std::optional<detail::DeviceOpenParams> returnTo;
        /// First interval for probing the preferred device while on a substitute (grows with back-off).
        std::int64_t probeIntervalNs = 2 * kNs;
    };

    Supervisor(std::shared_ptr<detail::RenderCore> core, detail::DeviceOpenParams params, Options options)
        : m_core(core), m_params(std::move(params)), m_options(std::move(options)),
          m_io(std::make_unique<detail::DeviceIo>(std::move(core))) {}

    /// Starts a supervisor thread with an initial Open command; the future answers it.
    static std::pair<std::shared_ptr<Supervisor>, std::future<Result>> launch(std::shared_ptr<detail::RenderCore> core,
                                                                              detail::DeviceOpenParams params,
                                                                              Options options) {
        auto sup = std::make_shared<Supervisor>(std::move(core), std::move(params), std::move(options));
        auto future = sup->post(CommandKind::Open, {});
        sup->m_thread = std::thread([sup] { sup->run(); }); // the thread keeps its supervisor alive
        return {std::move(sup), std::move(future)};
    }

    std::future<Result> post(CommandKind kind, std::string deviceId) {
        auto promise = std::make_shared<Promise>();
        auto future = promise->get_future();
        {
            std::lock_guard lock(m_mutex);
            m_commands.push_back({kind, std::move(deviceId), std::move(promise)});
        }
        m_wake.notify_all();
        return future;
    }

    /// Asks the thread to close the device and end; true if it did within `timeout` (it is then joined).
    bool shutDown(std::chrono::milliseconds timeout) {
        {
            std::lock_guard lock(m_mutex);
            m_quit = true;
        }
        m_wake.notify_all();
        {
            std::unique_lock lock(m_mutex);
            if (!m_exitedCv.wait_for(lock, timeout, [this] { return m_exited; })) {
                return false;
            }
        }
        join();
        return true;
    }

    /// Joins a thread that ends by itself (start() after a failed open).
    void join() {
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    /// Disowns the device and the thread (see the banner). The thread may run on, but it never touches the
    /// engine's RenderCore again and ends as soon as the backend lets it.
    void abandon() {
        (void)m_io->abandon(500ms);
        {
            std::lock_guard lock(m_mutex);
            m_quit = true;
        }
        m_wake.notify_all();
        if (m_thread.joinable()) {
            m_thread.detach();
        }
    }

    /// Steady time (ns) since which the thread has been inside one backend operation; 0 when it is not.
    std::int64_t busySinceNs() const noexcept { return m_busySince.load(std::memory_order_relaxed); }
    /// The backend operation in progress ("" when none), for error messages.
    const char* busyWith() const noexcept { return m_io->busyWith(); }

    bool status(detail::DeviceStatus& out) const {
        std::lock_guard lock(m_mutex);
        if (m_haveStatus) {
            out = m_status;
        }
        return m_haveStatus;
    }
    std::uint64_t reopens() const noexcept { return m_reopens.load(std::memory_order_relaxed); }
    bool usingFallback() const noexcept { return m_usingFallback.load(std::memory_order_relaxed); }

private:
    struct Command {
        CommandKind kind;
        std::string deviceId;
        std::shared_ptr<Promise> done; // shared: a caller that timed out may have dropped its future
    };

    /// Supervisor thread. Publishes `s` with the wedge note (if any) in front of its fallback reason.
    void publishStatus(detail::DeviceStatus s) {
        const std::string& note = m_options.wedgeNote;
        if (!note.empty()) {
            s.fallbackReason = s.fallbackReason.empty() ? note : note + "; " + s.fallbackReason;
        }
        std::lock_guard lock(m_mutex);
        m_status = std::move(s);
        m_haveStatus = true;
    }

    /// Runs one backend operation, timed for the engine's monitor.
    template <class Fn>
    auto timed(Fn&& fn) {
        m_busySince.store(steadyNowNs(), std::memory_order_relaxed);
        auto result = fn();
        m_busySince.store(0, std::memory_order_relaxed);
        return result;
    }

    /// Supervisor thread: an explicit request for a device (select, reopen) leaves a forced null fallback.
    void leaveForcedFallback() {
        if (m_options.returnTo) {
            m_params = *m_options.returnTo;
            m_options.returnTo.reset();
        }
    }

    void run();

    std::shared_ptr<detail::RenderCore> m_core;
    detail::DeviceOpenParams m_params; // supervisor thread only (after launch)
    Options m_options;                 // supervisor thread only (after launch)
    std::unique_ptr<detail::DeviceIo> m_io;

    mutable std::mutex m_mutex; // guards the fields below
    std::condition_variable m_wake;
    std::condition_variable m_exitedCv;
    std::deque<Command> m_commands;
    bool m_quit = false;
    bool m_exited = false;
    detail::DeviceStatus m_status;
    bool m_haveStatus = false;

    std::atomic<std::uint64_t> m_reopens{0};
    std::atomic<bool> m_usingFallback{false};
    std::atomic<std::int64_t> m_busySince{0};
    std::thread m_thread;
};

void Supervisor::run() {
    detail::DeviceIo& io = *m_io;
    std::int64_t openedAtNs = 0;
    std::int64_t notStartedSinceNs = 0;
    std::int64_t lastNotificationNs = 0;
    std::uint64_t seenNotifications = 0;
    bool statusDirty = false;
    bool onPreferred = true;
    std::string fallbackReason;
    std::int64_t lastRetryNs = 0;
    std::int64_t nextProbeNs = 0;
    std::int64_t probeIntervalNs = m_options.probeIntervalNs;
    const std::int64_t probeCapNs = std::max(30 * kNs, m_options.probeIntervalNs);

    auto openDevice = [&](bool isReopen) -> Result {
        auto opened = timed([&] { return io.open(m_params); });
        const std::int64_t now = steadyNowNs();
        openedAtNs = now;
        notStartedSinceNs = 0;
        lastRetryNs = now;
        if (isReopen) {
            m_reopens.fetch_add(1, std::memory_order_relaxed);
        }
        if (!opened) {
            std::lock_guard lock(m_mutex);
            m_haveStatus = false;
            return std::unexpected(opened.error());
        }
        if (isReopen) {
            m_options.wedgeNote.clear(); // a later device change worked: the wedge is history
        }
        onPreferred = opened->onPreferredDevice && !m_options.returnTo;
        fallbackReason = opened->fallbackReason;
        m_usingFallback.store(!onPreferred, std::memory_order_relaxed);
        seenNotifications = io.notificationCount();
        statusDirty = false;
        nextProbeNs = now + probeIntervalNs;
        detail::DeviceStatus status = *opened;
        status.onPreferredDevice = onPreferred;
        publishStatus(std::move(status));
        return {};
    };

    for (;;) {
        std::optional<Command> command;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait_for(lock, 50ms, [this] { return m_quit || !m_commands.empty(); });
            if (m_quit) {
                break;
            }
            if (!m_commands.empty()) {
                command.emplace(std::move(m_commands.front()));
                m_commands.pop_front();
            }
        }

        if (command) {
            Result result;
            switch (command->kind) {
            case CommandKind::Open:
                result = openDevice(false);
                break;
            case CommandKind::Select:
                leaveForcedFallback();
                m_params.deviceId = command->deviceId;
                probeIntervalNs = 2 * kNs;
                result = openDevice(true);
                break;
            case CommandKind::Reopen:
                leaveForcedFallback();
                result = openDevice(true);
                break;
            }
            const bool openFailed = command->kind == CommandKind::Open && !result.has_value();
            command->done->set_value(std::move(result));
            if (openFailed && m_options.exitOnOpenFailure) {
                break; // start() joins us and reports the error
            }
            continue;
        }

        // ---- Periodic health check ----
        const std::int64_t now = steadyNowNs();
        m_core->collectRetired();

        if (!io.isOpen()) {
            // Nothing could be opened and the null fallback is disabled: keep trying once a second.
            if (now - lastRetryNs >= kNs) {
                (void)openDevice(true);
            }
            continue;
        }

        const std::uint64_t notifications = io.notificationCount();
        if (notifications != seenNotifications) {
            seenNotifications = notifications;
            lastNotificationNs = now;
            statusDirty = true;
        }
        if (statusDirty) {
            detail::DeviceStatus s;
            s.onPreferredDevice = onPreferred;
            s.fallbackReason = fallbackReason;
            if (io.readStatus(s)) {
                publishStatus(s);
                statusDirty = false;
            }
        }

        if (!io.isStarted()) {
            if (notStartedSinceNs == 0) {
                notStartedSinceNs = now;
            }
            const std::int64_t grace = (now - lastNotificationNs < 2 * kNs) ? 3 * kNs : kNs;
            if (now - notStartedSinceNs > grace) {
                (void)openDevice(true);
            }
            continue;
        }
        notStartedSinceNs = 0;

        const std::int64_t bufferNs = (io.bufferFrames() + io.periodFrames()) * kNs / std::max(m_params.sampleRate, 1);
        const std::int64_t stallNs = std::max<std::int64_t>(kNs, 8 * bufferNs);
        const std::int64_t lastActivity = std::max(m_core->lastCallbackNs(), openedAtNs);
        if (now - lastActivity > stallNs) {
            (void)openDevice(true);
            continue;
        }

        if (!onPreferred && now >= nextProbeNs) {
            // Test-open the preferred device on the side; only switch when that works, so a device that is
            // present but busy (e.g. held in exclusive mode by another app) never interrupts the fallback.
            const detail::DeviceOpenParams& preferred = m_options.returnTo ? *m_options.returnTo : m_params;
            if (timed([&] { return detail::DeviceIo::canOpen(preferred); })) {
                leaveForcedFallback();
                (void)openDevice(true);
            }
            if (!onPreferred) {
                probeIntervalNs = std::min(probeIntervalNs * 2, probeCapNs); // back off
            } else {
                probeIntervalNs = 2 * kNs;
            }
            nextProbeNs = now + probeIntervalNs;
        }
    }
    timed([&] {
        io.close();
        return 0;
    });
    std::lock_guard lock(m_mutex);
    m_exited = true;
    m_exitedCv.notify_all();
}

/// The engine's current device status (supervisor or Null backend), or nothing.
bool currentStatus(const std::shared_ptr<Supervisor>& sup, const detail::DeviceStatus& nullStatus,
                   detail::DeviceStatus& out) {
    if (sup) {
        return sup->status(out);
    }
    if (nullStatus.isNull) {
        out = nullStatus;
        return true;
    }
    return false;
}

} // namespace

// =========================================================================================================

struct AudioEngine::Impl {
    std::shared_ptr<detail::RenderCore> core = std::make_shared<detail::RenderCore>();

    /// Serialises start / stop / selectDevice / reopenDevice and wedge recovery. Timed, so the monitor never
    /// queues behind a control call (which handles its own timeout).
    std::timed_mutex lifecycleMutex;

    mutable std::mutex mutex; ///< Guards config, supervisor and nullStatus.
    AudioDeviceConfig config;
    std::shared_ptr<Supervisor> supervisor;
    detail::DeviceStatus nullStatus; ///< status of the Null backend, which has no supervisor

    std::atomic<bool> running{false};
    std::atomic<AudioBackend> backend{AudioBackend::Default};
    std::atomic<std::uint64_t> retiredReopens{0}; ///< reopens counted by supervisors no longer current
    std::atomic<std::uint64_t> wedges{0};

    // Monitor: notices a supervisor stuck in a backend call it made on its own (a self-healing reopen).
    std::thread monitor;
    std::mutex monitorMutex;
    std::condition_variable monitorWake;
    bool monitorQuit = false;

    std::shared_ptr<Supervisor> current() const {
        std::lock_guard lock(mutex);
        return supervisor;
    }

    std::chrono::milliseconds timeout() const {
        std::lock_guard lock(mutex);
        return config.deviceTimeout;
    }

    /// The supervisor's answer, or nothing if it did not come within `timeout`.
    static std::optional<Result> await(std::future<Result>& future, std::chrono::milliseconds timeout) {
        if (future.wait_for(timeout) != std::future_status::ready) {
            return std::nullopt;
        }
        return future.get();
    }

    /// Error text for a supervisor that did not answer within `limit`, naming the stuck stage when known.
    static std::string wedgeReason(const Supervisor& sup, const char* what, std::chrono::milliseconds limit) {
        const char* stage = sup.busyWith();
        return std::string("the audio device did not respond while ") + (*stage != '\0' ? stage : what) +
               " (no answer within " + std::to_string(limit.count()) + " ms)";
    }

    /// lifecycleMutex held; `sup` is the current supervisor and stuck in the backend. Abandons it and brings up
    /// a replacement: the configured device chain, then (if allowed and that stalls as well) the null device.
    /// Returns the reason, which also becomes the new device's fallback reason.
    std::string recover(const std::shared_ptr<Supervisor>& sup, const char* what) {
        const std::chrono::milliseconds limit = timeout();
        std::string reason = wedgeReason(*sup, what, limit);
        retiredReopens.fetch_add(sup->reopens(), std::memory_order_relaxed);
        wedges.fetch_add(1, std::memory_order_relaxed);
        sup->abandon();
        AudioDeviceConfig cfg;
        {
            std::lock_guard lock(mutex);
            cfg = config;
            supervisor.reset();
        }
        const detail::DeviceOpenParams params = openParams(cfg);
        std::shared_ptr<Supervisor> next;
        std::future<Result> opened;
        try {
            Supervisor::Options options;
            options.exitOnOpenFailure = false;
            options.wedgeNote = reason;
            std::tie(next, opened) = Supervisor::launch(core, params, options);
            if (!await(opened, limit) && cfg.fallbackToNull && !params.nullBackend) {
                // The replacement stalls too: keep a clock on the null device and probe the configured one
                // only now and then, backing off further with every wedge (each one leaves a thread behind).
                retiredReopens.fetch_add(next->reopens(), std::memory_order_relaxed);
                const std::uint64_t wedged = wedges.fetch_add(1, std::memory_order_relaxed) + 1;
                next->abandon();
                detail::DeviceOpenParams null = params;
                null.nullBackend = true;
                options.returnTo = params;
                options.probeIntervalNs =
                    std::min<std::int64_t>((2 * kNs) << std::min<std::uint64_t>(2 * wedged, 8), 300 * kNs);
                std::tie(next, opened) = Supervisor::launch(core, std::move(null), std::move(options));
                (void)await(opened, limit);
            }
        } catch (const std::system_error& e) {
            reason += std::string("; cannot start a new audio thread: ") + e.what();
            next.reset();
        }
        std::lock_guard lock(mutex);
        supervisor = next;
        return reason;
    }

    void monitorMain() {
        std::unique_lock lock(monitorMutex);
        while (!monitorQuit) {
            monitorWake.wait_for(lock, 250ms, [this] { return monitorQuit; });
            if (monitorQuit) {
                break;
            }
            lock.unlock();
            const auto sup = current();
            const std::int64_t since = sup ? sup->busySinceNs() : 0;
            const std::int64_t limitNs = static_cast<std::int64_t>(timeout().count()) * 1'000'000;
            // A control call holding the lifecycle lock handles its own timeout; only step in when it is free.
            if (since != 0 && steadyNowNs() - since > limitNs && lifecycleMutex.try_lock_for(50ms)) {
                std::unique_lock lifecycle(lifecycleMutex, std::adopt_lock);
                if (running.load(std::memory_order_acquire) && current() == sup && sup->busySinceNs() == since) {
                    (void)recover(sup, "recovering the device");
                }
            }
            lock.lock();
        }
    }

    void startMonitor() {
        {
            std::lock_guard lock(monitorMutex);
            monitorQuit = false;
        }
        monitor = std::thread([this] { monitorMain(); });
    }

    void stopMonitor() {
        {
            std::lock_guard lock(monitorMutex);
            monitorQuit = true;
        }
        monitorWake.notify_all();
        if (monitor.joinable()) {
            monitor.join();
        }
    }

    /// Snapshot for the status getters.
    bool status(detail::DeviceStatus& out) const {
        std::shared_ptr<Supervisor> sup;
        detail::DeviceStatus null;
        {
            std::lock_guard lock(mutex);
            sup = supervisor;
            null = nullStatus;
        }
        return currentStatus(sup, null, out);
    }
};

AudioEngine::AudioEngine() : m_impl(std::make_unique<Impl>()) {}

AudioEngine::~AudioEngine() { stop(); }

std::expected<std::vector<AudioDeviceInfo>, Error> AudioEngine::enumerateOutputDevices() {
    // Bounded like every other device operation: enumeration goes through the OS audio stack, which can stall.
    // A stuck enumeration is left to finish on its own thread.
    using Devices = std::expected<std::vector<AudioDeviceInfo>, Error>;
    auto promise = std::make_shared<std::promise<Devices>>();
    auto future = promise->get_future();
    try {
        std::thread([promise] { promise->set_value(detail::DeviceIo::enumerate()); }).detach();
    } catch (const std::system_error& e) {
        return std::unexpected(Error{ErrorCode::DeviceError, std::string("cannot start enumeration: ") + e.what()});
    }
    if (future.wait_for(AudioDeviceConfig{}.deviceTimeout) != std::future_status::ready) {
        return std::unexpected(Error{ErrorCode::DeviceError, "the audio system did not list its devices in time"});
    }
    return future.get();
}

std::expected<void, Error> AudioEngine::start(const AudioDeviceConfig& config) {
    Impl& d = *m_impl;
    std::lock_guard lifecycle(d.lifecycleMutex);
    if (d.running.load(std::memory_order_acquire)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "audio engine already running; stop() it first"});
    }
    if (auto valid = validate(config); !valid) {
        return valid;
    }
    {
        std::lock_guard lock(d.mutex);
        d.config = config;
        d.nullStatus = {};
        d.supervisor.reset();
    }
    d.backend.store(config.backend, std::memory_order_relaxed);
    d.retiredReopens.store(0, std::memory_order_relaxed);
    d.wedges.store(0, std::memory_order_relaxed);

    detail::RenderCore& core = *d.core;
    core.configure(config.channels, config.sampleRate, config.maxBlockFrames,
                   static_cast<int>(ticksToFramesRounded(config.declickTime, config.sampleRate)));
    core.setOffsetFrames(ticksToFramesRounded(config.latencyOffset, config.sampleRate));
    core.setDeviceLatencyFrames(config.backend == AudioBackend::Null ? config.nullLatencyFrames : 0);
    // Null: the simulated latency is exact. Device backends: DeviceIo reports it per stream.
    core.setDeviceLatencyQuality(config.backend == AudioBackend::Null ? LatencyQuality::Measured
                                                                      : LatencyQuality::Estimated);
    if (config.backend == AudioBackend::Null) {
        core.bumpDeviceEpoch(); // the Null "device" is new on every start (DeviceIo bumps it for real devices)
    }
    core.goLive(); // before any device callback can run

    if (config.backend == AudioBackend::Null) {
        detail::DeviceStatus s;
        s.name = "Null (manual pull)";
        s.backendName = "Null";
        s.bufferFrames = config.nullLatencyFrames;
        s.periodFrames = config.periodFrames;
        s.isNull = true;
        {
            std::lock_guard lock(d.mutex);
            d.nullStatus = s;
        }
        d.running.store(true, std::memory_order_release);
        return {};
    }

    std::shared_ptr<Supervisor> sup;
    std::future<Result> opened;
    try {
        std::tie(sup, opened) = Supervisor::launch(d.core, openParams(config), Supervisor::Options{});
    } catch (const std::system_error& e) {
        core.goIdle(steadyNowNs());
        return std::unexpected(Error{ErrorCode::DeviceError, std::string("cannot start the audio thread: ") + e.what()});
    }
    {
        std::lock_guard lock(d.mutex);
        d.supervisor = sup;
    }
    if (const auto result = Impl::await(opened, config.deviceTimeout); !result) {
        if (config.fallbackToNull) {
            // The backend did not answer in time: abandon that attempt and fall back (another try at the
            // configured device, then the null device). start() succeeds, like any other fallback.
            (void)d.recover(sup, "opening the device");
        }
        if (!config.fallbackToNull || !d.current()) {
            std::string reason = Impl::wedgeReason(*sup, "opening the device", config.deviceTimeout);
            if (d.current() == sup) {
                d.wedges.fetch_add(1, std::memory_order_relaxed);
                sup->abandon();
            }
            {
                std::lock_guard lock(d.mutex);
                d.supervisor.reset();
            }
            core.goIdle(steadyNowNs());
            return std::unexpected(Error{ErrorCode::DeviceError, std::move(reason)});
        }
    } else if (!*result) {
        sup->join(); // the thread ended after the failed open
        {
            std::lock_guard lock(d.mutex);
            d.supervisor.reset();
        }
        core.goIdle(steadyNowNs());
        return std::unexpected(result->error());
    }
    try {
        d.startMonitor();
    } catch (const std::system_error&) {
        // Without the monitor, spontaneous wedges are only caught by the next control call; still usable.
    }
    d.running.store(true, std::memory_order_release);
    return {};
}

void AudioEngine::stop() noexcept {
    Impl& d = *m_impl;
    std::lock_guard lifecycle(d.lifecycleMutex);
    if (!d.running.load(std::memory_order_acquire)) {
        return;
    }
    d.running.store(false, std::memory_order_release);
    d.stopMonitor(); // it may be waiting for the lifecycle lock: running == false makes it back off
    if (const auto sup = d.current()) {
        if (!sup->shutDown(d.timeout())) {
            // Closing hangs in the backend: disown it; none of its callbacks reaches the core after this.
            d.wedges.fetch_add(1, std::memory_order_relaxed);
            sup->abandon();
        }
        d.retiredReopens.fetch_add(sup->reopens(), std::memory_order_relaxed);
    }
    d.core->goIdle(steadyNowNs());
    std::lock_guard lock(d.mutex);
    d.supervisor.reset();
    d.nullStatus = {};
}

bool AudioEngine::isRunning() const noexcept { return m_impl->running.load(std::memory_order_acquire); }

AudioDeviceConfig AudioEngine::config() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->config;
}

std::string AudioEngine::deviceName() const {
    detail::DeviceStatus s;
    return m_impl->status(s) ? s.name : std::string{};
}

std::string AudioEngine::deviceId() const {
    detail::DeviceStatus s;
    return m_impl->status(s) ? s.id : std::string{};
}

std::string AudioEngine::deviceError() const {
    detail::DeviceStatus s;
    return m_impl->status(s) ? s.fallbackReason : std::string{};
}

std::string AudioEngine::backendName() const {
    detail::DeviceStatus s;
    return m_impl->status(s) ? s.backendName : std::string{};
}

std::expected<void, Error> AudioEngine::selectDevice(const std::string& deviceId) {
    Impl& d = *m_impl;
    std::lock_guard lifecycle(d.lifecycleMutex);
    {
        std::lock_guard lock(d.mutex);
        d.config.deviceId = deviceId;
    }
    if (!d.running.load(std::memory_order_acquire)) {
        return {};
    }
    if (d.backend.load(std::memory_order_relaxed) != AudioBackend::Default) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "device selection needs the Default backend"});
    }
    const auto sup = d.current();
    if (!sup) {
        return std::unexpected(Error{ErrorCode::DeviceError, "no audio device thread (an earlier recovery failed)"});
    }
    auto future = sup->post(Supervisor::CommandKind::Select, deviceId);
    if (auto result = Impl::await(future, d.timeout())) {
        return *result;
    }
    return std::unexpected(Error{ErrorCode::DeviceError, d.recover(sup, "switching the device")});
}

std::expected<void, Error> AudioEngine::reopenDevice() {
    Impl& d = *m_impl;
    std::lock_guard lifecycle(d.lifecycleMutex);
    if (!d.running.load(std::memory_order_acquire)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "audio engine is not running"});
    }
    if (d.backend.load(std::memory_order_relaxed) == AudioBackend::Null) {
        return {}; // no device to reopen
    }
    const auto sup = d.current();
    if (!sup) {
        return std::unexpected(Error{ErrorCode::DeviceError, "no audio device thread (an earlier recovery failed)"});
    }
    auto future = sup->post(Supervisor::CommandKind::Reopen, {});
    if (auto result = Impl::await(future, d.timeout())) {
        return *result;
    }
    return std::unexpected(Error{ErrorCode::DeviceError, d.recover(sup, "reopening the device")});
}

AudioLatency AudioEngine::latency() const {
    const Impl& d = *m_impl;
    AudioLatency l;
    int sampleRate = 48000;
    {
        std::lock_guard lock(d.mutex);
        sampleRate = d.config.sampleRate;
    }
    detail::DeviceStatus s;
    if (d.status(s)) {
        l.bufferFrames = s.bufferFrames;
        l.streamFrames = s.streamFrames;
        l.periodFrames = s.periodFrames;
    }
    l.offsetFrames = d.core->offsetFrames();
    l.totalFrames = l.bufferFrames + l.streamFrames + l.offsetFrames;
    l.total = samplesToTicks(l.totalFrames, Rate{sampleRate, 1});
    return l;
}

void AudioEngine::setLatencyOffset(Ticks offset) noexcept {
    Impl& d = *m_impl;
    std::lock_guard lock(d.mutex);
    d.config.latencyOffset = offset;
    d.core->setOffsetFrames(ticksToFramesRounded(offset, d.config.sampleRate));
}

void AudioEngine::setSource(std::shared_ptr<AudioSource> source) { m_impl->core->setSource(std::move(source)); }

std::shared_ptr<AudioSource> AudioEngine::source() const { return m_impl->core->source(); }

void AudioEngine::setOverlay(std::shared_ptr<AudioSource> overlay) { m_impl->core->setOverlay(std::move(overlay)); }

std::shared_ptr<AudioSource> AudioEngine::overlay() const { return m_impl->core->overlay(); }

void AudioEngine::setTimeline(Ticks start) { m_impl->core->seek(start, Ticks{}); }

std::uint64_t AudioEngine::seek(Ticks target, Ticks preroll) { return m_impl->core->seek(target, preroll); }

std::uint64_t AudioEngine::appliedSeekGeneration() const noexcept { return m_impl->core->appliedSeekGeneration(); }

void AudioEngine::pause() { m_impl->core->setPlaying(false); }

void AudioEngine::resume() { m_impl->core->setPlaying(true); }

bool AudioEngine::isPaused() const noexcept { return !m_impl->core->playing(); }

void AudioEngine::setSpeed(TransportSpeed speed) { m_impl->core->setSpeed(speed); }

TransportSpeed AudioEngine::speed() const { return m_impl->core->speed(); }

void AudioEngine::setLoop(std::optional<TimeRange> range) { m_impl->core->setLoop(range); }

std::optional<TimeRange> AudioEngine::loop() const { return m_impl->core->loop(); }

ClockSample AudioEngine::clockSample() const noexcept { return m_impl->core->clockSample(); }

TransportMap AudioEngine::transportMap() const noexcept { return m_impl->core->transportMap(); }

Ticks AudioEngine::clock() const noexcept { return m_impl->core->clockAt(steadyNowNs()); }

Ticks AudioEngine::clockAt(std::chrono::steady_clock::time_point now) const noexcept {
    return m_impl->core->clockAt(toNs(now));
}

std::expected<void, Error> AudioEngine::pull(std::span<float> interleaved,
                                             std::chrono::steady_clock::time_point now) noexcept {
    Impl& d = *m_impl;
    if (!d.running.load(std::memory_order_acquire) || d.backend.load(std::memory_order_relaxed) != AudioBackend::Null) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "pull() needs a running Null backend"});
    }
    const auto channels = static_cast<std::size_t>(d.core->channels());
    if (interleaved.size() % channels != 0) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "pull() size is not a whole number of frames"});
    }
    const std::size_t frames = interleaved.size() / channels;
    if (frames > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "pull() block too large"});
    }
    d.core->process(interleaved.data(), static_cast<std::uint32_t>(frames), toNs(now));
    return {};
}

AudioEngineStats AudioEngine::stats() const noexcept {
    const Impl& d = *m_impl;
    std::shared_ptr<Supervisor> sup;
    try {
        sup = d.current();
    } catch (...) {
        // std::mutex::lock only throws on system failure; report the core's counters without the device's.
    }
    AudioEngineStats s;
    s.callbacks = d.core->callbacks();
    s.framesRendered = d.core->framesRendered();
    s.deviceReopens = d.retiredReopens.load(std::memory_order_relaxed) + (sup ? sup->reopens() : 0);
    s.clockResyncs = d.core->clockResyncs();
    s.lastCallbackFrames = d.core->lastCallbackFrames();
    s.deviceEpoch = d.core->clockSample().deviceEpoch;
    s.driftPpm = d.core->driftPpm();
    s.usingFallbackDevice = sup && sup->usingFallback();
    s.deviceWedges = d.wedges.load(std::memory_order_relaxed);
    return s;
}

} // namespace wabcut::audio
