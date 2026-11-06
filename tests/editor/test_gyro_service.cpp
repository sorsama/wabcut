// 0.7 (card #130): the editor's view of a clip's camera gyro - probeGyro() for the Effect Controls note and the Method menu,
// MediaGyroService for the analysis - on small MP4 files with one data track built here (../media/DataTrackMp4.h): a GoPro file
// (a real GPMF payload from the gpmf-parser repository) is readable, a DJI file is recognised and refused with DJI's reason, a
// file without a data track says so, and RED / Blackmagic RAW / Insta360 files are refused by name with the maker's reason.

#include "../media/DataTrackMp4.h"

#include <wabcut/editor/Gyro.h>
#include <wabcut/model/ProjectState.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>

using namespace wabcut;
using namespace wabcut::editor;
using namespace wabcut::test;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {

Mp4Bytes goProPayload() {
    std::ifstream in(std::string(WABCUT_TEST_DATA_DIR) + "/gpmf/hero5.raw", std::ios::binary);
    REQUIRE(in);
    return Mp4Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string fileOf(const char* name, const Mp4Bytes& bytes) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "editor" / "gyro";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string path = (dir / name).string();
    REQUIRE(writeBytes(path, bytes));
    return path;
}

/// A project whose one media item is the file at `path` (one video stream).
struct OneClip {
    ProjectState state;
    MediaId media{1};
    explicit OneClip(const std::string& path) {
        auto item = std::make_shared<MediaItem>();
        item->id = media;
        item->name = "clip.mp4";
        item->locator.path = path;
        item->info.path = path;
        item->info.video.push_back(VideoStreamInfo{});
        state.media.push_back(std::move(item));
    }
};

} // namespace

TEST_CASE("Gyro service: a GoPro file is probed (camera, rate) and read whole for the analysis", "[editor][gyro]") {
    const Mp4Bytes payload = goProPayload();
    const std::string path = fileOf("gopro.mp4", dataTrackMp4("gpmd", "GoPro MET", {payload, payload, payload}));
    const GyroProbe probe = probeGyro(path);
    INFO(probe.reason);
    REQUIRE(probe.state == GyroProbe::State::Readable);
    CHECK(probe.make == "GoPro");
    CHECK(probe.format == "GoPro GPMF");
    CHECK(probe.rateHz == Approx(399.0).margin(1.0));
    CHECK(probe.axesChecked);

    OneClip tl(path);
    MediaGyroService service;
    const auto track = service.read(tl.state, tl.media, 0);
    REQUIRE(track);
    CHECK(track->times.size() == 3 * 399);
    CHECK(track->camera == gyro::Camera::GoPro);

    // The service stops when asked (the analysis' Cancel).
    MediaGyroService cancelled({}, [] { return true; });
    const auto stopped = cancelled.read(tl.state, tl.media, 0);
    REQUIRE_FALSE(stopped);
    CHECK(stopped.error().code == ErrorCode::Cancelled);
    // A stream or a media item that is not there.
    CHECK(service.read(tl.state, tl.media, 5).error().code == ErrorCode::NotFound);
    CHECK(service.read(tl.state, MediaId{987654}, 0).error().code == ErrorCode::NotFound);
}

TEST_CASE("Gyro service: files with gyro data Wabcut does not read, and files with none, say why", "[editor][gyro]") {
    // DJI: recognised, not read.
    const std::string dji = fileOf("dji.mp4", dataTrackMp4("djmd", "DJI meta", {Mp4Bytes(64, 1), Mp4Bytes(64, 2)}));
    const GyroProbe probe = probeGyro(dji);
    CHECK(probe.state == GyroProbe::State::Unreadable);
    CHECK(probe.reason.find("DJI") != std::string::npos);
    CHECK(probe.reason.find("not published") != std::string::npos);
    OneClip tl(dji);
    const auto refused = MediaGyroService().read(tl.state, tl.media, 0);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == ErrorCode::NotFound);
    CHECK(refused.error().message.find("DJI") != std::string::npos);

    // A data track that is no telemetry: no gyro.
    const std::string timecode = fileOf("timecode.mp4", dataTrackMp4("tmcd", "Timecode", {Mp4Bytes(4, 0)}));
    const GyroProbe none = probeGyro(timecode);
    CHECK(none.state == GyroProbe::State::None);
    CHECK(none.reason.find("no camera gyro data") != std::string::npos);

    // A GoPro track that holds no gyroscope (an older camera): unreadable, with the parser's reason.
    const std::string empty = fileOf("empty.mp4", dataTrackMp4("gpmd", "GoPro MET", {Mp4Bytes(16, 0)}));
    const GyroProbe bare = probeGyro(empty);
    CHECK(bare.state == GyroProbe::State::Unreadable);
    CHECK(bare.reason.find("no gyroscope") != std::string::npos);

    // Files FFmpeg cannot open (RED, Blackmagic RAW, Insta360) are refused by their name, with the maker's reason, without
    // even being opened.
    const GyroProbe red = probeGyro("C:/footage/A001_C002.R3D");
    CHECK(red.state == GyroProbe::State::Unreadable);
    CHECK(red.reason.find("RED") != std::string::npos);
    CHECK(probeGyro("clip.braw").reason.find("Blackmagic") != std::string::npos);
    CHECK(probeGyro("VID_20260101.insv").reason.find("Insta360") != std::string::npos);
    // A file that is not there.
    const GyroProbe missing = probeGyro((fs::path(WABCUT_TEST_OUTPUT_DIR) / "editor" / "gyro" / "nothing.mp4").string());
    CHECK(missing.state == GyroProbe::State::None);
    CHECK_FALSE(missing.reason.empty());
}
