#pragma once

// A small in-place radix-2 FFT (own implementation) for the beat tracker's STFT.

#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <vector>

namespace wabcut::ai::analysis::detail {

class Fft {
public:
    /// `size` must be a power of two.
    explicit Fft(std::size_t size) : m_size(size), m_twiddles(size / 2), m_reversed(size) {
        for (std::size_t i = 0; i < size / 2; ++i) {
            const double angle = -2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(size);
            m_twiddles[i] = {std::cos(angle), std::sin(angle)};
        }
        std::size_t bits = 0;
        while ((std::size_t{1} << bits) < size) {
            ++bits;
        }
        for (std::size_t i = 0; i < size; ++i) {
            std::size_t r = 0;
            for (std::size_t b = 0; b < bits; ++b) {
                r |= ((i >> b) & 1u) << (bits - 1 - b);
            }
            m_reversed[i] = r;
        }
    }

    std::size_t size() const { return m_size; }

    void transform(std::vector<std::complex<double>>& data) const {
        for (std::size_t i = 0; i < m_size; ++i) {
            if (i < m_reversed[i]) {
                std::swap(data[i], data[m_reversed[i]]);
            }
        }
        for (std::size_t length = 2; length <= m_size; length <<= 1) {
            const std::size_t half = length / 2;
            const std::size_t step = m_size / length;
            for (std::size_t start = 0; start < m_size; start += length) {
                for (std::size_t k = 0; k < half; ++k) {
                    const std::complex<double> t = m_twiddles[k * step] * data[start + k + half];
                    data[start + k + half] = data[start + k] - t;
                    data[start + k] += t;
                }
            }
        }
    }

private:
    std::size_t m_size;
    std::vector<std::complex<double>> m_twiddles;
    std::vector<std::size_t> m_reversed;
};

} // namespace wabcut::ai::analysis::detail
