// TranscriptionService end to end against the scripted fake helper (tests/ai/fake_infer.cpp).
// No model is downloaded and no real inference runs here (see test_real_infer.cpp for that).

#include "AiTestUtil.h"

#include <wabcut/ai/ChildProcess.h>
#include <wabcut/ai/InferProtocol.h>
#include <wabcut/ai/Sha256.h>
#include <wabcut/ai/Text.h>
#include <wabcut/ai/TranscriptJson.h>
#include <wabcut/ai/TranscriptionService.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <fstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#endif

using namespace wabcut;
using namespace wabcut::ai;
using namespace std::chrono_literals;

namespace {

constexpr std::int64_t kS = 16000;

Ticks ms(std::int64_t v) { return Ticks{v * (kTicksPerSecond / 1000)}; }

/// 45 s of 16 kHz PCM with tone bursts at [1, 3), [5, 6.5) and [40, 42) s.
std::vector<float> syntheticSpeech() {
    std::vector<float> pcm(static_cast<std::size_t>(45 * kS), 0.0f);
    for (const auto [a, b] : {std::pair{1.0, 3.0}, {5.0, 6.5}, {40.0, 42.0}}) {
        for (auto i = static_cast<std::size_t>(a * kS); i < static_cast<std::size_t>(b * kS); ++i) {
            pcm[i] = 0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 300.0 * static_cast<double>(i) / kS));
        }
    }
    return pcm;
}

bool processGone(int pid) {
#ifdef _WIN32
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (h == nullptr) {
        return true;
    }
    const bool gone = WaitForSingleObject(h, 2000) == WAIT_OBJECT_0;
    CloseHandle(h);
    return gone;
#else
    for (int i = 0; i < 200 && ::kill(pid, 0) == 0; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    return ::kill(pid, 0) != 0;
#endif
}

int readPid(const std::filesystem::path& file) {
    for (int i = 0; i < 300; ++i) {
        std::ifstream in(file);
        int pid = 0;
        if (in >> pid && pid > 0) {
            return pid;
        }
        std::this_thread::sleep_for(10ms);
    }
    return 0;
}

int countLines(const std::filesystem::path& file) {
    std::ifstream in(file);
    int n = 0;
    for (std::string line; std::getline(in, line);) {
        ++n;
    }
    return n;
}

/// A service wired to the fake helper, fake models and synthetic audio (no media decoding).
struct Fixture {
    std::filesystem::path dir;
    TranscriptionServiceConfig config;
    std::vector<float> pcm = syntheticSpeech();
    int extractions = 0;

    explicit Fixture(const std::string& name, std::vector<std::string> helperArgs = {}) : dir(test::freshDir(name)) {
        config.inferExecutable = WABCUT_FAKE_INFER;
        config.helperArguments = std::move(helperArgs);
        config.modelRoot = dir / "models";
        config.cacheRoot = dir / "cache";
        config.workDir = dir / "work";
        config.helperLog = dir / "helper.log";
        config.models = {installed("fake-whisper", ModelKind::Whisper), installed("fake-vad", ModelKind::Vad)};
        config.identify = [](const TranscriptionRequest& r) -> std::expected<AudioIdentity, Error> {
            return AudioIdentity{std::hash<std::string>{}(r.mediaPath) | 1, 1, Ticks::fromSeconds(45)};
        };
        config.extract = [this](const TranscriptionRequest&, const AudioIdentity&, const std::filesystem::path& file,
                                std::stop_token, const std::function<void(float)>& progress)
            -> std::expected<ExtractedAudio, Error> {
            ++extractions;
            std::ofstream out(file, std::ios::binary);
            out.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(pcm.size() * sizeof(float)));
            progress(1.0f);
            return ExtractedAudio{static_cast<std::int64_t>(pcm.size()), Ticks{}};
        };
    }

    /// A model "installed" in the store: a small file plus the verification marker.
    ModelInfo installed(const std::string& id, ModelKind kind) {
        const std::string body = "fake weights for " + id;
        ModelInfo m{id, kind, id + ".bin", "https://example.invalid/" + id, body.size(), Sha256::hex(body), "MIT", "test", ""};
        std::filesystem::create_directories(dir / "models");
        std::ofstream(dir / "models" / m.fileName, std::ios::binary) << body;
        std::ofstream(dir / "models" / (m.fileName + ".verified"), std::ios::binary) << m.sha256 << '\n';
        return m;
    }

    TranscriptionRequest request(std::string path = "C:/media/clip.mov") const {
        TranscriptionRequest r;
        r.mediaPath = std::move(path);
        r.options.model = "fake-whisper";
        r.options.vadModel = "fake-vad";
        r.options.beamSize = 1;
        return r;
    }
};

