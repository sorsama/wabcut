// Tests against the machine's real audio hardware. They skip (not fail) when there is no output device, so CI
// machines without sound stay green.

#include <wabcut/audio/AudioEngine.h>
#include <wabcut/audio/Sources.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace wabcut;
using namespace wabcut::audio;
using namespace std::chrono_literals;

TEST_CASE("Output devices can be enumerated", "[audio][device]") {
    auto devices = AudioEngine::enumerateOutputDevices();
    if (!devices) {
        SKIP("no audio backend available: " << devices.error().message);
    }
    std::printf("[audio] %zu output device(s)\n", devices->size());
    for (const AudioDeviceInfo& d : *devices) {
        std::printf("[audio]   %s%s  (%s)\n", d.isDefault ? "* " : "  ", d.name.c_str(), d.id.c_str());
        CHECK_FALSE(d.id.empty());
        CHECK_FALSE(d.name.empty());
    }
}

TEST_CASE("The default output device plays 200 ms of silence and stops", "[audio][device]") {
    AudioEngine engine;
    engine.setSource(std::make_shared<SilenceSource>());
    engine.resume();

    AudioDeviceConfig config; // 48 kHz, stereo, 512-frame period, system default device
    config.fallbackToNull = false;
    // WABCUT_AUDIO_TEST_DEVICE=<part of a device name> runs this test on that device instead of the default
    // (handy for measuring the latency of a particular output).
    if (const char* wanted = std::getenv("WABCUT_AUDIO_TEST_DEVICE"); wanted != nullptr && *wanted != '\0') {
        if (auto devices = AudioEngine::enumerateOutputDevices()) {
            for (const AudioDeviceInfo& d : *devices) {
                if (d.name.find(wanted) != std::string::npos) {
                    config.deviceId = d.id;
                    break;
                }
            }
        }
    }
    auto started = engine.start(config);
    if (!started) {
        SKIP("no usable audio output device: " << started.error().message);
    }
    REQUIRE(engine.isRunning());
    INFO("fallback reason: " << engine.deviceError());
    CHECK_FALSE(engine.stats().usingFallbackDevice);

    const auto begin = std::chrono::steady_clock::now();
    Ticks previous = engine.clock();
    bool monotonic = true;
    while (std::chrono::steady_clock::now() - begin < 200ms) {
        const Ticks c = engine.clock();
        monotonic = monotonic && c >= previous;
        previous = c;
        std::this_thread::sleep_for(1ms);
    }
    const double elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    const AudioEngineStats stats = engine.stats();
    const AudioLatency latency = engine.latency();
    const std::string name = engine.deviceName();
    const std::string backend = engine.backendName();
    const ClockSample sample = engine.clockSample();
    engine.stop();

    const double rate = 48000.0;
    std::printf("[audio] device: %s (%s)\n", name.c_str(), backend.c_str());
    const char* quality = sample.quality == LatencyQuality::Measured         ? "measured"
                          : sample.quality == LatencyQuality::UserCalibrated ? "user-calibrated"
                                                                             : "estimated";
    std::printf("[audio] clock sample: epoch %u, latency %lld frames (%s), drift %.1f ppm\n", sample.deviceEpoch,
                static_cast<long long>(sample.latencyFrames), quality, stats.driftPpm);
    std::printf("[audio] latency: buffer %lld + stream %lld frames = %lld frames = %.2f ms (period %lld frames = "
                "%.2f ms)\n",
                static_cast<long long>(latency.bufferFrames), static_cast<long long>(latency.streamFrames),
                static_cast<long long>(latency.totalFrames), static_cast<double>(latency.totalFrames) * 1000.0 / rate,
                static_cast<long long>(latency.periodFrames), static_cast<double>(latency.periodFrames) * 1000.0 / rate);
    std::printf("[audio] measured over %.1f ms: %llu callbacks, %llu frames (%.1f frames/callback, one every %.2f ms), "
                "clock reached %.1f ms\n",
                elapsedMs, static_cast<unsigned long long>(stats.callbacks),
                static_cast<unsigned long long>(stats.framesRendered),
                stats.callbacks ? static_cast<double>(stats.framesRendered) / static_cast<double>(stats.callbacks) : 0.0,
                stats.callbacks ? elapsedMs / static_cast<double>(stats.callbacks) : 0.0, previous.toSeconds() * 1000.0);

    CHECK_FALSE(name.empty());
    CHECK(stats.callbacks > 0);
    CHECK(stats.framesRendered > 0);
    CHECK(latency.totalFrames > 0);
    CHECK(monotonic);
    CHECK(previous > Ticks{}); // the clock advanced while silence played
    CHECK(sample.running);
    CHECK(sample.latencyFrames == latency.totalFrames);
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("An unknown selected device falls back, says why, and can be switched at run time", "[audio][device]") {
    AudioEngine engine;
    engine.setSource(std::make_shared<SilenceSource>());
    engine.resume();
    AudioDeviceConfig config;
    config.deviceId = "wasapi:{does-not-exist}";
    REQUIRE(engine.start(config)); // fallbackToNull: always starts, on the default device or the null device
    CHECK(engine.isRunning());
    CHECK(engine.stats().usingFallbackDevice);
    INFO("fallback reason: " << engine.deviceError());
    CHECK(engine.deviceError().find("not found") != std::string::npos);
    std::printf("[audio] fallback for a missing device: %s (%s)\n", engine.deviceName().c_str(),
                engine.backendName().c_str());

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
    sampleFor(100ms);
    REQUIRE(engine.selectDevice("")); // follow the system default
    sampleFor(100ms);
    CHECK(engine.stats().deviceReopens == 1);
    CHECK(engine.config().deviceId.empty());
    CHECK(monotonic);
    CHECK(previous > Ticks{});
    engine.stop();
}
