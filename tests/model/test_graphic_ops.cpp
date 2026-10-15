// Editing motion graphics (C.13): graphic clips and their layers, animation presets as editable keys, templates with
// fields (save, instantiate, set fields as one edit, responsive time), and brand kit swaps that re-theme instances.

#include "TimelineFixture.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/GraphicResolve.h>
#include <wabcut/model/Merge.h>
#include <wabcut/model/ProjectDiff.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

using namespace wabcut;
using namespace wabcut::test;
using Catch::Approx;

namespace {

constexpr std::string_view kPicture = R"(
    V2     |..........|
    V1 tgt |Aaaaaaaaaa|
)";

constexpr Ticks kHalf = Ticks{kTicksPerSecond / 2};
Ticks seconds(double s) { return Ticks{static_cast<std::int64_t>(s * static_cast<double>(kTicksPerSecond))}; }

GraphicLayer textLayer(std::uint32_t id, std::string text, double x = 0.0, double y = 0.0) {
    GraphicLayer l;
    l.id = id;
    l.kind = LayerKind::Text;
    TextStyle style;
    style.fontSize = 64.0;
    style.fontFamily = "Noto Sans";
    style.fontWeight = 600;
    l.text.runs = {TextRun{std::move(text), style}};
    l.transform.x = ParamValue(x);
    l.transform.y = ParamValue(y);
    return l;
}

GraphicLayer rectLayer(std::uint32_t id, double w, double h, double x = 0.0, double y = 0.0) {
    GraphicLayer l;
    l.id = id;
    l.kind = LayerKind::Shape;
    l.shape.width = ParamValue(w);
    l.shape.height = ParamValue(h);
    l.transform.x = ParamValue(x);
    l.transform.y = ParamValue(y);
    return l;
}

/// A lower third: a box (1) behind a name (2) and a title (3), and a logo (4).
Graphic lowerThird(MediaId logoMedia) {
    Graphic g;
    g.name = "Lower third";
    GraphicLayer box = rectLayer(1, 600, 120, 500, 900);
    box.shape.fill.color = GraphicColor{{0.2, 0.2, 0.2, 1.0}, "Primary"};
    box.pin = LayerPin{2, LayerPin::kLeft | LayerPin::kTop | LayerPin::kRight | LayerPin::kBottom, {-20, -10, 20, 10}};
    GraphicLayer name = textLayer(2, "Name", 220, 860);
    name.text.runs[0].style.brandFont = "heading";
    GraphicLayer title = textLayer(3, "Title", 220, 940);
    title.text.runs[0].style.fill = GraphicColor{{0.8, 0.8, 0.8, 1.0}, "Accent"};
    GraphicLayer logo;
    logo.id = 4;
    logo.kind = LayerKind::Image;
    logo.image.media = logoMedia;
    logo.image.brandLogo = "Main";
    g.layers = {box, name, title, logo};
    return g;
}

std::vector<TemplateField> lowerThirdFields() {
    return {TemplateField{"name", "Name", FieldKind::Text, 2, "text", {}},
            TemplateField{"title", "Title", FieldKind::Text, 3, "text", {}},
            TemplateField{"boxColor", "Box colour", FieldKind::Color, 1, "fill", {}},
            TemplateField{"font", "Font", FieldKind::Font, 2, "font", {}},
            TemplateField{"logo", "Logo", FieldKind::Image, 4, "media", {}},
            TemplateField{"showLogo", "Show logo", FieldKind::Checkbox, 4, "visible", {}},
            TemplateField{"titleOut", "Title ends", FieldKind::Duration, 3, "out", {}},
            TemplateField{"length", "Length", FieldKind::Duration, 0, "clip", {}}};
}

BrandKit kit(std::array<double, 4> primary, std::string heading, MediaId logo) {
    BrandKit k;
    k.name = "Kit";
    k.colors = {BrandColor{"Primary", primary}, BrandColor{"Accent", {1.0, 0.8, 0.0, 1.0}}};
    k.fonts = {BrandFont{"heading", std::move(heading), "Bold", 700}};
    k.logos = {BrandLogo{"Main", logo}};
    return k;
}

const Clip& clipOf(const ProjectState& s, SequenceId seq, ClipId id) { return s.findSequence(seq)->findClip(id)->clip(); }

const Graphic& graphicOf(const ProjectState& s, SequenceId seq, ClipId id) {
    const Graphic* g = clipOf(s, seq, id).graphic();
    REQUIRE(g);
    return *g;
}

std::vector<std::pair<std::int64_t, double>> keysOf(const ParamValue& v) {
    std::vector<std::pair<std::int64_t, double>> out;
    if (const Curve* c = v.curve()) {
        for (const Keyframe& k : c->keys) {
            out.emplace_back(k.time.value, k.value);
        }
    }
    return out;
}

std::vector<Interpolation> interpolationsOf(const ParamValue& v) {
    std::vector<Interpolation> out;
    if (const Curve* c = v.curve()) {
        for (const Keyframe& k : c->keys) {
            out.push_back(k.interpolation);
        }
    }
    return out;
}

