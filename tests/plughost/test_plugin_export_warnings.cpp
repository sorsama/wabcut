// Review wave 5, A5-P2-6: an export says what its file does not have although the project asks for it
// (ExportResult::warnings): a plugin missing on this computer (known before it renders), and a plugin that crashed or
// hung while it rendered (its processing is missing from then on). Before the fix the export rendered such plugins
// bypassed, wrote the file and reported plain success: nothing in the result (there were no warnings) and nothing in
// ExportJob or the mixdown looked at plugins at all. A complete export has no warnings.
//
// The exports here are audio only (the "audio-wav" preset) of a WAV this test writes, so they need neither the media
// suite's generated clips nor a GPU.

#include "PlughostFixture.h"

#include <wabcut/export/ExportJob.h>
#include <wabcut/export/ExportSettings.h>
#include <wabcut/media/Probe.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectState.h>

#include <algorithm>
#include <chrono>
#include <expected>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

/// A 2 s stereo float WAV of stereoTone() (440 Hz left, 660 Hz right) in the test's output folder.
std::filesystem::path toneWav(const std::string& name) {
    const std::filesystem::path file = plughostOutput(name + ".wav");
    const std::vector<float> samples = stereoTone(96000);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    const auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const auto bytes = static_cast<std::uint32_t>(samples.size() * sizeof(float));
    out.write("RIFF", 4);
    u32(36 + bytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(3); // IEEE float
    u16(2);
    u32(48000);
    u32(48000 * 8);
    u16(8);
    u16(32);
    out.write("data", 4);
    u32(bytes);
    out.write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(bytes));
    return file;
}

/// A project with one sequence: A1 plays `name`.wav (toneWav) from 0 for 1 s, its clip carrying `clipEffects`.
ProjectState exportProject(const std::string& name, EffectStack clipEffects) {
    const std::filesystem::path wav = toneWav(name);
    auto info = media::probe(pathToUtf8(wav));
    REQUIRE(info.has_value());
    ProjectState state;
    MediaItem item;
    item.id = state.ids.allocate<MediaId>();
    item.name = name + ".wav";
    item.locator.path = pathToUtf8(wav);
    item.info = *info;
    Clip clip;
    clip.id = state.ids.allocate<ClipId>();
    clip.media = item.id;
    clip.stream = {StreamKind::Audio, 0};
    clip.timelineRange = {Ticks{}, Ticks::fromSeconds(1)};
    clip.sourceIn = Ticks{};
    clip.setEffects(std::move(clipEffects));
    Track track;
    track.id = state.ids.allocate<TrackId>();
    track.kind = TrackKind::Audio;
    track.clips.push_back(std::move(clip));
    Sequence seq;
    seq.id = state.ids.allocate<SequenceId>();
    seq.name = "Plugins";
    seq.width = 640;
    seq.height = 360;
    seq.frameRate = Rate{30, 1};
    seq.duration = Ticks::fromSeconds(1);
    seq.audioTracks.push_back(std::make_shared<const Track>(std::move(track)));
    state.media.push_back(std::make_shared<const MediaItem>(std::move(item)));
    state.sequences.push_back(std::make_shared<const Sequence>(std::move(seq)));
    return state;
}

/// Exports `state`'s sequence with the "audio-wav" preset (24-bit PCM, 48 kHz, stereo) to `name`.wav.
std::expected<exporter::ExportResult, Error> exportWav(const ProjectState& state, const std::string& name) {
    const auto preset = exporter::findPreset("audio-wav");
    REQUIRE(preset.has_value());
    exporter::ExportRequest request;
    request.snapshot = std::make_shared<const ProjectState>(state);
    request.sequence = state.sequences.front()->id;
    request.settings = preset->settings;
    request.output = plughostOutput(name + "-export.wav");
    request.videoSource = exporter::syntheticVideoSourceFactory(); // unused: the preset has no video
    return exporter::ExportJob(std::move(request)).run();
}

