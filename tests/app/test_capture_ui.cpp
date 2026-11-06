// File > Capture… (0.7, card 078, app level), over the fake capture backend (no hardware):
//   * the Capture bridge: listing and picking a device open nothing, the preview and Record open exactly what was asked, the
//     finished file is imported into the chosen bin as ONE undo step "Capture", the system's refusals say how to allow access,
//     a drive that is nearly full is refused / stops the capture, the project closing finishes the file and keeps it for the
//     next start, and a capture a crash interrupted is repaired and added;
//   * the panel: the menu item and the command, the live picture, the counters, screenshots (dark, light, narrow, and the
//     "Windows only" state).

#include "A11yAudit.h"
#include "ShellFixture.h"

#include "CaptureBridge.h"
#include "DiskSpace.h"
#include "LibraryBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/capture/CaptureFiles.h>
#include <wabcut/capture/CaptureRecorder.h>
#include <wabcut/capture/FakeBackend.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QImage>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>

#include <functional>
#include <memory>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
namespace fs = std::filesystem;

namespace {

QQuickItem* findWith(QQuickItem* root, const std::function<bool(QQuickItem*)>& match) {
    if (!root)
        return nullptr;
    if (match(root))
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findWith(child, match))
            return found;
    }
    return nullptr;
}

QQuickItem* byName(QQuickItem* root, const QString& name) {
    return findWith(root, [&](QQuickItem* i) { return i->objectName() == name; });
}

QQuickItem* popupOf(QQuickItem* item) {
    QQuickItem* at = item;
    while (at && at->parentItem() && !QByteArray(at->parentItem()->metaObject()->className()).contains("Overlay"))
        at = at->parentItem();
    return at;
}

bool saveCard(const Shell& shell, QQuickItem* inside, const QString& file) {
    const QImage image = shell.grab();
    if (image.isNull() || !inside)
        return false;
    QQuickItem* card = popupOf(inside);
    const double dpr = image.devicePixelRatio();
    const QRectF area = card->mapRectToScene(QRectF(0, 0, card->width(), card->height()))
                            .adjusted(-8, -8, 8, 8)
                            .intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    return image.copy(QRectF(area.x() * dpr, area.y() * dpr, area.width() * dpr, area.height() * dpr).toRect())
        .save(QDir(outputDir()).filePath(file));
}

QVariant evaluate(const Shell& shell, const QString& code) {
    QQmlExpression expression(qmlContext(shell.window.get()), shell.window.get(), code);
    const QVariant result = expression.evaluate();
    INFO(code.toStdString() << ": " << expression.error().toString().toStdString());
    REQUIRE_FALSE(expression.hasError());
    return result;
}

std::filesystem::path toPath(const QString& text) {
    return std::filesystem::path(text.toStdWString());
}

/// The shell with a saved project in a temporary folder, the Capture bridge on the fake backend.
struct Rig {
    QTemporaryDir dir;
    Shell shell;
    ProjectLifecycle* project = nullptr;
    ProjectController* controller = nullptr;
    CaptureBridge* capture = nullptr;
    std::shared_ptr<capture::FakeCaptureBackend> backend;

    explicit Rig(capture::FakeBackendOptions options = capture::FakeCaptureBackend::defaults()) {
        REQUIRE(dir.isValid());
        REQUIRE(shell.window);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        project = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        capture = shell.engine.singletonInstance<CaptureBridge*>("Wabcut", "Capture");
        REQUIRE(project);
        REQUIRE(controller);
        REQUIRE(capture);
        project->newProject();
        REQUIRE_FALSE(project->demo());
        controller->setProjectFile(folder() / "Film.wabcut");
        backend = std::make_shared<capture::FakeCaptureBackend>(std::move(options));
        capture->setBackend(backend);
        capture->setEncoder("libx264");
        spin(50);
    }
    fs::path folder() const { return toPath(dir.path()); }
    fs::path captureFolder() const { return folder() / "Film.wabcut-media" / "Capture"; }
    std::size_t undoCount() { return controller->session().undoCount(); }
    bool waitState(const char* state, int timeoutMs = 15000) {
        return waitFor([&] { return capture->state() == QLatin1String(state); }, timeoutMs);
    }
    /// Lists the devices, picks the HDMI card at 640 x 360 and starts the preview, as the user would.
    void startPreview() {
        capture->refresh();
        REQUIRE(capture->waitIdle(10000));
        capture->setVideoDevice(QStringLiteral("fake:hdmi"));
        REQUIRE(capture->waitIdle(10000));
        REQUIRE(capture->formats().size() == 4);
        capture->setFormatIndex(3);
        capture->startPreview();
        REQUIRE(waitState("previewing"));
        REQUIRE(waitFor([&] { return capture->status().value(QStringLiteral("picture")).toBool(); }, 10000));
    }
    std::optional<QString> importedPath(const QString& name) {
        for (const MediaPtr& m : controller->snapshot()->media) {
            const QString path = QString::fromStdString(m->locator.path);
            if (path.endsWith(name))
                return path;
        }
        return std::nullopt;
    }
};

} // namespace

