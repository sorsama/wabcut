// 0.7, batch A10 through the command engine: keyframes.ease (card 96's easing library for scripts and the assistant) and Motion
// Blur (card 58) as an effect added and keyed like any other.

#include "CommandHarness.h"

#include <wabcut/model/MotionBlur.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace wabcut;
using namespace wabcut::test;
using command::Json;
using Catch::Approx;

namespace {

constexpr const char* kBase = R"(
    V1 tgt |Aaaaaaaaaaaa|
    A1 tgt |Aaaaaaaaaaaa|
)";

const Curve& opacityCurve(const CommandHarness& h, const char* clip = "V1@0") {
    const Curve* curve = h.state()->findSequence(h.tl.seq)->findClip(h.clip(clip))->clip().properties()[ParamId::Opacity].curve();
    REQUIRE(curve != nullptr);
    return *curve;
}

} // namespace

TEST_CASE("keyframes.ease shapes the segment after the keys like the preset, in one undo step (card 96)", "[command][keyframes][easing][a10]") {
    CommandHarness h(kBase);
    const Json clip = Json{{"track", "V1"}, {"time", tc(0)}};
    h.ok("clip.addKeyframe", {{"clip", clip}, {"param", "opacity.opacity"}, {"time", tc(0)}, {"value", 0.0}});
    h.ok("clip.addKeyframe", {{"clip", clip}, {"param", "opacity.opacity"}, {"time", tc(4)}, {"value", 1.0}});
    h.ok("clip.addKeyframe", {{"clip", clip}, {"param", "opacity.opacity"}, {"time", tc(8)}, {"value", 0.5}});
    const Curve before = opacityCurve(h);
    const std::size_t steps = h.session.undoCount();

    const auto r = h.ok("keyframes.ease", {{"clip", clip}, {"param", "opacity.opacity"}, {"keys", Json::array({0})}, {"preset", "easeInOutCubic"}});
    CHECK(r.data["segments"] == 1);
    CHECK(h.session.undoCount() == steps + 1);
    const Curve& eased = opacityCurve(h);
    CHECK(eased.keys[0].interpolation == Interpolation::Bezier);
    CHECK(eased.keys[0].out.slope == 0.0);
    CHECK(eased.keys[1].interpolation == Interpolation::Bezier);
    CHECK(eased.keys[1].in.slope == 0.0);
    // The first segment now starts slow; the second is as it was.
    CHECK(eased.valueAt(f(1)) < before.valueAt(f(1)));
    CHECK(eased.valueAt(f(6)) == Approx(before.valueAt(f(6))).margin(1e-9));
    REQUIRE(h.session.undo());
    CHECK(opacityCurve(h) == before);

    // Without "keys" every key's segment is eased (the last has none).
    const auto all = h.ok("keyframes.ease", {{"clip", clip}, {"param", "opacity.opacity"}, {"preset", "easeOutBack"}});
    CHECK(all.data["segments"] == 2);
    CHECK(opacityCurve(h).valueAt(f(3)) > 1.0); // Back overshoots on the way to the key
    REQUIRE(h.session.undo());

    CHECK(h.errorOf("keyframes.ease", {{"clip", clip}, {"param", "opacity.opacity"}, {"preset", "bounce"}}) != "");
    CHECK(h.errorOf("keyframes.ease", {{"clip", clip}, {"param", "opacity.opacity"}, {"keys", Json::array({2})}, {"preset", "easeIn"}}) != ""); // the last key has no segment
    CHECK(h.errorOf("keyframes.ease", {{"clip", clip}, {"param", "opacity.opacity"}, {"keys", Json::array({9})}, {"preset", "easeIn"}}) != "");
    CHECK(h.errorOf("keyframes.ease", {{"clip", clip}, {"param", "motion.scale"}, {"preset", "easeIn"}}) != ""); // not animated
    CHECK(opacityCurve(h) == before);
}

TEST_CASE("Motion Blur is an effect any clip can take and key like another (card 58)", "[command][effects][motionblur][a10]") {
    CommandHarness h(kBase);
    const auto added = h.ok("clip.addEffect", {{"clips", Json::array({Json{{"track", "V1"}, {"time", tc(0)}}})},
                                                {"effect", "wabcut.blur.motion"},
                                                {"params", Json{{"shutterAngle", 270.0}, {"samples", 32}}}});
    REQUIRE(added.createdIds.size() == 1);
    const Clip& clip = h.state()->findSequence(h.tl.seq)->findClip(h.clip("V1@0"))->clip();
    REQUIRE(clip.effects().size() == 1);
    const auto blur = motionBlurOf(clip.effects(), Ticks{});
    REQUIRE(blur.has_value());
    CHECK(blur->angle == 270.0);
    CHECK(blur->samples == 32);
    CHECK(blur->phase == -90.0);
    // It is listed with the video effects, in Blur & Sharpen.
    const auto list = h.ok("effect.list", {{"kind", "video"}});
    bool found = false;
    for (const auto& e : list.data["effects"]) {
        if (e["id"] == "wabcut.blur.motion") {
            found = true;
            CHECK(e["category"] == "Blur & Sharpen");
        }
    }
    CHECK(found);
    // Not on an audio clip.
    CHECK(h.errorOf("clip.addEffect", {{"clips", Json::array({Json{{"track", "A1"}, {"time", tc(0)}}})}, {"effect", "wabcut.blur.motion"}}) != "");
}
