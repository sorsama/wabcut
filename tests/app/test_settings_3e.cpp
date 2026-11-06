// P1 round, stream 3E (offscreen):
//   * #47 Interface size: 90 %, sizes too large for the screen greyed with a tooltip, Restart now through the normal
//     close path (the unsaved-changes prompt; Cancel calls the restart off), the restarted process's arguments;
//   * #48 Media & cache: proxies next to the project or in one folder (at once), the media cache and AI folders (at the
//     next start: Restart now), free space on each drive (the low-space warnings' lookup), the settings search.
// Screenshots: p1-interface-size.png / -light.png, p1-media-cache-folders.png / -light.png.

#include "A11yAudit.h"
#include "ShellFixture.h"

#include "AppSettingsBridge.h"
#include "AppShell.h"
#include "CacheFolders.h"
#include "DiskSpace.h"
#include "MediaReadinessBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "UiScale.h"

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>

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

QVariant evaluate(const Shell& shell, const QString& code) {
    QQmlExpression expression(qmlContext(shell.window.get()), shell.window.get(), code);
    const QVariant result = expression.evaluate();
    INFO(code.toStdString() << ": " << expression.error().toString().toStdString());
    REQUIRE_FALSE(expression.hasError());
    return result;
}

QObject* openSettingsPage(Shell& shell, const QString& page) {
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "openSettings", Q_ARG(QString, page), Q_ARG(QString, QString())));
    QObject* loader = shell.object("settingsWindowLoader");
    REQUIRE(loader);
    QObject* window = loader->property("item").value<QObject*>();
    REQUIRE(window);
    REQUIRE(waitFor([&] { return window->property("opened").toBool(); }));
    spin(50);
    return window;
}

QQuickItem* popupItem(QObject* popup) {
    auto* content = popup ? popup->property("contentItem").value<QQuickItem*>() : nullptr;
    return content ? content->parentItem() : nullptr;
}

void saveCrop(const QImage& image, const QQuickItem* item, const QString& name) {
    const double dpr = image.devicePixelRatio();
    const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()))
                         .adjusted(-16, -16, 16, 16)
                         .intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    REQUIRE(image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect())
                .save(QDir(outputDir()).filePath(name)));
}

QString native(const QString& path) { return QDir::toNativeSeparators(QDir::cleanPath(path)); }

void checkNoMessages() {
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

} // namespace

// ---- #47 --------------------------------------------------------------------------------------------------------

