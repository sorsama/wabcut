// Recorded-fixture contract tests of the three cloud transcription adapters (Deepgram, ElevenLabs Scribe,
// AssemblyAI): request shape, normalisation to the shared Transcript (source Ticks, speaker ids, language,
// segments, provenance), usage, and error mapping. No network.

#include "NetTestSupport.h"

#include <wabcut/ai/Transport.h>
#include <wabcut/ai_net/Adapters.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace wabcut::ai;
using namespace wabcut::ai::test;
using wabcut::Ticks;
using namespace std::chrono_literals;

namespace {

Ticks ms(std::int64_t milliseconds) { return Ticks{milliseconds * (wabcut::kTicksPerSecond / 1000)}; }

struct AudioFile {
    std::filesystem::path path = uniqueTempPath("wabcut-ai-net-audio", ".flac");
    AudioFile() { std::ofstream(path, std::ios::binary) << "fLaC-test-audio"; }
    ~AudioFile() { std::filesystem::remove(path); }
};

TranscribeRequest requestFor(const AudioFile& audio) {
    TranscribeRequest r;
    r.audioFile = audio.path;
    r.mediaType = "audio/flac";
    r.audioMilliseconds = 6250;
    r.source.fingerprint = 0x1234;
    r.source.streamIndex = 1;
    r.source.path = "interview.mov";
    r.origin = Ticks::fromSeconds(60); // the extracted audio starts one minute into the source
    return r;
}

} // namespace

TEST_CASE("Deepgram: request, words in source time, speakers, utterance segments", "[ai][net][deepgram]") {
    AudioFile audio;
    FakeTransport fake;
    fake.push(json(200, "deepgram/response.json"));
    auto stt = net::makeDeepgramTranscriber(context("nova-3", fake));
    float lastProgress = 0.0f;
    auto r = stt->transcribe(requestFor(audio), [&](float p) { lastProgress = p; }, {});
    REQUIRE(r);

    const HttpRequest sent = fake.requests().at(0);
    CHECK(sent.url == "https://api.deepgram.com/v1/listen?model=nova-3&smart_format=true&utterances=true&mip_opt_out=true"
                      "&diarize_model=latest&detect_language=true");
    CHECK(findHeader(sent.headers, "Authorization") == "Token test-key-0123456789");
    CHECK(findHeader(sent.headers, "content-type") == "audio/flac");
    CHECK(sent.bodyFile == audio.path);

    const Transcript& t = r->transcript;
    REQUIRE(t.validate());
    CHECK(t.language == "en");
    CHECK(t.source.path == "interview.mov");
    REQUIRE(t.words.size() == 6);
    CHECK(t.words[0].text == "Welcome");
    CHECK(t.words[1].text == "back.");
    CHECK(t.words[0].t0 == Ticks::fromSeconds(60) + ms(80));
    CHECK(t.words[5].t1 == Ticks::fromSeconds(60) + ms(4500));
    CHECK(t.words[0].confidence == 0.99f);
    CHECK(t.words[0].speakerId == 0);
    CHECK(t.words[2].speakerId == 1);
    REQUIRE(t.segments.size() == 2);
    CHECK(t.segments[0].text == "Welcome back.");
    CHECK(t.segments[1].firstWord == 2);
    CHECK(t.segments[1].wordCount == 4);
    CHECK(t.segments[1].speakerId == 1);
    CHECK(t.provenance.engine == "deepgram");
    CHECK(t.provenance.modelId == "nova-3");
    CHECK(t.provenance.modelVersion == "2187e11a-3532-4498-b076-81fa530bdd49");
    CHECK(t.provenance.params.at("diarize") == "true");
    CHECK(r->usage.audioMilliseconds == 6250);
    CHECK(r->duration == ms(6250));
    CHECK(r->requestId == "c1a4b6f0-1b2c-4d3e-9f00-1234567890ab");
    CHECK(lastProgress == 1.0f);
}

TEST_CASE("Deepgram: language hint, errors and limits", "[ai][net][deepgram]") {
    AudioFile audio;
    FakeTransport fake;
    auto stt = net::makeDeepgramTranscriber(context("nova-3", fake));
    TranscribeRequest request = requestFor(audio);
    request.language = "th";
    request.diarize = false;
    fake.push(json(401, "deepgram/bad_key.json"));
    auto denied = stt->transcribe(request, {}, {});
    REQUIRE_FALSE(denied);
    CHECK(denied.error().code == AiErrorCode::Unauthorized);
    CHECK(denied.error().providerCode == "INVALID_AUTH");
    CHECK(fake.requests().at(0).url.ends_with("&mip_opt_out=true&language=th"));

    request.audioFile = audio.path.parent_path() / "missing-audio.flac";
    CHECK(stt->transcribe(request, {}, {}).error().code == AiErrorCode::InvalidRequest);
}

