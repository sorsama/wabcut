// 0.2 timeline parity in the app: Group / Ungroup (Ctrl+G / Ctrl+Shift+G) and how a group selects, moves and trims,
// Selection Follows Playhead, Go to Gap (Next / Previous in Sequence), Select Next / Previous Clip, the render bar, fade
// handles and their display switches, Show Audio Time Units and sequence label colours.

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "BridgeIds.h"
#include "MediaReadinessBridge.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "SequenceTabsBridge.h"
#include "TimelineFixture.h"
#include "TimelineView.h"

#include <catch2/catch_test_macros.hpp>
#include <wabcut/model/ClipFades.h>
#include <wabcut/model/ClipGroups.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/PreviewCache.h>
#include <wabcut/model/ProjectIO.h>

#include <QUrl>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <span>
#include <utility>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

std::vector<ClipId> sortedIds(std::vector<ClipId> ids) {
    std::ranges::sort(ids);
    return ids;
}

void mouse(QQuickWindow* window, QEvent::Type type, QPointF scene, Qt::MouseButton button, Qt::MouseButtons buttons,
           Qt::KeyboardModifiers mods = {}) {
    QMouseEvent event(type, scene, scene, button, buttons, mods);
    QCoreApplication::sendEvent(window, &event);
}

} // namespace

