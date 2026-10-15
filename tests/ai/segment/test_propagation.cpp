// AI-14 propagation: the prompt frame on the media's grid, forward / backward / both / none, stopping at the shot cuts
// around the prompt (C.5) unless asked to go across, In / Out, confidence and boxes per frame, progress, cancel and
// the model's and the decoder's failures.

#include "SegmentTestUtil.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::ai::segment;
using namespace wabcut::ai::segment::test;
using Catch::Approx;

namespace {

PropagationOptions optionsAt(int frame, double x, double y) {
    PropagationOptions o;
    o.prompt.time = frameTicks(frame) + Ticks{17}; // inside the frame: snapped to its start
    o.prompt.points = {PromptPoint{x, y, true}};
    return o;
}

int frameIndex(Ticks t) { return static_cast<int>(t.value / frameTicks(1).value); }

} // namespace

TEST_CASE("Propagation stops at the shot cuts around the prompt, unless across shots", "[ai][segment][propagation]") {
    ScriptedVideo video(ScriptedScene{200, 60});
    RedSegmenter model;
    PropagationOptions o = optionsAt(30, 0.3, 0.4);
    o.cuts = {frameTicks(10), frameTicks(60), frameTicks(120)};
    o.span = TimeRange{Ticks{}, frameTicks(200)};

    SECTION("both ways: frames 10..59, the cuts reported") {
        auto r = propagate(model, video, o);
        REQUIRE(r);
        REQUIRE(r->track.frames.size() == 50);
        CHECK(frameIndex(r->track.frames.front().time) == 10);
        CHECK(frameIndex(r->track.frames.back().time) == 59);
        REQUIRE(r->stoppedBefore);
        REQUIRE(r->stoppedAfter);
        CHECK(*r->stoppedBefore == frameTicks(10));
        CHECK(*r->stoppedAfter == frameTicks(60));
        CHECK(r->track.cuts == std::vector<Ticks>{frameTicks(10), frameTicks(60)});
        CHECK(model.prompts == 2); // the forward and the backward pass start from the prompt
        CHECK(r->lost == 0);
        for (std::size_t i = 1; i < r->track.frames.size(); ++i) {
            CHECK(r->track.frames[i].time - r->track.frames[i - 1].time == frameTicks(1));
        }
        // Each frame's box is where the object was.
        const ScriptedScene scene{200, 60};
        for (const MatteFrame& f : r->track.frames) {
            REQUIRE(f.box);
            const MatteBox truth = scene.objectAt(frameIndex(f.time));
            CHECK(f.box->x == Approx(truth.x).margin(0.01));
            CHECK(f.box->width == Approx(truth.width).margin(0.01));
            CHECK(f.confidence == Approx(0.9 / (1.0 + std::exp(-6.0))).margin(1e-4));
        }
        CHECK(r->track.width == 512);
        CHECK(r->track.height == 288);
        CHECK(r->track.prompts.front().time == frameTicks(30)); // snapped to the frame's start
    }
    SECTION("forward only and backward only") {
        o.direction = Direction::Forward;
        auto f = propagate(model, video, o);
        REQUIRE(f);
        CHECK(f->track.frames.size() == 30); // 30..59
        CHECK_FALSE(f->stoppedBefore);
        CHECK(f->stoppedAfter);
        o.direction = Direction::Backward;
        auto b = propagate(model, video, o);
        REQUIRE(b);
        CHECK(b->track.frames.size() == 21); // 10..30
        CHECK(b->stoppedBefore);
        CHECK_FALSE(b->stoppedAfter);
        o.direction = Direction::None;
        auto n = propagate(model, video, o);
        REQUIRE(n);
        CHECK(n->track.frames.size() == 1);
    }
    SECTION("across shots: the whole span; the object leaves the frame and comes back in the next shot") {
        o.acrossShots = true;
        auto r = propagate(model, video, o);
        REQUIRE(r);
        CHECK(r->track.frames.size() == 200);
        CHECK_FALSE(r->stoppedBefore);
        CHECK_FALSE(r->stoppedAfter);
        CHECK(r->track.cuts.empty());
    }
    SECTION("In / Out bound the run; a cut exactly at the prompt starts its shot") {
        o.span = TimeRange::fromStartEnd(frameTicks(20), frameTicks(40));
        auto r = propagate(model, video, o);
        REQUIRE(r);
        CHECK(frameIndex(r->track.frames.front().time) == 20);
        CHECK(frameIndex(r->track.frames.back().time) == 39);
        CHECK_FALSE(r->stoppedAfter);
        o.span.reset();
        o.cuts = {frameTicks(30)};
        auto at = propagate(model, video, o);
        REQUIRE(at);
        CHECK(frameIndex(at->track.frames.front().time) == 30);
        CHECK(*at->stoppedBefore == frameTicks(30));
    }
}