/// A 2 s graphic clip on V2 holding `g`; the timeline and the clip.
struct GraphicClip {
    Timeline tl{kPicture};
    ProjectState state;
    ClipId clip;

    explicit GraphicClip(Graphic g, Ticks length = Ticks::fromSeconds(2)) {
        const auto r = ok(edit::addGraphic(tl.state, tl.seq, tl.track("V2"), Ticks{}, length, std::move(g)));
        state = r.state;
        clip = r.placed.at(0);
    }
    const Graphic& graphic() const { return graphicOf(state, tl.seq, clip); }
    const GraphicLayer& layer(std::uint32_t id) const {
        const GraphicLayer* l = graphic().findLayer(id);
        REQUIRE(l);
        return *l;
    }
    /// Applies a preset to layer `id` and keeps the result.
    void preset(std::uint32_t id, edit::PresetOptions o) { state = ok(edit::applyGraphicPreset(state, tl.seq, clip, id, o)).state; }
};

edit::PresetOptions options(edit::PresetKind kind, edit::PresetPhase phase,
                            edit::PresetEasing easing = edit::PresetEasing::EaseOut) {
    edit::PresetOptions o;
    o.kind = kind;
    o.phase = phase;
    o.easing = easing;
    return o;
}

} // namespace

// ---- Graphic clips and layers -----------------------------------------------------------------------------------------

TEST_CASE("addGraphic places a graphic clip on one shared canvas item", "[model][graphics][ops]") {
    Timeline tl(kPicture);
    Graphic g;
    g.layers = {rectLayer(0, 100, 100), textLayer(0, "Hi"), rectLayer(7, 10, 10)};
    const auto r = ok(edit::addGraphic(tl.state, tl.seq, tl.track("V2"), f(2), f(5), g));
    REQUIRE(r.placed.size() == 1);
    REQUIRE(r.createdMedia.isValid());
    const MediaItem* canvas = r.state.findMedia(r.createdMedia);
    REQUIRE(canvas);
    CHECK(canvas->isGraphic());
    CHECK(canvas->info.video.at(0).width == tl.sequence().width);
    CHECK(canvas->info.isStillImage);
    const Clip& clip = clipOf(r.state, tl.seq, r.placed[0]);
    CHECK(clip.start() == f(2));
    CHECK(clip.duration() == f(5));
    CHECK(clip.name() == "Graphic");
    // Layer ids 0 get fresh ones after the largest; the others stay.
    const Graphic& placed = graphicOf(r.state, tl.seq, r.placed[0]);
    CHECK(placed.layers[0].id == 8);
    CHECK(placed.layers[1].id == 9);
    CHECK(placed.layers[2].id == 7);
    REQUIRE(r.changes.addedMedia == std::vector<MediaId>{r.createdMedia});

    // A second graphic reuses the canvas.
    Graphic named;
    named.name = "Bug";
    const auto second = ok(edit::addGraphic(r.state, tl.seq, tl.track("V2"), f(8), f(2), named));
    CHECK_FALSE(second.createdMedia.isValid());
    CHECK(clipOf(second.state, tl.seq, second.placed[0]).media == r.createdMedia);
    CHECK(clipOf(second.state, tl.seq, second.placed[0]).name() == "Bug");

    // Refusals.
    CHECK_FALSE(edit::addGraphic(tl.state, tl.seq, TrackId{9999}, f(0), f(5), g));
    CHECK_FALSE(edit::addGraphic(tl.state, tl.seq, tl.track("V2"), f(0), Ticks{}, g));
    CHECK_FALSE(edit::addGraphic(tl.state, tl.seq, tl.track("V2"), Ticks{-1}, f(5), g));
    Graphic bad = g;
    bad.layers[0].transform.opacity = ParamValue(500.0);
    CHECK_FALSE(edit::addGraphic(tl.state, tl.seq, tl.track("V2"), f(0), f(5), bad));
}

TEST_CASE("Graphics go on unlocked video tracks only", "[model][graphics][ops]") {
    Timeline tl(R"(
        V2 lock |..........|
        V1 tgt  |Aaaaaaaaaa|
        A1      |Aaaaaaaaaa|
    )");
    CHECK_FALSE(edit::addGraphic(tl.state, tl.seq, tl.track("V2"), f(0), f(5), Graphic{}));
    CHECK_FALSE(edit::addGraphic(tl.state, tl.seq, tl.track("A1"), f(0), f(5), Graphic{}));
    // Not a graphic clip.
    CHECK_FALSE(edit::setGraphic(tl.state, tl.seq, tl.clip("V1@0"), Graphic{}));
}

