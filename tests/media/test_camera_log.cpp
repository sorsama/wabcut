// 0.2: camera log auto-detection (media/CameraLog.h) - log curves named in Sony's NonRealTimeMeta XML (in the file's
// metadata boxes or the clip's M01.XML sidecar) and in key / value metadata, read into VideoStreamInfo::logColorSpace
// by media::probe; untrusted bytes stay bounded.

#include "TestMedia.h"

#include <wabcut/media/CameraLog.h>
#include <wabcut/media/Probe.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

using namespace wabcut;
using namespace wabcut::media;
using namespace wabcut::media::test;

namespace {

constexpr std::string_view kSonyXml =
    R"(<?xml version="1.0" encoding="UTF-8"?>
<NonRealTimeMeta xmlns="urn:schemas-professionalDisc:nonRealTimeMeta:ver.2.20" lastUpdate="2026-05-01T10:00:00+09:00">
  <AcquisitionRecord>
    <Group name="CameraUnitMetadataSet">
      <Item name="CaptureGammaEquation" value="s-log3-cine"/>
      <Item name="CaptureColorPrimaries" value="s-gamut3-cine"/>
      <Item name="CodingEquations" value="rec709"/>
    </Group>
  </AcquisitionRecord>
</NonRealTimeMeta>)";

std::string box(std::string_view type, const std::string& body) {
    const auto size = static_cast<std::uint32_t>(body.size() + 8);
    std::string out;
    out.push_back(static_cast<char>(size >> 24));
    out.push_back(static_cast<char>(size >> 16 & 0xff));
    out.push_back(static_cast<char>(size >> 8 & 0xff));
    out.push_back(static_cast<char>(size & 0xff));
    out += type;
    out += body;
    return out;
}

void writeBytes(const std::filesystem::path& file, const std::string& bytes) {
    std::ofstream out(file, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

} // namespace

TEST_CASE("Camera log names map to input colour spaces", "[media][cameralog]") {
    CHECK(cameraLogFromName("s-log3-cine") == "s-log3-cine");
    CHECK(cameraLogFromName("s-log3", "s-gamut3-cine") == "s-log3-cine");
    CHECK(cameraLogFromName("S-Log3", "S-Gamut3") == "s-log3");
    CHECK(cameraLogFromName("V-Log") == "v-log");
    CHECK(cameraLogFromName("LOG-C") == "logc3");
    CHECK(cameraLogFromName("LogC4") == "logc4");
    CHECK(cameraLogFromName("Canon Log 3") == "canon-log3");
    CHECK(cameraLogFromName("C-Log2") == "canon-log2");
    CHECK(cameraLogFromName("REDLog3G10") == "red-log3g10");
    CHECK(cameraLogFromName("Apple Log") == "apple-log");
    CHECK(cameraLogFromName("rec709").empty());
    CHECK(cameraLogFromName("hlg").empty());
    CHECK(cameraLogFromName("").empty());
}

TEST_CASE("Sony's NonRealTimeMeta XML and key value tags name the camera log", "[media][cameralog]") {
    CHECK(cameraLogFromXml(kSonyXml) == "s-log3-cine");
    std::string plain(kSonyXml);
    plain.replace(plain.find("s-log3-cine"), 11, "rec709");
    CHECK(cameraLogFromXml(plain).empty());
    CHECK(cameraLogFromXml("<Item name=\"CaptureGammaEquation\"").empty()); // truncated: no value
    CHECK(cameraLogFromXml("no xml at all").empty());

    CHECK(cameraLogFromTag("com.arri.camera.ColorGammaSxS", "LOG-C") == "logc3");
    CHECK(cameraLogFromTag("gamma", "V-Log") == "v-log");
    CHECK(cameraLogFromTag("TransferCharacteristic", "Canon Log 3") == "canon-log3");
    CHECK(cameraLogFromTag("encoder", "Lavf61 LogC").empty()); // a key about something else
    CHECK(cameraLogFromTag("title", "S-Log3").empty());
}

TEST_CASE("The XML in an MP4's metadata boxes or the M01.XML sidecar names the log", "[media][cameralog]") {
    const auto dir = scratchDir("camera-log");
    // ftyp, then moov with a meta box carrying the XML, then the media data: only metadata boxes are read.
    const std::string ftyp = box("ftyp", std::string("isom\0\0\0\0isom", 12));
    const std::string moov = box("moov", box("mvhd", std::string(100, '\0')) + box("meta", std::string(4, '\0') + std::string(kSonyXml)));
    const std::string mdat = box("mdat", std::string(64, '\x55'));
    writeBytes(dir / "C0001.MP4", ftyp + moov + mdat);
    CHECK(cameraLogFromFile(dir / "C0001.MP4") == "s-log3-cine");

    // The same XML at the top level, in a "uuid" box after the media data.
    writeBytes(dir / "C0002.MP4", ftyp + mdat + box("uuid", std::string(16, '\x01') + std::string(kSonyXml)));
    CHECK(cameraLogFromFile(dir / "C0002.MP4") == "s-log3-cine");

    // XML inside the media data does not count (never read), nor does a file that is not MP4.
    writeBytes(dir / "C0003.MP4", ftyp + box("mdat", std::string(kSonyXml)));
    CHECK(cameraLogFromFile(dir / "C0003.MP4").empty());
    writeBytes(dir / "notes.txt", std::string(kSonyXml));
    CHECK(cameraLogFromFile(dir / "notes.txt").empty());

    // A box claiming more bytes than the file has: refused, no read past the end.
    std::string truncated = ftyp + box("moov", std::string(32, '\0'));
    truncated[ftyp.size() + 3] = '\x7f'; // the size says 127 bytes
    writeBytes(dir / "C0004.MP4", truncated);
    CHECK(cameraLogFromFile(dir / "C0004.MP4").empty());

    // Sony's sidecar next to a file without the XML.
    writeBytes(dir / "C0005.MP4", ftyp + mdat);
    CHECK(cameraLogFromFile(dir / "C0005.MP4").empty());
    writeBytes(dir / "C0005M01.XML", std::string(kSonyXml));
    CHECK(cameraLogFromFile(dir / "C0005.MP4") == "s-log3-cine");
}

TEST_CASE("probe reads the camera log of a clip with a Sony sidecar into its video stream", "[media][cameralog][probe]") {
    const std::string name = "h264_1080p60_aac.mp4";
    if (!haveMedia(name)) {
        SKIP("test clip " << name << " was not generated");
    }
    const auto dir = scratchDir("camera-log-probe");
    std::filesystem::copy_file(std::filesystem::path(WABCUT_TEST_MEDIA_DIR) / name, dir / "C0010.MP4");
    auto plain = probe((dir / "C0010.MP4").generic_string());
    REQUIRE(plain.has_value());
    REQUIRE_FALSE(plain->video.empty());
    CHECK(plain->video.front().logColorSpace.empty()); // nothing names a log: the stream's tags decide

    writeBytes(dir / "C0010M01.XML", std::string(kSonyXml));
    auto logged = probe((dir / "C0010.MP4").generic_string());
    REQUIRE(logged.has_value());
    CHECK(logged->video.front().logColorSpace == "s-log3-cine");
}
