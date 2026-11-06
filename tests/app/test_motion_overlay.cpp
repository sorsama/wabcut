// P.2 (E6): direct manipulation of Motion in the Program Monitor (ProgramMotion, MotionOverlay.qml): the box follows
// what playback draws, hit testing names the gestures, and move / scale / rotate / anchor point / motion path drags are
// one undo step each, keyed where Position is animated. Offscreen; no window is shown.

#include "BridgeIds.h"
#include "MotionBridge.h"
#include "ProjectController.h"
#include "ShellFixture.h"
#include "TimelineFixture.h"

#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/FrameScaling.h>
#include <wabcut/model/MotionOps.h>
#include <wabcut/model/TitleOps.h>
#include <wabcut/playback/video/Evaluate.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QKeyEvent>
#include <QQuickItem>

#include <array>
#include <memory>
#include <span>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using Catch::Approx;

namespace {

constexpr std::string_view kPicture = R"(
    V2     |..........|
    V1 tgt |Aaaaaaaaaa|
)";

/// A 1920 x 1080 clip on V1 (and optionally a second on V2), selected, the playhead at frame 4.
struct MotionRig {
    Timeline tl;
    ProjectController controller;
    MotionBridge motion;
    ClipId clip;

    explicit MotionRig(std::string_view picture = kPicture, int rotation = 0, int mediaWidth = 0, int mediaHeight = 0,
                       bool mirrored = false)
        : tl(picture) {
        if (rotation != 0 || mediaWidth > 0 || mirrored) {
            for (MediaPtr& m : tl.state.media) {
                auto copy = std::make_shared<MediaItem>(*m);
                copy->info.video.front().rotationDegrees = rotation;
                copy->info.video.front().mirrored = mirrored; // card 154
                if (mediaWidth > 0) { // p02 integration: a picture larger than the sequence frame
                    copy->info.video.front().width = mediaWidth;
                    copy->info.video.front().height = mediaHeight;
                }
                m = copy;
            }
        }
        clip = tl.clip("V1@0");
        controller.resetProject(tl.state, tl.seq);
        motion.setController(&controller);
        controller.selectClip(clipKey(clip));
        controller.setPlayhead(f(4));
    }
    const Clip& current(ClipId id) const {
        return controller.snapshot()->findSequence(tl.seq)->findClip(id)->clip();
    }
    edit::MotionValues values(ClipId id) const { return edit::motionAt(current(id), current(id).curveTimeAt(f(4))); }
    std::size_t undo() { return controller.session().undoCount(); }
    void set(ClipId id, edit::MotionChange change) {
        REQUIRE(controller.applyEditFn("set", [&](const ProjectState& s) {
            return edit::setMotion(s, tl.seq, id, current(id).curveTimeAt(f(4)), change);
        }));
    }
};

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

void checkQuad(const QVariantList& quad, std::initializer_list<double> expected) {
    REQUIRE(quad.size() == static_cast<int>(expected.size()));
    int i = 0;
    for (const double e : expected) {
        INFO("corner value " << i);
        CHECK(quad[i++].toDouble() == Approx(e).margin(1e-6));
    }
}

/// Drags `kind` from (x0, y0) to (x1, y1) (fractions) as one gesture.
void drag(MotionBridge& m, const QString& kind, double x0, double y0, double x1, double y1, bool shift = false,
          bool ctrl = false, bool alt = false) {
    REQUIRE(m.beginDrag(kind, x0, y0));
    m.dragTo((x0 + x1) / 2.0, (y0 + y1) / 2.0, shift, ctrl, alt, 8.0 / 1920.0);
    m.dragTo(x1, y1, shift, ctrl, alt, 8.0 / 1920.0);
    m.endDrag();
}

} // namespace

TEST_CASE("Motion direct manipulation engages on the selected video clip and lets go on another selection",
          "[app][monitor][motion]") {
    MotionRig rig;
    CHECK_FALSE(rig.motion.active());
    rig.motion.setEngaged(true);
    REQUIRE(rig.motion.engaged());
    REQUIRE(rig.motion.active());
    CHECK(rig.motion.clip() == clipKey(rig.clip));
    // The default Motion fills the frame: corners at the canvas corners, the anchor in the middle.
    checkQuad(rig.motion.quad(), {0, 0, 1, 0, 1, 1, 0, 1});
    CHECK(rig.motion.anchor().value(QStringLiteral("x")).toDouble() == Approx(0.5));
    CHECK(rig.motion.handles().size() == 8);
    CHECK(rig.motion.path().isEmpty()); // Position is not animated
    // Past the clip there is nothing to show.
    rig.controller.setPlayhead(f(20));
    CHECK_FALSE(rig.motion.active());
    rig.controller.setPlayhead(f(4));
    CHECK(rig.motion.active());
    // Selecting nothing (or another clip) lets go, as Premiere's Effect Controls does.
    rig.controller.clearSelection();
    CHECK_FALSE(rig.motion.engaged());
    CHECK_FALSE(rig.motion.active());
}

