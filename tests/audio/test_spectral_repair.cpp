// 0.7 (card 047): Spectral Repair. The edit list's text (round trip, packing into the effect's data, malformed lines), the DSP on
// synthetic sound (Attenuate by the asked dB, Heal and Fill with Noise Floor of a squeak, a cough and a click, Remove Tone of a
// steady, a harmonic and a moving tone, the listen modes, 96 kHz and 5.1), the effect through an EffectChain (preview and export
// agree to the last bit however the sound is sliced and wherever the render starts; the clip's time map moves the edit with
// the sound) and the tiled spectrogram behind the view (a tone at its bin, pooling that keeps a click, cancel, a memory budget
// on long sound).

#include "DspTestUtil.h"

#include <wabcut/audio/dsp/Fft.h>
#include <wabcut/audio/dsp/SpectralRepair.h>
#include <wabcut/audio/dsp/Spectrogram.h>
#include <wabcut/model/Clip.h>
#include <wabcut/model/SpectralRepair.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <memory>
#include <numbers>
#include <stop_token>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::audio;
using namespace wabcut::audio::test;
using Catch::Approx;
namespace dsp = wabcut::audio::dsp;

namespace {

constexpr int kRate = 48000;

std::size_t at(double seconds, int rate = kRate) { return static_cast<std::size_t>(std::llround(seconds * rate)); }

/// A mono signal: `x = a + b`, sample by sample.
std::vector<float> plus(std::vector<float> a, const std::vector<float>& b) {
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
        a[i] += b[i];
    }
    return a;
}

/// A tone that fades in and out over 10 ms, on for [from, to] seconds, in `seconds` of silence.
std::vector<float> burst(double frequency, double amplitude, double from, double to, double seconds, int rate = kRate) {
    std::vector<float> out(at(seconds, rate), 0.0f);
    for (std::size_t i = at(from, rate); i < std::min(out.size(), at(to, rate)); ++i) {
        const double t = static_cast<double>(i) / rate;
        const double ramp = std::min({1.0, (t - from) / 0.01, (to - t) / 0.01});
        out[i] = static_cast<float>(amplitude * ramp * std::sin(2.0 * std::numbers::pi * frequency * t));
    }
    return out;
}

/// Noise (as DspTestUtil's) that is on for [from, to] seconds only.
std::vector<float> noiseBurst(double amplitude, double from, double to, double seconds, std::uint32_t seed, int rate = kRate) {
    std::vector<float> n = noise(amplitude, seconds, 1, rate, seed);
    for (std::size_t i = 0; i < n.size(); ++i) {
        const double t = static_cast<double>(i) / rate;
        const double ramp = std::clamp(std::min((t - from) / 0.005, (to - t) / 0.005), 0.0, 1.0);
        n[i] = static_cast<float>(n[i] * ramp);
    }
    return n;
}

/// The mean power in [lo, hi] Hz over [from, to] seconds (Welch, Hann, 4096 points), dB.
double bandDb(const std::vector<float>& x, double from, double to, double lo, double hi, int rate = kRate) {
    constexpr int kSize = 4096;
    dsp::Fft fft(kSize);
    std::vector<double> window(kSize);
    std::vector<double> frame(kSize);
    std::vector<std::complex<double>> bins(static_cast<std::size_t>(fft.bins()));
    for (int i = 0; i < kSize; ++i) {
        window[static_cast<std::size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / kSize);
    }
    const double binHz = static_cast<double>(rate) / kSize;
    double sum = 0.0;
    std::size_t count = 0;
    for (std::size_t s = at(from, rate); s + kSize <= std::min(x.size(), at(to, rate)); s += kSize / 2) {
        for (int i = 0; i < kSize; ++i) {
            frame[static_cast<std::size_t>(i)] = x[s + static_cast<std::size_t>(i)] * window[static_cast<std::size_t>(i)];
        }
        fft.forwardReal(frame.data(), bins.data());
        for (int b = static_cast<int>(lo / binHz); b <= static_cast<int>(hi / binHz); ++b) {
            sum += std::norm(bins[static_cast<std::size_t>(b)]);
            ++count;
        }
    }
    return 10.0 * std::log10(sum / static_cast<double>(std::max<std::size_t>(count, 1)) + 1.0e-30);
}

double toneDb(const std::vector<float>& x, double frequency, double from, double to, int channels = 1, int channel = 0, int rate = kRate) {
    return ampToDb(toneAmplitude(x, channels, frequency, at(from, rate), at(to, rate), channel, rate));
}

SpectralEdit rect(SpectralKind kind, double start, double end, double low, double high, double gainDb = -24.0) {
    SpectralEdit e;
    e.kind = kind;
    e.shape = SpectralShape::Rectangle;
    e.start = start;
    e.end = end;
    e.low = low;
    e.high = high;
    e.gainDb = gainDb;
    return e;
}

SpectralEdit tone(double hz, double start, double end, int harmonics = 1, double widthHz = 12.0, double gainDb = -60.0) {
    SpectralEdit e;
    e.kind = SpectralKind::Tone;
    e.path = {{start, hz}};
    e.start = start;
    e.end = end;
    e.harmonics = harmonics;
    e.widthHz = widthHz;
    e.gainDb = gainDb;
    return e;
}

std::vector<float> repair(const std::vector<float>& x, std::vector<SpectralEdit> edits, int channels = 1, int rate = kRate,
                          dsp::SpectralListen listen = dsp::SpectralListen::Off, int fftSize = 2048) {
    return dsp::renderSpectralRepair(x, channels, rate, edits, {fftSize, listen}, dsp::IdentityTimeMap(rate));
}

