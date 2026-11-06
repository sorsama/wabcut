// Transcript model: validation, JSON round trip, schema versioning, captions.

#include <wabcut/ai/Captions.h>
#include <wabcut/ai/TranscriptJson.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

using namespace wabcut;
using namespace wabcut::ai;

namespace {

Ticks ms(std::int64_t v) { return Ticks{v * (kTicksPerSecond / 1000)}; }

Transcript sample() {
    Transcript t;
    t.language = "en";
    t.source = {0xFEDCBA9876543210ULL, 1, "C:/media/ïnterview.mov"};
    t.provenance = {"whisper.cpp", "1.9.5", "base", "60ed5bc3dd14", {{"language", "auto"}, {"beamSize", "5"}}, "2026-10-11T08:30:00Z"};
    t.words = {
        {"Hello,", ms(1250), ms(1600), 0.93f, 0, false, false},
        {"world.", ms(1600), ms(2100), 0.81f, 0, true, false},
        {"สวัสดี", ms(3000), ms(3400), 0.5f, std::nullopt, false, true},
    };
    t.segments = {
        {ms(1250), ms(2100), 0, 2, "Hello, world.", 0},
        {ms(3000), ms(3400), 2, 1, "สวัสดี", std::nullopt},
    };
    return t;
}

} // namespace

TEST_CASE("A transcript survives a JSON round trip exactly", "[ai][transcript]") {
    const Transcript t = sample();
    REQUIRE(t.validate());
    for (const int indent : {-1, 2}) {
        const std::string json = toJson(t, indent);
        auto back = transcriptFromJson(json);
        REQUIRE(back);
        CHECK(*back == t);
        CHECK(toJson(*back, indent) == json); // stable output
    }
}

