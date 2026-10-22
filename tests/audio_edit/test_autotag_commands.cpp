// p02 audio: audio.autoTag through the CommandEngine: proposals in a dry run, tags written into the clips' Essential
// Sound records in one undo step, types set by hand kept unless "overwrite", silent clips skipped; with a fake tagger
// that hears each media item as the test says.

#include "CommandHarness.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/audio/edit/AudioAnalysis.h>
#include <wabcut/audio/edit/EssentialSound.h>

#include <catch2/catch_test_macros.hpp>

#include <map>

using namespace wabcut;
using namespace wabcut::audio::edit;
using namespace wabcut::test;
using command::Json;

namespace {

constexpr std::string_view kPicture = R"(
    V1 tgt |Aaaaaaaaaa|
    A1 tgt |Aaaaaaaaaa|
    A2     |Mmmmmmmmmm|
    A3     |..Ssssss..|
)";

class FakeTagger final : public SoundTagger {
public:
    std::map<std::uint64_t, SoundGuess> guesses; ///< by media id
    int calls = 0;
    std::expected<SoundGuess, Error> classify(const ProjectState&, const MediaItem& item, int, TimeRange,
                                             command::ExecutionControl*) override {
        ++calls;
        const auto it = guesses.find(item.id.value);
        if (it == guesses.end()) {
            SoundGuess silent;
            silent.silent = true;
            return silent;
        }
        return it->second;
    }
};

SoundGuess guess(SoundType type, double confidence) {
    SoundGuess g;
    g.type = type;
    g.confidence = confidence;
    g.scores = {0.1, 0.1, 0.1, 0.1};
    g.scores[static_cast<std::size_t>(type)] = 0.8;
    return g;
}

} // namespace

TEST_CASE("audio.autoTag proposes in a dry run and tags in one undo step, keeping types set by hand",
          "[audio][edit][autotag][commands]") {
    Timeline tl(kPicture);
    auto services = std::make_shared<AudioServices>(nullptr, nullptr);
    auto tagger = std::make_shared<FakeTagger>();
    tagger->guesses[tl.media('A').value] = guess(SoundType::Dialogue, 0.8);
    tagger->guesses[tl.media('M').value] = guess(SoundType::Music, 0.6);
    services->tagger = tagger;
    CommandHarness h(std::move(tl), command::EnginePolicy{command::CapabilitySet::all(), 64, services}, ai::edit::editingCommands());
    const ClipId dialogue = h.clip("A1@0");
    const ClipId music = h.clip("A2@0");
    const ClipId silent = h.clip("A3@2");
    const auto typeOf = [&](ClipId id) { return recordedSoundType(h.sequence(h.state()).findClip(id)->clip().effects()); };

    // A dry run proposes and changes nothing.
    const auto preview = h.run(Json{{"command", "audio.autoTag"}, {"params", Json::object()}, {"dryRun", true}});
    REQUIRE(preview.ok());
    CHECK(preview.dryRun);
    CHECK_FALSE(typeOf(dialogue).has_value());

    const std::size_t undo = h.session.undoCount();
    auto r = h.ok("audio.autoTag");
    CHECK(h.session.undoCount() == undo + 1);
    CHECK(r.data["tagged"] == 2);
    CHECK(typeOf(dialogue) == SoundType::Dialogue);
    CHECK(typeOf(music) == SoundType::Music);
    CHECK_FALSE(typeOf(silent).has_value());
    bool sawSilent = false;
    for (const Json& row : r.data["clips"]) {
        if (row["clip"] == silent.value) {
            sawSilent = row["skipped"] == "silent";
        }
        if (row["clip"] == dialogue.value) {
            CHECK(row["type"] == "dialogue");
            CHECK(row["applied"] == true);
        }
    }
    CHECK(sawSilent);

    // A type set by hand stays unless "overwrite"; a low confidence is reported, not applied.
    tagger->guesses[h.tl.media('A').value] = guess(SoundType::Ambience, 0.9);
    h.ok("audio.autoTag", {{"clips", Json::array({dialogue.value})}});
    CHECK(typeOf(dialogue) == SoundType::Dialogue);
    h.ok("audio.autoTag", {{"clips", Json::array({dialogue.value})}, {"overwrite", true}, {"minConfidence", 0.95}});
    CHECK(typeOf(dialogue) == SoundType::Dialogue);
    h.ok("audio.autoTag", {{"clips", Json::array({dialogue.value})}, {"overwrite", true}});
    CHECK(typeOf(dialogue) == SoundType::Ambience);
    // The record is the only effect auto-tagging puts on a clip.
    CHECK(h.sequence(h.state()).findClip(dialogue)->clip().effects().size() == 1);
}

TEST_CASE("audio.autoTag needs the host's tagger", "[audio][edit][autotag][commands]") {
    Timeline tl(kPicture);
    CommandHarness h(std::move(tl), command::EnginePolicy{command::CapabilitySet::all(), 64, std::make_shared<AudioServices>(nullptr, nullptr)},
                     ai::edit::editingCommands());
    CHECK(h.errorOf("audio.autoTag") == "InvalidRequest");
}
