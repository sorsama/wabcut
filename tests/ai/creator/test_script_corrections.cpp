// Correct the transcript from the script (0.6, P1 round #37): transcript.correctFromScript finds each line of the script
// in the sequence's transcript and replaces the words the recogniser got wrong (names, jargon) with the script's -
// substitutions only (a skipped word is never inserted, an added one never deleted), casing and punctuation from the
// script, retakes corrected too, lines that do not match skipped and listed; Thai cluster runs become whole words. One
// undo step; a dry run changes nothing.

#include "CommandHarness.h"
#include "CreatorTestUtil.h"

#include <wabcut/ai/roughcut/ScriptCorrections.h>
#include <wabcut/model/CaptionOps.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::roughcut;
using namespace wabcut::ai::roughcut::test;
using wabcut::test::CommandHarness;
using command::Json;

namespace {

/// V1/A1 play media A from 0 for `frames` frames (25 fps).
std::string longClip(int frames) {
    const std::string clip = "A" + std::string(static_cast<std::size_t>(frames - 1), 'a');
    return "V1 tgt |" + clip + "|\nA1 tgt |" + clip + "|\n";
}

/// The sequence's transcript text as the project has it now (corrections applied).
std::string textNow(const ProjectState& state, SequenceId seq, const ai::edit::TranscriptProvider& transcripts) {
    const ai::edit::SequenceTranscript st = ai::edit::SequenceTranscript::build(state, seq, transcripts);
    return st.empty() ? std::string() : st.textOf(0, st.size() - 1);
}

struct Rig {
    explicit Rig(const std::vector<W>& words, const std::string& language = "en", int frames = 1500) : tl(longClip(frames), std::map<char, std::int64_t>{{'A', frames + 100}}) {
        tl.linkMatching();
        transcripts = std::make_shared<ai::edit::MapTranscriptProvider>();
        transcripts->add(tl.media('A'), 0, ai::edit::test::transcript(language, words));
    }
    command::EnginePolicy policy() const {
        return command::EnginePolicy{command::CapabilitySet::all(), 64, std::make_shared<ai::edit::TranscriptServices>(transcripts)};
    }
    Timeline tl;
    std::shared_ptr<ai::edit::MapTranscriptProvider> transcripts;
};

constexpr const char* kScript = "Today we test Wabcut with Dr. Somchai in the studio.\n"
                                "We talk about Kubernetes for a while.\n"
                                "This line was never said by anyone here.\n";

std::vector<W> interview() {
    std::vector<W> words;
    double t = say(words, 1.0, "So um today we test web cut with Dr. Somchay in the studio.");
    t = say(words, t + 1.0, "We talk about cooper nettie's for a little while.");
    t = say(words, t + 1.5, "Today we test web cut with Somchay in the studio."); // a retake that skips "Dr."
    say(words, t + 1.5, "Something else entirely happens here.");
    return words;
}

} // namespace