TEST_CASE("Propagation: the object lost, progress, cancel and failures", "[ai][segment][propagation]") {
    ScriptedVideo video(ScriptedScene{400, -1}); // one shot: the object walks out of the frame on the right
    RedSegmenter model;
    PropagationOptions o = optionsAt(100, 0.5, 0.4);
    o.direction = Direction::Forward;
    o.span = TimeRange{Ticks{}, frameTicks(250)};

    SECTION("frames without the object are kept (empty, not present) and counted as lost") {
        auto r = propagate(model, video, o);
        REQUIRE(r);
        CHECK(r->track.frames.size() == 150);
        CHECK(r->lost > 0);
        const MatteFrame& last = r->track.frames.back();
        CHECK_FALSE(last.present);
        CHECK_FALSE(last.box);
        CHECK(last.confidence < 0.01f);
        CHECK(r->framesPerSecond > 0.0);
        CHECK(r->device == "TestCPU");
    }
    SECTION("progress reports each frame; returning false cancels") {
        int calls = 0, frames = 0;
        double last = 0.0;
        o.span = TimeRange{Ticks{}, frameTicks(110)};
        o.progress = [&](double f, const std::string& what) {
            ++calls;
            CHECK(f >= last);
            last = f;
            CHECK_FALSE(what.empty());
            return true;
        };
        o.frameDone = [&](const MatteFrame&) { ++frames; };
        auto r = propagate(model, video, o);
        REQUIRE(r);
        CHECK(calls == 10);
        CHECK(frames == 10);
        CHECK(last == Approx(1.0));
        o.progress = [&](double, const std::string&) { return false; };
        auto cancelled = propagate(model, video, o);
        REQUIRE_FALSE(cancelled);
        CHECK(cancelled.error().code == ErrorCode::Cancelled);
    }
    SECTION("a stop token stops the run between frames") {
        std::stop_source stop;
        model.stopAfter = 5;
        model.stopSource = &stop;
        auto r = propagate(model, video, o, stop.get_token());
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::Cancelled);
        CHECK(model.tracked == 5);
    }
    SECTION("the model's and the decoder's errors end the run") {
        model.failOnTrack = 4;
        auto r = propagate(model, video, o);
        REQUIRE_FALSE(r);
        CHECK(r.error().message.find("scripted model failure") != std::string::npos);
        model.failOnTrack.reset();
        video.failAt = 103;
        auto d = propagate(model, video, o);
        REQUIRE_FALSE(d);
        CHECK(d.error().code == ErrorCode::DecoderError);
    }
    SECTION("a prompt outside the span, or none") {
        o.prompt.time = frameTicks(300);
        auto r = propagate(model, video, o);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::NotFound);
        o.prompt.time = frameTicks(100);
        o.prompt.points.clear();
        auto empty = propagate(model, video, o);
        REQUIRE_FALSE(empty);
        CHECK(empty.error().code == ErrorCode::InvalidArgument);
        o.prompt.box = MatteBox{0.4, 0.3, 0.2, 0.2};
        CHECK(propagate(model, video, o).has_value()); // a box alone is a prompt
    }
}

TEST_CASE("Direction names", "[ai][segment][propagation]") {
    for (const Direction d : {Direction::None, Direction::Forward, Direction::Backward, Direction::Both}) {
        CHECK(directionFromString(toString(d)) == d);
    }
    CHECK_FALSE(directionFromString("sideways"));
}
