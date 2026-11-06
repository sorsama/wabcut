// 0.7, batch A4 (timeline accessibility, labels and slip): the app side of cards 007, 008, 010, 077 and 127.
//   * 007 Go to Gap > Next / Previous in Track selects the gap it lands on; the Sequence menu's rows; Auto Reframe Sequence.
//   * 008 The Menu key / Shift+F10 open the context menu of the selected clip, transition or gap (lanes, clip list, list).
//   * 010 Named label colours in the clip menu (the current one checked), Edit > Label and Select Label Group.
//   * 077 A track's label colour: the header menu, the header bar, the lane wash, the mixer colour, undo.
//   * 127 Slip with the trim tool (Avid style): both edges of a clip, the Slip type, Slip Trim.
// Screenshots under test-output/ui (timeline-a4-*.png).

#include "ShellFixture.h"
#include "TimelineFixture.h"
#include "TimelineHarness.h"

#include "BridgeIds.h"
#include "CommandPalette.h"
#include "EditingBridge.h"
#include "LibraryBridge.h"
#include "ProjectController.h"
#include "TimelineView.h"
#include "TrimBridge.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QKeyEvent>
#include <QQuickItem>

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
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

QRectF sceneRect(const QQuickItem* item) {
    return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
}

void tap(QQuickWindow* window, int key, Qt::KeyboardModifiers mods = {}) {
    QKeyEvent press(QEvent::KeyPress, key, mods);
    QCoreApplication::sendEvent(window, &press);
    QKeyEvent release(QEvent::KeyRelease, key, mods);
    QCoreApplication::sendEvent(window, &release);
}

std::vector<ClipId> sorted(std::vector<ClipId> ids) {
    std::ranges::sort(ids);
    return ids;
}

/// A project from a timeline picture under a ProjectController and an EditingBridge (no window).
struct Project {
    Timeline tl;
    ProjectController controller;
    EditingBridge editing;

    explicit Project(std::string_view picture, const std::map<char, std::int64_t>& media = {}) : tl(picture, media) {
        tl.linkMatching();
        controller.resetProject(tl.state, tl.seq);
        editing.setController(&controller);
    }
    const Sequence& sequence() const { return *controller.currentSequence(); }
    Clip clip(std::string_view ref) const { return sequence().findClip(tl.clip(ref))->clip(); }
    std::size_t undoSteps() { return controller.session().undoCount(); }
    QString key(std::string_view ref) const { return clipKey(tl.clip(ref)); }
    std::vector<ClipId> selected() const { return sorted(controller.selection()); }
    std::vector<ClipId> ids(std::initializer_list<std::string_view> refs) const {
        std::vector<ClipId> out;
        for (const std::string_view ref : refs)
            out.push_back(tl.clip(ref));
        return sorted(out);
    }
};

/// The real shell (Main.qml, hidden, 1600 x 960, reduced motion) over a project from a timeline picture.
struct ShellProject {
    Shell shell;
    ProjectController* controller = nullptr;
    Timeline tl;

    explicit ShellProject(std::string_view picture, bool dark = true) : tl(picture) {
        REQUIRE(shell.window);
        INFO(join(shell.errors));
        REQUIRE(shell.errors.isEmpty());
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        shell.setAppearance(dark, "ultraviolet");
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        REQUIRE(controller);
        tl.linkMatching();
        controller->resetProject(tl.state, tl.seq);
        spin(200);
    }
    template <class T>
    T* singleton(const char* name) {
        T* found = shell.engine.singletonInstance<T*>("Wabcut", name);
        REQUIRE(found);
        return found;
    }
    QQuickItem* item(const QString& name) const { return findNamed(shell.window->contentItem(), name); }
    QObject* object(const QString& name) const { return shell.window->findChild<QObject*>(name); }
    const Sequence& sequence() const { return *controller->currentSequence(); }
    bool run(const char* command) {
        return QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QString::fromLatin1(command)));
    }
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
    QColor themeLabel(int index) const {
        QVariant color;
        REQUIRE(QMetaObject::invokeMethod(shell.theme, "clipColor", Q_RETURN_ARG(QVariant, color), Q_ARG(QVariant, index)));
        return color.value<QColor>();
    }
    /// A popup (a context menu) is open.
    static bool opened(QObject* popup) { return popup && popup->property("opened").toBool(); }
};

void checkNoMessages() {
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

} // namespace

// ---- 007: Go to Gap in a track selects the gap ---------------------------------------------------------------------------

TEST_CASE("Go to Gap in Track selects the gap it lands on and Delete closes it (card 007)", "[app][editing][gaps][a4]") {
    Project p(R"(
        V1 tgt |Aaa...Bbbb..Cc|
    )");
    ProjectController& c = p.controller;
    c.setPlayhead(f(0));

    REQUIRE(p.editing.runCommand(QStringLiteral("sequence.nextInTrack")));
    CHECK(c.playhead() == f(3));
    REQUIRE(c.gapSelection().has_value());
    CHECK(c.gapSelection()->track == p.tl.track("V1"));
    CHECK(c.gapSelection()->range.start == f(3));
    CHECK(c.gapSelection()->range.end() == f(6));
    CHECK(c.selectedGap().value(QStringLiteral("start")).toString() == c.formatTimecode(f(3)));
    // The jump moved the playhead and selected the gap; it is not an edit.
    CHECK(p.undoSteps() == 0);

    // The next one, then back.
    REQUIRE(p.editing.runCommand(QStringLiteral("sequence.nextInTrack")));
    CHECK(c.playhead() == f(10));
    CHECK(c.gapSelection()->range.start == f(10));
    CHECK(c.gapSelection()->range.end() == f(12));
    REQUIRE(p.editing.runCommand(QStringLiteral("sequence.previousInTrack")));
    CHECK(c.playhead() == f(3));
    CHECK(c.gapSelection()->range.start == f(3));

    // Delete / Backspace / Shift+Delete run the panel's ripple delete of the selected gap: one undo step.
    REQUIRE(c.rippleDeleteGap());
    CHECK(p.undoSteps() == 1);
    CHECK(p.clip("V1@6").start() == f(3)); // B moved up to where the gap began
    CHECK(c.undoText() == QStringLiteral("Undo Ripple Delete"));
    REQUIRE(c.undo());
    CHECK(p.clip("V1@6").start() == f(6));

    // A clip selection goes when a gap is selected (a gap and clips are selected one or the other).
    c.selectClip(p.key("V1@0"));
    REQUIRE_FALSE(c.selection().empty());
    c.setPlayhead(f(0));
    REQUIRE(p.editing.runCommand(QStringLiteral("sequence.nextInTrack")));
    CHECK(c.selection().empty());
    CHECK(c.gapSelection().has_value());

    // Past the last gap nothing moves and the selection stays.
    c.setPlayhead(f(12));
    CHECK_FALSE(p.editing.goToGap(true, false));
    CHECK(c.playhead() == f(12));
    CHECK(c.gapSelection()->range.start == f(3));

    // The in-Sequence variant (Shift+;) only moves the playhead.
    c.clearSelection();
    c.setPlayhead(f(0));
    REQUIRE(p.editing.runCommand(QStringLiteral("sequence.nextInSequence")));
    CHECK(c.playhead() == f(3));
    CHECK_FALSE(c.gapSelection().has_value());
}

