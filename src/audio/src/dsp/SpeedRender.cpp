#include <wabcut/audio/dsp/SpeedRender.h>
#include <wabcut/audio/dsp/Equalizer.h> // kMaxDspChannels

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

namespace wabcut::audio::dsp {

namespace {

std::int64_t floorDiv(std::int64_t v, std::int64_t d) { return v >= 0 ? v / d : -((-v + d - 1) / d); }
std::int64_t floorMod(std::int64_t v, std::int64_t d) { return v - floorDiv(v, d) * d; }

/// A window of source frames [base, base + count) read through a SpeedSource, with a mono mix for the similarity
/// search. Moving it keeps the frames it already holds, so a renderer walking through the source reads every frame
/// about once.
class SourceCache {
public:
    explicit SourceCache(int channels) : m_channels(channels) {}

    void clear() noexcept { m_count = 0; }

    /// Makes [lo, hi) available; the read reaches ahead in the direction of travel (`direction` < 0: backwards).
    void ensure(std::int64_t lo, std::int64_t hi, SpeedSource& source, int direction) {
        if (m_count > 0 && lo >= m_base && hi <= m_base + m_count) {
            return;
        }
        const std::int64_t ahead = std::max<std::int64_t>(4096, hi - lo);
        const std::int64_t nl = direction < 0 ? lo - ahead : lo - 64;
        const std::int64_t nh = direction < 0 ? hi + 64 : hi + ahead;
        const auto ch = static_cast<std::size_t>(m_channels);
        const auto n = static_cast<std::size_t>(nh - nl);
        m_next.resize(n * ch);
        m_nextMono.resize(n);
        std::int64_t ol = nh;
        std::int64_t oh = nh;
        if (m_count > 0) {
            ol = std::max(nl, m_base);
            oh = std::min(nh, m_base + m_count);
            if (oh <= ol) {
                ol = oh = nh;
            }
        }
        const auto readInto = [&](std::int64_t a, std::int64_t b) {
            if (b <= a) {
                return;
            }
            float* dst = m_next.data() + static_cast<std::size_t>(a - nl) * ch;
            source.read(a, b - a, dst);
            for (std::int64_t f = a; f < b; ++f) {
                const float* s = m_next.data() + static_cast<std::size_t>(f - nl) * ch;
                float m = 0.0f;
                for (std::size_t c = 0; c < ch; ++c) {
                    m += s[c];
                }
                m_nextMono[static_cast<std::size_t>(f - nl)] = m;
            }
        };
        if (ol < oh) {
            readInto(nl, ol);
            std::memcpy(m_next.data() + static_cast<std::size_t>(ol - nl) * ch,
                        m_frames.data() + static_cast<std::size_t>(ol - m_base) * ch,
                        static_cast<std::size_t>(oh - ol) * ch * sizeof(float));
            std::memcpy(m_nextMono.data() + static_cast<std::size_t>(ol - nl), m_mono.data() + static_cast<std::size_t>(ol - m_base),
                        static_cast<std::size_t>(oh - ol) * sizeof(float));
            readInto(oh, nh);
        } else {
            readInto(nl, nh);
        }
        std::swap(m_frames, m_next);
        std::swap(m_mono, m_nextMono);
        m_base = nl;
        m_count = nh - nl;
    }

    const float* frame(std::int64_t f) const noexcept {
        return m_frames.data() + static_cast<std::size_t>(f - m_base) * static_cast<std::size_t>(m_channels);
    }
    const float* mono(std::int64_t f) const noexcept { return m_mono.data() + static_cast<std::size_t>(f - m_base); }

private:
    int m_channels;
    std::int64_t m_base = 0;
    std::int64_t m_count = 0;
    std::vector<float> m_frames;
    std::vector<float> m_mono;
    std::vector<float> m_next;
    std::vector<float> m_nextMono;
};

/// Kaiser-windowed sinc, tabulated over its zero crossings (beta 8.5: about -85 dB stopband).
class SincKernel {
public:
    static constexpr int kZeros = 12;
    static constexpr int kSteps = 512;

