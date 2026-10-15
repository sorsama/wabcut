// A.5 audio sync over real files (the media-backed AudioSyncService): synthetic recordings written as WAV (a 48 kHz
// stereo camera, a 44.1 kHz mono recorder, a camera that split its recording into three files) decoded with
// AudioDecoder; offsets within a sample at 48 kHz, the split files put in recording order, results cached by the
// files' fingerprints. The speed figure: WABCUT_TEST_SYNC_BENCH_DIR with four hour-long angles (see the benchmark).

#include "SyncTestSignals.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/audio/edit/MediaAnalysis.h>
#include <wabcut/media/Probe.h>
#include <wabcut/model/MediaLocator.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace wabcut;
using namespace wabcut::audio::edit;
using namespace wabcut::test;

namespace {

std::filesystem::path outputDir(const std::string& name) {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "audio_edit" / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

/// A 16-bit PCM WAV of `seconds` of the recording at `rate`, `channels` channels (the same sound in each).
void writeWav(const std::filesystem::path& file, RecordingSignal& signal, int rate, int channels, std::int64_t first,
              std::int64_t frames) {
    std::ofstream out(file, std::ios::binary);
    const auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const auto bytes = static_cast<std::uint32_t>(frames * channels * 2);
    out.write("RIFF", 4);
    u32(36 + bytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(static_cast<std::uint16_t>(channels));
    u32(static_cast<std::uint32_t>(rate));
    u32(static_cast<std::uint32_t>(rate * channels * 2));
    u16(static_cast<std::uint16_t>(channels * 2));
    u16(16);
    out.write("data", 4);
    u32(bytes);
    std::vector<float> block(static_cast<std::size_t>(rate));
    std::vector<std::int16_t> pcm;
    for (std::int64_t at = 0; at < frames; at += rate) {
        const auto n = static_cast<std::size_t>(std::min<std::int64_t>(rate, frames - at));
        REQUIRE(signal.read(rate, first + at, std::span<float>(block.data(), n)));
        pcm.clear();
        for (std::size_t i = 0; i < n; ++i) {
            const auto v = static_cast<std::int16_t>(std::lround(std::clamp(block[i], -1.0f, 1.0f) * 32767.0f));
            for (int c = 0; c < channels; ++c) {
                pcm.push_back(v);
            }
        }
        out.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(pcm.size() * 2));
    }
}

MediaId addMedia(ProjectState& state, const std::filesystem::path& file, std::int64_t modifiedTime = 0) {
    auto info = media::probe(pathToUtf8(file));
    REQUIRE(info);
    MediaItem item;
    item.id = state.ids.allocate<MediaId>();
    item.name = pathToUtf8(file.filename());
    item.locator.path = pathToUtf8(file);
    item.info = *info;
    if (modifiedTime != 0) {
        item.info.modifiedTime = modifiedTime;
    }
    const MediaId id = item.id;
    state.media.push_back(std::make_shared<const MediaItem>(std::move(item)));
    return id;
}

std::int64_t samples48(double seconds) { return std::llround(seconds * 48000.0); }

} // namespace

TEST_CASE("Audio sync of real files: a 48 kHz camera and a 44.1 kHz recorder to the sample, cached by fingerprint",
          "[audio][edit][sync][media]") {
    const auto dir = outputDir("sync-media");
    const SyntheticEvent event(80.0, 51);
    RecordingSignal camera(event, {0.0, 40.0, 0.3, 0.01, true});
    RecordingSignal recorder(event, {6.789, 40.0, 1.0, 0.002});
    writeWav(dir / "camera.wav", camera, 48000, 2, 0, 40 * 48000);
    writeWav(dir / "recorder.wav", recorder, 44100, 1, 0, 40 * 44100);
    ProjectState state;
    const MediaId cam = addMedia(state, dir / "camera.wav");
    const MediaId rec = addMedia(state, dir / "recorder.wav");

    auto cache = std::make_shared<ai::AnalysisCache>(dir / "cache");
    auto sync = makeMediaAudioSync(cache);
    const std::vector<AudioSyncService::Angle> angles{{{cam}}, {{rec}}};
    auto r = sync->sync(state, angles, 0, {}, {}, {});
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    const SyncMatch& m = r->report.matches[1];
    INFO(m.problem);
    REQUIRE(m.synced);
    CHECK(std::llabs(m.offset - samples48(6.789)) <= 1);
    CHECK(r->starts[1].front() == samplesToTicks(m.offset, Rate{48000, 1}));
    CHECK_FALSE(r->cached);
    std::printf("audio sync, 40 s camera + 44.1 kHz recorder: %.3f s, offset error %lld samples, confidence %.2f\n",
                r->report.seconds, static_cast<long long>(m.offset - samples48(6.789)), m.confidence);

    // Again: from the cache, the same answer.
    auto again = sync->sync(state, angles, 0, {}, {}, {});
    REQUIRE(again);
    CHECK(again->cached);
    CHECK(again->report.matches[1].offset == m.offset);
    // Other options are another key.
    SyncOptions strict;
    strict.minConfidence = 0.9;
    auto other = sync->sync(state, angles, 0, strict, {}, {});
    REQUIRE(other);
    CHECK_FALSE(other->cached);

    // A cancel stops it.
    auto uncached = makeMediaAudioSync(nullptr);
    std::stop_source stop;
    stop.request_stop();
    auto cancelled = uncached->sync(state, angles, 0, {}, stop.get_token(), {});
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == ErrorCode::Cancelled);
    // A missing medium.
    const std::vector<AudioSyncService::Angle> bad{{{cam}}, {{MediaId{999}}}};
    CHECK_FALSE(uncached->sync(state, bad, 0, {}, {}, {}));
}

