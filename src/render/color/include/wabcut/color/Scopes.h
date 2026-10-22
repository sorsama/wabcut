#pragma once

// Video scopes (docs/color.md, "Scopes"): waveforms (luma, RGB parade, YCbCr parade), a vectorscope and
// histograms, plus the statistics an editor or an agent reads off them ("is this shot underexposed?").
//
// The input is the picture as shown: display-referred (BT.709-encoded) R'G'B' in [0, 1], with an optional coverage
// alpha (pixels with alpha 0 are not counted; a partial alpha counts as a weight of 1 - scopes count pixels). The
// binning below is the contract the GPU scopes (src/render/shaders/scopes.comp) follow, so both give the same counts:
//
//   column    = floor(x * columns / width)                 (waveforms)
//   level(v)  = min(levels - 1, floor(clamp(v, 0, 1) * levels))
//   Y'        = Rec.709 luma of R'G'B';  Cb = (B' - Y') / 1.8556;  Cr = (R' - Y') / 1.5748     (each in [-0.5, 0.5])
//   vector    = (vbin(Cb), vbin(-Cr)) with vbin(c) = clamp(floor(size * (0.5 + c * zoom) + 0.5), 0, size - 1): bin k is
//               centred on c = (k / size - 0.5) / zoom, so zero chroma is the middle of bin size / 2 (Cr up, like a
//               hardware vectorscope; zoom 1, 2 or 5)
//   histogram = level(R'), level(G'), level(B'), level(Y') with `levels` bins
//
// IRE is Y' x 100 (0 = black, 100 = reference white); nits follow BT.1886 for a 100 nit display: 100 * V^2.4.

#include <wabcut/color/ColorMath.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace wabcut::color {

struct ScopeSettings {
    int columns = 256;   ///< waveform columns
    int levels = 256;    ///< waveform rows and histogram bins
    int vectorSize = 256;
    double vectorZoom = 1.0; ///< 1, 2 or 5
    /// Only every step-th pixel in x and y is read (the GPU and CPU paths use the same grid). 0 = automatic: the
    /// smallest step that keeps at most ~600k pixels.
    int step = 0;
};

/// Counts per bin. Waveforms are columns x levels, row-major by level (index = level * columns + column).
struct ScopeData {
    ScopeSettings settings;
    int sourceWidth = 0;
    int sourceHeight = 0;
    int step = 1;
    std::vector<std::uint32_t> waveLuma;
    std::array<std::vector<std::uint32_t>, 3> waveRgb;  ///< R', G', B'
    std::array<std::vector<std::uint32_t>, 2> waveCbCr; ///< Cb + 0.5, Cr + 0.5 (the YCbCr parade's Y is waveLuma)
    std::vector<std::uint32_t> vector;                  ///< vectorSize^2, index = y * size + x
    std::array<std::vector<std::uint32_t>, 4> histogram; ///< R', G', B', Y'
    std::uint64_t pixels = 0;                           ///< pixels counted
    /// I.9: what the levels are - 0: SDR code values (the IRE scale), 1: PQ code values of an HDR project (the nits
    /// scale: 0.508 = 100, 0.581 = 203, 0.752 = 1000, 1 = 10000 nits), 2: the HLG signal (0..100 %, 75 % = reference
    /// white).
    int scale = 0;
};

/// Statistics of a picture for people and agents. Levels are IRE (0..100).
struct ScopeStats {
    std::uint64_t pixels = 0;
    double lumaMin = 0, lumaMax = 0, lumaMean = 0;
    double p01 = 0, p05 = 0, p50 = 0, p95 = 0, p99 = 0; ///< luma percentiles (IRE)
    double clippedHigh = 0; ///< fraction of pixels with a channel >= 99.5 %
    double clippedLow = 0;  ///< fraction with Y' <= 0.5 %
    Rgb meanRgb{0, 0, 0};   ///< mean R'G'B' (0..1)
    double saturationMean = 0; ///< mean chroma magnitude sqrt(Cb^2 + Cr^2), x 100 (a 75 % bar is about 40)
    double castHue = 0;     ///< hue of the mean chroma vector, degrees (0 = +Cb axis, counter-clockwise, Cr up)
    double castStrength = 0; ///< magnitude of the mean chroma vector x 100 (0 = neutral on average)
    /// "underexposed", "overexposed", "low contrast", "clipped highlights", "crushed blacks" or "ok" (first match).
    std::string exposure;
    /// "neutral" or the cast's colour name ("warm/orange", "green", "magenta", "blue", ...).
    std::string cast;
};

/// Pixel step for a picture of this size (settings.step, or automatic).
int scopeStep(const ScopeSettings& settings, int width, int height);
/// Bins of a display-referred float RGB(A) picture (`channels` 3 or 4, rows of `width * channels` floats).
ScopeData computeScopes(const float* pixels, int width, int height, int channels, const ScopeSettings& settings = {});
/// Same for 8-bit RGBA rows (`stride` bytes per row).
ScopeData computeScopes8(const std::uint8_t* rgba, int width, int height, int stride, const ScopeSettings& settings = {});
/// Statistics of the binned picture (luma, clipping and cast from the histograms and vectorscope).
ScopeStats scopeStats(const ScopeData& data);

/// Where a colour bar target lands on the vectorscope: (Cb, Cr) of the colour at `amount` (0.75 or 1).
std::array<double, 2> vectorscopeTarget(const Rgb& color, double amount);
/// The skin-tone line's angle (degrees, from +Cb, counter-clockwise): 123.
inline constexpr double kSkinToneAngle = 123.0;

/// Images for display, RGBA8 rows (width x height x 4): a waveform (mode 0 luma, 1 RGB parade, 2 YCbCr parade; the
/// parades are three columns-wide panels side by side), the vectorscope and the RGB + luma histogram. Brightness is a
/// log scale of the counts, lifted so that a single hit stays visible (40 % of full) on a dark background; empty bins
/// are transparent.
std::vector<std::uint8_t> renderWaveform(const ScopeData& data, int mode, int& width, int& height);
std::vector<std::uint8_t> renderVectorscope(const ScopeData& data, int& width, int& height);
std::vector<std::uint8_t> renderHistogram(const ScopeData& data, int& width, int& height);

/// The same counts in coarser bins: `columnFactor` waveform columns, `levelFactor` levels (waveforms and histograms)
/// and `vectorFactor` x `vectorFactor` vectorscope bins are summed into one. A factor that does not divide its size
/// counts as 1. A view drawn smaller than the scope's bins pools first, so a thin trace keeps its weight instead of
/// being filtered away by the downscale.
ScopeData pooledScopes(const ScopeData& data, int columnFactor, int levelFactor, int vectorFactor);

} // namespace wabcut::color
