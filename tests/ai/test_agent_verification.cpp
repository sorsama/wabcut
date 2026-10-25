// The editing assistant's deterministic checks (AI-5): verify() on staged results made with real catalog commands,
// and the constraints read from free-form requests.

#include "CommandHarness.h"

#include <wabcut/ai/agent/Preview.h>
#include <wabcut/ai/agent/Verification.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace wabcut;
using namespace wabcut::ai::agent;
using namespace wabcut::test;
using command::Json;

namespace {

// 30 frames at 25 fps: A 0-10, B 10-20, C 20-30 on V1 and A1.
constexpr std::string_view kThree = R"(
    V1 tgt |AaaaaaaaaaBbbbbbbbbbCccccccccc|
    A1 tgt |AaaaaaaaaaBbbbbbbbbbCccccccccc|
)";

Json ranges(int from, int to) { return Json{{"ranges", Json::array({Json{{"start", tc(from)}, {"end", tc(to)}}})}}; }

struct Checked {
    explicit Checked(std::string_view picture = kThree) : h(picture), before(h.state()) {}

    VerificationReport run(const Constraints& c, std::size_t commands = 1) const {
        return verify(*before, *h.state(), h.tl.seq, c, commands);
    }
    const VerificationItem& item(const VerificationReport& r, std::string_view id) const {
        const VerificationItem* i = r.item(id);
        INFO("no item " << id);
        REQUIRE(i != nullptr);
        return *i;
    }

    CommandHarness h;
    ProjectSnapshot before;
};

Constraints loose() {
    Constraints c;
    c.durationTolerance = f(1);
    return c;
}

} // namespace

TEST_CASE("Agent verification: the target length is checked within its tolerance", "[ai][agent][verify]") {
    Checked x;
    x.h.ok("timeline.removeSilences", ranges(10, 20)); // B goes: 20 frames left

    Constraints c = loose();
    c.targetDuration = f(20);
    auto report = x.run(c);
    CHECK(report.passed());
    CHECK(x.item(report, "duration").passed);

    c.targetDuration = f(25);
    report = x.run(c);
    CHECK_FALSE(report.passed());
    const auto& duration = x.item(report, "duration");
    CHECK_FALSE(duration.passed);
    CHECK(duration.detail.find("too short") != std::string::npos);
    CHECK(report.failuresForModel().find("Length is") != std::string::npos);

    Constraints max = loose();
    max.maxDuration = f(15);
    CHECK_FALSE(x.run(max).passed());
    max.maxDuration = f(20);
    CHECK(x.run(max).passed());
}

TEST_CASE("Agent verification: protected content may move but not be cut", "[ai][agent][verify]") {
    Constraints c;
    c.protectedRanges = {TimeRange::fromStartEnd(f(12), f(16))}; // inside B

    Checked moved;
    moved.h.ok("timeline.removeSilences", ranges(0, 5)); // B slides left by 5 frames
    CHECK(moved.run(c).passed());

    Checked cut;
    cut.h.ok("timeline.removeSilences", ranges(13, 15));
    const auto report = cut.run(c);
    CHECK_FALSE(report.passed());
    const auto& item = cut.item(report, "protected");
    CHECK_FALSE(item.passed);
    CHECK(item.detail.find("'B'") != std::string::npos);
}

TEST_CASE("Agent verification: edits outside the referenced range or tracks are caught; ripple shifts are not edits",
          "[ai][agent][verify]") {
    Constraints c;
    c.scope = TimeRange::fromStartEnd(f(0), f(10)); // A only

    Checked inside;
    inside.h.ok("timeline.removeSilences", ranges(2, 4)); // trims A; B and C only move
    CHECK(inside.run(c).passed());

    Checked outside;
    outside.h.ok("timeline.removeSilences", ranges(22, 24)); // cuts into C
    const auto report = outside.run(c);
    CHECK_FALSE(report.passed());
    CHECK(outside.item(report, "scope").detail.find("'C'") != std::string::npos);

    Checked both;
    both.h.ok("timeline.removeSilences", ranges(2, 4)); // all tracks by default: V1 changes too
    Constraints tracks;
    tracks.onlyTracks = {both.h.tl.track("A1")};
    const auto r2 = both.run(tracks);
    CHECK_FALSE(r2.passed());
    CHECK(both.item(r2, "scope").detail.find("V1") != std::string::npos);
}

