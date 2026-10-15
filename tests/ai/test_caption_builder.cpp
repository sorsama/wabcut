// Captions from the transcript (AI-3): cue building, Thai-aware line breaking, SRT/VTT with line breaks.

#include "EditTestUtil.h"

#include <wabcut/ai/edit/CaptionBuilder.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::ai::edit;
using namespace wabcut::ai::edit::test;

namespace {

std::vector<std::string> split(std::string_view text) {
    std::vector<std::string> out;
    std::string_view rest = text;
    while (!rest.empty()) {
        const std::size_t sp = rest.find(' ');
        out.emplace_back(rest.substr(0, sp));
        rest = sp == std::string_view::npos ? std::string_view{} : rest.substr(sp + 1);
    }
    return out;
}

/// "start-end text" per cue in frames, lines joined by '/'.
std::string cues(const CaptionTrack& track) {
    std::string out;
    for (const CaptionCue& c : track.cues) {
        std::string text = c.text;
        std::ranges::replace(text, '\n', '/');
        out += (out.empty() ? "" : " | ") + frameText(c.start) + "-" + frameText(c.end()) + " " + text;
    }
    return out;
}

} // namespace

TEST_CASE("Display length and joining follow the language's script", "[ai][edit][captions]") {
    CHECK(displayLength("Hello") == 5);
    CHECK(displayLength("สวัสดี") == 4);   // the vowel marks above take no cell
    CHECK(displayLength("ไม่เว้น") == 5);   // nor do tone marks
    const std::vector<std::string> en{"Hello", "world", "!"};
    CHECK(joinWords(en, "en") == "Hello world!");
    const std::vector<std::string> th{"ใช้", "Wabcut", "ตัด", "ต่อ", "AI", "tools"};
    CHECK(joinWords(th, "th") == "ใช้Wabcutตัดต่อAI tools");
}

TEST_CASE("English lines break between words, balanced", "[ai][edit][captions]") {
    const auto words = split("The quick brown fox jumps over the lazy dog");
    CHECK(breakLines(words, "en", 43, 2) == std::vector<std::string>{"The quick brown fox jumps over the lazy dog"});
    CHECK(breakLines(words, "en", 42, 2) == std::vector<std::string>{"The quick brown fox", "jumps over the lazy dog"});
    CHECK(breakLines(words, "en", 24, 2) == std::vector<std::string>{"The quick brown fox", "jumps over the lazy dog"});
    CHECK(breakLines(words, "en", 20, 2) == std::nullopt);
    CHECK(breakLines(words, "en", 20, 3) ==
          std::vector<std::string>{"The quick brown", "fox jumps over", "the lazy dog"});
    // Equal balance: break after the comma.
    CHECK(breakLines(split("Yes, we can do it now"), "en", 12, 2) == std::vector<std::string>{"Yes, we can", "do it now"});
    // One word longer than a line stands alone.
    CHECK(breakLines(split("Supercalifragilistic is long"), "en", 10, 2) ==
          std::vector<std::string>{"Supercalifragilistic", "is long"});
}

TEST_CASE("Thai lines break between word units, never at spaces that are not there", "[ai][edit][captions]") {
    const std::vector<std::string> words{"ภาษาไทย", "เขียน", "ติดกัน", "โดย", "ไม่", "เว้น", "วรรค"};
    // Display widths 7 4 4 3 2 3 4 = 27 cells.
    CHECK(breakLines(words, "th", 30, 2) == std::vector<std::string>{"ภาษาไทยเขียนติดกันโดยไม่เว้นวรรค"});
    CHECK(breakLines(words, "th", 16, 2) == std::vector<std::string>{"ภาษาไทยเขียนติดกัน", "โดยไม่เว้นวรรค"});
    CHECK(breakLines(words, "th", 14, 2) == std::nullopt);
    CHECK(breakLines(words, "th", 14, 3).has_value());
}

