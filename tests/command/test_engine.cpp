// CommandEngine: dry runs, commits, revisions, idempotency, permissions, validation, batches and the
// locked-track / invariant guards.

#include "CommandHarness.h"

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Schema.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;
using command::CommandRegistry;
using command::CommandSpec;

namespace {

Timeline linkedTimeline() {
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb|
        A1 tgt |Aaaa..Bbbb|
    )");
    tl.linkMatching();
    return tl;
}

Json rippleDeleteFirst() {
    return Json{{"command", "timeline.rippleDelete"},
                {"params", Json{{"clips", Json::array({Json{{"track", "V1"}, {"time", tc(1)}}})}}}};
}

Json withField(Json document, const char* key, Json value) {
    document[key] = std::move(value);
    return document;
}

std::string codeOf(const command::CommandResult& r) {
    return r.ok() ? std::string("ok") : std::string(command::toString(r.error->code));
}

} // namespace

TEST_CASE("A dry run reports the change and a preview but commits nothing", "[command][engine]") {
    CommandHarness h(linkedTimeline());
    const auto r = h.run(withField(rippleDeleteFirst(), "dryRun", true));
    REQUIRE(r.ok());
    CHECK(r.dryRun);
    CHECK_FALSE(r.committed);
    CHECK(r.revision == 0);
    REQUIRE(r.preview);
    CHECK_TIMELINE(*r.preview, h.tl.seq, R"(
        V1 |..Bbbb....|
        A1 |..Bbbb....|
    )");
    REQUIRE(r.changes.sequences.size() == 1);
    CHECK(r.changes.sequences[0].removedClips.size() == 2);
    CHECK(h.session.revision() == 0);
    CHECK(h.session.undoCount() == 0);
    CHECK_SESSION(h, R"(
        V1 |Aaaa..Bbbb|
        A1 |Aaaa..Bbbb|
    )");
    const Json json = command::toJson(r);
    CHECK(json["preview"] == true);
    CHECK(json["committed"] == false);
}

TEST_CASE("A committed command is one undo step and advances the revision", "[command][engine]") {
    CommandHarness h(linkedTimeline());
    const auto r = h.run(rippleDeleteFirst());
    REQUIRE(r.ok());
    CHECK(r.committed);
    CHECK_FALSE(r.preview);
    CHECK(r.revision == 1);
    CHECK(h.engine.revision() == 1);
    CHECK(h.session.undoCount() == 1);
    CHECK(h.session.undoLabel() == "Ripple Delete");
    CHECK_SESSION(h, R"(
        V1 |..Bbbb....|
        A1 |..Bbbb....|
    )");
    const Json json = command::toJson(r);
    CHECK(json["ok"] == true);
    CHECK(json["revision"] == 1);
    CHECK(json["changes"]["sequences"][0]["removedClips"].size() == 2);

    REQUIRE(h.session.undo());
    CHECK_SESSION(h, R"(
        V1 |Aaaa..Bbbb|
        A1 |Aaaa..Bbbb|
    )");
}

TEST_CASE("A query or a no-op edit commits nothing", "[command][engine]") {
    CommandHarness h(linkedTimeline());
    const auto query = h.ok("sequence.summary");
    CHECK_FALSE(query.committed);
    CHECK(query.data.contains("tracks"));
    const auto noop = h.ok("clip.rename", Json{{"clips", Json::array({h.clip("V1@0").value})}, {"name", "A"}});
    CHECK_FALSE(noop.committed);
    CHECK(noop.changes.empty());
    CHECK(h.session.revision() == 0);
    CHECK(h.session.undoCount() == 0);
}

TEST_CASE("A stale expectedRevision fails without editing", "[command][engine]") {
    CommandHarness h(linkedTimeline());
    const auto stale = h.run(withField(rippleDeleteFirst(), "expectedRevision", 3));
    REQUIRE_FALSE(stale.ok());
    CHECK(stale.error->code == command::ErrorCode::StaleRevision);
    CHECK(stale.error->details["expected"] == 3);
    CHECK(stale.error->details["current"] == 0);
    CHECK(h.session.undoCount() == 0);

    const auto current = h.run(withField(rippleDeleteFirst(), "expectedRevision", 0));
    REQUIRE(current.ok());
    CHECK(current.revision == 1);
    CHECK(codeOf(h.run(withField(rippleDeleteFirst(), "expectedRevision", 0))) == "StaleRevision");
}

