// Colour management (ColorSpaces.h): working spaces, input and output plans, OpenColorIO built-in transforms baked into
// 3D LUTs, and the CPU references.

#include <wabcut/color/ColorSpaces.h>

#include <OpenColorIO/OpenColorIO.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

namespace OCIO = OCIO_NAMESPACE;

namespace wabcut::color {

namespace {

// ---- Matrices ----------------------------------------------------------------------------------------------------

Matrix3 mul(const Matrix3& a, const Matrix3& b) {
    Matrix3 r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double s = 0.0;
            for (int k = 0; k < 3; ++k) {
                s += a[static_cast<std::size_t>(i * 3 + k)] * b[static_cast<std::size_t>(k * 3 + j)];
            }
            r[static_cast<std::size_t>(i * 3 + j)] = s;
        }
    }
    return r;
}

Matrix3 inverse(const Matrix3& m) {
    const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    const double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const double det = a * A + b * B + c * C;
    const double k = det != 0.0 ? 1.0 / det : 0.0;
    return {A * k, -(b * i - c * h) * k, (b * f - c * e) * k, B * k, (a * i - c * g) * k, -(a * f - c * d) * k,
            C * k, -(a * h - b * g) * k, (a * e - b * d) * k};
}

struct Chromaticities {
    double rx, ry, gx, gy, bx, by, wx, wy;
};

constexpr Chromaticities kRec709{0.64, 0.33, 0.30, 0.60, 0.15, 0.06, 0.3127, 0.3290};
constexpr Chromaticities kRec2020{0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290};
constexpr Chromaticities kAp1{0.713, 0.293, 0.165, 0.830, 0.128, 0.044, 0.32168, 0.33767};
constexpr Chromaticities kAp0{0.7347, 0.2653, 0.0, 1.0, 0.0001, -0.0770, 0.32168, 0.33767};
constexpr Chromaticities kP3D65{0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290}; // 0.6: SMPTE EG 432-1
// Camera gamuts (D65) of the camera logs made here (0.6). F-Gamut (Fujifilm "F-Log Data Sheet" Ver.1.1), N-Gamut (Nikon
// "N-Log Specification Document" 1.0.0) and Leica's L-Log gamut ("L-Log Reference Manual" V1.9, section 5: ITU-R BT.2020)
// are Rec.2020.
constexpr Chromaticities kDGamut{0.71, 0.31, 0.21, 0.88, 0.09, -0.08, 0.3127, 0.3290}; // DJI D-Log / D-Gamut white paper
constexpr Chromaticities kSGamut{0.73, 0.28, 0.14, 0.855, 0.10, -0.05, 0.3127, 0.3290}; // Sony S-Gamut (= S-Gamut3)
constexpr Chromaticities kSGamut3Cine{0.766, 0.275, 0.225, 0.800, 0.089, -0.087, 0.3127, 0.3290}; // Sony S-Gamut3.Cine
constexpr Chromaticities kCinemaGamut{0.74, 0.27, 0.17, 1.14, 0.08, -0.10, 0.3127, 0.3290}; // Canon Cinema Gamut

Rgb3 xyzOf(double x, double y) { return {x / y, 1.0, (1.0 - x - y) / y}; }

Matrix3 rgbToXyz(const Chromaticities& c) {
    const Rgb3 r = xyzOf(c.rx, c.ry), g = xyzOf(c.gx, c.gy), b = xyzOf(c.bx, c.by);
    const Matrix3 p{r[0], g[0], b[0], r[1], g[1], b[1], r[2], g[2], b[2]};
    const Rgb3 s = color::apply(inverse(p), xyzOf(c.wx, c.wy));
    return {p[0] * s[0], p[1] * s[1], p[2] * s[2], p[3] * s[0], p[4] * s[1], p[5] * s[2], p[6] * s[0], p[7] * s[1], p[8] * s[2]};
}

constexpr Matrix3 kBradford{0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296};
constexpr Matrix3 kCat02{0.7328, 0.4296, -0.1624, -0.7036, 1.6975, 0.0061, 0.0030, 0.0136, 0.9834};

/// Von Kries chromatic adaptation in the cone space `cone` (Bradford, CAT02) from white `from` to white `to` (XYZ -> XYZ).
Matrix3 adaptation(const Matrix3& cone, double fx, double fy, double tx, double ty) {
    const Rgb3 s = color::apply(cone, xyzOf(fx, fy));
    const Rgb3 d = color::apply(cone, xyzOf(tx, ty));
    const Matrix3 scale{d[0] / s[0], 0, 0, 0, d[1] / s[1], 0, 0, 0, d[2] / s[2]};
    return mul(inverse(cone), mul(scale, cone));
}

/// Linear RGB in `from` -> linear RGB in `to`, adapted between different whites in `cone` (Bradford by default).
Matrix3 convert(const Chromaticities& from, const Chromaticities& to, const Matrix3& cone = kBradford) {
    Matrix3 m = rgbToXyz(from);
    if (from.wx != to.wx || from.wy != to.wy) {
        m = mul(adaptation(cone, from.wx, from.wy, to.wx, to.wy), m);
    }
    return mul(inverse(rgbToXyz(to)), m);
}

const Chromaticities& chromaticitiesOf(Working w) {
    switch (w) {
    case Working::AcesCg: return kAp1;
    case Working::LinearRec2020: return kRec2020;
    case Working::LinearRec709: break;
    }
    return kRec709;
}

// ---- Curves ------------------------------------------------------------------------------------------------------

constexpr double kPqM1 = 2610.0 / 16384.0;
constexpr double kPqM2 = 2523.0 / 4096.0 * 128.0;
constexpr double kPqC1 = 3424.0 / 4096.0;
constexpr double kPqC2 = 2413.0 / 4096.0 * 32.0;
constexpr double kPqC3 = 2392.0 / 4096.0 * 32.0;
constexpr double kHlgA = 0.17883277;
constexpr double kHlgB = 0.28466892; // 1 - 4a
constexpr double kHlgC = 0.55991073; // 0.5 - a ln(4a)

