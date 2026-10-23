// Main.qml loads cleanly, its seams exist, the GPU pill shows the real renderer, every focusable control has an
// accessible name and a visible focus ring, and interacting with the mock shell stays warning-free.

#include "ShellFixture.h"

#include "ProjectController.h"

#include <catch2/catch_test_macros.hpp>

#include <QAccessible>
#include <QMetaObject>
#include <QQuickItem>

using namespace wabcut::test;

namespace {

QStringList takeAndReport() {
    return takeMessages();
}

// Visible FocusRing component somewhere in `item`'s visual subtree.
bool hasVisibleFocusRing(const QQuickItem* item) {
    for (const QQuickItem* child : item->childItems()) {
        if (QString::fromLatin1(child->metaObject()->className()).startsWith(QLatin1String("FocusRing"))
            && child->isVisible())
            return true;
        if (hasVisibleFocusRing(child))
            return true;
    }
    return false;
}

QString describeItem(const QQuickItem* item) {
    const QString name = item->objectName();
    return QString::fromLatin1(item->metaObject()->className()) + (name.isEmpty() ? QString() : QLatin1Char('#') + name);
}

QList<QQuickItem*> focusChain(QQuickWindow* window) {
    QList<QQuickItem*> chain;
    QQuickItem* start = window->contentItem();
    QQuickItem* current = start;
    for (int i = 0; i < 2000; ++i) {
        current = current->nextItemInFocusChain(true);
        if (!current || current == start || chain.contains(current))
            break;
        chain << current;
    }
    return chain;
}

} // namespace

