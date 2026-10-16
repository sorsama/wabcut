// G.2 end to end, as a gamer uses it: an OBS-style recording (720p30 H.264, a game and a mic track, chapter markers)
// on a timeline -> highlights.find -> highlights.toReel (transitions, a slow-motion peak) and highlights.toShort
// (layout.apply) -> both rendered with the real renderer (YouTube 1080p, vertical 1080x1920), checked with ffprobe.

#include "ExportTestUtil.h"
#include "../ai/analysis/AnalysisTestUtil.h"
#include "../ai/highlights/SyntheticGameplay.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/highlights/MediaHighlights.h>
#include <wabcut/command/Engine.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/model/MediaLocator.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace exporttest;
using namespace wabcut;
using command::Json;

namespace {

std::string inQuotes(const std::filesystem::path& p) { return "\"" + pathToUtf8(p) + "\""; }

/// The vcpkg ffmpeg next to the ffprobe the export tests use.
std::string ffmpegPath() {
    std::string probe = WABCUT_FFPROBE_EXECUTABLE;
    const auto at = probe.rfind("ffprobe");
    if (at == std::string::npos) {
        return "ffmpeg";
    }
    return probe.replace(at, 7, "ffmpeg");
}

} // namespace

TEST_CASE("Gaming end to end: a recording's highlights become a reel and a Short that export to MP4", "[export][media][highlights][e2e][gpu]") { // the GPU renderer
    namespace gp = wabcut::test::gameplay;
    gp::Script script;
    script.seconds = 120.0;
    script.events = {{20.0, true, true, false, true, false, true}, {55.0, true, true, false, true, false, false},
                     {90.0, true, false, true, true, false, false}};
    script.decoyGame = {38.0, 75.0};
    const auto dir = freshOutputDir("highlights-e2e");
    ai::analysis::test::writeWav(dir / "game.wav", gp::gameAudio(script), ai::highlights::kAudioRate);
    ai::analysis::test::writeWav(dir / "mic.wav", gp::micAudio(script), ai::highlights::kAudioRate);
    {
        std::ofstream raw(dir / "frames.rgb", std::ios::binary);
        for (const ai::analysis::AnalysisFrame& f : gp::frames(script, 15, 96, 54)) {
            raw.write(reinterpret_cast<const char*>(f.rgb.data()), static_cast<std::streamsize>(f.rgb.size()));
        }
    }
    {
        std::ofstream meta(dir / "chapters.txt", std::ios::binary);
        meta << ";FFMETADATA1\n";
        for (const ai::highlights::Hint& h : gp::markers(script)) {
            const auto msec = static_cast<long long>(h.at.toSeconds() * 1000.0);
            meta << "[CHAPTER]\nTIMEBASE=1/1000\nSTART=" << msec << "\nEND=" << msec + 1000 << "\ntitle=" << h.label << "\n";
        }
    }
    // OBS-like: 1280x720 at 30 fps H.264, AAC game and mic tracks, chapters.
    const auto recording = dir / "2026-10-13 20-01-07.mkv";
    const std::string command = "\"" + inQuotes(ffmpegPath()) + " -loglevel error -y -f rawvideo -pix_fmt rgb24 -s 96x54 -r 15 -i " +
                                inQuotes(dir / "frames.rgb") + " -i " + inQuotes(dir / "game.wav") + " -i " + inQuotes(dir / "mic.wav") +
                                " -f ffmetadata -i " + inQuotes(dir / "chapters.txt") +
                                " -map 0:v -map 1:a -map 2:a -map_chapters 3 -vf scale=1280:720:flags=neighbor -r 30 -c:v libx264 -preset ultrafast "
                                "-pix_fmt yuv420p -c:a aac -ar 48000 -b:a 128k " +
                                inQuotes(recording) + "\"";
    if (std::system(command.c_str()) != 0) {
        SKIP("FFmpeg could not write the recording here");
    }

    // The timeline: the recording on V1, its game track on A1 and its mic on A2 (as an OBS import lays them out).
    TestProject p(1280, 720, Rate{30, 1}, Ticks::fromSeconds(120));
    const MediaId media = p.addMedia(pathToUtf8(recording));
    REQUIRE(p.state.findMedia(media)->info.audio.size() == 2);
    p.addVideoTrack(media, Ticks{}, Ticks::fromSeconds(120), Ticks{});
    p.addAudioTrack(media, Ticks{}, Ticks::fromSeconds(120), Ticks{});
    Clip mic = p.clip(media, StreamKind::Audio, Ticks{}, Ticks::fromSeconds(120), Ticks{});
    mic.stream.index = 1;
    p.addTrack(TrackKind::Audio, {mic});

    ai::highlights::MediaHighlightOptions options;
    options.cacheRoot = dir / "cache";
    auto provider = std::make_shared<ai::highlights::MediaHighlightProvider>(options, dir / "project.wabcut");
    auto services = std::make_shared<command::ServiceBundle>(
        std::vector<std::shared_ptr<command::CommandServices>>{std::make_shared<ai::highlights::HighlightServices>(provider)});
    EditSession session(p.state);
    command::CommandEngine engine(session, ai::edit::editingCommands(), command::EnginePolicy{command::CapabilitySet::all(), 64, services});

    const auto found = engine.executeJson(Json{{"command", "highlights.find"}, {"params", Json{{"count", 5}}}});
    INFO(command::toJson(found).dump(2));
    REQUIRE(found.ok());
    REQUIRE(found.data["moments"].size() >= 3);
    std::vector<ai::highlights::Moment> moments;
    for (const Json& m : found.data["moments"]) {
        ai::highlights::Moment moment;
        moment.range = TimeRange::fromStartEnd(Ticks{m["startTicks"].get<std::int64_t>()}, Ticks{m["endTicks"].get<std::int64_t>()});
        moment.peak = Ticks{m["peakTicks"].get<std::int64_t>()};
        moments.push_back(moment);
    }
    const auto accuracy = ai::highlights::measure(moments, gp::eventTimes(script));
    std::printf("highlights e2e (720p30 MKV, 2 tracks, chapters): %zu moments, recall %.2f, precision %.2f\n", moments.size(),
                accuracy.recall, accuracy.precision);
    CHECK(accuracy.recall == 1.0);

    const auto reel = engine.executeJson(Json{{"command", "highlights.toReel"},
                                              {"params", Json{{"order", "time"}, {"count", 3}, {"slowMotion", true}, {"transition", "crossDissolve"}}}});
    INFO(command::toJson(reel).dump(2));
    REQUIRE(reel.ok());
    REQUIRE(reel.createdIds.size() == 1);
    const SequenceId reelId{reel.createdIds.front()};
    const double reelSeconds = reel.data["duration"].get<double>();
    CHECK(reelSeconds > 10.0);
    CHECK(reel.data["transitions"].get<int>() >= 2);

    // Export the reel with the real renderer.
    exporter::ExportRequest request;
    request.snapshot = session.snapshot();
    request.sequence = reelId;
    request.settings = exporter::findPreset("youtube-1080p")->settings;
    request.output = dir / "reel.mp4";
    request.videoSource = {}; // the rendered source
    const auto result = exporter::ExportJob(request).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result);
    const auto probe = ffprobe(request.output);
    REQUIRE(probe);
    REQUIRE(probe->stream("video") != nullptr);
    REQUIRE(probe->stream("audio") != nullptr);
    REQUIRE(probe->format.contains("duration"));
    CHECK(std::abs(std::stod(probe->format.at("duration")) - reelSeconds) < 0.2);
    CHECK(std::abs(result->duration.toSeconds() - reelSeconds) < 0.05);
    std::printf("highlights e2e: a %.1f s reel of %zu moments rendered to MP4 in %.1f s\n", reelSeconds, reel.data["moments"].size(),
                result->seconds);

    // The best moment as a vertical Short (layout.apply's blurred fill: this recording has no facecam), exported for
    // Shorts / TikTok at 1080x1920.
    const auto made = engine.executeJson(Json{{"command", "highlights.toShort"},
                                              {"params", Json{{"sequence", p.sequence.value}, {"moment", 1}, {"captions", false}}}});
    INFO(command::toJson(made).dump(2));
    REQUIRE(made.ok());
    CHECK(made.data["layout"] == "verticalBlurFill");
    const SequenceId shortId{made.createdIds.front()};
    exporter::ExportRequest vertical;
    vertical.snapshot = session.snapshot();
    vertical.sequence = shortId;
    vertical.settings = exporter::findPreset("vertical-1080x1920")->settings;
    vertical.output = dir / "short.mp4";
    vertical.videoSource = {}; // the rendered source
    const auto shortResult = exporter::ExportJob(vertical).run();
    INFO((shortResult ? std::string() : shortResult.error().message));
    REQUIRE(shortResult);
    const auto shortProbe = ffprobe(vertical.output);
    REQUIRE(shortProbe);
    const auto* video = shortProbe->stream("video");
    REQUIRE(video != nullptr);
    CHECK(video->at("width") == "1080");
    CHECK(video->at("height") == "1920");
    CHECK(shortProbe->stream("audio") != nullptr);
    const double shortSeconds = made.data["end"].get<double>() - made.data["start"].get<double>();
    CHECK(std::abs(std::stod(shortProbe->format.at("duration")) - shortSeconds) < 0.2);
    std::printf("highlights e2e: a %.1f s Short rendered to a 1080x1920 MP4 in %.1f s\n", shortSeconds, shortResult->seconds);
}
