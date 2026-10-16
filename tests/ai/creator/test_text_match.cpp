// Text matching for script-driven editing (AI-11): units of spaced and unspaced scripts (Thai without ICU), fuzzy unit
// similarity, the semi-global alignment that finds a script line inside a transcript (fillers free, dropped and added
// words), the word-level diff between takes, and the script parser.

#include <wabcut/ai/roughcut/Script.h>
#include <wabcut/ai/roughcut/TextMatch.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace wabcut;
using namespace wabcut::ai::roughcut;
using Catch::Approx;

namespace {

std::vector<std::string> texts(const std::vector<Unit>& us) {
    std::vector<std::string> out;
    for (const Unit& u : us) {
        out.push_back(u.text);
    }
    return out;
}

/// Transcript units from words, each word one source index; `fillers` marks words as fillers.
std::vector<Unit> spoken(const std::vector<std::string>& words, const std::vector<std::size_t>& fillers = {}) {
    std::vector<Unit> out;
    for (std::size_t i = 0; i < words.size(); ++i) {
        appendUnits(out, words[i], static_cast<std::uint32_t>(i), std::ranges::find(fillers, i) != fillers.end());
    }
    return out;
}

} // namespace

TEST_CASE("Units: words for spaced scripts, character clusters for Thai, no whitespace needed", "[ai][creator][text]") {
    // Contractions are the words they stand for ("we're" = "we are"); possessives and "o'clock" stay one word.
    CHECK(texts(units("Don't STOP, it's 10 o'clock!")) == std::vector<std::string>{"do", "not", "stop", "it", "is", "10", "oclock"});
    CHECK(texts(units("We're sure John's can't")) == std::vector<std::string>{"we", "are", "sure", "johns", "can", "not"});
    CHECK(texts(units("Ten well-known Cafés")) == std::vector<std::string>{"10", "well", "known", "cafés"});
    // Thai: one unit per base character with its vowels and tone marks; spaces or none, the same units.
    const auto thai = texts(units("ที่นี่"));
    CHECK(thai == std::vector<std::string>{"ที่", "นี่"});
    CHECK(texts(units("วันนี้ เรา")) == texts(units("วันนี้เรา")));
    // Thai digits become ASCII, mixed text switches per run, Thai punctuation drops.
    CHECK(texts(units("รุ่น ๒ Pro ๆ")) == std::vector<std::string>{"รุ่", "น", "2", "pro"});
    CHECK(isUnspacedCodePoint(U'ก'));
    CHECK(isUnspacedCodePoint(U'中'));
    CHECK_FALSE(isUnspacedCodePoint(U'a'));
    CHECK(normalizedText("Hello,   World!") == "hello world");
    // Invalid UTF-8 never throws.
    CHECK_NOTHROW(units(std::string("\xFF\xFEok\xE0", 5)));
}

TEST_CASE("Unit similarity is exact, fuzzy for long words, lenient on Thai marks", "[ai][creator][text]") {
    const auto u = [](std::string_view s) { return units(s).front(); };
    CHECK(unitSimilarity(u("editor"), u("editor")) == 1.0);
    CHECK(unitSimilarity(u("colour"), u("color")) >= 0.75);
    CHECK(unitSimilarity(u("launched"), u("launch")) == Approx(0.75)); // a stem
    CHECK(unitSimilarity(u("cat"), u("cut")) == 0.0);                 // short words must be exact
    CHECK(unitSimilarity(u("10"), u("11")) == 0.0);
    CHECK(unitSimilarity(u("ม่"), u("ม้")) == Approx(0.6)); // same consonant, another tone mark
    CHECK(unitSimilarity(u("ก"), u("ข")) == 0.0);
}

TEST_CASE("Alignment finds a line inside a transcript: fillers free, words dropped or added", "[ai][creator][text]") {
    const std::vector<Unit> query = units("today we're looking at the new editor");
    // An exact take inside unrelated speech.
    const auto exact = spoken({"so", "anyway", "today", "we're", "looking", "at", "the", "new", "editor", "and", "more"});
    Alignment a = align(query, exact);
    REQUIRE(a.valid());
    CHECK(a.begin == 2);
    CHECK(a.end == 10); // units: "we're" is two
    CHECK(a.coverage == Approx(1.0));
    CHECK(a.quality() == Approx(1.0));
    // Fillers inside cost nothing; the span keeps them but precision ignores them.
    const auto messy = spoken({"um", "today", "we", "are", "uh", "looking", "at", "the", "new", "editor"}, {0, 4});
    a = align(query, messy);
    REQUIRE(a.valid());
    CHECK(a.begin == 1); // the leading filler is not part of the take
    CHECK(a.end == 10);
    CHECK(a.coverage > 0.8);
    CHECK(a.quality() > 0.75);
    // A dropped word lowers coverage; an unrelated text has no useful alignment.
    a = align(query, spoken({"today", "looking", "at", "the", "editor"}));
    CHECK(a.coverage < 0.8);
    CHECK(a.coverage > 0.5);
    CHECK(align(query, spoken({"bananas", "are", "yellow", "fruit"})).quality() < 0.3);
    // Thai: the script line unsegmented, the transcript in word units with a filler.
    const std::vector<Unit> th = units("วันนี้เราจะมาดูโปรแกรมตัดต่อใหม่");
    const auto thai = spoken({"เอ่อ", "วันนี้", "เรา", "จะ", "มา", "ดู", "โปรแกรม", "ตัดต่อ", "ใหม่", "นะ"}, {0});
    a = align(th, thai);
    REQUIRE(a.valid());
    CHECK(thai[a.begin].source == 1);
    CHECK(thai[a.end - 1].source == 8);
    CHECK(a.quality() > 0.9);
}

