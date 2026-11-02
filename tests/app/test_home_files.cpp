// 0.6 (P1 round, stream 2D): Home.
//   * #49 the project menu's Rename…, Duplicate…, Versions… and Move to Recycle Bin…: the project's sidecars
//     (autosaves, backups, "<name> Preview Files") follow a rename and go to the recycle bin with it, an open project or
//     one another live session holds is refused, Undo brings the files back, a duplicate is read and saved again; a
//     project that fails to open lists its own versions (ProjectFile.scanRecoveryVersions(file)) and offers "Recover a
//     version…". These tests never touch the real recycle bin: HomeBridge::setTrasher moves files into a test folder.
//   * #63 Home's Import folder…: the folder's files go into the folder's bin, subfolders nested, in one undo step;
//     hidden files are left out as File > Import Folder leaves them out.
// Screenshots (dark and light): the project menu, the Rename dialog, the Versions dashboard of a broken project.

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "BridgeIds.h"
#include "HomeBridge.h"
#include "LibraryBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJSValue>
#include <QQuickItem>
#include <QScopeGuard>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <filesystem>
#include <optional>
#include <string>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

/// Home over a real project file service (no window).
struct Fixture {
    ProjectController controller;
    ProjectLifecycle lifecycle{&controller};
    HomeBridge home;
    QTemporaryDir dir;

    Fixture() {
        resetSettings();
        home.setController(&controller);
        home.setLifecycle(&lifecycle);
        home.refreshRecent();
    }
    QString path(const QString& name) const { return dir.filePath(name); }
};

/// Moves what goes to the "recycle bin" into a folder of the test instead (never the real one) for its lifetime.
struct TestTrash {
    QString folder;
    int moved = 0;
    explicit TestTrash(const QString& root) : folder(QDir(root).filePath(QStringLiteral("_trash"))) {
        QDir().mkpath(folder);
        HomeBridge::setTrasher([this](const QString& path, QString* inTrash) {
            const QString target = QDir(folder).filePath(QStringLiteral("%1_%2").arg(++moved).arg(QFileInfo(path).fileName()));
            if (!QDir().rename(path, target))
                return false;
            if (inTrash)
                *inTrash = target;
            return true;
        });
    }
    ~TestTrash() { HomeBridge::setTrasher({}); }
    QStringList contents() const { return QDir(folder).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden); }
};

void writeFile(const QString& path, const QByteArray& data) {
    REQUIRE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(data);
}

/// A small valid project saved at `path` (not opened).
void saveProjectAt(const QString& path) {
    Timeline tl(R"(
        V1 tgt |AaaaBbbb|
    )");
    REQUIRE(io::saveProject(tl.state, std::filesystem::path(path.toStdU16String()), io::SaveOptions{0}));
}

QStringList recentPaths(const HomeBridge& home) {
    QStringList out;
    for (const QVariant& e : home.recent())
        out << QDir::fromNativeSeparators(e.toMap().value(QStringLiteral("path")).toString());
    return out;
}

QSet<QString> names(const QStringList& paths) {
    QSet<QString> out;
    for (const QString& p : paths)
        out.insert(QFileInfo(p).fileName());
    return out;
}

std::filesystem::path libraryTestMedia(const char* name) { return std::filesystem::path(WABCUT_TEST_MEDIA_DIR) / name; }

bool haveTestMedia() {
    return std::filesystem::is_regular_file(libraryTestMedia("h264_1080p60_aac.mp4")) &&
           std::filesystem::is_regular_file(libraryTestMedia("sine_48k.wav"));
}

void copyMedia(const char* name, const QString& to) {
    REQUIRE(QDir().mkpath(QFileInfo(to).absolutePath()));
    QFile::remove(to);
    REQUIRE(QFile::copy(QString::fromStdU16String(libraryTestMedia(name).u16string()), to));
}

/// Like findNamed, hidden items included.
QQuickItem* findAnyNamed(QQuickItem* root, const QString& name) {
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findAnyNamed(child, name))
            return found;
    }
    return nullptr;
}

