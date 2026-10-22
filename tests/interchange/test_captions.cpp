// Caption files (P.2): every format written and read back, golden files of each writer byte for byte, readers on
// files as other tools write them (styles, positions, 608 display modes), CEA-608 encoding and decoding, and
// malformed input.

#include "InterchangeTestSupport.h"

#include "CaptionFormats.h"
#include "Cea608.h"

#include <wabcut/interchange/CaptionFiles.h>
#include <wabcut/model/CaptionText.h>
#include <wabcut/model/Timecode.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <random>

using namespace wabcut;
using namespace wabcut::interchange;
namespace it = wabcut::interchange::test;
namespace cea608 = wabcut::interchange::detail::cea608;

namespace {

Ticks ms(std::int64_t v) { return Ticks{v * (kTicksPerSecond / 1000)}; }

CaptionCue cue(std::int64_t startMs, std::int64_t endMs, std::string text) {
    CaptionCue c;
    c.start = ms(startMs);
    c.duration = ms(endMs - startMs);
    c.text = std::move(text);
    return c;
}

/// Cues on whole frames of 25 fps and of 29.97 fps alike (multiples of 40 ms that are also 30000/1001 frames: 1001 ms
/// steps), so every format can carry them exactly.
std::vector<CaptionCue> sampleCues() {
    return {cue(1001, 3003, "Hello there."),
            cue(4004, 7007, "Second caption,\ntwo lines."),
            cue(8008, 10010, "Caf\xC3\xA9 \xE2\x99\xAA \xC2\xBF Qu\xC3\xA9 pas\xC3\xB3?"), // accented letters, a music note, an inverted question mark
            cue(11011, 12012, "<Tags> & \"quotes\"")};
}

std::string describe(const std::vector<CaptionCue>& cues, Rate rate) {
    std::string out;
    for (const CaptionCue& c : cues) {
        out += ticksToTimecode(c.start, rate, false) + "-" + ticksToTimecode(c.end(), rate, false) + " " + c.text + "|";
    }
    return out;
}

CaptionWriteOptions optionsFor(CaptionFileFormat format) {
    CaptionWriteOptions o;
    o.language = "en";
    o.title = "Sample";
    o.creationDate = "2026-10-15";
    o.creationTime = "12:00:00";
    if (format == CaptionFileFormat::EbuStl) {
        o.frameRate = kRate25;
        o.dropFrame = false;
    }
    return o;
}

} // namespace

TEST_CASE("Captions: format names extensions and detection (P.2)", "[interchange][captions]") {
    for (const CaptionFileFormat f : captionFileFormats()) {
        CHECK(captionFormatFromName(toString(f)) == f);
        CHECK_FALSE(displayName(f).empty());
    }
    CHECK(captionFormatFromName("WebVTT") == CaptionFileFormat::WebVtt);
    CHECK(captionFormatFromName("xml") == CaptionFileFormat::Ttml);
    CHECK_FALSE(captionFormatFromName("docx").has_value());
    CHECK(detectCaptionFile("WEBVTT\n\n00:00.000 --> 00:01.000\nhi\n", "x.txt") == CaptionFileFormat::WebVtt);
    CHECK(detectCaptionFile("\xEF\xBB\xBF" "1\n00:00:01,000 --> 00:00:02,000\nhi\n", "a.srt") == CaptionFileFormat::Srt);
    CHECK(detectCaptionFile("Scenarist_SCC V1.0\n", "") == CaptionFileFormat::Scc);
    CHECK(detectCaptionFile("File Format=MacCaption_MCC V1.0\n", "") == CaptionFileFormat::Mcc);
    CHECK(detectCaptionFile("<?xml version=\"1.0\"?>\n<tt xmlns=\"http://www.w3.org/ns/ttml\"></tt>", "a.xml") ==
          CaptionFileFormat::Ttml);
    CHECK(detectCaptionFile("<tt:tt xmlns:tt=\"http://www.w3.org/ns/ttml\"/>", "a.itt") == CaptionFileFormat::Itt);
    CHECK_FALSE(detectCaptionFile("<xmeml version=\"5\"/>", "a.xml").has_value());
    CHECK_FALSE(detectCaptionFile("just text", "notes.txt").has_value());
    CHECK(hasCaptionFileExtension("C:/a/b.SCC"));
    CHECK_FALSE(hasCaptionFileExtension("b.xml"));
}

