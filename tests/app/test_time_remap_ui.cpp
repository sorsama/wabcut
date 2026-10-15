#include "ShellFixture.h"
#include "TimeRemapBridge.h"
#include "TimelineHarness.h"

#include <wabcut/model/TimeRemap.h>

#include <QDir>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {
Clip current(const Rig& r) {
    return r.controller.currentSequence()->findClip(r.tl.clip("V1@0"))->clip();
}
QPointF band(const Rig& r, double frame) {
    const auto& row = r.view->rows().front();
    const QRectF area = TimelineView::volumeArea(QRectF(0, row.y, 800, row.height));
    const Clip c = current(r);
    return {r.x(frame), area.top() + TimeRemapBridge::speedFraction(TimeRemapBridge::speedAt(
                                         c, c.curveTimeAt(f(static_cast<int>(frame))))) *
                                         area.height()};
}
QQuickItem* named(QQuickItem* item, const QString& name) {
    if (item->objectName() == name)
        return item;
    for (auto* child : item->childItems())
        if (auto* found = named(child, name))
            return found;
    return nullptr;
}
} // namespace

TEST_CASE("Time remap: command-backed speed, ramp halves, hold and reverse each undo atomically",
          "[app][remap]") {
    Rig r("V1 tgt |A10aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|");
    r.controller.selectClipById(r.tl.clip("V1@0"), ProjectController::Replace);
    TimeRemapBridge bridge;
    bridge.setController(&r.controller);
    REQUIRE(bridge.available());
    CHECK(TimeRemapBridge::speedLabel(current(r)) == QStringLiteral("100%"));
    Clip held = current(r);
    ClipContent heldContent = held.content();
    Curve holdCurve;
    holdCurve.keys = {{held.sourceIn, 0.0}, {held.sourceOut(), 0.0}};
    heldContent.timeRemap = std::move(holdCurve);
    held.setContent(std::move(heldContent));
    CHECK(TimeRemapBridge::speedLabel(held) == QStringLiteral("0%"));
    REQUIRE(bridge.setEnabled(true));
    REQUIRE(bridge.addKey(0));
    REQUIRE(bridge.addKey(0.5));
    REQUIRE(bridge.addKey(1));
    const auto keys = bridge.keys();
    REQUIRE(keys.size() >= 3);
    const int middle = keys[1].toMap().value("index").toInt();
    const auto before = current(r).timeRemap()->keys;
    const auto undo = r.controller.session().undoCount();
    REQUIRE(bridge.setSpeed(middle, 200));
    CHECK(TimeRemapBridge::speedLabel(current(r)) == QStringLiteral("Variable speed"));
    CHECK(TimeRemapBridge::speedAt(current(r),
                                   current(r).timeRemap()->keys[static_cast<std::size_t>(middle)].time) ==
          Catch::Approx(200).margin(0.01));
    CHECK(r.controller.session().undoCount() == undo + 1);
    r.controller.session().undo();
    CHECK(current(r).timeRemap()->keys == before);

    REQUIRE(bridge.splitKey(middle, 0.35, true));
    CHECK(current(r).timeRemap()->keys.size() == before.size() + 1);
    CHECK(current(r).timeRemap()->keys[static_cast<std::size_t>(middle)].interpolation ==
          Interpolation::Bezier);
    CHECK(r.controller.session().undoCount() == undo + 1);
    r.controller.session().undo();
    CHECK(current(r).timeRemap()->keys == before);

    for (const bool reverse : {false, true}) {
        REQUIRE((reverse ? bridge.reverse(0.5, 0.12) : bridge.hold(0.5, 0.12)));
        bool found = false;
        for (const auto& segment : remapSegments(*current(r).timeRemap())) {
            if (segment.from >= current(r).sourceIn && segment.to <= current(r).sourceOut())
                found = found || segment.motion == (reverse ? RemapMotion::Reverse : RemapMotion::Hold);
        }
        CHECK(found);
        CHECK(r.controller.session().undoCount() == undo + 1);
        r.controller.session().undo();
        CHECK(current(r).timeRemap()->keys == before);
    }
    bridge.beginGesture();
    REQUIRE(bridge.moveKey(middle, 0.55));
    REQUIRE(bridge.moveKey(middle, 0.6));
    bridge.cancelGesture();
    CHECK(current(r).timeRemap()->keys == before);
    CHECK(r.controller.session().undoCount() == undo);
}

