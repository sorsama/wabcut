// M5o: motion graphics on the program monitor (C.13): the GraphicCanvas bridge's mapping from the resolved draw list
// to the monitor, the handles' gestures (move, scale, rotate, anchor, the paragraph text box) as one undo step each,
// snapping, inline text editing (Thai), and the timeline's responsive-time bands. Offscreen; no window is shown.

#include "BridgeIds.h"
#include "GraphicCanvasBridge.h"
#include "GraphicsBridge.h"
#include "ProjectController.h"
#include "ShellFixture.h"
#include "TimelineFixture.h"
#include "TimelineHarness.h"
#include "TimelineView.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/Graphics.h>
#include <wabcut/model/TitleOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQuickItem>

#include <memory>
#include <string>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using Catch::Approx;

namespace {

constexpr std::string_view kPicture = R"(
    V2     |..........|
    V1 tgt |Aaaaaaaaaa|
)";

Ticks seconds(double s) { return Ticks{static_cast<std::int64_t>(s * static_cast<double>(kTicksPerSecond))}; }

GraphicLayer rectLayer(std::uint32_t id, double w, double h, double x, double y) {
    GraphicLayer l;
    l.id = id;
    l.name = "Box";
    l.kind = LayerKind::Shape;
    l.shape.width = ParamValue(w);
    l.shape.height = ParamValue(h);
    l.transform.x = ParamValue(x);
    l.transform.y = ParamValue(y);
    return l;
}

GraphicLayer textLayer(std::uint32_t id, std::string text, double x, double y, double boxWidth = 0.0) {
    GraphicLayer l;
    l.id = id;
    l.name = "Name";
    l.kind = LayerKind::Text;
    TextStyle style;
    style.fontSize = 64.0;
    style.fontWeight = 600;
    l.text.runs = {TextRun{std::move(text), style}};
    l.text.boxWidth = boxWidth;
    l.transform.x = ParamValue(x);
    l.transform.y = ParamValue(y);
    return l;
}

/// A box (1, 200 x 100 at 480, 270), a name (2) and a paragraph (3, 400 px wide at 1200, 200) on a 1920 x 1080 canvas.
Graphic lowerThird() {
    Graphic g;
    g.name = "Lower third";
    g.layers = {rectLayer(1, 200, 100, 480, 270), textLayer(2, "Somchai", 220, 860), textLayer(3, "A paragraph of text", 1200, 200, 400)};
    return g;
}

/// A graphic clip on V2 from 0 for 2 s in a sequence of the given size, selected, the playhead at frame 10 (0.4 s); the
/// bridge resolves on the calling thread.
struct CanvasRig {
    Timeline tl{kPicture};
    ProjectController controller;
    GraphicCanvasBridge canvas;
    ClipId clip;

    explicit CanvasRig(int width = 1920, int height = 1080, Graphic graphic = lowerThird()) {
        if (width != tl.sequence().width || height != tl.sequence().height) {
            auto seq = std::make_shared<Sequence>(tl.sequence());
            seq->width = width;
            seq->height = height;
            tl.state = tl.state.withSequence(seq);
        }
        const auto r = edit::addGraphic(tl.state, tl.seq, tl.track("V2"), Ticks{}, seconds(2), std::move(graphic));
        REQUIRE(r);
        clip = r->placed.at(0);
        controller.resetProject(r->state, tl.seq);
        canvas.setSynchronous(true);
        canvas.setController(&controller);
        controller.selectClip(clipKey(clip));
        controller.setPlayhead(f(10)); // 0.4 s: the playhead is on the frame grid (setPlayhead snaps to frames)
        REQUIRE(canvas.active());
        REQUIRE(canvas.selectedClip() == clipKey(clip));
    }
    const GraphicLayer& layer(std::uint32_t id) const {
        const ProjectSnapshot snap = controller.snapshot();
        const Graphic* g = snap->findSequence(tl.seq)->findClip(clip)->clip().graphic();
        REQUIRE(g);
        const GraphicLayer* l = g->findLayer(id);
        REQUIRE(l);
        return *l;
    }
    double x(std::uint32_t id) const { return layer(id).transform.x.valueAt({}); }
    double y(std::uint32_t id) const { return layer(id).transform.y.valueAt({}); }
    std::size_t undo() { return controller.session().undoCount(); }
    /// A design-canvas point as fractions of a 1920 x 1080 sequence (16:9, the design canvas fills it).
    static double fx(double x) { return x / 1920.0; }
    static double fy(double y) { return y / 1080.0; }
};

QVariantList quadOf(const QVariantMap& selection) { return selection.value(QStringLiteral("quad")).toList(); }

void checkQuad(const QVariantList& quad, std::initializer_list<double> expected) {
    REQUIRE(quad.size() == static_cast<int>(expected.size()));
    int i = 0;
    for (const double e : expected) {
        INFO("corner value " << i);
        CHECK(quad[i++].toDouble() == Approx(e).margin(1e-6));
    }
}

QVariantMap handle(const QVariantMap& selection, const QString& kind) {
    for (const QVariant& h : selection.value(QStringLiteral("handles")).toList()) {
        if (h.toMap().value(QStringLiteral("kind")).toString() == kind)
            return h.toMap();
    }
    return {};
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

std::unique_ptr<QQuickItem> makeItem(Shell& shell, const QByteArray& qml, QQuickItem* parent) {
    QQmlComponent component(&shell.engine);
    component.setData(qml, QUrl(QStringLiteral("qrc:/m5o-graphics-canvas/Host.qml")));
    std::unique_ptr<QObject> object(component.create());
    INFO(component.errorString().toStdString());
    REQUIRE(object);
    auto* item = qobject_cast<QQuickItem*>(object.get());
    REQUIRE(item);
    object.release();
    item->setParentItem(parent);
    item->setPosition(QPointF(0, 0));
    item->setSize(parent->size());
    item->setZ(1000);
    return std::unique_ptr<QQuickItem>(item);
}

void key(QQuickWindow* window, int code, Qt::KeyboardModifiers modifiers = {}, const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, code, modifiers, text);
    QCoreApplication::sendEvent(window, &press);
    QKeyEvent release(QEvent::KeyRelease, code, modifiers, text);
    QCoreApplication::sendEvent(window, &release);
}

