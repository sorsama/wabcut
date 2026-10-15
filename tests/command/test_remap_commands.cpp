// C.6 commands through the engine: time remapping (timeRemap.*), several keyframes at once (keyframes.move,
// keyframes.setInterpolation and the keyframes.add/remove aliases), stored transition alignment, and where
// title.add puts a title.

#include "CommandHarness.h"

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;
using command::Json;

namespace {

Json at(const char* track, int frame) { return Json{{"track", track}, {"time", tc(frame)}}; }

const Clip& clipOf(const CommandHarness& h, std::string_view ref) {
    return h.sequence(h.state()).findClip(h.clip(ref))->clip();
}

/// Source frames the clip shows at timeline frames [from, to).
std::vector<std::int64_t> shown(const Clip& c, std::int64_t from, std::int64_t to) {
    std::vector<std::int64_t> out;
    for (std::int64_t k = from; k < to; ++k) {
        out.push_back(ticksToFrames(c.sourceTimeAt(f(k)), kRate));
    }
    return out;
}

std::vector<std::int64_t> opacityKeyFrames(const Clip& c) {
    std::vector<std::int64_t> out;
    for (const Keyframe& k : c.properties()[ParamId::Opacity].curve()->keys) {
        out.push_back(ticksToFrames(k.time, kRate));
    }
    return out;
}

Timeline linked(std::string_view picture) {
    Timeline tl(picture);
    tl.linkMatching();
    return tl;
}

} // namespace