TEST_CASE("Motion box matches what playback draws for a rotated phone clip", "[app][monitor][motion]") {
    MotionRig rig(kPicture, 90);
    edit::MotionChange change;
    change.scale = 0.5;
    change.rotation = 10.0;
    change.anchorX = 100.0;
    change.anchorY = -40.0;
    change.positionX = 33.0;
    rig.set(rig.clip, change);
    rig.motion.setEngaged(true);
    REQUIRE(rig.motion.active());
    // The evaluator's layer for the clip, placed by the renderer's own math.
    const ProjectSnapshot snap = rig.controller.snapshot();
    const Sequence& seq = *snap->findSequence(rig.tl.seq);
    std::vector<playback::LayerSpec> specs;
    playback::evaluateVideoLayers(*snap, seq, f(4), specs, false);
    REQUIRE(specs.size() == 1);
    const render::Affine2D m = render::layerToCanvas(specs[0].transform, 1920, 1080, 1.0, {seq.width, seq.height});
    const render::Vec2 tl = m.map({0, 0});
    const render::Vec2 br = m.map({1920, 1080});
    const QVariantList quad = rig.motion.quad();
    CHECK(quad[0].toDouble() == Approx(tl.x / seq.width).margin(1e-9));
    CHECK(quad[1].toDouble() == Approx(tl.y / seq.height).margin(1e-9));
    CHECK(quad[4].toDouble() == Approx(br.x / seq.width).margin(1e-9));
    CHECK(quad[5].toDouble() == Approx(br.y / seq.height).margin(1e-9));

    SECTION("dragging the anchor point keeps the picture where it is") {
        const QVariantList before = rig.motion.quad();
        const QVariantMap a = rig.motion.anchor();
        drag(rig.motion, QStringLiteral("anchor"), a.value(QStringLiteral("x")).toDouble(),
             a.value(QStringLiteral("y")).toDouble(), 0.3, 0.6);
        const QVariantList after = rig.motion.quad();
        for (int i = 0; i < 8; ++i)
            CHECK(after[i].toDouble() == Approx(before[i].toDouble()).margin(1e-9));
        CHECK(rig.motion.anchor().value(QStringLiteral("x")).toDouble() == Approx(0.3));
        CHECK(rig.motion.anchor().value(QStringLiteral("y")).toDouble() == Approx(0.6));
    }
    SECTION("Alt moves the anchor point alone: the picture moves, the anchor stays") {
        const QVariantList before = rig.motion.quad();
        const QVariantMap a = rig.motion.anchor();
        drag(rig.motion, QStringLiteral("anchor"), a.value(QStringLiteral("x")).toDouble(),
             a.value(QStringLiteral("y")).toDouble(), a.value(QStringLiteral("x")).toDouble() + 0.1,
             a.value(QStringLiteral("y")).toDouble(), false, false, true);
        const QVariantList after = rig.motion.quad();
        for (int i = 0; i < 8; i += 2) {
            CHECK(after[i].toDouble() == Approx(before[i].toDouble() + 0.1).margin(1e-9));
            CHECK(after[i + 1].toDouble() == Approx(before[i + 1].toDouble()).margin(1e-9));
        }
        CHECK(rig.motion.anchor().value(QStringLiteral("x")).toDouble() == Approx(a.value(QStringLiteral("x")).toDouble()));
    }
}

TEST_CASE("Motion box and anchor follow a mirrored clip the way playback draws it (card 154)", "[app][monitor][motion]") {
    for (const int rotation : {0, 90, 180, 270}) {
        INFO("rotation " << rotation);
        MotionRig rig(kPicture, rotation, 0, 0, true);
        edit::MotionChange change;
        change.scale = 0.5;
        change.rotation = 10.0;
        change.anchorX = 100.0;
        change.anchorY = -40.0;
        change.positionX = 33.0;
        rig.set(rig.clip, change);
        rig.motion.setEngaged(true);
        REQUIRE(rig.motion.active());
        const ProjectSnapshot snap = rig.controller.snapshot();
        const Sequence& seq = *snap->findSequence(rig.tl.seq);
        std::vector<playback::LayerSpec> specs;
        playback::evaluateVideoLayers(*snap, seq, f(4), specs, false);
        REQUIRE(specs.size() == 1);
        CHECK(specs[0].transform.mirrorX);
        const render::Affine2D m = render::layerToCanvas(specs[0].transform, 1920, 1080, 1.0, {seq.width, seq.height});
        const render::Vec2 corners[] = {m.map({0, 0}), m.map({1920, 0}), m.map({1920, 1080}), m.map({0, 1080})};
        const QVariantList quad = rig.motion.quad();
        REQUIRE(quad.size() == 8);
        for (int i = 0; i < 4; ++i) {
            CHECK(quad[2 * i].toDouble() == Approx(corners[i].x / seq.width).margin(1e-9));
            CHECK(quad[2 * i + 1].toDouble() == Approx(corners[i].y / seq.height).margin(1e-9));
        }
        // Dragging the anchor point keeps the picture where it is, flip and all.
        const QVariantList before = rig.motion.quad();
        const QVariantMap a = rig.motion.anchor();
        drag(rig.motion, QStringLiteral("anchor"), a.value(QStringLiteral("x")).toDouble(),
             a.value(QStringLiteral("y")).toDouble(), 0.3, 0.6);
        const QVariantList after = rig.motion.quad();
        for (int i = 0; i < 8; ++i)
            CHECK(after[i].toDouble() == Approx(before[i].toDouble()).margin(1e-9));
        CHECK(rig.motion.anchor().value(QStringLiteral("x")).toDouble() == Approx(0.3));
        CHECK(rig.motion.anchor().value(QStringLiteral("y")).toDouble() == Approx(0.6));
    }
}

