// Reference matching (0.7, cards 045, 104, 141): the average spectrum of a signal in the Graphic Equalizer's bands, the EQ
// Match curve fitted from two spectra and realised by the Graphic Equalizer (30 Bands) through the effect chain, the level
// accumulator, and the Loudness Radar's inputs: the short-term series of the loudness meter, the live range, the tap's
// history, and the folding of both into a revolution.

#include "DspTestUtil.h"

#include <wabcut/audio/LoudnessTap.h>
#include <wabcut/audio/dsp/Biquad.h>
#include <wabcut/audio/dsp/Loudness.h>
#include <wabcut/audio/dsp/Match.h>
#include <wabcut/audio/edit/MatchCurve.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <vector>

using namespace wabcut;
using namespace wabcut::audio;
using namespace wabcut::audio::test;
using Catch::Approx;

namespace {

constexpr int kRate = 48000;

/// The profile of interleaved audio as the match commands read it.
edit::SpectrumProfile profileOf(const std::vector<float>& x, int channels) {
    dsp::SpectrumAverager averager(channels, kRate);
    averager.add(x.data(), x.size() / static_cast<std::size_t>(channels));
    const dsp::SpectrumAverage a = averager.result();
    edit::SpectrumProfile p;
    p.levelDb = a.levelDb;
    p.seconds = a.seconds;
    p.activeSeconds = a.activeSeconds;
    p.sampleRate = a.sampleRate;
    return p;
}

/// `x` through a biquad on every channel.
std::vector<float> filtered(std::vector<float> x, int channels, const dsp::BiquadCoefficients& c) {
    std::vector<dsp::BiquadState> state(static_cast<std::size_t>(channels));
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = state[i % static_cast<std::size_t>(channels)].process(c, x[i]);
    }
    return x;
}

bool matched(int band) { return band >= edit::kMatchFirstBand && band < edit::kMatchFirstBand + edit::kMatchBands; }

/// The mean of a band-wise quantity over the matched bands.
template <class F>
double meanOverMatched(F value) {
    double sum = 0.0;
    for (int b = edit::kMatchFirstBand; b < edit::kMatchFirstBand + edit::kMatchBands; ++b) {
        sum += value(b);
    }
    return sum / edit::kMatchBands;
}

} // namespace

TEST_CASE("Match: the average spectrum of filtered noise differs from white noise by the filter's response", "[audio][match]") {
    const std::vector<float> white = noise(0.3, 40.0, 2, kRate, 7);
    const dsp::BiquadCoefficients shelf = dsp::highShelf(kRate, 3000.0, -6.0);
    const std::vector<float> shaped = filtered(white, 2, shelf);
    const edit::SpectrumProfile flat = profileOf(white, 2);
    const edit::SpectrumProfile tilted = profileOf(shaped, 2);
    REQUIRE(flat.usable());
    REQUIRE(tilted.usable());
    for (int b = edit::kMatchFirstBand; b < edit::kMatchFirstBand + edit::kMatchBands; ++b) {
        INFO("band " << b << " at " << edit::matchBandHz(b) << " Hz");
        const auto i = static_cast<std::size_t>(b);
        CHECK(tilted.levelDb[i] - flat.levelDb[i] == Approx(dsp::magnitudeDb(shelf, edit::matchBandHz(b), kRate)).margin(0.5));
    }
}