TEST_CASE("Time remap: real timeline mouse gestures add keys, scale speed, hold, split and cancel",
          "[app][remap][gpu]") {
    Rig r("V1 tgt |A10aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|");
    r.view->setShowSpeed(true);
    r.harness.frame();
    const QPointF p = band(r, 18);
    r.press(p);
    r.release(p);
    REQUIRE(current(r).isRemapped());
    const auto original = current(r).timeRemap()->keys;
    REQUIRE(original[1].time == current(r).sourceIn + f(18));
    const auto undo = r.controller.session().undoCount();
    r.drag(p, p + QPointF(0, -28), Qt::ControlModifier);
    REQUIRE(r.controller.session().undoCount() == undo + 1);
    CHECK(current(r).timeRemap()->keys != original);
    r.controller.session().undo();
    r.harness.frame();

    r.drag(p, p + QPointF(40, 0), Qt::AltModifier);
    REQUIRE(r.controller.session().undoCount() == undo + 1);
    CHECK(current(r).timeRemap()->keys.size() > original.size());
    r.controller.session().undo();
    r.harness.frame();
    r.drag(p - QPointF(3, 0), p - QPointF(45, 0), {}, false);
    INFO(r.view->speedReadout().toStdString());
    REQUIRE(r.view->speedReadout().startsWith(QStringLiteral("Ramp transition")));
    r.release(p - QPointF(45, 0));
    REQUIRE(r.controller.session().undoCount() == undo + 1);
    CHECK(current(r).timeRemap()->keys.size() == original.size() + 1);
    r.controller.session().undo();
    r.harness.frame();
    r.drag(p, p + QPointF(0, -25), Qt::ControlModifier, false);
    r.key(Qt::Key_Escape);
    r.release(p + QPointF(0, -25), Qt::ControlModifier);
    CHECK(current(r).timeRemap()->keys == original);
    CHECK(r.controller.session().undoCount() == undo);
}

TEST_CASE("Time remap: presets and source and speed graph screenshots", "[app][remap][screenshots][qml]") {
    // A hidden grab tears down Shape resources: use a fresh window for each view.
    for (const bool speedView : {false, true}) {
        resetSettings();
        clearMessages();
        Shell shell;
        REQUIRE(shell.window);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        auto* bridge = shell.engine.singletonInstance<TimeRemapBridge*>("Wabcut", "TimeRemap");
        auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        REQUIRE(bridge);
        REQUIRE(controller);
        REQUIRE(bridge->available());
        const auto before = controller->session().undoCount();
        for (const QString preset : {QStringLiteral("in"), QStringLiteral("out"), QStringLiteral("inOut")}) {
            REQUIRE(bridge->preset(preset));
            CHECK(controller->session().undoCount() == before + 1);
            controller->session().undo();
            CHECK_FALSE(bridge->enabled());
        }
        REQUIRE(bridge->preset(QStringLiteral("inOut")));
        const auto selected = controller->propertyTarget(StreamKind::Video);
        REQUIRE(selected);
        CHECK(TimeRemapBridge::speedLabel(controller->currentSequence()->findClip(*selected)->clip()) ==
              QStringLiteral("Variable speed"));
        auto* panel = named(shell.window->contentItem(), QStringLiteral("timeRemapControls"));
        REQUIRE(panel);
        panel->setProperty("expanded", true);
        panel->setProperty("speedView", speedView);
        shell.item("timelinePanel")->setProperty("showSpeed", true);
        spin(50);
        const auto screenshot =
            speedView ? QStringLiteral("time-remap-speed.png") : QStringLiteral("time-remap-source.png");
        REQUIRE(shell.grab().save(QDir(outputDir()).filePath(screenshot)));
        const QStringList messages = takeMessages();
        INFO(join(messages));
        CHECK(messages.isEmpty());
    }
}

