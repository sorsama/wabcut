// 0.7 (card 044): Dynamics Processing, the graph-based multiband dynamics (wabcut.audio.dynamicsProcessing). The DSP is
// measured directly (wabcut/audio/dsp/DynamicsProcessing.h: the static curves, the attack and release times, the meters)
// and through an EffectChain, the path preview and export take (flat bands, band solo and bypass, mono / stereo / 5.1 /
// 7.1, automation, latency, the CPU budget).

#include "DspTestUtil.h"

#include <wabcut/audio/dsp/DynamicsProcessing.h>
#include <wabcut/model/Parameters.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::audio;
using namespace wabcut::audio::test;
using Catch::Approx;
namespace dsp = wabcut::audio::dsp;

namespace {

constexpr int kRate = 48000;
const std::string kId = "wabcut.audio.dynamicsProcessing";

/// Four bands that do nothing (threshold 0 dB, ratio 1): the crossovers alone.
std::vector<EffectParam> neutral() {
    std::vector<EffectParam> p;
    for (int b = 1; b <= 4; ++b) {
        p.push_back({"band" + std::to_string(b) + "Threshold", ParamValue(0.0)});
        p.push_back({"band" + std::to_string(b) + "Ratio", ParamValue(1.0)});
    }
    return p;
}

void set(std::vector<EffectParam>& p, const std::string& key, double value) {
    for (EffectParam& e : p) {
        if (e.key == key) {
            e.value = ParamValue(value);
            return;
        }
    }
    p.push_back({key, ParamValue(value)});
}

/// One band (the others and the crossovers are out of the way).
std::vector<EffectParam> oneBand(dsp::DynamicsMode mode, double threshold, double ratio, double knee, double range = 40.0,
                                 double attack = 2.0, double release = 50.0, double makeup = 0.0) {
    std::vector<EffectParam> p;
    p.push_back({"bandCount", ParamValue(1.0)});
    p.push_back({"band1Mode", ParamValue(static_cast<double>(mode))});
    p.push_back({"band1Threshold", ParamValue(threshold)});
    p.push_back({"band1Ratio", ParamValue(ratio)});
    p.push_back({"band1Knee", ParamValue(knee)});
    p.push_back({"band1Range", ParamValue(range)});
    p.push_back({"band1Attack", ParamValue(attack)});
    p.push_back({"band1Release", ParamValue(release)});
    p.push_back({"band1Makeup", ParamValue(makeup)});
    return p;
}

dsp::DynamicsBand bandOf(dsp::DynamicsMode mode, double threshold, double ratio, double knee, double range = 40.0,
                         double attack = 2.0, double release = 50.0, double makeup = 0.0) {
    dsp::DynamicsBand b;
    b.mode = mode;
    b.thresholdDb = threshold;
    b.ratio = ratio;
    b.kneeDb = knee;
    b.rangeDb = range;
    b.attackMs = attack;
    b.releaseMs = release;
    b.makeupDb = makeup;
    return b;
}

/// The 1 kHz fundamental of the last half second of a 1.5 s run, dB.
double steadyOutputDb(const EffectInstance& e, double inputDb, int channels = 1) {
    const std::vector<float> x = sine(1000.0, dbToAmp(inputDb), 1.5, channels);
    const std::vector<float> y = runEffect(e, x, channels);
    return ampToDb(toneAmplitude(y, channels, 1000.0, 48000, 72000));
}

double dbAt(const std::vector<float>& x, int channels, double frequency, std::size_t first, std::size_t last, int channel = 0) {
    return ampToDb(toneAmplitude(x, channels, frequency, first, last, channel));
}

} // namespace

// ---- The crossovers ---------------------------------------------------------------------------------------------------------

TEST_CASE("Dynamics Processing: the four bands sum back flat when nothing acts", "[audio][dsp][dynamics][dynprocessing]") {
    const EffectInstance e = effectOf(kId, neutral());
    for (const double f : {40.0, 120.0, 400.0, 1200.0, 3000.0, 8000.0, 14000.0}) {
        INFO("frequency " << f);
        const std::vector<float> x = sine(f, 0.25, 0.7, 2);
        const std::vector<float> y = runEffect(e, x, 2);
        const double in = dbAt(x, 2, f, 14400, 33600);
        const double out = dbAt(y, 2, f, 14400, 33600);
        CHECK(out == Approx(in).margin(0.05));
    }
}

