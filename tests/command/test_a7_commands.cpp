// 0.7, batch A7 through the command engine: several markers at once (card 119: marker.removeMany, marker.moveMany,
// marker.duplicate), notes on clips (card 125: clip.setNote), keyframes that follow edits (card 120: clip.pinKeyframe,
// sequence.setSettings rippleTrackKeyframes).

#include "CommandHarness.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;
using command::Json;

namespace {

constexpr const char* kBase = R"(
    V1 tgt |Aaaaaaaaaaaa|
    A1 tgt |Aaaaaaaaaaaa|
)";

void addMarkers(CommandHarness& h, std::initializer_list<std::pair<int, const char*>> markers) {
    for (const auto& [frame, name] : markers) {
        h.ok("marker.add", {{"time", tc(frame)}, {"name", name}});
    }
}

std::string layout(const CommandHarness& h) {
    std::string out;
    for (const Marker& m : h.sequence(h.state()).markers) {
        if (!out.empty()) {
            out += ' ';
        }
        out += m.name + "@" + std::to_string(ticksToFrames(m.start, kRate));
    }
    return out;
}

} // namespace

TEST_CASE("marker.removeMany removes several markers in one undo step (card 119)", "[command][markers][group]") {
    CommandHarness h(kBase);
    addMarkers(h, {{1, "a"}, {3, "b"}, {5, "c"}, {7, "d"}});
    const std::size_t steps = h.session.undoCount();
    const auto r = h.ok("marker.removeMany", {{"indexes", Json::array({0, 2})}});
    CHECK(r.data["removed"] == 2);
    CHECK(layout(h) == "b@3 d@7");
    CHECK(h.session.undoCount() == steps + 1);
    REQUIRE(h.session.undo());
    CHECK(layout(h) == "a@1 b@3 c@5 d@7");

    CHECK(h.errorOf("marker.removeMany", {{"indexes", Json::array({9})}}) == "NotFound");
    CHECK(h.errorOf("marker.removeMany", {{"indexes", Json::array()}}) == "InvalidParams");
    CHECK(h.errorOf("marker.removeMany", {{"indexes", Json::array({-1})}}) == "InvalidParams");
    CHECK(layout(h) == "a@1 b@3 c@5 d@7");
}

TEST_CASE("marker.moveMany moves a group by one amount and reports the new indices (card 119)", "[command][markers][group]") {
    CommandHarness h(kBase);
    addMarkers(h, {{2, "a"}, {4, "b"}, {9, "c"}, {12, "d"}});
    const auto r = h.ok("marker.moveMany", {{"indexes", Json::array({1, 2})}, {"by", tc(6)}});
    CHECK(layout(h) == "a@2 b@10 d@12 c@15");
    CHECK(r.data["indexes"] == Json::array({1, 3}));
    CHECK(r.data["moved"] == 2);

    // Earlier than 0 is cut short: the earliest of the group stops at 0.
    const auto early = h.ok("marker.moveMany", {{"indexes", Json::array({0, 3})}, {"by", "-" + tc(30)}});
    CHECK(layout(h) == "a@0 b@10 d@12 c@13");
    CHECK(early.data["applied"]["ticks"] == -f(2).value);
    CHECK(h.errorOf("marker.moveMany", {{"indexes", Json::array({7})}, {"by", tc(1)}}) == "NotFound");
}

TEST_CASE("marker.duplicate copies a group at an offset (card 119)", "[command][markers][group]") {
    CommandHarness h(kBase);
    h.ok("marker.add", {{"time", tc(2)}, {"name", "a"}, {"colorIndex", 2}});
    h.ok("marker.add", {{"time", tc(4)}, {"name", "b"}, {"duration", tc(2)}, {"type", "chapter"}});
    const auto r = h.ok("marker.duplicate", {{"indexes", Json::array({0, 1})}, {"offset", tc(6)}});
    CHECK(layout(h) == "a@2 b@4 a@8 b@10");
    CHECK(r.data["indexes"] == Json::array({2, 3}));
    const auto& markers = h.sequence(h.state()).markers;
    CHECK(markers[2].colorIndex == 2);
    CHECK(markers[3].type == MarkerType::Chapter);
    CHECK(markers[3].duration == f(2));
    CHECK(h.errorOf("marker.duplicate", {{"indexes", Json::array({0})}, {"offset", tc(0)}}) == "InvalidParams");
    CHECK(h.errorOf("marker.duplicate", {{"indexes", Json::array({0})}, {"offset", "-" + tc(3)}}) == "InvalidParams");
    CHECK(layout(h) == "a@2 b@4 a@8 b@10");
}

