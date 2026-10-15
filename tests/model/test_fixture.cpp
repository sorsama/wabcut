// The timeline picture DSL itself: what the edit specs rely on.

#include "TimelineFixture.h"

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::test;

TEST_CASE("A timeline picture builds the tracks, flags and clips it shows", "[model][fixture]") {
    const char* picture = R"(
        V2 lock |....Cccc..|
        V1 tgt  |Aaaa..B5bb|
        A1 tgt  |Aaaa..B5bb|
        A2 nosync mute |M|
    )";
    Timeline tl(picture);
    CHECK_TIMELINE(tl.state, tl.seq, picture);

    const Sequence& s = tl.sequence();
    REQUIRE(s.videoTracks.size() == 2);
    REQUIRE(s.audioTracks.size() == 2);
    CHECK(s.videoTracks[1]->locked);
    CHECK_FALSE(s.videoTracks[1]->targeted);
    CHECK(s.videoTracks[0]->targeted);
    CHECK(s.audioTracks[0]->targeted);
    CHECK_FALSE(s.audioTracks[1]->syncLocked);
    CHECK(s.audioTracks[1]->muted);

    const Clip& b = s.videoTracks[0]->clips[1];
    CHECK(b.name() == "B");
    CHECK(b.start() == f(6));
    CHECK(b.duration() == f(4));
    CHECK(b.sourceIn == f(5));
    CHECK(b.stream == StreamRef{StreamKind::Video, 0});
    CHECK(s.audioTracks[0]->clips[0].stream == StreamRef{StreamKind::Audio, 0});
    CHECK(tl.clip("V1@6") == b.id);
}

TEST_CASE("Touching clips of the same media are separate clips", "[model][fixture]") {
    Timeline tl(R"(
        V1 |AaaA3aa|
    )");
    const Track& v1 = *tl.sequence().videoTracks[0];
    REQUIRE(v1.clips.size() == 2);
    CHECK(v1.clips[1].sourceIn == f(3));
}

TEST_CASE("Links render as groups of clip references", "[model][fixture]") {
    Timeline tl(R"(
        V1 |Aaaa..Bbbb|
        A1 |Aaaa..Bbbb|
        A2 |Aaaa......|
    )");
    tl.linkMatching();
    CHECK(links(tl.state, tl.seq) == "V1@0+A1@0+A2@0 V1@6+A1@6");
}

TEST_CASE("Media letters get the requested length", "[model][fixture]") {
    Timeline tl(R"(
        V1 |Aaaa|
    )",
                {{'A', 8}, {'S', 3}});
    CHECK(tl.state.findMedia(tl.media('A'))->info.duration == f(8));
    CHECK(tl.state.findMedia(tl.media('S'))->info.duration == f(3));
}
