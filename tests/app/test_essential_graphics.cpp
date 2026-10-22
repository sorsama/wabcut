// M5o: the Essential Graphics panel (Graphics = GraphicsBridge over C.13's graphic and template commands): the template
// browser with thumbnails painted off the GUI thread, a template added where it is dropped and at the playhead, Save as
// template with the fields chosen, and the Edit tab - a template's fields, layers, properties, keyframes, presets and
// pins as one undo step each - and the Qt text measurer on Thai. Offscreen; no window is shown.

#include "A11yAudit.h"
#include "BridgeIds.h"
#include "GlobalBridge.h" // qtLineBreaker()
#include "GraphicText.h"
#include "GraphicsBridge.h"
#include "ProjectController.h"
#include "ShellFixture.h"
#include "TimelineFixture.h"

#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/GraphicResolve.h>
#include <wabcut/model/Graphics.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFileInfo>
#include <QQmlComponent>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QTextBoundaryFinder>
#include <QThread>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using Catch::Approx;

namespace {

constexpr std::string_view kPicture = R"(
    V2     |....................................................................................................|
    V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
)";

Ticks seconds(double s) { return Ticks{static_cast<std::int64_t>(s * static_cast<double>(kTicksPerSecond))}; }

GraphicColor rgb(double r, double g, double b, double a = 1.0, std::string brand = {}) { return GraphicColor{{r, g, b, a}, std::move(brand)}; }

GraphicLayer shapeLayer(std::uint32_t id, std::string name, double w, double h, double x, double y, GraphicColor fill) {
    GraphicLayer l;
    l.id = id;
    l.name = std::move(name);
    l.kind = LayerKind::Shape;
    l.shape.width = ParamValue(w);
    l.shape.height = ParamValue(h);
    l.shape.cornerRadius = ParamValue(10.0);
    l.shape.fill.color = std::move(fill);
    l.transform.x = ParamValue(x);
    l.transform.y = ParamValue(y);
    return l;
}

GraphicLayer textLayer(std::uint32_t id, std::string name, std::string text, double size, int weight, double x, double y,
                       TextAlign align = TextAlign::Left) {
    GraphicLayer l;
    l.id = id;
    l.name = std::move(name);
    l.kind = LayerKind::Text;
    TextStyle style;
    style.fontSize = size;
    style.fontWeight = weight;
    l.text.runs = {TextRun{std::move(text), style}};
    l.text.align = align;
    l.transform.x = ParamValue(x);
    l.transform.y = ParamValue(y);
    return l;
}

/// A bar (1, the brand's Primary), a name (2) and a title (3) at the bottom left of a 1920 x 1080 canvas.
Graphic lowerThird() {
    Graphic g;
    g.name = "Lower third";
    g.layers = {shapeLayer(1, "Bar", 760, 170, 470, 905, rgb(0.49, 0.36, 1.0, 0.92, "Primary")),
                textLayer(2, "Name", "Somchai Jaidee", 64, 700, 120, 832), textLayer(3, "Title", "Producer", 40, 400, 120, 912)};
    return g;
}

/// A chapter title in the middle with an accent rule under it.
Graphic titleCard() {
    Graphic g;
    g.name = "Title card";
    g.layers = {shapeLayer(1, "Rule", 520, 14, 960, 640, rgb(0.78, 1.0, 0.24, 1.0, "Accent")),
                textLayer(2, "Heading", "Chapter one", 120, 800, 960, 470, TextAlign::Center)};
    return g;
}

/// A full-width red strap with a headline.
Graphic newsStrap() {
    Graphic g;
    g.name = "Breaking news";
    g.layers = {shapeLayer(1, "Strap", 1920, 150, 960, 980, rgb(0.86, 0.12, 0.16)),
                textLayer(2, "Headline", "Breaking news", 72, 800, 80, 940)};
    return g;
}

BrandKit studioKit() {
    BrandKit kit;
    kit.name = "Studio";
    kit.colors = {BrandColor{"Primary", {0.49, 0.36, 1.0, 1.0}}, BrandColor{"Accent", {0.78, 1.0, 0.24, 1.0}}};
    return kit;
}

std::filesystem::path pathOf(const QTemporaryDir& dir) { return std::filesystem::path(dir.path().toStdU16String()); }

/// The lower third on V2 from 0 for 4 s with the studio kit, selected, the playhead at 1 s; the user's library in a
/// temporary folder.
struct Rig {
    Timeline tl{kPicture};
    ProjectController controller;
    GraphicsBridge graphics;
    QTemporaryDir library;
    ClipId clip;

    Rig() {
        REQUIRE(library.isValid());
        auto kit = edit::setBrandKit(tl.state, studioKit());
        REQUIRE(kit);
        auto added = edit::addGraphic(kit->state, tl.seq, tl.track("V2"), Ticks{}, seconds(4), lowerThird());
        REQUIRE(added);
        clip = added->placed.at(0);
        controller.resetProject(added->state, tl.seq);
        graphics.setUserLibrary(pathOf(library));
        graphics.setController(&controller);
        controller.selectClip(clipKey(clip));
        controller.setPlayhead(seconds(1));
        REQUIRE(graphics.clip() == clipKey(clip));
    }
    std::size_t undo() { return controller.session().undoCount(); }
    const Clip& clipOf(ClipId id) const {
        snap = controller.snapshot();
        const auto loc = snap->findSequence(tl.seq)->findClip(id);
        REQUIRE(loc);
        return loc->clip();
    }
    const Graphic& graphic(ClipId id) const {
        const Graphic* g = clipOf(id).graphic();
        REQUIRE(g);
        return *g;
    }
    const GraphicLayer& layer(std::uint32_t id) const {
        const GraphicLayer* l = graphic(clip).findLayer(id);
        REQUIRE(l);
        return *l;
    }
    /// The candidates of exposeCandidates() with these ids, in that order (labels kept).
    QVariantList pick(std::initializer_list<const char*> ids) const {
        const QVariantList all = graphics.exposeCandidates();
        QVariantList out;
        for (const char* id : ids) {
            for (const QVariant& v : all) {
                if (v.toMap().value(QStringLiteral("id")).toString() == QLatin1String(id))
                    out.append(v);
            }
        }
        REQUIRE(out.size() == static_cast<qsizetype>(ids.size()));
        return out;
    }
    mutable ProjectSnapshot snap;
};

