// 0.6 (P1 round, stream 1E) in the shell: menus, tooltips, panel focus, workspaces, History, updates and extensions.
//   * #33: the menu bar's and the palette's rows of the Editing bridge's commands and of the commands Main.qml routes
//     (Lift / Extract, Insert / Overwrite, Speed/Duration, Audio Gain / Channels, Play In to Out, Export Frame) grey
//     out with the reason the run path gives while there is nothing to act on; keys are not gated.
//   * #31: a disabled button's tooltip and a greyed menu row's tooltip say why; keyboard focus shows a button's tooltip
//     (a key hides it); a long tooltip wraps; the panel grip has a tooltip.
//   * #30: Ctrl+F searches the active panel (else the Library); Ctrl+F6 / Ctrl+Shift+F6 move between panel groups.
//   * #32: Save Changes to This Workspace / Reset to Saved Layout / Reset to <Preset> Preset / Lock Workspace.
//   * #4: History as a docked panel; Delete (Backspace) and Clear History ask first and cannot be undone.
//   * #5: compareVersions (a leading v, pre-releases), the Help menu's dot while a newer version is out, What's New
//     once after an update.

#include "ShellFixture.h"

#include "BridgeIds.h"
#include "CommandPalette.h"
#include "EditingBridge.h"
#include "LibraryBridge.h"
#include "ProjectController.h"
#include "SourceMonitorBridge.h"
#include "TimelineFixture.h"
#include "UpdateBridge.h"
#include "update/UpdateManifest.h"

#include <catch2/catch_test_macros.hpp>

#include <wabcut/model/EditOps.h>
#include <wabcut/model/FinishingOps.h>

#include <QCoreApplication>
#include <QDir>
#include <QKeyEvent>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QMouseEvent>
#include <QSettings>

#include <algorithm>
#include <string>
#include <tuple>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

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

QQuickItem* scene(const Shell& shell, const char* name) {
    QQuickItem* item = findNamed(shell.window->contentItem(), QString::fromLatin1(name));
    INFO(name);
    REQUIRE(item != nullptr);
    return item;
}

void runCommand(const Shell& shell, const char* id) {
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QString::fromLatin1(id))));
}

void key(QQuickWindow* window, int code, Qt::KeyboardModifiers mods = {}, const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, code, mods, text);
    QCoreApplication::sendEvent(window, &press);
    QKeyEvent release(QEvent::KeyRelease, code, mods, text);
    QCoreApplication::sendEvent(window, &release);
}

bool opened(QObject* popup) { return popup && popup->property("opened").toBool(); }
bool shown(QObject* popup) { return popup && popup->property("visible").toBool(); }

/// The pointer moves (no button) to `scene` in the window: hover.
void hoverAt(QQuickWindow* window, QPointF scene) {
    QMouseEvent event(QEvent::MouseMove, scene, scene, window->mapToGlobal(scene), Qt::NoButton, Qt::NoButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(window, &event);
    spin(30);
}

QPointF centre(const QQuickItem* item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)); }

/// The tooltip a control's Loader (`loaderName`) shows now, once it is visible (its delay), else null.
QObject* shownTip(QObject* control, const char* loaderName) {
    QObject* loader = control->findChild<QObject*>(QLatin1String(loaderName));
    INFO(loaderName);
    REQUIRE(loader != nullptr);
    QObject* tip = nullptr;
    waitFor([&] {
        tip = loader->property("item").value<QObject*>();
        return tip && tip->property("opened").toBool();
    }, 3000);
    return tip && tip->property("opened").toBool() ? tip : nullptr;
}

/// Whether the window's focus is in an item named `name` (or is it).
bool focusIn(const Shell& shell, const char* name) {
    for (QQuickItem* p = shell.window->activeFocusItem(); p; p = p->parentItem()) {
        if (p->objectName() == QLatin1String(name))
            return true;
    }
    return false;
}

void saveCrop(const Shell& shell, const QRectF& area, const QString& file) {
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    const double dpr = image.devicePixelRatio();
    const QRectF r = area.intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    REQUIRE(image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect()).save(QDir(outputDir()).filePath(file)));
}

QRectF sceneRect(const QQuickItem* item) { return item->mapRectToScene(QRectF(0, 0, item->width(), item->height())); }

/// The editor at 1600x960, motion off, with the singletons the tests drive.
struct P1Shell {
    Shell shell;
    QQuickWindow* w = nullptr;
    ProjectController* controller = nullptr;
    CommandPalette* palette = nullptr;

    P1Shell() {
        REQUIRE(shell.window != nullptr);
        INFO(join(shell.errors));
        REQUIRE(shell.errors.isEmpty());
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        w = shell.window.get();
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        palette = shell.engine.singletonInstance<CommandPalette*>("Wabcut", "CommandPalette");
        REQUIRE(controller != nullptr);
        REQUIRE(palette != nullptr);
        spin(50);
    }
    bool enabled(const char* id) const {
        return palette->commandState(QString::fromLatin1(id)).value(QStringLiteral("enabled")).toBool();
    }
    QString reason(const char* id) const {
        return palette->commandState(QString::fromLatin1(id)).value(QStringLiteral("reason")).toString();
    }
};

} // namespace

