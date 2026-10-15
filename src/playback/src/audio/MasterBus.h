#pragma once

// Internal: the sequence's master audio effect stack (Sequence::audioEffects) and then its master fader
// (Sequence::masterVolume, A.3) on the mix, for the player's audio callback and for OfflineMixdown.
//
// The control side builds a "program" (a copy of the stack and the fader curve, and an EffectChain bound to the
// stack) whenever either changes and publishes it with one atomic pointer store. The new chain shares the processors of instances that
// did not change, so a limiter keeps its state while the user drags another effect's parameter. The audio thread
// loads the pointer once per render; a retired program is freed on the control side once the audio thread is past
// every render that may still have used it (a render sequence counter, odd while rendering). Nothing on the audio
// thread allocates, frees or locks.
//
// Latency: the master chain delays the mix by latencyFrames(); the caller reads the tracks that much ahead, so the
// output is on time (see TimelineMixSource / OfflineMixdown).

#include <wabcut/audio/Mixer.h>
#include <wabcut/audio/effects/EffectChain.h>

#include <wabcut/model/Effects.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace wabcut::playback::detail {

class LiveFader;

class MasterBus final : public audio::MasterInsert {
public:
    MasterBus(int channels, int sampleRate);
    ~MasterBus();
    MasterBus(const MasterBus&) = delete;
    MasterBus& operator=(const MasterBus&) = delete;

    // ---- Control side (one non-real-time thread) ---------------------------------------------------------------
    /// Publishes `stack` and the master fader `volume` (dB; a constant or a curve in timeline time) if they differ
    /// from the current program's. Allocates; frees retired programs.
    void update(const EffectStack& stack, const ParamValue& volume = {});
    /// Frees retired programs the audio thread is done with.
    void collect();
    /// The live player's master fader override (automation passes; it outlives the bus). Export has none. Set before
    /// the first render.
    void setLive(LiveFader* live) noexcept { m_live = live; }

    // ---- Audio thread (RT-safe) ------------------------------------------------------------------------------
    /// Starts a render: picks up the current program. Every beginRender() needs an endRender().
    void beginRender() noexcept;
    void endRender() noexcept;
    /// The current render's program: its latency and whether it runs anything (effects or a fader away from 0 dB).
    int latencyFrames() const noexcept;
    bool empty() const noexcept;
    /// Restarts the chain at a discontinuity: the parameter grid counts from `origin`, effect state is cleared.
    void restart(Ticks origin, std::int64_t ticksPerFrame) noexcept;
    /// The stream goes on at timeline time `at` (a loop wrap): effect state and the parameter grid's frame count go
    /// on; from the next frame the chain's and the fader's parameters are taken at `at` onwards.
    void continueAt(Ticks at) noexcept { m_clock.origin = Ticks{at.value - m_frame * m_clock.ticksPerFrame}; }
    /// MasterInsert: processes the next frames of the stream started by restart(): the chain, then the fader on the
    /// parameter grid counted from the restart origin (as the chain's parameters are).
    void process(std::span<float> block, int channels) noexcept override;

private:
    struct Program {
        EffectStack stack;
        ParamValue volume; ///< the master fader
        audio::EffectChain chain;
        /// Live: the player runs the master in the audio callback, so a plugin there never waits (and export, which
        /// runs it the same way, has the same latency).
        Program(int channels, int sampleRate) : chain(channels, sampleRate, audio::ChainContext::Live) {}
    };

    const int m_channels;
    const int m_sampleRate;
    std::atomic<Program*> m_current{nullptr};
    std::unique_ptr<Program> m_owned;                                   ///< control side: what m_current points to
    std::vector<std::pair<std::unique_ptr<Program>, std::uint64_t>> m_retired; ///< with the render sequence at retirement
    std::atomic<std::uint64_t> m_renderSeq{0};

    LiveFader* m_live = nullptr;
    int m_rampFrames;

    // Audio thread.
    Program* m_active = nullptr;
    Program* m_lastProgram = nullptr;
    audio::CurveClock m_clock;
    std::int64_t m_frame = 0;
};

} // namespace wabcut::playback::detail
