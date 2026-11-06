// Shell QoL, presets, custom words and profanity sounds (offscreen; no window is shown):
//   * the Window menu lists its panels A to Z (card 020), the Gaming workspace preset (014), the workspace tabs can be
//     hidden (066), the Video Options rows say what each does (088);
//   * a project another session has open shows a Read-only badge and says so once (018); files dropped on any place of the
//     editor are imported (019);
//   * an Audio Track Mixer chain is saved as a preset and added to another strip (101); caption looks are kept for every
//     project and shared as files (115); custom words for the transcriber (061); a sound file in place of the bleep (102).
// Screenshots land in <build>/test-output/ui/ (dark and light themes).

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "AudioMixerBridge.h"
#include "BridgeIds.h"
#include "CaptionsBridge.h"
#include "CommandBridge.h"
#include "CommandJob.h"
#include "CommandPalette.h"
#include "EffectPresetsBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "TranscriptBridge.h"

#include <wabcut/editor/Production.h>
#include <wabcut/model/CaptionOps.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <QCollator>
#include <QCoreApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QMimeData>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using command::Json;
namespace fs = std::filesystem;

namespace {

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

QQuickItem* visibleNamed(Shell& shell, const QString& name) {
    return findItem(shell.window->contentItem(), [&](QQuickItem* i) { return i->objectName() == name && i->isVisible(); });
}

void saveShot(const QImage& image, const QQuickItem* item, const QString& name, int margin = 0) {
    REQUIRE_FALSE(image.isNull());
    const double dpr = image.devicePixelRatio();
    QRectF area = item->mapRectToScene(QRectF(0, 0, item->width(), item->height())).adjusted(-margin, -margin, margin, margin);
    area = area.intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    REQUIRE(image.copy(QRectF(area.x() * dpr, area.y() * dpr, area.width() * dpr, area.height() * dpr).toRect())
                .save(QDir(outputDir()).filePath(name)));
}

QVariant evaluate(const Shell& shell, const QString& code) {
    QQmlExpression expression(qmlContext(shell.window.get()), shell.window.get(), code);
    const QVariant result = expression.evaluate();
    INFO(code.toStdString() << ": " << expression.error().toString().toStdString());
    REQUIRE_FALSE(expression.hasError());
    return result;
}

bool opened(QObject* popup) { return popup && popup->property("opened").toBool(); }
bool shown(QObject* popup) { return popup && popup->property("visible").toBool(); }

/// The editor at 1600x960 (the menu titles show), motion off, the way the menu bar tests have it.
struct EditorShell {
    Shell shell;
    QQuickWindow* w = nullptr;
    QObject* bar = nullptr;
    ProjectController* controller = nullptr;

    explicit EditorShell(bool dark = true) {
        REQUIRE(shell.window != nullptr);
        INFO(join(shell.errors));
        REQUIRE(shell.errors.isEmpty());
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        shell.setAppearance(dark, "ultraviolet");
        w = shell.window.get();
        bar = shell.object("appMenuBar");
        REQUIRE(bar != nullptr);
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        REQUIRE(controller != nullptr);
        spin(50);
    }
    /// Opens a menu of the bar (index: File 0, Edit 1, Clip 2, Sequence 3, Markers 4, Graphics 5, View 6, Window 7, Help 8).
    QObject* open(int index, const char* menuName) const {
        REQUIRE(QMetaObject::invokeMethod(bar, "open", Q_ARG(int, index), Q_ARG(bool, false)));
        QObject* menu = shell.object(menuName);
        REQUIRE(menu != nullptr);
        REQUIRE(waitFor([&] { return opened(menu); }));
        return menu;
    }
    void dismiss(QObject* menu) const {
        REQUIRE(QMetaObject::invokeMethod(menu, "close"));
        REQUIRE(waitFor([&] { return !shown(menu); }));
        spin(30);
    }
};

/// A project file of the sample project saved at `file`.
bool saveSampleAs(ProjectLifecycle* project, const fs::path& file) {
    return project->saveAs(QUrl::fromLocalFile(QString::fromStdU16String(file.u16string())));
}

QString fileText(const fs::path& file) {
    QFile f(QString::fromStdU16String(file.u16string()));
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

} // namespace

// ---- 020: the Window menu's panels A to Z --------------------------------------------------------------------------------

TEST_CASE("Window menu: the panels are listed A to Z by name, the fixed rows stay above them", "[app][menubar][qml][d3]") {
    resetSettings();
    clearMessages();
    EditorShell s;
    QObject* menu = s.open(7, "windowMenu");

    QStringList labels;
    QStringList keys;
    int firstPanel = -1;
    int resetRow = -1;
    int count = 0;
    REQUIRE(menu->property("count").isValid());
    count = menu->property("count").toInt();
    for (int i = 0; i < count; ++i) {
        QQuickItem* item = nullptr;
        REQUIRE(QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, i)));
        REQUIRE(item != nullptr);
        const QString name = item->objectName();
        if (name == QLatin1String("resetPanelLayoutMenuItem"))
            resetRow = i;
        if (name.startsWith(QLatin1String("windowMenu_"))) {
            if (firstPanel < 0)
                firstPanel = i;
            labels << item->property("text").toString();
            keys << name.mid(11);
        }
    }
    REQUIRE(labels.size() >= 20);
    // Premiere's order of the fixed rows is kept: Workspaces, Maximize Frame, Reset to Saved Layout, then the panels.
    CHECK(resetRow >= 0);
    CHECK(firstPanel > resetRow);
    // A to Z by the (translated) name.
    QStringList sorted = labels;
    std::sort(sorted.begin(), sorted.end(), [](const QString& a, const QString& b) { return QString::localeAwareCompare(a, b) < 0; });
    INFO(labels.join(QLatin1String(", ")).toStdString());
    CHECK(labels == sorted);
    CHECK(labels.first() == QStringLiteral("Audio Meters"));
    // History stays among them, Generate is listed while AI is on.
    CHECK(keys.contains(QStringLiteral("history")));
    CHECK(keys.contains(QStringLiteral("generate")));
    // The catalog itself keeps its own order (docking and the presets rely on it).
    CHECK(evaluate(s.shell, QStringLiteral("Workspaces.catalog.slice(0, 3).map(p => p.key).join(',')")).toString() ==
          QStringLiteral("library,mediaBrowser,effects"));
    // Checked state and shortcuts still follow the key: the Timeline row is checked in the Edit preset.
    CHECK(s.shell.object("windowMenu_timeline")->property("checked").toBool());
    s.dismiss(menu);

    // AI off: no Generate; the rest stay sorted.
    evaluate(s.shell, QStringLiteral("Preferences.aiEnabled = false"));
    spin(100);
    menu = s.open(7, "windowMenu");
    CHECK(s.shell.object("windowMenu_generate") == nullptr);
    CHECK(s.shell.object("windowMenu_scopes") != nullptr);
    s.dismiss(menu);
    evaluate(s.shell, QStringLiteral("Preferences.aiEnabled = true"));
    resetSettings();
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// ---- 014: the Gaming workspace preset -------------------------------------------------------------------------------------

