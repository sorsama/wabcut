#pragma once

// p02 audio: a fixed-size radix-2 FFT and a streaming short-time Fourier transform (STFT) for the spectral effects
// (FFT Filter, DeNoise, DeReverb, Adaptive Noise Reduction, Convolution Reverb, Pitch Shifter). Own code, double
// precision. Construction allocates; transforms and processing are RT-safe.

#include <complex>
#include <cstddef>
#include <vector>

namespace wabcut::audio::dsp {

/// An in-place iterative radix-2 complex FFT of one power-of-two size (2 .. 2^20), with a precomputed twiddle and
/// bit-reversal table, and the real-signal helpers the effects use.
class Fft {
public:
    using Complex = std::complex<double>;

    /// `size` is rounded up to a power of two (at least 2).
    explicit Fft(int size);

    int size() const noexcept { return m_size; }
    /// Bins of a real transform: size / 2 + 1 (DC .. Nyquist).
    int bins() const noexcept { return m_size / 2 + 1; }

    /// X[k] = sum_n x[n] e^(-2 pi i k n / N), in place on size() values.
    void forward(Complex* data) const noexcept;
    /// x[n] = (1 / N) sum_k X[k] e^(+2 pi i k n / N), in place: inverse(forward(x)) == x.
    void inverse(Complex* data) const noexcept;

    /// The bins() bins of the real signal `in` (size() samples). Uses an internal scratch: one call at a time.
    void forwardReal(const double* in, Complex* out) noexcept;
    /// The real signal (size() samples) of bins() bins (the rest are their conjugate mirror), scaled so that
    /// inverseReal(forwardReal(x)) == x.
    void inverseReal(const Complex* in, double* out) noexcept;

private:
    void transform(Complex* data, bool inverse) const noexcept;

    int m_size = 2;
    std::vector<Complex> m_twiddles; ///< e^(-2 pi i k / N), k < N / 2
    std::vector<int> m_reversed;     ///< bit-reversed index of each index
    std::vector<Complex> m_scratch;
};

/// Streaming STFT analysis-modification-synthesis per channel: frames of size() samples every hop() samples (size / 4:
/// 75 % overlap), square-root periodic Hann windows on analysis and synthesis (perfect reconstruction when nothing is
/// changed), overlap-add. The callback edits each frame's bins in place.
///
/// Latency: the output for input sample n is output sample n + latencyFrames() (= size). With an unchanged spectrum
/// the output is the input delayed by exactly that, to rounding.
class Stft {
public:
    /// `size` is rounded up to a power of two (64 .. 65536); `channels` 1..8.
    Stft(int channels, int size);

    int channels() const noexcept { return m_channels; }
    int size() const noexcept { return m_fft.size(); }
    int hop() const noexcept { return m_hop; }
    int bins() const noexcept { return m_fft.bins(); }
    int latencyFrames() const noexcept { return size(); }

    /// Processes `frames` interleaved frames in place. `onFrame(int channel, std::complex<double>* bins)` is called
    /// once per channel for every completed frame (every hop() input frames) with bins() bins.
    template <typename OnFrame>
    void process(float* io, int frames, OnFrame&& onFrame) noexcept {
        const auto ch = static_cast<std::size_t>(m_channels);
        for (int f = 0; f < frames; ++f) {
            float* frame = io + static_cast<std::size_t>(f) * ch;
            for (std::size_t c = 0; c < ch; ++c) {
                Channel& s = m_state[c];
                s.input[static_cast<std::size_t>(m_size - m_hop + m_fill)] = frame[c];
                frame[c] = static_cast<float>(s.output[static_cast<std::size_t>(m_fill)]);
            }
            if (++m_fill == m_hop) {
                m_fill = 0;
                for (std::size_t c = 0; c < ch; ++c) {
                    analyse(m_state[c]);
                    onFrame(static_cast<int>(c), m_bins.data());
                    synthesise(m_state[c]);
                }
            }
        }
    }

    /// Clears every buffer (the output restarts from silence).
    void reset() noexcept;

private:
    struct Channel {
        std::vector<double> input;  ///< the last size() input samples (the newest hop() still filling)
        std::vector<double> output; ///< overlap-add accumulator; the first hop() are due next
    };

    void analyse(Channel& s) noexcept;
    void synthesise(Channel& s) noexcept;

    int m_channels = 1;
    Fft m_fft;
    int m_size = 0;
    int m_hop = 0;
    int m_fill = 0;
    std::vector<double> m_window; ///< sqrt of the periodic Hann window
    double m_scale = 1.0;         ///< overlap-add gain correction
    std::vector<Channel> m_state;
    std::vector<double> m_frame;
    std::vector<std::complex<double>> m_bins;
};

} // namespace wabcut::audio::dsp
