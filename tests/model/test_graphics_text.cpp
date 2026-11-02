// 0.6, text in Effect Controls (roadmap #75): a text layer's keyframable numbers as layer parameters
// (GraphicOps.h layerParam, edit::ParamRef::ofLayer), Source Text hold keys, keyed size and spacing resolved per frame, and
// the new text properties (vertical alignment, strokes, kerning, capitals, faux styles, shadow size) saved and read back
// while old graphics read and write unchanged.

#include "TimelineFixture.h"

#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/GraphicJson.h>
#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/GraphicResolve.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace wabcut;
using namespace wabcut::test;
using Catch::Approx;

namespace {

constexpr std::string_view kPicture = R"(
    V2     |..........|
    V1 tgt |Aaaaaaaaaa|
)";

Ticks seconds(double s) { return Ticks{static_cast<std::int64_t>(s * static_cast<double>(kTicksPerSecond))}; }

/// A text layer: "Hello" at 40 px and " world" at 20 px.
GraphicLayer twoRuns(std::uint32_t id = 2) {
    GraphicLayer l;
    l.id = id;
    l.name = "Name";
    l.kind = LayerKind::Text;
    TextStyle big;
    big.fontSize = 40.0;
    big.tracking = 10.0;
    TextStyle small = big;
    small.fontSize = 20.0;
    small.tracking = 30.0;
    l.text.runs = {TextRun{"Hello", big}, TextRun{" world", small}};
    return l;
}

Curve linear(Ticks a, double va, Ticks b, double vb) {
    Curve c;
    c.keys = {Keyframe{a, va}, Keyframe{b, vb}};
    return c;
}

std::string joined(const std::vector<TextRun>& runs) {
    std::string out;
    for (const TextRun& r : runs) {
        out += r.text;
    }
    return out;
}

const Clip& clipOf(const ProjectState& s, SequenceId seq, ClipId id) {
    const auto loc = s.findSequence(seq)->findClip(id);
    REQUIRE(loc);
    return loc->clip();
}

} // namespace

TEST_CASE("0.6: layer parameters read and write the transform and the text as a whole", "[model][graphics][text]") {
    GraphicLayer l = twoRuns();
    l.transform.scaleX = ParamValue(150.0);
    l.transform.scaleY = ParamValue(150.0);
    l.text.strokes = {TextStroke{}};

    REQUIRE(edit::layerParam(l, "text.fontSize"));
    CHECK(edit::layerParam(l, "text.fontSize")->constant() == 40.0); // the first run's
    CHECK(edit::layerParam(l, "transform.scale")->constant() == 150.0);
    CHECK(edit::layerParam(l, "text.stroke0.width")->constant() == 4.0);
    CHECK_FALSE(edit::layerParam(l, "text.stroke1.width")); // no second stroke
    CHECK_FALSE(edit::layerParam(l, "text.nothing"));
    CHECK_FALSE(edit::layerParamInfo(l, "shape.width"));

    // A constant size scales every run in proportion; tracking moves every run by the same amount.
    REQUIRE(edit::setLayerParam(l, "text.fontSize", ParamValue(80.0)));
    CHECK(l.text.runs[0].style.fontSize == 80.0);
    CHECK(l.text.runs[1].style.fontSize == 40.0);
    REQUIRE(edit::setLayerParam(l, "text.tracking", ParamValue(50.0)));
    CHECK(l.text.runs[0].style.tracking == 50.0);
    CHECK(l.text.runs[1].style.tracking == 70.0);
    // Uniform scale writes both axes; a stroke's width.
    REQUIRE(edit::setLayerParam(l, "transform.scale", ParamValue(80.0)));
    CHECK(l.transform.scaleX.constant() == 80.0);
    CHECK(l.transform.scaleY.constant() == 80.0);
    REQUIRE(edit::setLayerParam(l, "text.stroke0.width", ParamValue(9.0)));
    CHECK(l.text.strokes[0].width.constant() == 9.0);

    // A shape layer has transform parameters but no text ones.
    GraphicLayer shape;
    shape.id = 1;
    shape.kind = LayerKind::Shape;
    CHECK(edit::layerParamInfo(shape, "transform.x"));
    CHECK_FALSE(edit::layerParamInfo(shape, "text.fontSize"));
}

