// Motion graphics model (C.13): validation of untrusted graphics, the JSON format, project files with graphics, brand
// kits and templates (and schema 11 files without them), text units and the text-range selector, and the draw-list
// resolver the renderer rasterises.

#include "TimelineFixture.h"

#include <wabcut/model/CaptionStyle.h>
#include <wabcut/model/GraphicJson.h>
#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/GraphicResolve.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

using namespace wabcut;
using namespace wabcut::test;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {

GraphicLayer textLayer(std::uint32_t id, std::string text, double x = 0.0, double y = 0.0, double size = 100.0) {
    GraphicLayer l;
    l.id = id;
    l.name = "Text " + std::to_string(id);
    l.kind = LayerKind::Text;
    TextStyle style;
    style.fontSize = size;
    l.text.runs = {TextRun{std::move(text), style}};
    l.transform.x = ParamValue(x);
    l.transform.y = ParamValue(y);
    return l;
}

GraphicLayer rectLayer(std::uint32_t id, double w, double h, double x = 0.0, double y = 0.0) {
    GraphicLayer l;
    l.id = id;
    l.name = "Box " + std::to_string(id);
    l.kind = LayerKind::Shape;
    l.shape.kind = ShapeKind::Rectangle;
    l.shape.width = ParamValue(w);
    l.shape.height = ParamValue(h);
    l.transform.x = ParamValue(x);
    l.transform.y = ParamValue(y);
    return l;
}

GraphicLayer groupLayer(std::uint32_t id, std::vector<GraphicLayer> children) {
    GraphicLayer l;
    l.id = id;
    l.name = "Group " + std::to_string(id);
    l.kind = LayerKind::Group;
    l.children = std::move(children);
    return l;
}

Mask rectangleMask(std::uint32_t id, double cx, double cy, double sx, double sy) {
    Mask m;
    m.id = id;
    m.name = "Mask " + std::to_string(id);
    m.shape = MaskShape::Rectangle;
    m.set("center.x", ParamValue(cx));
    m.set("center.y", ParamValue(cy));
    m.set("size.x", ParamValue(sx));
    m.set("size.y", ParamValue(sy));
    return m;
}

Curve curve(std::initializer_list<std::pair<Ticks, double>> keys) {
    Curve c;
    for (const auto& [t, v] : keys) {
        Keyframe k;
        k.time = t;
        k.value = v;
        c.keys.push_back(k);
    }
    return c;
}

/// A graphic that uses every part of the model.
Graphic richGraphic() {
    Graphic g;
    g.name = "Lower third";
    g.width = 1920;
    g.height = 1080;

    GraphicLayer box = rectLayer(1, 600, 120, 200, 900);
    box.shape.cornerRadius = ParamValue(curve({{f(0), 0.0}, {f(10), 24.0}}));
    box.shape.fill.kind = PaintKind::Linear;
    box.shape.fill.stops = {GradientStop{0.0, GraphicColor{{1.0, 0.0, 0.5, 1.0}, "Primary"}},
                            GradientStop{1.0, GraphicColor{{0.1, 0.1, 0.1, 0.8}, {}}}};
    box.shape.fill.start = {-300.0, 0.0};
    box.shape.fill.end = {300.0, 0.0};
    box.shape.stroke.enabled = true;
    box.shape.stroke.width = ParamValue(3.5);
    box.shape.stroke.dashes = {10.0, 4.0};
    box.shape.stroke.join = LineJoin::Round;
    box.shape.stroke.cap = LineCap::Square;
    box.shape.trimEnd = ParamValue(curve({{f(0), 0.0}, {f(12), 100.0}}));
    box.blend = BlendMode::Screen;
    box.shadow.enabled = true;
    box.shadow.color.brand = "Shadow";
    box.masks.push_back(rectangleMask(1, 0.5, 0.5, 0.3, 0.2));

    GraphicLayer name = textLayer(2, "Somchai สมชาย", 0, 0, 64);
    name.text.runs.push_back(TextRun{"\nProducer", TextStyle{}});
    name.text.runs[1].style.brandFont = "body";
    name.text.runs[1].style.fontSize = 40;
    name.text.runs[1].style.italic = true;
    name.text.runs[1].style.fill = GraphicColor{{0.9, 0.9, 0.9, 1.0}, "Accent"};
    name.text.runs[0].style.fontFamily = "Noto Sans Thai";
    name.text.runs[0].style.fontStyle = "Bold";
    name.text.runs[0].style.fontWeight = 700;
    name.text.runs[0].style.tracking = 50;
    name.text.runs[0].style.leading = 80;
    name.text.runs[0].style.underline = true;
    name.text.runs[0].style.baselineShift = -2;
    name.text.align = TextAlign::Center;
    name.text.language = "th";
    name.text.background.enabled = true;
    name.text.background.radius = 6;
    TextAnimator typewriter;
    typewriter.name = "Typewriter In";
    typewriter.start = ParamValue(curve({{f(0), 0.0}, {f(12), 100.0}}));
    typewriter.opacity = 0.0;
    typewriter.shape = TextSelectorShape::Ramp;
    typewriter.softness = 2.0;
    typewriter.offsetY = 12.0;
    name.text.animators.push_back(typewriter);
    name.transform.x = ParamValue(200.0);
    name.transform.y = ParamValue(860.0);

    GraphicLayer frame = rectLayer(3, 10, 10);
    frame.pin = LayerPin{2, LayerPin::kLeft | LayerPin::kTop | LayerPin::kRight | LayerPin::kBottom, {-16, -8, 16, 8}};
    frame.shape.fill.kind = PaintKind::None;
    frame.shape.stroke.enabled = true;

    GraphicLayer star = rectLayer(4, 0, 0, 1700, 120);
    star.shape.kind = ShapeKind::Star;
    star.shape.points = 6;
    star.shape.outerRadius = ParamValue(60.0);
    star.shape.innerRadius = ParamValue(25.0);
    star.transform.rotation = ParamValue(curve({{f(0), 0.0}, {f(25), 360.0}}));
    star.in = f(5);
    star.out = f(40);

    GraphicLayer path = rectLayer(5, 0, 0, 100, 100);
    path.shape.kind = ShapeKind::Path;
    path.shape.closed = false;
    path.shape.path = {MaskPathKey{f(0), {{0, 0, 0, 0, 10, 0}, {50, 40, -10, 0, 0, 0}, {100, 0, 0, 0, 0, 0}}, Interpolation::EaseInOut},
                       MaskPathKey{f(10), {{0, 10, 0, 0, 10, 0}, {50, 60, -10, 0, 0, 0}, {100, 10, 0, 0, 0, 0}}, Interpolation::Linear}};

    GraphicLayer logo;
    logo.id = 6;
    logo.kind = LayerKind::Image;
    logo.image.brandLogo = "Main";
    logo.image.fit = ImageFit::Cover;
    logo.image.width = 160;
    logo.image.height = 90;
    logo.visible = false;

    GraphicLayer group = groupLayer(7, {std::move(star), std::move(path), std::move(logo)});
    group.transform.opacity = ParamValue(80.0);
    group.transform.anchorX = ParamValue(10.0);

    g.layers = {std::move(box), std::move(name), std::move(frame), std::move(group)};
    g.responsive = ResponsiveTime{Ticks::fromSeconds(5), Ticks::fromSeconds(1), Ticks::fromSeconds(1)};
    g.instance = TemplateInstance{"lower-third", 3, {TemplateField{"name", "Name", FieldKind::Text, 2, "text", FieldValue{"Somchai สมชาย"}}}};
    return g;
}

BrandKit kit(std::array<double, 4> primary, std::string bodyFont = "Inter") {
    BrandKit k;
    k.name = "Studio";
    k.colors = {BrandColor{"Primary", primary}, BrandColor{"Accent", {0.0, 0.5, 1.0, 1.0}}};
    k.fonts = {BrandFont{"body", std::move(bodyFont), "Regular", 400}, BrandFont{"heading", "Inter Display", "Black", 900}};
    return k;
}

std::string replaceOnce(std::string text, std::string_view from, std::string_view to) {
    const auto at = text.find(from);
    REQUIRE(at != std::string::npos);
    return text.replace(at, from.size(), to);
}

const DrawItem* itemOf(const DrawList& list, std::uint32_t layer) {
    for (const DrawItem& i : list.items) {
        if (i.layer == layer) {
            return &i;
        }
    }
    return nullptr;
}

} // namespace

