// M5f: the Home screen — sequence presets and New Project, recent projects (persisted, missing files flagged), the
// import checks (detectImport), importing a timeline as a new project whose sequence opens (no empty placeholder, one
// undo step, the old default unchanged), the experimental .prproj reader only on request, and the screen in the
// shell: File > Home, a real drop of an FCP7 XML, the import report, 1100x700. Design language v3: the search narrows
// the projects, a star persists and the Starred filter shows it, the list view, the rail scrolls to a section.
// Offscreen; no window is shown.

#include "A11yAudit.h"
#include "ShellFixture.h"

#include "HomeBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Time.h>

#include <catch2/catch_test_macros.hpp>

#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeData>
#include <QMouseEvent>
#include <QQuickItem>
#include <QSettings>
#include <QTemporaryDir>

#include <functional>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

QString fixture(const char* name) {
    return QDir(QStringLiteral(WABCUT_INTERCHANGE_FIXTURES)).filePath(QString::fromLatin1(name));
}

QVariantMap presetNamed(const HomeBridge& home, const QString& id) {
    for (const QVariant& p : home.presets()) {
        if (p.toMap().value(QStringLiteral("id")).toString() == id)
            return p.toMap();
    }
    return {};
}

QVariantMap rateLabelled(const HomeBridge& home, const QString& label) {
    for (const QVariant& r : home.rates()) {
        if (r.toMap().value(QStringLiteral("label")).toString() == label)
            return r.toMap();
    }
    return {};
}

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
    QUrl folder(const QString& sub = {}) const {
        return QUrl::fromLocalFile(sub.isEmpty() ? dir.path() : dir.filePath(sub));
    }
};

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

QQuickItem* visibleItem(Shell& shell, const QString& objectName) {
    return findItem(shell.window->contentItem(), [&](QQuickItem* i) {
        return i->objectName() == objectName && i->isVisible();
    });
}

/// A QML `var` list (a JS array) as a QVariantList.
QVariantList variantList(const QVariant& value) {
    if (value.canConvert<QJSValue>() && value.metaType() == QMetaType::fromType<QJSValue>())
        return value.value<QJSValue>().toVariant().toList();
    return value.toList();
}

/// The demo project is still what the editor shows (nothing was imported).

bool demoShown(Shell& shell) {
    auto* lifecycle = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    return lifecycle && lifecycle->demo();
}

} // namespace

// ---- New project ----------------------------------------------------------------------------------------------------

