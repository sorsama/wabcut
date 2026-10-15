// Media analysis through the real decoders (full builds, C.5): FFmpeg-generated shot fixtures (hard cuts, dissolve,
// fade, flashes), the speed at 1080p, the analysis cache, beats from a WAV file, the provider and the CLI.

#include "AnalysisTestUtil.h"

#include <wabcut/ai/ChildProcess.h>
#include <wabcut/ai/analysis/MediaAnalysis.h>
#include <wabcut/model/MediaLocator.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>

using namespace wabcut;
using namespace wabcut::ai::analysis;
using namespace wabcut::ai::analysis::test;
using namespace std::chrono_literals;

namespace {

const std::filesystem::path& mediaDir() {
    static const std::filesystem::path dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "ai" / "analysis-media";
    return dir;
}

/// The fixture. Under ctest the ai suite's fixture step (ai.generate_analysis_media) has made them; run directly, the
/// first test that needs them runs gen_analysis_media.py, which is safe next to other processes doing the same (each
/// encodes into its own temporary file and moves it into place atomically). ".complete" is written last.
std::string fixture(const char* name) {
    static std::once_flag generated;
    std::call_once(generated, [] {
        if (std::filesystem::exists(mediaDir() / ".complete")) {
            return;
        }
        ai::ChildProcessOptions options;
        options.executable = WABCUT_PYTHON;
        options.arguments = {WABCUT_ANALYSIS_MEDIA_SCRIPT, "--out", pathToUtf8(mediaDir()), "--ffmpeg", WABCUT_FFMPEG};
        auto child = ai::ChildProcess::spawn(options);
        REQUIRE(child);
        REQUIRE((*child)->wait(300s) == 0);
    });
    const auto path = mediaDir() / name;
    REQUIRE(std::filesystem::exists(path));
    return pathToUtf8(path);
}

std::vector<std::int64_t> frames(const ShotAnalysis& a) {
    std::vector<std::int64_t> out;
    for (const ShotCut& c : a.cuts) {
        out.push_back(c.frame);
    }
    return out;
}

ShotAnalysis analysed(const char* name) {
    MediaAnalyzer analyzer(MediaAnalyzerOptions{});
    auto result = analyzer.shots(fixture(name), -1, ShotOptions{});
    REQUIRE(result);
    return *result;
}

} // namespace

TEST_CASE("Shot fixtures: hard cuts exact, one cut per dissolve and fade, no cut for flashes", "[ai][analysis][media]") {
    MediaAnalyzer analyzer(MediaAnalyzerOptions{});
    MediaAnalyzer::Stats stats;
    const auto cuts = analyzer.shots(fixture("cuts_1080p.mp4"), -1, ShotOptions{}, {}, &stats);
    REQUIRE(cuts);
    CHECK(frames(*cuts) == std::vector<std::int64_t>{50, 100, 150, 200});
    for (const ShotCut& c : cuts->cuts) {
        CHECK(c.kind == CutKind::Cut);
        CHECK(c.time == framesToTicks(c.frame, kFps));
    }
    const double factor = stats.mediaDuration.toSeconds() / stats.seconds;
    std::printf("[shots] 1080p H.264, 250 frames: %.2f s with the %s decoder = %.1fx realtime\n", stats.seconds,
                stats.backend.c_str(), factor);
    CHECK(factor > 5.0); // ~17-35x on the development machine; the goal is > 10x

    const ShotAnalysis dissolve = analysed("dissolve.mp4");
    REQUIRE(dissolve.cuts.size() == 1);
    CHECK(dissolve.cuts[0].kind == CutKind::Dissolve);
    CHECK(dissolve.cuts[0].frame >= 56);
    CHECK(dissolve.cuts[0].frame <= 70);

    const ShotAnalysis fade = analysed("fade.mp4");
    REQUIRE(fade.cuts.size() == 1);
    CHECK(fade.cuts[0].kind == CutKind::Fade);
    CHECK(fade.cuts[0].frame >= 50);
    CHECK(fade.cuts[0].frame <= 60);

    CHECK(frames(analysed("flash.mp4")) == std::vector<std::int64_t>{90});
}

