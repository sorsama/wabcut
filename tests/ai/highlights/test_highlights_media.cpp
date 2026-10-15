// G.2 gameplay highlights on a decoded recording (full builds): synthetic gameplay written by FFmpeg as an MKV with a
// game and a mic track and OBS-style chapter markers, a marker file next to it; measured by MediaHighlightProvider
// (cached on the second run), its moments found through highlights.find, with precision and recall against the
// scripted events.

#include "SyntheticGameplay.h"
#include "../analysis/AnalysisTestUtil.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/highlights/MediaHighlights.h>
#include <wabcut/command/Engine.h>
#include <wabcut/media/Probe.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/MediaLocator.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace wabcut;
using namespace wabcut::ai::highlights;
namespace gameplay = wabcut::test::gameplay;
using command::Json;

namespace {

gameplay::Script mediaScript() {
    gameplay::Script s;
    s.seconds = 150.0;
    s.events = {
        {25.0, true, true, false, true, false, true},  // marked by a chapter
        {60.0, true, false, true, true, false, false}, // a laugh
        {98.0, true, true, false, true, false, false}, // marked in the marker file
        {128.0, false, true, false, true, false, true}, // a shout over a quiet game, chapter
    };
    s.decoyGame = {45.0, 112.0};
    s.decoyFlash = {80.0};
    return s;
}

std::string inQuotes(const std::filesystem::path& p) { return "\"" + pathToUtf8(p) + "\""; }

} // namespace

