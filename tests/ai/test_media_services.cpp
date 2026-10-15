// The media-backed transcript services (full builds): the audio-energy probes on a generated WAV, transcripts found
// in the analysis cache by source fingerprint, transcript.transcribe over ai::TranscribeService, and the wabcut-cli
// transcript subcommands (transcribe, cmd, transcript-edit, captions).

#include "AiTestUtil.h"
#include "CommandHarness.h"
#include "EditTestUtil.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/ChildProcess.h>
#include <wabcut/ai/TranscriptJson.h>
#include <wabcut/ai/edit/MediaServices.h>
#include <wabcut/ai/edit/Review.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/media/Fingerprint.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <numbers>
#include <tuple>

using namespace wabcut;
using namespace wabcut::ai::edit;
using namespace wabcut::ai::edit::test;
using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;

namespace {

/// A 48 kHz mono 16-bit WAV: a 440 Hz tone of amplitude 0.5 (RMS -9.03 dB), except silence (exact zeros) in
/// [silentFrom, silentTo) seconds.
void writeWav(const std::filesystem::path& file, double seconds, double silentFrom, double silentTo) {
    constexpr int kWavRate = 48000;
    const auto samples = static_cast<std::uint32_t>(seconds * kWavRate);
    std::ofstream out(file, std::ios::binary);
    const auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4);
    u32(36 + samples * 2);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);         // PCM
    u16(1);         // mono
    u32(kWavRate);
    u32(kWavRate * 2); // bytes per second
    u16(2);
    u16(16);
    out.write("data", 4);
    u32(samples * 2);
    const double amplitude = 0.5 * 32767.0;
    for (std::uint32_t n = 0; n < samples; ++n) {
        const double t = static_cast<double>(n) / kWavRate;
        const bool silent = t >= silentFrom && t < silentTo;
        const double v = silent ? 0.0 : amplitude * std::sin(2.0 * std::numbers::pi * 440.0 * t);
        u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(v))));
    }
}

MediaItem wavMedia(const std::filesystem::path& file) {
    MediaItem m;
    m.id = MediaId{7};
    m.name = "tone.wav";
    m.locator.path = pathToUtf8(file);
    AudioStreamInfo audio;
    audio.index = 0;
    audio.channels = 1;
    m.info.audio.push_back(audio);
    return m;
}

std::string run(const std::vector<std::string>& args, int& exitCode) {
    ai::ChildProcessOptions options;
    options.executable = WABCUT_CLI_PATH;
    options.arguments = args;
    auto child = ai::ChildProcess::spawn(options);
    REQUIRE(child);
    std::string out;
    std::string line;
    while ((*child)->readLine(line, 30s) == ai::ChildProcess::ReadStatus::Line) {
        out += line + "\n";
    }
    const auto code = (*child)->wait(30s);
    REQUIRE(code);
    exitCode = *code;
    return out;
}

} // namespace

TEST_CASE("DecoderEnergyProbe measures RMS through the media decoder", "[ai][edit][media][silence]") {
    const auto dir = ai::test::freshDir("energy");
    const auto wav = dir / "tone.wav";
    writeWav(wav, 3.0, 1.0, 2.0);
    const MediaItem media = wavMedia(wav);
    const DecoderEnergyProbe probe(dir / "project.wabcut");

    const auto tone = probe.rmsDb(media, 0, TimeRange::fromStartEnd(Ticks::fromSeconds(0), Ticks{kTicksPerSecond / 2}));
    REQUIRE(tone.has_value());
    CHECK_THAT(*tone, WithinAbs(20.0 * std::log10(0.5 / std::numbers::sqrt2), 0.2));
    const auto silence = probe.rmsDb(media, 0, TimeRange::fromStartEnd(Ticks{kTicksPerSecond * 11 / 10}, Ticks{kTicksPerSecond * 19 / 10}));
    REQUIRE(silence.has_value());
    CHECK(*silence < -90.0);
    const auto past = probe.rmsDb(media, 0, TimeRange::fromStartEnd(Ticks::fromSeconds(5), Ticks::fromSeconds(6)));
    CHECK((!past || *past < -90.0)); // past the end: silence or nothing

    MediaItem missing = media;
    missing.locator.path = pathToUtf8(dir / "nope.wav");
    CHECK_FALSE(probe.rmsDb(missing, 0, TimeRange::fromStartEnd(Ticks{}, Ticks::fromSeconds(1))).has_value());
}

