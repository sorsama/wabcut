// Evaluating a graphic at a time (C.13): GraphicResolve.h.

#include <wabcut/model/GraphicResolve.h>

#include <wabcut/model/ProjectState.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace wabcut {

namespace {

constexpr double kKappa = 0.5522847498307936; // a quarter circle as one cubic Bezier

double radians(double degrees) { return degrees * std::numbers::pi / 180.0; }

Affine2D translation(double x, double y) { return {1.0, 0.0, 0.0, 1.0, x, y}; }
Affine2D scaling(double x, double y) { return {x, 0.0, 0.0, y, 0.0, 0.0}; }
Affine2D rotationMatrix(double degrees) {
    const double r = radians(degrees);
    const double c = std::cos(r);
    const double s = std::sin(r);
    return {c, s, -s, c, 0.0, 0.0}; // clockwise on screen (y down)
}

/// The values of a layer's transform at a time.
struct TransformAt {
    double x = 0.0;
    double y = 0.0;
    double anchorX = 0.0;
    double anchorY = 0.0;
    double scaleX = 1.0;
    double scaleY = 1.0;
    double rotation = 0.0;
    double opacity = 1.0;

    Affine2D matrix() const {
        return translation(x, y) * rotationMatrix(rotation) * scaling(scaleX, scaleY) * translation(-anchorX, -anchorY);
    }
};

TransformAt transformAt(const LayerTransform& t, Ticks time) {
    return {t.x.valueAt(time),
            t.y.valueAt(time),
            t.anchorX.valueAt(time),
            t.anchorY.valueAt(time),
            t.scaleX.valueAt(time) / 100.0,
            t.scaleY.valueAt(time) / 100.0,
            t.rotation.valueAt(time),
            std::clamp(t.opacity.valueAt(time) / 100.0, 0.0, 1.0)};
}

bool shownAt(const GraphicLayer& l, Ticks time) { return l.visible && time >= l.in && (!l.out || time < *l.out); }

MaskVertex corner(double x, double y) { return {x, y, 0.0, 0.0, 0.0, 0.0}; }

std::vector<MaskVertex> rectanglePath(double w, double h, double radius) {
    const double hw = w / 2.0;
    const double hh = h / 2.0;
    const double r = std::clamp(radius, 0.0, std::min(hw, hh));
    if (r <= 0.0) {
        return {corner(-hw, -hh), corner(hw, -hh), corner(hw, hh), corner(-hw, hh)};
    }
    const double k = kKappa * r;
    return {{-hw + r, -hh, -k, 0.0, 0.0, 0.0}, {hw - r, -hh, 0.0, 0.0, k, 0.0},
            {hw, -hh + r, 0.0, -k, 0.0, 0.0},  {hw, hh - r, 0.0, 0.0, 0.0, k},
            {hw - r, hh, k, 0.0, 0.0, 0.0},    {-hw + r, hh, 0.0, 0.0, -k, 0.0},
            {-hw, hh - r, 0.0, k, 0.0, 0.0},   {-hw, -hh + r, 0.0, 0.0, 0.0, -k}};
}

std::vector<MaskVertex> ellipsePath(double w, double h) {
    const double rx = w / 2.0;
    const double ry = h / 2.0;
    const double kx = kKappa * rx;
    const double ky = kKappa * ry;
    return {{0.0, -ry, -kx, 0.0, kx, 0.0}, {rx, 0.0, 0.0, -ky, 0.0, ky}, {0.0, ry, kx, 0.0, -kx, 0.0},
            {-rx, 0.0, 0.0, ky, 0.0, -ky}};
}

std::vector<MaskVertex> starPath(std::uint32_t points, double outer, double inner, bool star) {
    std::vector<MaskVertex> out;
    const std::uint32_t n = std::clamp<std::uint32_t>(points, 3, static_cast<std::uint32_t>(kMaxShapePoints));
    const std::uint32_t count = star ? 2 * n : n;
    for (std::uint32_t i = 0; i < count; ++i) {
        const double angle = -90.0 + 360.0 * static_cast<double>(i) / static_cast<double>(count);
        const double r = star && (i % 2 == 1) ? inner : outer;
        out.push_back(corner(r * std::cos(radians(angle)), r * std::sin(radians(angle))));
    }
    return out;
}

std::vector<MaskVertex> pathAt(const std::vector<MaskPathKey>& keys, Ticks time) {
    Mask mask;
    mask.shape = MaskShape::Path;
    mask.path = keys;
    return maskPathAt(mask, time);
}

Box boxOf(const std::vector<MaskVertex>& path) {
    if (path.empty()) {
        return {};
    }
    double x0 = path[0].x;
    double y0 = path[0].y;
    double x1 = x0;
    double y1 = y0;
    for (const MaskVertex& v : path) {
        for (const auto& [x, y] : {std::pair{v.x, v.y}, std::pair{v.x + v.inX, v.y + v.inY}, std::pair{v.x + v.outX, v.y + v.outY}}) {
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x);
            y1 = std::max(y1, y);
        }
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

/// The box `local` covers once mapped through `m` (its corners' bounding box).
Box mapBox(const Affine2D& m, const Box& local) {
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
    bool first = true;
    for (const auto& [px, py] : {std::pair{local.x, local.y}, std::pair{local.x + local.width, local.y},
                                 std::pair{local.x, local.y + local.height},
                                 std::pair{local.x + local.width, local.y + local.height}}) {
        const auto p = m.map(px, py);
        x0 = first ? p[0] : std::min(x0, p[0]);
        y0 = first ? p[1] : std::min(y0, p[1]);
        x1 = first ? p[0] : std::max(x1, p[0]);
        y1 = first ? p[1] : std::max(y1, p[1]);
        first = false;
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

Box unite(const Box& a, const Box& b) {
    if (a.width <= 0.0 && a.height <= 0.0) {
        return b;
    }
    if (b.width <= 0.0 && b.height <= 0.0) {
        return a;
    }
    const double x0 = std::min(a.x, b.x);
    const double y0 = std::min(a.y, b.y);
    return {x0, y0, std::max(a.x + a.width, b.x + b.width) - x0, std::max(a.y + a.height, b.y + b.height) - y0};
}

double alignFactor(TextAlign a) { return a == TextAlign::Center ? 0.5 : a == TextAlign::Right ? 1.0 : 0.0; }

std::string resolvedFamily(const TextStyle& s, const BrandKit* kit) {
    if (!s.brandFont.empty() && kit) {
        if (const BrandFont* f = kit->font(s.brandFont)) {
            return f->family;
        }
    }
    return s.fontFamily;
}

std::vector<std::string> runFonts(const TextContent& t, const BrandKit* kit) {
    std::vector<std::string> fonts;
    for (const TextRun& r : t.runs) {
        fonts.push_back(resolvedFamily(r.style, kit));
    }
    return fonts;
}

const TextMeasurer& measurerOf(const ResolveOptions& o) { return o.measurer ? *o.measurer : approximateTextMeasurer(); }
const LineBreaker& breakerOf(const ResolveOptions& o) { return o.lineBreaker ? *o.lineBreaker : simpleLineBreaker(); }

/// A shape layer's sizes at a time, before pins.
struct ShapeSize {
    double width = 0.0;
    double height = 0.0;
};

ShapeSize shapeSizeAt(const ShapeContent& s, Ticks time) {
    return {std::max(0.0, s.width.valueAt(time)), std::max(0.0, s.height.valueAt(time))};
}

Box shapeBounds(const ShapeContent& s, Ticks time, ShapeSize size) {
    switch (s.kind) {
    case ShapeKind::Rectangle:
    case ShapeKind::Ellipse:
        return {-size.width / 2.0, -size.height / 2.0, size.width, size.height};
    case ShapeKind::Polygon:
    case ShapeKind::Star: {
        const double r = std::max(s.outerRadius.valueAt(time), s.kind == ShapeKind::Star ? s.innerRadius.valueAt(time) : 0.0);
        return {-r, -r, 2.0 * r, 2.0 * r};
    }
    case ShapeKind::Path:
        return boxOf(pathAt(s.path, time));
    }
    return {};
}

Box textBounds(const TextContent& t, Ticks time, const ResolveOptions& o) {
    if (t.path) {
        // Type on a path: the path's box, grown by the text's height on every side (the characters stand on it and
        // lines go below; an open path's text may run on past its ends).
        TextContent lines = t;
        lines.path.reset();
        lines.boxWidth = 0.0;
        lines.boxHeight = 0.0;
        const auto size = measurerOf(o).measure(lines, runFonts(lines, o.brandKit));
        Box box = boxOf(pathAt(t.path->path, time));
        double maxSize = 0.0;
        for (const TextRun& r : t.runs) {
            maxSize = std::max(maxSize, r.style.fontSize);
        }
        const double pad = size[1] + maxSize;
        box = {box.x - pad, box.y - pad, box.width + 2 * pad, box.height + 2 * pad};
        if (!t.path->closed) {
            box = {box.x - size[0], box.y - size[0], box.width + 2 * size[0], box.height + 2 * size[0]};
        }
        return box;
    }
    const auto size = measurerOf(o).measure(t, runFonts(t, o.brandKit));
    return {-size[0] * alignFactor(t.align), 0.0, size[0], size[1]};
}

Box boundsOf(const GraphicLayer& l, Ticks time, const ResolveOptions& o, std::size_t depth);

Box groupBounds(const GraphicLayer& g, Ticks time, const ResolveOptions& o, std::size_t depth) {
    Box box;
    if (depth > kMaxGraphicDepth) {
        return box;
    }
    for (const GraphicLayer& c : g.children) {
        if (shownAt(c, time)) {
            box = unite(box, mapBox(transformAt(c.transform, time).matrix(), boundsOf(c, time, o, depth + 1)));
        }
    }
    return box;
}

Box boundsOf(const GraphicLayer& l, Ticks time, const ResolveOptions& o, std::size_t depth) {
    switch (l.kind) {
    case LayerKind::Shape:
        return shapeBounds(l.shape, time, shapeSizeAt(l.shape, time));
    case LayerKind::Text:
        return textBounds(l.text, time, o);
    case LayerKind::Image:
        return {-l.image.width / 2.0, -l.image.height / 2.0, l.image.width, l.image.height};
    case LayerKind::Group:
        return groupBounds(l, time, o, depth);
    }
    return {};
}

/// One layer of a sibling list at a time, with pins applied.
struct Placed {
    const GraphicLayer* layer = nullptr;
    TransformAt transform;
    ShapeSize size;      ///< shapes and images, after pins
    Box bounds;          ///< own pixels, after pins
    bool resolved = false;
};

/// The edge `bit` of a box (left, top, right, bottom).
double edgeOf(const Box& b, std::uint8_t bit) {
    switch (bit) {
    case LayerPin::kLeft:
        return b.x;
    case LayerPin::kTop:
        return b.y;
    case LayerPin::kRight:
        return b.x + b.width;
    default:
        return b.y + b.height;
    }
}

constexpr std::array<std::uint8_t, 4> kEdgeBits{LayerPin::kLeft, LayerPin::kTop, LayerPin::kRight, LayerPin::kBottom};

/// Moves (and for boxes pinned on opposite edges, resizes) `p` so its pinned edges sit at the target's edges plus the
/// pin's offsets, in the parent's pixels.
void applyPin(Placed& p, const Placed& target, Ticks time) {
    const LayerPin& pin = *p.layer->pin;
    const Box targetBox = mapBox(target.transform.matrix(), target.bounds);
    const Box ownBox = mapBox(p.transform.matrix(), p.bounds);
    std::array<double, 4> want{};
    for (std::size_t i = 0; i < 4; ++i) {
        want[i] = edgeOf(targetBox, kEdgeBits[i]) + pin.offsets[i];
    }
    const bool resizable = (p.layer->kind == LayerKind::Shape &&
                            (p.layer->shape.kind == ShapeKind::Rectangle || p.layer->shape.kind == ShapeKind::Ellipse)) ||
                           p.layer->kind == LayerKind::Image;
    const bool upright = std::fmod(std::abs(p.transform.rotation), 360.0) < 1e-9;
    const auto axis = [&](std::uint8_t lowBit, std::uint8_t highBit, std::size_t lowIndex, std::size_t highIndex, double& pos,
                          double scale, double& size, double anchor, double ownLow, double ownHigh) {
        const bool low = (pin.edges & lowBit) != 0;
        const bool high = (pin.edges & highBit) != 0;
        if (low && high && resizable && upright && std::abs(scale) > 1e-12) {
            const double extent = std::max(0.0, want[highIndex] - want[lowIndex]);
            size = extent / std::abs(scale);
            // The box is centred on the layer's origin: put its centre between the wanted edges.
            pos = (want[lowIndex] + want[highIndex]) / 2.0 + anchor * scale;
        } else if (low) {
            pos += want[lowIndex] - ownLow;
        } else if (high) {
            pos += want[highIndex] - ownHigh;
        }
    };
    axis(LayerPin::kLeft, LayerPin::kRight, 0, 2, p.transform.x, p.transform.scaleX, p.size.width, p.transform.anchorX, ownBox.x,
         ownBox.x + ownBox.width);
    axis(LayerPin::kTop, LayerPin::kBottom, 1, 3, p.transform.y, p.transform.scaleY, p.size.height, p.transform.anchorY, ownBox.y,
         ownBox.y + ownBox.height);
    if (p.layer->kind == LayerKind::Shape) {
        p.bounds = shapeBounds(p.layer->shape, time, p.size);
    } else if (p.layer->kind == LayerKind::Image) {
        p.bounds = {-p.size.width / 2.0, -p.size.height / 2.0, p.size.width, p.size.height};
    }
}

std::vector<Placed> placeSiblings(const std::vector<GraphicLayer>& layers, Ticks time, const ResolveOptions& o,
                                  std::size_t depth, const Box* frame) {
    std::vector<Placed> placed;
    placed.reserve(layers.size());
    for (const GraphicLayer& l : layers) {
        Placed p;
        p.layer = &l;
        p.transform = transformAt(l.transform, time);
        if (l.kind == LayerKind::Shape) {
            p.size = shapeSizeAt(l.shape, time);
        } else if (l.kind == LayerKind::Image) {
            p.size = {l.image.width, l.image.height};
        }
        p.bounds = boundsOf(l, time, o, depth);
        p.resolved = !l.pin.has_value();
        placed.push_back(p);
    }
    // Targets first; pins are acyclic and between siblings (graphicProblem), so this ends within n passes.
    for (std::size_t pass = 0; pass <= layers.size(); ++pass) {
        bool progress = false;
        for (Placed& p : placed) {
            if (p.resolved) {
                continue;
            }
            if (p.layer->pin->target == LayerPin::kVideoFrame) {
                // 0.2: pinned to the video frame (top-level layers): the canvas box.
                if (frame) {
                    Placed canvas;
                    canvas.bounds = *frame;
                    applyPin(p, canvas, time);
                }
                p.resolved = true;
                progress = true;
                continue;
            }
            const auto target = std::find_if(placed.begin(), placed.end(),
                                             [&](const Placed& q) { return q.layer->id == p.layer->pin->target; });
            if (target == placed.end()) {
                p.resolved = true; // a dangling pin is ignored
                progress = true;
                continue;
            }
            if (target->resolved) {
                applyPin(p, *target, time);
                p.resolved = true;
                progress = true;
            }
        }
        if (!progress) {
            break;
        }
    }
    return placed;
}

Rgba resolved(const GraphicColor& c, const BrandKit* kit) { return resolveColor(c, kit); }

DrawPaint drawPaint(const Paint& p, const BrandKit* kit) {
    DrawPaint out;
    out.kind = p.kind;
    out.color = resolved(p.color, kit);
    for (const GradientStop& s : p.stops) {
        out.stops.emplace_back(s.offset, resolved(s.color, kit));
    }
    out.start = p.start;
    out.end = p.end;
    return out;
}

std::vector<DrawMask> drawMasks(std::uint32_t owner, const std::vector<Mask>& masks, Ticks time, const Affine2D& toCanvas,
                                std::int32_t width, std::int32_t height) {
    std::vector<DrawMask> out;
    const double w = width;
    const double h = height;
    const double aspect = w / h;
    for (const Mask& m : masks) {
        if (!m.enabled) {
            continue;
        }
        DrawMask dm;
        dm.owner = owner;
        dm.mode = m.mode;
        dm.inverted = m.inverted;
        dm.feather = maskValue(m, "feather", time) * h;
        dm.expansion = maskValue(m, "expansion", time) * h;
        dm.opacity = std::clamp(maskValue(m, "opacity", time), 0.0, 1.0);
        for (const MaskVertex& v : maskOutlineAt(m, time, aspect)) {
            const auto p = toCanvas.map(v.x * w, v.y * h);
            const auto mapVector = [&](double dx, double dy) {
                return std::array<double, 2>{toCanvas.a * dx * w + toCanvas.c * dy * h, toCanvas.b * dx * w + toCanvas.d * dy * h};
            };
            const auto in = mapVector(v.inX, v.inY);
            const auto out2 = mapVector(v.outX, v.outY);
            dm.outline.push_back({p[0], p[1], in[0], in[1], out2[0], out2[1]});
        }
        out.push_back(std::move(dm));
    }
    return out;
}

DrawShadow drawShadow(const DropShadow& s, const BrandKit* kit) {
    return {s.enabled, resolved(s.color, kit), s.opacity, s.angle, s.distance, s.softness};
}

DrawText drawText(const TextContent& t, Ticks time, const ResolveOptions& o) {
    DrawText out;
    std::size_t byte = 0;
    for (const TextRun& r : t.runs) {
        DrawTextRun run;
        run.byteStart = byte;
        run.text = r.text;
        const TextStyle& s = r.style;
        run.fontFamily = s.fontFamily;
        run.fontStyle = s.fontStyle;
        run.fontWeight = s.fontWeight;
        if (!s.brandFont.empty() && o.brandKit) {
            if (const BrandFont* f = o.brandKit->font(s.brandFont)) {
                run.fontFamily = f->family;
                run.fontStyle = f->style;
                run.fontWeight = f->weight;
            }
        }
        run.fontSize = s.fontSize;
        run.fill = resolved(s.fill, o.brandKit);
        run.italic = s.italic;
        run.underline = s.underline;
        run.tracking = s.tracking;
        run.leading = s.leading;
        run.baselineShift = s.baselineShift;
        byte += r.text.size();
        out.runs.push_back(std::move(run));
    }
    out.align = t.align;
    out.boxWidth = t.boxWidth;
    out.boxHeight = t.boxHeight;
    out.language = t.language;
    out.background = t.background.enabled;
    out.backgroundColor = resolved(t.background.color, o.brandKit);
    out.backgroundPadding = t.background.padding;
    out.backgroundRadius = t.background.radius;
    if (t.path) {
        out.path = pathAt(t.path->path, time);
        out.pathClosed = t.path->closed;
        out.pathReverse = t.path->reverse;
        out.pathPerpendicular = t.path->perpendicular;
        out.pathForceAlignment = t.path->forceAlignment;
        out.firstMargin = t.path->firstMargin.valueAt(time);
        out.lastMargin = t.path->lastMargin.valueAt(time);
        out.boxWidth = 0.0; // the path is the line
        out.boxHeight = 0.0;
    }
    if (t.animators.empty() && !t.path) {
        return out;
    }
    const std::string text = t.text();
    std::vector<DrawCluster> clusters;
    for (const auto& [b, e] : graphemeClusters(text)) {
        if (text[b] != '\n' && text[b] != '\r') {
            clusters.push_back(DrawCluster{b, e});
        }
    }
    for (const TextAnimator& a : t.animators) {
        const auto units = textUnits(text, a.unit, t.language, breakerOf(o));
        std::size_t unit = 0;
        for (DrawCluster& c : clusters) {
            while (unit < units.size() && units[unit].second <= c.byteStart) {
                ++unit;
            }
            if (unit >= units.size() || c.byteStart < units[unit].first) {
                continue; // between units (the spaces between words): untouched
            }
            const double amount = textSelection(a, unit, units.size(), time);
            if (amount <= 0.0) {
                continue;
            }
            c.opacity *= 1.0 - amount * (1.0 - a.opacity / 100.0);
            c.offsetX += amount * a.offsetX;
            c.offsetY += amount * a.offsetY;
            c.scale *= 1.0 + amount * (a.scale / 100.0 - 1.0);
            c.rotation += amount * a.rotation;
        }
    }
    out.clusters = std::move(clusters);
    return out;
}

struct Context {
    const ResolveOptions& options;
    std::int32_t width;
    std::int32_t height;
    Ticks time;
    std::vector<DrawItem>& items;
};

void resolveLayers(Context& ctx, const std::vector<GraphicLayer>& layers, const Affine2D& parent, double parentOpacity,
                   BlendMode parentBlend, const std::vector<DrawMask>& parentMasks, std::size_t depth) {
    if (depth > kMaxGraphicDepth) {
        return;
    }
    const Box frame{0.0, 0.0, static_cast<double>(ctx.width), static_cast<double>(ctx.height)};
    const std::vector<Placed> placed = placeSiblings(layers, ctx.time, ctx.options, depth, depth == 1 ? &frame : nullptr);
    for (const Placed& p : placed) {
        const GraphicLayer& l = *p.layer;
        if (!shownAt(l, ctx.time)) {
            continue;
        }
        const Affine2D toCanvas = parent * p.transform.matrix();
        const double opacity = parentOpacity * p.transform.opacity;
        const BlendMode blend = l.blend == BlendMode::Normal ? parentBlend : l.blend;
        std::vector<DrawMask> masks = drawMasks(l.id, l.masks, ctx.time, toCanvas, ctx.width, ctx.height);
        masks.insert(masks.end(), parentMasks.begin(), parentMasks.end());
        if (l.kind == LayerKind::Group) {
            resolveLayers(ctx, l.children, toCanvas, opacity, blend, masks, depth + 1);
            continue;
        }
        DrawItem item;
        item.layer = l.id;
        item.transform = toCanvas;
        item.opacity = opacity;
        item.blend = blend;
        item.shadow = drawShadow(l.shadow, ctx.options.brandKit);
        item.masks = std::move(masks);
        item.bounds = p.bounds;
        switch (l.kind) {
        case LayerKind::Shape: {
            const ShapeContent& s = l.shape;
            item.kind = DrawKind::Shape;
            switch (s.kind) {
            case ShapeKind::Rectangle:
                item.path = rectanglePath(p.size.width, p.size.height, s.cornerRadius.valueAt(ctx.time));
                break;
            case ShapeKind::Ellipse:
                item.path = ellipsePath(p.size.width, p.size.height);
                break;
            case ShapeKind::Polygon:
                item.path = starPath(s.points, s.outerRadius.valueAt(ctx.time), 0.0, false);
                break;
            case ShapeKind::Star:
                item.path = starPath(s.points, s.outerRadius.valueAt(ctx.time), s.innerRadius.valueAt(ctx.time), true);
                break;
            case ShapeKind::Path:
                item.path = pathAt(s.path, ctx.time);
                break;
            }
            item.closed = s.kind != ShapeKind::Path || s.closed;
            item.fill = drawPaint(s.fill, ctx.options.brandKit);
            item.stroke = DrawStroke{s.stroke.enabled, resolved(s.stroke.color, ctx.options.brandKit),
                                     std::max(0.0, s.stroke.width.valueAt(ctx.time)), s.stroke.dashes, s.stroke.join, s.stroke.cap};
            item.trimStart = std::clamp(s.trimStart.valueAt(ctx.time) / 100.0, 0.0, 1.0);
            item.trimEnd = std::clamp(s.trimEnd.valueAt(ctx.time) / 100.0, 0.0, 1.0);
            item.trimOffset = s.trimOffset.valueAt(ctx.time);
            break;
        }
        case LayerKind::Text:
            item.kind = DrawKind::Text;
            item.text = drawText(l.text, ctx.time, ctx.options);
            break;
        case LayerKind::Image: {
            item.kind = DrawKind::Image;
            MediaId media = l.image.media;
            if (!l.image.brandLogo.empty() && ctx.options.brandKit) {
                if (const BrandLogo* logo = ctx.options.brandKit->logo(l.image.brandLogo)) {
                    media = logo->media;
                }
            }
            item.image = DrawImage{media, p.size.width, p.size.height, l.image.fit};
            break;
        }
        case LayerKind::Group:
            break;
        }
        ctx.items.push_back(std::move(item));
    }
}

class ApproximateMeasurer : public TextMeasurer {
public:
    std::array<double, 2> measure(const TextContent& text, const std::vector<std::string>&) const override {
        struct Line {
            double width = 0.0;
            double height = 0.0;
        };
        std::vector<Line> lines(1);
        const auto newLine = [&] { lines.emplace_back(); };
        const double wrap = text.boxWidth;
        for (const TextRun& run : text.runs) {
            const double em = run.style.fontSize;
            const double advance = 0.55 * em + run.style.tracking / 1000.0 * em;
            const double lineHeight = run.style.leading > 0.0 ? run.style.leading : 1.2 * em;
            std::size_t start = 0;
            while (start <= run.text.size()) {
                const std::size_t nl = std::min(run.text.find('\n', start), run.text.size());
                const std::string_view piece = std::string_view(run.text).substr(start, nl - start);
                std::size_t from = 0;
                std::vector<std::size_t> breaks = wrap > 0.0 ? simpleLineBreaker().breakOpportunities(piece, text.language)
                                                             : std::vector<std::size_t>{};
                breaks.push_back(piece.size());
                for (const std::size_t to : breaks) {
                    const double w = static_cast<double>(captionDisplayLength(piece.substr(from, to - from))) * advance;
                    if (wrap > 0.0 && lines.back().width > 0.0 && lines.back().width + w > wrap) {
                        newLine();
                    }
                    lines.back().width += w;
                    lines.back().height = std::max(lines.back().height, lineHeight);
                    from = to;
                }
                if (nl == run.text.size()) {
                    break;
                }
                newLine();
                lines.back().height = lineHeight;
                start = nl + 1;
            }
        }
        double width = 0.0;
        double height = 0.0;
        for (const Line& l : lines) {
            width = std::max(width, l.width);
            height += l.height;
        }
        if (wrap > 0.0) {
            width = wrap;
        }
        if (text.boxHeight > 0.0) {
            height = text.boxHeight;
        }
        return {width, height};
    }
};

} // namespace

Affine2D Affine2D::operator*(const Affine2D& in) const {
    return {a * in.a + c * in.b,     b * in.a + d * in.b,     a * in.c + c * in.d,
            b * in.c + d * in.d,     a * in.e + c * in.f + e, b * in.e + d * in.f + f};
}

std::array<double, 2> Affine2D::map(double x, double y) const { return {a * x + c * y + e, b * x + d * y + f}; }

const TextMeasurer& approximateTextMeasurer() {
    static const ApproximateMeasurer kMeasurer;
    return kMeasurer;
}

DrawList resolveGraphic(const Graphic& graphic, Ticks time, const ResolveOptions& options) {
    DrawList list;
    list.width = graphic.width;
    list.height = graphic.height;
    Context ctx{options, graphic.width, graphic.height, time, list.items};
    resolveLayers(ctx, graphic.layers, translation(options.offset[0], options.offset[1]), 1.0, BlendMode::Normal, {}, 1);
    return list;
}

Box contentBox(const DrawList& list) {
    Box box;
    for (const DrawItem& item : list.items) {
        if (item.bounds.width > 0.0 || item.bounds.height > 0.0) {
            box = unite(box, mapBox(item.transform, item.bounds));
        }
    }
    return box;
}

std::array<double, 2> rollOffset(const GraphicRoll& roll, const Box& content, std::int32_t width, std::int32_t height,
                                 Ticks sinceStart, Ticks clipDuration) {
    if (content.width <= 0.0 && content.height <= 0.0) {
        return {0.0, 0.0};
    }
    // Where the travel starts and ends along its axis (canvas pixels added to the content's position).
    const bool vertical = roll.kind == RollKind::Roll;
    const double extent = vertical ? height : width;
    const double low = vertical ? content.y : content.x;              // the content's top / left edge
    const double high = low + (vertical ? content.height : content.width); // its bottom / right edge
    double from = 0.0;
    double to = 0.0;
    if (roll.kind == RollKind::CrawlRight) {
        // Travels right: in past the frame's left edge, out past its right edge.
        from = roll.startOffScreen ? -high : 0.0;
        to = roll.endOffScreen ? extent - low : std::max(0.0, -low);
    } else {
        // Roll travels up, Crawl Left travels left: in past the frame's bottom / right edge, out past the top / left.
        from = roll.startOffScreen ? extent - low : 0.0;
        to = roll.endOffScreen ? -high : std::min(0.0, extent - high);
    }
    // How far along: still in the preroll and postroll, a trapezoidal speed between them.
    const double d = static_cast<double>(std::max<std::int64_t>(0, clipDuration.value));
    const double u = std::clamp(static_cast<double>(sinceStart.value), 0.0, d);
    const double pre = std::min(static_cast<double>(roll.preroll.value), d);
    const double post = std::min(static_cast<double>(roll.postroll.value), d - pre);
    const double span = d - pre - post;
    double p = 0.0;
    if (span <= 0.0) {
        p = u >= d - post && d > 0.0 ? 1.0 : 0.0;
    } else if (u <= pre) {
        p = 0.0;
    } else if (u >= pre + span) {
        p = 1.0;
    } else {
        double a = static_cast<double>(roll.easeIn.value);
        double b = static_cast<double>(roll.easeOut.value);
        if (a + b > span) {
            const double k = span / (a + b);
            a *= k;
            b *= k;
        }
        const double s = u - pre;
        const double v = 1.0 / (span - a / 2.0 - b / 2.0); // the cruising speed, fractions per tick
        if (a > 0.0 && s < a) {
            p = v * s * s / (2.0 * a);
        } else if (b > 0.0 && s > span - b) {
            p = 1.0 - v * (span - s) * (span - s) / (2.0 * b);
        } else {
            p = v * (s - a / 2.0);
        }
        p = std::clamp(p, 0.0, 1.0);
    }
    const double offset = from + (to - from) * p;
    return vertical ? std::array<double, 2>{0.0, offset} : std::array<double, 2>{offset, 0.0};
}

Ticks responsiveDesignTime(const ResponsiveTime& r, Ticks sinceStart, Ticks clipDuration) {
    const std::int64_t d0 = r.duration.value;
    const std::int64_t intro = std::min(r.intro.value, d0);
    const std::int64_t outro = std::min(r.outro.value, d0 - intro);
    const std::int64_t d = clipDuration.value;
    const std::int64_t t = std::clamp(sinceStart.value, std::int64_t{0}, std::max<std::int64_t>(d, 0));
    if (d <= 0 || d0 <= 0) {
        return Ticks{};
    }
    if (intro + outro >= d) {
        // Too short for both: play intro then outro, squeezed in proportion.
        const std::int64_t u = intro + outro > 0 ? scaleTicks(Ticks{t}, intro + outro, d).value : 0;
        return u < intro ? Ticks{u} : Ticks{d0 - (intro + outro - u)};
    }
    if (t < intro) {
        return Ticks{t};
    }
    if (t >= d - outro) {
        return Ticks{d0 - (d - t)};
    }
    const std::int64_t middle = d0 - intro - outro;
    return Ticks{intro + scaleTicks(Ticks{t - intro}, middle, d - intro - outro).value};
}

Ticks graphicTimeAt(const Clip& clip, const Graphic& graphic, Ticks t) {
    if (graphic.responsive) {
        return responsiveDesignTime(*graphic.responsive, Ticks{t.value - clip.start().value}, clip.duration());
    }
    return clip.curveTimeAt(t);
}

DrawList resolveGraphicClip(const ProjectState& state, const Clip& clip, Ticks t, ResolveOptions options) {
    const Graphic* graphic = clip.graphic();
    if (!graphic) {
        return {};
    }
    if (!options.brandKit && state.brandKit) {
        options.brandKit = &*state.brandKit;
    }
    const Ticks time = graphicTimeAt(clip, *graphic, t);
    if (!graphic->roll) {
        return resolveGraphic(*graphic, time, options);
    }
    // 0.2, Roll / Crawl: where the content is at this time, then the whole graphic moved along.
    DrawList still = resolveGraphic(*graphic, time, options);
    const auto offset = rollOffset(*graphic->roll, contentBox(still), graphic->width, graphic->height,
                                   Ticks{t.value - clip.start().value}, clip.duration());
    if (offset[0] == 0.0 && offset[1] == 0.0) {
        return still;
    }
    options.offset = {options.offset[0] + offset[0], options.offset[1] + offset[1]};
    return resolveGraphic(*graphic, time, options);
}

Box layerBounds(const GraphicLayer& layer, Ticks time, const ResolveOptions& options) {
    return boundsOf(layer, time, options, 1);
}

std::vector<std::pair<std::size_t, std::size_t>> textUnits(std::string_view text, TextUnit unit, std::string_view language,
                                                           const LineBreaker& breaker) {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    if (unit == TextUnit::Character) {
        for (const auto& [b, e] : graphemeClusters(text)) {
            if (text[b] != '\n' && text[b] != '\r') {
                out.emplace_back(b, e);
            }
        }
        return out;
    }
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t nl = std::min(text.find('\n', start), text.size());
        std::size_t lineEnd = nl;
        if (lineEnd > start && text[lineEnd - 1] == '\r') {
            --lineEnd;
        }
        const std::string_view line = text.substr(start, lineEnd - start);
        if (unit == TextUnit::Line) {
            if (!line.empty()) {
                out.emplace_back(start, lineEnd);
            }
        } else {
            std::vector<std::size_t> cuts = breaker.breakOpportunities(line, language);
            cuts.push_back(line.size());
            std::size_t from = 0;
            for (const std::size_t to : cuts) {
                std::size_t b = from;
                std::size_t e = std::min(to, line.size());
                while (b < e && (line[b] == ' ' || line[b] == '\t')) {
                    ++b;
                }
                while (e > b && (line[e - 1] == ' ' || line[e - 1] == '\t')) {
                    --e;
                }
                if (e > b) {
                    out.emplace_back(start + b, start + e);
                }
                from = std::max(from, to);
            }
        }
        if (nl == text.size()) {
            break;
        }
        start = nl + 1;
    }
    return out;
}

double textSelection(const TextAnimator& a, std::size_t index, std::size_t count, Ticks time) {
    if (count == 0) {
        return 0.0;
    }
    const double offset = a.offset.valueAt(time);
    double s = a.start.valueAt(time) + offset;
    double e = a.end.valueAt(time) + offset;
    if (s > e) {
        std::swap(s, e);
    }
    const double n = static_cast<double>(count);
    const double from = s / 100.0 * n;
    const double to = e / 100.0 * n;
    const double x = static_cast<double>(index) + 0.5;
    if (a.shape == TextSelectorShape::Square || a.softness <= 0.0) {
        return x >= from && x < to ? 1.0 : 0.0;
    }
    const double k = a.softness;
    return std::clamp((x - from) / k + 0.5, 0.0, 1.0) * std::clamp((to - x) / k + 0.5, 0.0, 1.0);
}


// ---- Type on a path (0.2) ---------------------------------------------------------------------------------------------

TextPathWalker::TextPathWalker(const std::vector<MaskVertex>& v, bool closed, bool reverse) : m_closed(closed) {
    if (v.empty()) {
        return;
    }
    const std::size_t segments = closed ? v.size() : v.size() - 1;
    m_points.push_back({v[0].x, v[0].y});
    for (std::size_t i = 0; i < segments; ++i) {
        const MaskVertex& p = v[i];
        const MaskVertex& q = v[(i + 1) % v.size()];
        const std::array<double, 2> p0{p.x, p.y};
        const std::array<double, 2> c1{p.x + p.outX, p.y + p.outY};
        const std::array<double, 2> c2{q.x + q.inX, q.y + q.inY};
        const std::array<double, 2> p3{q.x, q.y};
        const bool straight = p.outX == 0 && p.outY == 0 && q.inX == 0 && q.inY == 0;
        const int n = straight ? 1 : 48;
        for (int k = 1; k <= n; ++k) {
            const double t = static_cast<double>(k) / n;
            const double u = 1.0 - t;
            m_points.push_back({u * u * u * p0[0] + 3 * u * u * t * c1[0] + 3 * u * t * t * c2[0] + t * t * t * p3[0],
                                u * u * u * p0[1] + 3 * u * u * t * c1[1] + 3 * u * t * t * c2[1] + t * t * t * p3[1]});
        }
    }
    if (reverse) {
        std::reverse(m_points.begin(), m_points.end());
    }
    // Drop repeated points (no direction between them).
    std::vector<std::array<double, 2>> kept;
    for (const auto& p : m_points) {
        if (kept.empty() || std::hypot(p[0] - kept.back()[0], p[1] - kept.back()[1]) > 1e-9) {
            kept.push_back(p);
        }
    }
    m_points = std::move(kept);
    m_lengths.assign(m_points.size(), 0.0);
    for (std::size_t i = 1; i < m_points.size(); ++i) {
        m_lengths[i] = m_lengths[i - 1] + std::hypot(m_points[i][0] - m_points[i - 1][0], m_points[i][1] - m_points[i - 1][1]);
    }
    m_length = m_lengths.empty() ? 0.0 : m_lengths.back();
}

std::array<double, 3> TextPathWalker::at(double distance) const {
    if (m_points.empty()) {
        return {0.0, 0.0, 0.0};
    }
    if (m_points.size() == 1 || m_length <= 0.0) {
        return {m_points[0][0], m_points[0][1], 0.0};
    }
    const auto direction = [&](std::size_t i) { // of the segment from point i to i + 1
        return std::atan2(m_points[i + 1][1] - m_points[i][1], m_points[i + 1][0] - m_points[i][0]) * 180.0 / std::numbers::pi;
    };
    if (m_closed) {
        distance = std::fmod(distance, m_length);
        if (distance < 0.0) {
            distance += m_length;
        }
    } else if (distance < 0.0 || distance > m_length) {
        // Past an end: straight on along the end's direction.
        const bool start = distance < 0.0;
        const std::size_t i = start ? 0 : m_points.size() - 2;
        const double a = direction(i) * std::numbers::pi / 180.0;
        const auto& p = start ? m_points.front() : m_points.back();
        const double d = start ? distance : distance - m_length;
        return {p[0] + d * std::cos(a), p[1] + d * std::sin(a), direction(i)};
    }
    const auto it = std::upper_bound(m_lengths.begin(), m_lengths.end(), distance);
    std::size_t i = it == m_lengths.begin() ? 0 : static_cast<std::size_t>(it - m_lengths.begin()) - 1;
    i = std::min(i, m_points.size() - 2);
    const double span = m_lengths[i + 1] - m_lengths[i];
    const double f = span > 0.0 ? (distance - m_lengths[i]) / span : 0.0;
    return {m_points[i][0] + f * (m_points[i + 1][0] - m_points[i][0]), m_points[i][1] + f * (m_points[i + 1][1] - m_points[i][1]),
            direction(i)};
}

std::array<double, 2> TextPathWalker::lineStart(double width, TextAlign align, double firstMargin, double lastMargin,
                                                bool forceAlignment) const {
    const double available = m_length - firstMargin - lastMargin;
    if ((forceAlignment || align == TextAlign::Justify) && width > 0.0 && available > 0.0) {
        return {firstMargin, available / width};
    }
    switch (align) {
    case TextAlign::Center:
        return {firstMargin + (available - width) / 2.0, 1.0};
    case TextAlign::Right:
        return {m_length - lastMargin - width, 1.0};
    case TextAlign::Left:
    case TextAlign::Justify:
        break;
    }
    return {firstMargin, 1.0};
}

} // namespace wabcut
