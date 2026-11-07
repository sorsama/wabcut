// Stream A, batch A10 (0.7), app: the Graph Editor (card 96) - KeyframeGraph, the engine of the panel (curves, legend, the key
// selection shared with Effect Controls, dragging keys and handles, numeric entry, easing presets, adding keys), and the panel
// in the shell (both themes, the speed graph, real pointer gestures). Offscreen; no window is shown.

#include "EffectControls.h"
#include "KeyframeGraph.h"
#include "ProjectController.h"
#include "ShellFixture.h"
#include "TimelineFixture.h"

#include <wabcut/model/MotionBlur.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QImage>
#include <QMouseEvent>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QVariantMap>

#include <cmath>
#include <functional>
#include <tuple>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using Catch::Approx;

namespace {

Ticks operator*(Ticks t, std::int64_t n) {
    return Ticks{t.value * n};
}

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

QQuickItem* named(QQuickItem* root, const char* name) {
    return findItem(root, [&](QQuickItem* i) { return i->objectName() == QLatin1String(name) && i->isVisible(); });
}

void mouse(QQuickWindow* window, QEvent::Type type, QPointF p, Qt::MouseButtons buttons, Qt::KeyboardModifiers mods = {}) {
    QMouseEvent event(type, p, p, type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, mods);
    QCoreApplication::sendEvent(window, &event);
}

QVariant evaluate(const Shell& shell, const QString& code) {
    QQmlExpression expression(qmlContext(shell.window.get()), shell.window.get(), code);
    const QVariant result = expression.evaluate();
    INFO(code.toStdString() << ": " << expression.error().toString().toStdString());
    REQUIRE_FALSE(expression.hasError());
    return result;
}

/// The city clip of a fresh controller with Opacity keyed 100 / 40 / 90 % and Scale keyed 100 / 140 / 110 %, each at three places.
struct Animated {
    ProjectController controller;
    EffectControlsModel& fx = *controller.effectControls();
    KeyframeGraph& graph = *fx.graph();
    ClipId clip = controller.primaryClip();
    Ticks frame = controller.frameTicks();
    const QString opacity = QStringLiteral("p:opacity");
    const QString scale = QStringLiteral("p:scale");

    explicit Animated(bool active = true) {
        const Clip c = controller.currentSequence()->findClip(clip)->clip();
        controller.setPlayhead(c.start() + frame * 24);
        fx.setAnimated(opacity, true);
        fx.setAnimated(scale, true);
        for (const auto& [frames, o, sc] : {std::tuple{96, 40.0, 140.0}, std::tuple{168, 90.0, 110.0}}) {
            controller.setPlayhead(c.start() + frame * frames);
            fx.setValue(opacity, o);
            fx.endEdit();
            fx.setValue(scale, sc);
            fx.endEdit();
        }
        graph.setActive(active);
    }
    Curve curveOf(ParamId id) const { return *controller.currentSequence()->findClip(clip)->clip().properties()[id].curve(); }
    std::size_t undoCount() { return controller.session().undoCount(); }
    QVariantMap curve(const QString& row) const {
        for (const QVariant& c : graph.curves()) {
            if (c.toMap().value("row").toString() == row)
                return c.toMap();
        }
        return {};
    }
    QVariantMap legend(const QString& row) const {
        for (const QVariant& c : graph.legend()) {
            if (c.toMap().value("row").toString() == row)
                return c.toMap();
        }
        return {};
    }
    QVariantMap key(const QString& row, int index) const { return curve(row).value("keys").toList().value(index).toMap(); }
    /// Presses (and releases) the key without moving it: selects it.
    void select(const QString& row, int index, int modifiers = 0) {
        const QVariantMap k = key(row, index);
        REQUIRE_FALSE(k.isEmpty());
        REQUIRE(graph.press("value", k.value("x").toDouble(), k.value("y").toDouble(), 0.01, 0.03, modifiers) == QStringLiteral("key"));
        graph.release();
    }
};

} // namespace