bool sameSamples(const std::vector<float>& a, const std::vector<float>& b, double from, double to, int rate = kRate) {
    for (std::size_t i = at(from, rate); i < std::min({a.size(), b.size(), at(to, rate)}); ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

} // namespace

// ---- The edit list as text ------------------------------------------------------------------------------------------------

TEST_CASE("A spectral edit survives its text form and malformed text is refused", "[audio][spectral][model]") {
    SpectralEdit lasso;
    lasso.kind = SpectralKind::Heal;
    lasso.shape = SpectralShape::Lasso;
    lasso.logFrequency = false;
    lasso.amount = 80.0;
    lasso.path = {{1.25, 400.0}, {2.5, 400.0}, {2.0, 3200.5}, {1.1, 2800.0}};
    SpectralEdit brush;
    brush.kind = SpectralKind::NoiseFloor;
    brush.shape = SpectralShape::Brush;
    brush.path = {{3.0, 1000.0}, {3.2, 1500.0}};
    brush.radiusSeconds = 0.08;
    brush.radiusOctaves = 0.4;
    for (const SpectralEdit& e : {rect(SpectralKind::Attenuate, 1.5, 2.25, 700.0, 3200.0, -18.5), lasso, brush, tone(50.0, 0.0, 12.5, 4, 8.0, -48.0)}) {
        INFO(describeSpectralEdit(e));
        CHECK_FALSE(spectralEditProblem(e).has_value());
        const std::string text = encodeSpectralEdit(e);
        CHECK(text.size() <= kMaxEffectText);
        const auto back = decodeSpectralEdit(text);
        REQUIRE(back.has_value());
        CHECK(*back == e);
    }
    for (const char* bad : {"", "zz", "arg", "arg -24 100 1 2 3", "xrg -24 100 1 2 700 3200 0.05 0.25 1 12 0", "arg -24 100 2 1 700 3200 0.05 0.25 1 12 0",
                            "arg -24 100 1 2 700 3200 0.05 0.25 1 12 2 1:1000", "arg nan 100 1 2 700 3200 0.05 0.25 1 12 0",
                            "arg -24 100 1 2 700 3200 0.05 0.25 1 12 1 1.0-1000"}) {
        INFO(bad);
        CHECK_FALSE(decodeSpectralEdit(bad).has_value());
    }
    CHECK(spectralEditProblem(rect(SpectralKind::Attenuate, 2.0, 1.0, 100.0, 200.0)).has_value());
    CHECK(spectralEditProblem(rect(SpectralKind::Attenuate, 1.0, 2.0, 100.0, 200.0, 5.0)).has_value()); // no boost
    SpectralEdit shortLasso = lasso;
    shortLasso.path.resize(2);
    CHECK(spectralEditProblem(shortLasso).has_value());
    SpectralEdit noTrace = tone(50.0, 0.0, 1.0);
    noTrace.path.clear();
    CHECK(spectralEditProblem(noTrace).has_value());
    CHECK_FALSE(spectralEditProblem(clampedSpectralEdit(rect(SpectralKind::Heal, 5.0, 1.0, 20000.0, 10.0, 7.0))).has_value());
}

TEST_CASE("Spectral edits pack into effect data entries within the bounds and come back in order", "[audio][spectral][model]") {
    std::vector<SpectralEdit> edits;
    for (int i = 0; i < 150; ++i) {
        SpectralEdit e = rect(i % 2 == 0 ? SpectralKind::Attenuate : SpectralKind::Heal, i * 0.5, i * 0.5 + 0.25, 300.0 + i, 3000.0 + i, -6.0 - i % 40);
        if (i % 5 == 0) {
            e.shape = SpectralShape::Lasso;
            e.path = {{i * 0.5, 300.0}, {i * 0.5 + 0.2, 400.0}, {i * 0.5 + 0.1, 3000.0}};
        }
        edits.push_back(e);
    }
    SpectralEdit longPath;
    longPath.shape = SpectralShape::Brush;
    for (int i = 0; i < 48; ++i) {
        longPath.path.push_back({1.0 + i * 0.01234567, 500.0 + 12.3456789 * i});
    }
    edits.push_back(longPath);
    const std::vector<EffectData> data = packSpectralEdits(edits);
    REQUIRE_FALSE(data.empty());
    CHECK(data.size() <= kMaxEffectDataEntries);
    for (std::size_t i = 0; i < data.size(); ++i) {
        INFO(data[i].key);
        CHECK(isSpectralEditKey(data[i].key));
        CHECK_FALSE(effectDataProblem(data[i]).has_value());
        if (i > 0) {
            CHECK(data[i - 1].key < data[i].key);
        }
    }
    EffectInstance instance;
    instance.effect = std::string(kSpectralRepairEffect);
    instance.data = data;
    const std::vector<SpectralEdit> back = unpackSpectralEdits(instance);
    REQUIRE(back.size() == edits.size());
    for (std::size_t i = 0; i < edits.size(); ++i) {
        INFO(i);
        CHECK(back[i].kind == edits[i].kind);
        CHECK(back[i].shape == edits[i].shape);
        CHECK(back[i].start == Approx(edits[i].start).margin(1e-5));
        CHECK(back[i].path.size() == edits[i].path.size());
    }
    // The stack with the effect is a valid stack, and an unknown data key beside the edits does not disturb them.
    instance.id = EffectInstanceId{1};
    CHECK_FALSE(effectStackProblem({instance}, EffectKind::Audio, 100).has_value());
    instance.setData(EffectData{"zzNote", std::string("kept")});
    CHECK(unpackSpectralEdits(instance).size() == edits.size());
}

// ---- Attenuate ----------------------------------------------------------------------------------------------------------

TEST_CASE("Attenuate lowers the region by the asked dB and leaves the rest of the sound alone", "[audio][spectral][dsp]") {
    const std::vector<float> x = plus(plus(sine(1000.0, 0.2, 4.0, 1), sine(5000.0, 0.2, 4.0, 1)), noise(0.01, 4.0, 1));
    for (const double gain : {-12.0, -24.0, -40.0}) {
        INFO("gain " << gain);
        const std::vector<float> y = repair(x, {rect(SpectralKind::Attenuate, 1.0, 2.5, 800.0, 1250.0, gain)});
        REQUIRE(y.size() == x.size());
        CHECK(toneDb(y, 1000.0, 1.4, 2.2) - toneDb(x, 1000.0, 1.4, 2.2) == Approx(gain).margin(0.7));
        CHECK(toneDb(y, 5000.0, 1.4, 2.2) - toneDb(x, 5000.0, 1.4, 2.2) == Approx(0.0).margin(0.1));
        CHECK(toneDb(y, 1000.0, 3.0, 3.8) - toneDb(x, 1000.0, 3.0, 3.8) == Approx(0.0).margin(0.05));
        CHECK(sameSamples(x, y, 0.0, 0.9));
        CHECK(sameSamples(x, y, 2.65, 4.0));
    }
}

TEST_CASE("Attenuate with a lasso and a brush covers its shape", "[audio][spectral][dsp]") {
    const std::vector<float> x = plus(plus(sine(1000.0, 0.2, 3.0, 1), sine(4000.0, 0.2, 3.0, 1)), noise(0.005, 3.0, 1));
    SpectralEdit lasso;
    lasso.kind = SpectralKind::Attenuate;
    lasso.shape = SpectralShape::Lasso;
    lasso.gainDb = -30.0;
    lasso.path = {{1.0, 700.0}, {2.0, 700.0}, {2.0, 1500.0}, {1.0, 1500.0}};
    const std::vector<float> ly = repair(x, {lasso});
    CHECK(toneDb(ly, 1000.0, 1.3, 1.7) - toneDb(x, 1000.0, 1.3, 1.7) == Approx(-30.0).margin(1.0));
    CHECK(toneDb(ly, 4000.0, 1.3, 1.7) - toneDb(x, 4000.0, 1.3, 1.7) == Approx(0.0).margin(0.1));
    CHECK(toneDb(ly, 1000.0, 2.3, 2.8) - toneDb(x, 1000.0, 2.3, 2.8) == Approx(0.0).margin(0.1));

    SpectralEdit brush;
    brush.kind = SpectralKind::Attenuate;
    brush.shape = SpectralShape::Brush;
    brush.gainDb = -30.0;
    brush.path = {{1.0, 4000.0}, {2.0, 4000.0}};
    brush.radiusSeconds = 0.1;
    brush.radiusOctaves = 0.2;
    const std::vector<float> by = repair(x, {brush});
    CHECK(toneDb(by, 4000.0, 1.3, 1.7) - toneDb(x, 4000.0, 1.3, 1.7) == Approx(-30.0).margin(1.0));
    CHECK(toneDb(by, 1000.0, 1.3, 1.7) - toneDb(x, 1000.0, 1.3, 1.7) == Approx(0.0).margin(0.1));
    CHECK(toneDb(by, 4000.0, 2.3, 2.8) - toneDb(x, 4000.0, 2.3, 2.8) == Approx(0.0).margin(0.1));
}

// ---- Heal, Fill with Noise Floor -------------------------------------------------------------------------------------------

TEST_CASE("Heal fills a squeak with what surrounds it and leaves everything else", "[audio][spectral][dsp][heal]") {
    const std::vector<float> bed = plus(noise(0.03, 3.0, 1), sine(800.0, 0.1, 3.0, 1));
    const std::vector<float> x = plus(bed, burst(3000.0, 0.2, 1.0, 1.5, 3.0));
    const std::vector<float> y = repair(x, {rect(SpectralKind::Heal, 0.95, 1.55, 2600.0, 3400.0)});
    REQUIRE(y.size() == x.size());
    // The squeak is gone ...
    CHECK(toneDb(y, 3000.0, 1.1, 1.4) < toneDb(x, 3000.0, 1.1, 1.4) - 25.0);
    // ... the band is as loud as the noise around it ...
    const double healed = bandDb(y, 1.05, 1.45, 2700.0, 3300.0);
    const double around = bandDb(y, 2.0, 2.8, 2700.0, 3300.0);
    INFO("healed " << healed << " dB, around " << around << " dB");
    CHECK(std::fabs(healed - around) < 3.0);
    // ... and nothing else moved: the other tone, the other bands, the sound outside.
    CHECK(toneDb(y, 800.0, 1.1, 1.4) - toneDb(x, 800.0, 1.1, 1.4) == Approx(0.0).margin(0.2));
    CHECK(std::fabs(bandDb(y, 1.05, 1.45, 5000.0, 9000.0) - bandDb(x, 1.05, 1.45, 5000.0, 9000.0)) < 0.2);
    CHECK(sameSamples(x, y, 0.0, 0.9));
    CHECK(sameSamples(x, y, 1.65, 3.0));
}

TEST_CASE("Heal and Fill with Noise Floor take a cough out of a steady bed", "[audio][spectral][dsp][heal]") {
    const std::vector<float> bed = plus(noise(0.02, 4.0, 1, kRate, 99), sine(300.0, 0.05, 4.0, 1));
    const std::vector<float> x = plus(bed, noiseBurst(0.4, 1.0, 1.4, 4.0, 4321));
    const double bedLevel = bandDb(x, 2.0, 3.0, 1000.0, 8000.0);
    CHECK(bandDb(x, 1.05, 1.35, 1000.0, 8000.0) > bedLevel + 15.0); // the cough is there
    for (const SpectralKind kind : {SpectralKind::Heal, SpectralKind::NoiseFloor}) {
        INFO(toString(kind));
        const std::vector<float> y = repair(x, {rect(kind, 0.95, 1.45, 150.0, 12000.0)});
        const double level = bandDb(y, 1.05, 1.35, 1000.0, 8000.0);
        INFO("cough healed to " << level << " dB, bed " << bedLevel << " dB");
        CHECK(std::fabs(level - bedLevel) < 3.5);
        CHECK(sameSamples(x, y, 0.0, 0.85));
        CHECK(sameSamples(x, y, 1.6, 4.0));
    }
}

TEST_CASE("Heal takes a click out of speech-like sound", "[audio][spectral][dsp][heal]") {
    const std::vector<float> bed = plus(noise(0.02, 2.0, 1, kRate, 5), sine(440.0, 0.1, 2.0, 1));
    std::vector<float> x = bed;
    for (std::size_t i = at(1.0); i < at(1.0) + 40; ++i) { // a click: under a millisecond of sound from 3 to 12 kHz
        const double n = static_cast<double>(i - at(1.0));
        const double shape = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * n / 40.0);
        x[i] += static_cast<float>(shape * (0.5 * std::sin(0.45 * n) + 0.4 * std::sin(1.1 * n) + 0.3 * std::sin(1.6 * n)));
    }
    const std::vector<float> y = repair(x, {rect(SpectralKind::Heal, 0.995, 1.005, 1000.0, 20000.0)});
    const double before = peak(x, 1, at(1.0) - 20, at(1.0) + 60);
    const double after = peak(y, 1, at(1.0) - 20, at(1.0) + 60);
    INFO("peak " << before << " -> " << after);
    CHECK(after < before / 4.0);
    CHECK(toneDb(y, 440.0, 0.5, 1.5) - toneDb(x, 440.0, 0.5, 1.5) == Approx(0.0).margin(0.2));
    CHECK(sameSamples(x, y, 0.0, 0.9));
    CHECK(sameSamples(x, y, 1.1, 2.0));
}

