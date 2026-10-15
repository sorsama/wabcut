// AI-6 security and robustness regressions (docs/internal/reviews-codex-wave2.md P1-1, P1-2, P1-4, P2-9, P2-10), core part:
// URL checks, type-safe JSON access, bounded streams and helper output, the project no-cloud lock (model, I/O,
// consent) and the spend accounting of failed, cancelled and interrupted calls. No network: FakeTransport.

#include "FakeTransport.h"

#include <wabcut/ai/ChildProcess.h>
#include <wabcut/ai/ConsentLedger.h>
#include <wabcut/ai/Http.h>
#include <wabcut/ai/JsonAccess.h>
#include <wabcut/ai/ModelRegistry.h>
#include <wabcut/ai/Provider.h>
#include <wabcut/ai/Secrets.h>
#include <wabcut/ai/SpendGuard.h>
#include <wabcut/ai/Transport.h>
#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <stdexcept>
#include <thread>

using namespace wabcut;
using namespace wabcut::ai;
using namespace std::chrono;
using namespace std::chrono_literals;

namespace {

year_month_day ymd(int y, unsigned m, unsigned d) { return year_month_day{year(y), month(m), day(d)}; }
constexpr year_month kOctober{year(2026), month(10)};

/// The values a provider could send instead of the expected type.
std::vector<Json> substitutes() {
    return {Json(nullptr), Json(42), Json(-1.5), Json("text"), Json(true), Json::object({{"x", 1}}), Json::array({1, 2})};
}

} // namespace

// --- P1-1: URLs from servers ------------------------------------------------------------------------------------

TEST_CASE("Security: parseHttpUrl reads only clean absolute http(s) URLs", "[ai][security][url]") {
    const auto ok = parseHttpUrl("https://generativelanguage.googleapis.com/upload/v1beta/files?upload_id=X");
    REQUIRE(ok);
    CHECK(ok->scheme == "https");
    CHECK(ok->host == "generativelanguage.googleapis.com");
    CHECK_FALSE(ok->port);
    CHECK(ok->userinfo.empty());
    CHECK(ok->path == "/upload/v1beta/files?upload_id=X");

    const auto upper = parseHttpUrl("HTTPS://Example.COM:8443");
    REQUIRE(upper);
    CHECK(upper->scheme == "https");
    CHECK(upper->host == "example.com");
    CHECK(upper->port == 8443);
    CHECK(upper->path.empty());

    const auto creds = parseHttpUrl("https://user:pw@good.example/x");
    REQUIRE(creds);
    CHECK(creds->userinfo == "user:pw");
    CHECK(creds->host == "good.example");
    const auto trick = parseHttpUrl("https://good.example@attacker.invalid/x");
    REQUIRE(trick);
    CHECK(trick->host == "attacker.invalid");
    CHECK_FALSE(trick->userinfo.empty());
    CHECK_FALSE(parseHttpUrl("https://@good.example/x")->userinfo.empty());

    const auto v6 = parseHttpUrl("http://[::1]:9000/v1");
    REQUIRE(v6);
    CHECK(v6->host == "::1");
    CHECK(v6->port == 9000);

    for (const char* bad : {"ftp://example.com/", "https://", "https:///path", "//example.com/", "example.com",
                            "https://exa mple.com/", "https://example.com\\@attacker.invalid/", "https://example.com:/",
                            "https://example.com:0/", "https://example.com:70000/", "https://example.com:44a/",
                            "https://ex%61mple.com/", "https://[::1/", "https://[::1]x/", "https://example.com\t/",
                            "https://\xC3\xA9xample.com/"}) {
        INFO(bad);
        CHECK_FALSE(parseHttpUrl(bad));
    }
}

// --- P1-2: typed access to external JSON --------------------------------------------------------------------------

