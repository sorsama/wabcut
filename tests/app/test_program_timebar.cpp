// 0.1.3 (WP-M): the Program Monitor's time bar (SequenceTimeBar + ProgramTimeBar.qml): it maps the sequence onto
// its width, seeks and scrubs on frames, shows the In/Out marks, zooms at the pointer and pages a zoomed window
// along with the playhead; in the shell, a press on it seeks and Ctrl+wheel zooms. And the gutters' resize cost
// (WorkbenchSplitter): deferred by default (a guide follows the pointer, one layout on release), live on request
// (at most one layout per frame); a track header's height drag coalesces to frames too. Offscreen.
// 0.4: the Program Monitor's In to Out pill (ProjectController.markDurationText; screenshot program-inout-duration.png),
// clear of the playback resolution pill, which gives way on a narrow monitor (wrench > Playback Resolution; screenshots
// program-inout-duration-project.png, program-playback-resolution-menu.png).

#include "ShellFixture.h"

#include "ProjectController.h"
#include "SequenceTimeBar.h"
#include "TimelineFixture.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QWheelEvent>

#include <cmath>
#include <functional>
#include <memory>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using Catch::Approx;

namespace {

QQuickItem* findItem(QQuickItem* root, const std::function<bool(QQuickItem*)>& match) {
    if (!root)
        return nullptr;
    if (match(root))
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findItem(child, match))
            return found;
    }
    return nullptr;
}

QQuickItem* named(QQuickItem* root, const QString& name) {
    return findItem(root, [&](QQuickItem* i) { return i->objectName() == name; });
}

void mouse(QQuickWindow* window, QEvent::Type type, QPointF p, Qt::MouseButtons buttons) {
    QMouseEvent event(type, p, p, type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, {});
    QCoreApplication::sendEvent(window, &event);
}

void wheel(QQuickWindow* window, QPointF p, int delta, Qt::KeyboardModifiers mods) {
    QWheelEvent event(p, p, QPoint(), QPoint(0, delta), Qt::NoButton, mods, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(window, &event);
}

/// A bar over the demo project's "Sequence 01" (1:20:00 at 23.976 fps, 5 markers), 800 x 16.
struct Bar {
    ProjectController controller;
    SequenceTimeBar bar;
    Bar() {
        bar.setController(&controller);
        bar.setSize(QSizeF(800, 16));
    }
    double seconds() const { return controller.playhead().toSeconds(); }
};

} // namespace

TEST_CASE("Time bar: the sequence maps onto the width and a seek lands on a frame", "[app][timebar]") {
    Bar b;
    REQUIRE(b.bar.active());
    const double span = b.bar.span().toSeconds();
    CHECK(span >= 80.0);
    CHECK_FALSE(b.bar.zoomed());

    b.bar.seekToX(400);
    CHECK(b.seconds() == Approx(span / 2).margin(1.0 / 23.976));
    // On a frame of the sequence (23.976 fps: 1001/24000 s).
    const double frames = b.seconds() * 24000.0 / 1001.0;
    CHECK(frames == Approx(std::round(frames)).margin(1e-6));
    CHECK(b.bar.playheadX() == Approx(400).margin(800.0 / (span * 23.976) + 0.5));

    // Clamped to the bar.
    b.bar.seekToX(-50);
    CHECK(b.seconds() == 0.0);
    b.bar.seekToX(5000);
    CHECK(b.seconds() == Approx(span).margin(0.05));
    CHECK(b.bar.timecodeAt(0) == QStringLiteral("00:00:00:00"));
}

TEST_CASE("Time bar: In and Out marks show where they are", "[app][timebar]") {
    Bar b;
    b.controller.clearMarks();
    CHECK_FALSE(b.bar.hasIn());
    CHECK_FALSE(b.bar.hasOut());
    CHECK(std::isnan(b.bar.inX()));

    b.bar.seekToX(200);
    b.controller.markInAtPlayhead();
    b.bar.seekToX(600);
    b.controller.markOutAtPlayhead();
    CHECK(b.bar.hasIn());
    CHECK(b.bar.hasOut());
    CHECK(b.bar.inX() == Approx(200).margin(1));
    // Out is exclusive: one frame after the frame it marks.
    CHECK(b.bar.outX() > b.bar.playheadX());
    CHECK(b.bar.outX() == Approx(600).margin(2));
}