TEST_CASE("Layers are added into groups removed with their pins and fields and updated in place",
          "[model][graphics][ops]") {
    Graphic g = lowerThird(MediaId{});
    g.layers.pop_back(); // no logo
    GraphicLayer group;
    group.id = 10;
    group.kind = LayerKind::Group;
    group.children = {rectLayer(11, 5, 5)};
    g.layers.push_back(group);
    g.instance = TemplateInstance{"t", 1, {TemplateField{"name", "Name", FieldKind::Text, 2, "text", {"Name"}}}};
    GraphicClip c(g);

    // A layer whose ids collide is renumbered, its inner pins following.
    GraphicLayer inner;
    inner.id = 2; // taken
    inner.kind = LayerKind::Group;
    GraphicLayer a = rectLayer(3, 10, 10); // taken
    GraphicLayer b = rectLayer(40, 10, 10);
    b.pin = LayerPin{3, LayerPin::kLeft, {5, 0, 0, 0}};
    inner.children = {a, b};
    const auto added = ok(edit::addGraphicLayer(c.state, c.tl.seq, c.clip, inner, 10, 0));
    REQUIRE(added.changes.sequences.size() == 1);
    CHECK(added.changes.sequences[0].modifiedClips == std::vector<ClipId>{c.clip});
    const Graphic& after = graphicOf(added.state, c.tl.seq, c.clip);
    const GraphicLayer* g10 = after.findLayer(10);
    REQUIRE(g10);
    REQUIRE(g10->children.size() == 2);
    const GraphicLayer& placed = g10->children[0]; // at index 0, below layer 11
    CHECK(placed.id == added.createdLayer);
    CHECK(placed.id == 12);
    REQUIRE(placed.children.size() == 2);
    CHECK(placed.children[0].id == 13);
    CHECK(placed.children[1].id == 40);
    CHECK(placed.children[1].pin->target == 13);
    CHECK(g10->children[1].id == 11);
    CHECK_FALSE(edit::addGraphicLayer(c.state, c.tl.seq, c.clip, a, 2)); // layer 2 is not a group
    CHECK_FALSE(edit::addGraphicLayer(c.state, c.tl.seq, c.clip, a, 99));

    // Updating keeps the id.
    GraphicLayer renamed = c.layer(3);
    renamed.id = 77;
    renamed.name = "Subtitle";
    const auto updated = ok(edit::updateGraphicLayer(c.state, c.tl.seq, c.clip, 3, renamed));
    CHECK(graphicOf(updated.state, c.tl.seq, c.clip).findLayer(3)->name == "Subtitle");
    CHECK_FALSE(graphicOf(updated.state, c.tl.seq, c.clip).findLayer(77));
    // Updating to the same layer changes nothing.
    CHECK(ok(edit::updateGraphicLayer(c.state, c.tl.seq, c.clip, 3, c.layer(3))).changes.empty());
    GraphicLayer invalid = c.layer(3);
    invalid.transform.opacity = ParamValue(-5.0);
    CHECK_FALSE(edit::updateGraphicLayer(c.state, c.tl.seq, c.clip, 3, invalid));

    // Removing the name drops the box's pin to it and the field bound to it.
    const auto removed = ok(edit::removeGraphicLayer(c.state, c.tl.seq, c.clip, 2));
    const Graphic& without = graphicOf(removed.state, c.tl.seq, c.clip);
    CHECK_FALSE(without.findLayer(2));
    CHECK_FALSE(without.findLayer(1)->pin);
    CHECK(without.instance->fields.empty());
    // A group goes with its children.
    const auto noGroup = ok(edit::removeGraphicLayer(c.state, c.tl.seq, c.clip, 10));
    CHECK_FALSE(graphicOf(noGroup.state, c.tl.seq, c.clip).findLayer(11));
    CHECK_FALSE(edit::removeGraphicLayer(c.state, c.tl.seq, c.clip, 99));
}

TEST_CASE("Copies of a graphic clip are independent graphics", "[model][graphics][ops]") {
    GraphicClip c(lowerThird(MediaId{}));
    const Graphic before = c.graphic();
    const auto edited = ok(edit::updateGraphicLayer(c.state, c.tl.seq, c.clip, 2, textLayer(0, "Changed")));
    CHECK(graphicOf(edited.state, c.tl.seq, c.clip).findLayer(2)->text.text() == "Changed");
    CHECK(c.graphic() == before); // the earlier revision is untouched
}

// ---- Presets ----------------------------------------------------------------------------------------------------------

TEST_CASE("Fade presets key the opacity at the start and the end of the graphic", "[model][graphics][presets]") {
    GraphicClip c(lowerThird(MediaId{}));
    c.preset(2, options(edit::PresetKind::Fade, edit::PresetPhase::In));
    c.preset(2, options(edit::PresetKind::Fade, edit::PresetPhase::Out, edit::PresetEasing::Linear));
    const ParamValue& opacity = c.layer(2).transform.opacity;
    const std::int64_t end = Ticks::fromSeconds(2).value;
    CHECK(keysOf(opacity) == std::vector<std::pair<std::int64_t, double>>{
                                 {0, 0.0}, {kHalf.value, 100.0}, {end - kHalf.value, 100.0}, {end, 0.0}});
    // Ease out: the arriving key eases in.
    CHECK(interpolationsOf(opacity) ==
          std::vector<Interpolation>{Interpolation::Linear, Interpolation::EaseIn, Interpolation::Linear, Interpolation::Linear});

    // Again with another duration: the window's keys are replaced, the Out keys stay.
    edit::PresetOptions longer = options(edit::PresetKind::Fade, edit::PresetPhase::In);
    longer.duration = Ticks::fromSeconds(1);
    c.preset(2, longer);
    CHECK(keysOf(c.layer(2).transform.opacity) ==
          std::vector<std::pair<std::int64_t, double>>{
              {0, 0.0}, {Ticks::fromSeconds(1).value, 100.0}, {end - kHalf.value, 100.0}, {end, 0.0}});
}

