#pragma once

// Reference blend modes (C.15, model/Blend.h): docs/internal/c15-parity-handoff.md, "Blend modes" - W3C Compositing and Blending
// Level 1, source-over, on linear-light premultiplied RGBA.

#include <wabcut/model/Blend.h>
#include <wabcut/render/reference/Image.h>

#include <cstdint>

namespace wabcut::render::reference {

/// Whether a mode clamps Cb and Cs to [0, 1] before its blend function (the handoff's modes marked "clamped"): Color
/// Burn, Linear Burn, Screen, Color Dodge, Overlay, Soft Light, Hard Light, Vivid Light, Linear Light, Pin Light,
/// Hard Mix, Exclusion, and the four component modes (Hue, Saturation, Color, Luminosity: "on clamped linear RGB").
/// The others take any value as it is (HDR-safe).
bool blendClampsInputs(wabcut::BlendMode mode);

/// B(Cb, Cs) of a mode on un-premultiplied colours (Cb = backdrop, Cs = source), after the clamp above. Per channel:
///   Normal, Dissolve  Cs
///   Darken            min(Cb, Cs)                          Lighten         max(Cb, Cs)
///   Multiply          Cb Cs                                Screen          Cb + Cs - Cb Cs
///   Color Burn        Cb >= 1 ? 1 : Cs <= 0 ? 0 : 1 - min(1, (1 - Cb) / Cs)
///   Color Dodge       Cb <= 0 ? 0 : Cs >= 1 ? 1 : min(1, Cb / (1 - Cs))
///   Linear Burn       max(0, Cb + Cs - 1)                  Add             Cb + Cs (Linear Dodge, not clamped)
///   Overlay           HardLight(Cs, Cb) (arguments swapped)
///   Hard Light        Cs <= 0.5 ? Multiply(Cb, 2 Cs) : Screen(Cb, 2 Cs - 1)
///   Soft Light        Cs <= 0.5 ? Cb - (1 - 2 Cs) Cb (1 - Cb) : Cb + (2 Cs - 1) (D(Cb) - Cb),
///                     D(x) = x <= 0.25 ? ((16 x - 12) x + 4) x : sqrt(x)
///   Vivid Light       Cs <= 0.5 ? ColorBurn(Cb, 2 Cs) : ColorDodge(Cb, 2 Cs - 1)
///   Linear Light      clamp(Cb + 2 Cs - 1, 0, 1)
///   Pin Light         Cs <= 0.5 ? min(Cb, 2 Cs) : max(Cb, 2 Cs - 1)
///   Hard Mix          Cb + Cs >= 1 ? 1 : 0
///   Difference        |Cb - Cs|                            Exclusion       Cb + Cs - 2 Cb Cs
///   Subtract          max(0, Cb - Cs)
///   Divide            Cs == 0 ? (Cb == 0 ? 0 : 1) : min(1, Cb / Cs)   (the reference is SDR: clamped to 1)
/// Whole pixel (Lum = luma(), Rec. 709 weights):
///   Darker Color      Lum(Cs) < Lum(Cb) ? Cs : Cb          Lighter Color   Lum(Cs) > Lum(Cb) ? Cs : Cb   (ties: Cb)
///   Hue               SetLum(SetSat(Cs, Sat(Cb)), Lum(Cb))
///   Saturation        SetLum(SetSat(Cb, Sat(Cs)), Lum(Cb))
///   Color             SetLum(Cs, Lum(Cb))
///   Luminosity        SetLum(Cb, Lum(Cs))
/// with W3C's ClipColor, SetLum, SetSat and Sat = max - min (n and x of ClipColor taken once, before either branch).
Rgb blendFunction(wabcut::BlendMode mode, const Rgb& cb, const Rgb& cs);

/// splitmix64: the n-th output (0-based) of the splitmix64 generator whose state starts at `seed`:
///   z = seed + (n + 1) * 0x9E3779B97F4A7C15             (mod 2^64)
///   z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9
///   z = (z ^ (z >> 27)) * 0x94D049BB133111EB
///   return z ^ (z >> 31)
std::uint64_t splitmix64(std::uint64_t seed, std::uint64_t n);

/// Dissolve's per-pixel value in [0, 1): splitmix64(seed, n) >> 11, times 2^-53, with
/// n = y * width + x + frame * 2^32 (unsigned 64-bit arithmetic, wrapping; a negative frame wraps too).
double dissolveValue(std::uint64_t seed, int x, int y, int width, std::int64_t frame);

/// Composites `source` over `backdrop` with `mode` at `opacity` (clamped to [0, 1]); the result has the backdrop's size
/// and pixels outside the source count as transparent. Per pixel, with premultiplied inputs (cb, ab) and (cs, as), alpha
/// clamped to [0, 1]:
///   Cb = ab > 0 ? cb / ab : 0,  Cs = as > 0 ? cs / as : 0           (un-premultiplied)
///   as' = as * opacity
///   Cr  = (1 - ab) Cs + ab B(Cb, Cs)                                  (B = blendFunction; Cs here is not clamped)
///   co  = as' Cr + (1 - as') cb                                       (premultiplied result)
///   ao  = as' + ab (1 - as')
/// Dissolve: the source shows at full alpha, (Cs, 1), where dissolveValue(seed, x, y, width, frame) < as'; elsewhere the
/// backdrop is unchanged. `seed` is the clip id, `frame` the frame number (the pattern changes per frame).
Image blend(const Image& backdrop, const Image& source, wabcut::BlendMode mode, double opacity, std::uint64_t seed = 0,
            std::int64_t frame = 0);

} // namespace wabcut::render::reference