TEST_CASE("Transcript JSON keeps 64-bit fingerprints and tick values exact", "[ai][transcript]") {
    Transcript t = sample();
    t.words[0].t0 = Ticks{9'007'199'254'740'993}; // 2^53 + 1: not representable as a double
    t.words[0].t1 = t.words[0].t0;
    t.words[1].t0 = t.words[0].t0;
    t.words[1].t1 = t.words[0].t0;
    t.segments[0].t0 = t.words[0].t0;
    t.segments[0].t1 = t.words[0].t0;
    t.words[2].t0 = t.words[0].t0;
    t.words[2].t1 = t.words[0].t0;
    t.segments[1].t0 = t.words[0].t0;
    t.segments[1].t1 = t.words[0].t0;
    auto back = transcriptFromJson(toJson(t));
    REQUIRE(back);
    CHECK(back->words[0].t0.value == 9'007'199'254'740'993);
    CHECK(back->source.fingerprint == 0xFEDCBA9876543210ULL);
    const auto doc = nlohmann::json::parse(toJson(t));
    CHECK(doc["source"]["fingerprint"] == "fedcba9876543210");
    CHECK(doc["schema"] == "wabcut.transcript");
    CHECK(doc["version"] == 1);
    CHECK(doc["ticksPerSecond"] == kTicksPerSecond);
}

TEST_CASE("Transcript JSON rejects other versions and broken documents", "[ai][transcript]") {
    auto doc = nlohmann::json::parse(toJson(sample()));
    SECTION("newer version") {
        doc["version"] = 2;
        const auto r = transcriptFromJson(doc.dump());
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::VersionMismatch);
    }
    SECTION("another schema") {
        doc["schema"] = "something.else";
        CHECK(transcriptFromJson(doc.dump()).error().code == ErrorCode::VersionMismatch);
    }
    SECTION("another tick rate") {
        doc["ticksPerSecond"] = 1000;
        CHECK(transcriptFromJson(doc.dump()).error().code == ErrorCode::VersionMismatch);
    }
    SECTION("missing member") {
        doc["words"][0].erase("t0");
        CHECK(transcriptFromJson(doc.dump()).error().code == ErrorCode::ParseError);
    }
    SECTION("wrong type") {
        doc["words"][0]["t0"] = "soon";
        CHECK(transcriptFromJson(doc.dump()).error().code == ErrorCode::ParseError);
    }
    SECTION("not JSON") {
        CHECK(transcriptFromJson("{not json").error().code == ErrorCode::ParseError);
        CHECK(transcriptFromJson("[1,2]").error().code == ErrorCode::ParseError);
    }
    SECTION("structurally invalid") {
        doc["segments"][1]["firstWord"] = 1; // overlaps the first segment's words
        CHECK(transcriptFromJson(doc.dump()).error().code == ErrorCode::InvalidArgument);
    }
    SECTION("unknown members are ignored") {
        doc["future"] = {{"x", 1}};
        doc["words"][0]["emphasis"] = true;
        auto r = transcriptFromJson(doc.dump());
        REQUIRE(r);
        CHECK(*r == sample());
    }
}

TEST_CASE("Transcript validation catches ordering, range and encoding errors", "[ai][transcript]") {
    CHECK(Transcript{}.validate());
    Transcript t = sample();
    SECTION("word ends before it starts") {
        t.words[1].t1 = t.words[1].t0 - Ticks{1};
        CHECK_FALSE(t.validate());
    }
    SECTION("words out of order") {
        t.words[2].t0 = ms(100);
        CHECK_FALSE(t.validate());
    }
    SECTION("segment past the words") {
        t.segments[1].wordCount = 5;
        CHECK_FALSE(t.validate());
    }
    SECTION("words after the last segment") {
        t.words.push_back({"extra", ms(5000), ms(5100), 0.5f, std::nullopt, false, false});
        CHECK_FALSE(t.validate());
    }
    SECTION("invalid UTF-8") {
        t.words[0].text = "\xC3";
        CHECK_FALSE(t.validate());
    }
    SECTION("confidence out of range") {
        t.words[0].confidence = 1.5f;
        CHECK_FALSE(t.validate());
    }
}

TEST_CASE("Captions: SubRip and WebVTT from transcript segments", "[ai][transcript][captions]") {
    Transcript t = sample();
    t.words[1].text = "<world> & co."; // edited: captions follow the words, and VTT escapes markup
    CHECK(toSrt(t) == "1\n00:00:01,250 --> 00:00:02,100\nHello, <world> & co.\n\n"
                      "2\n00:00:03,000 --> 00:00:03,400\nสวัสดี\n\n");
    CHECK(toVtt(t) == "WEBVTT\n\n00:00:01.250 --> 00:00:02.100\nHello, &lt;world&gt; &amp; co.\n\n"
                      "00:00:03.000 --> 00:00:03.400\nสวัสดี\n\n");

    SECTION("hours, rounding and empty segments") {
        Transcript long_;
        long_.language = "en";
        long_.words = {{"late", Ticks::fromSeconds(3725) + Ticks{kTicksPerSecond / 2000 * 3}, Ticks::fromSeconds(3726), 1.0f, std::nullopt, false, false}};
        long_.segments = {{long_.words[0].t0, long_.words[0].t1, 0, 1, "late", std::nullopt}, {Ticks::fromSeconds(3727), Ticks::fromSeconds(3728), 1, 0, "", std::nullopt}};
        CHECK(toSrt(long_) == "1\n01:02:05,002 --> 01:02:06,000\nlate\n\n"); // 1.5 ms rounds to 2 ms
    }
    SECTION("languages without spaces join words without spaces, except Latin words") {
        Transcript th;
        th.language = "th";
        th.words = {{"ใช้", ms(0), ms(100), 1.0f, std::nullopt, false, false},
                    {"iPhone", ms(100), ms(200), 1.0f, std::nullopt, false, false},
                    {"15", ms(200), ms(300), 1.0f, std::nullopt, false, false},
                    {"ถ่าย", ms(300), ms(400), 1.0f, std::nullopt, false, false}};
        th.segments = {{ms(0), ms(400), 0, 4, "", std::nullopt}};
        CHECK(segmentWordsText(th, th.segments[0]) == "ใช้iPhone 15ถ่าย");
    }
}

// ---- 0.7 (card 111): segments that name their own language -------------------------------------------------------------

TEST_CASE("A transcript of mixed languages names the language of each segment and keeps it in JSON", "[ai][transcript][mixed]") {
    Transcript t = sample();
    // The sample has "Hello, world." (English) and "สวัสดี" (Thai): a transcript made as one language names none.
    CHECK(languageShares(t) == std::vector<LanguageShare>{{"en", 3}});
    CHECK(t.wordLanguage(2) == "en");
    CHECK(toJson(t).find("\"language\":\"th\"") == std::string::npos);

    t.segments[1].language = "th";
    REQUIRE(t.validate());
    CHECK(t.segmentLanguage(0) == "en"); // no language of its own: the transcript's
    CHECK(t.segmentLanguage(1) == "th");
    CHECK(t.wordLanguage(0) == "en");
    CHECK(t.wordLanguage(1) == "en");
    CHECK(t.wordLanguage(2) == "th");
    CHECK(t.wordLanguage(99) == "th"); // past the end: the last segment's
    CHECK(languageShares(t) == std::vector<LanguageShare>{{"en", 2}, {"th", 1}});

    // Round trip: the language of the segment that names one comes back; the other still names none.
    for (const int indent : {-1, 2}) {
        const std::string json = toJson(t, indent);
        auto back = transcriptFromJson(json);
        REQUIRE(back);
        CHECK(*back == t);
        CHECK(back->segments[0].language.empty());
        CHECK(back->segments[1].language == "th");
    }
    // An older build's transcript (no segment names a language) reads as one language.
    nlohmann::json doc = nlohmann::json::parse(toJson(t));
    for (auto& segment : doc["segments"]) {
        segment.erase("language");
    }
    auto old = transcriptFromJson(doc.dump());
    REQUIRE(old);
    CHECK(old->segments[1].language.empty());
    CHECK(old->wordLanguage(2) == "en");

    // Ties go by code; an empty transcript has no shares.
    Transcript tie;
    tie.language = "th";
    tie.words = {{"a", ms(0), ms(100), 0.9f, std::nullopt, false, false}, {"b", ms(100), ms(200), 0.9f, std::nullopt, false, false}};
    tie.segments = {{ms(0), ms(100), 0, 1, "a", std::nullopt, "th"}, {ms(100), ms(200), 1, 1, "b", std::nullopt, "en"}};
    CHECK(languageShares(tie) == std::vector<LanguageShare>{{"en", 1}, {"th", 1}});
    CHECK(languageShares(Transcript{}).empty());

    Transcript bad = t;
    bad.segments[1].language = "\xFF\xFE"; // not UTF-8
    CHECK_FALSE(bad.validate());
}

TEST_CASE("Captions of a transcript with mixed languages join each segment the way its language writes", "[ai][transcript][captions][mixed]") {
    Transcript t = sample();
    t.words = {{"Hello", ms(1000), ms(1500), 0.9f, std::nullopt, false, false},
               {"world", ms(1500), ms(2000), 0.9f, std::nullopt, false, false},
               {"ส", ms(3000), ms(3200), 0.9f, std::nullopt, false, false},
               {"ว", ms(3200), ms(3400), 0.9f, std::nullopt, false, false}};
    t.segments = {{ms(1000), ms(2000), 0, 2, "Hello world", std::nullopt, ""}, {ms(3000), ms(3400), 2, 2, "สว", std::nullopt, "th"}};
    REQUIRE(t.validate());
    const std::string srt = toSrt(t);
    CHECK(srt.find("Hello world") != std::string::npos); // English: spaced
    CHECK(srt.find("สว\n") != std::string::npos);        // Thai: not spaced, though the transcript's language is English
    CHECK(srt.find("ส ว") == std::string::npos);
}
