// 0.4 (qol round), stream B: keys and transport tests (nudge clip up / down, clip volume keys, play around, paging,
// numpad timecode entry). Registered by stream E in Phase 1 (tests/app/CMakeLists.txt); stream B owns this file.
//   * Nudge Clip Selection Up / Down (Alt+Up / Alt+Down, ProjectController::nudgeSelectionTrack): one track up or down,
//     overwriting, one undo step "Nudge" per press; linked clips move in unison; edges and locked tracks say why.
//   * The window (QML): Markers > Ripple Sequence Markers (checked, one undo step), Alt+Up / Alt+Down and the clip
//     volume keys through the keymap with their toasts, PageUp / PageDown paging, Shift+K in trim mode, Shift+L slow
//     shuttle in the Source Monitor (its ladder follows the source, not the Program), and numpad
//     timecode entry in the Timeline and both monitors (main-row digits and Esc leave it alone). Screenshots:
//     markers-menu-ripple.png, clip-volume-toast.png, timecode-numpad-entry.png.

#include "BridgeIds.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"
#include "ShellFixture.h"
#include "SourceMonitorBridge.h"
#include "TimelineFixture.h"
#include "TimelineView.h"
#include "TrimBridge.h"

#include <wabcut/editor/PlaybackSession.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QKeyEvent>
#include <QQuickItem>
#include <QQuickWindow>

#include <string>
#include <string_view>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using Catch::Approx;

namespace {

/// A controller on a timeline picture, with helpers to select and read the picture back.
struct Rig {
    Timeline tl;
    ProjectController controller;

    explicit Rig(std::string_view picture, bool linked = false) : tl(picture) {
        if (linked)
            tl.linkMatching();
        controller.resetProject(tl.state, tl.seq);
    }
    void select(std::string_view ref) { controller.setSelection({clipAt(*controller.snapshot(), tl.seq, ref)}); }
    std::string picture(std::string_view widthFrom) const { return wabcut::test::render(*controller.snapshot(), tl.seq, widthFrom); }
    std::size_t undoCount() { return controller.session().undoCount(); }
    void setFlag(std::string_view track, const char* flag, bool on) {
        controller.setTrackFlag(trackKey(tl.track(track)), QString::fromLatin1(flag), on);
    }
};

} // namespace

