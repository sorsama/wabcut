// Single-instance file opening, version history in the File menu, Report a Bug, Snap Layouts and the system menu, and
// transcribing a recording that switches language (offscreen; no window is shown):
//   * "Open with" on a recording or a timeline file: every file argument reaches the running Wabcut (005), which imports
//     media into the current bin (the editor) or starts the New project page with it (Home); a lone project opens as before;
//   * File > Save Version... / Version History..., the interval setting, the Recover dashboard's link (006);
//   * Help > Report a Bug... and the system information it shows, copies and puts into a GitHub issue address (016);
//   * the custom frame's right-click system menu and Windows 11 Snap Layouts on the maximize pill (017);
//   * the Transcript tab's "Mixed languages" switch, a transcript that names each paragraph's language (111).
// Screenshots land in <build>/test-output/ui/ (dark and light themes).

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "AppShell.h"
#include "CommandPalette.h"
#include "CrashNotice.h"
#include "HistoryBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "SingleInstance.h"
#include "TranscriptBridge.h"
#include "platform/WindowChromeFilter.h"

#include <wabcut/ai/SpokenLanguages.h>
#include <wabcut/ai/Transcript.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
namespace fs = std::filesystem;

namespace {

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

QQuickItem* visibleNamed(Shell& shell, const QString& name) {
    return findItem(shell.window->contentItem(), [&](QQuickItem* i) { return i->objectName() == name && i->isVisible(); });
}

QQuickItem* anyNamed(Shell& shell, const QString& name) {
    return findItem(shell.window->contentItem(), [&](QQuickItem* i) { return i->objectName() == name; });
}

void saveShot(const QImage& image, const QQuickItem* item, const QString& name, int margin = 0) {
    REQUIRE_FALSE(image.isNull());
    const double dpr = image.devicePixelRatio();
    QRectF area = item->mapRectToScene(QRectF(0, 0, item->width(), item->height())).adjusted(-margin, -margin, margin, margin);
    area = area.intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    REQUIRE(image.copy(QRectF(area.x() * dpr, area.y() * dpr, area.width() * dpr, area.height() * dpr).toRect())
                .save(QDir(outputDir()).filePath(name)));
}

QVariant evaluate(const Shell& shell, const QString& code) {
    QQmlExpression expression(qmlContext(shell.window.get()), shell.window.get(), code);
    const QVariant result = expression.evaluate();
    INFO(code.toStdString() << ": " << expression.error().toString().toStdString());
    REQUIRE_FALSE(expression.hasError());
    return result;
}

bool opened(QObject* popup) { return popup && popup->property("opened").toBool(); }
bool shown(QObject* popup) { return popup && popup->property("visible").toBool(); }

/// Presses `button` (a Copy button) until the clipboard holds what it copies: another process may hold the system
/// clipboard for a moment (Windows' OpenClipboard fails then), so the press is retried; the text is returned.
QString pressCopy(QObject* button, const std::function<bool(const QString&)>& expected) {
    QString text;
    for (int attempt = 0; attempt < 20; ++attempt) {
        QGuiApplication::clipboard()->setText(QStringLiteral("before"));
        REQUIRE(QMetaObject::invokeMethod(button, "clicked"));
        spin(100);
        text = QGuiApplication::clipboard()->text();
        if (expected(text))
            break;
    }
    return text;
}

/// The captured messages without the system's complaints about a clipboard another process holds.
QStringList withoutClipboardNoise(QStringList messages) {
    messages.erase(std::remove_if(messages.begin(), messages.end(),
                                  [](const QString& m) { return m.contains(QStringLiteral("OleSetClipboard")); }),
                   messages.end());
    return messages;
}

fs::path workDir(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "app" / "d4" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

QString q(const fs::path& p) { return QString::fromStdU16String(p.u16string()); }

QString touch(const fs::path& dir, const char* name) {
    const fs::path file = dir / name;
    std::ofstream(file) << "x";
    return q(file);
}

QUrl urlOf(const QString& file) { return QUrl::fromLocalFile(QFileInfo(file).absoluteFilePath()); }

/// The shared test media (build/test-output/media), which the test FAILs on when it is missing.
QString sampleVideo() {
    const QString video = QDir(outputDir()).filePath(QStringLiteral("../media/h264_360p120.mp4"));
    if (!QFile::exists(video))
        FAIL("the shared test media is missing (build\\win-msvc\\test-output\\media): " << video.toStdString());
    return video;
}

/// The editor at 1600x960 (the menu titles show), motion off.
struct EditorShell {
    Shell shell;
    QQuickWindow* w = nullptr;
    QObject* bar = nullptr;
    ProjectController* controller = nullptr;
    ProjectLifecycle* project = nullptr;

    explicit EditorShell(bool dark = true) {
        REQUIRE(shell.window != nullptr);
        INFO(join(shell.errors));
        REQUIRE(shell.errors.isEmpty());
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        shell.setAppearance(dark, "ultraviolet");
        w = shell.window.get();
        bar = shell.object("appMenuBar");
        REQUIRE(bar != nullptr);
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        project = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
        REQUIRE(controller != nullptr);
        REQUIRE(project != nullptr);
        spin(50);
    }
    /// Opens a menu of the bar (index: File 0, Edit 1, Clip 2, Sequence 3, Markers 4, Graphics 5, View 6, Window 7, Help 8).
    QObject* open(int index, const char* menuName) const {
        REQUIRE(QMetaObject::invokeMethod(bar, "open", Q_ARG(int, index), Q_ARG(bool, false)));
        QObject* menu = shell.object(menuName);
        REQUIRE(menu != nullptr);
        REQUIRE(waitFor([&] { return opened(menu); }));
        return menu;
    }
    void dismiss(QObject* menu) const {
        REQUIRE(QMetaObject::invokeMethod(menu, "close"));
        REQUIRE(waitFor([&] { return !shown(menu); }));
        spin(30);
    }
    QString toastText() const {
        QObject* toast = shell.object("toast");
        REQUIRE(toast != nullptr);
        return toast->property("text").toString();
    }
};

} // namespace

// ---- 005: files opened from File Explorer ---------------------------------------------------------------------------------

TEST_CASE("Launch files: every existing file the app was started with and absolute and once and in order", "[app][shell][instance][d4]") {
    const fs::path dir = workDir("launch-files");
    const QString film = touch(dir, "Film.wabcut");
    const QString clip = touch(dir, "Take 1.mp4");
    const QString edl = touch(dir, "cut.edl");
    const QString exe = QStringLiteral("wabcut.exe");
    using Urls = QList<QUrl>;

    CHECK(ProjectLifecycle::launchFilesIn({exe, clip}) == Urls{urlOf(clip)});
    // Options and the value of --rhi are skipped; a project and media come in argument order.
    CHECK(ProjectLifecycle::launchFilesIn({exe, QStringLiteral("--rhi"), QStringLiteral("d3d11"), clip, QStringLiteral("--new-instance"),
                                           edl, film}) == Urls{urlOf(clip), urlOf(edl), urlOf(film)});
    CHECK(ProjectLifecycle::launchFilesIn({exe, QStringLiteral("--rhi=d3d11"), clip}) == Urls{urlOf(clip)});
    CHECK(ProjectLifecycle::launchFilesIn({exe, clip, clip}) == Urls{urlOf(clip)}); // named twice: once
    // Missing files, folders, empty arguments and an option's value are no files.
    CHECK(ProjectLifecycle::launchFilesIn({exe, QStringLiteral("missing.mp4"), q(dir), QString()}).isEmpty());
    CHECK(ProjectLifecycle::launchFilesIn({exe, QStringLiteral("--rhi"), clip}).isEmpty());
    CHECK(ProjectLifecycle::launchFilesIn({exe}).isEmpty());
    CHECK(ProjectLifecycle::launchFilesIn({}).isEmpty());
    // A project launch still finds its project (launchProject) beside the media.
    CHECK(ProjectLifecycle::launchProjectIn({exe, clip, film}) == urlOf(film));

    // What a launch hands to a running Wabcut: all of them, unless a separate Wabcut was asked for.
    CHECK(SingleInstance::filesToHandOver({exe, clip, film}) == Urls{urlOf(clip), urlOf(film)});
    CHECK(SingleInstance::filesToHandOver({exe, clip, QStringLiteral("--new-instance")}).isEmpty());
    CHECK(SingleInstance::filesToHandOver({exe, QStringLiteral("notes-that-are-not-there.txt")}).isEmpty());
}

TEST_CASE("Single instance: a later launch hands over several files at once", "[app][shell][instance][d4]") {
    const fs::path dir = workDir("hand-over");
    const QString a = touch(dir, "OBS 2026-11-05.mkv");
    const QString b = touch(dir, "Intérview café.mov");
    const QString c = touch(dir, "Film.wabcut");
    SingleInstance running(QStringLiteral("wabcut-test-d4-") + QString::number(QDateTime::currentMSecsSinceEpoch()));
    REQUIRE(running.listen());
    QList<QList<QUrl>> received;
    QObject::connect(&running, &SingleInstance::filesRequested, [&](const QList<QUrl>& files) { received.append(files); });

    const QList<QUrl> files = SingleInstance::filesToHandOver({QStringLiteral("wabcut.exe"), a, b, c});
    REQUIRE(files.size() == 3);
    std::atomic<int> handedOver{-1};
    std::unique_ptr<QThread> launch(QThread::create([&] {
        const SingleInstance later(running.serverName());
        handedOver = later.handOver(files) ? 1 : 0;
    }));
    launch->start();
    REQUIRE(waitFor([&] { return launch->isFinished(); }));
    CHECK(handedOver == 1);
    REQUIRE(waitFor([&] { return !received.isEmpty(); }));
    REQUIRE(received.size() == 1);
    CHECK(received.front() == files); // all three, in order, as the same file URLs
}

TEST_CASE("Files the app was started with or handed over: media is imported and Home starts a project with it and a project opens",
          "[app][shell][instance][import][qml][d4]") {
    resetSettings();
    clearMessages();
    const QString video = sampleVideo();
    const fs::path dir = workDir("open-with");

    SECTION("in the editor the media goes into the current bin, with a message") {
        EditorShell s;
        auto* shell = s.shell.engine.singletonInstance<AppShell*>("Wabcut", "AppShell");
        REQUIRE(shell != nullptr);
        CHECK_FALSE(evaluate(s.shell, QStringLiteral("home.opened")).toBool());
        const std::size_t before = s.controller->snapshot()->media.size();
        shell->requestOpen({QUrl::fromLocalFile(video)});
        REQUIRE(waitFor([&] { return s.controller->snapshot()->media.size() != before; }, 20000));
        spin(200);
        CHECK(s.controller->snapshot()->media.size() >= 1);
        CHECK(s.toastText().contains(QStringLiteral("Imported 1 file")));
        const QStringList messages = takeMessages();
        INFO(join(messages));
        CHECK(messages.isEmpty());
    }

    SECTION("over Home the media starts the New project page") {
        EditorShell s;
        auto* shell = s.shell.engine.singletonInstance<AppShell*>("Wabcut", "AppShell");
        REQUIRE(shell != nullptr);
        evaluate(s.shell, QStringLiteral("home.open()"));
        REQUIRE(evaluate(s.shell, QStringLiteral("home.opened")).toBool());
        const std::size_t before = s.controller->snapshot()->media.size();
        shell->requestOpen({QUrl::fromLocalFile(video)});
        REQUIRE(waitFor([&] { return evaluate(s.shell, QStringLiteral("home.page")).toString() == QLatin1String("new"); }, 10000));
        // The page starts with that recording (the project is made when the user presses Create).
        CHECK(evaluate(s.shell, QStringLiteral("home.newPage().files.length")).toInt() == 1);
        CHECK(s.controller->snapshot()->media.size() == before);
        CHECK(evaluate(s.shell, QStringLiteral("home.opened")).toBool());
    }

    SECTION("a lone project opens as before") {
        EditorShell s;
        auto* shell = s.shell.engine.singletonInstance<AppShell*>("Wabcut", "AppShell");
        REQUIRE(shell != nullptr);
        const fs::path file = dir / "Film.wabcut";
        REQUIRE(s.project->saveAs(urlOf(q(file))));
        s.project->newProject(); // another (untitled) project is open now
        CHECK(s.project->filePath().isEmpty());
        shell->requestOpen({urlOf(q(file))});
        REQUIRE(waitFor([&] { return !s.project->filePath().isEmpty(); }, 10000));
        CHECK(QFileInfo(s.project->filePath()) == QFileInfo(q(file)));
    }
    resetSettings();
}

// ---- 006: File > Save Version... / Version History... -----------------------------------------------------------------------

TEST_CASE("File menu: Save Version and Version History need a saved project; Save Version asks for a name",
          "[app][history][menubar][qml][d4][screenshots]") {
    for (const bool dark : {true, false}) {
        DYNAMIC_SECTION((dark ? "dark" : "light")) {
            resetSettings();
            QSettings().remove(QStringLiteral("history"));
            clearMessages();
            EditorShell s(dark);
            auto* history = s.shell.engine.singletonInstance<HistoryBridge*>("Wabcut", "History");
            REQUIRE(history != nullptr);

            // The sample project is not saved: no history, the rows are greyed out and the command says why.
            CHECK_FALSE(history->available());
            QObject* file = s.open(0, "fileMenu");
            QObject* save = file->findChild<QObject*>(QStringLiteral("fileMenuSaveVersion"));
            QObject* versions = file->findChild<QObject*>(QStringLiteral("fileMenuVersionHistory"));
            REQUIRE(save != nullptr);
            REQUIRE(versions != nullptr);
            CHECK(save->property("text").toString() == QString::fromUtf8("Save Version…"));
            CHECK(versions->property("text").toString() == QString::fromUtf8("Version History…"));
            CHECK_FALSE(save->property("enabled").toBool());
            CHECK_FALSE(versions->property("enabled").toBool());
            s.dismiss(file);
            evaluate(s.shell, QStringLiteral("runCommand(\"file.saveVersion\")"));
            CHECK(s.toastText() == history->reason());
            CHECK_FALSE(shown(s.shell.object("saveVersionDialog")));
            // The command palette lists both, greyed out with the same reason.
            auto* palette = s.shell.engine.singletonInstance<CommandPalette*>("Wabcut", "CommandPalette");
            REQUIRE(palette != nullptr);
            for (const char* id : {"file.saveVersion", "file.versionHistory"}) {
                INFO(id);
                CHECK(KeymapCommandProvider::viewHandles(QString::fromLatin1(id)));
                const QVariantMap state = palette->commandState(QString::fromLatin1(id));
                CHECK(state.value(QStringLiteral("known")).toBool());
                CHECK_FALSE(state.value(QStringLiteral("enabled")).toBool());
                CHECK(state.value(QStringLiteral("reason")).toString() == history->reason());
            }

            // Saved: the history exists, the rows come alive.
            const fs::path film = workDir("save-version") / "Film.wabcut";
            REQUIRE(s.project->saveAs(urlOf(q(film))));
            REQUIRE(waitFor([&] { return history->available(); }, 5000));
            file = s.open(0, "fileMenu");
            CHECK(file->findChild<QObject*>(QStringLiteral("fileMenuSaveVersion"))->property("enabled").toBool());
            CHECK(file->findChild<QObject*>(QStringLiteral("fileMenuVersionHistory"))->property("enabled").toBool());
            s.dismiss(file);
            for (const char* id : {"file.saveVersion", "file.versionHistory"}) {
                INFO(id);
                CHECK(palette->commandState(QString::fromLatin1(id)).value(QStringLiteral("enabled")).toBool());
            }

            // Save Version…: a name is needed; Enter keeps the project as a named version (never an undo step).
            const auto undoSteps = s.controller->session().undoCount();
            const int before = history->versions().size();
            evaluate(s.shell, QStringLiteral("runCommand(\"file.saveVersion\")"));
            QObject* dialog = s.shell.object("saveVersionDialog");
            REQUIRE(dialog != nullptr);
            REQUIRE(waitFor([&] { return opened(dialog); }));
            QQuickItem* name = visibleNamed(s.shell, QStringLiteral("saveVersionName"));
            QQuickItem* ok = visibleNamed(s.shell, QStringLiteral("saveVersionOk"));
            REQUIRE(name != nullptr);
            REQUIRE(ok != nullptr);
            CHECK_FALSE(ok->isEnabled()); // nothing typed yet
            spin(200);
            saveShot(s.shell.grab(), s.shell.window->contentItem(),
                     dark ? QStringLiteral("save-version-dialog.png") : QStringLiteral("save-version-dialog-light.png"));
            name->setProperty("text", QStringLiteral("  "));
            CHECK_FALSE(ok->isEnabled());
            name->setProperty("text", QStringLiteral("Client cut v2"));
            CHECK(ok->isEnabled());
            REQUIRE(QMetaObject::invokeMethod(name, "accepted"));
            REQUIRE(waitFor([&] { return !shown(dialog); }));
            REQUIRE(history->versions().size() == before + 1);
            const QVariantMap newest = history->versions().first().toMap();
            CHECK(newest.value(QStringLiteral("kind")).toString() == QStringLiteral("manual"));
            CHECK(newest.value(QStringLiteral("title")).toString() == QStringLiteral("Client cut v2"));
            CHECK(newest.value(QStringLiteral("named")).toBool());
            CHECK(s.toastText() == QString::fromUtf8("Saved version “Client cut v2”"));
            CHECK(s.controller->session().undoCount() == undoSteps);

            // Version History…: the Review panel on its Versions page.
            evaluate(s.shell, QStringLiteral("runCommand(\"file.versionHistory\")"));
            QQuickItem* tabs = nullptr;
            REQUIRE(waitFor([&] { return (tabs = visibleNamed(s.shell, QStringLiteral("reviewTabs"))) != nullptr; }, 5000));
            REQUIRE(waitFor([&] { return tabs->property("currentIndex").toInt() == 1; }, 3000));
            CHECK(visibleNamed(s.shell, QStringLiteral("historyVersionName")) != nullptr); // the Versions page

            const QStringList messages = takeMessages();
            INFO(join(messages));
            CHECK(messages.isEmpty());
            resetSettings();
        }
    }
}

TEST_CASE("Version history interval: off and 2 and 5 and 10 or 30 minutes and kept between sessions", "[app][history][settings][d4]") {
    resetSettings();
    QSettings().remove(QStringLiteral("history"));
    {
        HistoryBridge history;
        CHECK(history.autoIntervalSeconds() == 300); // the default: 5 minutes
        history.setAutoIntervalSeconds(1800);
        CHECK(QSettings().value(QStringLiteral("history/autoIntervalSeconds")).toInt() == 1800);
    }
    {
        HistoryBridge later;
        CHECK(later.autoIntervalSeconds() == 1800); // the next session starts there
        later.setAutoIntervalSeconds(0);
        CHECK(later.autoIntervalSeconds() == 0);
    }
    {
        HistoryBridge off;
        CHECK(off.autoIntervalSeconds() == 0); // Off is kept too
    }
    // A value no setting offers (a hand-edited file) is the default again.
    for (const int bad : {-5, 24 * 60 * 60 + 1, 999999999}) {
        QSettings().setValue(QStringLiteral("history/autoIntervalSeconds"), bad);
        HistoryBridge odd;
        INFO(bad);
        CHECK(odd.autoIntervalSeconds() == 300);
    }
    // The setter keeps within a day.
    HistoryBridge clamp;
    clamp.setAutoIntervalSeconds(10 * 24 * 60 * 60);
    CHECK(clamp.autoIntervalSeconds() == 24 * 60 * 60);
    clamp.setAutoIntervalSeconds(-3);
    CHECK(clamp.autoIntervalSeconds() == 0);
    resetSettings();
}

TEST_CASE("Version history interval: the checkpoints follow the interval and Off keeps only the saved versions",
          "[app][history][d4]") {
    resetSettings();
    QSettings().remove(QStringLiteral("history"));
    Timeline tl(R"(
        V1 tgt |AaaaaaaaaaBbbbbbbbbbCccccccccc|
        A1 tgt |AaaaaaaaaaBbbbbbbbbbCccccccccc|
    )");
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    const fs::path film = workDir("interval") / "Film.wabcut";
    controller.setProjectFile(film);
    HistoryBridge history;
    history.setController(&controller);
    REQUIRE(history.available());
    history.onSaved();
    REQUIRE(history.versions().size() == 1);

    // Every second, and only when something changed since the last one.
    history.setAutoIntervalSeconds(1);
    controller.setSelection({tl.clip("V1@10")});
    controller.deleteSelection();
    REQUIRE(waitFor([&] { return history.versions().size() == 2; }, 8000));
    CHECK(history.versions().first().toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("auto"));

    // Off: a further edit gets no automatic version (a named one still can be kept).
    history.setAutoIntervalSeconds(0);
    controller.setSelection({tl.clip("V1@20")});
    controller.deleteSelection();
    spin(2500);
    CHECK(history.versions().size() == 2);
    CHECK(history.saveVersion(QStringLiteral("by hand")).isEmpty());
    CHECK(history.versions().size() == 3);
    resetSettings();
}

TEST_CASE("Settings General: Keep versions every shows the interval and changes it; the search finds it",
          "[app][history][settings][qml][d4][screenshots]") {
    for (const bool dark : {true, false}) {
        DYNAMIC_SECTION((dark ? "dark" : "light")) {
            resetSettings();
            QSettings().remove(QStringLiteral("history"));
            clearMessages();
            Shell shell;
            REQUIRE(shell.window != nullptr);
            INFO(join(shell.errors));
            shell.resize(kShotWidth, kShotHeight);
            shell.theme->setProperty("reduceMotion", true);
            shell.setAppearance(dark, "ultraviolet");
            auto* history = shell.engine.singletonInstance<HistoryBridge*>("Wabcut", "History");
            REQUIRE(history != nullptr);
            QObject* loader = shell.object("settingsWindowLoader");
            REQUIRE(loader != nullptr);
            loader->setProperty("active", true);
            QObject* window = loader->property("item").value<QObject*>();
            REQUIRE(window != nullptr);
            REQUIRE(QMetaObject::invokeMethod(window, "openAt", Q_ARG(QString, QStringLiteral("general"))));
            REQUIRE(waitFor([&] { return window->property("opened").toBool(); }));
            spin(100);
            QQuickItem* content = window->property("contentItem").value<QQuickItem*>();
            QQuickItem* tabs = findItem(content, [](QQuickItem* i) { return i->objectName() == QLatin1String("versionIntervalTabs"); });
            REQUIRE(tabs != nullptr);
            CHECK(tabs->property("currentIndex").toInt() == 2); // 5 min
            REQUIRE(QMetaObject::invokeMethod(tabs, "activated", Q_ARG(int, 0)));
            CHECK(history->autoIntervalSeconds() == 0);
            CHECK(tabs->property("currentIndex").toInt() == 0);
            REQUIRE(QMetaObject::invokeMethod(tabs, "activated", Q_ARG(int, 4)));
            CHECK(history->autoIntervalSeconds() == 1800);
            history->setAutoIntervalSeconds(120); // the row follows a change made elsewhere
            CHECK(tabs->property("currentIndex").toInt() == 1);
            history->setAutoIntervalSeconds(300);
            spin(100);
            saveShot(shell.grab(), content, dark ? QStringLiteral("settings-version-interval.png") : QStringLiteral("settings-version-interval-light.png"));
            const QStringList messages = takeMessages();
            INFO(join(messages));
            CHECK(messages.isEmpty());
            resetSettings();
        }
    }
}

// ---- 016: Help > Report a Bug... -----------------------------------------------------------------------------------------

TEST_CASE("System information: the version and system and graphics and Qt - never a path or a user name", "[app][crash][bugreport][d4]") {
    CrashNotice notice;
    CrashNotice::setProductVersion(QStringLiteral("0.7.0 (abc1234)"));
    const QString info = notice.systemInfo(QStringLiteral("D3D11 on NVIDIA GeForce RTX 3060"));
    INFO(info.toStdString());
    CHECK(info.startsWith(QStringLiteral("Wabcut: 0.7.0 (abc1234)")));
    CHECK(info.contains(QStringLiteral("\nOS: ")));
    CHECK(info.contains(QStringLiteral("\nCPU: ")));
    CHECK(info.contains(QStringLiteral("\nGraphics: D3D11 on NVIDIA GeForce RTX 3060")));
    CHECK(info.contains(QStringLiteral("\nVideo decoding: ")));
    CHECK(info.contains(QStringLiteral("software")));
    CHECK(info.contains(QStringLiteral("\nQt: ") + QString::fromLatin1(qVersion())));
    CHECK(info.contains(QStringLiteral("\nInterface size: ")));
    CHECK(info.contains(QStringLiteral("\nScreens: ")));
    // The line of the renderer is left out when it is not known yet.
    CHECK_FALSE(notice.systemInfo({}).contains(QStringLiteral("Graphics:")));
    // Redacted: nothing that names the user or their folders.
    const QString home = QDir::homePath();
    CHECK_FALSE(info.contains(home));
    CHECK_FALSE(info.contains(QDir::toNativeSeparators(home)));
    if (const QString user = qEnvironmentVariable("USERNAME"); user.size() > 2)
        CHECK_FALSE(info.contains(user));
    CHECK_FALSE(info.contains(QStringLiteral("C:\\")));
    CHECK_FALSE(info.contains(QStringLiteral("/home/")));
    // A path or an address a renderer name might carry is redacted on the way out.
    CHECK_FALSE(notice.systemInfo(QStringLiteral("D3D11 at C:\\Users\\someone\\secret.txt")).contains(QStringLiteral("someone")));
    CrashNotice::setProductVersion({});
}

TEST_CASE("Report a Bug address: the issue form with the version and the system information and under 6 KB and encoded",
          "[app][crash][bugreport][d4]") {
    CrashNotice notice;
    CrashNotice::setProductVersion(QStringLiteral("0.7.0 (abc1234)"));
    const QString info = notice.systemInfo(QStringLiteral("D3D11 on NVIDIA GeForce RTX 3060"));
    const QString address = notice.bugReportUrl(info);
    CHECK(address.startsWith(QStringLiteral("https://github.com/sorsama/wabcut/issues/new?template=bug-report.yml&version=")));
    CHECK(address.size() < 6000);
    const QUrl url(address);
    REQUIRE(url.isValid());
    const QUrlQuery query(url);
    CHECK(query.queryItemValue(QStringLiteral("template")) == QStringLiteral("bug-report.yml"));
    CHECK(query.queryItemValue(QStringLiteral("version"), QUrl::FullyDecoded) == QStringLiteral("0.7.0 (abc1234)"));
    CHECK(query.queryItemValue(QStringLiteral("system"), QUrl::FullyDecoded) == info);
    CHECK_FALSE(address.contains(QLatin1Char('\n'))); // newlines travel encoded (%0A)
    CHECK_FALSE(address.contains(QLatin1Char(' ')));

    // A huge text is cut at a line boundary to stay shareable; what is kept is the start of it.
    QStringList lines;
    for (int i = 0; i < 400; ++i)
        lines << QStringLiteral("Line %1: ").arg(i) + QString(40, QLatin1Char('x'));
    const QString big = lines.join(QLatin1Char('\n'));
    const QString cut = notice.bugReportUrl(big);
    CHECK(cut.size() <= 6000);
    const QString kept = QUrlQuery(QUrl(cut)).queryItemValue(QStringLiteral("system"), QUrl::FullyDecoded);
    CHECK(big.startsWith(kept));
    CHECK(kept.startsWith(QStringLiteral("Line 0: ")));
    CHECK(kept.size() < big.size());
    CrashNotice::setProductVersion({});
}

TEST_CASE("Help > Report a Bug opens the dialog; it copies the system information and About copies the version",
          "[app][menubar][bugreport][qml][d4][screenshots]") {
    for (const bool dark : {true, false}) {
        DYNAMIC_SECTION((dark ? "dark" : "light")) {
            resetSettings();
            clearMessages();
            EditorShell s(dark);
            s.shell.preferences->setProperty("welcomeDone", true);

            // The Help menu's row (it replaced "Report an Issue").
            QObject* help = s.open(8, "helpMenu");
            QObject* row = help->findChild<QObject*>(QStringLiteral("helpItem_reportBug"));
            REQUIRE(row != nullptr);
            CHECK(row->property("text").toString() == QString::fromUtf8("Report a Bug…"));
            CHECK(row->property("enabled").toBool());
            CHECK(help->findChild<QObject*>(QStringLiteral("helpItem_reportIssue")) == nullptr);
            s.dismiss(help);

            QObject* dialog = s.shell.object("reportBugDialog");
            REQUIRE(dialog != nullptr);
            CHECK_FALSE(shown(dialog));
            REQUIRE(QMetaObject::invokeMethod(s.shell.window.get(), "menuAction", Q_ARG(QString, QStringLiteral("reportBug")), Q_ARG(QVariant, QVariant())));
            REQUIRE(waitFor([&] { return opened(dialog); }));
            spin(250);

            // It shows the text that Copy System Info copies and the address "Open on GitHub" opens.
            QQuickItem* infoText = visibleNamed(s.shell, QStringLiteral("reportBugInfo"));
            REQUIRE(infoText != nullptr);
            const QString info = infoText->property("text").toString();
            CHECK(info.startsWith(QStringLiteral("Wabcut: ")));
            CHECK(info.contains(QStringLiteral("\nQt: ")));
            CHECK(dialog->property("issueUrl").toString().startsWith(QStringLiteral("https://github.com/sorsama/wabcut/issues/new?template=bug-report.yml")));
            for (const char* name : {"reportBugOpenGitHub", "reportBugCopy", "reportBugCrashReports", "reportBugClose"}) {
                INFO(name);
                QQuickItem* button = visibleNamed(s.shell, QString::fromLatin1(name));
                REQUIRE(button != nullptr);
                CHECK(button->isEnabled());
                const QRectF at = button->mapRectToScene(QRectF(0, 0, button->width(), button->height()));
                CHECK(at.left() >= 0); // inside the window, not run off an edge
                CHECK(at.right() <= s.w->width());
            }
            QQuickItem* copy = visibleNamed(s.shell, QStringLiteral("reportBugCopy"));
            REQUIRE(copy != nullptr);
            CHECK(pressCopy(copy, [&](const QString& text) { return text == info; }) == info);
            CHECK(copy->property("text").toString() == QStringLiteral("Copied"));
            saveShot(s.shell.grab(), s.shell.window->contentItem(), dark ? QStringLiteral("report-bug-dialog.png") : QStringLiteral("report-bug-dialog-light.png"));
            REQUIRE(QMetaObject::invokeMethod(visibleNamed(s.shell, QStringLiteral("reportBugClose")), "clicked"));
            REQUIRE(waitFor([&] { return !shown(dialog); }));

            // About: the version line can be copied.
            QObject* about = s.shell.object("aboutDialog");
            REQUIRE(about != nullptr);
            REQUIRE(QMetaObject::invokeMethod(about, "open"));
            REQUIRE(waitFor([&] { return opened(about); }));
            QQuickItem* copyVersion = visibleNamed(s.shell, QStringLiteral("aboutCopyVersion"));
            REQUIRE(copyVersion != nullptr);
            const QString copied = pressCopy(copyVersion, [](const QString& text) { return text.startsWith(QStringLiteral("Wabcut ")); });
            CHECK(copied.startsWith(QStringLiteral("Wabcut ")));
            CHECK(copied.contains(QGuiApplication::applicationVersion()));
            spin(150);
            saveShot(s.shell.grab(), s.shell.window->contentItem(), dark ? QStringLiteral("about-copy-version.png") : QStringLiteral("about-copy-version-light.png"));
            REQUIRE(QMetaObject::invokeMethod(about, "close"));

            const QStringList messages = withoutClipboardNoise(takeMessages());
            INFO(join(messages));
            CHECK(messages.isEmpty());
            resetSettings();
        }
    }
}

// ---- 017: the system menu and Snap Layouts -------------------------------------------------------------------------------

TEST_CASE("The system menu: which rows a window in each state offers", "[app][shell][window][chrome][d4]") {
    using chrome::SystemMenuState;
    // Normal and resizable: everything but Restore.
    CHECK(chrome::systemMenuState(false, false, true) == SystemMenuState{false, true, true, true, true, true});
    // Maximized: Restore, Minimize and Close; no Move, Size or Maximize.
    CHECK(chrome::systemMenuState(true, false, true) == SystemMenuState{true, false, false, true, false, true});
    // Minimized: Restore and Maximize come back, Minimize goes.
    CHECK(chrome::systemMenuState(false, true, true) == SystemMenuState{true, false, false, false, true, true});
    // A window of one size cannot be sized or maximized.
    CHECK(chrome::systemMenuState(false, false, false) == SystemMenuState{false, true, false, true, false, true});
#ifdef _WIN32
    CHECK(chrome::systemMenuAvailable());
    // The menu pops asynchronously and needs a window.
    CHECK_FALSE(chrome::showSystemMenu(nullptr, QPointF()));
#else
    CHECK_FALSE(chrome::systemMenuAvailable());
    CHECK_FALSE(chrome::snapLayoutsAvailable());
    CHECK_FALSE(chrome::showSystemMenu(nullptr, QPointF()));
#endif
}

TEST_CASE("The title bar offers the system menu and its maximize pill is registered for Snap Layouts", "[app][shell][window][chrome][qml][d4]") {
    resetSettings();
    clearMessages();
    EditorShell s;
    auto* shell = s.shell.engine.singletonInstance<AppShell*>("Wabcut", "AppShell");
    REQUIRE(shell != nullptr);
    CHECK(shell->systemMenuAvailable() == chrome::systemMenuAvailable());
    CHECK(shell->snapLayoutsAvailable() == chrome::snapLayoutsAvailable());

    // Both WindowControls (the editor's title bar and Home's header) registered their maximize pill.
    CHECK(chrome::WindowChromeFilter::instance().buttonCount() >= 1);
    QQuickItem* maximize = visibleNamed(s.shell, QStringLiteral("windowMaximizeButton"));
    REQUIRE(maximize != nullptr);
    CHECK(maximize->property("nativeHovered").toBool() == false);
    CHECK(maximize->property("nativePressed").toBool() == false);

    // The filter finds that button where it is (in the window's pixels), and nothing beside it.
    const double dpr = s.w->devicePixelRatio();
    const QPointF middle = maximize->mapToScene(QPointF(maximize->width() / 2, maximize->height() / 2));
    const QPoint pixel(static_cast<int>(middle.x() * dpr), static_cast<int>(middle.y() * dpr));
    CHECK(chrome::WindowChromeFilter::instance().buttonAt(s.w, pixel) == maximize);
    CHECK(chrome::WindowChromeFilter::instance().buttonAt(s.w, QPoint(pixel.x() - static_cast<int>(60 * dpr), pixel.y())) != maximize);
    CHECK(chrome::WindowChromeFilter::instance().buttonAt(s.w, QPoint(-5, -5)) == nullptr);
    CHECK(chrome::WindowChromeFilter::instance().buttonAt(nullptr, pixel) == nullptr);
    // Home covers the editor's bar: its pill stops being the window's maximize button and Home's own takes over.
    CHECK(maximize->property("snapEnabled").toBool());
    evaluate(s.shell, QStringLiteral("home.open()"));
    spin(300);
    CHECK_FALSE(maximize->property("snapEnabled").toBool());
    CHECK(chrome::WindowChromeFilter::instance().buttonAt(s.w, pixel) != maximize);
    int homePills = 0;
    findItem(s.w->contentItem(), [&](QQuickItem* i) {
        if (i->objectName() == QLatin1String("windowMaximizeButton") && i != maximize && i->isVisible()) {
            ++homePills;
            CHECK(i->property("snapEnabled").toBool());
            const QPointF c = i->mapToScene(QPointF(i->width() / 2, i->height() / 2));
            CHECK(chrome::WindowChromeFilter::instance().buttonAt(s.w, QPoint(static_cast<int>(c.x() * dpr), static_cast<int>(c.y() * dpr))) == i);
        }
        return false;
    });
    CHECK(homePills == 1);
    evaluate(s.shell, QStringLiteral("home.close()"));
    spin(300);
    CHECK(maximize->property("snapEnabled").toBool());
    CHECK(chrome::WindowChromeFilter::instance().buttonAt(s.w, pixel) == maximize);

    // The title bar's right-click handler and the Alt+Space shortcut are there (the shortcut only with the custom frame).
    CHECK(s.shell.object("titleBarSystemMenuTap") != nullptr); // a pointer handler of the empty title bar
    QObject* shortcut = s.shell.object("systemMenuShortcut");
    REQUIRE(shortcut != nullptr);
    CHECK(shortcut->property("enabled").toBool() == chrome::systemMenuAvailable());
    CHECK(shortcut->property("sequence").toString().contains(QStringLiteral("Alt")));
    // The window asks for its system menu (the taskbar's menu uses it).
    CHECK((s.w->flags() & Qt::WindowSystemMenuHint));
    CHECK((s.w->flags() & Qt::FramelessWindowHint));
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
    resetSettings();
}

#ifdef _WIN32
TEST_CASE("Snap Layouts: the system's hit test answers the maximize button and the pointer messages become its look and click",
          "[app][shell][window][chrome][d4]") {
    // A window of its own with a button that behaves like WindowControls' (hover and pressed flags, a clicked signal).
    QQmlEngine engine;
    QQuickWindow window;
    window.resize(400, 120);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        Item {
            id: button
            property bool nativeHovered: false
            property bool nativePressed: false
            property int clicks: 0
            signal clicked()
            onClicked: clicks++
            x: 300; y: 10; width: 40; height: 30
        }
    )",
                      QUrl());
    std::unique_ptr<QObject> object(component.create());
    INFO(component.errorString().toStdString());
    REQUIRE(object);
    auto* button = qobject_cast<QQuickItem*>(object.get());
    REQUIRE(button != nullptr);
    button->setParentItem(window.contentItem());
    auto& filter = chrome::WindowChromeFilter::instance();
    const int registered = filter.buttonCount();
    filter.addMaximizeButton(button);
    filter.addMaximizeButton(button); // once
    CHECK(filter.buttonCount() == registered + 1);

    const HWND hwnd = reinterpret_cast<HWND>(window.winId());
    REQUIRE(hwnd != nullptr);
    const double dpr = window.devicePixelRatio();
    // A message about the point (x, y) of the client area, in logical pixels.
    const auto atPoint = [&](UINT message, WPARAM wParam, double x, double y) {
        POINT point{static_cast<LONG>(x * dpr), static_cast<LONG>(y * dpr)};
        ClientToScreen(hwnd, &point);
        MSG msg{};
        msg.hwnd = hwnd;
        msg.message = message;
        msg.wParam = wParam;
        msg.lParam = MAKELPARAM(static_cast<WORD>(point.x), static_cast<WORD>(point.y));
        return msg;
    };
    qintptr result = 0;

    // The hit test: over the button the answer is "maximize button" (what shows the flyout); elsewhere the window decides.
    CHECK(filter.handleMessage(&window, atPoint(WM_NCHITTEST, 0, 320, 25), &result));
    CHECK(result == HTMAXBUTTON);
    result = 0;
    CHECK_FALSE(filter.handleMessage(&window, atPoint(WM_NCHITTEST, 0, 100, 25), &result));
    CHECK_FALSE(filter.handleMessage(&window, atPoint(WM_NCHITTEST, 0, 320, 80), &result));
    CHECK_FALSE(filter.handleMessage(&window, atPoint(WM_NCHITTEST, 0, 299, 25), &result)); // just left of it

    // Hover: the system now sends non-client moves; the button is lit until the pointer leaves or returns to the client area.
    CHECK(filter.handleMessage(&window, atPoint(WM_NCMOUSEMOVE, HTMAXBUTTON, 320, 25), &result));
    CHECK(button->property("nativeHovered").toBool());
    CHECK(filter.hoveredButton() == button);
    CHECK_FALSE(filter.handleMessage(&window, atPoint(WM_MOUSEMOVE, 0, 100, 25), &result));
    CHECK_FALSE(button->property("nativeHovered").toBool());
    CHECK(filter.handleMessage(&window, atPoint(WM_NCMOUSEMOVE, HTMAXBUTTON, 320, 25), &result));
    CHECK_FALSE(filter.handleMessage(&window, atPoint(WM_NCMOUSELEAVE, 0, 320, 25), &result));
    CHECK_FALSE(button->property("nativeHovered").toBool());

    // A click: press on it (swallowed, shown pressed), release on it (swallowed, clicked once).
    CHECK(filter.handleMessage(&window, atPoint(WM_NCLBUTTONDOWN, HTMAXBUTTON, 320, 25), &result));
    CHECK(button->property("nativePressed").toBool());
    CHECK(filter.handleMessage(&window, atPoint(WM_NCLBUTTONUP, HTMAXBUTTON, 320, 25), &result));
    CHECK_FALSE(button->property("nativePressed").toBool());
    REQUIRE(waitFor([&] { return button->property("clicks").toInt() == 1; }, 3000));

    // A release without a press on it is no click (the press began elsewhere).
    CHECK(filter.handleMessage(&window, atPoint(WM_NCLBUTTONUP, HTMAXBUTTON, 320, 25), &result));
    spin(100);
    CHECK(button->property("clicks").toInt() == 1);

    // The button leaves: the filter forgets it (and a destroyed one never matters).
    filter.removeMaximizeButton(button);
    CHECK(filter.buttonCount() == registered);
    CHECK_FALSE(filter.handleMessage(&window, atPoint(WM_NCHITTEST, 0, 320, 25), &result));
}