TEST_CASE("Menus: the Editing and routed rows grey out with the reason until they can act", "[app][menubar][p1]") {
    resetSettings();
    clearMessages();
    P1Shell s;
    auto* library = s.shell.engine.singletonInstance<LibraryBridge*>("Wabcut", "Library");
    auto* source = s.shell.engine.singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor");
    auto* editing = s.shell.engine.singletonInstance<EditingBridge*>("Wabcut", "Editing");
    REQUIRE(library != nullptr);
    REQUIRE(source != nullptr);
    REQUIRE(editing != nullptr);
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb|
        A1 tgt |Aaaa..Bbbb|
    )");
    tl.linkMatching();
    s.controller->resetProject(tl.state, tl.seq);
    s.controller->clearSelection();
    library->clearSelection();
    spin(50);

    // Nothing selected, no marks, no source clip, no history.
    const std::pair<const char*, const char*> greyed[] = {
        {"clip.setToFrameSize", "Select a clip first"},
        {"clip.scaleToFrameSize", "Select a clip first"},
        {"clip.frameHoldOptions", "Select a video clip"},
        {"clip.addFrameHold", "Select a video clip"},
        {"clip.fieldOptions", "Select a video clip"},
        {"edit.duplicate", "Select items in the Library to duplicate"},
        {"clip.makeOffline", "Select clips in the Library to make offline"},
        {"clip.interpretFootage", "Select clips in the Library to interpret"},
        {"clip.modifyTimecode", "Select clips in the Library"},
        {"clip.replaceFootage", "Select a clip in the Library"},
        {"file.newSequenceFromClip", "Select a clip in the Library"},
        {"file.newSequencesFromClips", "Select two or more clips in the Library"},       // 0.7 (card 055)
        {"file.newSequenceFromClipsTogether", "Select two or more clips in the Library"}, // 0.7 (card 055)
        {"clip.reloadFootage", "Select clips in the Library to reload"},                 // 0.7 (card 143)
        {"clip.mergeClips", "Select a video clip and the audio clips to merge with it"},
        {"edit.editOriginal", "Select a clip of a media file"},
        {"clip.revealInProject", "Select a clip first"},
        {"sequence.makeSubsequenceFromInToOut", "Set an In and an Out in the sequence first"},
        {"sequence.replaceFromBin", "Select the clip to replace"},
        {"history.stepBackward", "Nothing to undo"},
        {"history.stepForward", "Nothing to redo"},
        {"sequence.lift", "Set an In and an Out in the sequence first"},
        {"sequence.extract", "Set an In and an Out in the sequence first"},
        {"clip.insert", "No clip is open in the Source Monitor"},
        {"clip.overwrite", "No clip is open in the Source Monitor"},
        {"clip.speedDuration", "Select a clip first"},
        {"clip.audioGain", "Select a clip first"},
        {"clip.audioChannels", "Select a clip first"},
    };
    for (const auto& [id, why] : greyed) {
        INFO(id);
        CHECK_FALSE(s.enabled(id));
        CHECK(s.reason(id) == QString::fromLatin1(why));
    }
    // What needs only the open sequence: ready.
    for (const char* id : {"sequence.settings", "sequence.closeGap", "sequence.nextInSequence", "transport.playInToOut",
                           "file.exportFrame", "window.history", "edit.preferencesTimeline"}) {
        INFO(id);
        CHECK(s.enabled(id));
    }

    // The Clip menu shows it: Make Offline greyed, its reason for screen readers.
    QObject* bar = s.shell.object("appMenuBar");
    REQUIRE(bar != nullptr);
    REQUIRE(QMetaObject::invokeMethod(bar, "open", Q_ARG(int, 2), Q_ARG(bool, false)));
    QObject* clipMenu = s.shell.object("clipMenu");
    REQUIRE(waitFor([&] { return opened(clipMenu); }));
    QObject* offline = clipMenu->findChild<QObject*>(QStringLiteral("menuCommand_clip.makeOffline"));
    REQUIRE(offline != nullptr);
    CHECK_FALSE(offline->property("enabled").toBool());
    CHECK(offline->property("commandInfo").toMap().value(QStringLiteral("reason")).toString() ==
          QStringLiteral("Select clips in the Library to make offline"));
    REQUIRE(QMetaObject::invokeMethod(clipMenu, "close"));
    REQUIRE(waitFor([&] { return !shown(clipMenu); }));

    // A selected clip, a Library item, marks and a source clip: they come alive.
    s.controller->selectClip(clipKey(tl.clip("V1@0")));
    library->select(mediaKey(tl.media('A')), LibraryBridge::Replace);
    s.controller->setPlayhead(f(1));
    s.controller->markInAtPlayhead();
    s.controller->setPlayhead(f(3));
    s.controller->markOutAtPlayhead();
    REQUIRE(source->load(mediaKey(tl.media('B'))));
    for (const char* id : {"clip.setToFrameSize", "clip.frameHoldOptions", "edit.duplicate", "clip.makeOffline",
                           "clip.replaceFootage", "file.newSequenceFromClip", "edit.editOriginal", "clip.revealInProject",
                           "sequence.makeSubsequenceFromInToOut", "sequence.replaceFromBin",
                           "sequence.replaceFromSourceMonitor", "sequence.lift", "sequence.extract", "clip.insert",
                           "clip.speedDuration", "clip.audioGain", "clip.reloadFootage"}) {
        INFO(id << ": " << s.reason(id).toStdString());
        CHECK(s.enabled(id));
    }
    CHECK_FALSE(s.enabled("file.newSequencesFromClips")); // one clip is New Sequence From Clip
    // Two items: Merge Clips can run, and so can New Sequence per Clip. An edit: Step Backward can, Step Forward still cannot.
    library->select(mediaKey(tl.media('B')), LibraryBridge::Toggle);
    CHECK(s.enabled("clip.mergeClips"));
    CHECK(s.enabled("file.newSequencesFromClips"));
    CHECK(s.enabled("file.newSequenceFromClipsTogether"));
    REQUIRE(editing->scaleToFrameSize({}, true).isEmpty());
    CHECK(s.enabled("history.stepBackward"));
    CHECK_FALSE(s.enabled("history.stepForward"));

    // No gap left: Close Gap greys out. Keys are not gated: the command still runs and says why.
    s.controller->clearSelection();
    REQUIRE(editing->closeGap().isEmpty());
    CHECK_FALSE(s.enabled("sequence.closeGap"));
    CHECK(s.reason("sequence.closeGap") == QStringLiteral("There is no gap to close"));
    CHECK(editing->runCommand(QStringLiteral("sequence.closeGap")));
    CHECK_FALSE(editing->message().isEmpty());
    resetSettings();
}