/// The centre of a layer's box on the monitor (fractions), while its graphic is resolved.
QPointF layerCentre(const GraphicCanvasBridge& canvas, int id) {
    for (const QVariant& l : canvas.layers()) {
        const QVariantMap layer = l.toMap();
        if (layer.value(QStringLiteral("id")).toInt() != id)
            continue;
        const QVariantList quad = layer.value(QStringLiteral("quad")).toList();
        REQUIRE(quad.size() == 8);
        return {(quad[0].toDouble() + quad[2].toDouble() + quad[4].toDouble() + quad[6].toDouble()) / 4.0,
                (quad[1].toDouble() + quad[3].toDouble() + quad[5].toDouble() + quad[7].toDouble()) / 4.0};
    }
    FAIL("no layer " << id);
    return {};
}

/// A double-click as the platform sends it to the monitor: press, (release,) press, double-click.
QVariantMap doubleClick(GraphicCanvasBridge& canvas, QPointF at) {
    canvas.pickPress(at.x(), at.y());
    canvas.pickPress(at.x(), at.y());
    return canvas.pickDoubleClick(at.x(), at.y());
}

QString keyOf(const QVariant& object) { return object.toMap().value(QStringLiteral("key")).toString(); }

} // namespace

TEST_CASE("Graphic canvas: resolved layers land on the monitor through the design canvas fit, 16:9 and 4:3",
          "[app][graphics]") {
    SECTION("16:9: the design canvas fills the sequence") {
        CanvasRig r;
        r.canvas.setSelectedLayer(1);
        // The box: 200 x 100 centred on (480, 270).
        checkQuad(quadOf(r.canvas.selection()), {380 / 1920.0, 220 / 1080.0, 580 / 1920.0, 220 / 1080.0, 580 / 1920.0,
                                                 320 / 1080.0, 380 / 1920.0, 320 / 1080.0});
        const QVariantMap rect = r.canvas.canvasRect();
        CHECK(rect.value(QStringLiteral("x")).toDouble() == Approx(0).margin(1e-9));
        CHECK(rect.value(QStringLiteral("height")).toDouble() == Approx(1));
        const QVariantMap anchor = r.canvas.selection().value(QStringLiteral("anchor")).toMap();
        CHECK(anchor.value(QStringLiteral("x")).toDouble() == Approx(480 / 1920.0));
        CHECK(anchor.value(QStringLiteral("y")).toDouble() == Approx(270 / 1080.0));
        // Hit-testing: the topmost layer under the point; nothing outside the layers.
        CHECK(r.canvas.layerAt(480 / 1920.0, 270 / 1080.0) == 1);
        CHECK(r.canvas.layerAt(0.98, 0.02) == 0);
        CHECK(r.canvas.layers().size() == 3);
    }
    SECTION("4:3: the 16:9 design canvas is letterboxed into the frame") {
        CanvasRig r(1440, 1080);
        r.canvas.setSelectedLayer(1);
        // Contain: 0.75 x, 135 px bars above and below.
        const QVariantMap rect = r.canvas.canvasRect();
        CHECK(rect.value(QStringLiteral("y")).toDouble() == Approx(135 / 1080.0));
        CHECK(rect.value(QStringLiteral("height")).toDouble() == Approx(0.75));
        CHECK(rect.value(QStringLiteral("width")).toDouble() == Approx(1));
        checkQuad(quadOf(r.canvas.selection()), {380 * 0.75 / 1440, (135 + 220 * 0.75) / 1080, 580 * 0.75 / 1440,
                                                 (135 + 220 * 0.75) / 1080, 580 * 0.75 / 1440, (135 + 320 * 0.75) / 1080,
                                                 380 * 0.75 / 1440, (135 + 320 * 0.75) / 1080});
        CHECK(r.canvas.layerAt(360 / 1440.0, (135 + 270 * 0.75) / 1080) == 1);
    }
}

