#pragma once

// Colour management (I.9, docs/color.md "Colour management"): the transforms between a clip's code values, the
// project's working space and the outputs (program monitor, export).
//
//   working spaces: linear Rec.709 (display-referred Rec.709 projects: today's pipeline), ACEScg (ACES 1.3 projects),
//                   linear Rec.2020 with 1.0 = the reference white (HDR Rec.2100 projects).
//   inputs:  BT.709 / sRGB / BT.2020 / PQ / HLG / linear are short analytic curves and a primaries matrix, run by the
//            layer shader; camera log formats (S-Log3, V-Log, LogC3/4, Canon Log 3, Apple Log), ACES renders and, in an
//            ACES project, display-referred pictures (Rec.709 video, titles, captions: the inverse of the output
//            transform, so they look as they did) are OpenColorIO 2 built-in transforms (no config file).
//   outputs: Rec.709 / sRGB as before; PQ and HLG analytic (SMPTE ST 2084, ITU-R BT.2100); the ACES output transforms
//            (SDR video, HDR 1000 nits) through OpenColorIO up to display light, then the display's clamp and
//            encoding (BT.1886, PQ, HLG) exactly; an HDR project shown on an SDR display through a tone-mapped
//            preview (ITU-R BT.2390 EETF into the SDR range, then Rec.709; analytic).
//
// The GPU never runs OpenColorIO's own shaders (the compositor's shaders are precompiled for every backend): a transform
// that is not analytic is baked into a 3D LUT on a lattice of OCIO's exact CPU processor - indexed by code values for an
// input (its texels hold asinh-encoded working values: linear through 0, logarithmic in the highlights), by a log2
// shaper of the working value for an output - and sampled with tetrahedral interpolation (exact on the grey axis). The CPU
// functions here (applyInput / applyOutput) are the references the GPU is tested against: OCIO's CPU processor itself
// for OCIO transforms, the same formulas for the analytic ones. Thread-safe; baked LUTs are cached.

