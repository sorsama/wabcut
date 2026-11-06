#include <wabcut/audio/dsp/SpectralRepair.h>

#include <wabcut/audio/dsp/Fft.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>

namespace wabcut::audio::dsp {

namespace {

using Complex = std::complex<double>;
using ComplexF = std::complex<float>;

constexpr double kTwoPi = 2.0 * std::numbers::pi;
/// A steady random-phase fill reads this much below the rms it stands for when the log-levels of Rayleigh-distributed bins are
/// averaged (the geometric mean of noise is 3.9 dB under its rms); a little less than the exact figure because the sound
/// around a hole is not always noise.
constexpr double kLogLevelBias = 1.4;
/// The lower quartile of Rayleigh-distributed magnitudes is 0.536 of their rms.
constexpr double kQuartileToRms = 1.87;
constexpr double kMaxReachSeconds = 1.0;
constexpr int kFrequencyReach = 96; ///< bins looked at above and below a hole
constexpr int kToneCellFrames = 32;
constexpr double kToneRampSeconds = 0.02;

std::int64_t floorDiv(std::int64_t a, std::int64_t b) noexcept {
    const std::int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

std::int64_t floorMod(std::int64_t a, std::int64_t b) noexcept { return a - floorDiv(a, b) * b; }

double smooth01(double u) noexcept {
    u = std::clamp(u, 0.0, 1.0);
    return u * u * (3.0 - 2.0 * u);
}

std::uint64_t mix64(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

/// A fixed phase for a bin of a frame (a function of the absolute frame number, so every render agrees).
double fillPhase(std::int64_t frame, int bin, int channel, int nyquistBin) noexcept {
    const std::uint64_t h = mix64(static_cast<std::uint64_t>(frame) * 0x9E3779B97F4A7C15ULL ^
                                  (static_cast<std::uint64_t>(bin) << 20) ^ (static_cast<std::uint64_t>(channel) << 52));
    if (bin == 0 || bin == nyquistBin) {
        return (h & 1U) != 0 ? std::numbers::pi : 0.0; // the DC and Nyquist bins are real
    }
    return static_cast<double>(h >> 11) * (kTwoPi / 9007199254740992.0);
}

int snapFftSize(int n) noexcept { return n < 1536 ? 1024 : n < 3072 ? 2048 : 4096; }

double segmentDistance2(double px, double py, double ax, double ay, double bx, double by) noexcept {
    const double vx = bx - ax;
    const double vy = by - ay;
    const double wx = px - ax;
    const double wy = py - ay;
    const double len2 = vx * vx + vy * vy;
    const double t = len2 > 0.0 ? std::clamp((wx * vx + wy * vy) / len2, 0.0, 1.0) : 0.0;
    const double dx = wx - t * vx;
    const double dy = wy - t * vy;
    return dx * dx + dy * dy;
}

/// One region edit (Attenuate, Heal, NoiseFloor) ready to evaluate: its shape in the analysis' resolution.
class Region {
public:
    Region(const SpectralEdit& e, int fftSize, int sampleRate) {
        kind = e.kind;
        shape = e.shape;
        logFrequency = e.logFrequency;
        gain = std::pow(10.0, e.gainDb / 20.0);
        amount = e.amount / 100.0;
        wt = (static_cast<double>(fftSize) / 4.0) / sampleRate;
        wf = 2.0 * sampleRate / fftSize;
        switch (shape) {
        case SpectralShape::Rectangle: {
            t0 = e.start;
            t1 = e.end;
            f0 = e.low;
            f1 = e.high;
            if (t1 - t0 < 2.0 * wt) {
                const double mid = 0.5 * (t0 + t1);
                t0 = mid - wt;
                t1 = mid + wt;
            }
            if (f1 - f0 < 4.0 * wf) {
                const double mid = 0.5 * (f0 + f1);
                f0 = std::max(1.0, mid - 2.0 * wf);
                f1 = mid + 2.0 * wf;
            }
            box = {t0 - wt, t1 + wt, std::max(1.0, f0 - wf), f1 + wf};
            break;
        }
        case SpectralShape::Lasso: {
            sigmaT = wt;
            sigmaG = logFrequency ? 0.1 : wf;
            for (const SpectralPoint& p : e.path) {
                u.push_back(p.time / sigmaT);
                v.push_back(toG(p.hz) / sigmaG);
            }
            const SpectralBounds b = spectralBounds(e);
            box = {b.start - wt, b.end + wt, logFrequency ? b.low * std::exp2(-0.15) : std::max(1.0, b.low - wf),
                   logFrequency ? b.high * std::exp2(0.15) : b.high + wf};
            break;
        }
        case SpectralShape::Brush: {
            radT = std::max(e.radiusSeconds, wt * 0.75);
            radG = std::max(e.radiusOctaves, 0.05);
            soft = std::max(0.5 * wt / radT, 0.12);
            for (const SpectralPoint& p : e.path) {
                u.push_back(p.time / radT);
                v.push_back(std::log2(std::max(p.hz, 1.0)) / radG);
            }
            SpectralEdit shifted = e;
            shifted.radiusSeconds = radT * (1.0 + soft);
            shifted.radiusOctaves = radG * (1.0 + soft);
            box = spectralBounds(shifted);
            break;
        }
        }
        // The seconds the correction is applied over (at least a millisecond).
        const SpectralBounds seconds = spectralBounds(e);
        gate0 = seconds.start;
        gate1 = std::max(seconds.end, seconds.start + 1.0e-3);
        fade = std::clamp(0.25 * (gate1 - gate0), 5.0e-4, 5.0e-3);
    }

    SpectralKind kind = SpectralKind::Attenuate;
    SpectralShape shape = SpectralShape::Rectangle;
    double gain = 1.0;
    double amount = 1.0;
    SpectralBounds box;

    bool damaging() const noexcept { return kind == SpectralKind::Heal || kind == SpectralKind::NoiseFloor; }
    bool coversTime(double tau) const noexcept { return tau >= box.start && tau <= box.end; }
    /// Whether a frame whose window spans [tau - half, tau + half] reaches the region.
    bool overlaps(double tau, double half) const noexcept { return tau + half >= box.start && tau - half <= box.end; }

    /// The time envelope an edit's correction is gated with: 1 inside the region, 0 outside, a short crossfade at its edges.
    /// An STFT frame is 40 ms long, but a repair acts on the seconds the user selected, not a window's reach beyond them.
    double gateAt(double tau) const noexcept { return smooth01((tau - gate0) / fade + 0.5) * smooth01((gate1 - tau) / fade + 0.5); }

    /// The strongest coverage the shape has at `hz` anywhere in a frame's window: every frame that reaches a rectangle takes
    /// its band in full (the time edges are the gate's job), a lasso or brush is looked at across the window.
    double frameCoverage(double tau, double half, double hz) const noexcept {
        if (!overlaps(tau, half) || hz < box.low || hz > box.high) {
            return 0.0;
        }
        if (shape == SpectralShape::Rectangle) {
            return smooth01((hz - f0) / wf + 0.5) * smooth01((f1 - hz) / wf + 0.5);
        }
        double best = 0.0;
        for (const double k : {0.0, -0.5, 0.5, -1.0, 1.0}) {
            best = std::max(best, coverage(tau + k * half, hz));
        }
        return best;
    }

    double coverage(double tau, double hz) const noexcept {
        if (tau < box.start || tau > box.end || hz < box.low || hz > box.high) {
            return 0.0;
        }
        switch (shape) {
        case SpectralShape::Rectangle: {
            const double a = smooth01((tau - t0) / wt + 0.5) * smooth01((t1 - tau) / wt + 0.5);
            const double b = smooth01((hz - f0) / wf + 0.5) * smooth01((f1 - hz) / wf + 0.5);
            return a * b;
        }
        case SpectralShape::Lasso: {
            const double px = tau / sigmaT;
            const double py = toG(hz) / sigmaG;
            const std::size_t n = u.size();
            bool inside = false;
            double best = std::numeric_limits<double>::max();
            for (std::size_t i = 0; i < n; ++i) {
                const std::size_t j = (i + 1) % n;
                if ((v[i] > py) != (v[j] > py)) {
                    const double x = u[i] + (py - v[i]) * (u[j] - u[i]) / (v[j] - v[i]);
                    if (px < x) {
                        inside = !inside;
                    }
                }
                best = std::min(best, segmentDistance2(px, py, u[i], v[i], u[j], v[j]));
            }
            const double d = std::sqrt(best);
            return smooth01(0.5 + (inside ? d : -d));
        }
        case SpectralShape::Brush: {
            const double px = tau / radT;
            const double py = std::log2(std::max(hz, 1.0)) / radG;
            double best = std::numeric_limits<double>::max();
            const std::size_t n = u.size();
            if (n == 1) {
                best = segmentDistance2(px, py, u[0], v[0], u[0], v[0]);
            }
            for (std::size_t i = 0; i + 1 < n; ++i) {
                best = std::min(best, segmentDistance2(px, py, u[i], v[i], u[i + 1], v[i + 1]));
            }
            const double d = std::sqrt(best) - 1.0;
            return 1.0 - smooth01((d + soft) / (2.0 * soft));
        }
        }
        return 0.0;
    }

private:
    double toG(double hz) const noexcept { return logFrequency ? std::log2(std::max(hz, 1.0)) : hz; }

    bool logFrequency = true;
    double wt = 0.01;
    double wf = 40.0;
    double gate0 = 0.0, gate1 = 0.0, fade = 1.0e-3;
    double t0 = 0.0, t1 = 0.0, f0 = 0.0, f1 = 0.0;
    double sigmaT = 1.0, sigmaG = 1.0;
    double radT = 1.0, radG = 1.0, soft = 0.12;
    std::vector<double> u, v; ///< a lasso's or a stroke's points in its own units
};

/// One Remove Tone edit: the trace's phase and the filters of its harmonics.
struct Tone {
    struct Lowpass {
        double r1 = 0.0, i1 = 0.0, r2 = 0.0, i2 = 0.0;
    };

    Tone(const SpectralEdit& e, int channels, int sampleRate) {
        start = e.start;
        end = e.end;
        depth = 1.0 - std::pow(10.0, e.gainDb / 20.0);
        std::vector<SpectralPoint> trace = e.path;
        std::stable_sort(trace.begin(), trace.end(), [](const SpectralPoint& a, const SpectralPoint& b) { return a.time < b.time; });
        double top = 0.0;
        for (const SpectralPoint& p : trace) {
            if (!t.empty() && p.time <= t.back()) {
                continue; // one frequency at a time
            }
            t.push_back(p.time);
            f.push_back(p.hz);
            top = std::max(top, p.hz);
        }
        cum.assign(t.size(), 0.0);
        for (std::size_t i = 1; i < t.size(); ++i) {
            cum[i] = cum[i - 1] + 0.5 * (f[i - 1] + f[i]) * (t[i] - t[i - 1]);
        }
        harmonics = top > 0.0 ? std::min(e.harmonics, static_cast<int>(std::floor(0.45 * sampleRate / top))) : 0;
        const double cutoff = 0.5 * e.widthHz;
        a = 1.0 - std::exp(-kTwoPi * cutoff / sampleRate);
        // The filters' memory dies away as exp(-t / tau) (two stages in a row): twenty time constants leave under a billionth.
        warmup = std::min(20.0 / (kTwoPi * cutoff), 0.6);
        state.assign(static_cast<std::size_t>(std::max(harmonics, 0)) * static_cast<std::size_t>(channels), Lowpass{});
    }

    bool usable() const noexcept { return harmonics > 0 && !t.empty(); }

    /// Cycles of the fundamental at source time `tau` (the integral of the trace).
    double cycles(double tau) const noexcept {
        const std::size_t n = t.size();
        if (n == 1 || tau <= t[0]) {
            return f[0] * (tau - t[0]);
        }
        if (tau >= t[n - 1]) {
            return cum[n - 1] + f[n - 1] * (tau - t[n - 1]);
        }
        const std::size_t i = static_cast<std::size_t>(std::upper_bound(t.begin(), t.end(), tau) - t.begin()) - 1;
        const double d = tau - t[i];
        const double k = (f[i + 1] - f[i]) / (t[i + 1] - t[i]);
        return cum[i] + f[i] * d + 0.5 * k * d * d;
    }

    double start = 0.0, end = 0.0, depth = 0.0, a = 0.1, warmup = 0.0;
    int harmonics = 0;
    bool running = false;
    std::vector<double> t, f, cum;
    std::vector<Lowpass> state;
};

/// The gain that makes a random-phase fill overlap-add to the level its magnitudes ask for: a flat unit-magnitude spectrum
/// with the fill's phases is synthesised and overlap-added exactly as the repair does, and compared with the white noise
/// of the same bin power.
double randomPhaseGain(int fftSize) {
    Fft fft(fftSize);
    const int hop = fftSize / 4;
    const int bins = fft.bins();
    std::vector<double> window(static_cast<std::size_t>(fftSize));
    for (int i = 0; i < fftSize; ++i) {
        window[static_cast<std::size_t>(i)] =
            std::sqrt(0.5 - 0.5 * std::cos(kTwoPi * i / static_cast<double>(fftSize)));
    }
    double overlap = 0.0;
    for (int i = 0; i < fftSize; i += hop) {
        overlap += window[static_cast<std::size_t>(i)] * window[static_cast<std::size_t>(i)];
    }
    const double scale = overlap > 0.0 ? 1.0 / overlap : 1.0;
    constexpr int kFrames = 64;
    std::vector<double> out(static_cast<std::size_t>(kFrames * hop + fftSize), 0.0);
    std::vector<Complex> spectrum(static_cast<std::size_t>(bins));
    std::vector<double> frame(static_cast<std::size_t>(fftSize));
    for (int j = 0; j < kFrames; ++j) {
        for (int b = 0; b < bins; ++b) {
            spectrum[static_cast<std::size_t>(b)] = std::polar(1.0, fillPhase(j, b, 0, bins - 1));
        }
        fft.inverseReal(spectrum.data(), frame.data());
        for (int i = 0; i < fftSize; ++i) {
            out[static_cast<std::size_t>(j * hop + i)] +=
                frame[static_cast<std::size_t>(i)] * window[static_cast<std::size_t>(i)] * scale;
        }
    }
    double sum = 0.0;
    int count = 0;
    for (int i = fftSize; i < (kFrames - 4) * hop; ++i) {
        const double v = out[static_cast<std::size_t>(i)];
        sum += v * v;
        ++count;
    }
    const double measured = count > 0 ? std::sqrt(sum / count) : 0.0;
    const double reference = std::sqrt(2.0 / fftSize); // white noise whose bins have unit power
    return measured > 0.0 ? reference / measured : 2.0;
}

double randomPhaseGainFor(int fftSize) {
    static const double g1024 = randomPhaseGain(1024);
    static const double g2048 = randomPhaseGain(2048);
    static const double g4096 = randomPhaseGain(4096);
    return fftSize == 1024 ? g1024 : fftSize == 2048 ? g2048 : g4096;
}

std::int64_t powerOfTwoAtLeast(std::int64_t n) noexcept {
    std::int64_t p = 2;
    while (p < n) {
        p *= 2;
    }
    return p;
}

} // namespace

struct SpectralRepair::Impl {
    struct FrameInfo {
        bool analysed = false;
        std::int64_t index = std::numeric_limits<std::int64_t>::min();
        double tau = 0.0;      ///< source seconds at the centre of the frame's window
        double tauStart = 0.0; ///< ... at its first and at its last sample
        double tauEnd = 0.0;
        double half = 0.0;     ///< half the window, in source seconds
        double ratio = 1.0;
    };

    Impl(int channelCount, int rate, std::span<const SpectralEdit> edits, const SpectralRepairOptions& options)
        : channels(std::clamp(channelCount, 1, kMaxChannels)), sampleRate(std::max(rate, 8000)),
          fftSize(snapFftSize(options.fftSize)), listen(options.listen), fft(fftSize) {
        hop = fftSize / 4;
        bins = fft.bins();
        binHz = static_cast<double>(sampleRate) / fftSize;
        window.resize(static_cast<std::size_t>(fftSize));
        for (int i = 0; i < fftSize; ++i) {
            window[static_cast<std::size_t>(i)] = std::sqrt(0.5 - 0.5 * std::cos(kTwoPi * i / static_cast<double>(fftSize)));
        }
        double overlap = 0.0;
        for (int i = 0; i < fftSize; i += hop) {
            overlap += window[static_cast<std::size_t>(i)] * window[static_cast<std::size_t>(i)];
        }
        scale = overlap > 0.0 ? 1.0 / overlap : 1.0;

        double longest = 0.0;
        std::size_t taken = 0;
        for (const SpectralEdit& e : edits) {
            if (taken >= kMaxSpectralEdits || spectralEditProblem(e)) {
                continue;
            }
            ++taken;
            if (e.kind == SpectralKind::Tone) {
                Tone tone(e, channels, sampleRate);
                if (tone.usable()) {
                    tones.push_back(std::move(tone));
                }
            } else {
                regions.emplace_back(e, fftSize, sampleRate);
                if (regions.back().damaging()) {
                    healing = true;
                    longest = std::max(longest, regions.back().box.end - regions.back().box.start);
                }
            }
        }
        hasMask = !regions.empty();
        hasTone = !tones.empty();

        if (hasMask) {
            const double reach = healing ? std::clamp(longest + 0.12, 0.12, kMaxReachSeconds) : 0.0;
            context = healing ? std::max(4, static_cast<int>(std::ceil(reach * sampleRate / hop))) : 0;
            latency = fftSize + context * hop;
            slots = 2 * context + 4;
            fillGain = randomPhaseGainFor(fftSize);
            info.assign(static_cast<std::size_t>(slots), FrameInfo{});
            spec.assign(static_cast<std::size_t>(channels) * static_cast<std::size_t>(slots) * static_cast<std::size_t>(bins), ComplexF{});
            damage.assign(static_cast<std::size_t>(slots) * static_cast<std::size_t>(bins), 0);
            work.assign(static_cast<std::size_t>(channels) * static_cast<std::size_t>(bins), Complex{});
            coverageRow.assign(static_cast<std::size_t>(bins), 0.0);
            delta.assign(static_cast<std::size_t>(bins), Complex{});
            gateRow.assign(static_cast<std::size_t>(fftSize), 0.0);
            timeBuffer.assign(static_cast<std::size_t>(fftSize), 0.0);
            floorScratch.assign(static_cast<std::size_t>(3 * (2 * std::min(context, 48) + 1) + 8), 0.0);
            accMask = powerOfTwoAtLeast(2LL * fftSize + 4LL * hop) - 1;
            acc.assign(static_cast<std::size_t>(channels), std::vector<double>(static_cast<std::size_t>(accMask + 1), 0.0));
        }
        const std::int64_t span = static_cast<std::int64_t>(latency) + fftSize + 4LL * hop;
        inMask = powerOfTwoAtLeast(std::max<std::int64_t>(span, 4096)) - 1;
        inRing.assign(static_cast<std::size_t>(channels), std::vector<float>(static_cast<std::size_t>(inMask + 1), 0.0f));
        if (hasTone) {
            toneRing.assign(static_cast<std::size_t>(channels), std::vector<float>(static_cast<std::size_t>(inMask + 1), 0.0f));
            for (const Tone& t : tones) {
                toneWarmup = std::max(toneWarmup, t.warmup);
            }
        }
        // The tone filters must have settled before the part of the sound the repair of the regions looks at starts.
        settle = (hasMask ? fftSize + context * hop + 2 * hop : 0) +
                 (hasTone ? static_cast<int>(std::ceil(toneWarmup * sampleRate)) + 64 : 0);
    }

    // ---- Configuration ----
    int channels;
    int sampleRate;
    int fftSize;
    SpectralListen listen;
    Fft fft;
    int hop = 0;
    int bins = 0;
    double binHz = 0.0;
    std::vector<double> window;
    double scale = 1.0;
    std::vector<Region> regions;
    std::vector<Tone> tones;
    bool hasMask = false;
    bool hasTone = false;
    bool healing = false;
    int context = 0;
    int latency = 0;
    int settle = 0;
    int slots = 0;
    double fillGain = 2.0;
    double toneWarmup = 0.0;

    // ---- State ----
    bool started = false;
    std::int64_t startAbs = 0;
    std::int64_t nextChain = 0;
    std::int64_t gridOffset = 0;
    bool previousTauValid = false;
    double previousTau = 0.0;
    std::int64_t inMask = 0;
    std::int64_t accMask = 0;
    std::vector<std::vector<float>> inRing;
    std::vector<std::vector<float>> toneRing;
    std::vector<std::vector<double>> acc;
    std::vector<FrameInfo> info;
    std::vector<ComplexF> spec;
    std::vector<std::uint8_t> damage;
    std::vector<Complex> work;
    std::vector<double> coverageRow;
    std::vector<Complex> delta;
    std::vector<double> gateRow;
    std::vector<double> timeBuffer;
    std::vector<double> floorScratch;
    std::int64_t toneCell = std::numeric_limits<std::int64_t>::min();
    double toneTau0 = 0.0;
    double toneTau1 = 0.0;

    int slotOf(std::int64_t frame) const noexcept { return static_cast<int>(floorMod(frame, slots)); }
    ComplexF& specAt(int channel, int slot, int bin) noexcept {
        return spec[(static_cast<std::size_t>(channel) * static_cast<std::size_t>(slots) + static_cast<std::size_t>(slot)) *
                        static_cast<std::size_t>(bins) +
                    static_cast<std::size_t>(bin)];
    }
    std::uint8_t& damageAt(int slot, int bin) noexcept {
        return damage[static_cast<std::size_t>(slot) * static_cast<std::size_t>(bins) + static_cast<std::size_t>(bin)];
    }

    void resetState() noexcept {
        started = false;
        previousTauValid = false;
        toneCell = std::numeric_limits<std::int64_t>::min();
        for (auto& r : inRing) {
            std::fill(r.begin(), r.end(), 0.0f);
        }
        for (auto& r : toneRing) {
            std::fill(r.begin(), r.end(), 0.0f);
        }
        for (auto& r : acc) {
            std::fill(r.begin(), r.end(), 0.0);
        }
        for (FrameInfo& f : info) {
            f = FrameInfo{};
        }
        for (Tone& t : tones) {
            t.running = false;
            std::fill(t.state.begin(), t.state.end(), Tone::Lowpass{});
        }
    }

    // ---- Remove Tone (time domain) ----
    void toneStage(float* frame, std::int64_t chainFrame, std::int64_t absFrame, const SpectralTimeMap& map) noexcept {
        const std::int64_t cell = floorDiv(chainFrame, kToneCellFrames);
        if (cell != toneCell) {
            toneCell = cell;
            toneTau0 = map.sourceSeconds(cell * kToneCellFrames);
            toneTau1 = map.sourceSeconds((cell + 1) * kToneCellFrames);
        }
        const double tau = toneTau0 + (toneTau1 - toneTau0) * static_cast<double>(chainFrame - cell * kToneCellFrames) / kToneCellFrames;
        std::array<double, kMaxChannels> x{};
        std::array<double, kMaxChannels> removedTotal{};
        for (int c = 0; c < channels; ++c) {
            x[static_cast<std::size_t>(c)] = frame[c];
        }
        for (Tone& t : tones) {
            if (tau < t.start - t.warmup || tau > t.end + kToneRampSeconds) {
                if (t.running) {
                    t.running = false;
                    std::fill(t.state.begin(), t.state.end(), Tone::Lowpass{});
                }
                continue;
            }
            t.running = true;
            const double weight = t.depth * smooth01((tau - t.start) / kToneRampSeconds) * smooth01((t.end - tau) / kToneRampSeconds);
            const double cycles = t.cycles(tau);
            const double fraction = cycles - std::floor(cycles);
            // The harmonics are taken out one after the other, each from what the one before left: parallel estimators
            // would each pick up a little of their neighbours' tones (a few percent when they are 50 Hz apart).
            for (int h = 0; h < t.harmonics; ++h) {
                const double angle = kTwoPi * (h + 1) * fraction;
                const double cs = std::cos(angle);
                const double sn = std::sin(angle);
                for (int c = 0; c < channels; ++c) {
                    Tone::Lowpass& s = t.state[static_cast<std::size_t>(h) * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)];
                    const double xin = x[static_cast<std::size_t>(c)];
                    const double zr = xin * cs;
                    const double zi = -xin * sn;
                    s.r1 += t.a * (zr - s.r1);
                    s.i1 += t.a * (zi - s.i1);
                    s.r2 += t.a * (s.r1 - s.r2);
                    s.i2 += t.a * (s.i1 - s.i2);
                    const double r = weight * 2.0 * (s.r2 * cs - s.i2 * sn);
                    x[static_cast<std::size_t>(c)] -= r;
                    removedTotal[static_cast<std::size_t>(c)] += r;
                }
            }
        }
        for (int c = 0; c < channels; ++c) {
            frame[c] = static_cast<float>(x[static_cast<std::size_t>(c)]);
            if (listen != SpectralListen::Off) {
                toneRing[static_cast<std::size_t>(c)][static_cast<std::size_t>(absFrame & inMask)] =
                    static_cast<float>(removedTotal[static_cast<std::size_t>(c)]);
            }
        }
    }

    // ---- Region edits (STFT) ----
    void analyseFrame(std::int64_t j, const SpectralTimeMap& map) noexcept {
        const int slot = slotOf(j);
        FrameInfo& fi = info[static_cast<std::size_t>(slot)];
        fi = FrameInfo{};
        fi.index = j;
        const std::int64_t first = j * hop;
        const std::int64_t centreChain = first + fftSize / 2 - gridOffset;
        fi.tau = map.sourceSeconds(centreChain);
        fi.tauStart = map.sourceSeconds(first - gridOffset);
        fi.tauEnd = map.sourceSeconds(first + fftSize - gridOffset);
        fi.ratio = std::max(map.pitchRatio(centreChain), 1.0e-3);
        const double hopSeconds = static_cast<double>(hop) / sampleRate;
        const double timeScale = previousTauValid ? std::max(1.0, std::fabs(fi.tau - previousTau) / hopSeconds) : 1.0;
        previousTau = fi.tau;
        previousTauValid = true;
        fi.half = 0.5 * std::max(std::fabs(fi.tauEnd - fi.tauStart), static_cast<double>(fftSize) / sampleRate);

        // Frames near an edit are analysed: those the edit reaches, and (for the healing ones) the context around them.
        bool need = false;
        const double pad = (context + 3) * hopSeconds * timeScale + fi.half;
        for (const Region& r : regions) {
            if (fi.tau >= r.box.start - pad && fi.tau <= r.box.end + pad) {
                need = true;
                break;
            }
        }
        if (!need) {
            return;
        }
        fi.analysed = true;
        // Which bins the healing edits have taken (the surroundings that remain are the material a hole is filled from).
        for (int b = 0; b < bins; ++b) {
            damageAt(slot, b) = 0;
        }
        for (const Region& r : regions) {
            if (!r.damaging() || !r.overlaps(fi.tau, fi.half)) {
                continue;
            }
            const int lo = std::max(0, static_cast<int>(std::floor(r.box.low * fi.ratio / binHz)) - 1);
            const int hi = std::min(bins - 1, static_cast<int>(std::ceil(r.box.high * fi.ratio / binHz)) + 1);
            for (int b = lo; b <= hi; ++b) {
                if (r.frameCoverage(fi.tau, fi.half, b * binHz / fi.ratio) > 0.05) {
                    damageAt(slot, b) = 1;
                }
            }
        }
        std::vector<double>& frameBuffer = timeBuffer;
        for (int c = 0; c < channels; ++c) {
            const std::vector<float>& ring = inRing[static_cast<std::size_t>(c)];
            for (int i = 0; i < fftSize; ++i) {
                frameBuffer[static_cast<std::size_t>(i)] =
                    static_cast<double>(ring[static_cast<std::size_t>((first + i) & inMask)]) * window[static_cast<std::size_t>(i)];
            }
            fft.forwardReal(frameBuffer.data(), work.data());
            for (int b = 0; b < bins; ++b) {
                const Complex v = work[static_cast<std::size_t>(b)];
                specAt(c, slot, b) = ComplexF(static_cast<float>(v.real()), static_cast<float>(v.imag()));
            }
        }
    }

    /// The level (linear magnitude) a healed bin should have, or a negative number when nothing around it is usable.
    double healLevel(int channel, std::int64_t f, int bin) noexcept {
        double sumWeight = 0.0;
        double sumLog = 0.0;
        const auto take = [&](double magnitude, double distance) {
            const double w = 1.0 / (distance * distance);
            sumWeight += w;
            sumLog += w * std::log(std::max(magnitude, 1.0e-12));
        };
        for (int d = 1; d <= context; ++d) {
            const std::int64_t q = f - d;
            const int sl = slotOf(q);
            const FrameInfo& fi = info[static_cast<std::size_t>(sl)];
            if (!fi.analysed || fi.index != q) {
                break;
            }
            if (damageAt(sl, bin) == 0) {
                take(std::abs(specAt(channel, sl, bin)), d);
                break;
            }
        }
        for (int d = 1; d <= context; ++d) {
            const std::int64_t q = f + d;
            const int sl = slotOf(q);
            const FrameInfo& fi = info[static_cast<std::size_t>(sl)];
            if (!fi.analysed || fi.index != q) {
                break;
            }
            if (damageAt(sl, bin) == 0) {
                take(std::abs(specAt(channel, sl, bin)), d);
                break;
            }
        }
        const int here = slotOf(f);
        for (int k = 1; k <= kFrequencyReach && bin - k >= 0; ++k) {
            if (damageAt(here, bin - k) == 0) {
                take(std::abs(specAt(channel, here, bin - k)), k);
                break;
            }
        }
        for (int k = 1; k <= kFrequencyReach && bin + k < bins; ++k) {
            if (damageAt(here, bin + k) == 0) {
                take(std::abs(specAt(channel, here, bin + k)), k);
                break;
            }
        }
        return sumWeight > 0.0 ? std::exp(sumLog / sumWeight) : -1.0;
    }

    /// The quiet background of a bin: the lower quartile of the untouched bins around it in time, or negative when too few.
    double floorLevel(int channel, std::int64_t f, int bin) noexcept {
        const int reach = std::min(context, 48);
        std::size_t n = 0;
        for (int d = -reach; d <= reach; ++d) {
            const std::int64_t q = f + d;
            const int sl = slotOf(q);
            const FrameInfo& fi = info[static_cast<std::size_t>(sl)];
            if (!fi.analysed || fi.index != q) {
                continue;
            }
            for (int bb = std::max(0, bin - 1); bb <= std::min(bins - 1, bin + 1); ++bb) {
                if (damageAt(sl, bb) == 0 && n < floorScratch.size()) {
                    floorScratch[n++] = std::abs(specAt(channel, sl, bb));
                }
            }
        }
        if (n < 6) {
            return -1.0;
        }
        const std::size_t k = n / 4;
        std::nth_element(floorScratch.begin(), floorScratch.begin() + static_cast<std::ptrdiff_t>(k),
                         floorScratch.begin() + static_cast<std::ptrdiff_t>(n));
        return floorScratch[k];
    }

    /// Applies the edits that reach frame `f`, one after the other on the frame's spectrum, and adds what each changes
    /// (gated to the seconds of its region) to the overlap-add accumulator.
    void finalizeFrame(std::int64_t f) noexcept {
        const int slot = slotOf(f);
        const FrameInfo fi = info[static_cast<std::size_t>(slot)];
        if (!fi.analysed || fi.index != f) {
            return;
        }
        const auto cur = [&](int c, int b) -> Complex& {
            return work[static_cast<std::size_t>(c) * static_cast<std::size_t>(bins) + static_cast<std::size_t>(b)];
        };
        bool loaded = false;
        const std::int64_t first = f * hop;
        const double sign = listen == SpectralListen::Removed ? -1.0 : 1.0;
        for (const Region& r : regions) {
            if (!r.overlaps(fi.tau, fi.half)) {
                continue;
            }
            // The edit's time envelope over this frame's samples; nothing to do where it is zero throughout.
            double gateMax = 0.0;
            for (int i = 0; i < fftSize; ++i) {
                const double tau = fi.tauStart + (fi.tauEnd - fi.tauStart) * static_cast<double>(i) / fftSize;
                gateRow[static_cast<std::size_t>(i)] = r.gateAt(tau);
                gateMax = std::max(gateMax, gateRow[static_cast<std::size_t>(i)]);
            }
            if (gateMax <= 0.0) {
                continue;
            }
            if (!loaded) {
                loaded = true;
                for (int c = 0; c < channels; ++c) {
                    for (int b = 0; b < bins; ++b) {
                        const ComplexF v = specAt(c, slot, b);
                        cur(c, b) = Complex(v.real(), v.imag());
                    }
                }
            }
            const int lo = std::max(0, static_cast<int>(std::floor(r.box.low * fi.ratio / binHz)) - 1);
            const int hi = std::min(bins - 1, static_cast<int>(std::ceil(r.box.high * fi.ratio / binHz)) + 1);
            for (int b = lo; b <= hi; ++b) {
                coverageRow[static_cast<std::size_t>(b)] = r.frameCoverage(fi.tau, fi.half, b * binHz / fi.ratio);
            }
            for (int c = 0; c < channels; ++c) {
                std::fill(delta.begin(), delta.end(), Complex{});
                for (int b = lo; b <= hi; ++b) {
                    const double cov = coverageRow[static_cast<std::size_t>(b)];
                    if (cov <= 0.0) {
                        continue;
                    }
                    Complex& y = cur(c, b);
                    if (listen == SpectralListen::Selection) {
                        delta[static_cast<std::size_t>(b)] = Complex(specAt(c, slot, b)) * cov; // the sound the edit selects
                        continue;
                    }
                    Complex changed;
                    if (r.kind == SpectralKind::Attenuate) {
                        changed = y * (1.0 + cov * (r.gain - 1.0));
                    } else {
                        const double level = r.kind == SpectralKind::Heal ? healLevel(c, f, b) : floorLevel(c, f, b);
                        const double weight = cov * r.amount;
                        Complex fill;
                        if (level < 0.0) {
                            fill = y * 0.01; // nothing around to fill from: a hole 40 dB down
                        } else {
                            const double bias = r.kind == SpectralKind::Heal ? kLogLevelBias : kQuartileToRms;
                            fill = std::polar(level * bias * fillGain, fillPhase(f, b, c, bins - 1));
                        }
                        changed = y * (1.0 - weight) + fill * weight;
                    }
                    delta[static_cast<std::size_t>(b)] = (changed - y) * sign;
                    y = changed;
                }
                fft.inverseReal(delta.data(), timeBuffer.data());
                std::vector<double>& ring = acc[static_cast<std::size_t>(c)];
                for (int i = 0; i < fftSize; ++i) {
                    ring[static_cast<std::size_t>((first + i) & accMask)] += timeBuffer[static_cast<std::size_t>(i)] *
                                                                           window[static_cast<std::size_t>(i)] * scale *
                                                                           gateRow[static_cast<std::size_t>(i)];
                }
            }
        }
    }

    void onFrameComplete(std::int64_t j, const SpectralTimeMap& map) noexcept {
        if (j * hop < startAbs) {
            FrameInfo& fi = info[static_cast<std::size_t>(slotOf(j))];
            fi = FrameInfo{};
            fi.index = j;
        } else {
            analyseFrame(j, map);
        }
        finalizeFrame(j - context);
    }

    void process(float* io, int frames, std::int64_t firstFrame, std::int64_t grid, const SpectralTimeMap& map) noexcept {
        if (!hasMask && !hasTone) {
            return;
        }
        gridOffset = grid;
        if (started && firstFrame != nextChain) {
            resetState();
        }
        if (!started) {
            started = true;
            startAbs = firstFrame + grid;
        }
        const auto ch = static_cast<std::size_t>(channels);
        for (int i = 0; i < frames; ++i) {
            const std::int64_t chainFrame = firstFrame + i;
            const std::int64_t a = chainFrame + grid;
            float* frame = io + static_cast<std::size_t>(i) * ch;
            if (hasTone) {
                toneStage(frame, chainFrame, a, map);
            }
            for (std::size_t c = 0; c < ch; ++c) {
                inRing[c][static_cast<std::size_t>(a & inMask)] = frame[c];
            }
            if (hasMask) {
                const std::int64_t first = a + 1 - fftSize;
                if (floorMod(first, hop) == 0) {
                    onFrameComplete(floorDiv(first, hop), map);
                }
            }
            const std::int64_t n = a - latency;
            for (std::size_t c = 0; c < ch; ++c) {
                const auto at = static_cast<std::size_t>(n & inMask);
                double base = 0.0;
                switch (listen) {
                case SpectralListen::Off: base = hasMask ? inRing[c][at] : frame[c]; break;
                case SpectralListen::Selection:
                    base = hasTone ? static_cast<double>(toneRing[c][at]) : 0.0; // the regions add the sound they select
                    break;
                case SpectralListen::Removed: base = hasTone ? static_cast<double>(toneRing[c][at]) : 0.0; break;
                }
                if (hasMask) {
                    double& slot = acc[c][static_cast<std::size_t>(n & accMask)];
                    base += slot;
                    slot = 0.0;
                }
                frame[c] = static_cast<float>(base);
            }
        }
        nextChain = firstFrame + frames;
    }
};

SpectralRepair::SpectralRepair(int channels, int sampleRate, std::span<const SpectralEdit> edits, const SpectralRepairOptions& options)
    : m_impl(std::make_unique<Impl>(channels, sampleRate, edits, options)) {}

SpectralRepair::~SpectralRepair() = default;

bool SpectralRepair::active() const noexcept { return m_impl->hasMask || m_impl->hasTone; }
int SpectralRepair::latencyFrames() const noexcept { return m_impl->hasMask ? m_impl->latency : 0; }
int SpectralRepair::settleFrames() const noexcept { return m_impl->settle; }
int SpectralRepair::fftSize() const noexcept { return m_impl->fftSize; }
int SpectralRepair::contextFrames() const noexcept { return m_impl->context; }

void SpectralRepair::process(float* interleaved, int frames, std::int64_t firstFrame, std::int64_t gridOffset,
                             const SpectralTimeMap& map) noexcept {
    m_impl->process(interleaved, frames, firstFrame, gridOffset, map);
}

void SpectralRepair::reset() noexcept { m_impl->resetState(); }

std::vector<float> renderSpectralRepair(std::span<const float> interleaved, int channels, int sampleRate,
                                        std::span<const SpectralEdit> edits, const SpectralRepairOptions& options,
                                        const SpectralTimeMap& map, std::int64_t gridOffset) {
    SpectralRepair repair(channels, sampleRate, edits, options);
    const auto ch = static_cast<std::size_t>(std::clamp(channels, 1, SpectralRepair::kMaxChannels));
    const std::size_t frames = interleaved.size() / ch;
    const std::size_t latency = static_cast<std::size_t>(repair.latencyFrames());
    std::vector<float> all(interleaved.begin(), interleaved.end());
    all.resize((frames + latency) * ch, 0.0f);
    constexpr std::size_t kBlock = 4096;
    for (std::size_t done = 0; done < frames + latency;) {
        const std::size_t n = std::min(kBlock, frames + latency - done);
        repair.process(all.data() + done * ch, static_cast<int>(n), static_cast<std::int64_t>(done), gridOffset, map);
        done += n;
    }
    return std::vector<float>(all.begin() + static_cast<std::ptrdiff_t>(latency * ch), all.end());
}

double spectralCoverage(const SpectralEdit& edit, double seconds, double hz, int fftSize, int sampleRate) {
    if (spectralEditProblem(edit)) {
        return 0.0;
    }
    if (edit.kind == SpectralKind::Tone) {
        if (seconds < edit.start || seconds > edit.end) {
            return 0.0;
        }
        const Tone tone(edit, 1, std::max(sampleRate, 8000));
        if (!tone.usable()) {
            return 0.0;
        }
        const std::size_t n = tone.t.size();
        double f0 = tone.f[0];
        if (n > 1 && seconds >= tone.t[0]) {
            const auto it = std::upper_bound(tone.t.begin(), tone.t.end(), seconds);
            if (it == tone.t.end()) {
                f0 = tone.f[n - 1];
            } else {
                const std::size_t i = static_cast<std::size_t>(it - tone.t.begin()) - 1;
                const double u = (seconds - tone.t[i]) / (tone.t[i + 1] - tone.t[i]);
                f0 = tone.f[i] + u * (tone.f[i + 1] - tone.f[i]);
            }
        }
        for (int h = 1; h <= tone.harmonics; ++h) {
            if (std::fabs(hz - h * f0) <= 0.5 * edit.widthHz) {
                return 1.0;
            }
        }
        return 0.0;
    }
    return Region(edit, snapFftSize(fftSize), std::max(sampleRate, 8000)).coverage(seconds, hz);
}

} // namespace wabcut::audio::dsp
