// Keyframe curves: evaluation (each interpolation, exact at keys, clamped outside, monotonic Bezier time), the
// keyframe operations, and how keys behave through razor, slip and trim (they stick to the media).

#include "TimelineFixture.h"

#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;
using edit::ParamRef;

namespace {

Keyframe key(std::int64_t frame, double value, Interpolation interpolation = Interpolation::Linear) {
    Keyframe k;
    k.time = f(frame);
    k.value = value;
    k.interpolation = interpolation;
    return k;
}

const ParamRef kOpacity = ParamRef::of(ParamId::Opacity);

const Clip& clipIn(const ProjectState& state, SequenceId seq, std::string_view ref) {
    return state.findSequence(seq)->findClip(clipAt(state, seq, ref))->clip();
}

} // namespace

// --- Evaluation ---------------------------------------------------------------------------------------------

TEST_CASE("Curves are exact at their keys and clamp outside them", "[model][keyframes]") {
    const Curve c{{key(10, 0.0), key(20, 1.0), key(30, 0.25)}};
    CHECK(c.valueAt(f(0)) == 0.0);
    CHECK(c.valueAt(f(10)) == 0.0);
    CHECK(c.valueAt(f(20)) == 1.0);
    CHECK(c.valueAt(f(30)) == 0.25);
    CHECK(c.valueAt(f(1000)) == 0.25);
    CHECK(c.valueAt(Ticks::min()) == 0.0);
    CHECK(c.valueAt(f(15)) == 0.5); // linear midpoint, exact
    CHECK(c.keyAt(f(20)) == 1u);
    CHECK_FALSE(c.keyAt(f(21)));
    CHECK(c.insertionIndex(f(25)) == 2u);
    // Single key: constant everywhere.
    CHECK(Curve{{key(5, 0.7)}}.valueAt(f(-100)) == 0.7);
}

TEST_CASE("Each interpolation shapes its segment", "[model][keyframes]") {
    SECTION("hold steps at the next key") {
        const Curve c{{key(0, 0.0, Interpolation::Hold), key(10, 1.0)}};
        CHECK(c.valueAt(f(9)) == 0.0);
        CHECK(c.valueAt(f(10) - Ticks{1}) == 0.0);
        CHECK(c.valueAt(f(10)) == 1.0);
    }
    SECTION("linear") {
        const Curve c{{key(0, 0.0), key(10, 1.0)}};
        CHECK(c.valueAt(f(1)) == Catch::Approx(0.1).margin(1e-15));
        CHECK(c.valueAt(f(5)) == 0.5);
    }
    SECTION("ease in and out: slow start, symmetric middle, slow arrival") {
        const Curve c{{key(0, 0.0, Interpolation::EaseInOut), key(10, 1.0, Interpolation::EaseInOut)}};
        CHECK(c.valueAt(f(1)) < 0.1);
        CHECK(c.valueAt(f(5)) == Catch::Approx(0.5).margin(1e-12));
        CHECK(c.valueAt(f(9)) > 0.9);
        CHECK(c.valueAt(f(3)) + c.valueAt(f(7)) == Catch::Approx(1.0).margin(1e-12));
    }
    SECTION("ease out leaves a key slowly, ease in arrives slowly") {
        const Curve out{{key(0, 0.0, Interpolation::EaseOut), key(10, 1.0)}};
        CHECK(out.valueAt(f(1)) < 0.1);
        const Curve in{{key(0, 0.0), key(10, 1.0, Interpolation::EaseIn)}};
        CHECK(in.valueAt(f(9)) > 0.9);
        CHECK(in.valueAt(f(1)) == Catch::Approx(0.1).margin(0.02)); // near-linear departure
    }
    SECTION("Bezier with flat handles equals ease in/out") {
        Keyframe a = key(0, 0.0, Interpolation::Bezier);
        Keyframe b = key(10, 1.0, Interpolation::Bezier);
        a.out = {0.0, 1.0 / 3.0};
        b.in = {0.0, 1.0 / 3.0};
        const Curve bezier{{a, b}};
        const Curve ease{{key(0, 0.0, Interpolation::EaseInOut), key(10, 1.0, Interpolation::EaseInOut)}};
        for (int i = 0; i <= 10; ++i) {
            CHECK(bezier.valueAt(f(i)) == Catch::Approx(ease.valueAt(f(i))).margin(1e-12));
        }
    }
    SECTION("Bezier slopes overshoot but stay continuous") {
        Keyframe a = key(0, 0.0, Interpolation::Bezier);
        Keyframe b = key(25, 1.0, Interpolation::Bezier); // one second
        a.out = {10.0, 0.5};                                // leaves at 10 units per second
        b.in = {10.0, 0.5};
        const Curve c{{a, b}};
        double previous = c.valueAt(f(0));
        double maximum = previous;
        for (int i = 1; i <= 250; ++i) {
            const double v = c.valueAt(Ticks{f(25).value * i / 250});
            CHECK(std::isfinite(v));
            CHECK(std::abs(v - previous) < 0.1);
            maximum = std::max(maximum, v);
            previous = v;
        }
        CHECK(maximum > 1.0); // the handles overshoot the end value
        CHECK(c.valueAt(f(25)) == 1.0);
    }
}

