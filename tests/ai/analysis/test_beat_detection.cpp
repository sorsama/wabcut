// Beat and tempo detection (C.5): click tracks at 90/120/128/174 BPM with known phases (tempo within 0.5 BPM, beats
// within 10 ms), accented downbeats, a synthesized drum loop, silence, determinism and the JSON form.

#include "AnalysisTestUtil.h"

#include <wabcut/ai/analysis/AnalysisJson.h>
#include <wabcut/ai/analysis/BeatDetection.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstdio>

using namespace wabcut;
using namespace wabcut::ai::analysis;
using namespace wabcut::ai::analysis::test;

namespace {

constexpr int kRate = 22050;

struct Match {
    std::size_t matched = 0;   ///< true beats with a detected beat within 10 ms
    double worstMs = 0.0;      ///< largest error among the matched
    std::size_t spurious = 0;  ///< detected beats more than 10 ms from every true beat
};

Match compare(const BeatAnalysis& a, const std::vector<std::int64_t>& truth) {
    Match m;
    std::vector<double> detected;
    for (const Ticks t : a.beats) {
        detected.push_back(t.toSeconds());
    }
    for (const std::int64_t s : truth) {
        const double t = static_cast<double>(s) / kRate;
        double best = 1e9;
        for (const double d : detected) {
            best = std::min(best, std::abs(d - t));
        }
        if (best <= 0.010) {
            ++m.matched;
            m.worstMs = std::max(m.worstMs, best * 1000.0);
        }
    }
    for (const double d : detected) {
        double best = 1e9;
        for (const std::int64_t s : truth) {
            best = std::min(best, std::abs(d - static_cast<double>(s) / kRate));
        }
        if (best > 0.010) {
            ++m.spurious;
        }
    }
    return m;
}

} // namespace

TEST_CASE("Click tracks: tempo within 0.5 BPM, beats within 10 ms", "[ai][analysis][beats]") {
    const auto [bpm, phase] = GENERATE(std::pair{90.0, 0.137}, std::pair{120.0, 0.25}, std::pair{128.0, 0.05},
                                       std::pair{174.0, 0.3});
    INFO(bpm << " BPM, phase " << phase << " s");
    std::vector<std::int64_t> clicks;
    const auto audio = clickTrack(bpm, phase, 20.0, kRate, 4, &clicks);
    const auto started = std::chrono::steady_clock::now();
    const auto result = detectBeats(MonoAudio{audio, kRate, Ticks{}});
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    REQUIRE(result);
    std::printf("[beats] %.0f BPM click track: detected %.3f BPM, confidence %.2f, %zu/%zu beats, %.1f ms for 20 s\n", bpm,
                result->bpm, result->confidence, result->beats.size(), clicks.size(), ms);
    CHECK(std::abs(result->bpm - bpm) <= 0.5);
    const Match m = compare(*result, clicks);
    INFO("matched " << m.matched << " of " << clicks.size() << ", worst " << m.worstMs << " ms, spurious " << m.spurious);
    CHECK(m.matched >= clicks.size() - 1);
    CHECK(m.spurious == 0);
    CHECK(m.worstMs <= 10.0);
    CHECK(result->confidence > 0.7f);
    // The accented clicks are the downbeats.
    std::size_t onAccents = 0;
    for (const Ticks d : result->downbeats) {
        for (std::size_t k = 0; k < clicks.size(); k += 4) {
            if (std::abs(d.toSeconds() - static_cast<double>(clicks[k]) / kRate) <= 0.010) {
                ++onAccents;
            }
        }
    }
    CHECK(onAccents == result->downbeats.size());
    CHECK(result->downbeats.size() >= clicks.size() / 4 - 1);
}

TEST_CASE("A synthesized drum loop", "[ai][analysis][beats]") {
    std::vector<std::int64_t> beats;
    const auto audio = drumLoop(105.0, 0.2, 20.0, kRate, &beats);
    const auto result = detectBeats(MonoAudio{audio, kRate, Ticks{}});
    REQUIRE(result);
    std::printf("[beats] 105 BPM drum loop: detected %.3f BPM, confidence %.2f, %zu/%zu beats\n", result->bpm,
                result->confidence, result->beats.size(), beats.size());
    CHECK(std::abs(result->bpm - 105.0) <= 0.5);
    const Match m = compare(*result, beats);
    INFO("matched " << m.matched << " of " << beats.size() << ", worst " << m.worstMs << " ms, spurious " << m.spurious);
    CHECK(m.matched >= beats.size() - 2);
    CHECK(m.spurious == 0);
}

TEST_CASE("Beat times keep the audio's origin and are deterministic; silence has no beats", "[ai][analysis][beats]") {
    const auto audio = clickTrack(120.0, 0.25, 10.0, kRate, 4);
    const Ticks origin = Ticks::fromSeconds(5);
    const auto shifted = detectBeats(MonoAudio{audio, kRate, origin});
    const auto plain = detectBeats(MonoAudio{audio, kRate, Ticks{}});
    REQUIRE(shifted);
    REQUIRE(plain);
    REQUIRE(shifted->beats.size() == plain->beats.size());
    CHECK(shifted->beats.front() == plain->beats.front() + origin);
    CHECK(*plain == *detectBeats(MonoAudio{audio, kRate, Ticks{}}));
    // Beats are sample-exact positions.
    CHECK(plain->beats.front().value % samplesToTicks(1, Rate{kRate, 1}).value == 0);

    const std::vector<float> silence(static_cast<std::size_t>(5 * kRate), 0.0f);
    const auto quiet = detectBeats(MonoAudio{silence, kRate, Ticks{}});
    REQUIRE(quiet);
    CHECK(quiet->beats.empty());
    CHECK(quiet->bpm == 0.0);

    CHECK_FALSE(detectBeats(MonoAudio{audio, 1000, Ticks{}}));
    const auto stopped = detectBeats(MonoAudio{audio, kRate, Ticks{}}, {}, [](double) { return false; });
    REQUIRE_FALSE(stopped);
    CHECK(stopped.error().code == ErrorCode::Cancelled);
}

TEST_CASE("Beat results round-trip through JSON; trackers plug in by id", "[ai][analysis][beats]") {
    const auto audio = clickTrack(128.0, 0.1, 8.0, kRate, 4);
    const auto result = detectBeats(MonoAudio{audio, kRate, Ticks{}});
    REQUIRE(result);
    const auto back = beatsFromJson(toJson(*result));
    REQUIRE(back);
    CHECK(*back == *result);
    CHECK(result->tracker == "dsp-1");
    CHECK_FALSE(beatsFromJson("{\"schema\":\"wabcut.shots\",\"version\":1}"));
    REQUIRE(makeBeatTracker("dsp"));
    CHECK(makeBeatTracker("dsp")->id() == "dsp-1");
    CHECK(makeBeatTracker("beat-this") == nullptr); // the ONNX tracker plugs in later
    BeatOptions narrow;
    narrow.minBpm = 200.0;
    narrow.maxBpm = 100.0;
    CHECK_FALSE(detectBeats(MonoAudio{audio, kRate, Ticks{}}, narrow));
}