TEST_CASE("Go to Gap in Track looks at the targeted tracks, or at every track when none is targeted (card 007)",
          "[app][editing][gaps][a4]") {
    SECTION("no track targeted: every track counts") {
        Project p(R"(
            V1 |Aaa...Bbbb|
            A1 |AaaBbbbbbb|
        )");
        p.controller.setPlayhead(f(0));
        REQUIRE(p.editing.goToGap(true, false));
        CHECK(p.controller.playhead() == f(3));
        REQUIRE(p.controller.gapSelection().has_value());
        CHECK(p.controller.gapSelection()->track == p.tl.track("V1")); // the track that has the gap
    }
    SECTION("a targeted track without a gap: nothing moves") {
        Project p(R"(
            V1     |Aaa...Bbbb|
            A1 tgt |AaaBbbbbbb|
        )");
        p.controller.setPlayhead(f(0));
        CHECK_FALSE(p.editing.goToGap(true, false));
        CHECK(p.controller.playhead() == f(0));
        CHECK_FALSE(p.controller.gapSelection().has_value());
    }
    SECTION("the gap of the first targeted track that has one is selected") {
        Project p(R"(
            V2 tgt |..Bbbb....|
            V1 tgt |Aaa...Cccc|
        )");
        p.controller.setPlayhead(f(0));
        REQUIRE(p.editing.goToGap(true, false));
        // V2's gap before B starts at 0 (not after the playhead), V1's gap starts at 3.
        CHECK(p.controller.playhead() == f(3));
        CHECK(p.controller.gapSelection()->track == p.tl.track("V1"));
        CHECK(p.controller.gapSelection()->range.start == f(3));
    }
}

TEST_CASE("Sequence > Go to Gap lists the in-Track rows, and Auto Reframe Sequence opens its dialog (card 007)",
          "[app][qml][menubar][a4]") {
    resetSettings();
    clearMessages();
    ShellProject p(R"(
        V1 tgt |Aaa...Bbbb..Cc|
    )");
    QObject* sequenceMenu = p.object(QStringLiteral("sequenceMenuBar"));
    REQUIRE(sequenceMenu);
    for (const char* name : {"sequenceMenuNextInTrack", "sequenceMenuPreviousInTrack", "sequenceMenuAutoReframeRow",
                             "sequenceMenuAutoReframe", "sequenceMenuSlipTrim"}) {
        INFO(name);
        CHECK(sequenceMenu->findChild<QObject*>(QString::fromLatin1(name)) != nullptr);
    }
    CommandPalette* palette = p.singleton<CommandPalette>("CommandPalette");
    for (const char* id : {"sequence.nextInTrack", "sequence.previousInTrack", "sequence.autoReframe", "sequence.slipTrim"}) {
        INFO(id);
        CHECK(palette->commandState(QString::fromLatin1(id)).contains(QStringLiteral("enabled")));
    }
    // The menu's row runs the command: the playhead jumps and the gap is selected.
    p.controller->setPlayhead(f(0));
    REQUIRE(p.run("sequence.nextInTrack"));
    CHECK(waitFor([&] { return p.controller->playhead() == f(3); }, 3000));
    CHECK(p.controller->gapSelection().has_value());
    // Auto Reframe Sequence… (the reframe dialog) has its own command, reachable from the menu and the palette.
    REQUIRE(p.run("sequence.autoReframe"));
    QObject* dialog = nullptr;
    REQUIRE(waitFor([&] {
        dialog = p.object(QStringLiteral("reframeDialog"));
        return ShellProject::opened(dialog);
    }, 5000));
    QMetaObject::invokeMethod(dialog, "close");
    checkNoMessages();
}

// ---- 010: label colours, Edit > Label, Select Label Group ----------------------------------------------------------------

TEST_CASE("Select Label Group selects every clip with the selection's label colour, with no undo step (card 010)",
          "[app][selection][label][a4]") {
    Project p(R"(
        V1 tgt |AaaaBbbbCccc|
        A1 tgt |AaaaBbbbCccc|
    )");
    ProjectController& c = p.controller;
    // Label 4 on A (picture and sound) and on C's picture only; B keeps the default label.
    c.selectClip(p.key("V1@0"));
    c.setSelectionColor(4);
    c.selectClip(p.key("V1@8"), ProjectController::Replace, false);
    c.setSelectionColor(4);
    CHECK(p.clip("V1@0").colorIndex == 4);
    CHECK(p.clip("A1@0").colorIndex == 4);
    CHECK(p.clip("V1@8").colorIndex == 4);
    CHECK(p.clip("A1@8").colorIndex == 0);
    const std::size_t steps = p.undoSteps();

    // The label the selection shares (-1: mixed or nothing selected).
    c.clearSelection();
    CHECK(c.selectionLabel() == -1);
    c.selectClip(p.key("V1@0"), ProjectController::Replace, false);
    CHECK(c.selectionLabel() == 4);
    c.selectClip(p.key("V1@4"), ProjectController::Add, false);
    CHECK(c.selectionLabel() == -1);

    // Linked selection off: the clips with the label, no partners.
    c.setLinkedSelection(false);
    c.selectClip(p.key("V1@0"), ProjectController::Replace, false);
    REQUIRE(c.selectLabelGroup());
    CHECK(p.selected() == p.ids({"V1@0", "A1@0", "V1@8"}));
    CHECK(c.primaryClip().value != 0);
    // Linked selection on: C's sound comes along with its picture.
    c.setLinkedSelection(true);
    c.selectClip(p.key("V1@0"), ProjectController::Replace, false);
    REQUIRE(c.selectLabelGroup());
    CHECK(p.selected() == p.ids({"V1@0", "A1@0", "V1@8", "A1@8"}));
    // Several labels selected: all of their clips.
    c.selectClip(p.key("V1@0"), ProjectController::Replace, false);
    c.selectClip(p.key("V1@4"), ProjectController::Add, false);
    REQUIRE(c.selectLabelGroup());
    CHECK(p.selected() == p.ids({"V1@0", "A1@0", "V1@8", "A1@8", "V1@4", "A1@4"}));
    // A selection change only: no undo step, the clips are as they were.
    CHECK(p.undoSteps() == steps);
    CHECK(p.clip("V1@4").colorIndex == 0);

    // Nothing selected: the command is greyed out with the reason, and runs as a no-op.
    c.clearSelection();
    CHECK_FALSE(c.selectLabelGroup());
    const QVariantMap state = c.commandAvailability(QStringLiteral("edit.selectLabelGroup"));
    CHECK(state.value(QStringLiteral("handled")).toBool());
    CHECK_FALSE(state.value(QStringLiteral("enabled")).toBool());
    CHECK(state.value(QStringLiteral("reason")).toString() == QStringLiteral("Select a clip first"));
    CHECK(c.selection().empty());
    // The keymap command selects the group of a selected clip.
    c.selectClip(p.key("V1@8"), ProjectController::Replace, false);
    CHECK(c.commandAvailability(QStringLiteral("edit.selectLabelGroup")).value(QStringLiteral("enabled")).toBool());
    REQUIRE(c.runCommand(QStringLiteral("edit.selectLabelGroup")));
    CHECK(p.selected().size() == 4);
    CHECK(p.undoSteps() == steps);
}

