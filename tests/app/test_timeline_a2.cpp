// 0.7, stream A, batch A2 (timeline QoL): navigation, shortcuts, snapping, track display.
//   * 063 Pan with the middle button; hold H / Z for the Hand / Zoom tool (the shell).
//   * 105 Markers > Add Marker from In/Out; 106 / 117 video solo and the track output / mute / solo keys.
//   * 109 Overlay, Insert Before / After and Append at End on keys (the shell, with a source loaded).
//   * 113 Snap to: clip edges / playhead / In-Out / markers / beats; 118 label palette; 126 clip duration on the clip;
//     142 a recording's audio track names on its clips; 182 "Add tracks when editing clips in".
//   * Track Select Backward carries the markers before the selection (the follow-up of card 095).
// Screenshots under test-output/ui (timeline-a2-*.png).

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "BridgeIds.h"
#include "EditPreferences.h"
#include "KeymapDispatcher.h"
#include "ProjectController.h"
#include "SourceMonitorBridge.h"
#include "TimelineFixture.h"
#include "TimelineView.h"

#include <wabcut/editor/SourceEdit.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/SequenceSettings.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QJSValue>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickItem>
#include <QSettings>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <string_view>
#include <vector>

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

QRectF sceneRect(const QQuickItem* item) {
    return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
}

void sendMouse(QQuickWindow* window, QEvent::Type type, QPointF scene, Qt::MouseButton button, Qt::MouseButtons buttons,
               Qt::KeyboardModifiers mods = {}) {
    QMouseEvent event(type, scene, scene, button, buttons, mods);
    QCoreApplication::sendEvent(window, &event);
}

void keyDown(QQuickWindow* window, int key, Qt::KeyboardModifiers mods = {}) {
    QKeyEvent press(QEvent::KeyPress, key, mods);
    QCoreApplication::sendEvent(window, &press);
}
void keyUp(QQuickWindow* window, int key, Qt::KeyboardModifiers mods = {}) {
    QKeyEvent release(QEvent::KeyRelease, key, mods);
    QCoreApplication::sendEvent(window, &release);
}
void tap(QQuickWindow* window, int key, Qt::KeyboardModifiers mods = {}) {
    keyDown(window, key, mods);
    keyUp(window, key, mods);
}

/// A project from a timeline picture under a ProjectController (no window).
struct Project {
    Timeline tl;
    ProjectController controller;

    explicit Project(std::string_view picture, const std::map<char, std::int64_t>& media = {}) : tl(picture, media) {
        tl.linkMatching();
        controller.resetProject(tl.state, tl.seq);
    }
    const Sequence& sequence() const { return *controller.currentSequence(); }
    const Track& track(std::string_view label) const { return *sequence().findTrack(tl.track(label)); }
    std::size_t undoSteps() { return controller.session().undoCount(); }
    void select(std::initializer_list<std::string_view> refs) {
        std::vector<ClipId> ids;
        for (const std::string_view ref : refs)
            ids.push_back(tl.clip(ref));
        controller.setSelection(ids, ids.front());
    }
};

/// The real shell (Main.qml, hidden, 1600 x 960, dark, reduced motion) over a project from a timeline picture.
struct ShellProject {
    Shell shell;
    ProjectController* controller = nullptr;
    Timeline tl;

    explicit ShellProject(std::string_view picture, const std::map<char, std::int64_t>& media = {}, bool dark = true)
        : tl(picture, media) {
        REQUIRE(shell.window);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        shell.setAppearance(dark, "ultraviolet");
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        REQUIRE(controller);
        tl.linkMatching();
        controller->resetProject(tl.state, tl.seq);
        spin(200);
    }
    template <class T>
    T* singleton(const char* name) {
        T* found = shell.engine.singletonInstance<T*>("Wabcut", name);
        REQUIRE(found);
        return found;
    }
    QQuickItem* item(const QString& name) const { return findNamed(shell.window->contentItem(), name); }
    const Sequence& sequence() const { return *controller->currentSequence(); }
    bool run(const char* command) {
        return QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QString::fromLatin1(command)));
    }
    void settle() const {
        REQUIRE_FALSE(shell.grab().isNull());
        spin(30);
    }
    void shot(const QRectF& rect, const QString& file) const {
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        const double dpr = image.devicePixelRatio();
        const QRectF r = rect.intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
        REQUIRE(image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect()).save(QDir(outputDir()).filePath(file)));
    }
};

void checkNoMessages() {
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

constexpr Rate kRate25f = kRate25;

/// `frames` frames of media `letter` as a timeline picture clip ("Aaaa").
std::string run(char letter, int frames) {
    std::string out(1, letter);
    out.append(static_cast<std::size_t>(frames - 1), static_cast<char>(letter - 'A' + 'a'));
    return out;
}

} // namespace

// ---- 106, 117: video solo and the track keys -------------------------------------------------------------------------------

