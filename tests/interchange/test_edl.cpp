// CMX3600 EDLs: a drop-frame promo, an Avid-style 25 fps cut, line endings and odd input.

#include "InterchangeTestSupport.h"

#include <wabcut/model/Timecode.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::interchange;
using namespace wabcut::interchange::test;

namespace {
constexpr auto O = ReportOutcome::Carried;
constexpr auto A = ReportOutcome::Approximated;
constexpr auto D = ReportOutcome::Dropped;

/// Frame index of a timecode label (the absolute source position an EDL clip keeps as its source In).
std::int64_t tc(std::string_view label, Rate rate, bool dropFrame) {
    const auto frame = parseTimecodeFrames(label, rate, dropFrame);
    REQUIRE(frame);
    return *frame;
}
} // namespace

TEST_CASE("interchange: EDL, 29.97 drop-frame: events, dissolve, channels, motion, locators",
          "[interchange][edl]") {
    const ImportResult r = importFixture("promo_2997df.edl");
    CHECK(r.format == Format::Edl);
    const Sequence& seq = sequenceAt(r, 0);
    CHECK(seq.name == "Promo_30s_v2");
    CHECK(seq.frameRate == kRate29_97);
    CHECK(seq.dropFrame);
    REQUIRE(seq.videoTracks.size() == 1);
    REQUIRE(seq.audioTracks.size() == 3);
    const auto src = [](std::string_view label) { return std::to_string(tc(label, kRate29_97, true)); };
    // Record 01:00:00;00 is the timeline's 0. The dissolve's incoming clip starts at the cut; BL is a gap.
    CHECK(clips(seq, "V1") == Strings{"0+150 A001_C002.mov @" + src("14:23:10;00"),
                                      "150+120 A002_C010.mov @" + src("09:01:00;02"),
                                      "300+90 A001_C002.mov @" + src("14:24:00;02")});
    // AA is one stereo clip on A1; A and A2 of the same source and times merge the same way.
    CHECK(clips(seq, "A1") == Strings{"0+300 Music_Bed.wav @0", "300+90 A001_C002.mov @" + src("14:24:00;02"),
                                      "390+60 Nats.wav @0"});
    CHECK(clips(seq, "A2") == Strings{"60+180 VO_Take3.wav @60"});
    // Avid's NONE + "AUD 3 4": channels 3 and 4.
    CHECK(clips(seq, "A3") == Strings{"330+60 Ambience.wav @0"});
    CHECK(links(seq) == Strings{"V1@300+A1@300"}); // channel B = video + A1

    REQUIRE(seq.markers.size() == 1);
    CHECK(seq.markers[0].name == "Logo here");
    CHECK(seq.markers[0].start == frames(tc("00:00:07;15", kRate29_97, true), kRate29_97));
    CHECK(seq.markers[0].colorIndex == 1);

    const ImportReport& rep = r.report;
    CHECK(hasEntry(rep, O, ReportCategory::Transition,
                   "'Dissolve' (30 frames, starting at the cut at 00:00:05;00) on V1 -> crossDissolve"));
    // M2 059.9 at 29.97 fps: CMX's one-decimal rate for 200%.
    CHECK(hasEntry(rep, O, ReportCategory::Speed, "'A001_C002.mov' on V1 plays at 200%"));
    CHECK(hasEntry(rep, O, ReportCategory::Speed, "'A001_C002.mov' on A1 plays at 200%"));
    CHECK(hasEntry(rep, A, ReportCategory::AudioMapping,
                   "channels A1+A2 of 'Music_Bed.wav' became one clip on A1"));
    CHECK(hasEntry(rep, A, ReportCategory::AudioMapping,
                   "channels A3+A4 of 'Ambience.wav' became one clip on A3"));
    CHECK(hasEntry(rep, A, ReportCategory::AudioMapping,
                   "A1 and A2 of 'Nats.wav' at 00:00:13;00 became one clip on A1"));
    CHECK(hasEntry(rep, A, ReportCategory::SequenceSetting,
                   "EDLs carry no frame rate; 29.97 fps drop-frame assumed"));
    CHECK(hasEntry(rep, O, ReportCategory::SequenceSetting, "start timecode 01:00:00;00")); // X.1: kept
    CHECK(hasEntry(rep, D, ReportCategory::Effect, "1 line(s) that are not CMX3600 events"));
    CHECK(hasEntry(rep, A, ReportCategory::Media, "'A001_C002.mov' is referenced by source timecode"));
    const ReportEntry& gap = entry(rep, O, ReportCategory::Gap, "gap on V1 at 00:00:09;00 (30 frames)");
    CHECK(gap.location.element == "event 003");
    CHECK(gap.location.line == 12);
    CHECK(entry(rep, O, ReportCategory::Clip, "'A002_C010.mov' on V1").location.line == 8);
}

TEST_CASE("interchange: EDL: the media inspector converts source timecode to media time",
          "[interchange][edl][relink]") {
    const auto root = scratchDir("edl-inspector");
    writeFile(root / "A001_C002.mov", "x");
    ImportOptions options;
    options.mediaSearchRoots = {root};
    // A001_C002.mov starts at timecode 14:23:00;02 (;00 and ;01 do not exist in that minute).
    const Ticks start = frames(tc("14:23:00;02", kRate29_97, true), kRate29_97);
    options.mediaInspector = [&](const std::filesystem::path& p) -> std::optional<MediaFacts> {
        if (p.filename() == "A001_C002.mov") {
            return MediaFacts{std::nullopt, std::nullopt, start};
        }
        return std::nullopt;
    };
    const ImportResult r = importFixture("promo_2997df.edl", options);
    const Sequence& seq = sequenceAt(r, 0);
    const Clip& first = track(seq, "V1").clips[0];
    CHECK(first.sourceIn == frames(298, kRate29_97)); // 14:23:00;02 to 14:23:10;00
    CHECK(hasEntry(r.report, O, ReportCategory::Media, "source timecodes of 'A001_C002.mov' converted"));
}

