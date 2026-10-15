#pragma once

// A small mixed-radix complex FFT (own implementation) for DeepFilterNet's 960-point frames (960 = 4 x 4 x 4 x 3 x 5;
// the beat tracker's radix-2 FFT cannot do them). Recursive decimation in time with one twiddle table; any size works
// (prime factors other than 2, 3 and 5 use a direct DFT butterfly). Double precision.

#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

namespace wabcut::ai::audio::detail {

class MixedFft {
public:
    using Complex = std::complex<double>;

    explicit MixedFft(std::size_t size) : m_size(size), m_twiddles(size), m_scratch(size) {
        for (std::size_t i = 0; i < size; ++i) {
            const double angle = -2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(size);
            m_twiddles[i] = {std::cos(angle), std::sin(angle)};
        }
        std::size_t n = size;
        for (const std::size_t p : {std::size_t{4}, std::size_t{2}, std::size_t{3}, std::size_t{5}}) {
            while (n % p == 0 && n > 1) {
                m_factors.push_back(p);
                n /= p;
            }
        }
        for (std::size_t p = 7; n > 1; p += 2) {
            while (n % p == 0) {
                m_factors.push_back(p);
                n /= p;
            }
        }
    }

    std::size_t size() const { return m_size; }

    /// out[k] = sum_n in[n] e^(-2 pi i k n / N) (unnormalised).
    void forward(std::span<const Complex> in, std::span<Complex> out) {
        transform(in.data(), out.data(), m_size, 1, 0);
    }

    /// out[n] = sum_k in[k] e^(+2 pi i k n / N) (unnormalised, like realfft's inverse).
    void inverse(std::span<const Complex> in, std::span<Complex> out) {
        m_conj.resize(m_size);
        for (std::size_t i = 0; i < m_size; ++i) {
            m_conj[i] = std::conj(in[i]);
        }
        transform(m_conj.data(), out.data(), m_size, 1, 0);
        for (Complex& c : out) {
            c = std::conj(c);
        }
    }

private:
    // DFT of the n samples in[0], in[stride], ... into out[0..n).
    void transform(const Complex* in, Complex* out, std::size_t n, std::size_t stride, std::size_t level) {
        if (n == 1) {
            out[0] = in[0];
            return;
        }
        const std::size_t p = m_factors[level];
        const std::size_t m = n / p;
        for (std::size_t r = 0; r < p; ++r) {
            transform(in + r * stride, out + r * m, m, stride * p, level + 1);
        }
        // Butterflies: X[k + q m] = sum_r W_n^(r (k + q m)) Y_r[k].
        const std::size_t step = m_size / n; // W_n^e = W_N^(e step)
        Complex* tmp = m_scratch.data();
        for (std::size_t k = 0; k < m; ++k) {
            for (std::size_t q = 0; q < p; ++q) {
                Complex sum = 0.0;
                const std::size_t kq = k + q * m;
                for (std::size_t r = 0; r < p; ++r) {
                    sum += out[r * m + k] * m_twiddles[(r * kq % n) * step];
                }
                tmp[kq] = sum;
            }
        }
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = tmp[i];
        }
    }

    std::size_t m_size;
    std::vector<Complex> m_twiddles;
    std::vector<Complex> m_scratch;
    std::vector<Complex> m_conj;
    std::vector<std::size_t> m_factors;
};

} // namespace wabcut::ai::audio::detail
