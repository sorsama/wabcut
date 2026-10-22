// M5d: the finishing UI — Effect Controls (effect stack, stopwatch and keyframes, the keyframe lane with drag, Alt
// duplicate, rubber band and the interpolation menu), the Effects library, the Speed/Duration dialog, transitions on
// the timeline (drop with handle feedback, selection, edge drag) and the Titles seam. Offscreen; no window is shown.

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "CommandBridge.h"
#include "CommandPalette.h"
#include "EffectControls.h"
#include "ProjectController.h"
#include "Titles.h"
#include "TimelineView.h"

#include <wabcut/model/Animations.h>
#include <wabcut/model/GraphicOps.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QMouseEvent>
#include <QQuickItem>

#include <cmath>
#include <functional>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

Ticks operator*(Ticks t, std::int64_t n) {
    return Ticks{t.value * n};
}

/// The clip's effect stack in the current snapshot.
const EffectStack& stackOf(const ProjectController& c, ClipId clip) {
    return c.currentSequence()->findClip(clip)->clip().effects();
}

ParamValue paramOf(const ProjectController& c, ClipId clip, ParamId id) {
    return c.currentSequence()->findClip(clip)->clip().properties()[id];
}

QStringList rowKeys(const EffectControlsModel& m) {
    QStringList keys;
    for (int i = 0; i < m.rowCount(); ++i)
        keys << m.keyAt(i);
    return keys;
}

QVariantList keysOf(const EffectControlsModel& m, const QString& row) {
    return m.row(row).value(QStringLiteral("keys")).toList();
}

double keyX(const EffectControlsModel& m, const QString& row, int index) {
    return keysOf(m, row).value(index).toMap().value(QStringLiteral("x")).toDouble();
}

/// Depth-first search of the visual tree (ListView delegates are not QObject children of the window).
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

QQuickItem* rowItem(Shell& shell, const QString& key) {
    return findItem(shell.window->contentItem(), [&](QQuickItem* i) {
        return i->property("rowKey").toString() == key && i->property("kind").isValid() && i->isVisible();
    });
}

QQuickItem* childNamed(QQuickItem* root, const char* name) {
    return findItem(root, [&](QQuickItem* i) { return i->objectName() == QLatin1String(name) && i->isVisible(); });
}

void sendMouse(Shell& shell, QEvent::Type type, QPointF scene, Qt::MouseButton button, Qt::MouseButtons buttons,
               Qt::KeyboardModifiers mods = {}) {
    QMouseEvent event(type, scene, scene, button, buttons, mods);
    QCoreApplication::sendEvent(shell.window.get(), &event);
}

void click(Shell& shell, QQuickItem* item, Qt::MouseButton button = Qt::LeftButton) {
    REQUIRE(item != nullptr);
    const QPointF p = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    sendMouse(shell, QEvent::MouseButtonPress, p, button, button);
    sendMouse(shell, QEvent::MouseButtonRelease, p, button, Qt::NoButton);
    spin(20);
}

} // namespace

// ---- Effect Controls: sections and the effect stack ---------------------------------------------------------------

TEST_CASE("Effect Controls: pinned sections, then the effect stack; add, reorder, switch, reset and remove", "[app][effects]") {
    ProjectController controller;
    EffectControlsModel& fx = *controller.effectControls();
    // The demo starts with City B-roll selected (video only).
    const ClipId city = controller.primaryClip();
    REQUIRE(city);
    CHECK(fx.mode() == QStringLiteral("clip"));
    CHECK(fx.title() == QStringLiteral("City B-roll"));
    QStringList keys = rowKeys(fx);
    CHECK(keys.mid(0, 9) == QStringList{QStringLiteral("h:motion"), QStringLiteral("p:scale"), QStringLiteral("p:positionX"),
                                        QStringLiteral("p:positionY"), QStringLiteral("p:rotation"), QStringLiteral("p:anchorX"),
                                        QStringLiteral("p:anchorY"), QStringLiteral("h:opacity"), QStringLiteral("p:opacity")});
    CHECK_FALSE(keys.contains(QStringLiteral("h:audio"))); // no linked audio
    CHECK(fx.row(QStringLiteral("p:scale")).value(QStringLiteral("value")).toDouble() == 100.0);

    // Add two effects: each gets a header and its parameters; one undo step each.
    REQUIRE(fx.addEffect(QStringLiteral("wabcut.blur.gaussian")).isEmpty());
    CHECK(controller.undoText() == QStringLiteral("Undo Add Effect"));
    REQUIRE(fx.addEffect(QStringLiteral("wabcut.color.basic")).isEmpty());
    REQUIRE(stackOf(controller, city).size() == 2);
    const QString blur = effectKey(stackOf(controller, city)[0].id);
    const QString basic = effectKey(stackOf(controller, city)[1].id);
    keys = rowKeys(fx);
    CHECK(keys.indexOf(QStringLiteral("h:") + blur) > keys.indexOf(QStringLiteral("p:opacity")));
    CHECK(keys.contains(blur + QStringLiteral(":radius")));
    CHECK(keys.indexOf(QStringLiteral("h:") + basic) > keys.indexOf(blur + QStringLiteral(":radius")));
    CHECK(fx.row(QStringLiteral("h:") + blur).value(QStringLiteral("title")).toString() == QStringLiteral("Gaussian Blur"));
    CHECK(fx.row(blur + QStringLiteral(":repeatEdges")).value(QStringLiteral("kind")).toString() == QStringLiteral("toggle"));

    // An audio effect has no audio clip to go to; a transition is not an effect.
    CHECK_FALSE(fx.addEffect(QStringLiteral("wabcut.audio.gain")).isEmpty());
    CHECK_FALSE(fx.addEffect(QStringLiteral("crossDissolve")).isEmpty());

    // Reorder: Basic Correction first (the rows follow, the header moves with its parameters).
    fx.moveEffect(basic, 0);
    CHECK(stackOf(controller, city)[0].effect == "wabcut.color.basic");
    keys = rowKeys(fx);
    CHECK(keys.indexOf(QStringLiteral("h:") + basic) < keys.indexOf(QStringLiteral("h:") + blur));
    CHECK(fx.stackIndexForRow(keys.indexOf(blur + QStringLiteral(":radius")), QStringLiteral("video")) == 1);
    CHECK(fx.stackIndexForRow(0, QStringLiteral("video")) == 0);
    CHECK(controller.undoText() == QStringLiteral("Undo Move Effect"));

    // A parameter (UI units: px), the fx switch, reset, remove.
    fx.setValue(blur + QStringLiteral(":radius"), 24.0);
    fx.endEdit();
    const auto radius = [&] {
        for (const EffectInstance& e : stackOf(controller, city)) {
            if (effectKey(e.id) == blur)
                return effectParameterAt(e, "radius", Ticks{});
        }
        return -1.0;
    };
    CHECK(radius() == 24.0);
    CHECK(fx.row(blur + QStringLiteral(":radius")).value(QStringLiteral("value")).toDouble() == 24.0);
    fx.setEffectEnabled(blur, false);
    CHECK_FALSE(stackOf(controller, city)[1].enabled);
    CHECK_FALSE(fx.row(QStringLiteral("h:") + blur).value(QStringLiteral("effectEnabled")).toBool());
    fx.resetSection(blur);
    CHECK(radius() == 0.0);
    fx.removeSection(basic);
    REQUIRE(stackOf(controller, city).size() == 1);
    CHECK_FALSE(rowKeys(fx).contains(QStringLiteral("h:") + basic));

    // Collapse hides a section's rows (UI state only).
    fx.setCollapsed(QStringLiteral("motion"), true);
    CHECK_FALSE(rowKeys(fx).contains(QStringLiteral("p:scale")));
    CHECK(fx.row(QStringLiteral("h:motion")).value(QStringLiteral("collapsed")).toBool());
    fx.setCollapsed(QStringLiteral("motion"), false);
    CHECK(rowKeys(fx).contains(QStringLiteral("p:scale")));

    // A linked clip shows the audio partner's sections; audio effects go to it.
    const SequencePtr seq = controller.currentSequence();
    controller.selectClipById(seq->videoTracks[0]->clips.front().id, ProjectController::Replace);
    CHECK(rowKeys(fx).contains(QStringLiteral("h:audio")));
    CHECK(rowKeys(fx).contains(QStringLiteral("p:volume")));
    REQUIRE(fx.addEffect(QStringLiteral("wabcut.audio.gain")).isEmpty());
    const ClipId audio = controller.currentSequence()->audioTracks[0]->clips.front().id;
    CHECK(stackOf(controller, audio).size() == 1);
    CHECK(fx.row(QStringLiteral("h:") + effectKey(stackOf(controller, audio)[0].id)).value(QStringLiteral("stackKind")).toString()
          == QStringLiteral("audio"));
}