TEST_CASE("Home: a new project from a preset gets that sequence and is saved where asked", "[app][home]") {
    Fixture fx;
    const QVariantList presets = fx.home.presets();
    REQUIRE(presets.size() == 9);
    QStringList ids;
    for (const QVariant& p : presets)
        ids << p.toMap().value(QStringLiteral("id")).toString();
    CHECK(ids == QStringList{"1080p23976", "1080p25", "1080p2997", "1080p30", "1080p60", "uhd", "vertical", "square", "custom"});
    CHECK(presetNamed(fx.home, "1080p2997").value("rateLabel").toString() == QStringLiteral("29.97"));
    CHECK(fx.lifecycle.demo());

    SECTION("1080p 29.97") {
        const QVariantMap preset = presetNamed(fx.home, "1080p2997");
        REQUIRE(fx.home.createProject(QStringLiteral("Brand Film"), fx.folder(), preset).isEmpty());
        const SequencePtr seq = fx.controller.currentSequence();
        REQUIRE(seq);
        CHECK(seq->frameRate.num == kRate29_97.num);
        CHECK(seq->frameRate.den == kRate29_97.den);
        CHECK(fx.controller.frameWidth() == 1920);
        CHECK(fx.controller.frameHeight() == 1080);
        CHECK(fx.controller.sequenceNames() == QStringList{QStringLiteral("Sequence 01")});
        CHECK_FALSE(fx.lifecycle.demo());
        CHECK_FALSE(fx.lifecycle.untitled());
        CHECK(QDir::fromNativeSeparators(fx.lifecycle.filePath()) == fx.dir.filePath(QStringLiteral("Brand Film.wabcut")));
        CHECK(QFile::exists(fx.dir.filePath(QStringLiteral("Brand Film.wabcut"))));
        // A second project with that name in that folder is refused; nothing changes.
        CHECK_FALSE(fx.home.createProject(QStringLiteral("Brand Film"), fx.folder(), preset).isEmpty());
    }
    SECTION("vertical 9:16 at another rate, in a folder that does not exist yet") {
        QVariantMap preset = presetNamed(fx.home, "vertical");
        CHECK(preset.value("rateChoice").toBool());
        const QVariantMap rate = rateLabelled(fx.home, "25");
        preset.insert(QStringLiteral("rateNum"), rate.value("rateNum"));
        preset.insert(QStringLiteral("rateDen"), rate.value("rateDen"));
        REQUIRE(fx.home.createProject(QStringLiteral("Social"), fx.folder(QStringLiteral("Clips/Social")), preset).isEmpty());
        CHECK(fx.controller.frameWidth() == 1080);
        CHECK(fx.controller.frameHeight() == 1920);
        CHECK(fx.controller.currentSequence()->frameRate.num == 25);
        CHECK(QFile::exists(fx.dir.filePath(QStringLiteral("Clips/Social/Social.wabcut"))));
    }
    SECTION("custom size") {
        QVariantMap preset = presetNamed(fx.home, "custom");
        preset.insert(QStringLiteral("width"), 1280);
        preset.insert(QStringLiteral("height"), 720);
        REQUIRE(fx.home.createProject(QStringLiteral("Custom"), fx.folder(), preset).isEmpty());
        CHECK(fx.controller.formatText() == QStringLiteral("720p30"));
    }
    SECTION("names and sizes that cannot be used") {
        CHECK_FALSE(fx.home.validateProject(QStringLiteral("  "), fx.folder()).isEmpty());
        CHECK_FALSE(fx.home.validateProject(QStringLiteral("a/b"), fx.folder()).isEmpty());
        CHECK_FALSE(fx.home.validateProject(QStringLiteral("Film"), QUrl()).isEmpty());
        CHECK(fx.home.validateProject(QStringLiteral("Film"), fx.folder()).isEmpty());
        CHECK(fx.home.projectPath(QStringLiteral("Film"), fx.folder()) ==
              QDir::toNativeSeparators(fx.dir.filePath(QStringLiteral("Film.wabcut"))));
        QVariantMap bad = presetNamed(fx.home, "custom");
        bad.insert(QStringLiteral("width"), 0);
        CHECK_FALSE(fx.home.createProject(QStringLiteral("Bad"), fx.folder(), bad).isEmpty());
        CHECK(fx.lifecycle.demo()); // nothing changed
        CHECK_FALSE(fx.lifecycle.newProjectWith({{QStringLiteral("width"), 1920}, {QStringLiteral("height"), 1080},
                                                 {QStringLiteral("rateNum"), 30}, {QStringLiteral("rateDen"), 0}}));
        CHECK_FALSE(fx.lifecycle.lastError().isEmpty());
    }
}

// ---- Recent projects ------------------------------------------------------------------------------------------------

TEST_CASE("Home: recent projects are remembered newest first, persist, flag missing files and can be removed", "[app][home]") {
    Fixture fx;
    CHECK(fx.home.recent().isEmpty());
    const QVariantMap hd = presetNamed(fx.home, "1080p25");
    REQUIRE(fx.home.createProject(QStringLiteral("First"), fx.folder(), hd).isEmpty());
    REQUIRE(fx.home.createProject(QStringLiteral("Second"), fx.folder(), presetNamed(fx.home, "uhd")).isEmpty());
    QVariantList recent = fx.home.recent();
    REQUIRE(recent.size() == 2);
    CHECK(recent[0].toMap().value("name").toString() == QStringLiteral("Second"));
    CHECK(recent[0].toMap().value("format").toString() == QStringLiteral("2160p23.976"));
    CHECK(recent[0].toMap().value("openedText").toString().startsWith(QStringLiteral("Today")));
    CHECK(recent[1].toMap().value("name").toString() == QStringLiteral("First"));
    CHECK_FALSE(recent[1].toMap().value("missing").toBool());
    CHECK(QDir::fromNativeSeparators(fx.home.defaultFolder().toLocalFile()) == fx.dir.path());

    // Opening one moves it to the front.
    REQUIRE(fx.lifecycle.open(QUrl::fromLocalFile(fx.dir.filePath(QStringLiteral("First.wabcut")))));
    CHECK(fx.home.recent()[0].toMap().value("name").toString() == QStringLiteral("First"));

    // Persisted: a new Home reads the same list.
    {
        HomeBridge again;
        REQUIRE(again.recent().size() == 2);
        CHECK(again.recent()[0].toMap().value("name").toString() == QStringLiteral("First"));
    }

    // A file that is gone is flagged, then removed from the list.
    REQUIRE(QFile::remove(fx.dir.filePath(QStringLiteral("Second.wabcut"))));
    fx.home.refreshRecent();
    recent = fx.home.recent();
    REQUIRE(recent.size() == 2);
    CHECK(recent[1].toMap().value("missing").toBool());
    fx.home.removeRecent(recent[1].toMap().value("url").toUrl());
    CHECK(fx.home.recent().size() == 1);
    fx.home.clearRecent();
    CHECK(fx.home.recent().isEmpty());
    CHECK(QSettings().value(QStringLiteral("home/recentProjects")).toList().isEmpty());
}

