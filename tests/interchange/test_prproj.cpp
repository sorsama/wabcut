// Experimental .prproj reader, checked against real Premiere Pro 26.3 projects and the ExtendScript dump of
// each one (tests/interchange/fixtures/prproj, from py-premiere, MIT).

#include "InterchangeTestSupport.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <map>
#include <nlohmann/json.hpp>

using namespace wabcut;
using namespace wabcut::interchange;
using namespace wabcut::interchange::test;
using Json = nlohmann::json;

namespace {

constexpr auto O = ReportOutcome::Carried;
constexpr auto A = ReportOutcome::Approximated;
constexpr auto D = ReportOutcome::Dropped;

ImportOptions experimental() {
    ImportOptions options;
    options.experimentalPremiereProject = true;
    return options;
}

std::int64_t ticks(const Json& t) {
    return std::stoll(t.at("ticks").get<std::string>());
}

} // namespace

TEST_CASE("interchange: prproj: every sequence, track, clip and marker matches Premiere's own report",
          "[interchange][prproj]") {
    const std::string name = GENERATE(as<std::string>{}, "07_transitions", "12_disabled", "20_track_lock",
                                      "23_merged_clip", "36_add_tracks", "37_retime");
    CAPTURE(name);
    const ImportResult r = importFixture("prproj/" + name + ".prproj", experimental());
    CHECK(r.format == Format::PremiereProject);
    const Json truth = Json::parse(readFixture("prproj/" + name + ".json"));

    std::map<std::string, const Sequence*> byName;
    for (std::size_t i = 0; i < r.sequences.size(); ++i) {
        byName[sequenceAt(r, i).name] = &sequenceAt(r, i);
    }
    REQUIRE(byName.size() == truth.at("sequences").size());
    for (const Json& ts : truth.at("sequences")) {
        const std::string seqName = ts.at("name").get<std::string>();
        CAPTURE(seqName);
        REQUIRE(byName.contains(seqName));
        const Sequence& seq = *byName[seqName];
        // Rate: Premiere's timebase is ticks per frame.
        CHECK(seq.frameRate.ticksPerUnit() == std::stoll(ts.at("timebase").get<std::string>()));
        CHECK(seq.width == ts.at("frameSizeHorizontal").get<int>());
        CHECK(seq.height == ts.at("frameSizeVertical").get<int>());
        for (const auto& [key, kind] :
             {std::pair{"videoTracks", TrackKind::Video}, std::pair{"audioTracks", TrackKind::Audio}}) {
            const Json& tracks = ts.at(key);
            REQUIRE(seq.tracks(kind).size() == tracks.size());
            for (std::size_t t = 0; t < tracks.size(); ++t) {
                const Track& track = *seq.tracks(kind)[t];
                const Json& tt = tracks[t];
                if (tt.contains("isLocked")) {
                    CHECK(track.locked == tt.at("isLocked").get<bool>());
                }
                // Expected clips: every clip, nested sequences too (X.1).
                std::vector<std::tuple<std::int64_t, std::int64_t, std::string>> want;
                for (const Json& c : tt.at("clips")) {
                    want.emplace_back(ticks(c.at("start")), ticks(c.at("end")),
                                      c.at("name").get<std::string>());
                }
                std::vector<std::tuple<std::int64_t, std::int64_t, std::string>> have;
                for (const Clip& c : track.clips) {
                    have.emplace_back(c.start().value, c.end().value, c.name());
                }
                std::sort(want.begin(), want.end());
                std::sort(have.begin(), have.end());
                CHECK(have == want);
                for (const Json& c : tt.at("clips")) {
                    for (const Clip& clip : track.clips) {
                        // Source In is the media position (stills report a timecode-like In and start at 0).
                        if (clip.start().value == ticks(c.at("start")) &&
                            ticks(c.at("inPoint")) < 100'000'000'000'000LL) {
                            CHECK(clip.sourceIn.value == ticks(c.at("inPoint")));
                        }
                    }
                }
            }
        }
        std::vector<std::pair<std::int64_t, std::string>> wantMarkers;
        for (const Json& m : ts.at("markers")) {
            wantMarkers.emplace_back(ticks(m.at("start")), m.at("name").get<std::string>());
        }
        std::vector<std::pair<std::int64_t, std::string>> haveMarkers;
        for (const Marker& m : seq.markers) {
            haveMarkers.emplace_back(m.start.value, m.name);
        }
        std::sort(wantMarkers.begin(), wantMarkers.end());
        CHECK(haveMarkers == wantMarkers);
    }
}