TEST_CASE("Effect Controls: Simple Text's text row and menu parameters of library effects (0.2)", "[app][effects]") {
    ProjectController controller;
    EffectControlsModel& fx = *controller.effectControls();
    const ClipId city = controller.primaryClip();
    REQUIRE(city);
    REQUIRE(fx.addEffect(QStringLiteral("wabcut.video.simpleText")).isEmpty());
    const QString section = effectKey(stackOf(controller, city)[0].id);
    const QString textRow = section + QStringLiteral(":data:text");
    REQUIRE(rowKeys(fx).contains(textRow));
    CHECK(fx.row(textRow).value(QStringLiteral("kind")).toString() == QStringLiteral("text"));
    CHECK(fx.row(textRow).value(QStringLiteral("text")).toString() == QStringLiteral("Simple Text"));
    CHECK(fx.row(section + QStringLiteral(":alignment")).value(QStringLiteral("kind")).toString() == QStringLiteral("choice"));

    fx.setEffectText(textRow, QStringLiteral("Chapter 2: The Fall"));
    const EffectData* data = stackOf(controller, city)[0].findData("text");
    REQUIRE(data);
    REQUIRE(data->text());
    CHECK(QString::fromStdString(*data->text()) == QStringLiteral("Chapter 2: The Fall"));
    CHECK(fx.row(textRow).value(QStringLiteral("text")).toString() == QStringLiteral("Chapter 2: The Fall"));
    CHECK(controller.undoText() == QStringLiteral("Undo Simple Text: Text"));
    controller.undo();
    CHECK_FALSE(stackOf(controller, city)[0].findData("text"));
    // Past the data bound: refused with a notice, nothing changes.
    fx.setEffectText(textRow, QString(1100, QLatin1Char('x')));
    CHECK_FALSE(stackOf(controller, city)[0].findData("text"));
}

// ---- Keyframes ------------------------------------------------------------------------------------------------------

TEST_CASE("Stopwatch: a value becomes a curve; changes at other times add keys; navigation and the key toggle", "[app][keyframes]") {
    ProjectController controller;
    EffectControlsModel& fx = *controller.effectControls();
    const ClipId city = controller.primaryClip();
    const Clip clip = controller.currentSequence()->findClip(city)->clip();
    const Ticks frame = controller.frameTicks();
    const QString opacity = QStringLiteral("p:opacity");

    controller.setPlayhead(clip.start() + frame * 24);
    fx.setAnimated(opacity, true);
    CHECK(controller.undoText() == QStringLiteral("Undo Toggle Animation"));
    REQUIRE(paramOf(controller, city, ParamId::Opacity).isCurve());
    CHECK(paramOf(controller, city, ParamId::Opacity).curve()->keys.size() == 1);
    CHECK(fx.row(opacity).value(QStringLiteral("animated")).toBool());
    CHECK(fx.row(opacity).value(QStringLiteral("keyAtPlayhead")).toBool());
    CHECK(keysOf(fx, opacity).size() == 1);

    // Another time: a new key with the new value (Premiere: an animated parameter keys on change).
    controller.setPlayhead(clip.start() + frame * 96);
    CHECK_FALSE(fx.row(opacity).value(QStringLiteral("keyAtPlayhead")).toBool());
    fx.setValue(opacity, 40.0);
    fx.endEdit();
    const ParamValue animated = paramOf(controller, city, ParamId::Opacity);
    const Curve* curve = animated.curve();
    REQUIRE(curve);
    REQUIRE(curve->keys.size() == 2);
    CHECK(curve->keys[1].value == 0.4);
    CHECK(fx.row(opacity).value(QStringLiteral("value")).toDouble() == 40.0);
    CHECK(keysOf(fx, opacity).size() == 2);
    CHECK(keyX(fx, opacity, 0) < keyX(fx, opacity, 1));
    // The value between the keys is interpolated (the playhead between them).
    controller.setPlayhead(clip.start() + frame * 60);
    const double between = fx.row(opacity).value(QStringLiteral("value")).toDouble();
    CHECK(between < 100.0);
    CHECK(between > 40.0);

    // Navigation: previous / next key.
    CHECK(fx.row(opacity).value(QStringLiteral("hasPreviousKey")).toBool());
    CHECK(fx.row(opacity).value(QStringLiteral("hasNextKey")).toBool());
    fx.goToKey(opacity, -1);
    CHECK(controller.playhead() == clip.start() + frame * 24);
    fx.goToKey(opacity, 1);
    CHECK(controller.playhead() == clip.start() + frame * 96);

    // The diamond button: remove the key here, add it back.
    fx.toggleKeyAtPlayhead(opacity);
    CHECK(paramOf(controller, city, ParamId::Opacity).curve()->keys.size() == 1);
    CHECK(controller.undoText() == QStringLiteral("Undo Remove Keyframe"));
    fx.toggleKeyAtPlayhead(opacity);
    CHECK(paramOf(controller, city, ParamId::Opacity).curve()->keys.size() == 2);

    // Stopwatch off: a constant again (the value at the playhead), keys gone.
    fx.setAnimated(opacity, false);
    CHECK(paramOf(controller, city, ParamId::Opacity).isConstant());
    CHECK_FALSE(fx.row(opacity).value(QStringLiteral("animated")).toBool());
    REQUIRE(controller.undo());
    CHECK(paramOf(controller, city, ParamId::Opacity).isCurve());

    // Effect parameters animate the same way.
    REQUIRE(fx.addEffect(QStringLiteral("wabcut.blur.gaussian")).isEmpty());
    const QString radius = effectKey(stackOf(controller, city).back().id) + QStringLiteral(":radius");
    fx.setAnimated(radius, true);
    CHECK(fx.row(radius).value(QStringLiteral("animated")).toBool());
    CHECK(stackOf(controller, city).back().find("radius")->isCurve());
}

