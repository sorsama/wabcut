#include <wabcut/audio/edit/AudioSync.h>

#include "SyncFft.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <map>
#include <mutex>
#include <numbers>
#include <thread>

namespace wabcut::audio::edit {

namespace {

using detail::Cf;
using detail::SyncFft;

constexpr int kEnvelopeRate = 1000;
constexpr double kHighPassHz = 150.0;
constexpr int kRunningMeanFrames = 30;      ///< the onset envelope's reference: the last 30 ms
constexpr std::int64_t kPeakExclusion = 50; ///< ms around the peak left out of the sidelobe statistics
constexpr double kMinWindowPsr = 6.0;       ///< a fine window below this matched nothing (silence there)

double confidenceOf(double psr) {
    return std::clamp((psr - 8.0) / 12.0, 0.0, 1.0);
}

/// First-order high-pass, sample by sample.
struct HighPass {
    explicit HighPass(int rate) : a(static_cast<float>(std::exp(-2.0 * std::numbers::pi * kHighPassHz / rate))) {}
    float a;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float operator()(float x) {
        const float y = a * (y1 + x - x1);
        x1 = x;
        y1 = y;
        return y;
    }
};

/// The onset envelope, built block by block: per millisecond, the high-passed energy in dB, minus the mean of the
/// previous 30 ms, kept where positive.
class EnvelopeBuilder {
public:
    explicit EnvelopeBuilder(int rate) : m_rate(rate), m_filter(rate) {}

    void push(std::span<const float> samples) {
        for (const float s : samples) {
            const float y = m_filter(s);
            m_energy += static_cast<double>(y) * y;
            ++m_count;
            ++m_sample;
            if (m_sample >= m_nextBoundary) {
                emit();
            }
        }
    }

    std::vector<float> take() { return std::move(m_out); }
    void reserve(std::int64_t frames) { m_out.reserve(static_cast<std::size_t>(std::max<std::int64_t>(frames, 0))); }

private:
    void emit() {
        const double level = 10.0 * std::log10(m_energy / static_cast<double>(std::max<std::int64_t>(m_count, 1)) + 1e-10);
        if (m_history.empty()) {
            m_history.assign(kRunningMeanFrames, level);
            m_sum = level * kRunningMeanFrames;
        }
        const double mean = m_sum / kRunningMeanFrames;
        m_out.push_back(static_cast<float>(std::max(0.0, level - mean)));
        m_sum += level - m_history[m_head];
        m_history[m_head] = level;
        m_head = (m_head + 1) % kRunningMeanFrames;
        m_energy = 0.0;
        m_count = 0;
        ++m_frame;
        m_nextBoundary = (m_frame + 1) * m_rate / kEnvelopeRate;
    }

