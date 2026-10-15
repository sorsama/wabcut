// AI platform: SSE/NDJSON decoding, HTTP error classification, Retry-After, the retrying and logging transports,
// redaction, and model downloads (HttpDownloadTransport for ModelStore).

#include "FakeTransport.h"

#include <wabcut/ai/HttpDownload.h>
#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/Redact.h>
#include <wabcut/ai/RequestLog.h>
#include <wabcut/ai/Sha256.h>
#include <wabcut/ai/Transport.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace wabcut::ai;
using namespace std::chrono_literals;

namespace {

struct Collected {
    std::vector<SseEvent> events;
    std::vector<std::string> lines;
    HttpStreamHandlers handlers;

    Collected() {
        handlers.onEvent = [this](const SseEvent& e) {
            events.push_back(e);
            return true;
        };
        handlers.onLine = [this](std::string_view l) {
            lines.emplace_back(l);
            return true;
        };
    }
};

void feedInChunks(StreamDecoder& d, std::string_view text, std::size_t chunk) {
    for (std::size_t pos = 0; pos < text.size(); pos += chunk) {
        REQUIRE(d.feed(text.substr(pos, std::min(chunk, text.size() - pos))));
    }
    REQUIRE(d.finish());
}

} // namespace

TEST_CASE("SSE: events, data lines, comments, ids and every line ending", "[ai][platform][sse]") {
    const std::string stream = "\xEF\xBB\xBF: keep-alive\r\n"
                               "event: message_start\r\n"
                               "data: {\"a\":1}\r\n"
                               "\r\n"
                               "data: line one\n"
                               "data:line two\n"
                               "id: 42\n"
                               "\n"
                               "event: ping\r"
                               "data: \r"
                               "\r"
                               "retry: 1000\n"
                               "event: ignored-without-data\n"
                               "\n"
                               "data: [DONE]\n\n"
                               "data: unterminated at the end";
    for (std::size_t chunk : {1u, 2u, 3u, 7u, 1000u}) {
        Collected c;
        StreamDecoder d(BodyMode::Sse, c.handlers);
        feedInChunks(d, stream, chunk);
        INFO("chunk size " << chunk);
        REQUIRE(c.events.size() == 4);
        CHECK(c.events[0] == SseEvent{"message_start", "{\"a\":1}", ""});
        CHECK(c.events[1] == SseEvent{"", "line one\nline two", "42"});
        CHECK(c.events[2] == SseEvent{"ping", "", "42"});
        CHECK(c.events[3].data == "[DONE]");
    }
}

TEST_CASE("SSE: a handler returning false stops decoding", "[ai][platform][sse]") {
    int seen = 0;
    HttpStreamHandlers h;
    h.onEvent = [&](const SseEvent&) { return ++seen < 2; };
    StreamDecoder d(BodyMode::Sse, h);
    CHECK(d.feed("data: 1\n\n"));
    CHECK_FALSE(d.feed("data: 2\n\ndata: 3\n\n"));
    CHECK(seen == 2);
}

TEST_CASE("NDJSON: one call per line, blank lines skipped, last line flushed", "[ai][platform][ndjson]") {
    Collected c;
    StreamDecoder d(BodyMode::Ndjson, c.handlers);
    feedInChunks(d, "{\"a\":1}\r\n\n  \n{\"b\":\"\xE0\xB8\x81\"}\n{\"done\":true}", 3);
    REQUIRE(c.lines.size() == 3);
    CHECK(c.lines[0] == "{\"a\":1}");
    CHECK(c.lines[1] == "{\"b\":\"\xE0\xB8\x81\"}"); // a Thai character split across chunks survives
    CHECK(c.lines[2] == "{\"done\":true}");
}

