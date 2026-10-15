// EditSession: undo/redo by snapshot, coalescing, listeners and concurrent snapshot readers.

#include "TimelineFixture.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

EditSessionOptions validating(std::size_t undoLimit = 500) {
    EditSessionOptions options;
    options.undoLimit = undoLimit;
    options.validateEdits = true;
    return options;
}

} // namespace

TEST_CASE("Undo and redo swap whole snapshots", "[model][session]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaa|
        A1 tgt |Aaaaaaaa|
    )");
    EditSession session(tl.state, validating());
    const ProjectSnapshot original = session.snapshot();
    CHECK_FALSE(session.canUndo());
    CHECK_FALSE(session.canRedo());

    const auto cut = session.apply("Add Edit", [&](const ProjectState& s) { return edit::addEditToAllTracks(s, tl.seq, f(4)); });
    REQUIRE(cut);
    CHECK(cut->created.size() == 2);
    const ProjectSnapshot afterCut = session.snapshot();

    const auto del = session.apply("Ripple Delete", [&](const ProjectState& s) {
        return edit::rippleDeleteClips(s, tl.seq, std::vector<ClipId>{clipAt(s, tl.seq, "V1@4")});
    });
    REQUIRE(del);
    CHECK_TIMELINE(*session.snapshot(), tl.seq, R"(
        V1 |Aaaa....|
        A1 |Aaaa....|
    )");
    const ProjectSnapshot afterDelete = session.snapshot();

    CHECK(session.undoCount() == 2);
    CHECK(session.undoLabel() == "Ripple Delete");
    REQUIRE(session.undo());
    CHECK(session.snapshot() == afterCut); // the very same snapshot object, no inverse op involved
    CHECK(session.redoLabel() == "Ripple Delete");
    REQUIRE(session.undo());
    CHECK(session.snapshot() == original);
    CHECK_FALSE(session.undo());

    REQUIRE(session.redo());
    REQUIRE(session.redo());
    CHECK(session.snapshot() == afterDelete);
    CHECK_FALSE(session.redo());

    // A new edit after undo drops the redo branch.
    REQUIRE(session.undo());
    REQUIRE(session.apply("Mark In", [&](const ProjectState& s) { return edit::markIn(s, tl.seq, f(2)); }));
    CHECK_FALSE(session.canRedo());
    CHECK(session.undoLabel() == "Mark In");
}

TEST_CASE("The revision counts every published snapshot", "[model][session]") {
    Timeline tl(R"(
        V1 |Aaaa|
    )");
    EditSession session(tl.state, validating());
    CHECK(session.revision() == 0);
    REQUIRE(session.apply("Mark In", [&](const ProjectState& s) { return edit::markIn(s, tl.seq, f(1)); }));
    CHECK(session.revision() == 1);
    CHECK_FALSE(session.apply("Extract", [&](const ProjectState& s) { return edit::extract(s, tl.seq); }));
    REQUIRE(session.apply("Add Edit", [&](const ProjectState& s) { return edit::addEditToAllTracks(s, tl.seq, f(0)); }));
    CHECK(session.revision() == 1); // failed and no-op edits publish nothing
    REQUIRE(session.undo());
    REQUIRE(session.redo());
    CHECK(session.revision() == 3);
    session.reset(tl.state);
    CHECK(session.revision() == 4);
}

TEST_CASE("Failed and no-op edits leave the session untouched", "[model][session]") {
    Timeline tl(R"(
        V1 |Aaaa|
    )");
    EditSession session(tl.state, validating());
    const ProjectSnapshot before = session.snapshot();

    const auto failed = session.apply("Extract", [&](const ProjectState& s) { return edit::extract(s, tl.seq); });
    CHECK_FALSE(failed);
    const auto noop = session.apply("Add Edit", [&](const ProjectState& s) { return edit::addEditToAllTracks(s, tl.seq, f(0)); });
    CHECK(noop);
    CHECK(session.snapshot() == before);
    CHECK_FALSE(session.canUndo());
}

TEST_CASE("The session refuses states that break an invariant", "[model][session]") {
    Timeline tl(R"(
        V1 |AaBb|
    )");
    EditSession session(tl.state, validating());
    const auto broken = session.apply("Corrupt", [&](const ProjectState& s) -> std::expected<ProjectState, Error> {
        auto seq = std::make_shared<Sequence>(*s.findSequence(tl.seq));
        auto track = std::make_shared<Track>(*seq->videoTracks[0]);
        track->clips.mutableAt(1).timelineRange.start = f(1); // overlaps the first clip
        seq->videoTracks[0] = track;
        return s.withSequence(seq);
    });
    REQUIRE_FALSE(broken);
    CHECK(broken.error().message.find("overlap") != std::string::npos);
    CHECK_FALSE(session.canUndo());
}

