// Lottie import (C.13): the lower-third fixture (shapes, parenting, easing, text, masks), the feature tour of what is
// approximated or dropped, and hostile input.

#include "InterchangeTestSupport.h"

#include <wabcut/interchange/Lottie.h>
#include <wabcut/model/GraphicResolve.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

using namespace wabcut;
using namespace wabcut::interchange;
using namespace wabcut::interchange::test;
using Catch::Approx;

namespace {

constexpr auto O = ReportOutcome::Carried;
constexpr auto A = ReportOutcome::Approximated;
constexpr auto D = ReportOutcome::Dropped;
constexpr auto G = ReportCategory::Graphic;

LottieImport imported(std::string_view fixture) {
    const auto r = importLottie(readFixture(fixture), fixture);
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    REQUIRE_FALSE(graphicProblem(r->graphic));
    return *r;
}

Ticks frame(double f) { return Ticks{static_cast<std::int64_t>(std::llround(f / 30.0 * static_cast<double>(kTicksPerSecond)))}; }

std::string importError(std::string_view text) {
    const auto r = importLottie(text);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::ParseError);
    CHECK(r.error().message.find("internal") == std::string::npos);
    return r.error().message;
}

/// Every report entry, one per line (shown when a check fails).
std::string listed(const ImportReport& report) {
    std::string out;
    for (const ReportEntry& e : report.entries()) {
        out += std::string(toString(e.outcome)) + " " + std::string(toString(e.category)) + " " + e.reason + " at " +
               e.location.element + "\n";
    }
    return out;
}

/// A minimal animation around `layers` (a JSON array).
std::string animation(const std::string& layers, const std::string& extra = {}) {
    return R"({"v": "5.7.0", "fr": 30, "ip": 0, "op": 60, "w": 1000, "h": 500, "layers": )" + layers + extra + "}";
}

} // namespace

