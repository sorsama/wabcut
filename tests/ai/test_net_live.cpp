// Optional live smoke tests against the real providers. Each runs only when its key is in the environment:
//   WABCUT_LIVE_ANTHROPIC_KEY  WABCUT_LIVE_OPENAI_KEY  WABCUT_LIVE_GEMINI_KEY
//   WABCUT_LIVE_DEEPGRAM_KEY   WABCUT_LIVE_ELEVENLABS_KEY  WABCUT_LIVE_ASSEMBLYAI_KEY
// Without the variable a test passes without doing anything. CI never sets them; they cost real money (a few
// tokens) and need the network. Run one with:  ctest --preset win-msvc -L ai_net -R live --output-on-failure

#include <wabcut/ai/Secrets.h>
#include <wabcut/ai_net/Adapters.h>
#include <wabcut/ai_net/Network.h>

#include <QCoreApplication>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut::ai;

namespace {

void ensureCoreApp() {
    if (!QCoreApplication::instance()) {
        static int argc = 1;
        static char name[] = "wabcut_ai_net_tests";
        static char* argv[] = {name, nullptr};
        static QCoreApplication app(argc, argv);
    }
}

std::optional<std::string> liveKey(const char* variable) {
    auto key = systemEnv(variable);
    if (!key) {
        SUCCEED(std::string(variable) + " is not set: live test skipped");
    }
    return key;
}

ProviderContext liveContext(const std::string& modelId, IHttpTransport& transport, std::string key) {
    const ModelSpec& model = *ModelRegistry::builtin().model(modelId);
    const ProviderSpec& provider = *ModelRegistry::builtin().provider(model.provider);
    return ProviderContext{provider, model, provider.baseUrl, Secret(std::move(key)), transport};
}

void liveChat(const char* variable, const std::string& modelId,
              std::unique_ptr<IChatModel> (*make)(ProviderContext)) {
    auto key = liveKey(variable);
    if (!key) {
        return;
    }
    ensureCoreApp();
    RequestLog log;
    net::NetworkStack stack(log);
    const ProviderSpec& provider = *ModelRegistry::builtin().provider(ModelRegistry::builtin().model(modelId)->provider);
    REQUIRE(validateKey(stack.transport(), provider, "", Secret(*key), {}));
    auto chat = make(liveContext(modelId, stack.transport(), *key));
    ChatRequest request;
    request.messages.push_back(ChatMessage::user("Reply with exactly the word: ok"));
    request.maxOutputTokens = 256;
    std::string streamed;
    auto r = chat->chat(request, [&](const ChatEvent& e) {
        if (const auto* t = std::get_if<TextDelta>(&e)) {
            streamed += t->text;
        }
    }, {});
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    CHECK_FALSE(r->message.text().empty());
    CHECK(streamed == r->message.text());
    CHECK(r->usage.outputTokens > 0);
}

void liveKeyCheck(const char* variable, const char* providerId) {
    auto key = liveKey(variable);
    if (!key) {
        return;
    }
    ensureCoreApp();
    RequestLog log;
    net::NetworkStack stack(log);
    auto valid = validateKey(stack.transport(), *ModelRegistry::builtin().provider(providerId), "", Secret(*key), {});
    INFO((valid ? std::string() : valid.error().message));
    CHECK(valid);
    CHECK(validateKey(stack.transport(), *ModelRegistry::builtin().provider(providerId), "", Secret("wabcut-invalid-key-0000"), {})
              .error()
              .code == AiErrorCode::Unauthorized);
}

} // namespace

TEST_CASE("Live: Anthropic Messages", "[ai][net][live]") {
    liveChat("WABCUT_LIVE_ANTHROPIC_KEY", "claude-haiku-5-5", [](ProviderContext c) { return net::makeAnthropicChat(std::move(c)); });
}

TEST_CASE("Live: OpenAI Responses", "[ai][net][live]") {
    liveChat("WABCUT_LIVE_OPENAI_KEY", "gpt-6-luna", [](ProviderContext c) { return net::makeOpenAiResponsesChat(std::move(c)); });
}

TEST_CASE("Live: Gemini Interactions", "[ai][net][live]") {
    liveChat("WABCUT_LIVE_GEMINI_KEY", "gemini-3.5-flash-lite",
             [](ProviderContext c) { return net::makeGeminiInteractionsChat(std::move(c)); });
}

TEST_CASE("Live: transcription provider keys", "[ai][net][live]") {
    liveKeyCheck("WABCUT_LIVE_DEEPGRAM_KEY", "deepgram");
    liveKeyCheck("WABCUT_LIVE_ELEVENLABS_KEY", "elevenlabs");
    liveKeyCheck("WABCUT_LIVE_ASSEMBLYAI_KEY", "assemblyai");
}
