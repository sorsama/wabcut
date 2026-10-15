// The Essential Sound DSP (A.7) against references: the cut filter's Butterworth response, DeHum on a synthetic hum
// (dB removed at each harmonic, the programme untouched), the de-esser on synthetic sibilance, the expander's static
// curve, the reverb's impulse response (RT60, high-frequency damping, decorrelated and mono-able tails, stability, the
// same samples whatever the block size) and stereo width's mono compatibility. `[.report]` prints the numbers.

#include "DspTestUtil.h"

#include <wabcut/audio/dsp/Repair.h>
#include <wabcut/audio/dsp/Space.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <functional>
#include <numbers>
#include <string>
#include <vector>

using namespace wabcut::audio;
using namespace wabcut::audio::test;
using Catch::Approx;

namespace {

constexpr int kRate = 48000;

/// Amplitude of the `frequency` component of channel `channel` over frames [first, last) (a single DFT bin; choose a
/// window of whole cycles).
double amplitudeAt(const std::vector<float>& x, int channels, double frequency, std::size_t first, std::size_t last, int channel = 0) {
    std::complex<double> sum{};
    for (std::size_t i = first; i < last; ++i) {
        const double phase = -2.0 * std::numbers::pi * frequency * static_cast<double>(i) / kRate;
        sum += static_cast<double>(x[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(channel)]) * std::polar(1.0, phase);
    }
    return 2.0 * std::abs(sum) / static_cast<double>(last - first);
}

/// Mono sum of sines: (frequency, amplitude) pairs, on every channel.
std::vector<float> tones(const std::vector<std::pair<double, double>>& parts, double seconds, int channels = 2) {
    std::vector<float> out = sine(parts.front().first, parts.front().second, seconds, channels, kRate);
    for (std::size_t p = 1; p < parts.size(); ++p) {
        const auto more = sine(parts[p].first, parts[p].second, seconds, channels, kRate);
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] += more[i];
        }
    }
    return out;
}

/// Runs `process(block, frames)` over interleaved audio in blocks of `block` frames.
template <class Fn>
void inBlocks(std::vector<float>& x, int channels, int block, Fn&& process) {
    const std::size_t frames = x.size() / static_cast<std::size_t>(channels);
    for (std::size_t i = 0; i < frames; i += static_cast<std::size_t>(block)) {
        const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), frames - i));
        process(x.data() + i * static_cast<std::size_t>(channels), n);
    }
}

/// White noise high-passed above `hz` (two cookbook sections): the sibilance of the de-esser test.
std::vector<float> hissAbove(double hz, double amplitude, double seconds, int channels) {
    std::vector<float> n = noise(1.0, seconds, channels, kRate, 777);
    const dsp::BiquadCoefficients hp = dsp::highPass(kRate, hz);
    for (int c = 0; c < channels; ++c) {
        dsp::BiquadState a;
        dsp::BiquadState b;
        for (std::size_t i = 0; i < n.size() / static_cast<std::size_t>(channels); ++i) {
            float& v = n[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)];
            v = b.process(hp, a.process(hp, v));
        }
    }
    const double r = rms(n, channels, 0, n.size() / static_cast<std::size_t>(channels));
    for (float& v : n) {
        v = static_cast<float>(v * amplitude / r);
    }
    return n;
}

/// RMS of the high-passed (above `hz`) channel 0 over frames [first, last).
double bandRms(const std::vector<float>& x, int channels, double hz, std::size_t first, std::size_t last) {
    const dsp::BiquadCoefficients hp = dsp::highPass(kRate, hz);
    dsp::BiquadState a;
    dsp::BiquadState b;
    double sum = 0.0;
    for (std::size_t i = 0; i < last; ++i) {
        const float v = b.process(hp, a.process(hp, x[i * static_cast<std::size_t>(channels)]));
        if (i >= first) {
            sum += static_cast<double>(v) * v;
        }
    }
    return std::sqrt(sum / static_cast<double>(last - first));
}