TEST_CASE("Lottie: a lower third imports its shapes parents easing text and masks", "[interchange][lottie]") {
    const LottieImport r = imported("lottie/lower_third.json");
    INFO(listed(r.report));
    const Graphic& g = r.graphic;
    CHECK(g.name == "Lower Third");
    CHECK(g.width == 1920);
    CHECK(g.height == 1080);
    CHECK(r.duration == Ticks::fromSeconds(3));
    CHECK(r.frameRate == Rate{30, 1});
    // Bottom to top: the backdrop, the star, then the bar and the name inside their parent's transform (the null
    // layer itself draws nothing).
    REQUIRE(g.layers.size() == 4);

    // The solid: a 1920 x 200 rectangle centred where Lottie's (0, 0)..(1920, 200) box sits, its mask moved with it.
    const GraphicLayer& backdrop = g.layers[0];
    CHECK(backdrop.name == "Backdrop");
    CHECK(backdrop.kind == LayerKind::Shape);
    CHECK(backdrop.shape.width == ParamValue(1920.0));
    CHECK(backdrop.shape.height == ParamValue(200.0));
    CHECK(backdrop.shape.fill.color.rgba[0] == Approx(0x20 / 255.0));
    CHECK(backdrop.shape.fill.color.rgba[2] == Approx(0x40 / 255.0));
    CHECK(backdrop.transform.anchorX == ParamValue(0.0));
    CHECK(backdrop.transform.anchorY == ParamValue(0.0));
    REQUIRE(backdrop.masks.size() == 1);
    CHECK(backdrop.masks[0].shape == MaskShape::Path);
    CHECK(backdrop.masks[0].path.at(0).vertices.at(0).x == Approx(-0.5));
    CHECK(backdrop.masks[0].path.at(0).vertices.at(0).y == Approx(-100.0 / 1080.0));
    const DrawList list = resolveGraphic(g, frame(30));
    const DrawItem& drawn = list.items.at(0);
    const auto topLeft = drawn.transform.map(drawn.bounds.x, drawn.bounds.y);
    CHECK(topLeft[0] == Approx(0.0).margin(1e-9));
    CHECK(topLeft[1] == Approx(880.0));

    // The star: a gradient-filled 5-point star; held opacity keys; linear rotation.
    const GraphicLayer& star = g.layers[1];
    CHECK(star.kind == LayerKind::Group);
    CHECK(star.transform.x == ParamValue(1700.0));
    const Curve* opacity = star.transform.opacity.curve();
    REQUIRE(opacity);
    REQUIRE(opacity->keys.size() == 3);
    CHECK(opacity->keys[0].interpolation == Interpolation::Hold);
    CHECK(opacity->keys[1].time == frame(45));
    CHECK(opacity->keys[1].value == 50.0);
    CHECK(star.transform.opacity.valueAt(frame(50)) == 50.0);
    const Curve* rotation = star.transform.rotation.curve();
    REQUIRE(rotation);
    CHECK(rotation->keys[0].interpolation == Interpolation::Linear);
    CHECK(star.transform.rotation.valueAt(frame(45)) == Approx(180.0));
    REQUIRE(star.children.size() == 1);
    const ShapeContent& starShape = star.children[0].shape;
    CHECK(starShape.kind == ShapeKind::Star);
    CHECK(starShape.points == 5);
    CHECK(starShape.outerRadius == ParamValue(50.0));
    CHECK(starShape.innerRadius == ParamValue(20.0));
    CHECK(starShape.fill.kind == PaintKind::Linear);
    REQUIRE(starShape.fill.stops.size() == 2);
    CHECK(starShape.fill.stops[0].color.rgba == std::array<double, 4>{1.0, 0.0, 0.0, 1.0});
    CHECK(starShape.fill.stops[1].color.rgba == std::array<double, 4>{0.0, 0.0, 1.0, 0.5}); // from the opacity stops
    CHECK(starShape.fill.start == std::array<double, 2>{-50.0, 0.0});

    // The bar: inside a group with the null's transform but not its 50 % opacity (parenting does not pass it on).
    const GraphicLayer& barParent = g.layers[2];
    CHECK(barParent.kind == LayerKind::Group);
    CHECK(barParent.name == "Parent: Controller");
    CHECK(barParent.transform.x == ParamValue(200.0));
    CHECK(barParent.transform.y == ParamValue(850.0));
    CHECK(barParent.transform.opacity == ParamValue(100.0));
    REQUIRE(barParent.children.size() == 1);
    const GraphicLayer& bar = barParent.children[0];
    CHECK(bar.name == "Bar");
    // Ease-in-out position: Bezier keys with flat handles a third of the way in; the middle is the halfway point.
    const Curve* x = bar.transform.x.curve();
    REQUIRE(x);
    REQUIRE(x->keys.size() == 2);
    CHECK(x->keys[0].interpolation == Interpolation::Bezier);
    CHECK(x->keys[0].out.influence == Approx(0.333));
    CHECK(x->keys[0].out.slope == Approx(0.0));
    CHECK(x->keys[1].in.influence == Approx(0.333));
    CHECK(bar.transform.x.valueAt(frame(7.5)) == Approx(-250.0));
    CHECK(bar.transform.x.valueAt(frame(2)) < -550.0); // slow out of the first key
    CHECK(bar.transform.y == ParamValue(900.0));
    REQUIRE(bar.children.size() == 1);
    const GraphicLayer& box = bar.children[0];
    CHECK(box.name == "Box");
    REQUIRE(box.children.size() == 1);
    const GraphicLayer& rect = box.children[0];
    CHECK(rect.shape.kind == ShapeKind::Rectangle);
    CHECK(rect.shape.width == ParamValue(600.0));
    CHECK(rect.shape.cornerRadius == ParamValue(12.0));
    CHECK(rect.transform.x == ParamValue(300.0));
    CHECK(rect.shape.fill.color.rgba[0] == Approx(0.9));
    CHECK(rect.shape.fill.color.rgba[3] == Approx(0.8)); // the fill's opacity
    CHECK(rect.shape.stroke.enabled);
    CHECK(rect.shape.stroke.width == ParamValue(4.0));
    CHECK(rect.shape.stroke.cap == LineCap::Round);
    CHECK(rect.shape.stroke.join == LineJoin::Round);
    CHECK(rect.shape.stroke.dashes == std::vector<double>{10.0, 5.0});
    CHECK(rect.shape.trimEnd.valueAt(frame(15)) == Approx(50.0));

    // The name: point text hanging from its ascent below the baseline origin; the typewriter selector keyed.
    const GraphicLayer& name = g.layers[3].children.at(0);
    CHECK(name.kind == LayerKind::Text);
    CHECK(name.text.text() == "Somchai\nProducer");
    const TextStyle& style = name.text.runs.at(0).style;
    CHECK(style.fontFamily == "Roboto");
    CHECK(style.fontStyle == "Bold");
    CHECK(style.fontWeight == 700);
    CHECK(style.fontSize == 48.0);
    CHECK(style.tracking == 20.0);
    CHECK(style.leading == Approx(57.6));
    CHECK(name.transform.anchorY == ParamValue(0.72 * 48.0));
    REQUIRE(name.text.animators.size() == 1);
    const TextAnimator& typewriter = name.text.animators[0];
    CHECK(typewriter.name == "Typewriter");
    CHECK(typewriter.unit == TextUnit::Character);
    CHECK(typewriter.opacity == 0.0);
    CHECK(typewriter.start.valueAt(frame(0)) == 0.0);
    CHECK(typewriter.start.valueAt(frame(20)) == 100.0);

    // The report: each layer carried, except the point text's placement.
    CHECK(hasEntry(r.report, O, G, "solid layer 'Backdrop'"));
    CHECK(hasEntry(r.report, O, G, "shape layer 'Star'"));
    CHECK(hasEntry(r.report, O, G, "shape layer 'Bar'"));
    CHECK(hasEntry(r.report, A, G, "point text is placed by its font's ascent"));
    CHECK(r.report.count(D) == 0);
    CHECK(entry(r.report, O, G, "solid layer 'Backdrop'").location.element == "/layers/4");
}

