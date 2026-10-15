// FCPXML 1.9-1.11: the library fixture, the .fcpxmld bundle, then targeted cases.

#include "InterchangeTestSupport.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::interchange;
using namespace wabcut::interchange::test;
using Catch::Approx;

namespace {
constexpr auto O = ReportOutcome::Carried;
constexpr auto A = ReportOutcome::Approximated;
constexpr auto D = ReportOutcome::Dropped;
} // namespace

TEST_CASE("interchange: FCPXML 1.10 library: spine, connected clips, gap, retime, trailing gap",
          "[interchange][fcpxml]") {
    const ImportResult r = importFixture("fcpx_1_10_23976.fcpxml");
    CHECK(r.format == Format::FcpXml);
    REQUIRE(r.sequences.size() == 1);
    const Sequence& seq = sequenceAt(r, 0);
    CHECK(seq.name == "Short Film v3");
    CHECK(seq.frameRate == kRate23_976);
    CHECK_FALSE(seq.dropFrame);
    CHECK(seq.width == 1920);
    CHECK(seq.audioRate == kRate48kHz);
    REQUIRE(seq.videoTracks.size() == 3); // X.1: the title on lane 2 is V3
    REQUIRE(seq.audioTracks.size() == 2);
    // The asset starts at 01:00:00:00; the clip's start 48 frames later is source In 48.
    CHECK(clips(seq, "V1") == Strings{"0+120 Interview_A @48", "120+96 Broll_City @72 off",
                                      "264+96 Broll_City @0", "360+48 Interview_A @200"});
    CHECK(clips(seq, "V2") == Strings{"24+48 Broll_City @24"});
    CHECK(binPath(r, seq.bin) == "Day 3"); // X.1: the event is the project's bin
    REQUIRE(track(seq, "V3").clips.size() == 1);
    CHECK(track(seq, "V3").clips[0].name() == "Lower Third");
    CHECK(track(seq, "V3").clips[0].start() == frames(10, kRate23_976)); // 10 frames into Interview_A
    CHECK(track(seq, "V3").clips[0].duration() == frames(72, kRate23_976));
    CHECK(clips(seq, "A1") == Strings{"0+120 Interview_A @48", "360+48 Interview_A @200"});
    CHECK(clips(seq, "A2") == Strings{"228+180 Music @0"});
    CHECK(links(seq) == Strings{"V1@0+A1@0", "V1@360+A1@360"});
    CHECK(track(seq, "A2").clips[0].properties().value(ParamId::VolumeDb) == Approx(-9.0));

    REQUIRE(seq.markers.size() == 1);
    CHECK(seq.markers[0].name == "Check sync");
    CHECK(seq.markers[0].comment == "lips vs. audio");
    CHECK(seq.markers[0].start == frames(60, kRate23_976));
    CHECK(seq.markers[0].duration == frames(1, kRate23_976));

    const ImportReport& rep = r.report;
    CHECK(hasEntry(rep, O, ReportCategory::Transition,
                   "'Cross Dissolve' (24 frames, centred on the cut at 00:00:05:00) on V1 -> crossDissolve"));
    // The audio crossfade: nothing on A1 after the cut, so it fades the outgoing audio out.
    CHECK(hasEntry(rep, A, ReportCategory::Transition, "on A1 -> constantPower; nothing follows the cut"));
    CHECK(hasEntry(rep, O, ReportCategory::Speed, "'Broll_City' on V1 plays at 50%"));
    CHECK(hasEntry(rep, O, ReportCategory::Clip, "title 'Dana Okafor' on V3"));
    // The sequence keeps its 48-frame trailing gap as its explicit duration.
    CHECK(hasEntry(rep, O, ReportCategory::Gap,
                   "trailing gap of 48 frames after the last clip (the sequence runs to 00:00:19:00)"));
    CHECK(seq.contentEnd() == frames(408, kRate23_976));
    CHECK(seq.duration == frames(456, kRate23_976));
    CHECK(hasEntry(rep, A, ReportCategory::Marker, "'Check sync' at 00:00:02:12"));
    const ReportEntry& gap = entry(rep, O, ReportCategory::Gap, "gap on V1 at 00:00:09:00 (48 frames)");
    CHECK(gap.location.element == "/fcpxml/library/event/project/sequence/spine/gap");

    CHECK(mediaNames(r) == Strings{"Interview_A", "Broll_City", "Music", "Dana Okafor"}); // X.1: and the title
    const MediaItem& interview = mediaNamed(r, "Interview_A");
    CHECK(interview.locator.path == "/Users/editor/Movies/Short Film/Interview_A.mov");
    CHECK(interview.info.duration == frames(360, kRate23_976));
    REQUIRE(interview.info.audio.size() == 1);
    CHECK(interview.info.audio[0].channels == 2);
    CHECK(mediaNamed(r, "Music").locator.path == "/Users/editor/Music/Score v2/Music.m4a");
    CHECK(mediaNamed(r, "Broll_City").info.audio.empty());
}