// ---- Remove Tone -------------------------------------------------------------------------------------------------------

TEST_CASE("Remove Tone cuts a steady tone and leaves the bands around it", "[audio][spectral][dsp][tone]") {
    const std::vector<float> neighbours = plus(plus(sine(900.0, 0.1, 4.0, 1), sine(1100.0, 0.1, 4.0, 1)), noise(0.02, 4.0, 1));
    const std::vector<float> x = plus(neighbours, sine(1000.0, 0.3, 4.0, 1));
    const std::vector<float> y = repair(x, {tone(1000.0, 0.0, 4.0)});
    CHECK(toneDb(y, 1000.0, 1.0, 3.0) < toneDb(x, 1000.0, 1.0, 3.0) - 50.0);
    // Less than 1 dB of change in the neighbouring bands (the card's measure).
    CHECK(std::fabs(toneDb(y, 900.0, 1.0, 3.0) - toneDb(x, 900.0, 1.0, 3.0)) < 0.5);
    CHECK(std::fabs(toneDb(y, 1100.0, 1.0, 3.0) - toneDb(x, 1100.0, 1.0, 3.0)) < 0.5);
    CHECK(std::fabs(bandDb(y, 1.0, 3.0, 1030.0, 1090.0) - bandDb(x, 1.0, 3.0, 1030.0, 1090.0)) < 1.0);
    CHECK(std::fabs(bandDb(y, 1.0, 3.0, 2000.0, 6000.0) - bandDb(x, 1.0, 3.0, 2000.0, 6000.0)) < 0.1);
}