QVariantMap templateOf(const GraphicsBridge& graphics, const QString& key) {
    for (const QVariant& t : graphics.templates()) {
        if (t.toMap().value(QStringLiteral("key")).toString() == key)
            return t.toMap();
    }
    return {};
}

/// The resolved width of a graphic's shape layer `id` (the Qt measurer, as the app draws), at `t`.
double shapeWidth(const ProjectState& state, const Clip& clip, std::uint32_t id, Ticks t) {
    ResolveOptions options;
    const auto measurer = qtTextMeasurer();
    const auto breaker = qtLineBreaker();
    options.measurer = measurer.get();
    options.lineBreaker = breaker.get();
    const DrawList list = resolveGraphicClip(state, clip, t, options);
    for (const DrawItem& item : list.items) {
        if (item.layer == id && item.kind == DrawKind::Shape)
            return item.bounds.width;
    }
    FAIL("the layer draws no shape");
    return 0.0;
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

void findAllNamed(QQuickItem* root, const QString& name, QList<QQuickItem*>& out) {
    if (!root)
        return;
    if (root->objectName() == name)
        out.append(root);
    for (QQuickItem* child : root->childItems())
        findAllNamed(child, name, out);
}

std::unique_ptr<QQuickItem> makePanel(Shell& shell, const QRectF& rect) {
    QQmlComponent component(&shell.engine);
    component.setData("import Wabcut\nEssentialGraphicsPanel {}\n", QUrl(QStringLiteral("qrc:/m5o-essential-graphics/Host.qml")));
    std::unique_ptr<QObject> object(component.create());
    INFO(component.errorString().toStdString());
    REQUIRE(object);
    auto* item = qobject_cast<QQuickItem*>(object.get());
    REQUIRE(item);
    object.release();
    item->setParentItem(shell.window->contentItem());
    item->setPosition(rect.topLeft());
    item->setSize(rect.size());
    item->setZ(1000);
    return std::unique_ptr<QQuickItem>(item);
}

void saveCrop(const QImage& image, const QQuickItem* item, const QString& name) {
    const double dpr = image.devicePixelRatio();
    const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()))
                         .adjusted(-12, -12, 12, 12)
                         .intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    REQUIRE(image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect())
                .save(QDir(outputDir()).filePath(name)));
}

/// Emits a button's clicked() (its handler runs as for a click).
void click(QQuickItem* button) {
    REQUIRE(button);
    REQUIRE(QMetaObject::invokeMethod(button, "clicked"));
}

/// The Shell's Graphics with its user library in `library`, over a project with the studio kit and these graphics on
/// V2 (at 0, 4.2, 8.4 s ... for 4 s each); returns their clips.
std::vector<ClipId> setUpShell(Shell& shell, ProjectController*& controller, GraphicsBridge*& graphics, const QTemporaryDir& library,
                               std::vector<Graphic> designs) {
    controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    graphics = shell.engine.singletonInstance<GraphicsBridge*>("Wabcut", "Graphics");
    REQUIRE(controller);
    REQUIRE(graphics);
    graphics->setUserLibrary(pathOf(library));
    Timeline tl(kPicture);
    auto kit = edit::setBrandKit(tl.state, studioKit());
    REQUIRE(kit);
    ProjectState state = kit->state;
    std::vector<ClipId> clips;
    for (std::size_t i = 0; i < designs.size(); ++i) {
        auto added = edit::addGraphic(state, tl.seq, tl.track("V2"), seconds(4.2 * static_cast<double>(i)), seconds(4), designs[i]);
        REQUIRE(added);
        clips.push_back(added->placed.at(0));
        state = added->state;
    }
    controller->resetProject(state, tl.seq);
    return clips;
}

} // namespace

