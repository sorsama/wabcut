// Graphics as JSON (C.13): GraphicJson.h, and the pieces ProjectIO.cpp embeds in project files (JsonIo.h).
//
// Every field is written when it differs from its default (whatever the layer's kind), so reading gives back exactly
// what was written. Readers accept a few shorthands for hand-written JSON (commands): a number for a constant parameter,
// "#RRGGBB(AA)" for a colour.

#include <wabcut/model/GraphicJson.h>

#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>

#include "JsonIo.h"

#include <algorithm>
#include <fstream>

namespace wabcut::io {

namespace detail {

namespace {

constexpr std::int64_t kMaxTicks = std::int64_t{1} << 60;

// ---- Small values -----------------------------------------------------------------------------------------------------

/// Readers of objects whose keys are all optional check the type first, so "[]" or 5 is an error, not the defaults.
void requireObject(const Reader& r) {
    if (!r.value().is_object()) {
        r.fail("expected an object");
    }
}

json paramJson(const ParamValue& v) {
    if (v.isConstant()) {
        return v.constant();
    }
    return paramValueToJson(v);
}

/// A number, a curve {"keys": [{"t", "v", ...}, ...]} (ProjectIO's), or just its array of keys.
ParamValue readParam(const Reader& r) {
    if (r.value().is_number()) {
        return ParamValue(r.asDouble());
    }
    if (r.value().is_array()) {
        const json wrapped{{"keys", r.value()}};
        return paramValueFromJson(Reader(wrapped, r.path()));
    }
    return paramValueFromJson(r);
}

ParamValue param(const Reader& r, const char* key, double fallback) {
    return r.has(key) ? readParam(r.child(key)) : ParamValue(fallback);
}

double number(const Reader& r, const char* key, double fallback) { return r.has(key) ? r.child(key).asDouble() : fallback; }

json colorJson(const GraphicColor& c) {
    json rgba = json::array({c.rgba[0], c.rgba[1], c.rgba[2], c.rgba[3]});
    if (c.brand.empty()) {
        return rgba;
    }
    return json{{"brand", c.brand}, {"rgba", std::move(rgba)}};
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::array<double, 4> readRgba(const Reader& r) {
    if (r.value().is_string()) {
        const std::string s = r.asString();
        if ((s.size() != 7 && s.size() != 9) || s[0] != '#') {
            r.fail("a colour string is \"#RRGGBB\" or \"#RRGGBBAA\"");
        }
        std::array<double, 4> rgba{0.0, 0.0, 0.0, 1.0};
        for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
            const int hi = hexDigit(s[i + 1]);
            const int lo = i + 2 < s.size() ? hexDigit(s[i + 2]) : -1;
            if (hi < 0 || lo < 0) {
                r.fail("a colour string is \"#RRGGBB\" or \"#RRGGBBAA\"");
            }
            rgba[i / 2] = (hi * 16 + lo) / 255.0;
        }
        return rgba;
    }
    const std::size_t n = r.arraySize();
    if (n != 3 && n != 4) {
        r.fail("a colour is [r, g, b] or [r, g, b, a], each 0..1");
    }
    std::array<double, 4> rgba{0.0, 0.0, 0.0, 1.0};
    for (std::size_t i = 0; i < n; ++i) {
        rgba[i] = r.element(i).asDouble();
    }
    return rgba;
}

GraphicColor readColor(const Reader& r) {
    GraphicColor c;
    if (r.value().is_object()) {
        c.brand = r.string("brand");
        if (r.has("rgba")) {
            c.rgba = readRgba(r.child("rgba"));
        }
        return c;
    }
    c.rgba = readRgba(r);
    return c;
}

GraphicColor color(const Reader& r, const char* key, GraphicColor fallback) {
    return r.has(key) ? readColor(r.child(key)) : std::move(fallback);
}

json pointJson(const std::array<double, 2>& p) { return json::array({p[0], p[1]}); }

std::array<double, 2> readPoint(const Reader& r) {
    if (r.arraySize() != 2) {
        r.fail("a point is [x, y]");
    }
    return {r.element(0).asDouble(), r.element(1).asDouble()};
}

template <class E>
E enumOf(const Reader& r, const char* key, E fallback, std::optional<E> (*parse)(std::string_view)) {
    if (!r.has(key)) {
        return fallback;
    }
    const std::string name = r.child(key).asString();
    if (const auto v = parse(name)) {
        return *v;
    }
    r.child(key).fail("unknown value \"" + name.substr(0, 64) + "\"");
}

std::uint32_t uint32(const Reader& r, const char* key, std::uint32_t fallback) {
    const std::uint64_t v = r.uint64(key, fallback);
    if (v > 0xFFFFFFFFu) {
        r.child(key).fail("value out of range");
    }
    return static_cast<std::uint32_t>(v);
}

Ticks boundedTicks(const Reader& r, const char* key, Ticks fallback) {
    const Ticks t = r.has(key) ? r.ticks(key) : fallback;
    if (t.value < 0 || t.value >= kMaxTicks) {
        r.child(key).fail("must be 0 or more and within the longest timeline");
    }
    return t;
}

/// Writes `key` when `value` differs from `fallback`.
template <class T, class F>
void put(json& out, const char* key, const T& value, const T& fallback, F&& toJson) {
    if (!(value == fallback)) {
        out[key] = toJson(value);
    }
}

void putParam(json& out, const char* key, const ParamValue& v, double fallback) {
    put(out, key, v, ParamValue(fallback), paramJson);
}

void putNumber(json& out, const char* key, double v, double fallback) {
    if (v != fallback) {
        out[key] = v;
    }
}

const auto kIdentity = [](const auto& v) { return json(v); };

// ---- Paint, stroke, shadow --------------------------------------------------------------------------------------------

json paintJson(const Paint& p) {
    json out{{"type", std::string(toString(p.kind))}};
    const Paint d;
    put(out, "color", p.color, d.color, colorJson);
    if (!p.stops.empty()) {
        json stops = json::array();
        for (const GradientStop& s : p.stops) {
            stops.push_back(json{{"offset", s.offset}, {"color", colorJson(s.color)}});
        }
        out["stops"] = std::move(stops);
    }
    put(out, "start", p.start, d.start, pointJson);
    put(out, "end", p.end, d.end, pointJson);
    return out;
}

Paint readPaint(const Reader& r) {
    requireObject(r);
    Paint p;
    p.kind = enumOf(r, "type", PaintKind::Solid, paintKindFromString);
    p.color = color(r, "color", p.color);
    if (r.has("stops")) {
        const Reader stops = r.child("stops");
        const std::size_t n = stops.arraySize();
        if (n > kMaxGradientStops) {
            stops.fail("too many gradient stops");
        }
        for (std::size_t i = 0; i < n; ++i) {
            const Reader s = stops.element(i);
            p.stops.push_back(GradientStop{number(s, "offset", 0.0), color(s, "color", GraphicColor{})});
        }
    }
    if (r.has("start")) {
        p.start = readPoint(r.child("start"));
    }
    if (r.has("end")) {
        p.end = readPoint(r.child("end"));
    }
    return p;
}

json strokeJson(const Stroke& s) {
    json out = json::object();
    const Stroke d;
    if (s.enabled != d.enabled) {
        out["enabled"] = s.enabled;
    }
    put(out, "color", s.color, d.color, colorJson);
    putParam(out, "width", s.width, 2.0);
    if (!s.dashes.empty()) {
        out["dashes"] = s.dashes;
    }
    put(out, "join", s.join, d.join, [](LineJoin v) { return std::string(toString(v)); });
    put(out, "cap", s.cap, d.cap, [](LineCap v) { return std::string(toString(v)); });
    return out;
}

Stroke readStroke(const Reader& r) {
    requireObject(r);
    Stroke s;
    s.enabled = r.boolean("enabled", s.enabled);
    s.color = color(r, "color", s.color);
    s.width = param(r, "width", 2.0);
    if (r.has("dashes")) {
        const Reader dashes = r.child("dashes");
        const std::size_t n = dashes.arraySize();
        if (n > kMaxDashes) {
            dashes.fail("more than 16 dash lengths");
        }
        for (std::size_t i = 0; i < n; ++i) {
            s.dashes.push_back(dashes.element(i).asDouble());
        }
    }
    s.join = enumOf(r, "join", s.join, lineJoinFromString);
    s.cap = enumOf(r, "cap", s.cap, lineCapFromString);
    return s;
}

json shadowJson(const DropShadow& s) {
    json out = json::object();
    const DropShadow d;
    if (s.enabled != d.enabled) {
        out["enabled"] = s.enabled;
    }
    put(out, "color", s.color, d.color, colorJson);
    putNumber(out, "opacity", s.opacity, d.opacity);
    putNumber(out, "angle", s.angle, d.angle);
    putNumber(out, "distance", s.distance, d.distance);
    putNumber(out, "softness", s.softness, d.softness);
    return out;
}

DropShadow readShadow(const Reader& r) {
    requireObject(r);
    DropShadow s;
    s.enabled = r.boolean("enabled", s.enabled);
    s.color = color(r, "color", s.color);
    s.opacity = number(r, "opacity", s.opacity);
    s.angle = number(r, "angle", s.angle);
    s.distance = number(r, "distance", s.distance);
    s.softness = number(r, "softness", s.softness);
    return s;
}

// ---- Text -------------------------------------------------------------------------------------------------------------

json runJson(const TextRun& run) {
    json out{{"text", run.text}};
    const TextStyle& s = run.style;
    const TextStyle d;
    put(out, "font", s.fontFamily, d.fontFamily, kIdentity);
    put(out, "style", s.fontStyle, d.fontStyle, kIdentity);
    put(out, "brandFont", s.brandFont, d.brandFont, kIdentity);
    put(out, "weight", s.fontWeight, d.fontWeight, kIdentity);
    putNumber(out, "size", s.fontSize, d.fontSize);
    put(out, "fill", s.fill, d.fill, colorJson);
    put(out, "italic", s.italic, d.italic, kIdentity);
    put(out, "underline", s.underline, d.underline, kIdentity);
    putNumber(out, "tracking", s.tracking, d.tracking);
    putNumber(out, "leading", s.leading, d.leading);
    putNumber(out, "baselineShift", s.baselineShift, d.baselineShift);
    return out;
}

TextRun readRun(const Reader& r) {
    requireObject(r);
    TextRun run;
    run.text = r.string("text");
    TextStyle& s = run.style;
    s.fontFamily = r.string("font");
    s.fontStyle = r.string("style");
    s.brandFont = r.string("brandFont");
    s.fontWeight = r.int32In("weight", s.fontWeight, 1, 1000);
    s.fontSize = number(r, "size", s.fontSize);
    s.fill = color(r, "fill", s.fill);
    s.italic = r.boolean("italic", false);
    s.underline = r.boolean("underline", false);
    s.tracking = number(r, "tracking", 0.0);
    s.leading = number(r, "leading", 0.0);
    s.baselineShift = number(r, "baselineShift", 0.0);
    return run;
}

json animatorJson(const TextAnimator& a) {
    json out = json::object();
    const TextAnimator d;
    put(out, "name", a.name, d.name, kIdentity);
    out["unit"] = std::string(toString(a.unit));
    putParam(out, "start", a.start, 0.0);
    putParam(out, "end", a.end, 100.0);
    putParam(out, "offset", a.offset, 0.0);
    put(out, "shape", a.shape, d.shape, [](TextSelectorShape v) { return std::string(toString(v)); });
    putNumber(out, "softness", a.softness, d.softness);
    putNumber(out, "opacity", a.opacity, d.opacity);
    putNumber(out, "offsetX", a.offsetX, d.offsetX);
    putNumber(out, "offsetY", a.offsetY, d.offsetY);
    putNumber(out, "scale", a.scale, d.scale);
    putNumber(out, "rotation", a.rotation, d.rotation);
    return out;
}

TextAnimator readAnimator(const Reader& r) {
    requireObject(r);
    TextAnimator a;
    a.name = r.string("name");
    a.unit = enumOf(r, "unit", a.unit, textUnitFromString);
    a.start = param(r, "start", 0.0);
    a.end = param(r, "end", 100.0);
    a.offset = param(r, "offset", 0.0);
    a.shape = enumOf(r, "shape", a.shape, textSelectorShapeFromString);
    a.softness = number(r, "softness", a.softness);
    a.opacity = number(r, "opacity", a.opacity);
    a.offsetX = number(r, "offsetX", a.offsetX);
    a.offsetY = number(r, "offsetY", a.offsetY);
    a.scale = number(r, "scale", a.scale);
    a.rotation = number(r, "rotation", a.rotation);
    return a;
}

json pathJson(const std::vector<MaskPathKey>& path);
std::vector<MaskPathKey> readPath(const Reader& r);

/// 0.2: type on a path ("onPath").
json textPathJson(const TextPath& p) {
    json out = json::object();
    const TextPath d;
    out["path"] = pathJson(p.path);
    put(out, "closed", p.closed, d.closed, kIdentity);
    put(out, "reverse", p.reverse, d.reverse, kIdentity);
    put(out, "perpendicular", p.perpendicular, d.perpendicular, kIdentity);
    put(out, "forceAlignment", p.forceAlignment, d.forceAlignment, kIdentity);
    putParam(out, "firstMargin", p.firstMargin, 0.0);
    putParam(out, "lastMargin", p.lastMargin, 0.0);
    return out;
}

TextPath readTextPath(const Reader& r) {
    requireObject(r);
    TextPath p;
    if (r.has("path")) {
        p.path = readPath(r.child("path"));
    }
    p.closed = r.boolean("closed", false);
    p.reverse = r.boolean("reverse", false);
    p.perpendicular = r.boolean("perpendicular", true);
    p.forceAlignment = r.boolean("forceAlignment", false);
    p.firstMargin = param(r, "firstMargin", 0.0);
    p.lastMargin = param(r, "lastMargin", 0.0);
    return p;
}

json textJson(const TextContent& t) {
    json out = json::object();
    const TextContent d;
    json runs = json::array();
    for (const TextRun& run : t.runs) {
        runs.push_back(runJson(run));
    }
    out["runs"] = std::move(runs);
    put(out, "align", t.align, d.align, [](TextAlign v) { return std::string(toString(v)); });
    putNumber(out, "boxWidth", t.boxWidth, d.boxWidth);
    putNumber(out, "boxHeight", t.boxHeight, d.boxHeight);
    put(out, "language", t.language, d.language, kIdentity);
    if (!(t.background == d.background)) {
        json b = json::object();
        if (t.background.enabled) {
            b["enabled"] = true;
        }
        put(b, "color", t.background.color, d.background.color, colorJson);
        putNumber(b, "padding", t.background.padding, d.background.padding);
        putNumber(b, "radius", t.background.radius, d.background.radius);
        out["background"] = std::move(b);
    }
    if (!t.animators.empty()) {
        json list = json::array();
        for (const TextAnimator& a : t.animators) {
            list.push_back(animatorJson(a));
        }
        out["animators"] = std::move(list);
    }
    if (t.path) {
        out["onPath"] = textPathJson(*t.path);
    }
    return out;
}

TextContent readText(const Reader& r) {
    requireObject(r);
    TextContent t;
    if (r.has("runs")) {
        const Reader runs = r.child("runs");
        const std::size_t n = runs.arraySize();
        if (n > kMaxTextRuns) {
            runs.fail("more than " + std::to_string(kMaxTextRuns) + " runs");
        }
        for (std::size_t i = 0; i < n; ++i) {
            t.runs.push_back(readRun(runs.element(i)));
        }
    }
    t.align = enumOf(r, "align", t.align, textAlignFromString);
    t.boxWidth = number(r, "boxWidth", 0.0);
    t.boxHeight = number(r, "boxHeight", 0.0);
    t.language = r.string("language");
    if (r.has("background")) {
        const Reader b = r.child("background");
        t.background.enabled = b.boolean("enabled", false);
        t.background.color = color(b, "color", t.background.color);
        t.background.padding = number(b, "padding", t.background.padding);
        t.background.radius = number(b, "radius", t.background.radius);
    }
    if (r.has("animators")) {
        const Reader list = r.child("animators");
        const std::size_t n = list.arraySize();
        if (n > kMaxTextAnimators) {
            list.fail("more than " + std::to_string(kMaxTextAnimators) + " animators");
        }
        for (std::size_t i = 0; i < n; ++i) {
            t.animators.push_back(readAnimator(list.element(i)));
        }
    }
    if (r.has("onPath")) {
        t.path = readTextPath(r.child("onPath"));
    }
    return t;
}

// ---- Shapes, images ---------------------------------------------------------------------------------------------------

json pathJson(const std::vector<MaskPathKey>& path) {
    json keys = json::array();
    for (const MaskPathKey& k : path) {
        json vertices = json::array();
        for (const MaskVertex& v : k.vertices) {
            vertices.push_back(json::array({v.x, v.y, v.inX, v.inY, v.outX, v.outY}));
        }
        keys.push_back(json{{"t", k.time.value}, {"i", std::string(toString(k.interpolation))}, {"v", std::move(vertices)}});
    }
    return keys;
}

std::vector<MaskPathKey> readPath(const Reader& r) {
    std::vector<MaskPathKey> path;
    const std::size_t n = r.arraySize();
    if (n > kMaxPathKeys) {
        r.fail("too many path keys");
    }
    for (std::size_t i = 0; i < n; ++i) {
        const Reader k = r.element(i);
        MaskPathKey key;
        key.time = k.has("t") ? k.ticks("t") : Ticks{};
        const std::string interpolation = k.string("i", "linear");
        const auto parsed = interpolationFromString(interpolation);
        if (!parsed) {
            k.child("i").fail("unknown interpolation \"" + interpolation.substr(0, 32) + "\"");
        }
        key.interpolation = *parsed;
        const Reader vertices = k.child("v");
        const std::size_t count = vertices.arraySize();
        if (count > kMaxPathVertices) {
            vertices.fail("more than " + std::to_string(kMaxPathVertices) + " vertices");
        }
        for (std::size_t j = 0; j < count; ++j) {
            const Reader v = vertices.element(j);
            const std::size_t values = v.arraySize();
            if (values != 2 && values != 6) {
                v.fail("a vertex is [x, y] or [x, y, inX, inY, outX, outY]");
            }
            MaskVertex vertex;
            vertex.x = v.element(0).asDouble();
            vertex.y = v.element(1).asDouble();
            if (values == 6) {
                vertex.inX = v.element(2).asDouble();
                vertex.inY = v.element(3).asDouble();
                vertex.outX = v.element(4).asDouble();
                vertex.outY = v.element(5).asDouble();
            }
            key.vertices.push_back(vertex);
        }
        path.push_back(std::move(key));
    }
    return path;
}

json shapeJson(const ShapeContent& s) {
    json out{{"type", std::string(toString(s.kind))}};
    const ShapeContent d;
    putParam(out, "width", s.width, 200.0);
    putParam(out, "height", s.height, 100.0);
    putParam(out, "cornerRadius", s.cornerRadius, 0.0);
    put(out, "points", s.points, d.points, kIdentity);
    putParam(out, "outerRadius", s.outerRadius, 100.0);
    putParam(out, "innerRadius", s.innerRadius, 50.0);
    if (!s.path.empty()) {
        out["path"] = pathJson(s.path);
    }
    put(out, "closed", s.closed, d.closed, kIdentity);
    put(out, "fill", s.fill, d.fill, paintJson);
    put(out, "stroke", s.stroke, d.stroke, strokeJson);
    putParam(out, "trimStart", s.trimStart, 0.0);
    putParam(out, "trimEnd", s.trimEnd, 100.0);
    putParam(out, "trimOffset", s.trimOffset, 0.0);
    return out;
}

ShapeContent readShape(const Reader& r) {
    requireObject(r);
    ShapeContent s;
    s.kind = enumOf(r, "type", s.kind, shapeKindFromString);
    s.width = param(r, "width", 200.0);
    s.height = param(r, "height", 100.0);
    s.cornerRadius = param(r, "cornerRadius", 0.0);
    s.points = uint32(r, "points", s.points);
    s.outerRadius = param(r, "outerRadius", 100.0);
    s.innerRadius = param(r, "innerRadius", 50.0);
    if (r.has("path")) {
        s.path = readPath(r.child("path"));
    }
    s.closed = r.boolean("closed", true);
    if (r.has("fill")) {
        s.fill = readPaint(r.child("fill"));
    }
    if (r.has("stroke")) {
        s.stroke = readStroke(r.child("stroke"));
    }
    s.trimStart = param(r, "trimStart", 0.0);
    s.trimEnd = param(r, "trimEnd", 100.0);
    s.trimOffset = param(r, "trimOffset", 0.0);
    return s;
}

json imageJson(const ImageContent& i) {
    json out = json::object();
    const ImageContent d;
    if (i.media.isValid()) {
        out["media"] = i.media.value;
    }
    put(out, "brandLogo", i.brandLogo, d.brandLogo, kIdentity);
    putNumber(out, "width", i.width, d.width);
    putNumber(out, "height", i.height, d.height);
    put(out, "fit", i.fit, d.fit, [](ImageFit v) { return std::string(toString(v)); });
    return out;
}

ImageContent readImage(const Reader& r) {
    requireObject(r);
    ImageContent i;
    i.media = MediaId{r.uint64("media", 0)};
    i.brandLogo = r.string("brandLogo");
    i.width = number(r, "width", i.width);
    i.height = number(r, "height", i.height);
    i.fit = enumOf(r, "fit", i.fit, imageFitFromString);
    return i;
}

// ---- Layers -----------------------------------------------------------------------------------------------------------

json transformJson(const LayerTransform& t) {
    json out = json::object();
    putParam(out, "x", t.x, 0.0);
    putParam(out, "y", t.y, 0.0);
    putParam(out, "anchorX", t.anchorX, 0.0);
    putParam(out, "anchorY", t.anchorY, 0.0);
    putParam(out, "scaleX", t.scaleX, 100.0);
    putParam(out, "scaleY", t.scaleY, 100.0);
    putParam(out, "rotation", t.rotation, 0.0);
    putParam(out, "opacity", t.opacity, 100.0);
    return out;
}

LayerTransform readTransform(const Reader& r) {
    requireObject(r);
    LayerTransform t;
    t.x = param(r, "x", 0.0);
    t.y = param(r, "y", 0.0);
    t.anchorX = param(r, "anchorX", 0.0);
    t.anchorY = param(r, "anchorY", 0.0);
    t.scaleX = param(r, "scaleX", 100.0);
    t.scaleY = param(r, "scaleY", 100.0);
    t.rotation = param(r, "rotation", 0.0);
    t.opacity = param(r, "opacity", 100.0);
    return t;
}

constexpr std::array<std::pair<std::uint8_t, const char*>, 4> kEdges{
    {{LayerPin::kLeft, "left"}, {LayerPin::kTop, "top"}, {LayerPin::kRight, "right"}, {LayerPin::kBottom, "bottom"}}};

json pinJson(const LayerPin& p) {
    json edges = json::array();
    for (const auto& [bit, name] : kEdges) {
        if (p.edges & bit) {
            edges.push_back(name);
        }
    }
    // 0.2: "frame" for the video frame (LayerPin::kVideoFrame).
    return json{{"to", p.target == LayerPin::kVideoFrame ? json("frame") : json(p.target)},
                {"edges", std::move(edges)},
                {"offsets", json::array({p.offsets[0], p.offsets[1], p.offsets[2], p.offsets[3]})}};
}

LayerPin readPin(const Reader& r) {
    requireObject(r);
    LayerPin p;
    if (r.has("to") && r.child("to").value().is_string()) {
        if (r.child("to").asString() != "frame") {
            r.child("to").fail("a pin is to a layer id or \"frame\"");
        }
        p.target = LayerPin::kVideoFrame;
    } else {
        p.target = uint32(r, "to", 0);
    }
    const Reader edges = r.child("edges");
    const std::size_t n = edges.arraySize();
    if (n > 4) {
        edges.fail("at most the four edges");
    }
    for (std::size_t i = 0; i < n; ++i) {
        const std::string name = edges.element(i).asString();
        const auto it = std::find_if(kEdges.begin(), kEdges.end(), [&](const auto& e) { return name == e.second; });
        if (it == kEdges.end()) {
            edges.element(i).fail("an edge is left, top, right or bottom");
        }
        p.edges = static_cast<std::uint8_t>(p.edges | it->first);
    }
    if (r.has("offsets")) {
        const Reader offsets = r.child("offsets");
        if (offsets.arraySize() != 4) {
            offsets.fail("offsets are [left, top, right, bottom]");
        }
        for (std::size_t i = 0; i < 4; ++i) {
            p.offsets[i] = offsets.element(i).asDouble();
        }
    }
    return p;
}

json layerJsonImpl(const GraphicLayer& l) {
    json out{{"id", l.id}, {"kind", std::string(toString(l.kind))}};
    const GraphicLayer d;
    put(out, "name", l.name, d.name, kIdentity);
    put(out, "visible", l.visible, d.visible, kIdentity);
    if (!(l.transform == d.transform)) {
        out["transform"] = transformJson(l.transform);
    }
    put(out, "blend", l.blend, d.blend, [](BlendMode v) { return std::string(toString(v)); });
    put(out, "shadow", l.shadow, d.shadow, shadowJson);
    if (!l.masks.empty()) {
        json masks = json::array();
        for (const Mask& m : l.masks) {
            masks.push_back(maskToJson(m));
        }
        out["masks"] = std::move(masks);
    }
    if (l.in.value != 0) {
        out["in"] = l.in.value;
    }
    if (l.out) {
        out["out"] = l.out->value;
    }
    if (l.pin) {
        out["pin"] = pinJson(*l.pin);
    }
    put(out, "textStyle", l.textStyle, d.textStyle, kIdentity); // 0.2
    if (l.kind == LayerKind::Text || !(l.text == d.text)) {
        out["text"] = textJson(l.text);
    }
    if (l.kind == LayerKind::Shape || !(l.shape == d.shape)) {
        out["shape"] = shapeJson(l.shape);
    }
    if (l.kind == LayerKind::Image || !(l.image == d.image)) {
        out["image"] = imageJson(l.image);
    }
    if (!l.children.empty()) {
        json children = json::array();
        for (const GraphicLayer& c : l.children) {
            children.push_back(layerJsonImpl(c));
        }
        out["children"] = std::move(children);
    }
    return out;
}

GraphicLayer readLayerImpl(const Reader& r, std::size_t depth) {
    requireObject(r);
    if (depth > kMaxGraphicDepth) {
        r.fail("groups nest deeper than " + std::to_string(kMaxGraphicDepth) + " levels");
    }
    GraphicLayer l;
    l.id = uint32(r, "id", 0);
    l.kind = enumOf(r, "kind", l.kind, layerKindFromString);
    l.name = r.string("name");
    l.visible = r.boolean("visible", true);
    if (r.has("transform")) {
        l.transform = readTransform(r.child("transform"));
    }
    l.blend = enumOf(r, "blend", l.blend, blendModeFromString);
    if (r.has("shadow")) {
        l.shadow = readShadow(r.child("shadow"));
    }
    if (r.has("masks")) {
        const Reader masks = r.child("masks");
        const std::size_t n = masks.arraySize();
        if (n > kMaxMasks) {
            masks.fail("more than " + std::to_string(kMaxMasks) + " masks");
        }
        for (std::size_t i = 0; i < n; ++i) {
            l.masks.push_back(maskFromJson(masks.element(i)));
        }
    }
    l.in = boundedTicks(r, "in", Ticks{});
    if (r.has("out")) {
        l.out = boundedTicks(r, "out", Ticks{});
    }
    if (r.has("pin")) {
        l.pin = readPin(r.child("pin"));
    }
    l.textStyle = r.string("textStyle");
    if (r.has("text")) {
        l.text = readText(r.child("text"));
    }
    if (r.has("shape")) {
        l.shape = readShape(r.child("shape"));
    }
    if (r.has("image")) {
        l.image = readImage(r.child("image"));
    }
    if (r.has("children")) {
        const Reader children = r.child("children");
        const std::size_t n = children.arraySize();
        if (n > kMaxGraphicLayers) {
            children.fail("more than " + std::to_string(kMaxGraphicLayers) + " layers");
        }
        for (std::size_t i = 0; i < n; ++i) {
            l.children.push_back(readLayerImpl(children.element(i), depth + 1));
        }
    }
    return l;
}

// ---- Templates --------------------------------------------------------------------------------------------------------

json fieldValueJson(FieldKind kind, const FieldValue& v) {
    switch (kind) {
    case FieldKind::Text:
        return v.text;
    case FieldKind::Color:
        return colorJson(v.color);
    case FieldKind::Font: {
        json out{{"family", v.text}};
        if (!v.fontStyle.empty()) {
            out["style"] = v.fontStyle;
        }
        if (v.fontWeight != 0) {
            out["weight"] = v.fontWeight;
        }
        return out;
    }
    case FieldKind::Image:
        return v.media.value;
    case FieldKind::Duration:
        return v.duration.value;
    case FieldKind::Checkbox:
        return v.flag;
    }
    return nullptr;
}

FieldValue readFieldValue(FieldKind kind, const Reader& r) {
    FieldValue v;
    switch (kind) {
    case FieldKind::Text:
        v.text = r.asString();
        break;
    case FieldKind::Color:
        v.color = readColor(r);
        break;
    case FieldKind::Font:
        if (r.value().is_string()) {
            v.text = r.asString();
        } else {
            v.text = r.string("family");
            v.fontStyle = r.string("style");
            v.fontWeight = r.int32In("weight", 0, 0, 1000);
        }
        break;
    case FieldKind::Image:
        v.media = MediaId{r.asUInt64()};
        break;
    case FieldKind::Duration: {
        const std::int64_t t = r.asInt64();
        if (t < 0 || t >= kMaxTicks) {
            r.fail("a duration must be 0 or more and within the longest timeline");
        }
        v.duration = Ticks{t};
        break;
    }
    case FieldKind::Checkbox:
        v.flag = r.asBool();
        break;
    }
    return v;
}

json fieldJson(const TemplateField& f) {
    json out{{"id", f.id}, {"kind", std::string(toString(f.kind))}, {"property", f.property},
             {"value", fieldValueJson(f.kind, f.value)}};
    if (!f.label.empty()) {
        out["label"] = f.label;
    }
    if (f.layer != 0) {
        out["layer"] = f.layer;
    }
    return out;
}

TemplateField readField(const Reader& r) {
    TemplateField f;
    f.id = r.child("id").asString();
    f.label = r.string("label");
    f.kind = enumOf(r, "kind", f.kind, fieldKindFromString);
    f.layer = uint32(r, "layer", 0);
    f.property = r.child("property").asString();
    if (r.has("value")) {
        f.value = readFieldValue(f.kind, r.child("value"));
    }
    return f;
}

json fieldsJson(const std::vector<TemplateField>& fields) {
    json out = json::array();
    for (const TemplateField& f : fields) {
        out.push_back(fieldJson(f));
    }
    return out;
}

std::vector<TemplateField> readFields(const Reader& r) {
    std::vector<TemplateField> fields;
    const std::size_t n = r.arraySize();
    if (n > kMaxTemplateFields) {
        r.fail("more than " + std::to_string(kMaxTemplateFields) + " fields");
    }
    for (std::size_t i = 0; i < n; ++i) {
        fields.push_back(readField(r.element(i)));
    }
    return fields;
}

/// Reads untrusted text into a JSON value: bounded size and nesting, a ParseError otherwise.
std::expected<json, Error> parseBounded(std::string_view text, const char* what) {
    if (text.size() > kMaxGraphicJsonBytes) {
        return std::unexpected(Error{ErrorCode::ParseError, std::string(what) + " is larger than 16 MiB"});
    }
    if (!jsonNestingWithin(text)) {
        return std::unexpected(Error{ErrorCode::ParseError, std::string(what) + " nests too deep"});
    }
    json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded()) {
        return std::unexpected(Error{ErrorCode::ParseError, std::string(what) + " is not valid JSON (UTF-8)"});
    }
    return doc;
}

template <class T, class F>
std::expected<T, Error> readChecked(std::string_view text, const char* what, F&& read) {
    auto doc = parseBounded(text, what);
    if (!doc) {
        return std::unexpected(doc.error());
    }
    try {
        return read(Reader(*doc, ""));
    } catch (const FormatError& e) {
        return std::unexpected(Error{ErrorCode::ParseError, std::string(what) + ": " + e.message});
    } catch (const json::exception& e) {
        return std::unexpected(Error{ErrorCode::ParseError, std::string(what) + ": " + e.what()});
    }
}

std::string dumped(const json& j) { return j.dump(2, ' ', false, json::error_handler_t::replace); }

} // namespace

// ---- What ProjectIO.cpp embeds ----------------------------------------------------------------------------------------

json graphicToJsonValue(const Graphic& g) {
    json out{{"name", g.name}, {"width", g.width}, {"height", g.height}};
    json layers = json::array();
    for (const GraphicLayer& l : g.layers) {
        layers.push_back(layerJsonImpl(l));
    }
    out["layers"] = std::move(layers);
    if (g.responsive) {
        out["responsive"] = json{{"duration", g.responsive->duration.value},
                                 {"intro", g.responsive->intro.value},
                                 {"outro", g.responsive->outro.value}};
    }
    if (g.roll) {
        const GraphicRoll& roll = *g.roll;
        out["roll"] = json{{"kind", std::string(toString(roll.kind))},
                           {"startOffScreen", roll.startOffScreen},
                           {"endOffScreen", roll.endOffScreen},
                           {"preroll", roll.preroll.value},
                           {"postroll", roll.postroll.value},
                           {"easeIn", roll.easeIn.value},
                           {"easeOut", roll.easeOut.value}};
    }
    if (g.instance) {
        out["template"] = json{{"id", g.instance->templateId},
                               {"version", g.instance->version},
                               {"fields", fieldsJson(g.instance->fields)}};
    }
    return out;
}

Graphic graphicFromJsonValue(const Reader& r) {
    requireObject(r);
    Graphic g;
    g.name = r.string("name");
    g.width = r.int32In("width", g.width, 1, kMaxGraphicCanvas);
    g.height = r.int32In("height", g.height, 1, kMaxGraphicCanvas);
    if (r.has("layers")) {
        const Reader layers = r.child("layers");
        const std::size_t n = layers.arraySize();
        if (n > kMaxGraphicLayers) {
            layers.fail("more than " + std::to_string(kMaxGraphicLayers) + " layers");
        }
        for (std::size_t i = 0; i < n; ++i) {
            g.layers.push_back(readLayerImpl(layers.element(i), 1));
        }
    }
    if (r.has("responsive")) {
        const Reader rt = r.child("responsive");
        requireObject(rt);
        g.responsive = ResponsiveTime{boundedTicks(rt, "duration", Ticks{}), boundedTicks(rt, "intro", Ticks{}),
                                      boundedTicks(rt, "outro", Ticks{})};
    }
    if (r.has("roll")) {
        const Reader rr = r.child("roll");
        requireObject(rr);
        GraphicRoll roll;
        const std::string kind = rr.string("kind", "roll");
        const auto k = rollKindFromString(kind);
        if (!k) {
            rr.child("kind").fail("unknown roll kind \"" + kind + "\"");
        }
        roll.kind = *k;
        roll.startOffScreen = rr.boolean("startOffScreen", true);
        roll.endOffScreen = rr.boolean("endOffScreen", true);
        roll.preroll = boundedTicks(rr, "preroll", Ticks{});
        roll.postroll = boundedTicks(rr, "postroll", Ticks{});
        roll.easeIn = boundedTicks(rr, "easeIn", Ticks{});
        roll.easeOut = boundedTicks(rr, "easeOut", Ticks{});
        g.roll = roll;
    }
    if (r.has("template")) {
        const Reader t = r.child("template");
        TemplateInstance instance;
        instance.templateId = t.child("id").asString();
        instance.version = t.int32In("version", 1, 1, 1'000'000);
        if (t.has("fields")) {
            instance.fields = readFields(t.child("fields"));
        }
        g.instance = std::move(instance);
    }
    if (const auto problem = graphicProblem(g)) {
        r.fail("graphic: " + *problem);
    }
    return g;
}

json brandKitToJsonValue(const BrandKit& kit) {
    json colors = json::array();
    for (const BrandColor& c : kit.colors) {
        colors.push_back(json{{"name", c.name}, {"rgba", json::array({c.rgba[0], c.rgba[1], c.rgba[2], c.rgba[3]})}});
    }
    json fonts = json::array();
    for (const BrandFont& f : kit.fonts) {
        json font{{"role", f.role}, {"family", f.family}};
        if (!f.style.empty()) {
            font["style"] = f.style;
        }
        if (f.weight != 400) {
            font["weight"] = f.weight;
        }
        fonts.push_back(std::move(font));
    }
    json logos = json::array();
    for (const BrandLogo& l : kit.logos) {
        logos.push_back(json{{"name", l.name}, {"media", l.media.value}});
    }
    return json{{"name", kit.name}, {"colors", std::move(colors)}, {"fonts", std::move(fonts)}, {"logos", std::move(logos)}};
}

BrandKit brandKitFromJsonValue(const Reader& r) {
    requireObject(r);
    BrandKit kit;
    kit.name = r.string("name");
    if (r.has("colors")) {
        const Reader list = r.child("colors");
        const std::size_t n = list.arraySize();
        if (n > kMaxBrandColors) {
            list.fail("more than " + std::to_string(kMaxBrandColors) + " colours");
        }
        for (std::size_t i = 0; i < n; ++i) {
            const Reader c = list.element(i);
            kit.colors.push_back(BrandColor{c.child("name").asString(), readRgba(c.child(c.has("rgba") ? "rgba" : "color"))});
        }
    }
    if (r.has("fonts")) {
        const Reader list = r.child("fonts");
        const std::size_t n = list.arraySize();
        if (n > kMaxBrandFonts) {
            list.fail("more than " + std::to_string(kMaxBrandFonts) + " fonts");
        }
        for (std::size_t i = 0; i < n; ++i) {
            const Reader f = list.element(i);
            kit.fonts.push_back(BrandFont{f.child("role").asString(), f.child("family").asString(), f.string("style"),
                                          f.int32In("weight", 400, 1, 1000)});
        }
    }
    if (r.has("logos")) {
        const Reader list = r.child("logos");
        const std::size_t n = list.arraySize();
        if (n > kMaxBrandLogos) {
            list.fail("more than " + std::to_string(kMaxBrandLogos) + " logos");
        }
        for (std::size_t i = 0; i < n; ++i) {
            const Reader l = list.element(i);
            kit.logos.push_back(BrandLogo{l.child("name").asString(), MediaId{l.uint64("media", 0)}});
        }
    }
    if (const auto problem = brandKitProblem(kit)) {
        r.fail("brand kit: " + *problem);
    }
    return kit;
}

json textStyleToJsonValue(const ProjectTextStyle& s) {
    // As a text layer holds it: one run (no text) with the style, the alignment and the background; and the shadow.
    TextContent text;
    text.runs = {TextRun{"", s.style}};
    text.align = s.align;
    text.background = s.background;
    json t = textJson(text);
    t["runs"][0].erase("text");
    json out{{"name", s.name}, {"text", std::move(t)}};
    put(out, "shadow", s.shadow, DropShadow{}, shadowJson);
    return out;
}

ProjectTextStyle textStyleFromJsonValue(const Reader& r) {
    requireObject(r);
    ProjectTextStyle s;
    s.name = r.child("name").asString();
    if (r.has("text")) {
        const TextContent text = readText(r.child("text"));
        if (text.runs.size() != 1) {
            r.child("text").fail("a text style has one run");
        }
        s.style = text.runs.front().style;
        s.align = text.align;
        s.background = text.background;
    }
    if (r.has("shadow")) {
        s.shadow = readShadow(r.child("shadow"));
    }
    if (const auto problem = textStyleProblem(s)) {
        r.fail(*problem);
    }
    return s;
}

json templateToJsonValue(const GraphicTemplate& t) {
    json out{{"id", t.id}, {"name", t.name}, {"version", t.version}, {"graphic", graphicToJsonValue(t.graphic)},
             {"fields", fieldsJson(t.fields)}};
    if (!t.description.empty()) {
        out["description"] = t.description;
    }
    if (t.duration.value != 0) {
        out["duration"] = t.duration.value;
    }
    return out;
}

GraphicTemplate templateFromJsonValue(const Reader& r) {
    requireObject(r);
    GraphicTemplate t;
    t.id = r.child("id").asString();
    t.name = r.child("name").asString();
    t.description = r.string("description");
    t.version = r.int32In("version", 1, 1, 1'000'000);
    t.graphic = graphicFromJsonValue(r.child("graphic"));
    if (r.has("fields")) {
        t.fields = readFields(r.child("fields"));
    }
    t.duration = boundedTicks(r, "duration", Ticks{});
    if (const auto problem = templateProblem(t)) {
        r.fail(*problem);
    }
    return t;
}

} // namespace detail