TEST_CASE("0.6: keyed size and tracking interpolate in the resolved text", "[model][graphics][text]") {
    GraphicLayer l = twoRuns();
    REQUIRE(edit::setLayerParam(l, "text.fontSize", ParamValue(linear(Ticks{}, 40.0, seconds(1), 80.0))));
    REQUIRE(edit::setLayerParam(l, "text.tracking", ParamValue(linear(Ticks{}, 0.0, seconds(1), 100.0))));
    REQUIRE(l.text.sizeKeys);
    CHECK(edit::layerParam(l, "text.fontSize")->isCurve());

    const TextContent half = textAt(l.text, seconds(0.5));
    REQUIRE(half.runs.size() == 2);
    CHECK(half.runs[0].style.fontSize == Approx(60.0));
    CHECK(half.runs[1].style.fontSize == Approx(30.0)); // in proportion to the first run
    CHECK(half.runs[0].style.tracking == Approx(50.0));
    CHECK(half.runs[1].style.tracking == Approx(70.0)); // by the same amount
    CHECK_FALSE(half.sizeKeys);

    // The draw list carries the values of its time.
    Graphic g;
    g.layers = {l};
    REQUIRE_FALSE(graphicProblem(g));
    const DrawList start = resolveGraphic(g, Ticks{});
    const DrawList end = resolveGraphic(g, seconds(1));
    REQUIRE(start.items.size() == 1);
    CHECK(start.items[0].text.runs[0].fontSize == Approx(40.0));
    CHECK(end.items[0].text.runs[0].fontSize == Approx(80.0));
    CHECK(end.items[0].text.runs[1].tracking == Approx(120.0));

    // The stopwatch off: a constant again (the runs take it), the keys gone.
    REQUIRE(edit::setLayerParam(l, "text.fontSize", ParamValue(64.0)));
    CHECK_FALSE(l.text.sizeKeys);
    CHECK(l.text.runs[0].style.fontSize == 64.0);
}

