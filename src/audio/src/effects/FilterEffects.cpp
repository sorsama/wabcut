// p02 audio: Premiere Pro's Filter and EQ effects, Stereo Imagery's Stereo Expander and the simple channel effects
// (descriptors: src/model/src/EffectsAudioFilter.cpp). Own DSP on the cookbook sections of dsp/Biquad.h, run in double
// precision between sections; the Scientific Filter designs its analog prototypes (Bessel, Butterworth, Chebyshev,
// elliptic: poles and zeros from the textbook formulas, Jacobi elliptic functions by Landen transformations) and maps
// them with the bilinear transform, prewarped at the band edges. Everything that allocates happens in the
// constructors; process() and reset() are RT-safe.

#include "PremiereEffects.h"

#include <wabcut/audio/dsp/Biquad.h>
#include <wabcut/audio/dsp/Equalizer.h> // kMaxDspChannels
#include <wabcut/audio/dsp/Fft.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

namespace wabcut::audio::premiere {

namespace {

using Complex = std::complex<double>;
using dsp::BiquadCoefficients;
constexpr double kPi = std::numbers::pi;

// ---- Effect ids --------------------------------------------------------------------------------------------------

constexpr std::string_view kGraphicEq10 = "wabcut.audio.graphicEq10";
constexpr std::string_view kGraphicEq20 = "wabcut.audio.graphicEq20";
constexpr std::string_view kGraphicEq30 = "wabcut.audio.graphicEq30";
constexpr std::string_view kParametricEq = "wabcut.audio.parametricEq";
constexpr std::string_view kSimpleParametricEq = "wabcut.audio.simpleParametricEq";
constexpr std::string_view kHighpass = "wabcut.audio.highpass";
constexpr std::string_view kLowpass = "wabcut.audio.lowpass";
constexpr std::string_view kBandpass = "wabcut.audio.bandpass";
constexpr std::string_view kSimpleNotch = "wabcut.audio.simpleNotch";
constexpr std::string_view kNotchFilter = "wabcut.audio.notchFilter";
constexpr std::string_view kScientificFilter = "wabcut.audio.scientificFilter";
constexpr std::string_view kFftFilter = "wabcut.audio.fftFilter";
constexpr std::string_view kStereoExpander = "wabcut.audio.stereoExpander";
constexpr std::string_view kBalance = "wabcut.audio.balance";
constexpr std::string_view kBass = "wabcut.audio.bass";
constexpr std::string_view kTreble = "wabcut.audio.treble";
constexpr std::string_view kChannelVolume = "wabcut.audio.channelVolume";
constexpr std::string_view kFillLeft = "wabcut.audio.fillLeftWithRight";
constexpr std::string_view kFillRight = "wabcut.audio.fillRightWithLeft";
constexpr std::string_view kInvert = "wabcut.audio.invert";
constexpr std::string_view kSwapChannels = "wabcut.audio.swapChannels";

constexpr std::array<std::string_view, 21> kIds{
    kGraphicEq10, kGraphicEq20, kGraphicEq30, kParametricEq, kSimpleParametricEq, kHighpass,    kLowpass,
    kBandpass,    kSimpleNotch, kNotchFilter, kScientificFilter, kFftFilter,     kStereoExpander, kBalance,
    kBass,        kTreble,      kChannelVolume, kFillLeft,    kFillRight,        kInvert,        kSwapChannels,
};

// ---- Small helpers -----------------------------------------------------------------------------------------------

double dbToGain(double db) noexcept { return db <= -144.0 ? 0.0 : std::pow(10.0, db / 20.0); }

/// A frequency the cookbook sections can take at this rate: 1 Hz .. 0.49 x the rate.
double usableFrequency(double frequency, double sampleRate) noexcept {
    return std::clamp(frequency, 1.0, 0.49 * sampleRate);
}

std::size_t slot(const EffectDescriptor& d, std::string_view key) noexcept {
    return static_cast<std::size_t>(paramIndex(d, key));
}

/// A non-animatable value from the build-time values (switches, choices, sizes), rounded to an integer.
int choiceOf(const EffectDescriptor& d, std::span<const double> values, std::string_view key) noexcept {
    const std::size_t i = slot(d, key);
    return i < values.size() ? static_cast<int>(std::lround(values[i])) : 0;
}

double constantOf(const EffectDescriptor& d, std::span<const double> values, std::string_view key) noexcept {
    const std::size_t i = slot(d, key);
    return i < values.size() ? values[i] : 0.0;
}

constexpr BiquadCoefficients kIdentity{};

/// A linear gain ramped across a parameter cell (sample-accurate automation, as the Gain effect does).
struct GainRamp {
    double g0 = 1.0;
    double step = 0.0;
    int offset = 0;

    static GainRamp ofDb(const ParamCell& cell, int index) noexcept {
        GainRamp r;
        r.g0 = dbToGain(cell.start[index]);
        r.step = (dbToGain(cell.end[index]) - r.g0) / cell.length;
        r.offset = cell.offset;
        return r;
    }
    double at(int i) const noexcept { return g0 + step * (offset + i); }
    bool unity() const noexcept { return g0 == 1.0 && step == 0.0; }
};

/// One value interpolated linearly across a cell (pan positions, widths).
double rampAt(const ParamCell& cell, int index, int i) noexcept {
    const double a = cell.start[index];
    const double b = cell.end[index];
    return a + (b - a) * (cell.offset + i) / cell.length;
}

// ---- Cookbook sections this family adds to dsp/Biquad.h ---------------------------------------------------------

BiquadCoefficients normalised(double b0, double b1, double b2, double a0, double a1, double a2) noexcept {
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

double omegaOf(double sampleRate, double frequency) noexcept {
    return 2.0 * kPi * usableFrequency(frequency, sampleRate) / sampleRate;
}

/// Band-pass of 0 dB peak gain at `frequency`, bandwidth set by q (-3 dB points).
BiquadCoefficients bandPass(double sampleRate, double frequency, double q) noexcept {
    const double w0 = omegaOf(sampleRate, frequency);
    const double alpha = std::sin(w0) / (2.0 * std::max(q, 1e-3));
    return normalised(alpha, 0.0, -alpha, 1.0 + alpha, -2.0 * std::cos(w0), 1.0 - alpha);
}

/// Full-depth notch at `frequency` (-3 dB bandwidth set by q).
BiquadCoefficients fullNotch(double sampleRate, double frequency, double q) noexcept {
    const double w0 = omegaOf(sampleRate, frequency);
    const double cosw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * std::max(q, 1e-3));
    return normalised(1.0, -2.0 * cosw, 1.0, 1.0 + alpha, -2.0 * cosw, 1.0 - alpha);
}

/// Shelves with the cookbook's Q form of alpha (q = 1/sqrt(2) is the steepest shelf without overshoot). 0 dB is
/// exactly the identity.
BiquadCoefficients shelfQ(double sampleRate, double frequency, double gainDb, double q, bool high) noexcept {
    const double a = std::pow(10.0, gainDb / 40.0);
    const double w0 = omegaOf(sampleRate, frequency);
    const double cosw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * std::max(q, 1e-3));
    const double k = 2.0 * std::sqrt(a) * alpha;
    if (high) {
        return normalised(a * ((a + 1.0) + (a - 1.0) * cosw + k), -2.0 * a * ((a - 1.0) + (a + 1.0) * cosw),
                          a * ((a + 1.0) + (a - 1.0) * cosw - k), (a + 1.0) - (a - 1.0) * cosw + k,
                          2.0 * ((a - 1.0) - (a + 1.0) * cosw), (a + 1.0) - (a - 1.0) * cosw - k);
    }
    return normalised(a * ((a + 1.0) - (a - 1.0) * cosw + k), 2.0 * a * ((a - 1.0) - (a + 1.0) * cosw),
                      a * ((a + 1.0) - (a - 1.0) * cosw - k), (a + 1.0) + (a - 1.0) * cosw + k,
                      -2.0 * ((a - 1.0) + (a + 1.0) * cosw), (a + 1.0) + (a - 1.0) * cosw - k);
}

/// First-order shelf (6 dB per octave): `gainDb` far below (low) or far above (high) `frequency`, half of it (in dB)
/// at `frequency`; the bilinear transform of (s + wc sqrt(g)) / (s + wc / sqrt(g)) (low) or
/// (g s + wc sqrt(g)) / (s + wc sqrt(g)) (high), prewarped at `frequency`. 0 dB is exactly the identity.
BiquadCoefficients firstOrderShelf(double sampleRate, double frequency, double gainDb, bool high) noexcept {
    const double k = std::tan(omegaOf(sampleRate, frequency) / 2.0);
    const double g = std::pow(10.0, gainDb / 20.0);
    const double r = std::sqrt(g);
    if (high) {
        const double a0 = 1.0 + k * r;
        return {(g + k * r) / a0, (k * r - g) / a0, 0.0, (k * r - 1.0) / a0, 0.0};
    }
    const double a0 = 1.0 + k / r;
    return {(1.0 + k * r) / a0, (k * r - 1.0) / a0, 0.0, (k / r - 1.0) / a0, 0.0};
}

/// First-order high- or low-pass (bilinear, prewarped): -3 dB at `frequency`, 6 dB per octave.
BiquadCoefficients firstOrder(double sampleRate, double frequency, bool high) noexcept {
    const double k = std::tan(omegaOf(sampleRate, frequency) / 2.0);
    const double a1 = (k - 1.0) / (k + 1.0);
    if (high) {
        return {1.0 / (1.0 + k), -1.0 / (1.0 + k), 0.0, a1, 0.0};
    }
    return {k / (1.0 + k), k / (1.0 + k), 0.0, a1, 0.0};
}

/// Butterworth high- or low-pass of `order` (1..8) at `frequency` (-3 dB) as cookbook sections (plus a first-order
/// one for an odd order) sharing the corner: the exact bilinear Butterworth. Writes order/2 + order%2 sections.
int butterworthSections(BiquadCoefficients* out, int order, bool high, double sampleRate, double frequency) noexcept {
    const int n = std::clamp(order, 1, 8);
    int count = 0;
    for (int m = 0; m < n / 2; ++m) {
        // Pole angle from the negative real axis: (2m + 1) pi / 2n (even order) or (m + 1) pi / n (odd order).
        const double phi = (n % 2 == 0) ? (2.0 * m + 1.0) * kPi / (2.0 * n) : (m + 1.0) * kPi / n;
        const double q = 1.0 / (2.0 * std::cos(phi));
        out[count++] = high ? dsp::highPass(sampleRate, usableFrequency(frequency, sampleRate), q)
                            : dsp::lowPass(sampleRate, usableFrequency(frequency, sampleRate), q);
    }
    if (n % 2 == 1) {
        out[count++] = firstOrder(sampleRate, frequency, high);
    }
    return count;
}

/// |H|^2 of a section at a frequency given as cos(w) and cos(2w) (no complex arithmetic: the graphic EQ's solver
/// evaluates hundreds of these per update).
double powerAt(const BiquadCoefficients& c, double cos1, double cos2) noexcept {
    const double num = c.b0 * c.b0 + c.b1 * c.b1 + c.b2 * c.b2 + 2.0 * (c.b0 * c.b1 + c.b1 * c.b2) * cos1 + 2.0 * c.b0 * c.b2 * cos2;
    const double den = 1.0 + c.a1 * c.a1 + c.a2 * c.a2 + 2.0 * (c.a1 + c.a1 * c.a2) * cos1 + 2.0 * c.a2 * cos2;
    return num / std::max(den, 1e-300);
}

/// Frames for a section's impulse response to fall by 80 dB (from its largest pole radius).
double decayFrames(const BiquadCoefficients& c) noexcept {
    const double disc = c.a1 * c.a1 - 4.0 * c.a2;
    double r = 0.0;
    if (disc < 0.0) {
        r = std::sqrt(std::max(c.a2, 0.0));
    } else {
        const double s = std::sqrt(disc);
        r = std::max(std::abs((-c.a1 + s) / 2.0), std::abs((-c.a1 - s) / 2.0));
    }
    if (r <= 1e-9) {
        return 2.0;
    }
    if (r >= 1.0) {
        return 1e12;
    }
    return std::log(1e-4) / std::log(r);
}

// ---- Cascade -----------------------------------------------------------------------------------------------------

/// Sections in series per channel, in transposed direct form II with double-precision state and signal between the
/// sections (the float rounding happens once, at the output). State far below the float floor is flushed (both
/// values together, see dsp::BiquadState). Identity sections pass the signal exactly, so a flat EQ is bit-exact.
class Cascade {
public:
    Cascade(int channels, int capacity)
        : m_channels(std::clamp(channels, 1, dsp::kMaxDspChannels)), m_capacity(std::max(capacity, 1)),
          m_coefficients(static_cast<std::size_t>(m_capacity)),
          m_state(static_cast<std::size_t>(m_channels) * static_cast<std::size_t>(m_capacity)) {}

