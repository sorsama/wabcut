// 0.7, batch A6: the timeline gestures and menus of card 040 (Alt-drag, then Ctrl before the drop, inserts the copy), card 041
// (a click on a clip's fx badge opens the Show Clip Keyframes menu), card 064 (Lock Clip), card 067 (dragging a track header
// reorders tracks) and card 097 (Match Frame into the sequence a nest plays). C++ rigs (TimelineHarness) for the pointer
// paths of the lanes, the real shell (Main.qml, hidden) for the QML. Screenshots under test-output/ui (a6-*.png).

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "BridgeIds.h"
#include "LibraryBridge.h"
#include "NestingBridge.h"
#include "ProjectController.h"
#include "SequenceTabsBridge.h"
#include "SourceMonitorBridge.h"
#include "TimelineView.h"

#include <wabcut/model/ClipLock.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickItem>

#include <array>
#include <string>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

void settle(Rig& rig) {
    for (int i = 0; i < 4; ++i) {
        QCoreApplication::processEvents();
        rig.harness.frame();
    }
}

std::string run(char letter, int frames) {
    return std::string(1, letter) + std::string(static_cast<std::size_t>(frames - 1), static_cast<char>(letter + 32));
}
std::string gap(int frames) { return std::string(static_cast<std::size_t>(frames), '.'); }

/// Press at `from` holding `pressMods`, move to `to` in six steps holding `phases[0]` for the first two, `phases[1]` for the
/// next two and `phases[2]` for the last two; optionally release there holding `phases[2]`.
void dragPhases(Rig& rig, QPointF from, QPointF to, Qt::KeyboardModifiers pressMods,
                const std::array<Qt::KeyboardModifiers, 3>& phases, bool release = true) {
    rig.press(from, pressMods);
    for (int i = 1; i <= 6; ++i)
        rig.move(from + (to - from) * (i / 6.0), phases[static_cast<std::size_t>((i - 1) / 2)]);
    if (release)
        rig.release(to, phases[2]);
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

/// The track header (the TrackHeader root) a part of it belongs to: the nearest parent that has a track id.
QQuickItem* headerOf(QQuickItem* part) {
    for (QQuickItem* item = part; item; item = item->parentItem()) {
        if (item->property("trackId").isValid())
            return item;
    }
    return nullptr;
}

QRectF sceneRect(const QQuickItem* item) {
    return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
}

void mouseAt(QQuickWindow* window, QEvent::Type type, QPointF scene, Qt::MouseButton button, Qt::MouseButtons buttons,
             Qt::KeyboardModifiers mods = {}) {
    QMouseEvent event(type, scene, scene, button, buttons, mods);
    QCoreApplication::sendEvent(window, &event);
}

/// The real shell (Main.qml, hidden, reduced motion) over a project from a timeline picture.
struct ShellProject {
    Shell shell;
    ProjectController* controller = nullptr;
    Timeline tl;

    explicit ShellProject(std::string_view picture, bool link = true) : tl(picture) {
        REQUIRE(shell.window);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        shell.setAppearance(true, "ultraviolet");
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        REQUIRE(controller);
        if (link)
            tl.linkMatching();
        controller->resetProject(tl.state, tl.seq);
        spin(200);
    }
    QQuickItem* item(const QString& name) const { return findNamed(shell.window->contentItem(), name); }
    QObject* object(const QString& name) const { return shell.window->findChild<QObject*>(name); }
    TimelineView* view() const {
        auto* v = qobject_cast<TimelineView*>(item(QStringLiteral("timelineView")));
        REQUIRE(v);
        return v;
    }
    const Sequence& sequence() const { return *controller->currentSequence(); }
    std::size_t undo() const { return controller->session().undoCount(); }
    void settle() const {
        REQUIRE_FALSE(shell.grab().isNull());
        spin(30);
    }
    void shot(const QRectF& rect, const QString& file) const {
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        const double dpr = image.devicePixelRatio();
        const QRectF r = rect.intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
        REQUIRE(image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect()).save(QDir(outputDir()).filePath(file)));
    }
};

} // namespace

// ---- Card 040: Alt-drag, then Ctrl, inserts the copy --------------------------------------------------------------------

