// P.2 (E6): Premiere's Source and Program Monitor settings and keys (docs/monitors.md): Loop and Play In to Out in both
// monitors, the Monitors bridge's routing of the monitor keys. Offscreen; no window is shown.
// 0.4: Play Around and Play from Playhead to Out in both monitors (also when the postroll runs into the end); platform
// safe zones on vertical sequences (wrench > Platform Safe Zones; screenshots program-safe-zones-*.png, the bright one
// over a white picture); transports that fit their panels with a "···" menu of the hidden buttons
// (monitor-transport-overflow.png, monitor-transport-overflow-menu.png).

#include "BridgeIds.h"
#include "MonitorBridge.h"
#include "MonitorGuides.h"
#include "MotionBridge.h"
#include "PlaybackBridge.h"
#include "ProgramMonitor.h"
#include "ProjectController.h"
#include "ShellFixture.h"
#include "SourceMonitorBridge.h"
#include "TimelineFixture.h"

#include <wabcut/command/Engine.h>
#include <wabcut/model/MotionOps.h>
#include <wabcut/playback/video/PlaybackController.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <string_view>

#include <QGuiApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QQmlComponent>
#include <QQuickItem>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUrl>
#include <QQuickWindow>
#include <QScreen>
#include <QSettings>
#include <QWindow>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using Catch::Approx;

namespace {

constexpr std::string_view kPicture = R"(
    V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    A1 tgt |Aaaaaaaaaaaaaaaaaaaa|
)";

struct ProgramRig {
    Timeline tl{kPicture};
    ProjectController controller;
    std::unique_ptr<PlaybackBridge> playback;

    ProgramRig() {
        resetSettings();
        controller.resetProject(tl.state, tl.seq);
        playback = std::make_unique<PlaybackBridge>(&controller);
    }
    void mark(int in, int lastFrame) {
        controller.setPlayhead(f(in));
        controller.markInAtPlayhead();
        controller.setPlayhead(f(lastFrame));
        controller.markOutAtPlayhead();
    }
};

} // namespace

TEST_CASE("Program Monitor Loop repeats In to Out or the whole sequence and is remembered", "[app][monitor][loop]") {
    ProgramRig rig;
    CHECK_FALSE(rig.playback->loop());
    // No marks: the whole sequence.
    REQUIRE(rig.playback->loopRange());
    CHECK(*rig.playback->loopRange() == TimeRange::fromStartEnd(Ticks{}, f(20)));
    rig.mark(5, 14);
    CHECK(*rig.playback->loopRange() == TimeRange::fromStartEnd(f(5), f(15)));
    REQUIRE(rig.playback->session());
    CHECK_FALSE(rig.playback->session()->loop()); // off: nothing handed over

    rig.playback->setLoop(true);
    CHECK(QSettings().value(QStringLiteral("monitor/programLoop")).toBool());
    CHECK(rig.playback->session()->loop() == TimeRange::fromStartEnd(f(5), f(15)));
    // The marks move: the loop follows.
    rig.controller.setPlayhead(f(8));
    rig.controller.markInAtPlayhead();
    CHECK(rig.playback->session()->loop() == TimeRange::fromStartEnd(f(8), f(15)));

    // Play from outside the range starts at its beginning.
    rig.controller.setPlayhead(f(2));
    rig.playback->togglePlay();
    CHECK(rig.playback->playing());
    CHECK(rig.controller.playhead() >= f(8));
    // It keeps playing past the Out (the transport wraps to the In).
    spin(800);
    CHECK(rig.playback->playing());
    CHECK(rig.controller.playhead() >= f(8));
    CHECK(rig.controller.playhead() < f(15));
    rig.playback->togglePlay();
    CHECK_FALSE(rig.playback->playing());

    rig.playback->setLoop(false);
    CHECK_FALSE(rig.playback->session()->loop());
    CHECK_FALSE(QSettings().value(QStringLiteral("monitor/programLoop")).toBool());
}

TEST_CASE("Program Monitor Play In to Out stops on the last marked frame unless Loop is on", "[app][monitor][loop]") {
    ProgramRig rig;
    rig.mark(4, 9); // In 4, Out after frame 9: 0.24 s at 25 fps
    rig.controller.setPlayhead(f(15));
    rig.playback->playInToOut();
    CHECK(rig.playback->playing());
    CHECK(rig.controller.playhead() >= f(4));
    REQUIRE(waitFor([&] { return !rig.playback->playing(); }, 3000));
    CHECK(rig.controller.playhead() == f(9));

    rig.playback->setLoop(true);
    rig.playback->playInToOut();
    spin(600);
    CHECK(rig.playback->playing()); // repeats
    rig.playback->togglePlay();
}

TEST_CASE("Play Around plays the preroll and postroll and parks back on the playhead", "[app][monitor][transport]") {
    // 0.4, Premiere's Play Around (Shift+K): Main.qml passes Settings > Timeline's "Play around the edit" times.
    ProgramRig rig; // 20 frames at 25 fps
    rig.controller.setPlayhead(f(10));
    rig.playback->playAround(0.2, 0.2); // 5 frames before, 5 after
    CHECK(rig.playback->playing());
    CHECK(rig.controller.playhead() >= f(5));
    REQUIRE(waitFor([&] { return !rig.playback->playing(); }, 3000));
    CHECK(rig.controller.playhead() == f(10)); // parked back

    // Loop is ignored while it plays once, and comes back after.
    rig.playback->setLoop(true);
    const std::optional<TimeRange> loop = rig.playback->session()->loop();
    REQUIRE(loop);
    rig.playback->playAround(0.2, 0.2);
    CHECK_FALSE(rig.playback->session()->loop());
    REQUIRE(waitFor([&] { return !rig.playback->playing(); }, 3000));
    CHECK(rig.controller.playhead() == f(10));
    CHECK(rig.playback->session()->loop() == loop);
    rig.playback->setLoop(false);

    // Space during it stops it where it is: no park-back.
    rig.playback->playAround(0.2, 0.4);
    spin(60);
    rig.playback->togglePlay();
    CHECK_FALSE(rig.playback->playing());
    const Ticks stopped = rig.controller.playhead();
    spin(500);
    CHECK(rig.controller.playhead() == stopped);

    // Clamped at the start.
    rig.controller.setPlayhead(f(1));
    rig.playback->playAround(2.0, 0.1);
    CHECK(rig.playback->playing());
    REQUIRE(waitFor([&] { return !rig.playback->playing(); }, 3000));
    CHECK(rig.controller.playhead() == f(1));
}

namespace {

/// Gives every media item of `state` a file in `dir` (64 zero bytes: nothing decodes, the picture stays black). Only
/// media with files get a sequence to play, and only then does the scheduler stop at the end on its own.
void giveFiles(ProjectState& state, const QTemporaryDir& dir) {
    for (MediaPtr& media : state.media) {
        auto item = std::make_shared<MediaItem>(*media);
        const QString path = dir.filePath(QString::fromStdString(item->name) + QStringLiteral(".mov"));
        QFile file(path);
        REQUIRE(file.open(QIODevice::WriteOnly));
        file.write(QByteArray(64, '\0'));
        item->locator.path = QDir::toNativeSeparators(path).toStdString();
        item->info.path = item->locator.path;
        media = std::move(item);
    }
}

} // namespace

TEST_CASE("Play Around parks back on the playhead when the postroll reaches the sequence end", "[app][monitor][transport]") {
    // 0.4 (fix round): the scheduler stops at the sequence end itself, a display latency early, and parks on the last
    // frame before the bridge's tick sees the stop point. Play Around must still go back to where it started.
    resetSettings();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    Timeline tl{kPicture}; // 20 frames at 25 fps
    giveFiles(tl.state, dir);
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    REQUIRE(controller.hasMediaFiles());
    PlaybackBridge playback(&controller);
    controller.setPlayhead(f(16));
    playback.playAround(0.2, 0.4); // from frame 11; the postroll runs into the end
    CHECK(playback.playing());
    REQUIRE(waitFor([&] { return !playback.playing(); }, 3000));
    INFO("parked on " << controller.playheadText().toStdString());
    CHECK(waitFor([&] { return controller.playhead() == f(16); }, 1000));
    spin(200);
    CHECK(controller.playhead() == f(16)); // and stays there
    CHECK_FALSE(playback.playing());

    // Space during it still stops it where it is (no park-back).
    playback.playAround(0.4, 0.4);
    spin(80);
    playback.togglePlay();
    CHECK_FALSE(playback.playing());
    const Ticks stopped = controller.playhead();
    spin(300);
    CHECK(controller.playhead() == stopped);
}

