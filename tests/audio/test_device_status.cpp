// Device status snapshots (review P0-3): the supervisor reads name/id/latency from a SeqLock snapshot that the
// device's own notifications publish, never from miniaudio's live fields, which a WASAPI reroute rewrites on
// another thread.

#include "DeviceIo.h" // internal (tests/audio adds src/audio/src to the include path)
#include "RenderCore.h"

#include <wabcut/audio/AudioEngine.h>
#include <wabcut/audio/Sources.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace wabcut;
using namespace wabcut::audio;
using namespace std::chrono_literals;
namespace internal = wabcut::audio::detail; // not wabcut::detail (model/Time.h)

namespace {

/// A synthetic identity: name "Device <k>" and, as a WASAPI id (backend 0 = ma_backend_wasapi, the first
/// miniaudio backend), the UTF-16 string "id-<k>". A torn snapshot would pair one k's name with another's id.
internal::DeviceIo::RawStatus identity(int k) {
    internal::DeviceIo::RawStatus raw;
    raw.backend = 0;
    const std::string name = "Device " + std::to_string(k) + std::string(static_cast<std::size_t>(k % 97), '.');
    std::memcpy(raw.name, name.c_str(), name.size() + 1);
    const std::string id = "id-" + std::to_string(k);
    for (std::size_t i = 0; i < id.size(); ++i) { // UTF-16LE
        raw.id[2 * i] = static_cast<unsigned char>(id[i]);
        raw.id[2 * i + 1] = 0;
    }
    return raw;
}

} // namespace

TEST_CASE("Device status snapshots never tear while notifications publish and the supervisor reads",
          "[audio][device-status][stress]") {
    auto core = std::make_shared<internal::RenderCore>();
    core->configure(2, 48000, 4096, 0);
    internal::DeviceIo io(core);
    internal::DeviceStatus status;
    CHECK_FALSE(io.readStatus(status)); // nothing captured yet

    std::atomic<bool> done{false};
    std::atomic<int> torn{0};
    std::atomic<long long> reads{0};
    std::vector<std::thread> threads;
    for (int w = 0; w < 2; ++w) { // two "notification threads" (a reroute's and the worker's)
        threads.emplace_back([&, w] {
            for (int k = w; k < 40000; k += 2) {
                io.publishStatus(identity(k));
            }
        });
    }
    for (int r = 0; r < 2; ++r) { // supervisor-like readers
        threads.emplace_back([&] {
            internal::DeviceStatus s;
            while (!done.load()) {
                if (!io.readStatus(s)) {
                    continue;
                }
                // name "Device <k>..." must come with id "wasapi:id-<k>".
                const auto space = s.name.find(' ');
                const auto dots = s.name.find('.');
                const std::string k = s.name.substr(space + 1, dots == std::string::npos ? std::string::npos : dots - space - 1);
                if (s.id != "wasapi:id-" + k || s.backendName != "WASAPI") {
                    torn.fetch_add(1);
                }
                reads.fetch_add(1);
            }
        });
    }
    threads[0].join();
    threads[1].join();
    done.store(true);
    threads[2].join();
    threads[3].join();
    CHECK(torn.load() == 0);
    CHECK(reads.load() > 0);
    REQUIRE(io.readStatus(status));
    CHECK((status.name.rfind("Device 39999", 0) == 0 || status.name.rfind("Device 39998", 0) == 0));
}

namespace {

/// Outcome of hammerStatusDuringReopen: how many reopens succeeded, and why it stopped early (if it did).
struct HammerResult {
    int reopened = 0;
    std::optional<Error> failure;
};

/// Reopens the engine's device `reopens` times while two threads hammer every status accessor. Stops at the
/// first failed reopen; the readers are always joined (also when a check throws).
HammerResult hammerStatusDuringReopen(AudioEngine& engine, int reopens) {
    std::atomic<bool> done{false};
    std::atomic<int> bad{0};
    std::atomic<long long> reads{0};
    const std::string expectedName = engine.deviceName();
    REQUIRE_FALSE(expectedName.empty());
    HammerResult result;
    {
        std::vector<std::jthread> readers;
        struct Stop {
            std::atomic<bool>& done;
            ~Stop() { done.store(true); }
        } stop{done}; // destroyed before `readers`: they see `done` and are joined
        for (int r = 0; r < 2; ++r) {
            readers.emplace_back([&] {
                while (!done.load()) {
                    const std::string name = engine.deviceName();
                    const std::string backend = engine.backendName();
                    const AudioLatency latency = engine.latency();
                    const AudioEngineStats stats = engine.stats();
                    // During a reopen the status is briefly unknown (""); otherwise it is exactly the device's.
                    if (!name.empty() && name != expectedName) {
                        bad.fetch_add(1);
                    }
                    if (latency.totalFrames < 0 || stats.callbacks > (1ULL << 40)) {
                        bad.fetch_add(1);
                    }
                    (void)backend;
                    reads.fetch_add(1);
                }
            });
        }
        for (int i = 0; i < reopens; ++i) {
            if (auto reopened = engine.reopenDevice(); !reopened) {
                result.failure = reopened.error();
                break;
            }
            ++result.reopened;
        }
    }
    if (result.failure) {
        return result; // the caller decides (a wedged real device is a skip, anything else a failure)
    }
    CHECK(bad.load() == 0);
    CHECK(reads.load() > 0);
    CHECK(engine.stats().deviceReopens == static_cast<std::uint64_t>(reopens));
    CHECK(engine.deviceName() == expectedName);
    return result;
}

} // namespace

