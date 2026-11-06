// 0.7 graphics in the app, offscreen: keyed template fields (card 71: the stopwatch on a field of an instance, keys at the
// playhead, chips that go to them, the draw list following them), text styles on part of a text (card 81: the editor
// keeps run styles when the text is committed, Text styles take a range of the selection, the Library's style cards
// reorder) and the template browser's folders, sort and folder install (card 87). Each change is one undo step. Then the
// panels in the real shell with screenshots in the dark and light themes.

#include "A11yAudit.h"
#include "BridgeIds.h"
#include "GraphicToolsBridge.h"
#include "GraphicsBridge.h"
#include "ProjectController.h"
#include "ShellFixture.h"
#include "TimelineFixture.h"

#include <wabcut/model/GraphicJson.h>
#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/GraphicResolve.h>
#include <wabcut/model/Graphics.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QColor>
#include <QDir>
#include <QFile>
#include <QQmlComponent>
#include <QQmlProperty>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QUrl>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using Catch::Approx;

namespace {

constexpr std::string_view kPicture = R"(
    V2     |....................................................................................................|
    V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
)";

Ticks seconds(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

std::filesystem::path pathOf(const QTemporaryDir& dir) { return std::filesystem::path(dir.path().toStdU16String()); }

/// The gaming fixtures (kill-streak has a text and a colour field) copied into a user's template library.
void copyFixtures(const QTemporaryDir& dir) {
    const QDir from(QDir(QStringLiteral(WABCUT_APP_TEST_DIR)).filePath(QStringLiteral("data/callouts")));
    const QStringList names = from.entryList({QStringLiteral("*.wabcut-template.json")}, QDir::Files);
    REQUIRE_FALSE(names.isEmpty());
    for (const QString& name : names)
        REQUIRE(QFile::copy(from.filePath(name), QDir(dir.path()).filePath(name)));
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

QVariantMap fieldOf(const GraphicsBridge& graphics, const QString& id) {
    for (const QVariant& f : graphics.fields()) {
        if (f.toMap().value(QStringLiteral("id")).toString() == id)
            return f.toMap();
    }
    return {};
}

/// A kill-streak instance on V2 at 6 s (4 s long): fields title (text), count (text), bannerColor (colour).
struct Rig {
    Timeline tl{kPicture};
    ProjectController controller;
    GraphicsBridge graphics;
    GraphicToolsBridge tools;
    QTemporaryDir library;
    ClipId clip;

    Rig() {
        REQUIRE(library.isValid());
        copyFixtures(library);
        controller.resetProject(tl.state, tl.seq);
        graphics.setUserLibrary(pathOf(library));
        graphics.setController(&controller);
        tools.setController(&controller);
        controller.setPlayhead(seconds(6));
        REQUIRE(graphics.instantiateAtPlayhead(QStringLiteral("user/gaming-kill-streak")).isEmpty());
        clip = controller.primaryClip();
        REQUIRE(clip.isValid());
    }
    Clip placed() const { return controller.snapshot()->findSequence(tl.seq)->findClip(clip)->clip(); }
    Graphic graphic() const { return *placed().graphic(); }
    std::size_t undo() { return controller.session().undoCount(); }
};

} // namespace

TEST_CASE("Template fields animate from the Essential Graphics stopwatch: keys at the playhead, one undo step each (card 71)",
          "[app][graphics][templates][keys]") {
    Rig r;
    const std::size_t start = r.undo();
    const Ticks clipStart = r.placed().start();
    QVariantMap banner = fieldOf(r.graphics, QStringLiteral("bannerColor"));
    REQUIRE_FALSE(banner.isEmpty());
    CHECK(banner.value(QStringLiteral("animatable")).toBool());
    CHECK_FALSE(banner.value(QStringLiteral("keyed")).toBool());
    CHECK(banner.value(QStringLiteral("keys")).toList().isEmpty());

    // The stopwatch: one key with the value shown, at the playhead inside the clip.
    CHECK(r.graphics.animateField(QStringLiteral("bannerColor"), true).isEmpty());
    CHECK(r.undo() == start + 1);
    banner = fieldOf(r.graphics, QStringLiteral("bannerColor"));
    CHECK(banner.value(QStringLiteral("keyed")).toBool());
    REQUIRE(banner.value(QStringLiteral("keys")).toList().size() == 1);
    CHECK(banner.value(QStringLiteral("keys")).toList()[0].toMap().value(QStringLiteral("seconds")).toDouble() == Approx(0.0).margin(0.001));
    // Turning it on again is nothing; the playhead outside the clip holds the key inside it.
    CHECK(r.graphics.animateField(QStringLiteral("bannerColor"), true).isEmpty());
    CHECK(r.undo() == start + 1);

    // Another moment: setting the field sets the key there (typing or dragging merges into one step), and it blends between.
    r.controller.setPlayhead(clipStart + seconds(2));
    CHECK(r.graphics.setField(QStringLiteral("bannerColor"), QStringLiteral("#FF0000")).isEmpty());
    CHECK(r.graphics.setField(QStringLiteral("bannerColor"), QStringLiteral("#0000FF")).isEmpty());
    r.graphics.endEdit();
    CHECK(r.undo() == start + 2);
    banner = fieldOf(r.graphics, QStringLiteral("bannerColor"));
    REQUIRE(banner.value(QStringLiteral("keys")).toList().size() == 2);
    CHECK(banner.value(QStringLiteral("keys")).toList()[1].toMap().value(QStringLiteral("seconds")).toDouble() == Approx(2.0).margin(0.01));
    CHECK(banner.value(QStringLiteral("value")).toString().toUpper() == QStringLiteral("#0000FF"));
    // The fields follow the playhead: at the first key the first colour, in between a mix.
    r.controller.setPlayhead(clipStart);
    const QString first = fieldOf(r.graphics, QStringLiteral("bannerColor")).value(QStringLiteral("value")).toString();
    r.controller.setPlayhead(clipStart + seconds(1));
    const QString middle = fieldOf(r.graphics, QStringLiteral("bannerColor")).value(QStringLiteral("value")).toString();
    CHECK(first != middle);
    CHECK(middle.toUpper() != QStringLiteral("#0000FF"));
    // ... and the picture follows the keys: the draw list at 1 s is neither colour.
    const Graphic g = r.graphic();
    const TemplateField* field = nullptr;
    for (const TemplateField& f : g.instance->fields)
        field = f.id == "bannerColor" ? &f : field;
    REQUIRE(field);
    const double blueAtMiddle = fieldValueAt(*field, seconds(1)).color.rgba[2];
    CHECK(blueAtMiddle > 0.0);
    CHECK(blueAtMiddle < 1.0);

    // A key chip goes to its key; x removes it; the stopwatch off keeps the value at the playhead.
    r.controller.setPlayhead(clipStart + seconds(3));
    r.graphics.goToFieldKey(QStringLiteral("bannerColor"), 1);
    CHECK(r.controller.playhead() == clipStart + seconds(2));
    r.graphics.goToFieldKey(QStringLiteral("bannerColor"), 9); // no such key: the playhead stays
    CHECK(r.controller.playhead() == clipStart + seconds(2));
    CHECK(r.graphics.removeFieldKey(QStringLiteral("bannerColor"), 1).isEmpty());
    CHECK(r.undo() == start + 3);
    CHECK(fieldOf(r.graphics, QStringLiteral("bannerColor")).value(QStringLiteral("keys")).toList().size() == 1);
    CHECK_FALSE(r.graphics.removeFieldKey(QStringLiteral("bannerColor"), 4).isEmpty());
    CHECK(r.graphics.animateField(QStringLiteral("bannerColor"), false).isEmpty());
    CHECK_FALSE(fieldOf(r.graphics, QStringLiteral("bannerColor")).value(QStringLiteral("keyed")).toBool());
    CHECK_FALSE(hasFieldKeys(r.graphic()));
    // Undo walks back through them one at a time.
    REQUIRE(r.controller.undo());
    CHECK(hasFieldKeys(r.graphic()));

    // A text field switches at its keys; a length cannot be animated.
    r.controller.setPlayhead(clipStart);
    CHECK(r.graphics.animateField(QStringLiteral("title"), true).isEmpty());
    r.controller.setPlayhead(clipStart + seconds(2));
    CHECK(r.graphics.setField(QStringLiteral("title"), QStringLiteral("LATER")).isEmpty());
    r.graphics.endEdit();
    const Graphic keyed = r.graphic();
    const DrawList early = resolveGraphic(keyed, seconds(1));
    const DrawList late = resolveGraphic(keyed, seconds(3));
    const auto textOf = [](const DrawList& list) {
        for (const DrawItem& item : list.items) {
            if (item.layer == 2 && !item.text.runs.empty())
                return item.text.runs.front().text;
        }
        return std::string();
    };
    CHECK(textOf(early) == "KILL STREAK");
    CHECK(textOf(late) == "LATER");
    CHECK_FALSE(r.graphics.animateField(QStringLiteral("no-such-field"), true).isEmpty());
}

TEST_CASE("Typing in a styled text keeps its run styles; a text style styles the selected characters; the cards reorder (card 81)",
          "[app][graphics][textStyles][runs]") {
    Rig r;
    // The title layer: "KILL STREAK". Two styles from layers' looks.
    REQUIRE(r.graphics.selectedLayer() == 2);
    REQUIRE(r.tools.createTextStyle(clipKey(r.clip), 2, QStringLiteral("Plain")).isEmpty());
    {
        ProjectTextStyle big = *r.controller.snapshot()->findTextStyle("Plain");
        big.name = "Big";
        big.style.fontSize = 150.0;
        big.style.fill.rgba = {1.0, 0.8, 0.0, 1.0};
        REQUIRE(r.controller.session().apply("Style", [&](const ProjectState& s) { return edit::saveTextStyle(s, big); }));
    }
    const std::size_t before = r.undo();
    // "STREAK" is characters 5..11 of the title.
    CHECK(r.tools.applyTextStyleToRange(clipKey(r.clip), 2, 5, 11, QStringLiteral("Big")).isEmpty());
    CHECK(r.undo() == before + 1);
    Graphic shown = r.graphic();
    const GraphicLayer* layer = shown.findLayer(2);
    REQUIRE(layer != nullptr);
    REQUIRE(layer->text.runs.size() == 2);
    CHECK(layer->text.runs[0].text == "KILL ");
    CHECK(layer->text.runs[1].text == "STREAK");
    CHECK(layer->text.runs[1].style.fontSize == 150.0);
    CHECK(layer->textStyle.empty() == false); // Plain, linked when it was created: styling a range does not change that

    // The editor's commit goes through setText: the styled word stays styled through more typing.
    CHECK(r.graphics.setText(2, QStringLiteral("KILL STREAKS")).isEmpty());
    r.graphics.endEdit();
    shown = r.graphic();
    layer = shown.findLayer(2);
    REQUIRE(layer->text.runs.size() == 2);
    CHECK(layer->text.runs[1].text == "STREAKS");
    CHECK(layer->text.runs[1].style.fontSize == 150.0);
    CHECK(r.graphics.setText(2, QStringLiteral("DOUBLE KILL STREAKS")).isEmpty());
    r.graphics.endEdit();
    shown = r.graphic();
    layer = shown.findLayer(2);
    CHECK(layer->text.text() == "DOUBLE KILL STREAKS");
    CHECK(layer->text.runs.back().style.fontSize == 150.0);
    // A selection past the text, no such style, a layer that is not text: refused with the reason, nothing changes.
    const std::size_t stepsNow = r.undo();
    CHECK_FALSE(r.tools.applyTextStyleToRange(clipKey(r.clip), 2, 30, 40, QStringLiteral("Big")).isEmpty());
    CHECK_FALSE(r.tools.applyTextStyleToRange(clipKey(r.clip), 2, 0, 4, QStringLiteral("Nope")).isEmpty());
    CHECK_FALSE(r.tools.applyTextStyleToRange(clipKey(r.clip), 1, 0, 1, QStringLiteral("Big")).isEmpty());
    CHECK(r.undo() == stepsNow);

    // The cards: Plain, Big -> Big, Plain (one undo step), and the order the items list follows.
    QVariantList items = r.tools.textStyleItems();
    REQUIRE(items.size() == 2);
    CHECK(items[0].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Plain"));
    CHECK(r.tools.moveTextStyle(QStringLiteral("Big"), 0).isEmpty());
    CHECK(r.undo() == stepsNow + 1);
    items = r.tools.textStyleItems();
    CHECK(items[0].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Big"));
    CHECK_FALSE(r.tools.moveTextStyle(QStringLiteral("Missing"), 0).isEmpty());
    REQUIRE(r.controller.undo());
    CHECK(r.tools.textStyleItems()[0].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Plain"));
}

TEST_CASE("The template browser files templates in folders and installs a folder of template files (card 87)",
          "[app][graphics][templates][folders]") {
    Rig r;
    // Save the instance's graphic as two project templates and file them.
    REQUIRE(r.graphics.saveTemplate(QStringLiteral("streak-a"), QStringLiteral("Streak A"), {}, QStringLiteral("project"), {}).isEmpty());
    REQUIRE(r.graphics.saveTemplate(QStringLiteral("streak-b"), QStringLiteral("Streak B"), {}, QStringLiteral("project"), {}).isEmpty());
    const auto folderOf = [&](const QString& key) {
        for (const QVariant& t : r.graphics.templates()) {
            if (t.toMap().value(QStringLiteral("key")).toString() == key)
                return t.toMap().value(QStringLiteral("folder")).toString();
        }
        return QStringLiteral("(missing)");
    };
    CHECK(folderOf(QStringLiteral("project/streak-a")).isEmpty());
    const std::size_t before = r.undo();
    CHECK(r.graphics.setTemplateFolder(QStringLiteral("project/streak-a"), QStringLiteral("  Titles/Streaks ")).isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(folderOf(QStringLiteral("project/streak-a")) == QStringLiteral("Titles/Streaks"));
    CHECK(r.graphics.setTemplateFolder(QStringLiteral("project/streak-a"), QString()).isEmpty()); // out of the folder
    CHECK(folderOf(QStringLiteral("project/streak-a")).isEmpty());
    // The user's library: the file is rewritten.
    CHECK(r.graphics.setTemplateFolder(QStringLiteral("user/gaming-kill-streak"), QStringLiteral("Gaming")).isEmpty());
    CHECK(folderOf(QStringLiteral("user/gaming-kill-streak")) == QStringLiteral("Gaming"));
    // Refusals: a built-in template, a bad folder.
    CHECK_FALSE(r.graphics.setTemplateFolder(QStringLiteral("builtin/gaming.lowerThird"), QStringLiteral("x")).isEmpty());
    CHECK_FALSE(r.graphics.setTemplateFolder(QStringLiteral("project/streak-b"), QStringLiteral("a/b/c/d")).isEmpty());

    // Install folder...: a tree of template files; each template is filed under the folder it was found in.
    QTemporaryDir source;
    REQUIRE(source.isValid());
    const std::filesystem::path root = pathOf(source) / "Lower thirds";
    const std::filesystem::path sub = root / "Dark";
    std::filesystem::create_directories(sub);
    const GraphicTemplate base = *r.controller.snapshot()->findTemplate("streak-b");
    const auto write = [&](const std::filesystem::path& where, const char* id, const char* name) {
        GraphicTemplate t = base;
        t.id = id;
        t.name = name;
        t.folder.clear();
        std::ofstream out(where / (std::string(id) + ".wabcut-template.json"), std::ios::binary);
        out << io::templateFileToJson(t);
    };
    write(root, "pack-one", "Pack one");
    write(sub, "pack-two", "Pack two");
    {
        std::ofstream broken(root / "broken.wabcut-template.json");
        broken << "{";
    }
    const std::size_t steps = r.undo();
    const QVariantMap result = r.graphics.importTemplateFolder(QUrl::fromLocalFile(QString::fromStdU16String(root.u16string())), QStringLiteral("project"));
    CHECK(result.value(QStringLiteral("error")).toString().isEmpty());
    CHECK(result.value(QStringLiteral("imported")).toInt() == 2);
    CHECK(result.value(QStringLiteral("skipped")).toInt() == 1);
    CHECK(r.undo() == steps + 1);
    CHECK(folderOf(QStringLiteral("project/pack-one")) == QStringLiteral("Lower thirds"));
    CHECK(folderOf(QStringLiteral("project/pack-two")) == QStringLiteral("Lower thirds/Dark"));
    CHECK_FALSE(r.graphics.importTemplateFolder(QUrl::fromLocalFile(QStringLiteral("Z:/no/such/folder")), QStringLiteral("project"))
                    .value(QStringLiteral("error"))
                    .toString()
                    .isEmpty());
    REQUIRE(r.controller.undo());
    r.graphics.refreshTemplates();
    CHECK(folderOf(QStringLiteral("project/pack-one")) == QStringLiteral("(missing)"));
}

TEST_CASE("Essential Graphics in the shell: keyed fields, folders and sort in the browser, the style bar of the text editor (screenshots)",
          "[app][graphics][qml][screenshots]") {
    A11yScope a11y; // before the shell, so the controls register their names
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    QTemporaryDir library;
    REQUIRE(library.isValid());
    copyFixtures(library);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* graphics = shell.engine.singletonInstance<GraphicsBridge*>("Wabcut", "Graphics");
    REQUIRE(controller);
    REQUIRE(graphics);
    graphics->setUserLibrary(pathOf(library));
    Timeline tl(kPicture);
    controller->resetProject(tl.state, tl.seq);
    controller->setPlayhead(seconds(6));
    REQUIRE(graphics->instantiateAtPlayhead(QStringLiteral("user/gaming-kill-streak")).isEmpty());
    const ClipId clip = controller->primaryClip();
    const Ticks clipStart = controller->currentSequence()->findClip(clip)->clip().start();
    REQUIRE(graphics->saveTemplate(QStringLiteral("streak-a"), QStringLiteral("Streak A"), {}, QStringLiteral("project"), {}).isEmpty());
    REQUIRE(graphics->saveTemplate(QStringLiteral("streak-b"), QStringLiteral("Streak B"), {}, QStringLiteral("project"), {}).isEmpty());
    REQUIRE(graphics->setTemplateFolder(QStringLiteral("project/streak-a"), QStringLiteral("Lower thirds")).isEmpty());
    REQUIRE(graphics->setTemplateFolder(QStringLiteral("project/streak-b"), QStringLiteral("Lower thirds/Dark")).isEmpty());
    // Two keys on the banner colour and one on the title.
    REQUIRE(graphics->animateField(QStringLiteral("bannerColor"), true).isEmpty());
    controller->setPlayhead(clipStart + seconds(2));
    REQUIRE(graphics->setField(QStringLiteral("bannerColor"), QStringLiteral("#FF3366")).isEmpty());
    graphics->endEdit();

    QQmlComponent component(&shell.engine);
    component.setData("import Wabcut\nEssentialGraphicsPanel {}\n", QUrl(QStringLiteral("qrc:/v07-graphics/Host.qml")));
    std::unique_ptr<QObject> object(component.create());
    INFO(component.errorString().toStdString());
    REQUIRE(object);
    auto* panel = qobject_cast<QQuickItem*>(object.get());
    REQUIRE(panel);
    object.release();
    panel->setParentItem(shell.window->contentItem());
    panel->setPosition(QPointF(40, 40));
    panel->setSize(QSizeF(460, 900));
    panel->setZ(1000);
    std::unique_ptr<QQuickItem> own(panel);

    const auto grabPanel = [&](const QString& name) {
        spin(60);
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        const double dpr = image.devicePixelRatio();
        const QRectF r = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
        REQUIRE(image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect()).save(QDir(outputDir()).filePath(name)));
    };

    // Edit: the stopwatch is on for the banner colour with its two keys as chips (a title is not keyed yet).
    QQuickItem* edit = findNamed(panel, QStringLiteral("graphicsEditPage"));
    REQUIRE(edit);
    REQUIRE(waitFor([&] { return edit->isVisible(); }, 3000));
    REQUIRE_FALSE(shell.grab().isNull());
    QQuickItem* stopwatch = findNamed(panel, QStringLiteral("graphicFieldStopwatch_bannerColor"));
    REQUIRE(stopwatch);
    CHECK(stopwatch->isVisible());
    CHECK(stopwatch->property("checked").toBool());
    CHECK(findNamed(panel, QStringLiteral("graphicFieldKey_bannerColor_0")));
    CHECK(findNamed(panel, QStringLiteral("graphicFieldKey_bannerColor_1")));
    QQuickItem* titleStopwatch = findNamed(panel, QStringLiteral("graphicFieldStopwatch_title"));
    REQUIRE(titleStopwatch);
    CHECK_FALSE(titleStopwatch->property("checked").toBool());
    // Clicking the title's stopwatch animates it: a key chip appears.
    REQUIRE(QMetaObject::invokeMethod(titleStopwatch, "clicked"));
    REQUIRE(waitFor([&] { return findNamed(panel, QStringLiteral("graphicFieldKey_title_0")) != nullptr; }, 3000));
    const QStringList editIssues = auditAccessibility(panel);
    INFO(editIssues.join(QLatin1Char('\n')).toStdString());
    CHECK(editIssues.isEmpty());
    for (const bool dark : {true, false}) {
        shell.setAppearance(dark, "ultraviolet");
        REQUIRE(shell.settle(dark, shell.themeColor("accentFill")));
        grabPanel(QStringLiteral("graphics-keyed-fields-%1.png").arg(dark ? QStringLiteral("dark") : QStringLiteral("light")));
    }
    shell.setAppearance(true, "ultraviolet");

    // Browse (nothing selected): folders, the sort button, Install folder...
    controller->clearSelection();
    QQuickItem* browse = findNamed(panel, QStringLiteral("graphicsBrowsePage"));
    REQUIRE(browse);
    REQUIRE(waitFor([&] { return browse->isVisible(); }, 3000));
    REQUIRE_FALSE(shell.grab().isNull());
    QQuickItem* sort = findNamed(panel, QStringLiteral("graphicsSort"));
    REQUIRE(sort);
    CHECK(sort->property("text").toString().contains(QStringLiteral("Name")));
    REQUIRE(findNamed(panel, QStringLiteral("graphicsInstallFolder")));
    REQUIRE(findNamed(panel, QStringLiteral("graphicsInstallFolderUser")));
    QQuickItem* fold = findNamed(panel, QStringLiteral("graphicsProjectTemplatesFolder_Lower thirds"));
    REQUIRE(fold);
    CHECK(findNamed(panel, QStringLiteral("graphicsProjectTemplatesFolder_Lower thirds/Dark")));
    CHECK(findNamed(panel, QStringLiteral("graphicsProjectTemplatesFolderCards_Lower thirds"))->isVisible());
    // A folder closes (its cards hide) and opens again.
    QQuickItem* toggle = findNamed(panel, QStringLiteral("graphicsProjectTemplatesFolderToggle_Lower thirds"));
    REQUIRE(toggle);
    REQUIRE(QMetaObject::invokeMethod(toggle, "clicked"));
    REQUIRE(waitFor([&] { return !findNamed(panel, QStringLiteral("graphicsProjectTemplatesFolderCards_Lower thirds"))->isVisible(); }, 3000));
    REQUIRE(QMetaObject::invokeMethod(toggle, "clicked"));
    REQUIRE(waitFor([&] { return findNamed(panel, QStringLiteral("graphicsProjectTemplatesFolderCards_Lower thirds"))->isVisible(); }, 3000));
    // The sort order: the page's sort key changes the order of the user's group.
    browse->setProperty("sortKey", QStringLiteral("nameDesc"));
    REQUIRE(waitFor([&] { return sort->property("text").toString().contains(QStringLiteral("Z to A")); }, 3000));
    browse->setProperty("sortKey", QStringLiteral("name"));
    REQUIRE_FALSE(shell.grab().isNull());
    if (QQuickItem* flick = findNamed(panel, QStringLiteral("graphicsBrowseFlick"))) {
        if (QQuickItem* project = findNamed(panel, QStringLiteral("calloutsProject")))
            flick->setProperty("contentY", project->y() - 8.0); // the folders are what the shot is of
    }
    REQUIRE_FALSE(shell.grab().isNull());
    const QStringList browseIssues = auditAccessibility(panel);
    INFO(browseIssues.join(QLatin1Char('\n')).toStdString());
    CHECK(browseIssues.isEmpty());
    for (const bool dark : {true, false}) {
        shell.setAppearance(dark, "ultraviolet");
        REQUIRE(shell.settle(dark, shell.themeColor("accentFill")));
        grabPanel(QStringLiteral("graphics-browse-folders-%1.png").arg(dark ? QStringLiteral("dark") : QStringLiteral("light")));
    }
    shell.setAppearance(true, "ultraviolet");

    // Styles: the project's text styles as cards, reordered from the card menu's command; screenshots.
    auto* tools = shell.engine.singletonInstance<GraphicToolsBridge*>("Wabcut", "GraphicTools");
    REQUIRE(tools);
    REQUIRE(tools->createTextStyle(clipKey(clip), 2, QStringLiteral("Headline")).isEmpty());
    REQUIRE(tools->createTextStyle(clipKey(clip), 3, QStringLiteral("Counter")).isEmpty());
    QQuickItem* tabs = findNamed(panel, QStringLiteral("graphicsTabs"));
    REQUIRE(tabs);
    REQUIRE(QQmlProperty::write(tabs, QStringLiteral("currentIndex"), 2)); // as a click does: the binding to the selection goes
    QQuickItem* stylesPage = findNamed(panel, QStringLiteral("graphicsStylesPage"));
    REQUIRE(stylesPage);
    REQUIRE(waitFor([&] { return stylesPage->isVisible() && findNamed(panel, QStringLiteral("textStyleItem_1")) != nullptr; }, 3000));
    const auto cardName = [&](int index) {
        QQuickItem* card = findNamed(panel, QStringLiteral("textStyleItem_%1").arg(index));
        return card ? card->property("modelData").toMap().value(QStringLiteral("name")).toString() : QString();
    };
    CHECK(cardName(0) == QStringLiteral("Headline"));
    CHECK(cardName(1) == QStringLiteral("Counter"));
    REQUIRE(tools->moveTextStyle(QStringLiteral("Counter"), 0).isEmpty());
    REQUIRE(waitFor([&] { return cardName(0) == QStringLiteral("Counter"); }, 3000));
    CHECK(cardName(1) == QStringLiteral("Headline"));
    REQUIRE_FALSE(shell.grab().isNull());
    // QA: a style's "Aa" sample sits on a chip that contrasts with its colour (the dark "Counter" style on a dark card was
    // unreadable): the chip is light when the style's colour is dark and dark when it is light, in either theme.
    for (int index = 0; index < 2; ++index) {
        QQuickItem* sample = findNamed(panel, QStringLiteral("textStyleSample_%1").arg(index));
        REQUIRE(sample != nullptr);
        QQuickItem* card = findNamed(panel, QStringLiteral("textStyleItem_%1").arg(index));
        REQUIRE(card != nullptr);
        const QColor ink = QColor(card->property("modelData").toMap().value(QStringLiteral("color")).toString());
        const QColor chip = sample->property("color").value<QColor>();
        const auto luma = [](const QColor& c) { return 0.2126 * c.redF() + 0.7152 * c.greenF() + 0.0722 * c.blueF(); };
        INFO("style " << index << " ink " << ink.name().toStdString() << " chip " << chip.name().toStdString());
        CHECK(std::abs(luma(ink) - luma(chip)) > 0.35);
    }
    const QStringList styleIssues = auditAccessibility(panel);
    INFO(styleIssues.join(QLatin1Char('\n')).toStdString());
    CHECK(styleIssues.isEmpty());
    for (const bool dark : {true, false}) {
        shell.setAppearance(dark, "ultraviolet");
        REQUIRE(shell.settle(dark, shell.themeColor("accentFill")));
        grabPanel(QStringLiteral("graphics-styles-%1.png").arg(dark ? QStringLiteral("dark") : QStringLiteral("light")));
    }
    shell.setAppearance(true, "ultraviolet");
    REQUIRE(QQmlProperty::write(tabs, QStringLiteral("currentIndex"), 0));

    // The text editor's style bar: a selection shows the styles as buttons; a click asks for that style on the range
    // and the editor keeps its focus.
    QQmlComponent editorComponent(&shell.engine);
    editorComponent.setData(
        "import Wabcut\nGraphicTextEditor { width: 600; height: 120; canvasWidth: 1000; canvasHeight: 600;\n"
        "  styleNames: [{ name: 'Highlight' }, { name: 'Quiet' }]\n"
        "  property var requests: []\n"
        "  onStyleRangeRequested: (from, to, name) => requests = requests.concat([[from, to, name]]) }\n",
        QUrl(QStringLiteral("qrc:/v07-graphics/Editor.qml")));
    std::unique_ptr<QObject> editorObject(editorComponent.create());
    INFO(editorComponent.errorString().toStdString());
    REQUIRE(editorObject);
    auto* editor = qobject_cast<QQuickItem*>(editorObject.get());
    REQUIRE(editor);
    editorObject.release();
    std::unique_ptr<QQuickItem> ownEditor(editor);
    editor->setParentItem(shell.window->contentItem());
    editor->setPosition(QPointF(700, 300));
    editor->setZ(1000);
    const QVariantMap editorInfo{{QStringLiteral("x"), 0.1},        {QStringLiteral("y"), 0.2},
                                 {QStringLiteral("width"), 0.5},    {QStringLiteral("height"), 0.2},
                                 {QStringLiteral("fontSize"), 0.1}, {QStringLiteral("text"), QStringLiteral("KILL STREAK")}};
    REQUIRE(QMetaObject::invokeMethod(editor, "open", Q_ARG(QVariant, 2), Q_ARG(QVariant, editorInfo), Q_ARG(QVariant, false)));
    QQuickItem* styleBar = findNamed(editor, QStringLiteral("graphicTextStyleBar"));
    REQUIRE(styleBar);
    REQUIRE_FALSE(shell.grab().isNull());
    CHECK_FALSE(styleBar->isVisible()); // nothing selected
    auto* edit2 = findNamed(editor, QStringLiteral("graphicTextEdit"));
    REQUIRE(edit2);
    REQUIRE(QMetaObject::invokeMethod(edit2, "select", Q_ARG(int, 5), Q_ARG(int, 11)));
    REQUIRE(waitFor([&] { return styleBar->isVisible(); }, 3000));
    QQuickItem* highlight = findNamed(editor, QStringLiteral("graphicTextStyleButton_0"));
    REQUIRE(highlight);
    REQUIRE(QMetaObject::invokeMethod(highlight, "clicked"));
    const QVariantList requests = editor->property("requests").toList();
    REQUIRE(requests.size() == 1);
    const QVariantList request = requests.at(0).toList();
    REQUIRE(request.size() == 3);
    CHECK(request.at(0).toInt() == 5);
    CHECK(request.at(1).toInt() == 11);
    CHECK(request.at(2).toString() == QStringLiteral("Highlight"));
    CHECK(edit2->property("activeFocus").toBool()); // the buttons do not take the focus from the editor
    spin(60);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    const double dpr = image.devicePixelRatio();
    const QRectF er = editor->mapRectToScene(QRectF(0, 0, editor->width(), editor->height() + 60)).adjusted(-12, -12, 12, 12);
    REQUIRE(image.copy(QRectF(er.x() * dpr, er.y() * dpr, er.width() * dpr, er.height() * dpr).toRect())
                .save(QDir(outputDir()).filePath(QStringLiteral("graphics-text-style-bar.png"))));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
