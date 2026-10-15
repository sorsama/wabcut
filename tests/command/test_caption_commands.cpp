// The caption workshop commands (C.7) through the engine: caption tracks and cues, styles and presets, speakers;
// dry runs, validation, one undo step each, and line breaking through the host's CaptionServices.

#include "CommandHarness.h"

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/CaptionOps.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;
using command::Json;

namespace {

const CaptionTrack& captions(const CommandHarness& h) { return h.sequence(h.state()).captionTracks.front(); }

std::string texts(const CommandHarness& h) {
    std::string out;
    for (const CaptionCue& c : captions(h).cues) {
        out += (out.empty() ? "" : " | ") + std::to_string(ticksToFrames(c.start, kRate)) + "-" +
               std::to_string(ticksToFrames(c.end(), kRate)) + " " + c.text;
    }
    return out;
}

/// A harness with one caption track holding "one" [0,4), "two" [5,10), "three" [12,16).
void addTrack(CommandHarness& h) {
    h.ok("captions.addTrack", Json{{"name", "English"}, {"language", "en"}});
    h.ok("captions.addCue", Json{{"start", tc(0)}, {"end", tc(4)}, {"text", "one"}});
    h.ok("captions.addCue", Json{{"start", tc(5)}, {"duration", tc(5)}, {"text", "two"}});
    h.ok("captions.addCue", Json{{"start", tc(12)}, {"end", tc(16)}, {"text", "three"}});
}

class FakeBreaker final : public LineBreaker {
public:
    std::vector<std::size_t> breakOpportunities(std::string_view text, std::string_view) const override {
        // Every 3 bytes (one Thai character in UTF-8) is a word here.
        std::vector<std::size_t> out;
        for (std::size_t i = 3; i < text.size(); i += 3) {
            out.push_back(i);
        }
        return out;
    }
};

} // namespace

TEST_CASE("captions.addTrack / addCue / updateCue / removeCue: one undo step each, validated", "[command][captions]") {
    CommandHarness h(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    )");
    const ProjectSnapshot empty = h.state();
    CHECK(h.errorOf("captions.addCue", Json{{"start", tc(0)}, {"end", tc(4)}, {"text", "x"}}) == "NotFound"); // no track
    const auto dry = h.run(Json{{"command", "captions.addTrack"}, {"params", Json{{"preset", "Bold Box"}}}, {"dryRun", true}});
    REQUIRE(dry.ok());
    CHECK(*h.state() == *empty);
    const auto made = h.ok("captions.addTrack", Json{{"name", "English"}, {"preset", "bold box"}, {"style", Json{{"fill", "#FFD400"}}}});
    CHECK(made.createdIds.size() == 1);
    CHECK(captions(h).style.fontWeight == 800);
    CHECK(captions(h).style.fill == CaptionRgb{1.0, 212.0 / 255.0, 0.0});
    CHECK(h.errorOf("captions.addTrack", Json{{"preset", "Nope"}}) == "NotFound");
    CHECK(h.errorOf("captions.addTrack", Json{{"style", Json{{"fontSize", 3.0}}}}) == "InvalidParams");

    CHECK(h.ok("captions.addCue", Json{{"start", tc(5)}, {"end", tc(9)}, {"text", "two"}}).data["cue"] == 0);
    CHECK(h.ok("captions.addCue", Json{{"start", tc(0)}, {"duration", tc(4)}, {"text", "one"}}).data["cue"] == 0);
    CHECK(texts(h) == "0-4 one | 5-9 two");
    CHECK(h.session.undoCount() == 3);
    CHECK(h.errorOf("captions.addCue", Json{{"start", tc(3)}, {"end", tc(6)}, {"text", "x"}}) == "EditRejected"); // overlap
    CHECK(h.errorOf("captions.addCue", Json{{"start", tc(10)}, {"text", "x"}}) == "InvalidParams");               // no end
    CHECK(h.errorOf("captions.addCue", Json{{"start", tc(10)}, {"end", tc(10)}, {"text", "x"}}) == "InvalidParams");
    CHECK(h.errorOf("captions.addCue", Json{{"start", tc(10)}, {"end", tc(12)}, {"text", std::string(5000, 'x')}}) ==
          "InvalidParams");
    CHECK(h.errorOf("captions.addCue", Json{{"start", tc(10)}, {"end", tc(12)}, {"text", "x"}, {"speaker", 999}}) ==
          "NotFound");

    h.ok("captions.updateCue", Json{{"cue", 1}, {"text", "TWO"}, {"end", tc(11)}});
    CHECK(texts(h) == "0-4 one | 5-11 TWO");
    CHECK(h.errorOf("captions.updateCue", Json{{"cue", 9}, {"text", "x"}}) == "NotFound");
    h.ok("captions.removeCue", Json{{"cue", 0}});
    CHECK(texts(h) == "5-11 TWO");
    h.ok("captions.setTrack", Json{{"name", "Thai"}, {"language", "th"}, {"enabled", false}});
    CHECK(captions(h).name == "Thai");
    CHECK_FALSE(captions(h).enabled);
    while (h.session.canUndo()) {
        h.session.undo();
    }
    CHECK(*h.state() == *empty);
}