// ---- Public text API --------------------------------------------------------------------------------------------------

std::string graphicToJson(const Graphic& graphic) { return detail::dumped(detail::graphicToJsonValue(graphic)); }

std::expected<Graphic, Error> graphicFromJson(std::string_view text) {
    return detail::readChecked<Graphic>(text, "graphic", [](const detail::Reader& r) { return detail::graphicFromJsonValue(r); });
}

std::string layerToJson(const GraphicLayer& layer) { return detail::dumped(detail::layerJsonImpl(layer)); }

std::expected<GraphicLayer, Error> layerFromJson(std::string_view text, std::int32_t width, std::int32_t height) {
    return detail::readChecked<GraphicLayer>(text, "layer", [&](const detail::Reader& r) {
        GraphicLayer layer = detail::readLayerImpl(r, 1);
        Graphic check;
        check.width = std::clamp(width, 1, kMaxGraphicCanvas);
        check.height = std::clamp(height, 1, kMaxGraphicCanvas);
        GraphicLayer probe = layer;
        if (probe.id == 0) {
            probe.id = 1; // ids are given by the graphic the layer joins
        }
        probe.pin.reset(); // pins are checked against the siblings it gets
        check.layers.push_back(std::move(probe));
        if (const auto problem = graphicProblem(check)) {
            r.fail(*problem);
        }
        return layer;
    });
}

