// Batch A3 (0.7), the parts a user sees in the real shell: the Markers panel's Import button and menu (card 121), and
// Copy Frame in the Export Frame dialog and on the camera button's right-click menu (card 001). Offscreen; screenshots
// in dark and light.

#include "ShellFixture.h"

#include "BridgeIds.h"
#include "MarkersBridge.h"
#include "MonitorBridge.h"
#include "ProjectController.h"
#include "TimelineFixture.h"

#include <wabcut/command/Engine.h>

#include <catch2/catch_test_macros.hpp>

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QMouseEvent>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QUrl>

#include <functional>

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

QQuickItem* sceneItem(const Shell& shell, const char* name) {
    return findNamed(shell.window->contentItem(), QString::fromLatin1(name));
}

/// An item that lives in a Popup or a Dialog: the window's scene, else the popup object's children.
QQuickItem* inPopup(const Shell& shell, const char* popup, const char* name) {
    if (QQuickItem* item = sceneItem(shell, name))
        return item;
    QObject* host = shell.object(popup);
    if (!host)
        return nullptr;
    const auto items = host->findChildren<QQuickItem*>(QString::fromLatin1(name));
    return items.isEmpty() ? nullptr : items.front();
}

void mouse(QQuickWindow* window, QEvent::Type type, QPointF scene, Qt::MouseButton button) {
    QMouseEvent event(type, scene, scene, button, type == QEvent::MouseButtonRelease ? Qt::NoButton : button, {});
    QCoreApplication::sendEvent(window, &event);
}

void rightClick(QQuickWindow* window, QQuickItem* item) {
    const QPointF p = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    mouse(window, QEvent::MouseButtonPress, p, Qt::RightButton);
    mouse(window, QEvent::MouseButtonRelease, p, Qt::RightButton);
}

bool isOpen(QObject* popup) { return popup && popup->property("opened").toBool(); }

constexpr std::string_view kPicture = R"(
    V1 tgt |............|
    A1 tgt |............|
)";
constexpr std::string_view kClipPicture = R"(
    V1 tgt |Aaaaaaaaaaaa|
    A1 tgt |Aaaaaaaaaaaa|
)";

} // namespace

