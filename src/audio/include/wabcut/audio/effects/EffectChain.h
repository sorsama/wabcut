#pragma once

// The audio effects of the model's EffectRegistry, evaluated: processors for wabcut.audio.gain / eq3 / compressor /
// limiter / denoise, Essential Sound's filter / dehum / deess / expander / reverb / width (A.7, wabcut/audio/dsp
// Repair.h and Space.h), and the chain that runs an EffectStack through them. wabcut.audio.essentialSound is the
// Essential Sound panel's record of a clip's type and preset: implemented, and it does nothing to the sound.
//
// Parameters (constants or keyframe curves) are evaluated on a fixed grid of kParamCellFrames frames counted from
// the chain's origin (the frame where rendering started), never from the caller's block boundaries: every cell sees
// the values at its first frame and at the next cell's first frame. Gain ramps linearly between them, sample by
// sample (sample-accurate fades and automation); the other effects take a cell's start values (coefficients change
// at most every 0.7 ms). Because the grid does not depend on how the audio is sliced, preview (the player's feeder
// chunks and audio callbacks) and export (its own blocks) produce the same samples.
//
// Audio plugins (A.6, docs/plugins.md): effect ids that start with "plugin." are created by the external effect
// factory the host installs (setExternalEffectFactory; wabcut_plugins runs them in the wabcut-plughost helper). Their
// parameters are the instance's own, any number of them (the ones the instance sets; the rest stay as the plugin's
// state has them), evaluated on the same grid; their latency is compensated like any effect's. A plugin's processor
// that can no longer run as built (its plugin came to report another latency) says so (stale()): the next bind()
// rebuilds it, handing the old one to the factory so the new one takes over its plugin. On the master (ChainContext::
// Live) a processor follows such a change itself instead: latencyFrames() reports the new latency, which it adopts at
// the next reset() (the player restarts the master when its latency changes).
//
// Threading: bind() and the constructors allocate and belong to a non-real-time thread; process() and reset() are
// RT-safe. A chain may share processors with another chain (EffectChain::bind with `adoptFrom`), which is how the
// master bus swaps in a new effect stack without losing effect state; the two chains must then never process
// concurrently. A shared processor is never changed by the chain that adopts it: a plugin's parameter keys are bound
// per chain (bindKeys() returns an immutable binding the chain passes back in every cell).

#include <wabcut/audio/RealTime.h>

#include <wabcut/model/Clip.h>
#include <wabcut/model/Effects.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::audio {

/// Frames per parameter cell (0.67 ms at 48 kHz).
inline constexpr int kParamCellFrames = 32;
/// Most parameters an effect has.
inline constexpr int kMaxEffectParams = 8;

/// Parameter values over one cell, in the descriptor's parameter order (a plugin's: its bound keys' order).
struct ParamCell {
    const double* start = nullptr; ///< at the cell's first frame
    const double* end = nullptr;   ///< at the next cell's first frame
    int offset = 0;                ///< frame of this call within the cell
    int length = kParamCellFrames;
    int count = 0;                 ///< values in `start` and `end`
    const void* binding = nullptr; ///< a plugin's: what its bindKeys() returned for these values' keys
};