TEST_CASE("interchange: FCPXML 1.11 bundle: clip containers, storylines, compound, multicam, audition",
          "[interchange][fcpxml]") {
    const ImportResult r = importFixture("Wedding Teaser.fcpxmld");
    REQUIRE(r.sequences.size() == 3); // the project, then its compound clip and multicam sources (C.8)
    const Sequence& seq = sequenceAt(r, 0);
    CHECK(seq.name == "Wedding Teaser");
    CHECK(seq.frameRate == kRate29_97);
    CHECK(seq.dropFrame);
    // tcStart 01:00:00;00 is the timeline's origin.
    CHECK(clips(seq, "V1") == Strings{"0+150 Clip A @30", "150+60 Compound 1 @0", "240+60 Multicam 1 @0",
                                      "300+90 Take 1 @0"});
    CHECK(clips(seq, "V2") == Strings{"30+60 Insert 1 @0", "90+60 Insert 2 @120"});
    // The compound has no sound; the multicam clip plays Cam A's.
    CHECK(clips(seq, "A1") == Strings{"0+150 Clip A @30", "240+60 Multicam 1 @0", "300+90 Take 1 @0"});
    CHECK(links(seq) == Strings{"V1@0+A1@0", "V1@240+A1@240", "V1@300+A1@300"});

    const Sequence& compound = sequenceAt(r, 1);
    CHECK(compound.name == "Compound 1");
    CHECK(clips(compound, "V1") == Strings{"0+60 Insert @0"});
    CHECK(track(seq, "V1").clips[1].sequence == compound.id);
    const Sequence& multicam = sequenceAt(r, 2);
    REQUIRE(multicam.multicam);
    REQUIRE(multicam.multicam->angles.size() == 1);
    CHECK(multicam.multicam->angles[0].name == "Cam A");
    CHECK(clips(multicam, "V1") == Strings{"0+900 Interview_B @0"});
    CHECK(track(seq, "V1").clips[2].sequence == multicam.id);
    CHECK(track(seq, "V1").clips[2].angles == AngleSet::of(0));

    const ImportReport& rep = r.report;
    CHECK(hasEntry(rep, O, ReportCategory::SequenceSetting, "start timecode 01:00:00;00")); // X.1: kept
    CHECK(hasEntry(rep, O, ReportCategory::Clip, "nested sequence 'Compound 1' on V1"));
    CHECK(hasEntry(rep, A, ReportCategory::Clip, "'Compound 1' on V1 is longer than its sequence (60 frames)"));
    CHECK(hasEntry(rep, O, ReportCategory::Clip, "multicam clip 'Multicam 1' on V1"));
    CHECK(hasEntry(rep, O, ReportCategory::SequenceSetting, "multicam source with 1 angles"));
    CHECK(hasEntry(rep, A, ReportCategory::Clip, "audition: only the active pick 'Take 1'"));
    CHECK(
        hasEntry(rep, O, ReportCategory::Transition, "(30 frames, centred on the cut at 00:00:03;00) on V2"));
    // The relative media URL is offline until the bundle has its Media folder; the locator keeps where it
    // belongs, absolute and relative to a project saved next to the bundle.
    const MediaLocator& locator = mediaNamed(r, "Interview_B").locator;
    CHECK(std::filesystem::path(locator.path) ==
          (fixturePath("Wedding Teaser.fcpxmld") / "Media" / "Interview_B.mov").lexically_normal());
    CHECK(locator.relativePath == "Media/Interview_B.mov");
    CHECK(locator.offline);
}