TEST_CASE("Match: a pause between phrases does not shape the average spectrum", "[audio][match]") {
    // 3 s of noise with a high shelf cut, then 3 s of a quiet noise floor, four times over.
    const std::vector<float> loud = filtered(noise(0.3, 3.0, 2, kRate, 11), 2, dsp::highShelf(kRate, 3000.0, -6.0));
    const std::vector<float> floorNoise = noise(0.0005, 3.0, 2, kRate, 99);
    std::vector<float> speechLike;
    for (int i = 0; i < 4; ++i) {
        append(speechLike, loud);
        append(speechLike, floorNoise);
    }
    const edit::SpectrumProfile withPauses = profileOf(speechLike, 2);
    std::vector<float> onlyLoud;
    for (int i = 0; i < 4; ++i) {
        append(onlyLoud, loud);
    }
    const edit::SpectrumProfile alone = profileOf(onlyLoud, 2);
    REQUIRE(withPauses.usable());
    // About half of it was pause, and none of it counted.
    CHECK(withPauses.activeSeconds == Approx(12.0).margin(2.5)); // plus the frames that straddle an edge
    CHECK(withPauses.seconds == Approx(24.0));
    for (int b = edit::kMatchFirstBand; b < edit::kMatchFirstBand + edit::kMatchBands; ++b) {
        INFO("band " << b);
        CHECK(withPauses.levelDb[static_cast<std::size_t>(b)] == Approx(alone.levelDb[static_cast<std::size_t>(b)]).margin(0.7));
    }
}

TEST_CASE("Match: silence and a clip shorter than a second give no usable profile and no curve", "[audio][match]") {
    const edit::SpectrumProfile silent = profileOf(std::vector<float>(static_cast<std::size_t>(kRate) * 4 * 2, 0.0F), 2);
    CHECK_FALSE(silent.usable());
    const edit::SpectrumProfile blip = profileOf(noise(0.3, 0.3, 2, kRate, 5), 2);
    CHECK_FALSE(blip.usable());
    const edit::SpectrumProfile good = profileOf(noise(0.3, 6.0, 2, kRate, 5), 2);
    CHECK_FALSE(edit::fitEqMatch(silent, good).valid);
    CHECK_FALSE(edit::fitEqMatch(good, blip).valid);
    CHECK(edit::fitEqMatch(good, good).valid);
}

TEST_CASE("Match: EQ Match recovers a known tilt within 0.5 dB and the Graphic EQ makes the target sound like it", "[audio][match]") {
    // The reference is a tilted version of the target's kind of material: noise through a +6 dB peak at 2 kHz and a high
    // cut; the target is plain noise (another seed).
    const dsp::BiquadCoefficients peak = dsp::peaking(kRate, 2000.0, 6.0, 0.8);
    const dsp::BiquadCoefficients cut = dsp::highShelf(kRate, 6000.0, -9.0);
    const std::vector<float> referenceSignal = filtered(filtered(noise(0.3, 40.0, 2, kRate, 21), 2, peak), 2, cut);
    const std::vector<float> targetSignal = noise(0.3, 40.0, 2, kRate, 22);
    const edit::SpectrumProfile reference = profileOf(referenceSignal, 2);
    const edit::SpectrumProfile target = profileOf(targetSignal, 2);

    edit::EqMatchOptions raw;
    raw.smoothing = 0.0; // follow each band
    const edit::EqMatchCurve curve = edit::fitEqMatch(reference, target, raw);
    REQUIRE(curve.valid);
    const auto expected = [&](int b) {
        return dsp::magnitudeDb(peak, edit::matchBandHz(b), kRate) + dsp::magnitudeDb(cut, edit::matchBandHz(b), kRate);
    };
    const double expectedMean = meanOverMatched(expected);
    for (int b = edit::kMatchFirstBand; b < edit::kMatchFirstBand + edit::kMatchBands; ++b) {
        INFO("band " << b << " at " << edit::matchBandHz(b) << " Hz");
        // The level is left alone: the curve is the response with its mean removed.
        CHECK(curve.gainDb[static_cast<std::size_t>(b)] == Approx(expected(b) - expectedMean).margin(0.5));
    }
    // Limits: never more than +-18 dB, and the bands beside the matched ones fade out.
    for (const double g : curve.gainDb) {
        CHECK(std::fabs(g) <= edit::kMatchMaxGainDb);
    }
    CHECK(std::fabs(curve.gainDb[static_cast<std::size_t>(edit::kMatchFirstBand - 1)]) <=
          std::fabs(curve.gainDb[static_cast<std::size_t>(edit::kMatchFirstBand)]) / 2.0 + 1e-9);

    // The Graphic Equalizer (30 Bands), through the effect chain preview and export both use, turns the target into
    // something that has the reference's shape.
    std::vector<EffectParam> params;
    for (int b = 0; b < edit::kMatchBandCount; ++b) {
        params.push_back({"band" + std::to_string(b + 1), ParamValue(curve.gainDb[static_cast<std::size_t>(b)])});
    }
    const std::vector<float> matchedSignal = runEffect(effectOf("wabcut.audio.graphicEq30", params), targetSignal, 2, kRate);
    const edit::SpectrumProfile after = profileOf(matchedSignal, 2);
    // Same shape: the difference to the reference is flat across the bands (its spread is what is left of the tone).
    const double offset = meanOverMatched([&](int b) {
        return reference.levelDb[static_cast<std::size_t>(b)] - after.levelDb[static_cast<std::size_t>(b)];
    });
    for (int b = edit::kMatchFirstBand; b < edit::kMatchFirstBand + edit::kMatchBands; ++b) {
        INFO("band " << b << " at " << edit::matchBandHz(b) << " Hz");
        // Up to 10 kHz within 1 dB. The two top bands (12.7 and 16 kHz at 48 kHz) are realised 1 to 3 dB short: the Graphic
        // Equalizer's bell filters narrow towards the sample rate's limit, and a plateau of -9 dB across its sliders measures
        // -7.7 and -6.0 dB there, so the match ends gently where the equalizer can no longer hold it.
        const double margin = b <= 25 ? 1.0 : 3.5;
        CHECK(reference.levelDb[static_cast<std::size_t>(b)] - after.levelDb[static_cast<std::size_t>(b)] ==
              Approx(offset).margin(margin));
    }
}