TEST_CASE("Source Monitor Play Around parks back when the postroll reaches the media end", "[app][monitor][transport]") {
    resetSettings();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    Timeline tl(kPicture, {{'S', 50}});
    giveFiles(tl.state, dir);
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    SourceMonitorBridge source;
    source.setController(&controller);
    REQUIRE(source.load(mediaKey(tl.media('S'))));
    REQUIRE(source.lengthTicks() == f(50));
    source.seek(f(46));
    source.playAround(0.2, 0.4); // the postroll runs into the end of the media
    CHECK(source.playing());
    REQUIRE(waitFor([&] { return !source.playing(); }, 3000));
    INFO("parked on " << source.positionText().toStdString());
    CHECK(waitFor([&] { return source.positionTicks() == f(46); }, 1000));
    spin(200);
    CHECK(source.positionTicks() == f(46));
    CHECK_FALSE(source.playing());
}

TEST_CASE("Play from Playhead to Out stops on the last marked frame", "[app][monitor][transport]") {
    // 0.4, Premiere's Play from Playhead to Out (Ctrl+Space), through the Monitors bridge as Main.qml routes it.
    ProgramRig rig;
    MonitorBridge monitors;
    monitors.setPlayback(rig.playback.get());
    CHECK(MonitorBridge::handlesCommand(QStringLiteral("transport.playFromPlayheadToOut")));
    rig.mark(4, 9); // Out after frame 9
    rig.controller.setPlayhead(f(6));
    REQUIRE(monitors.runCommand(QStringLiteral("transport.playFromPlayheadToOut")));
    CHECK(rig.playback->playing());
    CHECK(rig.controller.playhead() >= f(6));
    REQUIRE(waitFor([&] { return !rig.playback->playing(); }, 3000));
    CHECK(rig.controller.playhead() == f(9));
    // On the last marked frame (or past it) there is nothing to play.
    rig.playback->playToOut();
    CHECK_FALSE(rig.playback->playing());
    rig.controller.setPlayhead(f(12));
    rig.playback->playToOut();
    CHECK_FALSE(rig.playback->playing());
    // Loop on is ignored: it stops all the same, and Loop is back after.
    rig.playback->setLoop(true);
    rig.controller.setPlayhead(f(5));
    rig.playback->playToOut();
    CHECK(rig.playback->playing());
    REQUIRE(waitFor([&] { return !rig.playback->playing(); }, 3000));
    CHECK(rig.controller.playhead() == f(9));
    CHECK(rig.playback->session()->loop() == TimeRange::fromStartEnd(f(4), f(10)));
    rig.playback->setLoop(false);
}

TEST_CASE("Source Monitor Play Around and Play from Playhead to Out", "[app][monitor][transport]") {
    resetSettings();
    Timeline tl(kPicture, {{'S', 50}});
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    SourceMonitorBridge source;
    source.setController(&controller);
    REQUIRE(source.load(mediaKey(tl.media('S'))));
    // Play Around the frame shown: back on it at the end.
    source.seek(f(20));
    source.playAround(0.2, 0.2);
    CHECK(source.playing());
    REQUIRE(waitFor([&] { return !source.playing(); }, 3000));
    CHECK(source.positionTicks() == f(20));
    // Play from the frame shown to the source Out (the key while the Source Monitor has focus).
    source.seek(f(10));
    source.markIn();
    source.seek(f(19));
    source.markOut(); // Out after frame 19
    source.seek(f(12));
    REQUIRE(source.runCommand(QStringLiteral("transport.playFromPlayheadToOut")));
    CHECK(source.playing());
    REQUIRE(waitFor([&] { return !source.playing(); }, 3000));
    CHECK(source.positionTicks() == f(19));
    source.playToOut(); // on the last marked frame: nothing
    CHECK_FALSE(source.playing());
}

TEST_CASE("Source Monitor Loop repeats the source In to Out", "[app][monitor][loop]") {
    resetSettings();
    Timeline tl(kPicture, {{'S', 50}});
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    SourceMonitorBridge source;
    source.setController(&controller);
    REQUIRE(source.load(mediaKey(tl.media('S'))));
    source.seek(f(10));
    source.markIn();
    source.seek(f(19));
    source.markOut();
    CHECK_FALSE(source.playback()->loop());
    REQUIRE(source.runCommand(QStringLiteral("monitor.loop"))); // the key while the Source Monitor has focus
    CHECK(source.loop());
    CHECK(QSettings().value(QStringLiteral("monitor/sourceLoop")).toBool());
    CHECK(source.playback()->loop() == TimeRange::fromStartEnd(f(10), f(20)));
    // Play from outside the range starts at its In and keeps going round.
    source.seek(f(30));
    source.play();
    CHECK(source.positionTicks() >= f(10));
    spin(800);
    CHECK(source.playing());
    CHECK(source.positionTicks() < f(20));
    source.pause();
    source.setLoop(false);
    CHECK_FALSE(source.playback()->loop());
}

TEST_CASE("The Monitors bridge routes Loop Play In to Out and Nudge Selected Object", "[app][monitor]") {
    ProgramRig rig;
    MotionBridge motion;
    motion.setController(&rig.controller);
    MonitorBridge monitors;
    monitors.setPlayback(rig.playback.get());
    monitors.setMotion(&motion);
    CHECK(MonitorBridge::handlesCommand(QStringLiteral("monitor.loop")));
    CHECK(MonitorBridge::handlesCommand(QStringLiteral("programMonitor.nudgeLeftFive")));
    CHECK_FALSE(MonitorBridge::handlesCommand(QStringLiteral("timeline.rippleDelete")));
    CHECK_FALSE(monitors.runCommand(QStringLiteral("timeline.rippleDelete")));

    REQUIRE(monitors.runCommand(QStringLiteral("monitor.loop")));
    CHECK(rig.playback->loop());
    REQUIRE(monitors.runCommand(QStringLiteral("monitor.loop")));
    CHECK_FALSE(rig.playback->loop());

    // Nudges move the clip whose Motion is selected: Ctrl+Left by one, Ctrl+Shift+Down by five.
    const ClipId clip = rig.tl.clip("V1@0");
    rig.controller.selectClip(clipKey(clip));
    rig.controller.setPlayhead(f(3));
    motion.setEngaged(true);
    REQUIRE(motion.active());
    REQUIRE(monitors.runCommand(QStringLiteral("programMonitor.nudgeLeft")));
    REQUIRE(monitors.runCommand(QStringLiteral("programMonitor.nudgeDownFive")));
    const Clip& c = rig.controller.snapshot()->findSequence(rig.tl.seq)->findClip(clip)->clip();
    const edit::MotionValues v = edit::motionAt(c, c.curveTimeAt(f(3)));
    CHECK(v.positionX == -1.0);
    CHECK(v.positionY == 5.0);

    rig.mark(2, 6);
    REQUIRE(monitors.runCommand(QStringLiteral("transport.playInToOut")));
    CHECK(rig.playback->playing());
    rig.playback->togglePlay();
}

