// A.4: the Enhance Speech and stem separation jobs (wabcut/ai/audio/AudioJobs.h) on real decoding (media::AudioDecoder
// over float WAV fixtures) with the scripted helper: the take is as long as the stream at 48 kHz and equals the model
// run over the whole take (chunk seams without clicks), a 44.1 kHz source is resampled like playback does, stems sum
// to the mix, a finished take is reused without a model, and stem separation refuses on a computer without the memory.

#include "../AiTestUtil.h"

#include <wabcut/ai/audio/AudioJobs.h>
#include <wabcut/ai/audio/WavFile.h>
#include <wabcut/media/AudioDecoder.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

using namespace wabcut;
using namespace wabcut::ai::audio;

namespace {

AudioHelperOptions fakeHelper(const std::string& mode = "normal") {
    AudioHelperOptions o;
    o.executable = WABCUT_FAKE_EMBED;
    o.arguments = {"--mode", mode};
    o.speechModel = "deepfilternet3";
    o.stemModel = "htdemucs.onnx";
    o.workDir = ai::test::freshDir("audio-jobs-blocks");
    o.startTimeout = std::chrono::seconds(10);
    o.loadTimeout = std::chrono::seconds(10);
    o.requestTimeout = std::chrono::seconds(20);
    return o;
}

std::vector<float> program(std::int64_t frames, int channels, int rate) {
    std::vector<float> out(static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels));
    for (std::int64_t f = 0; f < frames; ++f) {
        const double t = static_cast<double>(f) / rate;
        for (int c = 0; c < channels; ++c) {
            out[static_cast<std::size_t>(f * channels + c)] =
                static_cast<float>(0.4 * std::sin(2.0 * 3.14159265358979 * (220.0 + 110.0 * c) * t) * (0.6 + 0.4 * std::sin(t * 3.0)));
        }
    }
    return out;
}

std::filesystem::path writeWav(const std::filesystem::path& file, const std::vector<float>& samples, int channels, int rate) {
    auto w = WavWriter::create(file, channels, rate);
    REQUIRE(w);
    REQUIRE(w->write(samples));
    REQUIRE(w->close());
    return file;
}

std::vector<float> lowpass(const std::vector<float>& x, int channels) {
    std::vector<float> y(x.size());
    std::vector<float> s(static_cast<std::size_t>(channels), 0.0f);
    for (std::size_t i = 0; i < x.size(); ++i) {
        float& v = s[i % static_cast<std::size_t>(channels)];
        v += 0.002f * (x[i] - v);
        y[i] = v;
    }
    return y;
}

/// What AudioDecoder makes of a file at `rate` (the source as playback and the jobs read it).
std::vector<float> decodeAll(const std::filesystem::path& file, int channels, int rate, std::int64_t frames) {
    auto d = media::AudioDecoder::open(pathToUtf8(file), -1, Rate{rate, 1}, channels);
    REQUIRE(d);
    std::vector<float> out(static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels));
    REQUIRE((*d)->read(Ticks{}, out));
    return out;
}

ChunkOptions smallChunks() {
    ChunkOptions o;
    o.body = 24000; // half a second: a three-second take is six requests
    o.preroll = 6000;
    o.postroll = 960;
    o.crossfade = 480;
    return o;
}

} // namespace

TEST_CASE("Enhance Speech job: the take is the model over the whole stream, chunk seams included, sample-aligned",
          "[ai][audio][jobs]") {
    const auto dir = ai::test::freshDir("audio-jobs-enhance");
    const int channels = 2;
    const std::int64_t frames = 3 * 48000 + 77;
    const auto source = writeWav(dir / "dialogue.wav", program(frames, channels, 48000), channels, 48000);
    AudioHelper helper(fakeHelper());
    const auto take = dir / "take.wav";
    std::vector<double> progress;
    auto rendered = renderEnhancedTake(pathToUtf8(source), -1, channels, samplesToTicks(frames, Rate{48000, 1}), take, helper,
                                       smallChunks(), {}, [&](double p) { progress.push_back(p); });
    REQUIRE(rendered);
    CHECK(*rendered == frames);
    WavInfo info;
    auto samples = readWavSamples(take, &info);
    REQUIRE(samples);
    CHECK(info.rate == 48000);
    CHECK(info.channels == channels);
    CHECK(info.frames == frames);
    const std::vector<float> expected = lowpass(decodeAll(source, channels, 48000, frames), channels);
    float worst = 0.0f;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        worst = std::max(worst, std::fabs((*samples)[i] - expected[i]));
    }
    CHECK(worst < 1e-4f);
    REQUIRE(progress.size() >= 6);
    CHECK(progress.back() == 1.0);
    CHECK(std::is_sorted(progress.begin(), progress.end()));
    CHECK_FALSE(std::filesystem::exists(dir / "take.wav.part"));
}