// ---- Validation -------------------------------------------------------------------------------------------------------

TEST_CASE("graphicProblem accepts a full graphic and names what is wrong with a bad one", "[model][graphics]") {
    const Graphic good = richGraphic();
    INFO(graphicProblem(good).value_or(""));
    CHECK_FALSE(graphicProblem(good));

    const auto problem = [&](auto&& mutate) {
        Graphic g = good;
        mutate(g);
        return graphicProblem(g).value_or("");
    };
    CHECK(problem([](Graphic& g) { g.layers[0].id = 0; }) != "");
    CHECK(problem([](Graphic& g) { g.layers[1].id = 1; }) != "");
    CHECK(problem([](Graphic& g) { g.width = 0; }) != "");
    CHECK(problem([](Graphic& g) { g.height = kMaxGraphicCanvas + 1; }) != "");
    CHECK(problem([](Graphic& g) { g.layers[0].transform.x = ParamValue(std::nan("")); }) != "");
    CHECK(problem([](Graphic& g) { g.layers[0].transform.x = ParamValue(2 * kMaxGraphicCoordinate); }) != "");
    CHECK(problem([](Graphic& g) { g.layers[0].transform.opacity = ParamValue(150.0); }) != "");
    CHECK(problem([](Graphic& g) { g.layers[0].shape.fill.stops.resize(1); }) != "");
    CHECK(problem([](Graphic& g) { g.layers[0].shape.fill.stops[1].offset = -0.5; }) != "");
    CHECK(problem([](Graphic& g) { g.layers[0].shape.stroke.dashes.assign(kMaxDashes + 1, 1.0); }) != "");
    CHECK(problem([](Graphic& g) { g.layers[3].children[0].shape.points = 2; }) != "");
    CHECK(problem([](Graphic& g) { g.layers[1].text.runs[0].text = "bad \xff utf-8"; }) != "");
    CHECK(problem([](Graphic& g) { g.layers[1].text.runs[0].text = std::string(kMaxGraphicTextBytes + 1, 'a'); }) != "");
    CHECK(problem([](Graphic& g) { g.layers[1].text.runs[0].style.fontSize = 0.0; }) != "");
    CHECK(problem([](Graphic& g) { g.layers[1].text.animators.resize(kMaxTextAnimators + 1); }) != "");
    CHECK(problem([](Graphic& g) { g.layers[1].text.animators[0].softness = -1.0; }) != "");
    CHECK(problem([](Graphic& g) { g.layers[1].text.runs.assign(kMaxTextRuns + 1, TextRun{"a", {}}); }) != "");
    CHECK(problem([](Graphic& g) { g.layers[0].masks.push_back(g.layers[0].masks[0]); }) != ""); // duplicate mask id
    CHECK(problem([](Graphic& g) { g.layers[0].out = Ticks{}; }) != "");                          // out before in
    CHECK(problem([](Graphic& g) { g.layers[2].pin->target = 99; }) != "");
    CHECK(problem([](Graphic& g) { g.layers[2].pin->target = 6; }) != ""); // not a sibling
    CHECK(problem([](Graphic& g) { g.layers[2].pin->target = 3; }) != ""); // itself
    CHECK(problem([](Graphic& g) { // a cycle
              g.layers[1].pin = LayerPin{3, LayerPin::kLeft, {}};
          }) != "");
    CHECK(problem([](Graphic& g) { g.layers[3].children[1].shape.path[1].vertices.pop_back(); }) != "");
    CHECK(problem([](Graphic& g) {
              g.layers[3].children[1].shape.path[0].vertices.assign(kMaxPathVertices + 1, MaskVertex{});
              g.layers[3].children[1].shape.path.resize(1);
          }) != "");
    CHECK(problem([](Graphic& g) { g.layers[3].children[1].shape.path.clear(); }) != "");
    CHECK(problem([](Graphic& g) { g.layers[1].kind = LayerKind::Group; }) != ""); // text content on a group
    CHECK(problem([](Graphic& g) { g.layers[0].children.push_back(textLayer(50, "x")); }) != "");
    CHECK(problem([](Graphic& g) { g.instance->fields[0].property = "fill"; g.instance->fields[0].kind = FieldKind::Image; }) != "");
    CHECK(problem([](Graphic& g) { g.instance->fields[0].layer = 99; }) != "");
    CHECK(problem([](Graphic& g) { g.instance->fields[0].id = "1bad"; }) != "");
    CHECK(problem([](Graphic& g) { g.responsive->intro = Ticks::fromSeconds(4); g.responsive->outro = Ticks::fromSeconds(2); }) != "");

    // Bounds on the whole tree: layer count and depth.
    Graphic many;
    for (std::uint32_t i = 1; i <= kMaxGraphicLayers + 1; ++i) {
        many.layers.push_back(rectLayer(i, 10, 10));
    }
    CHECK(graphicProblem(many));
    many.layers.pop_back();
    CHECK_FALSE(graphicProblem(many));

    GraphicLayer deep = rectLayer(100, 10, 10);
    for (std::uint32_t i = 1; i <= kMaxGraphicDepth; ++i) {
        deep = groupLayer(100 + i, {std::move(deep)});
    }
    Graphic tooDeep;
    tooDeep.layers.push_back(deep);
    CHECK(graphicProblem(tooDeep));
    tooDeep.layers[0] = std::move(tooDeep.layers[0].children[0]);
    CHECK_FALSE(graphicProblem(tooDeep));
}

