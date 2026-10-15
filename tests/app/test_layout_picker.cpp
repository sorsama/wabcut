// M5o (gaming): the layout picker (bridge `Layouts`) on the core's C.15 layout.apply, layout.list, sequence.presets and
// sequence.create, through a stub catalog with those commands (C.15 is not on master yet): the options as layout.apply's
// parameters, the dry run and Apply (one undo step), the facecam auto-pick, the facecam region drawn on the monitor, the
// schematics, the Shorts sequence, the unavailable state; in the window: the cards, the accessibility audit and a
// screenshot. Offscreen; no window is shown.

#include "A11yAudit.h"
#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "BridgeIds.h"
#include "LayoutBridge.h"
#include "ProjectController.h"

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQuickItem>
#include <QUrl>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using command::Json;
using Catch::Approx;

namespace {

/// The gameplay on V1 (and A1), a webcam clip on V2 over part of it.
constexpr std::string_view kGame = R"(
    V2     |.....Ffffffffff...............|
    V1 tgt |Gggggggggggggggggggggggggggggg|
    A1 tgt |Gggggggggggggggggggggggggggggg|
)";

/// layout.apply's parameters of the last call (dry runs too).
Json g_lastApply;

/// layout.apply, layout.list, sequence.presets and sequence.create as C.15 describes them; layout.apply marks the
/// gameplay with a label colour per preset instead of the real Motion and effects.
command::CommandRegistry stubCatalog() {
    command::CommandRegistry registry;
    command::CommandSpec list;
    list.name = "layout.list";
    list.title = "Layouts";
    list.description = "Stub";
    list.capability = command::Capability::Read;
    list.params = Json{{"type", "object"}};
    list.execute = [](command::ExecutionContext&) -> command::CommandOutput {
        command::CommandOutput out;
        out.data = Json{{"layouts", Json::array({Json{{"preset", "pip"}, {"needsFacecam", true}, {"description", "x"}},
                                                 Json{{"preset", "verticalSplit"}, {"needsFacecam", true}, {"description", "x"}},
                                                 Json{{"preset", "verticalBlurFill"}, {"needsFacecam", false}, {"description", "x"}}})},
                        {"defaults", Json{{"corner", "bottomRight"}, {"size", 0.28}, {"margin", 0.03}, {"circle", false},
                                          {"cornerRadius", 0.12}, {"borderWidth", 4}, {"borderColor", Json::array({1.0, 1.0, 1.0})},
                                          {"shadow", true}, {"split", 0.6}, {"facecamOnTop", false}, {"blur", 40}, {"zoom", 1.0}}}};
        return out;
    };
    REQUIRE(registry.add(std::move(list)));

    command::CommandSpec apply;
    apply.name = "layout.apply";
    apply.title = "Apply Layout";
    apply.description = "Stub";
    apply.capability = command::Capability::EditSequence;
    apply.params = Json{{"type", "object"}};
    apply.execute = [](command::ExecutionContext& c) -> command::CommandOutput {
        g_lastApply = c.params();
        const Json& p = c.params();
        const std::string preset = p.value("preset", std::string());
        const bool facecam = p.contains("facecam") || p.contains("facecamRegion");
        if ((preset == "pip" || preset == "verticalSplit") && !facecam)
            command::fail(command::ErrorCode::EditRejected, "pip and verticalSplit need a facecam or a facecamRegion");
        if (!p.contains("gameplay"))
            command::fail(command::ErrorCode::InvalidParams, "gameplay is required");
        const ClipId gameplay{p.at("gameplay").get<std::uint64_t>()};
        const SequenceId seq{p.at("sequence").get<std::uint64_t>()};
        edit::ClipAttributeChange change;
        change.colorIndex = preset == "pip" ? 1 : preset == "verticalSplit" ? 2 : 3;
        const std::vector<ClipId> clips{gameplay};
        edit::EditResult edited = c.take(edit::setClipAttributes(c.state(), seq, clips, change));
        command::CommandOutput out;
        out.state = std::move(edited.state);
        out.data = Json{{"preset", preset}, {"gameplay", gameplay.value}};
        return out;
    };
    REQUIRE(registry.add(std::move(apply)));

    command::CommandSpec presets;
    presets.name = "sequence.presets";
    presets.title = "Sequence Presets";
    presets.description = "Stub";
    presets.capability = command::Capability::Read;
    presets.params = Json{{"type", "object"}};
    presets.execute = [](command::ExecutionContext&) -> command::CommandOutput {
        command::CommandOutput out;
        out.data = Json{{"presets",
                         Json::array({Json{{"id", "1080p60"}, {"name", "1080p 60"}, {"group", "Landscape 16:9"}, {"width", 1920},
                                           {"height", 1080}, {"frameRate", Json{{"num", 60}, {"den", 1}, {"fps", 60.0}}}},
                                      Json{{"id", "shorts"}, {"name", "Shorts"}, {"group", "Vertical 9:16"}, {"width", 1080},
                                           {"height", 1920}, {"frameRate", Json{{"num", 60}, {"den", 1}, {"fps", 60.0}}}}})}};
        return out;
    };
    REQUIRE(registry.add(std::move(presets)));

    command::CommandSpec create;
    create.name = "sequence.create";
    create.title = "New Sequence";
    create.description = "Stub";
    create.capability = command::Capability::EditProject;
    create.params = Json{{"type", "object"}};
    create.execute = [](command::ExecutionContext& c) -> command::CommandOutput {
        edit::SequenceSettings settings;
        settings.name = c.params().value("name", std::string("Short"));
        settings.width = 1080;
        settings.height = 1920;
        auto created = edit::addSequence(c.state(), settings);
        if (!created)
            command::fail(command::ErrorCode::EditRejected, created.error().message);
        command::CommandOutput out;
        out.data = Json{{"sequence", created->id.value}, {"width", 1080}, {"height", 1920}};
        out.createdIds = {created->id.value};
        out.state = std::move(created->state);
        return out;
    };
    REQUIRE(registry.add(std::move(create)));
    return registry;
}