TEST_CASE("Essential Graphics: Save as template keeps the chosen fields in the project's library or the user's",
          "[app][graphics]") {
    Rig r;
    // What the lower third can expose: each layer's text, font and colours, visibility and out point; the clip's length.
    const QVariantList candidates = r.graphics.exposeCandidates();
    QStringList ids;
    for (const QVariant& c : candidates)
        ids << c.toMap().value(QStringLiteral("id")).toString();
    INFO(ids.join(QLatin1Char(' ')).toStdString());
    for (const char* id : {"name.text", "name.font", "name.fill", "title.text", "bar.fill", "bar.visible", "bar.out", "duration"})
        CHECK(ids.contains(QLatin1String(id)));

    QVariantList fields = r.pick({"name.text", "title.text", "bar.fill"});
    QVariantMap renamed = fields[0].toMap();
    renamed[QStringLiteral("label")] = QStringLiteral("Guest name");
    fields[0] = renamed;

    SECTION("the project's library: one undo step, listed with its fields and a thumbnail") {
        const auto before = r.undo();
        CHECK(r.graphics.saveTemplate({}, QStringLiteral("Lower third"), QStringLiteral("Name and role"), QStringLiteral("project"), fields)
                  .isEmpty());
        CHECK(r.undo() == before + 1);
        const QVariantMap t = templateOf(r.graphics, QStringLiteral("project/lower-third"));
        REQUIRE_FALSE(t.isEmpty());
        CHECK(t.value(QStringLiteral("name")).toString() == QStringLiteral("Lower third"));
        CHECK(t.value(QStringLiteral("description")).toString() == QStringLiteral("Name and role"));
        const QVariantList saved = t.value(QStringLiteral("fields")).toList();
        REQUIRE(saved.size() == 3);
        CHECK(saved[0].toMap().value(QStringLiteral("label")).toString() == QStringLiteral("Guest name"));
        CHECK(saved[2].toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("color"));
        CHECK(t.value(QStringLiteral("thumbnail")).toString().startsWith(QStringLiteral("image://graphicthumb/project/lower-third/")));
        CHECK(r.graphics.findTemplate(QStringLiteral("project/lower-third")));
        // Deleting it from the project's library is an undo step too.
        CHECK(r.graphics.removeTemplate(QStringLiteral("project/lower-third")).isEmpty());
        CHECK(templateOf(r.graphics, QStringLiteral("project/lower-third")).isEmpty());
        CHECK(r.undo() == before + 2);
    }
    SECTION("the user's library: a file in the folder, no undo step; deleting removes the file") {
        const auto before = r.undo();
        CHECK(r.graphics.saveTemplate(QStringLiteral("strap"), QStringLiteral("Strap"), {}, QStringLiteral("user"), fields).isEmpty());
        CHECK(r.undo() == before);
        const QVariantMap t = templateOf(r.graphics, QStringLiteral("user/strap"));
        REQUIRE_FALSE(t.isEmpty());
        CHECK(t.value(QStringLiteral("library")).toString() == QStringLiteral("user"));
        CHECK(r.graphics.libraryErrors().isEmpty());
        CHECK_FALSE(QDir(r.library.path()).entryList(QDir::Files).isEmpty());
        CHECK(r.graphics.removeTemplate(QStringLiteral("user/strap")).isEmpty());
        CHECK(templateOf(r.graphics, QStringLiteral("user/strap")).isEmpty());
        CHECK(QDir(r.library.path()).entryList(QDir::Files).isEmpty());
    }
    SECTION("without a graphic selected there is nothing to save") {
        r.controller.clearSelection();
        CHECK(r.graphics.clip().isEmpty());
        CHECK(r.graphics.exposeCandidates().isEmpty());
        CHECK_FALSE(r.graphics.saveTemplate({}, QStringLiteral("X"), {}, QStringLiteral("project"), {}).isEmpty());
    }
}

TEST_CASE("Essential Graphics: a template is added where it is dropped and at the playhead; a field edit is one undo step",
          "[app][graphics]") {
    Rig r;
    REQUIRE(r.graphics.saveTemplate({}, QStringLiteral("Lower third"), {}, QStringLiteral("project"),
                                    r.pick({"name.text", "bar.fill", "duration"}))
                .isEmpty());
    const QString key = QStringLiteral("project/lower-third");

    // A drop on V2 at 5 s, as Main's timeline drop area passes it (the track under the pointer, the timecode there).
    const auto beforeDrop = r.undo();
    CHECK(r.graphics.instantiate(key, trackKey(r.tl.track("V2")), QStringLiteral("00:00:05:00")).isEmpty());
    CHECK(r.undo() == beforeDrop + 1);
    const ClipId dropped = r.controller.primaryClip();
    REQUIRE(dropped.isValid());
    CHECK(dropped != r.clip);
    CHECK(r.clipOf(dropped).start() == seconds(5));
    CHECK(r.controller.snapshot()->findSequence(r.tl.seq)->findClip(dropped)->track->id == r.tl.track("V2"));
    REQUIRE(r.graphic(dropped).instance);
    CHECK(r.graphic(dropped).instance->templateId == "lower-third");
    CHECK(r.graphics.clip() == clipKey(dropped));

    // At the playhead (double-click, Enter): V1 and V2 are taken at 1 s, so the first free track above.
    r.controller.setPlayhead(seconds(1));
    CHECK(r.graphics.instantiateAtPlayhead(key).isEmpty());
    const ClipId atPlayhead = r.controller.primaryClip();
    REQUIRE(atPlayhead.isValid());
    CHECK(atPlayhead != dropped);
    CHECK(r.clipOf(atPlayhead).start() == seconds(1));
    const TrackId placedOn = r.controller.snapshot()->findSequence(r.tl.seq)->findClip(atPlayhead)->track->id;
    CHECK(placedOn != r.tl.track("V1"));
    CHECK(placedOn != r.tl.track("V2"));

    // The instance's fields, first in Edit: typing is one undo step until the gesture ends.
    const QVariantList fields = r.graphics.fields();
    REQUIRE(fields.size() == 3);
    CHECK(fields[0].toMap().value(QStringLiteral("value")).toString() == QStringLiteral("Somchai Jaidee"));
    CHECK(fields[1].toMap().value(QStringLiteral("value")).toString() == QStringLiteral("brand:Primary"));
    CHECK(fields[2].toMap().value(QStringLiteral("value")).toDouble() == Approx(4.0));
    const auto before = r.undo();
    for (const char* typed : {"S", "Su", "Sud", "Suda"})
        CHECK(r.graphics.setField(QStringLiteral("name.text"), QString::fromLatin1(typed)).isEmpty());
    r.graphics.endEdit();
    CHECK(r.undo() == before + 1);
    CHECK(r.graphic(atPlayhead).findLayer(2)->text.text() == "Suda");
    CHECK(r.graphics.fields()[0].toMap().value(QStringLiteral("value")).toString() == QStringLiteral("Suda"));
    // A colour field takes a hex colour or a brand colour; a new gesture is a new step.
    CHECK(r.graphics.setField(QStringLiteral("bar.fill"), QStringLiteral("#FF2040")).isEmpty());
    r.graphics.endEdit();
    CHECK(r.undo() == before + 2);
    CHECK(r.graphic(atPlayhead).findLayer(1)->shape.fill.color.brand.empty());
    CHECK(r.graphic(atPlayhead).findLayer(1)->shape.fill.color.rgba[0] == Approx(1.0));
    CHECK_FALSE(r.graphics.setField(QStringLiteral("bar.fill"), QStringLiteral("not a colour")).isEmpty());
    // The other instance kept its text.
    CHECK(r.graphic(dropped).findLayer(2)->text.text() == "Somchai Jaidee");
}