TEST_CASE("Captions: every format writes and reads its cues back (P.2)", "[interchange][captions]") {
    const auto cues = sampleCues();
    for (const CaptionFileFormat format : captionFileFormats()) {
        INFO(toString(format));
        const CaptionWriteOptions options = optionsFor(format);
        const auto written = writeCaptionData(cues, TimeRange{Ticks{}, Ticks::fromSeconds(60)}, format, options, "out");
        REQUIRE(written.has_value());
        CHECK(written->cues == 4);
        const auto read = readCaptionData(written->data, format, {}, "out");
        REQUIRE(read.has_value());
        const Rate rate = format == CaptionFileFormat::EbuStl ? kRate25 : kRate29_97;
        // 608 has no '<', '>' or '&quot;': those become their closest characters (reported); the rest is exact.
        if (format == CaptionFileFormat::Scc || format == CaptionFileFormat::Mcc) {
            REQUIRE(read->cues.size() == 4);
            for (std::size_t i = 0; i < 3; ++i) {
                CHECK(read->cues[i].text == cues[i].text);
                CHECK(std::llabs(read->cues[i].start.value - cues[i].start.value) <= framesToTicks(1, rate).value);
                CHECK(std::llabs(read->cues[i].end().value - cues[i].end().value) <= framesToTicks(1, rate).value);
            }
            continue;
        }
        if (format == CaptionFileFormat::EbuStl) {
            CHECK(read->language == "en");
            CHECK(read->title == "Sample");
        }
        CHECK(describe(read->cues, rate) == describe(cues, rate));
    }
}

TEST_CASE("Captions: each writer matches its golden file (P.2)", "[interchange][captions]") {
    const auto cues = sampleCues();
    const bool update = std::getenv("WABCUT_UPDATE_GOLDEN") != nullptr;
    for (const CaptionFileFormat format : captionFileFormats()) {
        INFO(toString(format));
        CaptionWriteOptions options = optionsFor(format);
        options.startTimecode = Ticks::fromSeconds(3600);
        const auto written = writeCaptionData(cues, TimeRange{Ticks{}, Ticks::fromSeconds(60)}, format, options, "out");
        REQUIRE(written.has_value());
        const std::string name = "captions/basic-" + std::string(toString(format)) + std::string(extension(format));
        const std::filesystem::path golden = it::fixturePath(name);
        if (update) {
            it::writeFile(golden, written->data);
        }
        const std::string want = std::filesystem::exists(golden) ? it::readFile(golden) : std::string();
        if (written->data != want) {
            it::writeFile(std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "captions" / golden.filename(), written->data);
        }
        CHECK(written->data == want);
    }
}

TEST_CASE("Captions: timecode files subtract the sequence start timecode (P.2)", "[interchange][captions]") {
    const auto cues = sampleCues();
    for (const CaptionFileFormat format : {CaptionFileFormat::Scc, CaptionFileFormat::Mcc, CaptionFileFormat::Itt}) {
        INFO(toString(format));
        CaptionWriteOptions options = optionsFor(format);
        options.startTimecode = Ticks::fromSeconds(3600);
        const auto written = writeCaptionData(cues, TimeRange{Ticks{}, Ticks::fromSeconds(60)}, format, options);
        REQUIRE(written.has_value());
        CaptionReadOptions read;
        read.startTimecode = Ticks::fromSeconds(3600);
        const auto back = readCaptionData(written->data, format, read);
        REQUIRE(back.has_value());
        REQUIRE_FALSE(back->cues.empty());
        CHECK(std::llabs(back->cues.front().start.value - cues.front().start.value) <= framesToTicks(1, kRate29_97).value);
        // A sequence starting at 00:00:00:00 reads the same file from the hour (broadcast files start at 01:00:00:00).
        const auto fromZero = readCaptionData(written->data, format, CaptionReadOptions{});
        REQUIRE(fromZero.has_value());
        CHECK(fromZero->cues.front().start >= Ticks::fromSeconds(3600));
    }
}

