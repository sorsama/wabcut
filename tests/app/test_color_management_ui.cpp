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

TEST_CASE("Color panel menu: Apply Grade to the selected clips and to all clips of the media", "[app][color][gradecopy]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* color = shell.engine.singletonInstance<ColorBridge*>("Wabcut", "Color");
    REQUIRE(controller);
    REQUIRE(color);
    // A1, A2 and A3 show media A; B1 media B.
    Timeline tl(R"(
        V1 tgt |AaaaaA5aaaBbbbbA10aaa|
    )");
    controller->resetProject(tl.state, tl.seq);
    const ClipId a1 = tl.clip("V1@0");
    const ClipId a2 = tl.clip("V1@5");
    const ClipId b1 = tl.clip("V1@10");
    const ClipId a3 = tl.clip("V1@15");
    controller->selectClipById(a1, ProjectController::Replace);
    REQUIRE(waitFor([&] { return color->targetClip() == a1; }));
    const auto gradeOf = [&](ClipId id) -> const EffectInstance* {
        for (const EffectInstance& e : controller->currentSequence()->findClip(id)->clip().effects()) {
            if (e.effect == "wabcut.color.grade")
                return &e;
        }
        return nullptr;
    };

    // What the menu rows count: the other selected video clips, the other clips of the media.
    QVariantMap targets = color->gradeCopyTargets();
    CHECK(targets.value(QStringLiteral("selection")).toInt() == 0);
    CHECK(targets.value(QStringLiteral("media")).toInt() == 2);
    CHECK(targets.value(QStringLiteral("mediaName")).toString().startsWith(QLatin1Char('A')));
    // Nothing to copy yet.
    CHECK_FALSE(color->applyGradeTo(QStringLiteral("media")));
    CHECK_FALSE(color->status().isEmpty());

    REQUIRE(color->setParam(QStringLiteral("basic.exposure"), 0.5));
    REQUIRE(waitFor([&] { return color->hasGrade(); }));
    controller->setSelection({a1, b1, a3}, a1);
    REQUIRE(waitFor([&] { return color->targetClip() == a1 && color->hasGrade(); }));
    targets = color->gradeCopyTargets();
    CHECK(targets.value(QStringLiteral("selection")).toInt() == 2);
    CHECK(targets.value(QStringLiteral("media")).toInt() == 2);

    // The Color workspace's panel menu: the two rows above Export .cube…, enabled; a screenshot with it open.
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("window.workspace5"))));
    QQuickItem* panel = nullptr;
    REQUIRE(waitFor([&] {
        panel = findNamed(shell.window->contentItem(), QStringLiteral("colorPanel"));
        return panel != nullptr && panel->isVisible();
    }));
    REQUIRE_FALSE(shell.grab().isNull()); // lays the workspace out
    QQuickItem* menuButton = findNamed(panel, QStringLiteral("colorPanelMenuButton"));
    REQUIRE(menuButton);
    REQUIRE(QMetaObject::invokeMethod(menuButton, "clicked"));
    QQuickItem* selectedRow = nullptr;
    REQUIRE(waitFor([&] {
        selectedRow = findNamed(shell.window->contentItem(), QStringLiteral("applyGradeSelectedItem"));
        return selectedRow != nullptr && selectedRow->isVisible();
    }));
    QQuickItem* mediaRow = findNamed(shell.window->contentItem(), QStringLiteral("applyGradeMediaItem"));
    REQUIRE(mediaRow);
    CHECK(selectedRow->isEnabled());
    CHECK(mediaRow->isEnabled());
    CHECK(mediaRow->property("text").toString().startsWith(QStringLiteral("Apply Grade to All Clips of \"A")));
    CHECK(findNamed(shell.window->contentItem(), QStringLiteral("exportCubeItem")) != nullptr);
    spin(150);
    {
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        REQUIRE(image.save(outputDir() + QStringLiteral("/color-apply-grade-menu.png")));
    }
    QObject* menu = selectedRow->property("menu").value<QObject*>();
    REQUIRE(menu);
    REQUIRE(QMetaObject::invokeMethod(menu, "close"));
    spin(100);

    // All clips of the media: A2 and A3 (not B1), one undo step.
    const auto undos = controller->session().undoCount();
    REQUIRE(color->applyGradeTo(QStringLiteral("media")));
    CHECK(color->status() == QStringLiteral("Grade applied to 2 clips"));
    CHECK(controller->session().undoCount() == undos + 1);
    CHECK(controller->undoText() == QStringLiteral("Undo Apply Grade"));
    REQUIRE(gradeOf(a2));
    REQUIRE(gradeOf(a3));
    CHECK_FALSE(gradeOf(b1));
    CHECK(effectParameterAt(*gradeOf(a2), "basic.exposure", Ticks{}) == 0.5);
    REQUIRE(controller->undo());
    CHECK_FALSE(gradeOf(a2));
    CHECK_FALSE(gradeOf(a3));

    // The selected clips: B1 and A3.
    REQUIRE(color->applyGradeTo(QStringLiteral("selection")));
    CHECK(color->status() == QStringLiteral("Grade applied to 2 clips"));
    CHECK(gradeOf(b1));
    CHECK(gradeOf(a3));
    CHECK_FALSE(gradeOf(a2));
    CHECK(controller->undoText() == QStringLiteral("Undo Apply Grade"));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