TEST_CASE("Motion box matches the picture Scale to Frame Size fits (p02 integration)", "[app][monitor][motion]") {
    // A UHD picture in the HD sequence with Scale to Frame Size on: playback fits it (half size) before Motion's Scale,
    // and the box follows the drawn picture, also through an anchor point drag.
    MotionRig rig(kPicture, 0, 3840, 2160);
    const ClipId fitted = rig.clip;
    REQUIRE(rig.controller.applyEditFn("fit", [&](const ProjectState& s) {
        return edit::setScaleToFrameSize(s, rig.tl.seq, std::span(&fitted, 1), true);
    }));
    REQUIRE(rig.current(rig.clip).content().scaleToFrame);
    edit::MotionChange change;
    change.scale = 0.8;
    change.rotation = 15.0;
    change.anchorX = 200.0;
    change.anchorY = -100.0;
    change.positionX = 50.0;
    rig.set(rig.clip, change);
    rig.motion.setEngaged(true);
    REQUIRE(rig.motion.active());
    const auto playbackCorners = [&] {
        const ProjectSnapshot snap = rig.controller.snapshot();
        const Sequence& seq = *snap->findSequence(rig.tl.seq);
        std::vector<playback::LayerSpec> specs;
        playback::evaluateVideoLayers(*snap, seq, f(4), specs, false);
        REQUIRE(specs.size() == 1);
        const render::Affine2D m = render::layerToCanvas(specs[0].transform, 3840, 2160, 1.0, {seq.width, seq.height});
        const render::Vec2 a = m.map({0, 0});
        const render::Vec2 b = m.map({3840, 2160});
        return std::array<double, 4>{a.x / seq.width, a.y / seq.height, b.x / seq.width, b.y / seq.height};
    };
    const auto boxMatches = [&] {
        const std::array<double, 4> expected = playbackCorners();
        const QVariantList quad = rig.motion.quad();
        CHECK(quad[0].toDouble() == Approx(expected[0]).margin(1e-9));
        CHECK(quad[1].toDouble() == Approx(expected[1]).margin(1e-9));
        CHECK(quad[4].toDouble() == Approx(expected[2]).margin(1e-9));
        CHECK(quad[5].toDouble() == Approx(expected[3]).margin(1e-9));
    };
    boxMatches();
    // The anchor point drag keeps the drawn picture in place (its maths uses the fitted scale too).
    const QVariantMap a = rig.motion.anchor();
    drag(rig.motion, QStringLiteral("anchor"), a.value(QStringLiteral("x")).toDouble(),
         a.value(QStringLiteral("y")).toDouble(), 0.3, 0.6);
    boxMatches();
    CHECK(rig.motion.anchor().value(QStringLiteral("x")).toDouble() == Approx(0.3));
    CHECK(rig.motion.anchor().value(QStringLiteral("y")).toDouble() == Approx(0.6));
}

TEST_CASE("Motion hit testing names the gestures and a double-click picks the topmost picture", "[app][monitor][motion]") {
    MotionRig rig(R"(
        V2     |Bbbbbbbbbb|
        V1 tgt |Aaaaaaaaaa|
    )");
    const ClipId top = rig.tl.clip("V2@0");
    edit::MotionChange half;
    half.scale = 0.5;
    rig.set(top, half); // V2 covers the middle: 480..1440 x 270..810
    CHECK(rig.motion.clipAt(0.5, 0.4) == clipKey(top));
    CHECK(rig.motion.clipAt(0.1, 0.1) == clipKey(rig.clip));
    CHECK(rig.motion.clipAt(1.5, 0.1).isEmpty());
    REQUIRE(rig.motion.engageAt(0.6, 0.6));
    CHECK(rig.motion.clip() == clipKey(top));
    CHECK(rig.controller.primaryClip() == top);
    CHECK(rig.motion.engaged()); // the selection it made did not let go
    const double r = 0.005;
    CHECK(rig.motion.hitTest(0.5, 0.5, r, r) == QStringLiteral("anchor"));
    CHECK(rig.motion.hitTest(0.3, 0.4, r, r) == QStringLiteral("move"));
    CHECK(rig.motion.hitTest(0.25, 0.25, r, r) == QStringLiteral("scale-tl"));
    CHECK(rig.motion.hitTest(0.5, 0.75, r, r) == QStringLiteral("scale-b"));
    CHECK(rig.motion.hitTest(0.25 - 2 * r, 0.25 - 2 * r, r, r) == QStringLiteral("rotate"));
    CHECK(rig.motion.hitTest(0.05, 0.05, r, r).isEmpty());
}