TEST_CASE("Bezier time solving is monotonic for any influence", "[model][keyframes]") {
    for (const double influence : {0.0, 0.01, 1.0 / 3.0, 0.9, 1.0}) {
        Keyframe a = key(0, 0.0, Interpolation::Bezier);
        Keyframe b = key(100, 1.0, Interpolation::Bezier);
        a.out = {0.0, influence};
        b.in = {0.0, influence};
        const Curve c{{a, b}};
        double previous = -1.0;
        for (std::int64_t t = 0; t <= f(100).value; t += f(100).value / 997) {
            const double v = c.valueAt(Ticks{t});
            INFO("influence " << influence << " at tick " << t);
            CHECK(v >= previous - 1e-12); // values rise with time: x(s) is solved monotonically
            previous = v;
        }
    }
}

TEST_CASE("Evaluation is exact far from zero (integer time until the last step)", "[model][keyframes]") {
    const Ticks tenHours = Ticks::fromSeconds(36000);
    const Curve c{{Keyframe{tenHours, 0.0}, Keyframe{tenHours + f(2), 1.0}}};
    CHECK(c.valueAt(tenHours + f(1)) == 0.5);
    CHECK(c.valueAt(tenHours + f(2) - Ticks{1}) < 1.0);
    // Deterministic: the same inputs give bit-identical results.
    const Curve e{{key(0, 0.0, Interpolation::EaseInOut), key(7, 3.0, Interpolation::Bezier)}};
    CHECK(e.valueAt(f(3)) == e.valueAt(f(3)));
}

// --- Operations ---------------------------------------------------------------------------------------------

