// Auto zoom for screen recordings (AI-11): cursor sidecars (JSON, CSV), local detection on a synthetic 1080p screen
// recording with a moving cursor, clicks and typing, the zoom path (targets within tolerance, smoothness bounds, idle
// zoom-out, never outside the frame), autozoom.apply through the engine (dry run with preview, one undo step,
// idempotent rerun), and the analysis throughput at 1080p.

#include "CommandHarness.h"
#include "CreatorTestUtil.h"
#include "ScreenRecording.h"

#include <wabcut/ai/autozoom/AutoZoomCommands.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace wabcut;
using namespace wabcut::ai::autozoom;
using wabcut::test::CommandHarness;
using wabcut::test::Timeline;
using wabcut::ai::autozoom::test::ScreenRecording;
using Catch::Approx;
using command::Json;

namespace {

Ticks at(double seconds) { return Ticks{static_cast<std::int64_t>(std::llround(seconds * static_cast<double>(kTicksPerSecond)))}; }

/// A project whose 20 s video clip (media A, 1920x1080 at 25 fps) is a screen recording in a 1920x1080 sequence.
struct ZoomStudio {
    ZoomStudio() : tl(picture(500), {{'A', 600}}) {
        tl.linkMatching();
        auto frames = [this](const MediaItem&, int) -> std::expected<FrameReader, Error> { return recording.reader(); };
        services = std::make_shared<AutoZoomServices>(std::move(frames));
    }
    static std::string picture(int frames) {
        const std::string cells = "A" + std::string(static_cast<std::size_t>(frames - 1), 'a');
        return "V1 |" + cells + "|\nA1 |" + cells + "|";
    }
    ScreenRecording recording;
    Timeline tl;
    std::shared_ptr<AutoZoomServices> services;
};

double clipValue(const Clip& clip, ParamId id, double seconds) { return clip.properties().value(id, at(seconds)); }

/// The visible part of the frame at `seconds` (frame units: 0 = centre), from the clip's Scale and Position.
struct Window {
    double u = 0, v = 0, half = 0.5;
};
Window windowAt(const Clip& clip, double seconds) {
    const double z = clipValue(clip, ParamId::Scale, seconds);
    return Window{-clipValue(clip, ParamId::PositionX, seconds) / z / 1920.0, -clipValue(clip, ParamId::PositionY, seconds) / z / 1080.0,
                  0.5 / z};
}
bool shows(const Window& w, double px, double py, double margin = 0.01) {
    const double fx = px / 1920.0 - 0.5;
    const double fy = py / 1080.0 - 0.5;
    return std::abs(fx - w.u) <= w.half - margin && std::abs(fy - w.v) <= w.half - margin;
}

std::filesystem::path outputDir() {
    const auto dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "ai-creator";
    std::filesystem::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE("Cursor sidecars: Wabcut JSON and CSV cursor logs", "[ai][creator][autozoom]") {
    auto track = parseCursorSidecar(R"({"format":"wabcut.cursor","version":1,"width":1920,"height":1080,"offset":0.5,
        "events":[{"t":0,"x":960,"y":540},{"t":1,"x":1500,"y":200},{"t":1.1,"x":1500,"y":200,"type":"click"},
                  {"t":2,"type":"key"},{"t":3,"x":10,"y":10,"type":"mystery"}]})");
    REQUIRE(track);
    CHECK(track->origin == "sidecar");
    REQUIRE(track->events.size() >= 2);
    const auto click = std::ranges::find_if(track->events, [](const ActivityEvent& e) { return e.kind == ActivityKind::Click; });
    REQUIRE(click != track->events.end());
    CHECK(click->time == at(1.6));
    CHECK(click->x == Approx(1500.0 / 1920.0));
    const auto key = std::ranges::find_if(track->events, [](const ActivityEvent& e) { return e.kind == ActivityKind::Key; });
    REQUIRE(key != track->events.end());
    CHECK(key->x == Approx(1500.0 / 1920.0)); // typed where the cursor was
    CHECK(std::ranges::any_of(track->events, [](const ActivityEvent& e) { return e.kind == ActivityKind::Dwell; }));
    CHECK_FALSE(parseCursorSidecar(R"({"format":"other","events":[]})"));
    CHECK_FALSE(parseCursorSidecar(R"({"events":[{"x":1,"y":2}]})"));
    CHECK_FALSE(parseCursorSidecar(R"({"events":[{"t":1,"x":500,"y":300}]})")); // pixels without a size
    CHECK(parseCursorSidecar(R"({"events":[{"t":1,"x":500,"y":300}]})", 1920, 1080));

    auto csv = parseCursorCsv("time,x,y,event\n0.0,100,100,\n0.5,960,540,move\n0.6,960,540,click\n", 1920, 1080);
    REQUIRE(csv);
    CHECK(csv->origin == "cursor-log");
    CHECK(csv->cursor.size() == 3);
    CHECK(std::ranges::count_if(csv->events, [](const ActivityEvent& e) { return e.kind == ActivityKind::Click; }) == 1);
    CHECK_FALSE(parseCursorCsv("0,100,100\n", 0, 0));
    CHECK_FALSE(parseCursorCsv("0,100\n", 1920, 1080));
    CHECK(parseCursorFile("  {\"events\":[]}", 1920, 1080));
    CHECK(parseCursorFile("0;0.5;0.5\n", 0, 0));
}

TEST_CASE("Local detection finds the clicks and the typing of a screen recording", "[ai][creator][autozoom]") {
    const ScreenRecording recording;
    DetectStats stats;
    auto track = detectActivity(recording.reader(), TimeRange{Ticks{}, at(20.0)}, DetectOptions{}, {}, &stats);
    REQUIRE(track);
    CHECK(track->origin == "detected");
    CHECK(stats.frames == 200);
    std::string kinds;
    for (const ActivityEvent& e : track->events) {
        kinds += std::string(toString(e.kind)) + "@" + std::to_string(e.time.toSeconds()) + " ";
    }
    INFO(kinds);
    const auto clickNear = [&](double seconds, double px, double py) {
        return std::ranges::any_of(track->events, [&](const ActivityEvent& e) {
            return e.kind == ActivityKind::Click && std::abs(e.time.toSeconds() - seconds) <= 0.5 && std::abs(e.x - px / 1920.0) <= 0.05 &&
                   std::abs(e.y - py / 1080.0) <= 0.05;
        });
    };
    CHECK(clickNear(3.2, ScreenRecording::kButtonX, ScreenRecording::kButtonY));
    CHECK(clickNear(9.2, ScreenRecording::kFieldX, ScreenRecording::kFieldY));
    CHECK(std::ranges::any_of(track->events, [](const ActivityEvent& e) {
        return e.kind == ActivityKind::Key && e.time.toSeconds() > 9.4 && e.time.toSeconds() < 12.6 && std::abs(e.y - 800.0 / 1080.0) < 0.05;
    }));
    // Nothing after the typing stopped, and the cursor was followed.
    CHECK(std::ranges::none_of(track->events, [](const ActivityEvent& e) { return e.time.toSeconds() > 13.0 && e.kind != ActivityKind::Dwell; }));
    CHECK_FALSE(track->cursor.empty());
}

TEST_CASE("The zoom follows the activity, stays smooth and inside the frame, and zooms out when idle", "[ai][creator][autozoom]") {
    ZoomStudio studio;
    CommandHarness h(studio.tl, command::EnginePolicy{command::CapabilitySet::all(), 64, studio.services}, ai::edit::editingCommands());
    const ClipId clip = h.clip("V1@0");
    const auto original = h.state();
    const auto applied = h.ok("autozoom.apply", Json{{"clip", clip.value}, {"maxZoom", 2.0}});
    INFO(applied.data.dump(1));
    CHECK(applied.committed);
    CHECK(applied.data["origin"] == "detected");
    CHECK(applied.data["segments"].size() >= 2);
    CHECK(applied.data["keyframes"].get<int>() > 3);
    CHECK(applied.data["outsideFrame"] == 0.0);
    const Clip& zoomed = h.sequence(h.state()).findClip(clip)->clip();
    // Zoomed out at the start and long after the last activity.
    CHECK(clipValue(zoomed, ParamId::Scale, 0.0) == Approx(1.0));
    CHECK(clipValue(zoomed, ParamId::Scale, 18.5) == Approx(1.0));
    CHECK(clipValue(zoomed, ParamId::PositionX, 18.5) == Approx(0.0).margin(0.5));
    // Zoomed in on the button around its click, and on the field while typing: the activity is in view.
    CHECK(clipValue(zoomed, ParamId::Scale, 4.0) > 1.4);
    CHECK(shows(windowAt(zoomed, 4.0), ScreenRecording::kButtonX, ScreenRecording::kButtonY));
    CHECK(clipValue(zoomed, ParamId::Scale, 10.5) > 1.4);
    CHECK(shows(windowAt(zoomed, 10.5), ScreenRecording::kFieldX + 60, ScreenRecording::kFieldY));
    // Smooth: no step between 60 Hz samples beyond what a 0.6 s eased zoom of 1 -> 2 needs, no jerky acceleration,
    // and the frame never shows outside the picture.
    double maxSpeed = 0.0;
    double maxAccel = 0.0;
    double previousSpeed = 0.0;
    for (int k = 1; k < 20 * 60; ++k) {
        const double z0 = clipValue(zoomed, ParamId::Scale, (k - 1) / 60.0);
        const double z1 = clipValue(zoomed, ParamId::Scale, k / 60.0);
        const double speed = (z1 - z0) * 60.0;
        maxSpeed = std::max(maxSpeed, std::abs(speed));
        if (k > 1) {
            maxAccel = std::max(maxAccel, std::abs(speed - previousSpeed) * 60.0);
        }
        previousSpeed = speed;
        const Window w = windowAt(zoomed, k / 60.0);
        CHECK(std::abs(w.u) + w.half <= 0.5 + 1e-6);
        CHECK(std::abs(w.v) + w.half <= 0.5 + 1e-6);
    }
    char line[160];
    std::snprintf(line, sizeof line, "Auto zoom path: max zoom speed %.2f/s, max acceleration %.1f/s^2, %d keyframes", maxSpeed, maxAccel,
                  applied.data["keyframes"].get<int>());
    std::ofstream(outputDir() / "autozoom.md", std::ios::app) << line << "\n";
    CHECK(maxSpeed <= 3.0);  // eased 1 -> 2 over >= 0.6 s peaks at 2.5/s
    CHECK(maxAccel <= 30.0);

    // Running it again changes nothing (the clip's own placement is recovered from the first key).
    const auto again = h.ok("autozoom.apply", Json{{"clip", clip.value}, {"maxZoom", 2.0}});
    CHECK(again.data["analysis"]["cached"] == true);
    CHECK(h.sequence(h.state()).findClip(clip)->clip().properties() == zoomed.properties());
    // Undo returns the clip's own constants.
    while (h.session.undo()) {
    }
    CHECK(h.sequence(h.state()).findClip(clip)->clip().properties() == h.sequence(original).findClip(clip)->clip().properties());
}

TEST_CASE("autozoom.apply previews with a dry run, uses a sidecar when given, and refuses what it cannot zoom",
          "[ai][creator][autozoom][commands]") {
    ZoomStudio studio;
    CommandHarness h(studio.tl, command::EnginePolicy{command::CapabilitySet::all(), 64, studio.services}, ai::edit::editingCommands());
    const ClipId clip = h.clip("V1@0");
    const auto before = h.state();
    const auto dry = h.run(Json{{"command", "autozoom.apply"}, {"params", Json{{"clip", clip.value}, {"preview", true}}}, {"dryRun", true}});
    REQUIRE(dry.ok());
    CHECK_FALSE(dry.committed);
    CHECK(h.state() == before);
    REQUIRE(dry.data["path"].is_array());
    CHECK(dry.data["path"].size() == 200); // 10 samples a second for 20 s
    CHECK(dry.data["pathColumns"].size() == 6);
    CHECK(dry.data["activity"].size() > 0);
    // The preview's window at 4 s holds the button (picture fractions).
    const Json& sample = dry.data["path"][40];
    const double x = sample[2].get<double>(), y = sample[3].get<double>(), w = sample[4].get<double>(), hgt = sample[5].get<double>();
    CHECK(std::abs(ScreenRecording::kButtonX / 1920.0 - x) < w / 2);
    CHECK(std::abs(ScreenRecording::kButtonY / 1080.0 - y) < hgt / 2);

    // A sidecar's content: zooms where it says, no frames read.
    const std::string sidecar = R"({"format":"wabcut.cursor","version":1,"width":1920,"height":1080,
        "events":[{"t":5,"x":200,"y":900,"type":"click"},{"t":5.5,"x":220,"y":900,"type":"key"}]})";
    const auto fromSidecar = h.ok("autozoom.apply", Json{{"clip", clip.value}, {"cursor", sidecar}, {"easing", "linear"}});
    CHECK(fromSidecar.data["origin"] == "sidecar");
    const Clip& zoomed = h.sequence(h.state()).findClip(clip)->clip();
    CHECK(clipValue(zoomed, ParamId::Scale, 5.5) > 1.4);
    CHECK(shows(windowAt(zoomed, 5.5), 210, 900));
    CHECK(clipValue(zoomed, ParamId::Scale, 2.0) == Approx(1.0));
    REQUIRE(h.session.undo());
    CHECK(h.state() == before);

    // Audio clips, bad sidecars, file paths from clients that may not read files, and hosts without frames.
    CHECK(h.errorOf("autozoom.apply", Json{{"clip", h.clip("A1@0").value}}) == "InvalidParams");
    CHECK(h.errorOf("autozoom.apply", Json{{"clip", clip.value}, {"cursor", "{\"events\":[{\"x\":1}]}"}}) == "InvalidParams");
    CHECK(h.errorOf("autozoom.apply", Json{{"clip", clip.value}, {"sidecar", "recording.cursor.json"}}) == "PermissionDenied");
    CHECK(h.errorOf("autozoom.apply", Json{{"clip", clip.value}, {"detect", "sidecar"}}) == "EditRejected");
    CommandHarness bare(studio.tl, command::EnginePolicy{}, ai::edit::editingCommands());
    CHECK(bare.errorOf("autozoom.apply", Json{{"clip", clip.value}}) == "InvalidRequest");
}

