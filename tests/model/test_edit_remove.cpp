// Lift (;), extract ('), add edit / razor (Ctrl+K), delete and ripple delete (clips and gaps).

#include "TimelineFixture.h"

#include <wabcut/model/EditOps.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

ProjectState withMarks(const ProjectState& state, SequenceId seq, std::int64_t in, std::int64_t lastFrame) {
    auto marked = ok(edit::markIn(state, seq, f(in))).state;
    return ok(edit::markOut(marked, seq, f(lastFrame))).state;
}

} // namespace

// --- Lift / extract -----------------------------------------------------------------------------------

TEST_CASE("Lift clears the marked range on the targeted tracks and leaves a gap", "[model][edit][lift]") {
    Timeline tl(R"(
        V2     |..Cccccc..|
        V1 tgt |Aaaaaa..Bb|
        A1 tgt |Aaaaaa..Bb|
        A2     |Mmmmmmmmmm|
    )");
    const auto marked = withMarks(tl.state, tl.seq, 2, 7); // [2, 8)
    const auto r = ok(edit::lift(marked, tl.seq));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V2 |..Cccccc..|
        V1 |Aa......Bb|
        A1 |Aa......Bb|
        A2 |Mmmmmmmmmm|
    )");
    CHECK_FALSE(r.state.findSequence(tl.seq)->markIn);
    CHECK_FALSE(r.state.findSequence(tl.seq)->markOut);
    REQUIRE(r.playhead);
    CHECK(*r.playhead == f(2));
}

TEST_CASE("Extract closes the marked range and keeps sync-locked tracks in sync", "[model][edit][extract]") {
    SECTION("sync-locked tracks lose the same range, cutting through their clips") {
        Timeline tl(R"(
            V2     |..Cccccc..|
            V1 tgt |Aaaaaa..Bb|
            A1 tgt |Aaaaaa..Bb|
            A2     |Mmmmmmmmmm|
        )");
        const auto r = ok(edit::extract(withMarks(tl.state, tl.seq, 2, 7), tl.seq));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V2 |....|
            V1 |AaBb|
            A1 |AaBb|
            A2 |MmM8|
        )");
        REQUIRE(r.playhead);
        CHECK(*r.playhead == f(2));
    }
    SECTION("tracks without sync lock, and locked tracks, stay put") {
        Timeline tl(R"(
            V2 nosync |..Cccccc..|
            V1 tgt    |Aaaaaa..Bb|
            A1 tgt    |Aaaaaa..Bb|
            A2 lock   |Mmmmmmmmmm|
        )");
        const auto r = ok(edit::extract(withMarks(tl.state, tl.seq, 2, 7), tl.seq));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V2 |..Cccccc..|
            V1 |AaBb......|
            A1 |AaBb......|
            A2 |Mmmmmmmmmm|
        )");
    }
}

TEST_CASE("Lift on one targeted track splits a linked clip; the lone piece is unlinked", "[model][edit][lift]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaa|
        A1     |Aaaaaa|
    )");
    tl.linkMatching();
    const auto r = ok(edit::lift(withMarks(tl.state, tl.seq, 2, 3), tl.seq));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V1 |Aa..A4|
        A1 |Aaaaaa|
    )");
    CHECK(links(r.state, tl.seq) == "V1@0+A1@0");
}

TEST_CASE("Lift and extract need marks and an unlocked targeted track", "[model][edit][lift]") {
    Timeline tl(R"(
        V1 tgt lock |Aaaa|
        A1          |Aaaa|
    )");
    CHECK_FALSE(edit::lift(tl.state, tl.seq)); // no marks
    const auto marked = withMarks(tl.state, tl.seq, 1, 2);
    const auto r = edit::extract(marked, tl.seq); // only a locked track is targeted
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::InvalidArgument);
}

// --- Add edit -----------------------------------------------------------------------------------------

