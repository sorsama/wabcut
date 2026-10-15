// TranscribeService (routing to local / cloud / added providers and "auto", the analysis cache, the cloud guards,
// chunking at silences, cancel) and AudioEnergyService (per-range RMS / peak / speech, memory and disk caches) on
// generated WAV files (full builds: they decode media). The local engine is the scripted fake helper
// (fake_infer.cpp); the cloud model is a fake ITranscriber behind a real AiPlatform. Nothing leaves the machine.

#include "AiTestUtil.h"
#include "FakeTransport.h"

#include <wabcut/ai/AudioEnergy.h>
#include <wabcut/ai/ConsentLedger.h>
#include <wabcut/ai/Provider.h>
#include <wabcut/ai/RequestLog.h>
#include <wabcut/ai/Secrets.h>
#include <wabcut/ai/Sha256.h>
#include <wabcut/ai/SpendGuard.h>
#include <wabcut/ai/TranscribeService.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <numbers>
#include <stdexcept>
#include <thread>
#include <tuple>

using namespace wabcut;
using namespace wabcut::ai;
using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;

namespace {

constexpr int kRate = 16000;

Ticks sec(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

/// `seconds` of 16 kHz audio: a 400 Hz sine of `amplitude` in each [from, to) of `tones`, digital silence elsewhere.
/// 400 Hz at 16 kHz is 40 samples a period, so a whole second has RMS amplitude / sqrt(2) and peak amplitude exactly.
std::vector<float> toneTrack(double seconds, std::vector<std::pair<double, double>> tones, float amplitude = 0.5f) {
    std::vector<float> pcm(static_cast<std::size_t>(seconds * kRate), 0.0f);
    for (const auto& [a, b] : tones) {
        for (auto i = static_cast<std::size_t>(a * kRate); i < static_cast<std::size_t>(b * kRate) && i < pcm.size(); ++i) {
            pcm[i] = amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * 400.0 * static_cast<double>(i) / kRate));
        }
    }
    return pcm;
}

/// Mono 16 kHz float32 WAV: decoded unchanged (no resampling, no downmix), so levels are exact.
std::filesystem::path writeWav(const std::filesystem::path& file, const std::vector<float>& samples) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    const auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const auto bytes = static_cast<std::uint32_t>(samples.size() * sizeof(float));
    out.write("RIFF", 4);
    u32(36 + bytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(3); // IEEE float
    u16(1);
    u32(kRate);
    u32(kRate * 4);
    u16(4);
    u16(32);
    out.write("data", 4);
    u32(bytes);
    out.write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(bytes));
    return file;
}

std::vector<std::string> texts(const Transcript& t) {
    std::vector<std::string> out;
    for (const TranscriptWord& w : t.words) {
        out.push_back(w.text);
    }
    return out;
}

/// A model "installed" in `dir`/models: a small file plus the verification marker (as test_transcription_service).
ModelInfo installed(const std::filesystem::path& dir, const std::string& id, ModelKind kind) {
    const std::string body = "fake weights for " + id;
    ModelInfo m{id, kind, id + ".bin", "https://example.invalid/" + id, body.size(), Sha256::hex(body), "MIT", "test", ""};
    std::filesystem::create_directories(dir / "models");
    std::ofstream(dir / "models" / m.fileName, std::ios::binary) << body;
    std::ofstream(dir / "models" / (m.fileName + ".verified"), std::ios::binary) << m.sha256 << '\n';
    return m;
}

/// The local engine: the fake helper with fake whisper and VAD models (or a helper that does not exist).
TranscriptionServiceConfig localEngine(const std::filesystem::path& dir, bool helper = true) {
    TranscriptionServiceConfig local;
    local.inferExecutable = helper ? std::filesystem::path(WABCUT_FAKE_INFER) : dir / "not-built" / "wabcut-infer.exe";
    local.modelRoot = dir / "models";
    local.helperLog = dir / "helper.log";
    local.models = {installed(dir, "fake-whisper", ModelKind::Whisper), installed(dir, "fake-vad", ModelKind::Vad)};
    return local;
}

TranscribeJobOptions localOptions(std::string provider = "local") {
    TranscribeJobOptions o;
    o.provider = std::move(provider);
    o.local.model = "fake-whisper";
    o.local.vadModel = "fake-vad";
    o.local.beamSize = 1;
    return o;
}

// --- A cloud transcription model behind a real AiPlatform ------------------------------------------------------

struct CloudCalls {
    std::mutex mutex;
    std::vector<TranscribeRequest> requests;
    std::vector<std::int64_t> uploadedSamples; ///< from each uploaded WAV's size
    std::vector<std::vector<std::int16_t>> uploads; ///< each uploaded WAV's samples (review P1-3: what left the machine)
    std::atomic<bool> waitForStop{false};
    std::atomic<bool> started{false};
};

