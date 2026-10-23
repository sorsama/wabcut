// UX rework (stream S): the editor shell's C++ services, without the window.
//   * Single instance (SingleInstance, AppShell): a later launch hands its project to the running Wabcut over a local
//     socket; nobody listening means it starts on its own; a running Wabcut that answers too late (the launch gave up)
//     or has no window yet never opens the file; a second Wabcut leaves the name to the running one; only a launch
//     naming a project hands over, never with --new-instance; AppShell passes the files on to QML (openRequested).
//   * The main window's place between runs (WindowPlacement): where a saved window goes on the screens there are now,
//     the keeper that follows the windowed geometry and the maximized state, the QML initial properties main() uses.
//   * File > Save a Copy and File > Revert (ProjectFile.saveCopy / revert).

#include "ShellFixture.h"

#include "AppShell.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "SingleInstance.h"
#include "WindowPlacement.h"

#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QScreen>
#include <QThread>
#include <QUrl>
#include <QUuid>
#include <QWindow>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
namespace fs = std::filesystem;

namespace {

QString uniqueName(const char* prefix) {
    return QString::fromLatin1(prefix) + QUuid::createUuid().toString(QUuid::Id128).left(12);
}

fs::path workDir(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "app" / "shell-services" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

QUrl url(const fs::path& p) {
    return QUrl::fromLocalFile(QString::fromStdU16String(p.u16string()));
}

ProjectState withSequences(int n) {
    ProjectState state;
    for (int i = 0; i < n; ++i) {
        auto created = edit::addSequence(state, edit::SequenceSettings{});
        REQUIRE(created);
        state = std::move(created->state);
    }
    return state;
}

int sequencesIn(const fs::path& file) {
    auto loaded = io::loadProject(file);
    return loaded ? static_cast<int>(loaded->sequences.size()) : -1;
}

} // namespace

// ---- Single instance ----------------------------------------------------------------------------------------------

TEST_CASE("Single instance: a later launch hands its project to the running Wabcut", "[app][shell][instance]") {
    const QString name = uniqueName("wabcut-test-");
    SingleInstance running(name);
    REQUIRE(running.listen());
    CHECK(running.isListening());
    QList<QList<QUrl>> received;
    QObject::connect(&running, &SingleInstance::filesRequested, [&](const QList<QUrl>& files) { received.append(files); });

    // A path with a space and a non-ASCII letter travels as an encoded file URL and arrives as the same URL.
    const QUrl project = QUrl::fromLocalFile(QDir::temp().filePath(QStringLiteral(u"My film café.wabcut")));
    std::atomic<int> handedOver{-1};
    // The later launch blocks while it waits for the answer, as main() does: here it runs on a thread of its own so the
    // running Wabcut (this thread's event loop) can answer.
    std::unique_ptr<QThread> launch(QThread::create([&] {
        const SingleInstance later(name);
        handedOver = later.handOver({project}) ? 1 : 0;
    }));
    launch->start();
    REQUIRE(waitFor([&] { return launch->isFinished(); }));
    CHECK(handedOver == 1);
    // The files open on the launch's confirmation ("go"), which may be read just after it ended.
    REQUIRE(waitFor([&] { return !received.isEmpty(); }));
    REQUIRE(received.size() == 1);
    REQUIRE(received.front().size() == 1);
    CHECK(received.front().front() == project);

    // Once it stopped listening (the software-renderer restart), a later launch starts on its own, without waiting.
    running.close();
    CHECK_FALSE(running.isListening());
    const SingleInstance later(name);
    CHECK_FALSE(later.handOver({project}, 2000));
    CHECK(received.size() == 1);
}

TEST_CASE("Single instance: a launch the running Wabcut answers too late or not ready starts on its own",
          "[app][shell][instance]") {
    const QString name = uniqueName("wabcut-test-");
    SingleInstance running(name);
    REQUIRE(running.listen());
    QList<QList<QUrl>> received;
    QObject::connect(&running, &SingleInstance::filesRequested, [&](const QList<QUrl>& files) { received.append(files); });
    const QUrl project = QUrl::fromLocalFile(QDir::temp().filePath(QStringLiteral("late.wabcut")));
    // A later launch on a thread of its own (it blocks while it waits, as main() does); its result: 1 handed over, 0 not.
    const auto launchOnThread = [&name, &project](int timeoutMs, std::atomic<int>* result) {
        return std::unique_ptr<QThread>(QThread::create([&name, &project, timeoutMs, result] {
            const SingleInstance later(name);
            *result = later.handOver({project}, timeoutMs) ? 1 : 0;
        }));
    };

    // The running Wabcut is busy (its event loop does not run: a slow start, a long synchronous job) until the launch
    // gave up and started a Wabcut of its own. When it gets to the request afterwards, it must not open it as well.
    std::atomic<int> late{-1};
    std::unique_ptr<QThread> launch = launchOnThread(400, &late);
    launch->start();
    REQUIRE(launch->wait(15000)); // blocks this thread without running its event loop
    CHECK(late == 0);
    spin(800); // now the running Wabcut reads whatever arrived
    CHECK(received.isEmpty());

    // No window to open it in yet (main(): setReady): "no", and the launch starts on its own at once.
    running.setReady([] { return false; });
    std::atomic<int> notReady{-1};
    launch = launchOnThread(3000, &notReady);
    launch->start();
    REQUIRE(waitFor([&] { return launch->isFinished(); }));
    CHECK(notReady == 0);
    spin(100);
    CHECK(received.isEmpty());

    // Ready: taken.
    running.setReady([] { return true; });
    std::atomic<int> taken{-1};
    launch = launchOnThread(3000, &taken);
    launch->start();
    REQUIRE(waitFor([&] { return launch->isFinished(); }));
    CHECK(taken == 1);
    REQUIRE(waitFor([&] { return !received.isEmpty(); }));
    CHECK(received.size() == 1);
}

TEST_CASE("Single instance: a second Wabcut leaves the name to the one running", "[app][shell][instance]") {
    const QString name = uniqueName("wabcut-test-");
    SingleInstance first(name);
    REQUIRE(first.listen());
    QList<QList<QUrl>> toFirst;
    QObject::connect(&first, &SingleInstance::filesRequested, [&](const QList<QUrl>& files) { toFirst.append(files); });

    // A plain launch while it runs: it does not take the name (it would leave the first one unreachable).
    {
        SingleInstance second(name);
        QList<QList<QUrl>> toSecond;
        QObject::connect(&second, &SingleInstance::filesRequested,
                         [&](const QList<QUrl>& files) { toSecond.append(files); });
        CHECK_FALSE(second.listen());
        CHECK_FALSE(second.isListening());
        // A project double-clicked now goes to the first one.
        const QUrl project = QUrl::fromLocalFile(QDir::temp().filePath(QStringLiteral("first.wabcut")));
        std::atomic<int> handedOver{-1};
        std::unique_ptr<QThread> launch(QThread::create([&] {
            const SingleInstance later(name);
            handedOver = later.handOver({project}) ? 1 : 0;
        }));
        launch->start();
        REQUIRE(waitFor([&] { return launch->isFinished(); }));
        CHECK(handedOver == 1);
        REQUIRE(waitFor([&] { return !toFirst.isEmpty(); }));
        CHECK(toSecond.isEmpty());
    }

    // Still the first one's once the second has gone; when the first stops, a later Wabcut takes the name.
    {
        const QUrl project = QUrl::fromLocalFile(QDir::temp().filePath(QStringLiteral("again.wabcut")));
        std::atomic<int> handedOver{-1};
        std::unique_ptr<QThread> launch(QThread::create([&] {
            const SingleInstance later(name);
            handedOver = later.handOver({project}) ? 1 : 0;
        }));
        launch->start();
        REQUIRE(waitFor([&] { return launch->isFinished(); }));
        CHECK(handedOver == 1);
        CHECK(waitFor([&] { return toFirst.size() == 2; }));
    }
    first.close();
    CHECK_FALSE(first.isListening());
    spin(100); // the first one's last connections go
    SingleInstance third(name);
    CHECK(third.listen());
}

TEST_CASE("Single instance: with nobody listening a launch starts on its own", "[app][shell][instance]") {
    const SingleInstance alone(uniqueName("wabcut-test-"));
    CHECK_FALSE(alone.handOver({QUrl::fromLocalFile(QDir::temp().filePath(QStringLiteral("a.wabcut")))}, 2000));
    CHECK_FALSE(alone.handOver({}, 2000)); // nothing to hand over
    CHECK_FALSE(alone.isListening());
}

TEST_CASE("Single instance: only a launch naming a project hands over and the new-instance switch never does",
          "[app][shell][instance]") {
    const fs::path dir = workDir("launch");
    const fs::path project = dir / "Film.wabcut";
    std::ofstream(project) << "{}";
    const QString file = QString::fromStdU16String(project.u16string());
    const QUrl expected = QUrl::fromLocalFile(QFileInfo(file).absoluteFilePath());
    const QString exe = QStringLiteral("wabcut.exe");
    CHECK(SingleInstance::projectToHandOver({exe, file}) == expected);
    CHECK(SingleInstance::projectToHandOver({exe, QStringLiteral("--rhi"), QStringLiteral("d3d11"), file}) == expected);
    CHECK(SingleInstance::projectToHandOver({exe}).isEmpty());
    CHECK(SingleInstance::projectToHandOver({exe, QStringLiteral("notes.txt")}).isEmpty());
    CHECK(SingleInstance::projectToHandOver({exe, SingleInstance::newInstanceSwitch(), file}).isEmpty());
    CHECK(SingleInstance::projectToHandOver({exe, file, QStringLiteral("--new-instance")}).isEmpty());
    CHECK(SingleInstance::separateInstance({exe, QStringLiteral("--new-instance")}));
    CHECK_FALSE(SingleInstance::separateInstance({exe, file}));

    // One name per user and settings location: the same every time, never empty.
    const QString name = SingleInstance::defaultServerName();
    CHECK(name.startsWith(QStringLiteral("wabcut-")));
    CHECK(name.size() == 7 + 16);
    CHECK(name == SingleInstance::defaultServerName());
}

TEST_CASE("AppShell passes handed-over files on to QML and only local ones", "[app][shell][instance][qml]") {
    AppShell shell;
    CHECK_FALSE(shell.singleInstance()); // the tests never take other launches' files
    QList<QList<QUrl>> asked;
    QObject::connect(&shell, &AppShell::openRequested, [&](const QList<QUrl>& files) { asked.append(files); });
    const QUrl local = QUrl::fromLocalFile(QDir::temp().filePath(QStringLiteral("b.wabcut")));
    shell.requestOpen({QUrl(QStringLiteral("https://example.com/a.wabcut")), local});
    REQUIRE(asked.size() == 1);
    CHECK(asked.front() == QList<QUrl>{local});
    shell.requestOpen({QUrl(QStringLiteral("https://example.com/a.wabcut"))});
    shell.requestOpen({});
    CHECK(asked.size() == 1); // nothing local: nothing asked

    // The QML singleton: a Connections in QML hears openRequested with the list of URLs.
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import Wabcut
        Item {
            property int count: 0
            property string first: ""
            Connections {
                target: AppShell
                function onOpenRequested(files) {
                    count = files.length
                    first = files[0].toString()
                }
            }
        }
    )",
                      QUrl());
    std::unique_ptr<QObject> item(component.create());
    INFO(component.errorString().toStdString());
    REQUIRE(item);
    auto* singleton = engine.singletonInstance<AppShell*>("Wabcut", "AppShell");
    REQUIRE(singleton);
    singleton->requestOpen({local});
    CHECK(item->property("count").toInt() == 1);
    CHECK(item->property("first").toString() == local.toString());
}