TEST_CASE("The clip menu names the label colours and checks the current one; Edit > Label sets them (card 010)",
          "[app][qml][label][a4][screenshot]") {
    bool dark = true;
    SECTION("dark") { dark = true; }
    SECTION("light") { dark = false; }
    resetSettings();
    clearMessages();
    ShellProject p(R"(
        V1 tgt |AaaaBbbbCccc|
        A1 tgt |AaaaBbbbCccc|
    )", dark);
    auto* library = p.singleton<LibraryBridge>("Library");
    ProjectController& c = *p.controller;
    const QStringList names = library->labelNames();
    REQUIRE(names.size() == 16);
    p.shell.resize(kShotWidth, 1500); // the clip menu is long: room for all of it in the screenshot
    spin(100);

    // Clip C has label 9; the right-click on it selects it and opens the menu.
    c.selectClip(clipKey(p.tl.clip("V1@8")));
    c.setSelectionColor(9);
    c.selectClip(clipKey(p.tl.clip("V1@8")));
    auto* view = qobject_cast<TimelineView*>(p.item(QStringLiteral("timelineView")));
    REQUIRE(view);
    QObject* menu = p.object(QStringLiteral("clipContextMenu"));
    REQUIRE(menu);
    REQUIRE(QMetaObject::invokeMethod(menu, "openAt", Q_ARG(QVariant, QVariant::fromValue(static_cast<QObject*>(view))),
                                      Q_ARG(QVariant, clipKey(p.tl.clip("V1@8"))), Q_ARG(QVariant, QPointF(60, 40))));
    REQUIRE(waitFor([&] { return ShellProject::opened(menu); }, 5000));
    for (int i = 0; i < 16; ++i) {
        QQuickItem* swatch = p.item(QStringLiteral("clipLabelColor_%1").arg(i));
        INFO(i);
        REQUIRE(swatch);
        CHECK(swatch->property("tooltip").toString() == names[i]);      // the colour's name, not "Label color 10"
        CHECK(swatch->property("checked").toBool() == (i == 9));        // the clip's own label is checked
    }
    QQuickItem* group = p.item(QStringLiteral("clipMenuSelectLabelGroup"));
    REQUIRE(group);
    CHECK(group->property("text").toString() == QStringLiteral("Select Label Group"));
    spin(100);
    p.settle();
    p.shot(QRectF(0, 0, kShotWidth, 1500),
           dark ? QStringLiteral("timeline-a4-label-menu.png") : QStringLiteral("timeline-a4-label-menu-light.png"));

    // A swatch sets the selected clips' label (one undo step) and closes the menu.
    const std::size_t steps = c.session().undoCount();
    QQuickItem* five = p.item(QStringLiteral("clipLabelColor_5"));
    REQUIRE(five);
    REQUIRE(QMetaObject::invokeMethod(five, "clicked"));
    CHECK(c.session().undoCount() == steps + 1);
    CHECK(p.sequence().findClip(p.tl.clip("V1@8"))->clip().colorIndex == 5);
    CHECK(waitFor([&] { return !ShellProject::opened(menu); }, 5000));

    // Select Label Group in the menu selects the other clips of that colour.
    c.selectClip(clipKey(p.tl.clip("V1@0")));
    c.setSelectionColor(5);
    c.selectClip(clipKey(p.tl.clip("V1@8")));
    REQUIRE(QMetaObject::invokeMethod(menu, "openAt", Q_ARG(QVariant, QVariant::fromValue(static_cast<QObject*>(view))),
                                      Q_ARG(QVariant, clipKey(p.tl.clip("V1@8"))), Q_ARG(QVariant, QPointF(60, 40))));
    REQUIRE(waitFor([&] { return ShellProject::opened(menu); }, 5000));
    REQUIRE(p.item(QStringLiteral("clipLabelColor_5")));
    CHECK(p.item(QStringLiteral("clipLabelColor_5"))->property("checked").toBool());
    CHECK_FALSE(p.item(QStringLiteral("clipLabelColor_9"))->property("checked").toBool());
    REQUIRE(QMetaObject::invokeMethod(group, "triggered"));
    CHECK(c.selection().size() == 4); // A and C, picture and sound
    QMetaObject::invokeMethod(menu, "close");

    // Edit > Label: the 16 names, the shared label checked, rows greyed out without a clip, Select Label Group.
    QObject* labelMenu = p.object(QStringLiteral("editMenuLabel"));
    REQUIRE(labelMenu);
    c.clearSelection();
    QMetaObject::invokeMethod(labelMenu, "aboutToShow");
    for (int i = 0; i < 16; ++i) {
        QObject* row = labelMenu->findChild<QObject*>(QStringLiteral("editMenuLabel_%1").arg(i));
        INFO(i);
        REQUIRE(row);
        CHECK(row->property("text").toString() == names[i]);
        CHECK_FALSE(row->property("enabled").toBool());
    }
    CHECK(labelMenu->findChild<QObject*>(QStringLiteral("editMenuSelectLabelGroup")) != nullptr);
    c.selectClip(clipKey(p.tl.clip("V1@4")));
    QMetaObject::invokeMethod(labelMenu, "aboutToShow");
    CHECK(labelMenu->property("current").toInt() == 0);
    QObject* row0 = labelMenu->findChild<QObject*>(QStringLiteral("editMenuLabel_0"));
    QObject* row3 = labelMenu->findChild<QObject*>(QStringLiteral("editMenuLabel_3"));
    REQUIRE(row0);
    REQUIRE(row3);
    CHECK(row0->property("checked").toBool());
    CHECK_FALSE(row3->property("checked").toBool());
    CHECK(row3->property("enabled").toBool());
    const std::size_t before = c.session().undoCount();
    REQUIRE(QMetaObject::invokeMethod(row3, "triggered"));
    REQUIRE(waitFor([&] { return p.sequence().findClip(p.tl.clip("V1@4"))->clip().colorIndex == 3; }, 5000));
    CHECK(c.session().undoCount() == before + 1);
    CHECK(c.undoText() == QStringLiteral("Undo Label Color"));
    checkNoMessages();
}