// Review: Windows 11 shows the Snap Layouts flyout only for a window with WS_MAXIMIZEBOX.
TEST_CASE("Snap Layouts: the window the filter answers for has WS_MAXIMIZEBOX", "[app][shell][window][chrome][d4]") {
    // The style bit's value and the helper (every platform builds this).
    CHECK(chrome::kMaximizeBoxStyle == static_cast<unsigned long>(WS_MAXIMIZEBOX));
    CHECK(chrome::withMaximizeBox(0x80000000UL) == (0x80000000UL | WS_MAXIMIZEBOX));
    CHECK(chrome::withMaximizeBox(chrome::withMaximizeBox(0)) == chrome::kMaximizeBoxStyle);

    const auto styleOf = [](QWindow& window) {
        return static_cast<unsigned long>(GetWindowLongPtrW(reinterpret_cast<HWND>(window.winId()), GWL_STYLE));
    };
    // The editor's window flags (Main.qml: customFrame) give the frameless window its box.
    QQuickWindow frameless;
    frameless.setFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowMinMaxButtonsHint | Qt::WindowSystemMenuHint);
    frameless.resize(300, 200);
    CHECK((styleOf(frameless) & WS_MAXIMIZEBOX) != 0);

    // A frameless window made without the hint has none - until the filter answers a message for it.
    QQuickWindow bare;
    bare.setFlags(Qt::Window | Qt::FramelessWindowHint);
    bare.resize(300, 200);
    const HWND hwnd = reinterpret_cast<HWND>(bare.winId());
    REQUIRE(hwnd != nullptr);
    REQUIRE((styleOf(bare) & WS_MAXIMIZEBOX) == 0);
    CHECK(chrome::ensureMaximizeBox(&bare));
    CHECK((styleOf(bare) & WS_MAXIMIZEBOX) != 0);
    CHECK(chrome::ensureMaximizeBox(&bare)); // already there: still true, nothing changes
    CHECK_FALSE(chrome::ensureMaximizeBox(nullptr));

    // The filter itself does it on the first message it answers (the hit test the flyout starts from).
    QQuickWindow viaFilter;
    viaFilter.setFlags(Qt::Window | Qt::FramelessWindowHint);
    viaFilter.resize(300, 200);
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData("import QtQuick\nItem { property bool nativeHovered: false; property bool nativePressed: false; "
                      "signal clicked(); x: 200; y: 10; width: 40; height: 30 }",
                      QUrl());
    std::unique_ptr<QObject> object(component.create());
    REQUIRE(object);
    auto* button = qobject_cast<QQuickItem*>(object.get());
    REQUIRE(button != nullptr);
    button->setParentItem(viaFilter.contentItem());
    auto& filter = chrome::WindowChromeFilter::instance();
    filter.addMaximizeButton(button);
    REQUIRE((styleOf(viaFilter) & WS_MAXIMIZEBOX) == 0);
    MSG msg{};
    msg.hwnd = reinterpret_cast<HWND>(viaFilter.winId());
    msg.message = WM_NCHITTEST;
    POINT point{static_cast<LONG>(220 * viaFilter.devicePixelRatio()), static_cast<LONG>(25 * viaFilter.devicePixelRatio())};
    ClientToScreen(msg.hwnd, &point);
    msg.lParam = MAKELPARAM(static_cast<WORD>(point.x), static_cast<WORD>(point.y));
    qintptr result = 0;
    CHECK(filter.handleMessage(&viaFilter, msg, &result));
    CHECK(result == HTMAXBUTTON);
    CHECK((styleOf(viaFilter) & WS_MAXIMIZEBOX) != 0);
    filter.removeMaximizeButton(button);

    // A window of one size cannot be maximized: no box.
    QQuickWindow fixed;
    fixed.setFlags(Qt::Window | Qt::FramelessWindowHint);
    fixed.setMinimumSize(QSize(300, 200));
    fixed.setMaximumSize(QSize(300, 200));
    fixed.resize(300, 200);
    CHECK_FALSE(chrome::ensureMaximizeBox(&fixed));
    CHECK((styleOf(fixed) & WS_MAXIMIZEBOX) == 0);
}
#else
TEST_CASE("Snap Layouts: off Windows nothing touches a window style", "[app][shell][window][chrome][d4]") {
    CHECK(chrome::withMaximizeBox(0x80000000UL) == (0x80000000UL | 0x00010000UL));
    CHECK_FALSE(chrome::ensureMaximizeBox(nullptr));
}
#endif

