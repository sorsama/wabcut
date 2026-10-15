// M5a: ProjectController (model bridge) and TimelineView (batched scene-graph timeline).
//
// Performance is measured with QQuickRenderControl: a hidden QQuickWindow renders into an offscreen texture on
// the real RHI backend and keeps its scene graph between frames, so an update after a scroll or zoom measures
// what the app does (unlike grabWindow(), which rebuilds the scene graph for every grab).

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "CaptionsBridge.h"
#include "DemoProject.h"
#include "HomeBridge.h"
#include "KeymapDispatcher.h"
#include "ProgramToolsBridge.h"
#include "ProjectController.h"
#include "ShortcutsBridge.h"
#include "MaskBridge.h"
#include "SourceMonitorBridge.h"
#include "TimelineView.h"
#include "TranscriptBridge.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectOps.h>

#include <QCoreApplication>
#include <QKeyEvent>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <rhi/qrhi.h>

#include <algorithm>
#include <memory>
#include <numeric>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

double ms(qint64 ns) {
    return static_cast<double>(ns) / 1e6;
}

double percentile(std::vector<double> values, double p) {
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(std::min<double>(static_cast<double>(values.size()) - 1,
                                                                  p * static_cast<double>(values.size())));
    return values[index];
}

QString rowShortName(ProjectController& controller, int row) {
    return controller.tracks()->data(controller.tracks()->index(row), TrackListModel::ShortNameRole).toString();
}

// Count of model clips in the current sequence.
int totalClips(const ProjectController& controller) {
    return controller.clipCount();
}

} // namespace

TEST_CASE("ProjectController builds the M1 demo project through the model", "[app][timeline]") {
    ProjectController controller;
    CHECK(controller.sequenceName() == QStringLiteral("Sequence 01"));
    CHECK(controller.sequenceNames()
          == QStringList({QStringLiteral("Sequence 01"), QStringLiteral("Social Cut 9x16"), QStringLiteral("Trailer 30s")}));
    CHECK(controller.frameRateText() == QStringLiteral("23.976 fps"));
    CHECK(controller.formatText() == QStringLiteral("1080p23.976"));
    CHECK(controller.durationText() == QStringLiteral("00:01:20:00"));
    CHECK(controller.clipCount() == 16);
    CHECK(controller.markers().size() == 5);
    CHECK(controller.playheadText() == QStringLiteral("00:00:12:08"));

    REQUIRE(controller.tracks()->rowCount() == 6);
    const QStringList order{QStringLiteral("V3"), QStringLiteral("V2"), QStringLiteral("V1"),
                            QStringLiteral("A1"), QStringLiteral("A2"), QStringLiteral("A3")};
    for (int i = 0; i < 6; ++i)
        CHECK(rowShortName(controller, i) == order[i]);

    // The shell opens with City B-roll selected (as in the M1 design).
    const QVariantMap clip = controller.selectedClip();
    CHECK(clip.value(QStringLiteral("name")).toString() == QStringLiteral("City B-roll"));
    CHECK(clip.value(QStringLiteral("trackName")).toString() == QStringLiteral("V2"));
    CHECK(clip.value(QStringLiteral("rangeText")).toString() == QStringLiteral("00:00:04:00 → 00:00:18:00"));
    CHECK(clip.value(QStringLiteral("properties")).toMap().value(QStringLiteral("scale")).toDouble() == 100.0);

    // Valid model state.
    REQUIRE(validate(*controller.snapshot()));
}

TEST_CASE("No raw ticks or 64-bit ids cross into QML (principle 9)", "[app][timeline]") {
    const auto check = [](const QMetaObject& meta) {
        QStringList offenders;
        const auto is64 = [](QMetaType type) {
            return type == QMetaType::fromType<qint64>() || type == QMetaType::fromType<quint64>()
                || type == QMetaType::fromType<long>() || type == QMetaType::fromType<unsigned long>();
        };
        for (int i = meta.propertyOffset(); i < meta.propertyCount(); ++i) {
            if (is64(meta.property(i).metaType()))
                offenders << QString::fromLatin1(meta.property(i).name());
        }
        for (int i = meta.methodOffset(); i < meta.methodCount(); ++i) {
            const QMetaMethod m = meta.method(i);
            if (m.access() != QMetaMethod::Public)
                continue;
            if (is64(m.returnMetaType()))
                offenders << QString::fromLatin1(m.methodSignature());
            for (int p = 0; p < m.parameterCount(); ++p) {
                if (is64(m.parameterMetaType(p)))
                    offenders << QString::fromLatin1(m.methodSignature());
            }
        }
        return offenders;
    };
    INFO("ProjectController: " << join(check(ProjectController::staticMetaObject)));
    CHECK(check(ProjectController::staticMetaObject).isEmpty());
    INFO("TimelineView: " << join(check(TimelineView::staticMetaObject)));
    CHECK(check(TimelineView::staticMetaObject).isEmpty());
    // The transcript, captions, Home screen and Keyboard Shortcuts bridges (M5e, M5f), the Source Monitor and the
    // Program Monitor's tools (M5h).
    for (const QMetaObject* meta : {&TranscriptBridge::staticMetaObject, &CaptionsBridge::staticMetaObject,
                                    &HomeBridge::staticMetaObject, &ShortcutsBridge::staticMetaObject,
                                    &SourceMonitorBridge::staticMetaObject, &ProgramToolsBridge::staticMetaObject,
                                    &MaskBridge::staticMetaObject}) {
        INFO(meta->className() << ": " << join(check(*meta)));
        CHECK(check(*meta).isEmpty());
    }
}