TEST_CASE("Clean-up rows: Remove Transitions, Remove All Transitions, Remove Disabled Clips and Select All Disabled Clips (0.7)",
          "[app][menubar][cleanup]") {
    resetSettings();
    clearMessages();
    P1Shell s;
    auto* editing = s.shell.engine.singletonInstance<EditingBridge*>("Wabcut", "Editing");
    REQUIRE(editing != nullptr);
    Timeline tl(R"(
        V1 tgt |AaaaaaaaaaB5bbbbbbbbC5ccccccccc|
        A1 tgt |AaaaaaaaaaB5bbbbbbbbC5ccccccccc|
    )");
    tl.linkMatching();
    const auto reset = [&](const ProjectState& state) {
        s.controller->resetProject(state, tl.seq);
        s.controller->clearSelection();
        spin(50);
    };
    reset(tl.state);

    // The rows are in the menus.
    QObject* editMenu = s.shell.object("editMenu");
    QObject* sequenceMenu = s.shell.object("sequenceMenuBar");
    REQUIRE(editMenu != nullptr);
    REQUIRE(sequenceMenu != nullptr);
    CHECK(editMenu->findChild<QObject*>(QStringLiteral("menuCommand_edit.selectDisabledClips")) != nullptr);
    for (const char* id : {"sequence.removeTransitions", "sequence.removeAllTransitions", "sequence.removeDisabledClips"}) {
        INFO(id);
        CHECK(sequenceMenu->findChild<QObject*>(QStringLiteral("menuCommand_") + QString::fromLatin1(id)) != nullptr);
    }

    // Nothing to remove, nothing disabled: the rows grey out and say why.
    const std::pair<const char*, const char*> greyed[] = {
        {"sequence.removeTransitions", "Select clips first"},
        {"sequence.removeAllTransitions", "The sequence has no transitions to remove"},
        {"sequence.removeDisabledClips", "There are no disabled clips to remove"},
        {"edit.selectDisabledClips", "There are no disabled clips"},
    };
    for (const auto& [id, why] : greyed) {
        INFO(id);
        CHECK_FALSE(s.enabled(id));
        CHECK(s.reason(id) == QString::fromLatin1(why));
    }
    // Keys are not gated: run with nothing to do, the reason is the message and nothing changes.
    const std::size_t steps = s.controller->session().undoCount();
    CHECK(editing->runCommand(QStringLiteral("sequence.removeAllTransitions")));
    CHECK_FALSE(editing->message().isEmpty());
    CHECK(s.controller->session().undoCount() == steps);

    // Two dissolves on V1 and a crossfade on A1.
    ProjectState withTransitions = tl.state;
    for (const auto& [track, from, to] : {std::tuple{"V1", "V1@0", "V1@10"}, std::tuple{"V1", "V1@10", "V1@20"},
                                          std::tuple{"A1", "A1@0", "A1@10"}}) {
        edit::TransitionSpec spec;
        spec.outgoing = tl.clip(from);
        spec.incoming = tl.clip(to);
        spec.before = f(2);
        spec.after = f(2);
        spec.effect = std::string(track) == "V1" ? "crossDissolve" : "constantPower";
        withTransitions = ok(edit::addTransition(withTransitions, tl.seq, tl.track(track), spec)).state;
    }
    reset(withTransitions);
    const auto transitions = [&] {
        std::size_t n = 0;
        s.controller->currentSequence()->forEachTrack([&](const Track& t) { n += t.transitions.size(); });
        return n;
    };
    REQUIRE(transitions() == 3);
    CHECK(s.enabled("sequence.removeAllTransitions"));
    CHECK_FALSE(s.enabled("sequence.removeTransitions")); // nothing selected

    // The selected clips' transitions: B's picture (and its linked sound) touch the V1 dissolves and A1's crossfade.
    s.controller->selectClip(clipKey(tl.clip("V1@10")));
    CHECK(s.enabled("sequence.removeTransitions"));
    // The Sequence menu with its new rows (screenshots, dark and light).
    QObject* bar = s.shell.object("appMenuBar");
    REQUIRE(bar != nullptr);
    for (const bool dark : {true, false}) {
        s.shell.setAppearance(dark, "ultraviolet");
        spin(100);
        REQUIRE(QMetaObject::invokeMethod(bar, "open", Q_ARG(int, 3), Q_ARG(bool, false)));
        REQUIRE(waitFor([&] { return opened(sequenceMenu); }));
        spin(100);
        saveCrop(s.shell, QRectF(0, 0, 1000, 760),
                 dark ? QStringLiteral("sequence-menu-cleanup.png") : QStringLiteral("sequence-menu-cleanup-light.png"));
        REQUIRE(QMetaObject::invokeMethod(sequenceMenu, "close"));
        REQUIRE(waitFor([&] { return !shown(sequenceMenu); }));
    }
    s.shell.setAppearance(true, "ultraviolet");
    spin(100);
    const std::size_t before = s.controller->session().undoCount();
    REQUIRE(editing->removeTransitions(false).isEmpty());
    CHECK(transitions() == 0);
    CHECK(s.controller->session().undoCount() == before + 1);
    CHECK(s.controller->undoText().contains(QStringLiteral("Remove Transitions")));
    REQUIRE(s.controller->undo());
    CHECK(transitions() == 3);
    // The whole sequence, with the key's command.
    CHECK(editing->runCommand(QStringLiteral("sequence.removeAllTransitions")));
    CHECK(editing->message().isEmpty());
    CHECK(transitions() == 0);
    CHECK_FALSE(s.enabled("sequence.removeAllTransitions")); // none left
    REQUIRE(s.controller->undo());

    // Disabled clips: disable B (picture and sound, linked), find them, delete them.
    s.controller->selectClip(clipKey(tl.clip("V1@10")));
    REQUIRE(s.controller->runCommand(QStringLiteral("clip.enable")));
    s.controller->clearSelection();
    CHECK(s.enabled("edit.selectDisabledClips"));
    CHECK(s.enabled("sequence.removeDisabledClips"));
    REQUIRE(QMetaObject::invokeMethod(bar, "open", Q_ARG(int, 1), Q_ARG(bool, false)));
    REQUIRE(waitFor([&] { return opened(editMenu); }));
    spin(100);
    saveCrop(s.shell, QRectF(0, 0, 1000, 760), QStringLiteral("edit-menu-select-disabled.png"));
    REQUIRE(QMetaObject::invokeMethod(editMenu, "close"));
    REQUIRE(waitFor([&] { return !shown(editMenu); }));
    REQUIRE(s.controller->runCommand(QStringLiteral("edit.selectDisabledClips")));
    std::vector<ClipId> selected = s.controller->selection();
    std::ranges::sort(selected);
    std::vector<ClipId> expected{tl.clip("V1@10"), tl.clip("A1@10")};
    std::ranges::sort(expected);
    CHECK(selected == expected);
    const std::size_t beforeRemove = s.controller->session().undoCount();
    REQUIRE(editing->removeDisabledClips().isEmpty());
    CHECK(s.controller->session().undoCount() == beforeRemove + 1);
    CHECK(s.controller->undoText().contains(QStringLiteral("Remove Disabled Clips")));
    CHECK(s.controller->currentSequence()->videoTracks[0]->clips.size() == 2);
    CHECK(s.controller->currentSequence()->audioTracks[0]->clips.size() == 2);
    CHECK_FALSE(s.enabled("sequence.removeDisabledClips"));
    CHECK_FALSE(s.enabled("edit.selectDisabledClips"));
    CHECK_FALSE(s.controller->runCommand(QStringLiteral("no.such.command")));
    resetSettings();
}