/// Opens `submenu` of the open `parent` menu the way the keyboard does: the row that holds it becomes current, Right.
bool openSubmenuByKey(ShellProject& p, QObject* parent, QObject* submenu) {
    int count = 0;
    count = parent->property("count").toInt();
    int row = -1;
    for (int i = 0; i < count; ++i) {
        QQuickItem* item = nullptr;
        QMetaObject::invokeMethod(parent, "itemAt", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, i));
        if (item && item->property("subMenu").value<QObject*>() == submenu)
            row = i;
    }
    if (row < 0)
        return false;
    parent->setProperty("currentIndex", row);
    tap(p.shell.window.get(), Qt::Key_Right);
    return waitFor([&] { return ShellProject::opened(submenu); }, 5000);
}

TEST_CASE("Edit > Label opens as a submenu from the keyboard and Select Label Group is its last row (card 010)",
          "[app][qml][menubar][label][a4][screenshot]") {
    bool dark = true;
    SECTION("dark") { dark = true; }
    SECTION("light") { dark = false; }
    resetSettings();
    clearMessages();
    ShellProject p(R"(
        V1 tgt |AaaaBbbbCccc|
        A1 tgt |AaaaBbbbCccc|
    )", dark);
    p.controller->selectClip(clipKey(p.tl.clip("V1@4")));
    QObject* bar = p.object(QStringLiteral("appMenuBar"));
    QObject* editMenu = p.object(QStringLiteral("editMenu"));
    QObject* labelMenu = p.object(QStringLiteral("editMenuLabel"));
    REQUIRE(bar);
    REQUIRE(editMenu);
    REQUIRE(labelMenu);
    REQUIRE(QMetaObject::invokeMethod(bar, "open", Q_ARG(int, 1), Q_ARG(bool, false)));
    REQUIRE(waitFor([&] { return ShellProject::opened(editMenu); }, 5000));
    REQUIRE(openSubmenuByKey(p, editMenu, labelMenu));
    // The selected clip's label (the default one) is checked; every row is named and enabled.
    QObject* first = labelMenu->findChild<QObject*>(QStringLiteral("editMenuLabel_0"));
    REQUIRE(first);
    CHECK(first->property("checked").toBool());
    CHECK(first->property("enabled").toBool());
    CHECK(labelMenu->property("count").toInt() == 18); // sixteen names, a separator, Select Label Group
    spin(100);
    p.settle();
    p.shot(QRectF(0, 0, 1000, 760), dark ? QStringLiteral("timeline-a4-edit-label-menu.png")
                                         : QStringLiteral("timeline-a4-edit-label-menu-light.png"));
    QMetaObject::invokeMethod(labelMenu, "close");
    QMetaObject::invokeMethod(editMenu, "close");
    checkNoMessages();
}

// ---- 077: colour-label timeline tracks ----------------------------------------------------------------------------------