TEST_CASE("Capture: listing and picking a device open nothing; Start preview opens the one picked", "[app][capture]") {
    resetSettings();
    clearMessages();
    Rig r;
    CHECK(r.capture->supported());
    CHECK(r.capture->state() == QLatin1String("idle"));
    CHECK_FALSE(r.capture->deviceOpen());

    r.capture->refresh();
    REQUIRE(r.capture->waitIdle(10000));
    REQUIRE(r.capture->videoDevices().size() == 2);
    CHECK(r.capture->audioDevices().size() == 2);
    CHECK(r.capture->videoDevice().isEmpty()); // nothing is picked for the user
    CHECK(r.backend->videoOpens() == 0);
    CHECK(r.backend->audioOpens() == 0);
    CHECK(r.backend->formatQueries() == 0);

    r.capture->startPreview(); // no device picked: refused, nothing opened
    CHECK(r.capture->state() == QLatin1String("idle"));
    CHECK_FALSE(r.capture->error().isEmpty());
    CHECK(r.backend->videoOpens() == 0);
    clearMessages(); // (the refusal was logged on purpose)

    r.capture->setVideoDevice(QStringLiteral("fake:hdmi"));
    REQUIRE(r.capture->waitIdle(10000));
    CHECK(r.backend->formatQueries() == 1); // the formats were read ...
    CHECK(r.backend->videoOpens() == 0);    // ... nothing was captured
    REQUIRE(r.capture->formats().size() == 4);
    CHECK(r.capture->formats().front().toMap().value(QStringLiteral("label")).toString().contains(QStringLiteral("1920 x 1080")));
    CHECK(r.capture->formatIndex() == 0);
    CHECK(r.capture->diskText().contains(QStringLiteral("free")));

    r.capture->setFormatIndex(3);
    r.capture->startPreview();
    CHECK(r.capture->state() == QLatin1String("opening"));
    CHECK(r.capture->deviceOpen());
    REQUIRE(r.waitState("previewing"));
    CHECK(r.backend->videoOpens() == 1);
    CHECK(r.backend->audioOpens() == 1);
    REQUIRE(waitFor([&] { return r.capture->status().value(QStringLiteral("picture")).toBool(); }, 10000));
    const int serial = r.capture->previewSerial();
    REQUIRE(waitFor([&] { return r.capture->previewSerial() != serial; }));
    CHECK(r.capture->status().value(QStringLiteral("deviceName")).toString() == QStringLiteral("Fake HDMI Capture"));

    r.capture->stopPreview();
    CHECK(r.capture->state() == QLatin1String("idle"));
    REQUIRE(r.capture->waitIdle(10000));
    CHECK_FALSE(r.capture->deviceOpen());
    CHECK(takeMessages().isEmpty());
}

