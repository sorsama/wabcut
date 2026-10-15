// EditSession history (contract rev 2): ids never reused across undo branches, gestures, the undo
// budgets and releasing superseded snapshots off the GUI thread.

#include "TimelineFixture.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

EditSessionOptions options(std::size_t undoLimit = 500, std::size_t memoryBudget = std::size_t{1} << 30,
                           bool deferRelease = true) {
    EditSessionOptions o;
    o.undoLimit = undoLimit;
    o.undoMemoryBudget = memoryBudget;
    o.validateEdits = true;
    o.deferRelease = deferRelease;
    return o;
}

std::uint64_t maxClipId(const ProjectState& s) {
    std::uint64_t out = 0;
    for (const SequencePtr& seq : s.sequences) {
        seq->forEachTrack([&](const Track& t) {
            for (const Clip& c : t.clips) {
                out = std::max(out, c.id.value);
            }
        });
    }
    return out;
}

} // namespace

TEST_CASE("Ids are never reused after undo, redo or a new branch", "[model][session][ids]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaa|
    )");
    EditSession session(tl.state, options());
    const auto cut = [&](std::int64_t frame) {
        return session.apply("Add Edit", [&](const ProjectState& s) { return edit::addEditToAllTracks(s, tl.seq, f(frame)); });
    };

    const auto first = cut(2);
    REQUIRE(first);
    REQUIRE(first->created.size() == 1);
    const ClipId abandoned = first->created[0];

    REQUIRE(session.undo());
    CHECK(session.snapshot()->ids.next <= abandoned.value); // the restored snapshot predates that id...
    const auto second = cut(4);                              // ...but the next edit still does not reuse it
    REQUIRE(second);
    REQUIRE(second->created.size() == 1);
    CHECK(second->created[0].value > abandoned.value);

    // Redo is gone (new branch); undo and edit again: still increasing.
    REQUIRE(session.undo());
    const auto third = cut(6);
    REQUIRE(third);
    CHECK(third->created[0].value > second->created[0].value);
    CHECK(session.idHighWater() > third->created[0].value);

    // What to save carries the high-water mark, so a reloaded project keeps counting from there.
    REQUIRE(session.undo());
    CHECK(session.stateForSave().ids.next >= session.idHighWater());
    CHECK(session.stateForSave().ids.next > maxClipId(*session.snapshot()));
    CHECK(session.stateForSave().sequences == session.snapshot()->sequences);
}

TEST_CASE("A gesture turns many applies into one undo step", "[model][session][gesture]") {
    Timeline tl(R"(
        V1 |Aa........|
    )");
    EditSession session(tl.state, options());
    const ProjectSnapshot before = session.snapshot();
    const ClipId a = tl.clip("V1@0");
    std::vector<std::string> labels;
    session.addListener([&](const ChangeEvent& e) { labels.emplace_back(e.label); });

    session.beginGesture("Drag Clip");
    for (int step = 0; step < 4; ++step) {
        REQUIRE(session.apply("Move", [&](const ProjectState& s) {
            return edit::moveClips(s, tl.seq, std::vector<ClipId>{a}, f(1));
        }));
    }
    CHECK(session.inGesture());
    session.endGesture();
    CHECK_FALSE(session.inGesture());
    CHECK(session.undoCount() == 1);
    CHECK(session.undoLabel() == "Drag Clip");
    CHECK(labels == std::vector<std::string>(4, "Drag Clip"));
    CHECK_TIMELINE(*session.snapshot(), tl.seq, R"(
        V1 |....Aa....|
    )");

    REQUIRE(session.undo());
    CHECK(session.snapshot() == before);
    REQUIRE(session.redo());
    CHECK_TIMELINE(*session.snapshot(), tl.seq, R"(
        V1 |....Aa....|
    )");
}

TEST_CASE("Gestures nest, override merge keys, and end on undo", "[model][session][gesture]") {
    Timeline tl(R"(
        V1 |Aaaa|
    )");
    EditSession session(tl.state, options());
    const auto mark = [&](std::int64_t frame, std::string_view key = {}) {
        return session.apply("Mark In", [&](const ProjectState& s) { return edit::markIn(s, tl.seq, f(frame)); }, key);
    };

    session.beginGesture("Outer");
    REQUIRE(mark(1, "slider"));
    session.beginGesture("Inner");
    REQUIRE(mark(2, "other"));
    session.endGesture(); // still inside the outer gesture
    REQUIRE(mark(3));
    session.endGesture();
    CHECK(session.undoCount() == 1);
    CHECK(session.undoLabel() == "Outer");

    session.beginGesture("Interrupted");
    REQUIRE(mark(2));
    REQUIRE(session.undo()); // undo inside a gesture ends it and undoes its step
    CHECK_FALSE(session.inGesture());
    CHECK(session.snapshot()->findSequence(tl.seq)->markIn == f(3));
}

