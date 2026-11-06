// 0.7 (card 046): a split edit (L/J cut) is two linked clips with their own extents; trimming, rippling or moving its picture
// with the link followed moves the sound with it, so the sound keeps starting where it did relative to the picture.

#include "TimelineFixture.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/SequenceSettings.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

TEST_CASE("A split edit stays split when its picture is trimmed, rippled or moved with the link followed (card 046)",
          "[model][split]") {
    wabcut::test::Timeline tl(R"(
        V1 tgt |AaaaBbbbb.......|
        A1 tgt |AaaaaaB2b.......|
    )");
    ProjectState s = tl.state;
    for (const auto& [video, audio] : std::vector<std::pair<std::string, std::string>>{{"V1@0", "A1@0"}, {"V1@4", "A1@6"}}) {
        s = ok(edit::linkClips(s, tl.seq, std::vector<ClipId>{tl.clip(video), tl.clip(audio)})).state;
    }
    // B's sound starts two frames after B's picture; tell where each is now.
    const auto bStarts = [&](const ProjectState& state) {
        Ticks picture{};
        Ticks sound{};
        for (const Clip& c : state.findSequence(tl.seq)->videoTracks.front()->clips)
            if (c.media == tl.media('B'))
                picture = c.start();
        for (const Clip& c : state.findSequence(tl.seq)->audioTracks.front()->clips)
            if (c.media == tl.media('B'))
                sound = c.start();
        return std::pair{picture, sound};
    };
    const auto [picture0, sound0] = bStarts(s);
    REQUIRE(sound0 - picture0 == f(2));

    // Moved later with its link: both move, the two frames between them stay.
    const ClipId bPicture = tl.clip("V1@4");
    const auto moved = ok(edit::moveClips(s, tl.seq, std::vector<ClipId>{bPicture}, f(3)));
    const auto [picture1, sound1] = bStarts(moved.state);
    CHECK(picture1 == f(7));
    CHECK(sound1 - picture1 == f(2));

    // Its tail rippled shorter: the sound's tail follows, the split at the head is the same.
    const auto rippled = ok(edit::rippleTrim(s, tl.seq, bPicture, edit::Edge::Tail, Ticks{} - f(1)));
    const auto [picture2, sound2] = bStarts(rippled.state);
    CHECK(sound2 - picture2 == f(2));

    // Its head trimmed: the sound's head moves by the same frames, so the sound still starts after the picture.
    const auto trimmed = ok(edit::trimEdge(s, tl.seq, bPicture, edit::Edge::Head, f(1)));
    const auto [picture3, sound3] = bStarts(trimmed.state);
    CHECK(picture3 == f(5));
    CHECK(sound3 - picture3 == f(2));
}

TEST_CASE("Sequence settings set and clear the work area; a range outside the sequence is refused (card 046)", "[model][workarea]") {
    wabcut::test::Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa|
        A1 tgt |Aaaaaaaaaa|
    )");
    const auto workArea = [&](const ProjectState& s) { return s.findSequence(tl.seq)->workArea; };
    CHECK_FALSE(workArea(tl.state).has_value());

    edit::SequenceSettingsChange set;
    set.workArea = std::optional<TimeRange>{TimeRange::fromStartEnd(f(2), f(7))};
    const auto changed = ok(edit::setSequenceSettings(tl.state, tl.seq, set));
    REQUIRE(workArea(changed.state).has_value());
    CHECK(*workArea(changed.state) == TimeRange::fromStartEnd(f(2), f(7)));

    // A change that does not mention the work area keeps it.
    edit::SequenceSettingsChange rename;
    rename.name = "Renamed";
    CHECK(*workArea(ok(edit::setSequenceSettings(changed.state, tl.seq, rename)).state) == TimeRange::fromStartEnd(f(2), f(7)));

    // Past the end, empty, negative: refused, nothing changes.
    for (const TimeRange bad : {TimeRange::fromStartEnd(f(2), f(11)), TimeRange::fromStartEnd(f(5), f(5)),
                                TimeRange::fromStartEnd(Ticks{} - f(1), f(3))}) {
        edit::SequenceSettingsChange wrong;
        wrong.workArea = std::optional<TimeRange>{bad};
        CHECK_FALSE(edit::setSequenceSettings(changed.state, tl.seq, wrong));
    }

    // Cleared.
    edit::SequenceSettingsChange clear;
    clear.workArea = std::optional<TimeRange>{};
    CHECK_FALSE(workArea(ok(edit::setSequenceSettings(changed.state, tl.seq, clear)).state).has_value());
}
