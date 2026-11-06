// CaptureRecorder (0.7): pictures and sound pushed with chosen times become a file - constant frame rate, repeated pictures
// where the device skipped, sound lined up with the first picture and kept in step, a bounded queue, a file that survives the
// program stopping. No device: the pictures are the fake ones (a barcode with their number) at times the test decides.

#include "CaptureTestUtil.h"

#include <wabcut/capture/CaptureFiles.h>
#include <wabcut/capture/CaptureRecorder.h>
#include <wabcut/capture/FakeBackend.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <condition_variable>
#include <mutex>
#include <numbers>
#include <thread>

using namespace wabcut;
using namespace wabcut::capture;
using namespace capturetest;
namespace fs = std::filesystem;

namespace {

constexpr std::int64_t kBase = 5'000'000'000'000LL; // some steady-clock time
constexpr int kRate = 48000;

std::int64_t at(std::int64_t frame, Rate fps = Rate{30, 1}) {
    return kBase + static_cast<std::int64_t>(std::llround(static_cast<double>(frame) * 1e9 * fps.den / fps.num));
}

void pictures(CaptureRecorder& rec, std::int64_t from, std::int64_t to, Rate fps = Rate{30, 1}) {
    for (std::int64_t k = from; k < to; ++k) {
        rec.pushVideo(fakeFrame(320, 180, k, at(k, fps)));
    }
}

/// Sound heard from host time `startNs` for `seconds`, in blocks of `blockFrames`, device frames numbered from `firstFrame`.
/// `clock` scales the host time of a device frame (1.005: the device's clock is 0.5 % slow against the computer's).
void tone(CaptureRecorder& rec, std::int64_t startNs, double seconds, std::int64_t firstFrame = 0, double clock = 1.0,
          std::int64_t blockFrames = 480) {
    const auto total = static_cast<std::int64_t>(seconds * kRate);
    for (std::int64_t done = 0; done < total; done += blockFrames) {
        const std::int64_t n = std::min(blockFrames, total - done);
        const std::int64_t frame = firstFrame + done;
        const std::int64_t host = startNs + static_cast<std::int64_t>(static_cast<double>(done) * 1e9 / kRate * clock);
        rec.pushAudio(toneBlock(kRate, 2, frame, n, host));
    }
}

fs::path fileIn(const fs::path& dir, const char* name = "capture.mkv") {
    return dir / name;
}

} // namespace

TEST_CASE("Recorder: steady pictures and sound become one file of exactly those pictures", "[capture][recorder]") {
    const fs::path dir = freshOutputDir("steady");
    CaptureRecorder rec;
    REQUIRE(rec.start(smallConfig(fileIn(dir))));
    CHECK(rec.status().state == RecorderState::Waiting);
    pictures(rec, 0, 90);
    tone(rec, at(0), 3.0);
    const auto result = rec.stop();
    REQUIRE(result);
    CHECK(result->videoFrames == 90);
    CHECK(result->audioFrames == 3 * kRate);
    CHECK(result->framesDropped == 0);
    CHECK(result->seconds == 3.0);
    CHECK(result->bytes > 1000);
    CHECK(result->audio);
    CHECK(fs::exists(result->file));

    const Readback back = readCapture(result->file, true);
    REQUIRE(back.ok);
    INFO(back.error);
    CHECK(back.container.find("matroska") != std::string::npos);
    CHECK(back.videoCodec == "h264");
    CHECK(back.audioCodec == "pcm_s16le");
    CHECK(back.width == 320);
    CHECK(back.height == 180);
    CHECK(back.title == "Test capture");
    REQUIRE(back.barcodes.size() == 90);
    for (std::size_t i = 0; i < back.barcodes.size(); ++i) {
        REQUIRE(back.barcodes[i] == static_cast<std::int64_t>(i));
    }
    // Constant frame rate: the pictures are 1/30 s apart (Matroska keeps milliseconds).
    for (std::size_t i = 1; i < back.times.size(); ++i) {
        REQUIRE(std::abs(back.times[i] - back.times[i - 1] - 1.0 / 30.0) < 0.002);
    }
    CHECK(back.audioFrames == 3 * kRate);
    CHECK(back.audioChannels == 2);
    CHECK(back.audioRate == kRate);
    CHECK(rms(back.audio, 2, 0, 3 * kRate) > 0.15); // the 0.25 tone: 0.177
    // The journal went with a clean stop.
    CHECK_FALSE(readJournal(result->file));
    CHECK(rec.status().state == RecorderState::Idle);
    CHECK_FALSE(rec.active());
}

