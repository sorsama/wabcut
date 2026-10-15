// C.16: builders of the built-in gaming templates (gaming_templates_author.h).
//
// The rules every template follows (docs/graphics.md, "Built-in templates (C.16)"):
//   - a 1920 x 1080 canvas with responsive time: everything animates in during the protected intro and out during the
//     protected outro, from nothing and to nothing; the middle stretches with the clip and only holds or plays loops;
//   - the poster time (templatePosterTime: the end of the intro) shows every part at rest: the library's thumbnail;
//   - original artwork only: shapes and text, icons drawn here as paths; no logos, no platform names;
//   - fonts that have Thai: Bai Jamjuree 600 for headings, IBM Plex Sans Thai for body text;
//   - a box behind user text is pinned to it (it grows with the text); position animations never go on pinned layers
//     (the pin places them) and wipes only on fixed-size shapes (a wipe's mask keeps the size it was baked with).

#include "gaming_templates_author.h"

#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/GraphicResolve.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace wabcut::test {

namespace {

using edit::PresetDirection;
using edit::PresetEasing;
using edit::PresetKind;
using edit::PresetPhase;

constexpr std::int32_t kWidth = 1920;
constexpr std::int32_t kHeight = 1080;

Ticks ms(std::int64_t milliseconds) { return Ticks{milliseconds * (kTicksPerSecond / 1000)}; }

/// Positions and sizes computed here are kept to 1/100 px, so the files hold short numbers.
double round2(double v) { return std::round(v * 100.0) / 100.0; }

// ---- Look -------------------------------------------------------------------------------------------------------------

GraphicColor rgba(double r, double g, double b, double a = 1.0) { return GraphicColor{{r, g, b, a}, {}}; }

// The palette follows docs/design-language.md ("Ultraviolet Cut"): ink and text colours of the app, the violet accent,
// UV pink, and a red for the recording dot.
GraphicColor white(double a = 1.0) { return rgba(0.953, 0.941, 0.98, a); }
GraphicColor ink(double a = 0.92) { return rgba(0.047, 0.039, 0.071, a); }
GraphicColor raised(double a = 0.95) { return rgba(0.114, 0.098, 0.157, a); }
GraphicColor violet() { return rgba(0.482, 0.231, 1.0); }
GraphicColor pink() { return rgba(0.878, 0.251, 0.984); }
GraphicColor red() { return rgba(1.0, 0.231, 0.361); }

/// Headings: Bai Jamjuree, whose bundled weights are 500 and 600.
TextStyle heading(double size, double tracking = 0.0, GraphicColor fill = white()) {
    TextStyle s;
    s.fontFamily = "Bai Jamjuree";
    s.fontWeight = 600;
    s.fontSize = size;
    s.tracking = tracking;
    s.fill = std::move(fill);
    return s;
}

/// Body text: IBM Plex Sans Thai (400, 500, 600).
TextStyle body(double size, std::int32_t weight, double tracking = 0.0, GraphicColor fill = white()) {
    TextStyle s;
    s.fontFamily = "IBM Plex Sans Thai";
    s.fontWeight = weight;
    s.fontSize = size;
    s.tracking = tracking;
    s.fill = std::move(fill);
    return s;
}

/// The height of one line (automatic leading: 1.2 em, the renderer's and the approximate measurer's).
double lineHeight(const TextStyle& s) { return 1.2 * s.fontSize; }

DropShadow softShadow(double opacity = 0.55, double distance = 6.0, double softness = 14.0) {
    DropShadow s;
    s.enabled = true;
    s.color = ink(1.0);
    s.opacity = opacity;
    s.angle = 90.0;
    s.distance = distance;
    s.softness = softness;
    return s;
}

Paint solid(GraphicColor c) {
    Paint p;
    p.kind = PaintKind::Solid;
    p.color = std::move(c);
    return p;
}

Paint noFill() {
    Paint p;
    p.kind = PaintKind::None;
    return p;
}

Stroke outline(GraphicColor c, double width, LineJoin join = LineJoin::Round, LineCap cap = LineCap::Round) {
    Stroke s;
    s.enabled = true;
    s.color = std::move(c);
    s.width = ParamValue(width);
    s.join = join;
    s.cap = cap;
    return s;
}

TextBackground backdrop(GraphicColor c, double padding, double radius) { return TextBackground{true, std::move(c), padding, radius}; }

// ---- Layers -----------------------------------------------------------------------------------------------------------

GraphicLayer layer(std::uint32_t id, std::string name, LayerKind kind, double x, double y) {
    GraphicLayer l;
    l.id = id;
    l.name = std::move(name);
    l.kind = kind;
    l.transform.x = ParamValue(x);
    l.transform.y = ParamValue(y);
    return l;
}

/// Point text hanging from (x, y): its top-left, top-centre or top-right by `align`.
GraphicLayer text(std::uint32_t id, std::string name, std::string content, const TextStyle& style, TextAlign align, double x,
                  double y) {
    GraphicLayer l = layer(id, std::move(name), LayerKind::Text, x, y);
    l.text.runs = {TextRun{std::move(content), style}};
    l.text.align = align;
    return l;
}

/// Anchors a one-line text layer at the middle of its line: its y is then that middle, and scale animations grow from
/// there.
void anchorMiddle(GraphicLayer& l) { l.transform.anchorY = ParamValue(lineHeight(l.text.runs.front().style) / 2.0); }

GraphicLayer shape(std::uint32_t id, std::string name, ShapeKind kind, double w, double h, Paint fill, double x, double y) {
    GraphicLayer l = layer(id, std::move(name), LayerKind::Shape, x, y);
    l.shape.kind = kind;
    l.shape.width = ParamValue(w);
    l.shape.height = ParamValue(h);
    l.shape.fill = std::move(fill);
    return l;
}

GraphicLayer rect(std::uint32_t id, std::string name, double w, double h, double radius, Paint fill, double x, double y) {
    GraphicLayer l = shape(id, std::move(name), ShapeKind::Rectangle, w, h, std::move(fill), x, y);
    l.shape.cornerRadius = ParamValue(radius);
    return l;
}

GraphicLayer ellipse(std::uint32_t id, std::string name, double w, double h, Paint fill, double x, double y) {
    return shape(id, std::move(name), ShapeKind::Ellipse, w, h, std::move(fill), x, y);
}

GraphicLayer star(std::uint32_t id, std::string name, std::uint32_t points, double outer, double inner, Paint fill, double x,
                  double y) {
    GraphicLayer l = layer(id, std::move(name), LayerKind::Shape, x, y);
    l.shape.kind = ShapeKind::Star;
    l.shape.points = points;
    l.shape.outerRadius = ParamValue(outer);
    l.shape.innerRadius = ParamValue(inner);
    l.shape.fill = std::move(fill);
    return l;
}

MaskVertex pt(double x, double y) { return MaskVertex{x, y, 0.0, 0.0, 0.0, 0.0}; }

MaskVertex curved(double x, double y, double inX, double inY, double outX, double outY) {
    return MaskVertex{x, y, inX, inY, outX, outY};
}

/// `vertices` moved so (cx, cy) is the origin, then scaled by `factor`.
std::vector<MaskVertex> scaled(std::vector<MaskVertex> vertices, double factor, double cx, double cy) {
    for (MaskVertex& v : vertices) {
        v.x = (v.x - cx) * factor;
        v.y = (v.y - cy) * factor;
        v.inX *= factor;
        v.inY *= factor;
        v.outX *= factor;
        v.outY *= factor;
    }
    return vertices;
}

GraphicLayer path(std::uint32_t id, std::string name, std::vector<MaskVertex> vertices, bool closed, Paint fill, double x,
                  double y) {
    GraphicLayer l = layer(id, std::move(name), LayerKind::Shape, x, y);
    l.shape.kind = ShapeKind::Path;
    l.shape.path = {MaskPathKey{Ticks{}, std::move(vertices), Interpolation::Linear}};
    l.shape.closed = closed;
    l.shape.fill = std::move(fill);
    return l;
}

GraphicLayer group(std::uint32_t id, std::string name, std::vector<GraphicLayer> children, double x, double y) {
    GraphicLayer l = layer(id, std::move(name), LayerKind::Group, x, y);
    l.children = std::move(children);
    return l;
}

/// An image slot: empty (no media) until its template field is set.
GraphicLayer image(std::uint32_t id, std::string name, double w, double h, ImageFit fit, double x, double y) {
    GraphicLayer l = layer(id, std::move(name), LayerKind::Image, x, y);
    l.image.width = w;
    l.image.height = h;
    l.image.fit = fit;
    return l;
}

/// A rectangle mask with rounded corners over a w x h box centred on the layer's origin (masks are in the layer's own
/// space, normalised by the canvas).
Mask roundedCorners(double w, double h, double radius) {
    Mask m;
    m.id = 1;
    m.name = "Rounded corners";
    m.shape = MaskShape::Rectangle;
    m.set("center.x", ParamValue(0.0));
    m.set("center.y", ParamValue(0.0));
    m.set("size.x", ParamValue(w / kWidth));
    m.set("size.y", ParamValue(h / kHeight));
    m.set("roundness", ParamValue(radius / (std::min(w, h) / 2.0)));
    return m;
}

// ---- Pins -------------------------------------------------------------------------------------------------------------

constexpr int kL = LayerPin::kLeft;
constexpr int kT = LayerPin::kTop;
constexpr int kR = LayerPin::kRight;
constexpr int kB = LayerPin::kBottom;
constexpr int kAll = kL | kT | kR | kB;

/// The box `l` covers in its parent before any animation (constant, upright, unscaled transforms).
Box placedBox(const GraphicLayer& l) {
    const Box b = layerBounds(l, Ticks{});
    return {b.x + l.transform.x.constant() - l.transform.anchorX.constant(),
            b.y + l.transform.y.constant() - l.transform.anchorY.constant(), b.width, b.height};
}

/// Pins `l` to its sibling `target` on `edges`, with offsets (left, top, right, bottom: this layer's edge minus the
/// target's), and places and sizes it the way the resolver will, so the stored design is what is drawn (with the
/// approximate measurer). Call before baking animations.
void pinTo(GraphicLayer& l, const GraphicLayer& target, int edges, std::array<double, 4> offsets) {
    const Box t = placedBox(target);
    const Box own = placedBox(l);
    const std::array<double, 4> want{t.x + offsets[0], t.y + offsets[1], t.x + t.width + offsets[2], t.y + t.height + offsets[3]};
    const bool resizable =
        l.kind == LayerKind::Shape && (l.shape.kind == ShapeKind::Rectangle || l.shape.kind == ShapeKind::Ellipse);
    const auto axis = [&](int lowEdge, int highEdge, std::size_t low, std::size_t high, ParamValue& position, ParamValue& size,
                          double anchor, double ownLow, double ownHigh) {
        const bool pinLow = (edges & lowEdge) != 0;
        const bool pinHigh = (edges & highEdge) != 0;
        if (pinLow && pinHigh && resizable) {
            size = ParamValue(round2(want[high] - want[low]));
            position = ParamValue(round2((want[low] + want[high]) / 2.0 + anchor));
        } else if (pinLow) {
            position = ParamValue(round2(position.constant() + want[low] - ownLow));
        } else if (pinHigh) {
            position = ParamValue(round2(position.constant() + want[high] - ownHigh));
        }
    };
    axis(kL, kR, 0, 2, l.transform.x, l.shape.width, l.transform.anchorX.constant(), own.x, own.x + own.width);
    axis(kT, kB, 1, 3, l.transform.y, l.shape.height, l.transform.anchorY.constant(), own.y, own.y + own.height);
    l.pin = LayerPin{target.id, static_cast<std::uint8_t>(edges), offsets};
}

// ---- Animation --------------------------------------------------------------------------------------------------------

/// The options of a preset besides its kind, phase and duration.
struct Motion {
    PresetEasing easing = PresetEasing::EaseOut;
    PresetDirection direction = PresetDirection::Left;
    double distance = 0.0;  ///< slide and bounce, pixels (0 = a quarter of the canvas)
    double overshoot = 10.0; ///< scale pop, bounce and overshoot easing, %
};

/// Bakes a preset into `l` for a graphic whose time runs over [fromMs, toMs) (design time).
void bake(GraphicLayer& l, PresetKind kind, PresetPhase phase, std::int64_t durationMs, const Motion& m, std::int64_t fromMs,
          std::int64_t toMs) {
    edit::PresetOptions o;
    o.kind = kind;
    o.phase = phase;
    o.duration = ms(durationMs);
    o.easing = m.easing;
    o.direction = m.direction;
    o.distance = m.distance;
    o.overshoot = m.overshoot;
    auto baked = edit::applyPresetToLayer(l, o, ms(fromMs), ms(toMs), kWidth, kHeight);
    if (!baked) {
        throw std::runtime_error("preset " + std::string(edit::toString(kind)) + " on layer '" + l.name + "': " +
                                 baked.error().message);
    }
    l = std::move(*baked);
}

/// An In preset playing over [atMs, atMs + durationMs]: it animates toward the layer's resting values.
void animateIn(GraphicLayer& l, PresetKind kind, std::int64_t atMs, std::int64_t durationMs, const Motion& m = {}) {
    bake(l, kind, PresetPhase::In, durationMs, m, atMs, atMs + durationMs);
}

/// An Out preset ending at endMs.
void animateOut(GraphicLayer& l, PresetKind kind, std::int64_t endMs, std::int64_t durationMs, const Motion& m = {}) {
    bake(l, kind, PresetPhase::Out, durationMs, m, endMs - durationMs, endMs);
}

/// A loop of cycleMs cycles over [fromMs, toMs).
void animateLoop(GraphicLayer& l, PresetKind kind, std::int64_t fromMs, std::int64_t toMs, std::int64_t cycleMs,
                 const Motion& m = {}) {
    bake(l, kind, PresetPhase::Loop, cycleMs, m, fromMs, toMs);
}

Keyframe key(std::int64_t atMs, double value, Interpolation interpolation = Interpolation::EaseInOut) {
    Keyframe k;
    k.time = ms(atMs);
    k.value = value;
    k.interpolation = interpolation;
    return k;
}

/// Hand-made keys (a click, a swing, a trim drawing on), written into `param` like a preset's: its keys at the same
/// times are replaced.
void addKeys(ParamValue& param, const std::vector<Keyframe>& keys) {
    Curve curve;
    if (const Curve* existing = param.curve()) {
        curve = *existing;
    }
    for (const Keyframe& k : keys) {
        const auto same = std::find_if(curve.keys.begin(), curve.keys.end(), [&](const Keyframe& e) { return e.time == k.time; });
        if (same != curve.keys.end()) {
            *same = k;
        } else {
            curve.keys.push_back(k);
        }
    }
    std::sort(curve.keys.begin(), curve.keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    param = ParamValue(std::move(curve));
}

// ---- Templates --------------------------------------------------------------------------------------------------------

TemplateField field(std::string id, std::string label, FieldKind kind, std::uint32_t layerId, std::string property) {
    TemplateField f;
    f.id = std::move(id);
    f.label = std::move(label);
    f.kind = kind;
    f.layer = layerId;
    f.property = std::move(property);
    return f;
}

TemplateField textField(std::string id, std::string label, std::uint32_t layerId) {
    return field(std::move(id), std::move(label), FieldKind::Text, layerId, "text");
}

TemplateField colorField(std::string id, std::string label, std::uint32_t layerId, std::string property = "fill") {
    return field(std::move(id), std::move(label), FieldKind::Color, layerId, std::move(property));
}

TemplateField checkboxField(std::string id, std::string label, std::uint32_t layerId) {
    return field(std::move(id), std::move(label), FieldKind::Checkbox, layerId, "visible");
}

TemplateField imageField(std::string id, std::string label, std::uint32_t layerId) {
    return field(std::move(id), std::move(label), FieldKind::Image, layerId, "media");
}

/// The clip's length (its default: the template's duration).
TemplateField lengthField() { return field("length", "Length", FieldKind::Duration, 0, "clip"); }

Graphic canvas(std::string name, std::int64_t durationMs, std::int64_t introMs, std::int64_t outroMs) {
    Graphic g;
    g.name = std::move(name);
    g.width = kWidth;
    g.height = kHeight;
    g.responsive = ResponsiveTime{ms(durationMs), ms(introMs), ms(outroMs)};
    return g;
}

/// The template of `g`: its default length is the responsive duration; field defaults are what `g` shows.
GraphicTemplate finish(const Graphic& g, std::string id, std::vector<TemplateField> fields, std::string description) {
    auto made = edit::makeTemplate(g, id, g.name, std::move(fields), g.responsive->duration, std::move(description));
    if (!made) {
        throw std::runtime_error("template " + id + ": " + made.error().message);
    }
    return std::move(*made);
}

// ---- 1. Subscribe, like and bell ----------------------------------------------------------------------------------------

GraphicTemplate subscribe() {
    constexpr std::int64_t kEnd = 4000;
    Graphic g = canvas("Subscribe, like and bell", kEnd, 1400, 600);

    // The subscribe button: a pill pinned to its label, so a longer or translated label widens it.
    const TextStyle labelStyle = heading(46, 60);
    GraphicLayer label = text(3, "Label", "SUBSCRIBE", labelStyle, TextAlign::Center, 0, -lineHeight(labelStyle) / 2.0);
    GraphicLayer button = rect(2, "Button", 0, 0, 50, solid(violet()), 0, 0);
    button.shadow = softShadow();
    pinTo(button, label, kAll, {-48, -22, 48, 22});
    GraphicLayer subscribeButton = group(1, "Subscribe", {button, label}, 830, 900);
    animateIn(subscribeButton, PresetKind::ScalePop, 0, 350, {.overshoot = 15});
    // The click: pressed in, then back a little past full size.
    for (ParamValue* scale : {&subscribeButton.transform.scaleX, &subscribeButton.transform.scaleY}) {
        addKeys(*scale, {key(1050, 100), key(1130, 90), key(1230, 104), key(1320, 100)});
    }
    animateOut(subscribeButton, PresetKind::ScalePop, kEnd, 300);

    // The like button: a thumbs-up (a cuff and a hand) on a round button.
    GraphicLayer likeCircle = ellipse(5, "Like circle", 100, 100, solid(white()), 0, 0);
    likeCircle.shadow = softShadow();
    GraphicLayer cuff = rect(6, "Thumb cuff", 10, 25, 2, solid(ink(1.0)), -18.75, 10.625);
    GraphicLayer hand = path(7, "Thumb",
                             scaled({pt(-10, -2), pt(-4, -10), pt(-2, -19), pt(3, -19), pt(4, -11), pt(2, -4), pt(15, -4),
                                     pt(18, -1), pt(17, 4), pt(17, 9), pt(15, 14), pt(12, 18), pt(-10, 18)},
                                    1.25, -1.0, -0.5),
                             true, solid(ink(1.0)), 0, 0);
    GraphicLayer like = group(4, "Like", {likeCircle, cuff, hand}, 1090, 900);
    animateIn(like, PresetKind::ScalePop, 200, 350, {.overshoot = 15});
    animateOut(like, PresetKind::ScalePop, kEnd - 150, 300);

    // The bell button: the bell swings from its top once it is in.
    GraphicLayer bellCircle = ellipse(9, "Bell circle", 100, 100, solid(white()), 0, 0);
    bellCircle.shadow = softShadow();
    GraphicLayer bellShape = path(11, "Bell shape",
                                  {pt(-3, 0), pt(3, 0), pt(3, 3), pt(12, 9), pt(14, 20), pt(15, 27), pt(21, 33), pt(21, 37),
                                   pt(-21, 37), pt(-21, 33), pt(-15, 27), pt(-14, 20), pt(-12, 9), pt(-3, 3)},
                                  true, solid(ink(1.0)), 0, 0);
    GraphicLayer clapper = ellipse(12, "Clapper", 11, 11, solid(ink(1.0)), 0, 42);
    GraphicLayer bellIcon = group(10, "Bell icon", {bellShape, clapper}, 0, -24);
    addKeys(bellIcon.transform.rotation,
            {key(760, 0), key(840, -16), key(920, 14), key(1000, -10), key(1080, 7), key(1160, -3), key(1240, 0)});
    GraphicLayer bell = group(8, "Bell", {bellCircle, bellIcon}, 1226, 900);
    animateIn(bell, PresetKind::ScalePop, 400, 350, {.overshoot = 15});
    animateOut(bell, PresetKind::ScalePop, kEnd - 300, 300);

    // A pointer that comes up to the button and clicks it.
    GraphicLayer cursor = path(13, "Cursor", {pt(0, 0), pt(0, 46), pt(11, 35), pt(19, 53), pt(27, 49), pt(19, 32), pt(34, 32)},
                               true, solid(white()), 948, 922);
    cursor.shape.stroke = outline(ink(1.0), 3);
    cursor.shadow = softShadow(0.5, 4, 8);
    animateIn(cursor, PresetKind::Fade, 700, 150);
    animateIn(cursor, PresetKind::Slide, 700, 300, {.direction = PresetDirection::Down, .distance = 150});
    animateOut(cursor, PresetKind::Fade, kEnd - 400, 200);

    g.layers = {subscribeButton, like, bell, cursor};
    return finish(g, "wabcut.gaming.subscribe",
                  {textField("label", "Button text", 3), colorField("accent", "Button colour", 2),
                   checkboxField("showLike", "Show like button", 4), checkboxField("showBell", "Show bell button", 8),
                   checkboxField("showCursor", "Show pointer", 13)},
                  "Subscribe, like and bell buttons pop in one by one and the subscribe button gets clicked.");
}

// ---- 2. Kill streak banner ----------------------------------------------------------------------------------------------

GraphicTemplate killStreak() {
    constexpr std::int64_t kEnd = 3000;
    Graphic g = canvas("Kill streak banner", kEnd, 700, 400);

    const TextStyle titleStyle = heading(116, 30);
    GraphicLayer title = text(5, "Title", "DOUBLE KILL", titleStyle, TextAlign::Center, 0, -lineHeight(titleStyle) / 2.0);
    GraphicLayer plate = rect(2, "Plate", 0, 0, 0, solid(ink()), 0, 0);
    pinTo(plate, title, kAll, {-70, -16, 70, 16});
    const double plateHeight = plate.shape.height.constant();
    GraphicLayer bar = rect(3, "Accent bar", 0, 12, 0, solid(violet()), 0, 0);
    pinTo(bar, plate, kL | kR | kB, {0, 0, 0, 12});
    const double half = plateHeight / 2.0;
    const std::vector<MaskVertex> slash{pt(-8, -half), pt(20, -half), pt(8, half), pt(-20, half)};
    GraphicLayer leftSlash = path(4, "Left slash", slash, true, solid(white()), 0, 0);
    pinTo(leftSlash, plate, kL, {-58, 0, 0, 0});
    GraphicLayer rightSlash = path(6, "Right slash", slash, true, solid(white()), 0, 0);
    pinTo(rightSlash, plate, kR, {0, 0, 58, 0});
    GraphicLayer subline = text(7, "Subline", "KILL STREAK x2", body(36, 600, 150), TextAlign::Center, 0, 0);
    subline.text.background = backdrop(ink(), 12, 4);
    pinTo(subline, plate, kT, {0, plateHeight + 34, 0, 0});

    animateIn(leftSlash, PresetKind::Wipe, 120, 260, {.direction = PresetDirection::Right});
    animateIn(rightSlash, PresetKind::Wipe, 120, 260, {.direction = PresetDirection::Left});
    animateIn(bar, PresetKind::ScalePop, 180, 300);
    animateIn(subline, PresetKind::Fade, 250, 150);
    animateIn(subline, PresetKind::Typewriter, 250, 400, {.easing = PresetEasing::Linear});

    GraphicLayer banner = group(1, "Banner", {plate, bar, leftSlash, title, rightSlash, subline}, 960, 300);
    banner.transform.rotation = ParamValue(-3.0);
    animateIn(banner, PresetKind::ScalePop, 0, 280, {.overshoot = 30});
    animateOut(banner, PresetKind::Slide, kEnd, 400,
               {.easing = PresetEasing::EaseIn, .direction = PresetDirection::Right, .distance = 600});
    animateOut(banner, PresetKind::Fade, kEnd, 300, {.easing = PresetEasing::EaseIn});

    g.layers = {banner};
    return finish(g, "wabcut.gaming.killStreak",
                  {textField("title", "Title", 5), textField("subtitle", "Subtitle", 7), colorField("accent", "Accent colour", 3)},
                  "Kill streak banner with a big title and a subtitle; punches in, slides away.");
}

// ---- 3. Lower third -----------------------------------------------------------------------------------------------------

GraphicTemplate lowerThird() {
    constexpr std::int64_t kEnd = 5000;
    Graphic g = canvas("Gamer lower third", kEnd, 700, 400);

    GraphicLayer name = text(4, "Name", "Player Name", heading(64), TextAlign::Left, 0, 0);
    GraphicLayer box = rect(2, "Name box", 0, 0, 0, solid(ink()), 0, 0);
    pinTo(box, name, kAll, {-28, -14, 36, 14});
    GraphicLayer bar = rect(3, "Accent bar", 12, 0, 0, solid(violet()), 0, 0);
    pinTo(bar, box, kL | kT | kB, {-12, 0, 0, 0});
    const Box boxArea = placedBox(box);
    // The handle hangs under the box, its background aligned with the box's left edge.
    GraphicLayer handle = text(5, "Handle", "@handle", body(34, 500, 20, ink(1.0)), TextAlign::Left, round2(boxArea.x + 10),
                               round2(boxArea.y + boxArea.height + 22));
    handle.text.background = backdrop(white(), 10, 4);

    animateIn(name, PresetKind::Typewriter, 200, 400, {.easing = PresetEasing::Linear});
    animateIn(bar, PresetKind::ScalePop, 250, 300);
    animateIn(handle, PresetKind::Fade, 350, 200);
    animateIn(handle, PresetKind::Slide, 350, 300, {.direction = PresetDirection::Down, .distance = 24});

    GraphicLayer lt = group(1, "Lower third", {box, bar, name, handle}, 136, 810);
    animateIn(lt, PresetKind::Slide, 0, 450,
              {.easing = PresetEasing::Overshoot, .direction = PresetDirection::Left, .distance = 640, .overshoot = 4});
    animateIn(lt, PresetKind::Fade, 0, 200);
    animateOut(lt, PresetKind::Slide, kEnd, 400,
               {.easing = PresetEasing::EaseIn, .direction = PresetDirection::Left, .distance = 640});
    animateOut(lt, PresetKind::Fade, kEnd, 300, {.easing = PresetEasing::EaseIn});

    g.layers = {lt};
    return finish(g, "wabcut.gaming.lowerThird",
                  {textField("name", "Name", 4), textField("handle", "Handle", 5), colorField("accent", "Accent colour", 3),
                   lengthField()},
                  "Lower third with a name, a handle and an accent bar; grows with the name.");
}

// ---- 4. Replay stinger --------------------------------------------------------------------------------------------------

GraphicTemplate replay() {
    // Designed at the length of a typical instant replay (a 4 s moment at half speed with its ramps): the middle stretches
    // with the clip (Responsive Design - Time), so the dot blinks near its designed rate in the usual case.
    constexpr std::int64_t kEnd = 8500;
    constexpr std::int64_t kIntro = 750;
    constexpr std::int64_t kOutro = 450;
    Graphic g = canvas("Replay stinger", kEnd, kIntro, kOutro);

    // The stinger: a flash and a band wiping across the frame, which then flies off to the left.
    GraphicLayer flash = rect(1, "Flash", kWidth, kHeight, 0, solid(white()), 960, 540);
    flash.transform.opacity = ParamValue(45.0);
    flash.out = ms(400);
    animateIn(flash, PresetKind::Fade, 0, 60, {.easing = PresetEasing::Linear});
    animateOut(flash, PresetKind::Fade, 380, 230);
    GraphicLayer band = path(3, "Band", {pt(-1000, -125), pt(1060, -125), pt(1000, 125), pt(-1060, 125)}, true, solid(pink()), 0, 0);
    animateIn(band, PresetKind::Wipe, 0, 220, {.direction = PresetDirection::Left});
    GraphicLayer stinger = group(2, "Stinger", {band}, 960, 540);
    stinger.out = ms(650);
    animateOut(stinger, PresetKind::Slide, 650, 220,
               {.easing = PresetEasing::EaseIn, .direction = PresetDirection::Left, .distance = 2300});

    // The tag: one "label" text drives the whole graphic. It slams in three times its size in the middle of the frame,
    // over the band, then flies into the top-left corner, where it stays for the whole replay, its dot blinking (a loop
    // over the stretchable middle).
    const TextStyle tagStyle = heading(44, 120);
    GraphicLayer tagLabel = text(7, "Tag label", "REPLAY", tagStyle, TextAlign::Left, 0, 0);
    GraphicLayer tagBox = rect(5, "Tag box", 0, 0, 8, solid(violet()), 0, 0);
    tagBox.shadow = softShadow(0.45, 4, 10);
    pinTo(tagBox, tagLabel, kAll, {-64, -12, 26, 12});
    GraphicLayer dot = ellipse(6, "Dot", 20, 20, solid(red()), -34, lineHeight(tagStyle) / 2.0);
    animateLoop(dot, PresetKind::Fade, kIntro, kEnd - kOutro, 800, {.easing = PresetEasing::EaseInOut});
    GraphicLayer tag = group(4, "Tag", {tagBox, dot, tagLabel}, 0, 0);
    // The group turns about the tag's middle; at rest the tag's box sits 96 px from the left, 52 px from the top.
    const Box tagArea = placedBox(tagBox);
    const double centreX = round2(tagArea.x + tagArea.width / 2.0);
    const double centreY = round2(tagArea.y + tagArea.height / 2.0);
    tag.transform.anchorX = ParamValue(centreX);
    tag.transform.anchorY = ParamValue(centreY);
    const double restX = round2(96.0 - tagArea.x + centreX);
    const double restY = round2(52.0 - tagArea.y + centreY);
    addKeys(tag.transform.x, {key(0, 960), key(420, 960), key(720, restX)});
    addKeys(tag.transform.y, {key(0, 540), key(420, 540), key(720, restY)});
    for (ParamValue* scale : {&tag.transform.scaleX, &tag.transform.scaleY}) {
        addKeys(*scale, {key(0, 0, Interpolation::Linear), key(150, 330), key(230, 300), key(420, 300), key(720, 100)});
    }
    animateIn(tag, PresetKind::Fade, 0, 100, {.easing = PresetEasing::Linear});
    animateOut(tag, PresetKind::Slide, kEnd, kOutro,
               {.easing = PresetEasing::EaseIn, .direction = PresetDirection::Left, .distance = 420});
    animateOut(tag, PresetKind::Fade, kEnd, 300, {.easing = PresetEasing::EaseIn});

    g.layers = {flash, stinger, tag};
    return finish(g, "wabcut.gaming.replay",
                  {textField("label", "Text", 7), colorField("accent", "Tag colour", 5),
                   colorField("stingerColor", "Stinger colour", 3)},
                  "REPLAY stinger that becomes a corner tag with a blinking dot for a replay of any length.");
}

// ---- 5. Countdown -------------------------------------------------------------------------------------------------------

GraphicTemplate countdown() {
    constexpr std::int64_t kEnd = 4500;
    Graphic g = canvas("Countdown 3-2-1", kEnd, 3500, 500);

    GraphicLayer disc = ellipse(2, "Disc", 300, 300, solid(ink(0.85)), 0, 0);
    GraphicLayer ring = ellipse(3, "Ring", 300, 300, noFill(), 0, 0);
    ring.shape.stroke = outline(violet(), 14);
    addKeys(ring.shape.trimEnd, {key(100, 0), key(600, 100)});
    GraphicLayer ringGroup = group(1, "Ring", {disc, ring}, 960, 540);
    animateIn(ringGroup, PresetKind::ScalePop, 0, 300, {.overshoot = 12});
    animateOut(ringGroup, PresetKind::ScalePop, kEnd, 400);

    // Each count shows for a second (its in and out points), popping in and fading out; GO! holds through the middle.
    const auto count = [](std::uint32_t id, std::string name, std::string label, double size, std::int64_t fromMs,
                          std::int64_t toMs) {
        GraphicLayer l = text(id, std::move(name), std::move(label), heading(size), TextAlign::Center, 960, 540);
        anchorMiddle(l);
        l.shadow = softShadow();
        l.in = ms(fromMs);
        l.out = ms(toMs);
        animateIn(l, PresetKind::ScalePop, fromMs, 300, {.overshoot = 20});
        animateOut(l, PresetKind::Fade, toMs, 200);
        return l;
    };
    GraphicLayer three = count(4, "Three", "3", 200, 0, 1000);
    GraphicLayer two = count(5, "Two", "2", 200, 1000, 2000);
    GraphicLayer one = count(6, "One", "1", 200, 2000, 3000);
    GraphicLayer go = text(7, "Go", "GO!", heading(150, 20), TextAlign::Center, 960, 540);
    anchorMiddle(go);
    go.shadow = softShadow();
    go.in = ms(3000);
    animateIn(go, PresetKind::ScalePop, 3000, 400, {.overshoot = 25});
    animateOut(go, PresetKind::ScalePop, kEnd, 400);

    g.layers = {ringGroup, three, two, one, go};
    return finish(g, "wabcut.gaming.countdown",
                  {textField("count3", "3", 4), textField("count2", "2", 5), textField("count1", "1", 6), textField("go", "Go", 7),
                   colorField("accent", "Ring colour", 3, "stroke")},
                  "Countdown 3, 2, 1 in a ring, one second each, then GO!");
}

// ---- 6. Chat bubble -----------------------------------------------------------------------------------------------------

GraphicTemplate chatBubble() {
    constexpr std::int64_t kEnd = 5000;
    Graphic g = canvas("Chat bubble", kEnd, 650, 300);

    // The (optional) avatar: a circle with a generic person.
    GraphicLayer avatarCircle = ellipse(3, "Avatar circle", 88, 88, solid(pink()), 0, 0);
    GraphicLayer head = ellipse(4, "Avatar head", 30, 30, solid(white()), 0, -10);
    GraphicLayer shoulders = path(5, "Avatar shoulders",
                                  {curved(-22, 30, 0, 0, 0, -12), curved(0, 11, -14, 0, 14, 0), curved(22, 30, 0, -12, 0, 0)},
                                  true, solid(white()), 0, 0);
    GraphicLayer avatar = group(2, "Avatar", {avatarCircle, head, shoulders}, 0, 0);

    // The bubble grows with the message; its tail and the name tag follow its edges.
    GraphicLayer message = text(9, "Message", "GG! That clutch was insane", body(40, 500, 0, ink(1.0)), TextAlign::Left, 88, -28);
    GraphicLayer bubble = rect(6, "Bubble", 0, 0, 26, solid(white()), 0, 0);
    bubble.shadow = softShadow(0.45, 6, 16);
    pinTo(bubble, message, kAll, {-30, -26, 30, 24});
    GraphicLayer tail = path(7, "Tail", {pt(30, -40), pt(30, -8), pt(-16, 4)}, true, solid(white()), 0, 0);
    pinTo(tail, bubble, kL | kB, {-18, 0, 0, -10});
    GraphicLayer username = text(8, "Username", "PlayerOne", heading(30, 20), TextAlign::Left, 0, 0);
    username.text.background = backdrop(violet(), 8, 6);
    pinTo(username, bubble, kL | kT, {26, -22, 0, 0});

    animateIn(username, PresetKind::Fade, 150, 200);
    animateIn(message, PresetKind::WordByWord, 200, 450, {.easing = PresetEasing::Linear});

    // The whole message pops from the avatar and scrolls up as it leaves.
    GraphicLayer chat = group(1, "Chat", {avatar, bubble, tail, username, message}, 190, 760);
    animateIn(chat, PresetKind::ScalePop, 0, 350, {.overshoot = 12});
    animateOut(chat, PresetKind::Slide, kEnd, 300, {.easing = PresetEasing::EaseIn, .direction = PresetDirection::Up, .distance = 80});
    animateOut(chat, PresetKind::Fade, kEnd, 300, {.easing = PresetEasing::EaseIn});

    g.layers = {chat};
    return finish(g, "wabcut.gaming.chatBubble",
                  {textField("username", "Username", 8), textField("message", "Message", 9),
                   colorField("accent", "Name colour", 8, "background"), colorField("avatarColor", "Avatar colour", 3),
                   checkboxField("showAvatar", "Show avatar", 2), lengthField()},
                  "Chat message bubble with a username and an optional avatar; grows with the message.");
}

// ---- 7. Emote pop -------------------------------------------------------------------------------------------------------

GraphicTemplate emotePop() {
    constexpr std::int64_t kEnd = 3000;
    constexpr std::int64_t kIntro = 750;
    constexpr std::int64_t kOutro = 350;
    Graphic g = canvas("Emote pop", kEnd, kIntro, kOutro);

    GraphicLayer burst = star(2, "Burst", 12, 230, 175, solid(violet()), 0, 0);
    animateIn(burst, PresetKind::ScalePop, 0, 350, {.overshoot = 15});
    animateLoop(burst, PresetKind::ScalePop, kIntro, kEnd - kOutro, 950, {.easing = PresetEasing::EaseInOut, .overshoot = 5});
    GraphicLayer glow = ellipse(3, "Glow", 340, 340, solid(white(0.18)), 0, 0);
    animateIn(glow, PresetKind::ScalePop, 60, 340);
    // The image slot: empty until the field is set (an empty slot draws nothing).
    GraphicLayer emote = image(4, "Emote", 300, 300, ImageFit::Contain, 0, 0);
    animateIn(emote, PresetKind::Bounce, 0, 650, {.direction = PresetDirection::Up, .distance = 220});
    animateIn(emote, PresetKind::Fade, 0, 150);
    GraphicLayer sparkle1 = star(5, "Sparkle 1", 4, 30, 8, solid(white()), -215, -150);
    animateIn(sparkle1, PresetKind::ScalePop, 300, 250, {.overshoot = 30});
    GraphicLayer sparkle2 = star(6, "Sparkle 2", 4, 22, 6, solid(white()), 225, -115);
    animateIn(sparkle2, PresetKind::ScalePop, 380, 250, {.overshoot = 30});
    GraphicLayer sparkle3 = star(7, "Sparkle 3", 4, 26, 7, solid(white()), 195, 175);
    animateIn(sparkle3, PresetKind::ScalePop, 460, 250, {.overshoot = 30});
    GraphicLayer caption = text(8, "Caption", "NICE!", heading(84, 40), TextAlign::Center, 0, 300);
    anchorMiddle(caption);
    caption.shadow = softShadow(0.8, 4, 10);
    animateIn(caption, PresetKind::ScalePop, 420, 300, {.overshoot = 20});

    GraphicLayer pop = group(1, "Emote pop", {burst, glow, emote, sparkle1, sparkle2, sparkle3, caption}, 960, 470);
    animateOut(pop, PresetKind::ScalePop, kEnd, kOutro);

    g.layers = {pop};
    return finish(g, "wabcut.gaming.emotePop",
                  {imageField("emote", "Emote image", 4), textField("caption", "Caption", 8),
                   colorField("accent", "Burst colour", 2), checkboxField("showCaption", "Show caption", 8)},
                  "Emote pop: your image bounces in on a pulsing burst, with a caption.");
}

// ---- 8. Arrow callout ---------------------------------------------------------------------------------------------------

GraphicTemplate arrowCallout() {
    constexpr std::int64_t kEnd = 4000;
    constexpr std::int64_t kIntro = 600;
    constexpr std::int64_t kOutro = 300;
    Graphic g = canvas("Arrow callout", kEnd, kIntro, kOutro);

    // The arrow points along its own x axis (tip at the origin); the group turns it to point down-left at (760, 640).
    GraphicLayer arrowShape = path(2, "Arrow shape",
                                   {pt(0, 0), pt(-70, -52), pt(-70, -18), pt(-300, -18), pt(-300, 18), pt(-70, 18), pt(-70, 52)},
                                   true, solid(violet()), 0, 0);
    arrowShape.shape.stroke = outline(white(), 5);
    arrowShape.shadow = softShadow();
    animateIn(arrowShape, PresetKind::Slide, 0, 420,
              {.easing = PresetEasing::Overshoot, .direction = PresetDirection::Left, .distance = 220, .overshoot = 8});
    animateIn(arrowShape, PresetKind::Fade, 0, 180);
    // It keeps poking at the target.
    animateLoop(arrowShape, PresetKind::Slide, kIntro, kEnd - kOutro, 700,
                {.easing = PresetEasing::EaseInOut, .direction = PresetDirection::Left, .distance = 26});
    animateOut(arrowShape, PresetKind::Fade, kEnd, kOutro, {.easing = PresetEasing::EaseIn});
    GraphicLayer arrow = group(1, "Arrow", {arrowShape}, 760, 640);
    arrow.transform.rotation = ParamValue(145.0);

    GraphicLayer label = text(3, "Label", "LOOK HERE!", heading(60, 30), TextAlign::Center, 1150, 395);
    anchorMiddle(label);
    label.text.background = backdrop(ink(), 18, 10);
    animateIn(label, PresetKind::ScalePop, 180, 300, {.overshoot = 15});
    animateOut(label, PresetKind::Fade, kEnd, kOutro, {.easing = PresetEasing::EaseIn});

    g.layers = {arrow, label};
    return finish(g, "wabcut.gaming.arrowCallout",
                  {textField("label", "Label", 3), colorField("accent", "Arrow colour", 2), lengthField()},
                  "Arrow callout with a label; the arrow slides in and keeps pointing.");
}

// ---- 9. Circle callout --------------------------------------------------------------------------------------------------

GraphicTemplate circleCallout() {
    constexpr std::int64_t kEnd = 4000;
    constexpr std::int64_t kIntro = 750;
    constexpr std::int64_t kOutro = 400;
    Graphic g = canvas("Circle callout", kEnd, kIntro, kOutro);

    // The circle draws itself on (trim end 0 -> 100 %) and off (trim start) like a marker stroke.
    GraphicLayer circle = ellipse(1, "Circle", 440, 300, noFill(), 900, 500);
    circle.transform.rotation = ParamValue(-8.0);
    circle.shape.stroke = outline(violet(), 10);
    circle.shadow = softShadow(0.5, 4, 10);
    addKeys(circle.shape.trimEnd, {key(0, 0), key(550, 100)});
    addKeys(circle.shape.trimStart, {key(kEnd - kOutro, 0), key(kEnd, 100)});

    GraphicLayer leader = path(2, "Leader", {pt(0, 0), pt(92, 70)}, false, noFill(), 1088, 578);
    leader.shape.stroke = outline(white(), 5);
    addKeys(leader.shape.trimEnd, {key(450, 0), key(650, 100)});
    animateOut(leader, PresetKind::Fade, kEnd, 300, {.easing = PresetEasing::EaseIn});

    GraphicLayer label = text(3, "Label", "CHECK THIS OUT", heading(52, 40), TextAlign::Left, 1200, 628);
    label.text.background = backdrop(ink(), 16, 8);
    animateIn(label, PresetKind::Fade, 500, 200);
    animateIn(label, PresetKind::Slide, 500, 250, {.direction = PresetDirection::Down, .distance = 24});
    animateOut(label, PresetKind::Fade, kEnd, 300, {.easing = PresetEasing::EaseIn});

    g.layers = {circle, leader, label};
    return finish(g, "wabcut.gaming.circleCallout",
                  {textField("label", "Label", 3), colorField("accent", "Circle colour", 1, "stroke"), lengthField()},
                  "Circle callout that draws itself around something, with a label.");
}

// ---- 10. Webcam frame ---------------------------------------------------------------------------------------------------

GraphicTemplate webcamFrame() {
    constexpr std::int64_t kEnd = 6000;
    constexpr std::int64_t kIntro = 700;
    constexpr std::int64_t kOutro = 350;
    Graphic g = canvas("Webcam frame", kEnd, kIntro, kOutro);

    // A 480 x 270 facecam frame at the bottom left: a border only (the webcam goes on a track below).
    GraphicLayer glow = rect(2, "Glow", 480, 270, 22, noFill(), 0, 0);
    glow.shape.stroke = outline(white(0.14), 20);
    animateIn(glow, PresetKind::Fade, 250, 350);
    GraphicLayer border = rect(3, "Border", 480, 270, 22, noFill(), 0, 0);
    border.shape.stroke = outline(violet(), 8);
    addKeys(border.shape.trimEnd, {key(0, 0), key(550, 100)});
    addKeys(border.shape.trimStart, {key(kEnd - kOutro, 0), key(kEnd, 100)});
    GraphicLayer topRight = path(4, "Corner top right", {pt(-34, 0), pt(0, 0), pt(0, 34)}, false, noFill(), 0, 0);
    topRight.shape.stroke = outline(white(), 6, LineJoin::Miter, LineCap::Square);
    pinTo(topRight, border, kR | kT, {0, -14, 14, 0});
    GraphicLayer bottomLeft = path(5, "Corner bottom left", {pt(0, -34), pt(0, 0), pt(34, 0)}, false, noFill(), 0, 0);
    bottomLeft.shape.stroke = outline(white(), 6, LineJoin::Miter, LineCap::Square);
    pinTo(bottomLeft, border, kL | kB, {-14, 0, 0, 14});
    animateIn(topRight, PresetKind::ScalePop, 300, 250, {.overshoot = 20});
    animateIn(bottomLeft, PresetKind::ScalePop, 380, 250, {.overshoot = 20});
    GraphicLayer tag = text(6, "Name tag", "PlayerOne", heading(30, 40), TextAlign::Left, 0, 0);
    tag.text.background = backdrop(violet(), 10, 6);
    pinTo(tag, border, kL | kT, {30, -18, 0, 0});
    animateIn(tag, PresetKind::Fade, 350, 200);
    animateIn(tag, PresetKind::Typewriter, 350, 350, {.easing = PresetEasing::Linear});

    GraphicLayer webcam = group(1, "Webcam", {glow, border, topRight, bottomLeft, tag}, 280, 900);
    animateOut(webcam, PresetKind::Fade, kEnd, kOutro, {.easing = PresetEasing::EaseIn});

    g.layers = {webcam};
    return finish(g, "wabcut.gaming.webcamFrame",
                  {textField("name", "Name", 6), colorField("accent", "Frame colour", 3, "stroke"),
                   colorField("tagColor", "Tag colour", 6, "background"), lengthField()},
                  "Facecam frame for the bottom-left corner with a name tag; webcam on the track below.");
}

// ---- 11. End card -------------------------------------------------------------------------------------------------------

GraphicTemplate endCard() {
    constexpr std::int64_t kEnd = 20000;
    constexpr std::int64_t kIntro = 1000;
    constexpr std::int64_t kOutro = 600;
    Graphic g = canvas("End card", kEnd, kIntro, kOutro);

    GraphicLayer backdropLayer = rect(1, "Backdrop", kWidth, kHeight, 0, solid(ink(0.72)), 960, 540);
    animateIn(backdropLayer, PresetKind::Fade, 0, 400);
    animateOut(backdropLayer, PresetKind::Fade, kEnd, kOutro, {.easing = PresetEasing::EaseIn});

    GraphicLayer headline = text(3, "Headline", "FOLLOW ME", heading(150, 20), TextAlign::Left, 140, 250);
    headline.shadow = softShadow();
    animateIn(headline, PresetKind::Slide, 100, 450,
              {.easing = PresetEasing::Overshoot, .direction = PresetDirection::Left, .distance = 160, .overshoot = 6});
    animateIn(headline, PresetKind::Fade, 100, 200);
    GraphicLayer bar = rect(4, "Accent bar", 220, 14, 0, solid(violet()), 146, 456);
    bar.transform.anchorX = ParamValue(-110.0); // grows from its left end
    animateIn(bar, PresetKind::ScalePop, 350, 300);
    GraphicLayer handle = text(5, "Handle", "@yourchannel", body(52, 500, 20, white(0.9)), TextAlign::Left, 146, 490);
    animateIn(handle, PresetKind::Fade, 450, 250);
    animateIn(handle, PresetKind::Slide, 450, 300, {.direction = PresetDirection::Down, .distance = 30});

    // Where a channel (subscribe) element can go: a dashed ring (optional).
    GraphicLayer ring = ellipse(7, "Channel ring", 220, 220, noFill(), 0, 0);
    ring.shape.stroke = outline(white(0.85), 6);
    ring.shape.stroke.dashes = {18, 12};
    addKeys(ring.shape.trimEnd, {key(500, 0), key(1000, 100)});
    GraphicLayer plus = path(8, "Channel plus",
                             {pt(-6, -26), pt(6, -26), pt(6, -6), pt(26, -6), pt(26, 6), pt(6, 6), pt(6, 26), pt(-6, 26),
                              pt(-6, 6), pt(-26, 6), pt(-26, -6), pt(-6, -6)},
                             true, solid(white(0.6)), 0, 0);
    animateIn(plus, PresetKind::ScalePop, 700, 250, {.overshoot = 20});
    GraphicLayer channel = group(6, "Channel", {ring, plus}, 256, 790);

    // Two 16:9 video slots where the end-screen video elements go, each with an optional thumbnail image.
    const auto slot = [](std::uint32_t firstId, const std::string& number, double y, std::int64_t popAt) {
        GraphicLayer back = rect(firstId + 1, "Slot " + number + " back", 640, 360, 20, solid(raised()), 0, 0);
        back.shadow = softShadow(0.6, 8, 24);
        GraphicLayer play = path(firstId + 2, "Slot " + number + " play", {pt(-24, -30), pt(36, 0), pt(-24, 30)}, true,
                                 solid(white(0.5)), 0, 0);
        GraphicLayer thumb = image(firstId + 3, "Video " + number + " thumbnail", 640, 360, ImageFit::Cover, 0, 0);
        thumb.masks = {roundedCorners(640, 360, 20)};
        GraphicLayer frame = rect(firstId + 4, "Slot " + number + " frame", 640, 360, 20, noFill(), 0, 0);
        frame.shape.stroke = outline(white(), 5);
        GraphicLayer s = group(firstId, "Video slot " + number, {back, play, thumb, frame}, 1400, y);
        animateIn(s, PresetKind::ScalePop, popAt, 350, {.overshoot = 6});
        return s;
    };
    GraphicLayer slot1 = slot(9, "1", 300, 300);
    GraphicLayer slot2 = slot(14, "2", 760, 450);

    GraphicLayer content = group(2, "Content", {headline, bar, handle, channel, slot1, slot2}, 0, 0);
    animateOut(content, PresetKind::Fade, kEnd, 500, {.easing = PresetEasing::EaseIn});

    g.layers = {backdropLayer, content};
    return finish(g, "wabcut.gaming.endCard",
                  {textField("title", "Title", 3), textField("handle", "Handle", 5), colorField("accent", "Accent colour", 4),
                   imageField("video1Thumb", "Video 1 thumbnail", 12), imageField("video2Thumb", "Video 2 thumbnail", 17),
                   checkboxField("showChannel", "Show channel circle", 6), lengthField()},
                  "20-second end card with a follow title, your handle and two video slots.");
}

// ---- 12. Intro ----------------------------------------------------------------------------------------------------------

GraphicTemplate intro() {
    constexpr std::int64_t kEnd = 4000;
    constexpr std::int64_t kIntro = 900;
    constexpr std::int64_t kOutro = 400;
    Graphic g = canvas("Gaming intro", kEnd, kIntro, kOutro);

    GraphicLayer band = path(2, "Band", {pt(-640, -150), pt(720, -150), pt(640, 150), pt(-720, 150)}, true, solid(violet()), 0, 0);
    animateIn(band, PresetKind::Wipe, 0, 350, {.direction = PresetDirection::Left});
    GraphicLayer stripe = path(3, "Stripe", {pt(-560, 168), pt(600, 168), pt(594, 184), pt(-566, 184)}, true, solid(white(0.85)), 0, 0);
    animateIn(stripe, PresetKind::Wipe, 120, 300, {.direction = PresetDirection::Left});
    GraphicLayer slash = group(1, "Slash", {band, stripe}, 960, 500);
    animateOut(slash, PresetKind::Wipe, kEnd, kOutro, {.easing = PresetEasing::EaseIn, .direction = PresetDirection::Left});

    GraphicLayer title = text(4, "Title", "GAME ON", heading(180, 60), TextAlign::Center, 960, 480);
    anchorMiddle(title);
    title.shadow = softShadow(0.6, 6, 14);
    animateIn(title, PresetKind::Slide, 150, 400,
              {.easing = PresetEasing::Overshoot, .direction = PresetDirection::Down, .distance = 90, .overshoot = 6});
    animateIn(title, PresetKind::Fade, 150, 200);
    animateOut(title, PresetKind::Slide, kEnd, kOutro, {.easing = PresetEasing::EaseIn, .direction = PresetDirection::Up, .distance = 90});
    animateOut(title, PresetKind::Fade, kEnd, 300, {.easing = PresetEasing::EaseIn});

    GraphicLayer subtitle = text(5, "Subtitle", "CHANNEL NAME", body(44, 600, 300), TextAlign::Center, 960, 724);
    anchorMiddle(subtitle);
    GraphicLayer lineLeft = rect(6, "Line left", 140, 6, 0, solid(white()), 0, 724);
    pinTo(lineLeft, subtitle, kL, {-180, 0, 0, 0});
    GraphicLayer lineRight = rect(7, "Line right", 140, 6, 0, solid(white()), 0, 724);
    pinTo(lineRight, subtitle, kR, {0, 0, 180, 0});
    animateIn(subtitle, PresetKind::WordByWord, 400, 450, {.easing = PresetEasing::Linear});
    animateOut(subtitle, PresetKind::Fade, kEnd, 300, {.easing = PresetEasing::EaseIn});
    animateIn(lineLeft, PresetKind::Wipe, 450, 300, {.direction = PresetDirection::Right});
    animateOut(lineLeft, PresetKind::Fade, kEnd, 300, {.easing = PresetEasing::EaseIn});
    animateIn(lineRight, PresetKind::Wipe, 450, 300, {.direction = PresetDirection::Left});
    animateOut(lineRight, PresetKind::Fade, kEnd, 300, {.easing = PresetEasing::EaseIn});

    g.layers = {slash, title, subtitle, lineLeft, lineRight};
    return finish(g, "wabcut.gaming.intro",
                  {textField("title", "Title", 4), textField("subtitle", "Channel line", 5), colorField("accent", "Accent colour", 2),
                   lengthField()},
                  "Intro title with a channel line over an accent band that wipes in and out.");
}

} // namespace

std::vector<GraphicTemplate> authorGamingTemplates() {
    std::vector<GraphicTemplate> out{subscribe(),     killStreak(),  lowerThird(),  replay(),   countdown(), chatBubble(),
                                     emotePop(),      arrowCallout(), circleCallout(), webcamFrame(), endCard(), intro()};
    std::sort(out.begin(), out.end(), [](const GraphicTemplate& a, const GraphicTemplate& b) { return a.id < b.id; });
    return out;
}

} // namespace wabcut::test