TEST_CASE("Enhance Speech job: a 44.1 kHz source becomes a 48 kHz take of the same length; a failure leaves no file",
          "[ai][audio][jobs]") {
    const auto dir = ai::test::freshDir("audio-jobs-441");
    const std::int64_t frames = 44100 * 2;
    const auto source = writeWav(dir / "phone.wav", program(frames, 1, 44100), 1, 44100);
    const Ticks duration = samplesToTicks(frames, Rate{44100, 1});
    AudioHelper helper(fakeHelper());
    auto rendered = renderEnhancedTake(pathToUtf8(source), -1, 1, duration, dir / "take.wav", helper, smallChunks(), {});
    REQUIRE(rendered);
    CHECK(*rendered == 96000);
    const auto samples = readWavSamples(dir / "take.wav");
    REQUIRE(samples);
    const std::vector<float> expected = lowpass(decodeAll(source, 1, 48000, 96000), 1);
    float worst = 0.0f;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        worst = std::max(worst, std::fabs((*samples)[i] - expected[i]));
    }
    CHECK(worst < 1e-4f);

    AudioHelper broken(fakeHelper("crash-on-embed"));
    auto failed = renderEnhancedTake(pathToUtf8(source), -1, 1, duration, dir / "broken.wav", broken, smallChunks(), {});
    REQUIRE_FALSE(failed);
    CHECK_FALSE(std::filesystem::exists(dir / "broken.wav"));
    CHECK_FALSE(std::filesystem::exists(dir / "broken.wav.part"));
    // Held by the resource governor until it gives up: cancelled before any request.
    auto held = renderEnhancedTake(pathToUtf8(source), -1, 1, duration, dir / "held.wav", helper, smallChunks(), {}, {},
                                   [](std::stop_token) { return false; });
    REQUIRE_FALSE(held);
    CHECK(held.error().code == ErrorCode::Cancelled);
}

TEST_CASE("Stem separation job: overlapping segments put back into stems that sum to the mix", "[ai][audio][jobs]") {
    const auto dir = ai::test::freshDir("audio-jobs-stems");
    const std::int64_t frames = 44100 * 3 + 5;
    const auto source = writeWav(dir / "song.wav", program(frames, 2, 44100), 2, 44100);
    StemModelSpec spec;
    spec.segment = 30000;
    spec.overlap = 7500;
    AudioHelper helper(fakeHelper());
    auto files = separateStemsOf(pathToUtf8(source), -1, samplesToTicks(frames, Rate{44100, 1}), dir / "stems", helper, spec, {});
    REQUIRE(files);
    REQUIRE(files->size() == 4);
    const std::vector<float> mix = decodeAll(source, 2, 44100, frames);
    std::vector<std::vector<float>> stems;
    for (const auto& f : *files) {
        WavInfo info;
        auto s = readWavSamples(f, &info);
        REQUIRE(s);
        CHECK(info.frames == frames);
        CHECK(info.channels == 2);
        stems.push_back(std::move(*s));
    }
    CHECK(files->back().filename() == "vocals.wav");
    float worst = 0.0f;
    for (std::size_t i = 0; i < mix.size(); ++i) {
        worst = std::max(worst, std::fabs(stems[0][i] + stems[1][i] + stems[2][i] + stems[3][i] - mix[i]));
        worst = std::max(worst, std::fabs(stems[3][i] - 0.4f * mix[i]));
    }
    CHECK(worst < 1e-5f);
}