TEST_CASE("Graph Editor: every animated parameter is a curve with a legend row, and the key selection is Effect Controls' own",
          "[app][graph][a10]") {
    Animated a(false);
    // Nothing is computed while the panel is closed.
    CHECK(a.graph.legend().isEmpty());
    CHECK(a.graph.curves().isEmpty());
    a.graph.setActive(true);
    REQUIRE(a.graph.legend().size() == 2);
    REQUIRE(a.graph.curves().size() == 2);
    CHECK(a.graph.info().value("hasClip").toBool());
    // In the order of Effect Controls' rows: Transform (Scale), then Opacity.
    CHECK(a.legend(a.scale).value("title").toString() == QStringLiteral("Scale"));
    CHECK(a.legend(a.opacity).value("title").toString() == QStringLiteral("Opacity"));
    CHECK(a.graph.legend().at(0).toMap().value("row").toString() == a.scale);
    CHECK(a.legend(a.opacity).value("color").toInt() != a.legend(a.scale).value("color").toInt());
    CHECK(a.legend(a.opacity).value("keys").toInt() == 3);
    CHECK_FALSE(a.legend(a.opacity).value("valueText").toString().isEmpty());

    const QVariantMap opacity = a.curve(a.opacity);
    CHECK(opacity.value("path").toList().size() == 2 * 241);
    CHECK(opacity.value("speedPath").toList().size() == 2 * 241);
    QVariantList keys = opacity.value("keys").toList();
    REQUIRE(keys.size() == 3);
    for (int i = 0; i < 3; ++i) {
        const double x = keys[i].toMap().value("x").toDouble();
        const double y = keys[i].toMap().value("y").toDouble();
        CHECK(x >= 0.0);
        CHECK(x <= 1.0);
        CHECK(y > 0.0); // a curve is fitted inside its window, with a margin
        CHECK(y < 1.0);
        if (i > 0)
            CHECK(x > keys[i - 1].toMap().value("x").toDouble());
    }
    // 100 %, 40 %, 90 %: the middle key is the lowest on screen (largest y).
    CHECK(keys[1].toMap().value("y").toDouble() > keys[0].toMap().value("y").toDouble());
    CHECK(keys[1].toMap().value("y").toDouble() > keys[2].toMap().value("y").toDouble());
    CHECK(keys[0].toMap().value("timeText").toString().isEmpty() == false);
    CHECK(keys[0].toMap().value("valueText").toString().contains(QStringLiteral("100")));

    // Selecting in the lane selects in the graph and the other way round; one selection.
    const double laneX = a.fx.row(a.opacity).value("keys").toList()[1].toMap().value("x").toDouble();
    REQUIRE(a.fx.pressKey(a.opacity, laneX, 0.01, 0));
    CHECK(a.graph.info().value("selCount").toInt() == 1);
    CHECK(a.graph.info().value("selRow").toString() == a.opacity);
    CHECK(a.key(a.opacity, 1).value("selected").toBool());
    CHECK_FALSE(a.key(a.opacity, 0).value("selected").toBool());
    a.fx.clearKeySelection();
    CHECK(a.graph.info().value("selCount").toInt() == 0);
    a.select(a.scale, 2);
    CHECK(a.fx.selectedKeyCount() == 1);
    CHECK(a.fx.row(a.scale).value("keys").toList()[2].toMap().value("selected").toBool());

    // A section collapsed in Effect Controls has no rows there, but its curves are still the editor's, and its keys select.
    a.fx.setCollapsed(QStringLiteral("opacity"), true);
    CHECK(a.fx.row(a.opacity).isEmpty());
    CHECK(a.graph.legend().size() == 2);
    CHECK(a.graph.curves().size() == 2);
    a.graph.clearSelection();
    a.select(a.opacity, 1);
    CHECK(a.graph.info().value("selCount").toInt() == 1);
    CHECK(a.key(a.opacity, 1).value("selected").toBool());
    a.fx.setCollapsed(QStringLiteral("opacity"), false);
    a.graph.clearSelection();
    a.select(a.scale, 2);

    // The legend hides curves; the data of a hidden curve is not built; "Selected" keeps the curves with selected keys.
    a.graph.setCurveVisible(a.opacity, false);
    CHECK(a.graph.curves().size() == 1);
    CHECK(a.graph.legend().size() == 2);
    CHECK_FALSE(a.legend(a.opacity).value("visible").toBool());
    CHECK(a.legend(a.scale).value("visible").toBool());
    a.graph.showAll();
    CHECK(a.graph.curves().size() == 2);
    a.graph.showSelected();
    REQUIRE(a.graph.curves().size() == 1);
    CHECK(a.graph.curves().at(0).toMap().value("row").toString() == a.scale);
    a.graph.isolate(a.opacity);
    REQUIRE(a.graph.curves().size() == 1);
    CHECK(a.graph.curves().at(0).toMap().value("row").toString() == a.opacity);
    CHECK(a.graph.curves().at(0).toMap().value("active").toBool());
    a.graph.showAll();
    // One shared scale puts both curves on one axis (their windows become the same range).
    a.graph.setSharedScale(true);
    CHECK(a.curve(a.opacity).value("lo").toDouble() == Approx(a.curve(a.scale).value("lo").toDouble()));
    CHECK(a.curve(a.opacity).value("hi").toDouble() == Approx(a.curve(a.scale).value("hi").toDouble()));
    a.graph.setSharedScale(false);
    CHECK(a.curve(a.opacity).value("hi").toDouble() != Approx(a.curve(a.scale).value("hi").toDouble()));
}