TEST_CASE("Security: JSON accessors never throw on wrongly typed fields", "[ai][security][json]") {
    for (const Json& value : substitutes()) {
        INFO(value.dump());
        const Json j = Json::object({{"field", value}});
        CHECK_NOTHROW(jsonString(j, "field"));
        CHECK_NOTHROW(jsonInt(j, "field"));
        CHECK_NOTHROW(jsonNumber(j, "field"));
        CHECK_NOTHROW(jsonBool(j, "field"));
        CHECK_NOTHROW(jsonObject(j, "field"));
        CHECK_NOTHROW(jsonArray(j, "field"));
        CHECK_NOTHROW(jsonIdOr(j, "field"));
        CHECK_NOTHROW(jsonAt(j, {"field", "deeper", "deepest"}));
        CHECK(jsonString(j, "field").has_value() == value.is_string());
        CHECK(jsonBool(j, "field").has_value() == value.is_boolean());
        CHECK((jsonObject(j, "field") != nullptr) == value.is_object());
        CHECK((jsonArray(j, "field") != nullptr) == value.is_array());
        // The container itself may have any type too.
        CHECK_NOTHROW(jsonString(value, "field"));
        CHECK_NOTHROW(jsonIntOr(value, "field", 7));
    }
    CHECK(jsonInt(Json{{"n", 3.0}}, "n") == 3);
    CHECK_FALSE(jsonInt(Json{{"n", 3.5}}, "n"));
    CHECK_FALSE(jsonInt(Json{{"n", 1e300}}, "n"));
    CHECK_FALSE(jsonInt(Json{{"n", 18446744073709551615ull}}, "n"));
    CHECK(jsonIdOr(Json{{"id", 17}}, "id") == "17");
    CHECK(jsonIdOr(Json{{"id", nullptr}}, "id").empty());
    CHECK(jsonStringOr(Json{{"id", nullptr}}, "id", "fallback") == "fallback");
}

TEST_CASE("Security: a stream handler that throws fails the request instead of escaping the transport",
          "[ai][security][stream]") {
    HttpStreamHandlers handlers;
    int calls = 0;
    handlers.onEvent = [&](const SseEvent& e) {
        ++calls;
        const Json j = Json::parse(e.data);
        return j.at("id").get<std::string>().size() > 0; // throws for {"id": null}, the reviewer's probe
    };
    StreamDecoder decoder(BodyMode::Sse, handlers);
    CHECK(decoder.feed("data: {\"id\":\"a\"}\n\n"));
    bool ok = true;
    CHECK_NOTHROW(ok = decoder.feed("data: {\"id\":null,\"choices\":[]}\n\n"));
    CHECK_FALSE(ok);
    REQUIRE(decoder.error());
    CHECK(decoder.error()->code == AiErrorCode::ProviderError);
    CHECK(decoder.error()->message.find("type must be string") != std::string::npos);
    CHECK_FALSE(decoder.feed("data: {\"id\":\"b\"}\n\n")); // stays failed
    CHECK(calls == 2);

    HttpStreamHandlers lines;
    lines.onLine = [](std::string_view) -> bool { throw std::out_of_range("bad index"); };
    StreamDecoder ndjson(BodyMode::Ndjson, lines);
    CHECK_FALSE(ndjson.feed("{\"a\":1}\n"));
    CHECK(ndjson.error()->code == AiErrorCode::ProviderError);
}

// --- P2-9: bounded streams -----------------------------------------------------------------------------------------

