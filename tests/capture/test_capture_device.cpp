// The system backend (0.7): on Windows the Media Foundation devices that are really connected, listed without opening them and
// described when picked; elsewhere the "not available in this version" backend. No camera needs to be connected: without one the
// device test is skipped, never passed silently. Tagged [device] (CI skips it: its runners have no camera).

#include <wabcut/capture/CaptureBackend.h>
#include <wabcut/capture/CaptureSession.h>

#include "CaptureTestUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <thread>

using namespace wabcut;
using namespace wabcut::capture;

TEST_CASE("System backend: devices that are connected are listed and describe their formats", "[capture][device]") {
    const auto backend = makeSystemBackend();
    REQUIRE(backend);
    INFO(backend->name());
#ifdef _WIN32
    REQUIRE(backend->supported());
    const auto devices = backend->videoDevices();
    REQUIRE(devices);
    if (devices->empty()) {
        SKIP("No video capture device (camera, capture card) is connected to this computer");
    }
    for (const VideoDevice& d : *devices) {
        INFO(d.name);
        WARN("video capture device found: " << d.name);
        CHECK_FALSE(d.id.empty());
        CHECK_FALSE(d.name.empty());
        CHECK(d.name.size() <= 128);
        CHECK(d.formats.empty()); // listing opens nothing: no formats yet
    }
    const auto formats = backend->videoFormats(devices->front().id);
    if (!formats) {
        if (accessDeniedKind(formats.error())) {
            SKIP("Windows denies desktop apps the camera: " + formats.error().message);
        }
        INFO(formats.error().message);
        FAIL("The first device did not describe its formats");
    }
    REQUIRE_FALSE(formats->empty());
    WARN("first device: " << devices->front().name << ", " << formats->size() << " formats, best " << formats->front().label());
    for (const VideoFormat& f : *formats) {
        INFO(f.label());
        CHECK(f.width >= 2);
        CHECK(f.height >= 2);
        CHECK((f.width % 2) == 0);
        CHECK((f.height % 2) == 0);
        CHECK(f.fps() > 0.0);
        CHECK_FALSE(f.nativeName.empty());
    }
    // The best format comes first: the largest picture.
    for (const VideoFormat& f : *formats) {
        CHECK(static_cast<long long>(f.width) * f.height <= static_cast<long long>(formats->front().width) * formats->front().height);
    }
    // A device that is gone is NotFound.
    const auto gone = backend->videoFormats("not-a-device");
    REQUIRE_FALSE(gone);
    CHECK(gone.error().code == ErrorCode::NotFound);
#else
    CHECK_FALSE(backend->supported());
    CHECK(backend->unsupportedReason().find("Windows") != std::string::npos);
    const auto devices = backend->videoDevices();
    REQUIRE(devices);
    CHECK(devices->empty());
    const auto opened = backend->openVideo("anything", VideoFormat{});
    REQUIRE_FALSE(opened);
    CHECK(opened.error().code == ErrorCode::Unsupported);
#endif
}

TEST_CASE("System backend: the sound inputs list without opening any", "[capture][device]") {
    const auto backend = makeSystemBackend();
    REQUIRE(backend);
    if (!backend->supported()) {
        SKIP("Capture is not available on this platform in this version");
    }
    const auto devices = backend->audioDevices();
    REQUIRE(devices);
    if (devices->empty()) {
        SKIP("No sound input is connected to this computer");
    }
    for (const AudioDevice& d : *devices) {
        CHECK_FALSE(d.id.empty());
        CHECK_FALSE(d.name.empty());
    }
    bool anyDefault = false;
    for (const AudioDevice& d : *devices) {
        anyDefault = anyDefault || d.isDefault;
    }
    if (anyDefault) {
        CHECK(devices->front().isDefault); // the default input first
    }
}

// ---- Opt-in: really open the first camera --------------------------------------------------------------------------------
// Opening a camera lights it, so this runs only when asked: set WABCUT_TEST_OPEN_CAMERA=1 (a developer's computer with a
// webcam or a capture card). It previews the first device for a moment, records two seconds and reads the file back.

TEST_CASE("System backend: the first camera previews and records two seconds (set WABCUT_TEST_OPEN_CAMERA=1)", "[capture][device]") {
    if (const char* ask = std::getenv("WABCUT_TEST_OPEN_CAMERA"); !ask || std::string(ask) != "1") {
        SKIP("Opening a camera lights it: set WABCUT_TEST_OPEN_CAMERA=1 to run this");
    }
    const auto backend = makeSystemBackend();
    REQUIRE(backend);
    if (!backend->supported()) {
        SKIP("Capture is not available on this platform in this version");
    }
    const auto devices = backend->videoDevices();
    REQUIRE(devices);
    if (devices->empty()) {
        SKIP("No video capture device is connected to this computer");
    }
    const auto formats = backend->videoFormats(devices->front().id);
    if (!formats) {
        SKIP("The first device did not open: " + formats.error().message);
    }
    REQUIRE_FALSE(formats->empty());
    // The smallest format of the device keeps the software encoder comfortable on any computer.
    VideoFormat chosen = formats->front();
    for (const VideoFormat& f : *formats) {
        if (static_cast<long long>(f.width) * f.height < static_cast<long long>(chosen.width) * chosen.height && f.fps() >= 5.0) {
            chosen = f;
        }
    }
    INFO(chosen.label());
    CaptureSession session(backend);
    SessionConfig config;
    config.videoDeviceId = devices->front().id;
    config.format = chosen;
    config.audio = false;
    const auto started = session.startPreview(config);
    if (!started) {
        SKIP("The first device did not start: " + started.error().message);
    }
    const auto waitFor = [](const std::function<bool()>& done, int ms) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end && !done()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return done();
    };
    REQUIRE(waitFor([&] { return session.status().pictures >= 5; }, 15000));
    const CaptureFrame frame = session.latestFrame();
    REQUIRE(frame.valid());
    CHECK(frame.width == chosen.width);
    CHECK(frame.height == chosen.height);

    const std::filesystem::path dir = capturetest::freshOutputDir("device_camera");
    RecordingRequest request;
    request.file = dir / "Camera.mkv";
    request.encoder = "libx264";
    REQUIRE(session.startRecording(request));
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const auto result = session.stopRecording();
    REQUIRE(result);
    session.stopPreview();
    CHECK(result->videoFrames >= static_cast<std::int64_t>(chosen.fps() * 1.5));
    const capturetest::Readback back = capturetest::readCapture(request.file);
    REQUIRE(back.ok);
    CHECK(back.width == chosen.width);
    CHECK(static_cast<std::int64_t>(back.barcodes.size()) == result->videoFrames);
}