TEST_CASE("Recorder: a picture the device skipped is the last one repeated and counted", "[capture][recorder]") {
    const fs::path dir = freshOutputDir("gaps");
    CaptureRecorder rec;
    REQUIRE(rec.start(smallConfig(fileIn(dir), Rate{30, 1}, false)));
    pictures(rec, 0, 10);
    pictures(rec, 15, 45); // 10..14 never came
    const auto result = rec.stop();
    REQUIRE(result);
    CHECK(result->videoFrames == 45);
    CHECK(result->framesDropped == 5);
    const Readback back = readCapture(result->file);
    REQUIRE(back.ok);
    REQUIRE(back.barcodes.size() == 45);
    for (std::size_t i = 0; i < 10; ++i) {
        CHECK(back.barcodes[i] == static_cast<std::int64_t>(i));
    }
    for (std::size_t i = 10; i < 15; ++i) {
        CHECK(back.barcodes[i] == 9); // the last picture shown again
    }
    for (std::size_t i = 15; i < 45; ++i) {
        CHECK(back.barcodes[i] == static_cast<std::int64_t>(i));
    }
    CHECK_FALSE(back.audioChannels > 0); // a capture without sound has none
}

TEST_CASE("Recorder: pictures that arrive a little early or late keep their slot; a second picture for a slot is not written",
          "[capture][recorder]") {
    const fs::path dir = freshOutputDir("jitter");
    CaptureRecorder rec;
    REQUIRE(rec.start(smallConfig(fileIn(dir), Rate{30, 1}, false)));
    const std::int64_t frameNs = 33'333'333;
    for (std::int64_t k = 0; k < 60; ++k) {
        // +-0.3 of a frame, deterministic (the first picture sets the grid: it is on time).
        const double jitter = k == 0 ? 0.0 : (static_cast<double>((k * 37) % 61) / 60.0 - 0.5) * 0.6;
        rec.pushVideo(fakeFrame(320, 180, k, at(k) + static_cast<std::int64_t>(jitter * static_cast<double>(frameNs))));
        if (k == 20) { // a second picture a few milliseconds after picture 20
            rec.pushVideo(fakeFrame(320, 180, 999, at(k) + frameNs / 8));
        }
    }
    const auto result = rec.stop();
    REQUIRE(result);
    CHECK(result->videoFrames == 60);
    CHECK(result->framesDropped == 0);
    const Readback back = readCapture(result->file);
    REQUIRE(back.ok);
    REQUIRE(back.barcodes.size() == 60);
    for (std::size_t i = 0; i < back.barcodes.size(); ++i) {
        REQUIRE(back.barcodes[i] == static_cast<std::int64_t>(i));
    }
}

TEST_CASE("Recorder: NTSC rates keep their exact rate", "[capture][recorder]") {
    const fs::path dir = freshOutputDir("ntsc");
    CaptureRecorder rec;
    const Rate ntsc{10000000, 333667}; // what a device reports for 29.97
    REQUIRE(rec.start(smallConfig(fileIn(dir), ntsc, false)));
    for (std::int64_t k = 0; k < 90; ++k) {
        rec.pushVideo(fakeFrame(320, 180, k, at(k, Rate{30000, 1001})));
    }
    const auto result = rec.stop();
    REQUIRE(result);
    CHECK(result->videoFrames == 90);
    CHECK(result->framesDropped == 0);
    CHECK(std::abs(result->seconds - 90.0 * 1001.0 / 30000.0) < 1e-9);
    const Readback back = readCapture(result->file);
    REQUIRE(back.ok);
    CHECK(back.barcodes.size() == 90);
}