TEST_CASE("Slide scale pop and bounce presets write overshooting keys", "[model][graphics][presets]") {
    GraphicClip c(lowerThird(MediaId{}));
    // Slide in from the left by a quarter of the canvas (480 px) to x = 220, overshooting by 10 %.
    c.preset(2, options(edit::PresetKind::Slide, edit::PresetPhase::In, edit::PresetEasing::Overshoot));
    const std::int64_t at70 = static_cast<std::int64_t>(std::llround(0.7 * static_cast<double>(kHalf.value)));
    CHECK(keysOf(c.layer(2).transform.x) ==
          std::vector<std::pair<std::int64_t, double>>{{0, 220.0 - 480.0}, {at70, 220.0 + 48.0}, {kHalf.value, 220.0}});
    CHECK(keysOf(c.layer(2).transform.y).empty());

    // Slide out downward by 200 px.
    edit::PresetOptions down = options(edit::PresetKind::Slide, edit::PresetPhase::Out, edit::PresetEasing::Linear);
    down.direction = edit::PresetDirection::Down;
    down.distance = 200.0;
    c.preset(3, down);
    const std::int64_t end = Ticks::fromSeconds(2).value;
    CHECK(keysOf(c.layer(3).transform.y) ==
          std::vector<std::pair<std::int64_t, double>>{{end - kHalf.value, 940.0}, {end, 1140.0}});

    // Scale pop: from nothing past full size and back, on both axes.
    c.preset(1, options(edit::PresetKind::ScalePop, edit::PresetPhase::In));
    for (const ParamValue* scale : {&c.layer(1).transform.scaleX, &c.layer(1).transform.scaleY}) {
        const auto pop = keysOf(*scale);
        REQUIRE(pop.size() == 3);
        CHECK(pop[0] == std::pair<std::int64_t, double>{0, 0.0});
        CHECK(pop[1].first == at70);
        CHECK(pop[1].second == Approx(110.0));
        CHECK(pop[2] == std::pair<std::int64_t, double>{kHalf.value, 100.0});
    }

    // Bounce in: drops from above and settles in smaller and smaller hops.
    c.preset(3, options(edit::PresetKind::Bounce, edit::PresetPhase::In));
    const auto bounce = keysOf(c.layer(3).transform.y);
    REQUIRE(bounce.size() == 2 + 6);
    CHECK(bounce[0] == std::pair<std::int64_t, double>{0, 940.0 - 270.0});
    CHECK(bounce[1].second == 940.0);
    CHECK(bounce[2].second == Approx(940.0 - 270.0 * 0.25));
    CHECK(bounce[5].second == 940.0);
}

TEST_CASE("Loop presets repeat over the whole graphic", "[model][graphics][presets]") {
    GraphicClip c(lowerThird(MediaId{}));
    c.preset(1, options(edit::PresetKind::Fade, edit::PresetPhase::Loop));
    // Four 0.5 s cycles of 100 -> 25 -> 100, sharing their ends.
    const auto keys = keysOf(c.layer(1).transform.opacity);
    REQUIRE(keys.size() == 9);
    CHECK(keys[1] == std::pair<std::int64_t, double>{kHalf.value / 2, 25.0});
    CHECK(keys[8] == std::pair<std::int64_t, double>{Ticks::fromSeconds(2).value, 100.0});

    edit::PresetOptions tiny = options(edit::PresetKind::Fade, edit::PresetPhase::Loop);
    tiny.duration = Ticks{kTicksPerSecond / 1000};
    CHECK_FALSE(edit::applyGraphicPreset(c.state, c.tl.seq, c.clip, 1, tiny)); // 2000 cycles
    CHECK_FALSE(edit::applyGraphicPreset(c.state, c.tl.seq, c.clip, 2,
                                         options(edit::PresetKind::Typewriter, edit::PresetPhase::Loop)));
    CHECK_FALSE(edit::applyGraphicPreset(c.state, c.tl.seq, c.clip, 1, options(edit::PresetKind::Wipe, edit::PresetPhase::Loop)));
    edit::PresetOptions zero = options(edit::PresetKind::Fade, edit::PresetPhase::In);
    zero.duration = Ticks{};
    CHECK_FALSE(edit::applyGraphicPreset(c.state, c.tl.seq, c.clip, 1, zero));
    CHECK_FALSE(edit::applyGraphicPreset(c.state, c.tl.seq, c.clip, 99, options(edit::PresetKind::Fade, edit::PresetPhase::In)));
}

