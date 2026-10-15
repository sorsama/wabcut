#pragma once

// Reference Ultra Key (C.15, wabcut.key.ultra, model/Effects.h): docs/internal/c15-parity-handoff.md, "Ultra Key (chroma key)
// reference". The key runs on the gamma-encoded picture (toEncoded(), the sequence's BT.709 encoding) in Rec. 709
// Y'CbCr; its outputs go back to linear premultiplied RGBA.

#include <wabcut/model/Effects.h>
#include <wabcut/model/Time.h>
#include <wabcut/render/reference/Image.h>

#include <array>
#include <cstdint>

namespace wabcut::render::reference {

enum class UltraKeyOutput : std::uint8_t { Composite = 0, AlphaChannel = 1, ColorChannel = 2 };

/// Every wabcut.key.ultra parameter with its descriptor default (Effects.cpp kUltraKeyParams). Sliders are 0..100 unless
/// noted. The key colour is encoded (0..1, the picture's gamma-encoded values, as an eyedropper picks it).
struct UltraKeyParams {
    UltraKeyOutput output = UltraKeyOutput::Composite; ///< "output": 0 composite, 1 alpha channel, 2 colour channel
    Rgb keyColor{0.0, 1.0, 0.0};                        ///< "keyColor.r/g/b"
    // Matte Generation
    double transparency = 45.0;
    double highlight = 10.0;
    double shadow = 50.0;
    double tolerance = 50.0;
    double pedestal = 10.0;
    // Matte Cleanup
    double choke = 0.0;
    double soften = 0.0;
    double contrast = 0.0;
    double midPoint = 50.0;
    // Spill Suppression
    double desaturate = 25.0;
    double range = 50.0;
    double spill = 50.0;
    double luma = 50.0;
    // Color Correction
    double saturation = 100.0; ///< 0..200, 100 = unchanged
    double hue = 0.0;          ///< degrees, -180..180
    double luminance = 100.0;  ///< 0..200, 100 = unchanged

    bool operator==(const UltraKeyParams&) const = default;
};

/// The parameters of an instance at a curve time (effectParameterAt: the instance's value, else the descriptor's
/// default). `output` is rounded and clamped to 0..2.
UltraKeyParams ultraKeyParams(const EffectInstance& instance, Ticks curveTime);

/// Rec. 709 Y'CbCr of an encoded colour (Cb and Cr in -0.5..0.5):
///   Y' = 0.2126 R + 0.7152 G + 0.0722 B,  Cb = (B - Y') / 1.8556,  Cr = (R - Y') / 1.5748
/// and back: R = Y' + 1.5748 Cr, B = Y' + 1.8556 Cb, G = (Y' - 0.2126 R - 0.0722 B) / 0.7152.
struct YCbCr {
    double y = 0.0;
    double cb = 0.0;
    double cr = 0.0;
};
YCbCr toYCbCr(const Rgb& encoded);
Rgb fromYCbCr(const YCbCr& c);

/// Step 1, distance of an encoded colour to the encoded key colour:
///   d  = |(Cb, Cr) - (Cbk, Crk)| / max(|(Cbk, Crk)|, 1e-6)    (0 at the key colour, 1 at neutral grey)
///   dl = Y' - Y'k                                              (luma difference)
struct KeyDistance {
    double d = 0.0;
    double dl = 0.0;
};
KeyDistance keyDistance(const Rgb& encoded, const Rgb& keyColor);

/// Step 2, the matte of one encoded colour before cleanup (1 = foreground), clamped to [0, 1] at the end:
///   t = tolerance / 100 * 0.5,  p = pedestal / 100 * 0.25,  w = 0.25 (1 - transparency / 100) + 0.02
///   c = smoothstep(t, t + w, d - p)                     smoothstep(e0, e1, x) = u^2 (3 - 2u), u = clamp((x-e0)/(e1-e0))
///   a = c + highlight / 100 * max(0, dl) * (1 - c)
///   a = a * (1 - shadow / 100 * max(0, -dl) * (1 - c))
/// (C.16: the shadow term is gated by the chroma matte c, so a foreground darker than the key colour stays opaque.)
double ultraKeyMatteAt(const Rgb& encoded, const UltraKeyParams& params);

/// Steps 4 and 5 on one encoded straight colour C (d from keyDistance of the ORIGINAL colour), result clamped to [0, 1]:
///   s     = spill / 100 * (1 - smoothstep(e, e + range / 100 * 0.5 + 0.05, d)),  e = t + p (where the matte rises)
///   K     = the key colour's largest channel (the first of equal largest, in R, G, B order); o1, o2 the others
///   C.K   = C.K - s * max(0, C.K - (C.o1 + C.o2) / 2)
///   Yc    = luma(C);  C = C + desaturate / 100 * s * (Yc - C)          (mix(C, Y, desaturate s): keeps the luma Yc)
///   C     = C + (luma / 100 - 0.5) * s * (Yoriginal - Yc)              (added to R, G and B: a pure luma change)
///   colour correction in Y'CbCr: Y' *= luminance / 100; (Cb, Cr) rotated by `hue` degrees
///   (Cb' = Cb cos h - Cr sin h, Cr' = Cb sin h + Cr cos h) and scaled by saturation / 100; back to R'G'B'.
Rgb ultraKeyColorAt(const Rgb& encoded, const UltraKeyParams& params);

/// The full key on a linear premultiplied picture. Per pixel: a_in = clamp(alpha, 0, 1), the straight colour
/// E = toEncoded(rgb / a_in) (0 where a_in = 0), m = ultraKeyMatteAt(E). Then on the whole matte, in order (W = width):
///   choke    erode m by a disc of radius rc = choke / 100 * 0.005 W pixels: m(p) = min m(q) over pixel centres with
///            |q - p| <= rc inside the picture (rc < 1: no change)
///   soften   Gaussian blur of m with sigma = soften / 100 * 0.005 W pixels: gaussianBlur()'s separable kernel, the
///            edges repeated (sigma < 0.05: no change)
///   contrast m = clamp((m - mp) (1 + contrast / 25) + mp, 0, 1), mp = midPoint / 100
/// Then C = ultraKeyColorAt(E), a = a_in * m, and the output:
///   Composite      (toLinear(C) a, a)
///   AlphaChannel   (g, g, g, 1), g = toLinear(a): the matte as an encoded grey, so its code values are the matte
///   ColorChannel   (toLinear(C), 1)
Image ultraKey(const Image& picture, const UltraKeyParams& params);

} // namespace wabcut::render::reference
