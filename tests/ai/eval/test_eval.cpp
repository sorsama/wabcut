// AI-15: the eval harness on deterministic models (label "eval"): every case is solvable through routing and the
// constrained envelope (the oracle passes all of them), the checks are not vacuous (a lazy model fails all of them),
// near-miss JSON from a server that does not enforce the format is repaired, and a fake llama-server answering with
// near misses still stages the edit. The real models: test_eval_live.cpp.
#include "EvalHarness.h"
#include "NetTestSupport.h"

#include <wabcut/ai/agent/ToolRouter.h>
#include <wabcut/ai_net/Adapters.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <cstdio>
#include <set>

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::eval;

namespace {

EvalSummary oracleRun(const std::string& modelId, OracleStyle style, std::optional<bool> constrained = std::nullopt) {
    const ModelSpec* spec = ModelRegistry::builtin().model(modelId);
    REQUIRE(spec != nullptr);
    std::unique_ptr<OracleModel> current;
    RunOptions options;
    options.constrained = constrained;
    return runEval(
        [&](const EvalCase& c, const Fixture& fx) -> IChatModel& {
            current = std::make_unique<OracleModel>(*spec, c, fx, style);
            return *current;
        },
        options);
}

std::string failures(const EvalSummary& s) {
    std::string out;
    for (const CaseResult& c : s.cases) {
        if (!c.success) {
            out += c.id + ": " + c.failure + "\n";
        }
    }
    return out;
}

/// A Chat Completions stream whose message content is `text` (what llama-server sends for a response format).
ai::test::ScriptedResponse contentStream(const std::string& text) {
    const Json delta{{"id", "chatcmpl-eval"}, {"object", "chat.completion.chunk"}, {"model", "qwen3-1.7b"},
                     {"choices", Json::array({{{"index", 0}, {"delta", {{"role", "assistant"}, {"content", text}}}, {"finish_reason", nullptr}}})}};
    const Json stop{{"id", "chatcmpl-eval"}, {"object", "chat.completion.chunk"}, {"model", "qwen3-1.7b"},
                    {"choices", Json::array({{{"index", 0}, {"delta", Json::object()}, {"finish_reason", "stop"}}})}};
    const Json usage{{"id", "chatcmpl-eval"}, {"object", "chat.completion.chunk"}, {"choices", Json::array()},
                     {"usage", {{"prompt_tokens", 1200}, {"completion_tokens", 24}, {"total_tokens", 1224}}}};
    return ai::test::sseBody("data: " + delta.dump() + "\n\ndata: " + stop.dump() + "\n\ndata: " + usage.dump() + "\n\ndata: [DONE]\n\n");
}

} // namespace

TEST_CASE("The eval set has 40 to 60 requests over every editing area each with a check and a reference", "[ai][eval]") {
    const auto& cases = evalCases();
    CHECK(cases.size() >= 40);
    CHECK(cases.size() <= 60);
    std::set<std::string> ids;
    std::map<std::string, int> categories;
    for (const EvalCase& c : cases) {
        INFO(c.id);
        CHECK(ids.insert(c.id).second);
        ++categories[c.category];
        CHECK(c.check);
        CHECK_FALSE(c.prompt.empty());
        CHECK((c.question || !c.reference.empty()));
    }
    for (const char* area : {"trim", "ripple", "markers", "captions", "audio", "colour", "multicam", "library", "question"}) {
        INFO(area);
        CHECK(categories[area] >= 4);
    }
    for (const char* fixture : {"interview", "captions", "multicam", "library"}) {
        INFO(fixture);
        const Fixture fx = makeFixture(fixture);
        wabcut::test::requireValid(fx.state);
        CHECK(fx.state.findSequence(fx.seq) != nullptr);
    }
}

TEST_CASE("Every eval case is solvable through routing and the constrained envelope", "[ai][eval]") {
    for (const std::string model : {"qwen3-1.7b", "qwen3-4b-instruct-2507", "qwen3-8b"}) {
        INFO(model);
        const EvalSummary s = oracleRun(model, OracleStyle::Clean);
        INFO(failures(s));
        CHECK(s.mode == "constrained");
        CHECK(s.cases.size() == evalCases().size());
        CHECK(s.passed() == static_cast<int>(evalCases().size()));
        CHECK(s.routingRecall() == 1.0);
        CHECK(s.validToolCallRate() == 1.0);
        CHECK(s.toJson()["successRate"] == 1.0);
    }
}

TEST_CASE("The oracle passes with native tool calls too except where the schemas overflow the context", "[ai][eval]") {
    // Native tool calls put every offered tool's full JSON schema into the prompt: on an 8k context some requests'
    // tools no longer fit (why the local tiers use the envelope's compact signatures). The agent leaves the lowest-ranked
    // routed tools out until the first call fits (p02 integration).
    const EvalSummary s = oracleRun("qwen3-4b-instruct-2507", OracleStyle::Clean, false);
    INFO(failures(s));
    CHECK(s.mode == "native");
    const auto overflow = std::ranges::count_if(s.cases, [](const CaseResult& c) { return c.status == "BudgetExceeded"; });
    CHECK(s.passed() + overflow == static_cast<std::ptrdiff_t>(evalCases().size()));
    CHECK(overflow <= 4);
}

TEST_CASE("Near-miss JSON from a server that does not enforce the format is repaired and not lost", "[ai][eval][constrained]") {
    const EvalSummary s = oracleRun("qwen3-1.7b", OracleStyle::NearMiss);
    INFO(failures(s));
    CHECK(s.passed() == static_cast<int>(evalCases().size()));
    CHECK(s.validToolCallRate() == 1.0);
    for (const CaseResult& c : s.cases) {
        CHECK(c.malformedReplies == 0);
    }
}

