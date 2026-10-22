// C.7 on the transcript side: corrections that survive re-transcription, speakers on transcripts and captions,
// captions.generateFromTranscript / updateFromTranscript, Thai line breaking through the host's breaker, and
// captions following transcript-first deletes.

#include "CommandHarness.h"
#include "EditTestUtil.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/edit/TranscriptEditing.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/interchange/Docx.h>

#include <catch2/catch_test_macros.hpp>

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

std::shared_ptr<const Transcript> interview() {
    return transcript("en", {{"So", 0, 3, 0},       // 0
                             {"welcome", 8, 13, 0}, // 1
                             {"to", 13, 15, 0},     // 2
                             {"teh", 15, 17, 0},    // 3  a typo
                             {"show.", 17, 22, 0},  // 4  then a pause
                             {"Thanks", 62, 67, 1}, // 5
                             {"for", 67, 69, 1},    // 6
                             {"having", 69, 74, 1}, // 7
                             {"me.", 74, 80, 1}});  // 8
}

struct Fixture {
    explicit Fixture(std::shared_ptr<const command::CommandServices> extra = nullptr)
        : transcripts(provider(Timeline(kPicture), {{'A', interview()}})), h(linked(), policy(transcripts, extra), editingCommands()) {}

    static Timeline linked() {
        Timeline tl(kPicture);
        tl.linkMatching();
        return tl;
    }
    static command::EnginePolicy policy(const std::shared_ptr<MapTranscriptProvider>& p,
                                        const std::shared_ptr<const command::CommandServices>& extra) {
        std::vector<std::shared_ptr<command::CommandServices>> parts{std::make_shared<TranscriptServices>(p)};
        if (extra) {
            parts.push_back(std::const_pointer_cast<command::CommandServices>(extra));
        }
        return command::EnginePolicy{command::CapabilitySet::all(), 1024,
                                     std::make_shared<command::ServiceBundle>(std::move(parts))};
    }
    std::vector<std::string> words() {
        std::vector<std::string> out;
        for (const auto& w : h.ok("transcript.words").data["words"]) {
            out.push_back(w[1].get<std::string>());
        }
        return out;
    }
    std::vector<std::string> cueTexts() {
        std::vector<std::string> out;
        for (const CaptionCue& c : h.sequence(h.state()).captionTracks.front().cues) {
            out.push_back(c.text);
        }
        return out;
    }

    std::shared_ptr<MapTranscriptProvider> transcripts;
    CommandHarness h;
};

class FakeBreaker final : public LineBreaker {
public:
    explicit FakeBreaker(std::vector<std::size_t> offsets) : m_offsets(std::move(offsets)) {}
    std::vector<std::size_t> breakOpportunities(std::string_view, std::string_view) const override { return m_offsets; }

private:
    std::vector<std::size_t> m_offsets;
};

} // namespace

