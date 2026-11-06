#pragma once

// Colour-chart matching (0.7, docs/color.md "Match to a colour chart"): finds an X-Rite ColorChecker Classic (24 patches,
// 6 x 4) in a picture, measures its patches and fits the grade's ordinary parameters so the patches come out at the
// chart's published colours. No machine learning and no new effect: the result is the Basic temperature, tint,
// exposure, contrast and saturation plus the RGB curves and the three hue curves of wabcut.color.grade (editable like
// anything Auto or Color Match write), fitted against the grade's CPU reference (Grade.h) so what it computes is what
// the compositor shows.
//
//   detectColorChecker  automatic: flat regions of about the same size on a 6 x 4 lattice, placed and turned by the
//                       colours (the neutral row has to come out light to dark); a rotated, mirrored or tilted chart is
//                       found, a partly hidden one with at least 10 patches. Falls back to nothing: the app then offers
//                       four corners to drag.
//   measureChart        a trimmed mean of the middle half of every patch, so a speck, a fingerprint or a reflection on
//                       the patch does not move the number.
//   solveChartMatch     global fit (temperature, tint, exposure, contrast, saturation) by Nelder-Mead on the weighted
//                       mean delta E 2000; the neutral row's per-channel curves (Premiere-style RGB curves, one point
//                       per grey step); the saturation again; and hue vs hue / sat / luma points at the hues of the
//                       coloured patches. Every step is kept only when it lowers the error.
//
// Reference colours: X-Rite's published CIE L*a*b* (D50) values of the ColorChecker Classic, both editions (data as
// tabulated by BabelColor, https://babelcolor.com/colorchecker.htm "New color specifications since November 2014", and
// the colour-science project's colour_checkers datasets, BSD-3-Clause). The sRGB value a patch should show is computed
// from them (Bradford D50 -> D65, the sRGB curve) and clamped into the gamut; errors are CIE delta E 2000 (Sharma, Wu
// and Dalal 2005) between the picture as the monitor shows it (code values read through the sRGB curve) and that
// value.

#include <wabcut/color/ColorTools.h>
#include <wabcut/color/Grade.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::color {

inline constexpr int kChartColumns = 6;
inline constexpr int kChartRows = 4;
inline constexpr int kChartPatches = kChartColumns * kChartRows;
/// The width the app and the command render the picture at when they look for a chart (enough for a small chart).
inline constexpr int kChartFrameWidth = 1280;

/// Which print of the chart: X-Rite changed the formulation in November 2014 (about 1 to 2 delta E on some patches).
enum class ChartEdition { AfterNov2014, BeforeNov2014 };

std::string_view chartEditionId(ChartEdition edition); ///< "after-2014" | "before-2014"
std::optional<ChartEdition> chartEditionFromId(std::string_view id);

/// Patch `index` (0..23, row by row from dark skin; 18 is white, 23 black): its name, published L*a*b* (D50), and what
/// the monitor should show (sRGB-encoded code values in [0, 1]) with the L*a*b* of exactly that value (after the
/// clamp into the gamut), which is what errors are measured against.
std::string_view chartPatchName(int index);
Rgb chartReferenceLab(int index, ChartEdition edition);
Rgb chartReferenceCode(int index, ChartEdition edition);
Rgb chartTargetLab(int index, ChartEdition edition);

/// Colour science (exposed for the tests).
double srgbDecode(double code);
double srgbEncode(double linear);
/// sRGB-encoded code values -> CIE L*a*b* (D50, Bradford-adapted).
Rgb srgbToLab(const Rgb& code);
/// CIE delta E 2000 (kL = kC = kH = 1).
double deltaE2000(const Rgb& lab1, const Rgb& lab2);

/// A point in normalised picture coordinates ([0, 1] across and down, y down).
struct ChartPoint {
    double x = 0.0;
    double y = 0.0;
    bool operator==(const ChartPoint&) const = default;
};

/// The outer corners of the 24-patch area as the chart reads upright: top-left (above dark skin), top-right (above
/// bluish green), bottom-right (below black), bottom-left (below white). In the picture they may sit anywhere: turned,
/// mirrored or tilted.
struct ChartCorners {
    std::array<ChartPoint, 4> p{};
    bool operator==(const ChartCorners&) const = default;
};

/// The same four points assigned to the next corner (the chart turned a quarter), and with the chart mirrored left to
/// right; for the app's Rotate and Mirror buttons.
ChartCorners rotatedCorners(const ChartCorners& corners, int quarterTurns = 1);
ChartCorners mirroredCorners(const ChartCorners& corners);
/// A rectangle with the chart's shape in the middle of a picture (`aspect` = width / height), the manual fallback's
/// starting point.
ChartCorners defaultChartCorners(double aspect);