const command::CommandRegistry& catalog() {
    static const command::CommandRegistry registry = stubCatalog();
    return registry;
}

/// The bridge over a plain controller (no window), the gameplay selected.
struct Rig {
    Timeline tl{kGame};
    ProjectController controller;
    LayoutBridge layouts;

    Rig() {
        controller.resetProject(tl.state, tl.seq);
        layouts.setRegistry(&catalog());
        layouts.setSynchronous(true);
        layouts.setController(&controller);
        controller.setSelection({tl.clip("V1@0")});
    }
    std::size_t undo() { return controller.session().undoCount(); }
    std::int32_t gameplayColor() const { return controller.currentSequence()->findClip(tl.clip("V1@0"))->clip().colorIndex; }
};

QVariantMap opts(std::initializer_list<std::pair<const QString, QVariant>> values) { return QVariantMap(values); }

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

std::unique_ptr<QQuickItem> makeItem(Shell& shell, const QByteArray& qml, QRectF at) {
    QQmlComponent component(&shell.engine);
    component.setData(qml, QUrl(QStringLiteral("qrc:/m5o-layout/Host.qml")));
    std::unique_ptr<QObject> object(component.create());
    INFO(component.errorString().toStdString());
    REQUIRE(object);
    auto* item = qobject_cast<QQuickItem*>(object.get());
    REQUIRE(item);
    object.release();
    item->setParentItem(shell.window->contentItem());
    item->setPosition(at.topLeft());
    item->setSize(at.size());
    item->setZ(1000);
    return std::unique_ptr<QQuickItem>(item);
}

QRectF sceneRect(const QQuickItem* item) { return item->mapRectToScene(QRectF(0, 0, item->width(), item->height())); }

/// A left-button press, move or release at `scene`.
void mouse(QQuickWindow* window, QEvent::Type type, QPointF scene) {
    const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    const Qt::MouseButtons buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type, scene, scene, window->mapToGlobal(scene), button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &event);
}

} // namespace

