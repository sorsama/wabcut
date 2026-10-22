// Motion graphics model (C.13): names, lookups, checks and grapheme clusters.

#include <wabcut/model/Graphics.h>

#include <wabcut/model/TitleOps.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <utility>

namespace wabcut {

namespace {

template <class E, std::size_t N>
struct Names {
    std::array<std::pair<E, std::string_view>, N> items;

    std::string_view name(E value) const {
        for (const auto& [v, n] : items) {
            if (v == value) {
                return n;
            }
        }
        return items[0].second;
    }
    std::optional<E> parse(std::string_view text) const {
        for (const auto& [v, n] : items) {
            if (n == text) {
                return v;
            }
        }
        return std::nullopt;
    }
};

constexpr Names<LayerKind, 4> kLayerKinds{{{{LayerKind::Text, "text"}, {LayerKind::Shape, "shape"},
                                            {LayerKind::Image, "image"}, {LayerKind::Group, "group"}}}};
constexpr Names<ShapeKind, 5> kShapeKinds{{{{ShapeKind::Rectangle, "rectangle"}, {ShapeKind::Ellipse, "ellipse"},
                                            {ShapeKind::Polygon, "polygon"}, {ShapeKind::Star, "star"},
                                            {ShapeKind::Path, "path"}}}};
constexpr Names<PaintKind, 4> kPaintKinds{{{{PaintKind::None, "none"}, {PaintKind::Solid, "solid"},
                                            {PaintKind::Linear, "linear"}, {PaintKind::Radial, "radial"}}}};
constexpr Names<TextAlign, 4> kTextAligns{{{{TextAlign::Left, "left"}, {TextAlign::Center, "center"},
                                            {TextAlign::Right, "right"}, {TextAlign::Justify, "justify"}}}};
constexpr Names<TextUnit, 3> kTextUnits{{{{TextUnit::Character, "character"}, {TextUnit::Word, "word"},
                                          {TextUnit::Line, "line"}}}};
constexpr Names<TextSelectorShape, 2> kSelectorShapes{{{{TextSelectorShape::Square, "square"},
                                                        {TextSelectorShape::Ramp, "ramp"}}}};
constexpr Names<ImageFit, 4> kImageFits{{{{ImageFit::Contain, "contain"}, {ImageFit::Cover, "cover"},
                                          {ImageFit::Fill, "fill"}, {ImageFit::None, "none"}}}};
constexpr Names<LineJoin, 3> kLineJoins{{{{LineJoin::Miter, "miter"}, {LineJoin::Round, "round"},
                                          {LineJoin::Bevel, "bevel"}}}};
constexpr Names<LineCap, 3> kLineCaps{{{{LineCap::Butt, "butt"}, {LineCap::Round, "round"},
                                        {LineCap::Square, "square"}}}};
constexpr Names<FieldKind, 6> kFieldKinds{{{{FieldKind::Text, "text"}, {FieldKind::Color, "color"},
                                            {FieldKind::Font, "font"}, {FieldKind::Image, "image"},
                                            {FieldKind::Duration, "duration"}, {FieldKind::Checkbox, "checkbox"}}}};
constexpr Names<RollKind, 3> kRollKinds{{{{RollKind::Roll, "roll"}, {RollKind::CrawlLeft, "crawlLeft"},
                                          {RollKind::CrawlRight, "crawlRight"}}}};

constexpr std::int64_t kMaxTicks = std::int64_t{1} << 60;

bool within(double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; }

bool isText(std::string_view s, std::size_t maxBytes) { return s.size() <= maxBytes && edit::isValidUtf8(s); }

/// Collects the first problem, with where it is.
class Checker {
public:
    bool ok() const { return !m_problem; }
    std::optional<std::string> take() { return std::move(m_problem); }
    void fail(const std::string& where, const std::string& what) {
        if (!m_problem) {
            m_problem = where.empty() ? what : where + ": " + what;
        }
    }
    void require(bool condition, const std::string& where, const std::string& what) {
        if (!condition) {
            fail(where, what);
        }
    }
    void param(const ParamValue& v, double lo, double hi, const std::string& where) {
        if (const Curve* c = v.curve(); c && c->keys.size() > kMaxPathKeys) {
            fail(where, "has more than " + std::to_string(kMaxPathKeys) + " keys");
            return;
        }
        if (!v.isValid(lo, hi)) {
            fail(where, "must stay within " + format(lo) + ".." + format(hi) + " (and be a well-formed curve)");
            return;
        }
        if (const Curve* c = v.curve()) {
            for (const Keyframe& k : c->keys) {
                if (std::llabs(k.time.value) >= kMaxTicks) {
                    fail(where, "has a key past the longest timeline");
                    return;
                }
            }
        }
    }
    void color(const GraphicColor& c, const std::string& where) {
        for (const double v : c.rgba) {
            if (!within(v, 0.0, 1.0)) {
                fail(where, "colour components must be 0..1");
                return;
            }
        }
        require(isText(c.brand, kMaxGraphicNameBytes), where, "brand colour name too long or not UTF-8");
    }

private:
    static std::string format(double v) {
        std::string s = std::to_string(v);
        s.erase(s.find_last_not_of('0') + 1);
        if (!s.empty() && s.back() == '.') {
            s.pop_back();
        }
        return s;
    }
    std::optional<std::string> m_problem;
};

void checkPaint(Checker& c, const Paint& p, const std::string& where) {
    c.color(p.color, where + ".color");
    for (const double v : {p.start[0], p.start[1], p.end[0], p.end[1]}) {
        c.require(within(v, -kMaxGraphicCoordinate, kMaxGraphicCoordinate), where, "gradient points must be finite");
    }
    if (p.kind == PaintKind::Linear || p.kind == PaintKind::Radial) {
        c.require(p.stops.size() >= 2 && p.stops.size() <= kMaxGradientStops, where,
                  "a gradient needs 2 to " + std::to_string(kMaxGradientStops) + " stops");
    } else {
        c.require(p.stops.size() <= kMaxGradientStops, where, "too many gradient stops");
    }
    double last = 0.0;
    for (const GradientStop& s : p.stops) {
        c.require(within(s.offset, 0.0, 1.0) && s.offset >= last, where, "stop offsets must be 0..1, non-decreasing");
        last = s.offset;
        c.color(s.color, where + ".stops");
    }
}

void checkPath(Checker& c, const std::vector<MaskPathKey>& path, bool closed, const std::string& where) {
    if (path.empty()) {
        c.fail(where, "a path shape needs at least one key");
        return;
    }
    c.require(path.size() <= kMaxPathKeys, where, "too many path keys");
    const std::size_t count = path.front().vertices.size();
    c.require(count >= (closed ? 3u : 2u) && count <= kMaxPathVertices, where,
              "a path needs " + std::string(closed ? "3" : "2") + " to " + std::to_string(kMaxPathVertices) + " vertices");
    for (std::size_t i = 0; i < path.size(); ++i) {
        const MaskPathKey& key = path[i];
        c.require(key.vertices.size() == count, where, "every path key needs the same vertex count");
        c.require(std::llabs(key.time.value) < kMaxTicks && (i == 0 || key.time > path[i - 1].time), where,
                  "path key times must increase");
        for (const MaskVertex& v : key.vertices) {
            for (const double x : {v.x, v.y, v.inX, v.inY, v.outX, v.outY}) {
                if (!within(x, -kMaxGraphicCoordinate, kMaxGraphicCoordinate)) {
                    c.fail(where, "path coordinates must be finite and within a million pixels");
                    return;
                }
            }
        }
        if (!c.ok()) {
            return;
        }
    }
}

void checkTextStyle(Checker& c, const TextStyle& s, const std::string& where) {
    c.require(isText(s.fontFamily, kMaxFontBytes) && isText(s.fontStyle, kMaxFontBytes) &&
                  isText(s.brandFont, kMaxFontBytes),
              where, "font names must be UTF-8 of at most " + std::to_string(kMaxFontBytes) + " bytes");
    c.require(s.fontWeight >= 100 && s.fontWeight <= 900, where, "font weight must be 100..900");
    c.require(within(s.fontSize, 1.0, 2000.0), where, "font size must be 1..2000 pixels");
    c.require(within(s.tracking, -1000.0, 10000.0), where, "tracking must be -1000..10000");
    c.require(within(s.leading, 0.0, 10000.0), where, "leading must be 0..10000 pixels");
    c.require(within(s.baselineShift, -10000.0, 10000.0), where, "baseline shift must be within 10000 pixels");
    c.color(s.fill, where + ".fill");
}

bool isLanguageTag(std::string_view s) {
    return s.size() <= 16 && std::all_of(s.begin(), s.end(), [](char ch) {
               return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '-';
           });
}

void checkText(Checker& c, const TextContent& t, const std::string& where) {
    c.require(t.runs.size() <= kMaxTextRuns, where, "more than " + std::to_string(kMaxTextRuns) + " text runs");
    std::size_t bytes = 0;
    for (std::size_t i = 0; i < t.runs.size() && c.ok(); ++i) {
        const TextRun& r = t.runs[i];
        bytes += r.text.size();
        c.require(edit::isValidUtf8(r.text) && r.text.find('\0') == std::string::npos, where, "text must be UTF-8 without NUL");
        checkTextStyle(c, r.style, where + ".runs[" + std::to_string(i) + "]");
    }
    c.require(bytes <= kMaxGraphicTextBytes, where, "more than " + std::to_string(kMaxGraphicTextBytes / 1024) + " KiB of text");
    c.require(within(t.boxWidth, 0.0, kMaxGraphicCoordinate) && within(t.boxHeight, 0.0, kMaxGraphicCoordinate), where,
              "the text box must be 0..1e6 pixels");
    c.require(isLanguageTag(t.language), where, "language must be a language tag such as \"th\"");
    c.color(t.background.color, where + ".background");
    c.require(within(t.background.padding, 0.0, 10000.0) && within(t.background.radius, 0.0, 10000.0), where,
              "background padding and radius must be 0..10000 pixels");
    c.require(t.animators.size() <= kMaxTextAnimators, where,
              "more than " + std::to_string(kMaxTextAnimators) + " text animators");
    for (std::size_t i = 0; i < t.animators.size() && c.ok(); ++i) {
        const TextAnimator& a = t.animators[i];
        const std::string at = where + ".animators[" + std::to_string(i) + "]";
        c.require(isText(a.name, kMaxGraphicNameBytes), at, "name too long or not UTF-8");
        c.param(a.start, -100.0, 200.0, at + ".start");
        c.param(a.end, -100.0, 200.0, at + ".end");
        c.param(a.offset, -200.0, 200.0, at + ".offset");
        c.require(within(a.softness, 0.0, 1000.0), at, "softness must be 0..1000 units");
        c.require(within(a.opacity, 0.0, 100.0), at, "opacity must be 0..100");
        c.require(within(a.offsetX, -kMaxGraphicCoordinate, kMaxGraphicCoordinate) &&
                      within(a.offsetY, -kMaxGraphicCoordinate, kMaxGraphicCoordinate),
                  at, "offsets must be within a million pixels");
        c.require(within(a.scale, -100000.0, 100000.0), at, "scale must be within 100000 %");
        c.require(within(a.rotation, -1e6, 1e6), at, "rotation must be finite");
    }
    if (t.path && c.ok()) {
        checkPath(c, t.path->path, t.path->closed, where + ".onPath.path");
        c.param(t.path->firstMargin, -kMaxGraphicCoordinate, kMaxGraphicCoordinate, where + ".onPath.firstMargin");
        c.param(t.path->lastMargin, -kMaxGraphicCoordinate, kMaxGraphicCoordinate, where + ".onPath.lastMargin");
    }
}

void checkShape(Checker& c, const ShapeContent& s, const std::string& where) {
    c.param(s.width, 0.0, kMaxGraphicCoordinate, where + ".width");
    c.param(s.height, 0.0, kMaxGraphicCoordinate, where + ".height");
    c.param(s.cornerRadius, 0.0, kMaxGraphicCoordinate, where + ".cornerRadius");
    c.require(s.points >= 3 && s.points <= kMaxShapePoints, where, "points must be 3..64");
    c.param(s.outerRadius, 0.0, kMaxGraphicCoordinate, where + ".outerRadius");
    c.param(s.innerRadius, 0.0, kMaxGraphicCoordinate, where + ".innerRadius");
    if (s.kind == ShapeKind::Path) {
        checkPath(c, s.path, s.closed, where + ".path");
    } else {
        c.require(s.path.empty(), where, "only a path shape has path keys");
    }
    checkPaint(c, s.fill, where + ".fill");
    c.color(s.stroke.color, where + ".stroke");
    c.param(s.stroke.width, 0.0, 1000.0, where + ".stroke.width");
    c.require(s.stroke.dashes.size() <= kMaxDashes, where, "more than 16 dash lengths");
    for (const double d : s.stroke.dashes) {
        c.require(within(d, 0.0, 10000.0), where, "dash lengths must be 0..10000 pixels");
    }
    c.param(s.trimStart, 0.0, 100.0, where + ".trimStart");
    c.param(s.trimEnd, 0.0, 100.0, where + ".trimEnd");
    c.param(s.trimOffset, -1e6, 1e6, where + ".trimOffset");
}

void checkTransform(Checker& c, const LayerTransform& t, const std::string& where) {
    c.param(t.x, -kMaxGraphicCoordinate, kMaxGraphicCoordinate, where + ".x");
    c.param(t.y, -kMaxGraphicCoordinate, kMaxGraphicCoordinate, where + ".y");
    c.param(t.anchorX, -kMaxGraphicCoordinate, kMaxGraphicCoordinate, where + ".anchorX");
    c.param(t.anchorY, -kMaxGraphicCoordinate, kMaxGraphicCoordinate, where + ".anchorY");
    c.param(t.scaleX, -100000.0, 100000.0, where + ".scaleX");
    c.param(t.scaleY, -100000.0, 100000.0, where + ".scaleY");
    c.param(t.rotation, -1e6, 1e6, where + ".rotation");
    c.param(t.opacity, 0.0, 100.0, where + ".opacity");
}

struct Walk {
    Checker& c;
    std::set<std::uint32_t> ids;
    std::size_t count = 0;
};

void checkLayers(Walk& w, const std::vector<GraphicLayer>& layers, std::size_t depth, const std::string& where) {
    Checker& c = w.c;
    if (depth > kMaxGraphicDepth) {
        c.fail(where, "groups nest deeper than " + std::to_string(kMaxGraphicDepth) + " levels");
        return;
    }
    for (std::size_t i = 0; i < layers.size() && c.ok(); ++i) {
        const GraphicLayer& l = layers[i];
        const std::string at = where + "[" + std::to_string(i) + "]";
        if (++w.count > kMaxGraphicLayers) {
            c.fail("", "more than " + std::to_string(kMaxGraphicLayers) + " layers");
            return;
        }
        c.require(l.id != 0 && w.ids.insert(l.id).second, at, "layer ids must be unique and not 0");
        c.require(isText(l.name, kMaxGraphicNameBytes), at, "name too long or not UTF-8");
        checkTransform(c, l.transform, at + ".transform");
        c.color(l.shadow.color, at + ".shadow");
        c.require(within(l.shadow.opacity, 0.0, 1.0) && within(l.shadow.angle, -1e6, 1e6) &&
                      within(l.shadow.distance, 0.0, 10000.0) && within(l.shadow.softness, 0.0, 10000.0),
                  at, "shadow opacity 0..1, distance and softness 0..10000 pixels");
        if (auto problem = masksProblem(l.masks)) {
            c.fail(at + ".masks", *problem);
        }
        c.require(l.in.value >= 0 && l.in.value < kMaxTicks && (!l.out || (l.out->value > l.in.value && l.out->value < kMaxTicks)),
                  at, "in must be >= 0 and out after in");
        c.require(isText(l.textStyle, kMaxGraphicNameBytes) && (l.textStyle.empty() || l.kind == LayerKind::Text), at,
                  "only a text layer follows a text style (a name of at most 256 bytes)");
        if (l.kind == LayerKind::Text) {
            checkText(c, l.text, at + ".text");
        } else {
            c.require(l.text.runs.empty() && l.text.animators.empty() && !l.text.path, at, "only a text layer has text");
        }
        if (l.kind == LayerKind::Shape) {
            checkShape(c, l.shape, at + ".shape");
        }
        if (l.kind == LayerKind::Image) {
            c.require(within(l.image.width, 0.0, kMaxGraphicCoordinate) && within(l.image.height, 0.0, kMaxGraphicCoordinate),
                      at, "the image box must be 0..1e6 pixels");
            c.require(isText(l.image.brandLogo, kMaxGraphicNameBytes), at, "brand logo name too long or not UTF-8");
        }
        if (l.kind == LayerKind::Group) {
            checkLayers(w, l.children, depth + 1, at + ".children");
        } else {
            c.require(l.children.empty(), at, "only a group has children");
        }
        if (l.pin) {
            // 0.2: a top-level layer may be pinned to the video frame (LayerPin::kVideoFrame).
            c.require(l.pin->target != l.id && (l.pin->target != LayerPin::kVideoFrame || depth == 1), at,
                      "a pin needs another layer (or, at the top level, the video frame)");
            c.require(l.pin->edges != 0 && (l.pin->edges & ~0x0Fu) == 0, at, "a pin needs edges");
            for (const double o : l.pin->offsets) {
                c.require(within(o, -kMaxGraphicCoordinate, kMaxGraphicCoordinate), at, "pin offsets must be finite");
            }
        }
    }
    // Pins: to a sibling, no cycles.
    for (const GraphicLayer& l : layers) {
        if (!c.ok() || !l.pin || l.pin->target == LayerPin::kVideoFrame) {
            continue;
        }
        std::uint32_t at = l.id;
        for (std::size_t hops = 0; hops <= layers.size(); ++hops) {
            const auto it = std::find_if(layers.begin(), layers.end(), [&](const GraphicLayer& s) { return s.id == at; });
            if (it == layers.end() || !it->pin) {
                break;
            }
            const std::uint32_t next = it->pin->target;
            if (next == LayerPin::kVideoFrame) {
                break; // the chain ends at the frame
            }
            if (std::none_of(layers.begin(), layers.end(), [&](const GraphicLayer& s) { return s.id == next; })) {
                c.fail("layer " + std::to_string(l.id), "is pinned to layer " + std::to_string(next) + ", which is not a sibling");
                break;
            }
            if (next == l.id) {
                c.fail("layer " + std::to_string(l.id), "pins form a cycle");
                break;
            }
            at = next;
        }
    }
}

std::optional<std::string> fieldProblem(const TemplateField& f, const Graphic& g) {
    if (!isValidFieldId(f.id)) {
        return "field id \"" + f.id.substr(0, 64) + "\" must match [A-Za-z][A-Za-z0-9_.-]* (64 bytes at most)";
    }
    const std::string where = "field " + f.id + ": ";
    if (!isText(f.label, kMaxGraphicNameBytes) || !isText(f.property, 32)) {
        return where + "label or property too long or not UTF-8";
    }
    if (!isText(f.value.text, kMaxGraphicTextBytes) || !isText(f.value.fontStyle, kMaxFontBytes) ||
        (f.value.fontWeight != 0 && (f.value.fontWeight < 100 || f.value.fontWeight > 900)) ||
        f.value.duration.value < 0 || f.value.duration.value >= kMaxTicks) {
        return where + "its value is out of range";
    }
    Checker c;
    c.color(f.value.color, where);
    if (auto problem = c.take()) {
        return problem;
    }
    if (f.kind == FieldKind::Duration && f.property == "clip") {
        return std::nullopt; // no layer
    }
    const GraphicLayer* layer = g.findLayer(f.layer);
    if (!layer) {
        return where + "layer " + std::to_string(f.layer) + " does not exist";
    }
    const LayerKind k = layer->kind;
    const std::string& p = f.property;
    bool fits = false;
    switch (f.kind) {
    case FieldKind::Text:
        fits = k == LayerKind::Text && p == "text";
        break;
    case FieldKind::Color:
        fits = (p == "fill" && (k == LayerKind::Text || k == LayerKind::Shape)) || (p == "stroke" && k == LayerKind::Shape) ||
               (p == "background" && k == LayerKind::Text) || p == "shadow";
        break;
    case FieldKind::Font:
        fits = k == LayerKind::Text && p == "font";
        break;
    case FieldKind::Image:
        fits = k == LayerKind::Image && p == "media";
        break;
    case FieldKind::Duration:
        fits = p == "out";
        break;
    case FieldKind::Checkbox:
        fits = p == "visible";
        break;
    }
    if (!fits) {
        return where + "a " + std::string(toString(f.kind)) + " field cannot edit \"" + p + "\" of a " +
               std::string(toString(k)) + " layer";
    }
    return std::nullopt;
}

std::optional<std::string> fieldsProblem(const std::vector<TemplateField>& fields, const Graphic& g) {
    if (fields.size() > kMaxTemplateFields) {
        return "more than " + std::to_string(kMaxTemplateFields) + " template fields";
    }
    std::set<std::string> ids;
    for (const TemplateField& f : fields) {
        if (auto problem = fieldProblem(f, g)) {
            return problem;
        }
        if (!ids.insert(f.id).second) {
            return "field id \"" + f.id + "\" is used twice";
        }
    }
    return std::nullopt;
}

/// The cluster-extending code points (UAX #29's Extend and SpacingMark, for the scripts editors meet most).
bool extendsCluster(char32_t cp) {
    struct Range {
        char32_t lo;
        char32_t hi;
    };
    static constexpr Range kExtend[] = {
        {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF}, {0x05C1, 0x05C2}, {0x05C4, 0x05C5},
        {0x05C7, 0x05C7}, {0x0610, 0x061A}, {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x06DF, 0x06E4},
        {0x06E7, 0x06E8}, {0x06EA, 0x06ED}, {0x0900, 0x0903}, {0x093A, 0x093C}, {0x093E, 0x094F}, {0x0951, 0x0957},
        {0x0962, 0x0963}, {0x0981, 0x0983}, {0x09BC, 0x09BC}, {0x09BE, 0x09CD}, {0x0E31, 0x0E31}, {0x0E33, 0x0E3A},
        {0x0E47, 0x0E4E}, {0x0EB1, 0x0EB1}, {0x0EB3, 0x0EBC}, {0x0EC8, 0x0ECE}, {0x102B, 0x103E}, {0x17B4, 0x17D3},
        {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x200C, 0x200C}, {0x20D0, 0x20FF}, {0x302A, 0x302F}, {0x3099, 0x309A},
        {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0x1F3FB, 0x1F3FF}, {0xE0020, 0xE007F}, {0xE0100, 0xE01EF},
    };
    for (const Range& r : kExtend) {
        if (cp >= r.lo && cp <= r.hi) {
            return true;
        }
    }
    return false;
}

/// Decodes the code point at `i` (advancing it); an invalid byte decodes as itself, one byte long.
char32_t decode(std::string_view s, std::size_t& i) {
    const auto b = static_cast<unsigned char>(s[i]);
    const int extra = b < 0x80 ? 0 : (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : (b & 0xF8) == 0xF0 ? 3 : -1;
    if (extra < 0 || (extra > 0 && i + static_cast<std::size_t>(extra) >= s.size())) {
        ++i;
        return b;
    }
    char32_t cp = extra == 0 ? b : extra == 1 ? (b & 0x1Fu) : extra == 2 ? (b & 0x0Fu) : (b & 0x07u);
    for (int k = 1; k <= extra; ++k) {
        const auto c = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
        if ((c & 0xC0) != 0x80) {
            ++i;
            return b;
        }
        cp = (cp << 6) | (c & 0x3Fu);
    }
    i += static_cast<std::size_t>(extra) + 1;
    return cp;
}

bool isRegionalIndicator(char32_t cp) { return cp >= 0x1F1E6 && cp <= 0x1F1FF; }

} // namespace

// ---- Names ------------------------------------------------------------------------------------------------------------

std::string_view toString(LayerKind v) { return kLayerKinds.name(v); }
std::optional<LayerKind> layerKindFromString(std::string_view t) { return kLayerKinds.parse(t); }
std::string_view toString(ShapeKind v) { return kShapeKinds.name(v); }
std::optional<ShapeKind> shapeKindFromString(std::string_view t) { return kShapeKinds.parse(t); }
std::string_view toString(PaintKind v) { return kPaintKinds.name(v); }
std::optional<PaintKind> paintKindFromString(std::string_view t) { return kPaintKinds.parse(t); }
std::string_view toString(TextAlign v) { return kTextAligns.name(v); }
std::optional<TextAlign> textAlignFromString(std::string_view t) { return kTextAligns.parse(t); }
std::string_view toString(TextUnit v) { return kTextUnits.name(v); }
std::optional<TextUnit> textUnitFromString(std::string_view t) { return kTextUnits.parse(t); }
std::string_view toString(TextSelectorShape v) { return kSelectorShapes.name(v); }
std::optional<TextSelectorShape> textSelectorShapeFromString(std::string_view t) { return kSelectorShapes.parse(t); }
std::string_view toString(ImageFit v) { return kImageFits.name(v); }
std::optional<ImageFit> imageFitFromString(std::string_view t) { return kImageFits.parse(t); }
std::string_view toString(LineJoin v) { return kLineJoins.name(v); }
std::optional<LineJoin> lineJoinFromString(std::string_view t) { return kLineJoins.parse(t); }
std::string_view toString(LineCap v) { return kLineCaps.name(v); }
std::optional<LineCap> lineCapFromString(std::string_view t) { return kLineCaps.parse(t); }
std::string_view toString(FieldKind v) { return kFieldKinds.name(v); }
std::optional<FieldKind> fieldKindFromString(std::string_view t) { return kFieldKinds.parse(t); }
std::string_view toString(RollKind v) { return kRollKinds.name(v); }
std::optional<RollKind> rollKindFromString(std::string_view t) { return kRollKinds.parse(t); }

// ---- Lookups ----------------------------------------------------------------------------------------------------------

std::string TextContent::text() const {
    std::string out;
    for (const TextRun& r : runs) {
        out += r.text;
    }
    return out;
}

namespace {

const GraphicLayer* findIn(const std::vector<GraphicLayer>& layers, std::uint32_t id) {
    for (const GraphicLayer& l : layers) {
        if (l.id == id) {
            return &l;
        }
        if (const GraphicLayer* found = findIn(l.children, id)) {
            return found;
        }
    }
    return nullptr;
}

std::size_t countIn(const std::vector<GraphicLayer>& layers) {
    std::size_t n = layers.size();
    for (const GraphicLayer& l : layers) {
        n += countIn(l.children);
    }
    return n;
}

std::uint32_t maxIdIn(const std::vector<GraphicLayer>& layers) {
    std::uint32_t m = 0;
    for (const GraphicLayer& l : layers) {
        m = std::max({m, l.id, maxIdIn(l.children)});
    }
    return m;
}

} // namespace

const GraphicLayer* Graphic::findLayer(std::uint32_t id) const { return findIn(layers, id); }

GraphicLayer* Graphic::findLayer(std::uint32_t id) {
    return const_cast<GraphicLayer*>(std::as_const(*this).findLayer(id)); // this object is not const
}

std::size_t Graphic::layerCount() const { return countIn(layers); }

std::uint32_t Graphic::nextLayerId() const { return maxIdIn(layers) + 1; }

const BrandColor* BrandKit::color(std::string_view n) const {
    const auto it = std::find_if(colors.begin(), colors.end(), [&](const BrandColor& c) { return c.name == n; });
    return it == colors.end() ? nullptr : &*it;
}

const BrandFont* BrandKit::font(std::string_view r) const {
    const auto it = std::find_if(fonts.begin(), fonts.end(), [&](const BrandFont& f) { return f.role == r; });
    return it == fonts.end() ? nullptr : &*it;
}

const BrandLogo* BrandKit::logo(std::string_view n) const {
    const auto it = std::find_if(logos.begin(), logos.end(), [&](const BrandLogo& l) { return l.name == n; });
    return it == logos.end() ? nullptr : &*it;
}

std::array<double, 4> resolveColor(const GraphicColor& color, const BrandKit* kit) {
    if (!color.brand.empty() && kit) {
        if (const BrandColor* c = kit->color(color.brand)) {
            return c->rgba;
        }
    }
    return color.rgba;
}

// ---- Checks -----------------------------------------------------------------------------------------------------------

bool isValidFieldId(std::string_view id) {
    if (id.empty() || id.size() > 64 || !((id[0] >= 'a' && id[0] <= 'z') || (id[0] >= 'A' && id[0] <= 'Z'))) {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
    });
}

bool isValidTemplateId(std::string_view id) {
    const auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
    if (id.empty() || id.size() > 128 || !alnum(id[0]) ||
        !std::all_of(id.begin(), id.end(), [&](char c) { return alnum(c) || c == '_' || c == '.' || c == '-'; })) {
        return false;
    }
    // The id names a file in the user's library: not a Windows device name (CON, NUL, COM1, ...), with or without an
    // extension.
    std::string stem(id.substr(0, id.find('.')));
    std::transform(stem.begin(), stem.end(), stem.begin(), [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; });
    for (const std::string_view device : {"CON", "PRN", "AUX", "NUL"}) {
        if (stem == device) {
            return false;
        }
    }
    return !(stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem[3] >= '0' && stem[3] <= '9');
}

std::optional<std::string> textStyleProblem(const ProjectTextStyle& s) {
    if (s.name.empty() || !isText(s.name, kMaxGraphicNameBytes) ||
        std::any_of(s.name.begin(), s.name.end(), [](char ch) { return static_cast<unsigned char>(ch) < 0x20; })) {
        return "text style '" + s.name.substr(0, 64) + "': a name of 1 to 256 bytes of UTF-8 without control characters";
    }
    // Its values are checked as a text layer's.
    Graphic probe;
    GraphicLayer layer;
    layer.id = 1;
    layer.kind = LayerKind::Text;
    layer.text.runs = {TextRun{"A", s.style}};
    layer.text.align = s.align;
    layer.text.background = s.background;
    layer.shadow = s.shadow;
    probe.layers = {layer};
    if (const auto problem = graphicProblem(probe)) {
        return "text style '" + s.name + "': " + *problem;
    }
    return std::nullopt;
}

std::optional<std::string> graphicProblem(const Graphic& g) {
    Checker c;
    c.require(isText(g.name, kMaxGraphicNameBytes), "name", "too long or not UTF-8");
    c.require(g.width >= 1 && g.width <= kMaxGraphicCanvas && g.height >= 1 && g.height <= kMaxGraphicCanvas, "canvas",
              "width and height must be 1.." + std::to_string(kMaxGraphicCanvas));
    Walk walk{c, {}, 0};
    if (c.ok()) {
        checkLayers(walk, g.layers, 1, "layers");
    }
    if (g.responsive) {
        const ResponsiveTime& r = *g.responsive;
        c.require(r.duration.value > 0 && r.duration.value < kMaxTicks && r.intro.value >= 0 && r.outro.value >= 0 &&
                      r.intro.value + r.outro.value <= r.duration.value,
                  "responsive", "needs a positive duration with intro + outro inside it");
    }
    if (g.roll) {
        const GraphicRoll& r = *g.roll;
        for (const Ticks t : {r.preroll, r.postroll, r.easeIn, r.easeOut}) {
            c.require(t.value >= 0 && t.value < kMaxTicks, "roll", "preroll, postroll, ease-in and ease-out must be >= 0");
        }
    }
    if (g.instance && c.ok()) {
        const TemplateInstance& i = *g.instance;
        c.require(isValidTemplateId(i.templateId) && i.version >= 1, "template", "a bad template id or version");
        if (auto problem = fieldsProblem(i.fields, g)) {
            c.fail("template", *problem);
        }
    }
    return c.take();
}

std::optional<std::string> brandKitProblem(const BrandKit& kit) {
    Checker c;
    c.require(isText(kit.name, kMaxGraphicNameBytes), "name", "too long or not UTF-8");
    c.require(kit.colors.size() <= kMaxBrandColors && kit.fonts.size() <= kMaxBrandFonts && kit.logos.size() <= kMaxBrandLogos,
              "", "a brand kit holds at most 64 colours, 16 fonts and 32 logos");
    std::set<std::string> names;
    for (const BrandColor& col : kit.colors) {
        c.require(!col.name.empty() && isText(col.name, kMaxGraphicNameBytes) && names.insert(col.name).second, "colors",
                  "names must be unique, UTF-8, 1..256 bytes");
        c.color(GraphicColor{col.rgba, {}}, "colors." + col.name.substr(0, 64));
    }
    names.clear();
    for (const BrandFont& f : kit.fonts) {
        c.require(!f.role.empty() && isText(f.role, kMaxFontBytes) && names.insert(f.role).second, "fonts",
                  "roles must be unique, UTF-8, 1..128 bytes");
        c.require(!f.family.empty() && isText(f.family, kMaxFontBytes) && isText(f.style, kMaxFontBytes), "fonts",
                  "a font needs a family of at most 128 bytes");
        c.require(f.weight >= 100 && f.weight <= 900, "fonts", "weights must be 100..900");
    }
    names.clear();
    for (const BrandLogo& l : kit.logos) {
        c.require(!l.name.empty() && isText(l.name, kMaxGraphicNameBytes) && names.insert(l.name).second, "logos",
                  "names must be unique, UTF-8, 1..256 bytes");
    }
    return c.take();
}

std::optional<std::string> templateProblem(const GraphicTemplate& t) {
    if (!isValidTemplateId(t.id)) {
        return "template id \"" + t.id.substr(0, 128) + "\" must match [A-Za-z0-9][A-Za-z0-9_.-]* (128 bytes at most)";
    }
    if (t.name.empty() || !isText(t.name, kMaxGraphicNameBytes) || !isText(t.description, 4096)) {
        return "template " + t.id + ": a name (1..256 bytes) and a description of at most 4 KiB, UTF-8";
    }
    if (t.version < 1 || t.duration.value < 0 || t.duration.value >= kMaxTicks) {
        return "template " + t.id + ": version must be >= 1 and the duration >= 0";
    }
    if (auto problem = graphicProblem(t.graphic)) {
        return "template " + t.id + ": " + *problem;
    }
    if (auto problem = fieldsProblem(t.fields, t.graphic)) {
        return "template " + t.id + ": " + *problem;
    }
    return std::nullopt;
}

// ---- Clusters ---------------------------------------------------------------------------------------------------------

std::vector<std::pair<std::size_t, std::size_t>> graphemeClusters(std::string_view text) {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t start = i;
        const char32_t first = decode(text, i);
        if (first == '\r' && i < text.size() && text[i] == '\n') {
            ++i;
        } else if (first != '\n' && first != '\r') {
            bool joined = false;
            bool regionalPair = isRegionalIndicator(first);
            while (i < text.size()) {
                std::size_t next = i;
                const char32_t cp = decode(text, next);
                if (extendsCluster(cp)) {
                    i = next;
                } else if (cp == 0x200D) { // ZWJ: it and the next code point stay
                    i = next;
                    joined = true;
                } else if (joined && cp != '\n' && cp != '\r') {
                    i = next;
                    joined = false;
                } else if (regionalPair && isRegionalIndicator(cp)) {
                    i = next;
                    regionalPair = false;
                } else {
                    break;
                }
            }
        }
        out.emplace_back(start, i);
    }
    return out;
}

} // namespace wabcut