TEST_CASE("Dynamics Processing: fewer bands are flat too and a single band is the signal itself", "[audio][dsp][dynamics][dynprocessing]") {
    for (const double bands : {1.0, 2.0, 3.0}) {
        INFO("bands " << bands);
        std::vector<EffectParam> p = neutral();
        set(p, "bandCount", bands);
        const EffectInstance e = effectOf(kId, p);
        for (const double f : {60.0, 500.0, 5000.0}) {
            const std::vector<float> x = sine(f, 0.25, 0.7, 1);
            const std::vector<float> y = runEffect(e, x, 1);
            CHECK(dbAt(y, 1, f, 14400, 33600) == Approx(dbAt(x, 1, f, 14400, 33600)).margin(0.05));
        }
    }
    std::vector<EffectParam> p = neutral();
    set(p, "bandCount", 1.0);
    const std::vector<float> x = noise(0.4, 0.3, 2);
    const std::vector<float> y = runEffect(effectOf(kId, p), x, 2);
    double worst = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        worst = std::max(worst, static_cast<double>(std::fabs(x[i] - y[i])));
    }
    CHECK(worst < 1e-6); // one band: no filter in the path at all
}

// ---- The static curves --------------------------------------------------------------------------------------------------------

TEST_CASE("Dynamics Processing: the static curve of every mode is the reference transfer function", "[audio][dsp][dynamics][dynprocessing]") {
    using Mode = dsp::DynamicsMode;
    struct Row {
        Mode mode;
        double threshold, ratio, knee, range, makeup;
        std::vector<double> levels;
        double tolerance;
    };
    const std::vector<Row> rows{
        {Mode::Compress, -20.0, 4.0, 0.0, 40.0, 0.0, {-40.0, -26.0, -20.0, -10.0, -3.0}, 0.3},
        {Mode::Compress, -20.0, 4.0, 6.0, 40.0, 0.0, {-30.0, -22.0, -20.0, -18.0, -6.0}, 0.3},
        {Mode::Compress, -24.0, 3.0, 0.0, 6.0, 2.0, {-30.0, -12.0, -3.0}, 0.3}, // the Range holds the reduction to 6 dB
        {Mode::Limit, -18.0, 2.0, 0.0, 40.0, 0.0, {-30.0, -12.0, -3.0}, 0.3},
        {Mode::Expand, -30.0, 2.0, 0.0, 40.0, 0.0, {-10.0, -25.0, -34.0, -40.0}, 0.6},
        {Mode::Gate, -30.0, 2.0, 0.0, 24.0, 0.0, {-10.0, -45.0, -60.0}, 0.4},
    };
    for (const Row& r : rows) {
        dsp::DynamicsBand band = bandOf(r.mode, r.threshold, r.ratio, r.knee, r.range, 2.0, 50.0, r.makeup);
        const EffectInstance e = effectOf(kId, oneBand(r.mode, r.threshold, r.ratio, r.knee, r.range, 2.0, 50.0, r.makeup));
        for (const double level : r.levels) {
            INFO("mode " << static_cast<int>(r.mode) << " threshold " << r.threshold << " level " << level);
            const double expected = dsp::dynamicsCurveDb(level, band);
            CHECK(steadyOutputDb(e, level) == Approx(expected).margin(r.tolerance));
        }
    }
}

