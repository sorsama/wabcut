// AI-12 on real media code: the providers' speech decoded by FFmpeg, lines sped up by the audio engine's WSOLA
// stretcher with the pitch kept, the dub written as a float WAV that probes as an audio media item. And, only when
// asked (WABCUT_TEST_TRANSLATION_MODELS=1; downloads ~0.25 GB once into the model store), OPUS-MT through the real
// wabcut-embed helper: English to Thai and back, with the speed in cues a second on the CPU and on DirectML.

#include "GlobalTestUtil.h"

#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/audio/WavFile.h>
#include <wabcut/ai/dub/MediaSpeechAudio.h>
#include <wabcut/ai/translate/LocalTranslation.h>
#include <wabcut/model/MediaLocator.h>
#ifdef WABCUT_NET_DOWNLOAD
#include <wabcut/ai/HttpDownload.h>
#include <wabcut/ai_net/Network.h>

#include <QCoreApplication>
#endif

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>

using namespace wabcut;
using namespace wabcut::ai;

namespace {

constexpr int kRate = 48000;

std::vector<float> tone(double hz, double seconds, int rate) {
    std::vector<float> out(static_cast<std::size_t>(seconds * rate));
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = 0.4f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * hz * static_cast<double>(i) / rate));
    }
    return out;
}

/// Frequency from rising zero crossings over the middle half.
double frequency(const std::vector<float>& x, int rate) {
    const std::size_t from = x.size() / 4;
    const std::size_t to = x.size() * 3 / 4;
    std::size_t first = 0;
    std::size_t last = 0;
    int crossings = 0;
    for (std::size_t i = from + 1; i < to; ++i) {
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
            first = crossings == 0 ? i : first;
            last = i;
            ++crossings;
        }
    }
    return crossings > 1 ? static_cast<double>(crossings - 1) * rate / static_cast<double>(last - first) : 0.0;
}