TEST_CASE("0.6: Source Text keys hold their text and editing between keys adds one", "[model][graphics][text]") {
    GraphicLayer l = twoRuns();
    // The stopwatch on at 0: one hold key showing the runs.
    Curve on;
    on.keys = {Keyframe{Ticks{}, 0.0}};
    REQUIRE(edit::setLayerParam(l, "text.source", ParamValue(on)));
    REQUIRE(l.text.sourceKey.isCurve());

    // An edit at 1 s (no key there) adds a key holding the new text; before it the old text shows.
    const auto setText = [](std::string text) {
        return [text = std::move(text)](std::vector<TextRun>& runs) {
            const TextStyle style = runs.front().style;
            runs = {TextRun{text, style}};
            return true;
        };
    };
    REQUIRE(edit::editSourceRunsAt(l.text, seconds(1), setText("World")));
    const Curve* keys = l.text.sourceKey.curve();
    REQUIRE(keys);
    REQUIRE(keys->keys.size() == 2);
    CHECK(keys->keys[0].interpolation == Interpolation::Hold);
    CHECK(keys->keys[1].interpolation == Interpolation::Hold);
    REQUIRE(l.text.sources.size() == 1);
    CHECK(joined(sourceRunsAt(l.text, seconds(0.5))) == "Hello world");
    CHECK(joined(sourceRunsAt(l.text, seconds(0.99))) == "Hello world");
    CHECK(joined(sourceRunsAt(l.text, seconds(1))) == "World");
    CHECK(joined(sourceRunsAt(l.text, seconds(5))) == "World");
    CHECK(textAt(l.text, seconds(2)).text() == "World");

    // An edit at the key's time changes that key's version only.
    REQUIRE(edit::editSourceRunsAt(l.text, seconds(1), setText("Earth")));
    CHECK(joined(sourceRunsAt(l.text, seconds(1))) == "Earth");
    CHECK(joined(sourceRunsAt(l.text, Ticks{})) == "Hello world");

    // A copied key (same version) gets a version of its own when edited.
    Curve copied = *l.text.sourceKey.curve();
    copied.keys.push_back(Keyframe{seconds(2), copied.keys[1].value, Interpolation::Hold});
    REQUIRE(edit::setLayerParam(l, "text.source", ParamValue(copied)));
    REQUIRE(edit::editSourceRunsAt(l.text, seconds(2), setText("Moon")));
    CHECK(joined(sourceRunsAt(l.text, seconds(1.5))) == "Earth");
    CHECK(joined(sourceRunsAt(l.text, seconds(2))) == "Moon");
    CHECK(l.text.sources.size() == 2);

    // Interpolation is kept hold; deleting the second key drops its version.
    Curve linearKeys = *l.text.sourceKey.curve();
    for (Keyframe& k : linearKeys.keys) {
        k.interpolation = Interpolation::Linear;
    }
    linearKeys.keys.erase(linearKeys.keys.begin() + 1);
    REQUIRE(edit::setLayerParam(l, "text.source", ParamValue(linearKeys)));
    CHECK(l.text.sourceKey.curve()->keys[1].interpolation == Interpolation::Hold);
    CHECK(l.text.sources.size() == 1);
    CHECK(joined(sourceRunsAt(l.text, seconds(1.5))) == "Hello world");

    // The stopwatch off at 2 s: the text shown there becomes the layer's runs.
    REQUIRE(edit::setLayerParam(l, "text.source", ParamValue(l.text.sourceKey.valueAt(seconds(2)))));
    CHECK(l.text.sourceKey.isConstant());
    CHECK(l.text.sources.empty());
    CHECK(l.text.text() == "Moon");

    Graphic g;
    g.layers = {l};
    CHECK_FALSE(graphicProblem(g));
}

TEST_CASE("0.6: the new text properties round-trip through JSON and old graphics read unchanged", "[model][graphics][text]") {
    GraphicLayer l = twoRuns();
    l.text.verticalAlign = TextVerticalAlign::Bottom;
    l.text.boxWidth = 600.0;
    l.text.boxHeight = 300.0;
    TextStroke outer;
    outer.color = GraphicColor{{1.0, 0.5, 0.0, 1.0}, {}};
    outer.width = ParamValue(linear(Ticks{}, 2.0, seconds(1), 12.0));
    TextStroke inner;
    inner.position = StrokePosition::Inside;
    inner.enabled = false;
    l.text.strokes = {outer, inner};
    l.text.runs[0].style.kerning = false;
    l.text.runs[0].style.caps = TextCaps::SmallCaps;
    l.text.runs[1].style.fauxBold = true;
    l.text.runs[1].style.fauxItalic = true;
    l.shadow.enabled = true;
    l.shadow.size = 7.5;
    l.text.sizeKeys = linear(Ticks{}, 40.0, seconds(1), 90.0);
    l.text.leadingKeys = linear(Ticks{}, 50.0, seconds(1), 70.0);
    Curve source;
    source.keys = {Keyframe{Ticks{}, 0.0, Interpolation::Hold}, Keyframe{seconds(1), 1.0, Interpolation::Hold}};
    l.text.sourceKey = ParamValue(source);
    l.text.sources = {{TextRun{"Later", l.text.runs[0].style}}};
    Graphic g;
    g.name = "Text";
    g.layers = {l};
    REQUIRE_FALSE(graphicProblem(g));

    const std::string json = io::graphicToJson(g);
    const auto back = io::graphicFromJson(json);
    REQUIRE(back);
    CHECK(*back == g);

    // A text layer with none of them writes none of their keys (old projects keep their bytes).
    Graphic plain;
    plain.layers = {twoRuns()};
    const std::string plainJson = io::graphicToJson(plain);
    for (const char* key : {"verticalAlign", "strokes", "sourceKey", "sources", "sizeKeys", "kerning", "caps", "fauxBold",
                            "fauxItalic", "\"size\": 0"}) {
        INFO(key);
        CHECK(plainJson.find(key) == std::string::npos);
    }
    // ... and a graphic written before 0.6 reads with the defaults.
    const auto old = io::graphicFromJson(
        R"({"name": "Old", "width": 1920, "height": 1080, "layers": [{"id": 1, "kind": "text",
            "text": {"runs": [{"text": "Hi", "size": 50}]}, "shadow": {"enabled": true}}]})");
    REQUIRE(old);
    const GraphicLayer& o = old->layers.at(0);
    CHECK(o.text.verticalAlign == TextVerticalAlign::Top);
    CHECK(o.text.strokes.empty());
    CHECK(o.text.runs[0].style.kerning);
    CHECK(o.text.runs[0].style.caps == TextCaps::Normal);
    CHECK(o.shadow.size == 0.0);

    // Bounds: a Source Text key showing a version that is not there, too many strokes.
    Graphic bad = g;
    bad.layers[0].text.sources.clear();
    CHECK(graphicProblem(bad));
    bad = g;
    bad.layers[0].text.strokes.resize(kMaxTextStrokes + 1);
    CHECK(graphicProblem(bad));
    bad = g;
    bad.layers[0].shadow.size = 5000.0;
    CHECK(graphicProblem(bad));
}

