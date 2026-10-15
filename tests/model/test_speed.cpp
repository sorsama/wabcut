// Speed/Duration (constant speed, reverse, ripple, maintain pitch) and frame holds, and how retimed clips
// behave under trims and razor cuts.

#include "TimelineFixture.h"

#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace wabcut;
using namespace wabcut::test;
using edit::SpeedChange;

namespace {

const Clip& clipIn(const ProjectState& state, SequenceId seq, std::string_view ref) {
    return state.findSequence(seq)->findClip(clipAt(state, seq, ref))->clip();
}

std::int64_t frameAt(const Clip& c, std::int64_t timelineFrame) {
    return ticksToFrames(c.sourceTimeAt(f(timelineFrame)), kRate);
}

SpeedChange speed(std::int32_t num, std::int32_t den, bool ripple) {
    SpeedChange s;
    s.speed = Speed{num, den};
    s.ripple = ripple;
    return s;
}

} // namespace

TEST_CASE("Speed/Duration keeps the source material and changes the length", "[model][speed]") {
    Timeline tl(R"(
        V1 tgt |AaaaaaaaBb....|
        A1 tgt |AaaaaaaaBb....|
    )");
    tl.linkMatching();
    const ClipId a = tl.clip("V1@0");

    SECTION("twice as fast without ripple leaves a gap") {
        const auto r = ok(edit::setSpeed(tl.state, tl.seq, std::vector<ClipId>{a}, speed(2, 1, false)));
        const Clip& c = clipIn(r.state, tl.seq, "V1@0");
        CHECK(c.duration() == f(4));
        CHECK(c.sourceDuration() == f(8));
        CHECK(c.speed == Speed{2, 1});
        CHECK(clipIn(r.state, tl.seq, "A1@0").duration() == f(4)); // the linked audio too
        CHECK(clipIn(r.state, tl.seq, "V1@8").name() == "B");    // B stays put
        CHECK(frameAt(c, 3) == 6);
    }
    SECTION("twice as fast with ripple pulls later clips in") {
        const auto r = ok(edit::setSpeed(tl.state, tl.seq, std::vector<ClipId>{a}, speed(2, 1, true)));
        CHECK(clipIn(r.state, tl.seq, "V1@4").name() == "B");
        CHECK(clipIn(r.state, tl.seq, "A1@4").name() == "B");
    }
    SECTION("half speed with ripple pushes later clips out") {
        const auto r = ok(edit::setSpeed(tl.state, tl.seq, std::vector<ClipId>{a}, speed(1, 2, true)));
        CHECK(clipIn(r.state, tl.seq, "V1@0").duration() == f(16));
        CHECK(clipIn(r.state, tl.seq, "V1@16").name() == "B");
    }
    SECTION("half speed without ripple only grows into the gap after the clip") {
        const auto r = ok(edit::setSpeed(tl.state, tl.seq, std::vector<ClipId>{a}, speed(1, 2, false)));
        const Clip& c = clipIn(r.state, tl.seq, "V1@0");
        CHECK(c.duration() == f(8));        // B starts at 8
        CHECK(c.sourceDuration() == f(4));  // so only the first half of the material plays
    }
    SECTION("a sync-locked track follows a rippled speed change") {
        Timeline sync(R"(
            V1 tgt |AaaaaaaaBb....|
            A1     |Mmmmmmmmmmmmmm|
        )");
        const auto r = ok(edit::setSpeed(sync.state, sync.seq, std::vector<ClipId>{sync.clip("V1@0")}, speed(1, 2, true)));
        CHECK(clipIn(r.state, sync.seq, "A1@16").name() == "M"); // cut at the old end and pushed by 8 frames
    }
    SECTION("speeds must be positive; frame holds cannot be retimed") {
        CHECK_FALSE(edit::setSpeed(tl.state, tl.seq, std::vector<ClipId>{a}, speed(0, 1, false)));
        const auto hold = ok(edit::frameHold(tl.state, tl.seq, a, f(0)));
        CHECK_FALSE(edit::setSpeed(hold.state, tl.seq, std::vector<ClipId>{a}, speed(2, 1, false)));
    }
    SECTION("maintain pitch is stored") {
        SpeedChange change = speed(3, 2, false);
        change.maintainPitch = true;
        const auto r = ok(edit::setSpeed(tl.state, tl.seq, std::vector<ClipId>{a}, change));
        CHECK(clipIn(r.state, tl.seq, "A1@0").maintainPitch);
        CHECK(clipIn(r.state, tl.seq, "V1@0").duration() == f(5)); // 8 / 1.5 = 5.33, whole frames
    }
}