TEST_CASE("Keyframe lane: drag moves the selected keys in one undo step, Alt duplicates, rubber band and interpolation", "[app][keyframes]") {
    ProjectController controller;
    EffectControlsModel& fx = *controller.effectControls();
    const ClipId city = controller.primaryClip();
    const Clip clip = controller.currentSequence()->findClip(city)->clip();
    const Ticks frame = controller.frameTicks();
    const QString opacity = QStringLiteral("p:opacity");
    controller.setPlayhead(clip.start() + frame * 24);
    fx.setAnimated(opacity, true);
    controller.setPlayhead(clip.start() + frame * 96);
    fx.setValue(opacity, 40.0);
    fx.endEdit();
    const double tolerance = 0.01;
    const auto times = [&] {
        std::vector<Ticks> out;
        for (const Keyframe& k : paramOf(controller, city, ParamId::Opacity).curve()->keys)
            out.push_back(k.time);
        return out;
    };
    const std::vector<Ticks> original = times();
    const double laneFrames = static_cast<double>(clip.duration().value) / static_cast<double>(frame.value);

    // Select the first key, drag it 24 frames later in several moves: one undo step.
    REQUIRE(fx.pressKey(opacity, keyX(fx, opacity, 0), tolerance, 0));
    CHECK(fx.selectedKeyCount() == 1);
    REQUIRE(fx.beginKeyDrag(false));
    for (int step = 1; step <= 4; ++step)
        fx.dragKeys(6.0 * step / laneFrames);
    fx.endKeyDrag();
    std::vector<Ticks> moved = times();
    REQUIRE(moved.size() == 2);
    CHECK(moved[0] == original[0] + frame * 24);
    CHECK(moved[1] == original[1]);
    CHECK(controller.undoText() == QStringLiteral("Undo Move Keyframes"));
    CHECK(fx.selectedKeyCount() == 1); // the moved key stays selected
    REQUIRE(controller.undo());
    CHECK(times() == original);

    // Onto another key: refused, the key stays where it was last.
    REQUIRE(fx.pressKey(opacity, keyX(fx, opacity, 0), tolerance, 0));
    REQUIRE(fx.beginKeyDrag(false));
    fx.dragKeys(72.0 / laneFrames); // exactly onto the second key
    fx.endKeyDrag();
    CHECK(times() == original);

    // Alt-drag duplicates: the original stays, a copy lands later.
    REQUIRE(fx.pressKey(opacity, keyX(fx, opacity, 0), tolerance, 0));
    REQUIRE(fx.beginKeyDrag(true));
    fx.dragKeys(12.0 / laneFrames);
    fx.endKeyDrag();
    REQUIRE(times().size() == 3);
    CHECK(times()[0] == original[0]);
    CHECK(times()[1] == original[0] + frame * 12);
    CHECK(controller.undoText() == QStringLiteral("Undo Duplicate Keyframes"));
    REQUIRE(controller.undo());
    CHECK(times().size() == 2);

    // Rubber band over the row selects both keys; Shift-click toggles one off with Ctrl.
    fx.selectKeysInRange({opacity}, 0.0, 1.0, 0);
    CHECK(fx.selectedKeyCount() == 2);
    fx.pressKey(opacity, keyX(fx, opacity, 1), tolerance, Qt::ControlModifier);
    CHECK(fx.selectedKeyCount() == 1);
    fx.pressKey(opacity, keyX(fx, opacity, 1), tolerance, Qt::ShiftModifier);
    CHECK(fx.selectedKeyCount() == 2);
    CHECK(keysOf(fx, opacity).value(0).toMap().value(QStringLiteral("selected")).toBool());

    // Interpolation menu: every selected key, one undo step; the lane shape follows.
    fx.setKeyInterpolation(QStringLiteral("hold"));
    for (const Keyframe& k : paramOf(controller, city, ParamId::Opacity).curve()->keys)
        CHECK(k.interpolation == Interpolation::Hold);
    CHECK(fx.selectedKeyInterpolation() == QStringLiteral("hold"));
    CHECK(keysOf(fx, opacity).value(0).toMap().value(QStringLiteral("shape")).toString() == QStringLiteral("hold"));
    CHECK(controller.undoText() == QStringLiteral("Undo Keyframe Interpolation"));
    fx.setKeyInterpolation(QStringLiteral("easeInOut"));
    CHECK(fx.selectedKeyInterpolation() == QStringLiteral("easeInOut"));

    // Delete the selected keys: the parameter keeps its value at the playhead.
    fx.deleteSelectedKeys();
    CHECK(paramOf(controller, city, ParamId::Opacity).isConstant());
    CHECK(fx.selectedKeyCount() == 0);

    // A press beside every key clears the selection.
    fx.selectKeysInRange({opacity}, 0.0, 1.0, 0);
    fx.pressKey(opacity, 0.999, tolerance, 0);
    CHECK(fx.selectedKeyCount() == 0);
}

// ---- Effect Controls in the shell: clicks, the lane, the interpolation menu -----------------------------------------