TEST_CASE("Toggle Full Screen and Transmit show the program in its own window through a mirror", "[app][monitor][output]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    auto* monitors = shell.engine.singletonInstance<MonitorBridge*>("Wabcut", "Monitors");
    auto* playback = shell.engine.singletonInstance<PlaybackBridge*>("Wabcut", "Playback");
    REQUIRE(monitors);
    REQUIRE(playback);
    monitors->setPresentWindows(false); // never on screen in a test run
    QQuickWindow* output = nullptr;
    for (QWindow* w : QGuiApplication::allWindows()) {
        if (w->objectName() == QStringLiteral("programOutputWindow"))
            output = qobject_cast<QQuickWindow*>(w);
    }
    REQUIRE(output);
    CHECK_FALSE(output->isVisible());
    CHECK_FALSE(monitors->outputActive());
    CHECK(playback->mirror() == nullptr);

    // Ctrl+` (monitor.toggleFullScreen): the mirror runs and feeds the window's monitor, on the main window's display.
    REQUIRE(monitors->runCommand(QStringLiteral("monitor.toggleFullScreen")));
    CHECK(monitors->fullScreen());
    CHECK(monitors->outputActive());
    CHECK_FALSE(monitors->outputVisible()); // windows are not allowed here
    REQUIRE(playback->mirror() != nullptr);
    auto* picture = output->findChild<ProgramMonitor*>(QStringLiteral("programOutputPicture"));
    REQUIRE(picture);
    CHECK(picture->playbackController() == playback->mirror());
    CHECK_FALSE(picture->shareDecoderDevice());
    CHECK(output->screen() == shell.window->screen());
    CHECK_FALSE(output->isVisible());
    REQUIRE(monitors->runCommand(QStringLiteral("monitor.toggleFullScreen")));
    CHECK_FALSE(monitors->outputActive());
    CHECK(playback->mirror() == nullptr);
    CHECK(picture->playbackController() == nullptr);

    // Transmit to display N (Premiere's Mercury Transmit): remembered, on that display, without the focus.
    const QVariantList screens = monitors->screens();
    REQUIRE_FALSE(screens.isEmpty());
    const int last = static_cast<int>(screens.size()) - 1;
    monitors->setTransmitScreen(last);
    REQUIRE(monitors->runCommand(QStringLiteral("monitor.enableTransmit")));
    CHECK(monitors->transmit());
    CHECK(QSettings().value(QStringLiteral("monitor/transmit")).toBool());
    CHECK(QSettings().value(QStringLiteral("monitor/transmitScreen")).toInt() == last);
    CHECK(monitors->outputScreen() == last);
    CHECK(monitors->outputActive());
    CHECK(playback->mirror() != nullptr);
    CHECK(output->screen() == QGuiApplication::screens().at(last));
    CHECK((output->flags() & Qt::WindowDoesNotAcceptFocus) != 0);
    // A display that is gone: nothing to transmit to.
    monitors->setTransmitScreen(static_cast<int>(screens.size()) + 3);
    CHECK_FALSE(monitors->outputActive());
    CHECK(playback->mirror() == nullptr);
    monitors->setTransmit(false);
    monitors->setTransmitScreen(-1);
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Guides files and settings are bounded and checked", "[app][monitor][guides]") {
    using namespace wabcut::app::guides;
    std::vector<Guide> set{Guide{false, 540, false, QColor(255, 0, 0)}, Guide{true, 25.5, true, defaultColor()}};
    const auto back = fromJson(toJson(set));
    REQUIRE(back);
    CHECK(*back == set);
    CHECK(set[0].fraction(1920, 1080) == 0.5);
    CHECK(set[1].fraction(1920, 1080) == 0.255);
    CHECK(set[1].valueAt(0.5, 1920, 1080) == 50.0);
    CHECK(Guide{true, 480, false}.valueAt(0.5, 1920, 1080) == 960.0);

    const auto parse = [](const char* json) { return fromJson(QJsonDocument::fromJson(json).array()); };
    CHECK_FALSE(parse(R"([{"orientation": "diagonal", "position": 1}])"));
    CHECK_FALSE(parse(R"([{"orientation": "vertical", "position": 1e9}])"));
    CHECK_FALSE(parse(R"([{"orientation": "vertical", "position": 2000, "units": "percent"}])"));
    CHECK_FALSE(parse(R"([{"orientation": "vertical", "position": "12"}])"));
    CHECK_FALSE(parse(R"([{"orientation": "vertical", "position": 1, "color": "not a colour"}])"));
    CHECK_FALSE(parse(R"([3])"));
    QJsonArray many;
    for (int i = 0; i <= kMaxGuides; ++i)
        many.append(QJsonObject{{QStringLiteral("orientation"), QStringLiteral("vertical")}, {QStringLiteral("position"), i}});
    CHECK_FALSE(fromJson(many));

    const std::vector<Template> templates{{QStringLiteral("Thirds"), set}, {QStringLiteral("Centre"), {set[0]}}};
    const auto file = importFile(exportFile(templates));
    REQUIRE(file);
    CHECK(*file == templates);
    CHECK_FALSE(importFile(R"({"format": "something else"})"));
    CHECK_FALSE(importFile(R"({"format": "wabcut-guides", "version": 99, "templates": []})"));
    CHECK_FALSE(importFile(
        R"({"format": "wabcut-guides", "version": 1, "templates": [{"name": "A", "guides": []}, {"name": "a", "guides": []}]})"));
    CHECK_FALSE(importFile(R"({"format": "wabcut-guides", "version": 1, "templates": [{"name": "", "guides": []}]})"));
    CHECK_FALSE(importFile("not json"));
}