TEST_CASE("Agent verification: marker and caption actions may not change clips", "[ai][agent][verify]") {
    Constraints c;
    c.allowClipEdits = false;

    Checked marker;
    marker.h.ok("marker.add", Json{{"time", tc(5)}, {"name", "Topic"}});
    CHECK(marker.run(c).passed());

    Checked clips;
    clips.h.ok("timeline.removeSilences", ranges(2, 4));
    CHECK_FALSE(clips.item(clips.run(c), "onlyMarkers").passed);
}

TEST_CASE("Agent verification: flash frames, new gaps and track changes", "[ai][agent][verify]") {
    SECTION("a 1-frame remnant is a flash frame") {
        Checked x;
        x.h.ok("timeline.removeSilences", ranges(11, 20)); // B keeps one frame
        const auto report = x.run({});
        CHECK_FALSE(report.passed());
        CHECK(x.item(report, "flashFrames").detail.find("1 frame") != std::string::npos);
        Constraints one;
        one.minClipFrames = 1;
        CHECK(x.run(one).passed());
    }
    SECTION("a lift leaves a gap, allowed only when asked") {
        Checked x;
        x.h.ok("sequence.lift", Json{{"in", tc(12)}, {"out", tc(15)}});
        const auto report = x.run({});
        CHECK_FALSE(x.item(report, "gaps").passed);
        Constraints lift;
        lift.allowGaps = true;
        CHECK(x.run(lift).passed());
    }
    SECTION("adding a track") {
        Checked x;
        x.h.ok("sequence.addTrack", Json{{"kind", "audio"}});
        CHECK_FALSE(x.item(x.run({}), "tracks").passed);
        Constraints add;
        add.keepTrackCount = false;
        CHECK(x.run(add).passed());
    }
}

TEST_CASE("Agent verification: locked tracks, nothing staged, and an untouched project", "[ai][agent][verify]") {
    const Timeline before(R"(
        V1 tgt  |Aaaaaaaaaa|
        A1 lock |Aaaaaaaaaa|
    )");
    const Timeline after(R"(
        V1 tgt  |Aaaaaaaaaa|
        A1 lock |Aaaaaaa...|
    )");
    const auto report = verify(before.state, after.state, before.seq, {}, 1);
    CHECK_FALSE(report.item("locked")->passed);
    CHECK(report.item("locked")->detail == "A1 is locked but changed");

    const auto nothing = verify(before.state, before.state, before.seq, {}, 0);
    CHECK_FALSE(nothing.passed());
    CHECK(nothing.item("changes")->detail == "nothing changed");
    Constraints optional;
    optional.expectChanges = false;
    const auto fine = verify(before.state, before.state, before.seq, optional, 0);
    CHECK(fine.passed());
    CHECK(fine.item("changes")->label == "No edits were needed");
    CHECK(fine.item("invariants")->passed);
}