TEST_CASE("Keyframe operations edit one parameter of one clip", "[model][keyframes]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa|
        A1 tgt |Aaaaaaaaaa|
    )");
    const ClipId video = tl.clip("V1@0");
    auto r = ok(edit::addKeyframe(tl.state, tl.seq, video, kOpacity, key(2, 0.0)));
    r = ok(edit::addKeyframe(r.state, tl.seq, video, kOpacity, key(6, 1.0)));
    const Clip& c = clipIn(r.state, tl.seq, "V1@0");
    REQUIRE(c.properties()[ParamId::Opacity].isCurve());
    CHECK(c.properties()[ParamId::Opacity].curve()->keys.size() == 2);
    CHECK(c.parameterAt(ParamId::Opacity, f(4)) == 0.5);
    CHECK(c.parameterAt(ParamId::Opacity, f(0)) == 0.0);
    CHECK(r.changes.sequences[0].roles.has(PropertyRole::Opacity));
    // Only that clip changed.
    CHECK(r.changes.sequences[0].modifiedClips == std::vector<ClipId>{video});

    SECTION("adding at an existing time replaces the key") {
        const auto again = ok(edit::addKeyframe(r.state, tl.seq, video, kOpacity, key(6, 0.5)));
        CHECK(clipIn(again.state, tl.seq, "V1@0").properties()[ParamId::Opacity].curve()->keys[1].value == 0.5);
    }
    SECTION("moving keeps the keys sorted and refuses a taken time") {
        const auto moved = ok(edit::moveKeyframe(r.state, tl.seq, video, kOpacity, 0, f(8)));
        const auto& keys = clipIn(moved.state, tl.seq, "V1@0").properties()[ParamId::Opacity].curve()->keys;
        CHECK(keys[0].time == f(6));
        CHECK(keys[1].time == f(8));
        CHECK_FALSE(edit::moveKeyframe(r.state, tl.seq, video, kOpacity, 0, f(6)));
        CHECK_FALSE(edit::moveKeyframe(r.state, tl.seq, video, kOpacity, 5, f(1)));
    }
    SECTION("interpolation, value and handles") {
        edit::KeyframeChange change;
        change.interpolation = Interpolation::Hold;
        const auto held = ok(edit::setKeyframe(r.state, tl.seq, video, kOpacity, 0, change));
        CHECK(clipIn(held.state, tl.seq, "V1@0").parameterAt(ParamId::Opacity, f(5)) == 0.0);
        edit::KeyframeChange bad;
        bad.out = BezierHandle{0.0, 2.0};
        CHECK_FALSE(edit::setKeyframe(r.state, tl.seq, video, kOpacity, 0, bad));
    }
    SECTION("removing the last key leaves a constant") {
        auto one = ok(edit::removeKeyframe(r.state, tl.seq, video, kOpacity, 0));
        auto none = ok(edit::removeKeyframe(one.state, tl.seq, video, kOpacity, 0));
        const ParamValue& v = clipIn(none.state, tl.seq, "V1@0").properties()[ParamId::Opacity];
        CHECK(v.isConstant());
        CHECK(v.constant() == 1.0);
    }
    SECTION("the stopwatch turns animation on and off") {
        const auto on = ok(edit::setAnimated(tl.state, tl.seq, video, kOpacity, true, f(3)));
        const ParamValue& v = clipIn(on.state, tl.seq, "V1@0").properties()[ParamId::Opacity];
        REQUIRE(v.isCurve());
        CHECK(v.curve()->keys == std::vector<Keyframe>{Keyframe{f(3), 1.0}});
        const auto off = ok(edit::setAnimated(r.state, tl.seq, video, kOpacity, false, f(4)));
        CHECK(clipIn(off.state, tl.seq, "V1@0").properties()[ParamId::Opacity] == ParamValue(0.5));
    }
    SECTION("values stay in range and parameters on the right kind of clip") {
        CHECK_FALSE(edit::addKeyframe(tl.state, tl.seq, video, kOpacity, key(1, 1.5)));
        CHECK_FALSE(edit::addKeyframe(tl.state, tl.seq, video, ParamRef::of(ParamId::VolumeDb), key(1, -6.0)));
        CHECK(edit::addKeyframe(tl.state, tl.seq, tl.clip("A1@0"), ParamRef::of(ParamId::VolumeDb), key(1, -6.0)));
        CHECK_FALSE(edit::moveKeyframe(tl.state, tl.seq, video, kOpacity, 0, f(1))); // not animated
    }
}

TEST_CASE("Keyframes never change on a locked track", "[model][keyframes]") {
    Timeline tl(R"(
        V1 lock |Aaaa|
    )");
    CHECK_FALSE(edit::addKeyframe(tl.state, tl.seq, tl.clip("V1@0"), kOpacity, key(1, 0.0)));
}

// --- Keys follow the media ------------------------------------------------------------------------------------