TEST_CASE("Remove Tone takes out mains hum and its harmonics only where it is on", "[audio][spectral][dsp][tone]") {
    std::vector<float> hum = plus(plus(sine(50.0, 0.1, 5.0, 1), sine(100.0, 0.05, 5.0, 1)), sine(150.0, 0.05, 5.0, 1));
    const std::vector<float> x = plus(plus(hum, sine(180.0, 0.05, 5.0, 1)), noise(0.01, 5.0, 1));
    const std::vector<float> y = repair(x, {tone(50.0, 1.0, 4.0, 3, 10.0)});
    for (const double f : {50.0, 100.0, 150.0}) {
        INFO(f);
        CHECK(toneDb(y, f, 2.0, 3.5) < toneDb(x, f, 2.0, 3.5) - 40.0);
        CHECK(std::fabs(toneDb(y, f, 0.0, 0.8) - toneDb(x, f, 0.0, 0.8)) < 0.01); // before it is on: untouched
        CHECK(std::fabs(toneDb(y, f, 4.3, 5.0) - toneDb(x, f, 4.3, 5.0)) < 0.2);
    }
    CHECK(std::fabs(toneDb(y, 180.0, 2.0, 3.5) - toneDb(x, 180.0, 2.0, 3.5)) < 0.5);
}

TEST_CASE("Remove Tone follows a tone whose pitch moves", "[audio][spectral][dsp][tone]") {
    std::vector<float> chirp(at(3.0));
    for (std::size_t i = 0; i < chirp.size(); ++i) {
        const double t = static_cast<double>(i) / kRate;
        chirp[i] = static_cast<float>(0.2 * std::sin(2.0 * std::numbers::pi * (1000.0 * t + 125.0 * t * t))); // 1 kHz rising 250 Hz per second
    }
    const std::vector<float> x = plus(chirp, noise(0.005, 3.0, 1));
    SpectralEdit e = tone(1000.0, 0.0, 3.0, 1, 40.0);
    e.path = {{0.0, 1000.0}, {3.0, 1750.0}};
    const std::vector<float> y = repair(x, {e});
    const double before = rms(x, 1, at(0.5), at(2.5));
    const double after = rms(y, 1, at(0.5), at(2.5));
    INFO("rms " << before << " -> " << after);
    CHECK(ampToDb(after) < ampToDb(before) - 20.0);
    // A steady notch at the start frequency leaves most of the chirp.
    const std::vector<float> steady = repair(x, {tone(1000.0, 0.0, 3.0, 1, 40.0)});
    CHECK(ampToDb(rms(steady, 1, at(1.5), at(2.5))) > ampToDb(before) - 3.0);
}

// ---- The listen modes ---------------------------------------------------------------------------------------------------

