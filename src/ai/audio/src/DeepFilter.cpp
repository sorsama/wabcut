#include <wabcut/ai/audio/DeepFilter.h>

#include "Fft.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>

namespace wabcut::ai::audio::dfn {

namespace {

constexpr float kMeanNormInit[2] = {-60.0f, -90.0f};
constexpr float kUnitNormInit[2] = {0.001f, 0.0001f};

float freq2erb(float hz) {
    return 9.265f * std::log1p(hz / (24.7f * 9.265f));
}
float erb2freq(float erb) {
    return 24.7f * 9.265f * (std::exp(erb / 9.265f) - 1.0f);
}

/// 1 / (fft_size^2 / (2 hop)): libDF's analysis normalisation (its inverse FFT is unnormalised).
constexpr float kNorm = 1.0f / (static_cast<float>(kFftSize * kFftSize) / static_cast<float>(2 * kHop));

detail::MixedFft& fft() {
    thread_local detail::MixedFft plan(kFftSize);
    return plan;
}

} // namespace

std::vector<int> erbWidths() {
    const float freqWidth = static_cast<float>(kSampleRate) / static_cast<float>(kFftSize);
    const float erbLow = freq2erb(0.0f);
    const float erbHigh = freq2erb(static_cast<float>(kSampleRate / 2));
    const float step = (erbHigh - erbLow) / static_cast<float>(kErbBands);
    std::vector<int> widths(kErbBands, 0);
    int previous = 0; // last bin of the previous band
    int over = 0;     // bins already given to earlier bands
    for (int i = 1; i <= kErbBands; ++i) {
        const float f = erb2freq(erbLow + static_cast<float>(i) * step);
        const int fb = static_cast<int>(std::round(f / freqWidth));
        int count = fb - previous - over;
        if (count < kMinErbBins) {
            over = kMinErbBins - count;
            count = kMinErbBins;
        } else {
            over = 0;
        }
        widths[static_cast<std::size_t>(i - 1)] = count;
        previous = fb;
    }
    widths.back() += 1; // fft_size / 2 + 1 bins
    int total = 0;
    for (const int w : widths) {
        total += w;
    }
    if (total > kBins) {
        widths.back() -= total - kBins;
    }
    return widths;
}

const std::vector<float>& window() {
    static const std::vector<float> w = [] {
        std::vector<float> out(kFftSize);
        const double half = kFftSize / 2;
        for (int i = 0; i < kFftSize; ++i) {
            const double s = std::sin(0.5 * std::numbers::pi * (i + 0.5) / half);
            out[static_cast<std::size_t>(i)] = static_cast<float>(std::sin(0.5 * std::numbers::pi * s * s));
        }
        return out;
    }();
    return w;
}

int framesFor(std::size_t samples) {
    return samples == 0 ? 0 : static_cast<int>((samples - 1) / kHop) + 2;
}

Spectrogram analyse(std::span<const float> samples, int frames) {
    Spectrogram out;
    out.frames = std::max(frames, 0);
    out.bins.assign(static_cast<std::size_t>(out.frames) * kBins, Complex{});
    const std::vector<float>& w = window();
    std::vector<std::complex<double>> in(kFftSize);
    std::vector<std::complex<double>> spectrum(kFftSize);
    const auto n = static_cast<std::ptrdiff_t>(samples.size());
    for (int t = 0; t < out.frames; ++t) {
        const std::ptrdiff_t first = static_cast<std::ptrdiff_t>(t - 1) * kHop;
        for (int j = 0; j < kFftSize; ++j) {
            const std::ptrdiff_t at = first + j;
            const float x = at >= 0 && at < n ? samples[static_cast<std::size_t>(at)] : 0.0f;
            in[static_cast<std::size_t>(j)] = {static_cast<double>(x * w[static_cast<std::size_t>(j)]), 0.0};
        }
        fft().forward(in, spectrum);
        Complex* bins = out.frame(t);
        for (int k = 0; k < kBins; ++k) {
            const std::complex<double>& c = spectrum[static_cast<std::size_t>(k)];
            bins[k] = Complex(static_cast<float>(c.real()), static_cast<float>(c.imag())) * kNorm;
        }
    }
    return out;
}

std::vector<float> synthesise(const Spectrogram& spectra, std::size_t samples) {
    std::vector<float> out(samples, 0.0f);
    const std::vector<float>& w = window();
    std::vector<std::complex<double>> full(kFftSize);
    std::vector<std::complex<double>> time(kFftSize);
    const auto n = static_cast<std::ptrdiff_t>(samples);
    for (int t = 0; t < spectra.frames; ++t) {
        const std::ptrdiff_t first = static_cast<std::ptrdiff_t>(t - 1) * kHop;
        if (first >= n) {
            break;
        }
        const Complex* bins = spectra.frame(t);
        for (int k = 0; k < kBins; ++k) {
            full[static_cast<std::size_t>(k)] = {bins[k].real(), bins[k].imag()};
        }
        // The inverse of a real FFT ignores the imaginary parts of DC and Nyquist (realfft's convention).
        full[0] = {full[0].real(), 0.0};
        full[kBins - 1] = {full[kBins - 1].real(), 0.0};
        for (int k = 1; k < kBins - 1; ++k) {
            full[static_cast<std::size_t>(kFftSize - k)] = std::conj(full[static_cast<std::size_t>(k)]);
        }
        fft().inverse(full, time);
        for (int j = 0; j < kFftSize; ++j) {
            const std::ptrdiff_t at = first + j;
            if (at >= 0 && at < n) {
                out[static_cast<std::size_t>(at)] +=
                    static_cast<float>(time[static_cast<std::size_t>(j)].real()) * w[static_cast<std::size_t>(j)];
            }
        }
    }
    return out;
}

Features features(const Spectrogram& spectra) {
    static const std::vector<int> widths = erbWidths();
    Features out;
    out.frames = spectra.frames;
    const auto frames = static_cast<std::size_t>(spectra.frames);
    out.erb.assign(frames * kErbBands, 0.0f);
    out.spec.assign(2 * frames * kDfBins, 0.0f);
    float meanState[kErbBands];
    for (int i = 0; i < kErbBands; ++i) {
        meanState[i] = kMeanNormInit[0] + static_cast<float>(i) * ((kMeanNormInit[1] - kMeanNormInit[0]) / static_cast<float>(kErbBands - 1));
    }
    float unitState[kDfBins];
    for (int i = 0; i < kDfBins; ++i) {
        unitState[i] = kUnitNormInit[0] + static_cast<float>(i) * ((kUnitNormInit[1] - kUnitNormInit[0]) / static_cast<float>(kDfBins - 1));
    }
    constexpr float a = kNormAlpha;
    for (int t = 0; t < spectra.frames; ++t) {
        const Complex* x = spectra.frame(t);
        // ERB features: band power in dB, exponential mean normalisation, / 40.
        float* erb = out.erb.data() + static_cast<std::size_t>(t) * kErbBands;
        int bin = 0;
        for (int b = 0; b < kErbBands; ++b) {
            const int width = widths[static_cast<std::size_t>(b)];
            const float k = 1.0f / static_cast<float>(width);
            float power = 0.0f;
            for (int j = 0; j < width; ++j, ++bin) {
                power += (x[bin].real() * x[bin].real() + x[bin].imag() * x[bin].imag()) * k;
            }
            float v = std::log10(power + 1e-10f) * 10.0f;
            meanState[b] = v * (1.0f - a) + meanState[b] * a;
            v -= meanState[b];
            erb[b] = v / 40.0f;
        }
        // Complex features: the lowest 96 bins over the square root of their smoothed magnitude.
        float* re = out.spec.data() + static_cast<std::size_t>(t) * kDfBins;
        float* im = out.spec.data() + (frames + static_cast<std::size_t>(t)) * kDfBins;
        for (int f = 0; f < kDfBins; ++f) {
            unitState[f] = std::hypot(x[f].real(), x[f].imag()) * (1.0f - a) + unitState[f] * a;
            const float s = std::sqrt(unitState[f]);
            re[f] = x[f].real() / s;
            im[f] = x[f].imag() / s;
        }
    }
    return out;
}

Spectrogram enhance(const Spectrogram& noisy, const NetworkOutput& net, int frames) {
    static const std::vector<int> widths = erbWidths();
    Spectrogram out;
    out.frames = frames;
    out.bins.assign(static_cast<std::size_t>(frames) * kBins, Complex{});
    for (int tau = 0; tau < frames; ++tau) {
        const int t = tau + kLookahead; // the network frame that has seen tau's lookahead
        const Complex* x = noisy.frame(tau);
        Complex* y = out.frame(tau);
        const float* gains = net.gains.data() + static_cast<std::size_t>(t) * kErbBands;
        int bin = 0;
        for (int b = 0; b < kErbBands; ++b) {
            for (int j = 0; j < widths[static_cast<std::size_t>(b)]; ++j, ++bin) {
                y[bin] = x[bin] * gains[b];
            }
        }
        // Deep filter on the lowest bins: sum over the order of the noisy frames tau - 2 + i times coefficient i.
        const float* coefs = net.coefs.data() + static_cast<std::size_t>(t) * kDfBins * kDfOrder * 2;
        for (int f = 0; f < kDfBins; ++f) {
            Complex sum{};
            for (int i = 0; i < kDfOrder; ++i) {
                const int frame = tau - kDfLookahead + i;
                if (frame < 0 || frame >= noisy.frames) {
                    continue;
                }
                const float* c = coefs + (static_cast<std::size_t>(f) * kDfOrder + static_cast<std::size_t>(i)) * 2;
                sum += noisy.frame(frame)[f] * Complex(c[0], c[1]);
            }
            y[f] = sum;
        }
    }
    return out;
}

std::expected<std::vector<float>, Error> enhanceChannel(std::span<const float> samples, const Network& network) {
    if (samples.empty()) {
        return std::vector<float>{};
    }
    const int frames = framesFor(samples.size());
    const Spectrogram noisy = analyse(samples, frames + kLookahead);
    auto net = network(features(noisy));
    if (!net) {
        return std::unexpected(net.error());
    }
    const auto expected = static_cast<std::size_t>(noisy.frames);
    if (net->frames != noisy.frames || net->gains.size() != expected * kErbBands ||
        net->coefs.size() != expected * kDfBins * kDfOrder * 2) {
        return std::unexpected(Error{ErrorCode::InvalidArgument,
                                     "the speech model returned " + std::to_string(net->frames) + " frames for " +
                                         std::to_string(noisy.frames)});
    }
    return synthesise(enhance(noisy, *net, frames), samples.size());
}

NetworkOutput identityOutput(int frames) {
    NetworkOutput out;
    out.frames = frames;
    const auto n = static_cast<std::size_t>(std::max(frames, 0));
    out.gains.assign(n * kErbBands, 1.0f);
    out.coefs.assign(n * kDfBins * kDfOrder * 2, 0.0f);
    for (std::size_t t = 0; t < n; ++t) {
        for (std::size_t f = 0; f < kDfBins; ++f) {
            out.coefs[((t * kDfBins + f) * kDfOrder + kDfLookahead) * 2] = 1.0f;
        }
    }
    out.lsnr.assign(n, 35.0f);
    return out;
}

} // namespace wabcut::ai::audio::dfn
