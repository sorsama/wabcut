#include <wabcut/audio/Mixer.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace wabcut::audio {

struct Mixer::BusParams {
    std::atomic<float> gainDb{0.0f};
    std::atomic<float> pan{0.0f};
    std::atomic<bool> muted{false};
    std::atomic<bool> soloed{false};
};

struct Mixer::BusState {
    std::array<float, LevelMeter::kMaxChannels> current{}; ///< Gain per output channel reached by the last block.
    bool primed = false;                                   ///< False until the first block: start at the target.
    bool addedThisBlock = false;
};

Mixer::Mixer(int busCount, int outputChannels, int sampleRate, int maxBlockFrames, MeterBallistics ballistics)
    : m_busCount(std::max(busCount, 0)), m_channels(std::clamp(outputChannels, 1, LevelMeter::kMaxChannels)),
      m_sampleRate(std::max(sampleRate, 1)), m_maxBlockFrames(std::max(maxBlockFrames, 16)),
      m_params(std::make_unique<BusParams[]>(static_cast<std::size_t>(m_busCount))),
      m_state(std::make_unique<BusState[]>(static_cast<std::size_t>(m_busCount))),
      m_busMeters(std::make_unique<LevelMeter[]>(static_cast<std::size_t>(m_busCount))),
      m_masterMeter(m_channels, m_sampleRate, ballistics), m_silentMeter(m_channels, m_sampleRate, ballistics),
      m_scratch(static_cast<std::size_t>(m_maxBlockFrames) * static_cast<std::size_t>(m_channels)) {
    for (int i = 0; i < m_busCount; ++i) {
        m_busMeters[i].configure(m_channels, m_sampleRate, ballistics);
    }
}

Mixer::~Mixer() = default;

// ---- Parameters ----------------------------------------------------------------------------------------

void Mixer::setBusGainDb(int bus, float db) noexcept {
    if (bus >= 0 && bus < m_busCount) {
        m_params[bus].gainDb.store(db, std::memory_order_relaxed);
    }
}

float Mixer::busGainDb(int bus) const noexcept {
    return (bus >= 0 && bus < m_busCount) ? m_params[bus].gainDb.load(std::memory_order_relaxed) : 0.0f;
}

void Mixer::setBusPan(int bus, float pan) noexcept {
    if (bus >= 0 && bus < m_busCount) {
        m_params[bus].pan.store(std::isnan(pan) ? 0.0f : std::clamp(pan, -1.0f, 1.0f), std::memory_order_relaxed);
    }
}

float Mixer::busPan(int bus) const noexcept {
    return (bus >= 0 && bus < m_busCount) ? m_params[bus].pan.load(std::memory_order_relaxed) : 0.0f;
}

void Mixer::setBusMuted(int bus, bool muted) noexcept {
    if (bus >= 0 && bus < m_busCount) {
        m_params[bus].muted.store(muted, std::memory_order_relaxed);
    }
}

bool Mixer::busMuted(int bus) const noexcept {
    return bus >= 0 && bus < m_busCount && m_params[bus].muted.load(std::memory_order_relaxed);
}

void Mixer::setBusSoloed(int bus, bool soloed) noexcept {
    if (bus >= 0 && bus < m_busCount) {
        m_params[bus].soloed.store(soloed, std::memory_order_relaxed);
    }
}

bool Mixer::busSoloed(int bus) const noexcept {
    return bus >= 0 && bus < m_busCount && m_params[bus].soloed.load(std::memory_order_relaxed);
}

void Mixer::setMasterGainDb(float db) noexcept { m_masterGainDb.store(db, std::memory_order_relaxed); }

const LevelMeter& Mixer::busMeter(int bus) const noexcept {
    return (bus >= 0 && bus < m_busCount) ? m_busMeters[bus] : m_silentMeter;
}

// ---- Processing ----------------------------------------------------------------------------------------

