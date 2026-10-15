// Final Cut Pro 7 XML (xmeml): the Premiere-style and FCP7-style fixtures, then targeted cases.

#include "InterchangeTestSupport.h"

#include <wabcut/interchange/ImportSummary.h>
#include <wabcut/model/Effects.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <utility>

using namespace wabcut;
using namespace wabcut::interchange;
using namespace wabcut::interchange::test;
using Catch::Approx;

namespace {

constexpr auto O = ReportOutcome::Carried;
constexpr auto A = ReportOutcome::Approximated;
constexpr auto D = ReportOutcome::Dropped;

Ticks f30(std::int64_t n) {
    return frames(n, kRate29_97);
}

} // namespace

TEST_CASE("interchange: FCP7 XML from Premiere: the whole timeline", "[interchange][fcp7]") {
    const ImportResult r = importFixture("premiere_2997df.xml");
    CHECK(r.format == Format::Fcp7Xml);
    REQUIRE(r.sequences.size() == 2);
    const Sequence& seq = sequenceAt(r, 0);
    CHECK(seq.name == "Main Edit");
    CHECK(seq.frameRate == kRate29_97);
    CHECK(seq.dropFrame);
    CHECK(seq.width == 1920);
    CHECK(seq.height == 1080);
    CHECK(seq.audioRate == kRate48kHz);
    REQUIRE(seq.videoTracks.size() == 3);
    REQUIRE(seq.audioTracks.size() == 4); // two exploded stereo tracks + two mono tracks

    CHECK(clips(seq, "V1") == Strings{"0+150 A001_C002.mov @30", "150+150 A002_C010.mov @100",
                                      "330+90 A001_C002.mov @500 off", "420+60 B001_C001.mov @10"});
    CHECK(clips(seq, "V2") == Strings{"30+90 Title_Card.png @0", "200+60 Logo Sting @0"}); // C.8: a nested clip
    CHECK(clips(seq, "V3") == Strings{"350+50 A002_C010.mov @10"});
    CHECK(clips(seq, "A1") == Strings{"0+150 A001_C002.mov @30", "150+150 A002_C010.mov @100"});
    CHECK(clips(seq, "A2") == Strings{"0+480 Music_Bed.wav @0"});
    CHECK(clips(seq, "A3") == Strings{"60+180 VO_Take3.wav @0"});
    CHECK(clips(seq, "A4") == Strings{"330+90 A001_C002.mov @500"});
    CHECK(links(seq) == Strings{"V1@0+A1@0", "V1@150+A1@150", "V1@330+A4@330"});

    // Track toggles.
    CHECK(seq.videoTracks[0]->enabled);
    CHECK_FALSE(seq.videoTracks[1]->enabled);
    CHECK(seq.videoTracks[2]->locked);
    CHECK_FALSE(seq.audioTracks[0]->locked);
    CHECK(seq.audioTracks[3]->locked);
    CHECK(seq.videoTracks[0]->targeted);
    CHECK_FALSE(seq.videoTracks[1]->targeted);

    // Marks and markers (X.1: the clip marker is a marker of its source, below).
    CHECK(seq.markIn == f30(30));
    CHECK(seq.markOut == f30(450));
    REQUIRE(seq.markers.size() == 2);
    CHECK(seq.markers[0].name == "Intro");
    CHECK(seq.markers[0].comment == "Music hits");
    CHECK(seq.markers[0].start == f30(45));
    CHECK(seq.markers[0].duration == Ticks{});
    CHECK(seq.markers[0].colorIndex == 1); // red
    CHECK(seq.markers[1].name == "Chapter 2");
    CHECK(seq.markers[1].start == f30(330));
    CHECK(seq.markers[1].duration == f30(30));
    CHECK(seq.markers[1].colorIndex == 6); // blue
    // The clip marker on A002_C010.mov is a marker of the file at its media frame 120: it moves with the clip.
    const MediaItem& a002 = mediaNamed(r, "A002_C010.mov");
    REQUIRE(a002.markers.size() == 1);
    CHECK(a002.markers[0].name == "Pickup");
    CHECK(a002.markers[0].start == f30(120));

    // Static effect values.
    CHECK(track(seq, "V2").clips[0].properties().value(ParamId::Opacity) == Approx(0.8));
    CHECK(track(seq, "A2").clips[0].properties().value(ParamId::VolumeDb) == Approx(-6.0).margin(0.001));

    // The nested sequence is a sequence of its own, which the nested clip on V2 plays.
    const Sequence& nest = sequenceAt(r, 1);
    CHECK(nest.name == "Logo Sting");
    CHECK(track(seq, "V2").clips[1].sequence == nest.id);
    CHECK(nest.frameRate == kRate29_97);
    CHECK(clips(nest, "V1") == Strings{"0+60 A002_C010.mov @0"});

    // Media: decoded Windows and UNC paths, declared streams and durations.
    CHECK(mediaNames(r) == Strings{"A001_C002.mov", "A002_C010.mov", "B001_C001.mov", "Title_Card.png",
                                   "Music_Bed.wav", "VO_Take3.wav"});
    const MediaItem& a001 = mediaNamed(r, "A001_C002.mov");
    CHECK(a001.locator.path == "C:/Projects/Wabcut Demo/Footage/A001_C002.mov");
    REQUIRE(a001.info.video.size() == 1);
    REQUIRE(a001.info.audio.size() == 1);
    CHECK(a001.info.audio[0].channels == 2);
    CHECK(a001.info.duration == f30(900));
    CHECK(mediaNamed(r, "B001_C001.mov").locator.path == "//fileserver/share/Footage/B001_C001.mov");
    CHECK(mediaNamed(r, "Title_Card.png").info.isStillImage);
    CHECK(r.unresolvedMedia.size() == 6);
}

