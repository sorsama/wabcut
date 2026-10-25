// Moving clips (overwrite, insert and, 0.4, rearrange modes, across tracks), copying them (0.4, Alt-drag) and linking.

#include "TimelineFixture.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/FinishingOps.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;
using edit::MoveMode;

namespace {

const Clip& clipOf(const ProjectState& state, SequenceId seq, ClipId id) {
    return state.findSequence(seq)->findClip(id)->clip();
}

} // namespace

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

// ---- 0.4: Alt-drag copies (edit::duplicateClips) ----------------------------------------------------------------------

TEST_CASE("Duplicating a linked pair gives new clips in a new link group", "[model][edit][move][duplicate]") {
    Timeline tl(R"(
        V1 |AaaBbb....|
        A1 |AaaBbb....|
    )");
    tl.linkMatching();
    const ClipId b = tl.clip("V1@3");
    const ClipId bAudio = tl.clip("A1@3");
    const std::vector<ClipId> sel{b};
    const auto r = ok(edit::duplicateClips(tl.state, tl.seq, sel, f(4)));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V1 |AaaBbb.Bbb|
        A1 |AaaBbb.Bbb|
    )");
    CHECK(links(r.state, tl.seq) == "V1@0+A1@0 V1@3+A1@3 V1@7+A1@7");
    REQUIRE(r.created.size() == 2);
    CHECK(r.placed == r.created);
    CHECK(r.created[0] == clipAt(r.state, tl.seq, "V1@7"));
    CHECK(r.created[1] == clipAt(r.state, tl.seq, "A1@7"));
    CHECK(r.created[0] != b);
    CHECK(r.created[1] != bAudio);
    // The originals keep their ids and ranges.
    CHECK(clipAt(r.state, tl.seq, "V1@3") == b);
    CHECK(clipAt(r.state, tl.seq, "A1@3") == bAudio);
    CHECK(clipOf(r.state, tl.seq, b).timelineRange == clipOf(tl.state, tl.seq, b).timelineRange);
    CHECK(r.removed.empty());
    CHECK(r.appliedDelta == f(4));
}

TEST_CASE("Duplicating keeps effects and keyframes under new effect ids", "[model][edit][move][duplicate]") {
    Timeline tl(R"(
        V1 |Aaaa......|
    )");
    const ClipId a = tl.clip("V1@0");
    ProjectState s = tl.state;
    s = ok(edit::addEffect(s, tl.seq, std::span(&a, 1), "wabcut.blur.gaussian")).state;
    s = ok(edit::addKeyframe(s, tl.seq, a, edit::ParamRef::of(ParamId::Scale), Keyframe{f(1), 0.5})).state;
    s = ok(edit::addKeyframe(s, tl.seq, a, edit::ParamRef::of(ParamId::Scale), Keyframe{f(3), 1.5})).state;
    const std::vector<ClipId> sel{a};
    const auto r = ok(edit::duplicateClips(s, tl.seq, sel, f(5)));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V1 |Aaaa.Aaaa.|
    )");
    REQUIRE(r.created.size() == 1);
    const Clip& original = clipOf(r.state, tl.seq, a);
    const Clip& copy = clipOf(r.state, tl.seq, r.created.front());
    REQUIRE(copy.effects().size() == 1);
    REQUIRE(original.effects().size() == 1);
    CHECK(copy.effects()[0].id != original.effects()[0].id);
    CHECK(copy.effects()[0].effect == original.effects()[0].effect);
    CHECK(copy.effects()[0].params == original.effects()[0].params);
    CHECK(copy.properties()[ParamId::Scale] == original.properties()[ParamId::Scale]);
    REQUIRE(copy.properties()[ParamId::Scale].curve() != nullptr);
    CHECK(copy.properties()[ParamId::Scale].curve()->keys.size() == 2);
    CHECK(copy.sourceIn == original.sourceIn);
    CHECK(copy.speed == original.speed);
}

