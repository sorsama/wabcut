#pragma once

// The wire between the editor and wabcut-plughost, the helper process that runs audio plugins (A.6, docs/plugins.md).
//
// Two channels per helper process:
//   * control: JSON lines over the helper's stdin/stdout (load a plugin, create and destroy instances, state, editor
//     windows, scanning). Not real-time; every request carries an "id" the reply repeats, and the helper may send
//     notifications without one ({"event": "stateChanged", "instance": 3}).
//   * audio: one shared-memory block per instance (InstanceShared below) with a ring of block slots and two named
//     auto-reset events ("work": the host submitted blocks, "done": the helper completed some). The host writes a
//     block's input and parameter events into a slot, publishes it with `submitted` (release) and signals "work";
//     the helper's audio thread processes slots in order, publishes `completed` (release) and signals "done". Neither
//     side ever locks: the host never waits on the audio callback (a block that is not done in time plays the dry
//     signal, and is counted), and only waits, bounded, where rendering runs ahead of playback (the feeder, export).
//
// The block size is fixed (kBlockFrames) and blocks are aligned to the stream the effect chain renders, so a plugin
// sees the same blocks in preview and export and a deterministic plugin renders the same samples in both.
//
// Latency: the helper writes the plugin's latency when it creates the instance, and again whenever the plugin asks for a
// restart with a new one (CLAP request_restart / latency.changed, VST3 kLatencyChanged): it stops the instance's audio
// thread, deactivates and activates the plugin, stores `latency` and then bumps `latencyGeneration` (release). A latency
// above kMaxLatencySeconds is a plugin bug: the instance fails.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace wabcut::plughost {

inline constexpr std::uint32_t kProtocolVersion = 2;
inline constexpr std::uint32_t kSharedMagic = 0x57504831; // "WPH1"

/// Frames per plugin block (5.3 ms at 48 kHz).
inline constexpr int kBlockFrames = 256;
/// Channels an instance processes (stereo effects; mono sources are upmixed by the engine before the stack).
inline constexpr int kMaxChannels = 2;
/// Block slots per instance: more than the deepest pipeline (kLivePipelineFrames / kBlockFrames) plus one.
inline constexpr int kSlots = 24;
/// Parameter changes one block can carry (8 cells of 32 frames each; changes beyond this are dropped and counted).
inline constexpr int kMaxBlockEvents = 256;

/// Pipeline depth where the engine renders ahead of playback and may wait for a block (clip and track stacks in the
/// player's feeder, export): one block. The output lags the input by it (plus the plugin's own latency); the engine
/// compensates both (TrackRenderer reads ahead).
inline constexpr int kAheadPipelineFrames = kBlockFrames;
/// Pipeline depth for stacks that run in the audio callback (the master bus): the helper has this long to return a
/// block, longer than one device block (up to kLivePipelineFrames - kBlockFrames frames per callback). Export renders
/// the master with the same depth, so it matches preview.
inline constexpr int kLivePipelineFrames = 16 * kBlockFrames;

/// The longest latency a plugin may report. More is a plugin bug (or another unit): the instance fails.
inline constexpr int kMaxLatencySeconds = 1;
constexpr std::int64_t maxLatencyFrames(double sampleRate) noexcept {
    return static_cast<std::int64_t>(sampleRate) * kMaxLatencySeconds;
}

/// Slot flags.
inline constexpr std::uint32_t kSlotReset = 1u << 0; ///< reset the plugin's state before this block (a seek, a restart)

struct ParamEvent {
    std::uint32_t param = 0;  ///< the plugin's parameter id
    std::uint32_t offset = 0; ///< frame within the block
    double value = 0.0;       ///< CLAP: plain value; VST3: normalised 0..1
};

struct BlockSlot {
    std::uint64_t block = 0;      ///< the block number written here (host)
    std::uint32_t frames = 0;     ///< kBlockFrames, or fewer at the end of a stream
    std::uint32_t flags = 0;      ///< kSlot* flags
    std::uint32_t eventCount = 0; ///< valid entries of `events`, in offset order
    std::uint32_t reserved = 0;
    ParamEvent events[kMaxBlockEvents];
    float input[kMaxChannels][kBlockFrames];  ///< planar
    float output[kMaxChannels][kBlockFrames]; ///< planar, written by the helper
};

/// Instance status, written by the helper.
enum class InstanceStatus : std::uint32_t {
    Starting = 0,
    Running = 1,
    Failed = 2,  ///< the plugin refused to process (an error result); the host bypasses it
    Stopped = 3, ///< destroyed
};

struct InstanceShared {
    std::uint32_t magic = kSharedMagic;
    std::uint32_t version = kProtocolVersion;
    std::uint32_t channels = 2;
    std::uint32_t sampleRate = 48000;
    std::atomic<std::uint64_t> submitted{0}; ///< blocks the host submitted (block numbers 0 .. submitted - 1)
    std::atomic<std::uint64_t> completed{0}; ///< blocks the helper completed
    std::atomic<std::uint32_t> status{static_cast<std::uint32_t>(InstanceStatus::Starting)};
    std::atomic<std::uint32_t> latency{0};   ///< the plugin's latency in frames (from its last activation)
    std::atomic<std::uint32_t> latencyGeneration{0}; ///< bumped after `latency` changed (a restart the plugin asked for)
    std::atomic<std::uint64_t> stateChanges{0}; ///< bumped when the plugin says its state changed (its editor)
    std::atomic<std::uint64_t> droppedEvents{0}; ///< parameter changes beyond kMaxBlockEvents (host)
    BlockSlot ring[kSlots];
};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "shared-memory atomics must be lock-free");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "shared-memory atomics must be lock-free");

/// Names of the kernel objects of instance `instance` of host process `hostPid` (Local\ namespace on Windows).
struct InstanceNames {
    char memory[96];
    char work[96];
    char done[96];
};
InstanceNames instanceNames(std::uint32_t hostPid, std::uint64_t instance);

} // namespace wabcut::plughost
