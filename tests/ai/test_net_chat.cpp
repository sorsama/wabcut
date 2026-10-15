// Recorded-fixture contract tests of the four chat adapters: request snapshots, streamed text, tool calls,
// reasoning replay, usage, mid-stream errors and HTTP errors including 429 with Retry-After. No network.

#include "NetTestSupport.h"

#include <wabcut/ai/Transport.h>
#include <wabcut/ai_net/Adapters.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut::ai;
using namespace wabcut::ai::test;
using namespace std::chrono_literals;

namespace {

ChatRequest hello() {
    ChatRequest r;
    r.messages.push_back(ChatMessage::user("Say hello."));
    r.maxOutputTokens = 64;
    return r;
}

} // namespace

// --- Anthropic Messages ---------------------------------------------------------------------------------------

TEST_CASE("Anthropic: request shape for an agent turn", "[ai][net][anthropic]") {
    FakeTransport fake;
    fake.push(sse("anthropic/text_stream.sse"));
    auto chat = net::makeAnthropicChat(context("claude-sonnet-5-5", fake));
    REQUIRE(chat->chat(agentRequest(), {}, {}));

    const HttpRequest sent = fake.requests().at(0);
    CHECK(sent.url == "https://api.anthropic.com/v1/messages");
    CHECK(sent.responseMode == BodyMode::Sse);
    CHECK(findHeader(sent.headers, "x-api-key") == "test-key-0123456789");
    CHECK(findHeader(sent.headers, "anthropic-version") == "2023-06-01");
    const Json body = requestBody(sent);
    CHECK(body["model"] == "claude-sonnet-5-5");
    CHECK(body["stream"] == true);
    CHECK_FALSE(body.contains("temperature"));
    // Sonnet 5.5 rejects forced tool choice: Required is sent as auto, with strict tools.
    CHECK(body["tool_choice"] == Json{{"type", "auto"}});
    CHECK(body["tools"][0]["strict"] == true);
    CHECK_FALSE(body["tools"][0]["input_schema"]["properties"]["frames"].contains("minimum"));
    // tool_result first in the user turn that answers the tool_use.
    CHECK(body["messages"][2]["content"][0]["type"] == "tool_result");
    CHECK(body["messages"][0]["content"][1]["source"]["type"] == "base64");
    checkSnapshot("anthropic/agent_request.json", body);
}

TEST_CASE("Anthropic: streamed text, usage and events", "[ai][net][anthropic]") {
    FakeTransport fake;
    fake.push(sse("anthropic/text_stream.sse", 3)); // tiny chunks split the Thai characters
    auto chat = net::makeAnthropicChat(context("claude-sonnet-5-5", fake));
    EventLog events;
    auto r = chat->chat(hello(), events.handler(), {});
    REQUIRE(r);
    CHECK(r->message.text() == "Hello, สวัสดีครับ");
    CHECK(events.text == r->message.text());
    CHECK(r->stopReason == StopReason::EndTurn);
    CHECK(r->rawStopReason == "end_turn");
    CHECK(r->id == "msg_01TextExample");
    CHECK(r->usage.inputTokens == 35); // 25 + 10 cached
    CHECK(r->usage.cacheReadTokens == 10);
    CHECK(r->usage.outputTokens == 15);
    CHECK(events.usageUpdates == 2);
}