TEST_CASE("Tooltips say why a button or a menu row is disabled and show on keyboard focus", "[app][tooltip][p1]") {
    resetSettings();
    clearMessages();
    P1Shell s;
    Timeline tl(R"(
        V1 tgt |Aaaaaaaa|
    )");
    s.controller->resetProject(tl.state, tl.seq);
    spin(50);

    // Undo with nothing to undo: disabled, and its tooltip says why (after its name).
    QQuickItem* undo = scene(s.shell, "undoButton");
    REQUIRE_FALSE(undo->isEnabled());
    CHECK(undo->property("disabledReason").toString() == QStringLiteral("Nothing to undo"));
    hoverAt(s.w, centre(undo));
    QObject* tip = shownTip(undo, "iconButtonTip");
    REQUIRE(tip != nullptr);
    CHECK(tip->property("text").toString().endsWith(QStringLiteral("Nothing to undo")));
    CHECK(tip->property("shortcut").toString().isEmpty()); // no key chip on a button that cannot run
    saveCrop(s.shell, sceneRect(undo).adjusted(-160, -10, 220, 60), QStringLiteral("p1-tooltip-disabled-undo.png"));
    hoverAt(s.w, QPointF(4, s.w->height() - 4));
    CHECK(waitFor([&] { return !undo->findChild<QObject*>(QStringLiteral("iconButtonTip"))->property("active").toBool(); }));

    // Keyboard focus shows a button's tooltip; a key press hides it until the focus moves. A long one wraps.
    QQuickItem* add = scene(s.shell, "newWorkspaceButton");
    add->setProperty("tooltip", QStringLiteral("A new workspace from one of the presets (Edit, Color, Audio, Captions or "
                                               "Export) or as a copy of the one you are in, with its own panel layout"));
    add->forceActiveFocus(Qt::TabFocusReason);
    REQUIRE(add->property("visualFocus").toBool());
    tip = shownTip(add, "iconButtonTip");
    REQUIRE(tip != nullptr);
    auto* contentItem = tip->property("contentItem").value<QQuickItem*>();
    REQUIRE(contentItem != nullptr);
    QQuickItem* text = nullptr;
    for (QQuickItem* child : contentItem->childItems()) {
        if (child->objectName() == QLatin1String("tooltipText"))
            text = child;
    }
    REQUIRE(text != nullptr);
    CHECK(text->width() <= 320.5); // Theme.space(80)
    CHECK(text->property("lineCount").toInt() > 1);
    const QRectF tipRect = sceneRect(tip->property("contentItem").value<QQuickItem*>()->parentItem());
    saveCrop(s.shell, tipRect.united(sceneRect(add)).adjusted(-20, -20, 20, 20), QStringLiteral("p1-tooltip-wrapped.png"));
    key(s.w, Qt::Key_Shift);
    CHECK(waitFor([&] { return !add->findChild<QObject*>(QStringLiteral("iconButtonTip"))->property("active").toBool(); }));
    CHECK(add->hasActiveFocus());

    // A greyed menu row: its reason in a tooltip under the pointer (Clip > Make Offline, nothing in the Library chosen).
    QObject* bar = s.shell.object("appMenuBar");
    REQUIRE(QMetaObject::invokeMethod(bar, "open", Q_ARG(int, 2), Q_ARG(bool, false)));
    QObject* clipMenu = s.shell.object("clipMenu");
    REQUIRE(waitFor([&] { return opened(clipMenu); }));
    auto* offline = qobject_cast<QQuickItem*>(clipMenu->findChild<QObject*>(QStringLiteral("menuCommand_clip.makeOffline")));
    REQUIRE(offline != nullptr);
    REQUIRE_FALSE(offline->isEnabled());
    CHECK(offline->property("disabledReason").toString() == QStringLiteral("Select clips in the Library to make offline"));
    spin(100);
    hoverAt(s.w, centre(offline));
    tip = shownTip(offline, "menuItemReasonTip");
    REQUIRE(tip != nullptr);
    CHECK(tip->property("text").toString() == QStringLiteral("Select clips in the Library to make offline"));
    saveCrop(s.shell, sceneRect(offline).adjusted(-20, -70, 120, 20), QStringLiteral("p1-menu-row-reason.png"));
    REQUIRE(QMetaObject::invokeMethod(clipMenu, "close"));
    REQUIRE(waitFor([&] { return !shown(clipMenu); }));

    // The panel grip: a tooltip under the pointer.
    QQuickItem* grip = scene(s.shell, "dockGrip_library");
    hoverAt(s.w, centre(grip));
    tip = shownTip(grip, "dockGripTip");
    REQUIRE(tip != nullptr);
    CHECK(tip->property("text").toString() == QStringLiteral("Drag to move this panel group"));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    for (const QString& m : messages)
        CHECK_FALSE(m.contains(QStringLiteral("qml"), Qt::CaseInsensitive));
}