TEST_CASE("Track keys toggle the output, mute and solo of the selected clips' tracks in one undo step (cards 106 117)",
          "[app][timeline][tracks][keys]") {
    resetSettings();
    Project p(R"(
        V2     |Bbbb....|
        V1 tgt |Aaaa....|
        A1 tgt |Aaaa....|
        A2     |Bbbb....|
    )");
    ProjectController& c = p.controller;

    SECTION("Toggle Track Output hides the video track, again shows it") {
        p.select({"V2@0"});
        const std::size_t steps = p.undoSteps();
        REQUIRE(c.toggleTrackSwitch(QStringLiteral("output")));
        CHECK_FALSE(p.track("V2").enabled);
        CHECK(p.track("V1").enabled);
        CHECK(p.undoSteps() == steps + 1);
        CHECK(c.undoText() == QStringLiteral("Undo Hide Track"));
        REQUIRE(c.toggleTrackSwitch(QStringLiteral("output")));
        CHECK(p.track("V2").enabled);
        CHECK(c.undoText() == QStringLiteral("Undo Show Track"));
        REQUIRE(c.undo());
        REQUIRE(c.undo());
        CHECK(p.track("V2").enabled);
        CHECK(p.undoSteps() == steps);
    }
    SECTION("a linked picture and sound: output acts on the picture, mute on the sound, solo on both") {
        p.select({"V1@0", "A1@0"});
        REQUIRE(c.toggleTrackSwitch(QStringLiteral("output")));
        CHECK_FALSE(p.track("V1").enabled);
        CHECK(p.track("A1").enabled); // an audio track has no eye
        REQUIRE(c.toggleTrackSwitch(QStringLiteral("mute")));
        CHECK(p.track("A1").muted);
        CHECK_FALSE(p.track("V1").muted);
        CHECK(c.undoText() == QStringLiteral("Undo Mute Track"));
        REQUIRE(c.toggleTrackSwitch(QStringLiteral("solo")));
        CHECK(p.track("V1").solo);
        CHECK(p.track("A1").solo);
        CHECK_FALSE(p.track("V2").solo);
        CHECK(c.undoText() == QStringLiteral("Undo Solo Track"));
        REQUIRE(c.toggleTrackSwitch(QStringLiteral("solo")));
        CHECK_FALSE(p.track("V1").solo);
        CHECK_FALSE(p.track("A1").solo);
        CHECK(c.undoText() == QStringLiteral("Undo Unsolo Track"));
    }
    SECTION("without a selection the targeted tracks are used") {
        c.clearSelection();
        REQUIRE(c.toggleTrackSwitch(QStringLiteral("mute")));
        CHECK(p.track("A1").muted); // A1 is targeted; A2 is not
        CHECK_FALSE(p.track("A2").muted);
        REQUIRE(c.toggleTrackSwitch(QStringLiteral("solo")));
        CHECK(p.track("V1").solo);
        CHECK(p.track("A1").solo);
    }
    SECTION("a switch that does not apply is refused with the reason and no undo step") {
        p.select({"A2@0"});
        const std::size_t steps = p.undoSteps();
        CHECK_FALSE(c.toggleTrackSwitch(QStringLiteral("output")));
        CHECK(c.lastError().contains(QStringLiteral("video track")));
        CHECK(p.undoSteps() == steps);
        CHECK_FALSE(c.toggleTrackSwitch(QStringLiteral("nonsense")));
        p.select({"V2@0"});
        CHECK_FALSE(c.toggleTrackSwitch(QStringLiteral("mute")));
        CHECK(c.lastError().contains(QStringLiteral("audio track")));
        CHECK(p.undoSteps() == steps);
    }
    SECTION("nothing selected and nothing targeted: refused, greyed out with the reason") {
        Project bare(R"(
            V1 |Aaaa....|
            A1 |Aaaa....|
        )");
        bare.controller.clearSelection();
        for (const char* id : {"timeline.toggleTrackOutput", "timeline.toggleTrackMute", "timeline.toggleTrackSolo",
                               "timeline.soloSelectedClipTrack"}) {
            INFO(id);
            const QVariantMap state = bare.controller.commandAvailability(QLatin1String(id));
            CHECK(state.value(QStringLiteral("handled")).toBool());
            CHECK_FALSE(state.value(QStringLiteral("enabled")).toBool());
            CHECK(state.value(QStringLiteral("reason")).toString() == QStringLiteral("Select a clip or target a track first"));
        }
        const std::size_t steps = bare.undoSteps();
        CHECK_FALSE(bare.controller.soloSelectedClipTracks());
        CHECK(bare.undoSteps() == steps);
        // With a clip selected they can run (Toggle Track Output wants a picture).
        bare.select({"A1@0"});
        CHECK(bare.controller.commandAvailability(QStringLiteral("timeline.toggleTrackMute")).value(QStringLiteral("enabled")).toBool());
        CHECK_FALSE(bare.controller.commandAvailability(QStringLiteral("timeline.toggleTrackOutput")).value(QStringLiteral("enabled")).toBool());
    }
}

TEST_CASE("Solo Selected Clip's Track solos it alone and a second press ends the solo (card 117)",
          "[app][timeline][tracks][keys]") {
    resetSettings();
    Project p(R"(
        V3     |Cccc....|
        V2     |Bbbb....|
        V1 tgt |Aaaa....|
        A1 tgt |Aaaa....|
        A2     |Bbbb....|
    )");
    ProjectController& c = p.controller;
    const std::size_t steps = p.undoSteps();

    p.select({"V2@0"});
    REQUIRE(c.soloSelectedClipTracks());
    CHECK(p.track("V2").solo);
    CHECK_FALSE(p.track("V1").solo);
    CHECK_FALSE(p.track("V3").solo);
    CHECK(c.undoText() == QStringLiteral("Undo Solo Track"));
    CHECK(p.undoSteps() == steps + 1);

    // Another clip: the solo moves (the previous one is unsoloed) in one step.
    p.select({"V3@0"});
    REQUIRE(c.soloSelectedClipTracks());
    CHECK(p.track("V3").solo);
    CHECK_FALSE(p.track("V2").solo);
    CHECK(p.undoSteps() == steps + 2);

    // The same track again: the solo ends.
    REQUIRE(c.soloSelectedClipTracks());
    CHECK_FALSE(p.track("V3").solo);
    CHECK(c.undoText() == QStringLiteral("Undo Unsolo Tracks"));
    CHECK(p.undoSteps() == steps + 3);

    // A picture with its sound: both kinds are soloed, each alone among its own kind; the other kind is not touched.
    c.setTrackFlag(trackKey(p.tl.track("A2")), QStringLiteral("solo"), true);
    p.select({"V1@0", "A1@0"});
    REQUIRE(c.soloSelectedClipTracks());
    CHECK(p.track("V1").solo);
    CHECK(p.track("A1").solo);
    CHECK_FALSE(p.track("A2").solo); // moved to A1
    // One undo brings the whole previous state back.
    REQUIRE(c.undo());
    CHECK_FALSE(p.track("V1").solo);
    CHECK_FALSE(p.track("A1").solo);
    CHECK(p.track("A2").solo);
    // Only an audio clip selected: the video solo is left alone.
    c.setTrackFlag(trackKey(p.tl.track("V2")), QStringLiteral("solo"), true);
    p.select({"A1@0"});
    REQUIRE(c.soloSelectedClipTracks());
    CHECK(p.track("A1").solo);
    CHECK_FALSE(p.track("A2").solo);
    CHECK(p.track("V2").solo);
}

// ---- 105: Add Marker from In/Out ---------------------------------------------------------------------------------------

TEST_CASE("Add Marker from In/Out makes a range marker as long as the marked range (card 105)", "[app][markers]") {
    resetSettings();
    Project p(R"(
        V1 tgt |AaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaA|
        A1 tgt |AaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaA|
    )");
    ProjectController& c = p.controller;

    // No marks: refused, greyed with the reason, no undo step.
    CHECK_FALSE(c.commandAvailability(QStringLiteral("marker.addFromInOut")).value(QStringLiteral("enabled")).toBool());
    CHECK(c.commandAvailability(QStringLiteral("marker.addFromInOut")).value(QStringLiteral("reason")).toString()
          == QStringLiteral("Set In and Out first"));
    const std::size_t steps = p.undoSteps();
    CHECK_FALSE(c.addMarkerFromInOut());
    CHECK(c.lastError() == QStringLiteral("Set In and Out first"));
    CHECK(p.undoSteps() == steps);
    // Only an In mark is not enough either.
    c.setPlayhead(f(4));
    c.markInAtPlayhead();
    CHECK_FALSE(c.addMarkerFromInOut());

    c.setPlayhead(f(10));
    c.markOutAtPlayhead(); // the Out mark includes frame 10: the range is frames 4 to 10, seven frames
    const std::size_t withMarks = p.undoSteps();
    CHECK(c.commandAvailability(QStringLiteral("marker.addFromInOut")).value(QStringLiteral("enabled")).toBool());
    REQUIRE(c.addMarkerFromInOut());
    REQUIRE(p.sequence().markers.size() == 1);
    const Marker& m = p.sequence().markers.front();
    CHECK(m.start == f(4));
    CHECK(m.duration == f(7));
    CHECK(m.end() == f(11));
    CHECK(p.undoSteps() == withMarks + 1);
    CHECK(c.undoText() == QStringLiteral("Undo Add Marker from In/Out"));
    // The marks stay (the marker is a copy of the range), and the key works through the keymap's command.
    CHECK(c.markIn().has_value());
    CHECK(c.markOut().has_value());
    REQUIRE(c.runCommand(QStringLiteral("marker.addFromInOut")));
    CHECK(p.sequence().markers.size() == 2);

    REQUIRE(c.undo());
    REQUIRE(c.undo());
    CHECK(p.sequence().markers.empty());
    REQUIRE(c.redo());
    CHECK(p.sequence().markers.size() == 1);
}

