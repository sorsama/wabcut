// AI-6 security and robustness regressions for the wire adapters (docs/internal/reviews-codex-wave2.md P1-1, P1-2, P2-9):
// the Gemini Files API upload URL is checked before the key or the video go anywhere; every field each adapter reads
// may arrive with another type (null, a number, a string, an object, an array) without anything escaping the
// adapter; and streamed answers are bounded. No network: FakeTransport and the recorded fixtures.

#include "NetTestSupport.h"

#include <wabcut/ai/ConsentLedger.h>
#include <wabcut/ai/Provider.h>
#include <wabcut/ai/RequestLog.h>
#include <wabcut/ai/Secrets.h>
#include <wabcut/ai/SpendGuard.h>
#include <wabcut/ai_net/Adapters.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <functional>
#include <sstream>

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

// --- P1-1 helpers -------------------------------------------------------------------------------------------------

struct VideoFile {
    std::filesystem::path path = uniqueTempPath("wabcut-ai-net-security-video", ".mp4");
    VideoFile() { std::ofstream(path, std::ios::binary) << "not-a-video!"; }
    ~VideoFile() { std::filesystem::remove(path); }
};

/// A Gemini chat whose Files API start returns `uploadUrl`; returns the outcome and the requests that were sent.
std::pair<std::expected<ChatResponse, AiError>, std::vector<HttpRequest>> uploadWith(const std::string& uploadUrl) {
    VideoFile video;
    FakeTransport fake;
    ScriptedResponse started;
    started.headers = {{"x-goog-upload-url", uploadUrl}};
    fake.push(started);
    fake.push(json(200, "gemini/file_active.json"));
    fake.push(sse("gemini/text_stream.sse"));
    net::AdapterOptions options;
    options.sleeper = [](std::chrono::milliseconds, std::stop_token) { return true; };
    auto chat = net::makeGeminiInteractionsChat(context("gemini-3.8-flash", fake), options);
    ChatRequest request = hello();
    VideoPart part;
    part.localFile = video.path;
    request.messages[0].parts.push_back(part);
    auto result = chat->chat(request, {}, {});
    return {std::move(result), fake.requests()};
}

// --- P1-2 helpers -------------------------------------------------------------------------------------------------

/// Every value inside `j` (not the root), as JSON pointers.
void collectPaths(const Json& j, const Json::json_pointer& at, std::vector<Json::json_pointer>& out) {
    if (j.is_object()) {
        for (const auto& [key, value] : j.items()) {
            const auto child = at / key;
            out.push_back(child);
            collectPaths(value, child, out);
        }
    } else if (j.is_array()) {
        for (std::size_t i = 0; i < j.size(); ++i) {
            const auto child = at / i;
            out.push_back(child);
            collectPaths(j[i], child, out);
        }
    }
}

std::vector<Json> substitutes() {
    return {Json(nullptr), Json(7), Json(-2.5), Json("x"), Json(true), Json::object({{"k", 1}}), Json::array({1, "two"})};
}

/// Lines of an SSE fixture; the "data: {...}" ones are what a mutation rewrites.
std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        out.push_back(line);
    }
    return out;
}

/// For every data event of an SSE fixture, every field in it and every wrongly typed substitute: runs `call` with the
/// mutated stream pushed on a FakeTransport. Nothing may throw; the outcome is a result or an AiError.
std::size_t fuzzSse(const std::string& fixtureName, const std::function<std::expected<ChatResponse, AiError>(FakeTransport&)>& call) {
    const auto original = lines(fixture(fixtureName));
    std::size_t runs = 0;
    for (std::size_t li = 0; li < original.size(); ++li) {
        if (!original[li].starts_with("data: ")) {
            continue;
        }
        const Json event = Json::parse(original[li].substr(6), nullptr, false);
        if (event.is_discarded() || !event.is_structured()) {
            continue;
        }
        std::vector<Json::json_pointer> paths;
        collectPaths(event, Json::json_pointer(), paths);
        for (const auto& path : paths) {
            for (const Json& value : substitutes()) {
                Json mutated = event;
                mutated[path] = value;
                std::string body;
                for (std::size_t k = 0; k < original.size(); ++k) {
                    body += (k == li ? "data: " + mutated.dump() : original[k]) + "\n";
                }
                FakeTransport fake;
                ScriptedResponse r;
                r.headers = {{"content-type", "text/event-stream"}};
                r.body = body;
                r.chunkSize = 64;
                fake.push(r);
                INFO(fixtureName << " event " << li << " " << path.to_string() << " = " << value.dump());
                std::expected<ChatResponse, AiError> result = std::unexpected(AiError());
                REQUIRE_NOTHROW(result = call(fake));
                if (!result) {
                    CHECK(result.error().code != AiErrorCode::Cancelled); // a malformed event is an error, not a "cancel"
                }
                ++runs;
            }
        }
    }
    return runs;
}

