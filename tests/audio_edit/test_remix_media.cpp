// Music Remix over real files (A.7, the media-backed MusicAnalyzer): the synthetic song (MusicTestSignals.h) written as a
// 44.1 kHz stereo WAV, decoded with AudioDecoder, its beats tracked and its features computed; the analysis cached by
// the file's fingerprint; a take rendered piece by piece from the file is the plan's in-memory render; and the analysis
// time of a five-minute file, decoding included.

#include "MusicTestSignals.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/audio/edit/MediaAnalysis.h>
#include <wabcut/media/AudioDecoder.h>
#include <wabcut/media/Probe.h>
#include <wabcut/model/MediaLocator.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace wabcut;
using namespace wabcut::audio::edit;
using namespace wabcut::test;
using Catch::Approx;

namespace {

std::filesystem::path outputDir(const std::string& name) {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "audio_edit" / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

/// A 16-bit stereo WAV of a mono song (the same in both channels).
void writeWav(const std::filesystem::path& file, const SyntheticSong& song) {
    std::ofstream out(file, std::ios::binary);
    const auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const auto bytes = static_cast<std::uint32_t>(song.samples.size() * 4);
    out.write("RIFF", 4);
    u32(36 + bytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(2);
    u32(static_cast<std::uint32_t>(song.rate));
    u32(static_cast<std::uint32_t>(song.rate * 4));
    u16(4);
    u16(16);
    out.write("data", 4);
    u32(bytes);
    std::vector<std::int16_t> pcm;
    pcm.reserve(song.samples.size() * 2);
    for (const float s : song.samples) {
        const auto v = static_cast<std::int16_t>(std::lround(std::clamp(s * 0.8f, -1.0f, 1.0f) * 32767.0f));
        pcm.push_back(v);
        pcm.push_back(v);
    }
    out.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(pcm.size() * 2));
}

MediaItem itemOf(const std::filesystem::path& file) {
    auto info = media::probe(pathToUtf8(file));
    REQUIRE(info);
    MediaItem item;
    item.id = MediaId{1};
    item.name = pathToUtf8(file.filename());
    item.locator.path = pathToUtf8(file);
    item.info = *info;
    return item;
}

Ticks secs(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

} // namespace

TEST_CASE("Music Remix over a real file: analysis cached by fingerprint, a take that is the plan's render",
          "[audio][edit][remix][media]") {
    const auto dir = outputDir("remix-media");
    const SyntheticSong song = synthesizeSong("IIIIAAAAAAAABBBBBBBBAAAAAAAABBBBBBBBOOOO", 120.0, 44100);
    const auto wav = dir / "song.wav";
    writeWav(wav, song);
    const MediaItem item = itemOf(wav);
    auto cache = std::make_shared<ai::AnalysisCache>(dir / "cache");
    auto analyzer = makeMediaMusicAnalyzer(cache, dir / "takes");

    const auto t0 = std::chrono::steady_clock::now();
    auto analysis = analyzer->analyze(item, 0, {}, {});
    const double first = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(analysis);
    CHECK(analysis->bpm == Approx(120.0).margin(1.0));
    CHECK(analysis->beatsPerBar == 4);
    CHECK(analysis->features.size() + 1 == analysis->beats.size());
    CHECK(analysis->duration.toSeconds() == Approx(80.0).margin(0.01));
    // Cached: the same analysis, without decoding again.
    const auto t1 = std::chrono::steady_clock::now();
    auto again = analyzer->analyze(item, 0, {}, {});
    const double second = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    REQUIRE(again);
    CHECK(again->beats == analysis->beats);
    CHECK(again->features.size() == analysis->features.size());
    CHECK(second < first);
    WARN("80 s file: analysis " << first << " s, from the cache " << second << " s");

    // The plan: 80 s to 48 s (a verse and a chorus fewer), the intro and the outro found.
    RemixOptions options;
    options.target = secs(48.0);
    auto plan = planRemix(*analysis, TimeRange{Ticks{}, analysis->duration}, options);
    REQUIRE(plan);
    CHECK(plan->introEnd.toSeconds() == Approx(8.0).margin(0.05));
    CHECK(plan->outroStart.toSeconds() == Approx(72.0).margin(0.05));
    CHECK(std::fabs(plan->duration.toSeconds() - 48.0) <= 0.5);
    REQUIRE_FALSE(plan->joins.empty());

    // The take: rendered from the file piece by piece, as the in-memory render of the decoded file.
    auto take = analyzer->render(item, 0, *plan, {}, {});
    REQUIRE(take);
    CHECK(std::filesystem::exists(pathFromUtf8(take->path)));
    REQUIRE(take->info.audio.size() == 1);
    CHECK(take->info.audio[0].channels == 2);
    CHECK(take->info.audio[0].sampleRate == Rate{44100, 1});
    CHECK(take->info.duration.toSeconds() == Approx(plan->duration.toSeconds()).margin(0.001));
    CHECK(take->fingerprint.isKnown());
    auto decoder = media::AudioDecoder::open(pathToUtf8(wav), -1, Rate{44100, 1}, 2);
    REQUIRE(decoder);
    std::vector<float> source(static_cast<std::size_t>(song.samples.size()) * 2);
    REQUIRE((*decoder)->read(Ticks{}, source));
    const std::vector<float> expected = renderRemix(*plan, source, 2, 44100);
    auto takeDecoder = media::AudioDecoder::open(take->path, -1, Rate{44100, 1}, 2);
    REQUIRE(takeDecoder);
    std::vector<float> rendered(expected.size());
    REQUIRE((*takeDecoder)->read(Ticks{}, rendered));
    double worst = 0.0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        worst = std::max(worst, static_cast<double>(std::fabs(rendered[i] - expected[i])));
    }
    CHECK(worst < 1e-5);
    // The same plan again: the take is found, not rendered again.
    auto reused = analyzer->render(item, 0, *plan, {}, {});
    REQUIRE(reused);
    CHECK(reused->path == take->path);
}

TEST_CASE("Music Remix analysis time of a five-minute file (decoding included)", "[audio][edit][remix][media][perf]") {
    const auto dir = outputDir("remix-media-5min");
    std::string sections = "IIII";
    while (sections.size() < 146) {
        sections += "AAAAAAAABBBBBBBB";
    }
    sections.resize(146);
    sections += "OOOO";
    const SyntheticSong song = synthesizeSong(sections, 120.0, 44100);
    const auto wav = dir / "five-minutes.wav";
    writeWav(wav, song);
    const MediaItem item = itemOf(wav);
    auto analyzer = makeMediaMusicAnalyzer(nullptr, dir / "takes");
    const auto t0 = std::chrono::steady_clock::now();
    auto analysis = analyzer->analyze(item, 0, {}, {});
    const double analysed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(analysis);
    RemixOptions options;
    options.target = secs(150.0);
    const auto t1 = std::chrono::steady_clock::now();
    auto plan = planRemix(*analysis, TimeRange{Ticks{}, analysis->duration}, options);
    const double planned = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    REQUIRE(plan);
    CHECK(std::fabs(plan->duration.toSeconds() - 150.0) <= 0.5);
    WARN("5-minute 44.1 kHz stereo WAV: decode + beats + features " << analysed << " s, plan to 2:30 " << planned << " s ("
                                                                     << analysis->beats.size() << " beats)");
    CHECK(analysed + planned < 30.0);
}