TEST_CASE("captions.splitCue / mergeCues / shiftCues", "[command][captions]") {
    CommandHarness h(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    )");
    addTrack(h);
    h.ok("captions.updateCue", Json{{"cue", 1}, {"text", "two words"}});
    h.ok("captions.splitCue", Json{{"cue", 1}, {"at", tc(7)}});
    CHECK(texts(h) == "0-4 one | 5-7 two | 7-10 words | 12-16 three");
    CHECK(h.errorOf("captions.splitCue", Json{{"cue", 0}, {"at", tc(2)}}) == "EditRejected"); // one word
    h.ok("captions.splitCue", Json{{"cue", 0}, {"at", tc(2)}, {"textOffset", 1}});
    CHECK(texts(h) == "0-2 o | 2-4 ne | 5-7 two | 7-10 words | 12-16 three");
    h.ok("captions.mergeCues", Json{{"cue", 0}, {"count", 2}, {"maxCharsPerLine", 10}});
    CHECK(texts(h) == "0-4 o ne | 5-7 two | 7-10 words | 12-16 three");
    h.ok("captions.shiftCues", Json{{"from", tc(5)}, {"to", tc(11)}, {"delta", "-00:00:00:01"}});
    CHECK(texts(h) == "0-4 o ne | 4-6 two | 6-9 words | 12-16 three");
    CHECK(h.errorOf("captions.shiftCues", Json{{"delta", "-00:00:00:05"}}) == "EditRejected"); // before 0
    CHECK(h.errorOf("captions.shiftCues", Json{{"from", tc(12)}, {"delta", "-00:00:00:04"}}) == "EditRejected");

    // Line breaking through the host's rules: a Thai cue splits where the host's breaker allows.
    auto breaker = std::make_shared<FakeBreaker>();
    command::EnginePolicy policy{command::CapabilitySet::all(), 1024,
                                 std::make_shared<command::CaptionServices>(breaker)};
    CommandHarness thai(Timeline(R"(
        V1 tgt |Aaaaaaaaaa|
    )"),
                        policy);
    thai.ok("captions.addTrack", Json{{"language", "th"}});
    const std::string text = "\xE0\xB8\x81\xE0\xB8\x82\xE0\xB8\x84\xE0\xB8\x87"; // กขคง
    thai.ok("captions.addCue", Json{{"start", tc(0)}, {"end", tc(8)}, {"text", text}});
    thai.ok("captions.splitCue", Json{{"cue", 0}, {"at", tc(4)}});
    CHECK(captions(thai).cues[0].text == text.substr(0, 6));
    CHECK(captions(thai).cues[1].text == text.substr(6));
    CommandHarness plain(Timeline(R"(
        V1 tgt |Aaaaaaaaaa|
    )"));
    plain.ok("captions.addTrack", Json{{"language", "th"}});
    plain.ok("captions.addCue", Json{{"start", tc(0)}, {"end", tc(8)}, {"text", text}});
    CHECK(plain.errorOf("captions.splitCue", Json{{"cue", 0}, {"at", tc(4)}}) == "EditRejected"); // no breaker
}

TEST_CASE("captions.setStyle / applyPreset / savePreset / removePreset / presets, and captions.get shows them",
          "[command][captions][style]") {
    CommandHarness h(R"(
        V1 tgt |Aaaaaaaaaa|
    )");
    addTrack(h);
    const ProjectSnapshot before = h.state();
    const auto dry = h.run(Json{{"command", "captions.setStyle"},
                                {"params", Json{{"style", Json{{"box", false}}}}},
                                {"dryRun", true}});
    REQUIRE(dry.ok());
    CHECK(*h.state() == *before);
    const auto styled = h.ok("captions.setStyle", Json{{"style", Json{{"box", false},
                                                                      {"outlineWidth", 0.1},
                                                                      {"outline", Json::array({0.0, 0.0, 1.0})},
                                                                      {"placement", "custom"},
                                                                      {"positionY", 0.7},
                                                                      {"alignment", "left"},
                                                                      {"animation", "wordByWord"}}}});
    CHECK(styled.data["style"]["outline"] == "#0000FF");
    const CaptionStyle& s = captions(h).style;
    CHECK_FALSE(s.box);
    CHECK(s.placement == CaptionPlacement::Custom);
    CHECK(s.positionY.constant() == 0.7);
    CHECK(s.alignment == CaptionAlign::Left);
    CHECK(s.fontWeight == 600); // untouched
    CHECK(h.errorOf("captions.setStyle", Json{{"style", Json{{"fill", "red"}}}}) == "InvalidParams");
    CHECK(h.errorOf("captions.setStyle", Json{{"style", Json{{"bogus", 1}}}}) == "InvalidParams");
    CHECK(h.errorOf("captions.setStyle", Json{{"captionTrack", 999}, {"style", Json{{"box", true}}}}) == "NotFound");

    h.ok("captions.applyPreset", Json{{"preset", "Podcast"}});
    CHECK(captions(h).style.showSpeakers);
    h.ok("captions.savePreset",
         Json{{"name", "Channel"}, {"captionTrack", captions(h).id.value}, {"style", Json{{"fontWeight", 900}}}});
    CHECK(h.state()->captionPresets.size() == 1);
    CHECK(h.state()->captionPresets[0].style.showSpeakers); // the track's style, then the fields
    CHECK(h.state()->captionPresets[0].style.fontWeight == 900);
    CHECK(h.errorOf("captions.savePreset", Json{{"name", "Shorts"}}) == "EditRejected");
    const auto list = h.ok("captions.presets");
    REQUIRE(list.data["presets"].size() == 6);
    CHECK(list.data["presets"][5]["name"] == "Channel");
    CHECK(list.data["presets"][5]["builtIn"] == false);
    h.ok("captions.applyPreset", Json{{"preset", "channel"}});
    CHECK(captions(h).style.fontWeight == 900);
    h.ok("captions.removePreset", Json{{"name", "Channel"}});
    CHECK(h.state()->captionPresets.empty());
    CHECK(h.errorOf("captions.removePreset", Json{{"name", "Channel"}}) == "NotFound");

    const auto got = h.ok("captions.get");
    CHECK(got.data["captionTrack"]["style"]["fontWeight"] == 900);
    CHECK_FALSE(got.data.contains("cueSpeakers"));
}

TEST_CASE("Speakers: transcript.addSpeaker / renameSpeaker / mergeSpeakers / assignSpeaker", "[command][speakers]") {
    Timeline tl(R"(
        V1 tgt |A10aaaaaaaaaaaaaaaaaaaa|
        A1 tgt |A10aaaaaaaaaaaaaaaaaaaa|
    )");
    tl.linkMatching();
    CommandHarness h(std::move(tl));
    addTrack(h);
    const auto host = h.ok("transcript.addSpeaker", Json{{"name", "Host"}, {"color", "#FF0000"}});
    const auto hostId = host.data["speaker"].get<std::uint64_t>();
    CHECK(h.state()->findSpeaker(SpeakerId{hostId})->color == 0xFF0000u);
    // A diarization label without a speaker gets one when it is named.
    const auto guest = h.ok("transcript.renameSpeaker", Json{{"media", "A"}, {"label", 1}, {"name", "Guest"}});
    const SpeakerId guestId{guest.data["speaker"].get<std::uint64_t>()};
    CHECK(h.state()->findSpeaker(guestId)->name == "Guest");
    CHECK(h.state()->findTranscriptEdits(h.tl.media('A'), 0)->speakerOfLabel(1) == guestId);
    h.ok("transcript.renameSpeaker", Json{{"speaker", hostId}, {"color", Json::array({0.0, 1.0, 0.0})}});
    CHECK(h.state()->findSpeaker(SpeakerId{hostId})->color == 0x00FF00u);
    CHECK(h.errorOf("transcript.renameSpeaker", Json{{"name", "x"}}) == "InvalidParams");
    CHECK(h.errorOf("transcript.renameSpeaker", Json{{"speaker", 999}, {"name", "x"}}) == "NotFound");

    // Assign over timeline 2..6: the audio clip plays media 12..16 there.
    const auto assigned = h.ok("transcript.assignSpeaker", Json{{"start", tc(2)}, {"end", tc(6)}, {"speaker", hostId}});
    CHECK(assigned.data["clips"] == 1);
    const TranscriptEdits* edits = h.state()->findTranscriptEdits(h.tl.media('A'), 0);
    CHECK(edits->speakerAt(f(12)) == SpeakerId{hostId});
    CHECK(edits->speakerAt(f(15)) == SpeakerId{hostId});
    CHECK_FALSE(edits->speakerAt(f(17)).isValid());
    const auto fresh = h.ok("transcript.assignSpeaker", Json{{"start", tc(8)}, {"end", tc(9)}, {"name", "Caller"}});
    CHECK(h.state()->speakers.size() == 3);
    CHECK(h.errorOf("transcript.assignSpeaker", Json{{"start", tc(30)}, {"end", tc(31)}, {"speaker", hostId}}) ==
          "InvalidParams");

    // Merge: cues and assignments follow.
    h.ok("captions.updateCue", Json{{"cue", 0}, {"speaker", guestId.value}});
    const auto got = h.ok("captions.get");
    CHECK(got.data["cueSpeakers"] == Json::array({Json::array({0, guestId.value})}));
    h.ok("transcript.mergeSpeakers", Json{{"from", Json::array({guestId.value, fresh.data["speaker"]})}, {"into", hostId}});
    CHECK(h.state()->speakers.size() == 1);
    CHECK(captions(h).cues[0].speaker == SpeakerId{hostId});
    h.ok("captions.updateCue", Json{{"cue", 0}, {"speaker", nullptr}});
    CHECK_FALSE(captions(h).cues[0].speaker.isValid());
}

// Review wave 5, S5-P1-2: captions.get handed the stored text of a cue over redacted speech - typed, imported, or made
// before the redaction - to the assistant's cloud model and to MCP agents. It reads "[bleep]" now, whatever made the
// cue; the stored text (the user's own) is unchanged.
TEST_CASE("captions.get masks a cue over redacted speech whatever made it", "[command][captions][privacy]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    )");
    tl.linkMatching();
    CommandHarness h(std::move(tl));
    addTrack(h); // typed cues: "one" [0,4), "two" [5,10), "three" [12,16)
    Redaction r;
    r.media = h.tl.media('A');
    r.source = TimeRange::fromStartEnd(f(6), f(8));
    r.audio = RedactionAudio::Bleep;
    auto added = edit::addRedaction(*h.state(), r);
    REQUIRE(added);
    REQUIRE(h.session.commit("Add Redaction", added->state));

    const auto got = h.ok("captions.get");
    REQUIRE(got.data["cues"].size() == 3);
    CHECK(got.data["cues"][0][2] == "one");
    CHECK(got.data["cues"][1][2] == "[bleep]");
    CHECK(got.data["cues"][2][2] == "three");
    CHECK(got.data["redactedCues"] == Json::array({1}));
    CHECK(captions(h).cues[1].text == "two");

    // A redaction that only covers the picture leaves the words.
    Redaction pictureOnly = *added->state.findRedaction(added->id);
    pictureOnly.audio = RedactionAudio::None;
    auto updated = edit::updateRedaction(*h.state(), pictureOnly);
    REQUIRE(updated);
    REQUIRE(h.session.commit("Redaction Sound", *updated));
    const auto clear = h.ok("captions.get");
    CHECK(clear.data["cues"][1][2] == "two");
    CHECK_FALSE(clear.data.contains("redactedCues"));
}
