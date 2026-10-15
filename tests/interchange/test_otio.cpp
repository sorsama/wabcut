// OpenTimelineIO JSON: the Resolve-style fixture, then targeted cases built inline.

#include "InterchangeTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace wabcut;
using namespace wabcut::interchange;
using namespace wabcut::interchange::test;
using Json = nlohmann::json;

namespace {
constexpr auto O = ReportOutcome::Carried;
constexpr auto A = ReportOutcome::Approximated;
constexpr auto D = ReportOutcome::Dropped;
} // namespace

TEST_CASE("interchange: OTIO from Resolve: tracks, gaps, transitions, links, markers",
          "[interchange][otio]") {
    const ImportResult r = importFixture("resolve_timeline.otio");
    CHECK(r.format == Format::Otio);
    REQUIRE(r.sequences.size() == 2); // C.8: the nested stack 'Compound 1' is a sequence of its own
    const Sequence& seq = sequenceAt(r, 0);
    CHECK(seq.name == "Resolve Cut v12");
    CHECK(seq.frameRate == kRate23_976); // from the 23.976023976023978 rate of the times
    REQUIRE(seq.videoTracks.size() == 3);
    REQUIRE(seq.audioTracks.size() == 4);
    // Explicit gaps became implicit ones; available_range.start_time (01:00:00:00) was subtracted.
    CHECK(clips(seq, "V1") == Strings{"0+96 Shot_010 @48", "96+72 Shot_020 @24", "216+50 Shot_030 @10"});
    CHECK(clips(seq, "V2") == Strings{"30+60 Graphic_Overlay @0 off"});
    CHECK(clips(seq, "V3") == Strings{"100+40 Compound 1 @0", "140+30 Shot_040 @0"}); // C.8: the nested stack
    CHECK(clips(seq, "A1") == Strings{"0+96 Shot_010 @48", "96+72 Shot_020 @24"});
    CHECK(clips(seq, "A2") == Strings{"0+336 Music_Score @0"}); // 672672 samples at 48 kHz
    CHECK(clips(seq, "A3") == Strings{"24+120 Room_Tone @0"});
    CHECK(clips(seq, "A4") == Strings{"200+12 SFX_Door @0"});
    // OTIO has no links: same media, range and source In on video and audio are linked.
    CHECK(links(seq) == Strings{"V1@0+A1@0", "V1@96+A1@96"});
    CHECK(seq.videoTracks[0]->name == "V1");

    REQUIRE(seq.markers.size() == 3);
    CHECK(seq.markers[0].name == "Act 1");
    CHECK(seq.markers[0].colorIndex == 0); // green
    CHECK(seq.markers[1].name == "Reel break");
    CHECK(seq.markers[1].comment == "change reel");
    CHECK(seq.markers[1].start == frames(168, kRate23_976));
    CHECK(seq.markers[1].colorIndex == 6); // blue
    CHECK(seq.markers[2].name == "slam");
    CHECK(seq.markers[2].start == frames(204, kRate23_976));
    CHECK(seq.markers[2].colorIndex == 1); // red

    const ImportReport& rep = r.report;
    CHECK(hasEntry(rep, O, ReportCategory::Transition,
                   "'Cross Dissolve' (24 frames, centred on the cut at 00:00:04:00) on V1 -> crossDissolve"));
    CHECK(hasEntry(rep, O, ReportCategory::Transition, "on A1 -> constantPower"));
    CHECK(hasEntry(rep, O, ReportCategory::Speed, "'Shot_030' on V1 plays at 200%"));
    // C.8: the nested stack is a nested clip of its own sequence.
    CHECK(hasEntry(rep, O, ReportCategory::Clip, "nested sequence 'Compound 1' on V3"));
    const Sequence& compound = sequenceAt(r, 1);
    CHECK(compound.name == "Compound 1");
    CHECK(compound.videoTracks.size() == 1);
    CHECK(hasEntry(rep, O, ReportCategory::Gap, "trailing gap of 30 frames"));
    CHECK(seq.duration == frames(366, kRate23_976)); // V1's trailing gap
    CHECK(hasEntry(rep, O, ReportCategory::SequenceSetting, "start timecode 01:00:00:00")); // X.1: kept
    CHECK(hasEntry(rep, A, ReportCategory::SequenceSetting, "no frame size"));
    CHECK(hasEntry(rep, A, ReportCategory::Marker, "'slam' at 00:00:08:12"));
    const ReportEntry& gap = entry(rep, O, ReportCategory::Gap, "gap on V1 at 00:00:07:00 (48 frames)");
    CHECK(gap.location.element == "/tracks/children/0/children/3");
    CHECK(gap.location.line == 0);

    CHECK(mediaNamed(r, "Shot 030 (alt).mov").locator.path ==
          "/Users/editor/Projects/Film/Shot 030 (alt).mov");
    // MissingReference: offline, found only by name.
    REQUIRE(r.unresolvedMedia.size() == 8);
    CHECK(mediaNamed(r, "Shot_040.mov").locator.path.empty());
    CHECK(mediaNamed(r, "Shot_040.mov").locator.offline);
    CHECK(mediaNamed(r, "Shot_010.mov").info.duration == frames(480, kRate23_976));
}

