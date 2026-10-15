// A.4: the real models in the real helper, only when they are on disk (never downloaded here):
//   WABCUT_TEST_AUDIO_MODELS   a folder with deepfilternet3/ (enc.onnx, erb_dec.onnx, df_dec.onnx) and/or
//                              htdemucs_fp16weights.onnx
//   WABCUT_TEST_SPEECH_WAV     a clean speech recording (16-bit or float WAV, any rate), for the SNR figures
//   WABCUT_TEST_AUDIO_SECONDS  how long the enhanced programme is (default 60)
//   WABCUT_TEST_DFN3_ARCHIVE   DeepFilterNet3_onnx.tar.gz as published: installSpeechModel downloads it from a local
//                              transport, extracts it with the system's tar and checks every graph
// Enhance Speech must improve the SNR and SI-SDR of the speech with noise mixed in at 5 dB; speed in times real time on
// the GPU (DirectML) and the CPU is printed. Stem separation runs a short mix on both and checks the stems.

#include "../AiTestUtil.h"

#include <wabcut/ai/audio/AudioJobs.h>
#include <wabcut/ai/audio/WavFile.h>
#include <wabcut/media/AudioDecoder.h>
#include <wabcut/media/Probe.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

using namespace wabcut;
using namespace wabcut::ai::audio;

namespace {

std::filesystem::path env(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && *v != '\0' ? std::filesystem::path(v) : std::filesystem::path{};
}

double snrDb(const std::vector<float>& clean, const std::vector<float>& test) {
    double s = 0.0;
    double e = 0.0;
    for (std::size_t i = 0; i < clean.size(); ++i) {
        s += static_cast<double>(clean[i]) * clean[i];
        e += (static_cast<double>(clean[i]) - test[i]) * (static_cast<double>(clean[i]) - test[i]);
    }
    return 10.0 * std::log10(s / std::max(e, 1e-20));
}

double siSdrDb(const std::vector<float>& clean, const std::vector<float>& test) {
    double dot = 0.0;
    double rr = 0.0;
    for (std::size_t i = 0; i < clean.size(); ++i) {
        dot += static_cast<double>(clean[i]) * test[i];
        rr += static_cast<double>(clean[i]) * clean[i];
    }
    const double a = dot / rr;
    double s = 0.0;
    double e = 0.0;
    for (std::size_t i = 0; i < clean.size(); ++i) {
        const double t = a * clean[i];
        s += t * t;
        e += (t - test[i]) * (t - test[i]);
    }
    return 10.0 * std::log10(s / std::max(e, 1e-20));
}

std::vector<float> decode(const std::filesystem::path& file, int channels, int rate, std::int64_t& frames) {
    auto info = media::probe(pathToUtf8(file));
    REQUIRE(info);
    REQUIRE_FALSE(info->audio.empty());
    const Ticks duration = info->audio.front().duration.value > 0 ? info->audio.front().duration : info->duration;
    frames = ticksToSamples(duration, Rate{rate, 1});
    auto d = media::AudioDecoder::open(pathToUtf8(file), -1, Rate{rate, 1}, channels);
    REQUIRE(d);
    std::vector<float> out(static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels));
    REQUIRE((*d)->read(Ticks{}, out));
    return out;
}

std::filesystem::path writeWav(const std::filesystem::path& file, const std::vector<float>& samples, int channels, int rate) {
    auto w = WavWriter::create(file, channels, rate);
    REQUIRE(w);
    REQUIRE(w->write(samples));
    REQUIRE(w->close());
    return file;
}

/// Serves one local file for any URL (the model store's download path without a network).
class FileTransport final : public ai::IDownloadTransport {
public:
    explicit FileTransport(std::filesystem::path file) : m_file(std::move(file)) {}
    std::expected<Response, Error> get(const std::string&, std::uint64_t offset, const std::function<bool(const Response&)>& onResponse,
                                       const std::function<bool(std::span<const std::byte>)>& onData, std::stop_token) override {
        std::ifstream in(m_file, std::ios::binary);
        const auto size = static_cast<std::uint64_t>(std::filesystem::file_size(m_file));
        Response r{offset > 0 ? 206 : 200, size};
        if (!onResponse(r)) {
            return std::unexpected(Error(ErrorCode::Cancelled, "cancelled"));
        }
        in.seekg(static_cast<std::streamoff>(offset));
        std::vector<char> buffer(1 << 16);
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto n = static_cast<std::size_t>(in.gcount());
            if (n > 0 && !onData(std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()), n))) {
                return std::unexpected(Error(ErrorCode::Cancelled, "cancelled"));
            }
        }
        return r;
    }

private:
    std::filesystem::path m_file;
};

} // namespace