TEST_CASE("Reversed clips play their media backwards and stay exact under edits", "[model][speed]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaa|
    )");
    SpeedChange change = speed(1, 1, false);
    change.reversed = true;
    const auto r = ok(edit::setSpeed(tl.state, tl.seq, std::vector<ClipId>{tl.clip("V1@0")}, change));
    const Clip& c = clipIn(r.state, tl.seq, "V1@0");
    CHECK(c.reversed);
    CHECK(frameAt(c, 0) == 7);
    CHECK(frameAt(c, 7) == 0);

    SECTION("a razor cut keeps every frame where it was") {
        const auto cut = ok(edit::addEdit(r.state, tl.seq, f(3), std::vector<TrackId>{tl.track("V1")}));
        const Clip& left = clipIn(cut.state, tl.seq, "V1@0");
        const Clip& right = clipIn(cut.state, tl.seq, "V1@3");
        CHECK(left.sourceIn == f(5));
        CHECK(right.sourceIn == f(0));
        for (int t = 0; t < 8; ++t) {
            INFO("frame " << t);
            CHECK(frameAt(t < 3 ? left : right, t) == 7 - t);
        }
    }
    SECTION("trims remove the frames under the trimmed end") {
        const auto tail = ok(edit::trimEdge(r.state, tl.seq, c.id, edit::Edge::Tail, -f(2)));
        const Clip& t = clipIn(tail.state, tl.seq, "V1@0");
        CHECK(t.sourceIn == f(2)); // frames 0 and 1 (shown last) are gone
        CHECK(t.sourceOut() == f(8));
        CHECK(frameAt(t, 0) == 7);
        const auto head = ok(edit::trimEdge(r.state, tl.seq, c.id, edit::Edge::Head, f(2)));
        const Clip& h = clipIn(head.state, tl.seq, "V1@2");
        CHECK(h.sourceIn == f(0));
        CHECK(frameAt(h, 2) == 5); // frames 7 and 6 (shown first) are gone
    }
    SECTION("extending the tail reaches for earlier media") {
        Timeline mid(R"(
            V1 tgt |A5aaaa|
        )");
        const auto rev = ok(edit::setSpeed(mid.state, mid.seq, std::vector<ClipId>{mid.clip("V1@0")}, change));
        const auto longer = ok(edit::trimEdge(rev.state, mid.seq, mid.clip("V1@0"), edit::Edge::Tail, f(10)));
        const Clip& l = clipIn(longer.state, mid.seq, "V1@0");
        CHECK(l.duration() == f(11)); // grew by the 5 frames of media before source frame 5
        CHECK(l.sourceIn == f(0));
    }
}

TEST_CASE("Frame holds", "[model][speed]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaa..|
        A1 tgt |Aaaaaaaa..|
    )");
    tl.linkMatching();
    const ClipId a = tl.clip("V1@0");

    SECTION("Add Frame Hold: the rest of the clip shows the frame at the playhead") {
        const auto r = ok(edit::frameHold(tl.state, tl.seq, a, f(3)));
        const Clip& hold = clipIn(r.state, tl.seq, "V1@3");
        CHECK(hold.isFreeze());
        CHECK(hold.duration() == f(5));
        CHECK(frameAt(hold, 3) == 3);
        CHECK(frameAt(hold, 7) == 3);
        CHECK(clipIn(r.state, tl.seq, "V1@0").duration() == f(3));
        CHECK(clipIn(r.state, tl.seq, "A1@0").duration() == f(8)); // audio continues
        // A hold extends without running out of media.
        const auto longer = ok(edit::trimEdge(r.state, tl.seq, hold.id, edit::Edge::Tail, f(2), edit::LinkMode::IgnoreLinks));
        CHECK(clipIn(longer.state, tl.seq, "V1@3").duration() == f(7));
    }
    SECTION("Insert Frame Hold Segment ripples the track and sync-locked tracks") {
        const auto r = ok(edit::frameHold(tl.state, tl.seq, a, f(3), f(2)));
        const Clip& hold = clipIn(r.state, tl.seq, "V1@3");
        CHECK(hold.isFreeze());
        CHECK(hold.duration() == f(2));
        CHECK(frameAt(clipIn(r.state, tl.seq, "V1@5"), 5) == 3); // the clip continues after the hold
        CHECK(clipIn(r.state, tl.seq, "A1@5").start() == f(5));  // audio cut and pushed in sync
        CHECK_FALSE(r.state.findSequence(tl.seq)->audioTracks[0]->clipIndexAt(f(3)));
    }
    SECTION("effects keep animating through a hold") {
        auto animated = ok(edit::addKeyframe(tl.state, tl.seq, a, edit::ParamRef::of(ParamId::Opacity), Keyframe{f(3), 1.0}));
        animated = ok(edit::addKeyframe(animated.state, tl.seq, a, edit::ParamRef::of(ParamId::Opacity), Keyframe{f(7), 0.0}));
        const auto r = ok(edit::frameHold(animated.state, tl.seq, a, f(3)));
        const Clip& hold = clipIn(r.state, tl.seq, "V1@3");
        CHECK(hold.parameterAt(ParamId::Opacity, f(3)) == 1.0);
        CHECK(hold.parameterAt(ParamId::Opacity, f(5)) == 0.5);
    }
    SECTION("holds are video only, and not twice") {
        CHECK_FALSE(edit::frameHold(tl.state, tl.seq, tl.clip("A1@0"), f(3)));
        const auto r = ok(edit::frameHold(tl.state, tl.seq, a, f(0)));
        CHECK_FALSE(edit::frameHold(r.state, tl.seq, a, f(2)));
        CHECK_FALSE(edit::frameHold(tl.state, tl.seq, a, f(9)));
    }
}

