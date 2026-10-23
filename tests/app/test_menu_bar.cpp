// UX rework (stream S): the title bar's menu bar (panels/menubar/, AppMenuBar) that replaced the left rail.
//   * Every menu is there in Premiere's order, every command row reaches a handler, and one row of each menu runs
//     (once the menu has closed, with the focus back where it was: a focus-routed command acts on the timeline).
//   * Alt+letter opens a menu with its first row highlighted; Left / Right move between the menus; Esc gives the focus
//     back.
//   * Short of room the titles fold into one Menu button with the same menus as submenus.
//   * A Help row runs after its menu closed and its dialog keeps the focus.
//   * The Window menu and the palette's shell rows leave out Generate while AI is off; the shell rows open panels,
//     workspaces and recent projects.
//   * A project another launch hands over (AppShell.openRequested) opens, after the unsaved-changes prompt; the project
//     open now is not read again (a hand-over, Open Recent, the palette), and a hand-over while the prompt asks about
//     another action waits its turn.
//   * Alt+letter leaves a chord the keymap binds to its command and is off on macOS.
//   * A row's state follows where it will act: the Source Monitor's marks while it has the focus, a focused text
//     field's text for Edit's text rows (which then act on the text, not on the timeline).

#include "ShellFixture.h"

#include "AppShell.h"
#include "BridgeIds.h"
#include "CommandPalette.h"
#include "KeymapDispatcher.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "SourceMonitorBridge.h"

#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QMetaObject>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QUrl>
#include <qpa/qwindowsysteminterface.h>

#include <filesystem>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
namespace fs = std::filesystem;

namespace {

const char* const kMenuKeys[] = {"file", "edit", "clip", "sequence", "markers", "graphics", "view", "window", "help"};
const char* const kMenuNames[] = {"fileMenu",     "editMenu",     "clipMenu", "sequenceMenuBar", "markersMenu",
                                  "graphicsMenu", "viewMenu",     "windowMenu", "helpMenu"};
const char* const kTitles[] = {"File", "Edit", "Clip", "Sequence", "Markers", "Graphics", "View", "Window", "Help"};

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

QVariant evaluate(const Shell& shell, const QString& code) {
    QQmlExpression expression(qmlContext(shell.window.get()), shell.window.get(), code);
    const QVariant result = expression.evaluate();
    INFO(code.toStdString() << ": " << expression.error().toString().toStdString());
    REQUIRE_FALSE(expression.hasError());
    return result;
}

void tap(QQuickWindow* window, int key, Qt::KeyboardModifiers mods = {}) {
    QKeyEvent press(QEvent::KeyPress, key, mods);
    QCoreApplication::sendEvent(window, &press);
    QKeyEvent release(QEvent::KeyRelease, key, mods);
    QCoreApplication::sendEvent(window, &release);
}

// Through the window system, as the platform delivers the pointer (presses on the title bar then reach its buttons,
// not the window-drag surface under them).
void mouse(QQuickWindow* window, QEvent::Type type, QPointF scenePos, Qt::MouseButtons buttons) {
    const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(
        window, scenePos, window->mapToGlobal(scenePos), buttons, button, type);
}

/// A left click at `scenePos` (the pointer moves there first).
void clickAt(QQuickWindow* window, QPointF scenePos) {
    mouse(window, QEvent::MouseMove, scenePos, Qt::NoButton);
    mouse(window, QEvent::MouseButtonPress, scenePos, Qt::LeftButton);
    mouse(window, QEvent::MouseButtonRelease, scenePos, Qt::NoButton);
}

QPointF centreOf(const QQuickItem* item) {
    return item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
}

bool isInside(const QQuickItem* item, const QQuickItem* ancestor) {
    for (const QQuickItem* p = item; p; p = p->parentItem()) {
        if (p == ancestor)
            return true;
    }
    return false;
}

/// The popup item of a menu or dialog (its contentItem's parent).
QQuickItem* popupItem(QObject* popup) {
    auto* content = popup->property("contentItem").value<QQuickItem*>();
    return content ? content->parentItem() : nullptr;
}

bool opened(QObject* popup) {
    return popup && popup->property("opened").toBool();
}

bool shown(QObject* popup) {
    return popup && popup->property("visible").toBool();
}

/// The editor at 1600x960 (the menu titles show), motion off.
struct MenuShell {
    Shell shell;
    QQuickWindow* w = nullptr;
    QObject* bar = nullptr;
    ProjectController* controller = nullptr;

    MenuShell() {
        REQUIRE(shell.window != nullptr);
        INFO(join(shell.errors));
        REQUIRE(shell.errors.isEmpty());
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        w = shell.window.get();
        bar = shell.object("appMenuBar");
        REQUIRE(bar != nullptr);
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        REQUIRE(controller != nullptr);
        spin(50);
    }

