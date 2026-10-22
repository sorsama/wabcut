#pragma once

// PreparedComposite: one fully evaluated picture request (contract rev 2, "playback"/"render").
//
// The scheduler builds it from a project snapshot at a target time (every layer's frame already decoded) and
// publishes it, immutable, through a CompositeMailbox; the monitor's render thread and the export renderer
// feed it to the Compositor. It holds no Qt types, so engine code can build it without touching the GPU.

#include <wabcut/color/Grade.h>
#include <wabcut/media/FrameHandle.h>
#include <wabcut/model/Blend.h>
#include <wabcut/model/Time.h>
#include <wabcut/motion/Flow.h>
#include <wabcut/render/ColorPipeline.h>
#include <wabcut/render/FrameView.h>
#include <wabcut/render/Geometry.h>
#include <wabcut/render/Mask.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace wabcut::render {

/// The media frame type a layer carries. media's rev-2 FrameHandle keeps `using VideoFrame = FrameHandle`,
/// and render reaches its payload only through frameViewOf() (FrameView.h), the single adapter.
using LayerFrame = media::VideoFrame;

/// Sequence canvas in pixels.
struct CanvasSize {
    int width = 1920;
    int height = 1080;

    constexpr bool isValid() const { return width > 0 && height > 0; }
    constexpr bool operator==(const CanvasSize&) const = default;
};

/// Premiere's Motion effect, in the units of wabcut::ClipProperties.
struct LayerTransform {
    /// Offset of the anchor point from the canvas centre, in sequence pixels (0,0 = centred).
    Vec2 position{};
    /// Uniform scale (1 = 100 %).
    double scale = 1.0;
    /// Clockwise rotation in degrees.
    double rotationDegrees = 0.0;
    /// Anchor point as an offset from the frame centre, in source pixels.
    Vec2 anchor{};

    constexpr bool operator==(const LayerTransform&) const = default;
};

/// The model's blend modes (C.15, model/Blend.h). The compositor draws Normal and Add with fixed-function blending and
/// every other mode in a shader over the canvas below the layer (0.1.2; CompositeStats::shaderBlends), as
/// render/reference/Blend.h.
using BlendMode = ::wabcut::BlendMode;

/// The video effects of the C.3 EffectRegistry the compositor implements (stack effects; Motion and Opacity are
/// the layer's transform and opacity). They run in canvas space after Motion/Opacity, in stack order, on linear
/// premultiplied RGBA (docs/architecture.md, "Evaluator obligations"). Lengths are sequence pixels; the
/// compositor scales them to its canvas resolution (preview divisor).
enum class EffectType : std::uint8_t {
    Crop,            ///< wabcut.crop: left, top, right, bottom (fractions of the layer's frame), feather (px)
    GaussianBlur,    ///< wabcut.blur.gaussian: radius (px; sigma = radius / 3), repeatEdges (0/1)
    BasicCorrection, ///< wabcut.color.basic: exposure (stops), contrast, saturation, temperature, tint
    Vignette,        ///< wabcut.vignette: amount (stops at the edge), midpoint, roundness, feather (0..100)
    Sharpen,         ///< wabcut.sharpen: amount (%), radius (px, the blur sigma)
    DropShadow,      ///< wabcut.dropShadow: opacity, angle (deg, 0 = up, clockwise), distance (px),
                     ///< softness (px, like the blur radius), red, green, blue (display-referred 0..1)
    Grade,           ///< wabcut.color.grade: everything is in LayerEffect::grade (shaders/grade.frag, I.5)
    OpacityMask,     ///< wabcut.mask: the layer's alpha times the coverage of LayerEffect::mask (I.6)
    Mosaic,          ///< wabcut.mosaic: cell size (a fraction of the layer's frame height)
    Border,          ///< wabcut.border (C.15): width (px), red, green, blue (display-referred 0..1), opacity: the layer
                     ///< over its alpha dilated by the width (a disc) in the colour - rounded boxes get rounded borders
    UltraKey,        ///< wabcut.key.ultra (C.15; GPU in 0.1.1): the 20 parameters in descriptor order - output, key
                     ///< colour r/g/b (encoded 0..1), transparency, highlight, shadow, tolerance, pedestal, choke, soften,
                     ///< contrast, midPoint, desaturate, range, spill, luma, saturation, hue (deg), luminance. Formulas:
                     ///< render/reference/UltraKey.h (choke and soften radii are fractions of the canvas width)
    TrackMatte,      ///< wabcut.key.trackMatte (C.15; GPU in 0.1.2): matteTrack (1-based), compositeUsing (0 alpha,
                     ///< 1 luma), reverse, then whether the matte track is valid (set by the evaluator). The layer, at
                     ///< its position in the stack, times m = alpha or clamp(Lum(premultiplied matte)), 1 - m reversed;
                     ///< LayerEffect::matte is the matte track's composite on this canvas (null: empty, m from
                     ///< transparent black). An invalid track shows nothing (docs/internal/c15-parity-handoff.md)
    Library,         ///< 0.2: an effect of Premiere's library (render/EffectLibrary.h): LayerEffect::op names it, its
                     ///< parameters are in the order EffectLibrary.h lists; one or a few passes of a family shader
};

