#pragma once

// Track-bus mixer: gain, pan, mute/solo, master gain, soft clip guard and meters.

#include <wabcut/audio/AudioSource.h>
#include <wabcut/audio/Gain.h>
#include <wabcut/audio/LevelMeter.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace wabcut::audio {

/// The master bus effects: processes the summed buses in place, before the master fader (Mixer::endBlock). RT-safe.
struct MasterInsert {
    virtual void process(std::span<float> block, int channels) noexcept = 0;

protected:
    ~MasterInsert() = default;
};

/// Sums N track buses into one interleaved output.
///
/// Per bus: fader gain (dB), pan, mute and solo. A bus is audible when it is not muted and either no bus is
/// soloed or it is soloed itself (mute always wins over solo). Mono buses are panned with the -3 dB
/// constant-power law (constantPowerPan); buses that already have the output's channel count use the
/// unity-centre balance (stereoBalance) on channels 0/1 and plain gain on any further channels. Gain changes,
/// including mute/solo, are ramped linearly across one block so automation and clicks on the mute button never
/// produce zipper noise; the very first block of a bus starts at its target with no ramp.
///
/// Master: the master insert (the sequence's master effect stack, if one is passed to endBlock), gain (dB), then the
/// master meter (so overs are visible and latch the clip indicator), then the soft clip guard (softClip, on by
/// default) protecting the device.
///
/// Meters: one per bus, post-fader/pan/mute, so a muted or non-soloed bus reads silence like in Premiere.
///
/// Threading. Parameter setters and getters and the meter accessors may be called from any thread at any time:
/// each parameter is an independent std::atomic accessed with relaxed ordering, which is enough because the
/// audio thread only needs to see each new value eventually (within a block) and nothing else is published
/// through them. beginBlock()/addBus()/endBlock() run on the audio thread only and are RT-safe: all memory is
/// allocated by the constructor.
class Mixer {
public:
    /// `busCount` >= 0; `outputChannels` and `sampleRate` are clamped to at least 1 (and outputChannels to
    /// LevelMeter::kMaxChannels). `maxBlockFrames` sizes the internal scratch; larger blocks are handled in
    /// slices, so it is a performance knob, not a limit.
    Mixer(int busCount, int outputChannels, int sampleRate, int maxBlockFrames = 1024,
          MeterBallistics ballistics = {});

    ~Mixer();

    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    int busCount() const noexcept { return m_busCount; }
    int outputChannels() const noexcept { return m_channels; }
    int sampleRate() const noexcept { return m_sampleRate; }

    // ---- Parameters (any thread; out-of-range bus indices are ignored / read as defaults) -----------------
    void setBusGainDb(int bus, float db) noexcept;
    float busGainDb(int bus) const noexcept;
    /// -1 = hard left, 0 = centre, +1 = hard right (clamped).
    void setBusPan(int bus, float pan) noexcept;
    float busPan(int bus) const noexcept;
    void setBusMuted(int bus, bool muted) noexcept;
    bool busMuted(int bus) const noexcept;
    void setBusSoloed(int bus, bool soloed) noexcept;
    bool busSoloed(int bus) const noexcept;
    void setMasterGainDb(float db) noexcept;
    float masterGainDb() const noexcept { return m_masterGainDb.load(std::memory_order_relaxed); }
    void setSoftClipEnabled(bool enabled) noexcept { m_softClip.store(enabled, std::memory_order_relaxed); }
    bool softClipEnabled() const noexcept { return m_softClip.load(std::memory_order_relaxed); }

    /// Meters (any thread). An out-of-range bus returns a meter that always reads silence.
    const LevelMeter& busMeter(int bus) const noexcept;
    const LevelMeter& masterMeter() const noexcept { return m_masterMeter; }

    // ---- Processing (audio thread only, RT-safe) ------------------------------------------------------------

    /// Start a block: zero `output` (outputChannels() interleaved; a trailing partial frame is left alone) and
    /// sample the solo state for the block. `output` must stay valid until endBlock().
    void beginBlock(std::span<float> output) noexcept;

    /// Mix one bus into the current block. `input` holds `inputChannels` interleaved channels and at least as
    /// many frames as the block (missing frames count as silence). Each bus may be added at most once per
    /// block; a bus that is not added this block meters silence and keeps its gain state.
    void addBus(int bus, std::span<const float> input, int inputChannels) noexcept;

    /// Finish the block: master insert, master gain, master meter, soft clip guard.
    void endBlock(MasterInsert* insert = nullptr) noexcept;

private:
    struct BusParams;
    struct BusState;

    void targetGains(int bus, int inputChannels, std::span<float> gains) const noexcept;

    int m_busCount = 0;
    int m_channels = 2;
    int m_sampleRate = 48000;
    int m_maxBlockFrames = 1024;

    std::unique_ptr<BusParams[]> m_params;
    std::unique_ptr<BusState[]> m_state;
    std::unique_ptr<LevelMeter[]> m_busMeters;
    LevelMeter m_masterMeter;
    LevelMeter m_silentMeter;
    std::vector<float> m_scratch; // one bus slice, post-fader

    std::atomic<float> m_masterGainDb{0.0f};
    std::atomic<bool> m_softClip{true};

    // Audio-thread block state.
    std::span<float> m_block;
    std::int64_t m_blockFrames = 0;
    bool m_anySolo = false;
    float m_masterCurrent = 1.0f;
    bool m_masterPrimed = false;
};

/// One mixer input: a source and the channel count it is rendered at (1 = mono, panned; or the output's
/// channel count, balanced).
struct MixerInput {
    std::shared_ptr<AudioSource> source;
    int channels = 2;
};

/// AudioSource that renders a fixed set of inputs through a Mixer. The input list is immutable after
/// construction (build a new MixerSource and swap it in with AudioEngine::setSource to change the graph);
/// mixer parameters stay live through mixer().
class MixerSource final : public AudioSource {
public:
    /// Allocates all scratch memory. Null sources in `inputs` render as silence.
    MixerSource(std::vector<MixerInput> inputs, int outputChannels, int sampleRate, int maxBlockFrames = 1024,
                MeterBallistics ballistics = {});

    Mixer& mixer() noexcept { return m_mixer; }
    const Mixer& mixer() const noexcept { return m_mixer; }

    /// RT-safe. Renders silence if `channels` / `sampleRate` differ from the construction values.
    void render(std::span<float> interleaved, int channels, Rate sampleRate, Ticks startTime) noexcept override;

private:
    std::vector<MixerInput> m_inputs;
    Mixer m_mixer;
    int m_maxBlockFrames;
    std::vector<float> m_inputScratch;
};

} // namespace wabcut::audio