TEST_CASE("Retry-After parsing: seconds, fractions, HTTP dates", "[ai][platform][retry]") {
    const auto now = std::chrono::sys_days(std::chrono::year(2026) / 10 / 11) + 8h;
    CHECK(parseRetryAfter("120", now) == 120000ms);
    CHECK(parseRetryAfter(" 1.5 ", now) == 1500ms);
    CHECK(parseRetryAfter("Sun, 11 Oct 2026 08:00:30 GMT", now) == 30000ms);
    CHECK(parseRetryAfter("Sun, 11 Oct 2026 07:00:00 GMT", now) == 0ms); // in the past
    CHECK_FALSE(parseRetryAfter("soon", now));
    CHECK_FALSE(parseRetryAfter("-1", now));
    CHECK(retryAfterFrom({{"Retry-After-Ms", "250"}, {"retry-after", "9"}}, now) == 250ms);
    CHECK(retryAfterFrom({{"Retry-After", "9"}}, now) == 9000ms);
    CHECK_FALSE(retryAfterFrom({}, now));
}

TEST_CASE("HTTP errors are classified per provider body", "[ai][platform][errors]") {
    SECTION("Anthropic rate limit with retry-after") {
        const auto e = classifyHttpError(429, {{"retry-after", "7"}},
                                         R"({"type":"error","error":{"type":"rate_limit_error","message":"slow down"}})");
        CHECK(e.code == AiErrorCode::RateLimited);
        CHECK(e.retryAfter == 7000ms);
        CHECK(e.providerCode == "rate_limit_error");
        CHECK(e.isRetryable());
    }
    SECTION("Anthropic spend limit 429 is a quota error, not retried") {
        const auto e = classifyHttpError(
            429, {}, R"({"type":"error","error":{"type":"rate_limit_error","message":"limit","details":{"error_code":"enforced_spend_limit_reached"}}})");
        CHECK(e.code == AiErrorCode::QuotaExceeded);
        CHECK_FALSE(e.isRetryable());
    }
    SECTION("OpenAI insufficient_quota vs rate limit") {
        CHECK(classifyHttpError(429, {}, R"({"error":{"message":"m","type":"insufficient_quota","param":null,"code":"insufficient_quota"}})")
                  .code == AiErrorCode::QuotaExceeded);
        CHECK(classifyHttpError(429, {}, R"({"error":{"message":"m","type":"requests","param":null,"code":"rate_limit_exceeded"}})")
                  .code == AiErrorCode::RateLimited);
    }
    SECTION("Gemini, Deepgram, ElevenLabs, AssemblyAI shapes") {
        const auto gemini = classifyHttpError(429, {}, R"({"error":{"code":"rate_limit_exceeded","message":"m"}})");
        CHECK(gemini.code == AiErrorCode::RateLimited);
        CHECK(gemini.providerCode == "rate_limit_exceeded");
        const auto legacy = classifyHttpError(400, {}, R"({"error":{"code":400,"message":"bad","status":"INVALID_ARGUMENT"}})");
        CHECK(legacy.code == AiErrorCode::InvalidRequest);
        CHECK(legacy.providerCode == "INVALID_ARGUMENT");
        const auto deepgram = classifyHttpError(401, {}, R"({"err_code":"INVALID_AUTH","err_msg":"Invalid credentials.","request_id":"x"})");
        CHECK(deepgram.code == AiErrorCode::Unauthorized);
        CHECK(deepgram.providerCode == "INVALID_AUTH");
        CHECK(classifyHttpError(402, {}, R"({"detail":{"type":"payment_required","code":"insufficient_credits","message":"m"}})")
                  .code == AiErrorCode::QuotaExceeded);
        CHECK(classifyHttpError(422, {}, R"({"detail":[{"loc":["body"],"msg":"bad field","type":"value_error"}]})").code ==
              AiErrorCode::InvalidRequest);
        CHECK(classifyHttpError(401, {}, R"({"error":"Authentication error, API token missing/invalid"})").code ==
              AiErrorCode::Unauthorized);
    }
    SECTION("status-only mappings") {
        CHECK(classifyHttpError(413, {}, "too big").code == AiErrorCode::PayloadTooLarge);
        CHECK(classifyHttpError(529, {}, R"({"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})").code ==
              AiErrorCode::RateLimited);
        CHECK(classifyHttpError(404, {}, R"({"error":{"code":"model_not_found","message":"m"}})").code ==
              AiErrorCode::ModelMissing);
        CHECK(classifyHttpError(404, {}, "Not Found").code == AiErrorCode::InvalidRequest);
        const auto server = classifyHttpError(503, {{"retry-after", "2"}}, "<html>unavailable</html>");
        CHECK(server.code == AiErrorCode::ProviderError);
        CHECK(server.isRetryable());
        CHECK(server.retryAfter == 2000ms);
        CHECK(classifyHttpError(408, {}, "").code == AiErrorCode::Network);
    }
    SECTION("messages are redacted") {
        const auto e = classifyHttpError(401, {}, R"({"error":{"message":"Incorrect API key provided: sk-proj-abcdefghijklmnopqrstuvwxyz0123","code":"invalid_api_key"}})");
        CHECK(e.message.find("sk-proj-abcdefghijklmnop") == std::string::npos);
        CHECK(e.message.find(kRedacted) != std::string::npos);
    }
}

