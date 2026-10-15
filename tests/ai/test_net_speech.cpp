// AI-12 provider contracts on recorded response shapes (fixtures/elevenlabs, fixtures/gemini, fixtures/anthropic): the
// three text-to-speech adapters through the guarded platform (consent, the no-cloud lock, the run's cap, quota and
// cancel), ElevenLabs' instant voice cloning (multipart, deleted again), and caption translation by the user's chat
// model through the same guard. No network, no key beyond a fixture string.

#include "FakeTransport.h"

#include <wabcut/ai/Provider.h>
#include <wabcut/ai/Transport.h>
#include <wabcut/ai/translate/CaptionTranslation.h>
#include <wabcut/ai_net/Adapters.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::test;

namespace {

std::string fixture(const std::string& name) { return readFile(std::string(WABCUT_AI_FIXTURES) + "/" + name); }

/// A 16-bit mono WAV of `seconds` of silence at 24 kHz, as OpenAI sends it.
std::string wav(double seconds) {
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
        for (const char* p : {"elevenlabs", "openai", "gemini", "anthropic"}) {
            (void)keys.save(*models.provider(p), Secret(std::string("fixture-only-") + p + "-key"));
        }
    }
    static CallOptions allowed(std::optional<MicroUsd> cap = MicroUsd{1'000'000}) { return CallOptions{true, cap}; }
    SpeechRequest line(std::string voice, std::string text = "สวัสดี") {
        SpeechRequest r;
        r.text = std::move(text);
        r.voiceId = std::move(voice);
        r.language = "th";
        return r;
    }
};

} // namespace

TEST_CASE("ElevenLabs speech: the timestamps endpoint, characters billed, alignment read", "[ai][net][speech][elevenlabs]") {
    Fixture f;
    f.fake.pushJson(200, fixture("elevenlabs/tts_with_timestamps.json"));
    auto speech = f.ai.speech("eleven_v3", Fixture::allowed());
    REQUIRE(speech);
    auto result = (*speech)->synthesize(f.line("JBFqnCBsd6RMkjVDRZzb"), {});
    REQUIRE(result);
    CHECK(result->mediaType == "audio/mpeg");
    CHECK(result->audio.size() > 16);
    REQUIRE(result->alignment.size() == 6);
    CHECK(result->alignment.back().t1 == Ticks{kTicksPerSecond * 743 / 1000});
    CHECK(result->usage.characters == 6);
    const auto requests = f.fake.requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].url == "https://api.elevenlabs.io/v1/text-to-speech/JBFqnCBsd6RMkjVDRZzb/with-timestamps?output_format=mp3_44100_128");
    CHECK_FALSE(requests[0].retrySafe); // a network ambiguity must not bill a line twice
    CHECK(findHeader(requests[0].headers, "xi-api-key").has_value());
    const Json body = Json::parse(requests[0].body);
    CHECK(body["text"] == "สวัสดี");
    CHECK(body["model_id"] == "eleven_v3");
    // Billed per character: 6 characters at $0.08 per 1,000.
    REQUIRE_FALSE(f.log.entries().empty());
    CHECK(f.log.entries().back().cost == 480);

    // Out of credits: QuotaExceeded, said plainly.
    f.fake.pushJson(402, fixture("elevenlabs/no_credits.json"));
    auto broke = (*speech)->synthesize(f.line("JBFqnCBsd6RMkjVDRZzb"), {});
    REQUIRE_FALSE(broke);
    CHECK(broke.error().code == AiErrorCode::QuotaExceeded);
    // A voice id that is not one is refused before anything is sent.
    CHECK_FALSE((*speech)->synthesize(f.line("../../v1/user"), {}));
    CHECK(f.fake.requests().size() == 2);
}

