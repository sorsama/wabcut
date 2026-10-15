// The finishing commands: keyframes, fades, effects, transitions, speed and frame holds, through the engine.

#include "CommandHarness.h"

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::test;

namespace {

const Clip& clipOf(const CommandHarness& h, std::string_view ref) {
    static ProjectSnapshot keep;
    keep = h.state();
    return h.sequence(keep).findClip(clipAt(*keep, h.tl.seq, ref))->clip();
}

Json at(const char* track, int frame) { return Json{{"track", track}, {"time", tc(frame)}}; }

} // namespace

TEST_CASE("Keyframe commands take timeline times; keys live in media time", "[command][finishing][keyframes]") {
    CommandHarness h(R"(
        V1 tgt |A5aaaaaaaaa|
    )");
    // The clip shows media frames 5..14 at timeline 0..9: a key at 00:00:00:02 sits on media frame 7.
    h.ok("clip.addKeyframe", Json{{"clip", at("V1", 0)}, {"param", "opacity.opacity"}, {"time", tc(2)}, {"value", 0}});
    h.ok("clip.addKeyframe", Json{{"clip", at("V1", 0)}, {"param", "opacity.opacity"}, {"time", tc(6)}, {"value", 1},
                                  {"interpolation", "easeInOut"}});
    const Clip& c = clipOf(h, "V1@0");
    REQUIRE(c.properties()[ParamId::Opacity].isCurve());
    CHECK(c.properties()[ParamId::Opacity].curve()->keys[0].time == f(7));
    CHECK(c.parameterAt(ParamId::Opacity, f(2)) == 0.0);
    CHECK(c.parameterAt(ParamId::Opacity, f(6)) == 1.0);
    CHECK(c.parameterAt(ParamId::Opacity, f(4)) > 0.5); // linear departure, eased (flat) arrival

    const Json d = h.ok("clip.details", Json{{"clip", at("V1", 0)}}).data;
    const Json& keys = d["params"]["opacity.opacity"]["keys"];
    REQUIRE(keys.size() == 2);
    CHECK(keys[0] == Json::array({0, tc(2), f(7).value, 0.0, "linear"}));
    CHECK(keys[1][4] == "easeInOut");
    CHECK(d["keyFields"][2] == "mediaTime");

    h.ok("clip.moveKeyframe", Json{{"clip", at("V1", 0)}, {"param", "opacity.opacity"}, {"index", 0}, {"time", tc(1)}});
    h.ok("clip.setKeyframe", Json{{"clip", at("V1", 0)}, {"param", "opacity.opacity"}, {"index", 1},
                                  {"interpolation", "bezier"}, {"inSlope", 0.5}, {"inInfluence", 0.5}});
    CHECK(clipOf(h, "V1@0").properties()[ParamId::Opacity].curve()->keys[1].in == BezierHandle{0.5, 0.5});
    CHECK(h.errorOf("clip.addKeyframe", Json{{"clip", at("V1", 0)}, {"param", "opacity.opacity"}, {"time", tc(12)},
                                             {"value", 0}}) == "InvalidParams"); // outside the clip
    CHECK(h.errorOf("clip.addKeyframe", Json{{"clip", at("V1", 0)}, {"param", "opacity.opacity"}, {"time", tc(3)},
                                             {"value", 7}}) == "EditRejected"); // out of range
    CHECK(h.errorOf("clip.addKeyframe", Json{{"clip", at("V1", 0)}, {"param", "glow"}, {"time", tc(3)}, {"value", 1}}) ==
          "InvalidParams");
    h.ok("clip.removeKeyframe", Json{{"clip", at("V1", 0)}, {"param", "opacity.opacity"}, {"index", 0}});
    h.ok("clip.setAnimated", Json{{"clip", at("V1", 0)}, {"param", "opacity.opacity"}, {"animated", false}});
    CHECK(clipOf(h, "V1@0").properties()[ParamId::Opacity].isConstant());
}