TEST_CASE("Security: streamed bodies are bounded (lines, events, the whole stream)", "[ai][security][stream]") {
    HttpStreamHandlers handlers;
    std::size_t events = 0;
    handlers.onEvent = [&](const SseEvent&) {
        ++events;
        return true;
    };
    handlers.onLine = [&](std::string_view) {
        ++events;
        return true;
    };

    SECTION("an 8 MiB line with no newline (default limits)") {
        StreamDecoder decoder(BodyMode::Sse, handlers);
        const std::string chunk(64 * 1024, 'x');
        bool ok = decoder.feed("data: ");
        for (int i = 0; ok && i < 128; ++i) { // 8 MiB, never terminated
            ok = decoder.feed(chunk);
        }
        CHECK_FALSE(ok);
        REQUIRE(decoder.error());
        CHECK(decoder.error()->code == AiErrorCode::PayloadTooLarge);
        CHECK(decoder.error()->message.find("line longer than") != std::string::npos);
        CHECK(events == 0);
    }
    SECTION("an NDJSON line too") {
        StreamDecoder decoder(BodyMode::Ndjson, handlers);
        CHECK_FALSE(decoder.feed(std::string(StreamLimits{}.maxLineBytes + 1, '{')));
        CHECK(decoder.error()->code == AiErrorCode::PayloadTooLarge);
    }
    SECTION("many short data lines in one event") {
        StreamDecoder decoder(BodyMode::Sse, handlers);
        const std::string line = "data: " + std::string(1000, 'y') + "\n";
        bool ok = true;
        for (int i = 0; ok && i < 5000; ++i) { // ~5 MB of data lines, never dispatched
            ok = decoder.feed(line);
        }
        CHECK_FALSE(ok);
        CHECK(decoder.error()->message.find("an event larger than") != std::string::npos);
        CHECK(events == 0);
    }
    SECTION("many small, valid events past the total limit") {
        StreamLimits limits;
        limits.maxTotalBytes = 1024 * 1024;
        StreamDecoder decoder(BodyMode::Sse, handlers, limits);
        std::string batch;
        for (int i = 0; i < 1000; ++i) {
            batch += "data: {\"n\":1}\n\n";
        }
        bool ok = true;
        int batches = 0;
        while (ok && batches < 1000) {
            ok = decoder.feed(batch);
            ++batches;
        }
        CHECK_FALSE(ok);
        CHECK(decoder.error()->code == AiErrorCode::PayloadTooLarge);
        CHECK(decoder.error()->message.find("more than 1 MiB") != std::string::npos);
        CHECK(events < 1024 * 1024 / 15); // stopped at the limit, not after everything
        CHECK(StreamLimits{}.maxTotalBytes == std::uint64_t{64} * 1024 * 1024);
    }
    SECTION("downloads (Raw) are not cut by the stream limits") {
        std::uint64_t bytes = 0;
        HttpStreamHandlers raw;
        raw.onData = [&](std::span<const std::byte> b) {
            bytes += b.size();
            return true;
        };
        StreamLimits limits;
        limits.maxTotalBytes = 1024;
        StreamDecoder decoder(BodyMode::Raw, raw, limits);
        CHECK(decoder.feed(std::string(4096, 'z')));
        CHECK(bytes == 4096);
    }
}

TEST_CASE("Security: a helper process that floods its output is cut off", "[ai][security][process]") {
    for (const char* mode : {"flood-line", "flood-lines"}) {
        INFO(mode);
        ChildProcessOptions options;
        options.executable = WABCUT_FAKE_INFER;
        options.arguments = {"--mode", mode};
        auto child = ChildProcess::spawn(options);
        REQUIRE(child);
        if (std::string_view(mode) == "flood-lines") {
            std::this_thread::sleep_for(2s); // nobody reads while it writes 80 MiB: the queue must not take it all
        }
        std::string line;
        ChildProcess::ReadStatus status = ChildProcess::ReadStatus::Timeout;
        const auto until = steady_clock::now() + 30s;
        while (steady_clock::now() < until) {
            status = (*child)->readLine(line, 100ms);
            if (status == ChildProcess::ReadStatus::Overflow || status == ChildProcess::ReadStatus::Closed) {
                break;
            }
        }
        CHECK(status == ChildProcess::ReadStatus::Overflow);
        CHECK(line.size() <= ChildProcess::kMaxLineBytes);
        (*child)->kill();
        CHECK((*child)->readLine(line, 100ms) == ChildProcess::ReadStatus::Overflow); // stays cut off
    }
}

// --- P1-4: the project no-cloud lock --------------------------------------------------------------------------------

TEST_CASE("Security: the no-cloud lock is part of the project: saved, reopened, recovered, undoable", "[ai][security][lock]") {
    ProjectState unlocked;
    const auto plain = io::toJson(unlocked);
    REQUIRE(plain);
    CHECK(plain->find("aiNoCloud") == std::string::npos); // unlocked projects are written exactly as before
    CHECK_FALSE(io::fromJson(*plain)->aiNoCloud);              // and older files (no key) open unlocked

    ProjectState locked;
    locked.aiNoCloud = true;
    CHECK_FALSE(locked == unlocked);
    CHECK_FALSE(isSameRevision(locked, unlocked));
    CHECK(diff(unlocked, locked).aiLockChanged);
    CHECK_FALSE(diff(unlocked, locked).empty());

    const auto dir = test::uniqueTempPath("wabcut-ai-lock");
    std::filesystem::create_directories(dir);
    const auto file = dir / "nda.wabcut";
    REQUIRE(io::saveProject(locked, file));
    const auto reopened = io::loadProject(file);
    REQUIRE(reopened);
    CHECK(reopened->aiNoCloud); // save -> reopen -> still locked

    // The recovery copy (autosave) carries it too.
    REQUIRE(io::writeAutosave(locked, file));
    const auto recovered = io::loadProject(io::autosavePath(file));
    REQUIRE(recovered);
    CHECK(recovered->aiNoCloud);
    std::filesystem::remove_all(dir);

    // Setting it is an edit: one undo step (not dropped as a no-op), and it is in the ChangeSet.
    EditSession session(unlocked);
    const auto changes = session.commit("Lock Project to No-Cloud AI", locked);
    REQUIRE(changes);
    CHECK(changes->aiLockChanged);
    CHECK(session.snapshot()->aiNoCloud);
    CHECK(session.undoCount() == 1);
    session.undo();
    CHECK_FALSE(session.snapshot()->aiNoCloud);
}

