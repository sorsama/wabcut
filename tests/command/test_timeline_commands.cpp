// 0.2 timeline parity commands: clip.group, clip.ungroup and clip.setFade (CatalogTimeline.cpp).

#include "CommandHarness.h"

#include <wabcut/command/Registry.h>
#include <wabcut/model/ClipFades.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::test;

TEST_CASE("clip.group and clip.ungroup are one undo step each", "[command][timeline]") {
    CommandHarness h(R"(
        V2 |..Cc..|
        V1 |AaBb..|
    )");
    const ClipId a = h.clip("V1@0");
    const ClipId c = h.clip("V2@2");
    h.ok("clip.group", Json{{"clips", Json::array({a.value, c.value})}});
    CHECK(h.session.undoLabel() == "Group");
    const auto grouped = h.state();
    const ClipGroupId group = h.sequence(grouped).findClip(a)->clip().group;
    CHECK(group.isValid());
    CHECK(h.sequence(grouped).findClip(c)->clip().group == group);
    CHECK_FALSE(h.sequence(grouped).findClip(h.clip("V1@2"))->clip().group.isValid());

    h.ok("clip.ungroup", Json{{"clips", Json::array({c.value})}});
    CHECK(h.session.undoLabel() == "Ungroup");
    CHECK_FALSE(h.sequence(h.state()).findClip(a)->clip().group.isValid());
    REQUIRE(h.session.undo());
    CHECK(h.sequence(h.state()).findClip(a)->clip().group == group);

    CHECK(h.errorOf("clip.group", Json{{"clips", Json::array({a.value})}}) == "InvalidParams"); // minItems 2
    CHECK_FALSE(h.errorOf("clip.group", Json{{"clips", Json::array({a.value, 999999})}}).empty());
    // The aliases resolve.
    CHECK(command::builtinCommands().find("clips.group")->name == "clip.group");
    CHECK(command::builtinCommands().find("clips.ungroup")->name == "clip.ungroup");
    CHECK(command::builtinCommands().find("clip.setFade")->name == "clip.setFade"); // clip.fade is the keyframe fade
}

TEST_CASE("clip.setFade adds changes and removes a clip's fade", "[command][timeline]") {
    CommandHarness h(R"(
        V1 |Aaaaaaaaaa|
        A1 |Aaaaaaaaaa|
    )");
    const ClipId v = h.clip("V1@0");
    const ClipId a = h.clip("A1@0");
    auto r = h.ok("clip.setFade", Json{{"clip", v.value}, {"edge", "head"}, {"duration", tc(4)}});
    CHECK(h.session.undoLabel() == "Set Fade");
    CHECK(r.data["fade"]["timecode"] == tc(4));
    CHECK(edit::clipFade(h.sequence(h.state()), v, edit::Edge::Head) == f(4));

    r = h.ok("clip.setFade", Json{{"clip", a.value}, {"edge", "tail"}, {"duration", tc(30)}});
    CHECK(r.data["fade"]["timecode"] == tc(10)); // clamped to the clip
    const Track& a1 = *h.sequence(h.state()).findTrack(h.tl.track("A1"));
    REQUIRE(a1.transitions.size() == 1);
    CHECK(a1.transitions[0].effect == "constantPower");

    r = h.ok("clip.setFade", Json{{"clip", v.value}, {"edge", "head"}, {"duration", 0}});
    CHECK(r.data["fade"].is_null());
    CHECK(h.sequence(h.state()).findTrack(h.tl.track("V1"))->transitions.empty());
    CHECK(h.errorOf("clip.setFade", Json{{"clip", v.value}, {"edge", "middle"}, {"duration", 0}}) == "InvalidParams");
}