struct PreparedComposite;

/// One evaluated stack effect: its type and parameters in the order listed above, already at the frame's
/// curve time.
struct LayerEffect {
    EffectType type = EffectType::Crop;
    /// EffectType::Library: which effect (render::LibraryOp, EffectLibrary.h).
    std::uint16_t op = 0;
    std::array<double, 32> params{}; ///< 32: Levels has the most
    /// EffectType::Grade: the grade at the frame - parameters, baked curves, LUTs (color/Grade.h). Shared, immutable.
    std::shared_ptr<const color::GradeSpec> grade;
    /// The effect's masks at the frame (I.6, Mask.h): its result is mixed into its input by their coverage (the
    /// OpacityMask multiplies alpha by it instead). Null = everywhere.
    std::shared_ptr<const MaskSpec> mask;
    /// The effect's second picture, composited on a canvas of the layer's level: EffectType::TrackMatte's matte track
    /// at this time (0.1.2); a library effect's layer of another track, image or text (0.2, EffectLibrary.h).
    std::shared_ptr<const PreparedComposite> matte;

    bool operator==(const LayerEffect&) const = default;
};

/// Views of the Color workspace on the program monitor (preview only: the scheduler sets them from the monitor's
/// settings; export never does). They change how graded layers look, not the grade.
struct ColorView {
    /// The HSL secondary's matte: 0 off, 1 grey, 2 colour on black, 3 colour on white.
    int maskView = 0;
    /// The clip whose grade shows its matte (0 = every graded layer).
    std::uint64_t maskClip = 0;
    /// Before/after: left of this fraction of the canvas width, graded layers show themselves before their grade;
    /// negative = off.
    double split = -1.0;

    bool active() const { return maskView != 0 || split >= 0.0; }
    bool operator==(const ColorView&) const = default;
};

/// A video transition around an edit point (C.3 Transition), evaluated at the frame. The library kinds (0.2) are
/// Premiere's transitions: shaders/fx_transition.frag, CPU twin render/reference/Transitions.h (the exact maths).
enum class TransitionKind : std::uint8_t {
    CrossDissolve, ///< linear-light blend of A and B (also Film Dissolve: a dissolve in linear light)
    DipToColor,    ///< A to `color` over the first half, `color` to B over the second
    Wipe,          ///< B revealed behind a straight edge at `angleDegrees` with `feather`
    // 0.2: Premiere's transition library.
    AdditiveDissolve,
    NonAdditiveDissolve,
    IrisBox,
    IrisCross,
    IrisDiamond,
    IrisRound,
    PagePeel,
    PageTurn,
    BandSlide,
    CenterSplit,
    Push,
    Slide,
    Split,
    Whip,
    BandWipe,
    BarnDoors,
    CheckerWipe,
    CheckerBoard,
    ClockWipe,
    GradientWipe,
    Inset,
    PaintSplatter,
    Pinwheel,
    RadialWipe,
    RandomBlocks,
    RandomWipe,
    SpiralBoxes,
    VenetianBlinds,
    WedgeWipe,
    ZigZagBlocks,
    CrossZoom,
    ZoomBlur,
    LightLeak,
    Flash,
    Glitch,
    // The library shader's pattern codes are the values above (fx_transition.frag, reference::TransitionPattern):
    // kinds outside the library come after them.
    FlowMorph, ///< P.2: A and B warped toward each other along `motion` and blended (shaders/flowmix.frag)
};