TEST_CASE("A track's label colour: header bar, lane wash, mixer colour and one undo step (card 077)",
          "[app][qml][timeline][tracks][label][a4][screenshot]") {
    bool dark = true;
    SECTION("dark") { dark = true; }
    SECTION("light") { dark = false; }
    resetSettings();
    clearMessages();
    ShellProject p(R"(
        V2     |Bbbbbbbbbbbbbbbbbbbbbbbb|
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaa|
        A2     |Cccccccccccc............|
    )", dark);
    ProjectController& c = *p.controller;
    const QString v1 = trackKey(p.tl.track("V1"));
    const QString a1 = trackKey(p.tl.track("A1"));
    const QString a2 = trackKey(p.tl.track("A2"));
    TrackListModel* tracks = c.tracks();
    const auto roleOf = [&](const QString& track, int role) {
        for (int row = 0; row < tracks->rowCount(); ++row) {
            if (tracks->data(tracks->index(row), TrackListModel::TrackIdRole).toString() == track)
                return tracks->data(tracks->index(row), role).toInt();
        }
        return -99;
    };
    auto* view = qobject_cast<TimelineView*>(p.item(QStringLiteral("timelineView")));
    REQUIRE(view);
    view->setPixelsPerSecond(60);
    view->setScrollX(0);
    p.settle();

    // The lane colour at the right end of the lanes, where there are no clips (video and audio lanes alike).
    const auto laneAt = [&](const char* label) {
        const QImage image = p.shell.grab();
        REQUIRE_FALSE(image.isNull());
        for (const TimelineView::Row& row : view->rows()) {
            if (row.track->id == p.tl.track(label))
                return pixelAt(image, view, QPointF(view->width() - 12, row.y - view->scrollY() + 3), image.devicePixelRatio());
        }
        FAIL("no lane " << label);
        return QColor();
    };
    const QColor before = laneAt("V1");
    CHECK(sameColor(before, laneAt("A1"), 3)); // plain lanes look alike
    CHECK_FALSE(p.item(QStringLiteral("trackLabelStripe_V1"))->isVisible());
    CHECK(roleOf(v1, TrackListModel::LabelIndexRole) == -1);
    const int defaultMixerColor = roleOf(v1, TrackListModel::ColorIndexRole);

    // The header menu: Label with the names, No Label, the current one checked.
    auto* library = p.singleton<LibraryBridge>("Library");
    const QStringList names = library->labelNames();
    QQuickItem* stripe = p.item(QStringLiteral("trackLabelStripe_V1"));
    REQUIRE(stripe);
    QQuickItem* header = stripe->parentItem();
    REQUIRE(header);
    REQUIRE(QMetaObject::invokeMethod(header, "openMenu", Q_ARG(QVariant, QVariant::fromValue(static_cast<QObject*>(header))),
                                      Q_ARG(QVariant, QPointF(10, 10))));
    QObject* menu = nullptr;
    REQUIRE(waitFor([&] {
        menu = header->findChild<QObject*>(QStringLiteral("trackHeaderMenu"));
        return ShellProject::opened(menu);
    }, 5000));
    for (int i = 0; i < 16; ++i) {
        QObject* row = menu->findChild<QObject*>(QStringLiteral("trackMenuLabel_%1").arg(i));
        INFO(i);
        REQUIRE(row);
        CHECK(row->property("text").toString() == names[i]);
        CHECK_FALSE(row->property("checked").toBool());
    }
    QObject* none = menu->findChild<QObject*>(QStringLiteral("trackMenuLabelNone"));
    REQUIRE(none);
    CHECK(none->property("checked").toBool());

    const std::size_t steps = c.session().undoCount();
    QObject* row4 = menu->findChild<QObject*>(QStringLiteral("trackMenuLabel_4"));
    REQUIRE(row4);
    REQUIRE(QMetaObject::invokeMethod(row4, "triggered"));
    spin(100);
    CHECK(c.session().undoCount() == steps + 1);
    CHECK(c.undoText() == QStringLiteral("Undo Track Color"));
    CHECK(p.sequence().findTrack(p.tl.track("V1"))->colorIndex == 4);
    CHECK(p.sequence().findTrack(p.tl.track("A1"))->colorIndex == -1);
    CHECK(roleOf(v1, TrackListModel::LabelIndexRole) == 4);
    CHECK(roleOf(v1, TrackListModel::ColorIndexRole) == 4); // the mixer strip and the header chip use it
    CHECK(roleOf(a1, TrackListModel::LabelIndexRole) == -1);
    CHECK(c.trackInfo(v1).value(QStringLiteral("colorIndex")).toInt() == 4);
    QMetaObject::invokeMethod(menu, "close");
    REQUIRE(waitFor([&] { return !ShellProject::opened(menu); }, 5000));
    // Opened again, the menu checks the track's label.
    REQUIRE(QMetaObject::invokeMethod(header, "openMenu", Q_ARG(QVariant, QVariant::fromValue(static_cast<QObject*>(header))),
                                      Q_ARG(QVariant, QPointF(10, 10))));
    REQUIRE(waitFor([&] { return ShellProject::opened(menu); }, 5000));
    CHECK(menu->findChild<QObject*>(QStringLiteral("trackMenuLabel_4"))->property("checked").toBool());
    CHECK_FALSE(menu->findChild<QObject*>(QStringLiteral("trackMenuLabelNone"))->property("checked").toBool());
    // The Label row opens its submenu from the keyboard (screenshot: the sixteen names with their colours).
    QObject* labelSubmenu = menu->findChild<QObject*>(QStringLiteral("trackMenuLabel"));
    REQUIRE(labelSubmenu);
    REQUIRE(openSubmenuByKey(p, menu, labelSubmenu));
    spin(100);
    p.settle();
    p.shot(QRectF(0, 0, 900, 900), dark ? QStringLiteral("timeline-a4-track-label-menu.png")
                                        : QStringLiteral("timeline-a4-track-label-menu-light.png"));
    QMetaObject::invokeMethod(labelSubmenu, "close");
    QMetaObject::invokeMethod(menu, "close");
    REQUIRE(waitFor([&] { return !ShellProject::opened(menu); }, 5000));

    // The header's bar has the label's colour, and the lane is washed with it.
    p.settle();
    CHECK(stripe->isVisible());
    CHECK(sameColor(stripe->property("color").value<QColor>(), p.themeLabel(4), 2));
    const QColor washed = laneAt("V1");
    const QColor plain = laneAt("A1");
    const QColor label = p.themeLabel(4);
    CHECK_FALSE(sameColor(washed, plain, 6));
    CHECK(sameColor(plain, before, 3));
    // Between the lane's colour and the label's, a little towards it.
    const auto between = [](int a, int b, int x) { return x >= std::min(a, b) - 2 && x <= std::max(a, b) + 2; };
    CHECK(between(plain.red(), label.red(), washed.red()));
    CHECK(between(plain.green(), label.green(), washed.green()));
    CHECK(between(plain.blue(), label.blue(), washed.blue()));

    // More colours for the screenshot; each is its own undo step, the last one cleared with No Label.
    CHECK(c.setTrackColor(a1, 10));
    CHECK(c.setTrackColor(a2, 13));
    CHECK(c.setTrackColor(trackKey(p.tl.track("V2")), 1));
    p.settle();
    QQuickItem* panel = p.item(QStringLiteral("timelinePanel"));
    REQUIRE(panel);
    p.shot(sceneRect(panel), dark ? QStringLiteral("timeline-a4-track-colors.png")
                                  : QStringLiteral("timeline-a4-track-colors-light.png"));
    CHECK(c.setTrackColor(a2, -1));
    CHECK(roleOf(a2, TrackListModel::LabelIndexRole) == -1);
    CHECK(c.setTrackColor(v1, 4)); // already 4: no empty step
    const std::size_t after = c.session().undoCount();
    CHECK(c.setTrackColor(v1, 4));
    CHECK(c.session().undoCount() == after);
    CHECK_FALSE(c.setTrackColor(v1, 16));
    CHECK_FALSE(c.lastError().isEmpty());
    CHECK_FALSE(c.setTrackColor(QStringLiteral("t99999"), 3));

    // Undo walks back through the colours; with the label gone the track is shown in its default colour again.
    while (c.session().undoCount() > steps)
        REQUIRE(c.undo());
    CHECK(roleOf(v1, TrackListModel::LabelIndexRole) == -1);
    CHECK(roleOf(v1, TrackListModel::ColorIndexRole) == defaultMixerColor);
    p.settle();
    CHECK_FALSE(stripe->isVisible());
    CHECK(sameColor(laneAt("V1"), before, 3));
    checkNoMessages();
}

// ---- 008: the Menu key opens the timeline's context menus ---------------------------------------------------------------