TEST_CASE("Correct from script fixes misheard words of every take and leaves skipped and added words alone",
          "[ai][creator][corrections]") {
    Rig rig(interview());
    CommandHarness h(rig.tl, rig.policy(), ai::edit::editingCommands());
    const SequenceId seq = rig.tl.seq;
    const auto before = h.state();
    const std::string original = textNow(*before, seq, *rig.transcripts);
    REQUIRE(original.find("web cut") != std::string::npos);

    // A dry run lists the changes and changes nothing.
    const auto dry = h.run(Json{{"command", "transcript.correctFromScript"}, {"params", Json{{"text", kScript}}}, {"dryRun", true}});
    INFO(command::toJson(dry).dump());
    REQUIRE(dry.ok());
    CHECK_FALSE(dry.committed);
    CHECK(h.state() == before);
    const Json& data = dry.data;
    // "Dr." does not end the line (a title, not a sentence end).
    CHECK(splitProse("Today we test Wabcut with Dr. Somchai in the studio.").size() == 1);
    CHECK(data["corrected"] == 5);
    CHECK(data["beats"] == 3);
    CHECK(data["beatsMatched"] == 2);
    CHECK(data["beatsSkipped"] == 1);
    REQUIRE(data["skipped"].size() == 1);
    CHECK(data["skipped"][0]["line"] == 3);
    REQUIRE(data["changes"].size() == 5);
    // In transcript order: the first take, the second line, the retake.
    const std::pair<const char*, const char*> expected[] = {
        {"web cut", "Wabcut"}, {"Somchay", "Somchai"}, {"cooper nettie's", "Kubernetes"}, {"web cut", "Wabcut"}, {"Somchay", "Somchai"}};
    for (std::size_t i = 0; i < 5; ++i) {
        INFO(i);
        CHECK(data["changes"][i]["before"] == expected[i].first);
        CHECK(data["changes"][i]["after"] == expected[i].second);
        CHECK(data["changes"][i]["time"].is_string());
    }
    CHECK(data["changes"][2]["line"] == 2);
    CHECK(data["changes"][0]["words"][0].get<int>() < data["changes"][0]["words"][1].get<int>()); // "web" .. "cut"

    // Applied: one undo step; the transcript reads the script's words, the speaker's own words stay.
    const auto applied = h.ok("transcript.correctFromScript", Json{{"text", kScript}});
    CHECK(applied.committed);
    const std::string corrected = textNow(*h.state(), seq, *rig.transcripts);
    INFO(corrected);
    CHECK(corrected.find("web cut") == std::string::npos);
    CHECK(corrected.find("cooper") == std::string::npos);
    CHECK(corrected.find("today we test Wabcut with Dr. Somchai in the studio.") != std::string::npos);
    CHECK(corrected.find("We talk about Kubernetes for a little while.") != std::string::npos); // "little" kept
    CHECK(corrected.find("Today we test Wabcut with Somchai in the studio.") != std::string::npos); // no "Dr." added
    CHECK(corrected.find("So um") != std::string::npos);                                         // the filler kept
    const TranscriptEdits* edits = h.state()->findTranscriptEdits(rig.tl.media('A'), 0);
    REQUIRE(edits);
    CHECK(edits->corrections.size() == 5);

    // Again: nothing left to correct, nothing committed.
    const auto again = h.ok("transcript.correctFromScript", Json{{"text", kScript}});
    CHECK(again.data["corrected"] == 0);
    CHECK_FALSE(again.committed);

    REQUIRE(h.session.undo());
    CHECK(textNow(*h.state(), seq, *rig.transcripts) == original);

    // Refusals: a script with no lines, a stricter match than any line reaches, no transcript.
    CHECK(h.errorOf("transcript.correctFromScript", Json{{"text", "\n\n"}}) == "InvalidParams");
    const auto strict = h.ok("transcript.correctFromScript", Json{{"text", kScript}, {"minMatch", 0.99}});
    CHECK(strict.data["corrected"] == 0);
    CHECK(strict.data["beatsSkipped"] == 3);
    CommandHarness none(rig.tl, command::EnginePolicy{command::CapabilitySet::all(), 64,
                                                      std::make_shared<ai::edit::TranscriptServices>(std::make_shared<ai::edit::MapTranscriptProvider>())},
                        ai::edit::editingCommands());
    CHECK(none.errorOf("transcript.correctFromScript", Json{{"text", kScript}}) == "EditRejected");
}

TEST_CASE("Correct from script grows Thai cluster fixes to whole words", "[ai][creator][corrections][thai]") {
    std::vector<W> words;
    // The recogniser's words; the last one is misspelled (ไหม่ for ใหม่, the commonest vowel slip).
    say(words, 1.0, "สวัสดี ครับ วันนี้ เรา มา ลอง โปรแกรม ไหม่");
    Rig rig(words, "th");
    CommandHarness h(rig.tl, rig.policy(), ai::edit::editingCommands());
    const auto result = h.ok("transcript.correctFromScript", Json{{"text", "สวัสดีครับ วันนี้เรามาลองโปรแกรมใหม่"}});
    INFO(result.data.dump());
    REQUIRE(result.data["corrected"] == 1);
    CHECK(result.data["changes"][0]["before"] == "ไหม่");
    CHECK(result.data["changes"][0]["after"] == "ใหม่");
    const std::string corrected = textNow(*h.state(), rig.tl.seq, *rig.transcripts);
    CHECK(corrected.find("ใหม่") != std::string::npos);
    CHECK(corrected.find("ไหม่") == std::string::npos);
}

