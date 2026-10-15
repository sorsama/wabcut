#pragma once

// Evaluating a graphic at a time (C.13, docs/internal/c13-graphics-handoff.md): the draw list the renderer rasterises.
//
// resolveGraphic() flattens a graphic's layer tree at one graphic time into items, bottom to top, each with everything
// resolved: the transform from the layer's own pixels to the canvas (groups composed in), the opacity (groups
// multiplied in), blend mode, drop shadow, masks as outlines in canvas pixels (a group's masks apply to its children),
// and the content:
//   shape   the outline as a closed or open cubic Bezier path (rectangles with rounded corners, ellipses, polygons,
//           stars, keyframed paths), the fill (solid or gradient) and stroke with brand colours resolved, trim range;
//   text    the runs with brand fonts and colours resolved, alignment and box, and - when text-range animators are
//           active - every grapheme cluster with the opacity, offset, scale and rotation they give it;
//   image   the media (a brand logo resolved), the box and the fit.
// Pinned layers (LayerPin) follow their target's edges: bounds of text come from a TextMeasurer (the renderer's real
// one; approximateTextMeasurer() otherwise). The renderer lays text out (shaping, line breaking - Thai through the
// host's LineBreaker) and applies each cluster's adjustments around the cluster's own centre after layout.
//
// Graphic time: keys of a graphic are in the clip's curve time (Clip::curveTimeAt), except in a graphic with
// responsive time, where they are in design time (responsiveDesignTime). graphicTimeAt() picks the right one.

#include <wabcut/model/CaptionText.h>
#include <wabcut/model/Clip.h>
#include <wabcut/model/Graphics.h>
#include <wabcut/model/Masks.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wabcut {

struct ProjectState;

/// (x, y) -> (a x + c y + e, b x + d y + f), like QTransform(m11 = a, m12 = b, m21 = c, m22 = d, dx = e, dy = f).
struct Affine2D {
    double a = 1.0;
    double b = 0.0;
    double c = 0.0;
    double d = 1.0;
    double e = 0.0;
    double f = 0.0;

    /// This transform applied after `inner`: p -> this(inner(p)).
    Affine2D operator*(const Affine2D& inner) const;
    std::array<double, 2> map(double x, double y) const;
    bool operator==(const Affine2D&) const = default;
};

using Rgba = std::array<double, 4>;

/// An axis-aligned box: x, y the top-left corner.
struct Box {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    bool operator==(const Box&) const = default;
};

/// Measures a text block in the layer's own pixels: the box from (0, 0) at the block's top-left (left-aligned), its
/// top-centre (centred) or its top-right (right-aligned).
class TextMeasurer {
public:
    virtual ~TextMeasurer() = default;
    /// Width and height of the laid-out block (paragraph text: boxWidth wide). `fonts` are the runs' resolved families.
    virtual std::array<double, 2> measure(const TextContent& text, const std::vector<std::string>& fonts) const = 0;
};

/// 0.55 em per display character (combining marks take none), plus tracking; lines 1.2 em (or the leading); paragraph
/// text wraps at the break opportunities of the simple line breaker. Good enough for pins and wipes without fonts.
const TextMeasurer& approximateTextMeasurer();

struct DrawPaint {
    PaintKind kind = PaintKind::Solid;
    Rgba color{1.0, 1.0, 1.0, 1.0};
    std::vector<std::pair<double, Rgba>> stops;
    std::array<double, 2> start{0.0, 0.0};
    std::array<double, 2> end{100.0, 0.0};

    bool operator==(const DrawPaint&) const = default;
};

struct DrawStroke {
    bool enabled = false;
    Rgba color{0.0, 0.0, 0.0, 1.0};
    double width = 0.0;
    std::vector<double> dashes;
    LineJoin join = LineJoin::Miter;
    LineCap cap = LineCap::Butt;

    bool operator==(const DrawStroke&) const = default;
};

struct DrawShadow {
    bool enabled = false;
    Rgba color{0.0, 0.0, 0.0, 1.0};
    double opacity = 0.0;
    double angle = 0.0;
    double distance = 0.0;
    double softness = 0.0;

    bool operator==(const DrawShadow&) const = default;
};

/// A mask outline in canvas pixels (closed cubic Bezier). The masks of one `owner` (a layer) combine in list order into
/// one matte, like Masks.h's (the first Add starts from nothing, a first Subtract or Intersect from everything); the
/// mattes of different owners - the layer's own and each enclosing group's - multiply.
struct DrawMask {
    std::uint32_t owner = 0; ///< the layer the mask belongs to
    std::vector<MaskVertex> outline;
    MaskMode mode = MaskMode::Add;
    bool inverted = false;
    double feather = 0.0;   ///< pixels
    double expansion = 0.0; ///< pixels
    double opacity = 1.0;