TEST_CASE("Security: the ACTIVE project's lock blocks every cloud call, whatever consent says", "[ai][security][lock]") {
    const ModelRegistry& builtin = ModelRegistry::builtin();
    const ProviderSpec& anthropic = *builtin.provider("anthropic");
    ConsentLedger consent;
    consent.grantAlways(anthropic, Capability::Chat, "2026-10-11");
    consent.grantAlways(anthropic, Capability::Transcribe, "2026-10-11");
    CHECK(consent.authorize(anthropic, anthropic.baseUrl, Capability::Chat));

    // Two projects in one session: the source reads whichever is active at the time of the call.
    EditSession session{ProjectState{}};
    consent.setProjectLockSource([&session] { return session.snapshot()->aiNoCloud; });
    ProjectState nda;
    nda.aiNoCloud = true;
    session.reset(nda); // open the NDA project
    CHECK(consent.projectLock());
    CHECK(consent.state(anthropic, anthropic.baseUrl, Capability::Chat) == ConsentState::Blocked);
    const auto blocked = consent.authorize(anthropic, anthropic.baseUrl, Capability::Chat, /*allowOnce=*/true);
    REQUIRE_FALSE(blocked);
    CHECK(blocked.error().code == AiErrorCode::Offline);
    CHECK(blocked.error().message == "this project is locked to no-cloud AI");
    CHECK_FALSE(consent.authorize(anthropic, anthropic.baseUrl, Capability::Transcribe));
    // A local provider is still allowed (nothing leaves the machine).
    CHECK(consent.authorize(*builtin.provider("ollama"), builtin.provider("ollama")->baseUrl, Capability::Chat));

    session.reset(ProjectState{}); // switch to an unlocked project: its own (absent) lock applies
    CHECK_FALSE(consent.projectLock());
    CHECK(consent.authorize(anthropic, anthropic.baseUrl, Capability::Chat));
    CHECK(consent.toJson().dump().find("projectLock") == std::string::npos); // never stored as a global setting
    consent.setProjectLockSource({});
}

// --- P2-10: spend accounting ---------------------------------------------------------------------------------------

namespace {

ModelRegistry spendRegistry() {
    auto r = ModelRegistry::fromJson(R"({"schemaVersion":1,"providers":[
        {"id":"cloud","name":"Cloud","api":"scripted","baseUrl":"https://cloud.test","auth":"bearer","keyRequired":false,
         "termsUrl":"https://cloud.test/terms","dataPolicy":"kept 30 days"}],
      "models":[
        {"id":"cloud-model","provider":"cloud","capabilities":{"chat":true},"prices":[{"inputPerMTok":2,"outputPerMTok":10}]}]})");
    REQUIRE(r);
    return std::move(*r);
}

/// What the scripted adapter does in one call (it gets the guarded transport and the event handler).
using Script = std::function<std::expected<ChatResponse, AiError>(IHttpTransport&, const ChatEventHandler&, std::stop_token)>;

class ScriptedAdapter final : public IChatModel {
public:
    ScriptedAdapter(ModelSpec spec, IHttpTransport& transport, Script& script)
        : m_spec(std::move(spec)), m_transport(transport), m_script(script) {}
    const ModelSpec& spec() const override { return m_spec; }
    std::expected<ChatResponse, AiError> chat(const ChatRequest&, const ChatEventHandler& onEvent, std::stop_token stop) override {
        return m_script(m_transport, onEvent, stop);
    }

private:
    ModelSpec m_spec;
    IHttpTransport& m_transport;
    Script& m_script;
};