    int channels() const noexcept { return m_channels; }
    int capacity() const noexcept { return m_capacity; }
    int size() const noexcept { return m_size; }
    void resize(int n) noexcept { m_size = std::clamp(n, 0, m_capacity); }
    void set(int i, const BiquadCoefficients& c) noexcept { m_coefficients[static_cast<std::size_t>(i)] = c; }
    const BiquadCoefficients& at(int i) const noexcept { return m_coefficients[static_cast<std::size_t>(i)]; }
    BiquadCoefficients* data() noexcept { return m_coefficients.data(); }

    /// One sample of channel `c` through the active sections.
    double run(int c, double x) noexcept {
        State* s = m_state.data() + static_cast<std::size_t>(c) * static_cast<std::size_t>(m_capacity);
        for (int i = 0; i < m_size; ++i) {
            const BiquadCoefficients& k = m_coefficients[static_cast<std::size_t>(i)];
            State& z = s[i];
            const double y = k.b0 * x + z.z1;
            z.z1 = k.b1 * x - k.a1 * y + z.z2;
            z.z2 = k.b2 * x - k.a2 * y;
            if (std::abs(z.z1) < kFlush && std::abs(z.z2) < kFlush) {
                z.z1 = 0.0;
                z.z2 = 0.0;
            }
            x = y;
        }
        return x;
    }

    /// Every channel through the sections, then times `gain` (ramped).
    void process(float* io, int frames, const GainRamp& gain) noexcept {
        const auto ch = static_cast<std::size_t>(m_channels);
        for (int i = 0; i < frames; ++i) {
            const double g = gain.at(i);
            float* frame = io + static_cast<std::size_t>(i) * ch;
            for (std::size_t c = 0; c < ch; ++c) {
                frame[c] = static_cast<float>(run(static_cast<int>(c), frame[c]) * g);
            }
        }
    }

    void reset() noexcept { std::fill(m_state.begin(), m_state.end(), State{}); }

    double magnitudeDb(double frequency, double sampleRate) const noexcept {
        double db = 0.0;
        for (int i = 0; i < m_size; ++i) {
            db += dsp::magnitudeDb(m_coefficients[static_cast<std::size_t>(i)], frequency, sampleRate);
        }
        return db;
    }

    /// Pre-roll for the current sections: twice the slowest section's 80 dB decay, 10 ms .. 2 s.
    int settleFrames(int sampleRate) const noexcept {
        double frames = 0.0;
        for (int i = 0; i < m_size; ++i) {
            frames = std::max(frames, decayFrames(m_coefficients[static_cast<std::size_t>(i)]));
        }
        return static_cast<int>(std::clamp(2.0 * frames, 0.01 * sampleRate, 2.0 * sampleRate));
    }

private:
    static constexpr double kFlush = 1e-30;
    struct State {
        double z1 = 0.0;
        double z2 = 0.0;
    };

    int m_channels;
    int m_capacity;
    int m_size = 0;
    std::vector<BiquadCoefficients> m_coefficients;
    std::vector<State> m_state; ///< channel-major: [channel * capacity + section]
};

// ---- Analog prototypes and the bilinear transform (Scientific Filter) --------------------------------------------

constexpr int kMaxPrototypeOrder = 32;
constexpr int kMaxRoots = 2 * kMaxPrototypeOrder;

/// Poles, zeros and gain of a transfer function, s or z domain (fixed capacity: designs run in process()).
struct Zpk {
    std::array<Complex, kMaxRoots> zeros{};
    std::array<Complex, kMaxRoots> poles{};
    int nz = 0;
    int np = 0;
    double gain = 1.0;