double rec709ToLinear(double v) {
    const double a = 1.09929682680944, b = 0.018053968510807;
    return v < 4.5 * b ? v / 4.5 : std::pow((v + (a - 1.0)) / a, 1.0 / 0.45);
}
double linearToRec709(double v) {
    const double a = 1.09929682680944, b = 0.018053968510807;
    v = std::clamp(v, 0.0, 1.0);
    return v < b ? 4.5 * v : a * std::pow(v, 0.45) - (a - 1.0);
}
double srgbToLinear(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
double linearToSrgb(double v) {
    v = std::clamp(v, 0.0, 1.0);
    return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}

constexpr double kCctMax = 1.4679964; // acesCctFromLinear(65504)
constexpr double kShaperFloor = 1.0 / 4096.0; // 2^-12: the shaper's offset

double lumaRec2020(const Rgb3& c) { return 0.2627 * c[0] + 0.6780 * c[1] + 0.0593 * c[2]; }

// ---- Camera log curves OpenColorIO does not build in (0.6) ----------------------------------------------------------
// Each is the maker's published decode: a full-range code value (10-bit code / 1023, as the file stores it before any
// video-range expansion) -> scene-linear reflectance (0.18 = an 18 % grey card), with the document it comes from.

/// DJI D-Log. Source: DJI, "White Paper on D-Log and D-Gamut of DJI Cinema Color System" (2017). 18 % grey = 0.3988.
double dLogToLinear(double v) {
    return v <= 0.14 ? (v - 0.0929) / 6.025 : (std::pow(10.0, 3.89616 * v - 2.27752) - 0.0108) / 0.9892;
}

/// Fujifilm F-Log and F-Log2: one form, two sets of constants.
struct FLogConstants {
    double a, b, c, d, e, f, cut2;
};
/// Source: Fujifilm, "F-Log Data Sheet" Ver.1.1 (2022): 0 % / 18 % / 90 % = 95 / 470 / 705 of 1023.
constexpr FLogConstants kFLog{0.555556, 0.009468, 0.344676, 0.790453, 8.735631, 0.092864, 0.100537775223865};
/// Source: Fujifilm, "F-Log2 Data Sheet" Ver.1.0 (2022): 0 % / 18 % / 90 % = 95 / 400 / 570 of 1023.
constexpr FLogConstants kFLog2{5.555556, 0.064829, 0.245281, 0.384316, 8.799461, 0.092864, 0.100686685370811};
double fLogCurve(const FLogConstants& k, double v) {
    return v < k.cut2 ? (v - k.f) / k.e : std::pow(10.0, (v - k.d) / k.c) / k.a - k.b / k.a;
}
double fLogToLinear(double v) { return fLogCurve(kFLog, v); }
double fLog2ToLinear(double v) { return fLogCurve(kFLog2, v); }

/// Nikon N-Log: a cube-root segment below code 452, a natural log above. Source: Nikon, "N-Log Specification Document"
/// Version 1.0.0 (2018). 18 % grey = 372 of 1023.
double nLogToLinear(double v) {
    const double x = v * 1023.0;
    return x < 452.0 ? std::pow(x / 650.0, 3.0) - 0.0075 : std::exp((x - 619.0) / 150.0);
}

/// Sony S-Log2: S-Log on 155 / 219 of the light, written in the video range (64 + 876 y of 1023); reflectance is 0.9 of
/// the curve's input. Source: Sony, "S-Log2 Technical Paper" (2012): 0 % / 18 % / 90 % = 90 / 347 / 582 of 1023.
double sLog2ToLinear(double v) {
    const double y = (v * 1023.0 - 64.0) / 876.0;
    const double x = y >= 0.030001222851889303 ? std::pow(10.0, (y - 0.616596 - 0.03) / 0.432699) - 0.037584
                                                : (y - 0.030001222851889303) / 5.0;
    return x * 0.9 * 219.0 / 155.0;
}

/// Canon Log (the original): mirrored below black, written in the video range; reflectance is 0.9 of the curve's input.
/// Source: L. Thorpe, Canon, "Canon-Log Transfer Characteristic" (2012): 0 % / 18 % / 90 % = 128 / 351 / 614 of 1023.
double canonLogToLinear(double v) {
    const double c = (v * 1023.0 - 64.0) / 876.0;
    const double x = c < 0.0730597 ? -(std::pow(10.0, (0.0730597 - c) / 0.529136) - 1.0) / 10.1596
                                   : (std::pow(10.0, (c - 0.0730597) / 0.529136) - 1.0) / 10.1596;
    return x * 0.9;
}

/// Leica L-Log. Source: Leica Camera AG, "L-Log Reference Manual" V1.9 (2026; the curve unchanged since V1.0, 2021):
/// 0 % / 18 % / 90 % = 92 / 445 / 634 of 1023.
double lLogToLinear(double v) { return v <= 0.1380 ? (v - 0.09) / 8.0 : (std::pow(10.0, (v - 0.6) / 0.27) - 0.0115) / 1.3; }

// ---- OpenColorIO ---------------------------------------------------------------------------------------------------

constexpr const char* kAp0ToXyz = "UTILITY - ACES-AP0_to_CIE-XYZ-D65_BFD";
constexpr const char* kOdtSdr = "ACES-OUTPUT - ACES2065-1_to_CIE-XYZ-D65 - SDR-VIDEO_1.0";
constexpr const char* kOdtHdr1000 = "ACES-OUTPUT - ACES2065-1_to_CIE-XYZ-D65 - HDR-VIDEO-1000nit-15nit-REC2020lim_1.1";
constexpr const char* kDisplay709 = "DISPLAY - CIE-XYZ-D65_to_REC.1886-REC.709";
constexpr const char* kDisplayPq = "DISPLAY - CIE-XYZ-D65_to_REC.2100-PQ";
constexpr const char* kDisplayHlg = "DISPLAY - CIE-XYZ-D65_to_REC.2100-HLG-1000nit";
// 0.6 (#72): the other SDR output colour spaces of an ACES project's exports.
constexpr const char* kDisplay2020 = "DISPLAY - CIE-XYZ-D65_to_REC.1886-REC.2020";
constexpr const char* kDisplayP3D65 = "DISPLAY - CIE-XYZ-D65_to_G2.6-P3-D65";

/// How a camera format reaches ACES2065-1: an OCIO built-in transform, or (formats OCIO does not build in) the maker's
/// curve and its gamut's primaries.
struct CameraFormat {
    const char* builtin = nullptr;           ///< the OCIO built-in transform's style
    double (*decode)(double) = nullptr;      ///< else: the maker's curve (code value -> scene-linear reflectance)
    const Chromaticities* gamut = nullptr;   ///< and its primaries
};

CameraFormat cameraFormat(InputColorSpace space) {
    switch (space) {
    case InputColorSpace::SLog3SGamut3: return {"SONY_SLOG3-SGAMUT3_to_ACES2065-1"};
    case InputColorSpace::SLog3SGamut3Cine: return {"SONY_SLOG3-SGAMUT3.CINE_to_ACES2065-1"};
    case InputColorSpace::VLogVGamut: return {"PANASONIC_VLOG-VGAMUT_to_ACES2065-1"};
    case InputColorSpace::LogC3: return {"ARRI_ALEXA-LOGC-EI800-AWG_to_ACES2065-1"};
    case InputColorSpace::LogC4: return {"ARRI_LOGC4_to_ACES2065-1"};
    case InputColorSpace::CanonLog3: return {"CANON_CLOG3-CGAMUT_to_ACES2065-1"};
    case InputColorSpace::AppleLog: return {"APPLE_LOG_to_ACES2065-1"};
    case InputColorSpace::CanonLog2: return {"CANON_CLOG2-CGAMUT_to_ACES2065-1"};
    case InputColorSpace::RedLog3G10: return {"RED_LOG3G10-RWG_to_ACES2065-1"};
    case InputColorSpace::AcesCct: return {"ACEScct_to_ACES2065-1"};
    case InputColorSpace::RedLogFilm: return {"RED_REDLOGFILM-RWG_to_ACES2065-1"};
    case InputColorSpace::DLogDGamut: return {nullptr, dLogToLinear, &kDGamut};
    case InputColorSpace::FLogFGamut: return {nullptr, fLogToLinear, &kRec2020};
    case InputColorSpace::FLog2FGamut: return {nullptr, fLog2ToLinear, &kRec2020};
    case InputColorSpace::NLogNGamut: return {nullptr, nLogToLinear, &kRec2020};
    case InputColorSpace::SLog2SGamut: return {nullptr, sLog2ToLinear, &kSGamut};
    case InputColorSpace::SLog2SGamut3Cine: return {nullptr, sLog2ToLinear, &kSGamut3Cine};
    case InputColorSpace::CanonLogCinemaGamut: return {nullptr, canonLogToLinear, &kCinemaGamut};
    case InputColorSpace::LLogRec2020: return {nullptr, lLogToLinear, &kRec2020};
    default: return {};
    }
}

bool isLog(InputColorSpace space) {
    const CameraFormat f = cameraFormat(space);
    return f.builtin != nullptr || f.decode != nullptr;
}

/// A camera gamut -> ACES2065-1 as the ACES input transforms and OCIO's camera built-ins make it: CAT02-adapted from the
/// camera's white to the ACES white (S-Gamut3, Cinema Gamut and D-Gamut give exactly the matrices of Sony's, Canon's and
/// DJI's ACES transforms).
Matrix3 cameraGamutToAp0(const Chromaticities& gamut) { return convert(gamut, kAp0, kCat02); }

OCIO::MatrixTransformRcPtr matrixTransform(const Matrix3& m, const std::array<double, 3>& offset = {0, 0, 0}) {
    const double m44[16]{m[0], m[1], m[2], 0, m[3], m[4], m[5], 0, m[6], m[7], m[8], 0, 0, 0, 0, 1};
    const double off[4]{offset[0], offset[1], offset[2], 0};
    auto t = OCIO::MatrixTransform::Create();
    t->setMatrix(m44);
    t->setOffset(off);
    return t;
}

OCIO::BuiltinTransformRcPtr builtin(const char* style, bool inverse = false) {
    auto t = OCIO::BuiltinTransform::Create();
    t->setStyle(style);
    t->setDirection(inverse ? OCIO::TRANSFORM_DIR_INVERSE : OCIO::TRANSFORM_DIR_FORWARD);
    return t;
}

/// A camera curve as an OCIO 1D LUT: `decode` at 4096 code values over [0, 1], linear between (exact at the samples,
/// within 3e-6 relative of the equation elsewhere - up to 2e-4 absolute over the cell where a white paper's two segments
/// meet with a small step of their own, as N-Log's and L-Log's do).
/// Not LogCameraTransform: it makes its linear toe the log's tangent, and these curves' toes are not (N-Log's is a cube
/// root, Canon Log's a mirrored log, the others' linear segments are the white papers' own).
OCIO::Lut1DTransformRcPtr curveLut(double (*decode)(double)) {
    constexpr unsigned long n = 4096;
    auto lut = OCIO::Lut1DTransform::Create(n, false);
    for (unsigned long i = 0; i < n; ++i) {
        const auto v = static_cast<float>(decode(static_cast<double>(i) / static_cast<double>(n - 1)));
        lut->setValue(i, v, v, v);
    }
    lut->setInterpolation(OCIO::INTERP_LINEAR);
    return lut;
}

/// The OCIO transform taking a camera format's full-range code values to ACES2065-1, or null (not a camera format): its
/// built-in, or a group of the maker's curve (curveLut) and the gamut -> AP0 matrix (cameraGamutToAp0).
OCIO::TransformRcPtr cameraToAces(InputColorSpace space) {
    const CameraFormat f = cameraFormat(space);
    if (f.builtin) {
        return builtin(f.builtin);
    }
    if (!f.decode || !f.gamut) {
        return nullptr;
    }
    auto group = OCIO::GroupTransform::Create();
    group->appendTransform(curveLut(f.decode));
    group->appendTransform(matrixTransform(cameraGamutToAp0(*f.gamut)));
    return group;
}

const OCIO::ConstConfigRcPtr& rawConfig() {
    static const OCIO::ConstConfigRcPtr config = OCIO::Config::CreateRaw();
    return config;
}

/// One float pixel through a processor, in double.
Rgb3 run(const OCIO::ConstCPUProcessorRcPtr& cpu, const Rgb3& v) {
    float p[3]{static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])};
    cpu->applyRGB(p);
    return {p[0], p[1], p[2]};
}