TEST_CASE("Effect Controls in the shell: stopwatch click, lane drag with the mouse, right-click interpolation menu", "[app][keyframes][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    // A hidden window advances no animations: with reduce motion the rows take their places at once (no displaced
    // transition), so the points below are where the rows really are.
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    EffectControlsModel& fx = *controller->effectControls();
    const ClipId city = controller->primaryClip();
    const Clip clip = controller->currentSequence()->findClip(city)->clip();
    const Ticks frame = controller->frameTicks();
    REQUIRE_FALSE(shell.grab().isNull());

    // The stopwatch of Opacity, clicked: animated with a key at the playhead. The list scrolls the row to its top
    // (rows never move here, so no animation has to run in the hidden window).
    controller->setPlayhead(clip.start() + frame * 24);
    QObject* list = shell.object("effectControlsList");
    REQUIRE(list != nullptr);
    REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, fx.rowOf(QStringLiteral("h:opacity"))),
                                      Q_ARG(int, 0)));
    spin(50);
    QQuickItem* opacityRow = rowItem(shell, QStringLiteral("p:opacity"));
    REQUIRE(opacityRow != nullptr);
    QQuickItem* stopwatch = childNamed(opacityRow, "stopwatch");
    REQUIRE(stopwatch != nullptr);
    click(shell, stopwatch);
    REQUIRE(paramOf(*controller, city, ParamId::Opacity).isCurve());
    controller->setPlayhead(clip.start() + frame * 96);
    fx.setValue(QStringLiteral("p:opacity"), 30.0);
    fx.endEdit();
    spin(30);
    REQUIRE(keysOf(fx, QStringLiteral("p:opacity")).size() == 2);

    // Drag the first diamond in the lane with the mouse: it moves; one undo step.
    QQuickItem* lane = shell.item("keyframeLane");
    REQUIRE(lane != nullptr);
    REQUIRE(lane->isVisible());
    opacityRow = rowItem(shell, QStringLiteral("p:opacity"));
    const double rowY = opacityRow->mapToItem(lane, QPointF(0, opacityRow->height() / 2)).y();
    const QPointF from = lane->mapToScene(QPointF(keyX(fx, QStringLiteral("p:opacity"), 0) * lane->width(), rowY));
    const QPointF to = from + QPointF(lane->width() * 24.0 / (static_cast<double>(clip.duration().value) / frame.value), 0);
    const Ticks before = paramOf(*controller, city, ParamId::Opacity).curve()->keys[0].time;
    sendMouse(shell, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    CHECK(lane->property("pressed").toBool());
    for (int i = 1; i <= 4; ++i)
        sendMouse(shell, QEvent::MouseMove, from + (to - from) * (i / 4.0), Qt::NoButton, Qt::LeftButton);
    sendMouse(shell, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
    spin(30);
    const Ticks after = paramOf(*controller, city, ParamId::Opacity).curve()->keys[0].time;
    CHECK(after > before);
    CHECK(controller->undoText() == QStringLiteral("Undo Move Keyframes"));
    REQUIRE(controller->undo());
    CHECK(paramOf(*controller, city, ParamId::Opacity).curve()->keys[0].time == before);
    spin(30);

    // Right-click a key: the interpolation menu; Bezier applies to it.
    const QPointF keyPoint = lane->mapToScene(QPointF(keyX(fx, QStringLiteral("p:opacity"), 1) * lane->width(), rowY));
    sendMouse(shell, QEvent::MouseButtonPress, keyPoint, Qt::RightButton, Qt::RightButton);
    sendMouse(shell, QEvent::MouseButtonRelease, keyPoint, Qt::RightButton, Qt::NoButton);
    spin(60);
    QObject* menu = shell.object("keyframeMenu");
    REQUIRE(menu != nullptr);
    CHECK(menu->property("visible").toBool());
    QQuickItem* bezier = findItem(shell.window->contentItem(), [](QQuickItem* i) {
        return i->property("text").toString() == QStringLiteral("Bezier") && i->inherits("QQuickMenuItem");
    });
    REQUIRE(bezier != nullptr);
    QMetaObject::invokeMethod(bezier, "triggered");
    spin(30);
    CHECK(paramOf(*controller, city, ParamId::Opacity).curve()->keys[1].interpolation == Interpolation::Bezier);
    QMetaObject::invokeMethod(menu, "close");
    spin(50);

    // The effect stack through its headers: three effects (as dropped), the fx switch and the trash clicked, the
    // grip dragged to reorder.
    REQUIRE(fx.addEffect(QStringLiteral("wabcut.blur.gaussian")).isEmpty());
    REQUIRE(fx.addEffect(QStringLiteral("wabcut.vignette")).isEmpty());
    REQUIRE(fx.addEffect(QStringLiteral("wabcut.sharpen")).isEmpty());
    const EffectStack three = stackOf(*controller, city);
    REQUIRE(three.size() == 3);
    const QString blur = effectKey(three[0].id), vignette = effectKey(three[1].id), sharpen = effectKey(three[2].id);
    // Rows are only scrolled here, never moved before a click (see reduceMotion above).
    const auto scrollTo = [&](const QString& key) {
        REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, fx.rowOf(key)), Q_ARG(int, 0)));
        spin(50);
    };
    scrollTo(QStringLiteral("h:") + vignette);
    click(shell, childNamed(rowItem(shell, QStringLiteral("h:") + vignette), "effectEnabledButton"));
    CHECK_FALSE(stackOf(*controller, city)[1].enabled);
    click(shell, childNamed(rowItem(shell, QStringLiteral("h:") + sharpen), "sectionRemove"));
    REQUIRE(stackOf(*controller, city).size() == 2);
    scrollTo(QStringLiteral("h:") + blur);
    // Drag Vignette's grip above Blur: Vignette first.
    QQuickItem* vignetteRow = rowItem(shell, QStringLiteral("h:") + vignette);
    QQuickItem* blurRow = rowItem(shell, QStringLiteral("h:") + blur);
    REQUIRE(vignetteRow != nullptr);
    REQUIRE(blurRow != nullptr);
    QQuickItem* grip = childNamed(vignetteRow, "sectionGrip");
    REQUIRE(grip != nullptr);
    const QPointF gripPoint = grip->mapToScene(QPointF(grip->width() / 2, grip->height() / 2));
    const QPointF above = blurRow->mapToScene(QPointF(0, blurRow->height() * 0.25));
    sendMouse(shell, QEvent::MouseButtonPress, gripPoint, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 8; ++i)
        sendMouse(shell, QEvent::MouseMove, QPointF(gripPoint.x(), gripPoint.y() + (above.y() - gripPoint.y()) * i / 8.0),
                  Qt::NoButton, Qt::LeftButton);
    CHECK(vignetteRow->property("dragging").toBool()); // lifted while dragged
    sendMouse(shell, QEvent::MouseButtonRelease, QPointF(gripPoint.x(), above.y()), Qt::LeftButton, Qt::NoButton);
    spin(50);
    REQUIRE(stackOf(*controller, city).size() == 2);
    CHECK(effectKey(stackOf(*controller, city)[0].id) == vignette);
    CHECK(controller->undoText() == QStringLiteral("Undo Move Effect"));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// ---- Effects library ------------------------------------------------------------------------------------------------