TEST_CASE("Recorder: sound that began before the first picture is trimmed, sound that began after it is preceded by silence",
          "[capture][recorder]") {
    const fs::path dir = freshOutputDir("align");
    SECTION("sound first") {
        CaptureRecorder rec;
        REQUIRE(rec.start(smallConfig(fileIn(dir))));
        tone(rec, at(0) - 200'000'000, 3.2); // heard from 0.2 s before the first picture
        pictures(rec, 0, 90);
        const auto result = rec.stop();
        REQUIRE(result);
        CHECK(std::abs(result->audioFrames - 3 * kRate) <= 480);
        const Readback back = readCapture(result->file, true);
        REQUIRE(back.ok);
        // The file's first sound is device frame 9600 (0.2 s in): the tone is in phase with where it was heard.
        const std::int64_t offset = 9600;
        double worst = 0.0;
        for (std::int64_t f = 0; f < 4800; ++f) {
            const double expected = 0.25 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(offset + f) / kRate);
            worst = std::max(worst, std::abs(static_cast<double>(back.audio[static_cast<std::size_t>(f) * 2]) - expected));
        }
        CHECK(worst < 0.001);
        CHECK(rec.status().audioTrimmed == 9600); // 0.2 s dropped from the front
        CHECK(rec.status().audioPadded == 0);
    }
    SECTION("sound after the picture") {
        CaptureRecorder rec;
        REQUIRE(rec.start(smallConfig(fileIn(dir))));
        pictures(rec, 0, 90);
        tone(rec, at(0) + 300'000'000, 2.7); // heard from 0.3 s after the first picture
        const auto result = rec.stop();
        REQUIRE(result);
        CHECK(std::abs(result->audioFrames - 3 * kRate) <= 480);
        const Readback back = readCapture(result->file, true);
        REQUIRE(back.ok);
        CHECK(rms(back.audio, 2, 0, 14000) < 0.001);          // 0.29 s of silence ...
        CHECK(rms(back.audio, 2, 15000, 40000) > 0.15);       // ... then the tone
    }
}

