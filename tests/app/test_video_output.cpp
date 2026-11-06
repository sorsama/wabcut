// 0.7 (batch A9, card 110): the video output to another display. The decisions are plain functions (VideoOutput.h) tested
// without a screen: where the picture goes in a display, how the sequence's rate sits on the display's refresh rate, which mode
// Match Refresh Rate would set, and what the output does as displays come and go. MonitorBridge follows them with the
// displays and the platform's mode switch replaced by fakes, and the Video Output dialog and the output window's picture are
// checked in the shell, dark and light (test-output/ui: video-output-*.png).

#include "ShellFixture.h"

#include "MonitorBridge.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"
#include "TimelineFixture.h"
#include "VideoOutput.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QWindow>

#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::app::videoout;
using namespace wabcut::test;
using Catch::Approx;

namespace {

DisplayInfo display(const char* key, const char* label, int w, int h, double hz, bool primary = false) {
    DisplayInfo d;
    d.key = key;
    d.name = std::string("\\\\.\\") + key;
    d.label = label;
    d.width = w;
    d.height = h;
    d.refreshHz = hz;
    d.primary = primary;
    return d;
}

Timing timing(std::int32_t num, std::int32_t den, int w = 1920, int h = 1080, double par = 1.0) {
    Timing t;
    t.rateNum = num;
    t.rateDen = den;
    t.width = w;
    t.height = h;
    t.pixelAspect = par;
    return t;
}

QQuickItem* findNamed(QQuickItem* root, const QString& name) {
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findNamed(child, name))
            return found;
    }
    return nullptr;
}

} // namespace

TEST_CASE("The picture goes in the display as Fit, Fill or 1:1 says (card 110)", "[app][monitor][videooutput]") {
    // 16:9 in a 16:9 display: all three fill it or (1:1) are the frame's own size.
    const Timing hd = timing(25, 1);
    CHECK(placePicture(hd, 1920, 1080, Scaling::Fit) == Rect{0, 0, 1920, 1080});
    CHECK(placePicture(hd, 1920, 1080, Scaling::Fill) == Rect{0, 0, 1920, 1080});
    CHECK(placePicture(hd, 1920, 1080, Scaling::Native) == Rect{0, 0, 1920, 1080});

    // A wider display (21:9): Fit pillarboxes, Fill crops top and bottom, 1:1 sits in the middle.
    const Rect fit = placePicture(hd, 2560, 1080, Scaling::Fit);
    CHECK(fit.width == Approx(1920));
    CHECK(fit.height == Approx(1080));
    CHECK(fit.x == Approx(320));
    CHECK(fit.y == Approx(0));
    const Rect fill = placePicture(hd, 2560, 1080, Scaling::Fill);
    CHECK(fill.width == Approx(2560));
    CHECK(fill.height == Approx(1440));
    CHECK(fill.x == Approx(0));
    CHECK(fill.y == Approx(-180)); // 180 px cut off above and below
    const Rect native = placePicture(hd, 2560, 1080, Scaling::Native);
    CHECK(native == Rect{320, 0, 1920, 1080});
    // A narrower display (4:3): Fit letterboxes, Fill crops the sides, 1:1 is cut off at the sides.
    const Rect letterbox = placePicture(hd, 1024, 768, Scaling::Fit);
    CHECK(letterbox.width == Approx(1024));
    CHECK(letterbox.height == Approx(576));
    CHECK(letterbox.y == Approx(96));
    const Rect cropped = placePicture(hd, 1024, 768, Scaling::Fill);
    CHECK(cropped.height == Approx(768));
    CHECK(cropped.width == Approx(768 * 16.0 / 9.0));
    CHECK(cropped.x < 0);
    CHECK(placePicture(hd, 1024, 768, Scaling::Native).x < 0);
    // The shape the picture is shown in follows the pixel aspect: HDV (1440 x 1080, 4:3 pixels) is 16:9 on the display.
    const Timing hdv = timing(25, 1, 1440, 1080, 4.0 / 3.0);
    CHECK(hdv.displayAspect() == Approx(16.0 / 9.0));
    CHECK(placePicture(hdv, 1920, 1080, Scaling::Fit).width == Approx(1920));
    CHECK(placePicture(hdv, 1920, 1080, Scaling::Native).width == Approx(1920)); // its pixels, widened
    // Nothing to place on an empty display or without a picture.
    CHECK(placePicture(hd, 0, 1080, Scaling::Fit) == Rect{});
    CHECK(placePicture(timing(25, 1, 0, 0), 1920, 1080, Scaling::Fit) == Rect{});

    CHECK(scalingFromString(toString(Scaling::Fill)) == Scaling::Fill);
    CHECK(scalingFromString(toString(Scaling::Native)) == Scaling::Native);
    CHECK(scalingFromString("nonsense") == Scaling::Fit);
    CHECK(clampSyncOffset(9000) == kMaxSyncOffsetMs);
    CHECK(clampSyncOffset(-9000) == -kMaxSyncOffsetMs);
    CHECK(clampSyncOffset(40) == 40);
}