TEST_CASE("Edits go through the EditSession: undo, redo and change notification", "[app][timeline]") {
    ProjectController controller;
    QList<QVariantMap> changed;
    QObject::connect(&controller, &ProjectController::changed, [&](const QVariantMap& changeSet) { changed << changeSet; });
    const QString v1 = controller.tracks()->data(controller.tracks()->index(2), TrackListModel::TrackIdRole).toString();
    REQUIRE(rowShortName(controller, 2) == QStringLiteral("V1"));
    REQUIRE_FALSE(controller.canUndo());

    SECTION("track toggles") {
        controller.setTrackFlag(v1, QStringLiteral("locked"), true);
        CHECK(controller.canUndo());
        CHECK(controller.undoText() == QStringLiteral("Undo Lock Track"));
        CHECK(controller.tracks()->data(controller.tracks()->index(2), TrackListModel::LockedRole).toBool());
        REQUIRE(changed.size() == 1);
        const QVariantMap changeSet = changed.takeFirst();
        CHECK(changeSet.value(QStringLiteral("kind")).toString() == QStringLiteral("edit"));
        CHECK(changeSet.value(QStringLiteral("tracks")).toStringList() == QStringList{v1}); // only V1 changed
        CHECK_FALSE(changeSet.value(QStringLiteral("structural")).toBool());

        REQUIRE(controller.undo());
        CHECK_FALSE(controller.tracks()->data(controller.tracks()->index(2), TrackListModel::LockedRole).toBool());
        CHECK(controller.redoText() == QStringLiteral("Redo Lock Track"));
        REQUIRE(controller.redo());
        CHECK(controller.tracks()->data(controller.tracks()->index(2), TrackListModel::LockedRole).toBool());
    }

    SECTION("clip properties merge per gesture") {
        const auto scale = [&] {
            return controller.selectedClip().value(QStringLiteral("properties")).toMap().value(QStringLiteral("scale")).toDouble();
        };
        controller.setClipProperty(QStringLiteral("scale"), 120);
        controller.setClipProperty(QStringLiteral("scale"), 150); // same drag: merged
        controller.endPropertyGesture();
        controller.setClipProperty(QStringLiteral("scale"), 80);  // a new gesture
        CHECK(scale() == 80.0);
        CHECK(controller.undoText() == QStringLiteral("Undo Scale"));
        REQUIRE(controller.undo());
        CHECK(scale() == 150.0);
        REQUIRE(controller.undo());
        CHECK(scale() == 100.0);
        CHECK_FALSE(controller.canUndo());
    }

    SECTION("audio properties edit the linked audio clip") {
        const auto interviewA = controller.currentSequence()->videoTracks[0]->clips.front().id;
        controller.selectClipById(interviewA, ProjectController::Replace);
        CHECK(controller.selection().size() == 2); // linked selection: video + audio
        CHECK(controller.selectedClip().value(QStringLiteral("hasAudio")).toBool());
        controller.setClipProperty(QStringLiteral("volume"), -6.0);
        const Clip& audio = controller.currentSequence()->audioTracks[0]->clips.front();
        CHECK(audio.properties().value(ParamId::VolumeDb) == -6.0);
        CHECK(controller.undoText() == QStringLiteral("Undo Volume"));
    }

    SECTION("new sequence is undoable and switches to it") {
        controller.newSequence();
        CHECK(controller.sequenceName() == QStringLiteral("Sequence 04"));
        CHECK(controller.currentSequenceIndex() == 3);
        REQUIRE(controller.undo());
        CHECK(controller.sequenceNames().size() == 3);
        CHECK(controller.currentSequenceIndex() >= 0); // fell back to an existing sequence
    }

    SECTION("playhead and edit points") {
        controller.goToStart();
        CHECK(controller.playheadText() == QStringLiteral("00:00:00:00"));
        // Shift+Down/Up: edit points on any track.
        controller.goToNextEdit(true); // City B-roll starts at 4 s
        CHECK(controller.playheadText() == QStringLiteral("00:00:04:00"));
        controller.goToNextEdit(true); // Lower Third at 8 s
        CHECK(controller.playheadText() == QStringLiteral("00:00:08:00"));
        controller.goToPreviousEdit(true);
        CHECK(controller.playheadText() == QStringLiteral("00:00:04:00"));
        // Down/Up: only the targeted tracks (V1 and A1 in a new sequence): Interview A ends at 24 s.
        controller.goToNextEdit();
        CHECK(controller.playheadText() == QStringLiteral("00:00:24:00"));
        controller.goToPreviousEdit();
        CHECK(controller.playheadText() == QStringLiteral("00:00:00:00"));
        // With no track targeted, every track counts.
        for (int row = 0; row < controller.tracks()->rowCount(); ++row) {
            const QModelIndex index = controller.tracks()->index(row);
            controller.setTrackFlag(controller.tracks()->data(index, TrackListModel::TrackIdRole).toString(),
                                    QStringLiteral("targeted"), false);
        }
        controller.goToNextEdit();
        CHECK(controller.playheadText() == QStringLiteral("00:00:04:00"));
        controller.stepFrames(3);
        CHECK(controller.playheadText() == QStringLiteral("00:00:04:03"));
        controller.goToEnd();
        CHECK(controller.playheadText() == QStringLiteral("00:01:20:00"));
        controller.seekToMarker(1);
        CHECK(controller.playheadText() == QStringLiteral("00:00:24:00"));
        CHECK(controller.playheadMarkerIndex() == 1);
    }
}

