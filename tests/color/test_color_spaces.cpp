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
    // Canon Log 3 and Apple Log: monotonic neutrals, 18 % grey at a mid code value.
    for (const char* style : {"CANON_CLOG3-CGAMUT_to_ACES2065-1", "APPLE_LOG_to_ACES2065-1"}) {
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