    int m_rate;
    HighPass m_filter;
    double m_energy = 0.0;
    std::int64_t m_count = 0;
    std::int64_t m_sample = 0;
    std::int64_t m_frame = 0;
    std::int64_t m_nextBoundary = m_rate / kEnvelopeRate;
    std::vector<double> m_history;
    std::size_t m_head = 0;
    double m_sum = 0.0;
    std::vector<float> m_out;
};

/// The cross-spectrum of the real signals packed as z = a + i b (after z = FFT(z)), PHAT-weighted, in place:
/// conjugate(a) * b when `aConjugate`, else a * conjugate(b). Bins outside [lowBin, highBin] are zeroed.
void phatCrossSpectrum(std::vector<Cf>& z, bool aConjugate, std::size_t lowBin, std::size_t highBin) {
    const std::size_t n = z.size();
    const auto cross = [&](Cf zk, Cf zm) {
        const Cf a = (zk + std::conj(zm)) * 0.5f;
        const Cf b = (zk - std::conj(zm)) * Cf(0.0f, -0.5f);
        Cf g = aConjugate ? std::conj(a) * b : a * std::conj(b);
        const float m = std::abs(g);
        return m > 1e-20f ? g / m : Cf{};
    };
    const auto inBand = [&](std::size_t k) {
        const std::size_t f = std::min(k, n - k); // the bin's frequency index (both halves)
        return f >= lowBin && f <= highBin;
    };
    for (std::size_t k = 0; k <= n / 2; ++k) {
        const std::size_t m = (n - k) % n;
        const Cf zk = z[k];
        const Cf zm = z[m];
        const Cf gk = inBand(k) ? cross(zk, zm) : Cf{};
        const Cf gm = inBand(m) ? cross(zm, zk) : Cf{};
        z[k] = gk;
        z[m] = gm;
    }
}

struct Peak {
    std::int64_t lag = 0;
    double value = 0.0;
    double psr = 0.0;
};

/// The peak of c over lags [lo, hi] (index of lag L: L mod n), with its peak-to-sidelobe ratio.
Peak findPeak(const std::vector<Cf>& c, std::int64_t lo, std::int64_t hi) {
    const auto n = static_cast<std::int64_t>(c.size());
    const auto at = [&](std::int64_t lag) { return static_cast<double>(c[static_cast<std::size_t>(((lag % n) + n) % n)].real()); };
    Peak p;
    p.value = -1e300;
    for (std::int64_t lag = lo; lag <= hi; ++lag) {
        const double v = at(lag);
        if (v > p.value) {
            p.value = v;
            p.lag = lag;
        }
    }
    double sum = 0.0;
    double sum2 = 0.0;
    std::int64_t count = 0;
    for (std::int64_t lag = lo; lag <= hi; ++lag) {
        if (std::abs(lag - p.lag) <= kPeakExclusion) {
            continue;
        }
        const double v = at(lag);
        sum += v;
        sum2 += v * v;
        ++count;
    }
    if (count > 1) {
        const double mean = sum / static_cast<double>(count);
        const double sd = std::sqrt(std::max(sum2 / static_cast<double>(count) - mean * mean, 1e-30));
        p.psr = (p.value - mean) / sd;
    }
    return p;
}

/// One fine window: the lag (refineRate samples, fractional) of `angle` against `reference` around `predicted`.
struct WindowLag {
    double centre = 0.0; ///< reference samples
    double lag = 0.0;
    double psr = 0.0;
};

std::expected<WindowLag, Error> refineWindow(SyncSignal& reference, SyncSignal& angle, int rate, std::int64_t centre,
                                             std::int64_t window, std::int64_t search, double predicted) {
    const std::int64_t refStart = centre - window / 2;
    const auto lagGuess = static_cast<std::int64_t>(std::llround(predicted));
    const std::int64_t angleStart = refStart - lagGuess - search;
    std::vector<float> x(static_cast<std::size_t>(window));
    std::vector<float> y(static_cast<std::size_t>(window + 2 * search));
    if (auto r = reference.read(rate, refStart, x); !r) {
        return std::unexpected(r.error());
    }
    if (auto r = angle.read(rate, angleStart, y); !r) {
        return std::unexpected(r.error());
    }
    HighPass hx(rate);
    HighPass hy(rate);
    // Cosine tapers over the first and last 10 % of the window: the cut edges of both chunks would otherwise
    // correlate with each other (a spurious peak where the two starts line up).
    const auto taperLength = static_cast<std::size_t>(std::max<std::int64_t>(window / 10, 1));
    const auto taper = [&](std::size_t i, std::size_t length) {
        const std::size_t edge = std::min(i, length - 1 - i);
        return edge >= taperLength ? 1.0f
                                   : static_cast<float>(0.5 - 0.5 * std::cos(std::numbers::pi * static_cast<double>(edge) /
                                                                             static_cast<double>(taperLength)));
    };
    const std::size_t n = detail::nextPowerOfTwo(x.size() + y.size());
    std::vector<Cf> z(n);
    for (std::size_t i = 0; i < x.size(); ++i) {
        z[i] = Cf(hx(x[i]) * taper(i, x.size()), 0.0f);
    }
    for (std::size_t i = 0; i < y.size(); ++i) {
        z[i] = Cf(z[i].real(), hy(y[i]) * taper(i, y.size()));
    }
    const SyncFft fft(n);
    fft.forward(z.data());
    // Speech and music band: 100 Hz .. 8 kHz.
    const auto bin = [&](double hz) { return static_cast<std::size_t>(hz * static_cast<double>(n) / rate); };
    phatCrossSpectrum(z, true, std::max<std::size_t>(bin(100.0), 1), bin(std::min(8000.0, rate * 0.45)));
    fft.inverse(z.data());
    const Peak p = findPeak(z, 0, 2 * search);
    // Parabolic interpolation around the peak.
    double fraction = 0.0;
    if (p.lag > 0 && p.lag < 2 * search) {
        const double l = z[static_cast<std::size_t>(p.lag - 1)].real();
        const double c = z[static_cast<std::size_t>(p.lag)].real();
        const double r = z[static_cast<std::size_t>(p.lag + 1)].real();
        const double d = l - 2.0 * c + r;
        if (d < 0.0) {
            fraction = std::clamp(0.5 * (l - r) / d, -0.5, 0.5);
        }
    }
    WindowLag w;
    w.centre = static_cast<double>(centre);
    w.lag = static_cast<double>(lagGuess + search) - (static_cast<double>(p.lag) + fraction);
    // A peak on the search's edge is the edge of the range, not a match.
    w.psr = p.lag <= 0 || p.lag >= 2 * search ? 0.0 : p.psr;
    return w;
}

struct LineFit {
    double a = 0.0;
    double b = 0.0;
    double rms = 0.0;
};

LineFit fitLine(const std::vector<WindowLag>& points) {
    LineFit f;
    if (points.empty()) {
        return f;
    }
    if (points.size() == 1) {
        f.a = points.front().lag;
        return f;
    }
    double sw = 0.0, st = 0.0, sl = 0.0, stt = 0.0, stl = 0.0;
    for (const WindowLag& p : points) {
        const double w = std::max(p.psr, 1.0);
        sw += w;
        st += w * p.centre;
        sl += w * p.lag;
        stt += w * p.centre * p.centre;
        stl += w * p.centre * p.lag;
    }
    const double det = sw * stt - st * st;
    if (std::fabs(det) < 1e-9) {
        f.a = sl / sw;
        return f;
    }
    f.b = (sw * stl - st * sl) / det;
    f.a = (sl - f.b * st) / sw;
    double e = 0.0;
    for (const WindowLag& p : points) {
        const double r = p.lag - (f.a + f.b * p.centre);
        e += r * r;
    }
    f.rms = std::sqrt(e / static_cast<double>(points.size()));
    return f;
}

/// The fine pass of one pair (b against a): offsets, drift and agreement from windows across the overlap.
std::expected<SyncMatch, Error> refinePair(SyncSignal& a, SyncSignal& b, const CoarseMatch& coarse, const SyncOptions& o,
                                           std::stop_token stop) {
    SyncMatch m;
    m.peakToSidelobe = coarse.peakToSidelobe;
    const int rate = o.refineRate;
    const std::int64_t lengthA = a.frames(rate);
    const std::int64_t lengthB = b.frames(rate);
    const double coarseLag = static_cast<double>(coarse.lag) * rate / kEnvelopeRate;
    const auto lag0 = static_cast<std::int64_t>(std::llround(coarseLag));
    const std::int64_t overlapStart = std::max<std::int64_t>(0, lag0);
    const std::int64_t overlapEnd = std::min(lengthA, lag0 + lengthB);
    const std::int64_t overlap = overlapEnd - overlapStart;
    if (overlap <= 0) {
        m.problem = "no common audio at the coarse match";
        return m;
    }
    const auto search = static_cast<std::int64_t>(o.searchSeconds * rate);
    std::int64_t window = std::min(static_cast<std::int64_t>(o.windowSeconds * rate), overlap);
    window = std::max<std::int64_t>(window, rate / 10);
    const int count = std::max(1, o.refineWindows);
    std::vector<std::int64_t> centres;
    const std::int64_t first = overlapStart + window / 2;
    const std::int64_t last = overlapEnd - window / 2;
    if (count == 1 || last <= first) {
        centres.push_back((overlapStart + overlapEnd) / 2);
    } else {
        for (int i = 0; i < count; ++i) {
            centres.push_back(first + (last - first) * i / (count - 1));
        }
    }
    // From the middle outwards, each window predicted by the line through the ones already measured (drift).
    std::ranges::sort(centres, [&](std::int64_t x, std::int64_t y) {
        const std::int64_t mid = (overlapStart + overlapEnd) / 2;
        return std::abs(x - mid) < std::abs(y - mid);
    });
    std::vector<WindowLag> good;
    for (const std::int64_t c : centres) {
        if (stop.stop_requested()) {
            return std::unexpected(Error(ErrorCode::Cancelled, "cancelled"));
        }
        const LineFit sofar = fitLine(good);
        const double predicted = good.empty() ? coarseLag : (good.size() == 1 ? good.front().lag : sofar.a + sofar.b * static_cast<double>(c));
        auto w = refineWindow(a, b, rate, c, window, search, predicted);
        if (!w) {
            return std::unexpected(w.error());
        }
        if (w->psr >= kMinWindowPsr) {
            good.push_back(*w);
        }
    }
    if (good.empty()) {
        m.problem = "the fine pass found no match (silence where the recordings overlap?)";
        return m;
    }
    // Windows off the line by more than 1 ms disagree (a repeated sound, noise): refit without them.
    LineFit fit = fitLine(good);
    const double tolerance = rate / 1000.0;
    std::vector<WindowLag> agreed;
    for (const WindowLag& w : good) {
        if (std::fabs(w.lag - (fit.a + fit.b * w.centre)) <= tolerance) {
            agreed.push_back(w);
        }
    }
    if (agreed.empty()) {
        agreed.push_back(*std::ranges::max_element(good, {}, &WindowLag::psr));
    }
    fit = fitLine(agreed);
    if (agreed.size() < 3) {
        fit.b = 0.0; // two points measure no drift reliably
        double sum = 0.0;
        for (const WindowLag& w : agreed) {
            sum += w.lag;
        }
        fit.a = sum / static_cast<double>(agreed.size());
    }
    const double mid = 0.5 * static_cast<double>(overlapStart + overlapEnd);
    m.offset = std::llround(fit.a + fit.b * mid);
    m.offsetAtStart = std::llround(fit.a + fit.b * static_cast<double>(overlapStart));
    m.overlapSeconds = static_cast<double>(overlap) / rate;
    m.windows = static_cast<int>(agreed.size());
    m.driftPpm = -fit.b * 1e6;
    m.driftResidualMs = fit.rms * 1000.0 / rate;
    // How well the line's slope is known: the windows' spread against their scatter (at least 0.05 ms per window).
    double spread = 0.0;
    for (const WindowLag& w : agreed) {
        spread = std::max(spread, std::fabs(w.centre - mid));
    }
    const double jitter = std::max(fit.rms, 0.05 * rate / 1000.0);
    const double ppmError = spread > 0.0 ? jitter / spread * 1e6 : 1e9;
    m.driftSignificant = agreed.size() >= 3 && std::fabs(m.driftPpm) >= 2.0 && std::fabs(m.driftPpm) > 3.0 * ppmError;
    m.speedProposal = m.driftSignificant ? 1.0 + m.driftPpm * 1e-6 : 1.0;
    m.confidence = coarse.confidence * static_cast<double>(agreed.size()) / static_cast<double>(centres.size());
    m.synced = m.confidence >= o.minConfidence;
    if (!m.synced) {
        m.problem = "the match is not certain enough (confidence " + std::to_string(m.confidence) + ")";
    }
    return m;
}

/// Runs `job(i)` for i in [0, count) on up to `threads` threads, reporting `progress()` from the calling thread every
/// 50 ms (it may throw: the workers are stopped first, then it is rethrown). The first error wins.
template <class Job>
std::expected<void, Error> parallel(std::size_t count, int threads, std::stop_source& stopper, const std::function<void()>& report,
                                    Job job) {
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> finished{0};
    std::mutex mutex;
    std::optional<Error> failure;
    std::vector<std::jthread> workers;
    const auto worker = [&] {
        for (;;) {
            const std::size_t i = next.fetch_add(1);
            if (i >= count || stopper.stop_requested()) {
                return;
            }
            auto r = job(i);
            finished.fetch_add(1);
            if (!r) {
                std::lock_guard lock(mutex);
                if (!failure) {
                    failure = r.error();
                }
                stopper.request_stop();
                return;
            }
        }
    };
    const int n = std::max(1, std::min<int>(threads, static_cast<int>(count)));
    for (int t = 0; t < n; ++t) {
        workers.emplace_back(worker);
    }
    std::exception_ptr thrown;
    for (;;) {
        if (finished.load() >= count || stopper.stop_requested()) {
            break;
        }
        try {
            if (report) {
                report();
            }
        } catch (...) {
            thrown = std::current_exception();
            stopper.request_stop();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    for (std::jthread& w : workers) {
        w.join();
    }
    if (thrown) {
        std::rethrow_exception(thrown);
    }
    if (failure) {
        return std::unexpected(*failure);
    }
    return {};
}

} // namespace

std::expected<std::vector<float>, Error> onsetEnvelope(SyncSignal& signal, int rate, std::stop_token stop,
                                                       const std::function<void(double)>& progress) {
    if (rate < kEnvelopeRate) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "the analysis rate must be at least 1 kHz"));
    }
    const std::int64_t frames = signal.frames(rate);
    EnvelopeBuilder builder(rate);
    builder.reserve(frames * kEnvelopeRate / rate + 1);
    std::vector<float> block(static_cast<std::size_t>(rate)); // one second at a time
    for (std::int64_t at = 0; at < frames; at += rate) {
        if (stop.stop_requested()) {
            return std::unexpected(Error(ErrorCode::Cancelled, "cancelled"));
        }
        const auto n = static_cast<std::size_t>(std::min<std::int64_t>(rate, frames - at));
        if (auto r = signal.read(rate, at, std::span<float>(block.data(), n)); !r) {
            return std::unexpected(r.error());
        }
        builder.push(std::span<const float>(block.data(), n));
        if (progress) {
            progress(static_cast<double>(at + static_cast<std::int64_t>(n)) / static_cast<double>(std::max<std::int64_t>(frames, 1)));
        }
    }
    return builder.take();
}

