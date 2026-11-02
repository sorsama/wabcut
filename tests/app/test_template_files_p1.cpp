// 0.6 (P1 round, task D1) #50: Motion Graphics template files in the app.
//   * Graphics.isTemplateFile (.mogrt, .wabcut-template.json, any case), template.export through the bridge (the file's
//     suffix, an existing file only with overwrite) and template.import into This project or My templates, one undo step
//     each, with the import report's shape;
//   * Graphics > Install Motion Graphics Template… (into This project or My templates) and Import / a drop sending a
//     template file to the template library instead of the media import, with a toast;
//   * Callouts > Browse's Install template… button, the template menu's Export template… row, and the window's two file
//     dialogs; a screenshot of the Browse page's install button.

#include "ShellFixture.h"

#include "GraphicsBridge.h"
#include "ProjectController.h"
#include "TimelineFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFileInfo>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>

#include <filesystem>
#include <memory>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

constexpr std::string_view kPicture = R"(
    V1 tgt |Aaaaaaaaaa|
)";

std::filesystem::path pathOf(const QTemporaryDir& dir) { return std::filesystem::path(dir.path().toStdU16String()); }

QUrl fileUrl(const std::filesystem::path& file) { return QUrl::fromLocalFile(QString::fromStdU16String(file.u16string())); }

QVariantMap templateOf(const GraphicsBridge& graphics, const QString& key) {
    for (const QVariant& t : graphics.templates()) {
        if (t.toMap().value(QStringLiteral("key")).toString() == key)
            return t.toMap();
    }
    return {};
}

QVariant evaluate(const Shell& shell, const QString& code) {
    QQmlExpression expression(qmlContext(shell.window.get()), shell.window.get(), code);
    const QVariant result = expression.evaluate();
    INFO(code.toStdString() << ": " << expression.error().toString().toStdString());
    REQUIRE_FALSE(expression.hasError());
    return result;
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

QRectF sceneRect(const QQuickItem* item) { return item->mapRectToScene(QRectF(0, 0, item->width(), item->height())); }

void saveCrop(const Shell& shell, const QRectF& area, const QString& file) {
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    const double dpr = image.devicePixelRatio();
    const QRectF r = area.intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    REQUIRE(image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect()).save(QDir(outputDir()).filePath(file)));
}

} // namespace

