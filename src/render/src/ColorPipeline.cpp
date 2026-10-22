#include <wabcut/render/ColorPipeline.h>

#include <wabcut/color/ColorSpaces.h>

#include <algorithm>
#include <cmath>

namespace wabcut::render {

std::string_view outputTransformName(OutputTransform transform) {
    switch (transform) {
    case OutputTransform::Rec709:
        return "rec709";
    case OutputTransform::SRGB:
        return "srgb";
    case OutputTransform::Linear:
        return "linear";
    }
    return "unknown";
}

ResolvedColor resolveSourceColor(const ColorInfo& color, const ColorPipeline& pipeline, bool isRgb) {
    const ColorInfo& fb = pipeline.untaggedFallback;
    ResolvedColor r;
    if (isRgb) {
        r.matrix = ColorMatrix::RGB;
    } else {
        r.matrix = color.matrix != ColorMatrix::Unknown && color.matrix != ColorMatrix::RGB ? color.matrix
                   : fb.matrix != ColorMatrix::Unknown && fb.matrix != ColorMatrix::RGB ? fb.matrix
                                                                                        : ColorMatrix::BT709;
    }
    if (color.range != ColorRange::Unknown) {
        r.range = color.range;
    } else if (isRgb) {
        r.range = ColorRange::Full;
    } else {
        r.range = fb.range != ColorRange::Unknown ? fb.range : ColorRange::Limited;
    }
    r.primaries = color.primaries != ColorPrimaries::Unknown ? color.primaries
                  : fb.primaries != ColorPrimaries::Unknown  ? fb.primaries
                                                             : ColorPrimaries::BT709;
    if (color.transfer != TransferFunction::Unknown) {
        r.transfer = color.transfer;
    } else if (isRgb) {
        r.transfer = TransferFunction::SRGB;
    } else {
        r.transfer = fb.transfer != TransferFunction::Unknown ? fb.transfer : TransferFunction::BT709;
    }
    r.chromaLocation = color.chromaLocation != ChromaLocation::Unknown ? color.chromaLocation
                       : fb.chromaLocation != ChromaLocation::Unknown  ? fb.chromaLocation
                                                                       : ChromaLocation::Left;
    r.hdr = r.transfer == TransferFunction::PQ || r.transfer == TransferFunction::HLG;
    return r;
}

// ---- Matrices ----

Mat3 Mat3::operator*(const Mat3& o) const {
    Mat3 r;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            double sum = 0.0;
            for (int k = 0; k < 3; ++k) {
                sum += m[static_cast<std::size_t>(row * 3 + k)] * o.m[static_cast<std::size_t>(k * 3 + col)];
            }
            r.m[static_cast<std::size_t>(row * 3 + col)] = sum;
        }
    }
    return r;
}

Mat3 inverse(const Mat3& a) {
    const auto& m = a.m;
    const double c00 = m[4] * m[8] - m[5] * m[7];
    const double c01 = m[5] * m[6] - m[3] * m[8];
    const double c02 = m[3] * m[7] - m[4] * m[6];
    const double det = m[0] * c00 + m[1] * c01 + m[2] * c02;
    const double inv = det != 0.0 ? 1.0 / det : 0.0;
    Mat3 r;
    r.m = {c00 * inv, (m[2] * m[7] - m[1] * m[8]) * inv, (m[1] * m[5] - m[2] * m[4]) * inv,
           c01 * inv, (m[0] * m[8] - m[2] * m[6]) * inv, (m[2] * m[3] - m[0] * m[5]) * inv,
           c02 * inv, (m[1] * m[6] - m[0] * m[7]) * inv, (m[0] * m[4] - m[1] * m[3]) * inv};
    return r;
}

std::array<double, 2> lumaCoefficients(ColorMatrix matrix) {
    switch (matrix) {
    case ColorMatrix::BT601:
        return {0.299, 0.114};
    case ColorMatrix::BT2020NonConstant:
    case ColorMatrix::BT2020Constant: // constant-luminance decoding is not separable; NCL is the closest
                                      // affine map
        return {0.2627, 0.0593};
    case ColorMatrix::SMPTE240M:
        return {0.212, 0.087};
    case ColorMatrix::BT709:
    case ColorMatrix::RGB:
    case ColorMatrix::Unknown:
        break;
    }
    return {0.2126, 0.0722};
}

