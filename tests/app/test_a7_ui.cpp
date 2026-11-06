// 0.7, batch A7: several markers at once (card 119: the bridge, the ruler's flags and marquee, the Markers panel), clip notes
// (card 125: the controller, the timeline's drawing, the menu and its popover), keyframes that follow edits (card 120: the
// Effect Controls pin, Sequence > Ripple Track Keyframes) and the pacing strip (card 180: the bridge and its strip). C++ rigs
// for the drawing, the real shell (Main.qml, hidden) for the QML. Screenshots under test-output/ui (a7-*.png).

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "BridgeIds.h"
#include "EffectControls.h"
#include "MarkersBridge.h"
#include "PacingBridge.h"
#include "ProjectController.h"
#include "TimelineView.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/KeyPins.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QKeyEvent>
#include <QQuickItem>
#include <QSettings>

#include <qpa/qwindowsysteminterface.h>

#include <atomic>
#include <expected>
#include <map>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

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

QObject* named(QQuickWindow* window, const char* name) {
    QObject* found = window->findChild<QObject*>(QLatin1String(name));
    INFO(name);
    REQUIRE(found);
    return found;
}

QQuickItem* itemNamed(QQuickWindow* window, const char* name) {
    QQuickItem* found = findNamed(window->contentItem(), QLatin1String(name));
    INFO(name);
    REQUIRE(found);
    return found;
}

/// A pointer event through the window system (as the platform delivers it: handlers see real taps, the modifiers count).
void pointer(QQuickWindow* window, QEvent::Type type, QPointF at, Qt::MouseButton button, Qt::MouseButtons buttons,
             Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(window, at, window->mapToGlobal(at), buttons,
                                                                                          button, type, modifiers);
}

void click(QQuickWindow* window, QPointF at, Qt::KeyboardModifiers modifiers = Qt::NoModifier, Qt::MouseButton button = Qt::LeftButton) {
    pointer(window, QEvent::MouseMove, at, Qt::NoButton, Qt::NoButton, modifiers);
    pointer(window, QEvent::MouseButtonPress, at, button, button, modifiers);
    pointer(window, QEvent::MouseButtonRelease, at, button, Qt::NoButton, modifiers);
    spin(30);
}

void clickItem(QQuickWindow* window, QQuickItem* item, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    REQUIRE(item);
    click(window, item->mapToScene(QPointF(item->width() / 2, item->height() / 2)), modifiers);
}

/// A drag from `from` to `to` in eight steps, holding `modifiers`.
void drag(QQuickWindow* window, QPointF from, QPointF to, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    pointer(window, QEvent::MouseMove, from, Qt::NoButton, Qt::NoButton, modifiers);
    pointer(window, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton, modifiers);
    for (int i = 1; i <= 8; ++i)
        pointer(window, QEvent::MouseMove, from + (to - from) * (i / 8.0), Qt::NoButton, Qt::LeftButton, modifiers);
    pointer(window, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton, modifiers);
    spin(30);
}

void key(QQuickWindow* window, int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier, const QString& text = QString()) {
    QKeyEvent press(QEvent::KeyPress, code, modifiers, text);
    QCoreApplication::sendEvent(window, &press);
    QKeyEvent release(QEvent::KeyRelease, code, modifiers, text);
    QCoreApplication::sendEvent(window, &release);
    spin(30);
}

Ticks operator*(Ticks t, std::int64_t n) { return Ticks{t.value * n}; }

void settle(Rig& rig) {
    for (int i = 0; i < 4; ++i) {
        QCoreApplication::processEvents();
        rig.harness.frame();
    }
}

std::string stripe(char letter, int frames) {
    return std::string(1, letter) + std::string(static_cast<std::size_t>(frames - 1), static_cast<char>(letter + 32));
}

/// The sequence's markers: m<i> at the given frames.
void setMarkers(ProjectController& c, const std::vector<int>& frames) {
    std::vector<Marker> markers;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        Marker m;
        m.start = f(frames[i]);
        m.name = "m" + std::to_string(i);
        markers.push_back(m);
    }
    const SequenceId seq = c.currentSequenceId();
    REQUIRE(c.applyEditFn("Add Markers", [&](const ProjectState& st) { return edit::setSequenceMarkers(st, seq, markers); }));
}

std::string layout(const ProjectController& c) {
    std::string out;
    for (const Marker& m : c.currentSequence()->markers) {
        if (!out.empty())
            out += ' ';
        out += m.name + "@" + std::to_string(ticksToFrames(m.start, kRate));
    }
    return out;
}

std::vector<int> selection(const MarkersBridge& markers) {
    std::vector<int> out;
    for (const QVariant& v : markers.selection())
        out.push_back(v.toInt());
    return out;
}

/// The real shell over a project of one long clip and `markers` at the given frames.
struct ShellMarkers {
    Shell shell;
    ProjectController* controller = nullptr;
    MarkersBridge* markers = nullptr;
    Timeline tl;