TEST_CASE("Grouped clips select and move together and Alt-click picks one", "[app][timeline][groups]") {
    Rig rig(R"(
        V2     |......Cccc......|
        V1 tgt |Aaaa..Bbbb......|
        A1 tgt |Aaaa..Bbbb......|
    )");
    const ClipId a = rig.clip("V1@0");
    const ClipId c = rig.clip("V2@6");
    rig.controller.setSelection({a, c});
    REQUIRE(rig.controller.runCommand(QStringLiteral("clip.group")));
    CHECK(rig.controller.undoText() == QStringLiteral("Undo Group"));
    CHECK(rig.controller.selectionGrouped());

    // A click on C selects the group: A, C and A's linked audio.
    rig.controller.clearSelection();
    rig.press({rig.x(8), rig.y("V2")});
    rig.release({rig.x(8), rig.y("V2")});
    CHECK(sortedIds(rig.controller.selection()) == sortedIds({a, c, rig.clip("A1@0")}));

    // Alt-click: that clip alone.
    rig.controller.clearSelection();
    rig.press({rig.x(8), rig.y("V2")}, Qt::AltModifier);
    rig.release({rig.x(8), rig.y("V2")}, Qt::AltModifier);
    CHECK(rig.controller.selection() == std::vector<ClipId>{c});

    // Nudging the group moves every member (B is not in it).
    rig.controller.selectClipById(a, ProjectController::Replace);
    REQUIRE(rig.controller.runCommand(QStringLiteral("timeline.nudgeClipRight")));
    CHECK(rig.picture() == normalize(R"(
        V2 |.......Cccc.....|
        V1 |.Aaaa.Bbbb......|
        A1 |.Aaaa.Bbbb......|
    )"));

    // Ungroup: a click selects C alone again.
    rig.controller.undo();
    rig.controller.selectClipById(c, ProjectController::Replace);
    REQUIRE(rig.controller.runCommand(QStringLiteral("clip.ungroup")));
    CHECK(rig.controller.undoText() == QStringLiteral("Undo Ungroup"));
    CHECK_FALSE(rig.controller.selectionGrouped());
    rig.controller.selectClipById(c, ProjectController::Replace);
    CHECK(rig.controller.selection() == std::vector<ClipId>{c});
}

TEST_CASE("The pen edits a video clip's opacity rubber band", "[app][timeline][pen]") {
    Rig rig(R"(
        V1 |Aaaaaaaaaaaaaaaaaaaa....|
        A1 |Aaaaaaaaaaaaaaaaaaaa....|
    )");
    const ClipId v = rig.clip("V1@0");
    const TrackId v1 = rig.tl.track("V1");
    const auto band = [&] {
        for (const auto& row : rig.view->rows()) {
            if (row.track->id == v1)
                return TimelineView::volumeArea(QRectF(rig.x(0), row.y - rig.view->scrollY(), rig.x(20) - rig.x(0), row.height));
        }
        FAIL("no V1 row");
        return QRectF();
    };
    const auto opacity = [&]() -> ParamValue {
        return rig.controller.currentSequence()->findClip(v)->clip().properties()[ParamId::Opacity];
    };
    // The selection tool leaves video clips alone: a click there selects, nothing is keyed.
    rig.press({rig.x(4), band().top()});
    rig.release({rig.x(4), band().top()});
    CHECK_FALSE(opacity().isCurve());

    rig.view->setTool(QStringLiteral("pen"));
    rig.harness.frame();
    // A click on the line (100 %, the band's top) adds a key there.
    rig.press({rig.x(4), band().top()});
    rig.release({rig.x(4), band().top()});
    REQUIRE(opacity().isCurve());
    REQUIRE(opacity().curve()->keys.size() == 1);
    CHECK(opacity().curve()->keys[0].value == 1.0);
    CHECK(rig.controller.undoText() == QStringLiteral("Undo Add Opacity Keyframe"));
    rig.press({rig.x(12), band().top()}); // a second key
    rig.release({rig.x(12), band().top()});
    REQUIRE(opacity().curve()->keys.size() == 2);

    // Dragging the second key half way down: 50 %, one undo step.
    const std::size_t steps = rig.undoSteps();
    const QRectF area = band();
    rig.drag({rig.x(12), area.top()}, {rig.x(12), area.top() + area.height() / 2});
    REQUIRE(opacity().curve()->keys.size() == 2);
    CHECK(std::abs(opacity().curve()->keys[1].value - 0.5) < 0.05);
    CHECK(opacity().curve()->keys[0].value == 1.0);
    CHECK(rig.undoSteps() == steps + 1);
    CHECK(rig.controller.undoText() == QStringLiteral("Undo Opacity Keyframe"));
    // The audio clip's volume band is untouched.
    CHECK_FALSE(rig.controller.currentSequence()->findClip(rig.clip("A1@0"))->clip().properties()[ParamId::VolumeDb].isCurve());
}

TEST_CASE("Dragging a selected clip's fade handles makes fades", "[app][timeline][fades]") {
    Rig rig(R"(
        V1 |Aaaaaaaaaaaaaaaaaaaa....|
        A1 |Aaaaaaaaaaaaaaaaaaaa....|
    )",
            10.0, false);
    const ClipId v = rig.clip("V1@0");
    const TrackId v1 = rig.tl.track("V1");
    double top = 0;
    for (const auto& row : rig.view->rows()) {
        if (row.track->id == v1)
            top = row.y - rig.view->scrollY();
    }
    const double handleY = top + 4.5;
    const auto fade = [&](edit::Edge edge) { return edit::clipFade(*rig.controller.currentSequence(), v, edge); };

    // Not selected: no handles, the press selects the clip (and may start a move).
    rig.press({rig.x(0) + 3.5, handleY});
    rig.release({rig.x(0) + 3.5, handleY});
    REQUIRE(rig.controller.isSelected(v));
    rig.harness.frame();

    const std::size_t steps = rig.undoSteps();
    rig.drag({rig.x(0) + 3.5, handleY}, {rig.x(5), handleY});
    CHECK(fade(edit::Edge::Head) == f(5));
    CHECK(rig.undoSteps() == steps + 1);
    CHECK(rig.controller.undoText() == QStringLiteral("Undo Fade In"));
    CHECK(rig.view->speedReadout().isEmpty()); // the readout goes with the drag
    const Track& track = *rig.controller.currentSequence()->findTrack(v1);
    REQUIRE(track.transitions.size() == 1);
    CHECK(track.transitions[0].effect == "crossDissolve");

    // The fade out handle in the top-right corner.
    rig.harness.frame();
    rig.drag({rig.x(20) - 3.5, handleY}, {rig.x(16), handleY});
    CHECK(fade(edit::Edge::Tail) == f(4));
    CHECK(rig.controller.undoText() == QStringLiteral("Undo Fade Out"));
    // The model still has the fade in; the clip did not move.
    CHECK(fade(edit::Edge::Head) == f(5));
    CHECK(rig.clip("V1@0") == v);
}

TEST_CASE("Selection Follows Playhead selects the clips under the playhead", "[app][timeline][selection]") {
    Rig rig(R"(
        V2     |Cccc........|
        V1 tgt |AaaaBbbb....|
        A1 tgt |AaaaBbbb....|
    )");
    CHECK_FALSE(rig.controller.selectionFollowsPlayhead());
    rig.controller.setPlayhead(f(5));
    CHECK(rig.controller.selection().empty()); // off: the playhead leaves the selection alone

    REQUIRE(rig.controller.runCommand(QStringLiteral("sequence.selectionFollowsPlayhead")));
    CHECK(rig.controller.selectionFollowsPlayhead());
    CHECK(sortedIds(rig.controller.selection()) == sortedIds({rig.clip("V1@4"), rig.clip("A1@4")}));
    rig.controller.setPlayhead(f(1)); // V2 is not targeted
    CHECK(sortedIds(rig.controller.selection()) == sortedIds({rig.clip("V1@0"), rig.clip("A1@0")}));
    rig.controller.setPlayhead(f(10));
    CHECK(rig.controller.selection().empty());

    REQUIRE(rig.controller.runCommand(QStringLiteral("sequence.selectionFollowsPlayhead")));
    CHECK_FALSE(rig.controller.selectionFollowsPlayhead());
    rig.controller.setPlayhead(f(1));
    CHECK(rig.controller.selection().empty());
}