void Mixer::targetGains(int bus, int inputChannels, std::span<float> gains) const noexcept {
    std::fill(gains.begin(), gains.end(), 0.0f);
    const BusParams& p = m_params[bus];
    const bool audible = !p.muted.load(std::memory_order_relaxed) &&
                         (!m_anySolo || p.soloed.load(std::memory_order_relaxed));
    const float level = audible ? dbToLinear(p.gainDb.load(std::memory_order_relaxed)) : 0.0f;
    if (m_channels == 1) {
        gains[0] = level; // mono output: pan does not apply
        return;
    }
    const float pan = p.pan.load(std::memory_order_relaxed);
    if (inputChannels == 1) {
        const StereoGains g = constantPowerPan(pan);
        gains[0] = level * g.left;
        gains[1] = level * g.right;
    } else {
        const StereoGains g = stereoBalance(pan);
        gains[0] = level * g.left;
        gains[1] = level * g.right;
        for (int c = 2; c < m_channels; ++c) {
            gains[c] = level;
        }
    }
}

void Mixer::beginBlock(std::span<float> output) noexcept {
    const auto channels = static_cast<std::size_t>(m_channels);
    m_blockFrames = static_cast<std::int64_t>(output.size() / channels);
    m_block = output.first(static_cast<std::size_t>(m_blockFrames) * channels);
    std::fill(m_block.begin(), m_block.end(), 0.0f);
    m_anySolo = false;
    for (int i = 0; i < m_busCount; ++i) {
        m_anySolo = m_anySolo || m_params[i].soloed.load(std::memory_order_relaxed);
        m_state[i].addedThisBlock = false;
    }
}

bool Mixer::busAudible(int bus) const noexcept {
    if (bus < 0 || bus >= m_busCount) {
        return false;
    }
    const BusParams& p = m_params[bus];
    return !p.muted.load(std::memory_order_relaxed) && (!m_anySolo || p.soloed.load(std::memory_order_relaxed));
}

void Mixer::addBus(int bus, std::span<const float> input, int inputChannels) noexcept {
    addBusTo(bus, input, inputChannels, m_block);
}

void Mixer::addBusTo(int bus, std::span<const float> input, int inputChannels, std::span<float> destination) noexcept {
    if (bus < 0 || bus >= m_busCount || m_blockFrames == 0 ||
        destination.size() < static_cast<std::size_t>(m_blockFrames) * static_cast<std::size_t>(m_channels)) {
        return;
    }
    BusState& state = m_state[bus];
    if (state.addedThisBlock) {
        return;
    }
    state.addedThisBlock = true;

    const int inCh = std::clamp(inputChannels, 1, LevelMeter::kMaxChannels);
    const auto outCh = static_cast<std::size_t>(m_channels);
    std::array<float, LevelMeter::kMaxChannels> target{};
    targetGains(bus, inCh, target);
    if (!state.primed) {
        state.current = target;
        state.primed = true;
    }
    const bool ramp = state.current != target;

    const std::int64_t frames = m_blockFrames;
    const std::int64_t inputFrames = std::min<std::int64_t>(frames, static_cast<std::int64_t>(input.size()) / inCh);
    const double invFrames = 1.0 / static_cast<double>(frames);
    const auto inChannels = static_cast<std::size_t>(inCh);
    float* const scratch = m_scratch.data();

    for (std::int64_t sliceStart = 0; sliceStart < frames; sliceStart += m_maxBlockFrames) {
        const std::int64_t n = std::min<std::int64_t>(m_maxBlockFrames, frames - sliceStart);
        for (std::int64_t i = 0; i < n; ++i) {
            const std::int64_t frame = sliceStart + i;
            // Linear ramp that lands exactly on the target at the block's last frame.
            const auto position = static_cast<float>(static_cast<double>(frame + 1) * invFrames);
            const bool haveInput = frame < inputFrames;
            const auto inBase = static_cast<std::size_t>(frame) * inChannels;
            for (std::size_t c = 0; c < outCh; ++c) {
                const float gain = ramp ? state.current[c] + (target[c] - state.current[c]) * position : target[c];
                float x = 0.0f;
                if (haveInput) {
                    if (inCh == 1) {
                        x = input[inBase];
                    } else if (c < inChannels) {
                        x = input[inBase + c];
                    }
                }
                scratch[static_cast<std::size_t>(i) * outCh + c] = x * gain;
            }
        }
        const std::size_t samples = static_cast<std::size_t>(n) * outCh;
        m_busMeters[bus].process(std::span<const float>(scratch, samples));
        float* const out = destination.data() + static_cast<std::size_t>(sliceStart) * outCh;
        for (std::size_t s = 0; s < samples; ++s) {
            out[s] += scratch[s];
        }
    }
    state.current = target;
}