TEST_CASE("Agent constraints are read from unambiguous phrases only", "[ai][agent][verify]") {
    const Timeline tl(kThree);
    auto parse = [&](std::string_view prompt) { return parseConstraints(prompt, tl.state, tl.seq); };

    CHECK(parse("Make a 30-second highlight of this").targetDuration == Ticks::fromSeconds(30));
    CHECK(parse("make a 2 minute video from the interview").targetDuration == Ticks::fromSeconds(120));
    CHECK(parse("Cut it down to 90 seconds").targetDuration == Ticks::fromSeconds(90));
    CHECK(parse("keep it under 60 seconds please").maxDuration == Ticks::fromSeconds(60));
    CHECK_FALSE(parse("keep it under 60 seconds please").targetDuration);
    // Lengths of pauses or clips are not lengths of the result.
    const Constraints pauses = parse("Remove pauses less than 2 s and silences longer than 1 s");
    CHECK_FALSE(pauses.targetDuration);
    CHECK_FALSE(pauses.maxDuration);

    const Constraints keep = parse("Tighten the talk but don't touch 00:00:01:00-00:00:02:00");
    REQUIRE(keep.protectedRanges.size() == 1);
    CHECK(keep.protectedRanges[0] == TimeRange::fromStartEnd(f(25), f(50)));
    CHECK_FALSE(keep.scope); // the protected range is not also a scope

    const Constraints scope = parse("remove the ums between 0:10 and 0:20 only on A1");
    REQUIRE(scope.scope);
    CHECK(*scope.scope == TimeRange::fromStartEnd(Ticks::fromSeconds(10), Ticks::fromSeconds(20)));
    CHECK(scope.onlyTracks == std::vector<TrackId>{tl.track("A1")});

    CHECK(parse("Lift the bad take").allowGaps);
    CHECK_FALSE(parse("Remove the bad take").allowGaps);
    CHECK_FALSE(parse("add a new audio track for music").keepTrackCount);
    CHECK(parse("Remove silences").keepTrackCount);
    CHECK(parse("Remove silences") == Constraints{});
}

TEST_CASE("Agent durations read as minutes:seconds", "[ai][agent][verify]") {
    CHECK(formatDuration(Ticks::fromSeconds(30)) == "0:30");
    CHECK(formatDuration(Ticks::fromSeconds(62) + Ticks{kTicksPerSecond / 2}) == "1:02.5");
    CHECK(formatDuration(Ticks{-kTicksPerSecond * 3}) == "-0:03");
}

TEST_CASE("Agent preview: cuts in base time, new content and new markers; shifts and splits are not changes",
          "[ai][agent][verify]") {
    using Range = StagedPreview::Range;
    auto sorted = [](std::vector<Range> v) {
        std::sort(v.begin(), v.end(), [](const Range& a, const Range& b) {
            return a.track.value != b.track.value ? a.track.value < b.track.value : a.start < b.start;
        });
        return v;
    };

    Checked cut;
    cut.h.ok("timeline.removeSilences", ranges(13, 15)); // B is split and C shifts
    const TrackId v1 = cut.h.tl.track("V1");
    const TrackId a1 = cut.h.tl.track("A1");
    const auto p = stagedPreview(*cut.before, *cut.h.state(), cut.h.tl.seq);
    CHECK(sorted(p.removed) == sorted({Range{v1, f(13), f(15)}, Range{a1, f(13), f(15)}}));
    CHECK(p.added.empty());
    CHECK(p.lengthBefore == f(30));
    CHECK(p.lengthAfter == f(28));

    Checked lift;
    lift.h.ok("sequence.lift", Json{{"in", tc(12)}, {"out", tc(15)}});
    CHECK(sorted(stagedPreview(*lift.before, *lift.h.state(), lift.h.tl.seq).removed) ==
          sorted({Range{v1, f(12), f(15)}, Range{a1, f(12), f(15)}}));

    Checked marker;
    marker.h.ok("marker.add", Json{{"time", tc(5)}, {"name", "Topic"}});
    const auto m = stagedPreview(*marker.before, *marker.h.state(), marker.h.tl.seq);
    CHECK(m.removed.empty());
    CHECK(m.markers == std::vector<Ticks>{f(5)});

    CHECK(stagedPreview(*cut.before, *cut.before, cut.h.tl.seq).empty());

    // New material: the second half of A becomes B.
    const Timeline before("V1 |Aaaaaaaaaa|");
    const Timeline after("V1 |AaaaaBbbbb|");
    const auto replaced = stagedPreview(before.state, after.state, before.seq);
    CHECK(replaced.removed == std::vector<Range>{Range{before.track("V1"), f(5), f(10)}});
    CHECK(replaced.added == std::vector<Range>{Range{before.track("V1"), f(5), f(10)}});
}

