#include <wabcut/audio/dsp/Denoiser.h>
#include <wabcut/audio/dsp/Equalizer.h> // kMaxDspChannels

#include <rnnoise.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <vector>

namespace wabcut::audio::dsp {

namespace {
// RNNoise works on 16-bit scaled floats.
constexpr float kScale = 32768.0f;
} // namespace

struct Denoiser::Impl {
    struct Channel {
        DenoiseState* state = nullptr;
        std::array<float, kFrameFrames> in{};
        std::array<float, kFrameFrames> out{};
        std::array<float, kLatencyFrames> dry{};
    };

    int channels = 1;
    std::vector<Channel> perChannel;
    int pos = 0;    ///< frame position within the current 10 ms block
    int dryPos = 0;
    float wet = 1.0f;

    ~Impl() {
        for (Channel& c : perChannel) {
            rnnoise_destroy(c.state);
        }
    }

    void clear() noexcept {
        for (Channel& c : perChannel) {
            rnnoise_init(c.state, nullptr); // memset + binding the built-in model: no allocation
            c.in.fill(0.0f);
            c.out.fill(0.0f);
            c.dry.fill(0.0f);
        }
        pos = 0;
        dryPos = 0;
    }
};

Denoiser::Denoiser(int channels, double sampleRate) : m_impl(std::make_unique<Impl>()) {
    m_impl->channels = std::clamp(channels, 1, kMaxDspChannels);
    m_active = sampleRate == 48000.0;
    if (!m_active) {
        return;
    }
    m_impl->perChannel.resize(static_cast<std::size_t>(m_impl->channels));
    for (Impl::Channel& c : m_impl->perChannel) {
        c.state = rnnoise_create(nullptr);
        if (c.state == nullptr) {
            m_active = false; // the built-in model failed to bind: pass audio through
        }
    }
    if (m_active) {
        m_impl->clear();
    }
}

Denoiser::~Denoiser() = default;

void Denoiser::setAmount(double amount) noexcept { m_impl->wet = static_cast<float>(std::clamp(amount, 0.0, 1.0)); }

void Denoiser::reset() noexcept {
    if (m_active) {
        m_impl->clear();
    }
}

void Denoiser::process(float* interleaved, int frames) noexcept {
    if (!m_active) {
        return;
    }
    Impl& d = *m_impl;
    const auto ch = static_cast<std::size_t>(d.channels);
    const float wet = d.wet;
    const float dryMix = 1.0f - wet;
    for (int i = 0; i < frames; ++i) {
        float* frame = interleaved + static_cast<std::size_t>(i) * ch;
        const auto p = static_cast<std::size_t>(d.pos);
        const auto q = static_cast<std::size_t>(d.dryPos);
        for (std::size_t c = 0; c < ch; ++c) {
            Impl::Channel& s = d.perChannel[c];
            const float x = frame[c];
            s.in[p] = x * kScale;
            const float dry = s.dry[q];
            s.dry[q] = x;
            frame[c] = wet * (s.out[p] / kScale) + dryMix * dry;
        }
        d.dryPos = (d.dryPos + 1) % kLatencyFrames;
        if (++d.pos == kFrameFrames) {
            d.pos = 0;
            for (Impl::Channel& s : d.perChannel) {
                rnnoise_process_frame(s.state, s.out.data(), s.in.data());
            }
        }
    }
}

} // namespace wabcut::audio::dsp