TEST_CASE("Graphic canvas: move, scale, rotate and anchor drags are one undo step each", "[app][graphics]") {
    SECTION("Move (no snapping with Ctrl)") {
        CanvasRig r;
        r.canvas.setSelectedLayer(1);
        const auto before = r.undo();
        REQUIRE(r.canvas.beginDrag(QStringLiteral("move"), r.fx(480), r.fy(270)));
        CHECK(r.canvas.dragging());
        r.canvas.dragTo(r.fx(530), r.fy(270), false, true, 0.01);
        r.canvas.dragTo(r.fx(672), r.fy(300), false, true, 0.01);
        r.canvas.endDrag();
        CHECK_FALSE(r.canvas.dragging());
        CHECK(r.undo() == before + 1);
        CHECK(r.x(1) == Approx(672));
        CHECK(r.y(1) == Approx(300));
        // The handles followed: the box is centred on the new position.
        const QVariantList quad = quadOf(r.canvas.selection());
        CHECK(quad[0].toDouble() == Approx(572 / 1920.0));
        REQUIRE(r.controller.undo());
        CHECK(r.x(1) == Approx(480));
    }
    SECTION("Scale from a corner: uniform, then free with Shift") {
        CanvasRig r;
        r.canvas.setSelectedLayer(1);
        const QVariantMap corner = handle(r.canvas.selection(), QStringLiteral("scale-br"));
        REQUIRE_FALSE(corner.isEmpty());
        CHECK(corner.value(QStringLiteral("x")).toDouble() == Approx(580 / 1920.0));
        const auto before = r.undo();
        // Twice as far from the anchor (the centre) along the corner's direction: 200 %.
        REQUIRE(r.canvas.beginDrag(QStringLiteral("scale-br"), r.fx(580), r.fy(320)));
        r.canvas.dragTo(r.fx(680), r.fy(370), false, false, 0);
        r.canvas.endDrag();
        CHECK(r.undo() == before + 1);
        CHECK(r.layer(1).transform.scaleX.valueAt({}) == Approx(200));
        CHECK(r.layer(1).transform.scaleY.valueAt({}) == Approx(200));
        // Shift: each axis on its own.
        REQUIRE(r.canvas.beginDrag(QStringLiteral("scale-br"), r.fx(680), r.fy(370)));
        r.canvas.dragTo(r.fx(780), r.fy(370), true, false, 0);
        r.canvas.endDrag();
        CHECK(r.undo() == before + 2);
        CHECK(r.layer(1).transform.scaleX.valueAt({}) == Approx(300));
        CHECK(r.layer(1).transform.scaleY.valueAt({}) == Approx(200));
    }
    SECTION("Rotate around the anchor; Shift snaps to 15 degrees") {
        CanvasRig r;
        r.canvas.setSelectedLayer(1);
        CHECK_FALSE(handle(r.canvas.selection(), QStringLiteral("rotate")).isEmpty());
        const auto before = r.undo();
        REQUIRE(r.canvas.beginDrag(QStringLiteral("rotate"), r.fx(580), r.fy(270)));
        r.canvas.dragTo(r.fx(480), r.fy(370), false, false, 0); // a quarter turn clockwise (y down)
        r.canvas.endDrag();
        CHECK(r.undo() == before + 1);
        CHECK(r.layer(1).transform.rotation.valueAt({}) == Approx(90));
        REQUIRE(r.canvas.beginDrag(QStringLiteral("rotate"), r.fx(480), r.fy(370)));
        r.canvas.dragTo(r.fx(480 - 50), r.fy(270 + 80), true, false, 0); // ~122 degrees: snaps to 120
        r.canvas.endDrag();
        CHECK(r.layer(1).transform.rotation.valueAt({}) == Approx(120));
    }
    SECTION("Anchor: moved onto the top-left corner, the box stays where it is") {
        CanvasRig r;
        r.canvas.setSelectedLayer(1);
        const QVariantList quadBefore = quadOf(r.canvas.selection());
        const auto before = r.undo();
        REQUIRE(r.canvas.beginDrag(QStringLiteral("anchor"), r.fx(480), r.fy(270)));
        r.canvas.dragTo(r.fx(380), r.fy(220), false, true, 0);
        r.canvas.endDrag();
        CHECK(r.undo() == before + 1);
        const GraphicLayer& l = r.layer(1);
        CHECK(l.transform.anchorX.valueAt({}) == Approx(-100));
        CHECK(l.transform.anchorY.valueAt({}) == Approx(-50));
        CHECK(l.transform.x.valueAt({}) == Approx(380));
        CHECK(l.transform.y.valueAt({}) == Approx(220));
        const QVariantList quadAfter = quadOf(r.canvas.selection());
        REQUIRE(quadAfter.size() == quadBefore.size());
        for (int i = 0; i < quadBefore.size(); ++i)
            CHECK(quadAfter[i].toDouble() == Approx(quadBefore[i].toDouble()).margin(1e-6));
        const QVariantMap anchor = r.canvas.selection().value(QStringLiteral("anchor")).toMap();
        CHECK(anchor.value(QStringLiteral("x")).toDouble() == Approx(380 / 1920.0));
    }
    SECTION("An animated position gets a key at the playhead; the other keys stay") {
        Graphic g = lowerThird();
        Curve curve;
        curve.keys = {Keyframe{Ticks{}, 400.0}, Keyframe{seconds(1), 600.0}};
        g.layers[0].transform.x = ParamValue(curve);
        CanvasRig r(1920, 1080, g);
        r.canvas.setSelectedLayer(1);
        // At the playhead (frame 10, 0.4 s) the box is at 480 (linear); moved 60 px right, keyed there.
        REQUIRE(r.canvas.beginDrag(QStringLiteral("move"), r.fx(480), r.fy(270)));
        r.canvas.dragTo(r.fx(540), r.fy(270), false, true, 0);
        r.canvas.endDrag();
        const Curve* x = r.layer(1).transform.x.curve();
        REQUIRE(x);
        REQUIRE(x->keys.size() == 3);
        CHECK(x->keys[0].value == Approx(400));
        CHECK(x->keys[1].time == f(10));
        CHECK(x->keys[1].value == Approx(540));
        CHECK(x->keys[2].value == Approx(600));
        CHECK_FALSE(r.layer(1).transform.y.isCurve()); // only what moved
    }
    SECTION("Esc cancels a drag: nothing changes") {
        CanvasRig r;
        r.canvas.setSelectedLayer(1);
        const auto before = r.undo();
        REQUIRE(r.canvas.beginDrag(QStringLiteral("move"), r.fx(480), r.fy(270)));
        r.canvas.dragTo(r.fx(700), r.fy(400), false, true, 0);
        r.canvas.cancelDrag();
        CHECK(r.undo() == before);
        CHECK(r.x(1) == Approx(480));
    }
    SECTION("Keyboard: arrows nudge one pixel, one undo step each; Tab picks the next layer") {
        CanvasRig r;
        r.canvas.setSelectedLayer(1);
        const auto before = r.undo();
        REQUIRE(r.canvas.nudge(1, 0));
        REQUIRE(r.canvas.nudge(0, -10));
        CHECK(r.undo() == before + 2);
        CHECK(r.x(1) == Approx(481));
        CHECK(r.y(1) == Approx(260));
        r.canvas.selectAdjacent(1);
        CHECK(r.canvas.selectedLayer() == 2);
        r.canvas.selectAdjacent(-1);
        r.canvas.selectAdjacent(-1);
        CHECK(r.canvas.selectedLayer() == 3); // wraps
    }
}