TEST_CASE("interchange: OTIO: a MissingReference is found by its name under a search folder",
          "[interchange][otio][relink]") {
    const auto root = scratchDir("otio-missing");
    writeFile(root / "renders" / "Shot_040.mov", "x");
    ImportOptions options;
    options.mediaSearchRoots = {root};
    const ImportResult r = importFixture("resolve_timeline.otio", options);
    CHECK(r.unresolvedMedia.size() == 7);
    CHECK(std::filesystem::equivalent(std::filesystem::path(mediaNamed(r, "Shot_040.mov").locator.path),
                                      root / "renders" / "Shot_040.mov"));
}

namespace {

Json rt(double value, double rate = 25.0) {
    return {{"OTIO_SCHEMA", "RationalTime.1"}, {"rate", rate}, {"value", value}};
}
Json range(double start, double duration, double rate = 25.0) {
    return {
        {"OTIO_SCHEMA", "TimeRange.1"}, {"start_time", rt(start, rate)}, {"duration", rt(duration, rate)}};
}
Json clip(const std::string& name, const std::string& url, double start, double duration) {
    return {{"OTIO_SCHEMA", "Clip.1"},
            {"name", name},
            {"source_range", range(start, duration)},
            {"media_reference",
             {{"OTIO_SCHEMA", "ExternalReference.1"}, {"target_url", url}, {"available_range", nullptr}}}};
}
Json track(const std::string& kind, Json children) {
    return {{"OTIO_SCHEMA", "Track.1"}, {"kind", kind}, {"name", ""}, {"children", std::move(children)}};
}
Json timeline(const std::string& name, Json tracks) {
    return {{"OTIO_SCHEMA", "Timeline.1"},
            {"name", name},
            {"global_start_time", nullptr},
            {"tracks", {{"OTIO_SCHEMA", "Stack.1"}, {"children", std::move(tracks)}}}};
}

} // namespace