TEST_CASE("clip.fade fades video opacity and audio level", "[command][finishing][keyframes]") {
    CommandHarness h(R"(
        V1 tgt |Aaaaaaaaaa|
        A1 tgt |Aaaaaaaaaa|
    )");
    h.ok("clip.fade", Json{{"clips", Json::array({at("V1", 0), at("A1", 0)})}, {"edge", "both"}, {"duration", tc(2)}});
    const Clip& v = clipOf(h, "V1@0");
    CHECK(v.parameterAt(ParamId::Opacity, f(0)) == 0.0);
    CHECK(v.parameterAt(ParamId::Opacity, f(1)) == 0.5);
    CHECK(v.parameterAt(ParamId::Opacity, f(5)) == 1.0);
    CHECK(v.parameterAt(ParamId::Opacity, f(9)) == 0.5);
    const Clip& a = clipOf(h, "A1@0");
    CHECK(a.parameterAt(ParamId::VolumeDb, f(0)) == -144.0);
    CHECK(a.parameterAt(ParamId::VolumeDb, f(5)) == 0.0);
    CHECK(h.session.undoCount() == 1);
}

TEST_CASE("Effect commands", "[command][finishing][effects]") {
    CommandHarness h(R"(
        V1 tgt |Aaaaaaaa|
        A1 tgt |Aaaaaaaa|
    )");
    const Json list = h.ok("effect.list", Json{{"kind", "audio"}}).data;
    CHECK(list["paramFields"][0] == "key");
    for (const Json& e : list["effects"]) {
        CHECK(e["kind"] == "audio");
    }
    CHECK(h.ok("effect.list").data["effects"].size() == EffectRegistry::builtin().all().size());

    const auto added = h.ok("clip.addEffect", Json{{"clips", Json::array({at("V1", 0)})},
                                                   {"effect", "wabcut.color.basic"},
                                                   {"params", Json{{"exposure", 0.5}}}});
    REQUIRE(added.createdIds.size() == 1);
    const std::uint64_t color = added.createdIds[0];
    const auto blur = h.ok("clip.addEffect", Json{{"clips", Json::array({at("V1", 0)})}, {"effect", "wabcut.blur.gaussian"},
                                                  {"index", 0}})
                          .createdIds[0];
    CHECK(clipOf(h, "V1@0").effects()[0].id.value == blur);
    h.ok("clip.setEffect", Json{{"clip", at("V1", 0)}, {"effect", color}, {"enabled", false}, {"params", Json{{"tint", 20}}}});
    h.ok("clip.moveEffect", Json{{"clip", at("V1", 0)}, {"effect", color}, {"index", 0}});
    h.ok("clip.addKeyframe", Json{{"clip", at("V1", 0)}, {"effect", blur}, {"param", "radius"}, {"time", tc(4)}, {"value", 20}});
    const Json d = h.ok("clip.details", Json{{"clip", at("V1", 0)}}).data;
    REQUIRE(d["effects"].size() == 2);
    CHECK(d["effects"][0]["effect"] == "wabcut.color.basic");
    CHECK(d["effects"][0]["enabled"] == false);
    CHECK(d["effects"][0]["params"]["tint"] == 20.0);
    CHECK(d["effects"][1]["params"]["radius"]["keys"].size() == 1);
    CHECK(d["effects"][1]["known"] == true);

    CHECK(h.errorOf("clip.addEffect", Json{{"clips", Json::array({at("A1", 0)})}, {"effect", "wabcut.crop"}}) ==
          "EditRejected");
    CHECK(h.errorOf("clip.addEffect", Json{{"clips", Json::array({at("V1", 0)})}, {"effect", "wabcut.opacity"}}) ==
          "InvalidParams"); // not in the stack-effect enum
    CHECK(h.errorOf("clip.setEffect", Json{{"clip", at("V1", 0)}, {"effect", color}, {"params", Json{{"tint", 500}}}}) ==
          "EditRejected");
    h.ok("clip.removeEffect", Json{{"clip", at("V1", 0)}, {"effect", blur}});
    CHECK(clipOf(h, "V1@0").effects().size() == 1);
    CHECK(h.errorOf("clip.removeEffect", Json{{"clip", at("V1", 0)}, {"effect", blur}}) == "NotFound");
}