TEST_CASE("Agent preview: sequence markers a ripple moved are not new markers", "[ai][agent][verify]") {
    // 0.4: Ripple Sequence Markers (on by default) moves the markers after a cut; only the added one is new.
    Checked c;
    c.h.ok("marker.add", Json{{"time", tc(25)}, {"name", "Outro"}});
    c.h.ok("marker.add", Json{{"time", tc(18)}, {"name", "Outro"}});
    c.before = c.h.state();
    c.h.ok("timeline.removeSilences", ranges(13, 15)); // both Outro markers move back 2 frames
    c.h.ok("marker.add", Json{{"time", tc(5)}, {"name", "Topic"}});
    const auto p = stagedPreview(*c.before, *c.h.state(), c.h.tl.seq);
    CHECK(p.markers == std::vector<Ticks>{f(5)});
    // A second marker with the name of a moved one is still new.
    c.h.ok("marker.add", Json{{"time", tc(2)}, {"name", "Outro"}});
    CHECK(stagedPreview(*c.before, *c.h.state(), c.h.tl.seq).markers == std::vector<Ticks>{f(2), f(5)});
}

TEST_CASE("Agent verification: overlapping the scope is no licence to change a whole clip (review wave 3, P2-9)",
          "[ai][agent][verify][security]") {
    // One 20-frame clip; the request names frames 5-6 only.
    constexpr std::string_view kOne = R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    )";
    Constraints c;
    c.scope = TimeRange::fromStartEnd(f(5), f(6));

    // A cut inside the scope; the rest slides left.
    Checked trimmed(kOne);
    trimmed.h.ok("timeline.removeSilences", ranges(5, 6));
    CHECK(trimmed.run(c).passed());

    // The review's probe: the whole clip deleted because it overlaps the scope.
    Checked whole(kOne);
    whole.h.ok("timeline.removeSilences", ranges(0, 20));
    const auto report = whole.run(c);
    CHECK_FALSE(report.passed());
    CHECK(whole.item(report, "scope").detail.find("'A'") != std::string::npos);

    // A change to the whole clip (it is disabled): outside the scope too.
    Checked disabled(kOne);
    disabled.h.ok("clip.enable", Json{{"clips", Json::array({Json{{"track", "V1"}, {"time", tc(2)}}})}, {"enabled", false}});
    CHECK_FALSE(disabled.run(c).passed());

    // A clip spanning both ends of a wider scope: cuts inside pass, cuts reaching past either end do not.
    Constraints wide;
    wide.scope = TimeRange::fromStartEnd(f(5), f(15));
    Checked inside(kOne);
    inside.h.ok("timeline.removeSilences", ranges(7, 12));
    CHECK(inside.run(wide).passed());
    Checked early(kOne);
    early.h.ok("timeline.removeSilences", ranges(3, 12));
    CHECK_FALSE(early.run(wide).passed());
    Checked late(kOne);
    late.h.ok("timeline.removeSilences", ranges(7, 17));
    CHECK_FALSE(late.run(wide).passed());
}

TEST_CASE("Agent verification: another instance of the same source does not stand in for removed content (P2-9)",
          "[ai][agent][verify][security]") {
    // Media A twice (the same source 0-10 at 0 and at 15), B between them.
    constexpr std::string_view kTwice = R"(
        V1 tgt |AaaaaaaaaaBbbbbAaaaaaaaaa|
    )";
    Constraints c;
    c.scope = TimeRange::fromStartEnd(f(5), f(6));

    // The first A deleted whole: its frames 0-5 and 6-10 still exist in the second A, but not where they were.
    Checked first(kTwice);
    first.h.ok("timeline.removeSilences", ranges(0, 10));
    CHECK_FALSE(first.run(c).passed());

    // Only the head of the first A cut (frames 0-5, outside the scope): the same frames remain in the second A.
    Checked head(kTwice);
    head.h.ok("timeline.removeSilences", ranges(0, 5));
    CHECK_FALSE(head.run(c).passed());

    // The second A deleted (far outside the scope).
    Checked second(kTwice);
    second.h.ok("timeline.removeSilences", ranges(15, 25));
    CHECK_FALSE(second.run(c).passed());

    // A cut inside the scope: everything else is where it was, or slid left by the one frame removed.
    Checked inside(kTwice);
    inside.h.ok("timeline.removeSilences", ranges(5, 6));
    CHECK(inside.run(c).passed());
}