// ---- Track Select Backward carries the markers (follow-up of card 095) -------------------------------------------------------

TEST_CASE("Track Select Backward drags the markers before the selection along, in the same undo step (card 095 follow-up)",
          "[app][timeline][edit][markers]") {
    const char* picture = R"(
        V2 |Cc....Dd....|
        V1 |AaaaBbbb....|
        A1 |AaaaBbbb....|
    )";
    const auto marked = [&](Rig& rig, bool ripple, std::initializer_list<int> frames) {
        for (const int frame : frames) {
            Marker m;
            m.start = f(frame);
            rig.tl.state = ok(edit::addMarker(rig.tl.state, rig.tl.seq, m)).state;
        }
        if (!ripple) {
            edit::SequenceSettingsChange change;
            change.rippleMarkers = false;
            rig.tl.state = ok(edit::setSequenceSettings(rig.tl.state, rig.tl.seq, change)).state;
        }
        rig.controller.resetProject(rig.tl.state, rig.tl.seq);
        rig.harness.frame();
    };
    const auto markerFrames = [](Rig& rig) {
        std::vector<std::int64_t> frames;
        for (const Marker& m : rig.controller.snapshot()->findSequence(rig.tl.seq)->markers)
            frames.push_back(ticksToFrames(m.start, kRate25f));
        return frames;
    };

    SECTION("the markers at or before the end of the last clip moved follow it; later ones stay") {
        Rig rig(picture);
        marked(rig, true, {2, 6, 10});
        rig.view->setSnapping(false);
        rig.view->setTool(QStringLiteral("trackBackward"));
        const std::size_t steps = rig.undoSteps();
        // Click B (frames 4-7) and drag it 3 frames later: A and B (and C, before it on V2) move; D, after the click, stays.
        rig.drag({rig.x(5.5), rig.y("V1")}, {rig.x(8.5), rig.y("V1")});
        CHECK(rig.undoSteps() == steps + 1);
        CHECK(rig.controller.undoText() == QStringLiteral("Undo Move"));
        const ClipId b = rig.clip("V1@7");
        REQUIRE(b.isValid()); // B now starts at 7
        // B ended at 8: the markers at 2 and 6 move by 3, the one at 10 is after it.
        CHECK(markerFrames(rig) == std::vector<std::int64_t>{5, 9, 10});
        // One undo puts the clips and the markers back.
        REQUIRE(rig.controller.undo());
        CHECK(rig.picture() == normalize(picture));
        CHECK(markerFrames(rig) == std::vector<std::int64_t>{2, 6, 10});
        CHECK(rig.undoSteps() == steps);
        REQUIRE(rig.controller.redo());
        CHECK(markerFrames(rig) == std::vector<std::int64_t>{5, 9, 10});
    }
    SECTION("with Ripple Sequence Markers off the markers stay") {
        Rig rig(picture);
        marked(rig, false, {2, 6, 10});
        rig.view->setSnapping(false);
        rig.view->setTool(QStringLiteral("trackBackward"));
        rig.drag({rig.x(5.5), rig.y("V1")}, {rig.x(8.5), rig.y("V1")});
        CHECK(markerFrames(rig) == std::vector<std::int64_t>{2, 6, 10});
    }
    SECTION("the Forward tool still takes the markers after the selection, not before") {
        Rig rig(picture);
        marked(rig, true, {2, 6, 10});
        rig.view->setSnapping(false);
        rig.view->setTool(QStringLiteral("trackForward"));
        rig.drag({rig.x(5.5), rig.y("V1")}, {rig.x(8.5), rig.y("V1")});
        CHECK(markerFrames(rig) == std::vector<std::int64_t>{2, 9, 13});
    }
    SECTION("no marker at or before the last clip: the markers stay and the move is still one step") {
        Rig rig("V1 |AaaaBbbb....|", 10.0, false);
        marked(rig, true, {9, 10});
        rig.view->setSnapping(false);
        rig.view->setTool(QStringLiteral("trackBackward"));
        const std::size_t steps = rig.undoSteps();
        rig.drag({rig.x(5.5), rig.y("V1")}, {rig.x(7.5), rig.y("V1")});
        CHECK(markerFrames(rig) == std::vector<std::int64_t>{9, 10});
        CHECK(rig.undoSteps() == steps + 1);
    }
}

// ---- 113: Snap to ----------------------------------------------------------------------------------------------------------

TEST_CASE("Snap to: each kind of target can be left out (card 113)", "[app][timeline][snap]") {
    // Clip edges at 0, 4, 12 and 16; a marker at 8; the playhead at 20; an In mark at 26 (the picture is 30 frames wide).
    const std::string picture = "V1 |Aaaa........Bbbb..............|";
    Rig rig(picture, 10.0, false);
    Marker marker;
    marker.start = f(8);
    rig.tl.state = ok(edit::addMarker(rig.tl.state, rig.tl.seq, marker)).state;
    rig.controller.resetProject(rig.tl.state, rig.tl.seq);
    rig.controller.setPlayhead(f(20));
    rig.controller.setPlayhead(f(26));
    rig.controller.markInAtPlayhead();
    rig.controller.setPlayhead(f(20));
    rig.harness.frame();
    TimelineView& view = *rig.view;
    view.setSnapping(true);
    CHECK(view.snapTargetMask() == TimelineView::kSnapAll);

    // A drop point 5 px (half a frame) beside each target: snapped with the target on, plain without it.
    const auto snapped = [&](double frame) { return view.dropPoint(rig.x(frame) + 5.0, rig.y("V1")).value(QStringLiteral("snapped")).toBool(); };
    CHECK(snapped(4));   // a clip edge
    CHECK(snapped(8));   // a marker
    CHECK(snapped(20));  // the playhead
    CHECK(snapped(26));  // the In mark
    CHECK_FALSE(snapped(23)); // nothing there

    view.setSnapTargetMask(TimelineView::kSnapAll & ~TimelineView::kSnapClipEdges);
    CHECK_FALSE(snapped(4));
    CHECK(snapped(8));
    CHECK(snapped(20));
    view.setSnapTargetMask(TimelineView::kSnapAll & ~TimelineView::kSnapMarkers);
    CHECK(snapped(4));
    CHECK_FALSE(snapped(8));
    view.setSnapTargetMask(TimelineView::kSnapAll & ~TimelineView::kSnapPlayhead);
    CHECK_FALSE(snapped(20));
    CHECK(snapped(8));
    view.setSnapTargetMask(TimelineView::kSnapAll & ~TimelineView::kSnapMarks);
    CHECK_FALSE(snapped(26));
    CHECK(snapped(20));
    view.setSnapTargetMask(0);
    for (const double frame : {4.0, 8.0, 20.0, 26.0})
        CHECK_FALSE(snapped(frame));
    // Bits outside the five are ignored.
    view.setSnapTargetMask(0xFFFF);
    CHECK(view.snapTargetMask() == TimelineView::kSnapAll);
    // The model's options follow the mask.
    view.setSnapTargetMask(TimelineView::kSnapMarkers | TimelineView::kSnapBeats);
    const edit::SnapOptions options = view.snapOptions();
    CHECK_FALSE(options.clipEdges);
    CHECK_FALSE(options.playhead);
    CHECK_FALSE(options.marks);
    CHECK(options.markers);
    CHECK(options.beats);
}