void Mixer::endBlock(MasterInsert* insert) noexcept {
    if (m_blockFrames == 0) {
        return;
    }
    for (int i = 0; i < m_busCount; ++i) {
        if (!m_state[i].addedThisBlock) {
            m_busMeters[i].processSilence(m_blockFrames);
        }
    }
    if (insert != nullptr) {
        insert->process(m_block, m_channels);
    }

    const float target = dbToLinear(m_masterGainDb.load(std::memory_order_relaxed));
    if (!m_masterPrimed) {
        m_masterCurrent = target;
        m_masterPrimed = true;
    }
    const auto outCh = static_cast<std::size_t>(m_channels);
    if (m_masterCurrent != target) {
        const double invFrames = 1.0 / static_cast<double>(m_blockFrames);
        for (std::int64_t f = 0; f < m_blockFrames; ++f) {
            const auto position = static_cast<float>(static_cast<double>(f + 1) * invFrames);
            const float gain = m_masterCurrent + (target - m_masterCurrent) * position;
            for (std::size_t c = 0; c < outCh; ++c) {
                m_block[static_cast<std::size_t>(f) * outCh + c] *= gain;
            }
        }
    } else if (target != 1.0f) {
        for (float& x : m_block) {
            x *= target;
        }
    }
    m_masterCurrent = target;

    m_masterMeter.process(m_block); // pre-guard, so overs show and latch the clip indicator
    if (m_softClip.load(std::memory_order_relaxed)) {
        for (float& x : m_block) {
            x = softClip(x);
        }
    }
    m_block = {};
    m_blockFrames = 0;
}

// ---- MixerSource ---------------------------------------------------------------------------------------

namespace {
int maxInputChannels(const std::vector<MixerInput>& inputs) {
    int channels = 1;
    for (const MixerInput& in : inputs) {
        channels = std::max(channels, std::clamp(in.channels, 1, LevelMeter::kMaxChannels));
    }
    return channels;
}
} // namespace

MixerSource::MixerSource(std::vector<MixerInput> inputs, int outputChannels, int sampleRate, int maxBlockFrames,
                         MeterBallistics ballistics)
    : m_inputs(std::move(inputs)),
      m_mixer(static_cast<int>(m_inputs.size()), outputChannels, sampleRate, maxBlockFrames, ballistics),
      m_maxBlockFrames(std::max(maxBlockFrames, 16)),
      m_inputScratch(static_cast<std::size_t>(m_maxBlockFrames) * static_cast<std::size_t>(maxInputChannels(m_inputs))) {}

void MixerSource::render(std::span<float> interleaved, int channels, Rate sampleRate, Ticks startTime) noexcept {
    const bool formatOk = channels == m_mixer.outputChannels() && sampleRate.isValid() &&
                          static_cast<std::int64_t>(sampleRate.num) ==
                              static_cast<std::int64_t>(m_mixer.sampleRate()) * sampleRate.den;
    if (!formatOk) {
        std::fill(interleaved.begin(), interleaved.end(), 0.0f);
        return;
    }
    const auto outCh = static_cast<std::size_t>(channels);
    const std::size_t frames = interleaved.size() / outCh;
    const std::int64_t ticksPerFrame = sampleRate.ticksPerUnit();

    for (std::size_t done = 0; done < frames;) {
        const std::size_t n = std::min(static_cast<std::size_t>(m_maxBlockFrames), frames - done);
        const Ticks blockStart = startTime + Ticks{static_cast<std::int64_t>(done) * ticksPerFrame};
        m_mixer.beginBlock(interleaved.subspan(done * outCh, n * outCh));
        for (std::size_t i = 0; i < m_inputs.size(); ++i) {
            const MixerInput& in = m_inputs[i];
            const int inCh = std::clamp(in.channels, 1, LevelMeter::kMaxChannels);
            const std::span<float> scratch(m_inputScratch.data(), n * static_cast<std::size_t>(inCh));
            if (in.source) {
                in.source->render(scratch, inCh, sampleRate, blockStart);
            } else {
                std::fill(scratch.begin(), scratch.end(), 0.0f);
            }
            m_mixer.addBus(static_cast<int>(i), scratch, inCh);
        }
        m_mixer.endBlock();
        done += n;
    }
    // A trailing partial frame (malformed span) is silenced rather than left uninitialised.
    std::fill(interleaved.begin() + static_cast<std::ptrdiff_t>(frames * outCh), interleaved.end(), 0.0f);
}

} // namespace wabcut::audio
