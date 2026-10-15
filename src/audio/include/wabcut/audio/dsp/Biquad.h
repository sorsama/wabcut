#pragma once

// Second-order IIR sections from Robert Bristow-Johnson's "Audio EQ Cookbook": the shelves and the peaking band of
// the 3-band EQ, the high- and low-pass sections of the cut filter, the de-esser's side chain and DeHum's notches,
// plus the magnitude response the tests and the UI curve read. Pure and RT-safe.

#include <cmath>

namespace wabcut::audio::dsp {

/// Coefficients normalised by a0: y[n] = b0 x[n] + b1 x[n-1] + b2 x[n-2] - a1 y[n-1] - a2 y[n-2].
struct BiquadCoefficients {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;

    bool operator==(const BiquadCoefficients&) const = default;
};

/// Low shelf: `gainDb` below `frequency` (the half-gain point), unity far above. `slope` S = 1 is the steepest shelf
/// without overshoot. At 0 dB the section is exactly the identity.
BiquadCoefficients lowShelf(double sampleRate, double frequency, double gainDb, double slope = 1.0) noexcept;
/// High shelf: `gainDb` above `frequency`.
BiquadCoefficients highShelf(double sampleRate, double frequency, double gainDb, double slope = 1.0) noexcept;
/// Peaking band: exactly `gainDb` at `frequency`, bandwidth set by `q`.
BiquadCoefficients peaking(double sampleRate, double frequency, double gainDb, double q) noexcept;
/// Second-order high-pass: -3 dB at `frequency` with q = 1/sqrt(2) (Butterworth), 12 dB per octave below it.
BiquadCoefficients highPass(double sampleRate, double frequency, double q = 0.70710678118654752) noexcept;
/// Second-order low-pass, the mirror of highPass().
BiquadCoefficients lowPass(double sampleRate, double frequency, double q = 0.70710678118654752) noexcept;
/// A notch of depth `depthDb` (>= 0) at `frequency`: 1 - (1 - 10^(-depth/20)) x the cookbook's band-pass (0 dB peak),
/// so exactly -depthDb at the frequency and, unlike a deep peaking cut, as narrow as `q` says at any depth.
BiquadCoefficients notch(double sampleRate, double frequency, double q, double depthDb) noexcept;
/// The q of section `k` (0-based) of an order-2m Butterworth filter built from m second-order sections.
double butterworthQ(int sections, int k) noexcept;

/// |H| in dB at `frequency` Hz.
double magnitudeDb(const BiquadCoefficients& c, double frequency, double sampleRate) noexcept;

/// One channel of a section in transposed direct form II with double-precision state (quiet at low frequencies).
/// State far below the float floor is flushed to 0: a narrow section (DeHum's notches) rings down slowly, and in a long
/// silence it would reach the denormal range, which costs x86 about 100 cycles an operation where the thread does not
/// flush denormals (rt::ScopedDenormalsOff).
struct BiquadState {
    static constexpr double kFlush = 1e-30;

    double z1 = 0.0;
    double z2 = 0.0;

    float process(const BiquadCoefficients& c, float input) noexcept {
        const double x = input;
        const double y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        // Both together: flushing one alone (the other still ringing) sets up a limit cycle near 1e-28 at low
        // frequencies that never decays.
        if (std::abs(z1) < kFlush && std::abs(z2) < kFlush) {
            z1 = 0.0;
            z2 = 0.0;
        }
        return static_cast<float>(y);
    }
    void reset() noexcept {
        z1 = 0.0;
        z2 = 0.0;
    }
};

} // namespace wabcut::audio::dsp