TEST_CASE("Anthropic: thinking, text and a streamed tool call; thinking replayed verbatim", "[ai][net][anthropic]") {
    FakeTransport fake;
    fake.push(sse("anthropic/tool_stream.sse"));
    auto chat = net::makeAnthropicChat(context("claude-sonnet-5-5", fake));
    EventLog events;
    auto r = chat->chat(agentRequest(), events.handler(), {});
    REQUIRE(r);
    CHECK(r->stopReason == StopReason::ToolUse);
    REQUIRE(r->message.parts.size() == 3);
    const auto& thinking = std::get<ReasoningPart>(r->message.parts[0]);
    CHECK(thinking.provider == "anthropic");
    CHECK(thinking.text == "The user wants the gap removed.");
    CHECK(thinking.signature == "EqQBCgIYAhIM1gbcDa9GJwZA2b3hGgxBdjrkzLoky3dl1pkiMOYds");
    const auto calls = r->message.toolCalls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].id == "toolu_01T1x1fJ34qAmk2tNTrN7Up6");
    CHECK(calls[0].name == "timeline_rippleDelete");
    CHECK(calls[0].arguments == Json{{"track", "A1"}, {"frames", 48}});
    CHECK(events.toolsStarted == std::vector<std::string>{"timeline_rippleDelete"});
    CHECK(events.arguments == R"({"track": "A1", "frames": 48})");
    CHECK(events.reasoning == "The user wants the gap removed.");

    // The next turn sends the thinking block back, signature included, before the tool_use.
    ChatRequest next = agentRequest();
    next.messages.push_back(r->message);
    next.messages.push_back(ChatMessage::toolResults({ToolResult{calls[0].id, calls[0].name, "{\"ok\":true}", false}}));
    fake.push(sse("anthropic/text_stream.sse"));
    REQUIRE(chat->chat(next, {}, {}));
    const Json body = requestBody(fake.requests().at(1));
    const Json& replayed = body["messages"][3]["content"];
    CHECK(replayed[0] == Json{{"type", "thinking"}, {"thinking", thinking.text}, {"signature", thinking.signature}});
    CHECK(replayed[2]["type"] == "tool_use");
    CHECK(replayed[2]["input"] == Json{{"track", "A1"}, {"frames", 48}});
}

TEST_CASE("Anthropic: errors in the stream and over HTTP", "[ai][net][anthropic]") {
    FakeTransport fake;
    auto chat = net::makeAnthropicChat(context("claude-sonnet-5-5", fake));

    fake.push(sse("anthropic/error_stream.sse"));
    auto overloaded = chat->chat(hello(), {}, {});
    REQUIRE_FALSE(overloaded);
    CHECK(overloaded.error().code == AiErrorCode::RateLimited);
    CHECK(overloaded.error().providerCode == "overloaded_error");

    fake.push(json(401, "anthropic/bad_key.json"));
    CHECK(chat->chat(hello(), {}, {}).error().code == AiErrorCode::Unauthorized);

    ScriptedResponse cut = sse("anthropic/text_stream.sse");
    cut.body.resize(cut.body.find("event: message_stop"));
    fake.push(cut);
    CHECK(chat->chat(hello(), {}, {}).error().code == AiErrorCode::Network); // ended before message_stop

    ChatRequest video = hello();
    video.messages[0].parts.push_back(VideoPart{});
    CHECK(chat->chat(video, {}, {}).error().code == AiErrorCode::Unsupported);
    CHECK(fake.remaining() == 0);
}

TEST_CASE("Anthropic: 429 with Retry-After is retried by the transport, then succeeds", "[ai][net][anthropic][retry]") {
    FakeTransport fake;
    std::vector<std::chrono::milliseconds> waits;
    RetryingTransport retrying(fake, {}, [&](std::chrono::milliseconds d, std::stop_token) {
        waits.push_back(d);
        return true;
    });
    auto chat = net::makeAnthropicChat(context("claude-haiku-5-5", retrying));
    fake.push(json(429, "anthropic/rate_limit.json", {{"retry-after", "3"}}));
    fake.push(sse("anthropic/text_stream.sse"));
    auto r = chat->chat(hello(), {}, {});
    REQUIRE(r);
    CHECK(r->message.text() == "Hello, สวัสดีครับ");
    CHECK(waits == std::vector<std::chrono::milliseconds>{3000ms});

    // A retry-after the policy will not wait for surfaces as RateLimited with the hint.
    fake.push(json(429, "anthropic/rate_limit.json", {{"retry-after", "900"}}));
    auto limited = chat->chat(hello(), {}, {});
    REQUIRE_FALSE(limited);
    CHECK(limited.error().code == AiErrorCode::RateLimited);
    CHECK(limited.error().retryAfter == 900000ms);

    // Haiku accepts forced tool choice.
    fake.push(sse("anthropic/text_stream.sse"));
    REQUIRE(chat->chat(agentRequest(), {}, {}));
    CHECK(requestBody(fake.requests().back())["tool_choice"] == Json{{"type", "any"}});
}

// --- OpenAI Responses -----------------------------------------------------------------------------------------