/// A built OCIO chain: its CPU processor (cached by key).
struct Chain {
    OCIO::ConstCPUProcessorRcPtr cpu;
};

std::mutex& cacheMutex() {
    static std::mutex m;
    return m;
}
std::map<std::string, std::shared_ptr<const Chain>>& chainCache() {
    static std::map<std::string, std::shared_ptr<const Chain>> c;
    return c;
}
std::map<std::string, std::shared_ptr<const BakedLut>>& lutCache() {
    static std::map<std::string, std::shared_ptr<const BakedLut>> c;
    return c;
}

template <class Build>
std::expected<std::shared_ptr<const Chain>, Error> chainFor(const std::string& key, Build&& build) {
    {
        std::lock_guard lock(cacheMutex());
        if (const auto it = chainCache().find(key); it != chainCache().end()) {
            return it->second;
        }
    }
    try {
        auto group = OCIO::GroupTransform::Create();
        build(*group);
        const OCIO::ConstProcessorRcPtr processor = rawConfig()->getProcessor(group);
        auto chain = std::make_shared<Chain>();
        chain->cpu = processor->getOptimizedCPUProcessor(OCIO::BIT_DEPTH_F32, OCIO::BIT_DEPTH_F32, OCIO::OPTIMIZATION_DEFAULT);
        std::lock_guard lock(cacheMutex());
        return chainCache().emplace(key, std::move(chain)).first->second;
    } catch (const OCIO::Exception& e) {
        return std::unexpected(Error{ErrorCode::Unsupported, std::string("OpenColorIO cannot build ") + key + ": " + e.what()});
    }
}

/// Working space -> ACES2065-1 (linear).
Matrix3 workingToAp0(Working w) { return convert(chromaticitiesOf(w), kAp0); }

/// The primaries of an SDR target's display (0.6: Rec.2020 SDR and P3-D65 besides Rec.709).
const Chromaticities& sdrPrimaries(OutputTarget target) {
    return target == OutputTarget::P3D65 ? kP3D65 : target == OutputTarget::Rec2020Sdr ? kRec2020 : kRec709;
}

/// The ACES output chain of an ACES project to `target`: ACEScg -> AP0 -> output transform -> display encoding.
void acesOutput(OCIO::GroupTransform& g, OutputTarget target) {
    const bool sdr = isSdrTarget(target);
    g.appendTransform(matrixTransform(workingToAp0(Working::AcesCg)));
    g.appendTransform(builtin(sdr ? kOdtSdr : kOdtHdr1000));
    g.appendTransform(builtin(target == OutputTarget::Sdr          ? kDisplay709
                              : target == OutputTarget::Rec2020Sdr ? kDisplay2020
                              : target == OutputTarget::P3D65      ? kDisplayP3D65
                              : target == OutputTarget::Pq         ? kDisplayPq
                                                                   : kDisplayHlg));
}

/// The part of it baked into the output LUT: ACEScg -> AP0 -> output transform -> display light in the display's
/// primaries (Rec.709 for SDR, Rec.2020 for HDR; 0.6: Rec.2020 or P3-D65 for those SDR targets), unclamped; the display
/// encoding runs after the LUT (finish).
void acesOutputLight(OCIO::GroupTransform& g, OutputTarget target) {
    const bool sdr = isSdrTarget(target);
    g.appendTransform(matrixTransform(workingToAp0(Working::AcesCg)));
    g.appendTransform(builtin(sdr ? kOdtSdr : kOdtHdr1000));
    g.appendTransform(matrixTransform(inverse(rgbToXyz(sdr ? sdrPrimaries(target) : kRec2020))));
}

std::string targetName(OutputTarget t) {
    switch (t) {
    case OutputTarget::Sdr: return "sdr";
    case OutputTarget::Pq: return "pq";
    case OutputTarget::Hlg: return "hlg";
    case OutputTarget::Rec2020Sdr: return "sdr-2020";
    case OutputTarget::P3D65: return "sdr-p3d65";
    }
    return "sdr";
}

/// Direct Rec.709 (SDR): a log clip is tone-mapped into SDR on input (Premiere: "tone-mapped and gamut-compressed on
/// input") - its OCIO chain ends in scene-linear Rec.2020 (1.0 = diffuse white), which is then tone-mapped exactly as an
/// HDR capture is (inputPost; the shader's captureToSdr): the BT.2390 EETF on the luminance from a 1000-nit master whose
/// 203-nit reference white is the scene's white, the colour scaled along, Rec.2020 -> Rec.709 clipped.
bool toneMappedOnInput(const ColorManagement& m, InputColorSpace space) {
    return m.mode == ColorManagementMode::DisplayRec709 && isLog(space);
}

/// The key of an input chain (and of its baked LUT).
std::string inputKey(const ColorManagement& m, InputColorSpace space, bool limitedRange, bool displayReferred) {
    return std::string("in:") + std::string(toString(space)) + (limitedRange ? ":video" : ":full") +
           (displayReferred ? ":display" : "") + (toneMappedOnInput(m, space) ? ":direct709" : "") + ":" +
           std::to_string(static_cast<int>(workingOf(m.mode)));
}

/// What follows an input chain's OCIO processor: Direct Rec.709's log is tone-mapped into SDR from its scene light;
/// everything else is already the working value.
Rgb3 inputPost(const ColorManagement& m, InputColorSpace space, const Rgb3& v) {
    if (!toneMappedOnInput(m, space)) {
        return v;
    }
    return toneMapToSdr({std::max(v[0], 0.0), std::max(v[1], 0.0), std::max(v[2], 0.0)}, kCaptureReferenceWhiteNits,
                        kCapturePeakNits);
}

/// The OCIO chain of an input that is not analytic (a camera log, or an ACES project's display-referred picture).
std::expected<std::shared_ptr<const Chain>, Error> inputChain(const ColorManagement& m, InputColorSpace space,
                                                              bool limitedRange, bool displayReferred) {
    const Working w = workingOf(m.mode);
    const std::string key = inputKey(m, space, limitedRange, displayReferred);
    return chainFor(key, [&](OCIO::GroupTransform& g) {
        if (displayReferred) {
            // An ACES project's display-referred picture: the inverse of its SDR output transform (round trip: it
            // looks as it did), then AP0 -> ACEScg. Code values are Rec.709 display values here.
            g.appendTransform(builtin(kDisplay709, true));
            g.appendTransform(builtin(kOdtSdr, true));
            g.appendTransform(matrixTransform(inverse(workingToAp0(Working::AcesCg))));
            return;
        }
        if (limitedRange) {
            // The curves are defined on full-range code values: back from the expanded video range (64..940 of 1023).
            const double s = (940.0 - 64.0) / 1023.0;
            g.appendTransform(matrixTransform({s, 0, 0, 0, s, 0, 0, 0, s}, {64.0 / 1023.0, 64.0 / 1023.0, 64.0 / 1023.0}));
        }
        // A camera format (ACEScg and ACES2065-1 renders are analytic: planInput).
        g.appendTransform(cameraToAces(space));
        if (toneMappedOnInput(m, space)) {
            // Scene-linear Rec.2020 (inputPost tone-maps it into SDR).
            g.appendTransform(matrixTransform(inverse(workingToAp0(Working::LinearRec2020))));
            return;
        }
        g.appendTransform(matrixTransform(inverse(workingToAp0(w))));
    });
}

