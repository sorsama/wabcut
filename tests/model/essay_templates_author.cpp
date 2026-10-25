// 0.4: builders of the built-in essay and documentary templates (essay_templates_author.h).
//
// The rules every template follows (docs/graphics.md, "Built-in templates (C.16)"), the gaming pack's plus the brand kit:
//   - a 1920 x 1080 canvas with responsive time: everything animates in during the protected intro and out during the
//     protected outro, from nothing and to nothing; the middle stretches with the clip and only holds;
//   - the poster time (templatePosterTime: the end of the intro) shows every part at rest: the library's thumbnail;
//   - original artwork only: shapes and text; no logos, no platform names;
//   - the brand kit: boxes and bars take the kit's "Primary" / "Accent" colours (neutral fallbacks without a kit) and
//     text its "heading" / "body" font roles, with IBM Plex Sans Thai (400..600) as the family, so Thai renders;
//   - a box behind user text is pinned to it (it grows with the text); position animations never go on pinned layers
//     and wipes only on fixed-size shapes.
// The helpers are copies of gaming_templates_author.cpp's (that file's byte-for-byte check must stay as it is).

#include "essay_templates_author.h"

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

/// Neutral text and shadow colours (documentary graphics stay out of the picture's way).
GraphicColor white(double a = 1.0) { return rgba(0.96, 0.96, 0.95, a); }
GraphicColor ink(double a = 1.0) { return rgba(0.03, 0.03, 0.04, a); }

/// The brand kit's "Primary" colour (boxes, the chapter card); a neutral near-black without a kit.
GraphicColor primary() {
    GraphicColor c = rgba(0.07, 0.07, 0.08, 1.0);
    c.brand = "Primary";
    return c;
}

/// The brand kit's "Accent" colour (rules and bars); a warm off-white without a kit.
GraphicColor accent() {
    GraphicColor c = rgba(0.93, 0.92, 0.88, 1.0);
    c.brand = "Accent";
    return c;
}

/// Headings: the kit's "heading" font; IBM Plex Sans Thai (it has Thai) without a kit.
TextStyle heading(double size, std::int32_t weight = 600, double tracking = 0.0, GraphicColor fill = white()) {
    TextStyle s;
    s.fontFamily = "IBM Plex Sans Thai";
    s.brandFont = "heading";
    s.fontWeight = weight;
    s.fontSize = size;
    s.tracking = tracking;
    s.fill = std::move(fill);
    return s;
}

/// Body text: the kit's "body" font; IBM Plex Sans Thai without a kit.
TextStyle body(double size, std::int32_t weight = 400, double tracking = 0.0, GraphicColor fill = white()) {
    TextStyle s;
    s.fontFamily = "IBM Plex Sans Thai";
    s.brandFont = "body";
    s.fontWeight = weight;
    s.fontSize = size;
    s.tracking = tracking;
    s.fill = std::move(fill);
    return s;
}

/// The height of one line (automatic leading: 1.2 em, the renderer's and the approximate measurer's).
double lineHeight(const TextStyle& s) { return 1.2 * s.fontSize; }