TEST_CASE("Transcript corrections apply on top of the cache and survive re-transcription", "[ai][edit][captions][corrections]") {
    Fixture fx;
    const ProjectSnapshot before = fx.h.state();
    const auto dry = fx.h.run(Json{{"command", "transcript.correctWord"}, {"params", Json{{"word", 3}, {"text", "the"}}}, {"dryRun", true}});
    REQUIRE(dry.ok());
    CHECK(*fx.h.state() == *before);
    const auto fixed = fx.h.ok("transcript.correctWord", Json{{"word", 3}, {"text", "the"}});
    CHECK(fixed.data["original"] == "teh");
    fx.h.ok("transcript.correctRange", Json{{"words", Json::array({5, 6})}, {"text", "Thank you"}});
    CHECK(fx.words() == std::vector<std::string>{"So", "welcome", "to", "the", "show.", "Thank", "you", "having", "me."});
    CHECK(fx.h.session.undoCount() == 2);
    CHECK(fx.transcripts->transcript(*fx.h.state()->findMedia(fx.h.tl.media('A')), 0)->words[3].text == "teh"); // cache raw

    // Re-transcribed: timings moved a little, "for" heard as "four", and "So" lost.
    fx.transcripts->add(fx.h.tl.media('A'), 0,
                        transcript("en", {{"welcome", 8.2, 13, 0},
                                          {"to", 13, 15.1, 0},
                                          {"teh", 15.1, 17.2, 0},
                                          {"show.", 17.2, 22, 0},
                                          {"Thanks", 62.3, 67, 1},
                                          {"four", 67, 69, 1},
                                          {"having", 69, 74, 1},
                                          {"me.", 74, 80, 1}}));
    CHECK(fx.words() == std::vector<std::string>{"welcome", "to", "the", "show.", "Thank", "you", "having", "me."});
    const auto report = fx.h.ok("transcript.corrections");
    REQUIRE(report.data["corrections"].size() == 2);
    CHECK(report.data["corrections"][0][6] == "applied");
    CHECK(report.data["corrections"][1][6] == "conflict"); // "Thanks four" now, still corrected
    CHECK(report.data["corrections"][1][7] == "Thanks four");

    // A correction whose words disappear brings them back; the project file keeps all of it.
    fx.transcripts->add(fx.h.tl.media('A'), 0, transcript("en", {{"welcome", 8, 13, 0}, {"show.", 17, 22, 0}}));
    CHECK(fx.words() == std::vector<std::string>{"welcome", "the", "show.", "Thank", "you"});
    CHECK(fx.h.ok("transcript.corrections").data["corrections"][0][6] == "restored");
    CHECK(fx.h.errorOf("transcript.correctRange", Json{{"words", Json::array({2, 1})}, {"text", "x"}}) == "InvalidParams");
}

TEST_CASE("captions.generateFromTranscript: speakers, gaps, punctuation; updateFromTranscript", "[ai][edit][captions]") {
    Fixture fx;
    const auto made = fx.h.ok("captions.generateFromTranscript", Json{{"gap", 0.08}, {"maxCharsPerLine", 20}});
    CHECK(made.data["cuesWithSpeaker"] == made.data["cues"]);
    CHECK(fx.h.session.undoCount() == 1); // speakers registered in the same step
    const auto speakers = fx.h.ok("transcript.speakers");
    REQUIRE(speakers.data["speakers"].size() == 2);
    REQUIRE(speakers.data["labels"].size() == 2);
    const auto first = speakers.data["labels"][0][3].get<std::uint64_t>();
    const auto second = speakers.data["labels"][1][3].get<std::uint64_t>();
    const std::vector<CaptionCue> cues = fx.h.sequence(fx.h.state()).captionTracks.front().cues;
    REQUIRE(cues.size() == 2);
    CHECK(cues[0].speaker == SpeakerId{first});
    CHECK(cues[1].speaker == SpeakerId{second});
    CHECK(fx.cueTexts() == std::vector<std::string>{"So welcome\nto teh show.", "Thanks for\nhaving me."}); // balanced

    // The guest gets a name; a correction reaches the existing cues on request, timing kept.
    fx.h.ok("transcript.renameSpeaker", Json{{"media", "A"}, {"label", 1}, {"name", "Guest"}});
    CHECK(fx.h.state()->findSpeaker(SpeakerId{second})->name == "Guest");
    const Ticks start = cues[0].start;
    fx.h.ok("transcript.correctWord", Json{{"word", 3}, {"text", "the"}});
    CHECK(fx.cueTexts()[0] == "So welcome\nto teh show."); // generated before the correction
    CHECK(fx.h.ok("captions.updateFromTranscript", Json{{"maxCharsPerLine", 20}}).data["updated"] == 1);
    CHECK(fx.cueTexts()[0] == "So welcome\nto the show.");
    CHECK(fx.h.sequence(fx.h.state()).captionTracks.front().cues[0].start == start);

    // An assigned speaker wins over diarization in the next take, which keeps the track's style.
    fx.h.ok("captions.applyPreset", Json{{"preset", "Podcast"}});
    fx.h.ok("transcript.assignSpeaker", Json{{"start", tc(60)}, {"end", tc(90)}, {"speaker", first}});
    const auto id = made.data["captionTrack"].get<std::uint64_t>();
    fx.h.ok("captions.generateFromTranscript", Json{{"replace", id}, {"maxCharsPerLine", 20}});
    const CaptionTrack& again = fx.h.sequence(fx.h.state()).captionTracks.front();
    CHECK(again.cues[1].speaker == SpeakerId{first});
    CHECK(again.style.showSpeakers);

    // No breaks at pauses, speakers or sentence ends: the 3.2 s of speech fit one cue.
    Fixture other;
    other.h.ok("captions.generateFromTranscript", Json{{"splitOnPunctuation", false}, {"maxPause", 30.0}, {"breakOnSpeakerChange", false},
                                                        {"maxCharsPerLine", 60}, {"maxDuration", 10.0}});
    CHECK(other.cueTexts() == std::vector<std::string>{"So welcome to teh show. Thanks for having me."});
}

