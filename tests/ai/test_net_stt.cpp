// r07 speech-to-text contracts on fake transports: the OpenAI-compatible /audio/transcriptions adapter (OpenAI whisper-1
// words, gpt-4o-transcribe-diarize segments with speakers, Mistral's segment-only answer, Groq), Speechmatics' job API and
// Gladia's upload / job API, both deleting the job once read. Request shape, auth header, error mapping, cancel.

#include "NetTestSupport.h"

#include <wabcut/ai/Transport.h>
#include <wabcut/ai_net/Adapters.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace wabcut::ai;
using namespace wabcut::ai::test;
using wabcut::Ticks;

namespace {

Ticks ms(std::int64_t milliseconds) { return Ticks{milliseconds * (wabcut::kTicksPerSecond / 1000)}; }

struct AudioFile {
    std::filesystem::path path = uniqueTempPath("wabcut-ai-stt-audio", ".flac");
    AudioFile() { std::ofstream(path, std::ios::binary) << "fLaC-test-audio"; }
    ~AudioFile() { std::filesystem::remove(path); }
};

TranscribeRequest requestFor(const AudioFile& audio) {
    TranscribeRequest r;
    r.audioFile = audio.path;
    r.mediaType = "audio/flac";
    r.audioMilliseconds = 4000;
    r.source.path = "interview.mov";
    r.origin = Ticks::fromSeconds(10);
    return r;
}

std::string fieldValue(const HttpRequest& request, const std::string& name) {
    for (const auto& f : request.multipart) {
        if (f.name == name) {
            return f.value;
        }
    }
    return "<missing>";
}

std::size_t fieldCount(const HttpRequest& request, const std::string& name) {
    return static_cast<std::size_t>(std::count_if(request.multipart.begin(), request.multipart.end(),
                                                  [&](const MultipartField& f) { return f.name == name; }));
}

net::AdapterOptions noWait() {
    net::AdapterOptions options;
    options.sleeper = [](std::chrono::milliseconds, std::stop_token stop) { return !stop.stop_requested(); };
    options.pollInterval = std::chrono::milliseconds(0);
    return options;
}

} // namespace

TEST_CASE("OpenAI transcriptions: whisper-1 words in source time / verbose_json / language name", "[ai][net][transcribe][r07]") {
    AudioFile audio;
    FakeTransport fake;
    fake.pushJson(200, R"({"task":"transcribe","language":"english","duration":4.0,"text":"Welcome back everyone",
        "words":[{"word":"Welcome","start":0.1,"end":0.5},{"word":"back","start":0.5,"end":0.9},{"word":"everyone","start":1.0,"end":1.6}],
        "segments":[{"id":0,"start":0.1,"end":1.6,"text":"Welcome back everyone"}],"usage":{"type":"duration","seconds":4}})",
                  {{"x-request-id", "req_fixture"}});
    auto stt = net::makeOpenAiTranscriber(context("whisper-1", fake));
    float last = 0.0f;
    auto r = stt->transcribe(requestFor(audio), [&](float p) { last = p; }, {});
    REQUIRE(r);
    const HttpRequest sent = fake.requests().at(0);
    CHECK(sent.url == "https://api.openai.com/v1/audio/transcriptions");
    CHECK(findHeader(sent.headers, "Authorization") == "Bearer test-key-0123456789");
    CHECK(fieldValue(sent, "model") == "whisper-1");
    CHECK(fieldValue(sent, "response_format") == "verbose_json");
    CHECK(fieldCount(sent, "timestamp_granularities[]") == 2);
    CHECK(fieldValue(sent, "diarize") == "<missing>");
    CHECK_FALSE(sent.retrySafe);
    const Transcript& t = r->transcript;
    REQUIRE(t.validate());
    CHECK(t.language == "en");
    REQUIRE(t.words.size() == 3);
    CHECK(t.words[0].text == "Welcome");
    CHECK(t.words[0].t0 == Ticks::fromSeconds(10) + ms(100));
    CHECK(t.words[2].t1 == Ticks::fromSeconds(10) + ms(1600));
    CHECK(t.provenance.modelId == "whisper-1");
    CHECK(r->usage.audioMilliseconds == 4000);
    CHECK(r->requestId == "req_fixture");
    CHECK(last == 1.0f);
}

