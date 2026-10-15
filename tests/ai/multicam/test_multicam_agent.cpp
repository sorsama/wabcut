// AI-13 in the editing assistant: "switch cameras to whoever is talking" reads the speakers (analysis.speakers, an
// allowed read) and stages multicam.autoSwitch on the request's multicam clip; "edit this 3-camera podcast" stages
// workflow.podcast (an additive project command: a new multicam source and episode, nothing else changed). Both are
// verified, cost nothing (local) and apply as one undo step.

#include "../AgentTestUtil.h"
#include "PodcastTestUtil.h"

#include <wabcut/ai/agent/EditAgent.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/model/EditSession.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::agent;
using namespace wabcut::ai::multicam::test;
using ai::test::ScriptedModel;

namespace {

AgentRequest requestOn(const EditSession& session, SequenceId sequence, std::string prompt) {
    AgentRequest r;
    r.prompt = std::move(prompt);
    r.sequence = sequence;
    r.base = std::make_shared<const ProjectState>(session.stateForSave());
    r.baseRevision = session.revision();
    r.constraints = parseConstraints(r.prompt, *r.base, sequence);
    return r;
}

} // namespace

TEST_CASE("The assistant switches cameras to whoever speaks: staged, verified, one undo step", "[ai][multicam][agent]") {
    Studio s;
    EditSession session(s.state);
    AgentOptions options;
    options.services = s.services();
    EditAgent agent(options);
    const auto tools = agent.tools();
    const auto offered = [&](const std::string& name) { return std::ranges::any_of(tools, [&](const ToolSpec& t) { return t.name == name; }); };
    CHECK(offered("multicam_autoSwitch"));
    CHECK(offered("analysis_speakers"));
    CHECK(offered("workflow_podcast"));
    CHECK_FALSE(offered("multicam_create")); // other project commands stay out of reach

    ScriptedModel model;
    model.tool("analysis_speakers", Json{{"clip", s.picture.value}})
        .tool("multicam_autoSwitch", Json{{"clip", s.picture.value}, {"preset", "interview"}})
        .text("Cam A follows the host, Cam B the guest, and the wide shot covers the crosstalk.");
    const auto before = session.snapshot();
    const AgentOutcome out = agent.run(model, requestOn(session, s.tl.seq, "switch cameras to whoever is talking"), {}, {});
    INFO(out.error << "\n" << out.verification.failuresForModel());
    REQUIRE(out.status == AgentStatus::Staged);
    CHECK(out.verification.passed());
    REQUIRE(out.batch.commands.size() == 1); // the read is not part of the edit
    CHECK(out.batch.commands[0].command == "multicam.autoSwitch");
    CHECK(session.snapshot() == before);
    // The speakers reached the model.
    const std::string conversation = conversationToJson(model.requests().back().messages).dump();
    CHECK(conversation.find("Mic 2") != std::string::npos);

    ApplyOptions apply;
    apply.services = s.services();
    const ApplyResult applied = applyStaged(session, out, apply);
    INFO(applied.message);
    REQUIRE(applied.status == ApplyResult::Status::Applied);
    CHECK(session.snapshot()->findSequence(s.tl.seq)->videoTracks.front()->clips.size() > 3);
    REQUIRE(session.undo());
    CHECK(*session.snapshot() == *before);
}

TEST_CASE("The assistant edits a 3-camera podcast with the workflow: a new episode, nothing else changed", "[ai][multicam][agent]") {
    PodcastStudio p;
    EditSession session(p.studio.state);
    AgentOptions options;
    options.services = p.services;
    EditAgent agent(options);

    Json media = Json::array();
    for (const char m : {'A', 'B', 'C', 'D', 'E'}) {
        media.push_back(p.studio.tl.media(m).value);
    }
    ScriptedModel model;
    model.tool("workflow_podcast", Json{{"media", media}, {"name", "Episode 12"}})
        .tool("workflow_podcast", Json{{"media", media}, {"name", "Episode 12"}, {"minChapterSeconds", 10}, {"run", true}})
        .text("Your episode is laid out: synced, enhanced, at -16 LUFS, cleaned up, switched, captioned and chaptered.");
    const auto before = session.snapshot();
    const AgentOutcome out = agent.run(model, requestOn(session, p.studio.tl.seq, "edit this 3-camera podcast"), {}, {});
    INFO(out.error << "\n" << out.verification.failuresForModel());
    for (const AgentStep& step : out.steps) {
        UNSCOPED_INFO(step.title << ": " << step.detail);
    }
    REQUIRE(out.status == AgentStatus::Staged);
    CHECK(out.verification.passed());
    REQUIRE(out.staged);
    CHECK(out.staged->sequences.size() == before->sequences.size() + 2); // the multicam source and the episode
    // The plan reached the model with its cost.
    const std::string conversation = conversationToJson(model.requests()[1].messages).dump();
    CHECK(conversation.find("local, free") != std::string::npos);

    ApplyOptions apply;
    apply.services = p.services;
    const ApplyResult applied = applyStaged(session, out, apply);
    INFO(applied.message);
    REQUIRE(applied.status == ApplyResult::Status::Applied);
    CHECK(session.snapshot()->sequences.size() == before->sequences.size() + 2);
    REQUIRE(session.undo());
    CHECK(*session.snapshot() == *before);

    // Without the allow-list entry the workflow is out of reach.
    AgentOptions narrow = options;
    narrow.additiveProjectCommands = {};
    const auto narrowTools = EditAgent(narrow).tools();
    CHECK_FALSE(std::ranges::any_of(narrowTools, [](const ToolSpec& t) { return t.name == "workflow_podcast"; }));
}