TEST_CASE("Captions: SubRip markup and positions are read as plain text and placement (P.2)", "[interchange][captions]") {
    const std::string srt = "1\r\n00:00:01,000 --> 00:00:02,500\r\n<i>Hello</i> <font color=\"#ff0000\">world</font>\r\n\r\n"
                            "2\r\n00:00:03,000 --> 00:00:04,000 X1:100 X2:200 Y1:10 Y2:20\r\n{\\an8}On top\r\n\r\n"
                            "3\r\n00:00:05.000 --> 00:00:04,000\r\nbackwards\r\n";
    const auto read = readCaptionData(srt, CaptionFileFormat::Srt, {}, "in.srt");
    REQUIRE(read.has_value());
    REQUIRE(read->cues.size() == 2);
    CHECK(read->cues[0].text == "Hello world");
    CHECK(read->cues[1].text == "On top");
    CHECK(read->placement == CaptionPlacement::Top);
    CHECK(it::hasEntry(read->report, ReportOutcome::Approximated, ReportCategory::Caption, "markup removed"));
    CHECK(it::hasEntry(read->report, ReportOutcome::Dropped, ReportCategory::Caption, "without duration"));
    // Windows-1252 text is converted.
    const auto latin = readCaptionData("1\n00:00:01,000 --> 00:00:02,000\nCaf\xE9\n", CaptionFileFormat::Srt);
    REQUIRE(latin.has_value());
    CHECK(latin->cues.at(0).text == "Caf\xC3\xA9");
}

TEST_CASE("Captions: WebVTT settings notes styles and entities (P.2)", "[interchange][captions]") {
    const std::string vtt = "WEBVTT - with a title\nKind: captions\n\nNOTE a comment\nspanning lines\n\nSTYLE\n::cue { color: red }\n\n"
                            "intro\n00:01.000 --> 00:02.000 line:0 align:start\n<v Narrator>Fish &amp; chips</v> <b>now</b>\n\n"
                            "00:00:03.000 --> 00:00:04.000\nTwo <00:00:03.500>words &lt;3\n";
    const auto read = readCaptionData(vtt, CaptionFileFormat::WebVtt, {}, "in.vtt");
    REQUIRE(read.has_value());
    REQUIRE(read->cues.size() == 2);
    CHECK(read->cues[0].text == "Fish & chips now");
    CHECK(read->cues[1].text == "Two words <3");
    CHECK(read->alignment == CaptionAlign::Left);
    CHECK(it::hasEntry(read->report, ReportOutcome::Approximated, ReportCategory::Caption, "Voice spans"));
    CHECK_FALSE(readCaptionData("1\n00:00:01,000 --> 00:00:02,000\nhi\n", CaptionFileFormat::WebVtt).has_value());
}

TEST_CASE("Captions: Timed Text regions offsets ticks frames and spans (P.2)", "[interchange][captions]") {
    const std::string ttml = R"(<?xml version="1.0" encoding="UTF-8"?>
<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling" xmlns:ttp="http://www.w3.org/ns/ttml#parameter"
    xml:lang="fr" ttp:frameRate="25" ttp:tickRate="10000000">
  <head>
    <styling><style xml:id="it" tts:fontStyle="italic"/></styling>
    <layout><region xml:id="up" tts:origin="10% 5%" tts:extent="80% 20%" tts:displayAlign="before"/></layout>
  </head>
  <body region="up">
    <div begin="10s">
      <p begin="1s" end="2s">Bonjour <span style="it">tout</span>
         le monde</p>
      <p begin="30000000t" dur="25f">Ligne un<br/>ligne deux</p>
      <p begin="00:00:05:10" end="00:00:06.000">Images</p>
    </div>
  </body>
</tt>)";
    const auto read = readCaptionData(ttml, CaptionFileFormat::Ttml, {}, "in.xml");
    REQUIRE(read.has_value());
    REQUIRE(read->cues.size() == 3);
    CHECK(read->language == "fr");
    CHECK(read->placement == CaptionPlacement::Top);
    CHECK(read->cues[0].start == Ticks::fromSeconds(11));
    CHECK(read->cues[0].end() == Ticks::fromSeconds(12));
    CHECK(read->cues[0].text == "Bonjour tout le monde");
    CHECK(read->cues[1].start == Ticks::fromSeconds(13));
    CHECK(read->cues[1].end() == Ticks::fromSeconds(14));
    CHECK(read->cues[1].text == "Ligne un\nligne deux");
    CHECK(read->cues[2].start == Ticks::fromSeconds(15) + framesToTicks(10, kRate25));
    CHECK(it::hasEntry(read->report, ReportOutcome::Approximated, ReportCategory::Caption, "styles are not kept"));
    CHECK_FALSE(readCaptionData("<xmeml/>", CaptionFileFormat::Ttml).has_value());
    CHECK_FALSE(readCaptionData("<tt><body><p begin=\"x\"", CaptionFileFormat::Ttml).has_value());
}

