// 0.2: colour management in the shell (docs/color.md "In the app") - the Color panel's gear opens the Color Management
// dialog with Premiere's Color Setup presets; OK sets the project's colour management in one undo step and the monitor
// badge follows; the Media color space menu is in Basic Correction. Offscreen, no window shown.

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "ColorBridge.h"
#include "ProjectController.h"

#include <catch2/catch_test_macros.hpp>

#include <QQuickItem>

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

} // namespace

TEST_CASE("Color Management dialog: Premiere's colour setups in one undo step and the monitor badge follows (0.2)",
          "[app][color][colormanagement]") {
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(1600, 900);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* color = shell.engine.singletonInstance<ColorBridge*>("Wabcut", "Color");
    REQUIRE(controller);
    REQUIRE(color);
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa|
    )");
    controller->resetProject(tl.state, tl.seq);
    spin(100);
    REQUIRE(color->colorManagementModes().size() == 6);
    CHECK(color->monitorLabel().isEmpty()); // Direct Rec.709: the plain picture

    shell.window->setProperty("workspace", 3); // the Color workspace
    QQuickItem* panel = nullptr;
    REQUIRE(waitFor([&] {
        panel = findNamed(shell.window->contentItem(), QStringLiteral("colorPanel"));
        return panel != nullptr;
    }));
    CHECK(findNamed(panel, QStringLiteral("mediaColorSpaceButton")) != nullptr);
    QQuickItem* gear = findNamed(panel, QStringLiteral("colorManagementButton"));
    REQUIRE(gear);
    REQUIRE(QMetaObject::invokeMethod(gear, "clicked"));
    QObject* dialog = panel->findChild<QObject*>(QStringLiteral("colorManagementDialog"));
    REQUIRE(dialog);
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
    CHECK(dialog->property("mode").toString() == QStringLiteral("rec709"));
    auto* content = dialog->property("contentItem").value<QQuickItem*>();
    REQUIRE(content);
    for (const char* id : {"rec709", "hdr-hlg", "hdr-pq", "aces", "wide-gamut", "off"}) {
        INFO(id);
        CHECK(findNamed(content, QStringLiteral("colorSetup_") + QLatin1String(id)) != nullptr);
    }

    // Direct PQ (HDR) with a 4000-nit peak: one undo step, the badge says what the monitor shows.
    const std::size_t before = controller->session().undoCount();
    dialog->setProperty("mode", QStringLiteral("hdr-pq"));
    dialog->setProperty("peakNits", 4000.0);
    REQUIRE(QMetaObject::invokeMethod(dialog, "apply"));
    CHECK(controller->snapshot()->colorManagement.mode == ColorManagementMode::HdrPq);
    CHECK(controller->snapshot()->colorManagement.peakNits == 4000.0);
    CHECK(controller->session().undoCount() == before + 1);
    REQUIRE(waitFor([&] { return color->monitorLabel().contains(QStringLiteral("tone-mapped")); }));
    CHECK(color->colorManagement().value(QStringLiteral("name")).toString() == QStringLiteral("Direct PQ (HDR)"));
    REQUIRE(waitFor([&] { return !dialog->property("opened").toBool(); }));

    // Disable Color Management: the badge says so.
    REQUIRE(color->setColorManagement({{QStringLiteral("mode"), QStringLiteral("off")}}));
    REQUIRE(waitFor([&] { return color->monitorLabel() == QStringLiteral("Color management off"); }));
}
