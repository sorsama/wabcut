// HDR game captures (I.9, gaming; color/Hdr.h): SMPTE ST 2084 (PQ) and ITU-R BT.2100 HLG reference points and round
// trips, the BT.2390 tone map into SDR, and what PQ / HLG captures become in an SDR project.

#include <wabcut/color/Hdr.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::color;
using Catch::Approx;

TEST_CASE("HDR: PQ and HLG reference values and round trips", "[color][hdr]") {
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
    CHECK(hlgToDisplayNits({0.75, 0.75, 0.75}, 1000.0)[1] == Approx(203.0).epsilon(0.01));
    CHECK(hlgSystemGamma(1000.0) == Approx(1.2));
}

TEST_CASE("HDR: the BT.2390 tone map keeps the mid-tones and lands the peak on SDR white", "[color][hdr]") {
    constexpr double white = 203.0, peak = 1000.0;
    // Mid grey (18 % of the reference white) is below the knee: unchanged (Rec.2020 -> Rec.709 of a neutral is neutral).
    CHECK(toneMapToSdr({0.18, 0.18, 0.18}, white, peak)[1] == Approx(0.18).epsilon(1e-6));
    // The master's peak lands on SDR white; above it, held there.
    CHECK(toneMapToSdr({peak / white, peak / white, peak / white}, white, peak)[1] == Approx(1.0).epsilon(1e-6));
    CHECK(toneMapToSdr({3 * peak / white, 3 * peak / white, 3 * peak / white}, white, peak)[1] == Approx(1.0).epsilon(1e-6));
    double previous = 0.0;
    for (double v = 0.01; v <= peak / white; v *= 1.1) {
        const double out = toneMapToSdr({v, v, v}, white, peak)[1];
        CHECK(out >= previous);
        previous = out;
    }
    // Saturated Rec.2020 colours outside Rec.709 are clipped, never negative or above 1.
    const Rgb3 green = toneMapToSdr({0.0, 2.0, 0.0}, white, peak);
    for (const double v : green) {
        CHECK(v >= 0.0);
        CHECK(v <= 1.0);
    }
}

TEST_CASE("HDR: PQ and HLG captures read into SDR, other transfers left alone", "[color][hdr]") {
    // PQ at the reference white (203 nits) is a near-white grey; at the 1000-nit peak, white.
    const double atWhite = nitsToPq(203.0);
    const auto white = hdrCaptureToSdr(TransferFunction::PQ, {atWhite, atWhite, atWhite});
    REQUIRE(white);
    CHECK((*white)[1] > 0.7);
    CHECK((*white)[1] < 0.85);
    const double atPeak = nitsToPq(1000.0);
    CHECK((*hdrCaptureToSdr(TransferFunction::PQ, {atPeak, atPeak, atPeak}))[1] == Approx(1.0).epsilon(1e-6));
    // HLG 75 % is the reference white too.
    const auto hlg = hdrCaptureToSdr(TransferFunction::HLG, {0.75, 0.75, 0.75});
    REQUIRE(hlg);
    CHECK((*hlg)[1] == Approx((*white)[1]).epsilon(0.02));
    CHECK_FALSE(hdrCaptureToSdr(TransferFunction::BT709, {0.5, 0.5, 0.5}).has_value());
    CHECK_FALSE(hdrCaptureToSdr(TransferFunction::SRGB, {0.5, 0.5, 0.5}).has_value());
    // Rec.2020 -> Rec.709 keeps white.
    const Rgb3 w = color::apply(rec2020ToRec709(), {1.0, 1.0, 1.0});
    for (const double v : w) {
        CHECK(v == Approx(1.0).epsilon(1e-9));
    }
}