TEST_CASE("Graphic canvas: moves snap to the canvas centre and other layers; Ctrl does not snap", "[app][graphics]") {
    Graphic g = lowerThird();
    g.layers[0].transform.x = ParamValue(950.0); // 10 px left of the centre
    g.layers[0].transform.y = ParamValue(600.0);
    CanvasRig r(1920, 1080, g);
    r.canvas.setSelectedLayer(1);
    int guideChanges = 0;
    QObject::connect(&r.canvas, &GraphicCanvasBridge::guidesChanged, [&] { ++guideChanges; });
    REQUIRE(r.canvas.beginDrag(QStringLiteral("move"), r.fx(950), r.fy(600)));
    r.canvas.dragTo(r.fx(951), r.fy(600), false, false, 0.01); // reach 19.2 px
    bool vertical = false;
    for (const QVariant& v : r.canvas.guides()) {
        const QVariantMap guide = v.toMap();
        if (guide.value(QStringLiteral("orientation")).toString() == QStringLiteral("vertical")
            && guide.value(QStringLiteral("position")).toDouble() == Approx(0.5))
            vertical = true;
    }
    CHECK(vertical);
    CHECK(guideChanges >= 1);
    r.canvas.endDrag();
    CHECK(r.canvas.guides().isEmpty());
    CHECK(r.x(1) == Approx(960));
    // Ctrl: exactly where the pointer took it.
    REQUIRE(r.canvas.beginDrag(QStringLiteral("move"), r.fx(960), r.fy(600)));
    r.canvas.dragTo(r.fx(951), r.fy(600), false, true, 0.01);
    r.canvas.endDrag();
    CHECK(r.x(1) == Approx(951));
    // Another layer's edge: the box's left edge (851) onto the paragraph's left edge (1200)? Too far; the name's left
    // edge (220) is within reach of the box's left edge moved to 210.
    REQUIRE(r.canvas.beginDrag(QStringLiteral("move"), r.fx(951), r.fy(600)));
    r.canvas.dragTo(r.fx(951 - 631), r.fy(600), false, false, 0.01); // left edge 851 - 631 = 220: on the name's edge
    r.canvas.endDrag();
    CHECK(r.x(1) - 100 == Approx(220));
}

TEST_CASE("Graphic canvas: the paragraph text box resizes from its edges; point text scales", "[app][graphics]") {
    CanvasRig r;
    r.canvas.setSelectedLayer(3);
    const QVariantMap selection = r.canvas.selection();
    CHECK(selection.value(QStringLiteral("paragraph")).toBool());
    CHECK(handle(selection, QStringLiteral("scale-br")).isEmpty());
    const QVariantMap right = handle(selection, QStringLiteral("box-right"));
    REQUIRE_FALSE(right.isEmpty());
    CHECK(right.value(QStringLiteral("x")).toDouble() == Approx(1600 / 1920.0));
    const auto before = r.undo();
    REQUIRE(r.canvas.beginDrag(QStringLiteral("box-right"), r.fx(1600), r.fy(230)));
    r.canvas.dragTo(r.fx(1700), r.fy(230), false, false, 0);
    r.canvas.endDrag();
    CHECK(r.undo() == before + 1);
    CHECK(r.layer(3).text.boxWidth == Approx(500));
    REQUIRE(r.canvas.beginDrag(QStringLiteral("box-bottom"), r.fx(1450), r.fy(260)));
    r.canvas.dragTo(r.fx(1450), r.fy(500), false, false, 0);
    r.canvas.endDrag();
    CHECK(r.layer(3).text.boxHeight == Approx(300));
    // Point text has the scale corners instead.
    r.canvas.setSelectedLayer(2);
    CHECK_FALSE(handle(r.canvas.selection(), QStringLiteral("scale-tl")).isEmpty());
    CHECK(handle(r.canvas.selection(), QStringLiteral("box-right")).isEmpty());
    CHECK_FALSE(r.canvas.beginDrag(QStringLiteral("box-right"), 0.1, 0.8));
}

TEST_CASE("Graphic canvas: a pinned layer's handles are locked", "[app][graphics]") {
    Graphic g = lowerThird();
    LayerPin pin;
    pin.target = 2;
    pin.edges = LayerPin::kLeft | LayerPin::kTop | LayerPin::kRight | LayerPin::kBottom;
    pin.offsets = {-24, -12, 24, 12};
    g.layers[0].pin = pin;
    CanvasRig r(1920, 1080, g);
    r.canvas.setSelectedLayer(1);
    CHECK(r.canvas.selection().value(QStringLiteral("pinned")).toBool());
    CHECK(r.canvas.selection().value(QStringLiteral("handles")).toList().isEmpty());
    CHECK_FALSE(r.canvas.beginDrag(QStringLiteral("move"), 0.1, 0.8));
    CHECK_FALSE(r.canvas.message().isEmpty());
    CHECK_FALSE(r.canvas.nudge(1, 0));
}