TEST_CASE("Brand kits and templates are checked like graphics", "[model][graphics]") {
    BrandKit k = kit({1.0, 0.0, 0.0, 1.0});
    CHECK_FALSE(brandKitProblem(k));
    BrandKit dup = k;
    dup.colors.push_back(dup.colors[0]);
    CHECK(brandKitProblem(dup));
    BrandKit badColor = k;
    badColor.colors[0].rgba[3] = 2.0;
    CHECK(brandKitProblem(badColor));
    BrandKit tooMany = k;
    tooMany.colors.clear();
    for (std::size_t i = 0; i <= kMaxBrandColors; ++i) {
        tooMany.colors.push_back(BrandColor{"c" + std::to_string(i), {}});
    }
    CHECK(brandKitProblem(tooMany));
    BrandKit badWeight = k;
    badWeight.fonts[0].weight = 50;
    CHECK(brandKitProblem(badWeight));

    GraphicTemplate t;
    t.id = "lower-third.v2";
    t.name = "Lower third";
    t.graphic = richGraphic();
    t.graphic.instance.reset();
    t.fields = {TemplateField{"name", "Name", FieldKind::Text, 2, "text", FieldValue{"Name"}},
                TemplateField{"accent", "Accent", FieldKind::Color, 1, "fill", {}},
                TemplateField{"showLogo", "Logo", FieldKind::Checkbox, 6, "visible", {}},
                TemplateField{"length", "Length", FieldKind::Duration, 0, "clip", {}}};
    INFO(templateProblem(t).value_or(""));
    CHECK_FALSE(templateProblem(t));
    CHECK(isValidTemplateId("a"));
    CHECK_FALSE(isValidTemplateId("-a"));
    CHECK_FALSE(isValidTemplateId("a/b"));
    CHECK_FALSE(isValidTemplateId(std::string(129, 'a')));
    // Ids name files in the user's library: never a Windows device name.
    for (const char* device : {"con", "NUL", "aux.v2", "com1", "LPT9.x", "prn"}) {
        INFO(device);
        CHECK_FALSE(isValidTemplateId(device));
    }
    CHECK(isValidTemplateId("console"));
    CHECK(isValidTemplateId("com10"));
    CHECK(isValidTemplateId("lpt"));
    CHECK(isValidFieldId("name.first_2"));
    CHECK_FALSE(isValidFieldId("2name"));
    GraphicTemplate badId = t;
    badId.id = "../evil";
    CHECK(templateProblem(badId));
    GraphicTemplate twice = t;
    twice.fields.push_back(twice.fields[0]);
    CHECK(templateProblem(twice));
    GraphicTemplate wrongKind = t;
    wrongKind.fields[2].layer = 2;
    wrongKind.fields[1] = TemplateField{"img", "Image", FieldKind::Image, 2, "media", {}}; // an image field on text
    CHECK(templateProblem(wrongKind));
    GraphicTemplate noName = t;
    noName.name.clear();
    CHECK(templateProblem(noName));
}

// ---- JSON -------------------------------------------------------------------------------------------------------------

