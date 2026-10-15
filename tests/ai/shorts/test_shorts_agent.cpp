// The editing assistant makes shorts (AI-9): "make 2 shorts from this podcast about the product" with a scripted model.
// shorts.create and reframe.createSequence are EditProject commands the agent may use without that capability because
// they only add sequences; verification checks that nothing else changed and that at most maxNewSequences were made;
// other project commands stay out of reach; the staged batch applies as one undo step.

#include "../AgentTestUtil.h"
#include "ShortsTestUtil.h"

#include <wabcut/ai/agent/EditAgent.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/reframe/ReframeCommands.h>
#include <wabcut/ai/shorts/ShortsCommands.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::agent;
using namespace wabcut::ai::shorts::test;
using ai::test::ScriptedModel;

namespace {

struct Studio {
    Studio() : podcast(makePodcast()), tl(avPicture(static_cast<int>(std::ceil(podcast.seconds * 25.0))), {{'A', static_cast<std::int64_t>(std::ceil(podcast.seconds * 25.0)) + 10}}) {
        tl.linkMatching();
        transcripts = ai::edit::test::provider(tl, {{'A', ai::edit::test::transcript("en", podcast.words)}});
        services = std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
            std::make_shared<ai::edit::TranscriptServices>(transcripts),
            std::make_shared<reframe::ReframeServices>(std::make_shared<vision::MapSubjectProvider>())});
        session = std::make_unique<EditSession>(tl.state);
    }
    AgentOptions options() const {
        AgentOptions o;
        o.services = services;
        return o;
    }
    AgentRequest request(std::string prompt) const {
        AgentRequest r;
        r.prompt = std::move(prompt);
        r.sequence = tl.seq;
        r.base = std::make_shared<const ProjectState>(session->stateForSave());
        r.baseRevision = session->revision();
        r.constraints = parseConstraints(r.prompt, *r.base, tl.seq);
        return r;
    }
    Podcast podcast;
    Timeline tl;
    std::shared_ptr<ai::edit::MapTranscriptProvider> transcripts;
    std::shared_ptr<command::ServiceBundle> services;
    std::unique_ptr<EditSession> session;
};

Json twoShorts(const Podcast& p) {
    return Json::array({Json{{"start", p.topics[3].first - 0.2}, {"end", p.topics[3].first + 30.0}, {"title", "Ship the prototype"}},
                        Json{{"start", p.topics[3].first + 31.0}, {"end", p.topics[3].second}, {"title", "What users told us"}}});
}

} // namespace

TEST_CASE("The assistant finds and makes shorts, verified and applied as one undo step", "[ai][shorts][agent]") {
    Studio studio;
    ScriptedModel model;
    model.tool("shorts_find", Json{{"query", "product"}, {"count", 2}})
        .tool("shorts_create", Json{{"shorts", twoShorts(studio.podcast)}, {"captions", true}})
        .text("I made 2 shorts about the product.");
    EditAgent agent(studio.options());
    // Without EditProject the two additive commands are offered, other project commands are not.
    const auto tools = agent.tools();
    const auto offered = [&](const std::string& name) {
        return std::ranges::any_of(tools, [&](const ToolSpec& t) { return t.name == name; });
    };
    CHECK(offered("shorts_create"));
    CHECK(offered("reframe_createSequence"));
    CHECK(offered("shorts_find"));
    CHECK_FALSE(offered("sequence_addTrack"));
    CHECK_FALSE(offered("reframe_rerun"));

    const auto before = studio.session->snapshot();
    const AgentOutcome out = agent.run(model, studio.request("make 2 shorts from this podcast about the product"), {}, {});
    INFO(out.error << "\n" << out.verification.failuresForModel());
    REQUIRE(out.status == AgentStatus::Staged);
    CHECK(out.verification.passed());
    REQUIRE(out.verification.item("others") != nullptr);
    CHECK(out.verification.item("others")->passed);
    REQUIRE(out.verification.item("newSequences") != nullptr);
    CHECK(out.verification.item("newSequences")->passed);
    REQUIRE(out.staged);
    CHECK(out.staged->sequences.size() == 3);
    CHECK(studio.session->snapshot() == before); // staged only

    const ApplyResult applied = applyStaged(*studio.session, out, ApplyOptions{nullptr, {command::CapabilitySet::of({command::Capability::Read, command::Capability::EditSequence})}, defaultAdditiveProjectCommands(), studio.services});
    INFO(applied.message);
    REQUIRE(applied.status == ApplyResult::Status::Applied);
    CHECK(studio.session->snapshot()->sequences.size() == 3);
    REQUIRE(studio.session->undo());
    CHECK(studio.session->snapshot()->sequences.size() == 1);

    // Without the allow-list the same batch is refused at apply.
    ApplyOptions strict{nullptr, command::CapabilitySet::of({command::Capability::Read, command::Capability::EditSequence}), {}, studio.services};
    CHECK(applyStaged(*studio.session, out, strict).status == ApplyResult::Status::Refused);
}

TEST_CASE("The assistant may not exceed the new-sequence cap or use other project commands", "[ai][shorts][agent]") {
    Studio studio;
    AgentOptions options = studio.options();
    options.maxNewSequences = 1;
    ScriptedModel model;
    model.tool("shorts_create", Json{{"shorts", twoShorts(studio.podcast)}})
        .tool("sequence_addTrack", Json{{"kind", "video"}})
        .text("Done.");
    EditAgent agent(options);
    const AgentOutcome out = agent.run(model, studio.request("make 2 shorts"), {}, {});
    // Both calls came back to the model as permission errors; nothing was staged.
    REQUIRE(model.requests().size() >= 2);
    const std::string results = conversationToJson(model.requests().back().messages).dump();
    CHECK(results.find("At most 1 new sequences per request") != std::string::npos);
    CHECK(results.find("may not use sequence.addTrack") != std::string::npos);
    CHECK(out.batch.commands.empty());

    // The verification check on its own: a removed or changed other sequence fails it.
    ProjectState two = studio.tl.state;
    {
        auto added = wabcut::edit::addSequence(two, wabcut::edit::SequenceSettings{});
        REQUIRE(added);
        two = added->state;
    }
    const SequenceId other = two.sequences.back()->id;
    ProjectState removed = two;
    std::erase_if(removed.sequences, [&](const SequencePtr& s) { return s->id == other; });
    const VerificationReport report = verify(two, removed, studio.tl.seq, Constraints{.expectChanges = false}, 0);
    REQUIRE(report.item("others") != nullptr);
    CHECK_FALSE(report.item("others")->passed);
    CHECK_FALSE(report.passed());
}