/// Decay time to -60 dB from the Schroeder energy decay curve of `ir` (channel 0 + 1), fitted from -5 to -35 dB.
double rt60(const std::vector<float>& ir, int channels) {
    const std::size_t frames = ir.size() / static_cast<std::size_t>(channels);
    std::vector<double> energy(frames + 1, 0.0);
    for (std::size_t i = frames; i-- > 0;) {
        double e = 0.0;
        for (int c = 0; c < channels; ++c) {
            const double v = ir[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)];
            e += v * v;
        }
        energy[i] = energy[i + 1] + e;
    }
    const double total = energy[0];
    std::size_t a = 0;
    std::size_t b = 0;
    for (std::size_t i = 0; i < frames; ++i) {
        const double db = 10.0 * std::log10(energy[i] / total + 1e-300);
        if (a == 0 && db <= -5.0) {
            a = i;
        }
        if (b == 0 && db <= -35.0) {
            b = i;
            break;
        }
    }
    return b > a ? 2.0 * static_cast<double>(b - a) / kRate : 0.0;
}

std::vector<float> reverbImpulse(dsp::ReverbSettings s, double size, double seconds, int block = 256) {
    dsp::Reverb reverb(2, kRate, size, 0.0);
    reverb.setSettings(s);
    std::vector<float> x(static_cast<std::size_t>(seconds * kRate) * 2, 0.0f);
    x[0] = 1.0f;
    x[1] = 1.0f;
    inBlocks(x, 2, block, [&](float* p, int n) { reverb.process(p, n); });
    return x;
}

/// Correlation of L and R over frames [first, last).
double correlation(const std::vector<float>& x, std::size_t first, std::size_t last) {
    double lr = 0.0;
    double ll = 0.0;
    double rr = 0.0;
    for (std::size_t i = first; i < last; ++i) {
        const double l = x[2 * i];
        const double r = x[2 * i + 1];
        lr += l * r;
        ll += l * l;
        rr += r * r;
    }
    return lr / std::sqrt(ll * rr + 1e-300);
}

} // namespace

TEST_CASE("Cut filter: Butterworth slopes at the cut frequencies, off at the ends of its ranges", "[audio][dsp][essential]") {
    dsp::CutFilter hp(1, kRate, 24.0);
    hp.setFrequencies(100.0, dsp::CutFilter::kHighCutOff);
    CHECK(hp.sections() == 2);
    CHECK(hp.magnitudeDb(100.0) == Approx(-3.0103).margin(0.01));
    CHECK(hp.magnitudeDb(50.0) == Approx(-24.1).margin(0.5)); // an octave below: 24 dB per octave
    CHECK(hp.magnitudeDb(1000.0) == Approx(0.0).margin(0.01));
    // The measured response of sines matches the analytic one.
    for (const double f : {40.0, 100.0, 250.0, 2000.0}) {
        dsp::CutFilter filter(1, kRate, 24.0);
        filter.setFrequencies(100.0, dsp::CutFilter::kHighCutOff);
        auto y = sine(f, 0.25, 2.0, 1, kRate);
        const auto in = y;
        filter.process(y.data(), static_cast<int>(y.size()));
        INFO(f);
        CHECK(ampToDb(rms(y, 1, 48000, 96000) / rms(in, 1, 48000, 96000)) == Approx(hp.magnitudeDb(f)).margin(0.05));
    }
    // "Phone": 300 .. 3400 Hz.
    dsp::CutFilter phone(1, kRate, 24.0);
    phone.setFrequencies(300.0, 3400.0);
    CHECK(phone.magnitudeDb(1000.0) == Approx(0.0).margin(0.2));
    CHECK(phone.magnitudeDb(100.0) < -35.0);
    CHECK(phone.magnitudeDb(8000.0) < -25.0);
    // Off: the audio passes bit for bit.
    dsp::CutFilter off(2, kRate, 48.0);
    off.setFrequencies(dsp::CutFilter::kLowCutOff, dsp::CutFilter::kHighCutOff);
    auto x = noise(0.5, 0.5, 2, kRate);
    const auto copy = x;
    off.process(x.data(), static_cast<int>(x.size() / 2));
    CHECK(x == copy);
}