TEST_CASE("Captions: SCC pop-on roll-up and paint-on captions decode as other tools write them (P.2)", "[interchange][captions]") {
    // Pop-on: ENM, RCL, PAC row 15 indent 4 with a tab offset, "HELLO", a special character, EOC; erased two seconds on.
    const std::string popOn = "Scenarist_SCC V1.0\n\n"
                              "00:00:01;00\t94ae 94ae 9420 9420 94f2 94f2 97a1 97a1 c845 4c4c 4f80 9137 9137 942f 942f\n\n"
                              "00:00:03;00\t942c 942c\n\n";
    const auto read = readCaptionData(popOn, CaptionFileFormat::Scc, {}, "in.scc");
    REQUIRE(read.has_value());
    REQUIRE(read->cues.size() == 1);
    CHECK(read->cues[0].text == "HELLO\xE2\x99\xAA");
    CHECK(read->frameRate == kRate29_97);
    CHECK(read->dropFrame);
    CHECK(ticksToTimecode(read->cues[0].start, kRate29_97, true) == "00:00:01;13"); // EOC is the 13th word
    CHECK(ticksToTimecode(read->cues[0].end(), kRate29_97, true) == "00:00:03;00");
    CHECK(read->placement == CaptionPlacement::Bottom);

    // Roll-up 2: two lines, each a cue from its first character to the carriage return that rolls it up.
    const std::string rollUp = "Scenarist_SCC V1.0\n\n"
                               "00:00:10:00\t9425 9425 94ad 94ad 9470 9470 4fce 45ae\n\n"
                               "00:00:12:00\t94ad 94ad 9470 9470 5457 4f80\n\n"
                               "00:00:14:00\t942c 942c\n\n";
    const auto rolled = readCaptionData(rollUp, CaptionFileFormat::Scc);
    REQUIRE(rolled.has_value());
    REQUIRE(rolled->cues.size() == 2);
    CHECK(rolled->cues[0].text == "ONE.");
    CHECK(rolled->cues[1].text == "TWO");
    CHECK_FALSE(rolled->dropFrame);
    CHECK(it::hasEntry(rolled->report, ReportOutcome::Approximated, ReportCategory::Caption, "Roll-up"));

    // Paint-on: text shows as it is painted, until erased.
    const std::string paint = "Scenarist_SCC V1.0\n\n00:00:20:00\t9429 9429 9140 9140 d061 e96e f480 942c 942c\n";
    const auto painted = readCaptionData(paint, CaptionFileFormat::Scc);
    REQUIRE(painted.has_value());
    REQUIRE(painted->cues.size() == 1);
    CHECK(painted->cues[0].text == "Paint");
    CHECK(painted->placement == CaptionPlacement::Top); // row 1

    CHECK_FALSE(readCaptionData("00:00:01:00\t9420\n", CaptionFileFormat::Scc).has_value());
}