TEST_CASE("Go to Gap and Select Next or Previous Clip", "[app][timeline][selection]") {
    Rig rig(R"(
        V1 tgt |Aaaa..Bbbb..Cc..|
        A1 tgt |Aaaa..Bbbb..Cc..|
    )");
    rig.controller.setPlayhead(f(0));
    REQUIRE(rig.controller.runCommand(QStringLiteral("sequence.nextInSequence")));
    CHECK(rig.controller.playhead() == f(4));
    REQUIRE(rig.controller.runCommand(QStringLiteral("sequence.nextInSequence")));
    CHECK(rig.controller.playhead() == f(10));
    REQUIRE(rig.controller.runCommand(QStringLiteral("sequence.nextInSequence")));
    CHECK(rig.controller.playhead() == f(10)); // no gap after the last clip
    REQUIRE(rig.controller.runCommand(QStringLiteral("sequence.previousInSequence")));
    CHECK(rig.controller.playhead() == f(4));

    rig.controller.selectClipById(rig.clip("V1@0"), ProjectController::Replace);
    REQUIRE(rig.controller.runCommand(QStringLiteral("timeline.selectNextClip")));
    CHECK(sortedIds(rig.controller.selection()) == sortedIds({rig.clip("V1@6"), rig.clip("A1@6")}));
    REQUIRE(rig.controller.runCommand(QStringLiteral("timeline.selectNextClip")));
    CHECK(rig.controller.primaryClip() == rig.clip("V1@12"));
    REQUIRE(rig.controller.runCommand(QStringLiteral("timeline.selectPreviousClip")));
    CHECK(rig.controller.primaryClip() == rig.clip("V1@6"));
}

TEST_CASE("Zoom tool marquee zooms to fit the dragged range", "[app][timeline][tools][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    REQUIRE_FALSE(shell.grab().isNull());
    auto* view = qobject_cast<TimelineView*>(shell.item("timelineView"));
    QQuickItem* panel = shell.item("timelinePanel");
    QQuickItem* area = shell.item("timelineViewToolArea");
    REQUIRE(view);
    REQUIRE(panel);
    REQUIRE(area);
    REQUIRE(panel->setProperty("tool", QStringLiteral("zoom")));
    REQUIRE(area->property("enabled").toBool());

    // Drag from 40 % to 50 % of the width: that time range then fills the view.
    const double x0 = area->width() * 0.4;
    const double x1 = area->width() * 0.5;
    const double t0 = (x0 + view->scrollX()) / view->pixelsPerSecond();
    const double t1 = (x1 + view->scrollX()) / view->pixelsPerSecond();
    const double y = area->height() / 2;
    const ProjectSnapshot before = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController")->snapshot();
    mouse(shell.window.get(), QEvent::MouseButtonPress, area->mapToScene({x0, y}), Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 5; ++i)
        mouse(shell.window.get(), QEvent::MouseMove, area->mapToScene({x0 + (x1 - x0) * i / 5, y + i}), Qt::NoButton,
              Qt::LeftButton);
    CHECK(area->property("marqueeActive").toBool());
    mouse(shell.window.get(), QEvent::MouseButtonRelease, area->mapToScene({x1, y + 5}), Qt::LeftButton, Qt::NoButton);
    CHECK_FALSE(area->property("marqueeActive").toBool());
    CHECK(std::abs(view->scrollX() / view->pixelsPerSecond() - t0) < 1e-3);
    CHECK(std::abs((view->scrollX() + view->width()) / view->pixelsPerSecond() - t1) < 1e-3);
    // Nothing was edited.
    CHECK(shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController")->snapshot() == before);
    const QStringList messages = takeMessages();
    INFO(messages.join(QLatin1Char('\n')).toStdString());
    CHECK(messages.isEmpty());
}

// =====================================================================================================================
// 0.2 timeline parity, second round: the render bar, fade handles on hover and their display switches, audio time
// units, a group's outer edges, Select Next / Previous Clip across the targeted tracks, sequence label colours.

namespace {

/// Writes a small file per segment (Render In to Out in tests).
class BarPreviewRenderer final : public PreviewRenderer {
public:
    std::expected<std::int64_t, Error> render(const ProjectState&, SequenceId, TimeRange, const std::filesystem::path& file,
                                              const std::function<bool(double)>&) override {
        std::ofstream(file, std::ios::binary) << "preview";
        return std::int64_t{7};
    }
};

/// The first item named `name` under `root` (Repeater delegates are not QObject children: walk the item tree).
QQuickItem* itemNamed(QQuickItem* root, const QString& name) {
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = itemNamed(child, name))
            return found;
    }
    return nullptr;
}