    explicit ShellMarkers(const std::vector<int>& frames, std::string picture = "V1 tgt |" + stripe('A', 400) + "|",
                          const std::map<char, std::int64_t>& mediaFrames = {})
        : tl(picture, mediaFrames) {
        REQUIRE(shell.window);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        shell.setAppearance(true, "ultraviolet");
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        markers = shell.engine.singletonInstance<MarkersBridge*>("Wabcut", "Markers");
        REQUIRE(controller);
        REQUIRE(markers);
        controller->resetProject(tl.state, tl.seq);
        if (!frames.empty())
            setMarkers(*controller, frames);
        spin(200);
    }
    QQuickWindow* window() const { return shell.window.get(); }
    std::size_t undo() const { return controller->session().undoCount(); }
    TimelineView* view() const {
        auto* v = qobject_cast<TimelineView*>(findNamed(shell.window->contentItem(), QStringLiteral("timelineView")));
        REQUIRE(v);
        return v;
    }
    void save(const QRectF& rect, const QString& file) const {
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        const double dpr = image.devicePixelRatio();
        const QRectF r = rect.intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
        REQUIRE(image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect()).save(QDir(outputDir()).filePath(file)));
    }
    QRectF sceneRect(const QQuickItem* item) const { return item->mapRectToScene(QRectF(0, 0, item->width(), item->height())); }
};

} // namespace

// ---- 119: the bridge ------------------------------------------------------------------------------------------------------

TEST_CASE("Markers: select, delete, move, nudge, cut, copy and paste several at once, each one undo step (card 119)",
          "[app][markers][group]") {
    resetSettings();
    ShellMarkers s({10, 20, 30, 40, 50, 60});
    MarkersBridge& m = *s.markers;
    ProjectController& c = *s.controller;
    REQUIRE(m.totalCount() == 6);

    SECTION("selecting: replace, toggle, extend from the anchor, select all within the filters") {
        m.select(1);
        CHECK(selection(m) == std::vector<int>{1});
        m.select(3, QStringLiteral("toggle"));
        CHECK(selection(m) == std::vector<int>{1, 3});
        m.select(1, QStringLiteral("toggle"));
        CHECK(selection(m) == std::vector<int>{3});
        m.select(2);
        m.select(4, QStringLiteral("extend"));
        CHECK(selection(m) == std::vector<int>{2, 3, 4});
        m.select(0, QStringLiteral("extend")); // from the same anchor, the other way
        CHECK(selection(m) == std::vector<int>{0, 1, 2});
        m.select(99);                           // there is no such marker: nothing changes
        CHECK(selection(m) == std::vector<int>{0, 1, 2});
        m.selectAll();
        CHECK(m.selectionCount() == 6);
        m.clearSelection();
        CHECK(m.selectionCount() == 0);
        // The colour filter limits Select All to what the panel lists.
        REQUIRE(m.update(0, {{QStringLiteral("colorIndex"), 1}}));
        m.setColorMask(1 << 1);
        m.selectAll();
        CHECK(selection(m) == std::vector<int>{0});
        m.setColorMask(0xFF);
    }
    SECTION("Delete removes the selection as one undo step; undo brings it back and the selection is dropped") {
        m.select(1);
        m.select(3, QStringLiteral("toggle"));
        const std::size_t steps = s.undo();
        REQUIRE(m.removeSelected());
        CHECK(layout(c) == "m0@10 m2@30 m4@50 m5@60");
        CHECK(s.undo() == steps + 1);
        CHECK(c.undoText() == QStringLiteral("Undo Remove Markers"));
        CHECK(m.selectionCount() == 0);
        REQUIRE(c.undo());
        CHECK(layout(c) == "m0@10 m1@20 m2@30 m3@40 m4@50 m5@60");
        CHECK(m.selectionCount() == 0);
        CHECK_FALSE(m.removeSelected()); // nothing selected
    }
    SECTION("moving by a distance or by frames: one step, clamped at 0, the group stays selected where it lands") {
        m.select(2);
        m.select(3, QStringLiteral("toggle"));
        const std::size_t steps = s.undo();
        REQUIRE(m.moveSelectedBy(QString::number(f(5).value)));
        CHECK(layout(c) == "m0@10 m1@20 m2@35 m3@45 m4@50 m5@60");
        CHECK(s.undo() == steps + 1);
        CHECK(c.undoText() == QStringLiteral("Undo Move Markers"));
        CHECK(selection(m) == std::vector<int>{2, 3});
        REQUIRE(m.nudgeSelected(3)); // past m4 (50): the list re-sorts, the selection follows

        CHECK(layout(c) == "m0@10 m1@20 m2@38 m3@48 m4@50 m5@60");
        CHECK(selection(m) == std::vector<int>{2, 3});
        m.select(0);
        REQUIRE(m.nudgeSelected(-50)); // stops at 0
        CHECK(c.currentSequence()->markers.front().start == Ticks{});
        CHECK_FALSE(m.nudgeSelected(0));
    }
    SECTION("cut, copy and paste: the first copied marker lands on the playhead, the others keep their distances") {
        m.select(0);
        m.select(1, QStringLiteral("toggle"));
        CHECK(m.copySelected() == 2);
        CHECK(m.clipboardCount() == 2);
        CHECK(m.message() == QStringLiteral("Copied 2 markers"));
        c.setPlayhead(f(100));
        const std::size_t steps = s.undo();
        REQUIRE(m.paste());
        CHECK(layout(c) == "m0@10 m1@20 m2@30 m3@40 m4@50 m5@60 m0@100 m1@110");
        CHECK(s.undo() == steps + 1);
        CHECK(c.undoText() == QStringLiteral("Undo Paste Markers"));
        CHECK(selection(m) == std::vector<int>{6, 7}); // the pasted ones are selected
        // Cut m2 and paste it at 150.
        m.select(2);
        REQUIRE(m.cutSelected());
        CHECK(c.undoText() == QStringLiteral("Undo Cut Markers"));
        CHECK(m.clipboardCount() == 1);
        c.setPlayhead(f(150));
        REQUIRE(m.paste());
        CHECK(layout(c) == "m0@10 m1@20 m3@40 m4@50 m5@60 m0@100 m1@110 m2@150");
        // The clipboard survives a change of sequence, as plain values.
        CHECK(m.clipboardCount() == 1);
        m.clearSelection();
        CHECK_FALSE(m.cutSelected());
    }
    SECTION("Clear Selected Marker clears the selected markers; with none selected it clears the one at the playhead") {
        m.select(0);
        m.select(2, QStringLiteral("toggle"));
        REQUIRE(m.runCommand(QStringLiteral("marker.clearSelected")));
        CHECK(layout(c) == "m1@20 m3@40 m4@50 m5@60");
        CHECK(c.undoText() == QStringLiteral("Undo Clear Selected Markers"));
        c.setPlayhead(f(40));
        REQUIRE(m.runCommand(QStringLiteral("marker.clearSelected")));
        CHECK(layout(c) == "m1@20 m4@50 m5@60");
    }
    SECTION("an edit that is not the selection's own drops the selection: its indices would mean something else") {
        m.select(1);
        m.select(2, QStringLiteral("toggle"));
        REQUIRE(m.update(0, {{QStringLiteral("name"), QStringLiteral("first")}}));
        CHECK(m.selectionCount() == 0);
        m.select(1);
        m.setScope(QStringLiteral("clip"));
        CHECK(m.selectionCount() == 0);
        m.setScope(QStringLiteral("sequence"));
    }
}