/// Bakes `chain` (or `f`) on a lattice: inputs index code values, outputs the log2 shaper of the working value;
/// texels stored per `values`.
std::shared_ptr<const BakedLut> bake(const std::string& key, BakedLut::Shaper shaper, BakedLut::Values values,
                                     const std::function<void(std::vector<float>& rgb)>& f) {
    {
        std::lock_guard lock(cacheMutex());
        if (const auto it = lutCache().find(key); it != lutCache().end()) {
            return it->second;
        }
    }
    const int n = kLutSize;
    std::vector<float> rgb(static_cast<std::size_t>(n) * n * n * 3);
    std::size_t k = 0;
    for (int b = 0; b < n; ++b) {
        for (int g = 0; g < n; ++g) {
            for (int r = 0; r < n; ++r) { // red fastest
                const std::array<int, 3> idx{r, g, b};
                for (int c = 0; c < 3; ++c) {
                    const double u = static_cast<double>(idx[static_cast<std::size_t>(c)]) / (n - 1);
                    rgb[k++] = static_cast<float>(shaper == BakedLut::Shaper::CodeValues
                                                      ? u
                                                      : linearFromShaper(u));
                }
            }
        }
    }
    f(rgb);
    const bool asinhValues = values == BakedLut::Values::Asinh;
    auto lut = std::make_shared<BakedLut>();
    lut->size = n;
    lut->shaper = shaper;
    lut->values = values;
    lut->key = key;
    lut->rgba.resize(static_cast<std::size_t>(n) * n * n * 4);
    const auto stored = [&](float v) {
        if (!std::isfinite(v)) {
            v = std::isnan(v) ? 0.0f : (v > 0 ? 65504.0f : -65504.0f);
        }
        return asinhValues ? static_cast<float>(std::asinh(static_cast<double>(v))) : v;
    };
    for (std::size_t i = 0, count = rgb.size() / 3; i < count; ++i) {
        lut->rgba[i * 4] = stored(rgb[i * 3]);
        lut->rgba[i * 4 + 1] = stored(rgb[i * 3 + 1]);
        lut->rgba[i * 4 + 2] = stored(rgb[i * 3 + 2]);
        lut->rgba[i * 4 + 3] = 1.0f;
    }
    std::lock_guard lock(cacheMutex());
    return lutCache().emplace(key, std::move(lut)).first->second;
}

std::shared_ptr<const BakedLut> bakeChain(const std::string& key, BakedLut::Shaper shaper, BakedLut::Values values,
                                          const Chain& chain) {
    return bake(key, shaper, values, [&](std::vector<float>& rgb) {
        OCIO::PackedImageDesc image(rgb.data(), static_cast<long>(rgb.size() / 3), 1, 3);
        chain.cpu->apply(image);
    });
}

/// A working value as an output LUT sees it: the shaper's domain, [0, kShaperMaxLinear] per channel.
Rgb3 inOutputDomain(const Rgb3& w) {
    return {std::clamp(w[0], 0.0, kShaperMaxLinear), std::clamp(w[1], 0.0, kShaperMaxLinear),
            std::clamp(w[2], 0.0, kShaperMaxLinear)};
}

/// The analytic decode of a code value, before primaries (PQ: nits; HLG: handled whole elsewhere).
double decodeCurve(TransferFunction tf, double v) {
    switch (tf) {
    case TransferFunction::SRGB: return srgbToLinear(v);
    case TransferFunction::Linear: return v;
    case TransferFunction::PQ: return pqToNits(v);
    case TransferFunction::Gamma22: return std::pow(std::max(v, 0.0), 2.2);
    case TransferFunction::Gamma28: return std::pow(std::max(v, 0.0), 2.8);
    default: return rec709ToLinear(v);
    }
}

} // namespace

Rgb3 apply(const Matrix3& m, const Rgb3& v) {
    return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
            m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
}

Working workingOf(ColorManagementMode mode) {
    switch (mode) {
    case ColorManagementMode::Aces13:
    case ColorManagementMode::WideGamut:
    case ColorManagementMode::WideGamutMinimal: return Working::AcesCg;
    case ColorManagementMode::HdrPq:
    case ColorManagementMode::HdrHlg: return Working::LinearRec2020;
    case ColorManagementMode::DisplayRec709:
    case ColorManagementMode::Disabled: break;
    }
    return Working::LinearRec709;
}

// ---- Curves ------------------------------------------------------------------------------------------------------

double acesCctFromLinear(double linear) {
    if (linear <= 0.0078125) {
        return 10.5402377416545 * linear + 0.0729055341958355;
    }
    return (std::log2(linear) + 9.72) / 17.52;
}

double linearFromAcesCct(double cct) {
    if (cct <= 0.155251141552511) {
        return (cct - 0.0729055341958355) / 10.5402377416545;
    }
    if (cct < kCctMax) {
        return std::exp2(cct * 17.52 - 9.72);
    }
    return 65504.0;
}

double shaperOf(double linear) {
    return std::clamp((std::log2(std::clamp(linear, 0.0, kShaperMaxLinear) + kShaperFloor) + 12.0) / 19.0, 0.0, 1.0);
}

double linearFromShaper(double s) {
    return std::clamp(std::exp2(std::clamp(s, 0.0, 1.0) * 19.0 - 12.0) - kShaperFloor, 0.0, kShaperMaxLinear);
}

double pqToNits(double code) {
    const double e = std::pow(std::clamp(code, 0.0, 1.0), 1.0 / kPqM2);
    return 10000.0 * std::pow(std::max(e - kPqC1, 0.0) / (kPqC2 - kPqC3 * e), 1.0 / kPqM1);
}

double nitsToPq(double nits) {
    const double y = std::pow(std::clamp(nits / 10000.0, 0.0, 1.0), kPqM1);
    return std::pow((kPqC1 + kPqC2 * y) / (1.0 + kPqC3 * y), kPqM2);
}

double hlgOetf(double e) {
    e = std::max(e, 0.0);
    return e <= 1.0 / 12.0 ? std::sqrt(3.0 * e) : kHlgA * std::log(12.0 * e - kHlgB) + kHlgC;
}

double hlgInverseOetf(double v) {
    v = std::max(v, 0.0);
    return v <= 0.5 ? v * v / 3.0 : (std::exp((v - kHlgC) / kHlgA) + kHlgB) / 12.0;
}

double hlgSystemGamma(double peakNits) { return 1.2 + 0.42 * std::log10(std::max(peakNits, 1.0) / 1000.0); }

Rgb3 hlgToDisplayNits(const Rgb3& signal, double peakNits) {
    const Rgb3 scene{hlgInverseOetf(signal[0]), hlgInverseOetf(signal[1]), hlgInverseOetf(signal[2])};
    const double ys = lumaRec2020(scene);
    const double k = ys > 0.0 ? peakNits * std::pow(ys, hlgSystemGamma(peakNits) - 1.0) : 0.0;
    return {scene[0] * k, scene[1] * k, scene[2] * k};
}

Rgb3 displayNitsToHlg(const Rgb3& nits, double peakNits) {
    const double gamma = hlgSystemGamma(peakNits);
    const double yd = std::max(lumaRec2020(nits), 0.0);
    // Inverse OOTF: E = (F / peak) * (Yd / peak)^((1 - gamma) / gamma).
    const double k = yd > 0.0 ? std::pow(yd / peakNits, (1.0 - gamma) / gamma) / peakNits : 0.0;
    return {hlgOetf(nits[0] * k), hlgOetf(nits[1] * k), hlgOetf(nits[2] * k)};
}

