// M5b: interactive editing on the timeline. Mouse gestures are driven with real mouse events through an offscreen
// window (TimelineHarness.h) and the resulting model timeline is compared with the model tests' ASCII pictures
// (tests/model/TimelineFixture.h: one character per frame at 25 fps). Every gesture must:
//   * show a ghost preview without touching the model until release,
//   * commit exactly one undo step that one undo reverts,
//   * match the EditOps result for the same parameters.

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "DemoProject.h"
#include "ProjectController.h"
#include "TimelineEditing.h"
#include "TimelineFixture.h"
#include "TimelineView.h"

#include <catch2/catch_test_macros.hpp>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectOps.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMouseEvent>

#include <algorithm>
#include <array>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

// ---- Snapping math ------------------------------------------------------------------------------------------------

TEST_CASE("Snapping moves the edge closest to a target onto it", "[app][timeline][edit]") {
    const std::vector<Ticks> targets{Ticks{0}, Ticks{150}, Ticks{240}};
    const std::vector<Ticks> edges{Ticks{100}, Ticks{200}};
    SECTION("the nearest of several edges wins") {
        // 100+30 = 130 is 20 from 150; 200+30 = 230 is 10 from 240: the tail snaps.
        const SnapResult r = snapDelta(edges, Ticks{30}, targets, Ticks{25});
        REQUIRE(r.target);
        CHECK(*r.target == Ticks{240});
        CHECK(r.delta == Ticks{40});
    }
    SECTION("out of reach: the delta is unchanged") {
        const SnapResult r = snapDelta(edges, Ticks{70}, targets, Ticks{5});
        CHECK_FALSE(r.target);
        CHECK(r.delta == Ticks{70});
    }
    SECTION("the tolerance is inclusive") {
        const SnapResult r = snapDelta(edges, Ticks{45}, targets, Ticks{5});
        REQUIRE(r.target);
        CHECK(*r.target == Ticks{150});
        CHECK(r.delta == Ticks{50});
    }
    SECTION("no edges, no targets") {
        CHECK_FALSE(snapDelta({}, Ticks{7}, targets, Ticks{100}).target);
        CHECK_FALSE(snapDelta(edges, Ticks{7}, {}, Ticks{100}).target);
    }
}

// ---- Selection tool: move ------------------------------------------------------------------------------------------

