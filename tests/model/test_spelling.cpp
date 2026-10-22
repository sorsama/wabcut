// 0.2 graphics: spell check (Spelling.h) and Find and Replace's Unicode case folding and Replace (TextFind.h).

#include <wabcut/model/Graphics.h>
#include <wabcut/model/Sequence.h>
#include <wabcut/model/Spelling.h>
#include <wabcut/model/TextFind.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>

using namespace wabcut;

namespace {

std::vector<std::string> wordsOf(std::string_view text) {
    std::vector<std::string> out;
    for (const auto& [b, e] : spellWords(text)) {
        out.emplace_back(text.substr(b, e - b));
    }
    return out;
}

std::shared_ptr<const SpellChecker> english() {
    return std::make_shared<WordListSpellChecker>(
        std::vector<std::string>{"the", "quick", "brown", "fox", "don't", "colour", "caption", "Wabcut", "is", "a", "editor", "in"},
        std::vector<std::string>{"en"});
}

} // namespace

TEST_CASE("Find and Replace folds case across scripts without Match Case", "[model][textfind]") {
    CHECK(foldCase(U'A') == U'a');
    CHECK(foldCase(U'É') == U'é');   // É
    CHECK(foldCase(U'Σ') == U'σ');   // Σ
    CHECK(foldCase(U'ς') == U'σ');   // final sigma folds like sigma
    CHECK(foldCase(U'Ж') == U'ж');   // Ж
    CHECK(foldCase(U'ก') == U'ก');   // Thai has no case
    CHECK(foldCase(std::string("\xC3\x89""COLE")) == "\xC3\xA9""cole");

    // "ÉCOLE" finds "école" (two-byte letters: the byte ranges are the text's own).
    const std::string text = "L'\xC3\xA9""cole et l'\xC3\x89""COLE";
    const auto found = findText(text, "\xC3\x89""cole");
    REQUIRE(found.size() == 2);
    CHECK(text.substr(found[0].first, found[0].second - found[0].first) == "\xC3\xA9""cole");
    CHECK(text.substr(found[1].first, found[1].second - found[1].first) == "\xC3\x89""COLE");
    CHECK(findText(text, "\xC3\x89""cole", {.matchCase = true}).empty());
    // Greek, with the final sigma.
    CHECK(findText("\xCE\xA3\xCE\x9F\xCE\xA6\xCE\x9F\xCE\xA3", "\xCF\x83\xCE\xBF\xCF\x86\xCE\xBF\xCF\x82").size() == 1);
    // Bytes that are not UTF-8 match only themselves.
    CHECK(findText("a\xFF" "b", "\xFF").size() == 1);
}

TEST_CASE("Whole Word treats punctuation of every script as a boundary", "[model][textfind]") {
    // A Thai word between spaces is whole; inside an unspaced run it is not.
    const std::string thai = "\xE0\xB8\x81\xE0\xB8\xB2\xE0\xB8\xA3"; // การ
    CHECK(findText("x " + thai + " y", thai, {.wholeWord = true}).size() == 1);
    CHECK(findText("\xE0\xB8\x97\xE0\xB8\xB3" + thai, thai, {.wholeWord = true}).empty());
    // Curly quotes and em dashes (General Punctuation) are not word characters.
    CHECK(findText("\xE2\x80\x9C" "fox" "\xE2\x80\x9D", "fox", {.wholeWord = true}).size() == 1);
    CHECK(findText("fox\xE2\x80\x94" "den", "fox", {.wholeWord = true}).size() == 1);
    CHECK(findText("foxes", "fox", {.wholeWord = true}).empty());
}

TEST_CASE("Replace changes only the match at the given byte", "[model][textfind]") {
    const std::string text = "red, Red and RED";
    const auto matches = findText(text, "red");
    REQUIRE(matches.size() == 3);
    const auto once = replaceTextAt(text, "red", "blue", matches[1].first);
    REQUIRE(once);
    CHECK(*once == "red, blue and RED");
    CHECK_FALSE(replaceTextAt(text, "red", "blue", 1)); // no match starts there
    CHECK_FALSE(replaceTextAt(text, "red", "blue", matches[1].first, {.matchCase = true}));
    std::size_t n = 0;
    CHECK(replaceText(text, "RED", "x", {}, &n) == "x, x and x");
    CHECK(n == 3);
}

