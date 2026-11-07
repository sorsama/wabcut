// P.2 (E6): the Source and Program Monitors' commands - clip.setMotion (the Program Monitor's direct manipulation as
// one undo step), sequence.exportFrame and source.exportFrame (through a fake host renderer).

#include "CommandHarness.h"

#include <wabcut/command/FrameServices.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/MediaLocator.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

constexpr std::string_view kPicture = R"(
    V1 tgt |Aaaaaaaaaa|
    A1 tgt |Aaaaaaaaaa|
)";

const Clip& clipIn(const ProjectSnapshot& s, SequenceId seq, ClipId id) { return s->findSequence(seq)->findClip(id)->clip(); }

} // namespace

TEST_CASE("clip.setMotion sets Position Scale Rotation and Anchor Point as one undo step", "[command][monitor]") {
    CommandHarness h(kPicture);
    const ClipId clip = h.clip("V1@0");
    const auto undoBefore = h.session.undoCount();
    const auto r = h.ok("clip.setMotion", Json{{"clip", clip.value},
                                               {"positionX", 320.0},
                                               {"positionY", -180.0},
                                               {"scale", 0.5},
                                               {"rotation", 15.0},
                                               {"anchorX", 10.0},
                                               {"anchorY", -5.0}});
    CHECK(h.session.undoCount() == undoBefore + 1);
    CHECK(h.session.undoLabel() == "Motion");
    const ProjectSnapshot state = h.state(); // kept: the clip reference points into it
    const Clip& c = clipIn(state, h.tl.seq, clip);
    CHECK(c.properties()[ParamId::PositionX] == ParamValue(320.0));
    CHECK(c.properties()[ParamId::PositionY] == ParamValue(-180.0));
    CHECK(c.properties()[ParamId::Scale] == ParamValue(0.5));
    CHECK(c.properties()[ParamId::Rotation] == ParamValue(15.0));
    CHECK(c.properties()[ParamId::AnchorX] == ParamValue(10.0));
    CHECK(c.properties()[ParamId::AnchorY] == ParamValue(-5.0));
    CHECK(r.data.at("scale") == 0.5);
    CHECK(r.data.at("clip") == clip.value);
    // Undo restores all six at once.
    REQUIRE(h.session.undo());
    const ProjectSnapshot undone = h.state();
    const Clip& u = clipIn(undone, h.tl.seq, clip);
    CHECK(u.properties().isDefault(ParamId::PositionX));
    CHECK(u.properties().isDefault(ParamId::Scale));
    CHECK(u.properties().isDefault(ParamId::AnchorY));
}

TEST_CASE("clip.setMotion keys an animated parameter at the time and needs the time for it", "[command][monitor]") {
    CommandHarness h(kPicture);
    const ClipId clip = h.clip("V1@0");
    h.ok("clip.setAnimated", Json{{"clip", clip.value}, {"param", "motion.scale"}, {"animated", true}});
    // Without a time an animated parameter cannot be keyed.
    CHECK(h.errorOf("clip.setMotion", Json{{"clip", clip.value}, {"scale", 2.0}}) == "InvalidParams");
    // Outside the clip.
    CHECK(h.errorOf("clip.setMotion", Json{{"clip", clip.value}, {"scale", 2.0}, {"time", tc(10)}}) == "InvalidParams");
    h.ok("clip.setMotion", Json{{"clip", clip.value}, {"scale", 2.0}, {"positionX", 50.0}, {"time", tc(5)}});
    const ProjectSnapshot state = h.state();
    const Clip& c = clipIn(state, h.tl.seq, clip);
    const Curve* scale = c.properties()[ParamId::Scale].curve();
    REQUIRE(scale);
    REQUIRE(scale->keys.size() == 2);
    CHECK(scale->keys[1].time == c.curveTimeAt(f(5)));
    CHECK(scale->keys[1].value == 2.0);
    CHECK(c.properties()[ParamId::PositionX] == ParamValue(50.0)); // constant stays constant
}