std::vector<std::string> wordTexts(const Transcript& t) {
    std::vector<std::string> out;
    for (const TranscriptWord& w : t.words) {
        out.push_back(w.text);
    }
    return out;
}

} // namespace

TEST_CASE("Transcription maps helper output to exact source times through VAD chunks", "[ai][service]") {
    Fixture f("service-basic");
    TranscriptionService service(f.config);
    TranscriptionRequest request = f.request();
    std::vector<std::size_t> partialSegments;
    request.onSegment = [&](const Transcript& sofar) { partialSegments.push_back(sofar.segments.size()); };
    TranscriptionStats stats;
    request.onStats = [&](const TranscriptionStats& s) { stats = s; };
    std::vector<float> progress;
    auto t = service.run(request, {}, [&](float p) { progress.push_back(p); });
    REQUIRE(t);
    REQUIRE(t->validate());

    // Speech at [1, 3) [5, 6.5) [40, 42) s: one word per second, named by absolute second.
    CHECK(wordTexts(*t) == std::vector<std::string>{"w1", "w2", "w5", "w6", "w40", "w41"});
    CHECK(t->words[0].t0 == ms(1000));
    CHECK(t->words[0].t1 == ms(2000));
    CHECK(t->words[3].t0 == ms(6000));
    CHECK(t->words[3].t1 == ms(6500));
    CHECK(t->words[4].t0 == ms(40000)); // second chunk: the long silence was skipped
    CHECK(t->words[5].t1 == ms(42000));
    CHECK(t->words[0].confidence == 0.8f);
    REQUIRE(t->segments.size() == 3);
    CHECK(t->segments[0].t0 == ms(1000));
    CHECK(t->segments[2].t1 == ms(42000));
    CHECK(t->language == "en");
    CHECK(t->source.streamIndex == 1);
    CHECK(t->source.path == "C:/media/clip.mov");
    CHECK(t->provenance.engine == "fake");
    CHECK(t->provenance.modelId == "fake-whisper");
    CHECK(t->provenance.params.at("language") == "auto");
    CHECK(t->provenance.params.at("device") == "FakeCPU");
    CHECK_FALSE(t->provenance.createdAt.empty());

    CHECK(partialSegments == std::vector<std::size_t>{1, 2, 3});
    CHECK(stats.device == "FakeCPU");
    CHECK(stats.mediaSeconds == 45.0);
    CHECK(stats.speechSeconds < 10.0); // only the chunks, not the silence
    CHECK_FALSE(stats.fromCache);
    REQUIRE_FALSE(progress.empty());
    CHECK(std::ranges::is_sorted(progress));
    CHECK(progress.back() == 1.0f);
    // Temporary PCM is removed.
    CHECK(std::filesystem::is_empty(f.config.workDir));
}

