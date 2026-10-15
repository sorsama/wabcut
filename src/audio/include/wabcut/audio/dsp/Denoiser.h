#pragma once

// Reduce Noise (wabcut.audio.denoise): RNNoise (Valin, "A Hybrid DSP/Deep Learning Approach to Real-Time Full-Band
// Speech Enhancement", 2018) run per channel on 10 ms frames, with a dry/wet amount. 48 kHz only (the network's rate);
// at other rates it passes audio through untouched with no latency.

#include <memory>

namespace wabcut::audio::dsp {

class Denoiser {
public:
    /// RNNoise's frame: 10 ms at 48 kHz.
    static constexpr int kFrameFrames = 480;
    /// Input to output delay when active: one frame of buffering here plus RNNoise's two frames (its synthesis
    /// overlap-adds the window it analysed one call earlier).
    static constexpr int kLatencyFrames = 3 * kFrameFrames;

    /// Allocates one RNNoise state per channel (channels clamped to [1, kMaxDspChannels]).
    Denoiser(int channels, double sampleRate);
    ~Denoiser();
    Denoiser(const Denoiser&) = delete;
    Denoiser& operator=(const Denoiser&) = delete;

    /// True at 48 kHz.
    bool active() const noexcept { return m_active; }
    int latencyFrames() const noexcept { return m_active ? kLatencyFrames : 0; }
    /// The recurrent state adapts within about half a second.
    int settleFrames() const noexcept { return m_active ? 24000 + kLatencyFrames : 0; }

    /// 0 = the original (delayed to match), 1 = fully denoised.
    void setAmount(double amount) noexcept;
    /// RT-safe.
    void process(float* interleaved, int frames) noexcept;
    /// RT-safe (re-initialises the states in place).
    void reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_active = false;
};

} // namespace wabcut::audio::dsp