TEST_CASE("How a frame rate sits on a refresh rate, and the mode that suits it (card 110)", "[app][monitor][videooutput]") {
    CHECK(analyzeCadence(30, 60).kind == Cadence::Even);
    CHECK(analyzeCadence(30, 60).multiple == 2);
    CHECK(analyzeCadence(25, 50).kind == Cadence::Even);
    CHECK(analyzeCadence(24, 120).multiple == 5);
    CHECK(analyzeCadence(24, 48).kind == Cadence::Even);
    CHECK(analyzeCadence(30000.0 / 1001, 60000.0 / 1001).kind == Cadence::Even); // 29.97 on 59.94
    CHECK(analyzeCadence(24000.0 / 1001, 60).kind == Cadence::Pulldown); // 23.976 on 60: 2.5025 refreshes a frame
    CHECK(analyzeCadence(24000.0 / 1001, 24).kind == Cadence::NearEven);           // a repeat every 41 s
    CHECK(analyzeCadence(30000.0 / 1001, 60).kind == Cadence::NearEven);
    CHECK(analyzeCadence(24, 60).kind == Cadence::Pulldown);
    CHECK(analyzeCadence(24, 60).ratio == Approx(2.5));
    CHECK(analyzeCadence(24000.0 / 1001, 60000.0 / 1001).kind == Cadence::Pulldown);
    CHECK(analyzeCadence(25, 60).kind == Cadence::Uneven);
    CHECK(analyzeCadence(50, 60).kind == Cadence::Uneven);
    CHECK(analyzeCadence(0, 60).kind == Cadence::Unknown);
    CHECK(analyzeCadence(24, 0).kind == Cadence::Unknown);

    // Match Refresh Rate: the lowest listed rate that is a whole multiple, at the display's own size.
    const std::vector<DisplayMode> modes{{1920, 1080, 60}, {1920, 1080, 59.94}, {1920, 1080, 120}, {1920, 1080, 72},
                                         {1920, 1080, 48}, {1280, 720, 24}, {1920, 1080, 75}};
    const DisplayMode now{1920, 1080, 60};
    REQUIRE(bestModeFor(24, now, modes));
    CHECK(*bestModeFor(24, now, modes) == DisplayMode{1920, 1080, 48}); // 48 (x2) before 72 (x3) and 120 (x5); 1280 x 720 is another size
    REQUIRE(bestModeFor(25, now, modes));
    CHECK(bestModeFor(25, now, modes)->refreshHz == 75); // x3
    CHECK_FALSE(bestModeFor(30, now, modes));            // it already runs at 60: nothing to switch
    REQUIRE(bestModeFor(24000.0 / 1001, now, modes)); // 23.976: the 48 Hz mode is within a thousandth of x2
    CHECK(bestModeFor(24000.0 / 1001, now, modes)->refreshHz == 48);
    CHECK_FALSE(bestModeFor(0, now, modes));
    CHECK_FALSE(bestModeFor(24, now, std::vector<DisplayMode>{{1920, 1080, 60}}));
}