TEST_CASE("interchange: FCPXML: a relative media URL resolves against the bundle",
          "[interchange][fcpxml][relink]") {
    const auto dir = scratchDir("fcpxml-bundle") / "Wedding Teaser.fcpxmld";
    writeFile(dir / "Info.fcpxml", readFixture("Wedding Teaser.fcpxmld/Info.fcpxml"));
    writeFile(dir / "Media" / "Interview_B.mov", "x");
    const auto r = ImportRouter::importFile(dir);
    REQUIRE(r);
    const MediaItem* interview = nullptr;
    for (const MediaId id : r->media) {
        if (r->project.findMedia(id)->name == "Interview_B") {
            interview = r->project.findMedia(id);
        }
    }
    REQUIRE(interview);
    CHECK(std::filesystem::equivalent(std::filesystem::path(interview->locator.path),
                                      dir / "Media" / "Interview_B.mov"));
    CHECK(r->unresolvedMedia.size() == 1); // Insert.mov on /Volumes/Shoot
    CHECK(hasEntry(r->report, O, ReportCategory::Media, "'Interview_B' found next to the imported file"));
}

namespace {

std::string fcpxml(const std::string& spine, const std::string& extraResources = {}) {
    return R"(<?xml version="1.0" encoding="UTF-8"?>
<fcpxml version="1.9"><resources>
<format id="r1" frameDuration="100/2500s" width="1280" height="720"/>
<asset id="a" name="A" start="0s" duration="100s" hasVideo="1" hasAudio="1" audioChannels="2" format="r1"><media-rep kind="original-media" src="file:///m/A.mov"/></asset>
<asset id="b" name="B" start="10s" duration="100s" hasVideo="1" format="r1" src="file:///m/B.mov"/>
)" + extraResources +
           R"(</resources><project name="P"><sequence format="r1" tcStart="0s"><spine>)" + spine +
           "</spine></sequence></project></fcpxml>";
}

} // namespace

TEST_CASE("interchange: FCPXML: connected clips of one lane that overlap are spread over tracks",
          "[interchange][fcpxml]") {
    // Two primary clips each carry a lane-1 clip; the second one starts before the first ends.
    const auto r = importText(fcpxml(R"(
        <asset-clip ref="a" offset="0s" duration="4s" start="0s"><asset-clip ref="b" lane="1" offset="2s" duration="3s" start="10s"/></asset-clip>
        <asset-clip ref="a" offset="4s" duration="4s" start="0s"><asset-clip ref="b" lane="1" offset="0s" duration="2s" start="20s"/></asset-clip>)"),
                              Format::FcpXml);
    const Sequence& seq = sequenceAt(r, 0);
    CHECK(seq.frameRate == kRate25);
    REQUIRE(seq.videoTracks.size() == 3);
    CHECK(clips(seq, "V2") == Strings{"50+75 B @0"});
    CHECK(clips(seq, "V3") == Strings{"100+50 B @250"});
    CHECK(hasEntry(r.report, A, ReportCategory::Track,
                   "connected clips of lane 1 overlap; they were spread over 2 tracks"));
}

