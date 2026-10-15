// The analysis commands (C.5) through the engine, with analyses handed over in memory: shot markers, split at shots,
// beat markers + grid, dry run / apply / undo, and the rhythm commands on the detected grid.

#include "CommandHarness.h"

#include <wabcut/ai/analysis/AnalysisCommands.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/Nesting.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::test;
using namespace wabcut::ai::analysis;
using command::Json;

namespace {

Json at(const char* track, int frame) { return Json{{"track", track}, {"time", tc(frame)}}; }

/// Shots of media A in its own time: cuts at media frames 20 and 45 (a dissolve).
ShotAnalysis shotsOfA() {
    ShotAnalysis a;
    a.frames = 1000;
    a.end = f(1000);
    a.cuts = {ShotCut{f(20), 20, CutKind::Cut, 0.98f}, ShotCut{f(45), 45, CutKind::Dissolve, 0.6f},
              ShotCut{f(400), 400, CutKind::Cut, 0.9f}};
    return a;
}

/// Beats of media M: 150 BPM (every 10 frames) from media frame 2, downbeats every 4.
BeatAnalysis beatsOfM() {
    BeatAnalysis b;
    b.bpm = 150.0;
    b.confidence = 0.95f;
    b.tracker = "dsp-1";
    for (int k = 0; k < 20; ++k) {
        b.beats.push_back(f(2 + 10 * k));
        if (k % 4 == 1) {
            b.downbeats.push_back(f(2 + 10 * k));
        }
    }
    return b;
}

struct Fixture {
    explicit Fixture(Timeline timeline) : provider(std::make_shared<MapAnalysisProvider>()), h(prepare(std::move(timeline)), policy(provider), ai::edit::editingCommands()) {
        provider->addShots(h.tl.media('A'), shotsOfA());
        provider->addBeats(h.tl.media('M'), beatsOfM());
    }
    static Timeline prepare(Timeline tl) {
        tl.linkMatching();
        return tl;
    }
    static command::EnginePolicy policy(const std::shared_ptr<MapAnalysisProvider>& p) {
        // Bundled like wabcut-cli does (transcripts would sit next to it).
        auto bundle = std::make_shared<command::ServiceBundle>(
            std::vector<std::shared_ptr<command::CommandServices>>{std::make_shared<AnalysisServices>(p)});
        return command::EnginePolicy{command::CapabilitySet::all(), 1024, bundle};
    }
    std::shared_ptr<MapAnalysisProvider> provider;
    CommandHarness h;
};

} // namespace