TEST_CASE("Duplicating overwrites what lies under the copy and Insert makes room", "[model][edit][move][duplicate]") {
    Timeline tl(R"(
        V1        |AaaBbbCcc|
        A1        |.........|
        A2        |Mmmmmmmmm|
        A3 nosync |Nnnnnnnnn|
    )");
    const ClipId a = tl.clip("V1@0");
    const ClipId b = tl.clip("V1@3");
    const std::vector<ClipId> sel{a};

    const auto over = ok(edit::duplicateClips(tl.state, tl.seq, sel, f(3)));
    CHECK_TIMELINE(over.state, tl.seq, R"(
        V1 |AaaAaaCcc|
        A1 |.........|
        A2 |Mmmmmmmmm|
        A3 |Nnnnnnnnn|
    )");
    CHECK(std::ranges::find(over.removed, b) != over.removed.end()); // B was under the copy
    CHECK(clipAt(over.state, tl.seq, "V1@0") == a);

    const auto insert = ok(edit::duplicateClips(tl.state, tl.seq, sel, f(3), {}, MoveMode::Insert));
    CHECK_TIMELINE(insert.state, tl.seq, R"(
        V1 |AaaAaaBbbCcc|
        A1 |............|
        A2 |Mmm...M3mmmm|
        A3 |Nnnnnnnnn...|
    )");
    CHECK(clipAt(insert.state, tl.seq, "V1@6") == b);
    REQUIRE_FALSE(insert.placed.empty());
    CHECK(insert.placed.front() == clipAt(insert.state, tl.seq, "V1@3"));
    CHECK(insert.created.front() == insert.placed.front()); // the copy first, then A2's split piece
}

TEST_CASE("Duplicating one side of a linked pair gives an unlinked copy", "[model][edit][move][duplicate]") {
    Timeline tl(R"(
        V1 |AaaBbb....|
        A1 |AaaBbb....|
    )");
    tl.linkMatching();
    const std::vector<ClipId> sel{tl.clip("V1@3")};
    const auto r = ok(edit::duplicateClips(tl.state, tl.seq, sel, f(4), {}, MoveMode::Overwrite, edit::LinkMode::IgnoreLinks));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V1 |AaaBbb.Bbb|
        A1 |AaaBbb....|
    )");
    CHECK(links(r.state, tl.seq) == "V1@0+A1@0 V1@3+A1@3");
    REQUIRE(r.created.size() == 1);
    CHECK_FALSE(clipOf(r.state, tl.seq, r.created.front()).linkGroup.isValid());
}

TEST_CASE("Duplicating copies the transitions between copied clips", "[model][edit][move][duplicate]") {
    Timeline tl(R"(
        V1 |AaaaaaaaaaB5bbbbbbbb......................|
    )");
    ProjectState s = tl.state;
    edit::TransitionSpec spec;
    spec.outgoing = tl.clip("V1@0");
    spec.incoming = tl.clip("V1@10");
    spec.before = f(2);
    spec.after = f(2);
    s = ok(edit::addTransition(s, tl.seq, tl.track("V1"), spec)).state;
    const TransitionId original = s.findSequence(tl.seq)->findTrack(tl.track("V1"))->transitions.at(0).id;
    const std::vector<ClipId> sel{tl.clip("V1@0"), tl.clip("V1@10")};
    const auto r = ok(edit::duplicateClips(s, tl.seq, sel, f(22)));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V1 |AaaaaaaaaaB5bbbbbbbb..AaaaaaaaaaB5bbbbbbbb|
    )");
    const Track& v1 = *r.state.findSequence(tl.seq)->findTrack(tl.track("V1"));
    REQUIRE(v1.transitions.size() == 2);
    CHECK(v1.transitions[0].id == original);
    const Transition& copied = v1.transitions[1];
    CHECK(copied.id != original);
    CHECK(copied.outgoing == clipAt(r.state, tl.seq, "V1@22"));
    CHECK(copied.incoming == clipAt(r.state, tl.seq, "V1@32"));
    CHECK(copied.before == f(2));
    CHECK(copied.after == f(2));

    // Only one side copied: the transition stays with the originals.
    const std::vector<ClipId> one{tl.clip("V1@0")};
    const auto single = ok(edit::duplicateClips(s, tl.seq, one, f(22)));
    CHECK(single.state.findSequence(tl.seq)->findTrack(tl.track("V1"))->transitions.size() == 1);
}