    QObject* menu(int index) const {
        QObject* m = shell.object(kMenuNames[index]);
        REQUIRE(m != nullptr);
        return m;
    }
    /// Opens menu `index` the way a click on its title does; returns it.
    QObject* open(int index) const {
        REQUIRE(QMetaObject::invokeMethod(bar, "open", Q_ARG(int, index), Q_ARG(bool, false)));
        QObject* m = menu(index);
        REQUIRE(waitFor([&] { return opened(m); }));
        return m;
    }
    /// The row `item` of the menu `menuObject` (or of a submenu in it).
    QObject* row(QObject* menuObject, const char* item) const {
        QObject* found = menuObject->findChild<QObject*>(QLatin1String(item));
        INFO(item);
        REQUIRE(found != nullptr);
        return found;
    }
    bool enabled(QObject* menuObject, const char* item) const { return row(menuObject, item)->property("enabled").toBool(); }
    /// Closes the open menu `menuObject` without choosing anything.
    void dismiss(QObject* menuObject) const {
        REQUIRE(QMetaObject::invokeMethod(menuObject, "close"));
        REQUIRE(waitFor([&] { return !shown(menuObject); }));
        spin(30);
    }
    /// Chooses the row `item` of the open menu `menuObject` (a click) and waits for the menus to close.
    void choose(QObject* menuObject, const char* item) const {
        QObject* row = menuObject->findChild<QObject*>(QLatin1String(item));
        INFO(item);
        REQUIRE(row != nullptr);
        CHECK(row->property("enabled").toBool());
        REQUIRE(QMetaObject::invokeMethod(row, "clicked"));
        REQUIRE(waitFor([&] { return !shown(menuObject); }));
        spin(30); // the action runs once the menus have closed
    }
};

} // namespace

