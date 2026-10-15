// The JSON wire format of requests, batches and results.

#include <wabcut/command/Command.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut::command;

namespace {

std::string requestError(const Json& json) {
    const auto r = requestFromJson(json);
    return r ? std::string() : r.error().message;
}

} // namespace

TEST_CASE("Requests parse with their envelope", "[command][wire]") {
    const auto r = requestFromJson(Json{{"command", "clip.rename"},
                                        {"version", 1},
                                        {"params", Json{{"name", "x"}}},
                                        {"expectedRevision", 7},
                                        {"idempotencyKey", "k"},
                                        {"dryRun", true}});
    REQUIRE(r);
    CHECK(r->command == "clip.rename");
    CHECK(r->version == 1);
    CHECK(r->params["name"] == "x");
    CHECK(r->expectedRevision == 7u);
    CHECK(r->idempotencyKey == "k");
    CHECK(r->dryRun);
    CHECK(requestFromJson(toJson(*r)).value().params == r->params);

    const auto minimal = requestFromJson(Json{{"command", "project.summary"}});
    REQUIRE(minimal);
    CHECK(minimal->params == Json::object());
    CHECK_FALSE(minimal->version);
    CHECK_FALSE(minimal->dryRun);
}

TEST_CASE("Malformed requests are InvalidRequest", "[command][wire]") {
    CHECK(requestError(Json::array()) == "a request must be a JSON object");
    CHECK(requestError(Json{{"params", Json::object()}}) == "a request needs a \"command\" name");
    CHECK(requestError(Json{{"command", "a.b"}, {"params", Json::array()}}) == "\"params\" must be a JSON object");
    CHECK(requestError(Json{{"command", "a.b"}, {"version", 0}}) == "\"version\" must be a positive integer");
    CHECK(requestError(Json{{"command", "a.b"}, {"expectedRevision", -1}}) ==
          "\"expectedRevision\" must be a non-negative integer");
    CHECK(requestError(Json{{"command", "a.b"}, {"idempotencyKey", ""}}) == "\"idempotencyKey\" must be a non-empty string");
    CHECK(requestError(Json{{"command", "a.b"}, {"dryRun", "yes"}}) == "\"dryRun\" must be true or false");
    CHECK(requestError(Json{{"command", "a.b"}, {"param", Json::object()}}) == "unknown field \"param\"");
    CHECK(requestFromJson(Json{{"command", "a.b"}, {"bogus", 1}}).error().code == ErrorCode::InvalidRequest);
}

TEST_CASE("Batches parse; envelope fields stay on the batch", "[command][wire]") {
    const Json doc{{"commands", Json::array({Json{{"command", "a.b"}}, Json{{"command", "c.d"}, {"params", Json{{"x", 1}}}}})},
                   {"label", "Two"},
                   {"dryRun", true}};
    REQUIRE(isBatchJson(doc));
    const auto b = batchFromJson(doc);
    REQUIRE(b);
    CHECK(b->commands.size() == 2);
    CHECK(b->label == "Two");
    CHECK(b->dryRun);
    CHECK(batchFromJson(toJson(*b)).value().commands[1].params["x"] == 1);

    const auto inner = batchFromJson(Json{{"commands", Json::array({Json{{"command", "a.b"}, {"idempotencyKey", "k"}}})}});
    REQUIRE_FALSE(inner);
    CHECK(inner.error().details["index"] == 0);
    const auto bad = batchFromJson(Json{{"commands", Json::array({Json{{"command", "a.b"}}, Json{{"x", 1}}})}});
    REQUIRE_FALSE(bad);
    CHECK(bad.error().details["index"] == 1);
    CHECK_FALSE(batchFromJson(Json{{"commands", Json::object()}}));
    CHECK_FALSE(batchFromJson(Json{{"commands", Json::array()}, {"labels", "x"}}));
    CHECK_FALSE(isBatchJson(Json{{"command", "a.b"}}));
}

TEST_CASE("Results and errors serialize compactly", "[command][wire]") {
    CommandResult failed;
    failed.error = CommandError{ErrorCode::StaleRevision, "old", Json{{"current", 3}}};
    failed.revision = 3;
    const Json f = toJson(failed);
    CHECK(f["ok"] == false);
    CHECK(f["error"]["code"] == "StaleRevision");
    CHECK(f["error"]["details"]["current"] == 3);
    CHECK_FALSE(f.contains("changes"));

    CommandResult done;
    done.revision = 4;
    done.committed = true;
    done.createdIds = {11, 12};
    wabcut::SequenceChange change;
    change.sequence = wabcut::SequenceId{2};
    change.removedClips = {wabcut::ClipId{5}};
    change.roles = wabcut::PropertyRole::Timing;
    change.marksChanged = true;
    done.changes.sequences.push_back(change);
    const Json d = toJson(done);
    CHECK(d["ok"] == true);
    CHECK(d["revision"] == 4);
    CHECK(d["committed"] == true);
    CHECK(d["createdIds"] == Json::array({11, 12}));
    CHECK(d["changes"]["sequences"][0]["sequence"] == 2);
    CHECK(d["changes"]["sequences"][0]["removedClips"] == Json::array({5}));
    CHECK(d["changes"]["sequences"][0]["roles"] == Json::array({"timing"}));
    CHECK(d["changes"]["sequences"][0]["changed"] == Json::array({"marks"}));
    CHECK_FALSE(d.contains("error"));
    CHECK_FALSE(d.contains("replayed"));

    CommandResult query;
    query.data = Json{{"tracks", Json::array()}};
    const Json q = toJson(query);
    CHECK(q["data"]["tracks"].empty());
    CHECK_FALSE(q.contains("changes")); // empty parts are left out
    CHECK_FALSE(q.contains("createdIds"));
    CHECK_FALSE(q.contains("warnings"));
    CHECK_FALSE(q.contains("steps"));
}