TEST_CASE("Decoded gameplay: an MKV with game and mic tracks and chapters is measured and its moments found",
          "[ai][highlights][media]") {
    const std::string ffmpeg = WABCUT_FFMPEG;
    if (ffmpeg.empty() || ffmpeg.ends_with("NOTFOUND")) {
        SKIP("no FFmpeg here to write the fixture");
    }
    const gameplay::Script script = mediaScript();
    const auto dir = std::filesystem::temp_directory_path() / "wabcut-highlights-media";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    ai::analysis::test::writeWav(dir / "game.wav", gameplay::gameAudio(script), kAudioRate);
    ai::analysis::test::writeWav(dir / "mic.wav", gameplay::micAudio(script), kAudioRate);
    {
        std::ofstream raw(dir / "frames.rgb", std::ios::binary);
        for (const ai::analysis::AnalysisFrame& f : gameplay::frames(script, 15, 96, 54)) {
            raw.write(reinterpret_cast<const char*>(f.rgb.data()), static_cast<std::streamsize>(f.rgb.size()));
        }
    }
    {
        // OBS writes chapter markers into hybrid MP4 / MKV: here 3 s after the marked events.
        std::ofstream meta(dir / "chapters.txt", std::ios::binary);
        meta << ";FFMETADATA1\n";
        for (const Hint& h : gameplay::markers(script)) {
            const auto ms = static_cast<long long>(h.at.toSeconds() * 1000.0);
            meta << "[CHAPTER]\nTIMEBASE=1/1000\nSTART=" << ms << "\nEND=" << ms + 1000 << "\ntitle=" << h.label << "\n";
        }
    }
    const auto recording = dir / "2026-10-12 21-04-11.mkv";
    {
        // A hotkey logger's marker file next to the recording: the third event.
        std::ofstream markers(std::filesystem::path(recording).concat(".markers.txt"), std::ios::binary);
        markers << "# hotkey markers\n1:41 nice shot\n";
    }
    const std::string command = "\"" + inQuotes(ffmpeg) + " -loglevel error -y -f rawvideo -pix_fmt rgb24 -s 96x54 -r 15 -i " +
                                inQuotes(dir / "frames.rgb") + " -i " + inQuotes(dir / "game.wav") + " -i " + inQuotes(dir / "mic.wav") +
                                " -f ffmetadata -i " + inQuotes(dir / "chapters.txt") +
                                " -map 0:v -map 1:a -map 2:a -map_chapters 3 -c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac -b:a 128k " +
                                inQuotes(recording) + "\"";
    REQUIRE(std::system(command.c_str()) == 0);

    auto info = media::probe(pathToUtf8(recording));
    REQUIRE(info);
    REQUIRE(info->audio.size() == 2);
    ProjectState state;
    MediaItem item;
    item.id = state.ids.allocate<MediaId>();
    item.name = pathToUtf8(recording.filename());
    item.locator.path = pathToUtf8(recording);
    item.info = *info;
    const MediaId id = item.id;
    state.media.push_back(std::make_shared<const MediaItem>(item));

    MediaHighlightOptions options;
    options.cacheRoot = dir / "cache";
    auto provider = std::make_shared<MediaHighlightProvider>(options, dir / "project.wabcut");
    const auto t0 = std::chrono::steady_clock::now();
    auto measured = provider->measure(item, true, {});
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(measured);
    std::printf("highlights media: measured %.0f s of gameplay (2 audio streams + picture) in %.2f s\n", script.seconds, seconds);
    REQUIRE(measured->audio.size() == 2);
    REQUIRE(measured->video.has_value());
    CHECK(measured->audio[0].size() >= 1490);
    CHECK_FALSE(measured->fromCache);
    // Two chapters and the marker file's line.
    REQUIRE(measured->hints.size() == 3);
    CHECK(std::count_if(measured->hints.begin(), measured->hints.end(), [](const Hint& h) { return h.source == "chapter"; }) == 2);
    CHECK(std::any_of(measured->hints.begin(), measured->hints.end(), [](const Hint& h) { return h.label == "nice shot"; }));
    CHECK(classifyStreams({&measured->audio[0], &measured->audio[1]}) == std::vector<AudioRole>{AudioRole::Game, AudioRole::Mic});

    // Again: from the cache, nothing decoded.
    auto again = provider->measure(item, true, {});
    REQUIRE(again);
    CHECK(again->fromCache);
    REQUIRE(again->audio[1].size() == measured->audio[1].size());
    float largest = 0.0f;
    for (std::size_t i = 0; i < again->audio[1].size(); ++i) {
        largest = std::max(largest, std::abs(again->audio[1].levelDb[i] - measured->audio[1].levelDb[i]));
    }
    CHECK(largest <= 0.006f); // kept to 2 decimals

    // C.16's dead-time activity from these measurements (nothing decoded): 10 samples a second, valid values.
    const auto activity = cachedActivity(item, options.cacheRoot, dir / "project.wabcut");
    REQUIRE(activity.has_value());
    CHECK(activity->source == "analysis");
    CHECK(activity->step == Ticks{kTicksPerSecond / 10});
    CHECK(activity->audio.size() == measured->audio[0].size());
    CHECK(activity->motion.size() == measured->video->size());
    CHECK_FALSE(mediaActivityProblem(*activity).has_value());
    CHECK(*std::max_element(activity->audio.begin(), activity->audio.end()) > 0.5f); // the blasts
    MediaItem unmeasured = item;
    unmeasured.locator.path = pathToUtf8(dir / "game.wav");
    unmeasured.info.video.clear();
    CHECK_FALSE(cachedActivity(unmeasured, options.cacheRoot, dir / "project.wabcut").has_value());

    // Through the command, on the media item: clip markers, and the accuracy against the script.
    auto services = std::make_shared<command::ServiceBundle>(
        std::vector<std::shared_ptr<command::CommandServices>>{std::make_shared<HighlightServices>(provider)});
    EditSession session(state);
    command::CommandEngine engine(session, ai::edit::editingCommands(), command::EnginePolicy{command::CapabilitySet::all(), 64, services});
    const auto result = engine.executeJson(Json{{"command", "highlights.find"}, {"params", Json{{"media", id.value}, {"count", 4}}}});
    INFO(command::toJson(result).dump(2));
    REQUIRE(result.ok());
    CHECK(result.data["recordings"][0]["fromCache"] == true);
    std::vector<Moment> moments;
    for (const Json& m : result.data["moments"]) {
        Moment moment;
        moment.range = TimeRange::fromStartEnd(Ticks{m["startTicks"].get<std::int64_t>()}, Ticks{m["endTicks"].get<std::int64_t>()});
        moment.peak = Ticks{m["peakTicks"].get<std::int64_t>()};
        moments.push_back(moment);
    }
    const Accuracy a = measure(moments, gameplay::eventTimes(script));
    std::printf("highlights accuracy (decoded MKV, all signals but chat): precision %.2f, recall %.2f\n", a.precision, a.recall);
    CHECK(a.recall >= 0.75);
    CHECK(a.precision >= 0.75);
    CHECK(session.snapshot()->findMedia(id)->markers.size() == moments.size());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
