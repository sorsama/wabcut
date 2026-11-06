// 0.2: colour management in the shell (docs/color.md "In the app") - the Color panel's gear opens the Color Management
// dialog with Premiere's Color Setup presets; OK sets the project's colour management in one undo step and the monitor
// badge follows; the Media color space menu is in Basic Correction. 0.6 (#72): gamut compression, the output colour space
// and SDR Conform in the dialog (screenshots in both themes), and the Color panel's SDR Conform preview. Offscreen, no
// window shown.

#include "ShellFixture.h"
#include "TimelineFixture.h"
#include <wabcut/model/ProjectOps.h>

#include "ColorBridge.h"
#include "ProjectController.h"

#include <catch2/catch_test_macros.hpp>

#include <QQmlComponent>
#include <QQuickItem>

#include <algorithm>

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
    REQUIRE(color->colorManagementModes().size() == 7); // 0.6: with Minimal Tone Mapping
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
    for (const char* id : {"rec709", "hdr-hlg", "hdr-pq", "aces", "wide-gamut-minimal", "wide-gamut", "off"}) {
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

namespace {

/// 0.6 (#72): the Color Management dialog with HDR finishing set (Direct PQ, gamut compression, Rec.2020 out, SDR
/// Conform), its picture in the dark or light theme, then OK.
void hdrFinishingDialog(bool dark) {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, 1700); // tall enough for the whole dialog (it scrolls in a short window)
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* color = shell.engine.singletonInstance<ColorBridge*>("Wabcut", "Color");
    REQUIRE(controller);
    REQUIRE(color);
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa|
    )");
    controller->resetProject(tl.state, tl.seq);
    if (!dark) {
        shell.setAppearance(false, "ultraviolet");
        REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
    }
    shell.window->setProperty("workspace", 3); // the Color workspace
    QQuickItem* panel = nullptr;
    REQUIRE(waitFor([&] {
        panel = findNamed(shell.window->contentItem(), QStringLiteral("colorPanel"));
        return panel != nullptr;
    }));
    QQuickItem* gear = findNamed(panel, QStringLiteral("colorManagementButton"));
    REQUIRE(gear);
    REQUIRE(QMetaObject::invokeMethod(gear, "clicked"));
    QObject* dialog = panel->findChild<QObject*>(QStringLiteral("colorManagementDialog"));
    REQUIRE(dialog);
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
    auto* content = dialog->property("contentItem").value<QQuickItem*>();
    REQUIRE(content);
    // Direct Rec.709: no gamut compression or SDR Conform to set; the output colour space is there.
    QQuickItem* gamut = findNamed(content, QStringLiteral("colorGamutCompression"));
    QQuickItem* conform = findNamed(content, QStringLiteral("colorSdrConform"));
    QQuickItem* output = findNamed(content, QStringLiteral("colorOutputSpace"));
    REQUIRE(gamut);
    REQUIRE(conform);
    REQUIRE(output);
    CHECK_FALSE(gamut->isVisible());
    CHECK_FALSE(conform->isVisible());
    CHECK(output->isVisible());
    CHECK(output->property("text").toString() == QStringLiteral("Automatic"));

    dialog->setProperty("mode", QStringLiteral("hdr-pq"));
    dialog->setProperty("gamutCompression", true);
    dialog->setProperty("gamutStrength", 0.7);
    dialog->setProperty("outputSpace", QStringLiteral("rec2020"));
    dialog->setProperty("conformEnabled", true);
    dialog->setProperty("conformOperator", QStringLiteral("reinhard"));
    dialog->setProperty("conformGamut", QStringLiteral("saturation"));
    dialog->setProperty("conformPeak", 203.0);
    dialog->setProperty("conformLegal", true);
    spin(200);
    CHECK(gamut->isVisible());
    CHECK(conform->isVisible());
    CHECK(output->property("text").toString() == QStringLiteral("Rec.2020 (SDR)"));
    QQuickItem* strength = findNamed(content, QStringLiteral("colorGamutStrength"));
    REQUIRE(strength);
    CHECK(strength->isVisible());
    QQuickItem* operators = findNamed(content, QStringLiteral("colorConformOperator"));
    REQUIRE(operators);
    CHECK(operators->property("currentIndex").toInt() == 2);
    // The dialog's picture, all of it on screen.
    QQuickItem* scroller = findNamed(content, QStringLiteral("colorManagementScroller"));
    REQUIRE(scroller);
    CHECK(findNamed(content, QStringLiteral("colorConformLegal"))->isVisible());
    spin(200);
    {
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        auto* card = qobject_cast<QQuickItem*>(content->parentItem());
        QQuickItem* shot = card ? card : content;
        const double dpr = shell.window->effectiveDevicePixelRatio();
        const QRectF rect = shot->mapRectToScene(QRectF(-24, -24, shot->width() + 48, shot->height() + 48));
        REQUIRE(image.copy(QRectF(rect.topLeft() * dpr, rect.size() * dpr).toRect())
                    .save(outputDir() + (dark ? QStringLiteral("/color-management-hdr-finishing-dark.png")
                                              : QStringLiteral("/color-management-hdr-finishing-light.png"))));
    }

    // OK: one undo step for all of it; the badge says gamut compression is on.
    const std::size_t before = controller->session().undoCount();
    REQUIRE(QMetaObject::invokeMethod(dialog, "apply"));
    const ColorManagement& m = controller->snapshot()->colorManagement;
    CHECK(m.mode == ColorManagementMode::HdrPq);
    CHECK(m.gamutCompression);
    CHECK(m.gamutCompressionStrength == 0.7);
    CHECK(m.outputSpace == OutputColorSpace::Rec2020);
    CHECK(m.sdrConform.enabled);
    CHECK(m.sdrConform.toneMap == ToneMapOperator::Reinhard);
    CHECK(m.sdrConform.gamut == GamutMapping::Saturation);
    CHECK(m.sdrConform.targetPeakNits == 203.0);
    CHECK(m.sdrConform.legalRange);
    CHECK(controller->session().undoCount() == before + 1);
    REQUIRE(waitFor([&] { return color->monitorLabel().contains(QStringLiteral("gamut compression")); }));
    CHECK(color->colorManagement().value(QStringLiteral("conformable")).toBool());
    CHECK(color->colorManagement().value(QStringLiteral("outputChain")).toString().contains(QStringLiteral("SDR Conform")));

    // The Color panel's SDR Conform preview: before / after on the monitor, said by the badge; a setup without SDR
    // Conform ends it.
    QQuickItem* tabs = findNamed(panel, QStringLiteral("conformPreviewTabs"));
    REQUIRE(tabs);
    color->setConformPreview(1);
    CHECK(color->conformPreview() == 1);
    REQUIRE(waitFor([&] { return color->monitorLabel().contains(QStringLiteral("SDR Conform before / after")); }));
    REQUIRE(color->setColorManagement({{QStringLiteral("mode"), QStringLiteral("rec709")}}));
    REQUIRE(waitFor([&] { return color->conformPreview() == 0; }));
    // Undo goes back to Direct PQ with HDR finishing, and again to the default.
    REQUIRE(controller->undo());
    CHECK(controller->snapshot()->colorManagement.sdrConform.enabled);
    REQUIRE(controller->undo());
    CHECK(controller->snapshot()->colorManagement.isDefault());
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

} // namespace