TEST_CASE("Program Monitor guides: add, move, lock, clear, templates, files, and they persist", "[app][monitor][guides]") {
    resetSettings();
    Timeline tl(kPicture);
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    MotionBridge motion;
    motion.setController(&controller);
    auto monitors = std::make_unique<MonitorBridge>();
    monitors->setController(&controller);
    monitors->setMotion(&motion);
    CHECK(monitors->frameWidth() == 1920);
    CHECK(monitors->guides().isEmpty());

    // Keys and menus.
    CHECK_FALSE(monitors->showRulers());
    REQUIRE(monitors->runCommand(QStringLiteral("programMonitor.showRulers"))); // Ctrl+R
    CHECK(monitors->showRulers());
    REQUIRE(monitors->runCommand(QStringLiteral("programMonitor.showGuides"))); // Ctrl+;
    CHECK_FALSE(monitors->showGuides());
    monitors->setShowGuides(true);
    QString requested;
    QObject::connect(monitors.get(), &MonitorBridge::dialogRequested, [&](const QString& d, int) { requested = d; });
    REQUIRE(monitors->runCommand(QStringLiteral("programMonitor.addGuide")));
    CHECK(requested == QStringLiteral("addGuide"));
    REQUIRE(monitors->runCommand(QStringLiteral("programMonitor.manageGuides")));
    CHECK(requested == QStringLiteral("manageGuides"));

    // Add Guide: a horizontal one at 540 px and a vertical one at 25 %.
    CHECK(monitors->addGuide(QStringLiteral("horizontal"), 540, QStringLiteral("pixels")) == 0);
    CHECK(monitors->addGuide(QStringLiteral("vertical"), 25, QStringLiteral("percent"), QStringLiteral("#ff5252")) == 1);
    CHECK(monitors->addGuide(QStringLiteral("sideways"), 1, QStringLiteral("pixels")) == -1);
    CHECK(monitors->addGuide(QStringLiteral("vertical"), 1e9, QStringLiteral("pixels")) == -1);
    QVariantList list = monitors->guides();
    REQUIRE(list.size() == 2);
    CHECK(list[0].toMap().value(QStringLiteral("position")).toDouble() == 0.5);
    CHECK(list[1].toMap().value(QStringLiteral("position")).toDouble() == 0.25);
    CHECK(list[1].toMap().value(QStringLiteral("color")).toString() == QStringLiteral("#ff5252"));
    // Dragged: whole pixels; a percent guide keeps percent.
    REQUIRE(monitors->moveGuide(0, 0.3337));
    CHECK(monitors->guideList()[0].value == 360.0);
    REQUIRE(monitors->moveGuide(1, 0.5));
    CHECK(monitors->guideList()[1].value == 50.0);
    // Locked: no moving or removing.
    REQUIRE(monitors->runCommand(QStringLiteral("programMonitor.lockGuides")));
    CHECK_FALSE(monitors->moveGuide(0, 0.1));
    CHECK_FALSE(monitors->removeGuide(0));
    monitors->setLockGuides(false);

    // The guides reach Motion's snapping (Snap in Program Monitor).
    const ClipId clip = tl.clip("V1@0");
    controller.selectClip(clipKey(clip));
    controller.setPlayhead(f(2));
    edit::MotionChange half;
    half.scale = 0.5;
    REQUIRE(controller.applyEditFn("half", [&](const ProjectState& s) {
        return edit::setMotion(s, tl.seq, clip, f(2), half);
    }));
    motion.setEngaged(true);
    REQUIRE(motion.active());
    // The box (480..1440) dragged right by 470 px: its left edge at 950 snaps to the 50 % guide at 960.
    REQUIRE(motion.beginDrag(QStringLiteral("move"), 0.4, 0.4));
    motion.dragTo(0.4 + 470.0 / 1920.0, 0.4, false, false, false, 12.0 / 1920.0);
    motion.endDrag();
    {
        const Clip& c = controller.snapshot()->findSequence(tl.seq)->findClip(clip)->clip();
        CHECK(edit::motionAt(c, c.curveTimeAt(f(2))).positionX == 480.0);
    }
    // Snapping off: no snap.
    REQUIRE(monitors->runCommand(QStringLiteral("programMonitor.snap"))); // Ctrl+Shift+;
    CHECK_FALSE(motion.snapping());
    REQUIRE(motion.beginDrag(QStringLiteral("move"), 0.4, 0.4));
    motion.dragTo(0.4 + 3.0 / 1920.0, 0.4, false, false, false, 12.0 / 1920.0);
    motion.endDrag();
    {
        const Clip& c = controller.snapshot()->findSequence(tl.seq)->findClip(clip)->clip();
        CHECK(edit::motionAt(c, c.curveTimeAt(f(2))).positionX == Approx(483.0));
    }
    monitors->setSnap(true);

    // Templates: save, apply, remove; export and import a guides file.
    CHECK_FALSE(monitors->saveGuidesAsTemplate(QStringLiteral("  ")).isEmpty());
    CHECK(monitors->saveGuidesAsTemplate(QStringLiteral("Lower third")).isEmpty());
    CHECK(monitors->guideTemplates() == QStringList{QStringLiteral("Lower third")});
    monitors->clearGuides();
    CHECK(monitors->guides().isEmpty());
    CHECK_FALSE(monitors->saveGuidesAsTemplate(QStringLiteral("Empty")).isEmpty()); // no guides
    REQUIRE(monitors->applyGuideTemplate(QStringLiteral("Lower third")));
    CHECK(monitors->guides().size() == 2);
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QUrl file = QUrl::fromLocalFile(dir.filePath(QStringLiteral("mine.guides")));
    CHECK(monitors->exportGuideTemplates({}, file).isEmpty());
    REQUIRE(monitors->removeGuideTemplate(QStringLiteral("Lower third")));
    CHECK(monitors->guideTemplates().isEmpty());
    CHECK(monitors->importGuideTemplates(file).isEmpty());
    CHECK(monitors->guideTemplates() == QStringList{QStringLiteral("Lower third")});
    CHECK_FALSE(monitors->importGuideTemplates(QUrl::fromLocalFile(dir.filePath(QStringLiteral("missing.guides")))).isEmpty());

    // Like Premiere's, they persist across projects and sessions (the user's settings).
    monitors.reset();
    MonitorBridge again;
    again.setController(&controller);
    CHECK(again.guides().size() == 2);
    CHECK(again.guideTemplates() == QStringList{QStringLiteral("Lower third")});
    CHECK(again.showRulers());
    // A pixel guide sits elsewhere on another frame size; a percent guide stays.
    auto tall = std::make_shared<Sequence>(*controller.currentSequence());
    tall->width = 1080;
    tall->height = 1920;
    controller.resetProject(controller.snapshot()->withSequence(tall), tl.seq);
    list = again.guides();
    CHECK(list[0].toMap().value(QStringLiteral("position")).toDouble() == 360.0 / 1920.0);
    CHECK(list[1].toMap().value(QStringLiteral("position")).toDouble() == 0.5);
    again.clearGuides();
}

TEST_CASE("Program Monitor view settings: zoom levels, dropped frames, overlays, audio time units", "[app][monitor]") {
    resetSettings();
    Timeline tl(kPicture);
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    MonitorBridge monitors;
    monitors.setController(&controller);

    // Premiere's Select Zoom Level list besides Fit.
    const QVariantList zoom = MonitorBridge::zoomLevels();
    REQUIRE(zoom.size() == 10);
    CHECK(zoom.front().toDouble() == 0.1);
    CHECK(zoom.back().toDouble() == 16.0);

    // Show Dropped Frame Indicator, Overlays and Show Audio Time Units: off by default, toggled by their commands,
    // remembered.
    CHECK_FALSE(monitors.droppedFrameIndicator());
    REQUIRE(monitors.runCommand(QStringLiteral("programMonitor.droppedFrameIndicator")));
    CHECK(monitors.droppedFrameIndicator());
    CHECK(QSettings().value(QStringLiteral("monitor/droppedFrameIndicator")).toBool());

    // The readout: a timecode, or seconds and audio samples (48 kHz).
    controller.setPlayhead(f(30)); // 1.2 s at 25 fps
    CHECK(monitors.programTimeText() == QStringLiteral("00:00:01:05"));
    REQUIRE(monitors.runCommand(QStringLiteral("programMonitor.audioTimeUnits")));
    CHECK(monitors.audioTimeUnits());
    CHECK(monitors.programTimeText() == QStringLiteral("00:00:01:09600"));

    // Overlays: the clip under the playhead and its source timecode, and the sequence timecode.
    REQUIRE(monitors.runCommand(QStringLiteral("programMonitor.overlays")));
    CHECK(monitors.overlays());
    controller.setPlayhead(f(12));
    const QVariantMap overlay = monitors.overlayText();
    CHECK(overlay.value(QStringLiteral("sequence")).toString() == QStringLiteral("00:00:00:12"));
    CHECK(overlay.value(QStringLiteral("clip")).toString() == QStringLiteral("A"));
    CHECK(overlay.value(QStringLiteral("source")).toString() == QStringLiteral("00:00:00:12"));
    controller.setPlayhead(f(40)); // past the clips
    CHECK_FALSE(monitors.overlayText().contains(QStringLiteral("source")));

    MonitorBridge again;
    CHECK(again.droppedFrameIndicator());
    CHECK(again.overlays());
    CHECK(again.audioTimeUnits());
}