TEST_CASE("Add edit cuts the clips under the position on the given tracks", "[model][edit][razor]") {
    Timeline tl(R"(
        V2     |Cccccc|
        V1 tgt |Aaaaaa|
        A1 tgt |Aaaaaa|
    )");
    tl.linkMatching();
    const std::vector<TrackId> v1{tl.track("V1")};

    SECTION("only the given track; the new right piece is not linked") {
        const auto r = ok(edit::addEdit(tl.state, tl.seq, f(3), v1));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V2 |Cccccc|
            V1 |AaaA3a|
            A1 |Aaaaaa|
        )");
        CHECK(links(r.state, tl.seq) == "V1@0+A1@0");
        CHECK(r.created.size() == 1);
    }
    SECTION("razor with linked selection also cuts linked partners; the right pieces stay linked") {
        const auto r = ok(edit::addEdit(tl.state, tl.seq, f(3), v1, edit::LinkMode::FollowLinks));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V2 |Cccccc|
            V1 |AaaA3a|
            A1 |AaaA3a|
        )");
        CHECK(links(r.state, tl.seq) == "V1@0+A1@0 V1@3+A1@3");
    }
    SECTION("Ctrl+Shift+K cuts every unlocked track") {
        const auto r = ok(edit::addEditToAllTracks(tl.state, tl.seq, f(3)));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V2 |CccC3c|
            V1 |AaaA3a|
            A1 |AaaA3a|
        )");
        CHECK(r.created.size() == 3);
    }
    SECTION("on an edit point, in a gap or past the end nothing changes") {
        for (const Ticks at : {f(0), f(6), f(20), f(-3)}) {
            const auto r = ok(edit::addEditToAllTracks(tl.state, tl.seq, at));
            CHECK(isSameRevision(r.state, tl.state));
            CHECK(r.created.empty());
        }
    }
}

TEST_CASE("Add edit skips locked tracks", "[model][edit][razor]") {
    Timeline tl(R"(
        V1 lock |Aaaa|
        A1      |Aaaa|
    )");
    const auto r = ok(edit::addEditToAllTracks(tl.state, tl.seq, f(2)));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V1 |Aaaa|
        A1 |AaA2|
    )");
}

// --- Delete -------------------------------------------------------------------------------------------

TEST_CASE("Delete removes the clips and leaves gaps", "[model][edit][delete]") {
    Timeline tl(R"(
        V1 |AaBbCc|
        A1 |AaBbCc|
    )");
    tl.linkMatching();
    const std::vector<ClipId> b{tl.clip("V1@2")};

    SECTION("linked partners go too") {
        const auto r = ok(edit::deleteClips(tl.state, tl.seq, b));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V1 |Aa..Cc|
            A1 |Aa..Cc|
        )");
        CHECK(r.removed.size() == 2);
    }
    SECTION("ignoring links, the partner stays and is unlinked") {
        const auto r = ok(edit::deleteClips(tl.state, tl.seq, b, edit::LinkMode::IgnoreLinks));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V1 |Aa..Cc|
            A1 |AaBbCc|
        )");
        CHECK(links(r.state, tl.seq) == "V1@0+A1@0 V1@4+A1@4");
    }
}