    void addZero(Complex z) noexcept {
        if (nz < kMaxRoots) {
            zeros[static_cast<std::size_t>(nz++)] = z;
        }
    }
    void addPole(Complex p) noexcept {
        if (np < kMaxRoots) {
            poles[static_cast<std::size_t>(np++)] = p;
        }
    }
    void addZeroPair(Complex z) noexcept {
        addZero(z);
        addZero(std::conj(z));
    }
    void addPolePair(Complex p) noexcept {
        addPole(p);
        addPole(std::conj(p));
    }
};

/// Gain that gives `zpk` the value `target` at s = 0 (a low-pass prototype's DC gain).
double gainForDc(const Zpk& f, double target) noexcept {
    Complex num(1.0, 0.0);
    Complex den(1.0, 0.0);
    for (int i = 0; i < f.np; ++i) {
        den *= -f.poles[static_cast<std::size_t>(i)];
    }
    for (int i = 0; i < f.nz; ++i) {
        num *= -f.zeros[static_cast<std::size_t>(i)];
    }
    return target * (den / num).real();
}

Zpk butterworthPrototype(int order) noexcept {
    Zpk f;
    const int n = std::clamp(order, 1, kMaxPrototypeOrder);
    for (int k = 0; k < n / 2; ++k) {
        const double theta = kPi * (2.0 * k + 1.0) / (2.0 * n);
        f.addPolePair(Complex(-std::sin(theta), std::cos(theta)));
    }
    if (n % 2 == 1) {
        f.addPole(Complex(-1.0, 0.0));
    }
    f.gain = gainForDc(f, 1.0);
    return f;
}

/// Chebyshev type I: equiripple of `rippleDb` in the passband, which ends at 1 rad/s (where the gain is -rippleDb).
Zpk chebyshevPrototype(int order, double rippleDb) noexcept {
    Zpk f;
    const int n = std::clamp(order, 1, kMaxPrototypeOrder);
    const double eps = std::sqrt(std::pow(10.0, std::max(rippleDb, 1e-4) / 10.0) - 1.0);
    const double mu = std::asinh(1.0 / eps) / n;
    for (int k = 0; k < n / 2; ++k) {
        const double theta = kPi * (2.0 * k + 1.0) / (2.0 * n);
        f.addPolePair(Complex(-std::sinh(mu) * std::sin(theta), std::cosh(mu) * std::cos(theta)));
    }
    if (n % 2 == 1) {
        f.addPole(Complex(-std::sinh(mu), 0.0));
    }
    f.gain = gainForDc(f, n % 2 == 1 ? 1.0 : 1.0 / std::sqrt(1.0 + eps * eps));
    return f;
}

/// Bessel (maximally flat group delay), scaled to -3 dB at 1 rad/s. Roots of the reverse Bessel polynomial by
/// Durand-Kerner iteration, polished by Newton; not RT-safe in spirit (iterative) but allocation-free and only run in
/// constructors.
Zpk besselPrototype(int order) noexcept {
    Zpk f;
    const int n = std::clamp(order, 1, kMaxPrototypeOrder);
    // theta_n(s) = sum a_k s^k, a_k = (2n - k)! / (2^(n - k) k! (n - k)!); made monic.
    std::array<double, kMaxPrototypeOrder + 1> a{};
    for (int k = 0; k <= n; ++k) {
        double v = 1.0;
        // (2n - k)! / ((n - k)! k! 2^(n - k)) computed as a running product to stay in range.
        for (int i = n - k + 1; i <= 2 * n - k; ++i) {
            v *= i;
        }
        for (int i = 2; i <= k; ++i) {
            v /= i;
        }
        v /= std::pow(2.0, n - k);
        a[static_cast<std::size_t>(k)] = v;
    }
    const double lead = a[static_cast<std::size_t>(n)];
    for (int k = 0; k <= n; ++k) {
        a[static_cast<std::size_t>(k)] /= lead;
    }
    const auto poly = [&](Complex s) {
        Complex v(1.0, 0.0);
        for (int k = n - 1; k >= 0; --k) {
            v = v * s + a[static_cast<std::size_t>(k)];
        }
        return v;
    };
    const auto deriv = [&](Complex s) {
        Complex v(static_cast<double>(n), 0.0);
        for (int k = n - 1; k >= 1; --k) {
            v = v * s + static_cast<double>(k) * a[static_cast<std::size_t>(k)];
        }
        return v;
    };
    std::array<Complex, kMaxPrototypeOrder> r{};
    const double radius = std::pow(std::abs(a[0]), 1.0 / n);
    for (int i = 0; i < n; ++i) {
        r[static_cast<std::size_t>(i)] = std::polar(radius, 2.0 * kPi * i / n + 0.4);
    }
    for (int iter = 0; iter < 500; ++iter) {
        double change = 0.0;
        for (int i = 0; i < n; ++i) {
            Complex den(1.0, 0.0);
            for (int j = 0; j < n; ++j) {
                if (j != i) {
                    den *= r[static_cast<std::size_t>(i)] - r[static_cast<std::size_t>(j)];
                }
            }
            const Complex step = poly(r[static_cast<std::size_t>(i)]) / den;
            r[static_cast<std::size_t>(i)] -= step;
            change = std::max(change, std::abs(step) / std::max(1.0, std::abs(r[static_cast<std::size_t>(i)])));
        }
        if (change < 1e-15) {
            break;
        }
    }
    for (int i = 0; i < n; ++i) {
        Complex& s = r[static_cast<std::size_t>(i)];
        for (int iter = 0; iter < 3; ++iter) {
            const Complex d = deriv(s);
            if (std::abs(d) > 0.0) {
                s -= poly(s) / d;
            }
        }
    }
    // Keep one of each conjugate pair (upper half plane) and the real root of an odd order.
    std::sort(r.begin(), r.begin() + n, [](Complex x, Complex y) { return x.imag() > y.imag(); });
    for (int i = 0; i < n / 2; ++i) {
        f.addPolePair(r[static_cast<std::size_t>(i)]);
    }
    if (n % 2 == 1) {
        f.addPole(Complex(r[static_cast<std::size_t>(n / 2)].real(), 0.0));
    }
    // Scale to -3 dB at 1 rad/s: |H(jw)|^2 = prod |p|^2 / prod |jw - p|^2 falls monotonically; bisect on log w.
    const auto power = [&](double w) {
        double v = 1.0;
        for (int i = 0; i < f.np; ++i) {
            const Complex p = f.poles[static_cast<std::size_t>(i)];
            v *= std::norm(p) / std::norm(Complex(0.0, w) - p);
        }
        return v;
    };
    double lo = std::log(1e-3);
    double hi = std::log(1e3);
    for (int iter = 0; iter < 200; ++iter) {
        const double mid = 0.5 * (lo + hi);
        (power(std::exp(mid)) > 0.5 ? lo : hi) = mid;
    }
    const double w3 = std::exp(0.5 * (lo + hi));
    for (int i = 0; i < f.np; ++i) {
        f.poles[static_cast<std::size_t>(i)] /= w3;
    }
    f.gain = gainForDc(f, 1.0);
    return f;
}

// Jacobi elliptic functions for the elliptic (Cauer) design, after S. J. Orfanidis, "Lecture Notes on Elliptic
// Filter Design" (2006): descending Landen transformations of the modulus; functions take their argument in units of
// the quarter period K (cd(u K, k), sn(u K, k)).

struct Landen {
    std::array<double, 24> v{};
    int n = 0;
};

/// Descending Landen moduli of k, given with its complement k' = sqrt(1 - k^2): k_n = (k_n-1 / (1 + k'_n-1))^2 and
/// k'_n = 2 sqrt(k'_n-1) / (1 + k'_n-1), both free of cancellation (k may be 1 to double precision).
Landen landen(double k, double kp) noexcept {
    Landen out;
    while (out.n < static_cast<int>(out.v.size()) && k > 1e-18) {
        const double next = (k / (1.0 + kp)) * (k / (1.0 + kp));
        kp = 2.0 * std::sqrt(kp) / (1.0 + kp);
        k = next;
        out.v[static_cast<std::size_t>(out.n++)] = k;
    }
    return out;
}

/// Complete elliptic integral K(k) from the Landen moduli.
double ellipticK(const Landen& l) noexcept {
    double v = kPi / 2.0;
    for (int i = 0; i < l.n; ++i) {
        v *= 1.0 + l.v[static_cast<std::size_t>(i)];
    }
    return v;
}

Complex ascend(Complex w, const Landen& l) noexcept {
    for (int i = l.n - 1; i >= 0; --i) {
        const double v = l.v[static_cast<std::size_t>(i)];
        w = (1.0 + v) * w / (1.0 + v * w * w);
    }
    return w;
}

Complex cde(Complex u, const Landen& l) noexcept { return ascend(std::cos(u * (kPi / 2.0)), l); }
Complex sne(Complex u, const Landen& l) noexcept { return ascend(std::sin(u * (kPi / 2.0)), l); }

double symmetricRemainder(double x, double y) noexcept { return y > 0.0 ? x - y * std::round(x / y) : x; }

/// Inverse of cde: u with cd(u K, k) = w; Re u in [-2, 2], Im u in [-R, R] with R = K'/K.
Complex acde(Complex w, double k, const Landen& l, double ratio) noexcept {
    double previous = k;
    for (int i = 0; i < l.n; ++i) {
        const double v = l.v[static_cast<std::size_t>(i)];
        w = w / (1.0 + std::sqrt(1.0 - w * w * previous * previous)) * (2.0 / (1.0 + v));
        previous = v;
    }
    const Complex u = std::acos(w) * (2.0 / kPi);
    return {symmetricRemainder(u.real(), 4.0), symmetricRemainder(u.imag(), 2.0 * ratio)};
}

Complex asne(Complex w, double k, const Landen& l, double ratio) noexcept { return 1.0 - acde(w, k, l, ratio); }

/// Elliptic (Cauer): passband ripple `rippleDb` up to 1 rad/s, at least `stopDb` down beyond the stopband edge the
/// order allows (the degree equation gives the selectivity k = wp / ws).
Zpk ellipticPrototype(int order, double rippleDb, double stopDb) noexcept {
    const int n = std::clamp(order, 1, kMaxPrototypeOrder);
    const double rp = std::max(rippleDb, 1e-4);
    const double rs = std::max(stopDb, rp + 1.0);
    const double ep = std::sqrt(std::pow(10.0, rp / 10.0) - 1.0);
    const double es = std::sqrt(std::pow(10.0, rs / 10.0) - 1.0);
    const double k1 = ep / es;
    const double k1p = std::sqrt((1.0 - k1) * (1.0 + k1));
    const int pairs = n / 2;
    // Degree equation: k' = k1'^n prod sn^4((2i - 1)/n K1', k1').
    const Landen l1p = landen(k1p, k1);
    double kp = std::pow(k1p, n);
    for (int i = 1; i <= pairs; ++i) {
        const double s = sne(Complex((2.0 * i - 1.0) / n, 0.0), l1p).real();
        kp *= s * s * s * s;
    }
    const double k = std::sqrt(std::max(0.0, (1.0 - kp) * (1.0 + kp)));
    const Landen lk = landen(k, kp);
    const Landen l1 = landen(k1, k1p);
    const double ratio1 = ellipticK(l1p) / ellipticK(l1);
    const double v0 = (Complex(0.0, -1.0) * asne(Complex(0.0, 1.0 / ep), k1, l1, ratio1)).real() / n;
    Zpk f;
    for (int i = 1; i <= pairs; ++i) {
        const double u = (2.0 * i - 1.0) / n;
        const double zeta = cde(Complex(u, 0.0), lk).real();
        f.addZeroPair(Complex(0.0, 1.0 / (k * zeta)));
        Complex p = Complex(0.0, 1.0) * cde(Complex(u, -v0), lk);
        if (p.real() > 0.0) {
            p = -std::conj(p);
        }
        f.addPolePair(p);
    }
    if (n % 2 == 1) {
        const Complex p = Complex(0.0, 1.0) * sne(Complex(0.0, v0), lk);
        f.addPole(Complex(-std::abs(p.real()), 0.0));
    }
    f.gain = gainForDc(f, n % 2 == 1 ? 1.0 : 1.0 / std::sqrt(1.0 + ep * ep));
    return f;
}

enum class Band { LowPass, HighPass, BandPass, BandStop };

/// The prototype (low-pass, edge 1 rad/s) moved to analog edges (rad/s, prewarped): `w1` the cutoff (the lower edge of
/// a band), `w2` the upper edge of a band.
Zpk transform(const Zpk& proto, Band band, double w1, double w2) noexcept {
    Zpk f;
    const int degree = proto.np - proto.nz;
    Complex zprod(1.0, 0.0);
    Complex pprod(1.0, 0.0);
    for (int i = 0; i < proto.nz; ++i) {
        zprod *= -proto.zeros[static_cast<std::size_t>(i)];
    }
    for (int i = 0; i < proto.np; ++i) {
        pprod *= -proto.poles[static_cast<std::size_t>(i)];
    }
    switch (band) {
    case Band::LowPass:
        for (int i = 0; i < proto.nz; ++i) {
            f.addZero(proto.zeros[static_cast<std::size_t>(i)] * w1);
        }
        for (int i = 0; i < proto.np; ++i) {
            f.addPole(proto.poles[static_cast<std::size_t>(i)] * w1);
        }
        f.gain = proto.gain * std::pow(w1, degree);
        break;
    case Band::HighPass:
        for (int i = 0; i < proto.nz; ++i) {
            f.addZero(w1 / proto.zeros[static_cast<std::size_t>(i)]);
        }
        for (int i = 0; i < proto.np; ++i) {
            f.addPole(w1 / proto.poles[static_cast<std::size_t>(i)]);
        }
        for (int i = 0; i < degree; ++i) {
            f.addZero(Complex(0.0, 0.0));
        }
        f.gain = proto.gain * (zprod / pprod).real();
        break;
    case Band::BandPass: {
        const double bw = w2 - w1;
        const double w0 = std::sqrt(w1 * w2);
        const auto split = [&](Complex r, bool zero) {
            const Complex half = r * (bw / 2.0);
            const Complex root = std::sqrt(half * half - w0 * w0);
            if (zero) {
                f.addZero(half + root);
                f.addZero(half - root);
            } else {
                f.addPole(half + root);
                f.addPole(half - root);
            }
        };
        for (int i = 0; i < proto.nz; ++i) {
            split(proto.zeros[static_cast<std::size_t>(i)], true);
        }
        for (int i = 0; i < proto.np; ++i) {
            split(proto.poles[static_cast<std::size_t>(i)], false);
        }
        for (int i = 0; i < degree; ++i) {
            f.addZero(Complex(0.0, 0.0));
        }
        f.gain = proto.gain * std::pow(bw, degree);
        break;
    }
    case Band::BandStop: {
        const double bw = w2 - w1;
        const double w0 = std::sqrt(w1 * w2);
        const auto split = [&](Complex r, bool zero) {
            const Complex half = (bw / 2.0) / r;
            const Complex root = std::sqrt(half * half - w0 * w0);
            if (zero) {
                f.addZero(half + root);
                f.addZero(half - root);
            } else {
                f.addPole(half + root);
                f.addPole(half - root);
            }
        };
        for (int i = 0; i < proto.nz; ++i) {
            split(proto.zeros[static_cast<std::size_t>(i)], true);
        }
        for (int i = 0; i < proto.np; ++i) {
            split(proto.poles[static_cast<std::size_t>(i)], false);
        }
        for (int i = 0; i < degree; ++i) {
            f.addZero(Complex(0.0, w0));
            f.addZero(Complex(0.0, -w0));
        }
        f.gain = proto.gain * (zprod / pprod).real();
        break;
    }
    }
    return f;
}

/// s -> z by the bilinear transform s = 2 fs (z - 1) / (z + 1); zeros at infinity land on z = -1.
Zpk bilinear(const Zpk& analog, double sampleRate) noexcept {
    Zpk f;
    const double fs2 = 2.0 * sampleRate;
    Complex num(1.0, 0.0);
    Complex den(1.0, 0.0);
    for (int i = 0; i < analog.nz; ++i) {
        const Complex z = analog.zeros[static_cast<std::size_t>(i)];
        f.addZero((fs2 + z) / (fs2 - z));
        num *= fs2 - z;
    }
    for (int i = 0; i < analog.np; ++i) {
        const Complex p = analog.poles[static_cast<std::size_t>(i)];
        f.addPole((fs2 + p) / (fs2 - p));
        den *= fs2 - p;
    }
    for (int i = analog.nz; i < analog.np; ++i) {
        f.addZero(Complex(-1.0, 0.0));
    }
    f.gain = analog.gain * (num / den).real();
    return f;
}

/// Groups a digital zpk (as many zeros as poles, conjugate pairs) into second-order sections: each conjugate pole pair
/// (then each pair of real poles, then a last real pole) takes the nearest conjugate zero pair or real zeros. Each
/// section is scaled to unity gain at `referenceOmega` (rad/sample, a passband frequency: 0 for a low pass, pi for a high
/// pass, the centre of a band pass) and the overall gain there goes into the first section, so that no section carries
/// a tiny or huge gain (a high-order design's overall gain can be 1e-40: alone in one section, its state would sink
/// under the denormal flush). Returns the number of sections written (at most `capacity`).
int toSections(const Zpk& f, BiquadCoefficients* out, int capacity, double referenceOmega) noexcept {
    constexpr double kTol = 1e-9;
    std::array<Complex, kMaxRoots> pp{}; // pole pair representatives (Im > 0)
    std::array<double, kMaxRoots> pr{};  // real poles
    std::array<Complex, kMaxRoots> zp{};
    std::array<double, kMaxRoots> zr{};
    std::array<bool, kMaxRoots> zpUsed{};
    std::array<bool, kMaxRoots> zrUsed{};
    int npp = 0;
    int npr = 0;
    int nzp = 0;
    int nzr = 0;
    for (int i = 0; i < f.np; ++i) {
        const Complex p = f.poles[static_cast<std::size_t>(i)];
        if (std::abs(p.imag()) <= kTol * std::max(1.0, std::abs(p))) {
            pr[static_cast<std::size_t>(npr++)] = p.real();
        } else if (p.imag() > 0.0) {
            pp[static_cast<std::size_t>(npp++)] = p;
        }
    }
    for (int i = 0; i < f.nz; ++i) {
        const Complex z = f.zeros[static_cast<std::size_t>(i)];
        if (std::abs(z.imag()) <= kTol * std::max(1.0, std::abs(z))) {
            zr[static_cast<std::size_t>(nzr++)] = z.real();
        } else if (z.imag() > 0.0) {
            zp[static_cast<std::size_t>(nzp++)] = z;
        }
    }
    int count = 0;
    // Takes zeros for a group of `m` poles (1 or 2) near `near`: b(z) = 1 + b1 z^-1 + b2 z^-2.
    const auto takeZeros = [&](int m, Complex near, double& b1, double& b2) {
        b1 = 0.0;
        b2 = 0.0;
        if (m == 2) {
            int best = -1;
            double bestDist = 0.0;
            for (int i = 0; i < nzp; ++i) {
                if (!zpUsed[static_cast<std::size_t>(i)]) {
                    const double d = std::abs(zp[static_cast<std::size_t>(i)] - near);
                    if (best < 0 || d < bestDist) {
                        best = i;
                        bestDist = d;
                    }
                }
            }
            if (best >= 0) {
                const Complex z = zp[static_cast<std::size_t>(best)];
                zpUsed[static_cast<std::size_t>(best)] = true;
                b1 = -2.0 * z.real();
                b2 = std::norm(z);
                return;
            }
        }
        // Real zeros, nearest first.
        double roots[2] = {0.0, 0.0};
        int got = 0;
        for (int k = 0; k < m; ++k) {
            int best = -1;
            double bestDist = 0.0;
            for (int i = 0; i < nzr; ++i) {
                if (!zrUsed[static_cast<std::size_t>(i)]) {
                    const double d = std::abs(Complex(zr[static_cast<std::size_t>(i)], 0.0) - near);
                    if (best < 0 || d < bestDist) {
                        best = i;
                        bestDist = d;
                    }
                }
            }
            if (best >= 0) {
                zrUsed[static_cast<std::size_t>(best)] = true;
                roots[got++] = zr[static_cast<std::size_t>(best)];
            }
        }
        if (got == 2) {
            b1 = -(roots[0] + roots[1]);
            b2 = roots[0] * roots[1];
        } else if (got == 1) {
            b1 = -roots[0];
        }
    };
    // Least damped pole pairs first (nearest the unit circle): the most selective sections take their zeros first.
    std::sort(pp.begin(), pp.begin() + npp, [](Complex a, Complex b) { return std::abs(a) > std::abs(b); });
    for (int i = 0; i < npp && count < capacity; ++i) {
        const Complex p = pp[static_cast<std::size_t>(i)];
        double b1 = 0.0;
        double b2 = 0.0;
        takeZeros(2, p, b1, b2);
        out[count++] = {1.0, b1, b2, -2.0 * p.real(), std::norm(p)};
    }
    for (int i = 0; i + 1 < npr && count < capacity; i += 2) {
        const double p1 = pr[static_cast<std::size_t>(i)];
        const double p2 = pr[static_cast<std::size_t>(i + 1)];
        double b1 = 0.0;
        double b2 = 0.0;
        takeZeros(2, Complex(0.5 * (p1 + p2), 0.0), b1, b2);
        out[count++] = {1.0, b1, b2, -(p1 + p2), p1 * p2};
    }
    if (npr % 2 == 1 && count < capacity) {
        const double p = pr[static_cast<std::size_t>(npr - 1)];
        double b1 = 0.0;
        double b2 = 0.0;
        takeZeros(1, Complex(p, 0.0), b1, b2);
        out[count++] = {1.0, b1, b2, -p, 0.0};
    }
    // |H(e^jw)| of a section at the reference, numerator and denominator separately.
    const Complex z1 = std::polar(1.0, -referenceOmega);
    const Complex z2 = z1 * z1;
    double overall = f.gain;
    for (int i = 0; i < count; ++i) {
        BiquadCoefficients& c = out[i];
        const double num = std::abs(c.b0 + c.b1 * z1 + c.b2 * z2);
        const double den = std::abs(1.0 + c.a1 * z1 + c.a2 * z2);
        if (num > 1e-12 && den > 1e-300) {
            const double scale = den / num;
            c.b0 *= scale;
            c.b1 *= scale;
            c.b2 *= scale;
            overall /= scale;
        }
    }
    if (count > 0) {
        out[0].b0 *= overall;
        out[0].b1 *= overall;
        out[0].b2 *= overall;
    }
    return count;
}

/// A cut (or boost) of `gainDb` (not 0) at `frequency` of Butterworth order 2 `order` (`order` sections), after the
/// high-order parametric designs (S. J. Orfanidis, "High-Order Digital Parametric Equalizer Design", JAES 2005): a
/// Butterworth shelving prototype |H|^2 = (g^2 + e^2 w^2N) / (1 + e^2 w^2N) moved to the band by the low-pass to
/// band-pass transform. The band's edges (centre / q apart) are where the gain is -3 dB (+3 dB for a boost) when the
/// gain exceeds 6 dB, or half the gain otherwise: so a notch is as wide as q says whatever its depth, the centre is
/// exactly `gainDb`, the response falls monotonically into it (no bumps beside it) and a higher order has a flatter
/// bottom and steeper walls.
int notchSections(BiquadCoefficients* out, int order, double sampleRate, double frequency, double gainDb, double q) noexcept {
    const int n = std::clamp(order, 1, 4);
    const double fs = sampleRate;
    const double f0 = std::clamp(frequency, 1.0, 0.45 * fs);
    const double depth = std::clamp(gainDb, -120.0, 120.0);
    const double g = std::pow(10.0, depth / 20.0);
    const double edgeDb = depth < 0.0 ? std::max(depth / 2.0, -3.0103) : std::min(depth / 2.0, 3.0103);
    const double gb = std::pow(10.0, edgeDb / 20.0);
    const double eps = std::sqrt((g * g - gb * gb) / (gb * gb - 1.0));
    Zpk shelf;
    const double rp = std::pow(eps, -1.0 / n);
    const double rz = std::pow(g / eps, 1.0 / n);
    for (int k = 0; k < n / 2; ++k) {
        const double theta = kPi * (2.0 * k + 1.0) / (2.0 * n);
        const Complex unit(-std::sin(theta), std::cos(theta));
        shelf.addPolePair(rp * unit);
        shelf.addZeroPair(rz * unit);
    }
    if (n % 2 == 1) {
        shelf.addPole(Complex(-rp, 0.0));
        shelf.addZero(Complex(-rz, 0.0));
    }
    shelf.gain = 1.0;
    // Edges geometric about the centre, centre / q apart; the centre prewarped exactly, the width approximately.
    const double half = 1.0 / (2.0 * std::max(q, 0.1));
    const double f1 = f0 * (std::sqrt(1.0 + half * half) - half);
    const double f2 = std::min(f0 * (std::sqrt(1.0 + half * half) + half), 0.49 * fs);
    const auto warp = [fs](double f) { return 2.0 * fs * std::tan(kPi * f / fs); };
    const double w0 = warp(f0);
    const double bw = std::max(warp(f2) - warp(f1), 1e-6);
    const double w1 = (-bw + std::sqrt(bw * bw + 4.0 * w0 * w0)) / 2.0;
    return toSections(bilinear(transform(shelf, Band::BandPass, w1, w1 + bw), fs), out, n, 0.0); // unity far from the notch
}

// ---- Section-built effects (Parametric Equalizer, the simple filters, Notch Filter, Bass, Treble) ----------------

enum class Kind : std::uint8_t {
    Peak,      ///< peaking band: frequency, gain, q (or width in Hz)
    LowShelf,  ///< frequency, gain; `order` 1 (6 dB/oct) or 2 (cookbook shelf of q)
    HighShelf, ///< frequency, gain; `order` 1 or 2
    HighPass,  ///< Butterworth of `order`: frequency
    LowPass,   ///< Butterworth of `order`: frequency
    BandPass,  ///< 0 dB at the centre: frequency, q
    FullNotch, ///< frequency, q
    Notch,     ///< a cut or boost of Butterworth order 2 x `count` (notchSections): frequency, gain, q
};

/// Where one filter of an effect takes its settings from: parameter indices (-1: the fixed value instead).
struct SectionSpec {
    Kind kind = Kind::Peak;
    int first = 0; ///< first section in the cascade
    int count = 1; ///< sections it writes
    int order = 2; ///< HighPass / LowPass
    int frequency = -1;
    double fixedFrequency = 1000.0;
    int gain = -1;
    double fixedGain = 0.0;
    int q = -1;
    double fixedQ = 0.70710678118654752;
    int width = -1; ///< Peak: a bandwidth in Hz instead of q (Constant Width)
    std::array<double, 3> last{-1.0, -1.0, -1.0};
};

/// A fixed set of sections (the switches and slopes that decide it are non-animatable), recomputed from the cell's
/// values when they change, and a master gain ramped per sample.
class SectionsProcessor final : public EffectProcessor {
public:
    SectionsProcessor(int channels, int sampleRate, std::vector<SectionSpec> specs, int masterGain)
        : m_specs(std::move(specs)), m_cascade(channels, sectionsOf(m_specs)), m_sampleRate(sampleRate), m_master(masterGain) {
        m_cascade.resize(m_cascade.capacity());
        for (int i = 0; i < m_cascade.size(); ++i) {
            m_cascade.set(i, kIdentity);
        }
    }