TEST_CASE("interchange: FCPXML: clips below the primary storyline composite under it",
          "[interchange][fcpxml]") {
    const auto r = importText(
        fcpxml(
            R"(<asset-clip ref="a" offset="0s" duration="4s"><asset-clip ref="b" lane="-1" offset="1s" duration="1s" start="10s"/></asset-clip>)"),
        Format::FcpXml);
    const Sequence& seq = sequenceAt(r, 0);
    REQUIRE(seq.videoTracks.size() == 2);
    CHECK(clips(seq, "V1") == Strings{"25+25 B @0"});
    CHECK(clips(seq, "V2") == Strings{"0+100 A @0"});
}

TEST_CASE("interchange: FCPXML: 1.8 asset src, missing start, effects and unknown references",
          "[interchange][fcpxml]") {
    const auto r = importText(fcpxml(R"(
        <asset-clip ref="b" offset="0s" duration="2s" name="no start"/>
        <asset-clip ref="a" offset="2s" duration="2s" start="1s"><adjust-transform scale="1.5 1.5" rotation="90"/><adjust-blend amount="0.5"/><filter-video ref="fx" name="Gaussian Blur"/></asset-clip>
        <video ref="gen" offset="4s" duration="1s" name="Solid"/>
        <asset-clip ref="nope" offset="5s" duration="1s" name="Ghost"/>)",
                                     R"(<effect id="gen" name="Custom" uid="g"/>)"),
                              Format::FcpXml);
    const Sequence& seq = sequenceAt(r, 0);
    // Without a start attribute a clip begins at its asset's first frame (asset b starts at 10s).
    CHECK(clips(seq, "V1") == Strings{"0+50 no start @0", "50+50 A @25"});
    CHECK(mediaNamed(r, "B").locator.path == "/m/B.mov");
    const ClipProperties& p = track(seq, "V1").clips[1].properties();
    CHECK(p.value(ParamId::Scale) == Approx(1.5));
    CHECK(p.value(ParamId::Rotation) == Approx(-90.0));
    CHECK(p.value(ParamId::Opacity) == Approx(0.5));
    CHECK(hasEntry(r.report, D, ReportCategory::Effect, "effect 'Gaussian Blur' on 'A'"));
    CHECK(hasEntry(r.report, D, ReportCategory::Clip, "generator 'Solid'"));
    CHECK(hasEntry(r.report, D, ReportCategory::Clip, "'Ghost' refers to a missing asset"));
}

TEST_CASE("interchange: FCPXML: malformed documents are errors", "[interchange][fcpxml]") {
    CHECK(ImportRouter::importData("<fcpxml version=\"1.10\"><resources/></fcpxml>", Format::FcpXml, "x")
              .error()
              .code == ErrorCode::ParseError);
    CHECK(ImportRouter::importData("<fcpxml><project name=\"p\"><sequence/></project></fcpxml>",
                                   Format::FcpXml, "x")
              .error()
              .code == ErrorCode::ParseError);
    CHECK_FALSE(ImportRouter::importData("<fcpxml><resources>", Format::FcpXml, "x"));
    CHECK_FALSE(ImportRouter::importData(readFixture("premiere_2997df.xml"), Format::FcpXml, "x"));
    // Garbage times drop the element; they do not crash or produce negative clips.
    const auto r = importText(
        fcpxml(
            R"(<asset-clip ref="a" offset="zz" duration="-4s"/><asset-clip ref="a" offset="0s" duration="1/0s"/>)"),
        Format::FcpXml);
    CHECK(clips(sequenceAt(r, 0), "V1").empty());
}

