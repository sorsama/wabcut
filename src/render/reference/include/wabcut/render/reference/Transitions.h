#pragma once

// Reference video transitions (0.2: Premiere's transition library, docs/graphics.md "Transitions"): the CPU twin of
// shaders/fx_transition.frag, on linear-light premultiplied pictures at preview divisor 1.
//
// Positions are sequence pixels with pixel centres at +0.5 (y down); pictures are sampled bilinearly with the edge
// repeated (sampleBilinear), as the compositor's textures are. A and B have the same size; a missing side is transparent
// (a one-sided transition).
//
// Patterns are signed fields f(P, t) in pixels, B where f > 0:
//   coverage(f) = clamp(f / aa + 0.5, 0, 1), aa = antiAliasWidth(quality)
//   inB = coverage(f), inBorder = borderWidth > 0 ? coverage(f + borderWidth) - inB : 0
//   out = A (1 - inB - inBorder) + B inB + (borderColor, 1) inBorder
// Reverse: f(P, t) -> -f(P, 1 - t). Every pattern field is shifted by m (2t - 1), m = aa / 2 + borderWidth, so t = 0
// shows only A and t = 1 only B.
//
// Fields, with S = the frame size, C = center * S, H = max(C, S - C) (half sizes reaching the farthest edges), d = |P - C|:
//   Iris Round     t |H| - |P - C|
//   Iris Box       min(t Hx - dx, t Hy - dy)
//   Iris Diamond   (t - dx / 2Hx - dy / 2Hy) n, n = 1 / sqrt(1/(2Hx)^2 + 1/(2Hy)^2)
//   Iris Cross     max(min(r Hx - dx, t Hy - dy), min(r Hy - dy, t Hx - dx)), r = min(1, 2t)
//   Clock Wipe     sector [0, 2 pi t) clockwise from the Direction ("From North" = up) around the frame's centre:
//                  r * (sweep <= pi ? min(a, sweep - a) : max(a, sweep - 2 pi - a)), a = the clockwise angle in (-pi, pi]
//   Pinwheel       N wedges (custom[0]) around C from up: local = a mod w (w = 2 pi / N), sweep = t w:
//                  r (local < sweep ? min(local, sweep - local) : -min(local - sweep, w - local))
//   Wedge Wipe     r (t pi - |a|), a from the Direction around the frame's centre
//   Radial Wipe    r (t pi / 2 - a) around a corner (Direction: NW, NE, SE, SW), a clockwise from the frame edge there
//   Inset          min(t Sx - |Px - Kx|, t Sy - |Py - Ky|), K = the corner
//   Barn Doors     t Sx / 2 - |Px - Sx / 2| (Horizontal) or the same on y (Vertical)
//   Band Wipe      N bands (custom[0]) across the travel: even bands t L - along, odd along - (1 - t) L
//   Venetian Blinds  N bands (custom[0]): t size - local, local measured from each band's start in the Direction
//   Checker Wipe   cells custom[0] x custom[1]; in a cell, local u (0..1) along the Direction: (q - u) cell length,
//                  q = 1.5 t (even cells) or 1.5 t - 0.5 (odd cells)
//   CheckerBoard   the same with q = clamp(2t) (even) or clamp(2t - 1) (odd)
//   Random Blocks  cells custom[0] x custom[1]; B once t > hash01(i, j, 1)
//   Random Wipe    18 rows of square-ish cells; B once t > (index along the Direction + 4 hash01(i, j, 2)) / (count + 4)
//   Spiral Boxes   cells custom[0] x custom[1] in clockwise spiral order from the top-left (outside in), each wiped
//                  along the spiral: (t N - index - u) cell length
//   Zig-Zag Blocks rows (West to East, East to West) or columns (North to South, South to North) back and forth, each
//                  cell wiped along its row: (t N - index - u) cell length
//   Paint Splatter 28 splats (centre, radius 0.12..0.32 of the diagonal, start time 0..0.6 from hash01(i, k, 3), edge
//                  wobble 1 + 0.18 sin(7 angle + 2 pi hash01(i, 4, 3))) growing to full size by t = 1; max with a
//                  flood that covers everything from t = 0.8 to 1
//   Wipe           (I.2) a straight edge at `angleDegrees`: e - P.dir, e = s0 - w / 2 + t (len + w); with a feather
//                  (w > 0) the ramp is w wide instead of aa and only the border width shifts it
// Moving pictures (the edge mixed with coverage and the border as above):
//   Push           B pushes A along the Direction (Reverse: the other way): A at P - v t L, B at P - v t L + v L,
//                  the edge (no border) shifted by aa / 2 (2t - 1)
//   Slide          B's frame moves in along the Direction (8 ways): B at P - (t - 1) v S where inside its frame;
//                  Reverse: A's frame moves out, over B
//   Split          A's halves (Horizontal: left and right; Vertical) move apart by t S / 2 each
//   Center Split   A's quarters move to the corners by t S / 2 each
//   Band Slide     N bands of B (custom[0]) slide in from alternate ends
//   Whip           Push with eased travel e = t^2 (3 - 2t) and a 24-tap motion blur of (Blur / 100) 6t(1-t) L / 4 px
//   Cross Zoom, Zoom Blur  A zooms (scale 1 + 3 z^2 in, 1 / (1 + 3 z^2) out, z = 2t) about C with a 16-tap radial blur
//                  of 0.15 z (x Blur / 50 for Zoom Blur), B mirrors it (z = 2 (1 - t)), crossfaded by
//                  smoothstep(0.45, 0.55, t); zoom-outs sample the picture mirrored beyond its edges
//   Page Turn, Page Peel  the page (A) folds along a line moving from the corner (Direction) across the diagonal: B
//                  where tau < c (tau = distance from the corner along the diagonal, c = t (|S| + aa) - aa / 2), the flap (the page
//                  mirrored across the fold) over A where it lands inside the frame; Page Turn's flap is the picture at
//                  80 %, Page Peel's is shaded paper (0.72 x 0.55..1 across the curl) and B is shadowed near the fold
//                  (1 - 0.45 (1 - t) exp(-(c - tau) / 0.03 |S|)).
//                  Reverse: B is the page, peeling back over A (t -> 1 - t)
// Dissolves and the rest:
//   Cross Dissolve mix(A, B, t)       Dip to Color  t < 1/2 ? mix(A, colour, 2t) : mix(colour, B, 2t - 1)
//   Additive       A wa + B wb (alpha clamped to 1), wa = min(1, 2 (1 - t)), wb = min(1, 2t)
//   Non-Additive   max(A wa, B wb) per channel
//   Gradient Wipe  g = the ramp (custom[0]: x / Sx, y / Sy, |P - S/2| / |S/2|, (x / Sx + y / Sy) / 2), s = Softness
//                  (custom[1]) / 100: m = s > 0 ? clamp((t (1 + s) - g) / s) : coverage((t - g) L + m (2t - 1)); Reverse
//                  m = 1 - m(1 - t)
//   Light Leak     mix(A, B, smoothstep(0.35, 0.65, t)) plus 3 moving Gaussian glows of the colour, intensity
//                  custom[0] sin(pi t)^1.5, added where the picture is
//   Flash          (t < 1/2 ? A : B) brightened 1 + 3f, mixed to the colour by f^2, f = clamp(custom[0] (1 - |2t - 1|)^2)
//   Glitch         per 1/24 of the transition: bands jump sideways, the channels split by Color Split (custom[1]) x g,
//                  cells flicker to the other clip, g = Intensity (custom[0]) / 100 sin(pi t)^2; the cut at t = 1/2