TEST_CASE("Find searches the active panel and the panel group keys move between groups", "[app][find][p1]") {
    resetSettings();
    clearMessages();
    P1Shell s;
    Timeline tl(R"(
        V1 tgt |Aaaaaaaa|
    )");
    s.controller->resetProject(tl.state, tl.seq);
    spin(50);

    // Effects active: Ctrl+F goes to its search, the text selected.
    runCommand(s.shell, "window.effects");
    spin(100);
    QQuickItem* effectsSearch = scene(s.shell, "effectsSearch");
    effectsSearch->setProperty("text", QStringLiteral("blur"));
    runCommand(s.shell, "window.timeline");
    spin(50);
    REQUIRE(waitFor([&] { return focusIn(s.shell, "timelineLanes"); }));
    // From the timeline (no search of its own): the Library's, as before.
    runCommand(s.shell, "edit.find");
    CHECK(waitFor([&] { return focusIn(s.shell, "librarySearchBar"); }));
    evaluate(s.shell, QStringLiteral("window.focusPanel('effects')"));
    spin(100);
    key(s.w, Qt::Key_F, Qt::ControlModifier);
    CHECK(waitFor([&] { return effectsSearch->hasActiveFocus(); }));
    CHECK(effectsSearch->property("selectedText").toString() == QStringLiteral("blur"));

    // Markers, Metadata, Callouts and Review: their own search. The Transcript with no transcript yet: the Library's.
    const std::pair<const char*, const char*> panels[] = {{"markers", "markersSearch"}, {"metadata", "metadataSearch"},
                                                          {"graphics", "graphicsSearch"}, {"review", "reviewSearch"},
                                                          {"captions", "librarySearchBar"}};
    for (const auto& [panel, field] : panels) {
        INFO(panel);
        evaluate(s.shell, QStringLiteral("window.focusPanel('%1')").arg(QLatin1String(panel)));
        spin(100);
        runCommand(s.shell, "edit.find");
        CHECK(waitFor([&] { return focusIn(s.shell, field); }));
    }

    // Ctrl+F6 / Ctrl+Shift+F6: the next / previous panel group, in layout order, round.
    const QStringList order = evaluate(s.shell, QStringLiteral("Workspaces.groupsOf(docking.tree).map(g => g.group)"))
                                  .toStringList();
    REQUIRE(order.size() >= 3);
    evaluate(s.shell, QStringLiteral("window.focusPanel(docking.shownPanel('%1'))").arg(order.front()));
    spin(100);
    const auto active = [&] { return evaluate(s.shell, QStringLiteral("docking.activeGroup")).toString(); };
    REQUIRE(active() == order.front());
    key(s.w, Qt::Key_F6, Qt::ControlModifier);
    CHECK(waitFor([&] { return active() == order.at(1); }));
    CHECK(waitFor([&] {
        return evaluate(s.shell, QStringLiteral("docking.groupOfItem(window.activeFocusItem)")).toString() == order.at(1);
    }));
    runCommand(s.shell, "window.previousPanelGroup");
    CHECK(waitFor([&] { return active() == order.front(); }));
    runCommand(s.shell, "window.previousPanelGroup"); // round to the last
    CHECK(waitFor([&] { return active() == order.back(); }));
    CHECK(waitFor([&] {
        return evaluate(s.shell, QStringLiteral("docking.groupOfItem(window.activeFocusItem)")).toString() == order.back();
    }));
    key(s.w, Qt::Key_F6, Qt::ControlModifier | Qt::ShiftModifier);
    CHECK(waitFor([&] { return active() == order.at(order.size() - 2); }));
    // The palette lists them as ready, with their keys.
    CHECK(s.enabled("window.nextPanelGroup"));
    CHECK(s.palette->commandState(QStringLiteral("window.nextPanelGroup")).value(QStringLiteral("shortcut")).toString() ==
          QStringLiteral("Ctrl+F6"));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    for (const QString& m : messages)
        CHECK_FALSE(m.contains(QStringLiteral("qml"), Qt::CaseInsensitive));
}