namespace {

/// toneMapToSdr's luminance ratio (0.6: shared with the Rec.2020 and P3-D65 SDR outputs of HDR setups).
double toneMapRatio(const Rgb3& c, double referenceWhiteNits, double peakNits) {
    const double y = lumaRec2020(c);
    double ratio = 1.0;
    if (y > 0.0 && peakNits > referenceWhiteNits) {
        // BT.2390 EETF in PQ, the source range [0, peak] mastered onto [0, reference white].
        const double nits = y * referenceWhiteNits;
        const double pqPeak = nitsToPq(peakNits);
        const double e1 = std::min(nitsToPq(nits) / pqPeak, 1.0);
        const double maxLum = nitsToPq(referenceWhiteNits) / pqPeak;
        const double ks = 1.5 * maxLum - 0.5;
        double e2 = e1;
        if (e1 >= ks && ks < 1.0) {
            const double t = (e1 - ks) / (1.0 - ks);
            const double t2 = t * t, t3 = t2 * t;
            e2 = (2 * t3 - 3 * t2 + 1) * ks + (t3 - 2 * t2 + t) * (1.0 - ks) + (-2 * t3 + 3 * t2) * maxLum;
        }
        const double out = pqToNits(e2 * pqPeak);
        ratio = out / nits;
        if (nits > peakNits) { // above the master's peak: held at the SDR white
            ratio = referenceWhiteNits / nits;
        }
    }
    return ratio;
}

} // namespace

Rgb3 toneMapToSdr(const Rgb3& c, double referenceWhiteNits, double peakNits) {
    const double ratio = toneMapRatio(c, referenceWhiteNits, peakNits);
    const Rgb3 scaled{c[0] * ratio, c[1] * ratio, c[2] * ratio};
    const Rgb3 r709 = color::apply(convert(kRec2020, kRec709), scaled);
    return {std::clamp(r709[0], 0.0, 1.0), std::clamp(r709[1], 0.0, 1.0), std::clamp(r709[2], 0.0, 1.0)};
}

std::optional<Rgb3> hdrCaptureToSdr(TransferFunction transfer, const Rgb3& code) {
    const Rgb3 c{std::clamp(code[0], 0.0, 1.0), std::clamp(code[1], 0.0, 1.0), std::clamp(code[2], 0.0, 1.0)};
    Rgb3 nits;
    if (transfer == TransferFunction::PQ) {
        nits = {pqToNits(c[0]), pqToNits(c[1]), pqToNits(c[2])};
    } else if (transfer == TransferFunction::HLG) {
        nits = hlgToDisplayNits(c, kCapturePeakNits);
    } else {
        return std::nullopt;
    }
    const double white = kCaptureReferenceWhiteNits;
    return toneMapToSdr({nits[0] / white, nits[1] / white, nits[2] / white}, white, kCapturePeakNits);
}

Matrix3 rec2020ToRec709() { return convert(kRec2020, kRec709); }

// ---- Plans -----------------------------------------------------------------------------------------------------------

InputColorSpace inputSpaceOf(const ColorInfo& color, bool isRgb) { return autoInputColorSpace(color, isRgb); }

namespace {

/// 0.6 (#72): a scene-referred picture (camera log, HDR, linear, ACES): what gamut compression and Minimal Tone
/// Mapping's roll-off apply to. Display-referred pictures (Rec.709, sRGB, Rec.2020 SDR media, graphics) are inside every
/// working gamut and below its white, and pass exactly as they are.
bool sceneReferred(InputColorSpace space, bool graphics) {
    return !graphics && space != InputColorSpace::Rec709 && space != InputColorSpace::Srgb && space != InputColorSpace::Rec2020;
}

/// The working value of an input plan's result after its 0.6 operations (gamut compression, then the roll-off).
Rgb3 inputFinish(const InputPlan& plan, Rgb3 v) {
    if (plan.gamutStrength > 0.0) {
        v = gamutCompress(v, plan.gamutStrength);
    }
    if (plan.rolloffWhite > 0.0) {
        v = minimalRolloff(v, plan.rolloffWhite);
    }
    return v;
}

} // namespace

static std::expected<InputPlan, Error> planInputBase(const ColorManagement& m, InputColorSpace space, bool limitedRange,
                                                     bool graphics);

std::expected<InputPlan, Error> planInput(const ColorManagement& m, InputColorSpace space, bool limitedRange, bool graphics) {
    auto plan = planInputBase(m, space, limitedRange, graphics);
    if (plan && m.managed() && sceneReferred(space, graphics)) {
        if (m.compressesGamut()) {
            plan->gamutStrength = std::clamp(m.gamutCompressionStrength, 0.0, 1.0);
        }
        if (m.mode == ColorManagementMode::WideGamutMinimal) {
            plan->rolloffWhite = minimalRolloffWhite(m);
        }
    }
    return plan;
}

static std::expected<InputPlan, Error> planInputBase(const ColorManagement& m, InputColorSpace space, bool limitedRange,
                                                     bool graphics) {
    const Working w = workingOf(m.mode);
    InputPlan plan;
    if (m.mode == ColorManagementMode::Disabled) {
        return plan; // no conversion: code values through the BT.709 curve, as they are
    }
    if (m.mode == ColorManagementMode::DisplayRec709 &&
        (space == InputColorSpace::Rec2100Pq || space == InputColorSpace::Rec2100Hlg)) {
        // Direct Rec.709: HDR media tone-mapped into SDR on input, as HDR game captures always were (I.9, gaming).
        plan.kind = InputPlan::Kind::ToneMapSdr;
        plan.transfer = space == InputColorSpace::Rec2100Pq ? TransferFunction::PQ : TransferFunction::HLG;
        plan.matrix = convert(kRec2020, kRec709);
        plan.hlgPeakNits = kCapturePeakNits;
        return plan;
    }
    const bool aces = m.mode == ColorManagementMode::Aces13;
    const bool displayReferred = aces && (graphics || space == InputColorSpace::Rec709 || space == InputColorSpace::Srgb ||
                                          space == InputColorSpace::Rec2020);
    if (isLog(space) || displayReferred) {
        const bool range = limitedRange && isLog(space);
        auto chain = inputChain(m, space, range, displayReferred);
        if (!chain) {
            return std::unexpected(chain.error());
        }
        const std::string key = inputKey(m, space, range, displayReferred);
        plan.kind = InputPlan::Kind::Lut;
        plan.lut = bakeChain(key, BakedLut::Shaper::CodeValues, BakedLut::Values::Asinh, **chain);
        if (toneMappedOnInput(m, space)) {
            plan.kind = InputPlan::Kind::LutToneMapSdr; // scene light, tone-mapped as an HDR capture
            plan.matrix = convert(kRec2020, kRec709);
        }
        return plan;
    }
    // Analytic: the curve, then the primaries into the working space.
    const Chromaticities* primaries = &kRec709;
    switch (space) {
    case InputColorSpace::Srgb: plan.transfer = TransferFunction::SRGB; break;
    case InputColorSpace::LinearRec709: plan.transfer = TransferFunction::Linear; break;
    case InputColorSpace::AcesCg: // linear values: the primaries alone (exact, no LUT)
        plan.transfer = TransferFunction::Linear;
        primaries = &kAp1;
        break;
    case InputColorSpace::Aces2065:
        plan.transfer = TransferFunction::Linear;
        primaries = &kAp0;
        break;
    case InputColorSpace::Rec2020: primaries = &kRec2020; break;
    case InputColorSpace::Rec2100Pq:
        plan.transfer = TransferFunction::PQ;
        primaries = &kRec2020;
        // nits -> working: 1.0 = the reference white (HDR); 100 nits in an SDR or ACES project (display-referred
        // HDR read as scene light, graded down there).
        // 0.6: Minimal Tone Mapping reads HDR as the HDR setups do (1.0 = the reference white): its highlights up to
        // the peak then roll off into white instead of clipping.
        plan.scale = 1.0 / (m.isHdr() || m.mode == ColorManagementMode::WideGamutMinimal ? m.referenceWhiteNits : 100.0);
        break;
    case InputColorSpace::Rec2100Hlg:
        plan.transfer = TransferFunction::HLG;
        primaries = &kRec2020;
        plan.hlgPeakNits = m.peakNits;
        plan.scale = 1.0 / (m.isHdr() || m.mode == ColorManagementMode::WideGamutMinimal ? m.referenceWhiteNits : 100.0);
        break;
    default: break; // Rec.709
    }
    plan.matrix = convert(*primaries, chromaticitiesOf(w));
    return plan;
}