TEST_CASE("RetryingTransport honours Retry-After and backs off with jitter", "[ai][platform][retry]") {
    test::FakeTransport fake;
    std::vector<std::chrono::milliseconds> slept;
    auto sleeper = [&](std::chrono::milliseconds d, std::stop_token) {
        slept.push_back(d);
        return true;
    };
    RetryPolicy policy;
    RetryingTransport retrying(fake, policy, sleeper, 1234);
    HttpRequest request;
    request.url = "https://example.test/v1/x";

    SECTION("429 with retry-after, then success") {
        fake.pushJson(429, R"({"error":{"type":"rate_limit_error","message":"m"}})", {{"retry-after", "2"}});
        fake.pushJson(200, R"({"ok":true})");
        auto r = retrying.send(request, {}, {});
        REQUIRE(r);
        CHECK(r->status == 200);
        REQUIRE(slept.size() == 1);
        CHECK(slept[0] == 2000ms);
        CHECK(fake.requests().size() == 2);
    }
    SECTION("5xx without a hint: exponential backoff within the jitter band, then give up") {
        for (int i = 0; i < 4; ++i) {
            fake.pushJson(503, R"({"error":{"message":"unavailable"}})");
        }
        auto r = retrying.send(request, {}, {});
        REQUIRE(r);
        CHECK(r->status == 503); // the caller classifies the final response
        REQUIRE(slept.size() == 3);
        for (std::size_t i = 0; i < slept.size(); ++i) {
            const double base = 1000.0 * (1 << i);
            CHECK(slept[i].count() >= static_cast<long long>(base * 0.8));
            CHECK(slept[i].count() <= static_cast<long long>(base * 1.2));
        }
        CHECK(retrying.totalRetries() == 3);
    }
    SECTION("quota 429 and 400 are not retried") {
        fake.pushJson(429, R"({"error":{"type":"insufficient_quota","code":"insufficient_quota","message":"m"}})");
        CHECK(retrying.send(request, {}, {})->status == 429);
        fake.pushJson(400, R"({"error":{"message":"bad"}})");
        CHECK(retrying.send(request, {}, {})->status == 400);
        CHECK(slept.empty());
    }
    SECTION("a retry-after beyond the policy maximum is returned at once") {
        fake.pushJson(429, R"({"error":{"type":"rate_limit_error","message":"m"}})", {{"retry-after", "600"}});
        CHECK(retrying.send(request, {}, {})->status == 429);
        CHECK(slept.empty());
    }
    SECTION("network errors: retried before the response head, never after streaming began") {
        fake.pushError(AiErrorCode::Network, "connection reset");
        fake.pushJson(200, "{}");
        CHECK(retrying.send(request, {}, {}));
        CHECK(slept.size() == 1);

        test::ScriptedResponse broken;
        broken.body = "data: {\"n\":1}\n\ndata: {\"n\":2}\n\n";
        broken.error = AiError(AiErrorCode::Network, "reset mid-stream");
        broken.failAfterHead = true;
        fake.push(broken);
        HttpRequest streaming = request;
        streaming.responseMode = BodyMode::Sse;
        HttpStreamHandlers h;
        int events = 0;
        h.onEvent = [&](const SseEvent&) {
            ++events;
            return true;
        };
        auto r = retrying.send(streaming, h, {});
        REQUIRE_FALSE(r);
        CHECK(r.error().code == AiErrorCode::Network);
        CHECK(events == 1);
        CHECK(slept.size() == 1); // no second retry
    }
    SECTION("cancelled while waiting") {
        RetryingTransport cancelling(fake, policy, [](std::chrono::milliseconds, std::stop_token) { return false; });
        fake.pushJson(503, "{}");
        auto r = cancelling.send(request, {}, {});
        REQUIRE_FALSE(r);
        CHECK(r.error().code == AiErrorCode::Cancelled);
    }
}

