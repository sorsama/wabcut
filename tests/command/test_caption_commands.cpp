// The caption workshop commands (C.7) through the engine: caption tracks and cues, styles and presets, speakers;
// dry runs, validation, one undo step each, and line breaking through the host's CaptionServices.

#include "CommandHarness.h"

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Schema.h>
#include <wabcut/model/CaptionOps.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
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

TEST_CASE("captions.setTrack showTogether draws a track with the main one: one undo step, saved with the project",
          "[command][captions][bilingual]") {
    CommandHarness h(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    )");
    addTrack(h);
    h.ok("captions.addTrack", Json{{"name", "Spanish"}, {"language", "es"}});
    const auto& tracks = h.sequence(h.state()).captionTracks;
    REQUIRE(tracks.size() == 2);
    CHECK_FALSE(tracks[1].showTogether);
    const auto steps = h.session.undoCount();
    h.ok("captions.setTrack", Json{{"captionTrack", tracks[1].id.value}, {"showTogether", true}});
    CHECK(h.session.undoCount() == steps + 1);
    CHECK(h.sequence(h.state()).captionTracks[1].showTogether);
    CHECK_FALSE(h.sequence(h.state()).captionTracks[0].showTogether); // only the track asked
    // A rename leaves the flag alone; turning it off is an edit of its own.
    h.ok("captions.setTrack", Json{{"captionTrack", tracks[1].id.value}, {"name", "Espanol"}});
    CHECK(h.sequence(h.state()).captionTracks[1].showTogether);
    h.ok("captions.setTrack", Json{{"captionTrack", tracks[1].id.value}, {"showTogether", false}});
    CHECK_FALSE(h.sequence(h.state()).captionTracks[1].showTogether);
    h.session.undo();
    CHECK(h.sequence(h.state()).captionTracks[1].showTogether);
    // The mark is saved and loads back; a project without it writes no key.
    const auto text = io::toJson(*h.state());
    REQUIRE(text);
    CHECK(text->find("\"showTogether\": true") != std::string::npos);
    const auto loaded = io::fromJson(*text);
    REQUIRE(loaded);
    CHECK(loaded->findSequence(h.sequence(h.state()).id)->captionTracks[1].showTogether);
    h.session.undo();
    h.session.undo();
    const auto plain = io::toJson(*h.state());
    REQUIRE(plain);
    CHECK(plain->find("showTogether") == std::string::npos);
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
    REQUIRE(list.data["presets"].size() == 12); // the 11 built in (the animated gallery among them) and Channel
    CHECK(list.data["presets"][11]["name"] == "Channel");
    CHECK(list.data["presets"][11]["builtIn"] == false);
    h.ok("captions.applyPreset", Json{{"preset", "channel"}});
    CHECK(captions(h).style.fontWeight == 900);
    h.ok("captions.removePreset", Json{{"name", "Channel"}});
    CHECK(h.state()->captionPresets.empty());
    CHECK(h.errorOf("captions.removePreset", Json{{"name", "Channel"}}) == "NotFound");

    const auto got = h.ok("captions.get");
    CHECK(got.data["captionTrack"]["style"]["fontWeight"] == 900);
    CHECK_FALSE(got.data.contains("cueSpeakers"));
}