SampleToRgb sampleToRgb(ColorMatrix matrix, ColorRange range, SampleEncoding encoding, bool isRgb) {
    const int bits = encoding == SampleEncoding::Bits8 ? 8 : 10;
    // Normalized texture sample -> integer code value.
    const double toCode = encoding == SampleEncoding::Bits8       ? 255.0
                          : encoding == SampleEncoding::Bits10Low ? 65535.0
                                                                  : 65535.0 / 64.0;
    const double scale = std::ldexp(1.0, bits - 8); // 1 for 8-bit, 4 for 10-bit
    const double maxCode = std::ldexp(1.0, bits) - 1.0;
    const bool limited = range != ColorRange::Full;

    SampleToRgb out;
    if (isRgb) {
        const double k = limited ? toCode / (219.0 * scale) : toCode / maxCode;
        const double o = limited ? -16.0 / 219.0 : 0.0;
        out.matrix.m = {k, 0, 0, 0, k, 0, 0, 0, k};
        out.offset = {o, o, o};
        return out;
    }

    // Code value -> Y' in [0, 1] and Pb/Pr in [-0.5, 0.5] (ITU-T H.273 quantization).
    double ky = 0.0;
    double oy = 0.0;
    double kc = 0.0;
    double oc = 0.0;
    if (limited) {
        ky = toCode / (219.0 * scale);
        oy = -16.0 / 219.0;
        kc = toCode / (224.0 * scale);
        oc = -128.0 / 224.0;
    } else {
        ky = toCode / maxCode;
        oy = 0.0;
        kc = toCode / maxCode;
        oc = -std::ldexp(1.0, bits - 1) / maxCode;
    }

    const auto [kr, kb] = lumaCoefficients(matrix);
    const double kg = 1.0 - kr - kb;
    Mat3 ypbprToRgb;
    ypbprToRgb.m = {1.0,
                    0.0,
                    2.0 * (1.0 - kr), //
                    1.0,
                    -2.0 * kb * (1.0 - kb) / kg,
                    -2.0 * kr * (1.0 - kr) / kg, //
                    1.0,
                    2.0 * (1.0 - kb),
                    0.0};
    Mat3 quant;
    quant.m = {ky, 0, 0, 0, kc, 0, 0, 0, kc};
    out.matrix = ypbprToRgb * quant;
    out.offset = ypbprToRgb * std::array<double, 3>{oy, oc, oc};
    return out;
}

// ---- Transfer functions ----

