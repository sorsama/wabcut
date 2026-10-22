// 0.2 Essential Graphics parity: Roll / Crawl (GraphicRoll, rollOffset), Responsive Design - Position to the video frame
// (LayerPin::kVideoFrame) and Align / Distribute (edit::alignGraphicLayers).

#include "TimelineFixture.h"

#include <wabcut/model/GraphicJson.h>
#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/GraphicResolve.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace wabcut;
using namespace wabcut::test;
using Catch::Matchers::WithinAbs;

namespace {

constexpr Ticks kSecond{kTicksPerSecond};
constexpr Ticks operator*(Ticks t, std::int64_t n) { return Ticks{t.value * n}; }
constexpr Ticks operator/(Ticks t, std::int64_t n) { return Ticks{t.value / n}; }

GraphicLayer rect(std::uint32_t id, double x, double y, double w = 400.0, double h = 200.0) {
    GraphicLayer l;
    l.id = id;
    l.name = "Box " + std::to_string(id);
    l.kind = LayerKind::Shape;
    l.shape.kind = ShapeKind::Rectangle;
    l.shape.width = w;
    l.shape.height = h;
    l.transform.x = x;
    l.transform.y = y;
    return l;
}

/// The canvas box layer `id` draws.
Box boxOf(const DrawList& list, std::uint32_t id) {
    DrawList one;
    for (const DrawItem& item : list.items) {
        if (item.layer == id) {
            one.items.push_back(item);
        }
    }
    return contentBox(one);
}

Box boxOf(const Graphic& g, std::uint32_t id) { return boxOf(resolveGraphic(g, Ticks{}), id); }

} // namespace

TEST_CASE("Roll and crawl travel across the frame over the clip", "[model][graphics][roll]") {
    const Box content{760.0, 440.0, 400.0, 200.0};
    const Ticks d = kSecond * 10;
    GraphicRoll roll; // Roll, Start and End Off Screen
    // From the content's top at the frame's bottom to its bottom at the frame's top, at constant speed.
    CHECK_THAT(rollOffset(roll, content, 1920, 1080, Ticks{}, d)[1], WithinAbs(640.0, 1e-9));
    CHECK_THAT(rollOffset(roll, content, 1920, 1080, kSecond * 5, d)[1], WithinAbs(0.0, 1e-9));
    CHECK_THAT(rollOffset(roll, content, 1920, 1080, d, d)[1], WithinAbs(-640.0, 1e-9));
    CHECK(rollOffset(roll, content, 1920, 1080, d, d)[0] == 0.0);

    // Preroll and postroll hold the ends; the move happens between them.
    roll.preroll = kSecond;
    roll.postroll = kSecond;
    CHECK_THAT(rollOffset(roll, content, 1920, 1080, kSecond / 2, d)[1], WithinAbs(640.0, 1e-9));
    CHECK_THAT(rollOffset(roll, content, 1920, 1080, kSecond * 5, d)[1], WithinAbs(0.0, 1e-9));
    CHECK_THAT(rollOffset(roll, content, 1920, 1080, kSecond * 19 / 2, d)[1], WithinAbs(-640.0, 1e-9));
    // Ease-In and Ease-Out: from rest, a trapezoidal speed (1/24 of the way one second into a 2 s ease over 8 s).
    roll.easeIn = kSecond * 2;
    roll.easeOut = kSecond * 2;
    CHECK_THAT(rollOffset(roll, content, 1920, 1080, kSecond * 2, d)[1], WithinAbs(640.0 - 1280.0 / 24.0, 1e-6));
    CHECK_THAT(rollOffset(roll, content, 1920, 1080, kSecond * 5, d)[1], WithinAbs(0.0, 1e-6));

    // Without Start / End Off Screen, content that fits does not move.
    GraphicRoll still;
    still.startOffScreen = false;
    still.endOffScreen = false;
    CHECK(rollOffset(still, content, 1920, 1080, kSecond * 5, d)[1] == 0.0);
    // A tall credit list scrolls until its bottom reaches the frame's bottom.
    const Box credits{760.0, 100.0, 400.0, 3000.0};
    CHECK_THAT(rollOffset(still, credits, 1920, 1080, d, d)[1], WithinAbs(1080.0 - 3100.0, 1e-9));

    // Crawls: left from beyond the right edge to beyond the left edge, right the other way.
    GraphicRoll left;
    left.kind = RollKind::CrawlLeft;
    CHECK_THAT(rollOffset(left, content, 1920, 1080, Ticks{}, d)[0], WithinAbs(1160.0, 1e-9));
    CHECK_THAT(rollOffset(left, content, 1920, 1080, d, d)[0], WithinAbs(-1160.0, 1e-9));
    GraphicRoll right = left;
    right.kind = RollKind::CrawlRight;
    CHECK_THAT(rollOffset(right, content, 1920, 1080, Ticks{}, d)[0], WithinAbs(-1160.0, 1e-9));
    CHECK_THAT(rollOffset(right, content, 1920, 1080, d, d)[0], WithinAbs(1160.0, 1e-9));
}

