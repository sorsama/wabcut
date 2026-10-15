#pragma once

// DeepFilterNet3's signal processing, for Enhance Speech (A.4; docs/ai.md "Enhance Speech"). The networks (three
// ONNX graphs: encoder, ERB decoder, DF decoder) run in the wabcut-embed helper; everything around them is here, so
// the editor's tests cover it without a model: the STFT (960-point Vorbis window, 480 hop at 48 kHz), the 32-band
// ERB features and the 96-bin complex features with libDF's exponential normalisations, the ERB gains, the deep
// filter (order 5, two frames of lookahead) and the overlap-add synthesis.
//
// Ported from libDF (github.com/Rikorose/DeepFilterNet, MIT/Apache-2.0; lib.rs and tract.rs at commit 84d57ec,
// the commit of the DeepFilterNet3 ONNX export) for offline processing: the networks see a whole block of frames at
// once instead of one per call, which is the same computation (they are causal), and the spectra are taken without
// libDF's streaming delay: output sample n is input sample n (the STFT's 480 samples and the two lookahead frames are
// compensated, like deep-filter's --compensate-delay). Both stages always run, as deep-filter's default thresholds
// (-15 / 35 / 35 dB local SNR, the bounds of the estimate) make them; no post-filter; full attenuation (the editor
// mixes the original back in, which is exactly libDF's attenuation limit: a linear blend of the two spectra, so of
// the two signals).

#include <wabcut/model/Error.h>

#include <complex>
#include <cstddef>
#include <expected>
#include <functional>
#include <span>
#include <vector>

namespace wabcut::ai::audio::dfn {

inline constexpr int kSampleRate = 48000;
inline constexpr int kFftSize = 960;
inline constexpr int kHop = 480;
inline constexpr int kBins = kFftSize / 2 + 1; // 481
inline constexpr int kErbBands = 32;
inline constexpr int kMinErbBins = 2;
inline constexpr int kDfBins = 96;
inline constexpr int kDfOrder = 5;
inline constexpr int kDfLookahead = 2;
inline constexpr int kConvLookahead = 2;
/// Frames the networks look ahead: their output for frame t applies to frame t - kLookahead.
inline constexpr int kLookahead = kConvLookahead > kDfLookahead ? kConvLookahead : kDfLookahead;
/// The normalisations' smoothing, libDF's calc_norm_alpha(48000, 480, tau = 1 s).
inline constexpr float kNormAlpha = 0.99f;

using Complex = std::complex<float>;

/// Bins per ERB band (libDF erb_fb): 32 bands over the 481 bins, at least 2 bins each.
std::vector<int> erbWidths();

/// The analysis window (Vorbis: sin(pi/2 sin^2(pi (n + 0.5) / 960))).
const std::vector<float>& window();

/// Short-time spectra: frame t is the window over samples [(t - 1) hop, (t + 1) hop) (zero outside the signal),
/// scaled by libDF's 1 / (fft_size^2 / (2 hop)).
struct Spectrogram {
    int frames = 0;
    std::vector<Complex> bins; ///< frames x kBins

    Complex* frame(int t) { return bins.data() + static_cast<std::size_t>(t) * kBins; }
    const Complex* frame(int t) const { return bins.data() + static_cast<std::size_t>(t) * kBins; }
};

/// Output frames needed to synthesise `samples` samples (each sample is in two frames).
int framesFor(std::size_t samples);

/// The first `frames` frames of `samples`.
Spectrogram analyse(std::span<const float> samples, int frames);
/// Overlap-adds the frames into `samples` samples (the inverse of analyse(): synthesise(analyse(x)) == x).
std::vector<float> synthesise(const Spectrogram& spectra, std::size_t samples);

/// The encoder's inputs for every frame of `spectra`: "feat_erb" [1, 1, T, 32] and "feat_spec" [1, 2, T, 96] (the
/// real parts of all frames, then the imaginary parts), with libDF's normalisation states from their initial values.
struct Features {
    int frames = 0;
    std::vector<float> erb;  ///< T x 32
    std::vector<float> spec; ///< 2 x T x 96
};
Features features(const Spectrogram& spectra);

/// What the networks return for T frames; frame t's outputs apply to spectrum t - kLookahead.
struct NetworkOutput {
    int frames = 0;
    std::vector<float> gains; ///< T x 32: the ERB decoder's "m" (sigmoid gains)
    std::vector<float> coefs; ///< T x 96 x 5 x 2: the DF decoder's "coefs" (real, imaginary per order)
    std::vector<float> lsnr;  ///< T: the encoder's local SNR estimate, dB (reported, not used to skip stages)
};

/// The enhanced spectra of frames [0, frames): every bin times its band's gain, then the lowest 96 bins replaced by
/// the deep filter over the noisy frames t - 2 .. t + 2. `net` needs frames + kLookahead frames, `noisy` as many.
Spectrogram enhance(const Spectrogram& noisy, const NetworkOutput& net, int frames);

/// Runs the three networks on the features. Errors are the helper's.
using Network = std::function<std::expected<NetworkOutput, Error>(const Features&)>;

/// One channel through the whole pipeline: the enhanced signal, as long as `samples` and aligned with it.
/// Errors: the network's, InvalidArgument (it returned the wrong number of frames).
std::expected<std::vector<float>, Error> enhanceChannel(std::span<const float> samples, const Network& network);

/// A network that changes nothing (gains 1, the deep filter passing frame t through): enhanceChannel() then returns
/// its input. Tests, and the identity check of the alignment.
NetworkOutput identityOutput(int frames);

} // namespace wabcut::ai::audio::dfn