TEST_CASE("interchange: FCP7 XML from Premiere: the import report", "[interchange][fcp7]") {
    const ImportResult r = importFixture("premiere_2997df.xml");
    const ImportReport& rep = r.report;
    const std::string xml = readFixture("premiere_2997df.xml");

    const ReportEntry& dissolve =
        entry(rep, O, ReportCategory::Transition,
              "'Cross Dissolve' (30 frames, centred on the cut at 00:00:05;00) on V1 -> crossDissolve");
    CHECK(dissolve.location.file == "premiere_2997df.xml");
    CHECK(dissolve.location.element == "/xmeml/sequence/media/video/track[1]/transitionitem[1]");
    CHECK(dissolve.location.line == lineOf(xml, "<transitionitem>"));
    CHECK(dissolve.sequence == "Main Edit");
    CHECK(hasEntry(rep, O, ReportCategory::Transition,
                   "'Dip to Black' (15 frames, ending at the cut at 00:00:16;00) on V1 -> dipToBlack"));
    CHECK(hasEntry(rep, O, ReportCategory::Speed, "'B001_C001.mov' on V1 plays at 50%"));
    CHECK(hasEntry(rep, O, ReportCategory::Clip, "nested sequence 'Logo Sting' on V2 at 00:00:06;20 (60 frames)"));
    CHECK(hasEntry(rep, D, ReportCategory::Clip, "generated clip 'Black Video' on V3"));
    CHECK(hasEntry(rep, O, ReportCategory::Effect, "'Gaussian Blur' on")); // X.1: an effect of the clip's stack
    CHECK(hasEntry(rep, O, ReportCategory::Effect, "'Opacity' on 'Title_Card.png'"));
    CHECK(hasEntry(rep, O, ReportCategory::SequenceSetting, "start timecode 01:00:00;00")); // X.1: kept
    CHECK(hasEntry(rep, O, ReportCategory::Marker, "clip marker 'Pickup' of 'A002_C010.mov'"));
    // Exploded stereo tracks are whole stereo tracks; the mono track playing one channel of a stereo stream plays
    // that channel (A.9: a mono channel map).
    CHECK(rep.count(O, ReportCategory::AudioMapping) == 3);
    CHECK(hasEntry(rep, O, ReportCategory::AudioMapping,
                   "'A001_C002.mov' on A4 plays channel 1 of its 2-channel stream (a mono channel map)"));
    CHECK(reasons(rep, A, ReportCategory::AudioMapping).empty());
    const Clip& oneChannel = track(sequenceAt(r, 0), "A4").clips[0];
    REQUIRE(oneChannel.channelMap());
    CHECK(*oneChannel.channelMap() == AudioChannelMap::mono(0));
    CHECK(rep.count(O, ReportCategory::Clip) == 13); // 11 in Main Edit + the nested clip + 1 in Logo Sting
    CHECK(rep.count(A, ReportCategory::Media) == 6); // all offline: no search folders given
    CHECK(rep.count(D) == 1);
}

TEST_CASE("interchange: FCP7 XML: media found under a search folder", "[interchange][fcp7][relink]") {
    const auto root = scratchDir("fcp7-relink");
    for (const auto* name :
         {"A001_C002.mov", "A002_C010.mov", "B001_C001.mov", "Title_Card.png", "Music_Bed.wav"}) {
        writeFile(root / "Card 1" / name, "x");
    }
    ImportOptions options;
    options.mediaSearchRoots = {root};
    const ImportResult r = importFixture("premiere_2997df.xml", options);
    REQUIRE(r.unresolvedMedia.size() == 1);
    CHECK(r.unresolvedMedia[0].name == "VO_Take3.wav");
    CHECK(r.unresolvedMedia[0].reference ==
          "file://localhost/C%3a/Projects/Wabcut%20Demo/Audio/VO_Take3.wav");
    CHECK(r.unresolvedMedia[0].path == "C:/Projects/Wabcut Demo/Audio/VO_Take3.wav");
    CHECK_FALSE(r.unresolvedMedia[0].tried.empty());
    CHECK(std::filesystem::equivalent(std::filesystem::path(mediaNamed(r, "A001_C002.mov").locator.path),
                                      root / "Card 1" / "A001_C002.mov"));
    CHECK_FALSE(mediaNamed(r, "A001_C002.mov").locator.offline);
    CHECK(mediaNamed(r, "VO_Take3.wav").locator.offline);
    CHECK_FALSE(mediaNamed(r, "A001_C002.mov").locator.fingerprint.isKnown()); // filled in by the media probe
    CHECK(hasEntry(r.report, O, ReportCategory::Media, "'Music_Bed.wav' found under a search folder"));
}