TEST_CASE("Workspaces: the Gaming preset lays out Highlights, the Program Monitor, Effect Controls and the timeline",
          "[app][workspaces][qml][d3][screenshots]") {
    for (const bool dark : {true, false}) {
        DYNAMIC_SECTION((dark ? "dark" : "light")) {
            resetSettings();
            clearMessages();
            EditorShell s(dark);
            CHECK(evaluate(s.shell, QStringLiteral("Workspaces.presetOrder.join(',')")).toString() ==
                  QStringLiteral("edit,color,audio,captions,export,gaming"));
            CHECK(evaluate(s.shell, QStringLiteral("Workspaces.presetLabel('gaming')")).toString() == QStringLiteral("Gaming"));
            const QString shape = QStringLiteral("Workspaces.groupsOf(Workspaces.presetLayout('gaming')).map(g => g.panels.join('+')).join(' ')");
            CHECK(evaluate(s.shell, shape).toString() ==
                  QStringLiteral("highlights+shorts+library program effectControls+essential timeline"));
            CHECK(evaluate(s.shell, QStringLiteral("JSON.stringify(Workspaces.presetLayout('gaming').weights)")).toString() ==
                  QStringLiteral("[0.55,0.45]"));
            // A saved layout of the preset reads back as it is (same groups, panels and weights).
            const QString digest = QStringLiteral("(l => Workspaces.groupsOf(l).map(g => g.group + ':' + g.panels.join('+') + ':' + g.current).join(' ') + ' ' + JSON.stringify(l.weights) + JSON.stringify(l.children[0].weights))");
            CHECK(evaluate(s.shell, digest + QStringLiteral("(Workspaces.normalizedLayout(Workspaces.presetLayout('gaming'))) === ") + digest +
                                        QStringLiteral("(Workspaces.presetLayout('gaming'))")).toBool());
            // The Add workspace menu and Window > Workspaces > New Workspace offer it.
            CHECK(s.shell.object("windowMenuNewWorkspace_gaming") != nullptr);

            // Made from the preset: its tab, its panels, no key of its own.
            const int id = evaluate(s.shell, QStringLiteral("Workspaces.createFromPreset('gaming', '')")).toInt();
            REQUIRE(id >= 100);
            evaluate(s.shell, QStringLiteral("window.workspace = %1").arg(id));
            spin(300);
            CHECK(evaluate(s.shell, QStringLiteral("Workspaces.resolve(window.workspace).preset")).toString() == QStringLiteral("gaming"));
            CHECK(evaluate(s.shell, QStringLiteral("Workspaces.resolve(window.workspace).name")).toString() == QStringLiteral("Gaming"));
            CHECK(evaluate(s.shell, QStringLiteral("Workspaces.groupsOf(docking.tree).map(g => g.panels.join('+')).join(' ')")).toString() ==
                  QStringLiteral("highlights+shorts+library program effectControls+essential timeline"));
            // The workspace keys are positional (A3): the tab's place gives it Alt+Shift+<place> while it is among the first nine.
            CHECK(evaluate(s.shell, QStringLiteral("Workspaces.commandOf(%1) === 'window.workspace' + (Workspaces.all.findIndex(w => w.id === %1) + 1)").arg(id))
                      .toBool());
            CHECK(findItem(s.w->contentItem(), [](QQuickItem* i) { return i->objectName() == QLatin1String("workspaceTab_gaming"); }) != nullptr);
            // Highlights is the shown tab of the first group, and the Program Monitor and the timeline are on screen.
            CHECK(evaluate(s.shell, QStringLiteral("workArea.panelShown('highlights')")).toBool());
            CHECK(evaluate(s.shell, QStringLiteral("workArea.panelShown('program')")).toBool());
            CHECK(evaluate(s.shell, QStringLiteral("workArea.panelShown('effectControls')")).toBool());
            CHECK(evaluate(s.shell, QStringLiteral("workArea.panelShown('timeline')")).toBool());

            // Reset to the preset brings the layout back after a panel was closed.
            evaluate(s.shell, QStringLiteral("docking.closePanel('effectControls')"));
            spin(100);
            CHECK_FALSE(evaluate(s.shell, QStringLiteral("docking.isOpen('effectControls')")).toBool());
            evaluate(s.shell, QStringLiteral("Workspaces.resetToPreset(%1)").arg(id));
            evaluate(s.shell, QStringLiteral("window.loadWorkspace()"));
            spin(200);
            CHECK(evaluate(s.shell, QStringLiteral("docking.isOpen('effectControls')")).toBool());
            saveShot(s.shell.grab(), s.w->contentItem(), dark ? QStringLiteral("workspace-gaming.png") : QStringLiteral("workspace-gaming-light.png"));
            const QStringList messages = takeMessages();
            INFO(join(messages));
            CHECK(messages.isEmpty());
            resetSettings();
        }
    }
}

// ---- 066: hide the workspace tabs -----------------------------------------------------------------------------------------