TEST_CASE("TimelineView hit-testing and selection", "[app][timeline]") {
    ProjectController controller;
    RenderHarness harness(QSize(1200, 300));
    REQUIRE(harness.initialize());
    TimelineView* view = addView(harness, controller, QSize(1200, 300));
    harness.frame();

    // Rows: V3 (0-40), V2 (42-82), V1 (84-124), A1, A2, A3. City B-roll is on V2 from 4 s to 18 s.
    const double pps = view->pixelsPerSecond();
    const double x = 10.0 * pps;
    const QString city = view->clipAt(x, 60);
    CHECK_FALSE(city.isEmpty());
    CHECK(controller.clipName(city) == QStringLiteral("City B-roll"));
    CHECK(view->trackAt(60) == controller.tracks()->data(controller.tracks()->index(1), TrackListModel::TrackIdRole).toString());
    CHECK(view->trackAt(41).isEmpty()); // the gap between rows
    CHECK(view->clipAt(1.0 * pps, 60).isEmpty()); // V2 is empty before 4 s
    CHECK(view->timeAt(10.0 * pps).startsWith(QStringLiteral("00:00:09:")));

    // Click selection: replace, toggle with Shift, empty click clears.
    view->selectAt(x, 60, Qt::NoModifier);
    CHECK(controller.selectedClips() == QStringList{city});
    const QString lowerThird = view->clipAt(10.0 * pps, 20);
    REQUIRE_FALSE(lowerThird.isEmpty());
    view->selectAt(10.0 * pps, 20, Qt::ShiftModifier);
    CHECK(controller.selectedClips().size() == 2);
    view->selectAt(10.0 * pps, 20, Qt::ShiftModifier);
    CHECK(controller.selectedClips() == QStringList{city});
    view->selectAt(1.0 * pps, 60, Qt::NoModifier);
    CHECK(controller.selectedClips().isEmpty());

    // Marquee over V1 + A1 in the first 5 s: Interview A video + audio.
    view->selectInRect(QRectF(QPointF(0.5 * pps, 90), QPointF(5.0 * pps, 140)), Qt::NoModifier);
    CHECK(controller.selectedClips().size() == 2);

    // Ruler scrub moves the playhead to the frame under x.
    view->seekToX(10.0 * pps);
    CHECK(controller.playheadText().startsWith(QStringLiteral("00:00:09:")));
    CHECK(std::abs(view->playheadX() - 10.0 * pps) < pps / 23.0);

    // Selection is drawn: the overlay was rebuilt after the change.
    harness.frame();
    CHECK(view->stats().visibleClips > 0);
    CHECK(view->stats().labels > 0);
}