TEST_CASE("Without VAD the whole audio goes through in fixed chunks", "[ai][service]") {
    Fixture f("service-novad");
    TranscriptionService service(f.config);
    TranscriptionRequest request = f.request();
    request.options.vad = false;
    request.options.chunking.maxChunkSamples = 20 * kS; // chunks [0,15) [15,30) [30,45)
    auto t = service.run(request);
    REQUIRE(t);
    CHECK(wordTexts(*t) == std::vector<std::string>{"w1", "w2", "w5", "w6", "w40", "w41"});
    CHECK(t->words[4].t0 == ms(40000));
    CHECK(t->provenance.params.at("vad") == "off");
}

TEST_CASE("Thai output: byte-split tokens become whole words, no whitespace splitting", "[ai][service]") {
    Fixture f("service-thai", {"--thai"});
    TranscriptionService service(f.config);
    TranscriptionRequest request = f.request();
    request.options.language = "th";
    auto t = service.run(request);
    REQUIRE(t);
    CHECK(t->language == "th");
    REQUIRE(t->words.size() == 6);
    for (const TranscriptWord& w : t->words) {
        CHECK(text::isValidUtf8(w.text));
        CHECK_FALSE(text::startsWithCombiningMark(w.text));
    }
    // Syllable (second % 7) for seconds 1, 2, 5, 6, 40, 41.
    CHECK(wordTexts(*t) == std::vector<std::string>{"ดี", "ครับ", "วิดี", "โอ", "วิดี", "โอ"});
    CHECK(t->words[0].t0 == ms(1000));
    CHECK(t->words[0].t1 == ms(2000)); // both tokens' span
    CHECK(t->words[0].confidence == (0.9f + 0.7f) / 2.0f);
}

TEST_CASE("Transcripts are cached per source, model and settings", "[ai][service][cache]") {
    Fixture f("service-cache", {"--runs-file", (test::freshDir("runs") / "runs.txt").string()});
    const std::filesystem::path runs = f.config.helperArguments[1];
    TranscriptionService service(f.config);
    auto first = service.run(f.request());
    REQUIRE(first);
    CHECK(countLines(runs) == 1);

    TranscriptionStats stats;
    TranscriptionRequest again = f.request();
    again.onStats = [&](const TranscriptionStats& s) { stats = s; };
    auto second = service.run(again);
    REQUIRE(second);
    CHECK(countLines(runs) == 1); // served from the cache: no helper, no extraction
    CHECK(f.extractions == 1);
    CHECK(stats.fromCache);
    CHECK(*second == *first);

    TranscriptionRequest otherSettings = f.request();
    otherSettings.options.beamSize = 3;
    REQUIRE(service.run(otherSettings));
    CHECK(countLines(runs) == 2);

    REQUIRE(service.run(f.request("D:/other/file.mov"))); // another source
    CHECK(countLines(runs) == 3);

    TranscriptionRequest noCache = f.request();
    noCache.options.useCache = false;
    REQUIRE(service.run(noCache));
    CHECK(countLines(runs) == 4);

    // The cache is source-scoped: a second service (another project) on the same cache reuses the result.
    TranscriptionService other(f.config);
    REQUIRE(other.run(f.request()));
    CHECK(countLines(runs) == 4);
}

TEST_CASE("A crashing helper ends the job with a clean error", "[ai][service][failure]") {
    Fixture f("service-crash", {"--mode", "crash-on-transcribe"});
    TranscriptionService service(f.config);
    const auto t = service.run(f.request());
    REQUIRE_FALSE(t);
    CHECK(t.error().code == ErrorCode::DecoderError);
    CHECK(t.error().message.find("exit code 57") != std::string::npos);
    CHECK(t.error().message.find("transcribing") != std::string::npos);
}