TEST_CASE("Typewriter and word-by-word add keyed text-range animators", "[model][graphics][presets][text]") {
    GraphicClip c(lowerThird(MediaId{}));
    c.preset(2, options(edit::PresetKind::Typewriter, edit::PresetPhase::In, edit::PresetEasing::Linear));
    REQUIRE(c.layer(2).text.animators.size() == 1);
    const TextAnimator& tw = c.layer(2).text.animators[0];
    CHECK(tw.name == "Typewriter In");
    CHECK(tw.unit == TextUnit::Character);
    CHECK(tw.opacity == 0.0);
    CHECK(keysOf(tw.start) == std::vector<std::pair<std::int64_t, double>>{{0, 0.0}, {kHalf.value, 100.0}});
    CHECK(tw.end == ParamValue(100.0));

    // Halfway through, "Name" shows its first two characters.
    const auto shown = [&](Ticks t) {
        std::vector<double> out;
        const DrawList list = resolveGraphicClip(c.state, clipOf(c.state, c.tl.seq, c.clip), t);
        for (const DrawItem& i : list.items) {
            if (i.layer == 2) {
                for (const DrawCluster& cl : i.text.clusters) {
                    out.push_back(cl.opacity);
                }
            }
        }
        return out;
    };
    CHECK(shown(Ticks{}) == std::vector<double>{0.0, 0.0, 0.0, 0.0});
    CHECK(shown(Ticks{kHalf.value / 2}) == std::vector<double>{1.0, 1.0, 0.0, 0.0});
    CHECK(shown(kHalf) == std::vector<double>{1.0, 1.0, 1.0, 1.0});

    // Words out: the hidden range grows over the words; applying it again replaces it.
    c.preset(3, options(edit::PresetKind::WordByWord, edit::PresetPhase::Out));
    c.preset(3, options(edit::PresetKind::WordByWord, edit::PresetPhase::Out));
    REQUIRE(c.layer(3).text.animators.size() == 1);
    const TextAnimator& words = c.layer(3).text.animators[0];
    CHECK(words.name == "Words Out");
    CHECK(words.unit == TextUnit::Word);
    CHECK(words.start == ParamValue(0.0));
    const std::int64_t end = Ticks::fromSeconds(2).value;
    CHECK(keysOf(words.end) == std::vector<std::pair<std::int64_t, double>>{{end - kHalf.value, 0.0}, {end, 100.0}});

    CHECK_FALSE(edit::applyGraphicPreset(c.state, c.tl.seq, c.clip, 1,
                                         options(edit::PresetKind::Typewriter, edit::PresetPhase::In))); // a shape
}

TEST_CASE("Wipe presets key a rectangle mask over the layer", "[model][graphics][presets]") {
    GraphicClip c(lowerThird(MediaId{}));
    GraphicLayer box = c.layer(1);
    box.pin.reset();
    c.state = ok(edit::updateGraphicLayer(c.state, c.tl.seq, c.clip, 1, box)).state;
    c.preset(1, options(edit::PresetKind::Wipe, edit::PresetPhase::In, edit::PresetEasing::Linear));
    REQUIRE(c.layer(1).masks.size() == 1);
    const Mask& m = c.layer(1).masks[0];
    CHECK(m.name == "Wipe");
    CHECK(m.shape == MaskShape::Rectangle);
    // The 600 x 120 box: its left edge at -300 px (-0.15625 of the canvas width), 0.3125 wide.
    CHECK(maskValue(m, "size.x", Ticks{}) == Approx(0.0));
    CHECK(maskValue(m, "size.x", kHalf) == Approx(600.0 / 1920.0));
    CHECK(maskValue(m, "center.x", Ticks{}) == Approx(-300.0 / 1920.0));
    CHECK(maskValue(m, "center.x", kHalf) == Approx(0.0));
    CHECK(maskValue(m, "size.y", Ticks{}) == Approx(120.0 / 1080.0));
    // The visible part at the start is empty, at the end the whole box.
    const DrawList start = resolveGraphicClip(c.state, clipOf(c.state, c.tl.seq, c.clip), Ticks{});
    CHECK(start.items[0].masks.size() == 1);
    c.preset(1, options(edit::PresetKind::Wipe, edit::PresetPhase::Out));
    CHECK(c.layer(1).masks.size() == 1); // replaced, not added
}

TEST_CASE("Presets on a responsive graphic key its design time", "[model][graphics][presets]") {
    Graphic g = lowerThird(MediaId{});
    g.responsive = ResponsiveTime{Ticks::fromSeconds(4), Ticks::fromSeconds(1), Ticks::fromSeconds(1)};
    GraphicClip c(g, Ticks::fromSeconds(10));
    c.preset(2, options(edit::PresetKind::Fade, edit::PresetPhase::Out, edit::PresetEasing::Linear));
    const std::int64_t end = Ticks::fromSeconds(4).value;
    CHECK(keysOf(c.layer(2).transform.opacity) ==
          std::vector<std::pair<std::int64_t, double>>{{end - kHalf.value, 100.0}, {end, 0.0}});
    // On the 10 s clip the fade ends with the clip: 0.25 s before the end it is half done.
    const Clip& clip = clipOf(c.state, c.tl.seq, c.clip);
    const auto opacity = [&](Ticks t) {
        for (const DrawItem& i : resolveGraphicClip(c.state, clip, t).items) {
            if (i.layer == 2) {
                return i.opacity;
            }
        }
        return -1.0;
    };
    CHECK(opacity(seconds(5.0)) == Approx(1.0));
    CHECK(opacity(seconds(9.75)) == Approx(0.5));
    CHECK(graphicTimeAt(clip, c.graphic(), seconds(9.75)) == Ticks{end - kHalf.value / 2});
}