TEST_CASE("TimelineView.menuAnchor names the selected transition, gap or clip and scrolls it into view (card 008)",
          "[app][timeline][keys][a4]") {
    const std::string row = "AaaaaaaaaaB5bbbbbbbb..Cccc" + std::string(50, '.') + "Dddd"; // D starts at frame 76
    Rig rig("V1 tgt |" + row + "|\nA1 tgt |" + row + "|", 24.0);
    ProjectState withTransition = rig.tl.state;
    edit::TransitionSpec spec;
    spec.outgoing = rig.tl.clip("V1@0");
    spec.incoming = rig.tl.clip("V1@10");
    spec.before = f(2);
    spec.after = f(2);
    spec.effect = "crossDissolve";
    withTransition = ok(edit::addTransition(withTransition, rig.tl.seq, rig.tl.track("V1"), spec)).state;
    rig.controller.resetProject(withTransition, rig.tl.seq);
    rig.harness.frame();
    TimelineView& view = *rig.view;
    ProjectController& controller = rig.controller;
    view.setScrollX(0);

    // Nothing selected: no anchor.
    CHECK(view.menuAnchor().isEmpty());

    // A clip: its key and a point inside the view.
    controller.selectClip(clipKey(rig.tl.clip("V1@0")));
    QVariantMap anchor = view.menuAnchor();
    CHECK(anchor.value(QStringLiteral("kind")).toString() == QStringLiteral("clip"));
    CHECK(anchor.value(QStringLiteral("id")).toString() == QString(clipKey(controller.primaryClip())));
    CHECK(anchor.value(QStringLiteral("x")).toDouble() >= 0);
    CHECK(anchor.value(QStringLiteral("x")).toDouble() <= view.width());
    CHECK(anchor.value(QStringLiteral("y")).toDouble() >= 0);
    CHECK(anchor.value(QStringLiteral("y")).toDouble() <= view.height());

    // A clip out of view (24 px a frame, the closest zoom: frame 76 is at x = 1824 of 900) is scrolled into view first.
    controller.selectClip(clipKey(rig.tl.clip("V1@76")));
    anchor = view.menuAnchor();
    CHECK(anchor.value(QStringLiteral("kind")).toString() == QStringLiteral("clip"));
    CHECK(anchor.value(QStringLiteral("id")).toString() == QString(clipKey(controller.primaryClip())));
    CHECK(view.scrollX() > 0);
    CHECK(anchor.value(QStringLiteral("x")).toDouble() >= 0);
    CHECK(anchor.value(QStringLiteral("x")).toDouble() <= view.width());

    // A gap (it replaces the clips in the selection), then a transition (it replaces the gap).
    REQUIRE(controller.selectGapAt(rig.tl.track("V1"), f(20)));
    anchor = view.menuAnchor();
    CHECK(anchor.value(QStringLiteral("kind")).toString() == QStringLiteral("gap"));
    CHECK(anchor.value(QStringLiteral("id")).toString() == QString(trackKey(rig.tl.track("V1"))));
    CHECK(anchor.value(QStringLiteral("x")).toDouble() >= 0);
    CHECK(anchor.value(QStringLiteral("x")).toDouble() <= view.width());
    const TransitionId transitionId = controller.currentSequence()->findTrack(rig.tl.track("V1"))->transitions.front().id;
    controller.selectTransition(transitionKey(transitionId));
    anchor = view.menuAnchor();
    CHECK(anchor.value(QStringLiteral("kind")).toString() == QStringLiteral("transition"));
    CHECK(anchor.value(QStringLiteral("id")).toString() == QString(transitionKey(transitionId)));
    CHECK(anchor.value(QStringLiteral("x")).toDouble() >= 0);
    CHECK(anchor.value(QStringLiteral("x")).toDouble() <= view.width());
    CHECK(view.scrollX() < 500); // it came back for the transition near the start
}

TEST_CASE("The Menu key and Shift+F10 open the menu of the selected clip, gap or transition (card 008)",
          "[app][qml][timeline][keys][a4]") {
    resetSettings();
    clearMessages();
    ShellProject p(R"(
        V1 tgt |AaaaaaaaaaB5bbbbbbbb..Cccc|
        A1 tgt |AaaaaaaaaaB5bbbbbbbb..Cccc|
    )");
    ProjectController& c = *p.controller;
    ProjectState state = p.tl.state;
    edit::TransitionSpec spec;
    spec.outgoing = p.tl.clip("V1@0");
    spec.incoming = p.tl.clip("V1@10");
    spec.before = f(2);
    spec.after = f(2);
    spec.effect = "crossDissolve";
    state = ok(edit::addTransition(state, p.tl.seq, p.tl.track("V1"), spec)).state;
    c.resetProject(state, p.tl.seq);
    spin(100);

    QQuickItem* lanes = p.item(QStringLiteral("timelineLanesArea"));
    REQUIRE(lanes);
    QObject* clipMenu = p.object(QStringLiteral("clipContextMenu"));
    QObject* gapMenu = p.object(QStringLiteral("gapContextMenu"));
    QObject* transitionMenu = p.object(QStringLiteral("transitionContextMenu"));
    REQUIRE(clipMenu);
    REQUIRE(gapMenu);
    REQUIRE(transitionMenu);
    QQuickWindow* window = p.shell.window.get();
    // The track headers' menus (each header makes its own on first use; they hang under the header).
    const auto trackMenus = [&] {
        QList<QObject*> menus;
        for (const char* label : {"V1", "A1"}) {
            QQuickItem* stripe = p.item(QStringLiteral("trackLabelStripe_%1").arg(QLatin1String(label)));
            if (stripe && stripe->parentItem())
                menus += stripe->parentItem()->findChildren<QObject*>(QStringLiteral("trackHeaderMenu"));
        }
        return menus;
    };
    const auto closeAll = [&] {
        for (QObject* menu : {clipMenu, gapMenu, transitionMenu})
            QMetaObject::invokeMethod(menu, "close");
        for (QObject* menu : trackMenus())
            QMetaObject::invokeMethod(menu, "close");
        spin(30);
    };
    const auto anyTrackMenu = [&] {
        for (QObject* menu : trackMenus()) {
            if (ShellProject::opened(menu))
                return true;
        }
        return false;
    };

    // A selected clip: the Menu key opens the clip menu for it.
    lanes->forceActiveFocus(Qt::TabFocusReason);
    spin(30);
    c.selectClip(clipKey(p.tl.clip("V1@22")));
    tap(window, Qt::Key_Menu);
    REQUIRE(waitFor([&] { return ShellProject::opened(clipMenu); }, 5000));
    CHECK(clipMenu->property("clipId").toString() == QString(clipKey(p.tl.clip("V1@22"))));
    closeAll();
    REQUIRE(waitFor([&] { return !ShellProject::opened(clipMenu); }, 5000));

    // Shift+F10 does the same.
    lanes->forceActiveFocus(Qt::TabFocusReason);
    tap(window, Qt::Key_F10, Qt::ShiftModifier);
    REQUIRE(waitFor([&] { return ShellProject::opened(clipMenu); }, 5000));
    closeAll();

    // A selected gap: the gap menu.
    REQUIRE(c.selectGapAt(p.tl.track("V1"), f(20)));
    lanes->forceActiveFocus(Qt::TabFocusReason);
    tap(window, Qt::Key_Menu);
    REQUIRE(waitFor([&] { return ShellProject::opened(gapMenu); }, 5000));
    CHECK_FALSE(ShellProject::opened(clipMenu));
    closeAll();

    // A selected transition: the transition menu.
    const auto transition = c.currentSequence()->findTrack(p.tl.track("V1"))->transitions.front().id;
    c.selectTransition(transitionKey(transition));
    lanes->forceActiveFocus(Qt::TabFocusReason);
    tap(window, Qt::Key_F10, Qt::ShiftModifier);
    REQUIRE(waitFor([&] { return ShellProject::opened(transitionMenu); }, 5000));
    closeAll();

    // The list of clips the Tab key reaches (screen readers): the same key, the same menu.
    QQuickItem* access = p.item(QStringLiteral("timelineClipAccess"));
    REQUIRE(access);
    c.selectClip(clipKey(p.tl.clip("V1@0")));
    access->forceActiveFocus(Qt::TabFocusReason);
    spin(30);
    tap(window, Qt::Key_Menu);
    REQUIRE(waitFor([&] { return ShellProject::opened(clipMenu); }, 5000));
    closeAll();

    // Nothing selected: the track menu of the targeted track's header.
    c.clearSelection();
    c.clearGapSelection();
    c.selectTransition(QString());
    auto* view = qobject_cast<TimelineView*>(p.item(QStringLiteral("timelineView")));
    REQUIRE(view);
    CHECK(view->menuAnchor().isEmpty());
    QQuickItem* panel = p.item(QStringLiteral("timelinePanel"));
    REQUIRE(panel);
    bool menuOpened = false;
    REQUIRE(QMetaObject::invokeMethod(panel, "openContextMenu", Q_RETURN_ARG(bool, menuOpened)));
    CHECK(menuOpened);
    REQUIRE(waitFor(anyTrackMenu, 5000));
    CHECK_FALSE(ShellProject::opened(clipMenu));
    closeAll();
    REQUIRE(waitFor([&] { return !anyTrackMenu(); }, 5000));
    // And from the key. (A closing menu gives the focus back to the clip list, which selects its first clip: clear that.)
    lanes->forceActiveFocus(Qt::TabFocusReason);
    spin(100);
    c.clearSelection();
    c.clearGapSelection();
    c.selectTransition(QString());
    CHECK(view->menuAnchor().isEmpty());
    tap(window, Qt::Key_Menu);
    REQUIRE(waitFor(anyTrackMenu, 5000));
    closeAll();
    checkNoMessages();
}

