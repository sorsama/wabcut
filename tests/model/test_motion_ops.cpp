// P.2 (E6, monitors): Motion set as one edit (MotionOps.h) - the Program Monitor's direct manipulation and
// clip.setMotion: constants take the value, animated parameters get a key at the curve time, one ChangeSet.

#include "TimelineFixture.h"

#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/FrameScaling.h>
#include <wabcut/model/MotionOps.h>
#include <wabcut/model/ProjectIO.h>

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

// ---- 0.7 (card #12): Scale Width and a Uniform Scale switch on Motion ---------------------------------------------------

TEST_CASE("Scale Width and Uniform Scale default to a uniform clip and are saved only when set (card 12)", "[model][motion][scalewidth]") {
    const ClipProperties plain;
    CHECK(plain.uniformScale());
    CHECK(plain.scaleAt() == std::pair<double, double>{1.0, 1.0});
    CHECK(parameterInfo(ParamId::ScaleWidth).defaultValue == 1.0);
    CHECK(parameterInfo(ParamId::UniformScale).defaultValue == 1.0);
    CHECK_FALSE(parameterInfo(ParamId::UniformScale).animatable); // a switch: constants only
    CHECK(parameterInfo(ParamId::ScaleWidth).animatable);
    CHECK(parameterByKey("motion.scaleWidth") == ParamId::ScaleWidth);
    CHECK(parameterByKey("motion.uniformScale") == ParamId::UniformScale);
    CHECK(allParameters().size() == kParamCount);

    // With Uniform Scale on, Scale Width is ignored; off, it is the width and Scale the height.
    ClipProperties stretched;
    stretched.set(ParamId::Scale, 0.5);
    stretched.set(ParamId::ScaleWidth, 2.0);
    CHECK(stretched.scaleAt() == std::pair<double, double>{0.5, 0.5});
    stretched.set(ParamId::UniformScale, 0.0);
    CHECK(stretched.scaleAt() == std::pair<double, double>{2.0, 0.5});

    // A curve on the switch is not a valid clip property.
    ClipProperties bad;
    bad[ParamId::UniformScale] = ParamValue(Curve{{key(0, 1.0)}});
    CHECK(bad.firstInvalid() == ParamId::UniformScale);

    // A project that never stretches a clip writes neither key; one that does reads back equal.
    Timeline tl(kPicture);
    const auto before = io::toJson(tl.state);
    REQUIRE(before);
    CHECK(before->find("motion.scaleWidth") == std::string::npos);
    CHECK(before->find("motion.uniformScale") == std::string::npos);
    const ClipId clip = tl.clip("V1@0");
    ProjectState s = ok(edit::setParameter(tl.state, tl.seq, clip, ParamRef::of(ParamId::UniformScale), ParamValue(0.0))).state;
    s = ok(edit::setParameter(s, tl.seq, clip, ParamRef::of(ParamId::ScaleWidth), ParamValue(1.5))).state;
    const auto text = io::toJson(s);
    REQUIRE(text);
    CHECK(text->find("motion.scaleWidth") != std::string::npos);
    CHECK(text->find("motion.uniformScale") != std::string::npos);
    const auto loaded = io::fromJson(*text);
    INFO((loaded ? std::string() : loaded.error().message));
    REQUIRE(loaded);
    CHECK(*loaded == s);
    const Clip& again = clipIn(*loaded, tl.seq, "V1@0");
    CHECK_FALSE(again.properties().uniformScale());
    CHECK(again.properties().scaleAt() == std::pair<double, double>{1.5, 1.0});
}