TEST_CASE("Main.qml's Program Monitor has the settings menu, rulers, guides and the new view items", "[app][monitor]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* monitors = shell.engine.singletonInstance<MonitorBridge*>("Wabcut", "Monitors");
    REQUIRE(monitors);
    monitors->setPresentWindows(false);
    const std::function<QQuickItem*(QQuickItem*, const QString&)> named = [&](QQuickItem* item, const QString& name) -> QQuickItem* {
        if (!item)
            return nullptr;
        if (item->objectName() == name)
            return item;
        for (QQuickItem* child : item->childItems()) {
            if (QQuickItem* found = named(child, name))
                return found;
        }
        return nullptr;
    };
    const auto find = [&](const char* name) { return named(shell.window->contentItem(), QString::fromLatin1(name)); };
    REQUIRE(find("programMonitorSettingsButton"));
    REQUIRE(find("programGuidesOverlay"));
    QQuickItem* rulers = find("programRulers");
    REQUIRE(rulers);
    CHECK_FALSE(rulers->isVisible());
    monitors->setShowRulers(true);
    CHECK(rulers->isVisible());
    monitors->addGuide(QStringLiteral("vertical"), 50, QStringLiteral("percent"));
    REQUIRE(waitFor([&] { return find("programGuide_0") != nullptr; }, 3000));
    monitors->setDroppedFrameIndicator(true);
    QQuickItem* light = find("droppedFrameIndicator");
    REQUIRE(light);
    CHECK(light->isVisible());
    monitors->setOverlays(true);
    REQUIRE(find("programOverlays"));
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    CHECK(image.save(QDir(outputDir()).filePath(QStringLiteral("monitor-rulers-guides.png"))));
    monitors->clearGuides();
    monitors->setShowRulers(false);
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Export Frame dialog writes the Program and Source frames and imports one into the project",
          "[app][monitor][exportframe][gpu]") {
    resetSettings();
    Timeline tl(R"(
        V1 tgt |............|
        A1 tgt |............|
    )");
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    {
        command::CommandEngine engine(controller.session());
        command::CommandRequest title;
        title.command = "title.add";
        title.params = command::Json{{"text", "Export Frame"}, {"at", 0}, {"duration", f(12).value}, {"track", "V1"},
                                     {"sequence", tl.seq.value}};
        REQUIRE(engine.execute(title).ok());
    }
    controller.setPlayhead(f(4));
    SourceMonitorBridge source;
    source.setController(&controller);
    MonitorBridge monitors;
    monitors.setController(&controller);
    monitors.setSourceMonitor(&source);
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QUrl folder = QUrl::fromLocalFile(dir.path());

    // The dialog's defaults: the sequence's name with the playhead's timecode, Premiere's eight formats, PNG, no import.
    const QVariantMap defaults = monitors.exportFrameDefaults(false);
    CHECK(defaults.value(QStringLiteral("available")).toBool());
    CHECK(defaults.value(QStringLiteral("formats")).toList().size() == 8);
    CHECK(defaults.value(QStringLiteral("format")).toString() == QStringLiteral("png"));
    CHECK_FALSE(defaults.value(QStringLiteral("importIntoProject")).toBool());
    const QString name = defaults.value(QStringLiteral("name")).toString();
    CHECK(name.endsWith(QStringLiteral("00;00;00;04")));
    // Nothing loaded in the Source Monitor: nothing to export there.
    CHECK_FALSE(monitors.exportFrameDefaults(true).value(QStringLiteral("available")).toBool());
    CHECK(monitors.exportFrame(true, QStringLiteral("Frame"), QStringLiteral("jpeg"), folder, false).isEmpty());
    CHECK_FALSE(monitors.message().isEmpty());
    // A missing folder or an unknown format is refused before anything runs.
    CHECK(monitors.exportFrame(false, name, QStringLiteral("bmp"), QUrl::fromLocalFile(dir.filePath(QStringLiteral("none"))),
                               false)
              .isEmpty());
    CHECK(monitors.exportFrame(false, name, QStringLiteral("webp"), folder, false).isEmpty());

    // The Program Monitor's frame as BMP, imported into the project as one undo step.
    const std::size_t mediaBefore = controller.snapshot()->media.size();
    const QString program = monitors.exportFrame(false, name, QStringLiteral("bmp"), folder, true);
    INFO(monitors.message().toStdString());
    REQUIRE_FALSE(program.isEmpty());
    CHECK(QFileInfo(program).fileName() == name + QStringLiteral(".bmp"));
    const QImage written(program);
    REQUIRE_FALSE(written.isNull());
    CHECK(written.width() == controller.currentSequence()->width);
    CHECK(written.height() == controller.currentSequence()->height);
    const ProjectSnapshot snap = controller.snapshot();
    REQUIRE(snap->media.size() == mediaBefore + 1);
    const MediaItem& still = *snap->media.back();
    CHECK(still.info.hasVideo());
    CHECK(controller.session().undoLabel() == "Export Frame");
    // The choices are remembered for the next time.
    const QVariantMap again = monitors.exportFrameDefaults(false);
    CHECK(again.value(QStringLiteral("format")).toString() == QStringLiteral("bmp"));
    CHECK(again.value(QStringLiteral("importIntoProject")).toBool());
    CHECK(QDir(again.value(QStringLiteral("folder")).toUrl().toLocalFile()) == QDir(dir.path()));

    // The imported still in the Source Monitor, exported from there as JPEG at its own size.
    REQUIRE(source.load(mediaKey(still.id)));
    const QVariantMap sourceDefaults = monitors.exportFrameDefaults(true);
    CHECK(sourceDefaults.value(QStringLiteral("available")).toBool());
    const QString sourceName = sourceDefaults.value(QStringLiteral("name")).toString();
    CHECK(sourceName.startsWith(QString::fromStdString(still.name)));
    const QString sourceFile = monitors.exportFrame(true, sourceName, QStringLiteral("jpeg"), folder, false);
    INFO(monitors.message().toStdString());
    REQUIRE_FALSE(sourceFile.isEmpty());
    CHECK(sourceFile.endsWith(QStringLiteral(".jpg")));
    const QImage sourceImage(sourceFile);
    REQUIRE_FALSE(sourceImage.isNull());
    CHECK(sourceImage.size() == written.size());
    CHECK(controller.snapshot()->media.size() == mediaBefore + 1); // not imported this time
}

TEST_CASE("Transparency Grid in both monitors: a checkerboard under the picture and the frame's alpha kept",
          "[app][monitor]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight); // wide: both monitors side by side
    auto* monitors = shell.engine.singletonInstance<MonitorBridge*>("Wabcut", "Monitors");
    REQUIRE(monitors);
    const std::function<QQuickItem*(QQuickItem*, const QString&)> named = [&](QQuickItem* item, const QString& name) -> QQuickItem* {
        if (!item)
            return nullptr;
        if (item->objectName() == name)
            return item;
        for (QQuickItem* child : item->childItems()) {
            if (QQuickItem* found = named(child, name))
                return found;
        }
        return nullptr;
    };
    const auto find = [&](const char* name) { return named(shell.window->contentItem(), QString::fromLatin1(name)); };
    QQuickItem* program = find("programMonitor");
    QQuickItem* source = find("sourceMonitorPicture");
    QQuickItem* programGrid = find("programTransparencyGrid");
    QQuickItem* sourceGrid = find("sourceTransparencyGrid");
    REQUIRE(program);
    REQUIRE(source);
    REQUIRE(programGrid);
    REQUIRE(sourceGrid);
    // The tile is Premiere's white and grey checkerboard.
    CHECK(programGrid->property("status").toInt() == 1); // Image.Ready
    const QImage tile(QStringLiteral(":/qt/qml/Wabcut/resources/monitor/transparency-grid.svg"));
    REQUIRE(tile.size() == QSize(16, 16));
    CHECK(tile.pixelColor(2, 2) == QColor(200, 200, 200));
    CHECK(tile.pixelColor(12, 2) == QColor(255, 255, 255));
    CHECK(tile.pixelColor(12, 12) == QColor(200, 200, 200));

    // Off by default: the monitors composite over black.
    CHECK_FALSE(monitors->programTransparencyGrid());
    CHECK_FALSE(program->property("transparencyGrid").toBool());
    CHECK_FALSE(source->property("transparencyGrid").toBool());
    // Each monitor's own setting: the Program Monitor keeps the frame's alpha, the Source Monitor does not.
    REQUIRE(monitors->runCommand(QStringLiteral("programMonitor.transparencyGrid")));
    CHECK(monitors->programTransparencyGrid());
    CHECK(program->property("transparencyGrid").toBool());
    CHECK_FALSE(source->property("transparencyGrid").toBool());
    CHECK_FALSE(programGrid->isVisible()); // nothing shown yet: no picture to see through
    REQUIRE(monitors->runCommand(QStringLiteral("sourceMonitor.transparencyGrid"), true));
    CHECK(monitors->sourceTransparencyGrid());
    CHECK(source->property("transparencyGrid").toBool());
    // Remembered.
    CHECK(QSettings().value(QStringLiteral("monitor/programTransparencyGrid")).toBool());
    MonitorBridge again;
    CHECK(again.programTransparencyGrid());
    CHECK(again.sourceTransparencyGrid());
    REQUIRE(monitors->runCommand(QStringLiteral("programMonitor.transparencyGrid")));
    CHECK_FALSE(program->property("transparencyGrid").toBool());
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// ---- 0.4: platform safe zones and transports that fit (the window) -------------------------------------------------

namespace {

QQuickItem* findIn(QQuickItem* root, const QString& name) {
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findIn(child, name))
            return found;
    }
    return nullptr;
}

/// The editor at `width` x `height`, motion off, Transmit's windows not shown.
struct MonitorShell {
    Shell shell;
    ProjectController* controller = nullptr;