TEST_CASE("interruptibleSleep returns early when stopped", "[ai][platform][retry]") {
    std::stop_source source;
    source.request_stop();
    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(interruptibleSleep(10s, source.get_token()));
    CHECK(std::chrono::steady_clock::now() - start < 5s);
    CHECK(interruptibleSleep(1ms, {}));
}

TEST_CASE("Redactor masks credentials in every form it may appear", "[ai][platform][redact]") {
    Redactor r;
    r.addSecret("my-registered-secret-value");
    r.addSecret("short"); // too short to register
    const std::string text = "Authorization: Bearer abcdefghijklmnop0123\n"
                             "x-api-key: sk-ant-api03-zzzzzzzzzzzzzzzzzzzzzzzz\n"
                             "{\"xi-api-key\": \"eleven-secret-1234567890\", \"model\": \"scribe_v2\"}\n"
                             "GET https://example.test/v1beta/models?key=AIzaSyA1234567890abcdefghijklmnopqrstuv&pageSize=1\n"
                             "Token deepgram0123456789abcdef in text, also my-registered-secret-value, and a short word\n"
                             "OpenAI said: Incorrect API key provided: sk-proj-0123456789abcdefghijklmnop.";
    const std::string out = r.redact(text);
    for (const char* leaked : {"abcdefghijklmnop0123", "sk-ant-api03", "eleven-secret", "AIzaSyA123", "deepgram0123",
                               "my-registered-secret-value", "sk-proj-0123"}) {
        INFO(out);
        CHECK(out.find(leaked) == std::string::npos);
    }
    CHECK(out.find("\"model\": \"scribe_v2\"") != std::string::npos);
    CHECK(out.find("pageSize=1") != std::string::npos);
    CHECK(out.find("a short word") != std::string::npos);

    const HttpHeaders headers = r.redactHeaders({{"X-Goog-Api-Key", "AIzaSecret"}, {"Content-Type", "application/json"}});
    CHECK(headers[0].value == kRedacted);
    CHECK(headers[1].value == "application/json");
    CHECK(isSecretHeader("Ocp-Apim-Subscription-Key"));
    CHECK(isSecretHeader("DeepL-Auth-Key"));
    CHECK_FALSE(isSecretHeader("anthropic-version"));

    r.removeSecret("my-registered-secret-value");
    CHECK(r.redact("my-registered-secret-value") == "my-registered-secret-value");
}

TEST_CASE("LoggingTransport records a redacted summary of every exchange", "[ai][platform][log]") {
    test::FakeTransport fake;
    RequestLog log(10);
    Redactor redactor;
    LoggingTransport logging(fake, log, redactor);
    fake.pushJson(200, R"({"ok":true})");
    fake.pushJson(401, R"({"error":{"message":"bad key","type":"authentication_error"}})");

    HttpRequest request;
    request.url = "https://api.example.test/v1/x?key=SECRETKEY12345";
    request.headers = {{"Authorization", "Bearer sk-live-0123456789abcdefghij"}};
    request.body = Json{{"image", std::string(5000, 'A')}, {"prompt", "describe"}}.dump();
    REQUIRE(logging.send(request, {}, {}));
    REQUIRE(logging.send(request, {}, {}));

    const auto entries = log.entries();
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].status == 200);
    CHECK(entries[0].error.empty());
    CHECK(entries[0].url.find("SECRETKEY") == std::string::npos);
    CHECK(entries[0].requestSummary.find("<5000 chars>") != std::string::npos);
    CHECK(entries[0].requestSummary.find("describe") != std::string::npos);
    CHECK(entries[0].bytesSent == static_cast<std::int64_t>(request.body.size()));
    CHECK(entries[1].error == "Unauthorized");
    const std::string dumped = entries[1].toJson().dump();
    CHECK(dumped.find("sk-live") == std::string::npos);
}

