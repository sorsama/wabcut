// 0.2 graphics through the command protocol: Find (captions.find) and Replace one (captions.findReplace "at"), and spell
// check (captions.checkSpelling, graphic.checkSpelling, spelling.check) with the host's checker.

#include "CommandHarness.h"

#include <wabcut/command/SpellingServices.h>
#include <wabcut/model/Spelling.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::test;

namespace {

constexpr std::string_view kPicture = R"(
    V2 tgt |..................................................|
    V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
)";

command::EnginePolicy withChecker() {
    auto checker = std::make_shared<WordListSpellChecker>(
        std::vector<std::string>{"the", "quick", "brown", "fox", "jumps", "colour", "of", "night", "sky"},
        std::vector<std::string>{"en"});
    auto personal = std::make_shared<PersonalSpelling>(checker);
    command::EnginePolicy policy;
    policy.services = std::make_shared<command::SpellingServices>(personal);
    return policy;
}

std::string cueText(const CommandHarness& h, std::uint64_t track, std::size_t cue) {
    for (const CaptionTrack& t : h.sequence(h.state()).captionTracks) {
        if (t.id.value == track) {
            return t.cues.at(cue).text;
        }
    }
    return {};
}

} // namespace

TEST_CASE("captions.find lists matches and captions.findReplace at replaces one, then names the next", "[command][captions][findReplace]") {
    CommandHarness h(kPicture);
    const std::uint64_t track = h.ok("captions.addTrack", Json{{"name", "English"}}).data["captionTrack"];
    h.ok("captions.addCue", Json{{"captionTrack", track}, {"start", "00:00:00:00"}, {"end", "00:00:01:00"}, {"text", "red sky, Red sea"}});
    h.ok("captions.addCue", Json{{"captionTrack", track}, {"start", "00:00:01:00"}, {"end", "00:00:02:00"}, {"text", "RED"}});

    const auto found = h.ok("captions.find", Json{{"find", "red"}});
    REQUIRE(found.data["count"] == 3);
    const Json& second = found.data["matches"][1];
    CHECK(second["cue"] == 0);
    CHECK(second["start"] == 9);
    CHECK(second["end"] == 12);
    CHECK(second["time"] == "00:00:00:00");
    CHECK(second["text"] == "red sky, Red sea");
    CHECK(h.ok("captions.find", Json{{"find", "red"}, {"matchCase", true}}).data["count"] == 1);

    // Replace: only that match, one undo step; "next" is the match after it.
    const std::size_t before = h.session.undoCount();
    const auto one = h.ok("captions.findReplace",
                          Json{{"find", "red"}, {"replace", "blue"}, {"at", Json{{"captionTrack", track}, {"cue", 0}, {"start", 9}}}});
    CHECK(one.data["replaced"] == 1);
    CHECK(h.session.undoCount() == before + 1);
    CHECK(cueText(h, track, 0) == "red sky, blue sea");
    REQUIRE(one.data["next"].is_object());
    CHECK(one.data["next"]["cue"] == 1);
    CHECK(one.data["next"]["start"] == 0);
    // A stale match is refused; replacing the last wraps "next" round to the first.
    CHECK(h.errorOf("captions.findReplace",
                    Json{{"find", "red"}, {"replace", "x"}, {"at", Json{{"captionTrack", track}, {"cue", 0}, {"start", 9}}}}) ==
          "NotFound");
    const auto last = h.ok("captions.findReplace",
                           Json{{"find", "red"}, {"replace", "teal"}, {"at", Json{{"captionTrack", track}, {"cue", 1}, {"start", 0}}}});
    CHECK(cueText(h, track, 1) == "teal");
    CHECK(last.data["next"]["cue"] == 0);
    CHECK(last.data["next"]["start"] == 0);
    // Emptying a cue is refused here too.
    CHECK(h.errorOf("captions.findReplace",
                    Json{{"find", "teal"}, {"replace", ""}, {"at", Json{{"captionTrack", track}, {"cue", 1}, {"start", 0}}}}) ==
          "EditRejected");
}

TEST_CASE("captions.checkSpelling lists misspelled words with suggestions in the track's language", "[command][captions][spelling]") {
    CommandHarness h(kPicture, withChecker());
    const std::uint64_t track = h.ok("captions.addTrack", Json{{"name", "English"}, {"language", "en"}}).data["captionTrack"];
    h.ok("captions.addCue", Json{{"captionTrack", track}, {"start", "00:00:00:00"}, {"end", "00:00:01:00"}, {"text", "the quick brwn fox"}});
    h.ok("captions.addCue", Json{{"captionTrack", track}, {"start", "00:00:01:00"}, {"end", "00:00:02:00"}, {"text", "jumsp over"}});
    const auto r = h.ok("captions.checkSpelling", Json{{"captionTrack", track}});
    CHECK(r.data["available"] == true);
    CHECK(r.data["language"] == "en");
    const Json& issues = r.data["issues"];
    REQUIRE(issues.size() == 3);
    CHECK(issues[0]["cue"] == 0);
    CHECK(issues[0]["word"] == "brwn");
    CHECK(issues[0]["start"] == 10);
    CHECK(issues[0]["end"] == 14);
    CHECK(issues[0]["suggestions"][0] == "brown");
    CHECK(issues[1]["word"] == "jumsp");
    CHECK(issues[1]["suggestions"][0] == "jumps");
    CHECK(issues[2]["word"] == "over");
    // Another language: nothing to check, said so.
    const auto thai = h.ok("captions.checkSpelling", Json{{"captionTrack", track}, {"language", "th"}});
    CHECK(thai.data["available"] == false);
    CHECK(thai.data["issues"].empty());
    // Read only.
    CHECK(h.session.undoCount() == 3);
}

TEST_CASE("graphic.checkSpelling and spelling.check use the host's checker", "[command][graphics][spelling]") {
    CommandHarness h(kPicture, withChecker());
    const Json graphic{{"width", 1920},
                       {"height", 1080},
                       {"layers", Json::array({Json{{"id", 1}, {"kind", "text"}, {"text", Json{{"runs", Json::array({Json{{"text", "night skye"}}})}}}},
                                               Json{{"id", 2}, {"kind", "group"},
                                                    {"children", Json::array({Json{{"id", 3}, {"kind", "text"},
                                                                                   {"text", Json{{"runs", Json::array({Json{{"text", "colur"}}})}}}}})}}})}};
    const std::uint64_t clip = h.ok("graphic.add", Json{{"at", tc(0)}, {"duration", tc(25)}, {"track", "V2"}, {"graphic", graphic}})
                                   .createdIds.front();
    const auto r = h.ok("graphic.checkSpelling", Json{{"clip", clip}, {"language", "en"}});
    const Json& issues = r.data["issues"];
    REQUIRE(issues.size() == 2);
    CHECK(issues[0]["layer"] == 1);
    CHECK(issues[0]["word"] == "skye");
    CHECK(issues[0]["start"] == 6);
    CHECK(issues[1]["layer"] == 3);
    CHECK(issues[1]["suggestions"][0] == "colour");

    const auto text = h.ok("spelling.check", Json{{"text", "the browm fox"}, {"language", "en-US"}});
    REQUIRE(text.data["issues"].size() == 1);
    CHECK(text.data["issues"][0]["word"] == "browm");
    CHECK(text.data["issues"][0]["action"] == "suggest");

    // Without a spell checker the host refuses.
    CommandHarness bare(kPicture);
    CHECK(bare.errorOf("spelling.check", Json{{"text", "teh"}}) == "InvalidRequest");
}