    explicit MonitorShell(int width = kShotWidth, int height = kShotHeight) {
        REQUIRE(shell.window != nullptr);
        INFO(join(shell.errors));
        REQUIRE(shell.errors.isEmpty());
        shell.resize(width, height);
        shell.theme->setProperty("reduceMotion", true);
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        REQUIRE(controller != nullptr);
        auto* monitors = shell.engine.singletonInstance<MonitorBridge*>("Wabcut", "Monitors");
        REQUIRE(monitors != nullptr);
        monitors->setPresentWindows(false);
        spin(50);
    }
    QQuickItem* find(const char* name) const {
        QQuickItem* item = findIn(shell.window->contentItem(), QString::fromLatin1(name));
        INFO(name);
        REQUIRE(item != nullptr);
        return item;
    }
    /// The current sequence's frame size (Sequence Settings, one undo step).
    void setFrame(int width, int height) const {
        command::CommandEngine engine(controller->session());
        command::CommandRequest request;
        request.command = "sequence.setSettings";
        request.params = command::Json{{"sequence", controller->currentSequenceId().value}, {"width", width}, {"height", height}};
        const auto result = engine.execute(request);
        INFO(command::toJson(result).dump());
        REQUIRE(result.ok());
        REQUIRE(controller->frameWidth() == width);
        spin(20);
    }
    /// A title in the Source Monitor (it renders without media files), marked 1-5 s.
    void loadTitleSource() {
        command::CommandEngine engine(controller->session());
        command::CommandRequest title;
        title.command = "title.add";
        title.params = command::Json{{"text", "Source Monitor"},
                                     {"at", controller->currentSequence()->contentEnd().value},
                                     {"duration", Ticks::fromSeconds(8).value},
                                     {"track", "V3"},
                                     {"sequence", controller->currentSequenceId().value}};
        const auto added = engine.execute(title);
        INFO(command::toJson(added).dump());
        REQUIRE(added.ok());
        MediaId titleMedia;
        for (const MediaPtr& m : controller->snapshot()->media) {
            if (m->isTitle())
                titleMedia = m->id;
        }
        auto* source = shell.engine.singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor");
        REQUIRE(source != nullptr);
        REQUIRE(source->load(mediaKey(titleMedia)));
        source->seek(Ticks::fromSeconds(1));
        source->markIn();
        source->seek(Ticks::fromSeconds(5));
        source->markOut();
        source->seek(Ticks::fromSeconds(2));
    }
    /// The transport Item of the monitor whose overflow button is `overflowName`.
    QQuickItem* transportOf(const char* overflowName) const {
        QQuickItem* row = find(overflowName)->parentItem();
        REQUIRE(row != nullptr);
        REQUIRE(row->parentItem() != nullptr);
        return row->parentItem();
    }
    /// Every shown button of the transport lies inside it; returns how many are hidden.
    int checkInside(const char* overflowName) const {
        QQuickItem* transport = transportOf(overflowName);
        QQuickItem* row = find(overflowName)->parentItem();
        CHECK(row->width() <= transport->width() + 0.5);
        int hidden = 0;
        for (QQuickItem* child : row->childItems()) {
            if (!child->isVisible()) {
                ++hidden;
                continue;
            }
            const QPointF at = child->mapToItem(transport, QPointF(0, 0));
            INFO(overflowName << ": " << child->objectName().toStdString() << " at " << at.x() << " + " << child->width()
                              << " in " << transport->width());
            CHECK(at.x() >= -0.5);
            CHECK(at.x() + child->width() <= transport->width() + 0.5);
        }
        return hidden;
    }
};

bool shown(QObject* popup) { return popup && popup->property("visible").toBool(); }

} // namespace

TEST_CASE("Platform safe zones show on vertical sequences only", "[app][monitor][safezones][qml]") {
    resetSettings();
    clearMessages();
    MonitorShell s;
    QQuickItem* zone = s.find("platformSafeZone");
    QQuickItem* frame = s.find("programFrameArea");
    CHECK_FALSE(zone->isVisible()); // off by default
    s.shell.preferences->setProperty("platformSafeZone", QStringLiteral("shorts"));
    CHECK_FALSE(zone->isVisible()); // the sample sequence is 16:9
    s.setFrame(1080, 1920);
    REQUIRE(waitFor([&] { return zone->isVisible(); }));
    CHECK_FALSE(zone->isEnabled()); // no input: clicks reach the picture
    CHECK(zone->width() == Approx(frame->width()).margin(1));
    CHECK(zone->height() == Approx(frame->height()).margin(1));
    CHECK(frame->height() > frame->width());
    // Shorts: the top bar, the bottom band and the right-hand column.
    CHECK(s.find("platformSafeTop")->height() == Approx(0.08 * zone->height()).margin(1));
    CHECK(s.find("platformSafeBottom")->y() == Approx(0.79 * zone->height()).margin(1));
    CHECK(s.find("platformSafeBottom")->width() == Approx(0.86 * zone->width()).margin(1));
    QQuickItem* right = s.find("platformSafeRight");
    CHECK(right->x() == Approx(0.86 * zone->width()).margin(1));
    CHECK(right->y() == Approx(0.45 * zone->height()).margin(1));
    CHECK(right->y() + right->height() == Approx(0.79 * zone->height()).margin(1));
    QQuickItem* label = s.find("platformSafeLabel");
    CHECK(label->isVisible());
    const QList<QQuickItem*> labelText = label->childItems();
    REQUIRE_FALSE(labelText.isEmpty());
    CHECK(labelText.front()->property("text").toString() == QStringLiteral("Shorts UI"));
    // TikTok and Reels move the bands.
    s.shell.preferences->setProperty("platformSafeZone", QStringLiteral("reels"));
    CHECK(s.find("platformSafeTop")->height() == Approx(0.10 * zone->height()).margin(1));
    CHECK(right->y() == Approx(0.50 * zone->height()).margin(1));
    // Off, landscape, the Trim view or the multi-camera view: nothing.
    s.shell.preferences->setProperty("platformSafeZone", QStringLiteral("off"));
    CHECK_FALSE(zone->isVisible());
    s.shell.preferences->setProperty("platformSafeZone", QStringLiteral("tiktok"));
    CHECK(zone->isVisible());
    QObject* multicam = s.shell.engine.singletonInstance<QObject*>("Wabcut", "Multicam");
    REQUIRE(multicam != nullptr);
    multicam->setProperty("viewOn", true);
    CHECK_FALSE(zone->isVisible());
    multicam->setProperty("viewOn", false);
    CHECK(zone->isVisible());
    s.setFrame(1920, 1080);
    CHECK_FALSE(zone->isVisible());
    s.setFrame(1080, 1080); // square
    CHECK_FALSE(zone->isVisible());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Platform safe zones menu switches between Shorts TikTok and Reels", "[app][monitor][safezones][qml]") {
    resetSettings();
    clearMessages();
    MonitorShell s;
    QObject* menu = s.shell.object("platformSafeZoneMenu");
    REQUIRE(menu != nullptr);
    CHECK(menu->property("title").toString() == QStringLiteral("Platform Safe Zones"));
    // The wrench menu and its Platform Safe Zones submenu, open (a row's visibility follows its menu's).
    QObject* settings = s.shell.object("programMonitorSettingsMenu");
    REQUIRE(settings != nullptr);
    REQUIRE(QMetaObject::invokeMethod(s.find("programMonitorSettingsButton"), "clicked"));
    REQUIRE(waitFor([&] { return settings->property("opened").toBool(); }));
    REQUIRE(QMetaObject::invokeMethod(menu, "open"));
    REQUIRE(waitFor([&] { return menu->property("opened").toBool(); }));
    QObject* hint = menu->findChild<QObject*>(QStringLiteral("platformSafeZoneHint"));
    REQUIRE(hint != nullptr);
    CHECK(hint->property("visible").toBool()); // a landscape sequence: the hint says when they show
    CHECK_FALSE(hint->property("enabled").toBool());
    QObject* off = menu->findChild<QObject*>(QStringLiteral("platformSafeZone_off"));
    REQUIRE(off != nullptr);
    CHECK(off->property("visible").toBool());
    CHECK(off->property("checked").toBool());
    s.setFrame(1080, 1920);
    CHECK_FALSE(hint->property("visible").toBool());
    CHECK(settings->findChild<QObject*>(QStringLiteral("programSafeMarginsItem")) != nullptr);
    REQUIRE(QMetaObject::invokeMethod(menu, "close"));
    REQUIRE(QMetaObject::invokeMethod(settings, "close"));
    REQUIRE(waitFor([&] { return !shown(settings) && !shown(menu); }));
    for (const char* key : {"shorts", "tiktok", "reels", "off"}) {
        INFO(key);
        QObject* item = menu->findChild<QObject*>(QStringLiteral("platformSafeZone_%1").arg(QLatin1String(key)));
        REQUIRE(item != nullptr);
        CHECK(item->property("checkable").toBool());
        REQUIRE(QMetaObject::invokeMethod(item, "clicked"));
        CHECK(s.shell.preferences->property("platformSafeZone").toString() == QLatin1String(key));
        CHECK(item->property("checked").toBool());
        int checked = 0;
        for (const char* other : {"off", "shorts", "tiktok", "reels"}) {
            checked += menu->findChild<QObject*>(QStringLiteral("platformSafeZone_%1").arg(QLatin1String(other)))
                           ->property("checked").toBool() ? 1 : 0;
        }
        CHECK(checked == 1);
        CHECK(s.find("platformSafeZone")->isVisible() == (QLatin1String(key) != QLatin1String("off")));
    }
    // Remembered (monitor/platformSafeZone).
    s.shell.preferences->setProperty("platformSafeZone", QStringLiteral("tiktok"));
    CHECK(waitFor([] { return QSettings().value(QStringLiteral("monitor/platformSafeZone")).toString() == QStringLiteral("tiktok"); },
                  3000));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Screenshots: platform safe zones on a vertical sequence", "[app][monitor][safezones][qml][screenshot]") {
    struct Shot {
        const char* zone;
        const char* file;
        bool dark;
    };
    for (const Shot shot : {Shot{"shorts", "program-safe-zones-shorts.png", true},
                            Shot{"tiktok", "program-safe-zones-tiktok.png", true},
                            Shot{"reels", "program-safe-zones-reels.png", true},
                            Shot{"shorts", "program-safe-zones-light.png", false}}) {
        INFO(shot.file);
        resetSettings();
        clearMessages();
        MonitorShell s(2560, 1440); // a monitor large enough to read the bands
        s.setFrame(1080, 1920);
        s.shell.preferences->setProperty("platformSafeZone", QString::fromLatin1(shot.zone));
        if (!shot.dark) {
            s.shell.setAppearance(false, "ultraviolet");
            REQUIRE(s.shell.settle(false, s.shell.themeColor("accentFill")));
        }
        REQUIRE(s.find("platformSafeZone")->isVisible());
        const QImage image = s.shell.grab();
        REQUIRE_FALSE(image.isNull());
        // The Program Monitor panel.
        QQuickItem* panel = s.shell.item("monitorPanel");
        REQUIRE(panel != nullptr);
        const double dpr = s.shell.window->effectiveDevicePixelRatio();
        const QRectF r = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
        CHECK(image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect())
                  .save(QDir(outputDir()).filePath(QString::fromLatin1(shot.file))));
        const QStringList messages = takeMessages();
        INFO(join(messages));
        CHECK(messages.isEmpty());
    }
}

