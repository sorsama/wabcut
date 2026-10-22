// 0.2 timeline parity: Clip > Group / Ungroup (ClipGroups.h), fade handles (ClipFades.h), Selection Follows Playhead and
// Go to Gap (TimelineSelection.h).

#include "TimelineFixture.h"

#include <wabcut/model/ClipFades.h>
#include <wabcut/model/ClipGroups.h>
#include <wabcut/model/Clipboard.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/TimelineSelection.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <regex>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

const Clip& clipOf(const ProjectState& state, SequenceId seq, ClipId id) {
    return state.findSequence(seq)->findClip(id)->clip();
}

std::vector<ClipId> sorted(std::vector<ClipId> ids) {
    std::ranges::sort(ids);
    return ids;
}

} // namespace

TEST_CASE("Group puts clips on different tracks into one new group", "[model][groups]") {
    Timeline tl(R"(
        V2 |..Cccc....|
        V1 |AaaaBbbb..|
        A1 |AaaaBbbb..|
    )");
    tl.linkMatching();
    const ClipId a = tl.clip("V1@0");
    const ClipId c = tl.clip("V2@2");
    const std::vector<ClipId> both{a, c};
    const auto r = ok(edit::groupClips(tl.state, tl.seq, both));
    const Clip& ca = clipOf(r.state, tl.seq, a);
    const Clip& cc = clipOf(r.state, tl.seq, c);
    REQUIRE(ca.group.isValid());
    CHECK(ca.group == cc.group);
    CHECK(ca.group.value < r.state.ids.next);
    CHECK_FALSE(clipOf(r.state, tl.seq, tl.clip("V1@4")).group.isValid());
    // Links are untouched: a group is not a link.
    CHECK(links(r.state, tl.seq) == links(tl.state, tl.seq));

    // A click on one member selects the whole group and the clips linked to the members.
    const Sequence& s = *r.state.findSequence(tl.seq);
    const std::vector<ClipId> clicked{c};
    const auto selection = edit::withGroupedClips(s, clicked);
    CHECK(selection.front() == c);
    CHECK(sorted(selection) == sorted({a, c, tl.clip("A1@0")}));
    CHECK(edit::anyGrouped(s, clicked));
    const std::vector<ClipId> other{tl.clip("V1@4")};
    CHECK(edit::withGroupedClips(s, other) == std::vector<ClipId>{tl.clip("V1@4"), tl.clip("A1@4")});
    CHECK_FALSE(edit::anyGrouped(s, other));
}

TEST_CASE("Group needs two clips and refuses clips on a locked track", "[model][groups]") {
    Timeline tl(R"(
        V2 lock |Cccc|
        V1      |AaBb|
    )");
    const std::vector<ClipId> one{tl.clip("V1@0")};
    CHECK_FALSE(edit::groupClips(tl.state, tl.seq, one).has_value());
    const std::vector<ClipId> locked{tl.clip("V1@0"), tl.clip("V2@0")};
    CHECK_FALSE(edit::groupClips(tl.state, tl.seq, locked).has_value());
    const std::vector<ClipId> unknown{tl.clip("V1@0"), ClipId{999999}};
    CHECK_FALSE(edit::groupClips(tl.state, tl.seq, unknown).has_value());
}

TEST_CASE("Grouping again moves clips into the new group and Ungroup dissolves every member", "[model][groups]") {
    Timeline tl(R"(
        V1 |AaBbCcDd|
    )");
    const ClipId a = tl.clip("V1@0");
    const ClipId b = tl.clip("V1@2");
    const ClipId c = tl.clip("V1@4");
    const ClipId d = tl.clip("V1@6");
    const std::vector<ClipId> ab{a, b};
    const auto first = ok(edit::groupClips(tl.state, tl.seq, ab));
    const std::vector<ClipId> bc{b, c};
    const auto second = ok(edit::groupClips(first.state, tl.seq, bc));
    CHECK(clipOf(second.state, tl.seq, b).group == clipOf(second.state, tl.seq, c).group);
    CHECK(clipOf(second.state, tl.seq, a).group == clipOf(first.state, tl.seq, a).group); // a stays in the old group
    CHECK(clipOf(second.state, tl.seq, b).group != clipOf(second.state, tl.seq, a).group);

    // Ungroup naming one member takes every member of its group out (Premiere).
    const std::vector<ClipId> justC{c};
    const auto ungrouped = ok(edit::ungroupClips(second.state, tl.seq, justC));
    CHECK_FALSE(clipOf(ungrouped.state, tl.seq, b).group.isValid());
    CHECK_FALSE(clipOf(ungrouped.state, tl.seq, c).group.isValid());
    CHECK(clipOf(ungrouped.state, tl.seq, a).group.isValid());
    // Nothing grouped: unchanged.
    const std::vector<ClipId> justD{d};
    const auto same = ok(edit::ungroupClips(ungrouped.state, tl.seq, justD));
    CHECK(same.state.findSequence(tl.seq) == ungrouped.state.findSequence(tl.seq));
}