TEST_CASE("DeHum removes a synthetic mains hum and its harmonics and leaves the programme", "[audio][dsp][essential]") {
    for (const double mains : {50.0, 60.0}) {
        INFO(mains);
        // Hum at -20, -26, -30, -34 dBFS on the first four harmonics, a programme tone at 1 kHz, -12 dBFS.
        const std::vector<std::pair<double, double>> hum{{mains, dbToAmp(-20)}, {2 * mains, dbToAmp(-26)}, {3 * mains, dbToAmp(-30)},
                                                         {4 * mains, dbToAmp(-34)}};
        auto parts = hum;
        parts.push_back({1000.0, dbToAmp(-12)});
        auto x = tones(parts, 4.0);
        const auto in = x;
        dsp::DeHum dehum(2, kRate, mains, 4);
        dehum.setReduction(30.0);
        inBlocks(x, 2, 480, [&](float* p, int n) { dehum.process(p, n); });
        // After a second of settling, two seconds (whole cycles of every frequency).
        for (const auto& [f, a] : hum) {
            const double before = amplitudeAt(in, 2, f, 48000, 144000);
            const double after = amplitudeAt(x, 2, f, 48000, 144000);
            INFO(f);
            CHECK(ampToDb(after / before) < -28.0);
        }
        CHECK(ampToDb(amplitudeAt(x, 2, 1000.0, 48000, 144000) / amplitudeAt(in, 2, 1000.0, 48000, 144000)) == Approx(0.0).margin(0.05));
        CHECK(dehum.magnitudeDb(mains) == Approx(-30.0).margin(0.05));
        CHECK(dehum.magnitudeDb(mains * 1.5) > -0.5); // between the harmonics
    }
    // 0 dB reduction: bit-exact identity.
    dsp::DeHum off(2, kRate, 50.0, 8);
    off.setReduction(0.0);
    auto x = noise(0.3, 0.5, 2, kRate);
    const auto copy = x;
    off.process(x.data(), static_cast<int>(x.size() / 2));
    CHECK(x == copy);
}

// Review wave 5, A5-P2-9. DeHum's notches (q 12: poles at a radius of about 0.9997 at 50 Hz) ring down slowly; in a long
// digital silence their double state decays towards the denormal range, where x86 spends about 100 cycles an
// operation. BiquadState flushes state below 1e-30 (it is reached after about 5 s here). Fails today: after 10 s of
// silence the state is still about 1e-57, not 0.
TEST_CASE("A DeHum notch rings down to an exactly zero state in a long silence", "[audio][dsp][essential]") {
    const dsp::BiquadCoefficients hum = dsp::notch(kRate, 50.0, dsp::DeHum::kNotchQ, 30.0);
    dsp::BiquadState s;
    for (const float v : sine(50.0, dbToAmp(-20), 1.0, 1, kRate)) {
        (void)s.process(hum, v);
    }
    CHECK(s.z1 != 0.0); // ringing when the tone stops
    for (int i = 0; i < 10 * kRate; ++i) {
        (void)s.process(hum, 0.0f);
    }
    CHECK(s.z1 == 0.0);
    CHECK(s.z2 == 0.0);
    CHECK(s.process(hum, 0.0f) == 0.0f);
}