TEST_CASE("Idempotency keys replay the first result", "[command][engine]") {
    CommandHarness h(R"(
        V1 tgt |Aaaaaaaa|
    )");
    const Json cut{{"command", "sequence.addEdit"},
                   {"params", Json{{"time", tc(2)}}},
                   {"idempotencyKey", "agent-1/step-1"}};

    SECTION("a replay returns the first result and edits nothing") {
        const auto first = h.run(cut);
        REQUIRE(first.committed);
        CHECK(first.createdIds.size() == 1);
        const auto again = h.run(cut);
        REQUIRE(again.ok());
        CHECK(again.replayed);
        CHECK(again.revision == first.revision);
        CHECK(again.createdIds == first.createdIds);
        CHECK(h.session.undoCount() == 1);
        CHECK_SESSION(h, "V1 |AaA2aaaa|");
        // Replays are answered before the revision check: a retried request still sees its first result.
        CHECK(h.run(withField(cut, "expectedRevision", 0)).replayed);
    }
    SECTION("the same key for a different request is a conflict") {
        REQUIRE(h.run(cut).ok());
        Json other = cut;
        other["params"]["time"] = tc(4);
        const auto r = h.run(other);
        CHECK(codeOf(r) == "IdempotencyConflict");
        CHECK(h.session.undoCount() == 1);
    }
    SECTION("aliases and tool names are the same request") {
        REQUIRE(h.run(cut).ok());
        CHECK(h.run(withField(cut, "command", "timeline_razor")).replayed);
    }
    SECTION("failures and dry runs are not remembered") {
        Json bad = cut;
        bad["params"]["time"] = "nonsense";
        CHECK(codeOf(h.run(bad)) == "InvalidParams");
        CHECK(h.run(withField(cut, "dryRun", true)).ok());
        const auto r = h.run(cut);
        CHECK(r.committed);
        CHECK_FALSE(r.replayed);
    }
    SECTION("the oldest keys are forgotten past the cache size") {
        CommandHarness small(R"(
            V1 tgt |Aaaaaaaa|
        )",
                             command::EnginePolicy{command::CapabilitySet::all(), 1});
        REQUIRE(small.run(cut).committed);
        Json second = cut;
        second["idempotencyKey"] = "agent-1/step-2";
        second["params"]["time"] = tc(4);
        REQUIRE(small.run(second).committed);
        Json reuse = cut;
        reuse["params"]["time"] = tc(6);
        CHECK(small.run(reuse).committed); // "agent-1/step-1" was forgotten
    }
}

TEST_CASE("Commands need their capability", "[command][engine]") {
    CommandHarness readOnly(linkedTimeline(), command::EnginePolicy{command::CapabilitySet::of({command::Capability::Read})});
    const auto denied = readOnly.run(rippleDeleteFirst());
    REQUIRE_FALSE(denied.ok());
    CHECK(denied.error->code == command::ErrorCode::PermissionDenied);
    CHECK(denied.error->details["capability"] == "EditSequence");
    CHECK(readOnly.run(Json{{"command", "sequence.summary"}}).ok());

    CommandHarness editor(linkedTimeline(), command::EnginePolicy{command::CapabilitySet::of(
                                                {command::Capability::Read, command::Capability::EditSequence})});
    CHECK(editor.run(rippleDeleteFirst()).ok());
    CHECK(editor.errorOf("sequence.addTrack", Json{{"kind", "video"}}) == "PermissionDenied");
}

