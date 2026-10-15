#pragma once

// Internal: an in-place radix-2 complex FFT in single precision for audio sync's cross-correlations (A.5). The coarse
// pass correlates hour-long 1 kHz envelopes (2^23 points), so it is iterative (no recursion), keeps one twiddle table
// per size and reverses bits incrementally (no index table).

#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <utility>
#include <vector>

namespace wabcut::audio::edit::detail {

using Cf = std::complex<float>;

inline std::size_t nextPowerOfTwo(std::size_t n) {
    std::size_t p = 1;
    while (p < n) {
        p <<= 1;
    }
    return p;
}

class SyncFft {
public:
    /// `size` must be a power of two.
    explicit SyncFft(std::size_t size) : m_size(size), m_twiddles(size / 2) {
        for (std::size_t k = 0; k < size / 2; ++k) {
            const double a = -2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(size);
            m_twiddles[k] = Cf(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
        }
    }

    std::size_t size() const { return m_size; }

    /// X[k] = sum_n x[n] e^(-2 pi i k n / N), in place.
    void forward(Cf* data) const { transform(data, false); }
    /// x[n] = sum_k X[k] e^(+2 pi i k n / N) (unnormalised), in place.
    void inverse(Cf* data) const { transform(data, true); }

private:
    void transform(Cf* data, bool inverse) const {
        const std::size_t n = m_size;
        for (std::size_t i = 1, j = 0; i < n; ++i) {
            std::size_t bit = n >> 1;
            for (; (j & bit) != 0; bit >>= 1) {
                j ^= bit;
            }
            j ^= bit;
            if (i < j) {
                std::swap(data[i], data[j]);
            }
        }
        for (std::size_t length = 2; length <= n; length <<= 1) {
            const std::size_t half = length / 2;
            const std::size_t step = n / length;
            for (std::size_t start = 0; start < n; start += length) {
                Cf* a = data + start;
                Cf* b = a + half;
                for (std::size_t k = 0; k < half; ++k) {
                    Cf w = m_twiddles[k * step];
                    if (inverse) {
                        w = std::conj(w);
                    }
                    const Cf t = w * b[k];
                    b[k] = a[k] - t;
                    a[k] += t;
                }
            }
        }
    }

    std::size_t m_size;
    std::vector<Cf> m_twiddles;
};

} // namespace wabcut::audio::edit::detail