TEST_CASE("interchange: FCPXML: a speed ramp's time map becomes a time remap", "[interchange][fcpxml][remap]") {
    // 4 s on the timeline: 2 s at 100 % from media 0, then 4 s of media in 2 s (200 %).
    const auto map = [](const char* interp) {
        return std::string(R"(<asset-clip ref="a" offset="0s" duration="4s" start="0s"><timeMap>)") +
               R"(<timept time="0s" value="0s" interp="linear"/><timept time="2s" value="2s" interp=")" + interp +
               R"("/><timept time="4s" value="6s" interp="linear"/></timeMap></asset-clip>)";
    };
    const auto r = importText(fcpxml(map("linear")), Format::FcpXml);
    const Sequence& seq = sequenceAt(r, 0);
    for (const Clip& c : {track(seq, "V1").clips[0], track(seq, "A1").clips[0]}) {
        REQUIRE(c.isRemapped());
        CHECK(c.duration() == Ticks{kTicksPerSecond * 4});
        CHECK(c.sourceTimeAt(Ticks{kTicksPerSecond}) == Ticks{kTicksPerSecond});
        CHECK(c.sourceTimeAt(Ticks{kTicksPerSecond * 3}) == Ticks{kTicksPerSecond * 4});
    }
    CHECK(hasEntry(r.report, O, ReportCategory::Speed, "has variable speed: a time remap with 3 keys"));
    // X.1: a smooth point is a Bezier key: the speed ramps from 100 % to 200 % through it (monotone tangents).
    const auto smooth = importText(fcpxml(map("smooth2")), Format::FcpXml);
    const Clip& eased = track(sequenceAt(smooth, 0), "V1").clips[0];
    REQUIRE(eased.isRemapped());
    REQUIRE(eased.timeRemap()->keys.size() == 3);
    CHECK(eased.timeRemap()->keys[1].interpolation == Interpolation::Bezier);
    CHECK(eased.timeRemap()->keys[1].out.slope == Approx(4.0 / 3.0 * static_cast<double>(kTicksPerSecond)));
    CHECK(eased.sourceTimeAt(Ticks{kTicksPerSecond * 2}) == Ticks{kTicksPerSecond * 2});
    CHECK(eased.sourceTimeAt(Ticks{kTicksPerSecond}) != Ticks{kTicksPerSecond}); // no longer a straight line
    CHECK(hasEntry(smooth.report, A, ReportCategory::Speed, "smooth speed ramps were imported as Bezier keys"));
}

TEST_CASE("interchange: FCPXML: events and keyword collections become bins", "[interchange][fcpxml]") {
    // X.1: the project is in its event's bin; the event's browser clips put their media there, or in the bin of
    // their keyword collection (one bin per media item: the first keyword).
    const auto r = importText(R"(<?xml version="1.0" encoding="UTF-8"?>
<fcpxml version="1.10"><resources>
<format id="r1" frameDuration="100/2500s" width="1280" height="720"/>
<asset id="a" name="A" start="0s" duration="100s" hasVideo="1" format="r1"><media-rep kind="original-media" src="file:///m/A.mov"/></asset>
<asset id="b" name="B" start="0s" duration="100s" hasVideo="1" format="r1"><media-rep kind="original-media" src="file:///m/B.mov"/></asset>
<asset id="c" name="C" start="0s" duration="100s" hasVideo="1" format="r1"><media-rep kind="original-media" src="file:///m/C.mov"/></asset>
</resources><library><event name="Shoot Day">
<keyword-collection name="Interviews"/><keyword-collection name="Selects"/>
<asset-clip ref="a" offset="0s" duration="10s" name="A"><keyword start="0s" duration="10s" value="Interviews, Selects"/></asset-clip>
<asset-clip ref="b" offset="0s" duration="10s" name="B"/>
<project name="Cut 1"><sequence format="r1" tcStart="0s"><spine>
<asset-clip ref="a" offset="0s" duration="2s"/><asset-clip ref="c" offset="2s" duration="2s"/>
</spine></sequence></project>
</event></library></fcpxml>)",
                              Format::FcpXml);
    CHECK(binPath(r, sequenceAt(r, 0).bin) == "Shoot Day");
    CHECK(binPath(r, mediaNamed(r, "A").bin) == "Shoot Day/Interviews");
    CHECK(binPath(r, mediaNamed(r, "B").bin) == "Shoot Day");
    CHECK(binPath(r, mediaNamed(r, "C").bin).empty()); // not in the event's browser
    CHECK(hasEntry(r.report, A, ReportCategory::Media, "went into the bin of the first ('Interviews')"));
}