TEST_CASE("Color Management dialog HDR finishing in one undo step with its screenshot in the dark theme (0.6)",
          "[app][color][colormanagement][screenshots]") {
    hdrFinishingDialog(true);
}

TEST_CASE("Color Management dialog HDR finishing screenshot in the light theme (0.6)",
          "[app][color][colormanagement][screenshots]") {
    hdrFinishingDialog(false);
}

TEST_CASE("Export drawer: the Color space row sets the output colour space and the details show the chain (0.6)",
          "[app][color][export][screenshots]") {
    // #72 (3): the output colour space as an export choice, beside the sequence's Color Management one.
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(controller);
    Timeline tl("V1 |Aaaaaaaaaa|\nA1 |Aaaaaaaaaa|");
    controller->resetProject(tl.state, tl.seq);
    shell.window->setProperty("workspace", 2); // Export
    spin(200);
    QQuickItem* content = shell.window->contentItem();
    QQuickItem* drawer = nullptr;
    REQUIRE(waitFor([&] {
        drawer = findNamed(content, QStringLiteral("exportSettingsDrawer"));
        return drawer != nullptr && drawer->isVisible();
    }));
    drawer->setProperty("open", true);
    spin(150);
    QQuickItem* tabs = findNamed(drawer, QStringLiteral("exportOutputSpaceTabs"));
    REQUIRE(tabs);
    CHECK(tabs->isVisible());
    CHECK(tabs->property("currentIndex").toInt() == 0); // Auto
    const auto overrides = [&] { return drawer->property("overrides").toMap(); };
    const auto details = [&] { return drawer->property("summary").toMap().value(QStringLiteral("dynamicRange")).toString(); };
    CHECK_FALSE(details().contains(QStringLiteral("P3-D65")));

    REQUIRE(QMetaObject::invokeMethod(tabs, "activated", Q_ARG(int, 3)));
    REQUIRE(waitFor([&] { return overrides().value(QStringLiteral("outputSpace")).toString() == QStringLiteral("p3-d65"); }));
    REQUIRE(waitFor([&] { return details().contains(QStringLiteral("P3-D65")); }));
    // The screenshot. The panel's drawer opens with a height animation that an offscreen window that is never shown
    // does not advance, and it sits low in a scrolling column, so a second drawer made open (no animation) with the
    // same summary and overrides is drawn over the window's top left and grabbed.
    QQmlComponent component(&shell.engine, "Wabcut", "ExportSettingsDrawer");
    INFO(component.errorString().toStdString());
    QObject* made = component.createWithInitialProperties(
        {{QStringLiteral("open"), true}, {QStringLiteral("summary"), drawer->property("summary")},
         {QStringLiteral("overrides"), drawer->property("overrides")}, {QStringLiteral("width"), 600}});
    auto* shot = qobject_cast<QQuickItem*>(made);
    REQUIRE(shot);
    shot->setParentItem(shell.window->contentItem());
    shot->setZ(10'000);
    shot->setHeight(shot->implicitHeight());
    spin(300);
    QQuickItem* shotTabs = findNamed(shot, QStringLiteral("exportOutputSpaceTabs"));
    REQUIRE(shotTabs);
    CHECK(shotTabs->isVisible());
    CHECK(shotTabs->property("currentIndex").toInt() == 3); // P3-D65, from the overrides
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    const double dpr = shell.window->effectiveDevicePixelRatio();
    const QRectF rect = QRectF(0, 0, shot->width(), std::min<double>(shot->height(), shell.window->height()));
    REQUIRE(image.copy(QRectF(rect.topLeft() * dpr, rect.size() * dpr).toRect()).save(outputDir() + QStringLiteral("/export-drawer-color-space.png")));
    delete shot;

    REQUIRE(QMetaObject::invokeMethod(tabs, "activated", Q_ARG(int, 2)));
    REQUIRE(waitFor([&] { return overrides().value(QStringLiteral("outputSpace")).toString() == QStringLiteral("rec2020"); }));
    REQUIRE(QMetaObject::invokeMethod(tabs, "activated", Q_ARG(int, 0)));
    REQUIRE(waitFor([&] { return !overrides().contains(QStringLiteral("outputSpace")); }));
    const QStringList messages = takeMessages();
    INFO(messages.join(QLatin1Char('\n')).toStdString());
    CHECK(messages.isEmpty());
}

// 0.7 (card #85): the LUT menu groups LUTs by folder.
TEST_CASE("LUT menu: LUTs imported from folders are submenus and the loose ones stay in the list (card 85 screenshots)",
          "[app][color][lut][qml][screenshots]") {
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
    ProjectState state = tl.state;
    const auto add = [&](const char* name, const char* folder) {
        ProjectAsset lut;
        lut.kind = std::string(kAssetKindLut);
        lut.name = name;
        lut.folder = folder;
        lut.locator.path = std::string("C:/luts/") + name;
        auto added = edit::addAsset(state, std::move(lut));
        REQUIRE(added);
        state = added->state;
    };
    add("Loose.cube", "");
    add("2383.cube", "Film/Kodak");
    add("3510.cube", "Film/Fuji");
    add("2395.cube", "Film/Kodak");
    add("Teal.cube", "Looks");
    controller->resetProject(state, tl.seq);
    spin(100);
    shell.window->setProperty("workspace", 3); // the Color workspace
    QQuickItem* panel = nullptr;
    REQUIRE(waitFor([&] {
        panel = findNamed(shell.window->contentItem(), QStringLiteral("colorPanel"));
        return panel != nullptr;
    }));
    QObject* menu = panel->findChild<QObject*>(QStringLiteral("lutMenu"));
    REQUIRE(menu);

    // The bridge lists each LUT with its folder.
    int inFolders = 0;
    for (const QVariant& choice : color->lutChoices()) {
        const QVariantMap c = choice.toMap();
        if (c.value(QStringLiteral("kind")).toString() == QLatin1String("file") && !c.value(QStringLiteral("folder")).toString().isEmpty())
            ++inFolders;
    }
    CHECK(inFolders == 4);

    const auto rows = [&] {
        REQUIRE(QMetaObject::invokeMethod(menu, "rebuild"));
        const int count = menu->property("count").toInt();
        QStringList names;
        for (int i = 0; i < count; ++i) {
            QQuickItem* item = nullptr;
            REQUIRE(QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, i)));
            REQUIRE(item);
            QObject* sub = item->property("subMenu").value<QObject*>();
            names << (sub ? sub->objectName() : item->objectName());
        }
        return names;
    };
    // Creative > Look (the default slot): the built-in looks and the loose LUT in the list, then one submenu per folder,
    // sorted, with the import rows last.
    QStringList names = rows();
    INFO(names.join(QStringLiteral(", ")).toStdString());
    CHECK(menu->property("folderNames").toStringList() ==
          QStringList{QStringLiteral("Film/Fuji"), QStringLiteral("Film/Kodak"), QStringLiteral("Looks")});
    REQUIRE(names.size() >= 8);
    CHECK(names.first() == QStringLiteral("lutMenuNone"));
    CHECK(names.mid(names.size() - 6, 3) ==
          QStringList{QStringLiteral("lutMenuFolder_Film/Fuji"), QStringLiteral("lutMenuFolder_Film/Kodak"), QStringLiteral("lutMenuFolder_Looks")});
    CHECK(names.at(names.size() - 1) == QStringLiteral("lutMenuImportFolder"));
    CHECK(names.at(names.size() - 2) == QStringLiteral("lutMenuImport"));
    CHECK(names.filter(QStringLiteral("lutMenuRow_asset:")).size() == 1); // the loose LUT only, the others are in submenus
    CHECK(names.filter(QStringLiteral("lutMenuRow_builtin:")).size() == 5); // the five looks
    // A folder's submenu holds its LUTs.
    QQuickItem* kodakRow = nullptr;
    for (int i = 0; i < menu->property("count").toInt(); ++i) {
        QQuickItem* item = nullptr;
        REQUIRE(QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, i)));
        QObject* sub = item->property("subMenu").value<QObject*>();
        if (sub && sub->objectName() == QLatin1String("lutMenuFolder_Film/Kodak"))
            kodakRow = item;
    }
    REQUIRE(kodakRow);
    CHECK(kodakRow->property("subMenu").value<QObject*>()->property("count").toInt() == 2);
    // Rebuilding again does not pile rows up.
    CHECK(rows() == names);

    // Basic > Input LUT: the camera input LUTs instead of the looks; the project's LUTs are offered there too.
    menu->setProperty("slot", QStringLiteral("input"));
    names = rows();
    CHECK(names.filter(QStringLiteral("lutMenuRow_builtin:")).size() == 2);
    CHECK(menu->property("folderNames").toStringList().size() == 3);
    menu->setProperty("slot", QStringLiteral("look"));

    // The open menu, in both themes.
    REQUIRE(QMetaObject::invokeMethod(menu, "openAt", Q_ARG(QVariant, QVariant::fromValue<QObject*>(panel))));
    REQUIRE(waitFor([&] { return menu->property("opened").toBool(); }, 5000));
    for (const bool dark : {true, false}) {
        shell.setAppearance(dark, "ultraviolet");
        spin(80);
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        REQUIRE(image.save(outputDir() + QStringLiteral("/lut-menu-folders-") + (dark ? QStringLiteral("dark") : QStringLiteral("light")) +
                           QStringLiteral(".png")));
    }
    QMetaObject::invokeMethod(menu, "close");
    shell.setAppearance(true, "ultraviolet");
}