TEST_CASE("Dynamics Processing: the reference curve itself", "[audio][dsp][dynamics][dynprocessing]") {
    using Mode = dsp::DynamicsMode;
    // Compress: 4:1 above -20, hard knee.
    const dsp::DynamicsBand compress = bandOf(Mode::Compress, -20.0, 4.0, 0.0);
    CHECK(dsp::dynamicsCurveDb(-40.0, compress) == Approx(-40.0));
    CHECK(dsp::dynamicsCurveDb(-20.0, compress) == Approx(-20.0));
    CHECK(dsp::dynamicsCurveDb(0.0, compress) == Approx(-15.0));
    CHECK(dsp::dynamicsReductionDb(0.0, compress) == Approx(15.0));
    // The soft knee joins the two lines: nothing at its lower end, the full slope at its upper end, a quadratic between.
    dsp::DynamicsBand soft = compress;
    soft.kneeDb = 6.0;
    CHECK(dsp::dynamicsReductionDb(-23.0, soft) == Approx(0.0).margin(1e-12));
    CHECK(dsp::dynamicsReductionDb(-17.0, soft) == Approx(0.75 * 3.0).margin(1e-9));
    CHECK(dsp::dynamicsReductionDb(-20.0, soft) == Approx(0.75 * 3.0 * 3.0 / 12.0).margin(1e-9));
    // Limit never lets the level above the threshold; the Range caps what is taken off.
    const dsp::DynamicsBand limit = bandOf(Mode::Limit, -12.0, 1.0, 0.0);
    CHECK(dsp::dynamicsCurveDb(-3.0, limit) == Approx(-12.0));
    dsp::DynamicsBand capped = limit;
    capped.rangeDb = 4.0;
    CHECK(dsp::dynamicsCurveDb(-3.0, capped) == Approx(-7.0));
    // Expand: 2:1 under the threshold; Gate: the whole Range.
    const dsp::DynamicsBand expand = bandOf(Mode::Expand, -30.0, 2.0, 0.0);
    CHECK(dsp::dynamicsCurveDb(-20.0, expand) == Approx(-20.0));
    CHECK(dsp::dynamicsCurveDb(-40.0, expand) == Approx(-50.0));
    const dsp::DynamicsBand gate = bandOf(Mode::Gate, -30.0, 2.0, 0.0, 60.0);
    CHECK(dsp::dynamicsCurveDb(-20.0, gate) == Approx(-20.0));
    CHECK(dsp::dynamicsCurveDb(-45.0, gate) == Approx(-105.0));
    // Make-up gain adds, a bypassed band is the identity.
    dsp::DynamicsBand makeup = compress;
    makeup.makeupDb = 3.0;
    CHECK(dsp::dynamicsCurveDb(-40.0, makeup) == Approx(-37.0));
    makeup.bypass = true;
    CHECK(dsp::dynamicsCurveDb(0.0, makeup) == 0.0);
    // Monotonic: a louder input is never quieter at the output.
    double previous = dsp::dynamicsCurveDb(-80.0, soft);
    for (double x = -79.0; x <= 6.0; x += 1.0) {
        const double y = dsp::dynamicsCurveDb(x, soft);
        CHECK(y >= previous - 1e-9);
        previous = y;
    }
}

// ---- Attack and release ---------------------------------------------------------------------------------------------------------

namespace {

/// A constant-level square wave (one level on every sample: a steady detector input) through the DSP one frame at a time;
/// step() returns the band's reduction after the frame.
class Trajectory {
public:
    explicit Trajectory(dsp::DynamicsProcessing& processor) : m_processor(processor) {}
    dsp::DynamicsSettings settings;
    double step(double amplitude) {
        float frame = (m_position++ % 2 == 0 ? 1.0f : -1.0f) * static_cast<float>(amplitude);
        m_processor.process(&frame, 1, settings, {});
        return m_processor.bandReductionDb(0);
    }

private:
    dsp::DynamicsProcessing& m_processor;
    long m_position = 0;
};

} // namespace

TEST_CASE("Dynamics Processing: a compressor's attack and release are the time constants of the gain reduction", "[audio][dsp][dynamics][dynprocessing]") {
    dsp::DynamicsStructure st;
    st.bands = 1;
    dsp::DynamicsProcessing p(1, kRate, st);
    Trajectory t(p);
    t.settings.band[0] = bandOf(dsp::DynamicsMode::Compress, -30.0, 10.0, 0.0, 80.0, 50.0, 0.1);
    const double target = 0.9 * (-10.0 + 30.0); // 18 dB at -10 dBFS
    // Attack: 63.2 % of the reduction after one attack time.
    long rise = -1;
    for (long n = 0; n < kRate && rise < 0; ++n) {
        if (t.step(dbToAmp(-10.0)) >= 0.632 * target) {
            rise = n + 1;
        }
    }
    REQUIRE(rise > 0);
    CHECK(static_cast<double>(rise) * 1000.0 / kRate == Approx(50.0).epsilon(0.05));
    for (long n = 0; n < kRate; ++n) {
        t.step(dbToAmp(-10.0));
    }
    CHECK(p.bandReductionDb(0) == Approx(target).margin(0.05));

    // Release: back to 36.8 % after one release time (the attack is out of the way).
    dsp::DynamicsProcessing q(1, kRate, st);
    Trajectory u(q);
    u.settings.band[0] = bandOf(dsp::DynamicsMode::Compress, -30.0, 10.0, 0.0, 80.0, 0.1, 200.0);
    for (long n = 0; n < kRate; ++n) {
        u.step(dbToAmp(-10.0));
    }
    REQUIRE(q.bandReductionDb(0) == Approx(target).margin(0.05));
    long fall = -1;
    for (long n = 0; n < 4 * kRate && fall < 0; ++n) {
        if (u.step(dbToAmp(-70.0)) <= 0.368 * target) {
            fall = n + 1;
        }
    }
    REQUIRE(fall > 0);
    CHECK(static_cast<double>(fall) * 1000.0 / kRate == Approx(200.0).epsilon(0.05));
}