TEST_CASE("clip.setNote writes a note on clips; clip.details shows it; one undo step (card 125)", "[command][note]") {
    CommandHarness h(kBase);
    const Json clips = Json::array({Json{{"track", "V1"}, {"time", tc(0)}}});
    CHECK(h.ok("clip.details", {{"clip", Json{{"track", "V1"}, {"time", tc(0)}}}}).data.contains("note") == false);
    const std::size_t steps = h.session.undoCount();
    h.ok("clip.setNote", {{"clips", clips}, {"note", "Cut the cough at 0:04"}});
    CHECK(h.session.undoCount() == steps + 1);
    CHECK(h.session.undoLabel() == "Set Clip Note");
    CHECK(h.ok("clip.details", {{"clip", Json{{"track", "V1"}, {"time", tc(0)}}}}).data["note"] == "Cut the cough at 0:04");
    CHECK(h.state()->findSequence(h.tl.seq)->findClip(h.clip("V1@0"))->clip().note() == "Cut the cough at 0:04");
    CHECK(h.state()->findSequence(h.tl.seq)->findClip(h.clip("A1@0"))->clip().note().empty()); // exactly the named clips

    h.ok("clip.setNote", {{"clips", clips}, {"note", ""}});
    CHECK(h.state()->findSequence(h.tl.seq)->findClip(h.clip("V1@0"))->clip().note().empty());
    CHECK(h.errorOf("clip.setNote", {{"clips", clips}, {"note", std::string(kMaxClipNoteBytes + 1, 'n')}}) == "EditRejected");

    // A locked clip refuses a note like any other change.
    h.ok("clip.lock", {{"clips", clips}, {"locked", true}});
    CHECK(h.errorOf("clip.setNote", {{"clips", clips}, {"note", "x"}}) != "");
}

TEST_CASE("clip.pinKeyframe pins and unpins a key; a trim through the engine moves the pinned key (card 120)",
          "[command][keyframes][pin]") {
    CommandHarness h(kBase);
    const Json clip = Json{{"track", "V1"}, {"time", tc(0)}};
    h.ok("clip.addKeyframe", {{"clip", clip}, {"param", "opacity.opacity"}, {"time", tc(0)}, {"value", 0.0}});
    h.ok("clip.addKeyframe", {{"clip", clip}, {"param", "opacity.opacity"}, {"time", tc(4)}, {"value", 1.0}});
    h.ok("clip.pinKeyframe", {{"clip", clip}, {"param", "opacity.opacity"}, {"index", 0}, {"pin", "start"}});
    const auto details = h.ok("clip.details", {{"clip", clip}});
    CHECK(details.data["params"]["opacity.opacity"]["pins"] == Json::array({Json::array({0, "start"})}));

    // Trimming the head 3 frames through the command engine: the pinned key goes with the new first frame.
    h.ok("clip.trim", {{"clip", clip}, {"edge", "head"}, {"delta", tc(3)}});
    const Curve* curve = h.state()->findSequence(h.tl.seq)->findClip(h.clip("V1@3"))->clip().properties()[ParamId::Opacity].curve();
    REQUIRE(curve != nullptr);
    REQUIRE(curve->keys.size() == 2);
    CHECK(curve->keys[0].time == f(3));
    CHECK(curve->keys[0].value == 0.0);
    CHECK(curve->keys[0].pin == KeyPin::ClipStart);
    CHECK(curve->keys[1].time == f(4));

    h.ok("clip.pinKeyframe", {{"clip", Json{{"track", "V1"}, {"time", tc(3)}}}, {"param", "opacity.opacity"}, {"index", 0}, {"pin", "none"}});
    CHECK(h.errorOf("clip.pinKeyframe", {{"clip", Json{{"track", "V1"}, {"time", tc(3)}}}, {"param", "opacity.opacity"}, {"index", 9}, {"pin", "end"}}) != "");
    CHECK(h.errorOf("clip.pinKeyframe", {{"clip", Json{{"track", "V1"}, {"time", tc(3)}}}, {"param", "opacity.opacity"}, {"index", 0}, {"pin", "middle"}}) != "");
}

TEST_CASE("sequence.setSettings rippleTrackKeyframes turns the track keyframes' ripple on and shows in the summary (card 120)",
          "[command][keyframes][ripple]") {
    CommandHarness h(kBase);
    CHECK_FALSE(h.sequence(h.state()).rippleTrackKeyframes);
    h.ok("sequence.setSettings", {{"rippleTrackKeyframes", true}});
    CHECK(h.sequence(h.state()).rippleTrackKeyframes);
    CHECK(h.session.undoCount() == 1);
    h.ok("sequence.setSettings", {{"rippleTrackKeyframes", false}});
    CHECK_FALSE(h.sequence(h.state()).rippleTrackKeyframes);
}

TEST_CASE("sequence.mediaUsage reads the video solo like the picture does: only the soloed track's media is used (review fix, card 106)",
          "[command][solo][usage]") {
    CommandHarness h(R"(
        V2 solo |....Bbbb....|
        V1      |Aaaaaaaaaaaa|
    )");
    const auto soloed = h.ok("sequence.mediaUsage");
    REQUIRE(soloed.data["report"]["uses"].size() == 1);
    CHECK(soloed.data["report"]["uses"][0]["track"] == "V2");
    // Without the solo both tracks are used.
    h.ok("track.setState", {{"track", "V2"}, {"solo", false}});
    CHECK(h.ok("sequence.mediaUsage").data["report"]["uses"].size() == 2);
}