struct SpendBench {
    SpendBench() : registry(spendRegistry()), providers(registry), keys(store, [](std::string_view) { return std::optional<std::string>(); }),
                   platform(providers, keys, consent, spend, log, transport) {
        providers.registerChat("scripted", [this](ProviderContext ctx) {
            return std::make_unique<ScriptedAdapter>(ctx.model, ctx.transport, script);
        });
        platform.setClock([] { return ymd(2026, 10, 11); });
        consent.grantAlways(*registry.provider("cloud"), Capability::Chat, "2026-10-11");
        request.messages.push_back(ChatMessage::user("hello there"));
        request.maxOutputTokens = 1000;
    }
    std::expected<ChatResponse, AiError> call() {
        auto model = platform.chat("cloud-model");
        REQUIRE(model);
        return (*model)->chat(request, {}, {});
    }
    MicroUsd spent() const { return spend.spent("cloud", kOctober); }
    MicroUsd cost(std::int64_t in, std::int64_t out) const {
        Usage u;
        u.inputTokens = in;
        u.outputTokens = out;
        return costOf(*registry.model("cloud-model"), u, ymd(2026, 10, 11));
    }
    std::int64_t inputEstimate() const {
        return estimateChatCost(*registry.model("cloud-model"), request, ymd(2026, 10, 11)).inputTokens;
    }

    ModelRegistry registry;
    ProviderRegistry providers;
    MemorySecretStore store;
    KeyRing keys;
    ConsentLedger consent;
    SpendGuard spend;
    RequestLog log;
    test::FakeTransport transport;
    AiPlatform platform;
    Script script;
    ChatRequest request;
};

Usage usage(std::int64_t in, std::int64_t out) {
    Usage u;
    u.inputTokens = in;
    u.outputTokens = out;
    return u;
}

/// Sends a streamed request through the guarded transport (the provider answers 200, then the script goes on).
void openStream(IHttpTransport& transport, std::stop_token stop) {
    HttpRequest request;
    request.url = "https://cloud.test/v1/chat";
    request.responseMode = BodyMode::Sse;
    HttpStreamHandlers handlers;
    handlers.onEvent = [](const SseEvent&) { return true; };
    REQUIRE(transport.send(request, handlers, stop));
}

} // namespace

TEST_CASE("Security: partial usage of a failed call is settled, not released", "[ai][security][spend]") {
    SpendBench b;
    b.transport.push(test::sseBody("data: {}\n\n"));
    b.script = [](IHttpTransport& t, const ChatEventHandler& onEvent, std::stop_token stop) -> std::expected<ChatResponse, AiError> {
        openStream(t, stop);
        onEvent(UsageUpdate{usage(1000, 200)});
        onEvent(TextDelta{0, "partial answer"});
        return std::unexpected(AiError(AiErrorCode::Network, "the stream broke off"));
    };
    REQUIRE_FALSE(b.call());
    CHECK(b.spent() == b.cost(1000, 200)); // 1000 x $2/M + 200 x $10/M = 4000 micro-USD
    CHECK(b.spend.reserved("cloud") == 0);
    const auto entries = b.log.entries();
    REQUIRE_FALSE(entries.empty());
    CHECK(entries.back().cost == b.cost(1000, 200));
    CHECK(entries.back().error == "Network");

    // The budget sees it: with a cap that the partial call used up, the next call is refused before it runs.
    b.spend.setMonthlyCap("cloud", b.cost(1000, 200) + 1000);
    bool ran = false;
    b.script = [&](IHttpTransport&, const ChatEventHandler&, std::stop_token) -> std::expected<ChatResponse, AiError> {
        ran = true;
        return ChatResponse{};
    };
    const auto capped = b.call();
    REQUIRE_FALSE(capped);
    CHECK(capped.error().code == AiErrorCode::SpendCapExceeded);
    CHECK_FALSE(ran);
}

TEST_CASE("Security: usage then cancel is settled; output streamed after the last usage report counts", "[ai][security][spend]") {
    SpendBench b;
    b.transport.push(test::sseBody("data: {}\n\n"));
    b.script = [](IHttpTransport& t, const ChatEventHandler& onEvent, std::stop_token stop) -> std::expected<ChatResponse, AiError> {
        openStream(t, stop);
        onEvent(UsageUpdate{usage(800, 1)});
        onEvent(TextDelta{0, std::string(400, 'a')}); // ~100 tokens after the usage report
        return std::unexpected(AiError(AiErrorCode::Cancelled, "cancelled"));
    };
    REQUIRE_FALSE(b.call());
    CHECK(b.spent() == b.cost(800, 100));
    CHECK(b.spend.reserved("cloud") == 0);
}

