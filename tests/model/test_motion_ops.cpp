// P.2 (E6, monitors): Motion set as one edit (MotionOps.h) - the Program Monitor's direct manipulation and
// clip.setMotion: constants take the value, animated parameters get a key at the curve time, one ChangeSet.

#include "TimelineFixture.h"

#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/MotionOps.h>

#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace wabcut;
using namespace wabcut::test;
using edit::ParamRef;

namespace {

constexpr std::string_view kPicture = R"(
    V2     |..........|
    V1 tgt |Aaaaaaaaaa|
    A1 tgt |Aaaaaaaaaa|
)";

const Clip& clipIn(const ProjectState& state, SequenceId seq, std::string_view ref) {
    return state.findSequence(seq)->findClip(clipAt(state, seq, ref))->clip();
}

Keyframe key(std::int64_t frame, double value, Interpolation interpolation = Interpolation::Linear) {
    Keyframe k;
    k.time = f(frame);
    k.value = value;
    k.interpolation = interpolation;
    return k;
}

} // namespace

TEST_CASE("setMotion sets constant Motion parameters together as one edit", "[model][motion]") {
    Timeline tl(kPicture);
    const ClipId clip = tl.clip("V1@0");
    edit::MotionChange change;
    change.positionX = 120.0;
    change.positionY = -40.0;
    change.scale = 0.5;
    const auto r = ok(edit::setMotion(tl.state, tl.seq, clip, f(3), change));
    const Clip& c = clipIn(r.state, tl.seq, "V1@0");
    CHECK(c.properties()[ParamId::PositionX] == ParamValue(120.0));
    CHECK(c.properties()[ParamId::PositionY] == ParamValue(-40.0));
    CHECK(c.properties()[ParamId::Scale] == ParamValue(0.5));
    CHECK(c.properties()[ParamId::Rotation] == ParamValue(0.0)); // left out: unchanged
    // One ChangeSet naming the clip with the Motion role.
    REQUIRE(r.changes.sequences.size() == 1);
    const SequenceChange& sc = r.changes.sequences.front();
    REQUIRE(sc.modifiedClips.size() == 1);
    CHECK(sc.modifiedClips.front() == clip);
    CHECK(sc.roles.has(PropertyRole::Motion));
    const edit::MotionValues v = edit::motionAt(c, f(3));
    CHECK(v.positionX == 120.0);
    CHECK(v.scale == 0.5);
}

TEST_CASE("setMotion keys an animated parameter at the curve time and keeps the others constant", "[model][motion]") {
    Timeline tl(kPicture);
    const ClipId clip = tl.clip("V1@0");
    // Position X animated (0 at frame 0, 100 at frame 8, ease out first); Position Y constant.
    Curve x{{key(0, 0.0, Interpolation::EaseOut), key(8, 100.0)}};
    ProjectState s = ok(edit::setParameter(tl.state, tl.seq, clip, ParamRef::of(ParamId::PositionX), ParamValue(x))).state;

    SECTION("a new key between keys continues the segment's shape") {
        edit::MotionChange change;
        change.positionX = 300.0;
        change.positionY = 25.0;
        const auto r = ok(edit::setMotion(s, tl.seq, clip, f(4), change));
        const Clip& c = clipIn(r.state, tl.seq, "V1@0");
        const Curve* cx = c.properties()[ParamId::PositionX].curve();
        REQUIRE(cx);
        REQUIRE(cx->keys.size() == 3);
        CHECK(cx->keys[1].time == f(4));
        CHECK(cx->keys[1].value == 300.0);
        CHECK(cx->keys[1].interpolation == Interpolation::EaseOut);
        CHECK(c.properties()[ParamId::PositionY] == ParamValue(25.0));
    }
    SECTION("the key already there is updated") {
        edit::MotionChange change;
        change.positionX = -10.0;
        const auto r = ok(edit::setMotion(s, tl.seq, clip, f(8), change));
        const Curve* cx = clipIn(r.state, tl.seq, "V1@0").properties()[ParamId::PositionX].curve();
        REQUIRE(cx);
        REQUIRE(cx->keys.size() == 2);
        CHECK(cx->keys[1].value == -10.0);
    }
    SECTION("a value equal to the curve's adds a key there") {
        edit::MotionChange change;
        change.positionX = Curve{{key(0, 0.0, Interpolation::EaseOut), key(8, 100.0)}}.valueAt(f(2));
        const auto r = ok(edit::setMotion(s, tl.seq, clip, f(2), change));
        const Curve* cx = clipIn(r.state, tl.seq, "V1@0").properties()[ParamId::PositionX].curve();
        REQUIRE(cx);
        CHECK(cx->keys.size() == 3);
    }
}

TEST_CASE("setMotion refuses audio clips, locked tracks, values out of range and empty changes", "[model][motion]") {
    edit::MotionChange change;
    change.rotation = 45.0;
    SECTION("an audio clip has no Motion") {
        Timeline tl(kPicture);
        CHECK_FALSE(edit::setMotion(tl.state, tl.seq, tl.clip("A1@0"), f(0), change));
    }
    SECTION("a locked track never changes") {
        Timeline tl(R"(
            V1 lock |Aaaaaaaaaa|
        )");
        const auto r = edit::setMotion(tl.state, tl.seq, tl.clip("V1@0"), f(0), change);
        CHECK_FALSE(r);
    }
    SECTION("a scale beyond the parameter's range") {
        Timeline tl(kPicture);
        edit::MotionChange big;
        big.scale = 1000.0;
        CHECK_FALSE(edit::setMotion(tl.state, tl.seq, tl.clip("V1@0"), f(0), big));
    }
    SECTION("a value that is not finite") {
        Timeline tl(kPicture);
        edit::MotionChange nan;
        nan.anchorX = std::numeric_limits<double>::quiet_NaN();
        CHECK_FALSE(edit::setMotion(tl.state, tl.seq, tl.clip("V1@0"), f(0), nan));
    }
    SECTION("nothing to set") {
        Timeline tl(kPicture);
        CHECK_FALSE(edit::setMotion(tl.state, tl.seq, tl.clip("V1@0"), f(0), edit::MotionChange{}));
    }
    SECTION("an unchanged value is a no-op") {
        Timeline tl(kPicture);
        edit::MotionChange same;
        same.rotation = 0.0;
        const auto r = ok(edit::setMotion(tl.state, tl.seq, tl.clip("V1@0"), f(0), same));
        CHECK(r.changes.empty());
    }
}