TEST_CASE("Essential Graphics: layers are added, reordered and deleted; a property drag and the stopwatch are one undo step each",
          "[app][graphics]") {
    Rig r;
    const auto layerIds = [&] {
        std::vector<int> out;
        for (const QVariant& l : r.graphics.layers())
            out.push_back(l.toMap().value(QStringLiteral("id")).toInt());
        return out;
    };
    CHECK(layerIds() == std::vector<int>{3, 2, 1}); // top first

    SECTION("add, reorder, delete") {
        auto before = r.undo();
        CHECK(r.graphics.addLayer(QStringLiteral("text")).isEmpty());
        CHECK(r.undo() == before + 1);
        const int added = r.graphics.selectedLayer();
        CHECK(added == 4);
        CHECK(layerIds() == std::vector<int>{4, 3, 2, 1});
        CHECK(r.graphics.layer().value(QStringLiteral("kind")).toString() == QStringLiteral("text"));
        CHECK(r.graphics.moveLayer(added, QStringLiteral("backward")).isEmpty());
        CHECK(layerIds() == std::vector<int>{3, 4, 2, 1});
        CHECK(r.graphics.moveLayer(added, QStringLiteral("back")).isEmpty());
        CHECK(layerIds() == std::vector<int>{3, 2, 1, 4});
        // The keymap's Bring to Front on the selected layer.
        CHECK(r.graphics.runCommand(QStringLiteral("graphics.bringToFront")));
        CHECK(layerIds() == std::vector<int>{4, 3, 2, 1});
        CHECK(r.undo() == before + 4);
        CHECK(r.graphics.setLayerVisible(added, false).isEmpty());
        CHECK_FALSE(r.layer(4).visible);
        CHECK(r.graphics.renameLayer(added, QStringLiteral("Caption")).isEmpty());
        CHECK(r.layer(4).name == "Caption");
        before = r.undo();
        CHECK(r.graphics.removeLayer(added).isEmpty());
        CHECK(r.undo() == before + 1);
        CHECK(layerIds() == std::vector<int>{3, 2, 1});
        // Select Next Layer walks the tree.
        r.graphics.setSelectedLayer(3);
        CHECK(r.graphics.runCommand(QStringLiteral("graphics.selectNextLayer")));
        CHECK(r.graphics.selectedLayer() == 2);
        CHECK_FALSE(r.graphics.runCommand(QStringLiteral("sequence.trimEdit")));
    }
    SECTION("a slider drag is one undo step; the stopwatch keys the value at the playhead; the key toggle") {
        r.graphics.setSelectedLayer(1);
        auto before = r.undo();
        for (const double x : {480.0, 500.0, 520.0})
            CHECK(r.graphics.setValue(1, QStringLiteral("transform.x"), x).isEmpty());
        r.graphics.endEdit();
        CHECK(r.undo() == before + 1);
        CHECK(r.layer(1).transform.x.valueAt({}) == Approx(520));
        const QVariantMap x = r.graphics.layer().value(QStringLiteral("transform")).toMap().value(QStringLiteral("x")).toMap();
        CHECK(x.value(QStringLiteral("value")).toDouble() == Approx(520));
        CHECK_FALSE(x.value(QStringLiteral("animated")).toBool());

        // Stopwatch on at 1 s: a key there with the value.
        before = r.undo();
        CHECK(r.graphics.setAnimated(1, QStringLiteral("transform.opacity"), true).isEmpty());
        CHECK(r.undo() == before + 1);
        REQUIRE(r.layer(1).transform.opacity.curve());
        CHECK(r.layer(1).transform.opacity.curve()->keys.size() == 1);
        QVariantMap opacity = r.graphics.layer().value(QStringLiteral("transform")).toMap().value(QStringLiteral("opacity")).toMap();
        CHECK(opacity.value(QStringLiteral("animated")).toBool());
        CHECK(opacity.value(QStringLiteral("keyed")).toBool());
        // At 2 s a drag adds a key there (one step).
        r.controller.setPlayhead(seconds(2));
        for (const double v : {80.0, 60.0, 40.0})
            CHECK(r.graphics.setValue(1, QStringLiteral("transform.opacity"), v).isEmpty());
        r.graphics.endEdit();
        CHECK(r.undo() == before + 2);
        REQUIRE(r.layer(1).transform.opacity.curve());
        CHECK(r.layer(1).transform.opacity.curve()->keys.size() == 2);
        CHECK(r.layer(1).transform.opacity.valueAt(seconds(2)) == Approx(40));
        // The key toggle removes the key at the playhead, then adds one back.
        CHECK(r.graphics.toggleKey(1, QStringLiteral("transform.opacity")).isEmpty());
        CHECK(r.layer(1).transform.opacity.curve()->keys.size() == 1);
        opacity = r.graphics.layer().value(QStringLiteral("transform")).toMap().value(QStringLiteral("opacity")).toMap();
        CHECK_FALSE(opacity.value(QStringLiteral("keyed")).toBool());
        CHECK(r.graphics.toggleKey(1, QStringLiteral("transform.opacity")).isEmpty());
        CHECK(r.layer(1).transform.opacity.curve()->keys.size() == 2);
        CHECK(r.undo() == before + 4);
        // Stopwatch off: the value at the playhead becomes the constant.
        CHECK(r.graphics.setAnimated(1, QStringLiteral("transform.opacity"), false).isEmpty());
        CHECK_FALSE(r.layer(1).transform.opacity.isCurve());
    }
    SECTION("text, colours, the fill and the stroke") {
        r.graphics.setSelectedLayer(2);
        auto before = r.undo();
        for (const double size : {66.0, 70.0, 74.0})
            CHECK(r.graphics.setRun(2, 0, QStringLiteral("fontSize"), size).isEmpty());
        r.graphics.endEdit();
        CHECK(r.undo() == before + 1);
        CHECK(r.layer(2).text.runs[0].style.fontSize == Approx(74));
        CHECK(r.graphics.updateLayer(2, QVariantMap{{QStringLiteral("text"), QVariantMap{{QStringLiteral("align"), QStringLiteral("center")}}}})
                  .isEmpty());
        CHECK(r.layer(2).text.align == TextAlign::Center);
        CHECK(r.layer(2).text.text() == "Somchai Jaidee"); // the merge patch keeps the runs
        CHECK(r.graphics.setColor(2, QStringLiteral("fill"), QStringLiteral("brand:Accent")).isEmpty());
        r.graphics.endEdit();
        CHECK(r.layer(2).text.runs[0].style.fill.brand == "Accent");
        // The bar: a linear gradient with a stop added, a dashed stroke.
        CHECK(r.graphics.setFillKind(1, QStringLiteral("linear")).isEmpty());
        CHECK(r.layer(1).shape.fill.kind == PaintKind::Linear);
        const auto stops = r.layer(1).shape.fill.stops.size();
        CHECK(stops >= 2);
        CHECK(r.graphics.addStop(1).isEmpty());
        CHECK(r.layer(1).shape.fill.stops.size() == stops + 1);
        CHECK(r.graphics.setStroke(1, QStringLiteral("enabled"), true).isEmpty());
        CHECK(r.graphics.setStroke(1, QStringLiteral("dashes"), QStringLiteral("12 6")).isEmpty());
        CHECK(r.layer(1).shape.stroke.enabled);
        CHECK(r.layer(1).shape.stroke.dashes == std::vector<double>{12, 6});
        CHECK(r.graphics.updateLayer(1, QVariantMap{{QStringLiteral("blend"), QStringLiteral("screen")}}).isEmpty());
        CHECK(r.layer(1).blend == BlendMode::Screen);
    }
    SECTION("New Layer: Text (Ctrl+T) without a graphic selected makes a graphic clip at the playhead") {
        r.controller.clearSelection();
        r.controller.setPlayhead(seconds(6));
        const auto before = r.undo();
        CHECK(r.graphics.runCommand(QStringLiteral("graphics.newTextLayer")));
        CHECK(r.undo() == before + 1);
        const ClipId made = r.controller.primaryClip();
        REQUIRE(made.isValid());
        CHECK(made != r.clip);
        CHECK(r.clipOf(made).start() == seconds(6));
        CHECK(r.graphics.clip() == clipKey(made));
        CHECK(r.graphics.layers().size() == 1);
        CHECK(r.graphics.selectedLayer() != 0);
        CHECK(r.graphics.layer().value(QStringLiteral("kind")).toString() == QStringLiteral("text"));
    }
}