bool anyContains(const std::vector<std::string>& warnings, std::string_view text) {
    return std::any_of(warnings.begin(), warnings.end(), [&](const std::string& w) { return w.find(text) != std::string::npos; });
}

std::string joined(const std::vector<std::string>& warnings) {
    std::string out;
    for (const std::string& w : warnings) {
        out += (out.empty() ? "" : " | ") + w;
    }
    return out;
}

} // namespace

TEST_CASE("An export lists a plugin missing on this computer before it renders and still writes the file",
          "[plughost][export][warnings]") {
    TestPlugins plugins;
    EffectInstance absent;
    absent.id = EffectInstanceId{951};
    absent.effect = "plugin.vst3.00112233445566778899AABBCCDDEEFF";
    absent.set("p7", ParamValue(0.25));
    absent.setData(EffectData{std::string(plughost::kPluginNameKey), "Elsewhere EQ (Some Vendor)"});
    const EffectInstance gain = pluginEffect(plugins.clap(), 952, {{"p1", ParamValue(-6.0)}});

    SECTION("a missing plugin") {
        const auto result = exportWav(exportProject("export-missing", {absent, gain}), "export-missing");
        INFO((result ? joined(result->warnings) : result.error().message));
        REQUIRE(result.has_value());
        REQUIRE(result->warnings.size() == 1); // the missing one only: the CLAP gain played throughout
        CHECK(result->warnings[0].find("Missing plugin: Elsewhere EQ (Some Vendor)") != std::string::npos);
        CHECK(result->audioSamples == 48000);
        CHECK(std::filesystem::exists(result->output));
    }
    SECTION("a complete export has no warnings") {
        const auto result = exportWav(exportProject("export-complete", {gain}), "export-complete");
        INFO((result ? joined(result->warnings) : result.error().message));
        REQUIRE(result.has_value());
        CHECK(result->warnings.empty());
        CHECK(result->audioSamples == 48000);
    }
}

TEST_CASE("An export lists a plugin that crashed while it rendered", "[plughost][export][warnings][crash]") {
    TestPlugins plugins;
    // The CLAP test plugin crashes at 0.25 s (its "Crash" parameter): the helper process dies.
    const Curve crash{{Keyframe{Ticks{}, 0.0, Interpolation::Hold}, Keyframe{Ticks{kTicksPerSecond / 4}, 1.0, Interpolation::Hold}}};
    const auto result = exportWav(
        exportProject("export-crash", {pluginEffect(plugins.clap(), 961, {{"p1", ParamValue(-6.0)}, {"p3", ParamValue(crash)}})}),
        "export-crash");
    INFO((result ? joined(result->warnings) : result.error().message));
    REQUIRE(result.has_value()); // the file is written (the rest of it without the plugin)...
    CHECK(result->audioSamples == 48000);
    CHECK(anyContains(result->warnings, "Wabcut Test Gain (Wabcut)")); // ...and the result says what it lacks
    CHECK(anyContains(result->warnings, "crashed"));
}

TEST_CASE("An export lists a plugin that hung while it rendered", "[plughost][export][warnings][deadline]") {
    plughost::HostOptions options = testHostOptions();
    options.blockTimeout = std::chrono::milliseconds(300); // an offline render's deadline per block, short for the test
    options.playbackBlockTimeout = options.blockTimeout;
    TestPlugins plugins(options);
    // Every process call sleeps 1 s: the first block misses the 300 ms, the instance is hung and bypassed.
    const auto t0 = std::chrono::steady_clock::now();
    const auto result =
        exportWav(exportProject("export-hang", {pluginEffect(plugins.clap(), 971, {}, clapState(-6.0, 0.0, 0.0, 1000.0))}),
                  "export-hang");
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    INFO((result ? joined(result->warnings) : result.error().message));
    REQUIRE(result.has_value());
    CHECK(result->audioSamples == 48000);
    CHECK(anyContains(result->warnings, "Wabcut Test Gain (Wabcut)"));
    CHECK(anyContains(result->warnings, "stopped responding"));
    CHECK(seconds < 20.0); // one late block, not one wait per block
}