TEST_CASE("Match: a reference far from the target is limited to 18 dB a band, and Amount scales the curve", "[audio][match]") {
    const std::vector<float> targetSignal = noise(0.3, 12.0, 2, kRate, 3);
    // 45 dB of high-frequency boost: past what a band may move (the mean removed, the low bands want -22 dB too).
    const std::vector<float> referenceSignal = filtered(noise(0.3, 12.0, 2, kRate, 4), 2, dsp::highShelf(kRate, 1000.0, 45.0));
    const edit::SpectrumProfile target = profileOf(targetSignal, 2);
    const edit::SpectrumProfile reference = profileOf(referenceSignal, 2);
    edit::EqMatchOptions full;
    full.smoothing = 0.0;
    const edit::EqMatchCurve curve = edit::fitEqMatch(reference, target, full);
    REQUIRE(curve.valid);
    double most = 0.0;
    for (const double g : curve.gainDb) {
        most = std::max(most, std::fabs(g));
    }
    CHECK(most == Approx(18.0).margin(1e-9));
    edit::EqMatchOptions half = full;
    half.amount = 0.5;
    const edit::EqMatchCurve gentle = edit::fitEqMatch(reference, target, half);
    for (int b = edit::kMatchFirstBand; b < edit::kMatchFirstBand + edit::kMatchBands; ++b) {
        const auto i = static_cast<std::size_t>(b);
        CHECK(std::fabs(gentle.gainDb[i]) <= std::fabs(curve.gainDb[i]) + 1e-9);
    }
    edit::EqMatchOptions none = full;
    none.amount = 0.0;
    for (const double g : edit::fitEqMatch(reference, target, none).gainDb) {
        CHECK(g == 0.0);
    }
    CHECK(matched(edit::kMatchFirstBand));
}

TEST_CASE("Match: mono and 5.1 signals are analysed like stereo, the LFE left out", "[audio][match]") {
    const std::vector<float> mono = noise(0.3, 8.0, 1, kRate, 31);
    CHECK(profileOf(mono, 1).usable());
    // 5.1: a loud LFE does not change the average.
    const std::vector<float> quiet = noise(0.2, 8.0, 6, kRate, 32);
    std::vector<float> withLfe = quiet;
    for (std::size_t f = 0; f < withLfe.size() / 6; ++f) {
        withLfe[f * 6 + 3] = 0.9F * static_cast<float>(std::sin(2.0 * 3.14159265 * 60.0 * static_cast<double>(f) / kRate));
    }
    const edit::SpectrumProfile a = profileOf(quiet, 6);
    const edit::SpectrumProfile b = profileOf(withLfe, 6);
    REQUIRE(a.usable());
    for (int band = edit::kMatchFirstBand; band < edit::kMatchFirstBand + edit::kMatchBands; ++band) {
        CHECK(a.levelDb[static_cast<std::size_t>(band)] == Approx(b.levelDb[static_cast<std::size_t>(band)]).margin(0.05));
    }
}

