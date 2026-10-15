// Rhythm commands (C.5): sequence.setBeatGrid (tempo, taps, clear), sequence.beats, edit.snapToBeats and
// edit.cutToBeats through the engine.

#include "CommandHarness.h"

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::test;

namespace {

Json at(const char* track, int frame) { return Json{{"track", track}, {"time", tc(frame)}}; }

const Clip& clipOf(const CommandHarness& h, std::string_view ref) {
    static ProjectSnapshot keep;
    keep = h.state();
    return h.sequence(keep).findClip(clipAt(*keep, h.tl.seq, ref))->clip();
}

} // namespace

TEST_CASE("sequence.setBeatGrid: a tempo, tapped beats, or clear", "[command][rhythm]") {
    CommandHarness h(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    )");
    CHECK(h.errorOf("sequence.beats") == "EditRejected"); // no grid yet
    const auto set = h.ok("sequence.setBeatGrid", Json{{"bpm", 150}, {"phase", tc(2)}});
    CHECK(set.data["bpm"] == 150.0);
    CHECK(set.changes.sequences[0].beatGridChanged);
    const auto beats = h.ok("sequence.beats", Json{{"to", tc(30)}});
    CHECK(beats.data["beats"] == Json::array({Json::array({tc(2), 1, 0}), Json::array({tc(12), 2, 1}),
                                              Json::array({tc(22), 3, 2})}));

    // Taps every 0.48 s (125 BPM) starting at 00:00:00:05.
    const auto tapped = h.ok("sequence.setBeatGrid", Json{{"taps", Json::array({tc(5), tc(17), tc(29), tc(41)})}});
    CHECK(tapped.data["bpm"] == 125.0);
    CHECK(tapped.data["phase"] == tc(5));
    CHECK(h.state()->findSequence(h.tl.seq)->beatGrid->bpmMilli == 125000);

    const auto summary = h.ok("sequence.summary");
    CHECK(summary.data["beatGrid"]["bpm"] == 125.0);

    h.ok("sequence.setBeatGrid", Json{{"clear", true}});
    CHECK_FALSE(h.state()->findSequence(h.tl.seq)->beatGrid);
    CHECK(h.errorOf("sequence.setBeatGrid", Json{{"taps", Json::array({tc(5)})}}) == "InvalidParams");
    CHECK(h.errorOf("sequence.setBeatGrid", Json::object()) == "InvalidParams");
    CHECK(h.session.undoCount() == 3);
}

TEST_CASE("edit.snapToBeats trims clip edges to the nearest beats", "[command][rhythm]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaa.....B4bbbbbbbbbb........|
        A1 tgt |Aaaaaaaaa.....B4bbbbbbbbbb........|
    )");
    tl.linkMatching();
    CommandHarness h(std::move(tl));
    // A beat every 10 frames from frame 0. Within 4 frames: A's tail 9 -> 10, B's head 14 -> 10 (touching A), B's
    // tail 26 -> 30. Linked audio follows.
    h.ok("sequence.setBeatGrid", Json{{"bpm", 150}, {"phase", tc(0)}});
    const ProjectSnapshot before = h.state();
    const std::uint64_t a = clipAt(*before, h.tl.seq, "V1@0").value;
    const std::uint64_t b = clipAt(*before, h.tl.seq, "V1@14").value;
    const auto snapped =
        h.ok("edit.snapToBeats", Json{{"clips", Json::array({at("V1", 0), at("V1", 14)})}, {"tolerance", tc(4)}});
    CHECK(snapped.data["snapped"] == Json::array({Json::array({a, "tail", tc(9), tc(10), 2}),
                                                  Json::array({b, "head", tc(14), tc(10), 2}),
                                                  Json::array({b, "tail", tc(26), tc(30), 4})}));
    CHECK_SESSION(h, R"(
        V1 |AaaaaaaaaaBbbbbbbbbbbbbbbbbbbb....|
        A1 |AaaaaaaaaaBbbbbbbbbbbbbbbbbbbb....|
    )");
    CHECK(h.session.undoCount() == 2); // the grid, then one step for the snap
    h.session.undo();
    CHECK(*h.state() == *before);

    // Only heads, with the default tolerance (a quarter beat = 2.5 frames): nothing within reach.
    const auto heads = h.ok("edit.snapToBeats", Json{{"clips", Json::array({at("V1", 14)})}, {"edges", "head"}});
    CHECK(heads.data["snapped"].empty());
    CHECK_FALSE(heads.committed);
}