TEST_CASE("Dynamics Processing: a gate opens in its attack time and closes in its release time", "[audio][dsp][dynamics][dynprocessing]") {
    dsp::DynamicsStructure st;
    st.bands = 1;
    st.modes[0] = dsp::DynamicsMode::Gate;
    dsp::DynamicsProcessing p(1, kRate, st);
    Trajectory t(p);
    t.settings.band[0] = bandOf(dsp::DynamicsMode::Gate, -40.0, 2.0, 0.0, 40.0, 20.0, 80.0);
    // Closed under the threshold: the whole Range is taken off.
    for (long n = 0; n < 2 * kRate; ++n) {
        t.step(dbToAmp(-60.0));
    }
    REQUIRE(p.bandReductionDb(0) == Approx(40.0).margin(0.05));
    // Open: the reduction falls to 36.8 % in one attack time.
    long opened = -1;
    for (long n = 0; n < kRate && opened < 0; ++n) {
        if (t.step(dbToAmp(-10.0)) <= 0.368 * 40.0) {
            opened = n + 1;
        }
    }
    REQUIRE(opened > 0);
    CHECK(static_cast<double>(opened) * 1000.0 / kRate == Approx(20.0).epsilon(0.1));
    for (long n = 0; n < kRate; ++n) {
        t.step(dbToAmp(-10.0));
    }
    REQUIRE(p.bandReductionDb(0) == Approx(0.0).margin(0.05));
    // Closed again: 63.2 % of the range after one release time, which starts when the envelope (10 ms decay) has fallen from
    // -10 dBFS to the threshold: -10 dB - 0.0181 dB a frame = -40 dB after 34.5 ms.
    long closed = -1;
    for (long n = 0; n < 2 * kRate && closed < 0; ++n) {
        if (t.step(dbToAmp(-60.0)) >= 0.632 * 40.0) {
            closed = n + 1;
        }
    }
    REQUIRE(closed > 0);
    CHECK(static_cast<double>(closed) * 1000.0 / kRate == Approx(34.5 + 80.0).margin(3.0));
}

// ---- Metering -----------------------------------------------------------------------------------------------------------------

TEST_CASE("Dynamics Processing: the meters read the input and output peaks and each band's gain reduction", "[audio][dsp][dynamics][dynprocessing]") {
    dsp::DynamicsStructure st;
    st.bands = 2;
    {
        dsp::DynamicsProcessing p(2, kRate, st);
        auto meters = dsp::dynamicsMetersFor(424242);
        p.attachMeters(meters);
        CHECK(dsp::findDynamicsMeters(424242) == meters);
        dsp::DynamicsSettings s;
        s.crossoverHz[0] = 1000.0;
        s.band[0] = bandOf(dsp::DynamicsMode::Compress, -20.0, 4.0, 0.0, 40.0, 1.0, 50.0);
        s.band[1] = bandOf(dsp::DynamicsMode::Compress, 0.0, 1.0, 0.0); // untouched
        // 300 Hz at -6 dBFS lives in band 1: 14 dB over the threshold = 10.5 dB of reduction, band 2 stays at 0.
        std::vector<float> x = sine(300.0, dbToAmp(-6.0), 1.0, 2);
        for (std::size_t done = 0; done < x.size() / 2; done += 480) {
            p.process(x.data() + done * 2, 480, s, {});
        }
        const dsp::DynamicsMeters::Snapshot snap = meters->take();
        CHECK(snap.blocks > 0u);
        CHECK(snap.inputPeak == Approx(dbToAmp(-6.0)).epsilon(0.02));
        CHECK(snap.reductionDb[0] == Approx(10.5).margin(0.7));
        CHECK(snap.reductionDb[1] < 0.2f);
        CHECK(snap.outputPeak < snap.inputPeak);
        CHECK(p.bandReductionDb(0) == Approx(10.5).margin(0.7));
        // Taking clears the peaks; the counter stays.
        const dsp::DynamicsMeters::Snapshot again = meters->take();
        CHECK(again.inputPeak == 0.0f);
        CHECK(again.reductionDb[0] == 0.0f);
        CHECK(again.blocks == snap.blocks);
    }
    CHECK(dsp::findDynamicsMeters(424242) == nullptr); // nobody holds them any more
}