TEST_CASE("Alt-drag then Ctrl before the drop inserts the copy; Ctrl first keeps meaning rearrange (card 040)",
          "[app][timeline][edit][duplicate][insert]") {
    const Qt::KeyboardModifiers none;
    const Qt::KeyboardModifiers alt = Qt::AltModifier;
    const Qt::KeyboardModifiers ctrl = Qt::ControlModifier;
    const Qt::KeyboardModifiers altCtrl = Qt::AltModifier | Qt::ControlModifier;
    Rig rig(R"(
        V1 |AaaaBbbbCccc....|
    )", 10.0, false);
    const ClipId a = rig.clip("V1@0");
    const ClipId sel[] = {a};
    const QPointF from{rig.x(1.5), rig.y("V1")};
    const QPointF to{rig.x(9.5), rig.y("V1")};
    const std::string insertedCopy = "V1 |AaaaBbbbAaaaCccc|";
    const auto expectCopy = [&](edit::MoveMode mode) {
        return rig.expected(edit::duplicateClips(rig.tl.state, rig.tl.seq, sel, f(8), {}, mode, edit::LinkMode::IgnoreLinks));
    };

    SECTION("Alt from the press, Ctrl added before the drop") {
        dragPhases(rig, from, to, alt, {alt, alt, altCtrl}, false);
        REQUIRE(rig.view->gestureActive());
        CHECK(rig.view->cursorKey() == QStringLiteral("insertCopy"));
        CHECK(rig.view->gestureReadout().startsWith(QStringLiteral("Insert Copy ")));
        CHECK(rig.undoSteps() == 0);
        rig.release(to, altCtrl);
        CHECK(rig.picture() == expectCopy(edit::MoveMode::Insert));
        CHECK(rig.picture() == normalize(insertedCopy));
        CHECK(rig.undoSteps() == 1);
        CHECK(rig.controller.undoText() == QStringLiteral("Undo Insert Duplicate"));
        CHECK(rig.clip("V1@0") == a); // the original keeps its id and place
        REQUIRE(rig.controller.selection().size() == 1); // the copy is the selection
        CHECK(rig.controller.selection().front() == rig.clip("V1@8"));
        REQUIRE(rig.controller.undo());
        CHECK(rig.picture() == normalize(rig.original));
    }
    SECTION("pressed plainly, Alt added and then Ctrl: still an insert copy") {
        dragPhases(rig, from, to, none, {none, alt, altCtrl});
        CHECK(rig.picture() == normalize(insertedCopy));
        CHECK(rig.controller.undoText() == QStringLiteral("Undo Insert Duplicate"));
    }
    SECTION("Ctrl first, then Alt: a rearrange, as ever") {
        dragPhases(rig, from, to, ctrl, {ctrl, ctrl, altCtrl}, false);
        REQUIRE(rig.view->gestureActive());
        CHECK(rig.view->cursorKey() == QStringLiteral("rearrange"));
        CHECK(rig.view->gestureReadout().startsWith(QStringLiteral("Rearrange ")));
        rig.release(to, altCtrl);
        CHECK(rig.picture() == rig.expected(edit::moveClips(rig.tl.state, rig.tl.seq, sel, f(8), {}, edit::MoveMode::Rearrange)));
        CHECK(rig.controller.undoText() == QStringLiteral("Undo Rearrange"));
    }
    SECTION("Ctrl and Alt from the same moment, or from the press, rearrange") {
        dragPhases(rig, from, to, none, {none, none, altCtrl});
        CHECK(rig.controller.undoText() == QStringLiteral("Undo Rearrange"));
        REQUIRE(rig.controller.undo());
        dragPhases(rig, from, to, altCtrl, {altCtrl, altCtrl, altCtrl});
        CHECK(rig.controller.undoText() == QStringLiteral("Undo Rearrange"));
    }
    SECTION("Ctrl let go again before the drop leaves a plain copy") {
        dragPhases(rig, from, to, alt, {alt, altCtrl, alt});
        CHECK(rig.picture() == rig.expected(edit::duplicateClips(rig.tl.state, rig.tl.seq, sel, f(8), {}, edit::MoveMode::Overwrite,
                                                                  edit::LinkMode::IgnoreLinks)));
        CHECK(rig.controller.undoText() == QStringLiteral("Undo Duplicate"));
    }
    SECTION("Alt let go while Ctrl stays: an insert move") {
        dragPhases(rig, from, to, alt, {alt, altCtrl, ctrl});
        CHECK(rig.picture() == rig.expected(edit::moveClips(rig.tl.state, rig.tl.seq, sel, f(8), {}, edit::MoveMode::Insert,
                                                            edit::LinkMode::IgnoreLinks)));
        CHECK(rig.controller.undoText() == QStringLiteral("Undo Insert Move"));
    }
    SECTION("the key press alone changes the cursor and the preview, without the pointer moving") {
        dragPhases(rig, from, to, alt, {alt, alt, alt}, false);
        REQUIRE(rig.view->gestureActive());
        CHECK(rig.view->cursorKey() == QStringLiteral("copy"));
        CHECK(rig.view->gestureReadout().startsWith(QStringLiteral("Copy ")));
        // Like a platform's, a synthetic key event carries the modifiers from before it (QKeyEvent::modifiers() adds the key's).
        QKeyEvent down(QEvent::KeyPress, Qt::Key_Control, alt);
        QCoreApplication::sendEvent(&rig.harness.window(), &down);
        CHECK(rig.view->cursorKey() == QStringLiteral("insertCopy"));
        CHECK(rig.view->gestureReadout().startsWith(QStringLiteral("Insert Copy ")));
        QKeyEvent up(QEvent::KeyRelease, Qt::Key_Control, altCtrl);
        QCoreApplication::sendEvent(&rig.harness.window(), &up);
        CHECK(rig.view->cursorKey() == QStringLiteral("copy"));
        rig.release(to, alt);
        CHECK(rig.controller.undoText() == QStringLiteral("Undo Duplicate"));
    }
}

// ---- Card 064: Lock Clip ------------------------------------------------------------------------------------------------