TEST_CASE("Auto zoom analysis throughput on 1080p frames", "[ai][creator][autozoom][benchmark]") {
    const ScreenRecording recording;
    // Frames are made first, so only reduction to the analysis size and the detection are timed.
    std::vector<std::vector<std::uint8_t>> frames;
    for (int k = 0; k < 60; ++k) {
        frames.push_back(recording.frame(2.0 + k / 10.0));
    }
    ActivityDetector detector;
    const auto started = std::chrono::steady_clock::now();
    for (int k = 0; k < 60; ++k) {
        detector.push(toAnalysisFrame(frames[static_cast<std::size_t>(k)].data(), ScreenRecording::kWidth, ScreenRecording::kHeight, 640,
                                      at(2.0 + k / 10.0)));
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const double fps = 60.0 / seconds;
    char line[160];
    std::snprintf(line, sizeof line, "Auto zoom analysis: %.0f frames/s of 1920x1080 RGB (reduction to 640 px + detection), %.2f ms/frame",
                  fps, seconds * 1000.0 / 60.0);
    std::ofstream(outputDir() / "perf.md", std::ios::app) << line << "\n";
    WARN(line);
    CHECK(detector.finish().events.size() > 0);
    CHECK(fps > 10.0); // 10 analysed frames per second of recording run faster than real time
}