TEST_CASE("clip.setMotion refuses audio clips locked tracks and empty requests", "[command][monitor]") {
    SECTION("an audio clip") {
        CommandHarness h(kPicture);
        CHECK(h.errorOf("clip.setMotion", Json{{"clip", h.clip("A1@0").value}, {"rotation", 90.0}}) != "");
    }
    SECTION("nothing to set") {
        CommandHarness h(kPicture);
        CHECK(h.errorOf("clip.setMotion", Json{{"clip", h.clip("V1@0").value}}) == "InvalidParams");
    }
    SECTION("a scale beyond 10000 %") {
        CommandHarness h(kPicture);
        CHECK(h.errorOf("clip.setMotion", Json{{"clip", h.clip("V1@0").value}, {"scale", 101.0}}) == "InvalidParams");
    }
    SECTION("a locked track") {
        CommandHarness h(R"(
            V1 lock |Aaaaaaaaaa|
        )");
        CHECK(h.errorOf("clip.setMotion", Json{{"clip", h.clip("V1@0").value}, {"rotation", 90.0}}) != "");
    }
    SECTION("a dry run changes nothing") {
        CommandHarness h(kPicture);
        const auto r = h.run(Json{{"command", "clip.setMotion"},
                                  {"params", Json{{"clip", h.clip("V1@0").value}, {"rotation", 90.0}}},
                                  {"dryRun", true}});
        REQUIRE(r.ok());
        CHECK_FALSE(r.committed);
        CHECK(clipIn(h.state(), h.tl.seq, h.clip("V1@0")).properties().isDefault(ParamId::Rotation));
    }
}

TEST_CASE("clip.setMotion stretches one axis: scaleWidth and uniformScale, one undo step (card 12)", "[command][monitor][scalewidth]") {
    CommandHarness h(kPicture);
    const ClipId clip = h.clip("V1@0");
    h.ok("clip.setMotion", Json{{"clip", clip.value}, {"scale", 0.5}});
    const auto undoBefore = h.session.undoCount();

    // A width turns Uniform Scale off first; the report says what the picture is now.
    const auto r = h.ok("clip.setMotion", Json{{"clip", clip.value}, {"scaleWidth", 2.0}});
    CHECK(h.session.undoCount() == undoBefore + 1);
    CHECK(r.data.at("scale") == 0.5);
    CHECK(r.data.at("scaleWidth") == 2.0);
    CHECK(r.data.at("uniformScale") == false);
    const ProjectSnapshot state = h.state();
    const Clip& c = clipIn(state, h.tl.seq, clip);
    CHECK_FALSE(c.properties().uniformScale());
    CHECK(c.properties().scaleAt() == std::pair<double, double>{2.0, 0.5});

    // The switch back on: the width follows the Scale again; one step, undone as one.
    const auto on = h.ok("clip.setMotion", Json{{"clip", clip.value}, {"uniformScale", true}});
    CHECK(h.session.undoCount() == undoBefore + 2);
    CHECK(on.data.at("uniformScale") == true);
    CHECK(on.data.at("scaleWidth") == 0.5);
    REQUIRE(h.session.undo());
    CHECK(clipIn(h.state(), h.tl.seq, clip).properties().scaleAt() == std::pair<double, double>{2.0, 0.5});

    // The generic parameter commands carry the width along too, and refuse keys on the switch.
    h.ok("clip.setMotion", Json{{"clip", clip.value}, {"uniformScale", true}});
    h.ok("clip.setParam", Json{{"clips", Json::array({clip.value})}, {"param", "motion.uniformScale"}, {"value", 0.0}});
    CHECK(clipIn(h.state(), h.tl.seq, clip).properties()[ParamId::ScaleWidth] == ParamValue(0.5));
    CHECK(h.errorOf("clip.setAnimated", Json{{"clip", clip.value}, {"param", "motion.uniformScale"}, {"animated", true}}) != "");
    CHECK(h.errorOf("clip.setMotion", Json{{"clip", clip.value}, {"scaleWidth", 2.0}, {"uniformScale", true}}) == "InvalidParams");
    CHECK(h.errorOf("clip.setMotion", Json{{"clip", clip.value}, {"scaleWidth", 101.0}}) == "InvalidParams");
}

namespace {

/// Export Frame services that record what they were asked and write a small file instead of rendering.
struct FakeFrames {
    struct Call {
        ProjectState state;
        SequenceId sequence;
        Ticks time{};
        std::filesystem::path file;
        std::string format;
        command::FrameExportServices::Options options; // P1 (#23)
    };
    std::vector<Call> calls;