TEST_CASE("Duplicating refuses a locked destination and a rearrange", "[model][edit][move][duplicate]") {
    Timeline tl(R"(
        V2 lock |......|
        V1      |Aa....|
    )");
    const std::vector<ClipId> a{tl.clip("V1@0")};
    CHECK_FALSE(edit::duplicateClips(tl.state, tl.seq, a, f(0), {1, 0})); // onto locked V2
    CHECK_FALSE(edit::duplicateClips(tl.state, tl.seq, a, f(0), {-1, 0})); // there is no V0
    const auto rearranged = edit::duplicateClips(tl.state, tl.seq, a, f(2), {}, MoveMode::Rearrange);
    REQUIRE_FALSE(rearranged);
    CHECK(rearranged.error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("Duplicating onto the same place changes nothing", "[model][edit][move][duplicate]") {
    Timeline tl(R"(
        V1 |Aa....|
    )");
    const std::vector<ClipId> a{tl.clip("V1@0")};
    const auto same = ok(edit::duplicateClips(tl.state, tl.seq, a, f(0)));
    CHECK(isSameRevision(same.state, tl.state));
    CHECK(same.created.empty());
    const auto clamped = ok(edit::duplicateClips(tl.state, tl.seq, a, f(-3))); // clamped to 0: the same place
    CHECK(isSameRevision(clamped.state, tl.state));
}

// ---- 0.4: Ctrl+Alt-drag rearranges (MoveMode::Rearrange) --------------------------------------------------------------

TEST_CASE("Rearranging extracts at the origin and inserts at the drop", "[model][edit][move][rearrange]") {
    Timeline tl(R"(
        V1 |AaaBbbCcc|
    )");
    const ClipId a = tl.clip("V1@0");
    const std::vector<ClipId> sel{a};
    const auto r = ok(edit::moveClips(tl.state, tl.seq, sel, f(9), {}, MoveMode::Rearrange));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V1 |BbbCccAaa|
    )");
    CHECK(clipAt(r.state, tl.seq, "V1@6") == a); // the same clip, moved
    CHECK(r.created.empty());
    CHECK(r.removed.empty());
    CHECK(r.appliedDelta == f(9));
}

TEST_CASE("Rearranging a clip to the cut after its neighbour swaps the two", "[model][edit][move][rearrange]") {
    Timeline tl(R"(
        V1 |AaaBbbCcc|
    )");
    const std::vector<ClipId> sel{tl.clip("V1@0")};
    const auto r = ok(edit::moveClips(tl.state, tl.seq, sel, f(6), {}, MoveMode::Rearrange));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V1 |BbbAaaCcc|
    )");
    // Back the other way: C dropped at B's start goes in front of it.
    const std::vector<ClipId> c{tl.clip("V1@6")};
    const auto back = ok(edit::moveClips(tl.state, tl.seq, c, f(-3), {}, MoveMode::Rearrange));
    CHECK_TIMELINE(back.state, tl.seq, R"(
        V1 |AaaCccBbb|
    )");
}

TEST_CASE("Rearranging leaves other tracks and captions alone", "[model][edit][move][rearrange]") {
    Timeline tl(R"(
        V1 |AaaBbbCcc|
        A1 |.........|
        A2 |Mmmmmmmmm|
    )");
    CaptionTrack captions;
    captions.name = "Captions";
    captions.cues = {{f(1), f(4), "under the span"}};
    ProjectState s = ok(edit::setCaptionTrack(tl.state, tl.seq, captions)).state;
    const ClipId music = tl.clip("A2@0");
    const std::vector<ClipId> sel{tl.clip("V1@0")};
    const auto r = ok(edit::moveClips(s, tl.seq, sel, f(9), {}, MoveMode::Rearrange));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V1 |BbbCccAaa|
        A1 |.........|
        A2 |Mmmmmmmmm|
    )");
    CHECK(clipAt(r.state, tl.seq, "A2@0") == music); // the sync-locked music bed is not cut
    const ChangeSet changes = diff(s, r.state); // keep it: change points into it
    const SequenceChange* change = changes.sequence(tl.seq);
    REQUIRE(change != nullptr);
    CHECK_FALSE(change->captionsChanged);
    CHECK(change->track(tl.track("A2")) == nullptr);
    const Sequence& after = *r.state.findSequence(tl.seq);
    REQUIRE(after.captionTracks.size() == 1);
    REQUIRE(after.captionTracks[0].cues.size() == 1);
    CHECK(after.captionTracks[0].cues[0].start == f(1));
    CHECK(after.captionTracks[0].cues[0].duration == f(4));
}