TEST_CASE("Security: an exception in the middle of a call does not strand its reservation", "[ai][security][spend]") {
    SpendBench b;
    b.transport.push(test::sseBody("data: {}\n\n"));
    b.script = [](IHttpTransport& t, const ChatEventHandler& onEvent, std::stop_token stop) -> std::expected<ChatResponse, AiError> {
        openStream(t, stop);
        onEvent(UsageUpdate{usage(500, 50)});
        throw std::runtime_error("adapter bug");
    };
    CHECK_THROWS_AS(b.call(), std::runtime_error);
    CHECK(b.spend.reserved("cloud") == 0);
    CHECK(b.spent() == b.cost(500, 50));
}

TEST_CASE("Security: only proven unprocessed calls release their reservation", "[ai][security][spend]") {
    SECTION("rejected before anything was sent") {
        SpendBench b;
        b.script = [](IHttpTransport&, const ChatEventHandler&, std::stop_token) -> std::expected<ChatResponse, AiError> {
            return std::unexpected(AiError(AiErrorCode::InvalidRequest, "bad tool schema"));
        };
        REQUIRE_FALSE(b.call());
        CHECK(b.spent() == 0);
        CHECK(b.spend.reserved("cloud") == 0);
    }
    SECTION("refused by the provider with an error status") {
        SpendBench b;
        b.transport.pushJson(429, R"({"error":{"type":"rate_limit_error","message":"slow down"}})");
        b.script = [](IHttpTransport& t, const ChatEventHandler&, std::stop_token stop) -> std::expected<ChatResponse, AiError> {
            HttpRequest request;
            request.url = "https://cloud.test/v1/chat";
            auto response = t.send(request, {}, stop);
            REQUIRE(response);
            return std::unexpected(classifyHttpError(response->status, response->headers, response->body));
        };
        REQUIRE_FALSE(b.call());
        CHECK(b.spent() == 0);
    }
    SECTION("the provider answered and streamed output but reported no usage: a conservative estimate") {
        SpendBench b;
        b.transport.push(test::sseBody("data: {}\n\n"));
        b.script = [](IHttpTransport& t, const ChatEventHandler& onEvent, std::stop_token stop) -> std::expected<ChatResponse, AiError> {
            openStream(t, stop);
            onEvent(TextDelta{0, std::string(80, 'b')}); // 20 tokens
            return std::unexpected(AiError(AiErrorCode::ProviderError, "overloaded mid-stream"));
        };
        REQUIRE_FALSE(b.call());
        CHECK(b.spent() == b.cost(b.inputEstimate(), 20));
        CHECK(b.spent() > 0);
    }
}


// --- Review wave 3, P1-5: consent and the no-cloud lock are checked at EVERY dispatch -------------------------------

namespace {

/// A call that sends two requests through the guarded transport, like Gemini's upload (init, then the video).
std::expected<ChatResponse, AiError> twoRequests(IHttpTransport& t, const ChatEventHandler&, std::stop_token stop) {
    HttpRequest init;
    init.url = "https://cloud.test/upload/init";
    auto first = t.send(init, {}, stop);
    if (!first) {
        return std::unexpected(first.error());
    }
    HttpRequest upload;
    upload.url = "https://cloud.test/upload/video";
    upload.body = "the video";
    auto second = t.send(upload, {}, stop);
    if (!second) {
        return std::unexpected(second.error());
    }
    return ChatResponse{};
}

} // namespace

TEST_CASE("Security: the lock turned on between two requests of one call stops the second (wave 3, P1-5)", "[ai][security][lock]") {
    SpendBench b;
    test::ScriptedResponse init;
    init.status = 200;
    init.body = "{}";
    init.before = [&b] { b.consent.setProjectLock(true); }; // the user locks the project while the init is pending
    b.transport.push(init);
    b.transport.pushJson(200, "{}");
    b.script = twoRequests;
    const auto result = b.call();
    REQUIRE_FALSE(result);
    CHECK(result.error().code == AiErrorCode::Offline);
    REQUIRE(b.transport.requests().size() == 1); // the upload never left
    CHECK(b.transport.requests().front().url == "https://cloud.test/upload/init");
}