TEST_CASE("Thai captions break inside long word units only through the host's line breaker", "[ai][edit][captions][thai]") {
    // One long Thai unit (no spaces): สวัสดีครับวันนี้อากาศดีมาก.
    const std::string sawasdee = "\xE0\xB8\xAA\xE0\xB8\xA7\xE0\xB8\xB1\xE0\xB8\xAA\xE0\xB8\x94\xE0\xB8\xB5";
    const std::string krap = "\xE0\xB8\x84\xE0\xB8\xA3\xE0\xB8\xB1\xE0\xB8\x9A";
    const std::string rest = "\xE0\xB8\xA7\xE0\xB8\xB1\xE0\xB8\x99\xE0\xB8\x99\xE0\xB8\xB5\xE0\xB9\x89";
    const std::string unit = sawasdee + krap + rest;
    const auto thai = transcript("th", {{unit, 0, 30, 0}});
    for (const bool withBreaker : {false, true}) {
        auto services = withBreaker ? std::make_shared<command::CaptionServices>(std::make_shared<FakeBreaker>(
                                          std::vector<std::size_t>{sawasdee.size(), sawasdee.size() + krap.size()}))
                                    : nullptr;
        Fixture fx(services);
        fx.transcripts->add(fx.h.tl.media('A'), 0, thai);
        fx.h.ok("captions.generateFromTranscript", Json{{"maxCharsPerLine", 8}, {"speakers", false}});
        CHECK(fx.cueTexts() ==
              std::vector<std::string>{withBreaker ? sawasdee + "\n" + krap + rest : unit}); // 4 | 3 + 3 cells
    }
}

TEST_CASE("Transcript deletes take the captions with them, in one undo step", "[ai][edit][captions][follow]") {
    Fixture fx;
    fx.h.ok("captions.generateFromTranscript", Json{{"maxCharsPerLine", 20}, {"speakers", false}});
    const ProjectSnapshot before = fx.h.state();
    const Ticks second = fx.h.sequence(before).captionTracks.front().cues[1].start;
    fx.h.ok("transcript.deleteRanges", Json{{"words", Json::array({Json::array({1, 2})})}}); // "welcome to"
    const auto& cues = fx.h.sequence(fx.h.state()).captionTracks.front().cues;
    REQUIRE(cues.size() == 2);
    CHECK(cues[1].start < second); // pulled back with the cut
    CHECK(fx.h.sequence(fx.h.state()).captionTracks.front().cues[0].duration <
          fx.h.sequence(before).captionTracks.front().cues[0].duration);
    fx.h.session.undo();
    CHECK(*fx.h.state() == *before);
}

#include <filesystem>
#include <fstream>
#include <sstream>

