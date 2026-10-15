#include <wabcut/audio/dsp/Biquad.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace wabcut::audio::dsp {

namespace {

/// The cookbook's w0, clamped inside (0, Nyquist) so coefficients stay finite for any input.
double omega(double sampleRate, double frequency) noexcept {
    const double fs = sampleRate > 0.0 ? sampleRate : 48000.0;
    const double f = std::clamp(frequency, 1e-3, 0.4999 * fs);
    return 2.0 * std::numbers::pi * f / fs;
}

BiquadCoefficients normalise(double b0, double b1, double b2, double a0, double a1, double a2) noexcept {
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

/// alpha for a shelf of slope S: sin(w0)/2 * sqrt((A + 1/A)(1/S - 1) + 2).
double shelfAlpha(double w0, double a, double slope) noexcept {
    const double s = std::clamp(slope, 1e-3, 1.0);
    return std::sin(w0) / 2.0 * std::sqrt(std::max((a + 1.0 / a) * (1.0 / s - 1.0) + 2.0, 0.0));
}

} // namespace

BiquadCoefficients lowShelf(double sampleRate, double frequency, double gainDb, double slope) noexcept {
    const double a = std::pow(10.0, gainDb / 40.0);
    const double w0 = omega(sampleRate, frequency);
    const double cosw = std::cos(w0);
    const double alpha = shelfAlpha(w0, a, slope);
    const double sqrtA2alpha = 2.0 * std::sqrt(a) * alpha;
    return normalise(a * ((a + 1.0) - (a - 1.0) * cosw + sqrtA2alpha), 2.0 * a * ((a - 1.0) - (a + 1.0) * cosw),
                     a * ((a + 1.0) - (a - 1.0) * cosw - sqrtA2alpha), (a + 1.0) + (a - 1.0) * cosw + sqrtA2alpha,
                     -2.0 * ((a - 1.0) + (a + 1.0) * cosw), (a + 1.0) + (a - 1.0) * cosw - sqrtA2alpha);
}

BiquadCoefficients highShelf(double sampleRate, double frequency, double gainDb, double slope) noexcept {
    const double a = std::pow(10.0, gainDb / 40.0);
    const double w0 = omega(sampleRate, frequency);
    const double cosw = std::cos(w0);
    const double alpha = shelfAlpha(w0, a, slope);
    const double sqrtA2alpha = 2.0 * std::sqrt(a) * alpha;
    return normalise(a * ((a + 1.0) + (a - 1.0) * cosw + sqrtA2alpha), -2.0 * a * ((a - 1.0) + (a + 1.0) * cosw),
                     a * ((a + 1.0) + (a - 1.0) * cosw - sqrtA2alpha), (a + 1.0) - (a - 1.0) * cosw + sqrtA2alpha,
                     2.0 * ((a - 1.0) - (a + 1.0) * cosw), (a + 1.0) - (a - 1.0) * cosw - sqrtA2alpha);
}

BiquadCoefficients peaking(double sampleRate, double frequency, double gainDb, double q) noexcept {
    const double a = std::pow(10.0, gainDb / 40.0);
    const double w0 = omega(sampleRate, frequency);
    const double cosw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * std::max(q, 1e-3));
    return normalise(1.0 + alpha * a, -2.0 * cosw, 1.0 - alpha * a, 1.0 + alpha / a, -2.0 * cosw, 1.0 - alpha / a);
}

BiquadCoefficients highPass(double sampleRate, double frequency, double q) noexcept {
    const double w0 = omega(sampleRate, frequency);
    const double cosw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * std::max(q, 1e-3));
    return normalise((1.0 + cosw) / 2.0, -(1.0 + cosw), (1.0 + cosw) / 2.0, 1.0 + alpha, -2.0 * cosw, 1.0 - alpha);
}

BiquadCoefficients lowPass(double sampleRate, double frequency, double q) noexcept {
    const double w0 = omega(sampleRate, frequency);
    const double cosw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * std::max(q, 1e-3));
    return normalise((1.0 - cosw) / 2.0, 1.0 - cosw, (1.0 - cosw) / 2.0, 1.0 + alpha, -2.0 * cosw, 1.0 - alpha);
}

BiquadCoefficients notch(double sampleRate, double frequency, double q, double depthDb) noexcept {
    const double w0 = omega(sampleRate, frequency);
    const double cosw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * std::max(q, 1e-3));
    const double k = 1.0 - std::pow(10.0, -std::max(depthDb, 0.0) / 20.0); // how much of the band-pass is taken out
    // Band-pass: (alpha - alpha z^-2) / (1 + alpha - 2 cos z^-1 + (1 - alpha) z^-2); the result is 1 - k times it.
    return normalise(1.0 + alpha - k * alpha, -2.0 * cosw, 1.0 - alpha + k * alpha, 1.0 + alpha, -2.0 * cosw, 1.0 - alpha);
}

double butterworthQ(int sections, int k) noexcept {
    const int m = std::max(sections, 1);
    // Poles of an order-2m Butterworth filter in conjugate pairs: q = 1 / (2 cos((2k + 1) pi / (4m))).
    return 1.0 / (2.0 * std::cos((2.0 * k + 1.0) * std::numbers::pi / (4.0 * m)));
}

double magnitudeDb(const BiquadCoefficients& c, double frequency, double sampleRate) noexcept {
    const double w = 2.0 * std::numbers::pi * frequency / sampleRate;
    const std::complex<double> z1 = std::polar(1.0, -w);
    const std::complex<double> z2 = z1 * z1;
    const std::complex<double> h = (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
    return 20.0 * std::log10(std::max(std::abs(h), 1e-30));
}

} // namespace wabcut::audio::dsp