    int settleFrames() const noexcept override { return m_settle; }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        update(cell.start);
        if (m_specs.empty() && m_master < 0) {
            return;
        }
        GainRamp gain;
        if (m_master >= 0) {
            gain = GainRamp::ofDb(cell, m_master);
        }
        if (m_cascade.size() == 0 && gain.unity()) {
            return;
        }
        m_cascade.process(io, frames, gain);
    }
    void reset() noexcept override { m_cascade.reset(); }

    /// Computes the sections for `values` (the constructor's build-time values; then every cell).
    void update(const double* v) noexcept {
        const double fs = m_sampleRate;
        for (SectionSpec& s : m_specs) {
            const double f = s.frequency >= 0 ? v[s.frequency] : s.fixedFrequency;
            const double g = s.gain >= 0 ? v[s.gain] : s.fixedGain;
            double q = s.q >= 0 ? v[s.q] : s.fixedQ;
            if (s.width >= 0) {
                q = std::max(f, 1.0) / std::max(v[s.width], 0.1);
            }
            if (s.last[0] == f && s.last[1] == g && s.last[2] == q) {
                continue;
            }
            s.last = {f, g, q};
            BiquadCoefficients* out = m_cascade.data() + s.first;
            switch (s.kind) {
            case Kind::Peak:
                out[0] = g == 0.0 ? kIdentity : dsp::peaking(fs, usableFrequency(f, fs), g, q);
                break;
            case Kind::LowShelf:
            case Kind::HighShelf:
                if (g == 0.0) {
                    out[0] = kIdentity;
                } else if (s.order == 1) {
                    out[0] = firstOrderShelf(fs, f, g, s.kind == Kind::HighShelf);
                } else {
                    out[0] = shelfQ(fs, f, g, q, s.kind == Kind::HighShelf);
                }
                break;
            case Kind::HighPass:
            case Kind::LowPass:
                butterworthSections(out, s.order, s.kind == Kind::HighPass, fs, f);
                break;
            case Kind::BandPass:
                out[0] = bandPass(fs, f, q);
                break;
            case Kind::FullNotch:
                out[0] = fullNotch(fs, f, q);
                break;
            case Kind::Notch:
                if (std::abs(g) < 1e-6) {
                    for (int i = 0; i < s.count; ++i) {
                        out[i] = kIdentity;
                    }
                } else {
                    notchSections(out, s.count, fs, f, g, q);
                }
                break;
            }
        }
    }

