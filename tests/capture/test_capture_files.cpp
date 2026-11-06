// Capture, the pure parts (0.7): file names, frame rates, the journal and what finds interrupted captures, the presets and
// the disk they take, the picture layouts and the fake pictures' barcode.

#include <wabcut/capture/CaptureFiles.h>
#include <wabcut/capture/CapturePresets.h>
#include <wabcut/capture/CaptureTypes.h>
#include <wabcut/capture/FakeBackend.h>
#include <wabcut/capture/Preview.h>

#include "CaptureTestUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

using namespace wabcut;
using namespace wabcut::capture;
namespace fs = std::filesystem;

namespace {

void touch(const fs::path& file, const std::string& text = "x") {
    std::ofstream out(file, std::ios::binary);
    out << text;
}

} // namespace

TEST_CASE("Capture names: a clip name becomes a safe file name", "[capture][files]") {
    CHECK(safeFileName("Interview 1") == "Interview 1");
    CHECK(safeFileName("  My: tape / take?*  ") == "My tape take");
    CHECK(safeFileName("a<b>c|d\"e") == "a b c d e");
    CHECK(safeFileName("Rec. ") == "Rec");
    CHECK(safeFileName("...") == "Capture");
    CHECK(safeFileName("") == "Capture");
    CHECK(safeFileName("CON") == "CON_");
    CHECK(safeFileName("nul") == "nul_");
    CHECK(safeFileName("COM3") == "COM3_");
    CHECK(safeFileName("COM10") == "COM10"); // only COM1..COM9 are devices
    CHECK(safeFileName("../../evil") == "evil");
    CHECK(safeFileName("tab\there") == "tab here");
    // UTF-8 is kept and a long name is cut between characters, not inside one.
    CHECK(safeFileName("Café") == "Café");
    std::string thai;
    for (int i = 0; i < 60; ++i) {
        thai += "ไทย";
    }
    const std::string cut = safeFileName(thai);
    CHECK(cut.size() <= 100);
    CHECK(cut.size() % 3 == 0); // three bytes a character
}

TEST_CASE("Capture names: the next free name in the Capture folder", "[capture][files]") {
    const fs::path dir = capturetest::freshOutputDir("names");
    CHECK(defaultClipName(dir) == "Capture 1");
    touch(dir / "Capture 1.mkv");
    CHECK(defaultClipName(dir) == "Capture 2");
    touch(journalPath(dir / "Capture 2.mkv")); // a capture still being written holds its name
    CHECK(defaultClipName(dir) == "Capture 3");
    CHECK(captureFilePath(dir, "Interview") == dir / "Interview.mkv");
    touch(dir / "Interview.mkv");
    CHECK(captureFilePath(dir, "Interview") == dir / "Interview (2).mkv");
    touch(dir / "Interview (2).mkv");
    CHECK(captureFilePath(dir, "Interview") == dir / "Interview (3).mkv");
    // A name with path characters stays in the folder.
    CHECK(captureFilePath(dir, "..\\..\\x").parent_path() == dir);
    CHECK(captureFilePath(dir, "a/b/c").parent_path() == dir);
}

TEST_CASE("Capture rates: devices' ratios snap to the standard rates", "[capture][files]") {
    CHECK(snapFrameRate(Rate{30, 1}) == Rate{30, 1});
    CHECK(snapFrameRate(Rate{10000000, 333333}) == Rate{30, 1});
    CHECK(snapFrameRate(Rate{30000, 1001}) == Rate{30000, 1001});
    CHECK(snapFrameRate(Rate{2997, 100}) == Rate{30000, 1001});
    CHECK(snapFrameRate(Rate{5994, 100}) == Rate{60000, 1001});
    CHECK(snapFrameRate(Rate{23976, 1000}) == Rate{24000, 1001});
    CHECK(snapFrameRate(Rate{25, 1}) == Rate{25, 1});
    CHECK(snapFrameRate(Rate{50, 1}) == Rate{50, 1});
    CHECK(snapFrameRate(Rate{15, 1}) == Rate{15, 1});
    CHECK(snapFrameRate(Rate{20, 1}) == Rate{20, 1});   // not a standard rate: as it is
    CHECK(snapFrameRate(Rate{40, 2}) == Rate{20, 1});   // reduced
    CHECK(snapFrameRate(Rate{0, 0}) == Rate{30, 1});    // unusable: 30
}