// ---- The main window's place --------------------------------------------------------------------------------------

TEST_CASE("Window placement: a saved window comes back where it was or centred when its screen is gone",
          "[app][shell][window]") {
    const QRect primary(0, 0, 1920, 1040);      // a 1080p screen above its taskbar
    const QRect left(-2560, -200, 2560, 1400);  // a larger screen on its left, higher up
    const QList<QRect> screens{primary, left};
    // On a screen: where it was.
    CHECK(placeOnScreens(QRect(100, 80, 1600, 900), screens) == QRect(100, 80, 1600, 900));
    CHECK(placeOnScreens(QRect(-2000, 0, 1600, 900), screens) == QRect(-2000, 0, 1600, 900));
    // Partly off its screen with the title bar reachable: where it was.
    CHECK(placeOnScreens(QRect(1700, 500, 1600, 900), screens) == QRect(1700, 500, 1600, 900));
    // A few pixels above the top edge: moved down to it.
    CHECK(placeOnScreens(QRect(100, -5, 1600, 900), screens) == QRect(100, 0, 1600, 900));
    // Its screen is gone (unplugged): centred on the primary screen.
    CHECK(placeOnScreens(QRect(4000, 100, 1600, 900), screens) == QRect(160, 70, 1600, 900));
    // Only a sliver of its title bar (40 px) would show: centred too.
    CHECK(placeOnScreens(QRect(1880, 100, 1600, 900), screens) == QRect(160, 70, 1600, 900));
    // Larger than its screen (it was on a 4K monitor): cut to the screen and moved inside it.
    CHECK(placeOnScreens(QRect(50, 20, 3000, 2000), screens) == QRect(0, 0, 1920, 1040));
    // Never smaller than the window's minimum, even on a small screen.
    CHECK(placeOnScreens(QRect(4000, 0, 800, 500), {QRect(0, 0, 640, 480)}, QSize(900, 600)) ==
          QRect(-130, -60, 900, 600));
    // Nothing to place, or nowhere.
    CHECK_FALSE(placeOnScreens(QRect(), screens).has_value());
    CHECK_FALSE(placeOnScreens(QRect(0, 0, 800, 600), {}).has_value());
}