    /// Settle time for the build-time settings (set by the factory after the first update()).
    void fixSettle() noexcept { m_settle = m_cascade.settleFrames(m_sampleRate); }

private:
    static int sectionsOf(const std::vector<SectionSpec>& specs) noexcept {
        int n = 0;
        for (const SectionSpec& s : specs) {
            n = std::max(n, s.first + s.count);
        }
        return n;
    }

    std::vector<SectionSpec> m_specs;
    Cascade m_cascade;
    int m_sampleRate;
    int m_master;
    int m_settle = 0;
};

// ---- Graphic Equalizer -------------------------------------------------------------------------------------------

/// One peaking section per band at Premiere's centres (31.25 Hz x 2^(k / bandsPerOctave)), constant Q with a
/// bandwidth of 1.5 band spacings, so that neighbouring bands overlap into a smooth curve. Because the bands overlap,
/// the sections' gains are not the sliders' values: they are solved from the bands' interaction (the response of each
/// band at every centre, normalised by its gain, measured at a 12 dB prototype gain, after V. Valimaki and J. Liski,
/// "Accurate Cascade Graphic Equalizer", IEEE SPL 2017) so that the cascade's gain at each centre is the slider's.
/// Accuracy (Premiere's "points", 10 .. 8192) adds refinement passes that correct for the bands' shape changing with
/// their gain (each pass: the actual response at the centres, the error solved through the same interaction): one per
/// doubling from 125 points, at most four (Premiere's default 1000: three, which meets +-18 dB zigzags within 0.02 dB;
/// none below 177: the linear solution, a few dB off on such patterns). Passes cost only when a slider moves. Bands
/// at or above 0.45 x the sample rate cannot be realised and are left out. All sliders at 0 dB give identity sections
/// (bit-exact).
class GraphicEqProcessor final : public EffectProcessor {
public:
    static constexpr double kPrototypeDb = 12.0;
    static constexpr double kMaxSectionDb = 72.0;

    GraphicEqProcessor(const EffectDescriptor& d, int channels, int sampleRate, std::span<const double> values,
                       int bands, int bandsPerOctave)
        : m_cascade(channels, bands), m_sampleRate(sampleRate), m_master(paramIndex(d, "gain")) {
        const double spacing = 1.0 / bandsPerOctave;
        const double bw = 1.5 * spacing;
        m_q = std::sqrt(std::pow(2.0, bw)) / (std::pow(2.0, bw) - 1.0);
        m_range = std::max(constantOf(d, values, "range"), 0.0);
        const double accuracy = std::max(constantOf(d, values, "accuracy"), 1.0);
        m_refine = std::clamp(static_cast<int>(std::lround(std::log2(accuracy / 125.0))), 0, 4);
        for (int b = 0; b < bands; ++b) {
            const double f = 31.25 * std::pow(2.0, b * spacing);
            if (f >= 0.45 * sampleRate) {
                break;
            }
            const std::string key = "band" + std::to_string(b + 1);
            m_param.push_back(paramIndex(d, key));
            m_centre.push_back(f);
            const double w = 2.0 * kPi * f / sampleRate;
            m_cos1.push_back(std::cos(w));
            m_cos2.push_back(std::cos(2.0 * w));
        }
        const auto n = m_centre.size();
        m_target.assign(n, 0.0);
        m_gain.assign(n, 0.0);
        m_work.assign(n, 0.0);
        m_inverse.assign(n * n, 0.0);
        // Interaction matrix: B[i][j] = dB of band j (at the prototype gain) at centre i, per dB of its gain.
        std::vector<double> b(n * n);
        for (std::size_t j = 0; j < n; ++j) {
            const BiquadCoefficients c = dsp::peaking(sampleRate, m_centre[j], kPrototypeDb, m_q);
            for (std::size_t i = 0; i < n; ++i) {
                b[i * n + j] = 10.0 * std::log10(powerAt(c, m_cos1[i], m_cos2[i])) / kPrototypeDb;
            }
        }
        invert(b, n);
        m_cascade.resize(static_cast<int>(n));
        for (std::size_t j = 0; j < n; ++j) {
            m_cascade.set(static_cast<int>(j), kIdentity);
        }
        // Pre-roll: the slowest band (the lowest) at a strong boost rings the longest.
        double frames = 0.0;
        for (std::size_t j = 0; j < n; ++j) {
            frames = std::max(frames, decayFrames(dsp::peaking(sampleRate, m_centre[j], kMaxSectionDb / 2.0, m_q)));
        }
        m_settle = static_cast<int>(std::clamp(2.0 * frames, 0.01 * sampleRate, 2.0 * sampleRate));
    }