TEST_CASE("The output plan names the display, the picture and the rate (card 110)", "[app][monitor][videooutput]") {
    const std::vector<DisplayInfo> displays{display("main", "Desk", 2560, 1440, 144, true), display("tv", "Reference TV", 1920, 1080, 60)};
    Settings settings;
    settings.displayKey = "tv";
    settings.syncOffsetMs = 9999;

    // Off: nothing runs, but what is known of the display is planned (so the dialog can say how the rate would sit).
    Plan plan = planOutput(displays, settings, timing(24, 1));
    CHECK(plan.state == State::Off);
    CHECK(plan.displayIndex == 1);
    CHECK(plan.cadence.kind == Cadence::Pulldown);
    CHECK(plan.syncOffsetMs == kMaxSyncOffsetMs);

    settings.enabled = true;
    plan = planOutput(displays, settings, timing(24, 1));
    CHECK(plan.state == State::Active);
    CHECK(plan.display.label == "Reference TV");
    CHECK(plan.picture == Rect{0, 0, 1920, 1080});
    CHECK_FALSE(plan.switchTo); // not asked for
    settings.matchRefreshRate = true;
    const std::vector<DisplayMode> modes{{1920, 1080, 48}, {1920, 1080, 60}};
    plan = planOutput(displays, settings, timing(24, 1), modes);
    REQUIRE(plan.switchTo);
    CHECK(plan.switchTo->refreshHz == 48);
    plan = planOutput(displays, settings, timing(30, 1), modes);
    CHECK_FALSE(plan.switchTo); // 30 on 60 is even already

    // The display is not there: the output is off and says so; it is the same plan once the display is back.
    const std::vector<DisplayInfo> onlyMain{displays[0]};
    plan = planOutput(onlyMain, settings, timing(24, 1), modes);
    CHECK(plan.state == State::DisplayMissing);
    CHECK(plan.displayIndex == -1);
    CHECK(plan.display.key == "tv");
    CHECK(planOutput(displays, settings, timing(24, 1), modes).state == State::Active);
    // No display chosen.
    settings.displayKey.clear();
    CHECK(planOutput(displays, settings, timing(24, 1)).state == State::NoDisplay);
    // Display numbers change when displays come and go; keys do not.
    CHECK(findDisplay(displays, "tv") == 1);
    const std::vector<DisplayInfo> shifted{display("third", "Third", 800, 600, 60), displays[0], displays[1]};
    CHECK(findDisplay(shifted, "tv") == 2);
    CHECK(findDisplay(shifted, "gone") == -1);
    CHECK(findDisplay(shifted, "") == -1);
}