// ---- 111: transcribing a recording that switches language ------------------------------------------------------------------

namespace {

/// An English / Thai transcript of the mixed kind: every segment names its language (what the transcription makes).
std::shared_ptr<const ai::Transcript> mixedTranscript() {
    auto t = std::make_shared<ai::Transcript>();
    t->language = "en";
    const double frame = static_cast<double>(framesToTicks(1, kRate).value);
    const auto word = [&](const char* text, double from, double to) {
        ai::TranscriptWord w;
        w.text = text;
        w.t0 = Ticks{static_cast<std::int64_t>(from * frame)};
        w.t1 = Ticks{static_cast<std::int64_t>(to * frame)};
        w.confidence = 0.9f;
        t->words.push_back(std::move(w));
    };
    word("How", 0, 3);
    word("was", 4, 7);
    word("the", 8, 10);
    word("match", 11, 16);
    word("ดี", 20, 22);
    word("มาก", 23, 26);
    word("Great", 34, 38);
    word("game", 39, 44);
    const auto segment = [&](std::size_t first, std::size_t count, const char* text, const char* language) {
        ai::TranscriptSegment s;
        s.firstWord = first;
        s.wordCount = count;
        s.text = text;
        s.language = language;
        s.t0 = t->words[first].t0;
        s.t1 = t->words[first + count - 1].t1;
        t->segments.push_back(std::move(s));
    };
    segment(0, 4, "How was the match", "en");
    segment(4, 2, "ดีมาก", "th");
    segment(6, 2, "Great game", "en");
    return t;
}

struct MixedFixture {
    Timeline tl{R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )"};
    QTemporaryDir dir;
    ProjectController controller;
    TranscriptBridge bridge;

