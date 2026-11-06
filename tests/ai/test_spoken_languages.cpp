// Card 073: the languages a recording can be spoken in (wabcut/ai/SpokenLanguages.h): the list the Transcript tab's
// picker, transcript.transcribe and the CLI share - the card's languages are all there with their own names, the codes are
// normalised, right-to-left scripts are flagged, and the advice names a model size that suits the language.

#include <wabcut/ai/SpokenLanguages.h>

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

using namespace wabcut::ai;

TEST_CASE("The spoken languages include every language the card names once each with their own names", "[ai][language]") {
    std::set<std::string> codes;
    for (const SpokenLanguage& l : spokenLanguages()) {
        INFO(l.code);
        CHECK(codes.insert(std::string(l.code)).second); // no duplicates
        CHECK_FALSE(l.name.empty());
        CHECK_FALSE(l.nativeName.empty());
        CHECK((l.code.size() == 2 || l.code.size() == 3));
    }
    CHECK(codes.size() >= 99); // whisper's 99 languages and Cantonese
    // Indonesian, Hebrew, Ukrainian, Arabic, Polish, Turkish, Icelandic, Finnish, Czech, Slovak, Romanian, Bosnian, Croatian,
    // Serbian: the languages of the card (Adobe's feature requests).
    for (const char* code : {"id", "he", "uk", "ar", "pl", "tr", "is", "fi", "cs", "sk", "ro", "bs", "hr", "sr"}) {
        INFO(code);
        const SpokenLanguage* l = findSpokenLanguage(code);
        REQUIRE(l != nullptr);
        CHECK(isSpokenLanguageChoice(code));
    }
    CHECK(findSpokenLanguage("he")->name == "Hebrew");
    CHECK(findSpokenLanguage("he")->nativeName == "\xD7\xA2\xD7\x91\xD7\xA8\xD7\x99\xD7\xAA");
    CHECK(findSpokenLanguage("is")->nativeName == "\xC3\x8D" "slenska");
    CHECK(findSpokenLanguage("uk")->nativeName == "\xD0\xA3\xD0\xBA\xD1\x80\xD0\xB0\xD1\x97\xD0\xBD\xD1\x81\xD1\x8C\xD0\xBA\xD0\xB0");
}

TEST_CASE("Spoken language codes are normalised and unknown ones are refused", "[ai][language]") {
    CHECK(spokenLanguageCode("auto") == "auto");
    CHECK(spokenLanguageCode("he") == "he");
    CHECK(spokenLanguageCode("HE") == "he");
    CHECK(spokenLanguageCode("he-IL") == "he");
    CHECK(spokenLanguageCode("pt_BR") == "pt");
    CHECK(spokenLanguageCode("zh-Hant") == "zh");
    CHECK(spokenLanguageCode("iw") == "he");   // the old code for Hebrew
    CHECK(spokenLanguageCode("nb") == "no");   // Norwegian Bokmal
    CHECK(spokenLanguageCode("fil") == "tl");  // Filipino
    CHECK(spokenLanguageCode("yue") == "yue"); // three letters: Cantonese
    CHECK(spokenLanguageCode("klingon").empty());
    CHECK(spokenLanguageCode("").empty());
    CHECK(spokenLanguageCode("xx").empty());
    CHECK(isSpokenLanguageChoice("auto"));
    CHECK(isSpokenLanguageChoice("is"));
    CHECK_FALSE(isSpokenLanguageChoice("klingon"));
    CHECK_FALSE(isSpokenLanguageChoice("AUTO")); // "auto" is exact: it is not a language
    CHECK(findSpokenLanguage("auto") == nullptr);
}

TEST_CASE("Right-to-left languages are flagged", "[ai][language]") {
    for (const char* code : {"ar", "he", "fa", "ur", "yi", "ps", "sd"}) {
        INFO(code);
        CHECK(isRightToLeftLanguage(code));
    }
    for (const char* code : {"en", "th", "id", "uk", "tr", "auto", "klingon", ""}) {
        INFO(code);
        CHECK_FALSE(isRightToLeftLanguage(code));
    }
    CHECK(isRightToLeftLanguage("ar-EG"));
    CHECK(isRightToLeftLanguage("iw"));
}

TEST_CASE("The advice for a language names a model size that suits it", "[ai][language]") {
    // Nothing to say about auto-detect or an unknown code.
    CHECK(spokenLanguageAdvice("auto", "base").empty());
    CHECK(spokenLanguageAdvice("klingon", "base").empty());
    // A strong language is fine from base up; the tiny model is only for tests.
    CHECK(spokenLanguageAdvice("pl", "base").empty());
    CHECK(spokenLanguageAdvice("pl", "large-v3-turbo-q5_0").empty());
    CHECK(spokenLanguageAdvice("pl", "tiny").find("tiny") != std::string::npos);
    // A fair one wants small or larger.
    CHECK(spokenLanguageAdvice("cs", "base").find("Small") != std::string::npos);
    CHECK(spokenLanguageAdvice("cs", "small").empty());
    CHECK(spokenLanguageAdvice("ro", "").empty()); // the automatic model
    // A limited one: medium or large-v3 for the small models, a "read it through" for any other.
    const std::string icelandicBase = spokenLanguageAdvice("is", "base");
    CHECK(icelandicBase.find("Icelandic") != std::string::npos);
    CHECK(icelandicBase.find("Medium") != std::string::npos);
    CHECK(spokenLanguageAdvice("is", "large-v3-q5_0").find("read the result through") != std::string::npos);
    CHECK(spokenLanguageAdvice("sk", "small").find("Medium") != std::string::npos);
    // An English-only model cannot do another language.
    CHECK(spokenLanguageAdvice("he", "base.en").find("only knows English") != std::string::npos);
    CHECK(spokenLanguageAdvice("en", "base.en").empty());
}

TEST_CASE("Translating to English: English-only models cannot and turbo and tiny are advised against", "[ai][language]") {
    CHECK(modelCanTranslate("base"));
    CHECK(modelCanTranslate("small"));
    CHECK(modelCanTranslate("large-v3-turbo-q5_0"));
    CHECK(modelCanTranslate("")); // the automatic choice
    CHECK_FALSE(modelCanTranslate("base.en"));
    CHECK_FALSE(modelCanTranslate("small.en"));
    CHECK(translationAdvice("small").empty());
    CHECK(translationAdvice("medium-q5_0").empty());
    CHECK(translationAdvice("large-v3-q5_0").empty());
    CHECK(translationAdvice("large-v3-turbo-q5_0").find("translates poorly") != std::string::npos);
    CHECK(translationAdvice("large-v3-turbo-f16").find("Small, Medium or Large v3") != std::string::npos);
    CHECK(translationAdvice("base").find("roughly") != std::string::npos);
    CHECK(translationAdvice("base.en").find("English-only") != std::string::npos);
}

TEST_CASE("Mixed languages: the advice names a bigger model for the small ones and says it takes longer", "[ai][language][mixed]") {
    CHECK(mixedLanguagesAdvice("base.en").find("English-only") != std::string::npos);
    CHECK(mixedLanguagesAdvice("tiny").find("Small or larger") != std::string::npos);
    CHECK(mixedLanguagesAdvice("base").find("Small or larger") != std::string::npos);
    for (const char* model : {"small", "medium-q5_0", "large-v3-q5_0", "large-v3-turbo-q5_0", ""}) {
        INFO(model);
        CHECK(mixedLanguagesAdvice(model).find("takes longer") != std::string::npos);
        CHECK(mixedLanguagesAdvice(model).find("Small or larger") == std::string::npos);
    }
}