TEST_CASE("Ripple delete closes the clips' time on their tracks and on sync-locked tracks", "[model][edit][ripple]") {
    SECTION("sync-locked tracks are cut through") {
        Timeline tl(R"(
            V2 |..Ccc.....|
            V1 |AaBbbbDd..|
            A1 |AaBbbbDd..|
            A2 |Mmmmmmmmmm|
        )");
        tl.linkMatching();
        const std::vector<ClipId> b{tl.clip("V1@2")};
        const auto r = ok(edit::rippleDeleteClips(tl.state, tl.seq, b));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V2 |......|
            V1 |AaDd..|
            A1 |AaDd..|
            A2 |MmM6mm|
        )");
        CHECK(links(r.state, tl.seq) == "V1@0+A1@0 V1@2+A1@2");
    }
    SECTION("tracks without sync lock and locked tracks do not move") {
        Timeline tl(R"(
            V2 nosync |..Ccc.....|
            V1        |AaBbbbDd..|
            A1        |AaBbbbDd..|
            A2 lock   |Mmmmmmmmmm|
        )");
        tl.linkMatching();
        const std::vector<ClipId> b{tl.clip("V1@2")};
        const auto r = ok(edit::rippleDeleteClips(tl.state, tl.seq, b));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V2 |..Ccc.....|
            V1 |AaDd......|
            A1 |AaDd......|
            A2 |Mmmmmmmmmm|
        )");
    }
    SECTION("several clips at different times") {
        Timeline tl(R"(
            V1 |AaBbCcDdEe|
        )");
        const std::vector<ClipId> sel{tl.clip("V1@2"), tl.clip("V1@6")};
        const auto r = ok(edit::rippleDeleteClips(tl.state, tl.seq, sel));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V1 |AaCcEe....|
        )");
    }
    SECTION("refuses to take an unselected clip off a track the selection is on") {
        Timeline tl(R"(
            V2 |..CcDd|
            V1 |AaBbbb|
        )");
        const std::vector<ClipId> sel{tl.clip("V1@2"), tl.clip("V2@2")};
        const auto r = edit::rippleDeleteClips(tl.state, tl.seq, sel, edit::LinkMode::IgnoreLinks);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::InvalidArgument);
    }
    SECTION("clips on locked tracks cannot be ripple deleted") {
        Timeline tl(R"(
            V1 lock |AaBb|
        )");
        const std::vector<ClipId> sel{tl.clip("V1@2")};
        CHECK_FALSE(edit::rippleDeleteClips(tl.state, tl.seq, sel));
    }
}

TEST_CASE("Ripple delete of a gap closes it when every sync-locked track is empty there", "[model][edit][gap]") {
    SECTION("the gap closes on all sync-locked tracks") {
        Timeline tl(R"(
            V1 |Aa..Bb|
            A1 |Aa..Bb|
        )");
        const auto r = ok(edit::rippleDeleteGap(tl.state, tl.seq, tl.track("V1"), f(3)));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V1 |AaBb..|
            A1 |AaBb..|
        )");
    }
    SECTION("blocked by material on another sync-locked track") {
        Timeline tl(R"(
            V1 |Aa..Bb|
            A1 |Mmmmmm|
        )");
        const auto r = edit::rippleDeleteGap(tl.state, tl.seq, tl.track("V1"), f(2));
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::InvalidArgument);
    }
    SECTION("not blocked by tracks without sync lock or locked tracks") {
        for (const char* flag : {"nosync", "lock"}) {
            Timeline tl(std::string("V1 |Aa..Bb|\nA1 ") + flag + " |Mmmmmm|");
            const auto r = ok(edit::rippleDeleteGap(tl.state, tl.seq, tl.track("V1"), f(2)));
            CHECK_TIMELINE(r.state, tl.seq, R"(
                V1 |AaBb..|
                A1 |Mmmmmm|
            )");
        }
    }
    SECTION("a gap at the start of the track") {
        Timeline tl(R"(
            V1 |..Aa|
        )");
        const auto r = ok(edit::rippleDeleteGap(tl.state, tl.seq, tl.track("V1"), f(0)));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V1 |Aa..|
        )");
    }
    SECTION("not a gap: inside a clip, after the last clip, or on a locked track") {
        Timeline tl(R"(
            V1      |Aa..Bb|
            A1 lock |..Cc..|
        )");
        CHECK_FALSE(edit::rippleDeleteGap(tl.state, tl.seq, tl.track("V1"), f(1)));
        CHECK_FALSE(edit::rippleDeleteGap(tl.state, tl.seq, tl.track("V1"), f(6)));
        CHECK_FALSE(edit::rippleDeleteGap(tl.state, tl.seq, tl.track("A1"), f(0)));
    }
}