TEST_CASE("A hanging helper is stopped after the inactivity timeout", "[ai][service][failure]") {
    const auto pidFile = test::freshDir("pid-hang") / "pid.txt";
    Fixture f("service-hang", {"--mode", "hang-on-transcribe", "--pid-file", pidFile.string()});
    f.config.inactivityTimeout = 500ms;
    TranscriptionService service(f.config);
    const auto started = std::chrono::steady_clock::now();
    const auto t = service.run(f.request());
    CHECK(std::chrono::steady_clock::now() - started < 10s);
    REQUIRE_FALSE(t);
    CHECK(t.error().code == ErrorCode::DecoderError);
    CHECK(t.error().message.find("did not answer") != std::string::npos);
    const int pid = readPid(pidFile);
    REQUIRE(pid > 0);
    CHECK(processGone(pid));
}

TEST_CASE("Malformed helper output is a protocol error", "[ai][service][failure]") {
    Fixture f("service-garbage", {"--mode", "garbage"});
    TranscriptionService service(f.config);
    const auto t = service.run(f.request());
    REQUIRE_FALSE(t);
    CHECK(t.error().message.find("invalid message") != std::string::npos);
}

TEST_CASE("Helper start-up failures are reported", "[ai][service][failure]") {
    SECTION("exits at once") {
        Fixture f("service-exit", {"--mode", "exit-at-start"});
        TranscriptionService service(f.config);
        const auto t = service.run(f.request());
        REQUIRE_FALSE(t);
        CHECK(t.error().code == ErrorCode::DecoderError);
    }
    SECTION("speaks another protocol") {
        Fixture f("service-proto", {"--mode", "wrong-protocol"});
        TranscriptionService service(f.config);
        const auto t = service.run(f.request());
        REQUIRE_FALSE(t);
        CHECK(t.error().code == ErrorCode::VersionMismatch);
    }
    SECTION("is missing") {
        Fixture f("service-nohelper");
        f.config.inferExecutable = f.dir / "missing-helper.exe";
        TranscriptionService service(f.config);
        const auto t = service.run(f.request());
        REQUIRE_FALSE(t);
        CHECK(t.error().code == ErrorCode::NotFound);
    }
    SECTION("unknown or missing models") {
        Fixture f("service-models");
        TranscriptionService service(f.config);
        TranscriptionRequest unknown = f.request();
        unknown.options.model = "no-such-model";
        CHECK(service.run(unknown).error().code == ErrorCode::InvalidArgument);
        TranscriptionRequest notDownloaded = f.request();
        notDownloaded.options.model = "tiny"; // built in, but not in this store, and no transport
        const auto t = service.run(notDownloaded);
        REQUIRE_FALSE(t);
        CHECK(t.error().code == ErrorCode::NotFound);
        CHECK(t.error().message.find("not downloaded") != std::string::npos);
    }
}

TEST_CASE("Cancelling a transcription job kills the helper promptly", "[ai][service][cancel]") {
    const auto pidFile = test::freshDir("pid-cancel") / "pid.txt";
    Fixture f("service-cancel", {"--mode", "slow", "--pid-file", pidFile.string()});
    TranscriptionService service(f.config);
    media::Job<Transcript> job = service.transcribe(f.request());
    const int pid = readPid(pidFile);
    REQUIRE(pid > 0);
    for (int i = 0; i < 500 && job.progress() < 0.21f; ++i) { // transcribing (the fake reports progress)
        std::this_thread::sleep_for(10ms);
    }
    CHECK(job.progress() > 0.2f);
    const auto cancelled = std::chrono::steady_clock::now();
    job.cancel();
    REQUIRE(job.waitFor(10s));
    CHECK(std::chrono::steady_clock::now() - cancelled < 3s);
    REQUIRE_FALSE(job.wait());
    CHECK(job.wait().error().code == ErrorCode::Cancelled);
    CHECK(processGone(pid));
}

TEST_CASE("Destroying the service cancels its running jobs", "[ai][service][cancel]") {
    Fixture f("service-dtor", {"--mode", "slow"});
    media::Job<Transcript> job;
    {
        TranscriptionService service(f.config);
        job = service.transcribe(f.request());
        std::this_thread::sleep_for(300ms);
    }
    REQUIRE(job.isFinished());
    CHECK(job.wait().error().code == ErrorCode::Cancelled);
}