std::string fieldValueToJson(FieldKind kind, const FieldValue& value) {
    return detail::fieldValueJson(kind, value).dump(-1, ' ', false, detail::json::error_handler_t::replace);
}

std::expected<FieldValue, Error> fieldValueFromJson(FieldKind kind, std::string_view text) {
    return detail::readChecked<FieldValue>(text, "field value", [&](const detail::Reader& r) {
        TemplateField probe; // checked the way a template's field is (fieldProblem): text and colour bounds
        probe.id = "value";
        probe.kind = FieldKind::Duration;
        probe.property = "clip";
        probe.value = detail::readFieldValue(kind, r);
        GraphicTemplate check;
        check.id = "value";
        check.name = "value";
        check.fields = {probe};
        if (const auto problem = templateProblem(check)) {
            r.fail(*problem);
        }
        return probe.value;
    });
}

std::string brandKitToJson(const BrandKit& kit) { return detail::dumped(detail::brandKitToJsonValue(kit)); }

std::string textStyleToJson(const ProjectTextStyle& style) { return detail::dumped(detail::textStyleToJsonValue(style)); }

std::expected<ProjectTextStyle, Error> textStyleFromJson(std::string_view text) {
    return detail::readChecked<ProjectTextStyle>(text, "text style",
                                                 [](const detail::Reader& r) { return detail::textStyleFromJsonValue(r); });
}