/// Answers every request with one word "c<n>" (n = request number) 0.1 s into the audio, in source time.
class FakeCloudTranscriber final : public ITranscriber {
public:
    FakeCloudTranscriber(ModelSpec spec, CloudCalls* calls) : m_spec(std::move(spec)), m_calls(calls) {}
    const ModelSpec& spec() const override { return m_spec; }
    std::expected<TranscribeResult, AiError> transcribe(const TranscribeRequest& request, const ProgressFn& progress,
                                                        std::stop_token stop) override {
        std::size_t n = 0;
        {
            std::lock_guard lock(m_calls->mutex);
            m_calls->requests.push_back(request);
            m_calls->uploadedSamples.push_back(static_cast<std::int64_t>(std::filesystem::file_size(request.audioFile) - 44) / 2);
            std::vector<std::int16_t> samples(static_cast<std::size_t>(m_calls->uploadedSamples.back()));
            std::ifstream wav(request.audioFile, std::ios::binary);
            wav.seekg(44);
            wav.read(reinterpret_cast<char*>(samples.data()), static_cast<std::streamsize>(samples.size() * 2));
            m_calls->uploads.push_back(std::move(samples));
            n = m_calls->requests.size();
        }
        m_calls->started = true;
        while (m_calls->waitForStop && !stop.stop_requested()) {
            std::this_thread::sleep_for(5ms);
        }
        if (stop.stop_requested()) {
            return std::unexpected(AiError{AiErrorCode::Cancelled, "cancelled"});
        }
        if (progress) {
            progress(1.0f);
        }
        TranscribeResult r;
        TranscriptWord w;
        w.text = "c" + std::to_string(n);
        w.t0 = request.origin + sec(0.1);
        w.t1 = request.origin + sec(0.4);
        w.confidence = 0.9f;
        r.transcript.language = request.language.empty() ? "th" : request.language;
        r.transcript.source = request.source;
        r.transcript.provenance.engine = "fake-cloud";
        r.transcript.provenance.modelId = m_spec.id;
        r.transcript.words.push_back(w);
        r.transcript.segments.push_back(TranscriptSegment{w.t0, w.t1, 0, 1, w.text, std::nullopt});
        r.usage.audioMilliseconds = request.audioMilliseconds;
        return r;
    }

private:
    ModelSpec m_spec;
    CloudCalls* m_calls;
};

ModelRegistry cloudRegistry(std::int64_t maxAudioSeconds) {
    auto r = ModelRegistry::fromJson(std::string(R"({"schemaVersion":1,"providers":[
        {"id":"stt","name":"STT Cloud","api":"fake-stt","baseUrl":"https://stt.test","auth":"bearer",
         "envVar":"WABCUT_STT_TEST_KEY","termsUrl":"https://stt.test/terms","dataPolicy":"deleted after 30 days"}],
      "models":[{"id":"stt-model","provider":"stt","name":"STT Model","capabilities":{"transcribe":true},
                 "maxAudioSeconds":)") + std::to_string(maxAudioSeconds) +
                                     R"(,"prices":[{"perAudioMinute":0.01}]}]})");
    REQUIRE(r);
    return std::move(*r);
}

struct Cloud {
    explicit Cloud(std::int64_t maxAudioSeconds = 0) : registry(cloudRegistry(maxAudioSeconds)) {
        providers.registerTranscriber("fake-stt", [this](ProviderContext ctx) {
            return std::make_unique<FakeCloudTranscriber>(ctx.model, &calls);
        });
        REQUIRE(keys.save(*registry.provider("stt"), Secret("stt-key-0123456789")));
        providers.setPreference(Capability::Transcribe, {"stt-model"});
    }

    ModelRegistry registry;
    ProviderRegistry providers{registry};
    MemorySecretStore store;
    KeyRing keys{store, [](std::string_view) { return std::optional<std::string>(); }};
    ConsentLedger consent;
    SpendGuard spend;
    RequestLog log;
    test::FakeTransport transport;
    AiPlatform platform{providers, keys, consent, spend, log, transport};
    CloudCalls calls;
};

TranscribeServiceConfig serviceConfig(const std::filesystem::path& dir, bool helper, AiPlatform* platform = nullptr) {
    TranscribeServiceConfig config;
    config.cacheRoot = dir / "cache";
    config.workDir = dir / "work";
    config.local = localEngine(dir, helper);
    config.platform = platform;
    return config;
}

} // namespace

TEST_CASE("TranscribeService: local transcription through the helper, then the analysis cache", "[ai][transcribe]") {
    const auto dir = test::freshDir("transcribe-local");
    const TranscribeSource source{writeWav(dir / "talk.wav", toneTrack(6.0, {{1.0, 3.0}, {4.0, 5.0}})).string(), -1};
    TranscribeService service(serviceConfig(dir, true));
    CHECK(service.resolveProvider("auto") == "local");
    CHECK(service.cached(source) == nullptr);

    auto first = service.run(source, localOptions());
    REQUIRE(first);
    CHECK(first->provider == "local");
    CHECK_FALSE(first->fromCache);
    // The fake helper names words after the second they start in: speech at [1, 3) and [4, 5).
    CHECK(texts(first->transcript) == std::vector<std::string>{"w1", "w2", "w4"});
    CHECK(first->transcript.words[0].t0 == sec(1.0));
    CHECK(first->transcript.source.fingerprint != 0);
    CHECK(first->device == "FakeCPU");
    CHECK(first->note.find("Vulkan") != std::string::npos); // no GPU: the CPU fallback is reported

    // Any provider, any settings: the newest transcript of the audio.
    auto again = service.run(source, localOptions("auto"));
    REQUIRE(again);
    CHECK(again->fromCache);
    CHECK(again->provider == "local");
    CHECK(texts(again->transcript) == texts(first->transcript));
    REQUIRE(service.cached(source) != nullptr);

    auto cpuOptions = localOptions();
    cpuOptions.local.useGpu = false;
    cpuOptions.useCache = false;
    const auto requestedCpu = service.run(source, cpuOptions);
    REQUIRE(requestedCpu);
    CHECK(requestedCpu->device == "FakeCPU");
    CHECK(requestedCpu->note.empty()); // an explicit CPU request is not a Vulkan fallback

    std::stop_source cancelled;
    cancelled.request_stop();
    const auto cancelledHit = service.run(source, localOptions(), cancelled.get_token());
    REQUIRE_FALSE(cancelledHit);
    CHECK(cancelledHit.error().code == ErrorCode::Cancelled);

    // A build without the helper still serves what is cached, and says how to get a provider otherwise.
    TranscribeService bare(serviceConfig(dir, false));
    auto cachedOnly = bare.run(source, localOptions("auto"));
    REQUIRE(cachedOnly);
    CHECK(cachedOnly->fromCache);
    TranscribeJobOptions fresh = localOptions("auto");
    fresh.useCache = false;
    const auto none = bare.run(source, fresh);
    REQUIRE_FALSE(none);
    CHECK(none.error().code == ErrorCode::NotFound);
    CHECK(none.error().message.find("WABCUT_WITH_LOCAL_AI") != std::string::npos);
    CHECK(bare.resolveProvider("local").error().code == ErrorCode::NotFound);
    CHECK(bare.resolveProvider("nope").error().code == ErrorCode::NotFound);
    CHECK(bare.run(TranscribeSource{(dir / "missing.wav").string(), -1}, localOptions()).error().code == ErrorCode::NotFound);
}

