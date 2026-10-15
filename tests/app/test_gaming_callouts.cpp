// M5o (gaming): Callouts, the Essential Graphics panel made simple for gaming videos. Gaming templates (fixtures in
// tests/app/data/callouts, read through the user's library) validate and draw; each is added at the playhead as one
// undo step with its first text layer selected, ready to type; its fields edit the text; on a vertical 1080 x 1920
// sequence it sits centred. The built-in templates come from the command catalog (C.16: template.list's "builtin",
// template.resolve), stood in for here by a stub catalog over the fixtures: Browse groups them under Gaming with
// thumbnails painted off the GUI thread (from template.resolve's draw list, or the graphic template.list gives), and an
// empty catalog shows an honest empty state. The Stickers group holds Lottie imports kept as "lottie.<name>"
// templates; "Import Lottie sticker…" asks for the import. Edit shows the fields and the selected layer's text, font,
// size and colour, with Advanced closed until opened. Offscreen; no window is shown.

#include "A11yAudit.h"
#include "BridgeIds.h"
#include "GlobalBridge.h" // qtLineBreaker()
#include "GraphicCanvasBridge.h"
#include "GraphicText.h"
#include "GraphicsBridge.h"
#include "GraphicsImportBridge.h"
#include "ProjectController.h"
#include "ShellFixture.h"
#include "TimelineFixture.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/GraphicJson.h>
#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/GraphicResolve.h>
#include <wabcut/model/Graphics.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QUrl>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using Catch::Approx;
using Json = command::Json;

namespace {

constexpr std::string_view kPicture = R"(
    V2     |....................................................................................................|
    V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
)";

/// The gaming template fixtures, in file order.
const std::vector<std::string> kGaming{"gaming-countdown", "gaming-kill-streak", "gaming-subscribe"};

