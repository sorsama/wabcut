// Colour management (I.9, color/ColorSpaces.h) against reference values: the ACEScct curve (S-2016-001) and OpenColorIO's
// ACEScct; SMPTE ST 2084 (PQ) and ITU-R BT.2100 HLG reference points and round trips; camera log decodes on neutral
// ramps against the makers' published formulas (S-Log3, V-Log, LogC3 EI 800, LogC4) through OpenColorIO's built-in
// transforms; the input and output plans the compositor runs (analytic curves exactly, baked LUTs within the stated
// tolerance of OpenColorIO's CPU processor); the tone-mapped SDR preview of HDR; and the default Rec.709 project, whose
// plans are exactly today's pipeline. Tolerances: analytic 1e-9 (the tone-mapped preview included); baked 65^3 LUTs
// (tetrahedral) as each test states - camera log inputs 1 % of the colour's largest component + 2.5e-4, the ACES output
// transforms 3 / 1023 on greys and 16 / 1023 (99 % 3) on colours of mild saturation, the ACES round trip of Rec.709
// pictures 20 / 1023 (99 % 8) of OpenColorIO's own.

#include <wabcut/color/ColorSpaces.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <algorithm>
#include <cstdio>
#include <format>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace wabcut;
using namespace wabcut::color;
using Catch::Approx;

namespace {

double sLog3(double code) {
    const double in = code * 1023.0;
    return in >= 171.2102946929 ? std::pow(10.0, (in - 420.0) / 261.5) * (0.18 + 0.01) - 0.01
                                : (in - 95.0) * 0.01125000 / (171.2102946929 - 95.0);
}
double vLog(double v) { return v < 0.181 ? (v - 0.125) / 5.6 : std::pow(10.0, (v - 0.598206) / 0.241514) - 0.00873; }
double logC3(double t) {
    const double a = 5.555556, b = 0.052272, c = 0.247190, d = 0.385537, e = 5.367655, f = 0.092809, cut = 0.010591;
    return t > e * cut + f ? (std::pow(10.0, (t - d) / c) - b) / a : (t - f) / e;
}
double logC4(double e) {
    const double a = (std::pow(2.0, 18) - 16.0) / 117.45, b = (1023.0 - 95.0) / 1023.0, c = 95.0 / 1023.0;
    const double s = (7.0 * std::log(2.0) * std::pow(2.0, 7.0 - 14.0 * c / b)) / (a * b);
    const double t = (std::pow(2.0, 14.0 * (-c / b) + 6.0) - 64.0) / a;
    return e >= 0.0 ? (std::pow(2.0, 14.0 * (e - c) / b + 6.0) - 64.0) / a : e * s + t;
}

// 0.6: the camera logs made from the makers' documents, as each document writes its encoding (scene-linear reflectance
// -> full-range code value): the product decodes, these encode, so a round trip checks one against the other.
double dLogEncode(double x) { // DJI, "White Paper on D-Log and D-Gamut of DJI Cinema Color System" (2017)
    return x <= 0.0078 ? 6.025 * x + 0.0929 : std::log10(x * 0.9892 + 0.0108) * 0.256663 + 0.584555;
}
double fLogEncode(double in) { // Fujifilm, "F-Log Data Sheet" Ver.1.1
    return in < 0.00089 ? 8.735631 * in + 0.092864 : 0.344676 * std::log10(0.555556 * in + 0.009468) + 0.790453;
}
double fLog2Encode(double in) { // Fujifilm, "F-Log2 Data Sheet" Ver.1.0
    return in < 0.000889 ? 8.799461 * in + 0.092864 : 0.245281 * std::log10(5.555556 * in + 0.064829) + 0.384316;
}
double nLogEncode(double y) { // Nikon, "N-Log Specification Document" 1.0.0 (10-bit code values)
    return (y < 0.328 ? 650.0 * std::cbrt(y + 0.0075) : 150.0 * std::log(y) + 619.0) / 1023.0;
}
double sLog2Encode(double r) { // Sony, "S-Log2 Technical Paper": S-Log of 155 / 219 of the light, video range
    const double x = r / 0.9 * 155.0 / 219.0;
    const double y = x >= 0.0 ? 0.432699 * std::log10(x + 0.037584) + 0.616596 + 0.03 : x * 5.0 + 0.030001222851889303;
    return (876.0 * y + 64.0) / 1023.0;
}
double canonLogEncode(double r) { // L. Thorpe, Canon, "Canon-Log Transfer Characteristic" (2012), video range
    const double x = r / 0.9;
    const double c = x >= 0.0 ? 0.529136 * std::log10(10.1596 * x + 1.0) + 0.0730597
                              : -0.529136 * std::log10(-10.1596 * x + 1.0) + 0.0730597;
    return (876.0 * c + 64.0) / 1023.0;
}
double lLogEncode(double lsr) { // Leica Camera AG, "L-Log Reference Manual" V1.9, 4.1
    return lsr <= 0.006 ? 8.0 * lsr + 0.09 : 0.27 * std::log10(1.3 * lsr + 0.0115) + 0.6;
}

bool closeRel(double a, double b, double rel, double abs = 0.0) { return std::fabs(a - b) <= std::max(abs, rel * std::fabs(b)); }

ColorManagement mode(ColorManagementMode m) {
    ColorManagement c;
    c.mode = m;
    return c;
}

} // namespace

TEST_CASE("Colour spaces: OpenColorIO has every built-in transform used", "[color][ocio]") {
    INFO("OpenColorIO " << ocioVersion());
    for (const std::string& style : builtinTransformsUsed()) {
        INFO(style);
        const auto r = applyBuiltin(style, {0.18, 0.18, 0.18});
        CHECK(r.has_value());
    }
}

TEST_CASE("Colour spaces: the ACEScct curve and OpenColorIO's ACEScct agree", "[color][ocio][aces]") {
    CHECK(acesCctFromLinear(0.18) == Approx(0.4135884).margin(1e-7));
    CHECK(acesCctFromLinear(0.0) == Approx(0.0729055341958355));
    for (double lin = 1e-4; lin < 200.0; lin *= 1.37) {
        CHECK(linearFromAcesCct(acesCctFromLinear(lin)) == Approx(lin).epsilon(1e-12));
    }
    // OCIO's ACEScct -> ACES2065-1 on a neutral ramp: AP1 -> AP0 keeps neutrals, so each channel is the curve.
    for (double cct = 0.05; cct < 1.2; cct += 0.0731) {
        const auto r = applyBuiltin("ACEScct_to_ACES2065-1", {cct, cct, cct});
        REQUIRE(r);
        for (const double v : *r) {
            CHECK(closeRel(v, linearFromAcesCct(cct), 1e-5, 1e-7));
        }
    }
}

TEST_CASE("Colour spaces: PQ and HLG reference values and round trips", "[color][hdr]") {
    // SMPTE ST 2084 reference points.
    CHECK(nitsToPq(0.0) == Approx(0.0).margin(1e-6));
    CHECK(nitsToPq(100.0) == Approx(0.5080784).margin(1e-6));
    CHECK(nitsToPq(203.0) == Approx(0.5806888).margin(1e-6));
    CHECK(nitsToPq(1000.0) == Approx(0.7518271).margin(1e-6));
    CHECK(nitsToPq(10000.0) == Approx(1.0).margin(1e-9));
    for (double nits = 0.01; nits <= 10000.0; nits *= 1.9) {
        CHECK(pqToNits(nitsToPq(nits)) == Approx(nits).epsilon(1e-9));
    }
    // BT.2100 HLG: the OETF's knee, its top, and a round trip.
    CHECK(hlgOetf(1.0 / 12.0) == Approx(0.5).margin(1e-9));
    CHECK(hlgOetf(1.0) == Approx(1.0).margin(1e-6));
    for (double e = 0.0; e <= 1.0; e += 0.01) {
        CHECK(hlgInverseOetf(hlgOetf(e)) == Approx(e).margin(1e-12));
    }
    // ITU-R BT.2408: 75 % HLG is the 203-nit reference white of a 1000-nit display.
    const Rgb3 white = hlgToDisplayNits({0.75, 0.75, 0.75}, 1000.0);
    CHECK(white[1] == Approx(203.0).epsilon(0.01));
    const Rgb3 back = displayNitsToHlg(white, 1000.0);
    for (const double v : back) {
        CHECK(v == Approx(0.75).margin(1e-9));
    }
    CHECK(hlgSystemGamma(1000.0) == Approx(1.2));
}