TEST_CASE("Security: another project opened during a call stops its next request (wave 3, P1-5)", "[ai][security][lock]") {
    SpendBench b;
    std::atomic<std::uint64_t> project{7};
    b.consent.setProjectIdentitySource([&project] { return project.load(); });
    test::ScriptedResponse init;
    init.body = "{}";
    init.before = [&project] { project = 8; }; // File > Open while the operation runs
    b.transport.push(init);
    b.transport.pushJson(200, "{}");
    b.script = twoRequests;
    const auto result = b.call();
    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("another project") != std::string::npos);
    CHECK(b.transport.requests().size() == 1);
    b.consent.setProjectIdentitySource({});

    // The same call on a project that stays put goes through.
    SpendBench steady;
    steady.consent.setProjectIdentitySource([] { return std::uint64_t{7}; });
    steady.transport.pushJson(200, "{}");
    steady.transport.pushJson(200, "{}");
    steady.script = twoRequests;
    CHECK(steady.call());
    CHECK(steady.transport.requests().size() == 2);
    steady.consent.setProjectIdentitySource({});
}

TEST_CASE("Security: a retry is a new dispatch: the lock turned on before it stops it (wave 3, P1-5)", "[ai][security][lock]") {
    // The platform's transport retries (as the app's does): the first answer is a retryable 503, during which the
    // project gets locked. The retry must not go out.
    struct RetryBench {
        RetryBench()
            : registry(spendRegistry()), providers(registry), keys(store, [](std::string_view) { return std::optional<std::string>(); }),
              retrying(fake, RetryPolicy{}, [](std::chrono::milliseconds, std::stop_token) { return true; }),
              platform(providers, keys, consent, spend, log, retrying) {
            providers.registerChat("scripted", [this](ProviderContext ctx) {
                return std::make_unique<ScriptedAdapter>(ctx.model, ctx.transport, script);
            });
            platform.setClock([] { return ymd(2026, 10, 11); });
            consent.grantAlways(*registry.provider("cloud"), Capability::Chat, "2026-10-11");
        }
        ModelRegistry registry;
        ProviderRegistry providers;
        MemorySecretStore store;
        KeyRing keys;
        ConsentLedger consent;
        SpendGuard spend;
        RequestLog log;
        test::FakeTransport fake;
        RetryingTransport retrying;
        AiPlatform platform;
        Script script;
    } b;
    test::ScriptedResponse busy;
    busy.status = 503;
    busy.body = "{\"error\":{\"message\":\"overloaded\"}}";
    busy.before = [&b] { b.consent.setProjectLock(true); };
    b.fake.push(busy);
    b.fake.pushJson(200, "{}");
    b.script = [](IHttpTransport& t, const ChatEventHandler&, std::stop_token stop) -> std::expected<ChatResponse, AiError> {
        HttpRequest request;
        request.url = "https://cloud.test/v1/chat";
        auto response = t.send(request, {}, stop);
        if (!response) {
            return std::unexpected(response.error());
        }
        return ChatResponse{};
    };
    ChatRequest request;
    request.messages.push_back(ChatMessage::user("hello"));
    request.maxOutputTokens = 100;
    auto model = b.platform.chat("cloud-model");
    REQUIRE(model);
    const auto result = (*model)->chat(request, {}, {});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == AiErrorCode::Offline);
    CHECK(b.fake.requests().size() == 1); // no retry went out
    CHECK(b.retrying.totalRetries() == 1); // it was about to retry, and asked first
}

namespace {

/// A store that keeps keys across restarts (the OS keychain's stand-in).
class KeychainLike final : public SecretStore {
public:
    std::expected<std::optional<Secret>, AiError> read(std::string_view id) override { return m_memory.read(id); }
    std::expected<void, AiError> write(std::string_view id, const Secret& secret) override { return m_memory.write(id, secret); }
    std::expected<void, AiError> remove(std::string_view id) override { return m_memory.remove(id); }
    std::string backendName() const override { return "keychain"; }
    bool persistent() const override { return true; }

private:
    MemorySecretStore m_memory;
};

} // namespace