TEST_CASE("captions.setStyle: animations with timing and easing and emphasis words and case and punctuation and accents",
          "[command][captions][style][animation]") {
    CommandHarness h(R"(
        V1 tgt |Aaaaaaaaaa|
    )");
    addTrack(h);
    const auto styled = h.ok("captions.setStyle", Json{{"style", Json{{"animation", "highlightBar"},
                                                                      {"animationSpeed", 0.3},
                                                                      {"animationEase", "linear"},
                                                                      {"emphasis", "auto"},
                                                                      {"emphasisWords", Json::array({"GG", "clutch"})},
                                                                      {"emphasisColor", "#FF0000"},
                                                                      {"emphasisScale", 1.25},
                                                                      {"textCase", "upper"},
                                                                      {"stripPunctuation", true},
                                                                      {"stripAccents", true}}}});
    const CaptionStyle& s = captions(h).style;
    CHECK(s.animation == CaptionAnimation::HighlightBar);
    CHECK(s.animationSpeed == 0.3);
    CHECK(s.animationEase == CaptionEase::Linear);
    CHECK(s.emphasis == CaptionEmphasis::Auto);
    CHECK(s.emphasisWords == std::vector<std::string>{"GG", "clutch"});
    CHECK(s.emphasisColor == CaptionRgb{1.0, 0.0, 0.0});
    CHECK(s.emphasisScale == 1.25);
    CHECK(s.textCase == CaptionTextCase::Upper);
    CHECK(s.stripPunctuation);
    CHECK(s.stripAccents);
    CHECK(styled.data["style"]["animation"] == "highlightBar");
    CHECK(styled.data["style"]["emphasisWords"] == Json::array({"GG", "clutch"}));
    CHECK(styled.data["style"]["emphasisColor"] == "#FF0000");
    CHECK(styled.data["style"]["textCase"] == "upper");
    CHECK(captions(h).style.fontWeight == 600); // untouched

    // One undo step; the old look is back.
    h.session.undo();
    CHECK(captions(h).style == CaptionStyle{});

    // Out of range or unknown: refused, nothing changes.
    CHECK(h.errorOf("captions.setStyle", Json{{"style", Json{{"animationSpeed", 0.01}}}}) == "InvalidParams");
    CHECK(h.errorOf("captions.setStyle", Json{{"style", Json{{"emphasisScale", 2.0}}}}) == "InvalidParams");
    CHECK(h.errorOf("captions.setStyle", Json{{"style", Json{{"animation", "spin"}}}}) == "InvalidParams");
    CHECK(h.errorOf("captions.setStyle", Json{{"style", Json{{"textCase", "title"}}}}) == "InvalidParams");
    CHECK(h.errorOf("captions.setStyle", Json{{"style", Json{{"emphasisWords", Json::array({""})}}}}) == "InvalidParams");
    CHECK(captions(h).style == CaptionStyle{});

    // The gallery presets apply as any preset and carry their looks.
    h.ok("captions.applyPreset", Json{{"preset", "Hype"}});
    CHECK(captions(h).style.animation == CaptionAnimation::WordPop);
    CHECK(captions(h).style.textCase == CaptionTextCase::Upper);
    CHECK(captions(h).style.stripPunctuation);
    h.ok("captions.applyPreset", Json{{"preset", "highlighter"}});
    CHECK(captions(h).style.animation == CaptionAnimation::HighlightBar);
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

// ---- 0.7 (card 061): custom words for transcription -------------------------------------------------------------------

TEST_CASE("transcript.setCustomWords sets, adds and clears the project's words in one undo step", "[command][captions][words]") {
    CommandHarness h(R"(
        V1 tgt |Aaaaaaaaaa|
    )");
    CHECK(h.state()->customWords.empty());

    const auto first = h.ok("transcript.setCustomWords", Json{{"words", Json::array({"Malenia", " Elden Ring ", "malenia"})}});
    CHECK(h.state()->customWords == std::vector<std::string>{"Malenia", "Elden Ring"});
    CHECK(first.data["words"] == Json::array({"Malenia", "Elden Ring"}));
    CHECK(h.session.undoLabel() == std::optional<std::string>("Custom Words"));

    h.ok("transcript.setCustomWords", Json{{"text", "Radahn, Godrick\nMorgott"}, {"add", true}});
    CHECK(h.state()->customWords == std::vector<std::string>{"Malenia", "Elden Ring", "Radahn", "Godrick", "Morgott"});
    const auto listed = h.ok("transcript.customWords");
    CHECK(listed.data["words"].size() == 5);

    // Replacing: the text alone is the whole list.
    h.ok("transcript.setCustomWords", Json{{"text", "Only one"}});
    CHECK(h.state()->customWords == std::vector<std::string>{"Only one"});

    // One undo step each.
    REQUIRE(h.session.undo());
    CHECK(h.state()->customWords.size() == 5);
    REQUIRE(h.session.undo());
    CHECK(h.state()->customWords.size() == 2);
    REQUIRE(h.session.undo());
    CHECK(h.state()->customWords.empty());

    // A dry run changes nothing; an empty list clears; bad input is refused.
    const auto dry = h.run(Json{{"command", "transcript.setCustomWords"}, {"params", Json{{"words", Json::array({"X"})}}}, {"dryRun", true}});
    CHECK(dry.ok());
    CHECK(h.state()->customWords.empty());
    h.ok("transcript.setCustomWords", Json{{"words", Json::array({"X"})}});
    h.ok("transcript.setCustomWords", Json{{"words", Json::array()}});
    CHECK(h.state()->customWords.empty());
    CHECK(h.errorOf("transcript.setCustomWords") == "InvalidParams");
    CHECK(h.errorOf("transcript.setCustomWords", Json{{"words", Json::array({std::string(500, 'x')})}}) != "");
}

// ---- 0.7 (card 115): caption presets as files -------------------------------------------------------------------------

TEST_CASE("Caption presets are exported to a file and imported into another project", "[command][captions][presets][io]") {
    const std::filesystem::path dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "caption-presets-file";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string file = (dir / "looks.wabcut-caption-presets.json").string();

    CommandHarness h(R"(
        V1 tgt |Aaaaaaaaaa|
    )");
    addTrack(h);
    h.ok("captions.setStyle", Json{{"style", Json{{"fontWeight", 800}, {"box", false}}}});
    const std::uint64_t shown = captions(h).id.value;
    h.ok("captions.savePreset", Json{{"name", "Channel look"}, {"captionTrack", shown}});
    h.ok("captions.savePreset", Json{{"name", "Second look"}, {"style", Json{{"fontWeight", 400}}}});

    SECTION("export writes the project's presets; the project does not change") {
        const auto before = h.state();
        const auto result = h.ok("captions.exportPresets", Json{{"path", file}});
        CHECK(result.data["presets"].size() == 2);
        CHECK(h.state() == before);
        CHECK(std::filesystem::exists(file));
        // A second export refuses to replace it; overwrite and merge are allowed.
        CHECK(h.errorOf("captions.exportPresets", Json{{"path", file}}) == "EditRejected");
        h.ok("captions.exportPresets", Json{{"path", file}, {"names", Json::array({"Second look"})}, {"overwrite", true}});
        CHECK(h.ok("captions.exportPresets", Json{{"path", file}, {"names", Json::array({"Channel look"})}, {"merge", true}})
                  .data["fileHolds"] == 2);
        CHECK(h.errorOf("captions.exportPresets", Json{{"path", (dir / "x.json").string()}, {"names", Json::array({"nope"})}}) == "NotFound");
    }
    SECTION("a dry run writes nothing") {
        h.run(Json{{"command", "captions.exportPresets"}, {"params", Json{{"path", file}}}, {"dryRun", true}});
        CHECK_FALSE(std::filesystem::exists(file));
    }
    SECTION("import adds them to a project that has none, as one undo step") {
        h.ok("captions.exportPresets", Json{{"path", file}});
        CommandHarness other(R"(
            V1 tgt |Aaaaaaaaaa|
        )");
        const auto result = other.ok("captions.importPresets", Json{{"path", file}});
        CHECK(result.data["imported"] == Json::array({"Channel look", "Second look"}));
        CHECK(other.state()->captionPresets.size() == 2);
        CHECK(edit::captionPresetStyle(*other.state(), "channel look")->fontWeight == 800);
        CHECK(other.session.undoLabel() == std::optional<std::string>("Import Caption Presets"));
        REQUIRE(other.session.undo());
        CHECK(other.state()->captionPresets.empty());

        // Only the named ones; a name the project has is skipped unless "replace".
        other.ok("captions.importPresets", Json{{"path", file}, {"names", Json::array({"second look"})}});
        CHECK(other.state()->captionPresets.size() == 1);
        const auto again = other.ok("captions.importPresets", Json{{"path", file}});
        CHECK(again.data["imported"] == Json::array({"Channel look"}));
        CHECK(again.data["skipped"].size() == 1);
        h.ok("captions.setStyle", Json{{"style", Json{{"fontWeight", 600}}}});
        h.ok("captions.savePreset", Json{{"name", "Channel look"}, {"captionTrack", shown}});
        h.ok("captions.exportPresets", Json{{"path", file}, {"overwrite", true}});
        other.ok("captions.importPresets", Json{{"path", file}, {"replace", true}});
        CHECK(edit::captionPresetStyle(*other.state(), "Channel look")->fontWeight == 600);
    }
    SECTION("a missing, damaged or foreign file is refused and the project stays") {
        const auto before = h.state();
        CHECK(h.errorOf("captions.importPresets", Json{{"path", (dir / "missing.json").string()}}) == "NotFound");
        const std::string bad = (dir / "bad.json").string();
        std::ofstream(bad) << "{ not json";
        CHECK(h.errorOf("captions.importPresets", Json{{"path", bad}}) != "");
        const std::string foreign = (dir / "foreign.json").string();
        std::ofstream(foreign) << R"({"format": "wabcut-effect-presets", "version": 1, "presets": []})";
        CHECK(h.errorOf("captions.importPresets", Json{{"path", foreign}}) != "");
        CHECK(h.state() == before);
    }
    SECTION("the file commands are file commands: a host that holds those back refuses them") {
        CHECK(command::builtinCommands().find("captions.exportPresets")->touchesFiles);
        CHECK(command::builtinCommands().find("captions.importPresets")->touchesFiles);
    }
}

// ---- 0.7 (card 075): captions to graphics ---------------------------------------------------------------------------

namespace {

/// The graphic clips of video track `v` (1 = V1), in time order.
std::vector<const Clip*> graphicClips(const CommandHarness& h, std::size_t v) {
    std::vector<const Clip*> out;
    const Sequence& seq = h.sequence(h.state());
    if (v == 0 || v > seq.videoTracks.size()) {
        return out;
    }
    for (const Clip& c : seq.videoTracks[v - 1]->clips) {
        if (c.graphic() != nullptr) {
            out.push_back(&c);
        }
    }
    return out;
}

} // namespace

TEST_CASE("captions.toGraphics makes a graphic clip of every cue in the caption style, in one undo step", "[command][captions][graphics]") {
    CommandHarness h(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )");
    addTrack(h);
    h.ok("captions.setStyle", Json{{"style", Json{{"fontSize", 0.06}, {"fontWeight", 700}, {"fill", "#FFD400"},
                                                  {"outlineWidth", 0.1}, {"outline", "#101010"}, {"shadowOpacity", 0.5},
                                                  {"box", true}, {"boxOpacity", 0.8}, {"boxPadding", 0.4}, {"maxWidth", 0.7},
                                                  {"alignment", "center"}}}});
    const std::size_t undo = h.session.undoCount();
    const auto r = h.ok("captions.toGraphics");
    CHECK(h.session.undoCount() == undo + 1);
    CHECK(r.createdIds.size() == 3);
    CHECK(r.data["converted"] == 3);
    const Sequence& seq = h.sequence(h.state());
    REQUIRE(seq.videoTracks.size() == 2); // a new video track on top
    const auto clips = graphicClips(h, 2);
    REQUIRE(clips.size() == 3);
    // At the cues' times.
    CHECK(clips[0]->start() == captions(h).cues[0].start);
    CHECK(clips[1]->start() == captions(h).cues[1].start);
    CHECK(clips[1]->duration() == captions(h).cues[1].duration);
    CHECK(clips[2]->end() == captions(h).cues[2].end());

    const Graphic& g = *clips[1]->graphic();
    REQUIRE(g.layers.size() == 1);
    const GraphicLayer& layer = g.layers[0];
    CHECK(layer.kind == LayerKind::Text);
    CHECK(layer.text.text() == "two");
    CHECK(g.width == seq.width);
    CHECK(g.height == seq.height);
    // The look: size from the frame height, weight, colours, the outline as a stroke, the box as the background.
    const TextStyle& run = layer.text.runs.front().style;
    CHECK(run.fontSize == Catch::Approx(0.06 * seq.height));
    CHECK(run.fontWeight == 700);
    CHECK(run.fill.rgba[0] == Catch::Approx(1.0));
    CHECK(run.fill.rgba[1] == Catch::Approx(212.0 / 255.0));
    REQUIRE(layer.text.strokes.size() == 1);
    CHECK(layer.text.strokes[0].width.constant() == Catch::Approx(0.1 * run.fontSize));
    CHECK(layer.shadow.enabled);
    CHECK(layer.text.background.enabled);
    CHECK(layer.text.background.color.rgba[3] == Catch::Approx(0.8));
    CHECK(layer.text.background.padding == Catch::Approx(0.4 * run.fontSize));
    CHECK(layer.text.align == TextAlign::Center);
    // A text box that grows with its text: as wide as the caption block, as tall as its lines (boxHeight 0 = auto) ...
    CHECK(layer.text.boxWidth == Catch::Approx(0.7 * seq.width));
    CHECK(layer.text.boxHeight == 0.0);
    // ... and pinned to the bottom edge of the frame, so a longer text grows upwards from the safe margin.
    REQUIRE(layer.pin.has_value());
    CHECK(layer.pin->target == LayerPin::kVideoFrame);
    CHECK(layer.pin->edges == LayerPin::kBottom);
    CHECK(layer.pin->offsets[3] == Catch::Approx(-0.1 * seq.height));
    CHECK(layer.transform.x.constant() == Catch::Approx(seq.width / 2.0));

    // The caption track is switched off, so the text is not drawn twice; its cues are still there.
    CHECK_FALSE(captions(h).enabled);
    CHECK(captions(h).cues.size() == 3);
    REQUIRE(h.session.undo());
    CHECK(h.sequence(h.state()).videoTracks.size() == 1);
    CHECK(captions(h).enabled);
}

TEST_CASE("captions.toGraphics: placement, case, keep and remove, a range, overlapping cues and a named track", "[command][captions][graphics]") {
    CommandHarness h(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        V2     |..................................................................................................................................|
    )");
    addTrack(h);
    SECTION("top, custom, upper and lower case, left alignment") {
        h.ok("captions.setStyle", Json{{"style", Json{{"placement", "top"}, {"textCase", "upper"}, {"alignment", "left"}}}});
        h.ok("captions.toGraphics", Json{{"captions", "keep"}});
        const Sequence& seq = h.sequence(h.state());
        const GraphicLayer& top = graphicClips(h, seq.videoTracks.size())[0]->graphic()->layers[0];
        REQUIRE(top.pin.has_value());
        CHECK(top.pin->edges == LayerPin::kTop);
        CHECK(top.pin->offsets[1] == Catch::Approx(0.1 * seq.height));
        CHECK(top.text.runs.front().style.caps == TextCaps::AllCaps);
        CHECK(top.text.align == TextAlign::Left);
        CHECK(top.transform.x.constant() == Catch::Approx(seq.width / 2.0 - 0.8 * seq.width / 2.0)); // the block's left edge
        CHECK(captions(h).enabled); // "keep"
        h.ok("captions.setStyle", Json{{"style", Json{{"placement", "custom"}, {"alignment", "center"}, {"textCase", "lower"}, {"positionX", 0.25}}}});
        h.ok("captions.toGraphics", Json{{"captions", "keep"}, {"from", tc(5)}, {"to", tc(11)}});
        const Sequence& after = h.sequence(h.state());
        const auto placed = graphicClips(h, after.videoTracks.size());
        REQUIRE(placed.size() == 1); // only the cue that starts in the range
        const GraphicLayer& custom = placed[0]->graphic()->layers[0];
        CHECK_FALSE(custom.pin.has_value());
        CHECK(custom.transform.x.constant() == Catch::Approx(0.25 * after.width));
        CHECK(custom.text.text() == "two");
    }
    SECTION("remove deletes the caption track") {
        h.ok("captions.toGraphics", Json{{"captions", "remove"}});
        CHECK(h.sequence(h.state()).captionTracks.empty());
        CHECK(graphicClips(h, 3).size() == 3);
    }
    SECTION("overlapping cues go on more than one new track") {
        h.ok("captions.setStyle", Json{{"style", Json{{"allowOverlap", true}}}});
        h.ok("captions.addCue", Json{{"start", tc(2)}, {"end", tc(6)}, {"text", "overlap"}});
        h.ok("captions.toGraphics");
        const Sequence& seq = h.sequence(h.state());
        std::size_t lanes = 0;
        for (std::size_t v = 3; v <= seq.videoTracks.size(); ++v) {
            lanes += graphicClips(h, v).empty() ? 0 : 1;
        }
        CHECK(lanes == 2);
    }
    SECTION("a named free track takes them; a busy one is refused and nothing changes") {
        const auto before = h.state();
        CHECK(h.errorOf("captions.toGraphics", Json{{"track", "V1"}}) == "EditRejected"); // the picture is under the cues
        CHECK(h.state() == before);
        h.ok("captions.toGraphics", Json{{"track", "V2"}});
        CHECK(graphicClips(h, 2).size() == 3);
        CHECK(h.sequence(h.state()).videoTracks.size() == 2); // no new track
    }
    SECTION("nothing to convert, a bad choice and no caption track are refused") {
        CHECK(h.errorOf("captions.toGraphics", Json{{"from", tc(100)}}) == "EditRejected");
        CHECK(h.errorOf("captions.toGraphics", Json{{"captions", "delete"}}) == "InvalidParams");
        CHECK(h.errorOf("captions.toGraphics", Json{{"captions", "remove"}, {"from", tc(5)}}) == "InvalidParams");
        CommandHarness bare(R"(
            V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
        )");
        CHECK(bare.errorOf("captions.toGraphics") == "NotFound");
    }
}

TEST_CASE("captions.toGraphics is in the catalog and its example is valid", "[command][captions][graphics]") {
    const command::CommandSpec* spec = command::builtinCommands().find("captions.toGraphics");
    REQUIRE(spec != nullptr);
    CHECK(command::unsupportedSchemaKeywords(spec->params).empty());
    CHECK(command::validateAgainstSchema(spec->params, spec->example).empty());
}