TEST_CASE("A rolling graphic clip moves its layers and masks and reads back", "[model][graphics][roll]") {
    Timeline tl(R"(
        V1 tgt |..........|
    )");
    Graphic g;
    g.layers.push_back(rect(1, 960.0, 540.0));
    GraphicRoll roll;
    roll.kind = RollKind::Roll;
    roll.postroll = kSecond;
    g.roll = roll;
    REQUIRE_FALSE(graphicProblem(g));
    // JSON round trip.
    auto back = io::graphicFromJson(io::graphicToJson(g));
    REQUIRE(back);
    CHECK(*back == g);
    CHECK_FALSE(io::graphicFromJson(R"({"roll": {"kind": "spiral"}})"));
    CHECK_FALSE(io::graphicFromJson(R"({"roll": {"preroll": -1}})"));

    const auto added = ok(edit::addGraphic(tl.state, tl.seq, tl.track("V1"), Ticks{}, kSecond * 9, g));
    const Clip& clip = added.state.findSequence(tl.seq)->findClip(added.placed.front())->clip();
    const auto at = [&](Ticks t) { return boxOf(resolveGraphicClip(added.state, clip, t), 1); };
    CHECK_THAT(at(Ticks{}).y, WithinAbs(1080.0, 1e-6));
    CHECK_THAT(at(kSecond * 8).y + at(kSecond * 8).height, WithinAbs(0.0, 1e-6)); // gone by the postroll
    CHECK_THAT(at(kSecond * 4).y, WithinAbs(440.0, 1e-6));                        // half way: where it was designed
    // Without the roll it stays put.
    Graphic still = g;
    still.roll.reset();
    CHECK_THAT(boxOf(still, 1).y, WithinAbs(440.0, 1e-9));
}

TEST_CASE("Layers pinned to the video frame follow the canvas", "[model][graphics][pins]") {
    Graphic g;
    GraphicLayer logo = rect(1, 0.0, 0.0, 100.0, 100.0);
    logo.pin = LayerPin{LayerPin::kVideoFrame, LayerPin::kRight | LayerPin::kBottom, {0.0, 0.0, -50.0, -50.0}};
    g.layers.push_back(logo);
    REQUIRE_FALSE(graphicProblem(g));
    Box b = boxOf(g, 1);
    CHECK_THAT(b.x + b.width, WithinAbs(1870.0, 1e-9));
    CHECK_THAT(b.y + b.height, WithinAbs(1030.0, 1e-9));
    // A vertical canvas: still 50 px in from the right and bottom.
    g.width = 1080;
    g.height = 1920;
    b = boxOf(g, 1);
    CHECK_THAT(b.x + b.width, WithinAbs(1030.0, 1e-9));
    CHECK_THAT(b.y + b.height, WithinAbs(1870.0, 1e-9));
    // JSON says "frame".
    const std::string text = io::graphicToJson(g);
    CHECK(text.find("\"frame\"") != std::string::npos);
    auto back = io::graphicFromJson(text);
    REQUIRE(back);
    CHECK(*back == g);
    // Only top-level layers can be pinned to the frame.
    Graphic nested;
    GraphicLayer group;
    group.id = 5;
    group.kind = LayerKind::Group;
    group.children.push_back(logo);
    nested.layers.push_back(group);
    CHECK(graphicProblem(nested));
}

