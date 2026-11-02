// 0.6 (P1 round, task D9 #8): the beat tools in the shell: Sequence > Beat Grid (Detect Beats from Selected Clip…,
// Snap Selected Clips to Beats, Cut Selected Clips to Beats…, Clear Beat Grid), Clip > Detect Beats… and the clip
// right-click menu's Detect Beats…, the Timeline Display > Beat grid switch with its ruler ticks, the four keymap and
// palette rows with their reasons, and the edits (analysis.detectBeats, edit.snapToBeats, edit.cutToBeats,
// sequence.setBeatGrid clear), each one undo step. The analysis is a MapAnalysisProvider: no media is decoded.

#include "ShellFixture.h"
#include "BeatBridge.h"
#include "CommandPalette.h"
#include "ProjectController.h"
#include "TimelineFixture.h"
#include "TimelineView.h"

#include <wabcut/ai/analysis/AnalysisCommands.h>
#include <wabcut/ai/analysis/BeatDetection.h>
#include <wabcut/model/Sequence.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QImage>
#include <QQuickItem>

#include <string>

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

bool opened(QObject* popup) { return popup && popup->property("opened").toBool(); }
bool shown(QObject* popup) { return popup && popup->property("visible").toBool(); }

void runCommand(const Shell& shell, const char* id) {
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QString::fromLatin1(id))));
}

/// The clip's start after the edit in the current snapshot.
Ticks startOf(ProjectController* controller, ClipId clip) {
    const SequencePtr seq = controller->currentSequence();
    REQUIRE(seq);
    const auto loc = seq->findClip(clip);
    REQUIRE(loc);
    return loc->clip().start();
}

/// The editor at 1600x960 with the music clip on A1 and a shot on V1, and a beat analysis of 150 BPM (a beat every 10
/// frames) for the music's media, through the Beats bridge's synchronous switch.
struct BeatsShell {
    Shell shell;
    ProjectController* controller = nullptr;
    CommandPalette* palette = nullptr;
    BeatBridge* beats = nullptr;
    Timeline tl{
        R"(
            V1 tgt |.........Bbbbbbbbbb|
            A1 tgt |Mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm|
        )"};

    BeatsShell() {
        REQUIRE(shell.window != nullptr);
        INFO(join(shell.errors));
        REQUIRE(shell.errors.isEmpty());
        shell.resize(1600, 960);
        shell.theme->setProperty("reduceMotion", true);
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        palette = shell.engine.singletonInstance<CommandPalette*>("Wabcut", "CommandPalette");
        beats = shell.engine.singletonInstance<BeatBridge*>("Wabcut", "Beats");
        REQUIRE(controller != nullptr);
        REQUIRE(palette != nullptr);
        REQUIRE(beats != nullptr);

        auto analysis = std::make_shared<ai::analysis::MapAnalysisProvider>();
        ai::analysis::BeatAnalysis music;
        music.bpm = 150.0;
        music.confidence = 0.9F;
        music.beatsPerBar = 4;
        for (const std::int64_t frame : {0, 10, 20, 30, 40, 50, 60}) {
            music.beats.push_back(f(frame));
        }
        music.downbeats = {f(0), f(40)};
        music.duration = f(1000);
        analysis->addBeats(tl.media('M'), music);
        beats->setServices(std::make_shared<ai::analysis::AnalysisServices>(analysis));
        beats->setSynchronous(true);
        controller->resetProject(tl.state, tl.seq);
        spin(80);
    }

    bool enabled(const char* id) const {
        return palette->commandState(QString::fromLatin1(id)).value(QStringLiteral("enabled")).toBool();
    }
    QString reason(const char* id) const {
        return palette->commandState(QString::fromLatin1(id)).value(QStringLiteral("reason")).toString();
    }
};

} // namespace

