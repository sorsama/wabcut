// The editing assistant (AI-5) with a scripted model: the tool loop, staging isolation, verification feedback,
// the guarded apply (one undo step, rebase or refuse), limits, cancellation, the redacted transcript, Refine and
// the quick actions. The end-to-end "remove silences" run produces the same timeline as the AI-3 command itself.

#include "AgentTestUtil.h"
#include "CommandHarness.h"
#include "EditTestUtil.h"
#include "FakeTransport.h"

#include <wabcut/ai/Redact.h>
#include <wabcut/ai/agent/Describe.h>
#include <wabcut/ai/agent/EditAgent.h>
#include <wabcut/ai/agent/QuickActions.h>
#include <wabcut/ai/analysis/AnalysisCommands.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/audio/edit/AudioAnalysis.h>
#include <wabcut/audio/edit/EssentialSound.h>
#include <wabcut/command/Engine.h>
#include <wabcut/command/ExecutionContext.h>

#include <catch2/catch_test_macros.hpp>

#include <set>

#include <filesystem>
#include <fstream>
#include <thread>

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::agent;
using namespace wabcut::ai::edit;
using namespace wabcut::ai::edit::test;
using namespace wabcut::ai::test;
using command::Json;

namespace {

// 86 frames of media A on V1 and A1 (linked), and what is said in it.
constexpr std::string_view kPicture = R"(
    V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
)";

std::shared_ptr<const Transcript> interview() {
    return transcript("en", {{"So", 0, 3, 0},       // 0
                             {"um,", 4, 7, 0},      // 1  filler
                             {"welcome", 8, 13, 0}, // 2
                             {"to", 13, 15, 0},     // 3
                             {"the", 15, 17, 0},    // 4
                             {"show.", 17, 22, 0},  // 5  then a 1.6 s pause
                             {"Thanks", 62, 67, 1}, // 6
                             {"for", 67, 69, 1},    // 7
                             {"having", 69, 74, 1}, // 8
                             {"me.", 74, 80, 1}});  // 9
}

Timeline linked() {
    Timeline tl(kPicture);
    tl.linkMatching();
    return tl;
}

/// The live project: a session the agent must not touch until Accept.
struct Live {
    Live() : tl(linked()), transcripts(provider(tl, {{'A', interview()}})), services(std::make_shared<TranscriptServices>(transcripts)),
             session(tl.state) {}

    AgentOptions options() const {
        AgentOptions o;
        o.services = services;
        return o;
    }
    ApplyOptions applyOptions() const {
        ApplyOptions o;
        o.services = services;
        return o;
    }
    AgentRequest request(std::string prompt, Constraints constraints = {}) const {
        AgentRequest r;
        r.prompt = std::move(prompt);
        r.sequence = tl.seq;
        r.base = std::make_shared<const ProjectState>(session.stateForSave());
        r.baseRevision = session.revision();
        r.constraints = constraints;
        return r;
    }
    /// Runs a command on the live session, as the user would.
    void userEdit(std::string_view name, Json params) {
        command::CommandEngine engine(session, editingCommands(), command::EnginePolicy{command::CapabilitySet::all(), 16, services});
        command::CommandRequest req;
        req.command = std::string(name);
        req.params = std::move(params);
        const auto r = engine.execute(req);
        INFO(command::toJson(r).dump());
        REQUIRE(r.ok());
    }
    std::string text() const {
        const auto st = SequenceTranscript::build(*session.snapshot(), tl.seq, *transcripts);
        return st.size() ? st.textOf(0, st.size() - 1) : std::string();
    }

    Timeline tl;
    std::shared_ptr<MapTranscriptProvider> transcripts;
    std::shared_ptr<TranscriptServices> services;
    EditSession session;
};

Constraints quick(const Live& live, std::string_view id, std::optional<double> value = std::nullopt) {
    auto r = quickActionRequest(id, value, *live.session.snapshot(), live.tl.seq);
    REQUIRE(r);
    return r->constraints;
}

std::vector<std::string> titles(const AgentOutcome& o) {
    std::vector<std::string> out;
    for (const AgentStep& s : o.steps) {
        out.push_back(s.title);
    }
    return out;
}

} // namespace

TEST_CASE("Agent end to end: remove silences stages the AI-3 edit and applies it as one undo step", "[ai][agent]") {
    Live live;
    const auto qa = quickActionRequest("removeSilences", 1.0, *live.session.snapshot(), live.tl.seq);
    REQUIRE(qa);
    ScriptedModel model;
    model.tool("transcript_review", Json{{"silences", 1.0}})
        .tool("transcript_cleanup", Json{{"fillers", false}, {"silences", 1.0}})
        .text("I removed the 1.6 s pause after \"show.\".");

    const ProjectSnapshot before = live.session.snapshot();
    EditAgent agent(live.options());
    std::vector<AgentEvent> events;
    const AgentOutcome out = agent.run(model, live.request(qa->prompt, qa->constraints),
                                       [&](const AgentEvent& e) { events.push_back(e); }, {});

    INFO(out.error << "\n" << out.verification.failuresForModel());
    REQUIRE(out.status == AgentStatus::Staged);
    CHECK(out.verified);
    CHECK(out.verification.passed());
    CHECK(out.answer == "I removed the 1.6 s pause after \"show.\".");
    CHECK(titles(out) == std::vector<std::string>{"Find filler words and pauses", "Remove pauses longer than 1 s",
                                                  "All checks passed"});
    CHECK(out.steps[1].kind == AgentStep::Kind::Edit);
    CHECK(out.steps[1].detail.find("on V1, A1") != std::string::npos);
    REQUIRE(out.batch.commands.size() == 1); // the read is not part of the batch
    CHECK(out.batch.commands[0].command == "transcript.cleanup");
    CHECK(out.batch.label == undoLabelFor(qa->prompt));
    CHECK(out.modelCalls == 3);
    CHECK(out.toolCalls == 2);
    CHECK(out.cost > 0);

    // Staging isolation: the live session is exactly as it was.
    CHECK(live.session.snapshot() == before);
    CHECK(live.session.revision() == out.baseRevision);
    CHECK(live.session.undoCount() == 0);
    CHECK(live.text() == "So um, welcome to the show. Thanks for having me.");

    // The panel saw the steps stream by.
    bool started = false;
    bool finished = false;
    for (const AgentEvent& e : events) {
        started = started || (e.kind == AgentEvent::Kind::StepStarted && e.step.tool == "transcript.cleanup");
        finished = finished || (e.kind == AgentEvent::Kind::StepFinished && e.step.tool == "transcript.cleanup");
    }
    CHECK(started);
    CHECK(finished);

    // Accept: one undo step, the same timeline the AI-3 command makes on its own.
    const ApplyResult applied = applyStaged(live.session, out, live.applyOptions());
    INFO(applied.message);
    REQUIRE(applied.status == ApplyResult::Status::Applied);
    CHECK(live.session.undoCount() == 1);
    CHECK(live.session.undoLabel() == undoLabelFor(qa->prompt));
    CHECK(diff(*out.staged, *live.session.snapshot()).empty());

    CommandHarness direct(linked(), command::EnginePolicy{command::CapabilitySet::all(), 16, live.services}, editingCommands());
    direct.ok("transcript.cleanup", Json{{"fillers", false}, {"silences", 1.0}});
    CHECK(render(*live.session.snapshot(), live.tl.seq) == render(*direct.state(), direct.tl.seq));
    CHECK(live.text() == "So um, welcome to the show. Thanks for having me.");
    const auto st = SequenceTranscript::build(*live.session.snapshot(), live.tl.seq, *live.transcripts);
    CHECK(st[6].timeline.start - st[5].timeline.end() < Ticks::fromSeconds(1));

    live.session.undo();
    CHECK(*live.session.snapshot() == *before);
}