/// Where chart coordinates go in the picture. Chart coordinates are patch pitches: column c, row r has its middle at
/// (kChartCellMiddle + c, kChartCellMiddle + r) and the whole 24-patch area spans [0, kChartWidth] x [0, kChartHeight].
inline constexpr double kChartGap = 0.1;                                  ///< gap between patches, in patch sides
inline constexpr double kChartPatchSide = 1.0 / (1.0 + kChartGap);        ///< a patch's side in pitches
inline constexpr double kChartCellMiddle = kChartPatchSide / 2.0;
inline constexpr double kChartWidth = (kChartColumns - 1) + kChartPatchSide;
inline constexpr double kChartHeight = (kChartRows - 1) + kChartPatchSide;

/// The picture point (normalised) at chart coordinates (x, y); `imageAspect` = picture width / height (the lines
/// stay straight whatever the aspect, since the map is projective).
ChartPoint chartToPicture(const ChartCorners& corners, double x, double y);
/// The four picture points (normalised) of `fraction` of patch `index` around its middle (1 = the whole patch).
std::array<ChartPoint, 4> chartPatchQuad(const ChartCorners& corners, int index, double fraction = 1.0);
/// Whether the four corners make a convex quadrilateral of a sensible size inside (or near) the picture.
bool chartCornersPlausible(const ChartCorners& corners);

struct ChartDetection {
    ChartCorners corners;
    int patches = 0;          ///< patches seen directly (the rest follow from the lattice)
    double confidence = 0.0;  ///< 0..1
};

/// Looks for a chart. `image` is display-referred (BT.709-encoded, as the colour tools get it); alpha < 0.5 is ignored.
std::optional<ChartDetection> detectColorChecker(const ColorImage& image);

struct PatchMeasure {
    Rgb display{0, 0, 0};    ///< trimmed mean of the middle of the patch, code values
    double spread = 0.0;     ///< standard deviation of its luma (noise, texture, a reflection)
    bool usable = false;     ///< not clipped (any channel at white or all at black), not wildly uneven
};

struct ChartMeasurement {
    std::array<PatchMeasure, kChartPatches> patches{};
    int usable = 0;
};

/// Measures the 24 patches of `image` with `corners` (taps on the middle half of every patch).
ChartMeasurement measureChart(const ColorImage& image, const ChartCorners& corners);

struct ChartPatchReport {
    int index = 0;
    std::string name;
    Rgb reference{0, 0, 0}; ///< what the monitor should show
    Rgb before{0, 0, 0};    ///< what it shows now (the clip with its grade, minus the parts the match sets)
    Rgb after{0, 0, 0};     ///< what it shows with the match
    double deltaBefore = 0.0;
    double deltaAfter = 0.0;
    bool used = false;      ///< counted in the fit and the means
};

struct ChartOptions {
    ChartEdition edition = ChartEdition::AfterNov2014;
    /// false: only the white balance, exposure, contrast and the neutral row's curves (no saturation fit, no hue points).
    bool colours = true;
};

struct ChartMatchResult {
    bool ok = false;
    std::string error;                 ///< why not, when !ok
    GradeParams params;                ///< `base` with temperature, tint, exposure, contrast and saturation set
    GradeCurves curves;                ///< `baseCurves` with the RGB curves (master cleared) and the hue curves replaced
    std::array<ChartPatchReport, kChartPatches> patches{};
    int used = 0;
    double meanBefore = 0.0, maxBefore = 0.0;
    double meanAfter = 0.0, maxAfter = 0.0;
    /// The mean error to expect between the patches: the hue points are checked by leaving each coloured patch out in turn
    /// (the curves made from the others, the error at the one left out); the grey row and the global fit are as `meanAfter`.
    /// Never below `meanAfter`; the same when there are no hue points.
    double holdOutMean = 0.0;
    bool improved = false;             ///< the mean fell (a chart put in the wrong place, or turned, does not)
    bool usedRgbCurves = false;
    int hueCurvePoints = 0;            ///< 0: no hue points
    std::string summary;
    std::vector<std::string> warnings;
};

/// The fit. `base` is the clip's grade (its other stages stay and are part of the fit; its temperature, tint,
/// exposure, contrast, saturation, RGB curves and hue curves are what the match replaces); `baseCurves` its curves.
ChartMatchResult solveChartMatch(const ChartMeasurement& measurement, const GradeSpec& base, const GradeCurves& baseCurves,
                                 const ChartOptions& options = {});

} // namespace wabcut::color
