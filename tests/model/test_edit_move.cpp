// Moving clips (overwrite and insert modes, across tracks) and linking.

#include "TimelineFixture.h"

#include <wabcut/model/EditOps.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace wabcut;
using namespace wabcut::test;
using edit::MoveMode;

TEST_CASE("Dragging a clip moves its linked partner and leaves a gap behind", "[model][edit][move]") {
    Timeline tl(R"(
        V1 |AaaBbb....|
        A1 |AaaBbb....|
    )");
    tl.linkMatching();
    const ClipId b = tl.clip("V1@3");
    const std::vector<ClipId> sel{b};
    const auto r = ok(edit::moveClips(tl.state, tl.seq, sel, f(4)));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V1 |Aaa....Bbb|
        A1 |Aaa....Bbb|
    )");
    CHECK(links(r.state, tl.seq) == "V1@0+A1@0 V1@7+A1@7");
    CHECK(clipAt(r.state, tl.seq, "V1@7") == b); // same clip, new place
    CHECK(r.appliedDelta == f(4));
}

TEST_CASE("Dropping a clip onto another overwrites it", "[model][edit][move]") {
    Timeline tl(R"(
        V1 |AaaaaaBb..|
    )");
    const std::vector<ClipId> sel{tl.clip("V1@6")};
    const auto r = ok(edit::moveClips(tl.state, tl.seq, sel, f(-4)));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V1 |AaBbA4....|
    )");
    CHECK(r.created.size() == 1); // A's right-hand piece
}

TEST_CASE("Moving across tracks: video goes up, linked audio goes down", "[model][edit][move]") {
    Timeline tl(R"(
        V2 |......|
        V1 |AaaBbb|
        A1 |AaaBbb|
        A2 |......|
    )");
    tl.linkMatching();
    const std::vector<ClipId> sel{tl.clip("V1@3")};
    const auto r = ok(edit::moveClips(tl.state, tl.seq, sel, f(0), {1, 1}));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V2 |...Bbb|
        V1 |Aaa...|
        A1 |Aaa...|
        A2 |...Bbb|
    )");
    CHECK(links(r.state, tl.seq) == "V1@0+A1@0 V2@3+A2@3");
}

TEST_CASE("Ctrl-drag inserts: the destination and sync-locked tracks make room", "[model][edit][move]") {
    Timeline tl(R"(
        V1 |AaaBbbCcc|
        A1 |AaaBbbCcc|
        A2 |Mmmmmmmmm|
    )");
    tl.linkMatching();
    const std::vector<ClipId> sel{tl.clip("V1@6")};
    const auto r = ok(edit::moveClips(tl.state, tl.seq, sel, f(-6), {}, MoveMode::Insert));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V1 |CccAaaBbb...|
        A1 |CccAaaBbb...|
        A2 |...Mmmmmmmmm|
    )");
}

TEST_CASE("Moves clamp at the sequence start and respect locks", "[model][edit][move]") {
    Timeline tl(R"(
        V2 lock |......|
        V1      |..AaBb|
        A1 lock |......|
    )");
    const std::vector<ClipId> a{tl.clip("V1@2")};

    const auto clamped = ok(edit::moveClips(tl.state, tl.seq, a, f(-5)));
    CHECK(clamped.appliedDelta == f(-2));
    CHECK_TIMELINE(clamped.state, tl.seq, R"(
        V2 |......|
        V1 |Aa..Bb|
        A1 |......|
    )");

    const auto atStart = ok(edit::moveClips(clamped.state, tl.seq, a, f(-1)));
    CHECK(isSameRevision(atStart.state, clamped.state));

    CHECK_FALSE(edit::moveClips(tl.state, tl.seq, a, f(0), {1, 0}));  // onto locked V2
    CHECK_FALSE(edit::moveClips(tl.state, tl.seq, a, f(0), {-1, 0})); // there is no V0
    Timeline locked(R"(
        V1 lock |Aa|
    )");
    const std::vector<ClipId> l{locked.clip("V1@0")};
    CHECK_FALSE(edit::moveClips(locked.state, locked.seq, l, f(1)));
}

TEST_CASE("Linking and unlinking clips", "[model][edit][link]") {
    Timeline tl(R"(
        V1 |AaBb|
        A1 |AaBb|
    )");
    const ClipId va = tl.clip("V1@0");
    const ClipId aa = tl.clip("A1@0");
    const ClipId ab = tl.clip("A1@2");

    const auto first = ok(edit::linkClips(tl.state, tl.seq, std::vector<ClipId>{va, ab}));
    CHECK(links(first.state, tl.seq) == "V1@0+A1@2");

    // Re-linking V1@0 elsewhere dissolves its old group, which would be left with one clip.
    const auto second = ok(edit::linkClips(first.state, tl.seq, std::vector<ClipId>{va, aa}));
    CHECK(links(second.state, tl.seq) == "V1@0+A1@0");

    const auto unlinked = ok(edit::unlinkClips(second.state, tl.seq, std::vector<ClipId>{va}));
    CHECK(links(unlinked.state, tl.seq).empty());

    const auto noop = ok(edit::unlinkClips(unlinked.state, tl.seq, std::vector<ClipId>{va}));
    CHECK(isSameRevision(noop.state, unlinked.state));

    CHECK_FALSE(edit::linkClips(tl.state, tl.seq, std::vector<ClipId>{va}));
}

TEST_CASE("withLinkedClips expands a selection by its link groups", "[model][edit][link]") {
    Timeline tl(R"(
        V1 |AaBbCc|
        A1 |AaBbCc|
    )");
    tl.linkMatching();
    const std::vector<ClipId> sel{tl.clip("V1@2")};
    const auto expanded = edit::withLinkedClips(tl.sequence(), sel);
    CHECK(expanded == std::vector<ClipId>{tl.clip("V1@2"), tl.clip("A1@2")});
}
