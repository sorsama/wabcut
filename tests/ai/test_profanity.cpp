// p02 audio: profanity (Profanity.h): Premiere's Speech to Text "Hide profanity" in captions ("f***"), the editable
// English and Thai lists, and audio.bleepProfanity's audio-only redactions over the words (captions over them then read
// [bleep]), each one undo step.

#include "CommandHarness.h"
#include "EditTestUtil.h"

#include <wabcut/ai/edit/Profanity.h>
#include <wabcut/ai/edit/TranscriptCommands.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::ai::edit;
using namespace wabcut::ai::edit::test;
using wabcut::ai::Transcript;
using command::Json;

namespace {

constexpr std::string_view kPicture = R"(
    V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
)";

std::shared_ptr<const Transcript> rant() {
    return transcript("en", {{"What", 0, 3, 0},       // 0
                             {"the", 3, 5, 0},        // 1
                             {"fuck,", 5, 9, 0},      // 2
                             {"this", 10, 13, 0},     // 3
                             {"shitty", 13, 18, 0},   // 4
                             {"boss", 18, 22, 0},     // 5
                             {"is", 22, 24, 0},       // 6
                             {"classic.", 24, 30, 0}}); // 7
}

struct Fixture {
    Fixture()
        : transcripts(provider(Timeline(kPicture), {{'A', rant()}})),
          h(withLinks(Timeline(kPicture)),
            command::EnginePolicy{command::CapabilitySet::all(), 1024, std::make_shared<TranscriptServices>(transcripts)},
            editingCommands()) {}

    static Timeline withLinks(Timeline tl) {
        tl.linkMatching();
        return tl;
    }

    std::shared_ptr<MapTranscriptProvider> transcripts;
    CommandHarness h;
};

std::string allText(const CaptionTrack& track) {
    std::string out;
    for (const CaptionCue& cue : track.cues) {
        out += cue.text + "\n";
    }
    return out;
}

} // namespace

TEST_CASE("Profanity: words are masked as their first letter and asterisks with the punctuation kept", "[ai][edit][profanity]") {
    const ProfanityFilter filter;
    CHECK(filter.mask("fuck") == "f***");
    CHECK(filter.mask("Fucking!") == "F******!");
    CHECK(filter.mask("shitty") == "s*****");
    CHECK(filter.mask("(bitch)") == "(b****)");
    // Whole words only where the list says so: "class", "Dickens", "assassin", "cocktail" stay.
    for (const char* clean : {"class", "Dickens", "assassin", "cocktail", "Scunthorpe", "hello"}) {
        INFO(clean);
        CHECK_FALSE(filter.isProfane(clean));
        CHECK(filter.mask(clean) == clean);
    }
    CHECK(filter.maskText("What the fuck, this is shit.") == "What the f***, this is s***.");
}

TEST_CASE("Profanity: Thai words whole or inside unspaced runs, never inside ordinary words", "[ai][edit][profanity]") {
    const ProfanityFilter filter;
    CHECK(filter.isProfane("เหี้ย"));
    CHECK(filter.mask("เหี้ย") == "เ****");
    CHECK(filter.isProfane("หี"));
    CHECK_FALSE(filter.isProfane("หีบ"));      // a chest: "หี" matches whole words only
    CHECK_FALSE(filter.isProfane("สัดส่วน"));  // a proportion: "สัด" likewise
    CHECK(filter.isProfane("ไอ้เหี้ยนี่"));     // "เหี้ย" counts inside a word
    // An unspaced caption line: the '*' entries are masked where they are.
    const std::string line = "มึงเหี้ยมาก";
    const std::string masked = filter.maskText(line);
    CHECK(masked != line);
    CHECK(masked.find("เหี้ย") == std::string::npos);
    CHECK(masked.starts_with("มึง"));
    CHECK(masked.ends_with("มาก"));
}