TEST_CASE("A dragged clip snaps only to the targets that are on (card 113)", "[app][timeline][snap]") {
    // B starts at 12; a marker at 7. Dragging B 3.2 frames earlier (4 px per frame, 8 px reach) lands 1.8 frames from it.
    const auto dragB = [&](int mask) {
        Rig rig("V1 |Aaaa........Bbbb........|", 4.0, false);
        Marker marker;
        marker.start = f(7);
        rig.tl.state = ok(edit::addMarker(rig.tl.state, rig.tl.seq, marker)).state;
        rig.controller.resetProject(rig.tl.state, rig.tl.seq);
        rig.harness.frame();
        rig.view->setSnapping(true);
        rig.view->setSnapTargetMask(mask);
        const ClipId b = rig.clip("V1@12");
        rig.drag({rig.x(13.5), rig.y("V1")}, {rig.x(13.5) - 12.8, rig.y("V1")});
        const auto loc = rig.controller.currentSequence()->findClip(b);
        REQUIRE(loc.has_value());
        return loc->clip().start();
    };
    CHECK(dragB(TimelineView::kSnapAll) == f(7)); // the head sticks to the marker
    CHECK(dragB(TimelineView::kSnapAll & ~TimelineView::kSnapMarkers) != f(7));
}

// ---- 126, 142: what a clip says about itself -----------------------------------------------------------------------------

TEST_CASE("Clip duration is printed after the clip's name (card 126)", "[app][timeline][labels]") {
    const std::string row = run('A', 30) + run('B', 45) + std::string(10, '.');
    Rig rig("V1 |" + row + "|\nA1 |" + row + "|", 6.0, false);
    const ClipId a = rig.clip("V1@0");
    const ClipId b = rig.clip("V1@30");
    const ClipId audio = rig.clip("A1@30");
    const auto settle = [&] {
        for (int i = 0; i < 3; ++i)
            rig.harness.frame();
    };
    settle();
    const auto label = [&](ClipId id) { return rig.view->stats().labelTexts.value(id.value); };
    CHECK_FALSE(rig.view->showClipDuration()); // off by default: nothing changes for anyone
    CHECK(label(a) == QStringLiteral("A"));

    rig.view->setShowClipDuration(true);
    settle();
    CHECK(label(a) == QStringLiteral("A · 00:00:01:05")); // 30 frames at 25 fps
    CHECK(label(b) == QStringLiteral("B · 00:00:01:20"));
    CHECK(label(audio) == QStringLiteral("B · 00:00:01:20")); // audio clips too

    // A trim changes the number with the clip.
    REQUIRE(rig.controller.applyEditFn("Trim", [&](const ProjectState& s) {
        return edit::trimEdge(s, rig.tl.seq, b, edit::Edge::Tail, f(-2), edit::LinkMode::IgnoreLinks);
    }));
    settle();
    CHECK(label(b) == QStringLiteral("B · 00:00:01:18"));

    // After a speed badge, so a narrow clip loses the length first (the label is cut from its end).
    edit::SpeedChange half;
    half.speed = Speed{1, 2};
    half.keepDuration = true;
    const std::vector<ClipId> slow{a};
    REQUIRE(rig.controller.applyEditFn("Speed/Duration", [&](const ProjectState& s) {
        return edit::setSpeed(s, rig.tl.seq, slow, half, edit::LinkMode::IgnoreLinks);
    }));
    settle();
    CHECK(label(a) == QStringLiteral("A [50%] · 00:00:01:05"));

    rig.view->setShowClipDuration(false);
    settle();
    CHECK(label(a) == QStringLiteral("A [50%]"));
}

TEST_CASE("An audio clip of a recording with several tracks names its track from the file (card 142)",
          "[app][timeline][labels]") {
    const std::string row = run('A', 30) + run('B', 30) + std::string(10, '.');
    Rig rig("V1 |" + row + "|\nA1 |" + row + "|", 6.0, false);
    // Media A: the fixture's two audio streams, titled as OBS writes them. Media B: one stream with a title (a camera's
    // handler name) - not a recording with several tracks, so it shows nothing extra.
    ProjectState state = rig.tl.state;
    for (MediaPtr& item : state.media) {
        MediaItem edited = *item;
        if (edited.id == rig.tl.media('A')) {
            REQUIRE(edited.info.audio.size() >= 2);
            edited.info.audio[0].title = "Boom";
            edited.info.audio[1].title = "Lav 1";
        } else if (edited.id == rig.tl.media('B')) {
            edited.info.audio.resize(1);
            edited.info.audio[0].title = "Core Media Audio";
        }
        item = std::make_shared<const MediaItem>(std::move(edited));
    }
    rig.tl.state = state;
    rig.controller.resetProject(rig.tl.state, rig.tl.seq);
    for (int i = 0; i < 3; ++i)
        rig.harness.frame();
    const ClipId a = rig.clip("A1@0");
    const ClipId b = rig.clip("A1@30");
    const ClipId pictureA = rig.clip("V1@0");
    const auto label = [&](ClipId id) { return rig.view->stats().labelTexts.value(id.value); };
    CHECK(rig.view->showStreamNames());
    CHECK(label(a) == QStringLiteral("A · Boom"));
    CHECK(label(b) == QStringLiteral("B"));
    CHECK(label(pictureA) == QStringLiteral("A")); // pictures have no audio track names

    rig.view->setShowStreamNames(false);
    for (int i = 0; i < 3; ++i)
        rig.harness.frame();
    CHECK(label(a) == QStringLiteral("A"));
    rig.view->setShowStreamNames(true);
    rig.view->setShowClipDuration(true);
    for (int i = 0; i < 3; ++i)
        rig.harness.frame();
    CHECK(label(a) == QStringLiteral("A · Boom · 00:00:01:05")); // the length comes last
}

// ---- 182: Add tracks when editing clips in ---------------------------------------------------------------------------------