TEST_CASE("DeepFilterNet3's archive installs: downloaded, extracted with tar, every graph verified (WABCUT_TEST_DFN3_ARCHIVE)",
          "[ai][audio][model]") {
    const auto archive = env("WABCUT_TEST_DFN3_ARCHIVE");
    if (archive.empty()) {
        SKIP("set WABCUT_TEST_DFN3_ARCHIVE to DeepFilterNet3_onnx.tar.gz (commit 84d57ec)");
    }
    ai::ModelStore store(ai::test::freshDir("audio-model-install"));
    FileTransport transport(archive);
    std::vector<double> progress;
    auto dir = installSpeechModel(store, &transport, defaultExtractor(), [&](const ai::DownloadProgress& p) {
        progress.push_back(static_cast<double>(p.received));
    });
    INFO((dir ? std::string() : dir.error().message));
    REQUIRE(dir);
    CHECK(*dir == speechModelDirectory(store));
    CHECK(speechModelInstalled(store));
    for (const ArchiveMember& m : speechModelMembers()) {
        CHECK(std::filesystem::file_size(*dir / std::string(m.name)) == m.size);
    }
    CHECK_FALSE(progress.empty());
    // A second call finds it installed.
    auto again = installSpeechModel(store, nullptr, defaultExtractor());
    REQUIRE(again);
    // Nothing is left behind but the archive, its marker and the graphs.
    std::size_t parts = 0;
    for (const auto& entry : std::filesystem::directory_iterator(store.root())) {
        parts += entry.path().extension() == ".part" ? 1 : 0;
    }
    CHECK(parts == 0);
}

TEST_CASE("Enhance Speech with DeepFilterNet3: SNR improvement and speed (WABCUT_TEST_AUDIO_MODELS)", "[ai][audio][model]") {
#ifndef WABCUT_EMBED_HELPER
    SKIP("this build has no wabcut-embed (WABCUT_WITH_ONNX off)");
#else
    const auto models = env("WABCUT_TEST_AUDIO_MODELS");
    const auto speech = env("WABCUT_TEST_SPEECH_WAV");
    if (models.empty() || !std::filesystem::exists(models / "deepfilternet3" / "enc.onnx") || speech.empty()) {
        SKIP("set WABCUT_TEST_AUDIO_MODELS (with deepfilternet3/) and WABCUT_TEST_SPEECH_WAV");
    }
    const auto dir = ai::test::freshDir("audio-model-enhance");
    std::int64_t frames = 0;
    std::vector<float> clean = decode(speech, 1, 48000, frames);
    // Repeat the speech to a minute (a realistic job: two chunks and a seam; WABCUT_TEST_AUDIO_SECONDS for longer), then
    // noise at 5 dB SNR.
    const char* secondsText = std::getenv("WABCUT_TEST_AUDIO_SECONDS");
    const std::size_t target = 48000u * static_cast<std::size_t>(secondsText != nullptr ? std::max(1, std::atoi(secondsText)) : 60);
    const std::vector<float> once = clean;
    while (clean.size() < target) {
        const std::size_t n = std::min<std::size_t>(once.size(), target - clean.size());
        clean.insert(clean.end(), once.begin(), once.begin() + static_cast<std::ptrdiff_t>(n));
    }
    std::mt19937 rng(7);
    std::normal_distribution<float> g(0.0f, 1.0f);
    std::vector<float> noise(clean.size());
    float lp = 0.0f;
    for (std::size_t i = 0; i < noise.size(); ++i) {
        const float w = g(rng);
        lp = 0.98f * lp + 0.02f * w;
        noise[i] = 0.6f * w + 4.0f * lp + 0.8f * std::sin(2.0f * 3.14159265f * 120.0f * static_cast<float>(i) / 48000.0f);
    }
    double ps = 0.0;
    double pn = 0.0;
    for (std::size_t i = 0; i < clean.size(); ++i) {
        ps += static_cast<double>(clean[i]) * clean[i];
        pn += static_cast<double>(noise[i]) * noise[i];
    }
    const float gain = static_cast<float>(std::sqrt(ps / (pn * std::pow(10.0, 0.5))));
    std::vector<float> noisy(clean.size());
    for (std::size_t i = 0; i < clean.size(); ++i) {
        noisy[i] = clean[i] + gain * noise[i];
    }
    const auto source = writeWav(dir / "noisy.wav", noisy, 1, 48000);
    const Ticks duration = samplesToTicks(static_cast<std::int64_t>(noisy.size()), Rate{48000, 1});
    const double seconds = static_cast<double>(noisy.size()) / 48000.0;
    for (const char* device : {"auto", "cpu"}) {
        AudioHelperOptions o;
        o.executable = WABCUT_EMBED_HELPER;
        o.speechModel = models / "deepfilternet3";
        o.device = device;
        o.workDir = dir / "blocks";
        AudioHelper helper(o);
        const auto take = dir / (std::string("take-") + device + ".wav");
        const auto started = std::chrono::steady_clock::now();
        auto rendered = renderEnhancedTake(pathToUtf8(source), -1, 1, duration, take, helper, ChunkOptions{}, {});
        REQUIRE(rendered);
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        auto enhanced = readWavSamples(take);
        REQUIRE(enhanced);
        REQUIRE(enhanced->size() == clean.size());
        const double before = snrDb(clean, noisy);
        const double after = snrDb(clean, *enhanced);
        const double siBefore = siSdrDb(clean, noisy);
        const double siAfter = siSdrDb(clean, *enhanced);
        std::printf("Enhance Speech on %s (%s): %.1f s of audio in %.2f s = %.1fx real time; SNR %.2f -> %.2f dB, SI-SDR %.2f -> "
                    "%.2f dB\n",
                    helper.device().c_str(), device, seconds, elapsed, seconds / elapsed, before, after, siBefore, siAfter);
        CHECK(after > before + 5.0);
        CHECK(siAfter > siBefore + 5.0);
    }
#endif
}