TEST_CASE("OpenAI Responses: request shape (stateless, strict tools)", "[ai][net][openai]") {
    FakeTransport fake;
    fake.push(sse("openai/text_stream.sse"));
    auto chat = net::makeOpenAiResponsesChat(context("gpt-6.1-sol", fake));
    REQUIRE(chat->chat(agentRequest(), {}, {}));
    const HttpRequest sent = fake.requests().at(0);
    CHECK(sent.url == "https://api.openai.com/v1/responses");
    CHECK(findHeader(sent.headers, "Authorization") == "Bearer test-key-0123456789");
    const Json body = requestBody(sent);
    CHECK(body["store"] == false);
    CHECK(body["include"] == Json::array({"reasoning.encrypted_content"}));
    CHECK(body["instructions"] == "You edit video timelines. Use the tools.");
    CHECK(body["tool_choice"] == "required");
    const Json& params = body["tools"][0]["parameters"];
    CHECK(body["tools"][0]["strict"] == true);
    CHECK(params["required"] == Json::array({"frames", "track"})); // nlohmann::json keeps keys sorted
    CHECK(params["properties"]["frames"]["type"] == Json::array({"integer", "null"}));
    CHECK(body["input"][0]["content"][1]["type"] == "input_image");
    CHECK(body["input"][1]["role"] == "assistant");
    CHECK(body["input"][2]["type"] == "function_call");
    CHECK(body["input"][2]["arguments"] == R"({"track":"A1"})");
    CHECK(body["input"][3] == Json{{"type", "function_call_output"}, {"call_id", "call_1"}, {"output", R"({"clips":3})"}});
    checkSnapshot("openai/agent_request.json", body);
}

TEST_CASE("OpenAI Responses: text, usage, tool calls and encrypted reasoning", "[ai][net][openai]") {
    FakeTransport fake;
    auto chat = net::makeOpenAiResponsesChat(context("gpt-6.1-sol", fake));

    fake.push(sse("openai/text_stream.sse"));
    EventLog events;
    auto text = chat->chat(hello(), events.handler(), {});
    REQUIRE(text);
    CHECK(text->message.text() == "Hi there");
    CHECK(events.text == "Hi there");
    CHECK(text->stopReason == StopReason::EndTurn);
    CHECK(text->usage.inputTokens == 37);
    CHECK(text->usage.cacheReadTokens == 5);
    CHECK(text->usage.reasoningTokens == 4);
    CHECK(text->model == "gpt-6.1-sol");

    fake.push(sse("openai/tool_stream.sse"));
    auto tool = chat->chat(agentRequest(), {}, {});
    REQUIRE(tool);
    CHECK(tool->stopReason == StopReason::ToolUse);
    const auto calls = tool->message.toolCalls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].id == "call_abc123");
    CHECK(calls[0].arguments == Json{{"track", "A1"}, {"frames", nullptr}});
    const auto& reasoning = std::get<ReasoningPart>(tool->message.parts[0]);
    CHECK(reasoning.id == "rs_1");
    CHECK(reasoning.signature == "gAAAAABencryptedReasoningExample==");

    // Replayed in the next turn as a reasoning item, before the function call.
    ChatRequest next = agentRequest();
    next.messages.push_back(tool->message);
    next.messages.push_back(ChatMessage::toolResults({ToolResult{"call_abc123", calls[0].name, "done", false}}));
    fake.push(sse("openai/text_stream.sse"));
    REQUIRE(chat->chat(next, {}, {}));
    const Json input = requestBody(fake.requests().back())["input"];
    CHECK(input[4] == Json{{"type", "reasoning"}, {"id", "rs_1"}, {"encrypted_content", reasoning.signature}, {"summary", Json::array()}});
    CHECK(input[5]["call_id"] == "call_abc123");
}

TEST_CASE("OpenAI Responses: failed, incomplete and quota errors", "[ai][net][openai]") {
    FakeTransport fake;
    auto chat = net::makeOpenAiResponsesChat(context("gpt-6-luna", fake));

    fake.push(sse("openai/failed_stream.sse"));
    auto failed = chat->chat(hello(), {}, {});
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == AiErrorCode::ProviderError);
    CHECK(failed.error().providerCode == "server_error");

    fake.push(sse("openai/incomplete_stream.sse"));
    auto cut = chat->chat(hello(), {}, {});
    REQUIRE(cut);
    CHECK(cut->stopReason == StopReason::MaxTokens);
    CHECK(cut->message.text() == "The first three");

    fake.push(json(429, "openai/insufficient_quota.json"));
    CHECK(chat->chat(hello(), {}, {}).error().code == AiErrorCode::QuotaExceeded);
    fake.push(json(429, "openai/rate_limit.json", {{"retry-after", "1"}}));
    auto limited = chat->chat(hello(), {}, {});
    CHECK(limited.error().code == AiErrorCode::RateLimited);
    CHECK(limited.error().retryAfter == 1000ms);
}

