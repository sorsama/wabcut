// Transition operations: create / change / delete on edit points, handles, alignment, Premiere's default
// transition commands, and the mapping the interchange importers will use.

#include "TimelineFixture.h"

#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace wabcut;
using namespace wabcut::test;
using edit::HandleFit;
using edit::TransitionSpec;

namespace {

const Track& trackOf(const ProjectState& state, SequenceId seq, std::size_t videoIndex = 0) {
    return *state.findSequence(seq)->videoTracks[videoIndex];
}

TransitionSpec twoSided(ClipId a, ClipId b, std::int64_t before, std::int64_t after, std::string effect = "crossDissolve") {
    TransitionSpec s;
    s.outgoing = a;
    s.incoming = b;
    s.before = f(before);
    s.after = f(after);
    s.effect = std::move(effect);
    return s;
}

std::string errorOf(const edit::EditOutcome& outcome) { return outcome ? std::string() : outcome.error().message; }

} // namespace

TEST_CASE("A two-sided transition needs media beyond the cut", "[model][transitions]") {
    // A plays source 0..4 (996 frames left after it); B starts at source frame 5 (5 frames before it).
    Timeline tl(R"(
        V1 tgt |AaaaB5bbbbb|
    )");
    const ClipId a = tl.clip("V1@0");
    const ClipId b = tl.clip("V1@4");

    const auto r = ok(edit::addTransition(tl.state, tl.seq, tl.track("V1"), twoSided(a, b, 2, 2)));
    REQUIRE(r.createdTransitions.size() == 1);
    const Track& track = trackOf(r.state, tl.seq);
    REQUIRE(track.transitions.size() == 1);
    CHECK(track.transitions[0].id == r.createdTransitions[0]);
    CHECK(track.transitions[0].before == f(2));
    CHECK(track.transitions[0].after == f(2));
    REQUIRE(r.changes.sequences.size() == 1);
    CHECK(r.changes.sequences[0].tracks[0].transitionsChanged);

    SECTION("not enough handles: a clear error, or the available handles") {
        const auto exact = edit::addTransition(tl.state, tl.seq, tl.track("V1"), twoSided(a, b, 6, 2));
        CHECK(errorOf(exact).find("'A' has 5 frames before its In point, 6 frames needed") == std::string::npos);
        CHECK(errorOf(exact).find("has 5 frames before its In point, 6 frames needed") != std::string::npos);
        const auto fitted = ok(edit::addTransition(tl.state, tl.seq, tl.track("V1"), twoSided(a, b, 6, 2),
                                                   HandleFit::UseAvailable));
        CHECK(trackOf(fitted.state, tl.seq).transitions[0].before == f(4)); // B has 5, A is only 4 frames long
    }
    SECTION("one edit point, one transition; no overlaps") {
        CHECK(errorOf(edit::addTransition(r.state, tl.seq, tl.track("V1"), twoSided(a, b, 1, 1)))
                  .find("already has transition") != std::string::npos);
        TransitionSpec tail;
        tail.outgoing = b;
        tail.before = f(5);
        CHECK(edit::addTransition(r.state, tl.seq, tl.track("V1"), tail)); // B's tail, far from the cut
        TransitionSpec head;
        head.incoming = b;
        head.after = f(1);
        CHECK_FALSE(edit::addTransition(r.state, tl.seq, tl.track("V1"), head)); // same edit point
    }
    SECTION("change and delete") {
        edit::TransitionChange change;
        change.before = f(1);
        change.effect = "dipToBlack";
        const auto changed = ok(edit::setTransition(r.state, tl.seq, r.createdTransitions[0], change));
        CHECK(trackOf(changed.state, tl.seq).transitions[0].effect == "dipToBlack");
        CHECK(trackOf(changed.state, tl.seq).transitions[0].before == f(1));
        change.before = f(9);
        CHECK_FALSE(edit::setTransition(r.state, tl.seq, r.createdTransitions[0], change));
        const auto removed = ok(edit::removeTransition(changed.state, tl.seq, r.createdTransitions[0]));
        CHECK(trackOf(removed.state, tl.seq).transitions.empty());
        CHECK_FALSE(edit::removeTransition(removed.state, tl.seq, r.createdTransitions[0]));
    }
}