TEST_CASE("Grouped clips move together and a razor cut keeps both pieces in the group", "[model][groups]") {
    Timeline tl(R"(
        V2 |Cc......|
        V1 |Aaaa....|
    )");
    const ClipId a = tl.clip("V1@0");
    const ClipId c = tl.clip("V2@0");
    const std::vector<ClipId> ac{a, c};
    const auto grouped = ok(edit::groupClips(tl.state, tl.seq, ac));
    const Sequence& s = *grouped.state.findSequence(tl.seq);
    const std::vector<ClipId> clicked{a};
    const auto selection = edit::withGroupedClips(s, clicked);
    const auto moved = ok(edit::moveClips(grouped.state, tl.seq, selection, f(3)));
    CHECK_TIMELINE(moved.state, tl.seq, R"(
        V2 |...Cc...|
        V1 |...Aaaa.|
    )");
    const std::vector<TrackId> v1{tl.track("V1")};
    const auto cut = ok(edit::addEdit(moved.state, tl.seq, f(5), v1));
    const Clip& left = clipOf(cut.state, tl.seq, clipAt(cut.state, tl.seq, "V1@3"));
    const Clip& right = clipOf(cut.state, tl.seq, clipAt(cut.state, tl.seq, "V1@5"));
    CHECK(left.group.isValid());
    CHECK(left.group == right.group);
}

TEST_CASE("A group is saved, read back and bounded by the id generator", "[model][groups][io]") {
    Timeline tl(R"(
        V1 |AaBb|
    )");
    const std::vector<ClipId> ab{tl.clip("V1@0"), tl.clip("V1@2")};
    const auto grouped = ok(edit::groupClips(tl.state, tl.seq, ab));
    const auto text = io::toJson(grouped.state);
    REQUIRE(text);
    CHECK(text->find("\"group\"") != std::string::npos);
    const auto back = io::fromJson(*text);
    REQUIRE(back);
    CHECK(clipOf(*back, tl.seq, ab[0]).group == clipOf(grouped.state, tl.seq, ab[0]).group);
    // Ungrouped clips do not write the field.
    const auto plain = io::toJson(tl.state);
    REQUIRE(plain);
    CHECK(plain->find("\"group\"") == std::string::npos);
    // A group id at or above the generator is refused (it could collide with a new id).
    const std::string tampered =
        std::regex_replace(*text, std::regex(R"("group"\s*:\s*\d+)"), "\"group\": 999999999");
    REQUIRE(tampered != *text);
    CHECK_FALSE(io::fromJson(tampered).has_value());
}

TEST_CASE("Pasted copies of a group become a new group", "[model][groups][clipboard]") {
    Timeline tl(R"(
        V1 |AaBb........|
    )");
    const std::vector<ClipId> ab{tl.clip("V1@0"), tl.clip("V1@2")};
    const auto grouped = ok(edit::groupClips(tl.state, tl.seq, ab));
    const auto copied = copyClips(grouped.state, tl.seq, ab);
    REQUIRE(copied);
    const auto pasted = pasteClips(grouped.state, tl.seq, *copied, f(6));
    REQUIRE(pasted);
    requireValid(pasted->edit.state);
    REQUIRE(pasted->report.pasted.size() == 2);
    const Clip& p0 = clipOf(pasted->edit.state, tl.seq, pasted->report.pasted[0]);
    const Clip& p1 = clipOf(pasted->edit.state, tl.seq, pasted->report.pasted[1]);
    CHECK(p0.group.isValid());
    CHECK(p0.group == p1.group);
    CHECK(p0.group != clipOf(grouped.state, tl.seq, ab[0]).group);

    // One member copied alone leaves its group behind.
    const std::vector<ClipId> justA{ab[0]};
    const auto single = copyClips(grouped.state, tl.seq, justA);
    REQUIRE(single);
    const auto pastedOne = pasteClips(grouped.state, tl.seq, *single, f(8));
    REQUIRE(pastedOne);
    CHECK_FALSE(clipOf(pastedOne->edit.state, tl.seq, pastedOne->report.pasted[0]).group.isValid());
}