TEST_CASE("analysis.detectShots marks the cuts inside the clip", "[ai][analysis][commands]") {
    // A1/V1 play media A from frame 10 for 50 frames: the cuts at media 20 and 45 fall at timeline 10 and 35.
    Fixture fx(Timeline(R"(
        V1 tgt |A10aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt |A10aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )"));
    const ProjectSnapshot before = fx.h.state();
    const auto dry = fx.h.run(Json{{"command", "analysis.detectShots"}, {"params", Json{{"clip", at("V1", 0)}}}, {"dryRun", true}});
    REQUIRE(dry.ok());
    CHECK(*fx.h.state() == *before);
    const auto r = fx.h.ok("analysis.detectShots", Json{{"clip", at("V1", 0)}});
    CHECK(r.data["cuts"] == Json::array({Json::array({tc(10), 20, "cut", 0.98}), Json::array({tc(35), 45, "dissolve", 0.6})}));
    CHECK(r.data["markersAdded"] == 2);
    const Sequence& seq = *fx.h.state()->findSequence(fx.h.tl.seq);
    REQUIRE(seq.markers.size() == 2);
    CHECK(seq.markers[0].start == f(10));
    CHECK(seq.markers[0].name == "Shot 2");
    CHECK(fx.h.session.undoCount() == 1);
    fx.h.session.undo();
    CHECK(*fx.h.state() == *before);

    // Only listing them; a media item lists them in its own time.
    CHECK_FALSE(fx.h.ok("analysis.detectShots", Json{{"clip", at("V1", 0)}, {"markers", false}}).committed);
    CHECK(fx.h.ok("analysis.detectShots", Json{{"media", "A"}}).data["cuts"].size() == 3);
}

TEST_CASE("clip.splitAtShots razors at the cuts, linked partners too", "[ai][analysis][commands]") {
    Fixture fx(Timeline(R"(
        V1 tgt |A10aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt |A10aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )"));
    const ProjectSnapshot before = fx.h.state();
    const auto preview = fx.h.run(Json{{"command", "clip.splitAtShots"}, {"params", Json{{"clips", Json::array({at("V1", 0)})}}}, {"dryRun", true}});
    REQUIRE(preview.ok());
    CHECK(preview.changes.sequences[0].addedClips.size() == 4); // V1 and A1, two cuts each
    CHECK(*fx.h.state() == *before);
    fx.h.ok("clip.splitAtShots", Json{{"clips", Json::array({at("V1", 0)})}});
    CHECK_SESSION(fx.h, R"(
        V1 |A10aaaaaaaA20aaaaaaaaaaaaaaaaaaaaaaA45aaaaaaaaaaaa|
        A1 |A10aaaaaaaA20aaaaaaaaaaaaaaaaaaaaaaA45aaaaaaaaaaaa|
    )");
    fx.h.session.undo();
    CHECK(*fx.h.state() == *before);
}

TEST_CASE("analysis.detectBeats: beat markers 1-2-3-4 and the beat grid", "[ai][analysis][commands]") {
    // The music clip plays media M from frame 0 at timeline 5.
    Fixture fx(Timeline(R"(
        V1     |..............................................................|
        A1 tgt |.....Mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm|
    )"));
    const ProjectSnapshot before = fx.h.state();
    const auto r = fx.h.ok("analysis.detectBeats", Json{{"clip", at("A1", 10)}});
    CHECK(r.data["bpm"] == 150.0);
    CHECK(r.data["beatsInClip"] == 6); // media 2, 12, .., 52 -> timeline 7 .. 57
    CHECK(r.data["grid"] == true);
    CHECK(r.data["firstBeats"][0] == Json::array({tc(7), 4}));
    CHECK(r.data["firstBeats"][1] == Json::array({tc(17), 1}));
    const Sequence& seq = *fx.h.state()->findSequence(fx.h.tl.seq);
    REQUIRE(seq.markers.size() == 6);
    CHECK(seq.markers[1].name == "1");
    CHECK(seq.markers[1].colorIndex == 1); // downbeats red
    CHECK(seq.markers[2].name == "2");
    REQUIRE(seq.beatGrid);
    CHECK(seq.beatGrid->bpmMilli == 150000);
    CHECK(seq.beatGrid->phase == f(17));
    CHECK(seq.beatGrid->beats.size() == 6);
    CHECK(fx.h.session.undoCount() == 1); // markers and grid: one step
    fx.h.session.undo();
    CHECK(*fx.h.state() == *before);

    // Downbeat markers only, then the rhythm commands use the detected grid.
    fx.h.ok("analysis.detectBeats", Json{{"clip", at("A1", 10)}, {"markers", "downbeats"}});
    CHECK(fx.h.state()->findSequence(fx.h.tl.seq)->markers.size() == 2);
    const auto cut = fx.h.ok("edit.cutToBeats", Json{{"media", Json::array({"B", "C"})}, {"at", tc(0)}, {"beatsPerCut", 2}});
    CHECK(cut.data["placed"][0][2] == tc(7));
    CHECK(cut.data["placed"][0][3] == tc(27));
    CHECK(cut.data["placed"][1][3] == tc(47));

    // A media item: its beats in its own time, nothing edited.
    const auto media = fx.h.ok("analysis.detectBeats", Json{{"media", "M"}});
    CHECK(media.data["timeBase"] == "media");
    CHECK(media.data["firstBeats"][0] == Json::array({tc(2), 4}));
    CHECK_FALSE(media.committed);
}

TEST_CASE("Analysis commands need analysis services and media with the stream", "[ai][analysis][commands]") {
    CommandHarness plain(Timeline(R"(
        V1 tgt |Aaaaaaaaaa|
    )"),
                         {}, ai::edit::editingCommands());
    CHECK(plain.errorOf("analysis.detectShots", Json{{"clip", at("V1", 0)}}) == "InvalidRequest");
    Fixture fx(Timeline(R"(
        V1 tgt |Aaaaaaaaaa|
        A1 tgt |Aaaaaaaaaa|
    )"));
    CHECK(fx.h.errorOf("analysis.detectBeats", Json{{"clip", at("A1", 0)}}) == "EditRejected"); // no beats for A
    CHECK(fx.h.errorOf("analysis.detectShots", Json::object()) == "InvalidParams");
    CHECK(fx.h.errorOf("analysis.detectShots", Json{{"media", "A"}, {"detector", "nope"}}) == "InvalidParams");
    CHECK(timelineTimeOfSource(fx.h.state()->findSequence(fx.h.tl.seq)->findClip(fx.h.clip("V1@0"))->clip(), f(20)) ==
          std::nullopt); // past the clip's Out
}

TEST_CASE("clip.splitAtShots reads through a multicam clip and razors it in place (0.1.2)", "[ai][analysis][commands][multicam]") {
    // A one-angle multicam source of media A replaces V1/A1, playing source (= media) frames 10 to 60: A's cuts at
    // media 20 and 45 fall at timeline 10 and 35, as for the media clip itself.
    Fixture fx(Timeline(R"(
        V1 tgt |A10aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt |A10aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )"));
    SequenceId multicam;
    REQUIRE(fx.h.session.apply("Multicam", [&](const ProjectState& s) -> std::expected<ProjectState, Error> {
        edit::MulticamSpec spec;
        spec.angles = {fx.h.tl.media('A')};
        spec.sync = MulticamSync::Manual;
        auto mc = edit::createMulticam(s, spec);
        if (!mc) {
            return std::unexpected(mc.error());
        }
        multicam = mc->id;
        edit::SourceClip src;
        src.sequence = mc->id;
        src.sourceRange = TimeRange::fromStartEnd(f(10), f(60));
        src.patches = edit::defaultSourcePatches(*mc->state.findSequence(fx.h.tl.seq), *mc->state.findSequence(mc->id));
        src.videoAngle = AngleSet::of(0);
        src.audioAngles = AngleSet::of(0);
        auto placed = edit::overwriteEdit(mc->state, fx.h.tl.seq, src, Ticks{});
        if (!placed) {
            return std::unexpected(placed.error());
        }
        return std::move(placed->state);
    }));
    const auto v1 = [&] { return fx.h.state()->findSequence(fx.h.tl.seq)->findTrack(fx.h.tl.track("V1")); };
    REQUIRE(v1()->clips.size() == 1);
    const ClipId clip = v1()->clips.front().id;
    REQUIRE(v1()->clips.front().sequence == multicam);
    const ProjectSnapshot before = fx.h.state();

    const auto r = fx.h.ok("clip.splitAtShots", Json{{"sequence", fx.h.tl.seq.value}, {"clips", Json::array({clip.value})}});
    INFO(command::toJson(r).dump());
    CHECK(r.data["cuts"].size() == 2);
    const Track& track = *v1();
    REQUIRE(track.clips.size() == 3);
    CHECK(track.clips[0].start() == Ticks{});
    CHECK(track.clips[1].start() == f(10));
    CHECK(track.clips[2].start() == f(35));
    for (const Clip& piece : track.clips) {
        CHECK(piece.sequence == multicam); // still multicam clips
    }
    // Analysing the clip itself names the angle's media instead of failing on a missing media item.
    const auto shots =
        fx.h.run(Json{{"command", "analysis.detectShots"}, {"params", Json{{"sequence", fx.h.tl.seq.value}, {"clip", clip.value}}}});
    CHECK_FALSE(shots.ok());
    CHECK(command::toJson(shots).dump().find("multicam") != std::string::npos);
    fx.h.session.undo();
    CHECK(*fx.h.state() == *before);
}