TEST_CASE("Capture: Record writes a file, Stop adds it to the chosen bin as one undo step", "[app][capture]") {
    resetSettings();
    clearMessages();
    Rig r;
    auto* library = r.shell.engine.singletonInstance<LibraryBridge*>("Wabcut", "Library");
    REQUIRE(library);
    r.startPreview();

    const QString bin = library->newBin(QStringLiteral("Tape"));
    REQUIRE_FALSE(bin.isEmpty());
    const std::size_t before = r.undoCount();
    CHECK(r.capture->suggestName() == QStringLiteral("Capture 1"));
    REQUIRE(r.capture->record(QStringLiteral("Interview: take 1"), bin));
    CHECK(r.capture->state() == QLatin1String("starting"));
    REQUIRE(r.waitState("recording"));
    CHECK(r.capture->recording());
    const fs::path file = r.captureFolder() / "Interview take 1.mkv"; // the name made safe
    CHECK(fs::exists(file));
    CHECK(fs::exists(capture::journalPath(file))); // marked as being written
    CHECK(r.undoCount() == before);                 // nothing is in the project yet

    // The counters run.
    REQUIRE(waitFor([&] { return r.capture->status().value(QStringLiteral("seconds")).toDouble() > 0.9; }, 15000));
    const QVariantMap running = r.capture->status();
    CHECK(running.value(QStringLiteral("elapsedText")).toString().startsWith(QStringLiteral("0:00:0")));
    CHECK(running.value(QStringLiteral("dropped")).toInt() == 0);
    CHECK(running.value(QStringLiteral("encoder")).toString() == QStringLiteral("libx264"));
    CHECK(running.value(QStringLiteral("freeText")).toString().contains(QStringLiteral("free on")));
    CHECK(r.capture->status().value(QStringLiteral("level")).toDouble() > 0.0);

    // A second Record while recording, a change of device or format: refused.
    CHECK_FALSE(r.capture->record(QStringLiteral("Other"), QString()));
    r.capture->setFormatIndex(1);
    CHECK(r.capture->formatIndex() == 3);

    r.capture->stop();
    CHECK(r.capture->state() == QLatin1String("finishing"));
    REQUIRE(r.capture->waitIdle(15000));
    REQUIRE(r.waitState("previewing"));
    INFO(r.capture->error().toStdString());
    CHECK(r.capture->error().isEmpty());
    CHECK(r.capture->notice().contains(QStringLiteral("Interview: take 1")));
    CHECK_FALSE(fs::exists(capture::journalPath(file))); // finished: the marker is gone

    // One undo step, "Capture", with the media in the bin that was chosen.
    CHECK(r.undoCount() == before + 1);
    REQUIRE(r.controller->session().undoLabel());
    CHECK(*r.controller->session().undoLabel() == "Capture");
    const auto path = r.importedPath(QStringLiteral("Interview take 1.mkv"));
    REQUIRE(path);
    const MediaItem* item = nullptr;
    for (const MediaPtr& m : r.controller->snapshot()->media) {
        if (QString::fromStdString(m->locator.path) == *path)
            item = m.get();
    }
    REQUIRE(item);
    CHECK_FALSE(item->info.video.empty());
    CHECK(item->info.video.front().width == 640);
    CHECK_FALSE(item->info.audio.empty());
    CHECK(item->info.duration.toSeconds() > 0.8);
    CHECK(library->mediaInBin(bin, false).size() == 1);
    // Undo takes the capture out of the project again (the file stays on the disk).
    REQUIRE(r.controller->session().undo());
    CHECK_FALSE(r.importedPath(QStringLiteral("Interview take 1.mkv")));
    CHECK(fs::exists(file));
    CHECK(r.capture->suggestName() == QStringLiteral("Capture 1")); // the name was not "Capture 1"

    // The next capture with the same name does not overwrite.
    REQUIRE(r.capture->record(QStringLiteral("Interview: take 1"), QString()));
    REQUIRE(r.waitState("recording"));
    CHECK(fs::exists(r.captureFolder() / "Interview take 1 (2).mkv"));
    REQUIRE(waitFor([&] { return r.capture->status().value(QStringLiteral("seconds")).toDouble() > 0.3; }, 15000));
    r.capture->stop();
    REQUIRE(r.capture->waitIdle(15000));
    REQUIRE(r.waitState("previewing"));
    r.capture->stopPreview();
    REQUIRE(r.capture->waitIdle(10000));
    CHECK(takeMessages().isEmpty());
}

TEST_CASE("Capture: a refused camera or microphone says how to allow it and opens the Windows page on request", "[app][capture]") {
    resetSettings();
    clearMessages();
    capture::FakeBackendOptions options = capture::FakeCaptureBackend::defaults();
    options.denyCamera = true;
    Rig r(options);
    QUrl opened;
    r.capture->setUrlOpener([&](const QUrl& url) { opened = url; });
    r.capture->refresh();
    REQUIRE(r.capture->waitIdle(10000));
    r.capture->setVideoDevice(QStringLiteral("fake:hdmi"));
    REQUIRE(r.capture->waitIdle(10000));
    CHECK(r.capture->error().contains(QStringLiteral("Privacy & security")));
    CHECK(r.capture->accessKind() == QStringLiteral("camera"));
    CHECK(r.capture->formats().isEmpty());
    r.capture->openPrivacySettings();
    CHECK(opened == QUrl(QStringLiteral("ms-settings:privacy-webcam")));

    // The camera allowed, the microphone not: the preview does not open, the page for the microphone is offered.
    r.backend->options().denyCamera = false;
    r.backend->options().denyMicrophone = true;
    r.capture->clearMessages();
    r.capture->setVideoDevice(QString());
    r.capture->setVideoDevice(QStringLiteral("fake:hdmi"));
    REQUIRE(r.capture->waitIdle(10000));
    REQUIRE(r.capture->formats().size() == 4);
    r.capture->setFormatIndex(3);
    r.capture->startPreview();
    REQUIRE(r.capture->waitIdle(10000));
    REQUIRE(waitFor([&] { return r.capture->state() == QLatin1String("idle") && !r.capture->error().isEmpty(); }));
    CHECK(r.capture->accessKind() == QStringLiteral("microphone"));
    CHECK(r.capture->error().contains(QStringLiteral("Microphone")));
    CHECK(r.backend->videoOpens() == 0);
    r.capture->openPrivacySettings();
    CHECK(opened == QUrl(QStringLiteral("ms-settings:privacy-microphone")));

    // Without sound the same device opens.
    r.capture->setAudioEnabled(false);
    r.capture->startPreview();
    REQUIRE(r.waitState("previewing"));
    CHECK(r.backend->audioOpens() == 0);
    r.capture->stopPreview();
    REQUIRE(r.capture->waitIdle(10000));
    r.capture->setAudioEnabled(true);
}

