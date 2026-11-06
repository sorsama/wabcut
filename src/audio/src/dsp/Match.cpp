#include <wabcut/audio/dsp/Match.h>

#include <wabcut/audio/dsp/Fft.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace wabcut::audio::dsp {

namespace {

constexpr double kNegInf = -std::numeric_limits<double>::infinity();
/// Frames kept for the average (a programme this long is analysed with a stride; frames past it are ignored).
constexpr std::size_t kMaxFrames = 60000;

double powerDb(double power) { return 10.0 * std::log10(std::max(power, 1e-30)); }

struct BandBins {
    int lo = 0;
    int hi = -1; ///< hi < lo: the band is above the usable range
};

} // namespace

double spectrumBandHz(int band) noexcept { return 31.25 * std::pow(2.0, static_cast<double>(band) / 3.0); }

// ---- SpectrumAverager ----------------------------------------------------------------------------------------------

struct SpectrumAverager::Impl {
    struct Frame {
        float level; ///< mean square of the frame's samples, dB
        std::array<float, kSpectrumBands> band;
    };

    Impl(int ch, int rate, int stride_)
        : channels(std::clamp(ch, 1, 8)), sampleRate(std::max(rate, 8000)), stride(std::clamp(stride_, 1, 64)),
          size(SpectrumAverager::fftSizeFor(sampleRate)), hop(size / 2), fft(size), window(static_cast<std::size_t>(size)),
          buffer(static_cast<std::size_t>(channels)), spectrum(static_cast<std::size_t>(fft.bins())),
          power(static_cast<std::size_t>(fft.bins())), scratch(static_cast<std::size_t>(size)) {
        double sum = 0.0;
        for (int i = 0; i < size; ++i) {
            window[static_cast<std::size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / size);
            sum += window[static_cast<std::size_t>(i)];
        }
        scale = 4.0 / (sum * sum);
        const double binHz = static_cast<double>(sampleRate) / size;
        for (int b = 0; b < kSpectrumBands; ++b) {
            const double fc = spectrumBandHz(b);
            if (fc * std::pow(2.0, 1.0 / 6.0) > 0.45 * sampleRate) {
                continue; // above what the signal can tell
            }
            int lo = static_cast<int>(std::ceil(fc * std::pow(2.0, -1.0 / 6.0) / binHz));
            int hi = static_cast<int>(std::floor(fc * std::pow(2.0, 1.0 / 6.0) / binHz));
            if (hi < lo) {
                lo = hi = static_cast<int>(std::lround(fc / binHz));
            }
            bins[static_cast<std::size_t>(b)] = {std::max(lo, 1), std::min(hi, fft.bins() - 1)};
        }
    }

    bool counts(int channel) const { return !(channels == 6 && channel == 3); } // the LFE does not shape the tone

    void process() {
        std::fill(power.begin(), power.end(), 0.0);
        double meanSquare = 0.0;
        int used = 0;
        for (int c = 0; c < channels; ++c) {
            if (!counts(c)) {
                continue;
            }
            const std::vector<double>& in = buffer[static_cast<std::size_t>(c)];
            for (int i = 0; i < size; ++i) {
                const double v = in[static_cast<std::size_t>(start + i)];
                scratch[static_cast<std::size_t>(i)] = v * window[static_cast<std::size_t>(i)];
                meanSquare += v * v;
            }
            fft.forwardReal(scratch.data(), spectrum.data());
            for (std::size_t k = 0; k < spectrum.size(); ++k) {
                power[k] += std::norm(spectrum[k]);
            }
            ++used;
        }
        if (used == 0 || frames.size() >= kMaxFrames) {
            return;
        }
        Frame f{};
        f.level = static_cast<float>(powerDb(meanSquare / (static_cast<double>(size) * used)));
        for (int b = 0; b < kSpectrumBands; ++b) {
            const BandBins& range = bins[static_cast<std::size_t>(b)];
            if (range.hi < range.lo) {
                f.band[static_cast<std::size_t>(b)] = -1000.0F;
                continue;
            }
            double sum = 0.0;
            for (int k = range.lo; k <= range.hi; ++k) {
                sum += power[static_cast<std::size_t>(k)];
            }
            f.band[static_cast<std::size_t>(b)] =
                static_cast<float>(sum / (range.hi - range.lo + 1) / used * scale); // linear power per bin
        }
        frames.push_back(f);
    }

    void add(const float* interleaved, std::size_t count) {
        const auto ch = static_cast<std::size_t>(channels);
        for (std::size_t c = 0; c < ch; ++c) {
            std::vector<double>& b = buffer[c];
            b.reserve(b.size() + count);
            for (std::size_t i = 0; i < count; ++i) {
                b.push_back(static_cast<double>(interleaved[i * ch + c]));
            }
        }
        total += static_cast<std::int64_t>(count);
        while (static_cast<int>(buffer[0].size()) - start >= size) {
            if (frameIndex % stride == 0) {
                process();
            }
            ++frameIndex;
            start += hop;
            if (start >= size * 2) { // forget what no frame needs any more
                for (std::vector<double>& b : buffer) {
                    b.erase(b.begin(), b.begin() + start);
                }
                start = 0;
            }
        }
    }