TEST_CASE("One-sided transitions sit inside their clip", "[model][transitions]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaa..|
    )");
    TransitionSpec fromBlack;
    fromBlack.incoming = tl.clip("V1@0");
    fromBlack.after = f(2);
    fromBlack.effect = "dipToBlack";
    auto r = ok(edit::addTransition(tl.state, tl.seq, tl.track("V1"), fromBlack));
    TransitionSpec toBlack;
    toBlack.outgoing = tl.clip("V1@0");
    toBlack.before = f(2);
    r = ok(edit::addTransition(r.state, tl.seq, tl.track("V1"), toBlack));
    CHECK(trackOf(r.state, tl.seq).transitions.size() == 2);
    toBlack.before = f(7);
    CHECK(errorOf(edit::addTransition(tl.state, tl.seq, tl.track("V1"), toBlack)).find("longer") != std::string::npos);
}

TEST_CASE("Transitions check their effect, parameters and track", "[model][transitions]") {
    Timeline tl(R"(
        V1 tgt  |AaaaB5bbbbb|
        A1 tgt  |AaaaB5bbbbb|
        A2 lock |AaaaB5bbbbb|
    )");
    const ClipId a = tl.clip("V1@0");
    const ClipId b = tl.clip("V1@4");
    TransitionSpec wipe = twoSided(a, b, 1, 1, "wipe");
    wipe.params = {{"angle", ParamValue(90.0)}};
    const auto r = ok(edit::addTransition(tl.state, tl.seq, tl.track("V1"), wipe));
    CHECK(trackOf(r.state, tl.seq).transitions[0].params[0].key == "angle");
    wipe.params = {{"angle", ParamValue(400.0)}};
    CHECK_FALSE(edit::addTransition(tl.state, tl.seq, tl.track("V1"), wipe));
    wipe.params = {{"pattern", ParamValue(1.0)}};
    CHECK_FALSE(edit::addTransition(tl.state, tl.seq, tl.track("V1"), wipe));
    CHECK_FALSE(edit::addTransition(tl.state, tl.seq, tl.track("V1"), twoSided(a, b, 1, 1, "constantPower")));
    CHECK_FALSE(edit::addTransition(tl.state, tl.seq, tl.track("V1"), twoSided(a, b, 1, 1, "vendor.flash")));
    CHECK(edit::addTransition(tl.state, tl.seq, tl.track("A1"),
                              twoSided(tl.clip("A1@0"), tl.clip("A1@4"), 1, 1, "constantPower")));
    CHECK(errorOf(edit::addTransition(tl.state, tl.seq, tl.track("A2"),
                                      twoSided(tl.clip("A2@0"), tl.clip("A2@4"), 1, 1, "constantPower")))
              .find("locked") != std::string::npos);

    // Parameters survive the project file.
    const auto text = io::toJson(r.state);
    REQUIRE(text);
    const auto loaded = io::fromJson(*text);
    REQUIRE(loaded);
    CHECK(*loaded == r.state);
}

TEST_CASE("Alignment splits the duration around the cut", "[model][transitions]") {
    Timeline tl(R"(
        V1 tgt |AaaaaaB9bbbbb|
    )");
    const ClipId a = tl.clip("V1@0");
    const auto at = [&](edit::TransitionAlignment alignment, edit::Edge edge, ClipId clip) {
        return edit::transitionAtEdge(tl.state, tl.seq, clip, edge, f(5), alignment, "crossDissolve");
    };
    const auto center = at(edit::TransitionAlignment::Center, edit::Edge::Tail, a);
    REQUIRE(center);
    CHECK(center->spec.outgoing == a);
    CHECK(center->spec.incoming == tl.clip("V1@6"));
    CHECK(center->spec.before == f(2));
    CHECK(center->spec.after == f(3));
    const auto start = at(edit::TransitionAlignment::StartAtCut, edit::Edge::Head, tl.clip("V1@6"));
    CHECK(start->spec.before == Ticks{});
    CHECK(start->spec.after == f(5));
    CHECK(at(edit::TransitionAlignment::EndAtCut, edit::Edge::Tail, a)->spec.before == f(5));
    // No neighbour: one-sided inside the clip, whatever the alignment.
    const auto head = at(edit::TransitionAlignment::Center, edit::Edge::Head, a);
    CHECK_FALSE(head->spec.outgoing.isValid());
    CHECK(head->spec.after == f(5));
    CHECK(ok(edit::addTransition(tl.state, tl.seq, center->track, center->spec)).createdTransitions.size() == 1);
}

