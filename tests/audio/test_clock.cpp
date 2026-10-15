// Master clock tests on the Null backend: every "device callback" is a pull() at an explicit steady-clock time,
// so the expected clock values can be computed exactly.

#include <wabcut/audio/AudioEngine.h>
#include <wabcut/audio/Sources.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

using namespace wabcut;
using namespace wabcut::audio;

namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr Rate k48k{48000, 1};

/// Simulated time origin, safely in the past so real "now" (used by stop()) is always after every simulated
/// callback.
SteadyClock::time_point origin() {
    static const SteadyClock::time_point t0 = SteadyClock::now() - std::chrono::minutes(30);
    return t0;
}

SteadyClock::time_point at(double ms) {
    return origin() + std::chrono::nanoseconds(static_cast<std::int64_t>(std::llround(ms * 1e6)));
}

/// Milliseconds as ticks (exact for multiples of 1 µs).
Ticks ms(double v) { return Ticks{static_cast<std::int64_t>(std::llround(v * 1000.0)) * (kTicksPerSecond / 1'000'000)}; }

double toMs(Ticks t) { return t.toSeconds() * 1000.0; }

/// Clock readings taken during a simulated run, looked up afterwards. (clockAt() for a time before the latest
/// callback is evaluated as of that callback, so expectations about the past must be sampled at the time.)
struct Samples {
    void add(double t, Ticks value) { values.emplace_back(t, value); }
    Ticks operator()(double t) const {
        for (const auto& [time, value] : values) {
            if (time == t) {
                return value;
            }
        }
        FAIL("no clock sample at " << t << " ms");
        return {};
    }
    std::vector<std::pair<double, Ticks>> values;
};