QQuickItem* findNamed(QQuickItem* root, const QString& name) {
    if (!root)
        return nullptr;
    if (root->objectName() == name && root->isVisible())
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findNamed(child, name))
            return found;
    }
    return nullptr;
}

QQuickItem* popupOf(QQuickItem* item) {
    QQuickItem* at = item;
    while (at && at->parentItem() && !QByteArray(at->parentItem()->metaObject()->className()).contains("Overlay"))
        at = at->parentItem();
    return at;
}

void saveItemShot(const QImage& image, const QQuickItem* item, const QString& name) {
    const QRectF rect = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()))
                            .adjusted(-8, -8, 8, 8)
                            .intersected(QRectF(0, 0, kShotWidth, kShotHeight));
    const double dpr = image.devicePixelRatio();
    REQUIRE(image.copy(QRect(static_cast<int>(rect.x() * dpr), static_cast<int>(rect.y() * dpr),
                             static_cast<int>(rect.width() * dpr), static_cast<int>(rect.height() * dpr)))
                .save(QDir(outputDir()).filePath(name)));
}

} // namespace

// ---- #49 ------------------------------------------------------------------------------------------------------------

TEST_CASE("Home P1: Rename and Move to Recycle Bin take the sidecars and refuse an open project", "[app][home][p1]") {
    Fixture fx;
    TestTrash trash(fx.dir.path());
    const QString trailer = fx.path(QStringLiteral("Trailer.wabcut"));
    saveProjectAt(trailer);
    for (const char* suffix : {".autosave", ".autosave2", ".bak1", ".bak2"})
        writeFile(trailer + QString::fromLatin1(suffix), "{}");
    writeFile(fx.path(QStringLiteral("Trailer Preview Files/render 1.mov")), "frames");
    writeFile(trailer + QStringLiteral(".txt"), "not a sidecar");
    writeFile(fx.path(QStringLiteral("Trailer2.wabcut.bak1")), "another project's");
    writeFile(fx.path(QStringLiteral("Other.wabcut")), "{}");
    fx.home.rememberProject(QUrl::fromLocalFile(trailer));
    const QUrl url = QUrl::fromLocalFile(trailer);

    CHECK(names(HomeBridge::sidecarsOf(trailer)) ==
          QSet<QString>({QStringLiteral("Trailer.wabcut.autosave"), QStringLiteral("Trailer.wabcut.autosave2"),
                         QStringLiteral("Trailer.wabcut.bak1"), QStringLiteral("Trailer.wabcut.bak2"),
                         QStringLiteral("Trailer Preview Files")}));
    CHECK(fx.home.projectBlocker(url).isEmpty());
    CHECK(fx.home.validateRename(url, QStringLiteral("Trailer")) == QStringLiteral("Type a new name"));
    CHECK(fx.home.validateRename(url, QStringLiteral("a/b")).contains(QStringLiteral("cannot contain")));
    CHECK(fx.home.validateRename(url, QStringLiteral("Other")).contains(QStringLiteral("already exists")));
    CHECK(fx.home.validateRename(url, QStringLiteral("Final Cut")).isEmpty());

    // Rename: the project, its autosaves, backups and previews take the new name; the rest stays; the entry follows.
    const QVariantMap renamed = fx.home.renameProject(url, QStringLiteral("Final Cut"));
    INFO(renamed.value("error").toString().toStdString());
    REQUIRE(renamed.value("ok").toBool());
    CHECK(renamed.value("warning").toString().isEmpty());
    const QString finalPath = fx.path(QStringLiteral("Final Cut.wabcut"));
    CHECK(QFileInfo::exists(finalPath));
    CHECK_FALSE(QFileInfo::exists(trailer));
    for (const char* suffix : {".autosave", ".autosave2", ".bak1", ".bak2"}) {
        INFO(suffix);
        CHECK(QFileInfo::exists(finalPath + QString::fromLatin1(suffix)));
        CHECK_FALSE(QFileInfo::exists(trailer + QString::fromLatin1(suffix)));
    }
    CHECK(QFileInfo::exists(fx.path(QStringLiteral("Final Cut Preview Files/render 1.mov"))));
    CHECK(QFileInfo::exists(trailer + QStringLiteral(".txt")));
    CHECK(QFileInfo::exists(fx.path(QStringLiteral("Trailer2.wabcut.bak1"))));
    CHECK(recentPaths(fx.home).contains(QDir::fromNativeSeparators(QFileInfo(finalPath).absoluteFilePath())));
    CHECK_FALSE(recentPaths(fx.home).contains(QDir::fromNativeSeparators(QFileInfo(trailer).absoluteFilePath())));
    CHECK(renamed.value("oldPath").toString() == QDir::toNativeSeparators(QFileInfo(trailer).absoluteFilePath()));
    const QUrl finalUrl = QUrl::fromLocalFile(finalPath);

    // Move to Recycle Bin: the project and its five sidecars go (into the test's "bin"); the entry leaves the list.
    const QVariantMap trashed = fx.home.trashProject(finalUrl);
    INFO(trashed.value("error").toString().toStdString());
    REQUIRE(trashed.value("ok").toBool());
    CHECK(trashed.value("moved").toInt() == 6);
    CHECK_FALSE(QFileInfo::exists(finalPath));
    CHECK_FALSE(QFileInfo::exists(fx.path(QStringLiteral("Final Cut Preview Files"))));
    CHECK(trash.contents().size() == 6);
    CHECK_FALSE(recentPaths(fx.home).contains(QDir::fromNativeSeparators(QFileInfo(finalPath).absoluteFilePath())));
    // Undo brings every file back where it was, and the entry.
    REQUIRE(fx.home.undoTrash());
    CHECK(QFileInfo::exists(finalPath));
    CHECK(QFileInfo::exists(finalPath + QStringLiteral(".bak2")));
    CHECK(QFileInfo::exists(fx.path(QStringLiteral("Final Cut Preview Files/render 1.mov"))));
    CHECK(trash.contents().isEmpty());
    CHECK(recentPaths(fx.home).contains(QDir::fromNativeSeparators(QFileInfo(finalPath).absoluteFilePath())));
    CHECK_FALSE(fx.home.undoTrash()); // once

    // Duplicate: read and saved again next to it, without the sidecars; listed first.
    CHECK(fx.home.duplicateName(finalUrl) == QStringLiteral("Final Cut copy"));
    const QVariantMap copy = fx.home.duplicateProject(finalUrl, QStringLiteral("Final Cut copy"));
    INFO(copy.value("error").toString().toStdString());
    REQUIRE(copy.value("ok").toBool());
    const QString copyPath = fx.path(QStringLiteral("Final Cut copy.wabcut"));
    CHECK(io::loadProject(std::filesystem::path(copyPath.toStdU16String())).has_value());
    CHECK(HomeBridge::sidecarsOf(copyPath).isEmpty());
    CHECK(recentPaths(fx.home).front() == QDir::fromNativeSeparators(QFileInfo(copyPath).absoluteFilePath()));
    CHECK(fx.home.duplicateName(finalUrl) == QStringLiteral("Final Cut copy 2"));

    // An open project is refused (and nothing moves); so is one another live session holds.
    REQUIRE(fx.lifecycle.open(finalUrl));
    CHECK(fx.home.projectBlocker(finalUrl).contains(QStringLiteral("is open")));
    CHECK_FALSE(fx.home.trashProject(finalUrl).value("ok").toBool());
    CHECK_FALSE(fx.home.renameProject(finalUrl, QStringLiteral("Elsewhere")).value("ok").toBool());
    CHECK(QFileInfo::exists(finalPath));
    CHECK(trash.contents().isEmpty());
    fx.lifecycle.newProject();
    CHECK(fx.home.projectBlocker(finalUrl).isEmpty());
    writeFile(copyPath + QStringLiteral(".lock"),
              R"({"format": "wabcut-project-lock", "user": "sam", "host": "edit-bay-2", "pid": 4242, "since": "2026-11-01T10:00:00Z"})");
    const QUrl copyUrl = QUrl::fromLocalFile(copyPath);
    CHECK(fx.home.projectBlocker(copyUrl).contains(QStringLiteral("another Wabcut")));
    CHECK_FALSE(fx.home.trashProject(copyUrl).value("ok").toBool());
    CHECK(QFileInfo::exists(copyPath));
    // A missing project cannot be renamed or trashed (Locate first).
    CHECK(fx.home.projectBlocker(QUrl::fromLocalFile(fx.path(QStringLiteral("Gone.wabcut")))).contains(QStringLiteral("missing")));
}

