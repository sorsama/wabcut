#pragma once

// CPU reference renders (C.15 test oracles, docs/internal/c15-parity-handoff.md): the picture type, the transfer curves, sampling,
// image comparison and the golden-file format every reference test uses.
//
// wabcut_render_reference is pure C++ (it links only wabcut_model: no Qt, no GPU, no FFmpeg) and builds in the core
// preset. The render owner's shaders are tested against it, so every function documents its exact math. Arithmetic is
// double precision; pictures store float.

#include <wabcut/model/Error.h>

#include <array>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::render::reference {

/// One pixel: R, G, B, A.
using Pixel = std::array<float, 4>;
/// A colour or pixel in double precision (R, G, B[, A]).
using Rgb = std::array<double, 3>;
using Rgba = std::array<double, 4>;

/// A picture: linear-light Rec. 709, premultiplied RGBA floats (4 per pixel), row-major, top row first - the
/// compositor's intermediates (docs/internal/c15-parity-handoff.md, "Working space"). Pixel (x, y) covers [x, x + 1) x [y, y + 1)
/// and its centre is (x + 0.5, y + 0.5), the convention of the compositor's textures and of every sample below.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<float> rgba;

    Image() = default;
    /// w x h transparent black (a negative size counts as 0).
    Image(int w, int h);
    /// w x h, every pixel `p`.
    static Image filled(int w, int h, const Pixel& p);

    bool empty() const { return width <= 0 || height <= 0; }
    std::size_t pixelCount() const;
    /// Offset of pixel (x, y)'s red in `rgba`. No bounds check.
    std::size_t index(int x, int y) const;
    Pixel at(int x, int y) const;
    /// The pixel in double precision.
    Rgba atD(int x, int y) const;
    void set(int x, int y, const Pixel& p);
    /// Stores a double-precision pixel (each value cast to float).
    void setD(int x, int y, const Rgba& p);
    float alpha(int x, int y) const { return rgba[index(x, y) + 3]; }

    bool operator==(const Image&) const = default;
};

// ---- Transfer curves ----------------------------------------------------------------------------------------------

/// The working space's encoding: the BT.709 OETF and its inverse, as ColorPipeline.cpp (bt709ToLinear / linearToBt709)
/// and the layer shader (layer.frag, TF_BT709) do, with a = 1.09929682680944 and b = 0.018053968510807. It is the
/// sequence's encoding: a Rec. 709 source is linearised with it and the default display and export transforms
/// (OutputTransform::Rec709) encode with it, so an encoded value passes through the compositor unchanged. Both clamp
/// their input to [0, 1] first.
///   toLinear(v)  = v < 4.5 b ? v / 4.5 : ((v + a - 1) / a)^(1 / 0.45)
///   toEncoded(l) = l < b ? 4.5 l : a l^0.45 - (a - 1)
float toLinear(float encoded);
float toEncoded(float linear);
double toLinear(double encoded);
double toEncoded(double linear);

/// User colours ("display-referred 0..1": the Drop Shadow's and the Border's colour, a Solid redaction) -> linear, as
/// the compositor converts them (Compositor.cpp srgbToLinear): the IEC 61966-2-1 sRGB curve, input clamped to [0, 1].
///   srgbToLinear(v) = v <= 0.04045 ? v / 12.92 : ((v + 0.055) / 1.055)^2.4
///   linearToSrgb(l) = l <= 0.0031308 ? 12.92 l : 1.055 l^(1 / 2.4) - 0.055
double srgbToLinear(double encoded);
double linearToSrgb(double linear);

/// Rec. 709 luma weights: 0.2126 R + 0.7152 G + 0.0722 B (Lum() of the blend modes, Y' of the keyer).
double luma(const Rgb& c);

// ---- Sampling -----------------------------------------------------------------------------------------------------

/// Bilinear sample at pixel coordinates (pixel centres at +0.5), the edge texel repeated outside (a clamp-to-edge
/// linear sampler, as the compositor's textures):
///   fx = x - 0.5, fy = y - 0.5, x0 = floor(fx), y0 = floor(fy), tx = fx - x0, ty = fy - y0,
///   mix(mix(P(x0, y0), P(x0 + 1, y0), tx), mix(P(x0, y0 + 1), P(x0 + 1, y0 + 1), tx), ty), P clamped to the image.
/// An empty image samples transparent black.
Rgba sampleBilinear(const Image& image, double x, double y);

// ---- Comparison ---------------------------------------------------------------------------------------------------

struct Difference {
    /// The largest |a - b| over every channel (alpha too).
    double maxAbs = 0.0;
    /// 10 log10(1 / MSE) in dB, MSE over every channel (peak 1.0); +infinity when identical.
    double psnr = 0.0;
};

/// The difference of two pictures. Different sizes: maxAbs = +infinity, psnr = 0. Two empty pictures are identical.
Difference compare(const Image& a, const Image& b);

// ---- Golden files -------------------------------------------------------------------------------------------------

/// The golden-file format ("WRI1", text, diffable):
///   line 1:  "WRI1 <width> <height>"                     (1..4096 each)
///   then:    one line per pixel, row-major from the top row: "r g b a"
/// Each value is printed as printf("%.6g") does in the C locale (std::to_chars, general format, precision 6), -0 as 0.
/// Lines end in LF; the reader also takes CRLF and any whitespace between values, and rejects a wrong header, a count
/// other than width x height x 4, trailing data and values that are not finite. Six significant digits make the text
/// stable across compilers; compare a golden with a tolerance (a value is within 5e-7 relative of what was written).
std::string goldenText(const Image& image);
std::expected<Image, Error> parseGolden(std::string_view text);
/// Reads a golden file: NotFound (no file), IoError (unreadable or larger than 256 MB), ParseError (parseGolden).
std::expected<Image, Error> readGolden(const std::filesystem::path& file);
/// Writes goldenText(image), creating the parent folders. False for an empty picture, a value that is not finite or a
/// write that fails.
bool writeGolden(const std::filesystem::path& file, const Image& image);

} // namespace wabcut::render::reference