TEST_CASE("Colour spaces: camera logs decode as their makers publish, through OpenColorIO (neutral ramps)",
          "[color][ocio][log]") {
    struct Case {
        InputColorSpace space;
        const char* style;
        double (*decode)(double);
        double grey; ///< the code value of 18 % grey
    };
    const Case cases[] = {
        {InputColorSpace::SLog3SGamut3, "SONY_SLOG3-SGAMUT3_to_ACES2065-1", sLog3, 420.0 / 1023.0},
        {InputColorSpace::SLog3SGamut3Cine, "SONY_SLOG3-SGAMUT3.CINE_to_ACES2065-1", sLog3, 420.0 / 1023.0},
        {InputColorSpace::VLogVGamut, "PANASONIC_VLOG-VGAMUT_to_ACES2065-1", vLog, 0.42331},
        {InputColorSpace::LogC3, "ARRI_ALEXA-LOGC-EI800-AWG_to_ACES2065-1", logC3, 0.39101},
        {InputColorSpace::LogC4, "ARRI_LOGC4_to_ACES2065-1", logC4, 0.2783958},
    };
    for (const Case& c : cases) {
        INFO(c.style);
        CHECK(c.decode(c.grey) == Approx(0.18).epsilon(2e-4)); // the formula itself
        for (double code = 0.1; code <= 0.95; code += 0.05) {
            const auto r = applyBuiltin(c.style, {code, code, code});
            REQUIRE(r);
            for (const double v : *r) {
                CHECK(closeRel(v, c.decode(code), 2e-4, 1e-6));
            }
        }
    }
    // Canon Log 3, Apple Log and RED Log Film (0.6): monotonic neutrals, 18 % grey at a mid code value.
    for (const char* style : {"CANON_CLOG3-CGAMUT_to_ACES2065-1", "APPLE_LOG_to_ACES2065-1", "RED_REDLOGFILM-RWG_to_ACES2065-1"}) {
        INFO(style);
        double previous = -1.0;
        double greyCode = -1.0;
        for (double code = 0.05; code <= 0.95; code += 0.005) {
            const auto r = applyBuiltin(style, {code, code, code});
            REQUIRE(r);
            CHECK((*r)[0] == Approx((*r)[1]).margin(1e-5));
            CHECK((*r)[0] > previous);
            if (greyCode < 0 && (*r)[0] >= 0.18) {
                greyCode = code;
            }
            previous = (*r)[0];
        }
        CHECK(greyCode > 0.3);
        CHECK(greyCode < 0.55);
    }
}

TEST_CASE("Colour spaces: the 0.6 camera logs decode their makers grey and white code values", "[color][ocio][log]") {
    // Each document's 18 % grey and 90 % white as 10-bit code values (its own table; D-Log and N-Log from the equation,
    // their documents print no table) decode to 0.18 and 0.90 within the 10-bit rounding (0.5 %); the exact code value
    // of 18 % grey (the encoding of 0.18) within 1e-4; and exposures from 6 stops under grey to 5 over round-trip through
    // the documents' encodings within 1e-4 (the decode is a 4096-sample 1D LUT of the equation). Every camera log is a
    // camera format with an OpenColorIO transform, and nothing else is.
    struct Case {
        InputColorSpace space;
        double (*encode)(double);
        double grey10;  ///< 18 % grey, 10-bit code value
        double white10; ///< 90 % white
    };
    const Case cases[] = {
        {InputColorSpace::DLogDGamut, dLogEncode, 407.94, 586.12},
        {InputColorSpace::FLogFGamut, fLogEncode, 470.0, 705.0},
        {InputColorSpace::FLog2FGamut, fLog2Encode, 400.0, 570.0},
        {InputColorSpace::NLogNGamut, nLogEncode, 372.03, 603.20},
        {InputColorSpace::SLog2SGamut, sLog2Encode, 347.0, 582.0},
        {InputColorSpace::SLog2SGamut3Cine, sLog2Encode, 347.0, 582.0},
        {InputColorSpace::CanonLogCinemaGamut, canonLogEncode, 351.0, 614.0},
        {InputColorSpace::LLogRec2020, lLogEncode, 445.0, 634.0},
    };
    for (const Case& c : cases) {
        INFO(toString(c.space));
        CHECK(isCameraLog(c.space));
        CHECK(std::fabs(c.encode(0.18) * 1023.0 - c.grey10) < 0.5); // the document's table and its equation agree
        CHECK(std::fabs(c.encode(0.90) * 1023.0 - c.white10) < 0.5);
        for (const auto& [code10, linear] : {std::pair{c.grey10, 0.18}, std::pair{c.white10, 0.90}}) {
            const double v = code10 / 1023.0;
            const auto r = cameraToAces2065(c.space, {v, v, v});
            REQUIRE(r);
            for (const double x : *r) {
                CHECK(closeRel(x, linear, 0.005));
            }
        }
        const double grey = c.encode(0.18);
        const auto g = cameraToAces2065(c.space, {grey, grey, grey});
        REQUIRE(g);
        CHECK((*g)[0] == Approx(0.18).epsilon(1e-4));
        CHECK((*g)[1] == Approx(0.18).epsilon(1e-4));
        CHECK((*g)[2] == Approx(0.18).epsilon(1e-4));
        for (int stop = -6; stop <= 5; ++stop) {
            const double linear = 0.18 * std::exp2(stop);
            const double v = c.encode(linear);
            INFO("stop " << stop << " code " << v);
            const auto r = cameraToAces2065(c.space, {v, v, v});
            REQUIRE(r);
            CHECK(closeRel((*r)[1], linear, 1e-4, 1e-6));
        }
    }
    // The white papers' printed values: D-Log's 18 % grey (0.3988, colour-science's reference) and Canon Log's 2 % black
    // (169 of 1023); L-Log's top code value (1023) is 23.3 (Leica's table, 2330 %).
    const auto dlog = cameraToAces2065(InputColorSpace::DLogDGamut, {0.3987645561893306, 0.3987645561893306, 0.3987645561893306});
    REQUIRE(dlog);
    CHECK((*dlog)[1] == Approx(0.18).epsilon(1e-4));
    const auto black2 = cameraToAces2065(InputColorSpace::CanonLogCinemaGamut, {169.0 / 1023.0, 169.0 / 1023.0, 169.0 / 1023.0});
    REQUIRE(black2);
    CHECK((*black2)[1] == Approx(0.02).epsilon(0.02));
    const auto top = cameraToAces2065(InputColorSpace::LLogRec2020, {1.0, 1.0, 1.0});
    REQUIRE(top);
    CHECK((*top)[1] == Approx(23.30).epsilon(1e-3));

    for (const InputColorSpace space : allInputColorSpaces()) {
        INFO(toString(space));
        CHECK(cameraToAces2065(space, {0.4, 0.4, 0.4}).has_value() == isCameraLog(space));
    }
}