    SincKernel() {
        constexpr double beta = 8.5;
        const auto bessel0 = [](double x) { // I0 by its series
            double sum = 1.0;
            double term = 1.0;
            for (int k = 1; k < 50; ++k) {
                term *= (x / (2.0 * k)) * (x / (2.0 * k));
                sum += term;
                if (term < sum * 1e-17) {
                    break;
                }
            }
            return sum;
        };
        const double norm = bessel0(beta);
        m_table.resize(static_cast<std::size_t>(kZeros * kSteps + 2), 0.0);
        for (int i = 0; i <= kZeros * kSteps; ++i) {
            const double x = static_cast<double>(i) / kSteps;
            const double u = x / kZeros;
            const double window = bessel0(beta * std::sqrt(std::max(0.0, 1.0 - u * u))) / norm;
            const double sinc = i == 0 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
            m_table[static_cast<std::size_t>(i)] = sinc * window;
        }
        m_table[static_cast<std::size_t>(kZeros * kSteps)] = 0.0;
    }

    /// The kernel at `x` zero crossings from its centre.
    double at(double x) const noexcept {
        x = std::fabs(x);
        if (x >= kZeros) {
            return 0.0;
        }
        const double p = x * kSteps;
        const auto i = static_cast<std::size_t>(p);
        const double f = p - static_cast<double>(i);
        return m_table[i] + (m_table[i + 1] - m_table[i]) * f;
    }

private:
    std::vector<double> m_table;
};

const SincKernel& sincKernel() {
    static const SincKernel kernel;
    return kernel;
}

// ---- Varispeed ----------------------------------------------------------------------------------------------------

class Varispeed final : public SpeedRenderer {
public:
    /// Frames over which a piece fades in before its start and out after its end (about 0.7 ms at 48 kHz).
    static constexpr std::int64_t kMargin = 32;
    /// The speed gain is averaged over this many frames on each side (2 ms in all at 48 kHz): a speed that jumps out of
    /// the audible range (100 % into an ease that starts from a standstill) fades instead of clicking.
    static constexpr std::int64_t kGainSmoothing = 48;

    explicit Varispeed(int channels) : m_channels(channels), m_cache(channels) {}

    void setPiece(std::int64_t begin, std::int64_t end) override {
        if (begin != m_begin || end != m_end) {
            m_begin = begin;
            m_end = std::max(end, begin + 1);
            m_cache.clear();
        }
    }
    std::int64_t extentBegin() const noexcept override { return m_begin - kMargin; }
    std::int64_t extentEnd() const noexcept override { return m_end + kMargin; }
    double coverage(std::int64_t f) const noexcept override {
        if (f < m_begin - kMargin || f >= m_end + kMargin) {
            return 0.0;
        }
        if (f < m_begin) {
            return static_cast<double>(f - (m_begin - kMargin) + 1) / static_cast<double>(kMargin + 1);
        }
        if (f >= m_end) {
            return static_cast<double>(m_end + kMargin - f) / static_cast<double>(kMargin + 1);
        }
        return 1.0;
    }