TEST_CASE("Home P1: a project that fails to open lists its own versions", "[app][home][p1][recovery]") {
    Fixture fx;
    const QString broken = fx.path(QStringLiteral("Broken.wabcut"));
    writeFile(broken, "{ this is not a project");
    const QUrl url = QUrl::fromLocalFile(broken);
    CHECK_FALSE(fx.lifecycle.hasRecoveryVersions(url));
    saveProjectAt(broken + QStringLiteral(".bak1"));
    CHECK(fx.lifecycle.hasRecoveryVersions(url));
    CHECK_FALSE(fx.lifecycle.open(url));
    CHECK_FALSE(fx.lifecycle.lastError().isEmpty());
    // An untitled session's autosave (another project's unsaved work) is not a version of this project (fix round 1,
    // ARGON-10). The test's own app data folder (test mode).
    const QString untitledDir =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/Autosave");
    REQUIRE(QDir().mkpath(untitledDir));
    const QString untitled = untitledDir + QStringLiteral("/Untitled-fix1c.wabcut.autosave");
    saveProjectAt(untitled);
    const auto removeUntitled = qScopeGuard([&] { QFile::remove(untitled); });

    fx.lifecycle.scanRecoveryVersions(url);
    REQUIRE(waitFor([&] { return !fx.lifecycle.recoveryVersions().isEmpty(); }, 20000));
    bool backup = false;
    bool current = false;
    QUrl backupUrl;
    for (const QVariant& v : fx.lifecycle.recoveryVersions()) {
        const QVariantMap version = v.toMap();
        if (version.value("kind").toString() == QStringLiteral("backup")) {
            backup = version.value("loadable").toBool();
            backupUrl = version.value("url").toUrl();
            // The saved file cannot be read: no "+1 sequence · +2 clips" against it as if it held nothing (ARGON-10).
            CHECK(version.value("comparison").toString().isEmpty());
        }
        CHECK(version.value("kind").toString() != QStringLiteral("untitled"));
        if (version.value("kind").toString() == QStringLiteral("current"))
            current = !version.value("loadable").toBool(); // the broken file, listed as it is
    }
    CHECK(backup);
    CHECK(current);
    CHECK(waitFor([&] { return !fx.lifecycle.recoveryScanning(); }, 30000));
    // Open as copy works without the broken file opening: an untitled copy of the backup.
    REQUIRE(fx.lifecycle.openRecoveryCopy(backupUrl));
    CHECK(fx.lifecycle.filePath().isEmpty());
    CHECK(fx.controller.snapshot()->sequences.size() == 1);
}