TEST_CASE("0.6: clip parameter edits key a graphic layer's numbers in graphic time", "[model][graphics][text]") {
    Timeline tl{kPicture};
    Graphic g;
    g.name = "Card";
    g.layers = {twoRuns(2)};
    auto added = edit::addGraphic(tl.state, tl.seq, tl.track("V2"), Ticks{}, seconds(2), g);
    REQUIRE(added);
    ProjectState state = added->state;
    const ClipId clip = added->placed.at(0);
    const edit::ParamRef size = edit::ParamRef::ofLayer(2, "text.fontSize");

    // A constant, then the stopwatch, then a second key: the layer's own keys.
    auto r = edit::setParameter(state, tl.seq, clip, size, ParamValue(50.0));
    REQUIRE(r);
    state = r->state;
    CHECK(clipOf(state, tl.seq, clip).graphic()->findLayer(2)->text.runs[0].style.fontSize == 50.0);
    r = edit::setAnimated(state, tl.seq, clip, size, true, Ticks{});
    REQUIRE(r);
    state = r->state;
    r = edit::addKeyframe(state, tl.seq, clip, size, Keyframe{seconds(1), 100.0});
    REQUIRE(r);
    state = r->state;
    const GraphicLayer* layer = clipOf(state, tl.seq, clip).graphic()->findLayer(2);
    REQUIRE(layer->text.sizeKeys);
    CHECK(layer->text.sizeKeys->keys.size() == 2);
    CHECK(textAt(layer->text, seconds(0.5)).runs[0].style.fontSize == Approx(75.0));

    // Saved and reopened: the keys, a stroke and a Source Text key come back as they were.
    r = edit::setParameter(state, tl.seq, clip, edit::ParamRef::ofLayer(2, "text.source"),
                           ParamValue(Curve{{Keyframe{Ticks{}, 0.0, Interpolation::Hold}}}));
    REQUIRE(r);
    state = r->state;
    GraphicLayer styled = *clipOf(state, tl.seq, clip).graphic()->findLayer(2);
    REQUIRE(edit::editSourceRunsAt(styled.text, seconds(1), [](std::vector<TextRun>& runs) {
        runs[0].text = "Later";
        runs[0].style.caps = TextCaps::AllCaps;
        return true;
    }));
    styled.text.strokes = {TextStroke{}};
    styled.shadow.size = 3.0;
    r = edit::updateGraphicLayer(state, tl.seq, clip, 2, styled);
    REQUIRE(r);
    state = r->state;
    const auto saved = io::toJson(state);
    REQUIRE(saved);
    const auto reopened = io::fromJson(*saved);
    REQUIRE(reopened);
    const GraphicLayer* back = clipOf(*reopened, tl.seq, clip).graphic()->findLayer(2);
    REQUIRE(back);
    CHECK(*back == *clipOf(state, tl.seq, clip).graphic()->findLayer(2));
    CHECK(joined(sourceRunsAt(back->text, seconds(1.5))) == "Later world");

    // Out of range, an unknown path and a missing layer are refused.
    CHECK_FALSE(edit::setParameter(state, tl.seq, clip, size, ParamValue(5000.0)));
    CHECK_FALSE(edit::setParameter(state, tl.seq, clip, edit::ParamRef::ofLayer(2, "text.nothing"), ParamValue(1.0)));
    CHECK_FALSE(edit::setParameter(state, tl.seq, clip, edit::ParamRef::ofLayer(9, "transform.x"), ParamValue(1.0)));
    // A video clip that is not a graphic has no layers.
    const ClipId video = tl.clip("V1@0");
    CHECK_FALSE(edit::setParameter(state, tl.seq, video, edit::ParamRef::ofLayer(2, "transform.x"), ParamValue(1.0)));
}