TEST_CASE("Requests are validated before anything runs", "[command][engine]") {
    CommandHarness h(linkedTimeline());
    const Json firstClip = Json::array({Json{{"track", "V1"}, {"time", tc(0)}}});

    CHECK(h.errorOf("timeline.explode") == "UnknownCommand");
    CHECK(codeOf(h.run(Json{{"command", "clip.rename"}, {"version", 2}, {"params", Json::object()}})) ==
          "UnsupportedVersion");
    CHECK(codeOf(h.run(Json{{"command", "clip.rename"}, {"parms", Json::object()}})) == "InvalidRequest");

    SECTION("schema problems list JSON-pointer paths") {
        const auto r = h.run(Json{{"command", "clip.rename"},
                                  {"params", Json{{"clips", firstClip}, {"name", "x"}, {"bogus", 1}}}});
        REQUIRE_FALSE(r.ok());
        CHECK(r.error->code == command::ErrorCode::InvalidParams);
        CHECK(r.error->details["issues"][0]["path"] == "/bogus");

        const auto missing = h.run(Json{{"command", "clip.rename"}, {"params", Json{{"clips", firstClip}}}});
        REQUIRE_FALSE(missing.ok());
        CHECK(missing.error->details["issues"][0]["message"].get<std::string>().find("\"name\"") != std::string::npos);

        CHECK(h.errorOf("clip.setColor", Json{{"clips", firstClip}, {"colorIndex", 16}}) == "InvalidParams");
        CHECK(h.errorOf("clip.setColor", Json{{"clips", Json::array()}, {"colorIndex", 1}}) == "InvalidParams");
        CHECK(h.errorOf("clip.rename", Json{{"clips", Json::array({-3})}, {"name", "x"}}) == "InvalidParams");
    }
    SECTION("references must exist") {
        const auto r = h.run(Json{{"command", "clip.rename"},
                                  {"params", Json{{"clips", Json::array({9999})}, {"name", "x"}}}});
        REQUIRE_FALSE(r.ok());
        CHECK(r.error->code == command::ErrorCode::NotFound);
        CHECK(r.error->details["path"] == "/clips/0");
        CHECK(h.errorOf("sequence.addEdit", Json{{"time", tc(1)}, {"tracks", Json::array({"V7"})}}) == "NotFound");
        CHECK(h.errorOf("clip.rename", Json{{"clips", Json::array({Json{{"track", "V1"}, {"time", tc(5)}}})},
                                            {"name", "x"}}) == "NotFound");
        CHECK(h.errorOf("marker.remove", Json{{"index", 3}}) == "NotFound");
        CHECK(h.errorOf("sequence.addEdit", Json{{"time", tc(1)}, {"sequence", 999}}) == "NotFound");
    }
    SECTION("times use the sequence's timecode") {
        CHECK(h.errorOf("sequence.addEdit", Json{{"time", "00:00:00:30"}}) == "InvalidParams"); // 25 fps
        CHECK(h.errorOf("sequence.addEdit", Json{{"time", "soon"}}) == "InvalidParams");
        CHECK(h.run(Json{{"command", "sequence.addEdit"}, {"params", Json{{"time", f(1).value}}}, {"dryRun", true}}).ok());
    }
    SECTION("edits the model refuses are EditRejected") {
        CHECK(h.errorOf("timeline.rippleDelete", Json{{"gap", Json{{"track", "V1"}, {"time", tc(1)}}}}) ==
              "EditRejected");
        CHECK(h.errorOf("sequence.extract") == "EditRejected"); // no In/Out marks
    }
    CHECK(h.session.undoCount() == 0);
    CHECK(h.session.revision() == 0);
}

TEST_CASE("A batch is one undo step, all or nothing", "[command][engine][batch]") {
    CommandHarness h(R"(
        V1 tgt |Aaaa..Bbbb|
        A1 tgt |Aaaa..Bbbb|
    )");
    const Json cutA{{"command", "sequence.addEdit"}, {"params", Json{{"time", tc(2)}}}};
    const Json renameB{{"command", "clip.rename"},
                       {"params", Json{{"clips", Json::array({Json{{"track", "V1"}, {"time", tc(7)}}})},
                                       {"name", "Second"}}}};
    const Json summary{{"command", "sequence.summary"}};

    SECTION("commands see the state the previous ones produced") {
        const auto r = h.run(Json{{"commands", Json::array({cutA, renameB, summary})}});
        REQUIRE(r.ok());
        CHECK(r.committed);
        CHECK(h.session.undoCount() == 1);
        CHECK(h.session.undoLabel() == "Batch (3 commands)");
        CHECK_SESSION(h, R"(
            V1 |AaA2..Bbbb|
            A1 |AaA2..Bbbb|
        )");
        REQUIRE(r.steps.size() == 3);
        CHECK(r.steps[0].command == "sequence.addEdit");
        CHECK(r.steps[0].createdIds.size() == 2);
        CHECK(r.createdIds == r.steps[0].createdIds);
        CHECK(r.steps[1].data.is_null());
        // The query step already sees the cut.
        CHECK(r.steps[2].data["tracks"][0]["clips"].size() == 3);
        CHECK(command::toJson(r)["steps"].size() == 3);

        REQUIRE(h.session.undo());
        CHECK_SESSION(h, R"(
            V1 |Aaaa..Bbbb|
            A1 |Aaaa..Bbbb|
        )");
    }
    SECTION("a failing command rolls the whole batch back") {
        Json broken = renameB;
        broken["params"]["clips"] = Json::array({9999});
        const auto r = h.run(Json{{"commands", Json::array({cutA, broken})}, {"label", "Cut and rename"}});
        REQUIRE_FALSE(r.ok());
        CHECK(r.error->code == command::ErrorCode::NotFound);
        CHECK(r.error->details["index"] == 1);
        CHECK(h.session.undoCount() == 0);
        CHECK(h.session.revision() == 0);
    }
    SECTION("a batch can be previewed") {
        const auto r = h.run(Json{{"commands", Json::array({cutA, renameB})}, {"dryRun", true}});
        REQUIRE(r.ok());
        REQUIRE(r.preview);
        CHECK_TIMELINE(*r.preview, h.tl.seq, R"(
            V1 |AaA2..Bbbb|
            A1 |AaA2..Bbbb|
        )");
        CHECK(h.session.undoCount() == 0);
    }
    SECTION("the label names the undo step") {
        REQUIRE(h.run(Json{{"commands", Json::array({cutA})}, {"label", "Cut A"}}).ok());
        CHECK(h.session.undoLabel() == "Cut A");
    }
    SECTION("envelope fields belong to the batch") {
        Json inner = cutA;
        inner["dryRun"] = true;
        const auto r = h.run(Json{{"commands", Json::array({cutA, inner})}});
        REQUIRE_FALSE(r.ok());
        CHECK(r.error->code == command::ErrorCode::InvalidRequest);
        CHECK(r.error->details["index"] == 1);
        CHECK(codeOf(h.run(Json{{"commands", Json::array()}})) == "InvalidRequest");
    }
    SECTION("a batch may unlock a track, edit it and lock it again") {
        CommandHarness locked(R"(
            V1 lock |Aaaaaaaa|
        )");
        // Edits leave locked tracks alone.
        CHECK_FALSE(locked.ok("sequence.addEdit", Json{{"time", tc(2)}, {"tracks", Json::array({"V1"})}}).committed);
        const auto r = locked.run(Json{
            {"commands",
             Json::array({Json{{"command", "track.setState"}, {"params", Json{{"track", "V1"}, {"locked", false}}}},
                          Json{{"command", "sequence.addEdit"},
                               {"params", Json{{"time", tc(2)}, {"tracks", Json::array({"V1"})}}}},
                          Json{{"command", "track.setState"}, {"params", Json{{"track", "V1"}, {"locked", true}}}}})}});
        INFO(command::toJson(r).dump());
        REQUIRE(r.ok());
        CHECK_SESSION(locked, "V1 |AaA2aaaa|");
        CHECK(locked.sequence(locked.state()).videoTracks[0]->locked);
    }
}

