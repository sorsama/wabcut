// App start-up: the QML singletons are built by their factories, and Main.qml loaded the way main.cpp loads it
// (QQmlApplicationEngine::loadFromModule, here with visible=false so nothing appears on screen) has the library's
// image://wabcut-thumbnails provider.
//
// Regression: ProjectController, CommandBridge and CommandPalette are default-constructible, and Qt builds a
// QML_SINGLETON with a public default constructor in preference to a static create(). So ProjectController::create()
// never ran, the thumbnail provider it registers was missing in the app ("Invalid image provider"), and the palette
// and bridge factories were skipped too. Their singletons are now registered by QML_FOREIGN wrappers whose create()
// always runs.

#include "ShellFixture.h"

#include "CommandBridge.h"
#include "CommandPalette.h"
#include "MediaBridge.h"
#include "ProjectController.h"

#include <catch2/catch_test_macros.hpp>

#include <QQmlApplicationEngine>
#include <QQmlEngine>
#include <QQuickWindow>

using namespace wabcut::app;
using namespace wabcut::test;

namespace {

const QString kThumbnails = QLatin1String(ThumbnailImageProvider::kProviderId);

} // namespace

TEST_CASE("QML singletons are built by their factories: ProjectController registers the thumbnail provider",
          "[app][startup]") {
    QQmlEngine engine;
    REQUIRE(engine.imageProvider(kThumbnails) == nullptr);

    // Only ProjectController is created, so the provider can only come from its factory (not ProjectFile's fallback).
    auto* controller = engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(controller);
    CHECK(engine.imageProvider(kThumbnails) != nullptr);

    // The bridge and palette factories wire themselves to that controller and to the keymap.
    auto* bridge = engine.singletonInstance<CommandBridge*>("Wabcut", "CommandBridge");
    REQUIRE(bridge);
    CHECK(bridge->controller() == controller);
    auto* palette = engine.singletonInstance<CommandPalette*>("Wabcut", "CommandPalette");
    REQUIRE(palette);
    palette->setPersistRecent(false);
    palette->setQuery(QStringLiteral("undo"));
    REQUIRE_FALSE(palette->results().isEmpty());
    CHECK(palette->results().first().toMap().value(QStringLiteral("key")).toString() == QStringLiteral("command:edit.undo"));
}

TEST_CASE("Main.qml loaded like main.cpp has the thumbnail provider", "[app][startup]") {
    clearMessages();
    QQmlApplicationEngine engine;
    engine.setInitialProperties({{QStringLiteral("visible"), false}}); // never shown
    engine.loadFromModule("Wabcut", "Main");
    REQUIRE(engine.rootObjects().size() == 1);
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    REQUIRE(window);
    CHECK_FALSE(window->isVisible());

    CHECK(engine.imageProvider(kThumbnails) != nullptr);
    spin(50);
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK_FALSE(messages.join(QLatin1Char('\n')).contains(QStringLiteral("Invalid image provider")));
}