TEST_CASE("Edits with the same merge key coalesce into one undo step", "[model][session]") {
    Timeline tl(R"(
        V1 |Aaaa|
    )");
    EditSession session(tl.state, validating());
    const ClipId a = tl.clip("V1@0");
    const ProjectSnapshot original = session.snapshot();
    const auto setOpacity = [&](double opacity, std::string_view key) {
        edit::ClipAttributeChange change;
        change.properties = ClipProperties{};
        change.properties->set(ParamId::Opacity, opacity);
        return session.apply(
            "Opacity", [&](const ProjectState& s) { return edit::setClipAttributes(s, tl.seq, std::vector<ClipId>{a}, change); },
            key);
    };
    const auto opacity = [&] { return session.snapshot()->findSequence(tl.seq)->findClip(a)->clip().properties().value(ParamId::Opacity); };

    // One slider drag: many edits, one undo step back to before the drag.
    for (double v : {0.9, 0.7, 0.5, 0.3}) {
        REQUIRE(setOpacity(v, "opacity-drag"));
    }
    CHECK(session.undoCount() == 1);
    CHECK(opacity() == 0.3);

    // A second drag after endMerge() is its own step.
    session.endMerge();
    REQUIRE(setOpacity(0.2, "opacity-drag"));
    REQUIRE(setOpacity(0.1, "opacity-drag"));
    CHECK(session.undoCount() == 2);

    REQUIRE(session.undo());
    CHECK(opacity() == 0.3);
    REQUIRE(session.undo());
    CHECK(session.snapshot() == original);

    // A different key, or no key, starts a new step too.
    REQUIRE(setOpacity(0.8, "a"));
    REQUIRE(setOpacity(0.6, "b"));
    REQUIRE(setOpacity(0.4, ""));
    REQUIRE(setOpacity(0.2, ""));
    CHECK(session.undoCount() == 4);
}

TEST_CASE("The undo history is capped", "[model][session]") {
    Timeline tl(R"(
        V1 |Aaaaaaaaaa|
    )");
    EditSession session(tl.state, validating(3));
    for (int i = 1; i <= 5; ++i) {
        REQUIRE(session.apply("Mark In " + std::to_string(i),
                              [&](const ProjectState& s) { return edit::markIn(s, tl.seq, f(i)); }));
    }
    CHECK(session.undoCount() == 3);
    int undone = 0;
    while (session.undo()) {
        ++undone;
    }
    CHECK(undone == 3);
    CHECK(session.snapshot()->findSequence(tl.seq)->markIn == f(2)); // the oldest steps were dropped
}

TEST_CASE("Listeners hear about edits, undo, redo and reset", "[model][session]") {
    Timeline tl(R"(
        V1 |Aaaa|
    )");
    EditSession session(tl.state, validating());
    std::vector<std::string> heard;
    const auto id = session.addListener([&](const ChangeEvent& e) {
        const char* kinds[] = {"edit", "undo", "redo", "reset"};
        heard.push_back(std::string(kinds[static_cast<int>(e.kind)]) + ":" + std::string(e.label));
        CHECK(e.state == session.snapshot());
    });
    REQUIRE(session.apply("Mark In", [&](const ProjectState& s) { return edit::markIn(s, tl.seq, f(1)); }));
    session.undo();
    session.redo();
    session.reset(tl.state);
    CHECK_FALSE(session.canUndo());
    session.removeListener(id);
    REQUIRE(session.apply("Mark Out", [&](const ProjectState& s) { return edit::markOut(s, tl.seq, f(2)); }));
    CHECK(heard == std::vector<std::string>{"edit:Mark In", "undo:Mark In", "redo:Mark In", "reset:"});
}

TEST_CASE("Listeners may edit again from inside a notification", "[model][session]") {
    Timeline tl(R"(
        V1 |Aaaa|
    )");
    EditSession session(tl.state, validating());
    int calls = 0;
    session.addListener([&](const ChangeEvent& e) {
        ++calls;
        if (e.label == "Mark In") {
            REQUIRE(session.apply("Mark Out", [&](const ProjectState& s) { return edit::markOut(s, tl.seq, f(3)); }));
        }
    });
    REQUIRE(session.apply("Mark In", [&](const ProjectState& s) { return edit::markIn(s, tl.seq, f(1)); }));
    CHECK(calls == 2);
    CHECK(session.undoCount() == 2);
    CHECK(session.snapshot()->findSequence(tl.seq)->markedRange() == TimeRange::fromStartEnd(f(1), f(4)));
}

TEST_CASE("Engine threads read consistent snapshots while the GUI thread edits", "[model][session][threads]") {
    // V1 starts with one long clip; every edit cuts it once more. Readers check that every snapshot
    // they see is internally consistent and that revisions only move forward while no undo runs.
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )");
    constexpr std::int64_t kFrames = 40;
    REQUIRE(tl.sequence().contentEnd() == f(kFrames));
    EditSession session(tl.state, validating());
    std::atomic<bool> stop{false};
    std::atomic<int> inconsistent{0};
    std::atomic<long> reads{0};

    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&] {
            while (!stop.load()) {
                const ProjectSnapshot snap = session.snapshot();
                const Track& v1 = *snap->findSequence(tl.seq)->videoTracks[0];
                Ticks cursor{};
                for (const Clip& c : v1.clips) {
                    if (c.start() != cursor || c.duration().value <= 0) {
                        ++inconsistent;
                    }
                    cursor = c.end();
                }
                if (cursor != f(kFrames)) {
                    ++inconsistent;
                }
                if (!validate(*snap)) {
                    ++inconsistent;
                }
                ++reads;
            }
        });
    }

    for (int i = 1; i < kFrames; ++i) {
        REQUIRE(session.apply("Add Edit", [&](const ProjectState& s) {
            return edit::addEdit(s, tl.seq, f(i), edit::targetedTracks(*s.findSequence(tl.seq)));
        }));
        if (i % 10 == 0) {
            session.undo();
            session.redo();
        }
    }
    // Give the readers time to see the final revisions too.
    while (reads.load() < 2000) {
        std::this_thread::yield();
    }
    stop = true;
    for (auto& t : readers) {
        t.join();
    }
    CHECK(inconsistent.load() == 0);
    CHECK(session.snapshot()->findSequence(tl.seq)->videoTracks[0]->clips.size() == static_cast<std::size_t>(kFrames));
}