TEST_CASE("The Timeline list opens the clip menu of the current row from the keyboard (card 008)",
          "[app][qml][timeline][keys][a4]") {
    resetSettings();
    clearMessages();
    ShellProject p(R"(
        V1 tgt |AaaaaaaaaaB5bbbbbbbb..Cccc|
        A1 tgt |AaaaaaaaaaB5bbbbbbbb..Cccc|
    )");
    QQuickItem* panel = p.item(QStringLiteral("timelinePanel"));
    REQUIRE(panel);
    panel->setProperty("listMode", true);
    QQuickItem* list = p.item(QStringLiteral("timelineListView"));
    REQUIRE(list);
    REQUIRE(waitFor([&] { return list->hasActiveFocus(); }, 5000));
    p.controller->clearSelection();
    list->setProperty("currentIndex", 1);
    QObject* clipMenu = p.object(QStringLiteral("clipContextMenu"));
    REQUIRE(clipMenu);
    tap(p.shell.window.get(), Qt::Key_Menu);
    REQUIRE(waitFor([&] { return ShellProject::opened(clipMenu); }, 5000));
    // The row's clip was selected first and is the menu's clip.
    const QString menuClip = clipMenu->property("clipId").toString();
    CHECK(menuClip != QString());
    CHECK(p.controller->selectedClips().contains(menuClip));
    QMetaObject::invokeMethod(clipMenu, "close");
    panel->setProperty("listMode", false);
    checkNoMessages();
}

// ---- 127: slip with the trim tool ---------------------------------------------------------------------------------------

namespace {

constexpr std::string_view kSlipPicture = R"(
    V1 tgt |B20bbbbbbb..Aaaa|
    A1 tgt |B20bbbbbbb..Aaaa|
)";

} // namespace

TEST_CASE("Shift-click on a clip's other edge selects both edges and slips its content in trim mode (card 127)",
          "[app][timeline][trim][slip][a4]") {
    Project p(kSlipPicture);
    ProjectController& c = p.controller;
    TrimBridge trim;
    trim.setController(&c);
    const QString b = p.key("V1@0");

    trim.selectEditPoint(b, QStringLiteral("head"), QStringLiteral("regular"), 0);
    REQUIRE(trim.active());
    CHECK(trim.type() == QStringLiteral("regular"));
    // The other edge, with Shift: both edges of the clip, and the slip type.
    trim.selectEditPoint(b, QStringLiteral("tail"), QStringLiteral("regular"), Qt::ShiftModifier);
    CHECK(trim.type() == QStringLiteral("slip"));
    CHECK(trim.viewOpen()); // the Trim view shows the clip's first and last frame
    int edgesOfB = 0;
    for (const QVariant& point : trim.points()) {
        if (point.toMap().value(QStringLiteral("clip")).toString() == b)
            ++edgesOfB;
    }
    CHECK(edgesOfB == 2);

    // Trim Forward / Backward slip the source (+ = later frames); the clip stays where it is, its partner slips with it.
    const std::size_t steps = p.undoSteps();
    REQUIRE(trim.trim(3));
    CHECK(p.undoSteps() == steps + 1);
    CHECK(c.undoText() == QStringLiteral("Undo Slip"));
    CHECK(p.clip("V1@0").sourceIn == f(23));
    CHECK(p.clip("V1@0").start() == f(0));
    CHECK(p.clip("V1@0").duration() == f(10));
    CHECK(p.clip("A1@0").sourceIn == f(23));
    CHECK(trim.outgoingOffset() == 3);
    CHECK(trim.incomingOffset() == 3);
    REQUIRE(trim.trim(-5));
    CHECK(p.clip("V1@0").sourceIn == f(18));
    CHECK(trim.outgoingOffset() == -2);
    // Everything else is where it was.
    CHECK(p.clip("V1@12").start() == f(12));
    CHECK(p.clip("V1@12").sourceIn == Ticks{});
    CHECK(trim.active());
    CHECK(trim.type() == QStringLiteral("slip"));
    // One undo step each.
    REQUIRE(c.undo());
    CHECK(p.clip("V1@0").sourceIn == f(23));
    REQUIRE(c.undo());
    CHECK(p.clip("V1@0").sourceIn == f(20));

    // A slip has no cut to bring to the playhead; Toggle Trim Type leaves it for the roll.
    c.setPlayhead(f(5));
    CHECK_FALSE(trim.extendToPlayhead());
    trim.toggleType();
    CHECK(trim.type() == QStringLiteral("roll"));
}

TEST_CASE("A slip is clamped to the media and refused when there is no room (card 127)", "[app][timeline][trim][slip][a4]") {
    Project p(kSlipPicture);
    TrimBridge trim;
    trim.setController(&p.controller);
    REQUIRE(p.controller.selection().empty());
    p.controller.setPlayhead(f(3));
    REQUIRE(trim.slipEdit());
    // Back as far as the media starts: 20 frames, then no more.
    REQUIRE(trim.trim(-1000));
    CHECK(p.clip("V1@0").sourceIn == Ticks{});
    CHECK(trim.outgoingOffset() == -20);
    CHECK_FALSE(trim.trim(-1));
    CHECK(p.clip("V1@0").sourceIn == Ticks{});
    // Forward as far as the media goes (1000 frames, the clip is 10 long), then no more.
    REQUIRE(trim.trim(5000));
    CHECK(p.clip("V1@0").sourceIn == f(990));
    CHECK(p.clip("V1@0").sourceOut() <= f(1000));
    CHECK_FALSE(trim.trim(1));
}