TEST_CASE("Apply Video / Audio Transition (Ctrl+D / Ctrl+Shift+D)", "[model][transitions]") {
    Timeline tl(R"(
        V1 tgt |AaaaaaaaaaB9bbbbbbbbbb|
        A1 tgt |AaaaaaaaaaB9bbbbbbbbbb|
        A2     |Mmmmmmmmmmmmmmmmmmmmmm|
    )");
    edit::DefaultTransitions defaults;
    defaults.videoDuration = f(8);
    // The playhead is nearer the cut at 10 than any clip end.
    const auto r = ok(edit::applyDefaultTransition(tl.state, tl.seq, f(8), TrackKind::Video, defaults));
    REQUIRE(r.createdTransitions.size() == 1);
    const Transition& t = trackOf(r.state, tl.seq).transitions[0];
    CHECK(t.effect == "crossDissolve");
    CHECK(t.before == f(4));
    CHECK(t.after == f(4));
    CHECK(r.state.findSequence(tl.seq)->audioTracks[0]->transitions.empty()); // video only

    // Audio: one second at 25 fps, centered, shortened to B's 9 frames of head handle... and A's length.
    const auto audio = ok(edit::applyDefaultTransition(r.state, tl.seq, f(11), TrackKind::Audio));
    const Transition& at = audio.state.findSequence(tl.seq)->audioTracks[0]->transitions[0];
    CHECK(at.effect == "constantPower");
    CHECK(at.before == f(9));
    CHECK(at.after == f(12));
    CHECK(audio.state.findSequence(tl.seq)->audioTracks[1]->transitions.empty()); // A2 is not targeted
    // Again on the same edit point: nothing left to add.
    CHECK_FALSE(edit::applyDefaultTransition(audio.state, tl.seq, f(11), TrackKind::Audio));
}

TEST_CASE("Apply Default Transitions to Selection (Shift+D)", "[model][transitions]") {
    Timeline tl(R"(
        V1 tgt |AaaaaaB9bbbbb|
        A1 tgt |AaaaaaB9bbbbb|
    )");
    const std::vector<ClipId> clips{tl.clip("V1@0"), tl.clip("V1@6"), tl.clip("A1@6")};
    edit::DefaultTransitions defaults;
    defaults.videoDuration = f(2);
    defaults.audioDuration = f(2);
    const auto r = ok(edit::applyDefaultTransitions(tl.state, tl.seq, clips, defaults));
    const Sequence& seq = *r.state.findSequence(tl.seq);
    // V1: head of A (one-sided), the A|B cut (shared by two selected clips: once), tail of B (one-sided).
    CHECK(seq.videoTracks[0]->transitions.size() == 3);
    // A1: only B was selected: the cut and B's tail.
    CHECK(seq.audioTracks[0]->transitions.size() == 2);
    CHECK(seq.audioTracks[0]->transitions[0].effect == "constantPower");
    CHECK(r.createdTransitions.size() == 5);
}

TEST_CASE("Interchange mapping: an imported cross dissolve becomes a transition", "[model][transitions][interchange]") {
    // What TimelineBuilder::importTransition will do with an FCP7 <transitionitem>: the IR gives the track, the
    // cut, the extent before/after the cut and the kind; the importer finds the clips meeting at the cut.
    Timeline tl(R"(
        V1 tgt |AaaaaaB9bbbbb|
        A1 tgt |AaaaaaB9bbbbb|
    )");
    struct Ir {
        std::string track;
        std::int64_t cut, before, after;
        std::string effect; // Dissolve -> crossDissolve, dip -> dipToBlack/dipToWhite, wipe, audio -> constantPower
    };
    for (const Ir& ir : {Ir{"V1", 6, 3, 3, "crossDissolve"}, Ir{"A1", 6, 0, 5, "constantPower"}}) {
        INFO(ir.track);
        const Sequence& seq = *tl.state.findSequence(tl.seq);
        const Track& track = *seq.findTrack(tl.track(ir.track));
        TransitionSpec spec;
        for (const Clip& c : track.clips) {
            if (c.end() == f(ir.cut)) {
                spec.outgoing = c.id;
            }
            if (c.start() == f(ir.cut)) {
                spec.incoming = c.id;
            }
        }
        spec.before = f(ir.before);
        spec.after = f(ir.after);
        spec.effect = ir.effect;
        const auto r = ok(edit::addTransition(tl.state, tl.seq, track.id, spec, HandleFit::UseAvailable));
        const Track& after = *r.state.findSequence(tl.seq)->findTrack(track.id);
        REQUIRE(after.transitions.size() == 1);
        CHECK(*transitionEditPoint(after, after.transitions[0]) == f(ir.cut));
        CHECK(after.transitions[0].effect == ir.effect);
    }
}