// ---- #63 ------------------------------------------------------------------------------------------------------------

TEST_CASE("Home P1: Import folder from Home keeps subfolders as bins in one undo step", "[app][home][p1][import]") {
    if (!haveTestMedia())
        SKIP("test media missing");
    Fixture fx;
    const QString session = fx.path(QStringLiteral("Session1"));
    copyMedia("h264_1080p60_aac.mp4", session + QStringLiteral("/a.mp4"));
    copyMedia("sine_48k.wav", session + QStringLiteral("/Cam B/b.wav"));
    copyMedia("sine_48k.wav", session + QStringLiteral("/.cache/c.wav"));
    copyMedia("sine_48k.wav", session + QStringLiteral("/.d.wav"));
    writeFile(session + QStringLiteral("/notes.txt"), "not media");

    const QVariantMap c = fx.home.classifyImport({QUrl::fromLocalFile(session)});
    REQUIRE(c.value("route").toString() == QStringLiteral("media"));
    QStringList media;
    QList<QUrl> files;
    for (const QVariant& v : c.value("media").toList()) {
        media << v.toMap().value("name").toString();
        files << v.toMap().value("url").toUrl();
    }
    CHECK(media == QStringList({QStringLiteral("a.mp4"), QStringLiteral("b.wav")})); // hidden ones left out
    const QVariantList folders = c.value("folderUrls").toList();
    REQUIRE(folders.size() == 1);
    CHECK(QFileInfo(folders.front().toUrl().toLocalFile()).fileName() == QStringLiteral("Session1"));
    CHECK(c.value("notes").toStringList().join(QLatin1Char('\n')).contains(QStringLiteral("hidden")));

    const QVariantMap result = fx.home.createFromMediaWith(
        QStringLiteral("Session"), QUrl::fromLocalFile(fx.path(QStringLiteral("Projects"))), {}, files,
        {{QStringLiteral("folders"), folders}, {QStringLiteral("proxies"), QStringLiteral("off")}},
        [](const QStringList&) { return 0; });
    INFO(result.value("error").toString().toStdString() << " / " << result.value("warning").toString().toStdString());
    REQUIRE(result.value("ok").toBool());
    CHECK(result.value("added").toInt() == 2);
    CHECK(result.value("placed").toInt() == 2);

    const ProjectSnapshot snap = fx.controller.snapshot();
    const Bin* folderBin = nullptr;
    const Bin* camBin = nullptr;
    for (const Bin& b : snap->bins) {
        if (b.name == "Session1")
            folderBin = &b;
        if (b.name == "Cam B")
            camBin = &b;
    }
    REQUIRE(folderBin);
    REQUIRE(camBin);
    CHECK_FALSE(folderBin->parent.isValid()); // at the new project's root
    CHECK(camBin->parent == folderBin->id);
    for (const MediaPtr& m : snap->media) {
        INFO(m->name);
        CHECK(m->bin == (m->name == "a.mp4" ? folderBin->id : camBin->id));
    }
    // One undo step made the items and both bins.
    const std::vector<std::string> labels = fx.controller.session().undoLabels();
    CHECK(std::count(labels.begin(), labels.end(), std::string("Import Folder")) == 1);
    while (!fx.controller.snapshot()->media.empty() && fx.controller.session().undoLabel() != std::optional<std::string>("Import Folder"))
        REQUIRE(fx.controller.undo());
    REQUIRE(fx.controller.undo());
    CHECK(fx.controller.snapshot()->media.empty());
    CHECK(fx.controller.snapshot()->bins.empty());
}