TEST_CASE("ElevenLabs Scribe: multipart request, words only, ISO 639-3 language, logprob confidence", "[ai][net][elevenlabs]") {
    AudioFile audio;
    FakeTransport fake;
    fake.push(json(200, "elevenlabs/response.json"));
    auto stt = net::makeElevenLabsTranscriber(context("scribe_v2", fake));
    TranscribeRequest request = requestFor(audio);
    request.speakerCount = 2;
    auto r = stt->transcribe(request, {}, {});
    REQUIRE(r);

    const HttpRequest sent = fake.requests().at(0);
    CHECK(sent.url == "https://api.elevenlabs.io/v1/speech-to-text");
    CHECK(findHeader(sent.headers, "xi-api-key") == "test-key-0123456789");
    auto field = [&](const std::string& name) -> const MultipartField* {
        for (const auto& f : sent.multipart) {
            if (f.name == name) {
                return &f;
            }
        }
        return nullptr;
    };
    REQUIRE(field("model_id"));
    CHECK(field("model_id")->value == "scribe_v2");
    REQUIRE(field("file"));
    CHECK(field("file")->file == audio.path);
    CHECK(field("file")->contentType == "audio/flac");
    CHECK(field("timestamps_granularity")->value == "word");
    CHECK(field("diarize")->value == "true");
    CHECK(field("num_speakers")->value == "2");
    CHECK(field("language_code") == nullptr); // auto-detect

    const Transcript& t = r->transcript;
    REQUIRE(t.validate());
    CHECK(t.language == "th");
    REQUIRE(t.words.size() == 4); // spacing and the audio event are dropped
    CHECK(t.words[0].text == "สวัสดี");
    CHECK(t.words[0].speakerId == 0); // "speaker_1" appeared first
    CHECK(t.words[2].speakerId == 1);
    CHECK(t.words[0].confidence > 0.95f);
    CHECK(t.words[0].confidence < 0.96f); // exp(-0.05)
    REQUIRE(t.segments.size() == 2);      // speaker change
    CHECK(t.segments[1].text == "Thank you.");
    CHECK(r->usage.audioMilliseconds == 3000);
    CHECK(r->requestId == "tr_0123456789abcdef");

    fake.push(json(402, "elevenlabs/no_credits.json"));
    CHECK(stt->transcribe(request, {}, {}).error().code == AiErrorCode::QuotaExceeded);
}

TEST_CASE("AssemblyAI: upload, submit, poll, milliseconds, speaker letters", "[ai][net][assemblyai]") {
    AudioFile audio;
    FakeTransport fake;
    fake.push(json(200, "assemblyai/upload.json"));
    fake.push(json(200, "assemblyai/queued.json"));
    fake.push(json(200, "assemblyai/processing.json"));
    fake.push(json(200, "assemblyai/completed.json"));
    net::AdapterOptions options;
    std::vector<std::chrono::milliseconds> waits;
    options.sleeper = [&](std::chrono::milliseconds d, std::stop_token) {
        waits.push_back(d);
        return true;
    };
    auto stt = net::makeAssemblyAiTranscriber(context("universal-3-5-pro", fake), options);
    std::vector<float> progress;
    auto r = stt->transcribe(requestFor(audio), [&](float p) { progress.push_back(p); }, {});
    REQUIRE(r);

    const auto requests = fake.requests();
    REQUIRE(requests.size() == 4);
    CHECK(requests[0].url == "https://api.assemblyai.com/v2/upload");
    CHECK(findHeader(requests[0].headers, "Authorization") == "test-key-0123456789");
    CHECK(requests[0].bodyFile == audio.path);
    CHECK(requests[1].url == "https://api.assemblyai.com/v2/transcript");
    const Json submit = requestBody(requests[1]);
    CHECK(submit["audio_url"] == "https://cdn.assemblyai.com/upload/0123abcd-uploaded-audio");
    CHECK(submit["speech_models"] == Json::array({"universal-3-5-pro", "universal-2"}));
    CHECK(submit["speaker_labels"] == true);
    CHECK(submit["language_detection"] == true);
    CHECK_FALSE(submit.contains("language_code"));
    CHECK(requests[2].method == "GET");
    CHECK(requests[2].url == "https://api.assemblyai.com/v2/transcript/5551722-f677-48a6-9287-39c0aafd9ac1");
    CHECK(waits.size() == 2);
    CHECK(waits[0] == 3000ms);

    const Transcript& t = r->transcript;
    REQUIRE(t.validate());
    CHECK(t.language == "en");
    REQUIRE(t.words.size() == 7);
    CHECK(t.words[0].t0 == Ticks::fromSeconds(60) + ms(250));
    CHECK(t.words[0].speakerId == 0);
    CHECK(t.words[5].speakerId == 1);
    CHECK_FALSE(t.words[6].speakerId);
    REQUIRE(t.segments.size() == 2);
    CHECK(t.segments[0].text == "Smoke from hundreds of fires.");
    CHECK(t.segments[1].text == "Yes, indeed.");
    CHECK(t.provenance.modelId == "universal-3-5-pro");
    CHECK(r->usage.audioMilliseconds == 5000);
    CHECK(r->requestId == "5551722-f677-48a6-9287-39c0aafd9ac1");
    CHECK(progress.back() == 1.0f);
    CHECK(std::is_sorted(progress.begin(), progress.end()));
}