std::expected<OutputPlan, Error> planOutput(const ColorManagement& m, OutputTarget target, bool conform) {
    OutputPlan plan;
    plan.referenceWhiteNits = m.referenceWhiteNits;
    plan.peakNits = m.peakNits;
    const Working w = workingOf(m.mode);
    const bool sdr = isSdrTarget(target);
    const Chromaticities& display = sdrPrimaries(target);
    plan.curve = target == OutputTarget::P3D65 ? OutputPlan::Curve::Gamma26 : OutputPlan::Curve::Bt709;
    if (sdr && conform && (m.isHdr() || m.isWideGamutDirect())) {
        // 0.6: SDR Conform of an HDR or Wide Gamut setup (analytic: output.frag's conform()).
        plan.kind = OutputPlan::Kind::Conform;
        plan.conform = m.sdrConform;
        plan.conformIn = convert(chromaticitiesOf(w), kRec2020);
        plan.matrix = convert(kRec2020, display);
        plan.label = "SDR Conform";
        return plan;
    }
    if (m.mode == ColorManagementMode::Aces13) {
        const std::string key = "out:aces-light:" + targetName(target);
        auto chain = chainFor(key, [&](OCIO::GroupTransform& g) { acesOutputLight(g, target); });
        if (!chain) {
            return std::unexpected(chain.error());
        }
        plan.kind = OutputPlan::Kind::Lut;
        plan.lut = bakeChain(key, BakedLut::Shaper::Log2, BakedLut::Values::AsIs, **chain); // light: linear
        plan.finish = target == OutputTarget::P3D65 ? OutputPlan::Finish::Gamma26
                      : sdr                         ? OutputPlan::Finish::Bt1886
                      : target == OutputTarget::Pq  ? OutputPlan::Finish::Pq
                                                    : OutputPlan::Finish::Hlg;
        plan.displayNits = 100.0;
        plan.peakNits = 1000.0; // the ACES HDR output transform's display
        plan.label = target == OutputTarget::Sdr          ? "ACES 1.0 SDR Video"
                     : target == OutputTarget::Rec2020Sdr ? "ACES 1.0 SDR Video (Rec.2020)"
                     : target == OutputTarget::P3D65      ? "ACES 1.0 SDR Video (P3-D65)"
                                                          : "ACES 1.1 HDR Video (1000 nits)";
        return plan;
    }
    if (sdr) {
        if (m.isWideGamutDirect()) {
            // Wide Gamut (No / Minimal Tone Mapping): ACEScg -> the display's primaries, encoded, clipped above white
            // (Minimal: its scene-referred layers were rolled off as they were read).
            plan.kind = OutputPlan::Kind::Rec709;
            plan.matrix = convert(kAp1, display);
            plan.label = m.mode == ColorManagementMode::WideGamutMinimal ? "Wide Gamut · minimal tone mapping"
                                                                         : "Wide Gamut · no tone mapping";
            return plan;
        }
        if (m.isHdr()) {
            // An HDR project on an SDR display or file: the tone-mapped preview (and SDR export), analytic.
            plan.kind = OutputPlan::Kind::ToneMapSdr;
            plan.matrix = convert(kRec2020, display);
            plan.label = "HDR · tone-mapped SDR preview";
            return plan;
        }
        // Direct Rec.709 and Disable Color Management: today's output, byte for byte; 0.6: the Rec.2020 / P3-D65 SDR
        // exports convert the Rec.709 working space's primaries (a matrix) and encode.
        plan.kind = OutputPlan::Kind::Rec709;
        if (target != OutputTarget::Sdr) {
            plan.matrix = convert(kRec709, display);
        }
        return plan;
    }
    // PQ or HLG: the working space in Rec.2020 nits (an SDR project: 1.0 = the reference white, BT.2408).
    plan.kind = target == OutputTarget::Pq ? OutputPlan::Kind::Pq : OutputPlan::Kind::Hlg;
    plan.matrix = convert(chromaticitiesOf(w), kRec2020);
    return plan;
}

std::expected<Rgb3, Error> applyInput(const ColorManagement& m, InputColorSpace space, bool limitedRange, bool graphics,
                                      const Rgb3& code) {
    const auto plan = planInput(m, space, limitedRange, graphics);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    if (plan->kind == InputPlan::Kind::Lut || plan->kind == InputPlan::Kind::LutToneMapSdr) {
        const bool aces = m.mode == ColorManagementMode::Aces13;
        const bool displayReferred = aces && (graphics || space == InputColorSpace::Rec709 ||
                                              space == InputColorSpace::Srgb || space == InputColorSpace::Rec2020);
        auto chain = inputChain(m, space, limitedRange && isLog(space), displayReferred);
        if (!chain) {
            return std::unexpected(chain.error());
        }
        return inputFinish(*plan, inputPost(m, space, run((*chain)->cpu, code)));
    }
    return runInputPlan(*plan, code);
}

std::expected<Rgb3, Error> applyOutput(const ColorManagement& m, OutputTarget target, const Rgb3& working) {
    const auto plan = planOutput(m, target);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    if (plan->kind != OutputPlan::Kind::Lut) {
        return runOutputPlan(*plan, working);
    }
    // ACES: OpenColorIO's whole chain (the output transform and its display), as code values.
    auto chain = chainFor("out:aces:" + targetName(target), [&](OCIO::GroupTransform& g) { acesOutput(g, target); });
    if (!chain) {
        return std::unexpected(chain.error());
    }
    const Rgb3 code = run((*chain)->cpu, inOutputDomain(working));
    return Rgb3{std::clamp(code[0], 0.0, 1.0), std::clamp(code[1], 0.0, 1.0), std::clamp(code[2], 0.0, 1.0)};
}

Rgb3 runInputPlan(const InputPlan& plan, const Rgb3& code) {
    const Rgb3 c{std::clamp(code[0], 0.0, 1.0), std::clamp(code[1], 0.0, 1.0), std::clamp(code[2], 0.0, 1.0)};
    if (plan.kind == InputPlan::Kind::Lut) {
        return inputFinish(plan, plan.lut ? plan.lut->sample(c) : Rgb3{});
    }
    if (plan.kind == InputPlan::Kind::LutToneMapSdr) {
        const Rgb3 scene = plan.lut ? plan.lut->sample(c) : Rgb3{};
        return toneMapToSdr({std::max(scene[0], 0.0), std::max(scene[1], 0.0), std::max(scene[2], 0.0)},
                            kCaptureReferenceWhiteNits, kCapturePeakNits);
    }
    if (plan.kind == InputPlan::Kind::ToneMapSdr) {
        return hdrCaptureToSdr(plan.transfer, c).value_or(Rgb3{});
    }
    Rgb3 lin;
    if (plan.transfer == TransferFunction::HLG) {
        lin = hlgToDisplayNits(c, plan.hlgPeakNits);
    } else {
        lin = {decodeCurve(plan.transfer, c[0]), decodeCurve(plan.transfer, c[1]), decodeCurve(plan.transfer, c[2])};
    }
    const Rgb3 w = color::apply(plan.matrix, lin);
    return inputFinish(plan, {w[0] * plan.scale, w[1] * plan.scale, w[2] * plan.scale});
}

namespace {

double encodeCurve(OutputPlan::Curve curve, double v) {
    if (curve == OutputPlan::Curve::Gamma26) {
        return std::pow(std::clamp(v, 0.0, 1.0), 1.0 / 2.6);
    }
    return linearToRec709(v);
}

} // namespace