TEST_CASE("Colour spaces: the 0.6 camera gamuts match OpenColorIO's camera transforms", "[color][ocio][log]") {
    // A camera gamut's matrix to ACES2065-1 is CAT02-adapted, as the camera makers' ACES transforms are: the same scene
    // colours (each channel 0.01..2 linear) through the new space and through an OpenColorIO transform of the same
    // primaries (its code values found with the inverse transform of a neutral, channel by channel) agree within 1e-4 of
    // the largest channel (`tolerance`). S-Gamut has S-Gamut3's primaries. F-Gamut, N-Gamut and Leica's L-Log gamut are
    // Rec.2020, as is Apple Log's - but OpenColorIO's Apple Log transform adapts its white with Bradford (no maker's
    // matrix exists for these three), so against it they agree within 0.5 %, and their matrix is checked exactly below.
    struct Case {
        InputColorSpace space;
        double (*encode)(double);
        const char* reference; ///< an OCIO built-in transform with the same primaries
        double tolerance;
    };
    const Case cases[] = {
        {InputColorSpace::SLog2SGamut, sLog2Encode, "SONY_SLOG3-SGAMUT3_to_ACES2065-1", 1e-4},
        {InputColorSpace::SLog2SGamut3Cine, sLog2Encode, "SONY_SLOG3-SGAMUT3.CINE_to_ACES2065-1", 1e-4},
        {InputColorSpace::CanonLogCinemaGamut, canonLogEncode, "CANON_CLOG3-CGAMUT_to_ACES2065-1", 1e-4},
        {InputColorSpace::FLogFGamut, fLogEncode, "APPLE_LOG_to_ACES2065-1", 5e-3},
        {InputColorSpace::FLog2FGamut, fLog2Encode, "APPLE_LOG_to_ACES2065-1", 5e-3},
        {InputColorSpace::NLogNGamut, nLogEncode, "APPLE_LOG_to_ACES2065-1", 5e-3},
        {InputColorSpace::LLogRec2020, lLogEncode, "APPLE_LOG_to_ACES2065-1", 5e-3},
    };
    std::mt19937 rng(23);
    std::uniform_real_distribution<double> stops(std::log2(0.01), 1.0);
    for (const Case& c : cases) {
        INFO(toString(c.space) << " vs " << c.reference);
        for (int k = 0; k < 64; ++k) {
            const Rgb3 scene{std::exp2(stops(rng)), std::exp2(stops(rng)), std::exp2(stops(rng))};
            Rgb3 code{};
            Rgb3 referenceCode{};
            for (std::size_t i = 0; i < 3; ++i) {
                code[i] = c.encode(scene[i]);
                const auto back = applyBuiltin(c.reference, {scene[i], scene[i], scene[i]}, true);
                REQUIRE(back);
                referenceCode[i] = (*back)[0];
            }
            const auto got = cameraToAces2065(c.space, code);
            const auto expected = applyBuiltin(c.reference, referenceCode);
            REQUIRE(got);
            REQUIRE(expected);
            const double largest = std::max({std::fabs((*expected)[0]), std::fabs((*expected)[1]), std::fabs((*expected)[2])});
            for (std::size_t i = 0; i < 3; ++i) {
                CHECK(std::fabs((*got)[i] - (*expected)[i]) <= c.tolerance * largest + 1e-6);
            }
        }
    }
    // The Rec.2020 cameras' matrix, column by column (a scene primary at 1.0, the others at 0): Rec.2020 -> ACES2065-1,
    // CAT02 from D65 to the ACES white (computed independently of the product).
    constexpr double kRec2020ToAp0Cat02[9]{0.6788911506,  0.1588684224, 0.1622404270, 0.0455708309, 0.8607127720,
                                           0.0937163970, -0.0004857104, 0.0250601957, 0.9754255146};
    for (const Case& c : cases) {
        if (std::string_view(c.reference) != "APPLE_LOG_to_ACES2065-1") {
            continue;
        }
        INFO(toString(c.space));
        for (std::size_t column = 0; column < 3; ++column) {
            Rgb3 code{c.encode(0.0), c.encode(0.0), c.encode(0.0)};
            code[column] = c.encode(1.0);
            const auto r = cameraToAces2065(c.space, code);
            REQUIRE(r);
            for (std::size_t row = 0; row < 3; ++row) {
                CHECK((*r)[row] == Approx(kRec2020ToAp0Cat02[row * 3 + column]).margin(2e-5));
            }
        }
    }
    // D-Log / D-Gamut against the ACES transform OpenColorIO's studio config carries (CSC.DJI.DLog_DGamut_to_ACES): the
    // same log segment and matrix, so colours above the toe agree within 1e-4. (OpenColorIO 2.5 and later carry it.)
    const auto probe = applyBuiltinConfigSpace("studio-config-latest", "D-Log D-Gamut", {0.4, 0.4, 0.4});
    INFO("OpenColorIO " << ocioVersion() << ": " << (probe ? std::string("D-Log D-Gamut found") : probe.error().message));
    REQUIRE(probe);
    CHECK((*probe)[1] == Approx(0.18214).epsilon(1e-3)); // D-Log code 0.4: (10^(3.89616 x 0.4 - 2.27752) - 0.0108) / 0.9892
    for (int k = 0; k < 64; ++k) {
        const Rgb3 scene{std::exp2(stops(rng)), std::exp2(stops(rng)), std::exp2(stops(rng))};
        const Rgb3 code{dLogEncode(scene[0]), dLogEncode(scene[1]), dLogEncode(scene[2])};
        const auto got = cameraToAces2065(InputColorSpace::DLogDGamut, code);
        const auto expected = applyBuiltinConfigSpace("studio-config-latest", "D-Log D-Gamut", code);
        REQUIRE(got);
        REQUIRE(expected);
        const double largest = std::max({std::fabs((*expected)[0]), std::fabs((*expected)[1]), std::fabs((*expected)[2])});
        for (std::size_t i = 0; i < 3; ++i) {
            CHECK(std::fabs((*got)[i] - (*expected)[i]) <= 1e-4 * largest + 1e-6);
        }
    }
}

/// The worst of a sample, and its 99th percentile.
struct Worst {
    std::vector<double> errors;
    void add(double e) { errors.push_back(e); }
    double max() const { return errors.empty() ? 0.0 : *std::max_element(errors.begin(), errors.end()); }
    double p99() const {
        if (errors.empty()) {
            return 0.0;
        }
        std::vector<double> s = errors;
        std::sort(s.begin(), s.end());
        return s[static_cast<std::size_t>(0.99 * static_cast<double>(s.size() - 1))];
    }
};