std::expected<BrandKit, Error> brandKitFromJson(std::string_view text) {
    return detail::readChecked<BrandKit>(text, "brand kit", [](const detail::Reader& r) { return detail::brandKitFromJsonValue(r); });
}

std::string templateFileToJson(const GraphicTemplate& tmpl) {
    return detail::dumped(detail::json{{"format", std::string(kTemplateFileFormat)},
                                       {"version", kTemplateFileVersion},
                                       {"template", detail::templateToJsonValue(tmpl)}});
}

std::expected<GraphicTemplate, Error> templateFileFromJson(std::string_view text) {
    auto doc = detail::parseBounded(text, "template file");
    if (!doc) {
        return std::unexpected(doc.error());
    }
    const detail::json& file = *doc;
    if (!file.is_object() || !file.contains("format") || !file.at("format").is_string() ||
        file.at("format").get<std::string>() != kTemplateFileFormat) {
        return std::unexpected(Error{ErrorCode::ParseError, "not a Wabcut graphic template file"});
    }
    if (!file.contains("version") || !file.at("version").is_number_integer() || file.at("version").get<std::int64_t>() < 1) {
        return std::unexpected(Error{ErrorCode::ParseError, "template file: a bad version"});
    }
    if (file.at("version").get<std::int64_t>() > kTemplateFileVersion) {
        return std::unexpected(Error{ErrorCode::VersionMismatch, "the template was saved by a newer Wabcut"});
    }
    try {
        return detail::templateFromJsonValue(detail::Reader(file, "").child("template"));
    } catch (const detail::FormatError& e) {
        return std::unexpected(Error{ErrorCode::ParseError, "template file: " + e.message});
    } catch (const detail::json::exception& e) {
        return std::unexpected(Error{ErrorCode::ParseError, std::string("template file: ") + e.what()});
    }
}

