// AI-13 media signals (wabcut_ai_multicam_media, full builds): microphone levels decoded from real WAV files (speech
// bursts and silence where they were written, remembered for the session, offline media refused), and the decode
// speed on a 10-minute microphone.

#include <wabcut/ai/multicam/MediaAngleSignals.h>
#include <wabcut/model/ProjectState.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numbers>

using namespace wabcut;
using namespace wabcut::ai::multicam;

namespace {

void writeWav(const std::filesystem::path& file, const std::vector<float>& x, int rate) {
    std::ofstream out(file, std::ios::binary);
    const auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const auto bytes = static_cast<std::uint32_t>(x.size() * 2);
    out.write("RIFF", 4);
    u32(36 + bytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(1);
    u32(static_cast<std::uint32_t>(rate));
    u32(static_cast<std::uint32_t>(rate * 2));
    u16(2);
    u16(16);
    out.write("data", 4);
    u32(bytes);
    for (const float v : x) {
        u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f))));
    }
}

/// A microphone: a 220 Hz voice at -12 dBFS during [start, end) of each 10 s, near-silence elsewhere.
std::vector<float> micSignal(double seconds, int rate, double start, double end) {
    std::vector<float> x(static_cast<std::size_t>(seconds * rate));
    const double amp = std::pow(10.0, -12.0 / 20.0) * std::numbers::sqrt2;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double t = static_cast<double>(i) / rate;
        const double inCycle = std::fmod(t, 10.0);
        x[i] = inCycle >= start && inCycle < end ? static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * 220.0 * t)) : 0.0001f * static_cast<float>(static_cast<int>(i % 7) - 3);
    }
    return x;
}

MediaItem wavItem(const std::filesystem::path& file, double seconds) {
    MediaItem m;
    m.id = MediaId{1};
    m.name = file.filename().string();
    m.locator.path = file.generic_string();
    m.info.path = m.locator.path;
    m.info.duration = Ticks{static_cast<std::int64_t>(seconds * kTicksPerSecond)};
    AudioStreamInfo a;
    a.index = 0;
    a.codec = "pcm_s16le";
    a.channels = 1;
    a.sampleRate = Rate{48000, 1};
    a.duration = m.info.duration;
    m.info.audio.push_back(a);
    return m;
}

std::filesystem::path scratch() {
    const auto dir = std::filesystem::temp_directory_path() / "wabcut-multicam-media";
    std::filesystem::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE("Microphone levels from a real file: speech where it was, silence elsewhere", "[ai][multicam][media]") {
    const auto file = scratch() / "host-mic.wav";
    writeWav(file, micSignal(30.0, 48000, 2.0, 6.0), 48000);
    const MediaItem item = wavItem(file, 30.0);
    auto source = makeMediaAngleSignals(nullptr, scratch() / "project.wabcut");
    const Ticks frame{kTicksPerSecond / 20};
    auto levels = source->levels(item, 0, TimeRange{Ticks{}, Ticks{kTicksPerSecond * 30}}, frame, {});
    REQUIRE(levels);
    REQUIRE(levels->size() == 600);
    const auto at = [&](double s) { return (*levels)[static_cast<std::size_t>(s * 20.0)]; };
    CHECK(at(3.0) > -15.0f);
    CHECK(at(3.0) < -9.0f);
    CHECK(at(8.0) < -60.0f);
    CHECK(at(14.0) > -15.0f);
    CHECK(at(21.0) < -60.0f);
    // A part of the file, from the session's memory the second time.
    auto part = source->levels(item, 0, TimeRange{Ticks{kTicksPerSecond * 10}, Ticks{kTicksPerSecond * 10}}, frame, {});
    REQUIRE(part);
    CHECK(part->size() == 200);
    CHECK((*part)[80] > -15.0f);
    const auto again = source->levels(item, 0, TimeRange{Ticks{kTicksPerSecond * 10}, Ticks{kTicksPerSecond * 10}}, frame, {});
    REQUIRE(again);
    CHECK(*again == *part);
    // Errors: no such stream, offline media; faces need the subject provider.
    CHECK(source->levels(item, 3, TimeRange{Ticks{}, Ticks{kTicksPerSecond}}, frame, {}).error().code == ErrorCode::NotFound);
    MediaItem gone = item;
    gone.locator.path = (scratch() / "missing.wav").generic_string();
    CHECK_FALSE(source->levels(gone, 0, TimeRange{Ticks{}, Ticks{kTicksPerSecond}}, frame, {}));
    CHECK_FALSE(source->faces(item, 0, {}));

    // rmsLevels: -100 for digital silence, the RMS in dBFS otherwise.
    const std::vector<float> zeros(1600, 0.0f);
    CHECK(rmsLevels(zeros, 16000, Ticks{kTicksPerSecond / 10}).front() == -100.0f);
    std::vector<float> half(1600, 0.5f);
    CHECK(std::abs(rmsLevels(half, 16000, Ticks{kTicksPerSecond / 10}).front() - (-6.02f)) < 0.05f);
}

TEST_CASE("Decoding a 10-minute microphone for its levels is many times real time", "[ai][multicam][media][benchmark]") {
    const auto file = scratch() / "long-mic.wav";
    writeWav(file, micSignal(600.0, 48000, 1.0, 7.0), 48000);
    const MediaItem item = wavItem(file, 600.0);
    auto source = makeMediaAngleSignals(nullptr, scratch() / "project.wabcut");
    const auto t0 = std::chrono::steady_clock::now();
    auto levels = source->levels(item, 0, TimeRange{Ticks{}, Ticks{kTicksPerSecond * 600}}, Ticks{kTicksPerSecond / 20}, {});
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(levels);
    CHECK(levels->size() == 12000);
    std::printf("[perf] AI-13 microphone levels: 600 s of 48 kHz audio in %.2f s (%.0fx real time)\n", elapsed, 600.0 / std::max(elapsed, 1e-3));
    CHECK(elapsed < 60.0);
    std::filesystem::remove(file);
}