TEST_CASE("The output follows its display through plugging and unplugging, and Match Refresh Rate switches and restores (card 110)",
          "[app][monitor][videooutput][output]") {
    resetSettings();
    Timeline tl(R"(
        V1 tgt |Aaaa|
        A1 tgt |Aaaa|
    )"); // 25 fps, 1920 x 1080
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    PlaybackBridge playback(&controller);
    MonitorBridge monitors;
    monitors.setController(&controller);
    monitors.setPlayback(&playback);
    std::vector<DisplayInfo> displays{display("main", "Desk", 2560, 1440, 144, true), display("tv", "Reference TV", 1920, 1080, 60)};
    monitors.setDisplaySource([&] { return displays; });
    std::vector<std::pair<std::string, double>> switches; // (display, refresh rate; 0: put back)
    monitors.setModeSource([](const DisplayInfo&) { return std::vector<DisplayMode>{{1920, 1080, 50}, {1920, 1080, 60}, {1920, 1080, 75}}; });
    monitors.setModeSwitch([&](const DisplayInfo& d, const DisplayMode* mode) {
        switches.emplace_back(d.key, mode ? mode->refreshHz : 0.0);
        return true;
    });
    int planChanges = 0;
    QObject::connect(&monitors, &MonitorBridge::outputPlanChanged, [&] { ++planChanges; });
    monitors.refreshDisplays();
    const auto planState = [&] { return monitors.outputPlan().value(QStringLiteral("state")).toString(); };

    // Off until it is enabled; choosing the display by key finds it and saves the key.
    CHECK(planState() == QStringLiteral("off"));
    CHECK(monitors.transmitScreen() == -1);
    monitors.setTransmitDisplayKey(QStringLiteral("tv"));
    CHECK(monitors.transmitScreen() == 1);
    CHECK(QSettings().value(QStringLiteral("monitor/transmitDisplay")).toString() == QStringLiteral("tv"));
    CHECK(QSettings().value(QStringLiteral("monitor/transmitScreen")).toInt() == 1);
    CHECK_FALSE(monitors.outputActive());
    const QVariantMap off = monitors.outputPlan();
    CHECK(off.value(QStringLiteral("displayLabel")).toString() == QStringLiteral("Reference TV"));
    CHECK(off.value(QStringLiteral("cadence")).toString() == QStringLiteral("uneven")); // 25 fps on 60 Hz
    CHECK(off.value(QStringLiteral("rate")).toDouble() == Approx(25.0));
    monitors.setTransmit(true);
    CHECK(monitors.outputActive());
    CHECK(planState() == QStringLiteral("active"));
    CHECK(monitors.outputScreen() == 1);
    CHECK(planChanges > 0);

    // A display plugged in before it: the output stays on the TV, which is the third display now.
    displays.insert(displays.begin(), display("third", "Third", 1280, 720, 60));
    monitors.refreshDisplays();
    CHECK(monitors.transmitScreen() == 2);
    CHECK(monitors.outputScreen() == 2);
    CHECK(monitors.outputActive());
    CHECK(monitors.screens().size() == 3);
    CHECK(monitors.screens().at(2).toMap().value(QStringLiteral("key")).toString() == QStringLiteral("tv"));

    // The TV is unplugged: the output is off and says why; plugged back in, it is on again.
    displays.erase(displays.begin() + 2);
    monitors.refreshDisplays();
    CHECK_FALSE(monitors.outputActive());
    CHECK(monitors.transmitScreen() == -1);
    CHECK(planState() == QStringLiteral("missing"));
    CHECK(monitors.transmit()); // still wanted
    displays.push_back(display("tv", "Reference TV", 1920, 1080, 60));
    monitors.refreshDisplays();
    CHECK(monitors.outputActive());
    CHECK(planState() == QStringLiteral("active"));

    // Match Refresh Rate: while the output shows, the display is set to 50 Hz (25 fps, x2); gone or off, it is put back.
    CHECK(switches.empty());
    monitors.setOutputMatchRefreshRate(true);
    REQUIRE(switches.size() == 1);
    CHECK(switches.back() == std::pair<std::string, double>{"tv", 50.0});
    CHECK(monitors.switchedDisplayKey() == QStringLiteral("tv"));
    CHECK(monitors.outputPlan().value(QStringLiteral("switched")).toBool());
    displays.pop_back();
    monitors.refreshDisplays();
    REQUIRE(switches.size() == 2);
    CHECK(switches.back() == std::pair<std::string, double>{"tv", 0.0});
    CHECK(monitors.switchedDisplayKey().isEmpty());
    displays.push_back(display("tv", "Reference TV", 1920, 1080, 60));
    monitors.refreshDisplays();
    REQUIRE(switches.size() == 3);
    CHECK(switches.back().second == 50.0);
    monitors.setOutputMatchRefreshRate(false);
    REQUIRE(switches.size() == 4);
    CHECK(switches.back() == std::pair<std::string, double>{"tv", 0.0});
    monitors.setOutputMatchRefreshRate(true);
    REQUIRE(switches.size() == 5);
    monitors.setTransmit(false);
    REQUIRE(switches.size() == 6);
    CHECK(switches.back().second == 0.0);
    CHECK(monitors.switchedDisplayKey().isEmpty());
    // Toggle Full Screen is not the transmit: it never switches a display.
    monitors.setTransmit(true);
    REQUIRE(switches.size() == 7);
    monitors.setFullScreen(true);
    CHECK(switches.size() == 8);
    CHECK(switches.back().second == 0.0);
    monitors.setFullScreen(false);
    monitors.setTransmit(false);

    // The settings are remembered.
    monitors.setOutputScaling(QStringLiteral("fill"));
    monitors.setOutputSafeMargins(true);
    monitors.setOutputSyncOffsetMs(80);
    MonitorBridge again;
    CHECK(again.transmitDisplayKey() == QStringLiteral("tv"));
    CHECK(again.outputScaling() == QStringLiteral("fill"));
    CHECK(again.outputSafeMargins());
    CHECK(again.outputSyncOffsetMs() == 80);
    CHECK(again.outputMatchRefreshRate());
    monitors.setOutputSyncOffsetMs(5000);
    CHECK(monitors.outputSyncOffsetMs() == kMaxSyncOffsetMs);
}