    std::shared_ptr<command::FrameExportServices> services(bool canImport = true) {
        command::FrameExportServices::Writer writer =
            [this](const ProjectState& state, SequenceId sequence, Ticks time, const std::filesystem::path& file,
                   const std::string& format,
                   const command::FrameExportServices::Options& options) -> std::expected<command::FrameExportServices::Written, Error> {
            calls.push_back({state, sequence, time, file, format, options});
            std::ofstream(file, std::ios::binary) << "still";
            const Sequence* seq = state.findSequence(sequence);
            const int width = options.width > 0 ? options.width : seq ? seq->width : 0;
            const int height = options.height > 0 ? options.height : seq ? seq->height : 0;
            return command::FrameExportServices::Written{file, width, height, 5, format == "jpeg" ? (options.quality > 0 ? options.quality : 95) : 0};
        };
        command::FrameExportServices::Describer describer;
        if (canImport) {
            describer = [](const std::filesystem::path& file) -> std::expected<MediaItem, Error> {
                MediaItem item;
                item.name = pathToUtf8(file.filename());
                item.locator.path = pathToUtf8(file);
                VideoStreamInfo v;
                v.width = 1920;
                v.height = 1080;
                v.frameRate = Rate{25, 1};
                item.info.video.push_back(v);
                item.info.isStillImage = true;
                item.info.duration = Ticks{kTicksPerSecond};
                return item;
            };
        }
        return std::make_shared<command::FrameExportServices>(
            std::move(writer), std::move(describer),
            std::vector<std::string>{"dpx", "jpeg", "png", "tiff", "targa", "bmp", "gif", "openexr"});
    }
};

std::filesystem::path stillsDir(const char* name) {
    const std::filesystem::path dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "command-stills" / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE("sequence.exportFrame writes the frame through the host and can import it as one undo step", "[command][monitor]") {
    FakeFrames frames;
    command::EnginePolicy policy;
    policy.services = frames.services();
    CommandHarness h(kPicture, policy);
    const std::filesystem::path dir = stillsDir("sequence");
    const std::size_t media = h.state()->media.size();
    const auto undo = h.session.undoCount();

    // Without import the project does not change; the format's extension is added.
    const auto plain = h.ok("sequence.exportFrame",
                            Json{{"time", tc(4)}, {"format", "dpx"}, {"path", pathToUtf8(dir / "Frame.png")}});
    REQUIRE(frames.calls.size() == 1);
    CHECK(frames.calls[0].time == f(4));
    CHECK(frames.calls[0].format == "dpx");
    CHECK(frames.calls[0].file.filename() == "Frame.png.dpx");
    CHECK(plain.data.at("written") == true);
    CHECK_FALSE(plain.committed);
    CHECK(h.session.undoCount() == undo);
    CHECK(h.state()->media.size() == media);

    // "jpg" and "jpeg" are both JPEG's; Import into project adds a media item (one undo step).
    const auto imported = h.ok("sequence.exportFrame", Json{{"time", tc(2)}, {"format", "jpeg"},
                                                            {"path", pathToUtf8(dir / "Thumb.JPG")}, {"importIntoProject", true}});
    CHECK(frames.calls.back().file.filename() == "Thumb.JPG");
    CHECK(imported.committed);
    CHECK(h.session.undoCount() == undo + 1);
    REQUIRE(h.state()->media.size() == media + 1);
    const MediaId id{imported.data.at("media").get<std::uint64_t>()};
    REQUIRE(h.state()->findMedia(id));
    CHECK(h.state()->findMedia(id)->name == "Thumb.JPG");
    REQUIRE(h.session.undo());
    CHECK(h.state()->media.size() == media);

    // A dry run renders nothing.
    const auto calls = frames.calls.size();
    const auto dry = h.run(Json{{"command", "sequence.exportFrame"},
                                {"params", Json{{"time", tc(1)}, {"path", pathToUtf8(dir / "dry")}}},
                                {"dryRun", true}});
    REQUIRE(dry.ok());
    CHECK(frames.calls.size() == calls);
    CHECK(dry.data.at("path").get<std::string>().ends_with("dry.png")); // png by default

    CHECK(h.errorOf("sequence.exportFrame", Json{{"time", tc(1)}, {"format", "webp"}, {"path", pathToUtf8(dir / "x")}}) ==
          "InvalidParams");
    CHECK(h.errorOf("sequence.exportFrame", Json{{"time", tc(1)}, {"path", pathToUtf8(dir / "x")}, {"bin", 9999}}) ==
          "NotFound");
}

TEST_CASE("Export Frame needs a host renderer and an importer for Import into project", "[command][monitor]") {
    const std::filesystem::path dir = stillsDir("hosts");
    {
        CommandHarness h(kPicture);
        CHECK(h.errorOf("sequence.exportFrame", Json{{"time", tc(1)}, {"path", pathToUtf8(dir / "x")}}) == "InvalidRequest");
    }
    FakeFrames frames;
    command::EnginePolicy policy;
    policy.services = frames.services(false);
    CommandHarness h(kPicture, policy);
    CHECK(h.errorOf("sequence.exportFrame",
                    Json{{"time", tc(1)}, {"path", pathToUtf8(dir / "x")}, {"importIntoProject", true}}) == "InvalidRequest");
    h.ok("sequence.exportFrame", Json{{"time", tc(1)}, {"path", pathToUtf8(dir / "x")}});
}

TEST_CASE("source.exportFrame renders the media's frame at its own size and time", "[command][monitor]") {
    FakeFrames frames;
    command::EnginePolicy policy;
    policy.services = frames.services();
    CommandHarness h(kPicture, policy);
    const std::filesystem::path dir = stillsDir("source");
    const MediaId a = h.tl.media('A');
    h.ok("source.exportFrame", Json{{"media", a.value}, {"time", tc(7)}, {"format", "tiff"}, {"path", pathToUtf8(dir / "a")}});
    REQUIRE(frames.calls.size() == 1);
    const FakeFrames::Call& call = frames.calls.front();
    CHECK(call.file.filename() == "a.tif");
    CHECK(call.time == Ticks{});
    // A project of its own: one sequence of the media's size with the source frame at 0 on V1.
    const Sequence* seq = call.state.findSequence(call.sequence);
    REQUIRE(seq);
    CHECK(seq->width == 1920);
    CHECK(seq->height == 1080);
    REQUIRE_FALSE(seq->videoTracks.empty());
    REQUIRE(seq->videoTracks.front()->clips.size() == 1);
    const Clip& clip = *seq->videoTracks.front()->clips.begin();
    CHECK(clip.start() == Ticks{});
    CHECK(clip.sourceIn == f(7));
    // The project itself is untouched.
    CHECK_FALSE(h.session.canUndo());
    CHECK(h.errorOf("source.exportFrame", Json{{"media", 99999}, {"time", 0}, {"path", pathToUtf8(dir / "b")}}) == "NotFound");
}

// ---- P1 round (stream 1C, #23): Export Frame sizes and Export Frames at Markers ------------------------------------------

TEST_CASE("sequence.exportFrame passes a size a JPEG quality and a size cap to the host", "[command][monitor][p1]") {
    FakeFrames frames;
    command::EnginePolicy policy;
    policy.services = frames.services();
    CommandHarness h(kPicture, policy);
    const std::filesystem::path dir = stillsDir("sized");
    const auto r = h.ok("sequence.exportFrame",
                        Json{{"time", tc(3)}, {"format", "jpeg"}, {"path", pathToUtf8(dir / "Thumb")}, {"width", 1280},
                             {"height", 720}, {"fit", "fill"}, {"quality", 80}, {"maxBytes", 2000000}});
    REQUIRE(frames.calls.size() == 1);
    const command::FrameExportServices::Options& o = frames.calls[0].options;
    CHECK(o.width == 1280);
    CHECK(o.height == 720);
    CHECK(o.fill);
    CHECK(o.quality == 80);
    CHECK(o.maxBytes == 2000000);
    CHECK(r.data.at("width") == 1280);
    CHECK(r.data.at("height") == 720);
    CHECK(r.data.at("quality") == 80);
    CHECK(r.data.at("bytes") == 5);
    CHECK(r.data.at("overLimit") == false);
    // No size: the frame's own, fitted; a size needs both sides.
    h.ok("sequence.exportFrame", Json{{"time", tc(3)}, {"path", pathToUtf8(dir / "Plain")}});
    CHECK(frames.calls.back().options.width == 0);
    CHECK_FALSE(frames.calls.back().options.fill);
    CHECK(h.errorOf("sequence.exportFrame", Json{{"time", tc(3)}, {"path", pathToUtf8(dir / "x")}, {"width", 1280}}) ==
          "InvalidParams");
    CHECK(h.errorOf("sequence.exportFrame",
                    Json{{"time", tc(3)}, {"path", pathToUtf8(dir / "x")}, {"width", 8}, {"height", 8}}) == "InvalidParams");
}

TEST_CASE("sequence.exportFramesAtMarkers writes a still per marker named after it and imports them in one step",
          "[command][monitor][p1]") {
    FakeFrames frames;
    command::EnginePolicy policy;
    policy.services = frames.services();
    CommandHarness h(kPicture, policy);
    const std::filesystem::path dir = stillsDir("markers");
    h.ok("marker.add", Json{{"time", tc(1)}, {"name", "Clutch: 1v3"}, {"type", "chapter"}});
    h.ok("marker.add", Json{{"time", tc(4)}, {"name", "Clutch: 1v3"}, {"colorIndex", 1}});
    h.ok("marker.add", Json{{"time", tc(6)}});
    h.ok("marker.add", Json{{"time", tc(8)}, {"duration", tc(2)}, {"name", "Ace"},
                            {"highlight", Json{{"score", 0.9}, {"peak", tc(9)}, {"reasons", Json::array({"kills"})}}}});
    const auto undo = h.session.undoCount();

    // Every marker: one still each, named after it (a name used twice numbered; none: "Marker N"; ':' replaced).
    const auto all = h.ok("sequence.exportFramesAtMarkers",
                          Json{{"folder", pathToUtf8(dir)}, {"width", 1280}, {"height", 720}, {"fit", "fill"},
                               {"maxBytes", 2000000}});
    REQUIRE(frames.calls.size() == 4);
    CHECK(all.data.at("count") == 4);
    CHECK(frames.calls[0].file.filename() == "Clutch_ 1v3.jpg"); // jpeg by default
    CHECK(frames.calls[1].file.filename() == "Clutch_ 1v3 (2).jpg");
    CHECK(frames.calls[2].file.filename() == "Marker 3.jpg");
    CHECK(frames.calls[3].file.filename() == "Ace.jpg");
    CHECK(frames.calls[0].time == f(1));
    CHECK(frames.calls[3].time == f(9)); // a highlight's still is taken at its peak
    CHECK(frames.calls[0].options.width == 1280);
    CHECK(frames.calls[0].options.maxBytes == 2000000);
    CHECK_FALSE(all.committed);
    CHECK(h.session.undoCount() == undo);

    // The highlight markers, at their start; chapters; one colour.
    frames.calls.clear();
    h.ok("sequence.exportFramesAtMarkers", Json{{"folder", pathToUtf8(dir)}, {"markers", "highlights"}, {"at", "start"},
                                                {"format", "png"}});
    REQUIRE(frames.calls.size() == 1);
    CHECK(frames.calls[0].time == f(8));
    CHECK(frames.calls[0].file.filename() == "Ace.png");
    frames.calls.clear();
    h.ok("sequence.exportFramesAtMarkers", Json{{"folder", pathToUtf8(dir)}, {"markers", "chapters"}});
    CHECK(frames.calls.size() == 1);
    frames.calls.clear();
    h.ok("sequence.exportFramesAtMarkers", Json{{"folder", pathToUtf8(dir)}, {"colorIndex", 1}});
    REQUIRE(frames.calls.size() == 1);
    CHECK(frames.calls[0].time == f(4));

    // Import into project: a "Thumbnails" bin with the four stills, one undo step; undone in one.
    const std::size_t media = h.state()->media.size();
    const std::size_t bins = h.state()->bins.size();
    const auto imported = h.ok("sequence.exportFramesAtMarkers", Json{{"folder", pathToUtf8(dir)}, {"importIntoProject", true}});
    CHECK(imported.committed);
    CHECK(h.session.undoCount() == undo + 1);
    REQUIRE(h.state()->media.size() == media + 4);
    REQUIRE(h.state()->bins.size() == bins + 1);
    CHECK(h.state()->bins.back().name == "Thumbnails");
    const BinId thumbnails = h.state()->bins.back().id;
    CHECK(h.state()->media.back()->bin == thumbnails);
    CHECK(imported.data.at("media").size() == 4);
    // Again: the same bin.
    h.ok("sequence.exportFramesAtMarkers", Json{{"folder", pathToUtf8(dir)}, {"markers", "highlights"}, {"importIntoProject", true}});
    CHECK(h.state()->bins.size() == bins + 1);
    CHECK(h.state()->media.back()->bin == thumbnails);
    REQUIRE(h.session.undo());
    REQUIRE(h.session.undo());
    CHECK(h.state()->media.size() == media);
    CHECK(h.state()->bins.size() == bins);

    // A dry run lists the files only; no folder, no markers of a kind, no host: refused.
    const auto calls = frames.calls.size();
    const auto dry = h.run(Json{{"command", "sequence.exportFramesAtMarkers"},
                                {"params", Json{{"folder", pathToUtf8(dir)}}},
                                {"dryRun", true}});
    REQUIRE(dry.ok());
    CHECK(frames.calls.size() == calls);
    CHECK(dry.data.at("stills").size() == 4);
    CHECK(h.errorOf("sequence.exportFramesAtMarkers", Json{{"folder", pathToUtf8(dir / "missing")}}) == "NotFound");
    CommandHarness bare(kPicture, policy);
    CHECK(bare.errorOf("sequence.exportFramesAtMarkers", Json{{"folder", pathToUtf8(dir)}}) == "NotFound");
    CommandHarness hostless(kPicture);
    hostless.ok("marker.add", Json{{"time", tc(1)}});
    CHECK(hostless.errorOf("sequence.exportFramesAtMarkers", Json{{"folder", pathToUtf8(dir)}}) == "InvalidRequest");
}