TEST_CASE("Recorder: sound the device lost is replaced by silence of the same length", "[capture][recorder]") {
    const fs::path dir = freshOutputDir("audiogap");
    CaptureRecorder rec;
    REQUIRE(rec.start(smallConfig(fileIn(dir))));
    pictures(rec, 0, 90);
    tone(rec, at(0), 1.0);                              // 0 .. 1 s
    tone(rec, at(0) + 1'200'000'000, 1.8, 57600);       // the device's frame count jumps by 9600 (0.2 s were lost)
    const auto result = rec.stop();
    REQUIRE(result);
    CHECK(std::abs(result->audioFrames - 3 * kRate) <= 480);
    const Readback back = readCapture(result->file, true);
    REQUIRE(back.ok);
    CHECK(rms(back.audio, 2, 48000 + 2000, 48000 + 7600) < 0.001); // the lost 0.2 s
    CHECK(rms(back.audio, 2, 24000, 48000) > 0.15);
    CHECK(rms(back.audio, 2, 60000, 100000) > 0.15);
}

TEST_CASE("Recorder: a sound device whose clock runs slow is brought back in step with the picture", "[capture][recorder]") {
    const fs::path dir = freshOutputDir("drift");
    CaptureRecorder rec;
    REQUIRE(rec.start(smallConfig(fileIn(dir))));
    pictures(rec, 0, 600); // 20 s
    // The device delivers 48000 frames per 1.005 s of the computer's time: after 20 s it is 0.1 s short.
    tone(rec, at(0), 20.0 / 1.005, 0, 1.005);
    const auto result = rec.stop();
    REQUIRE(result);
    CHECK(result->videoFrames == 600);
    CHECK(std::abs(result->audioFrames - 20 * kRate) < static_cast<std::int64_t>(0.06 * kRate)); // within one correction
    const Readback back = readCapture(result->file);
    REQUIRE(back.ok);
    CHECK(back.barcodes.size() == 600);
}

TEST_CASE("Recorder: a signal lost for ages does not stretch the file by more than 30 seconds", "[capture][recorder]") {
    const fs::path dir = freshOutputDir("longgap");
    CaptureRecorder rec;
    REQUIRE(rec.start(smallConfig(fileIn(dir), Rate{30, 1}, false)));
    pictures(rec, 0, 10);
    rec.pushVideo(fakeFrame(320, 180, 10, at(10) + 100'000'000'000LL)); // 100 s later
    const auto result = rec.stop();
    REQUIRE(result);
    CHECK(result->videoFrames > 10 + 29 * 30);
    CHECK(result->videoFrames < 10 + 32 * 30);
}

TEST_CASE("Recorder: a full queue drops pictures and counts them, the gap is filled and the capture goes on", "[capture][recorder]") {
    const fs::path dir = freshOutputDir("queue");
    std::mutex m;
    std::condition_variable cv;
    bool hold = true;
    std::atomic<bool> entered{false};
    RecorderConfig config = smallConfig(fileIn(dir), Rate{30, 1}, false);
    config.maxQueuedFrames = 4;
    config.encodeHook = [&] {
        entered = true;
        std::unique_lock lock(m);
        cv.wait(lock, [&] { return !hold; });
    };
    CaptureRecorder rec;
    REQUIRE(rec.start(config));
    pictures(rec, 0, 1);
    for (int i = 0; i < 2000 && !entered; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(entered); // the worker holds picture 0
    pictures(rec, 1, 5);  // the queue's four places
    pictures(rec, 5, 10); // no place left: lost
    CHECK(rec.status().framesLost == 5);
    {
        std::lock_guard lock(m);
        hold = false;
    }
    cv.notify_all();
    for (int i = 0; i < 5000 && rec.status().framesWritten < 5; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(rec.status().framesWritten == 5);
    pictures(rec, 10, 15); // the picture 5..9 slots were never filled
    const auto result = rec.stop();
    REQUIRE(result);
    CHECK(result->videoFrames == 15);
    CHECK(result->framesDropped == 5);
    const RecorderStatus after = rec.status();
    CHECK(after.framesLost == 5);
    const Readback back = readCapture(result->file);
    REQUIRE(back.ok);
    REQUIRE(back.barcodes.size() == 15);
    for (std::size_t i = 5; i < 10; ++i) {
        CHECK(back.barcodes[i] == 4);
    }
}

TEST_CASE("Recorder: a picture of another size is refused, not written", "[capture][recorder]") {
    const fs::path dir = freshOutputDir("rejected");
    CaptureRecorder rec;
    REQUIRE(rec.start(smallConfig(fileIn(dir), Rate{30, 1}, false)));
    pictures(rec, 0, 5);
    rec.pushVideo(fakeFrame(640, 360, 5, at(5)));
    pictures(rec, 6, 10);
    const auto result = rec.stop();
    REQUIRE(result);
    CHECK(result->videoFrames == 10); // 5 was repeated
    CHECK(result->framesDropped == 1);
}

TEST_CASE("Recorder: no picture means no file", "[capture][recorder]") {
    const fs::path dir = freshOutputDir("nopicture");
    CaptureRecorder rec;
    const fs::path file = fileIn(dir);
    REQUIRE(rec.start(smallConfig(file)));
    CHECK(fs::exists(file));
    CHECK(readJournal(file));
    tone(rec, at(0), 0.5);
    const auto result = rec.stop();
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::NotFound);
    CHECK_FALSE(fs::exists(file));
    CHECK_FALSE(readJournal(file));
}

TEST_CASE("Recorder: cancel deletes the file; a capture cannot start twice or at an odd size", "[capture][recorder]") {
    const fs::path dir = freshOutputDir("cancel");
    const fs::path file = fileIn(dir);
    CaptureRecorder rec;
    REQUIRE(rec.start(smallConfig(file)));
    const auto again = rec.start(smallConfig(fileIn(dir, "second.mkv")));
    REQUIRE_FALSE(again);
    CHECK(again.error().code == ErrorCode::InvalidArgument);
    pictures(rec, 0, 20);
    rec.cancel();
    CHECK_FALSE(fs::exists(file));
    CHECK_FALSE(readJournal(file));
    CHECK_FALSE(rec.active());

    CaptureRecorder odd;
    RecorderConfig config = smallConfig(fileIn(dir, "odd.mkv"));
    config.video.width = 321;
    const auto refused = odd.start(config);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == ErrorCode::InvalidArgument);
    CHECK_FALSE(fs::exists(fileIn(dir, "odd.mkv")));
}

TEST_CASE("Recorder: a capture the program stopped in the middle is repaired from what reached the disk", "[capture][recorder][recovery]") {
    const fs::path dir = freshOutputDir("recovery");
    const fs::path file = fileIn(dir, "Interrupted.mkv");
    {
        CaptureRecorder rec;
        RecorderConfig config = smallConfig(file);
        config.provenance = "project=demo\nbin=b7";
        REQUIRE(rec.start(config));
        pictures(rec, 0, 120); // 4 s
        tone(rec, at(0), 4.0);
        // Wait until the worker has written the pictures (it works in the background), then end as a crash would.
        for (int i = 0; i < 10000 && rec.status().framesWritten < 120; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE(rec.status().framesWritten == 120);
        rec.abandon();
        CHECK_FALSE(rec.active());
    }
    REQUIRE(fs::exists(file));
    const auto journal = readJournal(file);
    REQUIRE(journal);
    CHECK(journal->note == "project=demo\nbin=b7");

    const auto found = findInterruptedCaptures(dir);
    REQUIRE(found.size() == 1);
    CHECK(found[0].file == file);
    CHECK(found[0].bytes > 0);

    const auto recovered = recoverCapture(found[0]);
    REQUIRE(recovered);
    CHECK(recovered->file == file);
    CHECK(recovered->videoFrames >= 60);  // the clusters that were complete: all but the last second or so
    CHECK(recovered->videoFrames <= 120);
    CHECK(recovered->seconds > 1.5);
    CHECK_FALSE(readJournal(file));
    CHECK_FALSE(fs::exists(fs::path(file).concat(".recovering")));

    const Readback back = readCapture(file, true);
    REQUIRE(back.ok);
    INFO(back.error);
    CHECK(static_cast<std::int64_t>(back.barcodes.size()) == recovered->videoFrames);
    for (std::size_t i = 0; i < back.barcodes.size(); ++i) {
        REQUIRE(back.barcodes[i] == static_cast<std::int64_t>(i));
    }
    CHECK(back.audioFrames > 0);
    CHECK(back.title == "Test capture");
    CHECK(findInterruptedCaptures(dir).empty());
}

TEST_CASE("Recovery: a file with no picture is removed, not offered again", "[capture][recorder][recovery]") {
    const fs::path dir = freshOutputDir("recovery_empty");
    const fs::path file = fileIn(dir, "Nothing.mkv");
    {
        CaptureRecorder rec;
        REQUIRE(rec.start(smallConfig(file)));
        rec.abandon(); // before any picture
    }
    const auto found = findInterruptedCaptures(dir);
    REQUIRE(found.size() == 1);
    const auto recovered = recoverCapture(found[0]);
    REQUIRE_FALSE(recovered);
    CHECK(recovered.error().code == ErrorCode::NotFound);
    CHECK_FALSE(fs::exists(file));
    CHECK_FALSE(readJournal(file));
}

TEST_CASE("Recovery: a big file that cannot be opened is kept, a tiny one is removed", "[capture][recorder][recovery]") {
    const fs::path dir = freshOutputDir("recovery_unreadable");
    const fs::path big = fileIn(dir, "Big.mkv");
    {
        std::ofstream out(big, std::ios::binary);
        const std::string noise(200 * 1024, '\x07'); // not a container any demuxer knows
        out.write(noise.data(), static_cast<std::streamsize>(noise.size()));
    }
    REQUIRE(writeJournal(big, "interrupted"));
    const fs::path tiny = fileIn(dir, "Tiny.mkv");
    {
        std::ofstream out(tiny, std::ios::binary);
        out << "xx";
    }
    REQUIRE(writeJournal(tiny, "interrupted"));
    const auto found = findInterruptedCaptures(dir);
    REQUIRE(found.size() == 2);
    for (const InterruptedCapture& capture : found) {
        const auto recovered = recoverCapture(capture);
        REQUIRE_FALSE(recovered);
        if (capture.file.filename() == "Big.mkv") {
            CHECK(recovered.error().code == ErrorCode::IoError);
            CHECK(fs::exists(big)); // the only copy of a take stays
        } else {
            CHECK(recovered.error().code == ErrorCode::NotFound);
            CHECK_FALSE(fs::exists(tiny));
        }
    }
}