TEST_CASE("Source Text keys pasted with their text show that text on any layer (fix F1A)", "[model][graphics][text]") {
    // ARGON-09: pasting Source Text keys copied only their values, indices into the source layer's versions; on
    // another layer they showed whatever that layer's versions held (or nothing changed).
    const auto runOf = [](std::string text) { return std::vector<TextRun>{TextRun{std::move(text), TextStyle{}}}; };
    GraphicLayer l = twoRuns(); // "Hello world", no Source Text keys
    const std::vector<Keyframe> keys = {Keyframe{seconds(1), 7.0, Interpolation::Linear}, Keyframe{seconds(2), 9.0}};
    const std::vector<std::vector<TextRun>> texts = {runOf("One"), runOf("Two")};
    REQUIRE(edit::pasteSourceTextKeys(l.text, keys, texts));
    const Curve* curve = l.text.sourceKey.curve();
    REQUIRE(curve);
    REQUIRE(curve->keys.size() == 2);
    CHECK(curve->keys[0].interpolation == Interpolation::Hold);
    // Hold keys: before the first key its text shows (the layer had no keys: its old text went, as a pasted key
    // replaces any constant).
    CHECK(joined(sourceRunsAt(l.text, seconds(0.5))) == "One");
    CHECK(joined(sourceRunsAt(l.text, seconds(1.5))) == "One");
    CHECK(joined(sourceRunsAt(l.text, seconds(2))) == "Two");
    CHECK(joined(sourceRunsAt(l.text, seconds(9))) == "Two");

    // Pasted again later onto keys of its own: a key at the same time is replaced, an identical text shared.
    const std::vector<Keyframe> more = {Keyframe{seconds(2), 0.0}, Keyframe{seconds(3), 0.0}};
    const std::vector<std::vector<TextRun>> moreTexts = {runOf("Three"), runOf("One")};
    REQUIRE(edit::pasteSourceTextKeys(l.text, more, moreTexts));
    REQUIRE(l.text.sourceKey.curve()->keys.size() == 3);
    CHECK(joined(sourceRunsAt(l.text, seconds(1))) == "One");
    CHECK(joined(sourceRunsAt(l.text, seconds(2))) == "Three");
    CHECK(joined(sourceRunsAt(l.text, seconds(3))) == "One");
    CHECK(l.text.sources.size() == 1); // "One" is the runs (version 0), "Three" the only other version; "Two" dropped

    // Mismatched sizes change nothing.
    const TextContent before = l.text;
    CHECK_FALSE(edit::pasteSourceTextKeys(l.text, more, std::vector<std::vector<TextRun>>{runOf("x")}));
    CHECK(l.text == before);
}