std::vector<LibraryTemplate> readTemplateLibrary(const std::filesystem::path& folder) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end && files.size() < kMaxLibraryTemplates;
         it.increment(ec)) {
        const std::string name = pathToUtf8(it->path().filename());
        if (it->symlink_status(ec).type() == std::filesystem::file_type::regular && name.ends_with(kTemplateFileSuffix)) {
            files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());
    std::vector<LibraryTemplate> out;
    for (const std::filesystem::path& file : files) {
        const std::uintmax_t size = std::filesystem::file_size(file, ec);
        if (ec || size > kMaxGraphicJsonBytes) {
            out.push_back({file, std::unexpected(Error{ErrorCode::ParseError, ec ? "cannot be read" : "larger than 16 MiB"})});
            ec.clear();
            continue;
        }
        // Read at most the size seen: a file that grows meanwhile is cut off (and refused by the parser), never over-read.
        std::ifstream in(file, std::ios::binary);
        std::string text(static_cast<std::size_t>(size), '\0');
        in.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<std::size_t>(in.gcount()));
        out.push_back({file, templateFileFromJson(text)});
    }
    return out;
}

std::expected<std::filesystem::path, Error> writeTemplateToLibrary(const std::filesystem::path& folder,
                                                                  const GraphicTemplate& tmpl) {
    if (const auto problem = templateProblem(tmpl)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, *problem});
    }
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::filesystem::path file = folder / pathFromUtf8(tmpl.id + std::string(kTemplateFileSuffix));
    if (auto written = writeFileAtomically(file, templateFileToJson(tmpl)); !written) {
        return std::unexpected(written.error());
    }
    return file;
}

} // namespace wabcut::io
