#pragma once

// The colour grade ("wabcut.color.grade", a full grading stack in one effect) - its parameters, its curve baking and
// the CPU reference of exactly what the compositor's grade passes do (src/render/shaders/grade.frag). docs/color.md
// is the user-facing description; this header is the contract the shader, the tests and the colour tools share.
//
// Stages, in this fixed order (each has an enable switch; a stage at its neutral values is skipped, so an untouched
// grade costs nothing and changes nothing). "Display" means BT.709-encoded values (encode709), "linear" the
// compositor's scene-linear Rec.709 working space:
//
//   1  Basic: input LUT                 display   a .cube (log -> Rec.709 and the like), full strength
//   2  Basic: white balance             linear    von Kries gains in Bradford cone space (whiteBalanceMatrix)
//   3  Basic: exposure                  linear    x 2^stops
//   4  Basic: contrast                  linear    a power about 18 % grey, per channel: 0.18 (x / 0.18)^(1 + c/100)
//   5  Basic: highlights, shadows,      display   one monotone curve of the encoded luma through (0, 0.12 b),
//      whites, blacks                             (0.25, 0.25 + 0.12 s), (0.5, 0.5), (0.75, 0.75 + 0.12 h),
//                                                 (1, 1 + 0.12 w), added to R, G and B alike (smooth tonal masks:
//                                                 no clipping, no hue shift); straight lines of the end slopes outside
//   6  Basic: saturation, vibrance      linear    luminance-preserving; vibrance weighs each pixel by (1 - s)^2 of its
//                                                 display saturation and protects skin hues (about 20-50 degrees)
//   7  Creative: look LUT (intensity), display   intensity mixes the look in (above 100 % it extrapolates);
//      faded film, shadow/highlight             faded film lifts the blacks: x + 0.2 f (1 - x)^2;
//      tint (balance), vibrance,                tints add a hue's chroma weighted by (1 - u)^2 / u^2 of the luma u
//      saturation                               (shifted by the balance); vibrance and saturation as in 6, on display
//   8  Curves: RGB (master, then R/G/B),display   monotone cubics (Fritsch-Carlson) baked into 1024-entry tables when
//      hue vs sat, hue vs hue, hue vs           the grade changes; hue curves are periodic shape-preserving cubics
//      luma, luma vs sat, sat vs sat            (0.5 = no change: sat x 2y, hue + (y - 0.5) turns, luma + (y - 0.5) s/2)
//   9  Wheels: lift/gamma/gain/offset   display   ASC CDL v1.2: slope = gain - lift, offset = lift + offset,
//      + CDL saturation                         power = gamma; clamp to [0, 1] ("ASC" style), then the CDL
//                                                 saturation on Rec.709 luma, clamped again; skipped when identity
//  10  HSL secondary                    display   a matte from hue (centre, width, soft), saturation and luma ranges
//                                                 (inverted on request), refined by a denoise (blur + soft threshold)
//                                                 and a blur, then temperature/tint, contrast (pivot 0.5), sharpen,
//                                                 saturation and an offset wheel mixed in by the matte
//  11  Vignette                         linear    wabcut.vignette's formula: x 2^(amount v)
//
// Scalars are keyframable effect parameters (EffectRegistry: "wabcut.color.grade"). Curves and LUT references are the
// instance's EffectData (not keyframable): point lists under the curve keys, and text "asset:<id>" (a ProjectAsset)
// or "builtin:<name>" (Looks.h) under the LUT keys.

#include <wabcut/color/ColorMath.h>
#include <wabcut/color/Lut.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/Time.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::color {

inline constexpr std::string_view kGradeEffect = "wabcut.color.grade";

/// EffectData keys of the grade.
namespace gradekey {
inline constexpr std::string_view inputLut = "basic.inputLut";
inline constexpr std::string_view look = "creative.look";
inline constexpr std::string_view master = "curves.master";
inline constexpr std::string_view red = "curves.red";
inline constexpr std::string_view green = "curves.green";
inline constexpr std::string_view blue = "curves.blue";
inline constexpr std::string_view hueSat = "curves.hueSat";
inline constexpr std::string_view hueHue = "curves.hueHue";
inline constexpr std::string_view hueLuma = "curves.hueLuma";
inline constexpr std::string_view lumaSat = "curves.lumaSat";
inline constexpr std::string_view satSat = "curves.satSat";
} // namespace gradekey