TEST_CASE("CachedTranscriptProvider finds what transcription stored for the media", "[ai][edit][media]") {
    const auto dir = ai::test::freshDir("cached-transcripts");
    const auto wav = dir / "tone.wav";
    writeWav(wav, 1.0, 0.0, 0.0);
    const MediaItem media = wavMedia(wav);
    const auto fingerprint = media::sourceFingerprint(pathToUtf8(wav));
    REQUIRE(fingerprint);

    ai::Transcript t = *transcript("en", {{"hello", 0, 5}});
    t.source.fingerprint = *fingerprint;
    t.source.streamIndex = 0;
    ai::AnalysisCache cache(dir / "cache");
    REQUIRE(cache.put(ai::AnalysisKey{*fingerprint, 0, "transcript", "whisper/base@1", "0011223344556677"}, ai::toJson(t)));

    const CachedTranscriptProvider provider(dir / "cache", dir / "project.wabcut");
    const auto found = provider.transcript(media, 0);
    REQUIRE(found);
    CHECK(found->words.size() == 1);
    CHECK(found->words[0].text == "hello");
    CHECK(provider.transcript(media, 1) == nullptr);

    MediaItem unprobed = media; // no stream index recorded: the lowest stream with a transcript
    unprobed.id = MediaId{8};
    unprobed.info.audio.clear();
    CHECK(provider.transcript(unprobed, 0) != nullptr);

    const CachedTranscriptProvider empty(dir / "other-cache", dir / "project.wabcut");
    CHECK(empty.transcript(media, 0) == nullptr);
}

TEST_CASE("wabcut-cli transcript-edit and captions", "[ai][edit][cli]") {
    const auto dir = ai::test::freshDir("cli");
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )");
    tl.linkMatching();
    const auto project = dir / "interview.wabcut";
    REQUIRE(io::saveProject(tl.state, project));
    const auto transcriptFile = dir / "a.transcript.json";
    {
        std::ofstream out(transcriptFile, std::ios::binary);
        out << ai::toJson(*transcript("en", {{"So", 0, 3}, {"um,", 4, 7}, {"welcome.", 8, 22}, {"Thanks.", 62, 80}}));
    }
    const std::string media = std::to_string(tl.media('A').value) + "=" + transcriptFile.string();
    const std::string cache = (dir / "empty-cache").string();

    int code = -1;
    const std::string dry = run({"transcript-edit", project.string(), "--cache", cache, "--transcript", media,
                                 "--remove-fillers", "--remove-silences", "0.8", "--dry-run"},
                                code);
    INFO(dry);
    REQUIRE(code == 0);
    const auto result = nlohmann::json::parse(dry);
    CHECK(result["ok"] == true);
    CHECK(result["dryRun"] == true);
    CHECK(result["data"]["fillers"] == 1);
    CHECK(result["data"]["silences"] == 1);
    CHECK(result["data"]["cuts"].size() == 2);

    const std::string saved = run({"transcript-edit", project.string(), "--cache", cache, "--transcript", media,
                                   "--remove-fillers", "--save"},
                                  code);
    REQUIRE(code == 0);
    INFO(saved);
    CHECK(nlohmann::json::parse(saved)["saved"] == true);
    const auto reloaded = io::loadProject(project);
    REQUIRE(reloaded);
    INFO(render(*reloaded, tl.seq));
    CHECK(reloaded->findSequence(tl.seq)->contentEnd() == f(86 - 5));

    const auto srt = dir / "out.srt";
    const std::string captions = run({"captions", project.string(), "--cache", cache, "--transcript", media,
                                      "--sequence", std::to_string(tl.seq.value), "--srt", srt.string()},
                                     code);
    INFO(captions);
    REQUIRE(code == 0);
    CHECK(nlohmann::json::parse(captions)["cues"] == 2);
    std::ifstream in(srt, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    CHECK(text.str().starts_with("1\n00:00:00,000 --> 00:00:00,"));
    CHECK(text.str().find("So welcome.") != std::string::npos);

    CHECK(run({"transcript-edit", project.string()}, code).empty());
    CHECK(code == 2);
}

namespace {

/// Sets an environment variable for the test's lifetime (children inherit it).
class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::string& value) : m_name(name) { set(value); }
    ~ScopedEnv() { set(""); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    void set(const std::string& value) {
#ifdef _WIN32
        _putenv_s(m_name, value.c_str());
#else
        if (value.empty()) {
            unsetenv(m_name);
        } else {
            setenv(m_name, value.c_str(), 1);
        }
#endif
    }
    const char* m_name;
};