TEST_CASE("Time bar: zoom keeps the time under the pointer and the window pages with the playhead", "[app][timebar]") {
    Bar b;
    const double span = b.bar.span().toSeconds();
    b.bar.zoomAt(4, 200);
    CHECK(b.bar.zoomed());
    CHECK(b.bar.viewEnd() - b.bar.viewStart() == Approx(0.25));
    // 200 px was a quarter of the way along; it still is the same time.
    CHECK(b.bar.viewStart() + 200.0 / 800.0 * 0.25 == Approx(0.25));

    // A window never goes past either end, and never shorter than half a second.
    b.bar.setView(-0.3, 0.1);
    CHECK(b.bar.viewStart() == 0.0);
    CHECK(b.bar.viewEnd() == Approx(0.4));
    b.bar.zoomAt(1e6, 0);
    CHECK((b.bar.viewEnd() - b.bar.viewStart()) * span == Approx(0.5).margin(1e-6));

    // The playhead leaving the window (playback, a key) pages the window to it.
    b.bar.setView(0, 0.25);
    b.controller.setPlayhead(Ticks::fromSeconds(60));
    CHECK(b.bar.viewStart() <= 60.0 / span);
    CHECK(b.bar.viewEnd() >= 60.0 / span);
    CHECK(b.bar.playheadX() >= 0);

    b.bar.resetZoom();
    CHECK_FALSE(b.bar.zoomed());
}