TEST_CASE("Lottie: precomps old keyframes and what Wabcut approximates or drops", "[interchange][lottie]") {
    const LottieImport r = imported("lottie/unsupported.json");
    INFO(listed(r.report));
    const Graphic& g = r.graphic;
    REQUIRE(g.layers.size() == 4); // the loop, the fancy shape, the matted shape, the precomp

    // The precomp: a group from frame 10 to 70 clipped to its 400 x 300, its layer's keys shifted by st = 10.
    const GraphicLayer& precomp = g.layers[3];
    CHECK(precomp.name == "Precomp");
    CHECK(precomp.kind == LayerKind::Group);
    CHECK(precomp.in == frame(10));
    CHECK(precomp.out == frame(70));
    REQUIRE_FALSE(precomp.masks.empty());
    const Mask& clip = precomp.masks.back();
    CHECK(clip.mode == MaskMode::Intersect);
    CHECK(maskValue(clip, "size.x", Ticks{}) == Approx(0.5));
    CHECK(maskValue(clip, "center.y", Ticks{}) == Approx(0.25));
    REQUIRE(precomp.children.size() == 1);
    const GraphicLayer& dot = precomp.children[0];
    CHECK(dot.in == frame(10));
    CHECK_FALSE(dot.out);
    const Curve* fade = dot.transform.opacity.curve();
    REQUIRE(fade); // written the pre-5.5 way, with s and e
    REQUIRE(fade->keys.size() == 2);
    CHECK(fade->keys[0].time == frame(10));
    CHECK(fade->keys[1].time == frame(20));
    CHECK(fade->keys[1].value == 100.0);
    CHECK(fade->keys[0].interpolation == Interpolation::Linear);
    CHECK(dot.children.at(0).shape.kind == ShapeKind::Ellipse);

    // The fancy layer: its drop shadow carried over, the rest reported.
    const GraphicLayer& fancy = g.layers[1];
    CHECK(fancy.shadow.enabled);
    CHECK(fancy.shadow.angle == Approx(45.0));
    CHECK(fancy.shadow.distance == Approx(10.0));
    CHECK(fancy.shadow.softness == Approx(6.0));
    CHECK(fancy.shadow.opacity == Approx(0.5));
    REQUIRE(fancy.children.size() == 1);
    const ShapeContent& path = fancy.children[0].shape;
    CHECK(path.kind == ShapeKind::Path);
    REQUIRE(path.path.size() == 2);
    CHECK(path.path[1].vertices[1].x == 120.0);
    CHECK(path.path[0].interpolation == Interpolation::Bezier);
    CHECK(path.fill.color.rgba == std::array<double, 4>{1.0, 0.0, 0.0, 1.0}); // the nearest fill
    CHECK(fancy.transform.rotation == ParamValue(45.0)); // the expression's own value

    // The self-containing precomp is an empty group.
    CHECK(g.layers[0].name == "Loop");
    CHECK(g.layers[0].children.empty());

    const ImportReport& rep = r.report;
    CHECK(hasEntry(rep, D, ReportCategory::Media, "layer 'Photo' is an image"));
    CHECK(hasEntry(rep, D, G, "layer 'Matte' is a track matte"));
    CHECK(hasEntry(rep, A, G, "layer 'Matted' is drawn without its track matte"));
    CHECK(hasEntry(rep, D, G, "a repeater is not imported"));
    CHECK(hasEntry(rep, D, G, "merge paths is not imported"));
    CHECK(hasEntry(rep, A, G, "layer 'Fancy' is a 3D layer"));
    CHECK(hasEntry(rep, A, G, "blend mode is imported as normal"));
    CHECK(hasEntry(rep, A, G, "an expression is ignored"));
    CHECK(hasEntry(rep, A, G, "a curved motion path"));
    CHECK(hasEntry(rep, A, G, "skew is not imported"));
    CHECK(hasEntry(rep, A, G, "several fills or strokes"));
    CHECK(hasEntry(rep, A, G, "path keyframe easing"));
    CHECK(hasEntry(rep, A, ReportCategory::Effect, "effect 'Drop Shadow' becomes the layer's drop shadow"));
    CHECK(hasEntry(rep, D, ReportCategory::Effect, "effect 'Gaussian Blur' is not imported"));
    CHECK(hasEntry(rep, D, G, "layer 'Again' contains itself"));
    CHECK(hasEntry(rep, D, G, "layer 'Camera' has a layer type"));
    CHECK(hasEntry(rep, D, G, "layer 'Late' is never visible"));
    CHECK(hasEntry(rep, D, ReportCategory::Marker, "1 composition marker(s)"));
    CHECK(entry(rep, D, G, "a repeater").location.element == "/layers/2/shapes/0");
    CHECK(entry(rep, D, G, "contains itself").location.element == "/assets/2/layers/0/refId");
}