TEST_CASE("Layout picker: the gameplay and the facecam are picked; options become clamped layout.apply parameters",
          "[app][layout]") {
    Rig r;
    CHECK(r.layouts.available());
    CHECK(r.layouts.gameplay() == clipKey(r.tl.clip("V1@0")));
    // The webcam on V2 overlaps the gameplay: picked.
    REQUIRE(r.layouts.facecamCandidates().size() == 1);
    CHECK(r.layouts.facecam() == clipKey(r.tl.clip("V2@5")));
    CHECK(r.layouts.facecamCandidates().front().toMap().value(QStringLiteral("track")).toString() == QStringLiteral("V2"));

    // Picture-in-picture: the facecam's look, nothing of the vertical layouts.
    QVariantMap p = r.layouts.params(opts({{QStringLiteral("preset"), QStringLiteral("pip")}}));
    CHECK(p.value(QStringLiteral("preset")).toString() == QStringLiteral("pip"));
    CHECK(p.value(QStringLiteral("gameplay")).toULongLong() == r.tl.clip("V1@0").value);
    CHECK(p.value(QStringLiteral("facecam")).toULongLong() == r.tl.clip("V2@5").value);
    CHECK(p.value(QStringLiteral("sequence")).toULongLong() == r.tl.seq.value);
    CHECK(p.value(QStringLiteral("corner")).toString() == QStringLiteral("bottomRight"));
    CHECK(p.value(QStringLiteral("size")).toDouble() == Approx(0.28));
    CHECK(p.value(QStringLiteral("margin")).toDouble() == Approx(0.03));
    CHECK(p.value(QStringLiteral("circle")).toBool() == false);
    CHECK(p.value(QStringLiteral("cornerRadius")).toDouble() == Approx(0.12));
    CHECK(p.value(QStringLiteral("borderWidth")).toDouble() == Approx(4));
    CHECK(p.value(QStringLiteral("borderColor")).toList() == QVariantList{1.0, 1.0, 1.0});
    CHECK(p.value(QStringLiteral("shadow")).toBool() == true);
    CHECK_FALSE(p.contains(QStringLiteral("split")));
    CHECK_FALSE(p.contains(QStringLiteral("blur")));
    CHECK_FALSE(p.contains(QStringLiteral("facecamRegion")));

    // Out of range: clamped to the command's ranges; an unknown corner is the default.
    p = r.layouts.params(opts({{QStringLiteral("preset"), QStringLiteral("pip")},
                               {QStringLiteral("size"), 2.0},
                               {QStringLiteral("margin"), -1.0},
                               {QStringLiteral("cornerRadius"), 0.9},
                               {QStringLiteral("borderWidth"), 400},
                               {QStringLiteral("corner"), QStringLiteral("middle")},
                               {QStringLiteral("borderColor"), QStringLiteral("#FF0000")},
                               {QStringLiteral("circle"), true}}));
    CHECK(p.value(QStringLiteral("size")).toDouble() == Approx(1.0));
    CHECK(p.value(QStringLiteral("margin")).toDouble() == Approx(0.0));
    CHECK(p.value(QStringLiteral("cornerRadius")).toDouble() == Approx(0.5));
    CHECK(p.value(QStringLiteral("borderWidth")).toDouble() == Approx(100));
    CHECK(p.value(QStringLiteral("corner")).toString() == QStringLiteral("bottomRight"));
    CHECK(p.value(QStringLiteral("borderColor")).toList() == QVariantList{1.0, 0.0, 0.0});
    CHECK(p.value(QStringLiteral("circle")).toBool());

    // Vertical split: its share and the facecam's place, not the corner look.
    p = r.layouts.params(opts({{QStringLiteral("preset"), QStringLiteral("verticalSplit")},
                               {QStringLiteral("split"), 0.9},
                               {QStringLiteral("facecamOnTop"), true}}));
    CHECK(p.value(QStringLiteral("split")).toDouble() == Approx(0.8));
    CHECK(p.value(QStringLiteral("facecamOnTop")).toBool());
    CHECK(p.contains(QStringLiteral("facecam")));
    CHECK_FALSE(p.contains(QStringLiteral("corner")));
    CHECK_FALSE(p.contains(QStringLiteral("blur")));

    // Blur fill: no facecam unless asked for; with one, its corner look.
    p = r.layouts.params(opts({{QStringLiteral("preset"), QStringLiteral("verticalBlurFill")},
                               {QStringLiteral("facecamSource"), QStringLiteral("none")},
                               {QStringLiteral("blur"), 900},
                               {QStringLiteral("zoom"), 0.5}}));
    CHECK(p.value(QStringLiteral("blur")).toDouble() == Approx(500));
    CHECK(p.value(QStringLiteral("zoom")).toDouble() == Approx(1.0));
    CHECK_FALSE(p.contains(QStringLiteral("facecam")));
    CHECK_FALSE(p.contains(QStringLiteral("corner")));
    p = r.layouts.params(opts({{QStringLiteral("preset"), QStringLiteral("verticalBlurFill")},
                               {QStringLiteral("facecamSource"), QStringLiteral("clip")}}));
    CHECK(p.contains(QStringLiteral("facecam")));
    CHECK(p.value(QStringLiteral("corner")).toString() == QStringLiteral("bottomRight"));
    CHECK(p.value(QStringLiteral("zoom")).toDouble() == Approx(1.0));
    // An unknown preset is picture-in-picture.
    CHECK(r.layouts.params(opts({{QStringLiteral("preset"), QStringLiteral("grid")}})).value(QStringLiteral("preset")).toString()
          == QStringLiteral("pip"));
}