    void render(std::int64_t first, int count, const SpeedPositions& positions, SpeedSource& source,
                float* out) override {
        const auto ch = static_cast<std::size_t>(m_channels);
        std::fill(out, out + static_cast<std::size_t>(std::max(count, 0)) * ch, 0.0f);
        const std::int64_t from = std::max(first, extentBegin());
        const std::int64_t to = std::min(first + count, extentEnd());
        if (to <= from) {
            return;
        }
        const auto n = static_cast<std::size_t>(to - from);
        constexpr auto side = static_cast<std::size_t>(kGainSmoothing);
        // Positions of frames [from - side - 1, to + side + 1): frame from + i is m_positions[i + side + 1].
        m_positions.resize(n + 2 * side + 2);
        positions.fill(from - kGainSmoothing - 1, static_cast<int>(n + 2 * side + 2), m_positions.data());
        // The local speed of frame from - side + j: the larger of the steps to its neighbours. A turnaround (reverse into
        // forward) repeats one position, so the smaller step can be 0 there; the larger keeps the frame audible.
        const auto speedAt = [&](std::size_t j) {
            return std::max(std::fabs(m_positions[j + 1] - m_positions[j]), std::fabs(m_positions[j + 2] - m_positions[j + 1]));
        };
        // The speed gain averaged over 2w + 1 frames (a running sum over the block's frames and side on each side).
        m_gains.resize(n + 2 * side + 1);
        m_gains[0] = 0.0;
        for (std::size_t j = 0; j < n + 2 * side; ++j) {
            m_gains[j + 1] = m_gains[j] + audibleSpeedGain(speedAt(j));
        }
        const auto gainOf = [&](std::size_t i) { // frame from + i
            return (m_gains[i + 2 * side + 1] - m_gains[i]) / static_cast<double>(2 * side + 1);
        };
        // The source frames this block reads: every audible position plus the kernel's reach at its cutoff.
        double lo = std::numeric_limits<double>::max();
        double hi = std::numeric_limits<double>::lowest();
        double reach = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            if (gainOf(i) <= 1e-12) {
                continue;
            }
            const double q = m_positions[i + side + 1];
            lo = std::min(lo, q);
            hi = std::max(hi, q);
            reach = std::max(reach, SincKernel::kZeros * std::max(1.0, std::min(speedAt(i + side), kMaxAudibleSpeed * 1.2)));
        }
        if (hi < lo) {
            return; // silent all through (a speed out of range)
        }
        const int direction = m_positions[n + side + 1] >= m_positions[side + 1] ? 1 : -1;
        m_cache.ensure(static_cast<std::int64_t>(std::floor(lo - reach)) - 2, static_cast<std::int64_t>(std::ceil(hi + reach)) + 3,
                       source, direction);
        const SincKernel& kernel = sincKernel();
        std::array<double, kMaxDspChannels> acc{};
        for (std::size_t i = 0; i < n; ++i) {
            const std::int64_t f = from + static_cast<std::int64_t>(i);
            const double q = m_positions[i + side + 1];
            const double v = std::min(speedAt(i + side), kMaxAudibleSpeed * 1.2);
            const double smoothed = gainOf(i);
            const double gain = coverage(f) * (smoothed > 1.0 - 1e-12 ? 1.0 : smoothed);
            if (gain <= 1e-12) {
                continue;
            }
            float* dst = out + static_cast<std::size_t>(f - first) * ch;
            // Slowing down needs no band limit (cutoff at Nyquist); speeding up lowers the cutoff to 1 / speed.
            const double cutoff = v > 1.0 ? 1.0 / v : 1.0;
            if (cutoff == 1.0 && q == std::floor(q)) { // a whole-frame position at up to 1x: the sample itself
                const float* s = m_cache.frame(static_cast<std::int64_t>(q));
                for (std::size_t c = 0; c < ch; ++c) {
                    dst[c] = static_cast<float>(s[c] * gain);
                }
                continue;
            }
            const double half = SincKernel::kZeros / cutoff;
            const auto k0 = static_cast<std::int64_t>(std::ceil(q - half));
            const auto k1 = static_cast<std::int64_t>(std::floor(q + half));
            std::fill_n(acc.begin(), ch, 0.0);
            double weights = 0.0;
            for (std::int64_t k = k0; k <= k1; ++k) {
                const double w = kernel.at((q - static_cast<double>(k)) * cutoff);
                if (w == 0.0) {
                    continue;
                }
                weights += w;
                const float* s = m_cache.frame(k);
                for (std::size_t c = 0; c < ch; ++c) {
                    acc[c] += w * s[c];
                }
            }
            if (weights == 0.0) {
                continue;
            }
            const double scale = gain / weights;
            for (std::size_t c = 0; c < ch; ++c) {
                dst[c] = static_cast<float>(acc[c] * scale);
            }
        }
    }

