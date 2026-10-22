// P.2 through the engine: clip.setTimeInterpolation and the Speed/Duration dialog's Time Interpolation preview in a dry
// run and commit in one undo step; Morph Cut goes on an edit point like any video transition.

#include "CommandHarness.h"

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/MotionOps.h>
#include <wabcut/model/TimeInterpolation.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace wabcut;
using namespace wabcut::test;
using command::Json;

namespace {

Json dryRun(std::string_view name, Json params) {
    return Json{{"command", name}, {"params", std::move(params)}, {"dryRun", true}};
}

} // namespace

TEST_CASE("clip.setTimeInterpolation previews and commits in one undo step", "[command][motion]") {
    CommandHarness h(R"(
        V1 tgt |Aaaa..Bbbb|
        A1 tgt |Aaaa..Bbbb|
    )");
    const ClipId a = h.clip("V1@0");
    const auto interpolationOf = [&](ClipId id) { return h.sequence(h.state()).findClip(id)->clip().timeInterpolation(); };
    const auto preview =
        h.run(dryRun("clip.setTimeInterpolation", Json{{"clips", Json::array({a.value})}, {"method", "opticalFlow"}}));
    REQUIRE(preview.ok());
    CHECK(interpolationOf(a) == TimeInterpolation::FrameSampling);
    CHECK(h.session.undoCount() == 0);

    const auto r = h.ok("clip.setTimeInterpolation", Json{{"clips", Json::array({a.value})}, {"method", "opticalFlow"}});
    CHECK(r.data["method"] == "opticalFlow");
    CHECK(interpolationOf(a) == TimeInterpolation::OpticalFlow);
    CHECK(h.session.undoCount() == 1);
    h.session.undo();
    CHECK(interpolationOf(a) == TimeInterpolation::FrameSampling);

    // Unknown methods and audio-only selections are refused.
    CHECK_FALSE(h.run(Json{{"command", "clip.setTimeInterpolation"},
                           {"params", Json{{"clips", Json::array({a.value})}, {"method", "motionBlur"}}}})
                    .ok());
    const ClipId audio = h.clip("A1@0");
    CHECK_FALSE(h.run(Json{{"command", "clip.setTimeInterpolation"},
                           {"params", Json{{"clips", Json::array({audio.value})}, {"method", "frameBlending"},
                                           {"linked", false}}}})
                    .ok());
}

TEST_CASE("clip.speedDuration sets the speed and the Time Interpolation in one step", "[command][motion]") {
    CommandHarness h(R"(
        V1 tgt |Aaaa......|
        A1 tgt |Aaaa......|
    )");
    const ClipId v = h.clip("V1@0");
    const auto clip = [&](ClipId id) { return h.sequence(h.state()).findClip(id)->clip(); };
    h.ok("clip.speedDuration",
         Json{{"clips", Json::array({v.value})}, {"speed", 50}, {"timeInterpolation", "frameBlending"}});
    CHECK(clip(v).speed == Speed{1, 2});
    CHECK(clip(v).timeInterpolation() == TimeInterpolation::FrameBlending);
    CHECK(h.session.undoCount() == 1);
    h.session.undo();
    CHECK(clip(v).speed == Speed{1, 1});
    CHECK(clip(v).timeInterpolation() == TimeInterpolation::FrameSampling);

    // Only the Time Interpolation (the dialog with the speed untouched): also on a time-remapped clip.
    h.ok("timeRemap.enable", Json{{"clip", v.value}, {"enabled", true}});
    h.ok("clip.speedDuration", Json{{"clips", Json::array({v.value})}, {"timeInterpolation", "opticalFlow"}});
    CHECK(clip(v).isRemapped());
    CHECK(clip(v).timeInterpolation() == TimeInterpolation::OpticalFlow);
}

TEST_CASE("Morph Cut goes on an edit point as a video transition", "[command][motion]") {
    CommandHarness h(R"(
        V1 tgt |A10aaaaaaaA40aaaaaaa|
    )");
    h.ok("transition.add", Json{{"clip", h.clip("V1@0").value},
                                {"edge", "tail"},
                                {"effect", "morphCut"},
                                {"duration", "00:00:00:06"}});
    const Track& v1 = *h.sequence(h.state()).videoTracks.front();
    REQUIRE(v1.transitions.size() == 1);
    CHECK(v1.transitions.front().effect == kMorphCutTransition);
    CHECK(v1.transitions.front().incoming == h.clip("V1@10")); // on the cut, two-sided
    CHECK(h.session.undoCount() == 1);
}