    int settleFrames() const noexcept override { return m_settle; }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        update(cell.start);
        m_cascade.process(io, frames, m_master >= 0 ? GainRamp::ofDb(cell, m_master) : GainRamp{});
    }
    void reset() noexcept override { m_cascade.reset(); }

private:
    /// Gauss-Jordan inverse with partial pivoting (constructor only).
    void invert(std::vector<double>& a, std::size_t n) {
        std::vector<double>& inv = m_inverse;
        for (std::size_t i = 0; i < n; ++i) {
            inv[i * n + i] = 1.0;
        }
        for (std::size_t c = 0; c < n; ++c) {
            std::size_t p = c;
            for (std::size_t r = c + 1; r < n; ++r) {
                if (std::abs(a[r * n + c]) > std::abs(a[p * n + c])) {
                    p = r;
                }
            }
            if (p != c) {
                for (std::size_t k = 0; k < n; ++k) {
                    std::swap(a[p * n + k], a[c * n + k]);
                    std::swap(inv[p * n + k], inv[c * n + k]);
                }
            }
            const double pivot = a[c * n + c];
            if (std::abs(pivot) < 1e-12) {
                continue;
            }
            for (std::size_t k = 0; k < n; ++k) {
                a[c * n + k] /= pivot;
                inv[c * n + k] /= pivot;
            }
            for (std::size_t r = 0; r < n; ++r) {
                if (r != c) {
                    const double f = a[r * n + c];
                    if (f != 0.0) {
                        for (std::size_t k = 0; k < n; ++k) {
                            a[r * n + k] -= f * a[c * n + k];
                            inv[r * n + k] -= f * inv[c * n + k];
                        }
                    }
                }
            }
        }
    }

    /// m_work = inverse x `v`.
    void solve(const std::vector<double>& v) noexcept {
        const std::size_t n = m_centre.size();
        for (std::size_t i = 0; i < n; ++i) {
            double s = 0.0;
            for (std::size_t j = 0; j < n; ++j) {
                s += m_inverse[i * n + j] * v[j];
            }
            m_work[i] = s;
        }
    }

    void setSections() noexcept {
        for (std::size_t j = 0; j < m_centre.size(); ++j) {
            const double g = std::clamp(m_gain[j], -kMaxSectionDb, kMaxSectionDb);
            m_cascade.set(static_cast<int>(j), g == 0.0 ? kIdentity : dsp::peaking(m_sampleRate, m_centre[j], g, m_q));
        }
    }

    void update(const double* v) noexcept {
        const std::size_t n = m_centre.size();
        bool changed = !m_designed;
        bool flat = true;
        const double limit = m_range / 2.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double t = std::clamp(v[m_param[i]], -limit, limit);
            changed = changed || t != m_target[i];
            flat = flat && t == 0.0;
            m_target[i] = t;
        }
        if (!changed) {
            return;
        }
        m_designed = true;
        if (flat) {
            std::fill(m_gain.begin(), m_gain.end(), 0.0);
            setSections();
            return;
        }
        solve(m_target);
        m_gain = m_work;
        setSections();
        for (int pass = 0; pass < m_refine; ++pass) {
            // The error at each centre, solved through the interaction, corrects the gains. The cascade's power at a
            // centre is the product of the sections' (each within 1e+-8: no overflow for 30 of them), one log per centre.
            std::vector<double>& error = m_errorScratch;
            for (std::size_t i = 0; i < n; ++i) {
                double power = 1.0;
                for (std::size_t j = 0; j < n; ++j) {
                    power *= powerAt(m_cascade.at(static_cast<int>(j)), m_cos1[i], m_cos2[i]);
                }
                error[i] = m_target[i] - 10.0 * std::log10(std::max(power, 1e-300));
            }
            solve(error);
            for (std::size_t j = 0; j < n; ++j) {
                m_gain[j] += m_work[j];
            }
            setSections();
        }
    }

    Cascade m_cascade;
    int m_sampleRate;
    int m_master;
    double m_q = 1.0;
    double m_range = 48.0;
    int m_refine = 1;
    int m_settle = 0;
    bool m_designed = false;
    std::vector<int> m_param;
    std::vector<double> m_centre;
    std::vector<double> m_cos1;
    std::vector<double> m_cos2;
    std::vector<double> m_inverse;
    std::vector<double> m_target;
    std::vector<double> m_gain;
    std::vector<double> m_work;
    std::vector<double> m_errorScratch = std::vector<double>(64, 0.0);
};

// ---- Scientific Filter -------------------------------------------------------------------------------------------

enum class FilterType { Bessel, Butterworth, Chebychev, Elliptical };

/// Audition's Transition Bandwidth mode: the lowest order whose response is at least `stopDb` down at the stopband
/// edge(s) - `transition` beyond Cutoff, `highWidth` beyond High Cutoff (inside the band, for a band stop) - while the
/// passband edge keeps its meaning (-3 dB for Butterworth, -ripple for Chebychev and elliptical). Computed on prewarped
/// frequencies, so it holds for the digital filter. A stop edge outside (0, Nyquist) asks for nothing.
int minimumOrder(FilterType type, Band band, double sampleRate, double cutoff, double high, double transition, double highWidth,
                 double rippleDb, double stopDb) noexcept {
    const double fs = sampleRate;
    const auto warp = [fs](double f) { return 2.0 * fs * std::tan(kPi * std::clamp(f, 1.0, 0.49 * fs) / fs); };
    constexpr double kFar = 1e9;
    const double tb = std::max(transition, 1e-3);
    const double hw = std::max(highWidth, 1e-3);
    double omega = kFar; // stopband edge / passband edge of the equivalent low-pass
    switch (band) {
    case Band::LowPass:
        omega = cutoff + tb < 0.49 * fs ? warp(cutoff + tb) / warp(cutoff) : kFar;
        break;
    case Band::HighPass:
        omega = cutoff - tb > 1.0 ? warp(cutoff) / warp(cutoff - tb) : kFar;
        break;
    case Band::BandPass:
    case Band::BandStop: {
        const double lo = std::min(cutoff, high);
        const double hi = std::max(std::max(cutoff, high), lo * 1.01);
        const double w1 = warp(lo);
        const double w2 = warp(hi);
        const double bw = std::max(w2 - w1, 1e-9);
        const auto equivalent = [&](double f) {
            const double w = warp(f);
            const double x = std::abs(w * w - w1 * w2) / (w * bw);
            return band == Band::BandPass ? x : 1.0 / std::max(x, 1e-300);
        };
        if (band == Band::BandPass) {
            omega = std::min(lo - tb > 1.0 ? equivalent(lo - tb) : kFar, hi + hw < 0.49 * fs ? equivalent(hi + hw) : kFar);
        } else {
            const double s1 = std::min(lo + tb, std::sqrt(lo * hi));
            const double s2 = std::max(hi - hw, std::sqrt(lo * hi));
            omega = std::min(equivalent(s1), equivalent(s2));
        }
        break;
    }
    }
    omega = std::max(omega, 1.0 + 1e-9);
    const double passDb = type == FilterType::Butterworth ? 3.0103 : std::max(rippleDb, 1e-4);
    const double d = (std::pow(10.0, stopDb / 10.0) - 1.0) / (std::pow(10.0, passDb / 10.0) - 1.0);
    if (!(d > 1.0)) {
        return 1;
    }
    double n = 1.0;
    if (type == FilterType::Butterworth) {
        n = std::log(d) / (2.0 * std::log(omega));
    } else if (type == FilterType::Chebychev) {
        n = std::acosh(std::sqrt(d)) / std::acosh(omega);
    } else {
        // Elliptical degree equation: n = K(k) K'(k1) / (K'(k) K(k1)), k = 1 / omega, k1 = 1 / sqrt(d).
        const double k = 1.0 / omega;
        const double kp = std::sqrt((1.0 - k) * (1.0 + k));
        const double k1 = 1.0 / std::sqrt(d);
        const double k1p = std::sqrt((1.0 - k1) * (1.0 + k1));
        n = ellipticK(landen(k, kp)) * ellipticK(landen(k1p, k1)) / (ellipticK(landen(kp, k)) * ellipticK(landen(k1, k1p)));
    }
    if (!(n < kMaxPrototypeOrder)) {
        return kMaxPrototypeOrder;
    }
    return std::clamp(static_cast<int>(std::ceil(n - 1e-9)), 1, kMaxPrototypeOrder);
}

class ScientificFilterProcessor final : public EffectProcessor {
public:
    ScientificFilterProcessor(const EffectDescriptor& d, int channels, int sampleRate, std::span<const double> values)
        : m_cascade(channels, kMaxRoots / 2 + 1), m_sampleRate(sampleRate), m_cutoff(paramIndex(d, "cutoff")),
          m_high(paramIndex(d, "highCutoff")), m_gain(paramIndex(d, "gain")) {
        const auto type = static_cast<FilterType>(std::clamp(choiceOf(d, values, "type"), 0, 3));
        m_band = static_cast<Band>(std::clamp(choiceOf(d, values, "mode"), 0, 3));
        const double ripple = constantOf(d, values, "passRipple");
        const double stop = constantOf(d, values, "stopAttenuation");
        int order = std::clamp(choiceOf(d, values, "order"), 1, kMaxPrototypeOrder);
        if (choiceOf(d, values, "orderMode") == 0 && type != FilterType::Bessel) {
            order = minimumOrder(type, m_band, sampleRate, values[static_cast<std::size_t>(m_cutoff)],
                                 values[static_cast<std::size_t>(m_high)], constantOf(d, values, "transitionBandwidth"),
                                 constantOf(d, values, "highWidth"), ripple, stop);
        }
        switch (type) {
        case FilterType::Bessel: m_prototype = besselPrototype(order); break;
        case FilterType::Butterworth: m_prototype = butterworthPrototype(order); break;
        case FilterType::Chebychev: m_prototype = chebyshevPrototype(order, ripple); break;
        case FilterType::Elliptical: m_prototype = ellipticPrototype(order, ripple, stop); break;
        }
        design(values[static_cast<std::size_t>(m_cutoff)], values[static_cast<std::size_t>(m_high)]);
        m_settle = m_cascade.settleFrames(sampleRate);
    }