TEST_CASE("edit.cutToBeats assembles clips one per beat group from the playhead", "[command][rhythm]") {
    CommandHarness h(R"(
        V1 tgt |..............................................................|
        A1 tgt |Mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm|
    )");
    // 150 BPM from frame 4: groups of 2 beats = 20 frames, from the first beat after the playhead (frame 6): 14.
    h.ok("sequence.setBeatGrid", Json{{"bpm", 150}, {"phase", tc(4)}});
    const ProjectSnapshot before = h.state();
    const Json params{{"media", Json::array({"A", "B", "C"})}, {"at", tc(6)}, {"beatsPerCut", 2}};
    const auto dry = h.run(Json{{"command", "edit.cutToBeats"}, {"params", params}, {"dryRun", true}});
    REQUIRE(dry.ok());
    CHECK_FALSE(dry.committed);
    CHECK(*h.state() == *before);

    const auto cut = h.ok("edit.cutToBeats", params);
    CHECK(cut.createdIds.size() == 3);
    const std::string v1 = std::string(14, '.') + "A" + std::string(19, 'a') + "B" + std::string(19, 'b') + "C" +
                           std::string(19, 'c');
    CHECK_SESSION(h, "V1 |" + v1 + "|\nA1 |M" + std::string(61, 'm') + "|");
    CHECK(cut.data["placed"][1] == Json::array({clipAt(*h.state(), h.tl.seq, "V1@34").value, "B", tc(34), tc(54), 4}));
    h.session.undo();
    CHECK(*h.state() == *before);

    // From existing clips: each from its own source In, with its audio when audioTrack is given.
    const auto fromClips = h.ok("edit.cutToBeats", Json{{"clips", Json::array({at("A1", 0)})},
                                                        {"at", tc(0)},
                                                        {"beatsPerCut", 1},
                                                        {"videoTrack", "V1"},
                                                        {"audioTrack", "A1"}});
    CHECK(fromClips.data["placed"][0][2] == tc(4));
    CHECK(fromClips.data["placed"][0][3] == tc(14));
    CHECK(h.errorOf("edit.cutToBeats", Json{{"at", tc(0)}}) == "InvalidParams");
}

TEST_CASE("Rhythm edits need a beat grid", "[command][rhythm]") {
    CommandHarness h(R"(
        V1 tgt |Aaaaaaaaaa|
    )");
    CHECK(h.errorOf("edit.snapToBeats", Json{{"clips", Json::array({at("V1", 0)})}}) == "EditRejected");
    CHECK(h.errorOf("edit.cutToBeats", Json{{"media", Json::array({"A"})}, {"at", tc(0)}}) == "EditRejected");
}

TEST_CASE("Cut to beats rejects an audio-only sequence without changing it", "[command][rhythm]") {
    CommandHarness h(R"(
        A1 tgt |Aaaaaaaaaa|
    )");
    REQUIRE(h.sequence(h.state()).videoTracks.empty());
    h.ok("sequence.setBeatGrid", Json{{"bpm", 150}});
    const ProjectSnapshot before = h.state();
    const auto undos = h.session.undoCount();
    for (const bool dryRun : {false, true}) {
        for (const bool explicitAudioTrack : {false, true}) {
            Json params{{"media", Json::array({"A"})}, {"at", tc(0)}};
            if (explicitAudioTrack) {
                params["videoTrack"] = "A1";
            }
            const auto result = h.run(Json{{"command", "edit.cutToBeats"}, {"params", params}, {"dryRun", dryRun}});
            REQUIRE_FALSE(result.ok());
            CHECK(command::toString(result.error->code) == "InvalidParams");
            CHECK(*h.state() == *before);
            CHECK(h.session.undoCount() == undos);
        }
    }
}