TEST_CASE("Rearranging back into its own place changes nothing", "[model][edit][move][rearrange]") {
    Timeline tl(R"(
        V1 |AaaBbbCcc|
    )");
    const std::vector<ClipId> sel{tl.clip("V1@0")};
    const auto r = ok(edit::moveClips(tl.state, tl.seq, sel, f(2), {}, MoveMode::Rearrange));
    CHECK(isSameRevision(r.state, tl.state));
    CHECK(r.changes.empty());
}

TEST_CASE("Rearranging to another track closes the old place and opens the new one", "[model][edit][move][rearrange]") {
    Timeline tl(R"(
        V2 |XxxYyy...|
        V1 |AaaBbbCcc|
    )");
    const ClipId b = tl.clip("V1@3");
    const std::vector<ClipId> sel{b};
    const auto r = ok(edit::moveClips(tl.state, tl.seq, sel, f(0), {1, 0}, MoveMode::Rearrange));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V2 |XxxBbbYyy|
        V1 |AaaCcc...|
    )");
    CHECK(clipAt(r.state, tl.seq, "V2@3") == b);
}

TEST_CASE("Rearranging to another track reads the drop on that track", "[model][edit][move][rearrange]") {
    // V2 loses no time when B leaves V1, so the drop after Y stays after Y (the time B leaves on V1 does not come off).
    Timeline tl(R"(
        V2 |XxxYyy...|
        V1 |AaaBbbCcc|
    )");
    const ClipId b = tl.clip("V1@3");
    const std::vector<ClipId> sel{b};
    const auto r = ok(edit::moveClips(tl.state, tl.seq, sel, f(3), {1, 0}, MoveMode::Rearrange));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V2 |XxxYyyBbb|
        V1 |AaaCcc...|
    )");
    CHECK(clipAt(r.state, tl.seq, "V2@6") == b);
    CHECK(r.appliedDelta == f(3));

    // A drop later than the clip's old end, one track up: A goes between Y and Z.
    Timeline later(R"(
        V2 |XxxxxYyyyyZzzzz|
        V1 |AaaaaBbbbb.....|
    )");
    const ClipId a = later.clip("V1@0");
    const std::vector<ClipId> one{a};
    const auto r2 = ok(edit::moveClips(later.state, later.seq, one, f(10), {1, 0}, MoveMode::Rearrange));
    CHECK_TIMELINE(r2.state, later.seq, R"(
        V2 |XxxxxYyyyyAaaaaZzzzz|
        V1 |Bbbbb...............|
    )");
    CHECK(clipAt(r2.state, later.seq, "V2@10") == a);

    // A linked pair moved out to V2 and A2 lands at the drop on both tracks, in sync.
    Timeline pair(R"(
        V2 |XxxxxYyyyy|
        V1 |AaaaaBbbbb|
        A1 |AaaaaBbbbb|
        A2 |PppppQqqqq|
    )");
    pair.linkMatching();
    const std::vector<ClipId> linked{pair.clip("V1@0")};
    const auto r3 = ok(edit::moveClips(pair.state, pair.seq, linked, f(10), {1, 1}, MoveMode::Rearrange));
    CHECK_TIMELINE(r3.state, pair.seq, R"(
        V2 |XxxxxYyyyyAaaaa|
        V1 |Bbbbb..........|
        A1 |Bbbbb..........|
        A2 |PppppQqqqqAaaaa|
    )");
    CHECK(links(r3.state, pair.seq) == "V1@0+A1@0 V2@10+A2@10");
}