TEST_CASE("Platform safe zone edges read on a bright picture", "[app][monitor][safezones][qml][screenshot]") {
    // 0.4 QA: a faint white edge alone was lost on bright footage. Each edge is two-tone (a dark line outside a light
    // one), so it reads on bright and dark pictures. Screenshot program-safe-zones-bright.png.
    resetSettings();
    clearMessages();
    MonitorShell s(2560, 1440);
    s.setFrame(1080, 1920);
    s.shell.preferences->setProperty("platformSafeZone", QStringLiteral("shorts"));
    QQuickItem* zone = s.find("platformSafeZone");
    REQUIRE(zone->isVisible());
    // A white picture over the monitor's own (in its slot, under the overlays), standing in for high-key footage: the
    // sample project has no files to show.
    QQuickItem* slot = s.find("programMonitorSlot");
    QQmlComponent component(&s.shell.engine);
    component.setData("import QtQuick\nRectangle { anchors.fill: parent; color: \"white\" }\n", QUrl());
    std::unique_ptr<QObject> object(component.createWithInitialProperties({{QStringLiteral("parent"), QVariant::fromValue(slot)}}));
    INFO(component.errorString().toStdString());
    REQUIRE(qobject_cast<QQuickItem*>(object.get()) != nullptr);
    REQUIRE_FALSE(s.shell.grab().isNull()); // lays it out
    const QImage image = s.shell.grab();
    REQUIRE_FALSE(image.isNull());
    const double dpr = s.shell.window->effectiveDevicePixelRatio();
    const auto gray = [&](const QQuickItem* item, QPointF at) {
        const QPointF p = item->mapToScene(at) * dpr;
        return qGray(image.pixel(static_cast<int>(p.x()), static_cast<int>(p.y())));
    };
    REQUIRE(gray(zone, QPointF(zone->width() * 0.4, zone->height() * 0.4)) > 230); // the picture is white
    // The top band's lower edge, and the bottom band's upper edge: dark pixels along them. Just outside, the picture.
    for (const char* name : {"platformSafeTop", "platformSafeBottom"}) {
        INFO(name);
        QQuickItem* band = s.find(name);
        const bool top = std::string_view(name) == "platformSafeTop";
        const double edge = top ? band->height() - 0.5 : 0.5;
        int darkest = 255;
        for (double x = band->width() * 0.3; x < band->width() * 0.7; x += 1.0)
            darkest = std::min({darkest, gray(band, QPointF(x, edge)), gray(band, QPointF(x, top ? edge - 1 : edge + 1))});
        CHECK(darkest < 110);
        CHECK(gray(band, QPointF(band->width() / 2, top ? band->height() + 3 : -3)) > 230);
    }
    QQuickItem* panel = s.shell.item("monitorPanel");
    REQUIRE(panel != nullptr);
    const QRectF r = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
    CHECK(image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect())
              .save(QDir(outputDir()).filePath(QStringLiteral("program-safe-zones-bright.png"))));
    object.reset();

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Monitor transports stay inside their panels at 1600x960", "[app][monitor][transport][qml][screenshot]") {
    resetSettings();
    clearMessages();
    MonitorShell s;
    s.loadTitleSource();
    // The source renders on its own thread; a few frames until it presents (each frame lays the hidden window out).
    QQuickItem* picture = s.find("sourceMonitorPicture");
    QImage image;
    waitFor([&] {
        image = s.shell.grab();
        return picture->property("hasFrame").toBool();
    });
    // Both rows fit; what does not fit is in the "···" menu, and the always-shown buttons are there.
    for (const char* overflow : {"programTransportOverflow", "sourceTransportOverflow"}) {
        INFO(overflow);
        const int hidden = s.checkInside(overflow);
        CHECK(s.find(overflow)->isVisible() == (hidden > 0));
    }
    for (const char* always : {"programAddMarker", "programPlay", "programLift", "programExtract", "programRecordVoiceOver",
                               "sourceMarkIn", "sourceMarkOut", "sourcePlay", "sourceInsertButton", "sourceOverwriteButton"}) {
        INFO(always);
        CHECK(s.find(always)->isVisible());
    }
    // A pair shows or hides together; the Source row keeps Go to In / Out before the rest.
    CHECK(s.find("programMarkIn")->isVisible() == s.find("programMarkOut")->isVisible());
    CHECK(s.find("programStepBack")->isVisible() == s.find("programStepForward")->isVisible());
    CHECK(s.find("sourceGoToIn")->isVisible() == s.find("sourceGoToOut")->isVisible());
    CHECK(s.find("sourceGoToIn")->isVisible());
    CHECK(s.find("sourceTransportOverflow")->isVisible());
    REQUIRE_FALSE(image.isNull());
    CHECK(image.save(QDir(outputDir()).filePath(QStringLiteral("monitor-transport-overflow.png"))));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Monitor transport overflow menu runs the hidden buttons", "[app][monitor][transport][qml][screenshot]") {
    // At 1600x960 the Editing workspace's monitors are about 330 px wide: the Program Monitor shows Add Marker, the
    // marks, Play, Lift, Extract and Record; the Source Monitor the marks, Go to In / Out, Play In to Out, Play, Insert
    // and Overwrite. The rest is in their "···" menus.
    resetSettings();
    clearMessages();
    MonitorShell s;
    s.loadTitleSource();
    auto* playback = s.shell.engine.singletonInstance<PlaybackBridge*>("Wabcut", "Playback");
    REQUIRE(playback != nullptr);
    REQUIRE_FALSE(s.shell.grab().isNull()); // a frame lays the hidden window out
    INFO("program transport " << s.transportOf("programTransportOverflow")->width());
    QQuickItem* overflow = s.find("programTransportOverflow");
    REQUIRE(overflow->isVisible());
    REQUIRE_FALSE(s.find("programGoToIn")->isVisible());
    REQUIRE_FALSE(s.find("programStepForward")->isVisible());
    REQUIRE_FALSE(s.find("programLoopButton")->isVisible());
    QObject* menu = s.shell.object("programTransportOverflowMenu");
    REQUIRE(menu != nullptr);
    const auto open = [&] {
        REQUIRE(QMetaObject::invokeMethod(overflow, "clicked"));
        REQUIRE(waitFor([&] { return menu->property("opened").toBool(); }));
    };
    // The rows are declared in the "···" button (its default property fills the menu): look them up there.
    const auto choose = [&](const char* row) {
        QObject* item = overflow->findChild<QObject*>(QLatin1String(row));
        INFO(row);
        REQUIRE(item != nullptr);
        CHECK(item->property("visible").toBool());
        REQUIRE(QMetaObject::invokeMethod(item, "clicked"));
        REQUIRE(waitFor([&] { return !shown(menu); }));
        spin(30); // the button's action runs once the menu has closed
    };
    // Marks for Go to In; the playhead elsewhere.
    REQUIRE(s.controller->seekToText(QStringLiteral("00:00:02:00")));
    s.controller->markInAtPlayhead();
    REQUIRE(s.controller->seekToText(QStringLiteral("00:00:04:00")));
    open();
    // Exactly the hidden buttons, with the button's text and shortcut; the shown and always-shown ones are not there.
    QObject* goToInRow = overflow->findChild<QObject*>(QStringLiteral("overflow_programGoToIn"));
    REQUIRE(goToInRow != nullptr);
    CHECK(goToInRow->property("visible").toBool());
    CHECK(goToInRow->property("text").toString() == QStringLiteral("Go to In"));
    CHECK(goToInRow->property("command").toString() == QStringLiteral("marker.goToIn"));
    QObject* markInRow = overflow->findChild<QObject*>(QStringLiteral("overflow_programMarkIn"));
    REQUIRE(markInRow != nullptr);
    CHECK(s.find("programMarkIn")->isVisible());
    CHECK_FALSE(markInRow->property("visible").toBool()); // Mark In shows in the row
    CHECK(overflow->findChild<QObject*>(QStringLiteral("overflow_programLift")) == nullptr);
    const QImage image = s.shell.grab();
    REQUIRE_FALSE(image.isNull());
    CHECK(image.save(QDir(outputDir()).filePath(QStringLiteral("monitor-transport-overflow-menu.png"))));
    // Go to In from the menu moves the playhead to the In.
    choose("overflow_programGoToIn");
    CHECK(waitFor([&] { return s.controller->playheadText() == QStringLiteral("00:00:02:00"); }));
    // Step forward moves one frame.
    const QString next = s.controller->timeEntryText(QStringLiteral("+1"));
    open();
    choose("overflow_programStepForward");
    CHECK(waitFor([&] { return s.controller->playheadText() == next; }));
    // Loop is checkable: the row shows the check and switches it.
    const bool loop = playback->property("loop").toBool();
    open();
    QObject* loopRow = overflow->findChild<QObject*>(QStringLiteral("overflow_programLoopButton"));
    REQUIRE(loopRow != nullptr);
    CHECK(loopRow->property("checkable").toBool());
    CHECK(loopRow->property("checked").toBool() == loop);
    choose("overflow_programLoopButton");
    CHECK(waitFor([&] { return playback->property("loop").toBool() != loop; }));
    open();
    CHECK(loopRow->property("checked").toBool() != loop);
    REQUIRE(QMetaObject::invokeMethod(menu, "close"));
    REQUIRE(waitFor([&] { return !shown(menu); }));
    playback->setProperty("loop", loop);

    // The Source Monitor: Redactions opens its list from the "···" button; Settings is a submenu there.
    QQuickItem* sourceOverflow = s.find("sourceTransportOverflow");
    REQUIRE(sourceOverflow->isVisible());
    REQUIRE_FALSE(s.find("sourceRedactionsButton")->isVisible());
    QObject* sourceMenu = s.shell.object("sourceTransportOverflowMenu");
    REQUIRE(sourceMenu != nullptr);
    CHECK(sourceOverflow->findChild<QObject*>(QStringLiteral("overflow_sourceMonitorSettingsMenu")) != nullptr);
    CHECK(sourceOverflow->findChild<QObject*>(QStringLiteral("overflow_sourceLoopItem")) != nullptr);
    REQUIRE(QMetaObject::invokeMethod(sourceOverflow, "clicked"));
    REQUIRE(waitFor([&] { return sourceMenu->property("opened").toBool(); }));
    QObject* redactions = sourceOverflow->findChild<QObject*>(QStringLiteral("overflow_sourceRedactionsButton"));
    REQUIRE(redactions != nullptr);
    CHECK(redactions->property("visible").toBool());
    CHECK(redactions->property("text").toString() == QStringLiteral("Redactions…"));
    if (redactions->property("enabled").toBool()) {
        REQUIRE(QMetaObject::invokeMethod(redactions, "clicked"));
        REQUIRE(waitFor([&] { return !shown(sourceMenu); }));
        QObject* popover = s.shell.object("redactionsPopover");
        REQUIRE(popover != nullptr);
        CHECK(waitFor([&] { return popover->property("opened").toBool(); }));
        REQUIRE(QMetaObject::invokeMethod(popover, "close"));
        REQUIRE(waitFor([&] { return !shown(popover); }));
    } else {
        REQUIRE(QMetaObject::invokeMethod(sourceMenu, "close"));
    }

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Monitor transports show every button at 2560x1440", "[app][monitor][transport][qml]") {
    resetSettings();
    clearMessages();
    MonitorShell s(2560, 1440);
    s.loadTitleSource();
    REQUIRE_FALSE(s.shell.grab().isNull()); // a frame lays the hidden window out
    INFO("program transport " << s.transportOf("programTransportOverflow")->width() << ", source transport "
                              << s.transportOf("sourceTransportOverflow")->width());
    CHECK(s.checkInside("programTransportOverflow") == 1); // only the "···" button is hidden
    CHECK(s.checkInside("sourceTransportOverflow") == 1);
    CHECK_FALSE(s.find("programTransportOverflow")->isVisible());
    CHECK_FALSE(s.find("sourceTransportOverflow")->isVisible());
    for (const char* name : {"programMarkIn", "programGoToIn", "programStepBack", "programPlayInToOut", "programLoopButton",
                             "programExportFrame", "multicamViewToggle", "sourceGoToIn", "sourcePlayInToOut",
                             "sourceStepBack", "sourceLoopButton", "sourceExportFrame", "sourceRedactButton",
                             "sourceRedactionsButton", "sourceMonitorSettingsButton"}) {
        INFO(name);
        CHECK(s.find(name)->isVisible());
    }
    // Redactions opens its list right above its button, where the button is now (not where it was at start-up).
    QQuickItem* redactions = s.find("sourceRedactionsButton");
    REQUIRE(redactions->isEnabled());
    REQUIRE(QMetaObject::invokeMethod(redactions, "clicked"));
    QObject* popover = s.shell.object("redactionsPopover");
    REQUIRE(popover != nullptr);
    REQUIRE(waitFor([&] { return popover->property("opened").toBool(); }));
    const QPointF at = redactions->mapToItem(s.transportOf("sourceTransportOverflow"), QPointF(0, 0));
    INFO("Redactions button at " << at.x() << ", " << at.y() << "; popover at " << popover->property("x").toDouble()
                                 << ", " << popover->property("y").toDouble());
    CHECK(popover->property("x").toDouble() == Approx(at.x()).margin(1));
    CHECK(popover->property("y").toDouble() + popover->property("height").toDouble() <= at.y() + 0.5);
    REQUIRE(QMetaObject::invokeMethod(popover, "close"));
    REQUIRE(waitFor([&] { return !shown(popover); }));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