TEST_CASE("Correct from script keeps changed words and fixes numbers abbreviations and capitals",
          "[ai][creator][corrections]") {
    std::vector<W> words;
    const double t = say(words, 1.0, "This is a great tool called kubernetes and it runs on github every day.");
    say(words, t + 1.0, "It has 25 nodes and Doctor Somchai restarts them 3 times a week.");
    Rig rig(words);
    CommandHarness h(rig.tl, rig.policy(), ai::edit::editingCommands());
    const std::string script = "This is a good tool called Kubernetes, and it runs on GitHub every day.\n"
                               "It has twenty-five nodes and Dr. Somchai restarts them five times a week.\n";
    const auto dry = h.run(Json{{"command", "transcript.correctFromScript"}, {"params", Json{{"text", script}}}, {"dryRun", true}});
    INFO(command::toJson(dry).dump());
    REQUIRE(dry.ok());
    const Json& data = dry.data;
    // Names and terms take the script's capitals (only their capitals: the transcript's own punctuation stays); a
    // number and an abbreviation written another way take the script's form.
    REQUIRE(data["corrected"] == 4);
    CHECK(data["caseFixes"] == 2);
    REQUIRE(data["changes"].size() == 4);
    const std::tuple<const char*, const char*, const char*> expected[] = {
        {"kubernetes", "Kubernetes", "case"}, {"github", "GitHub", "case"}, {"25", "twenty-five", "words"}, {"Doctor", "Dr.", "words"}};
    for (std::size_t i = 0; i < 4; ++i) {
        INFO(i);
        CHECK(data["changes"][i]["before"] == std::get<0>(expected[i]));
        CHECK(data["changes"][i]["after"] == std::get<1>(expected[i]));
        CHECK(data["changes"][i]["kind"] == std::get<2>(expected[i]));
    }
    // "great" for "good" and 3 for five are what the speaker said: kept and listed.
    CHECK(data["keptCount"] == 2);
    REQUIRE(data["kept"].size() == 2);
    CHECK(data["kept"][0]["said"] == "great");
    CHECK(data["kept"][0]["script"] == "good");
    CHECK(data["kept"][1]["said"] == "3");
    CHECK(data["kept"][1]["script"] == "five");
    CHECK(data["kept"][1]["time"].is_string());

    h.ok("transcript.correctFromScript", Json{{"text", script}});
    const std::string corrected = textNow(*h.state(), rig.tl.seq, *rig.transcripts);
    INFO(corrected);
    CHECK(corrected.find("This is a great tool called Kubernetes and it runs on GitHub every day.") != std::string::npos);
    CHECK(corrected.find("It has twenty-five nodes and Dr. Somchai restarts them 3 times a week.") != std::string::npos);

    // Capitals alone can be left as they are.
    REQUIRE(h.session.undo());
    const auto noCase = h.ok("transcript.correctFromScript", Json{{"text", script}, {"fixCase", false}});
    CHECK(noCase.data["corrected"] == 2);
    CHECK(noCase.data["caseFixes"] == 0);
}

TEST_CASE("Correct from script tells a mishearing from a different word", "[ai][creator][corrections]") {
    // Misheard names and jargon, homophones, numbers and abbreviations written another way.
    const std::pair<const char*, const char*> alike[] = {
        {"web cut", "Wabcut"},        {"Somchay", "Somchai"},         {"cooper nettie's", "Kubernetes"}, {"communities", "Kubernetes"},
        {"cuban eighties", "Kubernetes"}, {"psalm chai", "Somchai"},  {"their", "there"},                {"then", "than"},
        {"chat GPT", "ChatGPT"},      {"open eye", "OpenAI"},         {"jason", "JSON"},                 {"pie torch", "PyTorch"},
        {"my sequel", "MySQL"},       {"25", "twenty-five"},          {"1990", "nineteen ninety"},       {"3,000", "three thousand"},
        {"125", "one hundred and twenty five"}, {"Doctor", "Dr."},    {"1st", "first"},                  {"okay", "OK"}};
    for (const auto& [said, script] : alike) {
        INFO(said << " / " << script);
        CHECK(soundsAlike(said, script));
    }
    // Words the speaker changed: left as said.
    const std::pair<const char*, const char*> different[] = {
        {"great", "good"}, {"this", "that"}, {"big", "large"}, {"is", "was"},     {"we", "they"},     {"a", "the"},
        {"3", "five"},     {"fifteen", "fifty"}, {"start", "begin"}, {"kind", "sort"}, {"people", "folks"}, {"huge", "big"}};
    for (const auto& [said, script] : different) {
        INFO(said << " / " << script);
        CHECK_FALSE(soundsAlike(said, script));
    }
    // Thai: the vowel slip is a mishearing, another word is not.
    CHECK(soundsAlike("ไหม่", "ใหม่"));
    CHECK_FALSE(soundsAlike("วันนี้", "เมื่อวาน"));
}