TEST_CASE("Edits rebuild only the tracks whose ChangeSet reaches the built range", "[app][timeline]") {
    ProjectController controller;
    DemoProject stress = buildStressProject({3, 3, 10'000, 10});
    controller.resetProject(std::move(stress.state), stress.mainSequence);
    RenderHarness harness(QSize(1400, 400));
    REQUIRE(harness.initialize());
    TimelineView* view = addView(harness, controller, QSize(1400, 400));
    harness.frame();
    REQUIRE(view->stats().fullRebuild);

    const SequencePtr seq = controller.currentSequence();
    const ClipId nearClip = seq->videoTracks[0]->clips.front().id; // in view
    const ClipId farClip = seq->videoTracks[0]->clips.back().id;   // about 11 minutes later
    const auto relabel = [&](ClipId id, int color) {
        edit::ClipAttributeChange change;
        change.colorIndex = color;
        const ClipId ids[] = {id};
        const SequenceId seqId = controller.currentSequenceId();
        REQUIRE(controller.session().apply(
            "Label", [&](const ProjectState& state) { return edit::setClipAttributes(state, seqId, ids, change); }));
    };

    // An edit outside the built window: its track is in the ChangeSet but keeps its geometry.
    relabel(farClip, 5);
    REQUIRE(controller.lastChange().tracks.size() == 1);
    CHECK_FALSE(controller.lastChange().structural);
    harness.frame();
    CHECK_FALSE(view->stats().fullRebuild);
    CHECK(view->stats().rebuiltTracks == 0);
    CHECK(view->stats().skippedTracks == 1);

    // An edit in view rebuilds that one track.
    relabel(nearClip, 6);
    harness.frame();
    CHECK(view->stats().rebuiltTracks == 1);
    CHECK(view->stats().skippedTracks == 0);
    const double editMs = static_cast<double>(view->stats().updateNs) / 1e6;
    INFO("single-clip edit frame: " << editMs << " ms");
    CHECK(editMs < 4.0);

    // Undo and redo deliver ChangeSets too.
    REQUIRE(controller.undo());
    CHECK(controller.lastChange().tracks.size() == 1);
    harness.frame();
    CHECK(view->stats().rebuiltTracks == 1);
    REQUIRE(controller.undo());
    harness.frame();
    CHECK(view->stats().rebuiltTracks == 0);
    CHECK(view->stats().skippedTracks == 1);
    REQUIRE(controller.redo());
    harness.frame();
    CHECK(view->stats().rebuiltTracks == 0);

    // Scrolling to the far clip rebuilds from the current revision: hit-testing finds it.
    const double farX = seq->videoTracks[0]->clips.back().start().toSeconds() * view->pixelsPerSecond();
    view->setScrollX(farX - 100);
    harness.frame();
    CHECK(view->stats().fullRebuild);
    const double yV1 = view->rows()[2].y + 5; // rows: V3, V2, V1, A1, A2, A3
    CHECK(view->clipAt(105, yV1) == clipKey(farClip));

    // A marker-only edit is not a track change.
    const SequenceId seqId = controller.currentSequenceId();
    REQUIRE(controller.session().apply("Add Marker", [&](const ProjectState& state) {
        return edit::addMarker(state, seqId, Marker{Ticks::fromSeconds(2), {}, "M", {}, 1});
    }));
    CHECK(controller.lastChange().markersChanged);
    CHECK(controller.lastChange().tracks.empty());
    harness.frame();
    CHECK(view->stats().rebuiltTracks == 0);
}

TEST_CASE("Sequence marks and parameter values go through the model", "[app][timeline]") {
    ProjectController controller;
    QList<QVariantMap> changes;
    QObject::connect(&controller, &ProjectController::changed, [&](const QVariantMap& c) { changes << c; });

    SECTION("In/Out marks are undoable edits shown as timecode") {
        controller.goToStart();
        controller.stepFrames(48);
        controller.markInAtPlayhead();
        CHECK(controller.markInText() == QStringLiteral("00:00:02:00"));
        CHECK(controller.undoText() == QStringLiteral("Undo Mark In"));
        REQUIRE_FALSE(changes.isEmpty());
        CHECK(changes.last().value(QStringLiteral("marks")).toBool());
        CHECK(changes.last().value(QStringLiteral("tracks")).toStringList().isEmpty());
        controller.stepFrames(48);
        controller.markOutAtPlayhead();
        CHECK(controller.markOutText() == QStringLiteral("00:00:04:00")); // the last marked frame
        controller.goToStart();
        controller.goToMarkOut();
        CHECK(controller.playheadText() == QStringLiteral("00:00:04:00"));
        controller.goToMarkIn();
        CHECK(controller.playheadText() == QStringLiteral("00:00:02:00"));
        controller.clearMarks(true, true);
        CHECK(controller.markInText().isEmpty());
        CHECK(controller.markOutText().isEmpty());
        CHECK(controller.undoText() == QStringLiteral("Undo Clear In and Out"));
        REQUIRE(controller.undo());
        CHECK(controller.markInText() == QStringLiteral("00:00:02:00"));

        // Mark Clip (X): around the clips under the playhead on the targeted tracks (V1/A1: Interview A).
        controller.goToStart();
        controller.markClipAtPlayhead();
        CHECK(controller.markInText() == QStringLiteral("00:00:00:00"));
        CHECK(controller.markOutText() == QStringLiteral("00:00:23:23"));
    }

    SECTION("the clip editor reads the parameter table and clamps to its ranges") {
        // City B-roll is selected; opacity is a 0..1 fraction shown in percent.
        controller.setClipProperty(QStringLiteral("opacity"), 150.0);
        CHECK(controller.selectedClip().value(QStringLiteral("properties")).toMap().value(QStringLiteral("opacity")).toDouble()
              == 100.0);
        controller.setClipProperty(QStringLiteral("opacity"), 40.0);
        const auto city = controller.selection().front();
        const auto loc = controller.currentSequence()->findClip(city);
        REQUIRE(loc);
        CHECK(loc->clip().properties().value(ParamId::Opacity) == 0.4);
        // Position is shown from the frame's top-left, stored as an offset from the centre.
        controller.setClipProperty(QStringLiteral("positionX"), 1000.0);
        const auto moved = controller.currentSequence()->findClip(city);
        CHECK(moved->clip().properties().value(ParamId::PositionX) == 40.0);
        CHECK(controller.selectedClip().value(QStringLiteral("properties")).toMap().value(QStringLiteral("positionX")).toDouble()
              == 1000.0);
    }
}

TEST_CASE("Keyboard commands reach the timeline through KeymapDispatcher", "[app][timeline][keys]") {
    Shell shell;
    INFO(join(shell.errors));
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* keys = shell.engine.singletonInstance<KeymapDispatcher*>("Wabcut", "KeymapDispatcher");
    REQUIRE(controller != nullptr);
    REQUIRE(keys != nullptr);
    CHECK(keys->window() == shell.window.get()); // Main.qml attaches it explicitly
    QTemporaryDir config;
    keys->setOverridesPath(config.filePath(QStringLiteral("keymap/overrides.json")));
    keys->setPlatform(QStringLiteral("windows"));

    QObject* timelinePanel = shell.object("timelinePanel");
    QObject* project = shell.object("mockProject");
    auto* view = qobject_cast<TimelineView*>(shell.object("timelineView"));
    QQuickItem* lanes = shell.item("timelineLanesArea");
    REQUIRE(timelinePanel != nullptr);
    REQUIRE(project != nullptr);
    REQUIRE(view != nullptr);
    REQUIRE(lanes != nullptr);
    lanes->forceActiveFocus();
    // The lanes focus scope hands focus to the TimelineView inside it (Esc cancels its drags).
    REQUIRE(shell.window->activeFocusItem() == view);
    CHECK(keys->scopeAt(view) == QStringLiteral("timeline"));

    const auto tap = [&](int key, Qt::KeyboardModifiers mods = Qt::NoModifier, const QString& text = {}) {
        QKeyEvent press(QEvent::KeyPress, key, mods, text);
        QCoreApplication::sendEvent(shell.window.get(), &press);
        QKeyEvent release(QEvent::KeyRelease, key, mods, text);
        QCoreApplication::sendEvent(shell.window.get(), &release);
    };
    clearMessages();

    // Playhead navigation.
    tap(Qt::Key_Home);
    CHECK(controller->playheadText() == QStringLiteral("00:00:00:00"));
    tap(Qt::Key_Right);
    CHECK(controller->playheadText() == QStringLiteral("00:00:00:01"));
    tap(Qt::Key_Right, Qt::ShiftModifier);
    CHECK(controller->playheadText() == QStringLiteral("00:00:00:06"));
    tap(Qt::Key_Left);
    CHECK(controller->playheadText() == QStringLiteral("00:00:00:05"));
    tap(Qt::Key_Down, Qt::ShiftModifier); // any track: City B-roll at 4 s
    CHECK(controller->playheadText() == QStringLiteral("00:00:04:00"));
    tap(Qt::Key_Down); // targeted V1/A1: Interview A ends at 24 s
    CHECK(controller->playheadText() == QStringLiteral("00:00:24:00"));
    tap(Qt::Key_Up);
    CHECK(controller->playheadText() == QStringLiteral("00:00:00:00"));
    tap(Qt::Key_End);
    CHECK(controller->playheadText() == QStringLiteral("00:01:20:00"));
    CHECK(view->playheadX() >= 0.0); // revealed
    CHECK(view->playheadX() <= view->width());

    // Zoom: = and - around the playhead, \ fits the sequence.
    const double level = view->zoomLevel();
    tap(Qt::Key_Equal, Qt::NoModifier, QStringLiteral("="));
    CHECK(view->zoomLevel() > level);
    tap(Qt::Key_Minus, Qt::NoModifier, QStringLiteral("-"));
    CHECK(view->zoomLevel() == Catch::Approx(level));
    tap(Qt::Key_Backslash, Qt::NoModifier, QStringLiteral("\\"));
    CHECK(view->zoomLevel() == 0.0);

    // Tools and snapping (view state).
    tap(Qt::Key_C, Qt::NoModifier, QStringLiteral("c"));
    CHECK(timelinePanel->property("tool").toString() == QStringLiteral("razor"));
    tap(Qt::Key_V, Qt::NoModifier, QStringLiteral("v"));
    CHECK(timelinePanel->property("tool").toString() == QStringLiteral("select"));
    const bool snapping = timelinePanel->property("snapping").toBool();
    tap(Qt::Key_S, Qt::NoModifier, QStringLiteral("s"));
    CHECK(timelinePanel->property("snapping").toBool() == !snapping);

    // Selection.
    tap(Qt::Key_A, Qt::ControlModifier, QStringLiteral("\x01"));
    CHECK(controller->selection().size() == 16);
    tap(Qt::Key_A, Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral("\x01"));
    CHECK(controller->selection().empty());

    // Marks (model edits) and history.
    tap(Qt::Key_Home);
    tap(Qt::Key_I, Qt::NoModifier, QStringLiteral("i"));
    CHECK(controller->markInText() == QStringLiteral("00:00:00:00"));
    tap(Qt::Key_Down, Qt::ShiftModifier);
    tap(Qt::Key_O, Qt::NoModifier, QStringLiteral("o"));
    CHECK(controller->markOutText() == QStringLiteral("00:00:04:00"));
    QQuickItem* markedRange = shell.item("rulerMarkedRange");
    REQUIRE(markedRange != nullptr);
    CHECK(markedRange->isVisible());
    tap(Qt::Key_X, Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral("\x18"));
    CHECK(controller->markInText().isEmpty());
    CHECK_FALSE(markedRange->isVisible());
    tap(Qt::Key_Z, Qt::ControlModifier, QStringLiteral("\x1a"));
    CHECK(controller->markInText() == QStringLiteral("00:00:00:00"));
    tap(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral("\x1a"));
    CHECK(controller->markInText().isEmpty());

    // Transport: Space plays, J/K/L shuttle (mock playback state until PlaybackController is bridged).
    tap(Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
    CHECK(project->property("playing").toBool());
    tap(Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
    CHECK_FALSE(project->property("playing").toBool());
    tap(Qt::Key_L, Qt::NoModifier, QStringLiteral("l"));
    CHECK(project->property("shuttleSpeed").toDouble() == 1.0);
    tap(Qt::Key_L, Qt::NoModifier, QStringLiteral("l"));
    CHECK(project->property("shuttleSpeed").toDouble() == 2.0);
    tap(Qt::Key_K, Qt::NoModifier, QStringLiteral("k"));
    CHECK(project->property("shuttleSpeed").toDouble() == 0.0);
    CHECK_FALSE(project->property("playing").toBool());

    // Tooltips show the dispatcher's shortcut text.
    QObject* undoButton = shell.object("undoButton");
    REQUIRE(undoButton != nullptr);
    CHECK(undoButton->property("shortcut").toString() == keys->shortcutText(QStringLiteral("edit.undo")));
    CHECK_FALSE(undoButton->property("shortcut").toString().isEmpty());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

namespace {

/// Filmstrips that are always ready (M5i bench): one 16-tile atlas shared by every media item.
class ReadyFilmstrips final : public app::FilmstripSource {
public:
    ReadyFilmstrips() {
        std::vector<QImage> frames;
        std::vector<Ticks> times;
        for (int i = 0; i < 16; ++i) {
            QImage image(128, 72, QImage::Format_RGBA8888);
            image.fill(QColor::fromHsv(i * 22, 160, 200));
            frames.push_back(image);
            times.push_back(Ticks::fromSeconds(i));
        }
        m_atlas = app::buildFilmstripAtlas(frames, times, 72);
        m_atlas->serial = 1;
    }
    Lookup lookup(MediaId) const override { return {State::Ready, m_atlas}; }

private:
    std::shared_ptr<app::FilmstripAtlas> m_atlas;
};

struct TimelineBench {
    app::TimelineStats first, fit;
    double scrollMean = 0, scrollP95 = 0, scrollMax = 0, scrollRaw = 0;
    double zoomMean = 0, zoomP95 = 0, zoomMax = 0, zoomRaw = 0;
    double warmUp = 0;
    QStringList zoomFrames;
};

/// The 10,000-clip bench: first build, fit-all, then scrolling and zooming (best of three passes per frame). With
/// `filmstrips`, every video clip draws thumbnails (Continuous).
TimelineBench runTimelineBench(std::shared_ptr<const app::FilmstripSource> filmstrips) {
    TimelineBench r;
    ProjectController controller;
    DemoProject stress = buildStressProject({3, 3, 10'000, 10});
    controller.resetProject(std::move(stress.state), stress.mainSequence);
    REQUIRE(totalClips(controller) == 10'000);
    REQUIRE(validate(*controller.snapshot()));

    const QSize size(1400, 400);
    RenderHarness harness(size);
    REQUIRE(harness.initialize());
    TimelineView* view = addView(harness, controller, size);
    view->setFilmstripMode(QStringLiteral("continuous"));
    if (filmstrips)
        view->setFilmstripSource(std::move(filmstrips));
    view->setPixelsPerSecond(48.0); // the shell's default zoom: 10-frame clips are ~20 px wide

    // First scene graph.
    harness.frame();
    r.first = view->stats();
    const app::TimelineStats& first = r.first;
    INFO("first build: " << ms(first.updateNs) << " ms, nodes " << first.nodes << ", built " << first.builtClips
                         << ", visible " << first.visibleClips << ", labels " << first.labels << ", film tiles "
                         << first.filmstripTiles);
    CHECK(first.fullRebuild);
    CHECK(ms(first.updateNs) < 50.0);
    const int tracks = 6;
    // Node count: 10 fixed + two meshes per track (clips, and what goes over the thumbnails) + one textured node per
    // track and media atlas + the label text nodes (each with its glyph nodes); labels only for clips in the view.
    CHECK(first.nodes == 10 + 2 * tracks + first.filmstripNodes + first.labelNodes);
    CHECK(first.labels <= first.visibleClips);
    CHECK(first.labelNodes <= 4 * first.labels);
    CHECK(first.builtClips <= 2 * first.visibleClips + 2 * tracks); // view + half a viewport each side
    CHECK(first.nodes < 1000); // independent of the 10,000 clips in the sequence

    // Fit-all zoom: every clip is in the built range, sub-pixel runs keep the build cheap.
    view->setZoomLevel(0.0);
    harness.frame();
    r.fit = view->stats();
    INFO("fit-all build: " << ms(r.fit.updateNs) << " ms, built " << r.fit.builtClips << ", nodes " << r.fit.nodes);
    CHECK(r.fit.builtClips == 10'000);
    CHECK(ms(r.fit.updateNs) < 50.0);
    CHECK(r.fit.nodes == 10 + 2 * tracks + r.fit.filmstripNodes + r.fit.labelNodes);
    view->setPixelsPerSecond(48.0);
    harness.frame();

    // Warm the glyph cache once (the first labels rasterise their glyphs; the app pays this at start-up).
    const double startLevel = view->zoomLevel();
    view->zoomAt(0.7, size.width() / 2.0);
    harness.frame();
    r.warmUp = ms(view->stats().updateNs);
    view->zoomAt(startLevel, size.width() / 2.0);
    harness.frame();

    // Scrolling (120 frames of 23 px, inside and across the built range) and zooming (60 frames around the
    // cursor). Each sequence runs three times from the same start and every frame keeps its fastest time:
    // this filters scheduler preemption on a loaded machine without hiding slow frames that are slow every
    // time. The raw first-pass mean is reported too.
    const auto runPasses = [&](int frames, const std::function<void()>& reset, const std::function<void(int)>& step,
                               std::vector<double>& best, double& rawMean, QStringList& slowest) {
        best.assign(static_cast<std::size_t>(frames), 1e9);
        std::vector<QString> describe(static_cast<std::size_t>(frames));
        for (int pass = 0; pass < 3; ++pass) {
            reset();
            harness.frame();
            double sum = 0;
            for (int i = 0; i < frames; ++i) {
                step(i);
                harness.frame();
                const app::TimelineStats& st = view->stats();
                const double t = ms(st.updateNs);
                sum += t;
                CHECK(st.nodes == 10 + 2 * tracks + st.filmstripNodes + st.labelNodes);
                CHECK(st.builtClips <= 2 * st.visibleClips + 2 * tracks);
                if (t < best[static_cast<std::size_t>(i)]) {
                    best[static_cast<std::size_t>(i)] = t;
                    describe[static_cast<std::size_t>(i)] =
                        QStringLiteral("%1 ms: %2 px/s, built %3, labels %4, film tiles %7, rebuilt tracks %5%6")
                            .arg(t, 0, 'f', 3)
                            .arg(view->pixelsPerSecond(), 0, 'f', 1)
                            .arg(st.builtClips)
                            .arg(st.labels)
                            .arg(st.rebuiltTracks)
                            .arg(st.labelsPending ? QStringLiteral(", labels pending") : QString())
                            .arg(st.filmstripTiles);
                }
            }
            if (pass == 0)
                rawMean = sum / frames;
        }
        std::vector<std::size_t> order(static_cast<std::size_t>(frames));
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return best[a] > best[b]; });
        for (std::size_t k = 0; k < std::min<std::size_t>(5, order.size()); ++k)
            slowest << describe[order[k]];
    };

    const double scrollStart = view->scrollX();
    std::vector<double> scroll;
    QStringList scrollSlowest;
    runPasses(
        120, [&] { view->setScrollX(scrollStart); }, [&](int) { view->setScrollX(view->scrollX() + 23.0); }, scroll,
        r.scrollRaw, scrollSlowest);

    std::vector<double> zoom;
    runPasses(
        60, [&] { view->zoomAt(startLevel, size.width() / 2.0); },
        [&](int i) { view->zoomAt(view->zoomLevel() + (i < 30 ? 0.004 : -0.004), size.width() / 2.0); }, zoom,
        r.zoomRaw, r.zoomFrames);

    const auto mean = [](const std::vector<double>& v) { return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size()); };
    r.scrollMean = mean(scroll);
    r.scrollP95 = percentile(scroll, 0.95);
    r.scrollMax = percentile(scroll, 1.0);
    r.zoomMean = mean(zoom);
    r.zoomP95 = percentile(zoom, 0.95);
    r.zoomMax = percentile(zoom, 1.0);
    INFO("scroll mean/p95/max " << r.scrollMean << "/" << r.scrollP95 << "/" << r.scrollMax << " ms; zoom "
                                << r.zoomMean << "/" << r.zoomP95 << "/" << r.zoomMax << " ms");
    CHECK(r.scrollMean < 4.0);
    CHECK(r.scrollMax < 4.0);
    CHECK(r.zoomMean < 4.0);
    CHECK(r.zoomP95 < 4.0);
    return r;
}

QString benchText(const TimelineBench& b) {
    return QStringLiteral("First scene graph: %1 ms (nodes %2, built clips %3, visible clips %4, labels %5, film tiles %18)\n"
                          "Fit-all first build: %6 ms (built clips %7, nodes %8)\n"
                          "Scroll update (120 frames, best of 3 passes): mean %9 ms, p95 %10 ms, max %11 ms (raw first-pass mean %16 ms)\n"
                          "Zoom update (60 frames, best of 3 passes): mean %12 ms, p95 %13 ms, max %14 ms (raw first-pass mean %17 ms)\n"
                          "Glyph-cache warm-up frame (first labels, excluded): %15 ms\n")
               .arg(ms(b.first.updateNs), 0, 'f', 2)
               .arg(b.first.nodes)
               .arg(b.first.builtClips)
               .arg(b.first.visibleClips)
               .arg(b.first.labels)
               .arg(ms(b.fit.updateNs), 0, 'f', 2)
               .arg(b.fit.builtClips)
               .arg(b.fit.nodes)
               .arg(b.scrollMean, 0, 'f', 3)
               .arg(b.scrollP95, 0, 'f', 3)
               .arg(b.scrollMax, 0, 'f', 3)
               .arg(b.zoomMean, 0, 'f', 3)
               .arg(b.zoomP95, 0, 'f', 3)
               .arg(b.zoomMax, 0, 'f', 3)
               .arg(b.warmUp, 0, 'f', 2)
               .arg(b.scrollRaw, 0, 'f', 3)
               .arg(b.zoomRaw, 0, 'f', 3)
               .arg(b.first.filmstripTiles)
           + QStringLiteral("Slowest zoom frames:\n  ") + b.zoomFrames.mid(0, 5).join(QStringLiteral("\n  ")) + QStringLiteral("\n");
}

} // namespace

TEST_CASE("Timeline performance with 10,000 clips", "[app][timeline][perf]") {
    // Without thumbnails (media without files), then with every video clip showing a full filmstrip (M5i): the
    // filmstrips may cost at most 1 ms more at the 95th percentile.
    const TimelineBench off = runTimelineBench(nullptr);
    CHECK(off.first.filmstripTiles == 0);
    const TimelineBench on = runTimelineBench(std::make_shared<ReadyFilmstrips>());
    CHECK(on.first.filmstripTiles > 0);
    CHECK(on.first.filmstripNodes > 0);
    INFO("p95 scroll " << off.scrollP95 << " -> " << on.scrollP95 << " ms, zoom " << off.zoomP95 << " -> " << on.zoomP95
                       << " ms");
    CHECK(on.scrollP95 - off.scrollP95 <= 1.0);
    CHECK(on.zoomP95 - off.zoomP95 <= 1.0);

    writeOutputFile(QStringLiteral("timeline-perf.txt"),
                    QStringLiteral("10,000-clip sequence (3 video + 3 audio tracks, 10-frame clips), view 1400x400 at 48 px/s\n\n"
                                   "Filmstrips off (media without files):\n")
                        + benchText(off) + QStringLiteral("\nFilmstrips on (Continuous, every atlas ready):\n") + benchText(on)
                        + QStringLiteral("\nFilmstrip cost at p95: scroll %1 ms, zoom %2 ms (limit +1 ms)\n")
                              .arg(on.scrollP95 - off.scrollP95, 0, 'f', 3)
                              .arg(on.zoomP95 - off.zoomP95, 0, 'f', 3));
}

TEST_CASE("The lanes have no per-clip QML delegates", "[app][timeline]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    QQuickItem* lanes = shell.item("timelineLanes");
    REQUIRE(lanes != nullptr);
    const auto countItems = [](const QQuickItem* root) {
        int n = 0;
        std::function<void(const QQuickItem*)> walk = [&](const QQuickItem* item) {
            ++n;
            for (const QQuickItem* child : item->childItems())
                walk(child);
        };
        walk(root);
        return n;
    };
    const int withDemo = countItems(lanes);
    CHECK(withDemo <= 3); // TimelineLanes + TimelineView

    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(controller != nullptr);
    DemoProject stress = buildStressProject({3, 3, 10'000, 10});
    controller->resetProject(std::move(stress.state), stress.mainSequence);
    CHECK(countItems(lanes) == withDemo);
    REQUIRE_FALSE(shell.grab().isNull());

    const QStringList messages = takeMessages();
    INFO("Qt messages:\n" << join(messages));
    CHECK(messages.isEmpty());
}
