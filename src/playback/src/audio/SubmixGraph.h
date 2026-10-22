#pragma once

// Internal (p02 audio): the submix routing of a sequence's mix (SubmixOps.h; Premiere's Audio Track Mixer submixes and
// sends), shared by the player's audio callback and OfflineMixdown so preview and export route alike.
//
// Per block (the caller drives it, inside one Mixer block):
//   * every track slot's pre-fader audio feeds its pre-fader sends (sendPre), its fader is applied by the caller, the
//     post-fader audio feeds its post-fader sends (sendPost) and goes to its output (outputOf): the Mix block, or a
//     submix's input. Sends are scaled by their level and pan on the parameter grid counted from timeline 0, and by the
//     track's mute/solo audibility (ramped over the block).
//   * finishBlock() runs the submixes in processing order (every submix after those that feed it): its effect chain,
//     its fader and panner (a mono submix folds its input to mono first and pans it with the constant-power law), then
//     its mixer bus (mute, solo, meter) into its output.
//
// Latency (plug-in delay compensation): a submix's chain delays its input by its latency. Every input of a bus is
// aligned to the latest path into it: the tracks' contributions to a bus (outputs and sends) are delayed by the bus's
// alignment A, a submix's output by A(destination) - A(submix) - latency(submix). latencyFrames() is A(Mix); the
// caller reads the tracks that much further ahead (with the master's latency) and primes the graph like the master
// chain. Paths are compensated up to kMaxCompensation frames each; longer ones play late (counted).
//
// Programs: the control side builds an immutable program (the routing, the submixes' stacks, faders and chains) and
// publishes it with one atomic pointer; a new chain adopts the processors of unchanged instances (MasterBus's
// scheme). Retired programs are freed once the render counter shows the audio thread is past them. Delay lines and
// buffers belong to the graph and are allocated by its constructor: nothing on the audio thread allocates or locks.
//
// Solo: with routing, a track or submix is soloed when it is soloed itself, feeds a soloed submix (outputs or sends,
// through any submixes) or is fed by a soloed track or submix (effectiveSolo()); the owner sets the mixer's flags.

#include "LiveFader.h"

#include <wabcut/audio/Mixer.h>
#include <wabcut/audio/effects/EffectChain.h>

#include <wabcut/model/Sequence.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace wabcut::playback::detail {

class SubmixGraph {
public:
    /// Submix buses one graph mixes; further submix tracks are silent (droppedSubmixes()).
    static constexpr int kMaxSubmixes = 16;
    /// Most frames one path's latency is compensated by.
    static constexpr int kMaxCompensation = 8192;

    /// `trackSlots` track buses come first in the mixer, the submixes after them (busOf). `channels`: the mix's.
    SubmixGraph(int trackSlots, int channels, int sampleRate, int maxBlockFrames);
    ~SubmixGraph();
    SubmixGraph(const SubmixGraph&) = delete;
    SubmixGraph& operator=(const SubmixGraph&) = delete;

    int trackSlots() const noexcept { return m_slots; }
    /// The mixer bus of submix `index` (submix order in the sequence).
    int busOf(int index) const noexcept { return m_slots + index; }
    /// Mixer buses the graph needs: trackSlots() + kMaxSubmixes.
    int busCount() const noexcept { return m_slots + kMaxSubmixes; }

    // ---- Control side (one non-real-time thread) ------------------------------------------------------------------
    /// Publishes the routing of `sequence` (null: none) for the tracks the slots play (`slotTracks[i]`, TrackId{} =
    /// none), if it differs from the current program's. Allocates; frees retired programs.
    void update(const Sequence* sequence, std::span<const TrackId> slotTracks);
    /// Frees retired programs the audio thread is done with.
    void collect();
    /// The mute and solo flags of every track slot and submix bus with the routing's solo rules (see the file
    /// comment), set on `mixer`. Without routing the tracks' own flags.
    void applyFlags(const Sequence* sequence, std::span<const TrackId> slotTracks, audio::Mixer& mixer) const;
    /// Whether the current program routes anything (else the caller mixes the tracks straight into the Mix).
    bool routed() const noexcept;
    /// The live fader override of submix `index` (automation passes; TimelineAudioPlayer::setLiveTrackMix).
    LiveFader& live(int index) noexcept { return m_live[static_cast<std::size_t>(index)]; }
    /// The submix index playing track `submix`, or -1 (published with the program).
    int indexOf(TrackId submix) const noexcept;
    int droppedSubmixes() const noexcept { return m_dropped.load(std::memory_order_relaxed); }
    std::uint64_t uncompensatedPaths() const noexcept { return m_uncompensated.load(std::memory_order_relaxed); }

