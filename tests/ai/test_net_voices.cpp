// r07 provider contracts on fake transports: the new text-to-speech adapters (Deepgram Aura, Cartesia, Fish Audio), the
// OpenAI-compatible speech adapter on MP3 answers, the dedicated translation APIs (DeepL, Google Cloud Translation) through
// AiPlatform::translator(), and the registry's header auth (models.json "auth": "header"). Request shape, auth header,
// error mapping, cancel. No network, no key beyond a fixture string.

#include "FakeTransport.h"

#include <wabcut/ai/Provider.h>
#include <wabcut/ai/Redact.h>
#include <wabcut/ai/Transport.h>
#include <wabcut/ai_net/Adapters.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::test;

namespace {

/// A 16-bit mono WAV of `seconds` of silence at 24 kHz.
std::string wavBytes(double seconds) {
    const auto frames = static_cast<std::uint32_t>(seconds * 24000);
    std::string out = "RIFF";
    const auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
        }
    };
    const auto u16 = [&](std::uint16_t v) {
        out.push_back(static_cast<char>(v & 0xFF));
        out.push_back(static_cast<char>(v >> 8));
    };
    u32(36 + frames * 2);
    out += "WAVEfmt ";
    u32(16);
    u16(1);
    u16(1);
    u32(24000);
    u32(48000);
    u16(2);
    u16(16);
    out += "data";
    u32(frames * 2);
    out.append(frames * 2, '\0');
    return out;
}

/// One MPEG-1 Layer III frame at 128 kbps, 44.1 kHz (417 bytes, 1152 samples), repeated.
std::string mp3Bytes(int frames) {
    std::string frame("\xFF\xFB\x90\x00", 4);
    frame.resize(417, '\0');
    std::string out;
    for (int i = 0; i < frames; ++i) {
        out += frame;
    }
    return out;
}

ScriptedResponse audio(std::string body, std::string type) {
    ScriptedResponse r;
    r.headers = {{"content-type", std::move(type)}};
    r.body = std::move(body);
    return r;
}

struct Fixture {
    ModelRegistry models = ModelRegistry::builtin();
    ProviderRegistry providers{models};
    MemorySecretStore secrets;
    KeyRing keys{secrets};
    ConsentLedger consent;
    SpendGuard spend;
    RequestLog log;
    FakeTransport fake;
    AiPlatform ai{providers, keys, consent, spend, log, fake};

    Fixture() {
        net::registerCloudAdapters(providers);
        for (const char* p : {"deepgram", "cartesia", "fish-audio", "deepl", "deepl-free", "google-translate", "openai"}) {
            REQUIRE(models.provider(p) != nullptr);
            (void)keys.save(*models.provider(p), Secret(std::string("fixture-only-") + p + "-key"));
        }
    }
    static CallOptions allowed(std::optional<MicroUsd> cap = MicroUsd{1'000'000}) { return CallOptions{true, cap}; }
    static SpeechRequest line(std::string voice, std::string text = "Welcome back.") {
        SpeechRequest r;
        r.text = std::move(text);
        r.voiceId = std::move(voice);
        r.language = "en";
        return r;
    }
    static TranslationRequest captions() {
        TranslationRequest r;
        r.sourceLanguage = "en";
        r.targetLanguage = "pt";
        r.context = "captions of a cooking video";
        r.segments = {{"c1", "Hello there."}, {"c2", "Add the salt now."}};
        return r;
    }
};

} // namespace

TEST_CASE("Deepgram Aura speech: the voice is the model / WAV back / priced per character", "[ai][net][speech][r07]") {
    Fixture f;
    f.fake.push(audio(wavBytes(2.0), "audio/wav"));
    auto speech = f.ai.speech("aura-2", Fixture::allowed());
    REQUIRE(speech);
    auto spoken = (*speech)->synthesize(Fixture::line("thalia-en"), {});
    REQUIRE(spoken);
    CHECK(spoken->mediaType == "audio/wav");
    CHECK(spoken->usage.audioMilliseconds == 2000);
    CHECK(spoken->usage.characters == 13);
    const auto requests = f.fake.requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].url ==
          "https://api.deepgram.com/v1/speak?model=aura-2-thalia-en&encoding=linear16&container=wav&sample_rate=24000&mip_opt_out=true");
    CHECK(findHeader(requests[0].headers, "Authorization") == "Token fixture-only-deepgram-key");
    CHECK_FALSE(requests[0].retrySafe);
    CHECK(Json::parse(requests[0].body) == Json{{"text", "Welcome back."}});
    // 13 characters at $0.030 per 1,000.
    CHECK(f.log.entries().back().cost == 390);

    // A full voice id is sent as it is; a path is refused before anything is sent; 2,000 characters at most.
    f.fake.push(audio(wavBytes(1.0), "audio/wav"));
    REQUIRE((*speech)->synthesize(Fixture::line("aura-2-hector-fr"), {}));
    CHECK(f.fake.requests()[1].url.find("model=aura-2-hector-fr&") != std::string::npos);
    CHECK_FALSE((*speech)->synthesize(Fixture::line("../v1/listen"), {}));
    auto tooLong = (*speech)->synthesize(Fixture::line("thalia-en", std::string(2001, 'a')), {});
    REQUIRE_FALSE(tooLong);
    CHECK(tooLong.error().code == AiErrorCode::PayloadTooLarge);
    CHECK(f.fake.requests().size() == 2);

    // An HTML error page with 200 is not audio.
    f.fake.push(audio("<html>oops</html>", "text/html"));
    auto notAudio = (*speech)->synthesize(Fixture::line("thalia-en"), {});
    REQUIRE_FALSE(notAudio);
    CHECK(notAudio.error().code == AiErrorCode::ProviderError);
}