TEST_CASE("Effects library: grouped registry, fuzzy search, panel rows", "[app][effects]") {
    ProjectController controller;
    const EffectControlsModel& fx = *controller.effectControls();
    const QVariantList all = fx.library();
    QStringList titles;
    for (const QVariant& g : all)
        titles << g.toMap().value(QStringLiteral("title")).toString();
    CHECK(titles == QStringList{QStringLiteral("Video Effects"), QStringLiteral("Video Transitions"),
                                QStringLiteral("Audio Effects"), QStringLiteral("Audio Transitions")});
    const QVariantList videoEffects = all.front().toMap().value(QStringLiteral("items")).toList();
    QStringList names;
    QStringList ids;
    for (const QVariant& i : videoEffects) {
        names << i.toMap().value(QStringLiteral("name")).toString();
        ids << i.toMap().value(QStringLiteral("id")).toString();
    }
    CHECK(names.contains(QStringLiteral("Gaussian Blur")));
    CHECK(names.contains(QStringLiteral("Crop")));
    CHECK_FALSE(ids.contains(QStringLiteral("wabcut.motion"))); // intrinsic: not in the library
    CHECK(ids.contains(QStringLiteral("wabcut.distort.transform"))); // 0.2: Premiere's Transform effect is
    CHECK_FALSE(names.contains(QStringLiteral("Title Style"))); // comes with each title

    const QVariantList blur = fx.library(QStringLiteral("blur"));
    REQUIRE_FALSE(blur.isEmpty());
    CHECK(blur.front().toMap().value(QStringLiteral("title")).toString() == QStringLiteral("Video Effects"));
    QStringList blurIds;
    for (const QVariant& i : blur.front().toMap().value(QStringLiteral("items")).toList())
        blurIds << i.toMap().value(QStringLiteral("id")).toString();
    CHECK(blurIds.contains(QStringLiteral("wabcut.blur.gaussian")));

    // 0.2: Premiere's bins, alphabetical, with their effects alphabetical inside.
    const QVariantList transitionBins = all.at(1).toMap().value(QStringLiteral("bins")).toList();
    QStringList binTitles;
    for (const QVariant& b : transitionBins)
        binTitles << b.toMap().value(QStringLiteral("title")).toString();
    for (const char* bin : {"Dissolve", "Iris", "Page Peel", "Slide", "Wipe", "Zoom"})
        CHECK(binTitles.contains(QLatin1String(bin)));
    QStringList sorted = binTitles;
    std::sort(sorted.begin(), sorted.end(), [](const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
    CHECK(binTitles == sorted);
    const QVariantList iris = transitionBins.at(binTitles.indexOf(QStringLiteral("Iris"))).toMap().value(QStringLiteral("items")).toList();
    QStringList irisNames;
    for (const QVariant& i : iris)
        irisNames << i.toMap().value(QStringLiteral("name")).toString();
    CHECK(irisNames == QStringList{QStringLiteral("Iris Box"), QStringLiteral("Iris Cross"), QStringLiteral("Iris Diamond"),
                                   QStringLiteral("Iris Round")});
    // "dip": the dips lead the video transitions (fuzzy matches such as Turbulent Displace come under Video Effects).
    QVariantMap dips;
    for (const QVariant& g : fx.library(QStringLiteral("dip"))) {
        if (g.toMap().value(QStringLiteral("title")).toString() == QStringLiteral("Video Transitions"))
            dips = g.toMap();
    }
    REQUIRE_FALSE(dips.isEmpty());
    const QVariantList dipItems = dips.value(QStringLiteral("items")).toList();
    REQUIRE_FALSE(dipItems.isEmpty());
    CHECK(dipItems.front().toMap().value(QStringLiteral("name")).toString().startsWith(QStringLiteral("Dip to")));
    CHECK(fx.library(QStringLiteral("zzzz")).isEmpty());
    CHECK(fx.isTransition(QStringLiteral("crossDissolve")));
    CHECK_FALSE(fx.isTransition(QStringLiteral("wabcut.crop")));
}

// ---- Speed / Duration -----------------------------------------------------------------------------------------------

TEST_CASE("Speed/Duration dialog: linked speed and duration, dry run, apply as one undo step", "[app][speed][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true); // the dialog's timing is not what is tested
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    const ClipId city = controller->primaryClip(); // 14 s
    const Ticks length = controller->currentSequence()->findClip(city)->clip().duration();
    CHECK(controller->durationForSpeed(200.0) == controller->formatTimecode(Ticks{length.value / 2}));
    CHECK(controller->speedForDuration(controller->formatTimecode(Ticks{length.value / 2})) == 200.0);
    CHECK(controller->speedDurationInfo().value(QStringLiteral("available")).toBool());

    // Ctrl/Cmd+R routes through the keymap to Main.qml, which opens the dialog.
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("clip.speedDuration"))));
    QObject* dialog = shell.object("speedDurationDialog");
    REQUIRE(dialog != nullptr);
    REQUIRE(waitFor([&] { return dialog->property("visible").toBool(); }));
    CHECK(dialog->property("speed").toDouble() == 100.0);

    const auto snapshot = controller->snapshot();
    REQUIRE(QMetaObject::invokeMethod(dialog, "setSpeed", Q_ARG(QVariant, 200.0)));
    CHECK(dialog->property("duration").toString() == controller->formatTimecode(Ticks{length.value / 2}));
    const QVariantMap preview = dialog->property("preview").toMap();
    INFO(preview.value(QStringLiteral("error")).toString().toStdString());
    CHECK(preview.value(QStringLiteral("ok")).toBool());
    CHECK(controller->snapshot() == snapshot); // the dry run changes nothing

    // Typing a duration recomputes the speed.
    QVariant typed;
    REQUIRE(QMetaObject::invokeMethod(dialog, "setDuration", Q_RETURN_ARG(QVariant, typed),
                                      Q_ARG(QVariant, controller->formatTimecode(Ticks{length.value / 2}))));
    CHECK(typed.toBool());
    CHECK(dialog->property("speed").toDouble() == 200.0);
    CHECK(dialog->property("driver").toString() == QStringLiteral("duration"));

    REQUIRE(QMetaObject::invokeMethod(dialog, "apply"));
    CHECK(waitFor([&] { return !dialog->property("visible").toBool(); }));
    CHECK(controller->currentSequence()->findClip(city)->clip().duration() * 2 == length);
    CHECK(controller->undoText() == QStringLiteral("Undo Speed/Duration"));
    REQUIRE(controller->undo());
    CHECK(controller->currentSequence()->findClip(city)->clip().duration() == length);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// ---- Transitions ----------------------------------------------------------------------------------------------------