TEST_CASE("Graph Editor: dragging keys moves them in whole frames and in value, as one undo step", "[app][graph][a10]") {
    Animated a;
    const Ticks laneLength = a.controller.currentSequence()->findClip(a.clip)->clip().timelineRange.duration;
    const Curve before = a.curveOf(ParamId::Opacity);
    REQUIRE(before.keys.size() == 3);
    const QVariantMap key = a.key(a.opacity, 1);
    const double x = key.value("x").toDouble();
    const double y = key.value("y").toDouble();
    const std::size_t undo = a.undoCount();

    // A press on empty space is no key: the QML draws a selection box.
    CHECK(a.graph.press("value", 0.995, 0.5, 0.005, 0.01, 0) == QStringLiteral("none"));
    a.graph.release();

    REQUIRE(a.graph.press("value", x, y, 0.01, 0.03, 0) == QStringLiteral("key"));
    for (int step = 1; step <= 5; ++step)
        a.graph.dragTo("value", x + 0.02 * step, y - 0.02 * step, 0);
    a.graph.release();
    const Curve after = a.curveOf(ParamId::Opacity);
    REQUIRE(after.keys.size() == 3);
    const std::int64_t moved = (after.keys[1].time - before.keys[1].time).value;
    CHECK(moved > 0);
    CHECK(moved % a.frame.value == 0); // whole frames
    const double expected = 0.1 * static_cast<double>(laneLength.value);
    CHECK(static_cast<double>(moved) == Approx(expected).margin(static_cast<double>(a.frame.value)));
    CHECK(after.keys[1].value > before.keys[1].value); // up on screen is more opacity
    CHECK(after.keys[0].time == before.keys[0].time);
    CHECK(after.keys[0].value == before.keys[0].value);
    CHECK(a.undoCount() == undo + 1);
    CHECK(a.controller.undoText() == QStringLiteral("Undo Move Keyframes"));
    // The moved key stays selected, in the graph and in the lanes.
    CHECK(a.graph.info().value("selCount").toInt() == 1);
    CHECK(a.graph.info().value("selTimeText").toString() == a.graph.timeText(a.key(a.opacity, 1).value("x").toDouble()));
    REQUIRE(a.controller.undo());
    CHECK(a.curveOf(ParamId::Opacity).keys[1].time == before.keys[1].time);
    CHECK(a.curveOf(ParamId::Opacity).keys[1].value == before.keys[1].value);

    // Several keys of two curves move together in time (each by the same share of its own height in value).
    a.graph.boxSelect("value", 0.0, -5.0, 1.0, 5.0, 0);
    CHECK(a.graph.info().value("selCount").toInt() == 6);
    CHECK(a.fx.selectedKeyCount() == 6);
    const QVariantMap first = a.key(a.opacity, 0);
    const Curve scaleBefore = a.curveOf(ParamId::Scale);
    const std::size_t undo2 = a.undoCount();
    REQUIRE(a.graph.press("value", first.value("x").toDouble(), first.value("y").toDouble(), 0.01, 0.03, 0) == QStringLiteral("key"));
    for (int step = 1; step <= 3; ++step)
        a.graph.dragTo("value", first.value("x").toDouble() + 0.01 * step, first.value("y").toDouble(), 0);
    a.graph.release();
    const Curve scaleAfter = a.curveOf(ParamId::Scale);
    CHECK(scaleAfter.keys[0].time > scaleBefore.keys[0].time);
    CHECK((scaleAfter.keys[0].time - scaleBefore.keys[0].time) == (a.curveOf(ParamId::Opacity).keys[0].time - before.keys[0].time));
    CHECK(scaleAfter.keys[0].value == scaleBefore.keys[0].value); // no vertical movement
    CHECK(a.undoCount() == undo2 + 1);

    // A click without moving makes no edit; a key cannot be dragged onto another key.
    const std::size_t undo3 = a.undoCount();
    a.select(a.opacity, 2);
    CHECK(a.undoCount() == undo3);
    // Box select with Shift adds to the selection; clearing empties it in both views.
    a.graph.boxSelect("value", 0.0, -5.0, 1.0, 5.0, Qt::ShiftModifier);
    CHECK(a.graph.info().value("selCount").toInt() == 6);
    a.graph.clearSelection();
    CHECK(a.graph.info().value("selCount").toInt() == 0);
    CHECK(a.fx.selectedKeyCount() == 0);
}

