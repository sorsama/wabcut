// The editing assistant through each real chat adapter (AI-5), on recorded sessions (tests/ai/fixtures/agent/
// <provider>/turn1..4.sse): the model reads the sequence, calls marker_add without its required time, gets the
// error back in the provider's wire format, fixes the call and finishes. No network. An optional live session runs
// when WABCUT_LIVE_ANTHROPIC_KEY is set (never in CI; it costs a few cents).

#include "CommandHarness.h"
#include "NetTestSupport.h"

#include <wabcut/ai/agent/EditAgent.h>
#include <wabcut/ai_net/Adapters.h>
#include <wabcut/ai_net/Network.h>

#include <QCoreApplication>

#include <catch2/catch_test_macros.hpp>

#include <functional>

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::agent;
using namespace wabcut::ai::test;
using wabcut::test::Timeline;

namespace {

constexpr std::string_view kPicture = R"(
    V1 tgt |AaaaaaaaaaBbbbbbbbbbCccccccccc|
    A1 tgt |AaaaaaaaaaBbbbbbbbbbCccccccccc|
)";
constexpr std::string_view kPrompt = "Add a marker named Intro at 00:00:00:05.";

/// The first object in `j` (depth first) for which `match` holds.
const Json* find(const Json& j, const std::function<bool(const Json&)>& match) {
    if (j.is_object() && match(j)) {
        return &j;
    }
    if (j.is_structured()) {
        for (const Json& child : j) {
            if (const Json* found = find(child, match)) {
                return found;
            }
        }
    }
    return nullptr;
}

bool mentions(const Json& value, std::string_view text) {
    return value.is_string() ? value.get<std::string>().find(text) != std::string::npos : value.dump().find(text) != std::string::npos;
}

struct Session {
    Session() : tl(kPicture), live(tl.state) {}

    AgentOutcome run(IChatModel& model, FakeTransport& fake, const std::string& provider) {
        for (int turn = 1; turn <= 4; ++turn) {
            fake.push(sse("agent/" + provider + "/turn" + std::to_string(turn) + ".sse"));
        }
        AgentRequest request;
        request.prompt = std::string(kPrompt);
        request.sequence = tl.seq;
        request.base = std::make_shared<const ProjectState>(live.stateForSave());
        request.baseRevision = live.revision();
        EditAgent agent;
        AgentOutcome out = agent.run(model, request, {}, {});

        INFO(provider << ": " << out.error);
        CHECK(fake.remaining() == 0);
        CHECK(fake.requests().size() == 4);
        CHECK(out.status == AgentStatus::Staged);
        REQUIRE(out.steps.size() == 4);
        CHECK(out.steps[0].kind == AgentStep::Kind::Read);
        CHECK(out.steps[1].kind == AgentStep::Kind::Error);
        CHECK(out.steps[2].kind == AgentStep::Kind::Edit);
        CHECK(out.steps[3].kind == AgentStep::Kind::Verify);
        CHECK(out.answer == "Added the marker “Intro” at 00:00:00:05.");
        REQUIRE(out.batch.commands.size() == 1);
        CHECK(out.batch.commands[0].params == Json{{"time", "00:00:00:05"}, {"name", "Intro"}, {"sequence", tl.seq.value}});
        const Sequence& staged = *out.staged->findSequence(tl.seq);
        REQUIRE(staged.markers.size() == 1);
        CHECK(staged.markers[0].start == wabcut::test::f(5));
        CHECK(staged.markers[0].name == "Intro");
        CHECK(live.revision() == request.baseRevision); // staged only
        CHECK(out.modelCalls == 4);
        CHECK(out.usage.inputTokens > 30'000);

        const ApplyResult applied = applyStaged(live, out);
        CHECK(applied.status == ApplyResult::Status::Applied);
        CHECK(live.snapshot()->findSequence(tl.seq)->markers.size() == 1);
        return out;
    }

    /// Request bodies as JSON.
    static std::vector<Json> bodies(const FakeTransport& fake) {
        std::vector<Json> out;
        for (const HttpRequest& r : fake.requests()) {
            out.push_back(requestBody(r));
        }
        return out;
    }

    Timeline tl;
    EditSession live;
};

} // namespace

TEST_CASE("Agent session over Anthropic Messages: tool errors as is_error results, prompt cache breakpoints",
          "[ai][net][agent][anthropic]") {
    FakeTransport fake;
    auto chat = net::makeAnthropicChat(context("claude-sonnet-5-5", fake));
    Session s;
    const AgentOutcome out = s.run(*chat, fake, "anthropic");
    CHECK(out.usage.cacheReadTokens == 9000 + 9600 + 9780);
    CHECK(out.usage.cacheWriteTokens == 9000);

    const auto bodies = Session::bodies(fake);
    // The bad call's answer: a tool_result marked is_error, first in the user turn.
    const Json& error = bodies[2]["messages"].back()["content"][0];
    CHECK(error["type"] == "tool_result");
    CHECK(error["tool_use_id"] == "toolu_02Add");
    CHECK(error["is_error"] == true);
    CHECK(mentions(error["content"], "InvalidParams"));
    // The read's answer is not an error.
    CHECK_FALSE(bodies[1]["messages"].back()["content"][0].contains("is_error"));
    // Cache breakpoints: the last tool, the system prompt and the end of the conversation, on every call.
    for (const Json& body : bodies) {
        CHECK(body["tools"].back()["cache_control"] == Json{{"type", "ephemeral"}});
        CHECK(body["system"][0]["cache_control"] == Json{{"type", "ephemeral"}});
        CHECK(body["messages"].back()["content"].back()["cache_control"] == Json{{"type", "ephemeral"}});
        CHECK(body["system"] == bodies[0]["system"]); // the same prefix every time
        CHECK(body["tools"] == bodies[0]["tools"]);
    }
}

