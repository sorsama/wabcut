#include "FakeTransport.h"
#include <wabcut/ai_net/Adapters.h>
#include <wabcut/ai/Provider.h>
#include <wabcut/ai/Transport.h>
#include <catch2/catch_test_macros.hpp>
#include <tuple>
using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::test;
namespace {
// The start of an ISO media file ("ftyp" box): what a real generated MP4 begins with.
const std::string kMp4("\x00\x00\x00\x18" "ftypisom" "\x00\x00\x02\x00" "isomiso2", 24);
ScriptedResponse asset(std::string body, std::string type) {
    ScriptedResponse r;
    r.headers = {{"content-type", std::move(type)}};
    r.body = std::move(body);
    return r;
}
struct Fixture {
    ModelRegistry models = ModelRegistry::builtin();
    ProviderRegistry providers{models}; MemorySecretStore secrets; KeyRing keys{secrets};
    ConsentLedger consent; SpendGuard spend; RequestLog log; FakeTransport fake;
    AiPlatform ai{providers, keys, consent, spend, log, fake};
    std::filesystem::path directory = uniqueTempPath("wabcut-generation");
    Fixture() {
        net::registerCloudAdapters(providers);
        (void)keys.save(*models.provider("runway"), Secret("fixture-only-runway-key"));
        (void)keys.save(*models.provider("gemini"), Secret("fixture-only-google-key"));
        ai.setPolling([](auto, auto) { return true; }, std::chrono::milliseconds(0), std::chrono::seconds(1));
    }
    ~Fixture() { std::error_code ec; std::filesystem::remove_all(directory, ec); }
    MediaGenerationRequest request() { MediaGenerationRequest r; r.prompt = "aerial city"; r.duration = Ticks::fromSeconds(4); return r; }
    void runway() {
        fake.pushJson(200, R"({"id":"fixture-job"})");
        fake.pushJson(200, R"({"status":"SUCCEEDED","output":["https://dnznrvs05pmza.cloudfront.net/output.mp4?_jwt=fixture"]})");
        fake.push(asset(kMp4, "video/mp4"));
    }
};
}
TEST_CASE("Generation adapters submit poll download with fixed price and scoped credentials", "[ai][generation][net]") {
    Fixture f; f.runway();
    auto result = f.ai.generate("gen4.5", f.request(), f.directory, {}, {}, {true, 480000});
    REQUIRE(result); CHECK(result->cost == 480000); REQUIRE(result->files.size() == 1);
    CHECK(std::filesystem::file_size(result->files.front()) == 24);
    CHECK(result->files.front().extension() == ".mp4");
    const auto requests = f.fake.requests(); REQUIRE(requests.size() == 3);
    CHECK_FALSE(requests[0].retrySafe);
    const auto body = Json::parse(requests[0].body);
    CHECK(body["duration"] == 4); CHECK(body["ratio"] == "1280:720");
    CHECK(findHeader(requests[0].headers, "Authorization").has_value());
    CHECK_FALSE(findHeader(requests[2].headers, "Authorization"));
}
TEST_CASE("Veo contract uses an operation and authenticated Google-only download", "[ai][generation][net]") {
    Fixture f;
    f.fake.pushJson(200, R"({"name":"models/veo-3/operations/fixture"})");
    f.fake.pushJson(200, R"({"done":true,"response":{"generateVideoResponse":{"generatedSamples":[{"video":{"uri":"https://generativelanguage.googleapis.com/v1beta/files/output:download"}}]}}})");
    f.fake.push(asset(kMp4, "application/octet-stream"));
    auto result = f.ai.generate("veo-3.1-fast-generate-preview", f.request(), f.directory, {}, {}, {true, 400000});
    REQUIRE(result); CHECK(result->cost == 400000);
    const auto requests = f.fake.requests(); REQUIRE(requests.size() == 3);
    CHECK(Json::parse(requests[0].body)["parameters"]["durationSeconds"] == "4");
    CHECK(findHeader(requests[2].headers, "x-goog-api-key").has_value());
}
TEST_CASE("Generation refuses no consent offline no-cloud missing cap and exceeded cap before dispatch", "[ai][generation][security]") {
    Fixture f;
    SECTION("consent") { auto r = f.ai.generate("gen4.5", f.request(), f.directory, {}, {}, {false, 480000}); REQUIRE_FALSE(r); CHECK(r.error().code == AiErrorCode::ConsentRequired); }
    SECTION("offline") { f.consent.setOfflineMode(true); CHECK_FALSE(f.ai.generate("gen4.5", f.request(), f.directory, {}, {}, {true, 480000})); }
    SECTION("project") { f.consent.setProjectLock(true); CHECK_FALSE(f.ai.generate("gen4.5", f.request(), f.directory, {}, {}, {true, 480000})); }
    SECTION("cap") { CHECK_FALSE(f.ai.generate("gen4.5", f.request(), f.directory, {}, {}, {true, 1})); }
    SECTION("no cap") { CHECK_FALSE(f.ai.generate("gen4.5", f.request(), f.directory, {}, {}, {true})); }
    SECTION("monthly cap") { f.spend.setMonthlyCap("runway", 100); CHECK_FALSE(f.ai.generate("gen4.5", f.request(), f.directory, {}, {}, {true, 480000})); }
    CHECK(f.fake.requests().empty());
}
TEST_CASE("Generation refuses malicious download and operation URLs without leaking credentials", "[ai][generation][security]") {
    Fixture f;
    SECTION("operation") { f.fake.pushJson(200, R"({"name":"https://evil.example/job"})"); CHECK_FALSE(f.ai.generate("veo-3.1-fast-generate-preview", f.request(), f.directory, {}, {}, {true, 480000})); CHECK(f.fake.requests().size() == 1); }
    SECTION("download") {
        f.fake.pushJson(200, R"({"id":"fixture"})");
        f.fake.pushJson(200, R"({"status":"SUCCEEDED","output":["https://evil.example/output.mp4"]})");
        CHECK_FALSE(f.ai.generate("gen4.5", f.request(), f.directory, {}, {}, {true, 480000})); CHECK(f.fake.requests().size() == 2);
    }
}
TEST_CASE("Cancellation calls Runway DELETE and releases reservations conservatively", "[ai][generation][net]") {
    Fixture f; f.fake.pushJson(200, R"({"id":"fixture"})"); f.fake.pushJson(204, "");
    std::stop_source stop; GenerationEvents events;
    events.onSubmitted = [&](const auto&) { stop.request_stop(); };
    auto r = f.ai.generate("gen4.5", f.request(), f.directory, events, stop.get_token(), {true, 480000});
    REQUIRE_FALSE(r); CHECK(r.error().code == AiErrorCode::Cancelled);
    REQUIRE(f.fake.requests().size() == 2); CHECK(f.fake.requests()[1].method == "DELETE");
    // Review wave 4, P2-8: the best-effort cancel cannot hold shutdown for the default 10 minutes or retry with back-off.
    CHECK(f.fake.requests()[1].timeout <= std::chrono::seconds(5));
    CHECK(f.fake.requests()[1].idleTimeout <= std::chrono::seconds(5));
    CHECK_FALSE(f.fake.requests()[1].retrySafe);
    CHECK(f.spend.reserved("runway") == 0);
}
TEST_CASE("Generation rechecks offline mode between poll and download", "[ai][generation][security]") {
    Fixture f; f.runway(); GenerationEvents events;
    events.onStatus = [&](const auto&) { f.consent.setOfflineMode(true); };
    CHECK_FALSE(f.ai.generate("gen4.5", f.request(), f.directory, events, {}, {true, 480000}));
    CHECK(f.fake.requests().size() == 2);
}
TEST_CASE("Non-idempotent paid submissions never retry ambiguous failures", "[ai][generation][security]") {
    FakeTransport fake; fake.pushError(AiErrorCode::Network);
    RetryingTransport retry(fake, {}, [](auto,auto) { return true; });
    HttpRequest request; request.url = "https://api.dev.runwayml.com/v1/text_to_video"; request.retrySafe = false;
    CHECK_FALSE(retry.send(request, {}, {})); CHECK(fake.requests().size() == 1);
}
TEST_CASE("Provider quota and malformed JSON errors are surfaced", "[ai][generation][net]") {
    Fixture f;
    SECTION("quota") { f.fake.pushJson(429, R"({"error":{"code":"insufficient_quota","message":"quota"}})"); }
    SECTION("malformed") { f.fake.pushJson(200, "["); }
    CHECK_FALSE(f.ai.generate("gen4.5", f.request(), f.directory, {}, {}, {true, 480000}));
    CHECK(f.fake.requests().size() == 1); CHECK(f.spend.reserved("runway") == 0);
}
TEST_CASE("Global offline gate covers raw downloads and composes retry authorization", "[ai][generation][security]") {
    ConsentLedger consent; FakeTransport fake;
    RetryingTransport retry(fake, {}, [&](auto,auto) { consent.setOfflineMode(true); return true; });
    OfflineGateTransport gate(retry, consent);
    HttpRequest request; request.method = "GET"; request.url = "https://huggingface.co/model";
    consent.setOfflineMode(true);
    CHECK_FALSE(gate.send(request, {}, {})); CHECK(fake.requests().empty());
    request.url = "http://127.0.0.1:43210/v1/models"; fake.pushJson(200, "{}");
    CHECK(gate.send(request, {}, {})); CHECK(fake.requests().size() == 1);
    consent.setOfflineMode(false); request.url = "https://github.com/ggml-org/llama.cpp";
    fake.pushJson(503, "{}"); CHECK_FALSE(gate.send(request, {}, {})); CHECK(fake.requests().size() == 2);
    consent.setOfflineMode(false);
    request.beforeDispatch.check = []() -> std::expected<void, AiError> { return std::unexpected(AiError{AiErrorCode::ConsentRequired, "revoked"}); };
    CHECK_FALSE(gate.send(request, {}, {})); CHECK(fake.requests().size() == 2);
}

// Review wave 4, P2-7: a 200 that is not the asset (an HTML error page from a CDN, JSON) fails before it is imported,
// and the extension comes from what arrived, not from what was asked for.
TEST_CASE("Generated downloads are checked by content type and signature", "[ai][generation][net]") {
    for (const auto& [body, type, ok] : {std::tuple{std::string("<html><body>Access denied</body></html>"), std::string("text/html"), false},
                                         std::tuple{std::string("{\"error\":\"expired\"}"), std::string("application/json"), false},
                                         std::tuple{kMp4, std::string("text/html"), false},     // the type disagrees
                                         std::tuple{std::string("not a video at all"), std::string("application/octet-stream"), false},
                                         std::tuple{kMp4, std::string("video/mp4; codecs=avc1"), true}}) {
        INFO(type << " " << body.size());
        Fixture f;
        f.fake.pushJson(200, R"({"id":"fixture-job"})");
        f.fake.pushJson(200, R"({"status":"SUCCEEDED","output":["https://dnznrvs05pmza.cloudfront.net/output.mp4?_jwt=fixture"]})");
        f.fake.push(asset(body, type));
        auto result = f.ai.generate("gen4.5", f.request(), f.directory, {}, {}, {true, 480000});
        CHECK(result.has_value() == ok);
        if (!ok) {
            std::size_t files = 0;
            for (const auto& entry : std::filesystem::directory_iterator(f.directory)) {
                files += entry.is_regular_file() ? 1 : 0;
            }
            CHECK(files == 0); // nothing half-written is left behind
        }
    }
}