// ---- Import ---------------------------------------------------------------------------------------------------------

TEST_CASE("Home: detectImport says what a file is before importing it", "[app][home]") {
    ProjectController controller;
    ProjectLifecycle lifecycle(&controller);
    const QVariantMap xml = lifecycle.detectImport(QUrl::fromLocalFile(fixture("fcp7_23976_mac.xml")));
    CHECK(xml.value("kind").toString() == QStringLiteral("timeline"));
    CHECK(xml.value("format").toString() == QStringLiteral("fcp7-xml"));
    CHECK(xml.value("formatName").toString() == QStringLiteral("Final Cut Pro 7 XML"));
    CHECK(xml.value("supported").toBool());
    CHECK_FALSE(xml.value("experimental").toBool());

    const QVariantMap prproj = lifecycle.detectImport(QUrl::fromLocalFile(fixture("prproj/07_transitions.prproj")));
    CHECK(prproj.value("kind").toString() == QStringLiteral("timeline"));
    CHECK(prproj.value("experimental").toBool());
    CHECK_FALSE(prproj.value("guidance").toString().isEmpty()); // export XML from Premiere instead

    QTemporaryDir dir;
    const QString drp = dir.filePath(QStringLiteral("Grade.drp"));
    {
        QFile f(drp);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("not a timeline");
    }
    const QVariantMap resolve = lifecycle.detectImport(QUrl::fromLocalFile(drp));
    CHECK(resolve.value("kind").toString() == QStringLiteral("timeline"));
    CHECK_FALSE(resolve.value("supported").toBool());
    CHECK_FALSE(resolve.value("guidance").toString().isEmpty());

    CHECK(lifecycle.detectImport(QUrl::fromLocalFile(dir.filePath(QStringLiteral("a.wabcut")))).value("kind") == QStringLiteral("project"));
    CHECK(lifecycle.detectImport(QUrl::fromLocalFile(dir.filePath(QStringLiteral("clip.mov")))).value("kind") == QStringLiteral("media"));
}