    MixedFixture() {
        tl.linkMatching();
        REQUIRE(dir.isValid());
        controller.resetProject(tl.state, tl.seq);
        bridge.setUseAnalysisCache(false);
        bridge.setController(&controller);
    }
};

} // namespace

TEST_CASE("Mixed languages: the switch is remembered and needs auto-detect and the local transcriber and says what it costs",
          "[app][transcript][language][mixed][d4]") {
    resetSettings();
    QSettings().remove(QStringLiteral("transcript"));
    MixedFixture fx;
    TranscriptBridge& t = fx.bridge;
    CHECK_FALSE(t.mixedLanguages());
    CHECK(t.spokenLanguageNote().isEmpty());
    t.setMixedLanguages(true);
    CHECK(t.mixedLanguages());
    CHECK(QSettings().value(QStringLiteral("transcript/mixedLanguages")).toBool());
    {
        TranscriptBridge later;
        CHECK(later.mixedLanguages()); // the next session starts there
    }
    // The note (the local transcriber's advice) says each stretch is listened to apart; the cloud has none to give.
    t.setProvider(QStringLiteral("local"));
    CHECK(t.spokenLanguageNote().contains(QStringLiteral("takes longer")));
    // A named language says it is one language: the switch has nothing to say then.
    t.setSpokenLanguage(QStringLiteral("th"));
    CHECK_FALSE(t.spokenLanguageNote().contains(QStringLiteral("takes longer")));
    t.setSpokenLanguage(QStringLiteral("auto"));
    t.setTranslateToEnglish(true);
    CHECK_FALSE(t.spokenLanguageNote().contains(QStringLiteral("each stretch")));
    t.setTranslateToEnglish(false);
    t.setMixedLanguages(false);
    CHECK_FALSE(QSettings().value(QStringLiteral("transcript/mixedLanguages")).toBool());
    resetSettings();
}