namespace {

constexpr double kRec709Alpha = 1.09929682680944;
constexpr double kRec709Beta = 0.018053968510807;

double bt709ToLinear(double v) {
    return v < 4.5 * kRec709Beta ? v / 4.5 : std::pow((v + (kRec709Alpha - 1.0)) / kRec709Alpha, 1.0 / 0.45);
}

double linearToBt709(double l) {
    return l < kRec709Beta ? 4.5 * l : kRec709Alpha * std::pow(l, 0.45) - (kRec709Alpha - 1.0);
}

double srgbToLinear(double v) {
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

double linearToSrgb(double l) {
    return l <= 0.0031308 ? 12.92 * l : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
}

} // namespace

double transferToLinear(TransferFunction transfer, double encoded) {
    const double v = std::clamp(encoded, 0.0, 1.0);
    switch (transfer) {
    case TransferFunction::SRGB:
        return srgbToLinear(v);
    case TransferFunction::Linear:
        return v;
    case TransferFunction::Gamma22:
        return std::pow(v, 2.2);
    case TransferFunction::Gamma28:
        return std::pow(v, 2.8);
    case TransferFunction::BT709:
    case TransferFunction::PQ: // HDR: flagged, SDR fallback curve in the MVP
    case TransferFunction::HLG:
    case TransferFunction::Unknown:
        break;
    }
    return bt709ToLinear(v);
}

double linearToOutput(OutputTransform transform, double linear) {
    const double l = std::clamp(linear, 0.0, 1.0);
    switch (transform) {
    case OutputTransform::SRGB:
        return linearToSrgb(l);
    case OutputTransform::Linear:
        return l;
    case OutputTransform::Rec709:
        break;
    }
    return linearToBt709(l);
}

// ---- Primaries ----

namespace {

struct Chromaticities {
    double rx, ry, gx, gy, bx, by, wx, wy;
};

constexpr Chromaticities kRec709{0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290};

Chromaticities chromaticities(ColorPrimaries p) {
    switch (p) {
    case ColorPrimaries::BT601_625:
        return {0.640, 0.330, 0.290, 0.600, 0.150, 0.060, 0.3127, 0.3290};
    case ColorPrimaries::BT601_525:
        return {0.630, 0.340, 0.310, 0.595, 0.155, 0.070, 0.3127, 0.3290};
    case ColorPrimaries::BT2020:
        return {0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290};
    case ColorPrimaries::DCIP3:
        return {0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.314, 0.351};
    case ColorPrimaries::DisplayP3:
        return {0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290};
    case ColorPrimaries::BT709:
    case ColorPrimaries::Unknown:
        break;
    }
    return kRec709;
}

std::array<double, 3> xyToXyz(double x, double y) {
    return {x / y, 1.0, (1.0 - x - y) / y};
}

Mat3 rgbToXyz(const Chromaticities& c) {
    const auto r = xyToXyz(c.rx, c.ry);
    const auto g = xyToXyz(c.gx, c.gy);
    const auto b = xyToXyz(c.bx, c.by);
    Mat3 p;
    p.m = {r[0], g[0], b[0], r[1], g[1], b[1], r[2], g[2], b[2]};
    const auto s = inverse(p) * xyToXyz(c.wx, c.wy);
    Mat3 out;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            out.m[static_cast<std::size_t>(row * 3 + col)] =
                p.m[static_cast<std::size_t>(row * 3 + col)] * s[static_cast<std::size_t>(col)];
        }
    }
    return out;
}

/// Bradford chromatic adaptation from white (sx, sy) to white (dx, dy).
Mat3 bradford(double sx, double sy, double dx, double dy) {
    Mat3 ma;
    ma.m = {0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296};
    const auto src = ma * xyToXyz(sx, sy);
    const auto dst = ma * xyToXyz(dx, dy);
    Mat3 scale;
    scale.m = {dst[0] / src[0], 0, 0, 0, dst[1] / src[1], 0, 0, 0, dst[2] / src[2]};
    return inverse(ma) * scale * ma;
}

} // namespace

Mat3 primariesToRec709(ColorPrimaries primaries) {
    if (primaries == ColorPrimaries::BT709 || primaries == ColorPrimaries::Unknown) {
        return Mat3::identity();
    }
    const Chromaticities src = chromaticities(primaries);
    Mat3 toXyz = rgbToXyz(src);
    if (src.wx != kRec709.wx || src.wy != kRec709.wy) {
        toXyz = bradford(src.wx, src.wy, kRec709.wx, kRec709.wy) * toXyz;
    }
    return inverse(rgbToXyz(kRec709)) * toXyz;
}

std::array<double, 3> referencePixel(const std::array<double, 3>& samples, double alpha,
                                     const ResolvedColor& color, SampleEncoding encoding, bool isRgb,
                                     OutputTransform output) {
    std::array<double, 3> rgb = sampleToRgb(color.matrix, color.range, encoding, isRgb).apply(samples);
    if (const auto sdr = color::hdrCaptureToSdr(color.transfer, rgb)) {
        rgb = *sdr; // an HDR capture tone-mapped into SDR (I.9): already linear Rec.709
    } else {
        for (double& v : rgb) {
            v = transferToLinear(color.transfer, std::clamp(v, 0.0, 1.0));
        }
        rgb = primariesToRec709(color.primaries) * rgb;
    }
    const double a = std::clamp(alpha, 0.0, 1.0);
    for (double& v : rgb) {
        v = linearToOutput(output, v * a); // premultiplied, i.e. over black
    }
    return rgb;
}

} // namespace wabcut::render