TEST_CASE("RequestLog keeps a bounded ring and appends JSON lines", "[ai][platform][log]") {
    const auto file = test::uniqueTempPath("wabcut-ai-requestlog", ".jsonl");
    std::filesystem::remove(file);
    RequestLog log(2);
    log.setFile(file);
    for (int i = 0; i < 3; ++i) {
        RequestLogEntry e;
        e.kind = RequestLogEntry::Kind::Call;
        e.provider = "p" + std::to_string(i);
        e.cost = 1234;
        log.add(e);
    }
    const auto entries = log.entries();
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].provider == "p1");
    const std::string content = test::readFile(file.string());
    CHECK(std::count(content.begin(), content.end(), '\n') == 3);
    CHECK(content.find("\"cost\":\"$0.001234\"") != std::string::npos);
    std::filesystem::remove(file);
    const std::string now = nowIso8601();
    CHECK(now.size() == 20);
    CHECK(now.back() == 'Z');
}

TEST_CASE("HttpDownloadTransport serves ModelStore over IHttpTransport (Range, sizes, errors)", "[ai][platform][download]") {
    const std::string payload = "pretend-model-weights-0123456789";
    test::FakeTransport fake;
    HttpDownloadTransport downloads(fake);

    // A fresh download: 200 with Content-Length.
    test::ScriptedResponse full;
    full.headers = {{"Content-Length", std::to_string(payload.size())}};
    full.body = payload;
    full.chunkSize = 5;
    fake.push(full);
    std::string received;
    std::vector<IDownloadTransport::Response> heads;
    auto r = downloads.get("https://example.test/m.bin", 0,
                           [&](const IDownloadTransport::Response& h) {
                               heads.push_back(h);
                               return true;
                           },
                           [&](std::span<const std::byte> bytes) {
                               received.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                               return true;
                           },
                           {});
    REQUIRE(r);
    CHECK(received == payload);
    REQUIRE(heads.size() == 1);
    CHECK(heads[0].status == 200);
    CHECK(heads[0].totalSize == payload.size());
    CHECK(fake.requests().back().responseMode == BodyMode::Raw);
    CHECK_FALSE(findHeader(fake.requests().back().headers, "Range"));

    // A resumed download: Range request, 206 with Content-Range.
    test::ScriptedResponse partial;
    partial.status = 206;
    partial.headers = {{"Content-Range", "bytes 10-31/32"}, {"Content-Length", "22"}};
    partial.body = payload.substr(10);
    fake.push(partial);
    heads.clear();
    REQUIRE(downloads.get("https://example.test/m.bin", 10, [&](const auto& h) { heads.push_back(h); return true; },
                          [](std::span<const std::byte>) { return true; }, {}));
    CHECK(findHeader(fake.requests().back().headers, "Range") == "bytes=10-");
    CHECK(heads.at(0).status == 206);
    CHECK(heads.at(0).totalSize == 32);

    // HTTP errors reach onResponse once (the store then reports NotFound), network errors are IoError.
    fake.pushJson(404, "{}");
    heads.clear();
    REQUIRE(downloads.get("https://example.test/missing.bin", 0, [&](const auto& h) { heads.push_back(h); return true; },
                          [](std::span<const std::byte>) { return true; }, {}));
    REQUIRE(heads.size() == 1);
    CHECK(heads[0].status == 404);
    fake.pushError(AiErrorCode::Network, "reset");
    CHECK(downloads.get("https://example.test/m.bin", 0, {}, {}, {}).error().code == wabcut::ErrorCode::IoError);

    // End to end through ModelStore: download, verify the SHA-256, install.
    const auto root = test::uniqueTempPath("wabcut-ai-httpdownload-store");
    std::filesystem::remove_all(root);
    ModelStore store(root);
    ModelInfo model;
    model.id = "tiny-test";
    model.fileName = "tiny-test.bin";
    model.url = "https://example.test/tiny-test.bin";
    model.size = payload.size();
    model.sha256 = Sha256::hex(payload);
    model.license = "MIT";
    fake.push(full);
    auto path = store.ensure(model, &downloads);
    INFO((path ? std::string() : path.error().message));
    REQUIRE(path);
    CHECK(store.isInstalled(model));
    CHECK(test::readFile(path->string()) == payload);
    std::filesystem::remove_all(root);
}