TEST_CASE("Agent session over OpenAI Responses: strict nulls dropped, errors as function_call_output, reasoning replayed",
          "[ai][net][agent][openai]") {
    FakeTransport fake;
    auto chat = net::makeOpenAiResponsesChat(context("gpt-6.1-sol", fake));
    Session s;
    const AgentOutcome out = s.run(*chat, fake, "openai");
    CHECK(out.steps[0].ok); // {"from": null, ...} read as "not given"
    CHECK(out.steps[1].detail.find("time") != std::string::npos);
    CHECK(out.usage.cacheReadTokens == 9088 + 9728 + 9920);

    const auto bodies = Session::bodies(fake);
    const Json* error = find(bodies[2]["input"], [](const Json& o) {
        return o.value("type", "") == "function_call_output" && o.value("call_id", "") == "call_add_1";
    });
    REQUIRE(error);
    CHECK(mentions((*error)["output"], "InvalidParams"));
    const Json* reasoning = find(bodies[1]["input"], [](const Json& o) {
        return o.value("type", "") == "reasoning" && o.value("encrypted_content", "") == "gAAAAABagentTurn1Encrypted==";
    });
    CHECK(reasoning);
    CHECK(bodies[3]["store"] == false);
}

TEST_CASE("Agent session over Chat Completions (a local model): errors as tool messages", "[ai][net][agent][chat]") {
    ModelRegistry registry = ModelRegistry::builtin();
    ModelSpec local;
    local.id = "qwen3.5:9b";
    local.provider = "ollama";
    REQUIRE(registry.addModel(local));
    FakeTransport fake;
    const ProviderSpec& ollama = *registry.provider("ollama");
    auto chat = net::makeChatCompletionsChat(ProviderContext{ollama, *registry.model("qwen3.5:9b"), ollama.baseUrl, Secret(), fake});
    Session s;
    const AgentOutcome out = s.run(*chat, fake, "chat");
    CHECK(out.cost == 0); // a local model has no price

    const auto bodies = Session::bodies(fake);
    const Json* error = find(bodies[2]["messages"], [](const Json& o) {
        return o.value("role", "") == "tool" && o.value("tool_call_id", "") == "call_add_1";
    });
    REQUIRE(error);
    CHECK(mentions((*error)["content"], "InvalidParams"));
    const Json* call = find(bodies[1]["messages"], [](const Json& o) { return o.value("role", "") == "assistant"; });
    REQUIRE(call);
    CHECK((*call)["tool_calls"][0]["function"]["name"] == "sequence_summary");
}

TEST_CASE("Agent session over Gemini Interactions: errors as function_result is_error, thought signatures replayed",
          "[ai][net][agent][gemini]") {
    FakeTransport fake;
    auto chat = net::makeGeminiInteractionsChat(context("gemini-3.8-flash", fake));
    Session s;
    const AgentOutcome out = s.run(*chat, fake, "gemini");
    CHECK(out.usage.cacheReadTokens == 8990 + 9600 + 9800);

    const auto bodies = Session::bodies(fake);
    const Json* error = find(bodies[2], [](const Json& o) {
        return o.value("type", "") == "function_result" && o.value("call_id", "") == "fc_add_1";
    });
    REQUIRE(error);
    CHECK((*error)["is_error"] == true);
    CHECK(mentions((*error)["result"], "InvalidParams"));
    CHECK(bodies[1].dump().find("CiQBagentTurn1Signature") != std::string::npos);
}

TEST_CASE("Agent live session (WABCUT_LIVE_ANTHROPIC_KEY)", "[ai][net][agent][live]") {
    const auto key = systemEnv("WABCUT_LIVE_ANTHROPIC_KEY");
    if (!key) {
        SUCCEED("WABCUT_LIVE_ANTHROPIC_KEY is not set: live test skipped");
        return;
    }
    if (!QCoreApplication::instance()) {
        static int argc = 1;
        static char name[] = "wabcut_ai_net_tests";
        static char* argv[] = {name, nullptr};
        static QCoreApplication app(argc, argv);
    }
    RequestLog log;
    net::NetworkStack stack(log);
    auto chat = net::makeAnthropicChat(context("claude-haiku-5-5", stack.transport(), *key));
    Timeline tl(kPicture);
    EditSession live(tl.state);
    AgentRequest request;
    request.prompt = std::string(kPrompt);
    request.sequence = tl.seq;
    request.base = std::make_shared<const ProjectState>(live.stateForSave());
    request.baseRevision = live.revision();
    AgentOptions options;
    options.limits.maxCost = 200'000; // US$0.20
    const AgentOutcome out = EditAgent(options).run(*chat, request, {}, {});
    INFO(out.error << "\n" << out.transcript.dump(2));
    CHECK(out.status == AgentStatus::Staged);
    CHECK(out.staged->findSequence(tl.seq)->markers.size() == 1);
}