/// One effect instance's DSP. Built for a channel count, a sample rate and its constant (non-animatable) parameters.
class EffectProcessor {
public:
    virtual ~EffectProcessor() = default;
    /// Frames the output lags the input: constant for the processor's life, except for a plugin's on the master
    /// (ChainContext::Live), whose new latency takes effect at its next reset().
    virtual int latencyFrames() const noexcept { return 0; }
    /// Input frames that bring the state from a reset to steady behaviour (pre-roll after a seek).
    virtual int settleFrames() const noexcept { return 0; }
    /// Processes `frames` interleaved frames, all inside one parameter cell (cell.offset + frames <= cell.length).
    virtual void process(float* interleaved, int frames, const ParamCell& cell) noexcept = 0;
    virtual void reset() noexcept = 0;
    /// A plugin's processor binds the instance's own parameters, by key, instead of the descriptor's: bind() asks it
    /// for a binding of their keys (again whenever they change) and passes it in every cell (ParamCell::binding), whose
    /// values come in that order. The binding is immutable: chains that share the processor may bind other keys. Not
    /// RT-safe.
    virtual std::shared_ptr<const void> bindKeys(std::span<const std::string> keys) const {
        (void)keys;
        return nullptr;
    }
    /// A plugin's processor whose instance already has the data `instance` now holds (its state was just saved from
    /// this very instance) keeps running instead of being rebuilt. Not RT-safe.
    virtual bool acceptsData(const EffectInstance& instance) { (void)instance; return false; }
    /// The processor can no longer run as built (a plugin that now reports another latency): bind() replaces it.
    virtual bool stale() const noexcept { return false; }
};

/// Where a chain runs, which decides how an external effect (a plugin in another process) may wait for its results.
enum class ChainContext : std::uint8_t {
    /// Rendered ahead of playback (clip and track stacks in the player's feeder, nested mixes, export): the processor
    /// may wait, bounded, for the plugin.
    Ahead,
    /// The master bus, which the player runs in the audio callback: never waits (a late block plays the dry signal).
    /// Export runs the master in this context too, waiting where the callback would not, so the two have the same
    /// latency.
    Live,
};

/// Effects implemented outside the engine: the host's audio plugins (A.6). Installed once by the host.
class ExternalEffectFactory {
public:
    virtual ~ExternalEffectFactory() = default;
    /// Whether this factory runs the effect id (a scanned plugin of a format it hosts).
    virtual bool implements(std::string_view effectId) const = 0;
    /// A processor for `instance` (its parameters and data: a plugin's saved state), or null when it cannot be run
    /// now (the plugin failed to load: the chain bypasses it). `replacing`: the stale processor of the same build that
    /// the new one replaces (its chain drops it, or stops playing it, before the new one first processes).
    virtual std::unique_ptr<EffectProcessor> create(const EffectInstance& instance, const EffectDescriptor& descriptor,
                                                    int channels, int sampleRate, ChainContext context,
                                                    EffectProcessor* replacing) = 0;
    /// Why `instance` will not be heard as the project asks (a plugin missing on this computer, one that failed to
    /// load), or empty. Not RT-safe.
    virtual std::string problem(const EffectInstance& instance) const {
        return implements(instance.effect) ? std::string() : "the effect " + instance.effect + " is not available";
    }
};

/// Installs the external effect factory (null: none). Not RT-safe; bind() reads it.
void setExternalEffectFactory(std::shared_ptr<ExternalEffectFactory> factory);
std::shared_ptr<ExternalEffectFactory> externalEffectFactory();

/// Whether the engine implements this effect id: a built-in effect, or one the external factory runs.
bool isImplementedAudioEffect(std::string_view effectId);

/// Why an enabled plugin effect ("plugin." ids) will not be heard as the project asks (missing on this computer, failed
/// to load, no plugin support in this build), or empty (any other effect too). What an export warns about before it
/// renders. Not RT-safe.
std::string externalEffectProblem(const EffectInstance& instance);

/// What went wrong with external effects during an offline render (a plugin that crashed, hung, could not be loaded or
/// produced invalid samples), as messages for the user. Thread-safe; not RT-safe.
class RenderIncidents {
public:
    /// Adds `message` unless it is there already.
    void add(std::string message);
    std::vector<std::string> messages() const;

private:
    mutable rt::CheckedMutex m_mutex;
    std::vector<std::string> m_messages;
};