TEST_CASE("Stem separation with HTDemucs: four stems and speed (WABCUT_TEST_AUDIO_MODELS)", "[ai][audio][model]") {
#ifndef WABCUT_EMBED_HELPER
    SKIP("this build has no wabcut-embed (WABCUT_WITH_ONNX off)");
#else
    const auto models = env("WABCUT_TEST_AUDIO_MODELS");
    if (models.empty() || !std::filesystem::exists(models / "htdemucs_fp16weights.onnx")) {
        SKIP("set WABCUT_TEST_AUDIO_MODELS (with htdemucs_fp16weights.onnx)");
    }
    const auto dir = ai::test::freshDir("audio-model-stems");
    // Twenty seconds of a synthetic band: a bass line, a kick and hats, a voice-like vowel.
    const int rate = 44100;
    const std::size_t frames = static_cast<std::size_t>(rate) * 20;
    std::vector<float> mix(frames * 2);
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    for (std::size_t f = 0; f < frames; ++f) {
        const double t = static_cast<double>(f) / rate;
        const double beat = std::fmod(t, 0.5);
        const double bass = 0.25 * std::sin(2.0 * 3.14159265 * (55.0 + 27.5 * std::floor(std::fmod(t, 2.0))) * t);
        const double kick = beat < 0.08 ? 0.6 * std::sin(2.0 * 3.14159265 * (60.0 + 200.0 * (0.08 - beat)) * beat) * (1.0 - beat / 0.08) : 0.0;
        const double hat = std::fmod(t + 0.25, 0.5) < 0.02 ? 0.1 * u(rng) : 0.0;
        double vowel = 0.0;
        for (int h = 1; h <= 8; ++h) {
            vowel += 0.08 / h * std::sin(2.0 * 3.14159265 * 220.0 * h * t) * (h == 3 || h == 4 ? 2.0 : 1.0);
        }
        vowel *= std::fmod(t, 4.0) < 2.0 ? 1.0 : 0.0;
        const float v = static_cast<float>(bass + kick + hat + vowel);
        mix[f * 2] = v;
        mix[f * 2 + 1] = v;
    }
    const auto source = writeWav(dir / "band.wav", mix, 2, rate);
    const Ticks duration = samplesToTicks(static_cast<std::int64_t>(frames), Rate{rate, 1});
    for (const char* device : {"auto", "cpu"}) {
        AudioHelperOptions o;
        o.executable = WABCUT_EMBED_HELPER;
        o.stemModel = models / "htdemucs_fp16weights.onnx";
        o.device = device;
        o.workDir = dir / "blocks";
        AudioHelper helper(o);
        const auto started = std::chrono::steady_clock::now();
        auto files = separateStemsOf(pathToUtf8(source), -1, duration, dir / device, helper, stemModelSpec(), {});
        REQUIRE(files);
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        REQUIRE(files->size() == 4);
        double total = 0.0;
        for (const auto& f : *files) {
            WavInfo info;
            auto s = readWavSamples(f, &info);
            REQUIRE(s);
            CHECK(info.frames == static_cast<std::int64_t>(frames));
            double energy = 0.0;
            for (const float x : *s) {
                REQUIRE(std::isfinite(x));
                energy += static_cast<double>(x) * x;
            }
            total += energy;
            std::printf("  %s: %.1f dB\n", f.stem().string().c_str(), 10.0 * std::log10(std::max(energy / static_cast<double>(s->size()), 1e-20)));
        }
        CHECK(total > 0.0);
        std::printf("Stem separation on %s (%s): %.1f s of audio in %.2f s = %.2fx real time\n", helper.device().c_str(), device,
                    20.0, elapsed, 20.0 / elapsed);
    }
#endif
}