TEST_CASE("Cloud transcription goes through the platform's consent, locks and spend cap", "[ai][transcribe][cloud]") {
    const auto dir = test::freshDir("transcribe-cloud");
    const TranscribeSource source{writeWav(dir / "talk.wav", toneTrack(6.0, {{1.0, 3.0}})).string(), -1};
    Cloud cloud;
    TranscribeService service(serviceConfig(dir, false, &cloud.platform));
    CHECK(service.resolveProvider("auto") == "cloud:stt-model"); // no local engine: the preferred cloud model

    const auto providers = service.providers();
    REQUIRE(providers.size() == 2);
    CHECK(providers[0].id == "local");
    CHECK_FALSE(providers[0].available);
    CHECK(providers[1].id == "cloud:stt-model");
    CHECK(providers[1].available);

    TranscribeJobOptions options;
    options.provider = "cloud:stt-model";
    const auto refused = service.run(source, options);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == ErrorCode::Unsupported);
    CHECK(refused.error().message.find("ConsentRequired") != std::string::npos);

    options.allowOnce = true; // never overrides the offline mode or the project's no-cloud lock
    cloud.consent.setOfflineMode(true);
    CHECK(service.run(source, options).error().message.find("Offline") != std::string::npos);
    CHECK_FALSE(service.providers()[1].available);
    cloud.consent.setOfflineMode(false);
    cloud.consent.setProjectLock(true);
    CHECK(service.run(source, options).error().code == ErrorCode::Unsupported);
    cloud.consent.setProjectLock(false);
    cloud.spend.setMonthlyCap("stt", MicroUsd{0});
    CHECK(service.run(source, options).error().message.find("SpendCapExceeded") != std::string::npos);
    cloud.spend.setMonthlyCap("stt", std::nullopt);
    CHECK(cloud.calls.requests.empty()); // nothing was uploaded

    auto done = service.run(source, options);
    REQUIRE(done);
    CHECK(done->provider == "cloud:stt-model");
    CHECK(texts(done->transcript) == std::vector<std::string>{"c1"});
    REQUIRE(cloud.calls.requests.size() == 1);
    const TranscribeRequest& sent = cloud.calls.requests[0];
    CHECK(sent.mediaType == "audio/wav");
    CHECK(sent.language.empty()); // "auto": the provider detects it
    CHECK(sent.audioMilliseconds == 6000);
    CHECK(sent.origin == Ticks{});
    CHECK(cloud.calls.uploadedSamples[0] == 6 * kRate); // 16 kHz mono 16-bit, the whole file in one request
    CHECK(cloud.log.entries().size() >= 1);              // the platform logged the call

    // Stored under the source fingerprint, where the transcript commands (and the next run) find it.
    auto again = service.run(source, options);
    REQUIRE(again);
    CHECK(again->fromCache);
    CHECK(again->provider == "cloud:stt-model");
    CHECK(cloud.calls.requests.size() == 1);
    const auto entry = AnalysisCache(dir / "cache").newest(done->transcript.source.fingerprint,
                                                           done->transcript.source.streamIndex, "transcript");
    REQUIRE(entry);
    CHECK(entry->key.modelVersion == "cloud/stt-model");
}

TEST_CASE("Long audio goes up in chunks cut at silences and comes back in source time", "[ai][transcribe][cloud]") {
    const auto dir = test::freshDir("transcribe-chunks");
    const TranscribeSource source{
        writeWav(dir / "long.wav", toneTrack(10.0, {{0.5, 2.5}, {3.5, 5.5}, {6.5, 8.5}})).string(), -1};

    SECTION("the service's chunk length") {
        Cloud cloud;
        TranscribeServiceConfig config = serviceConfig(dir, false, &cloud.platform);
        config.cloudChunk = Ticks::fromSeconds(3);
        TranscribeService service(std::move(config));
        TranscribeJobOptions options;
        options.provider = "cloud:stt-model";
        options.allowOnce = true;
        auto done = service.run(source, options);
        REQUIRE(done);
        REQUIRE(cloud.calls.requests.size() == 3); // one per speech run: 2 s each, under the 3 s limit
        for (std::size_t k = 0; k < 3; ++k) {
            INFO(k);
            CHECK(cloud.calls.uploadedSamples[k] <= 3 * kRate);
            CHECK(cloud.calls.requests[k].audioMilliseconds == cloud.calls.uploadedSamples[k] * 1000 / kRate);
            if (k > 0) {
                CHECK(cloud.calls.requests[k].origin > cloud.calls.requests[k - 1].origin);
            }
            // Each chunk starts at (or just before) its speech and its word comes back in source time.
            CHECK(cloud.calls.requests[k].origin <= sec(0.5 + 3.0 * static_cast<double>(k)));
            CHECK(done->transcript.words[k].t0 == cloud.calls.requests[k].origin + sec(0.1));
        }
        CHECK(texts(done->transcript) == std::vector<std::string>{"c1", "c2", "c3"});
        CHECK(done->transcript.segments.size() == 3);
        CHECK(done->transcript.segments[2].firstWord == 2);
        CHECK(done->transcript.validate());
        CHECK(done->transcript.provenance.params.at("chunks") == "3");
    }
    SECTION("the model's documented limit") {
        Cloud cloud(2); // maxAudioSeconds 2: every speech run is cut in two
        TranscribeService service(serviceConfig(dir, false, &cloud.platform));
        TranscribeJobOptions options;
        options.provider = "cloud:stt-model";
        options.allowOnce = true;
        options.useCache = false;
        auto done = service.run(source, options);
        REQUIRE(done);
        CHECK(cloud.calls.requests.size() >= 3);
        for (const std::int64_t samples : cloud.calls.uploadedSamples) {
            CHECK(samples <= 2 * kRate);
        }
        CHECK(done->transcript.validate());
    }
}