TEST_CASE("Status reads stay valid while the device is reopened over and over (null device)",
          "[audio][device-status][stress]") {
    AudioEngine engine;
    engine.setSource(std::make_shared<SilenceSource>());
    engine.resume();
    AudioDeviceConfig config;
    config.backend = AudioBackend::NullRealtime;
    REQUIRE(engine.start(config));
    const HammerResult result = hammerStatusDuringReopen(engine, 40);
    CHECK_FALSE(result.failure.has_value());
    engine.stop();
}

TEST_CASE("Status reads stay valid while the real default device is reopened", "[audio][device-status][device]") {
    // Bounded: every engine device operation gives up after deviceTimeout (the engine then abandons the device
    // and reports a DeviceError), so this test takes at most a few timeouts even when the OS audio stack stalls.
    const auto started = std::chrono::steady_clock::now();
    AudioEngine engine;
    engine.setSource(std::make_shared<SilenceSource>());
    engine.resume();
    AudioDeviceConfig config;
    config.fallbackToNull = false;
    config.deviceTimeout = 3s;
    if (auto start = engine.start(config); !start) {
        SKIP("no usable audio output device: " << start.error().message);
    }
    const HammerResult result = hammerStatusDuringReopen(engine, 8);
    const AudioEngineStats stats = engine.stats();
    engine.stop();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed < 60s);
    if (result.failure && stats.deviceWedges > 0) {
        SKIP("the real device could not be reopened in time (after " << result.reopened
                                                                     << " reopens): " << result.failure->message);
    }
    if (result.failure) {
        FAIL("reopen " << result.reopened + 1 << " failed: " << result.failure->message);
    }
}

// ---- Bounded device operations ---------------------------------------------------------------------------
// The engine's test hook runs at the start of every backend operation on the supervisor thread; blocking in it
// stands in for a miniaudio/OS call that never returns (the stalls seen on real WASAPI devices).

namespace {

/// Blocks the first `count` device operations named `operation` until released (or destroyed). Shared with the
/// hook, so a thread still blocked in it after the test (an abandoned supervisor) never touches freed memory.
class OperationGate {
public:
    struct State {
        std::mutex mutex;
        std::condition_variable cv;
        std::string operation;
        int remaining = 0;
        int entered = 0;
        bool released = false;
    };

    OperationGate(std::string operation, int count) : m_state(std::make_shared<State>()) {
        m_state->operation = std::move(operation);
        m_state->remaining = count;
        internal::setDeviceOperationHookForTest([state = m_state](const char* op) {
            std::unique_lock lock(state->mutex);
            if (state->released || state->remaining == 0 || state->operation != op) {
                return;
            }
            --state->remaining;
            ++state->entered;
            state->cv.notify_all();
            state->cv.wait(lock, [&] { return state->released; });
        });
    }
    ~OperationGate() {
        release();
        internal::setDeviceOperationHookForTest({});
    }
    OperationGate(const OperationGate&) = delete;
    OperationGate& operator=(const OperationGate&) = delete;

    void release() {
        {
            std::lock_guard lock(m_state->mutex);
            m_state->released = true;
        }
        m_state->cv.notify_all();
    }
    int entered() const {
        std::lock_guard lock(m_state->mutex);
        return m_state->entered;
    }

private:
    std::shared_ptr<State> m_state;
};

AudioDeviceConfig nullRealtimeConfig() {
    AudioDeviceConfig config;
    config.backend = AudioBackend::NullRealtime;
    config.deviceTimeout = 300ms;
    return config;
}

/// Waits (bounded) until the engine's device has rendered more callbacks than `after`.
bool callbacksAdvance(const AudioEngine& engine, std::uint64_t after) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (engine.stats().callbacks > after) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

using Clock = std::chrono::steady_clock;

} // namespace

