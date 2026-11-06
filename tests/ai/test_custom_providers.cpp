// r07 (A1): custom providers and provider-scoped model ids end to end through the guarded platform: the adapter sends
// the provider's own model name (ModelSpec::apiId), the key in the provider's own header, its extra headers and the
// per-feature path; consent, offline mode and the project's no-cloud lock apply to a custom cloud provider like any
// other, and a custom provider on a loopback address counts as local. Recorded fixtures, FakeTransport: no network.

#include "NetTestSupport.h"

#include <wabcut/ai/ConsentLedger.h>
#include <wabcut/ai/Provider.h>
#include <wabcut/ai/RequestLog.h>
#include <wabcut/ai/Secrets.h>
#include <wabcut/ai/SpendGuard.h>
#include <wabcut/ai_net/Adapters.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut::ai;
using namespace wabcut::ai::test;

namespace {

ChatRequest hello() {
    ChatRequest r;
    r.messages.push_back(ChatMessage::user("Say hello."));
    r.maxOutputTokens = 64;
    return r;
}

/// A custom OpenAI-compatible provider as AiController builds it from the dialog.
ProviderSpec customProvider(std::string id, std::string baseUrl) {
    ProviderSpec p;
    p.id = std::move(id);
    p.name = "My gateway";
    p.api = "chat-completions";
    p.baseUrl = std::move(baseUrl);
    p.local = isLoopbackUrl(p.baseUrl);
    p.custom = true;
    p.protocol = "openai";
    p.keyCheck.path = "/models";
    return p;
}

ModelSpec customChat(std::string id, std::string apiId, std::string endpoint) {
    ModelSpec m;
    m.id = std::move(id);
    m.apiId = std::move(apiId);
    m.name = m.apiId.empty() ? m.id : m.apiId;
    m.api = "chat-completions";
    m.endpoint = std::move(endpoint);
    m.caps.capabilities = {Capability::Chat, Capability::Translate};
    m.caps.toolsOnChat = true;
    m.caps.forcedToolChoice = false;
    m.prices = {PriceRule{}};
    return m;
}

struct Platform {
    explicit Platform(ModelRegistry r) : registry(std::move(r)), providers(registry) {
        net::registerCloudAdapters(providers);
    }
    ModelRegistry registry;
    ProviderRegistry providers;
    MemorySecretStore store;
    KeyRing keys{store, [](std::string_view) { return std::optional<std::string>(); }};
    ConsentLedger consent;
    SpendGuard spend;
    RequestLog log;
    FakeTransport transport;
    AiPlatform platform{providers, keys, consent, spend, log, transport};
};

} // namespace

TEST_CASE("A custom cloud provider sends its own model name key header and path", "[ai][net][custom][r07]") {
    ModelRegistry registry = ModelRegistry::builtin();
    ProviderSpec gateway = customProvider("custom-gateway", "https://llm.example.test/v1");
    gateway.auth = AuthScheme::Header;
    gateway.authHeader = "x-gateway-key";
    gateway.extraHeaders = {{"X-Org", "studio-7"}};
    // "openai/gpt-oss-120b" is already a name other hosts use: scoped id, apiId = the name.
    REQUIRE(registry.addCustomProvider(gateway, {customChat("custom-gateway:openai/gpt-oss-120b", "openai/gpt-oss-120b",
                                                            "/v2/chat/completions")}));
    Platform p(std::move(registry));
    p.providers.setPreference(Capability::Chat, {"custom-gateway:openai/gpt-oss-120b"});
    const ProviderSpec& provider = *p.registry.provider("custom-gateway");

    // The routing picks it once its key is there.
    CHECK(p.platform.resolve(Capability::Chat).error().code == AiErrorCode::NoCredentials);
    REQUIRE(p.keys.save(provider, Secret("gateway-key-0123456789")));
    auto resolved = p.platform.resolve(Capability::Chat);
    REQUIRE(resolved);
    CHECK(resolved->front().model->id == "custom-gateway:openai/gpt-oss-120b");

    // Cloud: consent first, like every built-in provider.
    auto unconsented = p.platform.chat("custom-gateway:openai/gpt-oss-120b");
    REQUIRE(unconsented);
    CHECK((*unconsented)->chat(hello(), {}, {}).error().code == AiErrorCode::ConsentRequired);
    CHECK(p.transport.requests().empty());

    p.transport.push(sse("chat/text_stream.sse"));
    auto chat = p.platform.chat("custom-gateway:openai/gpt-oss-120b", CallOptions{true});
    REQUIRE(chat);
    CHECK((*chat)->spec().id == "openai/gpt-oss-120b"); // the adapter sees the provider's name
    REQUIRE((*chat)->chat(hello(), {}, {}));
    const HttpRequest sent = p.transport.requests().at(0);
    CHECK(sent.url == "https://llm.example.test/v1/v2/chat/completions");
    CHECK(findHeader(sent.headers, "x-gateway-key") == "gateway-key-0123456789");
    CHECK_FALSE(findHeader(sent.headers, "Authorization"));
    CHECK(findHeader(sent.headers, "X-Org") == "studio-7");
    CHECK(requestBody(sent)["model"] == "openai/gpt-oss-120b");

    // Offline mode and the project's no-cloud lock stop it before anything is sent.
    p.consent.setOfflineMode(true);
    CHECK(p.platform.resolve(Capability::Chat).error().code == AiErrorCode::Offline);
    CHECK_FALSE((*chat)->chat(hello(), {}, {}));
    p.consent.setOfflineMode(false);
    p.consent.setProjectLock(true);
    CHECK_FALSE(p.platform.resolve(Capability::Chat));
    CHECK_FALSE((*chat)->chat(hello(), {}, {}));
    CHECK(p.transport.requests().size() == 1);
}