TEST_CASE("interchange: EDL, Avid at 25 fps: clip paths, wipes and keys", "[interchange][edl]") {
    ImportOptions options;
    options.edlFrameRate = kRate25;
    const ImportResult r = importFixture("avid_25fps.edl", options);
    const Sequence& seq = sequenceAt(r, 0);
    CHECK(seq.name == "SCENE_12_CUT");
    CHECK(seq.frameRate == kRate25);
    CHECK_FALSE(seq.dropFrame);
    const auto src = [](std::string_view label) { return std::to_string(tc(label, kRate25, false)); };
    CHECK(clips(seq, "V1") == Strings{"0+100 12-3 Wide @" + src("10:00:00:00"),
                                      "100+75 12-7 Close @" + src("10:07:00:00"),
                                      "225+50 12-8 Insert @" + src("10:08:00:00")});
    CHECK(clips(seq, "A1") == Strings{"175+50 12-7 Close @" + src("10:07:03:00")});
    CHECK(mediaNamed(r, "12-3 Wide").locator.path == "/Volumes/Avid MediaFiles/12-3_Wide.mxf");
    CHECK(hasEntry(r.report, A, ReportCategory::Transition,
                   "'Wipe 001' (25 frames, starting at the cut at 00:00:04:00)"));
    CHECK(hasEntry(r.report, D, ReportCategory::Effect, "key (foreground 'GFX') in event 0004"));
    CHECK_FALSE(hasEntry(r.report, A, ReportCategory::SequenceSetting, "EDLs carry no frame rate"));
}

TEST_CASE("interchange: EDL: CRLF line endings and timecode-only drop-frame detection",
          "[interchange][edl]") {
    std::string crlf;
    for (const char c : readFixture("promo_2997df.edl")) {
        if (c == '\n') {
            crlf += '\r';
        }
        crlf += c;
    }
    const auto unix = importFixture("promo_2997df.edl");
    const auto dos = importText(crlf, Format::Edl);
    CHECK(clips(sequenceAt(dos, 0), "V1") == clips(sequenceAt(unix, 0), "V1"));
    CHECK(clips(sequenceAt(dos, 0), "A1") == clips(sequenceAt(unix, 0), "A1"));

    // No FCM line: ';' separators mean drop-frame.
    const auto r = importText(
        "001  X        V     C        00:00:01;00 00:00:02;00 00:00:00;00 00:00:01;00\n", Format::Edl);
    CHECK(sequenceAt(r, 0).dropFrame);
    CHECK(sequenceAt(r, 0).frameRate == kRate29_97);
}

TEST_CASE("interchange: EDL: bad events are reported, empty EDLs are errors", "[interchange][edl]") {
    const auto r =
        importText("TITLE: odd\nFCM: NON-DROP FRAME\n"
                   "001  X        Q     C        00:00:00:00 00:00:01:00 00:00:00:00 00:00:01:00\n"
                   "002  X        V     C        00:00:00:00 00:00:01:00 00:00:00:99 00:00:02:00\n"
                   "003  X        V     C        00:00:05:00 00:00:06:00 00:00:02:00 00:00:03:00\n",
                   Format::Edl);
    CHECK(hasEntry(r.report, D, ReportCategory::Clip, "event 001 has an unknown channel field 'Q'"));
    CHECK(hasEntry(r.report, D, ReportCategory::Clip,
                   "event 002 has a timecode that does not exist at 24 fps"));
    CHECK(clips(sequenceAt(r, 0), "V1") == Strings{"48+24 X @120"});

    CHECK(ImportRouter::importData("TITLE: nothing here\n", Format::Edl, "e.edl").error().code ==
          ErrorCode::ParseError);
    CHECK(ImportRouter::importData(std::string("001\0bin", 7), Format::Edl, "e.edl").error().code ==
          ErrorCode::ParseError);
}

TEST_CASE("interchange: EDL: M2 speeds, freezes and reverse become clip speeds", "[interchange][edl][fidelity]") {
    ImportOptions options;
    options.edlFrameRate = kRate25;
    const auto r = importText("TITLE: Motion\nFCM: NON-DROP FRAME\n"
                              "001  A001     V     C        01:00:00:00 01:00:02:00 01:00:00:00 01:00:02:00\n"
                              "M2   A001       000.0                01:00:00:00\n"
                              "002  A002     V     C        01:00:10:00 01:00:12:00 01:00:02:00 01:00:04:00\n"
                              "M2   A002       -025.0               01:00:12:00\n"
                              "003  A003     V     C        01:00:20:00 01:00:24:00 01:00:04:00 01:00:06:00\n"
                              "M2   A003       050.0                01:00:20:00\n",
                              Format::Edl, options);
    const Track& v1 = track(sequenceAt(r, 0), "V1");
    REQUIRE(v1.clips.size() == 3);
    CHECK(v1.clips[0].isFreeze());
    CHECK(v1.clips[1].reversed);
    CHECK(v1.clips[1].speed == Speed{1, 1});
    CHECK(v1.clips[2].speed == Speed{2, 1});
    CHECK(v1.clips[2].duration() == frames(50, kRate25));
    CHECK(hasEntry(r.report, O, ReportCategory::Speed, "'A001' on V1 is a freeze frame (frame hold)"));
    CHECK(hasEntry(r.report, A, ReportCategory::Speed, "'A002' on V1 plays at 100% in reverse"));
    CHECK(hasEntry(r.report, O, ReportCategory::Speed, "'A003' on V1 plays at 200%"));
}