TEST_CASE("Lottie: frame rates in point and stretched time map to graphic time", "[interchange][lottie]") {
    // 29.97 fps from frame 30: graphic time 0 is the in point; a layer stretched 2x plays its precomp at half speed.
    const std::string text = R"({"v": "5.7.0", "fr": 29.97, "ip": 30, "op": 90, "w": 100, "h": 100,
        "assets": [{"id": "inner", "layers": [{"ty": 4, "nm": "Spin", "ind": 1, "ip": 0, "op": 100, "st": 0,
            "ks": {"r": {"a": 1, "k": [{"t": 0, "s": [0]}, {"t": 10, "s": [90]}]}},
            "shapes": [{"ty": "rc", "s": {"a": 0, "k": [10, 10]}}, {"ty": "fl", "c": {"a": 0, "k": [1, 1, 1]}}]}]}],
        "layers": [{"ty": 0, "nm": "Slow", "refId": "inner", "ind": 1, "ip": 30, "op": 90, "st": 30, "sr": 2, "w": 100, "h": 100,
                    "ks": {}}]})";
    const auto r = importLottie(text);
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    CHECK(r->frameRate == Rate{30000, 1001});
    CHECK(r->duration == Ticks{std::llround(60.0 / 29.97 * static_cast<double>(kTicksPerSecond))});
    const GraphicLayer& spin = r->graphic.layers.at(0).children.at(0);
    const Curve* c = spin.transform.rotation.curve();
    REQUIRE(c);
    // Precomp frame 10 shows at layer time 20, composition frame 50: 20 frames after the in point.
    CHECK(c->keys[0].time == Ticks{});
    CHECK(c->keys[1].time == Ticks{std::llround(20.0 / 29.97 * static_cast<double>(kTicksPerSecond))});
}