TEST_CASE("The de-esser turns synthetic sibilance down and leaves the voice band and the quiet parts as they were",
          "[audio][dsp][essential]") {
    // A "voice" (220 Hz, -14 dBFS) throughout, with 150 ms bursts of hiss above 6 kHz (-18 dBFS) every 500 ms.
    const double seconds = 3.0;
    auto x = sine(220.0, dbToAmp(-14), seconds, 2, kRate);
    const auto hiss = hissAbove(6000.0, dbToAmp(-18), seconds, 2);
    const auto frames = static_cast<std::size_t>(seconds * kRate);
    std::vector<bool> burst(frames, false);
    for (std::size_t i = 0; i < frames; ++i) {
        burst[i] = (i % 24000) < 7200;
        if (burst[i]) {
            x[2 * i] += hiss[2 * i];
            x[2 * i + 1] += hiss[2 * i + 1];
        }
    }
    const auto in = x;
    dsp::DeEsser deess(2, kRate);
    deess.setSettings({-30.0, 10.0, 5500.0});
    inBlocks(x, 2, 64, [&](float* p, int n) { deess.process(p, n); });
    // Inside a burst (after its first 20 ms): the band is down by most of the 10 dB allowed.
    const std::size_t b0 = 24000 + 960;
    const std::size_t b1 = 24000 + 7200;
    const double reduction = ampToDb(bandRms(x, 2, 6000.0, b0, b1) / bandRms(in, 2, 6000.0, b0, b1));
    CHECK(reduction < -6.0);
    CHECK(reduction > -10.5);
    // The voice is not touched (within 0.3 dB at 220 Hz, also during the bursts).
    CHECK(ampToDb(amplitudeAt(x, 2, 220.0, b0, b0 + 4800) / amplitudeAt(in, 2, 220.0, b0, b0 + 4800)) == Approx(0.0).margin(0.3));
    // Between bursts, once the detector released: bit for bit.
    bool exact = true;
    for (std::size_t i = 24000 + 7200 + 9600; i < 48000; ++i) {
        exact = exact && x[2 * i] == in[2 * i] && x[2 * i + 1] == in[2 * i + 1];
    }
    CHECK(exact);
}

TEST_CASE("The expander follows its static curve: quiet passages go down, loud ones pass", "[audio][dsp][essential]") {
    dsp::ExpanderSettings s; // -45 dB, 2:1, 12 dB range
    CHECK(dsp::expanderGainDb(-20.0, s) == 0.0);
    CHECK(dsp::expanderGainDb(-50.0, s) == Approx(-5.0));
    CHECK(dsp::expanderGainDb(-80.0, s) == Approx(-12.0)); // the range
    s.ratio = 1.0;
    CHECK(dsp::expanderGainDb(-80.0, s) == 0.0);
    // Steady tones through the processor (peak level = amplitude).
    const auto through = [](double dbfs) {
        dsp::Expander e(2, kRate);
        e.setSettings(dsp::ExpanderSettings{});
        auto x = sine(1000.0, dbToAmp(dbfs), 2.0, 2, kRate);
        const auto in = x;
        e.process(x.data(), static_cast<int>(x.size() / 2));
        return ampToDb(rms(x, 2, 72000, 96000) / rms(in, 2, 72000, 96000));
    };
    CHECK(through(-20.0) == Approx(0.0).margin(0.01));
    CHECK(through(-49.0) == Approx(-4.0).margin(0.6));  // 4 dB under: 4 dB more
    CHECK(through(-70.0) == Approx(-12.0).margin(0.3)); // the range
}

