// Trims: Q/W ripple trim to playhead, regular trim, ripple edit (B), rolling edit (N), slip (Y), slide (U).

#include "TimelineFixture.h"

#include <wabcut/model/EditOps.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace wabcut;
using namespace wabcut::test;
using edit::Edge;

// --- Q / W --------------------------------------------------------------------------------------------

TEST_CASE("Q ripple-trims the previous edit to the playhead", "[model][edit][qw]") {
    Timeline tl(R"(
        V2     |Cccccccc..|
        V1 tgt |AaaaBbbbbb|
        A1 tgt |AaaaBbbbbb|
        A2     |Mmmmmmmmmm|
    )");
    tl.linkMatching();
    const ClipId b = tl.clip("V1@4");

    const auto r = ok(edit::rippleTrimPreviousEditToPlayhead(tl.state, tl.seq, f(6)));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V2 |CcccC6....|
        V1 |AaaaB2bb..|
        A1 |AaaaB2bb..|
        A2 |MmmmM6mm..|
    )");
    REQUIRE(r.playhead);
    CHECK(*r.playhead == f(4)); // the playhead lands on the edit point
    CHECK(clipAt(r.state, tl.seq, "V1@4") == b); // trimmed, not replaced
    CHECK(links(r.state, tl.seq) == "V1@0+A1@0 V1@4+A1@4");

    // On the edit point itself there is nothing to trim.
    const auto onEdit = ok(edit::rippleTrimPreviousEditToPlayhead(tl.state, tl.seq, f(4)));
    CHECK(isSameRevision(onEdit.state, tl.state));
}

TEST_CASE("W ripple-trims the next edit to the playhead", "[model][edit][qw]") {
    Timeline tl(R"(
        V2     |Cccccccc..|
        V1 tgt |AaaaBbbbbb|
        A1 tgt |AaaaBbbbbb|
        A2     |Mmmmmmmmmm|
    )");
    SECTION("inside a clip") {
        const auto r = ok(edit::rippleTrimNextEditToPlayhead(tl.state, tl.seq, f(6)));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V2 |Cccccc....|
            V1 |AaaaBb....|
            A1 |AaaaBb....|
            A2 |Mmmmmm....|
        )");
        REQUIRE(r.playhead);
        CHECK(*r.playhead == f(6));
    }
    SECTION("on a clip's first frame the whole clip goes (zero length)") {
        const ClipId b = tl.clip("V1@4");
        const auto r = ok(edit::rippleTrimNextEditToPlayhead(tl.state, tl.seq, f(4)));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V2 |Cccc......|
            V1 |Aaaa......|
            A1 |Aaaa......|
            A2 |Mmmm......|
        )");
        CHECK(std::find(r.removed.begin(), r.removed.end(), b) != r.removed.end());
    }
}

TEST_CASE("Q uses the nearest edit across all targeted tracks and keeps them in sync", "[model][edit][qw]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaa..|
        A1 tgt |AaaBbbbbbb|
    )");
    const auto r = ok(edit::rippleTrimPreviousEditToPlayhead(tl.state, tl.seq, f(5)));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V1 |AaaA5a....|
        A1 |AaaB2bbb..|
    )");
    REQUIRE(r.playhead);
    CHECK(*r.playhead == f(3));
}

TEST_CASE("Q and W need a targeted track with an edit", "[model][edit][qw]") {
    Timeline untargeted(R"(
        V1 |Aaaa|
    )");
    CHECK_FALSE(edit::rippleTrimPreviousEditToPlayhead(untargeted.state, untargeted.seq, f(2)));

    Timeline empty(R"(
        V1 tgt |....|
        A1     |Aaaa|
    )");
    CHECK_FALSE(edit::rippleTrimNextEditToPlayhead(empty.state, empty.seq, f(2)));
    CHECK_FALSE(edit::rippleTrimPreviousEditToPlayhead(empty.state, empty.seq, f(2)));
}