TEST_CASE("Time remap: Escape cancels a graph drag even while the pointer remains down",
          "[app][remap][qml]") {
    resetSettings();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* bridge = shell.engine.singletonInstance<TimeRemapBridge*>("Wabcut", "TimeRemap");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(bridge->setEnabled(true));
    REQUIRE(bridge->addKey(0.5));
    auto* panel = named(shell.window->contentItem(), QStringLiteral("timeRemapControls"));
    REQUIRE(panel);
    panel->setProperty("expanded", true);
    spin(50);
    REQUIRE_FALSE(shell.grab().isNull()); // polish the hidden window's expanded layouts before pointer input
    auto* graph = named(panel, QStringLiteral("timeRemapGraph"));
    REQUIRE(graph);
    REQUIRE(graph->width() > 0);
    const auto original = bridge->keys();
    const auto undo = controller->session().undoCount();
    const QPointF start = graph->mapToScene({graph->width() * 0.5, graph->height() * 0.5});
    const auto mouse = [&](QEvent::Type type, QPointF point, Qt::MouseButton button,
                           Qt::MouseButtons buttons) {
        QMouseEvent event(type, point, point, button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(shell.window.get(), &event);
        spin(10);
    };
    mouse(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    REQUIRE(panel->property("selectedKey").toInt() >= 0);
    mouse(QEvent::MouseMove, start + QPointF(25, 0), Qt::NoButton, Qt::LeftButton);
    INFO(bridge->message().toStdString());
    REQUIRE(bridge->keys() != original);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(shell.window.get(), &escape);
    mouse(QEvent::MouseMove, start + QPointF(40, 0), Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, start + QPointF(40, 0), Qt::LeftButton, Qt::NoButton);
    CHECK(bridge->keys() == original);
    CHECK(controller->session().undoCount() == undo);
}

TEST_CASE("Time remap review P2-40: a plain drag of the speed line changes the speed; nothing else commits nothing",
          "[app][remap][review][gpu]") {
    Rig r("V1 tgt |A10aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|");
    r.view->setShowSpeed(true);
    r.harness.frame();
    const QPointF p = band(r, 18);
    {
        const auto undo = r.controller.session().undoCount();
        r.drag(p, p + QPointF(0, -28));
        REQUIRE(r.controller.session().undoCount() == undo + 1);
        const Clip c = current(r);
        REQUIRE(c.isRemapped());
        CHECK(TimeRemapBridge::speedAt(c, c.curveTimeAt(f(18))) > 150.0);
        r.controller.session().undo();
        r.harness.frame();
        r.press(p);
        r.release(p); // adds a key
        const auto withKey = r.controller.session().undoCount();
        r.press(p);
        r.release(p); // a click on that key: nothing to do
        CHECK(r.controller.session().undoCount() == withKey);
    }
}

TEST_CASE("Time remap review P2-43: Esc, an undo and a second button end a speed drag safely",
          "[app][remap][review][gpu]") {
    Rig r("V1 tgt |A10aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|");
    r.view->setShowSpeed(true);
    r.harness.frame();
    const QPointF p = band(r, 18);

    SECTION("Esc belongs to a speed drag before any shortcut") {
        r.press(p);
        r.move(p + QPointF(0, -10));
        QKeyEvent override(QEvent::ShortcutOverride, Qt::Key_Escape, Qt::NoModifier);
        override.ignore();
        QCoreApplication::sendEvent(r.view, &override);
        CHECK(override.isAccepted());
        r.key(Qt::Key_Escape);
        r.release(p + QPointF(0, -10));
        CHECK(r.view->speedReadout().isEmpty());
    }
    SECTION("an undo under a speed drag cancels it") {
        r.press(p);
        r.release(p); // a key: remapping is on
        const auto undo = r.controller.session().undoCount();
        r.drag(p, p + QPointF(0, -30), Qt::ControlModifier, false);
        r.controller.session().undo(); // under the drag: the key index belongs to the old curve
        r.harness.frame();
        r.release(p + QPointF(0, -30), Qt::ControlModifier);
        CHECK(r.controller.session().undoCount() == undo - 1);
        CHECK_FALSE(current(r).isRemapped());
        CHECK(r.view->speedReadout().isEmpty());
    }
    SECTION("a second button during a speed drag does not strand it") {
        r.drag(p, p + QPointF(0, -20), Qt::ControlModifier, false);
        CHECK_FALSE(r.view->speedReadout().isEmpty());
        const QPointF at = p + QPointF(0, -20);
        r.send(QEvent::MouseButtonPress, at, Qt::RightButton, Qt::LeftButton | Qt::RightButton, Qt::ControlModifier);
        r.send(QEvent::MouseButtonRelease, at, Qt::RightButton, Qt::LeftButton, Qt::ControlModifier);
        r.harness.frame();
        r.release(at, Qt::ControlModifier);
        CHECK(r.view->speedReadout().isEmpty());
        CHECK(current(r).isRemapped()); // the left-button drag completed
    }
}