TEST_CASE("start() abandons a device open that never returns and falls back", "[audio][device-status][timeout]") {
    OperationGate gate("open", 1);
    AudioEngine engine;
    engine.setSource(std::make_shared<SilenceSource>());
    engine.resume();
    const auto t0 = Clock::now();
    REQUIRE(engine.start(nullRealtimeConfig()));
    CHECK(Clock::now() - t0 < 3s);
    CHECK(gate.entered() == 1);
    CHECK(engine.isRunning());
    CHECK(engine.stats().deviceWedges == 1);
    CHECK_FALSE(engine.stats().usingFallbackDevice); // the second attempt got the configured device
    CHECK(engine.deviceError().find("did not respond while opening the device") != std::string::npos);
    CHECK(callbacksAdvance(engine, 0)); // the replacement device drives the clock
    gate.release();                     // the abandoned open returns now and its thread ends on its own
    const auto t1 = Clock::now();
    engine.stop();
    CHECK(Clock::now() - t1 < 3s);
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("start() without fallback reports a clean error when opening the device hangs",
          "[audio][device-status][timeout]") {
    OperationGate gate("open", 1);
    AudioEngine engine;
    AudioDeviceConfig config = nullRealtimeConfig();
    config.fallbackToNull = false;
    const auto t0 = Clock::now();
    const auto started = engine.start(config);
    CHECK(Clock::now() - t0 < 3s);
    REQUIRE_FALSE(started);
    CHECK(started.error().code == ErrorCode::DeviceError);
    CHECK(started.error().message.find("opening the device") != std::string::npos);
    CHECK_FALSE(engine.isRunning());
    gate.release();
    // Nothing is left behind: the engine starts normally once the backend answers again.
    REQUIRE(engine.start(config));
    CHECK(callbacksAdvance(engine, 0));
    engine.stop();
}

TEST_CASE("reopenDevice() that hangs while closing returns an error and the engine keeps playing",
          "[audio][device-status][timeout]") {
    AudioEngine engine;
    engine.setSource(std::make_shared<SilenceSource>());
    engine.resume();
    REQUIRE(engine.start(nullRealtimeConfig()));
    REQUIRE(callbacksAdvance(engine, 0));
    const std::uint32_t epochBefore = engine.stats().deviceEpoch;
    OperationGate gate("close", 1); // the reopen closes the old device first: that call never returns
    const auto t0 = Clock::now();
    const auto reopened = engine.reopenDevice();
    CHECK(Clock::now() - t0 < 3s);
    REQUIRE_FALSE(reopened);
    CHECK(reopened.error().code == ErrorCode::DeviceError);
    CHECK(reopened.error().message.find("closing the device") != std::string::npos);
    CHECK(gate.entered() == 1);
    CHECK(engine.isRunning());
    CHECK(engine.stats().deviceWedges == 1);
    // A fresh device took over: new epoch, callbacks keep coming, and the abandoned one stays silent.
    const std::uint64_t callbacks = engine.stats().callbacks;
    CHECK(callbacksAdvance(engine, callbacks));
    CHECK(engine.stats().deviceEpoch != epochBefore);
    CHECK_FALSE(engine.deviceName().empty());
    // Later operations work normally on the replacement.
    CHECK(engine.reopenDevice());
    gate.release();
    engine.stop();
}

TEST_CASE("stop() is bounded when closing the device hangs", "[audio][device-status][timeout]") {
    AudioEngine engine;
    engine.setSource(std::make_shared<SilenceSource>());
    engine.resume();
    REQUIRE(engine.start(nullRealtimeConfig()));
    REQUIRE(callbacksAdvance(engine, 0));
    {
        OperationGate gate("close", 1);
        const auto t0 = Clock::now();
        engine.stop();
        CHECK(Clock::now() - t0 < 3s);
        CHECK(gate.entered() == 1);
        CHECK_FALSE(engine.isRunning());
        CHECK(engine.stats().deviceWedges == 1);
        // After stop() the abandoned device no longer reaches the engine.
        const std::uint64_t callbacks = engine.stats().callbacks;
        std::this_thread::sleep_for(100ms);
        CHECK(engine.stats().callbacks == callbacks);
    }
    // The engine can be started again right away.
    REQUIRE(engine.start(nullRealtimeConfig()));
    CHECK(callbacksAdvance(engine, engine.stats().callbacks));
    engine.stop();
}

TEST_CASE("Device operation timeouts are validated", "[audio][device-status][timeout]") {
    AudioEngine engine;
    AudioDeviceConfig config = nullRealtimeConfig();
    config.deviceTimeout = 1ms;
    const auto started = engine.start(config);
    REQUIRE_FALSE(started);
    CHECK(started.error().code == ErrorCode::InvalidArgument);
}