TEST_CASE("Workspaces: Save Changes keeps a layout to reset to; Reset to Preset and Lock Workspace", "[app][workspaces][p1]") {
    resetSettings();
    clearMessages();
    P1Shell s;
    Timeline tl(R"(
        V1 tgt |Aaaaaaaa|
    )");
    s.controller->resetProject(tl.state, tl.seq);
    spin(50);
    const auto eval = [&](const QString& code) { return evaluate(s.shell, code); };
    const auto isOpen = [&](const char* key) {
        return eval(QStringLiteral("docking.isOpen('%1')").arg(QLatin1String(key))).toBool();
    };
    const int id = eval(QStringLiteral("window.workspace")).toInt();
    REQUIRE(isOpen("audioMeters"));
    CHECK_FALSE(eval(QStringLiteral("Workspaces.hasSaved(window.workspace)")).toBool());

    // Close the Audio Meters, Save Changes; open them again: Reset to Saved Layout goes back to the saved layout.
    eval(QStringLiteral("docking.closePanel('audioMeters')"));
    REQUIRE_FALSE(isOpen("audioMeters"));
    eval(QStringLiteral("window.menuAction('saveWorkspaceChanges', window.workspace)"));
    CHECK(eval(QStringLiteral("Workspaces.hasSaved(window.workspace)")).toBool());
    eval(QStringLiteral("docking.openPanel('audioMeters')"));
    REQUIRE(isOpen("audioMeters"));
    runCommand(s.shell, "window.resetToSavedLayout");
    CHECK(waitFor([&] { return !isOpen("audioMeters"); }));
    // Kept with the workspace (version 2, the optional "saved" key).
    const QString kept = s.shell.preferences->property("workspaceLayout").toString();
    CHECK(kept.contains(QStringLiteral("\"version\":2")));
    CHECK(kept.contains(QStringLiteral("\"saved\":{")));

    // Reset to <Preset> Preset… asks first; then the preset's layout (the Audio Meters are back), the saved one stays.
    QObject* windowMenu = s.shell.object("windowMenu");
    REQUIRE(windowMenu != nullptr);
    QObject* presetRow = windowMenu->findChild<QObject*>(QStringLiteral("windowMenuResetWorkspaceToPreset"));
    REQUIRE(presetRow != nullptr);
    CHECK(presetRow->property("text").toString() == QStringLiteral("Reset to Edit Preset…"));
    eval(QStringLiteral("window.menuAction('resetWorkspaceToPreset', window.workspace)"));
    QObject* confirm = s.shell.object("menuConfirmDialog");
    REQUIRE(confirm != nullptr);
    REQUIRE(waitFor([&] { return opened(confirm); }));
    CHECK(confirm->property("text").toString().contains(QStringLiteral("Your saved layout stays")));
    QObject* yes = confirm->findChild<QObject*>(QStringLiteral("menuConfirmButton"));
    REQUIRE(yes != nullptr);
    REQUIRE(QMetaObject::invokeMethod(yes, "clicked"));
    CHECK(waitFor([&] { return isOpen("audioMeters"); }));
    CHECK(eval(QStringLiteral("Workspaces.hasSaved(window.workspace)")).toBool());

    // Lock Workspace: nothing closes, drags or resizes; the grips, tab closes and gutters are off; a lock on the tab;
    // opening a panel still works. Unlocked: closing works again.
    eval(QStringLiteral("window.menuAction('lockWorkspace', window.workspace)"));
    REQUIRE(eval(QStringLiteral("docking.locked")).toBool());
    CHECK(windowMenu->findChild<QObject*>(QStringLiteral("windowMenuLockWorkspace"))->property("checked").toBool());
    eval(QStringLiteral("docking.closePanel('audioMeters')"));
    CHECK(isOpen("audioMeters"));
    runCommand(s.shell, "file.close");
    CHECK(isOpen("audioMeters"));
    spin(100);
    CHECK_FALSE(scene(s.shell, "dockGrip_library")->isVisible());
    CHECK_FALSE(scene(s.shell, "dockClose_library")->isVisible());
    QQuickItem* splitter = scene(s.shell, "dockSplitter");
    CHECK(splitter->property("locked").toBool());
    const QString key = eval(QStringLiteral("Workspaces.keyOf(window.workspace)")).toString();
    QQuickItem* lock = scene(s.shell, QStringLiteral("workspaceLock_%1").arg(key).toLatin1().constData());
    CHECK(lock->isVisible());
    const QString before = eval(QStringLiteral("JSON.stringify(docking.tree)")).toString();
    eval(QStringLiteral("docking.shiftWeight([], 0, 1, 40, 400, 400, 50)"));
    CHECK(eval(QStringLiteral("JSON.stringify(docking.tree)")).toString() == before);
    eval(QStringLiteral("window.openPanelFromWindowMenu('markers')"));
    CHECK(waitFor([&] { return isOpen("markers"); }));
    spin(300); // the tab grows for its lock
    saveCrop(s.shell, sceneRect(lock).adjusted(-60, -12, 120, 12), QStringLiteral("p1-workspace-locked-tab.png"));
    // The Window menu's Workspaces submenu.
    QObject* bar = s.shell.object("appMenuBar");
    REQUIRE(QMetaObject::invokeMethod(bar, "open", Q_ARG(int, 7), Q_ARG(bool, false)));
    REQUIRE(waitFor([&] { return opened(windowMenu); }));
    QObject* workspacesMenu = windowMenu->findChild<QObject*>(QStringLiteral("windowMenuWorkspaces"));
    REQUIRE(workspacesMenu != nullptr);
    REQUIRE(QMetaObject::invokeMethod(workspacesMenu, "open"));
    REQUIRE(waitFor([&] { return opened(workspacesMenu); }));
    spin(200);
    auto* menuItem = qobject_cast<QQuickItem*>(workspacesMenu->property("contentItem").value<QObject*>());
    REQUIRE(menuItem != nullptr);
    auto* rootMenuItem = qobject_cast<QQuickItem*>(windowMenu->property("contentItem").value<QObject*>());
    saveCrop(s.shell, sceneRect(menuItem->parentItem()).united(sceneRect(rootMenuItem->parentItem())).adjusted(-8, -8, 8, 8),
             QStringLiteral("p1-window-workspaces-menu.png"));
    REQUIRE(QMetaObject::invokeMethod(windowMenu, "close"));
    REQUIRE(waitFor([&] { return !shown(windowMenu); }));

    eval(QStringLiteral("window.menuAction('lockWorkspace', window.workspace)"));
    REQUIRE_FALSE(eval(QStringLiteral("docking.locked")).toBool());
    CHECK_FALSE(lock->isVisible());
    eval(QStringLiteral("docking.closePanel('markers')"));
    CHECK_FALSE(isOpen("markers"));
    CHECK(id == eval(QStringLiteral("window.workspace")).toInt());

    // A version 2 workspace from before (no saved layout, no lock) loads as it was.
    const QVariant normalized = eval(QStringLiteral(
        "JSON.stringify(Workspaces.normalized({ id: 150, preset: 'audio', name: 'Old', layout: undefined }))"));
    CHECK(normalized.toString().contains(QStringLiteral("\"saved\":null")));
    CHECK(normalized.toString().contains(QStringLiteral("\"locked\":false")));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    for (const QString& m : messages)
        CHECK_FALSE(m.contains(QStringLiteral("qml"), Qt::CaseInsensitive));
    resetSettings();
}