/// The grade's scalar parameters at one time (their registry keys in comments).
struct GradeParams {
    // Stage switches ("<stage>.enabled", 0/1).
    bool basic = true;
    bool creative = true;
    bool curves = true;
    bool wheels = true;
    bool hsl = true;
    bool vignette = true;
    // Basic ("basic.*").
    double temperature = 0, tint = 0;                        // -100..100
    double exposure = 0;                                     // stops, -5..5
    double contrast = 0;                                     // -100..100
    double highlights = 0, shadows = 0, whites = 0, blacks = 0; // -100..100
    double saturation = 100;                                 // 0..200
    double vibrance = 0;                                     // -100..100
    // Creative ("creative.*").
    double lookIntensity = 100;  // 0..200
    double fadedFilm = 0;        // 0..100
    double creativeVibrance = 0; // -100..100
    double creativeSaturation = 100;
    double shadowTintHue = 0, shadowTintAmount = 0;       // degrees, 0..100
    double highlightTintHue = 0, highlightTintAmount = 0; // degrees, 0..100
    double tintBalance = 0;                               // -100..100
    // Wheels ("wheels.<lift|gamma|gain|offset>.<red|green|blue>", "wheels.saturation"): ASC CDL parts.
    Rgb lift{0, 0, 0};
    Rgb gamma{1, 1, 1}; ///< CDL power
    Rgb gain{1, 1, 1};
    Rgb offset{0, 0, 0};
    double cdlSaturation = 1;
    // HSL secondary ("hsl.*").
    double hueCenter = 0, hueWidth = 360, hueSoftness = 0;  // degrees
    double satLow = 0, satHigh = 100, satSoftness = 0;      // percent
    double lumaLow = 0, lumaHigh = 100, lumaSoftness = 0;   // percent
    bool invert = false;
    double denoise = 0; // 0..100
    double blur = 0;    // sequence px, 0..100
    double hslTemperature = 0, hslTint = 0, hslContrast = 0; // -100..100
    double hslSharpen = 0;                                  // 0..100
    double hslSaturation = 100;                             // 0..200
    Rgb hslOffset{0, 0, 0};                                 // -1..1
    // Vignette ("vignette.*", wabcut.vignette's units).
    double vignetteAmount = 0, vignetteMidpoint = 50, vignetteRoundness = 0, vignetteFeather = 50;

    bool operator==(const GradeParams&) const = default;
};

/// Control points of the grade's curves (EffectData). Empty = the identity / neutral curve.
struct GradeCurves {
    std::vector<EffectPoint> master, red, green, blue;     // x, y in [0, 1]
    std::vector<EffectPoint> hueSat, hueHue, hueLuma;       // x = hue in turns [0, 1), y in [0, 1], 0.5 = no change
    std::vector<EffectPoint> lumaSat, satSat;               // x in [0, 1], y in [0, 1], 0.5 = no change

    bool operator==(const GradeCurves&) const = default;
    bool rgbIdentity() const;
    bool hueNeutral() const;
    bool satNeutral() const;
};

/// A monotone cubic (Fritsch-Carlson / PCHIP) through `points` (sorted by x; duplicates keep the last), flat at the
/// end points' y inside [0, 1] outside them, and a slope-1 line beyond [0, 1]. No points = the identity.
double evaluateCurve(const std::vector<EffectPoint>& points, double x);
/// The periodic shape-preserving cubic of a hue curve at `turns` (wraps). No points = 0.5 (neutral).
double evaluateHueCurve(const std::vector<EffectPoint>& points, double turns);
/// A shape-preserving cubic over [0, 1] (luma vs sat, sat vs sat): flat outside the points. No points = 0.5.
double evaluateNeutralCurve(const std::vector<EffectPoint>& points, double x);

/// The curves baked into tables the GPU samples (rows of a texture; the CPU reference uses the same tables).
struct BakedCurves {
    static constexpr int kSize = 1024;
    /// RGB curves at x = i / (kSize - 1): R, G, B, master per entry.
    std::vector<float> rgbm;
    /// Hue curves at h = i / kSize (periodic): hue vs sat, hue vs hue, hue vs luma, 0.
    std::vector<float> hue;
    /// At x = i / (kSize - 1): luma vs sat, sat vs sat, 0, 0.
    std::vector<float> sat;
    bool rgbIdentity = true;
    bool hueNeutral = true;
    bool satNeutral = true;
    /// Content hash (cache key for textures).
    std::uint64_t key = 0;
};

std::shared_ptr<const BakedCurves> bakeCurves(const GradeCurves& curves);

/// Linear interpolation in a baked table row (`stride` floats per entry, component `c`) at `x` in [0, 1] (clamped)
/// with entries at i / (n - 1); `periodic` rows have entries at i / n and wrap.
double sampleRow(const std::vector<float>& row, int stride, int c, double x, bool periodic);
/// A baked RGB curve with the slope-1 extension beyond [0, 1] (component 0..3 = R, G, B, master).
double sampleRgbCurve(const BakedCurves& baked, int c, double x);