TEST_CASE("Dragging a clip previews the move and commits it as one undo step", "[app][timeline][edit]") {
    Rig rig(R"(
        V1 |AaaaBbbb....|
        A1 |AaaaBbbb....|
    )");
    const auto before = rig.controller.snapshot();
    const ClipId b = rig.clip("V1@4");

    rig.drag({rig.x(5.5), rig.y("V1")}, {rig.x(8.5), rig.y("V1")}, {}, false);
    // Ghost preview: the view shows B (and its linked audio) at its new place, the model has not changed.
    CHECK(rig.view->gestureActive());
    CHECK(rig.controller.snapshot() == before);
    CHECK(rig.undoSteps() == 0);
    CHECK(rig.view->clipAt(rig.x(9.5), rig.y("V1")) == clipKey(b));
    CHECK(rig.view->clipAt(rig.x(9.5), rig.y("A1")) == clipKey(rig.clip("A1@4")));
    CHECK(rig.view->gestureReadout() == QStringLiteral("+00:00:00:03"));

    rig.release({rig.x(8.5), rig.y("V1")});
    CHECK_FALSE(rig.view->gestureActive());
    CHECK(rig.picture() == normalize(R"(
        V1 |Aaaa...Bbbb.|
        A1 |Aaaa...Bbbb.|
    )"));
    CHECK(rig.undoSteps() == 1);
    CHECK(rig.controller.undoText() == QStringLiteral("Undo Move"));
    CHECK(clipAt(*rig.controller.snapshot(), rig.tl.seq, "V1@7") == b); // same clip, new place

    REQUIRE(rig.controller.undo());
    CHECK(rig.picture() == normalize(rig.original));
}

TEST_CASE("Move variants: overwrite, Ctrl insert, Alt ignores links, across tracks", "[app][timeline][edit]") {
    SECTION("dropping onto another clip overwrites it") {
        Rig rig(R"(
            V1 |AaaaaaBb..|
        )");
        const ClipId b = rig.clip("V1@6");
        rig.drag({rig.x(7), rig.y("V1")}, {rig.x(3), rig.y("V1")}); // the middle of a 2-frame clip (its edges trim)
        const ClipId sel[] = {b};
        CHECK(rig.picture() == rig.expected(edit::moveClips(rig.tl.state, rig.tl.seq, sel, f(-4))));
        CHECK(rig.picture() == normalize("V1 |AaBbA4....|"));
    }
    SECTION("Ctrl-drag inserts") {
        Rig rig(R"(
            V1 |AaaaBbbbCccc|
        )");
        const ClipId c = rig.clip("V1@8");
        rig.drag({rig.x(9.5), rig.y("V1")}, {rig.x(5.5), rig.y("V1")}, Qt::ControlModifier);
        const ClipId sel[] = {c};
        CHECK(rig.picture()
              == rig.expected(edit::moveClips(rig.tl.state, rig.tl.seq, sel, f(-4), {}, edit::MoveMode::Insert)));
        CHECK(rig.controller.undoText() == QStringLiteral("Undo Insert Move"));
    }
    SECTION("Alt-drag moves only the clip, not its linked partner") {
        Rig rig(R"(
            V1 |AaaaBbbb....|
            A1 |AaaaBbbb....|
        )");
        rig.drag({rig.x(5.5), rig.y("V1")}, {rig.x(8.5), rig.y("V1")}, Qt::AltModifier);
        CHECK(rig.picture() == normalize(R"(
            V1 |Aaaa...Bbbb.|
            A1 |AaaaBbbb....|
        )"));
    }
    SECTION("dragging up a track moves video up and its linked audio down") {
        Rig rig(R"(
            V2 |........|
            V1 |AaaBbb..|
            A1 |AaaBbb..|
            A2 |........|
        )");
        rig.drag({rig.x(4.5), rig.y("V1")}, {rig.x(4.5), rig.y("V2")});
        CHECK(rig.picture() == normalize(R"(
            V2 |...Bbb..|
            V1 |Aaa.....|
            A1 |Aaa.....|
            A2 |...Bbb..|
        )"));
    }
}

TEST_CASE("Linked clips cross tracks with the dragged one only onto tracks free where they land",
          "[app][timeline][edit]") {
    // 2026-10-15: dragging linked audio from A2 to A1 moved its picture from V3 onto the picture on V2.
    SECTION("the partner's track is taken: the picture stays on V3") {
        Rig rig(R"(
            V3 |Bbbb....|
            V2 |Cccc....|
            V1 |........|
            A1 |........|
            A2 |Bbbb....|
        )");
        rig.drag({rig.x(1.5), rig.y("A2")}, {rig.x(1.5), rig.y("A1")});
        CHECK(rig.picture() == normalize(R"(
            V3 |Bbbb....|
            V2 |Cccc....|
            V1 |........|
            A1 |Bbbb....|
            A2 |........|
        )"));
        CHECK(rig.undoSteps() == 1);
    }
    SECTION("two tracks over, the free one: the picture goes to V1") {
        Rig rig(R"(
            V3 |Bbbb....|
            V2 |Cccc....|
            V1 |........|
            A1 |........|
            A2 |........|
            A3 |Bbbb....|
        )");
        rig.view->minimizeAllTracks(); // six tracks fit the view: no vertical auto-scroll during the drag
        rig.harness.frame();
        rig.drag({rig.x(1.5), rig.y("A3")}, {rig.x(1.5), rig.y("A1")});
        CHECK(rig.picture() == normalize(R"(
            V3 |........|
            V2 |Cccc....|
            V1 |Bbbb....|
            A1 |Bbbb....|
            A2 |........|
            A3 |........|
        )"));
    }
    SECTION("free where it lands later on: the picture goes along") {
        Rig rig(R"(
            V3 |Bbbb........|
            V2 |Cccc........|
            V1 |............|
            A1 |............|
            A2 |Bbbb........|
        )");
        rig.drag({rig.x(1.5), rig.y("A2")}, {rig.x(7.5), rig.y("A1")});
        CHECK(rig.picture() == normalize(R"(
            V3 |............|
            V2 |Cccc..Bbbb..|
            V1 |............|
            A1 |......Bbbb..|
            A2 |............|
        )"));
    }
    SECTION("onto another A/V clip's sound: both its halves are replaced, as a drop does") {
        Rig rig(R"(
            V2 |Bbbb....|
            V1 |Dddd....|
            A1 |Dddd....|
            A2 |Bbbb....|
        )");
        rig.drag({rig.x(1.5), rig.y("A2")}, {rig.x(1.5), rig.y("A1")});
        CHECK(rig.picture() == normalize(R"(
            V2 |........|
            V1 |Bbbb....|
            A1 |Bbbb....|
            A2 |........|
        )"));
    }
    SECTION("the dragged picture still overwrites where it is dropped") {
        Rig rig(R"(
            V2 |Cccc....|
            V1 |Bbbb....|
            A1 |Bbbb....|
            A2 |Eeee....|
        )");
        rig.drag({rig.x(1.5), rig.y("V1")}, {rig.x(1.5), rig.y("V2")});
        CHECK(rig.picture() == normalize(R"(
            V2 |Bbbb....|
            V1 |........|
            A1 |Bbbb....|
            A2 |Eeee....|
        )"));
    }
}

// ---- Trims, ripple, roll, slip, slide -------------------------------------------------------------------------------

TEST_CASE("Edge drags trim; Ctrl and the B tool ripple-trim; N rolls", "[app][timeline][edit]") {
    SECTION("selection tool on an edge: regular trim (linked audio follows)") {
        Rig rig(R"(
            V1 |AaaaBbbb....|
            A1 |AaaaBbbb....|
        )");
        rig.drag({rig.x(8) - 2, rig.y("V1")}, {rig.x(6) - 2, rig.y("V1")});
        CHECK(rig.picture() == normalize(R"(
            V1 |AaaaBb......|
            A1 |AaaaBb......|
        )"));
        CHECK(rig.controller.undoText() == QStringLiteral("Undo Trim"));
        REQUIRE(rig.controller.undo());
        CHECK(rig.picture() == normalize(rig.original));
    }
    SECTION("Ctrl on an edge: ripple trim") {
        Rig rig(R"(
            V1 |AaaaBbbbCc..|
        )");
        const ClipId b = rig.clip("V1@4");
        rig.drag({rig.x(8) - 2, rig.y("V1")}, {rig.x(6) - 2, rig.y("V1")}, Qt::ControlModifier);
        CHECK(rig.picture() == rig.expected(edit::rippleTrim(rig.tl.state, rig.tl.seq, b, edit::Edge::Tail, f(-2))));
        CHECK(rig.picture() == normalize("V1 |AaaaBbCc....|"));
    }
    SECTION("B tool: ripple trim from the head") {
        Rig rig(R"(
            V1 |AaaaBbbbCc..|
        )");
        rig.view->setTool(QStringLiteral("ripple"));
        const ClipId b = rig.clip("V1@4");
        rig.drag({rig.x(4) + 2, rig.y("V1")}, {rig.x(6) + 2, rig.y("V1")});
        CHECK(rig.picture() == rig.expected(edit::rippleTrim(rig.tl.state, rig.tl.seq, b, edit::Edge::Head, f(2))));
        CHECK(rig.controller.undoText() == QStringLiteral("Undo Ripple Trim"));
    }
    SECTION("N tool: rolling edit between two clips") {
        Rig rig(R"(
            V1 |AaaaBbbb....|
        )");
        rig.view->setTool(QStringLiteral("roll"));
        const ClipId a = rig.clip("V1@0");
        rig.drag({rig.x(4) - 2, rig.y("V1")}, {rig.x(6) - 2, rig.y("V1")});
        CHECK(rig.picture() == rig.expected(edit::rollEdit(rig.tl.state, rig.tl.seq, a, edit::Edge::Tail, f(2))));
        CHECK(rig.picture() == normalize("V1 |AaaaaaB2....|"));
        CHECK(rig.controller.undoText() == QStringLiteral("Undo Rolling Edit"));
    }
}

TEST_CASE("Y slips the source with an In/Out readout; U slides the clip", "[app][timeline][edit]") {
    SECTION("slip: dragging left shows later source frames") {
        Rig rig(R"(
            V1 |AaaaBbbb....|
        )");
        rig.view->setTool(QStringLiteral("slip"));
        const ClipId b = rig.clip("V1@4");
        rig.drag({rig.x(6), rig.y("V1")}, {rig.x(3), rig.y("V1")}, {}, false);
        CHECK(rig.view->gestureReadout() == QStringLiteral("In 00:00:00:03 · Out 00:00:00:06"));
        rig.release({rig.x(3), rig.y("V1")});
        const ClipId sel[] = {b};
        CHECK(rig.picture() == rig.expected(edit::slip(rig.tl.state, rig.tl.seq, sel, f(3))));
        CHECK(rig.picture() == normalize("V1 |AaaaB3bb....|"));
    }
    SECTION("slide: neighbours give and take") {
        Rig rig(R"(
            V1 |AaaaBbbbCccc|
        )");
        rig.view->setTool(QStringLiteral("slide"));
        const ClipId b = rig.clip("V1@4");
        rig.drag({rig.x(6), rig.y("V1")}, {rig.x(7), rig.y("V1")});
        CHECK(rig.picture() == rig.expected(edit::slide(rig.tl.state, rig.tl.seq, b, f(1))));
        CHECK(rig.controller.undoText() == QStringLiteral("Undo Slide"));
    }
}

// ---- Razor and track select ---------------------------------------------------------------------------------------

TEST_CASE("C cuts the clip under the cursor; Shift cuts every track", "[app][timeline][edit]") {
    Rig rig(R"(
        V2 |..Ccccc.|
        V1 |AaaaBbbb|
        A1 |AaaaBbbb|
    )");
    rig.view->setTool(QStringLiteral("razor"));
    SECTION("one track (and its linked partner)") {
        rig.press({rig.x(2) + 1, rig.y("V1")});
        rig.release({rig.x(2) + 1, rig.y("V1")});
        const TrackId v1[] = {rig.tl.track("V1")};
        CHECK(rig.picture()
              == rig.expected(edit::addEdit(rig.tl.state, rig.tl.seq, f(2), v1, edit::LinkMode::FollowLinks)));
        CHECK(rig.picture() == normalize(R"(
            V2 |..Ccccc.|
            V1 |AaA2Bbbb|
            A1 |AaA2Bbbb|
        )"));
        CHECK(rig.undoSteps() == 1);
    }
    SECTION("Shift: all tracks") {
        rig.press({rig.x(3) + 1, rig.y("V1")}, Qt::ShiftModifier);
        rig.release({rig.x(3) + 1, rig.y("V1")}, Qt::ShiftModifier);
        CHECK(rig.picture() == rig.expected(edit::addEditToAllTracks(rig.tl.state, rig.tl.seq, f(3))));
    }
}

TEST_CASE("A selects every clip from the click on", "[app][timeline][edit]") {
    Rig rig(R"(
        V2 |Cc....Dd|
        V1 |AaaaBbbb|
        A1 |AaaaBbbb|
    )");
    rig.view->setTool(QStringLiteral("trackForward"));
    rig.press({rig.x(5), rig.y("V1")});
    rig.release({rig.x(5), rig.y("V1")});
    std::vector<ClipId> selected = rig.controller.selection();
    std::vector<ClipId> expected{rig.clip("V1@4"), rig.clip("A1@4"), rig.clip("V2@6")};
    std::ranges::sort(selected);
    std::ranges::sort(expected);
    CHECK(selected == expected);

    // Shift: only the clicked track (plus links).
    rig.press({rig.x(1), rig.y("V2")}, Qt::ShiftModifier);
    rig.release({rig.x(1), rig.y("V2")}, Qt::ShiftModifier);
    selected = rig.controller.selection();
    std::ranges::sort(selected);
    expected = {rig.clip("V2@0"), rig.clip("V2@6")};
    std::ranges::sort(expected);
    CHECK(selected == expected);
}

// ---- Snapping, Esc, locked tracks -----------------------------------------------------------------------------------

TEST_CASE("Drags snap to clip edges within the pixel threshold", "[app][timeline][edit]") {
    const char* picture = R"(
        V1 |Aaaa......Bbbb|
    )";
    SECTION("snapping on: B's head lands on A's tail and the snap line shows") {
        Rig rig(picture, 2.0); // 2 px per frame: 8 px reach 4 frames
        rig.drag({rig.x(12), rig.y("V1")}, {rig.x(7), rig.y("V1")}, {}, false);
        CHECK_FALSE(std::isnan(rig.view->snapX()));
        CHECK(rig.view->snapX() == rig.x(4));
        rig.release({rig.x(7), rig.y("V1")});
        CHECK(rig.picture() == normalize("V1 |AaaaBbbb......|"));
    }
    SECTION("snapping off: B stops where the pointer is") {
        Rig rig(picture, 2.0);
        rig.view->setSnapping(false);
        rig.drag({rig.x(12), rig.y("V1")}, {rig.x(7), rig.y("V1")});
        CHECK(rig.picture() == normalize("V1 |Aaaa.Bbbb.....|"));
    }
}

TEST_CASE("Esc cancels a drag and nothing is committed", "[app][timeline][edit]") {
    Rig rig(R"(
        V1 |AaaaBbbb....|
    )");
    const auto before = rig.controller.snapshot();
    rig.drag({rig.x(5.5), rig.y("V1")}, {rig.x(9.5), rig.y("V1")}, {}, false);
    REQUIRE(rig.view->gestureActive());
    rig.key(Qt::Key_Escape);
    CHECK_FALSE(rig.view->gestureActive());
    rig.move({rig.x(10.5), rig.y("V1")});
    rig.release({rig.x(10.5), rig.y("V1")});
    CHECK(rig.controller.snapshot() == before);
    CHECK(rig.undoSteps() == 0);
    CHECK(rig.view->clipAt(rig.x(5), rig.y("V1")) == clipKey(rig.clip("V1@4"))); // shown back in place
}

TEST_CASE("Locked tracks reject gestures and commands", "[app][timeline][edit]") {
    Rig rig(R"(
        V2      |....Cccc|
        V1 lock |AaaaBbbb|
    )");
    const auto before = rig.controller.snapshot();
    // Move and trim on the locked track: no preview, nothing committed.
    rig.drag({rig.x(5.5), rig.y("V1")}, {rig.x(6.5), rig.y("V1")});
    rig.drag({rig.x(8) - 1, rig.y("V1")}, {rig.x(6) - 1, rig.y("V1")});
    CHECK(rig.controller.snapshot() == before);
    // Moving C down onto the locked track stays on V2 (the preview keeps the last valid position).
    rig.drag({rig.x(5.5), rig.y("V2")}, {rig.x(5.5), rig.y("V1")});
    CHECK(rig.picture() == normalize(rig.original));
    // Delete on a locked clip: an error, no change.
    rig.controller.selectClipById(rig.clip("V1@0"), ProjectController::Replace);
    rig.controller.deleteSelection();
    CHECK_FALSE(rig.controller.lastError().isEmpty());
    CHECK(rig.picture() == normalize(rig.original));
    CHECK(rig.undoSteps() == 0);
}

TEST_CASE("Right-clicking a clip selects it and asks for the context menu", "[app][timeline][edit]") {
    Rig rig(R"(
        V1 |AaaaBbbb|
        A1 |AaaaBbbb|
    )");
    QString menuClip;
    QObject::connect(rig.view, &TimelineView::contextMenuRequested,
                     [&](const QString& id, double, double) { menuClip = id; });
    rig.press({rig.x(5), rig.y("V1")}, {}, Qt::RightButton);
    rig.send(QEvent::MouseButtonRelease, {rig.x(5), rig.y("V1")}, Qt::RightButton, Qt::NoButton, {});
    CHECK(menuClip == clipKey(rig.clip("V1@4")));
    CHECK(rig.controller.selection().size() == 2); // with its linked audio
}

// ---- Selection-based edit commands -----------------------------------------------------------------------------------

TEST_CASE("Edit commands act on the selection, the playhead and the targeted tracks", "[app][timeline][edit]") {
    Rig rig(R"(
        V1 tgt |AaaaBbbbCccc....|
        A1 tgt |AaaaBbbbCccc....|
    )");
    const ClipId b = rig.clip("V1@4");
    const auto select = [&](ClipId id) { rig.controller.selectClipById(id, ProjectController::Replace); };
    const auto at = [&](int frame) { rig.controller.setPlayhead(f(frame)); };

    SECTION("Delete and Ripple Delete") {
        select(b);
        REQUIRE(rig.controller.runCommand(QStringLiteral("edit.clear")));
        CHECK(rig.picture() == normalize(R"(
            V1 |Aaaa....Cccc....|
            A1 |Aaaa....Cccc....|
        )"));
        rig.controller.undo();
        select(b);
        REQUIRE(rig.controller.runCommand(QStringLiteral("edit.rippleDelete")));
        CHECK(rig.picture() == normalize(R"(
            V1 |AaaaCccc........|
            A1 |AaaaCccc........|
        )"));
    }
    SECTION("Add Edit at the playhead on the targeted tracks, Q and W") {
        at(6);
        REQUIRE(rig.controller.runCommand(QStringLiteral("sequence.addEdit")));
        CHECK(rig.picture() == normalize(R"(
            V1 |AaaaBbB2Cccc....|
            A1 |AaaaBbB2Cccc....|
        )"));
        rig.controller.undo();
        at(6);
        REQUIRE(rig.controller.runCommand(QStringLiteral("sequence.rippleTrimPreviousToPlayhead")));
        CHECK(rig.picture() == rig.expected(edit::rippleTrimPreviousEditToPlayhead(rig.tl.state, rig.tl.seq, f(6))));
        CHECK(rig.controller.playheadText().endsWith(QStringLiteral(":04"))); // parked on the edit point
        rig.controller.undo();
        at(6);
        REQUIRE(rig.controller.runCommand(QStringLiteral("sequence.rippleTrimNextToPlayhead")));
        CHECK(rig.picture() == rig.expected(edit::rippleTrimNextEditToPlayhead(rig.tl.state, rig.tl.seq, f(6))));
    }
    SECTION("E extends the selected clip's nearest edge to the playhead; D selects at the playhead") {
        select(rig.clip("V1@8"));
        at(14);
        REQUIRE(rig.controller.runCommand(QStringLiteral("sequence.extendEditToPlayhead")));
        CHECK(rig.picture() == normalize(R"(
            V1 |AaaaBbbbCccccc..|
            A1 |AaaaBbbbCccccc..|
        )"));
        at(5);
        REQUIRE(rig.controller.runCommand(QStringLiteral("sequence.selectClipAtPlayhead")));
        std::vector<ClipId> selected = rig.controller.selection();
        std::ranges::sort(selected);
        std::vector<ClipId> expected{b, rig.clip("A1@4")};
        std::ranges::sort(expected);
        CHECK(selected == expected);
    }
    SECTION("link toggling, enable toggling and nudging") {
        select(b);
        REQUIRE(rig.controller.selectionLinked());
        REQUIRE(rig.controller.runCommand(QStringLiteral("clip.link"))); // linked -> unlink
        CHECK_FALSE(rig.controller.selectionLinked());
        REQUIRE(rig.controller.runCommand(QStringLiteral("clip.link"))); // relink the two
        CHECK(rig.controller.selectionLinked());
        REQUIRE(rig.controller.runCommand(QStringLiteral("clip.enable")));
        CHECK_FALSE(rig.controller.selectionEnabled());
        CHECK(rig.controller.undoText() == QStringLiteral("Undo Disable"));
        rig.controller.undo();
        select(rig.clip("V1@8"));
        REQUIRE(rig.controller.runCommand(QStringLiteral("timeline.nudgeClipRight")));
        CHECK(rig.picture() == normalize(R"(
            V1 |AaaaBbbb.Cccc...|
            A1 |AaaaBbbb.Cccc...|
        )"));
    }
    SECTION("Copy and Paste (overwrite) and Paste Insert at the playhead") {
        select(b);
        REQUIRE(rig.controller.runCommand(QStringLiteral("edit.copy")));
        CHECK(rig.controller.canPaste());
        at(13);
        REQUIRE(rig.controller.runCommand(QStringLiteral("edit.paste")));
        CHECK(rig.picture() == normalize(R"(
            V1 |AaaaBbbbCccc.Bbbb|
            A1 |AaaaBbbbCccc.Bbbb|
        )"));
        CHECK(links(*rig.controller.snapshot(), rig.tl.seq) == "V1@0+A1@0 V1@13+A1@13 V1@4+A1@4 V1@8+A1@8");
        CHECK(rig.controller.playheadText().endsWith(QStringLiteral(":17"))); // after the pasted clips
        rig.controller.undo();
        at(2);
        REQUIRE(rig.controller.runCommand(QStringLiteral("edit.pasteInsert")));
        CHECK(rig.picture() == normalize(R"(
            V1 |AaBbbbA2BbbbCccc|
            A1 |AaBbbbA2BbbbCccc|
        )"));
        CHECK(rig.undoSteps() == 1);
    }
}

// ---- Performance ----------------------------------------------------------------------------------------------------

TEST_CASE("A drag preview on 10,000 clips stays within the frame budget", "[app][timeline][perf]") {
    ProjectController controller;
    DemoProject stress = buildStressProject({3, 3, 10'000, 10});
    controller.resetProject(std::move(stress.state), stress.mainSequence);
    const QSize size(1400, 400);
    RenderHarness harness(size);
    REQUIRE(harness.initialize());
    TimelineView* view = addView(harness, controller, size);
    harness.frame();

    const auto seq = controller.currentSequence();
    const Clip& grabbed = seq->videoTracks[0]->clips[12];
    const double x0 = (grabbed.start().toSeconds() + grabbed.end().toSeconds()) / 2 * view->pixelsPerSecond();
    const auto& rows = view->rows();
    const auto v1 = std::ranges::find_if(rows, [&](const TimelineView::Row& r) { return r.track->id == seq->videoTracks[0]->id; });
    REQUIRE(v1 != rows.end());
    const double y = v1->y + v1->height / 2;
    const auto send = [&](QEvent::Type type, double x, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, QPointF(x, y), QPointF(x, y), button, buttons, {});
        QCoreApplication::sendEvent(&harness.window(), &event);
    };

    send(QEvent::MouseButtonPress, x0, Qt::LeftButton, Qt::LeftButton);
    harness.frame();
    std::vector<double> preview, paint, total;
    QElapsedTimer clock;
    for (int i = 1; i <= 40; ++i) {
        clock.start();
        send(QEvent::MouseMove, x0 + 7.0 * i, Qt::NoButton, Qt::LeftButton);
        const double move = static_cast<double>(clock.nsecsElapsed()) / 1e6;
        harness.frame();
        if (i <= 2)
            continue; // the gesture starts (snap targets of 10,000 clips) and the glyph cache warms
        preview.push_back(static_cast<double>(view->stats().previewNs) / 1e6);
        paint.push_back(static_cast<double>(view->stats().updateNs) / 1e6);
        total.push_back(move + paint.back());
    }
    REQUIRE(view->gestureActive());
    const auto before = controller.snapshot();
    CHECK(controller.snapshot() == before);
    send(QEvent::MouseButtonRelease, x0 + 280, Qt::LeftButton, Qt::NoButton);
    harness.frame();
    CHECK(controller.session().undoCount() == 1);

    const auto mean = [](const std::vector<double>& v) { return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size()); };
    const auto maxOf = [](const std::vector<double>& v) { return *std::ranges::max_element(v); };
    std::vector<double> sorted = total;
    std::ranges::sort(sorted);
    const double p95 = sorted[static_cast<std::size_t>(0.95 * static_cast<double>(sorted.size() - 1))];
    INFO("drag frame (event + preview edit + updatePaintNode): mean " << mean(total) << " ms, p95 " << p95 << " ms");
    CHECK(mean(total) < 4.0);
    CHECK(p95 < 4.0);
    writeOutputFile(QStringLiteral("timeline-drag-perf.txt"),
                    QStringLiteral("10,000-clip sequence, dragging one linked clip (38 frames)\n"
                                   "Drag frame total (mouse move incl. preview edit + updatePaintNode): mean %1 ms, p95 %2 ms, max %3 ms\n"
                                   "Preview edit (moveClips on the snapshot): mean %4 ms, max %5 ms\n"
                                   "updatePaintNode: mean %6 ms, max %7 ms\n")
                        .arg(mean(total), 0, 'f', 3)
                        .arg(p95, 0, 'f', 3)
                        .arg(maxOf(total), 0, 'f', 3)
                        .arg(mean(preview), 0, 'f', 3)
                        .arg(maxOf(preview), 0, 'f', 3)
                        .arg(mean(paint), 0, 'f', 3)
                        .arg(maxOf(paint), 0, 'f', 3));
}

// ---- Screenshot sequence of a drag (lift, ghost, snap line, drop) ---------------------------------------------------

TEST_CASE("Drag screenshots: lift, ghost preview, snap line, drop", "[app][qml][screenshots][dragshots]") {
    // The demo's Lower Third (V3, 8 s) is dragged right until its head snaps to the playhead (12:08). Every image
    // comes from a fresh window that replays the drag (an offscreen grab tears the scene graph down).
    struct Step {
        const char* file;
        double dx;
        bool release;
    };
    const Step steps[] = {
        {"drag-1-lift.png", 24, false},
        {"drag-2-ghost.png", 120, false},
        {"drag-3-snap.png", 0, false}, // dx computed: just short of the playhead
        {"drag-4-drop.png", 0, true},
    };
    for (const Step& step : steps) {
        resetSettings();
        Shell shell;
        REQUIRE(shell.window != nullptr);
        shell.resize(kShotWidth, kShotHeight);
        auto* view = qobject_cast<TimelineView*>(shell.object("timelineView"));
        REQUIRE(view != nullptr);
        auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        REQUIRE(controller != nullptr);
        spin(400); // layout and theme settle

        const SequencePtr seq = controller->currentSequence();
        const Clip& lowerThird = seq->videoTracks[2]->clips.front();
        const auto& rows = view->rows();
        const double y = rows[0].y + rows[0].height / 2 - view->scrollY(); // V3 is the top row
        const double startX = (lowerThird.start().toSeconds() + 1.0) * view->pixelsPerSecond() - view->scrollX();
        const double headX = lowerThird.start().toSeconds() * view->pixelsPerSecond() - view->scrollX();
        const double snapDx = view->playheadX() - headX - 4; // 4 px short: the snap pulls it onto the playhead
        const double dx = step.dx > 0 ? step.dx : snapDx;
        const auto scene = [&](double x) { return view->mapToScene(QPointF(x, y)); };
        const auto send = [&](QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons) {
            QMouseEvent event(type, pos, pos, button, buttons, {});
            QCoreApplication::sendEvent(shell.window.get(), &event);
        };
        send(QEvent::MouseButtonPress, scene(startX), Qt::LeftButton, Qt::LeftButton);
        for (int i = 1; i <= 8; ++i)
            send(QEvent::MouseMove, scene(startX + dx * i / 8.0), Qt::NoButton, Qt::LeftButton);
        if (step.release) {
            send(QEvent::MouseButtonRelease, scene(startX + dx), Qt::LeftButton, Qt::NoButton);
            spin(600); // the drop spring settles
            CHECK_FALSE(view->gestureActive());
            CHECK(controller->undoText() == QStringLiteral("Undo Move"));
        } else {
            REQUIRE(view->gestureActive());
            if (step.dx == 0)
                CHECK_FALSE(std::isnan(view->snapX())); // snapped to the playhead
        }
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        REQUIRE(image.save(outputDir() + QLatin1Char('/') + QLatin1String(step.file)));
    }
}

TEST_CASE("Right-clicking a clip in the shell opens the clip context menu", "[app][qml][edit]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    auto* view = qobject_cast<TimelineView*>(shell.object("timelineView"));
    REQUIRE(view != nullptr);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    QObject* menu = shell.object("clipContextMenu");
    REQUIRE(menu != nullptr);
    spin(200);

    // Interview A on V1 (third row): right-click it.
    const auto& rows = view->rows();
    REQUIRE(rows.size() >= 3);
    const QPointF local(view->pixelsPerSecond() * 2.0, rows[2].y + rows[2].height / 2 - view->scrollY());
    const QPointF scene = view->mapToScene(local);
    QMouseEvent press(QEvent::MouseButtonPress, scene, scene, Qt::RightButton, Qt::RightButton, {});
    QCoreApplication::sendEvent(shell.window.get(), &press);
    QMouseEvent release(QEvent::MouseButtonRelease, scene, scene, Qt::RightButton, Qt::NoButton, {});
    QCoreApplication::sendEvent(shell.window.get(), &release);
    REQUIRE(waitFor([&] { return menu->property("opened").toBool(); }));
    CHECK(menu->property("clipId").toString() == view->clipAt(local.x(), local.y()));
    CHECK(controller->selectedClip().value(QStringLiteral("name")).toString() == QStringLiteral("Interview A"));

    // Label colour from the menu: one undo step on the selection (video and its linked audio).
    QMetaObject::invokeMethod(controller, "setSelectionColor", Q_ARG(int, 4));
    CHECK(controller->selectedClip().value(QStringLiteral("colorIndex")).toInt() == 4);
    CHECK(controller->undoText() == QStringLiteral("Undo Label Color"));
    QMetaObject::invokeMethod(menu, "close");

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// =====================================================================================================================
// 0.1.2: Premiere's rate stretch (R) and pen (P) tools

TEST_CASE("R stretches a clip from its edge: the speed fills the new length, linked audio follows, one undo step",
          "[app][timeline][edit]") {
    SECTION("the tail: the clip keeps its source In and plays the same material over the new length") {
        Rig rig(R"(
            V1 |Aaaa........|
            A1 |Aaaa........|
        )");
        rig.view->setTool(QStringLiteral("rateStretch"));
        CHECK(rig.view->tool() == QStringLiteral("rateStretch"));
        const ClipId a = rig.clip("V1@0");
        const ClipId audio = rig.clip("A1@0");
        const Ticks sourceIn = rig.controller.currentSequence()->findClip(a)->clip().sourceIn;
        const std::size_t undo = rig.controller.session().undoCount();
        rig.drag({rig.x(4) - 2, rig.y("V1")}, {rig.x(8) - 2, rig.y("V1")});
        edit::SpeedChange half;
        half.speed = *edit::speedForDuration(f(4), f(8));
        const std::array<ClipId, 1> ids{a};
        CHECK(rig.picture() == rig.expected(edit::setSpeed(rig.tl.state, rig.tl.seq, ids, half)));
        const SequencePtr seq = rig.controller.currentSequence();
        CHECK(seq->findClip(a)->clip().duration() == f(8));
        CHECK(seq->findClip(a)->clip().speed == half.speed);
        CHECK(seq->findClip(a)->clip().sourceIn == sourceIn);
        CHECK(seq->findClip(audio)->clip().duration() == f(8)); // linked audio follows
        CHECK(seq->findClip(audio)->clip().speed == half.speed);
        CHECK(rig.controller.session().undoCount() == undo + 1);
        CHECK(rig.controller.undoText() == QStringLiteral("Undo Rate Stretch"));
        REQUIRE(rig.controller.undo());
        CHECK(rig.picture() == normalize(rig.original));
    }
    SECTION("the head: the clip's end stays where it was") {
        Rig rig(R"(
            V1 |....Aaaa....|
        )");
        rig.view->setTool(QStringLiteral("rateStretch"));
        const ClipId a = rig.clip("V1@4");
        rig.drag({rig.x(4) + 2, rig.y("V1")}, {rig.x(2) + 2, rig.y("V1")});
        const Clip& c = rig.controller.currentSequence()->findClip(a)->clip();
        CHECK(c.start() == f(2));
        CHECK(c.end() == f(8));
        CHECK(c.speed == *edit::speedForDuration(f(4), f(6)));
        CHECK(rig.controller.undoText() == QStringLiteral("Undo Rate Stretch"));
    }
    SECTION("the body: nothing moves") {
        Rig rig(R"(
            V1 |Aaaa........|
        )");
        rig.view->setTool(QStringLiteral("rateStretch"));
        rig.drag({rig.x(2), rig.y("V1")}, {rig.x(6), rig.y("V1")});
        CHECK(rig.picture() == normalize(rig.original));
    }
}

TEST_CASE("P: a click on the volume line adds a keyframe, a drag moves it; the clips stay put", "[app][timeline][edit]") {
    Rig rig(R"(
        V1 |Aaaaaaaaaaaa|
        A1 |Aaaaaaaaaaaa|
    )");
    CHECK_FALSE(rig.view->volumeBandShown());
    rig.view->setTool(QStringLiteral("pen"));
    CHECK(rig.view->tool() == QStringLiteral("pen"));
    CHECK(rig.view->volumeBandShown()); // the pen shows the band
    rig.harness.frame();
    const ClipId audio = rig.clip("A1@0");
    const auto volume = [&] {
        return rig.controller.currentSequence()->findClip(audio)->clip().properties()[ParamId::VolumeDb];
    };
    REQUIRE(volume().isConstant());
    const TrackId a1 = rig.tl.track("A1");
    const auto row = std::ranges::find_if(rig.view->rows(), [&](const TimelineView::Row& r) { return r.track->id == a1; });
    REQUIRE(row != rig.view->rows().end());
    const QRectF clipRect(rig.x(0), row->y - rig.view->scrollY(), rig.x(12) - rig.x(0), row->height);
    const QRectF area = TimelineView::volumeArea(clipRect);
    const double y0 = area.top() + TimelineView::volumeFraction(0.0) * area.height();
    const double x = rig.x(6) + 1;

    // A click on the line: a key at 0 dB there.
    rig.press({x, y0});
    rig.release({x, y0});
    REQUIRE(volume().isCurve());
    REQUIRE(volume().curve()->keys.size() == 1);
    CHECK(volume().curve()->keys[0].value == 0.0);
    CHECK(rig.controller.undoText() == QStringLiteral("Undo Add Volume Keyframe"));

    // Dragging the key down (Shift: level only) is one undo step.
    const std::size_t undo = rig.controller.session().undoCount();
    rig.drag({x, y0}, {x, y0 + 20}, Qt::ShiftModifier);
    REQUIRE(volume().curve()->keys.size() == 1);
    CHECK(volume().curve()->keys[0].value < -5.0);
    CHECK(rig.controller.session().undoCount() == undo + 1);
    CHECK(rig.controller.undoText() == QStringLiteral("Undo Volume Keyframe"));
    CHECK(rig.picture() == normalize(rig.original)); // no clip moved or trimmed

    // Back on the selection tool the band is hidden again (it is not shown for it here).
    rig.view->setTool(QStringLiteral("select"));
    CHECK_FALSE(rig.view->volumeBandShown());
}