TEST_CASE("Correct from script finds a line with a long aside in parts", "[ai][creator][corrections]") {
    std::vector<W> words;
    say(words, 1.0,
        "Today we test the new web cut timeline and honestly I have been waiting for this moment for such a long time that "
        "you would not believe how happy I am right now with Doctor Somchay in our Bangkok studio.");
    Rig rig(words);
    CommandHarness h(rig.tl, rig.policy(), ai::edit::editingCommands());
    const auto result = h.ok("transcript.correctFromScript",
                             Json{{"text", "Today we test the new Wabcut timeline with Doctor Somchai in our Bangkok studio."}});
    INFO(result.data.dump());
    CHECK(result.data["beatsMatched"] == 1);
    CHECK(result.data["beatsPartial"] == 1);
    REQUIRE(result.data["corrected"] == 2);
    CHECK(result.data["changes"][0]["after"] == "Wabcut");
    CHECK(result.data["changes"][1]["after"] == "Somchai");
    const std::string corrected = textNow(*h.state(), rig.tl.seq, *rig.transcripts);
    CHECK(corrected.find("the new Wabcut timeline and honestly") != std::string::npos); // the aside stays
    CHECK(corrected.find("with Doctor Somchai in our Bangkok studio.") != std::string::npos);
}

TEST_CASE("Correct from script keeps a correction made by hand that it overlaps", "[ai][creator][corrections]") {
    std::vector<W> words;
    say(words, 1.0, "Today we test web cut with Somchay in the studio.");
    Rig rig(words);
    // Earlier, by hand: "web cut with" became "Webcut with" (still misspelled).
    const ai::edit::SequenceTranscript st = ai::edit::SequenceTranscript::build(rig.tl.state, rig.tl.seq, *rig.transcripts);
    REQUIRE(st.size() == 10);
    REQUIRE(st.text(3) == "web");
    REQUIRE(st.text(5) == "with");
    const TimeRange handRange = TimeRange::fromStartEnd(st[3].source.start, st[5].source.end());
    rig.tl.state = wabcut::edit::correctTranscript(rig.tl.state, rig.tl.media('A'), 0, WordCorrection{handRange, "web cut with", "Webcut with"}).value();
    CommandHarness h(rig.tl, rig.policy(), ai::edit::editingCommands());
    REQUIRE(textNow(*h.state(), rig.tl.seq, *rig.transcripts) == "Today we test Webcut with Somchay in the studio.");

    const auto result = h.ok("transcript.correctFromScript", Json{{"text", "Today we test Wabcut with Somchai in the studio."}});
    INFO(result.data.dump());
    CHECK(result.data["corrected"] == 2);
    CHECK(textNow(*h.state(), rig.tl.seq, *rig.transcripts) == "Today we test Wabcut with Somchai in the studio.");
    // One correction over the hand-made one: its words kept ("with"), its original as transcribed.
    const TranscriptEdits* edits = h.state()->findTranscriptEdits(rig.tl.media('A'), 0);
    REQUIRE(edits);
    REQUIRE(edits->corrections.size() == 2);
    CHECK(edits->corrections[0].source == handRange);
    CHECK(edits->corrections[0].original == "web cut with");
    CHECK(edits->corrections[0].text == "Wabcut with");
    REQUIRE(h.session.undo());
    CHECK(textNow(*h.state(), rig.tl.seq, *rig.transcripts) == "Today we test Webcut with Somchay in the studio.");
}