TEST_CASE("3E Interface size: 90 percent and the sizes that fit the screen", "[app][a11y][3e]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    QSettings settings(dir.filePath(QStringLiteral("s.ini")), QSettings::IniFormat);
    settings.setValue(QStringLiteral("appearance/uiScale"), 90);
    CHECK(savedUiScale(settings) == 90);
    settings.setValue(QStringLiteral("appearance/uiScale"), 80);
    CHECK(savedUiScale(settings) == 100);
    CHECK(kUiScales.front() == 90);

    // A 1080p screen above its taskbar (1920 x 1040) at 100 %: 150 % leaves 1280 x 693, under the 700 px minimum.
    CHECK(uiScaleFits(QSize(1920, 1040), 100, 100));
    CHECK(uiScaleFits(QSize(1920, 1040), 100, 125));
    CHECK_FALSE(uiScaleFits(QSize(1920, 1040), 100, 150));
    // The same screen measured while running at 150 % (1280 x 693): 100 % fits again.
    CHECK(uiScaleFits(QSize(1280, 693), 150, 100));
    // A 1080p laptop at 150 % Windows scaling (1280 x 680): only 90 % holds the minimum.
    CHECK_FALSE(uiScaleFits(QSize(1280, 680), 100, 100));
    CHECK(uiScaleFits(QSize(1280, 680), 100, 90));
    CHECK(uiScaleFits(QSize(), 100, 150)); // nothing known: offered

    // Restart now: the new process keeps the options, drops the project it was started with, opens the one open now.
    const QString project = QDir(dir.path()).filePath(QStringLiteral("Film.wabcut"));
    const QStringList args = AppShell::restartArguments(
        {QStringLiteral("wabcut"), QStringLiteral("--rhi"), QStringLiteral("d3d12"), QStringLiteral("old.wabcut"),
         QStringLiteral("--new-instance")},
        project);
    CHECK(args == QStringList{QStringLiteral("--rhi"), QStringLiteral("d3d12"), QStringLiteral("--new-instance"),
                              QFileInfo(project).absoluteFilePath()});
    CHECK(AppShell::restartArguments({QStringLiteral("wabcut"), QStringLiteral("Old.WABCUT")}, {}).isEmpty());
    // 0.7 (card 005): a recording the app was started with ("Open with") was imported then, not again after a restart.
    CHECK(AppShell::restartArguments({QStringLiteral("wabcut"), QStringLiteral("D:/Rec/Take 1.mp4"), QStringLiteral("--rhi"),
                                      QStringLiteral("d3d11")},
                                     {}) == QStringList{QStringLiteral("--rhi"), QStringLiteral("d3d11")});
    AppShell shell;
    CHECK_FALSE(AppShell::restartPending());
    shell.armRestart(QUrl::fromLocalFile(project).toString());
    CHECK(AppShell::restartPending());
    shell.cancelRestart();
    CHECK_FALSE(AppShell::restartPending());
    CHECK_FALSE(AppShell::launchPendingRestart()); // nothing armed: nothing starts
}

TEST_CASE("3E Interface size: Settings offers 90 percent greys sizes too large for the screen and Restart now asks first",
          "[app][a11y][qml][3e][screenshot]") {
    resetSettings();
    clearMessages();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    // The window's minimum is cut to the screen's work area.
    const QSize room = availableScreenSize(shell.window.get());
    if (!room.isEmpty()) {
        CHECK(shell.window->minimumWidth() == std::min(kWindowMinimum.width(), room.width()));
        CHECK(shell.window->minimumHeight() == std::min(kWindowMinimum.height(), room.height()));
    }

    QObject* settings = openSettingsPage(shell, QStringLiteral("appearance"));
    QQuickItem* root = settings->property("contentItem").value<QQuickItem*>();
    QQuickItem* tabs = findNamed(root, QStringLiteral("uiScaleTabs"));
    QQuickItem* row = findNamed(root, QStringLiteral("uiScaleRow"));
    QQuickItem* restart = findNamed(root, QStringLiteral("uiScaleRestart"));
    REQUIRE(tabs);
    REQUIRE(row);
    REQUIRE(restart);
    const QVariantList model = tabs->property("model").toList();
    REQUIRE(model.size() == 4);
    CHECK(model[0].toMap().value(QStringLiteral("label")).toString() == QStringLiteral("90%"));
    CHECK(tabs->property("currentIndex").toInt() == 1); // 100 %
    // 90 % and the size in use are always offered; the others when the screen holds the window at that size.
    const int sizes[] = {90, 100, 125, 150};
    for (int i = 0; i < 4; ++i) {
        const QVariantMap entry = model[i].toMap();
        const bool expected = sizes[i] <= 100 || uiScaleFits(room, 100, sizes[i]);
        INFO(sizes[i] << " % on " << room.width() << " x " << room.height());
        CHECK(entry.value(QStringLiteral("enabled")).toBool() == expected);
        CHECK((entry.value(QStringLiteral("tooltip")).toString() == QStringLiteral("Too large for this screen")) == !expected);
    }
    CHECK_FALSE(restart->isVisible());

    // 90 %: stored, and Restart now shows.
    REQUIRE(QMetaObject::invokeMethod(tabs, "activated", Q_ARG(int, 0)));
    CHECK(shell.preferences->property("uiScale").toInt() == 90);
    REQUIRE(waitFor([&] { return restart->isVisible(); }));
    CHECK(row->property("description").toString().contains(QStringLiteral("restarts")));
    // A disabled size cannot be chosen with the keys either (End stops at the last size that fits).
    tabs->forceActiveFocus(Qt::TabFocusReason);
    QKeyEvent end(QEvent::KeyPress, Qt::Key_End, Qt::NoModifier);
    QCoreApplication::sendEvent(shell.window.get(), &end);
    const int last = tabs->property("currentIndex").toInt();
    CHECK(model[last].toMap().value(QStringLiteral("enabled")).toBool());
    REQUIRE(QMetaObject::invokeMethod(tabs, "activated", Q_ARG(int, 0)));
    tabs->setProperty("currentIndex", 0);
    shell.preferences->setProperty("uiScale", 90);
    spin(50);

    for (const bool dark : {true, false}) {
        shell.setAppearance(dark, "ultraviolet");
        spin(150);
        saveCrop(shell.grab(), row, dark ? QStringLiteral("p1-interface-size.png") : QStringLiteral("p1-interface-size-light.png"));
    }
    shell.setAppearance(true, "ultraviolet");

    // Restart now with unsaved changes: "Save changes?" first; Cancel keeps Wabcut open and calls the restart off.
    auto* file = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    REQUIRE(file);
    REQUIRE(file->saveAs(QUrl::fromLocalFile(QDir(dir.path()).filePath(QStringLiteral("Film.wabcut")))));
    evaluate(shell, QStringLiteral("Markers.add(false)"));
    REQUIRE(file->dirty());
    QObject* unsaved = shell.object("unsavedChangesDialog");
    REQUIRE(unsaved);
    REQUIRE(QMetaObject::invokeMethod(restart, "clicked"));
    REQUIRE(waitFor([&] { return unsaved->property("visible").toBool(); }));
    CHECK(AppShell::restartPending());
    REQUIRE(QMetaObject::invokeMethod(unsaved, "close"));
    REQUIRE(waitFor([&] { return !AppShell::restartPending(); }, 3000));
    CHECK_FALSE(evaluate(shell, QStringLiteral("window.restartQuitArmed")).toBool());
    checkNoMessages();
    resetSettings();
}