struct PreparedComposite;

struct LayerTransition {
    TransitionKind kind = TransitionKind::CrossDissolve;
    /// 0 = all outgoing (A), 1 = all incoming (B): (t - (cut - before)) / (before + after), already mapped through the
    /// transition's Start and End (start + (end - start) p).
    double progress = 0.0;
    /// DipToColor: the colour, linear Rec.709 (0 = black, 1 = white); LightLeak, Flash: their colour (linear).
    std::array<double, 3> color{};
    /// Wipe: direction of travel (0 = left to right, clockwise) and border feather as a fraction of the frame.
    double angleDegrees = 0.0;
    double feather = 0.0;
    // 0.2: Premiere's transition options (render/reference/Transitions.h says what each kind reads).
    /// Border Width in sequence pixels (0 = none) and Border Color (linear Rec.709): a band on A's side of the edge.
    double borderWidth = 0.0;
    std::array<double, 3> borderColor{};
    /// Reverse: B shows where the pattern at 1 - progress does not.
    bool reverse = false;
    /// Anti-aliasing Quality: 0 Off (one pixel of coverage), 1 Low, 2 Medium, 3 High.
    int antiAliasing = 0;
    /// Center (iris, pinwheel, zooms): fractions of the frame's width and height.
    std::array<double, 2> center{0.5, 0.5};
    /// The descriptor's Direction option (an index of its menu).
    int direction = 0;
    /// Per kind: bands, cells, wedges, gradient and softness, blur or intensity amounts (Transitions.h).
    std::array<double, 4> custom{};
    /// Random patterns (blocks, splatters, glitch): the transition's id, so the pattern stays the same every frame.
    std::uint32_t seed = 0;
    /// The layers of the group: 2 (A, then B right after it in the layer list) or 1 (a one-sided transition,
    /// mixed with transparent; `incomingOnly` says the layer is B).
    int layerCount = 2;
    bool incomingOnly = false;
    /// FlowMorph (P.2, docs/motion.md): the motion between A and B on this level's canvas (motion::FlowPair: forward and
    /// backward, fractions of the canvas), as Time Interpolation's Optical Flow (two frames of one clip) and Morph Cut
    /// (the two sides of a jump cut) analyse it; the mix is motion::interpolate at `progress`. Without it, or one-sided,
    /// a FlowMorph mixes as a cross dissolve. Immutable and shared.
    std::shared_ptr<const motion::FlowPair> motion;

    bool operator==(const LayerTransition&) const = default;
};

