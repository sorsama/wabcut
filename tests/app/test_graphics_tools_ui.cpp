// 0.2 graphics in the app, offscreen: type on a path (Essential Graphics' Path row and the Program Monitor's path
// points), Align and Distribute on several selected layers, Text Styles as Library items and the caption Track Style
// menu - each change one undo step.

#include "BridgeIds.h"
#include "CaptionsBridge.h"
#include "GraphicCanvasBridge.h"
#include "GraphicToolsBridge.h"
#include "GraphicsBridge.h"
#include "ProjectController.h"
#include "ShellFixture.h"
#include "TimelineFixture.h"
#include "TranscriptBridge.h"

#include <wabcut/model/CaptionOps.h>
#include <wabcut/model/CaptionStyle.h>
#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/Graphics.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QQmlComponent>

#include <memory>

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

GraphicLayer box(std::uint32_t id, double x, double y) {
    GraphicLayer l;
    l.id = id;
    l.name = "Box " + std::to_string(id);
    l.kind = LayerKind::Shape;
    l.shape.width = ParamValue(100.0);
    l.shape.height = ParamValue(100.0);
    l.transform.x = ParamValue(x);
    l.transform.y = ParamValue(y);
    return l;
}

GraphicLayer text(std::uint32_t id, std::string value, double x, double y) {
    GraphicLayer l;
    l.id = id;
    l.name = "Text " + std::to_string(id);
    l.kind = LayerKind::Text;
    TextStyle style;
    style.fontSize = 48.0;
    l.text.runs = {TextRun{std::move(value), style}};
    l.transform.x = ParamValue(x);
    l.transform.y = ParamValue(y);
    return l;
}

/// Three boxes and a text layer on V2 for 2 s, selected, the playhead at 0.4 s.
struct Rig {
    Timeline tl{kPicture};
    ProjectController controller;
    GraphicsBridge graphics;
    GraphicToolsBridge tools;
    GraphicCanvasBridge canvas;
    ClipId clip;

    Rig() {
        Graphic g;
        g.layers = {box(1, 300, 300), box(2, 900, 500), box(3, 1200, 700), text(4, "Around the world", 960, 540)};
        auto added = edit::addGraphic(tl.state, tl.seq, tl.track("V2"), Ticks{}, seconds(2), g);
        REQUIRE(added);
        clip = added->placed.at(0);
        controller.resetProject(added->state, tl.seq);
        graphics.setController(&controller);
        tools.setController(&controller);
        canvas.setSynchronous(true);
        canvas.setController(&controller);
        controller.selectClip(clipKey(clip));
        controller.setPlayhead(f(10));
        REQUIRE(graphics.clip() == clipKey(clip));
    }
    const GraphicLayer& layer(std::uint32_t id) const {
        snap = controller.snapshot();
        const Graphic* g = snap->findSequence(tl.seq)->findClip(clip)->clip().graphic();
        REQUIRE(g);
        const GraphicLayer* l = g->findLayer(id);
        REQUIRE(l);
        return *l;
    }
    std::size_t undo() { return controller.session().undoCount(); }
    mutable ProjectSnapshot snap;
};

} // namespace