CoarseMatch coarseMatch(std::span<const float> a, std::span<const float> b, std::int64_t minOverlap) {
    CoarseMatch m;
    const auto lengthA = static_cast<std::int64_t>(a.size());
    const auto lengthB = static_cast<std::int64_t>(b.size());
    minOverlap = std::max<std::int64_t>(minOverlap, 1);
    if (lengthA < minOverlap || lengthB < minOverlap) {
        m.problem = "a recording is shorter than the minimum overlap";
        return m;
    }
    double meanA = 0.0;
    double meanB = 0.0;
    for (const float v : a) {
        meanA += v;
    }
    for (const float v : b) {
        meanB += v;
    }
    meanA /= static_cast<double>(lengthA);
    meanB /= static_cast<double>(lengthB);
    const std::size_t n = detail::nextPowerOfTwo(a.size() + b.size());
    std::vector<Cf> z(n);
    // Cosine tapers over the first and last second (at most a tenth): the recordings' edges must not match each other.
    const auto taper = [](std::size_t i, std::size_t length) {
        const std::size_t taperLength = std::max<std::size_t>(std::min<std::size_t>(kEnvelopeRate, length / 10), 1);
        const std::size_t edge = std::min(i, length - 1 - i);
        return edge >= taperLength ? 1.0
                                   : 0.5 - 0.5 * std::cos(std::numbers::pi * static_cast<double>(edge) / static_cast<double>(taperLength));
    };
    for (std::size_t i = 0; i < a.size(); ++i) {
        z[i] = Cf(static_cast<float>((a[i] - meanA) * taper(i, a.size())), 0.0f);
    }
    for (std::size_t i = 0; i < b.size(); ++i) {
        z[i] = Cf(z[i].real(), static_cast<float>((b[i] - meanB) * taper(i, b.size())));
    }
    const SyncFft fft(n);
    fft.forward(z.data());
    // Envelope modulation band: 0.5 Hz .. 100 Hz (DC and the 1 kHz frame grid's highs are noise).
    const auto bin = [&](double hz) { return static_cast<std::size_t>(hz * static_cast<double>(n) / kEnvelopeRate); };
    phatCrossSpectrum(z, false, std::max<std::size_t>(bin(0.5), 1), bin(100.0));
    fft.inverse(z.data());
    // b starts `lag` frames after a: lags that leave minOverlap frames in common.
    const Peak p = findPeak(z, minOverlap - lengthB, lengthA - minOverlap);
    m.lag = p.lag;
    m.peakToSidelobe = p.psr;
    m.confidence = confidenceOf(p.psr);
    m.overlap = std::min(lengthA, p.lag + lengthB) - std::max<std::int64_t>(0, p.lag);
    m.found = m.confidence > 0.0;
    if (!m.found) {
        m.problem = "no match: the recordings' audio does not correlate (peak " + std::to_string(p.psr) + " sigma)";
    }
    return m;
}