struct CompositeLayer {
    LayerFrame frame;
    /// Synthetic CPU pixels (generated mattes, titles, tests). When set, used instead of `frame`.
    std::optional<FrameView> view;
    /// A nested or multicam clip (C.8, docs/internal/c8-nested-multicam-handoff.md): the layer's picture is this composite of the
    /// nested sequence, rendered on its own canvas (`nested->canvas`; its colour pipeline and views are the parent's)
    /// and then placed, with the effects, opacity and transition below, exactly like a media frame of that size.
    /// When set, `frame` and `view` are ignored. A nested composite that cannot be rendered leaves the layer out, so
    /// the redactions inside it never fail open.
    std::shared_ptr<const PreparedComposite> nested;
    LayerTransform transform;
    /// P.2: a projective warp of the layer's own frame before its Motion, row-major 3 x 3 in fractions of the frame
    /// (x of its width, y of its height; top-left origin, y down): (x', y', w') = warp (x, y, 1), so it holds at any
    /// decode size. Identity = none. The stabilizers' compensation (Warp Stabilizer, Rolling Shutter Repair) of the frame
    /// inside their group. Only the layer draw honours it: effects, masks and redactions of a warped layer would see the
    /// unwarped placement, so the evaluator never gives one any of those.
    std::array<double, 9> warp{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    /// [0, 1]; multiplies the layer's (premultiplied) colour and alpha.
    double opacity = 1.0;
    BlendMode blend = BlendMode::Normal;
    /// P.2 (E5): Interpret Footage's alpha channel options for `frame`: ignore it (the picture is opaque) or invert it.
    bool ignoreAlpha = false;
    bool invertAlpha = false;
    /// 0.2: interlaced footage (model/FieldOptions.h): a FieldRender value - 0 the frame as it is, 1 keep the upper
    /// field (even rows) and interpolate the lower one's lines, 2 keep the lower field (odd rows), 3 Flicker Removal
    /// (each line blended [1 2 1] / 4 with its neighbours). Rows count in the frame's own pixels, before any scaling.
    std::uint8_t fieldMode = 0;
    /// Source-anchored redactions covering the layer's media at its frame (I.6), applied to the layer's own pixels
    /// before any of its effects. Not left out by anything but a per-clip exception (the model's Redaction).
    std::vector<LayerRedaction> redactions;
    /// The clip's stack effects, in order (disabled and bypassed ones already left out).
    std::vector<LayerEffect> effects;
    /// Set on the first layer of a transition group (see LayerTransition::layerCount).
    std::optional<LayerTransition> transition;
    /// Stack effects of the clip that were bypassed (unknown id, newer version, not a video effect): the UI
    /// shows a warning badge.
    int bypassedEffects = 0;
    /// Diagnostic only: the clip this layer came from (0 = none).
    std::uint64_t clipId = 0;
    /// Colour management (I.9, ColorPipeline v2): how the layer's code values are read into the working space - the
    /// clip's input colour space (or the camera log its media names), unset for Auto (the frame's colour tags). Direct
    /// Rec.709 converts only layers that have one (and HDR-tagged ones); Disable Color Management ignores it.
    std::optional<InputColorSpace> inputSpace;
    /// A display-referred graphic (a title, a burned-in caption, a colour matte): in an ACES project it is read through
    /// the inverse output transform, so it looks as designed; in HDR its white sits at the reference white.
    bool graphics = false;
};

struct PreparedComposite {
    /// Project revision the composite was evaluated from (diagnostics, cache keys).
    std::uint64_t revision = 0;
    /// Transport seek epoch it belongs to. Composites of an older epoch are stale (latest seek wins).
    std::uint64_t seekEpoch = 0;
    /// Sequence time shown: the start of the sequence frame (snapped to the frame grid).
    Ticks targetTime{};
    /// Length of one sequence frame (presentation bookkeeping).
    Ticks frameDuration{};
    CanvasSize canvas{};
    ColorPipeline color{};
    /// V1 (bottom) to Vn (top), already filtered to enabled tracks/clips. Empty = black.
    std::vector<CompositeLayer> layers;
    /// std::chrono::steady_clock nanoseconds when it was published (frame-age statistics). 0 = unknown.
    std::int64_t publishedNs = 0;
    /// Preview-only colour views (mask, before/after).
    ColorView colorView;

    /// Resets to an empty request but keeps the layer storage (pooled composites never reallocate).
    void reset() {
        revision = 0;
        seekEpoch = 0;
        targetTime = {};
        frameDuration = {};
        canvas = {};
        color = {};
        layers.clear();
        publishedNs = 0;
        colorView = {};
    }
};

// ---- Transform math ----

/// 2D affine map in pixel space (y down): x' = a*x + c*y + e, y' = b*x + d*y + f.
struct Affine2D {
    double a = 1.0, b = 0.0, c = 0.0, d = 1.0, e = 0.0, f = 0.0;

    constexpr Vec2 map(Vec2 p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
    /// this ∘ o (apply o first).
    constexpr Affine2D operator*(const Affine2D& o) const {
        return {a * o.a + c * o.b, b * o.a + d * o.b,     a * o.c + c * o.d,
                b * o.c + d * o.d, a * o.e + c * o.f + e, b * o.e + d * o.f + f};
    }
    constexpr double determinant() const { return a * d - b * c; }
    Affine2D inverted() const;
};

/// Maps a layer's source pixel coordinates (origin at the frame's top-left corner, y down, stored pixels) to
/// sequence canvas pixels (origin top-left, y down):
///   canvas = canvasCentre + position + R(rotation) * scale * (P * (src - frameCentre) - anchor)
/// where P stretches x by `pixelAspect` (anamorphic sources) and R rotates clockwise on screen.
Affine2D layerToCanvas(const LayerTransform& transform, double frameWidth, double frameHeight,
                       double pixelAspect, CanvasSize canvas);

} // namespace wabcut::render