    void reset() noexcept override { m_cache.clear(); }

private:
    int m_channels;
    std::int64_t m_begin = 0;
    std::int64_t m_end = 1;
    SourceCache m_cache;
    std::vector<double> m_positions;
    std::vector<double> m_gains; ///< running sum of the speed gains (gainOf)
};

// ---- PitchStretch -------------------------------------------------------------------------------------------------

int stretchHop(double sampleRate, const PitchStretchOptions& options) {
    const double fs = sampleRate > 0.0 ? sampleRate : 48000.0;
    return std::max(32, static_cast<int>(std::lround(options.windowMs * 0.001 * fs / 2.0)));
}

class PitchStretch final : public SpeedRenderer {
public:
    PitchStretch(int channels, double sampleRate, const PitchStretchOptions& options)
        : m_channels(channels), m_cache(channels) {
        const double fs = sampleRate > 0.0 ? sampleRate : 48000.0;
        m_hop = stretchHop(fs, options);
        m_window = 2 * m_hop;
        m_tolerance = std::clamp(static_cast<int>(std::lround(options.toleranceMs * 0.001 * fs)), 4, m_hop);
        const double hopSeconds = static_cast<double>(m_hop) / fs;
        m_resync = std::max<std::int64_t>(4, std::llround(std::max(options.resyncSeconds, 0.0) / hopSeconds));
        m_hann.resize(static_cast<std::size_t>(m_window));
        for (int i = 0; i < m_window; ++i) { // periodic Hann: overlapping by half it sums to 1
            m_hann[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / m_window));
        }
        m_acc.assign(static_cast<std::size_t>(m_window) * static_cast<std::size_t>(m_channels), 0.0f);
    }

    void setPiece(std::int64_t begin, std::int64_t end) override {
        end = std::max(end, begin + 1);
        if (begin == m_begin && end == m_end) {
            return;
        }
        m_begin = begin;
        m_end = end;
        // The first hop's rising half ends at or before `begin`, the last hop's falling half starts at or after `end`.
        m_firstHop = floorDiv(begin, m_hop) - 1;
        m_lastHop = floorDiv(end + m_hop - 1, m_hop) - 1;
        m_chainValid = false;
        m_cache.clear();
    }
    std::int64_t extentBegin() const noexcept override { return m_firstHop * m_hop; }
    std::int64_t extentEnd() const noexcept override { return (m_lastHop + 2) * m_hop; }
    double coverage(std::int64_t f) const noexcept override {
        double g = 0.0;
        const std::int64_t k1 = floorDiv(f, m_hop);
        for (std::int64_t k = k1 - 1; k <= k1; ++k) {
            if (k >= m_firstHop && k <= m_lastHop) {
                g += m_hann[static_cast<std::size_t>(f - k * m_hop)];
            }
        }
        return g;
    }

    void render(std::int64_t first, int count, const SpeedPositions& positions, SpeedSource& source,
                float* out) override {
        const auto ch = static_cast<std::size_t>(m_channels);
        std::fill(out, out + static_cast<std::size_t>(std::max(count, 0)) * ch, 0.0f);
        std::int64_t f = std::max(first, extentBegin());
        const std::int64_t stop = std::min(first + count, extentEnd());
        while (f < stop) {
            const std::int64_t h = floorDiv(f, m_hop);
            if (!m_chainValid || h < m_computed || h - m_computed > m_resync + 2) {
                restart(h, positions, source);
            } else if (h > m_computed) {
                advance(h, positions, source);
            }
            const std::int64_t o = m_computed * m_hop;
            const std::int64_t n = std::min(stop, o + m_hop) - f;
            std::memcpy(out + static_cast<std::size_t>(f - first) * ch, m_acc.data() + static_cast<std::size_t>(f - o) * ch,
                        static_cast<std::size_t>(n) * ch * sizeof(float));
            f += n;
        }
    }