TEST_CASE("Profanity: the lists are editable per request", "[ai][edit][profanity]") {
    const std::vector<std::string> extra{"heck", "frick*"};
    const std::vector<std::string> allowed{"piss"};
    const ProfanityFilter filter(extra, allowed);
    CHECK(filter.mask("heck") == "h***");
    CHECK(filter.mask("fricking") == "f*******");
    CHECK_FALSE(filter.isProfane("piss"));
    CHECK(filter.isProfane("fuck"));
    CHECK_FALSE(ProfanityFilter::builtin("en").empty());
    CHECK_FALSE(ProfanityFilter::builtin("th").empty());
    CHECK(ProfanityFilter::builtin("fr").empty());
}

TEST_CASE("Captions hide profanity when asked: new captions and an existing track", "[ai][edit][profanity][captions]") {
    Fixture f;
    f.h.ok("captions.generateFromTranscript", {{"hideProfanity", true}, {"name", "Clean"}});
    const auto& clean = f.h.sequence(f.h.state()).captionTracks.back();
    CHECK(allText(clean).find("f***,") != std::string::npos);
    CHECK(allText(clean).find("s*****") != std::string::npos);
    CHECK(allText(clean).find("fuck") == std::string::npos);
    CHECK(allText(clean).find("classic.") != std::string::npos);

    f.h.ok("captions.generateFromTranscript", {{"name", "Raw"}});
    const CaptionTrackId raw = f.h.sequence(f.h.state()).captionTracks.back().id;
    CHECK(allText(f.h.sequence(f.h.state()).captionTracks.back()).find("fuck") != std::string::npos);
    const std::size_t undo = f.h.session.undoCount();
    auto r = f.h.ok("captions.hideProfanity", {{"captionTrack", raw.value}, {"allowedWords", Json::array({"shit*"})}});
    CHECK(f.h.session.undoCount() == undo + 1);
    CHECK(r.data["cues"].get<int>() >= 1);
    const std::string text = allText(f.h.sequence(f.h.state()).captionTracks.back());
    CHECK(text.find("f***,") != std::string::npos);
    CHECK(text.find("shitty") != std::string::npos); // allowed this time
}

TEST_CASE("audio.bleepProfanity bleeps the words in the sound only and captions follow, one undo step",
          "[ai][edit][profanity][redaction]") {
    Fixture f;
    f.h.ok("captions.generateFromTranscript", {{"name", "Captions"}});
    const std::size_t undo = f.h.session.undoCount();
    // A dry run lists the words and changes nothing.
    const auto preview = f.h.run(Json{{"command", "audio.bleepProfanity"}, {"params", Json::object()}, {"dryRun", true}});
    REQUIRE(preview.ok());
    CHECK(f.h.state()->redactions.empty());
    auto r = f.h.ok("audio.bleepProfanity", {{"padding", 0.0}});
    CHECK(f.h.session.undoCount() == undo + 1);
    REQUIRE(r.data["words"].size() == 2);
    CHECK(r.data["words"][0]["text"] == "f***,");
    REQUIRE(f.h.state()->redactions.size() == 1);
    const Redaction& red = *f.h.state()->redactions.begin();
    CHECK(red.audio == RedactionAudio::Bleep);
    CHECK(red.audioRanges.size() == 2);
    // The picture is untouched: the region has no size (an empty shape covers nothing).
    CHECK(maskShapeAt(red.region, Ticks{}, 16.0 / 9.0).empty());
    // Captions over the bleeped words now read [bleep] (redacted speech), the others stay.
    const std::string text = allText(f.h.sequence(f.h.state()).captionTracks.back());
    CHECK(text.find("fuck") == std::string::npos);
    CHECK(text.find(std::string(kRedactedWordText)) != std::string::npos);
    CHECK(text.find("classic.") != std::string::npos);
    // Bleeped words are not found again.
    auto again = f.h.ok("audio.bleepProfanity");
    CHECK(again.data["words"].empty());
    CHECK(f.h.errorOf("audio.bleepProfanity", {{"mode", "none"}}) == "InvalidParams");
}