TEST_CASE("Templates: a .mogrt or .wabcut-template.json installs into a library and any template exports; the menu rows "
          "ask the window for its file dialogs",
          "[app][graphics][p1]") {
    resetSettings();
    clearMessages();
    Timeline tl(kPicture);
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    QTemporaryDir library;
    QTemporaryDir out;
    REQUIRE(library.isValid());
    REQUIRE(out.isValid());
    GraphicsBridge graphics;
    graphics.setUserLibrary(pathOf(library));
    graphics.setController(&controller);

    // Which files are templates: the two suffixes, any case, and nothing else.
    CHECK(graphics.isTemplateFile(QUrl::fromLocalFile(QStringLiteral("C:/T/Lower Third.mogrt"))));
    CHECK(graphics.isTemplateFile(QUrl::fromLocalFile(QStringLiteral("C:/T/lower.WABCUT-TEMPLATE.JSON"))));
    CHECK(graphics.isTemplateFile(QUrl::fromLocalFile(QStringLiteral("C:/T/simple.mogrt"))));
    CHECK_FALSE(graphics.isTemplateFile(QUrl::fromLocalFile(QStringLiteral("C:/T/template.json"))));
    CHECK_FALSE(graphics.isTemplateFile(QUrl::fromLocalFile(QStringLiteral("C:/T/clip.mp4"))));

    // Any template exports - a built-in here - and the file dialog's bare name gets the suffix.
    const std::filesystem::path target = pathOf(out) / "subscribe";
    CHECK(graphics.exportTemplate(QStringLiteral("builtin/wabcut.gaming.subscribe"), fileUrl(target), false).isEmpty());
    const std::filesystem::path written = pathOf(out) / "subscribe.wabcut-template.json";
    INFO(written.string());
    REQUIRE(std::filesystem::exists(written));
    CHECK(QFileInfo(QString::fromStdU16String(written.u16string())).size() > 0);
    // An existing file is only replaced with overwrite.
    CHECK_FALSE(graphics.exportTemplate(QStringLiteral("builtin/wabcut.gaming.subscribe"), fileUrl(target), false).isEmpty());
    CHECK(graphics.exportTemplate(QStringLiteral("builtin/wabcut.gaming.subscribe"), fileUrl(target), true).isEmpty());
    // An unknown template key says so; nothing is written.
    CHECK_FALSE(graphics.exportTemplate(QStringLiteral("nothing"), fileUrl(pathOf(out) / "x"), true).isEmpty());

    // Installing it into This project is one undo step, and the library lists it.
    const QVariantMap installed = graphics.importTemplate(fileUrl(written), "project");
    INFO(installed.value(QStringLiteral("error")).toString().toStdString());
    REQUIRE(installed.value(QStringLiteral("error")).toString().isEmpty());
    CHECK(installed.value(QStringLiteral("key")).toString() == QStringLiteral("project/wabcut.gaming.subscribe"));
    CHECK(installed.value(QStringLiteral("library")).toString() == QStringLiteral("project"));
    CHECK_FALSE(installed.value(QStringLiteral("name")).toString().isEmpty());
    const QVariantMap report = installed.value(QStringLiteral("report")).toMap();
    CHECK(report.value(QStringLiteral("format")).toString() == QStringLiteral("Motion Graphics template"));
    CHECK(report.value(QStringLiteral("file")).toString() == QStringLiteral("subscribe.wabcut-template.json"));
    CHECK_FALSE(templateOf(graphics, QStringLiteral("project/wabcut.gaming.subscribe")).isEmpty());
    REQUIRE(controller.session().undoCount() > 0);
    CHECK(controller.undoText().contains(QStringLiteral("Template")));
    REQUIRE(controller.undo());
    CHECK(templateOf(graphics, QStringLiteral("project/wabcut.gaming.subscribe")).isEmpty());

    // Into My templates: a file of its own in the user's library, and removing it takes the file away.
    const QVariantMap user = graphics.importTemplate(fileUrl(written), "user");
    REQUIRE(user.value(QStringLiteral("error")).toString().isEmpty());
    CHECK(user.value(QStringLiteral("key")).toString() == QStringLiteral("user/wabcut.gaming.subscribe"));
    CHECK_FALSE(templateOf(graphics, QStringLiteral("user/wabcut.gaming.subscribe")).isEmpty());
    CHECK(std::filesystem::exists(pathOf(library) / "wabcut.gaming.subscribe.wabcut-template.json"));
    CHECK(graphics.removeTemplate(QStringLiteral("user/wabcut.gaming.subscribe")).isEmpty());
    CHECK(templateOf(graphics, QStringLiteral("user/wabcut.gaming.subscribe")).isEmpty());

    // A file that is not a template is refused before anything is read.
    const QVariantMap refused = graphics.importTemplate(QUrl::fromLocalFile(QStringLiteral("C:/T/clip.mp4")), "project");
    CHECK_FALSE(refused.value(QStringLiteral("error")).toString().isEmpty());
    CHECK(refused.value(QStringLiteral("key")).toString().isEmpty());

    // The rows ask the window for its dialogs (Main.qml opens them after the menu closed).
    QStringList installed_for;
    QStringList exported_for;
    QObject::connect(&graphics, &GraphicsBridge::installRequested,
                     [&](const QString& where) { installed_for << where; });
    QObject::connect(&graphics, &GraphicsBridge::exportRequested,
                     [&](const QString& key) { exported_for << key; });
    graphics.openInstallDialog("project");
    graphics.openInstallDialog("user");
    graphics.openInstallDialog("elsewhere"); // only the two libraries exist
    graphics.openExportDialog(QStringLiteral("builtin/wabcut.gaming.subscribe"));
    CHECK(installed_for == QStringList({QStringLiteral("project"), QStringLiteral("user"), QStringLiteral("project")}));
    CHECK(exported_for == QStringList({QStringLiteral("builtin/wabcut.gaming.subscribe")}));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Templates: Install Motion Graphics Template and a template drop put the file in This project, and the Browse "
          "page's button and the template menu's export row are there",
          "[app][graphics][p1]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    INFO(join(shell.errors));
    REQUIRE(shell.errors.isEmpty());
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    QQuickWindow* w = shell.window.get();
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* graphics = shell.engine.singletonInstance<GraphicsBridge*>("Wabcut", "Graphics");
    REQUIRE(controller != nullptr);
    REQUIRE(graphics != nullptr);
    QTemporaryDir library;
    QTemporaryDir out;
    REQUIRE(library.isValid());
    REQUIRE(out.isValid());
    graphics->setUserLibrary(pathOf(library));
    spin(50);

    // A template file to install: a Wabcut template written by the bridge (a built-in, so the test needs no project).
    const std::filesystem::path written = pathOf(out) / "subscribe.wabcut-template.json";
    REQUIRE(graphics->exportTemplate(QStringLiteral("builtin/wabcut.gaming.subscribe"), fileUrl(written), false).isEmpty());
    REQUIRE(std::filesystem::exists(written));

    // File > Import (importUrls) sends a template file to the template library, not the media import.
    const QUrl url = fileUrl(written);
    REQUIRE(QMetaObject::invokeMethod(w, "importUrls", Q_ARG(QVariant, QVariant::fromValue(QVariantList{url}))));
    spin(50);
    CHECK_FALSE(templateOf(*graphics, QStringLiteral("project/wabcut.gaming.subscribe")).isEmpty());
    REQUIRE(controller->session().undoCount() > 0);
    CHECK(controller->undoText().contains(QStringLiteral("Template")));
    // One undo takes the template away again.
    REQUIRE(controller->undo());
    CHECK(templateOf(*graphics, QStringLiteral("project/wabcut.gaming.subscribe")).isEmpty());

    // The same through the window's helper (what the Install dialog's accepted signal calls), into My templates.
    REQUIRE(QMetaObject::invokeMethod(w, "installTemplateFiles",
                                      Q_ARG(QVariant, QVariant::fromValue(QVariantList{url})),
                                      Q_ARG(QVariant, QStringLiteral("user"))));
    spin(50);
    CHECK_FALSE(templateOf(*graphics, QStringLiteral("user/wabcut.gaming.subscribe")).isEmpty());

    // The window's two file dialogs: the template filters and where the export dialog points.
    QObject* installDialog = shell.object("installTemplateDialog");
    REQUIRE(installDialog != nullptr);
    CHECK(installDialog->property("library").toString() == QStringLiteral("project"));
    const QStringList filters = installDialog->property("nameFilters").toStringList();
    REQUIRE(filters.size() >= 1);
    CHECK(filters.front().contains(QStringLiteral("*.mogrt")));
    CHECK(filters.front().contains(QStringLiteral("*.wabcut-template.json")));
    QObject* exportDialog = shell.object("exportTemplateDialog");
    REQUIRE(exportDialog != nullptr);
    CHECK(exportDialog->property("nameFilters").toStringList().front().contains(QStringLiteral("*.wabcut-template.json")));
    CHECK(exportDialog->property("templateKey").toString().isEmpty());
    const QString suggested = evaluate(shell, QStringLiteral("window.exportTemplateFileName('builtin/wabcut.gaming.subscribe')"))
                                  .toString();
    CHECK(suggested.endsWith(QStringLiteral("/wabcut.gaming.subscribe.wabcut-template.json")));
    // Writing through the window (the dialog's accepted signal) lands the template in a file.
    const std::filesystem::path written2 = pathOf(out) / "again.wabcut-template.json";
    REQUIRE(QMetaObject::invokeMethod(w, "exportTemplateNow",
                                      Q_ARG(QVariant, QStringLiteral("builtin/wabcut.gaming.subscribe")),
                                      Q_ARG(QVariant, fileUrl(written2))));
    CHECK(std::filesystem::exists(written2));

    // Graphics > Install Motion Graphics Template… rows are in the menu; both ask the window for the dialog.
    QObject* menu = shell.object("graphicsMenu");
    REQUIRE(menu != nullptr);
    QObject* row = menu->findChild<QObject*>(QStringLiteral("installTemplateMenuItem"));
    REQUIRE(row != nullptr);
    CHECK(row->property("text").toString() == QStringLiteral("Install Motion Graphics Template…"));
    CHECK_FALSE(row->property("runsCommand").toBool());
    QObject* userRow = menu->findChild<QObject*>(QStringLiteral("installTemplateUserMenuItem"));
    REQUIRE(userRow != nullptr);
    CHECK(userRow->property("text").toString().contains(QStringLiteral("My Templates")));

    // Callouts > Browse's Install template… button (a page of its own engine here: no window to open a dialog from).
    {
        QQmlEngine engine;
        QQmlComponent pageComponent(&engine, "Wabcut", "GraphicsBrowsePage");
        INFO(pageComponent.errorString().toStdString());
        REQUIRE_FALSE(pageComponent.isError());
        auto* pageGraphics = engine.singletonInstance<GraphicsBridge*>("Wabcut", "Graphics");
        REQUIRE(pageGraphics != nullptr);
        QStringList asked;
        QObject::connect(pageGraphics, &GraphicsBridge::installRequested, [&](const QString& where) { asked << where; });
        std::unique_ptr<QObject> page(pageComponent.create());
        REQUIRE(page != nullptr);
        QQuickItem* button = findNamed(qobject_cast<QQuickItem*>(page.get()), QStringLiteral("graphicsInstallTemplate"));
        REQUIRE(button != nullptr);
        CHECK(QMetaObject::invokeMethod(button, "clicked"));
        CHECK(asked == QStringList({QStringLiteral("project")}));
    }

    // A template's menu offers Export template…, for a built-in too, and asks for the dialog once the menu has closed.
    {
        QQmlEngine engine;
        QQmlComponent menuComponent(&engine, "Wabcut", "GraphicTemplateMenu");
        INFO(menuComponent.errorString().toStdString());
        REQUIRE_FALSE(menuComponent.isError());
        auto* menuGraphics = engine.singletonInstance<GraphicsBridge*>("Wabcut", "Graphics");
        REQUIRE(menuGraphics != nullptr);
        QStringList asked;
        QObject::connect(menuGraphics, &GraphicsBridge::exportRequested, [&](const QString& key) { asked << key; });
        std::unique_ptr<QObject> templateMenu(menuComponent.create());
        REQUIRE(templateMenu != nullptr);
        templateMenu->setProperty("key", QStringLiteral("builtin/wabcut.gaming.subscribe"));
        QObject* exportRow = templateMenu->findChild<QObject*>(QStringLiteral("graphicTemplateExport"));
        REQUIRE(exportRow != nullptr);
        CHECK(exportRow->property("text").toString() == QStringLiteral("Export template…"));
        QObject* deleteRow = templateMenu->findChild<QObject*>(QStringLiteral("graphicTemplateDelete"));
        REQUIRE(deleteRow != nullptr);
        CHECK_FALSE(deleteRow->property("visible").toBool()); // a built-in template cannot be deleted
        REQUIRE(QMetaObject::invokeMethod(exportRow, "triggered"));
        CHECK(templateMenu->property("pendingExport").toString() == QStringLiteral("builtin/wabcut.gaming.subscribe"));
        CHECK(asked.isEmpty()); // it waits for the menu to close
        REQUIRE(QMetaObject::invokeMethod(templateMenu.get(), "finishPendingExport"));
        CHECK(asked == QStringList({QStringLiteral("builtin/wabcut.gaming.subscribe")}));
        CHECK(templateMenu->property("pendingExport").toString().isEmpty());
    }

    // The screenshot: Graphics > Install Motion Graphics Template… (dark).
    QObject* bar = shell.object("appMenuBar");
    REQUIRE(bar != nullptr);
    REQUIRE(QMetaObject::invokeMethod(bar, "open", Q_ARG(int, 5), Q_ARG(bool, false)));
    QObject* graphicsMenu = shell.object("graphicsMenu");
    REQUIRE(graphicsMenu != nullptr);
    REQUIRE(waitFor([&] { return graphicsMenu->property("opened").toBool(); }));
    spin(200);
    auto* menuItem = qobject_cast<QQuickItem*>(graphicsMenu->property("contentItem").value<QObject*>());
    REQUIRE(menuItem != nullptr);
    saveCrop(shell, sceneRect(menuItem->parentItem()).adjusted(-8, -8, 8, 8),
             QStringLiteral("p1d-graphics-install-template.png"));
    REQUIRE(QMetaObject::invokeMethod(graphicsMenu, "close"));
    REQUIRE(waitFor([&] { return !graphicsMenu->property("visible").toBool(); }));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    for (const QString& m : messages) {
        // Only what this test's QML could warn about (other streams' windows are not this test's business).
        if (!m.contains(QStringLiteral("Main.qml")) && !m.contains(QStringLiteral("GraphicsMenu.qml"))
                && !m.contains(QStringLiteral("GraphicTemplateMenu.qml"))
                && !m.contains(QStringLiteral("GraphicsBrowsePage.qml")))
            continue;
        CHECK_FALSE(m.contains(QStringLiteral("qml"), Qt::CaseInsensitive));
    }
    resetSettings();
}