TEST_CASE("Added providers, the Job API and cancel", "[ai][transcribe][cancel]") {
    const auto dir = test::freshDir("transcribe-jobs");
    const TranscribeSource source{writeWav(dir / "talk.wav", toneTrack(2.0, {{0.5, 1.5}})).string(), -1};
    TranscribeService service(serviceConfig(dir, false));
    std::atomic<int> runs{0};
    service.addProvider("echo", "Echo", [&](const TranscribeSource&, const TranscribeJobOptions& o,
                                            const std::function<void(float)>& progress, std::stop_token)
                                            -> std::expected<Transcript, Error> {
        ++runs;
        progress(0.5f);
        Transcript t;
        t.language = o.language == "auto" ? "en" : o.language;
        TranscriptWord w;
        w.text = "echo";
        w.t0 = sec(0.5);
        w.t1 = sec(1.0);
        t.words.push_back(w);
        t.segments.push_back(TranscriptSegment{w.t0, w.t1, 0, 1, "echo", std::nullopt});
        return t;
    });
    std::atomic<bool> slowStarted{false};
    service.addProvider("slow", "Slow", [&](const TranscribeSource&, const TranscribeJobOptions&,
                                            const std::function<void(float)>&, std::stop_token stop)
                                            -> std::expected<Transcript, Error> {
        slowStarted = true;
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(5ms);
        }
        return std::unexpected(Error{ErrorCode::Cancelled, "stopped"});
    });
    CHECK(service.resolveProvider("echo") == "echo");
    CHECK(service.providers().back().id == "slow");

    TranscribeJobOptions echo;
    echo.provider = "echo";
    echo.language = "th";
    auto job = service.transcribe(source, echo);
    const auto& result = job.wait();
    REQUIRE(result);
    CHECK(result->provider == "echo");
    CHECK(result->transcript.language == "th");
    CHECK(result->transcript.source.fingerprint != 0); // the service stamps the source
    CHECK(job.progress() == 1.0f);
    // Cached with its provider, found by "auto" (no local engine, no cloud) without running anything.
    auto cached = service.run(source, TranscribeJobOptions{});
    REQUIRE(cached);
    CHECK(cached->fromCache);
    CHECK(cached->provider == "echo");
    CHECK(runs == 1);

    TranscribeJobOptions slow;
    slow.provider = "slow";
    slow.useCache = false;
    auto stuck = service.transcribe(source, slow);
    for (int i = 0; i < 400 && !slowStarted; ++i) {
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(slowStarted);
    stuck.cancel();
    REQUIRE(stuck.waitFor(5s));
    CHECK(stuck.wait().error().code == ErrorCode::Cancelled);

    // A cloud upload is stopped too.
    Cloud cloud;
    cloud.calls.waitForStop = true;
    TranscribeService cloudService(serviceConfig(dir, false, &cloud.platform));
    TranscribeJobOptions up;
    up.provider = "cloud:stt-model";
    up.allowOnce = true;
    up.useCache = false;
    auto upload = cloudService.transcribe(source, up);
    for (int i = 0; i < 400 && !cloud.calls.started; ++i) {
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(cloud.calls.started);
    upload.cancel();
    REQUIRE(upload.waitFor(5s));
    CHECK(upload.wait().error().code == ErrorCode::Cancelled);
}

TEST_CASE("The upload WAV, the level-based speech finder and the CPU note", "[ai][transcribe]") {
    const auto dir = test::freshDir("transcribe-units");
    const std::vector<float> pcm = toneTrack(6.0, {{1.0, 3.0}, {4.0, 5.0}});
    const auto raw = dir / "audio.f32";
    {
        std::ofstream out(raw, std::ios::binary);
        out.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(pcm.size() * sizeof(float)));
    }
    const auto wav = dir / "part.wav";
    REQUIRE(writeWav16k(raw, kRate, 2 * kRate, wav) == 2 * kRate);
    CHECK(std::filesystem::file_size(wav) == 44u + 2u * 2u * kRate);
    // Past the end: what is there.
    CHECK(writeWav16k(raw, 5 * kRate, 4 * kRate, wav) == kRate);

    const auto speech = energySpeech(raw, static_cast<std::int64_t>(pcm.size()));
    REQUIRE(speech.size() == 2);
    CHECK(std::abs(speech[0].begin - 1 * kRate) <= kRate / 50);
    CHECK(std::abs(speech[0].end - 3 * kRate) <= kRate / 50);
    CHECK(std::abs(speech[1].begin - 4 * kRate) <= kRate / 50);
    CHECK(std::abs(speech[1].end - 5 * kRate) <= kRate / 50);

    CHECK(cpuFallbackNote("Vulkan0").empty());
    CHECK(cpuFallbackNote("").empty());
    CHECK_FALSE(cpuFallbackNote("CPU").empty());
}

