// Sequence In/Out marks (I, O, X, /), markers and snap targets.

#include "TimelineFixture.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/Snap.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace wabcut;
using namespace wabcut::test;

TEST_CASE("In and Out marks cover whole frames", "[model][marks]") {
    Timeline tl(R"(
        V1 |..........|
    )");
    const Ticks midFrame = f(3) + Ticks{12345};
    auto s = ok(edit::markIn(tl.state, tl.seq, midFrame)).state;
    s = ok(edit::markOut(s, tl.seq, f(6) + Ticks{1})).state;
    const Sequence& seq = *s.findSequence(tl.seq);
    CHECK(seq.markIn == f(3));
    CHECK(seq.markOut == f(7)); // the Out mark includes frame 6
    CHECK(seq.markedRange() == TimeRange{f(3), f(4)});
}

TEST_CASE("Setting a mark on the wrong side of the other clears the other", "[model][marks]") {
    Timeline tl(R"(
        V1 |..........|
    )");
    auto s = ok(edit::markIn(tl.state, tl.seq, f(3))).state;
    s = ok(edit::markOut(s, tl.seq, f(6))).state;

    const auto inAfterOut = ok(edit::markIn(s, tl.seq, f(8))).state;
    CHECK(inAfterOut.findSequence(tl.seq)->markIn == f(8));
    CHECK_FALSE(inAfterOut.findSequence(tl.seq)->markOut);

    const auto outBeforeIn = ok(edit::markOut(s, tl.seq, f(1))).state;
    CHECK(outBeforeIn.findSequence(tl.seq)->markOut == f(2));
    CHECK_FALSE(outBeforeIn.findSequence(tl.seq)->markIn);

    const auto cleared = ok(edit::clearMarks(s, tl.seq)).state;
    CHECK_FALSE(cleared.findSequence(tl.seq)->markIn);
    CHECK_FALSE(cleared.findSequence(tl.seq)->markOut);
    const auto clearedAgain = ok(edit::clearMarks(cleared, tl.seq));
    CHECK(isSameRevision(clearedAgain.state, cleared));

    CHECK_FALSE(edit::markIn(tl.state, tl.seq, f(-1)));
}

TEST_CASE("Mark Clip marks the clips under the playhead on the targeted tracks", "[model][marks]") {
    SECTION("targeted tracks only") {
        Timeline tl(R"(
            V2     |Cccccccccc|
            V1 tgt |..Aaaa....|
            A1 tgt |...Bbbbb..|
        )");
        const auto s = ok(edit::markClip(tl.state, tl.seq, f(4))).state;
        CHECK(s.findSequence(tl.seq)->markedRange() == TimeRange::fromStartEnd(f(2), f(8)));
        CHECK_FALSE(edit::markClip(tl.state, tl.seq, f(9))); // nothing under the playhead there
    }
    SECTION("all tracks when none is targeted") {
        Timeline tl(R"(
            V2 |Cccccccccc|
            V1 |..Aaaa....|
        )");
        const auto s = ok(edit::markClip(tl.state, tl.seq, f(4))).state;
        CHECK(s.findSequence(tl.seq)->markedRange() == TimeRange::fromStartEnd(f(0), f(10)));
    }
}

TEST_CASE("Mark Selection marks around the selected clips", "[model][marks]") {
    Timeline tl(R"(
        V2 |......Cc..|
        V1 |..Aa......|
    )");
    const std::vector<ClipId> sel{tl.clip("V1@2"), tl.clip("V2@6")};
    const auto s = ok(edit::markSelection(tl.state, tl.seq, sel)).state;
    CHECK(s.findSequence(tl.seq)->markedRange() == TimeRange::fromStartEnd(f(2), f(8)));
    CHECK_FALSE(edit::markSelection(tl.state, tl.seq, std::vector<ClipId>{}));
}

TEST_CASE("Markers stay sorted", "[model][marks]") {
    Timeline tl(R"(
        V1 |..........|
    )");
    auto s = ok(edit::addMarker(tl.state, tl.seq, {f(5), {}, "b", "", 1})).state;
    s = ok(edit::addMarker(s, tl.seq, {f(2), f(3), "a", "range marker", 2})).state;
    s = ok(edit::addMarker(s, tl.seq, {f(5), {}, "c", "", 0})).state;
    const auto& markers = s.findSequence(tl.seq)->markers;
    REQUIRE(markers.size() == 3);
    CHECK(markers[0].name == "a");
    CHECK(markers[1].name == "b");
    CHECK(markers[2].name == "c"); // equal positions keep insertion order

    const auto removed = ok(edit::removeMarker(s, tl.seq, 1)).state;
    CHECK(removed.findSequence(tl.seq)->markers.size() == 2);
    CHECK_FALSE(edit::removeMarker(s, tl.seq, 3));
    CHECK_FALSE(edit::addMarker(s, tl.seq, {f(-1), {}, "", "", 0}));
}

TEST_CASE("Snap targets are clip edges, playhead, marks and markers", "[model][snap]") {
    Timeline tl(R"(
        V2 |....Cc....|
        V1 |Aa....Bb..|
    )");
    auto s = ok(edit::markIn(tl.state, tl.seq, f(1))).state;
    s = ok(edit::addMarker(s, tl.seq, {f(8), f(1), "m", "", 0})).state;
    const Sequence& seq = *s.findSequence(tl.seq);

    const auto all = edit::snapTargets(seq, f(3));
    CHECK(all == std::vector<Ticks>{f(0), f(1), f(2), f(3), f(4), f(6), f(8), f(9)});

    const std::vector<ClipId> dragging{tl.clip("V2@4")};
    edit::SnapOptions options;
    options.exclude = dragging;
    options.playhead = false;
    options.markers = false;
    CHECK(edit::snapTargets(seq, f(3), options) == std::vector<Ticks>{f(0), f(1), f(2), f(6), f(8)});
}

TEST_CASE("The nearest snap target within the tolerance wins", "[model][snap]") {
    const std::vector<Ticks> targets{f(0), f(10), f(20)};
    CHECK(edit::nearestSnapTarget(targets, f(12), f(3)) == f(10));
    CHECK(edit::nearestSnapTarget(targets, f(18), f(3)) == f(20));
    CHECK_FALSE(edit::nearestSnapTarget(targets, f(15), f(3)));
    CHECK(edit::nearestSnapTarget(targets, f(15), f(5)) == f(10)); // a tie goes to the earlier target
    CHECK(edit::nearestSnapTarget(targets, f(-2), f(2)) == f(0));
    CHECK_FALSE(edit::nearestSnapTarget(std::vector<Ticks>{}, f(0), f(100)));
}