TEST_CASE("Nudge Clip Selection Up and Down move the selection one track", "[app][keys][nudge]") {
    constexpr std::string_view kStart = R"(
        V2     |..........|
        V1 tgt |..Aaaa....|
        A1 tgt |..........|
        A2     |..Bbbb....|
    )";
    Rig rig(kStart);
    rig.select("V1@2");
    const std::size_t undo = rig.undoCount();
    CHECK(rig.controller.nudgeSelectionTrack(1).isEmpty());
    CHECK(rig.picture(kStart) == normalize(R"(
        V2 |..Aaaa....|
        V1 |..........|
        A1 |..........|
        A2 |..Bbbb....|
    )"));
    CHECK(rig.undoCount() == undo + 1);
    CHECK(rig.controller.undoText().contains(QStringLiteral("Nudge")));
    CHECK(rig.controller.selectedClips().size() == 1); // the clip stays selected on its new track
    CHECK(rig.controller.nudgeSelectionTrack(-1).isEmpty());
    CHECK(rig.picture(kStart) == normalize(kStart));
    CHECK(rig.undoCount() == undo + 2);
    // Up for audio is towards the divider: A2 -> A1.
    rig.select("A2@2");
    CHECK(rig.controller.nudgeSelectionTrack(1).isEmpty());
    CHECK(rig.picture(kStart) == normalize(R"(
        V2 |..........|
        V1 |..Aaaa....|
        A1 |..Bbbb....|
        A2 |..........|
    )"));
    // One undo per press.
    REQUIRE(rig.controller.undo());
    CHECK(rig.picture(kStart) == normalize(kStart));
    // Overwrite: what is at the destination is cut, as a plain drag does.
    constexpr std::string_view kOver = R"(
        V2     |Cccccccc..|
        V1 tgt |..Aaaa....|
    )";
    Rig over(kOver);
    over.select("V1@2");
    CHECK(over.controller.nudgeSelectionTrack(1).isEmpty());
    CHECK(over.picture(kOver) == normalize(R"(
        V2 |CcAaaaC6..|
        V1 |..........|
    )"));
}

TEST_CASE("Nudge Up moves linked video up and keeps audio on A1", "[app][keys][nudge]") {
    constexpr std::string_view kStart = R"(
        V2     |..........|
        V1 tgt |Aaaa......|
        A1 tgt |Aaaa......|
        A2     |..........|
    )";
    Rig rig(kStart, true);
    rig.select("V1@0");
    const std::size_t undo = rig.undoCount();
    // No track above A1 (towards the divider): as in Premiere only the video moves, the pair stays linked and in
    // sync, and the toast says the audio stayed (it is not an error).
    CHECK(rig.controller.nudgeSelectionTrack(1) == QStringLiteral("A1 is the top audio track: the audio stays where it is"));
    CHECK(rig.controller.lastError().isEmpty());
    CHECK(rig.picture(kStart) == normalize(R"(
        V2 |Aaaa......|
        V1 |..........|
        A1 |Aaaa......|
        A2 |..........|
    )"));
    CHECK(links(*rig.controller.snapshot(), rig.tl.seq) == "V2@0+A1@0");
    CHECK(rig.undoCount() == undo + 1);
    // Down: the video back to V1 and the audio to A2, in unison.
    CHECK(rig.controller.nudgeSelectionTrack(-1).isEmpty());
    CHECK(rig.picture(kStart) == normalize(R"(
        V2 |..........|
        V1 |Aaaa......|
        A1 |..........|
        A2 |Aaaa......|
    )"));
    CHECK(links(*rig.controller.snapshot(), rig.tl.seq) == "V1@0+A2@0");
    CHECK(rig.undoCount() == undo + 2);
}

TEST_CASE("Nudge Down moves linked audio down and keeps video on V1", "[app][keys][nudge]") {
    constexpr std::string_view kStart = R"(
        V2     |..........|
        V1 tgt |Aaaa......|
        A1 tgt |Aaaa......|
        A2     |..........|
    )";
    Rig rig(kStart, true);
    rig.select("A1@0");
    const std::size_t undo = rig.undoCount();
    // No track below V1 (towards the divider): only the audio moves, still linked; the toast says the video stayed.
    CHECK(rig.controller.nudgeSelectionTrack(-1) == QStringLiteral("V1 is the bottom video track: the video stays where it is"));
    CHECK(rig.controller.lastError().isEmpty());
    CHECK(rig.picture(kStart) == normalize(R"(
        V2 |..........|
        V1 |Aaaa......|
        A1 |..........|
        A2 |Aaaa......|
    )"));
    CHECK(links(*rig.controller.snapshot(), rig.tl.seq) == "V1@0+A2@0");
    CHECK(rig.undoCount() == undo + 1);
    REQUIRE(rig.controller.undo());
    CHECK(rig.picture(kStart) == normalize(kStart));
}

TEST_CASE("Nudge refuses video and audio together at the top video track or the bottom audio track",
          "[app][keys][nudge]") {
    // Premiere adds a track there and moves both halves; Wabcut adds no tracks, so nothing moves and the toast says
    // why (moving the free half alone would leave the linked clip split across a track it cannot follow).
    constexpr std::string_view kStart = R"(
        V2     |Aaaa......|
        V1 tgt |..........|
        A1 tgt |..........|
        A2     |Aaaa......|
    )";
    Rig rig(kStart, true);
    rig.select("V2@0");
    const std::size_t undo = rig.undoCount();
    const QString top = QStringLiteral("V2 is the top video track: the video and audio cannot move up together");
    CHECK(rig.controller.nudgeSelectionTrack(1) == top);
    CHECK(rig.controller.lastError() == top);
    CHECK(rig.picture(kStart) == normalize(kStart));
    CHECK(links(*rig.controller.snapshot(), rig.tl.seq) == "V2@0+A2@0");
    CHECK(rig.undoCount() == undo);
    // The A2 edge with Down, chosen from the audio half.
    rig.select("A2@0");
    const QString bottom = QStringLiteral("A2 is the bottom audio track: the video and audio cannot move down together");
    CHECK(rig.controller.nudgeSelectionTrack(-1) == bottom);
    CHECK(rig.controller.lastError() == bottom);
    CHECK(rig.picture(kStart) == normalize(kStart));
    CHECK(rig.undoCount() == undo);
    // A selection of both kinds without a link is one block too.
    Rig loose(kStart);
    loose.controller.setSelection(
        {clipAt(*loose.controller.snapshot(), loose.tl.seq, "V2@0"), clipAt(*loose.controller.snapshot(), loose.tl.seq, "A2@0")});
    const std::size_t looseUndo = loose.undoCount();
    CHECK(loose.controller.nudgeSelectionTrack(1) == top);
    CHECK(loose.controller.nudgeSelectionTrack(-1) == bottom);
    CHECK(loose.picture(kStart) == normalize(kStart));
    CHECK(loose.undoCount() == looseUndo);
}

TEST_CASE("Nudge a one-kind selection still moves when the other kind is at its edge", "[app][keys][nudge]") {
    constexpr std::string_view kStart = R"(
        V2     |..........|
        V1 tgt |Aaaa......|
        A1 tgt |..Bbbb....|
        A2     |..........|
    )";
    Rig rig(kStart);
    // Video alone goes up from V1 while the audio clip sits on A1 (the top audio track): no message.
    rig.select("V1@0");
    CHECK(rig.controller.nudgeSelectionTrack(1).isEmpty());
    CHECK(rig.picture(kStart) == normalize(R"(
        V2 |Aaaa......|
        V1 |..........|
        A1 |..Bbbb....|
        A2 |..........|
    )"));
    // Audio alone goes down from A1 although nothing could go below V1: no message either.
    rig.select("A1@2");
    CHECK(rig.controller.nudgeSelectionTrack(-1).isEmpty());
    CHECK(rig.picture(kStart) == normalize(R"(
        V2 |Aaaa......|
        V1 |..........|
        A1 |..........|
        A2 |..Bbbb....|
    )"));
    // A linked partner on a locked track does not move, so the selection is one kind: it moves without a message.
    constexpr std::string_view kLocked = R"(
        V2     |..........|
        V1 tgt |Aaaa......|
        A1 tgt |Aaaa......|
    )";
    Rig locked(kLocked, true);
    locked.setFlag("A1", "locked", true);
    locked.select("V1@0");
    CHECK(locked.controller.nudgeSelectionTrack(1).isEmpty());
    CHECK(locked.picture(kLocked) == normalize(R"(
        V2 |Aaaa......|
        V1 |..........|
        A1 |Aaaa......|
    )"));
}

TEST_CASE("Nudge at the top track or onto a locked track changes nothing and says why", "[app][keys][nudge]") {
    constexpr std::string_view kStart = R"(
        V1 tgt |..Aaaa....|
        A1 tgt |..........|
    )";
    Rig rig(kStart);
    // Nothing selected: nothing happens, nothing to say.
    CHECK(rig.controller.nudgeSelectionTrack(1).isEmpty());
    const std::size_t undo = rig.undoCount();
    rig.select("V1@2");
    CHECK(rig.controller.nudgeSelectionTrack(1) == QStringLiteral("There is no track above the selection"));
    CHECK(rig.controller.lastError() == QStringLiteral("There is no track above the selection"));
    CHECK(rig.controller.nudgeSelectionTrack(-1) == QStringLiteral("There is no track below the selection"));
    CHECK(rig.picture(kStart) == normalize(kStart));
    CHECK(rig.undoCount() == undo); // no track is ever created, no empty undo step

    constexpr std::string_view kLocked = R"(
        V2     |..........|
        V1 tgt |..Aaaa....|
    )";
    Rig locked(kLocked);
    locked.setFlag("V2", "locked", true);
    const std::size_t lockedUndo = locked.undoCount();
    locked.select("V1@2");
    CHECK(locked.controller.nudgeSelectionTrack(1) == QStringLiteral("V2 is locked"));
    CHECK(locked.picture(kLocked) == normalize(kLocked));
    CHECK(locked.undoCount() == lockedUndo);
    // The commands route here too (the palette; Main.qml calls nudgeSelectionTrack to show the reason).
    CHECK(ProjectController::handlesCommand(QStringLiteral("timeline.nudgeClipUp")));
    CHECK(ProjectController::handlesCommand(QStringLiteral("timeline.nudgeClipDown")));
    locked.setFlag("V2", "locked", false);
    REQUIRE(locked.controller.runCommand(QStringLiteral("timeline.nudgeClipUp")));
    CHECK(locked.picture(kLocked) == normalize(R"(
        V2 |..Aaaa....|
        V1 |..........|
    )"));
    // The palette: nothing selected says so.
    locked.controller.clearSelection();
    const QVariantMap state = locked.controller.commandAvailability(QStringLiteral("timeline.nudgeClipDown"));
    CHECK(state.value(QStringLiteral("handled")).toBool());
    CHECK_FALSE(state.value(QStringLiteral("enabled")).toBool());
    CHECK(state.value(QStringLiteral("reason")).toString() == QStringLiteral("Select a clip first"));
}

TEST_CASE("marker.toggleRipple turns Ripple Sequence Markers off as one undo step", "[app][keys][markers]") {
    // 0.4, Markers > Ripple Sequence Markers through the controller (the Markers menu row and the ruler menu row run
    // this command); the views hear sequenceChanged.
    constexpr std::string_view kStart = R"(
        V1 tgt |AaaaBbbb|
    )";
    Rig rig(kStart);
    int notified = 0;
    QObject::connect(&rig.controller, &ProjectController::sequenceChanged, [&] { ++notified; });
    CHECK(rig.controller.rippleSequenceMarkers()); // on by default
    const std::size_t undo = rig.undoCount();
    REQUIRE(rig.controller.runCommand(QStringLiteral("marker.toggleRipple")));
    CHECK_FALSE(rig.controller.rippleSequenceMarkers());
    CHECK(notified >= 1);
    CHECK(rig.undoCount() == undo + 1);
    CHECK(rig.controller.undoText() == QStringLiteral("Undo Ripple Sequence Markers"));
    // Setting the same value again is no step.
    rig.controller.setRippleSequenceMarkers(false);
    CHECK(rig.undoCount() == undo + 1);
    REQUIRE(rig.controller.undo());
    CHECK(rig.controller.rippleSequenceMarkers());
    REQUIRE(rig.controller.redo());
    CHECK_FALSE(rig.controller.rippleSequenceMarkers());
    CHECK(rig.controller.commandAvailability(QStringLiteral("marker.toggleRipple")).value(QStringLiteral("enabled")).toBool());
}