TEST_CASE("Window placement: the keeper keeps the windowed geometry while maximized or minimized", "[app][shell][window]") {
    const QString group = uniqueName("test-window-");
    CHECK_FALSE(loadWindowPlacement(group).has_value());
    QWindow window; // never shown
    window.setGeometry(120, 90, 1280, 720);
    {
        WindowPlacementKeeper keeper(&window, group);
        keeper.save();
        CHECK(loadWindowPlacement(group) == WindowPlacement{QRect(120, 90, 1280, 720), false});
        // Maximized: the windowed geometry stays what it was.
        window.setWindowStates(Qt::WindowMaximized);
        window.setGeometry(0, 0, 1920, 1040);
        keeper.save();
        CHECK(loadWindowPlacement(group) == WindowPlacement{QRect(120, 90, 1280, 720), true});
        // Minimized from maximized: still maximized when it comes back.
        window.setWindowStates(Qt::WindowMinimized | Qt::WindowMaximized);
        keeper.save();
        CHECK(loadWindowPlacement(group) == WindowPlacement{QRect(120, 90, 1280, 720), true});
        // Back to a window, moved and resized.
        window.setWindowStates(Qt::WindowNoState);
        window.setGeometry(200, 150, 1000, 700);
        keeper.save();
        CHECK(loadWindowPlacement(group) == WindowPlacement{QRect(200, 150, 1000, 700), false});
        window.setGeometry(210, 150, 1000, 700); // the last move: written when the keeper goes
    }
    CHECK(loadWindowPlacement(group) == WindowPlacement{QRect(210, 150, 1000, 700), false});
    // A new keeper starts from what was saved (a window that opens maximized keeps its windowed geometry).
    QWindow maximized;
    maximized.setWindowStates(Qt::WindowMaximized);
    maximized.setGeometry(0, 0, 1920, 1040);
    const WindowPlacementKeeper next(&maximized, group);
    CHECK(next.placement() == WindowPlacement{QRect(210, 150, 1000, 700), true});
}