TEST_CASE("Jobs run concurrently and report results through the Job handle", "[ai][service]") {
    Fixture f("service-jobs");
    TranscriptionService service(f.config);
    TranscriptionRequest a = f.request("C:/a.mov");
    TranscriptionRequest b = f.request("C:/b.mov");
    a.options.useCache = false;
    b.options.useCache = false;
    auto ja = service.transcribe(a);
    auto jb = service.transcribe(b);
    REQUIRE(ja.waitFor(60s));
    REQUIRE(jb.waitFor(60s));
    REQUIRE(ja.wait());
    REQUIRE(jb.wait());
    CHECK(ja.wait()->words.size() == 6);
    CHECK(jb.wait()->source.path == "C:/b.mov");
    CHECK(ja.progress() == 1.0f);
}

// ---- 0.7 (card 111): a recording that switches language -------------------------------------------------------------

namespace {

/// 30 s of 16 kHz PCM with tone bursts at [1, 5), [12, 16) and [22, 26) s. With the fake helper's
/// --alternate-languages a request that begins in 0-10 s or 20-30 s is heard as English, one in 10-20 s as Thai: the
/// mixed-languages chunks begin 0.25 s before each burst, so the three stretches are English, Thai, English.
std::vector<float> threeStretches() {
    std::vector<float> pcm(static_cast<std::size_t>(30 * kS), 0.0f);
    for (const auto [a, b] : {std::pair{1.0, 5.0}, {12.0, 16.0}, {22.0, 26.0}}) {
        for (auto i = static_cast<std::size_t>(a * kS); i < static_cast<std::size_t>(b * kS); ++i) {
            pcm[i] = 0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 300.0 * static_cast<double>(i) / kS));
        }
    }
    return pcm;
}

} // namespace

TEST_CASE("Mixed languages: every stretch of speech is transcribed in its own language", "[ai][service][mixed]") {
    Fixture f("service-mixed", {"--alternate-languages"});
    f.pcm = threeStretches();
    TranscriptionService service(f.config);
    TranscriptionRequest request = f.request();
    request.options.mixedLanguages = true;
    std::vector<std::string> partialLanguages;
    request.onSegment = [&](const Transcript& sofar) { partialLanguages.push_back(sofar.segments.back().language); };
    auto t = service.run(request);
    REQUIRE(t);
    REQUIRE(t->validate());

    // Three chunks - one per stretch - each named by the helper: English, Thai, English.
    REQUIRE(t->segments.size() == 3);
    CHECK(t->segments[0].language == "en");
    CHECK(t->segments[1].language == "th");
    CHECK(t->segments[2].language == "en");
    CHECK(partialLanguages == std::vector<std::string>{"en", "th", "en"}); // named as they stream in
    CHECK(wordTexts(*t).front() == "w1");
    // The Thai stretch's words are Thai units (split by the Thai rule), the English ones are the fake's "w<second>".
    for (std::size_t i = t->segments[1].firstWord; i < t->segments[1].firstWord + t->segments[1].wordCount; ++i) {
        INFO(t->words[i].text);
        CHECK(static_cast<unsigned char>(t->words[i].text.front()) >= 0x80);
    }
    CHECK(t->words[t->segments[2].firstWord].text == "w22");
    CHECK(t->words[t->segments[1].firstWord].t0 >= ms(11900));
    // The transcript's own language is the one most words are in; the shares add up to every word.
    const std::vector<LanguageShare> shares = languageShares(*t);
    REQUIRE(shares.size() == 2);
    CHECK(t->language == shares.front().language);
    CHECK(shares[0].words + shares[1].words == t->words.size());
    CHECK(t->wordLanguage(0) == "en");
    CHECK(t->wordLanguage(t->segments[1].firstWord) == "th");
    CHECK(t->provenance.params.at("mixed") == "languages");

    // The JSON keeps each segment's language; it is read back the same.
    auto again = transcriptFromJson(toJson(*t));
    REQUIRE(again);
    CHECK(*again == *t);
}