TEST_CASE("Graph Editor: Bezier handles drag in the value graph and in the speed graph", "[app][graph][a10]") {
    Animated a;
    a.select(a.opacity, 1);
    a.fx.setKeyInterpolation(QStringLiteral("bezier"));
    QVariantMap key = a.key(a.opacity, 1);
    REQUIRE(key.value("hasOut").toBool());
    REQUIRE(key.value("hasIn").toBool());
    CHECK(key.value("outX").toDouble() > key.value("x").toDouble());
    CHECK(key.value("inX").toDouble() < key.value("x").toDouble());
    CHECK_FALSE(a.key(a.opacity, 0).value("hasOut").toBool()); // only selected Bezier keys show handles
    const Keyframe middle = a.curveOf(ParamId::Opacity).keys[1];

    // The value graph: a handle drag is the inline graph's maths - steeper and longer, the other side linked.
    std::size_t undo = a.undoCount();
    REQUIRE(a.graph.press("value", key.value("outX").toDouble(), key.value("outY").toDouble(), 0.01, 0.03, 0) == QStringLiteral("handle"));
    for (int step = 1; step <= 4; ++step)
        a.graph.dragTo("value", key.value("outX").toDouble() + 0.01 * step, key.value("outY").toDouble() - 0.04 * step, 0);
    a.graph.release();
    Keyframe shaped = a.curveOf(ParamId::Opacity).keys[1];
    CHECK(shaped.out.slope > middle.out.slope);
    CHECK(shaped.out.influence > middle.out.influence);
    CHECK(shaped.in.slope == shaped.out.slope);
    CHECK(a.undoCount() == undo + 1);
    CHECK(a.controller.undoText() == QStringLiteral("Undo Keyframe Handles"));

    // The speed graph: the handle sits at (its time, its slope); dragging it up makes it faster.
    key = a.key(a.opacity, 1);
    REQUIRE(key.value("sHasOut").toBool());
    undo = a.undoCount();
    const double sx = key.value("sOutX").toDouble();
    const double sy = key.value("sOutY").toDouble();
    REQUIRE(a.graph.press("speed", sx, sy, 0.01, 0.03, 0) == QStringLiteral("handle"));
    for (int step = 1; step <= 4; ++step)
        a.graph.dragTo("speed", sx, sy - 0.03 * step, 0);
    a.graph.release();
    const Keyframe faster = a.curveOf(ParamId::Opacity).keys[1];
    CHECK(faster.out.slope > shaped.out.slope);
    CHECK(faster.out.influence == Approx(shaped.out.influence)); // only the height moved
    CHECK(faster.in.slope == faster.out.slope);                    // linked
    CHECK(a.undoCount() == undo + 1);
    // Shift levels the handle (speed 0); Alt keeps the pair apart.
    key = a.key(a.opacity, 1);
    REQUIRE(a.graph.press("speed", key.value("sInX").toDouble(), key.value("sInY").toDouble(), 0.01, 0.03, 0) == QStringLiteral("handle"));
    a.graph.dragTo("speed", key.value("sInX").toDouble(), key.value("sInY").toDouble() - 0.05, Qt::AltModifier);
    a.graph.release();
    const Keyframe apart = a.curveOf(ParamId::Opacity).keys[1];
    CHECK(apart.in.slope != apart.out.slope);
    CHECK(apart.out.slope == faster.out.slope);
    key = a.key(a.opacity, 1);
    REQUIRE(a.graph.press("speed", key.value("sOutX").toDouble(), key.value("sOutY").toDouble(), 0.01, 0.03, 0) == QStringLiteral("handle"));
    a.graph.dragTo("speed", key.value("sOutX").toDouble(), 0.1, Qt::ShiftModifier);
    a.graph.release();
    CHECK(a.curveOf(ParamId::Opacity).keys[1].out.slope == 0.0);
}

