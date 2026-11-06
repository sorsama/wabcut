// 0.7 (card #130): the data tracks of a media file - the camera's gyro rides in them. A small MP4 with one 'gpmd' track built
// here byte by byte (and carrying a real GoPro payload from the gpmf-parser repository, MIT / Apache-2.0) is listed, read
// packet by packet with its times, cut short on request, cancelled between packets, refused for a stream that is not data,
// and survives being cut in half; the packets then parse into the gyro track the stabilizer reads.

#include "DataTrackMp4.h"

#include <wabcut/gyro/Gyro.h>
#include <wabcut/media/CameraLog.h>
#include <wabcut/media/DataStreams.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>

using namespace wabcut;
using namespace wabcut::media;
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
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "media" / "data-streams";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string path = (dir / name).string();
    REQUIRE(writeBytes(path, bytes));
    return path;
}

} // namespace

TEST_CASE("Data streams: a gpmd track is listed and its packets read with their times", "[media][datastreams]") {
    const Mp4Bytes payload = goProPayload();
    const std::string path = fileOf("gopro.mp4", dataTrackMp4("gpmd", "GoPro MET", {payload, payload, payload}));
    const auto streams = listDataStreams(path);
    REQUIRE(streams);
    REQUIRE(streams->size() == 1);
    CHECK((*streams)[0].tag == "gpmd");
    CHECK((*streams)[0].handler == "GoPro MET");
    CHECK((*streams)[0].index == 0);

    const auto packets = readDataPackets(path, (*streams)[0].index);
    REQUIRE(packets);
    REQUIRE(packets->size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK((*packets)[i].seconds == Approx(static_cast<double>(i)).margin(1e-6));
        CHECK((*packets)[i].duration == Approx(1.0).margin(1e-6));
        CHECK((*packets)[i].bytes == payload);
    }

    // The packets are what the gyro parser reads: three seconds of readings at the camera's 399 per second.
    std::vector<gyro::Packet> views;
    for (const DataPacket& p : *packets) {
        views.push_back({p.seconds, p.duration, std::span<const std::uint8_t>(p.bytes)});
    }
    const auto track = gyro::parseGpmf(views);
    REQUIRE(track);
    CHECK(track->times.size() == 3 * 399);
    CHECK(track->times.front() == Approx(0.0).margin(1e-9));
    CHECK(track->times.back() == Approx(3.0 - 1.0 / 399.0).margin(1e-4));
    CHECK(track->rateHz == Approx(399.0).margin(0.5));
}

TEST_CASE("Data streams: a quick look reads the first packets only; a stop request ends the read", "[media][datastreams]") {
    const Mp4Bytes payload = goProPayload();
    const std::string path = fileOf("gopro-five.mp4", dataTrackMp4("gpmd", "GoPro MET", {payload, payload, payload, payload, payload}));
    DataReadOptions quick;
    quick.firstPackets = 2;
    const auto few = readDataPackets(path, 0, quick);
    REQUIRE(few);
    CHECK(few->size() == 2);

    DataReadOptions limited;
    limited.maxPackets = 3;
    const auto capped = readDataPackets(path, 0, limited);
    REQUIRE(capped);
    CHECK(capped->size() == 3);

    DataReadOptions tinyBudget;
    tinyBudget.maxBytes = payload.size() + 10; // room for one packet
    const auto bytesCapped = readDataPackets(path, 0, tinyBudget);
    REQUIRE(bytesCapped);
    CHECK(bytesCapped->size() == 1);

    DataReadOptions stop;
    stop.cancelled = [] { return true; };
    const auto cancelled = readDataPackets(path, 0, stop);
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == ErrorCode::Cancelled);
}

TEST_CASE("Data streams: bad requests and damaged files are answered with errors, not crashes", "[media][datastreams][security]") {
    const Mp4Bytes payload = goProPayload();
    const Mp4Bytes whole = dataTrackMp4("gpmd", "GoPro MET", {payload, payload});
    const std::string path = fileOf("gopro-two.mp4", whole);
    CHECK(readDataPackets(path, 7).error().code == ErrorCode::InvalidArgument); // no such stream
    CHECK(readDataPackets(path, -1).error().code == ErrorCode::InvalidArgument);
    CHECK_FALSE(listDataStreams((fs::path(WABCUT_TEST_OUTPUT_DIR) / "media" / "data-streams" / "missing.mp4").string()));

    // The file cut at several lengths: an error or whatever packets are whole, never more than were written.
    for (const std::size_t length : {std::size_t{0}, std::size_t{20}, whole.size() / 3, whole.size() / 2, whole.size() - 100, whole.size() - 1}) {
        const std::string cut = fileOf("cut.mp4", Mp4Bytes(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(length)));
        const auto streams = listDataStreams(cut);
        if (!streams || streams->empty()) {
            continue;
        }
        const auto packets = readDataPackets(cut, (*streams)[0].index);
        if (packets) {
            CHECK(packets->size() <= 2);
        }
    }
    // Garbage with an .mp4 name.
    const std::string junk = fileOf("junk.mp4", Mp4Bytes(4096, 0x5A));
    CHECK_FALSE(listDataStreams(junk));
}

TEST_CASE("Data streams: other tracks are told apart by their sample entry", "[media][datastreams]") {
    const std::string dji = fileOf("dji.mp4", dataTrackMp4("djmd", "DJI meta", {Mp4Bytes(64, 1), Mp4Bytes(64, 2)}));
    const auto streams = listDataStreams(dji);
    REQUIRE(streams);
    REQUIRE(streams->size() == 1);
    CHECK((*streams)[0].tag == "djmd");
    CHECK(gyro::classifyDataTrack((*streams)[0].tag, (*streams)[0].handler) == gyro::TrackKind::DjiMetadata);
    const std::string sony = fileOf("sony.mp4", dataTrackMp4("rtmd", "Real Time Metadata", {Mp4Bytes(64, 1)}));
    const auto sonyStreams = listDataStreams(sony);
    REQUIRE(sonyStreams);
    REQUIRE(sonyStreams->size() == 1);
    CHECK(gyro::classifyDataTrack((*sonyStreams)[0].tag, (*sonyStreams)[0].handler) == gyro::TrackKind::SonyRtmd);
}

TEST_CASE("Camera model: Sony's NonRealTimeMeta XML in a metadata box names the camera", "[media][datastreams]") {
    // An MP4 whose moov holds a 'meta'-less 'udta' with an XML box, as Sony's cameras write it.
    using namespace wabcut::test::mp4;
    Mp4Bytes xml;
    text(xml, "<NonRealTimeMeta><Device manufacturer=\"Sony\" modelName=\"ILCE-7M4\" serialNo=\"1\"/>"
              "<Lens modelName=\"FE 24mm\"/></NonRealTimeMeta>");
    const Mp4Bytes file = join({box("ftyp", [] { Mp4Bytes b; text(b, "mp42"); u32(b, 0); text(b, "mp42"); return b; }()),
                                box("moov", box("udta", box("xml ", xml))), box("mdat", Mp4Bytes(16, 0))});
    const std::string path = fileOf("sony-xml.mp4", file);
    CHECK(cameraModelFromFile(fs::path(path)) == "ILCE-7M4");
    CHECK(cameraModelFromFile(fs::path(fileOf("none.mp4", dataTrackMp4("gpmd", "GoPro MET", {Mp4Bytes(8, 0)})))).empty());
}