// Review: a segment that arrives without a language used to be labelled with the language of the segment decoded before it
// (the Thai stretch below became English). It is the language its own chunk was heard in.
TEST_CASE("Mixed languages: a segment that arrives without a language is not labelled with the one before it",
          "[ai][service][mixed]") {
    Fixture f("service-mixed-unnamed", {"--alternate-languages", "--unnamed-thai"});
    f.pcm = threeStretches();
    TranscriptionService service(f.config);
    TranscriptionRequest request = f.request();
    request.options.mixedLanguages = true;
    auto t = service.run(request);
    REQUIRE(t);
    REQUIRE(t->validate());
    REQUIRE(t->segments.size() == 3);
    CHECK(t->segments[0].language == "en");
    CHECK(t->segments[1].language == "th"); // the helper named the segment in no message but the chunk's result
    CHECK(t->segments[2].language == "en");
    CHECK(t->wordLanguage(t->segments[1].firstWord) == "th");
}

TEST_CASE("Mixed languages off: one language for the whole file as before and a cache entry of its own", "[ai][service][mixed]") {
    Fixture f("service-mixed-off", {"--alternate-languages"});
    f.pcm = threeStretches();
    TranscriptionService service(f.config);

    // Without the switch the stretches share one chunk, heard as one language, and no segment names its own.
    TranscriptionRequest plain = f.request();
    auto single = service.run(plain);
    REQUIRE(single);
    REQUIRE(single->segments.size() == 3);
    CHECK(single->language == "en");
    for (const TranscriptSegment& s : single->segments) {
        CHECK(s.language.empty());
    }
    CHECK_FALSE(single->provenance.params.contains("mixed"));
    CHECK(languageShares(*single).size() == 1);

    // A named language (or translating) leaves the switch without effect.
    TranscriptionRequest named = f.request();
    named.options.language = "th";
    named.options.mixedLanguages = true;
    auto thai = service.run(named);
    REQUIRE(thai);
    CHECK(thai->language == "th");
    CHECK_FALSE(thai->provenance.params.contains("mixed"));
    for (const TranscriptSegment& s : thai->segments) {
        CHECK(s.language.empty());
    }

    // Mixed languages is another entry in the analysis cache: not answered by the one above, then cached itself.
    TranscriptionRequest mixed = f.request();
    mixed.options.mixedLanguages = true;
    TranscriptionStats stats;
    mixed.onStats = [&](const TranscriptionStats& s) { stats = s; };
    auto first = service.run(mixed);
    REQUIRE(first);
    CHECK_FALSE(stats.fromCache);
    CHECK(first->segments[1].language == "th");
    auto second = service.run(mixed);
    REQUIRE(second);
    CHECK(stats.fromCache);
    CHECK(*second == *first);
    auto plainAgain = service.run(plain);
    REQUIRE(plainAgain);
    CHECK(plainAgain->segments[1].language.empty()); // the single-language entry is still its own
}

TEST_CASE("Mixed languages needs a model that knows more than English", "[ai][service][mixed]") {
    Fixture f("service-mixed-en", {"--alternate-languages"});
    f.config.models.push_back(f.installed("fake-whisper.en", ModelKind::Whisper));
    TranscriptionService service(f.config);
    TranscriptionRequest request = f.request();
    request.options.model = "fake-whisper.en";
    request.options.mixedLanguages = true;
    auto t = service.run(request);
    REQUIRE_FALSE(t);
    CHECK(t.error().code == ErrorCode::InvalidArgument);
    CHECK(t.error().message.find("tell languages apart") != std::string::npos);
    // Without the switch the English-only model transcribes as it always did.
    request.options.mixedLanguages = false;
    CHECK(service.run(request));
}