TEST_CASE("Rearranging refuses to take unselected clips off a track", "[model][edit][move][rearrange]") {
    Timeline tl(R"(
        V1 |AaaBbbCcc|
        A1 |..Xxxxx..|
    )");
    tl.link("V1@3 A1@2"); // B's sound is longer than its picture: closing it would cut A and C
    const std::vector<ClipId> sel{tl.clip("V1@3")};
    const auto r = edit::moveClips(tl.state, tl.seq, sel, f(3), {}, MoveMode::Rearrange);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::InvalidArgument);
    CHECK(r.error().message.find("rearrange would also remove unselected clip") != std::string::npos);
    // A locked destination is refused as moves are.
    Timeline locked(R"(
        V2 lock |......|
        V1      |AaBb..|
    )");
    const std::vector<ClipId> a{locked.clip("V1@0")};
    CHECK_FALSE(edit::moveClips(locked.state, locked.seq, a, f(0), {1, 0}, MoveMode::Rearrange));
}

TEST_CASE("Rearranging does not move sequence markers", "[model][edit][move][rearrange]") {
    Timeline tl(R"(
        V1 |AaaBbbCcc|
    )");
    ProjectState on = tl.state;
    on = ok(edit::addMarker(on, tl.seq, Marker{f(1), Ticks{}, "in A"})).state;
    on = ok(edit::addMarker(on, tl.seq, Marker{f(7), Ticks{}, "in C"})).state;
    auto offSequence = std::make_shared<Sequence>(*on.findSequence(tl.seq));
    offSequence->rippleMarkers = false;
    const ProjectState off = on.withSequence(offSequence);
    const std::vector<ClipId> sel{tl.clip("V1@0")};
    const std::vector<const ProjectState*> states{&on, &off};
    for (const ProjectState* before : states) {
        const auto r = ok(edit::moveClips(*before, tl.seq, sel, f(9), {}, MoveMode::Rearrange));
        CHECK_TIMELINE(r.state, tl.seq, R"(
            V1 |BbbCccAaa|
        )");
        const auto& markers = r.state.findSequence(tl.seq)->markers;
        REQUIRE(markers.size() == 2);
        CHECK(markers[0].start == f(1));
        CHECK(markers[1].start == f(7));
        CHECK_FALSE(diff(*before, r.state).sequence(tl.seq)->markersChanged);
    }
}

TEST_CASE("Rearranging clips selected apart packs them at the drop", "[model][edit][move][rearrange]") {
    // A and B with C between them, dropped at the cut after D: only their 6 frames open there, B follows A with no gap
    // and the sequence keeps its length (the whole A..B span, C included, used to open at the drop).
    Timeline tl(R"(
        V1 |AaaCccccBbbDdddEee|
    )");
    const ClipId a = tl.clip("V1@0");
    const ClipId b = tl.clip("V1@8");
    const std::vector<ClipId> sel{a, b};
    const auto r = ok(edit::moveClips(tl.state, tl.seq, sel, f(15), {}, MoveMode::Rearrange));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V1 |CccccDdddAaaBbbEee|
    )");
    CHECK(clipAt(r.state, tl.seq, "V1@9") == a);
    CHECK(clipAt(r.state, tl.seq, "V1@12") == b);
    CHECK(r.created.empty());
    CHECK(r.removed.empty());
    CHECK(r.appliedDelta == f(15));
    const Sequence& after = *r.state.findSequence(tl.seq);
    CHECK(after.contentEnd() == tl.sequence().contentEnd());
    CHECK(after.duration == tl.sequence().duration);

    // Dropped over A's own place: B joins A there (a change, unlike one clip dropped back into its place). Not dragged
    // at all: nothing changes.
    const auto gathered = ok(edit::moveClips(tl.state, tl.seq, sel, f(2), {}, MoveMode::Rearrange));
    CHECK_TIMELINE(gathered.state, tl.seq, R"(
        V1 |AaaBbbCccccDdddEee|
    )");
    const auto still = ok(edit::moveClips(tl.state, tl.seq, sel, f(0), {}, MoveMode::Rearrange));
    CHECK(isSameRevision(still.state, tl.state));
    CHECK(still.changes.empty());

    // Up to V2 at the cut after X: the two land side by side there and only their 6 frames open on V2.
    Timeline up(R"(
        V2 |XxxxYyyy......|
        V1 |AaaCcccBbbDddd|
    )");
    const std::vector<ClipId> two{up.clip("V1@0"), up.clip("V1@7")};
    const auto r2 = ok(edit::moveClips(up.state, up.seq, two, f(4), {1, 0}, MoveMode::Rearrange));
    CHECK_TIMELINE(r2.state, up.seq, R"(
        V2 |XxxxAaaBbbYyyy|
        V1 |CcccDddd......|
    )");
}

