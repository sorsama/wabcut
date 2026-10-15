#include "FakeTransport.h"
#include <wabcut/ai/generation/Conform.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/model/ProjectOps.h>
#include <catch2/catch_test_macros.hpp>
using namespace wabcut;
TEST_CASE("Generated video conforms frame clock, dimensions, duration and reversed head", "[ai][generation][conform]") {
    edit::SequenceSettings settings; settings.width = 320; settings.height = 180; settings.frameRate = Rate{24, 1};
    auto added = edit::addSequence({}, settings); REQUIRE(added);
    auto timed = edit::setSequenceDuration(added->state, added->id, Ticks::fromSeconds(2)); REQUIRE(timed);
    auto input = ai::test::uniqueTempPath("generated-synthetic", ".mp4");
    exporter::ExportRequest request; request.snapshot = std::make_shared<const ProjectState>(timed->state);
    request.sequence = added->id; request.output = input; request.settings.audio.enabled = false;
    request.settings.video.encoder = "libx264"; request.videoSource = exporter::syntheticVideoSourceFactory();
    auto rendered = exporter::ExportJob(std::move(request)).run(); INFO((rendered ? "ok" : rendered.error().message)); REQUIRE(rendered);
    auto target = *timed->state.findSequence(added->id); target.width = 160; target.height = 90; target.frameRate = kRate25;
    for (const bool reverse : {false, true}) {
        auto output = ai::test::uniqueTempPath(reverse ? "conform-head" : "conform-tail", ".mp4");
        auto conformed = ai::generation::conformVideo(input, output, target, Ticks::fromSeconds(1), reverse, {});
        INFO((conformed ? "ok" : conformed.error().message)); REQUIRE(conformed);
        REQUIRE(conformed->video.size() == 1);
        CHECK(conformed->video[0].width == 160); CHECK(conformed->video[0].height == 90);
        CHECK(conformed->video[0].frameRate == kRate25);
        CHECK(std::abs(conformed->duration.toSeconds() - 1.0) < 0.041);
        auto frame = ai::generation::boundaryFrame(ai::test::uniqueTempPath("missing").string(), {}); CHECK_FALSE(frame);
        std::filesystem::remove(output);
    }
    CHECK_FALSE(ai::generation::conformVideo(input, ai::test::uniqueTempPath("too-long", ".mp4"), target, Ticks::fromSeconds(3), false, {}));
    std::filesystem::remove(input);
}