TEST_CASE("transcript.export writes text and CSV with timecodes and captions.import reads the text back", "[ai][edit][captions][transcriptExport]") {
    namespace fs = std::filesystem;
    Fixture fx;
    const Json paragraphs = fx.h.ok("transcript.text").data["paragraphs"];
    REQUIRE(paragraphs.size() == 2);
    const std::string s0 = paragraphs[0][2];
    const std::string e0 = paragraphs[0][3];
    const std::string s1 = paragraphs[1][2];
    const std::string e1 = paragraphs[1][3];
    // Without a path the text comes back: Premiere's layout, unnamed speakers numbered from 1.
    const auto text = fx.h.ok("transcript.export");
    CHECK(text.data["paragraphs"] == 2);
    CHECK(text.data["text"] == s0 + " - " + e0 + "\nSpeaker 1\nSo welcome to teh show.\n\n" + s1 + " - " + e1 +
                                   "\nSpeaker 2\nThanks for having me.\n");
    CHECK(fx.h.ok("transcript.export", Json{{"speakers", false}}).data["text"] ==
          s0 + " - " + e0 + "\nSo welcome to teh show.\n\n" + s1 + " - " + e1 + "\nThanks for having me.\n");

    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "transcript-export";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const auto readAll = [](const fs::path& file) {
        std::ifstream in(file, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    };
    // CSV, quoted where needed; a dry run writes nothing; an existing file needs "overwrite".
    fx.h.ok("transcript.correctWord", Json{{"word", 3}, {"text", "the, \"live\""}});
    const fs::path csv = dir / "talk.csv";
    REQUIRE(fx.h.run(Json{{"command", "transcript.export"}, {"params", Json{{"path", csv.generic_string()}}}, {"dryRun", true}}).ok());
    CHECK_FALSE(fs::exists(csv));
    const auto written = fx.h.ok("transcript.export", Json{{"path", csv.generic_string()}});
    CHECK(written.data["format"] == "csv");
    CHECK(readAll(csv) == "Start,End,Speaker,Text\n" + s0 + "," + e0 + ",Speaker 1,\"So welcome to the, \"\"live\"\" show.\"\n" + s1 +
                              "," + e1 + ",Speaker 2,Thanks for having me.\n");
    CHECK(fx.h.errorOf("transcript.export", Json{{"path", csv.generic_string()}}) == "EditRejected");
    CHECK(fx.h.errorOf("transcript.export", Json{{"path", (dir / "talk.doc").generic_string()}}) == "InvalidParams");

    // The text file comes back as a caption track, cue per paragraph.
    const fs::path txt = dir / "talk.txt";
    fx.h.ok("transcript.export", Json{{"path", txt.generic_string()}});
    const auto imported = fx.h.ok("captions.import", Json{{"path", txt.generic_string()}});
    CHECK(imported.data["cues"] == 2);
    const auto got = fx.h.ok("captions.get", Json{{"captionTrack", imported.data["captionTrack"]}});
    REQUIRE(got.data["cues"].size() == 2);
    CHECK(got.data["cues"][1][2] == "Thanks for having me.");
}

TEST_CASE("transcript.findReplace corrects every match in one undo step", "[ai][edit][captions][findReplace]") {
    Fixture fx;
    const auto fixed = fx.h.ok("transcript.findReplace", Json{{"find", "teh"}, {"replace", "the"}, {"wholeWord", true}});
    CHECK(fixed.data["replaced"] == 1);
    CHECK(fx.h.session.undoCount() == 1);
    CHECK(fx.words() == std::vector<std::string>{"So", "welcome", "to", "the", "show.", "Thanks", "for", "having", "me."});
    // Across words, any case.
    fx.h.ok("transcript.findReplace", Json{{"find", "THANKS FOR"}, {"replace", "Thank you for"}});
    CHECK(fx.words() == std::vector<std::string>{"So", "welcome", "to", "the", "show.", "Thank", "you", "for", "having", "me."});
    CHECK(fx.h.session.undoCount() == 2);
    // Match Case and Whole Word.
    CHECK(fx.h.ok("transcript.findReplace", Json{{"find", "so"}, {"replace", "Well"}, {"matchCase", true}}).data["replaced"] == 0);
    const auto me = fx.h.ok("transcript.findReplace", Json{{"find", "me"}, {"replace", "us"}, {"wholeWord", true}});
    CHECK(me.data["replaced"] == 1);
    CHECK(fx.words().back() == "us.");
    CHECK(fx.words()[1] == "welcome");
    // A dry run changes nothing.
    const ProjectSnapshot before = fx.h.state();
    REQUIRE(fx.h.run(Json{{"command", "transcript.findReplace"}, {"params", Json{{"find", "the"}, {"replace", "a"}}}, {"dryRun", true}}).ok());
    CHECK(*fx.h.state() == *before);
}

TEST_CASE("transcript.export writes a Word document and captions.import reads it back", "[ai][edit][captions][transcriptExport][docx]") {
    namespace fs = std::filesystem;
    Fixture fx;
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "transcript-export-docx";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path docx = dir / "talk.docx";
    const auto written = fx.h.ok("transcript.export", Json{{"path", docx.generic_string()}});
    CHECK(written.data["format"] == "docx");
    std::ifstream in(docx, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string bytes = ss.str();
    CHECK(bytes.starts_with("PK"));
    // The document reads as the .txt layout: timecodes, the speaker, the text, a blank paragraph between blocks.
    const auto text = interchange::readDocxText(bytes);
    REQUIRE(text);
    const std::string plain = fx.h.ok("transcript.export").data["text"];
    CHECK(*text == plain);
    // Without a path there is nothing to return a Word document in.
    CHECK(fx.h.errorOf("transcript.export", Json{{"format", "docx"}}) == "InvalidParams");
    // captions.import reads it into a caption track, speakers included.
    const auto imported = fx.h.ok("captions.import", Json{{"path", docx.generic_string()}});
    CHECK(imported.data["cues"] == 2);
    const auto got = fx.h.ok("captions.get", Json{{"captionTrack", imported.data["captionTrack"]}});
    REQUIRE(got.data["cues"].size() == 2);
    CHECK(got.data["cues"][0][2] == "So welcome to teh show.");
    CHECK(got.data["cues"][1][2] == "Thanks for having me.");
    // A Word document without timed blocks is refused.
    const std::vector<interchange::DocxParagraph> notes{{{{"Just some notes", false, false}}}};
    {
        std::ofstream out(dir / "notes.docx", std::ios::binary);
        out << interchange::writeDocx(notes, "notes");
    }
    CHECK(fx.h.errorOf("captions.import", Json{{"path", (dir / "notes.docx").generic_string()}}) == "InvalidParams");
}

TEST_CASE("transcript.find lists the matches and transcript.findReplace replaces one", "[ai][edit][captions][findReplace]") {
    Fixture fx;
    // Two matches of "o" ... as words: "so" and "to" contain it; find "o" within words.
    const auto found = fx.h.ok("transcript.find", Json{{"find", "TO"}, {"wholeWord", true}});
    REQUIRE(found.data["count"] == 1);
    CHECK(found.data["matches"][0]["words"] == Json::array({2, 2}));
    const auto across = fx.h.ok("transcript.find", Json{{"find", "for having"}});
    REQUIRE(across.data["count"] == 1);
    CHECK(across.data["matches"][0]["words"] == Json::array({6, 7}));
    CHECK(across.data["matches"][0]["text"] == "for having");
    // Replace only the match on word 3; the other "e"s stay.
    const auto ones = fx.h.ok("transcript.find", Json{{"find", "e"}});
    CHECK(ones.data["count"].get<int>() >= 3);
    fx.h.ok("transcript.findReplace", Json{{"find", "teh"}, {"replace", "the"}, {"words", Json::array({3, 3})}});
    CHECK(fx.words()[3] == "the");
    CHECK(fx.h.session.undoCount() == 1);
    // A stale match (the words changed) is refused.
    CHECK(fx.h.errorOf("transcript.findReplace", Json{{"find", "teh"}, {"replace", "the"}, {"words", Json::array({3, 3})}}) ==
          "NotFound");
    // Without Match Case the fold is Unicode's.
    fx.h.ok("transcript.correctWord", Json{{"word", 1}, {"text", "\xC3\x89""cole"}});
    CHECK(fx.h.ok("transcript.find", Json{{"find", "\xC3\xA9""COLE"}}).data["count"] == 1);
}