/// The same for buffered JSON responses: `scripted` pushes the flow with the document at `index` replaced.
std::size_t fuzzJson(const std::vector<std::string>& docs, std::size_t index,
                     const std::function<std::expected<TranscribeResult, AiError>(FakeTransport&)>& call) {
    const Json doc = fixtureJson(docs[index]);
    std::vector<Json::json_pointer> paths;
    collectPaths(doc, Json::json_pointer(), paths);
    std::size_t runs = 0;
    for (const auto& path : paths) {
        for (const Json& value : substitutes()) {
            Json mutated = doc;
            mutated[path] = value;
            FakeTransport fake;
            for (std::size_t k = 0; k < docs.size(); ++k) {
                fake.pushJson(200, k == index ? mutated.dump() : fixture(docs[k]));
            }
            INFO(docs[index] << " " << path.to_string() << " = " << value.dump());
            std::expected<TranscribeResult, AiError> result = std::unexpected(AiError());
            REQUIRE_NOTHROW(result = call(fake));
            ++runs;
        }
    }
    return runs;
}

struct AudioFile {
    std::filesystem::path path = uniqueTempPath("wabcut-ai-net-security-audio", ".flac");
    AudioFile() { std::ofstream(path, std::ios::binary) << "fLaC-test-audio"; }
    ~AudioFile() { std::filesystem::remove(path); }
};

TranscribeRequest transcribeRequest(const AudioFile& audio) {
    TranscribeRequest r;
    r.audioFile = audio.path;
    r.mediaType = "audio/flac";
    r.audioMilliseconds = 6250;
    r.source.fingerprint = 0x1234;
    return r;
}

} // namespace

// --- P1-1 ---------------------------------------------------------------------------------------------------------

TEST_CASE("Gemini: an upload URL outside the Files API gets neither the key nor the video", "[ai][net][gemini][security]") {
    const char* refused[] = {
        "https://attacker.invalid/upload/v1beta/files?upload_id=X",                        // cross-origin
        "http://generativelanguage.googleapis.com/upload/v1beta/files?upload_id=X",         // HTTPS downgrade
        "https://localhost/upload/v1beta/files?upload_id=X",                                // localhost
        "https://127.0.0.1/upload/v1beta/files?upload_id=X",
        "https://[::1]/upload/v1beta/files?upload_id=X",
        "https://user:pw@generativelanguage.googleapis.com/upload/v1beta/files?upload_id=X", // userinfo
        "https://generativelanguage.googleapis.com@attacker.invalid/upload/v1beta/files",   // userinfo hiding the host
        "https://generativelanguage.googleapis.com:8443/upload/v1beta/files?upload_id=X",   // odd port
        "https://generativelanguage.googleapis.com.attacker.invalid/upload/v1beta/files",   // look-alike hosts
        "https://xgenerativelanguage.googleapis.com/upload/v1beta/files",
        "https://generativelanguage.googleapis.com\\@attacker.invalid/upload/v1beta/files", // backslash trick
        "https://generativelanguage.googleapis.com/v1beta/models?key=x",                     // not the upload endpoint
        "/upload/v1beta/files?upload_id=X",                                                   // relative
        "javascript:alert(1)",
    };
    for (const char* url : refused) {
        INFO(url);
        const auto [result, requests] = uploadWith(url);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == AiErrorCode::ProviderError);
        CHECK(result.error().message.find("refused to upload the video") != std::string::npos);
        CHECK(result.error().message.find("upload_id") == std::string::npos); // the session id is not echoed
        REQUIRE(requests.size() == 1); // only the start request (to the configured endpoint) was sent
        CHECK(requests[0].url == "https://generativelanguage.googleapis.com/upload/v1beta/files");
        CHECK(requests[0].bodyFile.empty());
    }

    // The documented upload URL (with or without the explicit default port) works and carries the key and video.
    for (const char* url : {"https://generativelanguage.googleapis.com/upload/v1beta/files?upload_id=OK&upload_protocol=resumable",
                            "https://generativelanguage.googleapis.com:443/upload/v1beta/files?upload_id=OK"}) {
        INFO(url);
        const auto [result, requests] = uploadWith(url);
        REQUIRE(result);
        REQUIRE(requests.size() == 3);
        CHECK(requests[1].url == url);
        CHECK(findHeader(requests[1].headers, "x-goog-api-key") == "test-key-0123456789");
        CHECK_FALSE(requests[1].bodyFile.empty());
    }
}

