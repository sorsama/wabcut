// wabcut-cli with the audio commands on a real file: measure a WAV's loudness (decoded through MediaAudioFeed),
// normalise the mix to YouTube's -14 LUFS with the master true-peak limiter, save, and measure again (cached). And
// multicam (A.5 part 3): two recordings of one event synced by audio into a multicam source, cut in, switched, nested
// and rendered in sync. And Essential Sound and Music Remix (A.7): a podcast-voice preset on real dialogue, matched
// through the real render path, and a real song remixed to length and rendered: every piece plays its music.

#include "MusicTestSignals.h"
#include "TimelineFixture.h"

#include <wabcut/media/AudioDecoder.h>
#include <wabcut/media/Probe.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <sstream>
#include <string>

using namespace wabcut;
using Catch::Approx;

namespace {

/// A 48 kHz stereo 16-bit WAV: a 1 kHz tone of `peakDbfs` on both channels (EBU Tech 3341 case 1 style).
void writeTone(const std::filesystem::path& file, double seconds, double peakDbfs) {
    constexpr std::uint32_t kRate = 48000;
    const auto frames = static_cast<std::uint32_t>(seconds * kRate);
    std::ofstream out(file, std::ios::binary);
    const auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4);
    u32(36 + frames * 4);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(2);
    u32(kRate);
    u32(kRate * 4);
    u16(4);
    u16(16);
    out.write("data", 4);
    u32(frames * 4);
    const double amplitude = std::pow(10.0, peakDbfs / 20.0) * 32767.0;
    for (std::uint32_t n = 0; n < frames; ++n) {
        const auto v = static_cast<std::int16_t>(std::lround(amplitude * std::sin(2.0 * std::numbers::pi * 1000.0 * n / kRate)));
        u16(static_cast<std::uint16_t>(v));
        u16(static_cast<std::uint16_t>(v));
    }
}

