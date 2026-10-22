// Colour management (ColorSpaces.h): working spaces, input and output plans, OpenColorIO built-in transforms baked into
// 3D LUTs, and the CPU references.

#include <wabcut/color/ColorSpaces.h>

#include <OpenColorIO/OpenColorIO.h>

#include <algorithm>
#include <cmath>
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

Rgb3 xyzOf(double x, double y) { return {x / y, 1.0, (1.0 - x - y) / y}; }

Matrix3 rgbToXyz(const Chromaticities& c) {
    const Rgb3 r = xyzOf(c.rx, c.ry), g = xyzOf(c.gx, c.gy), b = xyzOf(c.bx, c.by);
    const Matrix3 p{r[0], g[0], b[0], r[1], g[1], b[1], r[2], g[2], b[2]};
    const Rgb3 s = color::apply(inverse(p), xyzOf(c.wx, c.wy));
    return {p[0] * s[0], p[1] * s[1], p[2] * s[2], p[3] * s[0], p[4] * s[1], p[5] * s[2], p[6] * s[0], p[7] * s[1], p[8] * s[2]};
}

/// Bradford chromatic adaptation from white `from` to white `to` (XYZ -> XYZ).
Matrix3 bradford(double fx, double fy, double tx, double ty) {
    constexpr Matrix3 m{0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296};
    const Rgb3 s = color::apply(m, xyzOf(fx, fy));
    const Rgb3 d = color::apply(m, xyzOf(tx, ty));
    const Matrix3 scale{d[0] / s[0], 0, 0, 0, d[1] / s[1], 0, 0, 0, d[2] / s[2]};
    return mul(inverse(m), mul(scale, m));
}