// ---- Templates --------------------------------------------------------------------------------------------------------

TEST_CASE("Templates save with field defaults and replacing one bumps its version", "[model][graphics][templates]") {
    Timeline tl(kPicture);
    auto t = edit::makeTemplate(lowerThird(tl.media('A')), "lower-third", "Lower third", lowerThirdFields(),
                                Ticks::fromSeconds(4), "Name and title");
    INFO((t ? std::string() : t.error().message));
    REQUIRE(t);
    CHECK(t->graphic.name == "Lower third");
    CHECK_FALSE(t->graphic.instance);
    CHECK(t->fields[0].value.text == "Name");
    CHECK(t->fields[2].value.color.brand == "Primary");
    CHECK(t->fields[3].value.text == "Noto Sans");
    CHECK(t->fields[3].value.fontWeight == 600);
    CHECK(t->fields[4].value.media == tl.media('A'));
    CHECK(t->fields[5].value.flag);
    CHECK(t->fields[7].value.duration == Ticks::fromSeconds(4));

    const auto saved = ok(edit::saveProjectTemplate(tl.state, *t));
    CHECK(saved.changes.templatesChanged);
    REQUIRE(saved.state.findTemplate("lower-third"));
    CHECK(saved.state.findTemplate("lower-third")->version == 1);
    const auto again = ok(edit::saveProjectTemplate(saved.state, *t));
    CHECK(again.state.findTemplate("lower-third")->version == 2);
    auto other = *t;
    other.id = "a-first";
    const auto two = ok(edit::saveProjectTemplate(again.state, other));
    REQUIRE(two.state.templates.size() == 2);
    CHECK(two.state.templates[0].id == "a-first"); // sorted by id
    const auto removed = ok(edit::removeProjectTemplate(two.state, "a-first"));
    CHECK(removed.state.templates.size() == 1);
    CHECK_FALSE(edit::removeProjectTemplate(removed.state, "a-first"));

    CHECK_FALSE(edit::makeTemplate(lowerThird(MediaId{}), "bad id", "x", {}));
    CHECK_FALSE(edit::makeTemplate(lowerThird(MediaId{}), "ok", "x",
                                   {TemplateField{"f", "F", FieldKind::Text, 1, "text", {}}})); // layer 1 is a shape
}

TEST_CASE("Instantiating a template fills its fields and sizes the clip", "[model][graphics][templates]") {
    Timeline tl(kPicture);
    const GraphicTemplate t =
        *edit::makeTemplate(lowerThird(tl.media('A')), "lower-third", "Lower third", lowerThirdFields(), Ticks::fromSeconds(4));
    FieldValue name;
    name.text = "Somchai";
    const auto r = ok(edit::instantiateTemplate(tl.state, tl.seq, tl.track("V2"), f(0), std::nullopt, t, {{"name", name}}));
    REQUIRE(r.placed.size() == 1);
    const Clip& clip = clipOf(r.state, tl.seq, r.placed[0]);
    CHECK(clip.duration() == Ticks::fromSeconds(4));
    CHECK(clip.name() == "Lower third");
    const Graphic& g = graphicOf(r.state, tl.seq, r.placed[0]);
    REQUIRE(g.instance);
    CHECK(g.instance->templateId == "lower-third");
    CHECK(g.instance->version == 1);
    CHECK(g.findLayer(2)->text.text() == "Somchai");
    CHECK(g.findLayer(2)->text.runs[0].style.fontWeight == 600); // the first run's style stays
    CHECK(g.instance->fields[0].value.text == "Somchai");
    CHECK(g.instance->fields[1].value.text == "Title"); // defaults stay

    // An explicit length; a clip-length field wins over it.
    const auto explicitLength = ok(edit::instantiateTemplate(tl.state, tl.seq, tl.track("V2"), f(0), f(7), t));
    CHECK(clipOf(explicitLength.state, tl.seq, explicitLength.placed[0]).duration() == f(7));
    FieldValue length;
    length.duration = Ticks::fromSeconds(3);
    const auto sized = ok(edit::instantiateTemplate(tl.state, tl.seq, tl.track("V2"), f(0), f(7), t, {{"length", length}}));
    CHECK(clipOf(sized.state, tl.seq, sized.placed[0]).duration() == Ticks::fromSeconds(3));

    // Refusals: an unknown field, a picture that is not in the project, a bad template.
    CHECK_FALSE(edit::instantiateTemplate(tl.state, tl.seq, tl.track("V2"), f(0), std::nullopt, t, {{"nope", name}}));
    FieldValue missing;
    missing.media = MediaId{424242};
    CHECK_FALSE(edit::instantiateTemplate(tl.state, tl.seq, tl.track("V2"), f(0), std::nullopt, t, {{"logo", missing}}));
    FieldValue tooLong;
    tooLong.text = std::string(kMaxGraphicTextBytes + 1, 'a');
    CHECK_FALSE(edit::instantiateTemplate(tl.state, tl.seq, tl.track("V2"), f(0), std::nullopt, t, {{"name", tooLong}}));
    GraphicTemplate broken = t;
    broken.id = "";
    CHECK_FALSE(edit::instantiateTemplate(tl.state, tl.seq, tl.track("V2"), f(0), std::nullopt, broken));
}