TEST_CASE("Capture journal: written, read back, removed; the writer's process is known", "[capture][files]") {
    const fs::path dir = capturetest::freshOutputDir("journal");
    const fs::path file = dir / "a.mkv";
    touch(file);
    REQUIRE(writeJournal(file, "project=x\nbin=b12"));
    const auto journal = readJournal(file);
    REQUIRE(journal);
    CHECK(journal->pid == currentProcessId());
    CHECK(journal->note == "project=x\nbin=b12");
    CHECK_FALSE(journal->startedAt.empty());
    CHECK(processIsRunning(journal->pid, journal->processStamp));
    removeJournal(file);
    CHECK_FALSE(readJournal(file));

    // Not a journal of ours, or too large: ignored.
    touch(journalPath(file), "something else");
    CHECK_FALSE(readJournal(file));
    touch(journalPath(file), std::string(100'000, 'a'));
    CHECK_FALSE(readJournal(file));
    removeJournal(file);
    // No such process.
    CHECK_FALSE(processIsRunning(0, 0));
    CHECK_FALSE(processIsRunning(-5, 0));
}

TEST_CASE("Capture journal: interrupted captures are found, a running one is not", "[capture][files]") {
    const fs::path dir = capturetest::freshOutputDir("interrupted");
    const fs::path mine = dir / "Mine.mkv";
    const fs::path dead = dir / "Dead.mkv";
    const fs::path orphan = dir / "Orphan.mkv"; // a journal without its file
    touch(mine, "data");
    touch(dead, "data data");
    REQUIRE(writeJournal(mine, "a"));
    REQUIRE(writeJournal(dead, "b"));
    REQUIRE(writeJournal(orphan, "c"));
    // Make `dead` the file of a process that is gone (the largest id a process can have is far below this).
    touch(journalPath(dead), "wabcut-capture-journal 1\npid 2000000000\nstamp 0\nstarted 2026-01-01T00:00:00Z\n\nb");

    // This process's capture that is not the active one counts as interrupted too (it was ended without being finished).
    auto found = findInterruptedCaptures(dir);
    REQUIRE(found.size() == 2);
    CHECK(found[0].file.filename() == "Dead.mkv");
    CHECK(found[0].bytes == 9);
    CHECK(found[0].journal.note == "b");
    CHECK(found[1].file.filename() == "Mine.mkv");

    // The one being written right now is not.
    found = findInterruptedCaptures(dir, {mine});
    REQUIRE(found.size() == 1);
    CHECK(found[0].file.filename() == "Dead.mkv");

    CHECK(findInterruptedCaptures(dir / "nowhere").empty());
}

TEST_CASE("Capture presets: three, with the disk a capture takes", "[capture][files]") {
    const auto& presets = capturePresets();
    REQUIRE(presets.size() == 3);
    CHECK(presets[0].id == "high");
    CHECK(presets[1].id == "compact");
    CHECK(presets[2].id == "intra");
    CHECK(&capturePreset("nope") == &presets[0]); // an unknown id: the first
    CHECK(&capturePreset("intra") == &presets[2]);
    CHECK(presets[0].video.codec == exporter::VideoCodec::H264);
    CHECK(presets[2].video.codec == exporter::VideoCodec::ProRes);
    for (const CapturePreset& p : presets) {
        INFO(p.id);
        CHECK(p.container == exporter::Container::Mkv); // the container that survives the program stopping
        CHECK(p.audio.codec == exporter::AudioCodec::Pcm16);
        CHECK(p.video.keyframeInterval.value <= Ticks::fromSeconds(2).value);
    }
    VideoFormat hd;
    hd.width = 1920;
    hd.height = 1080;
    hd.frameRate = Rate{30, 1};
    const std::int64_t high = estimateBytesPerSecond(presets[0], hd, true, 2, 48000);
    const std::int64_t compact = estimateBytesPerSecond(presets[1], hd, true, 2, 48000);
    const std::int64_t intra = estimateBytesPerSecond(presets[2], hd, true, 2, 48000);
    CHECK(high > 1'000'000);
    CHECK(high < 3'000'000); // about 1.6 MB/s
    CHECK(compact < high);
    CHECK(intra > 15'000'000); // ProRes 422 at 1080p30: about 150 Mb/s
    CHECK(intra < 25'000'000);
    // Sound only adds its own bytes (16-bit stereo 48 kHz = 192 000 B/s).
    CHECK(estimateBytesPerSecond(presets[0], hd, true, 2, 48000) - estimateBytesPerSecond(presets[0], hd, false, 2, 48000) == 192'000);
    CHECK(estimateBytesPerSecond(presets[0], VideoFormat{}, false, 2, 48000) == 0);

    CHECK(durationText(-1) == "");
    CHECK(durationText(42.9) == "42 s");
    CHECK(durationText(90) == "1 min");
    CHECK(durationText(12 * 60) == "12 min");
    CHECK(durationText(3600) == "1 h");
    CHECK(durationText(3600 + 20 * 60) == "1 h 20 min");
    CHECK(durationText(1000.0 * 3600) == "1000 h");
}

TEST_CASE("Capture formats: layouts, labels, bytes", "[capture][files]") {
    CHECK(frameBytes(PixelFormat::Nv12, 1920, 1080) == 1920u * 1080u * 3 / 2);
    CHECK(frameBytes(PixelFormat::Yuy2, 720, 480) == 720u * 480u * 2);
    CHECK(frameBytes(PixelFormat::I420, 640, 360) == 640u * 360u * 3 / 2);
    CHECK(frameBytes(PixelFormat::P010, 1280, 720) == 1280u * 720u * 3);
    CHECK(frameBytes(PixelFormat::Bgr24, 100, 50) == 100u * 50u * 3);
    CHECK(frameBytes(PixelFormat::Bgra, 100, 50) == 100u * 50u * 4);
    CHECK(frameBytes(PixelFormat::Nv12, 0, 10) == 0);
    CHECK(planeCount(PixelFormat::I420) == 3);
    CHECK(planeCount(PixelFormat::Nv12) == 2);
    CHECK(planeCount(PixelFormat::Yuy2) == 1);
    CHECK(pixelFormatName(PixelFormat::Yuy2) == "YUY2");

    VideoFormat f;
    f.width = 1920;
    f.height = 1080;
    f.frameRate = Rate{60000, 1001};
    f.nativeName = "MJPG";
    CHECK(f.label() == "1920 x 1080 · 59.94 fps · MJPG");
    f.frameRate = Rate{30, 1};
    f.nativeName = "NV12";
    f.interlaced = true;
    CHECK(f.label() == "1920 x 1080 · 30 fps · NV12 · interlaced");
    f.frameRate = Rate{24000, 1001};
    f.interlaced = false;
    CHECK(f.label() == "1920 x 1080 · 23.976 fps · NV12");

    // A frame's planes follow each other.
    const CaptureFrame frame = makeFrame(PixelFormat::Nv12, 64, 32, 5, [](std::uint8_t* b) { b[64 * 32] = 7; });
    REQUIRE(frame.valid());
    CHECK(frame.plane(1) == frame.plane(0) + 64 * 32);
    CHECK(frame.plane(1)[0] == 7);
    CHECK(frame.plane(2) == nullptr);
    CHECK(frame.stride(0) == 64);
    CHECK_FALSE(CaptureFrame{}.valid());
}

TEST_CASE("Fake pictures carry their number in a barcode every layout reads back", "[capture][files]") {
    for (const PixelFormat pixel : {PixelFormat::Nv12, PixelFormat::Yuy2, PixelFormat::Bgra}) {
        for (const std::int64_t index : {0, 1, 255, 4660, 65535}) {
            INFO(pixelFormatName(pixel) << " " << index);
            const CaptureFrame frame = makeFrame(pixel, 320, 180, 0, [&](std::uint8_t* bytes) { drawFakePicture(pixel, 320, 180, index, bytes); });
            CHECK(fakePictureNumber(pixel, 320, 180, frame.data->data()) == index);
        }
    }
    // Pictures differ from one to the next apart from the code (so a repeated picture can be told from a new one by its number).
    const CaptureFrame a = makeFrame(PixelFormat::Nv12, 320, 180, 0, [](std::uint8_t* b) { drawFakePicture(PixelFormat::Nv12, 320, 180, 3, b); });
    const CaptureFrame b = makeFrame(PixelFormat::Nv12, 320, 180, 0, [](std::uint8_t* bytes) { drawFakePicture(PixelFormat::Nv12, 320, 180, 4, bytes); });
    CHECK(*a.data != *b.data);
}

TEST_CASE("Capture preview: a picture is shown in its own shape, no wider than asked", "[capture][files]") {
    const CaptureFrame hd = makeFrame(PixelFormat::Nv12, 1280, 720, 0, [](std::uint8_t* b) { drawFakePicture(PixelFormat::Nv12, 1280, 720, 9, b); });
    auto preview = makePreview(hd, Rate{1, 1}, 640);
    REQUIRE(preview);
    CHECK(preview->width == 640);
    CHECK(preview->height == 360);
    CHECK(preview->rgba.size() == 640u * 360u * 4);
    CHECK(preview->rgba[3] == 255); // opaque

    // Never enlarged.
    preview = makePreview(hd, Rate{1, 1}, 4000);
    REQUIRE(preview);
    CHECK(preview->width == 1280);

    // DV: 720 x 480 with 10:11 pixels is about 654 wide.
    const CaptureFrame dv = makeFrame(PixelFormat::Yuy2, 720, 480, 0, [](std::uint8_t* b) { drawFakePicture(PixelFormat::Yuy2, 720, 480, 1, b); });
    preview = makePreview(dv, Rate{10, 11}, 4000);
    REQUIRE(preview);
    CHECK(preview->width == 654);
    CHECK(preview->height == 480);

    CHECK_FALSE(makePreview(CaptureFrame{}, Rate{1, 1}, 640));
    CHECK_FALSE(makePreview(hd, Rate{1, 1}, 1));
}
