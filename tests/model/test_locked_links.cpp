// Linked partners on locked tracks (review finding P1-7).
//
// Premiere never changes a locked track: deleting, overwriting or unlinking the unlocked side of a linked
// pair leaves the locked clip exactly as it was. In the model that clip may then be the only member of
// its link group; validate() allows such a single-clip group only on a locked track, and the group
// dissolves when the track is unlocked.

#include "TimelineFixture.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

ProjectState setLocked(const ProjectState& s, SequenceId seq, TrackId track, bool locked) {
    edit::TrackStateChange change;
    change.locked = locked;
    return ok(edit::setTrackState(s, seq, track, change)).state;
}

TrackPtr trackNode(const ProjectState& s, SequenceId seq, TrackId id) {
    const Sequence& sequence = *s.findSequence(seq);
    for (const auto* list : {&sequence.videoTracks, &sequence.audioTracks}) {
        for (const TrackPtr& t : *list) {
            if (t->id == id) {
                return t;
            }
        }
    }
    FAIL("no such track");
    return nullptr;
}

/// V1/A1 A/V pairs, linked, with A1 then locked.
struct LockedPartner {
    Timeline tl;
    TrackPtr lockedA1;

    explicit LockedPartner(std::string_view picture) : tl(picture) {
        tl.linkMatching();
        tl.state = setLocked(tl.state, tl.seq, tl.track("A1"), true);
        lockedA1 = trackNode(tl.state, tl.seq, tl.track("A1"));
    }

    /// The locked track is the very same node: not copied, not changed.
    void requireLockedUntouched(const ProjectState& after) const {
        const TrackPtr now = trackNode(after, tl.seq, lockedA1->id);
        CHECK(now == lockedA1);
        CHECK(*now == *lockedA1);
    }
};

constexpr const char* kPairs = R"(
    V1 tgt |AaaaBbbb|
    A1     |AaaaBbbb|
)";

} // namespace

TEST_CASE("Deleting a clip leaves its partner on a locked track as it was", "[model][links][locked]") {
    LockedPartner fx(kPairs);
    const auto r = ok(edit::deleteClips(fx.tl.state, fx.tl.seq, std::vector<ClipId>{fx.tl.clip("V1@0")}));
    CHECK_TIMELINE(r.state, fx.tl.seq, R"(
        V1 |....Bbbb|
        A1 |AaaaBbbb|
    )");
    fx.requireLockedUntouched(r.state);
    // The locked clip is still in its (now single-clip) group; validate() accepts that on a locked track.
    CHECK(links(r.state, fx.tl.seq) == "A1@0 V1@4+A1@4");
    CHECK(r.removed.size() == 1);
}

TEST_CASE("Unlocking the track dissolves the single-clip group", "[model][links][locked]") {
    LockedPartner fx(kPairs);
    const auto deleted = ok(edit::deleteClips(fx.tl.state, fx.tl.seq, std::vector<ClipId>{fx.tl.clip("V1@0")}));
    const ProjectState unlocked = setLocked(deleted.state, fx.tl.seq, fx.tl.track("A1"), false);
    CHECK(links(unlocked, fx.tl.seq) == "V1@4+A1@4");
    CHECK_TIMELINE(unlocked, fx.tl.seq, R"(
        V1 |....Bbbb|
        A1 |AaaaBbbb|
    )");
}

TEST_CASE("Unlinking the unlocked side does not touch the locked side", "[model][links][locked]") {
    LockedPartner fx(kPairs);
    const auto r = ok(edit::unlinkClips(fx.tl.state, fx.tl.seq, std::vector<ClipId>{fx.tl.clip("V1@0")}));
    fx.requireLockedUntouched(r.state);
    CHECK(links(r.state, fx.tl.seq) == "A1@0 V1@4+A1@4");
}

