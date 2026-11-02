// 0.6 (card #74): playback and export of auditions. The evaluator sees only the active take of an audition - exactly as
// it sees a plain clip of that take (media, source time, motion, effects) - and never the other takes; switching takes
// changes the picture without touching anything else. The render key (the preview cache's and the export's identity of
// what a range shows) is the plain clip's too, so a render preview made before the audition stays valid.

#include "TimelineFixture.h"

#include <wabcut/model/Auditions.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/playback/video/Evaluate.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace wabcut;
using namespace wabcut::playback;
using wabcut::test::f;
using wabcut::test::ok;

namespace {

const Clip& clipOf(const ProjectState& state, SequenceId seq, ClipId id) {
    const auto loc = state.findSequence(seq)->findClip(id);
    REQUIRE(loc.has_value());
    return loc->clip();
}

/// The layers at frame `frame` of the timeline's sequence in `state`.
std::vector<LayerSpec> layersAt(const ProjectState& state, SequenceId seq, std::int64_t frame) {
    std::vector<LayerSpec> layers;
    evaluateVideoLayers(state, *state.findSequence(seq), f(frame), layers);
    return layers;
}

} // namespace

TEST_CASE("Auditions 0.6: playback and export see the active take only and exactly as a plain clip of it",
          "[playback][evaluate][auditions]") {
    test::Timeline tl(R"(
        V2 |B5bbbb....|
        V1 |Aaaaaa....|
    )");
    const ClipId a = tl.clip("V1@0");
    const ClipId b = tl.clip("V2@0");
    // Take B has its own effect.
    const ProjectState withBlur = ok(edit::addEffect(tl.state, tl.seq, std::span(&b, 1), "wabcut.blur.gaussian",
                                                     {EffectParam{"radius", ParamValue(7.0)}}))
                                      .state;
    const ClipId ids[] = {a, b};
    const ProjectState audition = ok(edit::createAudition(withBlur, tl.seq, ids)).state;

    // Take 1 (A) plays: one layer of A, nothing of B (its clip left V2 for the audition).
    for (const std::int64_t frame : {0, 3, 5}) {
        const auto layers = layersAt(audition, tl.seq, frame);
        REQUIRE(layers.size() == 1);
        CHECK(layers[0].clip == a);
        CHECK(layers[0].media == tl.media('A'));
        CHECK(layers[0].sourceTime == f(frame));
        CHECK(layers[0].effects.empty());
    }
    // Use take 2: B from its source frame 5 with its blur, in the audition's place.
    const ProjectState active = ok(edit::setActiveTake(audition, tl.seq, a, 1)).state;
    // The same timeline with take 2 as a plain clip (Flatten Audition).
    const ProjectState plain = ok(edit::flattenAuditions(active, tl.seq, std::span(&a, 1))).state;
    REQUIRE_FALSE(clipOf(plain, tl.seq, a).isAudition());
    for (const std::int64_t frame : {0, 3, 5}) {
        const auto layers = layersAt(active, tl.seq, frame);
        const auto expected = layersAt(plain, tl.seq, frame);
        REQUIRE(layers.size() == 1);
        REQUIRE(expected.size() == 1);
        CHECK(layers[0].clip == a);
        CHECK(layers[0].media == tl.media('B'));
        CHECK(layers[0].sourceTime == f(5 + frame));
        CHECK(layers[0].sourceTime == expected[0].sourceTime);
        CHECK(layers[0].media == expected[0].media);
        REQUIRE(layers[0].effects.size() == 1);
        REQUIRE(expected[0].effects.size() == 1);
        CHECK(layers[0].effects[0].type == expected[0].effects[0].type);
        CHECK(layers[0].effects[0].params == expected[0].effects[0].params);
        CHECK(layers[0].transform.scale == expected[0].transform.scale);
        CHECK(layers[0].opacity == expected[0].opacity);
    }
    CHECK(layersAt(active, tl.seq, 6).empty()); // past the audition: the gap

    // Export and the render previews key a range by what it shows: the audition is its active take.
    const TimeRange range{f(0), f(10)};
    CHECK(io::renderKeyJson(active, tl.seq, range) == io::renderKeyJson(plain, tl.seq, range));
    CHECK(io::renderKeyJson(audition, tl.seq, range) != io::renderKeyJson(active, tl.seq, range));
    // Renaming a take that does not play changes nothing a render depends on.
    const ProjectState renamed = ok(edit::renameAuditionTake(active, tl.seq, a, 0, "Wide")).state;
    CHECK(io::renderKeyJson(renamed, tl.seq, range) == io::renderKeyJson(active, tl.seq, range));
}
