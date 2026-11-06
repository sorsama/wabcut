// 0.7, batch A8 through the command engine: clip.restoreUnrendered (card 069). Render and Replace itself runs in the export
// queue (a menu action); the engine restores what it left.

#include "CommandHarness.h"

#include <wabcut/model/RenderReplace.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace wabcut;
using namespace wabcut::test;
using command::Json;

namespace {

Json at(const char* track, int frame) { return Json{{"track", track}, {"time", tc(frame)}}; }

/// Several sequences now (the nest): commands say which one.
Json in(const CommandHarness& h, Json params) {
    params["sequence"] = h.tl.seq.value;
    return params;
}

} // namespace

TEST_CASE("clip.restoreUnrendered plays the nest again in one undo step and refuses clips that were not rendered (card 069)",
          "[command][render-replace]") {
    CommandHarness h(R"(
        V1 tgt |AaaaBbbb|
        A1 tgt |AaaaBbbb|
    )");
    h.ok("sequence.nest", Json{{"clips", Json::array({at("V1", 0), at("A1", 0)})}, {"name", "Nest"}});
    const ClipId video = h.clip("V1@0");
    const ClipId audio = h.clip("A1@0");
    const Clip nestedVideo = h.sequence(h.state()).findClip(video)->clip();
    REQUIRE(nestedVideo.isNested());

    // Nothing was rendered yet.
    CHECK(h.errorOf("clip.restoreUnrendered", in(h, Json{{"clips", Json::array({at("V1", 0)})}})) == "EditRejected");

    // Render and Replace (the bridge's step), then restore through the engine.
    const ClipId ids[] = {video};
    const auto plan = edit::planRenderAndReplace(*h.state(), h.tl.seq, ids, Ticks{});
    REQUIRE(plan);
    MediaItem file = *h.state()->findMedia(h.tl.media('A'));
    file.name = "Rendered";
    REQUIRE(h.session.apply("Render and Replace", [&](const ProjectState& s) {
        return edit::renderAndReplace(s, h.tl.seq, plan->front().clips, file, plan->front().range.start);
    }));
    REQUIRE_FALSE(h.sequence(h.state()).findClip(video)->clip().isNested());
    REQUIRE_FALSE(h.sequence(h.state()).findClip(audio)->clip().isNested());

    const std::size_t steps = h.session.undoCount();
    h.ok("clip.restoreUnrendered", in(h, Json{{"clips", Json::array({at("V1", 0)})}}));
    CHECK(h.session.undoCount() == steps + 1);
    CHECK(h.session.undoLabel() == "Restore Unrendered");
    CHECK(h.sequence(h.state()).findClip(video)->clip() == nestedVideo);
    CHECK(h.sequence(h.state()).findClip(audio)->clip().isNested()); // the linked sound came along

    // Undo brings the file back; the engine says plainly what it cannot do.
    REQUIRE(h.session.undo());
    CHECK_FALSE(h.sequence(h.state()).findClip(video)->clip().isNested());
    CHECK(h.errorOf("clip.restoreUnrendered", in(h, Json{{"clips", Json::array({at("V1", 99)})}})) != "");
}