TEST_CASE("Window placement: the restored window properties fit the screens there are now", "[app][shell][window]") {
    QScreen* screen = QGuiApplication::primaryScreen();
    REQUIRE(screen);
    const QRect avail = screen->availableGeometry();
    const QString group = uniqueName("test-window-");
    CHECK(restoredWindowProperties(group).isEmpty()); // nothing saved: the window's own size, the system's place

    const QRect inside(avail.x() + 20, avail.y() + 20, std::min(640, avail.width() - 40), std::min(480, avail.height() - 40));
    saveWindowPlacement({inside, true}, group);
    QVariantMap properties = restoredWindowProperties(group);
    CHECK(properties.value(QStringLiteral("x")).toInt() == inside.x());
    CHECK(properties.value(QStringLiteral("y")).toInt() == inside.y());
    CHECK(properties.value(QStringLiteral("width")).toInt() == inside.width());
    CHECK(properties.value(QStringLiteral("height")).toInt() == inside.height());
    CHECK(properties.value(QStringLiteral("visibility")).value<QWindow::Visibility>() == QWindow::Maximized);

    // Saved on a screen that is not there any more: centred on the primary screen, not maximized.
    saveWindowPlacement({QRect(avail.x() - 100000, avail.y(), inside.width(), inside.height()), false}, group);
    properties = restoredWindowProperties(group);
    CHECK(properties.value(QStringLiteral("x")).toInt() == avail.x() + (avail.width() - inside.width()) / 2);
    CHECK(properties.value(QStringLiteral("y")).toInt() == avail.y() + (avail.height() - inside.height()) / 2);
    CHECK_FALSE(properties.contains(QStringLiteral("visibility")));
}

// ---- File > Save a Copy, File > Revert ----------------------------------------------------------------------------