TEST_CASE("Transition commands", "[command][finishing][transitions]") {
    CommandHarness h(R"(
        V1 tgt |AaaaaaaaB9bbbbbbb|
        A1 tgt |AaaaaaaaB9bbbbbbb|
    )");
    SECTION("add, preview, list, change, remove") {
        const auto preview = h.run(Json{{"command", "transition.add"},
                                        {"params", Json{{"clip", at("V1", 0)}, {"edge", "tail"}, {"duration", tc(6)}}},
                                        {"dryRun", true}});
        REQUIRE(preview.ok());
        CHECK(preview.createdIds.size() == 1);
        CHECK(h.session.undoCount() == 0);
        const auto added = h.ok("transition.add", Json{{"clip", at("V1", 0)}, {"edge", "tail"}, {"duration", tc(6)}});
        const std::uint64_t id = added.createdIds[0];
        const Json summary = h.ok("sequence.summary").data;
        CHECK(summary["transitionFields"][2] == "cut");
        CHECK(summary["tracks"][0]["transitions"][0] == Json::array({id, "crossDissolve", tc(8), tc(3), tc(3)}));
        const Json d = h.ok("clip.details", Json{{"clip", at("V1", 9)}}).data;
        CHECK(d["transitions"][0]["edge"] == "head");

        h.ok("transition.set", Json{{"transition", id}, {"duration", tc(4)}, {"alignment", "endAtCut"}, {"effect", "dipToWhite"}});
        const Transition& t = h.sequence(h.state()).videoTracks[0]->transitions[0];
        CHECK(t.before == f(4));
        CHECK(t.after == Ticks{});
        CHECK(t.effect == "dipToWhite");
        h.ok("transition.remove", Json{{"transition", id}});
        CHECK(h.sequence(h.state()).videoTracks[0]->transitions.empty());
    }
    SECTION("handles: shortened by default, an error with fit exact") {
        // A tail head has 9 frames of media before B's In; ask for 12 frames before the cut.
        CHECK(h.errorOf("transition.add", Json{{"clip", at("V1", 9)}, {"edge", "head"}, {"duration", tc(12)},
                                               {"alignment", "endAtCut"}, {"fit", "exact"}}) == "EditRejected");
        h.ok("transition.add", Json{{"clip", at("V1", 9)}, {"edge", "head"}, {"duration", tc(12)}, {"alignment", "endAtCut"}});
        CHECK(h.sequence(h.state()).videoTracks[0]->transitions[0].before == f(8)); // A is 8 frames long
        CHECK(h.errorOf("transition.add", Json{{"clip", at("A1", 0)}, {"edge", "tail"}, {"effect", "crossDissolve"}}) ==
              "EditRejected"); // a video transition on an audio track
    }
    SECTION("Ctrl+D, Ctrl+Shift+D, Shift+D") {
        h.ok("sequence.applyVideoTransition", Json{{"time", tc(7)}, {"duration", tc(4)}});
        CHECK(h.sequence(h.state()).videoTracks[0]->transitions.size() == 1);
        h.ok("sequence.applyAudioTransition", Json{{"time", tc(7)}});
        CHECK(h.sequence(h.state()).audioTracks[0]->transitions[0].effect == "constantPower");
        CHECK(h.errorOf("sequence.applyAudioTransition", Json{{"time", tc(7)}}) == "EditRejected");
        h.ok("sequence.applyDefaultTransitions",
             Json{{"clips", Json::array({at("V1", 0)})}, {"videoDuration", tc(2)}});
        CHECK(h.sequence(h.state()).videoTracks[0]->transitions.size() == 2); // A's head; its tail already has one
    }
}