TEST_CASE("Mixed languages: a transcript of several languages names each paragraph's language and the shares",
          "[app][transcript][language][mixed][d4]") {
    resetSettings();
    QSettings().remove(QStringLiteral("transcript"));
    MixedFixture fx;
    TranscriptBridge& t = fx.bridge;

    // One language: nothing is named (the paragraphs carry their code only).
    {
        auto one = std::make_shared<ai::Transcript>(*mixedTranscript());
        for (auto& s : one->segments)
            s.language.clear();
        t.addTranscript(fx.tl.media('A'), 0, one);
        CHECK(t.languageSummary().isEmpty());
        for (int i = 0; i < t.paragraphs()->rowCount(); ++i) {
            CHECK(t.paragraphs()->get(i).value(QStringLiteral("languageName")).toString().isEmpty());
        }
    }

    t.addTranscript(fx.tl.media('A'), 0, mixedTranscript());
    REQUIRE(t.paragraphs()->rowCount() == 3); // a paragraph starts where the language changes
    const auto row = [&](int i) { return t.paragraphs()->get(i); };
    CHECK(row(0).value(QStringLiteral("language")).toString() == QStringLiteral("en"));
    CHECK(row(0).value(QStringLiteral("languageName")).toString() == QStringLiteral("English"));
    CHECK(row(1).value(QStringLiteral("language")).toString() == QStringLiteral("th"));
    CHECK(row(1).value(QStringLiteral("languageName")).toString() == QStringLiteral("Thai"));
    CHECK(row(2).value(QStringLiteral("languageName")).toString() == QStringLiteral("English"));
    CHECK(row(1).value(QStringLiteral("first")).toInt() == 4);
    CHECK(row(1).value(QStringLiteral("last")).toInt() == 5);
    // 6 of the 8 words are English, 2 Thai: most words first.
    CHECK(t.languageSummary() == QStringLiteral("English 75 % · Thai 25 %"));
    CHECK(t.language() == QStringLiteral("en"));
    CHECK_FALSE(t.rtl());
    resetSettings();
}

