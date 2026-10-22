#include <wabcut/audio/dsp/Fft.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace wabcut::audio::dsp {

namespace {

int powerOfTwoAtLeast(int n, int lo, int hi) {
    int p = lo;
    while (p < n && p < hi) {
        p <<= 1;
    }
    return p;
}

} // namespace

Fft::Fft(int size) : m_size(powerOfTwoAtLeast(size, 2, 1 << 20)) {
    const auto n = static_cast<std::size_t>(m_size);
    m_twiddles.resize(n / 2);
    for (std::size_t k = 0; k < n / 2; ++k) {
        const double a = -2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(n);
        m_twiddles[k] = Complex(std::cos(a), std::sin(a));
    }
    m_reversed.resize(n);
    int bits = 0;
    while ((1 << bits) < m_size) {
        ++bits;
    }
    for (int i = 0; i < m_size; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b) {
            r |= ((i >> b) & 1) << (bits - 1 - b);
        }
        m_reversed[static_cast<std::size_t>(i)] = r;
    }
    m_scratch.resize(n);
}

void Fft::transform(Complex* data, bool inverse) const noexcept {
    const int n = m_size;
    for (int i = 0; i < n; ++i) {
        const int r = m_reversed[static_cast<std::size_t>(i)];
        if (r > i) {
            std::swap(data[i], data[r]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len / 2;
        const int step = n / len;
        for (int start = 0; start < n; start += len) {
            for (int k = 0; k < half; ++k) {
                Complex w = m_twiddles[static_cast<std::size_t>(k * step)];
                if (inverse) {
                    w = std::conj(w);
                }
                const Complex a = data[start + k];
                const Complex b = data[start + k + half] * w;
                data[start + k] = a + b;
                data[start + k + half] = a - b;
            }
        }
    }
    if (inverse) {
        const double scale = 1.0 / static_cast<double>(n);
        for (int i = 0; i < n; ++i) {
            data[i] *= scale;
        }
    }
}

void Fft::forward(Complex* data) const noexcept { transform(data, false); }

void Fft::inverse(Complex* data) const noexcept { transform(data, true); }

void Fft::forwardReal(const double* in, Complex* out) noexcept {
    for (int i = 0; i < m_size; ++i) {
        m_scratch[static_cast<std::size_t>(i)] = Complex(in[i], 0.0);
    }
    transform(m_scratch.data(), false);
    std::copy_n(m_scratch.begin(), bins(), out);
}

void Fft::inverseReal(const Complex* in, double* out) noexcept {
    const int half = m_size / 2;
    for (int k = 0; k <= half; ++k) {
        m_scratch[static_cast<std::size_t>(k)] = in[k];
    }
    // A real signal's spectrum is conjugate-symmetric; DC and Nyquist are real.
    m_scratch[0] = Complex(in[0].real(), 0.0);
    m_scratch[static_cast<std::size_t>(half)] = Complex(in[half].real(), 0.0);
    for (int k = 1; k < half; ++k) {
        m_scratch[static_cast<std::size_t>(m_size - k)] = std::conj(in[k]);
    }
    transform(m_scratch.data(), true);
    for (int i = 0; i < m_size; ++i) {
        out[i] = m_scratch[static_cast<std::size_t>(i)].real();
    }
}

// ---- Stft ----------------------------------------------------------------------------------------------------------

Stft::Stft(int channels, int size)
    : m_channels(std::clamp(channels, 1, 8)), m_fft(powerOfTwoAtLeast(size, 64, 65536)), m_size(m_fft.size()),
      m_hop(m_fft.size() / 4) {
    const auto n = static_cast<std::size_t>(m_size);
    m_window.resize(n);
    double overlap = 0.0; // sum of the analysis x synthesis windows (the periodic Hann) at one sample over the overlaps
    for (std::size_t i = 0; i < n; ++i) {
        const double hann = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n));
        m_window[i] = std::sqrt(hann);
    }
    for (std::size_t i = 0; i < n; i += static_cast<std::size_t>(m_hop)) {
        overlap += m_window[i] * m_window[i];
    }
    m_scale = overlap > 0.0 ? 1.0 / overlap : 1.0;
    m_state.resize(static_cast<std::size_t>(m_channels));
    for (Channel& s : m_state) {
        s.input.assign(n, 0.0);
        s.output.assign(n, 0.0);
    }
    m_frame.assign(n, 0.0);
    m_bins.assign(static_cast<std::size_t>(m_fft.bins()), {});
}

void Stft::reset() noexcept {
    for (Channel& s : m_state) {
        std::fill(s.input.begin(), s.input.end(), 0.0);
        std::fill(s.output.begin(), s.output.end(), 0.0);
    }
    m_fill = 0;
}

void Stft::analyse(Channel& s) noexcept {
    const auto n = static_cast<std::size_t>(m_size);
    for (std::size_t i = 0; i < n; ++i) {
        m_frame[i] = s.input[i] * m_window[i];
    }
    m_fft.forwardReal(m_frame.data(), m_bins.data());
    // The input moves on by one hop: the oldest hop() samples leave.
    std::copy(s.input.begin() + m_hop, s.input.end(), s.input.begin());
    std::fill(s.input.end() - m_hop, s.input.end(), 0.0);
}

void Stft::synthesise(Channel& s) noexcept {
    m_fft.inverseReal(m_bins.data(), m_frame.data());
    std::copy(s.output.begin() + m_hop, s.output.end(), s.output.begin());
    std::fill(s.output.end() - m_hop, s.output.end(), 0.0);
    const auto n = static_cast<std::size_t>(m_size);
    for (std::size_t i = 0; i < n; ++i) {
        s.output[i] += m_frame[i] * m_window[i] * m_scale;
    }
}

} // namespace wabcut::audio::dsp
