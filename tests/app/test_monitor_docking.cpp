// 0.1.3: the Program Monitor in a real (shown) window keeps presenting while it plays and after a seek, and panel
// docking: panels in groups with tabs; a panel or a group dropped on another group's tabs or middle joins it, near its
// edge docks beside it, at the work area's edge runs along that side, and anywhere else (or with Ctrl) gets a window of
// its own; dragging a grip or a tab shows where it would go; tabs show and close panels, the close button in a header
// too, and a closed panel opens again; each workspace (made from a preset, none built in) keeps its own layout, kept
// between sessions (the first preview's workspaces too); File > Reset Panel Layout gives the preset's layout back; a
// group popped out into a window of its own gets the keys there and docks back when that window closes or when its grip
// is dragged onto the workbench. UX rework: a press, a tab, a drop or a focus key makes a group the active one (an
// accent outline), where the Window menu opens closed panels; Maximize Frame (`, Shift+`, a double-click on a tab, the
// tab menu) gives one group the whole work area and restores; the tab menu closes, undocks and maximizes; a lone
// panel's tab drags like a grouped one; Ctrl+W closes the active panel; Reset to Saved Layout can be undone. Two
// monitors sharing a narrow place: only the one shown takes presses, hover, drops and `; the Window menu keeps them
// sharing. Every way to maximize moves the keyboard focus into the frame; a group docked back from its window shows.
// 0.4: a crowded group keeps readable tabs and lists every panel behind its tab list button; a lone panel's tab opens
// its menu from the keyboard; header tools that do not fit go into the More tools menu (a chevron button), whose rows act
// like a click on the tool (a checkable one toggles), and hidden tools leave the Tab chain.

#include "ShellFixture.h"

#include "KeymapDispatcher.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/command/Engine.h>
#include <wabcut/playback/video/PlaybackController.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCloseEvent>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QProcess>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QScreen>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

/// Runs `code` in Main.qml's context (its ids: docking, browserDock, ...).
QVariant evaluate(const Shell& shell, const QString& code) {
    QQmlExpression expression(qmlContext(shell.window.get()), shell.window.get(), code);
    const QVariant result = expression.evaluate();
    INFO(code.toStdString() << ": " << expression.error().toString().toStdString());
    REQUIRE_FALSE(expression.hasError());
    return result;
}

QPointF scenePos(const QQuickItem* item) {
    return item->mapToScene({});
}

/// A mouse event at `scene` (window coordinates), with the matching screen position (the docking reads that one).
void mouseAt(QQuickWindow* window, QEvent::Type type, QPointF scene, Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, scene, scene, window->mapToGlobal(scene), button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &event);
}

/// Every item named `name` under `item`.
void collect(QQuickItem* item, const QString& name, QList<QQuickItem*>& out) {
    if (item->objectName() == name)
        out << item;
    for (QQuickItem* child : item->childItems())
        collect(child, name, out);
}

/// The first item named `name` in the shell (a Repeater's items, such as the groups and the gutters, are not the
/// window's QObject children, so Shell::item does not find them).
QQuickItem* findItem(const Shell& shell, const QString& name) {
    QList<QQuickItem*> found;
    collect(shell.window->contentItem(), name, found);
    return found.isEmpty() ? nullptr : found.first();
}

/// The slot (DockSlot) of the group that holds `panel`, or null (closed).
QQuickItem* groupOf(const Shell& shell, const char* panel) {
    return qobject_cast<QQuickItem*>(
        evaluate(shell, QStringLiteral("docking.slotOfPanel('%1')").arg(QLatin1String(panel))).value<QObject*>());
}
QString groupId(const Shell& shell, const char* panel) {
    return evaluate(shell, QStringLiteral("docking.groupOfPanel('%1').group").arg(QLatin1String(panel))).toString();
}
/// The panel shown by the group that holds `panel`.
QString shownPanel(const Shell& shell, const char* panel) {
    return evaluate(shell, QStringLiteral("docking.shownPanel(docking.groupOfPanel('%1').group)").arg(QLatin1String(panel)))
        .toString();
}
/// Theme.space(n): the 4 px grid.
double grid(double n) {
    return n * 4;
}
/// A left click at `local` in `item`.
void click(const Shell& shell, QQuickItem* item, QPointF local) {
    const QPointF p = item->mapToScene(local);
    mouseAt(shell.window.get(), QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton);
    mouseAt(shell.window.get(), QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton);
    spin(30);
}

/// A right click at `local` in `item`.
void rightClick(const Shell& shell, QQuickItem* item, QPointF local) {
    const QPointF p = item->mapToScene(local);
    mouseAt(shell.window.get(), QEvent::MouseButtonPress, p, Qt::RightButton, Qt::RightButton);
    mouseAt(shell.window.get(), QEvent::MouseButtonRelease, p, Qt::RightButton, Qt::NoButton);
    spin(30);
}
/// A double click at `local` in `item` (two presses close together in time).
void doubleClick(const Shell& shell, QQuickItem* item, QPointF local) {
    const QPointF p = item->mapToScene(local);
    QQuickWindow* window = shell.window.get();
    ulong at = 100000;
    for (int i = 0; i < 4; ++i) {
        const bool press = i % 2 == 0;
        QMouseEvent event(press ? QEvent::MouseButtonPress : QEvent::MouseButtonRelease, p, p, window->mapToGlobal(p),
                          Qt::LeftButton, press ? Qt::MouseButtons(Qt::LeftButton) : Qt::MouseButtons(Qt::NoButton),
                          Qt::NoModifier);
        event.setTimestamp(at += 40);
        QCoreApplication::sendEvent(window, &event);
    }
    spin(30);
}
/// The pointer moves (no button) to `local` in `item`: hover.
void hoverAt(const Shell& shell, QQuickItem* item, QPointF local) {
    const QPointF p = item->mapToScene(local);
    mouseAt(shell.window.get(), QEvent::MouseMove, p, Qt::NoButton, Qt::NoButton);
    spin(30);
}
/// The first item named `name` under `root`.
QQuickItem* findUnder(QQuickItem* root, const QString& name) {
    QList<QQuickItem*> found;
    collect(root, name, found);
    return found.isEmpty() ? nullptr : found.first();
}
/// The current layout's groups, in order: each one's panels joined with "+".
QString groupsText(const Shell& shell) {
    return evaluate(shell, QStringLiteral("Workspaces.groupsOf(docking.tree).map(g => g.panels.join('+')).join(' ')")).toString();
}
QString activeGroup(const Shell& shell) {
    return evaluate(shell, QStringLiteral("docking.activeGroup")).toString();
}
QString maximizedGroup(const Shell& shell) {
    return evaluate(shell, QStringLiteral("docking.maximizedGroup")).toString();
}
/// The group whose panels hold keyboard focus now ("" none).
QString focusGroup(const Shell& shell) {
    return evaluate(shell, QStringLiteral("docking.groupOfItem(window.activeFocusItem)")).toString();
}
/// The tab menu of the group that holds `panel`, once DockSlot made it (null before).
QObject* tabMenuOf(const Shell& shell, const char* panel) {
    return evaluate(shell, QStringLiteral("docking.slotOfPanel('%1').tabMenu").arg(QLatin1String(panel))).value<QObject*>();
}
/// Triggers the tab menu's item `name` (the menu is open) and waits until the menu closed and its choice ran.
/// The menu goes with its group when the choice closes the group's last panel: watched through a QPointer.
void chooseInMenu(QObject* menuObject, const char* name) {
    const QPointer<QObject> menu(menuObject);
    QObject* item = menu->findChild<QObject*>(QLatin1String(name));
    REQUIRE(item != nullptr);
    REQUIRE(item->property("enabled").toBool());
    REQUIRE(QMetaObject::invokeMethod(item, "triggered"));
    spin(30);
    if (menu && menu->property("visible").toBool()) // a menu closes itself on a click; a triggered() call may not
        QMetaObject::invokeMethod(menu, "close");
    REQUIRE(waitFor([&] { return !menu || !menu->property("visible").toBool(); }));
    spin(60);
}

/// A place beyond every screen: windows there put nothing on the desktop (the floating window test's).
QPoint offScreen() {
    QRect desktop;
    for (QScreen* screen : QGuiApplication::screens())
        desktop |= screen->geometry();
    return {desktop.right() + 400, desktop.top() + 40};
}

/// Nearly off the primary screen: a few pixels stay on it, enough for Windows to expose (and so render) the window,
/// which a window wholly off every screen never is.
QPoint nearlyOffScreen() {
    const QRect screen = QGuiApplication::primaryScreen()->geometry();
    return {screen.right() - 3, screen.bottom() - 3};
}

/// The build's ffmpeg: $WABCUT_FFMPEG, else <build>/vcpkg_installed/<triplet>/tools/ffmpeg, else PATH.
QString findFfmpeg() {
#ifdef Q_OS_WIN
    const QString exe = QStringLiteral("ffmpeg.exe");
#else
    const QString exe = QStringLiteral("ffmpeg");
#endif
    if (const QString env = qEnvironmentVariable("WABCUT_FFMPEG"); !env.isEmpty() && QFileInfo::exists(env))
        return env;
    QDir dir(QCoreApplication::applicationDirPath());
    for (int up = 0; up < 6; ++up) {
        const QDir installed(dir.filePath(QStringLiteral("vcpkg_installed")));
        if (installed.exists()) {
            for (const QString& triplet : installed.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                const QString candidate = installed.filePath(triplet + QStringLiteral("/tools/ffmpeg/") + exe);
                if (QFileInfo::exists(candidate))
                    return candidate;
            }
        }
        if (!dir.cdUp())
            break;
    }
    return QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
}

/// Two seconds of H.264 320x180 at 30 fps with an AAC tone (filter graph sources: the vcpkg ffmpeg has no lavfi input).
bool makeClip(const QString& ffmpeg, const QString& out) {
    QProcess process;
    process.start(ffmpeg, {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                           QStringLiteral("-y"), QStringLiteral("-filter_complex"),
                           QStringLiteral("testsrc2=size=320x180:rate=30:duration=2[v];"
                                          "sine=frequency=440:sample_rate=48000:duration=2[a]"),
                           QStringLiteral("-map"), QStringLiteral("[v]"), QStringLiteral("-map"), QStringLiteral("[a]"),
                           QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-preset"),
                           QStringLiteral("ultrafast"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
                           QStringLiteral("-c:a"), QStringLiteral("aac"), out});
    if (!process.waitForFinished(60000)) {
        process.kill();
        return false;
    }
    UNSCOPED_INFO(process.readAllStandardError().toStdString());
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0 && QFileInfo::exists(out);
}

} // namespace