TEST_CASE("Motion drags are one undo step each: move, snap, scale, rotate, cancel", "[app][monitor][motion]") {
    MotionRig rig;
    edit::MotionChange half;
    half.scale = 0.5;
    rig.set(rig.clip, half); // the box spans 480..1440
    rig.motion.setEngaged(true);
    REQUIRE(rig.motion.active());
    const std::size_t steps = rig.undo();

    SECTION("move, with Shift on one axis") {
        drag(rig.motion, QStringLiteral("move"), 0.4, 0.4, 0.5, 0.45, true);
        CHECK(rig.undo() == steps + 1);
        CHECK(rig.values(rig.clip).positionX == Approx(0.1 * 1920));
        CHECK(rig.values(rig.clip).positionY == Approx(0.0));
        CHECK(rig.controller.session().undoLabel() == "Position");
    }
    SECTION("a move snaps the box's edge to the frame's edge and shows the guide") {
        REQUIRE(rig.motion.beginDrag(QStringLiteral("move"), 0.4, 0.4));
        rig.motion.dragTo(0.4 + 476.0 / 1920.0, 0.4, false, false, false, 8.0 / 1920.0);
        CHECK(rig.values(rig.clip).positionX == Approx(480.0)); // the right edge on 1920
        bool vertical = false;
        for (const QVariant& g : rig.motion.guides()) {
            const QVariantMap guide = g.toMap();
            if (guide.value(QStringLiteral("orientation")).toString() == QStringLiteral("vertical")) {
                vertical = true;
                CHECK(guide.value(QStringLiteral("position")).toDouble() == Approx(1.0));
            }
        }
        CHECK(vertical);
        rig.motion.dragTo(0.4 + 470.0 / 1920.0, 0.4, false, true, false, 8.0 / 1920.0); // Ctrl: no snapping
        CHECK(rig.values(rig.clip).positionX == Approx(470.0));
        CHECK(rig.motion.guides().isEmpty());
        rig.motion.endDrag();
        CHECK(rig.undo() == steps + 1);
    }
    SECTION("a corner scales around the anchor point") {
        drag(rig.motion, QStringLiteral("scale-br"), 0.75, 0.75, 0.875, 0.875);
        CHECK(rig.values(rig.clip).scale == Approx(0.75));
        CHECK(rig.undo() == steps + 1);
        checkQuad(rig.motion.quad(), {0.125, 0.125, 0.875, 0.125, 0.875, 0.875, 0.125, 0.875});
    }
    SECTION("outside a corner rotates around the anchor point, Shift in 45-degree steps") {
        drag(rig.motion, QStringLiteral("rotate"), 0.75, 0.5, 0.5, 0.5 + 0.25 * 1920.0 / 1080.0);
        CHECK(rig.values(rig.clip).rotation == Approx(90.0));
        drag(rig.motion, QStringLiteral("rotate"), 0.75, 0.5, 0.75, 0.5 + 0.2 * 1920.0 / 1080.0, true);
        CHECK(rig.values(rig.clip).rotation == Approx(135.0)); // 90 + 38.7, in 45-degree steps
        CHECK(rig.undo() == steps + 2);
    }
    SECTION("Esc cancels the drag and restores the clip") {
        REQUIRE(rig.motion.beginDrag(QStringLiteral("move"), 0.4, 0.4));
        rig.motion.dragTo(0.6, 0.6, false, true, false, 0);
        CHECK(rig.values(rig.clip).positionX != Approx(0.0));
        rig.motion.cancelDrag();
        CHECK(rig.values(rig.clip).positionX == Approx(0.0));
        CHECK(rig.undo() == steps);
    }
    SECTION("Nudge Selected Object moves by sequence pixels, one step each") {
        REQUIRE(rig.motion.nudge(5, -1));
        CHECK(rig.values(rig.clip).positionX == Approx(5.0));
        CHECK(rig.values(rig.clip).positionY == Approx(-1.0));
        CHECK(rig.undo() == steps + 1);
    }
}