edit::EditOutcome addBlur(const ProjectState& s, SequenceId seq, ClipId clip) {
    return edit::addEffect(s, seq, std::span(&clip, 1), "wabcut.blur.gaussian");
}

} // namespace

TEST_CASE("The render bar shows render.status's segments and follows edits after a short delay",
          "[app][timeline][renderbar]") {
    Rig rig(R"(
        V1 |Aaaaa.Bbbbb.....|
    )",
            10.0, false);
    const ClipId a = rig.clip("V1@0");
    rig.view->refreshRenderBar();
    CHECK(rig.view->renderBar().empty()); // plain clips play in real time: no bar (Premiere)
    CHECK(rig.view->renderBarItems().isEmpty());

    // A blur makes A yellow. The bar follows the edit a moment later (a burst of edits costs one evaluation).
    REQUIRE(rig.controller.applyEditFn("Add Effect", [&](const ProjectState& s) { return addBlur(s, rig.tl.seq, a); }));
    CHECK(rig.view->renderBar().empty());
    REQUIRE(waitFor([&] { return !rig.view->renderBar().empty(); }, 5000));
    REQUIRE(rig.view->renderBar().size() == 1);
    const TimelineView::RenderBarSegment yellow = rig.view->renderBar().front();
    CHECK(yellow.status == RenderStatus::Yellow);
    CHECK(yellow.start == f(0));
    CHECK(yellow.end == f(5));
    CHECK_FALSE(yellow.reason.isEmpty());
    const QVariantMap item = rig.view->renderBarItems().front().toMap();
    CHECK(item.value(QStringLiteral("status")).toString() == QStringLiteral("yellow"));
    CHECK(std::abs(item.value(QStringLiteral("end")).toDouble() - f(5).toSeconds()) < 1e-9);
    // The same as render.status says.
    const std::vector<RenderSegment> model = renderSegments(*rig.controller.snapshot(), rig.tl.seq);
    CHECK(std::ranges::count_if(model, [](const RenderSegment& s) { return s.status == RenderStatus::Yellow; }) == 1);

    // B's blur too: adjacent segments of one status merge only when they touch; A and B are apart.
    const ClipId b = rig.clip("V1@6");
    REQUIRE(rig.controller.applyEditFn("Add Effect", [&](const ProjectState& s) { return addBlur(s, rig.tl.seq, b); }));
    REQUIRE(waitFor([&] { return rig.view->renderBar().size() == 2; }, 5000));
    // Undo twice: no bar again.
    rig.controller.undo();
    rig.controller.undo();
    REQUIRE(waitFor([&] { return rig.view->renderBar().empty(); }, 5000));
}

TEST_CASE("The render bar turns green after Render In to Out, and yellow again after an edit inside",
          "[app][timeline][renderbar][render]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "app" / "render-bar";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaa|
    )");
    const ClipId clip = clipAt(tl.state, tl.seq, "V1@0");
    const ProjectState blurred = ok(addBlur(tl.state, tl.seq, clip)).state;
    const fs::path project = dir / "bar.wabcut";
    REQUIRE(io::saveProject(blurred, project, io::SaveOptions{0}));

    ProjectController controller;
    ProjectLifecycle lifecycle(&controller);
    REQUIRE(lifecycle.open(QUrl::fromLocalFile(QString::fromStdU16String(project.u16string()))));
    PlaybackBridge playback(&controller);
    MediaReadinessBridge readiness(&lifecycle, &playback);
    readiness.setPreviewRendererForTests(std::make_shared<BarPreviewRenderer>());
    readiness.setRenderSynchronous(true);
    RenderHarness harness(QSize(600, 200));
    REQUIRE(harness.initialize());
    TimelineView* view = addView(harness, controller, QSize(600, 200));
    view->setReadiness(&readiness);
    view->refreshRenderBar();
    REQUIRE(view->renderBar().size() == 1);
    CHECK(view->renderBar().front().status == RenderStatus::Yellow);

    CHECK(readiness.renderInToOut().isEmpty());
    REQUIRE(waitFor([&] { return view->renderBar().size() == 1 && view->renderBar().front().status == RenderStatus::Rendered; },
                    5000));
    CHECK(view->renderBarItems().front().toMap().value(QStringLiteral("status")).toString() == QStringLiteral("rendered"));

    // A second effect on the clip changes what the segment shows: it needs rendering again.
    REQUIRE(controller.applyEditFn("Add Effect", [&](const ProjectState& s) {
        return edit::addEffect(s, tl.seq, std::span(&clip, 1), "wabcut.sharpen");
    }));
    REQUIRE(waitFor([&] { return !view->renderBar().empty() && view->renderBar().front().status == RenderStatus::Yellow; },
                    5000));
}

