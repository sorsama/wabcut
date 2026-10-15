// What a host can set on a client's engine (C.12, for extensions): undo labels that name the client, and holding back
// the commands that run an AI model.

#include "CommandHarness.h"

#include <wabcut/command/ExecutionContext.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::test;
using command::Json;

namespace {

constexpr std::string_view kPicture = R"(
    V1 tgt |Aaaa..Bbbb|
    A1 tgt |Aaaa..Bbbb|
)";

/// The built-in catalog plus a command flagged as running a model.
const command::CommandRegistry& withModelCommand() {
    static const command::CommandRegistry registry = [] {
        command::CommandRegistry r = command::builtinCommands();
        auto added = r.add(command::runsModel(
            {"test.describeScene", 1, "Describe Scene", "Runs a vision model over the sequence (a test command).",
             command::Capability::Read, Json{{"type", "object"}, {"properties", Json::object()}, {"additionalProperties", false}},
             Json::object(), {}, [](command::ExecutionContext& c) {
                 command::CommandOutput out;
                 out.state = c.state();
                 out.data = Json{{"scene", "a beach"}};
                 return out;
             }}));
        REQUIRE(added.has_value());
        return r;
    }();
    return registry;
}

} // namespace

TEST_CASE("An engine's undo label prefix names the client in the history", "[command][engine][hooks]") {
    command::EnginePolicy policy;
    policy.undoLabelPrefix = "Beat Markers";
    CommandHarness h(kPicture, policy);
    h.ok("marker.add", {{"time", "00:00:00:02"}, {"name", "Beat 1"}});
    CHECK(h.session.undoLabel() == "Beat Markers: Add Marker");
    const auto batch = h.run(Json{{"label", "Add Markers at Every Beat"},
                                  {"commands", Json::array({{{"command", "marker.add"}, {"params", {{"time", 4}}}},
                                                            {{"command", "marker.add"}, {"params", {{"time", 8}}}}})}});
    REQUIRE(batch.ok());
    CHECK(h.session.undoLabel() == "Beat Markers: Add Markers at Every Beat");
    REQUIRE(h.session.undo().has_value());
    CHECK(h.session.undoLabel() == "Beat Markers: Add Marker");

    CommandHarness plain(kPicture);
    plain.ok("marker.add", {{"time", 2}});
    CHECK(plain.session.undoLabel() == "Add Marker");
}

TEST_CASE("Commands that run a model can be held back whatever their capability", "[command][engine][hooks]") {
    const command::CommandRegistry& registry = withModelCommand();
    REQUIRE(registry.find("test.describeScene")->runsModel);
    CHECK_FALSE(registry.find("project.summary")->runsModel);

    CommandHarness allowed(Timeline(kPicture), {}, registry);
    CHECK(allowed.errorOf("test.describeScene").empty());

    command::EnginePolicy policy;
    policy.allowModelCommands = false;
    CommandHarness held(Timeline(kPicture), policy, registry);
    const auto refused = held.run(Json{{"command", "test.describeScene"}, {"params", Json::object()}});
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error->code == command::ErrorCode::PermissionDenied);
    CHECK(refused.error->details["runsModel"] == true);
    CHECK(refused.error->details["capability"] == "MediaAnalysis");
    CHECK(held.errorOf("project.summary").empty()); // everything else as before
    // In a batch, the step is named.
    const auto inBatch = held.run(Json{{"commands", Json::array({{{"command", "project.summary"}},
                                                                 {{"command", "test.describeScene"}}})}});
    REQUIRE_FALSE(inBatch.ok());
    CHECK(inBatch.error->details["index"] == 1);
}

TEST_CASE("Commands on the files their parameters name can be held back", "[command][engine][hooks]") {
    command::EnginePolicy policy;
    policy.allowFileCommands = false;
    CommandHarness held(kPicture, policy);
    for (const char* name : {"history.export", "review.exportPackage", "review.importPackage", "project.merge"}) {
        INFO(name);
        REQUIRE(command::builtinCommands().find(name)->touchesFiles);
        const auto refused = held.run(Json{{"command", name}, {"params", command::builtinCommands().find(name)->example}});
        REQUIRE_FALSE(refused.ok());
        CHECK(refused.error->code == command::ErrorCode::PermissionDenied);
        CHECK(refused.error->details["touchesFiles"] == true);
    }
    CHECK(held.errorOf("project.summary").empty());
    CHECK_FALSE(command::builtinCommands().find("sequence.exportMarkers")->touchesFiles); // returns text, no file
}