TEST_CASE("Cancelling a gesture returns to where it started, with no undo step", "[model][session][gesture]") {
    Timeline tl(R"(
        V1 |Aa....|
    )");
    EditSession session(tl.state, options());
    REQUIRE(session.apply("Mark In", [&](const ProjectState& s) { return edit::markIn(s, tl.seq, f(1)); }));
    const ProjectSnapshot before = session.snapshot();
    std::vector<ChangeKind> kinds;
    session.addListener([&](const ChangeEvent& e) { kinds.push_back(e.kind); });

    session.beginGesture("Drag Clip");
    REQUIRE(session.apply("Move", [&](const ProjectState& s) {
        return edit::moveClips(s, tl.seq, std::vector<ClipId>{tl.clip("V1@0")}, f(3));
    }));
    const auto cancelled = session.cancelGesture();
    REQUIRE(cancelled);
    CHECK(cancelled->sequence(tl.seq)->modifiedClips.size() == 1);
    CHECK(session.snapshot() == before);
    CHECK(session.undoCount() == 1);
    CHECK(session.undoLabel() == "Mark In");
    CHECK(kinds == std::vector<ChangeKind>{ChangeKind::Edit, ChangeKind::GestureCancelled});
    CHECK_FALSE(session.cancelGesture()); // nothing open
}

TEST_CASE("The undo history is capped by memory as well as by steps", "[model][session][budget]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    )");
    SECTION("each step's estimate is what it keeps alive beyond the next revision") {
        EditSession session(tl.state, options());
        for (int i = 1; i < 6; ++i) {
            REQUIRE(session.apply("Add Edit", [&](const ProjectState& s) { return edit::addEditToAllTracks(s, tl.seq, f(i)); }));
        }
        CHECK(session.undoCount() == 5);
        CHECK(session.undoMemoryBytes() > 0);
        CHECK(session.undoMemoryBytes() < std::size_t{64} * 1024); // only touched tracks and chunks count
        REQUIRE(session.undo());
        REQUIRE(session.undo());
        CHECK(session.undoCount() == 3);
    }
    SECTION("over budget, the oldest steps go first; the latest always stays") {
        EditSession session(tl.state, options(500, 1));
        for (int i = 1; i < 6; ++i) {
            REQUIRE(session.apply("Add Edit", [&](const ProjectState& s) { return edit::addEditToAllTracks(s, tl.seq, f(i)); }));
        }
        CHECK(session.undoCount() == 1);
        REQUIRE(session.undo());
        CHECK(session.snapshot()->findSequence(tl.seq)->videoTracks[0]->clips.size() == 5); // one cut undone
    }
}

TEST_CASE("Superseded snapshots wait for releasePending() on another thread", "[model][session][release]") {
    Timeline tl(R"(
        V1 |Aaaa|
    )");
    EditSession session(tl.state, options(1));
    const std::weak_ptr<const ProjectState> original = session.snapshot();
    REQUIRE(session.apply("Mark In", [&](const ProjectState& s) { return edit::markIn(s, tl.seq, f(1)); }));
    REQUIRE(session.apply("Mark In", [&](const ProjectState& s) { return edit::markIn(s, tl.seq, f(2)); }));
    // The cap of one step evicted the original snapshot: queued, not freed on this thread.
    CHECK(session.pendingReleases() >= 1);
    CHECK_FALSE(original.expired());

    std::size_t released = 0;
    std::thread worker([&] { released = session.releasePending(); });
    worker.join();
    CHECK(released >= 1);
    CHECK(session.pendingReleases() == 0);
    CHECK(original.expired());

    // With deferral off, nothing waits.
    EditSession direct(tl.state, options(1, std::size_t{1} << 30, false));
    REQUIRE(direct.apply("Mark In", [&](const ProjectState& s) { return edit::markIn(s, tl.seq, f(1)); }));
    REQUIRE(direct.apply("Mark In", [&](const ProjectState& s) { return edit::markIn(s, tl.seq, f(2)); }));
    CHECK(direct.pendingReleases() == 0);
}