TEST_CASE("Gemini: a Files API file name that would leave the API's path is refused", "[ai][net][gemini][security]") {
    VideoFile video;
    FakeTransport fake;
    ScriptedResponse started;
    started.headers = {{"x-goog-upload-url", "https://generativelanguage.googleapis.com/upload/v1beta/files?upload_id=OK"}};
    fake.push(started);
    fake.pushJson(200, R"({"file":{"name":"../../v1beta/models/x?key=","state":"PROCESSING"}})");
    net::AdapterOptions options;
    options.sleeper = [](std::chrono::milliseconds, std::stop_token) { return true; };
    auto chat = net::makeGeminiInteractionsChat(context("gemini-3.8-flash", fake), options);
    ChatRequest request = hello();
    VideoPart part;
    part.localFile = video.path;
    request.messages[0].parts.push_back(part);
    const auto result = chat->chat(request, {}, {});
    REQUIRE_FALSE(result);
    CHECK(result.error().message == "the Files API returned an invalid file name");
    CHECK(fake.requests().size() == 2); // no poll was sent
}

// --- P1-2 ---------------------------------------------------------------------------------------------------------

TEST_CASE("Chat adapters: every field may arrive with another type, nothing escapes", "[ai][net][security][fuzz]") {
    auto chatWith = [](std::function<std::unique_ptr<IChatModel>(FakeTransport&)> make) {
        return [make](FakeTransport& fake) {
            auto chat = make(fake);
            return chat->chat(agentRequest(), {}, {});
        };
    };
    ModelRegistry registry = ModelRegistry::builtin();
    ModelSpec local;
    local.id = "qwen3.5:9b";
    local.provider = "ollama";
    REQUIRE(registry.addModel(local));
    const ProviderSpec& ollama = *registry.provider("ollama");
    const auto anthropic = chatWith([](FakeTransport& f) { return net::makeAnthropicChat(context("claude-sonnet-5-5", f)); });
    const auto openai = chatWith([](FakeTransport& f) { return net::makeOpenAiResponsesChat(context("gpt-6.1-sol", f)); });
    const auto chat = chatWith([&](FakeTransport& f) {
        return net::makeChatCompletionsChat(ProviderContext{ollama, *registry.model("qwen3.5:9b"), ollama.baseUrl, Secret(), f});
    });
    const auto gemini = chatWith([](FakeTransport& f) { return net::makeGeminiInteractionsChat(context("gemini-3.8-flash", f)); });

    std::size_t runs = 0;
    for (const char* f : {"anthropic/text_stream.sse", "anthropic/tool_stream.sse", "anthropic/error_stream.sse",
                          "agent/anthropic/turn3.sse"}) {
        runs += fuzzSse(f, anthropic);
    }
    for (const char* f : {"openai/text_stream.sse", "openai/tool_stream.sse", "openai/failed_stream.sse",
                          "openai/incomplete_stream.sse", "agent/openai/turn2.sse"}) {
        runs += fuzzSse(f, openai);
    }
    for (const char* f : {"chat/text_stream.sse", "chat/tool_stream.sse", "chat/error_stream.sse", "chat/ollama_tool_stream.sse"}) {
        runs += fuzzSse(f, chat);
    }
    for (const char* f : {"gemini/text_stream.sse", "gemini/tool_stream.sse", "gemini/error_stream.sse", "agent/gemini/turn3.sse"}) {
        runs += fuzzSse(f, gemini);
    }
    CHECK(runs > 3000);

    // The reviewer's probe, exactly: {"id":null} in a Chat Completions chunk.
    FakeTransport fake;
    fake.push(sseBody("data: {\"id\":null,\"choices\":[]}\n\ndata: [DONE]\n\n"));
    auto probe = net::makeChatCompletionsChat(ProviderContext{ollama, *registry.model("qwen3.5:9b"), ollama.baseUrl, Secret(), fake});
    std::expected<ChatResponse, AiError> result = std::unexpected(AiError());
    REQUIRE_NOTHROW(result = probe->chat(hello(), {}, {}));
    CHECK(result); // a missing id is just absent
}