    void reset() noexcept override {
        m_chainValid = false;
        m_cache.clear();
    }

    std::int64_t hopsComputed() const noexcept override { return m_hops; }

private:
    /// Starts the chain at the last fresh hop (the piece's first, or a resync hop) before hop h - 1, then computes up to h.
    void restart(std::int64_t h, const SpeedPositions& positions, SpeedSource& source) {
        const std::int64_t start = std::max(m_firstHop, floorDiv(h - 1, m_resync) * m_resync);
        std::fill(m_acc.begin(), m_acc.end(), 0.0f);
        m_computed = start - 1;
        m_havePrev = false;
        m_chainValid = true;
        advance(h, positions, source);
    }

    /// Computes hops m_computed + 1 .. h; afterwards frames [h * hop, (h + 1) * hop) are final in m_acc[0, hop).
    void advance(std::int64_t h, const SpeedPositions& positions, SpeedSource& source) {
        const auto ch = static_cast<std::size_t>(m_channels);
        const auto hop = static_cast<std::size_t>(m_hop);
        while (m_computed < h) {
            std::memmove(m_acc.data(), m_acc.data() + hop * ch, hop * ch * sizeof(float));
            std::fill(m_acc.begin() + static_cast<std::ptrdiff_t>(hop * ch), m_acc.end(), 0.0f);
            ++m_computed;
            if (m_computed >= m_firstHop && m_computed <= m_lastHop) {
                addHop(m_computed, positions, source);
            }
        }
    }

    /// Normalised similarity of the candidate (its frames read with `step` from `candidate`) with the template (from
    /// `natural`, same step) over one hop; `stride` subsamples the sum.
    double similarity(std::int64_t natural, std::int64_t candidate, int step, int stride) const {
        double dot = 0.0;
        double energy = 1e-9;
        for (int i = 0; i < m_hop; i += stride) {
            const double t = *m_cache.mono(natural + static_cast<std::int64_t>(step) * i);
            const double c = *m_cache.mono(candidate + static_cast<std::int64_t>(step) * i);
            dot += t * c;
            energy += c * c;
        }
        return dot / std::sqrt(energy);
    }

    void addHop(std::int64_t k, const SpeedPositions& positions, SpeedSource& source) {
        const std::int64_t centre = k * m_hop + m_hop;
        std::array<double, 3> p{};
        positions.fill(centre - 1, 3, p.data());
        // The larger step to a neighbour (see Varispeed): a turnaround at the centre keeps its speed and direction.
        const double before = p[1] - p[0];
        const double after = p[2] - p[1];
        const double speed = std::fabs(after) >= std::fabs(before) ? after : before;
        const double gain = audibleSpeedGain(std::fabs(speed));
        if (gain <= 0.0) {
            m_havePrev = false;
            return;
        }
        const int dir = speed > 0.0 ? 1 : -1;
        const std::int64_t target = std::llround(p[1]);
        const bool fresh = k == m_firstHop || floorMod(k, m_resync) == 0 || !m_havePrev || dir != m_prevDir;
        const std::int64_t tol = m_tolerance;
        const std::int64_t hop = m_hop;
        // Forward, the segment is frames [anchor, anchor + N) played forwards; in reverse, frames (anchor - N, anchor]
        // played backwards. The nominal anchor centres the segment on the target; the search moves it by up to tol.
        const std::int64_t nominal = dir > 0 ? target - hop : target + hop;
        std::int64_t lo = nominal - tol - m_window;
        std::int64_t hi = nominal + tol + m_window + 1;
        // The template: the previous segment continued by one hop.
        const std::int64_t natural = dir > 0 ? m_prevAnchor + hop : m_prevAnchor - hop;
        if (!fresh) {
            lo = std::min(lo, natural - hop - 1);
            hi = std::max(hi, natural + hop + 1);
        }
        m_cache.ensure(lo, hi, source, dir);
        std::int64_t anchor = nominal;
        if (!fresh) {
            // Coarse pass every 4th offset (subsampled sum), then refine around the best. Ties keep the offset nearest
            // the nominal one (silence, a gated mic: every candidate scores 0 and the timing stays exact).
            double best = similarity(natural, nominal, dir, 2);
            for (std::int64_t d = 4; d <= tol; d += 4) {
                for (const std::int64_t candidate : {nominal - d, nominal + d}) {
                    const double s = similarity(natural, candidate, dir, 2);
                    if (s > best) {
                        best = s;
                        anchor = candidate;
                    }
                }
            }
            const std::int64_t around = anchor;
            best = similarity(natural, around, dir, 1);
            for (std::int64_t d = 1; d <= 3; ++d) {
                for (const std::int64_t candidate : {around - d, around + d}) {
                    if (candidate < nominal - tol || candidate > nominal + tol) {
                        continue;
                    }
                    const double s = similarity(natural, candidate, dir, 1);
                    if (s > best) {
                        best = s;
                        anchor = candidate;
                    }
                }
            }
        }
        const auto ch = static_cast<std::size_t>(m_channels);
        const auto g = static_cast<float>(gain);
        for (int n = 0; n < m_window; ++n) {
            const float w = m_hann[static_cast<std::size_t>(n)] * g;
            const float* s = m_cache.frame(anchor + static_cast<std::int64_t>(dir) * n);
            float* a = m_acc.data() + static_cast<std::size_t>(n) * ch;
            for (std::size_t c = 0; c < ch; ++c) {
                a[c] += w * s[c];
            }
        }
        m_prevAnchor = anchor;
        m_prevDir = dir;
        m_havePrev = true;
        ++m_hops;
    }