TEST_CASE("Spell check words: letters with inner apostrophes, not numbers", "[model][spelling]") {
    CHECK(wordsOf("Don't stop, rock'n'roll! 2026 is 4K") == std::vector<std::string>{"Don't", "stop", "rock'n'roll", "is", "4K"});
    CHECK(wordsOf("it\xE2\x80\x99s 'quoted'") == std::vector<std::string>{"it\xE2\x80\x99s", "quoted"});
    CHECK(wordsOf("snake_case") == std::vector<std::string>{"snake", "case"});
}

TEST_CASE("A word-list checker finds misspellings and suggests near words", "[model][spelling]") {
    const auto checker = english();
    const auto issues = checker->check("Teh quick browm fox", "en-US");
    REQUIRE(issues.size() == 2);
    CHECK(issues[0].word == "Teh");
    CHECK(issues[0].start == 0);
    CHECK(issues[0].end == 3);
    CHECK(issues[1].word == "browm");
    const auto suggestions = checker->suggest("Teh", "en");
    REQUIRE_FALSE(suggestions.empty());
    CHECK(suggestions.front() == "the"); // a swap of two letters is one edit
    CHECK(checker->suggest("browm", "en").front() == "brown");
    // Capitalised words are right when their lower-case form is listed; other languages are not checked.
    CHECK(checker->check("The Quick", "en").empty());
    CHECK_FALSE(checker->supports("th"));
    CHECK(checker->check("Teh", "th").empty());
}

TEST_CASE("Add to Dictionary persists; Ignore All lasts the session", "[model][spelling]") {
    namespace fs = std::filesystem;
    const fs::path file = fs::temp_directory_path() / "wabcut-test-spelling" / "dictionary.txt";
    fs::remove_all(file.parent_path());
    {
        PersonalSpelling spelling(english(), file);
        CHECK(spelling.check("Somchai edits in Wabcut", "en").size() == 2); // Somchai, edits
        CHECK(spelling.addWord("Somchai"));
        spelling.ignoreWord("edits");
        CHECK(spelling.check("Somchai edits in Wabcut", "en").empty());
        CHECK(spelling.check("SOMCHAI", "en").empty());                       // case-insensitive
        CHECK_FALSE(spelling.addWord("two words"));
        CHECK_FALSE(spelling.addWord(""));
        CHECK(spelling.words() == std::vector<std::string>{"Somchai"});
    }
    {
        // A new session: the added word is still right, the ignored one is not.
        PersonalSpelling spelling(english(), file);
        CHECK(spelling.words() == std::vector<std::string>{"Somchai"});
        CHECK(spelling.check("Somchai edits", "en").size() == 1);
        CHECK(spelling.removeWord("somchai"));
        CHECK(spelling.check("Somchai", "en").size() == 1);
    }
    std::ifstream in(file, std::ios::binary);
    const std::string saved((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(saved.empty());
}

TEST_CASE("Misspellings of a caption track and of a graphic's text layers", "[model][spelling]") {
    CaptionTrack track;
    track.language = "en";
    track.cues.push_back(CaptionCue{Ticks{0}, Ticks{kTicksPerSecond}, "the quick fox", {}});
    track.cues.push_back(CaptionCue{Ticks{kTicksPerSecond}, Ticks{kTicksPerSecond}, "a colur captoin", {}});
    const auto checker = english();
    const auto issues = spellingOf(track, *checker, {}, true);
    REQUIRE(issues.size() == 2);
    CHECK(issues[0].cue == 1);
    CHECK(issues[0].issue.word == "colur");
    CHECK(issues[0].issue.suggestions.front() == "colour");
    CHECK(issues[1].issue.word == "captoin");
    // The track's language picks the checker's: Thai is not checked by this one.
    track.language = "th";
    CHECK(spellingOf(track, *checker).empty());
    CHECK(spellingOf(track, *checker, "en").size() == 2);
    // Graphic text layers, groups included.
    Graphic g;
    GraphicLayer text;
    text.id = 1;
    text.kind = LayerKind::Text;
    text.text.runs.push_back(TextRun{"Wabcut is a editr", {}});
    GraphicLayer group;
    group.id = 2;
    group.kind = LayerKind::Group;
    GraphicLayer inner = text;
    inner.id = 3;
    inner.text.runs = {TextRun{"brwn fox", {}}};
    group.children.push_back(inner);
    g.layers = {text, group};
    const auto layerIssues = spellingOf(g, *checker, "en");
    REQUIRE(layerIssues.size() == 2);
    CHECK(layerIssues[0].layer == 1);
    CHECK(layerIssues[0].issue.word == "editr");
    CHECK(layerIssues[1].layer == 3);
    CHECK(layerIssues[1].issue.word == "brwn");
    CHECK(noSpellChecker().check("teh", "en").empty());
}