#include <wabcut/render/reference/Image.h>

#include <array>
#include <cstdint>

namespace wabcut::render::reference {

/// The transitions, in render::TransitionKind's order (the values are the same).
enum class TransitionPattern : int {
    CrossDissolve,
    DipToColor,
    Wipe,
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
};

/// What a transition reads (render::LayerTransition's fields).
struct TransitionSpec {
    TransitionPattern pattern = TransitionPattern::CrossDissolve;
    double progress = 0.0;               ///< after Start/End
    Rgb color{};                         ///< linear: dips, Light Leak, Flash
    double angleDegrees = 0.0;           ///< Wipe
    double feather = 0.0;                ///< Wipe: a fraction of the travel
    double borderWidth = 0.0;            ///< px
    Rgb borderColor{};                   ///< linear
    bool reverse = false;
    int antiAliasing = 0;                ///< 0 Off .. 3 High
    std::array<double, 2> center{0.5, 0.5};
    int direction = 0;
    std::array<double, 4> custom{};
    std::uint32_t seed = 0;
};

/// The edge's coverage ramp in sequence pixels: max(divisor, {1, 2, 3, 5}[quality]) (quality clamped to 0..3).
double antiAliasWidth(int quality, int divisor = 1);

/// The 32-bit integer hash of the random patterns: x ^= x >> 16, x *= 0x7feb352d, x ^= x >> 15, x *= 0x846ca68b,
/// x ^= x >> 16 (wrapping).
std::uint32_t transitionHash(std::uint32_t x);
/// A value in (0, 1): ((h & 1023) + 0.5) / 1024, h = hash(i + hash(j + hash((seed & 0xFFFFFF) + salt 0x9E3779B9))).
double transitionHash01(int i, int j, int salt, std::uint32_t seed);

/// The transition of `a` and `b` (null = transparent; at least one is set). The result has their size.
Image transition(const Image* a, const Image* b, const TransitionSpec& spec);

} // namespace wabcut::render::reference