TEST_CASE("Reverb: its impulse decays at the RT60 asked, damping shortens the highs, the tail is decorrelated and stable",
          "[audio][dsp][essential]") {
    for (const double decay : {600.0, 1500.0, 3000.0}) {
        dsp::ReverbSettings s;
        s.decayMs = decay;
        s.damping = 0.0;
        s.mix = 1.0;
        const auto ir = reverbImpulse(s, 0.5, decay / 1000.0 * 1.6 + 0.3);
        INFO(decay);
        CHECK(rt60(ir, 2) == Approx(decay / 1000.0).epsilon(0.15));
    }
    // Damping: the highs die away faster than the lows.
    {
        dsp::ReverbSettings s;
        s.decayMs = 2000.0;
        s.damping = 0.8;
        s.mix = 1.0;
        const auto ir = reverbImpulse(s, 0.5, 3.5);
        std::vector<float> highs = ir;
        std::vector<float> lows = ir;
        const dsp::BiquadCoefficients hp = dsp::highPass(kRate, 8000.0);
        const dsp::BiquadCoefficients lp = dsp::lowPass(kRate, 500.0);
        for (int c = 0; c < 2; ++c) {
            dsp::BiquadState a, b, d, e;
            for (std::size_t i = 0; i < ir.size() / 2; ++i) {
                float& h = highs[2 * i + static_cast<std::size_t>(c)];
                h = b.process(hp, a.process(hp, h));
                float& l = lows[2 * i + static_cast<std::size_t>(c)];
                l = e.process(lp, d.process(lp, l));
            }
        }
        const double low = rt60(lows, 2);
        const double high = rt60(highs, 2);
        CHECK(low == Approx(2.0).epsilon(0.25));
        CHECK(high < 0.7 * low);
    }
    dsp::ReverbSettings s;
    s.decayMs = 1000.0;
    s.mix = 1.0;
    const auto ir = reverbImpulse(s, 0.5, 3.0);
    // Decorrelated left and right in the tail (after the first 50 ms).
    CHECK(std::fabs(correlation(ir, 2400, 48000)) < 0.3);
    // Width 0: a mono tail.
    s.width = 0.0;
    const auto mono = reverbImpulse(s, 0.5, 1.0);
    bool same = true;
    for (std::size_t i = 0; i < mono.size() / 2; ++i) {
        same = same && std::fabs(mono[2 * i] - mono[2 * i + 1]) < 1e-7f;
    }
    CHECK(same);
    // Stable and without DC: 3 s after the impulse the tail is 150 dB down; its mean is ~0.
    const double first = rms(ir, 2, 0, 4800);
    const double last = rms(ir, 2, 139200, 144000);
    CHECK(ampToDb(last / first) < -150.0);
    double mean = 0.0;
    for (std::size_t i = 0; i < 48000; ++i) {
        mean += ir[2 * i];
    }
    CHECK(std::fabs(mean / 48000.0) < 0.05 * rms(ir, 2, 0, 48000));
    // The same samples whatever the blocks (preview's callbacks, export's blocks).
    s.width = 1.0;
    const auto a = reverbImpulse(s, 0.5, 1.0, 1);
    const auto b = reverbImpulse(s, 0.5, 1.0, 37);
    const auto c = reverbImpulse(s, 0.5, 1.0, 4096);
    CHECK(a == b);
    CHECK(a == c);
    // Mix 0: the dry signal bit for bit; and silence after the tail is exactly zero.
    {
        dsp::Reverb dry(2, kRate, 0.3, 20.0);
        dsp::ReverbSettings d;
        d.mix = 0.0;
        dry.setSettings(d);
        auto x = noise(0.5, 0.5, 2, kRate);
        const auto copy = x;
        dry.process(x.data(), static_cast<int>(x.size() / 2));
        CHECK(x == copy);
    }
    {
        dsp::Reverb r(2, kRate, 0.2, 0.0);
        dsp::ReverbSettings d;
        d.decayMs = 200.0;
        d.mix = 0.5;
        r.setSettings(d);
        std::vector<float> x(static_cast<std::size_t>(kRate) * 4 * 2, 0.0f);
        x[0] = 1.0f;
        r.process(x.data(), static_cast<int>(x.size() / 2));
        bool zero = true;
        for (std::size_t i = static_cast<std::size_t>(kRate) * 3 * 2; i < x.size(); ++i) {
            zero = zero && x[i] == 0.0f;
        }
        CHECK(zero);
    }
}

TEST_CASE("Reverb on a 5.1 buffer leaves the LFE dry and feeds the centre the tail's mid", "[audio][dsp][essential]") {
    // Review A5-P2-10: the tank's input was the mean of all six channels and its tail went to the channels by parity,
    // so the LFE (channel 3) got the right side's full-band tail.
    constexpr std::size_t kCh = 6;
    dsp::Reverb r(static_cast<int>(kCh), kRate, 0.5, 10.0);
    dsp::ReverbSettings s;
    s.mix = 1.0;
    s.decayMs = 800.0;
    r.setSettings(s);
    const auto stereo = noise(0.5, 0.5, 2, kRate, 5);
    std::vector<float> x(stereo.size() / 2 * kCh, 0.0f);
    for (std::size_t f = 0; f < stereo.size() / 2; ++f) {
        x[f * kCh + 0] = stereo[2 * f];     // L
        x[f * kCh + 1] = stereo[2 * f + 1]; // R
        x[f * kCh + 3] = 0.25f;             // a constant on the LFE: it must come out as it went in
    }
    r.process(x.data(), static_cast<int>(x.size() / kCh));
    bool lfeDry = true;
    double front = 0.0;
    double centre = 0.0;
    double surround = 0.0;
    for (std::size_t f = 0; f < x.size() / kCh; ++f) {
        lfeDry = lfeDry && x[f * kCh + 3] == 0.25f;
        front += std::fabs(x[f * kCh + 0]) + std::fabs(x[f * kCh + 1]);
        centre += std::fabs(x[f * kCh + 2]);
        surround += std::fabs(x[f * kCh + 4]) + std::fabs(x[f * kCh + 5]);
    }
    CHECK(lfeDry);
    CHECK(front > 0.0);
    CHECK(centre > 0.0);
    CHECK(surround > 0.0);
    // The front and surround pairs carry the same tail (the stereo tail on both pairs).
    for (std::size_t f = 1000; f < 1010; ++f) {
        CHECK(x[f * kCh + 4] == x[f * kCh + 0]);
        CHECK(x[f * kCh + 5] == x[f * kCh + 1]);
    }
}