// --- Chat Completions -----------------------------------------------------------------------------------------

TEST_CASE("Chat Completions: request shape for a local server", "[ai][net][chat]") {
    ModelRegistry registry = ModelRegistry::builtin();
    ModelSpec local;
    local.id = "qwen3.5:9b";
    local.provider = "ollama";
    REQUIRE(registry.addModel(local));
    FakeTransport fake;
    fake.push(sse("chat/text_stream.sse"));
    const ProviderSpec& ollama = *registry.provider("ollama");
    auto chat = net::makeChatCompletionsChat(ProviderContext{ollama, *registry.model("qwen3.5:9b"), ollama.baseUrl, Secret(), fake});
    ChatRequest request = agentRequest();
    request.toolChoice.mode = ToolChoiceMode::Auto;
    REQUIRE(chat->chat(request, {}, {}));
    const HttpRequest sent = fake.requests().at(0);
    CHECK(sent.url == "http://localhost:11434/v1/chat/completions");
    CHECK_FALSE(findHeader(sent.headers, "Authorization")); // no key for a local server
    const Json body = requestBody(sent);
    CHECK(body["stream_options"] == Json{{"include_usage", true}});
    CHECK_FALSE(body.contains("tool_choice")); // auto is the default; Ollama rejects the field
    CHECK(body["messages"][0] == Json{{"role", "system"}, {"content", "You edit video timelines. Use the tools."}});
    CHECK(body["messages"][1]["content"][1]["type"] == "image_url");
    CHECK(body["messages"][2]["tool_calls"][0]["function"]["arguments"] == R"({"track":"A1"})");
    CHECK(body["messages"][3] == Json{{"role", "tool"}, {"tool_call_id", "call_1"}, {"content", R"({"clips":3})"}});
    CHECK(body["tools"][0]["function"]["parameters"]["properties"]["frames"]["minimum"] == 1); // not strict: kept
    checkSnapshot("chat/agent_request.json", body);
}

TEST_CASE("Chat Completions: text, reasoning, parallel tool calls, usage", "[ai][net][chat]") {
    FakeTransport fake;
    auto chat = net::makeChatCompletionsChat(context("voxtral-mini-latest", fake)); // any model of a chat provider

    fake.push(sse("chat/text_stream.sse"));
    EventLog events;
    auto text = chat->chat(hello(), events.handler(), {});
    REQUIRE(text);
    CHECK(text->message.text() == "Three clips on A1.");
    CHECK(events.reasoning == "Short answer.");
    CHECK(text->stopReason == StopReason::EndTurn);
    CHECK(text->usage.inputTokens == 52);
    CHECK(text->usage.outputTokens == 9);

    fake.push(sse("chat/tool_stream.sse", 5));
    auto tools = chat->chat(agentRequest(), {}, {});
    REQUIRE(tools);
    CHECK(tools->stopReason == StopReason::ToolUse);
    const auto calls = tools->message.toolCalls();
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].id == "call_a");
    CHECK(calls[0].arguments == Json{{"track", "A2"}});
    CHECK(calls[1].name == "sequence_summary");
    CHECK(calls[1].arguments == Json::object());
    CHECK(tools->usage.reasoningTokens == 10);

    fake.push(sse("chat/ollama_tool_stream.sse"));
    auto ollama = chat->chat(agentRequest(), {}, {});
    REQUIRE(ollama);
    CHECK(ollama->stopReason == StopReason::ToolUse); // reported as "stop" with a tool call
    REQUIRE(ollama->message.toolCalls().size() == 1);
    CHECK(ollama->message.toolCalls()[0].id == "call_0");
    CHECK(ollama->message.toolCalls()[0].arguments == Json{{"track", "V1"}});

    fake.push(sse("chat/error_stream.sse"));
    auto failed = chat->chat(hello(), {}, {});
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == AiErrorCode::ProviderError);
    CHECK(failed.error().httpStatus == 200);
}