TEST_CASE("Captions: CEA-608 encoding of characters positions and modes (P.2)", "[interchange][captions]") {
    CHECK(cea608::withParity(0x14) == 0x94);
    CHECK(cea608::withParity(0x20) == 0x20);
    CHECK(cea608::withParity(0x2D) == 0xAD);
    CHECK(cea608::parityOk(0x94));
    CHECK_FALSE(cea608::parityOk(0x14));
    // Every character of the tables encodes and decodes to itself.
    for (char32_t ch : {U'A', U'z', U'\u00E1', U'\u00E9', U'\u00F1', U'\u00AE', U'\u266A', U'\u00C9', U'\u00DF', U'\u00C5', U'{'}) {
        const auto e = cea608::encodeCharacter(ch);
        REQUIRE(e.has_value());
        if (e->twoByte) {
            CHECK(cea608::decodeTwoByte(e->first, e->second) == ch);
        } else {
            CHECK(cea608::decodeBasic(e->first) == ch);
        }
    }
    CHECK_FALSE(cea608::encodeCharacter(U'\u0E01').has_value()); // Thai: no 608 code

    // Pop-on encodes back through the decoder: two lines, centred, at the bottom.
    cea608::Cue a;
    a.startFrame = 100;
    a.endFrame = 160;
    a.lines = {"First line", "and \xC3\x89t\xC3\xA9"};
    cea608::Cue b;
    b.startFrame = 160;
    b.endFrame = 200;
    b.lines = {"Next"};
    b.top = true;
    const std::vector<cea608::Cue> cues{a, b};
    for (const cea608::Mode mode : {cea608::Mode::PopOn, cea608::Mode::PaintOn}) {
        const cea608::EncodeResult r = cea608::encode(cues, mode);
        cea608::Decoder d;
        for (const auto& [frame, pair] : r.pairs) {
            d.push(frame, pair.first, pair.second);
        }
        const auto out = d.finish(300);
        REQUIRE(out.size() == 2);
        CHECK(out[0].rows == std::vector<std::string>{"First line", "and \xC3\x89t\xC3\xA9"});
        CHECK(out[0].topRow == 14);
        CHECK(out[1].rows == std::vector<std::string>{"Next"});
        CHECK(out[1].topRow == 1);
        if (mode == cea608::Mode::PopOn) {
            CHECK(out[0].startFrame == 100);
            CHECK(out[0].endFrame == 160);
            CHECK(out[1].endFrame == 200);
            CHECK(r.lateCaptions == 0);
            CHECK(out[0].column == (32 - 10) / 2); // "First line" centred
        }
    }
    // Too much text for the time between two captions: the second shows late, and says so.
    cea608::Cue c;
    c.startFrame = 0;
    c.endFrame = 2;
    c.lines = {"Short"};
    cea608::Cue d2;
    d2.startFrame = 2;
    d2.endFrame = 50;
    d2.lines = {"A much longer caption that needs", "many frames of loading time"};
    const std::vector<cea608::Cue> crowded{c, d2};
    const auto late = cea608::encode(crowded);
    CHECK(late.lateCaptions >= 1);
    CHECK(late.maxLateFrames > 0);
}

TEST_CASE("Captions: EBU STL characters vertical position and justification (P.2)", "[interchange][captions]") {
    CaptionWriteOptions options = optionsFor(CaptionFileFormat::EbuStl);
    options.placement = CaptionPlacement::Top;
    options.alignment = CaptionAlign::Left;
    // Angstrom with a ring and an umlaut, Lodz with a stroke, an acute and a dot above, a euro sign (not in ISO 6937).
    const std::vector<CaptionCue> cues{cue(1000, 2000, "\xC3\x85ngstr\xC3\xB6m \xC5\x81\xC3\xB3" "d\xC5\xBA \xE2\x82\xAC")};
    const auto written = writeCaptionData(cues, TimeRange{Ticks{}, Ticks::fromSeconds(10)}, CaptionFileFormat::EbuStl, options);
    REQUIRE(written.has_value());
    CHECK(written->data.size() == 1024 + 128);
    CHECK(it::hasEntry(written->report, ReportOutcome::Approximated, ReportCategory::Caption, "written as '?'"));
    const auto read = readCaptionData(written->data, CaptionFileFormat::EbuStl);
    REQUIRE(read.has_value());
    REQUIRE(read->cues.size() == 1);
    CHECK(read->cues[0].text == "\xC3\x85ngstr\xC3\xB6m \xC5\x81\xC3\xB3" "d\xC5\xBA ?");
    CHECK(read->placement == CaptionPlacement::Top);
    CHECK(read->alignment == CaptionAlign::Left);
    CHECK(read->frameRate == kRate25);
}