TEST_CASE("Motion side handles stretch one axis and turn Uniform Scale off; Shift keeps the aspect (card 12)",
          "[app][monitor][motion][scalewidth]") {
    MotionRig rig;
    edit::MotionChange half;
    half.scale = 0.5;
    rig.set(rig.clip, half); // the box spans 0.25..0.75 of the frame both ways
    rig.motion.setEngaged(true);
    REQUIRE(rig.motion.active());
    const std::size_t steps = rig.undo();

    SECTION("the right edge changes the width only, as one undo step") {
        drag(rig.motion, QStringLiteral("scale-r"), 0.75, 0.5, 0.875, 0.5);
        const edit::MotionValues v = rig.values(rig.clip);
        CHECK_FALSE(v.uniformScale);
        CHECK(v.scale == Approx(0.5)); // the height is untouched
        CHECK(v.scaleWidth == Approx(0.75));
        CHECK(rig.undo() == steps + 1); // the switch and the width are one step
        CHECK(rig.controller.session().undoLabel() == "Scale");
        checkQuad(rig.motion.quad(), {0.125, 0.25, 0.875, 0.25, 0.875, 0.75, 0.125, 0.75});
        REQUIRE(rig.controller.undo());
        CHECK(rig.values(rig.clip).uniformScale);
        CHECK(rig.values(rig.clip).scaleWidth == Approx(0.5));
    }
    SECTION("the left edge, dragged outward, widens around the anchor") {
        drag(rig.motion, QStringLiteral("scale-l"), 0.25, 0.5, 0.125, 0.5);
        CHECK(rig.values(rig.clip).scaleWidth == Approx(0.75));
        CHECK(rig.values(rig.clip).scale == Approx(0.5));
    }
    SECTION("the bottom edge changes the height only") {
        drag(rig.motion, QStringLiteral("scale-b"), 0.5, 0.75, 0.5, 0.875);
        const edit::MotionValues v = rig.values(rig.clip);
        CHECK_FALSE(v.uniformScale);
        CHECK(v.scale == Approx(0.75)); // Scale Height
        CHECK(v.scaleWidth == Approx(0.5));
        checkQuad(rig.motion.quad(), {0.25, 0.125, 0.75, 0.125, 0.75, 0.875, 0.25, 0.875});
    }
    SECTION("Shift on a side keeps the aspect: the clip stays uniform") {
        drag(rig.motion, QStringLiteral("scale-r"), 0.75, 0.5, 0.875, 0.5, true);
        const edit::MotionValues v = rig.values(rig.clip);
        CHECK(v.uniformScale);
        CHECK(v.scale == Approx(0.75));
        CHECK(rig.undo() == steps + 1);
    }
    SECTION("a corner of a uniform clip stays uniform") {
        drag(rig.motion, QStringLiteral("scale-br"), 0.75, 0.75, 0.875, 0.875);
        CHECK(rig.values(rig.clip).uniformScale);
        CHECK(rig.values(rig.clip).scale == Approx(0.75));
    }
    SECTION("a corner of a stretched clip follows the pointer on both axes; Shift keeps its aspect") {
        drag(rig.motion, QStringLiteral("scale-r"), 0.75, 0.5, 0.875, 0.5); // width 0.75, height 0.5
        REQUIRE_FALSE(rig.values(rig.clip).uniformScale);
        // The bottom-right corner is at (0.875, 0.75): drag it to (0.9375, 0.875).
        drag(rig.motion, QStringLiteral("scale-br"), 0.875, 0.75, 0.9375, 0.875);
        edit::MotionValues v = rig.values(rig.clip);
        CHECK_FALSE(v.uniformScale);
        CHECK(v.scaleWidth == Approx(0.75 * (0.4375 / 0.375)));
        CHECK(v.scale == Approx(0.5 * (0.375 / 0.25)));
        // Shift: both by one factor, so the box keeps its shape.
        const double ratio = v.scaleWidth / v.scale;
        drag(rig.motion, QStringLiteral("scale-br"), 0.9, 0.9, 0.6, 0.6, true);
        v = rig.values(rig.clip);
        CHECK(v.scaleWidth / v.scale == Approx(ratio));
    }
    SECTION("the anchor still keeps the picture in place on a stretched clip") {
        drag(rig.motion, QStringLiteral("scale-r"), 0.75, 0.5, 0.875, 0.5);
        const QVariantList before = rig.motion.quad();
        const QVariantMap a = rig.motion.anchor();
        drag(rig.motion, QStringLiteral("anchor"), a.value(QStringLiteral("x")).toDouble(),
             a.value(QStringLiteral("y")).toDouble(), 0.4, 0.45);
        const QVariantList after = rig.motion.quad();
        for (int i = 0; i < 8; ++i)
            CHECK(after[i].toDouble() == Approx(before[i].toDouble()).margin(1e-9));
    }
}

TEST_CASE("The Motion box of a stretched, turned clip matches what playback draws (card 12)", "[app][monitor][motion][scalewidth]") {
    MotionRig rig(kPicture, 90);
    edit::MotionChange change;
    change.scale = 0.5;
    change.scaleWidth = 1.5;
    change.rotation = 20.0;
    change.anchorX = 100.0;
    change.anchorY = -40.0;
    change.positionX = 33.0;
    rig.set(rig.clip, change);
    rig.motion.setEngaged(true);
    REQUIRE(rig.motion.active());
    const ProjectSnapshot snap = rig.controller.snapshot();
    const Sequence& seq = *snap->findSequence(rig.tl.seq);
    std::vector<playback::LayerSpec> specs;
    playback::evaluateVideoLayers(*snap, seq, f(4), specs, false);
    REQUIRE(specs.size() == 1);
    CHECK(specs[0].transform.scaleX() != Approx(specs[0].transform.scale)); // a stretched layer
    const render::Affine2D m = render::layerToCanvas(specs[0].transform, 1920, 1080, 1.0, {seq.width, seq.height});
    const QVariantList quad = rig.motion.quad();
    const std::array<render::Vec2, 4> corners{m.map({0, 0}), m.map({1920, 0}), m.map({1920, 1080}), m.map({0, 1080})};
    for (int i = 0; i < 4; ++i) {
        CHECK(quad[2 * i].toDouble() == Approx(corners[static_cast<std::size_t>(i)].x / seq.width).margin(1e-9));
        CHECK(quad[2 * i + 1].toDouble() == Approx(corners[static_cast<std::size_t>(i)].y / seq.height).margin(1e-9));
    }
    // Quarter turns swap which stored axis is the width: a stretch is along the picture as shown.
    MotionRig upright(kPicture, 0);
    upright.set(upright.clip, change);
    const ProjectSnapshot snapUpright = upright.controller.snapshot();
    std::vector<playback::LayerSpec> straight;
    playback::evaluateVideoLayers(*snapUpright, *snapUpright->findSequence(upright.tl.seq), f(4), straight, false);
    REQUIRE(straight.size() == 1);
    CHECK(straight[0].transform.scaleX() == Approx(1.5));
    CHECK(straight[0].transform.scale == Approx(0.5));
    CHECK(specs[0].transform.scaleX() == Approx(0.5)); // the turned stream: stored axes swapped
    CHECK(specs[0].transform.scale == Approx(1.5));
}