Ticks seconds(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

std::filesystem::path fixtures() {
    return std::filesystem::path(QDir(QStringLiteral(WABCUT_APP_TEST_DIR)).filePath(QStringLiteral("data/callouts")).toStdU16String());
}

QString userKey(const std::string& id) { return QStringLiteral("user/") + QString::fromStdString(id); }
QString builtinKey(const std::string& id) { return QStringLiteral("builtin/") + QString::fromStdString(id); }

std::filesystem::path pathOf(const QTemporaryDir& dir) { return std::filesystem::path(dir.path().toStdU16String()); }

/// The fixtures copied into `dir` (a user's template library).
void copyFixtures(const QTemporaryDir& dir) {
    const QDir from(QDir(QStringLiteral(WABCUT_APP_TEST_DIR)).filePath(QStringLiteral("data/callouts")));
    const QStringList names = from.entryList({QStringLiteral("*.wabcut-template.json")}, QDir::Files);
    REQUIRE(names.size() == static_cast<qsizetype>(kGaming.size()));
    for (const QString& name : names)
        REQUIRE(QFile::copy(from.filePath(name), QDir(dir.path()).filePath(name)));
}

QVariantMap templateOf(const GraphicsBridge& graphics, const QString& key) {
    for (const QVariant& t : graphics.templates()) {
        if (t.toMap().value(QStringLiteral("key")).toString() == key)
            return t.toMap();
    }
    return {};
}

/// The first text field's layer: the one a new instance selects.
std::uint32_t firstTextField(const Graphic& g) {
    REQUIRE(g.instance);
    for (const TemplateField& f : g.instance->fields) {
        if (f.kind == FieldKind::Text)
            return f.layer;
    }
    FAIL("no text field");
    return 0;
}

// ---- A stand-in for C.16's built-in templates ------------------------------------------------------------------------

const GraphicTemplate* findIn(const std::vector<io::LibraryTemplate>& library, const std::string& id) {
    for (const io::LibraryTemplate& t : library) {
        if (t.tmpl && t.tmpl->id == id)
            return &*t.tmpl;
    }
    return nullptr;
}

/// The app's catalog with the templates of `folder` as C.16's built-ins (category "gaming", the poster time half way):
/// template.list gains its "builtin" entries (with the graphic for one id), template.instantiate finds them after the
/// project's and the user's libraries, and - `withResolve` - template.resolve draws one (graphic.resolve on a scratch
/// instance: the real draw list JSON). Executors run on any thread: no test macros inside.
command::CommandRegistry makeStub(const std::filesystem::path& folder, bool withResolve) {
    const command::CommandRegistry& real = ai::edit::editingCommands();
    command::CommandRegistry stub;
    for (const command::CommandSpec& spec : real.all()) {
        if (spec.name != "template.list" && spec.name != "template.instantiate" && spec.name != "template.resolve")
            REQUIRE(stub.add(spec));
    }
    const auto measurer = qtTextMeasurer();

    command::CommandSpec list = *real.find("template.list");
    list.execute = [realList = list.execute, folder](command::ExecutionContext& c) {
        command::CommandOutput out = realList(c);
        const std::string only = c.string("id");
        Json builtin = Json::array();
        for (const io::LibraryTemplate& lt : io::readTemplateLibrary(folder)) {
            if (!lt.tmpl || (!only.empty() && lt.tmpl->id != only))
                continue;
            const GraphicTemplate& t = *lt.tmpl;
            Json fields = Json::array();
            for (const TemplateField& f : t.fields)
                fields.push_back(Json{{"id", f.id}, {"label", f.label}, {"kind", std::string(toString(f.kind))}, {"property", f.property}});
            Json entry{{"id", t.id},           {"name", t.name},     {"description", t.description},
                       {"version", t.version}, {"duration", t.duration.value}, {"width", t.graphic.width},
                       {"height", t.graphic.height}, {"fields", std::move(fields)}, {"category", "gaming"},
                       {"posterTime", t.duration.value / 2}};
            if (!only.empty())
                entry["graphic"] = Json::parse(io::graphicToJson(t.graphic));
            builtin.push_back(std::move(entry));
        }
        out.data["builtin"] = std::move(builtin);
        return out;
    };
    REQUIRE(stub.add(std::move(list)));

    command::CommandSpec instantiate = *real.find("template.instantiate");
    instantiate.execute = [realInstantiate = instantiate.execute, folder, measurer](command::ExecutionContext& c) {
        try {
            return realInstantiate(c);
        } catch (const command::CommandFailure& failure) {
            if (failure.error.code != command::ErrorCode::NotFound || c.params().contains("library"))
                throw;
        }
        // Not in the project's or the user's library: a built-in.
        command::GraphicsServices builtins(folder, measurer);
        command::ExecutionContext inner(c.state(), c.params(), c.control(), &builtins);
        return realInstantiate(inner);
    };
    REQUIRE(stub.add(std::move(instantiate)));

    if (withResolve) {
        command::CommandSpec resolve;
        resolve.name = "template.resolve";
        resolve.title = "Template Draw List";
        resolve.description = "C.16 stand-in";
        resolve.capability = command::Capability::Read;
        resolve.params = Json{{"type", "object"}};
        resolve.execute = [&real, folder, measurer](command::ExecutionContext& c) {
            const auto library = io::readTemplateLibrary(folder);
            const GraphicTemplate* t = findIn(library, c.string("template"));
            if (!t)
                command::fail(command::ErrorCode::NotFound, "no built-in template \"" + c.string("template") + "\"");
            const std::int64_t time = c.params().value("time", t->duration.value / 2);
            // A scratch sequence with the template at 0, as long as its design: timeline time = design time.
            auto seq = edit::addSequence(c.state(), edit::SequenceSettings{});
            if (!seq)
                command::fail(command::ErrorCode::Internal, seq.error().message);
            const TrackId track = seq->state.findSequence(seq->id)->videoTracks.front()->id;
            auto placed = edit::instantiateTemplate(seq->state, seq->id, track, Ticks{}, t->duration, *t);
            if (!placed)
                command::fail(command::ErrorCode::Internal, placed.error().message);
            // graphic.resolve through the engine on a scratch session of that state (as the app runs it).
            EditSession scratch(placed->state);
            command::EnginePolicy policy;
            policy.services = std::make_shared<command::GraphicsServices>(std::filesystem::path{},
                                                                          measurer);
            command::CommandEngine engine(scratch, real, policy);
            command::CommandRequest request;
            request.command = "graphic.resolve";
            request.params = Json{{"sequence", seq->id.value}, {"clip", placed->placed.front().value}, {"time", time}};
            command::CommandResult drawn = engine.execute(request);
            if (!drawn.ok())
                command::fail(drawn.error->code, drawn.error->message);
            command::CommandOutput out;
            out.state = c.state();
            out.data = std::move(drawn.data);
            return out;
        };
        REQUIRE(stub.add(std::move(resolve)));
    }
    return stub;
}

/// The stand-in catalogs (they outlive every thumbnail job): the fixtures with template.resolve, without it, and none.
const command::CommandRegistry& stubCatalog(bool withResolve) {
    static const command::CommandRegistry with = makeStub(fixtures(), true);
    static const command::CommandRegistry without = makeStub(fixtures(), false);
    return withResolve ? with : without;
}
const command::CommandRegistry& emptyCatalog() {
    static const command::CommandRegistry empty = makeStub({}, true);
    return empty;
}

// ---- Rigs and QML helpers --------------------------------------------------------------------------------------------

/// A sequence of `width` x `height` (the picture's V1 clip in it), a project controller and Graphics with the fixtures
/// in the user's library (a temporary copy).
struct Rig {
    Timeline tl{kPicture};
    ProjectController controller;
    GraphicsBridge graphics;
    QTemporaryDir library;

    explicit Rig(int width = 1920, int height = 1080) {
        REQUIRE(library.isValid());
        copyFixtures(library);
        if (width != tl.sequence().width || height != tl.sequence().height) {
            auto seq = std::make_shared<Sequence>(tl.sequence());
            seq->width = width;
            seq->height = height;
            tl.state = tl.state.withSequence(seq);
        }
        controller.resetProject(tl.state, tl.seq);
        graphics.setUserLibrary(pathOf(library));
        graphics.setController(&controller);
    }
    std::size_t undo() { return controller.session().undoCount(); }
    /// The clip `id` of the live project (a copy: the snapshot moves on).
    Clip clipOf(ClipId id) const {
        const ProjectSnapshot snap = controller.snapshot();
        const auto loc = snap->findSequence(tl.seq)->findClip(id);
        REQUIRE(loc);
        return loc->clip();
    }
    Graphic graphic(ClipId id) const {
        const Clip clip = clipOf(id);
        REQUIRE(clip.graphic());
        return *clip.graphic();
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
    component.setData("import Wabcut\nEssentialGraphicsPanel {}\n", QUrl(QStringLiteral("qrc:/m5o-callouts/Host.qml")));
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

/// A thumbnail from the provider at `url` ("image://graphicthumb/..."), waited for.
QImage thumbnail(QQuickAsyncImageProvider* provider, const QString& url, QSize size) {
    std::unique_ptr<QQuickImageResponse> response(provider->requestImageResponse(url.mid(21), size));
    REQUIRE(response);
    bool finished = false;
    QObject::connect(response.get(), &QQuickImageResponse::finished, [&] { finished = true; });
    REQUIRE(waitFor([&] { return finished; }, 10000));
    std::unique_ptr<QQuickTextureFactory> factory(response->textureFactory());
    REQUIRE(factory);
    return factory->image();
}

int drawnPixels(const QImage& image) {
    int drawn = 0;
    for (int y = 0; y < image.height(); y += 2) {
        for (int x = 0; x < image.width(); x += 2)
            drawn += qAlpha(image.pixel(x, y)) > 0 ? 1 : 0;
    }
    return drawn;
}

/// A five-pointed star: the graphic a Lottie sticker stands in for.
Graphic starBurst() {
    GraphicLayer star;
    star.id = 1;
    star.name = "Star";
    star.kind = LayerKind::Shape;
    star.shape.kind = ShapeKind::Star;
    star.shape.points = 5;
    star.shape.outerRadius = ParamValue(320.0);
    star.shape.innerRadius = ParamValue(140.0);
    star.shape.fill.color = GraphicColor{{0.98, 0.8, 0.08, 1.0}, {}};
    star.transform.x = ParamValue(960.0);
    star.transform.y = ParamValue(540.0);
    Graphic g;
    g.name = "Star burst";
    g.layers = {star};
    return g;
}

QVariantMap runOf(const QVariantMap& layer) {
    return layer.value(QStringLiteral("text")).toMap().value(QStringLiteral("runs")).toList().value(0).toMap();
}

QVariantMap mapOf(QVariant data) {
    if (data.metaType() == QMetaType::fromType<QJSValue>())
        data = data.value<QJSValue>().toVariant();
    return data.toMap();
}

} // namespace

TEST_CASE("Callouts: the gaming template fixtures read and validate and draw inside their canvas", "[app][graphics]") {
    Rig r;
    const auto entries = io::readTemplateLibrary(pathOf(r.library));
    REQUIRE(entries.size() == kGaming.size());
    const auto measurer = qtTextMeasurer();
    const auto breaker = qtLineBreaker();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const io::LibraryTemplate& entry = entries[i];
        INFO(entry.file.filename().string());
        INFO((entry.tmpl ? std::string() : entry.tmpl.error().message));
        REQUIRE(entry.tmpl);
        const GraphicTemplate& t = *entry.tmpl;
        CHECK(t.id == kGaming[i]);
        CHECK_FALSE(templateProblem(t).has_value());
        // Designed for 1920 x 1080 (fitted, centred, into any frame), with a text to type and a colour to pick.
        CHECK(t.graphic.width == 1920);
        CHECK(t.graphic.height == 1080);
        CHECK(std::any_of(t.fields.begin(), t.fields.end(), [](const TemplateField& f) { return f.kind == FieldKind::Text; }));
        CHECK(std::any_of(t.fields.begin(), t.fields.end(), [](const TemplateField& f) { return f.kind == FieldKind::Color; }));
        CHECK(t.duration.value > 0);
        // Written back, it reads the same.
        const auto again = io::templateFileFromJson(io::templateFileToJson(t));
        REQUIRE(again);
        CHECK(io::templateFileToJson(*again) == io::templateFileToJson(t));
        // Half way through every text is drawn, inside the canvas (the animation has landed).
        ResolveOptions options;
        options.measurer = measurer.get();
        options.lineBreaker = breaker.get();
        const DrawList list = resolveGraphic(t.graphic, Ticks{t.duration.value / 2}, options);
        int texts = 0;
        for (const DrawItem& item : list.items) {
            if (item.kind != DrawKind::Text)
                continue;
            ++texts;
            const Box& b = item.bounds;
            for (const auto& [x, y] : {item.transform.map(b.x, b.y), item.transform.map(b.x + b.width, b.y + b.height)}) {
                CHECK(x >= -1.0);
                CHECK(x <= 1921.0);
                CHECK(y >= -1.0);
                CHECK(y <= 1081.0);
            }
        }
        CHECK(texts >= 1);
        // Graphics lists it from the user's library, with its thumbnail and fields.
        const QVariantMap listed = templateOf(r.graphics, userKey(t.id));
        REQUIRE_FALSE(listed.isEmpty());
        CHECK(listed.value(QStringLiteral("thumbnail")).toString().startsWith(QStringLiteral("image://graphicthumb/user/")));
        CHECK(listed.value(QStringLiteral("fields")).toList().size() == static_cast<qsizetype>(t.fields.size()));
    }
    CHECK(r.graphics.libraryErrors().isEmpty());
}

TEST_CASE("Callouts: each gaming template is added at the playhead in one undo step with its first text layer selected",
          "[app][graphics]") {
    Rig r;
    ClipId last;
    for (std::size_t i = 0; i < kGaming.size(); ++i) {
        const std::string& id = kGaming[i];
        INFO(id);
        const Ticks at = seconds(5.0 * static_cast<double>(i));
        r.controller.setPlayhead(at);
        const auto before = r.undo();
        REQUIRE(r.graphics.instantiateAtPlayhead(userKey(id)).isEmpty());
        CHECK(r.undo() == before + 1);
        const ClipId clip = r.controller.primaryClip();
        REQUIRE(clip.isValid());
        CHECK(clip != last);
        last = clip;
        CHECK(r.graphics.clip() == clipKey(clip));
        const Clip placed = r.clipOf(clip);
        CHECK(placed.start() == at);
        // The template's length: 4 s, the countdown 3.8 s.
        CHECK(placed.duration() == (id == "gaming-countdown" ? Ticks{kTicksPerSecond * 38 / 10} : Ticks{4 * kTicksPerSecond}));
        const Graphic g = r.graphic(clip);
        REQUIRE(g.instance);
        CHECK(g.instance->templateId == id);
        // Ready to type: the first text field's layer is selected (the Program Monitor's inline editor follows it).
        CHECK(r.graphics.selectedLayer() == static_cast<int>(firstTextField(g)));
        CHECK(r.graphics.layer().value(QStringLiteral("kind")).toString() == QStringLiteral("text"));
        CHECK_FALSE(r.graphics.fields().isEmpty());
    }
    // Undo takes the last one out again.
    REQUIRE(r.controller.undo());
    CHECK_FALSE(r.controller.snapshot()->findSequence(r.tl.seq)->findClip(last).has_value());
    // A template that does not exist is an error, not a clip.
    const auto before = r.undo();
    CHECK_FALSE(r.graphics.instantiateAtPlayhead(userKey("no-such-callout")).isEmpty());
    CHECK_FALSE(r.graphics.instantiateAtPlayhead(builtinKey("no-such-callout")).isEmpty());
    CHECK(r.undo() == before);
}

TEST_CASE("Callouts: a callout's fields and its selected layer edit the text; one undo step a gesture", "[app][graphics]") {
    Rig r;
    r.controller.setPlayhead(seconds(6));
    REQUIRE(r.graphics.instantiateAtPlayhead(userKey("gaming-kill-streak")).isEmpty());
    const ClipId clip = r.controller.primaryClip();
    const QVariantList fields = r.graphics.fields();
    REQUIRE(fields.size() == 4);
    CHECK(fields[0].toMap().value(QStringLiteral("id")).toString() == QStringLiteral("title"));
    CHECK(fields[0].toMap().value(QStringLiteral("value")).toString() == QStringLiteral("KILL STREAK"));
    CHECK(fields[1].toMap().value(QStringLiteral("id")).toString() == QStringLiteral("count"));
    CHECK(fields[1].toMap().value(QStringLiteral("value")).toString() == QStringLiteral("x5"));

    // Typing the count: one undo step until the gesture ends.
    const auto before = r.undo();
    for (const char* typed : {"x1", "x12"})
        CHECK(r.graphics.setField(QStringLiteral("count"), QString::fromLatin1(typed)).isEmpty());
    r.graphics.endEdit();
    CHECK(r.undo() == before + 1);
    CHECK(r.graphic(clip).findLayer(3)->text.text() == "x12");
    CHECK(r.graphics.fields()[1].toMap().value(QStringLiteral("value")).toString() == QStringLiteral("x12"));
    // A colour field.
    CHECK(r.graphics.setField(QStringLiteral("bannerColor"), QStringLiteral("#22C55E")).isEmpty());
    r.graphics.endEdit();
    CHECK(r.undo() == before + 2);
    CHECK(r.graphic(clip).findLayer(1)->shape.fill.color.rgba[1] == Approx(0xC5 / 255.0));
    // The selected layer (the title, the first text field) takes its text straight away: the field follows.
    REQUIRE(r.graphics.selectedLayer() == 2);
    CHECK(r.graphics.setText(2, QStringLiteral("TRIPLE KILL")).isEmpty());
    r.graphics.endEdit();
    CHECK(r.undo() == before + 3);
    CHECK(r.graphics.fields()[0].toMap().value(QStringLiteral("value")).toString() == QStringLiteral("TRIPLE KILL"));
    // Another instance has the template's text.
    r.controller.setPlayhead(seconds(20));
    REQUIRE(r.graphics.instantiateAtPlayhead(userKey("gaming-kill-streak")).isEmpty());
    CHECK(r.graphics.fields()[1].toMap().value(QStringLiteral("value")).toString() == QStringLiteral("x5"));
}

TEST_CASE("Callouts: on a vertical 1080 x 1920 sequence a callout sits centred with its text inside the frame",
          "[app][graphics]") {
    Rig r(1080, 1920);
    GraphicCanvasBridge canvas;
    canvas.setSynchronous(true);
    canvas.setMeasurer(qtTextMeasurer());
    canvas.setController(&r.controller);
    r.controller.setPlayhead(seconds(10));
    REQUIRE(r.graphics.instantiateAtPlayhead(userKey("gaming-subscribe")).isEmpty());
    const ClipId clip = r.controller.primaryClip();
    CHECK(r.graphic(clip).width == 1920); // the design canvas stays; the frame fits it
    r.controller.setPlayhead(seconds(12)); // the pop has landed
    canvas.setSelectedLayer(r.graphics.selectedLayer()); // Main.qml keeps the two in step
    REQUIRE(canvas.active());
    // Contain: the full width, 1920 x 1080 scaled to 1080 x 607.5, centred top to bottom.
    const QVariantMap rect = canvas.canvasRect();
    const double top = rect.value(QStringLiteral("y")).toDouble();
    const double height = rect.value(QStringLiteral("height")).toDouble();
    CHECK(rect.value(QStringLiteral("x")).toDouble() == Approx(0.0).margin(1e-9));
    CHECK(rect.value(QStringLiteral("width")).toDouble() == Approx(1.0));
    CHECK(height == Approx(607.5 / 1920.0));
    CHECK(top == Approx((1920.0 - 607.5) / 2.0 / 1920.0));
    // The selected text (the button's label) is inside the fitted canvas, so inside the vertical frame.
    const QVariantMap selection = canvas.selection();
    CHECK(selection.value(QStringLiteral("kind")).toString() == QStringLiteral("text"));
    const QVariantList quad = selection.value(QStringLiteral("quad")).toList();
    REQUIRE(quad.size() == 8);
    for (int i = 0; i < 8; i += 2) {
        CHECK(quad[i].toDouble() > 0.0);
        CHECK(quad[i].toDouble() < 1.0);
        CHECK(quad[i + 1].toDouble() > top);
        CHECK(quad[i + 1].toDouble() < top + height);
    }
    // Typed in place (the monitor's inline editor): the layer's text, one undo step.
    const auto before = r.undo();
    CHECK(canvas.setLayerText(r.graphics.selectedLayer(), QStringLiteral("SUB NOW")).isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.graphic(clip).findLayer(2)->text.text() == "SUB NOW");
}

TEST_CASE("Callouts: built-in templates from the catalog list as Gaming and add at the playhead", "[app][graphics]") {
    QTemporaryDir empty; // the user's library: nothing there, so template.instantiate goes on to the built-ins
    REQUIRE(empty.isValid());
    Timeline tl{kPicture};
    ProjectController controller;
    GraphicsBridge graphics;
    controller.resetProject(tl.state, tl.seq);
    graphics.setUserLibrary(pathOf(empty));
    graphics.setController(&controller);
    graphics.setRegistry(&stubCatalog(true));

    // template.list's "builtin" entries, first, with their category and the poster time in the thumbnail's url.
    for (const std::string& id : kGaming) {
        INFO(id);
        const QVariantMap t = templateOf(graphics, builtinKey(id));
        REQUIRE_FALSE(t.isEmpty());
        CHECK(t.value(QStringLiteral("library")).toString() == QStringLiteral("builtin"));
        CHECK(t.value(QStringLiteral("category")).toString() == QStringLiteral("gaming"));
        CHECK_FALSE(t.value(QStringLiteral("fields")).toList().isEmpty());
        const QStringList url = t.value(QStringLiteral("thumbnail")).toString().split(QLatin1Char('/'));
        REQUIRE(url.size() == 8); // "image:", "", "graphicthumb", "builtin", id, version, kit revision, milliseconds
        const qint64 posterMs = (id == "gaming-countdown" ? 3800 : 4000) / 2;
        CHECK(url.last().toLongLong() == posterMs);
    }
    CHECK(graphics.templates().first().toMap().value(QStringLiteral("library")).toString() == QStringLiteral("builtin"));
    // template.resolve (the stand-in) draws one at a design time: the draw list the thumbnails paint.
    {
        command::CommandEngine engine(controller.session(), stubCatalog(true), command::EnginePolicy{});
        command::CommandRequest request;
        request.command = "template.resolve";
        request.params = Json{{"template", "gaming-kill-streak"}, {"time", 2 * kTicksPerSecond}};
        const command::CommandResult drawn = engine.execute(request);
        INFO((drawn.ok() ? std::string() : drawn.error->message));
        REQUIRE(drawn.ok());
        CHECK(drawn.data.value("width", 0) == 1920);
        CHECK(drawn.data.value("items", Json::array()).size() >= 4);
    }

    // Added at the playhead: one undo step, the first text field's layer selected.
    controller.setPlayhead(seconds(8));
    const auto before = controller.session().undoCount();
    REQUIRE(graphics.instantiateAtPlayhead(builtinKey("gaming-subscribe")).isEmpty());
    CHECK(controller.session().undoCount() == before + 1);
    const ClipId clip = controller.primaryClip();
    REQUIRE(clip.isValid());
    const Graphic* g = controller.snapshot()->findSequence(tl.seq)->findClip(clip)->clip().graphic();
    REQUIRE(g);
    REQUIRE(g->instance);
    CHECK(g->instance->templateId == "gaming-subscribe");
    CHECK(graphics.selectedLayer() == static_cast<int>(firstTextField(*g)));

    // Read-only.
    CHECK_FALSE(graphics.removeTemplate(builtinKey("gaming-subscribe")).isEmpty());
    CHECK_FALSE(templateOf(graphics, builtinKey("gaming-subscribe")).isEmpty());
    // A catalog with no built-ins lists none.
    graphics.setRegistry(&emptyCatalog());
    CHECK(std::none_of(graphics.templates().begin(), graphics.templates().end(),
                       [](const QVariant& t) { return t.toMap().value(QStringLiteral("library")).toString() == QStringLiteral("builtin"); }));
    graphics.setRegistry(nullptr);
}

TEST_CASE("Callouts on the app's catalog (C.16): the 12 built-in gaming callouts list as Gaming, draw, and add in one "
          "undo step", "[app][graphics]") {
    QTemporaryDir empty;
    REQUIRE(empty.isValid());
    Timeline tl{kPicture};
    ProjectController controller;
    GraphicsBridge graphics;
    controller.resetProject(tl.state, tl.seq);
    graphics.setUserLibrary(pathOf(empty));
    graphics.setController(&controller);
    graphics.setRegistry(nullptr); // ai::edit::editingCommands(), as in the app

    const std::vector<std::string> ids{"wabcut.gaming.subscribe",     "wabcut.gaming.killStreak",  "wabcut.gaming.lowerThird",
                                       "wabcut.gaming.replay",        "wabcut.gaming.countdown",   "wabcut.gaming.chatBubble",
                                       "wabcut.gaming.emotePop",      "wabcut.gaming.arrowCallout", "wabcut.gaming.circleCallout",
                                       "wabcut.gaming.webcamFrame",   "wabcut.gaming.endCard",     "wabcut.gaming.intro"};
    for (const std::string& id : ids) {
        INFO(id);
        const QVariantMap t = templateOf(graphics, builtinKey(id));
        REQUIRE_FALSE(t.isEmpty());
        CHECK(t.value(QStringLiteral("library")).toString() == QStringLiteral("builtin"));
        CHECK(t.value(QStringLiteral("category")).toString() == QStringLiteral("gaming"));
        CHECK_FALSE(t.value(QStringLiteral("name")).toString().isEmpty());
        CHECK(t.value(QStringLiteral("thumbnail")).toString().startsWith(QStringLiteral("image://graphicthumb/builtin/")));
    }
    // The real template.resolve draws one (what the thumbnails paint).
    {
        command::EnginePolicy policy;
        policy.services = graphics.graphicsServices();
        command::CommandEngine engine(controller.session(), ai::edit::editingCommands(), policy);
        command::CommandRequest request;
        request.command = "template.resolve";
        request.params = Json{{"template", "wabcut.gaming.killStreak"}};
        const command::CommandResult drawn = engine.execute(request);
        INFO((drawn.ok() ? std::string() : drawn.error->message));
        REQUIRE(drawn.ok());
        CHECK_FALSE(drawn.data.value("items", Json::array()).empty());
    }
    // Added at the playhead: one undo step, a graphic clip from the built-in, a text layer selected, ready to type.
    controller.setPlayhead(seconds(8));
    const auto before = controller.session().undoCount();
    REQUIRE(graphics.instantiateAtPlayhead(builtinKey("wabcut.gaming.subscribe")).isEmpty());
    CHECK(controller.session().undoCount() == before + 1);
    const ClipId clip = controller.primaryClip();
    REQUIRE(clip.isValid());
    const Graphic* g = controller.snapshot()->findSequence(tl.seq)->findClip(clip)->clip().graphic();
    REQUIRE(g);
    REQUIRE(g->instance);
    CHECK(g->instance->templateId == "wabcut.gaming.subscribe");
    CHECK(graphics.selectedLayer() >= 0);
}

TEST_CASE("Callouts panel: Import Lottie sticker asks for the import and the sticker shows under Stickers; an empty "
          "catalog says gaming templates are coming",
          "[app][graphics][qml]") {
    clearMessages();
    QQmlEngine engine;
    QQmlComponent component(&engine, "Wabcut", "GraphicsBrowsePage"); // loads the module and its singletons
    INFO(component.errorString().toStdString());
    REQUIRE_FALSE(component.isError());
    auto* controller = engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* graphics = engine.singletonInstance<GraphicsBridge*>("Wabcut", "Graphics");
    auto* import = engine.singletonInstance<GraphicsImportBridge*>("Wabcut", "GraphicsImport");
    REQUIRE(controller);
    REQUIRE(graphics);
    REQUIRE(import);
    QTemporaryDir library;
    REQUIRE(library.isValid());
    graphics->setUserLibrary(pathOf(library));
    graphics->setRegistry(&emptyCatalog());
    Timeline tl(kPicture);
    controller->resetProject(tl.state, tl.seq);
    controller->setPlayhead(seconds(1));
    import->setSynchronous(true);
    std::unique_ptr<QObject> page(component.create());
    REQUIRE(page);
    auto* item = qobject_cast<QQuickItem*>(page.get());
    REQUIRE(item);
    item->setSize(QSizeF(460, 900));

    // No built-in templates yet: the Gaming group says so.
    QQuickItem* gamingEmpty = findNamed(item, QStringLiteral("graphicsGamingTemplatesEmpty"));
    REQUIRE(gamingEmpty);
    CHECK(gamingEmpty->isVisible());
    CHECK(gamingEmpty->property("text").toString() == QStringLiteral("Gaming templates arrive with the next update."));
    QQuickItem* moreBuiltin = findNamed(item, QStringLiteral("calloutsMoreBuiltin"));
    REQUIRE(moreBuiltin);
    CHECK_FALSE(moreBuiltin->isVisible());

    // The button asks the window for its Lottie file dialog, to keep what is chosen as a sticker.
    QVariantList requests;
    QObject::connect(import, &GraphicsImportBridge::importRequested, [&](const QVariantMap& options) { requests.append(options); });
    QQuickItem* button = findNamed(item, QStringLiteral("calloutsImportSticker"));
    REQUIRE(button);
    CHECK(button->isVisible());
    click(button);
    REQUIRE(requests.size() == 1);
    const QVariantMap options = requests.front().toMap();
    CHECK(options.value(QStringLiteral("saveAsTemplate")).toBool());
    // File > Import Lottie… asks with no options.
    import->openImport();
    REQUIRE(requests.size() == 2);
    CHECK(requests.back().toMap().isEmpty());

    // What the window then does with the file chosen: the animation at the playhead, kept as "lottie.<name>", which the
    // Stickers group shows (not "This project").
    const QString file = QDir(QStringLiteral(WABCUT_INTERCHANGE_FIXTURES)).filePath(QStringLiteral("lottie/lower_third.json"));
    const auto before = controller->session().undoCount();
    REQUIRE(import->importLottie(QUrl::fromLocalFile(file), options).isEmpty());
    CHECK(controller->session().undoCount() > before);
    QQuickItem* stickers = findNamed(item, QStringLiteral("graphicsStickerTemplates"));
    REQUIRE(stickers);
    CHECK(findNamed(stickers, QStringLiteral("graphicTemplate_project/lottie.lower_third")));
    CHECK_FALSE(findNamed(findNamed(item, QStringLiteral("graphicsProjectTemplates")),
                          QStringLiteral("graphicTemplate_project/lottie.lower_third")));
    graphics->setRegistry(nullptr);
}

TEST_CASE("Callouts panel: Browse shows the Gaming templates with thumbnails off the GUI thread; Edit keeps Advanced "
          "closed; screenshots",
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
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* graphics = shell.engine.singletonInstance<GraphicsBridge*>("Wabcut", "Graphics");
    REQUIRE(controller);
    REQUIRE(graphics);
    graphics->setUserLibrary(pathOf(library));
    graphics->setRegistry(&stubCatalog(true)); // C.16's built-ins, stood in for by the fixtures
    // A sticker in the project's library: what Import Lottie sticker… keeps ("lottie.<name>").
    Timeline tl(kPicture);
    auto added = edit::addGraphic(tl.state, tl.seq, tl.track("V2"), Ticks{}, seconds(2), starBurst());
    REQUIRE(added);
    controller->resetProject(added->state, tl.seq);
    controller->selectClip(clipKey(added->placed.at(0)));
    REQUIRE(graphics->saveTemplate(QStringLiteral("lottie.star-burst"), QStringLiteral("Star burst"), {}, QStringLiteral("project"), {})
                .isEmpty());
    controller->clearSelection();

    // The provider paints a built-in's thumbnail on its pool from template.resolve's draw list: drawn, not on the GUI
    // thread. Without template.resolve, from the graphic template.list gives for its id.
    auto* provider = dynamic_cast<QQuickAsyncImageProvider*>(shell.engine.imageProvider(QStringLiteral("graphicthumb")));
    REQUIRE(provider);
    const QString url = templateOf(*graphics, builtinKey("gaming-kill-streak")).value(QStringLiteral("thumbnail")).toString();
    REQUIRE(url.startsWith(QStringLiteral("image://graphicthumb/builtin/")));
    const QImage resolved = thumbnail(provider, url, QSize(320, 180));
    REQUIRE_FALSE(resolved.isNull());
    CHECK(drawnPixels(resolved) > 200); // the banner, the badge and the text
    CHECK_FALSE(graphics->lastThumbnailOnGuiThread());
    graphics->setRegistry(&stubCatalog(false));
    const QImage listed = thumbnail(provider, url, QSize(160, 90)); // another size: not the cached one
    REQUIRE_FALSE(listed.isNull());
    CHECK(drawnPixels(listed) > 50);
    graphics->setRegistry(&stubCatalog(true));

    // Browse (nothing selected): the Gaming group first; the sticker under Stickers.
    const auto panel = makePanel(shell, QRectF(40, 40, 460, 900));
    QQuickItem* browse = findNamed(panel.get(), QStringLiteral("graphicsBrowsePage"));
    REQUIRE(browse);
    REQUIRE(waitFor([&] { return browse->isVisible(); }, 3000));
    QQuickItem* gaming = findNamed(panel.get(), QStringLiteral("graphicsGamingTemplates"));
    REQUIRE(gaming);
    for (const std::string& id : kGaming)
        REQUIRE(findNamed(gaming, QStringLiteral("graphicTemplate_") + builtinKey(id)));
    QQuickItem* gamingEmpty = findNamed(panel.get(), QStringLiteral("graphicsGamingTemplatesEmpty"));
    REQUIRE(gamingEmpty);
    CHECK_FALSE(gamingEmpty->isVisible());
    QQuickItem* subscribe = findNamed(gaming, QStringLiteral("graphicTemplate_builtin/gaming-subscribe"));
    CHECK(mapOf(subscribe->property("modelData")).value(QStringLiteral("name")).toString() == QStringLiteral("Subscribe"));
    REQUIRE(findNamed(findNamed(panel.get(), QStringLiteral("graphicsStickerTemplates")),
                      QStringLiteral("graphicTemplate_project/lottie.star-burst")));
    REQUIRE(findNamed(panel.get(), QStringLiteral("calloutsImportSticker")));
    REQUIRE_FALSE(shell.grab().isNull()); // a frame lays the cards out (hidden windows polish on render): final sizes
    QList<QQuickItem*> images;
    findAllNamed(panel.get(), QStringLiteral("graphicTemplateImage"), images);
    REQUIRE(images.size() >= 4);
    REQUIRE(waitFor(
        [&] {
            return std::all_of(images.begin(), images.end(), [](QQuickItem* image) { return image->property("status").toInt() == 1; });
        },
        20000)); // Image.Ready
    CHECK_FALSE(graphics->lastThumbnailOnGuiThread());
    const QStringList browseIssues = auditAccessibility(panel.get());
    INFO(browseIssues.join(QLatin1Char('\n')).toStdString());
    CHECK(browseIssues.isEmpty());
    CHECK(tabReaches(panel.get(), QStringLiteral("graphicTemplate_builtin/gaming-subscribe")));
    CHECK(tabReaches(panel.get(), QStringLiteral("calloutsImportSticker")));
    spin(50);
    const QImage browseShot = shell.grab();
    REQUIRE_FALSE(browseShot.isNull());
    saveCrop(browseShot, panel.get(), QStringLiteral("callouts-browser.png"));

    // Enter on Subscribe adds it at the playhead, one undo step; the panel turns to Edit with its label selected.
    controller->setPlayhead(seconds(6));
    const auto before = controller->session().undoCount();
    REQUIRE(pressWithKey(shell.window.get(), subscribe, Qt::Key_Return));
    CHECK(controller->session().undoCount() == before + 1);
    REQUIRE_FALSE(graphics->clip().isEmpty());
    QQuickItem* edit = findNamed(panel.get(), QStringLiteral("graphicsEditPage"));
    REQUIRE(edit);
    REQUIRE(waitFor([&] { return edit->isVisible(); }, 3000));
    CHECK(graphics->selectedLayer() == 2);

    // Advanced is closed: no layer list, no property sheet made; the fields and the basics are there.
    CHECK_FALSE(edit->property("advanced").toBool());
    QQuickItem* advanced = findNamed(panel.get(), QStringLiteral("graphicsAdvancedSection"));
    REQUIRE(advanced);
    CHECK_FALSE(advanced->isVisible());
    CHECK_FALSE(findNamed(panel.get(), QStringLiteral("graphicLayerProperties")));
    QQuickItem* field = findNamed(panel.get(), QStringLiteral("graphicFieldText_label"));
    REQUIRE(field);
    CHECK(field->isVisible());
    CHECK(field->property("text").toString() == QStringLiteral("SUBSCRIBE"));
    QQuickItem* text = findNamed(panel.get(), QStringLiteral("calloutText"));
    REQUIRE(text);
    CHECK(text->isVisible());
    CHECK(text->property("text").toString() == QStringLiteral("SUBSCRIBE"));

    // The basics edit the selected layer: typing is one undo step, then the size, the font and the colour.
    auto steps = controller->session().undoCount();
    text->setProperty("text", QStringLiteral("FOLLOW"));
    REQUIRE(QMetaObject::invokeMethod(text, "textEdited"));
    REQUIRE(QMetaObject::invokeMethod(text, "editingFinished"));
    CHECK(controller->session().undoCount() == steps + 1);
    CHECK(graphics->layer().value(QStringLiteral("text")).toMap().value(QStringLiteral("text")).toString() == QStringLiteral("FOLLOW"));
    CHECK(field->property("text").toString() == QStringLiteral("FOLLOW"));
    QQuickItem* size = findNamed(panel.get(), QStringLiteral("calloutSize"));
    REQUIRE(size);
    REQUIRE(QMetaObject::invokeMethod(size, "valueModified", Q_ARG(double, 64.0)));
    REQUIRE(QMetaObject::invokeMethod(size, "gestureEnded"));
    CHECK(runOf(graphics->layer()).value(QStringLiteral("fontSize")).toDouble() == Approx(64.0));
    QQuickItem* font = findNamed(panel.get(), QStringLiteral("calloutFont"));
    REQUIRE(font);
    REQUIRE(QMetaObject::invokeMethod(font, "chosen", Q_ARG(QString, QStringLiteral("IBM Plex Sans"))));
    CHECK(runOf(graphics->layer()).value(QStringLiteral("fontFamily")).toString() == QStringLiteral("IBM Plex Sans"));
    QQuickItem* colour = findNamed(panel.get(), QStringLiteral("calloutColor"));
    REQUIRE(colour);
    REQUIRE(QMetaObject::invokeMethod(colour, "picked", Q_ARG(QString, QStringLiteral("#FACC15"))));
    CHECK(runOf(graphics->layer()).value(QStringLiteral("fill")).toMap().value(QStringLiteral("color")).toString() == QStringLiteral("#FACC15"));
    CHECK(controller->session().undoCount() == steps + 4);

    REQUIRE_FALSE(shell.grab().isNull()); // polish the hidden window's layouts first
    const QStringList editIssues = auditAccessibility(panel.get());
    INFO(editIssues.join(QLatin1Char('\n')).toStdString());
    CHECK(editIssues.isEmpty());
    CHECK(tabReaches(panel.get(), QStringLiteral("graphicsAdvanced")));
    spin(50);
    const QImage editShot = shell.grab();
    REQUIRE_FALSE(editShot.isNull());
    saveCrop(editShot, panel.get(), QStringLiteral("callouts-edit.png"));

    // Advanced opens the deep editing: the layer tree and every property of the selected layer.
    click(findNamed(panel.get(), QStringLiteral("graphicsAdvanced")));
    CHECK(edit->property("advanced").toBool());
    CHECK(advanced->isVisible());
    REQUIRE(findNamed(panel.get(), QStringLiteral("graphicLayerProperties")));
    REQUIRE(findNamed(panel.get(), QStringLiteral("graphicLayerRow_10")));
    REQUIRE(findNamed(panel.get(), QStringLiteral("graphicLayerRow_2")));
    graphics->setRegistry(nullptr);
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}
