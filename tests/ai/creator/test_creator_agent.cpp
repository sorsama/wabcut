// The editing assistant with the AI-11 tools: "make a first cut from this script and pick the cleanest takes" runs
// roughcut.fromScript (an additive project command the assistant may use without EditProject), then edits the new cut
// it made (roughcut.swapTake), staged, verified and applied as one undo step. The tools are offered; reading files is
// not; other project commands stay out of reach.

#include "../AgentTestUtil.h"
#include "CreatorTestUtil.h"

#include <wabcut/ai/agent/EditAgent.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/model/EditSession.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::agent;
using namespace wabcut::ai::roughcut::test;
using ai::test::ScriptedModel;

namespace {

struct AgentStudio {
    AgentStudio() : studio(roughcut::test::launchTakes()), session(std::make_unique<EditSession>(studio.tl.state)) {}
    AgentOptions options() const {
        AgentOptions o;
        o.services = studio.services;
        return o;
    }
    AgentRequest request(std::string prompt) const {
        AgentRequest r;
        r.prompt = std::move(prompt);
        r.sequence = studio.tl.seq;
        r.base = std::make_shared<const ProjectState>(session->stateForSave());
        r.baseRevision = session->revision();
        r.constraints = parseConstraints(r.prompt, *r.base, studio.tl.seq);
        return r;
    }
    Studio studio;
    std::unique_ptr<EditSession> session;
};

/// The last tool result the model was sent, as JSON.
Json lastToolResult(const ChatRequest& request) {
    for (auto m = request.messages.rbegin(); m != request.messages.rend(); ++m) {
        for (auto p = m->parts.rbegin(); p != m->parts.rend(); ++p) {
            if (const auto* result = std::get_if<ToolResult>(&*p)) {
                return Json::parse(result->content, nullptr, false);
            }
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("The assistant makes a first cut from a script and swaps a take in it, verified and applied as one undo step",
          "[ai][creator][agent]") {
    AgentStudio s;
    EditAgent agent(s.options());
    const auto tools = agent.tools();
    const auto offered = [&](const std::string& name) { return std::ranges::any_of(tools, [&](const ToolSpec& t) { return t.name == name; }); };
    CHECK(offered("roughcut_fromScript"));
    CHECK(offered("roughcut_swapTake"));
    CHECK(offered("takes_find"));
    CHECK(offered("takes_use"));
    CHECK(offered("autozoom_apply"));
    CHECK_FALSE(offered("sequence_addTrack"));

    ScriptedModel model;
    model.tool("roughcut_fromScript", Json{{"text", kLaunchScript}, {"media", s.studio.mediaJson()}, {"prefer", "cleanest"}})
        .custom([](const ChatRequest& request, const ChatEventHandler&, std::stop_token) -> std::expected<ChatResponse, AiError> {
            // Swap line 2 to its second take in the sequence the first call made.
            const Json made = lastToolResult(request);
            ChatResponse r;
            r.message.role = Role::Assistant;
            ToolCall call;
            call.id = "call_swap";
            call.name = "roughcut_swapTake";
            const Json data = made.contains("data") ? made["data"] : made;
            call.arguments = Json{{"sequence", data.value("sequence", Json(0))}, {"beat", 2}, {"take", 2}};
            call.argumentsText = call.arguments.dump();
            r.message.parts.push_back(call);
            r.stopReason = StopReason::ToolUse;
            r.usage = ScriptedModel::defaultUsage();
            return r;
        })
        .text("I made a first cut from your script with the cleanest takes, and used the second take of line 2.");
    const auto before = s.session->snapshot();
    const AgentOutcome out = agent.run(model, s.request("make a first cut from this script and pick the cleanest takes"), {}, {});
    INFO(out.error << "\n" << out.verification.failuresForModel());
    for (const AgentStep& step : out.steps) {
        UNSCOPED_INFO(step.title << ": " << step.detail);
    }
    REQUIRE(out.status == AgentStatus::Staged);
    CHECK(out.verification.passed());
    REQUIRE(out.batch.commands.size() == 2);
    CHECK(out.batch.commands[0].command == "roughcut.fromScript");
    CHECK(out.batch.commands[1].command == "roughcut.swapTake");
    REQUIRE(out.staged);
    CHECK(out.staged->sequences.size() == before->sequences.size() + 1);
    CHECK(s.session->snapshot() == before); // staged only

    ApplyOptions apply;
    apply.services = s.studio.services;
    const ApplyResult applied = applyStaged(*s.session, out, apply);
    INFO(applied.message);
    REQUIRE(applied.status == ApplyResult::Status::Applied);
    const SequencePtr cut = s.session->snapshot()->sequences.back();
    REQUIRE(cut->derivedFrom);
    CHECK(roughcut::recordOfSequence(*cut)->beats[1].chosen == 1);
    REQUIRE(s.session->undo()); // one step: the cut and the swap together
    CHECK(s.session->snapshot()->sequences.size() == before->sequences.size());
}

TEST_CASE("The assistant may not read script files or edit sequences it did not make", "[ai][creator][agent]") {
    AgentStudio s;
    ScriptedModel model;
    model.tool("roughcut_fromScript", Json{{"file", "C:/Users/someone/notes.md"}})
        .tool("takes_use", Json{{"sequence", 999999}, {"best", true}})
        .text("Done.");
    EditAgent agent(s.options());
    const AgentOutcome out = agent.run(model, s.request("make a first cut from notes.md"), {}, {});
    REQUIRE(model.requests().size() >= 2);
    const std::string results = conversationToJson(model.requests().back().messages).dump();
    INFO(results);
    CHECK(results.find("may not read files") != std::string::npos);
    CHECK(results.find("edits sequence") != std::string::npos);
    CHECK(out.batch.commands.empty());
}