TEST_CASE("Motion keys animated Position at the playhead and draws its motion path", "[app][monitor][motion]") {
    MotionRig rig;
    // Position X and Y animated: (0, 0) at frame 0, (400, 200) at frame 8.
    for (const ParamId id : {ParamId::PositionX, ParamId::PositionY}) {
        Curve c;
        c.keys = {Keyframe{f(0), 0.0}, Keyframe{f(8), id == ParamId::PositionX ? 400.0 : 200.0}};
        REQUIRE(rig.controller.applyEditFn("key", [&](const ProjectState& s) {
            return edit::setParameter(s, rig.tl.seq, rig.clip, edit::ParamRef::of(id), ParamValue(c));
        }));
    }
    rig.motion.setEngaged(true);
    REQUIRE(rig.motion.active());
    QVariantMap path = rig.motion.path();
    REQUIRE(path.value(QStringLiteral("keys")).toList().size() == 2);
    CHECK(path.value(QStringLiteral("points")).toList().size() > 4);
    const std::size_t steps = rig.undo();

    // A move at frame 4 keys both curves there (the third key of each).
    drag(rig.motion, QStringLiteral("move"), 0.6, 0.6, 0.65, 0.6, false, true);
    CHECK(rig.undo() == steps + 1);
    const Curve* x = rig.current(rig.clip).properties()[ParamId::PositionX].curve();
    const Curve* y = rig.current(rig.clip).properties()[ParamId::PositionY].curve();
    REQUIRE(x);
    REQUIRE(y);
    CHECK(x->keys.size() == 3);
    CHECK(y->keys.size() == 3);
    CHECK(x->keys[1].value == Approx(200.0 + 0.05 * 1920));
    path = rig.motion.path();
    const QVariantList keys = path.value(QStringLiteral("keys")).toList();
    REQUIRE(keys.size() == 3);
    CHECK(keys[1].toMap().value(QStringLiteral("current")).toBool());

    // Dragging the last key's dot moves that key only.
    const QVariantMap last = keys[2].toMap();
    const QString kind = rig.motion.hitTest(last.value(QStringLiteral("x")).toDouble(), last.value(QStringLiteral("y")).toDouble(),
                                            0.004, 0.004);
    CHECK(kind == QStringLiteral("key:2"));
    drag(rig.motion, kind, last.value(QStringLiteral("x")).toDouble(), last.value(QStringLiteral("y")).toDouble(),
         last.value(QStringLiteral("x")).toDouble() - 0.1, last.value(QStringLiteral("y")).toDouble());
    x = rig.current(rig.clip).properties()[ParamId::PositionX].curve();
    REQUIRE(x);
    REQUIRE(x->keys.size() == 3);
    CHECK(x->keys[2].value == Approx(400.0 - 0.1 * 1920));
    CHECK(x->keys[1].value == Approx(200.0 + 0.05 * 1920)); // untouched
    CHECK(rig.undo() == steps + 2);
}

TEST_CASE("Crop direct manipulation drags the Crop effect's edges as one undo step", "[app][monitor][motion]") {
    MotionRig rig;
    // Without a Crop effect the crop handles have nothing to edit.
    rig.motion.toggleCrop();
    CHECK(rig.motion.mode() == QStringLiteral("crop"));
    CHECK(rig.motion.engaged());
    CHECK_FALSE(rig.motion.active());
    CHECK_FALSE(rig.motion.message().isEmpty());
    rig.motion.setEngaged(false);

    ClipId clip = rig.clip;
    EffectInstanceId crop;
    REQUIRE(rig.controller.applyEditFn("crop", [&](const ProjectState& s) {
        auto r = edit::addEffect(s, rig.tl.seq, std::span<const ClipId>(&clip, 1), "wabcut.crop");
        if (r)
            crop = r->createdEffects.at(0);
        return r;
    }));
    CHECK(rig.motion.isCropSection(effectKey(crop)));
    CHECK_FALSE(rig.motion.isCropSection(QStringLiteral("e999999")));
    rig.motion.toggleCrop();
    REQUIRE(rig.motion.active());
    CHECK(rig.motion.cropAvailable());
    CHECK(rig.motion.anchor().isEmpty()); // no anchor or path while cropping
    checkQuad(rig.motion.quad(), {0, 0, 1, 0, 1, 1, 0, 1});
    CHECK(rig.motion.hitTest(0.0, 0.5, 0.005, 0.005) == QStringLiteral("crop-l"));
    CHECK(rig.motion.hitTest(1.0, 1.0, 0.005, 0.005) == QStringLiteral("crop-br"));
    CHECK(rig.motion.hitTest(0.5, 0.5, 0.005, 0.005) == QStringLiteral("none")); // inside: nothing to drag
    const std::size_t steps = rig.undo();

    // The left edge to a quarter, then the bottom-right corner in: one undo step each.
    drag(rig.motion, QStringLiteral("crop-l"), 0.0, 0.5, 0.25, 0.4);
    drag(rig.motion, QStringLiteral("crop-br"), 1.0, 1.0, 0.9, 0.8);
    CHECK(rig.undo() == steps + 2);
    CHECK(rig.controller.session().undoLabel() == "Crop");
    const auto value = [&](const char* key) {
        for (const EffectInstance& e : rig.current(clip).effects()) {
            if (e.id == crop && e.find(key))
                return e.find(key)->valueAt({});
        }
        return 0.0;
    };
    CHECK(value("left") == Approx(0.25));
    CHECK(value("right") == Approx(0.1));
    CHECK(value("bottom") == Approx(0.2));
    CHECK(value("top") == Approx(0.0));
    checkQuad(rig.motion.quad(), {0.25, 0, 0.9, 0, 0.9, 0.8, 0.25, 0.8});
    // An edge never crosses the other: some picture stays.
    drag(rig.motion, QStringLiteral("crop-l"), 0.25, 0.5, 1.0, 0.5);
    CHECK(value("left") == Approx(0.89));

    // A double-click goes back to Motion.
    REQUIRE(rig.motion.engageAt(0.5, 0.5));
    CHECK(rig.motion.mode() == QStringLiteral("transform"));
}

