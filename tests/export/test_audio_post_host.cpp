// P.2: the host side of AAF / OMF export with real media: ClipAudioRenderer renders a clip's audio through the playback
// path (its media from its source In, past its edges for handles, without its Volume, which the AAF carries as clip
// gain), and `wabcut-cli export-timeline` writes AAF and OMF files with the audio rendered.

#include "ExportTestUtil.h"

#include <wabcut/export/AudioPostHost.h>
#include <wabcut/interchange/AudioPostExport.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <string>

using namespace exporttest;
using namespace wabcut;

namespace {

std::vector<float> renderClip(const TestProject& project, const Track& track, const Clip& clip, TimeRange range,
                              std::expected<void, Error>* status = nullptr) {
    exporter::ClipAudioRenderer renderer;
    interchange::AudioPostRenderRequest request;
    request.sequence = project.sequence;
    request.track = track.id;
    request.clip = clip.id;
    request.range = range;
    request.channels = 2;
    request.sampleRate = 48000;
    std::vector<float> out;
    auto result = renderer.render(project.state, request, [&](std::span<const float> block) {
        out.insert(out.end(), block.begin(), block.end());
        return true;
    });
    if (status) {
        *status = result;
    } else {
        INFO((result ? std::string() : result.error().message));
        REQUIRE(result.has_value());
    }
    return out;
}

double rms(std::span<const float> samples) {
    double sum = 0.0;
    for (const float s : samples) {
        sum += static_cast<double>(s) * s;
    }
    return samples.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(samples.size()));
}

std::string readAll(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct CliRun {
    int exitCode = -1;
    std::string output;
};

CliRun runCli(const std::string& arguments) {
    const std::string line = "\"" + std::string(WABCUT_CLI_EXECUTABLE) + "\" " + arguments + " 2>&1";
#if defined(_WIN32)
    const std::string command = "\"" + line + "\"";
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(line.c_str(), "r");
#endif
    CliRun run;
    if (!pipe) {
        return run;
    }
    char buf[4096];
    while (const std::size_t n = std::fread(buf, 1, sizeof buf, pipe)) {
        run.output.append(buf, n);
    }
#if defined(_WIN32)
    run.exitCode = _pclose(pipe);
#else
    const int status = pclose(pipe);
    run.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    return run;
}

std::string quoted(const std::filesystem::path& p) {
    const std::u8string u = p.generic_u8string();
    return "\"" + std::string(reinterpret_cast<const char*>(u.data()), u.size()) + "\"";
}

} // namespace

TEST_CASE("ClipAudioRenderer renders a clip from its media, with handles, without its volume (P.2)", "[export][media][p2]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const TestProject project = threeLayerTimeline(640, 360, Rate{30, 1}, Ticks::fromSeconds(2));
    const Sequence& seq = project.seq();
    // A2: sine_48k.wav from 0.25 s at 0.25..1.75 s, clip volume -6 dB (not rendered: the AAF carries it).
    const Track& a2 = *seq.audioTracks[1];
    const Clip& sine = a2.clips.front();
    const auto clipAudio = renderClip(project, a2, sine, sine.timelineRange);
    CHECK(clipAudio.size() == 2u * 48'000u * 3u / 2u); // 1.5 s, stereo
    const auto media = decodeAudio(mediaDir() / "sine_48k.wav");
    REQUIRE(media.has_value());
    const std::size_t from = static_cast<std::size_t>(48'000 / 4) * static_cast<std::size_t>(media->channels);
    const std::span<const float> source(media->samples.data() + from, 48'000u * static_cast<std::size_t>(media->channels));
    const double ratioDb = 20.0 * std::log10(rms(std::span<const float>(clipAudio).subspan(0, 2 * 48'000)) / rms(source));
    metric(std::format("P.2 clip audio vs its media: {:+.3f} dB", ratioDb));
    // Full level: the -6 dB clip volume is not in the file (a mono file's pan law may take up to 3 dB).
    CHECK(ratioDb > -4.0);
    CHECK(ratioDb < 1.0);

    // Handles: 0.1 s before the clip's start (the media goes on there) are rendered too.
    const Ticks handle = ms(100);
    const auto withHandles = renderClip(project, a2, sine, TimeRange{sine.start() - handle, sine.timelineRange.duration + handle + handle});
    CHECK(withHandles.size() == 2u * (48'000u * 3u / 2u + 2u * 4'800u));
    CHECK(rms(std::span<const float>(withHandles).subspan(0, 2 * 4'800)) > 0.1);

    // Offline media: NotFound.
    TestProject offline = project;
    offline.editMedia(sine.media, [](MediaItem& m) { m.locator.path = "C:/nowhere/missing.wav"; });
    std::expected<void, Error> status;
    renderClip(offline, *offline.seq().audioTracks[1], offline.seq().audioTracks[1]->clips.front(), sine.timelineRange, &status);
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code == ErrorCode::NotFound);
}

TEST_CASE("wabcut-cli export-timeline writes AAF and OMF with the clips' audio rendered (P.2)", "[export][media][cli][p2]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const auto dir = freshOutputDir("cli-audio-post");
    TestProject project = threeLayerTimeline(640, 360, Rate{30, 1}, Ticks::fromSeconds(1));
    const auto projectFile = dir / "project.wabcut";
    REQUIRE(io::saveProject(project.state, projectFile, io::SaveOptions{0}).has_value());

    const CliRun aaf = runCli("export-timeline " + quoted(projectFile) + " " + quoted(dir / "mix.aaf") +
                              " --separate-audio --handles 6 --json");
    INFO(aaf.output);
    REQUIRE(aaf.exitCode == 0);
    CHECK(aaf.output.find("\"format\": \"AAF\"") != std::string::npos);
    CHECK(readAll(dir / "mix.aaf").substr(0, 4) == std::string("\xD0\xCF\x11\xE0", 4));
    // Both stereo audio tracks broken out to Left / Right files, with sound in them.
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir / "mix Audio")) {
        ++files;
        const auto audio = decodeAudio(entry.path());
        REQUIRE(audio.has_value());
        CHECK(audio->channels == 1);
        CHECK(audio->sampleRate == 48'000);
        CHECK(rms(audio->samples) > 0.01);
    }
    CHECK(files == 4);

    const CliRun omf = runCli("export-timeline " + quoted(projectFile) + " " + quoted(dir / "mix.omf") +
                              " --omf-title \"Reel 1\" --bits 16");
    INFO(omf.output);
    REQUIRE(omf.exitCode == 0);
    CHECK(omf.output.find("Exported OMF") != std::string::npos);
    const std::string bytes = readAll(dir / "mix.omf");
    REQUIRE(bytes.size() > 24);
    CHECK(bytes.substr(bytes.size() - 24, 8) == std::string("\xA4\x43\x4D\xA5\x48\x64\x72\xD7", 8));
    CHECK(runCli("export-timeline " + quoted(projectFile) + " " + quoted(dir / "x.aaf") + " --sample-rate 44100").exitCode == 2);
    CHECK(partialFiles(dir).empty());
}