TEST_CASE("Beats: the menus and grid toggle, every beat edit one undo step", "[app][menubar][beats]") {
    resetSettings();
    clearMessages();
    BeatsShell s;
    const ClipId music = s.tl.clip("A1@0");
    const ClipId shot = s.tl.clip("V1@9");

    // ---- Nothing selected: the rows say what they need. -----------------------------------------------------------
    s.controller->clearSelection();
    spin(20);
    CHECK_FALSE(s.enabled("sequence.detectBeats"));
    CHECK(s.reason("sequence.detectBeats") == QStringLiteral("Select a clip with sound"));
    CHECK(s.reason("edit.snapToBeats") == QStringLiteral("Detect beats first"));
    CHECK(s.reason("sequence.cutToBeats") == QStringLiteral("Detect beats first"));
    CHECK(s.reason("sequence.clearBeatGrid") == QStringLiteral("No beat grid to clear"));
    CHECK_FALSE(s.beats->hasGrid());
    CHECK(s.beats->gridInfo().isEmpty());

    // ---- Sequence > Beat Grid: the four rows (their shortcuts show; the state is Beats'). ---------------------------
    QObject* bar = s.shell.object("appMenuBar");
    REQUIRE(bar != nullptr);
    REQUIRE(QMetaObject::invokeMethod(bar, "open", Q_ARG(int, 3), Q_ARG(bool, false)));
    QObject* sequenceMenu = s.shell.object("sequenceMenuBar");
    REQUIRE(waitFor([&] { return opened(sequenceMenu); }));
    QObject* beatGrid = sequenceMenu->findChild<QObject*>(QStringLiteral("sequenceMenuBeatGrid"));
    REQUIRE(beatGrid != nullptr);
    CHECK(beatGrid->property("title").toString() == QStringLiteral("Beat Grid"));
    REQUIRE(QMetaObject::invokeMethod(beatGrid, "open"));
    REQUIRE(waitFor([&] { return beatGrid->property("visible").toBool(); }));
    for (const char* id : {"sequence.detectBeats", "edit.snapToBeats", "sequence.cutToBeats", "sequence.clearBeatGrid"}) {
        INFO(id);
        QObject* row = beatGrid->findChild<QObject*>(QStringLiteral("menuCommand_") + QString::fromLatin1(id));
        REQUIRE(row != nullptr);
        CHECK(row->property("command").toString() == QString::fromLatin1(id));
    }
    // Nothing selected and no grid: the rows say what they need, in the row itself.
    QObject* snapRow = beatGrid->findChild<QObject*>(QStringLiteral("menuCommand_edit.snapToBeats"));
    REQUIRE(snapRow != nullptr);
    CHECK_FALSE(snapRow->property("enabled").toBool());
    CHECK(snapRow->property("commandInfo").toMap().value(QStringLiteral("reason")).toString()
          == QStringLiteral("Detect beats first"));
    REQUIRE(QMetaObject::invokeMethod(sequenceMenu, "close"));
    REQUIRE(waitFor([&] { return !shown(sequenceMenu); }));
    const QStringList menuMessages = takeMessages();
    INFO(join(menuMessages));
    CHECK(menuMessages.isEmpty());

    // ---- Clip > Detect Beats…: greyed with the reason until a clip with sound is selected. --------------------------
    QObject* clipMenu = s.shell.object("clipMenu");
    REQUIRE(QMetaObject::invokeMethod(bar, "open", Q_ARG(int, 2), Q_ARG(bool, false)));
    REQUIRE(waitFor([&] { return opened(clipMenu); }));
    QObject* clipDetect = clipMenu->findChild<QObject*>(QStringLiteral("menuCommand_sequence.detectBeats"));
    REQUIRE(clipDetect != nullptr);
    CHECK_FALSE(clipDetect->property("enabled").toBool());
    CHECK(clipDetect->property("commandInfo").toMap().value(QStringLiteral("reason")).toString()
          == QStringLiteral("Select a clip with sound"));
    REQUIRE(QMetaObject::invokeMethod(clipMenu, "close"));
    REQUIRE(waitFor([&] { return !shown(clipMenu); }));

    // ---- The clip right-click menu offers Detect Beats… on the music clip. ------------------------------------------
    s.controller->selectClip(clipKey(music));
    spin(20);
    CHECK(s.enabled("sequence.detectBeats"));
    CHECK(s.beats->detectInfo().value(QStringLiteral("clip")).toString() == clipKey(music));
    QObject* contextMenu = s.shell.object("clipContextMenu");
    QQuickItem* lanes = findNamed(s.shell.window->contentItem(), QStringLiteral("timelineLanesArea"));
    REQUIRE(contextMenu != nullptr);
    REQUIRE(lanes != nullptr);
    REQUIRE(QMetaObject::invokeMethod(contextMenu, "openAt", Q_ARG(QVariant, QVariant::fromValue<QObject*>(lanes)),
                                      Q_ARG(QVariant, clipKey(music)), Q_ARG(QVariant, QPointF(200, 20))));
    REQUIRE(waitFor([&] { return opened(contextMenu); }));
    QObject* contextDetect = contextMenu->findChild<QObject*>(QStringLiteral("detectBeatsMenuItem"));
    REQUIRE(contextDetect != nullptr);
    CHECK(contextDetect->property("visible").toBool());
    CHECK(contextDetect->property("command").toString() == QStringLiteral("sequence.detectBeats"));
    REQUIRE(QMetaObject::invokeMethod(contextMenu, "close"));
    REQUIRE(waitFor([&] { return !shown(contextMenu); }));

    // ---- Detect Beats… through the key (Main opens the dialog; its BPM range is optional). ---------------------------
    const std::size_t undoBefore = s.controller->session().undoCount();
    runCommand(s.shell, "sequence.detectBeats");
    QObject* loader = s.shell.object("detectBeatsDialogLoader");
    REQUIRE(loader != nullptr);
    REQUIRE(waitFor([&] { return loader->property("item").value<QObject*>() != nullptr; }));
    QObject* dialog = loader->property("item").value<QObject*>();
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
    REQUIRE(dialog->property("clipKey").toString() == clipKey(music));
    QObject* minField = dialog->findChild<QObject*>(QStringLiteral("detectBeatsMinBpm"));
    QObject* maxField = dialog->findChild<QObject*>(QStringLiteral("detectBeatsMaxBpm"));
    REQUIRE(minField != nullptr);
    REQUIRE(maxField != nullptr);
    minField->setProperty("text", QStringLiteral("140"));
    maxField->setProperty("text", QStringLiteral("160"));
    REQUIRE(QMetaObject::invokeMethod(dialog, "startDetect"));
    REQUIRE(waitFor([&] { return !s.beats->busy(); }, 20000));
    REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }));
    INFO(s.beats->message().toStdString());

    // One undo step "Detect Beats": the numbered downbeat markers and the sequence's grid.
    INFO(s.controller->undoText().toStdString());
    CHECK(s.controller->session().undoCount() == undoBefore + 1);
    CHECK(s.controller->undoText() == QStringLiteral("Undo Detect Beats"));
    CHECK(s.beats->hasGrid());
    CHECK(s.beats->gridInfo().value(QStringLiteral("bpm")).toDouble() == 150.0);
    CHECK(s.beats->gridInfo().value(QStringLiteral("beats")).toInt() == 5); // beats 0, 10, 20, 30 and 40
    CHECK(s.beats->gridInfo().value(QStringLiteral("beatsPerBar")).toInt() == 4);
    CHECK(s.controller->markers().size() == 2); // the downbeats at 0 and 40
    CHECK(s.beats->message() == QStringLiteral("150 BPM, 7 beats · 2 downbeat markers"));

    // The toast says the tempo and the beats.
    QObject* toast = s.shell.object("toast");
    REQUIRE(toast != nullptr);
    CHECK(toast->property("text").toString() == QStringLiteral("150 BPM, 7 beats · 2 downbeat markers"));

    // ---- The beat grid on the ruler, and Timeline Display > Beat grid. -----------------------------------------------
    auto* view = qobject_cast<TimelineView*>(s.shell.object("timelineView"));
    REQUIRE(view != nullptr);
    view->setPixelsPerSecond(100.0);
    view->setScrollX(0.0);
    spin(30);
    CHECK(view->showBeatGrid());
    CHECK(view->beatTicks().size() >= 2); // the beats at 0 and 10 frames
    QObject* display = s.shell.object("timelineDisplayPopover");
    REQUIRE(display != nullptr);
    QObject* toggle = display->findChild<QObject*>(QStringLiteral("beatGridToggle"));
    REQUIRE(toggle != nullptr);
    CHECK(toggle->property("checked").toBool());
    s.shell.preferences->setProperty("timelineShowBeatGrid", false);
    spin(30);
    CHECK_FALSE(view->showBeatGrid());
    CHECK(view->beatTicks().isEmpty());
    CHECK_FALSE(toggle->property("checked").toBool());
    s.shell.preferences->setProperty("timelineShowBeatGrid", true);
    spin(30);
    CHECK(view->showBeatGrid());

    // ---- Snap Selected Clips to Beats: the shot's head (9) goes to the beat at 10. ----------------------------------
    CHECK(s.enabled("edit.snapToBeats"));
    s.controller->setSelection({shot});
    s.controller->setPlayhead(f(0));
    const std::size_t beforeSnap = s.controller->session().undoCount();
    runCommand(s.shell, "edit.snapToBeats");
    spin(30);
    CHECK(s.controller->session().undoCount() == beforeSnap + 1);
    CHECK(startOf(s.controller, shot) == f(10));
    const SequencePtr afterSnap = s.controller->currentSequence();
    REQUIRE(afterSnap);
    CHECK(afterSnap->findClip(shot)->clip().end() == f(20)); // the head and the tail both reach a beat
    CHECK(s.beats->message() == QStringLiteral("Snapped 2 clip edges to the beats"));
    INFO(s.controller->undoText().toStdString());
    CHECK(s.controller->undoText() == QStringLiteral("Undo Snap to Beats"));

    // ---- Cut Selected Clips to Beats…: the dialog is the undo step's confirmation. ---------------------------------
    CHECK(s.enabled("sequence.cutToBeats"));
    const std::size_t beforeCut = s.controller->session().undoCount();
    runCommand(s.shell, "sequence.cutToBeats");
    QObject* cutLoader = s.shell.object("cutToBeatsDialogLoader");
    REQUIRE(cutLoader != nullptr);
    REQUIRE(waitFor([&] { return cutLoader->property("item").value<QObject*>() != nullptr; }));
    QObject* cutDialog = cutLoader->property("item").value<QObject*>();
    REQUIRE(waitFor([&] { return cutDialog->property("opened").toBool(); }));
    CHECK(cutDialog->property("info").toMap().value(QStringLiteral("clips")).toInt() == 1);
    cutDialog->setProperty("beatsPerCut", 2);
    REQUIRE(QMetaObject::invokeMethod(cutDialog, "cut"));
    REQUIRE(waitFor([&] { return !cutDialog->property("visible").toBool(); }));
    INFO(s.beats->message().toStdString());
    INFO(s.controller->undoText().toStdString());
    CHECK(s.controller->session().undoCount() == beforeCut + 1);
    CHECK(s.beats->message() == QStringLiteral("Cut 1 clip to the beat, 2 beats each"));
    CHECK(s.controller->undoText() == QStringLiteral("Undo Cut to Beats"));

    // ---- Clear Beat Grid, then Undo brings the grid back. -----------------------------------------------------------
    CHECK(s.enabled("sequence.clearBeatGrid"));
    runCommand(s.shell, "sequence.clearBeatGrid");
    spin(20);
    CHECK_FALSE(s.beats->hasGrid());
    CHECK(s.beats->message() == QStringLiteral("Beat grid cleared"));
    CHECK(view->beatTicks().isEmpty());
    CHECK(s.reason("edit.snapToBeats") == QStringLiteral("Detect beats first"));
    REQUIRE(s.controller->undo());
    spin(20);
    CHECK(s.beats->hasGrid());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Beats: the Detect Beats dialog in the window, dark and light", "[app][beats][qml][screenshots]") {
    for (const bool dark : {true, false}) {
        DYNAMIC_SECTION((dark ? "dark" : "light")) {
            resetSettings();
            clearMessages();
            BeatsShell s;
            s.shell.setAppearance(dark, "ultraviolet");
            const ClipId music = s.tl.clip("A1@0");
            s.controller->selectClip(clipKey(music));
            spin(20);
            runCommand(s.shell, "sequence.detectBeats");
            QObject* loader = s.shell.object("detectBeatsDialogLoader");
            REQUIRE(loader != nullptr);
            REQUIRE(waitFor([&] { return loader->property("item").value<QObject*>() != nullptr; }));
            QObject* dialog = loader->property("item").value<QObject*>();
            REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
            spin(300);
            const QImage image = s.shell.grab();
            REQUIRE_FALSE(image.isNull());
            REQUIRE(image.save(QDir(outputDir()).filePath(QStringLiteral("p1-detect-beats-%1.png")
                                                              .arg(dark ? QStringLiteral("dark")
                                                                        : QStringLiteral("light")))));
            REQUIRE(QMetaObject::invokeMethod(dialog, "close"));
        }
    }
}