TEST_CASE("Slip Trim puts the selected clips, else the clips under the playhead, in slip mode (card 127)",
          "[app][timeline][trim][slip][a4]") {
    Project p(kSlipPicture);
    ProjectController& c = p.controller;
    TrimBridge trim;
    trim.setController(&c);

    // Nothing selected, the playhead in a gap: there is no clip to slip.
    c.setPlayhead(f(10));
    CHECK_FALSE(trim.slipEdit());
    CHECK_FALSE(trim.active());
    CHECK_FALSE(trim.runCommand(QStringLiteral("sequence.slipTrim")));

    // The clips under the playhead on the targeted tracks.
    c.setPlayhead(f(4));
    REQUIRE(trim.slipEdit());
    CHECK(trim.active());
    CHECK(trim.type() == QStringLiteral("slip"));
    CHECK(trim.viewOpen());
    CHECK(trim.points().size() == 4); // both edges of the picture and of the sound
    // The Trim view's two sides are the clip's last frame (outgoing) and first frame (incoming), in source time.
    CHECK(trim.outgoingName() == QStringLiteral("B"));
    CHECK(trim.incomingName() == QStringLiteral("B"));
    CHECK(trim.incomingText() == c.formatTimecode(f(20)));
    CHECK(trim.outgoingText() == c.formatTimecode(f(29)));
    REQUIRE(trim.trim(2));
    CHECK(trim.incomingText() == c.formatTimecode(f(22)));
    CHECK(trim.outgoingText() == c.formatTimecode(f(31)));
    trim.clear();
    CHECK_FALSE(trim.active());

    // The selected clips win over the playhead; selecting a clip again leaves trim mode (as for any edit point).
    c.selectClip(p.key("V1@12"), ProjectController::Replace, false);
    REQUIRE(trim.runCommand(QStringLiteral("sequence.slipTrim")));
    CHECK(trim.points().size() == 2);
    CHECK(trim.incomingName() == QStringLiteral("A"));
    CHECK(trim.incomingText() == c.formatTimecode(Ticks{}));
    REQUIRE(trim.trim(4));
    CHECK(p.clip("V1@12").sourceIn == f(4));
    CHECK(p.clip("A1@12").sourceIn == f(4)); // Linked Selection is on: the picture's partner slips with it
}

TEST_CASE("With Linked Selection off a slip moves only the clips whose edges are selected (card 127)",
          "[app][timeline][trim][slip][a4]") {
    Project p(kSlipPicture);
    TrimBridge trim;
    trim.setController(&p.controller);
    trim.setLinkedSelection(false);
    const QString b = p.key("V1@0");
    trim.selectEditPoint(b, QStringLiteral("head"), QStringLiteral("regular"), 0);
    trim.selectEditPoint(b, QStringLiteral("tail"), QStringLiteral("regular"), Qt::ShiftModifier);
    REQUIRE(trim.type() == QStringLiteral("slip"));
    CHECK(trim.points().size() == 2);
    REQUIRE(trim.trim(3));
    CHECK(p.clip("V1@0").sourceIn == f(23));
    CHECK(p.clip("A1@0").sourceIn == f(20)); // the sound stayed
}

TEST_CASE("Slip Trim in the shell: menu row, Trim view tabs and labels, buttons that slip (card 127)",
          "[app][qml][timeline][trim][slip][a4][screenshot]") {
    bool dark = true;
    SECTION("dark") { dark = true; }
    SECTION("light") { dark = false; }
    resetSettings();
    clearMessages();
    ShellProject p(kSlipPicture, dark);
    ProjectController& c = *p.controller;
    auto* trim = p.singleton<TrimBridge>("Trim");

    // Nothing to slip: a toast says so, trim mode stays off.
    c.setPlayhead(f(10));
    REQUIRE(p.run("sequence.slipTrim"));
    spin(100);
    CHECK_FALSE(trim->active());

    c.selectClip(clipKey(p.tl.clip("V1@0")));
    REQUIRE(p.run("sequence.slipTrim"));
    REQUIRE(waitFor([&] { return trim->active(); }, 5000));
    CHECK(trim->type() == QStringLiteral("slip"));
    QQuickItem* view = nullptr;
    REQUIRE(waitFor([&] {
        view = p.item(QStringLiteral("trimView"));
        return view && view->isVisible();
    }, 5000));
    QQuickItem* tabs = findNamed(view, QStringLiteral("trimTypeTabs"));
    REQUIRE(tabs);
    CHECK(tabs->property("currentIndex").toInt() == 3);
    QQuickItem* outgoing = findNamed(view, QStringLiteral("trimOutgoing"));
    QQuickItem* incoming = findNamed(view, QStringLiteral("trimIncoming"));
    REQUIRE(outgoing);
    REQUIRE(incoming);
    CHECK(outgoing->property("label").toString() == QStringLiteral("Last frame"));
    CHECK(incoming->property("label").toString() == QStringLiteral("First frame"));
    // The first frame is on the left (the clip's In), the last on the right.
    CHECK(sceneRect(incoming).center().x() < sceneRect(outgoing).center().x());

    // The +1 button slips the content one frame later.
    QQuickItem* forward = findNamed(view, QStringLiteral("trimForward"));
    REQUIRE(forward);
    const std::size_t steps = c.session().undoCount();
    REQUIRE(QMetaObject::invokeMethod(forward, "clicked"));
    spin(100);
    CHECK(c.session().undoCount() == steps + 1);
    CHECK(p.sequence().findClip(p.tl.clip("V1@0"))->clip().sourceIn == f(21));
    CHECK(p.sequence().findClip(p.tl.clip("V1@0"))->clip().start() == f(0));
    CHECK(trim->incomingOffset() == 1);
    p.settle();
    p.shot(QRectF(0, 0, kShotWidth, kShotHeight),
           dark ? QStringLiteral("timeline-a4-slip-trim.png") : QStringLiteral("timeline-a4-slip-trim-light.png"));

    // The Regular / Ripple / Roll tabs give the edit points back to the ordinary trims.
    REQUIRE(QMetaObject::invokeMethod(tabs, "activated", Q_ARG(int, 2)));
    spin(50);
    CHECK(trim->type() == QStringLiteral("roll"));
    trim->clear();
    checkNoMessages();
}
