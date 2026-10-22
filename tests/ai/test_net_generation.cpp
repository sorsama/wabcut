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

// 0.2: generated sound effects (gen.sfx) through the user's ElevenLabs key: the request's fields, the MP3 checked and
// written, the price per minute of sound (30 s assumed when the model picks the length), the request log, and the
// same refusals as video and images.
namespace {
const std::string kMp3("ID3\x04\x00\x00\x00\x00\x00\x00" "\xff\xfb\x90\x64", 14);
MediaGenerationRequest sound(std::optional<double> seconds = 4.0) {
    MediaGenerationRequest r;
    r.kind = GeneratedMediaKind::SoundEffect;
    r.prompt = "footsteps on gravel";
    r.aspectRatio.clear();
    r.resolution.clear();
    if (seconds) {
        r.duration = Ticks{static_cast<std::int64_t>(*seconds * kTicksPerSecond)};
    }
    return r;
}
} // namespace

TEST_CASE("Sound effects: ElevenLabs sound generation through the user's key, priced per minute (0.2)", "[ai][generation][net][sfx]") {
    Fixture f;
    (void)f.keys.save(*f.models.provider("elevenlabs"), Secret("fixture-only-elevenlabs-key"));
    const ModelSpec* model = f.models.model("eleven_text_to_sound_v2");
    REQUIRE(model);
    CHECK(model->generation.soundEffect);
    CHECK(model->caps.has(Capability::GenerateMedia));
    // The estimate: $0.12 a minute; 4 s = $0.008; the model's own length is estimated at 30 s = $0.06.
    CHECK(estimateGenerationCost(*model, sound(4.0), todayUtc()).ceiling == 8000);
    CHECK(estimateGenerationCost(*model, sound(std::nullopt), todayUtc()).ceiling == 60000);
    CHECK(soundEffectSeconds(*model, sound(std::nullopt)) == 30.0);
    // What the model takes.
    CHECK(checkGenerationRequest(*model, sound(0.5)));
    CHECK_FALSE(checkGenerationRequest(*model, sound(0.2)));
    CHECK_FALSE(checkGenerationRequest(*model, sound(31.0)));
    auto empty = sound();
    empty.prompt.clear();
    CHECK_FALSE(checkGenerationRequest(*model, empty));
    auto influence = sound();
    influence.promptInfluence = 1.5;
    CHECK_FALSE(checkGenerationRequest(*model, influence));
    // A video model does not make sound effects, nor this one video.
    CHECK(checkGenerationRequest(*f.models.model("gen4.5"), sound()).error().code == AiErrorCode::Unsupported);
    auto video = f.request();
    CHECK(checkGenerationRequest(*model, video).error().code == AiErrorCode::Unsupported);

    auto request = sound(4.0);
    request.loop = true;
    request.promptInfluence = 0.6;
    f.fake.push(asset(kMp3, "audio/mpeg"));
    auto result = f.ai.generate(model->id, request, f.directory, {}, {}, {true, 8000});
    REQUIRE(result);
    CHECK(result->cost == 8000);
    CHECK(result->usage.audioMilliseconds == 4000);
    REQUIRE(result->files.size() == 1);
    CHECK(result->files.front().extension() == ".mp3");
    CHECK(std::filesystem::file_size(result->files.front()) == kMp3.size());
    const auto requests = f.fake.requests();
    REQUIRE(requests.size() == 1); // the answer is the file: no polling, no second download
    CHECK(requests[0].method == "POST");
    CHECK(requests[0].url == "https://api.elevenlabs.io/v1/sound-generation?output_format=mp3_44100_128");
    CHECK_FALSE(requests[0].retrySafe); // a network ambiguity must not bill a sound twice
    CHECK(findHeader(requests[0].headers, "xi-api-key") == "fixture-only-elevenlabs-key");
    const auto body = Json::parse(requests[0].body);
    CHECK(body["text"] == "footsteps on gravel");
    CHECK(body["model_id"] == "eleven_text_to_sound_v2");
    CHECK(body["duration_seconds"] == 4.0);
    CHECK(body["loop"] == true);
    CHECK(body["prompt_influence"] == 0.6);
    // The redacted request log has the call (provider, model, cost), never the key.
    bool logged = false;
    for (const RequestLogEntry& e : f.log.entries()) {
        logged = logged || (e.provider == "elevenlabs" && e.model == "eleven_text_to_sound_v2" && e.cost == 8000);
        CHECK(e.requestSummary.find("fixture-only-elevenlabs-key") == std::string::npos);
        CHECK(e.detail.find("fixture-only-elevenlabs-key") == std::string::npos);
    }
    CHECK(logged);

    // The model picks the length: no duration sent, the 30 s estimate must be approved.
    f.fake.push(asset(kMp3, "audio/mpeg"));
    CHECK_FALSE(f.ai.generate(model->id, sound(std::nullopt), f.directory, {}, {}, {true, 8000})); // above the run's limit
    auto picked = f.ai.generate(model->id, sound(std::nullopt), f.directory, {}, {}, {true, 60000});
    REQUIRE(picked);
    CHECK_FALSE(Json::parse(f.fake.requests().back().body).contains("duration_seconds"));

    // Something that is not an MP3 (an error page with a 200) is refused, nothing written.
    f.fake.push(asset("<html>busy</html>", "text/html"));
    CHECK_FALSE(f.ai.generate(model->id, sound(), f.directory, {}, {}, {true, 8000}));
}