TEST_CASE("Home: a timeline imports as a new project whose sequence opens, in one undo step", "[app][home]") {
    const QUrl xml = QUrl::fromLocalFile(fixture("fcp7_23976_mac.xml"));
    SECTION("newProject + openImported: just the imported sequence, current, reported") {
        ProjectController controller;
        ProjectLifecycle lifecycle(&controller);
        QList<QVariantMap> reports;
        QObject::connect(&lifecycle, &ProjectLifecycle::importReport, [&](const QVariantMap& r) { reports << r; });
        const QVariantMap result = lifecycle.importFiles(
            {xml}, {{QStringLiteral("newProject"), true}, {QStringLiteral("openImported"), true}});
        CHECK(result.value("timelines").toInt() == 1);
        CHECK(result.value("firstSequence").toInt() == 0);
        CHECK(controller.sequenceNames() == QStringList{QStringLiteral("Reel 2 Assembly")});
        CHECK(controller.sequenceName() == QStringLiteral("Reel 2 Assembly"));
        CHECK(controller.formatText() == QStringLiteral("1080p23.976"));
        REQUIRE(reports.size() == 1);
        CHECK(reports[0].value("format").toString() == QStringLiteral("Final Cut Pro 7 XML"));
        CHECK_FALSE(lifecycle.demo());
        CHECK(lifecycle.dirty());
        // One undo step: back to the new project's empty sequence.
        REQUIRE(controller.undo());
        CHECK(controller.sequenceNames() == QStringList{QStringLiteral("Sequence 01")});
    }
    SECTION("from an open project, newProject starts another one") {
        ProjectController controller;
        ProjectLifecycle lifecycle(&controller);
        lifecycle.newProject();
        controller.newSequence();
        REQUIRE(controller.sequenceNames().size() == 2);
        lifecycle.importFiles({xml}, {{QStringLiteral("newProject"), true}, {QStringLiteral("openImported"), true}});
        CHECK(controller.sequenceNames() == QStringList{QStringLiteral("Reel 2 Assembly")});
    }
    SECTION("without options the old behaviour stays: the demo's import keeps its first sequence current") {
        ProjectController controller;
        ProjectLifecycle lifecycle(&controller);
        const QVariantMap result = lifecycle.importFiles({xml});
        CHECK(result.value("timelines").toInt() == 1);
        CHECK_FALSE(result.contains(QStringLiteral("firstSequence")));
        CHECK(controller.sequenceNames() == QStringList{QStringLiteral("Sequence 01"), QStringLiteral("Reel 2 Assembly")});
        CHECK(controller.currentSequenceIndex() == 0);
    }
}

TEST_CASE("Home: a Premiere .prproj imports only with the experimental reader", "[app][home]") {
    const QUrl prproj = QUrl::fromLocalFile(fixture("prproj/07_transitions.prproj"));
    ProjectController controller;
    ProjectLifecycle lifecycle(&controller);
    const QVariantMap refused = lifecycle.importFiles({prproj}, {{QStringLiteral("newProject"), true}});
    CHECK(refused.value("timelines").toInt() == 0);
    CHECK(refused.value("failed").toList().size() == 1);
    const QVariantMap read = lifecycle.importFiles(
        {prproj}, {{QStringLiteral("newProject"), true},
                   {QStringLiteral("openImported"), true},
                   {QStringLiteral("experimentalPremiereProject"), true}});
    CHECK(read.value("timelines").toInt() == 1);
    CHECK(read.value("firstSequence").toInt() == 0);
    CHECK_FALSE(controller.sequenceNames().contains(QStringLiteral("Sequence 01")));
}

// ---- In the shell ---------------------------------------------------------------------------------------------------