/// Linear RGB in `from` -> linear RGB in `to` (Bradford-adapted between different whites).
Matrix3 convert(const Chromaticities& from, const Chromaticities& to) {
    Matrix3 m = rgbToXyz(from);
    if (from.wx != to.wx || from.wy != to.wy) {
        m = mul(bradford(from.wx, from.wy, to.wx, to.wy), m);
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

// ---- OpenColorIO ---------------------------------------------------------------------------------------------------

constexpr const char* kAp0ToXyz = "UTILITY - ACES-AP0_to_CIE-XYZ-D65_BFD";
constexpr const char* kOdtSdr = "ACES-OUTPUT - ACES2065-1_to_CIE-XYZ-D65 - SDR-VIDEO_1.0";
constexpr const char* kOdtHdr1000 = "ACES-OUTPUT - ACES2065-1_to_CIE-XYZ-D65 - HDR-VIDEO-1000nit-15nit-REC2020lim_1.1";
constexpr const char* kDisplay709 = "DISPLAY - CIE-XYZ-D65_to_REC.1886-REC.709";
constexpr const char* kDisplayPq = "DISPLAY - CIE-XYZ-D65_to_REC.2100-PQ";
constexpr const char* kDisplayHlg = "DISPLAY - CIE-XYZ-D65_to_REC.2100-HLG-1000nit";

/// The OCIO built-in transform taking a camera format to ACES2065-1, or null.
const char* cameraToAces(InputColorSpace space) {
    switch (space) {
    case InputColorSpace::SLog3SGamut3: return "SONY_SLOG3-SGAMUT3_to_ACES2065-1";
    case InputColorSpace::SLog3SGamut3Cine: return "SONY_SLOG3-SGAMUT3.CINE_to_ACES2065-1";
    case InputColorSpace::VLogVGamut: return "PANASONIC_VLOG-VGAMUT_to_ACES2065-1";
    case InputColorSpace::LogC3: return "ARRI_ALEXA-LOGC-EI800-AWG_to_ACES2065-1";
    case InputColorSpace::LogC4: return "ARRI_LOGC4_to_ACES2065-1";
    case InputColorSpace::CanonLog3: return "CANON_CLOG3-CGAMUT_to_ACES2065-1";
    case InputColorSpace::AppleLog: return "APPLE_LOG_to_ACES2065-1";
    case InputColorSpace::CanonLog2: return "CANON_CLOG2-CGAMUT_to_ACES2065-1";
    case InputColorSpace::RedLog3G10: return "RED_LOG3G10-RWG_to_ACES2065-1";
    case InputColorSpace::AcesCct: return "ACEScct_to_ACES2065-1";
    default: return nullptr;
    }
}

bool isLog(InputColorSpace space) { return cameraToAces(space) != nullptr; }

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

/// The ACES output chain of an ACES project to `target`: ACEScg -> AP0 -> output transform -> display encoding.
void acesOutput(OCIO::GroupTransform& g, OutputTarget target) {
    g.appendTransform(matrixTransform(workingToAp0(Working::AcesCg)));
    g.appendTransform(builtin(target == OutputTarget::Sdr ? kOdtSdr : kOdtHdr1000));
    g.appendTransform(builtin(target == OutputTarget::Sdr ? kDisplay709 : target == OutputTarget::Pq ? kDisplayPq : kDisplayHlg));
}

/// The part of it baked into the output LUT: ACEScg -> AP0 -> output transform -> display light in the display's
/// primaries (Rec.709 for SDR, Rec.2020 for HDR), unclamped; the display encoding runs after the LUT (finish).
void acesOutputLight(OCIO::GroupTransform& g, OutputTarget target) {
    g.appendTransform(matrixTransform(workingToAp0(Working::AcesCg)));
    g.appendTransform(builtin(target == OutputTarget::Sdr ? kOdtSdr : kOdtHdr1000));
    g.appendTransform(matrixTransform(inverse(rgbToXyz(target == OutputTarget::Sdr ? kRec709 : kRec2020))));
}

std::string targetName(OutputTarget t) {
    return t == OutputTarget::Sdr ? "sdr" : t == OutputTarget::Pq ? "pq" : "hlg";
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
        g.appendTransform(builtin(cameraToAces(space)));
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
    case ColorManagementMode::WideGamut: return Working::AcesCg;
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

Rgb3 toneMapToSdr(const Rgb3& c, double referenceWhiteNits, double peakNits) {
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

std::expected<InputPlan, Error> planInput(const ColorManagement& m, InputColorSpace space, bool limitedRange, bool graphics) {
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
        plan.scale = 1.0 / (m.isHdr() ? m.referenceWhiteNits : 100.0);
        break;
    case InputColorSpace::Rec2100Hlg:
        plan.transfer = TransferFunction::HLG;
        primaries = &kRec2020;
        plan.hlgPeakNits = m.peakNits;
        plan.scale = 1.0 / (m.isHdr() ? m.referenceWhiteNits : 100.0);
        break;
    default: break; // Rec.709
    }
    plan.matrix = convert(*primaries, chromaticitiesOf(w));
    return plan;
}

std::expected<OutputPlan, Error> planOutput(const ColorManagement& m, OutputTarget target) {
    OutputPlan plan;
    plan.referenceWhiteNits = m.referenceWhiteNits;
    plan.peakNits = m.peakNits;
    const Working w = workingOf(m.mode);
    if (m.mode == ColorManagementMode::Aces13) {
        const std::string key = "out:aces-light:" + targetName(target);
        auto chain = chainFor(key, [&](OCIO::GroupTransform& g) { acesOutputLight(g, target); });
        if (!chain) {
            return std::unexpected(chain.error());
        }
        plan.kind = OutputPlan::Kind::Lut;
        plan.lut = bakeChain(key, BakedLut::Shaper::Log2, BakedLut::Values::AsIs, **chain); // light: linear
        plan.finish = target == OutputTarget::Sdr  ? OutputPlan::Finish::Bt1886
                      : target == OutputTarget::Pq ? OutputPlan::Finish::Pq
                                                   : OutputPlan::Finish::Hlg;
        plan.displayNits = 100.0;
        plan.peakNits = 1000.0; // the ACES HDR output transform's display
        plan.label = target == OutputTarget::Sdr ? "ACES 1.0 SDR Video" : "ACES 1.1 HDR Video (1000 nits)";
        return plan;
    }
    if (target == OutputTarget::Sdr) {
        if (m.mode == ColorManagementMode::WideGamut) {
            // Wide Gamut (No Tone Mapping): ACEScg -> Rec.709, BT.709 encoded, clipped above white.
            plan.kind = OutputPlan::Kind::Rec709;
            plan.matrix = convert(kAp1, kRec709);
            plan.label = "Wide Gamut · no tone mapping";
            return plan;
        }
        if (m.isHdr()) {
            // An HDR project on an SDR display or file: the tone-mapped preview (and SDR export), analytic.
            plan.kind = OutputPlan::Kind::ToneMapSdr;
            plan.matrix = convert(kRec2020, kRec709);
            plan.label = "HDR · tone-mapped SDR preview";
            return plan;
        }
        plan.kind = OutputPlan::Kind::Rec709; // today's output, byte for byte
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
        return inputPost(m, space, run((*chain)->cpu, code));
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
        return plan.lut ? plan.lut->sample(c) : Rgb3{};
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
    return {w[0] * plan.scale, w[1] * plan.scale, w[2] * plan.scale};
}

Rgb3 runOutputPlan(const OutputPlan& plan, const Rgb3& working) {
    switch (plan.kind) {
    case OutputPlan::Kind::Lut: {
        const Rgb3 x = color::apply(plan.matrix, working);
        const Rgb3 light = plan.lut ? plan.lut->sample({shaperOf(x[0]), shaperOf(x[1]), shaperOf(x[2])}) : Rgb3{};
        switch (plan.finish) {
        case OutputPlan::Finish::Bt1886:
            return {std::pow(std::clamp(light[0], 0.0, 1.0), 1.0 / 2.4), std::pow(std::clamp(light[1], 0.0, 1.0), 1.0 / 2.4),
                    std::pow(std::clamp(light[2], 0.0, 1.0), 1.0 / 2.4)};
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
        const Rgb3 sdr = toneMapToSdr({std::max(working[0], 0.0), std::max(working[1], 0.0), std::max(working[2], 0.0)},
                                      plan.referenceWhiteNits, plan.peakNits);
        return {linearToRec709(sdr[0]), linearToRec709(sdr[1]), linearToRec709(sdr[2])};
    }
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
    return {linearToRec709(v[0]), linearToRec709(v[1]), linearToRec709(v[2])};
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
    std::vector<std::string> out{kAp0ToXyz, kOdtSdr, kOdtHdr1000, kDisplay709, kDisplayPq, kDisplayHlg};
    for (const InputColorSpace s : allInputColorSpaces()) {
        if (const char* camera = cameraToAces(s)) {
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

} // namespace wabcut::color
