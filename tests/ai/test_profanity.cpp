// p02 audio: profanity (Profanity.h): Premiere's Speech to Text "Hide profanity" in captions ("f***"), the editable
// English and Thai lists, and audio.bleepProfanity's audio-only redactions over the words (captions over them then read
// [bleep]), each one undo step.

#include "CommandHarness.h"
#include "EditTestUtil.h"

#include <wabcut/ai/edit/Profanity.h>
#include <wabcut/ai/edit/TranscriptCommands.h>

#include <wabcut/model/MediaLocator.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
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

// ---- 0.7 (card 102): a sound file of the user's instead of the bleep tone --------------------------------------------

namespace {

/// A tiny mono 16-bit WAV (silence is fine: the command only needs a readable file) in the test output folder.
std::string writeSoundFile(const char* name, bool riff = true) {
    const std::filesystem::path dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "replace-profanity";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / name;
    std::ofstream out(file, std::ios::binary);
    if (riff) {
        const std::uint32_t samples = 4800;
        const auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
        const auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
        out.write("RIFF", 4);
        u32(36 + samples * 2);
        out.write("WAVEfmt ", 8);
        u32(16);
        u16(1);
        u16(1);
        u32(48000);
        u32(96000);
        u16(2);
        u16(16);
        out.write("data", 4);
        u32(samples * 2);
        for (std::uint32_t i = 0; i < samples; ++i) {
            u16(static_cast<std::uint16_t>(1000 + i % 50));
        }
    } else {
        out << "not a sound";
    }
    return pathToUtf8(file);
}

} // namespace

TEST_CASE("audio.replaceProfanity plays the user's sound file on the words instead of the tone", "[ai][edit][profanity][redaction]") {
    const command::CommandSpec* spec = editingCommands().find("audio.replaceProfanity");
    REQUIRE(spec);
    CHECK(spec->touchesFiles); // it reads a file the caller names: held back from hosts that have no file access
    CHECK(spec->fileParams == std::vector<std::string>{"sound"});
    CHECK_FALSE(editingCommands().find("audio.bleepProfanity")->touchesFiles); // as before

    Fixture f;
    f.h.ok("captions.generateFromTranscript", {{"name", "Captions"}});
    const std::string sound = writeSoundFile("boing.wav");
    const std::size_t undo = f.h.session.undoCount();

    const auto dry = f.h.run(Json{{"command", "audio.replaceProfanity"}, {"params", Json{{"sound", sound}}}, {"dryRun", true}});
    REQUIRE(dry.ok());
    CHECK(f.h.state()->redactions.empty());
    CHECK(f.h.state()->assets.empty());

    const auto r = f.h.ok("audio.replaceProfanity", {{"sound", sound}, {"padding", 0.0}});
    CHECK(f.h.session.undoCount() == undo + 1); // one step: the file and the redaction together
    REQUIRE(r.data["words"].size() == 2);
    CHECK(std::filesystem::path(r.data["sound"].get<std::string>()) == std::filesystem::path(sound)); // (separators may differ)
    REQUIRE(f.h.state()->assets.size() == 1);
    const ProjectAsset& asset = f.h.state()->assets.front();
    CHECK(asset.kind == kAssetKindSound);
    CHECK(asset.name == "boing.wav");
    CHECK(std::filesystem::path(asset.locator.path) == std::filesystem::path(sound));
    REQUIRE(f.h.state()->redactions.size() == 1);
    const Redaction& red = *f.h.state()->redactions.begin();
    CHECK(red.audio == RedactionAudio::Bleep);
    CHECK(red.sound == asset.id);
    CHECK(red.audioRanges.size() == 2);
    // Captions over the replaced words read [bleep], as for a bleep.
    const std::string text = allText(f.h.sequence(f.h.state()).captionTracks.back());
    CHECK(text.find("fuck") == std::string::npos);
    CHECK(text.find(std::string(kRedactedWordText)) != std::string::npos);

    SECTION("undo takes the file and the redaction back") {
        REQUIRE(f.h.session.undo());
        CHECK(f.h.state()->redactions.empty());
        CHECK(f.h.state()->assets.empty());
    }
    SECTION("the same file is one asset however often it is used") {
        // Another media item with its own words would add a redaction, not an asset: bleep again with the same file.
        REQUIRE(f.h.ok("audio.replaceProfanity", {{"sound", sound}}).data["words"].empty()); // already replaced
        CHECK(f.h.state()->assets.size() == 1);
    }
}

TEST_CASE("audio.replaceProfanity refuses a file it cannot use and changes nothing", "[ai][edit][profanity][redaction]") {
    Fixture f;
    const auto before = f.h.state();
    CHECK(f.h.errorOf("audio.replaceProfanity") == "InvalidParams"); // "sound" is required
    CHECK(f.h.errorOf("audio.replaceProfanity", {{"sound", "C:/does/not/exist.wav"}}) == "InvalidParams");
    CHECK(f.h.errorOf("audio.replaceProfanity", {{"sound", ""}}) == "InvalidParams");
    // A folder is not a sound file.
    CHECK(f.h.errorOf("audio.replaceProfanity", {{"sound", pathToUtf8(std::filesystem::path(WABCUT_TEST_OUTPUT_DIR))}}) == "InvalidParams");
    CHECK(f.h.state() == before);
    // The old command did not learn the parameter: it still bleeps with the tone only.
    CHECK(f.h.errorOf("audio.bleepProfanity", {{"sound", writeSoundFile("late.wav")}}) == "InvalidParams");
}