TEST_CASE("History: a docked panel whose Delete and Clear History ask first and cannot be undone", "[app][history][p1]") {
    resetSettings();
    clearMessages();
    P1Shell s;
    auto* editing = s.shell.engine.singletonInstance<EditingBridge*>("Wabcut", "Editing");
    REQUIRE(editing != nullptr);
    Timeline tl(R"(
        V1 tgt |Aaaaaaaa|
    )");
    s.controller->resetProject(tl.state, tl.seq);
    s.controller->selectClip(clipKey(tl.clip("V1@0")));
    for (bool on : {true, false, true})
        REQUIRE(editing->scaleToFrameSize({}, on).isEmpty());
    REQUIRE(s.controller->session().undoCount() == 3);

    // Window > History: a panel of the workspace (a tab of the active group), focused in its key scope.
    runCommand(s.shell, "window.history");
    QObject* panel = nullptr;
    REQUIRE(waitFor([&] {
        panel = s.shell.window->findChild<QObject*>(QStringLiteral("historyPanel"));
        return panel && panel->property("opened").toBool();
    }));
    CHECK(evaluate(s.shell, QStringLiteral("docking.isOpen('history')")).toBool());
    CHECK(waitFor([&] {
        return evaluate(s.shell, QStringLiteral("KeymapDispatcher.scopeAt(window.activeFocusItem)")).toString() ==
               QStringLiteral("history");
    }));
    CHECK(s.shell.window->findChild<QObject*>(QStringLiteral("windowMenu_history")) != nullptr);

    // Back to step 2, then Delete (Backspace): it asks; Cancel keeps everything.
    REQUIRE(editing->goToHistoryStep(2));
    REQUIRE(s.controller->session().redoCount() == 1);
    QObject* confirm = s.shell.window->findChild<QObject*>(QStringLiteral("historyConfirmDialog"));
    REQUIRE(confirm != nullptr);
    key(s.w, Qt::Key_Backspace);
    REQUIRE(waitFor([&] { return opened(confirm); }));
    CHECK(confirm->property("title").toString() == QStringLiteral("Delete “Auto Fit to Frame”?"));
    REQUIRE(QMetaObject::invokeMethod(confirm->findChild<QObject*>(QStringLiteral("menuConfirmCancel")), "clicked"));
    REQUIRE(waitFor([&] { return !shown(confirm); }));
    CHECK(s.controller->session().undoCount() == 2);
    CHECK(s.controller->session().redoCount() == 1);

    // Delete, confirmed: step 2 and the one after it go; nothing to redo, and no undo step for it.
    REQUIRE(editing->runCommand(QStringLiteral("history.delete")));
    REQUIRE(waitFor([&] { return opened(confirm); }));
    QObject* yes = confirm->findChild<QObject*>(QStringLiteral("menuConfirmButton"));
    REQUIRE(QMetaObject::invokeMethod(yes, "clicked"));
    REQUIRE(waitFor([&] { return s.controller->session().undoCount() == 1; }));
    CHECK(s.controller->session().redoCount() == 0);
    CHECK_FALSE(s.controller->canRedo());
    CHECK(editing->historySteps().size() == 2);
    CHECK(s.controller->currentSequence()->findClip(tl.clip("V1@0"))->clip().content().scaleToFrame);
    QObject* list = panel->findChild<QObject*>(QStringLiteral("historySteps"));
    REQUIRE(list != nullptr);
    CHECK(waitFor([&] { return list->property("count").toInt() == 2; }));

    // Screenshots of the panel, dark and light.
    auto* panelItem = qobject_cast<QQuickItem*>(panel);
    REQUIRE(panelItem != nullptr);
    spin(200);
    saveCrop(s.shell, sceneRect(panelItem), QStringLiteral("p1-history-panel.png"));
    s.shell.setAppearance(false, "ultraviolet");
    REQUIRE(s.shell.settle(false, s.shell.themeColor("accentFill")));
    saveCrop(s.shell, sceneRect(panelItem), QStringLiteral("p1-history-panel-light.png"));
    s.shell.setAppearance(true, "ultraviolet");
    REQUIRE(s.shell.settle(true, s.shell.themeColor("accentFill")));

    // The panel menu: Clear History asks, then every step goes; the menu's rows grey out with the reason.
    QObject* menuButton = panel->findChild<QObject*>(QStringLiteral("historyMenuButton"));
    REQUIRE(menuButton != nullptr);
    REQUIRE(QMetaObject::invokeMethod(menuButton, "clicked"));
    QObject* menu = s.shell.window->findChild<QObject*>(QStringLiteral("historyPanelMenu"));
    REQUIRE(menu != nullptr);
    REQUIRE(waitFor([&] { return opened(menu); }));
    QObject* clearRow = menu->findChild<QObject*>(QStringLiteral("historyMenuClear"));
    REQUIRE(clearRow != nullptr);
    CHECK(clearRow->property("enabled").toBool());
    REQUIRE(QMetaObject::invokeMethod(clearRow, "triggered"));
    REQUIRE(QMetaObject::invokeMethod(menu, "close"));
    REQUIRE(waitFor([&] { return opened(confirm); }));
    CHECK(confirm->property("title").toString() == QStringLiteral("Clear the history?"));
    REQUIRE(QMetaObject::invokeMethod(confirm->findChild<QObject*>(QStringLiteral("menuConfirmButton")), "clicked"));
    REQUIRE(waitFor([&] { return !s.controller->canUndo(); }));
    CHECK(editing->historySteps().size() == 1);
    CHECK(s.controller->currentSequence()->findClip(tl.clip("V1@0"))->clip().content().scaleToFrame); // as it was
    CHECK_FALSE(s.enabled("history.clear"));
    CHECK(s.reason("history.delete") == QStringLiteral("The project as it was opened cannot be deleted"));
    REQUIRE(QMetaObject::invokeMethod(menuButton, "clicked"));
    REQUIRE(waitFor([&] { return opened(menu); }));
    CHECK_FALSE(menu->findChild<QObject*>(QStringLiteral("historyMenuDelete"))->property("enabled").toBool());
    REQUIRE(QMetaObject::invokeMethod(menu, "close"));
    REQUIRE(waitFor([&] { return !shown(menu); }));

    // Window > History again: it closes.
    runCommand(s.shell, "window.history");
    CHECK(waitFor([&] { return !panel->property("opened").toBool(); }));
    CHECK_FALSE(evaluate(s.shell, QStringLiteral("docking.isOpen('history')")).toBool());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    for (const QString& m : messages)
        CHECK_FALSE(m.contains(QStringLiteral("qml"), Qt::CaseInsensitive));
    resetSettings();
}