TEST_CASE("A custom provider on a loopback address counts as local", "[ai][net][custom][r07]") {
    ModelRegistry registry = ModelRegistry::builtin();
    ProviderSpec server = customProvider("custom-office", "http://127.0.0.1:8123/v1");
    server.auth = AuthScheme::None;
    server.keyRequired = false;
    REQUIRE(registry.addCustomProvider(server, {customChat("office-llm", "", "/chat/completions")}));
    Platform p(std::move(registry));
    p.providers.setPreference(Capability::Chat, {"office-llm"});
    const ProviderSpec& provider = *p.registry.provider("custom-office");
    CHECK(provider.local);
    CHECK_FALSE(isCloud(provider, provider.baseUrl));
    CHECK(p.consent.state(provider, provider.baseUrl, Capability::Chat) == ConsentState::NotNeeded);

    // No consent prompt, and it keeps working offline and under the no-cloud lock: nothing leaves the computer.
    p.consent.setOfflineMode(true);
    p.consent.setProjectLock(true);
    auto resolved = p.platform.resolve(Capability::Chat);
    REQUIRE(resolved);
    CHECK(resolved->front().model->id == "office-llm");
    p.transport.push(sse("chat/text_stream.sse"));
    auto chat = p.platform.chat("office-llm");
    REQUIRE(chat);
    REQUIRE((*chat)->chat(hello(), {}, {}));
    const HttpRequest sent = p.transport.requests().at(0);
    CHECK(sent.url == "http://127.0.0.1:8123/v1/chat/completions");
    CHECK_FALSE(findHeader(sent.headers, "Authorization"));
    CHECK(requestBody(sent)["model"] == "office-llm");

    // The same provider pointed at another host is cloud again.
    ProviderSpec moved = provider;
    moved.baseUrl = "http://10.0.0.5:8123/v1";
    moved.local = isLoopbackUrl(moved.baseUrl);
    CHECK(isCloud(moved, moved.baseUrl));
}

TEST_CASE("A catalogue host with a provider-scoped id sends the vendor's model name", "[ai][net][custom][r07]") {
    Platform p(ModelRegistry::builtin());
    const ProviderSpec& together = *p.registry.provider("together");
    REQUIRE(p.keys.save(together, Secret("together-key-0123456789")));
    p.providers.setPreference(Capability::Chat, {"together:openai/gpt-oss-120b"});
    p.transport.push(sse("chat/text_stream.sse"));
    auto chat = p.platform.chat("together:openai/gpt-oss-120b", CallOptions{true});
    REQUIRE(chat);
    REQUIRE((*chat)->chat(hello(), {}, {}));
    const HttpRequest sent = p.transport.requests().at(0);
    CHECK(sent.url == "https://api.together.ai/v1/chat/completions");
    CHECK(findHeader(sent.headers, "Authorization") == "Bearer together-key-0123456789");
    CHECK(requestBody(sent)["model"] == "openai/gpt-oss-120b");
    // The request log names the provider and the model the provider was asked for.
    const auto entries = p.log.entries();
    REQUIRE_FALSE(entries.empty());
    CHECK(entries.back().provider == "together");
}