TEST_CASE("With 'Add tracks when editing clips in' off, edits use the tracks that are there (card 182)",
          "[app][timeline][tracks][settings]") {
    resetSettings();
    CHECK(editprefs::addTracksOnEdit()); // on by default: nothing changes for anyone
    const auto setPref = [](bool on) { QSettings().setValue(QLatin1String(editprefs::kAddTracksKey), on); };

    SECTION("a drop of a recording with two audio streams adds the track for the second, or leaves it out") {
        for (const bool on : {true, false}) {
            INFO("addTracks " << on);
            Project p(R"(
                V1 tgt |........|
                A1 tgt |........|
            )");
            editor::MediaDrop drop;
            drop.media = p.tl.media('A');
            drop.sequence = p.tl.seq;
            drop.at = f(0);
            drop.mode = editor::DropMode::Overwrite;
            drop.sourceRange = TimeRange{f(0), f(4)};
            drop.addTracks = on;
            const auto dropped = editor::dropMedia(p.controller.session(), drop);
            REQUIRE(dropped.has_value());
            const std::size_t audioTracks = p.sequence().audioTracks.size();
            if (on) {
                CHECK(audioTracks == 2); // the second stream got a track of its own
                CHECK(dropped->placed.size() == 3);
            } else {
                CHECK(audioTracks == 1); // only what fits the existing tracks
                CHECK(dropped->placed.size() == 2);
                CHECK(p.sequence().audioTracks.front()->clips.size() == 1);
            }
        }
    }
    SECTION("Paste adds the tracks the clipboard spans, or stops when the setting is off") {
        for (const bool on : {true, false}) {
            INFO("addTracks " << on);
            setPref(on);
            Project p(R"(
                V2     |Bbbb....|
                V1     |Aaaa....|
            )");
            // Both clips copied; the paste goes to V2 (targeted) and the second one would need a V3.
            p.controller.setTrackFlag(trackKey(p.tl.track("V2")), QStringLiteral("targeted"), true);
            p.controller.selectAll();
            p.controller.copySelection();
            p.controller.setPlayhead(f(20));
            const std::size_t videoBefore = p.sequence().videoTracks.size();
            p.controller.paste();
            if (on) {
                CHECK(p.sequence().videoTracks.size() == videoBefore + 1);
            } else {
                CHECK(p.sequence().videoTracks.size() == videoBefore);
                CHECK(p.controller.lastError().contains(QStringLiteral("no video track")));
            }
        }
        setPref(true);
    }
    resetSettings();
}

// ---- The shell: 063 pan and hold, keys, menus, the video solo button, screenshots -------------------------------------------

TEST_CASE("Holding H or Z uses the Hand or Zoom tool until the key comes up (card 063)", "[app][qml][timeline][tools]") {
    resetSettings();
    clearMessages();
    ShellProject p("V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|\nA1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|");
    QQuickWindow* window = p.shell.window.get();
    QQuickItem* panel = p.item(QStringLiteral("timelinePanel"));
    QQuickItem* lanes = p.item(QStringLiteral("timelineLanesArea"));
    REQUIRE(panel);
    REQUIRE(lanes);
    p.settle();
    lanes->forceActiveFocus();
    const auto tool = [&] { return panel->property("tool").toString(); };
    REQUIRE(tool() == QStringLiteral("select"));

    // A hold of H: the Hand tool while the key is down, then the Selection tool is back.
    keyDown(window, Qt::Key_H);
    CHECK(tool() == QStringLiteral("hand"));
    spin(450);
    keyUp(window, Qt::Key_H);
    CHECK(tool() == QStringLiteral("select"));

    // A tap of H: as before, it chooses the Hand tool and keeps it.
    tap(window, Qt::Key_H);
    CHECK(tool() == QStringLiteral("hand"));

    // Holding Z from the Hand tool: the Zoom tool meanwhile, then the Hand tool again.
    keyDown(window, Qt::Key_Z);
    CHECK(tool() == QStringLiteral("zoom"));
    spin(450);
    keyUp(window, Qt::Key_Z);
    CHECK(tool() == QStringLiteral("hand"));

    // A tool chosen while the key is down wins: V during a hold of Z keeps the Selection tool.
    tap(window, Qt::Key_V);
    CHECK(tool() == QStringLiteral("select"));
    keyDown(window, Qt::Key_Z);
    spin(450);
    tap(window, Qt::Key_C); // the Razor tool
    keyUp(window, Qt::Key_Z);
    CHECK(tool() == QStringLiteral("razor"));

    // Key repeat counts as holding even before the threshold.
    tap(window, Qt::Key_V);
    keyDown(window, Qt::Key_H);
    QKeyEvent repeat(QEvent::KeyPress, Qt::Key_H, Qt::NoModifier, QString(), true);
    QCoreApplication::sendEvent(window, &repeat);
    keyUp(window, Qt::Key_H);
    CHECK(tool() == QStringLiteral("select"));
    checkNoMessages();
}

TEST_CASE("A middle-button drag pans the timeline with any tool and edits nothing (card 063)",
          "[app][qml][timeline][tools]") {
    resetSettings();
    clearMessages();
    ShellProject p("V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|\nA1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|");
    QQuickWindow* window = p.shell.window.get();
    auto* view = qobject_cast<TimelineView*>(p.item(QStringLiteral("timelineView")));
    QQuickItem* lanes = p.item(QStringLiteral("timelineLanesArea"));
    QQuickItem* cursor = p.item(QStringLiteral("timelineMiddlePanCursor"));
    QQuickItem* panel = p.item(QStringLiteral("timelinePanel"));
    REQUIRE(view);
    REQUIRE(lanes);
    REQUIRE(cursor);
    REQUIRE(panel);
    p.settle();
    view->setPixelsPerSecond(900); // far wider than the window: there is room to scroll
    view->setScrollX(300);
    p.settle();
    REQUIRE(view->scrollX() > 100);
    const ProjectSnapshot before = p.controller->snapshot();
    const std::vector<ClipId> selectionBefore = p.controller->selection();
    CHECK(cursor->property("shape").toString().isEmpty());

    const auto pan = [&](double dx, double dy) {
        const QPointF at = lanes->mapToScene(QPointF(lanes->width() * 0.5, lanes->height() * 0.4));
        const double start = view->scrollX();
        sendMouse(window, QEvent::MouseButtonPress, at, Qt::MiddleButton, Qt::MiddleButton);
        for (int i = 1; i <= 5; ++i)
            sendMouse(window, QEvent::MouseMove, at + QPointF(dx * i / 5, dy * i / 5), Qt::NoButton, Qt::MiddleButton);
        const bool grabbing = cursor->property("shape").toString() == QStringLiteral("grab");
        sendMouse(window, QEvent::MouseButtonRelease, at + QPointF(dx, dy), Qt::MiddleButton, Qt::NoButton);
        return std::pair{view->scrollX() - start, grabbing};
    };

    // Dragging left moves the content left with the pointer: the view scrolls right by the distance.
    const auto [moved, grabbing] = pan(-120, 0);
    CHECK(moved == Catch::Approx(120.0).margin(1.0));
    CHECK(grabbing); // the closed hand while it drags
    CHECK(cursor->property("shape").toString().isEmpty());
    const auto back = pan(80, 0);
    CHECK(back.first == Catch::Approx(-80.0).margin(1.0));

    // With another tool too (the Razor must not cut), and nothing is edited or selected.
    REQUIRE(p.run("tool.razor"));
    CHECK(panel->property("tool").toString() == QStringLiteral("razor"));
    const auto razor = pan(-60, 0);
    CHECK(razor.first == Catch::Approx(60.0).margin(1.0));
    CHECK(p.controller->snapshot() == before);
    CHECK(p.controller->selection() == selectionBefore);
    // And with the Hand tool, whose left-button drag is unchanged.
    REQUIRE(p.run("tool.hand"));
    const auto hand = pan(-40, 0);
    CHECK(hand.first == Catch::Approx(40.0).margin(1.0));
    checkNoMessages();
}

