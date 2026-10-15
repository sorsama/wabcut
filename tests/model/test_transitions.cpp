// Transitions as edge objects: validation, handle requirements, and how edits keep them valid.

#include "TimelineFixture.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

/// The fixture's state with `transitions` put on track `label`.
ProjectState withTransitions(Timeline& tl, std::string_view label, std::vector<Transition> transitions) {
    auto seq = std::make_shared<Sequence>(tl.sequence());
    const TrackId id = tl.track(label);
    for (auto* list : {&seq->videoTracks, &seq->audioTracks}) {
        for (TrackPtr& t : *list) {
            if (t->id == id) {
                auto copy = std::make_shared<Track>(*t);
                copy->transitions = std::move(transitions);
                t = copy;
            }
        }
    }
    const IdGenerator ids = tl.state.ids;
    ProjectState out = tl.state.withSequence(seq);
    out.ids = ids;
    return out;
}

Transition dissolve(Timeline& tl, ClipId a, ClipId b, std::int64_t before, std::int64_t after) {
    return Transition{tl.state.ids.allocate<TransitionId>(), a, b, f(before), f(after), "crossDissolve"};
}

std::string problems(const ProjectState& s, Validation level = Validation::Strict) {
    const auto valid = validate(s, level);
    return valid ? std::string() : valid.error().message;
}

const std::vector<Transition>& transitionsOf(const ProjectState& s, SequenceId seq, std::size_t videoTrack = 0) {
    return s.findSequence(seq)->videoTracks[videoTrack]->transitions;
}

} // namespace

TEST_CASE("A cross dissolve sits on an edit point and needs handles on both sides", "[model][transitions]") {
    Timeline tl(R"(
        V1 |AaaaB3bb|
    )",
                {{'A', 5}});
    const ClipId a = tl.clip("V1@0");
    const ClipId b = tl.clip("V1@4");

    SECTION("enough handles") {
        const Transition t = dissolve(tl, a, b, 2, 1);
        CHECK(t.handles().outgoingTail == f(1)); // A plays one frame past its Out point
        CHECK(t.handles().incomingHead == f(2)); // B starts two frames early
        CHECK(t.duration() == f(3));
        CHECK(problems(withTransitions(tl, "V1", {t})).empty());
    }
    SECTION("not enough source before B's In point") {
        const ProjectState s = withTransitions(tl, "V1", {dissolve(tl, a, b, 4, 1)}); // B has 3 frames
        CHECK(problems(s).find("before its incoming clip") != std::string::npos);
        CHECK(problems(s, Validation::Structure).empty()); // geometry is fine; handles are a Strict check
    }
    SECTION("not enough source after A's Out point") {
        const ProjectState s = withTransitions(tl, "V1", {dissolve(tl, a, b, 1, 2)}); // A media has 5 frames
        CHECK(problems(s).find("after its outgoing clip") != std::string::npos);
    }
    SECTION("longer than a clip") {
        CHECK(problems(withTransitions(tl, "V1", {dissolve(tl, a, b, 5, 0)})).find("longer than its outgoing") !=
              std::string::npos);
    }
    SECTION("zero length or no clips") {
        CHECK(problems(withTransitions(tl, "V1", {dissolve(tl, a, b, 0, 0)})).find("zero length") != std::string::npos);
        CHECK(problems(withTransitions(tl, "V1", {dissolve(tl, ClipId{}, ClipId{}, 1, 0)})).find("no clip") !=
              std::string::npos);
    }
}