TEST_CASE("Listen plays only the selected sound or only what the repair removes", "[audio][spectral][dsp]") {
    const std::vector<float> x = plus(plus(sine(1000.0, 0.2, 4.0, 1), sine(5000.0, 0.2, 4.0, 1)), noise(0.002, 4.0, 1));
    const std::vector<SpectralEdit> edits{rect(SpectralKind::Attenuate, 1.0, 3.0, 800.0, 1250.0, -24.0)};
    const std::vector<float> off = repair(x, edits);
    const std::vector<float> selection = repair(x, edits, 1, kRate, dsp::SpectralListen::Selection);
    const std::vector<float> removed = repair(x, edits, 1, kRate, dsp::SpectralListen::Removed);
    const double gain = std::pow(10.0, -24.0 / 20.0);
    // Selection: the 1 kHz tone as it is, nothing of the 5 kHz tone, and silence outside the region in time.
    CHECK(toneDb(selection, 1000.0, 1.5, 2.5) == Approx(ampToDb(0.2)).margin(0.5));
    CHECK(toneDb(selection, 5000.0, 1.5, 2.5) < ampToDb(0.2) - 40.0);
    CHECK(peak(selection, 1, at(0.0), at(0.8)) < 1e-4);
    CHECK(peak(selection, 1, at(3.3), at(4.0)) < 1e-4);
    // Removed: exactly what the repair took away.
    CHECK(toneDb(removed, 1000.0, 1.5, 2.5) == Approx(ampToDb(0.2 * (1.0 - gain))).margin(0.5));
    CHECK(toneDb(removed, 5000.0, 1.5, 2.5) < ampToDb(0.2) - 40.0);
    for (std::size_t i = at(1.5); i < at(2.5); i += 997) {
        CHECK(static_cast<double>(x[i]) - off[i] == Approx(static_cast<double>(removed[i])).margin(2e-4));
    }
}

// ---- Sample rates and channel counts -----------------------------------------------------------------------------------------

TEST_CASE("Spectral Repair works at 96 kHz and on 5.1 without leaking between channels", "[audio][spectral][dsp]") {
    constexpr int kHigh = 96000;
    // One tone in the third channel of six, at 96 kHz.
    constexpr int kChannels = 6;
    const std::size_t frames = at(3.0, kHigh);
    std::vector<float> x(frames * kChannels, 0.0f);
    for (std::size_t i = 0; i < frames; ++i) {
        x[i * kChannels + 2] = static_cast<float>(0.2 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / kHigh));
    }
    for (const int size : {1024, 2048, 4096}) {
        INFO("fft " << size);
        const std::vector<float> y = repair(x, {rect(SpectralKind::Attenuate, 0.8, 2.2, 800.0, 1250.0, -24.0)}, kChannels, kHigh,
                                            dsp::SpectralListen::Off, size);
        REQUIRE(y.size() == x.size());
        CHECK(toneDb(y, 1000.0, 1.2, 1.8, kChannels, 2, kHigh) - toneDb(x, 1000.0, 1.2, 1.8, kChannels, 2, kHigh) == Approx(-24.0).margin(0.7));
        for (const int other : {0, 1, 3, 4, 5}) {
            double worst = 0.0;
            for (std::size_t i = 0; i < frames; i += 7) {
                worst = std::max(worst, static_cast<double>(std::fabs(y[i * kChannels + static_cast<std::size_t>(other)])));
            }
            CHECK(worst == 0.0);
        }
    }
    // Healing and tone removal also keep the channels apart.
    const std::vector<float> healed = repair(x, {rect(SpectralKind::Heal, 0.8, 1.2, 800.0, 1250.0)}, kChannels, kHigh);
    double leaked = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        leaked = std::max(leaked, static_cast<double>(std::fabs(healed[i * kChannels + 0])));
    }
    CHECK(leaked < 1e-9);
}

// ---- The time map ------------------------------------------------------------------------------------------------------------

TEST_CASE("The edit stays on the sound when the clip is trimmed and moved", "[audio][spectral][chain]") {
    // The clip plays source time 3 s from timeline 10 s on; the edit is at source 5.0 .. 5.5 s, so it sounds 2.0 s into the clip.
    Clip clip;
    clip.timelineRange = TimeRange{Ticks{10 * kTicksPerSecond}, Ticks{6 * kTicksPerSecond}};
    clip.sourceIn = Ticks{3 * kTicksPerSecond};
    EffectInstance e = effectOf(std::string(kSpectralRepairEffect));
    e.data = packSpectralEdits(std::vector<SpectralEdit>{rect(SpectralKind::Attenuate, 5.0, 5.5, 800.0, 1250.0, -24.0)});
    const std::vector<float> x = sine(1000.0, 0.2, 5.0, 1);
    const CurveClock clock{clip.start(), Rate{kRate, 1}.ticksPerUnit(), &clip};
    EffectChain chain(1, kRate);
    const EffectStack stack{e};
    chain.bind(stack);
    const int latency = chain.latencyFrames();
    std::vector<float> y = x;
    y.resize(x.size() + static_cast<std::size_t>(latency), 0.0f);
    for (std::size_t done = 0; done < y.size();) {
        const std::size_t n = std::min<std::size_t>(512, y.size() - done);
        chain.process(y.data() + done, static_cast<int>(n), static_cast<std::int64_t>(done), clock);
        done += n;
    }
    y.erase(y.begin(), y.begin() + latency);
    CHECK(toneDb(y, 1000.0, 2.1, 2.4) - toneDb(x, 1000.0, 2.1, 2.4) == Approx(-24.0).margin(0.8)); // 2.0 .. 2.5 s into the clip
    CHECK(toneDb(y, 1000.0, 3.0, 4.5) - toneDb(x, 1000.0, 3.0, 4.5) == Approx(0.0).margin(0.05));
    CHECK(toneDb(y, 1000.0, 0.2, 1.8) - toneDb(x, 1000.0, 0.2, 1.8) == Approx(0.0).margin(0.05));
}