TEST_CASE("The track keys, the range marker key and the edit keys run from the keyboard and the menus (cards 105 106 109 117)",
          "[app][qml][timeline][keys]") {
    resetSettings();
    clearMessages();
    ShellProject p(R"(
        V2     |Bbbb........|
        V1 tgt |AaaaCccc....|
        A1 tgt |AaaaCccc....|
    )", {{'S', 50}});
    QQuickWindow* window = p.shell.window.get();
    ProjectController& c = *p.controller;
    QQuickItem* lanes = p.item(QStringLiteral("timelineLanesArea"));
    REQUIRE(lanes);
    p.settle();
    lanes->forceActiveFocus();
    const auto track = [&](const char* label) -> const Track& { return *p.sequence().findTrack(p.tl.track(label)); };

    // Alt+Shift+O toggles the eye of the selected clip's track; Alt+Shift+S solos it alone; the menu rows exist.
    c.setSelection({p.tl.clip("V2@0")}, p.tl.clip("V2@0"));
    tap(window, Qt::Key_O, Qt::AltModifier | Qt::ShiftModifier);
    CHECK_FALSE(track("V2").enabled);
    tap(window, Qt::Key_O, Qt::AltModifier | Qt::ShiftModifier);
    CHECK(track("V2").enabled);
    tap(window, Qt::Key_S, Qt::AltModifier | Qt::ShiftModifier);
    CHECK(track("V2").solo);
    CHECK_FALSE(track("V1").solo);
    tap(window, Qt::Key_S, Qt::AltModifier | Qt::ShiftModifier);
    CHECK_FALSE(track("V2").solo);
    // Mute is an audio switch: with a picture selected the key says why it did nothing.
    clearMessages();
    tap(window, Qt::Key_U, Qt::AltModifier | Qt::ShiftModifier);
    CHECK_FALSE(track("A1").muted);
    c.setSelection({p.tl.clip("A1@0")}, p.tl.clip("A1@0"));
    tap(window, Qt::Key_U, Qt::AltModifier | Qt::ShiftModifier);
    CHECK(track("A1").muted);
    tap(window, Qt::Key_S, Qt::AltModifier | Qt::ControlModifier | Qt::ShiftModifier); // Toggle Track Solo
    CHECK(track("A1").solo);
    tap(window, Qt::Key_S, Qt::AltModifier | Qt::ControlModifier | Qt::ShiftModifier);
    CHECK_FALSE(track("A1").solo);
    for (const char* name : {"sequenceMenuToggleTrackOutput", "sequenceMenuToggleTrackMute", "sequenceMenuToggleTrackSolo",
                             "sequenceMenuSoloClipTrack", "markersMenuAddFromInOut", "clipMenuOverlay",
                             "clipMenuInsertBefore", "clipMenuInsertAfter", "clipMenuAppend"}) {
        INFO(name);
        QObject* row = p.shell.object(name);
        REQUIRE(row);
        CHECK_FALSE(row->property("command").toString().isEmpty());
    }

    // Shift+X: a range marker from In to Out.
    c.setPlayhead(f(2));
    c.markInAtPlayhead();
    c.setPlayhead(f(5));
    c.markOutAtPlayhead();
    tap(window, Qt::Key_X, Qt::ShiftModifier);
    REQUIRE(p.sequence().markers.size() == 1);
    CHECK(p.sequence().markers.front().start == f(2));
    CHECK(p.sequence().markers.front().duration == f(4));
    checkNoMessages();
}

TEST_CASE("Overlay, Insert Before, Insert After and Append at End edit the source's clip from the keyboard (card 109)",
          "[app][qml][timeline][keys]") {
    resetSettings();
    clearMessages();
    ShellProject p(R"(
        V1 tgt |Aaaa....Bbbb....|
        A1 tgt |Aaaa....Bbbb....|
    )", {{'S', 10}});
    ProjectController& c = *p.controller;
    auto* source = p.singleton<SourceMonitorBridge>("SourceMonitor");
    const auto clipsOn = [&](const char* label) { return p.sequence().findTrack(p.tl.track(label))->clips; };
    const auto startOf = [&](std::int64_t frame) {
        const Track* v1 = p.sequence().findTrack(p.tl.track("V1"));
        const auto i = v1->clipIndexAt(f(frame));
        REQUIRE(i.has_value());
        return v1->clips[*i].start();
    };

    // Nothing loaded: a toast says so, nothing changes.
    const std::size_t steps = c.session().undoCount();
    REQUIRE(p.run("clip.insertAfter"));
    CHECK(c.session().undoCount() == steps);

    REQUIRE(source->load(mediaKey(p.tl.media('S'))));
    REQUIRE(waitFor([&] { return source->loaded(); }, 3000));

    // Insert After the clip under the playhead (A: frames 0-3): S goes in at frame 4, B and what follows move 10 later.
    c.setPlayhead(f(2));
    REQUIRE(p.run("clip.insertAfter"));
    CHECK(c.session().undoCount() == steps + 1);
    CHECK(c.undoText() == QStringLiteral("Undo Insert After"));
    CHECK(startOf(4) == f(4));
    CHECK(clipsOn("V1").size() == 3);
    const Track* v1 = p.sequence().findTrack(p.tl.track("V1"));
    CHECK(v1->clips[1].media == p.tl.media('S'));
    CHECK(v1->clips[2].start() == f(8 + 10)); // B moved by S's length
    REQUIRE(c.undo());
    CHECK(clipsOn("V1").size() == 2);

    // Insert Before the clip under the playhead (B: frames 8-11): S goes in at frame 8.
    c.setPlayhead(f(9));
    REQUIRE(p.run("clip.insertBefore"));
    CHECK(c.undoText() == QStringLiteral("Undo Insert Before"));
    CHECK(startOf(8) == f(8));
    CHECK(p.sequence().findTrack(p.tl.track("V1"))->clips[1].media == p.tl.media('S'));
    REQUIRE(c.undo());

    // With no clip under the playhead there is nothing to insert next to: nothing changes.
    c.setPlayhead(f(14));
    const std::size_t before = c.session().undoCount();
    REQUIRE(p.run("clip.insertBefore"));
    CHECK(c.session().undoCount() == before);

    // Append at End: after the last clip (frame 12).
    REQUIRE(p.run("clip.appendEdit"));
    CHECK(c.undoText() == QStringLiteral("Undo Append at End"));
    const Track* after = p.sequence().findTrack(p.tl.track("V1"));
    CHECK(after->clips.back().media == p.tl.media('S'));
    CHECK(after->clips.back().start() == f(12));
    REQUIRE(c.undo());

    // Overlay: V1 is busy where S would land, so it goes on a new V2 above, in one undo step.
    c.setPlayhead(f(2));
    const std::size_t videoBefore = p.sequence().videoTracks.size();
    REQUIRE(p.run("clip.overlayEdit"));
    CHECK(c.undoText() == QStringLiteral("Undo Overlay"));
    REQUIRE(p.sequence().videoTracks.size() == videoBefore + 1);
    CHECK(p.sequence().videoTracks.back()->clips.size() == 1);
    CHECK(p.sequence().videoTracks.back()->clips.front().start() == f(2));
    REQUIRE(c.undo());
    CHECK(p.sequence().videoTracks.size() == videoBefore); // the added track went with the edit

    // With 'Add tracks when editing clips in' off, Overlay stops instead of adding the track.
    p.shell.preferences->setProperty("addTracksOnEdit", false);
    REQUIRE(waitFor([&] { return !editprefs::addTracksOnEdit(); }, 5000)); // the settings store writes a moment later
    const std::size_t undoBefore = c.session().undoCount();
    REQUIRE(p.run("clip.overlayEdit"));
    CHECK(c.session().undoCount() == undoBefore);
    CHECK(p.sequence().videoTracks.size() == videoBefore);
    p.shell.preferences->setProperty("addTracksOnEdit", true);
    REQUIRE(waitFor([&] { return editprefs::addTracksOnEdit(); }, 5000));
    checkNoMessages();
}

