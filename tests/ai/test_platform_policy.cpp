// AI platform: keys (Secret, stores, KeyRing, validation), spend caps, consent and locks, and AiPlatform's guarded
// calls (consent -> reserve -> call -> settle -> log) with fallback resolution. No network: FakeTransport.

#include "FakeTransport.h"

#include <wabcut/ai/AiJob.h>
#include <wabcut/ai/ConsentLedger.h>
#include <wabcut/ai/Provider.h>
#include <wabcut/ai/Redact.h>
#include <wabcut/ai/Secrets.h>
#include <wabcut/ai/SpendGuard.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <map>
#include <stdexcept>
#include <thread>

using namespace wabcut::ai;
using namespace std::chrono;

namespace {

year_month_day ymd(int y, unsigned m, unsigned d) { return year_month_day{year(y), month(m), day(d)}; }

const ProviderSpec& provider(std::string_view id) { return *ModelRegistry::builtin().provider(id); }

/// A chat adapter for api "fake" that answers with fixed usage.
class FakeChat final : public IChatModel {
public:
    FakeChat(ModelSpec spec, std::string key, int* calls) : m_spec(std::move(spec)), m_key(std::move(key)), m_calls(calls) {}
    const ModelSpec& spec() const override { return m_spec; }
    std::expected<ChatResponse, AiError> chat(const ChatRequest&, const ChatEventHandler& onEvent, std::stop_token) override {
        ++*m_calls;
        if (onEvent) {
            onEvent(TextDelta{0, "ok"});
        }
        ChatResponse r;
        r.message = ChatMessage::assistant("ok, key " + m_key);
        r.usage.inputTokens = 1000;
        r.usage.outputTokens = 500;
        return r;
    }

private:
    ModelSpec m_spec;
    std::string m_key;
    int* m_calls;
};

ModelRegistry testRegistry() {
    auto r = ModelRegistry::fromJson(R"({"schemaVersion":1,"providers":[
        {"id":"cloud","name":"Cloud","api":"fake","baseUrl":"https://cloud.test","auth":"bearer","envVar":"WABCUT_CLOUD_TEST_KEY",
         "keyCheck":{"path":"/v1/models"},"termsUrl":"https://cloud.test/terms","dataPolicy":"kept 30 days"},
        {"id":"local","name":"Local","api":"fake","baseUrl":"http://127.0.0.1:9931/v1","keyRequired":false,"local":true}],
      "models":[
        {"id":"cloud-model","provider":"cloud","capabilities":{"chat":true},"prices":[{"inputPerMTok":2,"outputPerMTok":10}]},
        {"id":"local-model","provider":"local","capabilities":{"chat":true}}]})");
    REQUIRE(r);
    return std::move(*r);
}

} // namespace

TEST_CASE("Secret: masked display, explicit copies, wiped on move", "[ai][platform][secrets]") {
    Secret key("sk-test-0123456789abcd");
    CHECK(key.last4() == "abcd");
    CHECK(key.masked() == "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2" "abcd");
    CHECK(Secret("short").last4().empty()); // would reveal half the key
    Secret moved = std::move(key);
    CHECK(key.empty()); // NOLINT(bugprone-use-after-move): the moved-from secret is wiped
    CHECK(moved.reveal() == "sk-test-0123456789abcd");
    CHECK(moved.clone().reveal() == moved.reveal());
}

TEST_CASE("KeyRing: environment first, then the store; cached; registered with the redactor", "[ai][platform][secrets]") {
    MemorySecretStore store;
    std::map<std::string, std::string> env;
    Redactor redactor;
    KeyRing ring(store, [&](std::string_view name) -> std::optional<std::string> {
        auto it = env.find(std::string(name));
        return it == env.end() ? std::nullopt : std::optional<std::string>(it->second);
    }, &redactor);
    const ProviderSpec& anthropic = provider("anthropic");

    CHECK(ring.get(anthropic).error().code == AiErrorCode::NoCredentials);
    CHECK_FALSE(ring.status(anthropic).hasKey);

    REQUIRE(ring.save(anthropic, Secret("sk-ant-stored-0123456789")));
    CHECK(ring.get(anthropic)->reveal() == "sk-ant-stored-0123456789");
    CHECK(ring.status(anthropic).source == SecretSource::Session); // MemorySecretStore is not persistent
    CHECK(ring.status(anthropic).masked.ends_with("6789"));
    CHECK(redactor.redact("key sk-ant-stored-0123456789") == "key [REDACTED]");

    env["WABCUT_ANTHROPIC_API_KEY"] = "sk-ant-from-env-9876543210";
    CHECK(ring.get(anthropic)->reveal() == "sk-ant-from-env-9876543210");
    CHECK(ring.status(anthropic).source == SecretSource::Environment);
    env.clear();

    REQUIRE(ring.remove(anthropic));
    CHECK(ring.get(anthropic).error().code == AiErrorCode::NoCredentials);
    CHECK(redactor.redact("sk-ant-stored-0123456789").find("stored") == std::string::npos); // key shape still masked
    CHECK_FALSE(ring.save(anthropic, Secret("")));
}