TEST_CASE("Correct from script leaves out words that cross a cut", "[ai][creator][corrections]") {
    std::vector<W> words;
    // "web" is frames 55-63 of media A, "cut" frames 65-73: a through edit at frame 64 puts them in two clips.
    say(words, 1.0, "Today we test web cut with Somchay in the studio.");
    const std::string first = "A" + std::string(63, 'a');
    const std::string second = "A64" + std::string(433, 'a');
    Timeline tl("V1 tgt |" + first + second + "|\nA1 tgt |" + first + second + "|\n");
    tl.linkMatching();
    auto transcripts = std::make_shared<ai::edit::MapTranscriptProvider>();
    transcripts->add(tl.media('A'), 0, ai::edit::test::transcript("en", words));
    CommandHarness h(tl, command::EnginePolicy{command::CapabilitySet::all(), 64, std::make_shared<ai::edit::TranscriptServices>(transcripts)},
                     ai::edit::editingCommands());
    const auto result = h.ok("transcript.correctFromScript", Json{{"text", "Today we test Wabcut with Somchai in the studio."}});
    INFO(result.data.dump());
    CHECK(result.data["corrected"] == 1);
    CHECK(result.data["leftOut"] == 1);
    CHECK(result.data["changes"][0]["after"] == "Somchai");
    const std::string corrected = textNow(*h.state(), tl.seq, *transcripts);
    CHECK(corrected.find("web cut with Somchai") != std::string::npos);
}

namespace {

/// Records the progress messages and stops the request at the first one that contains `stopAt` ("" = never).
class Recorder final : public command::ExecutionControl {
public:
    explicit Recorder(std::string stopAt = {}) : m_stopAt(std::move(stopAt)) {}
    std::optional<command::CommandError> checkpoint(std::string_view message) override {
        messages.emplace_back(message);
        if (!m_stopAt.empty() && message.find(m_stopAt) != std::string_view::npos) {
            command::CommandError error;
            error.code = command::ErrorCode::Cancelled;
            error.message = "cancelled";
            return error;
        }
        return std::nullopt;
    }
    std::vector<std::string> messages;

private:
    std::string m_stopAt;
};

} // namespace

TEST_CASE("Correct from script stays quick on an hour of speech and a long script", "[ai][creator][corrections][perf]") {
    // About 12,600 words (an hour of narration) and a 12,600-word script of 900 lines: the plan is text only.
    std::vector<W> words;
    std::string script;
    double t = 1.0;
    for (int i = 0; i < 900; ++i) {
        const std::string n = std::to_string(i);
        t = say(words, t, "Paragraph " + n + " talks about the web cut editor and the timeline in section " + n + ".", 0.18, 0.04);
        t += 0.8;
        script += "Paragraph " + n + " talks about the Wabcut editor and the timeline in section " + n + ".\n";
    }
    Rig rig(words, "en", static_cast<int>(t * 25.0) + 50);
    CommandHarness h(rig.tl, rig.policy(), ai::edit::editingCommands());
    command::CommandRequest request;
    request.command = "transcript.correctFromScript";
    request.params = Json{{"text", script}};
    request.dryRun = true;
    Recorder progress;
    const auto started = std::chrono::steady_clock::now();
    const auto dry = h.engine.execute(request, &progress);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    INFO((dry.error ? dry.error->message : std::string()));
    REQUIRE(dry.ok());
    CHECK(dry.data["beats"] == 900);
    CHECK(dry.data["corrected"] == 900);
    CHECK(dry.data["changes"].size() == 200); // the list is capped; every correction is made
    UNSCOPED_INFO("correct from script, 12,600 words x 900 lines: " << ms << " ms");
    CHECK(ms < 10000.0);
    // Progress in percents, so the app shows how far it is and can stop it there.
    const auto percents = std::ranges::count_if(progress.messages, [](const std::string& m) {
        return m.starts_with("finding your lines in the transcript (") && m.ends_with("%)");
    });
    CHECK(percents >= 100);

    // Stopped half way: an error, nothing changed.
    const auto before = h.state();
    request.dryRun = false;
    Recorder stop("(50%)");
    const auto stopped = h.engine.execute(request, &stop);
    REQUIRE_FALSE(stopped.ok());
    CHECK(stopped.error->code == command::ErrorCode::Cancelled);
    CHECK(h.state() == before);
}