TEST_CASE("Type on a path from Essential Graphics and the Program Monitor's path points", "[app][graphics][textpath]") {
    Rig rig;
    auto before = rig.undo();
    CHECK(rig.graphics.setTextPath(4, QStringLiteral("arc")) == QString());
    CHECK(rig.undo() == before + 1);
    REQUIRE(rig.layer(4).text.path);
    CHECK(rig.layer(4).text.path->path.front().vertices.size() == 3);
    rig.graphics.setSelectedLayer(4);
    const QVariantMap onPath = rig.graphics.layer().value(QStringLiteral("text")).toMap().value(QStringLiteral("onPath")).toMap();
    CHECK(onPath.value(QStringLiteral("enabled")).toBool());
    CHECK(onPath.value(QStringLiteral("perpendicular")).toBool());
    // Options and a margin (an animatable number).
    CHECK(rig.graphics.setTextPathOption(4, QStringLiteral("reverse"), true) == QString());
    CHECK(rig.layer(4).text.path->reverse);
    CHECK(rig.graphics.setValue(4, QStringLiteral("text.onPath.firstMargin"), 120.0) == QString());
    rig.graphics.endEdit();
    CHECK(rig.layer(4).text.path->firstMargin.valueAt({}) == Approx(120.0));

    // The monitor: the selected layer shows its path and a handle on each point; dragging one moves it (one undo step).
    rig.canvas.setSelectedLayer(4);
    const QVariantMap selection = rig.canvas.selection();
    CHECK(selection.value(QStringLiteral("textPath")).toList().size() > 20);
    int vertexHandles = 0;
    QVariantMap first;
    for (const QVariant& h : selection.value(QStringLiteral("handles")).toList()) {
        const QVariantMap m = h.toMap();
        if (m.value(QStringLiteral("kind")).toString().startsWith(QStringLiteral("pathVertex-"))) {
            if (vertexHandles++ == 0)
                first = m;
        }
    }
    REQUIRE(vertexHandles == 3);
    const double x0 = rig.layer(4).text.path->path.front().vertices[0].x;
    before = rig.undo();
    REQUIRE(rig.canvas.beginDrag(QStringLiteral("pathVertex-0"), first.value(QStringLiteral("x")).toDouble(),
                                 first.value(QStringLiteral("y")).toDouble()));
    rig.canvas.dragTo(first.value(QStringLiteral("x")).toDouble() - 0.05, first.value(QStringLiteral("y")).toDouble(), false, true, 0.0);
    rig.canvas.dragTo(first.value(QStringLiteral("x")).toDouble() - 0.10, first.value(QStringLiteral("y")).toDouble(), false, true, 0.0);
    rig.canvas.endDrag();
    CHECK(rig.undo() == before + 1);
    CHECK(rig.layer(4).text.path->path.front().vertices[0].x == Approx(x0 - 0.10 * 1920.0).margin(1.0));
    // Back on straight lines.
    CHECK(rig.graphics.setTextPath(4, QStringLiteral("none")) == QString());
    CHECK_FALSE(rig.layer(4).text.path);
}

TEST_CASE("Align and Distribute act on the layers Ctrl-clicked into the selection", "[app][graphics][align]") {
    Rig rig;
    rig.graphics.setSelectedLayer(1);
    rig.graphics.toggleLayerSelected(2);
    rig.graphics.toggleLayerSelected(3);
    CHECK(rig.graphics.selectedLayers() == QVariantList{1, 2, 3});
    // Align to the selection: every box's top to the topmost (300 - 50).
    const auto before = rig.undo();
    CHECK(rig.tools.align(rig.graphics.clip(), rig.graphics.selectedLayers(), QStringLiteral("top"), true) == QString());
    CHECK(rig.undo() == before + 1);
    CHECK(rig.layer(2).transform.y.valueAt({}) == Approx(300.0));
    CHECK(rig.layer(3).transform.y.valueAt({}) == Approx(300.0));
    // Taking the selected one out passes the selection on; a plain select drops the others.
    rig.graphics.toggleLayerSelected(1);
    CHECK(rig.graphics.selectedLayer() == 2);
    CHECK(rig.graphics.selectedLayers() == QVariantList{2, 3});
    rig.graphics.setSelectedLayer(4);
    CHECK(rig.graphics.selectedLayers() == QVariantList{4});
}