TEST_CASE("interchange: FCP7 XML from FCP7 on macOS: project/bin wrapper, stereo pair, UTF-8 path",
          "[interchange][fcp7]") {
    const ImportResult r = importFixture("fcp7_23976_mac.xml");
    REQUIRE(r.sequences.size() == 1);
    const Sequence& seq = sequenceAt(r, 0);
    CHECK(seq.name == "Reel 2 Assembly");
    CHECK(seq.frameRate == kRate23_976);
    CHECK_FALSE(seq.dropFrame);
    CHECK(clips(seq, "V1") ==
          Strings{"0+96 Café Interview @240", "96+72 Broll_01.mov @0", "168+72 Broll_02.mov @48"});
    CHECK(clips(seq, "V2") == Strings{"200+30 Broll_01.mov @100"});
    // Channels 1 and 2 of the stereo stream (linked, same range) became one clip on A1.
    CHECK(clips(seq, "A1") == Strings{"0+96 Café Interview @240"});
    CHECK(clips(seq, "A2").empty());
    CHECK(clips(seq, "A3") == Strings{"0+240 Room_Tone.aif @0"});
    CHECK(links(seq) == Strings{"V1@0+A1@0"});
    CHECK(hasEntry(r.report, A, ReportCategory::AudioMapping,
                   "the 2 channels of 'Café Interview' on A1+A2 became one clip on A1"));
    CHECK(hasEntry(r.report, O, ReportCategory::Transition,
                   "'Cross Dissolve' (24 frames, centred on the cut at 00:00:07:00)"));
    CHECK(hasEntry(r.report, O, ReportCategory::SequenceSetting, "start timecode 00:59:58:00")); // X.1: kept
    CHECK(seq.startTimecode == frames(86'352, kRate23_976));

    const MediaItem& cafe = mediaNamed(r, "Café_Interview.mov");
    CHECK(cafe.locator.path == "/Volumes/RAID/Reel 2/Café_Interview.mov");
    // X.1: the sequence keeps its bin (inside the import bin); media without a master clip in a bin stay above it.
    CHECK(binPath(r, seq.bin) == "Edits");
    CHECK(binPath(r, cafe.bin).empty());
    CHECK(hasEntry(r.report, O, ReportCategory::Media, "bin 'Edits'"));
    CHECK(mediaNamed(r, "Broll_02.mov").locator.path == "/Volumes/RAID/Reel 2/B-Roll/Broll_02.mov");

    const Clip& moved = track(seq, "V2").clips[0];
    CHECK(moved.properties().value(ParamId::Scale) == Approx(1.2));
    CHECK(moved.properties().value(ParamId::Rotation) == Approx(15.0));
    REQUIRE(seq.markers.size() == 1);
    CHECK(seq.markers[0].name == "Scene 2");
    CHECK(seq.markers[0].start == frames(96, kRate23_976));
    CHECK(seq.markers[0].colorIndex == 1); // RGB 255,0,0
    // X.1: the clip marker is a marker of Broll_01.mov at its media frame 110: it moves with the clip.
    const MediaItem& broll = mediaNamed(r, "Broll_01.mov");
    REQUIRE(broll.markers.size() == 1);
    CHECK(broll.markers[0].name == "Flash frame");
    CHECK(broll.markers[0].start == frames(110, kRate23_976));
    CHECK(broll.markers[0].comment == "check with grade");
}

namespace {

// A minimal 25 fps xmeml with one video track holding `items`.
std::string xmeml(const std::string& items) {
    return R"(<?xml version="1.0" encoding="UTF-8"?>
<xmeml version="4"><sequence id="s1"><name>T</name><rate><timebase>25</timebase><ntsc>FALSE</ntsc></rate>
<media><video><format><samplecharacteristics><width>1280</width><height>720</height></samplecharacteristics></format>
<track>)" + items +
           R"(</track></video></media></sequence></xmeml>)";
}

std::string clipitem(const std::string& name, int start, int end, int in, int out,
                     const std::string& extra = {}) {
    return "<clipitem id=\"" + name + "\"><name>" + name +
           "</name><rate><timebase>25</timebase></rate><start>" + std::to_string(start) + "</start><end>" +
           std::to_string(end) + "</end><in>" + std::to_string(in) + "</in><out>" + std::to_string(out) +
           "</out><file id=\"file-" + name + "\"><name>" + name + "</name><pathurl>file:///m/" + name +
           "</pathurl><rate><timebase>25</timebase></rate><duration>1000</duration>"
           "<media><video/></media></file>" +
           extra + "</clipitem>";
}

std::string transition(int start, int end, const std::string& alignment, const std::string& extra = {}) {
    return "<transitionitem><start>" + std::to_string(start) + "</start><end>" + std::to_string(end) +
           "</end><alignment>" + alignment + "</alignment>" + extra +
           "<effect><name>Cross Dissolve</name><effectid>Cross Dissolve</effectid></effect></transitionitem>";
}

std::string speedFilter(const std::string& parameters) {
    return "<filter><effect><name>Time Remap</name><effectid>timeremap</effectid>" + parameters +
           "</effect></filter>";
}

} // namespace

TEST_CASE("interchange: FCP7 XML: transition alignment decides the edit point", "[interchange][fcp7]") {
    SECTION("centre, odd length: the extra frame goes after the cut") {
        // 25-frame dissolve over 90..115; A's in/out run to the transition's end, B's from its start.
        const auto r = importText(xmeml(clipitem("A", 0, -1, 0, 115) + transition(90, 115, "center") +
                                        clipitem("B", -1, 200, 40, 150)),
                                  Format::Fcp7Xml);
        CHECK(clips(sequenceAt(r, 0), "V1") == Strings{"0+102 A @0", "102+98 B @52"});
    }
    SECTION("start: the transition begins at the cut") {
        const auto r = importText(xmeml(clipitem("A", 0, -1, 0, 120) + transition(100, 120, "start") +
                                        clipitem("B", -1, 200, 10, 110)),
                                  Format::Fcp7Xml);
        CHECK(clips(sequenceAt(r, 0), "V1") == Strings{"0+100 A @0", "100+100 B @10"});
    }
    SECTION("end-black: a fade out at the clip's tail") {
        const auto r = importText(xmeml(clipitem("A", 0, -1, 5, 125) + transition(100, 120, "end-black")),
                                  Format::Fcp7Xml);
        CHECK(clips(sequenceAt(r, 0), "V1") == Strings{"0+120 A @5"});
        CHECK(hasEntry(r.report, O, ReportCategory::Transition, "ending at the cut at 00:00:04:20"));
    }
    SECTION("start-black: a fade in at the clip's head") {
        const auto r = importText(xmeml(transition(0, 10, "start-black") + clipitem("B", -1, 50, 7, 57)),
                                  Format::Fcp7Xml);
        CHECK(clips(sequenceAt(r, 0), "V1") == Strings{"0+50 B @7"});
        CHECK(hasEntry(r.report, O, ReportCategory::Transition, "starting at the cut at 00:00:00:00"));
    }
    SECTION("Premiere's cutPointTicks overrides the alignment") {
        const std::string ticks =
            "<cutPointTicks>" + std::to_string(10 * 10'160'640'000LL) + "</cutPointTicks>";
        const auto r = importText(xmeml(clipitem("A", 0, -1, 0, 130) + transition(100, 130, "center", ticks) +
                                        clipitem("B", -1, 200, 0, 100)),
                                  Format::Fcp7Xml);
        CHECK(clips(sequenceAt(r, 0), "V1") == Strings{"0+110 A @0", "110+90 B @10"});
    }
    SECTION("-1 without a transition next to it falls back to in/out") {
        const auto r = importText(xmeml(clipitem("A", -1, 50, 10, 30)), Format::Fcp7Xml);
        CHECK(clips(sequenceAt(r, 0), "V1") == Strings{"30+20 A @10"});
    }
}

TEST_CASE("interchange: FCP7 XML: the media In of retimed clips", "[interchange][fcp7]") {
    SECTION("speedkfin keyframe value") {
        const auto speed = speedFilter(
            "<parameter><parameterid>speed</parameterid><value>200</value></parameter>"
            "<parameter><parameterid>graphdict</parameterid><keyframe><when>40</when><value>80</value>"
            "<speedkfin>TRUE</speedkfin></keyframe><keyframe><when>90</when><value>180</"
            "value><speedkfout>TRUE</speedkfout>"
            "</keyframe></parameter>");
        const auto r = importText(xmeml(clipitem("A", 0, 50, 40, 90, speed)), Format::Fcp7Xml);
        CHECK(clips(sequenceAt(r, 0), "V1") == Strings{"0+50 A @80"});
        CHECK(hasEntry(r.report, O, ReportCategory::Speed, "'A' on V1 plays at 200%"));
    }
    SECTION("<in> in retimed frames, scaled by the speed") {
        const auto speed =
            speedFilter("<parameter><parameterid>speed</parameterid><value>50</value></parameter>");
        const auto r = importText(xmeml(clipitem("A", 0, 50, 40, 90, speed)), Format::Fcp7Xml);
        CHECK(clips(sequenceAt(r, 0), "V1") == Strings{"0+50 A @20"});
    }
    SECTION("reverse and variable speed are reported") {
        const auto reverse =
            speedFilter("<parameter><parameterid>speed</parameterid><value>-100</value></parameter>");
        const auto variable =
            speedFilter("<parameter><parameterid>variablespeed</parameterid><value>1</value></parameter>");
        const auto r =
            importText(xmeml(clipitem("A", 0, 50, 0, 50, reverse) + clipitem("B", 50, 90, 0, 40, variable)),
                       Format::Fcp7Xml);
        CHECK(hasEntry(r.report, A, ReportCategory::Speed, "'A' on V1 plays at 100% in reverse"));
        CHECK(hasEntry(r.report, A, ReportCategory::Speed, "'B' on V1 has variable speed"));
    }
}

TEST_CASE("interchange: FCP7 XML: keyframed and disabled effects", "[interchange][fcp7]") {
    const std::string keyframed = "<filter><effect><name>Opacity</name><effectid>opacity</"
                                  "effectid><parameter><parameterid>opacity</parameterid>"
                                  "<keyframe><when>0</when><value>25</value></keyframe><keyframe><when>20</"
                                  "when><value>100</value></keyframe>"
                                  "</parameter></effect></filter>";
    const std::string disabled =
        "<filter><enabled>FALSE</enabled><effect><name>Opacity</name><effectid>opacity</effectid><parameter>"
        "<parameterid>opacity</parameterid><value>10</value></parameter></effect></filter>";
    const auto r =
        importText(xmeml(clipitem("A", 0, 50, 0, 50, keyframed) + clipitem("B", 50, 90, 0, 40, disabled)),
                   Format::Fcp7Xml);
    const Sequence& seq = sequenceAt(r, 0);
    // X.1: keyframes are a curve in the clip's media frames.
    const ParamValue& opacity = track(seq, "V1").clips[0].properties()[ParamId::Opacity];
    REQUIRE(opacity.isCurve());
    REQUIRE(opacity.curve()->keys.size() == 2);
    CHECK(opacity.curve()->keys[0].value == Approx(0.25));
    CHECK(opacity.curve()->keys[1].value == Approx(1.0));
    CHECK(track(seq, "V1").clips[1].properties().value(ParamId::Opacity) == Approx(1.0));
    CHECK(hasEntry(r.report, O, ReportCategory::Effect, "'Opacity' on 'A' (keyframed)"));
    CHECK(hasEntry(r.report, D, ReportCategory::Effect, "disabled effect 'Opacity' on 'B'"));
}

TEST_CASE("interchange: FCP7 XML: overlapping clips are moved to an extra track, never lost",
          "[interchange][fcp7]") {
    const auto r =
        importText(xmeml(clipitem("A", 0, 50, 0, 50) + clipitem("B", 40, 80, 0, 40)), Format::Fcp7Xml);
    const Sequence& seq = sequenceAt(r, 0);
    REQUIRE(seq.videoTracks.size() == 2);
    CHECK(clips(seq, "V1") == Strings{"0+50 A @0"});
    CHECK(clips(seq, "V2") == Strings{"40+40 B @0"});
    CHECK(seq.videoTracks[1]->name == "V1 overlaps");
    CHECK(hasEntry(r.report, A, ReportCategory::Track,
                   "'B' at 00:00:01:15 overlaps another clip on V1; moved to V2"));
}

TEST_CASE("interchange: FCP7 XML: hostile nesting is cut off, not followed", "[interchange][fcp7]") {
    // 60 nested sequences, each holding the next inside a clipitem.
    std::string inner = clipitem("leaf", 0, 10, 0, 10);
    for (int i = 0; i < 60; ++i) {
        inner = "<clipitem id=\"n" + std::to_string(i) + "\"><name>nest " + std::to_string(i) +
                "</name><start>0</start><end>10</end><in>0</in><out>10</out><sequence id=\"seq" +
                std::to_string(i) + "\"><name>nest " + std::to_string(i) +
                "</name><rate><timebase>25</timebase></rate><media><video><track>" + inner +
                "</track></video></media></sequence></clipitem>";
    }
    const auto r = importText(xmeml(inner), Format::Fcp7Xml);
    CHECK(r.sequences.size() <= 33);
    CHECK(hasEntry(r.report, D, ReportCategory::Clip, "is nested too deep"));
}

TEST_CASE("interchange: FCP7 XML: malformed documents are errors", "[interchange][fcp7]") {
    const std::string xml = readFixture("premiere_2997df.xml");
    const auto truncated =
        ImportRouter::importData(std::string_view(xml).substr(0, xml.size() / 2), Format::Fcp7Xml, "t.xml");
    REQUIRE_FALSE(truncated);
    CHECK(truncated.error().code == ErrorCode::ParseError);
    CHECK(truncated.error().message.find("line") != std::string::npos);

    const auto noSequence =
        ImportRouter::importData("<xmeml version=\"5\"><clip/></xmeml>", Format::Fcp7Xml, "n.xml");
    REQUIRE_FALSE(noSequence);
    CHECK(noSequence.error().code == ErrorCode::ParseError);

    const auto wrongRoot = ImportRouter::importData("<fcpxml version=\"1.10\"/>", Format::Fcp7Xml, "w.xml");
    REQUIRE_FALSE(wrongRoot);
    CHECK(wrongRoot.error().code == ErrorCode::ParseError);
}

// --- C.4 fidelity: transitions, speeds and titles carried into the model ----------------------------------

namespace {

const Clip& clipNamed(const Track& t, std::string_view name) {
    const auto it = std::find_if(t.clips.begin(), t.clips.end(), [&](const Clip& c) { return c.name() == name; });
    REQUIRE(it != t.clips.end());
    return *it;
}

std::string audioXmeml(const std::string& items) {
    return R"(<?xml version="1.0" encoding="UTF-8"?>
<xmeml version="4"><sequence id="s1"><name>T</name><rate><timebase>25</timebase><ntsc>FALSE</ntsc></rate>
<media><audio><track>)" +
           items + R"(</track></audio></media></sequence></xmeml>)";
}

std::string audioClip(const std::string& name, int start, int end, int in, int out) {
    return "<clipitem id=\"" + name + "\"><name>" + name + "</name><rate><timebase>25</timebase></rate><start>" +
           std::to_string(start) + "</start><end>" + std::to_string(end) + "</end><in>" + std::to_string(in) +
           "</in><out>" + std::to_string(out) + "</out><file id=\"file-" + name + "\"><name>" + name +
           ".wav</name><pathurl>file:///m/" + name +
           ".wav</pathurl><rate><timebase>25</timebase></rate><duration>1000</duration>"
           "<media><audio><channelcount>1</channelcount></audio></media></file>"
           "<sourcetrack><mediatype>audio</mediatype><trackindex>1</trackindex></sourcetrack></clipitem>";
}

std::string audioTransition(int start, int end, const std::string& name) {
    return "<transitionitem><start>" + std::to_string(start) + "</start><end>" + std::to_string(end) +
           "</end><alignment>center</alignment><effect><name>" + name + "</name><effectid>" + name +
           "</effectid><mediatype>audio</mediatype></effect></transitionitem>";
}

std::string parameter(const std::string& id, const std::string& name, const std::string& value) {
    return "<parameter><parameterid>" + id + "</parameterid><name>" + name + "</name><value>" + value +
           "</value></parameter>";
}

std::string rgba(int r, int g, int b, int a) {
    return "<alpha>" + std::to_string(a) + "</alpha><red>" + std::to_string(r) + "</red><green>" + std::to_string(g) +
           "</green><blue>" + std::to_string(b) + "</blue>";
}

std::string generator(const std::string& name, int start, int end, const std::string& effect,
                      const std::string& params) {
    return "<generatoritem id=\"" + name + "\"><name>" + name + "</name><rate><timebase>25</timebase></rate><start>" +
           std::to_string(start) + "</start><end>" + std::to_string(end) + "</end><in>0</in><out>" +
           std::to_string(end - start) + "</out><enabled>TRUE</enabled><effect><name>" + effect + "</name><effectid>" +
           effect + "</effectid><effectcategory>Text</effectcategory><effecttype>generator</effecttype>"
                    "<mediatype>video</mediatype>" +
           params + "</effect></generatoritem>";
}

} // namespace

TEST_CASE("interchange: FCP7 XML: transitions and speeds become model transitions and clip speeds",
          "[interchange][fcp7][fidelity]") {
    const ImportResult r = importFixture("premiere_2997df.xml");
    const Track& v1 = track(sequenceAt(r, 0), "V1");
    REQUIRE(v1.transitions.size() == 2);
    CHECK(v1.transitions[0].effect == "crossDissolve");
    CHECK(v1.transitions[0].before == frames(15, kRate29_97));
    CHECK(v1.transitions[0].after == frames(15, kRate29_97));
    CHECK(v1.transitions[1].effect == "dipToBlack");
    CHECK(v1.transitions[1].before == frames(15, kRate29_97));
    CHECK(v1.transitions[1].after == Ticks{});
    const Clip& slow = clipNamed(v1, "B001_C001.mov");
    CHECK(slow.speed == Speed{1, 2});
    CHECK(slow.sourceDuration().value == slow.duration().value / 2);
}

TEST_CASE("interchange: FCP7 XML: constant speeds keep the clip's length and media In", "[interchange][fcp7][fidelity]") {
    const auto fast = speedFilter(
        "<parameter><parameterid>speed</parameterid><value>200</value></parameter>"
        "<parameter><parameterid>graphdict</parameterid><keyframe><when>40</when><value>80</value>"
        "<speedkfin>TRUE</speedkfin></keyframe></parameter>");
    const auto slow = speedFilter("<parameter><parameterid>speed</parameterid><value>50</value></parameter>");
    const auto backwards = speedFilter("<parameter><parameterid>speed</parameterid><value>-100</value></parameter>");
    const auto r = importText(xmeml(clipitem("A", 0, 50, 40, 90, fast) + clipitem("B", 50, 100, 40, 90, slow) +
                                    clipitem("C", 100, 150, 0, 50, backwards)),
                              Format::Fcp7Xml);
    const Track& v1 = track(sequenceAt(r, 0), "V1");
    CHECK(clips(sequenceAt(r, 0), "V1") == Strings{"0+50 A @80", "50+50 B @20", "100+50 C @0"});
    CHECK(clipNamed(v1, "A").speed == Speed{2, 1});
    CHECK(clipNamed(v1, "A").sourceOut() == frames(180, kRate25));
    CHECK(clipNamed(v1, "B").speed == Speed{1, 2});
    CHECK(clipNamed(v1, "C").reversed);
    CHECK(hasEntry(r.report, O, ReportCategory::Speed, "'B' on V1 plays at 50%"));
    CHECK(hasEntry(r.report, A, ReportCategory::Speed, "'C' on V1 plays at 100% in reverse; it plays the same media"));
}

TEST_CASE("interchange: FCP7 XML: audio cross fades map to constant gain or power", "[interchange][fcp7][fidelity]") {
    for (const auto& [name, effect] : {std::pair{std::string("Cross Fade (0dB)"), std::string("constantGain")},
                                       std::pair{std::string("Cross Fade (+3dB)"), std::string("constantPower")}}) {
        INFO(name);
        const auto r = importText(audioXmeml(audioClip("a", 0, -1, 0, 115) + audioTransition(90, 115, name) +
                                             audioClip("b", -1, 200, 40, 150)),
                                  Format::Fcp7Xml);
        const Track& a1 = track(sequenceAt(r, 0), "A1");
        REQUIRE(a1.transitions.size() == 1);
        CHECK(a1.transitions[0].effect == effect);
        CHECK(a1.transitions[0].before + a1.transitions[0].after == frames(25, kRate25));
        CHECK(hasEntry(r.report, O, ReportCategory::Transition, "on A1 -> " + effect));
    }
}

TEST_CASE("interchange: FCP7 XML: a transition without clips at its edit point stays a cut",
          "[interchange][fcp7][fidelity]") {
    // The clips leave a gap where the transition says the cut is.
    const auto r = importText(xmeml(clipitem("A", 0, 80, 0, 80) + transition(90, 110, "center") +
                                    clipitem("B", 120, 200, 0, 80)),
                              Format::Fcp7Xml);
    CHECK(track(sequenceAt(r, 0), "V1").transitions.empty());
    CHECK(hasEntry(r.report, A, ReportCategory::Transition, "imported as a cut: no clip starts or ends at its edit point"));
}

TEST_CASE("interchange: FCP7 XML: Text and Outline Text generators become titles", "[interchange][fcp7][titles]") {
    const std::string text = generator(
        "Text", 10, 60, "Text",
        parameter("str", "Text", "Hello&#13;World") + parameter("fontname", "Font", "Helvetica") +
            parameter("fontsize", "Size", "48") + parameter("fontstyle", "Style", "2") +
            parameter("fontalign", "Alignment", "1") + parameter("fontcolor", "Font Color", rgba(255, 0, 0, 255)) +
            parameter("origin", "Origin", "<horiz>0.1</horiz><vert>0.2</vert>") +
            parameter("autokern", "Auto Kerning", "TRUE"));
    const std::string outline = generator(
        "Lower third", 70, 120, "Outline Text",
        parameter("str", "Text", "Outlined") + parameter("fontname", "Font", "Arial") +
            parameter("linewidth", "Line Width", "4") + parameter("linecolor", "Line Color", rgba(0, 0, 255, 255)));
    const std::string slug =
        "<generatoritem id=\"slug\"><name>Slug</name><start>130</start><end>140</end><effect><name>Slug</name>"
        "<effectid>slug</effectid><effecttype>generator</effecttype></effect></generatoritem>";
    const auto r = importText(xmeml(text + outline + slug), Format::Fcp7Xml);
    const Sequence& seq = sequenceAt(r, 0);
    const Track& v1 = track(seq, "V1");
    REQUIRE(v1.clips.size() == 2);

    const Clip& hello = v1.clips[0];
    CHECK(hello.start() == frames(10, kRate25));
    CHECK(hello.duration() == frames(50, kRate25));
    const MediaItem* helloItem = r.project.findMedia(hello.media);
    REQUIRE(helloItem);
    REQUIRE(helloItem->isTitle());
    CHECK(helloItem->generator->text == "Hello\nWorld");
    CHECK(helloItem->generator->fontFamily == "Helvetica");
    CHECK(helloItem->generator->fontStyle == "Bold");
    CHECK(helloItem->info.video[0].width == 1280);
    CHECK(helloItem->bin == r.bin);
    CHECK(hello.name() == "Hello");
    REQUIRE(hello.effects().size() == 1);
    const EffectInstance& style = hello.effects()[0];
    CHECK(effectParameterAt(style, "size", Ticks{}) == 48.0);
    CHECK(effectParameterAt(style, "alignment", Ticks{}) == 0.0);
    CHECK(effectParameterAt(style, "fill.red", Ticks{}) == 1.0);
    CHECK(effectParameterAt(style, "fill.green", Ticks{}) == 0.0);
    CHECK(hasEntry(r.report, A, ReportCategory::Clip, "title 'Hello' on V1 at 00:00:00:10 (50 frames, Helvetica Bold); not carried: Origin"));

    const Clip& lower = v1.clips[1];
    CHECK(lower.name() == "Lower third");
    CHECK(r.project.findMedia(lower.media)->generator->text == "Outlined");
    CHECK(effectParameterAt(lower.effects()[0], "stroke.width", Ticks{}) == 4.0);
    CHECK(effectParameterAt(lower.effects()[0], "stroke.blue", Ticks{}) == 1.0);
    CHECK(hasEntry(r.report, O, ReportCategory::Clip, "title 'Outlined' on V1"));
    CHECK(hasEntry(r.report, D, ReportCategory::Clip, "generated clip 'Slug' on V1"));
    // Titles have no file: nothing to relink, never offline, listed as generated in the summary.
    CHECK(r.unresolvedMedia.empty());
    CHECK(r.media.size() == 2);
    CHECK(formatSummary(r).find("'Hello'                  (wabcut.title, no file)") != std::string::npos);
}

TEST_CASE("interchange: FCP7 XML: variable speed becomes a time remap", "[interchange][fcp7][remap]") {
    // A 50-frame clip whose <in> is 40: its time graph plays media 80..100 at 100 % (clip frames 40..60), holds
    // frame 100 (60..70), runs back to 90 (70..80) and on to 110 at 200 % (80..90).
    const auto graph = [](const std::string& extra) {
        return speedFilter("<parameter><parameterid>variablespeed</parameterid><value>1</value></parameter>"
                           "<parameter><parameterid>graphdict</parameterid>"
                           "<keyframe><when>40</when><value>80</value><speedkfin>TRUE</speedkfin></keyframe>"
                           "<keyframe><when>60</when><value>100</value>" +
                           extra +
                           "</keyframe><keyframe><when>70</when><value>100</value></keyframe>"
                           "<keyframe><when>80</when><value>90</value></keyframe>"
                           "<keyframe><when>90</when><value>110</value><speedkfout>TRUE</speedkfout></keyframe>"
                           "</parameter>");
    };
    const auto r = importText(xmeml(clipitem("A", 0, 50, 40, 90, graph(""))), Format::Fcp7Xml);
    const Clip& c = track(sequenceAt(r, 0), "V1").clips[0];
    REQUIRE(c.isRemapped());
    CHECK(c.speed == Speed{1, 1});
    std::vector<std::int64_t> shown;
    for (std::int64_t k = 0; k < 50; ++k) {
        shown.push_back(ticksToFrames(c.sourceTimeAt(frames(k, kRate25)), kRate25));
    }
    std::vector<std::int64_t> expected;
    for (std::int64_t k = 80; k < 100; ++k) {
        expected.push_back(k);
    }
    expected.insert(expected.end(), 10, 100);
    for (std::int64_t k = 99; k >= 90; --k) {
        expected.push_back(k);
    }
    for (std::int64_t k = 90; k < 110; k += 2) {
        expected.push_back(k);
    }
    CHECK(shown == expected);
    CHECK(hasEntry(r.report, O, ReportCategory::Speed, "'A' on V1 has variable speed: a time remap with 5 keys"));

    // X.1: a curved key is a Bezier key whose speed eases through it (FCP7 keeps no handles: monotone tangents
    // through its neighbours; here into the hold, so it eases out to a stop). Every key stays where it was.
    const auto smooth =
        importText(xmeml(clipitem("A", 0, 50, 40, 90, graph("<interpolation><name>FCPCurve</name></interpolation>"))),
                   Format::Fcp7Xml);
    const Clip& eased = track(sequenceAt(smooth, 0), "V1").clips[0];
    REQUIRE(eased.isRemapped());
    const Curve& curve = *eased.timeRemap();
    REQUIRE(curve.keys.size() == 5);
    CHECK(curve.keys[0].interpolation == Interpolation::Linear);
    CHECK(curve.keys[1].interpolation == Interpolation::Bezier);
    CHECK(curve.keys[1].in.slope == 0.0);
    CHECK(curve.keys[2].interpolation == Interpolation::Linear);
    std::vector<std::int64_t> easedShown;
    for (std::int64_t k = 0; k < 20; ++k) {
        easedShown.push_back(ticksToFrames(eased.sourceTimeAt(frames(k, kRate25)), kRate25));
    }
    CHECK(easedShown.front() == 80);
    CHECK(std::is_sorted(easedShown.begin(), easedShown.end()));
    CHECK(easedShown != std::vector<std::int64_t>(expected.begin(), expected.begin() + 20)); // not straight
    CHECK(ticksToFrames(eased.sourceTimeAt(frames(20, kRate25)), kRate25) == 100);
    CHECK(hasEntry(smooth.report, A, ReportCategory::Speed, "smooth speed ramps were imported as Bezier keys"));
}

TEST_CASE("Review P2-25: an FCP7 time graph that is not a valid remap is refused before it is evaluated",
          "[interchange][fcp7][remap][review]") {
    // The second key lies past 2^53 ticks (about 9.8 hours of media): not a remap position.
    const auto r = importText(
        xmeml(clipitem("A", 0, 50, 40, 90,
                       speedFilter("<parameter><parameterid>variablespeed</parameterid><value>1</value></parameter>"
                                   "<parameter><parameterid>graphdict</parameterid>"
                                   "<keyframe><when>40</when><value>80</value></keyframe>"
                                   "<keyframe><when>60</when><value>1000000</value></keyframe>"
                                   "</parameter>"))),
        Format::Fcp7Xml);
    const Clip& c = track(sequenceAt(r, 0), "V1").clips[0];
    CHECK_FALSE(c.isRemapped());
    CHECK(hasEntry(r.report, A, ReportCategory::Speed, "has a time graph that is not a valid time remap"));
}

TEST_CASE("interchange: FCP7 XML: a multiclip becomes a multicam source and a multicam clip",
          "[interchange][fcp7][multicam]") {
    const auto angle = [](const std::string& name, bool active, int start) {
        return std::string("<angle>") + (active ? "<activevideoangle>TRUE</activevideoangle>" : "") + "<clip id=\"" +
               name + "-clip\"><name>" + name + "</name><rate><timebase>25</timebase></rate><media><video><track>" +
               clipitem(name, start, start + 100, 0, 100) + "</track></video></media></clip></angle>";
    };
    const std::string multiclip =
        "<clipitem id=\"mc\"><name>Interview</name><rate><timebase>25</timebase></rate><start>0</start><end>40</end>"
        "<in>10</in><out>50</out><multiclip id=\"multiclip-1\"><name>Interview MC</name><synctype>3</synctype>" +
        angle("CamA", false, 0) + angle("CamB", true, 5) + "</multiclip></clipitem>";
    const auto r = importText(xmeml(multiclip), Format::Fcp7Xml);
    REQUIRE(r.sequences.size() == 2);
    const Sequence& seq = sequenceAt(r, 0);
    const Sequence& source = sequenceAt(r, 1);
    REQUIRE(source.multicam);
    CHECK(source.name == "Interview MC");
    CHECK(source.multicam->sync == MulticamSync::Timecode);
    REQUIRE(source.multicam->angles.size() == 2);
    CHECK(source.multicam->angles[0].name == "CamA");
    CHECK(source.multicam->angles[1].offset == frames(5, kRate25));
    CHECK(clips(source, "V2") == Strings{"5+100 CamB @0"});
    CHECK(clips(seq, "V1") == Strings{"0+40 Interview @10"});
    const Clip& c = track(seq, "V1").clips[0];
    CHECK(c.sequence == source.id);
    CHECK(c.angles == AngleSet::of(1)); // CamB is the active video angle
    CHECK(hasEntry(r.report, O, ReportCategory::Clip, "multicam clip 'Interview MC' on V1"));
    CHECK(hasEntry(r.report, O, ReportCategory::SequenceSetting, "multicam source with 2 angles (timecode sync)"));
}