TEST_CASE("Colour spaces: input plans run what the references compute (analytic exactly, LUTs within tolerance)",
          "[color][ocio][plans]") {
    // Baked inputs (camera logs): |LUT - OpenColorIO| <= 1 % of the colour's largest component + 2.5e-4, in working-space
    // linear values. Apple Log's code values under 0.03 are left out: far below its black (0.15), its square-root toe is
    // steepest there (errors up to 0.005, below black). Direct Rec.709's tone-mapped log (0.2: the camera's scene light
    // from the LUT, then the HDR capture tone map) is measured as the output writes it, in Rec.709 code values of 1023:
    // greys <= 2, any colour <= 54 (99 % <= 2; the worst are colours far outside every display gamut, whose negative
    // scene components the tone map clips - as the ACES output transforms' bound). (The I.9 modes first, in their order; Direct Rec.709's log draws its
    // samples from a generator of its own, so the other modes see the samples their tolerances were measured on.)
    std::mt19937 rng(7);
    std::mt19937 directRng(17);
    std::uniform_real_distribution<double> code(0.0, 1.0);
    Worst lut;
    Worst directGrey;
    Worst directAny;
    const auto bt709 = [](double linear) {
        const double a = 1.09929682680944, b = 0.018053968510807;
        const double v = std::clamp(linear, 0.0, 1.0);
        return v < b ? 4.5 * v : a * std::pow(v, 0.45) - (a - 1.0);
    };
    for (const ColorManagementMode m : {ColorManagementMode::DisplayRec709, ColorManagementMode::Aces13, ColorManagementMode::HdrPq,
                                        ColorManagementMode::HdrHlg, ColorManagementMode::WideGamut, ColorManagementMode::Disabled}) {
        for (const InputColorSpace space : allInputColorSpaces()) {
            for (const bool limited : {false, true}) {
                INFO(toString(m) << " / " << toString(space) << (limited ? " (video range)" : ""));
                const ColorManagement cm = mode(m);
                const auto plan = planInput(cm, space, limited, false);
                REQUIRE(plan);
                const bool displayReferred = m == ColorManagementMode::Aces13 &&
                                             (space == InputColorSpace::Rec709 || space == InputColorSpace::Srgb ||
                                              space == InputColorSpace::Rec2020);
                if (displayReferred) {
                    continue; // the inverse output transform: checked by its round trip below
                }
                const bool direct = plan->kind == InputPlan::Kind::LutToneMapSdr;
                std::mt19937& gen = direct ? directRng : rng;
                for (int k = 0; k < 256; ++k) {
                    const double grey = code(gen);
                    Rgb3 c = k % 4 == 0 ? Rgb3{grey, grey, grey} : Rgb3{code(gen), code(gen), code(gen)};
                    if (space == InputColorSpace::AppleLog) {
                        for (double& v : c) {
                            v = std::max(v, 0.03);
                        }
                    }
                    const auto expected = applyInput(cm, space, limited, false, c);
                    REQUIRE(expected);
                    const Rgb3 got = runInputPlan(*plan, c);
                    const double largest = std::max({std::fabs((*expected)[0]), std::fabs((*expected)[1]), std::fabs((*expected)[2])});
                    for (int i = 0; i < 3; ++i) {
                        const double e = (*expected)[static_cast<std::size_t>(i)];
                        const double g = got[static_cast<std::size_t>(i)];
                        if (plan->kind != InputPlan::Kind::Lut && !direct) {
                            CHECK(closeRel(g, e, 1e-9, 1e-12));
                        } else if (direct) {
                            (k % 4 == 0 ? directGrey : directAny).add(std::fabs(bt709(g) - bt709(e)) * 1023.0);
                        } else {
                            const double tol = 0.01 * largest + 2.5e-4;
                            lut.add(std::fabs(g - e) / tol);
                            CHECK(std::fabs(g - e) <= tol);
                        }
                    }
                }
            }
        }
    }
    WARN(std::format("camera log LUTs: worst {:.2f} of the tolerance, 99th percentile {:.2f}", lut.max(), lut.p99()));
    WARN(std::format("Direct Rec.709 log LUTs: greys worst {:.2f} / 1023; any colour worst {:.2f} (99 %: {:.2f})",
                     directGrey.max(), directAny.max(), directAny.p99()));
    CHECK(directGrey.max() <= 2.0);
    CHECK(directAny.max() <= 54.0);
    CHECK(directAny.p99() <= 2.0);
}

TEST_CASE("Colour spaces: output plans run what the references compute", "[color][ocio][plans]") {
    // Analytic outputs (Rec.709, PQ, HLG, the tone-mapped SDR preview) exactly. The ACES output transforms (a 65^3 LUT of
    // display light, then the display's encoding exactly), against OpenColorIO's whole chain, in code values of 1023:
    // greys <= 3, colours of mild saturation (each channel 0.6..1.4 x a level) <= 16 (99 % <= 3), any colour (channels up
    // to 14 stops apart, far outside the display's gamut) <= 54 (99 % <= 6).
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> stops(-8.0, 6.0);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    for (const ColorManagementMode m : {ColorManagementMode::DisplayRec709, ColorManagementMode::Aces13, ColorManagementMode::HdrPq,
                                        ColorManagementMode::HdrHlg, ColorManagementMode::WideGamut, ColorManagementMode::Disabled}) {
        for (const OutputTarget target : {OutputTarget::Sdr, OutputTarget::Pq, OutputTarget::Hlg}) {
            INFO(toString(m) << " -> " << static_cast<int>(target));
            const ColorManagement cm = mode(m);
            const auto plan = planOutput(cm, target);
            REQUIRE(plan);
            Worst grey, mild, any;
            for (int k = 0; k < 3000; ++k) {
                const double level = 0.18 * std::exp2(stops(rng));
                const int kind = k % 3;
                Rgb3 w{level, level, level};
                if (kind == 1) {
                    w = {level * (0.6 + 0.8 * unit(rng)), level * (0.6 + 0.8 * unit(rng)), level * (0.6 + 0.8 * unit(rng))};
                } else if (kind == 2) {
                    w = {0.18 * std::exp2(stops(rng)), 0.18 * std::exp2(stops(rng)), 0.18 * std::exp2(stops(rng))};
                }
                const auto expected = applyOutput(cm, target, w);
                REQUIRE(expected);
                const Rgb3 got = runOutputPlan(*plan, w);
                for (int i = 0; i < 3; ++i) {
                    const double d = std::fabs(got[static_cast<std::size_t>(i)] - (*expected)[static_cast<std::size_t>(i)]);
                    if (plan->kind != OutputPlan::Kind::Lut) {
                        CHECK(d <= 1e-9);
                    } else {
                        (kind == 0 ? grey : kind == 1 ? mild : any).add(d * 1023.0);
                    }
                }
            }
            if (plan->kind == OutputPlan::Kind::Lut) {
                WARN(std::format("{} -> {}: greys worst {:.2f} / 1023; mild saturation worst {:.2f} (99 %: {:.2f}); any colour "
                                 "worst {:.2f} (99 %: {:.2f})",
                                 toString(m), static_cast<int>(target), grey.max(), mild.max(), mild.p99(), any.max(), any.p99()));
                CHECK(grey.max() <= 3.0);
                CHECK(mild.max() <= 16.0);
                CHECK(mild.p99() <= 3.0);
                CHECK(any.max() <= 54.0);
                CHECK(any.p99() <= 6.0);
            }
        }
    }
}