namespace {
/// An MP3 of `frames` MPEG-1 Layer III frames (128 kbit/s, 44.1 kHz: 417 bytes and 1152 samples each) after an empty
/// ID3v2 tag.
std::string mp3Frames(int frames) {
    std::string out("ID3\x04\x00\x00\x00\x00\x00\x00", 10);
    for (int i = 0; i < frames; ++i) {
        std::string frame(417, '\0');
        frame[0] = '\xff', frame[1] = '\xfb', frame[2] = '\x90', frame[3] = '\x64';
        out += frame;
    }
    return out;
}
} // namespace

// 0.2 follow-up: the estimate assumes the longest sound (or the length asked for); the ledger is settled at the length
// that came back, measured from the MP3's frames, and never above the approved estimate.
TEST_CASE("A sound effect is booked at the length that came back, never above the estimate (0.2)", "[ai][generation][net][sfx]") {
    Fixture f;
    (void)f.keys.save(*f.models.provider("elevenlabs"), Secret("fixture-only-elevenlabs-key"));
    const std::string id = "eleven_text_to_sound_v2";
    const auto month = [] {
        const auto t = todayUtc();
        return std::chrono::year_month{t.year(), t.month()};
    }();
    // The model picked the length: estimated at 30 s ($0.06), 77 frames came back (2.011 s): booked at that.
    f.fake.push(asset(mp3Frames(77), "audio/mpeg"));
    auto picked = f.ai.generate(id, sound(std::nullopt), f.directory, {}, {}, {true, 60000});
    REQUIRE(picked);
    CHECK(picked->usage.audioMilliseconds == 2011); // 77 x 1152 / 44100 s
    CHECK(picked->cost == 4022);                     // 2.011 s at $0.12 a minute, rounded up
    CHECK(f.spend.spent("elevenlabs", month) == 4022);
    CHECK(f.spend.reserved("elevenlabs") == 0);
    // Asked for 4 s, a little more came back (the last frame): booked at the approved 4 s, not above.
    f.fake.push(asset(mp3Frames(154), "audio/mpeg")); // 4.023 s
    auto asked = f.ai.generate(id, sound(4.0), f.directory, {}, {}, {true, 8000});
    REQUIRE(asked);
    CHECK(asked->usage.audioMilliseconds == 4000);
    CHECK(asked->cost == 8000);
    CHECK(f.spend.spent("elevenlabs", month) == 4022 + 8000);
    // The file on disk is the whole answer.
    CHECK(std::filesystem::file_size(asked->files.front()) == 10 + 154 * 417);
}

TEST_CASE("Sound effects respect consent, offline, the no-cloud lock and caps before anything is sent (0.2)", "[ai][generation][security][sfx]") {
    Fixture f;
    (void)f.keys.save(*f.models.provider("elevenlabs"), Secret("fixture-only-elevenlabs-key"));
    const std::string id = "eleven_text_to_sound_v2";
    SECTION("consent") { CHECK(f.ai.generate(id, sound(), f.directory, {}, {}, {false, 8000}).error().code == AiErrorCode::ConsentRequired); }
    SECTION("offline") { f.consent.setOfflineMode(true); CHECK_FALSE(f.ai.generate(id, sound(), f.directory, {}, {}, {true, 8000})); }
    SECTION("no-cloud lock") { f.consent.setProjectLock(true); CHECK_FALSE(f.ai.generate(id, sound(), f.directory, {}, {}, {true, 8000})); }
    SECTION("cap") { CHECK_FALSE(f.ai.generate(id, sound(), f.directory, {}, {}, {true, 7999})); }
    SECTION("no cap") { CHECK_FALSE(f.ai.generate(id, sound(), f.directory, {}, {}, {true})); }
    CHECK(f.fake.requests().empty());
}