TEST_CASE("Program Monitor time bar in the shell: a press seeks and Ctrl+wheel zooms", "[app][timebar][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    INFO(join(shell.errors));
    shell.resize(kShotWidth, kShotHeight);
    REQUIRE_FALSE(shell.grab().isNull());
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(controller);

    QQuickItem* root = shell.window->contentItem();
    QQuickItem* bar = named(root, QStringLiteral("programTimeBar"));
    REQUIRE(bar);
    REQUIRE(bar->isVisible());
    auto* timeBar = qobject_cast<SequenceTimeBar*>(named(bar, QStringLiteral("sequenceTimeBar")));
    REQUIRE(timeBar);
    QQuickItem* area = named(bar, QStringLiteral("programTimeBarArea"));
    REQUIRE(area);
    CHECK(timeBar->width() > 100);

    // A press a quarter of the way along seeks there.
    const QPointF quarter = timeBar->mapToScene(QPointF(timeBar->width() / 4, timeBar->height() / 2));
    mouse(shell.window.get(), QEvent::MouseButtonPress, quarter, Qt::LeftButton);
    mouse(shell.window.get(), QEvent::MouseButtonRelease, quarter, Qt::NoButton);
    const double span = timeBar->span().toSeconds();
    CHECK(controller->playhead().toSeconds() == Approx(span / 4).margin(span / timeBar->width() + 0.05));
    QQuickItem* playhead = named(bar, QStringLiteral("programTimePlayhead"));
    REQUIRE(playhead);
    CHECK(playhead->isVisible());
    CHECK(playhead->mapToScene(QPointF()).x() == Approx(quarter.x()).margin(2));

    // Ctrl+wheel zooms in at the pointer; the zoom bar shows.
    QQuickItem* zoomBar = named(bar, QStringLiteral("programZoomBar"));
    REQUIRE(zoomBar);
    CHECK_FALSE(zoomBar->isVisible());
    wheel(shell.window.get(), quarter, 120, Qt::ControlModifier);
    CHECK(timeBar->zoomed());
    CHECK(zoomBar->isVisible());
    // The shell stays warning-free.
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Gutters: deferred by default, one layout on release; live on request, per frame", "[app][gutters][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    REQUIRE(shell.preferences);
    CHECK(shell.preferences->property("liveResize").toBool() == false);

    QQmlComponent component(&shell.engine);
    component.setData(R"(
        import QtQuick
        import Wabcut
        Item {
            property int calls: 0
            property real total: 0
            property alias splitter: s
            WorkbenchSplitter {
                id: s
                objectName: "testSplitter"
                anchors.fill: parent
                onDragged: delta => { parent.calls++; parent.total += delta }
            }
        })", QUrl(QStringLiteral("qrc:/wpm-gutters/Host.qml")));
    std::unique_ptr<QObject> object(component.create());
    INFO(component.errorString().toStdString());
    REQUIRE(object);
    auto* host = qobject_cast<QQuickItem*>(object.get());
    REQUIRE(host);
    host->setParentItem(shell.window->contentItem());
    host->setPosition(QPointF(400, 100));
    host->setSize(QSizeF(10, 300));
    host->setZ(1000);
    auto* splitter = qobject_cast<QQuickItem*>(host->property("splitter").value<QObject*>());
    REQUIRE(splitter);
    QQuickItem* guide = named(splitter, QStringLiteral("splitterGuide"));
    REQUIRE(guide);

    const QPointF start = host->mapToScene(QPointF(5, 150));
    const auto drag = [&](double distance, int steps) {
        mouse(shell.window.get(), QEvent::MouseButtonPress, start, Qt::LeftButton);
        for (int i = 1; i <= steps; ++i)
            mouse(shell.window.get(), QEvent::MouseMove, start + QPointF(distance * i / steps, 0), Qt::LeftButton);
    };

    // Deferred: nothing reaches the layout while the pointer moves; the guide shows where the gutter goes.
    drag(60, 30);
    spin(50);
    CHECK(host->property("calls").toInt() == 0);
    CHECK(guide->isVisible());
    CHECK(guide->mapToScene(QPointF(guide->width() / 2, 0)).x() == Approx(start.x() + 60).margin(1.5));
    mouse(shell.window.get(), QEvent::MouseButtonRelease, start + QPointF(60, 0), Qt::NoButton);
    CHECK(host->property("calls").toInt() == 1);
    CHECK(host->property("total").toDouble() == Approx(60));
    CHECK_FALSE(guide->isVisible());

    // The limits hold the guide (and the distance) where the layout would stop.
    splitter->setProperty("maxDelta", 25.0);
    host->setProperty("calls", 0);
    host->setProperty("total", 0.0);
    drag(60, 6);
    mouse(shell.window.get(), QEvent::MouseButtonRelease, start + QPointF(60, 0), Qt::NoButton);
    CHECK(host->property("total").toDouble() == Approx(25));
    splitter->setProperty("maxDelta", qInf());

    // Live: the layout follows during the drag, at most once a frame, and the distances add up to the drag.
    shell.preferences->setProperty("liveResize", true);
    CHECK(splitter->property("live").toBool());
    host->setProperty("calls", 0);
    host->setProperty("total", 0.0);
    drag(80, 40);
    REQUIRE(waitFor([&] { return host->property("calls").toInt() > 0; }, 3000));
    CHECK(host->property("calls").toInt() < 40);
    CHECK_FALSE(guide->isVisible());
    mouse(shell.window.get(), QEvent::MouseButtonRelease, start + QPointF(80, 0), Qt::NoButton);
    CHECK(host->property("total").toDouble() == Approx(80));
    shell.preferences->setProperty("liveResize", false);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Program Monitor shows the sequence In to Out duration", "[app][monitor][marks]") {
    // 0.4: ProjectController.markDurationText, the Out frame included (Premiere's In/Out duration). 25 fps.
    const std::string picture = "V1 tgt |A" + std::string(99, 'a') + "|";
    Timeline tl(picture);
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    int notified = 0;
    QObject::connect(&controller, &ProjectController::marksChanged, [&] { ++notified; });
    CHECK(controller.markDurationText().isEmpty()); // no marks
    controller.setPlayhead(f(25));
    controller.markInAtPlayhead();
    CHECK(notified == 1);
    CHECK(controller.markDurationText().isEmpty()); // one mark
    controller.setPlayhead(f(87)); // 3 s 12 f
    controller.markOutAtPlayhead();
    CHECK(notified == 2);
    CHECK(controller.markDurationText() == QStringLiteral("00:00:02:13"));
    CHECK(controller.markOutText() == QStringLiteral("00:00:03:12"));
    // Undo follows; clearing both empties it.
    REQUIRE(controller.undo());
    CHECK(controller.markDurationText().isEmpty());
    REQUIRE(controller.redo());
    CHECK(controller.markDurationText() == QStringLiteral("00:00:02:13"));
    const int before = notified;
    controller.clearMarks();
    CHECK(notified == before + 1);
    CHECK(controller.markDurationText().isEmpty());
}

TEST_CASE("Program Monitor In to Out pill shows and stays at narrow widths", "[app][monitor][marks][qml][screenshot]") {
    // 0.4: the raised pill after the playhead timecode (ProjectController.markDurationText), with both marks set only.
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    INFO(join(shell.errors));
    REQUIRE(shell.errors.isEmpty());
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(controller);
    QQuickItem* root = shell.window->contentItem();
    QQuickItem* pill = named(root, QStringLiteral("programInOutDuration"));
    QQuickItem* text = named(root, QStringLiteral("programInOutDurationText"));
    QQuickItem* timecode = named(root, QStringLiteral("programTimecode"));
    QQuickItem* wrench = named(root, QStringLiteral("programMonitorSettingsButton"));
    REQUIRE(pill);
    REQUIRE(text);
    REQUIRE(timecode);
    REQUIRE(wrench);

    controller->clearMarks();
    CHECK_FALSE(pill->isVisible());
    REQUIRE(controller->seekToText(QStringLiteral("00:00:01:00")));
    controller->markInAtPlayhead();
    CHECK_FALSE(pill->isVisible()); // one mark: nothing to measure
    REQUIRE(controller->seekToText(QStringLiteral("00:00:03:12")));
    controller->markOutAtPlayhead();
    REQUIRE(pill->isVisible());
    CHECK(text->property("text").toString() == QStringLiteral("00:00:02:13")); // the Out frame counts
    const QImage image = shell.grab(); // a frame lays the hidden window out
    REQUIRE_FALSE(image.isNull());
    CHECK(image.save(QDir(outputDir()).filePath(QStringLiteral("program-inout-duration.png"))));
    // Right after the timecode, clear of the right-hand readouts (they give way first).
    const auto right = [](const QQuickItem* item) { return item->mapToScene(QPointF(item->width(), 0)).x(); };
    const auto left = [](const QQuickItem* item) { return item->mapToScene(QPointF(0, 0)).x(); };
    INFO("timecode " << left(timecode) << "-" << right(timecode) << ", pill " << left(pill) << "-" << right(pill)
                     << ", wrench " << left(wrench));
    CHECK(left(pill) >= right(timecode));
    CHECK(left(pill) - right(timecode) <= 12);
    CHECK(right(pill) <= left(wrench));

    // That is a narrow monitor already: the Editing workspace at 1600x960 gives the Program Monitor under 360 px.
    QQuickItem* panel = shell.item("monitorPanel");
    REQUIRE(panel);
    INFO("panel " << panel->width());
    CHECK(panel->width() <= 360);
    // Undo and the clears follow.
    REQUIRE(controller->undo());
    CHECK_FALSE(pill->isVisible());
    REQUIRE(controller->redo());
    CHECK(pill->isVisible());
    controller->clearMarks();
    CHECK_FALSE(pill->isVisible());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Program Monitor In to Out pill stays clear of the playback resolution in a project",
          "[app][monitor][marks][qml]") {
    // Outside the demo the playback resolution pill shows too; on a narrow monitor it gives way to the In to Out pill
    // (wrench > Playback Resolution has the same choices).
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    INFO(join(shell.errors));
    REQUIRE(shell.errors.isEmpty());
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    QObject* file = shell.engine.singletonInstance<QObject*>("Wabcut", "ProjectFile");
    QObject* readiness = shell.engine.singletonInstance<QObject*>("Wabcut", "MediaReadiness");
    REQUIRE(controller);
    REQUIRE(file);
    REQUIRE(readiness);
    REQUIRE(QMetaObject::invokeMethod(file, "newProject"));
    REQUIRE_FALSE(file->property("demo").toBool());
    const std::string picture = "V1 tgt |A" + std::string(99, 'a') + "|";
    Timeline tl(picture);
    controller->resetProject(tl.state, tl.seq);
    controller->setPlayhead(f(25));
    controller->markInAtPlayhead();
    controller->setPlayhead(f(87));
    controller->markOutAtPlayhead();

    QQuickItem* root = shell.window->contentItem();
    QQuickItem* pill = named(root, QStringLiteral("programInOutDuration"));
    QQuickItem* wrench = named(root, QStringLiteral("programMonitorSettingsButton"));
    QQuickItem* quality = named(root, QStringLiteral("previewQualityButton"));
    REQUIRE(pill);
    REQUIRE(wrench);
    REQUIRE(quality);
    const auto right = [](const QQuickItem* item) { return item->mapToScene(QPointF(item->width(), 0)).x(); };
    const auto left = [](const QQuickItem* item) { return item->mapToScene(QPointF(0, 0)).x(); };
    const auto clear = [&] {
        REQUIRE_FALSE(shell.grab().isNull()); // a frame lays the hidden window out
        REQUIRE(pill->isVisible());
        INFO("pill " << left(pill) << "-" << right(pill) << ", wrench " << left(wrench) << ", quality "
                     << (quality->isVisible() ? left(quality) : -1.0));
        CHECK(right(pill) <= left(wrench));
        if (quality->isVisible())
            CHECK(right(pill) <= left(quality));
    };
    // The Editing workspace at 1600x960: the Program Monitor is narrow.
    clear();
    // Wide: everything shows.
    shell.resize(2560, 1440);
    clear();
    CHECK(quality->isVisible());
    shell.resize(kShotWidth, kShotHeight);
    clear();
    QQuickItem* panel = shell.item("monitorPanel");
    REQUIRE(panel);
    const double dpr = shell.window->effectiveDevicePixelRatio();
    const QRectF r = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
    CHECK(shell.grab()
              .copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect())
              .save(QDir(outputDir()).filePath(QStringLiteral("program-inout-duration-project.png"))));

    // The wrench menu's Playback Resolution sets the same preference.
    QObject* settings = shell.object("programMonitorSettingsMenu");
    QObject* menu = shell.object("playbackResolutionMenu");
    REQUIRE(settings != nullptr);
    REQUIRE(menu != nullptr);
    CHECK(menu->property("title").toString() == QStringLiteral("Playback Resolution"));
    REQUIRE(QMetaObject::invokeMethod(wrench, "clicked"));
    REQUIRE(waitFor([&] { return settings->property("opened").toBool(); }));
    REQUIRE(QMetaObject::invokeMethod(menu, "open"));
    REQUIRE(waitFor([&] { return menu->property("opened").toBool(); }));
    const QImage open = shell.grab();
    REQUIRE_FALSE(open.isNull());
    CHECK(open.save(QDir(outputDir()).filePath(QStringLiteral("program-playback-resolution-menu.png"))));
    REQUIRE(QMetaObject::invokeMethod(menu, "close"));
    REQUIRE(QMetaObject::invokeMethod(settings, "close"));
    REQUIRE(waitFor([&] { return !menu->property("visible").toBool() && !settings->property("visible").toBool(); }));
    const QString before = readiness->property("previewQuality").toString();
    for (const char* value : {"half", "quarter", "full", "auto"}) {
        INFO(value);
        QObject* item = menu->findChild<QObject*>(QStringLiteral("playbackResolution_%1").arg(QLatin1String(value)));
        REQUIRE(item != nullptr);
        CHECK(item->property("checkable").toBool());
        REQUIRE(QMetaObject::invokeMethod(item, "clicked"));
        CHECK(readiness->property("previewQuality").toString() == QLatin1String(value));
        CHECK(item->property("checked").toBool());
    }
    readiness->setProperty("previewQuality", before);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