TEST_CASE("A clip played faster moves the edit in time and in pitch", "[audio][spectral][chain]") {
    // At double speed (resampled) the source second 5.0 .. 5.5 of a clip that plays from 4 s sounds 0.5 .. 0.75 s into the clip, an octave up.
    Clip clip;
    clip.timelineRange = TimeRange{Ticks{0}, Ticks{4 * kTicksPerSecond}};
    clip.sourceIn = Ticks{4 * kTicksPerSecond};
    clip.speed = Speed{2, 1};
    clip.maintainPitch = false;
    EffectInstance e = effectOf(std::string(kSpectralRepairEffect));
    e.data = packSpectralEdits(std::vector<SpectralEdit>{rect(SpectralKind::Attenuate, 5.0, 5.5, 800.0, 1250.0, -24.0)});
    const std::vector<float> x = plus(sine(2000.0, 0.2, 2.0, 1), sine(1000.0, 0.2, 2.0, 1)); // a 1 kHz source tone heard at 2 kHz, and a 1 kHz one
    const CurveClock clock{Ticks{}, Rate{kRate, 1}.ticksPerUnit(), &clip};
    EffectChain chain(1, kRate);
    const EffectStack stack{e};
    chain.bind(stack);
    const int latency = chain.latencyFrames();
    std::vector<float> y = x;
    y.resize(x.size() + static_cast<std::size_t>(latency), 0.0f);
    for (std::size_t done = 0; done < y.size();) {
        const std::size_t n = std::min<std::size_t>(512, y.size() - done);
        chain.process(y.data() + done, static_cast<int>(n), static_cast<std::int64_t>(done), clock);
        done += n;
    }
    y.erase(y.begin(), y.begin() + latency);
    CHECK(toneDb(y, 2000.0, 0.56, 0.70) - toneDb(x, 2000.0, 0.56, 0.70) == Approx(-24.0).margin(1.5)); // the 2 kHz of the sped-up clip
    CHECK(toneDb(y, 1000.0, 0.56, 0.70) - toneDb(x, 1000.0, 0.56, 0.70) == Approx(0.0).margin(0.3));    // not the 1 kHz
    CHECK(toneDb(y, 2000.0, 1.0, 1.8) - toneDb(x, 2000.0, 1.0, 1.8) == Approx(0.0).margin(0.1));
}

// ---- Preview equals export ---------------------------------------------------------------------------------------------------

namespace {

/// Chain frames [from, to) of `x` (channels interleaved from chain frame 0) through an EffectChain in blocks of `block`: the
/// output of the chain frames [from, to) after the latency is compensated.
std::vector<float> runChain(const EffectInstance& e, const std::vector<float>& x, int channels, std::int64_t from, std::int64_t to,
                            int block, const CurveClock& clock, int* settle = nullptr) {
    const EffectStack stack{e};
    EffectChain chain(channels, kRate);
    chain.bind(stack);
    if (settle != nullptr) {
        *settle = chain.settleFrames();
    }
    const int latency = chain.latencyFrames();
    const auto ch = static_cast<std::size_t>(channels);
    const std::size_t total = x.size() / ch;
    std::vector<float> out;
    for (std::int64_t done = from; done < to + latency;) {
        const auto n = static_cast<int>(std::min<std::int64_t>(block, to + latency - done));
        std::vector<float> buffer(static_cast<std::size_t>(n) * ch, 0.0f);
        for (int i = 0; i < n; ++i) {
            const auto frame = static_cast<std::size_t>(done + i);
            if (frame < total) {
                for (std::size_t c = 0; c < ch; ++c) {
                    buffer[static_cast<std::size_t>(i) * ch + c] = x[frame * ch + c];
                }
            }
        }
        chain.process(buffer.data(), n, done, clock);
        out.insert(out.end(), buffer.begin(), buffer.end());
        done += n;
    }
    out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(latency) * static_cast<std::ptrdiff_t>(ch));
    return out;
}

} // namespace

TEST_CASE("Preview and export render the repair identically however the sound is sliced and wherever the render starts", "[audio][spectral][chain]") {
    constexpr int kChannels = 2;
    const std::size_t frames = at(4.0);
    std::vector<float> x(frames * kChannels);
    {
        const std::vector<float> l = plus(plus(noise(0.03, 4.0, 1, kRate, 11), sine(800.0, 0.1, 4.0, 1)), burst(3000.0, 0.2, 1.0, 1.5, 4.0));
        const std::vector<float> r = plus(plus(noise(0.03, 4.0, 1, kRate, 22), sine(1200.0, 0.1, 4.0, 1)), sine(1000.0, 0.2, 4.0, 1));
        for (std::size_t i = 0; i < frames; ++i) {
            x[i * kChannels] = l[i];
            x[i * kChannels + 1] = r[i];
        }
    }
    // The region edits (the STFT part) are bit-exact; the tone removal is a filter with a memory that dies away, so after the
    // pre-roll it agrees to better than a millionth.
    EffectInstance spectral = effectOf(std::string(kSpectralRepairEffect));
    spectral.data = packSpectralEdits(std::vector<SpectralEdit>{rect(SpectralKind::Heal, 0.95, 1.55, 2600.0, 3400.0),
                                                                rect(SpectralKind::Attenuate, 2.0, 2.6, 4000.0, 6000.0, -18.0),
                                                                rect(SpectralKind::NoiseFloor, 2.8, 3.1, 100.0, 9000.0)});
    EffectInstance withTone = spectral;
    withTone.id = EffectInstanceId{2};
    {
        std::vector<SpectralEdit> all = unpackSpectralEdits(spectral);
        all.push_back(tone(1000.0, 0.0, 4.0));
        withTone.data = packSpectralEdits(all);
    }
    const CurveClock clock{Ticks{}, Rate{kRate, 1}.ticksPerUnit(), nullptr};
    const std::int64_t end = static_cast<std::int64_t>(frames);
    for (const bool exact : {true, false}) {
        const EffectInstance& e = exact ? spectral : withTone;
        INFO((exact ? "region edits" : "region edits and a tone"));
        int settle = 0;
        const std::vector<float> whole = runChain(e, x, kChannels, 0, end, 512, clock, &settle);
        REQUIRE(whole.size() == frames * kChannels);
        CHECK(settle > 0);
        CHECK(whole != x); // the repair did something
        // Another block size: the same samples, every one.
        CHECK(runChain(e, x, kChannels, 0, end, 977, clock) == whole);
        CHECK(runChain(e, x, kChannels, 0, end, 31, clock) == whole);
        // A render that starts elsewhere (a seek into the sound: the settle time before the part that is kept) agrees with it.
        for (const std::int64_t keep : {static_cast<std::int64_t>(at(0.5)), static_cast<std::int64_t>(at(1.2)), static_cast<std::int64_t>(at(2.9))}) {
            const std::int64_t start = std::max<std::int64_t>(0, keep - settle);
            const std::vector<float> seek = runChain(e, x, kChannels, start, end, 700, clock);
            for (std::int64_t f = keep; f < end; ++f) {
                for (int c = 0; c < kChannels; ++c) {
                    const std::size_t a = static_cast<std::size_t>(f) * kChannels + static_cast<std::size_t>(c);
                    const std::size_t b = static_cast<std::size_t>(f - start) * kChannels + static_cast<std::size_t>(c);
                    const bool same = exact ? whole[a] == seek[b] : std::fabs(whole[a] - seek[b]) < 1e-6f;
                    if (!same) {
                        FAIL("frame " << f << " channel " << c << " differs when the render starts at " << start << ": " << whole[a] << " vs " << seek[b]);
                    }
                }
            }
        }
    }
}