TEST_CASE("Dynamics Processing: a bound effect chain files its meters under the instance", "[audio][dsp][dynamics][dynprocessing]") {
    EffectInstance e = effectOf(kId, oneBand(dsp::DynamicsMode::Compress, -20.0, 4.0, 0.0));
    e.id = EffectInstanceId{98765};
    const EffectStack stack{e};
    EffectChain chain(2, kRate);
    chain.bind(stack);
    auto meters = dsp::findDynamicsMeters(98765);
    REQUIRE(meters != nullptr);
    std::vector<float> x = sine(1000.0, dbToAmp(-6.0), 0.5, 2);
    const CurveClock clock{Ticks{}, Rate{kRate, 1}.ticksPerUnit(), nullptr};
    for (std::size_t done = 0; done + 512 <= x.size() / 2; done += 512) {
        chain.process(x.data() + done * 2, 512, static_cast<std::int64_t>(done), clock);
    }
    const dsp::DynamicsMeters::Snapshot snap = meters->take();
    CHECK(snap.blocks > 0u);
    CHECK(snap.reductionDb[0] > 5.0f);
}

// ---- Band solo and bypass --------------------------------------------------------------------------------------------------------

TEST_CASE("Dynamics Processing: soloing a band plays only its part of the spectrum", "[audio][dsp][dynamics][dynprocessing]") {
    // 60 Hz in band 1 (under 120), 400 Hz in band 2 (120..1200), 3 kHz in band 3, 18 kHz in band 4.
    std::vector<float> x = sine(60.0, 0.1, 1.0, 1);
    for (const double f : {400.0, 3000.0, 18000.0}) {
        const std::vector<float> t = sine(f, 0.1, 1.0, 1);
        for (std::size_t i = 0; i < x.size(); ++i) {
            x[i] += t[i];
        }
    }
    const double reference = dbAt(x, 1, 400.0, 24000, 48000);
    for (int soloed = 1; soloed <= 4; ++soloed) {
        INFO("band " << soloed);
        std::vector<EffectParam> p = neutral();
        set(p, "band" + std::to_string(soloed) + "Solo", 1.0);
        const std::vector<float> y = runEffect(effectOf(kId, p), x, 1);
        constexpr std::array<double, 4> kTone{60.0, 400.0, 3000.0, 18000.0};
        for (int k = 0; k < 4; ++k) {
            const double level = dbAt(y, 1, kTone[static_cast<std::size_t>(k)], 24000, 48000);
            INFO("tone " << kTone[static_cast<std::size_t>(k)]);
            if (k + 1 == soloed) {
                // All of it, less what the neighbouring crossovers' skirts take: a Linkwitz-Riley 24 dB/octave filter is
                // 1 / (1 + (f / fc)^4) in magnitude, 0.53 dB down one octave from its corner (60 Hz under 120 Hz).
                CHECK(level == Approx(reference).margin(0.7));
            } else {
                CHECK(level < reference - 20.0); // the crossover slopes are 24 dB per octave
            }
        }
    }
    // Two soloed bands are both heard.
    std::vector<EffectParam> p = neutral();
    set(p, "band1Solo", 1.0);
    set(p, "band2Solo", 1.0);
    const std::vector<float> y = runEffect(effectOf(kId, p), x, 1);
    CHECK(dbAt(y, 1, 60.0, 24000, 48000) == Approx(reference).margin(0.7));
    CHECK(dbAt(y, 1, 400.0, 24000, 48000) == Approx(reference).margin(0.7));
    CHECK(dbAt(y, 1, 3000.0, 24000, 48000) < reference - 20.0);
}

TEST_CASE("Dynamics Processing: a bypassed band is left alone by the dynamics", "[audio][dsp][dynamics][dynprocessing]") {
    std::vector<EffectParam> p;
    for (int b = 1; b <= 4; ++b) {
        set(p, "band" + std::to_string(b) + "Threshold", -60.0);
        set(p, "band" + std::to_string(b) + "Ratio", 30.0);
        set(p, "band" + std::to_string(b) + "Makeup", 6.0);
    }
    std::vector<float> x = sine(60.0, 0.3, 1.0, 1);
    const std::vector<float> mid = sine(400.0, 0.3, 1.0, 1);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] += mid[i];
    }
    const std::vector<float> squeezed = runEffect(effectOf(kId, p), x, 1);
    CHECK(dbAt(squeezed, 1, 400.0, 24000, 48000) < dbAt(x, 1, 400.0, 24000, 48000) - 10.0);
    set(p, "band2Bypass", 1.0);
    const std::vector<float> bypassed = runEffect(effectOf(kId, p), x, 1);
    CHECK(dbAt(bypassed, 1, 400.0, 24000, 48000) == Approx(dbAt(x, 1, 400.0, 24000, 48000)).margin(0.3)); // no make-up either
    CHECK(dbAt(bypassed, 1, 60.0, 24000, 48000) < dbAt(x, 1, 60.0, 24000, 48000) - 10.0);                 // band 1 still acts
}