TEST_CASE("OpenAI speech: WAV back, priced by its length; Gemini speech: the interactions API", "[ai][net][speech]") {
    Fixture f;
    ScriptedResponse r;
    r.headers = {{"content-type", "audio/wav"}};
    r.body = wav(1.5);
    f.fake.push(r);
    auto openai = f.ai.speech("gpt-4o-mini-tts", Fixture::allowed());
    REQUIRE(openai);
    SpeechRequest line = f.line("marin", "Welcome back.");
    line.style = "warm, unhurried";
    auto spoken = (*openai)->synthesize(line, {});
    REQUIRE(spoken);
    CHECK(spoken->mediaType == "audio/wav");
    CHECK(spoken->usage.audioMilliseconds == 1500);
    Json body = Json::parse(f.fake.requests()[0].body);
    CHECK(body["voice"] == "marin");
    CHECK(body["instructions"] == "warm, unhurried");
    CHECK(body["response_format"] == "wav");
    CHECK(f.fake.requests()[0].url == "https://api.openai.com/v1/audio/speech");

    f.fake.pushJson(200, fixture("gemini/tts_interaction.json"));
    auto gemini = f.ai.speech("gemini-3.8-flash-tts", Fixture::allowed());
    REQUIRE(gemini);
    auto said = (*gemini)->synthesize(f.line("Kore"), {});
    REQUIRE(said);
    CHECK(said->usage.audioMilliseconds == 100);
    const auto requests = f.fake.requests();
    CHECK(requests[1].url == "https://generativelanguage.googleapis.com/v1beta/interactions");
    body = Json::parse(requests[1].body);
    CHECK(body["generation_config"]["speech_config"][0]["voice"] == "Kore");
    CHECK(body["response_format"]["type"] == "audio");
    CHECK(findHeader(requests[1].headers, "x-goog-api-key").has_value());
}

TEST_CASE("Speech is refused without consent, under the no-cloud lock and above the run's cap; a cancel stops it",
          "[ai][net][speech][privacy]") {
    Fixture f;
    auto noConsent = f.ai.speech("eleven_v3", CallOptions{false, MicroUsd{1'000'000}});
    REQUIRE(noConsent);
    auto refused = (*noConsent)->synthesize(f.line("JBFqnCBsd6RMkjVDRZzb"), {});
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == AiErrorCode::ConsentRequired);

    f.consent.setProjectLock(true);
    auto locked = f.ai.speech("eleven_v3", Fixture::allowed());
    REQUIRE(locked);
    auto offline = (*locked)->synthesize(f.line("JBFqnCBsd6RMkjVDRZzb"), {});
    REQUIRE_FALSE(offline);
    CHECK(offline.error().code == AiErrorCode::Offline);
    f.consent.setProjectLock(false);

    auto tight = f.ai.speech("eleven_v3", Fixture::allowed(MicroUsd{1}));
    REQUIRE(tight);
    auto capped = (*tight)->synthesize(f.line("JBFqnCBsd6RMkjVDRZzb", std::string(2000, 'a')), {});
    REQUIRE_FALSE(capped);
    CHECK(capped.error().code == AiErrorCode::SpendCapExceeded);
    CHECK(f.fake.requests().empty()); // nothing reached the provider in any of these

    std::stop_source stop;
    stop.request_stop();
    auto open = f.ai.speech("eleven_v3", Fixture::allowed());
    REQUIRE(open);
    f.fake.pushJson(200, fixture("elevenlabs/tts_with_timestamps.json"));
    auto cancelled = (*open)->synthesize(f.line("JBFqnCBsd6RMkjVDRZzb"), stop.get_token());
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == AiErrorCode::Cancelled);
}

