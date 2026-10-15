#include <wabcut/audio/AudioEngine.h>
#include <wabcut/audio/Sources.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <thread>
#include <vector>

#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h> // _mm_getcsr
#endif

using namespace wabcut;
using namespace wabcut::audio;
using namespace std::chrono_literals;

namespace {

AudioDeviceConfig nullConfig(int latencyFrames = 0) {
    AudioDeviceConfig c;
    c.backend = AudioBackend::Null;
    c.nullLatencyFrames = latencyFrames;
    return c;
}

AudioDeviceConfig exactConfig() {
    AudioDeviceConfig c = nullConfig();
    c.declickTime = Ticks{};
    return c;
}

} // namespace

TEST_CASE("AudioEngine validates its configuration and usage", "[audio][engine]") {
    AudioEngine engine;
    std::vector<float> buffer(64);
    CHECK(engine.pull(buffer).error().code == ErrorCode::InvalidArgument); // not running

    AudioDeviceConfig bad = nullConfig();
    bad.sampleRate = 44099; // not a whole number of ticks per sample
    CHECK(engine.start(bad).error().code == ErrorCode::InvalidArgument);
    bad = nullConfig();
    bad.channels = 0;
    CHECK_FALSE(engine.start(bad).has_value());
    bad = nullConfig();
    bad.periodFrames = 1;
    CHECK_FALSE(engine.start(bad).has_value());
    bad = nullConfig();
    bad.nullLatencyFrames = -5;
    CHECK_FALSE(engine.start(bad).has_value());
    CHECK_FALSE(engine.isRunning());

    REQUIRE(engine.start(nullConfig()));
    CHECK(engine.isRunning());
    CHECK(engine.start(nullConfig()).error().code == ErrorCode::InvalidArgument); // already running
    CHECK(engine.backendName() == "Null");
    std::vector<float> odd(3);
    CHECK_FALSE(engine.pull(odd).has_value()); // not a whole number of stereo frames
    CHECK(engine.pull(buffer).has_value());
    CHECK(engine.reopenDevice().has_value()); // nothing to reopen on Null
    CHECK_FALSE(engine.selectDevice("wasapi:something").has_value());
    engine.stop();
    engine.stop(); // idempotent
    CHECK_FALSE(engine.isRunning());
    CHECK(engine.deviceName().empty());
    CHECK_FALSE(engine.reopenDevice().has_value());

    // Selecting a device while stopped only records the choice.
    CHECK(engine.selectDevice("wasapi:{0.0.0.00000000}.{example}").has_value());
    CHECK(engine.config().deviceId == "wasapi:{0.0.0.00000000}.{example}");
}

TEST_CASE("Paused or source-less engines output silence; NaN never reaches the device", "[audio][engine]") {
    struct NanSource final : AudioSource {
        void render(std::span<float> out, int, Rate, Ticks) noexcept override {
            for (std::size_t i = 0; i < out.size(); ++i) {
                out[i] = (i % 2 == 0) ? std::nanf("") : 0.5f;
            }
        }
    };
    AudioEngine engine;
    REQUIRE(engine.start(exactConfig())); // compares exact sample values: no de-click fades
    std::vector<float> buffer(256, 7.0f);
    REQUIRE(engine.pull(buffer));
    CHECK(std::ranges::all_of(buffer, [](float x) { return x == 0.0f; })); // paused

    engine.resume();
    std::ranges::fill(buffer, 7.0f);
    REQUIRE(engine.pull(buffer));
    CHECK(std::ranges::all_of(buffer, [](float x) { return x == 0.0f; })); // no source

    engine.setSource(std::make_shared<NanSource>());
    REQUIRE(engine.pull(buffer));
    CHECK(buffer[0] == 0.0f);
    CHECK(buffer[1] == 0.5f);
    CHECK(std::ranges::none_of(buffer, [](float x) { return std::isnan(x); }));
}

TEST_CASE("Stop freezes the clock and start continues from there", "[audio][engine][transport]") {
    AudioEngine engine;
    auto sine = std::make_shared<SineSource>(440.0);
    engine.setSource(sine);
    engine.resume();
    REQUIRE(engine.start(nullConfig()));
    const auto t0 = std::chrono::steady_clock::now() - 10min; // simulated callbacks, all in the past
    std::vector<float> buffer(480 * 2);
    for (int k = 0; k < 10; ++k) {
        REQUIRE(engine.pull(buffer, t0 + k * 10ms));
    }
    engine.stop(); // "now" is long after the last callback: everything rendered has been heard
    const Ticks frozen = engine.clock();
    CHECK(frozen == samplesToTicks(4800, Rate{48000, 1}));
    std::this_thread::sleep_for(5ms);
    CHECK(engine.clock() == frozen);
    CHECK(engine.source() == sine); // the source survives a stop
    CHECK_FALSE(engine.isPaused());

    REQUIRE(engine.start(nullConfig()));
    CHECK(engine.clock() == frozen);
    REQUIRE(engine.pull(buffer, t0 + 200ms));
    CHECK(engine.clockAt(t0 + 205ms) == frozen + samplesToTicks(240, Rate{48000, 1}));
    engine.stop();
}