TEST_CASE("Layout picker: the facecam region from a drag on the monitor in fractions of the gameplay frame",
          "[app][layout]") {
    Rig r;
    // The gameplay fills the 16:9 frame: the frame's fractions are the gameplay's; reversed corners are normalised.
    const QVariantMap region = r.layouts.setRegionFromFrame(0.95, 0.95, 0.70, 0.75);
    CHECK(region.value(QStringLiteral("x")).toDouble() == Approx(0.70));
    CHECK(region.value(QStringLiteral("y")).toDouble() == Approx(0.75));
    CHECK(region.value(QStringLiteral("width")).toDouble() == Approx(0.25));
    CHECK(region.value(QStringLiteral("height")).toDouble() == Approx(0.20));
    CHECK(r.layouts.facecamRegionInFrame().value(QStringLiteral("x")).toDouble() == Approx(0.70));
    // Outside the frame: clamped.
    CHECK(r.layouts.setRegionFromFrame(-0.5, -0.5, 0.25, 0.25).value(QStringLiteral("x")).toDouble() == Approx(0.0));
    // Too small: refused, no region.
    CHECK(r.layouts.setRegionFromFrame(0.5, 0.5, 0.505, 0.505).isEmpty());
    CHECK(r.layouts.facecamRegion().isEmpty());

    // The region instead of the clip: the copy of the gameplay cropped to it.
    r.layouts.setRegionFromFrame(0.70, 0.70, 0.95, 0.95);
    const QVariantMap p = r.layouts.params(opts({{QStringLiteral("preset"), QStringLiteral("pip")},
                                                 {QStringLiteral("facecamSource"), QStringLiteral("region")}}));
    CHECK_FALSE(p.contains(QStringLiteral("facecam")));
    const QVariantMap sent = p.value(QStringLiteral("facecamRegion")).toMap();
    CHECK(sent.value(QStringLiteral("x")).toDouble() == Approx(0.70));
    CHECK(sent.value(QStringLiteral("width")).toDouble() == Approx(0.25));
    r.layouts.clearRegion();
    CHECK(r.layouts.facecamRegion().isEmpty());
}