TEST_CASE("Switching Uniform Scale carries Scale Width along so the picture does not jump (card 12)", "[model][motion][scalewidth]") {
    Timeline tl(kPicture);
    const ClipId clip = tl.clip("V1@0");
    ProjectState s = ok(edit::setParameter(tl.state, tl.seq, clip, ParamRef::of(ParamId::Scale), ParamValue(0.5))).state;

    SECTION("off copies Scale to the width; on puts the width back to 100 %, each one step") {
        const auto off = ok(edit::setParameter(s, tl.seq, clip, ParamRef::of(ParamId::UniformScale), ParamValue(0.0)));
        const Clip& c = clipIn(off.state, tl.seq, "V1@0");
        CHECK_FALSE(c.properties().uniformScale());
        CHECK(c.properties()[ParamId::ScaleWidth] == ParamValue(0.5));
        CHECK(c.properties().scaleAt() == std::pair<double, double>{0.5, 0.5}); // the same picture as before
        REQUIRE(off.changes.sequences.size() == 1);
        CHECK(off.changes.sequences.front().modifiedClips.size() == 1);

        const auto widened = ok(edit::setParameter(off.state, tl.seq, clip, ParamRef::of(ParamId::ScaleWidth), ParamValue(2.0)));
        CHECK(clipIn(widened.state, tl.seq, "V1@0").properties().scaleAt() == std::pair<double, double>{2.0, 0.5});

        const auto on = ok(edit::setParameter(widened.state, tl.seq, clip, ParamRef::of(ParamId::UniformScale), ParamValue(1.0)));
        const Clip& back = clipIn(on.state, tl.seq, "V1@0");
        CHECK(back.properties().uniformScale());
        CHECK(back.properties().isDefault(ParamId::ScaleWidth));
        CHECK(back.properties().scaleAt() == std::pair<double, double>{0.5, 0.5});
    }
    SECTION("off copies the keys of a keyed Scale too") {
        Curve scale{{key(0, 0.5), key(8, 1.0)}};
        s = ok(edit::setParameter(s, tl.seq, clip, ParamRef::of(ParamId::Scale), ParamValue(scale))).state;
        const auto off = ok(edit::setParameter(s, tl.seq, clip, ParamRef::of(ParamId::UniformScale), ParamValue(0.0)));
        const Clip& c = clipIn(off.state, tl.seq, "V1@0");
        CHECK(c.properties()[ParamId::ScaleWidth] == c.properties()[ParamId::Scale]);
        CHECK(c.properties().scaleAt(f(4)).first == c.properties().scaleAt(f(4)).second);
    }
    SECTION("setting the switch to what it is changes nothing") {
        const auto same = ok(edit::setParameter(s, tl.seq, clip, ParamRef::of(ParamId::UniformScale), ParamValue(1.0)));
        CHECK(same.changes.empty());
    }
    SECTION("the switch cannot be keyframed or animated") {
        CHECK_FALSE(edit::setParameter(s, tl.seq, clip, ParamRef::of(ParamId::UniformScale), ParamValue(Curve{{key(0, 1.0), key(4, 0.0)}})));
        CHECK_FALSE(edit::addKeyframe(s, tl.seq, clip, ParamRef::of(ParamId::UniformScale), key(2, 0.0)));
        CHECK_FALSE(edit::setParameter(s, tl.seq, clip, ParamRef::of(ParamId::UniformScale), ParamValue(2.0))); // beyond 0..1
    }
}