TEST_CASE("Fade handles show on the clip under the pointer; the display settings hide them per kind",
          "[app][timeline][fades]") {
    Rig rig(R"(
        V1 |Aaaaaaaaaaaaaaaaaaaa....|
        A1 |Aaaaaaaaaaaaaaaaaaaa....|
    )",
            10.0, false);
    const ClipId v = rig.clip("V1@0");
    const ClipId a = rig.clip("A1@0");
    const auto rowTop = [&](std::string_view track) {
        const TrackId id = rig.tl.track(track);
        for (const auto& row : rig.view->rows()) {
            if (row.track->id == id)
                return row.y - rig.view->scrollY();
        }
        FAIL("no row");
        return 0.0;
    };
    const double videoHandle = rowTop("V1") + 4.5;
    const double audioHandle = rowTop("A1") + 4.5;
    const auto hover = [&](QPointF at) {
        // The hidden test window does not turn a mouse move into a hover event, but it remembers where the mouse is and
        // hovers the item there at each frame: move it there, then hover the view directly.
        rig.send(QEvent::MouseMove, at, Qt::NoButton, Qt::NoButton, {});
        QHoverEvent event(QEvent::HoverMove, at, at, at);
        QCoreApplication::sendEvent(rig.view, &event);
        rig.harness.frame();
    };
    const auto fade = [&](ClipId clip, edit::Edge edge) { return edit::clipFade(*rig.controller.currentSequence(), clip, edge); };

    // Nothing selected: hovering the clip shows its handles, and dragging one makes the fade (Premiere 26).
    REQUIRE(rig.controller.selection().empty());
    hover({rig.x(10), videoHandle});
    CHECK(rig.view->hoveredClip() == clipKey(v));
    rig.drag({rig.x(0) + 3.5, videoHandle}, {rig.x(4), videoHandle});
    CHECK(fade(v, edit::Edge::Head) == f(4));
    CHECK(rig.controller.undoText() == QStringLiteral("Undo Fade In"));
    CHECK(rig.controller.selection().empty()); // the drag did not select it
    rig.controller.undo();

    // Show Video Fade Handles off: no handle on video clips; the press there selects the clip instead.
    rig.view->setShowVideoFadeHandles(false);
    hover({rig.x(10), videoHandle});
    rig.press({rig.x(0) + 3.5, videoHandle});
    rig.release({rig.x(0) + 3.5, videoHandle});
    CHECK_FALSE(fade(v, edit::Edge::Head).has_value());
    CHECK(rig.controller.isSelected(v));
    rig.controller.clearSelection();

    // Audio clips keep theirs (Show Audio Fade Handles is on): the fade out handle in the top-right corner.
    hover({rig.x(10), audioHandle});
    CHECK(rig.view->hoveredClip() == clipKey(a));
    rig.drag({rig.x(20) - 3.5, audioHandle}, {rig.x(15), audioHandle});
    CHECK(fade(a, edit::Edge::Tail) == f(5));
    rig.controller.undo();
    rig.view->setShowAudioFadeHandles(false);
    hover({rig.x(10), audioHandle});
    rig.press({rig.x(20) - 3.5, audioHandle});
    rig.release({rig.x(20) - 3.5, audioHandle});
    CHECK_FALSE(fade(a, edit::Edge::Tail).has_value());
}