/// Records the start time of every render() call (without allocating: storage is reserved up front).
struct RecordingSource final : AudioSource {
    RecordingSource() { starts.reserve(100'000); }
    void render(std::span<float> interleaved, int, Rate, Ticks startTime) noexcept override {
        if (starts.size() < starts.capacity()) {
            starts.push_back(startTime);
        }
        std::fill(interleaved.begin(), interleaved.end(), 0.25f);
    }
    std::vector<Ticks> starts;
};

struct NullRig {
    explicit NullRig(int latencyFrames = 0) : source(std::make_shared<RecordingSource>()) {
        engine.setSource(source);
        config.backend = AudioBackend::Null;
        config.nullLatencyFrames = latencyFrames;
    }
    void start() { REQUIRE(engine.start(config)); }
    void pull(double atMs, std::size_t frames = 480) {
        buffer.assign(frames * 2, -1.0f);
        REQUIRE(engine.pull(buffer, at(atMs)));
    }
    Ticks clock(double atMs) const { return engine.clockAt(at(atMs)); }

    AudioEngine engine;
    AudioDeviceConfig config;
    std::shared_ptr<RecordingSource> source;
    std::vector<float> buffer;
};

} // namespace

TEST_CASE("Clock of a stopped engine follows the transport without advancing", "[audio][clock]") {
    AudioEngine engine;
    CHECK(engine.clock() == Ticks{});
    CHECK(engine.isPaused());
    engine.setTimeline(Ticks::fromSeconds(5));
    CHECK(engine.clock() == Ticks::fromSeconds(5));
    engine.resume();
    CHECK_FALSE(engine.isPaused());
    CHECK(engine.clock() == Ticks::fromSeconds(5));
}

TEST_CASE("Clock interpolates exactly between deterministic pulls", "[audio][clock]") {
    NullRig rig(0);
    rig.engine.setTimeline(Ticks::fromSeconds(1));
    rig.engine.resume();
    rig.start();

    CHECK(rig.clock(0) == Ticks::fromSeconds(1)); // nothing rendered yet: hold at the start position
    rig.pull(0);
    for (double u = 0; u <= 10; u += 0.5) {
        CHECK(rig.clock(u) == Ticks::fromSeconds(1) + ms(u));
    }
    // Never ahead of what has been rendered: with 480 frames delivered the clock stops at +10 ms.
    CHECK(rig.clock(20) == Ticks::fromSeconds(1) + ms(10));
    CHECK(rig.source->starts.front() == Ticks::fromSeconds(1));

    bool exact = true;
    bool monotonic = true;
    Ticks previous = rig.clock(10);
    for (int k = 1; k < 1000; ++k) { // 10 seconds of 10 ms callbacks
        rig.pull(10.0 * k);
        for (double u = 10.0 * k; u < 10.0 * (k + 1); u += 1.0) {
            const Ticks c = rig.clock(u);
            exact = exact && c == Ticks::fromSeconds(1) + ms(u);
            monotonic = monotonic && c >= previous;
            previous = c;
        }
    }
    CHECK(exact);
    CHECK(monotonic);
    CHECK(rig.engine.stats().callbacks == 1000);
    CHECK(rig.engine.stats().framesRendered == 480'000);
    CHECK(rig.engine.stats().clockResyncs == 1); // only the initial anchor
    // Source time handed to render() is contiguous.
    bool contiguous = true;
    for (std::size_t i = 1; i < rig.source->starts.size(); ++i) {
        contiguous = contiguous && rig.source->starts[i] - rig.source->starts[i - 1] == samplesToTicks(480, k48k);
    }
    CHECK(contiguous);
}

TEST_CASE("Clock compensates the output latency", "[audio][clock][latency]") {
    NullRig rig(960); // 20 ms
    rig.engine.resume();
    rig.start();
    const AudioLatency latency = rig.engine.latency();
    CHECK(latency.bufferFrames == 960);
    CHECK(latency.totalFrames == 960);
    CHECK(latency.total == ms(20));

    bool exact = true;
    for (int k = 0; k < 50; ++k) {
        rig.pull(10.0 * k);
        for (double u = 10.0 * k; u < 10.0 * (k + 1); u += 2.5) {
            const Ticks expected = u <= 20.0 ? Ticks{} : ms(u - 20.0);
            exact = exact && rig.clock(u) == expected;
        }
    }
    CHECK(exact);
}

TEST_CASE("Pause and resume never make the clock jump", "[audio][clock][transport]") {
    NullRig rig(960);
    rig.engine.resume();
    rig.start();

    Samples clock;
    for (double t = 0; t <= 400; t += 0.5) {
        if (std::fmod(t, 10.0) == 0.0) {
            if (t == 110) {
                rig.engine.pause(); // requested between the 100 ms and 110 ms callbacks
            }
            if (t == 200) {
                rig.engine.resume();
            }
            rig.pull(t);
        }
        clock.add(t, rig.clock(t));
    }

    // Exact expectations (20 ms latency; pause applied at the 110 ms block, resume at the 200 ms block).
    CHECK(clock(105) == ms(85));
    CHECK(clock(125) == ms(105)); // the audio queued before the pause still plays...
    CHECK(clock(130) == ms(110)); // ...until the last rendered sample is heard
    CHECK(clock(150) == ms(110));
    CHECK(clock(219.5) == ms(110)); // resumed audio is not heard before 220 ms
    CHECK(clock(230) == ms(120));
    CHECK(clock(400) == ms(290));

    bool monotonic = true;
    bool noJump = true;
    for (std::size_t i = 1; i < clock.values.size(); ++i) {
        const Ticks step = clock.values[i].second - clock.values[i - 1].second;
        monotonic = monotonic && step >= Ticks{};
        noJump = noJump && step <= ms(0.5);
    }
    CHECK(monotonic);
    CHECK(noJump);

    // The source never rendered while paused and resumed exactly where it stopped.
    const auto& starts = rig.source->starts;
    REQUIRE(starts.size() == 11 + 21); // blocks 0..100 ms, then 200..400 ms
    CHECK(starts[10] == ms(100));
    CHECK(starts[11] == ms(110));
    CHECK(starts.back() == ms(310));
}

TEST_CASE("Seeking rebases the clock immediately and holds until the new audio is heard", "[audio][clock][transport]") {
    NullRig rig(960);
    rig.engine.resume();
    rig.start();
    for (int k = 0; k <= 4; ++k) {
        rig.pull(10.0 * k);
    }
    CHECK(rig.clock(45) == ms(25));

    rig.engine.setTimeline(Ticks::fromSeconds(10));
    CHECK(rig.clock(45) == Ticks::fromSeconds(10)); // pending: reported at once
    CHECK(rig.clock(49) == Ticks::fromSeconds(10));
    rig.pull(50); // applied at device frame 2400, heard from 70 ms
    CHECK(rig.source->starts.back() == Ticks::fromSeconds(10));
    CHECK(rig.clock(55) == Ticks::fromSeconds(10));
    rig.pull(60);
    CHECK(rig.clock(60) == Ticks::fromSeconds(10));
    rig.pull(70);
    CHECK(rig.clock(70) == Ticks::fromSeconds(10));
    rig.pull(80);
    CHECK(rig.clock(80) == Ticks::fromSeconds(10) + ms(10));
    CHECK(rig.clock(85) == Ticks::fromSeconds(10) + ms(15));

    // Seeking backwards is the one way the clock may go back.
    rig.engine.setTimeline(Ticks::fromSeconds(2));
    CHECK(rig.clock(85) == Ticks::fromSeconds(2));
    rig.pull(90); // applied at frame 4320, heard from 110 ms
    rig.pull(100);
    CHECK(rig.clock(105) == Ticks::fromSeconds(2));
    rig.pull(110);
    CHECK(rig.clock(115) == Ticks::fromSeconds(2) + ms(5));

    // Seeking while paused: the clock lands on the target and stays there.
    rig.engine.pause();
    rig.pull(120);
    rig.engine.setTimeline(Ticks::fromSeconds(3));
    CHECK(rig.clock(125) == Ticks::fromSeconds(3));
    for (int k = 13; k <= 16; ++k) {
        rig.pull(10.0 * k);
    }
    CHECK(rig.clock(165) == Ticks::fromSeconds(3));
    rig.engine.resume(); // applied at the 170 ms block (frame 8160), heard from 190 ms
    for (int k = 17; k <= 21; ++k) {
        rig.pull(10.0 * k);
    }
    CHECK(rig.source->starts.back() == Ticks::fromSeconds(3) + ms(40));
    CHECK(rig.clock(215) == Ticks::fromSeconds(3) + ms(25));
}

TEST_CASE("Callback jitter is filtered: the clock stays smooth and on time", "[audio][clock][jitter]") {
    NullRig rig(960);
    rig.engine.resume();
    rig.start();

    std::mt19937 rng(42);
    std::uniform_real_distribution<double> jitter(-1.5, 1.5);
    double maxError = 0.0;
    double maxStepError = 0.0;
    bool monotonic = true;
    Ticks previous{};
    double nextPull = 0.0;
    int k = 0;
    for (double t = 0; t < 5000; t += 0.25) {
        while (nextPull <= t) {
            rig.pull(nextPull);
            ++k;
            nextPull = 10.0 * k + jitter(rng); // the device consumes steadily; only the callback time jitters
        }
        const Ticks c = rig.clock(t);
        monotonic = monotonic && c >= previous;
        if (t > 500) {
            maxError = std::max(maxError, std::fabs(toMs(c) - (t - 20.0)));
            maxStepError = std::max(maxStepError, std::fabs(toMs(c - previous) - 0.25));
        }
        previous = c;
    }
    INFO("max deviation from ideal: " << maxError << " ms, max step error: " << maxStepError << " ms");
    CHECK(monotonic);
    CHECK(maxError < 1.0);      // raw callback timing is off by up to 1.5 ms
    CHECK(maxStepError < 0.25); // no visible steps at display rate
    CHECK(rig.engine.stats().clockResyncs == 1);
}

TEST_CASE("A stalled device holds the clock; recovery never moves it backwards", "[audio][clock][stall]") {
    NullRig rig(960);
    rig.engine.resume();
    rig.start();
    for (int k = 0; k <= 20; ++k) {
        rig.pull(10.0 * k); // 21 blocks rendered: 10080 frames = 210 ms of audio
    }
    CHECK(rig.clock(225) == ms(205));
    CHECK(rig.clock(300) == ms(210)); // everything rendered has been heard: hold
    CHECK(rig.clock(499) == ms(210));

    Samples clock;
    Ticks previous = rig.clock(499);
    bool monotonic = true;
    for (double t = 500; t <= 700; t += 0.5) {
        if (std::fmod(t, 10.0) == 0.0) {
            rig.pull(t);
        }
        const Ticks c = rig.clock(t);
        clock.add(t, c);
        monotonic = monotonic && c >= previous;
        previous = c;
    }
    CHECK(monotonic);
    CHECK(clock(505) == ms(210)); // re-anchored behind what was already shown: hold...
    CHECK(clock(520) == ms(210));
    CHECK(clock(530) == ms(220)); // ...then continue at the real rate
    CHECK(clock(700) == ms(390));
    CHECK(rig.engine.stats().clockResyncs == 2);
}

TEST_CASE("A latency increase holds the clock instead of rewinding it", "[audio][clock][latency]") {
    NullRig rig(960);
    rig.engine.resume();
    rig.start();
    Samples clock;
    Ticks previous{};
    bool monotonic = true;
    for (double t = 0; t <= 600; t += 0.5) {
        if (std::fmod(t, 10.0) == 0.0) {
            if (t == 210) {
                rig.engine.setLatencyOffset(ms(50)); // e.g. switched to Bluetooth headphones
            }
            rig.pull(t);
        }
        const Ticks c = rig.clock(t);
        clock.add(t, c);
        monotonic = monotonic && c >= previous;
        previous = c;
    }
    CHECK(monotonic);
    CHECK(rig.engine.latency().totalFrames == 960 + 2400);
    CHECK(rig.engine.latency().offsetFrames == 2400);
    CHECK(clock(250) == ms(209.5)); // holding (at the last value shown) while the later timeline catches up
    CHECK(clock(300) == ms(230)); // then 70 ms behind real time
    CHECK(clock(600) == ms(530));
}

TEST_CASE("Clock is monotonic for concurrent readers while the audio thread runs", "[audio][clock][threads]") {
    AudioEngine engine;
    engine.setSource(std::make_shared<SineSource>(440.0));
    engine.resume();
    AudioDeviceConfig config;
    config.backend = AudioBackend::Null;
    config.nullLatencyFrames = 512;
    REQUIRE(engine.start(config));

    std::atomic<bool> done{false};
    std::thread audio([&] {
        std::vector<float> buffer(96 * 2);
        while (!done.load()) {
            (void)engine.pull(buffer); // timestamped with the real steady clock
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    });
    std::atomic<int> violations{0};
    std::atomic<long long> reads{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 3; ++r) {
        readers.emplace_back([&] {
            Ticks previous = engine.clock();
            while (!done.load()) {
                const Ticks c = engine.clock();
                if (c < previous) {
                    violations.fetch_add(1);
                }
                previous = c;
                reads.fetch_add(1);
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    done.store(true);
    audio.join();
    for (auto& t : readers) {
        t.join();
    }
    CHECK(violations.load() == 0);
    CHECK(reads.load() > 1000);
    CHECK(engine.clock() > Ticks{});
}