TEST_CASE("Graphics round trip through JSON unchanged", "[model][graphics][io]") {
    const Graphic g = richGraphic();
    const std::string text = io::graphicToJson(g);
    const auto back = io::graphicFromJson(text);
    INFO(text);
    REQUIRE(back);
    CHECK(*back == g);
    CHECK(io::graphicToJson(*back) == text);

    // One layer, as the commands take it.
    const auto layer = io::layerFromJson(io::layerToJson(g.layers[1]));
    REQUIRE(layer);
    CHECK(*layer == g.layers[1]);

    // Defaults are left out; colours read as hex too.
    const auto small = io::layerFromJson(R"({"id": 4, "kind": "shape", "shape": {"type": "ellipse", "width": 50,
        "fill": {"color": "#ff8000"}}})");
    REQUIRE(small);
    CHECK(small->shape.kind == ShapeKind::Ellipse);
    CHECK(small->shape.width == ParamValue(50.0));
    CHECK(small->shape.fill.color.rgba[0] == Approx(1.0));
    CHECK(small->shape.fill.color.rgba[1] == Approx(128.0 / 255.0));
    CHECK(small->shape.fill.color.rgba[2] == Approx(0.0));
    const auto withBrand = io::layerFromJson(R"({"id": 1, "kind": "text", "text": {"runs": [
        {"text": "Hi", "fill": {"brand": "Primary", "rgba": [1, 1, 1, 1]}, "brandFont": "heading"}]}})");
    REQUIRE(withBrand);
    CHECK(withBrand->text.runs[0].style.fill.brand == "Primary");
    CHECK(withBrand->text.runs[0].style.brandFont == "heading");

    BrandKit k = kit({0.2, 0.4, 0.6, 1.0});
    k.logos = {BrandLogo{"Main", MediaId{7}}};
    const auto kitBack = io::brandKitFromJson(io::brandKitToJson(k));
    REQUIRE(kitBack);
    CHECK(*kitBack == k);
}