// Review wave 5, A5-P2-14: deleting a cloned voice only removes the speaker's data, so neither a missing consent nor the
// no-cloud lock refuses it (they did, through the guarded call, and the voice stayed on the account); only the offline
// mode keeps it on this computer, as an error the dub reports.
TEST_CASE("Deleting a cloned voice is never refused by consent or the no-cloud lock", "[ai][net][speech][clone][privacy]") {
    Fixture f;
    auto speech = f.ai.speech("eleven_v3", CallOptions{false, MicroUsd{1'000'000}}); // no consent, no "Allow once"
    REQUIRE(speech);
    f.consent.setProjectLock(true);
    f.fake.pushJson(200, R"({"status":"ok"})");
    REQUIRE((*speech)->deleteVoice("c38kUX8pkfYO2kHyqfFy", {}));
    REQUIRE(f.fake.requests().size() == 1);
    CHECK(f.fake.requests()[0].method == "DELETE");
    f.consent.setProjectLock(false);
    f.consent.setOfflineMode(true);
    const auto offline = (*speech)->deleteVoice("c38kUX8pkfYO2kHyqfFy", {});
    REQUIRE_FALSE(offline);
    CHECK(offline.error().code == AiErrorCode::Offline);
    CHECK(f.fake.requests().size() == 1); // nothing more was sent
}

TEST_CASE("ElevenLabs instant voice cloning uploads the sample once and deletes the voice", "[ai][net][speech][clone]") {
    Fixture f;
    const auto sample = uniqueTempPath("wabcut-clone-sample", ".wav");
    {
        std::ofstream out(sample, std::ios::binary);
        const std::string w = wav(1.0);
        out.write(w.data(), static_cast<std::streamsize>(w.size()));
    }
    f.fake.pushJson(200, fixture("elevenlabs/voice_add.json"));
    f.fake.pushJson(200, R"({"status":"ok"})");
    auto speech = f.ai.speech("eleven_v3", Fixture::allowed());
    REQUIRE(speech);
    const std::filesystem::path samples[] = {sample};
    auto voice = (*speech)->cloneVoice(samples, "Host (Wabcut dub)", {});
    REQUIRE(voice);
    CHECK(voice->voiceId == "c38kUX8pkfYO2kHyqfFy");
    CHECK_FALSE(voice->requiresVerification);
    REQUIRE((*speech)->deleteVoice(voice->voiceId, {}));
    const auto requests = f.fake.requests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].url == "https://api.elevenlabs.io/v1/voices/add");
    const auto file = std::find_if(requests[0].multipart.begin(), requests[0].multipart.end(),
                                   [](const MultipartField& m) { return m.name == "files"; });
    REQUIRE(file != requests[0].multipart.end());
    CHECK(file->file == sample);
    CHECK(file->contentType == "audio/wav");
    CHECK(requests[1].method == "DELETE");
    CHECK(requests[1].url == "https://api.elevenlabs.io/v1/voices/c38kUX8pkfYO2kHyqfFy");
    // OpenAI has no cloning: Unsupported, nothing sent.
    auto openai = f.ai.speech("gpt-4o-mini-tts", Fixture::allowed());
    REQUIRE(openai);
    CHECK((*openai)->cloneVoice(samples, "x", {}).error().code == AiErrorCode::Unsupported);
    CHECK(f.fake.requests().size() == 2);
    std::error_code ec;
    std::filesystem::remove(sample, ec);
}

TEST_CASE("Caption translation by the user's chat model goes through consent, the lock and the request log",
          "[ai][net][translate]") {
    Fixture f;
    TranslationRequest request;
    request.sourceLanguage = "en";
    request.targetLanguage = "th";
    request.segments = {{"0", "Welcome"}, {"1", "Hello [bleep] friends"}};

    f.fake.push(sseBody(fixture("anthropic/translation_stream.sse")));
    auto model = f.ai.chat("claude-sonnet-5-5", CallOptions{true, {}});
    REQUIRE(model);
    translate::LlmTranslator translator(std::move(*model));
    auto result = translator.translate(request, {});
    REQUIRE(result);
    REQUIRE(result->segments.size() == 2);
    CHECK(result->segments[1].text == "สวัสดี [bleep] ครับ");
    const Json body = Json::parse(f.fake.requests().at(0).body);
    const std::string sent = body.dump();
    CHECK(sent.find("Hello [bleep] friends") != std::string::npos); // caption text and markers only
    CHECK(f.log.entries().back().cost > 0);

    // Under the project's no-cloud lock nothing is sent.
    f.consent.setProjectLock(true);
    auto lockedModel = f.ai.chat("claude-sonnet-5-5", CallOptions{true, {}});
    REQUIRE(lockedModel);
    translate::LlmTranslator locked(std::move(*lockedModel));
    auto refused = locked.translate(request, {});
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == AiErrorCode::Offline);
    CHECK(f.fake.requests().size() == 1);
}