TEST_CASE("The workspace tabs can be hidden; the workspaces stay reachable", "[app][workspaces][qml][d3][screenshots]") {
    resetSettings();
    clearMessages();
    EditorShell s;
    QQuickItem* tabs = s.shell.item("workspaceTabs");
    REQUIRE(tabs != nullptr);
    CHECK(tabs->isVisible());
    CHECK(evaluate(s.shell, QStringLiteral("Preferences.showWorkspaceTabs")).toBool()); // on by default
    // With an Export workspace the onboarding tour points at its tab; hidden, at the Export button.
    evaluate(s.shell, QStringLiteral("window.openPreset('export')"));
    spin(200);
    CHECK(evaluate(s.shell, QStringLiteral("topBar.exportTab.objectName")).toString() == QStringLiteral("workspaceTab_export"));

    // Window > Workspaces > Show Workspace Tabs: off. The tabs go; the Export hint falls back to the button.
    QObject* menu = s.open(7, "windowMenu");
    QObject* toggle = s.shell.object("windowMenuWorkspaceTabs");
    REQUIRE(toggle != nullptr);
    CHECK(toggle->property("checked").toBool());
    CHECK(s.shell.object("windowMenuRenameWorkspace")->property("enabled").toBool());
    REQUIRE(QMetaObject::invokeMethod(toggle, "triggered"));
    spin(100);
    s.dismiss(menu);
    CHECK_FALSE(evaluate(s.shell, QStringLiteral("Preferences.showWorkspaceTabs")).toBool());
    CHECK_FALSE(tabs->isVisible());
    CHECK(evaluate(s.shell, QStringLiteral("topBar.exportTab.objectName")).toString() == QStringLiteral("topBarExportButton"));
    CHECK(evaluate(s.shell, QStringLiteral("topBar.menusCollapsed")).toBool() == false);
    // Renaming a workspace needs its tab: the row is off, and asking does nothing.
    menu = s.open(7, "windowMenu");
    CHECK_FALSE(s.shell.object("windowMenuRenameWorkspace")->property("enabled").toBool());
    CHECK_FALSE(s.shell.object("windowMenuWorkspaceTabs")->property("checked").toBool());
    s.dismiss(menu);
    evaluate(s.shell, QStringLiteral("topBar.renameWorkspace(window.workspace)"));
    CHECK(takeMessages().isEmpty());
    saveShot(s.shell.grab(), s.shell.item("titleBar"), QStringLiteral("titlebar-no-workspace-tabs.png"));

    // The workspaces are still there: a workspace key switches, and the Window menu lists them.
    REQUIRE(QMetaObject::invokeMethod(s.w, "runCommand", Q_ARG(QVariant, QVariant(QStringLiteral("window.workspace3")))));
    spin(200);
    CHECK(evaluate(s.shell, QStringLiteral("window.workspaceSpec.preset")).toString() == QStringLiteral("audio"));
    CHECK(s.shell.object("windowMenuWorkspace_audio") != nullptr);
    // The choice is kept for the next start.
    CHECK(QSettings().value(QStringLiteral("interface/showWorkspaceTabs")).isValid());
    CHECK_FALSE(QSettings().value(QStringLiteral("interface/showWorkspaceTabs")).toBool());

    // Back on from the same menu row.
    menu = s.open(7, "windowMenu");
    REQUIRE(QMetaObject::invokeMethod(s.shell.object("windowMenuWorkspaceTabs"), "triggered"));
    spin(100);
    s.dismiss(menu);
    CHECK(tabs->isVisible());
    CHECK(evaluate(s.shell, QStringLiteral("Preferences.showWorkspaceTabs")).toBool());
    resetSettings();
}

// ---- 088: names that say what each command does --------------------------------------------------------------------------

TEST_CASE("Video Options: Auto Fit to Frame, Set Scale to Fit Frame and Set Scale to Fill Frame say what each does", "[app][editing][qml][d3]") {
    resetSettings();
    clearMessages();
    EditorShell s;
    QObject* menu = s.open(2, "clipMenu");
    const auto row = [&](const char* command) {
        QObject* r = s.shell.object((std::string("menuCommand_") + command).c_str());
        INFO(command);
        REQUIRE(r != nullptr);
        return r;
    };
    CHECK(row("clip.scaleToFrameSize")->property("text").toString() == QStringLiteral("Auto Fit to Frame"));
    CHECK(row("clip.setToFrameSize")->property("text").toString() == QStringLiteral("Set Scale to Fit Frame"));
    CHECK(row("clip.setToFillFrame")->property("text").toString() == QStringLiteral("Set Scale to Fill Frame"));
    // Each has a line that says what it does, and names Premiere's own name for it.
    for (const auto& [command, premiere] : {std::pair{"clip.scaleToFrameSize", "Scale to Frame Size"},
                                            std::pair{"clip.setToFrameSize", "Set to Frame Size"},
                                            std::pair{"clip.setToFillFrame", "Fill Frame"}}) {
        const QString hint = row(command)->property("hint").toString();
        INFO(command);
        CHECK(hint.contains(QLatin1String(premiere)));
        CHECK(hint.contains(QStringLiteral("Motion > Scale")));
    }
    CHECK(row("clip.scaleToFrameSize")->property("hint").toString().contains(QStringLiteral("100 %")));
    s.dismiss(menu);

    // The shortcut list and the palette show the new names and still find the old ones.
    auto* palette = s.shell.engine.singletonInstance<CommandPalette*>("Wabcut", "CommandPalette");
    REQUIRE(palette != nullptr);
    palette->setQuery(QStringLiteral("scale to frame size"));
    QVariantList rows = palette->results();
    bool found = false;
    for (const QVariant& r : rows) {
        found = found || r.toMap().value(QStringLiteral("title")).toString() == QStringLiteral("Auto Fit to Frame");
    }
    CHECK(found); // searching for Premiere's name finds it
    palette->setQuery(QStringLiteral("set scale to fill"));
    rows = palette->results();
    found = false;
    for (const QVariant& r : rows) {
        found = found || r.toMap().value(QStringLiteral("title")).toString() == QStringLiteral("Set Scale to Fill Frame");
    }
    CHECK(found);
    // The history step of the edit carries the same name (the catalog's title).
    CHECK(command::builtinCommands().find("clip.scaleToFrameSize")->title == "Auto Fit to Frame");
    CHECK(command::builtinCommands().find("clip.setToFrameSize")->title == "Set Scale to Fit Frame");
    CHECK(command::builtinCommands().find("clip.setToFillFrame")->title == "Set Scale to Fill Frame");
    resetSettings();
}

// ---- 018: a project another session has open -----------------------------------------------------------------------------

