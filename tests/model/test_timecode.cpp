// SMPTE timecode: non-drop and drop-frame formatting and parsing at all supported rates.

#include <wabcut/model/Timecode.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using namespace wabcut;

TEST_CASE("Timecode labels count frames at the nominal rate", "[model][timecode]") {
    CHECK(timecodeBase(kRate23_976) == 24);
    CHECK(timecodeBase(kRate24) == 24);
    CHECK(timecodeBase(kRate25) == 25);
    CHECK(timecodeBase(kRate29_97) == 30);
    CHECK(timecodeBase(kRate30) == 30);
    CHECK(timecodeBase(kRate50) == 50);
    CHECK(timecodeBase(kRate59_94) == 60);
    CHECK(timecodeBase(kRate60) == 60);

    CHECK(dropFramesPerMinute(kRate29_97) == 2);
    CHECK(dropFramesPerMinute(kRate59_94) == 4);
    CHECK(dropFramesPerMinute(Rate{120000, 1001}) == 8);
    CHECK_FALSE(supportsDropFrame(kRate23_976));
    CHECK_FALSE(supportsDropFrame(kRate30));
    CHECK_FALSE(supportsDropFrame(kRate25));
}

TEST_CASE("Non-drop-frame timecode", "[model][timecode]") {
    CHECK(framesToTimecode(0, kRate25, false) == "00:00:00:00");
    CHECK(framesToTimecode(24, kRate25, false) == "00:00:00:24");
    CHECK(framesToTimecode(25, kRate25, false) == "00:00:01:00");
    CHECK(framesToTimecode(90'000, kRate25, false) == "01:00:00:00");
    CHECK(framesToTimecode(86'400, kRate23_976, false) == "01:00:00:00"); // counts 24 labels per second
    CHECK(framesToTimecode(1800, kRate29_97, false) == "00:01:00:00");
    CHECK(framesToTimecode(215'999, kRate60, false) == "00:59:59:59");
    CHECK(framesToTimecode(30 * 3600 * 25, kRate30, false) == "25:00:00:00"); // no wrap at 24 h
    CHECK(framesToTimecode(-1, kRate25, false) == "-00:00:00:01");
    // Drop-frame is ignored where it does not exist.
    CHECK(framesToTimecode(1800, kRate30, true) == "00:01:00:00");
    // Rates above 100 fps get a wider frames field.
    CHECK(framesToTimecode(119, Rate{120, 1}, false) == "00:00:00:119");
}

TEST_CASE("Drop-frame timecode at 29.97 matches SMPTE reference values", "[model][timecode][df]") {
    CHECK(framesToTimecode(0, kRate29_97, true) == "00:00:00;00");
    CHECK(framesToTimecode(1799, kRate29_97, true) == "00:00:59;29");
    CHECK(framesToTimecode(1800, kRate29_97, true) == "00:01:00;02"); // ;00 and ;01 are skipped
    CHECK(framesToTimecode(3597, kRate29_97, true) == "00:01:59;29");
    CHECK(framesToTimecode(3598, kRate29_97, true) == "00:02:00;02");
    CHECK(framesToTimecode(17'981, kRate29_97, true) == "00:09:59;29");
    CHECK(framesToTimecode(17'982, kRate29_97, true) == "00:10:00;00"); // every tenth minute keeps ;00
    CHECK(framesToTimecode(17'983, kRate29_97, true) == "00:10:00;01");
    CHECK(framesToTimecode(19'781, kRate29_97, true) == "00:10:59;29");
    CHECK(framesToTimecode(19'782, kRate29_97, true) == "00:11:00;02");
    CHECK(framesToTimecode(107'892, kRate29_97, true) == "01:00:00;00"); // one hour of 29.97
    CHECK(framesToTimecode(2'589'408, kRate29_97, true) == "24:00:00;00");
}

TEST_CASE("Drop-frame timecode at 59.94 skips four labels a minute", "[model][timecode][df]") {
    CHECK(framesToTimecode(3599, kRate59_94, true) == "00:00:59;59");
    CHECK(framesToTimecode(3600, kRate59_94, true) == "00:01:00;04");
    CHECK(framesToTimecode(35'964, kRate59_94, true) == "00:10:00;00");
    CHECK(framesToTimecode(215'784, kRate59_94, true) == "01:00:00;00");
}

TEST_CASE("Tick positions format as the frame that contains them", "[model][timecode]") {
    const Ticks frame1800 = framesToTicks(1800, kRate29_97);
    CHECK(ticksToTimecode(frame1800, kRate29_97, true) == "00:01:00;02");
    CHECK(ticksToTimecode(frame1800 + Ticks{1}, kRate29_97, true) == "00:01:00;02");
    CHECK(ticksToTimecode(frame1800 - Ticks{1}, kRate29_97, true) == "00:00:59;29");
    CHECK(ticksToTimecode(Ticks::fromSeconds(3600), kRate25, false) == "01:00:00:00");
}

TEST_CASE("Parsing drop-frame timecode", "[model][timecode][df]") {
    CHECK(parseTimecodeFrames("00:01:00;02", kRate29_97, true) == 1800);
    CHECK(parseTimecodeFrames("00:10:00;00", kRate29_97, true) == 17'982);
    CHECK(parseTimecodeFrames("01:00:00;00", kRate29_97, true) == 107'892);
    CHECK(parseTimecodeFrames("00:01:00;04", kRate59_94, true) == 3600);
    CHECK(parseTimecode("00:01:00;02", kRate29_97, true) == framesToTicks(1800, kRate29_97));

    // Labels that drop-frame skips do not exist.
    const auto skipped = parseTimecodeFrames("00:01:00;00", kRate29_97, true);
    REQUIRE_FALSE(skipped);
    CHECK(skipped.error().code == ErrorCode::ParseError);
    CHECK_FALSE(parseTimecodeFrames("00:01:00;01", kRate29_97, true));
    CHECK_FALSE(parseTimecodeFrames("00:01:00;03", kRate59_94, true));
    CHECK(parseTimecodeFrames("00:01:00;00", kRate29_97, false) == 1800); // fine in non-drop
}

TEST_CASE("Every frame survives a format/parse round trip", "[model][timecode][df]") {
    for (const Rate rate : {kRate23_976, kRate25, kRate29_97, kRate59_94, kRate60}) {
        for (const bool dropFrame : {false, true}) {
            for (std::int64_t frame = 0; frame < 40'000; ++frame) {
                const auto parsed = parseTimecodeFrames(framesToTimecode(frame, rate, dropFrame), rate, dropFrame);
                if (!parsed || *parsed != frame) {
                    FAIL("frame " << frame << " at " << rate.num << "/" << rate.den << (dropFrame ? " DF" : ""));
                }
            }
        }
    }
    CHECK(parseTimecodeFrames(framesToTimecode(-1234, kRate29_97, true), kRate29_97, true) == -1234);
}

TEST_CASE("Parsing accepts Premiere's shorthand", "[model][timecode]") {
    CHECK(parseTimecodeFrames("1:02:03.04", kRate25, false) == ((3600 + 2 * 60 + 3) * 25 + 4));
    CHECK(parseTimecodeFrames("2:10", kRate25, false) == 60);           // SS:FF
    CHECK(parseTimecodeFrames("110", kRate25, false) == 35);            // packed digits: 1 s 10 f
    CHECK(parseTimecodeFrames("1000", kRate25, false) == 250);          // 10 s
    CHECK(parseTimecodeFrames("10002", kRate29_97, true) == 1800);      // 1:00;02 drop-frame
    CHECK(parseTimecodeFrames("90:00", kRate25, false) == 90 * 25);     // the leading field may overflow
    CHECK(parseTimecodeFrames("  -00:00:01:00 ", kRate25, false) == -25);
    CHECK(parseTimecodeFrames("+5", kRate25, false) == 5);
}

TEST_CASE("Parsing rejects malformed timecode", "[model][timecode]") {
    for (const char* bad : {"", "-", "abc", "00:00:00:30", "00:60:00:00", "00:00:60:00", "1:2:3:4:5", "00::00",
                            "12:34:56:7x", "1234567890123:00"}) {
        INFO(bad);
        const auto r = parseTimecodeFrames(bad, kRate30, false);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::ParseError);
    }
    CHECK_FALSE(parseTimecodeFrames("00:00:00:00", Rate{0, 1}, false));
}
