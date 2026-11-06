// 0.7 (card 064): Lock Clip - a locked clip never changes, without locking its track - and (card 067) moving a track within
// its stack: the track keeps its id, clips, links and settings.

#include "TimelineFixture.h"

#include <wabcut/model/Clipboard.h>
#include <wabcut/model/ClipLock.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

EditSessionOptions validating() {
    EditSessionOptions options;
    options.validateEdits = true;
    return options;
}

ProjectState lockClips(const ProjectState& s, SequenceId seq, std::vector<ClipId> ids, bool on = true) {
    return ok(edit::setClipsLocked(s, seq, ids, on)).state;
}

const Clip& clipOf(const ProjectState& s, SequenceId seq, ClipId id) {
    return s.findSequence(seq)->findClip(id)->clip();
}

/// The outcome is a refusal that names a locked clip.
template <class Outcome>
void requireLockedRefusal(const Outcome& outcome, const std::string& name) {
    REQUIRE_FALSE(outcome);
    CHECK(edit::isLockedClipError(outcome.error()));
    CHECK(outcome.error().message == "clip '" + name + "' is locked");
}

constexpr const char* kThree = R"(
    V1 tgt |AaaaBbbbCccc|
    A1 tgt |AaaaBbbbCccc|
)";

} // namespace

TEST_CASE("Lock Clip sets the flag on exactly the named clips and nothing else", "[model][clip-lock]") {
    Timeline tl(kThree);
    tl.linkMatching();
    const ClipId b = tl.clip("V1@4");
    const ClipId bAudio = tl.clip("A1@4");
    const auto r = ok(edit::setClipsLocked(tl.state, tl.seq, std::vector<ClipId>{b}, true));
    CHECK(clipOf(r.state, tl.seq, b).locked);
    CHECK_FALSE(clipOf(r.state, tl.seq, bAudio).locked); // the partner is the caller's to name
    CHECK_FALSE(clipOf(r.state, tl.seq, tl.clip("V1@0")).locked);
    CHECK_TIMELINE(r.state, tl.seq, kThree);
    requireValid(r.state);
    // The lock changes nothing but the flag: the clip is otherwise equal.
    Clip unlocked = clipOf(r.state, tl.seq, b);
    unlocked.locked = false;
    CHECK(unlocked == clipOf(tl.state, tl.seq, b));

    // Locking what is locked, or unlocking what is not, changes nothing; an unknown clip is NotFound.
    CHECK(ok(edit::setClipsLocked(r.state, tl.seq, std::vector<ClipId>{b}, true)).changes.sequences.empty());
    CHECK(ok(edit::setClipsLocked(tl.state, tl.seq, std::vector<ClipId>{b}, false)).changes.sequences.empty());
    const auto missing = edit::setClipsLocked(tl.state, tl.seq, std::vector<ClipId>{ClipId{987654}}, true);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::NotFound);

    // Both halves of a linked pair, named together, lock together; it works on a locked track too.
    const auto pair = ok(edit::setClipsLocked(tl.state, tl.seq, std::vector<ClipId>{b, bAudio}, true));
    CHECK(clipOf(pair.state, tl.seq, bAudio).locked);
    edit::TrackStateChange lockTrack;
    lockTrack.locked = true;
    const ProjectState trackLocked = ok(edit::setTrackState(tl.state, tl.seq, tl.track("A1"), lockTrack)).state;
    CHECK(clipOf(lockClips(trackLocked, tl.seq, {bAudio}), tl.seq, bAudio).locked);
}