TEST_CASE("Lottie: hostile and malformed files are refused or bounded", "[interchange][lottie]") {
    CHECK(importError("not json") != "");
    CHECK(importError("[]").find("not a Lottie") != std::string::npos);
    CHECK(importError(R"({"layers": []})").find("not a Lottie") != std::string::npos);
    CHECK(importError(R"({"fr": 0, "ip": 0, "op": 10, "w": 10, "h": 10, "layers": []})").find("frame rate") != std::string::npos);
    CHECK(importError(R"({"fr": 30, "ip": 10, "op": 10, "w": 10, "h": 10, "layers": []})").find("in point") != std::string::npos);
    CHECK(importError(R"({"fr": 30, "ip": 0, "op": 1e300, "w": 10, "h": 10, "layers": []})") != "");
    CHECK(importError(R"({"fr": 30, "ip": 0, "op": 10, "w": 0, "h": 10, "layers": []})").find("canvas") != std::string::npos);
    CHECK(importError(R"({"fr": 30, "ip": 0, "op": 10, "w": 100000, "h": 10, "layers": []})").find("canvas") != std::string::npos);
    CHECK(importError(std::string(100'000, '[') + std::string(100'000, ']')).find("deep") != std::string::npos);
    CHECK(importError(std::string(kMaxLottieBytes + 1, ' ')).find("16 MiB") != std::string::npos);

    // More layers than a graphic holds: the first 256 come in, the report says the rest did not.
    std::string many = "[";
    for (int i = 0; i < 300; ++i) {
        many += std::string(i ? "," : "") + R"({"ty": 1, "sc": "#ffffff", "sw": 10, "sh": 10, "ip": 0, "op": 60, "ks": {}})";
    }
    many += "]";
    const auto full = importLottie(animation(many));
    REQUIRE(full);
    CHECK(full->graphic.layerCount() == kMaxGraphicLayers);
    CHECK(hasEntry(full->report, D, G, "the graphic holds at most 256 layers"));

    // A path with too many vertices, keys out of order, NaN-ish values, a bad time stretch, a self parent.
    std::string vertices;
    for (int i = 0; i < 2000; ++i) {
        vertices += std::string(i ? "," : "") + "[" + std::to_string(i) + ", 0]";
    }
    const std::string odd = animation(R"([
        {"ty": 4, "nm": "Huge", "ip": 0, "op": 60, "ks": {}, "shapes": [
            {"ty": "sh", "ks": {"a": 0, "k": {"c": false, "v": [)" + vertices + R"(]}}},
            {"ty": "st", "c": {"a": 0, "k": [1, 0, 0, 1]}, "w": {"a": 0, "k": 2}}]},
        {"ty": 4, "nm": "Shuffled", "ind": 2, "parent": 2, "ip": 0, "op": 60, "ks": {
            "o": {"a": 1, "k": [{"t": 10, "s": [10]}, {"t": 5, "s": [20]}, {"t": 20, "s": [500]}, {"t": "x", "s": [1]}]},
            "s": {"a": 0, "k": ["big", null]}}, "shapes": []},
        {"ty": 1, "nm": "Stretched", "sr": -1, "ip": 0, "op": 60, "ks": {}},
        {"ty": 1, "nm": "Odd", "sc": "not a colour", "sw": 1e300, "sh": -5, "bm": 1e300, "ip": 0, "op": 60, "ks": {"p": {"a": 0, "k": [1e300, 5]}}},
        {"ty": 5, "nm": "Empty text", "ip": 0, "op": 60, "ks": {}, "t": {"d": {"k": []}}},
        "not a layer"
    ])");
    const auto r = importLottie(odd);
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    CHECK_FALSE(graphicProblem(r->graphic));
    CHECK(hasEntry(r->report, D, G, "a path without enough vertices"));
    CHECK(hasEntry(r->report, A, G, "keyframes out of time order"));
    CHECK(hasEntry(r->report, A, G, "values outside 0..100 are clamped"));
    CHECK(hasEntry(r->report, A, G, "missing or circular parent"));
    CHECK(hasEntry(r->report, D, G, "bad time stretch"));
    CHECK(hasEntry(r->report, D, G, "a text layer without text"));
    CHECK(hasEntry(r->report, D, G, "a layer that is not an object"));
}
