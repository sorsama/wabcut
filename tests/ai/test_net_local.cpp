#include "NetTestSupport.h"
#include "TimelineFixture.h"
#include <wabcut/ai/agent/EditAgent.h>
#include <wabcut/ai_net/Adapters.h>
#include <filesystem>
#include <system_error>
using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::test;
TEST_CASE("Managed local and cloud adapters replay the same staged tool repair task", "[ai][net][llm][eval]") {
    for (const bool local : {true, false}) {
        INFO((local ? "managed Qwen adapter fixture" : "OpenAI adapter fixture"));
        FakeTransport fake;
        for (int turn = 1; turn <= 4; ++turn)
            fake.push(sse(std::string("agent/") + (local ? "chat" : "openai") + "/turn" + std::to_string(turn) + ".sse"));
        bool busy = false; int prepares = 0;
        auto model = local ? net::makeManagedChat(context("qwen3-4b-instruct-2507", fake),
            [&](std::stop_token) -> std::expected<std::pair<std::string, Secret>, AiError> {
                ++prepares; return std::pair{std::string("http://127.0.0.1:42345/v1"), Secret("ephemeral-test-token")};
            }, [&](bool value) { busy = value; }) : net::makeOpenAiResponsesChat(context("gpt-6.1-sol", fake));
        wabcut::test::Timeline timeline("V1 tgt |Aaaaaaaaaa|\nA1 tgt |Aaaaaaaaaa|");
        EditSession live(timeline.state);
        agent::AgentRequest request;
        request.prompt = "Add a marker named Intro at 00:00:00:05.";
        request.sequence = timeline.seq; request.base = std::make_shared<const ProjectState>(live.stateForSave());
        request.baseRevision = live.revision();
        auto outcome = agent::EditAgent{}.run(*model, request, {}, {});
        INFO(outcome.error);
        REQUIRE(outcome.status == agent::AgentStatus::Staged);
        REQUIRE(outcome.steps.size() == 4);
        CHECK(outcome.steps[1].kind == agent::AgentStep::Kind::Error);
        CHECK(outcome.steps[2].kind == agent::AgentStep::Kind::Edit);
        CHECK(outcome.modelCalls == 4);
        REQUIRE(outcome.staged->findSequence(timeline.seq)->markers.size() == 1);
        CHECK(outcome.staged->findSequence(timeline.seq)->markers[0].start == wabcut::test::f(5));
        CHECK(live.snapshot()->findSequence(timeline.seq)->markers.empty());
        CHECK(fake.remaining() == 0);
        if (local) {
            CHECK(prepares == 4); CHECK_FALSE(busy);
            for (const auto& r : fake.requests()) {
                CHECK(r.url == "http://127.0.0.1:42345/v1/chat/completions");
                CHECK(findHeader(r.headers, "Authorization") == "Bearer ephemeral-test-token");
                CHECK(requestBody(r)["tools"].size() <= 15);
                CHECK(requestBody(r)["max_tokens"] == 1024);
            }
        }
    }
}

// Review wave 4, P2-6: preparing the managed runtime runs on the chat job thread (and AiController's pool), which do not
// catch. A file-system exception while preparing (an antivirus locking the fresh extraction, an access error) must
// become the call's error, not std::terminate.
TEST_CASE("A local runtime that throws while preparing fails the chat call, not the thread", "[ai][net][llm]") {
    FakeTransport fake;
    bool busy = false;
    auto model = net::makeManagedChat(
        context("qwen3-4b-instruct-2507", fake),
        [](std::stop_token) -> std::expected<std::pair<std::string, Secret>, AiError> {
            throw std::filesystem::filesystem_error("recursive_directory_iterator", std::make_error_code(std::errc::permission_denied));
        },
        [&](bool value) { busy = value; });
    ChatRequest request;
    request.messages.push_back(ChatMessage::user("hello"));
    std::expected<ChatResponse, AiError> result = std::unexpected(AiError());
    REQUIRE_NOTHROW(result = model->chat(request, {}, {}));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == AiErrorCode::ProviderError);
    CHECK(result.error().message.find("could not start") != std::string::npos);
    CHECK_FALSE(busy);
    CHECK(fake.requests().empty());
}