TEST_CASE("OpenAI transcriptions: the diarize model's segments give speakers and spread words", "[ai][net][transcribe][r07]") {
    AudioFile audio;
    FakeTransport fake;
    fake.pushJson(200, R"({"task":"transcribe","duration":3.0,"text":"Hi there. Hello.",
        "segments":[{"type":"transcript.text.segment","id":"s0","start":0.0,"end":1.0,"text":"Hi there.","speaker":"A"},
                    {"type":"transcript.text.segment","id":"s1","start":1.5,"end":2.0,"text":"Hello.","speaker":"B"}]})");
    auto stt = net::makeOpenAiTranscriber(context("gpt-4o-transcribe-diarize", fake));
    TranscribeRequest request = requestFor(audio);
    request.language = "en";
    auto r = stt->transcribe(request, {}, {});
    REQUIRE(r);
    const HttpRequest sent = fake.requests().at(0);
    CHECK(fieldValue(sent, "response_format") == "diarized_json");
    CHECK(fieldValue(sent, "chunking_strategy") == "auto");
    CHECK(fieldValue(sent, "language") == "en");
    CHECK(fieldCount(sent, "timestamp_granularities[]") == 0);
    const Transcript& t = r->transcript;
    REQUIRE(t.validate());
    REQUIRE(t.words.size() == 3);
    CHECK(t.words[0].text == "Hi");
    CHECK(t.words[0].t0 == Ticks::fromSeconds(10));
    CHECK(t.words[1].t1 == Ticks::fromSeconds(10) + ms(1000));
    CHECK(t.words[0].speakerId == 0);
    CHECK(t.words[2].speakerId == 1);
    REQUIRE(t.segments.size() == 2);
    CHECK(r->usage.audioMilliseconds == 3000);
}

TEST_CASE("OpenAI transcriptions on Mistral and Groq: segment-only Voxtral with diarize / Groq words", "[ai][net][transcribe][r07]") {
    AudioFile audio;
    FakeTransport fake;
    fake.pushJson(200, R"({"model":"voxtral-mini-transcribe-3","text":"Bonjour à tous","language":null,
        "segments":[{"type":"transcription_segment","text":"Bonjour à tous","start":0.2,"end":1.4,"speaker_id":"speaker_1"}],
        "usage":{"prompt_audio_seconds":4}})");
    auto mistral = net::makeOpenAiTranscriber(context("voxtral-mini-transcribe-3", fake));
    TranscribeRequest request = requestFor(audio);
    request.language = "fr";
    auto r = mistral->transcribe(request, {}, {});
    REQUIRE(r);
    const HttpRequest sent = fake.requests().at(0);
    CHECK(sent.url == "https://api.mistral.ai/v1/audio/transcriptions");
    CHECK(fieldValue(sent, "response_format") == "<missing>"); // Mistral answers JSON with segments on its own
    CHECK(fieldCount(sent, "timestamp_granularities[]") == 1);
    CHECK(fieldValue(sent, "timestamp_granularities[]") == "segment");
    CHECK(fieldValue(sent, "diarize") == "true");
    REQUIRE(r->transcript.words.size() == 3);
    CHECK(r->transcript.language == "fr");
    CHECK(r->transcript.words[0].t0 == Ticks::fromSeconds(10) + ms(200));
    CHECK(r->usage.audioMilliseconds == 4000); // the audio Mistral says it billed (usage.prompt_audio_seconds)

    fake.pushJson(200, R"({"text":" Hello","language":"en","duration":2.5,"words":[{"word":"Hello","start":0.0,"end":0.4}],
        "segments":[{"start":0.0,"end":0.4,"text":" Hello"}],"x_groq":{"id":"req_01"}})");
    auto groq = net::makeOpenAiTranscriber(context("whisper-large-v3-turbo", fake));
    request.language.clear();
    auto g = groq->transcribe(request, {}, {});
    REQUIRE(g);
    CHECK(fake.requests().at(1).url == "https://api.groq.com/openai/v1/audio/transcriptions");
    CHECK(fieldValue(fake.requests().at(1), "response_format") == "verbose_json");
    CHECK(g->usage.audioMilliseconds == 2500);

    // Errors: a bad key, a file over the model's limit (nothing sent), an answer without any timing.
    fake.pushJson(401, R"({"error":{"message":"Invalid API Key","type":"invalid_request_error","code":"invalid_api_key"}})");
    auto denied = groq->transcribe(request, {}, {});
    REQUIRE_FALSE(denied);
    CHECK(denied.error().code == AiErrorCode::Unauthorized);
    fake.pushJson(200, R"({"text":"no timing here"})");
    request.audioMilliseconds = 0;
    auto untimed = groq->transcribe(request, {}, {});
    REQUIRE_FALSE(untimed);
    CHECK(untimed.error().code == AiErrorCode::ProviderError);
}