TEST_CASE("Essential Graphics: presets write keyframes; a pin keeps the bar around the name; responsive time", "[app][graphics]") {
    Rig r;
    auto before = r.undo();
    CHECK(r.graphics.applyPreset(2, QStringLiteral("fade"), QStringLiteral("in"), 0.5, QStringLiteral("easeOut")).isEmpty());
    CHECK(r.undo() == before + 1);
    REQUIRE(r.layer(2).transform.opacity.curve());
    CHECK(r.layer(2).transform.opacity.curve()->keys.size() >= 2);
    CHECK(r.layer(2).transform.opacity.valueAt({}) == Approx(0).margin(1e-6));
    CHECK(r.graphics.applyPreset(3, QStringLiteral("slide"), QStringLiteral("out"), 0.5, QStringLiteral("easeIn"), QStringLiteral("left"))
              .isEmpty());
    CHECK(r.undo() == before + 2);
    CHECK(r.layer(3).transform.x.isCurve());
    CHECK_FALSE(r.graphics.applyPreset(2, QStringLiteral("explode"), QStringLiteral("in"), 0.5, QStringLiteral("linear")).isEmpty());

    // Responsive Design - Position: the bar pinned on all four edges to the name, where it is now.
    QStringList targets;
    for (const QVariant& t : r.graphics.pinTargets(1))
        targets << t.toMap().value(QStringLiteral("id")).toString();
    // 0.2: a top-level layer can also be pinned to the video frame (id -1), listed first.
    CHECK(targets == QStringList{QStringLiteral("-1"), QStringLiteral("3"), QStringLiteral("2")});
    const QStringList all{QStringLiteral("left"), QStringLiteral("top"), QStringLiteral("right"), QStringLiteral("bottom")};
    const double widthBefore = shapeWidth(*r.controller.snapshot(), r.clipOf(r.clip), 1, seconds(1));
    before = r.undo();
    CHECK(r.graphics.setPin(1, 2, all).isEmpty());
    CHECK(r.undo() == before + 1);
    REQUIRE(r.layer(1).pin);
    CHECK(r.layer(1).pin->target == 2);
    CHECK(r.layer(1).pin->edges == (LayerPin::kLeft | LayerPin::kTop | LayerPin::kRight | LayerPin::kBottom));
    // Pinned where it was: the same width now; a longer name stretches it by as much.
    CHECK(shapeWidth(*r.controller.snapshot(), r.clipOf(r.clip), 1, seconds(1)) == Approx(widthBefore).margin(1.0));
    CHECK(r.graphics.setText(2, QStringLiteral("Somchai Jaidee-Wongsawat")).isEmpty());
    r.graphics.endEdit();
    CHECK(shapeWidth(*r.controller.snapshot(), r.clipOf(r.clip), 1, seconds(1)) > widthBefore + 100.0);
    r.graphics.setSelectedLayer(1);
    CHECK(r.graphics.layer().value(QStringLiteral("pin")).toMap().value(QStringLiteral("target")).toInt() == 2);
    // Target 0 unpins.
    CHECK(r.graphics.setPin(1, 0, {}).isEmpty());
    CHECK_FALSE(r.layer(1).pin);

    // Responsive Design - Time.
    CHECK(r.graphics.setResponsive(true, 4.0, 1.0, 0.5).isEmpty());
    REQUIRE(r.graphic(r.clip).responsive);
    CHECK(r.graphic(r.clip).responsive->intro == seconds(1));
    CHECK(r.graphic(r.clip).responsive->outro == seconds(0.5));
    CHECK(r.graphics.graphic().value(QStringLiteral("responsive")).toMap().value(QStringLiteral("duration")).toDouble() == Approx(4));
    CHECK(r.graphics.setResponsive(false, 0, 0, 0).isEmpty());
    CHECK_FALSE(r.graphic(r.clip).responsive);
}