// ---- Channel layouts ------------------------------------------------------------------------------------------------------------

TEST_CASE("Dynamics Processing: one gain for every channel whatever the layout", "[audio][dsp][dynamics][dynprocessing]") {
    const EffectInstance e = effectOf(kId, oneBand(dsp::DynamicsMode::Compress, -20.0, 4.0, 0.0));
    const double mono = steadyOutputDb(e, -6.0, 1);
    for (const int channels : {1, 2, 6, 8}) {
        INFO("channels " << channels);
        const std::vector<float> x = sine(1000.0, dbToAmp(-6.0), 1.5, channels);
        const std::vector<float> y = runEffect(e, x, channels);
        for (int c = 0; c < channels; ++c) {
            CHECK(dbAt(y, channels, 1000.0, 48000, 72000, c) == Approx(mono).margin(0.01));
        }
    }
    // The envelope is linked: a loud left channel takes the quiet ones down by the same amount.
    for (const int channels : {2, 6, 8}) {
        INFO("channels " << channels);
        std::vector<float> x = sine(1000.0, dbToAmp(-40.0), 1.5, channels);
        const std::vector<float> loud = sine(1000.0, dbToAmp(-6.0), 1.5, 1);
        for (std::size_t i = 0; i < loud.size(); ++i) {
            x[i * static_cast<std::size_t>(channels)] = loud[i];
        }
        const std::vector<float> y = runEffect(e, x, channels);
        CHECK(dbAt(y, channels, 1000.0, 48000, 72000, 0) == Approx(mono).margin(0.01));
        const int quiet = channels == 2 ? 1 : 2; // not the LFE
        CHECK(dbAt(y, channels, 1000.0, 48000, 72000, quiet) == Approx(-40.0 + (mono + 6.0)).margin(0.05));
    }
}

TEST_CASE("Dynamics Processing: the LFE of 5.1 and 7.1 follows the gain but is not listened to", "[audio][dsp][dynamics][dynprocessing]") {
    const EffectInstance e = effectOf(kId, oneBand(dsp::DynamicsMode::Compress, -20.0, 4.0, 0.0));
    for (const int channels : {6, 8}) {
        INFO("channels " << channels);
        // A loud LFE alone does not trigger the compressor: the other channels (quiet, under the threshold) are untouched.
        std::vector<float> x(static_cast<std::size_t>(1.0 * kRate) * static_cast<std::size_t>(channels), 0.0f);
        const std::vector<float> tone = sine(1000.0, dbToAmp(-30.0), 1.0, 1);
        const std::vector<float> lfe = sine(1000.0, dbToAmp(-3.0), 1.0, 1);
        for (std::size_t i = 0; i < tone.size(); ++i) {
            x[i * static_cast<std::size_t>(channels)] = tone[i];
            x[i * static_cast<std::size_t>(channels) + 3] = lfe[i];
        }
        const std::vector<float> y = runEffect(e, x, channels);
        CHECK(dbAt(y, channels, 1000.0, 24000, 48000, 0) == Approx(-30.0).margin(0.02));
        CHECK(dbAt(y, channels, 1000.0, 24000, 48000, 3) == Approx(-3.0).margin(0.02));
        // A loud main channel takes the LFE down with it.
        std::vector<float> z = x;
        const std::vector<float> loud = sine(1000.0, dbToAmp(-6.0), 1.0, 1);
        for (std::size_t i = 0; i < loud.size(); ++i) {
            z[i * static_cast<std::size_t>(channels)] = loud[i];
        }
        const std::vector<float> w = runEffect(e, z, channels);
        const double reduction = 14.0 * 0.75; // 14 dB over the threshold at 4:1
        CHECK(dbAt(w, channels, 1000.0, 24000, 48000, 3) == Approx(-3.0 - reduction).margin(0.7));
    }
}