// --- Regular trim -------------------------------------------------------------------------------------

TEST_CASE("Trimming an edge moves only that edge and clamps to neighbours and media", "[model][edit][trim]") {
    Timeline tl(R"(
        V1 |Aaaa..B5bbbb..|
        A1 |Aaaa..B5bbbb..|
    )",
                {{'B', 12}});
    tl.linkMatching();
    const ClipId a = tl.clip("V1@0");
    const ClipId b = tl.clip("V1@6");

    SECTION("extending a head stops at the previous clip") {
        const auto r = ok(edit::trimEdge(tl.state, tl.seq, b, Edge::Head, f(-5)));
        CHECK(r.appliedDelta == f(-2));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V1 |AaaaB3bbbbbb..|
            A1 |AaaaB3bbbbbb..|
        )");
    }
    SECTION("shortening a head") {
        const auto r = ok(edit::trimEdge(tl.state, tl.seq, b, Edge::Head, f(3)));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V1 |Aaaa.....B8b..|
            A1 |Aaaa.....B8b..|
        )");
    }
    SECTION("a clip keeps at least one frame") {
        const auto r = ok(edit::trimEdge(tl.state, tl.seq, b, Edge::Head, f(100)));
        CHECK(r.appliedDelta == f(5));
        CHECK(r.state.findSequence(tl.seq)->findClip(b)->clip().duration() == f(1));
    }
    SECTION("extending a tail stops at the end of the source media") {
        const auto r = ok(edit::trimEdge(tl.state, tl.seq, b, Edge::Tail, f(5)));
        CHECK(r.appliedDelta == f(1));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V1 |Aaaa..B5bbbbb.|
            A1 |Aaaa..B5bbbbb.|
        )");
    }
    SECTION("extending a tail stops at the next clip") {
        const auto r = ok(edit::trimEdge(tl.state, tl.seq, a, Edge::Tail, f(5)));
        CHECK(r.appliedDelta == f(2));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V1 |AaaaaaB5bbbb..|
            A1 |AaaaaaB5bbbb..|
        )");
    }
    SECTION("extending a head stops at the start of the source media") {
        const auto r = ok(edit::trimEdge(tl.state, tl.seq, a, Edge::Head, f(-3)));
        CHECK(isSameRevision(r.state, tl.state));
        CHECK(r.appliedDelta == f(0));
    }
    SECTION("ignoring links trims one side only") {
        const auto r = ok(edit::trimEdge(tl.state, tl.seq, b, Edge::Head, f(1), edit::LinkMode::IgnoreLinks));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V1 |Aaaa...B6bbb..|
            A1 |Aaaa..B5bbbb..|
        )");
    }
}

// --- Ripple edit (B) ----------------------------------------------------------------------------------

