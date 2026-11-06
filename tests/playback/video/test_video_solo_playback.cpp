// 0.7 (card 106): video track solo in the evaluator, which the Program Monitor, exports and the preview renderer all
// use: while any video track is soloed only the soloed tracks make layers, and the eye (output) still hides a track.

#include "TimelineFixture.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/playback/video/Evaluate.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace wabcut;
using namespace wabcut::playback;
using wabcut::test::f;
using wabcut::test::ok;

namespace {

std::vector<LayerSpec> layersAt(const ProjectState& state, SequenceId seq, std::int64_t frame) {
    std::vector<LayerSpec> layers;
    // Without culling: an opaque full-frame layer on top would otherwise hide the ones under it from the list.
    evaluateVideoLayers(state, *state.findSequence(seq), f(frame), layers, /*cullOccluded=*/false);
    return layers;
}

} // namespace

TEST_CASE("The evaluator makes layers for the soloed video tracks only (card 106)", "[playback][evaluate][solo]") {
    test::Timeline tl(R"(
        V3     |Cccc|
        V2 solo|Bbbb|
        V1     |Aaaa|
    )");
    SECTION("only the soloed track draws") {
        const auto layers = layersAt(tl.state, tl.seq, 1);
        REQUIRE(layers.size() == 1);
        CHECK(layers[0].media == tl.media('B'));
    }
    SECTION("without a solo every track draws") {
        edit::TrackStateChange change;
        change.solo = false;
        const ProjectState none = ok(edit::setTrackState(tl.state, tl.seq, tl.track("V2"), change)).state;
        CHECK(layersAt(none, tl.seq, 1).size() == 3);
    }
    SECTION("a soloed track that is switched off draws nothing, and neither do the others") {
        edit::TrackStateChange change;
        change.enabled = false;
        const ProjectState off = ok(edit::setTrackState(tl.state, tl.seq, tl.track("V2"), change)).state;
        CHECK(layersAt(off, tl.seq, 1).empty());
    }
    SECTION("soloing a second track adds it") {
        edit::TrackStateChange change;
        change.solo = true;
        const ProjectState two = ok(edit::setTrackState(tl.state, tl.seq, tl.track("V3"), change)).state;
        const auto layers = layersAt(two, tl.seq, 1);
        REQUIRE(layers.size() == 2);
        const bool hasB = layers[0].media == tl.media('B') || layers[1].media == tl.media('B');
        const bool hasC = layers[0].media == tl.media('C') || layers[1].media == tl.media('C');
        CHECK(hasB);
        CHECK(hasC);
    }
}