// --- AudioEnergyService ---------------------------------------------------------------------------------------

TEST_CASE("AudioEnergyService measures RMS and peak per range, cached in memory and on disk", "[ai][energy]") {
    const auto dir = test::freshDir("energy");
    // [0, 1) sine 0.5; [1, 2) digital silence; [2, 3) square wave of 0.25; [3, 4) sine 0.5.
    std::vector<float> pcm = toneTrack(4.0, {{0.0, 1.0}, {3.0, 4.0}});
    for (std::size_t i = 2 * kRate; i < 3 * kRate; ++i) {
        pcm[i] = (i / 20) % 2 == 0 ? 0.25f : -0.25f;
    }
    const std::string file = writeWav(dir / "levels.wav", pcm).string();
    AudioEnergyServiceConfig config;
    config.cacheRoot = dir / "cache";
    config.workDir = dir / "work";
    config.local = localEngine(dir);
    AudioEnergyService service(config);

    EnergyRequest request{file, -1, {{sec(0), sec(1)}, {sec(1), sec(1)}, {sec(2), sec(1)}, {sec(0.5), sec(3)}}, false};
    auto job = service.measure(request);
    const auto& report = job.wait();
    REQUIRE(report);
    CHECK(job.progress() == 1.0f);
    CHECK_FALSE(report->fromCache);
    CHECK(report->fingerprint != 0);
    REQUIRE(report->ranges.size() == 4);
    CHECK_THAT(report->ranges[0].rmsDb, WithinAbs(20.0 * std::log10(0.5 / std::numbers::sqrt2), 0.01)); // -9.03
    CHECK_THAT(report->ranges[0].peakDb, WithinAbs(20.0 * std::log10(0.5), 0.01));                       // -6.02
    CHECK(std::isinf(report->ranges[1].rmsDb));
    CHECK(report->ranges[1].rmsDb < 0);
    CHECK(std::isinf(report->ranges[1].peakDb));
    CHECK_THAT(report->ranges[2].rmsDb, WithinAbs(20.0 * std::log10(0.25), 0.01)); // -12.04
    CHECK_THAT(report->ranges[2].peakDb, WithinAbs(20.0 * std::log10(0.25), 0.01));
    CHECK(report->ranges[3].rmsDb < report->ranges[0].rmsDb); // half silence in it
    CHECK_FALSE(report->ranges[0].speech);                    // not asked for

    auto again = service.run(request);
    REQUIRE(again);
    CHECK(again->fromCache);
    CHECK(again->ranges == report->ranges);
    REQUIRE(service.cached(file, -1, {sec(2), sec(1)}));
    CHECK(service.cached(file, -1, {sec(2), sec(1)})->rmsDb == report->ranges[2].rmsDb);
    CHECK_FALSE(service.cached(file, -1, {sec(2), sec(2)}));

    // Another service on the same analysis cache (the next session) reads it from disk.
    AudioEnergyService next(config);
    auto fromDisk = next.run(request);
    REQUIRE(fromDisk);
    CHECK(fromDisk->fromCache);
    CHECK(fromDisk->ranges == report->ranges);

    std::stop_source cancelled;
    cancelled.request_stop();
    const auto cancelledHit = next.run(request, cancelled.get_token());
    REQUIRE_FALSE(cancelledHit);
    CHECK(cancelledHit.error().code == ErrorCode::Cancelled);

    // A damaged cache entry is a miss, never an exception escaping the worker thread.
    AnalysisCache cache(config.cacheRoot);
    const auto entry = cache.newest(report->fingerprint, report->streamIndex, "energy");
    REQUIRE(entry);
    REQUIRE(cache.put(entry->key, R"({"ranges":[42,42,42,42]})"));
    AudioEnergyService repaired(config);
    const auto repair = repaired.measure(request).wait();
    REQUIRE(repair);
    CHECK_FALSE(repair->fromCache);
    CHECK(repair->ranges == report->ranges);

    CHECK_FALSE(service.run(EnergyRequest{(dir / "missing.wav").string(), -1, {{sec(0), sec(1)}}, false}));
}

TEST_CASE("AudioEnergyService: the speech fraction from the VAD, and cancel", "[ai][energy]") {
    const auto dir = test::freshDir("energy-vad");
    const std::string file = writeWav(dir / "talk.wav", toneTrack(6.0, {{1.0, 3.0}, {4.0, 5.0}})).string();
    AudioEnergyServiceConfig config;
    config.cacheRoot = dir / "cache";
    config.workDir = dir / "work";
    config.local = localEngine(dir);
    AudioEnergyService service(config);

    // The fake helper's VAD: 10 ms frames with |sample| > 0.01 are speech.
    config.vadModel = "fake-vad";
    AudioEnergyService withVad(config);
    auto report = withVad.run(EnergyRequest{file, -1, {{sec(1), sec(2)}, {sec(3), sec(1)}, {sec(2.5), sec(2)}}, true});
    REQUIRE(report);
    REQUIRE(report->ranges[0].speech);
    CHECK(*report->ranges[0].speech > 0.9f);
    CHECK(*report->ranges[1].speech < 0.1f);
    CHECK_THAT(*report->ranges[2].speech, WithinAbs(0.5, 0.1)); // [2.5, 3) and [4, 4.5) of [2.5, 4.5)

    // Without the helper the speech fraction stays unmeasured; the levels are still there.
    AudioEnergyServiceConfig bare = config;
    bare.local.inferExecutable = dir / "not-built" / "wabcut-infer.exe";
    bare.cacheRoot.clear();
    AudioEnergyService noVad(bare);
    auto levels = noVad.run(EnergyRequest{file, -1, {{sec(1), sec(2)}}, true});
    REQUIRE(levels);
    CHECK_FALSE(levels->ranges[0].speech);

    std::stop_source stop;
    stop.request_stop();
    CHECK(service.run(EnergyRequest{file, -1, {{sec(0), sec(6)}}, false}, stop.get_token()).error().code ==
          ErrorCode::Cancelled);
}