TEST_CASE("Capture: a nearly full drive is refused at the start and stops a capture that fills it", "[app][capture]") {
    resetSettings();
    clearMessages();
    Rig r;
    r.startPreview();
    diskspace::setFreeBytesProviderForTests([](const QString&) { return qint64(100) * 1024 * 1024; });
    CHECK_FALSE(r.capture->record(QStringLiteral("Full"), QString()));
    CHECK(r.capture->error().contains(QStringLiteral("free some space")));
    CHECK(r.capture->state() == QLatin1String("previewing"));
    CHECK_FALSE(fs::exists(r.captureFolder() / "Full.mkv"));

    diskspace::setFreeBytesProviderForTests([](const QString&) { return qint64(50) * 1024 * 1024 * 1024; });
    r.capture->clearMessages();
    REQUIRE(r.capture->record(QStringLiteral("Filling"), QString()));
    REQUIRE(r.waitState("recording"));
    REQUIRE(waitFor([&] { return r.capture->status().value(QStringLiteral("seconds")).toDouble() > 0.5; }, 15000));
    const std::size_t before = r.undoCount();
    // The drive fills up while recording: the capture stops itself, finished, and says why.
    diskspace::setFreeBytesProviderForTests([](const QString&) { return qint64(100) * 1024 * 1024; });
    REQUIRE(waitFor([&] { return r.capture->state() == QLatin1String("previewing") && r.undoCount() == before + 1; }, 20000));
    diskspace::setFreeBytesProviderForTests({});
    CHECK(r.capture->error().contains(QStringLiteral("only")));
    CHECK(r.importedPath(QStringLiteral("Filling.mkv")));
    r.capture->stopPreview();
    REQUIRE(r.capture->waitIdle(10000));
}