    int channels;
    int sampleRate;
    int stride;
    int size;
    int hop;
    Fft fft;
    std::vector<double> window;
    double scale = 1.0;
    std::array<BandBins, kSpectrumBands> bins{};
    std::vector<std::vector<double>> buffer;
    int start = 0; ///< index in `buffer` of the next frame
    std::int64_t frameIndex = 0;
    std::int64_t total = 0;
    std::vector<Fft::Complex> spectrum;
    std::vector<double> power;
    std::vector<double> scratch;
    std::vector<Frame> frames;
};

int SpectrumAverager::fftSizeFor(int sampleRate) noexcept { return sampleRate <= 48000 ? 8192 : sampleRate <= 96000 ? 16384 : 32768; }

SpectrumAverager::SpectrumAverager(int channels, int sampleRate, int stride)
    : m_impl(std::make_unique<Impl>(channels, sampleRate, stride)) {}
SpectrumAverager::~SpectrumAverager() = default;
SpectrumAverager::SpectrumAverager(SpectrumAverager&&) noexcept = default;
SpectrumAverager& SpectrumAverager::operator=(SpectrumAverager&&) noexcept = default;

void SpectrumAverager::add(const float* interleaved, std::size_t frames) {
    if (frames > 0) {
        m_impl->add(interleaved, frames);
    }
}

SpectrumAverage SpectrumAverager::result() const {
    const Impl& d = *m_impl;
    SpectrumAverage out;
    out.sampleRate = d.sampleRate;
    out.seconds = static_cast<double>(d.total) / d.sampleRate;
    if (d.frames.empty()) {
        return out;
    }
    // The gate: frames within 25 dB of the loud ones (the 95th percentile of the frame levels), never under -70 dB.
    std::vector<float> levels;
    levels.reserve(d.frames.size());
    for (const Impl::Frame& f : d.frames) {
        levels.push_back(f.level);
    }
    const auto top = levels.begin() + static_cast<std::ptrdiff_t>((levels.size() - 1) * 95 / 100);
    std::nth_element(levels.begin(), top, levels.end());
    const double gate = std::max(-70.0, static_cast<double>(*top) - 25.0);
    std::array<double, kSpectrumBands> sum{};
    std::size_t active = 0;
    for (const Impl::Frame& f : d.frames) {
        if (f.level < gate) {
            continue;
        }
        ++active;
        for (std::size_t b = 0; b < sum.size(); ++b) {
            sum[b] += static_cast<double>(f.band[b]);
        }
    }
    if (active == 0) {
        return out;
    }
    out.activeSeconds = static_cast<double>(active) * d.stride * d.hop / d.sampleRate;
    for (int b = 0; b < kSpectrumBands; ++b) {
        const BandBins& range = d.bins[static_cast<std::size_t>(b)];
        if (range.hi >= range.lo) {
            out.levelDb[static_cast<std::size_t>(b)] = powerDb(sum[static_cast<std::size_t>(b)] / static_cast<double>(active));
        }
    }
    return out;
}

// ---- Levels ---------------------------------------------------------------------------------------------------------

LevelAccumulator::LevelAccumulator(int channels) : m_channels(std::clamp(channels, 1, 8)) {}

void LevelAccumulator::add(const float* interleaved, std::size_t frames) {
    const auto ch = static_cast<std::size_t>(m_channels);
    for (std::size_t f = 0; f < frames; ++f) {
        for (std::size_t c = 0; c < ch; ++c) {
            if (m_channels == 6 && c == 3) {
                continue;
            }
            const double v = static_cast<double>(interleaved[f * ch + c]);
            m_sumSquares += v * v;
            ++m_samples;
            m_peak = std::max(m_peak, std::fabs(v));
        }
    }
    m_frames += static_cast<std::int64_t>(frames);
}

double LevelAccumulator::rmsDbfs() const noexcept {
    return m_samples > 0 && m_sumSquares > 0.0 ? 10.0 * std::log10(m_sumSquares / static_cast<double>(m_samples)) : kNegInf;
}
double LevelAccumulator::samplePeakDbfs() const noexcept { return m_peak > 0.0 ? 20.0 * std::log10(m_peak) : kNegInf; }
double LevelAccumulator::crestDb() const noexcept {
    const double rms = rmsDbfs();
    return std::isfinite(rms) && m_peak > 0.0 ? samplePeakDbfs() - rms : 0.0;
}

} // namespace wabcut::audio::dsp