TEST_CASE("A locked clip is refused by every operation that would change it", "[model][clip-lock]") {
    Timeline tl(kThree);
    const ClipId b = tl.clip("V1@4");
    const ClipId a = tl.clip("V1@0");
    const ProjectState s = lockClips(tl.state, tl.seq, {b});
    const std::vector<ClipId> only{b};

    requireLockedRefusal(edit::moveClips(s, tl.seq, only, f(2)), "B");
    requireLockedRefusal(edit::moveClips(s, tl.seq, only, f(2), {}, edit::MoveMode::Insert), "B");
    requireLockedRefusal(edit::moveClips(s, tl.seq, only, f(8), {}, edit::MoveMode::Rearrange), "B");
    requireLockedRefusal(edit::trimEdge(s, tl.seq, b, edit::Edge::Head, f(1)), "B");
    requireLockedRefusal(edit::trimEdge(s, tl.seq, b, edit::Edge::Tail, f(-1)), "B");
    requireLockedRefusal(edit::rippleTrim(s, tl.seq, b, edit::Edge::Tail, f(-1)), "B");
    requireLockedRefusal(edit::rollEdit(s, tl.seq, b, edit::Edge::Tail, f(1)), "B");
    requireLockedRefusal(edit::deleteClips(s, tl.seq, only), "B");
    requireLockedRefusal(edit::rippleDeleteClips(s, tl.seq, only), "B");
    requireLockedRefusal(edit::slip(s, tl.seq, only, f(1)), "B");
    requireLockedRefusal(edit::slide(s, tl.seq, b, f(1)), "B");
    edit::ClipAttributeChange rename;
    rename.name = "Approved";
    requireLockedRefusal(edit::setClipAttributes(s, tl.seq, only, rename), "B");
    // A selection that holds a locked clip changes nothing at all.
    requireLockedRefusal(edit::deleteClips(s, tl.seq, std::vector<ClipId>{a, b}), "B");

    // A neighbour that does not touch it is edited as ever.
    const auto deleted = ok(edit::deleteClips(s, tl.seq, std::vector<ClipId>{a}));
    CHECK_TIMELINE(deleted.state, tl.seq, R"(
        V1 |....BbbbCccc|
        A1 |AaaaBbbbCccc|
    )");
    CHECK(clipOf(deleted.state, tl.seq, b).locked);

    // The razor does not cut it (as it skips a locked track); a copy of it is allowed and is not locked.
    const auto cut = ok(edit::addEditToAllTracks(s, tl.seq, f(6)));
    CHECK(clipOf(cut.state, tl.seq, b).duration() == f(4));
    CHECK(cut.state.findSequence(tl.seq)->videoTracks[0]->clips.size() == 3); // V1 is whole, A1 (not locked) is cut
    CHECK(cut.state.findSequence(tl.seq)->audioTracks[0]->clips.size() == 4);
    const auto copy = ok(edit::duplicateClips(s, tl.seq, only, f(8)));
    REQUIRE(copy.placed.size() == 1);
    CHECK_FALSE(clipOf(copy.state, tl.seq, copy.placed.front()).locked);
    CHECK(clipOf(copy.state, tl.seq, b).locked);
}

TEST_CASE("An edit that would push, overwrite or split a locked clip is refused by the session", "[model][clip-lock][session]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbbCccc........|
        A1 tgt |AaaaBbbbCccc........|
        V2     |Dddd................|
    )");
    const ClipId b = tl.clip("V1@4");
    EditSession session(lockClips(tl.state, tl.seq, {b}), validating());
    const auto before = session.snapshot();
    const auto refuses = [&](const char* label, auto&& fn) {
        const auto outcome = session.apply(label, fn);
        INFO(label);
        REQUIRE_FALSE(outcome);
        CHECK(outcome.error().message == "clip 'B' is locked");
        CHECK(session.snapshot() == before); // nothing happened: no new state, no undo step
        CHECK(session.undoCount() == 0);
    };
    // Removing time before it would slide it earlier.
    refuses("Ripple Delete", [&](const ProjectState& s) {
        return edit::rippleDeleteClips(s, tl.seq, std::vector<ClipId>{clipAt(s, tl.seq, "V1@0")});
    });
    // Moving a clip onto it would overwrite it.
    refuses("Move", [&](const ProjectState& s) {
        return edit::moveClips(s, tl.seq, std::vector<ClipId>{clipAt(s, tl.seq, "V1@8")}, f(-4));
    });
    // An insert before it would push it later.
    refuses("Insert Move", [&](const ProjectState& s) {
        return edit::moveClips(s, tl.seq, std::vector<ClipId>{clipAt(s, tl.seq, "V2@0")}, f(2), edit::TrackShift{-1, 0},
                               edit::MoveMode::Insert);
    });
    // Its own name, through an operation that does not go through the clip selection: a hand-made state.
    refuses("Rename", [&](const ProjectState& s) -> std::expected<ProjectState, Error> {
        ProjectState next = s;
        const SequencePtr seq = s.sequencePtr(tl.seq);
        auto copy = std::make_shared<Sequence>(*seq);
        auto track = std::make_shared<Track>(*copy->videoTracks.front());
        track->clips.mutableAt(1).setName("Edited");
        copy->videoTracks.front() = track;
        return next.withSequence(std::move(copy));
    });

    // The same edits away from it go through.
    CHECK(session.apply("Delete C", [&](const ProjectState& s) {
        return edit::rippleDeleteClips(s, tl.seq, std::vector<ClipId>{clipAt(s, tl.seq, "V1@8")});
    }));
    CHECK(session.undoCount() == 1);
}