TEST_CASE("Match: the level accumulator reads RMS peak and crest factor", "[audio][match]") {
    dsp::LevelAccumulator levels(2);
    const std::vector<float> tone = sine(1000.0, 0.5, 2.0, 2, kRate);
    levels.add(tone.data(), tone.size() / 2);
    CHECK(levels.rmsDbfs() == Approx(-9.03).margin(0.02));
    CHECK(levels.samplePeakDbfs() == Approx(-6.02).margin(0.02));
    CHECK(levels.crestDb() == Approx(3.01).margin(0.03));
    dsp::LevelAccumulator silent(2);
    const std::vector<float> nothing(2000, 0.0F);
    silent.add(nothing.data(), 1000);
    CHECK_FALSE(std::isfinite(silent.rmsDbfs()));
    CHECK(silent.crestDb() == 0.0);
}

// ---- The Loudness Radar's inputs -------------------------------------------------------------------------------------

TEST_CASE("Radar: the loudness meter reads the short-term loudness once a second and the live range", "[audio][loudness][radar]") {
    dsp::LoudnessMeter meter(2, kRate);
    // 4 s at -23 LUFS-ish, then 4 s 10 dB louder.
    const std::vector<float> quietPart = sine(1000.0, 0.05, 4.0, 2, kRate);
    const std::vector<float> loudPart = sine(1000.0, 0.158, 4.0, 2, kRate);
    meter.add(quietPart.data(), quietPart.size() / 2);
    meter.add(loudPart.data(), loudPart.size() / 2);
    const std::vector<float>& series = meter.shortTermSeries();
    REQUIRE(series.size() == 8);
    CHECK_FALSE(std::isfinite(series[0])); // the first 2 s have no 3 s window
    CHECK_FALSE(std::isfinite(series[1]));
    CHECK(std::isfinite(series[2]));
    CHECK(series[3] == Approx(meter.result().shortTermSeries[3])); // the result carries it too
    CHECK(series[7] - series[3] == Approx(10.0).margin(0.7));       // the loud part reads about 10 LU higher
    CHECK(meter.rangeLu() == Approx(meter.result().rangeLu));
    CHECK(meter.rangeLu() > 5.0);
    meter.reset();
    CHECK(meter.shortTermSeries().empty());
}

TEST_CASE("Radar: the tap keeps a history of the short-term loudness and a live range", "[audio][loudness][radar]") {
    LoudnessTap tap(2, kRate, LoudnessTapOptions{false});
    const std::vector<float> tone = sine(1000.0, 0.1, 6.0, 2, kRate);
    for (std::size_t at = 0; at + 4800 * 2 <= tone.size(); at += 4800 * 2) {
        tap.push(std::span<const float>(tone.data() + at, 4800 * 2));
        tap.update();
    }
    const std::vector<float> history = tap.shortTermHistory();
    REQUIRE(history.size() == 6);
    CHECK(std::isfinite(history[5]));
    CHECK(history[5] == Approx(tap.reading().shortTermLufs).margin(0.3));
    CHECK(tap.reading().rangeLu >= 0.0);
    CHECK(tap.shortTermHistory(2).size() == 2);
    tap.reset();
    tap.update();
    CHECK(tap.shortTermHistory().empty());
}

