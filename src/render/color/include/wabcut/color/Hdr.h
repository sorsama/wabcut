#pragma once

// HDR game captures in SDR projects (I.9, gaming; docs/color.md "HDR game captures"): OBS, Xbox Game Bar and ShadowPlay
// HDR recordings are BT.2020 files with the SMPTE ST 2084 (PQ) or ARIB STD-B67 (HLG) transfer. Wabcut projects are SDR
// Rec.709, so such a clip is tone-mapped into SDR as it is read: BT.2100 decoding, an ITU-R BT.2390 EETF on the
// luminance from a 1000-nit master into the SDR range (the 203-nit reference white of ITU-R BT.2408 standing for SDR
// white), the colour scaled along, Rec.2020 -> Rec.709 with out-of-gamut values clipped. The layer shader runs the same
// formulas (layer.frag captureToSdr); these are the CPU references. No colour management, no OpenColorIO.

#include <wabcut/model/MediaInfo.h>

#include <array>
#include <optional>

namespace wabcut::color {

using Rgb3 = std::array<double, 3>;
/// Row-major 3x3 matrix.
using Matrix3 = std::array<double, 9>;
Rgb3 apply(const Matrix3& m, const Rgb3& v);

/// SMPTE ST 2084: code value -> absolute luminance (nits), and back.
double pqToNits(double code);
double nitsToPq(double nits);
/// ITU-R BT.2100 HLG OETF (scene linear 0..1 -> signal) and its inverse.
double hlgOetf(double scene);
double hlgInverseOetf(double signal);
/// The HLG system gamma for a display of `peakNits` (BT.2100 note 5f): 1.2 + 0.42 log10(peak / 1000).
double hlgSystemGamma(double peakNits);
/// BT.2100 HLG as a display of `peakNits`: signal R'G'B' -> display light in nits (Rec.2020).
Rgb3 hlgToDisplayNits(const Rgb3& signal, double peakNits);

/// Linear Rec.2020 (1.0 = the reference white) -> linear Rec.709 in [0, 1]: ITU-R BT.2390 EETF on the luminance (PQ
/// domain) from the master's peak into [0, reference white] (mid-tones pass below the knee, the peak lands on SDR
/// white), the colour scaled along, Rec.2020 -> Rec.709 with out-of-gamut values clipped.
Rgb3 toneMapToSdr(const Rgb3& rec2020, double referenceWhiteNits, double peakNits);

/// How captures are read: a 1000-nit master (HLG: a 1000-nit display) whose 203-nit reference white is SDR white.
inline constexpr double kCaptureReferenceWhiteNits = 203.0;
inline constexpr double kCapturePeakNits = 1000.0;
/// What a PQ / HLG capture's R'G'B' code values become in an SDR project: linear Rec.709, 0..1. Other transfers: nullopt.
std::optional<Rgb3> hdrCaptureToSdr(TransferFunction transfer, const Rgb3& code);
/// Linear Rec.2020 -> linear Rec.709 (D65 both).
Matrix3 rec2020ToRec709();

} // namespace wabcut::color
