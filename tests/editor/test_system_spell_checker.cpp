// 0.2 graphics: the system spell checker (SystemSpellChecker.h): Windows' spell checking API, from any thread.

#include <wabcut/editor/SystemSpellChecker.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <thread>

using namespace wabcut;

TEST_CASE("The system spell checker finds misspellings with byte ranges and suggests corrections", "[editor][spelling]") {
    const auto checker = editor::systemSpellChecker();
    REQUIRE(checker);
#ifdef _WIN32
    if (!checker->supports("en-US")) {
        SKIP("no English dictionary on this machine");
    }
    CHECK(checker->supports("en")); // ISO 639-1 picks an installed English
    const std::string text = "The \xC3\xA9t\xC3\xA9 quick brwon fox";
    const auto issues = checker->check(text, "en-US");
    const auto brwon = std::find_if(issues.begin(), issues.end(), [](const SpellIssue& i) { return i.word == "brwon"; });
    REQUIRE(brwon != issues.end());
    CHECK(text.substr(brwon->start, brwon->end - brwon->start) == "brwon"); // UTF-8 byte offsets after two-byte letters
    const auto suggestions = checker->suggest("brwon", "en-US");
    CHECK(std::find(suggestions.begin(), suggestions.end(), "brown") != suggestions.end());
    CHECK(suggestions.size() <= kMaxSpellSuggestions);
    CHECK(checker->check("The quick brown fox", "en-US").empty());
    // A repeated word is the checker's Delete.
    const auto repeated = checker->check("It is the the end", "en-US");
    CHECK(std::any_of(repeated.begin(), repeated.end(), [](const SpellIssue& i) { return i.action == SpellAction::Delete; }));
    // Another thread gets its own COM objects.
    std::size_t found = 0;
    std::thread worker([&] { found = checker->check("teh", "en-US").size(); });
    worker.join();
    CHECK(found == 1);
    // A language with no dictionary checks nothing.
    CHECK_FALSE(checker->supports("zz-ZZ"));
    CHECK(checker->check("qwxz", "zz-ZZ").empty());
    CHECK_FALSE(editor::systemSpellingLanguages().empty());
#else
    CHECK_FALSE(checker->supports("en-US"));
    CHECK(checker->check("teh", "en-US").empty());
#endif
}