TEST_CASE("Layout picker: the dry run checks the layout and Apply is one undo step; without layout.apply nothing applies",
          "[app][layout]") {
    Rig r;
    const std::size_t before = r.undo();
    // The dry run: the layout checks out and nothing changes.
    r.layouts.preview(opts({{QStringLiteral("preset"), QStringLiteral("pip")}}));
    QVariantMap result = r.layouts.previewResult();
    CHECK(result.value(QStringLiteral("ok")).toBool());
    CHECK(r.layouts.previewShown());
    CHECK(r.undo() == before);
    CHECK(g_lastApply.value("preset", std::string()) == "pip");
    // A picture-in-picture without a facecam: the command's refusal before Apply.
    r.layouts.preview(opts({{QStringLiteral("preset"), QStringLiteral("pip")},
                            {QStringLiteral("facecamSource"), QStringLiteral("region")}}));
    result = r.layouts.previewResult();
    CHECK_FALSE(result.value(QStringLiteral("ok")).toBool());
    CHECK(result.value(QStringLiteral("error")).toString().contains(QStringLiteral("facecam")));
    CHECK(r.undo() == before);

    // Apply: one undo step; what the command got is what params() says.
    const QVariantMap options = opts({{QStringLiteral("preset"), QStringLiteral("verticalSplit")}, {QStringLiteral("split"), 0.55}});
    CHECK(r.layouts.apply(options).isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.gameplayColor() == 2);
    CHECK(g_lastApply["split"].get<double>() == Approx(0.55));
    CHECK(g_lastApply["facecam"].get<std::uint64_t>() == r.tl.clip("V2@5").value);
    CHECK_FALSE(r.layouts.previewShown());
    REQUIRE(r.controller.undo());
    CHECK(r.gameplayColor() != 2);

    // Nothing selected: the lowest video clip under the playhead is the gameplay, the webcam above it still the facecam.
    r.controller.setSelection({});
    r.controller.setPlayhead(f(2));
    r.layouts.refreshTargets();
    CHECK(r.layouts.gameplay() == clipKey(r.tl.clip("V1@0")));
    CHECK(r.layouts.facecam() == clipKey(r.tl.clip("V2@5")));

    // A catalog without layout.apply (C.15 not on master): the built-in layouts, nothing applies.
    static const command::CommandRegistry empty;
    r.layouts.setRegistry(&empty);
    CHECK_FALSE(r.layouts.available());
    CHECK_FALSE(r.layouts.sequencePresetsAvailable());
    REQUIRE(r.layouts.layouts().size() == 3);
    for (const QVariant& l : r.layouts.layouts())
        CHECK_FALSE(l.toMap().value(QStringLiteral("description")).toString().isEmpty());
    CHECK(r.layouts.defaults().value(QStringLiteral("size")).toDouble() == Approx(0.28));
    CHECK_FALSE(r.layouts.apply(options).isEmpty());
    CHECK(r.undo() == before);
    r.layouts.preview(options);
    CHECK(r.layouts.previewResult().value(QStringLiteral("unavailable")).toBool());
    CHECK(r.layouts.sequencePresets().isEmpty());
}