TEST_CASE("measureSamples", "[ai][energy]") {
    CHECK(std::isinf(measureSamples({}).rmsDb));
    const std::vector<float> square{0.5f, -0.5f, 0.5f, -0.5f};
    const EnergyMeasure m = measureSamples(square);
    CHECK_THAT(m.rmsDb, WithinAbs(-6.0206, 0.001));
    CHECK_THAT(m.peakDb, WithinAbs(-6.0206, 0.001));
    const std::vector<float> zeros(100, 0.0f);
    CHECK(std::isinf(measureSamples(zeros).peakDb));
}

TEST_CASE("Simultaneous cloud jobs isolate uploads and clean up independently", "[ai][transcribe][cloud][cancel]") {
    const auto dir = test::freshDir("transcribe-isolation");
    const TranscribeSource a{writeWav(dir / "a.wav", toneTrack(2, {{0, 2}}, 0.25f)).string(), -1};
    const TranscribeSource b{writeWav(dir / "b.wav", toneTrack(2, {{0, 2}}, 0.75f)).string(), -1};
    Cloud first;
    Cloud second;
    first.calls.waitForStop = true;
    second.calls.waitForStop = true;
    TranscribeService one(serviceConfig(dir, false, &first.platform));
    TranscribeService two(serviceConfig(dir, false, &second.platform));
    TranscribeJobOptions options;
    options.provider = "cloud:stt-model";
    options.allowOnce = true;
    options.useCache = false;
    auto jobA = one.transcribe(a, options);
    auto jobB = two.transcribe(b, options);
    for (int i = 0; i < 1000 && !(first.calls.started && second.calls.started); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(first.calls.started);
    REQUIRE(second.calls.started);
    const auto pathA = first.calls.requests.front().audioFile;
    const auto pathB = second.calls.requests.front().audioFile;
    CHECK(pathA.parent_path() != pathB.parent_path());
    auto peakSample = [](const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        in.seekg(44 + 10 * 2); // 400 Hz sine reaches its first peak at sample 10
        std::int16_t sample = 0;
        in.read(reinterpret_cast<char*>(&sample), sizeof(sample));
        return sample;
    };
    CHECK_THAT(static_cast<double>(peakSample(pathA)), WithinAbs(0.25 * 32767, 1));
    CHECK_THAT(static_cast<double>(peakSample(pathB)), WithinAbs(0.75 * 32767, 1));
    jobA.cancel();
    REQUIRE_FALSE(jobA.wait());
    CHECK(jobA.wait().error().code == ErrorCode::Cancelled);
    CHECK_FALSE(std::filesystem::exists(pathA.parent_path()));
    CHECK(std::filesystem::exists(pathB));
    second.calls.waitForStop = false;
    REQUIRE(jobB.wait());
    CHECK_FALSE(std::filesystem::exists(pathB.parent_path()));
    CHECK(std::filesystem::is_empty(dir / "work"));
}

TEST_CASE("Cancelled extraction removes partial audio before any upload", "[ai][transcribe][cloud][cancel]") {
    const auto dir = test::freshDir("transcribe-extract-cancel");
    const TranscribeSource source{writeWav(dir / "long.wav", toneTrack(30, {{0, 30}})).string(), -1};
    Cloud cloud;
    TranscribeService service(serviceConfig(dir, false, &cloud.platform));
    TranscribeJobOptions options;
    options.provider = "cloud:stt-model";
    options.allowOnce = true;
    std::stop_source stop;
    bool sawPartialFile = false;
    auto result = service.run(source, options, stop.get_token(), [&](float) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir / "work")) {
            sawPartialFile = sawPartialFile || entry.path().extension() == ".f32";
        }
        stop.request_stop();
    });
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::Cancelled);
    CHECK(sawPartialFile);
    CHECK(cloud.calls.requests.empty());
    CHECK(std::filesystem::is_empty(dir / "work"));
}

TEST_CASE("Local extraction failure cleans its reserved workspace", "[ai][transcribe]") {
    const auto dir = test::freshDir("transcribe-extract-failure");
    const TranscribeSource source{writeWav(dir / "audio.wav", toneTrack(2, {{0, 2}})).string(), -1};
    auto config = serviceConfig(dir, true);
    config.local.workDir = dir / "local-work";
    std::filesystem::path partial;
    config.local.extract = [&](const TranscriptionRequest&, const AudioIdentity&, const std::filesystem::path& pcm,
                               std::stop_token, const std::function<void(float)>&) -> std::expected<ExtractedAudio, Error> {
        partial = pcm;
        std::ofstream(pcm, std::ios::binary) << "partial audio";
        return std::unexpected(Error{ErrorCode::IoError, "simulated extraction failure"});
    };
    TranscribeService service(config);
    const auto result = service.run(source, localOptions());
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::IoError);
    REQUIRE_FALSE(partial.empty());
    CHECK_FALSE(std::filesystem::exists(partial.parent_path()));
    CHECK(std::filesystem::is_empty(dir / "local-work"));
}

TEST_CASE("Cloud chunk dispatch rechecks a newly enabled no-cloud lock", "[ai][transcribe][cloud]") {
    const auto dir = test::freshDir("transcribe-lock-chunks");
    const TranscribeSource source{writeWav(dir / "audio.wav", toneTrack(6, {{0, 6}})).string(), -1};
    Cloud cloud(2);
    TranscribeService service(serviceConfig(dir, false, &cloud.platform));
    TranscribeJobOptions options;
    options.provider = "cloud:stt-model";
    options.allowOnce = true;
    auto result = service.run(source, options, {}, [&](float p) {
        if (p > 0.2f) {
            cloud.consent.setProjectLock(true);
        }
    });
    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("Offline") != std::string::npos);
    CHECK(cloud.calls.requests.size() == 1);
    CHECK(std::filesystem::is_empty(dir / "work"));
    CHECK_FALSE(service.cached(source));
}