TEST_CASE("Fade handles add a one-sided transition and change or remove it", "[model][fades]") {
    Timeline tl(R"(
        V1 |Aaaaaaaaaa..|
        A1 |Aaaaaaaaaa..|
    )");
    const ClipId v = tl.clip("V1@0");
    const ClipId a = tl.clip("A1@0");
    const auto in = ok(edit::setClipFade(tl.state, tl.seq, v, edit::Edge::Head, f(3)));
    const Sequence* s = in.state.findSequence(tl.seq);
    CHECK(edit::clipFade(*s, v, edit::Edge::Head) == f(3));
    CHECK_FALSE(edit::clipFade(*s, v, edit::Edge::Tail));
    const Track& v1 = *s->findTrack(tl.track("V1"));
    REQUIRE(v1.transitions.size() == 1);
    CHECK(v1.transitions[0].effect == "crossDissolve");
    CHECK_FALSE(v1.transitions[0].outgoing.isValid());
    CHECK(v1.transitions[0].incoming == v);

    // Dragging the handle again changes the same transition; past the clip it is clamped to the clip.
    const auto longer = ok(edit::setClipFade(in.state, tl.seq, v, edit::Edge::Head, f(5)));
    CHECK(edit::clipFade(*longer.state.findSequence(tl.seq), v, edit::Edge::Head) == f(5));
    CHECK(longer.state.findSequence(tl.seq)->findTrack(tl.track("V1"))->transitions.size() == 1);

    // Audio: Constant Power at the Out point.
    const auto out = ok(edit::setClipFade(longer.state, tl.seq, a, edit::Edge::Tail, f(4)));
    const Track& a1 = *out.state.findSequence(tl.seq)->findTrack(tl.track("A1"));
    REQUIRE(a1.transitions.size() == 1);
    CHECK(a1.transitions[0].effect == "constantPower");
    CHECK(a1.transitions[0].outgoing == a);
    CHECK(edit::clipFade(*out.state.findSequence(tl.seq), a, edit::Edge::Tail) == f(4));
    const auto clamped = ok(edit::setClipFade(out.state, tl.seq, a, edit::Edge::Tail, f(400)));
    CHECK(edit::clipFade(*clamped.state.findSequence(tl.seq), a, edit::Edge::Tail) == f(10));

    // 0 removes it; a negative length is refused.
    const auto removed = ok(edit::setClipFade(out.state, tl.seq, v, edit::Edge::Head, Ticks{}));
    CHECK_FALSE(edit::clipFade(*removed.state.findSequence(tl.seq), v, edit::Edge::Head));
    CHECK(removed.state.findSequence(tl.seq)->findTrack(tl.track("V1"))->transitions.empty());
    CHECK_FALSE(edit::setClipFade(out.state, tl.seq, v, edit::Edge::Head, f(-1)).has_value());
}

TEST_CASE("A fade handle refuses an edge that has a transition with the next clip", "[model][fades]") {
    Timeline tl(R"(
        V1 |AaaaaaBbbbbb|
    )");
    const ClipId a = tl.clip("V1@0");
    const ClipId b = tl.clip("V1@6");
    edit::TransitionSpec spec;
    spec.outgoing = a;
    spec.incoming = b;
    spec.before = f(0);
    spec.after = f(2); // A has media past its Out point; B none before its In
    const auto both = ok(edit::addTransition(tl.state, tl.seq, tl.track("V1"), spec));
    CHECK_FALSE(edit::setClipFade(both.state, tl.seq, a, edit::Edge::Tail, f(3)).has_value());
    CHECK_FALSE(edit::clipFade(*both.state.findSequence(tl.seq), a, edit::Edge::Tail));
    // The other edge is free.
    const auto in = ok(edit::setClipFade(both.state, tl.seq, a, edit::Edge::Head, f(2)));
    CHECK(edit::clipFade(*in.state.findSequence(tl.seq), a, edit::Edge::Head) == f(2));
}