TEST_CASE("The effect reports its latency and settle time from its edits and rebuilds when they change", "[audio][spectral][chain]") {
    EffectInstance none = effectOf(std::string(kSpectralRepairEffect));
    CHECK(effectLatency(none, 2) == 0);
    EffectInstance attenuate = none;
    attenuate.data = packSpectralEdits(std::vector<SpectralEdit>{rect(SpectralKind::Attenuate, 1.0, 2.0, 500.0, 900.0)});
    CHECK(effectLatency(attenuate, 2) == 2048);
    EffectInstance heal = none;
    heal.data = packSpectralEdits(std::vector<SpectralEdit>{rect(SpectralKind::Heal, 1.0, 1.4, 500.0, 900.0)});
    CHECK(effectLatency(heal, 2) > 2048 + 4 * 512);
    EffectInstance big = effectOf(std::string(kSpectralRepairEffect), {{"fftSize", ParamValue(2.0)}});
    big.data = attenuate.data;
    CHECK(effectLatency(big, 2) == 4096);

    // A processor is built from the data it had: changing the edits makes the chain build a new one, the same data keeps it.
    EffectChain chain(1, kRate);
    EffectStack stack{attenuate};
    chain.bind(stack);
    CHECK_FALSE(chain.bind(stack));
    stack[0].data = heal.data;
    CHECK(chain.bind(stack));
    CHECK(chain.latencyFrames() > 2048);
}

TEST_CASE("Spectral Repair is a registered audio effect with a size choice and a listen choice", "[audio][spectral][chain]") {
    const EffectDescriptor* d = EffectRegistry::builtin().find(kSpectralRepairEffect);
    REQUIRE(d != nullptr);
    CHECK(d->kind == EffectKind::Audio);
    CHECK(isImplementedAudioEffect(kSpectralRepairEffect));
    REQUIRE(d->param("fftSize") != nullptr);
    CHECK(d->param("fftSize")->optionLabels().size() == 3);
    REQUIRE(d->param("listen") != nullptr);
    CHECK(d->param("listen")->optionLabel(1) == "Selection");
    CHECK_FALSE(d->param("fftSize")->animatable); // keyframe-free
    CHECK_FALSE(d->param("listen")->animatable);
}

// ---- The spectrogram ---------------------------------------------------------------------------------------------------------

namespace {

/// A sine of `frequency` and amplitude, `length` samples long, and optional clicks, read on demand.
class SynthSource final : public dsp::SampleSource {
public:
    SynthSource(std::int64_t length, double frequency, double amplitude, int rate = kRate)
        : m_length(length), m_frequency(frequency), m_amplitude(amplitude), m_rate(rate) {}
    std::int64_t length() const override { return m_length; }
    void read(std::int64_t start, float* out, std::int64_t count) override {
        ++reads;
        for (std::int64_t i = 0; i < count; ++i) {
            const std::int64_t n = start + i;
            double v = 0.0;
            if (n >= 0 && n < m_length) {
                v = m_amplitude * std::sin(2.0 * std::numbers::pi * m_frequency * static_cast<double>(n % (10 * m_rate)) / m_rate);
                if (n == clickAt) {
                    v += 0.9;
                }
            }
            out[i] = static_cast<float>(v);
        }
    }
    std::int64_t clickAt = -1;
    int reads = 0;

private:
    std::int64_t m_length;
    double m_frequency;
    double m_amplitude;
    int m_rate;
};

int peakBin(const dsp::SpectrogramTile& t, int column) {
    int best = 0;
    for (int b = 0; b < t.bins; ++b) {
        if (t.at(column, b) > t.at(column, best)) {
            best = b;
        }
    }
    return best;
}

} // namespace