TEST_CASE("The checks are not vacuous: a model that never edits fails every case", "[ai][eval]") {
    const EvalSummary s = oracleRun("qwen3-1.7b", OracleStyle::Lazy);
    CHECK(s.passed() == 0);
    CHECK(s.successRate() == 0.0);
}

TEST_CASE("Eval metrics: success and valid calls and latency percentiles and decode speed", "[ai][eval]") {
    EvalSummary s;
    s.model = "m";
    CaseResult a;
    a.id = "a";
    a.category = "trim";
    a.success = true;
    a.toolCalls = 3;
    a.calls = {{1.0, 0.5, 11, 1000}, {2.0, 1.0, 21, 1000}};
    CaseResult b;
    b.id = "b";
    b.category = "trim";
    b.toolCalls = 2;
    b.invalidToolCalls = 1;
    b.malformedReplies = 1;
    b.routedReference = false;
    b.calls = {{3.0, 1.0, 41, 1000}};
    s.cases = {a, b};
    CHECK(s.passed() == 1);
    CHECK(s.successRate() == 0.5);
    CHECK(s.validToolCallRate() == 4.0 / 6.0); // (3 + 2 + 1 malformed) calls, 2 invalid
    CHECK(s.routingRecall() == 0.5);
    CHECK(s.medianStepSeconds() == 2.0);
    CHECK(s.p90StepSeconds() == 3.0);
    CHECK(s.medianFirstTokenSeconds() == 1.0);
    CHECK(s.decodeTokensPerSecond() == (10.0 + 20.0 + 40.0) / (0.5 + 1.0 + 2.0));
    CHECK(s.byCategory()["trim"] == std::pair{1, 2});
    const Json j = s.toJson();
    CHECK(j["passed"] == 1);
    CHECK(j["results"].size() == 2);
}

TEST_CASE("A llama-server answering with near-miss JSON still stages the edit through the constrained envelope", "[ai][eval][constrained]") {
    ai::test::FakeTransport fake;
    // Step 1: fenced, "tool"/"arguments" keys, the arguments as a string; step 2: words around a valid call that has a
    // wrong argument type (the agent's schema check sends it back); step 3: the corrected call; step 4: the answer.
    fake.push(contentStream("```json\n{\"tool\": \"sequence_summary\", \"arguments\": \"{}\"}\n```"));
    fake.push(contentStream("Here you go: {\"name\": \"marker_add\", \"parameters\": {\"time\": true, \"name\": \"Intro\"}}"));
    fake.push(contentStream("{\"name\": \"marker_add\", \"parameters\": {\"time\": \"00:00:00:05\", \"name\": \"Intro\"}"));
    fake.push(contentStream("{\"answer\": \"Added the marker Intro at 00:00:00:05.\"}"));
    auto model = net::makeChatCompletionsChat(ai::test::context("qwen3-1.7b", fake));

    wabcut::test::Timeline timeline("V1 tgt |Aaaaaaaaaa|\nA1 tgt |Aaaaaaaaaa|");
    agent::AgentRequest request;
    request.prompt = "Add a marker named Intro at 00:00:00:05.";
    request.sequence = timeline.seq;
    request.base = std::make_shared<const ProjectState>(timeline.state);
    const agent::AgentOutcome outcome = agent::EditAgent{}.run(*model, request, {}, {});
    INFO(outcome.error);
    REQUIRE(outcome.status == agent::AgentStatus::Staged);
    REQUIRE(outcome.staged->findSequence(timeline.seq)->markers.size() == 1);
    CHECK(outcome.staged->findSequence(timeline.seq)->markers[0].start == wabcut::test::f(5));
    CHECK(outcome.answer == "Added the marker Intro at 00:00:00:05.");
    CHECK(outcome.toolCalls == 3);
    CHECK(outcome.invalidToolCalls == 1); // "time": true
    CHECK(outcome.malformedReplies == 0);
    CHECK(outcome.repairedReplies == 3); // fenced, words around it, cut off
    CHECK(outcome.offeredTools.size() <= 8);
    CHECK(fake.remaining() == 0);
    for (const auto& r : fake.requests()) {
        const Json body = ai::test::requestBody(r);
        CHECK_FALSE(body.contains("tools"));
        CHECK(body["max_tokens"] == 1024);
        REQUIRE(body["response_format"]["type"] == "json_schema");
        const Json& schema = body["response_format"]["json_schema"]["schema"];
        CHECK(schema["anyOf"].size() == outcome.offeredTools.size() + 1);
        CHECK(body["messages"][0]["role"] == "system");
        CHECK(body["messages"][0]["content"].get<std::string>().find("# Tools\n") != std::string::npos);
    }
}

// Hidden diagnostic: how the router ranks each eval request (run with "[routing]").
TEST_CASE("Routing report for the eval requests", "[.][routing]") {
    const agent::EditAgent agent{agent::AgentOptions{}};
    const std::vector<ToolSpec> tools = agent.tools();
    agent::RouteOptions options;
    options.maxTools = 16;
    for (const EvalCase& c : evalCases()) {
        const auto ranked = agent::rankTools(tools, agent.registry(), c.prompt, options);
        std::string line = c.id + ":";
        for (std::size_t i = 0; i < ranked.size(); ++i) {
            line += " " + std::to_string(i + 1) + "." + ranked[i].name + "(" + std::to_string(static_cast<int>(ranked[i].score * 10)) + ")";
        }
        for (const ToolUse& use : c.reference) {
            const auto it = std::ranges::find_if(ranked, [&](const agent::RoutedTool& r) { return r.name == use.tool; });
            line += "  [" + use.tool + " @" + (it == ranked.end() ? std::string("-") : std::to_string(it - ranked.begin() + 1)) + "]";
        }
        std::printf("%s\n", line.c_str());
    }
}