TEST_CASE("A locked clip does not drag, trim or get overwritten through the lanes, and wears a padlock (card 064)",
          "[app][timeline][clip-lock]") {
    Rig rig(R"(
        V1 |AaaaBbbbCccc....|
    )", 20.0, false);
    const ClipId a = rig.clip("V1@0");
    const ClipId b = rig.clip("V1@4");
    rig.controller.selectClipById(b, ProjectController::Replace, false);
    CHECK_FALSE(rig.controller.selectionLocked());
    rig.controller.toggleLockSelection();
    CHECK(rig.controller.selectionLocked());
    CHECK(rig.controller.undoText() == QStringLiteral("Undo Lock Clip"));
    settle(rig);
    const std::string original = rig.picture();
    const std::size_t steps = rig.undoSteps();
    const double y = rig.y("V1");
    const QString why = QStringLiteral("Clip 'B' is locked");

    SECTION("the padlock sits inside the locked clip only") {
        const auto& badges = rig.view->stats().lockBadges;
        REQUIRE(badges.contains(b.value));
        CHECK_FALSE(badges.contains(a.value));
        const QRectF badge = badges.value(b.value);
        CHECK(badge.left() >= rig.x(4));
        CHECK(badge.right() <= rig.x(8));
        CHECK(badge.top() >= 0);
        CHECK(badge.bottom() <= rig.view->height());
    }
    SECTION("dragging it is refused, says so, and the drop changes nothing") {
        rig.drag({rig.x(5.5), y}, {rig.x(9.5), y}, {}, false);
        REQUIRE(rig.view->gestureActive());
        CHECK(rig.view->gestureReadout() == why);
        rig.release({rig.x(9.5), y});
        CHECK(rig.picture() == original);
        CHECK(rig.undoSteps() == steps);
    }
    SECTION("its edges do not trim") {
        rig.drag({rig.x(8) - 3, y}, {rig.x(8) - 43, y}, {}, false);
        REQUIRE(rig.view->gestureActive());
        CHECK(rig.view->gestureReadout() == why);
        rig.release({rig.x(8) - 43, y});
        CHECK(rig.picture() == original);
        CHECK(rig.undoSteps() == steps);
    }
    SECTION("a neighbour dragged onto it is refused; dragged away it moves") {
        rig.drag({rig.x(11.5), y}, {rig.x(7.5), y}, {}, false);
        REQUIRE(rig.view->gestureActive());
        CHECK(rig.view->gestureReadout() == why);
        rig.release({rig.x(7.5), y});
        CHECK(rig.picture() == original);
        CHECK(rig.undoSteps() == steps);
        rig.drag({rig.x(11.5), y}, {rig.x(13.5), y});
        CHECK(rig.picture() == normalize("V1 |AaaaBbbb..Cccc..|"));
        CHECK(rig.undoSteps() == steps + 1);
    }
    SECTION("an Alt-drag copy of it is allowed and the copy is not locked") {
        rig.drag({rig.x(5.5), y}, {rig.x(13.5), y}, Qt::AltModifier);
        CHECK(rig.picture() == normalize("V1 |AaaaBbbbCcccBbbb|"));
        REQUIRE(rig.controller.selection().size() == 1);
        CHECK(rig.controller.selection().front() == rig.clip("V1@12"));
        CHECK_FALSE(rig.controller.currentSequence()->findClip(rig.clip("V1@12"))->clip().locked);
        CHECK(rig.controller.currentSequence()->findClip(b)->clip().locked);
    }
    SECTION("the razor skips it, Delete and Ripple Delete refuse it, with the reason") {
        CHECK(rig.controller.razorAt(rig.tl.track("V1"), f(6), false));
        CHECK(rig.picture() == original); // nothing was cut
        rig.controller.selectClipById(b, ProjectController::Replace, false);
        rig.controller.deleteSelection();
        CHECK(rig.controller.lastError() == QStringLiteral("clip 'B' is locked"));
        rig.controller.rippleDeleteSelection();
        CHECK(rig.controller.lastError() == QStringLiteral("clip 'B' is locked"));
        CHECK(rig.picture() == original);
        // A ripple delete of a clip before it would push it: refused as well.
        rig.controller.selectClipById(a, ProjectController::Replace, false);
        rig.controller.rippleDeleteSelection();
        CHECK(rig.controller.lastError() == QStringLiteral("clip 'B' is locked"));
        CHECK(rig.picture() == original);
        CHECK(rig.undoSteps() == steps);
    }
    SECTION("unlocked again, it drags") {
        rig.controller.selectClipById(b, ProjectController::Replace, false);
        rig.controller.toggleLockSelection();
        CHECK_FALSE(rig.controller.selectionLocked());
        CHECK(rig.controller.undoText() == QStringLiteral("Undo Unlock Clip"));
        settle(rig);
        CHECK_FALSE(rig.view->stats().lockBadges.contains(b.value));
        rig.drag({rig.x(5.5), y}, {rig.x(9.5), y});
        CHECK(rig.controller.undoText() == QStringLiteral("Undo Move"));
    }
}