    int settleFrames() const noexcept override { return m_settle; }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        design(cell.start[m_cutoff], cell.start[m_high]);
        m_cascade.process(io, frames, GainRamp::ofDb(cell, m_gain));
    }
    void reset() noexcept override { m_cascade.reset(); }

private:
    void design(double cutoff, double high) noexcept {
        if (cutoff == m_lastCutoff && high == m_lastHigh) {
            return;
        }
        m_lastCutoff = cutoff;
        m_lastHigh = high;
        const double fs = m_sampleRate;
        const auto warp = [fs](double f) { return 2.0 * fs * std::tan(kPi * std::clamp(f, 1.0, 0.49 * fs) / fs); };
        double w1 = warp(cutoff);
        double w2 = w1;
        if (m_band == Band::BandPass || m_band == Band::BandStop) {
            // The band's edges in either order, at least a 1 % band.
            const double lo = std::clamp(std::min(cutoff, high), 1.0, 0.48 * fs);
            const double hi = std::clamp(std::max(std::max(cutoff, high), lo * 1.01), lo * 1.01, 0.49 * fs);
            w1 = warp(lo);
            w2 = warp(hi);
        }
        const Zpk digital = bilinear(transform(m_prototype, m_band, w1, w2), fs);
        // Each section unity in the passband: at DC (low pass, band stop), Nyquist (high pass) or the band's centre.
        double reference = 0.0;
        if (m_band == Band::HighPass) {
            reference = kPi;
        } else if (m_band == Band::BandPass) {
            reference = 2.0 * std::atan(std::sqrt(w1 * w2) / (2.0 * fs));
        }
        m_cascade.resize(toSections(digital, m_cascade.data(), m_cascade.capacity(), reference));
    }

    Cascade m_cascade;
    int m_sampleRate;
    int m_cutoff;
    int m_high;
    int m_gain;
    Band m_band = Band::LowPass;
    Zpk m_prototype;
    double m_lastCutoff = -1.0;
    double m_lastHigh = -1.0;
    int m_settle = 0;
};

// ---- FFT Filter --------------------------------------------------------------------------------------------------

/// A gain curve applied to the spectrum (dsp::Stft: sqrt-Hann windows, 75 % overlap; zero-phase gains, so the output is
/// the input delayed by the frame size, filtered). Premiere keeps the curve as numbered X/Y pairs; here X is a frequency
/// in Hz and Y a gain in dB, the first Number of X/Y pairs of them, sorted by frequency and joined in log frequency
/// (Logarithmic) or linear frequency, straight or by a monotone cubic (Splines: no overshoot between points), flat
/// beyond the end points. The pairs are animatable: the gains are recomputed in the cell where they change, and the
/// STFT frames that complete from then on use them. Window Type is accepted and changes nothing.
class FftFilterProcessor final : public EffectProcessor {
public:
    static constexpr int kMaxPairs = 12;

    FftFilterProcessor(const EffectDescriptor& d, int channels, int sampleRate, std::span<const double> values)
        : m_stft(std::clamp(channels, 1, dsp::kMaxDspChannels), fftSizeOf(constantOf(d, values, "fftSize"))),
          m_sampleRate(sampleRate), m_logScale(choiceOf(d, values, "scale") == 1), m_spline(choiceOf(d, values, "splines") != 0),
          m_pairs(std::clamp(choiceOf(d, values, "pairs"), 2, kMaxPairs)) {
        for (int i = 0; i < kMaxPairs; ++i) {
            m_x[static_cast<std::size_t>(i)] = paramIndex(d, "x" + std::to_string(i + 1));
            m_y[static_cast<std::size_t>(i)] = paramIndex(d, "y" + std::to_string(i + 1));
        }
        m_bins.assign(static_cast<std::size_t>(m_stft.bins()), 1.0);
        m_last.fill(-1e300);
    }

    /// Premiere's FFT Size (32 .. 32768) as the STFT's power of two (64 .. 32768).
    static int fftSizeOf(double size) noexcept {
        int n = 64;
        while (n < 32768 && n < size) {
            n *= 2;
        }
        return n;
    }

    int latencyFrames() const noexcept override { return m_stft.latencyFrames(); }
    int settleFrames() const noexcept override { return m_stft.latencyFrames(); }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        std::array<double, 2 * kMaxPairs> now{};
        for (int i = 0; i < m_pairs; ++i) {
            now[static_cast<std::size_t>(2 * i)] = cell.start[m_x[static_cast<std::size_t>(i)]];
            now[static_cast<std::size_t>(2 * i + 1)] = cell.start[m_y[static_cast<std::size_t>(i)]];
        }
        if (now != m_last) {
            m_last = now;
            curve();
        }
        m_stft.process(io, frames, [this](int, std::complex<double>* bins) {
            for (std::size_t k = 0; k < m_bins.size(); ++k) {
                bins[k] *= m_bins[k];
            }
        });
    }
    void reset() noexcept override { m_stft.reset(); }

private:
    /// The bins' gains from the pairs in m_last.
    void curve() noexcept {
        const int n = m_pairs;
        std::array<std::pair<double, double>, kMaxPairs> pts{};
        for (int i = 0; i < n; ++i) {
            pts[static_cast<std::size_t>(i)] = {std::max(m_last[static_cast<std::size_t>(2 * i)], 1.0),
                                                m_last[static_cast<std::size_t>(2 * i + 1)]};
        }
        std::stable_sort(pts.begin(), pts.begin() + n, [](const auto& a, const auto& b) { return a.first < b.first; });
        std::array<double, kMaxPairs> x{};
        std::array<double, kMaxPairs> y{};
        for (int i = 0; i < n; ++i) {
            const auto& p = pts[static_cast<std::size_t>(i)];
            x[static_cast<std::size_t>(i)] = m_logScale ? std::log2(p.first) : p.first;
            y[static_cast<std::size_t>(i)] = p.second;
        }
        // Fritsch-Carlson tangents: the harmonic mean of the neighbouring slopes, 0 at a turning point.
        std::array<double, kMaxPairs> slope{};
        std::array<double, kMaxPairs> tangent{};
        for (int i = 0; i + 1 < n; ++i) {
            const double dx = x[static_cast<std::size_t>(i + 1)] - x[static_cast<std::size_t>(i)];
            slope[static_cast<std::size_t>(i)] = dx > 1e-12 ? (y[static_cast<std::size_t>(i + 1)] - y[static_cast<std::size_t>(i)]) / dx : 0.0;
        }
        tangent[0] = slope[0];
        tangent[static_cast<std::size_t>(n - 1)] = slope[static_cast<std::size_t>(n - 2)];
        for (int i = 1; i + 1 < n; ++i) {
            const double a = slope[static_cast<std::size_t>(i - 1)];
            const double b = slope[static_cast<std::size_t>(i)];
            tangent[static_cast<std::size_t>(i)] = a * b <= 0.0 ? 0.0 : 2.0 / (1.0 / a + 1.0 / b);
        }
        const double binHz = static_cast<double>(m_sampleRate) / m_stft.size();
        int segment = 0;
        for (std::size_t k = 0; k < m_bins.size(); ++k) {
            const double f = std::max(static_cast<double>(k) * binHz, 1.0);
            const double t = m_logScale ? std::log2(f) : f;
            double db = 0.0;
            if (t <= x[0]) {
                db = y[0];
            } else if (t >= x[static_cast<std::size_t>(n - 1)]) {
                db = y[static_cast<std::size_t>(n - 1)];
            } else {
                while (segment + 2 < n && t > x[static_cast<std::size_t>(segment + 1)]) {
                    ++segment;
                }
                const auto i = static_cast<std::size_t>(segment);
                const double h = x[i + 1] - x[i];
                const double s = h > 1e-12 ? (t - x[i]) / h : 1.0;
                if (m_spline) {
                    const double s2 = s * s;
                    const double s3 = s2 * s;
                    db = (2.0 * s3 - 3.0 * s2 + 1.0) * y[i] + (s3 - 2.0 * s2 + s) * h * tangent[i] + (-2.0 * s3 + 3.0 * s2) * y[i + 1] +
                         (s3 - s2) * h * tangent[i + 1];
                } else {
                    db = y[i] + (y[i + 1] - y[i]) * s;
                }
            }
            m_bins[k] = dbToGain(db);
        }
    }

    dsp::Stft m_stft;
    int m_sampleRate;
    bool m_logScale;
    bool m_spline;
    int m_pairs;
    std::array<int, kMaxPairs> m_x{};
    std::array<int, kMaxPairs> m_y{};
    std::vector<double> m_bins;
    std::array<double, 2 * kMaxPairs> m_last{};
};

// ---- Stereo Expander and the channel effects ---------------------------------------------------------------------

/// Stereo Expander on channels 0/1 (mono and the others untouched): mid M = (L + R) / 2 placed by Center Channel Pan
/// (an equal-power pan normalised to unity at the centre: the mid is unchanged at 0, sqrt(2) on one side at +-100 %),
/// side S = (L - R) / 2 scaled by Stereo Expander / 100 (Premiere's panel: Narrow 0, Normal 100 - its default -, Wide
/// 300).
class StereoExpanderProcessor final : public EffectProcessor {
public:
    StereoExpanderProcessor(const EffectDescriptor& d, int channels)
        : m_channels(channels), m_pan(paramIndex(d, "centerPan")), m_expand(paramIndex(d, "stereoExpand")) {}

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        if (m_channels < 2) {
            return;
        }
        if (cell.start[m_pan] == 0.0 && cell.end[m_pan] == 0.0 && cell.start[m_expand] == 100.0 && cell.end[m_expand] == 100.0) {
            return; // the defaults: exactly the input
        }
        const auto ch = static_cast<std::size_t>(m_channels);
        for (int i = 0; i < frames; ++i) {
            const double pan = std::clamp(rampAt(cell, m_pan, i) / 100.0, -1.0, 1.0);
            const double width = std::max(rampAt(cell, m_expand, i), 0.0) / 100.0;
            const double angle = (pan + 1.0) * kPi / 4.0;
            const double gl = pan == 0.0 ? 1.0 : std::numbers::sqrt2 * std::cos(angle);
            const double gr = pan == 0.0 ? 1.0 : std::numbers::sqrt2 * std::sin(angle);
            float* frame = io + static_cast<std::size_t>(i) * ch;
            const double mid = 0.5 * (static_cast<double>(frame[0]) + frame[1]);
            const double side = 0.5 * (static_cast<double>(frame[0]) - frame[1]) * width;
            frame[0] = static_cast<float>(gl * mid + side);
            frame[1] = static_cast<float>(gr * mid - side);
        }
    }
    void reset() noexcept override {}

