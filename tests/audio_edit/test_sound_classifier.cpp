// p02 audio: Essential Sound auto-tagging's classifier on synthetic dialogue, music, sound effects and ambience, at two
// sample rates and under background noise; and the features it reads (printed by the hidden report test).

#include "SoundTestSignals.h"

#include <wabcut/audio/edit/SoundClassifier.h>

#include <catch2/catch_test_macros.hpp>

#include <iostream>
#include <string>
#include <vector>

using namespace wabcut::audio::edit;
namespace sounds = wabcut::test::sounds;

namespace {

/// `x` at 48 kHz (each 16 kHz sample three times, then smoothed): the rate media decodes at.
std::vector<float> at48k(const std::vector<float>& x) {
    std::vector<float> out;
    out.reserve(x.size() * 3);
    for (const float v : x) {
        out.insert(out.end(), {v, v, v});
    }
    return out;
}

std::string describe(const SoundGuess& g) {
    const SoundFeatures& f = g.features;
    return std::string(toString(g.type)) + " confidence " + std::to_string(g.confidence) + " scores d/m/s/a " +
           std::to_string(g.scores[0]) + " " + std::to_string(g.scores[1]) + " " + std::to_string(g.scores[2]) + " " +
           std::to_string(g.scores[3]) + " | syllabic " + std::to_string(f.syllabicModulation) + " voiced " +
           std::to_string(f.voicedShare) + " glide " + std::to_string(f.glideShare) + " steady " +
           std::to_string(f.steadyShare) + " pause " + std::to_string(f.pauseShare) + " beat " + std::to_string(f.beatRegularity) +
           " flat " + std::to_string(f.flatness) + " crest " + std::to_string(f.crestDb) + " spread " +
           std::to_string(f.levelSpreadDb) + " level " + std::to_string(f.levelDb);
}

} // namespace

TEST_CASE("Auto-tag: synthetic dialogue music effects and ambience get their Essential Sound types", "[audio][edit][autotag]") {
    struct Case {
        const char* name;
        std::vector<float> samples;
        SoundType type;
    };
    const std::vector<Case> cases{
        {"speech", sounds::speech(12.0, 7, 130.0), SoundType::Dialogue},
        {"speech, higher voice", sounds::speech(12.0, 21, 210.0), SoundType::Dialogue},
        {"music 120 bpm", sounds::music(12.0, 120.0), SoundType::Music},
        {"music 96 bpm", sounds::music(12.0, 96.0, 9), SoundType::Music},
        {"effects", sounds::effects(12.0), SoundType::Sfx},
        {"effects, other seed", sounds::effects(12.0, 29), SoundType::Sfx},
        {"ambience", sounds::ambience(12.0), SoundType::Ambience},
        {"speech over a room tone", sounds::mixed(sounds::speech(12.0, 13), sounds::ambience(12.0, 2), 0.08), SoundType::Dialogue},
    };
    for (const Case& c : cases) {
        const SoundGuess g = classifySound(c.samples, sounds::kRate);
        INFO(c.name << ": " << describe(g));
        CHECK_FALSE(g.silent);
        CHECK(g.type == c.type);
        CHECK(g.confidence > 0.05);
        // 48 kHz (what the media tagger decodes) says the same.
        CHECK(classifySound(at48k(c.samples), 48000).type == c.type);
    }
}

TEST_CASE("Auto-tag: silence proposes nothing and the result is deterministic", "[audio][edit][autotag]") {
    const std::vector<float> quiet(16000 * 5, 0.0f);
    CHECK(classifySound(quiet, sounds::kRate).silent);
    const std::vector<float> talk = sounds::speech(8.0, 3);
    const SoundGuess a = classifySound(talk, sounds::kRate);
    const SoundGuess b = classifySound(talk, sounds::kRate);
    CHECK(a.type == b.type);
    CHECK(a.scores == b.scores);
    CHECK(classifySound(std::vector<float>(100, 0.1f), sounds::kRate).silent); // too short to judge
}

TEST_CASE("Auto-tag feature report", "[.report][audio][edit][autotag]") {
    for (const auto& [name, samples] : std::vector<std::pair<std::string, std::vector<float>>>{
             {"speech", sounds::speech(12.0)},
             {"music", sounds::music(12.0)},
             {"effects", sounds::effects(12.0)},
             {"ambience", sounds::ambience(12.0)}}) {
        std::cout << name << ": " << describe(classifySound(samples, sounds::kRate)) << "\n";
    }
}