TEST_CASE("Updates: What's New shows once after an update and a first run only remembers the version", "[app][updates][p1]") {
    resetSettings();
    clearMessages();
    const QString appVersion = QCoreApplication::applicationVersion();
    QCoreApplication::setApplicationVersion(QStringLiteral("0.6.0"));
    {
        P1Shell s;
        QObject* whatsNew = s.shell.object("whatsNewDialog");
        REQUIRE(whatsNew != nullptr);
        const auto afterHome = [&] { return evaluate(s.shell, QStringLiteral("window.whatsNewAfterUpdate(true)")).toBool(); };
        // Updated from 0.5.3: What's New, once.
        s.shell.preferences->setProperty("lastSeenVersion", QStringLiteral("0.5.3"));
        CHECK(afterHome());
        CHECK(waitFor([&] { return opened(whatsNew); }));
        CHECK(s.shell.preferences->property("lastSeenVersion").toString() == QStringLiteral("0.6.0"));
        REQUIRE(QMetaObject::invokeMethod(whatsNew, "close"));
        REQUIRE(waitFor([&] { return !shown(whatsNew); }));
        CHECK_FALSE(afterHome());
        CHECK_FALSE(opened(whatsNew));
        // A pre-release of this version counts as older.
        s.shell.preferences->setProperty("lastSeenVersion", QStringLiteral("0.6.0-beta.1"));
        CHECK(afterHome());
        REQUIRE(waitFor([&] { return opened(whatsNew); }));
        REQUIRE(QMetaObject::invokeMethod(whatsNew, "close"));
        REQUIRE(waitFor([&] { return !shown(whatsNew); }));
        // A newer one (a downgrade): nothing.
        s.shell.preferences->setProperty("lastSeenVersion", QStringLiteral("0.7.0"));
        CHECK_FALSE(afterHome());
        // A first run (no version kept, the welcome not done): only remembered.
        s.shell.preferences->setProperty("lastSeenVersion", QString());
        s.shell.preferences->setProperty("welcomeDone", false);
        CHECK_FALSE(afterHome());
        CHECK(s.shell.preferences->property("lastSeenVersion").toString() == QStringLiteral("0.6.0"));
        // 0.5 kept no version: a Wabcut whose welcome ran is an update.
        s.shell.preferences->setProperty("lastSeenVersion", QString());
        s.shell.preferences->setProperty("welcomeDone", true);
        CHECK(afterHome());
        REQUIRE(waitFor([&] { return opened(whatsNew); }));
        REQUIRE(QMetaObject::invokeMethod(whatsNew, "close"));
        REQUIRE(waitFor([&] { return !shown(whatsNew); }));
    }
    QCoreApplication::setApplicationVersion(appVersion);
    resetSettings();
}