TEST_CASE("Transcription adapters: every field may arrive with another type, nothing escapes", "[ai][net][security][fuzz]") {
    AudioFile audio;
    std::size_t runs = 0;
    runs += fuzzJson({"deepgram/response.json"}, 0, [&](FakeTransport& f) {
        return net::makeDeepgramTranscriber(context("nova-3", f))->transcribe(transcribeRequest(audio), {}, {});
    });
    runs += fuzzJson({"elevenlabs/response.json"}, 0, [&](FakeTransport& f) {
        return net::makeElevenLabsTranscriber(context("scribe_v2", f))->transcribe(transcribeRequest(audio), {}, {});
    });
    const std::vector<std::string> assembly = {"assemblyai/upload.json", "assemblyai/queued.json", "assemblyai/completed.json"};
    for (std::size_t i = 0; i < assembly.size(); ++i) {
        runs += fuzzJson(assembly, i, [&](FakeTransport& f) {
            net::AdapterOptions options;
            options.sleeper = [](std::chrono::milliseconds, std::stop_token) { return true; };
            options.maxPollTime = std::chrono::minutes(1); // scripted responses run out long before
            return net::makeAssemblyAiTranscriber(context("universal-3-5-pro", f), options)
                ->transcribe(transcribeRequest(audio), {}, {});
        });
    }
    CHECK(runs > 500);

    // A transcript id that would change the poll URL's path is refused before the poll.
    FakeTransport fake;
    fake.push(json(200, "assemblyai/upload.json"));
    fake.pushJson(200, R"({"id":"../../v2/upload?x=","status":"queued"})");
    net::AdapterOptions options;
    options.sleeper = [](std::chrono::milliseconds, std::stop_token) { return true; };
    const auto r = net::makeAssemblyAiTranscriber(context("universal-3-5-pro", fake), options)->transcribe(transcribeRequest(audio), {}, {});
    REQUIRE_FALSE(r);
    CHECK(r.error().message == "AssemblyAI returned no valid transcript id");
    CHECK(fake.requests().size() == 2);
}

// --- P2-9 ---------------------------------------------------------------------------------------------------------