TEST_CASE("Overwriting or lifting the unlocked side does not touch the locked side", "[model][links][locked]") {
    LockedPartner fx(kPairs);
    SECTION("overwrite") {
        const edit::SourceClip source{fx.tl.media('S'), {f(0), f(4)}, {{{StreamKind::Video, 0}, fx.tl.track("V1")}}};
        const auto r = ok(edit::overwriteEdit(fx.tl.state, fx.tl.seq, source, f(0)));
        CHECK_TIMELINE(r.state, fx.tl.seq, R"(
            V1 |SsssBbbb|
            A1 |AaaaBbbb|
        )");
        fx.requireLockedUntouched(r.state);
        CHECK(links(r.state, fx.tl.seq) == "A1@0 V1@4+A1@4");
    }
    SECTION("lift") {
        auto marked = ok(edit::markIn(fx.tl.state, fx.tl.seq, f(0))).state;
        marked = ok(edit::markOut(marked, fx.tl.seq, f(3))).state;
        const auto r = ok(edit::lift(marked, fx.tl.seq));
        CHECK_TIMELINE(r.state, fx.tl.seq, R"(
            V1 |....Bbbb|
            A1 |AaaaBbbb|
        )");
        fx.requireLockedUntouched(r.state);
    }
    SECTION("ripple delete: the locked side stays where it was, out of sync") {
        const auto r = ok(edit::rippleDeleteClips(fx.tl.state, fx.tl.seq, std::vector<ClipId>{fx.tl.clip("V1@0")}));
        CHECK_TIMELINE(r.state, fx.tl.seq, R"(
            V1 |Bbbb....|
            A1 |AaaaBbbb|
        )");
        fx.requireLockedUntouched(r.state);
        CHECK(links(r.state, fx.tl.seq) == "A1@0 V1@0+A1@4");
    }
}

TEST_CASE("A razor cut next to a locked partner keeps the partner untouched", "[model][links][locked]") {
    LockedPartner fx(kPairs);
    const std::vector<TrackId> v1{fx.tl.track("V1")};
    const auto r = ok(edit::addEdit(fx.tl.state, fx.tl.seq, f(2), v1, edit::LinkMode::FollowLinks));
    CHECK_TIMELINE(r.state, fx.tl.seq, R"(
        V1 |AaA2Bbbb|
        A1 |AaaaBbbb|
    )");
    fx.requireLockedUntouched(r.state);
    // The left piece keeps the link; the right piece, cut alone, is not linked.
    CHECK(links(r.state, fx.tl.seq) == "V1@0+A1@0 V1@4+A1@4");

    // Deleting the left piece now leaves the locked clip alone in its group.
    const auto deleted = ok(edit::deleteClips(r.state, fx.tl.seq, std::vector<ClipId>{fx.tl.clip("V1@0")}));
    fx.requireLockedUntouched(deleted.state);
    CHECK(links(deleted.state, fx.tl.seq) == "A1@0 V1@4+A1@4");
}

TEST_CASE("Re-linking the unlocked side elsewhere leaves the locked side's group", "[model][links][locked]") {
    LockedPartner fx(kPairs);
    const std::vector<ClipId> pair{fx.tl.clip("V1@0"), fx.tl.clip("V1@4")};
    const auto r = ok(edit::linkClips(fx.tl.state, fx.tl.seq, pair));
    fx.requireLockedUntouched(r.state);
    CHECK(links(r.state, fx.tl.seq) == "A1@0 A1@4 V1@0+V1@4");
}

TEST_CASE("Deleting a track does not touch the locked partners of its clips", "[model][links][locked]") {
    LockedPartner fx(kPairs);
    const auto r = ok(edit::removeTrack(fx.tl.state, fx.tl.seq, fx.tl.track("V1")));
    fx.requireLockedUntouched(r.state);
    CHECK(r.removed.size() == 2);
    CHECK(links(r.state, fx.tl.seq) == "A1@0 A1@4");
}

TEST_CASE("Single-clip link groups are valid only on locked tracks", "[model][links][locked]") {
    LockedPartner fx(kPairs);
    const auto deleted = ok(edit::deleteClips(fx.tl.state, fx.tl.seq, std::vector<ClipId>{fx.tl.clip("V1@0")}));
    requireValid(deleted.state);

    // The same clip on an unlocked track is a broken invariant.
    auto seq = std::make_shared<Sequence>(*deleted.state.findSequence(fx.tl.seq));
    auto a1 = std::make_shared<Track>(*seq->audioTracks[0]);
    a1->locked = false;
    seq->audioTracks[0] = a1;
    const auto broken = validate(deleted.state.withSequence(seq));
    REQUIRE_FALSE(broken);
    CHECK(broken.error().message.find("single clip") != std::string::npos);

    // A saved project with such a group loads back as it was.
    const auto json = io::toJson(deleted.state);
    REQUIRE(json);
    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded);
    CHECK(*loaded == deleted.state);
}
