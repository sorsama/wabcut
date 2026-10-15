// A.8 voice-over recording, the audio layer: AudioInput (the capture rings, the clock line, meters, monitoring) on the
// Null backend, and the crash-safe WavWriter with recoverWav.

#include <wabcut/audio/AudioInput.h>
#include <wabcut/audio/WavWriter.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace wabcut;
using namespace wabcut::audio;
using Catch::Approx;

namespace {

std::filesystem::path scratchDir(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / "wabcut-tests" / "voice-over" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

std::vector<std::uint8_t> bytesOf(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::uint32_t u32(const std::vector<std::uint8_t>& b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) | (static_cast<std::uint32_t>(b[at + 2]) << 16) |
           (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

/// The samples of a canonical WAV, as floats.
std::vector<float> samplesOf(const std::filesystem::path& file) {
    const auto b = bytesOf(file);
    REQUIRE(b.size() >= 44);
    const int bits = b[34] | (b[35] << 8);
    const std::uint32_t dataBytes = u32(b, 40);
    REQUIRE(44 + dataBytes <= b.size());
    std::vector<float> out;
    for (std::size_t at = 44; at + bits / 8 <= 44 + dataBytes; at += static_cast<std::size_t>(bits / 8)) {
        if (bits == 32) {
            float v = 0.0f;
            std::memcpy(&v, b.data() + at, 4);
            out.push_back(v);
        } else if (bits == 24) {
            std::int32_t s = b[at] | (b[at + 1] << 8) | (b[at + 2] << 16);
            if ((s & 0x800000) != 0) {
                s -= 0x1000000;
            }
            out.push_back(static_cast<float>(s / 8388607.0));
        } else {
            const auto s = static_cast<std::int16_t>(b[at] | (b[at + 1] << 8));
            out.push_back(static_cast<float>(s / 32767.0));
        }
    }
    return out;
}

AudioInputConfig nullInput(int channels = 1, int latencyFrames = 0) {
    AudioInputConfig c;
    c.backend = AudioBackend::Null;
    c.channels = channels;
    c.nullLatencyFrames = latencyFrames;
    return c;
}

std::chrono::steady_clock::time_point at(std::int64_t ns) { return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(ns)); }

} // namespace

TEST_CASE("AudioInput (Null) delivers pushed frames in order, with the clock line and the reported latency", "[audio][voiceover]") {
    AudioInput input;
    CHECK_FALSE(input.isRunning());
    CHECK_FALSE(input.push(std::vector<float>(10, 0.0f))); // not running
    {
        AudioInput stereo;
        REQUIRE(stereo.start(nullInput(2)));
        CHECK_FALSE(stereo.push(std::vector<float>(3, 0.0f))); // not whole frames
        AudioInputConfig bad = nullInput(3);
        AudioInput other;
        CHECK_FALSE(other.start(bad));
        bad = nullInput();
        bad.backend = AudioBackend::NullRealtime;
        CHECK_FALSE(other.start(bad));
    }
    REQUIRE(input.start(nullInput(1, 480)));
    CHECK(input.isRunning());
    CHECK(input.deviceName() == "Null input");
    CHECK(input.latency().bufferFrames == 480);
    CHECK(input.latency().total == samplesToTicks(480, Rate{48000, 1}));
    CHECK_FALSE(input.start(nullInput())); // already running
    CHECK_FALSE(input.captureLine().valid);

    // Three blocks of 480 frames, delivered 10 ms apart; the second one 3 ms late (jitter): the line is the earliest.
    std::vector<float> block(480);
    const std::int64_t t0 = 1'000'000'000;
    for (int b = 0; b < 3; ++b) {
        for (int i = 0; i < 480; ++i) {
            block[static_cast<std::size_t>(i)] = static_cast<float>(b * 480 + i) / 2000.0f;
        }
        REQUIRE(input.push(block, at(t0 + (b + 1) * 10'000'000 + (b == 1 ? 3'000'000 : 0))));
    }
    const CaptureLine line = input.captureLine();
    REQUIRE(line.valid);
    CHECK(line.frames == 1440);
    CHECK(line.hostTimeNs(0) == t0);
    CHECK(line.hostTimeNs(480) == t0 + 10'000'000);
    CHECK(input.readable() == 1440);

    std::vector<float> got(1000);
    CHECK(input.read(got) == 1000);
    CHECK(input.readPosition() == 1000);
    CHECK(got[999] == Approx(999.0f / 2000.0f));
    CHECK(input.read(got) == 440);
    CHECK(input.readPosition() == 1440);
    CHECK(got[0] == Approx(1000.0f / 2000.0f));
    CHECK(input.stats().framesCaptured == 1440);
    CHECK(input.stats().callbacks == 3);
    CHECK(input.stats().framesDropped == 0);
    input.stop();
    CHECK_FALSE(input.isRunning());
}

TEST_CASE("AudioInput counts what a full capture ring loses and keeps the stream index of the frames after the gap",
          "[audio][voiceover]") {
    AudioInput input;
    AudioInputConfig c = nullInput();
    c.ringSeconds = 0.01; // 480 frames, rounded up to 512
    REQUIRE(input.start(c));
    std::vector<float> block(300);
    for (int b = 0; b < 3; ++b) {
        for (int i = 0; i < 300; ++i) {
            block[static_cast<std::size_t>(i)] = static_cast<float>(b * 300 + i);
        }
        REQUIRE(input.push(block, at(1'000'000'000LL + b * 6'250'000LL)));
    }
    // 900 frames pushed into a 512-frame ring nobody read: 388 lost (the end of block 2, all of block 3).
    CHECK(input.stats().framesDropped == 388);
    std::vector<float> got(1000);
    const std::size_t first = input.read(got);
    CHECK(first == 512);
    CHECK(input.readPosition() == 512);
    CHECK(got[511] == 511.0f);
    // The ring has room again: the next block's frames are stream frames 900.. (after the gap).
    for (int i = 0; i < 300; ++i) {
        block[static_cast<std::size_t>(i)] = static_cast<float>(900 + i);
    }
    REQUIRE(input.push(block, at(1'000'000'000LL + 3 * 6'250'000LL)));
    const std::size_t next = input.read(got);
    REQUIRE(next == 300);
    CHECK(got[0] == 900.0f);
    CHECK(input.readPosition() == 1200);
    CHECK(input.readPosition() - static_cast<std::int64_t>(next) == 900);
}

TEST_CASE("AudioInput meters peak, RMS and clipping since the last read, per channel", "[audio][voiceover]") {
    AudioInput input;
    REQUIRE(input.start(nullInput(2)));
    std::vector<float> block(2 * 480);
    for (std::size_t i = 0; i < 480; ++i) {
        block[2 * i] = (i % 2 == 0) ? 0.5f : -0.5f; // square: RMS 0.5
        block[2 * i + 1] = 1.0f;                    // full scale
    }
    REQUIRE(input.push(block, at(5'000'000'000LL)));
    const InputLevels levels = input.levels();
    CHECK(levels.peak[0] == Approx(0.5f));
    CHECK(levels.rms[0] == Approx(0.5f).margin(1e-6));
    CHECK(levels.peak[1] == Approx(1.0f));
    CHECK(levels.clipped);
    const InputLevels again = input.levels(); // the meter reset
    CHECK(again.peak[0] == 0.0f);
    CHECK(again.rms[0] == 0.0f);
    CHECK_FALSE(again.clipped);
}

TEST_CASE("AudioInput's monitor ring keeps only the newest frames (a bounded monitoring latency)", "[audio][voiceover]") {
    AudioInput input;
    REQUIRE(input.start(nullInput()));
    std::vector<float> block(1000);
    for (int i = 0; i < 1000; ++i) {
        block[static_cast<std::size_t>(i)] = static_cast<float>(i);
    }
    // Monitoring off: nothing reaches the monitor ring.
    REQUIRE(input.push(block, at(1'000'000'000LL)));
    std::vector<float> out(100);
    CHECK(input.readMonitor(out, 64) == 0);
    input.setMonitorEnabled(true);
    REQUIRE(input.push(block, at(1'020'833'333LL)));
    // 1000 frames waiting, 100 asked for with a backlog of 64 kept: the oldest 836 are dropped, frames 836..935 read.
    REQUIRE(input.readMonitor(out, 64) == 100);
    CHECK(out[0] == 836.0f);
    CHECK(out[99] == 935.0f);
    CHECK(input.stats().monitorFramesDropped == 836);
    // The capture ring still has every frame for the recorder.
    CHECK(input.readable() == 2000);
}

TEST_CASE("WavWriter writes 16-, 24-bit PCM and float WAVs, updating the header as it goes", "[audio][voiceover][wav]") {
    const auto dir = scratchDir("writer");
    std::vector<float> ramp(2 * 1000);
    for (std::size_t i = 0; i < ramp.size(); ++i) {
        ramp[i] = static_cast<float>(std::sin(static_cast<double>(i) * 0.01) * 0.8);
    }
    for (const int bits : {16, 24, 32}) {
        INFO(bits);
        const auto file = dir / ("ramp" + std::to_string(bits) + ".wav");
        auto writer = WavWriter::create(file, WavFormat{48000, 2, bits}, 300);
        REQUIRE(writer);
        REQUIRE((*writer)->write(std::span<const float>(ramp).first(2 * 400)));
        // Past a sync (300 frames): the header already covers the 400 frames written.
        const auto mid = bytesOf(file);
        CHECK(u32(mid, 40) == static_cast<std::uint32_t>(400 * 2 * bits / 8));
        CHECK(u32(mid, 4) == 36 + u32(mid, 40));
        REQUIRE((*writer)->write(std::span<const float>(ramp).subspan(2 * 400)));
        REQUIRE((*writer)->finish());
        REQUIRE((*writer)->finish()); // idempotent
        CHECK((*writer)->frames() == 1000);
        const auto b = bytesOf(file);
        CHECK(b.size() == 44 + 1000 * 2 * static_cast<std::size_t>(bits / 8));
        CHECK(std::memcmp(b.data(), "RIFF", 4) == 0);
        CHECK(std::memcmp(b.data() + 8, "WAVEfmt ", 8) == 0);
        CHECK((b[20] | (b[21] << 8)) == (bits == 32 ? 3 : 1));
        CHECK(u32(b, 24) == 48000);
        const auto samples = samplesOf(file);
        REQUIRE(samples.size() == ramp.size());
        const double tolerance = bits == 32 ? 0.0 : bits == 24 ? 1.0 / 8388607.0 : 1.0 / 32767.0;
        for (std::size_t i = 0; i < ramp.size(); i += 97) {
            CHECK(std::fabs(samples[i] - ramp[i]) <= tolerance);
        }
        CHECK_FALSE((*writer)->write(ramp)); // finished
    }
    CHECK_FALSE(WavWriter::create(dir / "bad.wav", WavFormat{48000, 1, 20}));
    CHECK_FALSE(WavWriter::create(dir / "missing-folder" / "x.wav", WavFormat{}));
}

TEST_CASE("A take cut off mid-write is recoverable: recoverWav repairs the header and drops the partial frame",
          "[audio][voiceover][wav]") {
    const auto dir = scratchDir("recover");
    const auto file = dir / "take.wav";
    std::vector<float> voice(4800);
    for (std::size_t i = 0; i < voice.size(); ++i) {
        voice[i] = static_cast<float>(std::sin(static_cast<double>(i) * 0.05) * 0.5);
    }
    {
        auto writer = WavWriter::create(file, WavFormat{48000, 1, 24}, 24000);
        REQUIRE(writer);
        REQUIRE((*writer)->write(voice));
        REQUIRE((*writer)->finish());
    }
    // The crash: the header as it was at the last sync (no frames yet: the sync interval was not reached) and the file
    // cut in the middle of a frame (3 bytes a frame; 2 bytes of frame 4000 made it to the disk).
    {
        auto b = bytesOf(file);
        for (std::size_t i : {4u, 40u}) {
            const std::uint32_t v = i == 4 ? 36u : 0u;
            for (int k = 0; k < 4; ++k) {
                b[i + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(v >> (8 * k));
            }
        }
        b.resize(44 + 4000 * 3 + 2);
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    }
    CHECK(samplesOf(file).empty()); // what a player sees before the repair: an empty WAV, not a broken one
    const auto recovered = recoverWav(file);
    REQUIRE(recovered);
    CHECK(recovered->repaired);
    CHECK(recovered->frames == 4000);
    CHECK(recovered->droppedBytes == 2);
    CHECK(recovered->format == WavFormat{48000, 1, 24});
    CHECK(std::filesystem::file_size(file) == 44 + 4000 * 3);
    const auto samples = samplesOf(file);
    REQUIRE(samples.size() == 4000);
    for (std::size_t i = 0; i < samples.size(); i += 101) {
        CHECK(std::fabs(samples[i] - voice[i]) <= 1.0 / 8388607.0);
    }
    // A sound file is left as it is.
    const auto again = recoverWav(file);
    REQUIRE(again);
    CHECK_FALSE(again->repaired);
    CHECK(again->frames == 4000);
    // Not a WAV / no file.
    {
        std::ofstream junk(dir / "junk.wav", std::ios::binary);
        junk << "this is not a wave file at all, not even close to one....";
    }
    CHECK(recoverWav(dir / "junk.wav").error().code == ErrorCode::InvalidArgument);
    CHECK(recoverWav(dir / "none.wav").error().code == ErrorCode::NotFound);
}

TEST_CASE("A WAV being written is readable at every sync point (the recorder syncs twice a second)", "[audio][voiceover][wav]") {
    const auto dir = scratchDir("live");
    const auto file = dir / "live.wav";
    auto writer = WavWriter::create(file, WavFormat{48000, 1, 24}, 24000);
    REQUIRE(writer);
    std::vector<float> chunk(4800, 0.1f);
    for (int k = 0; k < 10; ++k) {
        REQUIRE((*writer)->write(chunk));
    }
    // 48000 frames written: two syncs. Copied while still open (what a crash leaves), it recovers to what was flushed.
    const auto copy = dir / "copy.wav";
    std::filesystem::copy_file(file, copy);
    const auto b = bytesOf(copy);
    CHECK(u32(b, 40) == 48000 * 3);
    const auto recovered = recoverWav(copy);
    REQUIRE(recovered);
    CHECK(recovered->frames == 48000);
    REQUIRE((*writer)->finish());
}