TEST_CASE("Cartesia speech: bytes endpoint / version header / voice id and WAV format", "[ai][net][speech][r07]") {
    Fixture f;
    f.fake.push(audio(wavBytes(1.5), "audio/wav"));
    auto speech = f.ai.speech("sonic-3.6", Fixture::allowed());
    REQUIRE(speech);
    SpeechRequest request = Fixture::line("db6b0ed5-d5d3-463d-ae85-518a07d3c2b4");
    request.language = "en-GB";
    auto spoken = (*speech)->synthesize(request, {});
    REQUIRE(spoken);
    CHECK(spoken->usage.audioMilliseconds == 1500);
    const HttpRequest sent = f.fake.requests().at(0);
    CHECK(sent.url == "https://api.cartesia.ai/tts/bytes");
    CHECK(findHeader(sent.headers, "Authorization") == "Bearer fixture-only-cartesia-key");
    CHECK(findHeader(sent.headers, "Cartesia-Version") == "2026-08-14");
    const Json body = Json::parse(sent.body);
    CHECK(body["model_id"] == "sonic-3.6");
    CHECK(body["transcript"] == "Welcome back.");
    CHECK(body["voice"] == "db6b0ed5-d5d3-463d-ae85-518a07d3c2b4"); // QA-09: the id string of the 2026-08-14 docs
    CHECK(body["output_format"]["container"] == "wav");
    CHECK(body["language"] == "en");

    f.fake.pushJson(401, R"({"error":"invalid api key"})");
    auto denied = (*speech)->synthesize(request, {});
    REQUIRE_FALSE(denied);
    CHECK(denied.error().code == AiErrorCode::Unauthorized);
}

TEST_CASE("Fish Audio speech: model header / reference voice / billed per UTF-8 byte", "[ai][net][speech][r07]") {
    Fixture f;
    f.fake.push(audio(wavBytes(1.0), "audio/wav"));
    auto speech = f.ai.speech("s2.1-pro", Fixture::allowed());
    REQUIRE(speech);
    auto spoken = (*speech)->synthesize(Fixture::line("bf322df2096a46f18c579d0baa36f41d", "สวัสดี"), {});
    REQUIRE(spoken);
    CHECK(spoken->usage.characters == 18); // six Thai characters are 18 bytes
    const HttpRequest sent = f.fake.requests().at(0);
    CHECK(sent.url == "https://api.fish.audio/v1/tts");
    CHECK(findHeader(sent.headers, "model") == "s2.1-pro");
    CHECK(findHeader(sent.headers, "Authorization") == "Bearer fixture-only-fish-audio-key");
    const Json body = Json::parse(sent.body);
    CHECK(body["reference_id"] == "bf322df2096a46f18c579d0baa36f41d");
    CHECK(body["format"] == "wav");

    // Cancelled before sending: nothing goes out.
    std::stop_source stop;
    stop.request_stop();
    f.fake.push(audio(wavBytes(1.0), "audio/wav"));
    auto cancelled = (*speech)->synthesize(Fixture::line("bf322df2096a46f18c579d0baa36f41d"), stop.get_token());
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == AiErrorCode::Cancelled);
}

TEST_CASE("OpenAI-compatible speech accepts an MP3 answer and measures it", "[ai][net][speech][r07]") {
    Fixture f;
    f.fake.push(audio(mp3Bytes(100), "audio/mpeg")); // 100 x 1152 samples at 44.1 kHz
    auto speech = f.ai.speech("gpt-4o-mini-tts", Fixture::allowed());
    REQUIRE(speech);
    auto spoken = (*speech)->synthesize(Fixture::line("marin"), {});
    REQUIRE(spoken);
    CHECK(spoken->mediaType == "audio/mpeg");
    CHECK(spoken->usage.audioMilliseconds == 2612);
}