TEST_CASE("Mixed languages in the shell: the switch shows with auto-detect and paragraphs and the summary show languages; screenshots",
          "[app][transcript][language][mixed][qml][d4][screenshots]") {
    for (const bool dark : {true, false}) {
        DYNAMIC_SECTION((dark ? "dark" : "light")) {
            resetSettings();
            QSettings().remove(QStringLiteral("transcript"));
            clearMessages();
            Shell shell;
            REQUIRE(shell.window);
            shell.resize(kShotWidth, kShotHeight);
            shell.theme->setProperty("reduceMotion", true);
            shell.setAppearance(dark, "ultraviolet");
            auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
            auto* t = shell.engine.singletonInstance<TranscriptBridge*>("Wabcut", "Transcript");
            REQUIRE(t);
            t->setUseAnalysisCache(false);
            Timeline tl(R"(
                V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
                A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
            )");
            tl.linkMatching();
            controller->resetProject(tl.state, tl.seq);
            REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showBrowserPanel", Q_ARG(QVariant, QStringLiteral("captions"))));
            spin(300);
            REQUIRE_FALSE(shell.grab().isNull()); // lays the hidden window out

            // The set-up: with auto-detect the Mixed languages switch is there (off); naming a language or translating hides it.
            QQuickItem* mixed = anyNamed(shell, QStringLiteral("mixedLanguages"));
            REQUIRE(mixed != nullptr);
            CHECK(mixed->isVisible());
            CHECK_FALSE(mixed->property("checked").toBool());
            REQUIRE(QMetaObject::invokeMethod(mixed, "toggle"));
            REQUIRE(QMetaObject::invokeMethod(mixed, "toggled"));
            CHECK(t->mixedLanguages());
            t->setSpokenLanguage(QStringLiteral("th"));
            spin(100);
            CHECK_FALSE(mixed->isVisible());
            t->setSpokenLanguage(QStringLiteral("auto"));
            t->setTranslateToEnglish(true);
            spin(100);
            CHECK_FALSE(mixed->isVisible());
            t->setTranslateToEnglish(false);
            t->setProvider(QStringLiteral("local"));
            spin(150);
            CHECK(mixed->isVisible());
            QQuickItem* setup = anyNamed(shell, QStringLiteral("transcribeSetup"));
            REQUIRE(setup != nullptr);
            QQuickItem* setupContent = setup->property("contentItem").value<QQuickItem*>();
            if (setupContent) {
                const double most = std::max(0.0, setup->property("contentHeight").toDouble() - setup->height());
                setup->setProperty("contentY", std::clamp(mixed->mapToItem(setupContent, QPointF(0, 0)).y() - 120.0, 0.0, most));
                spin(100);
            }
            saveShot(shell.grab(), setup, dark ? QStringLiteral("transcript-mixed-setup.png") : QStringLiteral("transcript-mixed-setup-light.png"));

            // A transcript of mixed languages: a language tag per paragraph and the summary line.
            t->addTranscript(tl.media('A'), 0, mixedTranscript());
            spin(300);
            QQuickItem* summary = visibleNamed(shell, QStringLiteral("transcriptLanguages"));
            REQUIRE(summary != nullptr);
            CHECK(summary->property("text").toString() == QStringLiteral("Languages: English 75 % · Thai 25 %"));
            QQuickItem* second = visibleNamed(shell, QStringLiteral("transcriptLanguage_1"));
            REQUIRE(second != nullptr);
            CHECK(second->width() > 0);
            QQuickItem* panel = shell.item("transcriptPanel");
            REQUIRE(panel != nullptr);
            saveShot(shell.grab(), panel, dark ? QStringLiteral("transcript-mixed-languages.png") : QStringLiteral("transcript-mixed-languages-light.png"));

            const QStringList messages = takeMessages();
            INFO(join(messages));
            CHECK(messages.isEmpty());
            resetSettings();
        }
    }
}