/// Runs wabcut-cli with `args`, returning stdout (the exit code in `code`).
std::string cli(const std::filesystem::path& dir, const std::string& args, int& code) {
    const auto out = dir / "out.txt";
    const std::string line = "\"" + std::string(WABCUT_CLI_PATH) + "\" " + args + " > \"" + out.string() + "\"";
#if defined(_WIN32)
    code = std::system(("\"" + line + "\"").c_str()); // cmd.exe strips one pair of outer quotes
#else
    code = std::system(line.c_str());
#endif
    std::ifstream in(out, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

std::string request(const std::filesystem::path& dir, const nlohmann::json& body) {
    const auto file = dir / "request.json";
    std::ofstream(file, std::ios::binary) << body.dump();
    return "--json-file \"" + file.string() + "\"";
}

} // namespace

TEST_CASE("wabcut-cli measures and normalises a real file's loudness", "[audio][edit][cli]") {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "audio-edit-cli";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto wav = dir / "tone.wav";
    writeTone(wav, 10.0, -20.0);

    // A project with that file on A1 (250 frames = 10 s at the fixture's 25 fps).
    test::Timeline tl("A1 |A" + std::string(249, 'a') + "|");
    {
        const auto info = media::probe(wav.string());
        REQUIRE(info);
        auto item = std::make_shared<MediaItem>(*tl.state.findMedia(tl.media('A')));
        item->info = *info;
        item->locator.path = wav.string();
        std::vector<MediaPtr> media;
        for (const MediaPtr& m : tl.state.media) {
            media.push_back(m->id == item->id ? item : m);
        }
        tl.state.media = std::move(media);
    }
    const auto project = dir / "tone.wabcut";
    REQUIRE(io::saveProject(tl.state, project));
    const std::string cache = "WABCUT_CACHE_DIR=" + (dir / "cache").string();
#if defined(_WIN32)
    _putenv(cache.c_str());
#else
    putenv(const_cast<char*>(cache.c_str()));
#endif

    int code = -1;
    const auto t0 = std::chrono::steady_clock::now();
    const std::string measured = cli(dir, "cmd \"" + project.string() + "\" " +
                                              request(dir, {{"command", "audio.loudness"},
                                                            {"params", {{"targets", nlohmann::json::array({"mix"})}}}}),
                                     code);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    INFO(measured);
    REQUIRE(code == 0);
    const auto first = nlohmann::json::parse(measured);
    CHECK(first["data"]["results"][0]["integratedLufs"].get<double>() == Approx(-20.0).margin(0.1));
    WARN("wabcut-cli audio.loudness on 10 s of 48 kHz stereo: " << seconds << " s (process start included)");

    const std::string normalized = cli(dir, "cmd \"" + project.string() + "\" " +
                                                request(dir, {{"command", "audio.normalize"},
                                                              {"params", {{"preset", "youtube"}, {"limit", true}}}}) +
                                                " --save",
                                       code);
    INFO(normalized);
    REQUIRE(code == 0);
    const auto norm = nlohmann::json::parse(normalized);
    CHECK(norm["data"]["results"][0]["after"]["integratedLufs"].get<double>() == Approx(-14.0).margin(0.1));
    const auto reloaded = io::loadProject(project);
    REQUIRE(reloaded);
    const Sequence& seq = *reloaded->findSequence(tl.seq);
    REQUIRE(seq.audioEffects.size() == 2);
    CHECK(seq.audioEffects[0].effect == "wabcut.audio.gain");
    CHECK(seq.audioEffects[1].effect == "wabcut.audio.limiter");

    const std::string again = cli(dir, "cmd \"" + project.string() + "\" " +
                                           request(dir, {{"command", "audio.loudness"}, {"params", nlohmann::json::object()}}),
                                  code);
    INFO(again);
    REQUIRE(code == 0);
    const auto after = nlohmann::json::parse(again);
    CHECK(after["data"]["results"][0]["integratedLufs"].get<double>() == Approx(-14.0).margin(0.1));
    CHECK(after["data"]["results"][0]["truePeakDbtp"].get<double>() <= -1.0 + 0.02);
}

namespace {

std::uint64_t mix64(std::uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

double uniform(std::uint64_t x) { return static_cast<double>(mix64(x) >> 11) / 9007199254740992.0 * 2.0 - 1.0; }

/// The sound of one event at 48 kHz sample n >= 0: 40 ms noise bursts at irregular times over a quiet floor, what two
/// devices in one room record.
double eventAt(std::int64_t n) {
    double v = 0.003 * uniform(static_cast<std::uint64_t>(n) * 2 + 1);
    const std::int64_t slot = n / 9600; // a burst somewhere in every 200 ms
    const std::int64_t start = slot * 9600 + static_cast<std::int64_t>(mix64(static_cast<std::uint64_t>(slot) + 77) % 6000);
    const std::int64_t k = n - start;
    if (k >= 0 && k < 1920) {
        const double amplitude = 0.3 + 0.3 * static_cast<double>(mix64(static_cast<std::uint64_t>(slot) + 99) % 1000) / 1000.0;
        v += amplitude * std::exp(-static_cast<double>(k) / 480.0) * uniform(static_cast<std::uint64_t>(n) * 2);
    }
    return v;
}

/// A 48 kHz stereo 16-bit WAV of the event from its sample `from` on, at `gain`: a device that started recording then.
void writeEvent(const std::filesystem::path& file, double seconds, std::int64_t from, double gain) {
    constexpr std::uint32_t kRate = 48000;
    const auto frames = static_cast<std::uint32_t>(seconds * kRate);
    std::ofstream out(file, std::ios::binary);
    const auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4);
    u32(36 + frames * 4);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(2);
    u32(kRate);
    u32(kRate * 4);
    u16(4);
    u16(16);
    out.write("data", 4);
    u32(frames * 4);
    for (std::uint32_t n = 0; n < frames; ++n) {
        const auto v = static_cast<std::int16_t>(std::lround(std::clamp(eventAt(from + n) * gain, -1.0, 1.0) * 32767.0));
        u16(static_cast<std::uint16_t>(v));
        u16(static_cast<std::uint16_t>(v));
    }
}

std::vector<float> decodeFile(const std::filesystem::path& file, double seconds) {
    auto decoder = media::AudioDecoder::open(file.string(), -1, Rate{48000, 1}, 2);
    REQUIRE(decoder);
    std::vector<float> out(static_cast<std::size_t>(seconds * 48000.0) * 2);
    REQUIRE((*decoder)->read(Ticks{}, out));
    return out;
}

/// Frames of [from, to) seconds where `out` differs from `expected(frame)` by more than `tolerance`.
template <class Expected>
std::size_t mismatches(const std::vector<float>& out, double from, double to, Expected expected, double tolerance = 1e-3) {
    std::size_t wrong = 0;
    for (auto k = static_cast<std::size_t>(from * 48000.0); k < static_cast<std::size_t>(to * 48000.0); ++k) {
        wrong += std::fabs(static_cast<double>(out[k * 2]) - expected(k)) > tolerance ? 1 : 0;
    }
    return wrong;
}

} // namespace

TEST_CASE("wabcut-cli syncs a multicam source by audio and renders its angles, switched and nested, in sync",
          "[audio][edit][cli][multicam]") {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "audio-edit-cli-multicam";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    // The camera records the event from its start, the recorder from 3.2 s on (153600 samples), quieter.
    const auto camera = dir / "camera.wav";
    const auto recorder = dir / "recorder.wav";
    writeEvent(camera, 20.0, 0, 0.8);
    writeEvent(recorder, 16.0, 153600, 0.5);

    test::Timeline tl("A1 |A" + std::string(499, 'a') + "|\nA2 |R" + std::string(399, 'r') + "|");
    {
        std::vector<MediaPtr> media;
        for (const MediaPtr& m : tl.state.media) {
            const std::filesystem::path* file = m->id == tl.media('A') ? &camera : m->id == tl.media('R') ? &recorder : nullptr;
            if (file == nullptr) {
                media.push_back(m);
                continue;
            }
            const auto info = media::probe(file->string());
            REQUIRE(info);
            auto item = std::make_shared<MediaItem>(*m);
            item->info = *info;
            item->locator.path = file->string();
            media.push_back(item);
        }
        tl.state.media = std::move(media);
    }
    const auto project = dir / "stage.wabcut";
    REQUIRE(io::saveProject(tl.state, project));
    const std::string cache = "WABCUT_CACHE_DIR=" + (dir / "cache").string();
#if defined(_WIN32)
    _putenv(cache.c_str());
#else
    putenv(const_cast<char*>(cache.c_str()));
#endif
    int code = -1;
    const auto run = [&](const nlohmann::json& body) {
        const std::string text = cli(dir, "cmd \"" + project.string() + "\" " + request(dir, body) + " --save", code);
        INFO(text);
        REQUIRE(code == 0);
        return nlohmann::json::parse(text);
    };

    // Create Multi-Camera Source Sequence, synced by audio: the recorder's angle lies 3.2 s later, to the sample.
    const auto created = run({{"command", "multicam.create"},
                              {"params", {{"media", nlohmann::json::array({tl.media('A').value, tl.media('R').value})},
                                          {"sync", "audio"},
                                          {"name", "Stage"}}}});
    const std::uint64_t multicam = created["data"]["multicam"].get<std::uint64_t>();
    REQUIRE(created["data"]["angles"].size() == 2);
    CHECK(created["data"]["angles"][0]["offset"]["ticks"].get<std::int64_t>() == 0);
    CHECK(std::llabs(created["data"]["angles"][1]["offset"]["ticks"].get<std::int64_t>() - 153600 * 5'292'000LL) <= 5'292'000LL);

    // Cut 10 s of it (source 5 s to 15 s) onto A1 at 30 s with the camera's sound, then switch the sound to the recorder
    // at 35 s.
    run({{"command", "clip.overwrite"},
         {"params", {{"sequence", tl.seq.value},
                     {"sourceSequence", multicam},
                     {"at", "00:00:30:00"},
                     {"sourceIn", "00:00:05:00"},
                     {"sourceOut", "00:00:15:00"},
                     {"audioAngles", nlohmann::json::array({1})},
                     {"audio", nlohmann::json::array({"A1"})},
                     {"video", nullptr}}}});
    run({{"command", "multicam.switchAngle"},
         {"params", {{"sequence", tl.seq.value},
                     {"clip", {{"track", "A1"}, {"time", "00:00:31:00"}}},
                     {"angle", 2},
                     {"at", "00:00:35:00"},
                     {"target", "audio"}}}});

    const auto render = [&](const std::string& name) {
        const auto out = dir / name;
        cli(dir, "render \"" + project.string() + "\" \"" + out.string() + "\" --preset audio-wav --sequence " +
                     std::to_string(tl.seq.value), code);
        REQUIRE(code == 0);
        return decodeFile(out, 40.0);
    };
    const std::vector<float> mix = render("mix.wav");
    const std::vector<float> cam = decodeFile(camera, 20.0);
    const std::vector<float> rec = decodeFile(recorder, 16.0);
    // Timeline t (30..40 s) plays multicam time 5 + (t - 30): the camera's file there, then the recorder's file 3.2 s
    // (153600 samples) earlier - the same moment of the event.
    const auto camAt = [&](std::size_t k) { return static_cast<double>(cam[(k - 30 * 48000 + 5 * 48000) * 2]); };
    const auto recAt = [&](std::size_t k) { return static_cast<double>(rec[(k - 30 * 48000 + 5 * 48000 - 153600) * 2]); };
    CHECK(mismatches(mix, 30.01, 34.99, camAt) == 0);
    CHECK(mismatches(mix, 35.01, 39.99, recAt) == 0);
    // The switch crossfades (10 ms around 35 s) between two recordings of the same moment: the event goes on.
    CHECK(mismatches(mix, 34.99, 35.01, [&](std::size_t k) { return eventAt(static_cast<std::int64_t>(k) - 25 * 48000) * 0.65; },
                     0.25 * 0.6) == 0);

    // Nest the two multicam clips (Premiere's Nest): the nested sequence plays them as before.
    run({{"command", "sequence.nest"},
         {"params", {{"sequence", tl.seq.value},
                     {"clips", nlohmann::json::array({nlohmann::json{{"track", "A1"}, {"time", "00:00:31:00"}},
                                                     nlohmann::json{{"track", "A1"}, {"time", "00:00:36:00"}}})}}}});
    const std::vector<float> nested = render("nested.wav");
    CHECK(mismatches(nested, 30.01, 39.99, [&](std::size_t k) { return static_cast<double>(mix[k * 2]); }, 1e-6) == 0);
}

TEST_CASE("wabcut-cli: Essential Sound on real dialogue, a real song remixed to length and rendered (A.7)", "[audio][edit][cli][essential]") {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "audio-edit-cli-essential";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string cache = "WABCUT_CACHE_DIR=" + (dir / "cache").string();
#if defined(_WIN32)
    _putenv(cache.c_str());
#else
    putenv(const_cast<char*>(cache.c_str()));
#endif
    int code = -1;
    const auto withFile = [&](test::Timeline& tl, char letter, const std::filesystem::path& file) {
        const auto info = media::probe(file.string());
        REQUIRE(info);
        auto item = std::make_shared<MediaItem>(*tl.state.findMedia(tl.media(letter)));
        item->info = *info;
        item->locator.path = file.string();
        std::vector<MediaPtr> media;
        for (const MediaPtr& m : tl.state.media) {
            media.push_back(m->id == item->id ? item : m);
        }
        tl.state.media = std::move(media);
    };
    const auto run = [&](const std::filesystem::path& project, const nlohmann::json& body) {
        const std::string out = cli(dir, "cmd \"" + project.string() + "\" " + request(dir, body) + " --save", code);
        INFO(out);
        REQUIRE(code == 0);
        return nlohmann::json::parse(out);
    };

    // Dialogue: 10 s of a -20 dBFS tone on A1. The podcast-voice preset, its loudness matched to -23 LUFS by the render.
    {
        const auto wav = dir / "dialogue.wav";
        writeTone(wav, 10.0, -20.0);
        test::Timeline tl("A1 |A" + std::string(249, 'a') + "|");
        withFile(tl, 'A', wav);
        const auto project = dir / "dialogue.wabcut";
        REQUIRE(io::saveProject(tl.state, project));
        const auto r = run(project, {{"command", "audio.essentialSound"},
                                     {"params", {{"clips", nlohmann::json::array({test::clipAt(tl.state, tl.seq, "A1@0").value})},
                                                 {"type", "dialogue"},
                                                 {"preset", "podcastVoice"}}}});
        CHECK(r["data"]["clips"][0]["loudness"]["after"]["integratedLufs"].get<double>() == Approx(-23.0).margin(0.5));
        const auto reloaded = io::loadProject(project);
        REQUIRE(reloaded);
        const Clip& clip = reloaded->findSequence(tl.seq)->audioTracks[0]->clips[0];
        REQUIRE(clip.effects().size() == 8);
        CHECK(clip.effects()[0].effect == "wabcut.audio.essentialSound");
        CHECK(reloaded->findSequence(tl.seq)->audioTracks[0]->role == TrackRole::Dialogue);
    }

    // Music: the 80 s synthetic song at 48 kHz on A2, remixed to 48 s, saved, rendered.
    const test::SyntheticSong song = test::synthesizeSong("IIIIAAAAAAAABBBBBBBBAAAAAAAABBBBBBBBOOOO", 120.0, 48000);
    const auto songWav = dir / "song.wav";
    {
        std::ofstream out(songWav, std::ios::binary);
        const auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
        const auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
        const auto bytes = static_cast<std::uint32_t>(song.samples.size() * 4);
        out.write("RIFF", 4);
        u32(36 + bytes);
        out.write("WAVEfmt ", 8);
        u32(16);
        u16(1);
        u16(2);
        u32(48000);
        u32(48000 * 4);
        u16(4);
        u16(16);
        out.write("data", 4);
        u32(bytes);
        for (const float v : song.samples) {
            // Half scale: the song's peaks stay under the master's soft clip guard (-1 dBFS), so the render is the music as it is.
            const auto pcm = static_cast<std::int16_t>(std::lround(std::clamp(v * 0.5f, -1.0f, 1.0f) * 32767.0f));
            u16(static_cast<std::uint16_t>(pcm));
            u16(static_cast<std::uint16_t>(pcm));
        }
    }
    test::Timeline tl("A1 |M" + std::string(1999, 'm') + "|", {{'M', 2100}});
    withFile(tl, 'M', songWav);
    const auto project = dir / "music.wabcut";
    REQUIRE(io::saveProject(tl.state, project));
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = run(project, {{"command", "audio.remix"}, {"params", {{"clip", test::clipAt(tl.state, tl.seq, "A1@0").value}, {"duration", 48}}}});
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    WARN("wabcut-cli audio.remix of an 80 s song to 48 s: " << seconds << " s (process start and analysis included); " << r["data"]["joins"].size()
                                                            << " joins, confidence " << r["data"]["joins"][0]["confidence"]);
    CHECK(std::fabs(r["data"]["seconds"].get<double>() - 48.0) <= 0.5);
    const auto reloaded = io::loadProject(project);
    REQUIRE(reloaded);
    const Track& track = *reloaded->findSequence(tl.seq)->audioTracks[0];
    REQUIRE(track.clips.size() >= 2);
    CHECK(track.transitions.size() == track.clips.size() - 1);
    // Rendered: away from the joins (30 ms either side) every sample is its piece's music, to the sample.
    const auto out = dir / "remix.wav";
    cli(dir, "render \"" + project.string() + "\" \"" + out.string() + "\" --preset audio-wav --sequence " + std::to_string(tl.seq.value), code);
    REQUIRE(code == 0);
    const std::vector<float> mix = decodeFile(out, 48.0);
    const std::vector<float> source = decodeFile(songWav, 80.0);
    std::size_t checked = 0;
    std::size_t wrong = 0;
    for (std::size_t k = 0; k < track.clips.size(); ++k) {
        const Clip& c = track.clips[k];
        const double start = c.start().toSeconds() + 0.03; // clear of the joins and the edges' de-click fades
        const double end = c.end().toSeconds() - 0.03;
        for (auto n = static_cast<std::size_t>(start * 48000.0); n < static_cast<std::size_t>(std::min(end, 47.9) * 48000.0); ++n) {
            const double sourceSeconds = c.sourceIn.toSeconds() + (static_cast<double>(n) / 48000.0 - c.start().toSeconds());
            const auto s = static_cast<std::size_t>(std::llround(sourceSeconds * 48000.0));
            ++checked;
            wrong += std::fabs(mix[n * 2] - source[s * 2]) > 1e-3f ? 1 : 0;
        }
    }
    CHECK(checked > 40 * 48000);
    CHECK(wrong == 0);
}