TEST_CASE("Layout picker on the app's catalog (C.15): the real layout.apply checks and applies a facecam corner",
          "[app][layout]") {
    Rig r;
    r.layouts.setRegistry(nullptr); // ai::edit::editingCommands(), as in the app
    REQUIRE(r.layouts.available());
    CHECK(r.layouts.sequencePresetsAvailable());
    CHECK(r.layouts.layouts().size() == 3);
    CHECK(r.layouts.defaults().value(QStringLiteral("size")).toDouble() == Approx(0.28));
    const QVariantList presets = r.layouts.sequencePresets();
    CHECK(std::any_of(presets.begin(), presets.end(), [](const QVariant& p) {
        return p.toMap().value(QStringLiteral("id")).toString() == QStringLiteral("shorts");
    }));
    r.layouts.refreshTargets();
    CHECK(r.layouts.gameplay() == clipKey(r.tl.clip("V1@0")));
    CHECK(r.layouts.facecam() == clipKey(r.tl.clip("V2@5")));
    const ClipId cam = r.tl.clip("V2@5");
    const std::size_t effectsBefore = r.controller.currentSequence()->findClip(cam)->clip().effects().size();
    const std::size_t before = r.undo();

    // The dry run: the real command checks the layout; nothing changes.
    const QVariantMap pip = opts({{QStringLiteral("preset"), QStringLiteral("pip")},
                                  {QStringLiteral("corner"), QStringLiteral("topLeft")},
                                  {QStringLiteral("size"), 0.3}});
    r.layouts.preview(pip);
    QVariantMap result = r.layouts.previewResult();
    INFO(result.value(QStringLiteral("error")).toString().toStdString());
    CHECK(result.value(QStringLiteral("ok")).toBool());
    CHECK(r.undo() == before);
    CHECK(r.controller.currentSequence()->findClip(cam)->clip().effects().size() == effectsBefore);
    // A facecam corner without a facecam: the command's own refusal.
    r.layouts.preview(opts({{QStringLiteral("preset"), QStringLiteral("pip")},
                            {QStringLiteral("facecamSource"), QStringLiteral("region")}}));
    CHECK_FALSE(r.layouts.previewResult().value(QStringLiteral("ok")).toBool());
    CHECK_FALSE(r.layouts.previewResult().value(QStringLiteral("error")).toString().isEmpty());

    // Apply: one undo step; the facecam gets the layout's effects (crop / mask / border / shadow), undo takes them off.
    CHECK(r.layouts.apply(pip).isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.currentSequence()->findClip(cam)->clip().effects().size() > effectsBefore);
    REQUIRE(r.controller.undo());
    CHECK(r.controller.currentSequence()->findClip(cam)->clip().effects().size() == effectsBefore);
}

TEST_CASE("Layout picker: schematics of the three layouts", "[app][layout]") {
    Rig r;
    // Picture-in-picture on 16:9: the gameplay fills the frame, the facecam in the bottom right corner.
    QVariantList boxes = r.layouts.schematic(opts({{QStringLiteral("preset"), QStringLiteral("pip")}}), 16.0 / 9.0);
    REQUIRE(boxes.size() == 2);
    QVariantMap gameplay = boxes[0].toMap();
    QVariantMap facecam = boxes[1].toMap();
    CHECK(gameplay.value(QStringLiteral("role")).toString() == QStringLiteral("gameplay"));
    CHECK(gameplay.value(QStringLiteral("width")).toDouble() == Approx(1.0));
    CHECK(facecam.value(QStringLiteral("role")).toString() == QStringLiteral("facecam"));
    CHECK(facecam.value(QStringLiteral("width")).toDouble() == Approx(0.28));
    // A 16:9 facecam 0.28 of the width is 0.28 of the height too in a 16:9 frame.
    CHECK(facecam.value(QStringLiteral("height")).toDouble() == Approx(0.28));
    CHECK(facecam.value(QStringLiteral("x")).toDouble() + facecam.value(QStringLiteral("width")).toDouble()
          == Approx(1.0 - 0.03 * 9.0 / 16.0));
    CHECK(facecam.value(QStringLiteral("y")).toDouble() + facecam.value(QStringLiteral("height")).toDouble() == Approx(0.97));
    // A circle in the top left: square, round.
    facecam = r.layouts.schematic(opts({{QStringLiteral("preset"), QStringLiteral("pip")}, {QStringLiteral("circle"), true},
                                        {QStringLiteral("corner"), QStringLiteral("topLeft")}}),
                                  16.0 / 9.0)[1].toMap();
    CHECK(facecam.value(QStringLiteral("circle")).toBool());
    CHECK(facecam.value(QStringLiteral("x")).toDouble() == Approx(0.03 * 9.0 / 16.0));
    CHECK(facecam.value(QStringLiteral("y")).toDouble() == Approx(0.03));
    CHECK(facecam.value(QStringLiteral("height")).toDouble() == Approx(0.28 * 16.0 / 9.0));

    // Vertical split on 9:16: the gameplay's share on top, the facecam below.
    boxes = r.layouts.schematic(opts({{QStringLiteral("preset"), QStringLiteral("verticalSplit")}, {QStringLiteral("split"), 0.6}}),
                                9.0 / 16.0);
    REQUIRE(boxes.size() == 2);
    CHECK(boxes[0].toMap().value(QStringLiteral("height")).toDouble() == Approx(0.6));
    CHECK(boxes[1].toMap().value(QStringLiteral("y")).toDouble() == Approx(0.6));

    // Blur fill on 9:16: the blurred background, the 16:9 gameplay across the middle.
    boxes = r.layouts.schematic(opts({{QStringLiteral("preset"), QStringLiteral("verticalBlurFill")},
                                      {QStringLiteral("facecamSource"), QStringLiteral("none")}}),
                                9.0 / 16.0);
    REQUIRE(boxes.size() == 2);
    CHECK(boxes[0].toMap().value(QStringLiteral("blur")).toBool());
    const QVariantMap sharp = boxes[1].toMap();
    CHECK(sharp.value(QStringLiteral("width")).toDouble() == Approx(1.0));
    CHECK(sharp.value(QStringLiteral("height")).toDouble() == Approx((9.0 / 16.0) / (16.0 / 9.0)));
    CHECK(sharp.value(QStringLiteral("y")).toDouble() == Approx((1.0 - (9.0 / 16.0) / (16.0 / 9.0)) / 2.0));
}

