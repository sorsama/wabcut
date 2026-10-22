#pragma once

// Reference video effects of Premiere's library (0.2, docs/effects.md): the CPU twins of the compositor's family shaders
// (shaders/fx_color.frag, fx_distort.frag, fx_stylize.frag, fx_generate.frag), on linear-light premultiplied pictures of
// a layer that fills the canvas with an identity transform (frame pixel = canvas pixel) at preview divisor 1.
//
// `params` are the effect's parameters in render/EffectLibrary.h order (the registry's units: user colours
// display-referred 0..1, lengths in pixels, angles in degrees, points as fractions of the frame); params[31] is the
// curve time in seconds for the effects that animate by themselves.
//
// The Color family works on the straight colour encoded with the BT.709 curve (toEncoded; e = 0 where alpha is 0) and
// writes back toLinear(e') * alpha'. Luma is Rec. 709 (luma()); HLS has hue in turns; user colours are taken as encoded
// values. Per op (q = params):
//   Brightness & Contrast  k = (1 + c) / (1 - 0.99 c), c = q1 / 100: e' = clamp((e + q0 / 200 - 0.5) k + 0.5)
//   Levels         per channel L(v; black in, white in, black out, white out, gamma) with its (R), (G), (B) row (q5..q19),
//                  then the (RGB) row (q0..q4): x = clamp((v - bi) / (wi - bi)) (a step at bi when wi <= bi),
//                  out = bo + (wo - bo) x^(100 / gamma), levels / 255
//   ProcAmp        Y' = Y q1 / 100 + q0 / 200, (Cb, Cr) rotated by q2 degrees and scaled by q3 / 100 (BT.709 Y'CbCr);
//                  Split Screen (q4) leaves frame x >= q5 * width unprocessed
//   Extract        grey m: Y = 255 luma, s = q2 / 100 * 64: s > 0 ? clamp((Y - q0 + s) / s) clamp((q1 + s - Y) / s)
//                  : (q0 <= Y <= q1); Invert (q3): 1 - m
//   Invert         by Channel (q0): RGB 1 - e; R, G, B one channel; HLS (h + 1/2, 1 - l, 1 - s) or one of them; YIQ
//                  (1 - Y, -I, -Q) or one of them (NTSC matrices); Alpha 1 - a; then mixed with the original by q1
//   Tint           mix(e, mix(black q0..2, white q3..5, luma(e)), q6)
//   Change Color   d = distance to q4..6 (RGB: |e - k| / sqrt 3; Hue: 2 |hue delta|; Chroma: |CbCr - CbCr k|), mask
//                  m = 1 - smoothstep(q7, q7 + q8 + 1e-5, d) (inverted by q10); View q0: grey m; else hue += q1 / 360 m,
//                  lightness += q2 / 100 m, saturation += q3 / 100 m (clamped)
//   Change to Color  m = product over hue, lightness, saturation of 1 - smoothstep(tol, tol + q11 / 2 + 1e-5, |delta|)
//                  against From; Setting: hue (and lightness / saturation by Change) moves to To's by m; Transforming:
//                  they move by (To - From) m; View Correction Matte: grey m
//   Leave Color    keep = 1 - smoothstep(q4, q4 + q5 + 1e-5, d (RGB or Hue)), e' = mix(e, luma, q0 (1 - keep))
//   Color Balance  weights ws = 1 - smoothstep(0, 1/2, L), wh = smoothstep(1/2, 1, L), wm = 1 - ws - wh;
//                  e' = clamp(e + 0.3 (ws S + wm M + wh H) / 100); Preserve Luminosity adds L - luma(e')
//   Video Limiter  on the unclamped encoded values (the BT.709 curve extended above 1): level = 1 + Clip Level / 100,
//                  knee = level (1 - k), k = Compression (0, 3, 5, 10, 20 %): above the knee
//                  knee + (level - knee)(1 - exp(-(v - knee) / (level - knee))), else min(v, level); Gamut Warning paints
//                  limited pixels in the warning colour
//   Black & White  luma(e)       Gamma Correction  e^(q0 / 10)       Solarize  e > q0 ? 1 - e : e
//   Color Pass     keep = 1 - smoothstep(q3 / 100, q3 / 100 + 0.004, RGB distance to q0..2), Reverse flips it:
//                  mix(luma, e, keep)
//   Color Replace  m as Color Pass's keep with q6; e' = mix(e, Solid ? replace : SetLum(replace, luma(e)), m)
//   Posterize      min(floor(e L), L - 1) / (L - 1), L = max(2, round(q0))
//   Threshold      255 luma >= q0 ? 1 : 0
//   Strobe Light   while on (q10, set by the evaluator): Makes Layer Transparent: alpha q3 a; else the operator q8
//                  (Copy, Add, Subtract, Multiply, Difference, bitwise And / Or / Xor on 8-bit values, Lighten / Darken
//                  by whole-colour luma, Minimum, Maximum, Screen) with the colour, mixed with the original by q3
//   Alpha Adjust   a' = (Ignore ? 1 : a), inverted by Invert, times Opacity; Mask Only: grey toLinear(a'), opaque
//   Luma Key       a' = a (q0 == q1 ? luma >= q0 : clamp((luma - q1) / (q0 - q1)))
//   Noise          n = hash01(x + 65536 y, round(1000 time), channel or 0) per pixel, e' = e + (n - 1/2) q0, clipped
//                  (q2) or wrapped
//
// The Distort family (LibraryDistort.cpp) resamples the layer: for an output frame position F it finds the source
// position S and samples the picture bilinearly there, transparent outside the frame with one pixel of coverage at its
// edge (frameCoverage). Corner Pin inverts the square-to-quad homography (Heckbert) and anti-aliases the quad's edges by
// finite differences; Lens Distortion scales the offset from its (decentred) centre by 1 - 0.6 curvature r^2 plus the
// prisms' r^2 shifts; Twirl rotates by angle (1 - r / R)^2; Spherize maps r to asin(r) / (r pi / 2); Wave Warp displaces
// across the wave's travel by height x wave(position / width 2 pi - speed time 2 pi + phase), times the pinning weight;
// Turbulent Displace uses fractal value noise (fbm: floor(Complexity) octaves and a fractional one, Evolution as the
// third coordinate, wrapping with Cycle Evolution); Transform inverts anchor / position / scale / skew / rotation (16
// samples over the shutter for motion blur when the evaluator set its end, params[23]); Basic 3D intersects the eye's ray
// with the turned plane (focal length 1.6 x the larger side); the blurs sample the canvas (Directional: up to 97 taps along
// the direction, 0 degrees vertical; Camera: 64 golden-angle taps over a disc of up to 40 px).
// The Stylize family (LibraryStylize.cpp) reads neighbourhoods, some after a Gaussian blur as the compositor's passes:
// Unsharp Mask (sigma = Radius), Alpha Glow (Glow / 2), Bevel Alpha (Edge Thickness), Roughen Edges (Border / 2), Radial
// Shadow (Softness / 3, on its projected shadow), Color Key (matte, then a disc erosion or dilation of |Edge Thin|, then
// Edge Feather as sigma). Median and Dust & Scratches take the median by luma of 3 x 3 samples Radius apart. Convolution
// Kernel weighs the encoded colour of the 3 x 3 neighbours one canvas pixel apart: sum M / Scale + Offset / 255 (Process
// Alpha: the alpha too).
// The Generate family (LibraryGenerate.cpp) draws in frame pixels: gradients and shapes over or instead of the layer
// (Magnify's blending modes plus Stencil and Silhouette Alpha), the lens flare added, and the Transition bin's wipes
// (field > 0 where wiped, softened by Feather, shifted so completion 0 keeps all and 1 nothing). The second-picture
// effects combine the layer with `second`.

