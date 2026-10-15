// The editing assistant with the AI-12 tools: "make Thai and Japanese subtitles and a Thai voice-over" runs
// captions.translate (a staged edit, verified like any other) and dub.create on the new Thai track, which only proposes
// the voice-over with its cost (it changes nothing, so it is not part of the staged batch): no speech is generated and
// nothing is spent until the user approves it in the Dub dialog. Applied, the subtitles are one undo step.

#include "AgentTestUtil.h"
#include "GlobalTestUtil.h"
#include "TimelineFixture.h"

#include <wabcut/ai/agent/EditAgent.h>
#include <wabcut/ai/dub/DubCommands.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/translate/TranslateCommands.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/FinishingOps.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::agent;
using namespace wabcut::ai::global::test;
using ai::test::ScriptedModel;

namespace {

Ticks sec(double s) { return Ticks{static_cast<std::int64_t>(s * static_cast<double>(kTicksPerSecond))}; }

/// A host that only proposes, like the app's: it records what it was asked.
class RecordingProposals final : public dub::DubServices {
public:
    bool proposes() const override { return true; }
    command::CommandOutput dub(const dub::DubPlan& plan, command::ExecutionContext& context) override {
        plans.push_back(plan);
        command::CommandOutput out;
        out.state = context.state();
        out.data = dub::planToJson(plan);
        out.data["status"] = "approval-required";
        return out;
    }
    std::vector<dub::DubPlan> plans;
};

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

TEST_CASE("The assistant makes Thai and Japanese subtitles, staged and verified, and proposes the Thai voice-over with its cost",
          "[ai][global][agent]") {
    wabcut::test::Timeline tl("V1 |A" + std::string(149, 'a') + "|\nA1 dialogue |A" + std::string(149, 'a') + "|");
    tl.linkMatching();
    CaptionTrack english;
    english.name = "English";
    english.language = "en";
    english.cues = {CaptionCue{sec(0.5), sec(2), "Welcome back.", {}}, CaptionCue{sec(3), sec(2), "Let's cook.", {}}};
    auto made = wabcut::edit::setCaptionTrack(tl.state, tl.seq, english);
    REQUIRE(made);
    EditSession session(made->state);

    auto local = [](std::string_view, std::string_view target) -> std::expected<std::unique_ptr<ITranslator>, AiError> {
        if (target == "ja") {
            return std::make_unique<DictionaryTranslator>(std::map<std::string, std::string>{{"Welcome back.", "おかえりなさい。"}, {"Let's cook.", "料理しよう。"}});
        }
        return std::make_unique<DictionaryTranslator>(std::map<std::string, std::string>{{"Welcome back.", "ยินดีต้อนรับกลับครับ"}, {"Let's cook.", "มาทำอาหารกัน"}});
    };
    auto proposals = std::make_shared<RecordingProposals>();
    auto services = std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
        std::make_shared<translate::TranslateServices>(local, translate::TranslateServices::ChatOpener{}),
        std::make_shared<command::CaptionServices>(std::make_shared<WordListBreaker>(std::vector<std::string>{"ยินดี", "ต้อนรับ", "กลับ", "ครับ"})),
        proposals});
    AgentOptions options;
    options.services = services;
    options.additiveProjectCommands.push_back("dub.create"); // as the app allows it (it only proposes)
    EditAgent agent(options);
    const auto tools = agent.tools();
    const auto offered = [&](const std::string& name) { return std::ranges::any_of(tools, [&](const ToolSpec& t) { return t.name == name; }); };
    CHECK(offered("captions_translate"));
    CHECK(offered("dub_create"));

    ScriptedModel model;
    model.tool("captions_translate", Json{{"languages", Json::array({"th", "ja"})}, {"provider", "local"}})
        .custom([](const ChatRequest& request, const ChatEventHandler&, std::stop_token) -> std::expected<ChatResponse, AiError> {
            const Json result = lastToolResult(request);
            const Json data = result.contains("data") ? result["data"] : result;
            ChatResponse r;
            r.message.role = Role::Assistant;
            ToolCall call;
            call.id = "call_dub";
            call.name = "dub_create";
            call.arguments = Json{{"captionTrack", data["languages"][0].value("captionTrack", Json(0))}, {"language", "th"}};
            call.argumentsText = call.arguments.dump();
            r.message.parts.push_back(call);
            r.stopReason = StopReason::ToolUse;
            r.usage = ScriptedModel::defaultUsage();
            return r;
        })
        .text("I added Thai and Japanese subtitles. The Thai voice-over is ready to approve in the Dub dialog (about $0.002).");

    AgentRequest request;
    request.prompt = "make Thai and Japanese subtitles and a Thai voice-over";
    request.sequence = tl.seq;
    request.base = std::make_shared<const ProjectState>(session.stateForSave());
    request.baseRevision = session.revision();
    request.constraints = parseConstraints(request.prompt, *request.base, tl.seq);
    const auto before = session.snapshot();
    const AgentOutcome out = agent.run(model, request, {}, {});
    INFO(out.error << "\n" << out.verification.failuresForModel());
    for (const AgentStep& step : out.steps) {
        UNSCOPED_INFO(step.title << ": " << step.detail);
    }
    REQUIRE(out.status == AgentStatus::Staged);
    CHECK(out.verification.passed());
    // dub.create changed nothing (it only proposed), so the staged batch is the translation alone; the app opens the
    // prefilled Dub dialog once the Thai track exists, after the user applied it.
    REQUIRE(out.batch.commands.size() == 1);
    CHECK(out.batch.commands[0].command == "captions.translate");
    REQUIRE(out.staged);
    const Sequence& staged = *out.staged->findSequence(tl.seq);
    REQUIRE(staged.captionTracks.size() == 3);
    CHECK(staged.captionTracks[1].language == "th");
    CHECK(staged.captionTracks[2].language == "ja");
    CHECK(staged.audioTracks.size() == 1); // no speech: only proposed
    REQUIRE(proposals->plans.size() == 1);
    CHECK(proposals->plans[0].language == "th");
    CHECK(proposals->plans[0].lines.size() == 2);
    CHECK(proposals->plans[0].cost.expected > 0);
    // The model was told it needs approval and what it costs.
    const Json told = lastToolResult(model.requests().back());
    const Json data = told.contains("data") ? told["data"] : told;
    CHECK(data.value("status", std::string()) == "approval-required");
    CHECK(data["estimate"]["usd"].get<double>() > 0.0);
    CHECK(session.snapshot() == before); // staged only

    ApplyOptions apply;
    apply.services = services;
    const ApplyResult applied = applyStaged(session, out, apply);
    INFO(applied.message);
    REQUIRE(applied.status == ApplyResult::Status::Applied);
    CHECK(session.snapshot()->findSequence(tl.seq)->captionTracks.size() == 3);
    REQUIRE(session.undo());
    CHECK(session.snapshot()->findSequence(tl.seq)->captionTracks.size() == 1);
}