TEST_CASE("A project another session has open shows a Read-only badge and says so once with Save As", "[app][project][lock][qml][d3][screenshots]") {
    for (const bool dark : {true, false}) {
        DYNAMIC_SECTION((dark ? "dark" : "light")) {
            resetSettings();
            clearMessages();
            EditorShell s(dark);
            auto* project = s.shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
            REQUIRE(project != nullptr);
            project->setLockProjects(true);
            QTemporaryDir dir;
            REQUIRE(dir.isValid());
            const fs::path mine = fs::path(dir.path().toStdU16String()) / "Mine.wabcut";
            REQUIRE(saveSampleAs(project, mine));
            QQuickItem* badge = findItem(s.w->contentItem(), [](QQuickItem* i) { return i->objectName() == QLatin1String("readOnlyBadge"); });
            REQUIRE(badge != nullptr);
            CHECK_FALSE(project->readOnly());
            CHECK_FALSE(badge->isVisible()); // an ordinary project: no badge
            QObject* toast = s.shell.object("toast");
            REQUIRE(toast != nullptr);
            CHECK_FALSE(toast->property("text").toString().contains(QStringLiteral("read-only")));

            // Another computer has "Theirs" open.
            const fs::path theirs = fs::path(dir.path().toStdU16String()) / "Theirs.wabcut";
            fs::copy_file(mine, theirs);
            {
                std::ofstream lock(editor::projectLockPath(theirs), std::ios::binary);
                lock << R"({"format": "wabcut-project-lock", "user": "alex", "host": "EDIT-BAY-2", "pid": 77, "since": "2026-10-16T09:00:00Z"})";
            }
            REQUIRE(project->open(QUrl::fromLocalFile(QString::fromStdU16String(theirs.u16string()))));
            spin(300);
            REQUIRE(project->readOnly());
            CHECK(badge->isVisible());
            CHECK(badge->property("label").toString() == QStringLiteral("Read-only"));
            CHECK(badge->property("tooltip").toString() ==
                  QStringLiteral("alex on EDIT-BAY-2 has this project open. Changes here are not autosaved; use Save As to keep them."));
            QQuickItem* title = visibleNamed(s.shell, QStringLiteral("projectTitle"));
            REQUIRE(title != nullptr);
            CHECK(title->width() > 0);
            CHECK(title->property("text").toString() == QStringLiteral("Theirs"));
            // The message: once, with Save As….
            CHECK(toast->property("text").toString().contains(QStringLiteral("alex on EDIT-BAY-2 has this project open, so it opened read-only")));
            CHECK(toast->property("actionText").toString() == QStringLiteral("Save As…"));
            {
                const QImage image = s.shell.grab(); // a window is grabbed once
                saveShot(image, s.shell.item("titleBar"), dark ? QStringLiteral("titlebar-read-only.png") : QStringLiteral("titlebar-read-only-light.png"));
                saveShot(image, s.shell.item("toast"), dark ? QStringLiteral("toast-read-only.png") : QStringLiteral("toast-read-only-light.png"), 4);
            }

            // The message does not repeat while the same project stays read-only (a dirty flag, a new revision).
            REQUIRE(QMetaObject::invokeMethod(toast, "dismiss"));
            spin(100);
            s.controller->selectAdjacentClip(1);
            s.controller->deleteSelection();
            spin(150);
            CHECK(project->readOnly());
            CHECK_FALSE(toast->property("shown").toBool()); // nothing was said again
            CHECK(badge->isVisible());

            // Save As makes it ours: the badge goes.
            const fs::path copy = fs::path(dir.path().toStdU16String()) / "Copy.wabcut";
            REQUIRE(saveSampleAs(project, copy));
            spin(200);
            CHECK_FALSE(project->readOnly());
            CHECK_FALSE(badge->isVisible());
            project->setLockProjects(false);
            const QStringList messages = takeMessages();
            INFO(join(messages));
            CHECK(messages.isEmpty());
            resetSettings();
        }
    }
}

// ---- 019: dropping files anywhere on the editor --------------------------------------------------------------------------