TEST_CASE("Selection Follows Playhead picks the clips under the playhead on targeted tracks", "[model][selection]") {
    Timeline tl(R"(
        V2     |Cccc......|
        V1 tgt |AaaaBbbb..|
        A1 tgt |AaaaBbbb..|
    )");
    const Sequence& s = tl.sequence();
    CHECK(edit::clipsUnderTime(s, f(1)) == std::vector<ClipId>{tl.clip("V1@0"), tl.clip("A1@0")});
    CHECK(edit::clipsUnderTime(s, f(5)) == std::vector<ClipId>{tl.clip("V1@4"), tl.clip("A1@4")});
    CHECK(edit::clipsUnderTime(s, f(9)).empty());

    Timeline none(R"(
        V2 |Cccc......|
        V1 |AaaaBbbb..|
    )");
    // No track targeted: every track.
    CHECK(edit::clipsUnderTime(none.sequence(), f(1)) ==
          std::vector<ClipId>{none.clip("V1@0"), none.clip("V2@0")});
}

TEST_CASE("Go to Gap finds the next and previous gap in the sequence", "[model][selection]") {
    Timeline tl(R"(
        V2 |..Cc..........|
        V1 |Aaaa...Bbb..Dd|
        A1 |Aaaa......Ee..|
    )");
    const Sequence& s = tl.sequence();
    // Material covers 0-4, 7-12 and 12-14: the gaps start at 4 (V1 and A1 both empty from 4 to 7) only; 12 is covered
    // by D starting where E ends.
    CHECK(edit::sequenceGapStart(s, f(0), true) == f(4));
    CHECK(edit::sequenceGapStart(s, f(4), true) == std::nullopt);
    CHECK(edit::sequenceGapStart(s, f(10), false) == f(4));
    CHECK(edit::sequenceGapStart(s, f(4), false) == std::nullopt);

    Timeline lead(R"(
        V1 |..Aa..Bb|
    )");
    CHECK(edit::sequenceGapStart(lead.sequence(), f(3), false) == f(0));
    CHECK(edit::sequenceGapStart(lead.sequence(), f(3), true) == f(4));
}

TEST_CASE("A ripple edit of a group's outer edge trims every member and ripples the tracks", "[model][groups][ripple]") {
    // A (linked to its audio) and C are a group whose tails end at frame 4; A2 is sync-locked with nothing on the edge.
    Timeline tl(R"(
        V2 |Cccc..Dddd......|
        V1 |Aaaa..Bbbb......|
        A1 |Aaaa..Bbbb......|
        A2 |.......Eeee.....|
    )");
    tl.linkMatching();
    const ClipId a = tl.clip("V1@0");
    const ClipId c = tl.clip("V2@0");
    const ProjectState grouped = ok(edit::groupClips(tl.state, tl.seq, std::vector<ClipId>{a, c})).state;

    // Two frames longer: every member (and A's audio) grows, what follows moves two frames later on every track.
    const auto longer = ok(edit::rippleTrimEdges(grouped, tl.seq, std::vector<ClipId>{a, c}, edit::Edge::Tail, f(2)));
    CHECK(render(longer.state, tl.seq, R"(V1 |................|)") == normalize(R"(
        V2 |Cccccc..Dddd....|
        V1 |Aaaaaa..Bbbb....|
        A1 |Aaaaaa..Bbbb....|
        A2 |.........Eeee...|
    )"));
    CHECK(longer.appliedDelta == f(2));
    CHECK_FALSE(longer.changes.empty());

    // The head, two frames shorter: In points move, starts stay, everything after closes up.
    const auto shorter = ok(edit::rippleTrimEdges(grouped, tl.seq, std::vector<ClipId>{a, c}, edit::Edge::Head, f(2)));
    CHECK(render(shorter.state, tl.seq, R"(V1 |................|)") == normalize(R"(
        V2 |C2..Dddd........|
        V1 |A2..Bbbb........|
        A1 |A2..Bbbb........|
        A2 |.....Eeee.......|
    )"));
    // Clamped so every member keeps a frame: C and A are 4 frames long.
    const auto clamped = ok(edit::rippleTrimEdges(grouped, tl.seq, std::vector<ClipId>{a, c}, edit::Edge::Head, f(9)));
    CHECK(clamped.appliedDelta == f(3));
    // Two edges on one track are refused.
    const ClipId b = tl.clip("V1@6");
    CHECK_FALSE(edit::rippleTrimEdges(grouped, tl.seq, std::vector<ClipId>{a, b}, edit::Edge::Tail, f(1)).has_value());
}
