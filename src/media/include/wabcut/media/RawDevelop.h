#pragma once

// Camera RAW (0.2): developing a colour filter array (Bayer mosaic) into a picture Wabcut composites. FFmpeg decodes
// ProRes RAW and CinemaDNG (its TIFF decoder) into Bayer samples; VideoDecoder develops them here into 10-bit Rec.709
// Y'CbCr 4:4:4, the pipeline's own format:
//
//   samples -> (s - black) / (white - black) -> white balance per CFA site -> bilinear demosaic -> camera RGB to CIE XYZ
//   (D65) when the codec gives the matrix -> RAW Source Settings' white balance (a Bradford adaptation) -> XYZ to
//   linear Rec.709 -> gain -> exposure -> BT.709 OETF -> Y'CbCr.
//
// ProRes RAW carries the black and white levels, white balance, matrix and gain on every frame (FFmpeg's
// AVRawColorParams). CinemaDNG frames arrive from FFmpeg already linearised, black-subtracted and white balanced, in
// camera RGB (no matrix applied): they are read as linear Rec.709. Highlights above the white point clip.
//
// RAW Source Settings (model/RawSourceSettings.h) set the exposure and, instead of the camera's white balance, the
// colour temperature and tint of the light to balance for: the picture is adapted from the as-shot light (ProRes RAW
// names its temperature; otherwise it counts as 6500 K) to that light.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace wabcut::media {

/// The 2 x 2 colour filter pattern, top-left first then the rest of the top row and the bottom row: RGGB, BGGR, GRBG,
/// GBRG.
enum class BayerPattern : std::uint8_t { RGGB, BGGR, GRBG, GBRG };

struct RawDevelopParams {
    BayerPattern pattern = BayerPattern::RGGB;
    /// Black and white points as fractions of the sample range (0 .. 1).
    double black = 0.0;
    double white = 1.0;
    /// White balance multipliers of the red and blue sites (green is 1).
    double wbRed = 1.0;
    double wbBlue = 1.0;
    /// Camera RGB -> CIE XYZ (D65), row-major; unset: camera RGB is taken as linear Rec.709.
    bool hasMatrix = false;
    std::array<double, 9> cameraToXyz{1, 0, 0, 0, 1, 0, 0, 0, 1};
    /// Scene-linear gain after the matrix, and exposure in stops on top (+1 doubles the light).
    double gain = 1.0;
    double exposureStops = 0.0;
    /// RAW Source Settings' white balance: CIE XYZ -> XYZ after the matrix (whiteBalanceAdaptation); unset: As Shot.
    std::optional<std::array<double, 9>> whiteAdaptation;
};

/// The CIE 1931 xy chromaticity of the light of colour temperature `kelvin` (the Planckian locus, Kim et al.'s cubic
/// approximation, clamped to 1667 .. 25000 K), moved off the locus by `tint` / 3000 in CIE 1960 uv (Adobe's scale):
/// + toward green light, which balancing for turns the picture toward magenta.
std::array<double, 2> lightChromaticity(double kelvin, double tint);

/// The Bradford adaptation in CIE XYZ (D65 the picture's white, row-major) that re-balances a picture balanced for the
/// light `shotKelvin` (on the locus) for the light (`kelvin`, `tint`): a lower temperature turns it bluer, a higher one
/// warmer; the identity when both lights are the same.
std::array<double, 9> whiteBalanceAdaptation(double shotKelvin, double kelvin, double tint);

/// Develops `width` x `height` CFA samples (`cfa`, `strideSamples` per row; values out of `maxValue`) into 10-bit
/// Rec.709 limited-range Y'CbCr 4:4:4 planes (`y`, `cb`, `cr`, `outStride` samples per row, low bits). Width and height
/// at least 2.
void developRaw(const std::uint16_t* cfa, std::size_t strideSamples, int width, int height, int maxValue,
                const RawDevelopParams& params, std::uint16_t* y, std::uint16_t* cb, std::uint16_t* cr,
                std::size_t outStride);

} // namespace wabcut::media