TEST_CASE("The local Enhance Speech reuses a finished take; stem separation refuses without the memory",
          "[ai][audio][jobs]") {
    const auto dir = ai::test::freshDir("audio-jobs-local");
    const std::int64_t frames = 48000;
    const auto source = writeWav(dir / "a.wav", program(frames, 1, 48000), 1, 48000);
    MediaItem item;
    item.name = "Interview";
    item.locator.path = pathToUtf8(source);
    item.locator.fingerprint = *fingerprintFile(source);
    AudioStreamInfo stream;
    stream.index = 0;
    stream.channels = 1;
    stream.duration = samplesToTicks(frames, Rate{48000, 1});
    item.info.audio.push_back(stream);
    item.info.duration = stream.duration;

    LocalAudioOptions options;
    options.modelRoot = dir / "models"; // empty: any attempt to use a model would fail with NotFound
    options.takesDirectory = dir / "takes";
    options.stemsDirectory = dir / "stems";
    options.helper = fakeHelper();
    auto enhancer = makeSpeechEnhancer(options);
    CHECK(enhancer->modelVersion() == speechModelVersion());
    auto missing = enhancer->render(item, 0, {}, {});
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::NotFound); // DeepFilterNet3 is not downloaded and there is no transport

    // A finished take of this file and model: found, nothing rendered.
    const auto take = takeFileFor(options.takesDirectory, item.locator.fingerprint, 0, speechModelVersion());
    std::filesystem::create_directories(take.parent_path());
    writeWav(take, std::vector<float>(static_cast<std::size_t>(frames), 0.1f), 1, 48000);
    auto reused = enhancer->render(item, 0, {}, {});
    REQUIRE(reused);
    CHECK(reused->reused);
    CHECK(reused->path == pathToUtf8(take));
    CHECK(reused->source == item.locator.fingerprint);
    CHECK(reused->model == speechModelVersion());
    CHECK_FALSE(enhancer->render(item, 3, {}, {})); // no such stream

    options.probe = [] {
        ai::llm::SystemProbe p;
        p.ramTotal = std::uint64_t{4} << 30;
        p.ramAvailable = std::uint64_t{1} << 30;
        return p;
    };
    auto separator = makeStemSeparator(options);
    auto refused = separator->separate(item, 0, {}, {});
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == ErrorCode::Unsupported);
    CHECK(refused.error().message.find("free memory") != std::string::npos);
    // Enough memory: the CPU (DirectML cannot hold the export's inverse STFT), the GPU only when asked for.
    ai::llm::SystemProbe roomy;
    roomy.ramAvailable = std::uint64_t{16} << 30;
    roomy.gpus.push_back({"RTX 3060", ai::llm::GpuVendor::Nvidia, std::uint64_t{12} << 30, std::uint64_t{9} << 30, false});
    CHECK(*stemDevice(roomy) == "cpu");
    CHECK(*stemDevice(roomy, "gpu") == "gpu");
}

// Review wave 5, A5-P2-16. LocalStemSeparator shut its helper down only after a separation that succeeded: a failed one,
// or one cancelled between two requests (here the resource governor gives up before the second segment), left
// wabcut-embed running with the stem model loaded (about 2.5 GiB) until the next job. Fails today: the fake helper has
// not exited when separate() returns, so it wrote no "exit" line.
TEST_CASE("A stem separation cancelled at its second segment shuts its helper down", "[ai][audio][jobs]") {
    const auto dir = ai::test::freshDir("audio-jobs-stems-cancel");
    const StemModelSpec& spec = stemModelSpec();
    const std::int64_t frames = static_cast<std::int64_t>(spec.segment) * 2; // two segments or more
    const auto source = writeWav(dir / "song.wav", program(frames, 2, spec.sampleRate), 2, spec.sampleRate);
    MediaItem item;
    item.name = "Song";
    item.locator.path = pathToUtf8(source);
    item.locator.fingerprint = *fingerprintFile(source);
    AudioStreamInfo stream;
    stream.index = 0;
    stream.channels = 2;
    stream.duration = samplesToTicks(frames, Rate{spec.sampleRate, 1});
    item.info.audio.push_back(stream);
    item.info.duration = stream.duration;

    // The stem model "installed" as the store checks it (its size and the verification marker); the fake helper only
    // takes its path.
    const ai::ModelInfo& model = stemModel();
    const std::filesystem::path models = dir / "models";
    std::filesystem::create_directories(models);
    const std::filesystem::path weights = models / model.fileName;
    std::ofstream(weights, std::ios::binary).close();
    std::filesystem::resize_file(weights, model.size);
    std::ofstream(models / (model.fileName + ".verified"), std::ios::binary) << model.sha256 << '\n';

    const std::filesystem::path exits = dir / "exits.txt";
    LocalAudioOptions options;
    options.modelRoot = models;
    options.stemsDirectory = dir / "stems";
    options.helper = fakeHelper();
    options.helper.arguments.push_back("--exits");
    options.helper.arguments.push_back(pathToUtf8(exits));
    options.probe = [] {
        ai::llm::SystemProbe p;
        p.ramAvailable = std::uint64_t{16} << 30;
        return p;
    };
    int waits = 0;
    options.waitIdle = [&waits](std::stop_token) { return ++waits < 2; }; // gives up before the second segment
    auto separator = makeStemSeparator(options);
    auto cancelled = separator->separate(item, 0, {}, {});
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == ErrorCode::Cancelled);
    CHECK(waits == 2);
    // The separator is still there; its helper is not running (it exits cleanly on the shutdown request).
    std::ostringstream text;
    text << std::ifstream(exits).rdbuf();
    CHECK(text.str() == "exit\n");

    separator.reset();
    std::error_code ec;
    std::filesystem::remove(weights, ec);
}