// ---- In the window ----------------------------------------------------------------------------------------------

TEST_CASE("Home P1: the project menu renames and lists versions and a failed open offers to recover", "[app][home][p1][qml][screenshots]") {
    for (const bool dark : {true, false}) {
        resetSettings();
        clearMessages();
        QTemporaryDir dir;
        REQUIRE(dir.isValid());
        TestTrash trash(dir.path());
        Shell shell;
        REQUIRE(shell.window != nullptr);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        if (!dark) {
            shell.setAppearance(false, "ultraviolet");
            REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
        }
        auto* home = shell.engine.singletonInstance<HomeBridge*>("Wabcut", "Home");
        REQUIRE(home);
        const QString trailer = dir.filePath(QStringLiteral("Trailer.wabcut"));
        saveProjectAt(trailer);
        saveProjectAt(trailer + QStringLiteral(".bak1"));
        home->rememberProject(QUrl::fromLocalFile(trailer));
        QQuickItem* screen = shell.item("homeScreen");
        REQUIRE(screen);
        REQUIRE(QMetaObject::invokeMethod(screen, "open"));
        spin(100);
        REQUIRE_FALSE(shell.grab().isNull());

        // The menu of Trailer: the new rows, enabled (it is not open).
        QVariantMap entry;
        for (const QVariant& v : home->recent()) {
            if (v.toMap().value("name").toString() == QStringLiteral("Trailer"))
                entry = v.toMap();
        }
        REQUIRE_FALSE(entry.isEmpty());
        REQUIRE(QMetaObject::invokeMethod(screen, "showProjectMenu", Q_ARG(QVariant, QVariant(entry)),
                                          Q_ARG(QQuickItem*, screen), Q_ARG(double, 400.0), Q_ARG(double, 200.0)));
        QObject* menu = screen->findChild<QObject*>(QStringLiteral("homeProjectMenu"));
        REQUIRE(menu);
        REQUIRE(waitFor([&] { return menu->property("opened").toBool(); }, 3000));
        spin(150);
        QQuickItem* rename = findNamed(shell.window->contentItem(), QStringLiteral("homeProjectMenuRename"));
        QQuickItem* trashRow = findNamed(shell.window->contentItem(), QStringLiteral("homeProjectMenuTrash"));
        REQUIRE(rename);
        REQUIRE(trashRow);
        CHECK(rename->isEnabled());
        CHECK(trashRow->isEnabled());
        REQUIRE(findNamed(shell.window->contentItem(), QStringLiteral("homeProjectMenuVersions")));
        REQUIRE(findNamed(shell.window->contentItem(), QStringLiteral("homeProjectMenuDuplicate")));
        {
            const QImage image = shell.grab();
            REQUIRE_FALSE(image.isNull());
            saveItemShot(image, popupOf(rename), dark ? QStringLiteral("home-project-menu-files-dark.png")
                                                      : QStringLiteral("home-project-menu-files-light.png"));
        }

        // Rename… opens the dialog once the menu has closed; a new name renames the files.
        QMetaObject::invokeMethod(rename, "triggered");
        QMetaObject::invokeMethod(menu, "close");
        QObject* dialog = screen->findChild<QObject*>(QStringLiteral("homeProjectNameDialog"));
        REQUIRE(dialog);
        REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }, 3000));
        QQuickItem* field = findNamed(shell.window->contentItem(), QStringLiteral("homeProjectNameField"));
        REQUIRE(field);
        CHECK(field->property("text").toString() == QStringLiteral("Trailer"));
        field->setProperty("text", QStringLiteral("Teaser"));
        spin(150);
        {
            const QImage image = shell.grab();
            REQUIRE_FALSE(image.isNull());
            saveItemShot(image, popupOf(field), dark ? QStringLiteral("home-rename-dialog-dark.png")
                                                     : QStringLiteral("home-rename-dialog-light.png"));
        }
        QQuickItem* accept = findNamed(shell.window->contentItem(), QStringLiteral("homeProjectNameAccept"));
        REQUIRE(accept);
        QMetaObject::invokeMethod(accept, "clicked");
        const QString teaser = dir.filePath(QStringLiteral("Teaser.wabcut"));
        REQUIRE(waitFor([&] { return QFileInfo::exists(teaser); }, 3000));
        CHECK(QFileInfo::exists(teaser + QStringLiteral(".bak1")));
        CHECK(screen->property("status").toString() == QStringLiteral("Renamed to Teaser"));

        // A project that fails to open: Home's strip offers "Recover a version…", which shows its versions.
        const QString broken = dir.filePath(QStringLiteral("Broken.wabcut"));
        writeFile(broken, "{ not a project");
        saveProjectAt(broken + QStringLiteral(".bak1"));
        REQUIRE(QMetaObject::invokeMethod(screen, "openRequested", Q_ARG(QUrl, QUrl::fromLocalFile(broken))));
        REQUIRE(waitFor([&] { return !screen->property("message").toString().isEmpty(); }, 5000));
        CHECK(screen->property("messageActionText").toString() == QStringLiteral("Recover a version…"));
        QJSValue action = screen->property("messageAction").value<QJSValue>();
        REQUIRE(action.isCallable());
        action.call();
        QObject* recovery = shell.object("recoveryDialog");
        REQUIRE(recovery);
        REQUIRE(waitFor([&] { return recovery->property("opened").toBool(); }, 3000));
        CHECK(recovery->property("title").toString() == QStringLiteral("Versions of Broken"));
        // One project's versions only: no "Unsaved work" tab (another project's crash; ARGON-10).
        QQuickItem* tabs = findAnyNamed(shell.window->contentItem(), QStringLiteral("recoveryTabs"));
        REQUIRE(tabs);
        CHECK_FALSE(tabs->isVisible());
        auto* lifecycle = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
        REQUIRE(lifecycle);
        REQUIRE(waitFor([&] { return lifecycle->recoveryVersions().size() >= 2; }, 20000));
        spin(300);
        {
            QQuickItem* list = findNamed(shell.window->contentItem(), QStringLiteral("recoveryVersionsList"));
            REQUIRE(list);
            const QImage image = shell.grab();
            REQUIRE_FALSE(image.isNull());
            saveItemShot(image, popupOf(list), dark ? QStringLiteral("home-versions-broken-dark.png")
                                                    : QStringLiteral("home-versions-broken-light.png"));
        }
        QMetaObject::invokeMethod(recovery, "close");
        REQUIRE(waitFor([&] { return !lifecycle->recoveryScanning(); }, 30000));
        spin(100);

        // Versions… from the menu shows the same dashboard for that project.
        entry.insert(QStringLiteral("url"), QUrl::fromLocalFile(teaser));
        entry.insert(QStringLiteral("path"), QDir::toNativeSeparators(teaser));
        entry.insert(QStringLiteral("name"), QStringLiteral("Teaser"));
        REQUIRE(QMetaObject::invokeMethod(screen, "projectAction", Q_ARG(QString, QStringLiteral("versions")),
                                          Q_ARG(QVariant, QVariant(entry))));
        REQUIRE(waitFor([&] { return recovery->property("opened").toBool(); }, 3000));
        CHECK(recovery->property("title").toString() == QStringLiteral("Versions of Teaser"));
        QMetaObject::invokeMethod(recovery, "close");
        REQUIRE(waitFor([&] { return !lifecycle->recoveryScanning(); }, 30000));

        // Move to Recycle Bin… asks first; Undo brings the files back (the test's bin, never the real one).
        REQUIRE(QMetaObject::invokeMethod(screen, "projectAction", Q_ARG(QString, QStringLiteral("trash")),
                                          Q_ARG(QVariant, QVariant(entry))));
        QObject* confirm = screen->findChild<QObject*>(QStringLiteral("homeTrashConfirm"));
        REQUIRE(confirm);
        REQUIRE(waitFor([&] { return confirm->property("opened").toBool(); }, 3000));
        QQuickItem* yes = findNamed(shell.window->contentItem(), QStringLiteral("menuConfirmButton"));
        REQUIRE(yes);
        QMetaObject::invokeMethod(yes, "clicked");
        REQUIRE(waitFor([&] { return !QFileInfo::exists(teaser); }, 3000));
        CHECK(trash.contents().size() == 2); // the project and its backup
        CHECK(screen->property("trashUndo").toBool());
        REQUIRE(QMetaObject::invokeMethod(screen, "undoRemove"));
        CHECK(QFileInfo::exists(teaser));
        CHECK(QFileInfo::exists(teaser + QStringLiteral(".bak1")));

        const QStringList messages = takeMessages();
        INFO(join(messages));
        CHECK(messages.isEmpty());
    }
}