TEST_CASE("Chat Completions: strict schemas only where the model supports them", "[ai][net][chat]") {
    ModelRegistry registry = ModelRegistry::builtin();
    ModelSpec strictModel;
    strictModel.id = "openai/gpt-oss-120b";
    strictModel.provider = "groq";
    strictModel.caps.capabilities = {Capability::Chat};
    strictModel.caps.strictSchema = true;
    REQUIRE(registry.addModel(strictModel));
    FakeTransport fake;
    fake.push(sse("chat/text_stream.sse"));
    const ProviderSpec& groq = *registry.provider("groq");
    auto chat = net::makeChatCompletionsChat(
        ProviderContext{groq, *registry.model("openai/gpt-oss-120b"), groq.baseUrl, Secret("gsk-test-0123456789"), fake});
    ChatRequest request = agentRequest();
    request.toolChoice = {ToolChoiceMode::Named, "timeline_rippleDelete", false};
    REQUIRE(chat->chat(request, {}, {}));
    const Json body = requestBody(fake.requests().at(0));
    CHECK(fake.requests().at(0).url == "https://api.groq.com/openai/v1/chat/completions");
    CHECK(body["tools"][0]["function"]["strict"] == true);
    CHECK(body["tools"][0]["function"]["parameters"]["required"] == Json::array({"frames", "track"})); // nlohmann::json keeps keys sorted
    CHECK(body["tool_choice"] == Json{{"type", "function"}, {"function", {{"name", "timeline_rippleDelete"}}}});
    CHECK(body["parallel_tool_calls"] == false);
}

// --- Gemini Interactions --------------------------------------------------------------------------------------

TEST_CASE("Gemini: request shape, text stream and usage", "[ai][net][gemini]") {
    FakeTransport fake;
    fake.push(sse("gemini/text_stream.sse"));
    auto chat = net::makeGeminiInteractionsChat(context("gemini-3.8-flash", fake));
    EventLog events;
    auto r = chat->chat(agentRequest(), events.handler(), {});
    REQUIRE(r);
    CHECK(r->message.text() == "1, 2, 3, go.");
    CHECK(events.text == r->message.text());
    CHECK(r->stopReason == StopReason::EndTurn);
    CHECK(r->usage.inputTokens == 100);
    CHECK(r->usage.outputTokens == 30); // output + thought tokens
    CHECK(r->usage.reasoningTokens == 5);

    const HttpRequest sent = fake.requests().at(0);
    CHECK(sent.url == "https://generativelanguage.googleapis.com/v1beta/interactions?alt=sse");
    CHECK(findHeader(sent.headers, "x-goog-api-key") == "test-key-0123456789");
    const Json body = requestBody(sent);
    CHECK(body["store"] == false);
    CHECK(body["system_instruction"] == "You edit video timelines. Use the tools.");
    CHECK(body["generation_config"]["tool_choice"] == "any");
    CHECK_FALSE(body["tools"][0]["parameters"].contains("additionalProperties"));
    CHECK(body["input"][0]["type"] == "user_input");
    CHECK(body["input"][0]["content"][1]["type"] == "image");
    CHECK(body["input"][2] == Json{{"type", "function_call"}, {"id", "call_1"}, {"name", "sequence_summary"},
                                   {"arguments", {{"track", "A1"}}}});
    CHECK(body["input"][3]["type"] == "function_result");
    CHECK(body["input"][3]["name"] == "sequence_summary");
    checkSnapshot("gemini/agent_request.json", body);
}

TEST_CASE("Gemini: thought replay and function calls in both documented delta forms", "[ai][net][gemini]") {
    FakeTransport fake;
    fake.push(sse("gemini/tool_stream.sse"));
    auto chat = net::makeGeminiInteractionsChat(context("gemini-3.8-flash", fake));
    auto r = chat->chat(agentRequest(), {}, {});
    REQUIRE(r);
    CHECK(r->stopReason == StopReason::ToolUse);
    CHECK(r->rawStopReason == "requires_action");
    const auto calls = r->message.toolCalls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].id == "un6k8t18");
    CHECK(calls[0].arguments == Json{{"track", "A1"}, {"frames", 24}});
    const auto& thought = std::get<ReasoningPart>(r->message.parts[0]);
    CHECK(thought.text == "Find the gap first.");

    ChatRequest next = agentRequest();
    next.messages.push_back(r->message);
    next.messages.push_back(ChatMessage::toolResults({ToolResult{"un6k8t18", "timeline_rippleDelete", "{}", false}}));
    fake.push(sse("gemini/text_stream.sse"));
    REQUIRE(chat->chat(next, {}, {}));
    const Json input = requestBody(fake.requests().back())["input"];
    CHECK(input[4] == Json{{"type", "thought"},
                           {"signature", "CiQBVKhc7mQ3exampleSignature"},
                           {"summary", Json::array({Json{{"type", "text"}, {"text", "Find the gap first."}}})}});
    CHECK(input[5]["type"] == "function_call");
    CHECK(input[6] == Json{{"type", "function_result"}, {"name", "timeline_rippleDelete"}, {"call_id", "un6k8t18"}, {"result", "{}"}});
}