TEST_CASE("Speed for a duration", "[model][speed]") {
    CHECK(edit::speedForDuration(f(10), f(20)) == Speed{1, 2});
    CHECK(edit::speedForDuration(f(10), f(3)) == Speed{10, 3});
    CHECK_FALSE(edit::speedForDuration(f(10), Ticks{}));
    // Not exactly representable in int32: the result never reads past the media.
    const auto odd = edit::speedForDuration(Ticks{4'000'000'007}, Ticks{3'000'000'019});
    REQUIRE(odd);
    CHECK(toSourceTicks(Ticks{3'000'000'019}, *odd) <= Ticks{4'000'000'007});
}

TEST_CASE("Retimed clips round-trip through the project file", "[model][speed][io]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaa|
        A1 tgt |Aaaaaaaa|
    )");
    tl.linkMatching();
    SpeedChange change = speed(3, 2, true);
    change.reversed = true;
    change.maintainPitch = true;
    auto r = ok(edit::setSpeed(tl.state, tl.seq, std::vector<ClipId>{tl.clip("V1@0")}, change));
    r = ok(edit::frameHold(r.state, tl.seq, tl.clip("V1@0"), f(2)));
    const auto text = io::toJson(r.state);
    REQUIRE(text);
    CHECK(text->find("\"reversed\": true") != std::string::npos);
    CHECK(text->find("\"maintainPitch\": true") != std::string::npos);
    const auto loaded = io::fromJson(*text);
    REQUIRE(loaded);
    CHECK(*loaded == r.state);
}

TEST_CASE("Speed and duration unlinked: the clip keeps its length and plays more or less media", "[model][speed]") {
    // Premiere's Speed/Duration with the chain icon off (and what importers use to retime a placed clip).
    Timeline tl(R"(
        V1 tgt |AaaaaaaaBb....|
        A1 tgt |AaaaaaaaBb....|
    )",
                {{'A', 20}});
    tl.linkMatching();
    const ClipId a = tl.clip("V1@0");
    SpeedChange change = speed(2, 1, true); // ripple does not apply
    change.keepDuration = true;

    const auto r = ok(edit::setSpeed(tl.state, tl.seq, std::vector<ClipId>{a}, change));
    const Clip& c = clipIn(r.state, tl.seq, "V1@0");
    CHECK(c.speed == Speed{2, 1});
    CHECK(c.duration() == f(8));
    CHECK(c.sourceIn == Ticks{});
    CHECK(c.sourceDuration() == f(16));
    CHECK(clipIn(r.state, tl.seq, "A1@0").sourceDuration() == f(16)); // the linked audio too
    CHECK(clipIn(r.state, tl.seq, "V1@8").name() == "B");              // nothing moves
    CHECK(r.changes.sequence(tl.seq)->modifiedClips.size() == 2);

    SECTION("slower plays less of the media") {
        change.speed = Speed{1, 2};
        const auto slow = ok(edit::setSpeed(tl.state, tl.seq, std::vector<ClipId>{a}, change));
        CHECK(clipIn(slow.state, tl.seq, "V1@0").sourceDuration() == f(4));
        CHECK(clipIn(slow.state, tl.seq, "V1@0").duration() == f(8));
    }
    SECTION("refused when the media ends before the new Out") {
        change.speed = Speed{3, 1}; // 24 frames of a 20-frame media
        const auto refused = edit::setSpeed(tl.state, tl.seq, std::vector<ClipId>{a}, change);
        REQUIRE_FALSE(refused);
        CHECK(refused.error().message.find("past the end of its media") != std::string::npos);
    }
}