// ---- 119: the ruler -------------------------------------------------------------------------------------------------------

TEST_CASE("Ruler: flags select with a click, Ctrl-click and Shift-click, a Ctrl-drag marquee, and one dragged flag moves the group (card 119)",
          "[app][qml][markers][group][screenshot]") {
    resetSettings();
    ShellMarkers s({10, 20, 30, 40});
    TimelineView* view = s.view();
    view->setPixelsPerSecond(100);
    view->setScrollX(0);
    spin(100);
    QQuickItem* ruler = itemNamed(s.window(), "timelineRuler");
    const auto flagPoint = [&](int index) {
        QQuickItem* flag = findNamed(ruler, QStringLiteral("rulerMarker_%1").arg(index));
        INFO(index);
        REQUIRE(flag);
        return flag->mapToScene(QPointF(0, flag->height() - 8));
    };

    // A plain click selects the flag and puts the playhead on it.
    click(s.window(), flagPoint(1));
    CHECK(selection(*s.markers) == std::vector<int>{1});
    CHECK(s.controller->playhead() == f(20));
    // Ctrl-click adds a flag (the playhead stays), Shift-click selects from the last plain or Ctrl click.
    click(s.window(), flagPoint(2), Qt::ControlModifier);
    CHECK(selection(*s.markers) == std::vector<int>{1, 2});
    CHECK(s.controller->playhead() == f(20));
    click(s.window(), flagPoint(3), Qt::ShiftModifier);
    CHECK(selection(*s.markers) == std::vector<int>{2, 3});
    click(s.window(), flagPoint(3), Qt::ControlModifier);
    CHECK(selection(*s.markers) == std::vector<int>{2});

    // A Ctrl-drag over the ruler draws a marquee and selects the flags inside it (Ctrl+Shift adds to the selection).
    const QPointF a = flagPoint(0) + QPointF(-25, -6);
    const QPointF b = flagPoint(1) + QPointF(25, -6);
    pointer(s.window(), QEvent::MouseMove, a, Qt::NoButton, Qt::NoButton, Qt::ControlModifier);
    pointer(s.window(), QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
    pointer(s.window(), QEvent::MouseMove, (a + b) / 2, Qt::NoButton, Qt::LeftButton, Qt::ControlModifier);
    pointer(s.window(), QEvent::MouseMove, b, Qt::NoButton, Qt::LeftButton, Qt::ControlModifier);
    spin(30);
    QQuickItem* marquee = findNamed(ruler, QStringLiteral("rulerMarquee"));
    REQUIRE(marquee);
    CHECK(marquee->isVisible());
    CHECK(marquee->width() > 40);
    pointer(s.window(), QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::NoButton, Qt::ControlModifier);
    spin(30);
    CHECK_FALSE(marquee->isVisible());
    CHECK(selection(*s.markers) == std::vector<int>{0, 1});
    CHECK(s.controller->playhead() == f(20)); // a marquee is not a scrub
    const QPointF c3 = flagPoint(3) + QPointF(-14, -6);
    pointer(s.window(), QEvent::MouseMove, c3, Qt::NoButton, Qt::NoButton, Qt::ControlModifier | Qt::ShiftModifier);
    pointer(s.window(), QEvent::MouseButtonPress, c3, Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier | Qt::ShiftModifier);
    pointer(s.window(), QEvent::MouseMove, c3 + QPointF(28, 0), Qt::NoButton, Qt::LeftButton, Qt::ControlModifier | Qt::ShiftModifier);
    pointer(s.window(), QEvent::MouseButtonRelease, c3 + QPointF(28, 0), Qt::LeftButton, Qt::NoButton,
            Qt::ControlModifier | Qt::ShiftModifier);
    spin(30);
    CHECK(selection(*s.markers) == std::vector<int>{0, 1, 3});

    // Selected flags wear an outline, the others do not.
    CHECK(findNamed(findNamed(ruler, QStringLiteral("rulerMarker_0")), QStringLiteral("rulerMarkerSelected"))->isVisible());
    CHECK_FALSE(findNamed(findNamed(ruler, QStringLiteral("rulerMarker_2")), QStringLiteral("rulerMarkerSelected"))->isVisible());
    s.save(s.sceneRect(ruler).adjusted(-8, -4, 8, 12), QStringLiteral("a7-ruler-marker-selection.png"));

    // Dragging one flag of the selection (a second of timeline, 100 px) moves them all by the same distance, one undo step.
    s.markers->select(0);
    s.markers->select(1, QStringLiteral("toggle"));
    const std::size_t steps = s.undo();
    drag(s.window(), flagPoint(1), flagPoint(1) + QPointF(100, 0));
    CHECK(layout(*s.controller) == "m2@30 m0@35 m3@40 m1@45");
    CHECK(s.undo() == steps + 1);
    CHECK(s.controller->undoText() == QStringLiteral("Undo Move Markers"));
    CHECK(selection(*s.markers) == std::vector<int>{1, 3}); // m0 and m1, where they landed
    REQUIRE(s.controller->undo());
    CHECK(layout(*s.controller) == "m0@10 m1@20 m2@30 m3@40");

    // The same on the light theme (with a selection again: the undo dropped it).
    s.markers->select(0);
    s.markers->select(2, QStringLiteral("toggle"));
    s.markers->select(3, QStringLiteral("toggle"));
    s.shell.setAppearance(false, "ultraviolet");
    spin(150);
    CHECK(selection(*s.markers) == std::vector<int>{0, 2, 3});
    s.save(s.sceneRect(ruler).adjusted(-8, -4, 8, 12), QStringLiteral("a7-ruler-marker-selection-light.png"));
}

// ---- 119: the Markers panel -----------------------------------------------------------------------------------------------

TEST_CASE("Markers panel: Ctrl-click and Shift-click rows, the group keys and the bar select and act on several markers (card 119)",
          "[app][qml][markers][group][screenshot]") {
    resetSettings();
    ShellMarkers s({10, 20, 30, 40, 50});
    REQUIRE(QMetaObject::invokeMethod(s.window(), "showBrowserPanel", Q_ARG(QVariant, QStringLiteral("markers"))));
    spin(200);
    QQuickItem* panel = itemNamed(s.window(), "markersPanel");
    const auto row = [&](int index) { return itemNamed(s.window(), QStringLiteral("markerRow_%1").arg(index).toLatin1().constData()); };

    // A plain click selects the row and moves the playhead; Ctrl-click adds; Shift-click selects the rows between.
    clickItem(s.window(), row(0));
    CHECK(selection(*s.markers) == std::vector<int>{0});
    CHECK(s.controller->playhead() == f(10));
    clickItem(s.window(), row(2), Qt::ControlModifier);
    CHECK(selection(*s.markers) == std::vector<int>{0, 2});
    CHECK(s.controller->playhead() == f(10)); // a Ctrl-click does not seek
    clickItem(s.window(), row(4), Qt::ShiftModifier);
    CHECK(selection(*s.markers) == std::vector<int>{2, 3, 4});
    clickItem(s.window(), row(3), Qt::ControlModifier);
    CHECK(selection(*s.markers) == std::vector<int>{2, 4});

    // The bar under the filters counts the selection and has the actions.
    QQuickItem* bar = itemNamed(s.window(), "markersGroupBar");
    CHECK(bar->isVisible());
    CHECK(itemNamed(s.window(), "markersSelectionCount")->property("text").toString() == QStringLiteral("2 markers selected"));
    s.shell.setAppearance(true, "ultraviolet");
    spin(100);
    s.save(s.sceneRect(panel).adjusted(-6, -6, 6, 6), QStringLiteral("a7-markers-panel-selection.png"));

    // Ctrl+A selects every listed row, Escape drops the selection (the focused row takes the keys).
    clickItem(s.window(), row(1));
    row(1)->forceActiveFocus();
    key(s.window(), Qt::Key_A, Qt::ControlModifier, QStringLiteral("a"));
    CHECK(s.markers->selectionCount() == 5);
    key(s.window(), Qt::Key_Escape);
    CHECK(s.markers->selectionCount() == 0);

    // Ctrl+C then Ctrl+V on the playhead; Delete removes the selected rows together.
    clickItem(s.window(), row(0));
    clickItem(s.window(), row(1), Qt::ControlModifier);
    row(1)->forceActiveFocus();
    key(s.window(), Qt::Key_C, Qt::ControlModifier, QStringLiteral("c"));
    CHECK(s.markers->clipboardCount() == 2);
    s.controller->setPlayhead(f(200));
    row(1)->forceActiveFocus();
    key(s.window(), Qt::Key_V, Qt::ControlModifier, QStringLiteral("v"));
    CHECK(layout(*s.controller) == "m0@10 m1@20 m2@30 m3@40 m4@50 m0@200 m1@210");
    CHECK(selection(*s.markers) == std::vector<int>{5, 6});
    const std::size_t steps = s.undo();
    // The list only builds the rows in view: scroll to the pasted ones.
    REQUIRE(QMetaObject::invokeMethod(itemNamed(s.window(), "markersList"), "positionViewAtEnd"));
    spin(100);
    clickItem(s.window(), row(5));
    clickItem(s.window(), row(6), Qt::ControlModifier);
    row(6)->forceActiveFocus();
    key(s.window(), Qt::Key_Delete);
    CHECK(layout(*s.controller) == "m0@10 m1@20 m2@30 m3@40 m4@50");
    CHECK(s.undo() == steps + 1);

    // The bar's buttons: select two, Delete.
    s.markers->select(0);
    s.markers->select(1, QStringLiteral("toggle"));
    clickItem(s.window(), itemNamed(s.window(), "markersDeleteSelectedButton"));
    CHECK(layout(*s.controller) == "m2@30 m3@40 m4@50");
    CHECK_FALSE(itemNamed(s.window(), "markersCopyButton")->isEnabled());

    // The same panel on the light theme.
    s.markers->select(0);
    s.markers->select(2, QStringLiteral("toggle"));
    s.shell.setAppearance(false, "ultraviolet");
    spin(150);
    s.save(s.sceneRect(panel).adjusted(-6, -6, 6, 6), QStringLiteral("a7-markers-panel-selection-light.png"));
}

// ---- 125: notes on clips --------------------------------------------------------------------------------------------------

TEST_CASE("Clip notes: written through the controller, printed on the clip, in its tooltip, one undo step (card 125)", "[app][note]") {
    Rig rig("V1 |" + stripe('A', 20) + stripe('B', 3) + "|\nA1 |" + stripe('A', 20) + stripe('B', 3) + "|");
    ProjectController& c = rig.controller;
    const ClipId a = rig.tl.clip("V1@0");
    const ClipId b = rig.tl.clip("V1@20");
    const QString keyA = clipKey(a);
    const QString keyB = clipKey(b);
    TimelineView* view = rig.view;

    CHECK(c.clipNote(keyA).isEmpty());
    const std::size_t steps = c.session().undoCount();
    c.setClipNote(keyA, QStringLiteral("  Cut the cough at the second breath, then bring the music up  "));
    CHECK(c.clipNote(keyA) == QStringLiteral("Cut the cough at the second breath, then bring the music up")); // trimmed
    CHECK(c.session().undoCount() == steps + 1);
    CHECK(c.undoText() == QStringLiteral("Undo Clip Note"));
    c.setClipNote(keyB, QStringLiteral("short"));
    settle(rig);

    // A short clip carries the first line of its note after its name; the three-frame clip has no room for a label at all.
    const QString trackV1 = trackKey(rig.tl.track("V1"));
    view->setTrackHeight(trackV1, 40);
    settle(rig);
    CHECK(view->stats().labelTexts.value(a.value).contains(QStringLiteral("Cut the cough"))); // the note follows the name (elided to the clip)
    CHECK_FALSE(view->stats().clipNotes.contains(a.value)); // no plate: there is no room under the name
    CHECK_FALSE(view->stats().labelTexts.value(b.value).contains(QStringLiteral("short")));
    // A tall clip prints up to three lines on a plate at its foot instead, and not after its name.
    view->setTrackHeight(trackV1, 160);
    settle(rig);
    const auto& notes = view->stats().clipNotes;
    REQUIRE(notes.contains(a.value));
    CHECK(notes.value(a.value).startsWith(QStringLiteral("Cut the cough")));
    CHECK_FALSE(view->stats().labelTexts.value(a.value).contains(QStringLiteral("Cut the cough")));
    CHECK_FALSE(notes.contains(b.value)); // too narrow
    // Off with the display option, in both places.
    view->setShowClipNotes(false);
    settle(rig);
    CHECK_FALSE(view->stats().clipNotes.contains(a.value));
    view->setTrackHeight(trackV1, 40);
    settle(rig);
    CHECK_FALSE(view->stats().labelTexts.value(a.value).contains(QStringLiteral("Cut the cough")));
    view->setShowClipNotes(true);
    view->setTrackHeight(trackV1, 160);
    settle(rig);
    CHECK(view->stats().clipNotes.contains(a.value));

    // The tooltip carries the whole note.
    const QVariantMap info = view->clipHoverInfo(keyA);
    CHECK(info.value(QStringLiteral("text")).toString().contains(QStringLiteral("Note: Cut the cough at the second breath, then bring the music up")));
    CHECK(info.value(QStringLiteral("note")).toString().startsWith(QStringLiteral("Cut the cough")));

    // Too long a note is refused with the reason; an empty one clears.
    const QString tooLong(c.clipNoteLimit() + 1, QLatin1Char('n'));
    CHECK(c.clipNoteBytes(tooLong) == c.clipNoteLimit() + 1);
    const std::size_t before = c.session().undoCount();
    c.setClipNote(keyA, tooLong);
    CHECK(c.session().undoCount() == before);
    c.setClipNote(keyA, QString());
    CHECK(c.clipNote(keyA).isEmpty());
    CHECK(c.session().undoCount() == before + 1);
}

TEST_CASE("Clip notes: Clip Note... in the clip menu opens the note popover; Save writes it and the timeline prints it (card 125)",
          "[app][qml][note][screenshot]") {
    resetSettings();
    ShellMarkers s({}, "V1 tgt |" + stripe('A', 150) + stripe('B', 100) + "|\nA1 tgt |" + stripe('A', 150) + stripe('B', 100) + "|");
    TimelineView* view = s.view();
    view->setPixelsPerSecond(100);
    view->setScrollX(0);
    const ClipId a = s.tl.clip("V1@0");
    s.controller->selectClip(clipKey(a));
    spin(150);

    QObject* menu = named(s.window(), "clipContextMenu");
    QObject* popover = named(s.window(), "clipNotePopover");
    CHECK_FALSE(popover->property("visible").toBool());
    // Choosing the menu item asks the panel for the popover at the pointer.
    REQUIRE(QMetaObject::invokeMethod(menu, "noteRequested", Q_ARG(QString, clipKey(a)), Q_ARG(QPointF, QPointF(300, 400))));
    REQUIRE(waitFor([&] { return popover->property("opened").toBool(); }, 5000));
    QObject* area = popover->findChild<QObject*>(QStringLiteral("clipNoteArea"));
    REQUIRE(area);
    CHECK(popover->property("clipId").toString() == clipKey(a));
    area->setProperty("text", QStringLiteral("Open on the harbour, hold for the line about the storm."));
    spin(50);
    QObject* save = popover->findChild<QObject*>(QStringLiteral("clipNoteSave"));
    QObject* clear = popover->findChild<QObject*>(QStringLiteral("clipNoteClear"));
    REQUIRE(save);
    REQUIRE(clear);
    CHECK(save->property("enabled").toBool());
    CHECK_FALSE(clear->property("enabled").toBool()); // nothing to clear yet
    auto* background = popover->property("background").value<QQuickItem*>();
    REQUIRE(background);
    s.shell.setAppearance(true, "ultraviolet");
    spin(150);
    s.save(s.sceneRect(background).adjusted(-12, -12, 12, 12), QStringLiteral("a7-clip-note-popover.png"));
    s.shell.setAppearance(false, "ultraviolet");
    spin(150);
    s.save(s.sceneRect(background).adjusted(-12, -12, 12, 12), QStringLiteral("a7-clip-note-popover-light.png"));
    s.shell.setAppearance(true, "ultraviolet");
    spin(100);
    const std::size_t steps = s.undo();
    REQUIRE(QMetaObject::invokeMethod(save, "clicked"));
    spin(100);
    CHECK(s.controller->clipNote(clipKey(a)) == QStringLiteral("Open on the harbour, hold for the line about the storm."));
    CHECK(s.undo() == steps + 1);
    REQUIRE(waitFor([&] { return !popover->property("visible").toBool(); }, 5000));
    // On a track of the usual height the note follows the clip's name.
    REQUIRE(waitFor([&] {
        REQUIRE_FALSE(s.shell.grab().isNull()); // the lanes are drawn when a frame is rendered
        return view->stats().labelTexts.value(a.value).contains(QStringLiteral("Open on the harbour"));
    }, 5000));
    QQuickItem* lanes = itemNamed(s.window(), "timelineView");
    s.save(s.sceneRect(lanes).adjusted(0, -40, 0, 0), QStringLiteral("a7-clip-note-on-clip.png"));
    s.shell.setAppearance(false, "ultraviolet");
    spin(150);
    s.save(s.sceneRect(lanes).adjusted(0, -40, 0, 0), QStringLiteral("a7-clip-note-on-clip-light.png"));
    // On a tall track it is a plate of up to three lines at the clip's foot.
    view->setTrackHeight(trackKey(s.tl.track("V1")), 150);
    REQUIRE(waitFor([&] {
        REQUIRE_FALSE(s.shell.grab().isNull());
        return view->stats().clipNotes.contains(a.value);
    }, 5000));
    s.save(s.sceneRect(lanes).adjusted(0, -40, 0, 0), QStringLiteral("a7-clip-note-plate-light.png"));
    s.shell.setAppearance(true, "ultraviolet");
    spin(150);
    s.save(s.sceneRect(lanes).adjusted(0, -40, 0, 0), QStringLiteral("a7-clip-note-plate.png"));
    view->setTrackHeight(trackKey(s.tl.track("V1")), 40);

    // Clear removes it; Ctrl+Enter saves.
    REQUIRE(QMetaObject::invokeMethod(menu, "noteRequested", Q_ARG(QString, clipKey(a)), Q_ARG(QPointF, QPointF(300, 400))));
    REQUIRE(waitFor([&] { return popover->property("opened").toBool(); }, 5000));
    CHECK(area->property("text").toString().startsWith(QStringLiteral("Open on the harbour")));
    REQUIRE(clear->property("enabled").toBool());
    REQUIRE(QMetaObject::invokeMethod(clear, "clicked"));
    spin(100);
    CHECK(s.controller->clipNote(clipKey(a)).isEmpty());
}

// ---- 120: pinned keyframes in Effect Controls ----------------------------------------------------------------------------

TEST_CASE("Effect Controls: Pin to Clip Start moves the selected key to the clip's start and a head trim carries it (card 120)",
          "[app][keyframes][pin]") {
    ProjectController controller;
    EffectControlsModel& fx = *controller.effectControls();
    const ClipId city = controller.primaryClip();
    const Clip clip = controller.currentSequence()->findClip(city)->clip();
    const Ticks frame = controller.frameTicks();
    const QString opacity = QStringLiteral("p:opacity");
    const auto times = [&] {
        std::vector<Ticks> out;
        for (const Keyframe& k : controller.currentSequence()->findClip(city)->clip().properties()[ParamId::Opacity].curve()->keys)
            out.push_back(k.time);
        return out;
    };
    const auto keyAtIndex = [&](int i) { return controller.currentSequence()->findClip(city)->clip().properties()[ParamId::Opacity].curve()->keys[static_cast<std::size_t>(i)]; };
    controller.setPlayhead(clip.start() + frame * 12);
    fx.setAnimated(opacity, true);
    controller.setPlayhead(clip.start() + frame * 60);
    fx.setValue(opacity, 40.0);
    fx.endEdit();
    REQUIRE(times().size() == 2);

    // Select the first key (frame 12 of the clip) and pin it to the clip's start: it moves there, in one undo step.
    const QVariantList keys = fx.row(opacity).value(QStringLiteral("keys")).toList();
    REQUIRE(fx.pressKey(opacity, keys.value(0).toMap().value(QStringLiteral("x")).toDouble(), 0.01, 0));
    CHECK(fx.selectedKeyPin() == QStringLiteral("none")); // all the selected keys share it
    const double pinnedValue = keyAtIndex(0).value;
    const std::size_t steps = controller.session().undoCount();
    fx.setKeyPin(QStringLiteral("start"));
    CHECK(controller.session().undoCount() == steps + 1);
    CHECK(controller.undoText() == QStringLiteral("Undo Pin Keyframes"));
    CHECK(times().front() == clip.startCurveTime());
    CHECK(keyAtIndex(0).pin == KeyPin::ClipStart);
    CHECK(fx.selectedKeyPin() == QStringLiteral("start"));
    CHECK(fx.selectedKeyCount() == 1); // the key stays selected where it moved

    // Trim the clip's head 10 frames: the pinned key goes with the new first frame.
    const SequenceId seq = controller.currentSequenceId();
    REQUIRE(controller.applyEditFn("Trim", [&](const ProjectState& st) { return edit::trimEdge(st, seq, city, edit::Edge::Head, frame * 10); }));
    const Clip trimmed = controller.currentSequence()->findClip(city)->clip();
    CHECK(trimmed.startCurveTime() == clip.startCurveTime() + frame * 10);
    CHECK(times().front() == trimmed.startCurveTime());
    CHECK(keyAtIndex(0).value == pinnedValue); // its value is kept
    CHECK(times().size() == 2);
    // One undo step took both: the trim and the key's move.
    REQUIRE(controller.undo());
    CHECK(times().front() == clip.startCurveTime());

    // Unpin leaves the key where it is.
    REQUIRE(fx.pressKey(opacity, fx.row(opacity).value(QStringLiteral("keys")).toList().value(0).toMap().value(QStringLiteral("x")).toDouble(), 0.01, 0));
    fx.setKeyPin(QStringLiteral("none"));
    CHECK(keyAtIndex(0).pin == KeyPin::None);
    CHECK(times().front() == clip.startCurveTime());
}

// ---- 120: the Sequence menu's toggle -------------------------------------------------------------------------------------------

TEST_CASE("Sequence > Ripple Track Keyframes toggles the sequence's setting in one undo step (card 120)", "[app][qml][keyframes][ripple]") {
    resetSettings();
    ShellMarkers s({}, "V1 tgt |" + stripe('A', 100) + "|\nA1 tgt |" + stripe('A', 100) + "|");
    ProjectController& c = *s.controller;
    CHECK_FALSE(c.rippleTrackKeyframes());
    const std::size_t steps = s.undo();
    c.setRippleTrackKeyframes(true);
    CHECK(c.rippleTrackKeyframes());
    CHECK(s.undo() == steps + 1);
    CHECK(c.undoText() == QStringLiteral("Undo Ripple Track Keyframes"));
    c.setRippleTrackKeyframes(true); // already on: no empty step
    CHECK(s.undo() == steps + 1);
    REQUIRE(c.undo());
    CHECK_FALSE(c.rippleTrackKeyframes());
}

// ---- 180: the pacing strip -------------------------------------------------------------------------------------------------

TEST_CASE("Pacing strip: a long shot grades as a stretch that drags, measuring never calls an unmeasured file silent (card 180)",
          "[app][pacing]") {
    resetSettings();
    // 60 s with no cut, then a second 4 s clip; nothing measured (the test measures nothing: every file fails).
    ShellMarkers s({}, "V1 tgt |" + stripe('A', 1500) + stripe('B', 100) + "|\nA1 tgt |" + stripe('A', 1500) + stripe('B', 100) + "|",
                   {{'A', 3000}});
    auto* pacing = s.shell.engine.singletonInstance<PacingBridge*>("Wabcut", "Pacing");
    REQUIRE(pacing);
    std::atomic<int> asked{0};
    pacing->setMeasure([&](const ProjectState&, MediaId, const std::function<bool(double)>&) -> std::expected<MediaActivity, Error> {
        ++asked;
        return std::unexpected(Error{ErrorCode::NotFound, "offline"});
    });
    CHECK_FALSE(pacing->active());
    pacing->setActive(true);
    pacing->refreshNow();
    REQUIRE_FALSE(pacing->stretches().isEmpty());
    const QVariantMap first = pacing->stretches().first().toMap();
    CHECK(first.value(QStringLiteral("level")).toInt() == 3);
    CHECK(first.value(QStringLiteral("startSec")).toDouble() == 0.0);
    CHECK(first.value(QStringLiteral("endSec")).toDouble() == 60.0);
    CHECK(first.value(QStringLiteral("reasons")).toStringList() == QStringList{QStringLiteral("longShot")});
    CHECK(first.value(QStringLiteral("text")).toString().contains(QStringLiteral("The same shot runs 60 s")));
    CHECK(first.value(QStringLiteral("text")).toString().contains(QStringLiteral("Try a cut, a cutaway or B-roll here")));
    CHECK(pacing->stretchAt(30.0) == 0);
    CHECK(pacing->stretchAt(61.0) == -1);
    // The files were asked about once each, failed, and stay unknown: no silence or stillness is claimed for them.
    REQUIRE(waitFor([&] { return !pacing->busy() && asked >= 1; }, 10000));
    spin(100);
    CHECK(pacing->status().contains(QStringLiteral("could not be measured")));
    for (const QVariant& v : pacing->stretches()) {
        const QStringList reasons = v.toMap().value(QStringLiteral("reasons")).toStringList();
        CHECK_FALSE(reasons.contains(QStringLiteral("silence")));
        CHECK_FALSE(reasons.contains(QStringLiteral("still")));
    }
    const int asks = asked;
    pacing->refreshNow();
    spin(100);
    CHECK(asked.load() == asks); // a file that failed is not asked again

    // Click the stretch: In and Out go around it, the playhead to its start; one undo step each action.
    const std::size_t steps = s.undo();
    REQUIRE(pacing->markStretch(0));
    CHECK(s.undo() == steps + 1);
    const auto marked = s.controller->currentSequence()->markedRange();
    REQUIRE(marked.has_value());
    CHECK(marked->start == Ticks{});
    CHECK(marked->end() == f(1500));
    REQUIRE(pacing->addMarker(0));
    CHECK(s.controller->currentSequence()->markers.size() == 1);
    CHECK(s.controller->currentSequence()->markers.front().name == "Likely to drag");
    REQUIRE(pacing->selectStretch(0));
    CHECK(s.controller->selection().size() >= 2);
    pacing->setActive(false);
}

TEST_CASE("Pacing strip: measured loudness and motion grade a silent, still stretch; the strip draws in both themes (card 180)",
          "[app][qml][pacing][screenshot]") {
    resetSettings();
    // A 20 s clip cut every 2 s with a silent, still middle: only the measurements can tell.
    std::string picture = "V1 tgt |";
    for (int i = 0; i < 10; ++i)
        picture += stripe(static_cast<char>('A' + i % 2), 50);
    picture += "|\nA1 tgt |" + stripe('A', 500) + "|";
    ShellMarkers s({}, picture);
    auto* pacing = s.shell.engine.singletonInstance<PacingBridge*>("Wabcut", "Pacing");
    REQUIRE(pacing);
    pacing->setMeasure([](const ProjectState&, MediaId, const std::function<bool(double)>&) -> std::expected<MediaActivity, Error> {
        MediaActivity activity;
        activity.step = Ticks{kTicksPerSecond / 10};
        for (int i = 0; i < 400; ++i) { // 40 s of media: sound only in the first 6 s
            activity.audio.push_back(i < 60 ? 0.7F : 0.0F);
            activity.motion.push_back(i < 60 ? 0.6F : 0.0F);
        }
        return activity;
    });
    s.shell.preferences->setProperty("timelineShowPacing", true);
    spin(300);
    REQUIRE(waitFor([&] { return pacing->active(); }, 5000));
    pacing->refreshNow();
    REQUIRE(waitFor([&] {
        for (const QVariant& v : pacing->stretches()) {
            if (v.toMap().value(QStringLiteral("reasons")).toStringList().contains(QStringLiteral("silence")))
                return true;
        }
        return false;
    }, 15000));
    QVariantMap silent;
    for (const QVariant& v : pacing->stretches()) {
        if (v.toMap().value(QStringLiteral("reasons")).toStringList().contains(QStringLiteral("silence")))
            silent = v.toMap();
    }
    CHECK(silent.value(QStringLiteral("level")).toInt() >= 2);
    CHECK(silent.value(QStringLiteral("text")).toString().contains(QStringLiteral("Silent for")));
    CHECK(silent.value(QStringLiteral("text")).toString().contains(QStringLiteral("Tighten the pause")));

    // The strip sits under the ruler, above the tracks, and shows the stretches.
    TimelineView* view = s.view();
    view->setPixelsPerSecond(40);
    view->setScrollX(0);
    spin(200);
    QQuickItem* strip = itemNamed(s.window(), "timelinePacingStrip");
    CHECK(strip->isVisible());
    QQuickItem* ruler = itemNamed(s.window(), "timelineRuler");
    CHECK(strip->mapToScene(QPointF(0, 0)).y() >= ruler->mapToScene(QPointF(0, ruler->height())).y());
    QQuickItem* block = findNamed(strip, QStringLiteral("pacingStretch_%1").arg(silent.value(QStringLiteral("index")).toInt()));
    REQUIRE(block);
    CHECK(block->isVisible());
    CHECK(block->width() > 20);
    s.shell.setAppearance(true, "ultraviolet");
    spin(100);
    s.save(s.sceneRect(strip).adjusted(-10, -34, 10, 60), QStringLiteral("a7-pacing-strip.png"));
    s.shell.setAppearance(false, "ultraviolet");
    spin(150);
    s.save(s.sceneRect(strip).adjusted(-10, -34, 10, 60), QStringLiteral("a7-pacing-strip-light.png"));

    // Turned off, the strip is gone and the analysis stops.
    s.shell.preferences->setProperty("timelineShowPacing", false);
    spin(200);
    CHECK_FALSE(strip->isVisible());
    CHECK_FALSE(pacing->active());
}