namespace {

/// Drags `mime` over `scenePos` of the window (enter and move); whether a drop target took it.
bool dragOver(QQuickWindow* w, QPoint scenePos, QMimeData* mime) {
    QDragEnterEvent enter(scenePos, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(w, &enter);
    QDragMoveEvent move(scenePos, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(w, &move);
    spin(30);
    return move.isAccepted();
}

void dropAt(QQuickWindow* w, QPoint scenePos, QMimeData* mime) {
    QDropEvent drop(QPointF(scenePos), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(w, &drop);
    spin(30);
}

void leave(QQuickWindow* w) {
    QDragLeaveEvent out;
    QCoreApplication::sendEvent(w, &out);
    spin(30);
}

QPoint centreOf(const QQuickItem* item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint(); }

} // namespace

TEST_CASE("Files dropped on a place no panel takes are imported; the overlay shows while they hover", "[app][import][drop][qml][d3][screenshots]") {
    const QString video = QDir(outputDir()).filePath(QStringLiteral("../media/h264_360p120.mp4"));
    if (!QFile::exists(video))
        FAIL("the shared test media is missing (build\\win-msvc\\test-output\\media): " << video.toStdString());
    for (const bool dark : {true, false}) {
        DYNAMIC_SECTION((dark ? "dark" : "light")) {
            resetSettings();
            clearMessages();
            EditorShell s(dark);
            QQuickItem* area = findItem(s.w->contentItem(), [](QQuickItem* i) { return i->objectName() == QLatin1String("windowFileDropArea"); });
            REQUIRE(area != nullptr);
            QQuickItem* overlay = findItem(s.w->contentItem(), [](QQuickItem* i) { return i->objectName() == QLatin1String("windowDropOverlay"); });
            REQUIRE(overlay != nullptr);
            CHECK_FALSE(overlay->isVisible());

            QMimeData files;
            files.setUrls({QUrl::fromLocalFile(video)});
            QQuickItem* paletteButton = visibleNamed(s.shell, QStringLiteral("commandPaletteButton"));
            REQUIRE(paletteButton != nullptr);
            const QPoint onTitleBar = centreOf(paletteButton);

            // Over the title bar: nothing there takes a file, so the window does.
            CHECK(dragOver(s.w, onTitleBar, &files));
            CHECK(overlay->isVisible());
            saveShot(s.shell.grab(), s.w->contentItem(), dark ? QStringLiteral("window-drop-overlay.png") : QStringLiteral("window-drop-overlay-light.png"));
            leave(s.w);
            CHECK_FALSE(overlay->isVisible());

            // Over the Library the panel takes it (its own outline): the window's overlay stays off.
            QQuickItem* library = s.shell.item("libraryPanel");
            REQUIRE(library != nullptr);
            REQUIRE(library->isVisible());
            CHECK(dragOver(s.w, centreOf(library), &files));
            CHECK_FALSE(overlay->isVisible());
            leave(s.w);

            // Drags inside the editor (media, clips) carry other keys and never light it.
            QMimeData internal;
            internal.setData(QStringLiteral("application/x-wabcut-media"), "m1");
            dragOver(s.w, onTitleBar, &internal);
            CHECK_FALSE(overlay->isVisible());
            leave(s.w);

            // The drop imports: the file is in the project, and a message says so.
            const std::size_t before = s.controller->snapshot()->media.size();
            REQUIRE(dragOver(s.w, onTitleBar, &files));
            dropAt(s.w, onTitleBar, &files);
            REQUIRE(waitFor([&] { return s.controller->snapshot()->media.size() > 0 && s.controller->snapshot()->media.size() != before; }, 20000));
            spin(200);
            CHECK_FALSE(overlay->isVisible());
            CHECK(s.controller->snapshot()->media.size() >= 1);
            QObject* toast = s.shell.object("toast");
            REQUIRE(toast != nullptr);
            INFO(toast->property("text").toString().toStdString());
            CHECK(toast->property("text").toString().contains(QStringLiteral("Imported 1 file")));
            const QStringList messages = takeMessages();
            INFO(join(messages));
            CHECK(messages.isEmpty());
            resetSettings();
        }
    }
}

// ---- 101: an Audio Track Mixer chain as a preset --------------------------------------------------------------------------

TEST_CASE("An effect chain of a mixer strip is saved as a preset and added to another strip", "[app][audio][presets][d3]") {
    QTemporaryDir libraryDir;
    REQUIRE(libraryDir.isValid());
    qputenv("WABCUT_EFFECTS_LIBRARY", QDir(libraryDir.path()).filePath(QStringLiteral("effects-library.json")).toUtf8());
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* mixer = shell.engine.singletonInstance<AudioMixerBridge*>("Wabcut", "AudioMixer");
    auto* presets = shell.engine.singletonInstance<EffectPresetsBridge*>("Wabcut", "EffectPresets");
    REQUIRE(controller != nullptr);
    REQUIRE(mixer != nullptr);
    REQUIRE(presets != nullptr);
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
        A1 tgt |Aaaaaaaaaaaaaaaaaaaa|
        A2     |Bbbbbbbbbbbbbbbbbbbb|
    )");
    controller->resetProject(tl.state, tl.seq);
    const QString a1 = QStringLiteral("t%1").arg(tl.track("A1").value);
    const QString a2 = QStringLiteral("t%1").arg(tl.track("A2").value);
    const auto chain = [&](const QString& track) { return controller->currentSequence()->findAudioBus(TrackId{track.mid(1).toULongLong()})->effects; };

    CHECK(presets->audioPresets().isEmpty());
    CHECK(presets->saveTrackEffects(a1, QStringLiteral("Empty"), {}, {}, {}).contains(QStringLiteral("no effects"))); // nothing to save yet
    REQUIRE(mixer->addEffect(a1, QStringLiteral("wabcut.audio.compressor")).isEmpty());
    REQUIRE(mixer->addEffect(a1, QStringLiteral("wabcut.audio.eq3")).isEmpty());
    const std::size_t undoBefore = controller->session().undoCount();

    // Saving keeps the chain in the user's library and is not an edit of the project.
    REQUIRE(presets->saveTrackEffects(a1, QStringLiteral("Voice chain"), QStringLiteral("anchorToInPoint"), QStringLiteral("Compress, then EQ"), {}).isEmpty());
    CHECK(controller->session().undoCount() == undoBefore);
    const QVariantList audio = presets->audioPresets();
    REQUIRE(audio.size() == 1);
    CHECK(audio.front().toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Voice chain"));
    CHECK(audio.front().toMap().value(QStringLiteral("effects")).toInt() == 2);
    // The name is taken now.
    CHECK_FALSE(presets->saveTrackEffects(a1, QStringLiteral("voice chain"), {}, {}, {}).isEmpty());

    // Added to another strip: one undo step with a clear name.
    REQUIRE(chain(a2).empty());
    REQUIRE(presets->applyToTrack(QStringLiteral("Voice chain"), a2).isEmpty());
    REQUIRE(chain(a2).size() == 2);
    CHECK(chain(a2)[0].effect == "wabcut.audio.compressor");
    CHECK(controller->session().undoCount() == undoBefore + 1);
    CHECK(controller->undoText() == QStringLiteral("Undo Apply Preset to Tracks"));
    REQUIRE(controller->undo());
    CHECK(chain(a2).empty());
    // The master takes a chain too; an unknown preset or strip is refused.
    REQUIRE(presets->applyToTrack(QStringLiteral("Voice chain"), QStringLiteral("master")).isEmpty());
    CHECK(controller->currentSequence()->audioEffects.size() == 2);
    CHECK_FALSE(presets->applyToTrack(QStringLiteral("No such preset"), a2).isEmpty());
    CHECK_FALSE(presets->applyToTrack(QStringLiteral("Voice chain"), QStringLiteral("t999999")).isEmpty());
    // The chain shows in the mixer's strips.
    REQUIRE(mixer->addEffect(a2, QStringLiteral("wabcut.audio.limiter")).isEmpty());
    spin(50);
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
    qunsetenv("WABCUT_EFFECTS_LIBRARY");
    resetSettings();
}

TEST_CASE("The mixer strip's Presets row saves and adds a chain; screenshots", "[app][audio][presets][qml][d3][screenshots]") {
    // The effects library is one per process (the first Presets bridge fixes its file), so both themes share one folder.
    QTemporaryDir libraryDir;
    REQUIRE(libraryDir.isValid());
    qputenv("WABCUT_EFFECTS_LIBRARY", QDir(libraryDir.path()).filePath(QStringLiteral("effects-library.json")).toUtf8());
    for (const bool dark : {true, false}) {
        DYNAMIC_SECTION((dark ? "dark" : "light")) {
            const QString presetName = dark ? QStringLiteral("Voice chain") : QStringLiteral("Voice chain light");
            resetSettings();
            clearMessages();
            EditorShell s(dark);
            auto* mixer = s.shell.engine.singletonInstance<AudioMixerBridge*>("Wabcut", "AudioMixer");
            auto* presets = s.shell.engine.singletonInstance<EffectPresetsBridge*>("Wabcut", "EffectPresets");
            REQUIRE(mixer != nullptr);
            REQUIRE(presets != nullptr);
            Timeline tl(R"(
                V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
                A1 tgt |Aaaaaaaaaaaaaaaaaaaa|
                A2     |Bbbbbbbbbbbbbbbbbbbb|
            )");
            s.controller->resetProject(tl.state, tl.seq);
            const QString a1 = QStringLiteral("t%1").arg(tl.track("A1").value);
            REQUIRE(mixer->addEffect(a1, QStringLiteral("wabcut.audio.compressor")).isEmpty());
            REQUIRE(presets->saveTrackEffects(a1, presetName, {}, {}, {}).isEmpty());
            evaluate(s.shell, QStringLiteral("window.openPreset('audio')"));
            spin(400);
            REQUIRE_FALSE(s.shell.grab().isNull());
            QQuickItem* slot = nullptr;
            REQUIRE(waitFor([&] { return (slot = visibleNamed(s.shell, QStringLiteral("effectPresetsSlot"))) != nullptr; }, 5000));
            CHECK(slot->isVisible());
            // The menu: Save effects as preset…, then the preset to add.
            REQUIRE(QMetaObject::invokeMethod(slot, "clicked"));
            QObject* menu = nullptr;
            REQUIRE(waitFor([&] { return (menu = slot->findChild<QObject*>(QStringLiteral("effectPresetsMenu"))) != nullptr && shown(menu); }, 3000));
            QStringList texts;
            const int count = menu->property("count").toInt();
            for (int i = 0; i < count; ++i) {
                QQuickItem* item = nullptr;
                REQUIRE(QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, i)));
                if (item)
                    texts << item->property("text").toString();
            }
            INFO(texts.join(QLatin1String(" | ")).toStdString());
            CHECK(texts.contains(QStringLiteral("Save effects as preset…")));
            CHECK(texts.contains(QStringLiteral("Add “%1”").arg(presetName)));
            spin(150);
            saveShot(s.shell.grab(), s.w->contentItem(), dark ? QStringLiteral("mixer-presets-menu.png") : QStringLiteral("mixer-presets-menu-light.png"));
            REQUIRE(QMetaObject::invokeMethod(menu, "close"));
            const QStringList messages = takeMessages();
            INFO(join(messages));
            CHECK(messages.isEmpty());
            qunsetenv("WABCUT_EFFECTS_LIBRARY");
            resetSettings();
        }
    }
}

// ---- 115: looks for every project -----------------------------------------------------------------------------------------

TEST_CASE("Caption looks are kept for every project, applied by copying, and shared as files", "[app][captions][presets][d3]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString library = QDir(dir.path()).filePath(QStringLiteral("caption-presets.json"));
    qputenv("WABCUT_CAPTION_PRESETS", library.toUtf8());
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* captions = shell.engine.singletonInstance<CaptionsBridge*>("Wabcut", "Captions");
    REQUIRE(controller != nullptr);
    REQUIRE(captions != nullptr);
    Timeline tl{"V1 tgt |A" + std::string(99, 'a') + "|\n"};
    controller->resetProject(tl.state, tl.seq);
    REQUIRE(captions->addTrack(QStringLiteral("English"), {}).isEmpty());
    const auto row = [&](const QString& name) {
        for (const QVariant& v : captions->presets()) {
            if (v.toMap().value(QStringLiteral("name")).toString() == name)
                return v.toMap();
        }
        return QVariantMap{};
    };

    // A look of this project, in My looks.
    REQUIRE(captions->setStyle(QStringLiteral("fontWeight"), 800).isEmpty());
    REQUIRE(captions->savePreset(QStringLiteral("Channel look")).isEmpty());
    CHECK(captions->libraryCount() == 0);
    CHECK_FALSE(row(QStringLiteral("Channel look")).value(QStringLiteral("inLibrary")).toBool());
    REQUIRE(captions->setInLibrary(QStringLiteral("Channel look"), true).isEmpty());
    CHECK(captions->libraryCount() == 1);
    CHECK(row(QStringLiteral("Channel look")).value(QStringLiteral("inLibrary")).toBool());
    CHECK_FALSE(row(QStringLiteral("Channel look")).value(QStringLiteral("libraryOnly")).toBool()); // the project has it too
    CHECK(QFile::exists(library));
    CHECK_FALSE(captions->setInLibrary(QStringLiteral("Clean"), true).isEmpty()); // built-ins are in every project already
    CHECK_FALSE(captions->setInLibrary(QStringLiteral("No such look"), true).isEmpty());

    // Another project: the look is in its gallery, from My looks; applying it copies it in (one undo step).
    Timeline other{"V1 tgt |A" + std::string(99, 'a') + "|\n"};
    controller->resetProject(other.state, other.seq);
    REQUIRE(captions->addTrack(QStringLiteral("English"), {}).isEmpty());
    CHECK(controller->snapshot()->captionPresets.empty());
    const QVariantMap shared = row(QStringLiteral("Channel look"));
    REQUIRE_FALSE(shared.isEmpty());
    CHECK(shared.value(QStringLiteral("libraryOnly")).toBool());
    CHECK(shared.value(QStringLiteral("inLibrary")).toBool());
    CHECK(shared.value(QStringLiteral("fontWeight")).toInt() == 800);
    const std::size_t undoBefore = controller->session().undoCount();
    REQUIRE(captions->applyPreset(QStringLiteral("channel look")).isEmpty());
    REQUIRE(controller->snapshot()->findCaptionPreset("Channel look") != nullptr); // copied into the project
    CHECK(controller->currentSequence()->captionTracks.front().style.fontWeight == 800); // and applied
    CHECK(controller->session().undoCount() == undoBefore + 1);                           // as one step
    CHECK_FALSE(row(QStringLiteral("Channel look")).value(QStringLiteral("libraryOnly")).toBool());
    REQUIRE(controller->undo());
    CHECK(controller->snapshot()->captionPresets.empty());
    CHECK(controller->currentSequence()->captionTracks.front().style.fontWeight != 800);

    // Export writes My looks and the project's looks; import adds a file's looks to My looks.
    REQUIRE(captions->setStyle(QStringLiteral("fontWeight"), 500).isEmpty());
    REQUIRE(captions->savePreset(QStringLiteral("Quiet")).isEmpty());
    const QString file = QDir(dir.path()).filePath(QStringLiteral("shared"));
    REQUIRE(captions->exportPresets(QUrl::fromLocalFile(file)).isEmpty());
    const QString written = file + QString::fromUtf8(io::kCaptionPresetsFileSuffix.data(), static_cast<qsizetype>(io::kCaptionPresetsFileSuffix.size()));
    REQUIRE(QFile::exists(written));
    const auto looks = io::captionPresetsFromJson(fileText(fs::path(written.toStdU16String())).toStdString());
    REQUIRE(looks);
    CHECK(looks->size() == 2); // Channel look (from My looks) and Quiet (of this project)
    REQUIRE(captions->setInLibrary(QStringLiteral("Channel look"), false).isEmpty());
    CHECK(captions->libraryCount() == 0);
    REQUIRE(captions->importPresets(QUrl::fromLocalFile(written)).isEmpty());
    CHECK(captions->libraryCount() == 2);
    CHECK(captions->message().contains(QStringLiteral("2 look")));
    // A file that is not caption presets is refused and My looks stays.
    const QString junk = QDir(dir.path()).filePath(QStringLiteral("junk.json"));
    std::ofstream(fs::path(junk.toStdU16String())) << "{ nope";
    CHECK_FALSE(captions->importPresets(QUrl::fromLocalFile(junk)).isEmpty());
    CHECK_FALSE(captions->importPresets(QUrl::fromLocalFile(QDir(dir.path()).filePath(QStringLiteral("missing.json")))).isEmpty());
    CHECK(captions->libraryCount() == 2);
    // A broken library file is left alone and reads as empty.
    std::ofstream(fs::path(library.toStdU16String())) << "{ damaged";
    CHECK(captions->libraryCount() == 0);
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
    qunsetenv("WABCUT_CAPTION_PRESETS");
    resetSettings();
}

TEST_CASE("The Caption Style panel marks looks of My looks and imports and exports them; screenshots",
          "[app][captions][presets][qml][d3][screenshots]") {
    for (const bool dark : {true, false}) {
        DYNAMIC_SECTION((dark ? "dark" : "light")) {
            QTemporaryDir dir;
            REQUIRE(dir.isValid());
            qputenv("WABCUT_CAPTION_PRESETS", QDir(dir.path()).filePath(QStringLiteral("caption-presets.json")).toUtf8());
            resetSettings();
            clearMessages();
            EditorShell s(dark);
            auto* captions = s.shell.engine.singletonInstance<CaptionsBridge*>("Wabcut", "Captions");
            REQUIRE(captions != nullptr);
            Timeline tl{"V1 tgt |A" + std::string(99, 'a') + "|\n"};
            s.controller->resetProject(tl.state, tl.seq);
            REQUIRE(captions->addTrack(QStringLiteral("English"), {}).isEmpty());
            REQUIRE(captions->setStyle(QStringLiteral("fontWeight"), 800).isEmpty());
            REQUIRE(captions->savePreset(QStringLiteral("Channel look")).isEmpty());
            REQUIRE(captions->savePreset(QStringLiteral("Second look")).isEmpty());
            REQUIRE(captions->setInLibrary(QStringLiteral("Channel look"), true).isEmpty());
            evaluate(s.shell, QStringLiteral("window.openPreset('captions')"));
            spin(400);
            REQUIRE_FALSE(s.shell.grab().isNull());
            QQuickItem* gallery = nullptr;
            REQUIRE(waitFor([&] { return (gallery = visibleNamed(s.shell, QStringLiteral("captionPresets"))) != nullptr; }, 5000));
            QQuickItem* mark = visibleNamed(s.shell, QStringLiteral("captionPresetLibrary_Channel look"));
            REQUIRE(mark != nullptr); // lit while the look is in My looks
            CHECK(mark->property("tooltip").toString().contains(QStringLiteral("In My looks")));
            QQuickItem* keep = visibleNamed(s.shell, QStringLiteral("captionSaveToLibrary"));
            REQUIRE(keep != nullptr);
            CHECK(keep->property("checked").toBool());
            REQUIRE(visibleNamed(s.shell, QStringLiteral("captionImportLooks")) != nullptr);
            REQUIRE(visibleNamed(s.shell, QStringLiteral("captionExportLooks")) != nullptr);
            QQuickItem* count = visibleNamed(s.shell, QStringLiteral("captionLibraryCount"));
            REQUIRE(count != nullptr);
            CHECK(count->property("text").toString() == QStringLiteral("1 look in My looks")); // the English plural form (A3)
            // Saving with the toggle on keeps the new look in My looks too.
            QQuickItem* name = visibleNamed(s.shell, QStringLiteral("captionPresetName"));
            QQuickItem* save = visibleNamed(s.shell, QStringLiteral("captionSavePreset"));
            REQUIRE(name != nullptr);
            REQUIRE(save != nullptr);
            name->setProperty("text", QStringLiteral("Third look"));
            REQUIRE(QMetaObject::invokeMethod(save, "clicked"));
            spin(150);
            CHECK(captions->libraryCount() == 2);
            // And off, it does not.
            REQUIRE(QMetaObject::invokeMethod(keep, "toggle"));
            REQUIRE(QMetaObject::invokeMethod(keep, "toggled"));
            name->setProperty("text", QStringLiteral("Fourth look"));
            REQUIRE(QMetaObject::invokeMethod(save, "clicked"));
            spin(150);
            CHECK(captions->libraryCount() == 2);
            CHECK(captions->presets().size() >= 8);
            // Scroll the panel to the rows under the gallery, then look at the whole window.
            for (QQuickItem* p = keep->parentItem(); p; p = p->parentItem()) {
                if (p->property("contentY").isValid() && p->property("contentHeight").isValid()) {
                    const double most = std::max(0.0, p->property("contentHeight").toDouble() - p->height());
                    QQuickItem* content = p->property("contentItem").value<QQuickItem*>();
                    p->setProperty("contentY", std::clamp(keep->mapToItem(content, QPointF(0, 0)).y() - 150.0, 0.0, most));
                    break;
                }
            }
            spin(250);
            saveShot(s.shell.grab(), s.w->contentItem(), dark ? QStringLiteral("caption-my-looks.png") : QStringLiteral("caption-my-looks-light.png"));
            const QStringList messages = takeMessages();
            INFO(join(messages));
            CHECK(messages.isEmpty());
            qunsetenv("WABCUT_CAPTION_PRESETS");
            resetSettings();
        }
    }
}

// ---- 061: custom words ---------------------------------------------------------------------------------------------------

TEST_CASE("Custom words for the transcriber are the project's, one undo step each, and come back with undo", "[app][transcript][words][d3]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* t = shell.engine.singletonInstance<TranscriptBridge*>("Wabcut", "Transcript");
    REQUIRE(controller != nullptr);
    REQUIRE(t != nullptr);
    Timeline tl{"V1 tgt |A" + std::string(49, 'a') + "|\nA1 tgt |A" + std::string(49, 'a') + "|\n"};
    controller->resetProject(tl.state, tl.seq);
    CHECK(t->customWords().isEmpty());
    CHECK(t->customWordCount() == 0);
    int changedCount = 0;
    QObject::connect(t, &TranscriptBridge::customWordsChanged, t, [&] { ++changedCount; });

    const std::size_t undoBefore = controller->session().undoCount();
    REQUIRE(t->setCustomWords(QStringLiteral("Malenia, Elden Ring\n  Radahn  \nmalenia")).isEmpty());
    CHECK(t->customWords() == QStringLiteral("Malenia\nElden Ring\nRadahn"));
    CHECK(t->customWordCount() == 3);
    CHECK(controller->snapshot()->customWords == std::vector<std::string>{"Malenia", "Elden Ring", "Radahn"});
    CHECK(controller->session().undoCount() == undoBefore + 1);
    CHECK(controller->undoText() == QStringLiteral("Undo Custom Words"));
    CHECK(changedCount >= 1);
    // Undo: the words go, and the field reads the same.
    REQUIRE(controller->undo());
    CHECK(t->customWordCount() == 0);
    CHECK(t->customWords().isEmpty());
    REQUIRE(controller->redo());
    CHECK(t->customWordCount() == 3);
    // Cleared by an empty field; too long is refused and the words stay.
    CHECK_FALSE(t->setCustomWords(QString(500, QLatin1Char('x'))).isEmpty());
    CHECK(t->customWordCount() == 3);
    REQUIRE(t->setCustomWords(QString()).isEmpty());
    CHECK(t->customWordCount() == 0);
    // They are saved with the project.
    REQUIRE(t->setCustomWords(QStringLiteral("Godrick")).isEmpty());
    const auto json = io::toJson(*controller->snapshot());
    REQUIRE(json);
    CHECK(json->find("\"customWords\"") != std::string::npos);
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
    resetSettings();
}

TEST_CASE("The Transcript tab's Custom words row opens a list to edit; screenshots", "[app][transcript][words][qml][d3][screenshots]") {
    for (const bool dark : {true, false}) {
        DYNAMIC_SECTION((dark ? "dark" : "light")) {
            resetSettings();
            QSettings().remove(QStringLiteral("transcript"));
            clearMessages();
            EditorShell s(dark);
            auto* t = s.shell.engine.singletonInstance<TranscriptBridge*>("Wabcut", "Transcript");
            REQUIRE(t != nullptr);
            Timeline tl(R"(
                V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
                A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
            )");
            tl.linkMatching();
            s.controller->resetProject(tl.state, tl.seq);
            REQUIRE(QMetaObject::invokeMethod(s.w, "showBrowserPanel", Q_ARG(QVariant, QStringLiteral("captions"))));
            spin(300);
            REQUIRE_FALSE(s.shell.grab().isNull());
            QQuickItem* button = visibleNamed(s.shell, QStringLiteral("customWordsButton"));
            REQUIRE(button != nullptr);
            CHECK(button->property("text").toString() == QStringLiteral("None"));
            QQuickItem* setup = visibleNamed(s.shell, QStringLiteral("transcribeSetup"));
            REQUIRE(setup != nullptr);
            QQuickItem* setupContent = setup->property("contentItem").value<QQuickItem*>();
            REQUIRE(setupContent != nullptr);
            const double most = std::max(0.0, setup->property("contentHeight").toDouble() - setup->height());
            setup->setProperty("contentY", std::clamp(button->mapToItem(setupContent, QPointF(0, 0)).y() - 200.0, 0.0, most));
            spin(100);
            REQUIRE(QMetaObject::invokeMethod(button, "clicked"));
            QQuickItem* field = nullptr;
            REQUIRE(waitFor([&] { return (field = visibleNamed(s.shell, QStringLiteral("customWordsField"))) != nullptr; }, 3000));
            field->setProperty("text", QStringLiteral("Malenia\nElden Ring\nRadahn"));
            spin(150);
            saveShot(s.shell.grab(), s.w->contentItem(), dark ? QStringLiteral("transcript-custom-words.png") : QStringLiteral("transcript-custom-words-light.png"));
            QQuickItem* save = visibleNamed(s.shell, QStringLiteral("customWordsSave"));
            REQUIRE(save != nullptr);
            REQUIRE(QMetaObject::invokeMethod(save, "clicked"));
            spin(150);
            CHECK(t->customWordCount() == 3);
            CHECK(button->property("text").toString() == QStringLiteral("3 words"));
            // Reopened, it shows what is there; a word that is too long is refused in the popover and it stays open.
            REQUIRE(QMetaObject::invokeMethod(button, "clicked"));
            REQUIRE(waitFor([&] { return (field = visibleNamed(s.shell, QStringLiteral("customWordsField"))) != nullptr; }, 3000));
            CHECK(field->property("text").toString() == QStringLiteral("Malenia\nElden Ring\nRadahn"));
            field->setProperty("text", QString(500, QLatin1Char('x')));
            save = visibleNamed(s.shell, QStringLiteral("customWordsSave"));
            REQUIRE(save != nullptr);
            REQUIRE(QMetaObject::invokeMethod(save, "clicked"));
            spin(100);
            QQuickItem* error = visibleNamed(s.shell, QStringLiteral("customWordsError"));
            REQUIRE(error != nullptr);
            CHECK_FALSE(error->property("text").toString().isEmpty());
            CHECK(t->customWordCount() == 3);
            const QStringList messages = takeMessages();
            INFO(join(messages));
            CHECK(messages.isEmpty());
            resetSettings();
        }
    }
}