TEST_CASE("Lock Clip toggles for a selection and survives save and load (card 064)", "[app][timeline][clip-lock]") {
    Rig rig(R"(
        V1 |AaaaBbbbCccc|
        A1 |AaaaBbbbCccc|
    )", 10.0, true);
    const ClipId b = rig.clip("V1@4");
    const ClipId bAudio = rig.clip("A1@4");
    // The linked selection takes both halves: both lock, the menu says Unlock afterwards.
    rig.controller.selectClipById(b, ProjectController::Replace, true);
    rig.controller.toggleLockSelection();
    CHECK(rig.controller.selectionLocked());
    CHECK(rig.controller.currentSequence()->findClip(bAudio)->clip().locked);
    // A mixed selection locks first; all locked unlocks.
    rig.controller.setSelection({b, rig.clip("V1@0")});
    CHECK_FALSE(rig.controller.selectionLocked());
    rig.controller.toggleLockSelection();
    CHECK(rig.controller.selectionLocked());
    CHECK(rig.controller.undoText() == QStringLiteral("Undo Lock Clip"));
    rig.controller.toggleLockSelection();
    CHECK(rig.controller.undoText() == QStringLiteral("Undo Unlock Clip"));
    CHECK_FALSE(rig.controller.currentSequence()->findClip(rig.clip("V1@0"))->clip().locked);
    // The palette / keymap command.
    rig.controller.selectClipById(b, ProjectController::Replace, true);
    CHECK(rig.controller.commandAvailability(QStringLiteral("clip.lock")).value(QStringLiteral("enabled")).toBool());
    rig.controller.clearSelection();
    CHECK_FALSE(rig.controller.commandAvailability(QStringLiteral("clip.lock")).value(QStringLiteral("enabled")).toBool());
}