TEST_CASE("Cues break at pauses, speaker changes, length and duration", "[ai][edit][captions]") {
    Timeline tl(R"(
        A1 |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )",
             {{'A', 2000}});
    SECTION("Pauses and speakers") {
        const auto p = provider(tl, {{'A', transcript("en", {{"Hello", 0, 10, 0},
                                                             {"there.", 10, 20, 0},
                                                             {"How", 70, 75, 0}, // 2 s later
                                                             {"are", 75, 80, 0},
                                                             {"you?", 80, 90, 0},
                                                             {"Fine.", 91, 99, 1}})}}); // another speaker
        const CaptionTrack track = buildCaptions(SequenceTranscript::build(tl.state, tl.seq, *p));
        CHECK(track.language == "en");
        CHECK(track.name == "Captions");
        CHECK(cues(track) == "0-20 Hello there. | 70-90 How are you? | 91-108.50 Fine.");
    }
    SECTION("Two lines of at most 20 characters") {
        std::vector<W> words;
        const auto text = split("We cut the interview down to the best answers and kept every laugh in it");
        for (std::size_t i = 0; i < text.size(); ++i) {
            words.push_back(W{text[i], static_cast<double>(i * 5), static_cast<double>(i * 5 + 4)});
        }
        const auto p = provider(tl, {{'A', transcript("en", words)}});
        CaptionOptions o;
        o.maxCharsPerLine = 20;
        const CaptionTrack track = buildCaptions(SequenceTranscript::build(tl.state, tl.seq, *p), o);
        CHECK(cues(track) == "0-39 We cut the interview/down to the best | 40-74 answers and kept/every laugh in it");
        for (const CaptionCue& c : track.cues) {
            std::string_view rest = c.text;
            while (!rest.empty()) {
                const std::size_t nl = rest.find('\n');
                CHECK(displayLength(rest.substr(0, nl)) <= 20);
                rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
            }
        }
    }
    SECTION("At most maxDuration") {
        std::vector<W> words;
        for (int i = 0; i < 20; ++i) {
            words.push_back(W{"go", i * 5.0, i * 5.0 + 4.0}); // a word every 0.2 s
        }
        const auto p = provider(tl, {{'A', transcript("en", words)}});
        CaptionOptions o;
        o.maxDuration = Ticks::fromSeconds(1);
        o.maxCharsPerLine = 80;
        const CaptionTrack track = buildCaptions(SequenceTranscript::build(tl.state, tl.seq, *p), o);
        REQUIRE(track.cues.size() == 4);
        for (const CaptionCue& c : track.cues) {
            CHECK(c.duration <= Ticks::fromSeconds(1));
        }
    }
    SECTION("Thai cues") {
        const auto p = provider(tl, {{'A', transcript("th", {{"ภาษาไทย", 0, 10},
                                                             {"เขียน", 10, 15},
                                                             {"ติดกัน", 15, 22},
                                                             {"โดย", 22, 25},
                                                             {"ไม่", 25, 28},
                                                             {"เว้น", 28, 32},
                                                             {"วรรค", 32, 40}})}});
        CaptionOptions o;
        o.maxCharsPerLine = 16;
        const CaptionTrack track = buildCaptions(SequenceTranscript::build(tl.state, tl.seq, *p), o);
        CHECK(track.language == "th");
        CHECK(cues(track) == "0-40 ภาษาไทยเขียนติดกัน/โดยไม่เว้นวรรค");
    }
}

TEST_CASE("SRT and WebVTT keep the cue's lines", "[ai][edit][captions]") {
    CaptionTrack track;
    track.cues.push_back(CaptionCue{Ticks{}, f(20), "Hello there."});
    track.cues.push_back(CaptionCue{f(70), f(20), "How are\nyou <today>?"});
    track.cues.push_back(CaptionCue{f(100), f(5), "\n  \n"}); // nothing to show: skipped
    CHECK(toSrt(track) == "1\n00:00:00,000 --> 00:00:00,800\nHello there.\n\n"
                          "2\n00:00:02,800 --> 00:00:03,600\nHow are\nyou <today>?\n\n");
    CHECK(toVtt(track) == "WEBVTT\n\n00:00:00.000 --> 00:00:00.800\nHello there.\n\n"
                          "00:00:02.800 --> 00:00:03.600\nHow are\nyou &lt;today&gt;?\n\n");
}