TEST_CASE("Agent: tool errors go back to the model, which fixes its call", "[ai][agent]") {
    Live live;
    ScriptedModel model;
    model.tools({{"timeline_explode", Json::object()}, {"marker_add", Json{{"name", "Intro"}}}})
        .tool("marker_add", Json{{"time", tc(5)}, {"name", "Intro"}})
        .text("Added the marker.");
    EditAgent agent(live.options());
    const AgentOutcome out = agent.run(model, live.request("Add a marker called Intro at frame 5"), {}, {});

    REQUIRE(out.status == AgentStatus::Staged);
    const auto requests = model.requests();
    REQUIRE(requests.size() == 3);
    const auto results = toolResultsOf(requests[1]);
    REQUIRE(results.size() == 2); // both calls of the parallel turn are answered
    CHECK(results[0].isError);
    CHECK(Json::parse(results[0].content)["error"]["code"] == "UnknownCommand");
    CHECK(results[1].isError);
    CHECK(Json::parse(results[1].content)["error"]["code"] == "InvalidParams");
    const auto fixed = toolResultsOf(requests[2]);
    REQUIRE(fixed.size() == 1);
    CHECK_FALSE(fixed[0].isError);
    CHECK(Json::parse(fixed[0].content)["changed"] == "1 marker added");

    CHECK(out.steps[0].kind == AgentStep::Kind::Error);
    CHECK(out.steps[1].kind == AgentStep::Kind::Error);
    CHECK(out.steps[2].title == "Add marker “Intro” at " + tc(5));
    CHECK(out.batch.commands.size() == 1);
    CHECK(out.staged->findSequence(live.tl.seq)->markers.size() == 1);
}

TEST_CASE("Agent: verification catches a wrong plan, feeds it back once and never reports success", "[ai][agent]") {
    Live live;
    const Constraints target = parseConstraints("Make a 2-second highlight", *live.session.snapshot(), live.tl.seq);
    REQUIRE(target.targetDuration == Ticks::fromSeconds(2));

    SECTION("the model claims success twice: the outcome says the checks failed") {
        ScriptedModel model;
        model.tool("transcript_deleteRanges", Json{{"words", Json::array({Json::array({0, 1})})}})
            .text("Done: the highlight is 2 seconds long.")
            .text("It is fine as it is.");
        EditAgent agent(live.options());
        const AgentOutcome out = agent.run(model, live.request("Make a 2-second highlight", target), {}, {});

        CHECK(out.status == AgentStatus::VerificationFailed);
        CHECK(out.answer == "It is fine as it is.");
        const VerificationItem* duration = out.verification.item("duration");
        REQUIRE(duration);
        CHECK_FALSE(duration->passed);
        CHECK(duration->detail.find("too long") != std::string::npos);
        const auto requests = model.requests();
        REQUIRE(requests.size() == 3);
        const std::string feedback = requests[2].messages.back().text();
        CHECK(feedback.find("automatic checks of the staged edit failed") != std::string::npos);
        CHECK(feedback.find("Length is 0:02") != std::string::npos);
        CHECK(out.canApply()); // the user may still take it, knowing the checks failed
        CHECK(out.steps.back().title == "Some checks failed");
    }
    SECTION("the model fixes the edit after the feedback") {
        ScriptedModel model;
        model.tool("transcript_deleteRanges", Json{{"words", Json::array({Json::array({0, 1})})}})
            .text("Done.")
            .tool("transcript_keepRanges", Json{{"words", Json::array({Json::array({0, 3}), Json::array({4, 7})})}})
            .text("Kept the welcome and the thanks: about 1.5 s.");
        EditAgent agent(live.options());
        const AgentOutcome out = agent.run(model, live.request("Make a 2-second highlight", target), {}, {});
        INFO(out.verification.failuresForModel());
        CHECK(out.status == AgentStatus::Staged);
        CHECK(out.verification.item("duration")->passed);
        CHECK(out.batch.commands.size() == 2);
    }
}

