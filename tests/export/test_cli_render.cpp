// `wabcut-cli render`: a saved project rendered by the CLI (headless), checked with ffprobe.

#include "ExportTestUtil.h"

#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <string>

using namespace exporttest;
using namespace wabcut;

namespace {

struct CliRun {
    int exitCode = -1;
    std::string output; // stdout + stderr
};

CliRun runCli(const std::string& arguments) {
    const std::string line = "\"" + std::string(WABCUT_CLI_EXECUTABLE) + "\" " + arguments + " 2>&1";
#if defined(_WIN32)
    // cmd.exe strips the outer quotes of the whole line, so the line is wrapped in one more pair; /bin/sh must not get
    // them (it would read the whole line as one word).
    const std::string command = "\"" + line + "\"";
#else
    const std::string& command = line;
#endif
#if defined(_WIN32)
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
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

TEST_CASE("wabcut-cli render exports a saved project", "[export][media][cli]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const auto dir = freshOutputDir("cli");
    TestProject project = threeLayerTimeline(640, 360, Rate{30, 1}, Ticks::fromSeconds(1));
    project.editSequence([](Sequence& s) {
        s.markIn = ms(200);
        s.markOut = ms(700);
    });
    const auto projectFile = dir / "project.wabcut";
    const auto saved = io::saveProject(project.state, projectFile, io::SaveOptions{0});
    INFO((saved ? std::string() : saved.error().message));
    REQUIRE(saved.has_value());

    // Video (test pattern) + AAC, In/Out range, the container from the extension.
    const CliRun run = runCli("render " + quoted(projectFile) + " " + quoted(dir / "out.mov") +
                              " --range in-out --test-pattern --encoder libx264");
    INFO(run.output);
    REQUIRE(run.exitCode == 0);
    CHECK(run.output.find("video: libx264, 15 frames") != std::string::npos);
    CHECK(run.output.find("audio: aac, 24000 samples") != std::string::npos);
    const auto probe = ffprobe(dir / "out.mov");
    REQUIRE(probe.has_value());
    REQUIRE(probe->stream("video") != nullptr);
    CHECK(probe->stream("video")->at("codec_name") == "h264");
    CHECK(probe->stream("video")->at("nb_frames") == "15");
    CHECK(probe->format.at("format_name").find("mov") != std::string::npos);

    // Audio-only preset; no extension: the preset's.
    const CliRun wav =
        runCli("render " + quoted(projectFile) + " " + quoted(dir / "mix") + " --preset audio-wav");
    INFO(wav.output);
    REQUIRE(wav.exitCode == 0);
    const auto audio = decodeAudio(dir / "mix.wav");
    REQUIRE(audio.has_value());
    CHECK(audio->pcm.size() == 48'000u * 2);

    // Bad arguments.
    CHECK(runCli("render " + quoted(projectFile) + " " + quoted(dir / "x.mp4") + " --preset nope").exitCode ==
          2);
    CHECK(runCli("render " + quoted(projectFile) + " " + quoted(dir / "x.avi")).exitCode == 2);
    CHECK(runCli("render " + quoted(dir / "missing.wabcut") + " " + quoted(dir / "x.mp4")).exitCode == 2);
    CHECK(partialFiles(dir).empty());
}

TEST_CASE("wabcut-cli render renders the timeline on the GPU", "[export][media][cli][gpu]") {
    if (!haveTimelineMedia()) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const auto dir = freshOutputDir("cli_rendered");
    const TestProject project = threeLayerTimeline(640, 360, Rate{30, 1}, Ticks::fromSeconds(1));
    const auto projectFile = dir / "project.wabcut";
    REQUIRE(io::saveProject(project.state, projectFile, io::SaveOptions{0}).has_value());

    // Without --test-pattern: the timeline rendered by the offscreen renderer (the whole 1 s sequence).
    const CliRun real = runCli("render " + quoted(projectFile) + " " + quoted(dir / "real.mp4"));
    INFO(real.output);
    REQUIRE(real.exitCode == 0);
    CHECK(real.output.find("30 frames, source: OffscreenRenderer") != std::string::npos);
    const auto realProbe = ffprobe(dir / "real.mp4");
    REQUIRE(realProbe.has_value());
    REQUIRE(realProbe->stream("video") != nullptr);
    CHECK(realProbe->stream("video")->at("nb_frames") == "30");

    CHECK(partialFiles(dir).empty());
}