TEST_CASE("Capture: closing the project during a capture finishes the file and keeps it for the next start", "[app][capture]") {
    resetSettings();
    clearMessages();
    Rig r;
    r.startPreview();
    REQUIRE(r.capture->record(QStringLiteral("Unfinished"), QString()));
    REQUIRE(r.waitState("recording"));
    REQUIRE(waitFor([&] { return r.capture->status().value(QStringLiteral("seconds")).toDouble() > 0.5; }, 15000));
    const fs::path file = r.captureFolder() / "Unfinished.mkv";

    r.project->newProject(); // the project is closed (a new one replaces it)
    CHECK(r.capture->state() == QLatin1String("idle"));
    CHECK_FALSE(r.capture->deviceOpen());
    CHECK(fs::exists(file));
    CHECK(fs::exists(capture::journalPath(file))); // kept: the next start offers it
    CHECK(r.capture->notice().contains(QStringLiteral("Unfinished")));

    // The project opens again: the capture is offered, once, with a toast.
    int announced = 0;
    QObject::connect(r.capture, &CaptureBridge::interruptedFound, r.capture, [&](int) { ++announced; });
    r.controller->setProjectFile(r.folder() / "Film.wabcut");
    emit r.controller->mediaChanged();
    spin(50);
    REQUIRE(r.capture->interrupted().size() == 1);
    CHECK(announced == 1);
    CHECK(r.capture->interrupted().front().toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Unfinished"));

    // Recover and add: one undo step.
    const std::size_t before = r.undoCount();
    r.capture->recoverInterrupted(r.capture->interrupted().front().toMap().value(QStringLiteral("path")).toString(), QString());
    REQUIRE(r.capture->waitIdle(15000));
    INFO(r.capture->error().toStdString());
    CHECK(r.capture->error().isEmpty());
    CHECK(r.undoCount() == before + 1);
    CHECK(r.importedPath(QStringLiteral("Unfinished.mkv")));
    CHECK(r.capture->interrupted().isEmpty());
    CHECK_FALSE(fs::exists(capture::journalPath(file)));
}

TEST_CASE("Capture: a capture a crash interrupted is offered when the project opens, repaired, or deleted", "[app][capture]") {
    resetSettings();
    clearMessages();
    Rig r;
    std::error_code ec;
    fs::create_directories(r.captureFolder(), ec);
    // Two captures the program stopped in the middle of (the recorder abandoned = as a crash would leave them).
    const auto interrupt = [&](const char* name) {
        const fs::path file = r.captureFolder() / name;
        capture::CaptureRecorder rec;
        capture::RecorderConfig config;
        config.file = file;
        config.video.width = 320;
        config.video.height = 180;
        config.video.frameRate = Rate{30, 1};
        config.encoder = "libx264";
        config.maxQueuedFrames = 1000;
        config.provenance = "kind=capture\nbin=\n";
        REQUIRE(rec.start(config));
        const std::int64_t base = 1'000'000'000'000;
        for (std::int64_t k = 0; k < 100; ++k) {
            rec.pushVideo(capture::makeFrame(capture::PixelFormat::Nv12, 320, 180, base + k * 33'333'333,
                                             [&](std::uint8_t* b) { capture::drawFakePicture(capture::PixelFormat::Nv12, 320, 180, k, b); }));
        }
        for (int i = 0; i < 10000 && rec.status().framesWritten < 100; ++i)
            spin(1);
        rec.abandon();
        return file;
    };
    const fs::path first = interrupt("Crashed A.mkv");
    const fs::path second = interrupt("Crashed B.mkv");

    int announced = 0;
    QObject::connect(r.capture, &CaptureBridge::interruptedFound, r.capture, [&](int) { ++announced; });
    r.controller->setProjectFile(r.folder() / "Other.wabcut");
    emit r.controller->mediaChanged(); // another project file opened ...
    r.controller->setProjectFile(r.folder() / "Film.wabcut");
    emit r.controller->mediaChanged(); // ... and this one again
    spin(50);
    REQUIRE(r.capture->interrupted().size() == 2);
    CHECK(announced >= 1);

    r.capture->discardInterrupted(QString::fromStdWString(second.wstring()));
    CHECK_FALSE(fs::exists(second));
    CHECK(r.capture->interrupted().size() == 1);

    const std::size_t before = r.undoCount();
    r.capture->recoverInterrupted(QString::fromStdWString(first.wstring()), QString());
    REQUIRE(r.capture->waitIdle(15000));
    INFO(r.capture->error().toStdString());
    CHECK(r.capture->error().isEmpty());
    CHECK(r.undoCount() == before + 1);
    CHECK(r.importedPath(QStringLiteral("Crashed A.mkv")));
    CHECK(r.capture->interrupted().isEmpty());
    // A path that is not an interrupted capture of this project is refused.
    r.capture->recoverInterrupted(QStringLiteral("C:/Windows/notepad.exe"), QString());
    REQUIRE(r.capture->waitIdle(5000));
    CHECK_FALSE(r.capture->error().isEmpty());
    CHECK(r.undoCount() == before + 1);
}

TEST_CASE("Capture: a device that is unplugged during a capture stops it, finished, and says so", "[app][capture]") {
    resetSettings();
    clearMessages();
    capture::FakeBackendOptions options = capture::FakeCaptureBackend::defaults();
    options.videoFailAfter = 45;
    Rig r(options);
    r.startPreview();
    REQUIRE(r.capture->record(QStringLiteral("Unplugged"), QString()));
    REQUIRE(r.waitState("recording"));
    const std::size_t before = r.undoCount();
    REQUIRE(waitFor([&] { return r.undoCount() == before + 1; }, 20000));
    REQUIRE(r.capture->waitIdle(15000));
    CHECK(r.capture->error().contains(QStringLiteral("unplugged")));
    CHECK(r.importedPath(QStringLiteral("Unplugged.mkv")));
    r.capture->stopPreview();
    REQUIRE(r.capture->waitIdle(10000));
}

TEST_CASE("Capture: on a system without a capture source the panel says so and opens nothing", "[app][capture]") {
    resetSettings();
    clearMessages();
    capture::FakeBackendOptions options;
    options.supported = false;
    options.unsupportedReason = "Capturing from a camera or capture card works on Windows in this version.";
    Rig r(options);
    CHECK_FALSE(r.capture->supported());
    CHECK(r.capture->unsupportedReason().contains(QStringLiteral("Windows")));
    r.capture->refresh();
    REQUIRE(r.capture->waitIdle(5000));
    CHECK(r.capture->videoDevices().isEmpty());
    r.capture->startPreview();
    CHECK(r.capture->state() == QLatin1String("idle"));
    CHECK(r.backend->videoOpens() == 0);
}

TEST_CASE("Capture panel: File > Capture… opens it; the live picture, the counters and the choices; screenshots (078)",
          "[app][capture][qml][screenshot]") {
    for (const bool dark : {true, false}) {
        resetSettings();
        clearMessages();
        Rig r;
        if (!dark) {
            r.shell.setAppearance(false, "ultraviolet");
            REQUIRE(r.shell.settle(false, r.shell.themeColor("accentFill")));
        }
        REQUIRE_FALSE(r.shell.grab().isNull());
        QQuickItem* root = r.shell.window->contentItem();

        // The menu item and the command (File > Capture…) open the panel; nothing is opened by that.
        CHECK(evaluate(r.shell, QStringLiteral("CommandPalette.commandState(\"file.capture\").enabled")).toBool());
        evaluate(r.shell, QStringLiteral("runCommand(\"file.capture\")"));
        QObject* loader = r.shell.object("captureDialogLoader");
        REQUIRE(loader);
        QObject* dialog = nullptr;
        REQUIRE(waitFor([&] {
            dialog = qvariant_cast<QObject*>(loader->property("item"));
            return dialog && dialog->property("opened").toBool();
        }, 5000));
        REQUIRE(r.capture->waitIdle(10000));
        spin(150);
        CHECK(r.backend->videoOpens() == 0);
        CHECK_FALSE(r.capture->deviceOpen());
        QQuickItem* device = byName(root, QStringLiteral("captureDevice"));
        REQUIRE(device);
        CHECK(device->isEnabled());
        QQuickItem* preview = byName(root, QStringLiteral("capturePreviewButton"));
        REQUIRE(preview);
        CHECK_FALSE(preview->isEnabled()); // no device picked yet
        CHECK(byName(root, QStringLiteral("captureViewText"))->property("text").toString().contains(QStringLiteral("Choose a video device")));
        REQUIRE(saveCard(r.shell, device, dark ? QStringLiteral("capture-empty-dark.png") : QStringLiteral("capture-empty-light.png")));

        // Pick the device and a format as the user does; start the preview.
        r.capture->setVideoDevice(QStringLiteral("fake:hdmi"));
        REQUIRE(r.capture->waitIdle(10000));
        r.capture->setFormatIndex(2); // 1280 x 720 at 30
        spin(100);
        CHECK(preview->isEnabled());
        QQuickItem* format = byName(root, QStringLiteral("captureFormat"));
        REQUIRE(format);
        CHECK(format->property("shown").toString().contains(QStringLiteral("1280 x 720")));
        REQUIRE(QMetaObject::invokeMethod(preview, "clicked"));
        REQUIRE(r.waitState("previewing"));
        QQuickItem* pictureItem = byName(root, QStringLiteral("capturePicture"));
        REQUIRE(pictureItem);
        REQUIRE(waitFor([&] { return pictureItem->property("hasPicture").toBool(); }, 10000));
        CHECK(r.backend->videoOpens() == 1);

        // Record: the counter runs, the mark shows, the choices are locked.
        QQuickItem* record = byName(root, QStringLiteral("captureRecordButton"));
        REQUIRE(record);
        CHECK(record->isEnabled());
        QQuickItem* name = byName(root, QStringLiteral("captureNameField"));
        REQUIRE(name);
        CHECK(name->property("text").toString() == QStringLiteral("Capture 1"));
        name->setProperty("text", QStringLiteral("Cam A"));
        REQUIRE(QMetaObject::invokeMethod(record, "clicked"));
        REQUIRE(r.waitState("recording"));
        REQUIRE(waitFor([&] { return r.capture->status().value(QStringLiteral("seconds")).toDouble() > 1.0; }, 15000));
        spin(200);
        CHECK(byName(root, QStringLiteral("captureRecMark"))->isVisible());
        CHECK_FALSE(device->isEnabled());
        CHECK_FALSE(byName(root, QStringLiteral("captureClose"))->isEnabled()); // not while a capture runs
        CHECK(byName(root, QStringLiteral("captureTime"))->property("text").toString().startsWith(QStringLiteral("0:00:0")));
        CHECK(byName(root, QStringLiteral("captureSize"))->isVisible());
        CHECK(record->property("text").toString() == QStringLiteral("Stop"));
        QQuickItem* pill = byName(root, QStringLiteral("capturePill"));
        REQUIRE(pill);
        CHECK(pill->isVisible()); // the recording indicator in the top bar
        REQUIRE(saveCard(r.shell, byName(root, QStringLiteral("captureView")), dark ? QStringLiteral("capture-recording-dark.png") : QStringLiteral("capture-recording-light.png")));

        // Stop: the capture goes into the project (one undo step), the panel stays on the live picture.
        const std::size_t before = r.undoCount();
        REQUIRE(QMetaObject::invokeMethod(record, "clicked"));
        REQUIRE(r.capture->waitIdle(15000));
        REQUIRE(r.waitState("previewing"));
        CHECK(r.undoCount() == before + 1);
        CHECK(r.importedPath(QStringLiteral("Cam A.mkv")));
        QQuickItem* notice = byName(root, QStringLiteral("captureNotice"));
        REQUIRE(notice);
        CHECK(notice->isVisible());
        CHECK(notice->property("text").toString().contains(QStringLiteral("Cam A")));
        CHECK(name->property("text").toString() == QStringLiteral("Capture 1")); // the next name
        CHECK(byName(root, QStringLiteral("captureClose"))->isEnabled());

        // Closing the panel closes the camera.
        REQUIRE(QMetaObject::invokeMethod(byName(root, QStringLiteral("captureClose")), "clicked"));
        REQUIRE(waitFor([&] { return !dialog->property("opened").toBool(); }, 5000));
        REQUIRE(r.capture->waitIdle(10000));
        CHECK(r.capture->state() == QLatin1String("idle"));
        const QStringList messages = takeMessages();
        INFO(join(messages));
        CHECK(messages.isEmpty());
    }
}

TEST_CASE("Capture panel: nothing is cut off at the smallest window; the Windows-only state; an interrupted capture is offered; screenshots (078)",
          "[app][capture][qml][screenshot]") {
    resetSettings();
    clearMessages();
    Rig r;
    r.shell.resize(1100, 700);
    REQUIRE_FALSE(r.shell.grab().isNull());
    QQuickItem* root = r.shell.window->contentItem();
    // An interrupted capture waits in the project's folder.
    std::error_code ec;
    fs::create_directories(r.captureFolder(), ec);
    {
        const fs::path file = r.captureFolder() / "A tape from last week with a very long clip name that has to be shortened somewhere.mkv";
        QFile f(QString::fromStdWString(file.wstring()));
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(2'000'000, 'x'));
        f.close();
        REQUIRE(capture::writeJournal(file, "kind=capture\n"));
        // (a file with the journal of a process that is gone)
        QFile j(QString::fromStdWString(capture::journalPath(file).wstring()));
        REQUIRE(j.open(QIODevice::WriteOnly));
        j.write("wabcut-capture-journal 1\npid 2000000000\nstamp 0\nstarted 2026-11-05T09:30:00Z\n\nkind=capture\n");
        j.close();
    }
    r.capture->rescanInterrupted();
    REQUIRE(r.capture->interrupted().size() == 1);

    evaluate(r.shell, QStringLiteral("runCommand(\"file.capture\")"));
    QObject* loader = r.shell.object("captureDialogLoader");
    REQUIRE(loader);
    QObject* dialog = nullptr;
    REQUIRE(waitFor([&] {
        dialog = qvariant_cast<QObject*>(loader->property("item"));
        return dialog && dialog->property("opened").toBool();
    }, 5000));
    REQUIRE(r.capture->waitIdle(10000));
    r.capture->setVideoDevice(QStringLiteral("fake:deck")); // a tape deck: 720 x 480 with 10:11 pixels
    REQUIRE(r.capture->waitIdle(10000));
    spin(300);
    QQuickItem* view = byName(root, QStringLiteral("captureView"));
    REQUIRE(view);
    QQuickItem* card = popupOf(view);
    const QRectF rect = card->mapRectToScene(QRectF(0, 0, card->width(), card->height()));
    CHECK(rect.left() >= 0.0);
    CHECK(rect.right() <= 1100.0);
    CHECK(rect.top() >= 0.0);
    CHECK(rect.bottom() <= 700.0);
    QQuickItem* close = byName(root, QStringLiteral("captureClose"));
    REQUIRE(close);
    const QRectF closeRect = close->mapRectToScene(QRectF(0, 0, close->width(), close->height()));
    CHECK(closeRect.bottom() <= 700.0);
    CHECK(closeRect.right() <= 1100.0);
    QQuickItem* row = byName(root, QStringLiteral("captureInterruptedRow_0"));
    REQUIRE(row);
    CHECK(row->isVisible());
    const QRectF rowRect = row->mapRectToScene(QRectF(0, 0, row->width(), row->height()));
    CHECK(rowRect.right() <= rect.right());
    REQUIRE(saveCard(r.shell, view, QStringLiteral("capture-narrow.png")));
    // Delete it from the panel.
    REQUIRE(QMetaObject::invokeMethod(byName(root, QStringLiteral("captureDiscard")), "clicked"));
    REQUIRE(waitFor([&] { return r.capture->interrupted().isEmpty(); }, 3000));
    REQUIRE(QMetaObject::invokeMethod(close, "clicked"));
    REQUIRE(waitFor([&] { return !dialog->property("opened").toBool(); }, 5000));
}