TEST_CASE("The diff between two takes lists equal, deleted and inserted runs", "[ai][creator][text]") {
    const auto a = units("we doubled the price last week");
    const auto b = units("we doubled our price this week");
    const auto runs = diffUnits(a, b);
    std::string shape;
    for (const DiffRun& r : runs) {
        shape += r.kind == DiffRun::Kind::Equal ? "=" : r.kind == DiffRun::Kind::Deleted ? "-" : "+";
    }
    CHECK(shape == "=-+=-+=");
    CHECK(diffUnits(a, a).size() == 1);
}

TEST_CASE("Feature bags rank the right sentence first", "[ai][creator][text]") {
    const FeatureBag line = featuresOf(units("the best part is the price"));
    CHECK(cosine(line, featuresOf(units("so the best part is the price"))) > cosine(line, featuresOf(units("the price of the best camera"))));
    CHECK(cosine(line, featuresOf(units("nothing in common here"))) == 0.0);
    CHECK(featureHash("w:price") == featureHash("w:price"));
    CHECK(featureHash("w:price") != featureHash("w:prices"));
}

TEST_CASE("Scripts and outlines become beats", "[ai][creator][script]") {
    const auto script = parseScript(R"(# Launch video
Intro paragraph. It has two sentences here.
## Demo
- **Show** the timeline
* why we raised prices [B-roll: price tag]
1. Wrap up
HOST: Thanks for watching!
[VISUAL: logo sting]
(cut to the product shot)
```
code is ignored
```
สวัสดีครับ วันนี้เราจะมาดูโปรแกรมใหม่
)");
    REQUIRE(script);
    CHECK(script->title == "Launch video");
    REQUIRE(script->beats.size() == 7);
    CHECK(script->beats[0].text == "Intro paragraph.");
    CHECK(script->beats[1].text == "It has two sentences here.");
    CHECK(script->beats[0].section == "Launch video");
    CHECK(script->beats[2].text == "Show the timeline");
    CHECK(script->beats[2].outline);
    CHECK(script->beats[2].section == "Demo");
    CHECK(script->beats[3].text == "why we raised prices");
    CHECK(script->beats[3].broll == std::vector<std::string>{"price tag"});
    CHECK(script->beats[4].text == "Wrap up");
    CHECK(script->beats[5].text == "Thanks for watching!");
    CHECK(script->beats[5].speaker == "HOST");
    CHECK(script->beats[5].broll == std::vector<std::string>{"logo sting", "the product shot"});
    CHECK(script->beats[6].text == "สวัสดีครับ วันนี้เราจะมาดูโปรแกรมใหม่"); // Thai lines stay whole
    CHECK_FALSE(script->beats[6].outline);
    for (std::size_t i = 0; i < script->beats.size(); ++i) {
        CHECK(script->beats[i].index == i);
    }
    // Lines mode keeps prose whole; limits are enforced.
    ScriptOptions lines;
    lines.split = ScriptSplit::Lines;
    CHECK(parseScript("One. Two three four.", lines)->beats.size() == 1);
    CHECK(parseScript("One sentence here. Two three four.")->beats.size() == 2);
    ScriptOptions tiny;
    tiny.maxBeats = 2;
    CHECK_FALSE(parseScript("a\nb\nc", tiny));
    tiny.maxBytes = 4;
    CHECK_FALSE(parseScript("long text", tiny));
    CHECK(parseScript("\n\n   \n")->beats.empty());
    CHECK(parseScript("ไทยล้วน\nภาษาไทย")->language == "th");
    CHECK(splitProse("Version 3.5 is out. It works well!") == std::vector<std::string>{"Version 3.5 is out.", "It works well!"});
    CHECK(splitProse("Okay then we go. Fine.") == std::vector<std::string>{"Okay then we go. Fine."}); // a short fragment joins
}