/// A transcriber added under "echo": the words "hello" and "world" at 0.2-0.6 s and 2.2-2.6 s of the source.
void addEcho(ai::TranscribeService& service) {
    service.addProvider("echo", "Echo", [](const ai::TranscribeSource&, const ai::TranscribeJobOptions&,
                                           const std::function<void(float)>& progress, std::stop_token)
                                           -> std::expected<ai::Transcript, Error> {
        progress(0.5f);
        ai::Transcript t;
        t.language = "en";
        for (const auto& [text, a, b] : {std::tuple{"hello", 0.2, 0.6}, std::tuple{"world", 2.2, 2.6}}) {
            ai::TranscriptWord w;
            w.text = text;
            w.t0 = Ticks{static_cast<std::int64_t>(a * static_cast<double>(kTicksPerSecond))};
            w.t1 = Ticks{static_cast<std::int64_t>(b * static_cast<double>(kTicksPerSecond))};
            w.confidence = 0.9f;
            t.words.push_back(w);
        }
        t.segments.push_back(ai::TranscriptSegment{t.words[0].t0, t.words[1].t1, 0, 2, "hello world", std::nullopt});
        return t;
    });
}

/// The picture's media 'A' pointed at `file` (one audio stream, container index 0).
void useFile(Timeline& tl, char letter, const std::filesystem::path& file) {
    for (MediaPtr& m : tl.state.media) {
        if (m->id == tl.media(letter)) {
            auto copy = std::make_shared<MediaItem>(*m);
            copy->locator.path = pathToUtf8(file);
            AudioStreamInfo audio;
            audio.index = 0;
            audio.channels = 1;
            audio.duration = Ticks::fromSeconds(3);
            copy->info.audio = {audio};
            m = std::move(copy);
        }
    }
}

} // namespace

TEST_CASE("transcript.transcribe over TranscribeService: the transcript commands see the new words", "[ai][edit][media][transcribe]") {
    const auto dir = ai::test::freshDir("transcribe-command");
    const auto wav = dir / "talk.wav";
    writeWav(wav, 3.0, 1.0, 2.0);
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )");
    useFile(tl, 'A', wav);
    const auto project = dir / "talk.wabcut";
    ai::TranscribeServiceConfig config;
    config.cacheRoot = dir / "cache";
    config.local.inferExecutable = dir / "not-built" / "wabcut-infer.exe";
    auto service = std::make_shared<ai::TranscribeService>(config);
    addEcho(*service);
    auto services = std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
        std::make_shared<TranscriptServices>(std::make_shared<CachedTranscriptProvider>(dir / "cache", project)),
        std::make_shared<TranscribeServices>(std::make_shared<ServiceMediaTranscriber>(service, project))});
    CommandHarness h(std::move(tl), command::EnginePolicy{command::CapabilitySet::all(), 1024, services}, editingCommands());

    CHECK(h.ok("transcript.text").data["words"] == 0); // nothing yet (and the miss is remembered...)
    const auto check = h.ok("transcript.transcribe", Json{{"check", true}});
    CHECK(check.data["items"][0]["hasTranscript"] == false);
    CHECK(check.data["providerError"].get<std::string>().find("WABCUT_WITH_LOCAL_AI") != std::string::npos);
    CHECK(h.errorOf("transcript.transcribe") == "NotFound"); // "auto": no local engine, no cloud here

    const auto done = h.ok("transcript.transcribe", Json{{"provider", "echo"}});
    CHECK(done.data["transcribed"] == 1);
    CHECK(done.data["items"][0]["words"] == 2);
    // ... until something is stored: the words are there now.
    const auto text = h.ok("transcript.text");
    CHECK(text.data["words"] == 2);
    REQUIRE(text.data["paragraphs"].size() == 2); // split at the 1.6 s pause
    CHECK(text.data["paragraphs"][0][5] == "hello");
    CHECK(text.data["paragraphs"][1][5] == "world");
    CHECK(h.ok("transcript.transcribe").data["cached"] == 1); // "auto" finds it without a provider
}

TEST_CASE("ServiceEnergyProbe: review's pauses measured in one pass, then from memory", "[ai][edit][media][silence]") {
    const auto dir = ai::test::freshDir("energy-probe");
    const auto wav = dir / "tone.wav";
    writeWav(wav, 3.0, 1.0, 2.0);
    const MediaItem media = wavMedia(wav);
    ai::AudioEnergyServiceConfig config;
    config.cacheRoot = dir / "cache";
    config.local.inferExecutable = dir / "not-built" / "wabcut-infer.exe";
    auto service = std::make_shared<ai::AudioEnergyService>(config);
    const ServiceEnergyProbe probe(service, dir / "p.wabcut");
    const TimeRange silent{Ticks::fromSeconds(1) + Ticks{kTicksPerSecond / 10}, Ticks{kTicksPerSecond * 8 / 10}};
    const TimeRange tone{Ticks{}, Ticks{kTicksPerSecond * 9 / 10}};
    const std::vector<AudioEnergyProbe::Range> ranges{{&media, 0, silent}, {&media, 0, tone}};
    probe.prefetch(ranges);
    const std::string path = *mediaFilePath(media, dir / "p.wabcut");
    REQUIRE(service->cached(path, 0, silent));
    REQUIRE(service->cached(path, 0, tone));
    CHECK(std::isinf(*probe.rmsDb(media, 0, silent)));
    CHECK_THAT(*probe.rmsDb(media, 0, tone), WithinAbs(-9.03, 0.05));
    // Not prefetched: measured on the spot.
    CHECK_THAT(*probe.rmsDb(media, 0, TimeRange{Ticks::fromSeconds(2), Ticks::fromSeconds(1)}), WithinAbs(-9.03, 0.05));
    MediaItem missing = media;
    missing.locator.path = pathToUtf8(dir / "nope.wav");
    CHECK_FALSE(probe.rmsDb(missing, 0, tone));
}

