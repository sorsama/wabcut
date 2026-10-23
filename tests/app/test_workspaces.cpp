// 0.1.3 workspaces (Workspaces.qml, DockManager, WorkspaceTabs): none is built in, the first start makes one from the
// Edit preset and the presets (their keys, the Export button, the former built-in ids) make the others when first
// needed; workspaces are made from a preset or as a copy (its layout too), renamed, reordered, reset and deleted, the
// last one staying; the focus keys open panels as tabs where panels like them are and the Window menu as tabs of the
// group the user worked in last (UX rework); closing the monitors or the timeline gives their room to the rest;
// everything is kept and comes back at the next start.

#include "ShellFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlContext>
#include <QQmlExpression>
#include <QImage>
#include <QQuickItem>

#include <cmath>

using namespace wabcut::test;

namespace {

/// Runs `code` in Main.qml's context (its ids: workArea, docking, browserDock, ...; the Workspaces singleton).
QVariant evaluate(const Shell& shell, const QString& code) {
    QQmlExpression expression(qmlContext(shell.window.get()), shell.window.get(), code);
    const QVariant result = expression.evaluate();
    INFO(code.toStdString() << ": " << expression.error().toString().toStdString());
    REQUIRE_FALSE(expression.hasError());
    return result;
}

int evaluateInt(const Shell& shell, const QString& code) {
    return evaluate(shell, code).toInt();
}

/// The item named `name` in the scene under `root` (Repeater delegates are not QObject children of the window).
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

QQuickItem* scene(const Shell& shell, const QString& name) {
    return findNamed(shell.window->contentItem(), name);
}

/// Shown on screen: effectively visible and not faded out.
bool showing(const QQuickItem* item) {
    if (!item || !item->isVisible())
        return false;
    for (const QQuickItem* p = item; p; p = p->parentItem()) {
        if (p->opacity() <= 0.01)
            return false;
    }
    return true;
}

/// The current layout's groups, in order: each one's panels joined with "+".
QString groupsText(const Shell& shell) {
    return evaluate(shell, QStringLiteral("Workspaces.groupsOf(docking.tree).map(g => g.panels.join('+')).join(' ')")).toString();
}

} // namespace