std::expected<SyncReport, Error> syncSignals(std::span<SyncSignal* const> signals, std::size_t reference,
                                             const SyncOptions& options, std::stop_token stop,
                                             const std::function<void(double)>& progress) {
    const auto started = std::chrono::steady_clock::now();
    if (signals.size() < 2 || reference >= signals.size()) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "audio sync needs two recordings or more and a reference among them"));
    }
    if (options.analysisRate < kEnvelopeRate || options.refineRate < 8000 || options.minOverlapSeconds <= 0.0 ||
        options.windowSeconds <= 0.0 || options.searchSeconds <= 0.0) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "invalid audio sync options"));
    }
    std::stop_source stopper;
    std::stop_callback forward(stop, [&] { stopper.request_stop(); });
    const int threads = options.threads > 0 ? options.threads : std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 4);

    // 1. Envelopes, the recordings in parallel (each read by one thread).
    std::vector<std::int64_t> totals(signals.size());
    std::int64_t total = 0;
    for (std::size_t i = 0; i < signals.size(); ++i) {
        totals[i] = signals[i]->frames(options.analysisRate);
        total += totals[i];
    }
    std::vector<std::atomic<std::int64_t>> read(signals.size());
    std::vector<std::vector<float>> envelopes(signals.size());
    const auto report = [&](double from, double to, double fraction) {
        if (progress) {
            progress(from + (to - from) * std::clamp(fraction, 0.0, 1.0));
        }
    };
    const std::function<void()> envelopeProgress = [&] {
        std::int64_t done = 0;
        for (const auto& r : read) {
            done += r.load();
        }
        report(0.0, 0.7, total > 0 ? static_cast<double>(done) / static_cast<double>(total) : 1.0);
    };
    auto envelopesDone = parallel(signals.size(), threads, stopper, envelopeProgress, [&](std::size_t i) -> std::expected<void, Error> {
        auto e = onsetEnvelope(*signals[i], options.analysisRate, stopper.get_token(), [&, i](double f) {
            read[i].store(static_cast<std::int64_t>(f * static_cast<double>(totals[i])));
        });
        if (!e) {
            return std::unexpected(e.error());
        }
        envelopes[i] = std::move(*e);
        return {};
    });
    if (!envelopesDone) {
        return std::unexpected(stop.stop_requested() ? Error(ErrorCode::Cancelled, "cancelled") : envelopesDone.error());
    }
    if (stop.stop_requested()) {
        return std::unexpected(Error(ErrorCode::Cancelled, "cancelled"));
    }

    // 2. and 3. Every recording against the reference (coarse in parallel, fine one after the other), then the ones
    // left against the synced ones.
    SyncReport out;
    out.reference = reference;
    out.matches.resize(signals.size());
    out.matches[reference].synced = true;
    out.matches[reference].confidence = 1.0;
    out.matches[reference].relativeTo = reference;
    out.matches[reference].overlapSeconds = static_cast<double>(totals[reference]) / options.analysisRate;
    const auto minOverlap = static_cast<std::int64_t>(options.minOverlapSeconds * kEnvelopeRate);
    std::vector<std::size_t> pending;
    for (std::size_t i = 0; i < signals.size(); ++i) {
        if (i != reference) {
            pending.push_back(i);
        }
    }
    std::vector<std::size_t> anchors{reference};
    std::atomic<std::size_t> coarseDone{0};
    while (!pending.empty()) {
        const std::size_t anchor = anchors.front();
        anchors.erase(anchors.begin());
        std::vector<CoarseMatch> coarse(pending.size());
        const std::function<void()> coarseProgress = [&] {
            report(0.7, 0.85, static_cast<double>(coarseDone.load()) / static_cast<double>(signals.size() - 1));
        };
        auto coarseRun = parallel(pending.size(), threads, stopper, coarseProgress, [&](std::size_t k) -> std::expected<void, Error> {
            coarse[k] = coarseMatch(envelopes[anchor], envelopes[pending[k]], minOverlap);
            coarseDone.fetch_add(1);
            return {};
        });
        if (!coarseRun || stop.stop_requested()) {
            return std::unexpected(Error(ErrorCode::Cancelled, "cancelled"));
        }
        std::vector<std::size_t> still;
        for (std::size_t k = 0; k < pending.size(); ++k) {
            const std::size_t i = pending[k];
            SyncMatch m;
            if (!coarse[k].found || coarse[k].confidence < options.minConfidence) {
                m.peakToSidelobe = coarse[k].peakToSidelobe;
                m.confidence = coarse[k].confidence;
                m.problem = coarse[k].problem.empty() ? "no certain match against recording " + std::to_string(anchor) : coarse[k].problem;
            } else {
                auto fine = refinePair(*signals[anchor], *signals[i], coarse[k], options, stopper.get_token());
                if (!fine) {
                    return std::unexpected(fine.error());
                }
                m = std::move(*fine);
            }
            report(0.85, 1.0, static_cast<double>(signals.size() - pending.size() + k + 1) / static_cast<double>(signals.size()));
            if (m.synced && anchor != reference) {
                // Through another recording: its offset and drift on the reference's clock add up.
                const SyncMatch& via = out.matches[anchor];
                m.offset += via.offset;
                m.offsetAtStart += via.offset;
                m.driftPpm += via.driftPpm;
                m.confidence = std::min(m.confidence, via.confidence);
            }
            m.relativeTo = anchor;
            if (m.synced) {
                out.matches[i] = std::move(m);
                anchors.push_back(i);
            } else {
                if (anchor == reference || out.matches[i].problem.empty()) {
                    out.matches[i] = std::move(m);
                }
                still.push_back(i);
            }
        }
        pending = std::move(still);
        if (!options.chain || anchors.empty()) {
            break;
        }
    }
    report(0.0, 1.0, 1.0);
    out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return out;
}