TEST_CASE("Radar: a programme folds into one revolution and a live lap into the lap's slots", "[audio][radar]") {
    // 600 seconds at -23, with a 20 second shout at -10 starting at second 300.
    std::vector<float> series(600, -23.0F);
    for (int i = 300; i < 320; ++i) {
        series[static_cast<std::size_t>(i)] = -10.0F;
    }
    const edit::RadarFrame programme = edit::foldProgramme(series, 1.0, 120);
    REQUIRE(programme.values.size() == 120);
    CHECK(programme.secondsPerSlot == Approx(5.0));
    CHECK(programme.head == -1);
    CHECK(programme.values[0] == Approx(-23.0F));
    CHECK(programme.values[60] == Approx(-10.0F).margin(0.01)); // seconds 300 to 305
    CHECK(programme.values[63] == Approx(-10.0F).margin(0.01));
    CHECK(programme.values[64] == Approx(-23.0F));
    // Loudness averages as energy, not as dB: a slot with half -10 and half -23 is nearer the loud one.
    const edit::RadarFrame coarse = edit::foldProgramme(series, 1.0, 30); // 20 s per slot: seconds 300-320 are one slot
    CHECK(coarse.values[15] == Approx(-10.0F).margin(0.01));
    std::vector<float> mixed(10, -23.0F);
    for (int i = 5; i < 10; ++i) {
        mixed[static_cast<std::size_t>(i)] = -13.0F;
    }
    const edit::RadarFrame one = edit::foldProgramme(mixed, 1.0, 1);
    CHECK(one.values[0] > -18.0F); // the mean of the dB is -18; the energy mean is nearer the loud half
    CHECK(one.values[0] == Approx(-15.6F).margin(0.1));
    // Unknown seconds (the first two of a measurement) leave their slot empty.
    std::vector<float> start(10, -20.0F);
    start[0] = start[1] = -std::numeric_limits<float>::infinity();
    const edit::RadarFrame partial = edit::foldProgramme(start, 1.0, 10);
    CHECK(std::isnan(partial.values[0]));
    CHECK(partial.values[2] == Approx(-20.0F));

    // Live, a 60 second lap: 150 seconds in, the sweep is at second 30 of the third lap, the slots after it still show
    // the lap before.
    std::vector<float> live(150, -30.0F);
    for (std::size_t i = 120; i < 150; ++i) {
        live[i] = -20.0F;
    }
    const edit::RadarFrame frame = edit::foldLive(live, 60);
    REQUIRE(frame.values.size() == 60);
    CHECK(frame.head == 29);
    CHECK(frame.values[0] == Approx(-20.0F)); // seconds 120..149 of this lap
    CHECK(frame.values[29] == Approx(-20.0F));
    CHECK(frame.values[30] == Approx(-30.0F)); // the lap before
    // The first lap is not full yet: the slots ahead are empty.
    const edit::RadarFrame first = edit::foldLive(std::vector<float>(10, -25.0F), 60);
    CHECK(first.head == 9);
    CHECK(first.values[9] == Approx(-25.0F));
    CHECK(std::isnan(first.values[10]));
    // A long lap shares slots (3600 s in 360 slots: 10 s each).
    const edit::RadarFrame hour = edit::foldLive(std::vector<float>(3600, -22.0F), 3600);
    CHECK(hour.values.size() == 360);
    CHECK(hour.secondsPerSlot == Approx(10.0));
}

TEST_CASE("Radar: the loudness band is the 10th to the 95th percentile of the gated values", "[audio][radar]") {
    CHECK_FALSE(edit::loudnessBand(std::vector<float>{}).valid);
    // 100 seconds from -30 to -20 evenly, a silence (below the absolute gate) and a dip 25 LU under the mean (below the
    // relative gate): neither counts.
    std::vector<float> series;
    for (int i = 0; i < 100; ++i) {
        series.push_back(-30.0F + 10.0F * static_cast<float>(i) / 99.0F);
    }
    series.push_back(-80.0F);
    series.push_back(-52.0F);
    const edit::LoudnessBand band = edit::loudnessBand(series);
    REQUIRE(band.valid);
    CHECK(band.lowLufs == Approx(-29.0).margin(0.2));
    CHECK(band.highLufs == Approx(-20.5).margin(0.2));
    CHECK(band.rangeLu() == Approx(8.5).margin(0.3));
}
