// The real wabcut-infer (WABCUT_WITH_LOCAL_AI builds). Never downloads: the transcription test runs only when the
// tiny model is already in the user model store (wabcut-cli transcribe <file> --model tiny puts it there), and
// skips with the reason otherwise.

#include "AiTestUtil.h"

#include <wabcut/ai/ChildProcess.h>
#include <wabcut/ai/InferProtocol.h>
#include <wabcut/ai/TranscriptionService.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <fstream>

using namespace wabcut;
using namespace wabcut::ai;
using namespace std::chrono_literals;

#ifdef WABCUT_REAL_INFER

namespace {

/// A 16-bit PCM WAV file: 48 kHz stereo, a 440 Hz tone between `toneFrom` and `toneTo` seconds.
void writeWav(const std::filesystem::path& path, double seconds, double toneFrom, double toneTo) {
    const int rate = 48000;
    const int channels = 2;
    const auto frames = static_cast<std::uint32_t>(seconds * rate);
    std::ofstream out(path, std::ios::binary);
    auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4);
    u32(36 + frames * channels * 2);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(channels);
    u32(rate);
    u32(rate * channels * 2);
    u16(channels * 2);
    u16(16);
    out.write("data", 4);
    u32(frames * channels * 2);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate;
        const double v = (t >= toneFrom && t < toneTo) ? 0.4 * std::sin(2.0 * 3.14159265358979 * 440.0 * t) : 0.0;
        const auto s = static_cast<std::int16_t>(v * 32767);
        for (int c = 0; c < channels; ++c) {
            u16(static_cast<std::uint16_t>(s));
        }
    }
}

} // namespace

TEST_CASE("The real wabcut-infer starts, speaks the protocol and finds a CPU backend", "[ai][real]") {
    ChildProcessOptions options;
    options.executable = WABCUT_REAL_INFER;
    auto child = ChildProcess::spawn(options);
    REQUIRE(child);
    REQUIRE((*child)->writeLine(infer::encode(infer::Request{infer::Hello{}})));
    std::string line;
    REQUIRE((*child)->readLine(line, 30s) == ChildProcess::ReadStatus::Line);
    auto hello = infer::decodeResponse(line);
    REQUIRE(hello);
    const auto& reply = std::get<infer::HelloReply>(*hello);
    CHECK(reply.protocol == infer::kProtocolVersion);
    CHECK(reply.engine == "whisper.cpp");
    CHECK(reply.engineVersion.starts_with("1.9"));
    bool cpu = false;
    for (const infer::DeviceInfo& d : reply.devices) {
        cpu = cpu || d.kind == "cpu";
        UNSCOPED_INFO("device " << d.kind << " " << d.name << " (" << d.description << ")");
    }
    CHECK(cpu);
    // A request without a model is refused, not crashed on.
    REQUIRE((*child)->writeLine(infer::encode(infer::Request{infer::TranscribeRequest{"1", {"missing.f32", 0, 0}}})));
    REQUIRE((*child)->readLine(line, 30s) == ChildProcess::ReadStatus::Line);
    auto refused = infer::decodeResponse(line);
    REQUIRE(refused);
    CHECK(std::holds_alternative<infer::Failure>(*refused));
    (*child)->closeInput();
    CHECK((*child)->wait(30s) == 0);
}

TEST_CASE("Real tiny-model transcription of decoded media (skipped unless the model is present)", "[ai][real]") {
    const ModelInfo* tiny = findModel("tiny");
    REQUIRE(tiny != nullptr);
    ModelStore store(ModelStore::defaultRoot());
    if (!store.isInstalled(*tiny)) {
        SKIP("the tiny whisper model is not in " << store.root().string()
                                                 << " (tests never download; run `wabcut-cli transcribe <media> --model tiny` once)");
    }
    const auto dir = test::freshDir("real-tiny");
    const auto wav = dir / "tone.wav";
    writeWav(wav, 8.0, 2.0, 5.0);
    TranscriptionServiceConfig config;
    config.inferExecutable = WABCUT_REAL_INFER;
    config.workDir = dir / "work";
    config.helperLog = dir / "helper.log";
    TranscriptionService service(config); // real media extraction (AudioDecoder) and the real helper
    TranscriptionRequest request;
    request.mediaPath = wav.string();
    request.options.model = "tiny";
    request.options.language = "en";
    request.options.vad = store.isInstalled(*findModel("silero"));
    request.options.useGpu = false;
    TranscriptionStats stats;
    request.onStats = [&](const TranscriptionStats& s) { stats = s; };
    auto t = service.run(request);
    REQUIRE(t);
    CHECK(t->validate());
    CHECK(t->provenance.engine == "whisper.cpp");
    CHECK(t->provenance.modelId == "tiny");
    CHECK(stats.mediaSeconds > 7.9);
    CHECK(stats.mediaSeconds < 8.1);
    for (const TranscriptWord& w : t->words) { // whatever whisper hears in a tone, it is inside the audio
        CHECK(w.t0 >= Ticks{});
        CHECK(w.t1 <= Ticks::fromSeconds(8));
    }
}

#else

TEST_CASE("Real wabcut-infer tests need a WABCUT_WITH_LOCAL_AI build", "[ai][real]") {
    SKIP("built without WABCUT_WITH_LOCAL_AI: there is no wabcut-infer to run");
}

#endif