TEST_CASE("Layout picker: sequence presets and Create a Shorts sequence", "[app][layout]") {
    Rig r;
    CHECK(r.layouts.sequencePresetsAvailable());
    const QVariantList presets = r.layouts.sequencePresets();
    REQUIRE(presets.size() == 2);
    CHECK(presets[1].toMap().value(QStringLiteral("id")).toString() == QStringLiteral("shorts"));
    CHECK_FALSE(r.layouts.sequenceIsVertical());
    const std::size_t before = r.undo();
    CHECK(r.layouts.createShortsSequence().isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.currentSequence()->width == 1080);
    CHECK(r.controller.currentSequence()->height == 1920);
    CHECK(r.layouts.sequenceIsVertical());
}

TEST_CASE("Layout picker in the window: cards and options per layout; the region drawn on the monitor; a11y; screenshot",
          "[app][layout][qml][screenshot]") {
    resetSettings();
    clearMessages();
    A11yScope a11y;
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    shell.setAppearance(true, "ultraviolet");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* layouts = shell.engine.singletonInstance<LayoutBridge*>("Wabcut", "Layouts");
    REQUIRE(controller);
    REQUIRE(layouts);
    layouts->setRegistry(&catalog());
    layouts->setSynchronous(true);
    Timeline tl{kGame};
    controller->resetProject(tl.state, tl.seq);
    controller->setSelection({tl.clip("V1@0")});
    REQUIRE_FALSE(shell.grab().isNull());
    QQuickWindow* w = shell.window.get();

    // The monitor's overlay over the program picture (Main puts it in the Program Monitor's frame area).
    QQuickItem* frameArea = findNamed(w->contentItem(), QStringLiteral("programFrameArea"));
    REQUIRE(frameArea);
    const auto overlay = makeItem(shell, "import Wabcut\nLayoutMonitorOverlay {}\n", sceneRect(frameArea));

    // The picker, opened over the window beside the Program Monitor.
    QQuickItem* monitor = findNamed(w->contentItem(), QStringLiteral("monitorPanel"));
    REQUIRE(monitor);
    const auto host = makeItem(shell, "import QtQuick\nimport Wabcut\nItem {\n    property alias picker: p\n    LayoutPicker { id: p }\n}\n",
                               QRectF(0, 0, kShotWidth, kShotHeight));
    auto* picker = host->property("picker").value<QObject*>();
    REQUIRE(picker);
    REQUIRE(QMetaObject::invokeMethod(picker, "openFor", Q_ARG(QVariant, QVariant::fromValue<QObject*>(host.get())),
                                      Q_ARG(QVariant, QVariant::fromValue<QObject*>(monitor))));
    REQUIRE(waitFor([&] { return picker->property("opened").toBool(); }, 5000));
    auto* content = picker->property("contentItem").value<QQuickItem*>();
    REQUIRE(content);
    QQuickItem* popup = content->parentItem();
    REQUIRE(popup);
    CHECK(layouts->previewShown());
    CHECK(overlay->isVisible());
    // Beside the monitor, not over its picture.
    CHECK_FALSE(sceneRect(popup).intersects(sceneRect(frameArea)));

    // Three cards; picture-in-picture first, its corner options shown.
    QQuickItem* pip = findNamed(popup, QStringLiteral("layoutCard_pip"));
    QQuickItem* split = findNamed(popup, QStringLiteral("layoutCard_verticalSplit"));
    QQuickItem* blurFill = findNamed(popup, QStringLiteral("layoutCard_verticalBlurFill"));
    REQUIRE(pip);
    REQUIRE(split);
    REQUIRE(blurFill);
    CHECK(pip->property("chosen").toBool());
    CHECK(findNamed(popup, QStringLiteral("layoutCornerOptions"))->isVisible());
    CHECK_FALSE(findNamed(popup, QStringLiteral("layoutSplitOptions"))->isVisible());
    CHECK(findNamed(popup, QStringLiteral("layoutApply"))->isEnabled());
    // The schematic on the monitor: the gameplay and the facecam.
    CHECK(findNamed(overlay.get(), QStringLiteral("layoutBox_facecam")) != nullptr);

    // Every control named; the cards and Apply on the Tab path.
    {
        const QStringList issues = auditAccessibility(popup);
        INFO(join(issues));
        CHECK(issues.isEmpty());
    }
    CHECK(tabReaches(popup, QStringLiteral("layoutCard_verticalSplit")));
    CHECK(tabReaches(popup, QStringLiteral("layoutApply")));

    // Vertical split: its options instead of the corner's.
    REQUIRE(QMetaObject::invokeMethod(split, "clicked"));
    CHECK(picker->property("preset").toString() == QStringLiteral("verticalSplit"));
    CHECK(findNamed(popup, QStringLiteral("layoutSplitOptions"))->isVisible());
    CHECK_FALSE(findNamed(popup, QStringLiteral("layoutCornerOptions"))->isVisible());

    // The facecam region, drawn on the monitor: a drag over the right half's lower part.
    REQUIRE(QMetaObject::invokeMethod(pip, "clicked"));
    layouts->setDrawingRegion(true);
    const QRectF area = sceneRect(overlay.get());
    const QPointF from = area.topLeft() + QPointF(area.width() * 0.70, area.height() * 0.70);
    const QPointF to = area.topLeft() + QPointF(area.width() * 0.95, area.height() * 0.95);
    mouse(w, QEvent::MouseButtonPress, from);
    mouse(w, QEvent::MouseMove, (from + to) / 2);
    mouse(w, QEvent::MouseMove, to);
    mouse(w, QEvent::MouseButtonRelease, to);
    const QVariantMap region = layouts->facecamRegion();
    INFO("region " << region.value(QStringLiteral("x")).toDouble() << ", " << region.value(QStringLiteral("y")).toDouble());
    REQUIRE_FALSE(region.isEmpty());
    CHECK(region.value(QStringLiteral("x")).toDouble() == Approx(0.70).margin(0.01));
    CHECK(region.value(QStringLiteral("y")).toDouble() == Approx(0.70).margin(0.01));
    CHECK(region.value(QStringLiteral("width")).toDouble() == Approx(0.25).margin(0.01));
    CHECK_FALSE(layouts->drawingRegion());
    CHECK(findNamed(overlay.get(), QStringLiteral("layoutRegionOutline"))->isVisible());

    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    REQUIRE(image.save(QDir(outputDir()).filePath(QStringLiteral("layout-picker.png"))));

    // Apply from the picker: one undo step, the picker closes.
    const std::size_t before = controller->session().undoCount();
    REQUIRE(QMetaObject::invokeMethod(findNamed(popup, QStringLiteral("layoutApply")), "clicked"));
    CHECK(controller->session().undoCount() == before + 1);
    REQUIRE(waitFor([&] { return !picker->property("visible").toBool(); }, 3000));
    CHECK_FALSE(layouts->previewShown());
    layouts->setRegistry(nullptr);
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