// ---- The window (Phase 2, QML): the Markers menu row, the Main.qml key cases, paging and numpad entry ----------------

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

/// A press and release through the window (the keymap's event filter first, then the focused item).
void key(QQuickWindow* window, int code, Qt::KeyboardModifiers mods = {}, const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, code, mods, text);
    QCoreApplication::sendEvent(window, &press);
    QKeyEvent release(QEvent::KeyRelease, code, mods, text);
    QCoreApplication::sendEvent(window, &release);
}
/// A numpad key (Qt's KeypadModifier).
void keypad(QQuickWindow* window, int code, const QString& text) { key(window, code, Qt::KeypadModifier, text); }

/// The editor at 1600x960, motion off.
struct KeysShell {
    Shell shell;
    QQuickWindow* w = nullptr;
    ProjectController* controller = nullptr;

    KeysShell() {
        REQUIRE(shell.window != nullptr);
        INFO(join(shell.errors));
        REQUIRE(shell.errors.isEmpty());
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        w = shell.window.get();
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        REQUIRE(controller != nullptr);
        spin(50);
    }
    QQuickItem* find(const char* name) const {
        QQuickItem* item = findNamed(w->contentItem(), QString::fromLatin1(name));
        INFO(name);
        REQUIRE(item != nullptr);
        return item;
    }
    QQuickItem* focusLanes() const {
        QQuickItem* lanes = shell.item("timelineLanes");
        REQUIRE(lanes != nullptr);
        lanes->forceActiveFocus(Qt::TabFocusReason);
        REQUIRE(lanes->hasActiveFocus());
        return lanes;
    }
    QString toast() const { return shell.object("toast")->property("text").toString(); }
};