TEST_CASE("Ripple edit trims an edge and ripples later material on every sync-locked track",
          "[model][edit][rippletrim]") {
    Timeline tl(R"(
        V2 |..Cccccc..|
        V1 |AaaaB3bb..|
        A1 |AaaaB3bb..|
        A2 |Mmmmmmmmmm|
    )",
                {{'B', 7}});
    tl.linkMatching();
    const ClipId a = tl.clip("V1@0");
    const ClipId b = tl.clip("V1@4");

    SECTION("shortening a tail pulls everything after it earlier") {
        const auto r = ok(edit::rippleTrim(tl.state, tl.seq, a, Edge::Tail, f(-2)));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V2 |..C2cc....|
            V1 |AaB3bb....|
            A1 |AaB3bb....|
            A2 |MmM4mmmm..|
        )");
    }
    SECTION("lengthening a tail pushes everything after it later") {
        const auto r = ok(edit::rippleTrim(tl.state, tl.seq, a, Edge::Tail, f(2)));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V2 |..Cc..C2cc..|
            V1 |AaaaaaB3bb..|
            A1 |AaaaaaB3bb..|
            A2 |Mmmm..M4mmmm|
        )");
    }
    SECTION("shortening a head keeps the clip's start and pulls the rest in") {
        const auto r = ok(edit::rippleTrim(tl.state, tl.seq, b, Edge::Head, f(1)));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V2 |..CcC3c...|
            V1 |AaaaB4b...|
            A1 |AaaaB4b...|
            A2 |MmmmM5mmm.|
        )");
    }
    SECTION("lengthening a head reveals earlier source frames and pushes the rest out") {
        const auto r = ok(edit::rippleTrim(tl.state, tl.seq, b, Edge::Head, f(-2)));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V2 |..Cc..C2cc..|
            V1 |AaaaB1bbbb..|
            A1 |AaaaB1bbbb..|
            A2 |Mmmm..M4mmmm|
        )");
    }
    SECTION("clamps to one frame and to the source media") {
        CHECK(ok(edit::rippleTrim(tl.state, tl.seq, a, Edge::Tail, f(-10))).appliedDelta == f(-3));
        CHECK(ok(edit::rippleTrim(tl.state, tl.seq, b, Edge::Head, f(-10))).appliedDelta == f(-3));
        const auto atEnd = ok(edit::rippleTrim(tl.state, tl.seq, b, Edge::Tail, f(5))); // B's media is used up
        CHECK(isSameRevision(atEnd.state, tl.state));
    }
}

TEST_CASE("Ripple edit leaves non-sync-locked tracks alone", "[model][edit][rippletrim]") {
    Timeline tl(R"(
        V1        |AaaaBbbb|
        A1 nosync |Mmmmmmmm|
    )");
    const auto r = ok(edit::rippleTrim(tl.state, tl.seq, tl.clip("V1@0"), Edge::Tail, f(-2)));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V1 |AaBbbb..|
        A1 |Mmmmmmmm|
    )");
}

// --- Rolling edit (N) ---------------------------------------------------------------------------------

TEST_CASE("Rolling edit moves an edit point between two clips", "[model][edit][roll]") {
    Timeline tl(R"(
        V1 |AaaaB3bbbb|
        A1 |AaaaB3bbbb|
    )");
    tl.linkMatching();
    const ClipId a = tl.clip("V1@0");
    const ClipId b = tl.clip("V1@4");

    SECTION("later") {
        const auto r = ok(edit::rollEdit(tl.state, tl.seq, a, Edge::Tail, f(2)));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V1 |AaaaaaB5bb|
            A1 |AaaaaaB5bb|
        )");
    }
    SECTION("earlier") {
        const auto r = ok(edit::rollEdit(tl.state, tl.seq, a, Edge::Tail, f(-2)));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V1 |AaB1bbbbbb|
            A1 |AaB1bbbbbb|
        )");
    }
    SECTION("the incoming clip's head is the same edit point") {
        const auto fromTail = ok(edit::rollEdit(tl.state, tl.seq, a, Edge::Tail, f(1)));
        const auto fromHead = ok(edit::rollEdit(tl.state, tl.seq, b, Edge::Head, f(1)));
        CHECK(fromTail.state == fromHead.state);
    }
    SECTION("clamps to the incoming clip's source media") {
        const auto r = ok(edit::rollEdit(tl.state, tl.seq, a, Edge::Tail, f(-10)));
        CHECK(r.appliedDelta == f(-3));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V1 |ABbbbbbbbb|
            A1 |ABbbbbbbbb|
        )");
    }
}

TEST_CASE("Rolling edit next to a gap trims into the gap", "[model][edit][roll]") {
    Timeline tl(R"(
        V1 |Aaaa..Bbbb|
    )");
    const auto r = ok(edit::rollEdit(tl.state, tl.seq, tl.clip("V1@0"), Edge::Tail, f(5)));
    CHECK(r.appliedDelta == f(2));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V1 |AaaaaaBbbb|
    )");
}

