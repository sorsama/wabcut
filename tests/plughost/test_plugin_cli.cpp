// wabcut-cli with audio plugins (A.6): plugins.scan (a user folder), plugins.list, a plugin effect put on a track with
// audio.addEffect and on a clip with clip.addEffect, and the render: the source through both gains, aligned to the
// sample by the delay compensation.

#include "PlughostFixture.h"

#include "TimelineFixture.h"

#include <wabcut/media/AudioDecoder.h>
#include <wabcut/media/Probe.h>
#include <wabcut/model/ProjectIO.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <numbers>
#include <sstream>

using namespace wabcut;
using namespace wabcut::test;

namespace {

void writeTone(const std::filesystem::path& file, double seconds) {
    constexpr std::uint32_t kWavRate = 48000;
    const auto frames = static_cast<std::uint32_t>(seconds * kWavRate);
    std::ofstream out(file, std::ios::binary);
    const auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4);
    u32(36 + frames * 4);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(2);
    u32(kWavRate);
    u32(kWavRate * 4);
    u16(4);
    u16(16);
    out.write("data", 4);
    u32(frames * 4);
    for (std::uint32_t n = 0; n < frames; ++n) {
        const auto v = static_cast<std::int16_t>(std::lround(0.5 * 32767.0 * std::sin(2.0 * std::numbers::pi * 440.0 * n / kWavRate)));
        u16(static_cast<std::uint16_t>(v));
        u16(static_cast<std::uint16_t>(v));
    }
}

std::string cli(const std::filesystem::path& dir, const std::string& args, int& code) {
    const auto out = dir / "out.txt";
    const std::string line = "\"" + std::string(WABCUT_CLI_PATH) + "\" " + args + " > \"" + out.string() + "\"";
    code = std::system(("\"" + line + "\"").c_str());
    std::ifstream in(out, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

std::vector<float> decode(const std::filesystem::path& file, double seconds) {
    auto decoder = media::AudioDecoder::open(file.string(), -1, Rate{48000, 1}, 2);
    REQUIRE(decoder);
    std::vector<float> out(static_cast<std::size_t>(seconds * 48000.0) * 2);
    REQUIRE((*decoder)->read(Ticks{}, out));
    return out;
}

} // namespace

TEST_CASE("wabcut-cli scans and lists plugins, puts them on a track and a clip, and renders them aligned",
          "[plughost][cli]") {
    const auto dir = plughostOutput("cli");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto wav = dir / "tone.wav";
    writeTone(wav, 4.0);
    // A1: the tone, 4 s (100 frames at the fixture's 25 fps).
    test::Timeline tl("A1 |A" + std::string(99, 'a') + "|");
    {
        const auto info = media::probe(wav.string());
        REQUIRE(info);
        auto item = std::make_shared<MediaItem>(*tl.state.findMedia(tl.media('A')));
        item->info = *info;
        item->locator.path = wav.string();
        tl.state.media = {item};
    }
    const auto project = dir / "plugins.wabcut";
    REQUIRE(io::saveProject(tl.state, project));
    const std::string cache = "WABCUT_PLUGIN_CACHE=" + (dir / "plugins.json").string();
    _putenv(cache.c_str());
    int code = -1;
    const auto run = [&](const nlohmann::json& body, bool save) {
        const auto file = dir / "request.json";
        std::ofstream(file, std::ios::binary) << body.dump();
        const std::string text =
            cli(dir, "cmd \"" + project.string() + "\" --json-file \"" + file.string() + "\"" + (save ? " --save" : ""), code);
        INFO(text);
        REQUIRE(code == 0);
        return nlohmann::json::parse(text);
    };

    // Scan: the standard folders (whatever this computer has) and the test plugins' folder, added as a user folder.
    const auto scanned = run({{"command", "plugins.scan"},
                              {"params", {{"addFolders", nlohmann::json::array({(testPluginDir() / "clap").string()})}}}},
                             false);
    CHECK(scanned["data"]["plugins"].get<int>() >= 1);
    const auto listed = run({{"command", "plugins.list"}, {"params", {{"query", "wabcut test"}, {"params", true}}}}, false);
    REQUIRE(listed["data"]["plugins"].size() == 1);
    const auto& plugin = listed["data"]["plugins"][0];
    CHECK(plugin["effect"] == "plugin.clap.org.wabcut.test.gain");
    CHECK(plugin["params"].size() == 5);
    CHECK(plugin["params"][0]["key"] == "p1");

    // A plugin effect on the track (-6 dB) and one on the clip (-3 dB, 100 frames of latency through its state is not
    // reachable by command: parameters only).
    run({{"command", "audio.addEffect"}, {"params", {{"target", "A1"}, {"plugin", plugin["effect"]}, {"params", {{"p1", -6.0}}}}}}, true);
    run({{"command", "clip.addEffect"},
         {"params", {{"clips", nlohmann::json::array({tl.clip("A1@0").value})}, {"plugin", plugin["effect"]}, {"params", {{"p1", -3.0}}}}}},
        true);
    // An unknown plugin is refused with the way to find the right one.
    {
        const auto file = dir / "request.json";
        std::ofstream(file, std::ios::binary)
            << nlohmann::json{{"command", "audio.addEffect"}, {"params", {{"target", "A1"}, {"plugin", "plugin.vst3.00FF"}}}}.dump();
        const std::string text = cli(dir, "cmd \"" + project.string() + "\" --json-file \"" + file.string() + "\"", code);
        CHECK(code != 0);
        CHECK(text.find("plugins.list") != std::string::npos);
    }
    const auto saved = io::loadProject(project);
    REQUIRE(saved);
    const Sequence& seq = *saved->findSequence(tl.seq);
    REQUIRE(seq.audioTracks[0]->effects.size() == 1);
    CHECK(seq.audioTracks[0]->effects[0].effect == "plugin.clap.org.wabcut.test.gain");
    REQUIRE(seq.audioTracks[0]->clips[0].effects().size() == 1);

    // The render: the tone through both gains, where it lies.
    const auto out = dir / "mix.wav";
    cli(dir, "render \"" + project.string() + "\" \"" + out.string() + "\" --preset audio-wav", code);
    REQUIRE(code == 0);
    const std::vector<float> mix = decode(out, 4.0);
    const std::vector<float> source = decode(wav, 4.0);
    const float g = dbGain(-6.0) * dbGain(-3.0);
    std::size_t wrong = 0;
    for (std::size_t k = 1000; k < 190000; ++k) {
        wrong += std::fabs(mix[k * 2] - source[k * 2] * g) > 2e-4f ? 1 : 0;
    }
    CHECK(wrong == 0);

}