    // ---- Audio thread (RT-safe) -------------------------------------------------------------------------------------
    /// Starts a render: picks up the current program. Every beginRender() needs an endRender().
    void beginRender() noexcept;
    void endRender() noexcept;
    /// Whether this render's program routes anything.
    bool active() const noexcept;
    /// A(Mix): how far the routed paths delay the mix (frames).
    int latencyFrames() const noexcept;
    /// Changes whenever the program's per-path delays change (a restart then realigns the delay lines).
    std::uint64_t delayKey() const noexcept;
    /// Restarts at a discontinuity: chains reset, delay lines cleared, the parameter grid counted from `origin`.
    void restart(Ticks origin, std::int64_t ticksPerFrame) noexcept;
    /// The stream goes on at timeline time `at` (a loop wrap): state goes on, parameters from `at`.
    void continueAt(Ticks at) noexcept { m_clock.origin = Ticks{at.value - m_frame * m_clock.ticksPerFrame}; }

    /// Starts a block of `frames` frames (<= the maxBlockFrames given at construction) read at timeline time `at`.
    void beginBlock(Ticks at, std::int64_t frames) noexcept;
    /// Slot `slot`'s pre-fader audio (the mix's channels): its pre-fader sends. `mixer` tells its audibility.
    void sendPre(int slot, const float* pre, const audio::Mixer& mixer) noexcept;
    /// Its post-fader audio: its post-fader sends.
    void sendPost(int slot, const float* post, const audio::Mixer& mixer) noexcept;
    /// Where the slot's post-fader audio goes (through Mixer::addBusTo): a submix's input, the delayed-Mix input, or
    /// an empty span for the Mix itself (Mixer::addBus).
    std::span<float> outputOf(int slot) noexcept;
    /// Runs the submixes and adds them, and the delayed track inputs of the Mix, into `mix` (the Mixer's block,
    /// between its addBus calls and endBlock) through `mixer`.
    void finishBlock(audio::Mixer& mixer, std::span<float> mix) noexcept;

private:
    struct Program;
    struct Delay;

    Program* program() const noexcept { return m_active; }
    std::span<float> inputOf(int destination) noexcept; ///< -1: the Mix's delayed track input

    const int m_slots;
    const int m_channels;
    const int m_sampleRate;
    const int m_maxBlock;
    const int m_rampFrames;

    std::atomic<Program*> m_current{nullptr};
    std::unique_ptr<Program> m_owned;
    std::vector<std::pair<std::unique_ptr<Program>, std::uint64_t>> m_retired;
    std::atomic<std::uint64_t> m_renderSeq{0};
    std::atomic<int> m_dropped{0};
    std::atomic<std::uint64_t> m_uncompensated{0};
    std::unique_ptr<LiveFader[]> m_live;
    std::unique_ptr<std::atomic<std::uint64_t>[]> m_submixTrack; ///< which track each submix index plays

    // Audio thread.
    Program* m_active = nullptr;
    audio::CurveClock m_clock;
    std::int64_t m_frame = 0;
    Ticks m_at;                        ///< timeline time of the block's first frame
    std::int64_t m_frames = 0;         ///< frames of the current block
    std::vector<float> m_inputs;       ///< (kMaxSubmixes + 1) inputs of maxBlock frames: submixes, then the Mix's delayed input
    std::vector<float> m_send;         ///< one send's signal
    std::vector<float> m_work;         ///< a submix being processed (mono or the mix's channels)
    std::vector<float> m_out;          ///< a submix's output at the mix's channels
    std::vector<float> m_bus;          ///< a submix's output after its mixer bus (mute, solo, meter)
    std::unique_ptr<Delay[]> m_trackDelays;  ///< per destination (submixes, then the Mix): the tracks' inputs
    std::unique_ptr<Delay[]> m_submixDelays; ///< per submix: its output to its destination
    std::vector<std::array<float, 5>> m_sendGain; ///< per slot and send: the audibility ramp's current value
};

} // namespace wabcut::playback::detail