    bool operator==(const DrawMask&) const = default;
};

struct DrawTextRun {
    std::size_t byteStart = 0; ///< where the run starts in the layer's text
    std::string text;
    std::string fontFamily; ///< brand font resolved; empty = the platform's sans-serif
    std::string fontStyle;
    std::int32_t fontWeight = 400;
    double fontSize = 72.0;
    Rgba fill{1.0, 1.0, 1.0, 1.0};
    bool italic = false;
    bool underline = false;
    double tracking = 0.0;
    double leading = 0.0;
    double baselineShift = 0.0;

    bool operator==(const DrawTextRun&) const = default;
};

/// What the text-range animators do to one grapheme cluster ([byteStart, byteEnd) of the layer's text).
struct DrawCluster {
    std::size_t byteStart = 0;
    std::size_t byteEnd = 0;
    double opacity = 1.0;  ///< multiplier
    double offsetX = 0.0;  ///< pixels
    double offsetY = 0.0;
    double scale = 1.0;    ///< multiplier, about the cluster's centre
    double rotation = 0.0; ///< degrees, about the cluster's centre

    bool operator==(const DrawCluster&) const = default;
};

struct DrawText {
    std::vector<DrawTextRun> runs;
    TextAlign align = TextAlign::Left;
    double boxWidth = 0.0;
    double boxHeight = 0.0;
    std::string language;
    bool background = false;
    Rgba backgroundColor{0.0, 0.0, 0.0, 0.0};
    double backgroundPadding = 0.0;
    double backgroundRadius = 0.0;
    /// Every cluster of the text (newlines excepted) when an animator is active; empty otherwise.
    std::vector<DrawCluster> clusters;

    bool operator==(const DrawText&) const = default;
};

struct DrawImage {
    MediaId media;
    double width = 0.0;
    double height = 0.0;
    ImageFit fit = ImageFit::Contain;

    bool operator==(const DrawImage&) const = default;
};

enum class DrawKind : std::uint8_t { Shape, Text, Image };

struct DrawItem {
    std::uint32_t layer = 0; ///< the layer it draws
    DrawKind kind = DrawKind::Shape;
    Affine2D transform;      ///< the layer's pixels -> canvas pixels
    double opacity = 1.0;    ///< 0..1, ancestors included
    BlendMode blend = BlendMode::Normal;
    DrawShadow shadow;
    std::vector<DrawMask> masks; ///< the layer's, then its groups' innermost first (canvas pixels)
    Box bounds;                  ///< the content's box in the layer's own pixels
    // Shape
    std::vector<MaskVertex> path; ///< layer pixels
    bool closed = true;
    DrawPaint fill;
    DrawStroke stroke;
    double trimStart = 0.0; ///< 0..1
    double trimEnd = 1.0;
    double trimOffset = 0.0; ///< degrees
    // Text
    DrawText text;
    // Image
    DrawImage image;

    bool operator==(const DrawItem&) const = default;
};

struct DrawList {
    std::int32_t width = 0; ///< the design canvas
    std::int32_t height = 0;
    std::vector<DrawItem> items; ///< bottom to top

    bool operator==(const DrawList&) const = default;
};

struct ResolveOptions {
    const BrandKit* brandKit = nullptr;
    const TextMeasurer* measurer = nullptr;  ///< null = approximateTextMeasurer()
    const LineBreaker* lineBreaker = nullptr; ///< word units; null = simpleLineBreaker()
};

/// The graphic at graphic time `time`.
DrawList resolveGraphic(const Graphic& graphic, Ticks time, const ResolveOptions& options = {});

/// Responsive time: the design time shown `sinceStart` into a clip `clipDuration` long (ResponsiveTime).
Ticks responsiveDesignTime(const ResponsiveTime& responsive, Ticks sinceStart, Ticks clipDuration);
/// The graphic time a graphic clip shows at timeline time `t`.
Ticks graphicTimeAt(const Clip& clip, const Graphic& graphic, Ticks t);
/// The clip's graphic at timeline time `t`, with the project's brand kit (unless `options` names another).
DrawList resolveGraphicClip(const ProjectState& state, const Clip& clip, Ticks t, ResolveOptions options = {});

/// The content box of `layer` in its own pixels at graphic time `time` (groups: their visible children's, transformed).
Box layerBounds(const GraphicLayer& layer, Ticks time, const ResolveOptions& options = {});

/// The units a text-range animator steps over in `text` (TextUnit): byte ranges in order.
std::vector<std::pair<std::size_t, std::size_t>> textUnits(std::string_view text, TextUnit unit, std::string_view language,
                                                           const LineBreaker& breaker);

/// How much `animator` selects unit `index` of `count` at graphic time `time` (0..1).
double textSelection(const TextAnimator& animator, std::size_t index, std::size_t count, Ticks time);

} // namespace wabcut
