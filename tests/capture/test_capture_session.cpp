// CaptureSession over the fake backend (0.7): what the Capture panel drives, in real time and without hardware. Picking a device
// opens nothing, the preview opens exactly the devices asked for, Record writes a file that decodes picture by picture with the
// sound in step, the system's refusals and an unplugged device are reported, and a stopped program leaves a file that repairs.

#include "CaptureTestUtil.h"

#include <wabcut/capture/CaptureBackend.h>
#include <wabcut/capture/CaptureFiles.h>
#include <wabcut/capture/CaptureSession.h>
#include <wabcut/capture/FakeBackend.h>
#include <wabcut/capture/Preview.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <functional>
#include <thread>

using namespace wabcut;
using namespace wabcut::capture;
using namespace capturetest;
namespace fs = std::filesystem;

namespace {

bool waitFor(const std::function<bool()>& done, int timeoutMs = 5000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

/// The fake HDMI card at 640 x 360, YUY2, 30 fps (small enough to encode with a software encoder in real time anywhere).
SessionConfig hdmiConfig(bool audio = true) {
    SessionConfig config;
    config.videoDeviceId = "fake:hdmi";
    config.format = FakeCaptureBackend::defaults().video[0].formats[3];
    config.audio = audio;
    return config;
}

RecordingRequest request(const fs::path& file, const char* preset = "high") {
    RecordingRequest r;
    r.file = file;
    r.presetId = preset;
    r.encoder = std::string(preset) == "intra" ? "prores_ks" : "libx264";
    r.title = "Session capture";
    r.provenance = "bin=b3";
    return r;
}

} // namespace

TEST_CASE("Session: listing devices and picking one opens nothing, the preview opens only what was asked for", "[capture][session]") {
    auto backend = std::make_shared<FakeCaptureBackend>();
    CaptureSession session(backend);

    const auto devices = backend->videoDevices();
    REQUIRE(devices);
    REQUIRE(devices->size() == 2);
    CHECK((*devices)[0].name == "Fake HDMI Capture");
    CHECK((*devices)[0].formats.empty()); // formats are asked for when the device is picked
    REQUIRE(backend->audioDevices());
    CHECK(backend->videoOpens() == 0);
    CHECK(backend->audioOpens() == 0);

    const auto formats = backend->videoFormats("fake:hdmi");
    REQUIRE(formats);
    CHECK(formats->size() == 4);
    CHECK(backend->formatQueries() == 1);
    CHECK(backend->videoOpens() == 0); // reading the formats is not capturing
    CHECK_FALSE(session.previewing());

    REQUIRE(session.startPreview(hdmiConfig()));
    CHECK(backend->videoOpens() == 1);
    CHECK(backend->audioOpens() == 1);
    CHECK(session.previewing());
    REQUIRE(waitFor([&] { return session.status().pictures >= 8; }));
    const SessionStatus status = session.status();
    CHECK(status.previewing);
    CHECK_FALSE(status.recording);
    CHECK(status.videoDescription == "Fake HDMI Capture");
    CHECK(status.audioDescription == "Fake Microphone");
    CHECK(status.format.width == 640);
    CHECK(status.failure.empty());
    const CaptureFrame frame = session.latestFrame();
    REQUIRE(frame.valid());
    CHECK(frame.width == 640);
    auto preview = makePreview(frame, status.format.pixelAspect, 320);
    REQUIRE(preview);
    CHECK(preview->width == 320);
    CHECK(preview->height == 180);
    REQUIRE(waitFor([&] { return session.status().measuredFps > 0.0; }, 3000));
    CHECK(session.status().measuredFps > 15.0);
    CHECK(session.status().measuredFps < 45.0);
    // The newest picture changes.
    const std::uint64_t serial = session.status().previewSerial;
    REQUIRE(waitFor([&] { return session.status().previewSerial > serial; }));
    // The sound meter moves.
    REQUIRE(waitFor([&] { return session.status().audioPeak > 0.1f; }));

    // Starting twice is refused; stopping closes everything.
    CHECK_FALSE(session.startPreview(hdmiConfig()));
    session.stopPreview();
    CHECK_FALSE(session.previewing());
    CHECK_FALSE(session.latestFrame().valid());
    const std::uint64_t after = session.status().pictures;
    sleepMs(100);
    CHECK(session.status().pictures == after); // nothing arrives any more
}

TEST_CASE("Session: Record writes a file whose pictures decode in order with the sound in step", "[capture][session]") {
    const fs::path dir = freshOutputDir("session_record");
    auto backend = std::make_shared<FakeCaptureBackend>();
    CaptureSession session(backend);
    CHECK_FALSE(session.startRecording(request(dir / "x.mkv"))); // no preview yet
    REQUIRE(session.startPreview(hdmiConfig()));
    REQUIRE(waitFor([&] { return session.status().pictures >= 3; }));

    const fs::path file = dir / "Take 1.mkv";
    REQUIRE(session.startRecording(request(file)));
    CHECK(session.recording());
    CHECK(session.recordingFile() == file);
    CHECK_FALSE(session.startRecording(request(dir / "other.mkv")));
    CHECK(fs::exists(journalPath(file)));
    REQUIRE(waitFor([&] { return session.status().recorder.framesWritten >= 20; }));
    const SessionStatus mid = session.status();
    CHECK(mid.recording);
    CHECK(mid.recorder.state == RecorderState::Recording);
    CHECK(mid.recorder.seconds > 0.3);
    CHECK_FALSE(mid.recorder.encoder.empty());
    sleepMs(500);

    const auto result = session.stopRecording();
    REQUIRE(result);
    CHECK_FALSE(session.recording());
    CHECK(session.previewing()); // the live view carries on
    CHECK(result->videoFrames >= 25);
    CHECK(result->framesDropped == 0);
    CHECK(result->encoder == "libx264");
    CHECK(fs::exists(file));
    CHECK_FALSE(fs::exists(journalPath(file)));
    CHECK_FALSE(session.stopRecording()); // nothing to stop

    const Readback back = readCapture(file, true);
    REQUIRE(back.ok);
    INFO(back.error);
    REQUIRE(static_cast<std::int64_t>(back.barcodes.size()) == result->videoFrames);
    for (std::size_t i = 1; i < back.barcodes.size(); ++i) {
        // Consecutive pictures of the source: nothing repeated, nothing skipped.
        REQUIRE(back.barcodes[i] - back.barcodes[i - 1] == 1);
    }
    CHECK(back.width == 640);
    CHECK(back.height == 360);
    const double videoSeconds = static_cast<double>(back.barcodes.size()) / 30.0;
    const double audioSeconds = static_cast<double>(back.audioFrames) / back.audioRate;
    CHECK(std::abs(audioSeconds - videoSeconds) < 0.12); // the sound ends where the picture ends
    CHECK(rms(back.audio, back.audioChannels, 0, back.audioFrames) > 0.15);
    // The recorded sound has no silent stretch at the front (alignment trimmed what came before the first picture).
    CHECK(rms(back.audio, back.audioChannels, 0, 2400) > 0.1);
    CHECK(back.title == "Session capture");
}

TEST_CASE("Session: a deck's pixel shape is kept; the intra preset writes ProRes", "[capture][session]") {
    const fs::path dir = freshOutputDir("session_formats");
    auto backend = std::make_shared<FakeCaptureBackend>();
    CaptureSession session(backend);
    SessionConfig config;
    config.videoDeviceId = "fake:deck";
    config.format = FakeCaptureBackend::defaults().video[1].formats[0];
    config.audio = false;
    REQUIRE(session.startPreview(config));
    REQUIRE(waitFor([&] { return session.status().pictures >= 3; }));
    const fs::path dv = dir / "Tape.mkv";
    REQUIRE(session.startRecording(request(dv, "intra")));
    REQUIRE(waitFor([&] { return session.status().recorder.framesWritten >= 12; }, 8000));
    const auto result = session.stopRecording();
    REQUIRE(result);
    const Readback back = readCapture(dv);
    REQUIRE(back.ok);
    INFO(back.error);
    CHECK(back.videoCodec == "prores");
    CHECK(back.width == 720);
    CHECK(back.height == 480);
    REQUIRE(back.sarDen > 0);
    CHECK(std::abs(static_cast<double>(back.sarNum) / back.sarDen - 10.0 / 11.0) < 0.005); // 10:11 (Matroska stores a display width)
    CHECK(back.audioChannels == 0);
    session.stopPreview();
}

TEST_CASE("Session: the system's refusals are explained and leave nothing open", "[capture][session]") {
    auto backend = std::make_shared<FakeCaptureBackend>();
    CaptureSession session(backend);

    backend->options().denyCamera = true;
    const auto camera = session.startPreview(hdmiConfig());
    REQUIRE_FALSE(camera);
    REQUIRE(accessDeniedKind(camera.error()));
    CHECK(*accessDeniedKind(camera.error()) == AccessKind::Camera);
    CHECK(camera.error().message.find("Privacy & security") != std::string::npos);
    CHECK(camera.error().message.find("Camera") != std::string::npos);
    CHECK_FALSE(session.previewing());
    CHECK(backend->videoOpens() == 0);
    const auto formats = backend->videoFormats("fake:hdmi");
    REQUIRE_FALSE(formats);
    CHECK(accessDeniedKind(formats.error()) == AccessKind::Camera);

    backend->options().denyCamera = false;
    backend->options().denyMicrophone = true;
    const auto microphone = session.startPreview(hdmiConfig());
    REQUIRE_FALSE(microphone);
    REQUIRE(accessDeniedKind(microphone.error()));
    CHECK(*accessDeniedKind(microphone.error()) == AccessKind::Microphone);
    CHECK(microphone.error().message.find("Microphone") != std::string::npos);
    CHECK_FALSE(session.previewing());

    // Without sound the same camera opens: the error is about the microphone only.
    REQUIRE(session.startPreview(hdmiConfig(false)));
    session.stopPreview();

    // Other errors are not mistaken for a refusal.
    CHECK_FALSE(accessDeniedKind(Error{ErrorCode::DeviceError, "The device is in use"}));
    CHECK_FALSE(accessDeniedKind(Error{ErrorCode::NotFound, accessDeniedError(AccessKind::Camera).message}));
}

TEST_CASE("Session: a device that is unplugged while recording is reported and the file is kept whole", "[capture][session]") {
    const fs::path dir = freshOutputDir("session_unplug");
    FakeBackendOptions options = FakeCaptureBackend::defaults();
    options.videoFailAfter = 40;
    auto backend = std::make_shared<FakeCaptureBackend>(options);
    CaptureSession session(backend);
    REQUIRE(session.startPreview(hdmiConfig()));
    const fs::path file = dir / "Unplugged.mkv";
    REQUIRE(session.startRecording(request(file)));
    REQUIRE(waitFor([&] { return !session.status().failure.empty(); }, 8000));
    CHECK(session.status().failure.find("unplugged") != std::string::npos);
    const auto result = session.stopRecording();
    REQUIRE(result);
    CHECK(result->videoFrames >= 20);
    const Readback back = readCapture(file);
    REQUIRE(back.ok);
    CHECK(static_cast<std::int64_t>(back.barcodes.size()) == result->videoFrames);
    session.stopPreview();
}

TEST_CASE("Session: closing the devices during a recording finishes the file", "[capture][session]") {
    const fs::path dir = freshOutputDir("session_close");
    auto backend = std::make_shared<FakeCaptureBackend>();
    CaptureSession session(backend);
    REQUIRE(session.startPreview(hdmiConfig()));
    const fs::path file = dir / "Closed.mkv";
    REQUIRE(session.startRecording(request(file)));
    REQUIRE(waitFor([&] { return session.status().recorder.framesWritten >= 10; }));
    session.stopPreview();
    CHECK_FALSE(session.recording());
    const auto orphan = session.takeOrphanResult();
    REQUIRE(orphan);
    CHECK(orphan->file == file);
    CHECK(orphan->videoFrames >= 10);
    CHECK_FALSE(session.takeOrphanResult()); // once
    CHECK(readCapture(file).ok);
    CHECK_FALSE(fs::exists(journalPath(file)));
}

TEST_CASE("Session: a program that stops mid-capture leaves a file that repairs, and cancel leaves nothing", "[capture][session]") {
    const fs::path dir = freshOutputDir("session_abandon");
    auto backend = std::make_shared<FakeCaptureBackend>();
    CaptureSession session(backend);
    REQUIRE(session.startPreview(hdmiConfig()));
    const fs::path kept = dir / "Kept.mkv";
    REQUIRE(session.startRecording(request(kept)));
    // 3.5 seconds: the first clusters are complete on disk.
    REQUIRE(waitFor([&] { return session.status().recorder.framesWritten >= 105; }, 12000));
    session.abandonRecording();
    CHECK_FALSE(session.recording());
    CHECK(fs::exists(journalPath(kept)));
    const auto found = findInterruptedCaptures(dir);
    REQUIRE(found.size() == 1);
    CHECK(found[0].journal.note == "bin=b3"); // what the capture was for travels with it
    const auto recovered = recoverCapture(found[0]);
    REQUIRE(recovered);
    CHECK(recovered->videoFrames >= 30);
    const Readback back = readCapture(kept);
    REQUIRE(back.ok);
    CHECK(static_cast<std::int64_t>(back.barcodes.size()) == recovered->videoFrames);

    const fs::path dropped = dir / "Dropped.mkv";
    REQUIRE(session.startRecording(request(dropped)));
    REQUIRE(waitFor([&] { return session.status().recorder.framesWritten >= 5; }));
    session.cancelRecording();
    CHECK_FALSE(fs::exists(dropped));
    CHECK_FALSE(fs::exists(journalPath(dropped)));
    session.stopPreview();
}