TEST_CASE("Colour spaces: in an ACES project Rec.709 pictures and graphics look as they did (round trip)",
          "[color][ocio][aces]") {
    // Code values 0.1..0.9 per channel: OpenColorIO both ways within 6 / 1023 of the picture; both baked LUTs as the GPU
    // runs them within 20 / 1023 of OpenColorIO's round trip (99 % within 8). Greys (white and grey titles) within
    // 3 / 1023. The most saturated display colours (a channel near 0 or 1 with the others far from it) do not come back,
    // even through OpenColorIO: ACES 1.x's output transform never produces them (it clamps negative AP1 values and rolls
    // highlights off), so their inverse has no exact answer - reported, not checked.
    const ColorManagement aces = mode(ColorManagementMode::Aces13);
    const auto out = planOutput(aces, OutputTarget::Sdr);
    REQUIRE(out);
    CHECK(out->label == "ACES 1.0 SDR Video");
    for (const bool graphics : {false, true}) {
        const auto in = planInput(aces, InputColorSpace::Rec709, false, graphics);
        REQUIRE(in);
        CHECK(in->kind == InputPlan::Kind::Lut);
        Worst exactTrip, gpuTrip, exactInner, gpuInner, greys;
        for (double v = 0.0; v <= 1.0001; v += 1.0 / 64) {
            const Rgb3 code{v, v, v};
            const Rgb3 gpu = runOutputPlan(*out, runInputPlan(*in, code));
            for (std::size_t i = 0; i < 3; ++i) {
                greys.add(std::fabs(gpu[i] - code[i]) * 1023.0);
            }
        }
        for (double r = 0.0; r <= 1.0001; r += 1.0 / 32) {
            for (double g = 0.0; g <= 1.0001; g += 1.0 / 8) {
                for (double b = 0.0; b <= 1.0001; b += 1.0 / 8) {
                    const Rgb3 code{r, g, b};
                    const auto exact = applyOutput(aces, OutputTarget::Sdr, *applyInput(aces, InputColorSpace::Rec709, false, graphics, code));
                    REQUIRE(exact);
                    const Rgb3 gpu = runOutputPlan(*out, runInputPlan(*in, code));
                    const bool inner = std::min({r, g, b}) >= 0.1 && std::max({r, g, b}) <= 0.9;
                    for (std::size_t i = 0; i < 3; ++i) {
                        exactTrip.add(std::fabs((*exact)[i] - code[i]) * 1023.0);
                        gpuTrip.add(std::fabs(gpu[i] - (*exact)[i]) * 1023.0);
                        if (inner) {
                            exactInner.add(std::fabs((*exact)[i] - code[i]) * 1023.0);
                            gpuInner.add(std::fabs(gpu[i] - (*exact)[i]) * 1023.0);
                        }
                    }
                }
            }
        }
        WARN(std::format("ACES Rec.709 round trip{}: OpenColorIO worst {:.2f} / 1023 (99 %: {:.2f}); the GPU's LUTs vs "
                         "OpenColorIO worst {:.2f} (99 %: {:.2f})",
                         graphics ? " (graphics)" : "", exactTrip.max(), exactTrip.p99(), gpuTrip.max(), gpuTrip.p99()));
        WARN(std::format("  codes 0.1..0.9: OpenColorIO worst {:.2f} (99 %: {:.2f}); LUTs vs OpenColorIO worst {:.2f} (99 %: {:.2f})",
                         exactInner.max(), exactInner.p99(), gpuInner.max(), gpuInner.p99()));
        WARN(std::format("  greys through both LUTs: worst {:.2f} / 1023", greys.max()));
        CHECK(exactInner.max() <= 6.0);
        CHECK(gpuInner.max() <= 20.0);
        CHECK(gpuInner.p99() <= 8.0);
        CHECK(greys.max() <= 3.0);
    }
}

TEST_CASE("Colour spaces: the default Rec.709 project is today's pipeline", "[color][plans]") {
    const ColorManagement rec709;
    REQUIRE(rec709.isDefault());
    const auto in = planInput(rec709, InputColorSpace::Rec709, true, false);
    REQUIRE(in);
    CHECK(in->kind == InputPlan::Kind::Analytic);
    CHECK(in->transfer == TransferFunction::BT709);
    CHECK(in->scale == 1.0);
    for (int i = 0; i < 9; ++i) {
        CHECK(in->matrix[static_cast<std::size_t>(i)] == Approx(i % 4 == 0 ? 1.0 : 0.0).margin(1e-12));
    }
    const auto out = planOutput(rec709, OutputTarget::Sdr);
    REQUIRE(out);
    CHECK(out->kind == OutputPlan::Kind::Rec709);
    CHECK(out->label.empty());
    // Graphics in a Rec.709 project: sRGB stills as before (no inverse output transform).
    const auto graphics = planInput(rec709, InputColorSpace::Srgb, false, true);
    REQUIRE(graphics);
    CHECK(graphics->kind == InputPlan::Kind::Analytic);
}

TEST_CASE("Colour spaces: Direct Rec.709 tone-maps log and HDR media on input (0.2)", "[color][ocio][plans]") {
    const ColorManagement direct; // Direct Rec.709 (SDR), the default
    // Camera log: its scene light through OpenColorIO (a baked LUT), then tone-mapped as an HDR capture is.
    const auto plan = planInput(direct, InputColorSpace::SLog3SGamut3Cine, false, false);
    REQUIRE(plan);
    CHECK(plan->kind == InputPlan::Kind::LutToneMapSdr);
    for (const InputColorSpace space : allInputColorSpaces()) {
        if (!isCameraLog(space)) {
            continue;
        }
        INFO(toString(space));
        double previous = -1.0;
        for (int k = 0; k <= 20; ++k) { // a grey ramp of code values: monotone, inside SDR (tone-mapped, clipped)
            const double v = 0.05 + 0.9 * k / 20.0;
            const auto linear = applyInput(direct, space, false, false, {v, v, v});
            REQUIRE(linear);
            CHECK((*linear)[1] >= previous - 1e-6);
            CHECK((*linear)[1] >= 0.0);
            CHECK((*linear)[1] <= 1.0 + 1e-9);
            previous = (*linear)[1];
        }
    }
    // S-Log3's 18 % grey (code 420 / 1023) stays mid grey: the mid-tones pass the tone map unchanged (below its knee).
    const double grey = 420.0 / 1023.0;
    const auto mid = applyInput(direct, InputColorSpace::SLog3SGamut3Cine, false, false, {grey, grey, grey});
    REQUIRE(mid);
    CHECK((*mid)[1] == Approx(0.18).epsilon(0.03));
    CHECK((*mid)[0] == Approx((*mid)[1]).margin(2e-3)); // a grey stays grey
    // The scene's white (S-Log3 90 % reflectance, code 598 / 1023, i.e. 0.9 scene linear) sits below SDR white with room
    // for highlights; four stops over it are held at SDR white.
    const auto white = applyInput(direct, InputColorSpace::SLog3SGamut3Cine, false, false, {598.0 / 1023.0, 598.0 / 1023.0, 598.0 / 1023.0});
    REQUIRE(white);
    CHECK((*white)[1] > 0.6);
    CHECK((*white)[1] < 0.95);

    // HDR media (PQ / HLG): tone-mapped as HDR game captures always were.
    const auto pq = planInput(direct, InputColorSpace::Rec2100Pq, true, false);
    REQUIRE(pq);
    CHECK(pq->kind == InputPlan::Kind::ToneMapSdr);
    CHECK(pq->transfer == TransferFunction::PQ);
    const Rgb3 code{0.58, 0.58, 0.58}; // ~203 nits: the reference white
    const auto sdr = applyInput(direct, InputColorSpace::Rec2100Pq, true, false, code);
    REQUIRE(sdr);
    const auto capture = hdrCaptureToSdr(TransferFunction::PQ, code);
    REQUIRE(capture);
    CHECK((*sdr)[0] == Approx((*capture)[0]).margin(1e-12));
    // SDR media stays as it is (byte for byte): BT.709 analytic, no matrix.
    const auto rec709 = planInput(direct, InputColorSpace::Rec709, true, false);
    REQUIRE(rec709);
    CHECK(rec709->kind == InputPlan::Kind::Analytic);
    CHECK(rec709->transfer == TransferFunction::BT709);
}

