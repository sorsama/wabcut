// SnapshotReleaser (P.1): what an EditSession lets go is freed soon, on the releaser's thread, instead of waiting in
// the session's release queue until the session dies (the app's leak: every undo step evicted beyond the limit and
// every previous project stayed in memory).

#include "TimelineFixture.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/SnapshotReleaser.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

EditSessionOptions limited(std::size_t undoLimit) {
    EditSessionOptions o;
    o.undoLimit = undoLimit;
    o.validateEdits = true;
    o.deferRelease = true; // the app's setting
    return o;
}

/// Waits (at most 5 s) for `done`, polling; the releaser runs on its own thread.
template <class Fn>
bool eventually(Fn done) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done()) {
        if (std::chrono::steady_clock::now() > until) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

} // namespace

TEST_CASE("Snapshots evicted from the undo history are freed by the releaser, not kept until the session ends",
          "[model][session][release]") {
    Timeline tl(R"(
        V1 |AaaaBbbb|
    )");
    std::vector<std::weak_ptr<const ProjectState>> states;
    {
        // Without a releaser (what the app did): evicted steps stay queued.
        EditSession session(tl.state, limited(3));
        states.push_back(session.snapshot());
        for (int i = 1; i <= 8; ++i) {
            REQUIRE(session.apply("Mark In", [&](const ProjectState& s) { return edit::markIn(s, tl.seq, f(i)); }));
            states.push_back(session.snapshot());
        }
        CHECK(session.pendingReleases() >= 5);
        CHECK_FALSE(states.front().expired());
        CHECK_FALSE(states[4].expired());

        // A releaser frees what was queued before it existed...
        SnapshotReleaser releaser(session);
        releaser.flush();
        CHECK(session.pendingReleases() == 0);
        CHECK(releaser.released() >= 5);
        // The undo history keeps 3 steps: states 0..4 (beyond the limit) are gone, 5..8 live.
        for (std::size_t i = 0; i <= 4; ++i) {
            INFO("state " << i);
            CHECK(states[i].expired());
        }
        for (std::size_t i = 5; i <= 8; ++i) {
            INFO("state " << i);
            CHECK_FALSE(states[i].expired());
        }

        // ...and then each change's evictions as they happen, with nobody calling releasePending().
        REQUIRE(session.apply("Mark In", [&](const ProjectState& s) { return edit::markIn(s, tl.seq, f(9)); }));
        CHECK(eventually([&] { return states[5].expired(); }));
        CHECK(session.pendingReleases() == 0);
    }
}

TEST_CASE("Closing a project (a session reset) frees the previous project and its undo history",
          "[model][session][release]") {
    Timeline tl(R"(
        V1 |AaaaBbbbCccc|
    )");
    Timeline other(R"(
        V1 |Dddd|
    )");
    EditSession session(tl.state, limited(500));
    SnapshotReleaser releaser(session);
    std::vector<std::weak_ptr<const ProjectState>> before;
    before.push_back(session.snapshot());
    for (int i = 1; i <= 4; ++i) {
        REQUIRE(session.apply("Mark In", [&](const ProjectState& s) { return edit::markIn(s, tl.seq, f(i)); }));
        before.push_back(session.snapshot());
    }
    session.reset(other.state);
    CHECK(eventually([&] {
        for (const auto& w : before) {
            if (!w.expired()) {
                return false;
            }
        }
        return true;
    }));
    CHECK(session.pendingReleases() == 0);
    CHECK_FALSE(session.canUndo());
    CHECK(*session.snapshot() == other.state);
}

TEST_CASE("A releaser destroyed before its session frees what is still queued and stops listening",
          "[model][session][release]") {
    Timeline tl(R"(
        V1 |Aaaa|
    )");
    EditSession session(tl.state, limited(1));
    std::weak_ptr<const ProjectState> first;
    {
        SnapshotReleaser releaser(session);
        first = session.snapshot();
        REQUIRE(session.apply("Mark In", [&](const ProjectState& s) { return edit::markIn(s, tl.seq, f(1)); }));
        REQUIRE(session.apply("Mark In", [&](const ProjectState& s) { return edit::markIn(s, tl.seq, f(2)); }));
    }
    CHECK(first.expired());
    CHECK(session.pendingReleases() == 0);
    // No listener left behind: further edits queue again (and the session frees them when it ends).
    REQUIRE(session.apply("Mark In", [&](const ProjectState& s) { return edit::markIn(s, tl.seq, f(3)); }));
    CHECK(session.pendingReleases() >= 1);
}