// Review wave 5, S5-P2-9 / A5-P2-23: "Always allow" was recorded per provider x capability and covered whatever asked.
// The stock-voice speech consent let a person's recording be uploaded for cloning, and a call an extension started
// rode on the user's own consent. Cloning has a consent of its own; an extension's call always needs a fresh answer.
TEST_CASE("Remembered consent covers neither voice cloning nor an extension's calls", "[ai][net][speech][clone][privacy]") {
    Fixture f;
    const ProviderSpec& eleven = *f.models.provider("elevenlabs");
    f.consent.grantAlways(eleven, Capability::Speak, "2026-10-12T08:00:00Z");
    const auto sample = uniqueTempPath("wabcut-clone-consent", ".wav");
    {
        std::ofstream out(sample, std::ios::binary);
        const std::string w = wav(1.0);
        out.write(w.data(), static_cast<std::streamsize>(w.size()));
    }
    const std::filesystem::path samples[] = {sample};
    auto speech = f.ai.speech("eleven_v3", CallOptions{false, MicroUsd{1'000'000}});
    REQUIRE(speech);
    const auto refused = (*speech)->cloneVoice(samples, "Host (Wabcut dub)", {});
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == AiErrorCode::ConsentRequired);
    CHECK(f.fake.requests().empty()); // the recording never left

    // Its own consent lets it go; so does "Allow once" for the run.
    f.consent.grantAlways(eleven, Capability::CloneVoice, "2026-10-12T08:01:00Z");
    f.fake.pushJson(200, fixture("elevenlabs/voice_add.json"));
    CHECK((*speech)->cloneVoice(samples, "Host (Wabcut dub)", {}));
    f.consent.revoke("elevenlabs", Capability::CloneVoice);
    auto once = f.ai.speech("eleven_v3", Fixture::allowed());
    REQUIRE(once);
    f.fake.pushJson(200, fixture("elevenlabs/voice_add.json"));
    CHECK((*once)->cloneVoice(samples, "Host (Wabcut dub)", {}));

    // An extension's call: the user's "Always allow" for speech does not cover it, its own "Allow once" does.
    CallOptions extension{false, MicroUsd{1'000'000}};
    extension.caller = "extension:com.example.x";
    auto fromExtension = f.ai.speech("eleven_v3", extension);
    REQUIRE(fromExtension);
    const std::size_t sent = f.fake.requests().size();
    const auto denied = (*fromExtension)->synthesize(f.line("JBFqnCBsd6RMkjVDRZzb"), {});
    REQUIRE_FALSE(denied);
    CHECK(denied.error().code == AiErrorCode::ConsentRequired);
    CHECK(f.fake.requests().size() == sent);
    const ProviderSpec& deepgram = *f.models.provider("deepgram");
    f.consent.grantAlways(deepgram, Capability::Transcribe, "2026-10-12T08:02:00Z");
    CHECK(f.consent.authorize(deepgram, deepgram.baseUrl, Capability::Transcribe));
    const auto transcribe = f.consent.authorize(deepgram, deepgram.baseUrl, Capability::Transcribe, false, "extension:com.example.x");
    REQUIRE_FALSE(transcribe);
    CHECK(transcribe.error().code == AiErrorCode::ConsentRequired);
    CHECK(f.consent.authorize(deepgram, deepgram.baseUrl, Capability::Transcribe, true, "extension:com.example.x"));
    std::error_code ec;
    std::filesystem::remove(sample, ec);
}