TEST_CASE("Rearranging linked pairs selected apart keeps each pair in sync", "[model][edit][move][rearrange]") {
    Timeline tl(R"(
        V1 |AaaCcccBbbDddd|
        A1 |AaaCcccBbbDddd|
    )");
    tl.linkMatching();
    const std::vector<ClipId> sel{tl.clip("V1@0"), tl.clip("V1@7")};
    const auto r = ok(edit::moveClips(tl.state, tl.seq, sel, f(14), {}, MoveMode::Rearrange));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V1 |CcccDdddAaaBbb|
        A1 |CcccDdddAaaBbb|
    )");
    CHECK(links(r.state, tl.seq) == "V1@0+A1@0 V1@11+A1@11 V1@4+A1@4 V1@8+A1@8");

    // A J cut (A's sound starts a frame before its picture) keeps that offset, packed with the B pair.
    Timeline j(R"(
        V1 |.AaCcccBbbDddd|
        A1 |AaaCcccBbbDddd|
    )");
    j.link("V1@1 A1@0").link("V1@3 A1@3").link("V1@7 A1@7").link("V1@10 A1@10");
    const std::vector<ClipId> jSel{j.clip("V1@1"), j.clip("V1@7")};
    const auto rj = ok(edit::moveClips(j.state, j.seq, jSel, f(14), {}, MoveMode::Rearrange));
    CHECK_TIMELINE(rj.state, j.seq, R"(
        V1 |CcccDddd.AaBbb|
        A1 |CcccDdddAaaBbb|
    )");
    CHECK(links(rj.state, j.seq) == "V1@0+A1@0 V1@11+A1@11 V1@4+A1@4 V1@9+A1@8");
}

TEST_CASE("Rearranging refuses a link group spread over separate stretches", "[model][edit][move][rearrange]") {
    // A's sound sits apart from its picture (under the gap after C): packing would pull the two together.
    Timeline tl(R"(
        V1 |AaaCcc......|
        A1 |......Xxx...|
    )");
    tl.link("V1@0 A1@6");
    const std::vector<ClipId> sel{tl.clip("V1@0")};
    const auto r = edit::moveClips(tl.state, tl.seq, sel, f(9), {}, MoveMode::Rearrange);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::InvalidArgument);
    CHECK(r.error().message == "rearrange works on clips that are side by side; use Ctrl-drag to insert");
    // Refused at its own place too (a drag held there keeps saying why), as is one taking an unselected clip off.
    CHECK_FALSE(edit::moveClips(tl.state, tl.seq, sel, f(0), {}, MoveMode::Rearrange));
    Timeline lcut(R"(
        V1 |AaaBbbCcc|
        A1 |..Xxxxx..|
    )");
    lcut.link("V1@3 A1@2");
    const std::vector<ClipId> b{lcut.clip("V1@3")};
    CHECK_FALSE(edit::moveClips(lcut.state, lcut.seq, b, f(0), {}, MoveMode::Rearrange));
    // Alt's link override (IgnoreLinks): the picture alone rearranges.
    const auto alone =
        ok(edit::moveClips(tl.state, tl.seq, sel, f(6), {}, MoveMode::Rearrange, edit::LinkMode::IgnoreLinks));
    CHECK_TIMELINE(alone.state, tl.seq, R"(
        V1 |CccAaa......|
        A1 |......Xxx...|
    )");

    // A sound that starts where its picture ends is side by side with it: the pair moves as one stretch.
    Timeline touching(R"(
        V1 |Aaa...Ccc|
        A1 |...Xxx...|
    )");
    touching.link("V1@0 A1@3");
    const std::vector<ClipId> pair{touching.clip("V1@0")};
    const auto rt = ok(edit::moveClips(touching.state, touching.seq, pair, f(9), {}, MoveMode::Rearrange));
    CHECK_TIMELINE(rt.state, touching.seq, R"(
        V1 |CccAaa...|
        A1 |......Xxx|
    )");
    CHECK(links(rt.state, touching.seq) == "V1@3+A1@6");
}
