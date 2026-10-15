// The sequence beat grid (C.5): beat times, bars, nearest beats, explicit beats, validation, IO and snap targets.

#include "TimelineFixture.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/Snap.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace wabcut;
using namespace wabcut::test;

namespace {

BeatGrid regular(std::int64_t bpmMilli, Ticks phase, std::int32_t beatsPerBar = 4) {
    BeatGrid g;
    g.bpmMilli = bpmMilli;
    g.phase = phase;
    g.beatsPerBar = beatsPerBar;
    return g;
}

std::vector<Ticks> times(const std::vector<Beat>& beats) {
    std::vector<Ticks> out;
    for (const Beat& b : beats) {
        out.push_back(b.time);
    }
    return out;
}

} // namespace

TEST_CASE("A regular beat grid: exact beat times, bars and the nearest beat", "[model][beats]") {
    // 120 BPM = a beat every 12.5 frames at 25 fps; the downbeat at frame 10.
    const BeatGrid g = regular(120000, f(10));
    CHECK(g.period() == Ticks{kTicksPerSecond / 2});
    CHECK(g.bpm() == 120.0);
    const auto beats = g.beatsIn(TimeRange::fromStartEnd(Ticks{}, Ticks::fromSeconds(3)));
    REQUIRE(beats.size() == 6); // 0.4 0.9 1.4 1.9 2.4 2.9 s
    CHECK(beats.front().time == f(10));
    CHECK(beats.front().inBar == 1);
    CHECK(beats.front().isDownbeat());
    CHECK(beats[1].time == f(10) + Ticks{kTicksPerSecond / 2});
    CHECK(beats[1].inBar == 2);
    CHECK(beats[4].inBar == 1);
    CHECK(beats[4].index == 4);

    // Beats before the downbeat count backwards; none before 0.
    const BeatGrid early = regular(120000, Ticks::fromSeconds(2));
    const auto before = early.beatsIn(TimeRange::fromStartEnd(Ticks{}, Ticks::fromSeconds(2)));
    REQUIRE(before.size() == 4);
    CHECK(before.front().time == Ticks{});
    CHECK(before.front().index == -4);
    CHECK(before.front().inBar == 1);
    CHECK(before.back().inBar == 4);

    CHECK(g.nearestBeat(f(12))->time == f(10));
    CHECK(g.nearestBeat(f(17))->time == f(10) + Ticks{kTicksPerSecond / 2});
    CHECK(g.nearestBeat(Ticks{})->time == f(10)); // the first beat at or after 0 (one before it is negative)
    CHECK(g.beatAtOrAfter(f(10))->time == f(10));
    CHECK(g.beatAtOrAfter(f(11))->time == f(10) + Ticks{kTicksPerSecond / 2});

    // Odd tempos stay exact to the tick and never drift: beat k is phase + k x 60/bpm, rounded once.
    const BeatGrid odd = regular(174000, Ticks{});
    const auto many = odd.beatsIn(TimeRange::fromStartEnd(Ticks{}, Ticks::fromSeconds(600)));
    REQUIRE(many.size() == 1740);
    const auto expected = mulDiv(std::int64_t{1739}, 60 * kTicksPerSecond * 1000, 174000, Rounding::Nearest);
    CHECK(many.back().time.value == *expected);
}

TEST_CASE("Detected beats: the grid follows music that drifts", "[model][beats]") {
    BeatGrid g = regular(120000, f(20));
    g.beats = {f(8), f(20), f(33), f(45), f(58), f(70)};
    const auto beats = g.beatsIn(TimeRange::fromStartEnd(Ticks{}, f(100)));
    CHECK(times(beats) == g.beats);
    CHECK(beats[0].inBar == 4); // one before the downbeat
    CHECK(beats[1].inBar == 1);
    CHECK(beats[5].inBar == 1);
    CHECK(g.nearestBeat(f(39))->time == f(33));
    CHECK(g.nearestBeat(f(40))->time == f(45));
    CHECK(g.nearestBeat(f(500))->time == f(70));
    CHECK_FALSE(g.beatAtOrAfter(f(71)));
}

TEST_CASE("setBeatGrid validates, changes, removes; the grid survives the project file", "[model][beats][io]") {
    Timeline tl(R"(
        A1 |Aaaaaaaaaa|
    )");
    const auto set = ok(edit::setBeatGrid(tl.state, tl.seq, regular(128000, f(3), 3)));
    REQUIRE(set.state.findSequence(tl.seq)->beatGrid);
    CHECK(set.changes.sequence(tl.seq)->beatGridChanged);
    CHECK(ok(edit::setBeatGrid(set.state, tl.seq, regular(128000, f(3), 3))).changes.empty());

    CHECK_FALSE(edit::setBeatGrid(tl.state, tl.seq, regular(500, Ticks{})));      // 0.5 BPM
    CHECK_FALSE(edit::setBeatGrid(tl.state, tl.seq, regular(120000, Ticks{}, 0)));  // no beats in a bar
    CHECK_FALSE(edit::setBeatGrid(tl.state, tl.seq, regular(120000, f(-1))));       // downbeat before 0
    BeatGrid unsorted = regular(120000, f(2));
    unsorted.beats = {f(5), f(2)};
    CHECK_FALSE(edit::setBeatGrid(tl.state, tl.seq, unsorted));
    BeatGrid missing = regular(120000, f(3));
    missing.beats = {f(2), f(5)};
    CHECK_FALSE(edit::setBeatGrid(tl.state, tl.seq, missing)); // the downbeat is not a beat

    BeatGrid detected = regular(121500, f(2));
    detected.beats = {f(2), f(14), f(27)};
    const auto withBeats = ok(edit::setBeatGrid(set.state, tl.seq, detected));
    const auto json = io::toJson(withBeats.state);
    REQUIRE(json);
    CHECK(json->find("\"beatGrid\"") != std::string::npos);
    CHECK(json->find("\"schemaVersion\": " + std::to_string(io::kSchemaVersion)) != std::string::npos);
    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded);
    CHECK(*loaded == withBeats.state);
    CHECK(loaded->findSequence(tl.seq)->beatGrid == detected);

    const auto removed = ok(edit::setBeatGrid(withBeats.state, tl.seq, std::nullopt));
    CHECK_FALSE(removed.state.findSequence(tl.seq)->beatGrid);
    CHECK(removed.changes.sequence(tl.seq)->beatGridChanged);
}

TEST_CASE("Snapping targets the beat grid", "[model][beats][snap]") {
    Timeline tl(R"(
        V1 |Aaaaaaaaaa..........|
    )");
    // 150 BPM = a beat every 10 frames at 25 fps, from frame 3.
    const auto r = ok(edit::setBeatGrid(tl.state, tl.seq, regular(150000, f(3))));
    const Sequence& seq = *r.state.findSequence(tl.seq);
    const auto targets = edit::snapTargets(seq, std::nullopt);
    CHECK(std::ranges::find(targets, f(3)) != targets.end());
    CHECK(std::ranges::find(targets, f(13)) != targets.end());
    CHECK(std::ranges::find(targets, f(23)) != targets.end());
    CHECK(std::ranges::find(targets, f(43)) != targets.end()); // a bar past the end of the sequence
    CHECK(edit::nearestSnapTarget(targets, f(12), f(2)) == f(13));
    edit::SnapOptions noBeats;
    noBeats.beats = false;
    const auto plain = edit::snapTargets(seq, std::nullopt, noBeats);
    CHECK(std::ranges::find(plain, f(13)) == plain.end());
    CHECK(std::ranges::find(plain, f(10)) != plain.end()); // the clip's end
}