TEST_CASE("The Program Monitor in a shown window presents while it plays and after a seek", "[app][playback][window]") {
    const QString ffmpeg = findFfmpeg();
    if (ffmpeg.isEmpty())
        SKIP("no ffmpeg to make the clip with");
    QTemporaryDir dir;
    const QString clip = dir.filePath(QStringLiteral("clip.mp4"));
    REQUIRE(makeClip(ffmpeg, clip));

    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    auto* file = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    auto* project = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* playback = shell.engine.singletonInstance<PlaybackBridge*>("Wabcut", "Playback");
    const QVariantMap imported = file->importFiles({QUrl::fromLocalFile(clip)});
    REQUIRE(imported.value(QStringLiteral("added")).toInt() == 1);
    command::CommandEngine commands(project->session());
    command::CommandBatch batch;
    batch.label = "Edit the clip in";
    batch.commands = {{.command = "clip.insert", .params = {{"media", "clip.mp4"}, {"at", "00:00:00:00"}}}};
    const command::CommandResult result = commands.execute(batch);
    INFO((result.error ? result.error->message : std::string()));
    REQUIRE_FALSE(result.error);

    // A real window: the scene graph's own render loop drives the monitor from frame to frame, which an offscreen
    // grab (a fresh renderer for every picture) never does. The bug this guards: the monitor took the composite that
    // was due only when its item was updated from the GUI thread, so playing showed one frozen picture.
    shell.window->setPosition(nearlyOffScreen());
    shell.window->show();
    REQUIRE(waitFor([&] { return shell.window->isExposed(); }));
    playback::PlaybackController* video = playback->activeController();
    REQUIRE(video != nullptr);
    REQUIRE(waitFor([&] { return video->stats().presented > 0; }));

    playback->play();
    REQUIRE(waitFor([&] { return playback->playing(); }));
    const std::uint64_t before = video->stats().presented;
    spin(1000);
    const std::uint64_t during = video->stats().presented - before;
    playback->pause();
    INFO("presented while playing 1 s: " << during << ", " << playback->statsText().toStdString());
    CHECK(during >= 10); // about 30 at 30 fps; the frozen picture presented none

    // Paused: a seek still reaches the screen (the publish wakes the idle monitor).
    spin(200);
    const std::uint64_t paused = video->stats().presented;
    project->setPlayhead(Ticks{kTicksPerSecond / 2});
    CHECK(waitFor([&] { return video->stats().presented > paused; }, 5000));
    project->setPlayhead(Ticks{kTicksPerSecond});
    const std::uint64_t again = video->stats().presented;
    CHECK(waitFor([&] { return video->stats().presented > again; }, 5000));
    shell.window->hide();
}