constexpr std::string_view kKeysPicture = R"(
    V2     |....................|
    V1 tgt |AaaaaaaaaaBbbbbbbbbb|
    A1 tgt |AaaaaaaaaaBbbbbbbbbb|
    A2     |....................|
)";

} // namespace

TEST_CASE("Markers menu Ripple Sequence Markers is checked and toggles as one undo step",
          "[app][keys][markers][qml][screenshot]") {
    resetSettings();
    clearMessages();
    KeysShell s;
    QObject* bar = s.shell.object("appMenuBar");
    QObject* markers = s.shell.object("markersMenu");
    REQUIRE(bar != nullptr);
    REQUIRE(markers != nullptr);
    REQUIRE(s.controller->rippleSequenceMarkers());
    const auto open = [&] {
        REQUIRE(QMetaObject::invokeMethod(bar, "open", Q_ARG(int, 4), Q_ARG(bool, false)));
        REQUIRE(waitFor([&] { return markers->property("opened").toBool(); }));
    };
    open();
    QObject* row = markers->findChild<QObject*>(QStringLiteral("menuCommand_marker.toggleRipple"));
    REQUIRE(row != nullptr);
    CHECK(row->property("text").toString() == QStringLiteral("Ripple Sequence Markers"));
    CHECK(row->property("visible").toBool());
    CHECK(row->property("enabled").toBool());
    CHECK(row->property("checkable").toBool());
    CHECK(row->property("checked").toBool()); // on by default (Premiere's default)
    const QImage image = s.shell.grab();
    REQUIRE_FALSE(image.isNull());
    CHECK(image.save(QDir(outputDir()).filePath(QStringLiteral("markers-menu-ripple.png"))));

    // Choosing it turns it off for this sequence: one undo step.
    const std::size_t undo = s.controller->session().undoCount();
    REQUIRE(QMetaObject::invokeMethod(row, "clicked"));
    REQUIRE(waitFor([&] { return !markers->property("visible").toBool(); }));
    CHECK(waitFor([&] { return !s.controller->rippleSequenceMarkers(); }));
    CHECK(s.controller->session().undoCount() == undo + 1);
    CHECK(s.controller->undoText() == QStringLiteral("Undo Ripple Sequence Markers"));
    // The row follows the sequence the next time the menu opens; undo turns it back on.
    open();
    CHECK_FALSE(row->property("checked").toBool());
    REQUIRE(QMetaObject::invokeMethod(markers, "close"));
    REQUIRE(waitFor([&] { return !markers->property("visible").toBool(); }));
    REQUIRE(s.controller->undo());
    CHECK(s.controller->rippleSequenceMarkers());
    open();
    CHECK(row->property("checked").toBool());
    REQUIRE(QMetaObject::invokeMethod(markers, "close"));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Alt+Up and Alt+Down nudge through the keymap and a toast says why not", "[app][keys][nudge][qml]") {
    resetSettings();
    clearMessages();
    KeysShell s;
    Timeline tl{kKeysPicture};
    s.controller->resetProject(tl.state, tl.seq);
    s.controller->setSelection({clipAt(*s.controller->snapshot(), tl.seq, "V1@0")});
    s.focusLanes();
    const std::size_t undo = s.controller->session().undoCount();
    key(s.w, Qt::Key_Up, Qt::AltModifier);
    CHECK(wabcut::test::render(*s.controller->snapshot(), tl.seq, kKeysPicture) == normalize(R"(
        V2 |Aaaaaaaaaa..........|
        V1 |..........Bbbbbbbbbb|
        A1 |AaaaaaaaaaBbbbbbbbbb|
        A2 |....................|
    )"));
    CHECK(s.controller->session().undoCount() == undo + 1);
    CHECK(s.controller->undoText().contains(QStringLiteral("Nudge")));
    // At the top: nothing moves and the toast says why.
    key(s.w, Qt::Key_Up, Qt::AltModifier);
    CHECK(s.controller->session().undoCount() == undo + 1);
    CHECK(waitFor([&] { return s.toast() == QStringLiteral("There is no track above the selection"); }));
    key(s.w, Qt::Key_Down, Qt::AltModifier);
    CHECK(s.controller->session().undoCount() == undo + 2);
    // A linked clip on V1 / A1: Up moves the video (A1 is the top audio track) and the toast says the audio stayed.
    s.controller->setSelection({clipAt(*s.controller->snapshot(), tl.seq, "V1@10"), clipAt(*s.controller->snapshot(), tl.seq, "A1@10")});
    s.controller->linkSelection();
    REQUIRE(s.controller->selectionLinked());
    const std::size_t linkedUndo = s.controller->session().undoCount();
    key(s.w, Qt::Key_Up, Qt::AltModifier);
    CHECK(s.controller->session().undoCount() == linkedUndo + 1);
    CHECK(waitFor([&] { return s.toast() == QStringLiteral("A1 is the top audio track: the audio stays where it is"); }));
    CHECK(wabcut::test::render(*s.controller->snapshot(), tl.seq, kKeysPicture) == normalize(R"(
        V2 |..........Bbbbbbbbbb|
        V1 |Aaaaaaaaaa..........|
        A1 |AaaaaaaaaaBbbbbbbbbb|
        A2 |....................|
    )"));
    // A linked clip on V2 / A2: Up at the top video track refuses both halves (the audio could go to A1) and says why.
    constexpr std::string_view kEdge = R"(
        V2     |Aaaa|
        V1 tgt |....|
        A1 tgt |....|
        A2     |Aaaa|
    )";
    Timeline edge{kEdge};
    edge.linkMatching();
    s.controller->resetProject(edge.state, edge.seq);
    s.controller->setSelection({clipAt(*s.controller->snapshot(), edge.seq, "V2@0")});
    s.focusLanes();
    const std::size_t edgeUndo = s.controller->session().undoCount();
    key(s.w, Qt::Key_Up, Qt::AltModifier);
    CHECK(waitFor([&] {
        return s.toast() == QStringLiteral("V2 is the top video track: the video and audio cannot move up together");
    }));
    CHECK(s.controller->session().undoCount() == edgeUndo);
    CHECK(wabcut::test::render(*s.controller->snapshot(), edge.seq, kEdge) == normalize(kEdge));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Clip volume keys in the window change the selected clip and show a toast", "[app][keys][volume][qml][screenshot]") {
    resetSettings();
    clearMessages();
    KeysShell s;
    // The sample project's first V1 clip (Interview A, with its sound) selected: ] raises it 1 dB.
    const auto sequence = s.controller->currentSequence();
    REQUIRE(sequence);
    REQUIRE_FALSE(sequence->videoTracks.empty());
    REQUIRE_FALSE(sequence->videoTracks[0]->clips.empty());
    s.controller->setSelection({sequence->videoTracks[0]->clips.front().id});
    s.focusLanes();
    const std::size_t undo = s.controller->session().undoCount();
    key(s.w, Qt::Key_BracketRight, {}, QStringLiteral("]"));
    INFO(s.toast().toStdString());
    REQUIRE(waitFor([&] { return s.toast().startsWith(QStringLiteral("Volume +1 dB")); }));
    CHECK(s.controller->session().undoCount() == undo + 1);
    key(s.w, Qt::Key_BracketRight, {}, QStringLiteral("]"));
    CHECK(waitFor([&] { return s.toast().contains(QStringLiteral("2.0 dB")); }));
    CHECK(s.controller->session().undoCount() == undo + 1); // merged into one step within a second
    const QImage image = s.shell.grab();
    REQUIRE_FALSE(image.isNull());
    CHECK(image.save(QDir(outputDir()).filePath(QStringLiteral("clip-volume-toast.png"))));
    // Shift+[ lowers it 6 dB.
    key(s.w, Qt::Key_BraceLeft, Qt::ShiftModifier, QStringLiteral("{"));
    CHECK(waitFor([&] { return s.toast().startsWith(QStringLiteral("Volume -6 dB")); }));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Show Next Screen and Show Previous Screen scroll one view width and keep the playhead", "[app][keys][paging][qml]") {
    resetSettings();
    clearMessages();
    KeysShell s;
    auto* view = qobject_cast<TimelineView*>(s.shell.object("timelineView"));
    REQUIRE(view != nullptr);
    s.focusLanes();
    view->setPixelsPerSecond(400); // the sample sequence is many screens wide
    view->setScrollX(0);
    spin(20);
    const Ticks playhead = s.controller->playhead();
    const double width = view->width();
    REQUIRE(width > 100);
    key(s.w, Qt::Key_PageDown);
    CHECK(view->scrollX() == Approx(width).margin(1));
    key(s.w, Qt::Key_PageDown);
    CHECK(view->scrollX() == Approx(2 * width).margin(1));
    key(s.w, Qt::Key_PageUp);
    CHECK(view->scrollX() == Approx(width).margin(1));
    key(s.w, Qt::Key_PageUp);
    key(s.w, Qt::Key_PageUp); // clamped at the start
    CHECK(view->scrollX() == Approx(0).margin(1));
    CHECK(s.controller->playhead() == playhead);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Shift+K with an edit point selected plays around the edit", "[app][keys][trim][qml]") {
    resetSettings();
    clearMessages();
    KeysShell s;
    Timeline tl{kKeysPicture};
    s.controller->resetProject(tl.state, tl.seq);
    auto* trim = s.shell.engine.singletonInstance<TrimBridge*>("Wabcut", "Trim");
    REQUIRE(trim != nullptr);
    s.controller->setPlayhead(f(10)); // the A|B edit
    REQUIRE(trim->trimEdit());
    REQUIRE(trim->property("active").toBool());
    s.focusLanes();
    key(s.w, Qt::Key_K, Qt::ShiftModifier, QStringLiteral("K"));
    CHECK(trim->property("playingAround").toBool());
    trim->stopAround();
    trim->clear();

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Shift+L in the Source Monitor climbs from its own speed and starts at a quarter after a stop",
          "[app][keys][shuttle][qml]") {
    // The keys' ladder follows the monitor they drive: with the Source Monitor focused, its stops (Space, the end, a
    // play-once) and its speeds count, not the Program's.
    resetSettings();
    clearMessages();
    KeysShell s;
    Timeline tl{kKeysPicture, {{'S', 250}}};
    s.controller->resetProject(tl.state, tl.seq);
    auto* source = s.shell.engine.singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor");
    auto* program = s.shell.engine.singletonInstance<PlaybackBridge*>("Wabcut", "Playback");
    REQUIRE(source != nullptr);
    REQUIRE(program != nullptr);
    REQUIRE(source->load(mediaKey(tl.media('S'))));
    QQuickItem* sourcePanel = s.shell.item("sourceMonitorPanel");
    REQUIRE(sourcePanel != nullptr);
    REQUIRE(QMetaObject::invokeMethod(sourcePanel, "focusMonitor"));
    const auto speed = [&] {
        const auto v = source->playback()->speed();
        return source->playing() && v.den != 0 ? static_cast<double>(v.num) / v.den : 0.0;
    };
    const auto shiftL = [&] { key(s.w, Qt::Key_L, Qt::ShiftModifier, QStringLiteral("L")); };
    const auto space = [&] { key(s.w, Qt::Key_Space, {}, QStringLiteral(" ")); };
    shiftL();
    CHECK(speed() == Approx(0.25));
    shiftL();
    CHECK(speed() == Approx(0.5));
    space();
    CHECK_FALSE(source->playing());
    shiftL(); // stopped: a quarter again, not the next rung
    CHECK(speed() == Approx(0.25));
    shiftL();
    shiftL();
    CHECK(speed() == Approx(1.0));
    space();
    CHECK_FALSE(source->playing());
    shiftL(); // after 1x and a stop it plays again
    CHECK(speed() == Approx(0.25));
    space();

    // The Program playing pauses as the source starts: the source's ladder goes on from the source's speed.
    program->play();
    REQUIRE(program->playing());
    REQUIRE(QMetaObject::invokeMethod(sourcePanel, "focusMonitor"));
    shiftL();
    CHECK_FALSE(program->playing());
    CHECK(speed() == Approx(0.25));
    shiftL();
    CHECK(speed() == Approx(0.5));
    space();

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Numpad plus and digits open the Timeline timecode as an offset", "[app][keys][numpad][qml][screenshot]") {
    resetSettings();
    clearMessages();
    KeysShell s;
    QQuickItem* lanes = s.focusLanes();
    QQuickItem* entry = s.find("timelineTimecode");
    QQuickItem* field = s.find("timelineTimecodeField");
    REQUIRE(s.controller->seekToText(QStringLiteral("10"))); // frame 10
    const QString target = s.controller->timeEntryText(QStringLiteral("+15"));
    REQUIRE_FALSE(target.isEmpty());
    keypad(s.w, Qt::Key_Plus, QStringLiteral("+"));
    REQUIRE(entry->property("editing").toBool());
    CHECK(field->hasActiveFocus());
    keypad(s.w, Qt::Key_1, QStringLiteral("1"));
    keypad(s.w, Qt::Key_5, QStringLiteral("5"));
    CHECK(field->property("text").toString() == QStringLiteral("+15"));
    QObject* hint = s.shell.object("timelineTimecodeHint");
    REQUIRE(hint != nullptr);
    CHECK(waitFor([&] { return hint->property("visible").toBool(); }));
    const QImage image = s.shell.grab();
    REQUIRE_FALSE(image.isNull());
    CHECK(image.save(QDir(outputDir()).filePath(QStringLiteral("timecode-numpad-entry.png"))));
    // Keypad Enter: 15 frames on; the field closes and the lanes get the focus back.
    keypad(s.w, Qt::Key_Enter, QStringLiteral("\r"));
    CHECK(s.controller->playheadText() == target);
    CHECK_FALSE(entry->property("editing").toBool());
    CHECK(waitFor([&] { return lanes->hasActiveFocus(); }));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Numpad entry works in the Program and Source Monitors", "[app][keys][numpad][qml]") {
    resetSettings();
    clearMessages();
    KeysShell s;
    Timeline tl{kKeysPicture, {{'S', 50}}};
    s.controller->resetProject(tl.state, tl.seq);
    auto* source = s.shell.engine.singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor");
    REQUIRE(source != nullptr);

    // The Program Monitor: the playhead.
    QQuickItem* program = s.shell.item("monitorPanel");
    REQUIRE(program != nullptr);
    REQUIRE(QMetaObject::invokeMethod(program, "focusMonitor"));
    REQUIRE(s.controller->seekToText(QStringLiteral("5")));
    const QString target = s.controller->timeEntryText(QStringLiteral("+10"));
    keypad(s.w, Qt::Key_Plus, QStringLiteral("+"));
    QQuickItem* programEntry = s.find("programTimecode");
    REQUIRE(programEntry->property("editing").toBool());
    keypad(s.w, Qt::Key_1, QStringLiteral("1"));
    keypad(s.w, Qt::Key_0, QStringLiteral("0"));
    keypad(s.w, Qt::Key_Enter, QStringLiteral("\r"));
    CHECK(s.controller->playheadText() == target);
    CHECK_FALSE(programEntry->property("editing").toBool());

    // The Source Monitor: only with a source loaded.
    QQuickItem* sourcePanel = s.shell.item("sourceMonitorPanel");
    REQUIRE(sourcePanel != nullptr);
    QQuickItem* sourceEntry = s.find("sourceTimecode");
    REQUIRE(QMetaObject::invokeMethod(sourcePanel, "focusMonitor"));
    keypad(s.w, Qt::Key_5, QStringLiteral("5"));
    CHECK_FALSE(sourceEntry->property("editing").toBool());
    REQUIRE(source->load(mediaKey(tl.media('S'))));
    REQUIRE(QMetaObject::invokeMethod(sourcePanel, "focusMonitor"));
    keypad(s.w, Qt::Key_2, QStringLiteral("2"));
    REQUIRE(sourceEntry->property("editing").toBool());
    keypad(s.w, Qt::Key_0, QStringLiteral("0"));
    keypad(s.w, Qt::Key_Enter, QStringLiteral("\r"));
    CHECK(source->property("positionText").toString() == QStringLiteral("00:00:00:20"));
    CHECK_FALSE(sourceEntry->property("editing").toBool());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Main row digits and Esc leave the timecode alone", "[app][keys][numpad][qml]") {
    resetSettings();
    clearMessages();
    KeysShell s;
    QQuickItem* lanes = s.focusLanes();
    QQuickItem* entry = s.find("timelineTimecode");
    const QString before = s.controller->playheadText();
    // A main-row digit stays a keymap key; a numpad key with Ctrl does not open the field either.
    key(s.w, Qt::Key_5, {}, QStringLiteral("5"));
    CHECK_FALSE(entry->property("editing").toBool());
    key(s.w, Qt::Key_5, Qt::KeypadModifier | Qt::ControlModifier, QStringLiteral("5"));
    CHECK_FALSE(entry->property("editing").toBool());
    // Esc cancels a numpad entry: the playhead stays and the lanes get the focus back.
    s.focusLanes();
    keypad(s.w, Qt::Key_7, QStringLiteral("7"));
    REQUIRE(entry->property("editing").toBool());
    key(s.w, Qt::Key_Escape);
    CHECK_FALSE(entry->property("editing").toBool());
    CHECK(s.controller->playheadText() == before);
    CHECK(waitFor([&] { return lanes->hasActiveFocus(); }));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