// Review wave 4, P2-5: the plaintext key file of the removed opt-in is moved into the key store and securely deleted.
TEST_CASE("Legacy plaintext keys move into the key store and the file is overwritten and deleted", "[ai][security]") {
    const auto file = wabcut::ai::test::uniqueTempPath("ai-keys", ".json");
    {
        PlaintextFileSecretStore legacy(file);
        REQUIRE(legacy.write("anthropic", Secret("sk-ant-legacy-0123456789")));
    }
    CHECK(legacyKeyCount(file) == 1);
    CHECK(legacyKeyCount(file.string() + ".missing") == 0);
    KeychainLike store;
    KeyRing keys(store, [](std::string_view) { return std::optional<std::string>(); });
    const ModelRegistry& registry = ModelRegistry::builtin();
    const auto result = importLegacyKeys(file, keys, registry.providers());
    CHECK(result.error.empty());
    CHECK(result.keptBecause.empty());
    CHECK(result.imported == std::vector<std::string>{"anthropic"});
    CHECK(result.fileRemoved);
    CHECK_FALSE(std::filesystem::exists(file));
    auto key = keys.get(*registry.provider("anthropic"));
    REQUIRE(key);
    CHECK(key->reveal() == "sk-ant-legacy-0123456789");
    CHECK(legacyKeyCount(file) == 0);

    // A store that refuses keeps the file, so nothing is lost.
    class Refusing final : public SecretStore {
    public:
        std::expected<std::optional<Secret>, AiError> read(std::string_view) override { return std::optional<Secret>(); }
        std::expected<void, AiError> write(std::string_view, const Secret&) override {
            return std::unexpected(AiError(AiErrorCode::ProviderError, "keychain locked"));
        }
        std::expected<void, AiError> remove(std::string_view) override { return {}; }
        std::string backendName() const override { return "refusing"; }
        bool persistent() const override { return true; }
    } refusing;
    {
        PlaintextFileSecretStore legacy(file);
        REQUIRE(legacy.write("anthropic", Secret("sk-ant-legacy-0123456789")));
    }
    KeyRing locked(refusing, [](std::string_view) { return std::optional<std::string>(); });
    const auto kept = importLegacyKeys(file, locked, registry.providers());
    CHECK_FALSE(kept.error.empty());
    CHECK_FALSE(kept.fileRemoved);
    CHECK(std::filesystem::exists(file));
    CHECK(secureDeleteFile(file));
    CHECK_FALSE(std::filesystem::exists(file));
}

// Review wave 5, A5-P2-17: the file was deleted although its keys were only moved into memory (no keychain: all lost at
// the next start) or belonged to providers this build does not know (lost for good). It stays now, and says why.
TEST_CASE("Legacy plaintext keys stay in their file unless every key is safe elsewhere", "[ai][security]") {
    const auto file = wabcut::ai::test::uniqueTempPath("ai-keys", ".json");
    const auto write = [&] {
        PlaintextFileSecretStore legacy(file);
        REQUIRE(legacy.write("anthropic", Secret("sk-ant-legacy-0123456789")));
        REQUIRE(legacy.write("retired-provider", Secret("old-key-0123456789")));
    };
    const ModelRegistry& registry = ModelRegistry::builtin();
    write();
    MemorySecretStore session; // no keychain: this session only
    KeyRing forgetful(session, [](std::string_view) { return std::optional<std::string>(); });
    const auto memoryOnly = importLegacyKeys(file, forgetful, registry.providers());
    CHECK(memoryOnly.error.empty());
    CHECK(memoryOnly.imported == std::vector<std::string>{"anthropic"}); // usable now
    CHECK_FALSE(memoryOnly.fileRemoved);
    CHECK(memoryOnly.keptBecause.find("this session only") != std::string::npos);
    CHECK(legacyKeyCount(file) == 2);

    KeychainLike keychain;
    KeyRing keys(keychain, [](std::string_view) { return std::optional<std::string>(); });
    const auto unknown = importLegacyKeys(file, keys, registry.providers());
    CHECK(unknown.error.empty());
    CHECK(unknown.imported == std::vector<std::string>{"anthropic"});
    CHECK(unknown.unknown == std::vector<std::string>{"retired-provider"});
    CHECK_FALSE(unknown.fileRemoved);
    CHECK(unknown.keptBecause.find("retired-provider") != std::string::npos);
    CHECK(legacyKeyCount(file) == 2);
    CHECK(secureDeleteFile(file)); // the user's call, from Settings
}