TEST_CASE("Analyses are cached per source fingerprint and settings", "[ai][analysis][media]") {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "ai" / "analysis-cache";
    std::filesystem::remove_all(dir);
    MediaAnalyzerOptions options;
    options.cacheRoot = dir;
    MediaAnalyzer analyzer(options);
    MediaAnalyzer::Stats first;
    MediaAnalyzer::Stats second;
    const auto a = analyzer.shots(fixture("flash.mp4"), -1, ShotOptions{}, {}, &first);
    const auto b = analyzer.shots(fixture("flash.mp4"), -1, ShotOptions{}, {}, &second);
    REQUIRE(a);
    REQUIRE(b);
    CHECK_FALSE(first.fromCache);
    CHECK(second.fromCache);
    CHECK(*a == *b);
    ShotOptions other;
    other.detector = ShotDetector::Content;
    MediaAnalyzer::Stats third;
    REQUIRE(analyzer.shots(fixture("flash.mp4"), -1, other, {}, &third));
    CHECK_FALSE(third.fromCache); // other settings, another entry
}

TEST_CASE("Beats of a WAV file through the decoder", "[ai][analysis][media]") {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "ai" / "analysis-beats";
    std::filesystem::create_directories(dir);
    const auto wav = dir / "clicks_120.wav";
    std::vector<std::int64_t> clicks;
    writeWav(wav, clickTrack(120.0, 0.25, 12.0, 44100, 4, &clicks), 44100);
    MediaAnalyzer analyzer(MediaAnalyzerOptions{});
    const auto beats = analyzer.beats(pathToUtf8(wav), -1, BeatOptions{});
    REQUIRE(beats);
    CHECK(std::abs(beats->bpm - 120.0) <= 0.5);
    REQUIRE(beats->beats.size() >= clicks.size() - 1);
    for (std::size_t i = 0; i < beats->beats.size(); ++i) {
        double best = 1e9;
        for (const std::int64_t c : clicks) {
            best = std::min(best, std::abs(beats->beats[i].toSeconds() - static_cast<double>(c) / 44100.0));
        }
        CHECK(best <= 0.010);
    }
    CHECK(beats->downbeats.size() >= clicks.size() / 4 - 1);
}

TEST_CASE("MediaAnalysisProvider resolves media items; generated items have nothing to analyse", "[ai][analysis][media]") {
    MediaAnalysisProvider provider(MediaAnalyzerOptions{}, std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "p.wabcut");
    MediaItem item;
    item.id = MediaId{3};
    item.name = "flash";
    item.locator.path = fixture("flash.mp4");
    VideoStreamInfo video;
    video.index = 0;
    item.info.video.push_back(video);
    const auto shots = provider.shots(item, 0, ShotOptions{}, {});
    REQUIRE(shots);
    CHECK(frames(*shots) == std::vector<std::int64_t>{90});

    MediaItem title = item;
    title.locator = MediaLocator{};
    title.generator = GeneratorSpec{"wabcut.title", "Hi", "Arial", "Regular"};
    CHECK(provider.shots(title, 0, ShotOptions{}, {}).error().code == ErrorCode::Unsupported);
    MediaItem missing = item;
    missing.locator.path = pathToUtf8(mediaDir() / "nope.mp4");
    CHECK(provider.shots(missing, 0, ShotOptions{}, {}).error().code == ErrorCode::NotFound);
}

TEST_CASE("wabcut-cli analyze", "[ai][analysis][media][cli]") {
    ai::ChildProcessOptions options;
    options.executable = WABCUT_CLI_PATH;
    options.arguments = {"analyze", fixture("cuts_1080p.mp4"), "--shots", "--json", "--no-cache"};
    auto child = ai::ChildProcess::spawn(options);
    REQUIRE(child);
    std::string out;
    std::string line;
    while ((*child)->readLine(line, 60s) == ai::ChildProcess::ReadStatus::Line) {
        out += line + "\n";
    }
    REQUIRE((*child)->wait(60s) == 0);
    const auto json = nlohmann::json::parse(out);
    REQUIRE(json["shots"]["cuts"].size() == 4);
    CHECK(json["shots"]["cuts"][0]["frame"] == 50);
    CHECK(json["shots"]["stats"]["fromCache"] == false);
    CHECK_FALSE(json.contains("beats"));
}
