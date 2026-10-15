// Graphic clips (C.13) in the playback evaluator (I.9 Part 0.5): a graphic clip becomes a layer drawn from its resolved
// draw list (nothing decoded), with the clip's Motion; an unchanged frame reuses its raster; an animated layer and a
// new brand kit give new pictures; the layer is display-referred (graphics).

#include "TimelineFixture.h"

#include <wabcut/model/GraphicOps.h>
#include <wabcut/playback/video/Evaluate.h>
#include <wabcut/render/GraphicRaster.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace wabcut;
using namespace wabcut::test;
using wabcut::playback::LayerSpec;

namespace {

Graphic boxGraphic() {
    Graphic g;
    g.width = 1920;
    g.height = 1080;
    GraphicLayer box;
    box.id = 1;
    box.kind = LayerKind::Shape;
    box.shape.kind = ShapeKind::Rectangle;
    box.shape.width = ParamValue(600.0);
    box.shape.height = ParamValue(200.0);
    box.shape.fill.kind = PaintKind::Solid;
    box.shape.fill.color.rgba = {0.9, 0.4, 0.1, 1.0};
    box.transform.x = ParamValue(960.0);
    box.transform.y = ParamValue(900.0);
    g.layers = {box};
    return g;
}

std::vector<LayerSpec> layersAt(const ProjectState& state, SequenceId seq, Ticks t) {
    std::vector<LayerSpec> specs;
    playback::evaluateVideoLayers(state, *state.findSequence(seq), t, specs, true, nullptr, nullptr);
    return specs;
}

} // namespace

TEST_CASE("Graphic clips are evaluated to draw lists and rasterised; a still graphic reuses its raster", "[playback][graphics]") {
    Timeline tl(R"(
        V2 tgt |..........|
        V1 tgt |Aaaaaaaaaa|
    )");
    auto added = edit::addGraphic(tl.state, tl.seq, tl.track("V2"), f(0), f(10), boxGraphic());
    REQUIRE(added);
    const ProjectState state = added->state;
    const ClipId clip = added->placed.front();

    const std::vector<LayerSpec> specs = layersAt(state, tl.seq, f(3));
    REQUIRE(specs.size() == 2);
    const LayerSpec& top = specs.back();
    CHECK(top.clip == clip);
    REQUIRE(top.graphic);
    CHECK_FALSE(playback::needsDecode(top));
    CHECK(top.graphic->items.size() == 1);
    CHECK(top.graphicSize.width == tl.sequence().width);
    CHECK(top.graphicSize.height == tl.sequence().height);

    // Composite layers: the raster as a view; again at the same picture, the same raster.
    const auto before = render::GraphicRasterCache::shared().rasterizations();
    std::vector<media::VideoFrame> frames(specs.size());
    std::vector<render::CompositeLayer> layers;
    playback::buildCompositeLayers(specs, frames, layers);
    REQUIRE(layers.size() == 1); // V1's media frame was not decoded here: only the graphic draws
    REQUIRE(layers.back().view.has_value());
    const void* identity = layers.back().view->identity;
    std::vector<LayerSpec> again = layersAt(state, tl.seq, f(5));
    std::vector<media::VideoFrame> frames2(again.size());
    std::vector<render::CompositeLayer> layers2;
    playback::buildCompositeLayers(again, frames2, layers2);
    REQUIRE(layers2.size() == 1);
    CHECK(layers2.back().view->identity == identity); // a still graphic: rasterised once
    CHECK(render::GraphicRasterCache::shared().rasterizations() == before + 1);

    // An animated layer (its opacity keyed) draws a new picture at another time.
    Graphic animated = boxGraphic();
    Curve fade;
    fade.keys = {Keyframe{Ticks{}, 0.0}, Keyframe{f(10), 100.0}};
    animated.layers[0].transform.opacity = ParamValue(fade);
    auto set = edit::setGraphic(state, tl.seq, clip, animated);
    REQUIRE(set);
    const auto a = layersAt(set->state, tl.seq, f(2));
    const auto b = layersAt(set->state, tl.seq, f(6));
    REQUIRE(a.back().graphic);
    REQUIRE(b.back().graphic);
    CHECK(*a.back().graphic != *b.back().graphic);
    CHECK(a.back().graphic->items[0].opacity < b.back().graphic->items[0].opacity);
}