private:
    int m_channels;
    int m_pan;
    int m_expand;
};

enum class ChannelOp : std::uint8_t { Balance, Volume, FillLeft, FillRight, Invert, Swap };

/// The stateless channel effects (Balance, Channel Volume, Fill Left with Right, Fill Right with Left, Invert, Swap
/// Channels): gains ramp per sample across a cell.
class ChannelProcessor final : public EffectProcessor {
public:
    ChannelProcessor(const EffectDescriptor& d, int channels, ChannelOp op)
        : m_channels(channels), m_op(op), m_balance(paramIndex(d, "balance")) {
        constexpr std::array<std::string_view, 6> kVolumeKeys{"left", "right", "center", "lfe", "leftSurround", "rightSurround"};
        for (std::size_t c = 0; c < kVolumeKeys.size(); ++c) {
            m_volume[c] = paramIndex(d, kVolumeKeys[c]);
        }
    }

    void process(float* io, int frames, const ParamCell& cell) noexcept override {
        const auto ch = static_cast<std::size_t>(m_channels);
        switch (m_op) {
        case ChannelOp::Invert:
            for (std::size_t i = 0; i < static_cast<std::size_t>(frames) * ch; ++i) {
                io[i] = -io[i];
            }
            return;
        case ChannelOp::Volume: {
            // Left, Right, Center, LFE, Left Surround, Right Surround on channels 0..5 (the engine's 5.1 order); a mono
            // clip's one channel takes Left, channels after the sixth are untouched.
            const std::size_t used = std::min<std::size_t>(ch, m_volume.size());
            for (std::size_t c = 0; c < used; ++c) {
                const GainRamp gain = GainRamp::ofDb(cell, m_volume[c]);
                if (gain.unity()) {
                    continue;
                }
                for (int i = 0; i < frames; ++i) {
                    float& v = io[static_cast<std::size_t>(i) * ch + c];
                    v = static_cast<float>(v * gain.at(i));
                }
            }
            return;
        }
        default:
            break;
        }
        if (ch < 2) {
            return; // Balance, Fill and Swap need a pair: a mono clip is unchanged
        }
        for (int i = 0; i < frames; ++i) {
            float* frame = io + static_cast<std::size_t>(i) * ch;
            switch (m_op) {
            case ChannelOp::Balance: {
                const double b = std::clamp(rampAt(cell, m_balance, i) / 100.0, -1.0, 1.0);
                if (b > 0.0) {
                    frame[0] = static_cast<float>(frame[0] * (1.0 - b));
                } else if (b < 0.0) {
                    frame[1] = static_cast<float>(frame[1] * (1.0 + b));
                }
                break;
            }
            case ChannelOp::FillLeft: frame[0] = frame[1]; break;
            case ChannelOp::FillRight: frame[1] = frame[0]; break;
            case ChannelOp::Swap: std::swap(frame[0], frame[1]); break;
            default: break;
            }
        }
    }
    void reset() noexcept override {}

private:
    int m_channels;
    ChannelOp m_op;
    int m_balance;
    std::array<int, 6> m_volume{};
};

} // namespace

bool isFilterEffect(std::string_view id) noexcept {
    return std::find(kIds.begin(), kIds.end(), id) != kIds.end();
}

std::unique_ptr<EffectProcessor> createFilterProcessor(const EffectDescriptor& d, int channels, int sampleRate,
                                                       std::span<const double> values) {
    channels = std::clamp(channels, 1, dsp::kMaxDspChannels);
    sampleRate = std::max(sampleRate, 8000);
    const auto index = [&d](std::string_view key) { return paramIndex(d, key); };
    const auto sections = [&](std::vector<SectionSpec> specs, int master) -> std::unique_ptr<EffectProcessor> {
        auto p = std::make_unique<SectionsProcessor>(channels, sampleRate, std::move(specs), master);
        p->update(values.data());
        p->fixSettle();
        return p;
    };
    if (d.id == kGraphicEq10) {
        return std::make_unique<GraphicEqProcessor>(d, channels, sampleRate, values, 10, 1);
    }
    if (d.id == kGraphicEq20) {
        return std::make_unique<GraphicEqProcessor>(d, channels, sampleRate, values, 20, 2);
    }
    if (d.id == kGraphicEq30) {
        return std::make_unique<GraphicEqProcessor>(d, channels, sampleRate, values, 30, 3);
    }
    if (d.id == kParametricEq) {
        // Enables, slopes, shelf orders and Constant Q / Constant Width are switches: they decide the sections.
        std::vector<SectionSpec> specs;
        int next = 0;
        const bool constantWidth = choiceOf(d, values, "constantQ") == 0;
        for (const bool high : {true, false}) {
            const std::string prefix = high ? "highPass" : "lowPass";
            if (choiceOf(d, values, prefix + "Enabled") != 0) {
                SectionSpec s;
                s.kind = high ? Kind::HighPass : Kind::LowPass;
                s.order = std::clamp(choiceOf(d, values, prefix + "Slope"), 1, 8); // the order: 6 dB/oct each
                s.first = next;
                s.count = s.order / 2 + s.order % 2;
                s.frequency = index(prefix + "Frequency");
                next += s.count;
                specs.push_back(s);
            }
        }
        for (const bool high : {false, true}) {
            const std::string prefix = high ? "highShelf" : "lowShelf";
            if (choiceOf(d, values, prefix + "Enabled") != 0) {
                SectionSpec s;
                s.kind = high ? Kind::HighShelf : Kind::LowShelf;
                s.order = std::clamp(choiceOf(d, values, prefix + "Order"), 1, 2);
                s.first = next++;
                s.frequency = index(prefix + "Frequency");
                s.gain = index(prefix + "Gain");
                specs.push_back(s);
            }
        }
        for (int b = 1; b <= 5; ++b) {
            const std::string prefix = "band" + std::to_string(b);
            if (choiceOf(d, values, prefix + "Enabled") != 0) {
                SectionSpec s;
                s.kind = Kind::Peak;
                s.first = next++;
                s.frequency = index(prefix + "Frequency");
                s.gain = index(prefix + "Gain");
                if (constantWidth) {
                    s.width = index(prefix + "Bandwidth");
                } else {
                    s.q = index(prefix + "Q");
                }
                specs.push_back(s);
            }
        }
        return sections(std::move(specs), index("gain"));
    }
    if (d.id == kSimpleParametricEq) {
        SectionSpec s;
        s.kind = Kind::Peak;
        s.frequency = index("center");
        s.q = index("q");
        s.gain = index("boost");
        return sections({s}, -1);
    }
    if (d.id == kBandpass || d.id == kSimpleNotch) {
        SectionSpec s;
        s.kind = d.id == kBandpass ? Kind::BandPass : Kind::FullNotch;
        s.frequency = index("cutoff");
        s.q = index("q");
        return sections({s}, -1);
    }
    if (d.id == kHighpass || d.id == kLowpass) {
        // Order unverified against Premiere: second-order Butterworth.
        SectionSpec s;
        s.kind = d.id == kHighpass ? Kind::HighPass : Kind::LowPass;
        s.order = 2;
        s.frequency = index("cutoff");
        return sections({s}, -1);
    }
    if (d.id == kBass || d.id == kTreble) {
        SectionSpec s;
        s.kind = d.id == kBass ? Kind::LowShelf : Kind::HighShelf;
        s.fixedFrequency = d.id == kBass ? 200.0 : 4000.0;
        s.gain = index("boost");
        return sections({s}, -1);
    }
    if (d.id == kNotchFilter) {
        // Notch Width: 0 Narrow (2nd order, -3 dB edges a 1/8 of the frequency apart), 1 Very Narrow (4th order, 1/16),
        // 2 Super Narrow (6th order, 1/32).
        const int width = std::clamp(choiceOf(d, values, "notchWidth"), 0, 2);
        const int perNotch = width + 1;
        constexpr std::array<double, 3> kQ{8.0, 16.0, 32.0};
        const bool fixed = choiceOf(d, values, "fixGains") != 0;
        std::vector<SectionSpec> specs;
        int next = 0;
        for (int n = 1; n <= 6; ++n) {
            const std::string prefix = "notch" + std::to_string(n);
            if (choiceOf(d, values, prefix + "Enabled") == 0) {
                continue;
            }
            SectionSpec s;
            s.kind = Kind::Notch;
            s.first = next;
            s.count = perNotch;
            next += perNotch;
            s.frequency = index(prefix + "Frequency");
            s.gain = fixed ? index("fixedGain") : index(prefix + "Gain");
            s.fixedQ = kQ[static_cast<std::size_t>(width)];
            specs.push_back(s);
        }
        return sections(std::move(specs), -1);
    }
    if (d.id == kScientificFilter) {
        return std::make_unique<ScientificFilterProcessor>(d, channels, sampleRate, values);
    }
    if (d.id == kFftFilter) {
        return std::make_unique<FftFilterProcessor>(d, channels, sampleRate, values);
    }
    if (d.id == kStereoExpander) {
        return std::make_unique<StereoExpanderProcessor>(d, channels);
    }
    if (d.id == kBalance) {
        return std::make_unique<ChannelProcessor>(d, channels, ChannelOp::Balance);
    }
    if (d.id == kChannelVolume) {
        return std::make_unique<ChannelProcessor>(d, channels, ChannelOp::Volume);
    }
    if (d.id == kFillLeft) {
        return std::make_unique<ChannelProcessor>(d, channels, ChannelOp::FillLeft);
    }
    if (d.id == kFillRight) {
        return std::make_unique<ChannelProcessor>(d, channels, ChannelOp::FillRight);
    }
    if (d.id == kInvert) {
        return std::make_unique<ChannelProcessor>(d, channels, ChannelOp::Invert);
    }
    if (d.id == kSwapChannels) {
        return std::make_unique<ChannelProcessor>(d, channels, ChannelOp::Swap);
    }
    return nullptr;
}

} // namespace wabcut::audio::premiere