TEST_CASE("File Save a Copy writes another file and keeps the project as it is", "[app][shell][file]") {
    const fs::path dir = workDir("save-copy");
    const fs::path project = dir / "film.wabcut";
    REQUIRE(io::saveProject(withSequences(1), project, io::SaveOptions{0}));
    ProjectController controller;
    ProjectLifecycle lifecycle(&controller);
    REQUIRE(lifecycle.open(url(project)));
    controller.newSequence();
    REQUIRE(lifecycle.dirty());
    const QString path = lifecycle.filePath();

    const fs::path copy = dir / "copy of film.wabcut";
    REQUIRE(lifecycle.saveCopy(url(copy)));
    CHECK(lifecycle.lastError().isEmpty());
    CHECK(sequencesIn(copy) == 2);    // the project as it is now
    CHECK(sequencesIn(project) == 1); // its own file untouched
    CHECK(lifecycle.dirty());         // the changes are still unsaved
    CHECK(lifecycle.filePath() == path);
    CHECK_FALSE(fs::exists(io::backupPath(copy, 1)));

    // ".wabcut" is added; writing over an earlier copy is fine.
    REQUIRE(lifecycle.saveCopy(url(dir / "second")));
    CHECK(sequencesIn(dir / "second.wabcut") == 2);
    REQUIRE(lifecycle.saveCopy(url(copy)));

    // Never over the project's own file (that is Save).
    CHECK_FALSE(lifecycle.saveCopy(url(project)));
    CHECK_FALSE(lifecycle.lastError().isEmpty());
    CHECK(sequencesIn(project) == 1);

    // An untitled project can be copied too; it stays untitled.
    lifecycle.newProject();
    REQUIRE(lifecycle.saveCopy(url(dir / "untitled copy.wabcut")));
    CHECK(sequencesIn(dir / "untitled copy.wabcut") == 1);
    CHECK(lifecycle.untitled());
}

TEST_CASE("File Revert reads the saved project again and drops its autosaves", "[app][shell][file]") {
    const fs::path dir = workDir("revert");
    const fs::path project = dir / "film.wabcut";
    REQUIRE(io::saveProject(withSequences(2), project, io::SaveOptions{0}));
    ProjectController controller;
    ProjectLifecycle lifecycle(&controller);

    // Nothing saved to go back to: untitled.
    lifecycle.newProject();
    CHECK_FALSE(lifecycle.revert());
    CHECK_FALSE(lifecycle.lastError().isEmpty());

    REQUIRE(lifecycle.open(url(project)));
    controller.newSequence();
    controller.newSequence();
    REQUIRE(controller.sequenceNames().size() == 4);
    REQUIRE(lifecycle.autosaveNow());
    REQUIRE(fs::exists(io::autosavePath(project)));
    controller.setCurrentSequenceIndex(1);
    const QString path = lifecycle.filePath();

    REQUIRE(lifecycle.revert());
    CHECK(lifecycle.lastError().isEmpty());
    CHECK_FALSE(lifecycle.dirty());
    CHECK(controller.sequenceNames().size() == 2);
    CHECK(controller.currentSequenceIndex() == 1); // the sequence shown is still there: it stays
    CHECK_FALSE(controller.canUndo());
    CHECK_FALSE(fs::exists(io::autosavePath(project))); // no recovery offer for the changes thrown away
    CHECK_FALSE(lifecycle.autosaveNow());               // and nothing new to autosave
    CHECK(lifecycle.filePath() == path);

    // A file that cannot be read changes nothing: the unsaved work and its autosave stay.
    controller.newSequence();
    REQUIRE(lifecycle.autosaveNow());
    std::ofstream(project, std::ios::trunc) << "not a project";
    CHECK_FALSE(lifecycle.revert());
    CHECK_FALSE(lifecycle.lastError().isEmpty());
    CHECK(lifecycle.dirty());
    CHECK(controller.sequenceNames().size() == 3);
    CHECK(fs::exists(io::autosavePath(project)));
}

TEST_CASE("File Open of the project that is already open keeps it as it is with its undo history", "[app][shell][file]") {
    const fs::path dir = workDir("reopen");
    const fs::path project = dir / "film.wabcut";
    REQUIRE(io::saveProject(withSequences(2), project, io::SaveOptions{0}));
    ProjectController controller;
    ProjectLifecycle lifecycle(&controller);

    REQUIRE(lifecycle.open(url(project)));
    controller.newSequence();
    REQUIRE(lifecycle.save());
    controller.newSequence(); // an unsaved change on top of a saved one
    REQUIRE(controller.sequenceNames().size() == 4);
    REQUIRE(controller.canUndo());
    controller.setCurrentSequenceIndex(2);

    // File > Open (or an import or a drop) of its own file: nothing is read again.
    CHECK(lifecycle.open(url(project)));
    CHECK(lifecycle.lastError().isEmpty());
    CHECK(controller.sequenceNames().size() == 4);
    CHECK(controller.currentSequenceIndex() == 2);
    CHECK(controller.canUndo());
    CHECK(lifecycle.dirty());

    // File > Revert still reads the saved file again.
    REQUIRE(lifecycle.revert());
    CHECK(controller.sequenceNames().size() == 3);
}