TEST_CASE("Stereo width is mono compatible at any width and keeps the bass centred", "[audio][dsp][essential]") {
    const auto in = noise(0.4, 1.0, 2, kRate, 99); // different left and right
    for (const double width : {0.0, 0.5, 1.5, 2.0}) {
        dsp::StereoWidth w(2, kRate);
        w.setSettings(width, 0.0);
        auto x = in;
        w.process(x.data(), static_cast<int>(x.size() / 2));
        double worst = 0.0;
        double sideIn = 0.0;
        double sideOut = 0.0;
        for (std::size_t i = 0; i < x.size() / 2; ++i) {
            worst = std::max(worst, static_cast<double>(std::fabs((x[2 * i] + x[2 * i + 1]) - (in[2 * i] + in[2 * i + 1]))));
            sideIn += std::pow(in[2 * i] - in[2 * i + 1], 2.0);
            sideOut += std::pow(x[2 * i] - x[2 * i + 1], 2.0);
        }
        INFO(width);
        CHECK(worst < 1e-6); // the mono fold-down is the input's
        CHECK(std::sqrt(sideOut / sideIn) == Approx(width).margin(1e-5));
    }
    // Width 1 without mono bass: bit for bit.
    dsp::StereoWidth unity(2, kRate);
    unity.setSettings(1.0, 0.0);
    auto x = in;
    unity.process(x.data(), static_cast<int>(x.size() / 2));
    CHECK(x == in);
    // Mono bass below 150 Hz: a 40 Hz tone on the left only (half of it is side) comes out centred.
    dsp::StereoWidth bass(2, kRate);
    bass.setSettings(1.2, 150.0);
    auto low = sine(40.0, 0.5, 2.0, 2, kRate);
    for (std::size_t i = 0; i < low.size() / 2; ++i) {
        low[2 * i + 1] = 0.0f;
    }
    bass.process(low.data(), static_cast<int>(low.size() / 2));
    std::vector<float> side(low.size() / 2);
    for (std::size_t i = 0; i < side.size(); ++i) {
        side[i] = 0.5f * (low[2 * i] - low[2 * i + 1]);
    }
    CHECK(ampToDb(rms(side, 1, 48000, 96000) / (0.25 / std::sqrt(2.0))) < -15.0);
}