TEST_CASE("Setting template fields edits the instance in one edit", "[model][graphics][templates]") {
    Timeline tl(kPicture);
    const GraphicTemplate t =
        *edit::makeTemplate(lowerThird(tl.media('A')), "lower-third", "Lower third", lowerThirdFields(), Ticks::fromSeconds(4));
    const auto placed = ok(edit::instantiateTemplate(tl.state, tl.seq, tl.track("V2"), f(0), std::nullopt, t));
    const ClipId clip = placed.placed[0];

    FieldValue name;
    name.text = "Ann";
    FieldValue color;
    color.color = GraphicColor{{0.0, 0.0, 1.0, 1.0}, "Accent"};
    FieldValue font;
    font.text = "Sarabun";
    font.fontStyle = "Italic";
    font.fontWeight = 300;
    FieldValue hide;
    hide.flag = false;
    FieldValue titleOut;
    titleOut.duration = Ticks::fromSeconds(2);
    FieldValue length;
    length.duration = Ticks::fromSeconds(6);
    const auto r = ok(edit::setTemplateFields(placed.state, tl.seq, clip,
                                              {{"name", name}, {"boxColor", color}, {"font", font}, {"showLogo", hide},
                                               {"titleOut", titleOut}, {"length", length}}));
    const Graphic& g = graphicOf(r.state, tl.seq, clip);
    CHECK(g.findLayer(2)->text.text() == "Ann");
    CHECK(g.findLayer(2)->text.runs[0].style.fontFamily == "Sarabun");
    CHECK(g.findLayer(2)->text.runs[0].style.brandFont.empty()); // a chosen font wins over the brand's
    CHECK(g.findLayer(2)->text.runs[0].style.fontWeight == 300);
    CHECK(g.findLayer(1)->shape.fill.color.brand == "Accent");
    CHECK_FALSE(g.findLayer(4)->visible);
    CHECK(g.findLayer(3)->out == Ticks::fromSeconds(2));
    CHECK(clipOf(r.state, tl.seq, clip).duration() == Ticks::fromSeconds(6));
    // One change set covering the clip: the picture it shows changed.
    REQUIRE(r.changes.sequences.size() == 1);
    CHECK(r.changes.sequences[0].modifiedClips == std::vector<ClipId>{clip});
    CHECK(r.changes.sequences[0].roles.has(PropertyRole::Source));
    // The version history's diff names it, and the template library.
    const ProjectDiff history = diffProjects(placed.state, r.state);
    REQUIRE(history.sequences.size() == 1);
    REQUIRE(history.sequences[0].clips.size() == 1);
    CHECK(history.sequences[0].clips[0].source);
    CHECK_FALSE(history.graphicsLibraryChanged);
    CHECK(diffProjects(tl.state, ok(edit::saveProjectTemplate(tl.state, t)).state).graphicsLibraryChanged);
    for (const TemplateField& f : g.instance->fields) {
        if (f.id == "name") {
            CHECK(f.value.text == "Ann");
        }
    }
    CHECK(edit::currentFieldValue(g, g.instance->fields[0]).text == "Ann");

    // Refusals.
    FieldValue zero;
    CHECK_FALSE(edit::setTemplateFields(placed.state, tl.seq, clip, {{"length", zero}}));
    FieldValue early;
    early.duration = Ticks{};
    CHECK_FALSE(edit::setTemplateFields(placed.state, tl.seq, clip, {{"titleOut", early}}));
    CHECK_FALSE(edit::setTemplateFields(placed.state, tl.seq, clip, {{"unknown", name}}));
    CHECK_FALSE(edit::setTemplateFields(tl.state, tl.seq, tl.clip("V1@0"), {{"name", name}}));
    const auto plain = ok(edit::addGraphic(tl.state, tl.seq, tl.track("V2"), f(0), f(5), Graphic{}));
    CHECK_FALSE(edit::setTemplateFields(plain.state, tl.seq, plain.placed[0], {{"name", name}})); // not an instance
}

// ---- Brand kit --------------------------------------------------------------------------------------------------------