TEST_CASE("Graphic canvas: inline text, Thai included, one undo step", "[app][graphics]") {
    CanvasRig r;
    const auto before = r.undo();
    const QString thai = QStringLiteral("สวัสดีครับ ที่นี่");
    CHECK(r.canvas.setLayerText(2, thai).isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(QString::fromStdString(r.layer(2).text.text()) == thai);
    REQUIRE(r.layer(2).text.runs.size() == 1);
    CHECK(r.layer(2).text.runs[0].style.fontSize == Approx(64)); // the first run's style
    // The editor's paragraph separators become line breaks; the same text again is no step.
    CHECK(r.canvas.setLayerText(2, QStringLiteral("One") + QChar(0x2029) + QStringLiteral("Two")).isEmpty());
    CHECK(QString::fromStdString(r.layer(2).text.text()) == QStringLiteral("One\nTwo"));
    const auto steps = r.undo();
    CHECK(r.canvas.setLayerText(2, QStringLiteral("One\nTwo")).isEmpty());
    CHECK(r.undo() == steps);
    CHECK_FALSE(r.canvas.setLayerText(1, QStringLiteral("not text")).isEmpty());
}

TEST_CASE("Program Monitor picking: a click picks the topmost object, keeps the one picked last, and reaches the clip below",
          "[app][graphics][picking]") {
    CanvasRig r;
    const QPointF box = layerCentre(r.canvas, 1);
    r.controller.setPlayhead(f(5)); // A on V1 shows too (frames 0-9)
    const ClipId a = r.tl.clip("V1@0");
    const QString aKey = clipKey(a);
    const QString graphicKey = clipKey(r.clip);
    const QString boxKey = graphicKey + QStringLiteral("/1");

    // Under the box: the box layer of the graphic on V2, then A's whole frame on V1.
    const QVariantList stack = r.canvas.stackAt(box.x(), box.y());
    REQUIRE(stack.size() == 2);
    CHECK(keyOf(stack[0]) == boxKey);
    CHECK(stack[0].toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("shape"));
    CHECK(keyOf(stack[1]) == aKey);
    CHECK(stack[1].toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("clip"));
    checkQuad(stack[1].toMap().value(QStringLiteral("quad")).toList(), {0, 0, 1, 0, 1, 1, 0, 1});

    // Where only A shows, a click picks A - the timeline's selection - and outlines it.
    CHECK(keyOf(r.canvas.pickPress(0.03, 0.05)) == aKey);
    CHECK(r.controller.primaryClip() == a);
    CHECK(r.canvas.pickKey() == aKey);
    checkQuad(r.canvas.pickOutline(), {0, 0, 1, 0, 1, 1, 0, 1});

    // On the box, A - picked last and under the pointer - stays picked (it can be dragged from behind the box).
    CHECK(keyOf(r.canvas.pickTarget(box.x(), box.y())) == aKey);
    CHECK(keyOf(r.canvas.pickPress(box.x(), box.y())) == aKey);
    CHECK(r.controller.primaryClip() == a);

    // A selection made in the timeline is not a pick: the click takes the topmost object, the box layer.
    r.controller.selectClip(graphicKey);
    CHECK(r.canvas.pickOutline().isEmpty());
    CHECK(keyOf(r.canvas.pickPress(box.x(), box.y())) == boxKey);
    CHECK(r.controller.primaryClip() == r.clip);
    CHECK(r.canvas.selectedLayer() == 1);
    CHECK(r.canvas.pickKey() == boxKey);
    CHECK(r.canvas.pickOutline().isEmpty()); // a layer has its handles instead

    // Nothing under the pointer: the layer's handles go, the timeline keeps its selection.
    CHECK(r.canvas.pickPress(1.2, 0.5).isEmpty());
    CHECK(r.canvas.selectedLayer() == 0);
    CHECK(r.controller.primaryClip() == r.clip);
}

TEST_CASE("Program Monitor picking: a double-click edits a text layer that was not selected", "[app][graphics][picking]") {
    CanvasRig r;
    const QPointF name = layerCentre(r.canvas, 2);
    const QPointF boxAt = layerCentre(r.canvas, 1);
    r.controller.setPlayhead(f(5));
    r.controller.setSelection({});
    REQUIRE_FALSE(r.canvas.active());

    const QVariantMap result = doubleClick(r.canvas, name);
    CHECK(result.value(QStringLiteral("action")).toString() == QStringLiteral("edit"));
    CHECK(result.value(QStringLiteral("kind")).toString() == QStringLiteral("text"));
    CHECK(result.value(QStringLiteral("layer")).toInt() == 2);
    CHECK(r.controller.primaryClip() == r.clip);
    CHECK(r.canvas.selectedLayer() == 2);
    REQUIRE(r.canvas.active());
    CHECK(r.canvas.selection().value(QStringLiteral("id")).toInt() == 2);
    CHECK(r.canvas.selection().value(QStringLiteral("editor")).toMap().value(QStringLiteral("text")).toString() ==
          QStringLiteral("Somchai"));

    // A shape is picked, not edited.
    r.controller.setSelection({});
    const QVariantMap box = doubleClick(r.canvas, boxAt);
    CHECK(box.value(QStringLiteral("action")).toString() == QStringLiteral("select"));
    CHECK(box.value(QStringLiteral("layer")).toInt() == 1);
}

TEST_CASE("Program Monitor picking: each double-click goes one object deeper and wraps; text is edited on the way",
          "[app][graphics][picking]") {
    Graphic g;
    g.name = "Stack";
    g.layers = {rectLayer(1, 600, 300, 960, 540), textLayer(2, "Overlap", 960, 540)};
    CanvasRig r(1920, 1080, std::move(g));
    const QPointF at = layerCentre(r.canvas, 2);
    r.controller.setPlayhead(f(5));
    const ClipId a = r.tl.clip("V1@0");
    const QString text = clipKey(r.clip) + QStringLiteral("/2");
    const QString box = clipKey(r.clip) + QStringLiteral("/1");
    const QString aKey = clipKey(a);

    // Three deep: the text over the box over A.
    const QVariantList stack = r.canvas.stackAt(at.x(), at.y());
    REQUIRE(stack.size() == 3);
    CHECK(keyOf(stack[0]) == text);
    CHECK(keyOf(stack[1]) == box);
    CHECK(keyOf(stack[2]) == aKey);

    r.controller.setSelection({});
    const auto step = [&](const QString& action, const QString& key) {
        const QVariantMap result = doubleClick(r.canvas, at);
        CHECK(result.value(QStringLiteral("action")).toString() == action);
        CHECK(keyOf(result) == key);
        CHECK(r.canvas.pickKey() == key);
    };
    step(QStringLiteral("edit"), text);   // the topmost is text: edited at once
    step(QStringLiteral("select"), box);  // then the object behind it
    step(QStringLiteral("select"), aKey); // and behind that, the clip on V1
    CHECK(r.controller.primaryClip() == a);
    checkQuad(r.canvas.pickOutline(), {0, 0, 1, 0, 1, 1, 0, 1});
    step(QStringLiteral("select"), text); // back to the top: picked, not edited
    CHECK(r.canvas.pickOutline().isEmpty());
    step(QStringLiteral("edit"), text);   // the next double-click edits it
    step(QStringLiteral("select"), box);

    // A selection made elsewhere starts over: the next double-click edits the topmost text again.
    r.controller.selectClip(aKey);
    step(QStringLiteral("edit"), text);
}

TEST_CASE("Program Monitor picking: a title is picked by its text's box and edited in place", "[app][graphics][picking]") {
    Timeline tl(kPicture);
    const auto added = edit::addTitle(tl.state, tl.seq, tl.track("V2"), Ticks{}, seconds(2), "Your Title");
    REQUIRE(added);
    const ClipId title = added->placed.at(0);
    const ClipId a = tl.clip("V1@0");
    ProjectController controller;
    GraphicCanvasBridge canvas;
    controller.resetProject(added->state, tl.seq);
    canvas.setSynchronous(true);
    canvas.setController(&controller);
    controller.setPlayhead(f(5));

    // The title's text is centred in the frame; the rest of the picture is A's.
    const QVariantList stack = canvas.stackAt(0.5, 0.5);
    REQUIRE(stack.size() == 2);
    CHECK(keyOf(stack[0]) == clipKey(title));
    CHECK(stack[0].toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("title"));
    CHECK(keyOf(stack[1]) == clipKey(a));
    CHECK(keyOf(canvas.pickTarget(0.02, 0.02)) == clipKey(a));

    const QVariantMap result = doubleClick(canvas, QPointF(0.5, 0.5));
    CHECK(result.value(QStringLiteral("action")).toString() == QStringLiteral("edit"));
    CHECK(result.value(QStringLiteral("kind")).toString() == QStringLiteral("title"));
    const QVariantMap editor = result.value(QStringLiteral("editor")).toMap();
    CHECK(editor.value(QStringLiteral("text")).toString() == QStringLiteral("Your Title"));
    CHECK(editor.value(QStringLiteral("width")).toDouble() > 0.1);
    CHECK(editor.value(QStringLiteral("width")).toDouble() < 0.9);
    CHECK(editor.value(QStringLiteral("x")).toDouble() > 0.05);
    CHECK(controller.primaryClip() == title);
    CHECK_FALSE(canvas.pickOutline().isEmpty());

    // The inline editor's text goes to the title: one undo step; the same text again is none.
    const auto before = controller.session().undoCount();
    CHECK(canvas.setTitleText(clipKey(title), QStringLiteral("Hello")).isEmpty());
    CHECK(controller.session().undoCount() == before + 1);
    const auto textOf = [&] {
        const ProjectSnapshot snap = controller.snapshot();
        const MediaItem* item = snap->findMedia(snap->findSequence(tl.seq)->findClip(title)->clip().media);
        REQUIRE(item);
        return item->generator->text;
    };
    CHECK(textOf() == "Hello");
    CHECK(canvas.setTitleText(clipKey(title), QStringLiteral("Hello")).isEmpty());
    CHECK(controller.session().undoCount() == before + 1);
    CHECK_FALSE(canvas.setTitleText(clipKey(a), QStringLiteral("not a title")).isEmpty());
}

TEST_CASE("Graphic canvas in the window: resolved off the GUI thread, handles, guides and the inline Thai editor; screenshot",
          "[app][graphics][qml][screenshot]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* canvas = shell.engine.singletonInstance<GraphicCanvasBridge*>("Wabcut", "GraphicCanvas");
    REQUIRE(controller);
    REQUIRE(canvas);
    Timeline tl(kPicture);
    Graphic g = lowerThird();
    g.layers[1].transform.x = ParamValue(220.0);
    g.layers[1].transform.y = ParamValue(820.0);
    const auto added = edit::addGraphic(tl.state, tl.seq, tl.track("V2"), Ticks{}, seconds(2), std::move(g));
    REQUIRE(added);
    const ClipId clip = added->placed.at(0);
    controller->resetProject(added->state, tl.seq);
    controller->selectClip(clipKey(clip));
    controller->setPlayhead(seconds(0.5));
    REQUIRE(waitFor([&] { return canvas->active(); }, 5000));
    CHECK(canvas->lastResolveOffGuiThread()); // the resolve ran on a worker
    const int resolves = canvas->resolveCount();
    canvas->setSelectedLayer(2);

    // The overlay over the program monitor's picture (Main's, or one made here the same way).
    QQuickItem* frame = findNamed(shell.window->contentItem(), QStringLiteral("programFrameArea"));
    REQUIRE(frame);
    std::unique_ptr<QQuickItem> made;
    QQuickItem* overlay = findNamed(frame, QStringLiteral("programGraphicOverlay"));
    if (!overlay) {
        made = makeItem(shell, "import Wabcut\nGraphicCanvasOverlay {}\n", frame);
        overlay = made.get();
    }
    REQUIRE(waitFor([&] { return overlay->isVisible(); }, 3000));
    REQUIRE(findNamed(overlay, QStringLiteral("graphicHandle_scale-br")));
    REQUIRE(findNamed(overlay, QStringLiteral("graphicHandle_rotate")));
    REQUIRE(findNamed(overlay, QStringLiteral("graphicHandle_anchor")));

    // A move snapping to the action-safe margin draws its guide: the screenshot catches it mid-drag.
    const double left = 220.0 / 1920.0;
    REQUIRE(canvas->beginDrag(QStringLiteral("move"), left, 820.0 / 1080.0));
    canvas->dragTo(left - 0.02, 820.0 / 1080.0, false, false, 0.01);
    spin(50);
    CHECK_FALSE(canvas->guides().isEmpty());
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    const double dpr = image.devicePixelRatio();
    const QRectF area = frame->mapRectToScene(QRectF(0, 0, frame->width(), frame->height()));
    REQUIRE(image.copy(QRectF(area.x() * dpr, area.y() * dpr, area.width() * dpr, area.height() * dpr).toRect())
                .save(QDir(outputDir()).filePath(QStringLiteral("graphics-canvas-handles.png"))));
    canvas->endDrag();
    REQUIRE(waitFor([&] { return canvas->resolveCount() > resolves; }, 5000));

    // Inline editing: Enter on the focused overlay opens the editor over the name; the caret steps over a Thai
    // consonant with its marks at once (grapheme clusters); Enter keeps the text (one undo step).
    overlay->forceActiveFocus();
    REQUIRE(QMetaObject::invokeMethod(overlay, "startEditing"));
    QQuickItem* editor = findNamed(overlay, QStringLiteral("graphicTextEditor"));
    QQuickItem* edit = findNamed(overlay, QStringLiteral("graphicTextEdit"));
    REQUIRE(editor);
    REQUIRE(edit);
    REQUIRE(waitFor([&] { return editor->isVisible() && edit->hasActiveFocus(); }, 3000));
    CHECK(edit->property("text").toString() == QStringLiteral("Somchai"));
    const QString thai = QStringLiteral("ที่นี่"); // ท + ี + ่ twice: two clusters of three code units
    edit->setProperty("text", thai);
    edit->setProperty("cursorPosition", 0);
    key(shell.window.get(), Qt::Key_Right);
    CHECK(edit->property("cursorPosition").toInt() == 3);
    key(shell.window.get(), Qt::Key_Right);
    CHECK(edit->property("cursorPosition").toInt() == 6);
    const auto before = controller->session().undoCount();
    key(shell.window.get(), Qt::Key_Return);
    REQUIRE(waitFor([&] { return !editor->isVisible(); }, 3000));
    CHECK(controller->session().undoCount() == before + 1);
    const ProjectSnapshot snap = controller->snapshot();
    const Graphic* now = snap->findSequence(tl.seq)->findClip(clip)->clip().graphic();
    REQUIRE(now);
    CHECK(QString::fromStdString(now->findLayer(2)->text.text()) == thai);
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Timeline: a responsive graphic's protected intro and outro are drawn on its clip", "[app][graphics][timeline]") {
    Rig rig(R"(
        V2     |..........|
        V1 tgt |Aaaaaaaaaa|
    )");
    // 10 px a frame: 250 px a second. A 3 s clip with a 1 s intro and a 1 s outro; a 1.5 s one squeezed (0.75 s each).
    Graphic responsive;
    responsive.layers = {rectLayer(1, 100, 100, 960, 540)};
    responsive.responsive = ResponsiveTime{seconds(4), seconds(1), seconds(1)};
    Graphic plain;
    plain.layers = {rectLayer(1, 100, 100, 960, 540)};
    ClipId wide;
    ClipId squeezed;
    ClipId flat;
    REQUIRE(rig.controller.applyEditFn("Graphics", [&](const ProjectState& state) -> edit::EditOutcome {
        auto a = edit::addGraphic(state, rig.tl.seq, rig.tl.track("V2"), Ticks{}, seconds(3), responsive);
        if (!a)
            return a;
        wide = a->placed.at(0);
        auto b = edit::addGraphic(a->state, rig.tl.seq, rig.tl.track("V2"), seconds(3.2), seconds(1.5), responsive);
        if (!b)
            return b;
        squeezed = b->placed.at(0);
        auto c = edit::addGraphic(b->state, rig.tl.seq, rig.tl.track("V2"), seconds(5), seconds(1), plain);
        if (!c)
            return c;
        flat = c->placed.at(0);
        edit::EditResult result = *c;
        result.changes = diff(state, c->state);
        return result;
    }));
    rig.view->setPixelsPerSecond(250.0);
    rig.harness.frame();
    QCoreApplication::processEvents();
    rig.harness.frame();
    const TimelineStats& st = rig.view->stats();
    CHECK(st.responsiveClips == 2);
    REQUIRE(st.responsiveBands.contains(wide.value));
    CHECK(st.responsiveBands.value(wide.value).first == Approx(250).margin(1));
    CHECK(st.responsiveBands.value(wide.value).second == Approx(250).margin(1));
    REQUIRE(st.responsiveBands.contains(squeezed.value));
    CHECK(st.responsiveBands.value(squeezed.value).first == Approx(187.5).margin(1));
    CHECK_FALSE(st.responsiveBands.contains(flat.value));
    CHECK(st.responsiveTags >= 2); // the wide clip's two bands are tagged
    // Undo takes them away again.
    REQUIRE(rig.controller.undo());
    rig.harness.frame();
    CHECK(rig.view->stats().responsiveClips == 0);
    CHECK(rig.view->stats().responsiveTags == 0);
}

TEST_CASE("Type tool (T): a click in the Program Monitor adds a text layer there and edits it in place; Ctrl+T still works",
          "[app][graphics][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* graphics = shell.engine.singletonInstance<GraphicsBridge*>("Wabcut", "Graphics");
    REQUIRE(controller);
    REQUIRE(graphics);
    REQUIRE_FALSE(shell.grab().isNull());

    // T through Main: the timeline's tool; the timeline view itself keeps the selection tool.
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("tool.type"))));
    QQuickItem* panel = shell.item("timelinePanel");
    auto* view = qobject_cast<TimelineView*>(shell.item("timelineView"));
    REQUIRE(panel);
    REQUIRE(view);
    CHECK(panel->property("tool").toString() == QStringLiteral("type"));
    CHECK(view->tool() == QStringLiteral("select"));
    QQuickItem* area = findNamed(shell.window->contentItem(), QStringLiteral("programTypeToolArea"));
    REQUIRE(area);
    REQUIRE(area->isVisible());
    CHECK(area->cursor().shape() == Qt::IBeamCursor);

    // A click a quarter in and 30 % down: a graphic clip at the playhead with a text layer anchored there, edited in place.
    const auto undo = controller->session().undoCount();
    const QPointF at = area->mapToScene(QPointF(area->width() * 0.25, area->height() * 0.3));
    QMouseEvent press(QEvent::MouseButtonPress, at, at, Qt::LeftButton, Qt::LeftButton, {});
    QCoreApplication::sendEvent(shell.window.get(), &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, at, Qt::LeftButton, Qt::NoButton, {});
    QCoreApplication::sendEvent(shell.window.get(), &release);
    // 0.2: creating the graphic and placing its text is ONE undo step (Premiere).
    CHECK(controller->session().undoCount() == undo + 1);
    const int layer = graphics->selectedLayer();
    REQUIRE(layer > 0);
    REQUIRE(controller->selection().size() == 1);
    const ClipId clip = controller->selection().front();
    const auto layerNow = [&]() -> const GraphicLayer* {
        const Graphic* g = controller->currentSequence()->findClip(clip)->clip().graphic();
        return g ? g->findLayer(static_cast<std::uint32_t>(layer)) : nullptr;
    };
    REQUIRE(layerNow());
    CHECK(layerNow()->kind == LayerKind::Text);
    CHECK(layerNow()->transform.x.constant() == Approx(std::round(0.25 * controller->frameWidth())));
    CHECK(layerNow()->transform.y.constant() == Approx(std::round(0.3 * controller->frameHeight())));
    QQuickItem* editor = findNamed(shell.window->contentItem(), QStringLiteral("graphicTextEditor"));
    QQuickItem* edit = findNamed(shell.window->contentItem(), QStringLiteral("graphicTextEdit"));
    REQUIRE(editor);
    REQUIRE(edit);
    REQUIRE(waitFor([&] { return editor->isVisible() && edit->hasActiveFocus(); }, 5000));
    CHECK_FALSE(area->isVisible()); // no second text while one is edited

    // Typing and Enter keep the text.
    edit->setProperty("text", QStringLiteral("GG EZ"));
    key(shell.window.get(), Qt::Key_Return);
    REQUIRE(waitFor([&] { return !editor->isVisible(); }, 3000));
    REQUIRE(layerNow());
    CHECK(QString::fromStdString(layerNow()->text.text()) == QStringLiteral("GG EZ"));

    // Ctrl+T (graphics.newTextLayer) still adds a centred text layer to the selected graphic.
    const auto layers = [&] { return controller->currentSequence()->findClip(clip)->clip().graphic()->layers.size(); };
    const std::size_t before = layers();
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("graphics.newTextLayer"))));
    CHECK(layers() == before + 1);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Program Monitor in the window: a double-click on a title edits its text in place; screenshot",
          "[app][graphics][picking][qml][screenshot]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* canvas = shell.engine.singletonInstance<GraphicCanvasBridge*>("Wabcut", "GraphicCanvas");
    REQUIRE(controller);
    REQUIRE(canvas);
    Timeline tl(kPicture);
    const auto added = edit::addTitle(tl.state, tl.seq, tl.track("V2"), Ticks{}, seconds(2), "Your Title");
    REQUIRE(added);
    const ClipId title = added->placed.at(0);
    controller->resetProject(added->state, tl.seq);
    controller->setPlayhead(f(5));
    REQUIRE_FALSE(shell.grab().isNull());

    QQuickItem* area = findNamed(shell.window->contentItem(), QStringLiteral("programPickArea"));
    REQUIRE(area);
    REQUIRE(area->isVisible());
    REQUIRE(area->width() > 0);
    // The platform's sequence at the title's centre: press, release, press, double-click, release.
    const QPointF at = area->mapToScene(QPointF(area->width() * 0.5, area->height() * 0.5));
    const auto send = [&](QEvent::Type type, Qt::MouseButtons buttons) {
        QMouseEvent event(type, at, at, Qt::LeftButton, buttons, {});
        QCoreApplication::sendEvent(shell.window.get(), &event);
    };
    send(QEvent::MouseButtonPress, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, Qt::NoButton);
    send(QEvent::MouseButtonPress, Qt::LeftButton);
    send(QEvent::MouseButtonDblClick, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, Qt::NoButton);
    CHECK(controller->primaryClip() == title);

    QQuickItem* editor = findNamed(shell.window->contentItem(), QStringLiteral("titleTextEditor"));
    QQuickItem* edit = findNamed(shell.window->contentItem(), QStringLiteral("titleTextEdit"));
    REQUIRE(editor);
    REQUIRE(edit);
    REQUIRE(waitFor([&] { return editor->isVisible() && edit->hasActiveFocus(); }, 5000));
    CHECK(edit->property("text").toString() == QStringLiteral("Your Title"));
    QQuickItem* frame = findNamed(shell.window->contentItem(), QStringLiteral("programFrameArea"));
    REQUIRE(frame);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    const double dpr = image.devicePixelRatio();
    const QRectF rect = frame->mapRectToScene(QRectF(0, 0, frame->width(), frame->height()));
    REQUIRE(image.copy(QRectF(rect.x() * dpr, rect.y() * dpr, rect.width() * dpr, rect.height() * dpr).toRect())
                .save(QDir(outputDir()).filePath(QStringLiteral("program-monitor-title-edit.png"))));

    // Typing and Enter keep the text: the title shows it.
    const auto before = controller->session().undoCount();
    edit->setProperty("text", QStringLiteral("GG EZ"));
    key(shell.window.get(), Qt::Key_Return);
    REQUIRE(waitFor([&] { return !editor->isVisible(); }, 3000));
    CHECK(controller->session().undoCount() == before + 1);
    const ProjectSnapshot snap = controller->snapshot();
    const MediaItem* item = snap->findMedia(snap->findSequence(tl.seq)->findClip(title)->clip().media);
    REQUIRE(item);
    CHECK(item->generator->text == "GG EZ");

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