TEST_CASE("Home screen in the shell: File > Home, drop an FCP7 XML, the sequence opens with the report; screenshots",
          "[app][home][screenshots][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* home = shell.engine.singletonInstance<HomeBridge*>("Wabcut", "Home");
    REQUIRE(controller != nullptr);
    REQUIRE(home != nullptr);
    CHECK_FALSE(home->showAtLaunch()); // the tests start on the editor
    QQuickItem* screen = shell.item("homeScreen");
    REQUIRE(screen != nullptr);
    CHECK_FALSE(screen->property("opened").toBool());

    // Recent projects to show: two files that exist (one with a picture of the editor as its thumbnail), one gone.
    QTemporaryDir dir;
    const QImage editor = shell.grab();
    REQUIRE_FALSE(editor.isNull());
    QQuickItem* monitor = shell.item("monitorPanel");
    REQUIRE(monitor != nullptr);
    const double dpr = shell.window->effectiveDevicePixelRatio();
    const QRectF monitorRect = monitor->mapRectToScene(QRectF(0, 0, monitor->width(), monitor->height()));
    const QString thumbFile = dir.filePath(QStringLiteral("thumb.png"));
    REQUIRE(editor.copy(QRectF(monitorRect.topLeft() * dpr, monitorRect.size() * dpr).toRect()).save(thumbFile));
    const auto touch = [&](const QString& name) {
        QFile f(dir.filePath(name));
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("{}");
        return dir.filePath(name);
    };
    const QDateTime now = QDateTime::currentDateTime();
    QSettings().setValue(QStringLiteral("home/recentProjects"), QVariantList{
        QVariantMap{{"path", touch(QStringLiteral("Brand Film - Final.wabcut"))}, {"opened", now.toString(Qt::ISODate)},
                    {"format", QStringLiteral("1080p23.976")}, {"thumbnail", QUrl::fromLocalFile(thumbFile).toString()}},
        QVariantMap{{"path", touch(QStringLiteral("Interview Selects.wabcut"))}, {"opened", now.addDays(-1).toString(Qt::ISODate)},
                    {"format", QStringLiteral("2160p25")}, {"thumbnail", QString()}},
        QVariantMap{{"path", dir.filePath(QStringLiteral("Old Promo.wabcut"))}, {"opened", now.addDays(-9).toString(Qt::ISODate)},
                    {"format", QStringLiteral("1080p29.97")}, {"thumbnail", QString()}},
    });

    // File > Home
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showHome"));
    spin(400);
    CHECK(screen->property("opened").toBool());
    CHECK(screen->isVisible());
    QQuickItem* editorShell = shell.item("editorShell");
    REQUIRE(editorShell != nullptr);
    CHECK_FALSE(editorShell->isEnabled());
    CHECK_FALSE(editorShell->isVisible()); // covered: not drawn
    REQUIRE(home->recent().size() == 3);
    CHECK(home->recent()[2].toMap().value("missing").toBool());
    REQUIRE(visibleItem(shell, QStringLiteral("homeRecent_0")) != nullptr);
    REQUIRE(visibleItem(shell, QStringLiteral("homeImport_premiere")) != nullptr);
    REQUIRE(visibleItem(shell, QStringLiteral("homeImport_resolve")) != nullptr);
    REQUIRE(visibleItem(shell, QStringLiteral("homeImport_finalcut")) != nullptr);
    REQUIRE(visibleItem(shell, QStringLiteral("homePreset_1080p2997")) != nullptr);
    {
        const QImage image = shell.grab();
        REQUIRE(image.save(outputDir() + QStringLiteral("/home.png")));
    }

    // A .prproj waits for the experimental reader, clearly labelled.
    REQUIRE(QMetaObject::invokeMethod(screen, "handleUrls",
                                      Q_ARG(QVariant, QVariant::fromValue(QList<QUrl>{QUrl::fromLocalFile(fixture("prproj/07_transitions.prproj"))}))));
    spin(100);
    CHECK(visibleItem(shell, QStringLiteral("homeExperimentalPrompt")) != nullptr);
    CHECK(screen->property("opened").toBool());
    {
        const QImage image = shell.grab();
        REQUIRE(image.save(outputDir() + QStringLiteral("/home-prproj.png")));
    }
    // Cancel (a real click): nothing is imported.
    QQuickItem* cancel = visibleItem(shell, QStringLiteral("homeExperimentalCancel"));
    REQUIRE(cancel != nullptr);
    {
        const QPointF p = cancel->mapToScene(QPointF(cancel->width() / 2, cancel->height() / 2));
        QMouseEvent press(QEvent::MouseButtonPress, p, p, Qt::LeftButton, Qt::LeftButton, {});
        QCoreApplication::sendEvent(shell.window.get(), &press);
        QMouseEvent release(QEvent::MouseButtonRelease, p, p, Qt::LeftButton, Qt::NoButton, {});
        QCoreApplication::sendEvent(shell.window.get(), &release);
    }
    spin(100);
    CHECK(visibleItem(shell, QStringLiteral("homeExperimentalPrompt")) == nullptr);
    CHECK(screen->property("opened").toBool());
    CHECK(demoShown(shell));

    // Drag an FCP7 XML over the drop zone: it lights up.
    QQuickItem* zone = visibleItem(shell, QStringLiteral("homeDropZone"));
    REQUIRE(zone != nullptr);
    const QPoint at = zone->mapToScene(QPointF(zone->width() / 2, zone->height() / 2)).toPoint();
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(fixture("fcp7_23976_mac.xml"))});
    QDragEnterEvent enter(at, Qt::CopyAction, &mime, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(shell.window.get(), &enter);
    QDragMoveEvent move(at, Qt::CopyAction, &mime, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(shell.window.get(), &move);
    spin(150);
    QObject* importCard = shell.object("homeImportCard");
    REQUIRE(importCard != nullptr);
    CHECK(importCard->property("dragActive").toBool());
    {
        const QImage image = shell.grab();
        REQUIRE(image.save(outputDir() + QStringLiteral("/home-import.png")));
    }

    // Drop: a new project with the imported sequence open, and the import report.
    QDropEvent dropped(at, Qt::CopyAction, &mime, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(shell.window.get(), &dropped);
    spin(400);
    CHECK_FALSE(screen->property("opened").toBool());
    CHECK(controller->sequenceNames() == QStringList{QStringLiteral("Reel 2 Assembly")});
    CHECK(controller->sequenceName() == QStringLiteral("Reel 2 Assembly"));
    QObject* report = shell.object("importReportDialog");
    REQUIRE(report != nullptr);
    CHECK(report->property("opened").toBool());
    CHECK(editorShell->isEnabled());
    {
        const QImage image = shell.grab();
        REQUIRE(image.save(outputDir() + QStringLiteral("/home-import-report.png")));
    }
    QMetaObject::invokeMethod(report, "close");
    spin(100);

    // 1100x700, the smallest window: every card fits across.
    shell.resize(1100, 700);
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showHome"));
    spin(400);
    for (const char* name : {"homeImportCard", "homeNewProjectCard", "homeRecentCard"}) {
        QQuickItem* card = visibleItem(shell, QString::fromLatin1(name));
        INFO(name);
        REQUIRE(card != nullptr);
        const QRectF r = card->mapRectToScene(QRectF(0, 0, card->width(), card->height()));
        CHECK(r.left() >= 0);
        CHECK(r.right() <= 1100);
    }
    {
        const QImage image = shell.grab();
        REQUIRE(image.save(outputDir() + QStringLiteral("/home-1100x700.png")));
    }
    // Esc goes back to the project.
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(shell.window.get(), &esc);
    spin(300);
    CHECK_FALSE(screen->property("opened").toBool());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Home screen v3: the search narrows the projects and a star persists for the Starred filter",
          "[app][home][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* home = shell.engine.singletonInstance<HomeBridge*>("Wabcut", "Home");
    REQUIRE(home != nullptr);

    // Two projects that exist and one that is gone.
    QTemporaryDir dir;
    const auto touch = [&](const QString& name) {
        QFile f(dir.filePath(name));
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("{}");
        return dir.filePath(name);
    };
    const QDateTime now = QDateTime::currentDateTime();
    QSettings().setValue(QStringLiteral("home/recentProjects"), QVariantList{
        QVariantMap{{"path", touch(QStringLiteral("Brand Film - Final.wabcut"))}, {"opened", now.toString(Qt::ISODate)},
                    {"format", QStringLiteral("1080p23.976")}, {"thumbnail", QString()}},
        QVariantMap{{"path", touch(QStringLiteral("Interview Selects.wabcut"))}, {"opened", now.addDays(-1).toString(Qt::ISODate)},
                    {"format", QStringLiteral("2160p25")}, {"thumbnail", QString()}},
        QVariantMap{{"path", dir.filePath(QStringLiteral("Old Promo.wabcut"))}, {"opened", now.addDays(-9).toString(Qt::ISODate)},
                    {"format", QStringLiteral("1080p29.97")}, {"thumbnail", QString()}},
    });
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showHome"));
    spin(400);
    QQuickItem* screen = shell.item("homeScreen");
    REQUIRE(screen != nullptr);
    REQUIRE(screen->property("opened").toBool());
    REQUIRE(home->recent().size() == 3);
    QObject* recent = shell.object("homeRecentCard");
    REQUIRE(recent != nullptr);
    const auto shownNames = [&] {
        QStringList names;
        for (const QVariant& p : variantList(recent->property("shown")))
            names << p.toMap().value(QStringLiteral("name")).toString();
        return names;
    };
    CHECK(shownNames().size() == 3);
    CHECK_FALSE(screen->property("greeting").toString().isEmpty());

    // Continue: the newest project that is still there.
    QQuickItem* continueName = visibleItem(shell, QStringLiteral("homeContinueName"));
    REQUIRE(continueName != nullptr);
    CHECK(continueName->property("text").toString() == QStringLiteral("Brand Film - Final"));

    // The search (any case) narrows the projects; nothing matching says so.
    QQuickItem* search = visibleItem(shell, QStringLiteral("homeSearch"));
    REQUIRE(search != nullptr);
    search->setProperty("text", QStringLiteral("INTERVIEW"));
    spin(50);
    CHECK(shownNames() == QStringList{QStringLiteral("Interview Selects")});
    REQUIRE(visibleItem(shell, QStringLiteral("homeRecent_0")) != nullptr);
    search->setProperty("text", QStringLiteral("no such project"));
    spin(50);
    CHECK(shownNames().isEmpty());
    CHECK(visibleItem(shell, QStringLiteral("homeProjectsEmpty")) != nullptr);
    search->setProperty("text", QString());
    spin(50);
    CHECK(shownNames().size() == 3);

    // The rail scrolls the page to Projects and lights it.
    QQuickItem* projectsEntry = visibleItem(shell, QStringLiteral("homeNav_projects"));
    REQUIRE(projectsEntry != nullptr);
    REQUIRE(pressWithKey(shell.window.get(), projectsEntry, Qt::Key_Space, QStringLiteral(" ")));
    spin(100);
    CHECK(screen->property("currentSection").toString() == QStringLiteral("projects"));
    QQuickItem* content = shell.item("homeContent");
    REQUIRE(content != nullptr);
    if (content->property("contentHeight").toReal() > content->height())
        CHECK(content->property("contentY").toReal() > 0);

    // Star the second project (Space on its star): it persists and the Starred filter shows only it.
    QQuickItem* second = visibleItem(shell, QStringLiteral("homeRecent_1"));
    REQUIRE(second != nullptr);
    QQuickItem* star = findItem(second, [](QQuickItem* i) { return i->objectName() == QStringLiteral("homeRecentStar"); });
    REQUIRE(star != nullptr);
    CHECK_FALSE(star->property("starred").toBool());
    REQUIRE(pressWithKey(shell.window.get(), star, Qt::Key_Space, QStringLiteral(" ")));
    spin(50);
    CHECK(star->property("starred").toBool());
    CHECK(recent->property("starredCount").toInt() == 1);
    const QString secondPath = home->recent()[1].toMap().value(QStringLiteral("path")).toString();
    const QJsonArray starred = QJsonDocument::fromJson(shell.preferences->property("homeStarred").toString().toUtf8()).array();
    REQUIRE(starred.size() == 1);
    CHECK(starred[0].toString() == secondPath);
    CHECK(waitFor([] { return QSettings().value(QStringLiteral("home/starred")).toString().contains(QStringLiteral("Interview Selects")); },
                  3000));
    recent->setProperty("filter", 1);
    spin(50);
    CHECK(shownNames() == QStringList{QStringLiteral("Interview Selects")});

    // The list view shows the same projects as rows.
    recent->setProperty("listView", true);
    spin(100);
    CHECK(visibleItem(shell, QStringLiteral("homeRecentRows")) != nullptr);
    CHECK(visibleItem(shell, QStringLiteral("homeRecentList")) == nullptr);
    REQUIRE(visibleItem(shell, QStringLiteral("homeRecent_0")) != nullptr);
    {
        const QImage image = shell.grab();
        REQUIRE(image.save(outputDir() + QStringLiteral("/home-projects-list.png")));
    }

    // Unstar it: the Starred filter is empty again and says how to fill it.
    QQuickItem* rowStar = findItem(visibleItem(shell, QStringLiteral("homeRecent_0")),
                                   [](QQuickItem* i) { return i->objectName() == QStringLiteral("homeRecentStar"); });
    REQUIRE(rowStar != nullptr);
    REQUIRE(pressWithKey(shell.window.get(), rowStar, Qt::Key_Space, QStringLiteral(" ")));
    spin(50);
    CHECK(recent->property("starredCount").toInt() == 0);
    CHECK(shownNames().isEmpty());
    CHECK(visibleItem(shell, QStringLiteral("homeProjectsEmpty")) != nullptr);
    recent->setProperty("filter", 0);
    recent->setProperty("listView", false);
    spin(50);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
