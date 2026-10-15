// 0.1.3: the Program Monitor in a real (shown) window keeps presenting while it plays and after a seek, and panel
// docking: panels in groups with tabs; a panel or a group dropped on another group's tabs or middle joins it, near its
// edge docks beside it, at the work area's edge runs along that side, and anywhere else (or with Ctrl) gets a window of
// its own; dragging a grip or a tab shows where it would go; tabs show and close panels, the close button in a header
// too, and a closed panel opens again; each workspace (made from a preset, none built in) keeps its own layout, kept
// between sessions (the first preview's workspaces too); File > Reset Panel Layout gives the preset's layout back; a
// group popped out into a window of its own gets the keys there and docks back when that window closes or when its grip
// is dragged onto the workbench.

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
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCloseEvent>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
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
        // Opened again (the Window menu, Effect Controls' focus key): beside the monitors, on their right.
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