TEST_CASE("Transitions: drop on an edit point with handle feedback, select, edge drag, Effect Controls rows", "[app][transitions]") {
    // Two clips meeting at frame 32 ("A20" counts 3 frames); B starts 20 frames into its media (handles for a centred
    // 1 s transition).
    Rig rig(R"(
        V1 tgt |A20aaaaaaaaaaaaaaaaaaaaaaaaaaaaaB20bbbbbbbbbbbbbbbbbbbbbbbbbbbbb|
        A1 tgt |A20aaaaaaaaaaaaaaaaaaaaaaaaaaaaaB20bbbbbbbbbbbbbbbbbbbbbbbbbbbbb|
    )");
    TimelineView& view = *rig.view;
    ProjectController& controller = rig.controller;
    EffectControlsModel& fx = *controller.effectControls();
    const auto transitions = [&] { return controller.currentSequence()->videoTracks[0]->transitions; };
    const double cut = static_cast<double>(controller.currentSequence()->videoTracks[0]->clips[1].start().value) / f(1).value;
    REQUIRE(cut == 32.0);

    // Hovering near the cut: a 1 s cross dissolve centred on it.
    const QVariantMap target = view.effectDropTarget(rig.x(cut + 1), rig.y("V1"), QStringLiteral("crossDissolve"));
    CHECK(target.value(QStringLiteral("ok")).toBool());
    CHECK_FALSE(target.value(QStringLiteral("warning")).toBool());
    CHECK(target.value(QStringLiteral("kind")).toString() == QStringLiteral("edit"));
    CHECK(target.value(QStringLiteral("message")).toString().startsWith(QStringLiteral("Cross Dissolve")));
    CHECK(target.value(QStringLiteral("x")).toDouble() < rig.x(cut));
    CHECK(target.value(QStringLiteral("x")).toDouble() + target.value(QStringLiteral("width")).toDouble() > rig.x(cut));
    // On the wrong kind of track, or an effect on empty space: refused with the reason.
    const QVariantMap wrong = view.effectDropTarget(rig.x(cut + 1), rig.y("A1"), QStringLiteral("crossDissolve"));
    CHECK_FALSE(wrong.value(QStringLiteral("ok")).toBool());
    CHECK(wrong.value(QStringLiteral("message")).toString().contains(QStringLiteral("video transition")));
    const QVariantMap onClip = view.effectDropTarget(rig.x(10), rig.y("V1"), QStringLiteral("wabcut.blur.gaussian"));
    CHECK(onClip.value(QStringLiteral("ok")).toBool());
    CHECK(onClip.value(QStringLiteral("kind")).toString() == QStringLiteral("clip"));

    // Drop: one transition, selected; Effect Controls shows it.
    CHECK(view.dropEffect(rig.x(cut + 1), rig.y("V1"), QStringLiteral("crossDissolve")).isEmpty());
    REQUIRE(transitions().size() == 1);
    const Transition t = transitions().front();
    CHECK(t.duration() == f(25));
    CHECK(controller.selectedTransitionId() == t.id);
    CHECK(controller.selection().empty());
    CHECK(fx.mode() == QStringLiteral("transition"));
    CHECK(fx.title() == QStringLiteral("Cross Dissolve"));
    CHECK(rowKeys(fx).contains(QStringLiteral("t:duration")));
    CHECK(fx.row(QStringLiteral("t:alignment")).value(QStringLiteral("text")).toString() == QStringLiteral("center"));
    CHECK(view.transitionAt(rig.x(cut + 1), rig.y("V1")) == transitionKey(t.id));
    rig.harness.frame();

    // Clicking a clip selects it instead; clicking the transition selects it again.
    rig.press(QPointF(rig.x(5), rig.y("V1")));
    rig.release(QPointF(rig.x(5), rig.y("V1")));
    CHECK_FALSE(controller.selectedTransitionId());
    rig.press(QPointF(rig.x(cut), rig.y("V1")));
    rig.release(QPointF(rig.x(cut), rig.y("V1")));
    CHECK(controller.selectedTransitionId() == t.id);

    // Drag its right edge 5 frames later: longer after the cut, one undo step.
    const double right = rig.x(cut) + (static_cast<double>(t.after.value) * rig.pxPerFrame) / static_cast<double>(f(1).value);
    rig.drag(QPointF(right - 2, rig.y("V1")), QPointF(right - 2 + 5 * rig.pxPerFrame, rig.y("V1")));
    REQUIRE(transitions().size() == 1);
    INFO("after " << transitions().front().after.value / f(1).value << " frames, was " << t.after.value / f(1).value
                  << "; " << controller.undoText().toStdString() << "; " << controller.lastError().toStdString() << "; "
                  << join(takeMessages()));
    CHECK(transitions().front().after == t.after + f(5));
    CHECK(transitions().front().before == t.before);
    CHECK(controller.undoText() == QStringLiteral("Undo Transition Duration"));
    REQUIRE(controller.undo());
    CHECK(transitions().front().after == t.after);

    // Effect Controls: duration and alignment.
    fx.setTransitionDuration(QStringLiteral("00:00:00:12"));
    CHECK(transitions().front().duration() == f(12));
    fx.setTransitionAlignment(QStringLiteral("startAtCut"));
    CHECK(transitions().front().before == Ticks{});
    CHECK(transitions().front().after == f(12));
    CHECK(fx.row(QStringLiteral("t:alignment")).value(QStringLiteral("text")).toString() == QStringLiteral("startAtCut"));
    fx.setTransitionEffect(QStringLiteral("dipToBlack"));
    CHECK(transitions().front().effect == "dipToBlack");

    // Delete with the transition selected removes it.
    REQUIRE(controller.runCommand(QStringLiteral("edit.clear")));
    CHECK(transitions().empty());
    CHECK_FALSE(controller.selectedTransitionId());
    REQUIRE(controller.undo());
    CHECK(transitions().size() == 1);
}

TEST_CASE("Transitions: short media is fitted to the available handles, and said so before the drop", "[app][transitions]") {
    // B starts 4 frames into its media: a centred 1 s transition lacks 8 frames of head handle.
    Rig rig(R"(
        V1 tgt |A20aaaaaaaaaaaaaaaaaaaaaaaaaaaaaB4bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb|
    )");
    const QVariantMap target = rig.view->effectDropTarget(rig.x(31), rig.y("V1"), QStringLiteral("crossDissolve"));
    CHECK(target.value(QStringLiteral("ok")).toBool());
    CHECK(target.value(QStringLiteral("warning")).toBool());
    CHECK(target.value(QStringLiteral("message")).toString().contains(QStringLiteral("available handles")));
    CHECK(rig.view->dropEffect(rig.x(31), rig.y("V1"), QStringLiteral("crossDissolve")).isEmpty());
    const auto& transitions = rig.controller.currentSequence()->videoTracks[0]->transitions;
    REQUIRE(transitions.size() == 1);
    CHECK(transitions.front().before <= f(4));

    // Ctrl/Cmd+D at the playhead (frame 0): the nearest edit is A's head, which gets a one-sided transition.
    CHECK(rig.controller.applyDefaultTransition(false));
    const auto& after = rig.controller.currentSequence()->videoTracks[0]->transitions;
    REQUIRE(after.size() == 2);
    CHECK_FALSE(after.front().outgoing);
    CHECK(rig.controller.undoText() == QStringLiteral("Undo Apply Video Transition"));
}

TEST_CASE("Animations: drop on a clip's In and Out points; lengthen with the handle; remove; Surprise Me picks one",
          "[app][transitions][animations]") {
    // 0.2: Premiere 25.5's Animations sit on a clip's edge like transitions.
    const std::string picture = "\n        V1 tgt |A" + std::string(79, 'a') + "|\n";
    Rig rig(picture);
    TimelineView& view = *rig.view;
    ProjectController& controller = rig.controller;
    EffectControlsModel& fx = *controller.effectControls();
    const auto transitions = [&] { return controller.currentSequence()->videoTracks[0]->transitions; };
    const ClipId clip = controller.currentSequence()->videoTracks[0]->clips[0].id;
    const double y = rig.y("V1");

    // The Effects panel's Video Transitions have an Animations bin with Surprise Me.
    QStringList animations;
    for (const QVariant& group : fx.library(QString())) {
        for (const QVariant& bin : group.toMap().value(QStringLiteral("bins")).toList()) {
            if (bin.toMap().value(QStringLiteral("title")).toString() != QLatin1String("Animations"))
                continue;
            CHECK(group.toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("videoTransition"));
            for (const QVariant& item : bin.toMap().value(QStringLiteral("items")).toList())
                animations << item.toMap().value(QStringLiteral("name")).toString();
        }
    }
    INFO(animations.join(QStringLiteral(", ")).toStdString());
    CHECK(animations.size() == 10);
    CHECK(animations.contains(QStringLiteral("Surprise Me")));
    CHECK(animations.contains(QStringLiteral("Fold")));

    // On the In point: the clip pops in over a second; one undo step; selected.
    const QVariantMap target = view.effectDropTarget(rig.x(2), y, QStringLiteral("animationPop"));
    CHECK(target.value(QStringLiteral("ok")).toBool());
    CHECK(target.value(QStringLiteral("message")).toString().startsWith(QStringLiteral("Pop")));
    REQUIRE(view.dropEffect(rig.x(2), y, QStringLiteral("animationPop")).isEmpty());
    REQUIRE(transitions().size() == 1);
    const Transition in = transitions().front();
    CHECK(in.effect == "animationPop");
    CHECK_FALSE(in.outgoing);
    CHECK(in.incoming == clip);
    CHECK(in.before == Ticks{});
    CHECK(in.after == f(25));
    CHECK(controller.undoText() == QStringLiteral("Undo Add Transition"));
    CHECK(controller.selectedTransitionId() == in.id);
    CHECK(fx.title() == QStringLiteral("Pop"));
    rig.harness.frame();

    // Its duration handle: the right edge dragged 5 frames later lengthens it, one undo step.
    const double right = rig.x(25);
    rig.drag(QPointF(right - 2, y), QPointF(right - 2 + 5 * rig.pxPerFrame, y));
    REQUIRE(transitions().size() == 1);
    INFO(controller.undoText().toStdString() << "; " << controller.lastError().toStdString());
    CHECK(transitions().front().after == f(30));
    CHECK(controller.undoText() == QStringLiteral("Undo Transition Duration"));

    // On the Out point: the clip travels out.
    REQUIRE(view.dropEffect(rig.x(78), y, QStringLiteral("animationTravel")).isEmpty());
    REQUIRE(transitions().size() == 2);
    CHECK(transitions().back().effect == "animationTravel");
    CHECK(transitions().back().outgoing == clip);
    CHECK_FALSE(transitions().back().incoming);
    CHECK(transitions().back().before == f(25));

    // Surprise Me becomes one of the animations.
    REQUIRE(controller.undo());
    REQUIRE(transitions().size() == 1);
    REQUIRE(view.dropEffect(rig.x(78), y, QString::fromLatin1(kSurpriseAnimation.data())).isEmpty());
    REQUIRE(transitions().size() == 2);
    const Transition surprise = transitions().back();
    INFO(surprise.effect);
    CHECK(surprise.effect != kSurpriseAnimation);
    CHECK(isAnimationTransition(surprise.effect));
    CHECK(controller.selectedTransitionId() == surprise.id);
    CHECK(fx.title() != QStringLiteral("Surprise Me"));

    // Removable: Delete with it selected.
    REQUIRE(controller.runCommand(QStringLiteral("edit.clear")));
    REQUIRE(transitions().size() == 1);
    CHECK(transitions().front().effect == "animationPop");
}