TEST_CASE("Colour spaces: Wide Gamut without tone mapping passes SDR media through and clips the rest (0.2)",
          "[color][plans]") {
    ColorManagement wide;
    wide.mode = ColorManagementMode::WideGamut;
    CHECK(workingOf(wide.mode) == Working::AcesCg);
    const auto out = planOutput(wide, OutputTarget::Sdr);
    REQUIRE(out);
    CHECK(out->kind == OutputPlan::Kind::Rec709);
    CHECK_FALSE(out->label.empty());
    // A Rec.709 picture into ACEScg (analytic, no inverse output transform) and back out: the same code values.
    const auto in = planInput(wide, InputColorSpace::Rec709, false, true);
    REQUIRE(in);
    CHECK(in->kind == InputPlan::Kind::Analytic);
    for (const Rgb3 code : {Rgb3{0.1, 0.5, 0.9}, Rgb3{1.0, 0.0, 0.0}, Rgb3{0.5, 0.5, 0.5}, Rgb3{0.0, 0.0, 0.0}}) {
        const Rgb3 working = runInputPlan(*in, code);
        const Rgb3 back = runOutputPlan(*out, working);
        for (int i = 0; i < 3; ++i) {
            CHECK(back[static_cast<std::size_t>(i)] == Approx(code[static_cast<std::size_t>(i)]).margin(1e-9));
        }
    }
    // Above the output's white: clipped, not tone-mapped.
    const Rgb3 bright = runOutputPlan(*out, {4.0, 4.0, 4.0});
    CHECK(bright[1] == Approx(1.0).margin(1e-12));
    // Camera log reaches the working space through OpenColorIO (a baked LUT), as in Wide Gamut (Tone Mapped).
    const auto log = planInput(wide, InputColorSpace::LogC4, false, false);
    REQUIRE(log);
    CHECK(log->kind == InputPlan::Kind::Lut);
}

TEST_CASE("Colour spaces: the tone-mapped SDR preview of HDR keeps the mid-tones and lands the peak on SDR white",
          "[color][hdr]") {
    constexpr double white = 203.0, peak = 1000.0;
    // Mid grey (18 % of the reference white) is below the knee: unchanged (Rec.2020 -> Rec.709 of a neutral is neutral).
    const Rgb3 grey = toneMapToSdr({0.18, 0.18, 0.18}, white, peak);
    CHECK(grey[1] == Approx(0.18).epsilon(1e-6));
    // The mastering peak lands on SDR white; above it, held there.
    const Rgb3 top = toneMapToSdr({peak / white, peak / white, peak / white}, white, peak);
    CHECK(top[1] == Approx(1.0).epsilon(1e-6));
    CHECK(toneMapToSdr({3 * peak / white, 3 * peak / white, 3 * peak / white}, white, peak)[1] == Approx(1.0).epsilon(1e-6));
    // Monotonic between.
    double previous = 0.0;
    for (double v = 0.01; v <= peak / white; v *= 1.1) {
        const double out = toneMapToSdr({v, v, v}, white, peak)[1];
        CHECK(out >= previous);
        previous = out;
    }
    // The monitor's label says what it shows.
    ColorManagement hdr;
    hdr.mode = ColorManagementMode::HdrPq;
    const auto plan = planOutput(hdr, OutputTarget::Sdr);
    REQUIRE(plan);
    CHECK(plan->label == "HDR · tone-mapped SDR preview");
}

TEST_CASE("Colour spaces: stream tags choose the input space (Auto)", "[color][plans]") {
    ColorInfo c;
    c.transfer = TransferFunction::PQ;
    CHECK(inputSpaceOf(c, false) == InputColorSpace::Rec2100Pq);
    c.transfer = TransferFunction::HLG;
    CHECK(inputSpaceOf(c, false) == InputColorSpace::Rec2100Hlg);
    c.transfer = TransferFunction::BT709;
    c.primaries = ColorPrimaries::BT2020;
    CHECK(inputSpaceOf(c, false) == InputColorSpace::Rec2020);
    c.primaries = ColorPrimaries::BT709;
    CHECK(inputSpaceOf(c, false) == InputColorSpace::Rec709);
    c.transfer = TransferFunction::Unknown;
    CHECK(inputSpaceOf(c, true) == InputColorSpace::Srgb);
}

// ---- 0.6 (#72): HDR finishing --------------------------------------------------------------------------------------

namespace {

double rec709Oetf(double v) {
    const double a = 1.09929682680944, b = 0.018053968510807;
    v = std::clamp(v, 0.0, 1.0);
    return v < b ? 4.5 * v : a * std::pow(v, 0.45) - (a - 1.0);
}

ColorManagement setup(ColorManagementMode mode) {
    ColorManagement m;
    m.mode = mode;
    return m;
}

} // namespace

TEST_CASE("Colour spaces: gamut compression follows ACES 1.3 RGC and leaves the gamut core alone (0.6)", "[color][gamut]") {
    // Greys and colours whose distances from the achromatic axis stay below the thresholds pass unchanged.
    for (const Rgb3 c : {Rgb3{0.5, 0.5, 0.5}, Rgb3{0.5, 0.3, 0.25}, Rgb3{2.0, 1.2, 0.9}, Rgb3{0.0, 0.0, 0.0}}) {
        const Rgb3 out = gamutCompress(c);
        for (std::size_t i = 0; i < 3; ++i) {
            CHECK(out[i] == Approx(c[i]).margin(1e-12));
        }
    }
    // Outside the gamut (a negative channel): pulled in, the largest channel (the achromatic axis) kept.
    const Rgb3 outside{1.0, -0.2, 0.1};
    const Rgb3 in = gamutCompress(outside);
    CHECK(in[0] == Approx(1.0));
    CHECK(in[1] > -0.2);
    CHECK(in[1] >= 0.0); // its distance 1.2 is inside magenta's limit (1.264): it lands inside the gamut
    CHECK(in[2] > 0.1); // 0.9 from the axis, past yellow's threshold (0.880): compressed a little
    CHECK(in[2] < 0.12);
    // The limits land exactly on the gamut boundary (distance 1: the channel at 0).
    CHECK(gamutCompress({1.0 - 1.147, 1.0, 1.0})[0] == Approx(0.0).margin(1e-9));
    CHECK(gamutCompress({1.0, 1.0 - 1.264, 1.0})[1] == Approx(0.0).margin(1e-9));
    CHECK(gamutCompress({1.0, 1.0, 1.0 - 1.312})[2] == Approx(0.0).margin(1e-9));
    // The strength mixes the original and the compressed colour.
    const Rgb3 half = gamutCompress(outside, 0.5);
    CHECK(half[1] == Approx((outside[1] + in[1]) / 2.0));
    CHECK(gamutCompress(outside, 0.0)[1] == Approx(-0.2));
    // Monotone: farther out stays farther out.
    double last = 1.0;
    for (double g = 0.9; g >= -0.5; g -= 0.01) {
        const double out = gamutCompress({1.0, g, 1.0})[1];
        CHECK(out <= last + 1e-12);
        last = out;
    }
}