TEST_CASE("Keys stick to the media through razor, slip and trim", "[model][keyframes]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa|
    )");
    auto r = ok(edit::addKeyframe(tl.state, tl.seq, tl.clip("V1@0"), kOpacity, key(2, 0.0)));
    r = ok(edit::addKeyframe(r.state, tl.seq, tl.clip("V1@0"), kOpacity, key(8, 1.0)));
    const ProjectState animated = r.state;

    SECTION("a razor cut gives both halves the curve; values are seamless across the cut") {
        const auto cut = ok(edit::addEdit(animated, tl.seq, f(5), std::vector<TrackId>{tl.track("V1")}));
        const Clip& left = clipIn(cut.state, tl.seq, "V1@0");
        const Clip& right = clipIn(cut.state, tl.seq, "V1@5");
        CHECK(left.sharesContentWith(right));
        for (int frame = 0; frame < 10; ++frame) {
            const Clip& piece = frame < 5 ? left : right;
            INFO("frame " << frame);
            CHECK(piece.parameterAt(ParamId::Opacity, f(frame)) ==
                  clipIn(animated, tl.seq, "V1@0").parameterAt(ParamId::Opacity, f(frame)));
        }
        // Each half can then be animated on its own.
        const auto own = ok(edit::addKeyframe(cut.state, tl.seq, right.id, kOpacity, key(9, 0.0)));
        CHECK(clipIn(own.state, tl.seq, "V1@0").properties()[ParamId::Opacity].curve()->keys.size() == 2);
        CHECK(clipIn(own.state, tl.seq, "V1@5").properties()[ParamId::Opacity].curve()->keys.size() == 3);
    }
    SECTION("a slip moves the keys with the picture (Premiere)") {
        const auto slipped = ok(edit::slip(animated, tl.seq, std::vector<ClipId>{tl.clip("V1@0")}, f(2)));
        const Clip& c = clipIn(slipped.state, tl.seq, "V1@0");
        CHECK(c.parameterAt(ParamId::Opacity, f(0)) == 0.0);  // shows media frame 2: the first key
        CHECK(c.parameterAt(ParamId::Opacity, f(6)) == 1.0);  // media frame 8: the second key
        CHECK(c.properties() == clipIn(animated, tl.seq, "V1@0").properties()); // the curve itself is untouched
    }
    SECTION("a trim hides keys outside the clip but keeps them") {
        const auto trimmed = ok(edit::trimEdge(animated, tl.seq, tl.clip("V1@0"), edit::Edge::Head, f(4)));
        const Clip& c = clipIn(trimmed.state, tl.seq, "V1@4");
        CHECK(c.properties()[ParamId::Opacity].curve()->keys.size() == 2);
        CHECK_FALSE(c.timelineTimeOfCurveTime(f(2))); // the first key lies before the clip
        CHECK(c.timelineTimeOfCurveTime(f(8)) == f(8));
        const auto back = ok(edit::trimEdge(trimmed.state, tl.seq, c.id, edit::Edge::Head, -f(4)));
        CHECK(clipIn(back.state, tl.seq, "V1@0").parameterAt(ParamId::Opacity, f(2)) == 0.0);
    }
}

TEST_CASE("Curve edits touch only one clip", "[model][keyframes][perf]") {
    std::string picture = "V1 tgt |";
    for (int i = 0; i < 200; ++i) {
        picture += "Aa";
    }
    Timeline tl(picture + "|");
    const ClipId middle = tl.clip("V1@200");
    const auto r = ok(edit::addKeyframe(tl.state, tl.seq, middle, kOpacity, key(1, 0.5)));
    REQUIRE(r.changes.sequences.size() == 1);
    CHECK(r.changes.sequences[0].modifiedClips == std::vector<ClipId>{middle});
    CHECK(r.changes.sequences[0].tracks[0].ranges.size() == 1);
}