TEST_CASE("Gemini: native video is uploaded through the Files API first", "[ai][net][gemini]") {
    const auto video = uniqueTempPath("wabcut-ai-net-video", ".mp4");
    std::ofstream(video, std::ios::binary) << "not-a-video!";
    FakeTransport fake;
    ScriptedResponse started;
    started.headers = {{"x-goog-upload-url", "https://generativelanguage.googleapis.com/upload/v1beta/files?upload_id=SECRETUPLOAD&upload_protocol=resumable"}};
    fake.push(started);
    fake.push(json(200, "gemini/upload_processing.json"));
    fake.push(json(200, "gemini/file_active.json"));
    fake.push(sse("gemini/text_stream.sse"));
    net::AdapterOptions options;
    int polls = 0;
    options.sleeper = [&](std::chrono::milliseconds, std::stop_token) {
        ++polls;
        return true;
    };
    auto chat = net::makeGeminiInteractionsChat(context("gemini-3.8-flash", fake), options);
    ChatRequest request = hello();
    VideoPart part;
    part.localFile = video;
    part.framesPerSecond = 0.5;
    part.start = wabcut::Ticks::fromSeconds(10) + wabcut::Ticks{wabcut::kTicksPerSecond / 2};
    part.end = wabcut::Ticks::fromSeconds(40);
    request.messages[0].parts.push_back(part);
    REQUIRE(chat->chat(request, {}, {}));
    CHECK(polls == 1);

    const auto requests = fake.requests();
    REQUIRE(requests.size() == 4);
    CHECK(requests[0].url == "https://generativelanguage.googleapis.com/upload/v1beta/files");
    CHECK(findHeader(requests[0].headers, "X-Goog-Upload-Command") == "start");
    CHECK(findHeader(requests[0].headers, "X-Goog-Upload-Header-Content-Length") == "12");
    CHECK(requests[1].url.find("upload_id=SECRETUPLOAD") != std::string::npos);
    CHECK(findHeader(requests[1].headers, "X-Goog-Upload-Command") == "upload, finalize");
    CHECK(requests[1].bodyFile == video);
    CHECK(requests[2].method == "GET");
    CHECK(requests[2].url == "https://generativelanguage.googleapis.com/v1beta/files/abc123xyz");
    const Json videoContent = requestBody(requests[3])["input"][0]["content"][1];
    CHECK(videoContent == Json{{"type", "video"},
                               {"uri", "https://generativelanguage.googleapis.com/v1beta/files/abc123xyz"},
                               {"mime_type", "video/mp4"},
                               {"processing", {{"type", "static"}, {"fps", 0.5}, {"start_offset", "10.5s"}, {"end_offset", "40s"}}}});
    std::filesystem::remove(video);

    // Models without native video refuse it; Claude needs sampled frames.
    auto claude = net::makeAnthropicChat(context("claude-sonnet-5-5", fake));
    CHECK(claude->chat(request, {}, {}).error().code == AiErrorCode::Unsupported);
}

TEST_CASE("Gemini: stream and HTTP errors", "[ai][net][gemini]") {
    FakeTransport fake;
    auto chat = net::makeGeminiInteractionsChat(context("gemini-3.8-flash", fake));
    fake.push(sse("gemini/error_stream.sse"));
    auto failed = chat->chat(hello(), {}, {});
    REQUIRE_FALSE(failed);
    CHECK(failed.error().providerCode == "gateway_timeout");
    fake.push(json(429, "gemini/quota.json"));
    CHECK(chat->chat(hello(), {}, {}).error().code == AiErrorCode::RateLimited);
}