TEST_CASE("wabcut-cli transcribe reads the analysis cache; cmd serves transcript.transcribe", "[ai][edit][cli][transcribe]") {
    const auto dir = ai::test::freshDir("cli-transcribe");
    const ScopedEnv cacheDir("WABCUT_CACHE_DIR", pathToUtf8(dir / "user-cache"));
    const auto wav = dir / "talk.wav";
    writeWav(wav, 3.0, 1.0, 2.0);
    {
        // What the app (or a cloud provider) stored earlier, in the user's analysis cache.
        ai::TranscribeServiceConfig config; // cacheRoot: AnalysisCache::defaultRoot(), under WABCUT_CACHE_DIR now
        config.local.inferExecutable = dir / "not-built" / "wabcut-infer.exe";
        ai::TranscribeService seed(config);
        addEcho(seed);
        ai::TranscribeJobOptions options;
        options.provider = "echo";
        REQUIRE(seed.run(ai::TranscribeSource{pathToUtf8(wav), -1}, options));
    }
    int code = -1;
    const std::string json = run({"transcribe", wav.string(), "--json"}, code);
    INFO(json);
    REQUIRE(code == 0);
    const auto transcript = ai::transcriptFromJson(json);
    REQUIRE(transcript);
    REQUIRE(transcript->words.size() == 2);
    CHECK(transcript->words[1].text == "world");
    CHECK(transcript->provenance.params.at("provider") == "echo");

    const std::string text = run({"transcribe", wav.string()}, code);
    CHECK(code == 0);
    CHECK(text.find("hello world") != std::string::npos);
    const std::string providers = run({"transcribe", "--providers"}, code);
    CHECK(code == 0);
    CHECK(providers.find("local") != std::string::npos);
#ifndef WABCUT_REAL_INFER // (a local-AI build would transcribe for real: models download on first use)
    CHECK(run({"transcribe", wav.string(), "--force", "--provider", "local"}, code).empty());
    CHECK(code == 2); // no local engine in this build
#endif

    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )");
    useFile(tl, 'A', wav);
    const auto project = dir / "talk.wabcut";
    REQUIRE(io::saveProject(tl.state, project));
    const std::string cmd =
        run({"cmd", project.string(), "--json", R"({"command":"transcript.transcribe","params":{"check":true}})"}, code);
    INFO(cmd);
    REQUIRE(code == 0);
    const auto checked = nlohmann::json::parse(cmd);
    CHECK(checked["data"]["items"][0]["hasTranscript"] == true);
    const std::string cached = run({"cmd", project.string(), "--json", R"({"command":"transcript.transcribe"})"}, code);
    INFO(cached);
    REQUIRE(code == 0);
    CHECK(nlohmann::json::parse(cached)["data"]["cached"] == 1);
    const std::string words = run({"cmd", project.string(), "--json", R"({"command":"transcript.text"})"}, code);
    REQUIRE(code == 0);
    CHECK(nlohmann::json::parse(words)["data"]["words"] == 2);
}

TEST_CASE("wabcut-cli transcribe takes any file name", "[ai][edit][cli][transcribe]") {
    // Review P2-14: before the fix the ANSI argv was taken for UTF-8 and the CLI aborted on a name like this one.
    const auto dir = ai::test::freshDir("cli-unicode");
    const ScopedEnv cacheDir("WABCUT_CACHE_DIR", pathToUtf8(dir / "user-cache"));
    const auto wav = dir / std::filesystem::path(u8"interview-\u00E9t\u00E9-\U0001F600.wav");
    writeWav(wav, 3.0, 1.0, 2.0);
    {
        ai::TranscribeServiceConfig config;
        config.local.inferExecutable = dir / "not-built" / "wabcut-infer.exe";
        ai::TranscribeService seed(config);
        addEcho(seed);
        ai::TranscribeJobOptions options;
        options.provider = "echo";
        REQUIRE(seed.run(ai::TranscribeSource{pathToUtf8(wav), -1}, options));
    }
    int code = -1;
    const std::string text = run({"transcribe", pathToUtf8(wav)}, code);
    INFO(text);
    CHECK(code == 0);
    CHECK(text.find("hello world") != std::string::npos);
}