TEST_CASE("Panels join another group as tabs, dock beside it or along an edge, close and open again", "[app][docking][qml]") {
    resetSettings();
    clearMessages();
    {
        Shell shell;
        REQUIRE(shell.window != nullptr);
        shell.theme->setProperty("reduceMotion", true);
        spin(50);
        const double workWidth = evaluate(shell, QStringLiteral("workArea.width")).toDouble();
        const double workHeight = evaluate(shell, QStringLiteral("workArea.height")).toDouble();
        const double workX = evaluate(shell, QStringLiteral("workArea.mapToItem(null, 0, 0).x")).toDouble();

        // The first start: one workspace, made from the Edit preset (no built-in workspaces).
        CHECK(evaluate(shell, QStringLiteral("Workspaces.all.length")).toInt() == 1);
        CHECK(evaluate(shell, QStringLiteral("Workspaces.all[0].preset")).toString() == QStringLiteral("edit"));
        // Library | Source | Program | Effect Controls, the timeline below.
        QQuickItem* library = groupOf(shell, "library");
        QQuickItem* source = groupOf(shell, "source");
        QQuickItem* program = groupOf(shell, "program");
        QQuickItem* controls = groupOf(shell, "effectControls");
        QQuickItem* timeline = groupOf(shell, "timeline");
        REQUIRE(library != nullptr);
        REQUIRE(source != nullptr);
        REQUIRE(program != nullptr);
        REQUIRE(controls != nullptr);
        REQUIRE(timeline != nullptr);
        CHECK(scenePos(library).x() < scenePos(source).x());
        CHECK(scenePos(source).x() < scenePos(program).x());
        CHECK(scenePos(program).x() < scenePos(controls).x());
        CHECK(scenePos(timeline).y() > scenePos(program).y());
        CHECK(std::abs(timeline->width() - workWidth) <= 1.0);
        // Each group's shown panel carries the grip, the pop-out and the close button.
        for (const char* name : {"dockGrip_library", "dockToggle_library", "dockClose_library", "dockClose_effectControls",
                                 "dockToggle_program", "dockClose_timeline"}) {
            INFO(name);
            CHECK(shell.object(name) != nullptr);
        }

        // Effect Controls joins the Library's group: two tabs there, Effect Controls shown; its own group is gone and
        // the monitors take its room.
        evaluate(shell, QStringLiteral("docking.joinPanel('effectControls', docking.groupOfPanel('library').group)"));
        spin(50);
        CHECK(groupOf(shell, "effectControls") == library);
        QQuickItem* tabs = findItem(shell, QStringLiteral("dockTabs_") + groupId(shell, "library"));
        REQUIRE(tabs != nullptr);
        CHECK(tabs->isVisible());
        CHECK(shownPanel(shell, "library") == QStringLiteral("effectControls"));
        CHECK_FALSE(shell.item("libraryPanel")->isVisible());
        CHECK(scenePos(program).x() + program->width() >= workX + workWidth - 1.0);
        // A click on the Library's tab shows it again.
        QQuickItem* libraryTab = findItem(shell, QStringLiteral("dockTab_library"));
        REQUIRE(libraryTab != nullptr);
        click(shell, libraryTab, QPointF(grid(4), libraryTab->height() / 2));
        CHECK(shownPanel(shell, "library") == QStringLiteral("library"));
        CHECK(shell.item("libraryPanel")->isVisible());
        REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/docking-tabs.png")));

        // The × on Effect Controls' tab closes it: the group has one panel again (no tabs).
        QQuickItem* close = findItem(shell, QStringLiteral("dockTabClose_effectControls"));
        REQUIRE(close != nullptr);
        click(shell, close, QPointF(close->width() / 2, close->height() / 2));
        CHECK_FALSE(evaluate(shell, QStringLiteral("docking.isOpen('effectControls')")).toBool());
        CHECK_FALSE(tabs->isVisible());
        CHECK(evaluate(shell, QStringLiteral("docking.groupOfPanel('library').panels.length")).toInt() == 1);
        // Opened again where it fits (a programmatic reveal): beside the monitors, on their right. (The Window menu
        // would open it in the group clicked last, here the Library's.)
        evaluate(shell, QStringLiteral("window.revealPanel('effectControls')"));
        spin(50);
        controls = groupOf(shell, "effectControls");
        REQUIRE(controls != nullptr);
        CHECK(controls != library);
        CHECK(scenePos(controls).x() > scenePos(program).x());

        // The close button in the Program Monitor's header closes it; the Source Monitor takes its room.
        evaluate(shell, QStringLiteral("docking.slotOfPanel('program').closeCurrent()"));
        spin(50);
        CHECK_FALSE(evaluate(shell, QStringLiteral("docking.isOpen('program')")).toBool());
        CHECK_FALSE(shell.item("monitorPanel")->isVisible());
        CHECK(scenePos(controls).x() <= scenePos(source).x() + source->width() + 10.0);
        // The Program Monitor's focus key opens it again, beside the Source Monitor.
        evaluate(shell, QStringLiteral("monitorArea.show('program')"));
        spin(50);
        program = groupOf(shell, "program");
        REQUIRE(program != nullptr);
        CHECK(shell.item("monitorPanel")->isVisible());
        CHECK(scenePos(program).x() > scenePos(source).x());

        // The timeline below the Effect Controls group: as wide as it, and the monitors get the whole height.
        evaluate(shell, QStringLiteral("docking.movePanelBeside('timeline', docking.groupOfPanel('effectControls').group, 'bottom')"));
        spin(50);
        timeline = groupOf(shell, "timeline");
        CHECK(std::abs(scenePos(timeline).x() - scenePos(controls).x()) <= 1.0);
        CHECK(scenePos(timeline).y() > scenePos(controls).y() + controls->height() - 1.0);
        CHECK(std::abs(program->height() - workHeight) <= 1.0);
        // The Library along the whole left edge.
        evaluate(shell, QStringLiteral("docking.putAtEdge(docking.groupOfPanel('library').group, '', 'left')"));
        spin(50);
        CHECK(std::abs(library->height() - workHeight) <= 1.0);
        CHECK(scenePos(library).x() < workX + 1.0);

        // Each workspace keeps its own layout: a new one from the Audio preset has the mixer, Edit's comes back as left.
        const int edit = shell.window->property("workspace").toInt();
        evaluate(shell, QStringLiteral("window.openPreset('audio')"));
        spin(50);
        CHECK(shell.window->property("workspace").toInt() != edit);
        CHECK(evaluate(shell, QStringLiteral("docking.isOpen('mixer')")).toBool());
        CHECK_FALSE(evaluate(shell, QStringLiteral("docking.isOpen('library')")).toBool());
        shell.window->setProperty("workspace", edit);
        spin(50);
        CHECK(std::abs(groupOf(shell, "library")->height() - workHeight) <= 1.0);
        CHECK(evaluate(shell, QStringLiteral("docking.isOpen('effectControls')")).toBool());

        const QJsonObject saved =
            QJsonDocument::fromJson(shell.preferences->property("workspaceLayout").toString().toUtf8()).object();
        CHECK(saved.value(QStringLiteral("version")).toInt() == 2);
        CHECK(saved.value(QStringLiteral("workspaces")).toArray().size() == 2);
    }
    {
        // The next start lays the panels out the same way.
        Shell shell;
        REQUIRE(shell.window != nullptr);
        shell.theme->setProperty("reduceMotion", true);
        spin(50);
        const double workHeight = evaluate(shell, QStringLiteral("workArea.height")).toDouble();
        QQuickItem* library = groupOf(shell, "library");
        REQUIRE(library != nullptr);
        CHECK(std::abs(library->height() - workHeight) <= 1.0);
        QQuickItem* timeline = groupOf(shell, "timeline");
        QQuickItem* controls = groupOf(shell, "effectControls");
        CHECK(std::abs(scenePos(timeline).x() - scenePos(controls).x()) <= 1.0);

        // File > Reset Panel Layout: the Edit preset's layout again.
        evaluate(shell, QStringLiteral("docking.reset()"));
        spin(50);
        library = groupOf(shell, "library");
        timeline = groupOf(shell, "timeline");
        QQuickItem* program = groupOf(shell, "program");
        CHECK(scenePos(library).x() < scenePos(program).x());
        CHECK(scenePos(timeline).y() > scenePos(program).y());
        CHECK(std::abs(timeline->width() - evaluate(shell, QStringLiteral("workArea.width")).toDouble()) <= 1.0);

        // Every panel closed: the workbench says so; the Library comes back from there.
        for (const char* key : {"library", "source", "program", "effectControls", "timeline"})
            evaluate(shell, QStringLiteral("docking.closePanel('%1')").arg(QLatin1String(key)));
        spin(50);
        CHECK(evaluate(shell, QStringLiteral("docking.tree === null")).toBool());
        CHECK(shell.item("noPanelsPlaceholder")->isVisible());
        evaluate(shell, QStringLiteral("window.showBrowserPanel('library')"));
        spin(50);
        CHECK_FALSE(shell.item("noPanelsPlaceholder")->isVisible());
        CHECK(shell.item("libraryPanel")->isVisible());
    }
    {
        // A first preview's workspaces (version 1: built-ins and columns) load: its own ones keep their panels, the
        // built-ins are gone.
        resetSettings();
        Shell shell;
        REQUIRE(shell.window != nullptr);
        shell.preferences->setProperty(
            "workspaceLayout",
            QStringLiteral("{\"version\":1,\"workspaces\":[{\"id\":0},{\"id\":101,\"preset\":\"audio\",\"name\":\"Mix\","
                           "\"left\":[\"audio\"],\"right\":[\"mixer\",\"effectControls\"],\"source\":false,\"program\":true,"
                           "\"timeline\":true,\"volumeLanes\":true}]}"));
        evaluate(shell, QStringLiteral("Workspaces.restore()"));
        CHECK(evaluate(shell, QStringLiteral("Workspaces.all.length")).toInt() == 1);
        CHECK(evaluate(shell, QStringLiteral("Workspaces.all[0].name")).toString() == QStringLiteral("Mix"));
        CHECK(evaluate(shell, QStringLiteral("Workspaces.groupsOf(Workspaces.all[0].layout).map(g => g.panels.join('+')).join(' ')"))
                  .toString() == QStringLiteral("audio program mixer+effectControls timeline"));
    }
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Dragging a group's grip or a panel's tab shows where it would go and docks it there", "[app][docking][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    spin(50);
    QQuickItem* source = groupOf(shell, "source");
    QQuickItem* program = groupOf(shell, "program");
    QQuickItem* overlay = shell.item("dockDropOverlay");
    QQuickItem* preview = shell.item("dockDropPreview");
    REQUIRE(overlay != nullptr);
    REQUIRE(preview != nullptr);
    const auto dropIs = [&](const char* kind, const char* side = "") {
        const QString got = evaluate(shell, QStringLiteral("docking.drop.kind + '/' + (docking.drop.side ?? '')")).toString();
        INFO(got.toStdString());
        return got == QString::fromLatin1(kind) + QLatin1Char('/') + QString::fromLatin1(side);
    };
    const auto moveTo = [&](const QString& globalPoint, int modifiers = 0) {
        evaluate(shell, QStringLiteral("docking.moveDrag(docking.slotOfPanel('library'), %1, %2)").arg(globalPoint).arg(modifiers));
        spin(20);
    };
    const QString programAt = QStringLiteral("docking.slotOfPanel('program').mapToGlobal(docking.slotOfPanel('program').width * %1, "
                                             "docking.slotOfPanel('program').height * %2)");

    // The Library's grip.
    evaluate(shell, QStringLiteral("docking.beginDrag(docking.slotOfPanel('library'), '')"));
    spin(20);
    CHECK(overlay->isVisible());
    // Near the Program Monitor's lower edge: below it, in its lower half.
    moveTo(programAt.arg(0.5).arg(0.95));
    CHECK(dropIs("beside", "bottom"));
    CHECK(evaluate(shell, QStringLiteral("docking.target === docking.slotOfPanel('program')")).toBool());
    CHECK(evaluate(shell, QStringLiteral("docking.dropText")).toString().contains(QStringLiteral("below")));
    CHECK(preview->isVisible());
    CHECK(scenePos(preview).y() > scenePos(program).y() + program->height() / 3);
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/docking-drop-below.png")));
    // Near its left edge: left of it.
    moveTo(programAt.arg(0.1).arg(0.5));
    CHECK(dropIs("beside", "left"));
    // In its middle, or on its header: it would join its tabs (the whole group lights up).
    moveTo(programAt.arg(0.5).arg(0.5));
    CHECK(dropIs("join"));
    CHECK(evaluate(shell, QStringLiteral("docking.dropText")).toString().contains(QStringLiteral("tabs")));
    CHECK(std::abs(preview->width() - program->width()) <= 1.0);
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/docking-drop-tabs.png")));
    evaluate(shell, QStringLiteral("docking.moveDrag(docking.slotOfPanel('library'), docking.slotOfPanel('program').mapToGlobal("
                                   "docking.slotOfPanel('program').width * 0.1, 20), 0)"));
    CHECK(dropIs("join"));
    // With Ctrl held, over the title bar, or outside the window: a window of its own.
    moveTo(programAt.arg(0.5).arg(0.5), int(Qt::ControlModifier));
    CHECK(dropIs("float"));
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/docking-drop-window.png")));
    moveTo(QStringLiteral("workArea.mapToGlobal(workArea.width / 2, -Theme.space(4))"));
    CHECK(dropIs("float"));
    moveTo(QStringLiteral("workArea.mapToGlobal(workArea.width + Theme.space(50), workArea.height / 2)"));
    CHECK(dropIs("float"));
    // At the work area's left edge: along the whole left side. In a gutter: nothing.
    moveTo(QStringLiteral("workArea.mapToGlobal(2, workArea.height / 2)"));
    CHECK(dropIs("edge", "left"));
    moveTo(QStringLiteral("workArea.mapToGlobal((docking.slotOfPanel('source').x + docking.slotOfPanel('source').width "
                          "+ docking.slotOfPanel('program').x) / 2, docking.slotOfPanel('program').y + 100)"));
    CHECK(dropIs("none"));
    CHECK_FALSE(preview->isVisible());

    // Dropped in the Program Monitor's middle: the Library joins its tabs, shown; its own group is gone.
    moveTo(programAt.arg(0.5).arg(0.5));
    evaluate(shell, QStringLiteral("docking.endDrag(docking.slotOfPanel('library'))"));
    spin(50);
    CHECK_FALSE(overlay->isVisible());
    CHECK(groupOf(shell, "library") == program);
    CHECK(shownPanel(shell, "program") == QStringLiteral("library"));
    CHECK(shell.item("libraryPanel")->isVisible());
    CHECK_FALSE(shell.item("monitorPanel")->isVisible());
    CHECK(scenePos(source).x() < evaluate(shell, QStringLiteral("workArea.mapToItem(null, 0, 0).x")).toDouble() + 1.0);

    // A real drag of the Program Monitor's tab out of that group, released near the Source Monitor's left edge: the
    // Program Monitor alone, left of the Source Monitor; the Library stays where it was.
    QQuickItem* tab = findItem(shell, QStringLiteral("dockTab_program"));
    REQUIRE(tab != nullptr);
    REQUIRE(tab->isVisible());
    const QPointF from = tab->mapToScene(QPointF(grid(4), tab->height() / 2));
    const QPointF to = source->mapToScene(QPointF(source->width() * 0.08, source->height() / 2));
    mouseAt(shell.window.get(), QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 12; ++i) {
        mouseAt(shell.window.get(), QEvent::MouseMove, from + (to - from) * (i / 12.0), Qt::NoButton, Qt::LeftButton);
        spin(10);
    }
    CHECK(evaluate(shell, QStringLiteral("docking.draggedPanel")).toString() == QStringLiteral("program"));
    CHECK(dropIs("beside", "left"));
    mouseAt(shell.window.get(), QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
    spin(50);
    CHECK(evaluate(shell, QStringLiteral("docking.dragged === null")).toBool());
    QQuickItem* programAlone = groupOf(shell, "program");
    REQUIRE(programAlone != nullptr);
    CHECK(programAlone != groupOf(shell, "library"));
    CHECK(scenePos(programAlone).x() < scenePos(source).x());
    CHECK(shell.item("monitorPanel")->isVisible());
    CHECK(shell.item("libraryPanel")->isVisible());

    // A real drag of the timeline's grip onto the Library's group header: the timeline joins it as a tab.
    QQuickItem* grip = shell.item("dockGrip_timeline");
    REQUIRE(grip != nullptr);
    QQuickItem* library = groupOf(shell, "library");
    const QPointF gripFrom = grip->mapToScene(QPointF(grip->width() / 2, grip->height() / 2));
    const QPointF gripTo = library->mapToScene(QPointF(library->width() / 2, grid(3)));
    mouseAt(shell.window.get(), QEvent::MouseButtonPress, gripFrom, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 12; ++i) {
        mouseAt(shell.window.get(), QEvent::MouseMove, gripFrom + (gripTo - gripFrom) * (i / 12.0), Qt::NoButton, Qt::LeftButton);
        spin(10);
    }
    CHECK(dropIs("join"));
    mouseAt(shell.window.get(), QEvent::MouseButtonRelease, gripTo, Qt::LeftButton, Qt::NoButton);
    spin(50);
    CHECK(groupOf(shell, "timeline") == library);
    CHECK(shownPanel(shell, "library") == QStringLiteral("timeline"));
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/docking-timeline-tab.png")));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("A panel popped out into its own window gets the keys there and docks back when it closes",
          "[app][docking][window]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    spin(50);
    QQuickItem* program = groupOf(shell, "program");
    QQuickItem* source = groupOf(shell, "source");
    QQuickItem* area = shell.item("monitorArea");
    QQuickItem* monitorPanel = shell.item("monitorPanel");
    REQUIRE(program != nullptr);
    REQUIRE(monitorPanel != nullptr);
    REQUIRE(area->property("dual").toBool());

    const QPoint at = offScreen();
    evaluate(shell, QStringLiteral("docking.popOutAt(docking.slotOfPanel('program'), Qt.rect(%1, %2, 640, 400))")
                        .arg(at.x())
                        .arg(at.y()));
    spin(50);
    CHECK(program->property("floating").toBool());
    auto* floating = qobject_cast<QQuickWindow*>(program->property("floatWindow").value<QObject*>());
    REQUIRE(floating != nullptr);
    CHECK(monitorPanel->window() == floating);
    CHECK(floating->title().contains(QStringLiteral("Program Monitor")));
    // The Source Monitor has the monitor area to itself.
    CHECK_FALSE(area->property("dual").toBool());
    CHECK(source->isVisible());
    CHECK(std::abs(source->width() - area->width()) <= 1.0);
    const QString programGroup = groupId(shell, "program");
    const auto floatingKept = [&]() {
        const QJsonObject saved =
            QJsonDocument::fromJson(shell.preferences->property("workspaceLayout").toString().toUtf8()).object();
        return saved.value(QStringLiteral("workspaces")).toArray().at(0).toObject().value(QStringLiteral("floating")).toObject();
    };
    CHECK(floatingKept().contains(programGroup));

    // Shortcuts work in the floating window: Space is Play/Stop there too.
    auto* keymap = shell.engine.singletonInstance<KeymapDispatcher*>("Wabcut", "KeymapDispatcher");
    REQUIRE(keymap != nullptr);
    QStringList commands;
    const QMetaObject::Connection collect = QObject::connect(
        keymap, &KeymapDispatcher::commandTriggered, [&commands](const QString& id) { commands << id; });
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
    QCoreApplication::sendEvent(floating, &press);
    QCoreApplication::sendEvent(floating, &release);
    QObject::disconnect(collect);
    INFO(commands.join(QStringLiteral(", ")).toStdString());
    CHECK(commands.contains(QStringLiteral("transport.playStop")));

    // Closing the window (its title bar's close button) docks the monitor back.
    INFO("floating window visible: " << floating->isVisible());
    QCloseEvent close;
    QCoreApplication::sendEvent(floating, &close);
    spin(50);
    CHECK_FALSE(program->property("floating").toBool());
    CHECK(monitorPanel->window() == shell.window.get());
    CHECK(area->property("dual").toBool());
    CHECK(floatingKept().isEmpty());

    // Popped out again and its grip dragged near the Source Monitor's right edge: docked right of it again.
    evaluate(shell, QStringLiteral("docking.popOutAt(docking.slotOfPanel('program'), Qt.rect(%1, %2, 640, 400))")
                        .arg(at.x())
                        .arg(at.y()));
    spin(50);
    REQUIRE(program->property("floating").toBool());
    CHECK(shell.object("dockGrip_program") != nullptr);
    evaluate(shell, QStringLiteral("docking.beginDrag(docking.slotOfPanel('program'), '')"));
    evaluate(shell, QStringLiteral("docking.moveDrag(docking.slotOfPanel('program'), docking.slotOfPanel('source').mapToGlobal("
                                   "docking.slotOfPanel('source').width * 0.95, docking.slotOfPanel('source').height / 2), 0)"));
    CHECK(evaluate(shell, QStringLiteral("docking.drop.kind + '/' + docking.drop.side")).toString() ==
          QStringLiteral("beside/right"));
    evaluate(shell, QStringLiteral("docking.endDrag(docking.slotOfPanel('program'))"));
    spin(50);
    CHECK_FALSE(program->property("floating").toBool());
    CHECK(monitorPanel->window() == shell.window.get());
    CHECK(scenePos(program).x() > scenePos(source).x());
    CHECK(area->property("dual").toBool());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Docking: a press in a group makes it the active one and the Window menu opens closed panels there",
          "[app][docking][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    spin(50);

    // Startup focus (the timeline's lanes) makes no group the active one.
    CHECK(activeGroup(shell).isEmpty());
    QQuickItem* program = groupOf(shell, "program");
    REQUIRE(program != nullptr);
    const QString programId = groupId(shell, "program");
    QQuickItem* outline = findItem(shell, QStringLiteral("dockActiveOutline_") + programId);
    REQUIRE(outline != nullptr);
    CHECK_FALSE(outline->isVisible());

    // A press in the Program Monitor (its picture keeps the press: the group's glass only watches): its group is the
    // active one, with the accent outline.
    click(shell, program, QPointF(program->width() / 2, program->height() / 2));
    CHECK(activeGroup(shell) == programId);
    CHECK(outline->isVisible());
    QObject* pen = outline->property("border").value<QObject*>();
    REQUIRE(pen != nullptr);
    CHECK(sameColor(pen->property("color").value<QColor>(), shell.themeColor("accentStroke")));
    CHECK(pen->property("width").toDouble() == 2.0);
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/docking-active-group.png")));

    // Window > Markers (closed): a tab of the Program Monitor's group, shown there and focused.
    evaluate(shell, QStringLiteral("window.openPanelFromWindowMenu('markers')"));
    spin(100);
    CHECK(groupId(shell, "markers") == programId);
    CHECK(shownPanel(shell, "program") == QStringLiteral("markers"));
    CHECK(focusGroup(shell) == programId);

    // Window > Library (open elsewhere, behind Effects): it comes to the front where it is, focused, and its group is
    // the active one now.
    evaluate(shell, QStringLiteral("docking.joinPanel('effects', docking.groupOfPanel('library').group)"));
    spin(50);
    const QString libraryId = groupId(shell, "library");
    CHECK(shownPanel(shell, "library") == QStringLiteral("effects"));
    evaluate(shell, QStringLiteral("window.openPanelFromWindowMenu('library')"));
    spin(100);
    CHECK(groupId(shell, "library") == libraryId);
    CHECK(shownPanel(shell, "library") == QStringLiteral("library"));
    CHECK(activeGroup(shell) == libraryId);
    CHECK(focusGroup(shell) == libraryId);
    CHECK_FALSE(outline->isVisible());

    // A tab chosen makes its group the active one.
    QQuickItem* markersTab = findItem(shell, QStringLiteral("dockTab_markers"));
    REQUIRE(markersTab != nullptr);
    click(shell, markersTab, QPointF(grid(4), markersTab->height() / 2));
    CHECK(activeGroup(shell) == programId);

    // A drop makes the moved panel's group the active one: the Library's grip onto the Program Monitor's tabs.
    evaluate(shell, QStringLiteral("docking.beginDrag(docking.slotOfPanel('library'), '')"));
    evaluate(shell, QStringLiteral("docking.moveDrag(docking.slotOfPanel('library'), docking.slotOfPanel('program').mapToGlobal("
                                   "docking.slotOfPanel('program').width / 2, docking.slotOfPanel('program').height / 2), 0)"));
    evaluate(shell, QStringLiteral("docking.endDrag(docking.slotOfPanel('library'))"));
    spin(50);
    CHECK(groupId(shell, "library") == programId);
    CHECK(activeGroup(shell) == programId);
    evaluate(shell, QStringLiteral("docking.reset()"));
    spin(50);

    // The active group goes with its group (group ids come back).
    CHECK(activeGroup(shell).isEmpty()); // a reset lays the workspace out again
    click(shell, groupOf(shell, "source"), QPointF(40, 120));
    CHECK(activeGroup(shell) == groupId(shell, "source"));
    evaluate(shell, QStringLiteral("docking.closePanel('source')"));
    spin(50);
    CHECK(activeGroup(shell).isEmpty());

    // Without an active group the Window menu opens a panel where panels like it are: Scopes with the browser panels.
    evaluate(shell, QStringLiteral("window.openPanelFromWindowMenu('scopes')"));
    spin(100);
    CHECK(groupId(shell, "scopes") == groupId(shell, "library"));
    CHECK(activeGroup(shell) == groupId(shell, "library"));

    // Another workspace starts with none.
    evaluate(shell, QStringLiteral("window.openPreset('audio')"));
    spin(100);
    CHECK(activeGroup(shell).isEmpty());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Docking: Maximize Frame gives one group the whole work area and restores the layout", "[app][docking][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    spin(50);
    const double workWidth = evaluate(shell, QStringLiteral("workArea.width")).toDouble();
    const double workHeight = evaluate(shell, QStringLiteral("workArea.height")).toDouble();
    QQuickItem* timeline = groupOf(shell, "timeline");
    QQuickItem* library = groupOf(shell, "library");
    QQuickItem* program = groupOf(shell, "program");
    REQUIRE(timeline != nullptr);
    REQUIRE(library != nullptr);
    REQUIRE(program != nullptr);
    const QString timelineId = groupId(shell, "timeline");
    const QString programId = groupId(shell, "program");
    const QString layout = groupsText(shell);
    QQuickItem* toast = shell.item("toast");
    REQUIRE(toast != nullptr);
    const auto fills = [&](QQuickItem* slot) {
        return slot->isVisible() && std::abs(slot->width() - workWidth) <= 1.0 && std::abs(slot->height() - workHeight) <= 1.0;
    };

    // The keys: Shift+` (window.maximizeFrame); ` alone is a window shortcut (the group under the pointer).
    auto* keymap = shell.engine.singletonInstance<KeymapDispatcher*>("Wabcut", "KeymapDispatcher");
    REQUIRE(keymap != nullptr);
    CHECK(keymap->shortcutText(QStringLiteral("window.maximizeFrame")).contains(QStringLiteral("`")));

    // No group to maximize (none clicked, no focus in one): the toast says how to pick one, and nothing changes.
    evaluate(shell, QStringLiteral("window.toggleMaximizeFrame('')"));
    spin(50);
    CHECK(maximizedGroup(shell).isEmpty());
    CHECK(toast->property("shown").toBool());
    CHECK(toast->property("text").toString().contains(QStringLiteral("Click a panel")));

    // A click in the timeline, then Shift+`: the timeline takes the whole work area, the other groups hide.
    click(shell, timeline, QPointF(timeline->width() / 2, timeline->height() / 2));
    REQUIRE(activeGroup(shell) == timelineId);
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QVariant(QStringLiteral("window.maximizeFrame")))));
    spin(50);
    CHECK(maximizedGroup(shell) == timelineId);
    CHECK(fills(timeline));
    CHECK_FALSE(library->isVisible());
    CHECK_FALSE(program->isVisible());
    CHECK_FALSE(shell.item("libraryPanel")->isVisible());
    CHECK(evaluate(shell, QStringLiteral("workArea.dockLayout.gutters.length")).toInt() == 0);
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/docking-maximized.png")));
    // Again: the layout comes back as it was.
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QVariant(QStringLiteral("window.maximizeFrame")))));
    spin(50);
    CHECK(maximizedGroup(shell).isEmpty());
    CHECK(library->isVisible());
    CHECK(program->isVisible());
    CHECK(scenePos(timeline).y() > scenePos(program).y());
    CHECK(groupsText(shell) == layout);

    // ` (a window shortcut: a hidden test window never gets it from the keyboard, so it is activated here) with the
    // pointer over the Program Monitor: that group, not the active timeline; ` again restores.
    QObject* backtick = shell.object("maximizeFrameShortcut");
    REQUIRE(backtick != nullptr);
    CHECK(backtick->property("sequence").toString() == QStringLiteral("`"));
    hoverAt(shell, program, QPointF(program->width() / 2, program->height() / 2));
    CHECK(evaluate(shell, QStringLiteral("docking.groupUnderPointer()")).toString() == programId);
    REQUIRE(QMetaObject::invokeMethod(backtick, "activated"));
    spin(50);
    CHECK(maximizedGroup(shell) == programId);
    CHECK(fills(program));
    CHECK_FALSE(timeline->isVisible());
    REQUIRE(QMetaObject::invokeMethod(backtick, "activated"));
    spin(50);
    CHECK(maximizedGroup(shell).isEmpty());
    CHECK(timeline->isVisible());

    // A double-click on a lone panel's tab maximizes its group; again restores.
    QQuickItem* programTab = findUnder(program, QStringLiteral("panelTabArea"));
    REQUIRE(programTab != nullptr);
    doubleClick(shell, programTab, QPointF(programTab->width() / 2, programTab->height() / 2));
    CHECK(maximizedGroup(shell) == programId);
    doubleClick(shell, programTab, QPointF(programTab->width() / 2, programTab->height() / 2));
    CHECK(maximizedGroup(shell).isEmpty());

    // Maximized, the focus key of another group's panel restores the layout (the panel would stay hidden), and so
    // does a new group or another workspace.
    evaluate(shell, QStringLiteral("docking.toggleMaximize(docking.groupOfPanel('program').group)"));
    spin(30);
    REQUIRE(maximizedGroup(shell) == programId);
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showBrowserPanel", Q_ARG(QVariant, QVariant(QStringLiteral("markers")))));
    spin(50);
    CHECK(maximizedGroup(shell).isEmpty());
    CHECK(groupOf(shell, "markers") == library);
    CHECK(library->isVisible());
    evaluate(shell, QStringLiteral("docking.toggleMaximize(docking.groupOfPanel('program').group)"));
    spin(30);
    evaluate(shell, QStringLiteral("docking.detach('timeline')")); // a lone panel's group stays: nothing new
    CHECK(maximizedGroup(shell) == programId);
    evaluate(shell, QStringLiteral("docking.joinPanel('effects', docking.groupOfPanel('program').group)"));
    evaluate(shell, QStringLiteral("docking.detach('effects')")); // a new group beside it
    spin(30);
    CHECK(maximizedGroup(shell).isEmpty());
    evaluate(shell, QStringLiteral("docking.toggleMaximize(docking.groupOfPanel('program').group)"));
    REQUIRE(maximizedGroup(shell) == programId);
    evaluate(shell, QStringLiteral("window.openPreset('audio')"));
    spin(50);
    CHECK(maximizedGroup(shell).isEmpty());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Docking: the tab menu closes and undocks and maximizes and a lone panel's tab drags like a grouped one",
          "[app][docking][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    // Off every screen: the windows the menu pops panels out into open there (nothing on the desktop).
    shell.window->setPosition(offScreen());
    spin(50);

    // Effect Controls joins the Library's group: two tabs. The Library tab's right-click opens its menu.
    evaluate(shell, QStringLiteral("docking.joinPanel('effectControls', docking.groupOfPanel('library').group)"));
    spin(50);
    const QString libraryId = groupId(shell, "library");
    QQuickItem* libraryTab = findItem(shell, QStringLiteral("dockTab_library"));
    REQUIRE(libraryTab != nullptr);
    rightClick(shell, libraryTab, QPointF(grid(4), libraryTab->height() / 2));
    QObject* menu = tabMenuOf(shell, "library");
    REQUIRE(menu != nullptr);
    CHECK(menu->property("visible").toBool());
    CHECK(menu->property("panel").toString() == QStringLiteral("library"));
    CHECK(activeGroup(shell) == libraryId);
    CHECK(evaluate(shell, QStringLiteral("docking.slotOfPanel('library').tabMenu.several")).toBool());
    for (const char* name : {"dockTabMenuClose", "dockTabMenuCloseOthers", "dockTabMenuUndock", "dockTabMenuUndockGroup",
                             "dockTabMenuMaximize"}) {
        INFO(name);
        QObject* item = menu->findChild<QObject*>(QLatin1String(name));
        REQUIRE(item != nullptr);
        CHECK(item->property("enabled").toBool());
        CHECK_FALSE(item->property("text").toString().isEmpty());
    }
    CHECK(menu->findChild<QObject*>(QStringLiteral("dockTabMenuMaximize"))->property("text").toString() ==
          QStringLiteral("Maximize Frame"));
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/docking-tab-menu.png")));

    // Close Other Panels in Group: Effect Controls goes, the Library stays (alone, no tabs).
    chooseInMenu(menu, "dockTabMenuCloseOthers");
    CHECK_FALSE(evaluate(shell, QStringLiteral("docking.isOpen('effectControls')")).toBool());
    CHECK(evaluate(shell, QStringLiteral("docking.groupOfPanel('library').panels.length")).toInt() == 1);

    // A lone panel's tab has the menu too: Maximize Frame, then Restore Frame from it again.
    QQuickItem* libraryGroup = groupOf(shell, "library");
    QQuickItem* loneTab = findUnder(libraryGroup, QStringLiteral("panelTabArea"));
    REQUIRE(loneTab != nullptr);
    rightClick(shell, loneTab, QPointF(grid(4), loneTab->height() / 2));
    menu = tabMenuOf(shell, "library");
    REQUIRE(menu != nullptr);
    REQUIRE(menu->property("visible").toBool());
    CHECK_FALSE(menu->findChild<QObject*>(QStringLiteral("dockTabMenuCloseOthers"))->property("enabled").toBool());
    chooseInMenu(menu, "dockTabMenuMaximize");
    CHECK(maximizedGroup(shell) == libraryId);
    rightClick(shell, loneTab, QPointF(grid(4), loneTab->height() / 2));
    REQUIRE(menu->property("visible").toBool());
    CHECK(menu->findChild<QObject*>(QStringLiteral("dockTabMenuMaximize"))->property("text").toString() ==
          QStringLiteral("Restore Frame"));
    chooseInMenu(menu, "dockTabMenuMaximize");
    CHECK(maximizedGroup(shell).isEmpty());

    // Undock Panel Group: the group in a window of its own; its menu there docks it back.
    rightClick(shell, loneTab, QPointF(grid(4), loneTab->height() / 2));
    chooseInMenu(menu, "dockTabMenuUndockGroup");
    CHECK(libraryGroup->property("floating").toBool());
    evaluate(shell, QStringLiteral("docking.slotOfPanel('library').openTabMenu('library', docking.slotOfPanel('library').holder, 20, 20)"));
    REQUIRE(waitFor([&] { return menu->property("visible").toBool(); }));
    CHECK(menu->findChild<QObject*>(QStringLiteral("dockTabMenuUndockGroup"))->property("text").toString() ==
          QStringLiteral("Dock Panel Group"));
    CHECK_FALSE(menu->findChild<QObject*>(QStringLiteral("dockTabMenuMaximize"))->property("enabled").toBool());
    chooseInMenu(menu, "dockTabMenuUndockGroup");
    CHECK_FALSE(libraryGroup->property("floating").toBool());

    // Undock Panel in a group of two: that panel alone in a window of its own; the other one stays docked.
    evaluate(shell, QStringLiteral("docking.joinPanel('markers', docking.groupOfPanel('library').group)"));
    spin(50);
    QQuickItem* markersTab = findItem(shell, QStringLiteral("dockTab_markers"));
    REQUIRE(markersTab != nullptr);
    rightClick(shell, markersTab, QPointF(grid(4), markersTab->height() / 2));
    menu = tabMenuOf(shell, "library");
    REQUIRE(menu != nullptr);
    chooseInMenu(menu, "dockTabMenuUndock");
    QQuickItem* markersGroup = groupOf(shell, "markers");
    REQUIRE(markersGroup != nullptr);
    CHECK(markersGroup != groupOf(shell, "library"));
    CHECK(markersGroup->property("floating").toBool());
    CHECK_FALSE(groupOf(shell, "library")->property("floating").toBool());
    CHECK(activeGroup(shell) == groupId(shell, "markers"));

    // Close Panel (the window's lone tab): the panel closes and its window goes.
    evaluate(shell, QStringLiteral("docking.slotOfPanel('markers').openTabMenu('markers', docking.slotOfPanel('markers').holder, 20, 20)"));
    menu = tabMenuOf(shell, "markers");
    REQUIRE(menu != nullptr);
    REQUIRE(waitFor([&] { return menu->property("visible").toBool(); }));
    chooseInMenu(menu, "dockTabMenuClose");
    CHECK_FALSE(evaluate(shell, QStringLiteral("docking.isOpen('markers')")).toBool());

    // A lone panel's tab drags like a grouped one: the Program Monitor's, released near the Source Monitor's left edge.
    QQuickItem* source = groupOf(shell, "source");
    QQuickItem* program = groupOf(shell, "program");
    QQuickItem* programTab = findUnder(program, QStringLiteral("panelTabArea"));
    REQUIRE(programTab != nullptr);
    const QPointF from = programTab->mapToScene(QPointF(grid(4), programTab->height() / 2));
    const QPointF to = source->mapToScene(QPointF(source->width() * 0.08, source->height() / 2));
    mouseAt(shell.window.get(), QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 12; ++i) {
        mouseAt(shell.window.get(), QEvent::MouseMove, from + (to - from) * (i / 12.0), Qt::NoButton, Qt::LeftButton);
        spin(10);
    }
    CHECK(evaluate(shell, QStringLiteral("docking.draggedPanel")).toString() == QStringLiteral("program"));
    CHECK(evaluate(shell, QStringLiteral("docking.drop.kind + '/' + (docking.drop.side ?? '')")).toString() ==
          QStringLiteral("beside/left"));
    mouseAt(shell.window.get(), QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
    spin(50);
    CHECK(scenePos(groupOf(shell, "program")).x() < scenePos(source).x());
    CHECK(activeGroup(shell) == groupId(shell, "program"));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Docking: Ctrl+W closes the active panel and Shift+5 focuses Effect Controls and Reset to Saved Layout undoes",
          "[app][docking][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    spin(50);
    auto* keymap = shell.engine.singletonInstance<KeymapDispatcher*>("Wabcut", "KeymapDispatcher");
    REQUIRE(keymap != nullptr);
    CHECK(keymap->shortcutText(QStringLiteral("file.close")) == QStringLiteral("Ctrl+W"));
    CHECK(keymap->shortcutText(QStringLiteral("window.effectControls")) == QStringLiteral("Shift+5"));
    CHECK_FALSE(keymap->shortcutText(QStringLiteral("window.resetToSavedLayout")).isEmpty()); // Alt+Shift+0
    const auto run = [&](const char* id) {
        REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QVariant(QString::fromLatin1(id)))));
        spin(80);
    };

    // Ctrl+W: the panel the active group shows closes.
    QQuickItem* library = groupOf(shell, "library");
    click(shell, library, QPointF(library->width() / 2, library->height() / 2));
    run("file.close");
    CHECK_FALSE(evaluate(shell, QStringLiteral("docking.isOpen('library')")).toBool());
    CHECK(activeGroup(shell).isEmpty()); // its group went with it

    // Shift+5: Effect Controls shown and focused; its group is the active one.
    evaluate(shell, QStringLiteral("docking.joinPanel('markers', docking.groupOfPanel('effectControls').group)"));
    spin(50);
    REQUIRE(evaluate(shell, QStringLiteral("docking.shownPanel(docking.groupOfPanel('effectControls').group)")).toString() ==
            QStringLiteral("markers"));
    run("window.effectControls");
    const QString controlsId = groupId(shell, "effectControls");
    CHECK(shownPanel(shell, "effectControls") == QStringLiteral("effectControls"));
    CHECK(activeGroup(shell) == controlsId);
    CHECK(focusGroup(shell) == controlsId);
    CHECK(evaluate(shell, QStringLiteral("KeymapDispatcher.scopeAt(window.activeFocusItem)")).toString() ==
          QStringLiteral("effectControls"));

    // Window > Workspaces > Reset to Saved Layout (Alt+Shift+0): the preset's layout; Undo on the toast puts this one
    // back.
    const QString changed = groupsText(shell);
    REQUIRE(changed == QStringLiteral("source program effectControls+markers timeline"));
    run("window.resetToSavedLayout");
    CHECK(groupsText(shell) == QStringLiteral("library source program effectControls timeline"));
    QQuickItem* toast = shell.item("toast");
    REQUIRE(toast != nullptr);
    CHECK(toast->property("shown").toBool());
    CHECK(toast->property("actionText").toString() == QStringLiteral("Undo"));
    REQUIRE(QMetaObject::invokeMethod(toast, "runAction"));
    spin(80);
    CHECK(groupsText(shell) == changed);
    CHECK_FALSE(toast->property("shown").toBool());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Docking: two monitors sharing a narrow place take presses and drops and the Window menu only for the one shown",
          "[app][docking][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    // Narrow: the Edit workspace's Source and Program Monitors share one place (one shows, with a switch).
    shell.resize(shell.theme->property("windowMinWidth").toInt(), 760);
    spin(80);
    QQuickItem* area = shell.item("monitorArea");
    REQUIRE(area != nullptr);
    REQUIRE(area->property("single").toBool());
    QQuickItem* source = groupOf(shell, "source");
    QQuickItem* program = groupOf(shell, "program");
    QQuickItem* programPanel = shell.item("monitorPanel");
    REQUIRE(source != nullptr);
    REQUIRE(program != nullptr);
    REQUIRE(programPanel != nullptr);
    const QString sourceId = groupId(shell, "source");
    const QString programId = groupId(shell, "program");
    const QPointF middle(program->width() / 2, program->height() / 2);

    // The Program Monitor shows; the Source Monitor's group hides with its monitor (it lay over or under the shown
    // one, at the same place, and took its presses, hover and drops).
    CHECK(evaluate(shell, QStringLiteral("docking.sharedGroups.length")).toInt() == 2);
    CHECK(area->property("current").toString() == QStringLiteral("program"));
    CHECK(evaluate(shell, QStringLiteral("monitorArea.hiddenGroup")).toString() == sourceId);
    CHECK(program->isVisible());
    CHECK_FALSE(source->isVisible());

    // The pointer over it: the group under the pointer is the Program Monitor's (` maximizes that one).
    hoverAt(shell, program, middle);
    CHECK(evaluate(shell, QStringLiteral("docking.groupUnderPointer()")).toString() == programId);
    // A panel dragged over its middle: a tab there would split the two monitors into narrow columns, so nothing happens
    // there and the overlay says why.
    evaluate(shell, QStringLiteral("docking.beginDrag(docking.slotOfPanel('library'), '')"));
    evaluate(shell, QStringLiteral("docking.moveDrag(docking.slotOfPanel('library'), docking.slotOfPanel('program').mapToGlobal("
                                   "docking.slotOfPanel('program').width / 2, docking.slotOfPanel('program').height / 2), 0)"));
    CHECK(evaluate(shell, QStringLiteral("docking.drop.kind")).toString() == QStringLiteral("none"));
    CHECK(evaluate(shell, QStringLiteral("docking.drop.shared === true")).toBool());
    CHECK(evaluate(shell, QStringLiteral("docking.dropText")).toString().contains(QStringLiteral("share")));
    // Over its tabs: the same.
    evaluate(shell, QStringLiteral("docking.moveDrag(docking.slotOfPanel('library'), docking.slotOfPanel('program').mapToGlobal("
                                   "docking.slotOfPanel('program').width / 2, docking.edgeBand + 8), 0)"));
    INFO(evaluate(shell, QStringLiteral("JSON.stringify({ kind: docking.drop.kind, point: docking.dragPoint, "
                                        "top: docking.slotOfPanel('program').mapToItem(docking.area, 0, 0) })")).toString().toStdString());
    CHECK(evaluate(shell, QStringLiteral("docking.drop.kind")).toString() == QStringLiteral("none"));
    // Near its right side: beside the pair's right-hand monitor, so the two keep sharing their place.
    evaluate(shell, QStringLiteral("docking.moveDrag(docking.slotOfPanel('library'), docking.slotOfPanel('program').mapToGlobal("
                                   "docking.slotOfPanel('program').width * 0.95, docking.slotOfPanel('program').height / 2), 0)"));
    CHECK(evaluate(shell, QStringLiteral("docking.drop.kind")).toString() == QStringLiteral("beside"));
    CHECK(evaluate(shell, QStringLiteral("docking.drop.side")).toString() == QStringLiteral("right"));
    CHECK(evaluate(shell, QStringLiteral("docking.drop.target.dockKey")).toString()
          == evaluate(shell, QStringLiteral("docking.sharedGroups[1]")).toString());
    // Near its left side: beside the pair's left-hand monitor.
    evaluate(shell, QStringLiteral("docking.moveDrag(docking.slotOfPanel('library'), docking.slotOfPanel('program').mapToGlobal("
                                   "docking.slotOfPanel('program').width * 0.05, docking.slotOfPanel('program').height / 2), 0)"));
    CHECK(evaluate(shell, QStringLiteral("docking.drop.kind")).toString() == QStringLiteral("beside"));
    CHECK(evaluate(shell, QStringLiteral("docking.drop.side")).toString() == QStringLiteral("left"));
    CHECK(evaluate(shell, QStringLiteral("docking.drop.target.dockKey")).toString()
          == evaluate(shell, QStringLiteral("docking.sharedGroups[0]")).toString());
    // Released over the Library itself: nothing moves.
    evaluate(shell, QStringLiteral("docking.moveDrag(docking.slotOfPanel('library'), docking.slotOfPanel('library').mapToGlobal("
                                   "docking.slotOfPanel('library').width / 2, docking.slotOfPanel('library').height / 2), 0)"));
    evaluate(shell, QStringLiteral("docking.endDrag(docking.slotOfPanel('library'))"));
    spin(30);
    REQUIRE(area->property("single").toBool());

    // A press on it makes its group the active one (the outline on the monitor shown).
    click(shell, program, middle);
    CHECK(activeGroup(shell) == programId);
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/docking-shared-monitors.png")));

    // The switch in its header: the Source Monitor shows, its group is the active one now, and the keyboard focus that
    // was in the Program Monitor goes along (its keys would act on the monitor that hides).
    evaluate(shell, QStringLiteral("monitor.focusMonitor()"));
    spin(30);
    REQUIRE(focusGroup(shell) == programId);
    QQuickItem* programSwitch = findUnder(programPanel, QStringLiteral("monitorSwitch"));
    REQUIRE(programSwitch != nullptr);
    REQUIRE(QMetaObject::invokeMethod(programSwitch, "activated", Q_ARG(int, 0)));
    spin(50);
    CHECK(area->property("current").toString() == QStringLiteral("source"));
    CHECK(source->isVisible());
    CHECK_FALSE(program->isVisible());
    CHECK(activeGroup(shell) == sourceId);
    CHECK(focusGroup(shell) == sourceId);
    click(shell, source, middle);
    CHECK(activeGroup(shell) == sourceId);
    hoverAt(shell, source, middle);
    CHECK(evaluate(shell, QStringLiteral("docking.groupUnderPointer()")).toString() == sourceId);
    // ` over it maximizes the monitor shown; again restores, and the monitors share their place again.
    QObject* backtick = shell.object("maximizeFrameShortcut");
    REQUIRE(backtick != nullptr);
    REQUIRE(QMetaObject::invokeMethod(backtick, "activated"));
    spin(50);
    CHECK(maximizedGroup(shell) == sourceId);
    REQUIRE(QMetaObject::invokeMethod(backtick, "activated"));
    spin(50);
    CHECK(maximizedGroup(shell).isEmpty());
    REQUIRE(area->property("single").toBool());

    // A switch that does not come from the user (a clip loaded into the Source Monitor shows it; here back to the
    // Program Monitor): the active group does not stay on the monitor that hides.
    REQUIRE(focusGroup(shell) == sourceId);
    evaluate(shell, QStringLiteral("monitorArea.current = 'program'"));
    spin(30);
    CHECK(activeGroup(shell) == programId);
    CHECK(focusGroup(shell) == programId);

    // Window > Markers while that shared monitor is the active group: not a tab of it (that would split the monitors
    // into two narrow columns) but where panels like it are; the monitors keep sharing their place.
    evaluate(shell, QStringLiteral("window.openPanelFromWindowMenu('markers')"));
    spin(100);
    CHECK(groupId(shell, "markers") != programId);
    CHECK(groupId(shell, "markers") == groupId(shell, "library"));
    CHECK(evaluate(shell, QStringLiteral("docking.groupOfPanel('program').panels.length")).toInt() == 1);
    CHECK(area->property("single").toBool());
    CHECK(activeGroup(shell) == groupId(shell, "library"));

    // Wide again: each monitor has its place, nothing is hidden, and nothing is shared.
    shell.resize(kShotWidth, kShotHeight);
    spin(80);
    CHECK(area->property("dual").toBool());
    CHECK(source->isVisible());
    CHECK(program->isVisible());
    CHECK(evaluate(shell, QStringLiteral("monitorArea.hiddenGroup")).toString().isEmpty());
    CHECK(evaluate(shell, QStringLiteral("docking.sharedGroups.length")).toInt() == 0);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Docking: every way to maximize moves the focus into the frame and a group docked back from its window shows",
          "[app][docking][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    // Off every screen: the windows panels pop out into open there (nothing on the desktop).
    shell.window->setPosition(offScreen());
    spin(50);
    QQuickItem* timeline = groupOf(shell, "timeline");
    QQuickItem* program = groupOf(shell, "program");
    QQuickItem* library = groupOf(shell, "library");
    REQUIRE(timeline != nullptr);
    REQUIRE(program != nullptr);
    REQUIRE(library != nullptr);
    const QString timelineId = groupId(shell, "timeline");
    const QString programId = groupId(shell, "program");
    const QString libraryId = groupId(shell, "library");
    const auto run = [&](const char* id) {
        REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QVariant(QString::fromLatin1(id)))));
        spin(80);
    };
    const auto focusTimeline = [&] {
        evaluate(shell, QStringLiteral("timeline.focusLanes()"));
        spin(50);
        REQUIRE(focusGroup(shell) == timelineId);
    };

    // The keyboard focus in the timeline's lanes, a double-click on the Program Monitor's tab maximizes it: the focus
    // goes along (keys must not edit the timeline that hides).
    focusTimeline();
    QQuickItem* programTab = findUnder(program, QStringLiteral("panelTabArea"));
    REQUIRE(programTab != nullptr);
    doubleClick(shell, programTab, QPointF(programTab->width() / 2, programTab->height() / 2));
    spin(50);
    REQUIRE(maximizedGroup(shell) == programId);
    CHECK_FALSE(timeline->isVisible());
    CHECK(focusGroup(shell) == programId);
    doubleClick(shell, programTab, QPointF(programTab->width() / 2, programTab->height() / 2));
    spin(50);
    REQUIRE(maximizedGroup(shell).isEmpty());

    // The tab menu's Maximize Frame too (the Library's lone tab); its Restore Frame gives the layout back.
    focusTimeline();
    QQuickItem* libraryTab = findUnder(library, QStringLiteral("panelTabArea"));
    REQUIRE(libraryTab != nullptr);
    rightClick(shell, libraryTab, QPointF(grid(4), libraryTab->height() / 2));
    QObject* menu = tabMenuOf(shell, "library");
    REQUIRE(menu != nullptr);
    chooseInMenu(menu, "dockTabMenuMaximize");
    REQUIRE(maximizedGroup(shell) == libraryId);
    CHECK(focusGroup(shell) == libraryId);
    rightClick(shell, libraryTab, QPointF(grid(4), libraryTab->height() / 2));
    chooseInMenu(menu, "dockTabMenuMaximize");
    REQUIRE(maximizedGroup(shell).isEmpty());

    // A floating group is the active one (a press in its window): Shift+` (Window > Maximize Frame) still restores a
    // maximized frame - and with none maximized it leaves the floating group in its window.
    evaluate(shell, QStringLiteral("docking.popOut(docking.slotOfPanel('library'))"));
    spin(50);
    REQUIRE(library->property("floating").toBool());
    click(shell, timeline, QPointF(timeline->width() / 2, timeline->height() / 2));
    run("window.maximizeFrame");
    REQUIRE(maximizedGroup(shell) == timelineId);
    evaluate(shell, QStringLiteral("docking.setActiveGroup(docking.groupOfPanel('library').group)"));
    run("window.maximizeFrame");
    CHECK(maximizedGroup(shell).isEmpty());
    CHECK(program->isVisible());
    CHECK(timeline->isVisible());
    run("window.maximizeFrame");
    CHECK(maximizedGroup(shell).isEmpty());
    CHECK(library->property("floating").toBool());

    // A floating group docked back while a frame is maximized shows again (the layout comes back): its window closed
    // (the window's close button, Dock Panel Group) ...
    const auto maximizeTimeline = [&] {
        click(shell, timeline, QPointF(timeline->width() / 2, timeline->height() / 2));
        run("window.maximizeFrame");
        REQUIRE(maximizedGroup(shell) == timelineId);
    };
    const auto popOutLibrary = [&] {
        evaluate(shell, QStringLiteral("docking.popOut(docking.slotOfPanel('library'))"));
        spin(50);
        REQUIRE(library->property("floating").toBool());
    };
    maximizeTimeline();
    auto* floating = qobject_cast<QQuickWindow*>(library->property("floatWindow").value<QObject*>());
    REQUIRE(floating != nullptr);
    QCloseEvent close;
    QCoreApplication::sendEvent(floating, &close);
    spin(50);
    CHECK_FALSE(library->property("floating").toBool());
    CHECK(maximizedGroup(shell).isEmpty());
    CHECK(library->isVisible());
    popOutLibrary();
    maximizeTimeline();
    evaluate(shell, QStringLiteral("docking.tabMenuAction('undockGroup', docking.groupOfPanel('library').group, 'library')"));
    spin(50);
    CHECK_FALSE(library->property("floating").toBool());
    CHECK(maximizedGroup(shell).isEmpty());
    CHECK(library->isVisible());
    // ... its grip dropped beside the maximized timeline (it becomes the active group: a visible one) ...
    popOutLibrary();
    maximizeTimeline();
    evaluate(shell, QStringLiteral("docking.beginDrag(docking.slotOfPanel('library'), '')"));
    evaluate(shell, QStringLiteral("docking.moveDrag(docking.slotOfPanel('library'), docking.slotOfPanel('timeline').mapToGlobal("
                                   "docking.slotOfPanel('timeline').width * 0.05, docking.slotOfPanel('timeline').height / 2), 0)"));
    CHECK(evaluate(shell, QStringLiteral("docking.drop.kind + '/' + docking.drop.side")).toString() ==
          QStringLiteral("beside/left"));
    evaluate(shell, QStringLiteral("docking.endDrag(docking.slotOfPanel('library'))"));
    spin(50);
    CHECK_FALSE(library->property("floating").toBool());
    CHECK(maximizedGroup(shell).isEmpty());
    CHECK(library->isVisible());
    CHECK(activeGroup(shell) == libraryId);
    CHECK(scenePos(library).x() < scenePos(timeline).x());
    // ... or along an edge of the work area.
    popOutLibrary();
    maximizeTimeline();
    evaluate(shell, QStringLiteral("docking.putAtEdge(docking.groupOfPanel('library').group, '', 'right')"));
    spin(50);
    CHECK_FALSE(library->property("floating").toBool());
    CHECK(maximizedGroup(shell).isEmpty());
    CHECK(library->isVisible());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Docking: crowded tab groups keep readable tabs and list every panel behind the tab list button",
          "[app][docking][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    shell.resize(1100, 700);
    spin(50);

    // Six panels in the Library's group.
    for (const char* key : {"effectControls", "markers", "mediaBrowser", "metadata", "effects"})
        evaluate(shell, QStringLiteral("docking.joinPanel('%1', docking.groupOfPanel('library').group)").arg(QLatin1String(key)));
    spin(50);
    const QString gid = groupId(shell, "library");
    REQUIRE(evaluate(shell, QStringLiteral("docking.groupOfPanel('library').panels.length")).toInt() == 6);
    QQuickItem* group = groupOf(shell, "library");
    REQUIRE(group != nullptr);
    INFO("group width " << group->width());
    QQuickItem* tabs = findItem(shell, QStringLiteral("dockTabs_") + gid);
    REQUIRE(tabs != nullptr);
    const QStringList panels = evaluate(shell, QStringLiteral("docking.groupOfPanel('library').panels")).toStringList();
    REQUIRE(panels.size() == 6);
    const auto inTabs = [&](const QString& code) {
        QQmlExpression expression(qmlContext(tabs), tabs, code);
        const QVariant result = expression.evaluate();
        INFO(code.toStdString() << ": " << expression.error().toString().toStdString());
        REQUIRE_FALSE(expression.hasError());
        return result;
    };
    const auto current = [&] { return tabs->property("current").toString(); };

    // Not every tab fits: the tab list button shows; every shown tab keeps its dot and four characters (the shown one
    // its × too), and the shown one is always among them.
    CHECK(tabs->property("overflowing").toBool());
    QQuickItem* overflow = findUnder(tabs, QStringLiteral("dockTabsOverflow"));
    REQUIRE(overflow != nullptr);
    CHECK(overflow->isVisible());
    int shownTabs = 0;
    for (int i = 0; i < panels.size(); ++i) {
        QQuickItem* tab = findUnder(tabs, QStringLiteral("dockTab_") + panels[i]);
        REQUIRE(tab != nullptr);
        INFO(panels[i].toStdString());
        if (!tab->isVisible())
            continue;
        ++shownTabs;
        const double minimum = inTabs(QStringLiteral("minTabWidth(%1)").arg(i)).toDouble();
        CHECK(tab->width() >= minimum - 0.5);
        // Its label shows at least its first four characters.
        QQuickItem* text = nullptr;
        QList<QQuickItem*> stack{tab};
        while (!stack.isEmpty() && text == nullptr) {
            QQuickItem* item = stack.takeLast();
            if (QString::fromLatin1(item->metaObject()->className()).startsWith(QLatin1String("QQuickText"))
                && item->property("text").toString() == tab->property("label").toString())
                text = item;
            stack << item->childItems();
        }
        REQUIRE(text != nullptr);
        const QFontMetricsF fm(text->property("font").value<QFont>());
        CHECK(text->width() + 1.0 >= fm.horizontalAdvance(tab->property("label").toString().left(4)));
    }
    CHECK(shownTabs >= 1);
    CHECK(shownTabs < 6);
    CHECK(findUnder(tabs, QStringLiteral("dockTab_") + current())->isVisible());
    // The tab's tooltip names the panel and its focus key.
    QQuickItem* libraryTab = findUnder(tabs, QStringLiteral("dockTab_library"));
    REQUIRE(libraryTab != nullptr);
    const QString key = evaluate(shell, QStringLiteral("KeymapDispatcher.shortcutText('window.projects')")).toString();
    CHECK(libraryTab->property("tooltipText").toString() ==
          (key.isEmpty() ? QStringLiteral("Library") : QStringLiteral("Library  [%1]").arg(key)));
    spin(200);
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/docking-tabs-overflow.png")));

    // The tab list: every panel of the group in tab order, the shown one checked.
    REQUIRE(QMetaObject::invokeMethod(overflow, "clicked"));
    QObject* menu = nullptr;
    REQUIRE(waitFor([&] {
        menu = tabs->findChild<QObject*>(QStringLiteral("dockTabsOverflowMenu"));
        return menu != nullptr && menu->property("visible").toBool();
    }));
    CHECK(menu->property("count").toInt() == 6);
    for (const QString& panel : panels) {
        QObject* row = menu->findChild<QObject*>(QStringLiteral("dockTabsOverflow_") + panel);
        INFO(panel.toStdString());
        REQUIRE(row != nullptr);
        CHECK(row->property("checked").toBool() == (panel == current()));
    }
    // Choosing a hidden one shows it: its tab is there now.
    QString hidden;
    for (const QString& panel : panels) {
        if (!findUnder(tabs, QStringLiteral("dockTab_") + panel)->isVisible()) {
            hidden = panel;
            break;
        }
    }
    REQUIRE_FALSE(hidden.isEmpty());
    chooseInMenu(menu, ("dockTabsOverflow_" + hidden.toStdString()).c_str());
    CHECK(current() == hidden);
    CHECK(shownPanel(shell, "library") == hidden);
    CHECK(findUnder(tabs, QStringLiteral("dockTab_") + hidden)->isVisible());

    // Fewer panels in the group: every tab shows again, no tab list.
    for (const char* closing : {"effects", "metadata", "mediaBrowser", "markers"})
        evaluate(shell, QStringLiteral("docking.closePanel('%1')").arg(QLatin1String(closing)));
    spin(100);
    REQUIRE(evaluate(shell, QStringLiteral("docking.groupOfPanel('library').panels.length")).toInt() == 2);
    CHECK_FALSE(tabs->property("overflowing").toBool());
    CHECK_FALSE(overflow->isVisible());
    CHECK(findUnder(tabs, QStringLiteral("dockTab_library"))->isVisible());
    CHECK(findUnder(tabs, QStringLiteral("dockTab_effectControls"))->isVisible());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Docking: a lone panel tab opens its tab menu from the keyboard", "[app][docking][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    spin(50);
    QQuickItem* libraryGroup = groupOf(shell, "library");
    REQUIRE(libraryGroup != nullptr);
    QQuickItem* loneTab = findUnder(libraryGroup, QStringLiteral("panelTabArea"));
    REQUIRE(loneTab != nullptr);
    CHECK(loneTab->activeFocusOnTab());
    CHECK(loneTab->property("tooltipText").toString().startsWith(QStringLiteral("Library")));
    const auto press = [&](int key, Qt::KeyboardModifiers modifiers) {
        loneTab->forceActiveFocus(Qt::TabFocusReason);
        REQUIRE(loneTab->hasActiveFocus());
        QKeyEvent down(QEvent::KeyPress, key, modifiers);
        QCoreApplication::sendEvent(shell.window.get(), &down);
        QKeyEvent up(QEvent::KeyRelease, key, modifiers);
        QCoreApplication::sendEvent(shell.window.get(), &up);
        spin(50);
    };

    // Shift+F10 opens the lone tab's menu.
    press(Qt::Key_F10, Qt::ShiftModifier);
    QObject* menu = tabMenuOf(shell, "library");
    REQUIRE(menu != nullptr);
    REQUIRE(waitFor([&] { return menu->property("visible").toBool(); }));
    CHECK(menu->property("panel").toString() == QStringLiteral("library"));
    REQUIRE(QMetaObject::invokeMethod(menu, "close"));
    REQUIRE(waitFor([&] { return !menu->property("visible").toBool(); }));
    // The Menu key too.
    press(Qt::Key_Menu, Qt::NoModifier);
    REQUIRE(waitFor([&] { return menu->property("visible").toBool(); }));
    REQUIRE(QMetaObject::invokeMethod(menu, "close"));
    REQUIRE(waitFor([&] { return !menu->property("visible").toBool(); }));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Panel header: tools that do not fit go into the More tools menu", "[app][docking][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    shell.resize(kShotWidth, kShotHeight);
    spin(50);
    QQuickItem* panel = shell.item("libraryPanel");
    REQUIRE(panel != nullptr);
    QQuickItem* group = groupOf(shell, "library");
    REQUIRE(group != nullptr);
    INFO("library group width " << group->width());
    QObject* library = evaluate(shell, QStringLiteral("Library")).value<QObject*>();
    REQUIRE(library != nullptr);
    const auto hiddenNames = [&] {
        QQmlExpression expression(qmlContext(panel), panel, QStringLiteral("hiddenTools.map(t => t.objectName)"));
        const QVariant result = expression.evaluate();
        INFO(expression.error().toString().toStdString());
        REQUIRE_FALSE(expression.hasError());
        QStringList names;
        const QVariant list = result.canConvert<QJSValue>() && result.metaType() == QMetaType::fromType<QJSValue>()
                                  ? result.value<QJSValue>().toVariant()
                                  : result;
        for (const QVariant& v : list.toList())
            names << v.toString();
        return names;
    };

    // The Library at about 340 px: Import (the trailing row's last tool) and its neighbours show; the rest went into
    // the "..." menu.
    QQuickItem* overflow = findUnder(panel, QStringLiteral("panelToolsOverflow"));
    REQUIRE(overflow != nullptr);
    CHECK(overflow->isVisible());
    const QStringList hidden = hiddenNames();
    INFO(hidden.join(QLatin1Char(' ')).toStdString());
    REQUIRE_FALSE(hidden.isEmpty());
    CHECK(hidden.contains(QStringLiteral("libraryListButton")));
    QQuickItem* gridButton = shell.item("libraryGridButton");
    QQuickItem* moreButton = findUnder(panel, QStringLiteral("libraryMoreButton"));
    REQUIRE(gridButton != nullptr);
    REQUIRE(moreButton != nullptr);
    // A hidden tool sits in a 0-wide slot and leaves the Tab chain; a shown one keeps its room.
    CHECK(gridButton->parentItem()->width() == 0);
    CHECK(gridButton->property("focusPolicy").toInt() == Qt::NoFocus);
    CHECK(moreButton->parentItem()->width() > 0);
    spin(200);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    const QRectF rect = panel->mapRectToScene(QRectF(0, 0, panel->width(), 120)).intersected(QRectF(0, 0, kShotWidth, kShotHeight));
    REQUIRE(image.copy(QRectF(rect.topLeft() * image.devicePixelRatio(), rect.size() * image.devicePixelRatio()).toRect())
                .save(outputDir() + QStringLiteral("/panel-tools-overflow.png")));

    // Its menu lists them (on-screen order) with their names; List view switches the Library to the list once the menu
    // has closed.
    REQUIRE(QMetaObject::invokeMethod(overflow, "clicked"));
    QObject* menu = nullptr;
    REQUIRE(waitFor([&] {
        menu = panel->findChild<QObject*>(QStringLiteral("panelToolsMenu"));
        return menu != nullptr && menu->property("visible").toBool();
    }));
    CHECK(menu->property("count").toInt() == hidden.size());
    QObject* listRow = menu->findChild<QObject*>(QStringLiteral("panelToolsMenu_libraryListButton"));
    REQUIRE(listRow != nullptr);
    CHECK(listRow->property("text").toString() == QStringLiteral("List view"));
    CHECK(listRow->property("checkable").toBool());
    chooseInMenu(menu, "panelToolsMenu_libraryListButton");
    REQUIRE(waitFor([&] { return library->property("view").toString() == QStringLiteral("list"); }));

    // A wider group: everything fits again, the "..." button goes and the tools are back in the Tab chain.
    shell.resize(2560, 1200);
    spin(100);
    CHECK(hiddenNames().isEmpty());
    CHECK_FALSE(overflow->isVisible());
    CHECK(gridButton->parentItem()->width() > 0);
    CHECK(gridButton->property("focusPolicy").toInt() == Qt::StrongFocus);
    library->setProperty("view", QStringLiteral("grid"));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Panel header: More tools rows act like a click and hidden tools leave the Tab chain", "[app][docking][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    shell.resize(kShotWidth, kShotHeight);
    spin(50);
    const auto inPanel = [&](QQuickItem* panel, const QString& code) {
        QQmlExpression expression(qmlContext(panel), panel, code);
        const QVariant result = expression.evaluate();
        INFO(code.toStdString() << ": " << expression.error().toString().toStdString());
        REQUIRE_FALSE(expression.hasError());
        return result.canConvert<QJSValue>() && result.metaType() == QMetaType::fromType<QJSValue>()
                   ? result.value<QJSValue>().toVariant()
                   : result;
    };
    const auto hiddenNames = [&](QQuickItem* panel) {
        QStringList names;
        for (const QVariant& v : inPanel(panel, QStringLiteral("hiddenTools.map(t => t.objectName)")).toList())
            names << v.toString();
        return names;
    };
    const auto openMoreTools = [&](QQuickItem* panel) {
        QQuickItem* overflow = findUnder(panel, QStringLiteral("panelToolsOverflow"));
        REQUIRE(overflow != nullptr);
        REQUIRE(overflow->isVisible());
        REQUIRE(QMetaObject::invokeMethod(overflow, "clicked"));
        QObject* menu = nullptr;
        REQUIRE(waitFor([&] {
            menu = panel->findChild<QObject*>(QStringLiteral("panelToolsMenu"));
            return menu != nullptr && menu->property("visible").toBool();
        }));
        return menu;
    };

    // The Media Browser and Effect Controls join the Library's group (about 340 px): their header tools have no room.
    evaluate(shell, QStringLiteral("docking.joinPanel('effectControls', docking.groupOfPanel('library').group)"));
    evaluate(shell, QStringLiteral("docking.joinPanel('mediaBrowser', docking.groupOfPanel('library').group)"));
    spin(100);
    REQUIRE(shownPanel(shell, "library") == QStringLiteral("mediaBrowser"));
    QQuickItem* browser = nullptr;
    REQUIRE(waitFor([&] {
        browser = findItem(shell, QStringLiteral("mediaBrowserPanel"));
        return browser != nullptr && browser->isVisible();
    }));
    spin(50);
    const QStringList browserHidden = hiddenNames(browser);
    INFO("Media Browser hidden tools: " << browserHidden.join(QLatin1Char(' ')).toStdString());
    REQUIRE(browserHidden.contains(QStringLiteral("mediaBrowserMetadataToggle")));
    QQuickItem* metadataToggle = findUnder(browser, QStringLiteral("mediaBrowserMetadataToggle"));
    REQUIRE(metadataToggle != nullptr);
    CHECK_FALSE(browser->property("showMetadata").toBool());

    // A checkable tool that acts in onToggled (the Metadata toggle): its row toggles it as a click would, both ways.
    QObject* menu = openMoreTools(browser);
    QObject* row = menu->findChild<QObject*>(QStringLiteral("panelToolsMenu_mediaBrowserMetadataToggle"));
    REQUIRE(row != nullptr);
    CHECK(row->property("checkable").toBool());
    CHECK_FALSE(row->property("checked").toBool());
    chooseInMenu(menu, "panelToolsMenu_mediaBrowserMetadataToggle");
    REQUIRE(waitFor([&] { return browser->property("showMetadata").toBool(); }));
    CHECK(metadataToggle->property("checked").toBool());
    menu = openMoreTools(browser);
    row = menu->findChild<QObject*>(QStringLiteral("panelToolsMenu_mediaBrowserMetadataToggle"));
    REQUIRE(row != nullptr);
    CHECK(row->property("checked").toBool());
    chooseInMenu(menu, "panelToolsMenu_mediaBrowserMetadataToggle");
    REQUIRE(waitFor([&] { return !browser->property("showMetadata").toBool(); }));
    CHECK_FALSE(metadataToggle->property("checked").toBool());

    // Effect Controls: its rows use the tools' short names - a checked "Keyframe lane" (the lane shows), not "Hide the
    // keyframe lane" next to a check (Panel.menuLabel: the tool's menuText, else its text, else its tooltip).
    evaluate(shell, QStringLiteral("window.revealPanel('effectControls')"));
    spin(100);
    REQUIRE(shownPanel(shell, "library") == QStringLiteral("effectControls"));
    QQuickItem* inspector = shell.item("effectControls");
    REQUIRE(inspector != nullptr);
    REQUIRE(evaluate(shell, QStringLiteral("Preferences.keyframeLane")).toBool());
    CHECK(inPanel(inspector, QStringLiteral("trailing[1].objectName")).toString() == QStringLiteral("keyframeLaneToggle"));
    CHECK(inPanel(inspector, QStringLiteral("trailing[1].tooltip")).toString() == QStringLiteral("Hide the keyframe lane"));
    CHECK(inPanel(inspector, QStringLiteral("menuLabel(trailing[1])")).toString() == QStringLiteral("Keyframe lane"));
    CHECK(inPanel(inspector, QStringLiteral("menuLabel(trailing[0])")).toString() == QStringLiteral("Explain this frame"));
    // A tool without a menuText: its tooltip (the Library's List view).
    QQuickItem* libraryPanel = shell.item("libraryPanel");
    REQUIRE(libraryPanel != nullptr);
    CHECK(inPanel(libraryPanel, QStringLiteral("menuLabel(trailing[1])")).toString() == QStringLiteral("List view"));
    // In a short header the row shows it so.
    const QStringList inspectorHidden = hiddenNames(inspector);
    if (inspectorHidden.contains(QStringLiteral("keyframeLaneToggle"))) {
        menu = openMoreTools(inspector);
        QObject* laneRow = menu->findChild<QObject*>(QStringLiteral("panelToolsMenu_keyframeLaneToggle"));
        REQUIRE(laneRow != nullptr);
        CHECK(laneRow->property("text").toString() == QStringLiteral("Keyframe lane"));
        CHECK(laneRow->property("checked").toBool());
        REQUIRE(QMetaObject::invokeMethod(menu, "close"));
        REQUIRE(waitFor([&] { return !menu->property("visible").toBool(); }));
    }

    // The Timeline's group crowded with tabs: its edit-tool palette (an Item, not a button: it just hides) has no
    // room, and the buttons inside it leave the Tab chain with it (Tab never lands on a button nobody can see).
    const char* const crowd[] = {"effects", "captions", "audio",    "essential", "graphics", "highlights",
                                 "markers", "metadata", "production", "review",  "shorts",   "takes"};
    shell.resize(1100, 700);
    spin(50);
    for (const char* key : crowd)
        evaluate(shell, QStringLiteral("docking.joinPanel('%1', docking.groupOfPanel('timeline').group)").arg(QLatin1String(key)));
    evaluate(shell, QStringLiteral("window.revealPanel('timeline')"));
    spin(150);
    shell.grab();
    spin(50);
    REQUIRE(shownPanel(shell, "timeline") == QStringLiteral("timeline"));
    QQuickItem* timeline = shell.item("timelinePanel");
    REQUIRE(timeline != nullptr);
    INFO("timeline width " << timeline->width() << ", its group's panels "
                           << evaluate(shell, QStringLiteral("docking.groupOfPanel('timeline').panels.length")).toInt()
                           << ", tabs width " << evaluate(shell, QStringLiteral("docking.slotOfPanel('timeline').tabsWidth")).toDouble()
                           << ", room " << inPanel(timeline, QStringLiteral("toolFit.leading.length")).toInt());
    REQUIRE_FALSE(inPanel(timeline, QStringLiteral("toolFit.leading[0] === true")).toBool());
    QQuickItem* palette = findUnder(timeline, QStringLiteral("timelineToolPalette"));
    REQUIRE(palette != nullptr);
    QList<QQuickItem*> paletteButtons;
    QList<QQuickItem*> stack{palette};
    while (!stack.isEmpty()) {
        QQuickItem* item = stack.takeLast();
        if (QString::fromLatin1(item->metaObject()->className()).startsWith(QLatin1String("ToolGroupButton")))
            paletteButtons << item;
        stack << item->childItems();
    }
    REQUIRE_FALSE(paletteButtons.isEmpty());
    const auto tabChain = [&] {
        QList<QQuickItem*> chain;
        QQuickItem* item = shell.window->contentItem()->nextItemInFocusChain(true);
        for (int i = 0; i < 5000 && item != nullptr && !chain.contains(item); ++i) {
            chain << item;
            item = item->nextItemInFocusChain(true);
        }
        return chain;
    };
    const QList<QQuickItem*> crowded = tabChain();
    REQUIRE(crowded.size() > 1);
    for (QQuickItem* button : paletteButtons) {
        CHECK_FALSE(button->isEnabled());
        CHECK_FALSE(crowded.contains(button));
    }

    // Room again (the panels leave): the palette shows and its buttons are in the Tab chain again.
    for (const char* key : crowd)
        evaluate(shell, QStringLiteral("docking.closePanel('%1')").arg(QLatin1String(key)));
    spin(150);
    shell.grab();
    spin(50);
    REQUIRE(inPanel(timeline, QStringLiteral("toolFit.leading[0] === true")).toBool());
    for (QQuickItem* button : paletteButtons)
        CHECK(button->isEnabled());
    CHECK(tabChain().contains(paletteButtons.front()));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