TEST_CASE("Audio sync of a camera that split its recording into three files: played in recording order",
          "[audio][edit][sync][media]") {
    const auto dir = outputDir("sync-split");
    const SyntheticEvent event(120.0, 61);
    RecordingSignal camera(event, {2.0, 90.0, 0.5, 0.01});
    RecordingSignal recorder(event, {0.0, 100.0});
    // The camera's 90 s in three files of 30 s, named so that their names do not sort in order.
    writeWav(dir / "C003.wav", camera, 48000, 2, 0, 30 * 48000);
    writeWav(dir / "C001.wav", camera, 48000, 2, 30 * 48000, 30 * 48000);
    writeWav(dir / "C002.wav", camera, 48000, 2, 60 * 48000, 30 * 48000);
    writeWav(dir / "recorder.wav", recorder, 48000, 1, 0, 100 * 48000);
    ProjectState state;
    const MediaId first = addMedia(state, dir / "C003.wav", 1000);
    const MediaId second = addMedia(state, dir / "C001.wav", 1030);
    const MediaId third = addMedia(state, dir / "C002.wav", 1060);
    const MediaId rec = addMedia(state, dir / "recorder.wav");
    auto sync = makeMediaAudioSync(nullptr);
    // Given out of order: they are played as recorded (by the files' times).
    const std::vector<AudioSyncService::Angle> angles{{{rec}}, {{third, first, second}}};
    auto r = sync->sync(state, angles, 0, {}, {}, {});
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    REQUIRE(r->report.matches[1].synced);
    CHECK(r->order[1] == std::vector<MediaId>{first, second, third});
    // The recording starts 2 s into the recorder's; each file 30 s after the one before.
    const Rate rate{48000, 1};
    REQUIRE(r->starts[1].size() == 3);
    CHECK(std::llabs(ticksToSamples(r->starts[1][0], rate) - samples48(2.0)) <= 1);
    CHECK(std::llabs(ticksToSamples(r->starts[1][1], rate) - samples48(32.0)) <= 1);
    CHECK(std::llabs(ticksToSamples(r->starts[1][2], rate) - samples48(62.0)) <= 1);
    // Kept as listed when asked: the wrong order matches worse or not at all, and the order is as given.
    std::vector<AudioSyncService::Angle> asListed = angles;
    asListed[1].keepOrder = true;
    auto listed = sync->sync(state, asListed, 0, {}, {}, {});
    REQUIRE(listed);
    CHECK(listed->order[1] == std::vector<MediaId>{third, first, second});
}

TEST_CASE("Audio sync benchmark source: 65 minutes of the synthetic event (WABCUT_TEST_SYNC_BENCH_DIR)", "[.sync-bench-source]") {
    // Writes <dir>/source.wav (48 kHz mono); ffmpeg cuts the four angles from it (docs/ai.md "Audio sync").
    const char* env = std::getenv("WABCUT_TEST_SYNC_BENCH_DIR");
    if (env == nullptr || *env == '\0') {
        SKIP("set WABCUT_TEST_SYNC_BENCH_DIR");
    }
    const SyntheticEvent event(3950.0, 71);
    RecordingSignal source(event, {0.0, 3900.0, 1.0, 0.003});
    std::filesystem::create_directories(env);
    writeWav(std::filesystem::path(env) / "source.wav", source, 48000, 1, 0, 3900LL * 48000);
}

TEST_CASE("Audio sync speed: four hour-long angles from files (WABCUT_TEST_SYNC_BENCH_DIR)", "[audio][edit][sync][media][benchmark]") {
    const char* env = std::getenv("WABCUT_TEST_SYNC_BENCH_DIR");
    if (env == nullptr || *env == '\0') {
        SKIP("set WABCUT_TEST_SYNC_BENCH_DIR to a folder with angle1..angle4 (.m4a or .wav) and offsets.txt");
    }
    const std::filesystem::path dir(env);
    ProjectState state;
    std::vector<AudioSyncService::Angle> angles;
    for (int i = 1; i <= 4; ++i) {
        std::filesystem::path file = dir / ("angle" + std::to_string(i) + ".m4a");
        if (!std::filesystem::exists(file)) {
            file = dir / ("angle" + std::to_string(i) + ".wav");
        }
        REQUIRE(std::filesystem::exists(file));
        angles.push_back({{addMedia(state, file)}});
    }
    // offsets.txt: the true start of each angle on angle 1's clock, in seconds, one per line.
    std::ifstream offsets(dir / "offsets.txt");
    std::vector<double> truth(4, 0.0);
    for (double& t : truth) {
        offsets >> t;
    }
    auto sync = makeMediaAudioSync(nullptr);
    const auto started = std::chrono::steady_clock::now();
    auto r = sync->sync(state, angles, 0, {}, {}, {});
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    REQUIRE(r);
    double audio = 0.0;
    for (const MediaPtr& m : state.media) {
        audio += m->info.duration.toSeconds();
    }
    std::printf("audio sync: four angles (%.0f min of audio) from files in %.2f s\n", audio / 60.0, seconds);
    for (int i = 1; i < 4; ++i) {
        const SyncMatch& m = r->report.matches[static_cast<std::size_t>(i)];
        std::printf("  angle %d: offset error %lld samples, confidence %.2f, drift %.2f ppm\n", i + 1,
                    static_cast<long long>(m.offset - samples48(truth[static_cast<std::size_t>(i)])), m.confidence, m.driftPpm);
        CHECK(m.synced);
        CHECK(std::llabs(m.offset - samples48(truth[static_cast<std::size_t>(i)])) <= 2);
    }
    CHECK(seconds < 30.0);
}