TEST_CASE("Ids continue past undone edits", "[command][engine]") {
    CommandHarness h(R"(
        V1 tgt |Aaaaaaaa|
    )");
    const auto first = h.ok("sequence.addEdit", Json{{"time", tc(2)}});
    REQUIRE(first.createdIds.size() == 1);
    REQUIRE(h.session.undo());
    const auto second = h.ok("sequence.addEdit", Json{{"time", tc(4)}});
    REQUIRE(second.createdIds.size() == 1);
    CHECK(second.createdIds[0] > first.createdIds[0]);
}

TEST_CASE("Results that would change a locked track or break an invariant are refused", "[command][engine]") {
    // Built-in commands go through the model, which never does either; these rogue commands bypass it.
    CommandRegistry registry;
    REQUIRE(registry.add(CommandSpec{
        "test.editLockedTrack", 1, "Rogue", "Changes a locked track behind the model's back.",
        command::Capability::EditSequence, command::schema::object({}), Json::object(), {},
        [](command::ExecutionContext& c) {
            const SequenceId seq = c.sequenceId();
            const TrackId v1 = c.sequence().videoTracks[0]->id;
            edit::TrackStateChange unlock;
            unlock.locked = false;
            edit::TrackStateChange lock;
            lock.locked = true;
            ProjectState s = c.take(edit::setTrackState(c.state(), seq, v1, unlock)).state;
            s = c.take(edit::addEdit(s, seq, f(2), std::vector<TrackId>{v1})).state;
            command::CommandOutput out;
            out.state = c.take(edit::setTrackState(s, seq, v1, lock)).state;
            return out;
        }}));
    REQUIRE(registry.add(CommandSpec{
        "test.breakMarks", 1, "Rogue", "Puts the Out mark before the In mark.", command::Capability::EditSequence,
        command::schema::object({}), Json::object(), {}, [](command::ExecutionContext& c) {
            auto seq = std::make_shared<Sequence>(c.sequence());
            seq->markIn = f(6);
            seq->markOut = f(2);
            command::CommandOutput out;
            out.state = c.state().withSequence(std::move(seq));
            return out;
        }}));
    CommandHarness h(R"(
        V1 lock |Aaaaaaaa|
    )",
                     {}, registry);
    CHECK(h.errorOf("test.editLockedTrack") == "LockedTrack");
    CHECK(h.errorOf("test.breakMarks") == "InvariantViolation");
    CHECK(h.session.undoCount() == 0);
    CHECK_SESSION(h, "V1 |Aaaaaaaa|");
}