TEST_CASE("Capture panel: every control has an accessible name and the keyboard reaches Start preview, Record and the name (078)",
          "[app][capture][qml][a11y]") {
    resetSettings();
    clearMessages();
    A11yScope a11y; // before the shell, so the controls register their names
    Rig r;
    REQUIRE_FALSE(r.shell.grab().isNull());
    QQuickItem* root = r.shell.window->contentItem();
    evaluate(r.shell, QStringLiteral("runCommand(\"file.capture\")"));
    QObject* loader = r.shell.object("captureDialogLoader");
    REQUIRE(loader);
    QObject* dialog = nullptr;
    REQUIRE(waitFor([&] {
        dialog = qvariant_cast<QObject*>(loader->property("item"));
        return dialog && dialog->property("opened").toBool();
    }, 5000));
    REQUIRE(r.capture->waitIdle(10000));
    r.capture->setVideoDevice(QStringLiteral("fake:hdmi"));
    REQUIRE(r.capture->waitIdle(10000));
    spin(200);
    QQuickItem* card = popupOf(byName(root, QStringLiteral("captureDevice")));
    REQUIRE(card);
    const QStringList issues = auditAccessibility(card);
    INFO(join(issues));
    CHECK(issues.isEmpty());
    CHECK(tabReaches(card, QStringLiteral("captureDevice")));
    CHECK(tabReaches(card, QStringLiteral("captureFormat")));
    CHECK(tabReaches(card, QStringLiteral("captureAudioToggle")));
    CHECK(tabReaches(card, QStringLiteral("capturePreset")));
    CHECK(tabReaches(card, QStringLiteral("captureNameField")));
    CHECK(tabReaches(card, QStringLiteral("captureBin")));
    CHECK(tabReaches(card, QStringLiteral("capturePreviewButton")));
    // Space on Start preview opens the preview; Record then takes Space and Stop too.
    QQuickItem* preview = byName(root, QStringLiteral("capturePreviewButton"));
    REQUIRE(preview);
    REQUIRE(preview->isEnabled());
    REQUIRE(pressWithKey(r.shell.window.get(), preview, Qt::Key_Space, QStringLiteral(" ")));
    REQUIRE(r.waitState("previewing"));
    REQUIRE(waitFor([&] { return r.capture->status().value(QStringLiteral("picture")).toBool(); }, 10000));
    spin(100);
    CHECK(tabReaches(card, QStringLiteral("captureRecordButton")));
    QQuickItem* record = byName(root, QStringLiteral("captureRecordButton"));
    REQUIRE(record);
    REQUIRE(pressWithKey(r.shell.window.get(), record, Qt::Key_Space, QStringLiteral(" ")));
    REQUIRE(r.waitState("recording"));
    REQUIRE(waitFor([&] { return r.capture->status().value(QStringLiteral("seconds")).toDouble() > 0.4; }, 15000));
    CHECK(auditAccessibility(card).isEmpty());
    REQUIRE(pressWithKey(r.shell.window.get(), record, Qt::Key_Space, QStringLiteral(" "))); // Stop
    REQUIRE(r.capture->waitIdle(15000));
    REQUIRE(r.waitState("previewing"));
    CHECK(r.importedPath(QStringLiteral("Capture 1.mkv")));
    r.capture->stopPreview();
    REQUIRE(r.capture->waitIdle(10000));
    REQUIRE(QMetaObject::invokeMethod(byName(root, QStringLiteral("captureClose")), "clicked"));
    REQUIRE(waitFor([&] { return !dialog->property("opened").toBool(); }, 5000));
}