TEST_CASE("DeepL translation: header auth / target variants / batches / ids kept / 456 is quota", "[ai][net][translate][r07]") {
    Fixture f;
    f.fake.pushJson(200, R"({"translations":[{"detected_source_language":"EN","text":"Olá."},{"detected_source_language":"EN","text":"Adicione o sal agora."}]})");
    auto translator = f.ai.translator("deepl-pro", Fixture::allowed());
    REQUIRE(translator);
    auto result = (*translator)->translate(Fixture::captions(), {});
    REQUIRE(result);
    REQUIRE(result->segments.size() == 2);
    CHECK(result->segments[0] == TranslationSegment{"c1", "Olá."});
    CHECK(result->segments[1].id == "c2");
    CHECK(result->usage.characters == 29);
    const HttpRequest sent = f.fake.requests().at(0);
    CHECK(sent.url == "https://api.deepl.com/v2/translate");
    CHECK(findHeader(sent.headers, "Authorization") == "DeepL-Auth-Key fixture-only-deepl-key");
    CHECK_FALSE(sent.retrySafe);
    const Json body = Json::parse(sent.body);
    CHECK(body["target_lang"] == "PT-BR");
    CHECK(body["source_lang"] == "EN");
    CHECK(body["context"] == "captions of a cooking video");
    CHECK(body["text"] == Json::array({"Hello there.", "Add the salt now."}));
    // 29 characters at $25 per 1M.
    CHECK(f.log.entries().back().cost == 725);

    // 120 lines go in three requests of at most 50.
    TranslationRequest many = Fixture::captions();
    many.segments.clear();
    for (int i = 0; i < 120; ++i) {
        many.segments.push_back({"c" + std::to_string(i), "line " + std::to_string(i)});
    }
    for (const int n : {50, 50, 20}) {
        Json translations = Json::array();
        for (int i = 0; i < n; ++i) {
            translations.push_back(Json{{"text", "linha"}});
        }
        f.fake.pushJson(200, Json{{"translations", translations}}.dump());
    }
    auto batched = (*translator)->translate(many, {});
    REQUIRE(batched);
    CHECK(batched->segments.size() == 120);
    CHECK(batched->segments[119].id == "c119");
    CHECK(f.fake.requests().size() == 4);

    // A short answer is an error, not a silently shifted track.
    f.fake.pushJson(200, R"({"translations":[{"text":"Olá."}]})");
    auto shortAnswer = (*translator)->translate(Fixture::captions(), {});
    REQUIRE_FALSE(shortAnswer);
    CHECK(shortAnswer.error().code == AiErrorCode::ProviderError);

    f.fake.pushJson(456, R"({"message":"Quota for this billing period has been exceeded"})");
    auto quota = (*translator)->translate(Fixture::captions(), {});
    REQUIRE_FALSE(quota);
    CHECK(quota.error().code == AiErrorCode::QuotaExceeded);

    // A language code that is not one is refused before anything is sent.
    TranslationRequest bad = Fixture::captions();
    bad.targetLanguage = "pt&auth_key=x";
    const auto before = f.fake.requests().size();
    CHECK_FALSE((*translator)->translate(bad, {}));
    CHECK(f.fake.requests().size() == before);
}