TEST_CASE("Main.qml loads offscreen without QML warnings or errors", "[app][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    INFO("component errors:\n" << join(shell.errors));
    REQUIRE(shell.errors.isEmpty());
    REQUIRE(shell.window != nullptr);
    REQUIRE(shell.theme != nullptr);
    REQUIRE(shell.preferences != nullptr);
    CHECK_FALSE(shell.window->isVisible());
    CHECK((shell.window->flags() & Qt::FramelessWindowHint));
    CHECK(shell.window->minimumSize() == QSize(1100, 700));

    // Render one frame so delegates, images and effects are fully realised before checking the log.
    const QImage frame = shell.grab();
    CHECK_FALSE(frame.isNull());
    spin(50);

    // Seams for later milestones exist; the monitor slot hosts exactly the ProgramMonitor (M4).
    QQuickItem* slot = shell.item("programMonitorSlot");
    REQUIRE(slot != nullptr);
    REQUIRE(slot->childItems().size() == 1);
    CHECK(slot->childItems().front()->objectName() == QStringLiteral("programMonitor"));
    CHECK(QByteArray(slot->childItems().front()->metaObject()->className()) == "wabcut::app::ProgramMonitor");
    CHECK(slot->width() > 0);
    CHECK(shell.item("timelineLanes") != nullptr);
    CHECK(shell.item("workspaceTabs") != nullptr);

    // Saved preferences were applied, then color transitions switched on.
    CHECK(shell.theme->property("colorTransitions").toBool());

    const QStringList messages = takeAndReport();
    INFO("Qt messages:\n" << join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("GPU status pill shows the actual render backend and adapter", "[app][qml][rhi]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    QObject* renderer = shell.object("rendererInfo");
    QQuickItem* pill = shell.item("gpuPill");
    REQUIRE(renderer != nullptr);
    REQUIRE(pill != nullptr);

    // The first frame initializes the scene graph; RendererInfo reads QQuickWindow::rhi() then.
    REQUIRE_FALSE(shell.grab().isNull());
    REQUIRE(waitFor([&] { return renderer->property("ready").toBool(); }));

    const QString api = renderer->property("api").toString();
    const QString adapter = renderer->property("adapter").toString();
    const QString summary = renderer->property("summary").toString();
    INFO("api=" << api.toStdString() << " adapter=" << adapter.toStdString() << " summary=" << summary.toStdString());
    CHECK(api == requestedRhiBackend());
    CHECK_FALSE(adapter.isEmpty());
    CHECK(summary.startsWith(api));
    CHECK(pill->property("detail").toString() == summary);
    writeOutputFile(QStringLiteral("renderer.txt"),
                    QStringLiteral("GPU pill: %1\nAPI: %2\nAdapter: %3\n").arg(summary, api, adapter));

    const QStringList messages = takeAndReport();
    INFO("Qt messages:\n" << join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Every focusable control has an accessible name and a visible focus ring", "[app][a11y]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    QAccessible::setActive(true);

    const QList<QQuickItem*> chain = focusChain(shell.window.get());
    QStringList names;
    QStringList unnamed;
    QStringList noRing;
    QStringList noFocus;
    for (QQuickItem* item : chain) {
        if (!item->objectName().isEmpty())
            names << item->objectName();

        QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(item);
        const QString accessibleName = iface ? iface->text(QAccessible::Name).trimmed() : QString();
        if (accessibleName.isEmpty())
            unnamed << describeItem(item);

        item->forceActiveFocus(Qt::TabFocusReason);
        if (!item->hasActiveFocus()) {
            noFocus << describeItem(item);
            continue;
        }
        // Text fields show focus with their accent border (focus always means editing for them).
        if (item->inherits("QQuickTextInput"))
            continue;
        if (!hasVisibleFocusRing(item))
            noRing << describeItem(item);
    }
    QAccessible::setActive(false);

    INFO("tab stops: " << chain.size() << ", named: " << join(names));
    CHECK(chain.size() >= 30);
    // UX rework: the menu bar's titles replaced the left rail's buttons (Settings is in Edit; panels in Window).
    for (const char* expected : {"menuBar_file", "menuBar_edit", "menuBar_clip", "menuBar_sequence", "menuBar_markers",
                                 "menuBar_graphics", "menuBar_view", "menuBar_window", "menuBar_help", "saveButton",
                                 "sequencePill", "workspaceTab_edit", "newWorkspaceButton"})
        CHECK(names.contains(QLatin1String(expected)));
    INFO("without accessible name: " << join(unnamed));
    CHECK(unnamed.isEmpty());
    INFO("cannot take focus: " << join(noFocus));
    CHECK(noFocus.isEmpty());
    INFO("no visible focus ring: " << join(noRing));
    CHECK(noRing.isEmpty());

    const QStringList messages = takeAndReport();
    INFO("Qt messages:\n" << join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Interacting with the mock shell stays warning-free", "[app][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    REQUIRE_FALSE(shell.grab().isNull());

    QQuickItem* libraryTabs = shell.item("libraryTabs");
    QQuickItem* effectControls = shell.item("effectControls");
    QQuickItem* timeline = shell.item("timelinePanel");
    REQUIRE(libraryTabs != nullptr);
    REQUIRE(effectControls != nullptr);
    REQUIRE(timeline != nullptr);
    auto* controller = shell.engine.singletonInstance<wabcut::app::ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(controller != nullptr);
    QObject* view = timeline->property("view").value<QObject*>();
    REQUIRE(view != nullptr);

    SECTION("tabs, selection, zoom, scroll and workspaces") {
        // Library tabs (recreates the media grid delegates; Text is Essential Graphics) and Effect Controls with and
        // without its keyframe lane (the side panel resizes).
        for (int tab : {1, 2, 0}) {
            libraryTabs->setProperty("currentIndex", tab);
            shell.preferences->setProperty("keyframeLane", tab != 1);
            REQUIRE_FALSE(shell.grab().isNull());
        }
        // Selection: an audio clip (Music Bed), nothing, a linked video clip (Interview A + its audio).
        const auto sequence = controller->currentSequence();
        controller->selectClipById(sequence->audioTracks[2]->clips.front().id, wabcut::app::ProjectController::Replace);
        REQUIRE_FALSE(shell.grab().isNull());
        controller->clearSelection();
        REQUIRE_FALSE(shell.grab().isNull());
        controller->selectClipById(sequence->videoTracks[0]->clips.front().id, wabcut::app::ProjectController::Replace);
        CHECK(controller->selection().size() == 2);
        REQUIRE_FALSE(shell.grab().isNull());
        // Playhead, zoom and horizontal scroll through the full range; undo/redo of a clip edit.
        controller->goToNextEdit();
        for (double zoom : {1.0, 0.0, 0.6}) {
            view->setProperty("zoomLevel", zoom);
            view->setProperty("scrollX", 1200.0);
            REQUIRE_FALSE(shell.grab().isNull());
        }
        CHECK(view->property("scrollX").toDouble() > 0.0);
        controller->setClipProperty(QStringLiteral("opacity"), 50.0);
        REQUIRE(controller->undo());
        REQUIRE(controller->redo());
        REQUIRE_FALSE(shell.grab().isNull());
        // Workspaces round trip, including the layout animation.
        for (int workspace : {1, 2, 0}) {
            shell.window->setProperty("workspace", workspace);
            spin(450);
            REQUIRE_FALSE(shell.grab().isNull());
        }
    }

    SECTION("the panel focus keys open and focus the browser panels") {
        for (const char* key : {"effects", "captions", "markers", "library"}) {
            REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showBrowserPanel",
                                              Q_ARG(QVariant, QVariant(QString::fromLatin1(key)))));
            spin(300);
            CHECK(shell.window->property("browserPanel").toString() == QLatin1String(key));
            QQuickItem* focused = shell.window->activeFocusItem();
            INFO(key << " focus: " << (focused ? describeItem(focused).toStdString() : std::string("none")));
            CHECK(focused != nullptr);
            REQUIRE_FALSE(shell.grab().isNull());
        }
        // A workspace without browser panels (made from Export) opens one beside the monitors, and stays.
        shell.window->setProperty("workspace", 2);
        const int exportId = shell.window->property("workspace").toInt();
        REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showBrowserPanel",
                                          Q_ARG(QVariant, QVariant(QStringLiteral("markers")))));
        spin(100);
        CHECK(shell.window->property("workspace").toInt() == exportId);
        CHECK(shell.window->property("browserPanel").toString() == QStringLiteral("markers"));
    }

    SECTION("sequence switcher") {
        QQuickItem* pill = shell.item("sequencePill");
        REQUIRE(pill != nullptr);
        CHECK(pill->property("text").toString() == QStringLiteral("Sequence 01"));
        controller->newSequence();
        CHECK(controller->sequenceName() == QStringLiteral("Sequence 04"));
        CHECK(pill->property("text").toString() == QStringLiteral("Sequence 04"));
        controller->setCurrentSequenceIndex(1);
        CHECK(pill->property("text").toString() == QStringLiteral("Social Cut 9x16"));
        REQUIRE(QMetaObject::invokeMethod(pill, "clicked"));
        QObject* menu = shell.object("sequenceMenu");
        REQUIRE(menu != nullptr);
        CHECK(waitFor([&] { return menu->property("opened").toBool(); }));
        REQUIRE_FALSE(shell.grab().isNull());
        REQUIRE(QMetaObject::invokeMethod(menu, "close"));
    }

    SECTION("the Settings window and native window frame") {
        REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "openSettings", Q_ARG(QString, QStringLiteral("general")),
                                          Q_ARG(QString, QString())));
        QObject* settings = nullptr;
        for (QObject* o : shell.window->findChildren<QObject*>()) {
            if (o->objectName() == QStringLiteral("settingsWindow"))
                settings = o;
        }
        REQUIRE(settings != nullptr);
        CHECK(waitFor([&] { return settings->property("opened").toBool(); }));
        REQUIRE_FALSE(shell.grab().isNull());

        shell.preferences->setProperty("nativeFrame", true);
        CHECK_FALSE((shell.window->flags() & Qt::FramelessWindowHint));
        REQUIRE_FALSE(shell.grab().isNull());
        shell.preferences->setProperty("nativeFrame", false);
        CHECK((shell.window->flags() & Qt::FramelessWindowHint));
        REQUIRE(QMetaObject::invokeMethod(settings, "close"));
    }

    SECTION("compact density on a short window") {
        shell.resize(1100, 700);
        CHECK(shell.theme->property("compact").toBool());
        spin(450);
        REQUIRE_FALSE(shell.grab().isNull());
        shell.resize(kShotWidth, kShotHeight);
        CHECK_FALSE(shell.theme->property("compact").toBool());
    }

    const QStringList messages = takeAndReport();
    INFO("Qt messages:\n" << join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Settings: the AI category and openAiPreferences open Settings > AI", "[app][qml]") {
    // Regression (master 0853e65): openAiPreferences() threw "true is not a function" (a line starting with "("
    // continued the assignment above it), so the page never opened. Both ways in (0.1.3: the Settings window): its AI
    // category, and window.openAiPreferences() (the transcript panel's "set up transcription"; UX rework: the window
    // hosts Settings since the left rail went).
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);

    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "openSettings", Q_ARG(QString, QString()), Q_ARG(QString, QString())));
    QObject* settings = shell.object("settingsWindowLoader")->property("item").value<QObject*>();
    REQUIRE(settings != nullptr);
    REQUIRE(waitFor([&] { return settings->property("opened").toBool(); }));
    // The pages and categories are delegates: walk the visual tree.
    std::function<QQuickItem*(QQuickItem*, const QString&)> walk = [&](QQuickItem* item, const QString& name) -> QQuickItem* {
        if (!item || item->objectName() == name)
            return item;
        for (QQuickItem* child : item->childItems()) {
            if (QQuickItem* found = walk(child, name))
                return found;
        }
        return nullptr;
    };
    QQuickItem* content = settings->property("contentItem").value<QQuickItem*>();
    QQuickItem* category = walk(content, QStringLiteral("settingsCategory_ai"));
    REQUIRE(category != nullptr);
    REQUIRE(QMetaObject::invokeMethod(category, "clicked"));
    QQuickItem* page = nullptr;
    REQUIRE(waitFor([&] { return (page = walk(content, QStringLiteral("aiPreferences"))) != nullptr; }));
    const QStringList early = takeMessages();
    INFO(join(early));
    REQUIRE(waitFor([&] { return page->property("opened").toBool(); }));
    REQUIRE(QMetaObject::invokeMethod(settings, "close"));
    REQUIRE(waitFor([&] { return !page->property("opened").toBool(); }));

    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "openAiPreferences"));
    REQUIRE(waitFor([&] { return page->property("opened").toBool(); }));

    const QStringList messages = early + takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