TEST_CASE("The video track header has a solo button, and the timeline shows the new labels (cards 106 126 142)",
          "[app][qml][timeline][tracks][screenshot]") {
    bool dark = true;
    SECTION("dark") { dark = true; }
    SECTION("light") { dark = false; }
    resetSettings();
    clearMessages();
    const std::string row = run('A', 30) + run('C', 30) + ".";
    ShellProject p("V2     |" + run('B', 61) + "|\nV1 tgt |" + row + "|\nA1 tgt |" + row + "|", {}, dark);
    ProjectController& c = *p.controller;
    // Media A: two audio streams titled as OBS writes them.
    ProjectState state = p.tl.state;
    for (MediaPtr& item : state.media) {
        if (item->id == p.tl.media('A')) {
            MediaItem edited = *item;
            edited.info.audio[0].title = "Game";
            edited.info.audio[1].title = "Mic";
            item = std::make_shared<const MediaItem>(std::move(edited));
        }
    }
    c.resetProject(state, p.tl.seq);
    p.shell.preferences->setProperty("timelineShowClipDuration", true);
    spin(200);

    QQuickItem* solo = p.item(QStringLiteral("trackSolo_V1"));
    QQuickItem* soloAudio = p.item(QStringLiteral("trackSolo_A1"));
    REQUIRE(solo);
    REQUIRE(soloAudio);
    CHECK(solo->isVisible());
    CHECK(solo->property("iconName").toString() == QStringLiteral("crosshair"));
    CHECK(soloAudio->property("iconName").toString() == QStringLiteral("headphones"));
    CHECK_FALSE(solo->property("checked").toBool());
    // The button sets the track's solo (one undo step), like the audio one.
    const std::size_t steps = c.session().undoCount();
    p.settle();
    {
        const QPointF at = solo->mapToScene(QPointF(solo->width() / 2, solo->height() / 2));
        sendMouse(p.shell.window.get(), QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
        sendMouse(p.shell.window.get(), QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
    }
    spin(50);
    CHECK(c.session().undoCount() == steps + 1);
    CHECK(p.sequence().findTrack(p.tl.track("V1"))->solo);
    CHECK(solo->property("checked").toBool());
    CHECK(c.undoText() == QStringLiteral("Undo Solo Track"));

    auto* view = qobject_cast<TimelineView*>(p.item(QStringLiteral("timelineView")));
    REQUIRE(view);
    view->setPixelsPerSecond(260);
    view->setScrollX(0);
    p.settle();
    CHECK(view->stats().labelTexts.value(p.tl.clip("V1@0").value) == QStringLiteral("A · 00:00:01:05"));
    CHECK(view->stats().labelTexts.value(p.tl.clip("A1@0").value) == QStringLiteral("A · Game · 00:00:01:05"));
    QQuickItem* panel = p.item(QStringLiteral("timelinePanel"));
    REQUIRE(panel);
    p.shot(sceneRect(panel), dark ? QStringLiteral("timeline-a2-labels-solo.png") : QStringLiteral("timeline-a2-labels-solo-light.png"));
    checkNoMessages();
}

// ---- The options' controls: display settings, Settings pages, the label palette --------------------------------------------

namespace {

double luminanceOf(const QColor& c) {
    const auto lin = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * lin(c.redF()) + 0.7152 * lin(c.greenF()) + 0.0722 * lin(c.blueF());
}
double contrastOf(const QColor& a, const QColor& b) {
    const double la = luminanceOf(a);
    const double lb = luminanceOf(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

QQuickItem* inside(QObject* container, const char* name) {
    auto* content = container->property("contentItem").value<QQuickItem*>();
    REQUIRE(content);
    QQuickItem* found = findNamed(content->parentItem() ? content->parentItem() : content, QLatin1String(name));
    INFO(name);
    REQUIRE(found);
    return found;
}

void flipOn(QObject* toggle, bool on) {
    REQUIRE(toggle);
    toggle->setProperty("checked", on);
    REQUIRE(QMetaObject::invokeMethod(toggle, "toggled"));
}

} // namespace

TEST_CASE("The display settings and Settings pages carry the snap, duration, track-name, tracks and label options "
          "(cards 113 118 126 142 182)",
          "[app][qml][settings][timeline][screenshot]") {
    bool dark = true;
    SECTION("dark") { dark = true; }
    SECTION("light") { dark = false; }
    resetSettings();
    clearMessages();
    ShellProject p("V1 tgt |" + run('A', 40) + "|\nA1 tgt |" + run('A', 40) + "|", {}, dark);
    QObject* preferences = p.shell.preferences;
    QQuickWindow* window = p.shell.window.get();
    REQUIRE(preferences);
    CHECK(preferences->property("snapTargetMask").toInt() == 31);
    CHECK_FALSE(preferences->property("timelineShowClipDuration").toBool());
    CHECK(preferences->property("timelineShowStreamNames").toBool());
    CHECK(preferences->property("addTracksOnEdit").toBool());
    CHECK(preferences->property("labelPalette").toString() == QStringLiteral("echo"));
    auto* view = qobject_cast<TimelineView*>(p.item(QStringLiteral("timelineView")));
    REQUIRE(view);

    // The Tracks panel's display settings: Snap to (five switches), Clip duration, Audio track names.
    QObject* popover = p.shell.object("timelineDisplayPopover");
    REQUIRE(popover);
    REQUIRE(QMetaObject::invokeMethod(popover, "open"));
    REQUIRE(waitFor([&] { return popover->property("opened").toBool(); }, 5000));
    spin(100);
    for (const char* name : {"snapTargetClipEdgesToggle", "snapTargetPlayheadToggle", "snapTargetMarksToggle",
                             "snapTargetMarkersToggle", "snapTargetBeatsToggle"}) {
        INFO(name);
        CHECK(inside(popover, name)->property("checked").toBool());
    }
    // Scrolled to show the group, for the screenshot.
    QQuickItem* group = inside(popover, "snapTargetMarkersToggle");
    QQuickItem* scroller = inside(popover, "timelineDisplayScroll");
    const double groupY = group->mapToItem(scroller->property("contentItem").value<QQuickItem*>(), QPointF(0, 0)).y();
    scroller->setProperty("contentY", std::max(0.0, groupY - scroller->height() / 2));
    spin(100);
    p.settle();
    p.shot(sceneRect(scroller).adjusted(-16, -48, 16, 16),
           dark ? QStringLiteral("timeline-a2-display-popover.png") : QStringLiteral("timeline-a2-display-popover-light.png"));
    flipOn(inside(popover, "snapTargetMarkersToggle"), false);
    CHECK(preferences->property("snapTargetMask").toInt() == (31 & ~8));
    CHECK(view->snapTargetMask() == (31 & ~8)); // the lanes follow at once
    flipOn(inside(popover, "snapTargetBeatsToggle"), false);
    CHECK(view->snapTargetMask() == (31 & ~8 & ~16));
    flipOn(inside(popover, "snapTargetMarkersToggle"), true);
    flipOn(inside(popover, "snapTargetBeatsToggle"), true);
    CHECK(view->snapTargetMask() == 31);
    flipOn(inside(popover, "clipDurationToggle"), true);
    CHECK(view->showClipDuration());
    flipOn(inside(popover, "streamNamesToggle"), false);
    CHECK_FALSE(view->showStreamNames());
    flipOn(inside(popover, "clipDurationToggle"), false);
    flipOn(inside(popover, "streamNamesToggle"), true);
    REQUIRE(QMetaObject::invokeMethod(popover, "close"));
    REQUIRE(waitFor([&] { return !popover->property("visible").toBool(); }, 5000));

    // Settings > Timeline & editing: the rows write the same Preferences.
    REQUIRE(QMetaObject::invokeMethod(window, "openSettings", Q_ARG(QString, QStringLiteral("timeline")),
                                      Q_ARG(QString, QStringLiteral("snapClipEdges"))));
    QObject* loader = p.shell.object("settingsWindowLoader");
    REQUIRE(loader);
    QObject* settings = nullptr;
    REQUIRE(waitFor([&] {
        settings = loader->property("item").value<QObject*>();
        return settings && settings->property("opened").toBool();
    }, 8000));
    const auto settingsPage = [&](const char* key) -> QQuickItem* {
        QQuickItem* candidate = nullptr;
        QMetaObject::invokeMethod(settings, "page", Q_RETURN_ARG(QQuickItem*, candidate), Q_ARG(QString, QLatin1String(key)));
        return candidate;
    };
    QQuickItem* page = nullptr;
    REQUIRE(waitFor([&] {
        page = settingsPage("timeline");
        return page && page->isVisible() && findNamed(page, QStringLiteral("settingsSnapTargetMarkers"));
    }, 8000));
    spin(300);
    p.settle();
    for (const char* name : {"settingsSnapTargetClipEdges", "settingsSnapTargetPlayhead", "settingsSnapTargetMarks",
                             "settingsSnapTargetMarkers", "settingsSnapTargetBeats", "settingsAddTracks", "settingsClipDuration",
                             "settingsStreamNames"}) {
        INFO(name);
        REQUIRE(findNamed(page, QLatin1String(name)));
    }
    p.shot(QRectF(0, 0, window->width(), window->height()),
           dark ? QStringLiteral("settings-timeline-a2.png") : QStringLiteral("settings-timeline-a2-light.png"));
    flipOn(findNamed(page, QStringLiteral("settingsSnapTargetMarks")), false);
    CHECK(preferences->property("snapTargetMask").toInt() == (31 & ~4));
    flipOn(findNamed(page, QStringLiteral("settingsSnapTargetMarks")), true);
    flipOn(findNamed(page, QStringLiteral("settingsAddTracks")), false);
    CHECK_FALSE(preferences->property("addTracksOnEdit").toBool());
    REQUIRE(waitFor([&] { return !editprefs::addTracksOnEdit(); }, 5000)); // the C++ side reads the same key
    flipOn(findNamed(page, QStringLiteral("settingsAddTracks")), true);
    REQUIRE(waitFor([&] { return editprefs::addTracksOnEdit(); }, 5000));
    flipOn(findNamed(page, QStringLiteral("settingsClipDuration")), true);
    CHECK(view->showClipDuration());
    flipOn(findNamed(page, QStringLiteral("settingsClipDuration")), false);

    // Settings > Appearance > Label colours: Classic swaps the 16 labels (all carry black names), Echo brings them back,
    // and a custom label of the theme still wins over the choice.
    const QVariantList echo = p.shell.theme->property("defaultClipColors").toList();
    const QVariantList classic = p.shell.theme->property("classicClipColors").toList();
    REQUIRE(echo.size() == 16);
    REQUIRE(classic.size() == 16);
    CHECK(echo != classic);
    const QColor ink = p.shell.themeColor("clipInk");
    for (const QVariant& label : classic)
        CHECK(contrastOf(ink, QColor(label.toString())) >= 4.5);
    CHECK(p.shell.theme->property("clipColors").toList() == echo);
    REQUIRE(QMetaObject::invokeMethod(window, "openSettings", Q_ARG(QString, QStringLiteral("appearance")),
                                      Q_ARG(QString, QStringLiteral("labelPalette"))));
    QQuickItem* appearance = nullptr;
    REQUIRE(waitFor([&] {
        appearance = settingsPage("appearance");
        return appearance && appearance->isVisible() && findNamed(appearance, QStringLiteral("labelPaletteTabs"));
    }, 8000));
    QQuickItem* tabs = findNamed(appearance, QStringLiteral("labelPaletteTabs"));
    REQUIRE(tabs);
    CHECK(tabs->property("currentIndex").toInt() == 0);
    REQUIRE(QMetaObject::invokeMethod(tabs, "activated", Q_ARG(int, 1)));
    CHECK(preferences->property("labelPalette").toString() == QStringLiteral("classic"));
    CHECK(p.shell.theme->property("clipColors").toList() == classic);
    // The row in view for the screenshot (the page's own Flickable scrolled to it).
    QQuickItem* row = findNamed(appearance, QStringLiteral("labelPaletteRow"));
    REQUIRE(row);
    QQuickItem* flick = nullptr;
    for (QQuickItem* up = row->parentItem(); up && !flick; up = up->parentItem()) {
        if (up->inherits("QQuickFlickable"))
            flick = up;
    }
    REQUIRE(flick);
    const double rowY = row->mapToItem(flick->property("contentItem").value<QQuickItem*>(), QPointF(0, 0)).y();
    flick->setProperty("contentY", std::max(0.0, rowY - 24));
    spin(300);
    p.settle();
    p.shot(QRectF(0, 0, window->width(), window->height()),
           dark ? QStringLiteral("settings-label-palette.png") : QStringLiteral("settings-label-palette-light.png"));
    QJSValue custom = p.shell.engine.newArray(16); // a theme sets its labels from JavaScript, as an array
    for (quint32 i = 0; i < 16; ++i)
        custom.setProperty(i, QStringLiteral("#123456"));
    p.shell.theme->setProperty("customClipColors", QVariant::fromValue(custom));
    CHECK(p.shell.theme->property("clipColors").toList() != classic); // custom labels win
    p.shell.theme->setProperty("customClipColors", QVariant::fromValue(p.shell.engine.newArray(0)));
    CHECK(p.shell.theme->property("clipColors").toList() == classic); // Reset labels: back to the choice
    REQUIRE(QMetaObject::invokeMethod(tabs, "activated", Q_ARG(int, 0)));
    CHECK(p.shell.theme->property("clipColors").toList() == echo);
    checkNoMessages();
}