TEST_CASE("DeepL Free uses its own host; Google Cloud Translation batches and decodes entities", "[ai][net][translate][r07]") {
    Fixture f;
    f.fake.pushJson(200, R"({"translations":[{"text":"Hallo."},{"text":"Jetzt das Salz."}]})");
    auto free = f.ai.translator("deepl-free", Fixture::allowed());
    REQUIRE(free);
    TranslationRequest request = Fixture::captions();
    request.targetLanguage = "de";
    REQUIRE((*free)->translate(request, {}));
    CHECK(f.fake.requests().at(0).url == "https://api-free.deepl.com/v2/translate");
    CHECK(Json::parse(f.fake.requests().at(0).body)["target_lang"] == "DE");

    f.fake.pushJson(200, R"({"data":{"translations":[{"translatedText":"Bonjour."},{"translatedText":"Ajoutez le sel maintenant, c&#39;est l&#39;heure."}]}})");
    auto google = f.ai.translator("google-nmt", Fixture::allowed());
    REQUIRE(google);
    request.targetLanguage = "fr";
    auto translated = (*google)->translate(request, {});
    REQUIRE(translated);
    CHECK(translated->segments[1].text == "Ajoutez le sel maintenant, c'est l'heure.");
    const HttpRequest sent = f.fake.requests().at(1);
    CHECK(sent.url == "https://translation.googleapis.com/language/translate/v2");
    CHECK(findHeader(sent.headers, "x-goog-api-key") == "fixture-only-google-translate-key");
    const Json body = Json::parse(sent.body);
    CHECK(body["target"] == "fr");
    CHECK(body["source"] == "en");
    CHECK(body["format"] == "text");
    CHECK(body["q"].size() == 2);
}

TEST_CASE("Translation APIs keep consent / the no-cloud lock and the run cap; chat models are not translators", "[ai][net][translate][privacy][r07]") {
    Fixture f;
    auto noConsent = f.ai.translator("deepl-pro", CallOptions{false, MicroUsd{1'000'000}});
    REQUIRE(noConsent);
    auto refused = (*noConsent)->translate(Fixture::captions(), {});
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == AiErrorCode::ConsentRequired);

    f.consent.setProjectLock(true);
    auto locked = f.ai.translator("deepl-pro", Fixture::allowed());
    REQUIRE(locked);
    auto offline = (*locked)->translate(Fixture::captions(), {});
    REQUIRE_FALSE(offline);
    CHECK(offline.error().code == AiErrorCode::Offline);
    f.consent.setProjectLock(false);

    auto tight = f.ai.translator("deepl-pro", Fixture::allowed(MicroUsd{1}));
    REQUIRE(tight);
    auto capped = (*tight)->translate(Fixture::captions(), {});
    REQUIRE_FALSE(capped);
    CHECK(capped.error().code == AiErrorCode::SpendCapExceeded);
    CHECK(f.fake.requests().empty());

    CHECK(f.providers.hasAdapter("deepl", Capability::Translate));
    CHECK(f.providers.hasAdapter("google-translate", Capability::Translate));
    CHECK_FALSE(f.providers.hasAdapter("deepl", Capability::Chat));
    auto chat = f.ai.translator("gpt-6-luna", Fixture::allowed());
    REQUIRE_FALSE(chat);
    CHECK(chat.error().code == AiErrorCode::Unsupported);
}

TEST_CASE("Header auth in models.json: parsed / applied / written back / invalid headers refused", "[ai][registry][r07]") {
    const ModelRegistry& registry = ModelRegistry::builtin();
    const ProviderSpec* gladia = registry.provider("gladia");
    REQUIRE(gladia != nullptr);
    CHECK(gladia->auth == AuthScheme::Header);
    CHECK(gladia->authHeader == "x-gladia-key");
    HttpHeaders headers;
    applyAuth(headers, *gladia, "k-123");
    REQUIRE(headers.size() == 1);
    CHECK(headers[0] == HttpHeader{"x-gladia-key", "k-123"});
    headers.clear();
    applyAuth(headers, *registry.provider("fal"), "k-456");
    CHECK(headers.at(0) == HttpHeader{"Authorization", "Key k-456"});
    headers.clear();
    applyAuth(headers, *registry.provider("fal"), "");
    CHECK(headers.empty());

    // toJson() keeps header auth, so the registry round-trips.
    const auto again = ModelRegistry::fromJson(registry.toJson().dump());
    REQUIRE(again);
    CHECK(again->provider("deepl")->authPrefix == "DeepL-Auth-Key ");

    const auto docWith = [](std::string header) {
        return Json{{"schemaVersion", 1},
                    {"providers", Json::array({Json{{"id", "p"}, {"baseUrl", "https://example.com"}, {"auth", "header"}, {"authHeader", header}}})},
                    {"models", Json::array()}}
            .dump();
    };
    REQUIRE(ModelRegistry::fromJson(docWith("x-custom-key"))); // the control: a valid header name loads
    for (const char* header : {"", "Host", "Cookie", "x key", "x-key\r\nEvil: 1"}) {
        INFO(header);
        CHECK_FALSE(ModelRegistry::fromJson(docWith(header)));
    }

    // The new key headers are redacted by name too.
    CHECK(Redactor::global().redact("x-gladia-key: abc123secret").find("abc123secret") == std::string::npos);
}

TEST_CASE("Every r07 provider entry has an adapter for each of its models", "[ai][registry][r07]") {
    const ModelRegistry& registry = ModelRegistry::builtin();
    ProviderRegistry providers(registry);
    net::registerCloudAdapters(providers);
    for (const char* id : {"deepl", "deepl-free", "google-translate", "speechmatics", "gladia", "cartesia", "fish-audio", "stability", "bfl",
                           "ideogram", "fal", "replicate"}) {
        const ProviderSpec* provider = registry.provider(id);
        INFO(id);
        REQUIRE(provider != nullptr);
        CHECK_FALSE(provider->dataPolicy.empty());
        CHECK_FALSE(provider->consoleUrl.empty());
        CHECK(provider->source.starts_with("https://"));
        const auto models = registry.modelsOf(id);
        CHECK_FALSE(models.empty());
        for (const ModelSpec* model : models) {
            INFO(model->id);
            bool served = false;
            for (const Capability c : model->caps.capabilities) {
                served = served || providers.hasAdapter(model->api, c);
            }
            CHECK(served);
            CHECK_FALSE(model->prices.empty());
        }
    }
}