TEST_CASE("PlaintextFileSecretStore is a working opt-in store", "[ai][platform][secrets]") {
    const auto file = test::uniqueTempPath("wabcut-ai-keys") / "keys.json";
    std::filesystem::remove_all(file.parent_path());
    PlaintextFileSecretStore store(file);
    CHECK_FALSE(store.read("openai").value());
    REQUIRE(store.write("openai", Secret("sk-openai-abcdefghijkl")));
    REQUIRE(store.write("deepgram", Secret("dg-0123456789")));
    CHECK(store.read("openai").value()->reveal() == "sk-openai-abcdefghijkl");
    REQUIRE(store.remove("openai"));
    CHECK_FALSE(store.read("openai").value());
    CHECK(store.read("deepgram").value()->reveal() == "dg-0123456789");
    CHECK(store.persistent());
    std::filesystem::remove_all(file.parent_path());
}

TEST_CASE("Keys are validated with the provider's cheap authenticated call", "[ai][platform][secrets]") {
    test::FakeTransport fake;
    const ProviderSpec& anthropic = provider("anthropic");
    fake.pushJson(200, R"({"data":[{"type":"model","id":"claude-sonnet-5-5"}],"has_more":true})");
    REQUIRE(validateKey(fake, anthropic, "", Secret("sk-ant-valid-0123456789"), {}));
    const auto request = fake.requests().at(0);
    CHECK(request.method == "GET");
    CHECK(request.url == "https://api.anthropic.com/v1/models?limit=1");
    CHECK(findHeader(request.headers, "x-api-key") == "sk-ant-valid-0123456789");
    CHECK(findHeader(request.headers, "anthropic-version") == "2023-06-01");

    fake.pushJson(401, R"({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})");
    CHECK(validateKey(fake, anthropic, "", Secret("sk-ant-bad-0123456789"), {}).error().code == AiErrorCode::Unauthorized);

    fake.pushJson(200, R"({"data":[]})");
    REQUIRE(validateKey(fake, provider("deepgram"), "", Secret("dg-key-0123456789"), {}));
    CHECK(fake.requests().back().url == "https://api.deepgram.com/v1/auth/token");
    CHECK(findHeader(fake.requests().back().headers, "Authorization") == "Token dg-key-0123456789");

    fake.pushJson(200, R"({"data":[]})");
    REQUIRE(validateKey(fake, provider("assemblyai"), "", Secret("aai-key-0123456789"), {}));
    CHECK(findHeader(fake.requests().back().headers, "Authorization") == "aai-key-0123456789");

    fake.pushJson(200, R"({"object":"list","data":[]})");
    REQUIRE(validateKey(fake, provider("ollama"), "http://192.168.1.20:11434/v1", Secret(), {}));
    CHECK(fake.requests().back().url == "http://192.168.1.20:11434/v1/models");
    CHECK_FALSE(findHeader(fake.requests().back().headers, "Authorization"));

    CHECK(joinUrl("https://a.test/v1/", "/models") == "https://a.test/v1/models");
    CHECK(joinUrl("https://a.test/v1", "models") == "https://a.test/v1/models");
    CHECK(joinUrl("https://a.test/v1", "https://b.test/x") == "https://b.test/x");
}