TEST_CASE("setMotion changes one axis: a Scale Width turns Uniform Scale off in the same step (card 12)", "[model][motion][scalewidth]") {
    Timeline tl(kPicture);
    const ClipId clip = tl.clip("V1@0");
    ProjectState s = ok(edit::setParameter(tl.state, tl.seq, clip, ParamRef::of(ParamId::Scale), ParamValue(0.5))).state;

    SECTION("a width alone: the height stays, the switch goes off") {
        edit::MotionChange change;
        change.scaleWidth = 1.25;
        const auto r = ok(edit::setMotion(s, tl.seq, clip, f(2), change));
        const Clip& c = clipIn(r.state, tl.seq, "V1@0");
        CHECK_FALSE(c.properties().uniformScale());
        const edit::MotionValues v = edit::motionAt(c, f(2));
        CHECK(v.scale == 0.5);
        CHECK(v.scaleWidth == 1.25);
        CHECK_FALSE(v.uniformScale);
        REQUIRE(r.changes.sequences.size() == 1); // one ChangeSet: one undo step for the caller
        CHECK(r.changes.sequences.front().roles.has(PropertyRole::Motion));
    }
    SECTION("the height with the switch off leaves the width") {
        edit::MotionChange stretch;
        stretch.scaleWidth = 2.0;
        s = ok(edit::setMotion(s, tl.seq, clip, f(2), stretch)).state;
        edit::MotionChange height;
        height.scale = 0.25;
        const auto r = ok(edit::setMotion(s, tl.seq, clip, f(2), height));
        const edit::MotionValues v = edit::motionAt(clipIn(r.state, tl.seq, "V1@0"), f(2));
        CHECK(v.scale == 0.25);
        CHECK(v.scaleWidth == 2.0);
    }
    SECTION("the switch alone, off then on") {
        edit::MotionChange off;
        off.uniformScale = false;
        const auto r = ok(edit::setMotion(s, tl.seq, clip, f(2), off));
        CHECK(edit::motionAt(clipIn(r.state, tl.seq, "V1@0"), f(2)).scaleWidth == 0.5);
        edit::MotionChange on;
        on.uniformScale = true;
        const auto back = ok(edit::setMotion(r.state, tl.seq, clip, f(2), on));
        CHECK(clipIn(back.state, tl.seq, "V1@0").properties().uniformScale());
        // Already on: a no-op, not an error and not an empty undo step.
        const auto again = ok(edit::setMotion(back.state, tl.seq, clip, f(2), on));
        CHECK(again.changes.empty());
    }
    SECTION("a width is keyed at the playhead when it is animated") {
        edit::MotionChange stretch;
        stretch.scaleWidth = 1.0;
        s = ok(edit::setMotion(s, tl.seq, clip, f(0), stretch)).state;
        s = ok(edit::setParameter(s, tl.seq, clip, ParamRef::of(ParamId::ScaleWidth), ParamValue(Curve{{key(0, 1.0), key(8, 2.0)}}))).state;
        edit::MotionChange mid;
        mid.scaleWidth = 3.0;
        const auto r = ok(edit::setMotion(s, tl.seq, clip, f(4), mid));
        const Curve* width = clipIn(r.state, tl.seq, "V1@0").properties()[ParamId::ScaleWidth].curve();
        REQUIRE(width);
        REQUIRE(width->keys.size() == 3);
        CHECK(width->keys[1].time == f(4));
        CHECK(width->keys[1].value == 3.0);
    }
    SECTION("Scale Width with Uniform Scale on is a contradiction") {
        edit::MotionChange both;
        both.scaleWidth = 2.0;
        both.uniformScale = true;
        CHECK_FALSE(edit::setMotion(s, tl.seq, clip, f(0), both));
    }
    SECTION("a width beyond the range, or a switch on an audio clip") {
        edit::MotionChange big;
        big.scaleWidth = 1000.0;
        CHECK_FALSE(edit::setMotion(s, tl.seq, clip, f(0), big));
        edit::MotionChange audio;
        audio.uniformScale = false;
        CHECK_FALSE(edit::setMotion(s, tl.seq, tl.clip("A1@0"), f(0), audio));
    }
}

TEST_CASE("Set to Frame Size makes a stretched clip uniform again (card 12)", "[model][motion][scalewidth]") {
    Timeline tl(kPicture);
    const ClipId clip = tl.clip("V1@0");
    edit::MotionChange stretch;
    stretch.scaleWidth = 2.0;
    ProjectState s = ok(edit::setMotion(tl.state, tl.seq, clip, f(0), stretch)).state;
    REQUIRE_FALSE(clipIn(s, tl.seq, "V1@0").properties().uniformScale());
    const auto fitted = ok(edit::setToFrameSize(s, tl.seq, std::vector<ClipId>{clip}, edit::FrameFit::Frame));
    const Clip& c = clipIn(fitted.state, tl.seq, "V1@0");
    CHECK(c.properties().uniformScale());
    CHECK(c.properties().isDefault(ParamId::ScaleWidth));
}