TEST_CASE("One-sided transitions stay inside their clip", "[model][transitions]") {
    Timeline tl(R"(
        V1 |Aaaa..Bbbb|
    )");
    const ClipId a = tl.clip("V1@0");
    const ClipId b = tl.clip("V1@6");
    // To black at A's tail and from black at B's head: no handles needed.
    const Transition toBlack = dissolve(tl, a, ClipId{}, 2, 0);
    const Transition fromBlack = dissolve(tl, ClipId{}, b, 0, 2);
    CHECK(toBlack.handles().outgoingTail == Ticks{});
    CHECK(problems(withTransitions(tl, "V1", {toBlack, fromBlack})).empty());

    CHECK(problems(withTransitions(tl, "V1", {dissolve(tl, a, ClipId{}, 2, 1)})).find("past the end") != std::string::npos);
    CHECK(problems(withTransitions(tl, "V1", {dissolve(tl, ClipId{}, b, 1, 2)})).find("starts before") != std::string::npos);
}

TEST_CASE("Transitions must join neighbours and must not overlap", "[model][transitions]") {
    Timeline tl(R"(
        V1 |Aaaa..B5bbC5cc|
    )");
    const ClipId a = tl.clip("V1@0");
    const ClipId b = tl.clip("V1@6");
    const ClipId c = tl.clip("V1@10");
    CHECK(problems(withTransitions(tl, "V1", {dissolve(tl, a, b, 1, 1)})).find("do not meet") != std::string::npos);
    // B is 4 frames: dissolves of 3 frames into and out of it overlap inside it.
    const ProjectState overlapping =
        withTransitions(tl, "V1", {dissolve(tl, ClipId{}, b, 0, 3), dissolve(tl, b, c, 3, 1)});
    CHECK(problems(overlapping).find("overlaps") != std::string::npos);
    CHECK(problems(withTransitions(tl, "V1", {dissolve(tl, b, c, 2, 2)})).empty());
}

TEST_CASE("Edits drop transitions they break and keep the rest", "[model][transitions]") {
    Timeline tl(R"(
        V1 |AaaaB3bbbbC3cc|
    )");
    const ClipId a = tl.clip("V1@0");
    const ClipId b = tl.clip("V1@4");
    const ClipId c = tl.clip("V1@10");
    tl.state = withTransitions(tl, "V1", {dissolve(tl, a, b, 1, 1), dissolve(tl, b, c, 1, 1)});
    requireValid(tl.state);

    SECTION("deleting a clip removes its transitions") {
        const auto r = ok(edit::deleteClips(tl.state, tl.seq, std::vector<ClipId>{c}));
        REQUIRE(transitionsOf(r.state, tl.seq).size() == 1);
        CHECK(transitionsOf(r.state, tl.seq)[0].incoming == b);
        CHECK(r.changes.sequence(tl.seq)->tracks[0].transitionsChanged);
    }
    SECTION("cutting a clip hands its Out-point transition to the right-hand piece") {
        const auto r = ok(edit::addEdit(tl.state, tl.seq, f(7), std::vector<TrackId>{tl.track("V1")}));
        const ClipId rightOfB = clipAt(r.state, tl.seq, "V1@7");
        const auto& ts = transitionsOf(r.state, tl.seq);
        REQUIRE(ts.size() == 2);
        CHECK(ts[0].incoming == b);
        CHECK(ts[1].outgoing == rightOfB);
        CHECK(ts[1].incoming == c);
    }
    SECTION("a rolling edit moves the edit point and keeps the transition") {
        const auto r = ok(edit::rollEdit(tl.state, tl.seq, a, edit::Edge::Tail, f(1)));
        CHECK(transitionsOf(r.state, tl.seq).size() == 2);
    }
    SECTION("moving a clip away from its neighbour drops the transition between them") {
        const auto r = ok(edit::moveClips(tl.state, tl.seq, std::vector<ClipId>{c}, f(2)));
        REQUIRE(transitionsOf(r.state, tl.seq).size() == 1);
        CHECK(transitionsOf(r.state, tl.seq)[0].outgoing == a);
    }
    SECTION("transitions survive saving") {
        const auto json = io::toJson(tl.state);
        REQUIRE(json);
        const auto loaded = io::fromJson(*json);
        REQUIRE(loaded);
        CHECK(transitionsOf(*loaded, tl.seq) == transitionsOf(tl.state, tl.seq));
    }
}