TEST_CASE("interchange: OTIO: collections, Clip.1, untrimmed clips and nested durations",
          "[interchange][otio]") {
    Json untrimmed = {{"OTIO_SCHEMA", "Clip.1"},
                      {"name", "whole"},
                      {"source_range", nullptr},
                      {"media_reference",
                       {{"OTIO_SCHEMA", "ExternalReference.1"},
                        {"target_url", "file:///m/w.mov"},
                        {"available_range", range(100, 20)}}}};
    Json nested = {
        {"OTIO_SCHEMA", "Stack.1"},
        {"name", "nest"},
        {"children", Json::array({track("Video", Json::array({clip("n1", "file:///m/n.mov", 0, 15)})),
                                  track("Video", Json::array({clip("n2", "file:///m/n.mov", 0, 30)}))})}};
    Json collection = {
        {"OTIO_SCHEMA", "SerializableCollection.1"},
        {"name", "bin"},
        {"children",
         Json::array(
             {timeline("First",
                       Json::array({track("Video", Json::array({untrimmed, nested,
                                                                clip("after", "file:///m/a.mov", 5, 10)}))})),
              timeline("Second", Json::array({track("Audio", Json::array({clip("vo", "vo.wav", 0, 25)}))})),
              {{"OTIO_SCHEMA", "Clip.1"}, {"name", "stray"}}})}};
    const auto r = importText(collection.dump(), Format::Otio);
    REQUIRE(r.sequences.size() == 3); // the two timelines, then the nested stack (C.8)
    const Sequence& first = sequenceAt(r, 0);
    CHECK(first.name == "First");
    CHECK(first.frameRate == kRate25);
    // The untrimmed clip plays its available range (source In 0 of the media); the nested stack lasts as
    // long as its longest track (30) and is a nested clip of its own sequence.
    CHECK(clips(first, "V1") == Strings{"0+20 whole @0", "20+30 nest @0", "50+10 after @5"});
    const Sequence& nest = sequenceAt(r, 2);
    CHECK(nest.name == "nest");
    CHECK(first.videoTracks[0]->clips[1].sequence == nest.id);
    CHECK(hasEntry(r.report, D, ReportCategory::Clip, "collection item 2 (Clip) is not a timeline"));
    const Sequence& second = sequenceAt(r, 1);
    CHECK(clips(second, "A1") == Strings{"0+25 vo @0"});
    CHECK(mediaNamed(r, "vo.wav").locator.path == "vo.wav"); // relative URL kept for the relinker
}

TEST_CASE("interchange: OTIO: odd and hostile values", "[interchange][otio]") {
    SECTION("unknown track kinds and reference types are reported") {
        Json gen = {
            {"OTIO_SCHEMA", "Clip.1"},
            {"name", "bars"},
            {"source_range", range(0, 10)},
            {"media_reference", {{"OTIO_SCHEMA", "GeneratorReference.1"}, {"generator_kind", "SMPTEBars"}}}};
        Json seqRef = {{"OTIO_SCHEMA", "Clip.1"},
                       {"name", "frames"},
                       {"source_range", range(0, 10)},
                       {"media_reference", {{"OTIO_SCHEMA", "ImageSequenceReference.1"}}}};
        const auto r = importText(timeline("t", Json::array({track("Data", Json::array()),
                                                             track("Video", Json::array({gen, seqRef}))}))
                                      .dump(),
                                  Format::Otio);
        CHECK(hasEntry(r.report, D, ReportCategory::Track, "kind 'Data'"));
        CHECK(hasEntry(r.report, D, ReportCategory::Clip, "generated media (SMPTEBars)"));
        CHECK(hasEntry(r.report, D, ReportCategory::Clip, "image sequences are not supported yet"));
        CHECK(clips(sequenceAt(r, 0), "V1").empty());
    }
    SECTION("fractional frames are rounded to ticks and reported") {
        const auto r = importText(
            timeline("t", Json::array({track("Video", Json::array({clip("c", "file:///m/c.mov", 0.5, 10)}))}))
                .dump(),
            Format::Otio);
        CHECK(hasEntry(r.report, A, ReportCategory::Clip, "rounded to the nearest tick"));
        CHECK(track(sequenceAt(r, 0), "V1").clips[0].sourceIn == Ticks{kTicksPerSecond / 50});
    }
    SECTION("Marker.3 colour objects") {
        Json c = clip("c", "file:///m/c.mov", 0, 10);
        Json stack = {{"OTIO_SCHEMA", "Stack.1"},
                      {"children", Json::array({track("Video", Json::array({c}))})},
                      {"markers", Json::array({{{"OTIO_SCHEMA", "Marker.3"},
                                                {"name", "m"},
                                                {"marked_range", range(2, 0)},
                                                {"color",
                                                 {{"OTIO_SCHEMA", "Color.1"},
                                                  {"name", ""},
                                                  {"r", 1.0},
                                                  {"g", 0.9},
                                                  {"b", 0.1},
                                                  {"a", 1.0}}}}})}};
        Json t = {{"OTIO_SCHEMA", "Timeline.1"}, {"name", "t"}, {"tracks", stack}};
        const auto r = importText(t.dump(), Format::Otio);
        REQUIRE(sequenceAt(r, 0).markers.size() == 1);
        CHECK(sequenceAt(r, 0).markers[0].colorIndex == 4); // yellow
    }
    SECTION("negative durations and NaN-free garbage do not produce clips") {
        Json bad = clip("bad", "file:///m/b.mov", 0, -10);
        Json noRange = {{"OTIO_SCHEMA", "Clip.1"}, {"name", "none"}, {"media_reference", nullptr}};
        Json typeConfused = {{"OTIO_SCHEMA", "Clip.1"}, {"name", 42}, {"source_range", "soon"}};
        const auto r = importText(
            timeline("t", Json::array({track("Video", Json::array({bad, noRange, typeConfused}))})).dump(),
            Format::Otio);
        CHECK(clips(sequenceAt(r, 0), "V1").empty());
        CHECK(r.report.count(D, ReportCategory::Clip) == 3);
    }
    SECTION("not OTIO") {
        CHECK(ImportRouter::importData("{\"OTIO_SCHEMA\": \"Clip.1\"}", Format::Otio, "x").error().code ==
              ErrorCode::ParseError);
        CHECK(ImportRouter::importData("{\"a\": 1}", Format::Otio, "x").error().code ==
              ErrorCode::ParseError);
        CHECK(ImportRouter::importData("{\"OTIO_SCHEMA\": \"Timeline.1\", \"tracks\": 7}", Format::Otio, "x")
                  .error()
                  .code == ErrorCode::ParseError);
        CHECK(ImportRouter::importData("[1, 2", Format::Otio, "x").error().code == ErrorCode::ParseError);
    }
}