TEST_CASE("Speed/Duration and frame hold commands", "[command][finishing][speed]") {
    CommandHarness h(Timeline(R"(
        V1 tgt |AaaaaaaaBb..|
        A1 tgt |AaaaaaaaBb..|
    )")
                         .linkMatching());
    SECTION("percent with ripple") {
        h.ok("clip.speedDuration", Json{{"clips", Json::array({at("V1", 0)})}, {"speed", 200}, {"ripple", true}});
        CHECK(clipOf(h, "V1@0").duration() == f(4));
        CHECK(clipOf(h, "A1@4").name() == "B");
        CHECK(h.session.undoCount() == 1);
    }
    SECTION("a duration, reversed, pitch kept") {
        h.ok("clip.speedDuration", Json{{"clips", Json::array({at("V1", 0)})}, {"duration", tc(4)}, {"reverse", true},
                                        {"maintainPitch", true}});
        const Clip& v = clipOf(h, "V1@0");
        CHECK(v.duration() == f(4));
        CHECK(v.speed == Speed{2, 1});
        CHECK(v.reversed);
        CHECK(clipOf(h, "A1@0").maintainPitch);
        const Json d = h.ok("clip.details", Json{{"clip", at("V1", 0)}}).data;
        CHECK(d["reversed"] == true);
        CHECK(d["speed"] == "2/1");
        CHECK(h.errorOf("clip.speedDuration", Json{{"clips", Json::array({at("V1", 0)})}, {"speed", 50}, {"duration", tc(4)}}) ==
              "InvalidParams");
    }
    SECTION("Maintain Audio Pitch is on by default where a clip leaves 100 percent (A.10)") {
        h.ok("clip.speedDuration", Json{{"clips", Json::array({at("V1", 0)})}, {"speed", 50}, {"ripple", true}});
        CHECK(clipOf(h, "A1@0").maintainPitch); // the linked audio follows with the default
        // Turned off explicitly, a later change keeps it off; back at 100 % the setting stays as it was.
        h.ok("clip.speedDuration", Json{{"clips", Json::array({at("A1", 0)})}, {"speed", 25}, {"maintainPitch", false},
                                        {"ripple", true}});
        CHECK_FALSE(clipOf(h, "A1@0").maintainPitch);
        h.ok("clip.speedDuration", Json{{"clips", Json::array({at("A1", 0)})}, {"speed", 40}, {"ripple", true}});
        CHECK_FALSE(clipOf(h, "A1@0").maintainPitch);
        h.ok("clip.speedDuration", Json{{"clips", Json::array({at("A1", 0)})}, {"speed", 100}, {"ripple", true}});
        CHECK_FALSE(clipOf(h, "A1@0").maintainPitch);
        // Time remapping turns it on too (unless asked not to), in the same undo step.
        const auto undos = h.session.undoCount();
        h.ok("timeRemap.enable", Json{{"clip", at("A1", 0)}});
        CHECK(clipOf(h, "A1@0").maintainPitch);
        CHECK(clipOf(h, "A1@0").isRemapped());
        CHECK(h.session.undoCount() == undos + 1);
    }
    SECTION("frame hold") {
        h.ok("clip.frameHold", Json{{"clip", at("V1", 0)}, {"time", tc(3)}, {"duration", tc(2)}});
        CHECK(clipOf(h, "V1@3").isFreeze());
        CHECK(h.ok("clip.details", Json{{"clip", at("V1", 3)}}).data["frameHold"] == true);
        CHECK(clipOf(h, "V1@10").name() == "B");
        CHECK(h.errorOf("clip.frameHold", Json{{"clip", at("A1", 0)}, {"time", tc(1)}}) == "EditRejected");
    }
}

TEST_CASE("Finishing commands batch into one undo step", "[command][finishing][batch]") {
    CommandHarness h(R"(
        V1 tgt |AaaaaaaaB9bbbbbbb|
    )");
    const auto r = h.run(Json{
        {"commands",
         Json::array({Json{{"command", "clip.fade"}, {"params", Json{{"clips", Json::array({at("V1", 0)})}, {"edge", "in"},
                                                                    {"duration", tc(2)}}}},
                      Json{{"command", "transition.add"}, {"params", Json{{"clip", at("V1", 0)}, {"edge", "tail"},
                                                                          {"duration", tc(4)}}}},
                      Json{{"command", "clip.addEffect"},
                           {"params", Json{{"clips", Json::array({at("V1", 9)})}, {"effect", "wabcut.vignette"}}}}})},
        {"label", "Polish"}});
    INFO(command::toJson(r).dump());
    REQUIRE(r.ok());
    CHECK(h.session.undoCount() == 1);
    CHECK(h.session.undoLabel() == "Polish");
    REQUIRE(h.session.undo());
    CHECK(clipOf(h, "V1@0").properties()[ParamId::Opacity].isConstant());
    CHECK(h.sequence(h.state()).videoTracks[0]->transitions.empty());
}

TEST_CASE("clip.speedDuration with keepDuration keeps the clip's length", "[command][finishing][speed]") {
    CommandHarness h(R"(
        V1 tgt |AaaaaaaaBb|
    )");
    h.ok("clip.speedDuration", Json{{"clips", Json::array({at("V1", 0)})}, {"speed", 200}, {"keepDuration", true}});
    const Clip& c = clipOf(h, "V1@0");
    CHECK(c.speed == Speed{2, 1});
    CHECK(c.duration() == f(8));
    CHECK(c.sourceDuration() == f(16));
    CHECK(clipOf(h, "V1@8").name() == "B");
    CHECK(h.errorOf("clip.speedDuration", Json{{"clips", Json::array({at("V1", 0)})},
                                               {"duration", tc(4)},
                                               {"keepDuration", true}}) == "InvalidParams");
}