TEST_CASE("Essential Sound DSP numbers", "[.report]") {
    std::string out;
    char line[256];
    auto row = [&](const char* fmt, auto... args) {
        std::snprintf(line, sizeof line, fmt, args...);
        out += line;
        out += '\n';
    };
    // DeHum on the synthetic hum.
    for (const double mains : {50.0, 60.0}) {
        auto x = tones({{mains, dbToAmp(-20)}, {2 * mains, dbToAmp(-26)}, {3 * mains, dbToAmp(-30)}, {4 * mains, dbToAmp(-34)}, {1000.0, dbToAmp(-12)}}, 4.0);
        const auto in = x;
        dsp::DeHum dehum(2, kRate, mains, 4);
        dehum.setReduction(30.0);
        dehum.process(x.data(), static_cast<int>(x.size() / 2));
        std::string cells;
        for (int k = 1; k <= 4; ++k) {
            char cell[32];
            std::snprintf(cell, sizeof cell, " %.1f", ampToDb(amplitudeAt(x, 2, k * mains, 48000, 144000) / amplitudeAt(in, 2, k * mains, 48000, 144000)));
            cells += cell;
        }
        row("DeHum %.0f Hz, 30 dB, 4 harmonics: dB at the harmonics%s; 1 kHz programme %.3f dB", mains, cells.c_str(),
            ampToDb(amplitudeAt(x, 2, 1000.0, 48000, 144000) / amplitudeAt(in, 2, 1000.0, 48000, 144000)));
    }
    // De-esser.
    {
        auto x = sine(220.0, dbToAmp(-14), 3.0, 2, kRate);
        const auto hiss = hissAbove(6000.0, dbToAmp(-18), 3.0, 2);
        for (std::size_t i = 0; i < x.size() / 2; ++i) {
            if ((i % 24000) < 7200) {
                x[2 * i] += hiss[2 * i];
                x[2 * i + 1] += hiss[2 * i + 1];
            }
        }
        const auto in = x;
        dsp::DeEsser d(2, kRate);
        d.setSettings({-30.0, 10.0, 5500.0});
        d.process(x.data(), static_cast<int>(x.size() / 2));
        row("DeEsser (-30 dB threshold, 10 dB max): sibilance band %.2f dB, voice (220 Hz) %.3f dB",
            ampToDb(bandRms(x, 2, 6000.0, 24960, 31200) / bandRms(in, 2, 6000.0, 24960, 31200)),
            ampToDb(amplitudeAt(x, 2, 220.0, 24960, 29760) / amplitudeAt(in, 2, 220.0, 24960, 29760)));
    }
    // Reverb RT60.
    for (const double decay : {600.0, 1500.0, 3000.0}) {
        dsp::ReverbSettings s;
        s.decayMs = decay;
        s.damping = 0.0;
        s.mix = 1.0;
        const auto ir = reverbImpulse(s, 0.5, decay / 1000.0 * 1.6 + 0.3);
        row("Reverb decay %.0f ms (damping 0): measured RT60 %.3f s, L/R tail correlation %.3f", decay, rt60(ir, 2), correlation(ir, 2400, 24000));
    }
    // Cost: microseconds per second of stereo 48 kHz audio, processed in 256-frame blocks.
    const auto cost = [&](const char* name, const std::function<void(float*, int)>& process) {
        auto x = noise(0.3, 10.0, 2, kRate);
        const auto t0 = std::chrono::steady_clock::now();
        inBlocks(x, 2, 256, process);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 10.0;
        row("%-34s %8.1f us per second of stereo audio (%.3f %% of real time)", name, us, us / 1e4);
    };
    dsp::CutFilter filter(2, kRate, 24.0);
    filter.setFrequencies(80.0, 12000.0);
    cost("Filter (24 dB/oct, both sides)", [&](float* p, int n) { filter.process(p, n); });
    dsp::DeHum dehum(2, kRate, 50.0, 4);
    dehum.setReduction(30.0);
    cost("DeHum (4 harmonics)", [&](float* p, int n) { dehum.process(p, n); });
    dsp::DeEsser deess(2, kRate);
    deess.setSettings({-30.0, 10.0, 5500.0});
    cost("DeEsser", [&](float* p, int n) { deess.process(p, n); });
    dsp::Expander expander(2, kRate);
    cost("Expander", [&](float* p, int n) { expander.process(p, n); });
    dsp::Reverb reverb(2, kRate, 0.7, 20.0);
    dsp::ReverbSettings rs;
    rs.decayMs = 2000.0;
    reverb.setSettings(rs);
    cost("Reverb (8-line FDN)", [&](float* p, int n) { reverb.process(p, n); });
    dsp::StereoWidth width(2, kRate);
    width.setSettings(1.4, 120.0);
    cost("Stereo width (mono bass)", [&](float* p, int n) { width.process(p, n); });
    WARN(out);
}