TEST_CASE("Essential Graphics: the Qt measurer lays out Thai with its marks on their consonants and wraps it in the box",
          "[app][graphics]") {
    const QString thai = QStringLiteral("สวัสดีครับ ยินดีต้อนรับสู่รายการของเรา วันนี้เราจะพูดถึงเรื่องที่น่าสนใจ");
    const std::string utf8 = thai.toStdString();
    TextBlockRun run;
    run.text = utf8;
    run.fontSize = 48;
    const TextBlock block({run}, TextAlign::Left, 360, 0);
    const QStringList lines = block.lineTexts();
    INFO(lines.join(QStringLiteral(" | ")).toStdString());
    CHECK(lines.size() >= 3);
    CHECK(lines.join(QString()) == thai);
    CHECK(block.size().width() == Approx(360));
    // Every line starts on a grapheme boundary: no tone mark or vowel sign is cut from its consonant.
    QTextBoundaryFinder clusters(QTextBoundaryFinder::Grapheme, thai);
    qsizetype at = 0;
    for (const QString& line : lines) {
        REQUIRE_FALSE(line.isEmpty());
        CHECK_FALSE(line.at(0).isMark());
        clusters.setPosition(at);
        CHECK(clusters.isAtBoundary());
        at += line.size();
    }
    // The model's measurer over the same layout: the box's width, several lines tall; point text is one line.
    const QtTextMeasurer measurer{};
    TextContent content;
    TextStyle style;
    style.fontSize = 48;
    content.runs = {TextRun{utf8, style}};
    content.boxWidth = 360;
    const auto boxed = measurer.measure(content, {std::string()});
    CHECK(boxed[0] == Approx(360));
    CHECK(boxed[1] > 2.5 * 48);
    content.boxWidth = 0;
    const auto point = measurer.measure(content, {std::string()});
    CHECK(point[0] > 360);
    CHECK(point[1] < 2 * 48 * 1.2);
    // Marks take no width of their own: a consonant with its vowel and tone marks is about as wide as the consonant.
    TextContent bare;
    bare.runs = {TextRun{QStringLiteral("ท").toStdString(), style}};
    TextContent marked;
    marked.runs = {TextRun{QStringLiteral("ที่").toStdString(), style}};
    CHECK(measurer.measure(marked, {std::string()})[0] == Approx(measurer.measure(bare, {std::string()})[0]).margin(2.0));
}