// Fix round 1 (ARGON-08): an AAF or OMF is imported off the GUI thread: importFiles returns at once with it pending, the
// progress arrives in timelineImport, and the import is one undo step with its report. Cancel imports nothing and never
// removes audio an earlier import wrote.
TEST_CASE("Home: an AAF imports in the background with progress and Cancel imports nothing", "[app][home][import][aaf-import]") {
    Fixture f;
    const QString aaf = f.path(QStringLiteral("audiopost.aaf"));
    REQUIRE(QFile::copy(QDir(QStringLiteral(WABCUT_INTERCHANGE_FIXTURES)).filePath(QStringLiteral("export/audiopost.aaf")), aaf));
    QList<QVariantMap> states;
    QVariantMap report;
    QObject::connect(&f.lifecycle, &ProjectLifecycle::timelineImportChanged, &f.lifecycle,
                     [&] { states.append(f.lifecycle.timelineImport()); });
    QObject::connect(&f.lifecycle, &ProjectLifecycle::importReport, &f.lifecycle, [&](const QVariantMap& r) { report = r; });
    const QVariantMap summary = f.lifecycle.importFiles({QUrl::fromLocalFile(aaf)},
                                                        {{QStringLiteral("newProject"), true}, {QStringLiteral("openImported"), true}});
    CHECK(summary.value(QStringLiteral("pendingTimelines")).toInt() == 1);
    CHECK(summary.value(QStringLiteral("timelines")).toInt() == 0);
    CHECK(f.lifecycle.timelineImport().value(QStringLiteral("busy")).toBool());
    CHECK(f.lifecycle.timelineImport().value(QStringLiteral("text")).toString().startsWith(QStringLiteral("Importing audiopost.aaf")));
    CHECK(f.controller.session().undoCount() == 0); // nothing yet: the work is on a worker
    REQUIRE(waitFor([&] { return !f.lifecycle.timelineImport().value(QStringLiteral("busy")).toBool(); }, 60000));
    const QVariantMap done = f.lifecycle.timelineImport();
    INFO(done.value(QStringLiteral("error")).toString().toStdString());
    CHECK(done.value(QStringLiteral("done")).toString() == QStringLiteral("audiopost.aaf"));
    CHECK(report.value(QStringLiteral("file")).toString() == QStringLiteral("audiopost.aaf"));
    CHECK(f.controller.session().undoCount() == 1);
    CHECK(f.controller.session().undoLabel() == std::optional<std::string>("Import audiopost.aaf"));
    REQUIRE(f.controller.currentSequence());
    CHECK(f.controller.snapshot()->sequences.size() == 1); // the new project's empty sequence went in the same step
    // The unsaved project: the embedded audio went beside the file, which can be written.
    const QDir media(f.path(QStringLiteral("audiopost Media")));
    REQUIRE(media.exists());
    const QStringList written = media.entryList(QDir::Files);
    CHECK(written.size() == 6);
    // Progress went up to 100 %.
    double last = -1;
    bool rising = true;
    for (const QVariantMap& s : states) {
        if (!s.value(QStringLiteral("busy")).toBool())
            continue;
        const double p = s.value(QStringLiteral("progress")).toDouble();
        rising = rising && p >= last;
        last = p;
    }
    CHECK(rising);

    // Again, cancelled at once: nothing changes, and the audio the first import wrote stays.
    states.clear();
    const QVariantMap again = f.lifecycle.importFiles({QUrl::fromLocalFile(aaf)});
    CHECK(again.value(QStringLiteral("pendingTimelines")).toInt() == 1);
    f.lifecycle.cancelTimelineImport();
    REQUIRE(waitFor([&] { return !f.lifecycle.timelineImport().value(QStringLiteral("busy")).toBool(); }, 60000));
    CHECK(f.lifecycle.timelineImport().value(QStringLiteral("cancelled")).toBool());
    CHECK(f.controller.session().undoCount() == 1);
    CHECK(media.entryList(QDir::Files) == written);
}