TEST_CASE("Review P2-25: FCPXML time maps out of range are refused before anything evaluates them",
          "[interchange][fcpxml][remap][review]") {
    const auto map = [](const std::string& points) {
        return std::string(R"(<asset-clip ref="a" offset="0s" duration="4s" start="0s"><timeMap>)") + points +
               "</timeMap></asset-clip>";
    };
    // A span past the tick range (the differences would overflow), then a negative media time.
    for (const std::string& points :
         {std::string(R"(<timept time="-9223372036854775807/254016000000s" value="0s"/><timept time="0s" value="0s"/>)"
                      R"(<timept time="9223372036854775807/254016000000s" value="1s"/>)"),
          std::string(R"(<timept time="0s" value="-2s"/><timept time="2s" value="2s"/><timept time="4s" value="9s"/>)")}) {
        INFO(points);
        const auto r = importText(fcpxml(map(points)), Format::FcpXml);
        const Clip& c = track(sequenceAt(r, 0), "V1").clips[0];
        CHECK_FALSE(c.isRemapped());
        CHECK(c.speed == Speed{1, 1});
        CHECK(hasEntry(r.report, D, ReportCategory::Speed, "has a time map out of range"));
    }
}

TEST_CASE("Review P2-25: remap keys moved to the media's first frame are reported as approximated",
          "[interchange][fcpxml][remap][review]") {
    // B starts at 10 s in its file; the map starts 2 s before that (the clip itself shows from 2 s on).
    const auto r = importText(fcpxml(R"(<asset-clip ref="b" offset="0s" duration="4s" start="2s"><timeMap>)"
                                     R"(<timept time="0s" value="8s"/><timept time="2s" value="10s"/>)"
                                     R"(<timept time="6s" value="18s"/></timeMap></asset-clip>)"),
                              Format::FcpXml);
    const Clip& c = track(sequenceAt(r, 0), "V1").clips[0];
    REQUIRE(c.isRemapped());
    CHECK(hasEntry(r.report, A, ReportCategory::Speed, "keys before the start of the media were moved to its first frame"));
    CHECK_FALSE(hasEntry(r.report, O, ReportCategory::Speed, "has variable speed"));
}

TEST_CASE("interchange: FCPXML: compound and multicam clips play sequences of their own",
          "[interchange][fcpxml][multicam]") {
    const std::string resources =
        R"(<media id="c1" name="Comp"><sequence format="r1" duration="4s" tcStart="0s"><spine>)"
        R"(<asset-clip ref="a" offset="0s" duration="4s" start="2s"/></spine></sequence></media>)"
        R"(<media id="m1" name="MC"><multicam format="r1" tcStart="0s">)"
        R"(<mc-angle name="Wide" angleID="w"><asset-clip ref="a" offset="0s" duration="10s"/></mc-angle>)"
        R"(<mc-angle name="Close" angleID="c"><gap offset="0s" duration="1s"/>)"
        R"(<asset-clip ref="a" offset="1s" duration="9s" start="20s"/></mc-angle></multicam></media>)";
    const auto r = importText(
        fcpxml(R"(<ref-clip ref="c1" offset="0s" duration="2s" start="1s"/><ref-clip ref="c1" offset="2s" duration="1s"/>)"
               R"(<mc-clip ref="m1" offset="3s" duration="5s" start="2s"><mc-source angleID="c" srcEnable="video"/>)"
               R"(<mc-source angleID="w" srcEnable="audio"/></mc-clip>)",
               resources),
        Format::FcpXml);
    REQUIRE(r.sequences.size() == 3);
    const Sequence& seq = sequenceAt(r, 0);
    const Sequence& comp = sequenceAt(r, 1);
    const Sequence& mc = sequenceAt(r, 2);
    // One compound, used twice; its sound comes along.
    CHECK(clips(comp, "V1") == Strings{"0+100 A @50"});
    CHECK(clips(seq, "V1") == Strings{"0+50 Comp @25", "50+25 Comp @0", "75+125 MC @50"});
    CHECK(clips(seq, "A1") == Strings{"0+50 Comp @25", "50+25 Comp @0", "75+125 MC @50"});
    CHECK(track(seq, "V1").clips[1].sequence == comp.id);
    CHECK(links(seq) == Strings{"V1@0+A1@0", "V1@50+A1@50", "V1@75+A1@75"});
    // The multicam clip shows Close and plays Wide's sound; the angles keep their sync.
    REQUIRE(mc.multicam);
    REQUIRE(mc.multicam->angles.size() == 2);
    CHECK(mc.multicam->angles[1].name == "Close");
    CHECK(mc.multicam->angles[1].offset == frames(25, kRate25));
    CHECK(clips(mc, "V2") == Strings{"25+225 A @500"});
    CHECK(track(seq, "V1").clips[2].angles == AngleSet::of(1));
    CHECK(track(seq, "A1").clips[2].angles == AngleSet::of(0));
    CHECK(r.report.count(ReportOutcome::Dropped) == 0);
}