/// Marks the calling thread as rendering offline (export) for the scope's life: external effects created on it are
/// loaded before they process, and wait as long as an offline render may for each block, and what goes wrong with
/// them is reported to `incidents` (when given) as their processors go away. Elsewhere (the player's feeder, the
/// thread that starts playback) a plugin is loaded on a worker and bypassed until it is ready. Nests. Not RT-safe.
class ScopedOfflineRender {
public:
    explicit ScopedOfflineRender(std::shared_ptr<RenderIncidents> incidents = nullptr) noexcept;
    ~ScopedOfflineRender();
    ScopedOfflineRender(const ScopedOfflineRender&) = delete;
    ScopedOfflineRender& operator=(const ScopedOfflineRender&) = delete;

private:
    std::shared_ptr<RenderIncidents> m_previous;
};

/// Whether the calling thread is inside a ScopedOfflineRender, and the innermost scope's incidents (or null).
bool inOfflineRender() noexcept;
std::shared_ptr<RenderIncidents> offlineRenderIncidents() noexcept;

/// A processor for an implemented audio effect, or null. `values` are the instance's parameter values (descriptor
/// order) at the time it is built; the non-animatable ones are fixed for the processor's life.
std::unique_ptr<EffectProcessor> createEffectProcessor(const EffectDescriptor& descriptor, int channels, int sampleRate,
                                                       std::span<const double> values);

/// Maps a chain frame (counted from the chain's origin) to the curve time its parameters are read at: timeline time
/// for track and master stacks, Clip::curveTimeAt for a clip's stack.
struct CurveClock {
    Ticks origin;                     ///< timeline time of frame 0
    std::int64_t ticksPerFrame = 5'292'000;
    const Clip* clip = nullptr;       ///< null: curve time = timeline time

    Ticks timelineAt(std::int64_t frame) const noexcept { return origin + Ticks{frame * ticksPerFrame}; }
    Ticks curveTimeAt(std::int64_t frame) const noexcept {
        const Ticks t = timelineAt(frame);
        return clip != nullptr ? clip->curveTimeAt(t) : t;
    }
};

/// The processors of one audio effect stack, in stack order: enabled instances of implemented effects whose version
/// this build knows. Others (disabled, unknown, newer) are bypassed.
class EffectChain {
public:
    EffectChain(int channels, int sampleRate, ChainContext context = ChainContext::Ahead);
    ~EffectChain();
    EffectChain(EffectChain&&) noexcept;
    EffectChain& operator=(EffectChain&&) noexcept;

    int channels() const noexcept { return m_channels; }
    int sampleRate() const noexcept { return m_sampleRate; }
    ChainContext context() const noexcept { return m_context; }

    /// Brings the processors in line with `stack`: keeps the processor of an instance whose id, effect and constant
    /// parameters are unchanged (its state carries on), builds the others, drops the rest, and binds the parameter
    /// values. `stack` must stay alive and unchanged until the next bind(). With `adoptFrom`, processors are taken
    /// from (shared with) that chain instead of this one. Not RT-safe. Returns true when processors were added,
    /// removed or reordered (the latency may have changed).
    bool bind(const EffectStack& stack, const EffectChain* adoptFrom = nullptr);

    bool empty() const noexcept { return m_stages.empty(); }
    /// Sum of the processors' latencies (read again on every call: a plugin's on the master may change, see the file
    /// comment). RT-safe.
    int latencyFrames() const noexcept;
    /// Largest settle time (pre-roll) among the processors.
    int settleFrames() const noexcept { return m_settle; }

    /// Processes `frames` frames that start at chain frame `firstFrame` (RT-safe; the parameter grid is counted
    /// from frame 0, see the file comment).
    void process(float* interleaved, int frames, std::int64_t firstFrame, const CurveClock& clock) noexcept;
    /// Clears all effect state (a discontinuity).
    void reset() noexcept;

    struct Stage; ///< internal (EffectChain.cpp)

private:

    int m_channels;
    int m_sampleRate;
    ChainContext m_context = ChainContext::Ahead;
    std::vector<Stage> m_stages;
    int m_settle = 0;
};

} // namespace wabcut::audio