TEST_CASE("Workspaces: none is built in; the presets lay their panels out", "[app][workspaces][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    spin(50);

    // The first start: one workspace, made from the Edit preset, and its tab; the "+" makes the others.
    CHECK(evaluate(shell, QStringLiteral("Workspaces.all.map(w => w.preset + ':' + w.name).join(',')")).toString() ==
          QStringLiteral("edit:Edit"));
    CHECK(scene(shell, QStringLiteral("workspaceTab_edit")) != nullptr);
    CHECK(scene(shell, QStringLiteral("workspaceTab_color")) == nullptr);
    CHECK(scene(shell, QStringLiteral("newWorkspaceButton")) != nullptr);

    // Edit: the Library, both monitors, Effect Controls, the timeline; one panel per group, no tabs.
    QQuickItem* library = shell.item("libraryPanel");
    QQuickItem* effectControls = shell.item("effectControls");
    REQUIRE(library != nullptr);
    REQUIRE(effectControls != nullptr);
    CHECK(showing(library));
    CHECK(showing(effectControls));
    CHECK(shell.item("monitorArea")->property("sourceShown").toBool());
    CHECK(shell.item("monitorArea")->property("dual").toBool());
    CHECK(groupsText(shell) == QStringLiteral("library source program effectControls timeline"));
    CHECK(shell.window->property("railPanel").toString() == QStringLiteral("library"));

    // Audio (its key, Alt+Shift+3): made from the Audio preset on first use; Audio Tools, the mixer (wide), the volume
    // rubber bands.
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QVariant(QStringLiteral("window.workspace3")))));
    spin(200);
    CHECK(evaluate(shell, QStringLiteral("window.workspaceSpec.preset")).toString() == QStringLiteral("audio"));
    QQuickItem* tools = shell.item("audioToolsPanel");
    QQuickItem* mixer = shell.item("audioMixerPanel");
    REQUIRE(tools != nullptr);
    REQUIRE(mixer != nullptr);
    CHECK(showing(tools));
    CHECK(showing(mixer));
    CHECK_FALSE(showing(effectControls));
    CHECK(mixer->width() >= 460);
    CHECK_FALSE(shell.item("monitorArea")->property("sourceShown").toBool());
    CHECK(evaluate(shell, QStringLiteral("timeline.showVolume")).toBool());
    CHECK(scene(shell, QStringLiteral("workspaceTab_audio")) != nullptr);

    // Export (the Export button): the Program Monitor and the export settings.
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QVariant(QStringLiteral("file.exportMedia")))));
    spin(200);
    CHECK(evaluate(shell, QStringLiteral("window.workspaceSpec.preset")).toString() == QStringLiteral("export"));
    CHECK(groupsText(shell) == QStringLiteral("program export timeline"));
    CHECK(evaluate(shell, QStringLiteral("window.railPanel")).toString().isEmpty());

    // Color (a former built-in id still works): the Scopes, the Program Monitor and the Color panel; no browser panel
    // is in the left column (window.railPanel). A browser panel opens as a tab with the Scopes.
    shell.window->setProperty("workspace", 3);
    spin(200);
    CHECK(evaluate(shell, QStringLiteral("window.workspaceSpec.preset")).toString() == QStringLiteral("color"));
    QQuickItem* scopes = shell.item("scopesPanel");
    QQuickItem* color = shell.item("colorPanel");
    REQUIRE(scopes != nullptr);
    REQUIRE(color != nullptr);
    CHECK(showing(scopes));
    CHECK(showing(color));
    CHECK(evaluate(shell, QStringLiteral("window.railPanel")).toString().isEmpty());
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showBrowserPanel", Q_ARG(QVariant, QVariant(QStringLiteral("markers")))));
    spin(100);
    CHECK(groupsText(shell) == QStringLiteral("scopes+markers program color timeline"));
    CHECK(shell.window->property("railPanel").toString() == QStringLiteral("markers"));

    // Each preset's workspace is made once: its key opens the same one again.
    const int colorId = shell.window->property("workspace").toInt();
    shell.window->setProperty("workspace", 0);
    spin(100);
    shell.window->setProperty("workspace", 3);
    spin(100);
    CHECK(shell.window->property("workspace").toInt() == colorId);
    CHECK(evaluateInt(shell, QStringLiteral("Workspaces.all.length")) == 4);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Workspaces: made from a preset or as a copy, renamed, reordered, reset and deleted; the last one stays",
          "[app][workspaces][qml]") {
    resetSettings();
    clearMessages();
    {
        Shell shell;
        REQUIRE(shell.window != nullptr);
        shell.theme->setProperty("reduceMotion", true);
        spin(50);
        const int edit = shell.window->property("workspace").toInt();

        // The "+" menu: one item per preset, and a copy of the current workspace.
        for (const char* name : {"workspaceAddPreset_edit", "workspaceAddPreset_color", "workspaceAddPreset_audio",
                                 "workspaceAddPreset_captions", "workspaceAddPreset_export", "workspaceAddCopy"}) {
            INFO(name);
            CHECK(shell.object(name) != nullptr);
        }

        // Two from the Color preset: the second gets a number.
        const int color = evaluateInt(shell, QStringLiteral("Workspaces.createFromPreset('color', '')"));
        const int color2 = evaluateInt(shell, QStringLiteral("Workspaces.createFromPreset('color', '')"));
        CHECK(evaluate(shell, QStringLiteral("Workspaces.label(%1)").arg(color2)).toString() == QStringLiteral("Color 2"));

        // A changed layout is copied with the workspace.
        evaluate(shell, QStringLiteral("docking.joinPanel('effectControls', docking.groupOfPanel('library').group)"));
        spin(50);
        const int copy = evaluateInt(shell, QStringLiteral("Workspaces.create(%1, 'Logging')").arg(edit));
        shell.window->setProperty("workspace", copy);
        spin(100);
        CHECK(groupsText(shell) == QStringLiteral("library+effectControls source program timeline"));
        CHECK(evaluate(shell, QStringLiteral("Workspaces.label(%1)").arg(copy)).toString() == QStringLiteral("Logging"));

        // Renamed, moved first.
        evaluate(shell, QStringLiteral("Workspaces.rename(%1, 'Grade')").arg(color));
        evaluate(shell, QStringLiteral("Workspaces.moveTo(%1, 0)").arg(color));
        spin(50);
        CHECK(evaluate(shell, QStringLiteral("Workspaces.all.map(w => w.name).join(',')")).toString() ==
              QStringLiteral("Grade,Edit,Logging,Color 2"));
        CHECK(scene(shell, QStringLiteral("workspaceTab_color")) != nullptr); // the first one made from Color

        // Reset: the preset's layout again.
        evaluate(shell, QStringLiteral("Workspaces.reset(%1)").arg(copy));
        spin(50);
        CHECK(groupsText(shell) == QStringLiteral("library source program effectControls timeline"));

        // Deleting the current workspace goes to the first one; the last one stays.
        evaluate(shell, QStringLiteral("Workspaces.remove(%1)").arg(copy));
        spin(50);
        CHECK(shell.window->property("workspace").toInt() == color);
        for (const int id : {color2, edit, color})
            evaluate(shell, QStringLiteral("Workspaces.remove(%1)").arg(id));
        spin(50);
        CHECK(evaluateInt(shell, QStringLiteral("Workspaces.all.length")) == 1);
        CHECK(evaluateInt(shell, QStringLiteral("Workspaces.all[0].id")) == color);
        CHECK(shell.window->property("workspace").toInt() == color);
        // Its menu does not offer deleting it.
        QObject* deleteItem = shell.object("workspaceMenuDelete");
        REQUIRE(deleteItem != nullptr);
        CHECK_FALSE(deleteItem->property("enabled").toBool());
    }
    {
        // Kept: the next start has the same workspaces, and Settings' start workspace among them.
        Shell shell;
        REQUIRE(shell.window != nullptr);
        spin(50);
        CHECK(evaluate(shell, QStringLiteral("Workspaces.all.map(w => w.name).join(',')")).toString() == QStringLiteral("Grade"));
        CHECK(evaluate(shell, QStringLiteral("window.workspaceSpec.name")).toString() == QStringLiteral("Grade"));
        CHECK(groupsText(shell) == QStringLiteral("scopes program color timeline"));
    }
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Workspaces: the focus keys and the Window menu open panels as tabs where panels like them are",
          "[app][workspaces][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    spin(50);
    QQuickItem* library = shell.item("libraryPanel");
    REQUIRE(library != nullptr);
    // Startup focus makes no group the active one (UX rework: only the user's presses, tabs, drops and focus keys do).
    CHECK(evaluate(shell, QStringLiteral("docking.activeGroup")).toString().isEmpty());

    // A focus key (showBrowserPanel): Effects joins the Library's group as a tab and shows; that group is the active one.
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showBrowserPanel", Q_ARG(QVariant, QVariant(QStringLiteral("effects")))));
    spin(100);
    CHECK(groupsText(shell) == QStringLiteral("library+effects source program effectControls timeline"));
    CHECK(shell.window->property("browserPanel").toString() == QStringLiteral("effects"));
    CHECK(evaluate(shell, QStringLiteral("docking.activeGroup === docking.groupOfPanel('library').group")).toBool());
    CHECK_FALSE(showing(library));
    // Back to the Library: its tab, the same panel (it kept its state).
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showBrowserPanel", Q_ARG(QVariant, QVariant(QStringLiteral("library")))));
    spin(100);
    CHECK(showing(library));
    CHECK(shell.item("libraryPanel") == library);
    CHECK(shell.window->property("browserPanel").toString() == QStringLiteral("library"));

    // Explain this frame: a tab next to Effect Controls (a programmatic reveal: the column rule, not the active group);
    // its close button closes it.
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showFrameInspector"));
    spin(100);
    CHECK(groupsText(shell) == QStringLiteral("library+effects source program effectControls+frameInspector timeline"));
    evaluate(shell, QStringLiteral("docking.closePanel('frameInspector')"));
    spin(50);
    CHECK(groupsText(shell) == QStringLiteral("library+effects source program effectControls timeline"));

    // The Window menu lists every panel, checked when it is open; choosing a closed one opens it as a tab of the
    // active group (here the Library's, from the focus key above).
    QObject* scopesItem = shell.object("windowMenu_scopes");
    QObject* libraryItem = shell.object("windowMenu_library");
    REQUIRE(scopesItem != nullptr);
    REQUIRE(libraryItem != nullptr);
    CHECK(libraryItem->property("checked").toBool());
    CHECK_FALSE(scopesItem->property("checked").toBool());
    REQUIRE(QMetaObject::invokeMethod(scopesItem, "triggered"));
    spin(100);
    CHECK(scopesItem->property("checked").toBool());
    CHECK(groupsText(shell) == QStringLiteral("library+effects+scopes source program effectControls timeline"));

    // The Program Monitor's focus key makes its group the active one: the Window menu's Markers lands there, not with
    // the browser panels where the column rule would put it.
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QVariant(QStringLiteral("window.programMonitor")))));
    spin(100);
    CHECK(evaluate(shell, QStringLiteral("docking.activeGroup === docking.groupOfPanel('program').group")).toBool());
    QObject* markersItem = shell.object("windowMenu_markers");
    REQUIRE(markersItem != nullptr);
    REQUIRE(QMetaObject::invokeMethod(markersItem, "triggered"));
    spin(100);
    CHECK(groupsText(shell) == QStringLiteral("library+effects+scopes source program+markers effectControls timeline"));
    CHECK(evaluate(shell, QStringLiteral("docking.shownPanel(docking.groupOfPanel('program').group)")).toString() ==
          QStringLiteral("markers"));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Workspaces: without monitors or the timeline the other panels take the room", "[app][workspaces][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    spin(50);

    for (const char* key : {"source", "program", "timeline"})
        evaluate(shell, QStringLiteral("docking.closePanel('%1')").arg(QLatin1String(key)));
    spin(100);
    QQuickItem* area = shell.item("monitorArea");
    QQuickItem* browser = qobject_cast<QQuickItem*>(evaluate(shell, QStringLiteral("docking.slotOfPanel('library')")).value<QObject*>());
    QQuickItem* side = qobject_cast<QQuickItem*>(evaluate(shell, QStringLiteral("docking.slotOfPanel('effectControls')")).value<QObject*>());
    REQUIRE(area != nullptr);
    REQUIRE(browser != nullptr);
    REQUIRE(side != nullptr);
    const double workWidth = evaluate(shell, QStringLiteral("workArea.width")).toDouble();
    const double gap = shell.theme->property("gap").toDouble();
    // No monitors: the Library and Effect Controls share the width.
    CHECK(area->width() < 1.0);
    CHECK(std::abs(browser->width() + side->width() + gap - workWidth) <= 2.0);
    // No timeline: they take the height.
    CHECK_FALSE(shell.item("timelineHost")->isVisible());
    CHECK(std::abs(side->height() - evaluate(shell, QStringLiteral("workArea.height")).toDouble()) <= 1.0);
    // The timeline focus key opens it again, along the bottom.
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QVariant(QStringLiteral("window.timeline")))));
    spin(100);
    CHECK(shell.item("timelineHost")->isVisible());
    CHECK(groupsText(shell) == QStringLiteral("library effectControls timeline"));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Workspaces: the accent plate sits under the active tab after the tabs change", "[app][workspaces][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    spin(50);
    QQuickItem* plate = scene(shell, QStringLiteral("workspacePlate"));
    REQUIRE(plate != nullptr);
    const auto under = [&]() {
        const int id = shell.window->property("workspace").toInt();
        const QString name =
            QStringLiteral("workspaceTab_") + evaluate(shell, QStringLiteral("Workspaces.keyOf(%1)").arg(id)).toString();
        QQuickItem* tab = scene(shell, name);
        INFO(name.toStdString());
        REQUIRE(tab != nullptr);
        CHECK(plate->isVisible());
        CHECK(std::abs(plate->x() - tab->x()) < 0.5);
        CHECK(std::abs(plate->width() - tab->width()) < 0.5);
    };
    // The former built-in ids open (and make) the presets' workspaces.
    for (int id : {0, 3, 1, 4, 2, 0}) {
        shell.window->setProperty("workspace", id);
        spin(100);
        under();
    }
    // The tab items are rebuilt (a rename, a new workspace, a move): the plate follows the new items.
    const int color = evaluateInt(shell, QStringLiteral("Workspaces.firstOf('color')"));
    evaluate(shell, QStringLiteral("Workspaces.rename(%1, 'Grade')").arg(color));
    spin(100);
    under();
    shell.window->setProperty("workspace", color);
    spin(100);
    under();
    const int id = evaluateInt(shell, QStringLiteral("Workspaces.create(%1, 'Grade 2')").arg(color));
    spin(100);
    shell.window->setProperty("workspace", id);
    spin(100);
    under();
    evaluate(shell, QStringLiteral("Workspaces.move(%1, -1)").arg(id));
    spin(100);
    under();

    // With motion the plate glides, and arrives.
    shell.theme->setProperty("reduceMotion", false);
    for (int to : {0, 3, 4, id}) {
        shell.window->setProperty("workspace", to);
        spin(600);
        under();
    }

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