    int m_channels;
    int m_hop = 720;
    int m_window = 1440;
    int m_tolerance = 360;
    std::int64_t m_resync = 64;
    std::vector<float> m_hann;

    std::int64_t m_begin = 0;
    std::int64_t m_end = 1;
    std::int64_t m_firstHop = -1;
    std::int64_t m_lastHop = -1;

    bool m_chainValid = false;
    std::int64_t m_computed = 0; ///< the last hop computed: m_acc holds frames [m_computed * hop, + N)
    bool m_havePrev = false;
    int m_prevDir = 1;
    std::int64_t m_prevAnchor = 0;
    std::vector<float> m_acc;
    SourceCache m_cache;
    std::int64_t m_hops = 0;
};

} // namespace

double audibleSpeedGain(double speed) noexcept {
    speed = std::fabs(speed);
    if (!std::isfinite(speed) || speed < kMinAudibleSpeed * 0.8 || speed >= kMaxAudibleSpeed * 1.2) {
        return 0.0;
    }
    if (speed < kMinAudibleSpeed) {
        return (speed - kMinAudibleSpeed * 0.8) / (kMinAudibleSpeed * 0.2);
    }
    if (speed > kMaxAudibleSpeed) {
        return (kMaxAudibleSpeed * 1.2 - speed) / (kMaxAudibleSpeed * 0.2);
    }
    return 1.0;
}

std::unique_ptr<SpeedRenderer> makeSpeedRenderer(SpeedMode mode, int channels, double sampleRate,
                                                 const PitchStretchOptions& options) {
    channels = std::clamp(channels, 1, kMaxDspChannels);
    if (mode == SpeedMode::Varispeed) {
        return std::make_unique<Varispeed>(channels);
    }
    return std::make_unique<PitchStretch>(channels, sampleRate, options);
}

std::int64_t minimumPieceGap(SpeedMode mode, double sampleRate, const PitchStretchOptions& options) {
    // A piece's extent reaches up to two hops (the stretch) or the margin (varispeed) past each side.
    return mode == SpeedMode::Varispeed ? 2 * Varispeed::kMargin + 2 : 4 * std::int64_t{stretchHop(sampleRate, options)} + 2;
}

} // namespace wabcut::audio::dsp