TEST_CASE("Graph Editor: numeric entry, nudging, easing presets and adding a key are one undo step each", "[app][graph][a10]") {
    Animated a;
    a.select(a.opacity, 1);
    const Curve start = a.curveOf(ParamId::Opacity);

    // Value: 40 % becomes 65 %.
    std::size_t undo = a.undoCount();
    CHECK(a.graph.setSelectionValue(65.0));
    CHECK(a.curveOf(ParamId::Opacity).keys[1].value == Approx(0.65));
    CHECK(a.undoCount() == undo + 1);
    CHECK(a.controller.undoText() == QStringLiteral("Undo Set Keyframe Value"));
    CHECK(a.graph.info().value("selValue").toDouble() == Approx(65.0));
    CHECK(a.graph.setSelectionValue(65.0)); // no change, no step
    CHECK(a.undoCount() == undo + 1);
    CHECK(a.graph.setSelectionValue(500.0)); // clamped to the parameter's range
    CHECK(a.curveOf(ParamId::Opacity).keys[1].value == Approx(1.0));
    REQUIRE(a.controller.undo());
    REQUIRE(a.controller.undo());

    // Time: the typed timecode is two frames later.
    const QString now = a.graph.info().value("selTimeText").toString();
    REQUIRE_FALSE(now.isEmpty());
    const auto ticksOfText = a.controller.parseTimecodeText(now);
    REQUIRE(ticksOfText.has_value());
    undo = a.undoCount();
    CHECK(a.graph.setSelectionTime(a.controller.formatTimecode(*ticksOfText + a.frame * 2)));
    CHECK((a.curveOf(ParamId::Opacity).keys[1].time - start.keys[1].time) == a.frame * 2);
    CHECK(a.undoCount() == undo + 1);
    CHECK_FALSE(a.graph.setSelectionTime(QStringLiteral("not a time")));
    REQUIRE(a.controller.undo());
    a.graph.clearSelection(); // the selection names keys by time: pick the key again where it is now
    a.select(a.opacity, 1);

    // Nudge: a frame, and a step of the parameter.
    undo = a.undoCount();
    a.graph.nudge(-1, 0);
    CHECK((a.curveOf(ParamId::Opacity).keys[1].time - start.keys[1].time) == a.frame * -1);
    a.graph.nudge(0, 2);
    CHECK(a.curveOf(ParamId::Opacity).keys[1].value > start.keys[1].value);
    CHECK(a.undoCount() == undo + 2);
    REQUIRE(a.controller.undo());
    REQUIRE(a.controller.undo());

    // Easing: the segment after the first key.
    a.graph.clearSelection();
    a.select(a.opacity, 0);
    undo = a.undoCount();
    CHECK(a.graph.easingPresets().size() >= 30);
    CHECK(a.graph.easingPresets().at(0).toMap().value("thumb").toList().size() == 2 * 25);
    CHECK(a.graph.ease(QStringLiteral("nope")) == 0);
    CHECK(a.undoCount() == undo);
    CHECK(a.graph.ease(QStringLiteral("easeInOutCubic")) == 1);
    const Curve eased = a.curveOf(ParamId::Opacity);
    CHECK(eased.keys[0].interpolation == Interpolation::Bezier);
    CHECK(eased.keys[0].out.slope == 0.0); // the preset leaves flat
    CHECK(eased.keys[1].interpolation == Interpolation::Bezier);
    CHECK(eased.keys[1].in.slope == 0.0);
    CHECK(eased.keys[2].value == start.keys[2].value); // the rest is as it was
    CHECK(a.undoCount() == undo + 1);
    CHECK(a.controller.undoText() == QStringLiteral("Undo Ease Keyframes"));
    REQUIRE(a.controller.undo());
    CHECK(a.curveOf(ParamId::Opacity) == start);

    // Adding a key on the curve with a double click: the value the curve has there, on a frame.
    a.graph.clearSelection();
    const QVariantList path = a.curve(a.opacity).value("path").toList();
    const int at = 2 * 60; // a sample between the first and the second key
    const double px = path[at].toDouble();
    const double py = path[at + 1].toDouble();
    undo = a.undoCount();
    CHECK(a.graph.addKeyAt(px, py + 0.01, 0.03));
    const Curve added = a.curveOf(ParamId::Opacity);
    REQUIRE(added.keys.size() == 4);
    CHECK(added.keys[1].time > added.keys[0].time);
    CHECK(added.keys[1].time < added.keys[2].time);
    CHECK(added.keys[1].value == Approx(start.valueAt(added.keys[1].time)).margin(1e-9));
    CHECK(a.undoCount() == undo + 1);
    CHECK(a.graph.info().value("selCount").toInt() == 1); // the new key is selected
    CHECK_FALSE(a.graph.addKeyAt(px, 0.99, 0.01)); // not on any curve
    REQUIRE(a.controller.undo());
    CHECK(a.curveOf(ParamId::Opacity) == start);
}