TEST_CASE("Trimming a group trims its outer edges; an inner edge moves the group; Alt trims one clip",
          "[app][timeline][groups]") {
    Rig rig(R"(
        V2     |Cccccc..........|
        V1 tgt |Aaaa............|
    )",
            10.0, false);
    const ClipId a = rig.clip("V1@0");
    const ClipId c = rig.clip("V2@0");
    rig.controller.setSelection({a, c});
    REQUIRE(rig.controller.runCommand(QStringLiteral("clip.group")));
    rig.view->setSnapping(false);

    const auto extent = [&](ClipId id) {
        const auto loc = rig.controller.currentSequence()->findClip(id);
        REQUIRE(loc);
        return std::pair{loc->clip().start(), loc->clip().end()};
    };
    // The group's left edge is both clips' heads: dragging A's head trims both, one undo step.
    const std::size_t steps = rig.undoSteps();
    rig.drag({rig.x(0) + 1, rig.y("V1")}, {rig.x(2) + 1, rig.y("V1")});
    CHECK(extent(a) == std::pair{f(2), f(4)});
    CHECK(extent(c) == std::pair{f(2), f(6)});
    CHECK(rig.undoSteps() == steps + 1);
    CHECK(rig.controller.undoText() == QStringLiteral("Undo Trim"));
    rig.controller.undo();

    // The right edge of the group is C's tail: it trims C alone.
    rig.drag({rig.x(6) - 1, rig.y("V2")}, {rig.x(9) - 1, rig.y("V2")});
    CHECK(rig.picture() == normalize(R"(
        V2 |Ccccccccc.......|
        V1 |Aaaa............|
    )"));
    rig.controller.undo();

    // A's tail is inside the group: dragging it moves the whole group (Premiere trims only outer edges).
    rig.drag({rig.x(4) - 1, rig.y("V1")}, {rig.x(7) - 1, rig.y("V1")});
    CHECK(rig.picture() == normalize(R"(
        V2 |...Cccccc.......|
        V1 |...Aaaa.........|
    )"));
    rig.controller.undo();

    // Alt picks one clip: its inner edge trims, the group's other clip stays.
    rig.drag({rig.x(4) - 1, rig.y("V1")}, {rig.x(7) - 1, rig.y("V1")}, Qt::AltModifier);
    CHECK(rig.picture() == normalize(R"(
        V2 |Cccccc..........|
        V1 |Aaaaaaa.........|
    )"));
}

TEST_CASE("A ripple trim (Ctrl) of a group's outer edge ripples every member's track", "[app][timeline][groups][ripple]") {
    Rig rig(R"(
        V2     |Cccccc..Dd......|
        V1 tgt |Aaaa....Bb......|
    )",
            10.0, false);
    const ClipId a = rig.clip("V1@0");
    const ClipId c = rig.clip("V2@0");
    const ClipId b = rig.clip("V1@8");
    const ClipId d = rig.clip("V2@8");
    rig.controller.setSelection({a, c});
    REQUIRE(rig.controller.runCommand(QStringLiteral("clip.group")));
    rig.view->setSnapping(false);
    const auto extent = [&](ClipId id) {
        const auto loc = rig.controller.currentSequence()->findClip(id);
        REQUIRE(loc);
        return std::pair{loc->clip().start(), loc->clip().end()};
    };

    // Ctrl-dragging A's head (the group's left edge) two frames: both heads, both tracks close up, one undo step.
    const std::size_t steps = rig.undoSteps();
    rig.drag({rig.x(0) + 1, rig.y("V1")}, {rig.x(2) + 1, rig.y("V1")}, Qt::ControlModifier);
    CHECK(extent(a) == std::pair{f(0), f(2)});
    CHECK(extent(c) == std::pair{f(0), f(4)});
    CHECK(extent(b).first == f(6));
    CHECK(extent(d).first == f(6));
    CHECK(rig.undoSteps() == steps + 1);
    CHECK(rig.controller.undoText() == QStringLiteral("Undo Ripple Trim"));
    rig.controller.undo();

    // The Ripple Edit tool (B) on the group's right edge, C's tail: C grows, D and the sync-locked V1 after it move.
    rig.view->setTool(QStringLiteral("ripple"));
    rig.drag({rig.x(6) - 1, rig.y("V2")}, {rig.x(8) - 1, rig.y("V2")});
    CHECK(extent(c) == std::pair{f(0), f(8)});
    CHECK(extent(a) == std::pair{f(0), f(4)});
    CHECK(extent(d).first == f(10));
    CHECK(extent(b).first == f(10));
}

TEST_CASE("Select Next and Previous Clip walk the targeted tracks, not just the selected clip's",
          "[app][timeline][selection]") {
    Rig rig(R"(
        V2 tgt |..Cc..........|
        V1 tgt |Aaaa..Bbbb....|
        A1     |Aaaa..Bbbb....|
    )");
    rig.controller.selectClipById(rig.clip("V1@0"), ProjectController::Replace, false);
    // The next clip start on V1 or V2 after A's: C on V2.
    REQUIRE(rig.controller.runCommand(QStringLiteral("timeline.selectNextClip")));
    CHECK(rig.controller.primaryClip() == rig.clip("V2@2"));
    REQUIRE(rig.controller.runCommand(QStringLiteral("timeline.selectNextClip")));
    CHECK(rig.controller.primaryClip() == rig.clip("V1@6"));
    // With its link (Linked Selection): B's audio too, although A1 is not targeted.
    CHECK(rig.controller.isSelected(rig.clip("A1@6")));
    REQUIRE(rig.controller.runCommand(QStringLiteral("timeline.selectPreviousClip")));
    CHECK(rig.controller.primaryClip() == rig.clip("V2@2"));

    // Nothing selected: the clips under the playhead on the targeted tracks.
    rig.controller.clearSelection();
    rig.controller.setPlayhead(f(3));
    REQUIRE(rig.controller.runCommand(QStringLiteral("timeline.selectNextClip")));
    CHECK(rig.controller.isSelected(rig.clip("V2@2")));
    CHECK(rig.controller.isSelected(rig.clip("V1@0")));
}

TEST_CASE("Show Audio Time Units: the playhead and audio-only drags move by samples, video keeps frames",
          "[app][timeline][audioUnits]") {
    Rig rig(R"(
        V1 |Bbbbbbbb........|
        A1 |..Aaaaaaa.......|
    )",
            10.0, false);
    rig.view->setSnapping(false);
    const Rate audio = rig.controller.currentSequence()->audioRate;
    REQUIRE(audio == kRate48kHz);

    // Off: the playhead stays on frames.
    rig.controller.setPlayhead(f(1) + framesToTicks(100, audio));
    CHECK(rig.controller.playhead() == f(1));
    CHECK(rig.controller.finePlayhead() == f(1));
    CHECK(rig.controller.playheadText() == rig.controller.formatTimecode(f(1)));

    rig.controller.setAudioTimeUnits(true);
    rig.view->setAudioTimeUnits(true);
    rig.controller.setPlayhead(f(1) + framesToTicks(100, audio));
    CHECK(rig.controller.finePlayhead() == f(1) + framesToTicks(100, audio));
    CHECK(rig.controller.playhead() == f(1)); // the frame holding the sample: edits at the playhead land on frames
    CHECK(rig.controller.playheadText() == QStringLiteral("00:00:00:02020")); // 1920 + 100 samples
    rig.controller.stepFrames(1); // the arrow keys step one sample
    CHECK(rig.controller.finePlayhead() == f(1) + framesToTicks(101, audio));
    CHECK(rig.view->rulerLabel(1, 48000) == QStringLiteral("00:00:01:00000"));
    // The view zooms down to single samples.
    rig.view->setZoomLevel(1.0);
    CHECK(rig.view->pixelsPerSecond() >= 2.0 * 48000.0 - 1.0);
    rig.view->setPixelsPerSecond(250.0); // back to 10 px a frame
    rig.view->setScrollX(0);
    rig.harness.frame();

    // An audio-only drag of 1.5 frames moves 1.5 frames (2880 samples), not a whole frame.
    const ClipId a = rig.clip("A1@2");
    rig.drag({rig.x(5), rig.y("A1")}, {rig.x(6.5), rig.y("A1")});
    const auto audioClip = rig.controller.currentSequence()->findClip(a);
    REQUIRE(audioClip);
    CHECK(audioClip->clip().start() == f(2) + framesToTicks(2880, audio));
    // A video drag of 1.5 frames still moves whole frames.
    const ClipId b = rig.clip("V1@0");
    rig.drag({rig.x(4), rig.y("V1")}, {rig.x(5.5), rig.y("V1")});
    const auto videoClip = rig.controller.currentSequence()->findClip(b);
    REQUIRE(videoClip);
    CHECK(videoClip->clip().start() == f(1));

    // Off again: back on a frame.
    rig.controller.setAudioTimeUnits(false);
    CHECK(rig.controller.finePlayhead() == rig.controller.playhead());
}