// --- Slip (Y) -----------------------------------------------------------------------------------------

TEST_CASE("Slip changes which source frames a clip shows, not where it is", "[model][edit][slip]") {
    Timeline tl(R"(
        V1 |AaB3bbCc|
        A1 |AaB3bbCc|
    )",
                {{'B', 10}});
    tl.linkMatching();
    const std::vector<ClipId> b{tl.clip("V1@2")};

    SECTION("later source frames, linked audio follows") {
        const auto r = ok(edit::slip(tl.state, tl.seq, b, f(2)));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V1 |AaB5bbCc|
            A1 |AaB5bbCc|
        )");
    }
    SECTION("clamped to the end of the media") {
        const auto r = ok(edit::slip(tl.state, tl.seq, b, f(10)));
        CHECK(r.appliedDelta == f(3));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V1 |AaB6bbCc|
            A1 |AaB6bbCc|
        )");
    }
    SECTION("clamped to the start of the media") {
        const auto r = ok(edit::slip(tl.state, tl.seq, b, f(-10)));
        CHECK(r.appliedDelta == f(-3));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V1 |AaBbbbCc|
            A1 |AaBbbbCc|
        )");
    }
}

// --- Slide (U) ----------------------------------------------------------------------------------------

TEST_CASE("Slide moves a clip between its neighbours, trimming them", "[model][edit][slide]") {
    Timeline tl(R"(
        V1 |AaaaB3bbC3cc|
    )");
    const ClipId b = tl.clip("V1@4");

    SECTION("later: the previous clip grows, the next one shrinks") {
        const auto r = ok(edit::slide(tl.state, tl.seq, b, f(2)));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V1 |AaaaaaB3bbC5|
        )");
    }
    SECTION("earlier: the previous clip shrinks, the next one grows") {
        const auto r = ok(edit::slide(tl.state, tl.seq, b, f(-2)));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V1 |AaB3bbC1cccc|
        )");
    }
    SECTION("clamped by the next clip's length and media") {
        CHECK(ok(edit::slide(tl.state, tl.seq, b, f(10))).appliedDelta == f(3));
        CHECK(ok(edit::slide(tl.state, tl.seq, b, f(-10))).appliedDelta == f(-3));
    }
}

TEST_CASE("Slide next to gaps moves the clip within them", "[model][edit][slide]") {
    Timeline tl(R"(
        V1 |Aa..Bb..Cc|
    )");
    const ClipId b = tl.clip("V1@4");
    CHECK_TIMELINE(ok(edit::slide(tl.state, tl.seq, b, f(1))).state, tl.seq, R"(
        V1 |Aa...Bb.Cc|
    )");
    const auto clamped = ok(edit::slide(tl.state, tl.seq, b, f(5)));
    CHECK(clamped.appliedDelta == f(2));
    CHECK_TIMELINE(clamped.state, tl.seq, R"(
        V1 |Aa....BbCc|
    )");
}

TEST_CASE("Trims refuse clips on locked tracks and unknown clips", "[model][edit][trim]") {
    Timeline tl(R"(
        V1 lock |AaaaBbbb|
    )");
    const ClipId a = tl.clip("V1@0");
    CHECK_FALSE(edit::trimEdge(tl.state, tl.seq, a, Edge::Tail, f(1)));
    CHECK_FALSE(edit::rippleTrim(tl.state, tl.seq, a, Edge::Tail, f(1)));
    CHECK_FALSE(edit::rollEdit(tl.state, tl.seq, a, Edge::Tail, f(1)));
    CHECK_FALSE(edit::slide(tl.state, tl.seq, a, f(1)));
    const std::vector<ClipId> ids{a};
    CHECK_FALSE(edit::slip(tl.state, tl.seq, ids, f(1)));
    const auto missing = edit::trimEdge(tl.state, tl.seq, ClipId{424242}, Edge::Tail, f(1));
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::NotFound);
}