TEST_CASE("Graph Editor: the time and value views zoom, pan and fit", "[app][graph][a10]") {
    Animated a;
    CHECK(a.graph.viewStart() == 0.0);
    CHECK(a.graph.viewEnd() == 1.0);
    a.graph.zoomTime(4.0, 0.5);
    CHECK(a.graph.viewEnd() - a.graph.viewStart() == Approx(0.25));
    CHECK((a.graph.viewStart() + a.graph.viewEnd()) / 2 == Approx(0.5));
    // The path is sampled across the window now.
    CHECK(a.curve(a.opacity).value("path").toList().first().toDouble() == Approx(a.graph.viewStart()));
    CHECK(a.curve(a.opacity).value("path").toList().at(2 * 240).toDouble() == Approx(a.graph.viewEnd()));
    a.graph.panTime(0.1);
    CHECK(a.graph.viewStart() == Approx(0.475));
    a.graph.panTime(5.0); // stops at the clip's end
    CHECK(a.graph.viewEnd() == Approx(1.0));
    a.graph.zoomTime(1e9, 0.5); // three frames at the most
    CHECK(a.graph.viewEnd() - a.graph.viewStart() > 0.0);
    CHECK(a.graph.viewEnd() - a.graph.viewStart() < 0.05);
    a.graph.zoomValue(2.0, 0.5);
    CHECK(a.graph.valueEnd() - a.graph.valueStart() == Approx(0.5));
    a.graph.panValue(0.1);
    CHECK(a.graph.valueStart() > 0.0);
    a.graph.resetView();
    CHECK(a.graph.viewStart() == 0.0);
    CHECK(a.graph.viewEnd() == 1.0);
    CHECK(a.graph.valueStart() == 0.0);
    CHECK(a.graph.valueEnd() == 1.0);
    // Fit the selected keys: the window closes in on them.
    a.select(a.opacity, 1);
    a.graph.fit(true);
    CHECK(a.graph.viewEnd() - a.graph.viewStart() < 0.2);
    CHECK(a.graph.viewStart() < a.key(a.opacity, 1).value("x").toDouble());
    CHECK(a.graph.viewEnd() > a.key(a.opacity, 1).value("x").toDouble());
    // Fit every key: the three keys of both curves fit.
    a.graph.fit(false);
    CHECK(a.graph.viewStart() < a.key(a.opacity, 0).value("x").toDouble());
    CHECK(a.graph.viewEnd() > a.key(a.opacity, 2).value("x").toDouble());
    // The time ruler's labels and the playhead.
    CHECK_FALSE(a.graph.timeText(0.5).isEmpty());
    a.graph.seek(0.5);
    CHECK(a.graph.playhead() == Approx(0.5).margin(0.01));
}

TEST_CASE("Graph Editor: another clip, or no clip, empties it", "[app][graph][a10]") {
    Timeline tl("V2 |Bbbbbbbbbbbbbbbbbbbb|\n"
                "V1 |Aaaaaaaaaaaaaaaaaaaa|");
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    const ClipId v1 = clipAt(*controller.snapshot(), tl.seq, "V1@0");
    const ClipId v2 = clipAt(*controller.snapshot(), tl.seq, "V2@0");
    controller.selectClipById(v1, ProjectController::Replace);
    controller.setPlayhead(Ticks{});
    EffectControlsModel& fx = *controller.effectControls();
    KeyframeGraph& graph = *fx.graph();
    const QString opacity = QStringLiteral("p:opacity");
    graph.setActive(true);
    CHECK(graph.legend().isEmpty()); // nothing animated yet
    CHECK_FALSE(graph.info().value("hasClip").toBool());
    fx.setAnimated(opacity, true);
    controller.setPlayhead(controller.frameTicks() * 24);
    fx.setValue(opacity, 50.0);
    fx.endEdit();
    REQUIRE(graph.legend().size() == 1);
    REQUIRE(graph.curves().size() == 1);
    CHECK(graph.info().value("hasClip").toBool());

    // Another clip that is not animated: the graph is empty; back to the first: the curve is back.
    controller.selectClipById(v2, ProjectController::Replace);
    CHECK(graph.curves().isEmpty());
    CHECK(graph.legend().isEmpty());
    CHECK_FALSE(graph.info().value("hasClip").toBool());
    // Nothing is selected in it: the edits do nothing and say so.
    CHECK_FALSE(graph.setSelectionValue(10.0));
    CHECK(graph.ease(QStringLiteral("easeIn")) == 0);
    CHECK(graph.press("value", 0.5, 0.5, 0.01, 0.01, 0) == QStringLiteral("none"));
    controller.selectClipById(v1, ProjectController::Replace);
    CHECK(graph.curves().size() == 1);
    // The panel closed: nothing is kept up to date.
    graph.setActive(false);
    controller.selectClipById(v2, ProjectController::Replace);
    CHECK(graph.curves().size() == 1);
}