TEST_CASE("Text Styles: a style dragged from the Library onto a graphic clip styles its text layers in one undo step",
          "[app][graphics][textStyles]") {
    Rig rig(R"(
        V2     |..............................|
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )");
    ProjectController& controller = rig.controller;
    TimelineView& view = *rig.view;
    const auto textLayer = [](std::uint32_t id, std::string value) {
        GraphicLayer l;
        l.id = id;
        l.name = "Text " + std::to_string(id);
        l.kind = LayerKind::Text;
        TextStyle style;
        style.fontSize = 48.0;
        l.text.runs = {TextRun{std::move(value), style}};
        return l;
    };
    // A graphic on V2: a text layer, a shape and a group holding another text layer; "Title" made from the first.
    Graphic g;
    GraphicLayer shape;
    shape.id = 2;
    shape.name = "Box";
    shape.kind = LayerKind::Shape;
    shape.shape.width = ParamValue(100.0);
    shape.shape.height = ParamValue(100.0);
    GraphicLayer group;
    group.id = 3;
    group.name = "Group";
    group.kind = LayerKind::Group;
    group.children = {textLayer(4, "Inside")};
    g.layers = {textLayer(1, "Hello"), shape, group};
    auto added = edit::addGraphic(*controller.snapshot(), rig.tl.seq, rig.tl.track("V2"), Ticks{}, f(20), g);
    REQUIRE(added);
    const ClipId graphic = added->placed.at(0);
    auto styled = edit::createTextStyle(added->state, rig.tl.seq, graphic, 1, "Title");
    REQUIRE(styled);
    controller.resetProject(styled->state, rig.tl.seq);
    rig.harness.frame();
    CHECK(edit::textLayerIds(*styled->state.findSequence(rig.tl.seq)->findClip(graphic)->clip().graphic()) ==
          std::vector<std::uint32_t>{1, 4});
    const auto layer = [&](std::uint32_t id) {
        return controller.currentSequence()->findClip(graphic)->clip().graphic()->findLayer(id)->textStyle;
    };
    CHECK(layer(4).empty());

    // Over the graphic: it lights up; over a video clip or with an unknown style: refused with the reason.
    const QVariantMap target = view.effectDropTarget(rig.x(5), rig.y("V2"), QStringLiteral("textStyle:Title"));
    CHECK(target.value(QStringLiteral("ok")).toBool());
    CHECK(target.value(QStringLiteral("kind")).toString() == QStringLiteral("clip"));
    CHECK(target.value(QStringLiteral("message")).toString() == QStringLiteral("Apply Title"));
    CHECK(target.value(QStringLiteral("width")).toDouble() > 0.0);
    const QVariantMap onVideo = view.effectDropTarget(rig.x(5), rig.y("V1"), QStringLiteral("textStyle:Title"));
    CHECK_FALSE(onVideo.value(QStringLiteral("ok")).toBool());
    CHECK(onVideo.value(QStringLiteral("message")).toString().contains(QStringLiteral("graphic")));
    CHECK_FALSE(view.effectDropTarget(rig.x(5), rig.y("V2"), QStringLiteral("textStyle:Nope")).value(QStringLiteral("ok")).toBool());
    CHECK_FALSE(view.dropEffect(rig.x(5), rig.y("V1"), QStringLiteral("textStyle:Title")).isEmpty());

    // The drop: every text layer (the grouped one too) follows Title; the shape is left alone; one undo step.
    const auto before = rig.undoSteps();
    CHECK(view.dropEffect(rig.x(5), rig.y("V2"), QStringLiteral("textStyle:Title")).isEmpty());
    CHECK(rig.undoSteps() == before + 1);
    CHECK(controller.undoText() == QStringLiteral("Undo Apply Text Style"));
    CHECK(layer(1) == "Title");
    CHECK(layer(4) == "Title");
    REQUIRE(controller.undo());
    CHECK(layer(4).empty());
}

// ---- Titles ---------------------------------------------------------------------------------------------------------

