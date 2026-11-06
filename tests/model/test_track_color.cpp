// 0.7 (card 077): a track's label colour (Track::colorIndex, one of the 16 clip labels; -1 = none). It is set with the
// track header operation, refused outside the palette, allowed on locked tracks like the other header changes, one
// step for the ChangeSet, and written to project files only when set (an older build ignores the key).

#include "TimelineFixture.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace wabcut;
using namespace wabcut::test;

namespace {

constexpr std::string_view kPicture = R"(
    V1 tgt |Aaaaaaaa|
    A1 tgt |Aaaaaaaa|
    A2 lock |........|
)";

const Track& trackOf(const ProjectState& s, const Timeline& tl, std::string_view label) {
    return *s.findSequence(tl.seq)->findTrack(tl.track(label));
}

std::size_t occurrences(const std::string& text, const std::string& what) {
    std::size_t n = 0;
    for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size()))
        ++n;
    return n;
}

} // namespace

TEST_CASE("A track has no label colour until one is set, and setting it changes only that track", "[model][track][label]") {
    Timeline tl(kPicture);
    CHECK(trackOf(tl.state, tl, "V1").colorIndex == -1);
    CHECK(trackOf(tl.state, tl, "A1").colorIndex == -1);

    edit::TrackStateChange paint;
    paint.colorIndex = 5;
    const edit::EditResult painted = ok(edit::setTrackState(tl.state, tl.seq, tl.track("V1"), paint));
    CHECK(trackOf(painted.state, tl, "V1").colorIndex == 5);
    CHECK(trackOf(painted.state, tl, "A1").colorIndex == -1);
    // A header change: the ChangeSet sees it, and the clips are the same.
    CHECK_FALSE(diff(tl.state, painted.state).empty());
    CHECK(trackOf(painted.state, tl, "V1").clips.size() == trackOf(tl.state, tl, "V1").clips.size());

    // Another colour replaces it; -1 clears it.
    paint.colorIndex = 15;
    const edit::EditResult last = ok(edit::setTrackState(painted.state, tl.seq, tl.track("V1"), paint));
    CHECK(trackOf(last.state, tl, "V1").colorIndex == 15);
    paint.colorIndex = -1;
    const edit::EditResult cleared = ok(edit::setTrackState(last.state, tl.seq, tl.track("V1"), paint));
    CHECK(trackOf(cleared.state, tl, "V1").colorIndex == -1);
    CHECK(cleared.state == tl.state);
}

TEST_CASE("A track's label colour is refused outside the 16 labels and allowed on a locked track", "[model][track][label]") {
    Timeline tl(kPicture);
    edit::TrackStateChange paint;
    paint.colorIndex = 16;
    CHECK_FALSE(edit::setTrackState(tl.state, tl.seq, tl.track("V1"), paint).has_value());
    paint.colorIndex = -2;
    CHECK_FALSE(edit::setTrackState(tl.state, tl.seq, tl.track("V1"), paint).has_value());
    paint.colorIndex = 0;
    CHECK(edit::setTrackState(tl.state, tl.seq, tl.track("V1"), paint).has_value()); // label 0 is a colour, not "none"
    // A2 is locked: the colour is a header setting, like the lock itself.
    paint.colorIndex = 3;
    const edit::EditResult locked = ok(edit::setTrackState(tl.state, tl.seq, tl.track("A2"), paint));
    CHECK(trackOf(locked.state, tl, "A2").colorIndex == 3);
    CHECK(trackOf(locked.state, tl, "A2").locked);
    CHECK(trackOf(tl.state, tl, "A2").colorIndex == -1);
}

TEST_CASE("A track's label colour is saved only when set and loads back", "[model][track][label][io]") {
    Timeline tl(kPicture);
    const auto plain = io::toJson(tl.state);
    REQUIRE(plain);
    const std::size_t before = occurrences(*plain, "\"colorIndex\"");

    edit::TrackStateChange paint;
    paint.colorIndex = 11;
    ProjectState s = ok(edit::setTrackState(tl.state, tl.seq, tl.track("A1"), paint)).state;
    const auto saved = io::toJson(s);
    REQUIRE(saved);
    CHECK(occurrences(*saved, "\"colorIndex\"") == before + 1); // one key more: the painted track's
    CHECK(occurrences(*saved, "\"colorIndex\": 11") == 1);

    const auto back = io::fromJson(*saved);
    REQUIRE(back);
    CHECK(trackOf(*back, tl, "A1").colorIndex == 11);
    CHECK(trackOf(*back, tl, "V1").colorIndex == -1);
    CHECK(trackOf(*back, tl, "A2").colorIndex == -1);
    CHECK(*io::toJson(*back) == *saved);

    // Cleared again: the key is gone and the file is the one it was.
    paint.colorIndex = -1;
    s = ok(edit::setTrackState(s, tl.seq, tl.track("A1"), paint)).state;
    CHECK(*io::toJson(s) == *plain);

    // A file naming a colour outside the labels is refused, never clamped.
    std::string tooBig = *saved;
    const std::string key = "\"colorIndex\": 11";
    tooBig.replace(tooBig.find(key), key.size(), "\"colorIndex\": 99");
    CHECK_FALSE(io::fromJson(tooBig).has_value());
}