TEST_CASE("Speechmatics: multipart job / poll / json-v2 words with punctuation and speakers / job deleted", "[ai][net][transcribe][r07]") {
    AudioFile audio;
    FakeTransport fake;
    fake.pushJson(201, R"({"id":"job123abc"})");
    fake.pushJson(200, R"({"job":{"id":"job123abc","status":"running"}})");
    fake.pushJson(200, R"({"job":{"id":"job123abc","status":"done","duration":4}})");
    fake.pushJson(200, R"({"format":"2.9","metadata":{"transcription_config":{"language":"en","diarization":"speaker"}},"results":[
        {"type":"word","start_time":0.1,"end_time":0.5,"alternatives":[{"content":"Hello","confidence":0.98,"language":"en","speaker":"S1"}]},
        {"type":"punctuation","start_time":0.5,"end_time":0.5,"attaches_to":"previous","alternatives":[{"content":",","confidence":1,"speaker":"S1"}]},
        {"type":"word","start_time":0.6,"end_time":1.0,"alternatives":[{"content":"world","confidence":0.9,"language":"en","speaker":"S2"}]},
        {"type":"punctuation","start_time":1.0,"end_time":1.0,"attaches_to":"previous","is_eos":true,"alternatives":[{"content":".","speaker":"S2"}]}]})");
    fake.pushJson(200, R"({"job":{"id":"job123abc","status":"deleted"}})");
    auto stt = net::makeSpeechmaticsTranscriber(context("speechmatics:enhanced", fake), noWait());
    auto r = stt->transcribe(requestFor(audio), {}, {});
    REQUIRE(r);
    const auto requests = fake.requests();
    REQUIRE(requests.size() == 5);
    CHECK(requests[0].url == "https://eu1.asr.api.speechmatics.com/v2/jobs");
    CHECK(findHeader(requests[0].headers, "Authorization") == "Bearer test-key-0123456789");
    const Json config = Json::parse(fieldValue(requests[0], "config"));
    CHECK(config["type"] == "transcription");
    CHECK(config["transcription_config"]["language"] == "auto");
    CHECK(config["transcription_config"]["diarization"] == "speaker");
    CHECK(requests[0].multipart.back().name == "data_file");
    CHECK(requests[3].url == "https://eu1.asr.api.speechmatics.com/v2/jobs/job123abc/transcript?format=json-v2");
    CHECK(requests[4].method == "DELETE");
    const Transcript& t = r->transcript;
    REQUIRE(t.validate());
    REQUIRE(t.words.size() == 2);
    CHECK(t.words[0].text == "Hello,");
    CHECK(t.words[1].text == "world.");
    CHECK(t.words[0].speakerId == 0);
    CHECK(t.words[1].speakerId == 1);
    CHECK(t.language == "en");
    CHECK(r->usage.audioMilliseconds == 4000);
    CHECK(r->requestId == "job123abc");
}

TEST_CASE("Speechmatics: a rejected job is an error; a cancel force-deletes the job", "[ai][net][transcribe][r07]") {
    AudioFile audio;
    FakeTransport fake;
    fake.pushJson(201, R"({"id":"job9"})");
    fake.pushJson(200, R"({"job":{"id":"job9","status":"rejected","errors":[{"timestamp":"t","message":"unsupported file"}]}})");
    auto stt = net::makeSpeechmaticsTranscriber(context("speechmatics:standard", fake), noWait());
    auto rejected = stt->transcribe(requestFor(audio), {}, {});
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().message.find("unsupported file") != std::string::npos);

    std::stop_source stop;
    fake.pushJson(201, R"({"id":"job10"})");
    fake.pushJson(200, R"({"job":{"status":"deleted"}})");
    net::AdapterOptions options = noWait();
    options.sleeper = [&](std::chrono::milliseconds, std::stop_token) {
        stop.request_stop();
        return false;
    };
    auto cancelling = net::makeSpeechmaticsTranscriber(context("speechmatics:standard", fake), options);
    auto cancelled = cancelling->transcribe(requestFor(audio), {}, stop.get_token());
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == AiErrorCode::Cancelled);
    const auto requests = fake.requests();
    CHECK(requests.back().method == "DELETE");
    CHECK(requests.back().url == "https://eu1.asr.api.speechmatics.com/v2/jobs/job10?force=true");
}

