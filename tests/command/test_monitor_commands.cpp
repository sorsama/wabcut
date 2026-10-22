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
    const Clip& c = clipIn(h.state(), h.tl.seq, clip);
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
    const Clip& u = clipIn(h.state(), h.tl.seq, clip);
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
    const Clip& c = clipIn(h.state(), h.tl.seq, clip);
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

namespace {

/// Export Frame services that record what they were asked and write a small file instead of rendering.
struct FakeFrames {
    struct Call {
        ProjectState state;
        SequenceId sequence;
        Ticks time{};
        std::filesystem::path file;
        std::string format;
    };
    std::vector<Call> calls;

    std::shared_ptr<command::FrameExportServices> services(bool canImport = true) {
        command::FrameExportServices::Writer writer =
            [this](const ProjectState& state, SequenceId sequence, Ticks time, const std::filesystem::path& file,
                   const std::string& format) -> std::expected<command::FrameExportServices::Written, Error> {
            calls.push_back({state, sequence, time, file, format});
            std::ofstream(file, std::ios::binary) << "still";
            const Sequence* seq = state.findSequence(sequence);
            return command::FrameExportServices::Written{file, seq ? seq->width : 0, seq ? seq->height : 0};
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