TEST_CASE("Agent apply: a stale preview is rebased when it still makes the same changes, refused otherwise", "[ai][agent]") {
    ScriptedModel script;
    auto cleanup = [](ScriptedModel& m) {
        m.tool("transcript_cleanup", Json{{"fillers", false}, {"silences", 1.0}}).text("Removed the pause.");
    };

    SECTION("an unrelated edit meanwhile: rebased, one more undo step") {
        Live live;
        ScriptedModel model;
        cleanup(model);
        EditAgent agent(live.options());
        const AgentOutcome out = agent.run(model, live.request("Remove pauses over a second"), {}, {});
        REQUIRE(out.status == AgentStatus::Staged);

        live.userEdit("marker.add", Json{{"time", tc(2)}, {"name", "Mine"}});
        const ApplyResult r = applyStaged(live.session, out, live.applyOptions());
        INFO(r.message);
        CHECK(r.status == ApplyResult::Status::Rebased);
        CHECK(r.verification.passed());
        CHECK(live.session.undoCount() == 2);
        CHECK(live.session.snapshot()->findSequence(live.tl.seq)->markers.size() == 1);
        const auto st = SequenceTranscript::build(*live.session.snapshot(), live.tl.seq, *live.transcripts);
        CHECK(st[6].timeline.start - st[5].timeline.end() < Ticks::fromSeconds(1));
    }
    SECTION("the user already shortened the pause: the batch would do something else, so it is refused") {
        Live live;
        ScriptedModel model;
        cleanup(model);
        EditAgent agent(live.options());
        const AgentOutcome out = agent.run(model, live.request("Remove pauses over a second"), {}, {});
        REQUIRE(out.status == AgentStatus::Staged);

        live.userEdit("timeline.removeSilences", Json{{"ranges", Json::array({Json{{"start", tc(30)}, {"end", tc(50)}}})}});
        const ProjectSnapshot userState = live.session.snapshot();
        const ApplyResult r = applyStaged(live.session, out, live.applyOptions());
        CHECK(r.status == ApplyResult::Status::Refused);
        CHECK(r.message.find("Run the request again") != std::string::npos);
        CHECK(live.session.snapshot() == userState); // nothing applied
        CHECK(live.session.undoCount() == 1);
    }
    SECTION("nothing staged: nothing to apply") {
        Live live;
        ScriptedModel model;
        model.text("There is nothing to change.");
        EditAgent agent(live.options());
        const AgentOutcome out = agent.run(model, live.request("Is there a pause?"), {}, {});
        CHECK(out.status == AgentStatus::NoChanges);
        CHECK_FALSE(out.canApply());
        CHECK(applyStaged(live.session, out, live.applyOptions()).status == ApplyResult::Status::Refused);
    }
}

TEST_CASE("Agent limits: cost and token budgets, the spend cap, steps, cancellation and the timeout", "[ai][agent]") {
    Live live;
    SECTION("a cost budget too small for the first call") {
        AgentOptions o = live.options();
        o.limits.maxCost = 1'000; // US$0.001
        ScriptedModel model;
        model.text("never sent");
        const AgentOutcome out = EditAgent(o).run(model, live.request("Remove silences"), {}, {});
        CHECK(out.status == AgentStatus::BudgetExceeded);
        CHECK(out.error.find("budget") != std::string::npos);
        CHECK(model.requests().empty());
    }
    SECTION("the budget runs out after an expensive call") {
        AgentOptions o = live.options();
        o.limits.maxCost = 1'000'000; // US$1
        Usage expensive;
        expensive.inputTokens = 1'000'000; // well over US$1 at any listed price
        ScriptedModel model;
        model.tool("transcript_cleanup", Json{{"fillers", false}, {"silences", 1.0}}, expensive).text("never sent");
        const AgentOutcome out = EditAgent(o).run(model, live.request("Remove silences"), {}, {});
        CHECK(out.status == AgentStatus::BudgetExceeded);
        CHECK(model.requests().size() == 1);
        CHECK(out.cost >= 1'000'000);
        CHECK(out.hasChanges());
        CHECK(out.verified); // what was staged is still checked
        CHECK(out.canApply());
    }
    SECTION("the token budget") {
        AgentOptions o = live.options();
        o.limits.maxTokens = 1'000;
        ScriptedModel model;
        const AgentOutcome out = EditAgent(o).run(model, live.request("Remove silences"), {}, {});
        CHECK(out.status == AgentStatus::BudgetExceeded);
        CHECK(out.error.find("token") != std::string::npos);
    }
    SECTION("the monthly spend cap of the guarded model") {
        ScriptedModel model;
        model.error(AiErrorCode::SpendCapExceeded, "the monthly cap for anthropic is reached");
        const AgentOutcome out = EditAgent(live.options()).run(model, live.request("Remove silences"), {}, {});
        CHECK(out.status == AgentStatus::BudgetExceeded);
        CHECK(out.error == "the monthly cap for anthropic is reached");
    }
    SECTION("a provider failure") {
        ScriptedModel model;
        model.error(AiErrorCode::Unauthorized, "invalid x-api-key");
        const AgentOutcome out = EditAgent(live.options()).run(model, live.request("Remove silences"), {}, {});
        CHECK(out.status == AgentStatus::Failed);
        CHECK(out.error == "Unauthorized: invalid x-api-key");
        CHECK_FALSE(out.canApply());
    }
    SECTION("the step limit") {
        AgentOptions o = live.options();
        o.limits.maxSteps = 2;
        ScriptedModel model;
        model.tool("sequence_summary").tool("sequence_summary").tool("sequence_summary");
        const AgentOutcome out = EditAgent(o).run(model, live.request("Look around"), {}, {});
        CHECK(out.status == AgentStatus::StepLimit);
        CHECK(model.requests().size() == 2);
    }
    SECTION("cancel while the model answers: the edit it asked for is not staged") {
        std::stop_source stop;
        ScriptedModel model;
        model.custom([&](const ChatRequest&, const ChatEventHandler&, std::stop_token) {
            stop.request_stop(); // the user presses Stop
            ChatResponse r;
            r.message.role = Role::Assistant;
            r.message.parts.push_back(ToolCall{"call_1", "transcript_cleanup", Json{{"silences", 1.0}}, "", ""});
            r.stopReason = StopReason::ToolUse;
            return std::expected<ChatResponse, AiError>(r);
        });
        const ProjectSnapshot before = live.session.snapshot();
        const AgentOutcome out = EditAgent(live.options()).run(model, live.request("Remove silences"), {}, stop.get_token());
        CHECK(out.status == AgentStatus::Cancelled);
        CHECK_FALSE(out.hasChanges());
        CHECK_FALSE(out.canApply());
        CHECK(live.session.snapshot() == before);
    }
    SECTION("the timeout stops a model call that hangs") {
        AgentOptions o = live.options();
        o.limits.timeout = std::chrono::milliseconds(150);
        ScriptedModel model;
        model.custom([](const ChatRequest&, const ChatEventHandler&, std::stop_token stop) {
            for (int i = 0; i < 2000 && !stop.stop_requested(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return std::expected<ChatResponse, AiError>(std::unexpected(AiError(AiErrorCode::Cancelled, "cancelled")));
        });
        const auto t0 = std::chrono::steady_clock::now();
        const AgentOutcome out = EditAgent(o).run(model, live.request("Remove silences"), {}, {});
        CHECK(out.status == AgentStatus::TimedOut);
        CHECK(out.error == "Stopped at the time limit (1 s).");
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
    }
}

TEST_CASE("Agent prompt: stable system prompt per revision, cacheable, with the catalog's tools", "[ai][agent]") {
    Live live;
    ScriptedModel model;
    model.tool("sequence_summary").text("It is one 3.4 s interview clip.");
    EditAgent agent(live.options());
    const AgentOutcome out = agent.run(model, live.request("What is in this sequence?"), {}, {});
    CHECK(out.status == AgentStatus::NoChanges);

    const auto requests = model.requests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].system == requests[1].system);
    CHECK(requests[0].cachePrompt);
    CHECK(requests[0].tools == requests[1].tools);
    CHECK(requests[0].system == agent.systemPrompt(*live.session.snapshot(), live.session.revision(), live.tl.seq));
    CHECK(requests[0].system.find("Sequence to edit:") != std::string::npos);
    CHECK(requests[0].system.find("\"A\" 00:00:00:00-00:00:03:11") != std::string::npos);

    // Read + EditSequence: the transcript tools are there, project-structure tools are not.
    bool cleanup = false;
    bool addTrack = false;
    for (const ToolSpec& t : requests[0].tools) {
        cleanup = cleanup || t.name == "transcript_cleanup";
        addTrack = addTrack || t.name == "sequence_addTrack";
    }
    CHECK(cleanup);
    CHECK_FALSE(addTrack);

    // The user message restates what will be checked.
    CHECK(requests[0].messages[0].text().find("Checked automatically when you finish:") != std::string::npos);

    // Without the EditProject permission a structural call is refused, and the model hears why.
    ScriptedModel structural;
    structural.tool("sequence_addTrack", Json{{"kind", "audio"}}).text("I could not add a track.");
    const AgentOutcome refused = agent.run(structural, live.request("Add an audio track"), {}, {});
    CHECK(Json::parse(toolResultsOf(structural.requests()[1])[0].content)["error"]["code"] == "PermissionDenied");
    CHECK(refused.status == AgentStatus::NoChanges);
}