TEST_CASE("Gladia: upload / job / poll / utterance words and speakers / header key / job deleted", "[ai][net][transcribe][r07]") {
    AudioFile audio;
    FakeTransport fake;
    fake.pushJson(200, R"({"audio_url":"https://api.gladia.io/file/abc","audio_metadata":{"audio_duration":4}})");
    fake.pushJson(201, R"({"id":"45463597-20b7-4af7-b3b3-f5fb778203ab","result_url":"https://api.gladia.io/v2/pre-recorded/45463597-20b7-4af7-b3b3-f5fb778203ab"})");
    fake.pushJson(200, R"({"status":"processing"})");
    fake.pushJson(200, R"({"status":"done","result":{"metadata":{"audio_duration":4.0,"billing_time":4.0},
        "transcription":{"languages":["en"],"utterances":[
            {"start":0.2,"end":0.9,"speaker":0,"words":[{"word":" Hi","start":0.2,"end":0.4,"confidence":0.9},{"word":" there","start":0.4,"end":0.9,"confidence":0.8}]},
            {"start":1.2,"end":1.6,"speaker":1,"words":[{"word":" Yes","start":1.2,"end":1.6,"confidence":0.95}]}]}}})");
    fake.pushJson(202, "{}");
    auto stt = net::makeGladiaTranscriber(context("gladia:solaria-1", fake), noWait());
    TranscribeRequest request = requestFor(audio);
    request.speakerCount = 2;
    auto r = stt->transcribe(request, {}, {});
    REQUIRE(r);
    const auto requests = fake.requests();
    REQUIRE(requests.size() == 5);
    CHECK(requests[0].url == "https://api.gladia.io/v2/upload");
    CHECK(findHeader(requests[0].headers, "x-gladia-key") == "test-key-0123456789");
    CHECK_FALSE(findHeader(requests[0].headers, "Authorization"));
    CHECK(requests[0].multipart.at(0).name == "audio");
    const Json body = Json::parse(requests[1].body);
    CHECK(requests[1].url == "https://api.gladia.io/v2/pre-recorded");
    CHECK(body["audio_url"] == "https://api.gladia.io/file/abc");
    CHECK(body["diarization"] == true);
    CHECK(body["diarization_config"]["number_of_speakers"] == 2);
    CHECK(body["language_config"]["languages"].empty());
    CHECK(requests[2].url == "https://api.gladia.io/v2/pre-recorded/45463597-20b7-4af7-b3b3-f5fb778203ab");
    CHECK(requests[4].method == "DELETE");
    const Transcript& t = r->transcript;
    REQUIRE(t.validate());
    REQUIRE(t.words.size() == 3);
    CHECK(t.words[0].text == "Hi");
    CHECK(t.words[2].speakerId == 1);
    CHECK(t.language == "en");
    REQUIRE(t.segments.size() == 2);
    CHECK(r->usage.audioMilliseconds == 4000);

    // A failed job is an error and is deleted; an id that is not one never reaches a URL.
    fake.pushJson(200, R"({"audio_url":"https://api.gladia.io/file/x"})");
    fake.pushJson(201, R"({"id":"job-2"})");
    fake.pushJson(200, R"({"status":"error","error_code":500})");
    fake.pushJson(202, "{}");
    auto failed = stt->transcribe(request, {}, {});
    REQUIRE_FALSE(failed);
    CHECK(fake.requests().back().method == "DELETE");
    fake.pushJson(200, R"({"audio_url":"https://api.gladia.io/file/x"})");
    fake.pushJson(201, R"({"id":"../../v2/upload"})");
    auto evil = stt->transcribe(request, {}, {});
    REQUIRE_FALSE(evil);
    CHECK(evil.error().code == AiErrorCode::ProviderError);
}

TEST_CASE("The new transcription adapters are registered and usable through the platform", "[ai][net][transcribe][platform][r07]") {
    ModelRegistry models = ModelRegistry::builtin();
    ProviderRegistry providers{models};
    net::registerCloudAdapters(providers, noWait());
    for (const char* api : {"openai-transcriptions", "speechmatics", "gladia"}) {
        INFO(api);
        CHECK(providers.hasAdapter(api, Capability::Transcribe));
    }
    MemorySecretStore secrets;
    KeyRing keys{secrets};
    ConsentLedger consent;
    SpendGuard spend;
    RequestLog log;
    FakeTransport fake;
    AiPlatform ai{providers, keys, consent, spend, log, fake};
    (void)keys.save(*models.provider("speechmatics"), Secret("fixture-only-speechmatics-key"));
    // The registry id is "speechmatics:enhanced"; the API gets "enhanced" (ModelSpec::apiId).
    fake.pushJson(201, R"({"id":"jobA"})");
    fake.pushJson(200, R"({"job":{"status":"done","duration":60}})");
    fake.pushJson(200, R"({"results":[{"type":"word","start_time":0.1,"end_time":0.4,"alternatives":[{"content":"Hi","speaker":"UU"}]}]})");
    fake.pushJson(200, R"({"job":{"status":"deleted"}})");
    AudioFile audio;
    auto stt = ai.transcriber("speechmatics:enhanced", CallOptions{true, MicroUsd{1'000'000}});
    REQUIRE(stt);
    TranscribeRequest request = requestFor(audio);
    request.audioMilliseconds = 60000;
    auto r = (*stt)->transcribe(request, {}, {});
    REQUIRE(r);
    CHECK(Json::parse(fake.requests().at(0).multipart.at(0).value)["transcription_config"]["model"] == "enhanced");
    CHECK_FALSE(r->transcript.words.at(0).speakerId.has_value()); // "UU" = unknown
    // One minute at $0.75 per hour.
    CHECK(log.entries().back().cost == 12500);
}