TEST_CASE("Capture panel: a system without a capture source shows why, in both themes (078)", "[app][capture][qml][screenshot]") {
    for (const bool dark : {true, false}) {
        resetSettings();
        clearMessages();
        capture::FakeBackendOptions options;
        options.supported = false;
        options.unsupportedReason = "Capturing from a camera or capture card works on Windows in this version. Support for Linux (V4L2) and "
                                    "macOS (AVFoundation) is planned.";
        Rig r(options);
        if (!dark) {
            r.shell.setAppearance(false, "ultraviolet");
            REQUIRE(r.shell.settle(false, r.shell.themeColor("accentFill")));
        }
        REQUIRE_FALSE(r.shell.grab().isNull());
        QQuickItem* root = r.shell.window->contentItem();
        evaluate(r.shell, QStringLiteral("runCommand(\"file.capture\")"));
        QObject* loader = r.shell.object("captureDialogLoader");
        REQUIRE(loader);
        QObject* dialog = nullptr;
        REQUIRE(waitFor([&] {
            dialog = qvariant_cast<QObject*>(loader->property("item"));
            return dialog && dialog->property("opened").toBool();
        }, 5000));
        spin(200);
        QQuickItem* text = byName(root, QStringLiteral("captureUnsupportedText"));
        REQUIRE(text);
        CHECK(text->isVisible());
        CHECK(text->property("text").toString().contains(QStringLiteral("Windows")));
        CHECK_FALSE(byName(root, QStringLiteral("captureColumns"))->isVisible());
        CHECK_FALSE(byName(root, QStringLiteral("capturePreviewButton"))->isVisible());
        REQUIRE(saveCard(r.shell, text, dark ? QStringLiteral("capture-unsupported-dark.png") : QStringLiteral("capture-unsupported-light.png")));
        REQUIRE(QMetaObject::invokeMethod(byName(root, QStringLiteral("captureClose")), "clicked"));
        REQUIRE(waitFor([&] { return !dialog->property("opened").toBool(); }, 5000));
    }
}

TEST_CASE("Capture: over the sample project the command says why it cannot run", "[app][capture][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    auto* project = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    REQUIRE(project);
    spin(100);
    if (!project->demo())
        SKIP("The sample project is not open at start in this run");
    const QVariant state = evaluate(shell, QStringLiteral("CommandPalette.commandState(\"file.capture\")"));
    CHECK_FALSE(state.toMap().value(QStringLiteral("enabled")).toBool());
    CHECK(state.toMap().value(QStringLiteral("reason")).toString().contains(QStringLiteral("project")));
}