TEST_CASE("Unlocking changes only the flag, in one undo step each way", "[model][clip-lock][session]") {
    Timeline tl(kThree);
    const ClipId b = tl.clip("V1@4");
    EditSession session(tl.state, validating());
    REQUIRE(session.apply("Lock Clip", [&](const ProjectState& s) { return edit::setClipsLocked(s, tl.seq, std::vector<ClipId>{b}, true); }));
    CHECK(session.undoLabel() == "Lock Clip");
    CHECK(clipOf(*session.snapshot(), tl.seq, b).locked);
    REQUIRE(session.apply("Unlock Clip", [&](const ProjectState& s) { return edit::setClipsLocked(s, tl.seq, std::vector<ClipId>{b}, false); }));
    CHECK_FALSE(clipOf(*session.snapshot(), tl.seq, b).locked);
    // Unlocked, it is edited again.
    CHECK(session.apply("Delete", [&](const ProjectState& s) { return edit::deleteClips(s, tl.seq, std::vector<ClipId>{b}); }));
    REQUIRE(session.undo());
    REQUIRE(session.undo());
    CHECK(clipOf(*session.snapshot(), tl.seq, b).locked);
    REQUIRE(session.undo());
    CHECK_FALSE(clipOf(*session.snapshot(), tl.seq, b).locked);
    REQUIRE(session.redo());
    CHECK(clipOf(*session.snapshot(), tl.seq, b).locked);
}

TEST_CASE("A lock is saved with the project and loads back; copies and pastes are not locked", "[model][clip-lock][io]") {
    Timeline tl(kThree);
    const ClipId b = tl.clip("V1@4");
    const ProjectState s = lockClips(tl.state, tl.seq, {b});
    const auto text = io::toJson(s);
    REQUIRE(text);
    const auto loaded = io::fromJson(*text);
    REQUIRE(loaded);
    CHECK(*loaded == s);
    CHECK(clipOf(*loaded, tl.seq, b).locked);
    CHECK_FALSE(clipOf(*loaded, tl.seq, tl.clip("V1@0")).locked);
    // A project without a lock writes no key for it (older builds open it).
    const auto plain = io::toJson(tl.state);
    REQUIRE(plain);
    const auto squash = [](std::string json) {
        std::erase_if(json, [](char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; });
        return json;
    };
    CHECK(squash(*plain).find("\"locked\":true") == std::string::npos);
    CHECK(squash(*text).find("\"locked\":true") != std::string::npos);

    // The clipboard: a pasted copy of a locked clip is a new, unlocked clip.
    const auto copied = copyClips(s, tl.seq, std::vector<ClipId>{b});
    REQUIRE(copied);
    const auto pasted = pasteClips(s, tl.seq, *copied, f(12));
    REQUIRE(pasted);
    int locked = 0;
    int total = 0;
    pasted->edit.state.findSequence(tl.seq)->forEachTrack([&](const Track& t) {
        for (const Clip& c : t.clips) {
            ++total;
            locked += c.locked ? 1 : 0;
        }
    });
    CHECK(total == 7);  // A B C on V1 and on A1, and the pasted B on the targeted V1
    CHECK(locked == 1); // only the original
}

