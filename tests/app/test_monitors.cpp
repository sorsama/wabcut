// P.2 (E6): Premiere's Source and Program Monitor settings and keys (docs/monitors.md): Loop and Play In to Out in both
// monitors, the Monitors bridge's routing of the monitor keys. Offscreen; no window is shown.

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

#include <functional>
#include <memory>

#include <QGuiApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
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
