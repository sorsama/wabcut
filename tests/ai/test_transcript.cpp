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