TEST_CASE("Curves round-trip through the project file", "[model][keyframes][io]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa|
        A1 tgt |Aaaaaaaaaa|
    )");
    Keyframe bezier = key(4, 0.75, Interpolation::Bezier);
    bezier.in = {-0.5, 0.25};
    bezier.out = {2.0, 0.6};
    auto r = ok(edit::addKeyframe(tl.state, tl.seq, tl.clip("V1@0"), kOpacity, key(1, 0.0, Interpolation::EaseOut)));
    r = ok(edit::addKeyframe(r.state, tl.seq, tl.clip("V1@0"), kOpacity, bezier));
    r = ok(edit::addKeyframe(r.state, tl.seq, tl.clip("A1@0"), ParamRef::of(ParamId::VolumeDb),
                             key(9, -144.0, Interpolation::Hold)));
    const auto text = io::toJson(r.state);
    REQUIRE(text);
    CHECK(text->find("\"keys\"") != std::string::npos);
    CHECK(text->find("\"easeOut\"") != std::string::npos);
    const auto loaded = io::fromJson(*text);
    REQUIRE(loaded);
    CHECK(*loaded == r.state);

    // Malformed curves are rejected.
    std::string broken = *text;
    const auto at = broken.find("\"easeOut\"");
    broken.replace(at, 9, "\"wobbly\"");
    CHECK_FALSE(io::fromJson(broken));
}

TEST_CASE("Several keys move together, clamped to the clip, the moved key winning", "[model][keyframes]") {
    // The clip shows media 10..30: keys move inside that curve-time range.
    Timeline tl(R"(
        V1 tgt |A10aaaaaaaaaaaaaaaaa|
    )");
    const ClipId a = tl.clip("V1@0");
    ProjectState s = tl.state;
    for (const auto& [frame, value] : {std::pair{12, 0.0}, {15, 0.25}, {20, 0.5}, {25, 0.75}}) {
        s = ok(edit::addKeyframe(s, tl.seq, a, kOpacity, key(frame, value))).state;
    }
    const auto times = [&](const ProjectState& state) {
        std::vector<std::int64_t> out;
        for (const Keyframe& k : clipIn(state, tl.seq, "V1@0").properties()[ParamId::Opacity].curve()->keys) {
            out.push_back(ticksToFrames(k.time, kRate));
        }
        return out;
    };
    const std::size_t middle[] = {1, 2};
    const std::size_t last[] = {2, 3};
    const std::size_t first[] = {0};

    SECTION("by a delta, keeping their spacing") {
        const auto r = ok(edit::moveKeyframes(s, tl.seq, a, kOpacity, middle, f(3)));
        CHECK(times(r.state) == std::vector<std::int64_t>{12, 18, 23, 25});
        CHECK(r.appliedDelta == f(3));
    }
    SECTION("clamped so the last moved key stays inside the clip") {
        const auto r = ok(edit::moveKeyframes(s, tl.seq, a, kOpacity, last, f(10)));
        CHECK(times(r.state) == std::vector<std::int64_t>{12, 15, 25, 30});
        CHECK(r.appliedDelta == f(5));
        const auto back = ok(edit::moveKeyframes(s, tl.seq, a, kOpacity, first, f(-10)));
        CHECK(back.appliedDelta == f(-2));
    }
    SECTION("a moved key landing on another replaces it") {
        const auto r = ok(edit::moveKeyframes(s, tl.seq, a, kOpacity, first, f(3)));
        CHECK(times(r.state) == std::vector<std::int64_t>{15, 20, 25});
        CHECK(clipIn(r.state, tl.seq, "V1@0").properties()[ParamId::Opacity].curve()->keys[0].value == 0.0);
    }
    SECTION("refused: a key twice, a missing key, a constant parameter") {
        const std::size_t twice[] = {1, 1};
        const std::size_t missing[] = {9};
        CHECK_FALSE(edit::moveKeyframes(s, tl.seq, a, kOpacity, twice, f(1)));
        CHECK_FALSE(edit::moveKeyframes(s, tl.seq, a, kOpacity, missing, f(1)));
        CHECK_FALSE(edit::moveKeyframes(s, tl.seq, a, ParamRef::of(ParamId::Scale), first, f(1)));
    }
}