TEST_CASE("A locked clip wears a padlock in the shell, dark and light (card 064)",
          "[app][qml][timeline][clip-lock][screenshot]") {
    resetSettings();
    clearMessages();
    for (const bool dark : {true, false}) {
        ShellProject p("V1 tgt |" + run('A', 60) + run('B', 60) + run('C', 60) + "|", false);
        p.shell.setAppearance(dark, "ultraviolet");
        TimelineView* view = p.view();
        view->changeTrackHeights(QStringLiteral("video"), 6); // a tall track: the padlock reads in the screenshot
        const ClipId a = p.tl.clip("V1@0");
        const ClipId b = p.tl.clip("V1@60");
        p.controller->selectClipById(b, ProjectController::Replace, false);
        p.controller->toggleLockSelection();
        p.controller->selectClipById(a, ProjectController::Replace, false); // the locked clip shows its padlock unselected too
        p.settle();
        p.settle();
        REQUIRE(view->stats().lockBadges.contains(b.value));
        CHECK_FALSE(view->stats().lockBadges.contains(a.value));
        p.shot(sceneRect(view).adjusted(-10, -10, 10, 20),
               dark ? QStringLiteral("a6-clip-lock.png") : QStringLiteral("a6-clip-lock-light.png"));
    }
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
// ---- Card 041: the fx badge --------------------------------------------------------------------------------------------

TEST_CASE("Clicking a clip's fx badge opens the menu of its properties; choosing one shows its keyframes (card 041)",
          "[app][timeline][badges][keyframes]") {
    const std::string picture = "V1 |" + run('A', 8) + run('B', 8) + run('C', 8) + gap(4) + "|";
    Rig rig(picture, 12.0, false);
    const ClipId a = rig.clip("V1@0");
    const ClipId b = rig.clip("V1@8");
    const ClipId c = rig.clip("V1@16");
    REQUIRE(rig.controller.applyEditFn("Add Keyframe", [&](const ProjectState& s) {
        return edit::addKeyframe(s, rig.tl.seq, b, edit::ParamRef::of(ParamId::Scale), Keyframe{f(2), 0.5});
    }));
    REQUIRE(rig.controller.applyEditFn("Add Keyframe", [&](const ProjectState& s) {
        return edit::addKeyframe(s, rig.tl.seq, b, edit::ParamRef::of(ParamId::Scale), Keyframe{f(6), 1.5});
    }));
    REQUIRE(rig.controller.applyEditFn("Add Effect", [&](const ProjectState& s) {
        return edit::addEffect(s, rig.tl.seq, std::span(&a, 1), "wabcut.blur.gaussian");
    }));
    settle(rig);
    const auto& chips = rig.view->stats().fxRects;
    REQUIRE(chips.contains(a.value));
    REQUIRE(chips.contains(b.value));
    CHECK_FALSE(chips.contains(c.value)); // an untouched clip has no chip, so no click target
    const QRectF chipB = chips.value(b.value);
    CHECK(chipB.left() >= rig.x(8));
    CHECK(chipB.right() <= rig.x(16));

    int clicked = 0;
    QString clickedKey;
    QObject::connect(rig.view, &TimelineView::fxBadgeClicked, [&](const QString& key, double, double) {
        ++clicked;
        clickedKey = key;
    });
    // A click on the chip selects the clip and asks for the menu.
    rig.press(chipB.center());
    rig.release(chipB.center());
    CHECK(clicked == 1);
    CHECK(clickedKey == clipKey(b));
    REQUIRE(rig.controller.selection().size() == 1);
    CHECK(rig.controller.selection().front() == b);
    CHECK(rig.undoSteps() == 2 + 1); // two keyframes and the effect were the only edits
    // A click on the clip's body, and a drag that starts on the chip, do not.
    rig.press({rig.x(14), rig.y("V1")});
    rig.release({rig.x(14), rig.y("V1")});
    CHECK(clicked == 1);
    const std::size_t beforeDrag = rig.undoSteps();
    rig.press(chipB.center());
    rig.move(chipB.center() + QPointF(20, 0));
    rig.move(chipB.center() + QPointF(40, 0));
    CHECK(rig.view->gestureActive());
    rig.release(chipB.center() + QPointF(40, 0));
    CHECK(clicked == 1);
    REQUIRE(rig.undoSteps() == beforeDrag + 1); // the drag moved the clip: take it back
    REQUIRE(rig.controller.undo());
    settle(rig);

    // The menu's properties: Motion's and Opacity for a video clip, the keyed one marked; the effect's for the blurred one.
    const QVariantList props = rig.view->clipKeyframeProperties(clipKey(b));
    REQUIRE(props.size() == 7);
    QString scaleId;
    int keyed = 0;
    for (const QVariant& p : props) {
        const QVariantMap m = p.toMap();
        if (m.value(QStringLiteral("keyed")).toBool()) {
            ++keyed;
            scaleId = m.value(QStringLiteral("id")).toString();
            CHECK(m.value(QStringLiteral("group")).toString() == QStringLiteral("Motion"));
            CHECK(m.value(QStringLiteral("label")).toString() == QStringLiteral("Scale"));
        }
        CHECK_FALSE(m.value(QStringLiteral("shown")).toBool());
    }
    CHECK(keyed == 1);
    const QVariantList blurProps = rig.view->clipKeyframeProperties(clipKey(a));
    bool effectParam = false;
    for (const QVariant& p : blurProps)
        effectParam = effectParam || p.toMap().value(QStringLiteral("id")).toString().startsWith(QStringLiteral("e:"));
    CHECK(effectParam);
    CHECK(rig.view->clipKeyframeProperties(QStringLiteral("c999999")).isEmpty());

    // Choosing one shows its keyframes on the clip (two keys drawn); it is view state: no undo step, nothing saved.
    const std::size_t steps = rig.undoSteps();
    rig.view->showClipKeyframes(clipKey(b), scaleId);
    settle(rig);
    CHECK(rig.view->shownClipKeyframes(clipKey(b)) == scaleId);
    CHECK(rig.view->stats().keyframeLines.value(b.value, -1) == 2);
    CHECK(rig.undoSteps() == steps);
    for (const QVariant& p : rig.view->clipKeyframeProperties(clipKey(b)))
        CHECK(p.toMap().value(QStringLiteral("shown")).toBool() == (p.toMap().value(QStringLiteral("id")).toString() == scaleId));
    // The playhead jumps between its keys.
    rig.controller.setPlayhead(f(0));
    REQUIRE(rig.view->seekClipKeyframe(clipKey(b), true));
    CHECK(rig.controller.playhead() == f(10)); // media time 2 on a clip that starts at 8
    REQUIRE(rig.view->seekClipKeyframe(clipKey(b), true));
    CHECK(rig.controller.playhead() == f(14));
    CHECK_FALSE(rig.view->seekClipKeyframe(clipKey(b), true));
    REQUIRE(rig.view->seekClipKeyframe(clipKey(b), false));
    CHECK(rig.controller.playhead() == f(10));
    // A flat property shows a line without keys; ids that are no property of the clip change nothing.
    rig.view->showClipKeyframes(clipKey(b), QStringLiteral("p:6"));
    settle(rig);
    CHECK(rig.view->stats().keyframeLines.value(b.value, -1) == 0);
    rig.view->showClipKeyframes(clipKey(b), QStringLiteral("p:99"));
    rig.view->showClipKeyframes(clipKey(b), QStringLiteral("e:12345:radius"));
    rig.view->showClipKeyframes(clipKey(b), QStringLiteral("nonsense"));
    CHECK(rig.view->shownClipKeyframes(clipKey(b)) == QStringLiteral("p:6"));
    // Hide.
    rig.view->showClipKeyframes(clipKey(b), QString());
    settle(rig);
    CHECK(rig.view->shownClipKeyframes(clipKey(b)).isEmpty());
    CHECK_FALSE(rig.view->stats().keyframeLines.contains(b.value));
    CHECK(rig.undoSteps() == steps);
}

TEST_CASE("The fx badge menu opens in the shell on a click and lists the clip's properties (card 041)",
          "[app][qml][timeline][badges][keyframes][screenshot]") {
    resetSettings();
    clearMessages();
    for (const bool dark : {true, false}) {
        ShellProject p("V1 tgt |" + run('A', 60) + run('B', 60) + run('C', 60) + "|", false);
        p.shell.setAppearance(dark, "ultraviolet");
        const ClipId b = p.tl.clip("V1@60");
        REQUIRE(p.controller->applyEditFn("Add Keyframe", [&](const ProjectState& s) {
            return edit::addKeyframe(s, p.tl.seq, b, edit::ParamRef::of(ParamId::Opacity), Keyframe{f(2), 0.5});
        }));
        REQUIRE(p.controller->applyEditFn("Add Keyframe", [&](const ProjectState& s) {
            return edit::addKeyframe(s, p.tl.seq, b, edit::ParamRef::of(ParamId::Opacity), Keyframe{f(9), 1.0});
        }));
        TimelineView* view = p.view();
        view->changeTrackHeights(QStringLiteral("video"), 6); // a tall track: the line and its keys read in the screenshot
        p.settle();
        p.settle();
        REQUIRE(view->stats().fxRects.contains(b.value));
        const QPointF at = view->mapToScene(view->stats().fxRects.value(b.value).center());
        mouseAt(p.shell.window.get(), QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
        mouseAt(p.shell.window.get(), QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
        QObject* menu = p.object(QStringLiteral("clipKeyframesMenu"));
        REQUIRE(menu);
        REQUIRE(waitFor([&] { return menu->property("opened").toBool(); }, 5000));
        QQuickItem* background = menu->property("background").value<QQuickItem*>();
        REQUIRE(background);
        // The title, Hide / Previous / Next, then the seven properties of a video clip; the keyed Opacity carries a diamond.
        CHECK(menu->findChild<QObject*>(QStringLiteral("clipKeyframesHide")));
        CHECK_FALSE(menu->findChild<QObject*>(QStringLiteral("clipKeyframesHide"))->property("enabled").toBool());
        for (int i = 0; i < 7; ++i)
            CHECK(menu->findChild<QObject*>(QStringLiteral("clipKeyframesProperty_%1").arg(i)));
        CHECK_FALSE(menu->findChild<QObject*>(QStringLiteral("clipKeyframesProperty_7")));
        p.shot(sceneRect(background).adjusted(-16, -16, 16, 16).united(sceneRect(view).adjusted(0, 0, 0, 0)).intersected(
                   sceneRect(view).adjusted(-10, -10, 10, 400)),
               dark ? QStringLiteral("a6-fx-badge-menu.png") : QStringLiteral("a6-fx-badge-menu-light.png"));
        // Choosing a property through the menu's own function shows its keyframes and checks it.
        REQUIRE(QMetaObject::invokeMethod(menu, "choose", Q_ARG(QVariant, QStringLiteral("p:6"))));
        p.settle();
        CHECK(view->shownClipKeyframes(clipKey(b)) == QStringLiteral("p:6"));
        CHECK(view->stats().keyframeLines.value(b.value, -1) == 2);
        QMetaObject::invokeMethod(menu, "close");
        REQUIRE(waitFor([&] { return !menu->property("visible").toBool(); }, 5000));
        p.settle();
        p.shot(sceneRect(view).adjusted(-10, -10, 10, 20), dark ? QStringLiteral("a6-keyframes-on-clip.png")
                                                                : QStringLiteral("a6-keyframes-on-clip-light.png"));
    }
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// ---- Card 067: reorder tracks by dragging ------------------------------------------------------------------------------

TEST_CASE("Dragging a track header's name chip moves the track, with a drop line, in one undo step (card 067)",
          "[app][qml][timeline][tracks][screenshot]") {
    resetSettings();
    clearMessages();
    ShellProject p(R"(
        V3     |Cccccccc|
        V2     |Bbbbbbbb|
        V1 tgt |Aaaaaaaa|
        A1 tgt |Aaaaaaaa|
        A2     |Dddddddd|
    )");
    const auto header = [&](const char* name) {
        QQuickItem* found = headerOf(p.item(QStringLiteral("trackName_") + QLatin1String(name)));
        REQUIRE(found);
        return found;
    };
    const auto chipOf = [&](const char* name) { return findNamed(header(name), QStringLiteral("trackTarget")); };
    p.settle();
    QQuickItem* v3Header = header("V3");
    REQUIRE(v3Header);
    QQuickWindow* window = p.shell.window.get();
    const std::size_t steps = p.undo();

    // Press on the free space of V1's header (above its controls) and drag it up over V3's header: the drop line shows, the
    // track moves on release. (The chip drags too: the second drag below starts on A1's.)
    QQuickItem* v1Header = header("V1");
    const QPointF start = v1Header->mapToScene(QPointF(v1Header->width() * 0.5, 5));
    const QPointF end = v3Header->mapToScene(QPointF(v3Header->width() / 2, v3Header->height() / 2));
    mouseAt(window, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 8; ++i) {
        mouseAt(window, QEvent::MouseMove, start + (end - start) * (i / 8.0), Qt::NoButton, Qt::LeftButton);
        spin(10);
    }
    p.settle();
    QQuickItem* line = p.item(QStringLiteral("trackMoveLine"));
    REQUIRE(line);
    CHECK(line->isVisible());
    CHECK(header("V1")->property("reordering").toBool());
    p.shot(QRectF(0, sceneRect(header("V3")).top() - 20, 420, sceneRect(header("A2")).bottom() - sceneRect(header("V3")).top() + 40),
           QStringLiteral("a6-track-drag.png"));
    mouseAt(window, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
    spin(60);
    p.settle();
    CHECK_FALSE(line->isVisible());
    CHECK(p.undo() == steps + 1);
    CHECK(p.controller->undoText() == QStringLiteral("Undo Move Track"));
    CHECK_TIMELINE(*p.controller->snapshot(), p.tl.seq, R"(
        V3 |Aaaaaaaa|
        V2 |Cccccccc|
        V1 |Bbbbbbbb|
        A1 |Aaaaaaaa|
        A2 |Dddddddd|
    )");
    // The target flag went along with the track, and the picture's link to its sound stayed.
    CHECK(p.sequence().videoTracks[2]->targeted);
    CHECK(links(*p.controller->snapshot(), p.tl.seq) == "V3@0+A1@0");

    // A drag that does not leave the chip's own row is a click: it targets, and moves nothing.
    QQuickItem* a2 = chipOf("A2");
    REQUIRE(a2);
    const QPointF chip = a2->mapToScene(QPointF(a2->width() / 2, a2->height() / 2));
    mouseAt(window, QEvent::MouseButtonPress, chip, Qt::LeftButton, Qt::LeftButton);
    mouseAt(window, QEvent::MouseButtonRelease, chip, Qt::LeftButton, Qt::NoButton);
    spin(50);
    CHECK(p.undo() == steps + 2); // the target switch is its own step
    CHECK(p.sequence().audioTracks[1]->targeted);

    // An audio track drags down the audio stack: A1 to A2's place.
    p.settle();
    QQuickItem* a1 = chipOf("A1");
    QQuickItem* a2Header = header("A2");
    const QPointF from = a1->mapToScene(QPointF(a1->width() / 2, a1->height() / 2));
    const QPointF to = a2Header->mapToScene(QPointF(a2Header->width() / 2, a2Header->height() / 2));
    mouseAt(window, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 6; ++i) {
        mouseAt(window, QEvent::MouseMove, from + (to - from) * (i / 6.0), Qt::NoButton, Qt::LeftButton);
        spin(10);
    }
    mouseAt(window, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
    spin(60);
    CHECK_TIMELINE(*p.controller->snapshot(), p.tl.seq, R"(
        V3 |Aaaaaaaa|
        V2 |Cccccccc|
        V1 |Bbbbbbbb|
        A1 |Dddddddd|
        A2 |Aaaaaaaa|
    )");
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("A click on a track's name chip still targets it, a short slide too (card 067)", "[app][qml][timeline][tracks]") {
    resetSettings();
    clearMessages();
    ShellProject p(R"(
        V1 tgt |Aaaaaaaa|
        A1 tgt |Aaaaaaaa|
        A2     |Dddddddd|
    )");
    p.settle();
    QQuickItem* header = headerOf(p.item(QStringLiteral("trackName_A2")));
    REQUIRE(header);
    QQuickItem* chip = findNamed(header, QStringLiteral("trackTarget"));
    REQUIRE(chip);
    QQuickWindow* window = p.shell.window.get();
    const QPointF at = chip->mapToScene(QPointF(chip->width() / 2, chip->height() / 2));
    const std::size_t steps = p.undo();
    // Shift-click (every audio track): the click path the header's tests have always used.
    mouseAt(window, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton, Qt::ShiftModifier);
    mouseAt(window, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier);
    spin(60);
    CHECK(p.sequence().audioTracks[1]->targeted);
    CHECK(p.undo() == steps + 1);
    // A plain click targets this track only: switch it off again.
    mouseAt(window, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
    mouseAt(window, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
    spin(60);
    CHECK_FALSE(p.sequence().audioTracks[1]->targeted);
    CHECK(p.undo() == steps + 2);
    mouseAt(window, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
    mouseAt(window, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
    spin(60);
    CHECK(p.sequence().audioTracks[1]->targeted);
    CHECK(p.undo() == steps + 3);
    // A slide of a few pixels (under the drag distance) is a click as well, and never a reorder.
    mouseAt(window, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
    mouseAt(window, QEvent::MouseMove, at + QPointF(0, 3), Qt::NoButton, Qt::LeftButton);
    mouseAt(window, QEvent::MouseButtonRelease, at + QPointF(0, 3), Qt::LeftButton, Qt::NoButton);
    spin(60);
    CHECK_FALSE(p.sequence().audioTracks[1]->targeted);
    CHECK(p.undo() == steps + 4);
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Move Track Up and Down in the track menu, and the controller's move (card 067)", "[app][qml][timeline][tracks]") {
    resetSettings();
    clearMessages();
    ShellProject p(R"(
        V2     |Bbbbbbbb|
        V1 tgt |Aaaaaaaa|
        A1 tgt |Aaaaaaaa|
        A2     |Dddddddd|
    )");
    p.settle();
    ProjectController& c = *p.controller;
    const QString v1 = trackKey(p.tl.track("V1"));
    const QString a1 = trackKey(p.tl.track("A1"));
    CHECK(c.trackInfo(v1).value(QStringLiteral("kindCount")).toInt() == 2);
    // V1 up one place is V2; "up" on an audio track is toward A1.
    CHECK(c.moveTrackBy(v1, 1));
    CHECK(c.undoText() == QStringLiteral("Undo Move Track"));
    CHECK_TIMELINE(*c.snapshot(), p.tl.seq, R"(
        V2 |Aaaaaaaa|
        V1 |Bbbbbbbb|
        A1 |Aaaaaaaa|
        A2 |Dddddddd|
    )");
    const std::size_t steps = p.undo();
    CHECK(c.moveTrackBy(v1, 1)); // already on top: nothing happens
    CHECK(p.undo() == steps);
    CHECK(c.moveTrack(a1, 2));
    CHECK_TIMELINE(*c.snapshot(), p.tl.seq, R"(
        V2 |Aaaaaaaa|
        V1 |Bbbbbbbb|
        A1 |Dddddddd|
        A2 |Aaaaaaaa|
    )");
    CHECK_FALSE(c.moveTrack(QStringLiteral("t999999"), 1));
    CHECK_FALSE(c.lastError().isEmpty());

    // The menu's two rows are there and say what the track can do.
    QQuickItem* header = headerOf(p.item(QStringLiteral("trackName_V1")));
    REQUIRE(header);
    QQuickItem* chip = findNamed(header, QStringLiteral("trackTarget"));
    REQUIRE(QMetaObject::invokeMethod(header, "openMenu", Q_ARG(QVariant, QVariant::fromValue(chip)),
                                      Q_ARG(QVariant, QPointF(0, chip->height()))));
    QObject* menu = header->findChild<QObject*>(QStringLiteral("trackHeaderMenu"));
    if (!menu)
        menu = p.object(QStringLiteral("trackHeaderMenu"));
    REQUIRE(menu);
    REQUIRE(waitFor([&] { return menu->property("opened").toBool(); }, 5000));
    QObject* up = menu->findChild<QObject*>(QStringLiteral("trackMenuMoveUp"));
    QObject* down = menu->findChild<QObject*>(QStringLiteral("trackMenuMoveDown"));
    REQUIRE(up);
    REQUIRE(down);
    CHECK(up->property("enabled").toBool());     // V1 is the lowest video track now (it was moved down by the first step)
    CHECK_FALSE(down->property("enabled").toBool());
    QMetaObject::invokeMethod(menu, "close");
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// ---- Card 097: Match Frame into a nest ----------------------------------------------------------------------------------

TEST_CASE("Match Frame on a nested clip opens its source sequence on the frame it shows (card 097)", "[app][source][matchframe][nesting]") {
    Timeline tl(R"(
        V1 tgt |AaaaaaaaaaBbbbbbbbbbCccccccccc|
        A1 tgt |AaaaaaaaaaBbbbbbbbbbCccccccccc|
    )");
    tl.linkMatching();
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    SequenceTabsBridge tabs;
    tabs.setController(&controller);
    LibraryBridge library;
    library.setController(&controller);
    NestingBridge nesting;
    nesting.setController(&controller);
    nesting.setTabs(&tabs);
    nesting.setLibrary(&library);
    SourceMonitorBridge source;
    source.setController(&controller);
    source.setNesting(&nesting);

    controller.setSelection({tl.clip("V1@10")});
    REQUIRE(nesting.nestSelection(QStringLiteral("Talk")).isEmpty());
    const SequenceId parent = controller.currentSequenceId();
    const Clip nest = controller.currentSequence()->videoTracks[0]->clips[1];
    REQUIRE(nest.isNested());
    const SequenceId nestedId = nest.sequence;
    const std::size_t steps = controller.session().undoCount();

    // Frame 14 of the sequence is frame 4 of the nest (it starts at 10): Match Frame opens the nest there.
    controller.setPlayhead(f(14));
    REQUIRE(source.matchFrame());
    CHECK(controller.currentSequenceId() == nestedId);
    CHECK(tabs.current() == sequenceKey(nestedId));
    CHECK(controller.playhead() == f(4));
    CHECK(controller.session().undoCount() == steps); // opening a sequence is no edit
    REQUIRE(nesting.breadcrumb().size() == 2);        // the way back to the sequence it came from
    CHECK(nesting.goTo(sequenceKey(parent)));
    CHECK(controller.currentSequenceId() == parent);

    // A clip that is not nested still loads its media into the Source Monitor, as before.
    controller.setPlayhead(f(25));
    REQUIRE(source.matchFrame());
    CHECK(source.mediaId() == tl.media('C'));
    CHECK(controller.currentSequenceId() == parent);
    // Nothing under the playhead: refused, with a message.
    controller.setPlayhead(f(45));
    CHECK_FALSE(source.matchFrame());
    CHECK_FALSE(source.message().isEmpty());
    // Without a way to open sequences the nest is refused and says so.
    SourceMonitorBridge alone;
    alone.setController(&controller);
    controller.setPlayhead(f(14));
    CHECK_FALSE(alone.matchFrame());
    CHECK_FALSE(alone.message().isEmpty());
}