TEST_CASE("Captions: overlapping cues are shortened and identical starts joined (P.2)", "[interchange][captions]") {
    const std::string srt = "1\n00:00:01,000 --> 00:00:05,000\nfirst\n\n2\n00:00:03,000 --> 00:00:06,000\nsecond\n\n"
                            "3\n00:00:03,000 --> 00:00:04,000\nthird\n\n";
    const auto read = readCaptionData(srt, CaptionFileFormat::Srt);
    REQUIRE(read.has_value());
    REQUIRE(read->cues.size() == 2);
    CHECK(read->cues[0].end() == Ticks::fromSeconds(3));
    CHECK(read->cues[1].text == "second\nthird");
    CHECK(read->cues[1].end() == Ticks::fromSeconds(6));
}

TEST_CASE("Captions: the export range cuts and shifts cues (P.2)", "[interchange][captions]") {
    const auto written = writeCaptionData(sampleCues(), TimeRange{ms(2002), ms(4004)}, CaptionFileFormat::Srt);
    REQUIRE(written.has_value());
    CHECK(written->cues == 2);
    CHECK(written->data == "1\n00:00:00,000 --> 00:00:01,001\nHello there.\n\n"
                           "2\n00:00:02,002 --> 00:00:04,004\nSecond caption,\ntwo lines.\n\n");
}

TEST_CASE("Captions: malformed and hostile files never crash the readers (P.2)", "[interchange][captions]") {
    std::vector<std::string> seeds;
    for (const CaptionFileFormat format : captionFileFormats()) {
        seeds.push_back(writeCaptionData(sampleCues(), TimeRange{Ticks{}, Ticks::fromSeconds(60)}, format, optionsFor(format))->data);
    }
    std::mt19937 rng(4242);
    for (int round = 0; round < 3000; ++round) {
        const std::size_t k = static_cast<std::size_t>(rng()) % seeds.size();
        std::string data = seeds[k];
        const int edits = 1 + static_cast<int>(rng() % 6);
        for (int e = 0; e < edits && !data.empty(); ++e) {
            const std::size_t at = rng() % data.size();
            switch (rng() % 4) {
            case 0:
                data[at] = static_cast<char>(rng());
                break;
            case 1:
                data.erase(at, std::min<std::size_t>(rng() % 16, data.size() - at));
                break;
            case 2:
                data.insert(at, data.substr(at, std::min<std::size_t>(rng() % 64, data.size() - at)));
                break;
            default:
                data.resize(at);
                break;
            }
        }
        for (const CaptionFileFormat format : captionFileFormats()) {
            const auto read = readCaptionData(data, format);
            if (read) {
                for (std::size_t i = 0; i < read->cues.size(); ++i) {
                    REQUIRE(read->cues[i].duration.value > 0);
                    REQUIRE(read->cues[i].start.value >= 0);
                    REQUIRE(isValidCaptionText(read->cues[i].text));
                    if (i > 0) {
                        REQUIRE(read->cues[i - 1].end() <= read->cues[i].start);
                    }
                }
            }
        }
    }
    // A deep XML nest and an enormous SCC timecode are refused or bounded.
    std::string deep = "<tt><body>";
    for (int i = 0; i < 5000; ++i) {
        deep += "<div>";
    }
    deep += "<p begin=\"1s\" end=\"2s\">x</p>";
    const auto nested = readCaptionData(deep, CaptionFileFormat::Ttml);
    if (nested) {
        CHECK(nested->cues.empty());
    }
    CHECK(readCaptionData("Scenarist_SCC V1.0\n\n99:99:99:99\t9420\n", CaptionFileFormat::Scc).has_value());
}