TEST_CASE("AssemblyAI: provider errors, cancellation while polling, 429 retried", "[ai][net][assemblyai]") {
    AudioFile audio;
    FakeTransport fake;
    net::AdapterOptions options;
    options.sleeper = [](std::chrono::milliseconds, std::stop_token) { return true; };
    auto stt = net::makeAssemblyAiTranscriber(context("universal-3-5-pro", fake), options);

    fake.push(json(200, "assemblyai/upload.json"));
    fake.push(json(200, "assemblyai/queued.json"));
    fake.push(json(200, "assemblyai/failed.json"));
    auto failed = stt->transcribe(requestFor(audio), {}, {});
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == AiErrorCode::ProviderError);
    CHECK(failed.error().message.find("Download error") != std::string::npos);

    net::AdapterOptions cancelling;
    cancelling.sleeper = [](std::chrono::milliseconds, std::stop_token) { return false; };
    auto cancellable = net::makeAssemblyAiTranscriber(context("universal-3-5-pro", fake), cancelling);
    fake.push(json(200, "assemblyai/upload.json"));
    fake.push(json(200, "assemblyai/queued.json"));
    CHECK(cancellable->transcribe(requestFor(audio), {}, {}).error().code == AiErrorCode::Cancelled);

    // The upload hits a 429 with Retry-After; the retrying transport waits and the job completes.
    std::vector<std::chrono::milliseconds> waits;
    RetryingTransport retrying(fake, {}, [&](std::chrono::milliseconds d, std::stop_token) {
        waits.push_back(d);
        return true;
    });
    auto retried = net::makeAssemblyAiTranscriber(context("universal-3-5-pro", retrying), options);
    fake.push(json(429, "assemblyai/rate_limit.json", {{"Retry-After", "2"}}));
    fake.push(json(200, "assemblyai/upload.json"));
    fake.push(json(200, "assemblyai/completed.json"));
    REQUIRE(retried->transcribe(requestFor(audio), {}, {}));
    CHECK(waits == std::vector<std::chrono::milliseconds>{2000ms});
}

TEST_CASE("Cloud transcription through AiPlatform: consent, spend and the shared Transcript", "[ai][net][platform]") {
    AudioFile audio;
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
    REQUIRE(keys.save(*registry.provider("deepgram"), Secret("dg-test-key-0123456789")));
    consent.grantAlways(*registry.provider("deepgram"), Capability::Transcribe, "2026-10-11T08:00:00Z");
    spend.setMonthlyCap("deepgram", 10'000);

    auto stt = platform.transcriber("nova-3");
    REQUIRE(stt);
    fake.push(json(200, "deepgram/response.json"));
    auto r = (*stt)->transcribe(requestFor(audio), {}, {});
    REQUIRE(r);
    CHECK(r->transcript.words.size() == 6);
    const auto today = platform.today();
    CHECK(spend.spent("deepgram", {today.year(), today.month()}) == 448); // 6.25 s at $0.0043/min, rounded up
    const auto entries = log.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].capability == "transcribe");
    CHECK(entries[0].usage.audioMilliseconds == 6250);

    CHECK(providers.hasAdapter("anthropic-messages", Capability::Chat));
    CHECK(providers.hasAdapter("assemblyai", Capability::Transcribe));
    CHECK(providers.hasAdapter("anthropic-messages", Capability::Translate));
    CHECK_FALSE(providers.hasAdapter("openai-transcriptions", Capability::Transcribe)); // Voxtral: no adapter yet
}