TEST_CASE("A sequence's label colour: the tabs' Label menu, the Project panel and one undo step",
          "[app][timeline][label]") {
    Rig rig(R"(
        V1 |Aaaa|
    )");
    SequenceTabsBridge tabs;
    tabs.setController(&rig.controller);
    const QString key = tabs.current();
    REQUIRE_FALSE(key.isEmpty());
    CHECK(tabs.tabs().front().toMap().value(QStringLiteral("label")).toInt() == 0);
    const std::size_t steps = rig.undoSteps();
    REQUIRE(tabs.setLabel(key, 5));
    CHECK(rig.undoSteps() == steps + 1);
    CHECK(rig.controller.undoText() == QStringLiteral("Undo Label"));
    CHECK(rig.controller.currentSequence()->colorIndex == 5);
    CHECK(tabs.tabs().front().toMap().value(QStringLiteral("label")).toInt() == 5);
    CHECK_FALSE(tabs.setLabel(key, 16));
    CHECK_FALSE(tabs.setLabel(QStringLiteral("s999999"), 2));
    rig.controller.undo();
    CHECK(rig.controller.currentSequence()->colorIndex == 0);
}

TEST_CASE("The ruler draws the render bar; the display settings switch fade handles, sequence labels and audio units",
          "[app][timeline][renderbar][label][audioUnits][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    REQUIRE_FALSE(shell.grab().isNull());
    auto* view = qobject_cast<TimelineView*>(shell.item("timelineView"));
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(view);
    REQUIRE(controller);
    CHECK(view->readiness() != nullptr); // the preview cache: rendered segments are green

    // A blur on the first video clip: a yellow segment in the ruler, in the warning colour.
    const SequencePtr seq = controller->currentSequence();
    REQUIRE(seq);
    ClipId first;
    for (const TrackPtr& track : seq->videoTracks) {
        if (!track->clips.empty() && !first)
            first = track->clips.front().id;
    }
    REQUIRE(first);
    REQUIRE(controller->applyEditFn("Add Effect", [&](const ProjectState& s) { return addBlur(s, seq->id, first); }));
    view->refreshRenderBar();
    REQUIRE_FALSE(view->renderBar().empty());
    QQuickItem* ruler = shell.item("timelineRuler");
    REQUIRE(ruler);
    REQUIRE(waitFor([&] { return itemNamed(ruler, QStringLiteral("renderBarSegment_yellow")) != nullptr; }, 5000));
    QQuickItem* segment = itemNamed(ruler, QStringLiteral("renderBarSegment_yellow"));
    CHECK(segment->property("color").value<QColor>() == shell.themeColor("renderYellow"));
    CHECK(segment->height() == shell.theme->property("renderBarHeight").toDouble());

    // Show Video / Audio Fade Handles: Preferences, and the commands toggle them.
    CHECK(view->showVideoFadeHandles());
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("timeline.showVideoFadeHandles"))));
    CHECK_FALSE(view->showVideoFadeHandles());
    CHECK_FALSE(shell.preferences->property("showVideoFadeHandles").toBool());
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("timeline.showAudioFadeHandles"))));
    CHECK_FALSE(view->showAudioFadeHandles());
    shell.preferences->setProperty("showVideoFadeHandles", true);
    shell.preferences->setProperty("showAudioFadeHandles", true);
    CHECK(view->showVideoFadeHandles());

    // Show Sequence Label Color: a swatch of the label on the tab (off by default).
    auto* tabs = shell.engine.singletonInstance<SequenceTabsBridge*>("Wabcut", "SequenceTabs");
    REQUIRE(tabs);
    REQUIRE(tabs->setLabel(tabs->current(), 3));
    QQuickItem* tabBar = shell.item("sequenceTabs");
    REQUIRE(tabBar);
    const QString swatchName = QStringLiteral("sequenceTabLabel_") + tabs->current();
    REQUIRE(waitFor([&] { return itemNamed(tabBar, swatchName) != nullptr; }, 5000));
    CHECK_FALSE(itemNamed(tabBar, swatchName)->isVisible());
    shell.preferences->setProperty("showSequenceLabelColor", true);
    REQUIRE(waitFor([&] { return itemNamed(tabBar, swatchName)->isVisible(); }, 5000));
    QVariant label;
    REQUIRE(QMetaObject::invokeMethod(shell.theme, "clipColor", Q_RETURN_ARG(QVariant, label), Q_ARG(QVariant, 3)));
    CHECK(itemNamed(tabBar, swatchName)->property("color").value<QColor>() == label.value<QColor>());

    // Show Audio Time Units, from the timeline's display settings: one setting with the Program Monitor's.
    QObject* monitors = shell.engine.singletonInstance<QObject*>("Wabcut", "Monitors");
    REQUIRE(monitors);
    REQUIRE(monitors->setProperty("audioTimeUnits", true));
    CHECK(controller->audioTimeUnits());
    CHECK(view->audioTimeUnits());
    CHECK(view->rulerLabel(1, 48000) == QStringLiteral("00:00:01:00000"));
    REQUIRE(monitors->setProperty("audioTimeUnits", false));
    CHECK_FALSE(view->audioTimeUnits());

    const QStringList messages = takeMessages();
    INFO(messages.join(QLatin1Char('\n')).toStdString());
    CHECK(messages.isEmpty());
}