TEST_CASE("Dynamics Processing: silence stays silent and loud noise stays finite on every layout", "[audio][dsp][dynamics][dynprocessing]") {
    for (const int channels : {1, 2, 6, 8}) {
        INFO("channels " << channels);
        EffectInstance e = effectOf(kId, [] {
            std::vector<EffectParam> p;
            set(p, "limiterEnable", 1.0);
            set(p, "autoGate", 1.0);
            set(p, "band2Mode", 2.0);
            set(p, "band3Mode", 1.0);
            set(p, "band4Mode", 3.0);
            return p;
        }());
        const std::vector<float> quiet = runEffect(e, std::vector<float>(48000 * static_cast<std::size_t>(channels), 0.0f), channels);
        CHECK(peak(quiet, channels) < 1e-6);
        const std::vector<float> loud = runEffect(e, noise(1.0, 1.0, channels), channels);
        bool finite = true;
        for (const float v : loud) {
            finite = finite && std::isfinite(v);
        }
        CHECK(finite);
        CHECK(peak(loud, channels) < 4.0);
    }
}

// ---- Automation, master section, latency ------------------------------------------------------------------------------------------

TEST_CASE("Dynamics Processing: every control follows its keyframes", "[audio][dsp][dynamics][dynprocessing]") {
    // The threshold falls from 0 dB to -40 dB over the first second; the tone stays at -10 dBFS.
    Curve ramp;
    ramp.keys.push_back(Keyframe{Ticks{0}, 0.0});
    ramp.keys.push_back(Keyframe{Ticks{kTicksPerSecond}, -40.0});
    std::vector<EffectParam> p = oneBand(dsp::DynamicsMode::Compress, -20.0, 4.0, 0.0);
    set(p, "band1Threshold", 0.0);
    for (EffectParam& e : p) {
        if (e.key == "band1Threshold") {
            e.value = ParamValue(ramp);
        }
    }
    const std::vector<float> x = sine(1000.0, dbToAmp(-10.0), 1.3, 1);
    const std::vector<float> y = runEffect(effectOf(kId, p), x, 1);
    CHECK(dbAt(y, 1, 1000.0, 4800, 9600) == Approx(-10.0).margin(0.4)); // 0.1..0.2 s: threshold ~ -6 dB, nothing yet
    const auto at = [&](double t0) {
        const auto first = static_cast<std::size_t>(t0 * kRate);
        return dbAt(y, 1, 1000.0, first, first + 4800);
    };
    // Around 0.5 s the threshold is -20 dB: -20 + 10/4 = -17.5 dB (the window spans 4 dB of threshold either way).
    CHECK(at(0.45) == Approx(-17.5).margin(1.0));
    // After the ramp it stays at -40 dB: -40 + 30/4 = -32.5 dB.
    CHECK(at(1.15) == Approx(-32.5).margin(0.5));
    // The output gain and the mix are keyframable too: the output gain ramps from 0 to -12 dB.
    Curve out;
    out.keys.push_back(Keyframe{Ticks{0}, 0.0});
    out.keys.push_back(Keyframe{Ticks{kTicksPerSecond}, -12.0});
    std::vector<EffectParam> q = neutral();
    set(q, "bandCount", 1.0);
    q.push_back({"outputGain", ParamValue(out)});
    const std::vector<float> z = runEffect(effectOf(kId, q), sine(1000.0, 0.5, 1.2, 1), 1);
    CHECK(dbAt(z, 1, 1000.0, 2400, 7200) == Approx(ampToDb(0.5) - 1.2).margin(0.2));  // 0.05..0.15 s: about -1.2 dB
    CHECK(dbAt(z, 1, 1000.0, 52800, 57600) == Approx(ampToDb(0.5) - 12.0).margin(0.1)); // 1.1..1.2 s
}