std::filesystem::path scratch(std::string_view name) {
    const auto dir = std::filesystem::temp_directory_path() / ("wabcut-global-media-" + std::string(name));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::vector<std::uint8_t> readBytes(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string env(const char* name) {
    char* value = nullptr;
    std::size_t length = 0;
#ifdef _WIN32
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return {};
    }
    std::string s(value);
    std::free(value);
    return s;
#else
    (void)length;
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

} // namespace

TEST_CASE("Speech is decoded, sped up with its pitch kept, and written as an audio media item", "[ai][global][dub][media]") {
    const auto dir = scratch("speech");
    // A provider's WAV at 24 kHz (OpenAI and Gemini send WAV), read back at 48 kHz mono.
    const auto source = tone(220.0, 2.0, 24000);
    {
        auto writer = ai::audio::WavWriter::create(dir / "provider.wav", 1, 24000);
        REQUIRE(writer);
        REQUIRE(writer->write(source));
        REQUIRE(writer->close());
    }
    SpeechResult speech;
    speech.audio = readBytes(dir / "provider.wav");
    speech.mediaType = "audio/wav";
    auto audio = dub::makeMediaSpeechAudio(dir / "scratch");
    auto decoded = audio->decode(speech, kRate);
    REQUIRE(decoded);
    CHECK(static_cast<double>(decoded->size()) / kRate == Catch::Approx(2.0).margin(0.03));
    CHECK(frequency(*decoded, kRate) == Catch::Approx(220.0).margin(3.0));
    CHECK(std::filesystem::is_empty(dir / "scratch")); // the provider's file is gone

    // 15 % faster: 15 % shorter, the same pitch.
    const auto faster = audio->stretch(*decoded, kRate, 1.15);
    CHECK(static_cast<double>(faster.size()) == Catch::Approx(static_cast<double>(decoded->size()) / 1.15).margin(2.0));
    CHECK(frequency(faster, kRate) == Catch::Approx(220.0).margin(4.0));

    auto item = audio->write(dir / "dub-test.wav", faster, kRate);
    REQUIRE(item);
    REQUIRE(item->info.audio.size() == 1);
    CHECK(item->info.audio[0].channels == 1);
    CHECK(item->info.audio[0].sampleRate == Rate{kRate, 1});
    CHECK(static_cast<double>(item->info.duration.value) / kTicksPerSecond == Catch::Approx(2.0 / 1.15).margin(0.02));
    CHECK(item->locator.fingerprint.size > 0);
    CHECK_FALSE(std::filesystem::exists(dir / "dub-test.wav.part"));

    // A clone sample: the speaker's ranges joined.
    const TimeRange ranges[] = {{Ticks{}, Ticks{kTicksPerSecond / 2}}, {Ticks{kTicksPerSecond}, Ticks{kTicksPerSecond / 2}}};
    auto sample = audio->extractSample(*item, 0, ranges, dir / "sample.wav");
    REQUIRE(sample);
    CHECK(std::filesystem::file_size(*sample) > 44100 * 4); // > 1 s of 32-bit float at 44.1 kHz

    // Garbage is an error, not a crash.
    SpeechResult garbage;
    garbage.audio = {1, 2, 3, 4};
    garbage.mediaType = "audio/mpeg";
    CHECK_FALSE(audio->decode(garbage, kRate));
}

// Review wave 5, A5-P1-8. write() named its temporary file file.string() + ".part" and the item file.stem().string():
// MSVC's narrow string() throws std::system_error for a name outside the ANSI code page (emoji in every code page), so
// "Preview voice" ended the app when the cache sits under such a profile name. Fails today with that exception.
TEST_CASE("Speech is written as a media item in a folder named in Thai with an emoji", "[ai][global][dub][media]") {
    const std::filesystem::path dir = scratch("utf8") / u8"วิดีโอ 😀";
    std::filesystem::create_directories(dir);
    auto audio = dub::makeMediaSpeechAudio(dir);
    const auto samples = tone(220.0, 1.0, kRate);
    const std::filesystem::path file = dir / u8"voice-😀.wav";

    std::expected<MediaItem, Error> item = std::unexpected(Error{ErrorCode::NotFound, "not written"});
    REQUIRE_NOTHROW(item = audio->write(file, samples, kRate));
    REQUIRE(item);
    CHECK(std::filesystem::is_regular_file(file));
    CHECK(item->name == pathToUtf8(std::filesystem::path(u8"voice-😀")));
    CHECK(item->locator.path == pathToUtf8(file)); // strings: a failing path would be printed through string()
    REQUIRE(item->info.audio.size() == 1);
    CHECK(item->locator.fingerprint.size > 0);
    std::filesystem::path part = file;
    part += ".part";
    CHECK_FALSE(std::filesystem::exists(part));
    std::error_code ec;
    std::filesystem::remove_all(dir.parent_path(), ec);
}

TEST_CASE("OPUS-MT on the real helper: English to Thai and back, cues a second (WABCUT_TEST_TRANSLATION_MODELS)",
          "[ai][global][translate][real][benchmark]") {
#if !defined(WABCUT_EMBED_HELPER) || !defined(WABCUT_NET_DOWNLOAD)
    SKIP("no wabcut-embed or model downloads in this build");
#else
    if (env("WABCUT_TEST_TRANSLATION_MODELS") != "1") {
        SKIP("set WABCUT_TEST_TRANSLATION_MODELS=1 (downloads opus-mt-en-mul and opus-mt-th-en once)");
    }
    // The app's way: the AI network stack (Qt), pinned files checked by SHA-256.
    if (!QCoreApplication::instance()) {
        static int argc = 1;
        static char name[] = "wabcut_ai_global_media_tests";
        static char* argv[] = {name, nullptr};
        static QCoreApplication app(argc, argv);
    }
    RequestLog log;
    net::NetworkStack network(log);
    HttpDownloadTransport downloads(network.transport());
    ModelStore store(ModelStore::defaultRoot());
    for (const char* model : {"opus-mt-en-mul", "opus-mt-th-en"}) {
        INFO(model);
        auto ensured = translate::ensureLocalModel(store, model, &downloads, {}, {});
        INFO((ensured ? std::string() : ensured.error().message));
        REQUIRE(ensured);
    }
    const std::vector<std::string> english{
        "Welcome back to the channel.", "Today we are cooking green curry.", "First, heat the oil in a pan.",
        "Add the curry paste and stir for one minute.", "Pour in the coconut milk.", "Now add the chicken.",
        "Let it simmer for ten minutes.", "Taste it and add a little fish sauce.", "Serve it with jasmine rice.",
        "Thanks for watching, see you next time."};
    TranslationRequest request;
    request.sourceLanguage = "en";
    request.targetLanguage = "th";
    for (std::size_t i = 0; i < english.size(); ++i) {
        request.segments.push_back({std::to_string(i), english[i]});
    }
    for (const char* device : {"cpu", "auto"}) {
        translate::LocalTranslatorOptions options;
        options.executable = WABCUT_EMBED_HELPER;
        options.modelDirectory = translate::localModelDirectory(store, "opus-mt-en-mul");
        options.targetToken = ">>tha<<";
        options.device = device;
        options.log = std::filesystem::temp_directory_path() / (std::string("wabcut-embed-translate-") + device + ".log");
        translate::LocalTranslator translator(options);
        // Warm-up: the first request loads the model.
        TranslationRequest warm = request;
        warm.segments.resize(1);
        auto warmed = translator.translate(warm, {});
        INFO((warmed ? std::string() : warmed.error().message));
        REQUIRE(warmed);
        const auto started = std::chrono::steady_clock::now();
        auto result = translator.translate(request, {});
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        REQUIRE(result);
        REQUIRE(result->segments.size() == english.size());
        std::size_t thai = 0;
        for (const TranslationSegment& s : result->segments) {
            thai += s.text.find("\xE0\xB8") != std::string::npos ? 1 : 0; // Thai block U+0E00..U+0E3F
        }
        CHECK(thai >= english.size() * 8 / 10);
        std::printf("opus-mt-en-mul en->th asked for %s, ran on %s: %.1f cues/s, %.0f ms per cue, %lld tokens\n  \"%s\" -> \"%s\"\n", device,
                    translator.device().c_str(), english.size() / seconds, seconds * 1000.0 / english.size(),
                    static_cast<long long>(translator.tokens()), english[1].c_str(), result->segments[1].text.c_str());
    }
    translate::LocalTranslatorOptions back;
    back.executable = WABCUT_EMBED_HELPER;
    back.modelDirectory = translate::localModelDirectory(store, "opus-mt-th-en");
    back.device = "cpu";
    translate::LocalTranslator thEn(back);
    TranslationRequest thai;
    thai.sourceLanguage = "th";
    thai.targetLanguage = "en";
    thai.segments = {{"0", "สวัสดีครับ วันนี้เราจะทำแกงเขียวหวาน"}};
    auto english2 = thEn.translate(thai, {});
    REQUIRE(english2);
    CHECK_FALSE(english2->segments[0].text.empty());
    std::printf("opus-mt-th-en th->en: \"%s\"\n", english2->segments[0].text.c_str());
#endif
}