Rgb3 runOutputPlan(const OutputPlan& plan, const Rgb3& working) {
    switch (plan.kind) {
    case OutputPlan::Kind::Lut: {
        const Rgb3 x = color::apply(plan.matrix, working);
        const Rgb3 light = plan.lut ? plan.lut->sample({shaperOf(x[0]), shaperOf(x[1]), shaperOf(x[2])}) : Rgb3{};
        switch (plan.finish) {
        case OutputPlan::Finish::Bt1886:
            return {std::pow(std::clamp(light[0], 0.0, 1.0), 1.0 / 2.4), std::pow(std::clamp(light[1], 0.0, 1.0), 1.0 / 2.4),
                    std::pow(std::clamp(light[2], 0.0, 1.0), 1.0 / 2.4)};
        case OutputPlan::Finish::Gamma26:
            return {std::pow(std::clamp(light[0], 0.0, 1.0), 1.0 / 2.6), std::pow(std::clamp(light[1], 0.0, 1.0), 1.0 / 2.6),
                    std::pow(std::clamp(light[2], 0.0, 1.0), 1.0 / 2.6)};
        case OutputPlan::Finish::Pq:
            return {nitsToPq(std::max(light[0], 0.0) * plan.displayNits), nitsToPq(std::max(light[1], 0.0) * plan.displayNits),
                    nitsToPq(std::max(light[2], 0.0) * plan.displayNits)};
        case OutputPlan::Finish::Hlg: {
            const Rgb3 s = displayNitsToHlg({std::max(light[0], 0.0) * plan.displayNits,
                                             std::max(light[1], 0.0) * plan.displayNits,
                                             std::max(light[2], 0.0) * plan.displayNits},
                                            plan.peakNits);
            return {std::min(s[0], 1.0), std::min(s[1], 1.0), std::min(s[2], 1.0)};
        }
        case OutputPlan::Finish::AsIs: break;
        }
        return light;
    }
    case OutputPlan::Kind::ToneMapSdr: {
        // toneMapToSdr's luminance mapping, then the target's primaries (`matrix`: Rec.709 as before; 0.6: Rec.2020,
        // P3-D65), clipped, encoded.
        const Rgb3 c{std::max(working[0], 0.0), std::max(working[1], 0.0), std::max(working[2], 0.0)};
        const double ratio = toneMapRatio(c, plan.referenceWhiteNits, plan.peakNits);
        const Rgb3 t = color::apply(plan.matrix, {c[0] * ratio, c[1] * ratio, c[2] * ratio});
        return {encodeCurve(plan.curve, t[0]), encodeCurve(plan.curve, t[1]), encodeCurve(plan.curve, t[2])};
    }
    case OutputPlan::Kind::Conform: return sdrConformPixel(plan, working).code;
    case OutputPlan::Kind::Srgb:
        return {linearToSrgb(working[0]), linearToSrgb(working[1]), linearToSrgb(working[2])};
    case OutputPlan::Kind::Pq: {
        const Rgb3 v = color::apply(plan.matrix, working);
        return {nitsToPq(std::max(v[0], 0.0) * plan.referenceWhiteNits), nitsToPq(std::max(v[1], 0.0) * plan.referenceWhiteNits),
                nitsToPq(std::max(v[2], 0.0) * plan.referenceWhiteNits)};
    }
    case OutputPlan::Kind::Hlg: {
        const Rgb3 v = color::apply(plan.matrix, working);
        const Rgb3 s = displayNitsToHlg({std::max(v[0], 0.0) * plan.referenceWhiteNits,
                                         std::max(v[1], 0.0) * plan.referenceWhiteNits,
                                         std::max(v[2], 0.0) * plan.referenceWhiteNits},
                                        plan.peakNits);
        return {std::min(s[0], 1.0), std::min(s[1], 1.0), std::min(s[2], 1.0)}; // above the display's peak: the top
    }
    case OutputPlan::Kind::Rec709: break;
    }
    const Rgb3 v = color::apply(plan.matrix, working);
    return {encodeCurve(plan.curve, v[0]), encodeCurve(plan.curve, v[1]), encodeCurve(plan.curve, v[2])};
}

// ---- 0.6 (#72): gamut compression, Minimal Tone Mapping, SDR Conform ---------------------------------------------

namespace {

/// Extended Reinhard above a knee: v up to `knee` as it is, then slope 1 there and `white` landing on 1.0.
double kneeCurve(double v, double knee, double white) {
    if (v <= knee) {
        return v;
    }
    if (white <= 1.0) {
        return std::min(v, 1.0);
    }
    if (v >= white) {
        return 1.0;
    }
    const double a = 1.0 - knee;
    const double t = (v - knee) / a;
    const double big = (white - knee) / a;
    return knee + a * t * (1.0 + t / (big * big)) / (1.0 + t);
}

/// ACES 1.3 RGC's compression of one distance (the CTL's compress()).
double compressDistance(double dist, double limit, double threshold) {
    constexpr double kPower = 1.2;
    if (dist < threshold) {
        return dist;
    }
    const double scale =
        (limit - threshold) / std::pow(std::pow((1.0 - threshold) / (limit - threshold), -kPower) - 1.0, 1.0 / kPower);
    const double nd = (dist - threshold) / scale;
    return threshold + scale * nd / std::pow(1.0 + std::pow(nd, kPower), 1.0 / kPower);
}

double acesFilmic(double x) { return x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14); }

} // namespace

OutputTarget outputTargetOf(OutputColorSpace space, OutputTarget dynamicRange) {
    if (!isSdrTarget(dynamicRange)) {
        return dynamicRange;
    }
    return space == OutputColorSpace::Rec2020 ? OutputTarget::Rec2020Sdr
           : space == OutputColorSpace::P3D65 ? OutputTarget::P3D65
                                              : OutputTarget::Sdr;
}

Rgb3 gamutCompress(const Rgb3& rgb, double strength) {
    strength = std::clamp(strength, 0.0, 1.0);
    const double ach = std::max({rgb[0], rgb[1], rgb[2]});
    if (strength <= 0.0 || ach == 0.0) {
        return rgb;
    }
    static constexpr std::array<double, 3> kThreshold{0.815, 0.803, 0.880}; // cyan, magenta, yellow
    static constexpr std::array<double, 3> kLimit{1.147, 1.264, 1.312};
    Rgb3 out{};
    for (std::size_t i = 0; i < 3; ++i) {
        const double dist = (ach - rgb[i]) / std::abs(ach);
        const double compressed = ach - compressDistance(dist, kLimit[i], kThreshold[i]) * std::abs(ach);
        out[i] = rgb[i] + (compressed - rgb[i]) * strength;
    }
    return out;
}

double minimalRolloffCurve(double v, double white) { return kneeCurve(v, kMinimalKnee, white); }

Rgb3 minimalRolloff(const Rgb3& rgb, double white) {
    const double m = std::max({rgb[0], rgb[1], rgb[2]});
    if (m <= kMinimalKnee) {
        return rgb;
    }
    const double k = minimalRolloffCurve(m, white) / m;
    return {rgb[0] * k, rgb[1] * k, rgb[2] * k};
}

double minimalRolloffWhite(const ColorManagement& m) { return std::max(m.peakNits / m.referenceWhiteNits, 1.0); }

double conformToneMap(ToneMapOperator op, double s, double peak, double targetPeakNits) {
    if (s <= 0.0) {
        return 0.0;
    }
    if (op == ToneMapOperator::Aces) {
        return std::clamp(acesFilmic(0.6 * s) / acesFilmic(0.6 * std::max(peak, 1.0)), 0.0, 1.0);
    }
    if (peak <= 1.0 || s >= peak) {
        return std::min(s, 1.0);
    }
    if (op == ToneMapOperator::Reinhard) {
        return std::min(1.0, s * (1.0 + s / (peak * peak)) / (1.0 + s));
    }
    // BT.2390 EETF in PQ, from [0, peak] onto [0, 1] (nits: x the target's white), as toneMapToSdr.
    const double nits = s * targetPeakNits;
    const double pqPeak = nitsToPq(peak * targetPeakNits);
    const double e1 = std::min(nitsToPq(nits) / pqPeak, 1.0);
    const double maxLum = nitsToPq(targetPeakNits) / pqPeak;
    const double ks = 1.5 * maxLum - 0.5;
    double e2 = e1;
    if (e1 >= ks && ks < 1.0) {
        const double t = (e1 - ks) / (1.0 - ks);
        const double t2 = t * t, t3 = t2 * t;
        e2 = (2 * t3 - 3 * t2 + 1) * ks + (t3 - 2 * t2 + t) * (1.0 - ks) + (-2 * t3 + 3 * t2) * maxLum;
    }
    return std::min(pqToNits(e2 * pqPeak) / targetPeakNits, 1.0);
}

ConformPixel sdrConformPixel(const OutputPlan& plan, const Rgb3& working) {
    const SdrConform& cf = plan.conform;
    const Rgb3 c = color::apply(plan.conformIn, working); // linear Rec.2020, 1.0 = the reference white
    const double y = std::max(lumaRec2020(c), 0.0);
    const double gain = cf.paperWhiteNits / cf.targetPeakNits; // where the reference white lands on the SDR display
    const double s = y * gain;
    const double peak = plan.peakNits / plan.referenceWhiteNits * gain;
    ConformPixel out;
    out.toneMapped = s > 1.0 + 1e-6;
    const double yt = conformToneMap(cf.toneMap, s, peak, cf.targetPeakNits);
    const double k = y > 0.0 ? yt / y : gain;
    Rgb3 t = color::apply(plan.matrix, {c[0] * k, c[1] * k, c[2] * k}); // the target's primaries, 1.0 = its white
    constexpr double kTolerance = 1.0 / 1024.0;
    const double lo = std::min({t[0], t[1], t[2]});
    const double hi = std::max({t[0], t[1], t[2]});
    out.gamutMapped = lo < -kTolerance || hi > 1.0 + kTolerance;
    switch (cf.gamut) {
    case GamutMapping::AcesRgc: t = gamutCompress(t, 1.0); break;
    case GamutMapping::Saturation: {
        // Towards the pixel's own luminance (D65 greys are equal in every D65 RGB space) until it fits.
        double f = 1.0;
        if (lo < 0.0) {
            f = std::min(f, yt / (yt - lo));
        }
        if (hi > 1.0) { // at the white itself only a grey fits
            f = std::min(f, yt < 1.0 ? (1.0 - yt) / (hi - yt) : 0.0);
        }
        for (double& v : t) {
            v = yt + (v - yt) * f;
        }
        break;
    }
    case GamutMapping::Clip: break;
    }
    if (cf.legalRange) {
        // A soft clip on the largest component above 90 %: nothing piles up at 100 %.
        const double m = std::max({t[0], t[1], t[2]});
        if (m > 0.9) {
            const double scale = kneeCurve(m, 0.9, 1.25) / m;
            t = {t[0] * scale, t[1] * scale, t[2] * scale};
        }
    }
    out.code = {encodeCurve(plan.curve, t[0]), encodeCurve(plan.curve, t[1]), encodeCurve(plan.curve, t[2])};
    return out;
}