// ---- #48 --------------------------------------------------------------------------------------------------------

TEST_CASE("3E Media and cache: proxies in a chosen folder and the cache folders with free space", "[app][settings][qml][3e][screenshot]") {
    resetSettings();
    clearMessages();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    diskspace::setFreeBytesProviderForTests([](const QString&) { return qint64(412'000'000'000); });
    const QString root = QDir(dir.path()).filePath(QStringLiteral("fast drive"));
    const QString media = QDir(dir.path()).filePath(QStringLiteral("media cache"));
    const QString aiFolder = QDir(dir.path()).filePath(QStringLiteral("ai"));
    {
        Shell shell;
        REQUIRE(shell.window);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        auto* readiness = shell.engine.singletonInstance<MediaReadinessBridge*>("Wabcut", "MediaReadiness");
        auto* file = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
        REQUIRE(readiness);
        REQUIRE(file);
        REQUIRE(file->saveAs(QUrl::fromLocalFile(QDir(dir.path()).filePath(QStringLiteral("Film.wabcut")))));
        spin(50);

        // Next to the project by default.
        CHECK(readiness->proxyLocation() == QStringLiteral("project"));
        CHECK(readiness->proxyFolder() == native(QDir(dir.path()).filePath(QStringLiteral("Film Proxies"))));
        CHECK(readiness->proxyFreeText().startsWith(QStringLiteral("412 GB free on")));

        // In this folder: at once, kept in the settings.
        int changes = 0;
        QObject::connect(readiness, &MediaReadinessBridge::locationsChanged, [&] { ++changes; });
        readiness->setProxyRootFolder(QUrl::fromLocalFile(root).toString());
        readiness->setProxyLocation(QStringLiteral("folder"));
        CHECK(changes >= 2);
        CHECK(readiness->proxyLocation() == QStringLiteral("folder"));
        CHECK(readiness->proxyRootFolder() == native(root));
        CHECK(readiness->proxyFolder() == native(QDir(root).filePath(QStringLiteral("Film Proxies"))));
        CHECK(QSettings().value(QStringLiteral("proxies/location")).toString() == QStringLiteral("folder"));
        CHECK(cachefolders::proxyRoot() == QDir::cleanPath(root));
        CHECK_FALSE(readiness->restartNeeded()); // proxies need no restart

        // The media cache and AI folders: kept, used from the next start (Restart now).
        const QString inUse = readiness->mediaCacheFolderInUse();
        CHECK(readiness->mediaCacheFolder().isEmpty());
        CHECK(readiness->defaultMediaCacheFolder().endsWith(QStringLiteral("media")));
        readiness->setMediaCacheFolder(QUrl::fromLocalFile(media).toString());
        CHECK(readiness->mediaCacheFolder() == native(media));
        CHECK(readiness->mediaCacheFolderInUse() == inUse);
        CHECK(readiness->restartNeeded());
        CHECK(readiness->mediaCacheFreeText().startsWith(QStringLiteral("412 GB free on")));
        readiness->setAiCacheFolder(aiFolder);
        CHECK(QSettings().value(QStringLiteral("cache/aiFolder")).toString() == QDir::cleanPath(aiFolder));
        CHECK(readiness->aiCacheFreeText().startsWith(QStringLiteral("412 GB free on")));

        // The dialog: the location tabs, the folder rows with their free space, every control named.
        A11yScope a11y;
        REQUIRE(QMetaObject::invokeMethod(readiness, "openSettings"));
        QObject* loader = shell.object("mediaCacheSettingsLoader");
        REQUIRE(loader);
        QObject* dialog = nullptr;
        REQUIRE(waitFor([&] {
            dialog = loader->property("item").value<QObject*>();
            return dialog && dialog->property("opened").toBool();
        }));
        spin(100);
        QQuickItem* box = popupItem(dialog);
        REQUIRE(box);
        // #61: the dialog's own element is named (NVDA announces it when the focus enters).
        {
            QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(box);
            REQUIRE(iface);
            INFO("role 0x" << std::hex << static_cast<int>(iface->role()));
            CHECK(iface->text(QAccessible::Name) == QStringLiteral("Media & cache"));
        }
        // The sections scroll: the footer stays inside the window, also with the new rows.
        QQuickItem* done = findNamed(box, QStringLiteral("mediaCacheDone"));
        REQUIRE(done);
        REQUIRE_FALSE(shell.grab().isNull()); // a frame: the layouts take the window's size when one is drawn
        spin(50);
        CHECK(done->mapToScene(QPointF(0, done->height())).y() <= shell.window->height());
        QQuickItem* tabs = findNamed(box, QStringLiteral("proxyLocationTabs"));
        REQUIRE(tabs);
        CHECK(tabs->property("currentIndex").toInt() == 1);
        QQuickItem* choose = findNamed(box, QStringLiteral("proxyFolderChoose"));
        REQUIRE(choose);
        CHECK(choose->isVisible());
        QQuickItem* mediaPath = findNamed(box, QStringLiteral("folderPath_media"));
        REQUIRE(mediaPath);
        CHECK(mediaPath->property("text").toString() == native(media));
        QQuickItem* aiPath = findNamed(box, QStringLiteral("folderPath_ai"));
        REQUIRE(aiPath);
        CHECK(aiPath->property("text").toString() == native(aiFolder));
        QQuickItem* free = findNamed(box, QStringLiteral("folderFree_media"));
        REQUIRE(free);
        CHECK(free->property("text").toString().startsWith(QStringLiteral("412 GB free")));
        // QA (fix round 1, finding 3): the notes under a folder's label are secondary text (6.9:1 on the light card),
        // not the small tertiary shade that measured 3.3:1 there.
        CHECK(mediaPath->property("color").value<QColor>() == shell.themeColor("textSecondary"));
        CHECK(free->property("color").value<QColor>() == shell.themeColor("textSecondary"));
        QQuickItem* restart = findNamed(box, QStringLiteral("cacheFoldersRestart"));
        REQUIRE(restart);
        CHECK(restart->isVisible());
        REQUIRE(waitFor([&] { return !readiness->cacheBusy() && !readiness->caches().isEmpty(); }));
        spin(50);
        bool cacheFree = false;
        for (const QVariant& c : readiness->caches())
            cacheFree = cacheFree || c.toMap().value(QStringLiteral("freeText")).toString().startsWith(QStringLiteral("412 GB"));
        CHECK(cacheFree);
        {
            const QStringList issues = auditAccessibility(box);
            INFO(join(issues));
            CHECK(issues.isEmpty());
        }
        for (const bool dark : {true, false}) {
            shell.setAppearance(dark, "ultraviolet");
            spin(150);
            saveCrop(shell.grab(), box,
                     dark ? QStringLiteral("p1-media-cache-folders.png") : QStringLiteral("p1-media-cache-folders-light.png"));
        }
        shell.setAppearance(true, "ultraviolet");

        // Default goes back; Next to the project too.
        QQuickItem* toDefault = findNamed(box, QStringLiteral("folderDefault_media"));
        REQUIRE(toDefault);
        REQUIRE(QMetaObject::invokeMethod(toDefault, "clicked"));
        CHECK(readiness->mediaCacheFolder().isEmpty());
        REQUIRE(QMetaObject::invokeMethod(tabs, "activated", Q_ARG(int, 0)));
        CHECK(readiness->proxyLocation() == QStringLiteral("project"));
        CHECK(readiness->proxyFolder() == native(QDir(dir.path()).filePath(QStringLiteral("Film Proxies"))));
        CHECK(readiness->proxyRootFolder() == native(root)); // remembered for next time
        REQUIRE(QMetaObject::invokeMethod(dialog, "close"));
        checkNoMessages();
    }

    // The editor's media cache comes from the setting at start.
    QSettings().setValue(QStringLiteral("cache/mediaFolder"), media);
    QSettings().sync();
    {
        ProjectController controller;
        CHECK(QDir::cleanPath(controller.mediaCacheDirectory()) == QDir::cleanPath(media));
    }
    // The AI folder reaches WABCUT_CACHE_DIR at launch, unless the environment set one.
    {
        const bool had = qEnvironmentVariableIsSet("WABCUT_CACHE_DIR");
        const QByteArray before = qgetenv("WABCUT_CACHE_DIR");
        cachefolders::setChosenAiCacheFolder(aiFolder);
        qputenv("WABCUT_CACHE_DIR", "elsewhere");
        CHECK_FALSE(cachefolders::applyChosenAiCacheFolder());
        CHECK(qgetenv("WABCUT_CACHE_DIR") == QByteArray("elsewhere"));
        qunsetenv("WABCUT_CACHE_DIR");
        CHECK(cachefolders::applyChosenAiCacheFolder());
        CHECK(QDir::cleanPath(qEnvironmentVariable("WABCUT_CACHE_DIR")) == QDir::cleanPath(aiFolder));
        if (had)
            qputenv("WABCUT_CACHE_DIR", before);
        else
            qunsetenv("WABCUT_CACHE_DIR");
    }
    // Settings search finds the new rows (they open Media & cache).
    {
        AppSettingsBridge search;
        for (const char* query : {"media cache folder", "proxy folder", "ai models folder"}) {
            const QVariantList hits = search.search(QString::fromLatin1(query));
            INFO(query);
            REQUIRE_FALSE(hits.isEmpty());
            CHECK(hits.front().toMap().value(QStringLiteral("opens")).toString() == QStringLiteral("media"));
        }
    }
    diskspace::setFreeBytesProviderForTests({});
    resetSettings();
}