TEST_CASE("Graphic JSON readers refuse hostile input with the path of the problem", "[model][graphics][io]") {
    const auto error = [](std::string_view text) {
        const auto r = io::graphicFromJson(text);
        REQUIRE_FALSE(r);
        return r.error().message;
    };
    CHECK(error("not json") != "");
    CHECK(error("[]") != "");
    CHECK(error(R"({"layers": [{"id": 1, "kind": "hologram"}]})").find("kind") != std::string::npos);
    CHECK(error(R"({"layers": [{"id": 1, "kind": "shape", "transform": {"x": "left"}}]})").find("transform") !=
          std::string::npos);
    CHECK(error(R"({"layers": [{"id": 1, "kind": "shape"}, {"id": 1, "kind": "shape"}]})") != "");
    CHECK(error(R"({"width": 1e300, "layers": []})") != "");
    CHECK(error(R"({"layers": [{"id": -1, "kind": "shape"}]})") != "");
    CHECK(error(R"({"layers": [{"id": 1, "kind": "shape", "in": -5}]})") != "");
    // Nesting: groups deeper than the bound, and raw JSON nesting far deeper still.
    std::string nested = R"({"id": 1, "kind": "shape"})";
    for (int i = 0; i < 12; ++i) {
        nested = R"({"id": )" + std::to_string(i + 2) + R"(, "kind": "group", "children": [)" + nested + "]}";
    }
    CHECK(error(R"({"layers": [)" + nested + "]}") != "");
    CHECK(error(std::string(100'000, '[') + std::string(100'000, ']')) != "");
    // Too big.
    CHECK(error(R"({"name": ")" + std::string(io::kMaxGraphicJsonBytes, 'a') + R"("})") != "");

    CHECK_FALSE(io::layerFromJson(R"({"id": 1, "kind": "text", "text": {"runs": [{"text": 5}]}})"));
    CHECK_FALSE(io::layerFromJson(R"({"id": 1, "kind": "shape", "shape": 5})"));
    CHECK_FALSE(io::layerFromJson(R"({"id": 1, "kind": "text", "text": {"runs": [[]]}})"));
    CHECK_FALSE(io::layerFromJson("[]"));
    CHECK_FALSE(io::brandKitFromJson(R"({"colors": [{"name": "a", "rgba": [1, 1]}]})"));
    CHECK_FALSE(io::templateFileFromJson(R"({"format": "wabcut-graphic-template", "version": 2, "template": {}})"));
    CHECK_FALSE(io::templateFileFromJson(R"({"format": "something", "version": 1, "template": {}})"));
}

TEST_CASE("Template library folders hold one file per template and list broken ones", "[model][graphics][io]") {
    const fs::path folder = fs::temp_directory_path() / "wabcut-test-template-library";
    fs::remove_all(folder);

    GraphicTemplate t;
    t.id = "title.big";
    t.name = "Big title";
    t.description = "A big title";
    t.version = 2;
    t.graphic.layers = {textLayer(1, "Title")};
    t.fields = {TemplateField{"title", "Title", FieldKind::Text, 1, "text", FieldValue{"Title"}}};
    t.duration = Ticks::fromSeconds(4);
    const auto file = io::writeTemplateToLibrary(folder, t);
    REQUIRE(file);
    CHECK(file->filename() == "title.big.wabcut-template.json");
    {
        std::ofstream broken(folder / "broken.wabcut-template.json");
        broken << R"({"format": "wabcut-graphic-template", "version": 1, "template": {"id": "x"}})";
        std::ofstream other(folder / "notes.txt");
        other << "not a template";
    }
    const auto listed = io::readTemplateLibrary(folder);
    REQUIRE(listed.size() == 2);
    CHECK(listed[0].file.filename() == "broken.wabcut-template.json");
    CHECK_FALSE(listed[0].tmpl);
    REQUIRE(listed[1].tmpl);
    CHECK(*listed[1].tmpl == t);
    CHECK(io::readTemplateLibrary(folder / "missing").empty());

    GraphicTemplate evil = t;
    evil.id = "../escape";
    CHECK_FALSE(io::writeTemplateToLibrary(folder, evil));
    fs::remove_all(folder);
}

TEST_CASE("Projects with graphics a brand kit and templates round trip and schema 11 files still load",
          "[model][graphics][io][migration]") {
    Timeline tl(R"(
        V2     |..........|
        V1 tgt |Aaaaaaaaaa|
    )");
    ProjectState s = tl.state;
    Graphic g = richGraphic();
    g.instance.reset();
    s = ok(edit::addGraphic(s, tl.seq, tl.track("V2"), f(2), f(6), g)).state;
    BrandKit k = kit({1.0, 0.0, 0.0, 1.0});
    k.logos = {BrandLogo{"Main", tl.media('A')}};
    s = ok(edit::setBrandKit(s, k)).state;
    auto tmpl = edit::makeTemplate(g, "lower-third", "Lower third",
                                   {TemplateField{"name", "Name", FieldKind::Text, 2, "text", {}}}, f(50));
    REQUIRE(tmpl);
    s = ok(edit::saveProjectTemplate(s, *tmpl)).state;
    CaptionStyle style;
    style.brand.fill = "Primary";
    style.brand.font = "body";

    const auto json = io::toJson(s);
    REQUIRE(json);
    CHECK(json->find("\"schemaVersion\": " + std::to_string(io::kSchemaVersion)) != std::string::npos);
    const auto back = io::fromJson(*json);
    INFO((back ? std::string() : back.error().message));
    REQUIRE(back);
    CHECK(*back == s);
    CHECK(back->brandKit == k);
    REQUIRE(back->templates.size() == 1);
    CHECK(back->templates[0] == *tmpl);

    // A schema 11 file (no graphics, no kit, no templates) loads unchanged.
    const auto plain = io::toJson(tl.state);
    REQUIRE(plain);
    const std::string v11 = replaceOnce(*plain, "\"schemaVersion\": " + std::to_string(io::kSchemaVersion), "\"schemaVersion\": 11");
    const auto old = io::fromJson(v11);
    INFO((old ? std::string() : old.error().message));
    REQUIRE(old);
    CHECK(*old == tl.state);
    CHECK_FALSE(old->brandKit);
    CHECK(old->templates.empty());

    // A graphic on a clip that does not play a graphic canvas is refused, like one with a bad layer.
    const std::string clipGraphic = "\"graphic\": {";
    REQUIRE(json->find(clipGraphic) != std::string::npos);
    const std::string badLayer = replaceOnce(*json, "\"type\": \"star\"", "\"type\": \"blob\"");
    CHECK_FALSE(io::fromJson(badLayer));
}

TEST_CASE("Caption styles take brand colours and fonts and round trip their references", "[model][graphics][captions]") {
    CaptionStyle style;
    style.fill = {1.0, 1.0, 1.0};
    style.brand.fill = "Primary";
    style.brand.box = "Missing";
    style.brand.font = "body";
    CHECK_FALSE(captionStyleProblem(style));

    const BrandKit red = kit({1.0, 0.0, 0.0, 0.5});
    const CaptionStyle themed = resolvedCaptionStyle(style, &red);
    CHECK(themed.fill == CaptionRgb{1.0, 0.0, 0.0});
    CHECK(themed.boxColor == style.boxColor); // no such colour in the kit
    CHECK(themed.fontFamily == "Inter");
    CHECK(themed.fontWeight == 400);
    const BrandKit blue = kit({0.0, 0.0, 1.0, 1.0}, "Sarabun");
    const CaptionStyle rethemed = resolvedCaptionStyle(style, &blue);
    CHECK(rethemed.fill == CaptionRgb{0.0, 0.0, 1.0});
    CHECK(rethemed.fontFamily == "Sarabun");
    CHECK(resolvedCaptionStyle(style, nullptr) == style);

    CaptionStyle bad = style;
    bad.brand.outline = "two\nlines";
    CHECK(captionStyleProblem(bad));
}

// ---- Text units and the range selector --------------------------------------------------------------------------------

TEST_CASE("Grapheme clusters keep Thai marks and emoji sequences together", "[model][graphics][text]") {
    const auto texts = [](std::string_view s) {
        std::vector<std::string> out;
        for (const auto& [b, e] : graphemeClusters(s)) {
            out.emplace_back(s.substr(b, e - b));
        }
        return out;
    };
    CHECK(texts("ab") == std::vector<std::string>{"a", "b"});
    // สวัสดี: sa, wo-with-mai-han-akat, sa, do-with-sara-ii.
    CHECK(texts("\xE0\xB8\xAA\xE0\xB8\xA7\xE0\xB8\xB1\xE0\xB8\xAA\xE0\xB8\x94\xE0\xB8\xB5") ==
          std::vector<std::string>{"\xE0\xB8\xAA", "\xE0\xB8\xA7\xE0\xB8\xB1", "\xE0\xB8\xAA", "\xE0\xB8\x94\xE0\xB8\xB5"});
    // กี่: ko kai with sara ii and mai ek is one cluster.
    CHECK(texts("\xE0\xB8\x81\xE0\xB8\xB5\xE0\xB9\x88").size() == 1);
    // e + combining acute; a family emoji joined by ZWJ; a flag of two regional indicators.
    CHECK(texts("e\xCC\x81x").size() == 2);
    CHECK(texts("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7").size() == 1);
    CHECK(texts("\xF0\x9F\x87\xB9\xF0\x9F\x87\xAD\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5").size() == 2);
    CHECK(texts("a\r\nb") == std::vector<std::string>{"a", "\r\n", "b"});
    CHECK(texts("\xff\xfe").size() == 2); // invalid bytes: one cluster each
}

TEST_CASE("Text units step over characters words and lines", "[model][graphics][text]") {
    const auto units = [](std::string_view s, TextUnit unit) {
        std::vector<std::string> out;
        for (const auto& [b, e] : textUnits(s, unit, "", simpleLineBreaker())) {
            out.emplace_back(s.substr(b, e - b));
        }
        return out;
    };
    CHECK(units("Hi there\nyou", TextUnit::Character) ==
          std::vector<std::string>{"H", "i", " ", "t", "h", "e", "r", "e", "y", "o", "u"});
    CHECK(units("Hi  there\nyou all", TextUnit::Word) == std::vector<std::string>{"Hi", "there", "you", "all"});
    CHECK(units("Hi there\n\nyou\r\n", TextUnit::Line) == std::vector<std::string>{"Hi there", "you"});

    // Unspaced Thai is one word to the simple breaker; a dictionary breaker splits it.
    const std::string thai = "\xE0\xB8\xAA\xE0\xB8\xA7\xE0\xB8\xB1\xE0\xB8\xAA\xE0\xB8\x94\xE0\xB8\xB5"
                             "\xE0\xB8\x84\xE0\xB8\xA3\xE0\xB8\xB1\xE0\xB8\x9A"; // สวัสดีครับ
    CHECK(units(thai, TextUnit::Word).size() == 1);
    struct Dictionary : LineBreaker {
        std::vector<std::size_t> breakOpportunities(std::string_view, std::string_view) const override { return {18}; }
    } dictionary;
    const auto words = textUnits(thai, TextUnit::Word, "th", dictionary);
    REQUIRE(words.size() == 2);
    CHECK(words[0] == std::pair<std::size_t, std::size_t>{0, 18});
    CHECK(words[1] == std::pair<std::size_t, std::size_t>{18, thai.size()});
    CHECK(units(thai, TextUnit::Character).size() == 7);
}

TEST_CASE("The range selector selects units between start and end with an offset and a ramp", "[model][graphics][text]") {
    TextAnimator a;
    a.start = ParamValue(25.0);
    a.end = ParamValue(75.0);
    // Square: units whose centres fall in [1, 3) of 4.
    CHECK(textSelection(a, 0, 4, Ticks{}) == 0.0);
    CHECK(textSelection(a, 1, 4, Ticks{}) == 1.0);
    CHECK(textSelection(a, 2, 4, Ticks{}) == 1.0);
    CHECK(textSelection(a, 3, 4, Ticks{}) == 0.0);
    // The offset moves the range.
    a.offset = ParamValue(25.0);
    CHECK(textSelection(a, 1, 4, Ticks{}) == 0.0);
    CHECK(textSelection(a, 3, 4, Ticks{}) == 1.0);
    a.offset = ParamValue(0.0);
    // Start after end selects the same range.
    std::swap(a.start, a.end);
    CHECK(textSelection(a, 1, 4, Ticks{}) == 1.0);
    // Ramp: one unit of softness fades the edges.
    TextAnimator ramp;
    ramp.start = ParamValue(0.0);
    ramp.end = ParamValue(50.0);
    ramp.shape = TextSelectorShape::Ramp;
    ramp.softness = 2.0;
    // [0, 5) of 10: unit 4's centre is 4.5, half a unit inside the end: (5 - 4.5) / 2 + 0.5 = 0.75.
    CHECK(textSelection(ramp, 4, 10, Ticks{}) == Approx(0.75));
    CHECK(textSelection(ramp, 2, 10, Ticks{}) == Approx(1.0));
    CHECK(textSelection(ramp, 6, 10, Ticks{}) == Approx(0.0));
    CHECK(textSelection(ramp, 0, 0, Ticks{}) == 0.0);
    // A keyed start moves over time.
    TextAnimator keyed;
    keyed.start = ParamValue(curve({{f(0), 0.0}, {f(10), 100.0}}));
    CHECK(textSelection(keyed, 0, 4, f(0)) == 1.0);
    CHECK(textSelection(keyed, 0, 4, f(5)) == 0.0);
    CHECK(textSelection(keyed, 3, 4, f(5)) == 1.0);
    CHECK(textSelection(keyed, 3, 4, f(10)) == 0.0);
}

// ---- The draw list ----------------------------------------------------------------------------------------------------

TEST_CASE("The resolver composes transforms and paths into canvas space", "[model][graphics][resolve]") {
    Graphic g;
    GraphicLayer box = rectLayer(1, 200, 100, 960, 540);
    box.transform.rotation = ParamValue(90.0);
    box.transform.scaleX = ParamValue(50.0);
    box.transform.scaleY = ParamValue(50.0);
    GraphicLayer rounded = rectLayer(2, 200, 100);
    rounded.shape.cornerRadius = ParamValue(20.0);
    GraphicLayer ellipse = rectLayer(3, 100, 50);
    ellipse.shape.kind = ShapeKind::Ellipse;
    GraphicLayer star = rectLayer(4, 0, 0);
    star.shape.kind = ShapeKind::Star;
    star.shape.points = 5;
    GraphicLayer polygon = rectLayer(5, 0, 0);
    polygon.shape.kind = ShapeKind::Polygon;
    polygon.shape.points = 6;
    polygon.shape.outerRadius = ParamValue(50.0);
    GraphicLayer anchored = rectLayer(6, 100, 100, 500, 500);
    anchored.transform.anchorX = ParamValue(-50.0); // the left edge's midpoint sits at the position
    g.layers = {box, rounded, ellipse, star, polygon, anchored};

    const DrawList list = resolveGraphic(g, Ticks{});
    CHECK(list.width == 1920);
    CHECK(list.height == 1080);
    REQUIRE(list.items.size() == 6);
    const DrawItem& b = list.items[0];
    CHECK(b.kind == DrawKind::Shape);
    CHECK(b.bounds == Box{-100, -50, 200, 100});
    REQUIRE(b.path.size() == 4);
    // The right edge's midpoint: scaled by half to (50, 0), turned 90 degrees clockwise to (0, 50), moved to the centre.
    const auto p = b.transform.map(100.0, 0.0);
    CHECK(p[0] == Approx(960.0).margin(1e-9));
    CHECK(p[1] == Approx(590.0));
    CHECK(list.items[1].path.size() == 8);
    CHECK(list.items[2].path.size() == 4);
    CHECK(list.items[2].path[0].outX != 0.0); // ellipse arcs have tangents
    CHECK(list.items[3].path.size() == 10);
    CHECK(list.items[3].path[0].y == Approx(-100.0)); // the first point straight up
    CHECK(list.items[4].path.size() == 6);
    const auto left = list.items[5].transform.map(-50.0, 0.0);
    CHECK(left[0] == Approx(500.0));
    CHECK(left[1] == Approx(500.0));
}

TEST_CASE("Groups multiply opacity and pass their blend mode and masks to their children", "[model][graphics][resolve]") {
    Graphic g;
    GraphicLayer child = rectLayer(2, 100, 100, 10, 0);
    child.transform.opacity = ParamValue(50.0);
    child.masks.push_back(rectangleMask(1, 0.0, 0.0, 0.1, 0.1));
    GraphicLayer screen = rectLayer(3, 10, 10);
    screen.blend = BlendMode::Screen;
    GraphicLayer group = groupLayer(1, {child, screen});
    group.transform.x = ParamValue(100.0);
    group.transform.opacity = ParamValue(50.0);
    group.blend = BlendMode::Multiply;
    group.masks.push_back(rectangleMask(1, 0.5, 0.5, 1.0, 1.0));
    g.layers = {group};

    const DrawList list = resolveGraphic(g, Ticks{});
    REQUIRE(list.items.size() == 2); // groups draw nothing themselves
    const DrawItem& c = list.items[0];
    CHECK(c.layer == 2);
    CHECK(c.opacity == Approx(0.25));
    CHECK(c.blend == BlendMode::Multiply);
    CHECK(list.items[1].blend == BlendMode::Screen);
    CHECK(c.transform.e == Approx(110.0));
    REQUIRE(c.masks.size() == 2); // its own, then the group's: two mattes that multiply
    CHECK(c.masks[0].owner == 2);
    CHECK(c.masks[1].owner == 1);
    // The child's mask: centred on the child's origin, 0.1 canvas wide = 192 px.
    const auto mx = [](const DrawMask& m) {
        double lo = m.outline[0].x;
        double hi = lo;
        for (const MaskVertex& v : m.outline) {
            lo = std::min(lo, v.x);
            hi = std::max(hi, v.x);
        }
        return std::pair{lo, hi};
    };
    CHECK(mx(c.masks[0]).first == Approx(110.0 - 96.0));
    CHECK(mx(c.masks[0]).second == Approx(110.0 + 96.0));
    // The group's mask: the whole canvas, moved with the group.
    CHECK(mx(c.masks[1]).first == Approx(100.0));
    CHECK(mx(c.masks[1]).second == Approx(100.0 + 1920.0));
}

TEST_CASE("Layers show between their in and out points and when visible", "[model][graphics][resolve]") {
    Graphic g;
    GraphicLayer early = rectLayer(1, 10, 10);
    early.out = f(5);
    GraphicLayer late = rectLayer(2, 10, 10);
    late.in = f(5);
    GraphicLayer hidden = rectLayer(3, 10, 10);
    hidden.visible = false;
    g.layers = {early, late, hidden};
    const auto ids = [&](Ticks t) {
        std::vector<std::uint32_t> out;
        for (const DrawItem& i : resolveGraphic(g, t).items) {
            out.push_back(i.layer);
        }
        return out;
    };
    CHECK(ids(f(0)) == std::vector<std::uint32_t>{1});
    CHECK(ids(f(4)) == std::vector<std::uint32_t>{1});
    CHECK(ids(f(5)) == std::vector<std::uint32_t>{2});
}

TEST_CASE("Keyframed properties resolve at the asked time", "[model][graphics][resolve]") {
    Graphic g;
    GraphicLayer box = rectLayer(1, 100, 100);
    box.transform.x = ParamValue(curve({{f(0), 0.0}, {f(10), 100.0}}));
    box.transform.opacity = ParamValue(curve({{f(0), 0.0}, {f(10), 100.0}}));
    box.shape.width = ParamValue(curve({{f(0), 100.0}, {f(10), 300.0}}));
    box.shape.trimEnd = ParamValue(curve({{f(0), 0.0}, {f(10), 100.0}}));
    g.layers = {box};
    const DrawList mid = resolveGraphic(g, f(5));
    REQUIRE(mid.items.size() == 1);
    CHECK(mid.items[0].transform.e == Approx(50.0));
    CHECK(mid.items[0].opacity == Approx(0.5));
    CHECK(mid.items[0].bounds.width == Approx(200.0));
    CHECK(mid.items[0].trimEnd == Approx(0.5));
}

TEST_CASE("Text items carry resolved runs and per-cluster animator values", "[model][graphics][resolve][text]") {
    Graphic g;
    GraphicLayer t = textLayer(1, "abcd", 100, 200);
    t.text.runs.push_back(TextRun{" ef", TextStyle{}});
    t.text.runs[1].style.brandFont = "heading";
    t.text.runs[1].style.fill.brand = "Primary";
    TextAnimator a;
    a.unit = TextUnit::Character;
    a.start = ParamValue(curve({{f(0), 0.0}, {f(7), 100.0}}));
    a.opacity = 0.0;
    a.offsetY = 10.0;
    t.text.animators.push_back(a);
    g.layers = {t};

    const BrandKit k = kit({0.0, 1.0, 0.0, 1.0});
    ResolveOptions options;
    options.brandKit = &k;
    const DrawList list = resolveGraphic(g, f(3), options);
    REQUIRE(list.items.size() == 1);
    const DrawText& text = list.items[0].text;
    REQUIRE(text.runs.size() == 2);
    CHECK(text.runs[0].byteStart == 0);
    CHECK(text.runs[1].byteStart == 4);
    CHECK(text.runs[1].fontFamily == "Inter Display");
    CHECK(text.runs[1].fontStyle == "Black");
    CHECK(text.runs[1].fontWeight == 900);
    CHECK(text.runs[1].fill == Rgba{0.0, 1.0, 0.0, 1.0});
    // 7 clusters; at frame 3 the selection starts at 3/7 of them: the first three show, the rest are hidden and lowered.
    REQUIRE(text.clusters.size() == 7);
    CHECK(text.clusters[2].opacity == 1.0);
    CHECK(text.clusters[2].offsetY == 0.0);
    CHECK(text.clusters[3].opacity == 0.0);
    CHECK(text.clusters[3].offsetY == 10.0);
    CHECK(text.clusters[6].byteStart == 6);
    CHECK(text.clusters[6].byteEnd == 7);

    // Without animators there are no clusters; without the kit the run keeps its own font and colour.
    g.layers[0].text.animators.clear();
    const DrawList plain = resolveGraphic(g, f(3));
    CHECK(plain.items[0].text.clusters.empty());
    CHECK(plain.items[0].text.runs[1].fontFamily.empty());
    CHECK(plain.items[0].text.runs[1].fill == Rgba{1.0, 1.0, 1.0, 1.0});
    // Measured bounds: 0.55 em per character (4 at 100 px, 3 at 72 px), 1.2 em lines; centred text is centred on the
    // origin.
    const double width = 4 * 55.0 + 3 * 0.55 * 72.0;
    CHECK(plain.items[0].bounds.width == Approx(width));
    CHECK(plain.items[0].bounds.height == Approx(120.0));
    g.layers[0].text.align = TextAlign::Center;
    CHECK(resolveGraphic(g, f(3)).items[0].bounds.x == Approx(-width / 2.0));
}

TEST_CASE("Pinned layers follow their target and boxes pinned on both sides resize", "[model][graphics][resolve]") {
    Graphic g;
    GraphicLayer name = textLayer(1, "Hello", 100, 100); // 275 x 120 at (100, 100)
    GraphicLayer box = rectLayer(2, 10, 10);
    box.pin = LayerPin{1, LayerPin::kLeft | LayerPin::kTop | LayerPin::kRight | LayerPin::kBottom, {-10, -10, 10, 10}};
    GraphicLayer tag = rectLayer(3, 40, 20);
    tag.pin = LayerPin{2, LayerPin::kLeft | LayerPin::kBottom, {0, 0, 0, 30}}; // under the box, left-aligned
    g.layers = {box, tag, name}; // pins do not depend on the order

    const auto check = [&](double textWidth) {
        const DrawList list = resolveGraphic(g, Ticks{});
        const DrawItem* b = itemOf(list, 2);
        const DrawItem* t = itemOf(list, 3);
        REQUIRE(b);
        REQUIRE(t);
        CHECK(b->bounds.width == Approx(textWidth + 20.0));
        CHECK(b->bounds.height == Approx(140.0));
        CHECK(b->transform.e == Approx(90.0 + (textWidth + 20.0) / 2.0));
        CHECK(b->transform.f == Approx(160.0));
        // The tag's left edge on the box's, its bottom 30 px below the box's.
        CHECK(t->transform.e - 20.0 == Approx(90.0));
        CHECK(t->transform.f + 10.0 == Approx(230.0 + 30.0));
        CHECK(t->bounds.width == Approx(40.0)); // pinned on one side per axis: moved, not resized
    };
    check(275.0);
    g.layers[2].text.runs[0].text = "Hello world!";
    check(660.0);
}

TEST_CASE("Images resolve brand logos and keep their own media otherwise", "[model][graphics][resolve]") {
    Graphic g;
    GraphicLayer logo;
    logo.id = 1;
    logo.kind = LayerKind::Image;
    logo.image.media = MediaId{3};
    logo.image.brandLogo = "Main";
    g.layers = {logo};
    BrandKit k = kit({1, 1, 1, 1});
    k.logos = {BrandLogo{"Main", MediaId{9}}};
    ResolveOptions options;
    options.brandKit = &k;
    CHECK(resolveGraphic(g, Ticks{}, options).items[0].image.media == MediaId{9});
    CHECK(resolveGraphic(g, Ticks{}).items[0].image.media == MediaId{3});
    k.logos[0].name = "Other";
    CHECK(resolveGraphic(g, Ticks{}, options).items[0].image.media == MediaId{3});
    CHECK(resolveGraphic(g, Ticks{}).items[0].image.width == 400.0);
}

TEST_CASE("Responsive time keeps the intro and outro and stretches the middle", "[model][graphics][resolve]") {
    const ResponsiveTime r{Ticks::fromSeconds(5), Ticks::fromSeconds(1), Ticks::fromSeconds(1)};
    const auto at = [&](double since, double clip) {
        return responsiveDesignTime(r, Ticks{static_cast<std::int64_t>(since * kTicksPerSecond)},
                                    Ticks{static_cast<std::int64_t>(clip * kTicksPerSecond)})
                   .toSeconds();
    };
    // A 9 s clip: the intro plays as designed, the 3 s middle stretches over 7 s, the outro ends with the clip.
    CHECK(at(0.5, 9) == Approx(0.5));
    CHECK(at(4.5, 9) == Approx(1.0 + 3.5 * 3.0 / 7.0));
    CHECK(at(8.5, 9) == Approx(4.5));
    CHECK(at(9.0, 9) == Approx(5.0));
    // A 3 s clip squeezes the middle.
    CHECK(at(1.5, 3) == Approx(2.5));
    // A 1 s clip is too short for both: intro then outro, in proportion.
    CHECK(at(0.25, 1) == Approx(0.5));
    CHECK(at(0.75, 1) == Approx(4.5));
    // Out of range clamps.
    CHECK(at(-1, 9) == Approx(0.0));
    CHECK(at(0.5, 0) == Approx(0.0));
}