TEST_CASE("interchange: FCPXML: audio-channel-source becomes the clip's channel map (A.9)", "[interchange][fcpxml][channels]") {
    const auto r = importText(
        fcpxml(R"(
        <asset-clip ref="a" offset="0s" duration="2s"><audio-channel-source srcCh="2" outCh="C"/></asset-clip>
        <asset-clip ref="a" offset="2s" duration="2s"><audio-channel-source srcCh="1, 2" outCh="R, L"/></asset-clip>
        <asset-clip ref="s" offset="4s" duration="2s"><audio-channel-source srcCh="1, 2, 3, 4, 5, 6" outCh="L, R, C, LFE, Ls, Rs"/></asset-clip>
        <asset-clip ref="a" offset="6s" duration="2s"><audio-channel-source srcCh="1"/><audio-channel-source srcCh="2"/></asset-clip>
        <asset-clip ref="a" offset="8s" duration="2s"><audio-channel-source srcCh="1, 2" active="0"/></asset-clip>
        <asset-clip ref="a" offset="10s" duration="2s"/>)",
               R"(<asset id="s" name="S" start="0s" duration="100s" hasAudio="1" audioChannels="6" format="r1"><media-rep kind="original-media" src="file:///m/S.wav"/></asset>)"),
        Format::FcpXml);
    const Sequence& seq = sequenceAt(r, 0);
    const Track& a1 = track(seq, "A1");
    REQUIRE(a1.clips.size() == 6);
    const auto mapOf = [&](std::size_t i) { return a1.clips[i].channelMap(); };
    REQUIRE(mapOf(0));
    CHECK(*mapOf(0) == AudioChannelMap::mono(1));
    REQUIRE(mapOf(1));
    CHECK(*mapOf(1) == AudioChannelMap::swap());
    REQUIRE(mapOf(2));
    CHECK(*mapOf(2) == AudioChannelMap::identity(6));
    REQUIRE(mapOf(3));
    CHECK(mapOf(3)->format == ChannelFormat::Mono);
    CHECK(mapOf(3)->channels[0].sources == std::vector<std::int32_t>{0, 1});
    REQUIRE(mapOf(4));
    CHECK(mapOf(4)->channels[0].muted);
    CHECK(mapOf(4)->channels[1].muted);
    CHECK(mapOf(5) == nullptr); // no channel source: the whole stream
    CHECK(hasEntry(r.report, O, ReportCategory::AudioMapping, "'A' plays source channels 2 as a mono clip"));
    CHECK(hasEntry(r.report, O, ReportCategory::AudioMapping, "'S' plays source channels 1, 2, 3, 4, 5, 6 as a 5.1 clip"));
    CHECK(hasEntry(r.report, A, ReportCategory::AudioMapping,
                   "'A' has 2 audio components; they became one mono channel of their sources averaged"));
    // The video clips have no map.
    CHECK(track(seq, "V1").clips[0].channelMap() == nullptr);
}