TEST_CASE("Redacted audio is silenced before any transcriber hears it and never comes back as words",
          "[ai][transcribe][redaction]") {
    // Review P1-3. Before the fix the whole source went up (a bleeped name included) and came back as text.
    const auto dir = test::freshDir("transcribe-redaction");
    const std::vector<TimeRange> redacted{TimeRange::fromStartEnd(sec(1.5), sec(2.5))};

    SECTION("a cloud upload carries silence there") {
        const TranscribeSource source{writeWav(dir / "cloud.wav", toneTrack(6.0, {{1.0, 3.0}})).string(), -1, redacted};
        Cloud cloud;
        TranscribeService service(serviceConfig(dir, false, &cloud.platform));
        TranscribeJobOptions options;
        options.provider = "cloud:stt-model";
        options.allowOnce = true;
        REQUIRE(service.run(source, options));
        REQUIRE(cloud.calls.uploads.size() == 1);
        const std::vector<std::int16_t>& up = cloud.calls.uploads[0];
        const auto peak = [&](double from, double to) {
            int m = 0;
            for (auto i = static_cast<std::size_t>(from * kRate); i < static_cast<std::size_t>(to * kRate) && i < up.size(); ++i) {
                m = std::max(m, std::abs(static_cast<int>(up[i])));
            }
            return m;
        };
        CHECK(peak(1.5, 2.5) == 0);     // the redacted speech
        CHECK(peak(1.0, 1.5) > 10000);  // the rest of the tone went up
        CHECK(peak(2.5, 3.0) > 10000);
    }
    SECTION("local whisper never hears it") {
        const TranscribeSource source{writeWav(dir / "local.wav", toneTrack(6.0, {{1.0, 3.0}, {4.0, 5.0}})).string(), -1,
                                      {TimeRange::fromStartEnd(sec(4.0), sec(5.0))}};
        TranscribeService service(serviceConfig(dir, true));
        auto done = service.run(source, localOptions());
        REQUIRE(done);
        CHECK(texts(done->transcript) == std::vector<std::string>{"w1", "w2"}); // no "w4": that second was silenced
    }
    SECTION("words there are replaced in every result, cached ones too") {
        const std::string file = writeWav(dir / "words.wav", toneTrack(4.0, {{0.5, 3.5}})).string();
        TranscribeService service(serviceConfig(dir, false));
        service.addProvider("words", "Words", [](const TranscribeSource&, const TranscribeJobOptions&,
                                                 const std::function<void(float)>&, std::stop_token)
                                                  -> std::expected<Transcript, Error> {
            Transcript t;
            t.language = "en";
            for (const auto& [text, a, b] : {std::tuple{"keep", 0.5, 1.0}, std::tuple{"secret", 1.6, 2.0},
                                             std::tuple{"after", 3.0, 3.4}}) {
                TranscriptWord w;
                w.text = text;
                w.t0 = sec(a);
                w.t1 = sec(b);
                w.confidence = 0.9f;
                t.words.push_back(w);
            }
            t.segments.push_back(TranscriptSegment{t.words[0].t0, t.words[2].t1, 0, 3, "keep secret after", std::nullopt});
            return t;
        });
        TranscribeJobOptions options;
        options.provider = "words";
        // Transcribed before the redaction existed: cached with the word.
        auto before = service.run(TranscribeSource{file, -1, {}}, options);
        REQUIRE(before);
        CHECK(before->transcript.words[1].text == "secret");
        // The cache hit after the redaction was added comes back redacted.
        auto hit = service.run(TranscribeSource{file, -1, redacted}, options);
        REQUIRE(hit);
        CHECK(hit->fromCache);
        CHECK(texts(hit->transcript) == std::vector<std::string>{"keep", std::string(kRedactedWord), "after"});
        CHECK(hit->transcript.segments[0].text.find("secret") == std::string::npos);
        CHECK(hit->transcript.words[1].confidence == 0.0f);
        REQUIRE(service.cached(TranscribeSource{file, -1, redacted}));
        CHECK(service.cached(TranscribeSource{file, -1, redacted})->words[1].text == kRedactedWord);
        // A fresh transcription with the redaction: redacted before it is stored.
        options.useCache = false;
        auto fresh = service.run(TranscribeSource{file, -1, redacted}, options);
        REQUIRE(fresh);
        CHECK(fresh->transcript.words[1].text == kRedactedWord);
    }
}

TEST_CASE("audioRedactionRanges: enabled audio redactions of one media, merged; video-only ones ignored",
          "[ai][transcribe][redaction]") {
    ProjectState state;
    const MediaId a = state.ids.allocate<MediaId>();
    const MediaId b = state.ids.allocate<MediaId>();
    const auto add = [&](MediaId media, TimeRange source, RedactionAudio audio, std::vector<TimeRange> words, bool enabled) {
        Redaction r;
        r.id = state.ids.allocate<RedactionId>();
        r.media = media;
        r.source = source;
        r.audio = audio;
        r.audioRanges = std::move(words);
        r.enabled = enabled;
        state.redactions.push_back(r);
    };
    add(a, TimeRange::fromStartEnd(sec(10), sec(20)), RedactionAudio::Bleep, {TimeRange::fromStartEnd(sec(12), sec(13))}, true);
    add(a, TimeRange::fromStartEnd(sec(12.5), sec(14)), RedactionAudio::Mute, {}, true);     // overlaps: merged
    add(a, TimeRange::fromStartEnd(sec(30), sec(31)), RedactionAudio::None, {}, true);      // picture only
    add(a, TimeRange::fromStartEnd(sec(40), sec(41)), RedactionAudio::Mute, {}, false);     // disabled
    add(b, TimeRange::fromStartEnd(sec(1), sec(2)), RedactionAudio::Mute, {}, true);        // another media
    const auto ranges = audioRedactionRanges(state, a);
    REQUIRE(ranges.size() == 1);
    CHECK(ranges[0] == TimeRange::fromStartEnd(sec(12), sec(14)));
    CHECK(audioRedactionRanges(state, b).size() == 1);
}