TEST_CASE("Graph Editor in the shell: the panel shows the curves; pointer gestures edit them; screenshots", "[app][graph][a10][qml][screenshots]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    shell.resize(kShotWidth, kShotHeight);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(controller);
    EffectControlsModel& fx = *controller->effectControls();
    const ClipId clip = controller->primaryClip();
    const Clip c = controller->currentSequence()->findClip(clip)->clip();
    const Ticks frame = controller->frameTicks();
    const QString opacity = QStringLiteral("p:opacity");
    const QString scale = QStringLiteral("p:scale");
    controller->setPlayhead(c.start() + frame * 12);
    fx.setAnimated(opacity, true);
    fx.setAnimated(scale, true);
    for (const auto& [frames, o, sc] : {std::tuple{90, 40.0, 140.0}, std::tuple{180, 85.0, 110.0}, std::tuple{300, 55.0, 160.0}}) {
        controller->setPlayhead(c.start() + frame * frames);
        fx.setValue(opacity, o);
        fx.endEdit();
        fx.setValue(scale, sc);
        fx.endEdit();
    }
    controller->setPlayhead(c.start() + frame * 120);
    for (int i : {1, 2})
        fx.pressKey(scale, fx.row(scale).value("keys").toList()[i].toMap().value("x").toDouble(), 0.005, i == 1 ? 0 : static_cast<int>(Qt::ShiftModifier));
    fx.setKeyInterpolation(QStringLiteral("bezier"));

    // Window > Graph Editor opens it as a tab of a group.
    REQUIRE_FALSE(evaluate(shell, QStringLiteral("docking.isOpen('graphEditor')")).toBool());
    // Effect Controls' header has the button that opens it.
    REQUIRE_FALSE(shell.grab().isNull());
    spin(100);
    QQuickItem* button = named(shell.window->contentItem(), "graphEditorButton");
    REQUIRE(button != nullptr);
    REQUIRE(QMetaObject::invokeMethod(button, "clicked"));
    REQUIRE(waitFor([&] { return evaluate(shell, QStringLiteral("docking.isOpen('graphEditor')")).toBool(); }));
    spin(300);
    REQUIRE_FALSE(shell.grab().isNull());
    QQuickItem* panel = named(shell.window->contentItem(), "graphEditorPanel");
    REQUIRE(panel != nullptr);
    QQuickItem* valuePane = named(panel, "graphValuePane");
    REQUIRE(valuePane != nullptr);
    CHECK(valuePane->width() > 200);
    CHECK(valuePane->height() > 80);
    CHECK(named(panel, "graphLegend") != nullptr);
    CHECK(named(panel, "graphLegendRow_p:opacity") != nullptr);
    CHECK(named(panel, "graphLegendRow_p:scale") != nullptr);
    CHECK(named(panel, "graphSpeedPane") == nullptr); // off until asked for
    CHECK(named(panel, "graphEmptyState") == nullptr);
    CHECK(named(panel, "graphKey") != nullptr);
    CHECK(named(panel, "graphHandleOut") != nullptr); // the selected Bezier key shows its handles

    const auto save = [&](const char* name) {
        for (const bool dark : {true, false}) {
            shell.setAppearance(dark, "ultraviolet");
            REQUIRE(shell.settle(dark, shell.themeColor("accentFill")));
            REQUIRE_FALSE(shell.grab().isNull());
            spin(60);
            const QImage image = shell.grab();
            REQUIRE_FALSE(image.isNull());
            const double dpr = image.devicePixelRatio();
            const QRectF r = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
            REQUIRE(image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect())
                        .save(outputDir() + QStringLiteral("/graph-editor-%1-%2.png").arg(QLatin1String(name), dark ? QStringLiteral("dark") : QStringLiteral("light"))));
        }
        shell.setAppearance(true, "ultraviolet");
        REQUIRE(shell.settle(true, shell.themeColor("accentFill")));
    };
    save("value");

    // A real drag in the pane moves the first key of the first curve (Scale: Transform comes before Opacity) in time, as one
    // undo step.
    const auto firstOpacityKey = [&]() -> QQuickItem* {
        return findItem(valuePane, [&](QQuickItem* i) { return i->objectName() == QLatin1String("graphKey") && i->isVisible(); });
    };
    QQuickItem* key = firstOpacityKey();
    REQUIRE(key != nullptr);
    const Ticks beforeTime = controller->currentSequence()->findClip(clip)->clip().properties()[ParamId::Scale].curve()->keys[0].time;
    const std::size_t undo = controller->session().undoCount();
    const QPointF from = key->mapToScene(QPointF(key->width() / 2, key->height() / 2));
    mouse(shell.window.get(), QEvent::MouseButtonPress, from, Qt::LeftButton);
    for (int step = 1; step <= 6; ++step)
        mouse(shell.window.get(), QEvent::MouseMove, from + QPointF(6 * step, 0), Qt::LeftButton);
    mouse(shell.window.get(), QEvent::MouseButtonRelease, from + QPointF(36, 0), Qt::NoButton);
    spin(100);
    const Curve moved = *controller->currentSequence()->findClip(clip)->clip().properties()[ParamId::Scale].curve();
    CHECK(moved.keys[0].time > beforeTime);
    CHECK(controller->session().undoCount() == undo + 1);
    CHECK(controller->undoText() == QStringLiteral("Undo Move Keyframes"));

    // The speed graph.
    REQUIRE(QMetaObject::invokeMethod(panel, "focusContent"));
    fx.graph()->setSpeedGraph(true);
    spin(200);
    REQUIRE_FALSE(shell.grab().isNull());
    QQuickItem* speed = named(panel, "graphSpeedPane");
    REQUIRE(speed != nullptr);
    CHECK(speed->height() > 40);
    save("speed");

    // The easing library opens from the toolbar.
    fx.graph()->clearSelection();
    fx.graph()->setSpeedGraph(false);
    spin(100);
    const QVariantMap k0 = fx.graph()->curves().at(0).toMap().value("keys").toList().at(0).toMap();
    REQUIRE(fx.graph()->press("value", k0.value("x").toDouble(), k0.value("y").toDouble(), 0.01, 0.03, 0) == QStringLiteral("key"));
    fx.graph()->release();
    spin(100);
    QQuickItem* easing = named(panel, "graphEasingButton");
    REQUIRE(easing != nullptr);
    CHECK(easing->isEnabled());
    REQUIRE(QMetaObject::invokeMethod(easing, "clicked"));
    spin(300);
    REQUIRE_FALSE(shell.grab().isNull());
    CHECK(named(shell.window->contentItem(), "graphEasingGrid") != nullptr);
    {
        const QImage image = shell.grab();
        const double dpr = image.devicePixelRatio();
        const QRectF r = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
        REQUIRE(image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect())
                    .save(outputDir() + QStringLiteral("/graph-editor-easing.png")));
    }

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Motion Blur in Effect Controls: the effect's rows in the shell, one undo step per change (screenshots)",
          "[app][effectcontrols][motionblur][a10][qml][screenshots]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    shell.resize(kShotWidth, kShotHeight);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(controller);
    EffectControlsModel& fx = *controller->effectControls();
    const ClipId clip = controller->primaryClip();
    controller->setPlayhead(controller->currentSequence()->findClip(clip)->clip().start());
    REQUIRE(fx.addEffect(QStringLiteral("wabcut.blur.motion")).isEmpty());
    REQUIRE_FALSE(shell.grab().isNull());
    spin(100);

    // The four rows, in order: the angle and the phase keyframable, the two sample counts not.
    QString angleRow, previewRow;
    for (int i = 0; i < fx.rowCount(); ++i) {
        const QVariantMap row = fx.row(fx.keyAt(i));
        if (row.value("title").toString() == QStringLiteral("Shutter Angle"))
            angleRow = fx.keyAt(i);
        if (row.value("title").toString().startsWith(QStringLiteral("Preview Samples")))
            previewRow = fx.keyAt(i);
    }
    REQUIRE_FALSE(angleRow.isEmpty());
    REQUIRE_FALSE(previewRow.isEmpty());
    CHECK(fx.row(angleRow).value("animatable").toBool());
    CHECK_FALSE(fx.row(previewRow).value("animatable").toBool());
    CHECK(fx.row(angleRow).value("value").toDouble() == 180.0);
    CHECK(fx.row(angleRow).value("section").toString() == fx.row(previewRow).value("section").toString());

    const std::size_t undo = controller->session().undoCount();
    fx.setValue(angleRow, 270.0);
    fx.endEdit();
    CHECK(controller->session().undoCount() == undo + 1);
    const auto blur = motionBlurOf(controller->currentSequence()->findClip(clip)->clip().effects(), Ticks{});
    REQUIRE(blur.has_value());
    CHECK(blur->angle == 270.0);
    fx.setValue(angleRow, 0.0); // 0 is off
    fx.endEdit();
    CHECK_FALSE(motionBlurOf(controller->currentSequence()->findClip(clip)->clip().effects(), Ticks{}).has_value());
    fx.setValue(angleRow, 180.0);
    fx.endEdit();

    QObject* list = shell.object("effectControlsList");
    REQUIRE(list != nullptr);
    REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, fx.rowOf(previewRow)), Q_ARG(int, 2)));
    REQUIRE_FALSE(shell.grab().isNull());
    spin(60);
    QQuickItem* panel = shell.item("effectControls");
    REQUIRE(panel != nullptr);
    for (const bool dark : {true, false}) {
        shell.setAppearance(dark, "ultraviolet");
        REQUIRE(shell.settle(dark, shell.themeColor("accentFill")));
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        const double dpr = image.devicePixelRatio();
        const QRectF r = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
        REQUIRE(image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect())
                    .save(outputDir() + QStringLiteral("/effect-controls-motion-blur-%1.png").arg(dark ? QStringLiteral("dark") : QStringLiteral("light"))));
    }
    shell.setAppearance(true, "ultraviolet");
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