TEST_CASE("Menu bar: every menu is there in Premiere's order and one row of each runs", "[app][menubar][qml]") {
    resetSettings();
    clearMessages();
    MenuShell s;
    auto* palette = s.shell.engine.singletonInstance<CommandPalette*>("Wabcut", "CommandPalette");
    REQUIRE(palette != nullptr);
    INFO("menu titles " << s.bar->property("expandedWidth").toDouble() << " px, room "
                        << s.shell.item("appMenuBar")->parentItem()->parentItem()->property("menuRoom").toDouble());
    CHECK_FALSE(s.bar->property("collapsed").toBool());

    // The titles, left to right after the brand, each with its menu; every command row reaches a handler.
    QQuickItem* content = s.w->contentItem();
    double lastX = -1;
    int commandRows = 0;
    for (int i = 0; i < 9; ++i) {
        INFO(kMenuKeys[i]);
        QQuickItem* title = findNamed(content, QStringLiteral("menuBar_%1").arg(QLatin1String(kMenuKeys[i])));
        REQUIRE(title != nullptr);
        CHECK(title->isVisible());
        CHECK(title->property("text").toString() == QLatin1String(kTitles[i]));
        const double x = title->mapToScene(QPointF(0, 0)).x();
        CHECK(x > lastX);
        lastX = x;
        QObject* menu = s.menu(i);
        CHECK(menu->property("title").toString() == QLatin1String(kTitles[i]));
        for (QObject* row : menu->findChildren<QObject*>()) {
            if (!row->inherits("QQuickMenuItem"))
                continue;
            const QString command = row->property("command").toString();
            if (command.isEmpty())
                continue;
            ++commandRows;
            const QVariantMap state = palette->commandState(command);
            INFO(row->objectName().toStdString() << " " << command.toStdString() << ": "
                                                 << state.value(QStringLiteral("reason")).toString().toStdString());
            CHECK(state.value(QStringLiteral("known")).toBool());
            CHECK(state.value(QStringLiteral("reason")).toString() != QStringLiteral("Not available yet"));
        }
    }
    CHECK(commandRows > 100);
    // The Settings window's QML Ctrl+, is gone: the keymap's edit.preferences has the key.
    CHECK(s.shell.object("editMenuSettings")->property("command").toString() == QStringLiteral("edit.preferences"));

    // Edit > Deselect All.
    const auto sequence = s.controller->currentSequence();
    s.controller->selectClipById(sequence->videoTracks[0]->clips.front().id, ProjectController::Replace);
    REQUIRE_FALSE(s.controller->selection().empty());
    s.choose(s.open(1), "menuCommand_edit.deselectAll");
    CHECK(waitFor([&] { return s.controller->selection().empty(); }));

    // Clip > Speed/Duration… opens its dialog (after the menu closed).
    s.controller->selectClipById(sequence->videoTracks[0]->clips.front().id, ProjectController::Replace);
    s.choose(s.open(2), "menuCommand_clip.speedDuration");
    QObject* speed = s.shell.object("speedDurationDialog");
    REQUIRE(speed != nullptr);
    REQUIRE(waitFor([&] { return opened(speed); }));
    REQUIRE(QMetaObject::invokeMethod(speed, "close"));
    REQUIRE(waitFor([&] { return !shown(speed); }));

    // Sequence > Zoom In.
    QObject* view = s.shell.item("timelinePanel")->property("view").value<QObject*>();
    REQUIRE(view != nullptr);
    view->setProperty("zoomLevel", 0.3);
    const double zoom = view->property("zoomLevel").toDouble();
    s.choose(s.open(3), "menuCommand_sequence.zoomIn");
    CHECK(waitFor([&] { return view->property("zoomLevel").toDouble() > zoom; }));

    // Markers > Mark In from the Timeline: the focus comes back to the lanes before the command runs.
    QQuickItem* lanes = s.shell.item("timelineLanes");
    REQUIRE(lanes != nullptr);
    lanes->forceActiveFocus(Qt::TabFocusReason);
    REQUIRE(lanes->hasActiveFocus());
    REQUIRE(s.controller->seekToText(QStringLiteral("00:00:02:05")));
    const QString inBefore = s.controller->markInText();
    QObject* markers = s.open(4);
    CHECK_FALSE(lanes->hasActiveFocus()); // the menu has it while it is open
    s.choose(markers, "menuCommand_marker.markIn");
    CHECK(waitFor([&] { return lanes->hasActiveFocus(); }));
    CHECK(waitFor([&] { return s.controller->markInText() != inBefore; }));

    // Graphics > New Layer > Text (a submenu's row): a text layer, and the Callouts panel shows.
    s.choose(s.open(5), "menuCommand_graphics.newTextLayer");
    CHECK(waitFor([&] { return evaluate(s.shell, QStringLiteral("docking.isOpen('graphics')")).toBool(); }));

    // View > Show Rulers turns the Program Monitor's rulers on, and the row shows it.
    const bool rulers = evaluate(s.shell, QStringLiteral("Monitors.showRulers")).toBool();
    s.choose(s.open(6), "menuCommand_programMonitor.showRulers");
    CHECK(waitFor([&] { return evaluate(s.shell, QStringLiteral("Monitors.showRulers")).toBool() != rulers; }));
    CHECK(s.shell.object("menuCommand_programMonitor.showRulers")->property("checked").toBool() != rulers);
    evaluate(s.shell, QStringLiteral("Monitors.showRulers = %1").arg(rulers ? QStringLiteral("true") : QStringLiteral("false")));

    // Window > Markers: the closed panel opens (and is checked).
    CHECK_FALSE(evaluate(s.shell, QStringLiteral("docking.isOpen('markers')")).toBool());
    s.choose(s.open(7), "windowMenu_markers");
    CHECK(waitFor([&] { return evaluate(s.shell, QStringLiteral("docking.isOpen('markers')")).toBool(); }));
    CHECK(s.shell.object("windowMenu_markers")->property("checked").toBool());

    // Help > About Wabcut: its dialog, with the focus in it.
    s.choose(s.open(8), "helpItem_about");
    QObject* about = s.shell.object("aboutDialog");
    REQUIRE(about != nullptr);
    REQUIRE(waitFor([&] { return opened(about); }));
    CHECK(waitFor([&] { return isInside(s.w->activeFocusItem(), popupItem(about)); }));
    REQUIRE(QMetaObject::invokeMethod(about, "close"));
    REQUIRE(waitFor([&] { return !shown(about); }));

    // File > Close Project with only the sample project open: it starts over and Home shows.
    QObject* home = s.shell.object("homeScreen");
    REQUIRE(home != nullptr);
    CHECK_FALSE(home->property("opened").toBool());
    s.choose(s.open(0), "fileMenuCloseProject");
    // The rows above edited the sample project (the In mark, the text layer): its edits are asked about first.
    QObject* unsaved = s.shell.object("unsavedChangesDialog");
    REQUIRE(unsaved != nullptr);
    REQUIRE(waitFor([&] { return opened(unsaved) || home->property("opened").toBool(); }));
    if (opened(unsaved)) {
        REQUIRE(QMetaObject::invokeMethod(unsaved, "close"));
        REQUIRE(QMetaObject::invokeMethod(unsaved, "discardChosen"));
    }
    CHECK(waitFor([&] { return home->property("opened").toBool(); }));
    auto* project = s.shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    REQUIRE(project != nullptr);
    CHECK(project->demo());
    CHECK_FALSE(s.controller->canUndo());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Menu bar: Alt plus a letter opens a menu and Left and Right move between the menus", "[app][menubar][qml]") {
    resetSettings();
    clearMessages();
    MenuShell s;
    QQuickItem* lanes = s.shell.item("timelineLanes");
    REQUIRE(lanes != nullptr);
    lanes->forceActiveFocus(Qt::TabFocusReason);

    // Every menu has its Alt+letter (a window shortcut: the test window is never shown, so its signal stands in for
    // the key).
    const char* const letters[] = {"F", "E", "C", "S", "M", "G", "V", "W", "H"};
    for (int i = 0; i < 9; ++i) {
        QObject* key = s.shell.object(QStringLiteral("menuBarKey_%1").arg(QLatin1String(kMenuKeys[i])).toLatin1().constData());
        INFO(kMenuKeys[i]);
        REQUIRE(key != nullptr);
        CHECK(key->property("sequence").toString() == QStringLiteral("Alt+%1").arg(QLatin1String(letters[i])));
    }
    REQUIRE(QMetaObject::invokeMethod(s.shell.object("menuBarKey_edit"), "activated"));
    QObject* edit = s.menu(1);
    REQUIRE(waitFor([&] { return opened(edit); }));
    // Opened by the keyboard: its first row that can run now is highlighted and has the focus.
    const int current = edit->property("currentIndex").toInt();
    REQUIRE(current >= 0);
    QQuickItem* highlighted = nullptr;
    REQUIRE(QMetaObject::invokeMethod(edit, "itemAt", Q_RETURN_ARG(QQuickItem*, highlighted), Q_ARG(int, current)));
    REQUIRE(highlighted != nullptr);
    CHECK(highlighted->isEnabled());
    CHECK(highlighted->hasActiveFocus());
    CHECK(isInside(s.w->activeFocusItem(), popupItem(edit)));

    // Right: Clip; Left, Left: Edit, File; Left again wraps to Help.
    tap(s.w, Qt::Key_Right);
    QObject* clip = s.menu(2);
    REQUIRE(waitFor([&] { return opened(clip) && !shown(edit); }));
    CHECK(isInside(s.w->activeFocusItem(), popupItem(clip)));
    tap(s.w, Qt::Key_Left);
    REQUIRE(waitFor([&] { return opened(edit) && !shown(clip); }));
    tap(s.w, Qt::Key_Left);
    QObject* file = s.menu(0);
    REQUIRE(waitFor([&] { return opened(file) && !shown(edit); }));
    tap(s.w, Qt::Key_Left);
    QObject* help = s.menu(8);
    REQUIRE(waitFor([&] { return opened(help) && !shown(file); }));
    // The title of the open menu shows it.
    CHECK(findNamed(s.w->contentItem(), QStringLiteral("menuBar_help"))->property("open").toBool());

    // Esc closes it, and the focus goes back to the lanes.
    tap(s.w, Qt::Key_Escape);
    REQUIRE(waitFor([&] { return !shown(help); }));
    CHECK(waitFor([&] { return lanes->hasActiveFocus(); }));

    // A title has keyboard focus: Down opens its menu, Right moves the focus to the next title.
    QQuickItem* viewTitle = findNamed(s.w->contentItem(), QStringLiteral("menuBar_view"));
    REQUIRE(viewTitle != nullptr);
    viewTitle->forceActiveFocus(Qt::TabFocusReason);
    tap(s.w, Qt::Key_Right);
    QQuickItem* windowTitle = findNamed(s.w->contentItem(), QStringLiteral("menuBar_window"));
    CHECK(windowTitle->hasActiveFocus());
    tap(s.w, Qt::Key_Down);
    QObject* window = s.menu(7);
    REQUIRE(waitFor([&] { return opened(window); }));
    tap(s.w, Qt::Key_Escape);
    REQUIRE(waitFor([&] { return !shown(window); }));
    CHECK(waitFor([&] { return windowTitle->hasActiveFocus(); }));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Menu bar: the pointer opens switches and closes the menus like a native menu bar", "[app][menubar][qml]") {
    resetSettings();
    clearMessages();
    MenuShell s;
    QQuickItem* content = s.w->contentItem();
    QQuickItem* fileTitle = findNamed(content, QStringLiteral("menuBar_file"));
    QQuickItem* editTitle = findNamed(content, QStringLiteral("menuBar_edit"));
    QQuickItem* clipTitle = findNamed(content, QStringLiteral("menuBar_clip"));
    REQUIRE(fileTitle != nullptr);
    REQUIRE(editTitle != nullptr);
    REQUIRE(clipTitle != nullptr);
    QObject* file = s.menu(0);
    QObject* edit = s.menu(1);
    QObject* clip = s.menu(2);
    QQuickItem* lanes = s.shell.item("timelineLanes");
    REQUIRE(lanes != nullptr);
    lanes->forceActiveFocus(Qt::TabFocusReason);
    // A click on File opens it (the title does not take the focus).
    clickAt(s.w, centreOf(fileTitle));
    REQUIRE(waitFor([&] { return opened(file); }));
    CHECK(fileTitle->property("open").toBool());
    CHECK_FALSE(fileTitle->hasActiveFocus());
    // Pointing at Edit while File is open switches to Edit.
    mouse(s.w, QEvent::MouseMove, centreOf(editTitle), Qt::NoButton);
    REQUIRE(waitFor([&] { return opened(edit) && !shown(file); }));
    // A click on the open menu's title closes it (and nothing opens again); the focus is back on the lanes.
    mouse(s.w, QEvent::MouseButtonPress, centreOf(editTitle), Qt::LeftButton);
    mouse(s.w, QEvent::MouseButtonRelease, centreOf(editTitle), Qt::NoButton);
    REQUIRE(waitFor([&] { return !shown(edit); }));
    spin(100);
    CHECK_FALSE(shown(edit));
    CHECK(waitFor([&] { return lanes->hasActiveFocus(); }));
    // With nothing open, pointing at a title opens nothing.
    mouse(s.w, QEvent::MouseMove, centreOf(clipTitle), Qt::NoButton);
    spin(100);
    CHECK_FALSE(shown(clip));
    // File open (by a click), then the pointer over Clip: Clip.
    clickAt(s.w, centreOf(fileTitle));
    REQUIRE(waitFor([&] { return opened(file); }));
    mouse(s.w, QEvent::MouseMove, centreOf(clipTitle), Qt::NoButton);
    REQUIRE(waitFor([&] { return opened(clip) && !shown(file); }));
    // A click outside the menus closes Clip.
    clickAt(s.w, centreOf(lanes));
    REQUIRE(waitFor([&] { return !shown(clip); }));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Menu bar: short of room the titles fold into one Menu button with the same menus", "[app][menubar][qml]") {
    resetSettings();
    clearMessages();
    MenuShell s;
    QQuickItem* content = s.w->contentItem();
    QQuickItem* fileTitle = findNamed(content, QStringLiteral("menuBar_file"));
    QQuickItem* menuButton = findNamed(content, QStringLiteral("menuBar_menu"));
    REQUIRE(fileTitle != nullptr);
    REQUIRE(menuButton != nullptr);
    CHECK(fileTitle->isVisible());
    CHECK_FALSE(menuButton->isVisible());

    s.shell.resize(1100, 700);
    REQUIRE(waitFor([&] { return s.bar->property("collapsed").toBool(); }));
    CHECK_FALSE(fileTitle->isVisible());
    CHECK(menuButton->isVisible());
    // It stays inside the title bar, left of the workspace tabs.
    QQuickItem* tabs = s.shell.item("workspaceTabs");
    REQUIRE(tabs != nullptr);
    CHECK(menuButton->mapToScene(QPointF(menuButton->width(), 0)).x() <= tabs->mapToScene(QPointF(0, 0)).x());

    // Its menu: the nine menus as submenus, in order.
    REQUIRE(QMetaObject::invokeMethod(menuButton, "clicked"));
    QObject* root = nullptr;
    REQUIRE(waitFor([&] {
        root = s.shell.object("menuBarCollapsedMenu");
        return opened(root);
    }));
    CHECK(menuButton->property("open").toBool());
    REQUIRE(root->property("count").toInt() == 9);
    for (int i = 0; i < 9; ++i) {
        QQuickItem* row = nullptr;
        REQUIRE(QMetaObject::invokeMethod(root, "itemAt", Q_RETURN_ARG(QQuickItem*, row), Q_ARG(int, i)));
        REQUIRE(row != nullptr);
        CHECK(row->property("text").toString() == QLatin1String(kTitles[i]));
        CHECK(row->property("subMenu").value<QObject*>() != nullptr);
    }

    // A row in a submenu runs like the bar's: View > Show Rulers.
    QObject* view = s.shell.object("collapsedViewMenu");
    REQUIRE(view != nullptr);
    QObject* rulersRow = view->findChild<QObject*>(QStringLiteral("menuCommand_programMonitor.showRulers"));
    REQUIRE(rulersRow != nullptr);
    const bool rulers = evaluate(s.shell, QStringLiteral("Monitors.showRulers")).toBool();
    REQUIRE(QMetaObject::invokeMethod(rulersRow, "clicked"));
    REQUIRE(waitFor([&] { return !shown(root); }));
    CHECK(waitFor([&] { return evaluate(s.shell, QStringLiteral("Monitors.showRulers")).toBool() != rulers; }));

    // The logo (File) opens the Menu with File open.
    REQUIRE(QMetaObject::invokeMethod(s.bar, "open", Q_ARG(int, 0), Q_ARG(bool, false)));
    QObject* file = s.shell.object("collapsedFileMenu");
    REQUIRE(file != nullptr);
    REQUIRE(waitFor([&] { return opened(root) && opened(file); }));
    REQUIRE_FALSE(s.shell.grab().isNull());
    tap(s.w, Qt::Key_Escape);
    tap(s.w, Qt::Key_Escape);
    REQUIRE(waitFor([&] { return !shown(root) && !shown(file); }));

    // Room again: the titles come back.
    s.shell.resize(kShotWidth, kShotHeight);
    REQUIRE(waitFor([&] { return !s.bar->property("collapsed").toBool(); }));
    CHECK(fileTitle->isVisible());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Menu bar: a Help row runs after its menu closed and its dialog keeps the focus", "[app][menubar][qml]") {
    resetSettings();
    clearMessages();
    MenuShell s;
    s.shell.preferences->setProperty("welcomeDone", true);
    QQuickItem* lanes = s.shell.item("timelineLanes");
    REQUIRE(lanes != nullptr);
    lanes->forceActiveFocus(Qt::TabFocusReason);

    QObject* help = s.open(8);
    for (const char* row : {"helpItem_guide", "helpItem_gettingStarted", "helpItem_fromPremiere", "helpItem_shortcutsGuide",
                            "helpItem_shortcuts", "helpItem_welcome", "helpItem_tour", "helpItem_trainer",
                            "helpItem_whatsNew", "helpItem_releaseNotes", "helpItem_sample", "helpItem_reportIssue",
                            "helpItem_about"}) {
        INFO(row);
        QObject* item = help->findChild<QObject*>(QLatin1String(row));
        REQUIRE(item != nullptr);
        CHECK(item->property("enabled").toBool()); // the website rows have their address (Home.links)
    }
    QObject* trainer = s.shell.object("keyboardTrainer");
    REQUIRE(trainer != nullptr);
    s.choose(help, "helpItem_trainer");
    REQUIRE(waitFor([&] { return opened(trainer); }));
    CHECK_FALSE(shown(help));
    CHECK(waitFor([&] { return isInside(s.w->activeFocusItem(), popupItem(trainer)); }));
    tap(s.w, Qt::Key_Escape);
    REQUIRE(waitFor([&] { return !shown(trainer); }));

    // Help > Open the Sample Project after edits to the sample: the edits prompt first (Main.whenSaved).
    s.controller->newSequence();
    REQUIRE(s.controller->canUndo());
    s.choose(s.open(8), "helpItem_sample");
    QObject* unsaved = s.shell.object("unsavedChangesDialog");
    REQUIRE(unsaved != nullptr);
    REQUIRE(waitFor([&] { return opened(unsaved); }));
    REQUIRE(QMetaObject::invokeMethod(unsaved, "close"));
    REQUIRE(QMetaObject::invokeMethod(unsaved, "discardChosen"));
    CHECK(waitFor([&] { return !s.controller->canUndo(); }));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Menu bar: the Window menu and the palette leave Generate out while AI is off", "[app][menubar][ai][qml]") {
    qputenv("WABCUT_AI_KEYSTORE", "memory");
    resetSettings();
    clearMessages();
    MenuShell s;
    auto* palette = s.shell.engine.singletonInstance<CommandPalette*>("Wabcut", "CommandPalette");
    REQUIRE(palette != nullptr);
    const auto paletteHas = [&](const QString& key) {
        palette->setQuery(QStringLiteral("open panel"));
        for (const QVariant& row : palette->results()) {
            if (row.toMap().value(QStringLiteral("key")).toString() == key)
                return true;
        }
        return false;
    };

    QObject* window = s.menu(7);
    REQUIRE(window->findChild<QObject*>(QStringLiteral("windowMenu_generate")) != nullptr);
    CHECK(paletteHas(QStringLiteral("shell:panel:generate")));

    s.shell.preferences->setProperty("aiEnabled", false);
    spin(30);
    CHECK(window->findChild<QObject*>(QStringLiteral("windowMenu_generate")) == nullptr);
    CHECK(window->findChild<QObject*>(QStringLiteral("windowMenu_scopes")) != nullptr);
    CHECK_FALSE(paletteHas(QStringLiteral("shell:panel:generate")));
    CHECK(paletteHas(QStringLiteral("shell:panel:scopes")));

    s.shell.preferences->setProperty("aiEnabled", true);
    spin(30);
    CHECK(window->findChild<QObject*>(QStringLiteral("windowMenu_generate")) != nullptr);

    // The palette's shell rows run: a panel opens, a workspace comes up.
    CHECK(palette->activate(QStringLiteral("shell:panel:markers")).isEmpty());
    CHECK(waitFor([&] { return evaluate(s.shell, QStringLiteral("docking.isOpen('markers')")).toBool(); }));
    const int audio = evaluate(s.shell, QStringLiteral("Workspaces.createFromPreset('audio', '')")).toInt();
    spin(30);
    CHECK(palette->activate(QStringLiteral("shell:workspace:%1").arg(audio)).isEmpty());
    CHECK(waitFor([&] { return s.w->property("workspace").toInt() == audio; }));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Menu bar: a handed-over project opens after the unsaved-changes prompt and File Revert and Open Recent work",
          "[app][menubar][qml]") {
    resetSettings();
    clearMessages();
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "app" / "menu-bar" / "handover";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    ProjectState state;
    auto created = edit::addSequence(state, edit::SequenceSettings{});
    REQUIRE(created);
    const fs::path film = dir / "handed over.wabcut";
    REQUIRE(io::saveProject(created->state, film, io::SaveOptions{0}));
    const QUrl url = QUrl::fromLocalFile(QString::fromStdU16String(film.u16string()));

    MenuShell s;
    auto* shellApp = s.shell.engine.singletonInstance<AppShell*>("Wabcut", "AppShell");
    auto* project = s.shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    REQUIRE(shellApp != nullptr);
    REQUIRE(project != nullptr);

    // Edits to the sample project: the prompt first; Cancel keeps everything.
    s.controller->newSequence();
    shellApp->requestOpen({url});
    QObject* unsaved = s.shell.object("unsavedChangesDialog");
    REQUIRE(unsaved != nullptr);
    REQUIRE(waitFor([&] { return opened(unsaved); }));
    REQUIRE(QMetaObject::invokeMethod(unsaved, "close"));
    spin(30);
    CHECK(project->demo());

    // Again, and Don't Save: it opens.
    shellApp->requestOpen({url});
    REQUIRE(waitFor([&] { return opened(unsaved); }));
    REQUIRE(QMetaObject::invokeMethod(unsaved, "close"));
    REQUIRE(QMetaObject::invokeMethod(unsaved, "discardChosen"));
    CHECK(waitFor([&] { return !project->demo(); }));
    CHECK(QUrl::fromLocalFile(project->filePath()) == url);

    // File > Open Recent lists it (Home's recent projects, newest first).
    QObject* fileMenu = s.open(0);
    QObject* recent = fileMenu->findChild<QObject*>(QStringLiteral("fileMenuRecent"));
    REQUIRE(recent != nullptr);
    QObject* first = nullptr;
    CHECK(waitFor([&] {
        first = recent->findChild<QObject*>(QStringLiteral("fileMenuRecent_0"));
        return first != nullptr && first->property("text").toString() == QStringLiteral("handed over");
    }));
    // File > Revert: greyed out while nothing changed; after an edit it asks first, then reads the saved file again.
    QObject* revert = fileMenu->findChild<QObject*>(QStringLiteral("fileMenuRevert"));
    REQUIRE(revert != nullptr);
    CHECK_FALSE(revert->property("enabled").toBool());
    REQUIRE(QMetaObject::invokeMethod(fileMenu, "close"));
    REQUIRE(waitFor([&] { return !shown(fileMenu); }));
    const int sequences = static_cast<int>(s.controller->sequenceNames().size());
    s.controller->newSequence();
    REQUIRE(project->dirty());
    s.choose(s.open(0), "fileMenuRevert");
    QObject* confirm = s.shell.object("menuConfirmDialog");
    REQUIRE(confirm != nullptr);
    REQUIRE(waitFor([&] { return opened(confirm); }));
    CHECK(project->dirty()); // nothing happens before the answer
    QQuickItem* confirmButton = findNamed(popupItem(confirm), QStringLiteral("menuConfirmButton"));
    REQUIRE(confirmButton != nullptr);
    CHECK(confirmButton->property("text").toString() == QStringLiteral("Revert"));
    REQUIRE(QMetaObject::invokeMethod(confirmButton, "clicked"));
    CHECK(waitFor([&] { return !project->dirty(); }));
    CHECK(static_cast<int>(s.controller->sequenceNames().size()) == sequences);
    CHECK(QUrl::fromLocalFile(project->filePath()) == url);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Menu bar: the project open now is not read again and a hand-over waits for the prompt on screen",
          "[app][menubar][qml]") {
    resetSettings();
    clearMessages();
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "app" / "menu-bar" / "reopen";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    ProjectState state;
    auto created = edit::addSequence(state, edit::SequenceSettings{});
    REQUIRE(created);
    const fs::path film = dir / "film.wabcut";
    const fs::path other = dir / "other.wabcut";
    REQUIRE(io::saveProject(created->state, film, io::SaveOptions{0}));
    REQUIRE(io::saveProject(created->state, other, io::SaveOptions{0}));
    const QUrl filmUrl = QUrl::fromLocalFile(QString::fromStdU16String(film.u16string()));
    const QUrl otherUrl = QUrl::fromLocalFile(QString::fromStdU16String(other.u16string()));

    MenuShell s;
    auto* shellApp = s.shell.engine.singletonInstance<AppShell*>("Wabcut", "AppShell");
    auto* project = s.shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    auto* palette = s.shell.engine.singletonInstance<CommandPalette*>("Wabcut", "CommandPalette");
    REQUIRE(shellApp != nullptr);
    REQUIRE(project != nullptr);
    REQUIRE(palette != nullptr);
    QObject* unsaved = s.shell.object("unsavedChangesDialog");
    QObject* home = s.shell.object("homeScreen");
    REQUIRE(unsaved != nullptr);
    REQUIRE(home != nullptr);
    const auto isOpen = [&](const QUrl& file) {
        return !project->demo() && QUrl::fromLocalFile(project->filePath()) == file;
    };

    // The untouched sample project: the hand-over opens at once.
    shellApp->requestOpen({filmUrl});
    REQUIRE(waitFor([&] { return isOpen(filmUrl); }));
    // An edit: unsaved changes and an undo step.
    s.controller->newSequence();
    REQUIRE(project->dirty());
    REQUIRE(s.controller->canUndo());
    const auto sequences = s.controller->sequenceNames().size();
    // The session is kept: no prompt, the changes and the undo history stay, the file is not read again.
    const auto keptSession = [&](const char* how) {
        INFO(how);
        spin(50);
        CHECK_FALSE(shown(unsaved));
        CHECK(isOpen(filmUrl));
        CHECK(project->dirty());
        CHECK(s.controller->canUndo());
        CHECK(s.controller->sequenceNames().size() == sequences);
    };

    // film.wabcut double-clicked in Explorer again (the window only comes to the front); with Home open, Home closes.
    shellApp->requestOpen({filmUrl});
    keptSession("hand-over");
    REQUIRE(QMetaObject::invokeMethod(home, "open"));
    REQUIRE(waitFor([&] { return home->property("opened").toBool(); }));
    shellApp->requestOpen({filmUrl});
    CHECK(waitFor([&] { return !home->property("opened").toBool(); }));
    keptSession("hand-over with Home open");

    // File > Open Recent > film (the newest).
    QObject* fileMenu = s.open(0);
    QObject* first = nullptr;
    REQUIRE(waitFor([&] {
        first = fileMenu->findChild<QObject*>(QStringLiteral("fileMenuRecent_0"));
        return first != nullptr && first->property("text").toString() == QStringLiteral("film");
    }));
    s.choose(fileMenu, "fileMenuRecent_0");
    keptSession("File > Open Recent");

    // The palette's "Open Recent: film".
    palette->setQuery(QStringLiteral("open recent film"));
    QString recentKey;
    for (const QVariant& row : palette->results()) {
        const QString key = row.toMap().value(QStringLiteral("key")).toString();
        if (key.startsWith(QStringLiteral("shell:recent:")) && key.contains(QStringLiteral("film.wabcut")))
            recentKey = key;
    }
    REQUIRE_FALSE(recentKey.isEmpty());
    CHECK(palette->activate(recentKey).isEmpty());
    keptSession("the palette");

    // File > Close Project asks about the changes. A project handed over while it asks does not take Close Project's
    // place: Don't Save closes the project (the sample project and Home come), then the handed-over one opens.
    s.choose(s.open(0), "fileMenuCloseProject");
    REQUIRE(waitFor([&] { return opened(unsaved); }));
    shellApp->requestOpen({otherUrl});
    spin(50);
    CHECK(opened(unsaved));
    CHECK(isOpen(filmUrl));
    REQUIRE(QMetaObject::invokeMethod(unsaved, "close"));
    REQUIRE(QMetaObject::invokeMethod(unsaved, "discardChosen"));
    CHECK(project->demo()); // Close Project ran first
    CHECK(waitFor([&] { return isOpen(otherUrl); }));
    CHECK(waitFor([&] { return !home->property("opened").toBool(); }));
    CHECK_FALSE(project->dirty());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Menu bar: Alt plus a letter leaves a chord the keymap binds and is off on macOS", "[app][menubar][qml]") {
    resetSettings();
    clearMessages();
    MenuShell s;
    auto* keys = s.shell.engine.singletonInstance<KeymapDispatcher*>("Wabcut", "KeymapDispatcher");
    REQUIRE(keys != nullptr);
    QTemporaryDir config;
    keys->setOverridesPath(config.filePath(QStringLiteral("keymap/overrides.json")));
    keys->setPlatform(QStringLiteral("windows"));
    spin(30);
    const auto keyEnabled = [&](const char* menu) {
        QObject* key = s.shell.object(QStringLiteral("menuBarKey_%1").arg(QLatin1String(menu)).toLatin1().constData());
        INFO(menu);
        REQUIRE(key != nullptr);
        return key->property("enabled").toBool();
    };
    for (const char* menu : kMenuKeys)
        CHECK(keyEnabled(menu)); // none of the nine chords is bound in Premiere's Windows keys

    // The user puts Alt+F on Zoom In: the File menu's Alt+F steps aside and the key runs Zoom In.
    QStringList triggered;
    QObject::connect(keys, &KeymapDispatcher::commandTriggered, [&](const QString& id) { triggered << id; });
    const QVariantMap assigned = keys->assignShortcut(QStringLiteral("sequence.zoomIn"), QStringLiteral("Alt+F"));
    REQUIRE(assigned.value(QStringLiteral("ok")).toBool());
    spin(30);
    CHECK_FALSE(keyEnabled("file"));
    CHECK(keyEnabled("edit"));
    QQuickItem* lanes = s.shell.item("timelineLanes");
    REQUIRE(lanes != nullptr);
    lanes->forceActiveFocus(Qt::TabFocusReason);
    tap(s.w, Qt::Key_F, Qt::AltModifier);
    CHECK(triggered.contains(QStringLiteral("sequence.zoomIn")));
    CHECK_FALSE(shown(s.menu(0)));
    REQUIRE(keys->resetShortcut(QStringLiteral("sequence.zoomIn")));
    spin(30);
    CHECK(keyEnabled("file"));

    // macOS: no menu mnemonics (Option+letter types characters there; Option+M is Clear Selected Marker).
    keys->setPlatform(QStringLiteral("macos"));
    spin(30);
    for (const char* menu : kMenuKeys)
        CHECK_FALSE(keyEnabled(menu));
    keys->setPlatform(QStringLiteral("windows"));
    spin(30);
    CHECK(keyEnabled("markers"));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Menu bar: the Markers rows follow the Source Monitor marks while it has the focus",
          "[app][menubar][source][qml]") {
    resetSettings();
    clearMessages();
    MenuShell s;
    auto* source = s.shell.engine.singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor");
    REQUIRE(source != nullptr);
    // The sequence has no In or Out; the source gets both.
    s.controller->runCommand(QStringLiteral("marker.clearInAndOut"));
    REQUIRE_FALSE(s.controller->markIn());
    REQUIRE_FALSE(s.controller->markOut());
    REQUIRE(source->load(mediaKey(s.controller->snapshot()->media.front()->id)));
    spin(30);
    QQuickItem* well = s.shell.item("sourceMonitorWell");
    REQUIRE(well != nullptr);
    well->forceActiveFocus();
    REQUIRE(well->hasActiveFocus());
    source->markIn();
    source->seekFraction(0.5);
    source->markOut();
    REQUIRE(source->hasIn());
    REQUIRE(source->hasOut());

    // With the Source Monitor focused the In and Out rows go by its marks (the sequence has none).
    QObject* markers = s.open(4);
    for (const char* row : {"menuCommand_marker.goToIn", "menuCommand_marker.goToOut", "menuCommand_marker.clearIn",
                            "menuCommand_marker.clearOut", "menuCommand_marker.clearInAndOut",
                            "menuCommand_marker.markIn"}) {
        INFO(row);
        CHECK(s.enabled(markers, row));
    }
    CHECK_FALSE(s.enabled(markers, "menuCommand_marker.markSelection")); // only in the Timeline
    // Clear In and Out clears the source's marks.
    s.choose(markers, "menuCommand_marker.clearInAndOut");
    CHECK(waitFor([&] { return !source->hasIn() && !source->hasOut(); }));
    CHECK(waitFor([&] { return well->hasActiveFocus(); }));
    // Now the source has none: greyed out.
    markers = s.open(4);
    CHECK_FALSE(s.enabled(markers, "menuCommand_marker.clearIn"));
    CHECK_FALSE(s.enabled(markers, "menuCommand_marker.goToOut"));
    CHECK_FALSE(s.enabled(markers, "menuCommand_marker.clearInAndOut"));
    s.dismiss(markers);

    // The Timeline focused: the sequence's marks count, not the source's.
    source->markIn();
    REQUIRE(source->hasIn());
    QQuickItem* lanes = s.shell.item("timelineLanes");
    REQUIRE(lanes != nullptr);
    lanes->forceActiveFocus(Qt::TabFocusReason);
    markers = s.open(4);
    CHECK_FALSE(s.enabled(markers, "menuCommand_marker.clearIn"));
    CHECK_FALSE(s.enabled(markers, "menuCommand_marker.goToIn"));
    s.dismiss(markers);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Menu bar: the Edit rows act on a focused text field and not on the timeline", "[app][menubar][qml]") {
    resetSettings();
    clearMessages();
    MenuShell s;
    QQuickItem* field = findNamed(s.w->contentItem(), QStringLiteral("librarySearchField"));
    REQUIRE(field != nullptr);
    REQUIRE(field->isVisible());
    // A clip is selected in the timeline.
    const auto sequence = s.controller->currentSequence();
    const auto clips = sequence->videoTracks[0]->clips.size();
    s.controller->selectClipById(sequence->videoTracks[0]->clips.front().id, ProjectController::Replace);
    const auto selected = s.controller->selection().size();
    REQUIRE(selected > 0);

    // Text selected in the Library's search field: Cut, Copy and Clear can run (on the text); Clear removes it and
    // leaves the clip alone.
    field->forceActiveFocus(Qt::MouseFocusReason);
    field->setProperty("text", QStringLiteral("moment"));
    REQUIRE(QMetaObject::invokeMethod(field, "selectAll"));
    REQUIRE(field->property("selectedText").toString() == QStringLiteral("moment"));
    QObject* edit = s.open(1);
    CHECK(s.enabled(edit, "menuCommand_edit.copy"));
    CHECK(s.enabled(edit, "menuCommand_edit.cut"));
    CHECK(s.enabled(edit, "menuCommand_edit.selectAll"));
    s.choose(edit, "menuCommand_edit.clear");
    CHECK(waitFor([&] { return field->property("text").toString().isEmpty(); }));
    CHECK(field->hasActiveFocus());
    CHECK(s.controller->currentSequence()->videoTracks[0]->clips.size() == clips);
    CHECK(s.controller->selection().size() == selected);

    // No text selected: Copy is greyed out (the timeline's selection does not count); Select All selects the text, not
    // every clip.
    field->setProperty("text", QStringLiteral("goal"));
    REQUIRE(QMetaObject::invokeMethod(field, "deselect"));
    edit = s.open(1);
    CHECK_FALSE(s.enabled(edit, "menuCommand_edit.copy"));
    s.choose(edit, "menuCommand_edit.selectAll");
    CHECK(waitFor([&] { return field->property("selectedText").toString() == QStringLiteral("goal"); }));
    CHECK(s.controller->selection().size() == selected);

    // Back in the timeline the rows are the timeline's again.
    QQuickItem* lanes = s.shell.item("timelineLanes");
    REQUIRE(lanes != nullptr);
    lanes->forceActiveFocus(Qt::TabFocusReason);
    edit = s.open(1);
    CHECK(s.enabled(edit, "menuCommand_edit.copy")); // a clip is selected
    s.dismiss(edit);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