TEST_CASE("The sync offset plays the output's program ahead, only while transmitting (card 110)", "[app][monitor][videooutput][output]") {
    resetSettings();
    Timeline tl(R"(
        V1 tgt |Aaaa|
        A1 tgt |Aaaa|
    )");
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    PlaybackBridge playback(&controller);
    MonitorBridge monitors;
    monitors.setController(&controller);
    monitors.setPlayback(&playback);
    monitors.setDisplaySource([] { return std::vector<DisplayInfo>{display("tv", "TV", 1920, 1080, 60)}; });
    monitors.setTransmitDisplayKey(QStringLiteral("tv"));
    CHECK(playback.mirrorSyncOffsetMs() == 0);
    monitors.setOutputSyncOffsetMs(120);
    CHECK(playback.mirrorSyncOffsetMs() == 0); // not transmitting: the offset is the display's, not the program's
    monitors.setTransmit(true);
    CHECK(playback.mirrorSyncOffsetMs() == 120);
    monitors.setOutputSyncOffsetMs(-40);
    CHECK(playback.mirrorSyncOffsetMs() == -40);
    monitors.setFullScreen(true); // Toggle Full Screen shows the program on the main display with no offset
    CHECK(playback.mirrorSyncOffsetMs() == 0);
    monitors.setFullScreen(false);
    CHECK(playback.mirrorSyncOffsetMs() == -40);
    monitors.setOutputSyncOffsetMs(9000);
    CHECK(playback.mirrorSyncOffsetMs() == kMaxSyncOffsetMs);
    monitors.setTransmit(false);
    CHECK(playback.mirrorSyncOffsetMs() == 0);

    // Where the picture goes for the window, for the saved scaling (the output window sizes its monitor to it).
    monitors.setOutputScaling(QStringLiteral("fill"));
    const QVariantMap fill = monitors.placeOutputPicture(2560, 1080);
    CHECK(fill.value(QStringLiteral("width")).toDouble() == Approx(2560));
    CHECK(fill.value(QStringLiteral("height")).toDouble() == Approx(1440));
    monitors.setOutputScaling(QStringLiteral("fit"));
    const QVariantMap fit = monitors.placeOutputPicture(2560, 1080);
    CHECK(fit.value(QStringLiteral("x")).toDouble() == Approx(320));
    CHECK(fit.value(QStringLiteral("width")).toDouble() == Approx(1920));
    // On a display with a device pixel ratio the window is in logical pixels: 1:1 still means the frame's own pixels.
    monitors.setOutputScaling(QStringLiteral("native"));
    const QVariantMap native = monitors.placeOutputPicture(1920, 1080, 2.0);
    CHECK(native.value(QStringLiteral("width")).toDouble() == Approx(960));
    CHECK(native.value(QStringLiteral("x")).toDouble() == Approx(480));
}