TEST_CASE("Agent transcript: a redacted record of the run is kept locally", "[ai][agent]") {
    Live live;
    const std::string secret = "sk-ant-test-0123456789abcdef";
    Redactor::global().addSecret(secret);
    const auto dir = uniqueTempPath("wabcut-agent-transcripts");
    AgentOptions o = live.options();
    o.transcriptDir = dir;
    ScriptedModel model;
    model.tool("marker_add", Json{{"time", tc(5)}, {"name", "Intro"}}).text("Done.");
    const AgentOutcome out = EditAgent(o).run(model, live.request("Add a marker; my key is " + secret), {}, {});
    Redactor::global().removeSecret(secret);

    REQUIRE(out.status == AgentStatus::Staged);
    const std::string dumped = out.transcript.dump();
    CHECK(dumped.find(secret) == std::string::npos);
    CHECK(dumped.find(std::string(kRedacted)) != std::string::npos);
    CHECK(out.transcript["status"] == "Staged");
    CHECK(out.transcript["batch"]["commands"].size() == 1);
    CHECK(out.transcript["messages"].size() == out.conversation.size());
    CHECK(out.transcript["verification"]["passed"] == true);
    REQUIRE(std::filesystem::exists(out.transcriptFile));
    {
        std::ifstream file(out.transcriptFile);
        CHECK(Json::parse(file) == out.transcript);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("Agent transcript: a key the model echoes into tool parameters is redacted too", "[ai][agent][security]") {
    Live live;
    const std::string secret = "sk-ant-echo-0123456789abcdef";
    Redactor::global().addSecret(secret);
    ScriptedModel model;
    model.tool("marker_add", Json{{"time", tc(5)}, {"name", "key " + secret}}).text("Done with " + secret);
    const AgentOutcome out = EditAgent(live.options()).run(model, live.request("Add a marker named after my key " + secret), {}, {});
    Redactor::global().removeSecret(secret);
    REQUIRE(out.status == AgentStatus::Staged);
    CHECK(out.batch.commands[0].params["name"] == "key " + secret); // the edit itself is what the model asked for
    const std::string dumped = out.transcript.dump();
    CHECK(dumped.find(secret) == std::string::npos);
    CHECK(out.transcript["batch"]["commands"][0]["params"]["name"] == "key " + std::string(kRedacted));
}

TEST_CASE("Agent refine: continues the staged edit and applies everything as one undo step", "[ai][agent]") {
    Live live;
    ScriptedModel model;
    model.tool("transcript_cleanup", Json{{"fillers", false}, {"silences", 1.0}})
        .text("Removed the pause.")
        .tool("marker_add", Json{{"time", tc(0)}, {"name", "Start"}})
        .text("Added a marker at the start.");
    EditAgent agent(live.options());
    const auto first = std::make_shared<const AgentOutcome>(agent.run(model, live.request("Remove pauses over a second"), {}, {}));
    REQUIRE(first->status == AgentStatus::Staged);

    AgentRequest refine = live.request("Also add a marker at the start");
    refine.previous = first;
    const AgentOutcome second = agent.run(model, refine, {}, {});
    REQUIRE(second.status == AgentStatus::Staged);
    CHECK(second.batch.commands.size() == 2);
    CHECK(second.batch.label == first->batch.label);
    CHECK(second.baseRevision == first->baseRevision);
    CHECK(model.requests()[2].messages.front() == first->conversation.front()); // the conversation continues
    CHECK(second.steps[2].title == "Refine: Also add a marker at the start");

    REQUIRE(applyStaged(live.session, second, live.applyOptions()).status == ApplyResult::Status::Applied);
    CHECK(live.session.undoCount() == 1);
    CHECK(live.session.snapshot()->findSequence(live.tl.seq)->markers.size() == 1);
}

TEST_CASE("Agent quick actions: prompts, constraints and what is shown", "[ai][agent]") {
    std::vector<std::string> ids;
    for (const QuickAction& a : visibleQuickActions({})) {
        ids.push_back(a.id);
    }
    CHECK(ids == std::vector<std::string>{"removeSilences", "removeFillers", "highlight", "topicMarkers", "captions"});
    CHECK(visibleQuickActions(QuickActionContext{true, true}).size() == 6); // beat detection would show "Cut to the beat"

    Live live;
    CHECK(quick(live, "highlight", 30.0).targetDuration == Ticks::fromSeconds(30));
    CHECK(quick(live, "highlight", 1.0).targetDuration == Ticks::fromSeconds(5)); // clamped to the minimum
    CHECK_FALSE(quick(live, "topicMarkers").allowClipEdits);
    CHECK_FALSE(quick(live, "captions").allowClipEdits);
    CHECK_FALSE(quick(live, "removeSilences").expectChanges);
    CHECK_FALSE(quickActionRequest("nope", {}, *live.session.snapshot(), live.tl.seq));
    const auto silences = quickActionRequest("removeSilences", 0.5, *live.session.snapshot(), live.tl.seq);
    CHECK(silences->prompt.find("longer than 0.5 seconds") != std::string::npos);

    Constraints earlier;
    earlier.targetDuration = Ticks::fromSeconds(30);
    earlier.allowClipEdits = true;
    CHECK(mergeConstraints(earlier, Constraints{}).targetDuration == Ticks::fromSeconds(30));
    Constraints parsed;
    parsed.maxDuration = Ticks::fromSeconds(20);
    const Constraints merged = mergeConstraints(earlier, parsed);
    CHECK_FALSE(merged.targetDuration);
    CHECK(merged.maxDuration == Ticks::fromSeconds(20));

    // Topic markers: a model that cuts instead of adding markers fails the check.
    ScriptedModel model;
    model.tool("transcript_cleanup", Json{{"fillers", true}}).text("Added markers.");
    const auto qa = quickActionRequest("topicMarkers", {}, *live.session.snapshot(), live.tl.seq);
    AgentOptions o = live.options();
    o.limits.verificationRetries = 0;
    const AgentOutcome out = EditAgent(o).run(model, live.request(qa->prompt, qa->constraints), {}, {});
    CHECK(out.status == AgentStatus::VerificationFailed);
    CHECK_FALSE(out.verification.item("onlyMarkers")->passed);
}

TEST_CASE("Agent steps read as edits", "[ai][agent]") {
    const auto& registry = editingCommands();
    const Timeline tl(kPicture);
    auto describe = [&](std::string_view name, Json params) {
        return describeCall(*registry.find(name), params, tl.state, tl.seq);
    };
    CHECK(describe("transcript.cleanup", Json{{"fillers", true}, {"silences", 0.8}}) ==
          "Remove filler words and pauses longer than 0.8 s");
    CHECK(describe("transcript.cleanup", Json{{"fillers", true}}) == "Remove filler words");
    CHECK(describe("transcript.deleteRanges", Json{{"words", Json::array({Json::array({3, 5}), Json::array({9, 9})})}}) ==
          "Delete words 3–5, 9");
    CHECK(describe("transcript.deleteRanges", Json{{"gaps", Json::array({5})}}) == "Delete 1 pause");
    CHECK(describe("timeline.removeSilences",
                   Json{{"ranges", Json::array({Json{{"start", f(25).value}, {"end", "00:00:02:00"}}})}}) ==
          "Remove 1 silent range (00:00:01:00–00:00:02:00)");
    CHECK(describe("transcript.text", Json{{"from", "00:00:01:00"}}) == "Read the transcript (00:00:01:00–end)");
    CHECK(describe("clip.rename", Json{{"clip", 7}, {"name", "Take 2"}}) == "Rename Clip (clip 7, name Take 2)");
    CHECK(undoLabelFor("Remove every pause in the speech longer than one second from this sequence") ==
          "AI: Remove every pause in the speech longer than one…");
}

TEST_CASE("Agent quick action: cut to the beat with C.5 beat detection; the music track stays as it is", "[ai][agent]") {
    // Like the rhythm suite's snap: A ends at 9, B starts at 14 and ends at 26; the music on A2 beats every 10 frames.
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaa.....B4bbbbbbbbbb........|
        A1 tgt |Aaaaaaaaa.....B4bbbbbbbbbb........|
        A2     |Mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm|
    )");
    tl.linkMatching();
    auto beats = std::make_shared<analysis::MapAnalysisProvider>();
    analysis::BeatAnalysis music;
    music.bpm = 150.0;
    music.confidence = 0.95f;
    music.tracker = "dsp-1";
    music.duration = f(1000);
    for (int k = 0; k < 100; ++k) {
        music.beats.push_back(f(10 * k));
        if (k % 4 == 0) {
            music.downbeats.push_back(f(10 * k));
        }
    }
    beats->addBeats(tl.media('M'), music);
    EditSession live(tl.state);
    AgentOptions options;
    options.services = std::make_shared<command::ServiceBundle>(
        std::vector<std::shared_ptr<command::CommandServices>>{std::make_shared<analysis::AnalysisServices>(beats)});

    const auto qa = quickActionRequest("beatCut", {}, tl.state, tl.seq, "A2");
    REQUIRE(qa);
    CHECK(qa->prompt.find("analysis_detectBeats") != std::string::npos);
    CHECK(qa->constraints.onlyTracks == std::vector<TrackId>{tl.track("V1"), tl.track("A1")});

    ScriptedModel model;
    model.tool("analysis_detectBeats", Json{{"clip", Json{{"track", "A2"}, {"time", tc(0)}}}, {"markers", "downbeats"}, {"grid", true}})
        .tool("edit_snapToBeats", Json{{"clips", Json::array({Json{{"track", "V1"}, {"time", tc(0)}}, Json{{"track", "V1"}, {"time", tc(14)}}})},
                                       {"tolerance", tc(4)}})
        .text("The cuts now land on the beat.");
    AgentRequest request;
    request.prompt = qa->prompt;
    request.constraints = qa->constraints;
    request.sequence = tl.seq;
    request.base = std::make_shared<const ProjectState>(live.stateForSave());
    request.baseRevision = live.revision();
    const AgentOutcome out = EditAgent(options).run(model, request, {}, {});
    INFO(out.error << out.verification.failuresForModel());
    REQUIRE(out.status == AgentStatus::Staged);
    CHECK(out.steps[0].title == "Find the beats of the music on A2");
    CHECK(out.steps[1].title == "Snap the edges of 2 clips to the beat");
    CHECK(render(*out.staged, tl.seq) == normalize(R"(
        V1 |AaaaaaaaaaBbbbbbbbbbbbbbbbbbbb....|
        A1 |AaaaaaaaaaBbbbbbbbbbbbbbbbbbbb....|
        A2 |Mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm|
    )"));
    CHECK(out.staged->findSequence(tl.seq)->beatGrid.has_value());
    CHECK(out.verification.item("scope")->passed); // A2 (the music) unchanged

    ApplyOptions apply;
    apply.services = options.services;
    REQUIRE(applyStaged(live, out, apply).status == ApplyResult::Status::Applied);
    CHECK(live.undoCount() == 1);
}

TEST_CASE("Agent: wrongly typed tool arguments are error results, never a crash (review wave 3, P1-2)", "[ai][agent][security]") {
    Live live;
    const auto liveBefore = live.session.snapshot();
    const std::uint64_t revision = live.session.revision();

    // The review's probe: describing {"fillers": "yes"} threw (value<bool>() on a string) before the engine validated it.
    const command::CommandSpec* cleanup = editingCommands().find("transcript.cleanup");
    REQUIRE(cleanup != nullptr);
    std::string described;
    CHECK_NOTHROW(described = describeCall(*cleanup, Json{{"fillers", "yes"}}, *liveBefore, live.tl.seq));
    CHECK(described == "Clean up the transcript");

    // Through a run: arguments that fail the schema, and one that passes it but used to break the description (a
    // track given as a number), are answered as tool errors; the model fixes its call and the run goes on.
    ScriptedModel model;
    model.tools({{"transcript_cleanup", Json{{"fillers", "yes"}}},
                 {"clip_find", Json{{"name", 12}}},
                 {"analysis_detectBeats", Json{{"clip", Json{{"track", 7}, {"time", 3}}}}}})
        .tool("transcript_cleanup", Json{{"fillers", true}})
        .text("Removed the filler words.");
    EditAgent agent(live.options());
    const AgentOutcome out = agent.run(model, live.request("Remove the filler words"), {}, {});

    const auto requests = model.requests();
    REQUIRE(requests.size() == 3);
    const auto results = toolResultsOf(requests[1]);
    REQUIRE(results.size() == 3);
    for (const ToolResult& r : results) {
        CHECK(r.isError);
    }
    const Json first = Json::parse(results[0].content);
    CHECK(first["error"]["code"] == "InvalidParams");
    CHECK(first["error"]["message"].get<std::string>().find("/fillers") != std::string::npos);
    CHECK(Json::parse(results[1].content)["error"]["code"] == "InvalidParams");
    CHECK(Json::parse(results[2].content)["error"]["code"] != "Internal"); // described, then refused by the command
    REQUIRE(out.steps.size() >= 4);
    CHECK(out.steps[0].title == cleanup->title);
    CHECK(out.steps[0].kind == AgentStep::Kind::Error);
    CHECK(out.steps[2].title.find("7") != std::string::npos);
    CHECK_FALSE(toolResultsOf(requests[2]).front().isError);
    CHECK(out.batch.commands.size() == 1);

    // Nothing reached the live project.
    CHECK(live.session.revision() == revision);
    CHECK(live.session.snapshot() == liveBefore);
}

TEST_CASE("The assistant gets the faders and the loudness read (A.3)", "[ai][agent][faders]") {
    // "Mix the music 6 dB under the dialogue and hit -14 LUFS": track.setVolume / sequence.setMasterGain stage as
    // edits, audio.loudness checks the staged mix; other analysis commands stay out of reach.
    Live live;
    const auto names = [](const EditAgent& agent) {
        std::set<std::string> out;
        for (const ToolSpec& t : agent.tools()) {
            out.insert(t.name);
        }
        return out;
    };
    const EditAgent agent(live.options());
    const auto tools = names(agent);
    for (const char* name : {"track_setVolume", "track_setPan", "sequence_setMasterGain", "audio_addKeyframe",
                             "audio_moveKeyframe", "audio_normalize", "audio_loudness"}) {
        INFO(name);
        CHECK(tools.contains(name));
    }
    CHECK_FALSE(tools.contains("transcript_transcribe"));
    AgentOptions without = live.options();
    without.analysisCommands.clear();
    CHECK_FALSE(names(EditAgent(without)).contains("audio_loudness"));

    // A loudness read is run (here it has no audio services, so it reports that), not refused as out of reach.
    ScriptedModel model;
    model.tool("audio_loudness", Json::object()).tool("track_setVolume", Json{{"track", "A1"}, {"db", -6}}).text("Done.");
    EditAgent runner(live.options());
    const AgentOutcome out = runner.run(model, live.request("Make A1 6 dB quieter and check the loudness"), {}, {});
    bool sawRead = false;
    for (const AgentStep& step : out.steps) {
        if (step.tool == "audio.loudness") {
            sawRead = true;
            CHECK(step.detail.find("may not use") == std::string::npos);
        }
    }
    CHECK(sawRead);
    REQUIRE(out.staged);
    CHECK(out.staged->findSequence(live.tl.seq)->audioTracks[0]->volume == ParamValue(-6.0));
}

namespace {

/// A 4/4 song at 120 BPM, 40 bars (80 s): intro (4 bars), verse, chorus, verse, chorus (8 bars each), outro (4), as
/// beat features that repeat with the sections (what the media analyzer would find in such a song).
class SongAnalyzer final : public audio::edit::MusicAnalyzer {
public:
    std::expected<audio::edit::MusicAnalysis, Error> analyze(const MediaItem&, int, std::stop_token, const std::function<void(double)>&) override {
        audio::edit::MusicAnalysis a;
        a.bpm = 120.0;
        a.beatConfidence = 0.9f;
        const std::string sections = "IIIIAAAAAAAABBBBBBBBAAAAAAAABBBBBBBBOOOO";
        for (int k = 0; k <= 160; ++k) {
            a.beats.push_back(Ticks{kTicksPerSecond / 2 * k});
            if (k % 4 == 0) {
                a.downbeats.push_back(a.beats.back());
            }
        }
        for (int k = 0; k < 160; ++k) {
            const char s = sections[static_cast<std::size_t>(k / 4)];
            audio::edit::BeatFeatures f;
            f.chroma[static_cast<std::size_t>((s - 'A' + 2 * ((k / 4) % 4)) % 12)] = 1.0f; // a chord per bar, cycling every 4
            f.mfcc[0] = static_cast<float>(s == 'B' ? 3.0 : s == 'A' ? 1.0 : -2.0);         // the sections' sound
            f.mfcc[1] = static_cast<float>(k % 4);                                         // the beat in the bar
            f.rms = s == 'I' || s == 'O' ? 0.05f : 0.2f;
            a.features.push_back(f);
        }
        a.duration = Ticks::fromSeconds(80);
        return a;
    }
    std::expected<Take, Error> render(const MediaItem&, int, const audio::edit::RemixPlan&, std::stop_token,
                                      const std::function<void(double)>&) override {
        return std::unexpected(Error(ErrorCode::Unsupported, "no takes here"));
    }
};

} // namespace

TEST_CASE("Make the music fit the video length: the assistant remixes the music to end with the picture (A.7)",
          "[ai][agent][remix]") {
    // V1: 60 s of picture; A2: an 80 s song. The assistant's tools include audio.remix and audio.essentialSound.
    const std::string picture = "V1 tgt |V" + std::string(1499, 'v') + "|\nA1 tgt |V" + std::string(1499, 'v') + "|\nA2 |M" +
                                std::string(1999, 'm') + "|\n";
    Timeline tl(picture, {{'V', 2000}, {'M', 2500}});
    EditSession session(tl.state);
    auto audioServices = std::make_shared<audio::edit::AudioServices>(nullptr, nullptr);
    audioServices->music = std::make_shared<SongAnalyzer>();
    AgentOptions options;
    options.services = audioServices;
    EditAgent agent(options);
    std::set<std::string> tools;
    for (const ToolSpec& t : agent.tools()) {
        tools.insert(t.name);
    }
    CHECK(tools.contains("audio_remix"));
    CHECK(tools.contains("audio_essentialSound"));
    // Project edits stay out of reach (A.8 audit): the rendered take and Enhance Speech's takes add or change media.
    CHECK_FALSE(tools.contains("audio_remixRender"));
    CHECK_FALSE(tools.contains("audio_enhanceSpeech"));
    CHECK_FALSE(tools.contains("audio_separateStems"));
    const ClipId music = clipAt(tl.state, tl.seq, "A2@0");
    ScriptedModel model;
    model.tool("audio_remix", Json{{"clip", music.value}, {"fit", "video"}}).text("The music now ends with the video.");
    AgentRequest request;
    request.prompt = "make the music fit the video length";
    request.sequence = tl.seq;
    request.base = std::make_shared<const ProjectState>(session.stateForSave());
    request.baseRevision = session.revision();
    const AgentOutcome out = agent.run(model, request, [](const AgentEvent&) {}, {});
    INFO(out.error << "\n" << out.verification.failuresForModel());
    REQUIRE(out.status == AgentStatus::Staged);
    CHECK(out.verified);
    REQUIRE(out.staged);
    const Track& a2 = *out.staged->findSequence(tl.seq)->audioTracks[1];
    REQUIRE(a2.clips.size() >= 2);
    CHECK(std::fabs(a2.clips.back().end().toSeconds() - 60.0) <= 0.5); // within a beat of the picture's end
    CHECK(a2.transitions.size() == a2.clips.size() - 1);
    // The live project is untouched until the edit is accepted.
    CHECK(session.snapshot()->findSequence(tl.seq)->audioTracks[1]->clips.size() == 1);
}

namespace {

/// A.9: every clip's second channel is silent (an interview recorded with one microphone on the left only).
class LeftOnlyChannels final : public audio::edit::ChannelAnalyzer {
public:
    std::expected<std::vector<audio::edit::ChannelLevel>, Error> measure(const ProjectState&, const MediaItem&, int, TimeRange,
                                                                         command::ExecutionControl*) override {
        ++calls;
        return std::vector<audio::edit::ChannelLevel>{{-2.0, -20.0}, {-144.0, -144.0}};
    }
    int calls = 0;
};

constexpr std::string_view kInterview = R"(
    V1 tgt |Aaaaaaaaaaaa|
    A1 tgt |Aaaaaaaaaaaa|
    A2     |............|
)";

AgentOutcome runAgent(EditAgent& agent, ScriptedModel& model, Timeline& tl, EditSession& session, const char* prompt) {
    AgentRequest request;
    request.prompt = prompt;
    request.sequence = tl.seq;
    request.base = std::make_shared<const ProjectState>(session.stateForSave());
    request.baseRevision = session.revision();
    return agent.run(model, request, [](const AgentEvent&) {}, {});
}

} // namespace

TEST_CASE("Split the two mics into separate tracks: the assistant stages a split to mono (A.9)", "[ai][agent][channels]") {
    Timeline tl(kInterview);
    tl.linkMatching();
    EditSession session(tl.state);
    EditAgent agent(AgentOptions{});
    std::set<std::string> tools;
    for (const ToolSpec& t : agent.tools()) {
        tools.insert(t.name);
    }
    CHECK(tools.contains("audio_splitToMono"));
    CHECK(tools.contains("audio_setChannels"));
    CHECK(tools.contains("audio_channels"));
    CHECK_FALSE(tools.contains("audio_setMediaChannels")); // it changes media items: a project edit
    const ClipId a1 = clipAt(tl.state, tl.seq, "A1@0");
    ScriptedModel model;
    model.tool("audio_splitToMono", Json{{"clip", a1.value}}).text("Each microphone is on its own track now.");
    const AgentOutcome out = runAgent(agent, model, tl, session, "split the two mics into separate tracks");
    INFO(out.error << "\n" << out.verification.failuresForModel());
    REQUIRE(out.status == AgentStatus::Staged);
    REQUIRE(out.staged);
    const Sequence& staged = *out.staged->findSequence(tl.seq);
    REQUIRE(staged.audioTracks[1]->clips.size() == 1);
    const Clip& left = staged.findClip(a1)->clip();
    const Clip& right = staged.audioTracks[1]->clips[0];
    REQUIRE(left.channelMap());
    REQUIRE(right.channelMap());
    CHECK(*left.channelMap() == AudioChannelMap::mono(0));
    CHECK(*right.channelMap() == AudioChannelMap::mono(1));
    CHECK(left.linkGroup == right.linkGroup);
    // Staged only: the live project is unchanged until the edit is accepted.
    CHECK(session.snapshot()->findSequence(tl.seq)->audioTracks[1]->clips.empty());
}

TEST_CASE("Fix the left-only audio: the assistant measures the channels, then fills from the left (A.9)", "[ai][agent][channels]") {
    Timeline tl(kInterview);
    EditSession session(tl.state);
    auto services = std::make_shared<audio::edit::AudioServices>(nullptr, nullptr);
    auto analyzer = std::make_shared<LeftOnlyChannels>();
    services->channels = analyzer;
    AgentOptions options;
    options.services = services;
    EditAgent agent(options);
    std::set<std::string> tools;
    for (const ToolSpec& t : agent.tools()) {
        tools.insert(t.name);
    }
    CHECK(tools.contains("audio_channelLevels")); // a MediaAnalysis read the assistant may run
    CHECK(tools.contains("audio_fillChannel"));
    const ClipId a1 = clipAt(tl.state, tl.seq, "A1@0");
    ScriptedModel model;
    model.tool("audio_channelLevels", Json{{"clips", Json::array({a1.value})}})
        .tool("audio_fillChannel", Json{{"clips", Json::array({a1.value})}, {"from", "left"}})
        .text("The right channel was silent; both sides now play the left microphone.");
    const AgentOutcome out = runAgent(agent, model, tl, session, "fix the left-only audio");
    INFO(out.error << "\n" << out.verification.failuresForModel());
    REQUIRE(out.status == AgentStatus::Staged);
    REQUIRE(out.staged);
    CHECK(analyzer->calls >= 1);
    const Clip& fixed = out.staged->findSequence(tl.seq)->findClip(a1)->clip();
    REQUIRE(fixed.channelMap());
    CHECK(*fixed.channelMap() == AudioChannelMap::fill(0));
    CHECK(session.snapshot()->findSequence(tl.seq)->findClip(a1)->clip().channelMap() == nullptr);
}

TEST_CASE("The assistant cannot record: the microphone opens only on the user's action in the app (A.8)", "[ai][agent][voiceover]") {
    // record.start / record.stop / record.toggle are the app's (keymap) actions, not commands: no tool, no MCP method.
    // Arming a track and choosing among recorded takes are ordinary sequence edits.
    for (const char* name : {"record.start", "record.stop", "record.toggle"}) {
        INFO(name);
        CHECK_FALSE(ai::edit::editingCommands().knows(name));
    }
    EditAgent agent(AgentOptions{});
    std::set<std::string> tools;
    for (const ToolSpec& t : agent.tools()) {
        tools.insert(t.name);
    }
    for (const char* name : {"record_start", "record_stop", "record_toggle"}) {
        INFO(name);
        CHECK_FALSE(tools.contains(name));
    }
    CHECK(tools.contains("track_setRecordArm"));
    CHECK(tools.contains("record_takes"));
    CHECK(tools.contains("record_useTake"));
}

TEST_CASE("The assistant lists audio plugins and adds plugin effects, but never scans (A.6)", "[ai][agent][plugins]") {
    // "Put TDR Nova on the dialogue": plugins.list finds it, clip.addEffect / audio.addEffect ("plugin") put it on.
    // plugins.scan runs plugin code (MediaAnalysis, not one of the assistant's named analysis reads): out of reach.
    Live live;
    std::set<std::string> tools;
    for (const ToolSpec& t : EditAgent(live.options()).tools()) {
        tools.insert(t.name);
    }
    CHECK(tools.contains("plugins_list"));
    CHECK(tools.contains("clip_addEffect"));
    CHECK(tools.contains("audio_addEffect"));
    CHECK_FALSE(tools.contains("plugins_scan"));
}