TEST_CASE("Text Styles are Library items: apply to the selected graphics, duplicate, rename, delete", "[app][graphics][textStyles]") {
    Rig rig;
    CHECK(rig.tools.createTextStyle(rig.graphics.clip(), 4, QStringLiteral("Title")) == QString());
    QVariantList items = rig.tools.textStyleItems();
    REQUIRE(items.size() == 1);
    CHECK(items[0].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Title"));
    CHECK(items[0].toMap().value(QStringLiteral("uses")).toInt() == 1);
    CHECK(items[0].toMap().value(QStringLiteral("fontSize")).toDouble() == Approx(48.0));
    // Duplicate under a free name.
    CHECK(rig.tools.duplicateTextStyle(QStringLiteral("Title")) == QStringLiteral("Title copy"));
    CHECK(rig.tools.textStyleItems().size() == 2);
    // Apply to the selected graphic clip: its text layer follows the copy; one undo step.
    const auto before = rig.undo();
    CHECK(rig.tools.applyTextStyleToSelection(QStringLiteral("Title copy")) == QString());
    CHECK(rig.undo() == before + 1);
    CHECK(rig.layer(4).textStyle == "Title copy");
    // Without a graphic selected it says so.
    rig.controller.clearSelection();
    CHECK(rig.tools.applyTextStyleToSelection(QStringLiteral("Title")) != QString());
    CHECK(rig.tools.renameTextStyle(QStringLiteral("Title copy"), QStringLiteral("Heading")) == QString());
    CHECK(rig.layer(4).textStyle == "Heading");
    CHECK(rig.tools.removeTextStyle(QStringLiteral("Heading")) == QString());
    CHECK(rig.tools.textStyleItems().size() == 1);
}

// The QML singleton: Qt 6.11 constructs GraphicTools itself (it is default-constructible, so create() never runs) and the
// bridge has to find the shell's ProjectController on its own - else every tool said "No project".
TEST_CASE("GraphicTools in the shell works on the open project", "[app][graphics][textStyles][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(controller);
    Timeline tl{kPicture};
    Graphic g;
    g.layers = {text(4, "Around the world", 960, 540)};
    auto added = edit::addGraphic(tl.state, tl.seq, tl.track("V2"), Ticks{}, seconds(2), g);
    REQUIRE(added);
    const ClipId clip = added->placed.at(0);
    controller->resetProject(added->state, tl.seq);
    controller->selectClip(clipKey(clip));

    auto* tools = shell.engine.singletonInstance<GraphicToolsBridge*>("Wabcut", "GraphicTools");
    REQUIRE(tools);
    // On first use (no event loop yet): the action reaches the project, one undo step.
    const auto before = controller->session().undoCount();
    CHECK(tools->createTextStyle(clipKey(clip), 4, QStringLiteral("Title")) == QString());
    CHECK(tools->message().isEmpty());
    CHECK(controller->session().undoCount() == before + 1);
    CHECK(tools->textStyles() == QStringList{QStringLiteral("Title")});
    // QML sees the project's text styles, and the Library's Text Style items act on the selected graphic.
    QQmlComponent component(&shell.engine);
    component.setData("import QtQuick\nimport Wabcut\nQtObject { property var styles: GraphicTools.textStyles\n"
                      "property var items: GraphicTools.textStyles.length >= 0 ? GraphicTools.textStyleItems() : [] }\n",
                      QUrl());
    std::unique_ptr<QObject> probe(component.create());
    INFO(component.errorString().toStdString());
    REQUIRE(probe);
    CHECK(probe->property("styles").toStringList() == QStringList{QStringLiteral("Title")});
    CHECK(probe->property("items").toList().size() == 1);
    CHECK(tools->duplicateTextStyle(QStringLiteral("Title")) == QStringLiteral("Title copy"));
    spin(20);
    CHECK(probe->property("styles").toStringList() == QStringList{QStringLiteral("Title"), QStringLiteral("Title copy")});
    CHECK(tools->applyTextStyleToSelection(QStringLiteral("Title copy")) == QString());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("The Caption Style panel's Track Style follows a preset and pushes to it", "[app][captions][trackStyle]") {
    Timeline tl{kPicture};
    ProjectState s = tl.state;
    auto track = edit::addCaptionTrack(s, tl.seq, "English", "en", CaptionStyle{});
    REQUIRE(track);
    s = track->state;
    auto preset = edit::saveCaptionPreset(s, CaptionPreset{"Channel", CaptionStyle{}});
    REQUIRE(preset);
    s = *preset;
    ProjectController controller;
    controller.resetProject(s, tl.seq);
    TranscriptBridge transcripts;
    transcripts.setUseAnalysisCache(false);
    transcripts.setController(&controller);
    CaptionsBridge captions;
    captions.setController(&controller);
    captions.setTranscript(&transcripts);
    CHECK(captions.trackInfo().value(QStringLiteral("trackStyle")).toString().isEmpty());
    auto before = controller.session().undoCount();
    CHECK(captions.setTrackStyle(QStringLiteral("Channel")) == QString());
    CHECK(controller.session().undoCount() == before + 1);
    CHECK(captions.trackInfo().value(QStringLiteral("trackStyle")).toString() == QStringLiteral("Channel"));
    // Push: the preset takes the track's look.
    CHECK(captions.setStyle(QStringLiteral("fontWeight"), 800) == QString());
    captions.endStyleEdit();
    before = controller.session().undoCount();
    CHECK(captions.pushTrackStyle() == QString());
    CHECK(controller.session().undoCount() == before + 1);
    CHECK(controller.snapshot()->findCaptionPreset("Channel")->style.fontWeight == 800);
    CHECK(captions.setTrackStyle(QString()) == QString());
    CHECK(captions.trackInfo().value(QStringLiteral("trackStyle")).toString().isEmpty());
}