/// Everything the grade needs at one frame: the parameters, the baked curves and the LUTs. Shared and immutable.
struct GradeSpec {
    GradeParams params;
    std::shared_ptr<const BakedCurves> curves; ///< null = identity curves
    std::shared_ptr<const Lut> inputLut;       ///< null = none (or not found: `missingLuts` says so)
    std::shared_ptr<const Lut> look;
    int missingLuts = 0;                       ///< LUT references that did not resolve (the UI warns)
    LutInterpolation interpolation = LutInterpolation::Tetrahedral;
    /// The encoding of the display-referred stages (I.9): ACEScct in colour-managed projects (the compositor uses it
    /// whenever its ColorPipeline is managed, whatever this says; the CPU reference follows this field).
    GradeEncoding encoding = GradeEncoding::Rec709;

    /// Which stages do something (enabled and not neutral).
    bool inputLutActive() const;
    bool basicActive() const;
    bool creativeActive() const;
    bool curvesActive() const;
    bool wheelsActive() const;
    bool hslActive() const;
    bool vignetteActive() const;
    bool active() const;
    /// Whether the HSL matte needs a neighbourhood pass (denoise, blur or sharpen).
    bool hslNeedsRefine() const;
};

// ---- Reading the effect ----

/// The parameters of a grade instance at a curve time (descriptor defaults for parameters it does not list).
GradeParams gradeParamsAt(const EffectInstance& instance, Ticks curveTime);
GradeCurves gradeCurvesOf(const EffectInstance& instance);
/// The scalar parameters as effect params (only those that differ from the defaults when `onlyChanged`).
std::vector<EffectParam> gradeParamsToEffect(const GradeParams& params, bool onlyChanged);

/// Finds a LUT by its EffectData reference ("asset:<id>", "builtin:<name>"); null when it cannot.
using LutResolver = std::function<std::shared_ptr<const Lut>(std::string_view reference)>;

/// The grade of an instance at a curve time, with baked curves (cached by content, so evaluating every frame is cheap)
/// and resolved LUTs.
GradeSpec evaluateGrade(const EffectInstance& instance, Ticks curveTime, const LutResolver& luts);

// ---- CPU reference (double precision) of the shader ----

/// Tone curve of stage 5 (encoded luma in, encoded luma out).
double toneCurve(const GradeParams& p, double x);
/// Its spline for the shader: y at x = 0, 0.25, 0.5, 0.75, 1, then the five tangents.
std::array<double, 10> toneSplineCoefficients(const GradeParams& p);

/// Stages 1-9 on one straight (not premultiplied) linear Rec.709 value. Returns display-referred (encoded) RGB, the
/// input of the HSL secondary, so the secondary's matte can be computed on it.
Rgb gradeMain(const GradeSpec& spec, const Rgb& linear);
/// The HSL qualifier's raw matte (before the refine) of a display-referred value.
double qualifierMatte(const GradeParams& p, const Rgb& display);
/// The denoise threshold the refine applies after its first blur.
double denoiseThreshold(double matte);
/// Stage 10's correction mixed in by `matte`; `blurred` is the display value blurred for the sharpen (unused without).
Rgb applySecondary(const GradeParams& p, const Rgb& display, double matte, const Rgb& blurred,
                   GradeEncoding encoding = GradeEncoding::Rec709);
/// Stage 11 on linear RGB at normalised frame position q in [-1, 1]^2 (frame aspect `aspect`).
Rgb applyVignette(const GradeParams& p, const Rgb& linear, double qx, double qy, double aspect);

/// A whole pixel without neighbourhood effects (no denoise, blur or sharpen): stages 1-11. `qx`, `qy` as above.
Rgb gradePixel(const GradeSpec& spec, const Rgb& linear, double qx = 0.0, double qy = 0.0, double aspect = 16.0 / 9.0);

// ---- Export .cube (0.2) ----

/// Lumetri's Export .cube: the grade as a `size`^3 3D LUT (2..kMaxLut3dSize, Premiere writes 33) from display-referred
/// Rec.709 code values to code values, the way Wabcut and other applications apply a look. Every pixel-local stage goes
/// in (input LUT, Basic, Creative, Curves, Color Wheels, the HSL secondary's correction through its qualifier); the
/// vignette (it depends on where a pixel is) and the secondary's denoise / blur / sharpen (they read neighbours) cannot,
/// and are left out. Outputs are clamped to [0, 1]. `title` is cleaned for a .cube TITLE line.
Lut gradeToLut(const GradeSpec& spec, int size = 33, std::string title = {});
/// What gradeToLut leaves out of this grade ("Vignette", "masks", ...), for the export's report.
std::vector<std::string> gradeLutOmissions(const GradeSpec& spec, const EffectInstance& instance);

/// The sigma (sequence px) of the matte refine's denoise and blur, and the sharpen's blur.
double denoiseSigma(const GradeParams& p);
double refineBlurSigma(const GradeParams& p);
inline constexpr double kSecondarySharpenSigma = 1.5;

} // namespace wabcut::color