TEST_CASE("interchange: prproj: disabled clips, links, marker details and the report",
          "[interchange][prproj]") {
    const ImportResult r = importFixture("prproj/12_disabled.prproj", experimental());
    const Sequence& seqA = sequenceAt(r, 0);
    CHECK(seqA.name == "Seq A");
    CHECK(seqA.frameRate == kRate29_97);
    REQUIRE(track(seqA, "V1").clips.size() == 1);
    CHECK_FALSE(track(seqA, "V1").clips[0].enabled); // ClipTrackItem/IsMuted
    CHECK(mediaNamed(r, "red_64x36.bmp").info.isStillImage);
    CHECK(mediaNamed(r, "tone_440_1s.wav").locator.path ==
          "C:/Users/del-m/git/py-premiere/samples/models/assets/tone_440_1s.wav");
    // The sequence's In (0.5 s, between frames) is snapped to the start of its frame; a ranged marker.
    CHECK(seqA.markIn == frames(14, kRate29_97));
    CHECK(hasEntry(r.report, A, ReportCategory::SequenceSetting, "(snapped to frame boundaries)"));
    REQUIRE(seqA.markers.size() == 4);
    const auto ranged = std::find_if(seqA.markers.begin(), seqA.markers.end(),
                                     [](const Marker& m) { return m.name == "comment"; });
    REQUIRE(ranged != seqA.markers.end());
    CHECK(ranged->comment == "with end");
    CHECK(ranged->duration == Ticks{kTicksPerSecond});

    // X.1: Seq B plays Seq A as a nested clip (picture and mix, linked).
    const Sequence& seqB = sequenceAt(r, 1);
    CHECK(seqB.name == "Seq B");
    REQUIRE(track(seqB, "V1").clips.size() == 1);
    REQUIRE(track(seqB, "A1").clips.size() == 1);
    const Clip& nested = track(seqB, "V1").clips[0];
    CHECK(nested.isNested());
    CHECK(nested.sequence == seqA.id);
    CHECK(nested.sourceIn == Ticks{});
    CHECK(nested.duration() == Ticks{1508668761600});
    CHECK(track(seqB, "A1").clips[0].sequence == seqA.id);
    CHECK_FALSE(hasEntry(r.report, D, ReportCategory::Clip, "nested sequence"));
    CHECK(hasEntry(r.report, D, ReportCategory::Effect, "does not read effects"));
    CHECK(hasEntry(r.report, O, ReportCategory::SequenceSetting, "start timecode")); // X.1: MZ.ZeroPoint is kept
}

TEST_CASE("interchange: prproj: merged clips keep their A/V link", "[interchange][prproj]") {
    const ImportResult r = importFixture("prproj/23_merged_clip.prproj", experimental());
    const Sequence* merged = nullptr;
    for (std::size_t i = 0; i < r.sequences.size(); ++i) {
        if (sequenceAt(r, i).name == "bars_64x36_h264.mp4 - Merged") {
            merged = &sequenceAt(r, i);
        }
    }
    REQUIRE(merged);
    CHECK(merged->frameRate == kRate25);
    CHECK(links(*merged) == Strings{"V1@0+A1@0"});
}

TEST_CASE("interchange: prproj: a project without sequences still brings its media",
          "[interchange][prproj]") {
    const ImportResult r = importFixture("prproj/19_timecode.prproj", experimental());
    CHECK(r.sequences.empty());
    CHECK(mediaNames(r) == Strings{"bars_64x36_2997tc.mov"});
}

TEST_CASE("interchange: prproj: off by default, and corrupt projects are errors", "[interchange][prproj]") {
    const auto off = ImportRouter::importFile(fixturePath("prproj/12_disabled.prproj"));
    REQUIRE_FALSE(off);
    CHECK(off.error().code == ErrorCode::Unsupported);

    const std::string gz = readFixture("prproj/12_disabled.prproj");
    const auto truncated = ImportRouter::importData(std::string_view(gz).substr(0, gz.size() / 2),
                                                    Format::PremiereProject, "t.prproj", experimental());
    REQUIRE_FALSE(truncated);
    CHECK(truncated.error().code == ErrorCode::ParseError);

    // Uncompressed XML is accepted too (very old projects were not compressed).
    const auto plain = ImportRouter::importData(
        "<PremiereData Version=\"3\"><Sequence ObjectUID=\"s\"><Name>Empty</Name></Sequence></PremiereData>",
        Format::PremiereProject, "p.prproj", experimental());
    REQUIRE(plain);
    REQUIRE(plain->sequences.size() == 1);
    CHECK(plain->project.findSequence(plain->sequences[0])->name == "Empty");

    CHECK(ImportRouter::importData("<Other/>", Format::PremiereProject, "p.prproj", experimental())
              .error()
              .code == ErrorCode::ParseError);
    CHECK(ImportRouter::importData("<PremiereData/>", Format::PremiereProject, "p.prproj", experimental())
              .error()
              .code == ErrorCode::ParseError);
}