TEST_CASE("Titles: the style editor, presets, the command contract and the seam", "[app][titles]") {
    ProjectController controller;
    TitlesBridge titles;
    titles.setController(&controller);
    CHECK(titles.style().value(QStringLiteral("text")).toString() == QStringLiteral("Your Title"));
    titles.setStyleValue(QStringLiteral("text"), QStringLiteral("Hello"));
    titles.setStyleValue(QStringLiteral("size"), 120.0);
    titles.applyPreset({{QStringLiteral("preset"), QStringLiteral("lime")}, {QStringLiteral("color"), QStringLiteral("#B0E000")},
                        {QStringLiteral("strokeEnabled"), true}});
    CHECK(titles.style().value(QStringLiteral("preset")).toString() == QStringLiteral("lime"));
    CHECK(titles.style().value(QStringLiteral("text")).toString() == QStringLiteral("Hello")); // presets keep the text
    titles.setStyleValue(QStringLiteral("size"), 80.0);
    CHECK(titles.style().value(QStringLiteral("preset")).toString().isEmpty()); // edited after the preset

    const QVariantMap params = titles.commandParams();
    CHECK(params.value(QStringLiteral("text")).toString() == QStringLiteral("Hello"));
    CHECK(params.value(QStringLiteral("fontFamily")).toString() == QStringLiteral("Arial"));
    CHECK(params.value(QStringLiteral("fontStyle")).toString() == QStringLiteral("Bold"));
    const QVariantMap style = params.value(QStringLiteral("style")).toMap();
    CHECK(std::abs(style.value(QStringLiteral("fill.red")).toDouble() - 0xB0 / 255.0) < 1e-6);
    CHECK(style.value(QStringLiteral("stroke.width")).toDouble() == 4.0); // the preset turned the stroke on
    CHECK(style.value(QStringLiteral("size")).toDouble() == 80.0);
    CHECK(params.value(QStringLiteral("at")).toString() == controller.playheadText());
    CHECK(params.value(QStringLiteral("track")).toString().startsWith(QLatin1Char('V')));

    // New Title: a title clip at the playhead on a free video track at or above the targeted one, selected.
    REQUIRE(titles.available());
    const QString track = params.value(QStringLiteral("track")).toString();
    REQUIRE(titles.createTitle().isEmpty());
    CHECK(controller.undoText() == QStringLiteral("Undo New Title"));
    const ClipId clip = controller.primaryClip();
    REQUIRE(clip);
    const SequencePtr seq = controller.currentSequence();
    const auto loc = seq->findClip(clip);
    REQUIRE(loc);
    CHECK(QStringLiteral("V%1").arg(seq->trackPosition(loc->track->id)->index + 1) == track);
    const MediaItem* item = controller.snapshot()->findMedia(loc->clip().media);
    REQUIRE(item);
    REQUIRE(item->isTitle());
    CHECK(item->generator->text == "Hello");
    CHECK(titles.editing());
    CHECK(titles.style().value(QStringLiteral("size")).toDouble() == 80.0);
    CHECK(titles.style().value(QStringLiteral("color")).toString() == QStringLiteral("#B0E000"));
    CHECK(titles.style().value(QStringLiteral("strokeEnabled")).toBool());
    // Effect Controls shows its Title Style effect.
    CHECK(controller.effectControls()->rowOf(QStringLiteral("h:") + effectKey(loc->clip().effects().front().id)) >= 0);

    // Editing the selected title: text on the title item, style on its effect; a size drag is one undo step.
    titles.setStyleValue(QStringLiteral("text"), QStringLiteral("Hello world"));
    titles.endEdit();
    CHECK(controller.snapshot()->findMedia(loc->clip().media)->generator->text == "Hello world");
    CHECK(controller.undoText() == QStringLiteral("Undo Edit Title Text"));
    titles.setStyleValue(QStringLiteral("size"), 90.0);
    titles.setStyleValue(QStringLiteral("size"), 140.0);
    titles.endEdit();
    const auto sizeNow = [&] {
        const Clip& c = controller.currentSequence()->findClip(clip)->clip();
        return effectParameterAt(c.effects().front(), "size", Ticks{});
    };
    CHECK(sizeNow() == 140.0);
    CHECK(titles.style().value(QStringLiteral("size")).toDouble() == 140.0);
    REQUIRE(controller.undo());
    CHECK(sizeNow() == 80.0);
    titles.setStyleValue(QStringLiteral("alignment"), QStringLiteral("left"));
    CHECK(effectParameterAt(controller.currentSequence()->findClip(clip)->clip().effects().front(), "alignment", Ticks{}) == 0.0);
    titles.setStyleValue(QStringLiteral("font"), QStringLiteral("serif"));
    CHECK(controller.snapshot()->findMedia(loc->clip().media)->generator->fontFamily == "Georgia");

    // Deselected: the panel edits the draft again.
    controller.clearSelection();
    CHECK_FALSE(titles.editing());
    CHECK(titles.style().value(QStringLiteral("text")).toString() == QStringLiteral("Hello"));
}

// ---- Screenshots ----------------------------------------------------------------------------------------------------

TEST_CASE("Finishing screenshots: Effect Controls with keyframes, the Effects library, the speed dialog", "[app][screenshots][finishing]") {
    resetSettings();
    clearMessages();

    {
        Shell shell;
        REQUIRE(shell.window != nullptr);
        auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        EffectControlsModel& fx = *controller->effectControls();
        const ClipId city = controller->primaryClip();
        const Clip clip = controller->currentSequence()->findClip(city)->clip();
        const Ticks frame = controller->frameTicks();
        // Scale and Opacity animated with keys of different shapes, a Gaussian Blur and a Basic Correction.
        controller->setPlayhead(clip.start() + frame * 12);
        fx.setAnimated(QStringLiteral("p:scale"), true);
        fx.setAnimated(QStringLiteral("p:opacity"), true);
        controller->setPlayhead(clip.start() + frame * 120);
        fx.setValue(QStringLiteral("p:scale"), 130.0);
        fx.endEdit();
        fx.setValue(QStringLiteral("p:opacity"), 55.0);
        fx.endEdit();
        controller->setPlayhead(clip.start() + frame * 240);
        fx.setValue(QStringLiteral("p:scale"), 115.0);
        fx.endEdit();
        fx.selectKeysInRange({QStringLiteral("p:scale")}, 0.4, 1.0, 0);
        fx.setKeyInterpolation(QStringLiteral("bezier"));
        fx.selectKeysInRange({QStringLiteral("p:opacity")}, 0.0, 0.2, 0);
        fx.setKeyInterpolation(QStringLiteral("hold"));
        REQUIRE(fx.addEffect(QStringLiteral("wabcut.blur.gaussian")).isEmpty());
        fx.setValue(effectKey(stackOf(*controller, city).back().id) + QStringLiteral(":radius"), 8.0);
        fx.endEdit();
        REQUIRE(fx.addEffect(QStringLiteral("wabcut.color.basic")).isEmpty());
        fx.setCollapsed(effectKey(stackOf(*controller, city).back().id), true);
        controller->setPlayhead(clip.start() + frame * 180);
        fx.selectKeysInRange({QStringLiteral("p:scale")}, 0.3, 0.5, 0);
        // A taller window shows more of Effect Controls; the list from its top (Transform with the Scale keys).
        shell.resize(kShotWidth, 1280);
        QObject* list = shell.object("effectControlsList");
        REQUIRE(list != nullptr);
        REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, 0), Q_ARG(int, 0)));
        // The Effects panel in the browser slot.
        REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showBrowserPanel", Q_ARG(QVariant, QStringLiteral("effects"))));
        spin(450);
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        REQUIRE(image.save(outputDir() + QStringLiteral("/effect-controls.png")));
        const double dpr = shell.window->effectiveDevicePixelRatio();
        const auto crop = [&](const char* name, const QString& file) {
            QQuickItem* item = shell.item(name);
            REQUIRE(item != nullptr);
            const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
            REQUIRE(image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect()).save(outputDir() + file));
        };
        crop("effectControls", QStringLiteral("/effect-controls-crop.png"));
        crop("effectsPanel", QStringLiteral("/effects-library.png"));
    }
    {
        Shell shell;
        REQUIRE(shell.window != nullptr);
        REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("clip.speedDuration"))));
        QObject* dialog = shell.object("speedDurationDialog");
        REQUIRE(dialog != nullptr);
        REQUIRE(QMetaObject::invokeMethod(dialog, "setSpeed", Q_ARG(QVariant, 200.0)));
        spin(450);
        REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/speed-duration.png")));
    }
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