TEST_CASE("A locked clip keeps its link group when its partner is moved away, and loses it when unlocked", "[model][clip-lock][links]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbb|
        A1     |AaaaBbbb|
    )");
    tl.linkMatching();
    const ClipId b = tl.clip("V1@4");
    const ClipId bAudio = tl.clip("A1@4");
    const ProjectState s = lockClips(tl.state, tl.seq, {bAudio});
    // Moving B's picture (links followed) leaves its locked sound where it is, still in the group it was in.
    const auto r = ok(edit::moveClips(s, tl.seq, std::vector<ClipId>{b}, f(0), edit::TrackShift{}, edit::MoveMode::Overwrite));
    CHECK(r.changes.sequences.empty()); // no move at all: delta 0
    const auto deleted = ok(edit::deleteClips(s, tl.seq, std::vector<ClipId>{b}));
    CHECK_TIMELINE(deleted.state, tl.seq, R"(
        V1 |Aaaa....|
        A1 |AaaaBbbb|
    )");
    requireValid(deleted.state); // a lone link group is allowed while its clip is locked
    CHECK(clipOf(deleted.state, tl.seq, bAudio).linkGroup.isValid());
    // Unlocking it dissolves the group it is alone in.
    const auto freed = ok(edit::setClipsLocked(deleted.state, tl.seq, std::vector<ClipId>{bAudio}, false));
    requireValid(freed.state);
    CHECK_FALSE(clipOf(freed.state, tl.seq, bAudio).linkGroup.isValid());
    // The guard lets that through (the flag and its group), and nothing else.
    CHECK_FALSE(edit::lockedClipProblem(deleted.state, freed.state));
}

TEST_CASE("lockedClipProblem names a locked clip that was removed, moved or edited", "[model][clip-lock]") {
    Timeline tl(kThree);
    const ClipId b = tl.clip("V1@4");
    const ProjectState s = lockClips(tl.state, tl.seq, {b});
    CHECK_FALSE(edit::lockedClipProblem(s, s));
    CHECK_FALSE(edit::lockedClipProblem(s, ok(edit::deleteClips(s, tl.seq, std::vector<ClipId>{tl.clip("V1@0")})).state));
    // Locking is not a change of a locked clip (it was not locked before); unlocking is the flag only.
    CHECK_FALSE(edit::lockedClipProblem(tl.state, s));
    CHECK_FALSE(edit::lockedClipProblem(s, tl.state));
    // The same edits as a state without the lock would allow are problems with it.
    const ProjectState removed = ok(edit::deleteClips(tl.state, tl.seq, std::vector<ClipId>{b})).state;
    const auto removedProblem = edit::lockedClipProblem(s, removed);
    REQUIRE(removedProblem);
    CHECK(removedProblem->message == "clip 'B' is locked");
    const ProjectState pushed = ok(edit::rippleDeleteClips(tl.state, tl.seq, std::vector<ClipId>{tl.clip("V1@0")})).state;
    CHECK(edit::lockedClipProblem(s, pushed));
}

// ---- Card 067: moving a track within its stack ---------------------------------------------------------------------------

