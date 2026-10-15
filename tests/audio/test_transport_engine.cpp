// Contract rev 2 through the engine: ClockSample, device epochs, shuttle muting, preroll, loop, drift.

#include <wabcut/audio/AudioEngine.h>
#include <wabcut/audio/Sources.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

using namespace wabcut;
using namespace wabcut::audio;
using namespace std::chrono_literals;

namespace {

using SteadyClock = std::chrono::steady_clock;

SteadyClock::time_point origin() {
    static const SteadyClock::time_point t0 = SteadyClock::now() - std::chrono::minutes(30);
    return t0;
}
SteadyClock::time_point at(double ms) {
    return origin() + std::chrono::nanoseconds(static_cast<std::int64_t>(std::llround(ms * 1e6)));
}
std::int64_t originNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(origin().time_since_epoch()).count(); }
Ticks ms(double v) { return Ticks{static_cast<std::int64_t>(std::llround(v * 1000.0)) * (kTicksPerSecond / 1'000'000)}; }

/// Constant 0.25 on every sample; records render start times (storage reserved, no allocation while rendering).
struct ConstantSource final : AudioSource {
    ConstantSource() { starts.reserve(100'000); }
    void render(std::span<float> out, int, Rate, Ticks startTime) noexcept override {
        if (starts.size() < starts.capacity()) {
            starts.push_back(startTime);
        }
        std::ranges::fill(out, 0.25f);
    }
    std::vector<Ticks> starts;
};

struct NullRig {
    explicit NullRig(int latencyFrames = 0) : source(std::make_shared<ConstantSource>()) {
        engine.setSource(source);
        config.backend = AudioBackend::Null;
        config.nullLatencyFrames = latencyFrames;
        config.declickTime = Ticks{}; // these tests assert exact silence/audibility per block
    }
    void start() { REQUIRE(engine.start(config)); }
    /// Pull one 10 ms block at `atMs`; returns true if any sample was non-zero (audible).
    bool pull(double atMs, std::size_t frames = 480) {
        buffer.assign(frames * 2, -1.0f);
        REQUIRE(engine.pull(buffer, at(atMs)));
        return std::ranges::any_of(buffer, [](float x) { return x != 0.0f; });
    }
    Ticks clock(double atMs) const { return engine.clockAt(at(atMs)); }

    AudioEngine engine;
    AudioDeviceConfig config;
    std::shared_ptr<ConstantSource> source;
    std::vector<float> buffer;
};

} // namespace

TEST_CASE("clockSample reports the device clock with latency, rate, quality and epoch", "[audio][clocksample]") {
    NullRig rig(960);
    CHECK_FALSE(rig.engine.clockSample().running); // not started
    rig.engine.resume();
    rig.start();
    CHECK_FALSE(rig.engine.clockSample().running); // no callback yet
    rig.pull(0);
    rig.pull(10);
    const ClockSample s = rig.engine.clockSample();
    CHECK(s.running);
    CHECK(s.negotiatedRate == Rate{48000, 1});
    CHECK(s.latencyFrames == 960);
    CHECK(s.quality == LatencyQuality::Measured); // the Null backend's latency is exact by construction
    CHECK(s.devicePositionFrames == 480);         // written-equivalent position at the 10 ms callback...
    CHECK(s.presentedFrame() == 480 - 960);       // ...20 ms of which is still queued
    CHECK(s.hostTimeNs - originNs() == 10'000'000);
    CHECK(rig.engine.stats().deviceEpoch == s.deviceEpoch);

    rig.engine.setLatencyOffset(ms(5));
    rig.pull(20);
    CHECK(rig.engine.clockSample().quality == LatencyQuality::UserCalibrated);
    CHECK(rig.engine.clockSample().latencyFrames == 960 + 240);
    rig.engine.setLatencyOffset(Ticks{});
    rig.pull(30);
    CHECK(rig.engine.clockSample().quality == LatencyQuality::Measured);

    // Every start is a new device epoch.
    const std::uint32_t firstEpoch = rig.engine.clockSample().deviceEpoch;
    rig.engine.stop();
    CHECK_FALSE(rig.engine.clockSample().running);
    rig.start();
    rig.pull(100);
    CHECK(rig.engine.clockSample().deviceEpoch == firstEpoch + 1);
}

TEST_CASE("clockSample is consistent for concurrent readers", "[audio][clocksample][threads]") {
    NullRig rig(960);
    rig.engine.resume();
    rig.start();
    std::atomic<bool> done{false};
    std::thread audio([&] {
        std::vector<float> buffer(480 * 2);
        for (int k = 0; k < 20000; ++k) { // 200 s of exact 10 ms callbacks, as fast as possible
            (void)rig.engine.pull(buffer, at(10.0 * k));
        }
        done.store(true);
    });
    std::atomic<int> inconsistent{0};
    std::atomic<int> backwards{0};
    std::atomic<long long> samples{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 3; ++r) {
        readers.emplace_back([&] {
            ClockSample previous{};
            while (!done.load()) {
                const ClockSample s = rig.engine.clockSample();
                if (!s.running) {
                    continue;
                }
                // With exact callbacks every sample lies exactly on position = (host - origin) · 48 kHz; a torn
                // read (fields from two different samples) would break it.
                const bool onLine = s.devicePositionFrames * 1'000'000'000LL == (s.hostTimeNs - originNs()) * 48000LL;
                if (!onLine || s.latencyFrames != 960 || s.negotiatedRate != Rate{48000, 1}) {
                    inconsistent.fetch_add(1);
                }
                if (s.devicePositionFrames < previous.devicePositionFrames || s.hostTimeNs < previous.hostTimeNs) {
                    backwards.fetch_add(1);
                }
                previous = s;
                samples.fetch_add(1);
            }
        });
    }
    audio.join();
    for (auto& t : readers) {
        t.join();
    }
    CHECK(inconsistent.load() == 0);
    CHECK(backwards.load() == 0);
    CHECK(samples.load() > 100);
    CHECK(rig.engine.clockSample().devicePositionFrames == 480LL * 19999);
}

TEST_CASE("deviceEpoch increments on reopenDevice()", "[audio][clocksample][epoch]") {
    AudioEngine engine;
    engine.setSource(std::make_shared<SilenceSource>());
    engine.resume();
    AudioDeviceConfig config;
    config.backend = AudioBackend::NullRealtime;
    REQUIRE(engine.start(config));
    auto waitFor = [&](auto predicate) {
        const auto deadline = SteadyClock::now() + 3s;
        while (!predicate() && SteadyClock::now() < deadline) {
            std::this_thread::sleep_for(2ms);
        }
        return predicate();
    };
    REQUIRE(waitFor([&] { return engine.clockSample().running; }));
    const std::uint32_t before = engine.clockSample().deviceEpoch;
    const Ticks clockBefore = engine.clock();

    REQUIRE(engine.reopenDevice());
    CHECK(waitFor([&] { return engine.clockSample().deviceEpoch == before + 1 && engine.clockSample().running; }));
    CHECK(engine.stats().deviceEpoch == before + 1);
    CHECK(engine.clock() >= clockBefore); // the transport carried over

    REQUIRE(engine.reopenDevice());
    CHECK(waitFor([&] { return engine.clockSample().deviceEpoch == before + 2; }));
    engine.stop();
}

TEST_CASE("At any speed but 1/1 the source is muted while the clock follows the speed", "[audio][transport][shuttle]") {
    NullRig rig(0);
    rig.engine.resume();
    rig.engine.setSpeed({2, 1});
    rig.start();
    CHECK_FALSE(rig.pull(0));
    CHECK(rig.clock(5) == ms(10)); // 2x
    CHECK_FALSE(rig.pull(10));
    CHECK(rig.clock(15) == ms(30));
    CHECK(rig.source->starts.empty()); // the source was never asked for audio

    rig.engine.setSpeed(TransportSpeed::normal());
    CHECK(rig.pull(20)); // audible again at 1/1, continuing from 40 ms
    CHECK(rig.source->starts.back() == ms(40));
    CHECK(rig.clock(25) == ms(45));

    rig.engine.setSpeed({1, 2});
    CHECK_FALSE(rig.pull(30));
    CHECK(rig.clock(35) == ms(52.5));

    rig.engine.setSpeed({-1, 1}); // reverse: muted, clock runs backwards
    CHECK_FALSE(rig.pull(40));
    CHECK(rig.clock(45) == ms(50));
    CHECK(rig.engine.speed() == TransportSpeed{-1, 1});

    rig.engine.setSpeed({0, 1}); // frozen
    CHECK_FALSE(rig.pull(50));
    CHECK(rig.clock(55) == ms(45));
    CHECK(rig.source->starts.size() == 1);
}

TEST_CASE("Seek with preroll renders silence until the preroll has elapsed", "[audio][transport][seek]") {
    NullRig rig(0);
    rig.engine.resume();
    rig.start();
    CHECK(rig.pull(0));
    rig.engine.seek(Ticks::fromSeconds(10), ms(20)); // applied at frame 480; plays from frame 1440
    CHECK(rig.clock(5) == Ticks::fromSeconds(10));    // pending seek: reported at once
    CHECK_FALSE(rig.pull(10));
    CHECK(rig.clock(15) == Ticks::fromSeconds(10));
    CHECK_FALSE(rig.pull(20));
    CHECK(rig.pull(30));
    CHECK(rig.source->starts.back() == Ticks::fromSeconds(10));
    CHECK(rig.clock(30) == Ticks::fromSeconds(10));
    CHECK(rig.clock(35) == Ticks::fromSeconds(10) + ms(5));
}

TEST_CASE("A loop range wraps the audio and the clock together", "[audio][transport][loop]") {
    NullRig rig(0);
    rig.engine.setLoop(TimeRange{Ticks{}, ms(25)});
    REQUIRE(rig.engine.loop().has_value());
    rig.engine.resume();
    rig.start();
    for (int k = 0; k < 6; ++k) {
        CHECK(rig.pull(10.0 * k));
    }
    // Blocks: [0,10) [10,20) [20,25)+[0,5) [5,15) [15,25) [0,10): the third and fifth+sixth are split at the wrap.
    const std::vector<Ticks> expected{ms(0), ms(10), ms(20), ms(0), ms(5), ms(15), ms(0)};
    CHECK(rig.source->starts == expected);
    CHECK(rig.clock(52) == ms(2)); // 52 ms of playback in a 25 ms loop
    rig.engine.setLoop(std::nullopt);
    CHECK_FALSE(rig.engine.loop().has_value());
}

TEST_CASE("Clock drift is tracked continuously without snapping", "[audio][transport][drift]") {
    NullRig rig(960);
    rig.engine.resume();
    rig.start();
    // The device consumes 480 frames every 9.999 ms of steady time: its crystal runs +100 ppm fast.
    constexpr double kPeriodMs = 10.0 / (1.0 + 100e-6);
    Ticks previous{};
    bool monotonic = true;
    double maxStepError = 0.0;
    int k = 0;
    double nextPull = 0.0;
    for (double t = 0; t < 30000; t += 1.0) {
        while (nextPull <= t) {
            rig.pull(nextPull);
            nextPull = kPeriodMs * ++k;
        }
        const Ticks c = rig.clock(t);
        monotonic = monotonic && c >= previous;
        if (t > 1000) {
            // At +100 ppm, 1 ms of steady time is 1.0001 ms of audio.
            maxStepError = std::max(maxStepError, std::fabs((c - previous).toSeconds() * 1000.0 - 1.0001));
        }
        previous = c;
    }
    const double drift = rig.engine.stats().driftPpm;
    INFO("drift " << drift << " ppm, max step error " << maxStepError << " ms");
    CHECK(drift > 90.0);
    CHECK(drift < 110.0);
    CHECK(monotonic);
    CHECK(maxStepError < 0.01);                   // smooth: no snapping
    CHECK(rig.engine.stats().clockResyncs == 1); // never re-anchored after start
    // And it stays on time: 30 s of steady time is 30.003 s of audio, minus the 20 ms latency.
    CHECK(std::fabs(previous.toSeconds() - (29.999 * 1.0001 - 0.020)) < 0.001);
}

TEST_CASE("transportMap() lets a scheduler predict when a timeline time will be heard", "[audio][transport][clock]") {
    NullRig rig(960);
    rig.engine.resume();
    rig.start();
    for (int k = 0; k <= 10; ++k) {
        rig.pull(10.0 * k);
    }
    const TransportMap map = rig.engine.transportMap();
    REQUIRE(map.hasClock());
    const auto host = map.hostTimeFor(ms(70)); // frame 3360 is heard 20 ms after it was written at 70 ms
    REQUIRE(host.has_value());
    CHECK(*host - originNs() == 90'000'000);
    CHECK(map.transportTime(*host) == ms(70));
    CHECK(rig.engine.clockAt(at(100)) == ms(80)); // the engine's clock agrees at the latest callback
}

namespace {
/// Largest sample-to-sample step on channel 0 of `stream` (interleaved stereo).
double maxStep(const std::vector<float>& stream) {
    double worst = 0.0;
    for (std::size_t i = 2; i < stream.size(); i += 2) {
        worst = std::max(worst, std::fabs(static_cast<double>(stream[i]) - stream[i - 2]));
    }
    return worst;
}

/// Plays a 777 Hz, 0.8 sine through start, pause, resume, seek and a loop wrap; returns the whole output.
std::vector<float> transportEdges(Ticks declick) {
    AudioEngine engine;
    engine.setSource(std::make_shared<SineSource>(777.0, 0.8f));
    AudioDeviceConfig config;
    config.backend = AudioBackend::Null;
    config.declickTime = declick;
    REQUIRE(engine.start(config));
    engine.resume();
    std::vector<float> stream;
    std::vector<float> block(480 * 2);
    auto pull = [&](int k) {
        REQUIRE(engine.pull(block, at(10.0 * k)));
        stream.insert(stream.end(), block.begin(), block.end());
    };
    int k = 0;
    for (; k < 5; ++k) pull(k);
    engine.pause();
    for (; k < 8; ++k) pull(k);
    engine.resume();
    for (; k < 12; ++k) pull(k);
    engine.setTimeline(ms(1234.567)); // mid-waveform jump
    for (; k < 16; ++k) pull(k);
    engine.setLoop(TimeRange{ms(1280), ms(13.3)}); // entered ahead, then wraps mid-waveform every 13.3 ms
    for (; k < 24; ++k) pull(k);
    engine.stop();
    return stream;
}
} // namespace

TEST_CASE("Transport edges are de-clicked: start, pause, resume, seek and loop never step the waveform",
          "[audio][transport][declick]") {
    // The sine itself moves at most 2*pi*777/48000*0.8 = 0.081 per sample; a de-clicked edge adds at most
    // |x|/97 for the fade-in and |last|/97 for the decaying tail (2 ms = 96 frames).
    const double clean = maxStep(transportEdges(Ticks{kTicksPerSecond / 500}));
    const double raw = maxStep(transportEdges(Ticks{}));
    INFO("max step with de-click " << clean << ", without " << raw);
    CHECK(clean < 0.1);
    CHECK(raw > 0.3); // the same transport edges really are clicks without it
}