TEST_CASE("Dynamics Processing: input gain and output gain and mix", "[audio][dsp][dynamics][dynprocessing]") {
    std::vector<EffectParam> p = neutral();
    set(p, "bandCount", 1.0);
    set(p, "inputGain", 6.0);
    set(p, "outputGain", -2.0);
    const std::vector<float> x = sine(1000.0, 0.1, 1.0, 2);
    CHECK(dbAt(runEffect(effectOf(kId, p), x, 2), 2, 1000.0, 24000, 48000) == Approx(dbAt(x, 2, 1000.0, 24000, 48000) + 4.0).margin(0.02));
    // Mix 0 is the dry signal untouched, 50 % the mean of dry and wet.
    set(p, "mix", 0.0);
    const std::vector<float> dry = runEffect(effectOf(kId, p), x, 2);
    double worst = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        worst = std::max(worst, static_cast<double>(std::fabs(x[i] - dry[i])));
    }
    CHECK(worst < 1e-6);
    set(p, "mix", 50.0);
    const double half = dbAt(runEffect(effectOf(kId, p), x, 2), 2, 1000.0, 24000, 48000);
    CHECK(half == Approx(ampToDb(0.1 * (1.0 + std::pow(10.0, 4.0 / 20.0)) / 2.0)).margin(0.02));
}

TEST_CASE("Dynamics Processing: the output limiter holds the ceiling and its latency is reported and matched by the dry mix", "[audio][dsp][dynamics][dynprocessing]") {
    std::vector<EffectParam> p = neutral();
    set(p, "bandCount", 1.0);
    set(p, "limiterEnable", 1.0);
    set(p, "limiterCeiling", -6.0);
    set(p, "inputGain", 12.0);
    const EffectInstance e = effectOf(kId, p);
    const int latency = effectLatency(e, 2);
    CHECK(latency >= kRate * 5 / 1000);
    CHECK(latency < kRate / 50);
    const std::vector<float> y = runEffect(e, noise(0.8, 1.0, 2), 2);
    CHECK(peak(y, 2) <= dbToAmp(-6.0) + 1e-4);
    // Without the limiter there is no latency.
    set(p, "limiterEnable", 0.0);
    CHECK(effectLatency(effectOf(kId, p), 2) == 0);
    // Mix 0 with the limiter on is the input delayed by the latency (the dry signal waits for the wet one).
    set(p, "limiterEnable", 1.0);
    set(p, "mix", 0.0);
    set(p, "inputGain", 0.0);
    const EffectInstance dryEffect = effectOf(kId, p);
    const int l = effectLatency(dryEffect, 2);
    REQUIRE(l == latency);
    const std::vector<float> x = noise(0.5, 0.5, 2);
    const std::vector<float> d = runEffect(dryEffect, x, 2);
    double worst = 0.0;
    for (std::size_t i = static_cast<std::size_t>(l) * 2; i < d.size(); ++i) {
        worst = std::max(worst, static_cast<double>(std::fabs(d[i] - x[i - static_cast<std::size_t>(l) * 2])));
    }
    CHECK(worst < 1e-6);
}

TEST_CASE("Dynamics Processing: the Auto Gate closes under its threshold and opens on speech", "[audio][dsp][dynamics][dynprocessing]") {
    std::vector<EffectParam> p = neutral();
    set(p, "bandCount", 1.0);
    set(p, "autoGate", 1.0);
    set(p, "autoGateThreshold", -40.0);
    set(p, "autoGateRelease", 50.0);
    std::vector<float> x = sine(1000.0, dbToAmp(-60.0), 1.0, 1);
    const std::vector<float> speech = sine(1000.0, dbToAmp(-20.0), 0.5, 1);
    x.insert(x.end(), speech.begin(), speech.end());
    const std::vector<float> y = runEffect(effectOf(kId, p), x, 1);
    CHECK(peak(y, 1, 24000, 48000) < dbToAmp(-60.0) * 0.01); // the noise floor before the burst is gone
    CHECK(dbAt(y, 1, 1000.0, 52800, 67200) == Approx(-20.0).margin(0.05)); // the burst is whole
}

// ---- Speed ------------------------------------------------------------------------------------------------------------------------

TEST_CASE("Dynamics Processing: four bands with every stage on cost a small share of one core on a 48 kHz stereo track", "[audio][dsp][dynamics][dynprocessing][perf]") {
    std::vector<EffectParam> p;
    set(p, "limiterEnable", 1.0);
    set(p, "autoGate", 1.0);
    set(p, "band2Mode", 1.0);
    set(p, "band3Mode", 2.0);
    set(p, "band4Mode", 3.0);
    const EffectInstance e = effectOf(kId, p);
    std::vector<float> x = noise(0.4, 20.0, 2);
    const auto begin = std::chrono::steady_clock::now();
    const std::vector<float> y = runEffect(e, std::move(x), 2);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    CHECK(y.size() == static_cast<std::size_t>(20 * kRate) * 2);
    INFO("20 s of audio took " << seconds << " s");
    CHECK(seconds < 2.0); // under 10 % of real time (a typical run is about 2 %)
}