TEST_CASE("Swapping the brand kit re-themes template instances", "[model][graphics][brand]") {
    Timeline tl(kPicture);
    const GraphicTemplate t =
        *edit::makeTemplate(lowerThird(tl.media('A')), "lower-third", "Lower third", lowerThirdFields(), Ticks::fromSeconds(4));
    auto s = ok(edit::instantiateTemplate(tl.state, tl.seq, tl.track("V2"), f(0), std::nullopt, t));
    const ClipId clip = s.placed[0];

    const auto look = [&](const ProjectState& state) {
        const DrawList list = resolveGraphicClip(state, clipOf(state, tl.seq, clip), f(1));
        struct Look {
            Rgba box;
            std::string font;
            Rgba title;
            MediaId logo;
        } out{};
        for (const DrawItem& i : list.items) {
            if (i.layer == 1) {
                out.box = i.fill.color;
            } else if (i.layer == 2) {
                out.font = i.text.runs.at(0).fontFamily;
            } else if (i.layer == 3) {
                out.title = i.text.runs.at(0).fill;
            } else if (i.layer == 4) {
                out.logo = i.image.media;
            }
        }
        return out;
    };
    // Without a kit: the colours, font and media of the design.
    const auto plain = look(s.state);
    CHECK(plain.box == Rgba{0.2, 0.2, 0.2, 1.0});
    CHECK(plain.font == "Noto Sans");
    CHECK(plain.logo == tl.media('A'));

    const auto red = ok(edit::setBrandKit(s.state, kit({1.0, 0.0, 0.0, 1.0}, "Kanit", tl.media('A'))));
    CHECK(red.changes.brandKitChanged);
    CHECK(look(red.state).box == Rgba{1.0, 0.0, 0.0, 1.0});
    CHECK(look(red.state).font == "Kanit");
    CHECK(look(red.state).title == Rgba{1.0, 0.8, 0.0, 1.0});

    // Another kit with another logo picture.
    ProjectState withLogo = red.state;
    MediaItem picture = *withLogo.findMedia(tl.media('A'));
    const auto added = edit::addMedia(withLogo, picture);
    REQUIRE(added);
    const auto blue = ok(edit::setBrandKit(added->state, kit({0.0, 0.0, 1.0, 1.0}, "Prompt", added->id)));
    CHECK(look(blue.state).box == Rgba{0.0, 0.0, 1.0, 1.0});
    CHECK(look(blue.state).font == "Prompt");
    CHECK(look(blue.state).logo == added->id);
    // The instance itself did not change: only the kit.
    CHECK(graphicOf(blue.state, tl.seq, clip) == graphicOf(s.state, tl.seq, clip));

    // Removing the kit goes back to the design; the same kit again changes nothing.
    CHECK(look(ok(edit::setBrandKit(blue.state, std::nullopt)).state).box == Rgba{0.2, 0.2, 0.2, 1.0});
    CHECK(ok(edit::setBrandKit(blue.state, blue.state.brandKit)).changes.empty());
    // A logo that is not a picture of the project, or a bad kit, is refused.
    CHECK_FALSE(edit::setBrandKit(s.state, kit({1, 0, 0, 1}, "Kanit", MediaId{999999})));
    BrandKit dup = kit({1, 0, 0, 1}, "Kanit", tl.media('A'));
    dup.fonts.push_back(dup.fonts[0]);
    CHECK_FALSE(edit::setBrandKit(s.state, dup));
}

// ---- Project merge (C.9) ----------------------------------------------------------------------------------------------

TEST_CASE("Merging revisions merges templates by id and the brand kit as one object", "[model][graphics][merge]") {
    Timeline tl(kPicture);
    const GraphicTemplate t = *edit::makeTemplate(lowerThird(tl.media('A')), "lower-third", "Lower third", lowerThirdFields());
    const ProjectState base = ok(edit::saveProjectTemplate(tl.state, t)).state;

    GraphicTemplate mineOnly = t;
    mineOnly.id = "mine";
    const ProjectState mine = ok(edit::saveProjectTemplate(base, mineOnly)).state;
    GraphicTemplate theirsOnly = t;
    theirsOnly.id = "a-theirs";
    ProjectState theirs = ok(edit::saveProjectTemplate(base, theirsOnly)).state;
    theirs = ok(edit::setBrandKit(theirs, kit({1.0, 0.0, 0.0, 1.0}, "Kanit", tl.media('A')))).state;

    const auto merged = mergeProjects(base, mine, theirs);
    REQUIRE(merged);
    requireValid(merged->merged);
    CHECK(merged->conflicts.empty());
    const auto& templates = merged->merged.templates;
    REQUIRE(templates.size() == 3);
    CHECK(templates[0].id == "a-theirs"); // sorted by id
    CHECK(templates[1].id == "lower-third");
    CHECK(templates[2].id == "mine");
    CHECK(merged->merged.brandKit == theirs.brandKit);

    // Both sides change the kit: a conflict, mine by default, theirs when chosen.
    const ProjectState mineKit = ok(edit::setBrandKit(mine, kit({0.0, 0.0, 1.0, 1.0}, "Prompt", tl.media('A')))).state;
    const auto both = mergeProjects(base, mineKit, theirs);
    REQUIRE(both);
    REQUIRE(both->conflicts.size() == 1);
    CHECK(both->conflicts[0].id == "brandKit");
    CHECK(both->merged.brandKit == mineKit.brandKit);
    const auto chosen = mergeProjects(base, mineKit, theirs, {{"brandKit", MergeChoice::Theirs}});
    REQUIRE(chosen);
    CHECK(chosen->merged.brandKit == theirs.brandKit);
}
