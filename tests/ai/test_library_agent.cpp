// C.11: the editing assistant may organise the library - make, rename and move bins (sequences too), label and
// describe media, make and edit subclips - without the EditProject capability, because none of it deletes anything
// or changes a sequence's content. Deleting a bin stays out of its reach.

#include "AgentTestUtil.h"
#include "CommandHarness.h"

#include <wabcut/ai/agent/EditAgent.h>
#include <wabcut/ai/agent/Verification.h>
#include <wabcut/model/EditSession.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::agent;
using ai::test::ScriptedModel;
using command::Json;
using wabcut::test::Timeline;

TEST_CASE("The assistant organises the library: bins, labels, subclips; never deleting (C.11)", "[ai][agent][library]") {
    Timeline tl(R"(
        V1 tgt |AaaaaaaaaaBbbbbbbbbb|
        A1 tgt |AaaaaaaaaaBbbbbbbbbb|
    )");
    EditSession session(tl.state);
    EditAgent agent{AgentOptions{}};
    const auto tools = agent.tools();
    const auto offered = [&](const std::string& name) {
        return std::ranges::any_of(tools, [&](const ToolSpec& t) { return t.name == name; });
    };
    for (const char* name : {"bin_create", "bin_rename", "bin_move", "media_setAttributes", "media_makeSubclip", "media_setSubclip"}) {
        INFO(name);
        CHECK(offered(name));
    }
    CHECK_FALSE(offered("bin_remove"));
    CHECK_FALSE(offered("multicam_create")); // other project commands stay out of reach

    // The first id the request hands out is the base project's next id: the new bin.
    const std::uint64_t bin = tl.state.ids.next;
    const MediaId a = tl.media('A');
    ScriptedModel model;
    model.tool("bin_create", Json{{"name", "Interviews"}})
        .tool("media_setAttributes", Json{{"media", Json::array({a.value})}, {"colorIndex", 3}, {"comment", "Host"}})
        .tool("media_makeSubclip", Json{{"media", a.value}, {"in", "00:00:00:02"}, {"out", "00:00:00:06"}, {"name", "Hello"},
                                        {"bin", bin}})
        .tool("bin_move", Json{{"target", bin}, {"media", Json::array({a.value})}, {"sequences", Json::array({tl.seq.value})}})
        .text("The interview is in its own bin, labelled, with the greeting as a subclip.");
    AgentRequest request;
    request.prompt = "Put the interview in a bin and make a subclip of the greeting";
    request.sequence = tl.seq;
    request.base = std::make_shared<const ProjectState>(session.stateForSave());
    request.baseRevision = session.revision();
    const auto before = session.snapshot();
    const AgentOutcome out = agent.run(model, request, {}, {});
    INFO(out.error << "\n" << out.verification.failuresForModel());
    REQUIRE(out.status == AgentStatus::Staged);
    CHECK(out.verification.passed());
    REQUIRE(out.batch.commands.size() == 4);
    CHECK(session.snapshot() == before); // staged only

    const ApplyResult applied = applyStaged(session, out);
    INFO(applied.message);
    REQUIRE(applied.status == ApplyResult::Status::Applied);
    const ProjectState& now = *session.snapshot();
    REQUIRE(now.findBin(BinId{bin}));
    CHECK(now.findMedia(a)->bin == BinId{bin});
    CHECK(now.findMedia(a)->colorIndex == 3);
    CHECK(now.findSequence(tl.seq)->bin == BinId{bin});
    CHECK(now.media.size() == before->media.size() + 1); // the subclip
    REQUIRE(session.undo()); // one undo step
    CHECK(*session.snapshot() == *before);

    // Verification still refuses removed media: what an allowed library request can never do.
    ProjectState removed = *before;
    removed.media.erase(std::remove_if(removed.media.begin(), removed.media.end(),
                                       [&](const MediaPtr& m) { return m->name == "Z"; }),
                        removed.media.end());
    const VerificationReport report = verify(*before, removed, tl.seq, Constraints{}, 1);
    CHECK_FALSE(report.passed());
}