TEST_CASE("SpendGuard enforces monthly caps with reservations", "[ai][platform][spend]") {
    SpendGuard guard;
    int changes = 0;
    guard.setOnChanged([&] { ++changes; });
    const auto oct = ymd(2026, 10, 11);
    guard.setMonthlyCap("anthropic", 1'000'000); // $1.00

    auto a = guard.reserve("anthropic", 600'000, oct);
    REQUIRE(a);
    auto b = guard.reserve("anthropic", 500'000, oct); // 0.60 + 0.50 > 1.00
    REQUIRE_FALSE(b);
    CHECK(b.error().code == AiErrorCode::SpendCapExceeded);
    CHECK(b.error().message.find("$1.00") != std::string::npos);
    CHECK(guard.reserve("openai", 50'000'000, oct)); // no cap

    guard.settle(*a, 120'000); // the call cost much less than its ceiling
    CHECK(guard.spent("anthropic", year_month{year(2026), month(10)}) == 120'000);
    CHECK(guard.reserved("anthropic") == 0);
    auto c = guard.reserve("anthropic", 880'000, oct);
    REQUIRE(c);
    guard.release(*c);
    CHECK_FALSE(guard.reserve("anthropic", 880'001, oct));

    // A new month starts from zero.
    CHECK(guard.reserve("anthropic", 900'000, ymd(2026, 11, 1)));

    guard.record("deepgram", 25'800, oct);
    const auto summary = guard.summary(year_month{year(2026), month(10)});
    REQUIRE(summary.size() == 2);
    CHECK(summary[0].provider == "anthropic");
    CHECK(summary[0].cap == 1'000'000);
    CHECK(summary[1].spent == 25'800);

    SpendGuard restored;
    REQUIRE(restored.loadJson(guard.toJson()));
    CHECK(restored.monthlyCap("anthropic") == 1'000'000);
    CHECK(restored.spent("anthropic", year_month{year(2026), month(10)}) == 120'000);
    CHECK(changes >= 3);
    CHECK_FALSE(restored.loadJson(Json::object()));
}

TEST_CASE("Cost estimates: conservative ceilings from the registry's prices", "[ai][platform][spend]") {
    const ModelSpec& sonnet = *ModelRegistry::builtin().model("claude-sonnet-5-5");
    ChatRequest request;
    request.system = std::string(4000, 'x');                 // ~1000 tokens
    request.messages.push_back(ChatMessage::user(std::string(4000, 'y')));
    request.messages.back().parts.push_back(ImagePart{"image/png", {1, 2, 3}, ""});
    request.maxOutputTokens = 2000;
    const auto e = estimateChatCost(sonnet, request, ymd(2026, 10, 11));
    CHECK(e.priced);
    CHECK(e.inputTokens == 1000 + 1000 + 4 + 1600);
    CHECK(e.ceiling == e.inputTokens * 2 + 2000 * 10); // micro-USD: $2/M in, $10/M out
    CHECK(e.expected < e.ceiling);

    const auto t = estimateTranscriptionCost(*ModelRegistry::builtin().model("scribe_v2"), 3'600'000, ymd(2026, 10, 11));
    CHECK(t.ceiling == 220'020); // 3667 micro-USD per minute x 60
    CHECK(t.expected == t.ceiling);
}

TEST_CASE("Consent: first use per provider x capability, offline and project locks", "[ai][platform][consent]") {
    ConsentLedger ledger;
    const ProviderSpec& deepgram = provider("deepgram");
    const ProviderSpec& ollama = provider("ollama");

    CHECK(ledger.state(deepgram, "", Capability::Transcribe) == ConsentState::NeedsPrompt);
    CHECK(ledger.authorize(deepgram, "", Capability::Transcribe).error().code == AiErrorCode::ConsentRequired);
    CHECK(ledger.authorize(deepgram, "", Capability::Transcribe, /*allowOnce=*/true));
    CHECK(ledger.state(ollama, "", Capability::Chat) == ConsentState::NotNeeded);
    CHECK(ledger.state(ollama, "http://10.1.2.3:11434/v1", Capability::Chat) == ConsentState::NeedsPrompt);

    ledger.grantAlways(deepgram, Capability::Transcribe, "2026-10-11T08:00:00Z");
    CHECK(ledger.state(deepgram, "", Capability::Transcribe) == ConsentState::Granted);
    CHECK(ledger.state(deepgram, "", Capability::Chat) == ConsentState::NeedsPrompt);

    ledger.setProjectLock(true);
    CHECK(ledger.state(deepgram, "", Capability::Transcribe) == ConsentState::Blocked);
    CHECK(ledger.authorize(deepgram, "", Capability::Transcribe, true).error().code == AiErrorCode::Offline);
    CHECK(ledger.authorize(ollama, "", Capability::Chat)); // local stays available under the lock
    ledger.setProjectLock(false);
    ledger.setOfflineMode(true);
    CHECK_FALSE(ledger.cloudAllowed());
    CHECK(ledger.authorize(deepgram, "", Capability::Transcribe).error().code == AiErrorCode::Offline);

    ConsentLedger restored;
    REQUIRE(restored.loadJson(ledger.toJson()));
    CHECK(restored.offlineMode());
    CHECK(restored.records() == ledger.records());
    CHECK_FALSE(restored.projectLock()); // the project lock belongs to the project
    restored.revoke("deepgram", Capability::Transcribe);
    CHECK(restored.records().empty());

    const auto prompt = makeConsentPrompt(deepgram, "", Capability::Transcribe, "audio of 3 clips, 12 min", {});
    CHECK(prompt.destination == "api.deepgram.com");
    CHECK(prompt.termsUrl == deepgram.termsUrl);
    CHECK(prompt.payload == "audio of 3 clips, 12 min");

    CHECK(isLoopbackUrl("http://localhost:1234/v1"));
    CHECK(isLoopbackUrl("http://127.0.0.1:9931/v1"));
    CHECK(isLoopbackUrl("http://[::1]:8080/v1"));
    CHECK(isLoopbackUrl("http://user@127.1.2.3/x"));
    CHECK_FALSE(isLoopbackUrl("http://127.0.0.1.evil.test/v1"));
    CHECK_FALSE(isLoopbackUrl("https://api.openai.com/v1"));
    CHECK_FALSE(isLoopbackUrl("file:///etc/passwd"));
}

TEST_CASE("AiPlatform guards every call: consent, spend cap, usage settlement, request log", "[ai][platform][provider]") {
    const ModelRegistry registry = testRegistry();
    ProviderRegistry providers(registry);
    int calls = 0;
    providers.registerChat("fake", [&](ProviderContext ctx) {
        return std::make_unique<FakeChat>(ctx.model, std::string(ctx.key.reveal()), &calls);
    });
    MemorySecretStore store;
    KeyRing keys(store, [](std::string_view) { return std::optional<std::string>(); });
    ConsentLedger consent;
    SpendGuard spend;
    RequestLog log;
    test::FakeTransport transport;
    AiPlatform platform(providers, keys, consent, spend, log, transport);
    platform.setClock([] { return ymd(2026, 10, 11); });

    ChatRequest request;
    request.messages.push_back(ChatMessage::user("hello"));
    request.maxOutputTokens = 1000;

    CHECK(platform.chat("cloud-model").error().code == AiErrorCode::NoCredentials);
    REQUIRE(keys.save(*registry.provider("cloud"), Secret("cloud-key-0123456789")));

    auto model = platform.chat("cloud-model");
    REQUIRE(model);
    CHECK((*model)->chat(request, {}, {}).error().code == AiErrorCode::ConsentRequired);
    CHECK(calls == 0);

    auto once = platform.chat("cloud-model", CallOptions{true});
    std::string streamed;
    auto reply = (*once)->chat(request, [&](const ChatEvent& e) {
        if (const auto* t = std::get_if<TextDelta>(&e)) {
            streamed += t->text;
        }
    }, {});
    REQUIRE(reply);
    CHECK(streamed == "ok");
    CHECK(reply->message.text() == "ok, key cloud-key-0123456789");
    // 1000 in x $2/M + 500 out x $10/M = $0.007
    CHECK(spend.spent("cloud", year_month{year(2026), month(10)}) == 7000);
    CHECK(spend.reserved("cloud") == 0);
    const auto entries = log.entries();
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].error == "ConsentRequired");
    CHECK(entries[1].kind == RequestLogEntry::Kind::Call);
    CHECK(entries[1].cost == 7000);
    CHECK(entries[1].usage.outputTokens == 500);

    spend.setMonthlyCap("cloud", 10'000); // 0.007 spent, the next ceiling (~0.010) does not fit
    auto capped = (*once)->chat(request, {}, {});
    REQUIRE_FALSE(capped);
    CHECK(capped.error().code == AiErrorCode::SpendCapExceeded);
    CHECK(calls == 1);

    const auto preflight = platform.preflightChat("cloud-model", request);
    REQUIRE(preflight);
    CHECK(preflight->consent == ConsentState::NeedsPrompt);
    CHECK(preflight->remainingBudget == 3000);
    CHECK(preflight->cost.ceiling > 0);

    consent.setOfflineMode(true);
    CHECK((*once)->chat(request, {}, {}).error().code == AiErrorCode::Offline);
    auto local = platform.chat("local-model");
    REQUIRE(local);
    CHECK((*local)->chat(request, {}, {})); // local models keep working offline, without consent or keys
    CHECK(calls == 2);
}

TEST_CASE("Preferences resolve with fallback to usable models", "[ai][platform][provider]") {
    const ModelRegistry registry = testRegistry();
    ProviderRegistry providers(registry);
    int calls = 0;
    providers.registerChat("fake", [&](ProviderContext ctx) {
        return std::make_unique<FakeChat>(ctx.model, std::string(ctx.key.reveal()), &calls);
    });
    ConsentLedger consent;
    bool cloudKey = false;
    ProviderRegistry::ResolveContext context{ymd(2026, 10, 11), &consent, [&](const ProviderSpec&) { return cloudKey; }};

    CHECK(providers.resolve(Capability::Chat, context).error().code == AiErrorCode::ModelMissing);
    providers.setPreference(Capability::Chat, {"gone-model", "cloud-model", "local-model"});
    auto resolved = providers.resolve(Capability::Chat, context);
    REQUIRE(resolved);
    REQUIRE(resolved->size() == 1);
    CHECK(resolved->front().model->id == "local-model"); // no cloud key yet

    cloudKey = true;
    resolved = providers.resolve(Capability::Chat, context);
    REQUIRE(resolved->size() == 2);
    CHECK(resolved->front().model->id == "cloud-model");

    consent.setOfflineMode(true);
    resolved = providers.resolve(Capability::Chat, context);
    REQUIRE(resolved->size() == 1);
    CHECK(resolved->front().model->id == "local-model");

    providers.setPreference(Capability::Chat, {"cloud-model"});
    CHECK(providers.resolve(Capability::Chat, context).error().code == AiErrorCode::Offline);
    CHECK(providers.resolve(Capability::Transcribe, context).error().code == AiErrorCode::ModelMissing);
    CHECK(providers.candidate("local-model", Capability::Transcribe, context.today).error().code ==
          AiErrorCode::Unsupported);

    CHECK_FALSE(providers.setBaseUrl("cloud", "https://elsewhere.test"));
    REQUIRE(providers.setBaseUrl("local", "http://127.0.0.1:8080/v1/"));
    CHECK(providers.baseUrl(*registry.provider("local")) == "http://127.0.0.1:8080/v1");
    CHECK_FALSE(providers.setBaseUrl("local", "ftp://x"));
    REQUIRE(providers.setBaseUrl("local", ""));
    CHECK(providers.baseUrl(*registry.provider("local")) == "http://127.0.0.1:9931/v1");
}

TEST_CASE("AiJob runs a call on an executor, with cancellation", "[ai][platform][job]") {
    const AiExecutor inline_ = [](std::function<void()> task) { task(); };
    auto done = startAiJob<int>(inline_, [](std::stop_token, const ProgressFn& progress) -> std::expected<int, AiError> {
        progress(0.5f);
        return 42;
    });
    REQUIRE(done.isFinished());
    CHECK(done.wait().value() == 42);
    CHECK(done.progress() == 1.0f);

    std::vector<std::jthread> threads;
    const AiExecutor threaded = [&](std::function<void()> task) { threads.emplace_back(std::move(task)); };
    std::atomic<bool> started{false};
    auto slow = startAiJob<int>(threaded, [&](std::stop_token stop, const ProgressFn&) -> std::expected<int, AiError> {
        started = true;
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(1ms);
        }
        return std::unexpected(AiError(AiErrorCode::Cancelled, "stopped"));
    });
    while (!started) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(slow.waitFor(1ms) == nullptr);
    slow.cancel();
    CHECK(slow.wait().error().code == AiErrorCode::Cancelled);

    int callbacks = 0;
    slow.onDone([&](const AiJob<int>::Result& r) { callbacks += r ? 0 : 1; });
    CHECK(callbacks == 1);

    auto throwing = startAiJob<int>(inline_, [](std::stop_token, const ProgressFn&) -> std::expected<int, AiError> {
        throw std::runtime_error("boom");
    });
    CHECK(throwing.wait().error().code == AiErrorCode::ProviderError);
}

TEST_CASE("AiError maps onto wabcut::Error at module boundaries", "[ai][platform]") {
    CHECK(toError(AiError(AiErrorCode::Cancelled, "x")).code == wabcut::ErrorCode::Cancelled);
    CHECK(toError(AiError(AiErrorCode::Network, "x")).code == wabcut::ErrorCode::IoError);
    CHECK(toError(AiError(AiErrorCode::ModelMissing, "x")).code == wabcut::ErrorCode::NotFound);
    const auto e = toError(AiError(AiErrorCode::RateLimited, "slow"));
    CHECK(e.code == wabcut::ErrorCode::Unknown);
    CHECK(e.message == "RateLimited: slow");
    CHECK(toString(AiErrorCode::SpendCapExceeded) == "SpendCapExceeded");
}