namespace {
std::atomic<std::thread::id> g_audioThread;
std::atomic<int> g_constructed{0};
std::atomic<int> g_destroyed{0};
std::atomic<int> g_destroyedOnAudioThread{0};

struct TrackedSource final : AudioSource {
    TrackedSource() { g_constructed.fetch_add(1); }
    ~TrackedSource() override {
        g_destroyed.fetch_add(1);
        if (std::this_thread::get_id() == g_audioThread.load()) {
            g_destroyedOnAudioThread.fetch_add(1);
        }
    }
    void render(std::span<float> out, int, Rate, Ticks) noexcept override { std::ranges::fill(out, 0.125f); }
};
} // namespace

TEST_CASE("setSource can be hammered from another thread; sources die off the audio thread",
          "[audio][engine][threads]") {
    g_constructed = 0;
    g_destroyed = 0;
    g_destroyedOnAudioThread = 0;
    {
        AudioEngine engine;
        engine.resume();
        REQUIRE(engine.start(exactConfig())); // compares exact sample values: no de-click fades
        std::atomic<bool> done{false};
        std::atomic<bool> badSample{false};
        std::thread audio([&] {
            g_audioThread.store(std::this_thread::get_id());
            std::vector<float> buffer(64 * 2);
            while (!done.load()) {
                (void)engine.pull(buffer);
                for (float x : buffer) {
                    if (x != 0.0f && x != 0.125f) {
                        badSample.store(true);
                    }
                }
            }
        });
        for (int i = 0; i < 20000; ++i) {
            engine.setSource(i % 7 == 0 ? nullptr : std::make_shared<TrackedSource>());
        }
        done.store(true);
        audio.join();
        CHECK_FALSE(badSample.load());
        engine.setSource(nullptr);
        engine.stop();
    }
    CHECK(g_constructed.load() > 0);
    CHECK(g_destroyed.load() == g_constructed.load()); // nothing leaked
    CHECK(g_destroyedOnAudioThread.load() == 0);
}

TEST_CASE("NullRealtime runs the real device path and survives a reopen with a monotonic clock",
          "[audio][engine][realtime]") {
    AudioEngine engine;
    engine.setSource(std::make_shared<SineSource>(440.0, 0.25f));
    engine.resume();
    AudioDeviceConfig config;
    config.backend = AudioBackend::NullRealtime;
    const auto begin = std::chrono::steady_clock::now();
    auto started = engine.start(config);
    REQUIRE(started);
    CHECK(engine.isRunning());
    CHECK(engine.backendName() == "Null");
    CHECK_FALSE(engine.deviceName().empty());
    CHECK_FALSE(engine.stats().usingFallbackDevice);

    Ticks previous = engine.clock();
    bool monotonic = true;
    auto sampleFor = [&](std::chrono::milliseconds duration) {
        const auto end = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < end) {
            const Ticks c = engine.clock();
            monotonic = monotonic && c >= previous;
            previous = c;
            std::this_thread::sleep_for(1ms);
        }
    };
    sampleFor(300ms);
    REQUIRE(engine.reopenDevice());
    sampleFor(300ms);
    const double elapsedMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    const double clockMs = previous.toSeconds() * 1000.0;

    INFO("elapsed " << elapsedMs << " ms, clock " << clockMs << " ms, latency " << engine.latency().totalFrames
                    << " frames, callbacks " << engine.stats().callbacks);
    CHECK(monotonic);
    CHECK(engine.stats().deviceReopens == 1);
    CHECK(engine.stats().callbacks > 10);
    CHECK(clockMs > 0.5 * elapsedMs - 100.0); // advancing at roughly real time (generous: timer-driven null device)
    CHECK(clockMs < elapsedMs + 1.0);         // and never ahead of real time
    engine.stop();
    CHECK_FALSE(engine.isRunning());
    const Ticks frozen = engine.clock();
    std::this_thread::sleep_for(20ms);
    CHECK(engine.clock() == frozen);
}

#if defined(_M_X64) || defined(__x86_64__)
// Review wave 5, A5-P2-9. A filter ringing down in a long silence reaches the denormal range, where x86 spends about 100
// cycles an operation: the callback runs with denormals flushed (MXCSR's FTZ and DAZ bits, rt::ScopedDenormalsOff) and
// gives the thread its own mode back. Fails today: the source sees the thread's default mode (both bits clear), and the
// product below is a denormal instead of 0.
TEST_CASE("The audio callback flushes denormals to zero and restores the thread mode", "[audio][engine]") {
    constexpr unsigned int kFtzDaz = 0x8000u | 0x0040u;
    struct ModeSource final : AudioSource {
        unsigned int csr = 0;
        float product = 1.0f;
        void render(std::span<float> out, int, Rate, Ticks) noexcept override {
            csr = _mm_getcsr();
            volatile float smallest = std::numeric_limits<float>::min();
            product = smallest * 0.25f; // a denormal result
            std::ranges::fill(out, 0.0f);
        }
    };
    const unsigned int before = _mm_getcsr() & kFtzDaz;
    auto source = std::make_shared<ModeSource>();
    AudioEngine engine;
    REQUIRE(engine.start(exactConfig()));
    engine.resume();
    engine.setSource(source);
    std::vector<float> buffer(256);
    REQUIRE(engine.pull(buffer));
    CHECK((source->csr & kFtzDaz) == kFtzDaz);
    CHECK(source->product == 0.0f);
    CHECK((_mm_getcsr() & kFtzDaz) == before);
}
#endif