TEST_CASE("Non-ANSI paths and exceptions in a job end as errors, not crashes", "[ai][transcribe]") {
    // Review P2-13. Before the fix error messages used path::string(), which throws on Windows for characters outside
    // the ANSI code page, and the job threads had no guard: std::terminate.
    const auto dir = test::freshDir("transcribe-errors");
    const auto missing = dir / std::filesystem::path(u8"missing-\U0001F600.f32");
    const auto wav = writeWav16k(missing, 0, 16000, dir / std::filesystem::path(u8"out-\U0001F600.wav"));
    REQUIRE_FALSE(wav);
    CHECK(wav.error().code == ErrorCode::IoError);
    CHECK(wav.error().message.find("missing-") != std::string::npos);

    const TranscribeSource source{writeWav(dir / "talk.wav", toneTrack(1.0, {{0.2, 0.8}})).string(), -1};
    TranscribeService service(serviceConfig(dir, false));
    service.addProvider("boom", "Boom", [](const TranscribeSource&, const TranscribeJobOptions&,
                                           const std::function<void(float)>&, std::stop_token) -> std::expected<Transcript, Error> {
        throw std::runtime_error("the engine fell over");
    });
    TranscribeJobOptions options;
    options.provider = "boom";
    auto job = service.transcribe(source, options);
    const auto& result = job.wait();
    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("the engine fell over") != std::string::npos);
}

TEST_CASE("A media path that is not UTF-8 is refused, not thrown", "[ai][transcribe]") {
    // Review P2-14: an ANSI path (what Windows argv holds) reached path(const char8_t*), which throws.
    const auto dir = test::freshDir("transcribe-not-utf8");
    TranscribeService service(serviceConfig(dir, false));
    const auto notUtf8 = service.run(TranscribeSource{std::string("talk-\xE9.wav"), -1}, TranscribeJobOptions{});
    REQUIRE_FALSE(notUtf8);
    CHECK(notUtf8.error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("A refused cloud transcription decodes nothing", "[ai][transcribe][cloud]") {
    // Review P2-16: before the fix the whole file was extracted (and run through VAD) before the first guarded
    // request was refused; nothing was uploaded, but minutes were wasted.
    const auto dir = test::freshDir("transcribe-refused");
    const TranscribeSource source{writeWav(dir / "talk.wav", toneTrack(6.0, {{1.0, 3.0}})).string(), -1};
    Cloud cloud;
    TranscribeService service(serviceConfig(dir, false, &cloud.platform));
    TranscribeJobOptions options;
    options.provider = "cloud:stt-model";
    float progress = 0.0f;
    const auto track = [&](float p) { progress = std::max(progress, p); };

    const auto consent = service.run(source, options, {}, track); // no "Allow once", nothing recorded
    REQUIRE_FALSE(consent);
    CHECK(consent.error().message.find("ConsentRequired") != std::string::npos);
    CHECK(progress == 0.0f);

    options.allowOnce = true;
    cloud.consent.setProjectLock(true);
    REQUIRE_FALSE(service.run(source, options, {}, track));
    CHECK(progress == 0.0f);
    cloud.consent.setProjectLock(false);

    cloud.spend.setMonthlyCap("stt", MicroUsd{0});
    const auto cap = service.run(source, options, {}, track);
    REQUIRE_FALSE(cap);
    CHECK(cap.error().message.find("SpendCapExceeded") != std::string::npos);
    CHECK(progress == 0.0f);
    CHECK(cloud.calls.requests.empty());
}

TEST_CASE("AudioEnergyService: a replaced file is not answered from memory", "[ai][energy][cache]") {
    // Review P2-17: before the fix cached() was keyed by path and stream only, so after the file was replaced in
    // place it kept answering with the old file's levels.
    const auto dir = test::freshDir("energy-replaced");
    const auto file = dir / "take.wav";
    writeWav(file, toneTrack(2.0, {{0.0, 2.0}}, 0.5f));
    AudioEnergyServiceConfig config;
    config.cacheRoot.clear();
    config.local = localEngine(dir, false);
    AudioEnergyService service(config);
    const TimeRange range{sec(0.0), sec(1.0)};
    REQUIRE(service.run(EnergyRequest{file.string(), -1, {range}, false}));
    REQUIRE(service.cached(file.string(), -1, range));

    writeWav(file, toneTrack(3.0, {{0.0, 3.0}}, 0.125f)); // another take, same name
    CHECK_FALSE(service.cached(file.string(), -1, range));
    const auto fresh = service.run(EnergyRequest{file.string(), -1, {range}, false});
    REQUIRE(fresh);
    CHECK_FALSE(fresh->fromCache);
    CHECK_THAT(fresh->ranges[0].rmsDb, WithinAbs(20.0 * std::log10(0.125 / std::numbers::sqrt2), 0.01));
    REQUIRE(service.cached(file.string(), -1, range));
    CHECK(service.cached(file.string(), -1, range)->rmsDb == fresh->ranges[0].rmsDb);
}