TEST_CASE("Colour spaces: Wide Gamut Minimal Tone Mapping rolls off scene-referred highlights and passes SDR media (0.6)",
          "[color][plans][widegamut]") {
    const ColorManagement minimal = setup(ColorManagementMode::WideGamutMinimal);
    CHECK(workingOf(minimal.mode) == Working::AcesCg);
    CHECK(minimal.managed());
    CHECK(minimal.compressesGamut());
    const double white = minimalRolloffWhite(minimal);
    CHECK(white == Approx(1000.0 / 203.0));
    // The curve: as it is up to the knee, slope 1 there, the HDR peak on 1.0, monotone over a grey ramp.
    CHECK(minimalRolloffCurve(0.5, white) == 0.5);
    CHECK(minimalRolloffCurve(kMinimalKnee, white) == Approx(kMinimalKnee));
    CHECK((minimalRolloffCurve(kMinimalKnee + 1e-6, white) - kMinimalKnee) / 1e-6 == Approx(1.0).epsilon(1e-4));
    CHECK(minimalRolloffCurve(white, white) == Approx(1.0));
    CHECK(minimalRolloffCurve(20.0, white) == 1.0);
    double last = -1.0;
    for (double v = 0.0; v <= 6.0; v += 0.01) {
        const double out = minimalRolloffCurve(v, white);
        CHECK(out >= last);
        CHECK(out <= 1.0);
        last = out;
    }
    // Hue kept: the colour is scaled along its largest channel.
    const Rgb3 warm = minimalRolloff({3.0, 1.5, 0.75}, white);
    CHECK(warm[1] / warm[0] == Approx(0.5));
    CHECK(warm[2] / warm[0] == Approx(0.25));
    // SDR media and graphics: no roll-off, no compression - the same code values out as in.
    const auto out = planOutput(minimal, OutputTarget::Sdr);
    REQUIRE(out);
    CHECK(out->label == "Wide Gamut · minimal tone mapping");
    for (const bool graphics : {false, true}) {
        const auto in = planInput(minimal, InputColorSpace::Rec709, false, graphics);
        REQUIRE(in);
        CHECK(in->gamutStrength == 0.0);
        CHECK(in->rolloffWhite == 0.0);
        for (const Rgb3 code : {Rgb3{0.1, 0.5, 0.9}, Rgb3{1.0, 0.0, 0.0}, Rgb3{1.0, 1.0, 1.0}, Rgb3{0.95, 0.9, 0.2}}) {
            const Rgb3 back = runOutputPlan(*out, runInputPlan(*in, code));
            for (std::size_t i = 0; i < 3; ++i) {
                CHECK(back[i] == Approx(code[i]).margin(1e-9));
            }
        }
    }
    // Camera log: compressed and rolled off.
    const auto log = planInput(minimal, InputColorSpace::SLog3SGamut3Cine, false, false);
    REQUIRE(log);
    CHECK(log->gamutStrength == 1.0);
    CHECK(log->rolloffWhite == Approx(white));
    // PQ fixtures read with 1.0 = the reference white: 100 nits below the knee as it is, the 1000-nit peak on white,
    // the 203-nit reference white rolled off a little (not clipped as without tone mapping).
    const auto pq = planInput(minimal, InputColorSpace::Rec2100Pq, false, false);
    REQUIRE(pq);
    const auto pqGrey = [&](double nits) { return runInputPlan(*pq, {nitsToPq(nits), nitsToPq(nits), nitsToPq(nits)})[1]; };
    CHECK(pqGrey(100.0) == Approx(100.0 / 203.0).epsilon(1e-6));
    CHECK(pqGrey(1000.0) == Approx(1.0).epsilon(1e-6));
    CHECK(pqGrey(203.0) == Approx(minimalRolloffCurve(1.0, white)).epsilon(1e-6));
    CHECK(pqGrey(203.0) < 0.95);
    CHECK(pqGrey(4000.0) == Approx(1.0));
    // HLG fixture: its 75 % reference white (BT.2408) lands near the same place, highlights below 1.0.
    const auto hlg = planInput(minimal, InputColorSpace::Rec2100Hlg, false, false);
    REQUIRE(hlg);
    const double hlgWhite = runInputPlan(*hlg, {0.75, 0.75, 0.75})[1];
    CHECK(hlgWhite > 0.8);
    CHECK(hlgWhite < 1.0);
    CHECK(runInputPlan(*hlg, {1.0, 1.0, 1.0})[1] <= 1.0);
    // Without tone mapping the same PQ white clips (read at 100 nits = 1.0).
    const auto noTm = planInput(setup(ColorManagementMode::WideGamut), InputColorSpace::Rec2100Pq, false, false);
    REQUIRE(noTm);
    CHECK(runInputPlan(*noTm, {nitsToPq(203.0), nitsToPq(203.0), nitsToPq(203.0)})[1] == Approx(2.03).epsilon(1e-6));
}

TEST_CASE("Colour spaces: gamut compression is opt-in for managed setups and keeps negative working values (0.6)",
          "[color][plans][gamut]") {
    ColorManagement m = setup(ColorManagementMode::Aces13);
    CHECK_FALSE(m.compressesGamut());
    const auto plain = planInput(m, InputColorSpace::SLog3SGamut3, false, false);
    REQUIRE(plain);
    CHECK(plain->gamutStrength == 0.0);
    m.gamutCompression = true;
    m.gamutCompressionStrength = 0.6;
    CHECK(m.compressesGamut());
    const auto compressed = planInput(m, InputColorSpace::SLog3SGamut3, false, false);
    REQUIRE(compressed);
    CHECK(compressed->gamutStrength == Approx(0.6));
    // Display-referred pictures are left exactly as they are.
    const auto rec709 = planInput(m, InputColorSpace::Rec709, false, false);
    REQUIRE(rec709);
    CHECK(rec709->gamutStrength == 0.0);
    // A saturated S-Gamut3 blue: negative in ACEScg; compressed it comes in, and the plan returns the value as it is
    // (the layer pass no longer clips it at 0).
    const Rgb3 before = runInputPlan(*plain, {0.25, 0.25, 0.95});
    const Rgb3 after = runInputPlan(*compressed, {0.25, 0.25, 0.95});
    REQUIRE(std::min({before[0], before[1], before[2]}) < 0.0);
    CHECK(std::min({after[0], after[1], after[2]}) > std::min({before[0], before[1], before[2]}));
    const auto applied = applyInput(m, InputColorSpace::SLog3SGamut3, false, false, {0.25, 0.25, 0.95});
    REQUIRE(applied);
    CHECK(std::min({(*applied)[0], (*applied)[1], (*applied)[2]}) > std::min({before[0], before[1], before[2]}) - 0.02);
    // Direct Rec.709 and Disable Color Management never compress.
    ColorManagement direct = setup(ColorManagementMode::DisplayRec709);
    direct.gamutCompression = true;
    CHECK_FALSE(direct.compressesGamut());
}

TEST_CASE("Colour spaces: the output colour spaces Rec.2020 SDR and P3-D65 (0.6)", "[color][plans][output]") {
    CHECK(outputTargetOf(OutputColorSpace::Rec2020, OutputTarget::Sdr) == OutputTarget::Rec2020Sdr);
    CHECK(outputTargetOf(OutputColorSpace::P3D65, OutputTarget::Sdr) == OutputTarget::P3D65);
    CHECK(outputTargetOf(OutputColorSpace::Automatic, OutputTarget::Sdr) == OutputTarget::Sdr);
    CHECK(outputTargetOf(OutputColorSpace::P3D65, OutputTarget::Pq) == OutputTarget::Pq);
    CHECK(isSdrTarget(OutputTarget::P3D65));
    CHECK_FALSE(isSdrTarget(OutputTarget::Hlg));
    const ColorManagement direct;
    // Rec.2020 SDR: Rec.709's primaries converted, the BT.709 curve; white stays white, mid grey stays mid grey.
    const auto r2020 = planOutput(direct, OutputTarget::Rec2020Sdr);
    REQUIRE(r2020);
    CHECK(r2020->curve == OutputPlan::Curve::Bt709);
    const Rgb3 red = runOutputPlan(*r2020, {1.0, 0.0, 0.0});
    CHECK(red[0] == Approx(rec709Oetf(0.627404)).margin(1e-4));
    CHECK(red[1] == Approx(rec709Oetf(0.069097)).margin(1e-4));
    CHECK(runOutputPlan(*r2020, {0.18, 0.18, 0.18})[1] == Approx(rec709Oetf(0.18)).margin(1e-9));
    // P3-D65: a 2.6 power.
    const auto p3 = planOutput(direct, OutputTarget::P3D65);
    REQUIRE(p3);
    CHECK(p3->curve == OutputPlan::Curve::Gamma26);
    CHECK(runOutputPlan(*p3, {0.18, 0.18, 0.18})[0] == Approx(std::pow(0.18, 1.0 / 2.6)).margin(1e-9));
    CHECK(runOutputPlan(*p3, {1.0, 1.0, 1.0})[2] == Approx(1.0).margin(1e-9));
    // An HDR project's SDR versions in those spaces: the same tone map, the target's primaries.
    const ColorManagement hdr = setup(ColorManagementMode::HdrPq);
    const auto hdr709 = planOutput(hdr, OutputTarget::Sdr);
    const auto hdr2020 = planOutput(hdr, OutputTarget::Rec2020Sdr);
    REQUIRE(hdr709);
    REQUIRE(hdr2020);
    CHECK(hdr2020->kind == OutputPlan::Kind::ToneMapSdr);
    CHECK(runOutputPlan(*hdr709, {0.5, 0.5, 0.5})[1] == Approx(runOutputPlan(*hdr2020, {0.5, 0.5, 0.5})[1]).margin(1e-9));
    // ACES: the SDR output transform onto those displays, within the LUT's tolerance of OpenColorIO's own chain.
    const ColorManagement aces = setup(ColorManagementMode::Aces13);
    for (const OutputTarget target : {OutputTarget::Rec2020Sdr, OutputTarget::P3D65}) {
        const auto plan = planOutput(aces, target);
        REQUIRE(plan);
        CHECK(plan->kind == OutputPlan::Kind::Lut);
        for (const double v : {0.02, 0.18, 0.5, 1.0, 4.0}) {
            const auto exact = applyOutput(aces, target, {v, v, v});
            REQUIRE(exact);
            CHECK(runOutputPlan(*plan, {v, v, v})[1] == Approx((*exact)[1]).margin(3.0 / 1023.0));
        }
    }
    // What the export details say.
    CHECK(describeOutput(direct, OutputTarget::P3D65, false).find("P3-D65") != std::string::npos);
    ColorManagement conformed = hdr;
    conformed.sdrConform.enabled = true;
    CHECK(describeOutput(conformed, OutputTarget::Sdr, true).find("SDR Conform (BT.2390, ACES gamut compression)") !=
          std::string::npos);
    CHECK(describeOutput(setup(ColorManagementMode::WideGamut), OutputTarget::Sdr, false).find("clipped") != std::string::npos);
}