TEST_CASE("Transitions store their alignment; changes keep or re-derive it", "[model][transitions]") {
    Timeline tl(R"(
        V1 tgt |A100aaaaaaB100bbbbbbb|
    )");
    const ClipId a = tl.clip("V1@0");
    const auto added = [&](edit::TransitionAlignment alignment, std::int64_t frames) {
        const auto at = edit::transitionAtEdge(tl.state, tl.seq, a, edit::Edge::Tail, f(frames), alignment, "crossDissolve");
        REQUIRE(at);
        return ok(edit::addTransition(tl.state, tl.seq, at->track, at->spec));
    };
    const auto only = [&](const ProjectState& s) -> const Transition& { return trackOf(s, tl.seq).transitions.front(); };
    const auto set = [&](const ProjectState& s, edit::TransitionChange change) {
        return ok(edit::setTransition(s, tl.seq, only(s).id, change)).state;
    };
    const auto sides = [&](const ProjectState& s) {
        return std::pair{ticksToFrames(only(s).before, kRate), ticksToFrames(only(s).after, kRate)};
    };

    ProjectState s = added(edit::TransitionAlignment::Center, 5).state;
    CHECK(only(s).alignment == edit::TransitionAlignment::Center);
    CHECK(sides(s) == std::pair<std::int64_t, std::int64_t>{2, 3});
    edit::TransitionChange longer;
    longer.duration = f(8);
    s = set(s, longer); // a new length keeps the alignment
    CHECK(sides(s) == std::pair<std::int64_t, std::int64_t>{4, 4});
    edit::TransitionChange end;
    end.alignment = edit::TransitionAlignment::EndAtCut;
    s = set(s, end);
    CHECK(sides(s) == std::pair<std::int64_t, std::int64_t>{8, 0});
    CHECK(only(s).alignment == edit::TransitionAlignment::EndAtCut);
    edit::TransitionChange explicitSides;
    explicitSides.before = f(3);
    explicitSides.after = f(5);
    s = set(s, explicitSides); // sides set explicitly: the alignment they describe
    CHECK(only(s).alignment == edit::TransitionAlignment::Custom);
    edit::TransitionChange shorter;
    shorter.duration = f(6);
    s = set(s, shorter); // Custom keeps the cut where it is
    CHECK(sides(s) == std::pair<std::int64_t, std::int64_t>{3, 3});
    CHECK(only(s).alignment == edit::TransitionAlignment::Custom);
    edit::TransitionChange customEven;
    customEven.before = f(1);
    customEven.after = f(1);
    customEven.alignment = edit::TransitionAlignment::Custom;
    s = set(s, customEven); // asked for: stays Custom although centred
    CHECK(only(s).alignment == edit::TransitionAlignment::Custom);
    edit::TransitionChange center;
    center.alignment = edit::TransitionAlignment::Center;
    center.duration = f(1);
    s = set(s, center); // a one-frame centred transition: the side derivation cannot tell it from startAtCut
    CHECK(sides(s) == std::pair<std::int64_t, std::int64_t>{0, 1});
    CHECK(only(s).alignment == edit::TransitionAlignment::Center);
    CHECK(added(edit::TransitionAlignment::StartAtCut, 4).state.findSequence(tl.seq)->videoTracks[0]->transitions[0].alignment ==
          edit::TransitionAlignment::StartAtCut);

    // Shortened to the available handles: the sides no longer fit Center.
    Timeline shortHead(R"(
        V1 tgt |A100aaaaaaB2bbbbbbbbb|
    )");
    const auto at = edit::transitionAtEdge(shortHead.state, shortHead.seq, shortHead.clip("V1@0"), edit::Edge::Tail,
                                           f(10), edit::TransitionAlignment::Center, "crossDissolve");
    const auto fitted = ok(edit::addTransition(shortHead.state, shortHead.seq, at->track, at->spec, HandleFit::UseAvailable));
    const Transition& t = trackOf(fitted.state, shortHead.seq).transitions.front();
    CHECK(t.before == f(2));
    CHECK(t.after == f(5));
    CHECK(t.alignment == edit::TransitionAlignment::Custom);

    // One-sided transitions end or start at their clip's edge.
    Timeline lone(R"(
        V1 tgt |Aaaaaa..|
    )");
    for (const auto& [edge, expected] : {std::pair{edit::Edge::Tail, edit::TransitionAlignment::EndAtCut},
                                         std::pair{edit::Edge::Head, edit::TransitionAlignment::StartAtCut}}) {
        const auto one = edit::transitionAtEdge(lone.state, lone.seq, lone.clip("V1@0"), edge, f(2),
                                                edit::TransitionAlignment::Center, "crossDissolve");
        const auto r = ok(edit::addTransition(lone.state, lone.seq, one->track, one->spec));
        CHECK(trackOf(r.state, lone.seq).transitions.front().alignment == expected);
    }
    CHECK(transitionAlignmentFromString(toString(TransitionAlignment::Custom)) == TransitionAlignment::Custom);
    CHECK_FALSE(transitionAlignmentFromString("middle"));
}