TEST_CASE("interchange: OTIO: LinearTimeWarp and FreezeFrame become clip speeds", "[interchange][otio][fidelity]") {
    Json fast = clip("fast", "file:///m/f.mov", 10, 20);
    fast["effects"] = Json::array({{{"OTIO_SCHEMA", "LinearTimeWarp.1"}, {"name", "speed"}, {"time_scalar", 2.0}}});
    Json frozen = clip("frozen", "file:///m/z.mov", 5, 15);
    frozen["effects"] = Json::array({{{"OTIO_SCHEMA", "FreezeFrame.1"}, {"name", "hold"}, {"time_scalar", 0.0}}});
    Json third = clip("third", "file:///m/t.mov", 0, 30);
    third["effects"] = Json::array({{{"OTIO_SCHEMA", "LinearTimeWarp.1"}, {"name", "slow"}, {"time_scalar", 1.0 / 3.0}}});
    const auto r = importText(timeline("t", Json::array({track("Video", Json::array({fast, frozen, third}))})).dump(),
                              Format::Otio);
    const Track& v1 = track(sequenceAt(r, 0), "V1");
    REQUIRE(v1.clips.size() == 3);
    CHECK(v1.clips[0].speed == Speed{2, 1});
    CHECK(v1.clips[0].duration() == frames(20, kRate25));
    CHECK(v1.clips[0].sourceIn == frames(10, kRate25));
    CHECK(v1.clips[1].isFreeze());
    CHECK(v1.clips[2].speed == Speed{1, 3});
    CHECK(hasEntry(r.report, O, ReportCategory::Speed, "'fast' on V1 plays at 200%"));
    CHECK(hasEntry(r.report, O, ReportCategory::Speed, "'frozen' on V1 is a freeze frame"));
    CHECK(hasEntry(r.report, O, ReportCategory::Speed, "'third' on V1 plays at 33.33%"));
}

TEST_CASE("interchange: OTIO: non-linear time effects are reported and play at 100%", "[interchange][otio][remap]") {
    Json ramp = clip("ramp", "file:///m/r.mov", 0, 20);
    ramp["effects"] =
        Json::array({{{"OTIO_SCHEMA", "TimeEffect.1"}, {"name", "ramp"}, {"effect_name", "SpeedRamp"}}});
    const auto r = importText(timeline("t", Json::array({track("Video", Json::array({ramp}))})).dump(), Format::Otio);
    const Clip& c = track(sequenceAt(r, 0), "V1").clips[0];
    CHECK_FALSE(c.isRemapped());
    CHECK(c.speed == Speed{1, 1});
    CHECK(hasEntry(r.report, A, ReportCategory::Speed, "'ramp' has a non-linear time effect 'SpeedRamp'"));
}