TEST_CASE("Video Output Settings in the shell: the dialog and the output window's picture, dark and light (card 110)",
          "[app][qml][monitor][videooutput][screenshot]") {
    resetSettings();
    clearMessages();
    for (const bool dark : {true, false}) {
        Shell shell;
        REQUIRE(shell.window);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        shell.setAppearance(dark, "ultraviolet");
        auto* monitors = shell.engine.singletonInstance<MonitorBridge*>("Wabcut", "Monitors");
        auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        REQUIRE(monitors);
        REQUIRE(controller);
        Timeline tl(R"(
            V1 tgt |Aaaa|
            A1 tgt |Aaaa|
        )"); // 25 fps, 1920 x 1080
        controller->resetProject(tl.state, tl.seq);
        spin(100);
        monitors->setPresentWindows(false); // never on screen in a test run
        std::vector<DisplayInfo> displays{display("main", "Desk monitor", 2560, 1440, 144, true),
                                          display("tv", "Reference TV", 2560, 1080, 60)};
        monitors->setDisplaySource([&] { return displays; });
        monitors->refreshDisplays();
        monitors->setTransmitDisplayKey(QStringLiteral("tv"));
        monitors->setTransmit(true);

        // Program Monitor > Video Output Settings... opens the dialog.
        monitors->requestVideoOutputSettings();
        spin(200);
        auto* dialog = shell.window->findChild<QObject*>(QStringLiteral("videoOutputDialog"));
        REQUIRE(dialog);
        REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }, 5000));
        QQuickItem* content = qobject_cast<QQuickItem*>(dialog->property("contentItem").value<QObject*>());
        REQUIRE(content);
        QQuickItem* state = findNamed(content, QStringLiteral("videoOutputState"));
        REQUIRE(state);
        CHECK(state->property("label").toString().contains(QStringLiteral("Reference TV")));
        QQuickItem* cadence = findNamed(content, QStringLiteral("videoOutputCadence"));
        REQUIRE(cadence);
        CHECK(cadence->isVisible());
        CHECK(cadence->property("text").toString().contains(QStringLiteral("25 fps on a 60 Hz display")));
        CHECK(cadence->property("text").toString().contains(QStringLiteral("judder")));
        // The two displays are buttons; the chosen one is the primary pill.
        QQuickItem* first = findNamed(content, QStringLiteral("videoOutputDisplay_0"));
        QQuickItem* second = findNamed(content, QStringLiteral("videoOutputDisplay_1"));
        REQUIRE(first);
        REQUIRE(second);
        CHECK(second->property("variant").toString() == QStringLiteral("primary"));
        CHECK(first->property("variant").toString() == QStringLiteral("secondary"));
        CHECK(second->property("text").toString().contains(QStringLiteral("2560 × 1080")));
        shell.settle(dark, shell.themeColor("accentFill"));
        {
            const QImage image = shell.grab();
            REQUIRE_FALSE(image.isNull());
            REQUIRE(image.save(QDir(outputDir()).filePath(dark ? QStringLiteral("video-output-dialog.png") : QStringLiteral("video-output-dialog-light.png"))));
        }
        // Picking the main display moves the output there (and the choice is the key); Fill is one of the picture choices.
        QMetaObject::invokeMethod(first, "clicked");
        spin(60);
        CHECK(monitors->transmitDisplayKey() == QStringLiteral("main"));
        QMetaObject::invokeMethod(second, "clicked");
        spin(60);
        CHECK(monitors->transmitDisplayKey() == QStringLiteral("tv"));
        QQuickItem* margins = findNamed(content, QStringLiteral("videoOutputSafeMargins"));
        REQUIRE(margins);
        margins->setProperty("checked", true); // what a click does: the switch turns, then says so
        QMetaObject::invokeMethod(margins, "toggled");
        spin(60);
        CHECK(monitors->outputSafeMargins());
        QMetaObject::invokeMethod(dialog, "close");
        spin(100);

        // The output window's picture sits in a window as the saved scaling says (a 21:9 display, 16:9 sequence).
        QQuickWindow* output = nullptr;
        for (QWindow* w : QGuiApplication::allWindows()) {
            if (w->objectName() == QStringLiteral("programOutputWindow"))
                output = qobject_cast<QQuickWindow*>(w);
        }
        REQUIRE(output);
        output->resize(2560, 1080);
        output->contentItem()->setSize(QSizeF(2560, 1080));
        QQuickItem* picture = findNamed(output->contentItem(), QStringLiteral("programOutputPicture"));
        QQuickItem* overlay = findNamed(output->contentItem(), QStringLiteral("programOutputSafeMargins"));
        REQUIRE(picture);
        REQUIRE(overlay);
        const auto settle = [&] {
            for (int i = 0; i < 5; ++i)
                spin(20);
        };
        settle();
        CHECK(picture->width() == Approx(1920));  // Fit: pillarboxed
        CHECK(picture->x() == Approx(320));
        CHECK(overlay->isVisible());               // the safe lines are on
        CHECK(overlay->width() == Approx(picture->width()));
        monitors->setOutputScaling(QStringLiteral("fill"));
        settle();
        CHECK(picture->width() == Approx(2560));  // Fill: cropped top and bottom
        CHECK(picture->height() == Approx(1440));
        CHECK(picture->y() == Approx(-180));
        monitors->setOutputScaling(QStringLiteral("native"));
        settle();
        CHECK(picture->width() == Approx(1920));
        CHECK(picture->height() == Approx(1080));
        monitors->setOutputSafeMargins(false);
        settle();
        CHECK_FALSE(overlay->isVisible());
        // Toggle Full Screen always fits the display it is on, whatever the output settings say.
        monitors->setOutputScaling(QStringLiteral("fill"));
        monitors->setFullScreen(true);
        settle();
        CHECK(picture->width() == Approx(2560));
        CHECK(picture->x() == Approx(0));
        CHECK(picture->height() == Approx(1080));
        monitors->setFullScreen(false);
        monitors->setTransmit(false);
        monitors->setOutputScaling(QStringLiteral("fit"));
        monitors->setOutputSafeMargins(false);
    }
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