TEST_CASE("timeRemap: on, keys, segment speeds, holds, reverse, off", "[command][remap]") {
    CommandHarness h(linked(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaa..........|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaa..........|
    )"));
    const ProjectSnapshot before = h.state();
    h.ok("timeRemap.enable", Json{{"clip", at("V1", 0)}});
    CHECK(clipOf(h, "V1@0").isRemapped());
    CHECK(clipOf(h, "A1@0").isRemapped()); // linked partners too
    CHECK(h.session.undoCount() == 1);
    const auto details = h.ok("clip.details", Json{{"clip", at("V1", 0)}});
    CHECK(details.data["timeRemap"]["keys"][0] == Json::array({0, Json{{"ticks", 0}, {"timecode", tc(0)}},
                                                               Json{{"ticks", 0}, {"timecode", tc(0)}}, "linear"}));
    CHECK(details.data["timeRemap"]["keys"][1][1] == nullptr); // the media's end lies past the clip
    CHECK(details.data["timeRemap"]["segments"] ==
          Json::array({Json::array({Json{{"ticks", 0}, {"timecode", tc(0)}}, Json{{"ticks", f(20).value}, {"timecode", tc(20)}},
                                    "forward", 100.0})}));

    // A key at 10, then the first segment at 50 % with the clip lengthened by what it gained.
    CHECK(h.ok("timeRemap.addKey", Json{{"clip", at("V1", 0)}, {"time", tc(10)}}).data["index"] == 1);
    const auto slowed = h.ok("timeRemap.setSpeed",
                             Json{{"clip", at("V1", 0)}, {"time", tc(4)}, {"speed", 50}, {"extendClip", true}});
    CHECK(slowed.data["segment"] == 0);
    CHECK(clipOf(h, "V1@0").duration() == f(30));
    CHECK(clipOf(h, "A1@0").duration() == f(30));
    CHECK(shown(clipOf(h, "V1@0"), 18, 23) == std::vector<std::int64_t>{9, 9, 10, 11, 12});
    CHECK(shown(clipOf(h, "A1@0"), 18, 23) == std::vector<std::int64_t>{9, 9, 10, 11, 12});
    CHECK(clipOf(h, "A1@0").unitySpeedRanges() == std::vector<TimeRange>{TimeRange::fromStartEnd(f(20), f(30))});

    // A two-frame hold at 20 (the clip keeps its length), a ramp, a reverse boomerang, a key removed.
    h.ok("timeRemap.addHold", Json{{"clip", at("V1", 0)}, {"time", tc(20)}, {"duration", tc(2)}});
    CHECK(shown(clipOf(h, "V1@0"), 19, 24) == std::vector<std::int64_t>{9, 10, 10, 10, 11});
    h.ok("timeRemap.setInterpolation", Json{{"clip", at("V1", 0)}, {"keys", Json::array({0})}, {"interpolation", "easeIn"}});
    CHECK(clipOf(h, "V1@0").timeRemap()->keys[0].interpolation == Interpolation::EaseIn);
    h.ok("timeRemap.addReverse", Json{{"clip", at("V1", 0)}, {"time", tc(24)}, {"duration", tc(2)}});
    CHECK(shown(clipOf(h, "V1@0"), 24, 29) == std::vector<std::int64_t>{11, 10, 10, 11, 12});
    const std::size_t keys = clipOf(h, "V1@0").timeRemap()->keys.size();
    h.ok("timeRemap.removeKey", Json{{"clip", at("V1", 0)}, {"index", 2}});
    CHECK(clipOf(h, "V1@0").timeRemap()->keys.size() == keys - 1);
    CHECK(clipOf(h, "A1@0").timeRemap()->keys.size() == keys - 1);

    // Off again: back to a constant speed (100 % from the first frame here), then undo everything.
    h.ok("timeRemap.enable", Json{{"clip", at("V1", 0)}, {"enabled", false}});
    CHECK_FALSE(clipOf(h, "V1@0").isRemapped());
    CHECK(clipOf(h, "V1@0").speed == Speed{1, 1});
    while (h.session.canUndo()) {
        h.session.undo();
    }
    CHECK(*h.state() == *before);
}

TEST_CASE("timeRemap: moving keys with ripple, dry runs and refusals", "[command][remap]") {
    CommandHarness h(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    )");
    CHECK(h.errorOf("timeRemap.addKey", Json{{"clip", at("V1", 0)}, {"time", tc(3)}}) == "EditRejected"); // not on
    h.ok("timeRemap.enable", Json{{"clip", at("V1", 0)}});
    h.ok("timeRemap.addKey", Json{{"clip", at("V1", 0)}, {"time", tc(10)}});
    const ProjectSnapshot keyed = h.state();
    const auto dry = h.run(Json{{"command", "timeRemap.moveKey"},
                                {"params", Json{{"clip", at("V1", 0)}, {"index", 1}, {"time", tc(5)}, {"ripple", true}}},
                                {"dryRun", true}});
    REQUIRE(dry.ok());
    CHECK(*h.state() == *keyed);
    h.ok("timeRemap.moveKey", Json{{"clip", at("V1", 0)}, {"index", 1}, {"time", tc(5)}, {"ripple", true}});
    CHECK(shown(clipOf(h, "V1@0"), 0, 8) == std::vector<std::int64_t>{0, 2, 4, 6, 8, 10, 11, 12});
    CHECK(clipOf(h, "V1@0").duration() == f(20)); // no extendClip: the length stays
    CHECK(h.errorOf("timeRemap.moveKey", Json{{"clip", at("V1", 0)}, {"index", 1}, {"time", tc(30)}}) == "InvalidParams");
    CHECK(h.errorOf("timeRemap.setSpeed", Json{{"clip", at("V1", 0)}, {"speed", 50}}) == "InvalidParams");
    CHECK(h.errorOf("timeRemap.removeKey", Json{{"clip", at("V1", 0)}, {"index", 9}}) == "NotFound");
    CHECK(h.errorOf("clip.speedDuration", Json{{"clips", Json::array({at("V1", 0)})}, {"speed", 200}}) == "EditRejected");
}

TEST_CASE("timeRemap.setKey edits source positions and nonzero Bezier tangents atomically", "[command][remap]") {
    CommandHarness h(linked(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )"));
    CHECK(h.errorOf("timeRemap.setKey", Json{{"clip", at("V1", 0)}, {"index", 0}}) == "EditRejected");
    h.ok("timeRemap.enable", Json{{"clip", at("V1", 0)}});
    h.ok("timeRemap.addKey", Json{{"clip", at("V1", 0)}, {"time", tc(10)}});
    h.ok("timeRemap.addKey", Json{{"clip", at("V1", 0)}, {"time", tc(20)}});
    const ProjectSnapshot before = h.state();
    const auto undos = h.session.undoCount();
    const Json params{{"clip", at("V1", 0)}, {"index", 1}, {"source", f(15).value},
                      {"interpolation", "bezier"}, {"inSlope", kTicksPerSecond}, {"outSlope", kTicksPerSecond},
                      {"inInfluence", 0.25}, {"outInfluence", 0.25}};
    REQUIRE(h.run(Json{{"command", "timeRemap.setKey"}, {"params", params}, {"dryRun", true}}).ok());
    CHECK(*h.state() == *before);
    h.ok("timeRemap.setKey", params);
    CHECK(h.session.undoCount() == undos + 1);
    const Clip& clip = clipOf(h, "V1@0");
    const Keyframe key = clip.timeRemap()->keys[1];
    CHECK(key.value == static_cast<double>(f(15).value));
    CHECK(key.in == BezierHandle{static_cast<double>(kTicksPerSecond), 0.25});
    CHECK(key.out == key.in);
    CHECK(clip.timeRemap()->keys == clipOf(h, "A1@0").timeRemap()->keys);
    CHECK(clip.sourceTimeAt(f(10)) == f(15));
    Ticks previous = clip.sourceTimeAt(Ticks{});
    for (int i = 1; i <= 1000; ++i) {
        const Ticks now = clip.sourceTimeAt(Ticks{f(20).value * i / 1000});
        CHECK(now >= previous);
        previous = now;
    }
    // Both sides of the key retain a nonzero 1x tangent (within floating point rounding).
    const Ticks sample{1000000};
    CHECK((clip.sourceTimeAt(f(10)) - clip.sourceTimeAt(f(10) - sample)).value >= 999900);
    CHECK((clip.sourceTimeAt(f(10) + sample) - clip.sourceTimeAt(f(10))).value >= 999900);
    const auto good = h.state();
    CHECK(h.errorOf("timeRemap.setKey", Json{{"clip", at("V1", 0)}, {"index", 1}, {"outSlope", -1.0}}) == "EditRejected");
    CHECK(h.errorOf("timeRemap.setKey", Json{{"clip", at("V1", 0)}, {"index", 1}, {"source", f(1001).value}}) == "EditRejected");
    CHECK(h.errorOf("timeRemap.setKey", Json{{"clip", at("V1", 0)}, {"index", 1}, {"outInfluence", 1.1}}) == "InvalidParams");
    CHECK(h.errorOf("timeRemap.setKey", Json{{"clip", at("V1", 0)}, {"index", 1},
                                             {"outSlope", std::numeric_limits<double>::infinity()}}) == "InvalidParams");
    CHECK(h.errorOf("timeRemap.setKey", Json{{"clip", at("V1", 0)}, {"index", 9}}) == "NotFound");
    CHECK(*h.state() == *good);
    CHECK(h.session.undoCount() == undos + 1);
    // A partial handle edit keeps the other component and all other key fields.
    h.ok("timeRemap.setKey", Json{{"clip", at("V1", 0)}, {"index", 1}, {"outInfluence", 0.2}});
    CHECK(clipOf(h, "V1@0").timeRemap()->keys[1].out.slope == key.out.slope);
    h.session.undo();
    h.session.undo();
    CHECK(*h.state() == *before);
}

TEST_CASE("timeRemap.setKey makes bounded holds and reverse segments", "[command][remap]") {
    CommandHarness h(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )");
    h.ok("timeRemap.enable", Json{{"clip", at("V1", 0)}});
    h.ok("timeRemap.addKey", Json{{"clip", at("V1", 0)}, {"time", tc(10)}});
    h.ok("timeRemap.addKey", Json{{"clip", at("V1", 0)}, {"time", tc(20)}});
    h.ok("timeRemap.setKey", Json{{"clip", at("V1", 0)}, {"index", 1}, {"source", f(15).value},
                                  {"interpolation", "hold"}});
    CHECK(shown(clipOf(h, "V1@0"), 10, 13) == std::vector<std::int64_t>{15, 15, 15});
    h.ok("timeRemap.setKey", Json{{"clip", at("V1", 0)}, {"index", 2}, {"source", f(5).value}});
    h.ok("timeRemap.setKey", Json{{"clip", at("V1", 0)}, {"index", 1}, {"interpolation", "linear"}});
    CHECK(shown(clipOf(h, "V1@0"), 10, 13) == std::vector<std::int64_t>{14, 13, 12});
    CHECK(clipOf(h, "V1@0").sourceTimeAt(f(20)) == f(5));
}

TEST_CASE("keyframes.move / setInterpolation and the keyframes.add / remove aliases", "[command][keyframes]") {
    CommandHarness h(R"(
        V1 tgt |A10aaaaaaaaaaaaaaaaa|
    )");
    const auto add = [&](const char* name, int frame, double value) {
        h.ok(name, Json{{"clip", at("V1", 0)}, {"param", "opacity.opacity"}, {"time", tc(frame)}, {"value", value}});
    };
    add("keyframes.add", 2, 0.0);
    add("clip.addKeyframe", 5, 0.25);
    add("keyframes.add", 10, 0.5);
    add("keyframes.add", 15, 0.75);
    CHECK(opacityKeyFrames(clipOf(h, "V1@0")) == std::vector<std::int64_t>{12, 15, 20, 25});

    const auto moved = h.ok("keyframes.move", Json{{"clip", at("V1", 0)},
                                                   {"param", "opacity.opacity"},
                                                   {"keys", Json::array({1, 2})},
                                                   {"delta", tc(3)}});
    CHECK(moved.data["indices"] == Json::array({1, 2}));
    CHECK(moved.data["clamped"] == false);
    CHECK(opacityKeyFrames(clipOf(h, "V1@0")) == std::vector<std::int64_t>{12, 18, 23, 25});
    // Clamped to the clip's end, and a moved key replaces the one it lands on.
    const auto clamped =
        h.ok("keyframes.move", Json{{"clip", at("V1", 0)}, {"param", "opacity.opacity"}, {"keys", Json::array({3})},
                                    {"delta", tc(20)}});
    CHECK(clamped.data["clamped"] == true);
    CHECK(opacityKeyFrames(clipOf(h, "V1@0")) == std::vector<std::int64_t>{12, 18, 23, 30});
    const auto landed = h.ok("keyframes.move", Json{{"clip", at("V1", 0)},
                                                    {"param", "opacity.opacity"},
                                                    {"keys", Json::array({0})},
                                                    {"delta", "-00:00:00:00"}});
    CHECK(landed.data["clamped"] == false);
    h.ok("keyframes.move", Json{{"clip", at("V1", 0)}, {"param", "opacity.opacity"}, {"keys", Json::array({1})},
                                {"delta", tc(5)}});
    CHECK(opacityKeyFrames(clipOf(h, "V1@0")) == std::vector<std::int64_t>{12, 23, 30}); // 18 -> 23 replaced 23

    h.ok("keyframes.setInterpolation", Json{{"clip", at("V1", 0)}, {"param", "opacity.opacity"}, {"interpolation", "hold"}});
    for (const Keyframe& k : clipOf(h, "V1@0").properties()[ParamId::Opacity].curve()->keys) {
        CHECK(k.interpolation == Interpolation::Hold);
    }
    h.ok("keyframes.remove", Json{{"clip", at("V1", 0)}, {"param", "opacity.opacity"}, {"index", 0}});
    CHECK(opacityKeyFrames(clipOf(h, "V1@0")) == std::vector<std::int64_t>{23, 30});
    CHECK(h.errorOf("keyframes.setInterpolation",
                    Json{{"clip", at("V1", 0)}, {"param", "motion.scale"}, {"interpolation", "hold"}}) == "InvalidParams");
    CHECK(h.errorOf("keyframes.move", Json{{"clip", at("V1", 0)}, {"param", "opacity.opacity"},
                                           {"keys", Json::array({0, 0})}, {"delta", tc(1)}}) == "EditRejected");
}

TEST_CASE("Transition alignment is stored and kept", "[command][transitions]") {
    CommandHarness h(R"(
        V1 tgt |A100aaaaaaB100bbbbbbb|
    )");
    const auto transition = [&]() -> const Transition& { return h.sequence(h.state()).videoTracks[0]->transitions.front(); };
    h.ok("transition.add", Json{{"clip", at("V1", 0)}, {"edge", "tail"}, {"duration", tc(5)}, {"alignment", "startAtCut"}});
    CHECK(transition().alignment == TransitionAlignment::StartAtCut);
    const auto details = h.ok("clip.details", Json{{"clip", at("V1", 0)}});
    CHECK(details.data["transitions"][0]["alignment"] == "startAtCut");
    const auto id = static_cast<std::int64_t>(transition().id.value);
    h.ok("transition.set", Json{{"transition", id}, {"duration", tc(8)}});
    CHECK(transition().after == f(8)); // the new length keeps the alignment
    h.ok("transition.set", Json{{"transition", id}, {"alignment", "custom"}, {"offset", tc(1)}});
    CHECK(transition().alignment == TransitionAlignment::Custom);
    CHECK(transition().before == f(1));
    h.ok("transition.set", Json{{"transition", id}, {"duration", tc(6)}});
    CHECK(transition().before == f(1)); // custom keeps the cut
    CHECK(transition().after == f(5));
    h.ok("transition.set", Json{{"transition", id}, {"alignment", "center"}});
    CHECK(transition().before == f(3));
    CHECK(transition().alignment == TransitionAlignment::Center);
    h.ok("transition.remove", Json{{"transition", id}});
    h.ok("transition.add", Json{{"clip", at("V1", 0)}, {"edge", "tail"}, {"duration", tc(6)}, {"alignment", "custom"},
                                {"offset", tc(2)}});
    CHECK(transition().before == f(2));
    CHECK(transition().after == f(4));
    CHECK(transition().alignment == TransitionAlignment::Custom);
}

TEST_CASE("title.add placement: targeted, the first free track at or above, a new track", "[command][titles]") {
    const char* picture = R"(
        V2     |....Cccc..|
        V1 tgt |Aaaaaaaaaa|
    )";
    const auto add = [](CommandHarness& h, Json params) {
        params["text"] = "Title";
        return h.ok("title.add", std::move(params));
    };
    SECTION("without a track the first free one at or above the targeted track (C.16); a named track is overwritten") {
        CommandHarness h(picture);
        const auto r = add(h, Json{{"at", tc(0)}, {"duration", tc(4)}});
        CHECK(r.data["track"] == "V2"); // V1's clip is not overwritten
        CHECK(r.data["newTrack"] == false);
        CommandHarness named(picture);
        const auto onV1 = add(named, Json{{"at", tc(0)}, {"duration", tc(4)}, {"track", "V1"}});
        CHECK(onV1.data["track"] == "V1");
        CHECK(onV1.data["newTrack"] == false);
    }
    SECTION("freeTrackAtOrAbove skips busy tracks") {
        CommandHarness h(picture);
        const auto r = add(h, Json{{"at", tc(0)}, {"duration", tc(4)}, {"placement", "freeTrackAtOrAbove"}});
        CHECK(r.data["track"] == "V2");
        CommandHarness busy(picture);
        const auto top = add(busy, Json{{"at", tc(4)}, {"duration", tc(4)}, {"placement", "freeTrackAtOrAbove"}});
        CHECK(top.data["track"] == "V3"); // V1 and V2 are busy there: a new track on top
        CHECK(top.data["newTrack"] == true);
        CHECK(busy.sequence(busy.state()).videoTracks.size() == 3);
        CHECK(busy.session.undoCount() == 1);
    }
    SECTION("newTrack always makes one") {
        CommandHarness h(picture);
        const auto r = add(h, Json{{"at", tc(0)}, {"duration", tc(4)}, {"placement", "newTrack"}, {"track", "V1"}});
        CHECK(r.data["track"] == "V3");
        CHECK(h.errorOf("title.add", Json{{"text", "x"}, {"at", tc(0)}, {"placement", "elsewhere"}}) == "InvalidParams");
    }
}

namespace {

/// Real files: the audio stream of `letter` ends `frames` in, before its video stream.
Timeline withShortAudio(Timeline tl, char letter, std::int64_t frames) {
    auto media = std::make_shared<MediaItem>(*tl.state.findMedia(tl.media(letter)));
    for (auto& stream : media->info.audio) {
        stream.duration = f(frames);
    }
    for (MediaPtr& m : tl.state.media) {
        if (m->id == media->id) {
            m = media;
        }
    }
    return tl;
}

} // namespace

TEST_CASE("Review P1-5: linked video and audio of unequal stream lengths stay in sync through segment edits",
          "[command][remap][review]") {
    CommandHarness h(withShortAudio(linked(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaa..........|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaa..........|
    )"),
                                    'A', 900));
    const auto inSync = [&] {
        CHECK(*clipOf(h, "V1@0").timeRemap() == *clipOf(h, "A1@0").timeRemap());
        CHECK(clipOf(h, "V1@0").timelineRange == clipOf(h, "A1@0").timelineRange);
        CHECK(shown(clipOf(h, "V1@0"), 0, 30) == shown(clipOf(h, "A1@0"), 0, 30));
    };
    h.ok("timeRemap.enable", Json{{"clip", at("V1", 0)}});
    inSync();
    h.ok("timeRemap.addKey", Json{{"clip", at("V1", 0)}, {"time", tc(10)}});
    h.ok("timeRemap.setSpeed", Json{{"clip", at("V1", 0)}, {"time", tc(4)}, {"speed", 50}, {"extendClip", true}});
    CHECK(clipOf(h, "A1@0").duration() == f(30));
    inSync();
    CHECK(shown(clipOf(h, "A1@0"), 0, 4) == std::vector<std::int64_t>{0, 0, 1, 1}); // the audio slowed down too
    h.ok("timeRemap.moveKey", Json{{"clip", at("V1", 0)}, {"index", 1}, {"time", tc(16)}});
    h.ok("timeRemap.setInterpolation", Json{{"clip", at("V1", 0)}, {"keys", Json::array({1})}, {"interpolation", "hold"}});
    inSync();
    h.ok("timeRemap.removeKey", Json{{"clip", at("V1", 0)}, {"index", 1}});
    inSync();
}

TEST_CASE("Review P2-22: edits at a time skip linked partners that are not remapped", "[command][remap][review]") {
    CommandHarness h(linked(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    )"));
    h.ok("timeRemap.enable", Json{{"clip", at("V1", 0)}, {"linked", false}});
    REQUIRE_FALSE(clipOf(h, "A1@0").isRemapped());
    CHECK(h.ok("timeRemap.addKey", Json{{"clip", at("V1", 0)}, {"time", tc(10)}}).data["index"] == 1);
    h.ok("timeRemap.addHold", Json{{"clip", at("V1", 0)}, {"time", tc(4)}, {"duration", tc(2)}});
    h.ok("timeRemap.addReverse", Json{{"clip", at("V1", 0)}, {"time", tc(12)}, {"duration", tc(2)}});
    CHECK(clipOf(h, "V1@0").timeRemap()->keys.size() > 2);
    CHECK_FALSE(clipOf(h, "A1@0").isRemapped());
    // Named directly, an unremapped clip still refuses.
    CHECK(h.errorOf("timeRemap.addKey", Json{{"clip", at("A1", 0)}, {"time", tc(3)}}) == "EditRejected");
}

TEST_CASE("Review P1-7: match frame and reverse match frame read a remapped clip's media time",
          "[command][remap][review]") {
    CommandHarness h(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    )");
    // 200 % for the first 10 source frames, then 100 %: timeline 0..5 shows 0..10, 5..20 shows 10..25.
    h.ok("timeRemap.enable", Json{{"clip", at("V1", 0)}});
    h.ok("timeRemap.addKey", Json{{"clip", at("V1", 0)}, {"time", tc(10)}});
    h.ok("timeRemap.setSpeed", Json{{"clip", at("V1", 0)}, {"segment", 0}, {"speed", 200}});
    REQUIRE(shown(clipOf(h, "V1@0"), 4, 7) == std::vector<std::int64_t>{8, 10, 11});

    const auto r = h.ok("sequence.matchFrame", Json{{"time", tc(4)}});
    CHECK(r.data["sourceTime"]["timecode"] == tc(8));
    CHECK(r.data["marks"]["in"]["timecode"] == tc(0));
    CHECK(r.data["marks"]["out"]["timecode"] == tc(25)); // the media it shows, not its curve times
    CHECK(h.ok("clip.details", Json{{"clip", at("V1", 0)}}).data["timeRemap"]["shownMedia"]["end"]["timecode"] == tc(25));

    CHECK(h.ok("sequence.reverseMatchFrame", Json{{"media", "A"}, {"time", tc(12)}}).data["time"]["timecode"] == tc(7));
    CHECK(h.ok("sequence.reverseMatchFrame", Json{{"media", "A"}, {"time", tc(6)}}).data["time"]["timecode"] == tc(3));
    CHECK(h.errorOf("sequence.reverseMatchFrame", Json{{"media", "A"}, {"time", tc(30)}}) == "NotFound");
}