TEST_CASE("Markers panel: Import reads a marker list into the sequence or the clip's media (card 121)",
          "[app][markers][import][qml][screenshot]") {
    for (const bool dark : {true, false}) {
        DYNAMIC_SECTION((dark ? "dark" : "light")) {
            resetSettings();
            clearMessages();
            Shell shell;
            REQUIRE(shell.window != nullptr);
            shell.resize(kShotWidth, kShotHeight);
            shell.theme->setProperty("reduceMotion", true);
            shell.theme->setProperty("dark", dark);
            auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
            auto* markers = shell.engine.singletonInstance<MarkersBridge*>("Wabcut", "Markers");
            REQUIRE(controller != nullptr);
            REQUIRE(markers != nullptr);
            Timeline tl(kClipPicture, {{'A', 50}});
            controller->resetProject(tl.state, tl.seq);
            controller->setPlayhead(f(3));

            QTemporaryDir dir;
            REQUIRE(dir.isValid());
            const QString csv = dir.filePath(QStringLiteral("chapters.csv"));
            {
                QFile file(csv);
                REQUIRE(file.open(QIODevice::WriteOnly));
                file.write("Marker Name,Description,In,Duration,Marker Type,Color\n"
                           "Intro,Cold open,00:00:00:05,00:00:00:10,Chapter,Red\n"
                           "Cut,,00:00:01:00,00:00:00:00,Comment,Blue\n"
                           "Broken,,not a time,,,\n");
            }

            REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showBrowserPanel", Q_ARG(QVariant, QStringLiteral("markers"))));
            spin(150);
            QQuickItem* panel = sceneItem(shell, "markersPanel");
            REQUIRE(panel != nullptr);
            QQuickItem* importButton = sceneItem(shell, "markersImportButton");
            REQUIRE(importButton != nullptr);
            CHECK(importButton->isVisible());
            CHECK(importButton->property("tooltip").toString() == QStringLiteral("Import markers into the sequence"));

            // The menu: three rows; the playhead row is for the sequence tab.
            QObject* menu = shell.object("markersImportMenu");
            REQUIRE(menu != nullptr);
            REQUIRE(QMetaObject::invokeMethod(importButton, "clicked"));
            REQUIRE(waitFor([&] { return isOpen(menu); }));
            QObject* plain = menu->findChild<QObject*>(QStringLiteral("importMarkersFile"));
            QObject* atPlayhead = menu->findChild<QObject*>(QStringLiteral("importMarkersAtPlayhead"));
            QObject* replace = menu->findChild<QObject*>(QStringLiteral("importMarkersReplace"));
            REQUIRE(plain != nullptr);
            REQUIRE(atPlayhead != nullptr);
            REQUIRE(replace != nullptr);
            CHECK(plain->property("enabled").toBool());
            CHECK(atPlayhead->property("enabled").toBool());
            CHECK(replace->property("enabled").toBool());
            spin(200);
            REQUIRE(shell.grab().save(QDir(outputDir()).filePath(dark ? QStringLiteral("markers-import-menu.png")
                                                                       : QStringLiteral("markers-import-menu-light.png"))));
            QMetaObject::invokeMethod(menu, "close");
            REQUIRE(waitFor([&] { return !isOpen(menu); }));

            // Importing: the toast text, the markers (sorted), one undo step, the bad line skipped and named.
            const std::size_t steps = controller->session().undoCount();
            CHECK(markers->importFromFile(QUrl::fromLocalFile(csv)).isEmpty());
            CHECK(markers->message() == QStringLiteral("Imported 2 markers · 1 line skipped: line 4: \"not a time\" is not a time"));
            CHECK(controller->session().undoCount() == steps + 1);
            CHECK(controller->session().undoLabel() == "Import Markers");
            REQUIRE(markers->totalCount() == 2);
            CHECK(markers->items().at(0).toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Intro"));
            CHECK(markers->items().at(0).toMap().value(QStringLiteral("type")).toString() == QStringLiteral("chapter"));

            // The same file again: nothing new, in whole words; a file with one marker says "1 marker".
            CHECK(markers->importFromFile(QUrl::fromLocalFile(csv)).isEmpty());
            CHECK(markers->message().startsWith(QStringLiteral("Nothing new: the 2 markers in the file are already there")));
            const QString one = dir.filePath(QStringLiteral("one.txt"));
            {
                QFile file(one);
                REQUIRE(file.open(QIODevice::WriteOnly));
                file.write("0:03 Third\n");
            }
            // At the playhead: the file's 0:03 counts from the playhead (frame 3), so the marker lands 3 s + 3 frames in.
            CHECK(markers->importFromFile(QUrl::fromLocalFile(one), true).isEmpty());
            CHECK(markers->message() == QStringLiteral("Imported 1 marker"));
            REQUIRE(markers->totalCount() == 3);
            CHECK(markers->items().at(2).toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Third"));
            CHECK(controller->currentSequence()->markers.back().start == f(3) + Ticks::fromSeconds(3));

            // Replace.
            CHECK(markers->importFromFile(QUrl::fromLocalFile(one), false, true).isEmpty());
            CHECK(markers->totalCount() == 1);
            REQUIRE(controller->undo());
            CHECK(markers->totalCount() == 3); // one step back

            // A file that is no marker list, and a file that is missing: the reason, no change.
            const QString junk = dir.filePath(QStringLiteral("junk.txt"));
            {
                QFile file(junk);
                REQUIRE(file.open(QIODevice::WriteOnly));
                file.write("hello\n");
            }
            CHECK_FALSE(markers->importFromFile(QUrl::fromLocalFile(junk)).isEmpty());
            CHECK(markers->message().contains(QStringLiteral("no marker found")));
            CHECK_FALSE(markers->importFromFile(QUrl::fromLocalFile(dir.filePath(QStringLiteral("missing.csv")))).isEmpty());
            CHECK(markers->totalCount() == 3);

            // The Clip tab: the markers go onto the selected clip's media, in its own time.
            controller->selectClip(clipKey(tl.clip("V1@0")));
            markers->setScope(QStringLiteral("clip"));
            CHECK(importButton->property("tooltip").toString() == QStringLiteral("Import markers into the clip's media"));
            CHECK(markers->importFromFile(QUrl::fromLocalFile(csv)).isEmpty());
            CHECK(markers->message().startsWith(QStringLiteral("Imported 2 markers")));
            CHECK(markers->totalCount() == 2);
            CHECK(controller->currentSequence()->markers.size() == 3); // the sequence keeps its own
            markers->setScope(QStringLiteral("sequence"));

            const QStringList messages = takeMessages();
            INFO(join(messages));
            CHECK(messages.isEmpty());
        }
    }
}

TEST_CASE("Copy Frame in the UI: the Export Frame dialog's button and the camera button's right-click menu (card 001)",
          "[app][monitor][exportframe][qml][screenshot][gpu]") {
    for (const bool dark : {true, false}) {
        DYNAMIC_SECTION((dark ? "dark" : "light")) {
            resetSettings();
            clearMessages();
            Shell shell;
            REQUIRE(shell.window != nullptr);
            shell.resize(2560, 1440); // wide enough for every button of the monitor's transport row
            shell.theme->setProperty("reduceMotion", true);
            shell.theme->setProperty("dark", dark);
            auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
            auto* monitors = shell.engine.singletonInstance<MonitorBridge*>("Wabcut", "Monitors");
            QObject* file = shell.engine.singletonInstance<QObject*>("Wabcut", "ProjectFile");
            REQUIRE(controller != nullptr);
            REQUIRE(monitors != nullptr);
            REQUIRE(file != nullptr);
            REQUIRE(QMetaObject::invokeMethod(file, "newProject")); // outside the sample project the camera button is on
            REQUIRE_FALSE(file->property("demo").toBool());
            Timeline tl(kPicture);
            controller->resetProject(tl.state, tl.seq);
            {
                command::CommandEngine engine(controller->session());
                command::CommandRequest title;
                title.command = "title.add";
                title.params = command::Json{{"text", "Copy me"}, {"at", 0}, {"duration", f(12).value}, {"track", "V1"},
                                             {"sequence", tl.seq.value}};
                REQUIRE(engine.execute(title).ok());
            }
            controller->setPlayhead(f(4));
            QClipboard* clipboard = QGuiApplication::clipboard();
            REQUIRE(clipboard != nullptr);

            // The dialog: Copy to Clipboard beside Cancel and OK; one click puts the picture on the clipboard and closes it.
            QObject* dialog = shell.object("exportFrameDialog");
            REQUIRE(dialog != nullptr);
            monitors->requestExportFrame(false);
            REQUIRE(waitFor([&] { return dialog->property("visible").toBool(); }));
            QQuickItem* copy = inPopup(shell, "exportFrameDialog", "exportFrameCopy");
            REQUIRE(copy != nullptr);
            CHECK(copy->isVisible());
            CHECK(copy->isEnabled());
            CHECK(copy->property("text").toString() == QStringLiteral("Copy to Clipboard"));
            spin(250);
            REQUIRE(shell.grab().save(QDir(outputDir()).filePath(dark ? QStringLiteral("export-frame-copy.png")
                                                                       : QStringLiteral("export-frame-copy-light.png"))));
            clipboard->clear();
            REQUIRE(QMetaObject::invokeMethod(copy, "clicked"));
            REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }));
            const QImage image = clipboard->image();
            REQUIRE_FALSE(image.isNull());
            CHECK(image.width() == controller->currentSequence()->width);
            CHECK(monitors->message().startsWith(QStringLiteral("Frame copied (")));

            // Markers mode (Export Frames at Markers) has no use for it.
            monitors->requestExportFramesAtMarkers(false);
            REQUIRE(waitFor([&] { return dialog->property("visible").toBool(); }));
            QQuickItem* copyInMarkers = inPopup(shell, "exportFrameDialog", "exportFrameCopy");
            CHECK((copyInMarkers == nullptr || !copyInMarkers->isVisible()));
            QMetaObject::invokeMethod(dialog, "close");
            REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }));

            // A right-click on the camera button opens a one-row menu: Copy Frame.
            QQuickItem* camera = sceneItem(shell, "programExportFrame");
            REQUIRE(camera != nullptr);
            REQUIRE(camera->isVisible());
            REQUIRE(camera->isEnabled());
            QObject* menu = shell.object("programCopyFrameMenu");
            REQUIRE(menu != nullptr);
            rightClick(shell.window.get(), camera);
            REQUIRE(waitFor([&] { return isOpen(menu); }));
            spin(150);
            CHECK(shell.grab().save(QDir(outputDir()).filePath(dark ? QStringLiteral("copy-frame-menu.png")
                                                                    : QStringLiteral("copy-frame-menu-light.png"))));
            QObject* item = menu->findChild<QObject*>(QStringLiteral("programCopyFrameItem"));
            REQUIRE(item != nullptr);
            CHECK(item->property("text").toString() == QStringLiteral("Copy Frame"));
            clipboard->clear();
            REQUIRE(QMetaObject::invokeMethod(item, "triggered"));
            QMetaObject::invokeMethod(menu, "close");
            CHECK_FALSE(clipboard->image().isNull());

            // The left click still opens the dialog (the right-click handler does not eat it).
            REQUIRE(QMetaObject::invokeMethod(camera, "clicked"));
            REQUIRE(waitFor([&] { return dialog->property("visible").toBool(); }));
            QMetaObject::invokeMethod(dialog, "close");

            const QStringList messages = takeMessages();
            INFO(join(messages));
            CHECK(messages.isEmpty());
        }
    }
}