std::expected<std::vector<Ticks>, Error> angleOffsets(AudioSyncService& sync, const ProjectState& state,
                                                      std::span<const MediaId> angles, std::stop_token stop,
                                                      const std::function<void(double)>& progress) {
    std::vector<AudioSyncService::Angle> list;
    for (const MediaId id : angles) {
        list.push_back(AudioSyncService::Angle{{id}, 0, true});
    }
    auto result = sync.sync(state, list, 0, SyncOptions{}, stop, progress);
    if (!result) {
        return std::unexpected(result.error());
    }
    std::vector<Ticks> out;
    for (std::size_t i = 0; i < angles.size(); ++i) {
        const SyncMatch& m = result->report.matches[i];
        if (!m.synced) {
            const MediaItem* item = state.findMedia(angles[i]);
            return std::unexpected(Error(ErrorCode::InvalidArgument, "angle " + std::to_string(i + 1) + " ('" +
                                                                         (item != nullptr ? item->name : std::string("?")) +
                                                                         "') could not be synced by audio: " + m.problem));
        }
        out.push_back(result->starts[i].front());
    }
    return out;
}

std::expected<std::vector<Ticks>, Error> MulticamAudioSync::offsets(const ProjectState& state,
                                                                    std::span<const MediaId> angles) const {
    return offsets(state, angles, AnalysisControl{});
}

std::expected<std::vector<Ticks>, Error> MulticamAudioSync::offsets(const ProjectState& state, std::span<const MediaId> angles,
                                                                    const AnalysisControl& control) const {
    if (!m_sync) {
        return std::unexpected(Error(ErrorCode::Unsupported, "no audio sync analysis here"));
    }
    return angleOffsets(*m_sync, state, angles, control.stop, control.progress);
}

} // namespace wabcut::audio::edit