TEST_CASE("Essential Graphics panel: Browse shows the templates with thumbnails painted off the GUI thread; screenshot",
          "[app][graphics][qml][screenshot]") {
    A11yScope a11y; // before the shell, so the controls register their names
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    QTemporaryDir library;
    REQUIRE(library.isValid());
    ProjectController* controller = nullptr;
    GraphicsBridge* graphics = nullptr;
    const std::vector<ClipId> clips = setUpShell(shell, controller, graphics, library, {lowerThird(), titleCard(), newsStrap()});
    const auto save = [&](ClipId clip, const char* name, const char* where) {
        controller->selectClip(clipKey(clip));
        REQUIRE(graphics->clip() == clipKey(clip));
        CHECK(graphics->saveTemplate({}, QString::fromLatin1(name), {}, QString::fromLatin1(where), {}).isEmpty());
    };
    save(clips[0], "Lower third", "project");
    save(clips[1], "Title card", "project");
    save(clips[2], "Breaking news", "user");
    controller->clearSelection();
    REQUIRE(graphics->templates().size() >= 3);

    // The provider paints a thumbnail on its pool: the image arrives, drawn, and not on the GUI thread.
    auto* provider = dynamic_cast<QQuickAsyncImageProvider*>(shell.engine.imageProvider(QStringLiteral("graphicthumb")));
    REQUIRE(provider);
    const QString url = templateOf(*graphics, QStringLiteral("project/lower-third")).value(QStringLiteral("thumbnail")).toString();
    REQUIRE(url.startsWith(QStringLiteral("image://graphicthumb/")));
    const int rendersBefore = graphics->thumbnailRenders();
    std::unique_ptr<QQuickImageResponse> response(provider->requestImageResponse(url.mid(21), QSize(320, 180)));
    REQUIRE(response);
    bool finished = false;
    QObject::connect(response.get(), &QQuickImageResponse::finished, [&] { finished = true; });
    REQUIRE(waitFor([&] { return finished; }, 10000));
    std::unique_ptr<QQuickTextureFactory> factory(response->textureFactory());
    REQUIRE(factory);
    const QImage thumb = factory->image();
    REQUIRE_FALSE(thumb.isNull());
    CHECK(thumb.size() == QSize(320, 180));
    int drawn = 0;
    for (int y = 0; y < thumb.height(); y += 2) {
        for (int x = 0; x < thumb.width(); x += 2)
            drawn += qAlpha(thumb.pixel(x, y)) > 0 ? 1 : 0;
    }
    CHECK(drawn > 200); // the bar and the text
    CHECK(graphics->thumbnailRenders() > rendersBefore);
    CHECK_FALSE(graphics->lastThumbnailOnGuiThread());

    // The panel: Browse (nothing selected), each card's thumbnail loaded.
    const auto panel = makePanel(shell, QRectF(40, 40, 460, 820));
    QQuickItem* browse = findNamed(panel.get(), QStringLiteral("graphicsBrowsePage"));
    REQUIRE(browse);
    REQUIRE(waitFor([&] { return browse->isVisible(); }, 3000));
    REQUIRE(findNamed(panel.get(), QStringLiteral("graphicTemplate_project/lower-third")));
    REQUIRE(findNamed(panel.get(), QStringLiteral("graphicTemplate_project/title-card")));
    REQUIRE(findNamed(panel.get(), QStringLiteral("graphicTemplate_user/breaking-news")));
    REQUIRE_FALSE(shell.grab().isNull()); // a frame lays the cards out (hidden windows polish on render): final sizes
    QList<QQuickItem*> images;
    findAllNamed(panel.get(), QStringLiteral("graphicTemplateImage"), images);
    REQUIRE(images.size() >= 3);
    REQUIRE(waitFor(
        [&] {
            return std::all_of(images.begin(), images.end(), [](QQuickItem* image) { return image->property("status").toInt() == 1; });
        },
        15000)); // Image.Ready
    CHECK_FALSE(graphics->lastThumbnailOnGuiThread());
    const QStringList browseIssues = auditAccessibility(panel.get());
    INFO(browseIssues.join(QLatin1Char('\n')).toStdString());
    CHECK(browseIssues.isEmpty());
    CHECK(tabReaches(panel.get(), QStringLiteral("graphicTemplate_project/lower-third")));
    spin(50);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    saveCrop(image, panel.get(), QStringLiteral("essential-graphics-browse.png"));

    // Enter on a card adds it at the playhead (and the panel turns to Edit).
    controller->setPlayhead(seconds(14));
    QQuickItem* card = findNamed(panel.get(), QStringLiteral("graphicTemplate_project/title-card"));
    REQUIRE(card);
    const auto before = controller->session().undoCount();
    REQUIRE(pressWithKey(shell.window.get(), card, Qt::Key_Return));
    CHECK(controller->session().undoCount() == before + 1);
    REQUIRE_FALSE(graphics->clip().isEmpty());
    QQuickItem* edit = findNamed(panel.get(), QStringLiteral("graphicsEditPage"));
    REQUIRE(edit);
    CHECK(waitFor([&] { return edit->isVisible(); }, 3000));
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Essential Graphics panel: Edit shows a lower third's fields, layers and the selected layer; screenshot",
          "[app][graphics][qml][screenshot]") {
    A11yScope a11y; // before the shell, so the controls register their names
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    QTemporaryDir library;
    REQUIRE(library.isValid());
    ProjectController* controller = nullptr;
    GraphicsBridge* graphics = nullptr;
    const std::vector<ClipId> clips = setUpShell(shell, controller, graphics, library, {lowerThird()});
    controller->selectClip(clipKey(clips[0]));
    QVariantList fields;
    for (const QVariant& c : graphics->exposeCandidates()) {
        const QString id = c.toMap().value(QStringLiteral("id")).toString();
        if (id == QLatin1String("name.text") || id == QLatin1String("title.text") || id == QLatin1String("bar.fill"))
            fields.append(c);
    }
    REQUIRE(fields.size() == 3);
    REQUIRE(graphics->saveTemplate({}, QStringLiteral("Lower third"), {}, QStringLiteral("project"), fields).isEmpty());
    REQUIRE(graphics->instantiate(QStringLiteral("project/lower-third"), {}, static_cast<double>(seconds(5).value)).isEmpty());
    const QString instance = graphics->clip();
    REQUIRE_FALSE(instance.isEmpty());
    controller->setPlayhead(seconds(6));
    graphics->setSelectedLayer(2);

    const auto panel = makePanel(shell, QRectF(40, 40, 460, 900));
    QQuickItem* edit = findNamed(panel.get(), QStringLiteral("graphicsEditPage"));
    REQUIRE(edit);
    REQUIRE(waitFor([&] { return edit->isVisible(); }, 3000));
    QQuickItem* nameField = findNamed(panel.get(), QStringLiteral("graphicFieldText_name.text"));
    REQUIRE(nameField);
    CHECK(nameField->isVisible());
    CHECK(nameField->property("text").toString() == QStringLiteral("Somchai Jaidee"));
    REQUIRE(findNamed(panel.get(), QStringLiteral("graphicField_bar.fill")));
    // The layers and every property are behind Callouts' Advanced (closed until opened).
    CHECK_FALSE(edit->property("advanced").toBool());
    CHECK_FALSE(findNamed(panel.get(), QStringLiteral("graphicLayerProperties")));
    click(findNamed(panel.get(), QStringLiteral("graphicsAdvanced")));
    REQUIRE(edit->property("advanced").toBool());
    for (const char* row : {"graphicLayerRow_1", "graphicLayerRow_2", "graphicLayerRow_3"})
        REQUIRE(findNamed(panel.get(), QString::fromLatin1(row)));
    QQuickItem* properties = findNamed(panel.get(), QStringLiteral("graphicLayerProperties"));
    REQUIRE(properties);
    CHECK(properties->isVisible());
    QQuickItem* text = findNamed(properties, QStringLiteral("graphicText"));
    REQUIRE(text);
    CHECK(text->property("text").toString() == QStringLiteral("Somchai Jaidee"));
    REQUIRE(findNamed(properties, QStringLiteral("graphicBrandSwatch_textFill_Primary")));
    REQUIRE_FALSE(shell.grab().isNull()); // polish the hidden window's layouts first
    const QStringList editIssues = auditAccessibility(panel.get());
    INFO(editIssues.join(QLatin1Char('\n')).toStdString());
    CHECK(editIssues.isEmpty());
    CHECK(tabReaches(panel.get(), QStringLiteral("graphicLayerRow_2")));
    spin(50);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    saveCrop(image, panel.get(), QStringLiteral("essential-graphics-edit.png"));

    // The controls reach the graphic: bring the bar forward, Space on a layer's row selects it, the stopwatch, the eye,
    // Save as template from the dialog.
    auto before = controller->session().undoCount();
    click(findNamed(panel.get(), QStringLiteral("graphicLayerUp_1")));
    CHECK(controller->session().undoCount() == before + 1);
    CHECK(graphics->layers().last().toMap().value(QStringLiteral("id")).toInt() == 2); // the name is now at the bottom
    CHECK(pressWithKey(shell.window.get(), findNamed(panel.get(), QStringLiteral("graphicLayerRow_3")), Qt::Key_Space));
    CHECK(graphics->selectedLayer() == 3);
    click(findNamed(properties, QStringLiteral("graphicStopwatch_transform.x")));
    CHECK(graphics->layer().value(QStringLiteral("id")).toInt() == 3);
    CHECK(graphics->layer().value(QStringLiteral("transform")).toMap().value(QStringLiteral("x")).toMap().value(QStringLiteral("animated")).toBool());
    click(findNamed(panel.get(), QStringLiteral("graphicLayerVisible_3")));
    CHECK(graphics->layers().first().toMap().value(QStringLiteral("visible")).toBool() == false);
    CHECK(controller->session().undoCount() == before + 3);

    click(findNamed(panel.get(), QStringLiteral("graphicEditSaveTemplate")));
    QObject* dialog = panel->findChild<QObject*>(QStringLiteral("graphicTemplateDialog"));
    REQUIRE(dialog);
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }, 3000));
    REQUIRE_FALSE(shell.grab().isNull()); // lays the field list out: its rows are made
    auto* content = dialog->property("contentItem").value<QQuickItem*>();
    REQUIRE(content);
    REQUIRE(waitFor([&] { return findNamed(content, QStringLiteral("graphicExpose_name.text")) != nullptr; }, 3000));
    const QStringList dialogIssues = auditAccessibility(content);
    INFO(dialogIssues.join(QLatin1Char('\n')).toStdString());
    CHECK(dialogIssues.isEmpty());
    QQuickItem* expose = findNamed(content, QStringLiteral("graphicExpose_name.text"));
    CHECK(expose->property("checked").toBool()); // text is offered ticked
    QQuickItem* nameInput = findNamed(content, QStringLiteral("graphicTemplateName"));
    REQUIRE(nameInput);
    nameInput->setProperty("text", QStringLiteral("Guest strap"));
    click(findNamed(content, QStringLiteral("graphicTemplateSave")));
    REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }, 3000));
    const QVariantMap saved = templateOf(*graphics, QStringLiteral("project/guest-strap"));
    REQUIRE_FALSE(saved.isEmpty());
    CHECK_FALSE(saved.value(QStringLiteral("fields")).toList().isEmpty());
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