/// A soft shadow under text that sits on the footage.
DropShadow softShadow(double opacity = 0.6, double distance = 3.0, double softness = 10.0) {
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

/// Text hanging from (x, y): its top-left, top-centre or top-right by `align`; `wrap` > 0 makes it paragraph text.
GraphicLayer text(std::uint32_t id, std::string name, std::string content, const TextStyle& style, TextAlign align, double x,
                  double y, double wrap = 0.0) {
    GraphicLayer l = layer(id, std::move(name), LayerKind::Text, x, y);
    l.text.runs = {TextRun{std::move(content), style}};
    l.text.align = align;
    l.text.boxWidth = wrap;
    return l;
}

GraphicLayer rect(std::uint32_t id, std::string name, double w, double h, double radius, Paint fill, double x, double y) {
    GraphicLayer l = layer(id, std::move(name), LayerKind::Shape, x, y);
    l.shape.kind = ShapeKind::Rectangle;
    l.shape.width = ParamValue(w);
    l.shape.height = ParamValue(h);
    l.shape.cornerRadius = ParamValue(radius);
    l.shape.fill = std::move(fill);
    return l;
}

GraphicLayer group(std::uint32_t id, std::string name, std::vector<GraphicLayer> children, double x, double y) {
    GraphicLayer l = layer(id, std::move(name), LayerKind::Group, x, y);
    l.children = std::move(children);
    return l;
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
    double distance = 0.0;  ///< slide, pixels (0 = a quarter of the canvas)
    double overshoot = 10.0; ///< scale pop and overshoot easing, %
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

// ---- 1. Name and title ------------------------------------------------------------------------------------------------

GraphicTemplate nameTitle() {
    constexpr std::int64_t kEnd = 5000;
    Graphic g = canvas("Name and title", kEnd, 700, 400);

    // A thin accent bar beside two lines: the name (heading) over the title or role (body).
    const TextStyle nameStyle = heading(64, 600);
    const TextStyle roleStyle = body(36, 400, 10, white(0.88));
    GraphicLayer name = text(3, "Name", "Full Name", nameStyle, TextAlign::Left, 28, 0);
    name.shadow = softShadow();
    const double roleY = round2(lineHeight(nameStyle) + 4);
    GraphicLayer role = text(4, "Role", "Title or role", roleStyle, TextAlign::Left, 28, roleY);
    role.shadow = softShadow();
    const double barHeight = round2(roleY + lineHeight(roleStyle));
    GraphicLayer bar = rect(2, "Accent bar", 6, barHeight, 0, solid(accent()), 3, round2(barHeight / 2.0));

    animateIn(bar, PresetKind::Wipe, 100, 300, {.direction = PresetDirection::Down});
    animateIn(role, PresetKind::Fade, 250, 300);
    animateIn(role, PresetKind::Slide, 250, 300, {.direction = PresetDirection::Down, .distance = 16});

    GraphicLayer lt = group(1, "Name and title", {bar, name, role}, 150, 806);
    animateIn(lt, PresetKind::Slide, 0, 500, {.direction = PresetDirection::Down, .distance = 40});
    animateIn(lt, PresetKind::Fade, 0, 300);
    animateOut(lt, PresetKind::Fade, kEnd, 400, {.easing = PresetEasing::EaseIn});

    g.layers = {lt};
    return finish(g, "wabcut.essay.nameTitle",
                  {textField("name", "Name", 3), textField("role", "Title or role", 4), colorField("accent", "Accent colour", 2),
                   lengthField()},
                  "Lower third with a name over a title or role and a thin accent bar.");
}

// ---- 2. Location and date ---------------------------------------------------------------------------------------------

GraphicTemplate location() {
    constexpr std::int64_t kEnd = 5000;
    Graphic g = canvas("Location and date", kEnd, 700, 400);

    // A short accent rule over the place (heading) and the date (body), in the upper left.
    const TextStyle placeStyle = heading(56, 600);
    GraphicLayer rule = rect(2, "Accent rule", 72, 6, 0, solid(accent()), 36, 3);
    GraphicLayer place = text(3, "Place", "Place name", placeStyle, TextAlign::Left, 0, 22);
    place.shadow = softShadow();
    GraphicLayer date = text(4, "Date", "Date or year", body(34, 400, 20, white(0.88)), TextAlign::Left, 0,
                             round2(22 + lineHeight(placeStyle) + 6));
    date.shadow = softShadow();

    animateIn(rule, PresetKind::Wipe, 0, 350, {.direction = PresetDirection::Right});
    animateIn(date, PresetKind::Fade, 250, 250);
    animateIn(date, PresetKind::Slide, 250, 300, {.direction = PresetDirection::Down, .distance = 16});

    GraphicLayer card = group(1, "Location and date", {rule, place, date}, 140, 110);
    animateIn(card, PresetKind::Slide, 0, 450, {.direction = PresetDirection::Left, .distance = 40});
    animateIn(card, PresetKind::Fade, 0, 300);
    animateOut(card, PresetKind::Fade, kEnd, 400, {.easing = PresetEasing::EaseIn});

    g.layers = {card};
    return finish(g, "wabcut.essay.location",
                  {textField("place", "Place", 3), textField("date", "Date", 4), colorField("accent", "Accent colour", 2),
                   lengthField()},
                  "Place and date in the upper left with a short accent rule.");
}

// ---- 3. Quote ---------------------------------------------------------------------------------------------------------

GraphicTemplate quote() {
    constexpr std::int64_t kEnd = 7000;
    Graphic g = canvas("Quote", kEnd, 800, 500);

    // The quote wraps at 1200 px; the dimmed box is pinned to it (it grows with the text), the accent rule to the box's
    // top and the attribution hangs under the box (its bottom pinned to the box's bottom).
    const TextStyle quoteStyle = heading(56, 500);
    const TextStyle attributionStyle = body(34, 400, 20, white(0.88));
    GraphicLayer words = text(4, "Quote", "A sentence worth quoting, set large and centred so it reads at a glance.", quoteStyle,
                              TextAlign::Center, 0, 0, 1200);
    GraphicLayer box = rect(2, "Quote box", 0, 0, 18, solid(primary()), 0, 0);
    box.transform.opacity = ParamValue(84.0);
    pinTo(box, words, kAll, {-56, -44, 56, 44});
    GraphicLayer rule = rect(3, "Accent rule", 80, 6, 0, solid(accent()), 0, 0);
    pinTo(rule, box, kT, {0, 0, 0, 0});
    GraphicLayer attribution = text(5, "Attribution", "Name, source", attributionStyle, TextAlign::Center, 0, 0);
    attribution.shadow = softShadow();
    pinTo(attribution, box, kB, {0, 0, 0, round2(24 + lineHeight(attributionStyle))});

    animateIn(box, PresetKind::ScalePop, 0, 350, {.overshoot = 4});
    animateIn(rule, PresetKind::Wipe, 300, 250, {.direction = PresetDirection::Right});
    animateIn(words, PresetKind::WordByWord, 200, 500, {.easing = PresetEasing::Linear});
    animateIn(attribution, PresetKind::Fade, 500, 250);

    GraphicLayer block = group(1, "Quote", {box, rule, words, attribution}, 960, 430);
    animateIn(block, PresetKind::Fade, 0, 300);
    animateOut(block, PresetKind::Fade, kEnd, 500, {.easing = PresetEasing::EaseIn});

    g.layers = {block};
    return finish(g, "wabcut.essay.quote",
                  {textField("quote", "Quote", 4), textField("attribution", "Attribution", 5),
                   colorField("accent", "Accent colour", 3), lengthField()},
                  "Centred pull quote in a dimmed box that grows with the text, with the attribution below.");
}

// ---- 4. Source --------------------------------------------------------------------------------------------------------

GraphicTemplate source() {
    constexpr std::int64_t kEnd = 5000;
    Graphic g = canvas("Source", kEnd, 600, 400);

    // A small citation in the lower right: the source (heading) over a detail line (body), right-aligned beside a thin
    // accent bar.
    const TextStyle citationStyle = heading(30, 600);
    const TextStyle detailStyle = body(24, 400, 10, white(0.85));
    GraphicLayer citation = text(3, "Citation", "Source: Archive name", citationStyle, TextAlign::Right, -24, 0);
    citation.shadow = softShadow();
    const double detailY = round2(lineHeight(citationStyle) + 4);
    GraphicLayer detail = text(4, "Detail", "Collection, year", detailStyle, TextAlign::Right, -24, detailY);
    detail.shadow = softShadow();
    const double barHeight = round2(detailY + lineHeight(detailStyle));
    GraphicLayer bar = rect(2, "Accent bar", 4, barHeight, 0, solid(accent()), -2, round2(barHeight / 2.0));

    animateIn(bar, PresetKind::Wipe, 50, 300, {.direction = PresetDirection::Down});
    animateIn(detail, PresetKind::Fade, 200, 250);

    GraphicLayer line = group(1, "Source", {bar, citation, detail}, 1780, 930);
    animateIn(line, PresetKind::Slide, 0, 450, {.direction = PresetDirection::Right, .distance = 30});
    animateIn(line, PresetKind::Fade, 0, 300);
    animateOut(line, PresetKind::Fade, kEnd, 400, {.easing = PresetEasing::EaseIn});

    g.layers = {line};
    return finish(g, "wabcut.essay.source",
                  {textField("citation", "Citation", 3), textField("detail", "Detail", 4), colorField("accent", "Accent colour", 2),
                   lengthField()},
                  "Small source citation in the lower right with a detail line.");
}

// ---- 5. Chapter card --------------------------------------------------------------------------------------------------

GraphicTemplate chapterCard() {
    constexpr std::int64_t kEnd = 4000;
    Graphic g = canvas("Chapter card", kEnd, 800, 600);

    // A full-frame card in the kit's Primary colour: the chapter number in spaced capitals over an accent rule and the
    // title (wrapping at 1500 px).
    GraphicLayer background = rect(1, "Card", kWidth, kHeight, 0, solid(primary()), 960, 540);
    animateIn(background, PresetKind::Fade, 0, 400);
    animateOut(background, PresetKind::Fade, kEnd, 500, {.easing = PresetEasing::EaseIn});

    // The chapter number is the card's first field, so it takes the kit's heading font like the title.
    const TextStyle numberStyle = heading(34, 600, 300, white(0.8));
    GraphicLayer number = text(3, "Chapter number", "CHAPTER 1", numberStyle, TextAlign::Center, 0, 0);
    const double ruleY = round2(lineHeight(numberStyle) + 22);
    GraphicLayer rule = rect(4, "Accent rule", 96, 6, 0, solid(accent()), 0, ruleY);
    GraphicLayer title = text(5, "Chapter title", "Chapter title", heading(96, 600), TextAlign::Center, 0, round2(ruleY + 30), 1500);

    animateIn(number, PresetKind::Fade, 200, 250);
    animateIn(number, PresetKind::Slide, 200, 300, {.direction = PresetDirection::Down, .distance = 20});
    animateIn(rule, PresetKind::Wipe, 300, 300, {.direction = PresetDirection::Right});
    animateIn(title, PresetKind::Fade, 350, 350);
    animateIn(title, PresetKind::Slide, 350, 400, {.direction = PresetDirection::Down, .distance = 30});

    GraphicLayer content = group(2, "Chapter", {number, rule, title}, 960, 440);
    animateIn(content, PresetKind::Fade, 0, 250);
    animateOut(content, PresetKind::Fade, kEnd, 400, {.easing = PresetEasing::EaseIn});

    g.layers = {background, content};
    return finish(g, "wabcut.essay.chapterCard",
                  {textField("number", "Chapter number", 3), textField("title", "Title", 5), colorField("accent", "Accent colour", 4),
                   colorField("backgroundColor", "Card colour", 1), lengthField()},
                  "Full-frame chapter card: a small chapter number over the chapter title.");
}

} // namespace

std::vector<GraphicTemplate> authorEssayTemplates() {
    std::vector<GraphicTemplate> out{nameTitle(), location(), quote(), source(), chapterCard()};
    std::sort(out.begin(), out.end(), [](const GraphicTemplate& a, const GraphicTemplate& b) { return a.id < b.id; });
    return out;
}

} // namespace wabcut::test