TEST_CASE("Align and Distribute move layers by their position", "[model][graphics][align]") {
    Graphic g;
    g.layers = {rect(1, 100.0, 200.0, 100.0, 50.0), rect(2, 500.0, 600.0, 200.0, 50.0), rect(3, 1500.0, 900.0, 100.0, 50.0)};
    const std::vector<std::uint32_t> all{1, 2, 3};
    const auto aligned = [&](edit::LayerAlign how, edit::AlignTo to) {
        auto r = edit::alignGraphicLayers(g, all, how, to, Ticks{}, {});
        INFO((r ? std::string() : r.error().message));
        REQUIRE(r);
        return *r;
    };
    Graphic left = aligned(edit::LayerAlign::Left, edit::AlignTo::VideoFrame);
    for (const std::uint32_t id : all) {
        CHECK_THAT(boxOf(left, id).x, WithinAbs(0.0, 1e-9));
    }
    Graphic selection = aligned(edit::LayerAlign::Right, edit::AlignTo::Selection);
    for (const std::uint32_t id : all) {
        CHECK_THAT(boxOf(selection, id).x + boxOf(selection, id).width, WithinAbs(1550.0, 1e-9));
    }
    Graphic middle = aligned(edit::LayerAlign::VerticalCenter, edit::AlignTo::VideoFrame);
    CHECK_THAT(boxOf(middle, 2).y + 25.0, WithinAbs(540.0, 1e-9));
    // Distribute: the middle layer's centre half way between the outer ones.
    Graphic spread = aligned(edit::LayerAlign::DistributeHorizontal, edit::AlignTo::Selection);
    CHECK_THAT(boxOf(spread, 2).x + 100.0, WithinAbs(800.0, 1e-9));
    CHECK_THAT(boxOf(spread, 1).x, WithinAbs(50.0, 1e-9));
    CHECK_FALSE(edit::alignGraphicLayers(g, std::vector<std::uint32_t>{1, 2}, edit::LayerAlign::DistributeVertical,
                                         edit::AlignTo::Selection, Ticks{}, {}));
    CHECK_FALSE(edit::alignGraphicLayers(g, std::vector<std::uint32_t>{9}, edit::LayerAlign::Left, edit::AlignTo::VideoFrame,
                                         Ticks{}, {}));

    // Inside a rotated group the move goes through the group's transform; animated positions keep their keys' shape.
    Graphic turned;
    GraphicLayer group;
    group.id = 10;
    group.kind = LayerKind::Group;
    group.transform.x = 960.0;
    group.transform.y = 540.0;
    group.transform.rotation = 90.0;
    GraphicLayer child = rect(11, 0.0, 300.0, 100.0, 50.0);
    Curve keys;
    keys.keys = {Keyframe{Ticks{}, 300.0, Interpolation::Linear, {}, {}}, Keyframe{kSecond, 400.0, Interpolation::Linear, {}, {}}};
    child.transform.y = ParamValue(keys);
    group.children.push_back(child);
    turned.layers.push_back(group);
    auto moved = edit::alignGraphicLayers(turned, std::vector<std::uint32_t>{11}, edit::LayerAlign::Left, edit::AlignTo::VideoFrame,
                                          Ticks{}, {});
    REQUIRE(moved);
    CHECK_THAT(boxOf(*moved, 11).x, WithinAbs(0.0, 1e-6));
    const Curve* after = moved->findLayer(11)->transform.y.curve();
    REQUIRE(after);
    CHECK_THAT(after->keys[1].value - after->keys[0].value, WithinAbs(100.0, 1e-9));

    // A pinned layer does not move with its position.
    Graphic pinned = g;
    pinned.layers[1].pin = LayerPin{1, LayerPin::kLeft | LayerPin::kTop, {0.0, 0.0, 0.0, 0.0}};
    CHECK_FALSE(edit::alignGraphicLayers(pinned, std::vector<std::uint32_t>{2}, edit::LayerAlign::Left, edit::AlignTo::VideoFrame,
                                         Ticks{}, {}));
}