TEST_CASE("Main.qml has the Motion overlay over the program picture", "[app][monitor][motion]") {
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* motion = shell.engine.singletonInstance<MotionBridge*>("Wabcut", "ProgramMotion");
    REQUIRE(controller);
    REQUIRE(motion);
    Timeline tl(kPicture);
    controller->resetProject(tl.state, tl.seq);
    controller->selectClip(clipKey(tl.clip("V1@0")));
    controller->setPlayhead(f(4));
    QQuickItem* frame = findNamed(shell.window->contentItem(), QStringLiteral("programFrameArea"));
    REQUIRE(frame);
    QQuickItem* overlay = findNamed(frame, QStringLiteral("programMotionOverlay"));
    REQUIRE(overlay);
    QQuickItem* area = findNamed(overlay, QStringLiteral("programMotionArea"));
    REQUIRE(area);
    CHECK_FALSE(area->isVisible()); // nothing engaged: the pointer goes to the monitor
    motion->setEngaged(true);
    REQUIRE(waitFor([&] { return area->isVisible(); }, 3000));
    // A scaled, rotated clip with its handles, for the screenshot.
    REQUIRE(motion->beginDrag(QStringLiteral("scale-br"), 1.0, 1.0));
    motion->dragTo(0.8, 0.8, false, false, false, 0);
    motion->endDrag();
    REQUIRE(motion->beginDrag(QStringLiteral("rotate"), 0.8, 0.5));
    motion->dragTo(0.8, 0.6, false, false, false, 0);
    motion->endDrag();
    spin(50);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    const double dpr = image.devicePixelRatio();
    const QRectF rect = frame->mapRectToScene(QRectF(0, 0, frame->width(), frame->height()));
    CHECK(image.copy(QRectF(rect.x() * dpr, rect.y() * dpr, rect.width() * dpr, rect.height() * dpr).toRect())
              .save(QDir(outputDir()).filePath(QStringLiteral("motion-overlay.png"))));
    // Esc on the focused overlay lets go of Motion.
    overlay->forceActiveFocus();
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(shell.window.get(), &press);
    CHECK_FALSE(motion->engaged());
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// ---- Card 060: crop handles for a text clip -------------------------------------------------------------------------

namespace {

/// A title ("Crop me") on V2 over the V1 clip, selected, the playhead at frame 4.
struct TitleRig {
    Timeline tl{kPicture};
    ProjectController controller;
    MotionBridge motion;
    ClipId title;

    TitleRig() {
        auto added = edit::addTitle(tl.state, tl.seq, tl.track("V2"), Ticks{}, f(10), "Crop me");
        REQUIRE(added);
        title = added->placed.at(0);
        controller.resetProject(added->state, tl.seq);
        motion.setController(&controller);
        controller.selectClip(clipKey(title));
        controller.setPlayhead(f(4));
    }
    const Clip& current() const { return controller.snapshot()->findSequence(tl.seq)->findClip(title)->clip(); }
    EffectInstanceId addCrop() {
        EffectInstanceId crop;
        ClipId clip = title;
        REQUIRE(controller.applyEditFn("crop", [&](const ProjectState& s) {
            auto r = edit::addEffect(s, tl.seq, std::span<const ClipId>(&clip, 1), "wabcut.crop");
            if (r)
                crop = r->createdEffects.at(0);
            return r;
        }));
        return crop;
    }
    double value(EffectInstanceId crop, const char* key) const {
        for (const EffectInstance& e : current().effects()) {
            if (e.id == crop && e.find(key))
                return e.find(key)->valueAt({});
        }
        return 0.0; // an edge at its default is not stored
    }
};

} // namespace

TEST_CASE("A title gets crop handles like any picture: its Crop effect is edited in the Program Monitor (card 060)",
          "[app][monitor][motion][titles]") {
    TitleRig rig;
    // A text clip is a picture on a canvas of the sequence's size: Motion's box is the canvas, and crop mode is available.
    rig.motion.setEngaged(true);
    REQUIRE(rig.motion.active());
    CHECK(rig.motion.clip() == clipKey(rig.title));
    checkQuad(rig.motion.quad(), {0, 0, 1, 0, 1, 1, 0, 1});
    rig.motion.setEngaged(false);

    // Without a Crop effect there is nothing to edit, and the monitor says what to do.
    rig.motion.toggleCrop();
    CHECK(rig.motion.mode() == QStringLiteral("crop"));
    CHECK(rig.motion.engaged());
    CHECK_FALSE(rig.motion.active());
    CHECK(rig.motion.message().contains(QStringLiteral("Crop")));
    rig.motion.setEngaged(false);

    // With one, the crop handles are there for the selected text clip: Effect Controls' crop button (isCropSection) and
    // Toggle Crop Direct Manipulation both reach them.
    const EffectInstanceId crop = rig.addCrop();
    CHECK(rig.motion.isCropSection(effectKey(crop)));
    rig.motion.toggleCrop();
    REQUIRE(rig.motion.active());
    CHECK(rig.motion.cropAvailable());
    CHECK(rig.motion.clip() == clipKey(rig.title));
    CHECK(rig.motion.handles().size() == 8);
    CHECK(rig.motion.hitTest(0.0, 0.5, 0.005, 0.005) == QStringLiteral("crop-l"));
    CHECK(rig.motion.hitTest(1.0, 0.0, 0.005, 0.005) == QStringLiteral("crop-tr"));
    const std::size_t steps = rig.controller.session().undoCount();
    drag(rig.motion, QStringLiteral("crop-l"), 0.0, 0.5, 0.25, 0.4);
    drag(rig.motion, QStringLiteral("crop-b"), 0.6, 1.0, 0.6, 0.75);
    CHECK(rig.controller.session().undoCount() == steps + 2); // one undo step per drag
    CHECK(rig.controller.session().undoLabel() == "Crop");
    CHECK(rig.value(crop, "left") == Approx(0.25));
    CHECK(rig.value(crop, "bottom") == Approx(0.25));
    CHECK(rig.value(crop, "top") == Approx(0.0));
    checkQuad(rig.motion.quad(), {0.25, 0, 1, 0, 1, 0.75, 0.25, 0.75});

    // What the monitor shows is what plays: the evaluator puts the same Crop effect on the title's layer.
    const ProjectSnapshot snap = rig.controller.snapshot();
    std::vector<playback::LayerSpec> specs;
    playback::evaluateVideoLayers(*snap, *snap->findSequence(rig.tl.seq), f(4), specs, false);
    bool cropped = false;
    for (const playback::LayerSpec& spec : specs) {
        for (const render::LayerEffect& effect : spec.effects) {
            if (effect.type != render::EffectType::Crop)
                continue;
            cropped = true;
            CHECK(effect.params[0] == Approx(0.25)); // left
            CHECK(effect.params[3] == Approx(0.25)); // bottom
        }
    }
    CHECK(cropped);

    // Undo brings the edges back, and the handles follow.
    REQUIRE(rig.controller.undo());
    CHECK(rig.value(crop, "bottom") == Approx(0.0));
    CHECK(rig.value(crop, "left") == Approx(0.25));
}

TEST_CASE("Main.qml shows the crop handles over a selected title in the Program Monitor (card 060)",
          "[app][monitor][motion][titles][qml]") {
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* motion = shell.engine.singletonInstance<MotionBridge*>("Wabcut", "ProgramMotion");
    REQUIRE(controller);
    REQUIRE(motion);
    Timeline tl(kPicture);
    auto added = edit::addTitle(tl.state, tl.seq, tl.track("V2"), Ticks{}, f(10), "Crop me");
    REQUIRE(added);
    const ClipId title = added->placed.at(0);
    controller->resetProject(added->state, tl.seq);
    controller->selectClip(clipKey(title));
    controller->setPlayhead(f(4));
    ClipId clip = title;
    REQUIRE(controller->applyEditFn("crop", [&](const ProjectState& s) {
        return edit::addEffect(s, tl.seq, std::span<const ClipId>(&clip, 1), "wabcut.crop");
    }));
    QQuickItem* frame = findNamed(shell.window->contentItem(), QStringLiteral("programFrameArea"));
    REQUIRE(frame);
    QQuickItem* area = findNamed(frame, QStringLiteral("programMotionArea"));
    REQUIRE(area);
    CHECK_FALSE(area->isVisible());
    motion->toggleCrop();
    REQUIRE(waitFor([&] { return area->isVisible(); }, 3000));
    CHECK(motion->mode() == QStringLiteral("crop"));
    REQUIRE(motion->beginDrag(QStringLiteral("crop-l"), 0.0, 0.5));
    motion->dragTo(0.2, 0.5, false, false, false, 0);
    motion->endDrag();
    spin(50);
    for (const bool dark : {true, false}) {
        shell.theme->setProperty("dark", dark);
        spin(100);
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        const double dpr = image.devicePixelRatio();
        const QRectF rect = frame->mapRectToScene(QRectF(0, 0, frame->width(), frame->height()));
        CHECK(image.copy(QRectF(rect.x() * dpr, rect.y() * dpr, rect.width() * dpr, rect.height() * dpr).toRect())
                  .save(QDir(outputDir()).filePath(dark ? QStringLiteral("crop-title-overlay.png")
                                                        : QStringLiteral("crop-title-overlay-light.png"))));
    }
    shell.theme->setProperty("dark", true);
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