TEST_CASE("The spectrogram puts a tone at its bin and level, and pooling keeps a click", "[audio][spectral][spectrogram]") {
    auto source = std::make_shared<SynthSource>(kRate * 6, 3000.0, 0.5);
    dsp::SpectrogramCache cache(source, kRate, 2048, 64u << 20);
    CHECK(cache.bins() == 1025);
    CHECK(cache.hop() == 512);
    const auto tile = cache.tile(0, 0);
    REQUIRE(tile != nullptr);
    CHECK(tile->columns == dsp::kSpectrogramTileColumns);
    const int expected = static_cast<int>(std::lround(3000.0 / (static_cast<double>(kRate) / 2048)));
    for (const int column : {40, 120, 250}) {
        CHECK(std::abs(peakBin(*tile, column) - expected) <= 1);
        // A half-scale sine reads -6 dB at its bin (a quantisation step is 0.47 dB).
        CHECK(dsp::spectrogramDb(tile->at(column, peakBin(*tile, column))) == Approx(-6.0).margin(1.0));
        CHECK(dsp::spectrogramDb(tile->at(column, 900)) < -70.0);
    }
    // The same tone read at a coarse level is the same tone.
    const auto coarse = cache.tile(3, 0);
    REQUIRE(coarse != nullptr);
    CHECK(coarse->columns == static_cast<int>(cache.columns(3)) - 0);
    CHECK(std::abs(peakBin(*coarse, 10) - expected) <= 1);

    // A click 2.5 s in: the loudest bin of its column is far above the tone's sidelobes, and pooling by 16 columns keeps it.
    auto clicked = std::make_shared<SynthSource>(kRate * 6, 3000.0, 0.01);
    clicked->clickAt = kRate * 5 / 2;
    dsp::SpectrogramCache clickCache(clicked, kRate, 2048, 64u << 20);
    const auto fine = clickCache.tile(0, 0);
    const auto pooled = clickCache.tile(4, 0);
    REQUIRE((fine && pooled));
    // Column c is centred on sample c * hop, so the click (sample 120000) sits in columns 234 and 235. Far above the tone, at
    // 14 kHz (bin 600), the click is the only thing there.
    constexpr int kBin = 600;
    int loud = 0;
    for (const int column : {234, 235}) {
        loud = std::max<int>(loud, fine->at(column, kBin));
    }
    const int quiet = fine->at(100, kBin);
    INFO("click " << dsp::spectrogramDb(static_cast<std::uint8_t>(loud)) << " dB, around it " << dsp::spectrogramDb(static_cast<std::uint8_t>(quiet)) << " dB");
    CHECK(dsp::spectrogramDb(static_cast<std::uint8_t>(loud)) > -75.0);
    CHECK(dsp::spectrogramDb(static_cast<std::uint8_t>(quiet)) < -100.0);
    // Sixteen columns pooled into one (234 / 16 = 14) keep the click's level.
    CHECK(pooled->at(14, kBin) == loud);
    CHECK(pooled->at(2, kBin) == pooled->at(3, kBin)); // and the stretches without one stay quiet
    CHECK(dsp::spectrogramDb(pooled->at(2, kBin)) < -100.0);
}

TEST_CASE("Spectrogram tiles are computed on demand within a memory budget and can be cancelled", "[audio][spectral][spectrogram]") {
    // A minute of sound is 22 tiles at level 0; the budget holds four.
    auto source = std::make_shared<SynthSource>(kRate * 60, 1000.0, 0.3);
    dsp::SpectrogramCache cache(source, kRate, 2048, 1);
    const std::size_t tileBytes = static_cast<std::size_t>(dsp::kSpectrogramTileColumns) * 1025;
    CHECK(cache.budget() == 4 * tileBytes);
    CHECK(cache.tiles(0) == 22);
    for (std::int64_t i = 0; i < cache.tiles(0); ++i) {
        REQUIRE(cache.tile(0, i) != nullptr);
        CHECK(cache.bytes() <= cache.budget());
    }
    CHECK(cache.tilesComputed() == 22);
    // The last tiles are cached, the first are not; asking again for a cached one reads nothing.
    CHECK(cache.peek(0, 21) != nullptr);
    CHECK(cache.peek(0, 0) == nullptr);
    const int reads = source->reads;
    CHECK(cache.tile(0, 21) != nullptr);
    CHECK(source->reads == reads);
    // Cancel: a tile whose computation is stopped is not made and nothing partial is kept.
    std::stop_source stop;
    stop.request_stop();
    CHECK(cache.tile(0, 3, stop.get_token()) == nullptr);
    CHECK(cache.peek(0, 3) == nullptr);

    // Ten hours would be 3.4 million columns: the geometry is computed without reading any sound, a far tile reads only its own.
    auto huge = std::make_shared<SynthSource>(static_cast<std::int64_t>(kRate) * 36000, 1000.0, 0.3);
    dsp::SpectrogramCache big(huge, kRate, 2048, 8u << 20);
    CHECK(big.baseColumns() == (static_cast<std::int64_t>(kRate) * 36000 + 511) / 512 + 1);
    CHECK(big.maxLevel() >= 14);
    CHECK(huge->reads == 0);
    const auto far = big.tile(0, 5000);
    REQUIRE(far != nullptr);
    CHECK(far->columns == dsp::kSpectrogramTileColumns);
    CHECK(huge->reads <= 5);
    CHECK(big.bytes() <= big.budget());
    CHECK(dsp::SpectrogramCache::levelFor(0.5) == 0);
    CHECK(dsp::SpectrogramCache::levelFor(1000.0) == 9);
}

TEST_CASE("Spectral coverage is full inside an edit half at its edge and zero outside", "[audio][spectral][dsp]") {
    const SpectralEdit r = rect(SpectralKind::Attenuate, 1.0, 2.0, 1000.0, 2000.0);
    CHECK(dsp::spectralCoverage(r, 1.5, 1500.0, 2048, kRate) == Approx(1.0));
    CHECK(dsp::spectralCoverage(r, 1.0, 1500.0, 2048, kRate) == Approx(0.5).margin(0.01));
    CHECK(dsp::spectralCoverage(r, 1.5, 1000.0, 2048, kRate) == Approx(0.5).margin(0.01));
    CHECK(dsp::spectralCoverage(r, 0.5, 1500.0, 2048, kRate) == 0.0);
    CHECK(dsp::spectralCoverage(r, 1.5, 3000.0, 2048, kRate) == 0.0);
    CHECK(dsp::spectralCoverage(tone(1000.0, 0.0, 2.0, 2, 20.0), 1.0, 2005.0, 2048, kRate) == 1.0);
    CHECK(dsp::spectralCoverage(tone(1000.0, 0.0, 2.0, 2, 20.0), 1.0, 1500.0, 2048, kRate) == 0.0);
}