TEST_CASE("Move Track puts a track at another place of its stack and keeps everything on it", "[model][track-move]") {
    Timeline tl(R"(
        V3     |Cccc....|
        V2     |Bbbb....|
        V1 tgt |Aaaa....|
        A1 tgt |Aaaa....|
        A2     |Dddd....|
        A3     |Eeee....|
    )");
    tl.linkMatching();
    const ClipId a = tl.clip("V1@0");
    const auto upTwo = ok(edit::moveTrack(tl.state, tl.seq, tl.track("V1"), 3));
    CHECK_TIMELINE(upTwo.state, tl.seq, R"(
        V3 |Aaaa....|
        V2 |Cccc....|
        V1 |Bbbb....|
        A1 |Aaaa....|
        A2 |Dddd....|
        A3 |Eeee....|
    )");
    requireValid(upTwo.state);
    const Sequence& moved = *upTwo.state.findSequence(tl.seq);
    CHECK(moved.videoTracks[2]->id == tl.track("V1")); // the same track: id, targeting, clips and links
    CHECK(moved.videoTracks[2]->targeted);
    CHECK(clipAt(upTwo.state, tl.seq, "V3@0") == a);
    CHECK(upTwo.changes.sequences.size() == 1);
    CHECK(upTwo.changes.sequences.front().trackListChanged);
    CHECK(upTwo.changes.sequences.front().addedClips.empty());
    CHECK(upTwo.changes.sequences.front().removedClips.empty());
    CHECK(links(upTwo.state, tl.seq) == "V3@0+A1@0"); // the picture is still linked to its sound

    const auto down = ok(edit::moveTrack(tl.state, tl.seq, tl.track("V3"), 1));
    CHECK_TIMELINE(down.state, tl.seq, R"(
        V3 |Bbbb....|
        V2 |Aaaa....|
        V1 |Cccc....|
        A1 |Aaaa....|
        A2 |Dddd....|
        A3 |Eeee....|
    )");

    // Audio tracks have a stack of their own: A3 to the top of it.
    const auto audio = ok(edit::moveTrack(tl.state, tl.seq, tl.track("A3"), 1));
    CHECK_TIMELINE(audio.state, tl.seq, R"(
        V3 |Cccc....|
        V2 |Bbbb....|
        V1 |Aaaa....|
        A1 |Eeee....|
        A2 |Aaaa....|
        A3 |Dddd....|
    )");
    CHECK(audio.state.findSequence(tl.seq)->videoTracks[0]->id == tl.track("V1")); // the video stack did not move
}

TEST_CASE("Move Track clamps the position, ignores a track already there and refuses what is not a track", "[model][track-move]") {
    Timeline tl(R"(
        V2     |Bbbb|
        V1 tgt |Aaaa|
        A1 tgt |Aaaa|
    )");
    const auto past = ok(edit::moveTrack(tl.state, tl.seq, tl.track("V1"), 99));
    CHECK_TIMELINE(past.state, tl.seq, R"(
        V2 |Aaaa|
        V1 |Bbbb|
        A1 |Aaaa|
    )");
    CHECK(ok(edit::moveTrack(tl.state, tl.seq, tl.track("V1"), 1)).changes.sequences.empty());
    CHECK(ok(edit::moveTrack(tl.state, tl.seq, tl.track("V1"), 0)).changes.sequences.empty()); // 0 is the first place
    CHECK(ok(edit::moveTrack(tl.state, tl.seq, tl.track("A1"), 5)).changes.sequences.empty()); // the only audio track
    const auto unknown = edit::moveTrack(tl.state, tl.seq, TrackId{424242}, 1);
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error().code == ErrorCode::NotFound);
}

TEST_CASE("A locked track moves; the move is one undo step and the order is saved", "[model][track-move][session]") {
    Timeline tl(R"(
        V2 lock |Bbbb|
        V1 tgt  |Aaaa|
        A1 tgt  |Aaaa|
    )");
    EditSession session(tl.state, validating());
    REQUIRE(session.apply("Move Track", [&](const ProjectState& s) { return edit::moveTrack(s, tl.seq, tl.track("V2"), 1); }));
    CHECK(session.undoCount() == 1);
    CHECK(session.undoLabel() == "Move Track");
    CHECK_TIMELINE(*session.snapshot(), tl.seq, R"(
        V2 |Aaaa|
        V1 |Bbbb|
        A1 |Aaaa|
    )");
    CHECK(session.snapshot()->findSequence(tl.seq)->videoTracks[0]->locked); // the locked track is the same one
    const auto text = io::toJson(*session.snapshot());
    REQUIRE(text);
    const auto loaded = io::fromJson(*text);
    REQUIRE(loaded);
    CHECK_TIMELINE(*loaded, tl.seq, R"(
        V2 |Aaaa|
        V1 |Bbbb|
        A1 |Aaaa|
    )");
    REQUIRE(session.undo());
    CHECK_TIMELINE(*session.snapshot(), tl.seq, R"(
        V2 |Bbbb|
        V1 |Aaaa|
        A1 |Aaaa|
    )");
}