std::string describeOutput(const ColorManagement& m, OutputTarget target, bool conform) {
    const bool sdr = isSdrTarget(target);
    std::string working;
    switch (m.mode) {
    case ColorManagementMode::Aces13: working = "ACES (ACEScg)"; break;
    case ColorManagementMode::WideGamut:
    case ColorManagementMode::WideGamutMinimal: working = "ACEScg"; break;
    case ColorManagementMode::HdrPq:
    case ColorManagementMode::HdrHlg: working = "Rec.2020 HDR"; break;
    case ColorManagementMode::Disabled: working = "code values"; break;
    case ColorManagementMode::DisplayRec709: working = "Rec.709"; break;
    }
    const std::string out = target == OutputTarget::Sdr          ? "Rec.709"
                            : target == OutputTarget::Rec2020Sdr ? "Rec.2020 (SDR)"
                            : target == OutputTarget::P3D65      ? "P3-D65 (2.6)"
                            : target == OutputTarget::Pq         ? "Rec.2100 PQ"
                                                                 : "Rec.2100 HLG";
    std::string step;
    if (sdr && conform && (m.isHdr() || m.isWideGamutDirect())) {
        step = "SDR Conform (" + std::string(displayName(m.sdrConform.toneMap)) + ", " +
               std::string(displayName(m.sdrConform.gamut)) + (m.sdrConform.legalRange ? ", legal range" : "") + ")";
    } else if (m.mode == ColorManagementMode::Aces13) {
        step = sdr ? "ACES SDR output transform" : "ACES HDR output transform (1000 nits)";
    } else if (sdr && m.isHdr()) {
        step = "tone-mapped (BT.2390)";
    } else if (sdr && m.mode == ColorManagementMode::WideGamut) {
        step = "highlights above white clipped";
    } else if (sdr && m.mode == ColorManagementMode::WideGamutMinimal) {
        step = "scene-referred highlights rolled off on input, above white clipped";
    } else if (!sdr) {
        char nits[32];
        std::snprintf(nits, sizeof nits, "%g", m.referenceWhiteNits);
        step = std::string("1.0 = ") + nits + " nits";
    }
    std::string text = working + " \xE2\x86\x92 " + (step.empty() ? "" : step + " \xE2\x86\x92 ") + out;
    if (m.compressesGamut()) {
        text += " \xC2\xB7 gamut compression on input";
    }
    return text;
}

Rgb3 BakedLut::sample(const Rgb3& uvw) const {
    if (size < 2 || rgba.size() < static_cast<std::size_t>(size) * size * size * 4) {
        return {};
    }
    const int n = size;
    std::array<int, 3> i0{}, i1{};
    std::array<double, 3> t{};
    for (int c = 0; c < 3; ++c) {
        const double x = std::clamp(uvw[static_cast<std::size_t>(c)], 0.0, 1.0) * (n - 1);
        const int lo = std::min(static_cast<int>(std::floor(x)), n - 2);
        i0[static_cast<std::size_t>(c)] = lo;
        i1[static_cast<std::size_t>(c)] = lo + 1;
        t[static_cast<std::size_t>(c)] = x - lo;
    }
    const auto at = [&](int r, int g, int b, int ch) {
        return static_cast<double>(rgba[((static_cast<std::size_t>(b) * n + g) * n + r) * 4 + static_cast<std::size_t>(ch)]);
    };
    // Tetrahedral (as color::apply3d and the shaders): the cell is split into six tetrahedra along its grey diagonal.
    const double fr = t[0], fg = t[1], fb = t[2];
    std::array<int, 3> da{}, db{};
    double w0 = 0, wa = 0, wb = 0, w1 = 0;
    if (fr > fg) {
        if (fg > fb) {
            da = {1, 0, 0}, db = {1, 1, 0}, w0 = 1 - fr, wa = fr - fg, wb = fg - fb, w1 = fb;
        } else if (fr > fb) {
            da = {1, 0, 0}, db = {1, 0, 1}, w0 = 1 - fr, wa = fr - fb, wb = fb - fg, w1 = fg;
        } else {
            da = {0, 0, 1}, db = {1, 0, 1}, w0 = 1 - fb, wa = fb - fr, wb = fr - fg, w1 = fg;
        }
    } else {
        if (fb > fg) {
            da = {0, 0, 1}, db = {0, 1, 1}, w0 = 1 - fb, wa = fb - fg, wb = fg - fr, w1 = fr;
        } else if (fb > fr) {
            da = {0, 1, 0}, db = {0, 1, 1}, w0 = 1 - fg, wa = fg - fb, wb = fb - fr, w1 = fr;
        } else {
            da = {0, 1, 0}, db = {1, 1, 0}, w0 = 1 - fg, wa = fg - fr, wb = fr - fb, w1 = fb;
        }
    }
    Rgb3 out{};
    for (int ch = 0; ch < 3; ++ch) {
        const double v = w0 * at(i0[0], i0[1], i0[2], ch) + wa * at(i0[0] + da[0], i0[1] + da[1], i0[2] + da[2], ch) +
                         wb * at(i0[0] + db[0], i0[1] + db[1], i0[2] + db[2], ch) + w1 * at(i1[0], i1[1], i1[2], ch);
        out[static_cast<std::size_t>(ch)] = values == Values::Asinh ? std::sinh(v) : v;
    }
    return out;
}

std::vector<std::string> builtinTransformsUsed() {
    std::vector<std::string> out{kAp0ToXyz, kOdtSdr, kOdtHdr1000, kDisplay709, kDisplayPq, kDisplayHlg, kDisplay2020,
                                 kDisplayP3D65};
    for (const InputColorSpace s : allInputColorSpaces()) {
        if (const char* camera = cameraFormat(s).builtin) {
            out.emplace_back(camera);
        }
    }
    return out;
}

std::string ocioVersion() { return OCIO::GetVersion(); }

std::expected<Rgb3, Error> applyBuiltin(const std::string& style, const Rgb3& rgb, bool inverse) {
    auto chain = chainFor("builtin:" + style + (inverse ? ":inv" : ""), [&](OCIO::GroupTransform& g) {
        g.appendTransform(builtin(style.c_str(), inverse));
    });
    if (!chain) {
        return std::unexpected(chain.error());
    }
    return run((*chain)->cpu, rgb);
}

std::expected<Rgb3, Error> cameraToAces2065(InputColorSpace space, const Rgb3& code) {
    if (!isLog(space)) {
        return std::unexpected(Error{ErrorCode::Unsupported, std::string(toString(space)) + " is not a camera format"});
    }
    auto chain = chainFor("camera:" + std::string(toString(space)),
                          [&](OCIO::GroupTransform& g) { g.appendTransform(cameraToAces(space)); });
    if (!chain) {
        return std::unexpected(chain.error());
    }
    return run((*chain)->cpu, code);
}

std::expected<Rgb3, Error> applyBuiltinConfigSpace(const std::string& config, const std::string& colorSpace, const Rgb3& rgb) {
    try {
        const OCIO::ConstConfigRcPtr c = OCIO::Config::CreateFromBuiltinConfig(config.c_str());
        const OCIO::ConstCPUProcessorRcPtr cpu =
            c->getProcessor(colorSpace.c_str(), "ACES2065-1")->getDefaultCPUProcessor();
        return run(cpu, rgb);
    } catch (const OCIO::Exception& e) {
        return std::unexpected(Error{ErrorCode::Unsupported, "OpenColorIO's " + config + " has no " + colorSpace + ": " + e.what()});
    }
}

} // namespace wabcut::color