#include <wabcut/render/reference/Image.h>

#include <array>
#include <cstdint>

namespace wabcut::render::reference {

/// The library's effects, with render::LibraryOp's values.
enum class LibraryEffect : int {
    BrightnessContrast = 1,
    Levels = 2,
    ProcAmp = 3,
    Extract = 4,
    Invert = 5,
    Tint = 6,
    ChangeColor = 7,
    ChangeToColor = 8,
    LeaveColor = 9,
    ColorBalance = 10,
    VideoLimiter = 11,
    BlackWhite = 12,
    ColorPass = 13,
    ColorReplace = 14,
    GammaCorrection = 15,
    Posterize = 16,
    Solarize = 17,
    Threshold = 18,
    StrobeLight = 19,
    AlphaAdjust = 20,
    LumaKey = 21,
    Noise = 22,
    // Distort (LibraryDistort.cpp).
    CornerPin = 40,
    LensDistortion = 41,
    Magnify = 42,
    Mirror = 43,
    Offset = 44,
    Spherize = 45,
    Twirl = 46,
    WaveWarp = 47,
    TurbulentDisplace = 48,
    Transform = 49,
    HorizontalFlip = 50,
    VerticalFlip = 51,
    EdgeFeather = 52,
    Replicate = 53,
    Basic3D = 54,
    BevelEdges = 55,
    DirectionalBlur = 56,
    CameraBlur = 57,
    // Stylize (LibraryStylize.cpp).
    UnsharpMask = 70,
    Emboss = 71,
    ColorEmboss = 72,
    FindEdges = 73,
    Median = 74,
    DustScratches = 75,
    AlphaGlow = 76,
    BevelAlpha = 77,
    RadialShadow = 78,
    RoughenEdges = 79,
    ColorKey = 80,
    ConvolutionKernel = 81,
    // Generate (LibraryGenerate.cpp).
    FourColorGradient = 100,
    Ramp = 101,
    Circle = 102,
    Grid = 103,
    Checkerboard = 104,
    LensFlare = 105,
    BlockDissolve = 106,
    GradientWipe = 107,
    LinearWipe = 108,
    RadialWipe = 109,
    VenetianBlinds = 110,
    Echo = 111,
    TextOverlay = 112,
    DifferenceMatte = 113,
    ImageMatteKey = 114,
};

using LibraryParams = std::array<double, 32>;

/// The hash of the library's random effects (Noise): transitionHash (Transitions.h) chained over three words,
/// h(a + h(b + h(c))) >> 8, times 2^-24: a value in [0, 1).
double libraryHash01(std::uint32_t a, std::uint32_t b, std::uint32_t c);

/// One library effect on `in`; `second` is its second picture (LayerEffect::matte composited on the canvas: echoes, text,
/// a gradient or difference layer, a matte image), null when it has none.
Image libraryEffect(const Image& in, LibraryEffect effect, const LibraryParams& params, const Image* second = nullptr);

} // namespace wabcut::render::reference