#include <wabcut/model/ColorManagement.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/MediaInfo.h>

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wabcut::color {

using Rgb3 = std::array<double, 3>;

enum class Working : std::uint8_t { LinearRec709, AcesCg, LinearRec2020 };

/// The working space of a project's colour management.
Working workingOf(ColorManagementMode mode);

/// An output's target.
enum class OutputTarget : std::uint8_t {
    Sdr,  ///< an SDR Rec.709 display or file (the monitor; SDR exports)
    Pq,   ///< BT.2100 PQ, Rec.2020 (HDR exports, an HDR monitor)
    Hlg,  ///< BT.2100 HLG, Rec.2020
};

/// Row-major 3x3 matrix.
using Matrix3 = std::array<double, 9>;
Rgb3 apply(const Matrix3& m, const Rgb3& v);

/// A transform baked into a lattice: size^3 RGBA float texels, red fastest, then green, then blue.
struct BakedLut {
    int size = 0;
    std::vector<float> rgba;
    /// How a value becomes a lattice coordinate in [0, 1]: code values as they are (inputs), or the log2 shaper
    /// (outputs: shaperOf(linear)).
    enum class Shaper : std::uint8_t { CodeValues, Log2 } shaper = Shaper::CodeValues;
    /// What the texels hold: the values themselves (outputs: display light), or asinh of them (inputs: decode with
    /// sinh after interpolating - linear around 0, so saturated colours crossing 0 interpolate well, logarithmic for
    /// the steep highlights of log curves and inverse output transforms).
    enum class Values : std::uint8_t { AsIs, Asinh } values = Values::AsIs;
    std::string key; ///< what it is (cache identity, diagnostics)

    /// The value at lattice coordinate `uvw` (each in [0, 1]), tetrahedral, decoded per `values`: what the shader computes.
    Rgb3 sample(const Rgb3& uvw) const;
};

/// The ACEScct curve (S-2016-001; its linear toe continues below 0).
double acesCctFromLinear(double linear);
double linearFromAcesCct(double cct);
/// The output LUTs' shaper: (log2(clamp(v, 0, kShaperMaxLinear) + 2^-12) + 12) / 19 - 19 stops over the lattice,
/// smooth down to 0 (black keeps its resolution), and its inverse.
double shaperOf(double linear);
double linearFromShaper(double s);
/// The top of the output shaper: 128 (7 stops over 1.0; every output transform here is white well below it).
inline constexpr double kShaperMaxLinear = 128.0;

// ---- Analytic curves (the shaders run the same) ----

/// SMPTE ST 2084: code value -> absolute luminance (nits), and back.
double pqToNits(double code);
double nitsToPq(double nits);
/// ITU-R BT.2100 HLG OETF (scene linear 0..1 -> signal) and its inverse.
double hlgOetf(double scene);
double hlgInverseOetf(double signal);
/// BT.2100 HLG as a display of `peakNits` (system gamma 1.2 at 1000 nits, adjusted for other peaks): signal R'G'B'
/// -> display light in nits, and back (Rec.2020).
Rgb3 hlgToDisplayNits(const Rgb3& signal, double peakNits);
Rgb3 displayNitsToHlg(const Rgb3& nits, double peakNits);
/// The HLG system gamma for a display of `peakNits` (BT.2100 note 5f): 1.2 + 0.42 log10(peak / 1000).
double hlgSystemGamma(double peakNits);

/// The SDR preview of an HDR working-space colour (linear Rec.2020, 1.0 = reference white): ITU-R BT.2390 EETF on
/// the luminance, from the mastering peak into the SDR range (mid-tones pass below the knee, the peak lands on SDR
/// white), the colour scaled along, then Rec.2020 -> Rec.709 with out-of-gamut values clipped. Returns linear Rec.709,
/// 0..1.
Rgb3 toneMapToSdr(const Rgb3& working2020, double referenceWhiteNits, double peakNits);

/// HDR game captures in an SDR (display-referred) project (I.9, gaming: OBS, Game Bar and ShadowPlay HDR recordings):
/// a PQ- or HLG-tagged clip is tone-mapped into SDR with toneMapToSdr, read as a 1000-nit master whose 203-nit
/// reference white (ITU-R BT.2408) stands for SDR white; HLG as a 1000-nit display.
inline constexpr double kCaptureReferenceWhiteNits = 203.0;
inline constexpr double kCapturePeakNits = 1000.0;
/// What a PQ / HLG capture's R'G'B' code values become in an SDR project: linear Rec.709, 0..1 - the CPU reference of
/// the layer pass's tone-mapped HDR inputs. Other transfers: nullopt.
std::optional<Rgb3> hdrCaptureToSdr(TransferFunction transfer, const Rgb3& code);
/// Linear Rec.2020 -> linear Rec.709 (D65 both).
Matrix3 rec2020ToRec709();

// ---- Plans: what the compositor runs ----

/// How a layer's decoded code values (R'G'B' in [0, 1], range already expanded) reach the working space.
struct InputPlan {
    enum class Kind : std::uint8_t {
        Analytic,   ///< transfer curve per channel, then `matrix` (primaries -> working), times `scale`
        Lut,        ///< `lut` (indexed by code values)
        ToneMapSdr, ///< Direct Rec.709: a PQ / HLG (`transfer`) picture tone-mapped into SDR (hdrCaptureToSdr)
        LutToneMapSdr, ///< Direct Rec.709: camera log - `lut` (scene-linear Rec.2020, 1.0 = diffuse white), then
                       ///< tone-mapped into SDR as an HDR capture is (toneMapToSdr at the kCapture* white and peak)
    } kind = Kind::Analytic;
    TransferFunction transfer = TransferFunction::BT709; ///< Analytic: the curve (PQ: nits / reference white; HLG:
                                                          ///< a peakNits display / reference white)
    Matrix3 matrix{1, 0, 0, 0, 1, 0, 0, 0, 1};           ///< Analytic: linear source primaries -> working
    double scale = 1.0;                                  ///< Analytic, after the matrix (PQ, HLG: 1 / reference white)
    double hlgPeakNits = 1000.0;                         ///< Analytic HLG: the display the signal is read as
    std::shared_ptr<const BakedLut> lut;
};

/// How the working space reaches an output.
struct OutputPlan {
    enum class Kind : std::uint8_t {
        Rec709, ///< BT.709 OETF of `matrix` * working, clamped (the default pipeline's output)
        Srgb,   ///< sRGB encoding
        Pq,     ///< `matrix` * working * referenceWhite nits -> ST 2084
        Hlg,    ///< `matrix` * working * referenceWhite nits -> BT.2100 HLG (a peakNits display)
        Lut,    ///< `lut` indexed by the log2 shaper of `matrix` * working (identity: the working space); its
                ///< texels are linear display light in the display's primaries (1.0 = `displayNits`), then `finish`
        ToneMapSdr, ///< toneMapToSdr(max(working, 0)) (`matrix`: Rec.2020 -> Rec.709), BT.709 encoded
    } kind = Kind::Rec709;
    /// Lut: how display light becomes code values - as it is, BT.1886 (clamped to [0, 1], 1 / 2.4), PQ or HLG (a
    /// `peakNits` display) of display light x `displayNits`.
    enum class Finish : std::uint8_t { AsIs, Bt1886, Pq, Hlg } finish = Finish::AsIs;
    double displayNits = 100.0; ///< Lut: nits of display light 1.0 (OpenColorIO's convention)
    Matrix3 matrix{1, 0, 0, 0, 1, 0, 0, 0, 1};
    double referenceWhiteNits = 203.0;
    double peakNits = 1000.0;
    std::shared_ptr<const BakedLut> lut;
    /// What the output shows, for a monitor's label: "", or "HDR · tone-mapped SDR preview", "ACES 1.0 SDR Video".
    std::string label;
};

/// The input colour space a stream's tags imply (Auto): PQ / HLG transfer -> Rec.2100; BT.2020 primaries -> Rec.2020;
/// sRGB, linear; otherwise Rec.709. `isRgb`: an RGB picture (stills, graphics) with no transfer tag is sRGB.
InputColorSpace inputSpaceOf(const ColorInfo& color, bool isRgb);

/// The plan for a layer of `space` (its code values `limitedRange` when the file's levels were video range: camera
/// log curves are defined on full-range code values). `graphics`: a display-referred picture (a title, a caption, a
/// still graphic), which in an ACES project goes through the inverse output transform so it looks as it did.
/// Direct Rec.709 (SDR) tone-maps on input: PQ / HLG pictures as HDR captures (Kind::ToneMapSdr), camera log the same
/// way from its scene light (Kind::LutToneMapSdr: diffuse white at the 203-nit reference white of a 1000-nit master,
/// luminance tone-mapped, hue kept); other spaces are analytic into linear Rec.709. Disable Color Management has no plans (the compositor reads code values as they
/// are). Errors: Unsupported (OpenColorIO cannot build the transform).
std::expected<InputPlan, Error> planInput(const ColorManagement& management, InputColorSpace space, bool limitedRange,
                                          bool graphics);
/// The plan for an output of `target`.
std::expected<OutputPlan, Error> planOutput(const ColorManagement& management, OutputTarget target);

/// CPU references, double precision, exact (OCIO's CPU processor for OCIO transforms).
std::expected<Rgb3, Error> applyInput(const ColorManagement& management, InputColorSpace space, bool limitedRange,
                                      bool graphics, const Rgb3& code);
std::expected<Rgb3, Error> applyOutput(const ColorManagement& management, OutputTarget target, const Rgb3& working);
/// What a plan does to one value, as the GPU computes it (the LUT's trilinear interpolation included).
Rgb3 runInputPlan(const InputPlan& plan, const Rgb3& code);
Rgb3 runOutputPlan(const OutputPlan& plan, const Rgb3& working);

/// Lattice size of baked LUTs (65: the transforms here are smooth in code values and in ACEScct).
inline constexpr int kLutSize = 65;

/// The OpenColorIO built-in transform names this uses (tests check they exist in the linked library), and its version.
std::vector<std::string> builtinTransformsUsed();
std::string ocioVersion();
/// Applies one OpenColorIO built-in transform (forward or inverse) to `rgb` on the CPU (tests, references).
std::expected<Rgb3, Error> applyBuiltin(const std::string& style, const Rgb3& rgb, bool inverse = false);

} // namespace wabcut::color