TEST_CASE("Chat adapters: huge tool arguments and endless text are cut off with an explicit error", "[ai][net][security][limits]") {
    SECTION("tool-call arguments past 1 MiB (OpenAI Responses)") {
        std::string body =
            "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"type\":\"function_call\","
            "\"call_id\":\"c1\",\"name\":\"timeline_rippleDelete\",\"arguments\":\"\"}}\n\n";
        const std::string fragment = std::string(60 * 1024, 'a');
        for (int i = 0; i < 20; ++i) { // 1.2 MB of argument fragments, each event small
            body += "data: {\"type\":\"response.function_call_arguments.delta\",\"output_index\":0,\"delta\":\"" + fragment + "\"}\n\n";
        }
        FakeTransport fake;
        fake.push(sseBody(body));
        const auto result = net::makeOpenAiResponsesChat(context("gpt-6.1-sol", fake))->chat(hello(), {}, {});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == AiErrorCode::PayloadTooLarge);
        CHECK(result.error().message.find("timeline_rippleDelete") != std::string::npos);
    }
    SECTION("text past 8 MiB (Anthropic)") {
        std::string body = "data: {\"type\":\"message_start\",\"message\":{\"id\":\"m\",\"usage\":{}}}\n\n"
                           "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n";
        const std::string event = "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"" +
                                  std::string(500 * 1024, 'b') + "\"}}\n\n";
        for (int i = 0; i < 18; ++i) { // 9 MB of text in 500 KB events
            body += event;
        }
        FakeTransport fake;
        ScriptedResponse r = sseBody(body);
        r.chunkSize = 64 * 1024;
        fake.push(r);
        std::size_t streamed = 0;
        const auto result = net::makeAnthropicChat(context("claude-sonnet-5-5", fake))->chat(hello(), [&](const ChatEvent& e) {
            if (const auto* t = std::get_if<TextDelta>(&e)) {
                streamed += t->text.size();
            }
        }, {});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == AiErrorCode::PayloadTooLarge);
        CHECK(streamed <= std::size_t{8} * 1024 * 1024);
    }
    SECTION("an endless line through the adapter") {
        FakeTransport fake;
        ScriptedResponse r = sseBody("data: {\"type\":\"message_start\",\"message\":{\"id\":\"" + std::string(2 * 1024 * 1024, 'c'));
        r.chunkSize = 64 * 1024;
        fake.push(r);
        const auto result = net::makeAnthropicChat(context("claude-sonnet-5-5", fake))->chat(hello(), {}, {});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == AiErrorCode::PayloadTooLarge);
    }
}

TEST_CASE("Gemini video through the platform: the lock turned on during the upload's start stops the upload (wave 3, P1-5)",
          "[ai][net][gemini][security][lock]") {
    VideoFile video;
    const ModelRegistry& registry = ModelRegistry::builtin();
    ProviderRegistry providers(registry);
    net::registerCloudAdapters(providers);
    MemorySecretStore store;
    KeyRing keys(store, [](std::string_view) { return std::optional<std::string>(); });
    ConsentLedger consent;
    SpendGuard spend;
    RequestLog log;
    FakeTransport fake;
    AiPlatform platform(providers, keys, consent, spend, log, fake);
    const ModelSpec* model = registry.model("gemini-3.8-flash");
    REQUIRE(model != nullptr);
    const ProviderSpec& google = *registry.provider(model->provider);
    REQUIRE(keys.save(google, Secret("AIza-wave3-fake-key-0123456789abcdef")));
    consent.grantAlways(google, Capability::Chat, "2026-10-11T08:00:00Z");

    // The Files API start answers; while it was pending, the user locked the project.
    ScriptedResponse started;
    started.headers = {{"x-goog-upload-url", "https://generativelanguage.googleapis.com/upload/v1beta/files?upload_id=OK"}};
    started.before = [&consent] { consent.setProjectLock(true); };
    fake.push(started);
    fake.push(json(200, "gemini/file_active.json"));
    fake.push(sse("gemini/text_stream.sse"));

    auto chat = platform.chat("gemini-3.8-flash");
    REQUIRE(chat);
    ChatRequest request = hello();
    VideoPart part;
    part.localFile = video.path;
    request.messages[0].parts.push_back(part);
    const auto result = (*chat)->chat(request, {}, {});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == AiErrorCode::Offline);
    const auto sent = fake.requests();
    REQUIRE(sent.size() == 1); // only the start: the video upload never left
    CHECK(sent[0].bodyFile.empty());
    for (const HttpRequest& r : sent) {
        CHECK(r.bodyFile != video.path);
    }
}