TEST_CASE("Colour spaces: SDR Conform tone-maps by the operator and maps the gamut (0.6)", "[color][conform]") {
    // The operators: monotone, the source peak on white, BT.2390 nearly the identity in the shadows.
    for (const ToneMapOperator op : {ToneMapOperator::Bt2390, ToneMapOperator::Aces, ToneMapOperator::Reinhard}) {
        INFO(displayName(op));
        double last = 0.0;
        for (double s = 0.0; s <= 6.0; s += 0.02) {
            const double y = conformToneMap(op, s, 4.926, 100.0);
            CHECK(y >= last - 1e-12);
            CHECK(y <= 1.0);
            last = y;
        }
        CHECK(conformToneMap(op, 4.926, 4.926, 100.0) == Approx(1.0).margin(1e-6));
    }
    CHECK(conformToneMap(ToneMapOperator::Bt2390, 0.05, 4.926, 100.0) == Approx(0.05).epsilon(1e-3));
    CHECK(conformToneMap(ToneMapOperator::Reinhard, 0.5, 0.9, 100.0) == 0.5); // a source that fits: as it is

    // BT.2390 measured against the reference operator: with the paper white and target peak at the reference white,
    // SDR Conform's luminance is the tone-mapped SDR preview's (color::toneMapToSdr) on a grey ramp.
    ColorManagement hdr = setup(ColorManagementMode::HdrPq);
    hdr.sdrConform.enabled = true;
    hdr.sdrConform.targetPeakNits = 203.0;
    hdr.sdrConform.paperWhiteNits = 203.0;
    hdr.sdrConform.gamut = GamutMapping::Clip;
    CHECK(hdr.conformsSdr());
    const auto plan = planOutput(hdr, OutputTarget::Sdr, true);
    REQUIRE(plan);
    REQUIRE(plan->kind == OutputPlan::Kind::Conform);
    for (double v = 0.0; v <= 6.0; v += 0.05) {
        const Rgb3 reference = toneMapToSdr({v, v, v}, 203.0, 1000.0);
        const ConformPixel got = sdrConformPixel(*plan, {v, v, v});
        CHECK(got.code[1] == Approx(rec709Oetf(reference[1])).margin(1e-9));
        CHECK(got.toneMapped == (v > 1.0 + 1e-6));
        CHECK_FALSE(got.gamutMapped);
    }
    // The Rec.2020 primaries at mid luminance lie outside Rec.709: flagged, and each mapping brings them in.
    for (const GamutMapping mapping : {GamutMapping::AcesRgc, GamutMapping::Saturation, GamutMapping::Clip}) {
        INFO(displayName(mapping));
        hdr.sdrConform.gamut = mapping;
        const auto p = planOutput(hdr, OutputTarget::Sdr, true);
        REQUIRE(p);
        for (const Rgb3 primary : {Rgb3{0.5, 0.0, 0.0}, Rgb3{0.0, 0.5, 0.0}, Rgb3{0.0, 0.0, 0.5}}) {
            const ConformPixel px = sdrConformPixel(*p, primary);
            CHECK(px.gamutMapped);
            for (const double c : px.code) {
                CHECK(c >= 0.0);
                CHECK(c <= 1.0);
            }
            // The hue's dominant channel stays dominant.
            const auto dominant = std::max_element(primary.begin(), primary.end()) - primary.begin();
            CHECK(std::max_element(px.code.begin(), px.code.end()) - px.code.begin() == dominant);
        }
    }
    // Saturation mapping keeps the luminance of a colour that only crosses 0.
    hdr.sdrConform.gamut = GamutMapping::Saturation;
    const auto sat = planOutput(hdr, OutputTarget::Sdr, true);
    REQUIRE(sat);
    const ConformPixel green = sdrConformPixel(*sat, {0.05, 0.4, 0.05});
    const auto decode = [](double v) {
        const double a = 1.09929682680944, b = 0.018053968510807;
        return v < 4.5 * b ? v / 4.5 : std::pow((v + (a - 1.0)) / a, 1.0 / 0.45);
    };
    const double luma709 = 0.2126 * decode(green.code[0]) + 0.7152 * decode(green.code[1]) + 0.0722 * decode(green.code[2]);
    const double luma2020 = 0.2627 * 0.05 + 0.6780 * 0.4 + 0.0593 * 0.05;
    CHECK(luma709 == Approx(luma2020).epsilon(0.01));
    // Legal range: nothing at 100 %.
    hdr.sdrConform.legalRange = true;
    const auto legal = planOutput(hdr, OutputTarget::Sdr, true);
    REQUIRE(legal);
    const ConformPixel peak = sdrConformPixel(*legal, {10.0, 10.0, 10.0});
    CHECK(peak.code[1] < 0.99);
    CHECK(peak.code[1] > 0.95);
    CHECK(peak.toneMapped);
    // Wide Gamut (No and Minimal) conform too; ACES (Tone Mapped) keeps its output transform; SDR setups never.
    ColorManagement wide = setup(ColorManagementMode::WideGamut);
    wide.sdrConform.enabled = true;
    CHECK(wide.conformsSdr());
    CHECK(planOutput(wide, OutputTarget::P3D65, true)->kind == OutputPlan::Kind::Conform);
    ColorManagement aces = setup(ColorManagementMode::Aces13);
    aces.sdrConform.enabled = true;
    CHECK_FALSE(aces.conformsSdr());
    CHECK(planOutput(aces, OutputTarget::Sdr, true)->kind == OutputPlan::Kind::Lut);
    ColorManagement direct;
    direct.sdrConform.enabled = true;
    CHECK_FALSE(direct.conformsSdr());
    // HDR outputs are never conformed.
    CHECK(planOutput(hdr, OutputTarget::Pq, true)->kind == OutputPlan::Kind::Pq);
}
